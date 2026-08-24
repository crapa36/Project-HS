#include "gameplay_test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gameplay_test
{

namespace
{

std::vector<hs::GameplayChecksum> RunDeterministicOracle()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({0x12345678u, true}, hs::SimulationRules::Defaults()).Succeeded(),
          "determinism initialize");

    std::vector<hs::GameplayChecksum> checksums;
    checksums.reserve(240);
    for (hs::Tick tick = 1; tick <= 240; ++tick)
    {
        hs::HeldInputState held;
        held.move_held = true;
        held.move_target_world = tick <= 120 ? hs::Float3{30.0f, 0.0f, 20.0f}
                                             : hs::Float3{-20.0f, 0.0f, -30.0f};
        held.aim_world = {30.0f, 0.0f, 0.0f};
        held.basic_attack_held = true;
        checksums.push_back(Tick(simulation, held).checksum);
        simulation.ClearDomainSignals();
    }
    Check(simulation.Shutdown().Succeeded(), "determinism shutdown");
    return checksums;
}

} // namespace

void TestPauseStopsTicks()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({12}, QuietGameData()).Succeeded(), "pause initialize");
    (void)Tick(simulation);
    const auto before_pause = simulation.GetObservation();

    hs::Sequence sequence{};
    const auto paused = TickEdge(simulation, hs::GameAction::Pause,
                                 hs::EdgeKind::Pressed, sequence);
    Check(paused.phase == hs::SessionPhase::Paused && paused.tick == before_pause.tick,
          "pause edge stops current tick");
    for (std::uint32_t attempt = 0; attempt < 3; ++attempt)
    {
        (void)Tick(simulation);
    }
    const auto still_paused = simulation.GetObservation();
    Check(still_paused.tick == before_pause.tick &&
              still_paused.growth_ticks == before_pause.growth_ticks,
          "paused simulation does not advance");

    const auto resumed = TickEdge(simulation, hs::GameAction::Pause,
                                  hs::EdgeKind::Pressed, sequence);
    Check(resumed.phase == hs::SessionPhase::Playing &&
              resumed.tick == before_pause.tick + 1,
          "resume advances exactly one tick");
    Check(simulation.Shutdown().Succeeded(), "pause shutdown");
}

void TestStationaryCombatSimulationContract()
{
    auto data = QuietGameData();
    data.spawn_stages.front().per_second = 10.0f;
    data.spawn_stages.front().weights = {50, 50, 0};
    hs::SimulationConfig config{123};
    config.scenario = {.player_stationary = true, .player_invulnerable = true,
                       .progression_enabled = false};
    hs::GameSimulation attacking;
    hs::GameSimulation idle;
    Check(attacking.Initialize(config, data).Succeeded() &&
              idle.Initialize(config, data).Succeeded(),
          "stationary combat initialize");
    Debug(attacking, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::Trap));
    Debug(attacking, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::RetreatShot));
    Debug(attacking, hs::DebugCommandKind::SetStat,
          static_cast<std::uint64_t>(hs::StatKind::AttackPower), 10);
    Debug(attacking, hs::DebugCommandKind::GrantExperience, 1'000);
    Debug(attacking, hs::DebugCommandKind::SpawnEnemy,
          static_cast<std::uint64_t>(hs::EnemyKind::Ranged), 0, {10.0f, 0.0f});
    Debug(idle, hs::DebugCommandKind::SpawnEnemy,
          static_cast<std::uint64_t>(hs::EnemyKind::Ranged), 0, {10.0f, 0.0f});

    hs::HeldInputState held;
    held.basic_attack_held = true;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    hs::Sequence sequence{};
    (void)TickEdge(attacking, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    (void)Tick(idle);
    for (std::uint32_t tick = 1; tick < 600; ++tick)
    {
        if (tick == 30)
            (void)TickEdge(attacking, hs::GameAction::SkillW,
                           hs::EdgeKind::Pressed, sequence, held);
        else
            (void)Tick(attacking, held);
        (void)Tick(idle);
    }
    const auto active = attacking.GetObservation();
    const auto passive = idle.GetObservation();
    Check(active.player_position.x == 0.0f && active.player_position.y == 0.0f &&
              active.health == active.max_health && active.damage_taken > 0,
          "combat simulation keeps the player stationary and invulnerable while recording threat");
    Check(active.level == 1 && active.stat_points[
              static_cast<std::size_t>(hs::StatKind::AttackPower)] == 10,
          "combat simulation freezes progression and applies the requested build stats");
    Check(active.balance.enemy_spawned == passive.balance.enemy_spawned,
          "build-created entities do not change the paired enemy spawn stream");
    Check(attacking.Shutdown().Succeeded() && idle.Shutdown().Succeeded(),
          "stationary combat shutdown");
}

void TestStationaryProgressionSimulationContract()
{
    auto data = QuietGameData();
    data.enemies[static_cast<std::size_t>(hs::EnemyKind::Melee)].health = 1;
    hs::SimulationConfig config{124};
    config.automatic_choices = true;
    config.scenario = {.player_stationary = true, .player_invulnerable = true,
                       .progression_enabled = true,
                       .auto_collect_progression = true};
    hs::GameSimulation simulation;
    Check(simulation.Initialize(config, data).Succeeded(),
          "stationary progression initialize");
    for (std::uint32_t index = 0; index < 20; ++index)
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy,
              static_cast<std::uint64_t>(hs::EnemyKind::Melee), 0,
              {2.0f, static_cast<float>(index) * 0.01f});
    hs::HeldInputState held;
    held.basic_attack_held = true;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    for (std::uint32_t tick = 0;
         tick < 1'800 &&
         simulation.GetObservation().balance.pickup_collected[
             static_cast<std::size_t>(hs::PickupKind::Experience)] < 20;
         ++tick)
        (void)Tick(simulation, held);
    const auto probe = simulation.GetObservation();
    Check(probe.level >= 2 &&
              probe.balance.pickup_collected[
                  static_cast<std::size_t>(hs::PickupKind::Experience)] >= 20,
          "stationary progression instantly collects experience and levels normally");
    Check(probe.player_position.x == 0.0f && probe.player_position.y == 0.0f &&
              probe.health == probe.max_health,
          "stationary progression remains fixed and invulnerable");
    Check(simulation.Shutdown().Succeeded(), "stationary progression shutdown");
}

