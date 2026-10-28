#pragma once
#include "vfx_event_view.hpp"
#include <hs/render_data/vfx_frame_input.hpp>
#include <hs/renderer/vfx_program.hpp>
#include <hs/render_data/vfx_spawn_command.hpp>
#include <hs/runtime/vfx_image.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
namespace hs::runtime_detail
{
struct VfxPreparedBallisticBurst
{
    // Borrowed from VfxImage; the immutable image must outlive Prepare through Emit.
    const VfxBallisticRecipe *recipe{};
    std::size_t event_index{};
    std::size_t count{}, admitted_count{};
    std::uint32_t source_seed{}, importance{};
    Tick tick{}, birth_tick{};
    Float3 center{}, axis{};
    float birth_fraction{}, scale{1.0f}, radius{}, lifetime{};
    Sequence sequence{};
    bool cosmetic{};
};

void PrepareVfxTypedBallisticBursts(const VfxImage &image, VfxEventView inputs,
    std::vector<VfxPreparedBallisticBurst> &plans);
void AppendVfxPreparedBallisticBursts(std::span<const VfxPreparedBallisticBurst> plans,
    std::vector<ParticleSpawnCommand> &output,
    std::vector<std::size_t> *emitted_event_indices = nullptr);

// One immutable initial condition per authored burst particle; the GPU owns collision state.
[[nodiscard]] std::vector<ParticleSpawnCommand> BuildVfxTypedBallisticCommands(
    const VfxImage &image, VfxEventView inputs);
// Appends the logical source-order index for each input that actually emitted
// rows. Duplicate event sequences remain distinguishable.
void AppendVfxTypedBallisticCommands(const VfxImage &image, VfxEventView inputs,
    std::vector<ParticleSpawnCommand> &output,
    std::vector<std::size_t> *emitted_event_indices = nullptr);
// Compatibility append without provenance.
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
void AppendVfxSelectedPersistentStatusShardCommands(const VfxImage &image,
    std::span<const VfxPersistentInput> owners, std::span<const std::size_t> indices,
    Tick current_tick, std::vector<ParticleSpawnCommand> &output);
}
