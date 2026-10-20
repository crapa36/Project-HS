#include <hs/core/fixed_step_clock.hpp>
#include <hs/gameplay/game_simulation.hpp>

#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{

void Check(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

hs::GameplayChecksum RunDeterministicSimulation()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({0x12345678u}).Succeeded(), "simulation initialize");
    hs::GameplayChecksum checksum{};
    for (hs::Tick tick = 1; tick <= 240; ++tick)
    {
        hs::InputFrame input{};
        input.target_tick = tick;
        input.held.move_held = true;
        input.held.move_target_world =
            tick <= 120 ? hs::Float3{30.0f, 0.0f, 30.0f}
                        : hs::Float3{-30.0f, 0.0f, 0.0f};
        checksum = simulation.TickFixed(input, hs::FixedStepClock::kFixedStep).checksum;
    }
    return checksum;
}

void TestDeterminism()
{
    // Hash v7 includes the complete movement-echo position history.
    constexpr hs::GameplayChecksum kGameplayOracle = 15816561498904262959ull;
    const auto first = RunDeterministicSimulation();
    Check(first == RunDeterministicSimulation(), "repeated gameplay checksum");
    Check(first == kGameplayOracle,
          std::format("gameplay checksum oracle: {}", first));
}

void TestPauseDoesNotAdvance()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({7}).Succeeded(), "pause simulation initialize");
    std::array edge{hs::ActionEdge{1, hs::GameAction::Pause, hs::EdgeKind::Pressed}};
    hs::InputFrame pause{};
    pause.target_tick = 1;
    pause.ordered_edges = edge;
    const auto paused = simulation.TickFixed(pause, hs::FixedStepClock::kFixedStep);
    Check(paused.phase == hs::SessionPhase::Paused, "pause command enters paused phase");
    hs::InputFrame waiting{};
    waiting.target_tick = 2;
    const auto unchanged = simulation.TickFixed(waiting, hs::FixedStepClock::kFixedStep);
    Check(unchanged.tick == paused.tick && unchanged.checksum == paused.checksum,
          "paused simulation does not advance");
}

void TestOptionalChecksums()
{
    hs::GameSimulation captured, ordinary;
    hs::SimulationConfig config{37};
    Check(captured.Initialize(config).Succeeded(), "captured initialize");
    config.capture_checksums = false;
    Check(ordinary.Initialize(config).Succeeded(), "ordinary initialize");
    Check(ordinary.ComputeChecksum() == captured.ComputeChecksum(), "on-demand initial checksum");
    for (hs::Tick tick = 1; tick <= 240; ++tick)
    {
        if (tick % 40 == 1)
        {
            const hs::DebugCommand spawn{hs::DebugCommandKind::SpawnEnemy, tick % 3, 0, {5, 7}};
            Check(captured.ApplyDebugCommand(spawn).Succeeded() &&
                  ordinary.ApplyDebugCommand(spawn).Succeeded(), "optional checksum debug spawn");
            Check(ordinary.ComputeChecksum() == captured.ComputeChecksum(), "on-demand debug checksum");
        }
        hs::InputFrame input{};
        input.target_tick = tick;
        input.held.move_held = true;
        input.held.move_target_world = {20, 0, tick < 120 ? 10.0f : -10.0f};
        input.held.basic_attack_held = tick % 8 == 0;
        const auto expected = captured.TickFixed(input, hs::FixedStepClock::kFixedStep);
        const auto actual = ordinary.TickFixed(input, hs::FixedStepClock::kFixedStep);
        Check(actual.tick == expected.tick && actual.phase == expected.phase &&
              actual.checksum == 0, "ordinary tick omits checksum only");
        Check(ordinary.ComputeChecksum() == expected.checksum, "capture mode preserves gameplay state");
        hs::GameReadModelStorage model;
        ordinary.WriteReadModel(model);
        Check(model.checksum == 0, "explicit checksum does not enable read-model capture");
    }
    for (const auto action : {hs::UiActionKind::ReturnToMainMenu, hs::UiActionKind::StartSession})
    {
        captured.ApplyUiAction({action});
        ordinary.ApplyUiAction({action});
        Check(ordinary.ComputeChecksum() == captured.ComputeChecksum(), "on-demand UI checksum");
        const auto expected = captured.TickFixed({}, hs::FixedStepClock::kFixedStep);
        const auto actual = ordinary.TickFixed({}, hs::FixedStepClock::kFixedStep);
        Check(actual.checksum == 0 && ordinary.ComputeChecksum() == expected.checksum,
              "optional checksum handles inactive and resumed phases");
    }
}