void TestSingleWorkerOracleDeterminism()
{
    const auto first = RunDeterministicOracle();
    const auto second = RunDeterministicOracle();
    for (std::size_t index = 0; index < first.size(); ++index)
    {
        Check(first[index] == second[index],
              std::format("single-worker oracle differs at tick {}: {} != {}",
                          index + 1, first[index], second[index]));
    }
    Check(!first.empty() && first.back() != 0, "determinism checksum produced");
}

void TestExperimentDebugCommands()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({71, true, true}, QuietGameData()).Succeeded(),
          "experiment commands initialize");
    Check(simulation.GetObservation().phase == hs::SessionPhase::MainMenu,
          "experiment starts at main menu");
    Debug(simulation, hs::DebugCommandKind::StartSession);
    Check(simulation.GetObservation().phase == hs::SessionPhase::Playing,
          "start_session command");
    Debug(simulation, hs::DebugCommandKind::DamagePlayer, 50);
    Debug(simulation, hs::DebugCommandKind::HealPlayer, 20);
    Check(simulation.GetObservation().health == 70, "heal_player command clamps and heals");
    Debug(simulation, hs::DebugCommandKind::SpawnBoss,
          static_cast<std::uint64_t>(hs::BossKind::FiveMinute));
    Check(simulation.GetObservation().boss_count == 1, "spawn_boss command");
    Check(simulation.Shutdown().Succeeded(), "experiment commands shutdown");
}

void TestSemanticReadModel()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({81}, QuietGameData()).Succeeded(),
          "read model initialize");
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 1, 0, {3.0f, 4.0f});
    (void)Tick(simulation);
    hs::GameReadModelStorage storage;
    simulation.WriteReadModel(storage);
    const auto model = storage.View();
    Check(model.tick == simulation.GetObservation().tick, "read model tick");
    Check(model.checksum == simulation.ComputeChecksum(), "read model checksum");
    Check(model.session.player_position.x == model.player.position.x &&
              model.session.player_position.y == model.player.position.y,
          "read model player semantics");
    Check(model.enemies.size() == 1 &&
              model.enemies.front().kind == hs::EnemyKind::Ranged &&
              !model.enemies.front().boss.has_value(),
          "read model enemy semantics");
    Check(simulation.Shutdown().Succeeded(), "read model shutdown");
}

void RunGameplayDeterminismTests()
{
    TestPauseStopsTicks();
    TestStationaryCombatSimulationContract();
    TestStationaryProgressionSimulationContract();
    TestSingleWorkerOracleDeterminism();
    TestExperimentDebugCommands();
    TestSemanticReadModel();
}

} // namespace gameplay_test
