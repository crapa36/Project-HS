#include <hs/runtime/experiment_spec.hpp>
#include <hs/runtime/experiment_pipe.hpp>
#include <hs/runtime/save_store.hpp>
#include <hs/runtime/playtest_recording.hpp>
#include <hs/core/cooked_format.hpp>
#include "vfx_catalog.hpp"

#include <Windows.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

void Check(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

std::filesystem::path TestDirectory()
{
    return std::filesystem::temp_directory_path() /
           (L"ProjectHS_runtime_" + std::to_wstring(GetCurrentProcessId()));
}

void WriteCorrupt(const std::filesystem::path &path)
{
    std::ofstream(path, std::ios::trunc) << "{broken";
}

void TestSaveRecovery(const std::filesystem::path &root)
{
    hs::SaveStore store(root);
    hs::ProfileData first;
    first.best_level = 7;
    Check(store.SaveProfile(first).Succeeded(), "first profile save");

    auto second = first;
    second.best_level = 11;
    Check(store.SaveProfile(second).Succeeded(), "second profile save");
    Check(std::filesystem::is_regular_file(root / "profile.json.bak"),
          "atomic replacement retains backup");

    WriteCorrupt(root / "profile.json");
    hs::ProfileData recovered;
    Check(store.LoadProfile(recovered).Succeeded() && recovered.best_level == 7,
          "corrupt primary restores valid backup");

    WriteCorrupt(root / "profile.json");
    WriteCorrupt(root / "profile.json.bak");
    hs::ProfileData reset;
    Check(store.LoadProfile(reset).Succeeded() && reset.best_level == 1,
          "two corrupt copies reset profile");
    std::size_t corrupt_count{};
    for (const auto &entry : std::filesystem::directory_iterator(root))
    {
        if (entry.path().extension() == ".corrupt") ++corrupt_count;
    }
    Check(corrupt_count >= 2, "corrupt profile copies are preserved");
}

void TestSettingsPersistence(const std::filesystem::path &root)
{
    hs::SaveStore store(root);
    hs::SettingsData first;
    first.vsync = false;
    first.frame_cap = 120;
    first.master_volume = 0.7f;
    first.skill_virtual_keys = {'W', 'Q', 'E', 'R'};
    Check(store.SaveSettings(first).Succeeded(), "settings save");

    hs::SettingsData loaded;
    Check(store.LoadSettings(loaded).Succeeded() && !loaded.vsync &&
              loaded.frame_cap == 120 && loaded.master_volume == 0.7f &&
              loaded.skill_virtual_keys == first.skill_virtual_keys,
          "screen, audio, and exchanged key settings round trip");

    auto invalid = first;
    invalid.skill_virtual_keys = {'Q', 'Q', 'E', 'R'};
    Check(!store.SaveSettings(invalid), "duplicate action keys are rejected");
}

void TestExperimentSpecRoundTrip(const std::filesystem::path &root)
{
    hs::ExperimentSpec source;
    source.scenario_id = "runtime-integration";
    source.seed = 42;
    source.mode = hs::ExperimentMode::SimulationOnly;
    source.build_hash = "0123456789abcdef";
    source.content_hash = "fedcba9876543210";
    source.timeline_actions.push_back({1, 1, "start_session", "{}"});
    source.probes = {"gameplay.checksum", "ecs.count"};
    source.assertions.push_back({"tick", "/tick", "eq", std::uint64_t{60}, 0.0});
    source.termination = {60, true, true, true};

    const auto path = root / "experiment.json";
    Check(hs::SaveExperimentSpec(path, source).Succeeded(), "save experiment spec");
    hs::ExperimentSpec loaded;
    Check(hs::LoadExperimentSpec(path, loaded).Succeeded(), "load experiment spec");
    Check(loaded.scenario_id == source.scenario_id && loaded.seed == 42 &&
              loaded.timeline_actions.size() == 1 && loaded.termination.maximum_tick == 60,
          "experiment spec round trip");
}

void TestNamedPipeCommandRoundTrip()
{
    const auto name = L"test-" + std::to_wstring(GetCurrentProcessId());
    hs::Result server_result = hs::Result::Success();
    std::jthread server([&] {
        server_result = hs::ServeExperimentPipeOnce(
            name, [](std::string_view request, std::string &reply) {
                hs::ExperimentPipeCommand command;
                if (auto parsed = hs::ParseExperimentPipeCommand(request, command); !parsed)
                    return parsed;
                reply = hs::BuildExperimentPipeReply(command.sequence,
                                                     command.target_tick, "accepted", "applied");
                return hs::Result::Success();
            });
    });
    const std::string request =
        R"({"sequence":7,"target_tick":30,"command":"damage_player","payload":{"amount":5}})";
    std::string reply;
    hs::Result transaction = hs::Result::Failure(hs::ErrorCode::InvalidState,
                                                 "test", "not attempted");
    for (unsigned attempt = 0; attempt < 20 && !transaction; ++attempt)
    {
        transaction = hs::TransactExperimentPipe(name, request, reply,
                                                 std::chrono::milliseconds(100));
        if (!transaction) Sleep(5);
    }
    server.join();
    Check(transaction.Succeeded() && server_result.Succeeded(), "named pipe transaction");
    Check(reply.find("\"sequence\":7") != std::string::npos &&
              reply.find("\"status\":\"accepted\"") != std::string::npos,
          "named pipe ack preserves sequence");

    hs::ExperimentPipeCommand invalid;
    Check(!hs::ParseExperimentPipeCommand(R"({"sequence":1})", invalid),
          "named pipe rejects incomplete JSON command");
}

void TestNamedPipeIdempotencyAndTargetTick()
{
    hs::ExperimentPipeCommandQueue queue;
    const std::string accepted =
        R"({"sequence":11,"target_tick":30,"command":"damage_player","payload":{"amount":5}})";
    std::string first;
    std::string duplicate;
    Check(queue.Handle(accepted, 10, first).Succeeded() &&
              queue.Handle(accepted, 20, duplicate).Succeeded() && first == duplicate &&
              queue.AcceptedCount() == 1,
          "duplicate sequence replays response without queueing twice");

    const std::string missed =
        R"({"sequence":12,"target_tick":20,"command":"heal_player","payload":{"amount":5}})";
    std::string rejected;
    Check(queue.Handle(missed, 20, rejected).Succeeded() &&
              rejected.find("missed_target_tick") != std::string::npos &&
              queue.AcceptedCount() == 1,
          "elapsed target tick is rejected");
    const auto commands = queue.TakeAccepted();
    Check(commands.size() == 1 && commands.front().sequence == 11,
          "accepted command executes once");
}

void TestVfxCatalog()
{
    hs::VfxCatalog catalog;
    Check(hs::VfxCatalog::Load("Cooked/particle_effects.hsbin", catalog).Succeeded(),
          "load cooked VFX catalog");
    Check(catalog.SpriteCount() == 39, "load data-driven VFX sprite registry");

    hs::PresentationEvent event;
    event.sequence = 42;
    event.tick = 10;
    event.kind = hs::PresentationKind::Vfx;
    event.asset = hs::MakeAssetId("particle.common.hit");
    event.position = {1.0f, 2.0f, 3.0f};
    hs::VfxEventParameters parameters;
    parameters.direction = {1.0f, 0.0f, 0.0f};
    parameters.scale = 2.0f;
    event.parameters = hs::EncodeVfxParameters(parameters);

    std::vector<hs::ParticleSpawnCommand> full;
    std::vector<hs::ParticleSpawnCommand> half;
    std::vector<hs::EffectLineSpawnCommand> lines;
    Check(catalog.Expand(event, 100, full, lines).Succeeded() && !full.empty(),
          "expand full-quality VFX");
    Check(std::ranges::all_of(full, [](const auto &command) {
              return command.renderer == hs::VfxRenderer::Mesh &&
                     command.primitive == hs::VfxPrimitive::Shard;
          }),
          "hit VFX uses procedural mesh shards instead of sprites");
    Check(catalog.Expand(event, 50, half, lines).Succeeded() && half.size() == full.size(),
          "expand half-quality VFX");
      for (std::size_t index = 0; index < full.size(); ++index)
      {
          Check(half[index].count == (full[index].count + 1) / 2,
                "half-quality emitter count");
          Check(full[index].seed == half[index].seed, "deterministic emitter seed");
      }

      constexpr std::array new_effects{
          "particle.common.player_hit",
          "particle.common.player_death",
          "particle.enemy.melee.windup",
          "particle.enemy.melee.hit",
          "particle.boss.dash.start",
          "particle.boss.dash.impact",
          "particle.boss.volley.release",
          "particle.boss.area.activate",
          "particle.boss.shockwave.release",
          "particle.skill.retreat_shot.move",
          "particle.skill.damage_area.pulse",
          "particle.skill.fire_area.pulse",
          "particle.skill.arrow_rain.impact",
          "particle.skill.arrow_rain.area_pulse",
          "particle.skill.trap.idle",
          "particle.status.slow_apply",
          "particle.status.slow_area",
      };
      for (const auto *effect_id : new_effects)
      {
          event.asset = hs::MakeAssetId(effect_id);
          std::vector<hs::ParticleSpawnCommand> particles;
          std::vector<hs::EffectLineSpawnCommand> effect_lines;
          Check(catalog.Find(event.asset) != nullptr,
                "new VFX exists in cooked catalog");
          Check(catalog.Expand(event, 100, particles, effect_lines).Succeeded() &&
                    !particles.empty(),
                "new VFX expands into particles");
      }

      event.asset = hs::MakeAssetId("particle.line.ricochet");
    Check(!catalog.Expand(event, 100, full, lines).Succeeded(), "line target required");
    parameters.flags = static_cast<std::uint32_t>(hs::VfxEventFlag::HasTarget);
    parameters.target = {4.0f, 0.0f, 5.0f};
    event.parameters = hs::EncodeVfxParameters(parameters);
    Check(catalog.Expand(event, 100, full, lines).Succeeded() && lines.size() == 1 &&
              lines.front().sprite != 0 && lines.front().uv_repeat > 1.0f &&
              lines.front().primitive == hs::VfxPrimitive::DashedRicochet,
          "expand line VFX");

    const auto check_visual = [&](std::string_view id, hs::VfxRenderer renderer,
                                  hs::VfxPrimitive primitive) {
        event.asset = hs::MakeAssetId(id);
        std::vector<hs::ParticleSpawnCommand> particles;
        std::vector<hs::EffectLineSpawnCommand> ignored_lines;
        Check(catalog.Expand(event, 100, particles, ignored_lines).Succeeded() &&
                  std::ranges::any_of(particles, [&](const auto &command) {
                      return command.renderer == renderer && command.primitive == primitive;
                  }),
              "VFX renderer and primitive survive content cook");
    };
    check_visual("particle.status.slow_area", hs::VfxRenderer::Ground,
                 hs::VfxPrimitive::Rune);
    check_visual("particle.skill.explosive_arrow.main", hs::VfxRenderer::Ground,
                 hs::VfxPrimitive::Ring);
    check_visual("particle.skill.retreat_shot.move", hs::VfxRenderer::Segment,
                 hs::VfxPrimitive::DashWake);
    check_visual("particle.status.burn_apply", hs::VfxRenderer::Mesh,
                 hs::VfxPrimitive::Ember);
}

void TestPlaytestRecordAndReplay(const std::filesystem::path &root)
{
    const auto directory = root / "playtest";
    hs::PlaytestRecorder recorder;
    Check(recorder.Start({directory, 42, 99, 101}).Succeeded(), "start playtest recorder");
    const std::array edges{hs::ActionEdge{1, hs::GameAction::SkillQ,
                                         hs::EdgeKind::Pressed}};
    const std::array ui_actions{
        hs::UiAction{hs::UiActionKind::SwapLoadoutSlots, 0, 1}};
    hs::InputFrame input{1, {}, edges, ui_actions};
    hs::SimulationObservation probe;
    probe.tick = 1;
    probe.phase = hs::SessionPhase::Playing;
    probe.balance.skill_uses[1] = 1;
    probe.balance.skill_casts_with_hit[1] = 1;
    probe.skill_levels[1] = 2;
    probe.upgrade_masks[1] = 1;
    probe.balance.upgrade_damage[1][0] = 17;
    probe.balance.upgrade_triggers[1][0] = 2;
    probe.balance.upgrade_effects[1][0][static_cast<std::size_t>(
        hs::UpgradeEffectMetric::ProjectilesCreated)] = 3;
    probe.balance.upgrade_effects[1][0][static_cast<std::size_t>(
        hs::UpgradeEffectMetric::CooldownTicksSaved)] = 12;
    probe.balance.upgrade_relic_synergy_count = 1;
    auto &synergy = probe.balance.upgrade_relic_synergies[0];
    synergy.skill = hs::SkillKind::PiercingShot;
    synergy.upgrade = 0;
    synergy.relic = hs::RelicKind::BurnPropagation;
    synergy.metrics[static_cast<std::size_t>(
        hs::UpgradeRelicSynergyMetric::Damage)] = 23;
    probe.damage_by_skill[1] = 30;
    probe.damage_dealt = 30;
    probe.balance.skill_boss_damage[1] = 17;
    probe.stat_points[static_cast<std::size_t>(hs::StatKind::AttackPower)] = 2;
    probe.balance.stat_utility[
        static_cast<std::size_t>(hs::StatKind::AttackPower)] = 6;
    Check(recorder.Record(input, 1234, probe, {}).Succeeded(), "record playtest tick");

    probe.tick = 2;
    probe.phase = hs::SessionPhase::Defeat;
    probe.health = 0;
    const hs::InputFrame terminal_input{2, {}, {}};
    Check(recorder.Record(terminal_input, 2234, probe, {}).Succeeded(),
          "record terminal playtest tick");
    probe.tick = 3;
    probe.phase = hs::SessionPhase::MainMenu;
    probe.health = 100;
    const hs::InputFrame menu_input{3, {}, {}};
    Check(recorder.Record(menu_input, 3234, probe, {}).Succeeded(),
          "record post-run menu tick");
    Check(recorder.Finish(true).Succeeded(), "finish playtest recorder");
    Check(std::filesystem::is_regular_file(directory / "result.json") &&
              std::filesystem::is_regular_file(directory / "analysis.md"),
          "playtest reports exist");
    std::ifstream result_stream(directory / "result.json");
    const std::string result_json((std::istreambuf_iterator<char>(result_stream)), {});
    Check(result_json.contains("\"build\"") && result_json.contains("\"skills\"") &&
              result_json.contains("\"upgrades\"") &&
              result_json.contains("\"relics\"") &&
              result_json.contains("\"enemies\"") &&
              result_json.contains("\"pickups\"") &&
              result_json.contains("\"stats\"") &&
              result_json.contains("\"damage\": 17") &&
              result_json.contains("\"damage_triggers\": 2") &&
              result_json.contains("\"projectiles_created\"") &&
               result_json.contains("\"cooldown_ticks_saved\"") &&
               result_json.contains("\"value\": 12") &&
               result_json.contains("\"upgrade_relic_synergies\"") &&
               result_json.contains("\"burn_propagation\"") &&
               result_json.contains("\"damage\": 23"),
          "playtest result contains balance metrics");
    Check(result_json.contains("\"outcome\": \"defeat\"") &&
              result_json.contains("\"tick\": 2"),
          "first terminal result survives a later menu transition");
    Check(result_json.contains("\"damage_to_normal\": 13") &&
              result_json.contains("\"damage_to_boss\": 17") &&
              result_json.contains("\"boss\": 17"),
          "skill damage is split by normal and boss targets");
    Check(result_json.contains("\"per_point_utility\": 0.1") &&
              result_json.contains("\"unit\": \"session_contribution_ratio\""),
          "stat utility uses a comparable per-point session contribution ratio");
    std::ifstream analysis_stream(directory / "analysis.md");
    const std::string analysis((std::istreambuf_iterator<char>(analysis_stream)), {});
    Check(analysis.contains("piercing_shot / 강화 1: 17 피해 (2회)"),
          "playtest analysis separates damage caused by each selected upgrade");
    Check(analysis.contains("추가 투사체 3") &&
              analysis.contains("쿨타임 단축 틱 12"),
          "playtest analysis explains each realized non-damage upgrade effect");
    Check(analysis.contains("attack_power: 10.00% / 포인트"),
          "UTF-8 analysis reports normalized stat utility in Korean");
    hs::PlaytestReplay replay;
    Check(hs::LoadPlaytestReplay(directory, replay).Succeeded() &&
              replay.header.format_version == 4 &&
              replay.header.simulation_version == hs::kSimulationVersion &&
              replay.header.gameplay_hash_version == hs::kGameplayHashVersion &&
              replay.header.determinism_profile == hs::kDeterminismProfile &&
              replay.header.seed == 42 && replay.header.simulation_rules_hash == 99 &&
              replay.frames.size() == 3 && replay.frames[0].expected_checksum == 1234 &&
              replay.frames[0].ui_actions.size() == 1 &&
              replay.frames[0].ui_actions[0] == ui_actions[0] &&
              replay.frames[2].expected_checksum == 3234,
          "playtest inputs replay exactly");
}

} // namespace

int main()
{
    const auto root = TestDirectory();
    std::error_code error;
    std::filesystem::remove_all(root, error);
    try
    {
        TestVfxCatalog();
        TestSaveRecovery(root);
        TestSettingsPersistence(root);
        TestExperimentSpecRoundTrip(root);
        TestNamedPipeCommandRoundTrip();
        TestNamedPipeIdempotencyAndTargetTick();
        TestPlaytestRecordAndReplay(root);
        std::filesystem::remove_all(root, error);
        std::cout << "runtime_tests passed\n";
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << "runtime_tests failed: " << exception.what() << '\n';
        return 1;
    }
}