void TestQueuedResumePreservesQuit()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({7}).Succeeded(), "queued UI simulation initialize");
    const std::array pause{hs::ActionEdge{1, hs::GameAction::Pause, hs::EdgeKind::Pressed}};
    const auto paused = simulation.TickFixed({1, {}, pause}, hs::FixedStepClock::kFixedStep);
    const std::array resume{hs::UiAction{hs::UiActionKind::Resume}};
    const auto playing = simulation.TickFixed({1, {}, {}, resume}, hs::FixedStepClock::kFixedStep);
    Check(playing.phase == hs::SessionPhase::Playing && playing.tick == paused.tick + 1,
          "legitimate UI resume advances one fixed tick");
    const auto paused_again = simulation.TickFixed({2, {}, pause}, hs::FixedStepClock::kFixedStep);
    const std::array queued{hs::UiAction{hs::UiActionKind::Quit},
                            hs::UiAction{hs::UiActionKind::Resume}};
    const auto quit = simulation.TickFixed({2, {}, {}, queued}, hs::FixedStepClock::kFixedStep);
    Check(quit.phase == hs::SessionPhase::QuitRequested && quit.tick == paused_again.tick,
          "queued Resume cannot cancel QuitRequested or advance gameplay");
    simulation.ApplyUiAction({hs::UiActionKind::Resume});
    Check(simulation.GetSessionProbe().phase == hs::SessionPhase::QuitRequested,
          "stale direct Resume preserves terminal quit");
    hs::GameSimulation menu;
    Check(menu.Initialize({9, false, true}).Succeeded(), "menu UI simulation initialize");
    const auto before = menu.ComputeChecksum();
    const auto unchanged = menu.TickFixed({1, {}, {}, resume}, hs::FixedStepClock::kFixedStep);
    Check(unchanged.phase == hs::SessionPhase::MainMenu && unchanged.tick == 0 &&
              unchanged.checksum == before,
          "stale Resume cannot start a session from MainMenu");
}

void TestRulesHashAndAbilityMapping()
{
    const auto rules = hs::SimulationRules::Defaults();
    Check(rules.status_tick_interval == 30 &&
              rules.bleed_duration == 240 &&
              rules.bleed_tick_coefficient == 0.20f &&
              rules.burn_duration == 240 &&
              rules.burn_tick_coefficient == 0.35f,
          "default status rules match the gameplay specification");
    constexpr std::array expected{
        hs::AbilityHandlerId::BasicProjectileCadence,
        hs::AbilityHandlerId::PiercingProjectile,
        hs::AbilityHandlerId::UniformFanProjectiles,
        hs::AbilityHandlerId::HoldReleaseLinearCharge,
        hs::AbilityHandlerId::ProjectileToAreaExplosion,
        hs::AbilityHandlerId::NearestUnhitTargetRicochet,
        hs::AbilityHandlerId::TargetedPeriodicArea,
        hs::AbilityHandlerId::ForwardRollLeaveTrap,
        hs::AbilityHandlerId::ForcedRetreatAndProjectile};
    for (std::size_t index = 0; index < expected.size(); ++index)
        Check(rules.skills[index].handler == expected[index],
              "default ability handler mapping");

    auto changed = rules;
    changed.skills[0].damage_coefficient += 0.25f;
    Check(hs::SimulationRulesHash(rules) == hs::SimulationRulesHash(rules),
          "rules hash stability");
    Check(hs::SimulationRulesHash(rules) != hs::SimulationRulesHash(changed),
          "gameplay rule changes rules hash");
}

