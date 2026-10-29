#pragma once

#include <hs/render_data/render_snapshot_values.hpp>

#include <cstdint>
#include <span>
#include <type_traits>

namespace hs
{
enum class VfxEntitySurfaceShape : std::uint32_t
{
    Transition,
    DissolveEdge,
};
enum class VfxEntitySurfaceDirection : std::uint32_t
{
    Up,
    UpAndOut,
};

// A surface output belongs to the actual rendered mesh and source episode.
// Phase is already resolved from the exact cooked source interval; ticks are
// owner endpoints stay integral; current_tick is the fractional display clock.
struct VfxEntitySurfaceCommand
{
    std::uint64_t render_instance_id{}, owner_id{}, event_sequence{};
    std::uint64_t status_episode_generation{};
    Tick owner_started_tick{}, owner_until_tick{};
    double current_tick{};
    std::uint32_t output_index{}, stable_seed{}, gradient_row{};
    VfxEntitySurfaceShape shape{VfxEntitySurfaceShape::Transition};
    VfxEntitySurfaceDirection direction{VfxEntitySurfaceDirection::Up};
    Float4 color{};
    float hdr{}, alpha{}, gradient_age{}, normalized_phase{};
    float noise_scale{}, edge_width{};
    // Fixed death output may outlive the simulation actor. This is a captured
    // actual instance, including its asset/pose identity; never a default mesh.
    bool has_retained_instance{};
    RenderInstance retained_instance{};
    // Legacy poses live outside RenderInstance in the snapshot. Capture the
    // actual record and remap its index when the retained instance is inserted.
    bool has_retained_animation_pose{};
    AnimationPoseRef retained_animation_pose{};
};
static_assert(std::is_trivially_copyable_v<VfxEntitySurfaceCommand>);
using VfxEntitySurfaceInputs = std::span<const VfxEntitySurfaceCommand>;
} // namespace hs
