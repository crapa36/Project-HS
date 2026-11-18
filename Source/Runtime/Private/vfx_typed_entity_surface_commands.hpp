#pragma once

#include <hs/runtime/vfx_image.hpp>
#include <hs/render_data/vfx_entity_surface_input.hpp>
#include <hs/render_data/vfx_frame_input.hpp>

#include <span>
#include <vector>

namespace hs::runtime_detail
{
// Capture is supplied by the actual presentation mesh projection. A position
// match or a default RenderMesh is never a substitute for this instance.
struct VfxEntitySurfaceEventCapture
{
    const VfxEventInput &input;
    const RenderInstance &instance;
    bool retained_instance{};
    bool has_animation_pose{};
    const AnimationPoseRef &animation_pose;
};

struct VfxEntitySurfacePersistentCapture
{
    const VfxPersistentInput &input;
    const RenderInstance &instance;
    Tick owner_started_tick{}, owner_until_tick{};
};

// Converts only the two fixed boss transitions and the live phase-transition
// emissive edge. The image must be prepared and immutable.
// Caller owns event retention and snapshot/capture provenance.
void AppendVfxTypedEntitySurfaceCommands(
    const VfxImage &image,
    std::span<const VfxEntitySurfaceEventCapture> events,
    std::span<const VfxEntitySurfacePersistentCapture> persistent,
    double current_tick,
    double death_current_tick,
    std::vector<VfxEntitySurfaceCommand> &result);
} // namespace hs::runtime_detail
