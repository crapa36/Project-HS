#include <hs/game_rules/simulation_rules_io.hpp>
#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/core/cooked_format.hpp>
#include <hs/core/process_info.hpp>
#include <hs/core/shipped_content_owner.hpp>
#include <hs/gameplay/game_simulation.hpp>
#include <hs/presentation/projector.hpp>
#include <hs/runtime/application.hpp>
#include <hs/runtime/experiment_spec.hpp>
#include <hs/runtime/shipping_content.hpp>

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

extern "C"
{
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}

namespace
{

std::string UtcNow()
{
    SYSTEMTIME time{};
    GetSystemTime(&time);
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
                       time.wYear, time.wMonth, time.wDay, time.wHour,
                       time.wMinute, time.wSecond, time.wMilliseconds);
}

struct Arguments
{
    std::optional<std::filesystem::path> spec_path;
    std::optional<std::filesystem::path> artifact_directory;
    std::optional<std::filesystem::path> generation_pointer;
    bool warp{};
    bool simulation_only{};
};

[[nodiscard]] hs::Result ParseArguments(int argc, char **argv, Arguments &arguments)
{
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        if (argument.starts_with("--spec=") && !arguments.spec_path)
        {
            arguments.spec_path = argument.substr(7);
        }
        else if (argument.starts_with("--artifacts=") && !arguments.artifact_directory)
        {
            arguments.artifact_directory = argument.substr(12);
        }
        else if (argument.starts_with("--generation=") && !arguments.generation_pointer)
        {
            arguments.generation_pointer = argument.substr(13);
        }
        else if (argument == "--warp")
        {
            arguments.warp = true;
        }
        else if (argument == "--simulation-only")
        {
            arguments.simulation_only = true;
        }
        else
        {
            return hs::Result::Failure(hs::ErrorCode::InvalidArgument, "hs_experiment",
                                       std::format("Unknown or duplicate argument: {}", argument));
        }
    }
    if ((arguments.spec_path && arguments.spec_path->empty()) ||
        (arguments.artifact_directory && arguments.artifact_directory->empty()) ||
        (arguments.generation_pointer && arguments.generation_pointer->empty()))
    {
        return hs::Result::Failure(hs::ErrorCode::InvalidArgument, "hs_experiment",
                                   "Spec and artifact paths must not be empty.");
    }
    return hs::Result::Success();
}

[[nodiscard]] std::optional<std::uint64_t> HashFile(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        return std::nullopt;
    }
    const auto size = stream.tellg();
    if (size < 0)
    {
        return std::nullopt;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char *>(bytes.data()), size);
    if (!stream)
    {
        return std::nullopt;
    }
    return hs::Fnv1a64(bytes);
}

[[nodiscard]] bool EqualHex(std::string_view left, std::string_view right) noexcept
{
    return std::ranges::equal(left, right, [](unsigned char lhs, unsigned char rhs) {
        return std::tolower(lhs) == std::tolower(rhs);
    });
}

[[nodiscard]] hs::Result ClearPreviousArtifacts(
    const std::filesystem::path &directory, const hs::ExperimentSpec &spec)
{
    constexpr std::array<std::string_view, 18> names{
        "adapter.txt", "camera.json", "d3d12_validation.log", "dred.txt",
        "dred_config.json", "events.ndjson", "gpu_passes.csv", "heartbeat.json",
        "particle_stats.json", "render.json",
        "result.json", "shader_hot_reload.log", "spec.json", "timeline.csv",
        "watchdog.dmp", "watchdog.json", "input_spec.json", "child_failure.dmp"};
    std::vector<std::filesystem::path> paths;
    paths.reserve(names.size() + spec.captures.size());
    for (const auto name : names) paths.emplace_back(name);
    for (const auto &capture : spec.captures) paths.emplace_back(capture.path);

    for (const auto &relative : paths)
    {
        if (relative.empty() || relative.is_absolute() ||
            std::ranges::find(relative, std::filesystem::path("..")) != relative.end())
        {
            return hs::Result::Failure(hs::ErrorCode::InvalidArgument, "hs_experiment",
                                       "Artifact paths must stay inside the artifact directory.");
        }
        std::error_code error;
        std::filesystem::remove(directory / relative, error);
        if (error)
        {
            return hs::Result::Failure(hs::ErrorCode::InvalidState, "hs_experiment",
                                       error.message());
        }
    }
    return hs::Result::Success();
}

