#include <hs/gameplay/game_simulation.hpp>
#include <hs/gameplay/game_simulation_development.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <ranges>

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
        checksum = simulation.TickFixed(input).checksum;
    }
    return checksum;
}

void TestDeterminism()
{
    // Hash v9 adds persistent enemy facing; attack, movement and RNG order are unchanged.
    constexpr hs::GameplayChecksum kGameplayOracle = 17582488999634637405ull;
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
    const auto paused = simulation.TickFixed(pause);
    Check(paused.phase == hs::SessionPhase::Paused, "pause command enters paused phase");
    hs::InputFrame waiting{};
    waiting.target_tick = 2;
    const auto unchanged = simulation.TickFixed(waiting);
    Check(unchanged.tick == paused.tick && unchanged.checksum == paused.checksum,
          "paused simulation does not advance");
    const std::array reroll{hs::UiAction{hs::UiActionKind::Reroll}};
    waiting.ui_actions = reroll;
    const auto player = simulation.GetObservation().player_position;
    waiting.held.aim_world = {player.x, 0.0f, player.y};
    const auto rejected = simulation.TickFixed(waiting);
    Check(!rejected.world_changed && rejected.tick == paused.tick,
          "rejected paused UI action leaves the simulation unchanged");
}

void TestScopedTickReadView()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({317}).Succeeded(), "borrow simulation initialize");
    Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::SpawnEnemy, 0, 0, {5, 7}}).Succeeded(),
          "borrow live enemy fixture");
    const auto checksum = simulation.ComputeChecksum();
    {
        auto first = simulation.BorrowTickReadView();
        auto nested = simulation.BorrowTickReadView();
        auto moved = std::move(first);
        Check(moved.View().enemies.size() == 1 &&
                  moved.View().enemies.begin()->position.x == 5 &&
                  &moved.View().session == &nested.View().session,
              "nested and moved scopes borrow the same current owner");
        Check(moved.View().checksum == checksum && simulation.ComputeChecksum() == checksum,
              "borrowed state retains the current checksum");
    }
    Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::TogglePause}).Succeeded(), "pause before same-tick mutation");
    const auto before = simulation.GetSessionProbe();
    Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::DamagePlayer, 3}).Succeeded(), "same-tick debug mutation");
    {
        auto read = simulation.BorrowTickReadView();
        Check(read.View().tick == before.tick && read.View().session.health == before.health - 3 &&
                  read.View().player.health == before.health - 3,
              "small facts refresh after same-tick paused mutations");
    }
    const auto failed = simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantSkill, 999});
    Check(!failed.Succeeded() && simulation.GetSessionProbe().health == before.health - 3,
          "failed mutation keeps observation fresh");
    simulation.ApplyUiAction({hs::UiActionKind::ReturnToMainMenu});
    auto changed = simulation.Rules();
    changed.skills[0].range += 1.0f;
    changed.waves[0].start_tick += 1;
    Check(simulation.ApplySimulationRules(changed).Succeeded(), "fixture rules mutation");
    {
        auto read = simulation.BorrowTickReadView();
        Check(read.View().skills[0].effective_range == changed.skills[0].range &&
                  read.View().waves[0].start == changed.waves[0].start_tick &&
                  read.View().arena_obstacles.data() == simulation.Rules().arena_obstacles.data(),
              "immutable facts initialize on rules changes while arena stays borrowed");
    }
    simulation.ApplyUiAction({hs::UiActionKind::StartSession});
    Check(simulation.GetSessionProbe().health == simulation.GetSessionProbe().max_health,
          "session reset invalidates same-tick facts");
    Check(simulation.Shutdown().Succeeded(), "borrow scopes fully released before shutdown");
}

void TestQueuedResumePreservesQuit()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({7}).Succeeded(), "queued UI simulation initialize");
    const std::array pause{hs::ActionEdge{1, hs::GameAction::Pause, hs::EdgeKind::Pressed}};
    const auto paused = simulation.TickFixed({1, {}, pause});
    const std::array resume{hs::UiAction{hs::UiActionKind::Resume}};
    const auto playing = simulation.TickFixed({1, {}, {}, resume});
    Check(playing.phase == hs::SessionPhase::Playing && playing.tick == paused.tick + 1,
          "legitimate UI resume advances one fixed tick");
    const auto paused_again = simulation.TickFixed({2, {}, pause});
    const std::array queued{hs::UiAction{hs::UiActionKind::Quit},
                            hs::UiAction{hs::UiActionKind::Resume}};
    const auto quit = simulation.TickFixed({2, {}, {}, queued});
    Check(quit.phase == hs::SessionPhase::QuitRequested && quit.tick == paused_again.tick,
          "queued Resume cannot cancel QuitRequested or advance gameplay");
    simulation.ApplyUiAction({hs::UiActionKind::Resume});
    Check(simulation.GetSessionProbe().phase == hs::SessionPhase::QuitRequested,
          "stale direct Resume preserves terminal quit");
    hs::GameSimulation menu;
    Check(menu.Initialize({9, false, true}).Succeeded(), "menu UI simulation initialize");
    const auto before = menu.ComputeChecksum();
    menu.ApplyUiAction({hs::UiActionKind::Resume});
    Check(menu.ComputeChecksum() == before,
          "stale direct Resume leaves the MainMenu checksum unchanged");
    const auto unchanged = menu.TickFixed({1, {}, {}, resume});
    Check(unchanged.phase == hs::SessionPhase::MainMenu && unchanged.tick == 0 &&
              unchanged.checksum == before,
          "stale Resume cannot start a session from MainMenu");
}

void TestBatchedUiOrder()
{
    hs::GameSimulation individual, batched;
    Check(individual.Initialize({81}).Succeeded() && batched.Initialize({81}).Succeeded(),
          "UI batch initialize");
    const std::array actions{
        hs::UiAction{hs::UiActionKind::ReturnToMainMenu},
        hs::UiAction{hs::UiActionKind::StartSession},
        hs::UiAction{hs::UiActionKind::SwapLoadoutSlots, 0, 1},
        hs::UiAction{hs::UiActionKind::Quit}};
    for (const auto &action : actions) individual.ApplyUiAction(action);
    const auto expected = individual.TickFixed({1});
    const auto actual = batched.TickFixed({1, {}, {}, actions});
    Check(actual.checksum == expected.checksum && actual.phase == expected.phase &&
              actual.tick == expected.tick, "batched UI preserves ordered final observation");
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
    (void)simulation.TickFixed(input);
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
        (void)simulation.TickFixed(input);
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
    const auto result = simulation.TickFixed(input);
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
    return simulation.TickFixed(input);
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
        hs::CopyReadModelForDevelopment(simulation, model);
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
    return checksum;
}

void TestMovementEchoHistory()
{
    for (const hs::Tick age : {hs::Tick{0}, hs::Tick{5}, hs::Tick{60}, hs::Tick{120}})
        Check(RunMovementEchoHistory(age, false) == RunMovementEchoHistory(age, false),
              "historical echo remains deterministic");
    (void)RunMovementEchoHistory(60, true);
}

} // namespace

int main()
{
    try
    {
        TestPauseDoesNotAdvance();
        TestScopedTickReadView();
        TestQueuedResumePreservesQuit();
        TestBatchedUiOrder();
        TestSameTickFinalBossVictory();
        TestTerminalPhaseKeepsPickups();
        TestMultipleChestRewards();
        TestMovementEchoHistory();
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