void TestSameTickFinalBossVictory()
{
    auto rules = hs::SimulationRules::Defaults();
    for (auto &stage : rules.spawn_stages) stage.per_second = 60.0f;
    for (auto &wave : rules.waves) wave.count = 0;
    rules.waves.front().start_tick = rules.progression.boss_spawn_ticks[2];
    rules.waves.front().duration_ticks = 1;
    rules.waves.front().count = 1;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({91}, rules).Succeeded(),
          "victory priority initialize");
    Check(simulation.ApplyDebugCommand(
              {hs::DebugCommandKind::SetGrowthTick, 15 * 60 * 60 - 1})
              .Succeeded(),
          "advance to final boss tick");
    hs::InputFrame input{};
    input.target_tick = 1;
    (void)simulation.TickFixed(input, hs::FixedStepClock::kFixedStep);
    Check(simulation.GetSessionProbe().final_boss_spawned &&
              simulation.GetObservation().boss_count == 0 &&
              simulation.GetObservation().normal_enemy_count == 0,
          "final boss warning starts without a same-tick normal spawn");
    for (const auto &signal : simulation.PendingDomainSignals())
        Check(signal.kind != hs::DomainSignalKind::EnemySpawnWarning,
              "final boss tick emits no canceled normal-spawn warning");
    for (hs::Tick tick = 2; tick <= 91; ++tick)
    {
        input.target_tick = tick;
        (void)simulation.TickFixed(input, hs::FixedStepClock::kFixedStep);
    }
    Check(simulation.GetObservation().boss_count == 1,
          "final boss spawns after the warning window");
    Check(simulation.ApplyDebugCommand(
              {hs::DebugCommandKind::DamageFinalBoss, 1'000'000})
              .Succeeded(),
          "kill final boss");
    Check(simulation.ApplyDebugCommand(
              {hs::DebugCommandKind::DamagePlayer, 1'000'000})
              .Succeeded(),
          "kill player");
    input.target_tick = 92;
    const auto result = simulation.TickFixed(input, hs::FixedStepClock::kFixedStep);
    Check(result.phase == hs::SessionPhase::Victory,
          "same-tick final boss and player death resolves victory");
}

hs::SimulationRules QuietRules()
{
    auto rules = hs::SimulationRules::Defaults();
    for (auto &stage : rules.spawn_stages) stage.per_second = 0;
    for (auto &wave : rules.waves) wave.count = 0;
    rules.arena_obstacle_count = 0;
    rules.growth.utility_pickup_base_chance = 0;
    rules.growth.utility_pickup_miss_increment = 0;
    rules.relic_drop.healing_pickup_probability = 0;
    rules.relic_drop.normal_enemy_base_probability = 0;
    rules.relic_drop.normal_enemy_probability_increment_per_kill = 0;
    return rules;
}

hs::TickResult Advance(hs::GameSimulation &simulation,
                       const hs::HeldInputState &held = {},
                       std::span<const hs::ActionEdge> edges = {})
{
    hs::InputFrame input{};
    input.target_tick = simulation.GetSessionProbe().tick + 1;
    input.held = held;
    input.ordered_edges = edges;
    return simulation.TickFixed(input, hs::FixedStepClock::kFixedStep);
}

void Debug(hs::GameSimulation &simulation, hs::DebugCommandKind kind,
           std::uint64_t value = 0, hs::Float2 position = {})
{
    Check(simulation.ApplyDebugCommand({kind, value, 0, position}).Succeeded(),
          "regression debug command");
}

hs::SimulationRules ChestRules()
{
    auto rules = QuietRules();
    rules.enemies[0].health = 1;
    rules.enemies[0].move_speed = 0;
    rules.relic_drop.normal_enemy_base_probability = 1;
    rules.relic_drop.normal_enemy_probability_cap = 1;
    rules.progression.required_xp_base = 100'000;
    rules.character_initial.starting_active_skill_count = 1;
    rules.character_initial.starting_active_skill_ids[0] = hs::SkillKind::ArrowRain;
    rules.skills[static_cast<std::size_t>(hs::SkillKind::ArrowRain)].activation_delay_ticks = 0;
    return rules;
}

