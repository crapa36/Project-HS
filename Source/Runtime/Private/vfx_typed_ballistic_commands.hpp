#pragma once
#include "vfx_event_view.hpp"
#include <hs/render_data/vfx_frame_input.hpp>
#include <hs/renderer/vfx_program.hpp>
#include <hs/render_data/vfx_spawn_command.hpp>
#include <hs/runtime/vfx_image.hpp>
#include <span>
#include <vector>
namespace hs::runtime_detail
{
// One immutable initial condition per authored burst particle; the GPU owns collision state.
[[nodiscard]] std::vector<ParticleSpawnCommand> BuildVfxTypedBallisticCommands(
    const VfxImage &image, VfxEventView inputs);
// Append into the retained submission queue without copying a per-event buffer.
void BuildVfxTypedBallisticCommands(const VfxImage &image,
    VfxEventView inputs, std::vector<ParticleSpawnCommand> &output);

// Call only for newly activated attachment owners. Each owner creates its five
// immutable status_shards initial conditions at current_tick.
[[nodiscard]] std::vector<ParticleSpawnCommand> BuildVfxPersistentStatusShardCommands(
    const VfxImage &image,
    std::span<const VfxPersistentInput> inputs,
    Tick current_tick);
void BuildVfxPersistentStatusShardCommands(const VfxImage &image,
    std::span<const VfxPersistentInput> inputs, Tick current_tick,
    std::vector<ParticleSpawnCommand> &output);
}
