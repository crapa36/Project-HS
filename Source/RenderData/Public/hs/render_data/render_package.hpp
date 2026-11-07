#pragma once

#include <hs/render_data/render_view_input.hpp>
#include <hs/render_data/render_snapshot_values.hpp>
#include <hs/render_data/ui_draw_content.hpp>
#include <hs/render_data/devtools_frame_data.hpp>
#include <hs/render_data/vfx_decal_input.hpp>
#include <hs/render_data/vfx_distortion_input.hpp>
#include <hs/render_data/vfx_entity_surface_input.hpp>
#include <hs/render_data/vfx_frame_input.hpp>
#include <hs/render_data/vfx_fresnel_input.hpp>
#include <hs/render_data/vfx_light_input.hpp>
#include <hs/render_data/vfx_owned_mesh.hpp>
#include <hs/render_data/vfx_ribbon_input.hpp>
#include <hs/render_data/vfx_screen_input.hpp>
#include <hs/render_data/vfx_spawn_command.hpp>
#include <hs/render_data/vfx_volume_input.hpp>
#include <cstdint>
#include <memory>
#include <optional>

namespace hs
{
// A zero sequence supplies only the held world clock; nonzero identifies a frozen batch.
struct RenderVfxWorldBatch
{
    std::uint64_t session{};
    Tick stateful_target{};
    std::uint64_t sequence{};
    bool reset_particle_state{};
};

struct RenderVfxFrameInput
{
    // Producer supplies the complete native birth set; Renderer does not synthesize snapshot births.
    std::span<const ParticleSpawnCommand> particle_spawns;
    std::span<const VfxLineSpawnCommand> effect_lines;
    std::uint32_t event_count{};
    std::uint32_t persistent_count{};
    std::span<const VfxGroundSpawnInput> ground;
    std::span<const VfxFlashSpawnInput> flashes;
    std::span<const VfxOwnedMeshInput> meshes;
    // Borrowed from an admitted catalog through Render's frame submission.
    PreparedVfxMeshRecipeView mesh_recipe_view;
    // Borrowed from an admitted catalog/static image through Render's joined record callbacks.
    PreparedVfxFresnelRecipeView fresnel_recipe_view;
    std::span<const VfxFresnelInput> fresnels;
    VfxRibbonFrameInput ribbons;
    std::span<const VfxLightInput> lights;
    std::span<const VfxDistortionInput> distortions;
    std::span<const VfxDecalInput> decals;
    VfxVolumeInputs volumes;
    VfxScreenInputs screen;
    VfxEntitySurfaceInputs entity_surfaces;
    RenderVfxWorldBatch world_batch;
    // Borrowed with the other frame spans; commands address slices by index.
    std::span<const float> distortion_gap_angles;
    std::span<const float> ground_gap_angles;
};
struct RenderDisplaySample
{
    std::chrono::steady_clock::time_point time;
    float interpolation;
};

// The caller retains snapshot leases and borrowed spans until Render returns.
// Display time, UI content and native VFX identity are fixed before submission.
struct RenderPackage
{
    std::span<const RenderViewInput> views;
    RenderSnapshotPair snapshots;
    RenderVfxFrameInput vfx;
    DevToolsFrameData devtools;
    // The owner, including an empty UI, is selected by the producer.
    std::shared_ptr<const UiDrawContent> ui_draw_content{EmptyUiDrawContent()};
    std::optional<float> screen_overlay_intensity;
    std::uint64_t ui_packet_generation{};
    // Capture before preparing effects so all package consumers sample one clock.
    // Views do not retime this explicit sample; discontinuities reset their history.
    std::optional<RenderDisplaySample> display_sample;
};
} // namespace hs
