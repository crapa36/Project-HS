#include "renderer_impl.hpp"
#include "renderer_reconstruction.hpp"
#include "renderer_named_animation.hpp"
#include "renderer_entity_surface.hpp"
#include <numbers>
#include <unordered_set>

namespace hs
{
namespace
{
Result ViewFailure(std::string_view message)
{
    return Result::Failure(ErrorCode::InvalidArgument, "hs_render_view", std::string(message));
}
bool Finite(RenderWorldPosition p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
bool ExtentValid(ViewExtent e)
{
    return e.width && e.height && e.width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        e.height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}
}

Result D3D12Renderer::Impl::ValidateFrameInput(const RenderFrameInput &input) const
{
    if (input.views.empty()) return ViewFailure("A render frame requires at least one view.");
    if (input.delta_seconds && (!std::isfinite(*input.delta_seconds) || *input.delta_seconds < 0))
        return ViewFailure("Frame delta must be finite and nonnegative.");
    if (input.screen_overlay_intensity && (!std::isfinite(*input.screen_overlay_intensity) ||
        *input.screen_overlay_intensity < 0 || *input.screen_overlay_intensity > 1))
        return ViewFailure("Screen overlay intensity must be finite and between zero and one.");
    if (std::ranges::any_of(input.views, [](const auto &view) { return view.role == RenderViewRole::Main; }) &&
        sdk_presentation.LatencyOwner() != SdkLatencyOwner::None && native_input_phase != 3)
        return Result::Failure(ErrorCode::InvalidState, "hs_render_view",
            "Native latency rendering requires acknowledged input sampling and simulation for this frame token.");
    if (streamline_session && streamline_session->Initialized() && render_package_id >= UINT32_MAX)
        return ViewFailure("Native rendered-package identity is exhausted; reinitialize the renderer.");
    std::vector<std::uint32_t> identities;
    const bool dynamic_resolution_active = config.dynamic_resolution && !ResolveFeatures(config).software_gi_enabled;
    const auto origin = input.views.front().scene_origin;
    if (native_input_phase && std::ranges::none_of(input.views, [](const auto &view) { return view.role == RenderViewRole::Main; }))
        return ViewFailure("An outstanding main input token must be completed by a packet containing the main view.");
    for (const auto &view : input.views)
    {
        if (!view.id.value || std::ranges::find(identities, view.id.value) != identities.end())
            return ViewFailure("View IDs must be nonzero and unique within a frame.");
        identities.push_back(view.id.value);
        if (view.role > RenderViewRole::Capture ||
            ((view.role == RenderViewRole::Main) != (view.id == kMainRenderViewId)))
            return ViewFailure("The reserved main view ID must have the Main role; other IDs are offscreen.");
        if (const auto found = owned_views.find(view.id.value); found != owned_views.end() && found->second->role != view.role)
            return ViewFailure("A resident view ID cannot change role; release it first.");
        if (!ExtentValid(view.output_extent) || !ExtentValid(view.render_capacity) ||
            !ExtentValid(view.render_active) || view.render_active.width > view.render_capacity.width ||
            view.render_active.height > view.render_capacity.height)
            return ViewFailure("View output, capacity and active extents are outside the native limits.");
        if (dynamic_resolution_active)
        {
            ViewExtent minimum;
            if (!ResolveDynamicResolutionExtent(view.output_extent, config.dynamic_min_percent, minimum) ||
                view.render_capacity.width < minimum.width || view.render_capacity.height < minimum.height)
                return ViewFailure("View capacity cannot cover the configured integer DRS minimum extent.");
        }
        if (view.role == RenderViewRole::Main &&
            (view.output_extent != ViewExtent{width, height} || view.render_capacity != view_state.render_capacity))
            return ViewFailure("Main output/capacity must match the current presentation; use Resize or ApplyOptions first.");
        if (!Finite(view.scene_origin) || !Finite(view.view_origin) || !Finite(view.camera.position) ||
            !Finite(view.camera.target) || view.scene_origin != origin ||
            !std::isfinite(view.camera.up.x) || !std::isfinite(view.camera.up.y) || !std::isfinite(view.camera.up.z) ||
            !std::isfinite(view.camera.vertical_fov_degrees) || view.camera.vertical_fov_degrees <= 0 ||
            view.camera.vertical_fov_degrees >= 179 || !std::isfinite(view.near_plane) ||
            !std::isfinite(view.far_plane) || view.near_plane <= 0 || view.far_plane <= view.near_plane)
            return ViewFailure("Views require finite cameras, ordered near/far planes and one common scene origin.");
        const double dx = view.camera.target.x - view.camera.position.x;
        const double dy = view.camera.target.y - view.camera.position.y;
        const double dz = view.camera.target.z - view.camera.position.z;
        const double cx = dy * view.camera.up.z - dz * view.camera.up.y;
        const double cy = dz * view.camera.up.x - dx * view.camera.up.z;
        const double cz = dx * view.camera.up.y - dy * view.camera.up.x;
        if (!std::isfinite(dx*dx+dy*dy+dz*dz) || dx*dx+dy*dy+dz*dz == 0 ||
            !std::isfinite(cx*cx+cy*cy+cz*cz) || cx*cx+cy*cy+cz*cz == 0)
            return ViewFailure("Camera direction and up must form a finite nondegenerate basis.");
        if (const auto found = owned_views.find(view.id.value); found != owned_views.end())
        {
            const bool active = view.id == active_view_id;
            const auto &plan = active ? reconstruction_plan : found->second->reconstruction_plan;
            const auto *provider = active ? reconstruction_provider.get() : found->second->reconstruction_provider.get();
            const bool same_configuration = active || (found->second->state.output_extent == view.output_extent &&
                found->second->state.render_capacity == view.render_capacity &&
                found->second->configuration_revision == view_configuration_revision &&
                found->second->content_generation == resource_generation_id && found->second->pipeline_revision == pipeline_revision);
            if (same_configuration && provider && provider->Initialized())
                if (view.render_active.width < plan.input_minimum.width || view.render_active.height < plan.input_minimum.height ||
                    view.render_active.width > plan.input_maximum.width || view.render_active.height > plan.input_maximum.height)
                    return ViewFailure("Requested active extent is outside this view's selected native reconstruction domain.");
            const bool prior_valid = active ? previous_origins_valid : found->second->previous_origins_valid;
            const auto prior_origin = active ? previous_scene_origin : found->second->previous_scene_origin;
            if (same_configuration && prior_valid && !view.camera_cut)
                for (const double shift : {view.scene_origin.x-prior_origin.x, view.scene_origin.y-prior_origin.y, view.scene_origin.z-prior_origin.z})
                    if (!std::isfinite(shift) || std::abs(shift) > std::numeric_limits<float>::max())
                        return ViewFailure("History origin shift exceeds the GPU coordinate domain; submit an explicit camera cut.");
        }
        // Coordinates enter float GPU contracts only after double subtraction.
        for (const auto p : {view.camera.position, view.camera.target, view.view_origin})
            for (const double delta : {p.x-origin.x, p.y-origin.y, p.z-origin.z})
                if (!std::isfinite(delta) || std::abs(delta) > std::numeric_limits<float>::max())
                    return ViewFailure("Scene-relative camera/origin exceeds the GPU coordinate domain.");
    }
    const auto snapshot_scene = input.snapshots.has_current ? input.snapshots.current.static_scene : nullptr;
    const auto scene = input.scene ? input.scene->lease : snapshot_scene;
    if (input.scene && snapshot_scene && input.scene->lease != snapshot_scene)
        return ViewFailure("Frame scene association must retain the snapshot's actual immutable scene lease.");
    if (input.scene && scene && input.scene->content_generation_id != scene->content_generation_id)
        return ViewFailure("Frame scene association does not match its actual lease generation.");
    if (input.scene && scene && input.scene->scene_asset_id.value != scene->scene_asset_id.value)
        return ViewFailure("Frame scene asset identity does not match its actual immutable lease.");
    if (scene && !scene->content_generation_id.empty() && scene->content_generation_id != resource_generation_id)
        return ViewFailure("Frame scene generation must match the resident GPU asset generation.");
    if (input.scene && !input.scene->content_generation_id.empty() && input.scene->content_generation_id != resource_generation_id)
        return ViewFailure("Frame scene association must match the resident GPU asset generation.");
    for (const auto &event : input.vfx.events)
        if (event.effect_handle == kInvalidVfxEffectHandle) return ViewFailure("Typed VFX event has an invalid effect handle.");
    // Admission is transactional: validation of the persistent set uses a copy.
    auto persistent = typed_vfx_persistent_state;
    std::string error;
    if (!persistent.Update(input.vfx.persistent, error)) return ViewFailure(error);
    return ValidateFramePayload(input);
}

Result D3D12Renderer::Impl::ValidateFramePayload(const RenderFrameInput &frame_input) const
{
    const auto &input = frame_input;
    std::unordered_set<std::uint64_t> fresnel_ids;
    if (input.vfx.fresnels.size() > 32) return ViewFailure("Too many Fresnel shell commands.");
    for (const auto &source : input.vfx.fresnels)
    {
        const auto finite_transform = [](const VfxTransform &transform) {
            return std::ranges::all_of(transform, [](float value) { return std::isfinite(value); });
        };
        const bool crown = source.kind == VfxFresnelShellKind::BossCrown;
        const bool transition = source.kind == VfxFresnelShellKind::BossTransition;
        const bool player = source.kind == VfxFresnelShellKind::PlayerInvulnerable;
        if (source.render_instance_id == 0 || source.stable_id == 0 ||
            source.effect_handle == kInvalidVfxEffectHandle ||
            !fresnel_ids.insert(source.stable_id).second || (!crown && !transition && !player) ||
            !finite_transform(source.current_transform) ||
            !finite_transform(source.previous_transform) ||
            !std::isfinite(source.footprint_radius) || source.footprint_radius <= 0.0f ||
            !std::isfinite(source.source_progress) || source.source_progress < 0.0f ||
            source.source_progress >= 1.0f ||
            !std::isfinite(source.elapsed_seconds) || source.elapsed_seconds < 0.0f ||
            !std::isfinite(source.color.x) || source.color.x < 0.0f ||
            !std::isfinite(source.color.y) || source.color.y < 0.0f ||
            !std::isfinite(source.color.z) || source.color.z < 0.0f ||
            !std::isfinite(source.color.w) || source.color.w < 0.0f || source.color.w > 1.0f ||
            !std::isfinite(source.hdr) || source.hdr <= 0.0f ||
            source.gradient_row >= this->vfx_gradient_rows ||
            !std::isfinite(source.motion_rate_hz) || source.motion_rate_hz < 0.0f ||
            !std::isfinite(source.motion_amplitude) || source.motion_amplitude < 0.0f ||
            source.motion_amplitude > 1.0f ||
            !std::isfinite(source.motion_inset_fraction) ||
            source.motion_inset_fraction < 0.0f || source.motion_inset_fraction > 1.0f ||
            !std::isfinite(source.noise_amount) || source.noise_amount < 0.0f ||
            source.noise_amount > 1.0f ||
            (crown && (source.sector_count != 6 || !std::isfinite(source.rotation_hz) ||
                       source.rotation_hz >= 0.0f || source.fresnel_power != 0.0f ||
                       source.end_crack != 0 || source.noise_amount != 0.0f)) ||
            (transition && (source.sector_count != 0 || source.rotation_hz != 0.0f ||
                            !std::isfinite(source.fresnel_power) || source.fresnel_power <= 0.0f ||
                            source.end_crack != 1 || source.noise_amount != 0.0f)) ||
            (player && (source.sector_count != 0 || source.rotation_hz != 0.0f ||
                        !std::isfinite(source.fresnel_power) || source.fresnel_power <= 0.0f ||
                        source.end_crack != 0)))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                   "Invalid Fresnel shell command.");
    }
    for (const auto &source : input.vfx.meshes)
    {
            const auto finite = [](const Float3 &v) {
                return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
            };
            if (source.mesh_index == 0 || source.mesh_index > 7 ||
                source.gradient_row >= this->vfx_gradient_rows || !finite(source.position) ||
                !finite(source.previous_position) || !finite(source.velocity) ||
                !finite({source.color.x,source.color.y,source.color.z}) || !std::isfinite(source.color.w) ||
                !std::isfinite(source.radius) || source.radius <= 0 ||
                !std::isfinite(source.hdr) || source.hdr <= 0 ||
                !std::isfinite(source.fresnel) || source.fresnel < 0 || source.fresnel > 1 ||
                !std::isfinite(source.head_to_tail_ratio) || source.head_to_tail_ratio < 0 || source.head_to_tail_ratio >= 1 ||
                !std::isfinite(source.shell_thickness) || source.shell_thickness < 0)
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid owner mesh command.");
    }
    for (const auto &source : input.vfx.flashes)
    {
        const auto finite = std::isfinite(source.position.x) &&
            std::isfinite(source.position.y) && std::isfinite(source.position.z) &&
            std::isfinite(source.color.x) && std::isfinite(source.color.y) &&
            std::isfinite(source.color.z) && std::isfinite(source.color.w) &&
            std::isfinite(source.size) && source.size > 0.0f &&
            std::isfinite(source.delay) && source.delay >= 0.0f &&
            std::isfinite(source.lifetime) && source.lifetime > 0.0f &&
            std::isfinite(source.hdr) && source.hdr > 0.0f &&
            std::isfinite(source.normalized_age) && source.normalized_age >= 0.0f &&
            source.normalized_age <= 1.0f && source.gradient_row < this->vfx_gradient_rows &&
            static_cast<std::uint32_t>(source.shape) <= static_cast<std::uint32_t>(VfxImpactShape::Spark) &&
            std::isfinite(source.direction.x) && std::isfinite(source.direction.y) && std::isfinite(source.direction.z) &&
            std::isfinite(source.aspect) && source.aspect > 0 &&
            (source.curve_row == 0xffffffffu || source.curve_row < 7) &&
            (source.mask_slice == 0xffffffffu || source.mask_slice < 12) &&
            std::isfinite(source.mask_strength) && source.mask_strength >= 0 && source.mask_strength <= 1 &&
            std::isfinite(source.initial_offset.x) && std::isfinite(source.initial_offset.y) && std::isfinite(source.initial_offset.z) &&
            std::isfinite(source.initial_velocity.x) && std::isfinite(source.initial_velocity.y) && std::isfinite(source.initial_velocity.z) &&
            std::isfinite(source.orbit_radius) && source.orbit_radius >= 0 &&
            std::isfinite(source.orbit_phase) && std::isfinite(source.orbit_rate) &&
            std::isfinite(source.drag) && source.drag >= 0 &&
            source.oit == (static_cast<std::uint32_t>(source.shape) >= 10 && static_cast<std::uint32_t>(source.shape) <= 12) &&
            source.fbm_octaves > 0 && source.fbm_octaves <= 4 &&
            std::isfinite(source.domain_warp) && source.domain_warp >= 0 &&
            std::isfinite(source.opacity_scale) && source.opacity_scale > 0 &&
            std::isfinite(source.motion_strength) && source.motion_strength >= 0 && source.motion_strength <= 1 &&
            std::isfinite(source.procedural_flow_speed) && source.procedural_flow_speed >= 0 &&
            std::isfinite(source.smoke_fps) && source.smoke_fps >= 0 &&
            (source.shape != VfxImpactShape::Smoke6Way ||
             (source.smoke_frame_count > 0 && source.smoke_first_frame < 64 &&
              source.smoke_frame_count <= 64 - source.smoke_first_frame && source.smoke_fps > 0));
        const auto spark_speed = std::hypot(source.initial_velocity.x,
                                            source.initial_velocity.y,
                                            source.initial_velocity.z);
        const auto spark_direction = std::hypot(source.direction.x,
                                                source.direction.y,
                                                source.direction.z);
        const auto spark_alignment = source.direction.x * source.initial_velocity.x +
            source.direction.y * source.initial_velocity.y +
            source.direction.z * source.initial_velocity.z;
        if (!finite || (source.shape == VfxImpactShape::Spark &&
            (source.oit || source.ground_base_anchor || source.aspect != 1.0f ||
             !std::isfinite(spark_speed) || spark_speed <= 0.0f ||
             !std::isfinite(spark_direction) || std::abs(spark_direction - 1.0f) > 0.001f ||
             spark_alignment < 0.999f * spark_speed)))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                   "Invalid authored flash command.");
    }
    for (const auto &input : frame_input.vfx.ground)
    {
            const auto &source = input.command;
            const auto &geometry = input.geometry;
            const bool line = geometry.shape == VfxGroundShape::LineBorder ||
                              geometry.shape == VfxGroundShape::LineHatch;
            const bool cone = geometry.shape == VfxGroundShape::ConeBorder ||
                              geometry.shape == VfxGroundShape::ConeHatch;
            const bool safe_sector = geometry.shape == VfxGroundShape::SafeSectorMarker;
            const bool hex = geometry.shape == VfxGroundShape::HexConstellation;
            const bool cross_ring = geometry.shape == VfxGroundShape::CrossRing;
            const bool broken_hex = geometry.shape == VfxGroundShape::BrokenHex;
            const bool axial_fracture = geometry.shape == VfxGroundShape::AxialFracture;
            const bool repeating_chevron = geometry.shape == VfxGroundShape::RepeatingChevron;
            const bool broken_crown = geometry.shape == VfxGroundShape::BrokenCrown;
            const bool closed_crown = geometry.shape == VfxGroundShape::ClosedCrownRing;
            const bool state_ring = geometry.shape == VfxGroundShape::StateRing;
            const bool polar_rune = geometry.shape == VfxGroundShape::PolarRune;
            const bool authored_additive_shape = cross_ring || broken_hex || axial_fracture ||
                repeating_chevron || broken_crown || closed_crown;
            const bool circle_preview = geometry.shape == VfxGroundShape::CirclePreviewBorder || geometry.shape == VfxGroundShape::CirclePreviewTicks;
            const bool preview = circle_preview || geometry.shape == VfxGroundShape::RingGapsPreviewBorder || geometry.shape == VfxGroundShape::RingGapsTicks;
            const bool annulus = preview || geometry.shape == VfxGroundShape::RingGapsBorder || geometry.shape == VfxGroundShape::RingGapsFill;
            const bool boss_signature = input.additive && geometry.animation_phase == 1.0f &&
                (geometry.shape == VfxGroundShape::Circle ||
                 geometry.shape == VfxGroundShape::ConeBorder ||
                 geometry.shape == VfxGroundShape::RingGapsBorder);
            if (preview && (!std::isfinite(geometry.progress) || geometry.progress < 0 || geometry.progress > 1 ||
                !std::isfinite(geometry.animation_phase) || geometry.animation_phase < 0 ||
                ((geometry.shape == VfxGroundShape::RingGapsTicks || geometry.shape == VfxGroundShape::CirclePreviewTicks) && geometry.tick_count == 0)))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored annulus preview timing.");
            if (annulus && (!std::isfinite(geometry.inner_radius) || geometry.inner_radius < 0 ||
                !std::isfinite(geometry.outer_radius) || geometry.outer_radius <= geometry.inner_radius ||
                !std::isfinite(geometry.edge_width) || geometry.edge_width < 0 ||
                ((geometry.shape == VfxGroundShape::RingGapsBorder || geometry.shape == VfxGroundShape::RingGapsPreviewBorder || geometry.shape == VfxGroundShape::CirclePreviewBorder) && geometry.edge_width == 0) ||
                (circle_preview && (geometry.inner_radius != 0 || !geometry.gap_angles_radians.empty() || geometry.gap_half_angle_radians != 0)) ||
                !std::isfinite(geometry.gap_half_angle_radians) || geometry.gap_half_angle_radians < 0 ||
                geometry.gap_half_angle_radians > std::numbers::pi_v<float> ||
                std::ranges::any_of(geometry.gap_angles_radians, [](float angle) { return !std::isfinite(angle); })))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored annulus geometry.");
            const float direction_length = std::hypot(geometry.direction.x, geometry.direction.z);
            if (safe_sector && (!std::isfinite(geometry.inner_radius) || geometry.inner_radius < 0 ||
                !std::isfinite(geometry.outer_radius) || geometry.outer_radius <= geometry.inner_radius ||
                !std::isfinite(geometry.half_angle_radians) || geometry.half_angle_radians <= 0 || geometry.half_angle_radians > std::numbers::pi_v<float> ||
                !std::isfinite(geometry.edge_width) || geometry.edge_width <= 0 ||
                !std::isfinite(geometry.animation_phase) || geometry.spokes == 0 || geometry.rings == 0 ||
                !std::isfinite(direction_length) || direction_length < .999f || direction_length > 1.001f ||
                !std::isfinite(geometry.direction.y) || geometry.direction.y != 0))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored safe-sector marker.");
            if (hex && (!input.additive || source.primitive != VfxPrimitive::ExactRing ||
                geometry.spokes != 6 || !std::isfinite(geometry.progress) ||
                geometry.progress < 0.0f || geometry.progress >= 1.0f ||
                (geometry.animation_phase != 0.0f && geometry.animation_phase != 1.0f) ||
                !std::isfinite(geometry.outer_radius) || geometry.outer_radius <= 0.0f ||
                !std::isfinite(geometry.edge_width) || geometry.edge_width <= 0.0f ||
                !std::isfinite(source.start_size_min) ||
                source.start_size_min < geometry.outer_radius + geometry.edge_width ||
                (geometry.mask_slice != 0xffffffffu && geometry.mask_slice >= 12) ||
                !std::isfinite(geometry.mask_strength) || geometry.mask_strength < 0.0f ||
                geometry.mask_strength > 1.0f ||
                (geometry.mask_slice == 0xffffffffu && geometry.mask_strength != 0.0f)))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored hex constellation.");
            if (state_ring && (input.additive || source.primitive != VfxPrimitive::ExactRing ||
                geometry.spokes < 3 || geometry.spokes > 64 ||
                geometry.rings > 1 ||
                !std::isfinite(geometry.progress) ||
                geometry.progress < 0.0f || geometry.progress > 1.0f ||
                !std::isfinite(geometry.animation_phase) || geometry.animation_phase < 0.0f ||
                !std::isfinite(geometry.outer_radius) || geometry.outer_radius <= 0.0f ||
                !std::isfinite(geometry.inner_radius) || geometry.inner_radius < 0.0f ||
                geometry.inner_radius >= geometry.outer_radius ||
                !std::isfinite(geometry.edge_width) || geometry.edge_width <= 0.0f ||
                std::abs(geometry.inner_radius -
                         std::max(0.0f, geometry.outer_radius - geometry.edge_width)) > 0.0001f ||
                !std::isfinite(source.start_size_min) ||
                source.start_size_min <= 0.0f ||
                std::abs(source.start_size_min -
                         (geometry.outer_radius + geometry.edge_width)) > 0.0001f ||
                !std::isfinite(source.stretch) ||
                std::abs(source.stretch - geometry.outer_radius / source.start_size_min) > 0.001f ||
                geometry.mask_slice != 0xffffffffu || geometry.mask_strength != 0.0f ||
                !geometry.gap_angles_radians.empty()))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored state ring.");
            if (polar_rune && (input.additive || source.primitive != VfxPrimitive::ExactRing ||
                geometry.spokes != 6 || geometry.rings != 2 ||
                !std::isfinite(geometry.progress) || geometry.progress < 0.0f ||
                geometry.progress >= 1.0f || !std::isfinite(geometry.animation_phase) ||
                geometry.animation_phase < 0.0f ||
                !std::isfinite(geometry.outer_radius) || geometry.outer_radius <= 0.0f ||
                !std::isfinite(geometry.inner_radius) || geometry.inner_radius < 0.0f ||
                geometry.inner_radius >= geometry.outer_radius ||
                !std::isfinite(geometry.edge_width) || geometry.edge_width <= 0.0f ||
                std::abs(geometry.inner_radius -
                         std::max(0.0f, geometry.outer_radius - geometry.edge_width)) > 0.0001f ||
                !std::isfinite(source.start_size_min) ||
                std::abs(source.start_size_min -
                         (geometry.outer_radius + geometry.edge_width)) > 0.0001f ||
                !std::isfinite(source.stretch) ||
                std::abs(source.stretch - geometry.outer_radius / source.start_size_min) > 0.001f ||
                geometry.mask_slice != 0xffffffffu || geometry.mask_strength != 0.0f ||
                !geometry.gap_angles_radians.empty()))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored polar rune.");
            const float authored_radius = geometry.outer_radius;
            const float authored_edge = geometry.edge_width;
            const float authored_extent = axial_fracture
                ? std::max(geometry.range * 0.5f + authored_edge,
                           std::max(authored_radius, authored_edge * 3.0f))
                : repeating_chevron
                    ? std::max(geometry.half_length, geometry.half_width) + authored_edge
                    : authored_radius + authored_edge;
            const float authored_direction_length = std::hypot(geometry.direction.x,
                                                                geometry.direction.z);
            const bool invalid_direction = (axial_fracture || repeating_chevron) &&
                (!std::isfinite(authored_direction_length) ||
                 authored_direction_length < 0.999f ||
                 authored_direction_length > 1.001f ||
                 !std::isfinite(geometry.direction.y) || geometry.direction.y != 0.0f);
            const bool invalid_chevron = repeating_chevron &&
                (!std::isfinite(geometry.half_width) || geometry.half_width <= 0.0f ||
                 !std::isfinite(geometry.half_length) || geometry.half_length <= 0.0f ||
                 !std::isfinite(geometry.spacing) || geometry.spacing <= 0.0f ||
                 geometry.spokes != 7);
            if (authored_additive_shape && (!input.additive ||
                source.primitive != VfxPrimitive::ExactRing ||
                !std::isfinite(geometry.progress) || geometry.progress < 0.0f ||
                geometry.progress >= 1.0f ||
                !std::isfinite(geometry.animation_phase) ||
                (!repeating_chevron && (!std::isfinite(authored_radius) ||
                                        authored_radius <= 0.0f)) ||
                !std::isfinite(authored_edge) || authored_edge <= 0.0f ||
                !std::isfinite(authored_extent) ||
                !std::isfinite(source.start_size_min) ||
                source.start_size_min < authored_extent ||
                (broken_hex && (!std::isfinite(geometry.gap_half_angle_radians) ||
                    geometry.gap_half_angle_radians <= 0.0f ||
                    geometry.gap_half_angle_radians >= std::numbers::pi_v<float> / 3.0f)) ||
                (broken_crown && (!std::isfinite(geometry.inner_radius) ||
                    geometry.inner_radius <= 0.0f ||
                    geometry.inner_radius >= authored_radius)) ||
                invalid_chevron || invalid_direction ||
                (axial_fracture && (!std::isfinite(geometry.range) ||
                    geometry.range <= 0.0f))))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored additive ground geometry.");
            if (boss_signature && (source.primitive != VfxPrimitive::ExactRing ||
                !std::isfinite(geometry.progress) || geometry.progress < 0.0f ||
                geometry.progress >= 1.0f || !std::isfinite(geometry.edge_width) ||
                geometry.edge_width <= 0.0f ||
                (geometry.shape == VfxGroundShape::Circle &&
                 (!std::isfinite(geometry.outer_radius) || geometry.outer_radius <= 0.0f)) ||
                !std::isfinite(source.start_size_min) ||
                source.start_size_min < (geometry.shape == VfxGroundShape::ConeBorder
                    ? geometry.range : geometry.outer_radius) + geometry.edge_width))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored boss signature.");
            if ((geometry.shape != VfxGroundShape::Circle && !line && !cone && !annulus && !safe_sector && !hex && !authored_additive_shape && !state_ring && !polar_rune) ||
                ((line || cone) && (!std::isfinite(direction_length) || direction_length < 0.999f || direction_length > 1.001f ||
                          !std::isfinite(geometry.direction.y) || geometry.direction.y != 0.0f ||
                          (line && (!std::isfinite(geometry.half_width) || geometry.half_width <= 0.0f ||
                                    !std::isfinite(geometry.half_length) || geometry.half_length <= 0.0f)) ||
                          (cone && (!std::isfinite(geometry.range) || geometry.range <= 0.0f ||
                                    !std::isfinite(geometry.half_angle_radians) || geometry.half_angle_radians <= 0.0f ||
                                    geometry.half_angle_radians > std::numbers::pi_v<float>)) ||
                          !std::isfinite(geometry.edge_width) || geometry.edge_width < 0.0f ||
                          ((geometry.shape == VfxGroundShape::LineBorder || geometry.shape == VfxGroundShape::ConeBorder) && geometry.edge_width == 0.0f) ||
                          !std::isfinite(geometry.spacing) || geometry.spacing <= 0.0f ||
                          !std::isfinite(geometry.scroll))))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored line geometry.");
            const auto finite_color = std::isfinite(source.start_color.x) &&
                std::isfinite(source.start_color.y) &&
                std::isfinite(source.start_color.z) &&
                std::isfinite(source.start_color.w);
            if (input.effect_handle == kInvalidVfxEffectHandle ||
                source.renderer != VfxRenderer::Ground ||
                (source.primitive != VfxPrimitive::ExactRing &&
                 source.primitive != VfxPrimitive::LowFrequencyFill) || source.count != 1 ||
                (input.additive && ((geometry.shape != VfxGroundShape::Circle && !hex && !boss_signature && !authored_additive_shape) ||
                                    source.primitive != VfxPrimitive::ExactRing)) ||
                (input.additive && geometry.shape == VfxGroundShape::Circle &&
                 geometry.animation_phase != 0.0f && !boss_signature) ||
                !std::isfinite(source.position.x) ||
                !std::isfinite(source.position.y) ||
                !std::isfinite(source.position.z) ||
                !std::isfinite(source.start_size_min) ||
                source.start_size_min <= 0.0f ||
                !std::isfinite(source.stretch) ||
                (!line && !cone && !annulus && !safe_sector && !axial_fracture &&
                 !repeating_chevron &&
                 source.primitive == VfxPrimitive::ExactRing &&
                 (source.stretch <= 0.0f || source.stretch >= 1.0f)) ||
                !finite_color || !std::isfinite(input.motion.rate_hz) ||
                !std::isfinite(input.motion.amplitude) ||
                !std::isfinite(input.motion.inset_fraction) ||
                input.motion.rate_hz < 0.0f || input.motion.amplitude < 0.0f ||
                input.motion.amplitude > 1.0f || input.motion.inset_fraction < 0.0f ||
                input.motion.inset_fraction > 1.0f ||
                !std::isfinite(input.hdr) || input.hdr < 0.0f ||
                input.gradient_row >= this->vfx_gradient_rows)
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                       "Invalid authored ground command.");
    }
    for (const auto &source : input.vfx.lights)
    {
        if (!std::isfinite(source.position.x) || !std::isfinite(source.position.y) || !std::isfinite(source.position.z) ||
            !std::isfinite(source.radius) || source.radius <= 0 || !std::isfinite(source.intensity) || source.intensity < 0 ||
            !std::isfinite(source.linear_rgb.x) || !std::isfinite(source.linear_rgb.y) || !std::isfinite(source.linear_rgb.z) ||
            source.linear_rgb.x < 0 || source.linear_rgb.y < 0 || source.linear_rgb.z < 0 ||
            static_cast<std::uint32_t>(source.quality) > 2)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored VFX light.");
    }
    for (const auto &source : input.vfx.distortions)
    {
        const auto finite = [](Float3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
        if (!finite(source.position) || !finite(source.direction) || !std::isfinite(source.radius) || source.radius <= 0 ||
            !std::isfinite(source.strength) || source.strength < 0 || !std::isfinite(source.alpha) || source.alpha < 0 || source.alpha > 1 ||
            !std::isfinite(source.normalized_age) || source.normalized_age < 0 || source.normalized_age > 1 ||
            static_cast<std::uint32_t>(source.shape) > 6 || static_cast<std::uint32_t>(source.quality) > 2)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored distortion command.");
        const bool cone = source.shape == VfxDistortionShape::ConeSector;
        const bool annulus = source.shape == VfxDistortionShape::GappedAnnulus;
        const float horizontal_direction = std::hypot(source.direction.x,source.direction.z);
        if ((cone && (!std::isfinite(source.half_angle_degrees) || source.half_angle_degrees <= 0 ||
                      source.half_angle_degrees > 180 || !std::isfinite(horizontal_direction) ||
                      horizontal_direction <= .0001f || source.inner_radius != 0 ||
                      source.gap_half_width_degrees != 0 || !source.gap_angles_degrees.empty())) ||
            (annulus && (!std::isfinite(source.inner_radius) || source.inner_radius < 0 ||
                         source.inner_radius >= source.radius || !std::isfinite(source.gap_half_width_degrees) ||
                         source.gap_half_width_degrees < 0 || source.gap_half_width_degrees > 180 ||
                         source.half_angle_degrees != 0 ||
                         (!source.gap_angles_degrees.empty() && source.gap_half_width_degrees == 0) ||
                         std::any_of(source.gap_angles_degrees.begin(),source.gap_angles_degrees.end(),
                             [](float angle){return !std::isfinite(angle); }))) ||
            (!cone && !annulus && (source.inner_radius != 0 || source.half_angle_degrees != 0 ||
                                   source.gap_half_width_degrees != 0 || !source.gap_angles_degrees.empty())))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid exact distortion geometry.");
    }
    for (const auto &source : input.vfx.decals)
    {
        const auto finite = [](Float3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
        const bool fracture = source.kind == VfxDecalKind::Fracture;
        if (!finite(source.position) || !finite(source.direction) || !finite(source.linear_rgb) ||
            source.linear_rgb.x < 0 || source.linear_rgb.y < 0 || source.linear_rgb.z < 0 ||
            !std::isfinite(source.radius) || source.radius <= 0 || !std::isfinite(source.hdr) || source.hdr < 0 ||
            !std::isfinite(source.alpha) || source.alpha < 0 || source.alpha > 1 || !std::isfinite(source.normalized_age) ||
            source.normalized_age < 0 || source.normalized_age > 1 || !std::isfinite(source.edge_glow) || source.edge_glow < 0 || source.edge_glow > 1 ||
            source.gradient_row >= this->vfx_gradient_rows || source.texture_slice >= (fracture ? 4u : 8u) ||
            static_cast<std::uint32_t>(source.kind) > 1 || static_cast<std::uint32_t>(source.quality) > 2 ||
            (fracture && source.voronoi_cells == 0))
            return Result::Failure(ErrorCode::InvalidArgument,"hs_renderer_d3d12","Invalid authored projected decal.");
    }
    for (const auto &source : input.vfx.particle_spawns)
    {
        if (source.mesh_index > 7)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "VFX mesh selector exceeds atlas range");
        if (source.authored_ballistic && (source.renderer != VfxRenderer::Mesh || source.mesh_index == 0 || source.count != 1 ||
            !std::isfinite(source.position.x) || !std::isfinite(source.position.y) || !std::isfinite(source.position.z) ||
            !std::isfinite(source.lifetime_min) || !std::isfinite(source.lifetime_max) || source.lifetime_min <= 0 || source.lifetime_max < source.lifetime_min ||
            !std::isfinite(source.start_size_min) || !std::isfinite(source.start_size_max) || source.start_size_min <= 0 || source.start_size_max < source.start_size_min ||
            !std::isfinite(source.end_size_min) || !std::isfinite(source.end_size_max) || source.end_size_min < 0 || source.end_size_max < source.end_size_min ||
            !std::isfinite(source.start_color.x) || !std::isfinite(source.start_color.y) || !std::isfinite(source.start_color.z) || !std::isfinite(source.start_color.w) ||
            !std::isfinite(source.end_color.x) || !std::isfinite(source.end_color.y) || !std::isfinite(source.end_color.z) || !std::isfinite(source.end_color.w) ||
            !std::isfinite(source.rotation_min) || !std::isfinite(source.rotation_max) || !std::isfinite(source.angular_velocity_min) || !std::isfinite(source.angular_velocity_max) ||
            !std::isfinite(source.authored_ground_y + source.authored_collision_radius) ||
            !std::isfinite(source.authored_initial_velocity.x) || !std::isfinite(source.authored_initial_velocity.y) || !std::isfinite(source.authored_initial_velocity.z) ||
            !std::isfinite(source.authored_drag) || source.authored_drag < 0 ||
            !std::isfinite(source.authored_bounce) || source.authored_bounce < 0 || source.authored_bounce > 1 ||
            !std::isfinite(source.authored_ground_y) || !std::isfinite(source.authored_collision_radius) || source.authored_collision_radius < 0 ||
            !std::isfinite(source.authored_hdr) || source.authored_hdr <= 0 || source.authored_gradient_row >= this->vfx_gradient_rows ||
            !std::isfinite(source.authored_birth_fraction) || source.authored_birth_fraction < 0 || source.authored_birth_fraction >= 1 ||
            !std::isfinite(source.gravity)))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored ballistic command.");
    }
    for (const auto &command : input.vfx.screen)
    {
        const auto &v = command;
        const float values[]{v.color.x,v.color.y,v.color.z,v.hdr,v.alpha,v.gradient_age,v.envelope,v.center_clear,v.edge_power,
            v.source_direction.x,v.source_direction.y,v.source_direction.z};
        if(!std::ranges::all_of(values,[](float x){return std::isfinite(x);})||v.color.x<0||v.color.y<0||v.color.z<0||v.hdr<0||v.alpha<0||v.alpha>1||
           v.gradient_age<0||v.gradient_age>1||v.envelope<0||v.envelope>1||v.center_clear<0||v.center_clear>=1||v.edge_power<=0||
           v.gradient_row>=vfx_gradient_rows||v.shape>VfxScreenShape::DirectionalArc)
            return ViewFailure("Screen overlay command violates its analytic opacity contract.");
    }
    if (auto r = ValidateVfxHeroVolumes(input.vfx.volumes, vfx_gradient_rows); !r) return r;
    std::uint64_t gaps{};
    for (const auto &ground : input.vfx.ground) gaps += ground.geometry.gap_angles_radians.size();
    for (const auto &distortion : input.vfx.distortions)
        if (distortion.shape == VfxDistortionShape::GappedAnnulus) gaps += distortion.gap_angles_degrees.size();
    if (gaps > UINT32_MAX || input.vfx.ground.size() > UINT32_MAX || input.vfx.flashes.size() > UINT32_MAX || input.vfx.meshes.size() > UINT32_MAX)
        return ViewFailure("Authored VFX upload counts exceed their native index capacity.");
    auto snapshot = input.snapshots.has_current ? input.snapshots.current : RenderSnapshot{};
    if (input.scene) snapshot.static_scene = input.scene->lease;
    std::vector<RenderInstance> render_instances(snapshot.instances.begin(), snapshot.instances.end());
    for (const auto &command : input.vfx.entity_surfaces)
    {
        if (!command.has_retained_instance || std::ranges::any_of(render_instances,
                [&](const auto &instance) { return instance.stable_id == command.render_instance_id; })) continue;
        const auto &instance = command.retained_instance;
        if (instance.stable_id != command.render_instance_id ||
            instance.mesh < RenderMesh::BossFiveMinute || instance.mesh > RenderMesh::BossFinal ||
            instance.asset_id.value == 0 || instance.material_id.value == 0 ||
            (instance.animation_clip_id.value == 0 && !command.has_retained_animation_pose))
            return ViewFailure("Entity surface retention requires its actual boss mesh and captured pose.");
        render_instances.push_back(instance);
    }
    std::vector<VfxEntitySurfaceCommand> admitted_surfaces;
    for (const auto &command : input.vfx.entity_surfaces)
        if (std::ranges::any_of(render_instances, [&](const auto &instance) {
                return instance.stable_id == command.render_instance_id &&
                    instance.mesh >= RenderMesh::BossFiveMinute && instance.mesh <= RenderMesh::BossFinal;
            })) admitted_surfaces.push_back(command);
    EntitySurfaceFrame surface;
    if (auto r = surface.Build(admitted_surfaces); !r) return r;
    auto extractor_validation = dynamic_scene_extractor;
    if (!extractor_validation.Adopt(render_instances, false))
        return ViewFailure("Dynamic render scene identities are invalid or duplicated.");
    std::array<DirectX::XMFLOAT4X4, kMaxCharacterBones> palette;
    for (const auto &instance : render_instances)
    {
        if (!instance.animation_clip_id.value) continue;
        if (instance.mesh != RenderMesh::Archer) return ViewFailure("Explicit named animation requires a loaded skeleton-bound character extension.");
        if (auto r = SampleNamedCharacterAnimation(archer_named_animations, instance,
            std::span(palette.data(), archer_bone_count)); !r) return r;
    }
    if (snapshot.static_scene)
    {
        std::uint64_t output_count{};
        for (const auto &instance : snapshot.static_scene->instances)
        {
            const bool environment = render_data::IsEnvironmentMesh(instance.mesh);
            if (!environment && instance.mesh != RenderMesh::Ground && instance.mesh != RenderMesh::DirtPatch)
                return ViewFailure("Unsupported mesh in static scene.");
            output_count += environment ? 3u : 1u;
        }
        if (output_count > UINT32_MAX / 4u) return ViewFailure("Static visibility output index overflow.");
    }
    if (render_instances.size() > UINT32_MAX / 12u) return ViewFailure("Dynamic visibility output index overflow.");
    auto ribbon_validation = ribbon_state;
    std::string ribbon_error;
    const auto &camera = input.views.front();
    const Float3 camera_scene_position{static_cast<float>(camera.camera.position.x-camera.scene_origin.x),
        static_cast<float>(camera.camera.position.y-camera.scene_origin.y), static_cast<float>(camera.camera.position.z-camera.scene_origin.z)};
    if (!ribbon_validation.Update(input.vfx.ribbons.sources, snapshot.header.tick, snapshot.header.session_id,
        input.vfx.ribbons.catalog_generation, config.ribbon_history_points, 1.0f, camera_scene_position, ribbon_error))
        return ViewFailure(ribbon_error);
    return Result::Success();
}

