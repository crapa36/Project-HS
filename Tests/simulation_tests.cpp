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

} // namespace

int main()
{
    try
    {
        TestDeterminism();
        TestPauseDoesNotAdvance();
        TestRulesHashAndAbilityMapping();
        std::cout << "simulation_tests passed\n";
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
