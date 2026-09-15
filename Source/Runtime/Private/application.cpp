#include <hs/runtime/application.hpp>
#include <hs/presentation/projector.hpp>

#include "runtime_channels.hpp"
#include "vfx_catalog.hpp"
#include "window.hpp"
#include "camera_pose.hpp"

#include <hs/core/fixed_step_clock.hpp>
#include <hs/core/process_info.hpp>
#include <hs/gameplay/game_simulation.hpp>
#include <hs/jobs/task_system.hpp>
#include <hs/renderer/renderer.hpp>
#include <hs/runtime/audio_engine.hpp>
#include <hs/runtime/playtest_recording.hpp>
#include <hs/runtime/save_store.hpp>

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
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

PresentationEvent MakeAudioEvent(std::string_view cue, Sequence sequence) noexcept
{
    PresentationEvent event;
    event.sequence = sequence;
    event.kind = PresentationKind::Audio;
    event.asset = MakeAssetId(cue);
    event.parameters = EncodeAudioAction(AudioEventAction::Play);
    return event;
}

} // namespace

ApplicationResult RunApplication(const ApplicationConfig &config)
{
    const auto executable_directory = CurrentExecutableDirectory();
    if (executable_directory.empty())
    {
        return {Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                "Cannot resolve the executable directory.")};
    }
    SimulationRules simulation_rules;
    PresentationCatalog presentation_catalog;
    std::uint64_t content_hash{};
    const auto cooked_simulation_rules_path =
        executable_directory / "Cooked" / "simulation_rules.hsbin";
    const auto cooked_presentation_path =
        executable_directory / "Cooked" / "presentation_catalog.hsbin";
    const auto cooked_particle_effects_path =
        executable_directory / "Cooked" / "particle_effects.hsbin";
    if (auto loaded = LoadSimulationRules(cooked_simulation_rules_path,
                                          simulation_rules, &content_hash);
        !loaded)
        return {loaded};
    if (auto loaded = LoadPresentationCatalog(cooked_presentation_path,
                                              presentation_catalog, &content_hash);
        !loaded)
    {
        return {loaded};
    }
    auto simulation_rules_hash = SimulationRulesHash(simulation_rules);
    VfxCatalog vfx_catalog;
    if (auto loaded = VfxCatalog::Load(
            cooked_particle_effects_path, vfx_catalog);
        !loaded)
        return {loaded};
    PlaytestReplay replay;
    const auto replaying = !config.replay_directory.empty();
    if (replaying)
    {
        if (auto loaded = LoadPlaytestReplay(config.replay_directory, replay); !loaded)
            return {loaded};
        if (!config.replay_compare &&
            replay.header.simulation_rules_hash != simulation_rules_hash)
            return {Result::Failure(ErrorCode::InvalidState, "hs_playtest",
                                    "Replay content hash differs from the current content.")};
    }
    const auto effective_seed = replaying ? replay.header.seed : config.seed;
    PlaytestRecorder playtest_recorder;
    if (config.record_playtest)
    {
        if (auto started = playtest_recorder.Start(
                {config.playtest_output_directory, effective_seed,
                 simulation_rules_hash, content_hash}); !started)
            return {started};
    }
    SaveStore save_store(config.smoke
                             ? std::optional<std::filesystem::path>(
                                   config.artifact_directory / "Profile")
                             : std::nullopt);
    SettingsData settings;
    if (auto loaded = save_store.LoadSettings(settings); !loaded)
    {
        return {loaded};
    }
    if (config.mute_audio)
        settings.master_volume = 0.0f;
    ProfileData profile;
    if (auto loaded = save_store.LoadProfile(profile); !loaded)
    {
        return {loaded};
    }
    AudioEngine audio;
    if (auto initialized = audio.Initialize(settings, executable_directory / "Cooked" / "Audio"); !initialized)
    {
        return {initialized};
    }

    RuntimeChannels channels;
    channels.camera_zoom_percent.store(std::clamp(config.camera_zoom_percent, 15u, 120u));
    channels.best_level.store(profile.best_level, std::memory_order_relaxed);
    TaskSystem task_system;
    Window window(channels, settings.skill_virtual_keys, presentation_catalog.camera);
    Sequence ui_audio_sequence{};
    AssetId active_ambience{};
    AssetId active_bgm = MakeAssetId("audio.bgm.main_menu");
    // MainMenu is the initial phase, so it never crosses a phase transition
    // that would otherwise trigger the normal BGM selection below.
    audio.Play(MakeAudioEvent("audio.bgm.main_menu", ++ui_audio_sequence));
    window.SetUiClickHandler([&](Float2 cursor) {
        const auto session = channels.ReadSessionProbe();
        PresentationUiState ui{
            static_cast<UiPage>(channels.ui_page.load(std::memory_order_acquire)),
            channels.collection_skill.load(std::memory_order_acquire),
            channels.character_skill.load(std::memory_order_acquire),
            channels.loadout_source.load(std::memory_order_acquire)};
        const auto interaction = ResolveUiInteraction(session, ui, settings, cursor);
        channels.ui_page.store(static_cast<std::uint8_t>(ui.page),
                               std::memory_order_release);
        channels.collection_skill.store(ui.selected_collection_skill, std::memory_order_release);
        channels.character_skill.store(ui.selected_character_skill, std::memory_order_release);
        channels.loadout_source.store(ui.loadout_source_slot, std::memory_order_release);
        if (interaction.gameplay_action) (void)channels.ui_actions.TryPush(*interaction.gameplay_action);
        if (interaction.runtime_command) (void)channels.ui_commands.TryPush(*interaction.runtime_command);
        std::string_view cue;
        if (interaction.invalid)
            cue = "audio.ui.invalid";
        if (interaction.gameplay_action)
        {
            switch (interaction.gameplay_action->kind)
            {
            case UiActionKind::StartSession: cue = "audio.ui.confirm"; break;
            case UiActionKind::Quit:
            case UiActionKind::ReturnToMainMenu: cue = "audio.ui.cancel"; break;
            case UiActionKind::Reroll: cue = "audio.ui.reroll"; break;
            case UiActionKind::SelectCard: cue = "audio.ui.card_select"; break;
            case UiActionKind::AssignStat: cue = "audio.ui.stat_allocate"; break;
            case UiActionKind::Resume: cue = "audio.ui.pause_close"; break;
            case UiActionKind::SwapLoadoutSlots: cue = "audio.ui.tab"; break;
            }
        }
        if (interaction.runtime_command)
        {
            switch (interaction.runtime_command->kind)
            {
            case UiCommandKind::BeginSkillRebind: cue = "audio.ui.rebind_start"; break;
            case UiCommandKind::CancelSkillRebind: cue = "audio.ui.cancel"; break;
            case UiCommandKind::SetMasterVolumePercent:
            case UiCommandKind::SetBgmVolumePercent:
            case UiCommandKind::SetSfxVolumePercent:
            case UiCommandKind::SetUiVolumePercent: cue = "audio.ui.slider_tick"; break;
            default: if (cue.empty()) cue = "audio.ui.confirm"; break;
            }
        }
        if (!cue.empty())
            audio.Play(MakeAudioEvent(cue, ++ui_audio_sequence));
    });
    std::uint32_t hover_target = 0;
    std::uint32_t previous_hover_target = 0;
    window.SetUiHoverHandler([&](Float2 cursor) {
        const auto phase = static_cast<SessionPhase>(
            channels.session_phase.load(std::memory_order_acquire));
        const auto page = static_cast<UiPage>(
            channels.ui_page.load(std::memory_order_acquire));
        const auto x = (cursor.x + 1.0f) * 960.0f;
        const auto y = (1.0f - cursor.y) * 540.0f;
        const auto inside = [&](float left, float top, float width, float height) {
            return x >= left && x <= left + width && y >= top && y <= top + height;
        };
        if (phase == SessionPhase::MainMenu && page == UiPage::Root)
        {
            for (std::uint32_t i = 0; i < 4; ++i)
                if (inside(760.0f, 270.0f + i * 150.0f, 400.0f, 92.0f)) { hover_target = i + 1; goto hover_done; }
        }
        if ((phase == SessionPhase::CardSelection || phase == SessionPhase::RelicSelection) &&
            inside(760.0f, 790.0f, 400.0f, 72.0f)) { hover_target = 20; goto hover_done; }
        if (phase == SessionPhase::Paused && page == UiPage::Root)
        {
            for (std::uint32_t i = 0; i < 3; ++i)
                if (inside(760.0f, 420.0f + i * 100.0f, 400.0f, 72.0f)) { hover_target = 30 + i; goto hover_done; }
        }
        hover_target = 0;
    hover_done:
        if (hover_target != previous_hover_target)
        {
            previous_hover_target = hover_target;
            if (hover_target != 0)
                audio.Play(MakeAudioEvent("audio.ui.hover", ++ui_audio_sequence));
        }
    });
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
    auto record_thread_failure = [&](Result failure) {
        std::lock_guard lock(result_mutex);
        if (thread_result)
        {
            thread_result = std::move(failure);
        }
        channels.stop_requested.store(true, std::memory_order_release);
    };

    auto render_ports = channels.ForRender();
    std::jthread render_thread([&, render_ports](std::stop_token) mutable {
        auto &channels = render_ports;
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
        renderer_config.character_preview = config.character_preview;
        renderer_config.monster_preview_asset = config.monster_preview_asset;
        renderer_config.slime_family_preview_count = config.slime_family_preview_count;
        renderer_config.environment_preview = config.environment_preview;
        renderer_config.monster_preview_clip = config.monster_preview_clip;
        renderer_config.monster_preview_time = config.monster_preview_time;
        renderer_config.preview_camera_override = config.preview_camera_override;
        renderer_config.preview_camera_yaw = config.preview_camera_yaw;
        renderer_config.preview_camera_pitch = config.preview_camera_pitch;
        renderer_config.preview_camera_distance = config.preview_camera_distance;
        renderer_config.devtools_visible = config.skill_vfx_capture >= kCombatSkillCount;
        renderer_config.render_scale_percent = config.render_scale_percent;
        renderer_config.shadow_resolution = config.shadow_resolution;
        renderer_config.particle_percentage = config.particle_percentage;
        const auto sprite_binding = [&](std::string_view id) {
            ParticleSpriteBinding binding;
            if (const auto *sprite = vfx_catalog.FindSprite(MakeAssetId(id)))
            {
                binding.sprite = sprite->index;
                binding.frame_columns = sprite->frame_columns;
                binding.frame_rows = sprite->frame_rows;
            }
            return binding;
        };
        renderer_config.bleed_status_sprite = sprite_binding("particle_sprite.blood");
        renderer_config.burn_status_sprite = sprite_binding("particle_sprite.fire");
        renderer_config.slow_status_sprite = sprite_binding("particle_sprite.slow_rune");
        renderer_config.mark_status_sprite = sprite_binding("particle_sprite.mark_target");
        renderer_config.trap_sprite = sprite_binding("particle_sprite.trap_rune");
        renderer_config.fire_area_sprite = sprite_binding("particle_sprite.fire_ground");
        renderer_config.barrier_mode = config.barrier_mode;
        renderer_config.artifact_directory = config.artifact_directory;

        const auto initialized = renderer.Initialize(renderer_config);
        if (!initialized)
        {
            record_thread_failure(initialized);
            channels.render_ready.store(true, std::memory_order_release);
            channels.render_ready.notify_all();
            return;
        }
        channels.render_ready.store(true, std::memory_order_release);
        channels.render_ready.notify_all();

        auto active_vsync = config.vsync;
        auto active_frame_cap = config.frame_cap;
        auto active_particle_percentage = config.particle_percentage;
#if defined(HS_DEVELOPMENT_TOOLS)
        std::error_code vfx_watch_error;
        auto vfx_catalog_write = std::filesystem::last_write_time(
            cooked_particle_effects_path, vfx_watch_error);
        auto next_vfx_catalog_check = std::chrono::steady_clock::now();
#endif

        RenderSnapshotExchange::Consumer snapshot_consumer(channels.snapshots);
        std::array<PresentationEvent, 256> event_storage{};
        std::vector<ParticleSpawnCommand> pending_particle_spawns;
        std::vector<VfxLineSpawnCommand> pending_effect_lines;
        std::vector<std::string> event_lines;
        std::vector<std::string> timeline_lines;
        std::vector<std::string> gpu_lines;
        float previous_foot_phase = -1.0f;
        Tick previous_foot_tick{};
        event_lines.reserve(32);
        pending_particle_spawns.reserve(256);
        pending_effect_lines.reserve(64);
        timeline_lines.reserve(static_cast<std::size_t>(config.maximum_ticks) + 16);

        Tick last_tick{};
        bool captured{};
        bool resized{};
        bool vfx_showcase_injected{};
        auto next_frame = std::chrono::steady_clock::now();
        for (;;)
        {
#if defined(HS_DEVELOPMENT_TOOLS)
            if (std::chrono::steady_clock::now() >= next_vfx_catalog_check)
            {
                next_vfx_catalog_check = std::chrono::steady_clock::now() +
                                         std::chrono::milliseconds(250);
                const auto write = std::filesystem::last_write_time(
                    cooked_particle_effects_path, vfx_watch_error);
                if (!vfx_watch_error && write != vfx_catalog_write)
                {
                    VfxCatalog replacement;
                    if (auto loaded = VfxCatalog::Load(cooked_particle_effects_path,
                                                       replacement); loaded)
                    {
                        vfx_catalog = std::move(replacement);
                        vfx_catalog_write = write;
                    }
                }
            }
#endif
            SettingsData renderer_settings;
            bool apply_renderer_settings{};
            while (channels.renderer_settings.TryPop(renderer_settings))
                apply_renderer_settings = true;
            if (apply_renderer_settings)
            {
                const RendererOptions options{
                    renderer_settings.vsync, renderer_settings.bloom,
                    renderer_settings.outline, renderer_settings.render_scale_percent,
                    renderer_settings.shadow_resolution,
                    renderer_settings.particle_percentage};
                if (auto applied = renderer.ApplyOptions(options); !applied)
                {
                    record_thread_failure(applied);
                    break;
                }
                active_vsync = renderer_settings.vsync;
                active_frame_cap = renderer_settings.frame_cap;
                active_particle_percentage = renderer_settings.particle_percentage;
            }
            NativeWindowMessage window_message;
            while (channels.window_messages.TryPop(window_message))
            {
                renderer.HandleWindowMessage(window_message);
            }
            ResizeCommand resize_command;
            std::uint32_t resize_width{};
            std::uint32_t resize_height{};
            while (channels.resize_commands.TryPop(resize_command))
            {
                resize_width = resize_command.width;
                resize_height = resize_command.height;
            }
            if (resize_width && resize_height)
            {
                if (auto resize_result = renderer.Resize(resize_width, resize_height);
                    !resize_result)
                {
                    record_thread_failure(resize_result);
                    break;
                }
            }

            std::size_t event_count{};
            PresentationEvent event;
            while (event_count < event_storage.size() &&
                   channels.presentation_events.TryPop(event))
            {
                event_storage[event_count++] = event;
                if (event.kind == PresentationKind::Vfx)
                {
                    if (auto expanded = vfx_catalog.ExpandEvent(
                            event, active_particle_percentage,
                            pending_particle_spawns, pending_effect_lines); !expanded)
                    {
#if defined(HS_DEVELOPMENT_TOOLS)
                        record_thread_failure(expanded);
                        break;
#endif
                    }
                }
                if (event.kind == PresentationKind::Audio &&
                    !channels.audio_events.TryPush(event))
                {
                    channels.dropped_presentation_events.fetch_add(1,
                                                                   std::memory_order_relaxed);
                }
                event_lines.push_back(
                    std::format("{{\"sequence\":{},\"tick\":{},\"kind\":{},"
                                "\"asset\":{},\"position\":[{},{},{}]}}\n",
                                event.sequence, event.tick, static_cast<unsigned>(event.kind),
                                event.asset.value, event.position.x, event.position.y,
                                event.position.z));
            }
            const auto snapshots = snapshot_consumer.AcquireLatest();
            if (snapshots.has_current)
            {
                const auto &current_snapshot = snapshots.current;
                if (!current_snapshot.poses.empty() && !current_snapshot.instances.empty() &&
                    current_snapshot.header.tick != previous_foot_tick)
                {
                    const auto &pose = current_snapshot.poses.front();
                    const auto phase = pose.secondary_normalized_time -
                                       std::floor(pose.secondary_normalized_time);
                    const auto crossed = [&](float marker) {
                        return previous_foot_phase >= 0.0f &&
                               (previous_foot_phase < marker && phase >= marker ||
                                previous_foot_phase > phase &&
                                    (previous_foot_phase < marker || phase >= marker));
                    };
                    if (pose.secondary_weight > 0.01f &&
                        (crossed(0.0f) || crossed(0.5f)))
                    {
                        const auto cue = pose.secondary_weight >= 0.5f
                                             ? "audio.player.footstep_run"
                                             : "audio.player.footstep_walk";
                        auto event = MakeAudioEvent(cue, current_snapshot.header.tick);
                        event.tick = current_snapshot.header.tick;
                        event.position = current_snapshot.instances.front().position;
                        (void)channels.audio_events.TryPush(event);
                    }
                    previous_foot_phase = phase;
                    previous_foot_tick = current_snapshot.header.tick;
                }
                if (config.vfx_showcase && !vfx_showcase_injected)
                {
                    constexpr std::array colors{
                        Float4{0.35f, 0.75f, 2.5f, 0.9f},
                        Float4{1.8f, 0.25f, 2.2f, 0.9f},
                        Float4{3.0f, 2.4f, 0.5f, 1.0f},
                        Float4{0.4f, 2.2f, 3.0f, 0.9f},
                        Float4{0.65f, 0.72f, 0.8f, 0.8f},
                        Float4{3.2f, 0.55f, 0.08f, 0.95f},
                        Float4{1.8f, 0.02f, 0.03f, 0.95f},
                        Float4{0.4f, 1.6f, 2.8f, 0.95f},
                        Float4{2.8f, 2.8f, 3.0f, 1.0f},
                        Float4{0.3f, 1.2f, 3.0f, 0.9f},
                        Float4{0.7f, 1.8f, 3.0f, 0.9f}};
                    constexpr std::array primitives{
                        VfxPrimitive::Disc, VfxPrimitive::Ring, VfxPrimitive::Sector,
                        VfxPrimitive::Chevron, VfxPrimitive::Rune, VfxPrimitive::Cracks,
                        VfxPrimitive::Arrow, VfxPrimitive::Shard, VfxPrimitive::Ember,
                        VfxPrimitive::Spike, VfxPrimitive::ShockShell,
                        VfxPrimitive::SolidTrail, VfxPrimitive::DashedRicochet,
                        VfxPrimitive::FireTransfer, VfxPrimitive::RelicChain,
                        VfxPrimitive::DashWake};
                    for (std::size_t index = 0; index < primitives.size(); ++index)
                    {
                        ParticleSpawnCommand command;
                        command.sequence = 0xF000u + index;
                        command.tick = 0;
                        command.position = {
                            (static_cast<float>(index % 6) - 2.5f) * 3.2f,
                            index < 6 || index >= 11 ? 0.04f : 1.0f,
                            (static_cast<float>(index / 6) - 1.0f) * 4.0f};
                        command.direction = index >= 6 && index < 11
                                                ? Float3{0.5f, 0.35f, 1.0f}
                                                : Float3{0.0f, 1.0f, 0.0f};
                        command.shape = ParticleShape::Point;
                        command.velocity_mode = ParticleVelocity::Direction;
                        command.speed_min = command.speed_max =
                            index >= 6 && index < 11 ? 0.001f : 0.0f;
                        command.facing = index < 6 || index >= 11
                                             ? ParticleFacing::Ground
                                             : ParticleFacing::Velocity;
                        command.renderer = index < 6 ? VfxRenderer::Ground
                                           : index < 11 ? VfxRenderer::Mesh
                                                        : VfxRenderer::Segment;
                        command.primitive = primitives[index];
                        command.count = 1;
                        command.lifetime_min = command.lifetime_max = 10.0f;
                        command.start_color = command.end_color = colors[index % colors.size()];
                        command.start_size_min = command.start_size_max =
                            command.renderer == VfxRenderer::Segment ? 0.18f : 1.15f;
                        command.end_size_min = command.end_size_max =
                            command.start_size_min;
                        command.stretch = command.renderer == VfxRenderer::Segment ? 7.0f : 1.0f;
                        command.seed = static_cast<std::uint32_t>(index + 1);
                        pending_particle_spawns.push_back(command);
                    }
                    vfx_showcase_injected = true;
                }
                std::size_t particle_spawn_count{};
                while (particle_spawn_count < pending_particle_spawns.size() &&
                       pending_particle_spawns[particle_spawn_count].tick <=
                           snapshots.current.header.tick)
                {
                    ++particle_spawn_count;
                }
                std::size_t effect_line_count{};
                while (effect_line_count < pending_effect_lines.size() &&
                       pending_effect_lines[effect_line_count].tick <=
                           snapshots.current.header.tick)
                    ++effect_line_count;
                if (config.resize_test && !resized &&
                    snapshots.current.header.tick >= config.maximum_ticks / 2)
                {
                    if (auto resize_result =
                            renderer.Resize(config.width + 64, config.height + 36);
                        !resize_result)
                    {
                        record_thread_failure(resize_result);
                        break;
                    }
                    resized = true;
                }
                RendererFrameResult frame;
                const DevToolsFrameData devtools{
                    static_cast<std::uint32_t>(task_system.WorkerCount()),
                    static_cast<std::uint32_t>(channels.action_edges.Size()),
                    static_cast<std::uint32_t>(channels.presentation_events.Size()),
                    0,
                    static_cast<std::uint32_t>(channels.resize_commands.Size()),
                    static_cast<std::uint32_t>(channels.debug_commands.Size()),
                    channels.dropped_input_edges.load(std::memory_order_relaxed),
                    channels.dropped_presentation_events.load(std::memory_order_relaxed),
                    0};
                const auto frame_start = std::chrono::steady_clock::now();
                if (auto rendered =
                        renderer.Render(
                            snapshots, std::span(event_storage.data(), event_count),
                            std::span(pending_particle_spawns.data(), particle_spawn_count),
                            std::span(pending_effect_lines.data(), effect_line_count),
                            devtools, frame);
                    !rendered)
                {
                    record_thread_failure(rendered);
                    break;
                }
                pending_particle_spawns.erase(
                    pending_particle_spawns.begin(),
                    pending_particle_spawns.begin() + particle_spawn_count);
                pending_effect_lines.erase(pending_effect_lines.begin(),
                                           pending_effect_lines.begin() + effect_line_count);
                const auto frame_microseconds =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - frame_start)
                        .count();
                channels.rendered_frames.store(frame.frame, std::memory_order_release);
                channels.devtools_capture_mouse.store(frame.capture_mouse,
                                                       std::memory_order_release);
                channels.devtools_capture_keyboard.store(frame.capture_keyboard,
                                                          std::memory_order_release);
                if (frame.debug_command != 0)
                {
                    (void)channels.debug_commands.TryPush(
                        {static_cast<DebugCommandKind>(frame.debug_command - 1),
                         frame.debug_value, frame.debug_secondary, {}});
                }
                last_tick = frame.rendered_tick;
                timeline_lines.push_back(
                    std::format("{},{},{},{},{},{}\n", frame.frame, last_tick,
                                frame_microseconds, CurrentProcessWorkingSetBytes(),
                                snapshots.current.instances.size(),
                                devtools.input_queue_depth +
                                    devtools.presentation_queue_depth +
                                    devtools.particle_queue_depth +
                                    devtools.resize_queue_depth +
                                    devtools.debug_queue_depth));
                if (!active_vsync && active_frame_cap != 0 && !config.smoke)
                {
                    next_frame += std::chrono::nanoseconds(
                        1'000'000'000ull / std::clamp(active_frame_cap, 30u, 120u));
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
                        record_thread_failure(capture);
                        break;
                    }
                    channels.rendered_particles.store(renderer.LastParticleCount(),
                                                      std::memory_order_release);
                    const auto gpu_timings = renderer.LastGpuPassTimings();
                    if (gpu_timings.valid)
                    {
                        for (std::size_t pass = 0; pass < gpu_timings.nanoseconds.size(); ++pass)
                        {
                            gpu_lines.push_back(std::format(
                                "{},{},{},{}\n", frame.frame, last_tick, kRenderPassNames[pass],
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
            record_thread_failure(shutdown);
        }
        if (validation_errors != 0)
        {
            record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                        std::format("{} validation errors.", validation_errors)));
        }

        std::string timeline = "frame,tick,cpu_us,memory_bytes,ecs_count,queue_depth\n";
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

    auto simulation_ports = channels.ForSimulation();
    std::jthread simulation_thread([&, simulation_ports](std::stop_token) mutable {
        auto &channels = simulation_ports;
        SetThreadName(L"HS Simulation");
        GameSimulation simulation;
        SettingsData projection_settings = settings;
        if (auto initialized = simulation.Initialize(
                {effective_seed, config.smoke, !config.smoke},
                simulation_rules);
            !initialized)
        {
            record_thread_failure(initialized);
            channels.simulation_done.store(true, std::memory_order_release);
            return;
        }

        const auto captured_skill =
            config.skill_vfx_capture < kCombatSkillCount
                ? static_cast<SkillKind>(config.skill_vfx_capture)
                : SkillKind::Count;
        if (captured_skill != SkillKind::Count)
        {
            if (captured_skill != SkillKind::BasicAttack)
            {
                if (auto granted = simulation.ApplyDebugCommand(
                        {DebugCommandKind::GrantSkill,
                         static_cast<std::uint64_t>(captured_skill)});
                    !granted)
                {
                    record_thread_failure(granted);
                    channels.simulation_done.store(true, std::memory_order_release);
                    return;
                }
            }
            for (std::uint32_t upgrade = 0; upgrade < kUpgradeCount; ++upgrade)
            {
                if ((config.skill_vfx_upgrade_mask & (1u << upgrade)) == 0) continue;
                if (auto granted = simulation.ApplyDebugCommand(
                        {DebugCommandKind::GrantUpgrade,
                         static_cast<std::uint64_t>(captured_skill), upgrade});
                    !granted)
                {
                    record_thread_failure(granted);
                    channels.simulation_done.store(true, std::memory_order_release);
                    return;
                }
            }
            constexpr std::array enemy_positions{
                Float2{-2.0f, 5.0f}, Float2{0.0f, 5.0f}, Float2{2.0f, 5.0f},
                Float2{-3.0f, 7.0f}, Float2{-1.0f, 7.0f}, Float2{1.0f, 7.0f},
                Float2{3.0f, 7.0f},  Float2{-2.0f, 9.0f}, Float2{0.0f, 9.0f},
                Float2{2.0f, 9.0f},  Float2{-1.0f, 11.0f}, Float2{1.0f, 11.0f}};
            for (const auto position : enemy_positions)
            {
                if (auto spawned = simulation.ApplyDebugCommand(
                        {DebugCommandKind::SpawnEnemy, 0, 0, position});
                    !spawned)
                {
                    record_thread_failure(spawned);
                    channels.simulation_done.store(true, std::memory_order_release);
                    return;
                }
            }
        }
        if (config.monster_preview_asset < 6)
        {
            const auto boss = config.monster_preview_asset >= 3;
            if (auto spawned = simulation.ApplyDebugCommand(
                    {boss ? DebugCommandKind::SpawnBoss : DebugCommandKind::SpawnEnemy,
                     boss ? config.monster_preview_asset - 3 : config.monster_preview_asset,
                     0, {0.0f, 0.0f}});
                !spawned)
            {
                record_thread_failure(spawned);
                channels.simulation_done.store(true, std::memory_order_release);
                return;
            }
        }

        FixedStepClock clock;
        auto now = FixedStepClock::Clock::now();
        (void)clock.Advance(now);
        std::uint64_t observed_focus_epoch{};
        Sequence publish_sequence{};
        std::array<ActionEdge, 256> action_storage{};
        std::array<UiAction, 64> ui_action_storage{};
        auto previous_phase = config.smoke ? SessionPhase::Playing : SessionPhase::MainMenu;
        GameReadModelStorage read_model;
        EnemyAnimationState enemy_animations;
        std::size_t next_timeline_action{};
        std::size_t replay_frame_index{};
        if (!config.heartbeat_path.empty())
        {
            WriteText(config.heartbeat_path, "{\"tick\":0,\"state\":\"running\"}\n");
        }

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
                SimulationRules updated_game_data;
                while (channels.simulation_rules_updates.TryPop(updated_game_data))
                {
                    if (auto applied = simulation.ApplySimulationRules(updated_game_data); !applied)
                    {
                        record_thread_failure(applied);
                        break;
                    }
                }
                SettingsData updated_presentation_settings;
                while (channels.presentation_settings.TryPop(
                    updated_presentation_settings))
                    projection_settings = updated_presentation_settings;
                std::size_t ui_action_count{};
                UiAction ui_action;
                while (ui_action_count < ui_action_storage.size() &&
                       channels.ui_actions.TryPop(ui_action))
                    ui_action_storage[ui_action_count++] = ui_action;
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
                    const auto current_phase = static_cast<SessionPhase>(
                        channels.session_phase.load(std::memory_order_acquire));
                    const auto ui_page = static_cast<UiPage>(
                        channels.ui_page.load(std::memory_order_acquire));
                    if (action.kind == EdgeKind::Pressed &&
                        action.action == GameAction::Pause &&
                        ((current_phase == SessionPhase::Paused &&
                          ui_page == UiPage::PauseSettings) ||
                         (current_phase == SessionPhase::MainMenu &&
                          ui_page != UiPage::Root)))
                    {
                        channels.ui_page.store(static_cast<std::uint8_t>(UiPage::Root),
                                               std::memory_order_release);
                        continue;
                    }
                    if (action.kind == EdgeKind::Pressed &&
                        action.action == GameAction::CharacterPage)
                    {
                        channels.ui_page.store(
                            static_cast<std::uint8_t>(
                                current_phase == SessionPhase::Playing
                                    ? UiPage::CharacterOverview
                                    : UiPage::Root),
                            std::memory_order_release);
                        channels.loadout_source.store(0xFF, std::memory_order_release);
                    }
                    action_storage[action_count++] = action;
                }

                InputFrame input;
                if (replaying)
                {
                    if (replay_frame_index >= replay.frames.size())
                    {
                        channels.stop_requested.store(true, std::memory_order_release);
                        break;
                    }
                    input = replay.frames[replay_frame_index].View();
                }
                else
                {
                    input.target_tick =
                        channels.completed_tick.load(std::memory_order_relaxed) + 1;
                    input.held = channels.held_input.load(std::memory_order_acquire);
                    input.ordered_edges = std::span(action_storage.data(), action_count);
                    input.ui_actions =
                        std::span(ui_action_storage.data(), ui_action_count);
                }
                if (captured_skill != SkillKind::Count && !replaying)
                {
                    input.held.aim_world = {config.skill_vfx_aim.x, 0.0f,
                                            config.skill_vfx_aim.y};
                    input.held.basic_attack_held =
                        captured_skill == SkillKind::BasicAttack && input.target_tick >= 5;
                    const auto add_edge = [&](GameAction action, EdgeKind kind) {
                        action_storage[action_count++] =
                            {static_cast<Sequence>(0xF000u + input.target_tick), action, kind};
                        input.ordered_edges =
                            std::span(action_storage.data(), action_count);
                    };
                    if (captured_skill != SkillKind::BasicAttack && input.target_tick == 5)
                        add_edge(GameAction::SkillQ, EdgeKind::Pressed);
                    if (captured_skill == SkillKind::ChargedShot && input.target_tick == 65)
                        add_edge(GameAction::SkillQ, EdgeKind::Released);
                }
                if (next_timeline_action < config.timeline_actions.size() &&
                    config.timeline_actions[next_timeline_action].target_tick < input.target_tick)
                {
                    record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                                "Timeline action missed its target tick."));
                    break;
                }
                while (next_timeline_action < config.timeline_actions.size() &&
                       config.timeline_actions[next_timeline_action].target_tick ==
                           input.target_tick)
                {
                    const auto &scheduled = config.timeline_actions[next_timeline_action++];
                    if (auto applied = simulation.ApplyDebugCommand(
                            {static_cast<DebugCommandKind>(scheduled.kind), scheduled.value,
                             scheduled.secondary, scheduled.position});
                        !applied)
                    {
                        record_thread_failure(applied);
                        break;
                    }
                }
                if (channels.stop_requested.load(std::memory_order_acquire))
                    break;
                DebugCommand debug_command;
                while (channels.debug_commands.TryPop(debug_command))
                {
                    if (auto applied = simulation.ApplyDebugCommand(debug_command); !applied)
                    {
                        record_thread_failure(applied);
                        break;
                    }
                }
                const auto tick = simulation.TickFixed(input, FixedStepClock::kFixedStep);
                simulation.WriteReadModel(read_model);
                const auto model = read_model.View();
                enemy_animations.Update(model, simulation.PendingDomainSignals());
                const auto &probe = model.session;
                channels.PublishSessionProbe(probe);
                if (replaying && !config.replay_compare && tick.checksum !=
                                     replay.frames[replay_frame_index].expected_checksum)
                {
                    record_thread_failure(Result::Failure(
                        ErrorCode::InvalidState, "hs_playtest",
                        std::format("Replay checksum mismatch at tick {}.", tick.tick)));
                    break;
                }
                if (playtest_recorder.Active())
                {
                    const auto diagnostics = simulation.GetDiagnostics();
                    SimulationObservation recorded_probe;
                    static_cast<SessionProbe &>(recorded_probe) = probe;
                    recorded_probe.balance = diagnostics.balance;
                    if (auto recorded = playtest_recorder.Record(
                            input, tick.checksum, recorded_probe,
                            [&] {
                                std::vector<PresentationEvent> events;
                                events.reserve(simulation.PendingDomainSignals().size() * 2);
                                for (const auto &signal : simulation.PendingDomainSignals())
                                {
                                    std::array<PresentationEvent, 3> projected{};
                                    const auto count = ProjectDomainSignal(signal, projected);
                                    events.insert(events.end(), projected.begin(),
                                                  projected.begin() + count);
                                }
                                return events;
                            }()); !recorded)
                    {
                        record_thread_failure(recorded);
                        break;
                    }
                }
                if (replaying) ++replay_frame_index;
                channels.camera_target_x.store(probe.player_position.x,
                                               std::memory_order_release);
                channels.camera_target_z.store(probe.player_position.y,
                                               std::memory_order_release);
                channels.session_phase.store(static_cast<std::uint8_t>(tick.phase),
                                             std::memory_order_release);
                if ((tick.phase == SessionPhase::Victory ||
                     tick.phase == SessionPhase::Defeat) &&
                    tick.phase != previous_phase)
                {
                    channels.completed_run_kills.fetch_add(probe.kills,
                                                           std::memory_order_relaxed);
                    if (tick.phase == SessionPhase::Victory)
                    {
                        channels.completed_run_wins.fetch_add(1,
                                                              std::memory_order_relaxed);
                    }
                    auto best = channels.best_level.load(std::memory_order_relaxed);
                    while (best < probe.level &&
                           !channels.best_level.compare_exchange_weak(
                               best, probe.level, std::memory_order_relaxed))
                    {
                    }
                }
                previous_phase = tick.phase;

                for (const auto &signal : simulation.PendingDomainSignals())
                {
                    std::array<PresentationEvent, 3> projected{};
                    const auto count = ProjectDomainSignal(signal, projected);
                    for (const auto &presentation :
                         std::span(projected).first(count))
                        if (!channels.presentation_events.TryPush(presentation))
                            channels.dropped_presentation_events.fetch_add(
                                1, std::memory_order_relaxed);
                }
                simulation.ClearDomainSignals();
                if (auto slot = channels.snapshots.TryBeginWrite())
                {
                    slot->storage->camera.yaw_degrees = presentation_catalog.camera.yaw_degrees;
                    const auto zoom_percent = static_cast<float>(channels.camera_zoom_percent.load(std::memory_order_acquire));
                    const auto pose = ComputeRuntimeCameraPose(presentation_catalog.camera.distance_m, presentation_catalog.camera.pitch_degrees, zoom_percent);
                    slot->storage->camera.pitch_degrees = pose.pitch_degrees;
                    slot->storage->camera.vertical_fov_degrees =
                        presentation_catalog.camera.vertical_fov_degrees;
                    slot->storage->camera.distance = pose.distance_m;
                    if (ProjectRenderSnapshot(read_model.View(), presentation_catalog,
                                              {static_cast<UiPage>(channels.ui_page.load(
                                                   std::memory_order_acquire)),
                                               channels.collection_skill.load(std::memory_order_acquire),
                                               channels.character_skill.load(std::memory_order_acquire),
                                               channels.loadout_source.load(std::memory_order_acquire)},
                                              projection_settings,
                                              *slot->storage,
                                               channels.pending_rebind_slot.load(
                                                   std::memory_order_acquire),
                                               &enemy_animations))
                    {
                        slot->storage->camera.target.y = pose.target_height_m;
                        channels.snapshots.Publish(*slot, ++publish_sequence);
                    }
                    else
                    {
                        channels.snapshots.Abandon(*slot);
                        record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_gameplay",
                                                    "Render snapshot capacity exceeded."));
                        break;
                    }
                }
                channels.completed_tick.store(tick.tick, std::memory_order_release);
                channels.gameplay_checksum.store(tick.checksum, std::memory_order_release);
                if (!config.heartbeat_path.empty() && tick.tick % 60 == 0)
                {
                    WriteText(config.heartbeat_path,
                              std::format("{{\"tick\":{},\"state\":\"running\"}}\n",
                                          tick.tick));
                }

                if (tick.phase == SessionPhase::QuitRequested)
                {
                    channels.stop_requested.store(true, std::memory_order_release);
                    break;
                }

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
            record_thread_failure(shutdown);
        }
        if (!config.heartbeat_path.empty())
        {
            WriteText(config.heartbeat_path,
                      std::format("{{\"tick\":{},\"state\":\"finished\"}}\n",
                                  channels.completed_tick.load(std::memory_order_acquire)));
        }
        channels.simulation_done.store(true, std::memory_order_release);
    });