void D3D12Renderer::Impl::ExchangeView(OwnedView &view) noexcept
{
    using std::swap;
    swap(skin_cache, view.skin_history);
    if (cluster_runtime) swap(static_cast<ClusterViewRuntime &>(*cluster_runtime), view.cluster_view);
    swap(previous_skin_deformation, view.previous_skin_deformation);
    swap(previous_instances, view.previous_instances);
    swap(previous_grass_benders, view.previous_grass_benders);
    swap(previous_grass_state, view.previous_grass_state);
    swap(view_session_id, view.view_session_id);
    swap(width, view.width);
    swap(height, view.height);
    swap(render_width, view.render_width);
    swap(render_height, view.render_height);
    swap(last_presented_index, view.last_presented_index);
    swap(exposure_read_index, view.exposure_read_index);
    swap(temporal_read_index, view.temporal_read_index);
    swap(hzb_read_index, view.hzb_read_index);
    swap(exposure_initialized, view.exposure_initialized);
    swap(view_targets_initialized, view.view_targets_initialized);
    swap(temporal_history_valid, view.temporal_history_valid);
    swap(temporal_targets_initialized, view.temporal_targets_initialized);
    swap(hzb_history_valid, view.hzb_history_valid);
    swap(material_sidecars_initialized, view.material_sidecars_initialized);
    swap(visibility_targets_initialized, view.visibility_targets_initialized);
    swap(transient_textures_common, view.transient_textures_common);
    swap(brixelizer_history_valid, view.brixelizer_history_valid);
    swap(exposure_sample_time, view.exposure_sample_time);
    swap(render_submission_time, view.render_submission_time);
    swap(previous_view_projection, view.previous_view_projection);
    swap(previous_unjittered_view_projection, view.previous_unjittered_view_projection);
    swap(previous_projection_depth, view.previous_projection_depth);
    swap(previous_jitter_uv, view.previous_jitter_uv);
    swap(temporal_previous_eye, view.temporal_previous_eye);
    swap(temporal_previous_fov, view.temporal_previous_fov);
    swap(previous_scene_origin, view.previous_scene_origin);
    swap(previous_view_origin, view.previous_view_origin);
    swap(previous_origins_valid, view.previous_origins_valid);
    swap(frame_number, view.frame_number);
    swap(dynamic_resolution_controller, view.dynamic_resolution_controller);
    swap(completed_gpu_timings, view.completed_gpu_timings);
    swap(dynamic_resolution_sample_id, view.dynamic_resolution_sample_id);
    swap(dynamic_lod_states, view.dynamic_lod_states);
    swap(lod_session_epoch, view.lod_session_epoch);
    swap(previous_volumetric_camera, view.previous_volumetric_camera);
    swap(previous_shadow_casters, view.previous_shadow_casters);
    swap(previous_shadow_caster_nodes, view.previous_shadow_caster_nodes);
    swap(previous_geometry_page_versions, view.previous_geometry_page_versions);
    swap(reconstruction_provider, view.reconstruction_provider);
    swap(reconstruction_plan, view.reconstruction_plan);
    swap(sdk_camera, view.sdk_camera);
    swap(sdk_frame_identity, view.sdk_frame_identity);
    swap(display_state, view.display_state);
    swap(exposure_values, view.exposure_values);
    swap(exposure_histogram, view.exposure_histogram);
    swap(initial_exposure, view.initial_exposure);
    swap(depth, view.targets.depth);
    swap(shadow, view.targets.shadow);
    swap(gbuffer_base, view.targets.gbuffer_base);
    swap(gbuffer_normal, view.targets.gbuffer_normal);
    swap(gbuffer_metadata, view.targets.gbuffer_metadata);
    swap(gbuffer_material, view.targets.gbuffer_material);
    swap(gbuffer_motion, view.targets.gbuffer_motion);
    swap(gbuffer_surface_history, view.targets.gbuffer_surface_history);
    swap(material_sidecars, view.targets.material_sidecars);
    swap(visibility_ids, view.targets.visibility_ids);
    swap(visibility_front_face, view.targets.visibility_front_face);
    swap(visibility_pixels, view.targets.visibility_pixels);
    swap(visibility_counts, view.targets.visibility_counts);
    swap(visibility_dispatch_args, view.targets.visibility_dispatch_args);
    swap(hdr_color, view.targets.hdr_color);
    swap(subsurface_diffuse, view.targets.subsurface_diffuse);
    swap(environment_specular, view.targets.environment_specular);
    swap(indirect_diffuse, view.targets.indirect_diffuse);
    swap(oit_accumulation, view.targets.oit_accumulation);
    swap(oit_revealage, view.targets.oit_revealage);
    swap(post_a, view.targets.post_a);
    swap(post_b, view.targets.post_b);
    swap(hdr_capture_source, view.hdr_capture_source);
    swap(hdr_capture_extent, view.hdr_capture_extent);
    swap(hdr_capture_valid, view.hdr_capture_valid);
    swap(hdr_capture_pre_exposed, view.hdr_capture_pre_exposed);
    swap(hdr_capture_submission_id, view.hdr_capture_submission_id);
    swap(hdr_capture_package_id, view.hdr_capture_package_id);
    swap(hdr_capture_snapshot_tick, view.hdr_capture_snapshot_tick);
    swap(hdr_capture_source_name, view.hdr_capture_source_name);
    swap(temporal_history, view.targets.temporal_history);
    swap(temporal_normal_history, view.targets.temporal_normal_history);
    swap(temporal_material_history, view.targets.temporal_material_history);
    swap(scene_work, view.targets.scene_work);
    swap(display_linear, view.targets.display_linear);
    swap(composite_hdr, view.targets.composite_hdr);
    swap(hzb_history, view.targets.hzb_history);
    swap(hzb_layout, view.targets.hzb_layout);
    ambient_occlusion.SwapState(view.ambient_occlusion);
    adaptive_shading.SwapState(view.adaptive_shading);
    subsurface.SwapState(view.subsurface);
    screen_reflections.SwapState(view.screen_reflections);
    hero_transparency.SwapState(view.hero_transparency);
    hdr_post.SwapState(view.hdr_post);
    screen_overlay.SwapState(view.screen_overlay);
    brixelizer_gi.SwapState(view.brixelizer_gi);
    virtual_shadows.SwapState(view.virtual_shadows);
    local_shadows.SwapState(view.local_shadows);
    volumetrics.SwapState(view.volumetrics);
    light_list.SwapState(view.light_list);
    decal_surface.SwapState(view.decal_surface);
    reconstruction_guides.SwapState(view.reconstruction_guides);
    swap(view_state, view.state);
    swap(back_buffers, view.outputs);
    swap(back_buffer_completion, view.output_completion);
    swap(ui_textures, view.ui.textures);
    swap(ui_surfaces, view.ui.surfaces);
    swap(ui_footprint, view.ui.footprint);
    swap(ui_rows, view.ui.rows);
    swap(ui_raster_width, view.ui.raster_width);
    swap(ui_raster_height, view.ui.raster_height);
    swap(ui_row_bytes, view.ui.row_bytes);
    swap(ui_upload_bytes, view.ui.upload_bytes);
    swap(ui_raster_models, view.ui_models);
    swap(ui_dirty, view.ui_dirty);
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        swap(frames[i].ui_upload, view.ui.uploads[i]);
        swap(frames[i].ui_mapped, view.ui.mapped[i]);
        swap(frames[i].ui_initialized, view.ui_initialized[i]);
        transient_pools[i].Swap(view.targets.transient_pools[i]);
    }
    if (static_gpu_scene) swap(static_gpu_scene->lod_states, view.static_lod_states);
}