void SpawnChestEnemies(hs::GameSimulation &simulation)
{
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, {1, 0});
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, {1, 1});
}

void CastChestRain(hs::GameSimulation &simulation)
{
    hs::HeldInputState held{};
    held.aim_world = {1, 0, 0};
    const std::array edges{hs::ActionEdge{1, hs::GameAction::SkillQ, hs::EdgeKind::Pressed}};
    (void)Advance(simulation, held, edges);
}

void TestTerminalPhaseKeepsPickups()
{
    for (const auto terminal : {hs::SessionPhase::Victory, hs::SessionPhase::Defeat})
    {
        hs::SimulationConfig config{141};
        config.scenario.auto_collect_progression = true;
        hs::GameSimulation simulation;
        Check(simulation.Initialize(config, ChestRules()).Succeeded(), "terminal pickups initialize");
        SpawnChestEnemies(simulation);
        if (terminal == hs::SessionPhase::Victory)
        {
            Debug(simulation, hs::DebugCommandKind::SpawnBoss,
                  static_cast<std::uint64_t>(hs::BossKind::Final));
            Debug(simulation, hs::DebugCommandKind::DamageFinalBoss, 1'000'000);
        }
        else Debug(simulation, hs::DebugCommandKind::DamagePlayer, 1'000'000);
        CastChestRain(simulation);
        const auto observation = simulation.GetObservation();
        const auto chest = static_cast<std::size_t>(hs::PickupKind::RelicChest);
        Check(observation.balance.pickup_drops[chest] == 2,
              "terminal tick produces two collectable chests");
        Check(observation.phase == terminal && observation.card_count == 0 &&
                  observation.experience == 0 && observation.balance.pickup_collected[chest] == 0,
              "terminal phase blocks XP and relic collection");
        Check(Advance(simulation).phase == terminal,
              "pending rewards cannot reopen a terminal session");
    }
}

void TestMultipleChestRewards()
{
    for (const bool automatic : {false, true})
    {
        hs::SimulationConfig config{142, automatic};
        config.scenario.auto_collect_progression = true;
        hs::GameSimulation simulation;
        Check(simulation.Initialize(config, ChestRules()).Succeeded(), "multiple chest initialize");
        SpawnChestEnemies(simulation);
        CastChestRain(simulation);
        const auto chest = static_cast<std::size_t>(hs::PickupKind::RelicChest);
        if (!automatic)
        {
            Check(simulation.GetObservation().phase == hs::SessionPhase::RelicSelection &&
                      simulation.GetObservation().balance.pickup_collected[chest] == 1,
                  "manual selection consumes one chest and preserves the next");
            Debug(simulation, hs::DebugCommandKind::SelectCard);
            (void)Advance(simulation);
            Check(simulation.GetObservation().phase == hs::SessionPhase::RelicSelection,
                  "remaining chest opens its own selection after the first choice");
            Debug(simulation, hs::DebugCommandKind::SelectCard);
        }
        const auto observation = simulation.GetObservation();
        Check(observation.phase == hs::SessionPhase::Playing &&
                  observation.balance.pickup_collected[chest] == 2 &&
                  std::popcount(observation.relic_mask) == 2,
              "both manual and automatic collection award two distinct relics");
    }
}

void CheckPosition(hs::Float2 actual, hs::Float2 expected, std::string_view message)
{
    Check(std::abs(actual.x - expected.x) < 0.00001f &&
              std::abs(actual.y - expected.y) < 0.00001f, message);
}

