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

Result D3D12Renderer::Impl::ValidateFrameInput(const RenderFrameInput &input)
{
    if (input.views.empty()) return ViewFailure("A render frame requires at least one view.");
    if (input.vfx.world_batch)
    {
        const auto &batch = *input.vfx.world_batch;
        if (batch.session != input.snapshots.current.header.session_id ||
            (batch.sequence == 0 && (batch.reset_particle_state ||
                !input.vfx.particle_spawns.empty() || !input.vfx.effect_lines.empty())))
            return ViewFailure("Native VFX batch must match the session and identify its emission/reset submission.");
    }
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
    const auto frame_features = ResolveFeatures(config);
    const bool dynamic_resolution_active = EffectiveDynamicResolution(config.dynamic_resolution, frame_features);
    const auto &main_owner = *owned_views.at(kMainRenderViewId.value);
    const auto origin = input.views.front().scene_origin;
    if (native_input_phase && std::ranges::none_of(input.views, [](const auto &view) { return view.role == RenderViewRole::Main; }))
        return ViewFailure("An outstanding main input token must be completed by a packet containing the main view.");
    for (auto current = input.views.begin(); current != input.views.end(); ++current)
    {
        const auto &view = *current;
        if (!view.id.value || std::find_if(input.views.begin(), current,
                [&](const auto &prior) { return prior.id == view.id; }) != current)
            return ViewFailure("View IDs must be nonzero and unique within a frame.");
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
            (view.output_extent != ViewExtent{main_owner.width, main_owner.height} ||
             view.render_capacity != main_owner.state.render_capacity))
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
            const auto &owner = *found->second;
            const auto &plan = owner.reconstruction_plan;
            const auto *provider = owner.reconstruction_provider.get();
            const bool same_configuration = owner.state.output_extent == view.output_extent &&
                found->second->state.render_capacity == view.render_capacity &&
                owner.configuration_revision == view_configuration_revision &&
                owner.content_generation == resource_generation_id && owner.pipeline_revision == pipeline_revision;
            if (same_configuration && provider && provider->Initialized())
                if (view.render_active.width < plan.input_minimum.width || view.render_active.height < plan.input_minimum.height ||
                    view.render_active.width > plan.input_maximum.width || view.render_active.height > plan.input_maximum.height)
                    return ViewFailure("Requested active extent is outside this view's selected native reconstruction domain.");
            const bool prior_valid = owner.previous_origins_valid;
            const auto prior_origin = owner.previous_scene_origin;
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
    return ValidateFramePayload(input);
}

