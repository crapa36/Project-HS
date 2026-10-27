#pragma once
#include "vfx_event_view.hpp"

#include <hs/render_data/vfx_frame_input.hpp>
#include <hs/runtime/vfx_image.hpp>
#include <hs/render_data/vfx_spawn_command.hpp>

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
    const VfxImage &image,
    std::span<const VfxPersistentInput> inputs,
    Tick current_tick);

[[nodiscard]] std::vector<VfxTypedGroundCommand> BuildVfxTypedEventGroundCommands(
    const VfxImage &image,
    VfxEventView inputs,
    double current_tick);

} // namespace hs::runtime_detail