[[nodiscard]] hs::ExperimentSpec DefaultSpec(std::string build_hash,
                                             std::string content_hash)
{
    hs::ExperimentSpec spec;
    spec.scenario_id = "simulation";
    spec.seed = 1;
    spec.mode = hs::ExperimentMode::OffscreenRender;
    spec.build_hash = std::move(build_hash);
    spec.content_hash = std::move(content_hash);
    spec.probes = {"gpu.pass", "gameplay.checksum"};
    spec.captures.push_back({180, "capture_tick_180.png"});
    spec.assertions.push_back(
        {"execution-valid", "/execution_valid", "eq", true, 0.0});
    spec.assertions.push_back(
        {"runtime-assertions", "/assertions_passed", "eq", true, 0.0});
    spec.termination = {180, false, false, true};
    return spec;
}

[[nodiscard]] hs::Result ValidateRunnerSupport(const hs::ExperimentSpec &spec)
{
    if (spec.mode == hs::ExperimentMode::SimulationOnly)
    {
        if (!spec.captures.empty())
        {
            return hs::Result::Failure(hs::ErrorCode::InvalidArgument, "hs_experiment",
                                       "simulation_only does not produce PNG captures.");
        }
        return hs::Result::Success();
    }
    if (spec.termination.on_victory || spec.termination.on_defeat)
    {
        return hs::Result::Failure(
            hs::ErrorCode::InvalidArgument, "hs_experiment",
            "Current RunApplication contract supports maximum_tick termination only.");
    }
    if (std::ranges::any_of(spec.captures, [&](const hs::ExperimentCapture &capture) {
            return capture.tick != spec.termination.maximum_tick;
        }))
    {
        return hs::Result::Failure(
            hs::ErrorCode::InvalidArgument, "hs_experiment",
            "Current render runner captures termination.maximum_tick only.");
    }
    return hs::Result::Success();
}

[[nodiscard]] hs::Result DecodeAction(const hs::ExperimentTimelineAction &action,
                                      hs::DebugCommand &command)
{
    try
    {
        const auto payload = nlohmann::json::parse(action.payload_json);
        const auto unsigned_value = [&](std::string_view name,
                                        std::uint64_t fallback = 0) {
            return payload.contains(name) ? payload.at(name).get<std::uint64_t>() : fallback;
        };
        const auto position = [&] {
            if (!payload.contains("position")) return hs::Float2{};
            const auto &value = payload.at("position");
            if (!value.is_array() || value.size() != 2)
            {
                throw std::runtime_error("position must be [x,z]");
            }
            return hs::Float2{value.at(0).get<float>(), value.at(1).get<float>()};
        }();

        if (action.command == "start_session")
            command.kind = hs::DebugCommandKind::StartSession;
        else if (action.command == "grant_skill")
        {
            command.kind = hs::DebugCommandKind::GrantSkill;
            command.value = unsigned_value("skill");
        }
        else if (action.command == "grant_upgrade")
        {
            command.kind = hs::DebugCommandKind::GrantUpgrade;
            command.value = unsigned_value("skill");
            command.secondary = static_cast<std::uint32_t>(unsigned_value("upgrade"));
        }
        else if (action.command == "grant_relic")
        {
            command.kind = hs::DebugCommandKind::GrantRelic;
            command.value = unsigned_value("relic");
        }
        else if (action.command == "set_growth_tick")
        {
            command.kind = hs::DebugCommandKind::SetGrowthTick;
            command.value = unsigned_value("tick");
        }
        else if (action.command == "damage_player")
        {
            command.kind = hs::DebugCommandKind::DamagePlayer;
            command.value = unsigned_value("amount");
        }
        else if (action.command == "heal_player")
        {
            command.kind = hs::DebugCommandKind::HealPlayer;
            command.value = unsigned_value("amount");
        }
        else if (action.command == "spawn_enemy")
        {
            command.kind = hs::DebugCommandKind::SpawnEnemy;
            command.value = unsigned_value("kind");
            command.position = position;
        }
        else if (action.command == "spawn_boss")
        {
            command.kind = hs::DebugCommandKind::SpawnBoss;
            command.value = unsigned_value("kind");
        }
        else
        {
            return hs::Result::Failure(hs::ErrorCode::InvalidArgument, "hs_experiment",
                                       "Unsupported timeline command.");
        }
        return hs::Result::Success();
    }
    catch (const std::exception &exception)
    {
        return hs::Result::Failure(hs::ErrorCode::InvalidArgument, "hs_experiment",
                                   std::format("Action {} payload: {}", action.sequence,
                                               exception.what()));
    }
}

