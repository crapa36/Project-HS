#pragma once

#include <hs/core/input.hpp>
#include <hs/core/particle_spawn_command.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/core/render_snapshot.hpp>
#include <hs/core/result.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>

namespace hs
{

struct SimulationConfig
{
    std::uint64_t seed{1};
};

struct TickResult
{
    Tick tick{};
    GameplayChecksum checksum{};
};

class GameSimulation
{
  public:
    GameSimulation();
    ~GameSimulation();

    GameSimulation(const GameSimulation &) = delete;
    GameSimulation &operator=(const GameSimulation &) = delete;

    [[nodiscard]] Result Initialize(const SimulationConfig &config);
    [[nodiscard]] TickResult TickFixed(const InputFrame &input,
                                       std::chrono::nanoseconds fixed_delta);
    [[nodiscard]] GameplayChecksum ComputeChecksum() const;
    [[nodiscard]] bool WriteRenderSnapshot(RenderSnapshotStorage &snapshot) const;
    [[nodiscard]] std::span<const PresentationEvent> PendingPresentationEvents() const noexcept;
    void ClearPresentationEvents() noexcept;
    [[nodiscard]] std::span<const ParticleSpawnCommand> PendingParticleSpawns() const noexcept;
    void ClearParticleSpawns() noexcept;
    [[nodiscard]] Result Shutdown();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hs