hs::GameplayChecksum RunMovementEchoHistory(hs::Tick age, bool forced_move)
{
    auto rules = QuietRules();
    rules.relics.movement_echo.position_history_age_ticks = age;
    rules.relics.movement_echo.required_distance = 0;
    rules.character_initial.starting_active_skill_count = 1;
    rules.character_initial.starting_active_skill_ids[0] = hs::SkillKind::RetreatShot;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({143}, rules).Succeeded(), "echo history initialize");
    std::array<hs::Float2, 73> positions{};
    hs::HeldInputState held{};
    held.move_held = true;
    held.move_target_world = {30, 0, 0};
    held.aim_world = {30, 0, 0};
    const std::array retreat{hs::ActionEdge{1, hs::GameAction::SkillQ, hs::EdgeKind::Pressed}};
    for (hs::Tick tick = 1; tick <= 72; ++tick)
    {
        (void)Advance(simulation, held, forced_move && tick == 5
                          ? std::span<const hs::ActionEdge>(retreat) : std::span<const hs::ActionEdge>{});
        positions[tick] = simulation.GetSessionProbe().player_position;
    }
    // Acquiring the relic after movement must retain that movement's history.
    Debug(simulation, hs::DebugCommandKind::GrantRelic,
          static_cast<std::uint64_t>(hs::RelicKind::MovementEcho));
    held.move_held = false;
    held.basic_attack_held = true;
    simulation.ClearDomainSignals();
    (void)Advance(simulation, held); // Cast tick 73, before its Movement phase.
    const auto sample_tick = age == 0 ? 72 : age < 73 ? 73 - age : 0;
    const auto expected = positions[sample_tick];
    const auto signals = simulation.PendingDomainSignals();
    const auto echo = std::ranges::find(signals, hs::DomainSignalKind::AfterimageArrowFired,
                                       &hs::DomainSignal::kind);
    Check(echo != signals.end(), "echo activation emits its source position");
    CheckPosition({echo->position.x, echo->position.z}, expected,
                  "echo source uses attack tick minus authored age, including forced movement");
    bool released{};
    for (int step = 0; step < 30 && !released; ++step)
    {
        (void)Advance(simulation);
        hs::GameReadModelStorage model;
        simulation.WriteReadModel(model);
        for (const auto &projectile : model.View().projectiles)
            if (projectile.source_relic == static_cast<std::uint8_t>(hs::RelicKind::MovementEcho))
            {
                CheckPosition(projectile.previous_position, expected,
                              "delayed echo projectile retains the historical source");
                released = true;
            }
    }
    Check(released, "historical echo projectile releases");
    const auto checksum = simulation.ComputeChecksum();
    Debug(simulation, hs::DebugCommandKind::StartSession);
    Debug(simulation, hs::DebugCommandKind::GrantRelic,
          static_cast<std::uint64_t>(hs::RelicKind::MovementEcho));
    simulation.ClearDomainSignals();
    (void)Advance(simulation, held);
    bool restarted_echo{};
    for (const auto &signal : simulation.PendingDomainSignals())
        if (signal.kind == hs::DomainSignalKind::AfterimageArrowFired)
        {
            CheckPosition({signal.position.x, signal.position.z}, {},
                          "new session warmup uses its own start position");
            restarted_echo = true;
        }
    Check(restarted_echo, "new session emits a warmup echo");
    if (age == 0)
    {
        simulation.ApplyUiAction({hs::UiActionKind::ReturnToMainMenu});
        rules.relics.movement_echo.position_history_age_ticks = 5;
        Check(simulation.ApplySimulationRules(rules).Succeeded(), "echo history age reload");
        simulation.ApplyUiAction({hs::UiActionKind::StartSession});
        Debug(simulation, hs::DebugCommandKind::GrantRelic,
              static_cast<std::uint64_t>(hs::RelicKind::MovementEcho));
        held.basic_attack_held = false;
        held.move_held = true;
        for (std::size_t tick = 1; tick <= 10; ++tick)
        {
            (void)Advance(simulation, held);
            positions[tick] = simulation.GetSessionProbe().player_position;
        }
        held.move_held = false;
        held.basic_attack_held = true;
        simulation.ClearDomainSignals();
        (void)Advance(simulation, held);
        const auto reloaded_signals = simulation.PendingDomainSignals();
        const auto reloaded_echo = std::ranges::find(
            reloaded_signals, hs::DomainSignalKind::AfterimageArrowFired, &hs::DomainSignal::kind);
        Check(reloaded_echo != reloaded_signals.end(), "reloaded history emits an echo");
        CheckPosition({reloaded_echo->position.x, reloaded_echo->position.z}, positions[6],
                      "reloaded age resizes and resets the new session history");
    }
    return checksum;
}

