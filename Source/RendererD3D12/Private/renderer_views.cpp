#include "renderer_impl.hpp"
#include "renderer_reconstruction.hpp"
#include "renderer_named_animation.hpp"
#include "renderer_entity_surface.hpp"
#include <array>
#include <numbers>

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

Result D3D12Renderer::Impl::ValidateFrameInput(const RenderPackage &input, PreparedVfxHeroVolumes &hero_volumes)
{
    if (input.views.empty()) return ViewFailure("A render frame requires at least one view.");
    {
        const auto &batch = input.vfx.world_batch;
        if (batch.session != input.snapshots.current.header.session_id ||
            (batch.sequence == 0 && (batch.reset_particle_state ||
                !input.vfx.particle_spawns.empty() || !input.vfx.effect_lines.empty())))
            return ViewFailure("Native VFX batch must match the session and identify its emission/reset submission.");
    }
#if defined(HS_DEVELOPMENT_TOOLS) || defined(HS_OFFLINE_PROBE_BAKER)
    if (input.devtools.delta_seconds &&
        (!std::isfinite(*input.devtools.delta_seconds) || *input.devtools.delta_seconds < 0))
        return ViewFailure("Frame delta must be finite and nonnegative.");
#endif
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
    const auto scene = input.snapshots.has_current ? input.snapshots.current.static_scene.get() : nullptr;
    if (scene && !scene->content_generation_id.empty() && scene->content_generation_id != resource_generation_id)
        return ViewFailure("Frame scene generation must match the resident GPU asset generation.");
    return PrepareFramePayload(input, hero_volumes);
}

