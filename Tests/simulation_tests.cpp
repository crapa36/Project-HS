#include <hs/core/fixed_step_clock.hpp>
#include <hs/gameplay/game_simulation.hpp>

#include <array>
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
    constexpr hs::GameplayChecksum kGameplayOracle = 16791108035892396461ull;
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
    for (auto &stage : rules.spawn_stages) stage.per_second = 0.0f;
    for (auto &wave : rules.waves) wave.count = 0;
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
    Check(simulation.GetSessionView().final_boss_spawned,
          "final boss spawned");
    Check(simulation.ApplyDebugCommand(
              {hs::DebugCommandKind::DamageFinalBoss, 1'000'000})
              .Succeeded(),
          "kill final boss");
    Check(simulation.ApplyDebugCommand(
              {hs::DebugCommandKind::DamagePlayer, 1'000'000})
              .Succeeded(),
          "kill player");
    input.target_tick = 2;
    const auto result = simulation.TickFixed(input, hs::FixedStepClock::kFixedStep);
    Check(result.phase == hs::SessionPhase::Victory,
          "same-tick final boss and player death resolves victory");
}

} // namespace

int main()
{
    try
    {
        TestDeterminism();
        TestPauseDoesNotAdvance();
        TestRulesHashAndAbilityMapping();
        TestSameTickFinalBossVictory();
        std::cout << "simulation_tests passed\n";
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