[[nodiscard]] hs::Result PopulateApplicationTimeline(const hs::ExperimentSpec &spec,
                                                     hs::ApplicationConfig &config)
{
    config.timeline_actions.clear();
    config.timeline_actions.reserve(spec.timeline_actions.size());
    for (const auto &action : spec.timeline_actions)
    {
        hs::DebugCommand command;
        if (auto decoded = DecodeAction(action, command); !decoded) return decoded;
        config.timeline_actions.push_back(
            {action.sequence, action.target_tick,
             static_cast<std::uint8_t>(command.kind), command.value,
             command.secondary, command.position});
    }
    return hs::Result::Success();
}

[[nodiscard]] hs::ApplicationResult
RunSimulationExperiment(const hs::ExperimentSpec &spec,
                        const std::filesystem::path &directory,
                        const hs::SimulationRules &rules)
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
    {
        return {hs::Result::Failure(hs::ErrorCode::InvalidState, "hs_experiment",
                                    error.message())};
    }

    hs::GameSimulation simulation;
    if (auto initialized = simulation.Initialize({spec.seed, true, false},
                                                 rules);
        !initialized)
    {
        return {initialized};
    }

    std::ofstream timeline(directory / "timeline.csv", std::ios::trunc);
    std::ofstream events(directory / "events.ndjson", std::ios::trunc);
    timeline << "tick,checksum,phase,enemies,projectiles,pickups,cpu_us,memory_bytes,ecs_count,queue_depth\n";
    std::size_t next_action{};
    hs::TickResult tick;
    hs::Result result = hs::Result::Success();
    for (hs::Tick target = 1; target <= spec.termination.maximum_tick; ++target)
    {
        const auto tick_start = std::chrono::steady_clock::now();
        while (next_action < spec.timeline_actions.size() &&
               spec.timeline_actions[next_action].target_tick == target)
        {
            hs::DebugCommand command;
            const auto &action = spec.timeline_actions[next_action++];
            if (auto decoded = DecodeAction(action, command); !decoded)
            {
                result = decoded;
                break;
            }
            if (auto applied = simulation.ApplyDebugCommand(command); !applied)
            {
                result = applied;
                break;
            }
            events << nlohmann::json{{"sequence", action.sequence},
                                     {"target_tick", action.target_tick},
                                     {"command", action.command},
                                     {"ack", "applied"}}
                              .dump()
                   << '\n';
        }
        if (!result) break;

        hs::InputFrame input;
        input.target_tick = target;
        tick = simulation.TickFixed(input);
        const auto probe = simulation.GetObservation();
        const auto cpu_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - tick_start)
                                .count();
        const auto ecs_count = 1u + probe.normal_enemy_count + probe.boss_count +
                               probe.player_projectile_count +
                               probe.enemy_projectile_count + probe.pickup_count;
        timeline << tick.tick << ',' << tick.checksum << ','
                 << static_cast<unsigned>(tick.phase) << ',' << probe.normal_enemy_count << ','
                 << probe.player_projectile_count + probe.enemy_projectile_count << ','
                 << probe.pickup_count << ',' << cpu_us << ','
                 << hs::CurrentProcessWorkingSetBytes() << ','
                 << ecs_count << ",0\n";
        for (const auto &signal : simulation.PendingDomainSignals())
        {
            std::array<hs::PresentationEvent, 3> projected{};
            const auto count = hs::ProjectDomainSignal(signal, projected);
            for (const auto &event : std::span(projected).first(count))
                events << nlohmann::json{{"sequence", event.sequence},
                                         {"tick", event.tick},
                                         {"kind", static_cast<unsigned>(event.kind)}}
                                  .dump()
                       << '\n';
        }
        simulation.ClearDomainSignals();
        if ((spec.termination.on_victory && tick.phase == hs::SessionPhase::Victory) ||
            (spec.termination.on_defeat && tick.phase == hs::SessionPhase::Defeat))
            break;
    }

    const auto probe = simulation.GetObservation();
    if (auto shutdown = simulation.Shutdown(); !shutdown && result) result = shutdown;
    const bool valid = result.Succeeded() && timeline.good() && events.good();
    nlohmann::json output{{"schema_version", 1},
                          {"scenario_id", spec.scenario_id},
                          {"mode", "simulation_only"},
                          {"execution_valid", valid},
                          {"assertions_passed", valid},
                          {"tick", tick.tick},
                          {"checksum", tick.checksum},
                          {"phase", static_cast<unsigned>(tick.phase)},
                          {"level", probe.level},
                          {"health", probe.health},
                          {"kills", probe.kills},
                          {"normal_enemy_count", probe.normal_enemy_count},
                          {"boss_count", probe.boss_count},
                          {"player_projectile_count", probe.player_projectile_count},
                          {"enemy_projectile_count", probe.enemy_projectile_count},
                          {"pickup_count", probe.pickup_count},
                          {"memory_bytes", hs::CurrentProcessWorkingSetBytes()},
                          {"ecs_count", 1u + probe.normal_enemy_count + probe.boss_count +
                                            probe.player_projectile_count +
                                            probe.enemy_projectile_count + probe.pickup_count},
                          {"queue_depth", 0},
                          {"user_review", "awaiting"}};
    std::ofstream(directory / "result.json", std::ios::trunc) << output.dump(2) << '\n';
    return {result, tick.tick, tick.checksum, 0};
}

} // namespace

