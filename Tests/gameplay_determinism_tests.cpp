#include "gameplay_test_support.hpp"

#include <array>
#include <cstdint>
#include <format>
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
    Check(paused.phase == hs::SessionPhase::Paused && paused.tick == before_pause.tick &&
              paused.world_changed,
          "pause edge stops current tick");
    const std::array no_op_reroll{hs::UiAction{hs::UiActionKind::Reroll}};
    hs::HeldInputState steady_aim;
    steady_aim.aim_world = {before_pause.player_position.x, 0.0f,
                            before_pause.player_position.y};
    const auto no_op = simulation.TickFixed(
        {before_pause.tick, steady_aim, {}, no_op_reroll});
    Check(!no_op.world_changed && no_op.tick == before_pause.tick,
          "a rejected paused UI action does not report a world change");
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
    Check(resumed.phase == hs::SessionPhase::Playing && resumed.world_changed &&
              resumed.tick == before_pause.tick + 1,
          "resume advances exactly one tick");
    Check(simulation.Shutdown().Succeeded(), "pause shutdown");
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



void RunGameplayDeterminismTests()
{
    {
        hs::GameSimulation individual, batched;
        const auto rules = QuietGameData();
        Check(individual.Initialize({81}, rules).Succeeded() &&
                  batched.Initialize({81}, rules).Succeeded(), "UI batch initialize");
        hs::InputFrame input;
        const std::array actions{
            hs::UiAction{hs::UiActionKind::ReturnToMainMenu},
            hs::UiAction{hs::UiActionKind::StartSession},
            hs::UiAction{hs::UiActionKind::SwapLoadoutSlots, 0, 1},
            hs::UiAction{hs::UiActionKind::Quit}};
        for (const auto &action : actions)
        {
            individual.ApplyUiAction(action);
        }
        input.ui_actions = actions;
        const auto expected = Tick(individual);
        const auto actual = batched.TickFixed(input);
        Check(actual.checksum == expected.checksum && actual.phase == expected.phase &&
                  actual.tick == expected.tick, "batched UI preserves ordered final observation");
        Check(individual.Shutdown().Succeeded() && batched.Shutdown().Succeeded(),
              "UI batch shutdown");
    }
    TestPauseStopsTicks();
    TestSingleWorkerOracleDeterminism();
}

} // namespace gameplay_test