Result D3D12Renderer::Impl::PrepareFramePayload(const RenderPackage &frame_input, PreparedVfxHeroVolumes &hero_volumes)
{
    const auto &input = frame_input;
    if (input.vfx.fresnels.size() > 32) return ViewFailure("Too many Fresnel shell commands.");
    std::array<std::uint64_t, 32> fresnel_ids;
    std::size_t fresnel_count{};
    const auto &fresnel_recipe_view = input.vfx.fresnel_recipe_view;
    const auto fresnel_recipes = fresnel_recipe_view.Rows();
    if (!input.vfx.fresnels.empty() && !fresnel_recipe_view.IsAdmitted())
        return ViewFailure("Fresnel commands require an admitted recipe table.");
    if (fresnel_recipe_view.IsAdmitted() && !fresnel_recipes.empty() &&
        fresnel_recipe_view.MaximumGradientRow() >= this->vfx_gradient_rows)
        return ViewFailure("Admitted Fresnel recipe table exceeds the selected gradient atlas.");
    for (const auto &source : input.vfx.fresnels)
    {
        const auto finite_transform = [](const VfxTransform &transform) {
            return std::ranges::all_of(transform, [](float value) { return std::isfinite(value); });
        };
        if (source.render_instance_id == 0 || source.stable_id == 0 ||
            source.recipe_index >= fresnel_recipes.size() ||
            !finite_transform(source.current_transform) ||
            !finite_transform(source.previous_transform) ||
            !std::isfinite(source.footprint_radius) || source.footprint_radius <= 0.0f ||
            !std::isfinite(source.source_progress) || source.source_progress < 0.0f ||
            source.source_progress >= 1.0f ||
            !std::isfinite(source.elapsed_seconds) || source.elapsed_seconds < 0.0f)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                   "Invalid Fresnel shell command.");
        fresnel_ids[fresnel_count++] = source.stable_id;
    }
    const auto used_fresnel_ids = std::span(fresnel_ids).first(fresnel_count);
    std::ranges::sort(used_fresnel_ids);
    if (std::adjacent_find(used_fresnel_ids.begin(), used_fresnel_ids.end()) != used_fresnel_ids.end())
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Invalid Fresnel shell command.");
    const auto &mesh_recipe_view = input.vfx.mesh_recipe_view;
    const auto mesh_recipes = mesh_recipe_view.Rows();
    if (!input.vfx.meshes.empty() && !mesh_recipe_view.IsAdmitted())
        return ViewFailure("Owned mesh commands require an admitted recipe table.");
    if (mesh_recipe_view.IsAdmitted() && !mesh_recipes.empty() &&
        mesh_recipe_view.MaximumGradientRow() >= this->vfx_gradient_rows)
        return ViewFailure("Admitted owned mesh recipe table exceeds the selected gradient atlas.");
    const auto finite_mesh_vector = [](const Float3 &v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    for (const auto &source : input.vfx.meshes)
    {
        if (source.recipe_index >= mesh_recipes.size())
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid owner mesh command.");
        if (!finite_mesh_vector(source.position) ||
            !finite_mesh_vector(source.previous_position) || !finite_mesh_vector(source.velocity) ||
            !std::isfinite(source.radius) || source.radius <= 0)
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
        if (!finite)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored flash command.");
        if (source.shape == VfxImpactShape::Spark)
        {
        const auto spark_speed = std::hypot(source.initial_velocity.x,
                                            source.initial_velocity.y,
                                            source.initial_velocity.z);
        const auto spark_direction = std::hypot(source.direction.x,
                                                source.direction.y,
                                                source.direction.z);
        const auto spark_alignment = source.direction.x * source.initial_velocity.x +
            source.direction.y * source.initial_velocity.y +
            source.direction.z * source.initial_velocity.z;
        if (source.oit || source.ground_base_anchor || source.aspect != 1.0f ||
             !std::isfinite(spark_speed) || spark_speed <= 0.0f ||
             !std::isfinite(spark_direction) || std::abs(spark_direction - 1.0f) > 0.001f ||
             spark_alignment < 0.999f * spark_speed)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                   "Invalid authored flash command.");
        }
    }
    for (const auto &input : frame_input.vfx.ground)
    {
            const auto &source = input.command;
            const auto &geometry = input.geometry;
            if (!geometry.GapStorageConsistent(frame_input.vfx.ground_gap_angles.size()))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored ring-gap storage.");
            const auto gap_angles = geometry.frame_gap_count != 0
                ? frame_input.vfx.ground_gap_angles.subspan(geometry.frame_gap_first, geometry.frame_gap_count)
                : std::span<const float>(geometry.gap_angles_radians);
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
                (circle_preview && (geometry.inner_radius != 0 || geometry.GapCount() != 0 || geometry.gap_half_angle_radians != 0)) ||
                !std::isfinite(geometry.gap_half_angle_radians) || geometry.gap_half_angle_radians < 0 ||
                geometry.gap_half_angle_radians > std::numbers::pi_v<float> ||
                std::ranges::any_of(gap_angles, [](float angle) { return !std::isfinite(angle); })))
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
                geometry.GapCount() != 0))
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
                (geometry.mask_slice != 0xffffffffu && geometry.mask_slice >= 12) ||
                !std::isfinite(geometry.mask_strength) || geometry.mask_strength < 0.0f ||
                geometry.mask_strength > 1.0f ||
                (geometry.mask_slice == 0xffffffffu && geometry.mask_strength != 0.0f) ||
                geometry.GapCount() != 0))
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
        if (!source.GapStorageConsistent(input.vfx.distortion_gap_angles.size()))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored distortion gap storage.");
        const auto gap_angles = source.frame_gap_count != 0
            ? input.vfx.distortion_gap_angles.subspan(source.frame_gap_first, source.frame_gap_count)
            : std::span<const float>(source.gap_angles_degrees);
        if (!finite(source.position) || !finite(source.direction) || !std::isfinite(source.radius) || source.radius <= 0 ||
            !std::isfinite(source.strength) || source.strength < 0 || !std::isfinite(source.alpha) || source.alpha < 0 || source.alpha > 1 ||
            !std::isfinite(source.normalized_age) || source.normalized_age < 0 || source.normalized_age > 1 ||
            static_cast<std::uint32_t>(source.shape) > 6 || static_cast<std::uint32_t>(source.quality) > 2)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored distortion command.");
        const bool cone = source.shape == VfxDistortionShape::ConeSector;
        const bool annulus = source.shape == VfxDistortionShape::GappedAnnulus;
        const float horizontal_direction = cone ? std::hypot(source.direction.x,source.direction.z) : 0.0f;
        if ((cone && (!std::isfinite(source.half_angle_degrees) || source.half_angle_degrees <= 0 ||
                      source.half_angle_degrees > 180 || !std::isfinite(horizontal_direction) ||
                      horizontal_direction <= .0001f || source.inner_radius != 0 ||
                      source.gap_half_width_degrees != 0 || source.GapCount() != 0)) ||
            (annulus && (!std::isfinite(source.inner_radius) || source.inner_radius < 0 ||
                         source.inner_radius >= source.radius || !std::isfinite(source.gap_half_width_degrees) ||
                         source.gap_half_width_degrees < 0 || source.gap_half_width_degrees > 180 ||
                         source.half_angle_degrees != 0 ||
                         (source.GapCount() != 0 && source.gap_half_width_degrees == 0) ||
                         std::any_of(gap_angles.begin(), gap_angles.end(),
                                     [](float angle) { return !std::isfinite(angle); }))) ||
            (!cone && !annulus && (source.inner_radius != 0 || source.half_angle_degrees != 0 ||
                                   source.gap_half_width_degrees != 0 || source.GapCount() != 0)))
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
    if (auto r = PrepareVfxHeroVolumes(input.vfx.volumes, vfx_gradient_rows, hero_volumes); !r) return r;
    std::uint64_t gaps{};
    for (const auto &ground : input.vfx.ground) gaps += ground.geometry.GapCount();
    for (const auto &distortion : input.vfx.distortions)
        if (distortion.shape == VfxDistortionShape::GappedAnnulus) gaps += distortion.GapCount();
    if (gaps > UINT32_MAX || input.vfx.ground.size() > UINT32_MAX || input.vfx.flashes.size() > UINT32_MAX || input.vfx.meshes.size() > UINT32_MAX)
        return ViewFailure("Authored VFX upload counts exceed their native index capacity.");
    const RenderSnapshot empty_snapshot{};
    const auto &snapshot = input.snapshots.has_current ? input.snapshots.current : empty_snapshot;
    const auto &static_scene = snapshot.static_scene;
    const auto render_instances = snapshot.instances;
    auto &prepared_instances = prepared_scene_package.instances;
    prepared_instances.assign(render_instances.begin(), render_instances.end());
    if (!input.vfx.entity_surfaces.empty())
    {
        for (const auto &command : input.vfx.entity_surfaces)
        {
            if (!command.has_retained_instance ||
                std::ranges::any_of(prepared_instances, [&](const auto &instance) {
                    return instance.stable_id == command.render_instance_id;
                })) continue;
            const auto &instance = command.retained_instance;
            if (instance.stable_id != command.render_instance_id ||
                instance.mesh < RenderMesh::BossFiveMinute || instance.mesh > RenderMesh::BossFinal ||
                instance.asset_id.value == 0 || instance.material_id.value == 0 ||
                (instance.animation_clip_id.value == 0 && !command.has_retained_animation_pose))
                return ViewFailure("Entity surface retention requires its actual boss mesh and captured pose.");
            prepared_instances.push_back(instance);
        }
    }
    const auto render_instance_count = prepared_instances.size();
    if (render_instance_count > UINT32_MAX / 12u) return ViewFailure("Dynamic visibility output index overflow.");
    // Admission finishes before LOD ordering reuses this source-index scratch.
    auto &source_order = prepared_scene_package.order_scratch;
    source_order.resize(render_instance_count);
    for (std::size_t index = 0; index < render_instance_count; ++index)
        source_order[index] = static_cast<std::uint32_t>(index);
    const auto source_id = [&](std::uint32_t index) { return prepared_instances[index].stable_id; };
    std::ranges::sort(source_order, {}, source_id);
    if (std::adjacent_find(source_order.begin(), source_order.end(), [&](auto a, auto b) {
            return source_id(a) != 0 && source_id(a) == source_id(b);
        }) != source_order.end())
        return ViewFailure("Dynamic render scene identities are invalid or duplicated.");
    std::vector<VfxEntitySurfaceCommand> admitted_surfaces;
    for (const auto &command : input.vfx.entity_surfaces)
    {
        const auto owners = std::ranges::equal_range(source_order, command.render_instance_id, {}, source_id);
        if (std::ranges::any_of(owners, [&](std::uint32_t index) {
                const auto mesh = prepared_instances[index].mesh;
                return mesh >= RenderMesh::BossFiveMinute && mesh <= RenderMesh::BossFinal;
            })) admitted_surfaces.push_back(command);
    }
    if (auto r = prepared_scene_package.entity_surfaces.Build(admitted_surfaces); !r) return r;
    const auto validate_animation = [&](const RenderInstance &instance) -> Result {
        if (!instance.animation_clip_id.value) return Result::Success();
        if (instance.mesh != RenderMesh::Archer)
            return ViewFailure("Explicit named animation requires a loaded skeleton-bound character extension.");
        const auto key = MakeRenderPoseSampleKey(instance, nullptr);
        if (std::ranges::any_of(prepared_archer_palettes,
                [&](const auto &entry) { return entry.key == key; })) return Result::Success();
        auto &prepared = prepared_archer_palettes.emplace_back();
        prepared.key = key;
        const auto sampled = archer_compiled_animation
            ? SampleNamedCharacterAnimation(*archer_compiled_animation, archer_named_packed_transforms, instance,
                std::span(prepared.bones.data(), archer_bone_count))
            : SampleNamedCharacterAnimation(archer_named_animations, instance, std::span(prepared.bones.data(), archer_bone_count));
        return sampled;
    };
    for (const auto &instance : prepared_instances)
        if (auto r = validate_animation(instance); !r) return r;
    if (static_scene && (!static_gpu_scene || static_gpu_scene->source != static_scene))
    {
        std::uint64_t output_count{};
        for (const auto &instance : static_scene->instances)
        {
            const bool environment = render_data::IsEnvironmentMesh(instance.mesh);
            if (!environment && instance.mesh != RenderMesh::Ground && instance.mesh != RenderMesh::DirtPatch)
                return ViewFailure("Unsupported mesh in static scene.");
            output_count += environment ? 3u : 1u;
        }
        if (output_count > UINT32_MAX / 4u) return ViewFailure("Static visibility output index overflow.");
        const auto keys = static_scene->static_keys;
        if (keys.size() != static_scene->instances.size() ||
            !std::ranges::is_sorted(keys, {}, &RenderSceneInstanceKey::stable_id) ||
            std::adjacent_find(keys.begin(), keys.end()) != keys.end())
            return ViewFailure("Static scene does not carry complete unique prepared identities.");
        for (const auto &instance : static_scene->instances)
            if (instance.stable_id && !std::ranges::binary_search(
                    keys, instance.stable_id, {}, &RenderSceneInstanceKey::stable_id))
                return ViewFailure("Static scene identity does not match its prepared owner.");
    }
    if (static_scene)
    {
        for (const auto &instance : prepared_instances)
            if (instance.stable_id && std::ranges::binary_search(static_scene->static_keys,
                    instance.stable_id, {}, &RenderSceneInstanceKey::stable_id))
                return ViewFailure("Dynamic render identity collides with the immutable process scene.");
    }
    std::string ribbon_error;
    if (!ribbon_state.ValidateFrameSources(input.vfx.ribbons.sources, ribbon_error,
            input.vfx.ribbons.outputs))
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
    // Prepare produces a candidate independently; the selected OwnedView takes
    // ownership when it commits the candidate after its own completion checks.
    return DecalSurfaceSystem{}.Prepare(device.Get(), allocator, root_signature.Get(), pipeline_cache, capacity, kFrameCount,
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
    const auto capacity = input.render_capacity;
    if (v.reconstruction_provider && (native_capacity.width > capacity.width || native_capacity.height > capacity.height ||
        input.render_active.width < v.reconstruction_plan.input_minimum.width ||
        input.render_active.height < v.reconstruction_plan.input_minimum.height ||
        input.render_active.width > v.reconstruction_plan.input_maximum.width ||
        input.render_active.height > v.reconstruction_plan.input_maximum.height))
        return ViewFailure("Requested view capacity/active extent does not cover the selected native reconstruction domain.");
    if (auto r = PrepareOptionTargets(v.targets, input.render_capacity.width, input.render_capacity.height,
            config.shadow_resolution, config.bloom, input.output_extent.width, input.output_extent.height,
            UsesInternalResolveTargets(v.reconstruction_plan),
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
    if (auto r = PrepareExposureResources(v); !r) return r;
    const auto completed = graph_queue_executor.Completed();
    ViewFeatureCandidates features;
    if (auto r = PrepareViewFeatures(v, config, capacity, input.output_extent, decal_work, features); !r)
        return r;
    if (auto r = CommitViewFeaturePrefix(v, features, completed); !r) return r;
    if (auto r = CommitAdditionalShadows(v, std::move(features.additional)); !r) return r;
    if (auto r = CommitVolumetrics(v, std::move(features.volume)); !r) return r;
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
#if defined(HS_DEVELOPMENT_TOOLS)
    for (auto &[id, view] : owned_views)
    {
        view->hdr_capture_source.Reset();
        view->hdr_capture_valid = false;
    }
#endif
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
    view.last_history_frame_slot = UINT32_MAX;
    ResetClusterViewResources(view.cluster_view);
    if (view.reconstruction_provider)
        if (auto r = view.reconstruction_provider->ResetAfterCompletion(); !r) return r;
    if (auto r = view.ambient_occlusion.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.adaptive_shading.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.subsurface.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.screen_reflections.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = RetireLayeredTranslucency(view.translucency); !r) return r;
    if (auto r = view.hdr_post.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.screen_overlay.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
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
    const auto &camera = source;
    const auto yaw_degrees = camera.yaw_degrees;
    const auto pitch_degrees = camera.pitch_degrees;
    const auto distance = camera.distance;
    const auto target_height = camera.target.y;
    const auto yaw = DirectX::XMConvertToRadians(yaw_degrees);
    const auto pitch = DirectX::XMConvertToRadians(pitch_degrees);
    const auto direction = DirectX::XMVector3Normalize(DirectX::XMVectorSet(
        std::cos(pitch) * std::sin(yaw), -std::sin(pitch), std::cos(pitch) * std::cos(yaw), 0));
    const auto target = DirectX::XMVectorSet(camera.target.x, target_height,
        camera.target.z, 1);
    DirectX::XMFLOAT3 position{};
    DirectX::XMStoreFloat3(&position, DirectX::XMVectorSubtract(target, DirectX::XMVectorScale(direction, distance)));
    RenderViewInput input;
    input.id = kMainRenderViewId;
    input.camera.position = {position.x, position.y, position.z};
    input.camera.target = {camera.target.x, target_height, camera.target.z};
    input.camera.vertical_fov_degrees = camera.vertical_fov_degrees;
    input.view_origin = input.camera.position;
    const auto &main_owner = *impl_->owned_views.at(kMainRenderViewId.value);
    input.output_extent = {main_owner.width, main_owner.height};
    input.render_capacity = main_owner.state.render_capacity;
    input.render_active = main_owner.state.render_active;
    return input;
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
