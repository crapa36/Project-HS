#include <hs/runtime/application.hpp>

#include "runtime_channels.hpp"
#include "window.hpp"

#include <hs/core/fixed_step_clock.hpp>
#include <hs/gameplay/game_simulation.hpp>
#include <hs/jobs/task_system.hpp>
#include <hs/renderer/renderer.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace hs
{
namespace
{

void SetThreadName(const wchar_t *name) noexcept
{
    SetThreadDescription(GetCurrentThread(), name);
}

void WriteText(const std::filesystem::path &path, std::string_view text)
{
    std::ofstream stream(path, std::ios::trunc);
    stream << text;
}

} // namespace

ApplicationResult RunApplication(const ApplicationConfig &config)
{
    RuntimeChannels channels;
    TaskSystem workers;
    Window window(channels);
    auto result =
        window.Create(config.width, config.height, config.visible, config.borderless);
    if (!result)
    {
        return {result};
    }
    if (config.smoke)
    {
        window.StopInput();
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(config.artifact_directory, filesystem_error);

    std::mutex result_mutex;
    Result thread_result = Result::Success();
    auto set_failure = [&](Result failure) {
        std::lock_guard lock(result_mutex);
        if (thread_result)
        {
            thread_result = std::move(failure);
        }
        channels.stop_requested.store(true, std::memory_order_release);
    };

    std::jthread render_thread([&](std::stop_token) {
        SetThreadName(L"HS Render");
        D3D12Renderer renderer;
        RendererConfig renderer_config;
        renderer_config.window = window.Handle();
        renderer_config.width = config.width;
        renderer_config.height = config.height;
        renderer_config.warp = config.warp;
        renderer_config.validation = config.validation;
        renderer_config.gpu_validation = config.gpu_validation;
        renderer_config.vsync = config.vsync;
        renderer_config.bloom = config.bloom;
        renderer_config.outline = config.outline;
        renderer_config.interpolate = !config.smoke;
        renderer_config.render_scale_percent = config.render_scale_percent;
        renderer_config.shadow_resolution = config.shadow_resolution;
        renderer_config.particle_percentage = config.particle_percentage;
        renderer_config.barrier_mode = config.barrier_mode;
        renderer_config.artifact_directory = config.artifact_directory;

        const auto initialized = renderer.Initialize(renderer_config);
        if (!initialized)
        {
            set_failure(initialized);
            channels.render_ready.store(true, std::memory_order_release);
            channels.render_ready.notify_all();
            return;
        }
        channels.render_ready.store(true, std::memory_order_release);
        channels.render_ready.notify_all();

        RenderSnapshotExchange::Consumer snapshot_consumer(channels.snapshots);
        std::array<PresentationEvent, 256> event_storage{};
        std::vector<ParticleSpawnCommand> pending_particle_spawns;
        std::vector<std::string> event_lines;
        std::vector<std::string> timeline_lines;
        std::vector<std::string> gpu_lines;
        event_lines.reserve(32);
        pending_particle_spawns.reserve(256);
        timeline_lines.reserve(static_cast<std::size_t>(config.maximum_ticks) + 16);

        Tick last_tick{};
        bool captured{};
        bool resized{};
        auto next_frame = std::chrono::steady_clock::now();
        for (;;)
        {
            GraphicsCommand graphics_command;
            std::uint32_t resize_width{};
            std::uint32_t resize_height{};
            while (channels.graphics_commands.TryPop(graphics_command))
            {
                if (graphics_command.kind == GraphicsCommandKind::Resize)
                {
                    resize_width = graphics_command.width;
                    resize_height = graphics_command.height;
                }
            }
            if (resize_width && resize_height)
            {
                if (auto resize_result = renderer.Resize(resize_width, resize_height);
                    !resize_result)
                {
                    set_failure(resize_result);
                    break;
                }
            }

            std::size_t event_count{};
            PresentationEvent event;
            while (event_count < event_storage.size() &&
                   channels.presentation_events.TryPop(event))
            {
                event_storage[event_count++] = event;
                event_lines.push_back(
                    std::format("{{\"sequence\":{},\"tick\":{},\"kind\":{}}}\n",
                                event.sequence, event.tick, static_cast<unsigned>(event.kind)));
            }
            const auto snapshots = snapshot_consumer.AcquireLatest();
            if (snapshots.has_current)
            {
                ParticleSpawnCommand particle_spawn;
                while (channels.particle_spawns.TryPop(particle_spawn))
                {
                    pending_particle_spawns.push_back(particle_spawn);
                }
                std::size_t particle_spawn_count{};
                while (particle_spawn_count < pending_particle_spawns.size() &&
                       pending_particle_spawns[particle_spawn_count].tick <=
                           snapshots.current.header.tick)
                {
                    ++particle_spawn_count;
                }
                if (config.resize_test && !resized &&
                    snapshots.current.header.tick >= config.maximum_ticks / 2)
                {
                    if (auto resize_result =
                            renderer.Resize(config.width + 64, config.height + 36);
                        !resize_result)
                    {
                        set_failure(resize_result);
                        break;
                    }
                    resized = true;
                }
                RendererFrameResult frame;
                const auto frame_start = std::chrono::steady_clock::now();
                if (auto rendered =
                        renderer.Render(
                            snapshots, std::span(event_storage.data(), event_count),
                            std::span(pending_particle_spawns.data(), particle_spawn_count),
                            frame);
                    !rendered)
                {
                    set_failure(rendered);
                    break;
                }
                pending_particle_spawns.erase(
                    pending_particle_spawns.begin(),
                    pending_particle_spawns.begin() + particle_spawn_count);
                const auto frame_microseconds =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - frame_start)
                        .count();
                channels.rendered_frames.store(frame.frame, std::memory_order_release);
                last_tick = frame.rendered_tick;
                timeline_lines.push_back(
                    std::format("{},{},{}\n", frame.frame, last_tick, frame_microseconds));
                if (!config.vsync && config.frame_cap != 0 && !config.smoke)
                {
                    next_frame += std::chrono::nanoseconds(
                        1'000'000'000ull / std::clamp(config.frame_cap, 30u, 120u));
                    std::this_thread::sleep_until(next_frame);
                    if (std::chrono::steady_clock::now() > next_frame +
                                                                   std::chrono::milliseconds(100))
                    {
                        next_frame = std::chrono::steady_clock::now();
                    }
                }

                if (config.smoke && last_tick >= config.maximum_ticks && !captured)
                {
                    if (auto capture = renderer.CapturePng(
                            config.artifact_directory /
                            std::format("capture_tick_{}.png", config.maximum_ticks));
                        !capture)
                    {
                        set_failure(capture);
                        break;
                    }
                    channels.rendered_particles.store(renderer.LastParticleCount(),
                                                      std::memory_order_release);
                    constexpr std::string_view pass_names[] = {
                        "GPU Particle Spawn/Update", "3-cascade Directional Shadow",
                        "GBuffer+Depth", "Deferred Cel Lighting", "Forward Transparent/OIT",
                        "OIT Composite", "Bloom", "ToneMap", "Screen-space Outline", "FXAA",
                        "Game UI"};
                    const auto gpu_timings = renderer.LastGpuPassTimings();
                    if (gpu_timings.valid)
                    {
                        for (std::size_t pass = 0; pass < gpu_timings.nanoseconds.size(); ++pass)
                        {
                            gpu_lines.push_back(std::format(
                                "{},{},{},{}\n", frame.frame, last_tick, pass_names[pass],
                                gpu_timings.nanoseconds[pass]));
                        }
                    }
                    captured = true;
                }
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            if (config.smoke && captured)
            {
                channels.stop_requested.store(true, std::memory_order_release);
            }
            if (channels.stop_requested.load(std::memory_order_acquire) &&
                channels.simulation_done.load(std::memory_order_acquire) &&
                channels.presentation_events.Size() == 0)
            {
                break;
            }
        }

        const auto enhanced = renderer.UsesEnhancedBarriers();
        const auto validation_errors = renderer.ValidationErrorCount();
        if (auto shutdown = renderer.Shutdown(); !shutdown)
        {
            set_failure(shutdown);
        }
        if (validation_errors != 0)
        {
            set_failure(Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                        std::format("{} validation errors.", validation_errors)));
        }

        std::string timeline = "frame,tick,cpu_us\n";
        for (const auto &line : timeline_lines)
        {
            timeline += line;
        }
        WriteText(config.artifact_directory / "timeline.csv", timeline);
        std::string gpu_timeline = "frame,tick,pass,gpu_ns\n";
        for (const auto &line : gpu_lines)
        {
            gpu_timeline += line;
        }
        WriteText(config.artifact_directory / "gpu_passes.csv", gpu_timeline);

        std::string events;
        for (const auto &line : event_lines)
        {
            events += line;
        }
        WriteText(config.artifact_directory / "events.ndjson", events);
        WriteText(config.artifact_directory / "render.json",
                  std::format("{{\"enhanced_barriers\":{},\"validation_errors\":{},"
                              "\"last_tick\":{}}}\n",
                              enhanced ? "true" : "false", validation_errors, last_tick));
    });

    channels.render_ready.wait(false, std::memory_order_acquire);
    {
        std::lock_guard lock(result_mutex);
        if (!thread_result)
        {
            render_thread.join();
            return {thread_result};
        }
    }

    std::jthread simulation_thread([&](std::stop_token) {
        SetThreadName(L"HS Simulation");
        GameSimulation simulation;
        if (auto initialized = simulation.Initialize({1}); !initialized)
        {
            set_failure(initialized);
            channels.simulation_done.store(true, std::memory_order_release);
            return;
        }

        FixedStepClock clock;
        auto now = FixedStepClock::Clock::now();
        (void)clock.Advance(now);
        std::uint64_t observed_focus_epoch{};
        Sequence publish_sequence{};
        std::array<ActionEdge, 256> action_storage{};

        while (!channels.stop_requested.load(std::memory_order_acquire))
        {
            std::uint32_t tick_count = 1;
            if (!config.smoke)
            {
                now = FixedStepClock::Clock::now();
                tick_count = clock.Advance(now).tick_count;
                if (tick_count == 0)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
            }

            for (std::uint32_t local_tick = 0; local_tick < tick_count; ++local_tick)
            {
                const auto focus_epoch = channels.focus_epoch.load(std::memory_order_acquire);
                if (focus_epoch != observed_focus_epoch)
                {
                    channels.action_edges.ClearConsumer();
                    observed_focus_epoch = focus_epoch;
                }

                std::size_t action_count{};
                ActionEdge action;
                while (action_count < action_storage.size() &&
                       channels.action_edges.TryPop(action))
                {
                    action_storage[action_count++] = action;
                }

                InputFrame input;
                input.target_tick = channels.completed_tick.load(std::memory_order_relaxed) + 1;
                input.held = channels.held_input.load(std::memory_order_acquire);
                input.ordered_edges = std::span(action_storage.data(), action_count);
                const auto tick = simulation.TickFixed(input, FixedStepClock::kFixedStep);

                for (const auto &presentation : simulation.PendingPresentationEvents())
                {
                    if (!channels.presentation_events.TryPush(presentation))
                    {
                        channels.dropped_presentation_events.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                }
                simulation.ClearPresentationEvents();
                for (const auto &particle_spawn : simulation.PendingParticleSpawns())
                {
                    if (!channels.particle_spawns.TryPush(particle_spawn))
                    {
                        channels.dropped_particle_spawns.fetch_add(1,
                                                                   std::memory_order_relaxed);
                    }
                }
                simulation.ClearParticleSpawns();
                if (auto slot = channels.snapshots.TryBeginWrite())
                {
                    if (simulation.WriteRenderSnapshot(*slot->storage))
                    {
                        channels.snapshots.Publish(*slot, ++publish_sequence);
                    }
                    else
                    {
                        channels.snapshots.Abandon(*slot);
                        set_failure(Result::Failure(ErrorCode::InvalidState, "hs_gameplay",
                                                    "Render snapshot capacity exceeded."));
                        break;
                    }
                }
                channels.completed_tick.store(tick.tick, std::memory_order_release);
                channels.checksum.store(tick.checksum, std::memory_order_release);

                if (config.smoke && tick.tick >= config.maximum_ticks)
                {
                    break;
                }
            }

            if (config.smoke &&
                channels.completed_tick.load(std::memory_order_acquire) >= config.maximum_ticks)
            {
                break;
            }
        }
        if (auto shutdown = simulation.Shutdown(); !shutdown)
        {
            set_failure(shutdown);
        }
        channels.simulation_done.store(true, std::memory_order_release);
    });

    while (!channels.stop_requested.load(std::memory_order_acquire))
    {
        if (!window.PumpMessages())
        {
            channels.stop_requested.store(true, std::memory_order_release);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    window.StopInput();
    channels.stop_requested.store(true, std::memory_order_release);
    simulation_thread.join();
    render_thread.join();

    {
        std::lock_guard lock(result_mutex);
        result = thread_result;
    }
    const auto final_tick = channels.completed_tick.load(std::memory_order_acquire);
    const auto checksum = channels.checksum.load(std::memory_order_acquire);
    const auto rendered_frames = channels.rendered_frames.load(std::memory_order_acquire);
    const auto rendered_particles =
        channels.rendered_particles.load(std::memory_order_acquire);
    const auto expected_particles =
        10'000u * std::clamp(config.particle_percentage, 50u, 100u) / 100u;
    const auto valid =
        result && final_tick >= (config.smoke ? config.maximum_ticks : 0) && rendered_frames > 0 &&
        (!config.smoke || rendered_particles == expected_particles) &&
        channels.dropped_input_edges.load() == 0 &&
        channels.dropped_presentation_events.load() == 0 &&
        channels.dropped_particle_spawns.load() == 0;

    WriteText(config.artifact_directory / "spec.json",
              std::format("{{\"scenario_id\":\"stage1\",\"seed\":1,\"maximum_ticks\":{},"
                          "\"mode\":\"offscreen_render\"}}\n",
                          config.maximum_ticks));
    WriteText(config.artifact_directory / "result.json",
              std::format("{{\"schema_version\":1,\"scenario_id\":\"stage1\","
                          "\"mode\":\"offscreen_render\",\"execution_valid\":{},"
                          "\"assertions_passed\":{},"
                          "\"tick\":{},\"checksum\":{},\"rendered_frames\":{},"
                          "\"rendered_particles\":{},"
                          "\"worker_count\":{},"
                          "\"dropped_input_edges\":{},\"dropped_presentation_events\":{},"
                          "\"dropped_particle_spawns\":{},"
                          "\"user_review\":\"awaiting\"}}\n",
                          valid ? "true" : "false", valid ? "true" : "false", final_tick,
                          checksum, rendered_frames, rendered_particles, workers.WorkerCount(),
                          channels.dropped_input_edges.load(),
                          channels.dropped_presentation_events.load(),
                          channels.dropped_particle_spawns.load()));

    if (!valid && result)
    {
        result = Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                 "Stage 1 execution assertions failed.");
    }
    return {result, final_tick, checksum, rendered_frames};
}

} // namespace hs
