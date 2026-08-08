#pragma once

#include <hs/core/input.hpp>
#include <hs/core/particle_spawn_command.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/core/render_snapshot.hpp>
#include <hs/core/result.hpp>
#include <hs/core/settings.hpp>
#include <hs/gameplay/game_data.hpp>
#include <hs/gameplay/gameplay_types.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>

namespace hs
{

struct SimulationConfig
{
    std::uint64_t seed{1};
    bool automatic_choices{};
    bool start_in_main_menu{};
    SettingsData settings{};
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
    [[nodiscard]] Result Initialize(const SimulationConfig &config, const GameData &data);
    [[nodiscard]] TickResult TickFixed(const InputFrame &input,
                                       std::chrono::nanoseconds fixed_delta);
    [[nodiscard]] GameplayChecksum ComputeChecksum() const;
    [[nodiscard]] SessionProbe Probe() const noexcept;
    [[nodiscard]] Result ApplyDebugCommand(const DebugCommand &command);
    void ApplySettings(const SettingsData &settings) noexcept;
    [[nodiscard]] Result ApplyGameData(const GameData &data);
    [[nodiscard]] bool WriteRenderSnapshot(RenderSnapshotStorage &snapshot) const;
    [[nodiscard]] std::span<const PresentationEvent> PendingPresentationEvents() const noexcept;
    void ClearPresentationEvents() noexcept;
    [[nodiscard]] std::span<const UiCommand> PendingUiCommands() const noexcept;
    void ClearUiCommands() noexcept;
    [[nodiscard]] std::span<const ParticleSpawnCommand> PendingParticleSpawns() const noexcept;
    void ClearParticleSpawns() noexcept;
    [[nodiscard]] Result Shutdown();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hs
