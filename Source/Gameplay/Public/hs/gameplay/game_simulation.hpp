#pragma once

#include <hs/core/input.hpp>
#include <hs/core/result.hpp>
#include <hs/game_rules/simulation_rules.hpp>
#include <hs/game_domain/game_types.hpp>
#include <hs/game_domain/domain_signal.hpp>
#include <hs/game_domain/tick_read_view.hpp>

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
    bool automatic_spawning{true};
};

struct SimulationConfig
{
    std::uint64_t seed{1};
    bool automatic_choices{};
    bool start_in_main_menu{};
    SimulationScenarioPolicy scenario{};
    // Recorded/test sessions publish checksums; ordinary play can request them explicitly.
    bool capture_checksums{true};
    bool capture_timings{false};
    // Session-start switch for optional upgrade/relic synergy detail. Fixed balance
    // aggregates and gameplay state are collected identically in either mode.
    bool capture_detailed_balance{true};
};

struct TickResult
{
    Tick tick{};
    // Zero when automatic checksum capture is disabled.
    GameplayChecksum checksum{};
    SessionPhase phase{SessionPhase::MainMenu};
};

class GameSimulation
{
    struct SimulationWorld;
    friend struct GameplayTestAccess;
  public:
    class ScopedTickReadView
    {
      public:
        ~ScopedTickReadView();
        ScopedTickReadView(ScopedTickReadView &&other) noexcept;
        ScopedTickReadView(const ScopedTickReadView &) = delete;
        ScopedTickReadView &operator=(const ScopedTickReadView &) = delete;
        ScopedTickReadView &operator=(ScopedTickReadView &&) = delete;
        [[nodiscard]] const TickReadView &View() const &;
        const TickReadView &View() const && = delete;
      private:
        friend class GameSimulation;
        ScopedTickReadView(SimulationWorld &owner, TickReadView view);
        SimulationWorld *owner_{};
        TickReadView view_;
    };
    GameSimulation();
    ~GameSimulation();

    GameSimulation(const GameSimulation &) = delete;
    GameSimulation &operator=(const GameSimulation &) = delete;

    [[nodiscard]] Result Initialize(const SimulationConfig &config);
    [[nodiscard]] Result Initialize(const SimulationConfig &config,
                                    const SimulationRules &rules);
    [[nodiscard]] TickResult TickFixed(const InputFrame &input,
                                       std::chrono::nanoseconds fixed_delta);
    // Always returns the current checksum, even when automatic capture is disabled.
    [[nodiscard]] GameplayChecksum ComputeChecksum() const;
    [[nodiscard]] const SimulationRules &Rules() const noexcept;
    [[nodiscard]] SessionProbe GetSessionProbe() const;
    [[nodiscard]] SimulationDiagnostics GetDiagnostics() const;
    [[nodiscard]] SimulationTimingDiagnostics GetTimingDiagnostics() const;
    [[nodiscard]] SimulationObservation GetObservation() const;
    [[nodiscard]] ScopedTickReadView BorrowTickReadView() const;
    // Owned-format export for development fixtures; ordinary consumers borrow instead.
    void CopyReadModelForDevelopment(GameReadModelStorage &model) const;
    [[nodiscard]] Result ApplyDebugCommand(const DebugCommand &command);
    void ApplyUiAction(const UiAction &action);
    [[nodiscard]] Result ApplySimulationRules(const SimulationRules &rules);
    [[nodiscard]] std::span<const DomainSignal> PendingDomainSignals() const noexcept;
    void ClearDomainSignals();
    [[nodiscard]] Result Shutdown();

  private:
    std::unique_ptr<SimulationWorld> impl_;
};

} // namespace hs