int main(int argc, char **argv)
{
    Arguments arguments;
    if (auto parsed = ParseArguments(argc, argv, arguments); !parsed)
    {
        std::cerr << parsed.Message() << '\n';
        return 2;
    }

    const auto executable = hs::CurrentExecutablePath();
    std::filesystem::path generation_pointer;
    auto simulation_rules = std::make_unique<hs::SimulationRules>();
    std::uint64_t content_hash_value{};
    if (arguments.generation_pointer)
    {
        generation_pointer = std::filesystem::absolute(*arguments.generation_pointer);
        hs::SelectedGeneration selected_generation;
        if (auto loaded = hs::LoadSelectedGeneration(generation_pointer, selected_generation); !loaded)
        {
            std::cerr << loaded.Message() << '\n';
            return 2;
        }
        std::filesystem::path simulation_rules_path;
        if (auto resolved = selected_generation.Require("game-data/simulation-rules", simulation_rules_path); !resolved)
        {
            std::cerr << resolved.Message() << '\n';
            return 2;
        }
        if (auto loaded = hs::LoadSimulationRules(simulation_rules_path, *simulation_rules, &content_hash_value); !loaded)
        {
            std::cerr << loaded.Message() << '\n';
            return 2;
        }
    }
    else
    {
        const auto &info = hs::shipping::ImageInfo();
        hs::ShippedContentOwner owner;
        if (auto opened = hs::shipping::OpenImage(executable.parent_path(), info, owner); !opened)
        {
            std::cerr << opened.Message() << '\n';
            return 2;
        }
        std::vector<std::byte> bytes;
        if (auto read = hs::shipping::ReadRange(owner, info.tables[static_cast<std::size_t>(hs::shipping::ShippingTable::Simulation)], bytes); !read)
        {
            std::cerr << read.Message() << '\n';
            return 2;
        }
        if (auto decoded = hs::DecodeSimulationRules(bytes, *simulation_rules); !decoded)
        {
            std::cerr << decoded.Message() << '\n';
            return 2;
        }
        content_hash_value = info.content_hash;
    }
    const auto build_hash_value = HashFile(executable);
    if (executable.empty() || !build_hash_value || !content_hash_value)
    {
        std::cerr << "Cannot calculate build/content hashes.\n";
        return 2;
    }
    const auto build_hash = std::format("{:016x}", *build_hash_value);
    const auto content_hash = std::format("{:016x}", content_hash_value);

    hs::ExperimentSpec spec;
    if (arguments.spec_path)
    {
        if (auto loaded = hs::LoadExperimentSpec(*arguments.spec_path, spec); !loaded)
        {
            std::cerr << loaded.Message() << '\n';
            return 2;
        }
        if (!EqualHex(spec.build_hash, build_hash) ||
            !EqualHex(spec.content_hash, content_hash))
        {
            std::cerr << "Spec build_hash/content_hash does not match this runtime.\n";
            return 2;
        }
    }
    else
    {
        spec = DefaultSpec(build_hash, content_hash);
        if (arguments.simulation_only)
        {
            spec.mode = hs::ExperimentMode::SimulationOnly;
            spec.captures.clear();
        }
        std::ranges::sort(spec.timeline_actions, {}, [](const auto &action) {
            return std::pair{action.target_tick, action.sequence};
        });
    }
    hs::ApplicationConfig config;
    config.smoke = true;
    config.mute_audio = true;
    config.visible = spec.mode == hs::ExperimentMode::PresentPerformance;
    config.capture_diagnostics = config.visible;
    config.vsync = false;
    config.frame_cap = 0;
    config.validation = true;
    config.seed = spec.seed;
    config.maximum_ticks = spec.termination.maximum_tick;
    config.artifact_directory = arguments.artifact_directory.value_or(
        std::filesystem::path("Artifacts") / spec.scenario_id);
    config.generation_pointer = generation_pointer;
    if (auto applied = hs::ApplyExperimentOverrides(spec, config); !applied)
    {
        std::cerr << applied.Message() << '\n';
        return 2;
    }
    config.warp = config.warp || arguments.warp;
    config.artifact_directory = std::filesystem::absolute(config.artifact_directory);
    std::error_code directory_error;
    std::filesystem::create_directories(config.artifact_directory, directory_error);
    if (directory_error)
    {
        std::cerr << "Cannot create artifact directory: " << directory_error.message() << '\n';
        return 2;
    }

    if (auto supported = ValidateRunnerSupport(spec); !supported)
    {
        std::cerr << supported.Message() << '\n';
        return 2;
    }

    if (auto cleared = ClearPreviousArtifacts(config.artifact_directory, spec); !cleared)
    {
        std::cerr << cleared.Message() << '\n';
        return 2;
    }

    if (auto timeline = PopulateApplicationTimeline(spec, config); !timeline)
    {
        std::cerr << timeline.Message() << '\n';
        return 2;
    }

    const auto start_utc = UtcNow();
    const auto run = spec.mode == hs::ExperimentMode::SimulationOnly
                         ? RunSimulationExperiment(
                               spec, config.artifact_directory,
                               *simulation_rules)
                         : hs::RunApplication(config);
    if (!run.result)
    {
        std::cerr << run.result.Subsystem() << ": " << run.result.Message() << '\n';
        return 1;
    }
    const auto end_utc = UtcNow();
    if (auto finalized = hs::FinalizeExperimentArtifacts(
            spec, config.artifact_directory, start_utc, end_utc, 0);
        !finalized)
    {
        std::cerr << finalized.Message() << '\n';
        return 1;
    }
    std::cout << "experiment=" << spec.scenario_id << " tick=" << run.final_tick
              << " checksum=" << run.checksum << " frames=" << run.rendered_frames << '\n';
    return 0;
}