Result D3D12Renderer::Impl::ValidateFramePayload(const RenderFrameInput &frame_input)
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
            (!source.interpolate_owner_position ||
             (std::isfinite(source.previous_position.x) && std::isfinite(source.previous_position.y) &&
              std::isfinite(source.previous_position.z))) &&
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
    auto render_instances = snapshot.instances;
    std::vector<RenderInstance> retained_instances;
    if (!input.vfx.entity_surfaces.empty())
    {
        retained_instances.assign(render_instances.begin(), render_instances.end());
        for (const auto &command : input.vfx.entity_surfaces)
        {
            if (!command.has_retained_instance || std::ranges::any_of(retained_instances,
                    [&](const auto &instance) { return instance.stable_id == command.render_instance_id; })) continue;
            const auto &instance = command.retained_instance;
            if (instance.stable_id != command.render_instance_id ||
                instance.mesh < RenderMesh::BossFiveMinute || instance.mesh > RenderMesh::BossFinal ||
                instance.asset_id.value == 0 || instance.material_id.value == 0 ||
                (instance.animation_clip_id.value == 0 && !command.has_retained_animation_pose))
                return ViewFailure("Entity surface retention requires its actual boss mesh and captured pose.");
            retained_instances.push_back(instance);
        }
        render_instances = retained_instances;
    }
    std::vector<VfxEntitySurfaceCommand> admitted_surfaces;
    for (const auto &command : input.vfx.entity_surfaces)
        if (std::ranges::any_of(render_instances, [&](const auto &instance) {
                return instance.stable_id == command.render_instance_id &&
                    instance.mesh >= RenderMesh::BossFiveMinute && instance.mesh <= RenderMesh::BossFinal;
            })) admitted_surfaces.push_back(command);
    EntitySurfaceFrame surface;
    if (auto r = surface.Build(admitted_surfaces); !r) return r;
    std::unordered_set<RenderSceneInstanceKey, RenderSceneInstanceKeyHash> source_keys;
    bool unique_sources = render_instances.size() <= UINT32_MAX;
    for (std::size_t index = 0; unique_sources && index < render_instances.size(); ++index)
        unique_sources = source_keys.emplace(MakeRenderSceneInstanceKey(
            render_instances[index].stable_id, static_cast<std::uint32_t>(index))).second;
    if (!unique_sources)
        return ViewFailure("Dynamic render scene identities are invalid or duplicated.");
    for (const auto &instance : render_instances)
    {
        if (!instance.animation_clip_id.value) continue;
        if (instance.mesh != RenderMesh::Archer) return ViewFailure("Explicit named animation requires a loaded skeleton-bound character extension.");
        const auto index = static_cast<std::uint32_t>(&instance - render_instances.data());
        const auto key = MakeRenderPoseKey(instance, FindLastRenderPose(snapshot.poses, index));
        if (std::ranges::any_of(prepared_archer_palettes,
                [&](const auto &entry) { return entry.key == key; })) continue;
        auto &prepared = prepared_archer_palettes.emplace_back();
        prepared.key = key;
        const auto sampled = archer_compiled_animation
            ? SampleNamedCharacterAnimation(*archer_compiled_animation, archer_named_packed_transforms, instance,
                std::span(prepared.bones.data(), archer_bone_count))
            : SampleNamedCharacterAnimation(archer_named_animations, instance, std::span(prepared.bones.data(), archer_bone_count));
        if (!sampled) return sampled;
    }
    if (snapshot.static_scene && (!static_gpu_scene || static_gpu_scene->source != snapshot.static_scene))
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
        const auto keys = snapshot.static_scene->static_keys;
        if (keys.size() != snapshot.static_scene->instances.size() ||
            !std::ranges::is_sorted(keys, {}, &RenderSceneInstanceKey::stable_id) ||
            std::adjacent_find(keys.begin(), keys.end()) != keys.end())
            return ViewFailure("Static scene does not carry complete unique prepared identities.");
        for (const auto &instance : snapshot.static_scene->instances)
            if (instance.stable_id && !std::ranges::binary_search(
                    keys, instance.stable_id, {}, &RenderSceneInstanceKey::stable_id))
                return ViewFailure("Static scene identity does not match its prepared owner.");
    }
    if (snapshot.static_scene)
        for (const auto &instance : render_instances)
            if (instance.stable_id && std::ranges::binary_search(snapshot.static_scene->static_keys,
                    instance.stable_id, {}, &RenderSceneInstanceKey::stable_id))
                return ViewFailure("Dynamic render identity collides with the immutable process scene.");
    if (render_instances.size() > UINT32_MAX / 12u) return ViewFailure("Dynamic visibility output index overflow.");
    std::string ribbon_error;
    if (config.ribbon_history_points < kRibbonHistorySamples)
        return ViewFailure("Ribbon point budget must provide at least one 64-sample slot");
    const auto &camera = input.views.front();
    const Float3 camera_scene_position{static_cast<float>(camera.camera.position.x-camera.scene_origin.x),
        static_cast<float>(camera.camera.position.y-camera.scene_origin.y), static_cast<float>(camera.camera.position.z-camera.scene_origin.z)};
    if (!std::isfinite(camera_scene_position.x) || !std::isfinite(camera_scene_position.y) || !std::isfinite(camera_scene_position.z))
        return ViewFailure("Ribbon interpolation and previous eye must be finite");
    if (!VfxRibbonState::ValidateSources(input.vfx.ribbons.sources, ribbon_error))
        return ViewFailure(ribbon_error);
    return Result::Success();
}

