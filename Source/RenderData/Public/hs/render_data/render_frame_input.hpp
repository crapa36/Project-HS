#pragma once

#include <hs/render_data/render_view_input.hpp>
#include <hs/render_data/ui_draw_content.hpp>
#include <hs/render_data/render_scene_ref.hpp>
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
#include <hs/core/presentation_event.hpp>
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
    std::span<const ParticleSpawnCommand> particle_spawns;
    std::span<const VfxLineSpawnCommand> effect_lines;
    std::uint32_t event_count{};
    std::uint32_t persistent_count{};
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
    // Unspecified preserves legacy snapshot-tick stepping.
    std::optional<RenderVfxWorldBatch> world_batch;
};
// Borrowed frame package: the caller keeps snapshot leases and all spans alive
// until Render returns. One common scene is rendered sequentially into views;
// only Main presents. No generated presentation advances this render package.
struct RenderFrameInput
{
    std::span<const RenderViewInput> views;
    RenderSnapshotPair snapshots;
    std::span<const PresentationEvent> events;
    RenderVfxFrameInput vfx;
    DevToolsFrameData devtools;
    std::span<const UiModel> ui;
    // Optional immutable owner enables renderer cache hits without pointer/size identity.
    std::shared_ptr<const UiDrawContent> ui_draw_content;
    bool override_ui{};
    std::optional<std::chrono::nanoseconds> simulation_time;
    std::optional<float> delta_seconds;
    std::optional<RenderSceneRef> scene;
    std::optional<float> screen_overlay_intensity;
    // Offline lockstep: complete prior GPU work and await admitted asset reads
    // so publication boundaries depend on frames rather than wall-clock speed.
    bool wait_for_streaming{};
    std::uint64_t ui_packet_generation{};
    // Capture before preparing effects so all package consumers sample one clock.
    // Views do not retime this explicit sample; discontinuities reset their history.
    std::optional<std::chrono::steady_clock::time_point> sample_time;
};
} // namespace hs
