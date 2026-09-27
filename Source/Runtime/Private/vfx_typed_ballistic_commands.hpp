#pragma once
#include <hs/renderer/vfx_frame_input.hpp>
#include <hs/renderer/vfx_program_loader.hpp>
#include <hs/renderer/vfx_spawn_command.hpp>
#include <span>
#include <vector>
namespace hs::runtime_detail
{
// One immutable initial condition per authored burst particle; the GPU owns collision state.
[[nodiscard]] std::vector<ParticleSpawnCommand> BuildVfxTypedBallisticCommands(
    const VfxProgramData &program, std::span<const VfxEventInput> inputs);

// Call only for newly activated attachment owners. Each owner creates its five
// immutable status_shards initial conditions at current_tick.
[[nodiscard]] std::vector<ParticleSpawnCommand> BuildVfxPersistentStatusShardCommands(
    const VfxProgramData &program,
    std::span<const VfxPersistentInput> inputs,
    Tick current_tick);
}
