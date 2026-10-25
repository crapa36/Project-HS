#pragma once

#include <hs/renderer/vfx_entity_surface_input.hpp>
#include <hs/renderer/vfx_frame_input.hpp>
#include <hs/renderer/vfx_program.hpp>

#include <span>
#include <memory>
#include <vector>

namespace hs::runtime_detail
{
// Capture is supplied by the actual presentation mesh projection. A position
// match or a default RenderMesh is never a substitute for this instance.
struct VfxEntitySurfaceEventCapture
{
    VfxEventInput input;
    RenderInstance instance;
    bool retained_instance{};
    bool has_animation_pose{};
    AnimationPoseRef animation_pose{};
    std::shared_ptr<const void> generation_lease;
};

struct VfxEntitySurfacePersistentCapture
{
    VfxPersistentInput input;
    RenderInstance instance;
    Tick owner_started_tick{}, owner_until_tick{};
};

// Converts only the two fixed boss transitions and the live phase-transition
// emissive edge. Caller owns event retention and snapshot/capture provenance.
[[nodiscard]] std::vector<VfxEntitySurfaceCommand> BuildVfxTypedEntitySurfaceCommands(
    const VfxProgramView &program,
    std::span<const VfxEntitySurfaceEventCapture> events,
    std::span<const VfxEntitySurfacePersistentCapture> persistent,
    Tick current_tick,
    Tick death_current_tick = 0);
} // namespace hs::runtime_detail