Result D3D12Renderer::Impl::ActivateView(ViewId id)
{
    if (id == active_view_id) return Result::Success();
    const auto next = owned_views.find(id.value);
    if (next == owned_views.end() || !next->second->ready)
        return ViewFailure("View has no prepared GPU state.");
    if (auto drained = WaitForGpu(); !drained) return drained;
    if (cluster_runtime && !next->second->cluster_view.descriptors)
        if (auto prepared = CreateClusterViewResources(next->second->cluster_view); !prepared) return prepared;
    std::vector<ID3D12CommandQueue *> queues;
    for (const auto hint : {QueueHint::Direct, QueueHint::Compute, QueueHint::Copy})
        if (auto *queue = graph_queue_executor.NativeQueue(hint)) queues.push_back(queue);
    if (auto reusable = sdk_presentation.BeforeReuse(queues); !reusable) return reusable;
    // Native selectors retain contexts; they do not destroy/recreate history.
    if (streamline_session)
        if (auto selected = streamline_session->SelectReconstructionViewAfterCompletion(next->second->native_viewport_id); !selected) return selected;
    if (fsr_session)
        if (auto selected = fsr_session->SelectReconstructionViewAfterCompletion(next->second->native_viewport_id); !selected) return selected;
    ExchangeView(*owned_views.at(active_view_id.value));
    ExchangeView(*next->second);
    active_view_id = id;
    active_view_role = next->second->role;
    active_native_viewport_id = next->second->native_viewport_id;
    WriteOptionTargetDescriptors();
    auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (const auto &output : back_buffers)
    {
        device->CreateRenderTargetView(output.Get(), nullptr, rtv);
        rtv.ptr += rtv_stride;
    }
    // UI descriptors contain view allocations and must follow the exchange.
    for (std::uint32_t frame = 0; frame < kFrameCount; ++frame)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC ui{};
        ui.Format = DXGI_FORMAT_B8G8R8A8_UNORM; ui.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        ui.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; ui.Texture2D.MipLevels = 1;
        for (const auto table : {0u, kToneMapInputDescriptorOffset, kFxaaInputDescriptorOffset})
        {
            auto handle = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += (static_cast<SIZE_T>(frame) * kTextureDescriptorCount + table + 9) * srv_stride;
            device->CreateShaderResourceView(ui_textures[frame].resource.Get(), &ui, handle);
        }
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::PrepareDecalSurface(const RendererConfig &candidate_config,
    ViewExtent capacity, bool admitted_work, DecalSurfaceCandidate &candidate) const
{
    return decal_surface.Prepare(device.Get(), allocator, root_signature.Get(), capacity, kFrameCount,
        admitted_work, [&](std::string_view name, std::vector<std::byte> &bytes) {
            std::filesystem::path path;
            if (auto r = candidate_config.ResolveProduct("render/shader/" + std::string(name), path); !r) return r;
            return ReadBinary(path, bytes);
        }, candidate);
}

Result D3D12Renderer::Impl::PrepareOwnedView(const RenderViewInput &input, std::unique_ptr<OwnedView> &output, bool decal_work)
{
    auto candidate = std::make_unique<OwnedView>();
    candidate->role = input.role;
    candidate->native_viewport_id = next_native_viewport_id++;
    candidate->width = input.output_extent.width; candidate->height = input.output_extent.height;
    candidate->render_width = input.render_active.width; candidate->render_height = input.render_active.height;
    candidate->configuration_revision = view_configuration_revision;
    candidate->content_generation = resource_generation_id;
    candidate->pipeline_revision = pipeline_revision;
    (void)UpdateViewState(candidate->state, input.output_extent, input.render_capacity, input.render_active);
    auto &v = *candidate;
    ViewExtent native_capacity;
    if (auto r = ResolveNativeReconstruction(config, input.output_extent, v.reconstruction_plan, native_capacity); !r) return r;
    if (auto r = PrepareReconstructionProvider(config.reconstruction_request ? &*config.reconstruction_request : nullptr,
            config.dynamic_resolution, config.dynamic_min_percent, config.dynamic_max_percent, input.output_extent,
            v.reconstruction_plan, native_capacity, v.reconstruction_provider, &config); !r) return r;
    if (auto r = PrepareOptionTargets(v.targets, input.render_capacity.width, input.render_capacity.height,
            config.shadow_resolution, config.bloom, input.output_extent.width, input.output_extent.height,
            UsesInternalResolveTargets(v.reconstruction_plan), config.outline,
            RequiredLightingAuxiliaries(config)); !r) return r;
    candidate->transient_textures_common = enhanced;
    if (auto r = PrepareUiResources(v.ui, v.width, v.height); !r) return r;
    D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = v.width; desc.Height = v.height;
    desc.DepthOrArraySize = desc.MipLevels = 1; desc.Format = display_state.format; desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for (std::size_t i = 0; i < kFrameCount; ++i)
    {
        if (auto r = CreateAllocation(v.offscreen_allocations[i], allocation, desc, D3D12_RESOURCE_STATE_COMMON); !r) return r;
        v.outputs[i] = v.offscreen_allocations[i].resource;
    }
    for (auto &value : v.exposure_values)
        if (auto r = CreateAllocation(value, allocation, BufferDescription(2*sizeof(float),
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), D3D12_RESOURCE_STATE_COPY_DEST); !r) return r;
    if (auto r = CreateAllocation(v.exposure_histogram, allocation, BufferDescription(256*sizeof(std::uint32_t),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), D3D12_RESOURCE_STATE_UNORDERED_ACCESS); !r) return r;
    allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    if (auto r = CreateAllocation(v.initial_exposure, allocation, BufferDescription(2*sizeof(float)),
            D3D12_RESOURCE_STATE_GENERIC_READ); !r) return r;
    void *mapped{}; const D3D12_RANGE no_read{};
    const auto map_result = v.initial_exposure.resource->Map(0, &no_read, &mapped);
    if (FAILED(map_result)) return HResultFailure("Map per-view initial exposure", map_result);
    constexpr float initial[2]{1,1}; std::memcpy(mapped, initial, sizeof(initial));
    v.initial_exposure.resource->Unmap(0, nullptr);
    const auto completed = graph_queue_executor.Completed();
    const auto capacity = input.render_capacity;
    AdaptiveShadingCandidate adaptive;
    AmbientOcclusionCandidate ambient;
    SubsurfaceCandidate diffusion; ScreenReflectionsCandidate reflections;
    HeroTransparencyCandidate transparency; HdrPostCandidate hdr; ScreenOverlayCandidate overlay;
    DecalSurfaceCandidate decals;
    LightListCandidate lights; BrixelizerCandidate brixelizer;
    VirtualShadowCandidate shadows; AdditionalShadowPrepared additional; VolumetricPrepared volume;
    if (auto r = PrepareAdaptiveShading(config, capacity, adaptive); !r) return r;
    if (auto r = PrepareAmbientOcclusion(config, capacity, ambient, adaptive.Status().compute_ready); !r) return r;
    if (auto r = PrepareSurfaceLighting(config, capacity, diffusion, reflections); !r) return r;
    if (auto r = PrepareHeroTransparency(config, capacity, transparency); !r) return r;
    if (auto r = PrepareHdrPost(config, capacity, input.output_extent, hdr); !r) return r;
    if (auto r = PrepareScreenOverlay(config, capacity, overlay); !r) return r;
    if (auto r = PrepareLightList(config, capacity, lights); !r) return r;
    if (auto r = PrepareDecalSurface(config, capacity, decal_work, decals); !r) return r;
    if (auto r = PrepareBrixelizerGi(config, input.render_active, brixelizer); !r) return r;
    if (auto r = PrepareVirtualShadows(config, capacity, shadows); !r) return r;
    if (auto r = PrepareAdditionalShadows(config, capacity, additional); !r) return r;
    if (auto r = PrepareVolumetrics(config, capacity, volume); !r) return r;
    if (auto r = v.adaptive_shading.Commit(std::move(adaptive), completed); !r) return r;
    if (auto r = v.ambient_occlusion.Commit(std::move(ambient), completed); !r) return r;
    if (auto r = v.subsurface.Commit(std::move(diffusion), completed); !r) return r;
    if (auto r = v.screen_reflections.Commit(std::move(reflections), completed); !r) return r;
    if (auto r = v.hero_transparency.Commit(std::move(transparency), completed); !r) return r;
    if (auto r = v.hdr_post.Commit(std::move(hdr), completed); !r) return r;
    if (auto r = v.screen_overlay.Commit(std::move(overlay), completed); !r) return r;
    if (auto r = v.light_list.Commit(std::move(lights), completed); !r) return r;
    if (auto r = v.decal_surface.Commit(std::move(decals), completed); !r) return r;
    if (auto r = v.brixelizer_gi.Commit(std::move(brixelizer), completed); !r) return r;
    if (auto r = v.virtual_shadows.Commit(std::move(shadows), completed); !r) return r;
    if (auto r = v.local_shadows.Commit(std::move(additional.local), completed); !r) return r;
    if (auto r = v.volumetrics.Commit(std::move(volume.service), completed); !r) return r;
    if (v.reconstruction_provider && (native_capacity.width > capacity.width || native_capacity.height > capacity.height ||
        input.render_active.width < v.reconstruction_plan.input_minimum.width ||
        input.render_active.height < v.reconstruction_plan.input_minimum.height ||
        input.render_active.width > v.reconstruction_plan.input_maximum.width ||
        input.render_active.height > v.reconstruction_plan.input_maximum.height))
        return ViewFailure("Requested view capacity/active extent does not cover the selected native reconstruction domain.");
    ReconstructionGuidesCandidate guides;
    if (v.reconstruction_provider)
    {
        v.reconstruction_provider->SetViewportId(v.native_viewport_id);
        if (auto r = PrepareReconstructionGuides(config, capacity, input.output_extent, guides,
                v.reconstruction_provider->NeedsCompositionGuide()); !r) return r;
        if (auto r = v.reconstruction_guides.Commit(std::move(guides), completed); !r) return r;
    }
    v.reconstruction_plan.render_active = input.render_active;
    v.display_state = display_state;
    v.ready = true;
    output = std::move(candidate);
    return Result::Success();
}

void D3D12Renderer::Impl::RecordViewHistoryKeys(ViewId id)
{
    auto &view = *owned_views.at(id.value);
    const auto state = id == active_view_id ? view_state : view.state;
    view.history_keys = {
        {id, ViewHistoryFeature::Temporal, "pre_exposed_hdr", DXGI_FORMAT_R16G16B16A16_FLOAT, state.output_extent, view.history_generation},
        {id, ViewHistoryFeature::Temporal, "normal", DXGI_FORMAT_R16G16_SNORM, state.output_extent, view.history_generation},
        {id, ViewHistoryFeature::Temporal, "material_identity", DXGI_FORMAT_R32G32_UINT, state.output_extent, view.history_generation},
        {id, ViewHistoryFeature::Exposure, "adapted_pre_exposure", DXGI_FORMAT_R32G32_FLOAT, {1,1}, view.history_generation},
        {id, ViewHistoryFeature::Occlusion, "hzb", DXGI_FORMAT_R32_FLOAT, state.render_capacity, view.history_generation},
        {id, ViewHistoryFeature::NativeReconstruction, "native_context", 0, state.render_capacity, view.history_generation}};
    view.history_resources.clear();
    const auto bind = [&](std::size_t ordinal, const auto &resources) {
        view.history_resources.emplace(view.history_keys[ordinal],
            std::array<ID3D12Resource *,2>{resources[0].resource.Get(), resources[1].resource.Get()});
    };
    if (id == active_view_id)
    {
        bind(0, temporal_history); bind(1, temporal_normal_history); bind(2, temporal_material_history);
        bind(3, exposure_values); bind(4, hzb_history);
    }
    else
    {
        bind(0, view.targets.temporal_history); bind(1, view.targets.temporal_normal_history);
        bind(2, view.targets.temporal_material_history); bind(3, view.exposure_values); bind(4, view.targets.hzb_history);
    }
}

void D3D12Renderer::Impl::InvalidateCanonicalDeformationAfterCompletion() noexcept
{
    hdr_capture_source.Reset();
    hdr_capture_valid = false;
    for (auto &[id, view] : owned_views)
    {
        view->hdr_capture_source.Reset();
        view->hdr_capture_valid = false;
    }
    previous_skin_deformation.reset();
    previous_instances.reset();
    skin_cache.Clear();
    for (auto &[id, view] : owned_views)
    {
        if (id == active_view_id.value) continue;
        view->previous_skin_deformation.reset();
        view->previous_instances.reset();
        view->skin_history.Clear();
    }
    canonical_skin_current.reset();
    canonical_instance_current.reset();
    canonical_skin_frames.clear();
    for (auto &frame : frames) frame.skin_cache_valid = false;
}

Result D3D12Renderer::Impl::ResetOwnedViewAfterCompletion(OwnedView &view)
{
    ResetClusterViewResources(view.cluster_view);
    if (view.reconstruction_provider)
        if (auto r = view.reconstruction_provider->ResetAfterCompletion(); !r) return r;
    if (auto r = view.ambient_occlusion.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.adaptive_shading.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.subsurface.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.screen_reflections.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.hero_transparency.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.hdr_post.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.screen_overlay.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.brixelizer_gi.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.virtual_shadows.Commit(VirtualShadowCandidate{}, graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.local_shadows.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.volumetrics.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.light_list.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.decal_surface.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.reconstruction_guides.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    return Result::Success();
}

Result D3D12Renderer::Impl::ResetOwnedViewsAfterCompletion()
{
    for (auto &[id, view] : owned_views)
        if (id != active_view_id.value)
            if (auto r = ResetOwnedViewAfterCompletion(*view); !r) return r;
    for (auto &candidate : unresolved_view_candidates)
        if (auto r = ResetOwnedViewAfterCompletion(*candidate); !r) return r;
    unresolved_view_candidates.clear();
    owned_views.clear();
    return Result::Success();
}

RenderViewInput D3D12Renderer::MainViewInput(const CameraView &source) const
{
    auto camera = source;
    const auto &config = impl_->config;
    const bool family = config.slime_family_preview_count != 0;
    const bool monster = config.monster_preview_asset < 6 || family;
    if (config.environment_preview)
    {
        camera.target = {0,1,0};
        if (!config.preview_camera_override) { camera.yaw_degrees = 0; camera.pitch_degrees = 22; camera.distance = 22; }
    }
    const float target_height = monster ? (!family && config.monster_preview_asset >= 3 ? 3.0f : 1.0f)
        : config.character_preview ? 1.0f : camera.target.y;
    const float yaw_degrees = config.preview_camera_override ? config.preview_camera_yaw
#if defined(HS_DEVELOPMENT_TOOLS)
        : config.character_preview ? impl_->preview_yaw
#endif
        : camera.yaw_degrees;
    const float pitch_degrees = config.preview_camera_override ? config.preview_camera_pitch
#if defined(HS_DEVELOPMENT_TOOLS)
        : config.character_preview ? impl_->preview_pitch
#endif
        : camera.pitch_degrees;
    const float distance = config.preview_camera_override ? config.preview_camera_distance
        : family ? std::max(10.0f, std::sqrt(config.slime_family_preview_count * 3.0f) * 1.9f)
#if defined(HS_DEVELOPMENT_TOOLS)
        : monster ? (config.monster_preview_asset >= 3 ? 18.0f : 8.0f)
        : config.character_preview ? impl_->preview_distance
#endif
        : camera.distance;
    const auto yaw = DirectX::XMConvertToRadians(yaw_degrees);
    const auto pitch = DirectX::XMConvertToRadians(pitch_degrees);
    const auto direction = DirectX::XMVector3Normalize(DirectX::XMVectorSet(
        std::cos(pitch) * std::sin(yaw), -std::sin(pitch), std::cos(pitch) * std::cos(yaw), 0));
    const auto target = DirectX::XMVectorSet(family ? 0.0f : camera.target.x, target_height,
        family ? 0.0f : camera.target.z, 1);
    DirectX::XMFLOAT3 position{};
    DirectX::XMStoreFloat3(&position, DirectX::XMVectorSubtract(target, DirectX::XMVectorScale(direction, distance)));
    RenderViewInput input;
    input.id = kMainRenderViewId;
    input.camera.position = {position.x, position.y, position.z};
    input.camera.target = {family ? 0.0 : camera.target.x, target_height, family ? 0.0 : camera.target.z};
    input.camera.vertical_fov_degrees = camera.vertical_fov_degrees;
    input.view_origin = input.camera.position;
    input.output_extent = {impl_->width, impl_->height};
    input.render_capacity = impl_->view_state.render_capacity;
    input.render_active = impl_->view_state.render_active;
    return input;
}

Result D3D12Renderer::Render(const RenderFrameInput &input, RendererFrameResult &frame_result)
{
    const auto wrapper_start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration view_render_time{};
    if (!impl_->initialized || impl_->frame_submission_faulted)
        return Result::Failure(ErrorCode::InvalidState, "hs_render_view", "Renderer is not available for frame submission.");
    if (auto r = impl_->ValidateFrameInput(input); !r) return r;
    if (impl_->owned_views.empty())
    {
        auto main = std::make_unique<Impl::OwnedView>(); main->ready = true;
        main->configuration_revision = impl_->view_configuration_revision;
        main->content_generation = impl_->resource_generation_id;
        main->pipeline_revision = impl_->pipeline_revision;
        impl_->owned_views.emplace(kMainRenderViewId.value, std::move(main));
        impl_->RecordViewHistoryKeys(kMainRenderViewId);
    }
    // Prepare all new/replaced view packages before publishing any of them.
    std::map<std::uint32_t, std::unique_ptr<Impl::OwnedView>> candidates;
    for (const auto &view : input.views)
    {
        if (view.role == RenderViewRole::Main) continue;
        const auto found = impl_->owned_views.find(view.id.value);
        const bool replace = found == impl_->owned_views.end() ||
            found->second->state.output_extent != view.output_extent ||
            found->second->state.render_capacity != view.render_capacity ||
            found->second->configuration_revision != impl_->view_configuration_revision ||
            found->second->content_generation != impl_->resource_generation_id ||
            found->second->pipeline_revision != impl_->pipeline_revision;
        if (!replace) continue;
        std::unique_ptr<Impl::OwnedView> candidate;
        if (auto r = impl_->PrepareOwnedView(view, candidate, false); !r) return r;
        if (found != impl_->owned_views.end()) candidate->history_generation = found->second->history_generation + 1;
        candidates.emplace(view.id.value, std::move(candidate));
    }
    const auto restore_native_selection = [&]() -> Result {
        if (impl_->streamline_session)
            if (auto r = impl_->streamline_session->SelectReconstructionViewAfterCompletion(impl_->active_native_viewport_id); !r) return r;
        if (impl_->fsr_session)
            if (auto r = impl_->fsr_session->SelectReconstructionViewAfterCompletion(impl_->active_native_viewport_id); !r) return r;
        return Result::Success();
    };
    const auto reject_candidates = [&](Result failure) -> Result {
        for (auto &[id, candidate] : candidates)
            if (auto r = impl_->ResetOwnedViewAfterCompletion(*candidate); !r) {
                // Failed native cleanup must retain its complete candidate lease.
                impl_->initialized = false;
                impl_->unresolved_view_candidates.push_back(std::move(candidate));
                return r;
            }
        if (auto r = restore_native_selection(); !r) return r;
        return failure;
    };
    if (!candidates.empty())
    {
        if (auto r = impl_->WaitForGpu(); !r) return r;
        // Configuration of native candidates affects only their unique viewport.
        for (auto &[id, candidate] : candidates)
        {
            if (impl_->streamline_session)
                if (auto r = impl_->streamline_session->SelectReconstructionViewAfterCompletion(candidate->native_viewport_id); !r) return reject_candidates(r);
            if (impl_->fsr_session)
                if (auto r = impl_->fsr_session->SelectReconstructionViewAfterCompletion(candidate->native_viewport_id); !r) return reject_candidates(r);
            if (candidate->reconstruction_provider)
                if (auto r = candidate->reconstruction_provider->ConfigureAfterCompletion(candidate->state.render_capacity); !r) return reject_candidates(r);
        }
        if (impl_->streamline_session)
            if (auto r = impl_->streamline_session->SelectReconstructionViewAfterCompletion(impl_->active_native_viewport_id); !r) return reject_candidates(r);
        if (impl_->fsr_session)
            if (auto r = impl_->fsr_session->SelectReconstructionViewAfterCompletion(impl_->active_native_viewport_id); !r) return reject_candidates(r);
        for (auto &[id, candidate] : candidates)
        {
            if (auto old = impl_->owned_views.find(id); old != impl_->owned_views.end())
            {
                if (auto r = impl_->RetireHlodView(ViewId{id}); !r) return r;
                if (auto r = impl_->ResetOwnedViewAfterCompletion(*old->second); !r) return r;
            }
            impl_->owned_views.insert_or_assign(id, std::move(candidate));
            impl_->RecordViewHistoryKeys(ViewId{id});
        }
        if (impl_->streamline_session)
            if (auto r = impl_->streamline_session->SelectReconstructionViewAfterCompletion(impl_->active_native_viewport_id); !r) return r;
        if (impl_->fsr_session)
            if (auto r = impl_->fsr_session->SelectReconstructionViewAfterCompletion(impl_->active_native_viewport_id); !r) return r;
    }
    auto &main_owner = *impl_->owned_views.at(kMainRenderViewId.value);
    if (main_owner.configuration_revision != impl_->view_configuration_revision ||
        main_owner.content_generation != impl_->resource_generation_id || main_owner.pipeline_revision != impl_->pipeline_revision ||
        (!main_owner.history_keys.empty() && (main_owner.history_keys.front().capacity != impl_->view_state.output_extent)))
    {
        ++main_owner.history_generation;
        main_owner.configuration_revision = impl_->view_configuration_revision;
        main_owner.content_generation = impl_->resource_generation_id;
        main_owner.pipeline_revision = impl_->pipeline_revision;
        impl_->temporal_history_valid = false; impl_->hzb_history_valid = false;
        impl_->RecordViewHistoryKeys(kMainRenderViewId);
    }
    const auto saved_devtools_visible = impl_->config.devtools_visible;
    const auto saved_identity = impl_->sdk_frame_identity;
    const auto saved_input_phase = impl_->native_input_phase;
    impl_->active_render_package_id = impl_->render_package_id + 1;
    impl_->render_package_time = std::chrono::steady_clock::now();
    if (auto r = impl_->BeginHlodViewPackage(input); !r) return r;
    impl_->explicit_frame = &input;
    Result result = Result::Success();
    RendererFrameResult main_result{};
    std::vector<RenderViewFrameResult> view_results;
    bool rendered_main{}, rendered_any{};
    std::vector<const RenderViewInput *> execution_views;
    for (const auto &view : input.views) if (view.role == RenderViewRole::Main) execution_views.push_back(&view);
    for (const auto &view : input.views) if (view.role != RenderViewRole::Main) execution_views.push_back(&view);
    for (const auto *view_pointer : execution_views)
    {
        const auto &view = *view_pointer;
        if (auto r = impl_->ActivateView(view.id); !r) { result = r; break; }
        impl_->explicit_view = &view;
        impl_->config.devtools_visible = saved_devtools_visible && view.role == RenderViewRole::Main;
        if (view.role == RenderViewRole::Main) { impl_->native_input_phase = saved_input_phase; impl_->sdk_frame_identity = saved_identity; }
        const bool active_changed = view.render_active != impl_->view_state.render_active;
        (void)UpdateViewState(impl_->view_state, view.output_extent, view.render_capacity, view.render_active);
        impl_->render_width = view.render_active.width; impl_->render_height = view.render_active.height;
        if (active_changed) { impl_->hzb_history_valid = false; impl_->hzb_layout = MakeHzbLayout(impl_->render_width, impl_->render_height); }
        if (view.camera_cut)
        {
            impl_->temporal_history_valid = false; impl_->hzb_history_valid = false; impl_->brixelizer_history_valid = false;
            impl_->exposure_initialized = false; impl_->previous_origins_valid = false;
            impl_->previous_skin_deformation.reset(); impl_->previous_instances.reset(); impl_->skin_cache.Clear();
            impl_->ambient_occlusion.ResetHistory(); impl_->screen_reflections.ResetHistory();
            impl_->volumetrics.ResetHistory();
        }
        auto snapshots = input.snapshots;
        if (input.scene) { snapshots.current.static_scene = input.scene->lease; snapshots.has_current = true; }
        if (input.override_ui) { snapshots.current.ui = input.ui; snapshots.has_current = true; }
        if (input.simulation_time) { snapshots.current.header.simulation_time = *input.simulation_time; snapshots.has_current = true; }
        const auto &v = input.vfx;
        RendererFrameResult rendered{};
        // Event spawns are scene simulation inputs and are consumed once; the
        // retained particles/ribbons are shared by every sequential view.
        const bool first = view_pointer == execution_views.front();
        impl_->render_scene_inputs = first;
        const auto view_render_start = std::chrono::steady_clock::now();
        result = Render(snapshots, input.events, first ? v.particle_spawns : std::span<const ParticleSpawnCommand>{},
            v.effect_lines, v.events, v.persistent, v.ground, v.flashes, v.meshes, v.fresnels, v.ribbons,
            v.lights, v.distortions, v.decals, input.devtools, rendered, v.volumes, v.screen, v.entity_surfaces);
        view_render_time += std::chrono::steady_clock::now() - view_render_start;
        if (!result) break;
        rendered_any = true;
        if (view.camera_cut)
        {
            ++impl_->owned_views.at(view.id.value)->history_generation;
            // The successful cut frame starts the new history generation and
            // remains eligible for the next render of this same view.
            if (impl_->cluster_runtime && impl_->last_presented_index < kClusterRuntimeFrames)
                impl_->cluster_runtime->frames[impl_->last_presented_index].history_generation =
                    impl_->owned_views.at(view.id.value)->history_generation;
            impl_->RecordViewHistoryKeys(view.id);
            rendered.view_history_generation = impl_->owned_views.at(view.id.value)->history_generation;
        }
        view_results.push_back({rendered.identity, impl_->rendered_submission_count, rendered.frame,
            rendered.view_history_generation, rendered.rendered_view, rendered.unjittered_view_projection});
        frame_result = rendered;
        if (view.role == RenderViewRole::Main) { rendered_main = true; main_result = rendered; }
    }
    if (!result && impl_->explicit_view)
    {
        impl_->temporal_history_valid = false;
        impl_->hzb_history_valid = false;
        impl_->brixelizer_history_valid = false;
        impl_->previous_origins_valid = false;
        impl_->previous_skin_deformation.reset();
        impl_->previous_instances.reset();
        impl_->skin_cache.Clear();
    }
    impl_->explicit_view = nullptr; impl_->explicit_frame = nullptr;
    impl_->config.devtools_visible = saved_devtools_visible;
    if (auto restored = impl_->ActivateView(kMainRenderViewId); !restored) return restored;
    if (!rendered_main) { impl_->native_input_phase = saved_input_phase; impl_->sdk_frame_identity = saved_identity; }
    if (rendered_main) frame_result = main_result;
    frame_result.views = std::move(view_results);
    frame_result.cpu_timings.wrapper_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - wrapper_start - view_render_time).count());
    if (!result)
    {
        // Successfully rendered views retain their histories. The failed view
        // cannot publish a partial camera/pose/native history on the next call.
        if (rendered_any && impl_->frame_submission_faulted) impl_->native_input_phase = 0;
    }
    return result;
}

Result D3D12Renderer::CaptureViewPng(ViewId id, const std::filesystem::path &path)
{
    if (!impl_->initialized) return Result::Failure(ErrorCode::InvalidState, "hs_render_view", "Renderer is not initialized.");
    if (id != kMainRenderViewId && !impl_->owned_views.contains(id.value)) return ViewFailure("Unknown capture view ID.");
    if (auto r = impl_->ActivateView(id); !r) return r;
    if (impl_->frame_number == 0)
    {
        (void)impl_->ActivateView(kMainRenderViewId);
        return ViewFailure("View has not produced a successful rendered output.");
    }
    const auto result = CapturePng(path);
    if (auto r = impl_->ActivateView(kMainRenderViewId); !r) return r;
    return result;
}

Result D3D12Renderer::ReleaseView(ViewId id)
{
    if (!id.value || id == kMainRenderViewId) return ViewFailure("The main presentation view cannot be released independently.");
    const auto found = impl_->owned_views.find(id.value);
    if (found == impl_->owned_views.end()) return Result::Success();
    if (auto r = impl_->ActivateView(kMainRenderViewId); !r) return r;
    if (auto r = impl_->WaitForGpu(); !r) return r;
    if (auto r = impl_->RetireHlodView(id); !r) return r;
    if (auto r = impl_->ResetOwnedViewAfterCompletion(*found->second); !r) return r;
    if (impl_->streamline_session)
        if (auto r = impl_->streamline_session->SelectReconstructionViewAfterCompletion(0); !r) return r;
    if (impl_->fsr_session)
        if (auto r = impl_->fsr_session->SelectReconstructionViewAfterCompletion(0); !r) return r;
    impl_->owned_views.erase(found);
    return Result::Success();
}
} // namespace hs