#if defined(HS_DEVELOPMENT_TOOLS)
    std::error_code hot_reload_error;
    auto observed_cooked_write =
        std::filesystem::last_write_time(cooked_simulation_rules_path, hot_reload_error);
    auto next_hot_reload_check = std::chrono::steady_clock::now();
#endif

    SessionPhase previous_audio_phase = SessionPhase::MainMenu;
    while (!channels.stop_requested.load(std::memory_order_acquire))
    {
        if (!window.PumpMessages())
        {
            channels.stop_requested.store(true, std::memory_order_release);
            break;
        }
#if defined(HS_DEVELOPMENT_TOOLS)
        if (std::chrono::steady_clock::now() >= next_hot_reload_check)
        {
            next_hot_reload_check = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(250);
            hot_reload_error.clear();
            const auto write =
                std::filesystem::last_write_time(cooked_simulation_rules_path, hot_reload_error);
            if (!hot_reload_error && write != observed_cooked_write)
            {
                observed_cooked_write = write;
                std::ofstream log(config.artifact_directory / "hot_reload.log",
                                  std::ios::app);
                if (static_cast<SessionPhase>(channels.session_phase.load(
                        std::memory_order_acquire)) != SessionPhase::MainMenu)
                {
                    log << "rejected active_gameplay\n";
                }
                else
                {
                    SimulationRules replacement;
                    std::uint64_t replacement_hash{};
                    if (auto loaded = LoadSimulationRules(cooked_simulation_rules_path,
                                                          replacement,
                                                          &replacement_hash);
                        !loaded)
                    {
                        log << "rejected validation " << loaded.Message() << '\n';
                    }
                    else if (!channels.simulation_rules_updates.TryPush(replacement))
                    {
                        log << "rejected queue_full\n";
                    }
                    else
                    {
                        content_hash = replacement_hash;
                        simulation_rules_hash = SimulationRulesHash(replacement);
                        log << "applied content_hash=" << replacement_hash << '\n';
                    }
                }
            }
        }
#endif
        UiCommand ui_command;
        bool settings_changed{};
        while (channels.ui_commands.TryPop(ui_command))
        {
            switch (ui_command.kind)
            {
            case UiCommandKind::SetBorderless:
                settings.borderless = ui_command.value != 0;
                if (auto applied = window.SetBorderless(settings.borderless); !applied)
                    record_thread_failure(applied);
                settings_changed = true;
                break;
            case UiCommandKind::SetVsync:
                settings.vsync = ui_command.value != 0;
                settings_changed = true;
                break;
            case UiCommandKind::SetFrameCap:
                settings.frame_cap = ui_command.value;
                settings_changed = true;
                break;
            case UiCommandKind::SetRenderScale:
                settings.render_scale_percent = ui_command.value;
                settings_changed = true;
                break;
            case UiCommandKind::SetShadowResolution:
                settings.shadow_resolution = ui_command.value;
                settings_changed = true;
                break;
            case UiCommandKind::SetParticlePercentage:
                settings.particle_percentage = ui_command.value;
                settings_changed = true;
                break;
            case UiCommandKind::SetBloom:
                settings.bloom = ui_command.value != 0;
                settings_changed = true;
                break;
            case UiCommandKind::SetOutline:
                settings.outline = ui_command.value != 0;
                settings_changed = true;
                break;
            case UiCommandKind::SetMasterVolumePercent:
                settings.master_volume = static_cast<float>(ui_command.value) / 100.0f;
                settings_changed = true;
                break;
            case UiCommandKind::SetBgmVolumePercent:
                settings.bgm_volume = static_cast<float>(ui_command.value) / 100.0f;
                settings_changed = true;
                break;
            case UiCommandKind::SetSfxVolumePercent:
                settings.sfx_volume = static_cast<float>(ui_command.value) / 100.0f;
                settings_changed = true;
                break;
            case UiCommandKind::SetUiVolumePercent:
                settings.ui_volume = static_cast<float>(ui_command.value) / 100.0f;
                settings_changed = true;
                break;
            case UiCommandKind::BeginSkillRebind:
                window.BeginSkillRebind(ui_command.value);
                channels.pending_rebind_slot.store(
                    static_cast<std::uint8_t>(ui_command.value),
                    std::memory_order_release);
                break;
            case UiCommandKind::CancelSkillRebind:
                window.CancelSkillRebind();
                channels.pending_rebind_slot.store(0xFF, std::memory_order_release);
                break;
            }
        }
        if (settings_changed)
        {
            audio.ApplySettings(settings);
            if (!channels.renderer_settings.TryPush(settings) ||
                !channels.presentation_settings.TryPush(settings))
                record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                            "Settings queue capacity exceeded."));
            else if (auto saved = save_store.SaveSettings(settings); !saved)
                record_thread_failure(saved);
        }
        std::array<std::uint16_t, 4> rebound_keys;
        if (window.ConsumeReboundSkillKeys(rebound_keys))
        {
            audio.Play(MakeAudioEvent("audio.ui.rebind_success", ++ui_audio_sequence));
            channels.pending_rebind_slot.store(0xFF, std::memory_order_release);
            settings.skill_virtual_keys = rebound_keys;
            if (!channels.presentation_settings.TryPush(settings))
                record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                            "Presentation settings queue capacity exceeded."));
            if (auto saved = save_store.SaveSettings(settings); !saved)
                record_thread_failure(saved);
        }
        PresentationEvent audio_event;
        while (channels.audio_events.TryPop(audio_event))
        {
            if (DecodeAudioAction(audio_event.parameters) == AudioEventAction::Stop)
                audio.Stop(audio_event.asset);
            else
            {
                if (audio_event.asset.value == MakeAssetId("audio.bgm.final_boss").value)
                {
                    if (active_bgm.value != 0) audio.Stop(active_bgm);
                    active_bgm = MakeAssetId("audio.bgm.final_boss");
                }
                audio.Play(audio_event);
            }
        }
        audio.UpdateListener(
            {channels.camera_target_x.load(std::memory_order_acquire), 0.0f,
             channels.camera_target_z.load(std::memory_order_acquire)},
            {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f});
        const auto session_phase = static_cast<SessionPhase>(
            channels.session_phase.load(std::memory_order_acquire));
        if (session_phase != previous_audio_phase)
        {
            const auto old_phase = previous_audio_phase;
            const auto target = session_phase == SessionPhase::MainMenu
                                    ? MakeAssetId("audio.bgm.main_menu")
                                    : (session_phase == SessionPhase::Victory
                                           ? MakeAssetId("audio.stinger.victory")
                                           : (session_phase == SessionPhase::Defeat
                                                  ? MakeAssetId("audio.stinger.defeat")
                                                  : MakeAssetId("audio.bgm.session")));
            if (session_phase == SessionPhase::Victory || session_phase == SessionPhase::Defeat)
            {
                if (active_bgm.value != 0) audio.Stop(active_bgm);
                if (active_ambience.value != 0) audio.Stop(active_ambience);
                active_ambience = {};
                audio.Play(MakeAudioEvent(target.value == MakeAssetId("audio.stinger.victory").value
                                              ? "audio.stinger.victory" : "audio.stinger.defeat",
                                          ++ui_audio_sequence));
                active_bgm = {};
            }
            else if (target.value != active_bgm.value)
            {
                if (active_bgm.value != 0) audio.Stop(active_bgm);
                active_bgm = target;
                audio.Play(MakeAudioEvent(session_phase == SessionPhase::MainMenu
                                              ? "audio.bgm.main_menu" : "audio.bgm.session",
                                          ++ui_audio_sequence));
            }
            if (session_phase == SessionPhase::Playing && active_ambience.value == 0)
            {
                active_ambience = MakeAssetId("audio.ambience.arena");
                audio.Play(MakeAudioEvent("audio.ambience.arena", ++ui_audio_sequence));
            }
            else if (session_phase != SessionPhase::Playing && active_ambience.value != 0)
            {
                audio.Stop(active_ambience);
                active_ambience = {};
            }
            if (session_phase == SessionPhase::Paused)
                audio.Play(MakeAudioEvent("audio.ui.pause_open", ++ui_audio_sequence));
            else if (old_phase == SessionPhase::Paused)
                audio.Play(MakeAudioEvent("audio.ui.pause_close", ++ui_audio_sequence));
            else if (session_phase == SessionPhase::CardSelection ||
                     session_phase == SessionPhase::StatAllocation)
                audio.Play(MakeAudioEvent("audio.ui.level_up", ++ui_audio_sequence));
            else if (session_phase == SessionPhase::RelicSelection)
                audio.Play(MakeAudioEvent("audio.ui.relic_select", ++ui_audio_sequence));
            previous_audio_phase = session_phase;
        }
        if (session_phase == SessionPhase::Paused ||
            session_phase == SessionPhase::CardSelection ||
            session_phase == SessionPhase::StatAllocation ||
            session_phase == SessionPhase::RelicSelection)
        {
            audio.PauseCombat();
        }
        else
        {
            audio.ResumeCombat();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    window.StopInput();
    channels.stop_requested.store(true, std::memory_order_release);
    simulation_thread.join();
    render_thread.join();
    audio.Shutdown();

    if (!config.smoke)
    {
        profile.best_level = channels.best_level.load(std::memory_order_relaxed);
        profile.total_wins += channels.completed_run_wins.load(std::memory_order_relaxed);
        profile.total_kills += channels.completed_run_kills.load(std::memory_order_relaxed);
        if (auto saved = save_store.SaveProfile(profile); !saved)
        {
            record_thread_failure(saved);
        }
    }

    {
        std::lock_guard lock(result_mutex);
        result = thread_result;
    }
    const auto final_tick = channels.completed_tick.load(std::memory_order_acquire);
    const auto checksum = channels.gameplay_checksum.load(std::memory_order_acquire);
    const auto rendered_frames = channels.rendered_frames.load(std::memory_order_acquire);
    const auto rendered_particles =
        channels.rendered_particles.load(std::memory_order_acquire);
    const auto valid =
        result && final_tick >= (config.smoke ? config.maximum_ticks : 0) && rendered_frames > 0 &&
        rendered_particles <= 10'000u &&
        channels.dropped_input_edges.load() == 0 &&
        channels.dropped_presentation_events.load() == 0;
    if (playtest_recorder.Active())
    {
        if (auto finished = playtest_recorder.Finish(valid); !finished)
        {
            result = finished;
        }
    }

    WriteText(config.artifact_directory / "spec.json",
              std::format("{{\"scenario_id\":\"runtime-smoke\",\"seed\":{},\"maximum_ticks\":{},"
                          "\"content_hash\":{},\"mode\":\"offscreen_render\"}}\n",
                          config.seed, config.maximum_ticks, content_hash));
    WriteText(config.artifact_directory / "result.json",
              std::format("{{\"schema_version\":1,\"scenario_id\":\"runtime-smoke\","
                          "\"mode\":\"offscreen_render\",\"execution_valid\":{},"
                          "\"assertions_passed\":{},"
                          "\"tick\":{},\"checksum\":{},\"rendered_frames\":{},"
                          "\"rendered_particles\":{},"
                          "\"content_hash\":{},"
                          "\"worker_count\":{},"
                          "\"dropped_input_edges\":{},\"dropped_presentation_events\":{},"
                          "\"particle_effects_hash\":{},"
                          "\"user_review\":\"awaiting\"}}\n",
                          valid ? "true" : "false", valid ? "true" : "false", final_tick,
                          checksum, rendered_frames, rendered_particles, content_hash,
                          task_system.WorkerCount(),
                          channels.dropped_input_edges.load(),
                          channels.dropped_presentation_events.load(),
                          vfx_catalog.PayloadHash()));

    if (!valid && result)
    {
        result = Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                 "Runtime execution assertions failed.");
    }
    return {result, final_tick, checksum, rendered_frames, playtest_recorder.Directory()};
}

} // namespace hs
