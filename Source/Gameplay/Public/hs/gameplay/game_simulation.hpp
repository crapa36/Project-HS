#pragma once

#include <hs/core/input.hpp>
#include <hs/core/result.hpp>
#include <hs/game_rules/simulation_rules.hpp>
#include <hs/game_domain/game_types.hpp>
#include <hs/game_domain/domain_signal.hpp>
#include <hs/game_domain/game_read_model.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>

namespace hs
{

struct SimulationScenarioPolicy
{
    bool player_stationary{};
    bool player_invulnerable{};
    bool progression_enabled{true};
    bool auto_collect_progression{};
};

struct SimulationConfig
{
    std::uint64_t seed{1};
    bool automatic_choices{};
    bool start_in_main_menu{};
    SimulationScenarioPolicy scenario{};
};

struct TickResult
{
    Tick tick{};
    GameplayChecksum checksum{};
    SessionPhase phase{SessionPhase::MainMenu};
};

class GameSimulation
{
  public:
    GameSimulation();
    ~GameSimulation();

    GameSimulation(const GameSimulation &) = delete;
    GameSimulation &operator=(const GameSimulation &) = delete;

    [[nodiscard]] Result Initialize(const SimulationConfig &config);
    [[nodiscard]] Result Initialize(const SimulationConfig &config,
                                    const SimulationRules &rules);
    [[nodiscard]] TickResult TickFixed(const InputFrame &input,
                                       std::chrono::nanoseconds fixed_delta);
    [[nodiscard]] GameplayChecksum ComputeChecksum() const;
    [[nodiscard]] const SimulationRules &Rules() const noexcept;
    [[nodiscard]] SessionProbe GetSessionView() const noexcept;
    [[nodiscard]] SimulationDiagnostics GetDiagnostics() const noexcept;
    [[nodiscard]] SimulationObservation Probe() const noexcept;
    void WriteReadModel(GameReadModelStorage &model) const;
    [[nodiscard]] Result ApplyDebugCommand(const DebugCommand &command);
    void ApplyUiAction(const UiAction &action);
    [[nodiscard]] Result ApplySimulationRules(const SimulationRules &rules);
    [[nodiscard]] std::span<const DomainSignal> PendingDomainSignals() const noexcept;
    void ClearDomainSignals() noexcept;
    [[nodiscard]] Result Shutdown();

  private:
    struct SimulationWorld;
    std::unique_ptr<SimulationWorld> impl_;
};

} // namespace hs
