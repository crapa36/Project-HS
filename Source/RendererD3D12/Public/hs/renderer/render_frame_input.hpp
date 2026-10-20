#pragma once

#include <hs/core/render_view_input.hpp>
#include <hs/core/render_scene_ref.hpp>
#include <hs/renderer/renderer.hpp>
#include <optional>

namespace hs
{
struct RenderVfxFrameInput
{
    std::span<const ParticleSpawnCommand> particle_spawns;
    std::span<const VfxLineSpawnCommand> effect_lines;
    std::span<const VfxEventInput> events;
    std::span<const VfxPersistentInput> persistent;
    std::span<const VfxGroundSpawnInput> ground;
    std::span<const VfxFlashSpawnInput> flashes;
    std::span<const VfxOwnedMeshInput> meshes;
    std::span<const VfxFresnelInput> fresnels;
    VfxRibbonFrameInput ribbons;
    std::span<const VfxLightInput> lights;
    std::span<const VfxDistortionInput> distortions;
    std::span<const VfxDecalInput> decals;
    VfxVolumeInputs volumes;
    VfxScreenInputs screen;
    VfxEntitySurfaceInputs entity_surfaces;
};
// Borrowed frame package: the caller keeps snapshot leases and all spans alive
// until Render returns. One common scene is rendered sequentially into views;
// only Main presents. No generated presentation advances this render package.
struct RenderFrameInput
{
    std::span<const RenderViewInput> views;
    RenderSnapshotExchange::ReadPair snapshots;
    std::span<const PresentationEvent> events;
    RenderVfxFrameInput vfx;
    DevToolsFrameData devtools;
    std::span<const UiModel> ui;
    bool override_ui{};
    std::optional<std::chrono::nanoseconds> simulation_time;
    std::optional<float> delta_seconds;
    std::optional<RenderSceneRef> scene;
    std::optional<float> screen_overlay_intensity;
    // Offline lockstep: complete prior GPU work and await admitted asset reads
    // so publication boundaries depend on frames rather than wall-clock speed.
    bool wait_for_streaming{};
};
} // namespace hs