Result D3D12Renderer::Impl::ActivateView(ViewId id)
{
    const auto next = owned_views.find(id.value);
    if (next == owned_views.end() || !next->second->ready)
        return ViewFailure("View has no prepared GPU state.");
    if (id == active_view_id) return Result::Success();
    if (cluster_runtime && !next->second->cluster_view.descriptors)
        if (auto prepared = CreateClusterViewResources(next->second->cluster_view); !prepared) return prepared;
    // Per-view native reconstruction contexts retain their own history. Selecting
    // one does not mutate another view's resources or submission-slot descriptors.
    if (streamline_session)
        if (auto selected = streamline_session->SelectReconstructionViewAfterCompletion(next->second->native_viewport_id); !selected) return selected;
    if (fsr_session)
        if (auto selected = fsr_session->SelectReconstructionViewAfterCompletion(next->second->native_viewport_id); !selected) return selected;
    active_view_id = id;
    active_view_role = next->second->role;
    active_native_viewport_id = next->second->native_viewport_id;
    return Result::Success();
}
Result D3D12Renderer::Impl::PrepareDecalSurface(const RendererConfig &candidate_config,
    ViewExtent capacity, bool admitted_work, DecalSurfaceCandidate &candidate) const
{
    return decal_surface.Prepare(device.Get(), allocator, root_signature.Get(), pipeline_cache, capacity, kFrameCount,
        admitted_work, [&](std::string_view name, std::vector<std::byte> &bytes) {
            if (candidate_config.packed_content && name == "decal_surface_cs.dxil")
            {
                for (const auto &material : candidate_config.packed_materials)
                {
                    if (material.kind != render_data::MaterialKind::Decal) continue;
                    const auto program = std::ranges::find(material.programs, render_data::MaterialProgramKind::DecalSurfaceCS,
                        &render_data::MaterialProgramDescriptor::kind);
                    if (program != material.programs.end() && program->product_id == "render/shader/decal_surface_cs.dxil")
                    {
                        StartupOperationScope timing(RendererStartupOperation::CheckedBinaryRead);
                        return ReadRendererMaterialProgram(candidate_config, *program, bytes);
                    }
                }
                return Result::Failure(ErrorCode::InvalidState, "RendererPack", "Missing declared decal Surface program.");
            }
            RendererAssetSource path;
            if (auto r = ResolveRendererAsset(candidate_config, "render/shader/" + std::string(name), path); !r) return r;
            return ReadBinary(path, bytes);
        }, candidate);
}

