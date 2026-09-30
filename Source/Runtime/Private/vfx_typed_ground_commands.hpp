#pragma once

#include <hs/renderer/vfx_frame_input.hpp>
#include <hs/renderer/vfx_program_loader.hpp>
#include <hs/renderer/vfx_spawn_command.hpp>

#include <span>
#include <vector>

namespace hs::runtime_detail
{

struct VfxTypedGroundCommand
{
    ParticleSpawnCommand command;
    bool additive{};
    std::uint64_t stable_id{}; // Identity of the gameplay owner, not a generated particle ID.
    VfxEffectHandle effect_handle{kInvalidVfxEffectHandle};
    std::uint8_t source_visual_kind{0xff};
    float edge_width_world{};
    float normalized_age{};
    float lifetime01{};
    VfxGroundMotion motion;
    std::uint32_t gradient_row{};
    float hdr{};
    VfxGroundGeometry geometry;
};

// Converts authored persistent CircleAreaPayload boundaries and interior fills.
// Callers retain the owner identity and world-space edge width alongside the
// legacy spawn command until the renderer's exact-ring path consumes them.
[[nodiscard]] std::vector<VfxTypedGroundCommand> BuildVfxTypedGroundCommands(
    const VfxProgramData &cooked,
    std::span<const VfxPersistentInput> inputs,
    Tick current_tick);

[[nodiscard]] std::vector<VfxTypedGroundCommand> BuildVfxTypedEventGroundCommands(
    const VfxProgramData &cooked,
    std::span<const VfxEventInput> inputs,
    Tick current_tick);

} // namespace hs::runtime_detail