void TestMovementEchoHistory()
{
    for (const hs::Tick age : {hs::Tick{0}, hs::Tick{5}, hs::Tick{60}, hs::Tick{120}})
        Check(RunMovementEchoHistory(age, false) == RunMovementEchoHistory(age, false),
              "historical echo remains deterministic");
    (void)RunMovementEchoHistory(60, true);
}

void TestNavigationRulesReload()
{
    auto old_rules = QuietRules();
    old_rules.arena_obstacle_count = 2;
    old_rules.arena_obstacles[0] = {hs::ArenaObstacleKind::Rock, {7, 0}, 2};
    const auto edge = old_rules.arena_half_extent - 1;
    old_rules.arena_obstacles[1] = {hs::ArenaObstacleKind::Rock, {-edge / 2, -edge / 2}, 1};
    old_rules.boss_common.initial_pattern_delay_ticks = 0;
    old_rules.bosses[static_cast<std::size_t>(hs::BossKind::TenMinute)].preferred_distance_far = 0;
    hs::GameSimulation reloaded;
    Check(reloaded.Initialize({144}, old_rules).Succeeded(), "navigation reload initialize");
    const auto spawn = [](hs::GameSimulation &simulation) {
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, {15, 0});
        Debug(simulation, hs::DebugCommandKind::SpawnBoss,
              static_cast<std::uint64_t>(hs::BossKind::TenMinute));
    };
    hs::HeldInputState held{};
    held.move_held = true;
    held.move_target_world = {15, 0, 0};
    spawn(reloaded);
    (void)Advance(reloaded, held); // Prime both cached target cells with the old grids.
    reloaded.ApplyUiAction({hs::UiActionKind::ReturnToMainMenu});
    auto new_rules = old_rules;
    new_rules.arena_obstacles[0].radius = 3;
    new_rules.arena_obstacles[1].radius = 4;
    new_rules.stats.player_collision_radius += 0.25f;
    new_rules.enemies[0].collision_radius += 0.25f;
    new_rules.boss_common.collision_radius += 0.25f;
    Check(reloaded.ApplySimulationRules(new_rules).Succeeded(), "navigation rules reload");
    reloaded.ApplyUiAction({hs::UiActionKind::StartSession});
    hs::GameSimulation fresh;
    Check(fresh.Initialize({144}, new_rules).Succeeded(), "fresh navigation initialize");
    spawn(reloaded);
    spawn(fresh);
    for (int step = 0; step < 120; ++step)
    {
        (void)Advance(reloaded, held);
        (void)Advance(fresh, held);
        CheckPosition(reloaded.GetSessionProbe().player_position, fresh.GetSessionProbe().player_position,
                      "reloaded player navigation matches a fresh rules initialization");
        hs::GameReadModelStorage actual, expected;
        reloaded.WriteReadModel(actual);
        fresh.WriteReadModel(expected);
        Check(actual.View().enemies.size() == 2 && expected.View().enemies.size() == 2,
              "navigation reload keeps both normal enemy and boss alive");
        for (std::size_t index = 0; index < 2; ++index)
            CheckPosition(actual.View().enemies[index].position, expected.View().enemies[index].position,
                          "reloaded normal and boss navigation match fresh grids");
    }
}

} // namespace

int main()
{
    try
    {
        TestPauseDoesNotAdvance();
        TestOptionalChecksums();
        TestQueuedResumePreservesQuit();
        TestRulesHashAndAbilityMapping();
        TestSameTickFinalBossVictory();
        TestTerminalPhaseKeepsPickups();
        TestMultipleChestRewards();
        TestMovementEchoHistory();
        TestNavigationRulesReload();
        TestDeterminism();
        std::cout << "simulation_tests passed\n";
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