Result D3D12Renderer::Impl::PrepareOwnedView(const RenderViewInput &input, std::unique_ptr<OwnedView> &output, bool decal_work)
{
    auto reconstruction_config = config;
    const auto feature_plan = ResolveFeatures(config);
    reconstruction_config.dynamic_resolution = EffectiveDynamicResolution(config.dynamic_resolution, feature_plan);
    auto candidate = std::make_unique<OwnedView>();
    candidate->view_id = input.id;
    candidate->role = input.role;
    candidate->native_viewport_id = next_native_viewport_id++;
    candidate->width = input.output_extent.width; candidate->height = input.output_extent.height;
    candidate->render_width = input.render_active.width; candidate->render_height = input.render_active.height;
    candidate->display_state = owned_views.at(kMainRenderViewId.value)->display_state;
    candidate->configuration_revision = view_configuration_revision;
    candidate->content_generation = resource_generation_id;
    candidate->pipeline_revision = pipeline_revision;
    (void)UpdateViewState(candidate->state, input.output_extent, input.render_capacity, input.render_active);
    auto &v = *candidate;
    ViewExtent native_capacity;
    if (auto r = ResolveNativeReconstruction(reconstruction_config, input.output_extent, v.reconstruction_plan, native_capacity); !r) return r;
    if (auto r = PrepareReconstructionProvider(config.reconstruction_request ? &*config.reconstruction_request : nullptr,
            reconstruction_config.dynamic_resolution, config.dynamic_min_percent, config.dynamic_max_percent, input.output_extent,
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
    desc.DepthOrArraySize = desc.MipLevels = 1; desc.Format = v.display_state.format; desc.SampleDesc.Count = 1;
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
    if (auto r = PrepareAdaptiveShading(v, config, capacity, adaptive); !r) return r;
    if (auto r = PrepareAmbientOcclusion(v, config, capacity, ambient, adaptive.Status().compute_ready); !r) return r;
    if (auto r = PrepareSurfaceLighting(v, config, capacity, diffusion, reflections); !r) return r;
    if (auto r = PrepareHeroTransparency(v, config, capacity, transparency); !r) return r;
    if (auto r = PrepareHdrPost(v, config, capacity, input.output_extent, hdr); !r) return r;
    if (auto r = PrepareScreenOverlay(config, capacity, overlay); !r) return r;
    if (auto r = PrepareLightList(v, config, capacity, lights); !r) return r;
    if (auto r = PrepareDecalSurface(config, capacity, decal_work, decals); !r) return r;
    if (auto r = PrepareBrixelizerGi(v, config, input.render_active, brixelizer); !r) return r;
    if (auto r = PrepareVirtualShadows(v, config, capacity, shadows); !r) return r;
    if (auto r = PrepareAdditionalShadows(v, config, capacity, additional); !r) return r;
    if (auto r = PrepareVolumetrics(v, config, capacity, volume); !r) return r;
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
        if (auto r = PrepareReconstructionGuides(v, config, capacity, input.output_extent, guides,
                v.reconstruction_provider->NeedsCompositionGuide()); !r) return r;
        if (auto r = v.reconstruction_guides.Commit(std::move(guides), completed); !r) return r;
    }
    v.reconstruction_plan.render_active = input.render_active;
    v.ready = true;
    output = std::move(candidate);
    return Result::Success();
}

void D3D12Renderer::Impl::InvalidateCanonicalDeformationAfterCompletion() noexcept
{
    for (auto &[id, view] : owned_views)
    {
        view->hdr_capture_source.Reset();
        view->hdr_capture_valid = false;
    }
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
    if (auto r = RetireHeroTransparency(view.hero_transparency); !r) return r;
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
        if (auto r = ResetOwnedViewAfterCompletion(*view); !r) return r;
    for (auto &candidate : unresolved_view_candidates)
        if (auto r = ResetOwnedViewAfterCompletion(*candidate); !r) return r;
    unresolved_view_candidates.clear();
    owned_views.clear();
    timing_previous_source_endpoints.clear();
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
    const auto &main_owner = *impl_->owned_views.at(kMainRenderViewId.value);
    input.output_extent = {main_owner.width, main_owner.height};
    input.render_capacity = main_owner.state.render_capacity;
    input.render_active = main_owner.state.render_active;
    return input;
}

Result D3D12Renderer::Render(const RenderFrameInput &source_input, RendererFrameResult &frame_result)
{
    frame_result.render_history_committed = false;
    // Rejected preflight keeps the last frame evidence, with no new submission receipt.
    struct WorldStepReceipt
    {
        RendererFrameResult &output;
        Tick tick{};
        std::uint64_t session{}, sequence{};
        bool submitted{};
        std::uint64_t presented_ui_generation{};
        ~WorldStepReceipt()
        {
            output.vfx_world_step_submitted = submitted;
            output.vfx_world_tick = tick;
            output.vfx_world_session_id = session;
            output.vfx_world_batch_sequence = sequence;
            output.presented_ui_packet_generation = presented_ui_generation;
        }
    } world_step_receipt{frame_result};
    const auto wrapper_start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration view_render_time{};
    if (!impl_->initialized || impl_->frame_submission_faulted)
        return Result::Failure(ErrorCode::InvalidState, "hs_render_view", "Renderer is not available for frame submission.");
    auto input = source_input;
    impl_->render_package_time = input.sample_time.value_or(std::chrono::steady_clock::now());
    if (input.simulation_time)
    {
        input.snapshots.current.header.simulation_time = *input.simulation_time;
        input.snapshots.current.header.clock.mode = RenderClockMode::Scripted;
    }
    auto alpha = RenderSnapshotInterpolation(input.snapshots, impl_->render_package_time, impl_->config.interpolate);
    auto &sample_header = input.snapshots.current.header;
    const auto current_time = sample_header.simulation_time;
    auto display_time = current_time;
    if (input.snapshots.has_previous && alpha < 1.0f)
    {
        const auto previous_time = input.snapshots.previous.header.simulation_time;
        display_time = previous_time + std::chrono::nanoseconds(static_cast<std::int64_t>(
            static_cast<double>((current_time - previous_time).count()) * alpha));
        // Explicit package time already owns caller-prepared VFX ages.
        if (!input.sample_time)
        {
            for (const auto &view : input.views)
            {
                const auto found = impl_->owned_views.find(view.id.value);
                if (found == impl_->owned_views.end() || view.camera_cut) continue;
                const auto &history = *found->second;
                if (history.has_rendered_sample && history.last_rendered_sample.session_id == sample_header.session_id &&
                    history.last_rendered_sample.clock.epoch == sample_header.clock.epoch)
                    display_time = std::min(current_time, std::max(display_time, history.last_rendered_sample.simulation_time));
            }
            alpha = static_cast<float>(static_cast<double>((display_time - previous_time).count()) /
                static_cast<double>((current_time - previous_time).count()));
        }
    }
    const auto previous_count = input.snapshots.has_previous && alpha < 1.0f
        ? input.snapshots.previous.instances.size() : 0;
    if (input.snapshots.current.instances.size() > UINT32_MAX ||
        previous_count > UINT32_MAX - input.snapshots.current.instances.size())
        return ViewFailure("Display sample exceeds native instance addressing.");
    impl_->render_scene_sample.Prepare(input.snapshots, alpha,
        [&](const RenderInstance &instance, CharacterAnimationClip clip) {
            const auto index = static_cast<std::size_t>(clip);
            if (instance.mesh == RenderMesh::Archer)
                return index < impl_->archer_clips.size() && impl_->archer_clips[index].looping != 0;
            if (instance.mesh >= RenderMesh::MonsterMelee && instance.mesh <= RenderMesh::BossFinal)
            {
                const auto &asset = impl_->monster_assets[static_cast<std::size_t>(instance.mesh) -
                    static_cast<std::size_t>(RenderMesh::MonsterMelee)];
                return index < asset.clip_meta.size() && asset.clip_meta[index].z != 0;
            }
            return false;
        });
    impl_->render_package_alpha = alpha;
    input.snapshots.current = impl_->render_scene_sample.View(input.snapshots.current);
    sample_header.simulation_time = display_time;
    impl_->prepared_archer_palettes.clear();
    if (auto r = impl_->ValidateFrameInput(input); !r) return r;
    if (!impl_->owned_views.contains(kMainRenderViewId.value) ||
        !impl_->owned_views.at(kMainRenderViewId.value)->ready)
        return ViewFailure("Main render view has no initialized GPU owner.");
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
        }
        if (impl_->streamline_session)
            if (auto r = impl_->streamline_session->SelectReconstructionViewAfterCompletion(impl_->active_native_viewport_id); !r) return r;
        if (impl_->fsr_session)
            if (auto r = impl_->fsr_session->SelectReconstructionViewAfterCompletion(impl_->active_native_viewport_id); !r) return r;
    }
    auto &main_owner = *impl_->owned_views.at(kMainRenderViewId.value);
    if (main_owner.configuration_revision != impl_->view_configuration_revision ||
        main_owner.content_generation != impl_->resource_generation_id || main_owner.pipeline_revision != impl_->pipeline_revision ||
        main_owner.history_output_extent != main_owner.state.output_extent)
    {
        ++main_owner.history_generation;
        main_owner.configuration_revision = impl_->view_configuration_revision;
        main_owner.content_generation = impl_->resource_generation_id;
        main_owner.pipeline_revision = impl_->pipeline_revision;
        main_owner.temporal_history_valid = false; main_owner.hzb_history_valid = false;
        main_owner.history_output_extent = main_owner.state.output_extent;
    }
    const auto saved_devtools_visible = impl_->config.devtools_visible;
    const auto saved_identity = main_owner.sdk_frame_identity;
    const auto saved_input_phase = impl_->native_input_phase;
    impl_->active_render_package_id = impl_->render_package_id + 1;
    if (auto r = impl_->BeginHlodViewPackage(input); !r) return r;
    impl_->explicit_frame = &input;
    auto &prepared_scene = impl_->prepared_scene_package;
    prepared_scene.ReleaseBorrowed();
    struct PreparedSceneLease
    {
        Impl::PreparedScenePackage &scene;
        ~PreparedSceneLease() { scene.ReleaseBorrowed(); }
    } prepared_scene_lease{prepared_scene};
    Result result = Result::Success();
    RendererFrameResult main_result{};
    std::vector<RenderViewFrameResult> view_results;
    view_results.reserve(input.views.size());
    bool rendered_main{}, rendered_any{}, failed_view_history_committed{};
    const auto main = std::ranges::find(input.views, RenderViewRole::Main, &RenderViewInput::role);
    auto next = input.views.begin();
    for (std::size_t ordinal = 0; ordinal < input.views.size(); ++ordinal)
    {
        // Main consumes the presentation token first; other views keep packet order.
        auto view_pointer = main;
        if (ordinal != 0 || main == input.views.end())
        {
            if (next == main) ++next;
            view_pointer = next++;
        }
        const auto &view = *view_pointer;
        if (auto r = impl_->ActivateView(view.id); !r) { result = r; break; }
        auto &owned_view = *impl_->owned_views.at(view.id.value);
        impl_->explicit_view = &view;
        impl_->config.devtools_visible = saved_devtools_visible && view.role == RenderViewRole::Main;
        if (view.role == RenderViewRole::Main) { impl_->native_input_phase = saved_input_phase; owned_view.sdk_frame_identity = saved_identity; }
        const bool active_changed = view.render_active != owned_view.state.render_active;
        (void)UpdateViewState(owned_view.state, view.output_extent, view.render_capacity, view.render_active);
        owned_view.width = view.output_extent.width; owned_view.height = view.output_extent.height;
        owned_view.render_width = view.render_active.width; owned_view.render_height = view.render_active.height;
        if (active_changed) { owned_view.hzb_history_valid = false; owned_view.targets.hzb_layout = MakeHzbLayout(owned_view.render_width, owned_view.render_height); }
        if (view.camera_cut)
        {
            owned_view.temporal_history_valid = false; owned_view.hzb_history_valid = false; owned_view.brixelizer_history_valid = false;
            owned_view.exposure_initialized = false; owned_view.previous_origins_valid = false;
            owned_view.previous_skin_deformation.reset(); owned_view.previous_instances.reset(); owned_view.skin_history.Clear();
            owned_view.ambient_occlusion.ResetHistory(); owned_view.screen_reflections.ResetHistory();
            owned_view.volumetrics.ResetHistory();
        }
        auto snapshots = input.snapshots;
        if (input.scene) { snapshots.current.static_scene = input.scene->lease; snapshots.has_current = true; }
        if (input.override_ui) { snapshots.current.ui = input.ui; snapshots.has_current = true; }
        if (input.simulation_time) snapshots.has_current = true;
        const auto &v = input.vfx;
        RendererFrameResult rendered{};
        // Event spawns are scene simulation inputs and are consumed once; the
        // retained particles/ribbons are shared by every sequential view.
        const bool first = ordinal == 0;
        impl_->render_scene_inputs = first;
        const auto view_render_start = std::chrono::steady_clock::now();
        result = Render(snapshots, input.events, first ? v.particle_spawns : std::span<const ParticleSpawnCommand>{},
            v.effect_lines, v.event_count, v.persistent_count, v.ground, v.flashes, v.meshes, v.fresnels, v.ribbons,
            v.lights, v.distortions, v.decals, input.devtools, rendered, v.volumes, v.screen, v.entity_surfaces);
        view_render_time += std::chrono::steady_clock::now() - view_render_start;
        if (rendered.presented_ui_packet_generation)
            world_step_receipt.presented_ui_generation = rendered.presented_ui_packet_generation;
        if (rendered.vfx_world_step_submitted)
        {
            world_step_receipt.submitted = true;
            world_step_receipt.tick = rendered.vfx_world_tick;
            world_step_receipt.session = rendered.vfx_world_session_id;
            world_step_receipt.sequence = rendered.vfx_world_batch_sequence;
        }
        if (!result)
        {
            failed_view_history_committed = rendered.render_history_committed;
            frame_result.render_history_committed |= rendered.render_history_committed;
            break;
        }
        rendered_any = true;
        view_results.push_back({rendered.identity, impl_->rendered_submission_count, rendered.frame,
            rendered.view_history_generation, rendered.rendered_view, rendered.unjittered_view_projection});
        frame_result = rendered;
        if (view.role == RenderViewRole::Main) { rendered_main = true; main_result = rendered; }
    }
    if (!result && impl_->explicit_view && !failed_view_history_committed)
    {
        if (impl_->explicit_view)
        {
            auto &failed_view = *impl_->owned_views.at(impl_->explicit_view->id.value);
            failed_view.temporal_history_valid = false;
            failed_view.hzb_history_valid = false;
            failed_view.brixelizer_history_valid = false;
            failed_view.previous_origins_valid = false;
            failed_view.previous_skin_deformation.reset();
            failed_view.previous_instances.reset();
            failed_view.skin_history.Clear();
        }
    }
    impl_->explicit_view = nullptr; impl_->explicit_frame = nullptr;
    impl_->config.devtools_visible = saved_devtools_visible;
    if (auto restored = impl_->ActivateView(kMainRenderViewId); !restored) return restored;
    if (!rendered_main) { impl_->native_input_phase = saved_input_phase; main_owner.sdk_frame_identity = saved_identity; }
    if (rendered_main) frame_result = main_result;
    frame_result.views = std::move(view_results);
    frame_result.cpu_timings.wrapper_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - wrapper_start - view_render_time).count());
    if (!result)
    {
        const auto removed = impl_->device->GetDeviceRemovedReason();
        if (FAILED(removed)) return impl_->CheckDevice(removed, "Render view submission");
        // Successfully rendered views retain their histories. The failed view
        // cannot publish a partial camera/pose/native history on the next call.
        if (rendered_any && impl_->frame_submission_faulted) impl_->native_input_phase = 0;
    }
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
    impl_->timing_previous_source_endpoints.erase(id.value);
    impl_->owned_views.erase(found);
    return Result::Success();
}
} // namespace hs
