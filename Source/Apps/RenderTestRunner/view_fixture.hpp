#pragma once

#include <hs/render_data/render_frame_input.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace hs::render_test
{
inline constexpr ViewId kOwnedViewProbePreview{0x50525641u};
inline constexpr ViewId kOwnedViewProbeCapture{0x43415042u};

struct OwnedViewProbeEvidence
{
    // Package order; each row is Main, Preview A, Capture B. These are the
    // actual public submission stamps, including opaque provider provenance.
    std::array<std::array<RenderViewFrameResult, 3>, 3> frames;
    std::array<std::uint32_t, 3> admitted_decals{};
    std::array<std::uint32_t, 3> admitted_distortions{};
    std::array<std::uint32_t, 3> admitted_ribbons{};
    std::array<std::uint32_t, 3> admitted_ground{};
    std::array<bool, 4> invalid_distortion_gap_rejected{};
    std::array<bool, 4> invalid_ribbon_pool_rejected{};
    std::array<bool, 4> invalid_ground_gap_rejected{};
    std::array<std::uint64_t, 2> archer_clip_ids{};
    std::uint64_t motion_epoch_target_id{}, motion_epoch_before{}, motion_epoch_after{};
    RendererFrameResult final_frame;
    std::uint64_t submissions_before{}, submissions_after_bad_preflight{}, submissions_after{};
    std::uint64_t overwrite_submission_id{};
    bool duplicate_rejected{}, nonfinite_origin_rejected{}, rejected_outputs_unchanged{}, stale_diagnostics_rejected{};
    bool nonfinite_flash_history_rejected{};
    std::array<std::filesystem::path, 2> captures;
    std::filesystem::path hdr_capture;
    nlohmann::ordered_json motion_readbacks{nlohmann::ordered_json::object()};
};

namespace owned_view_probe_detail
{
inline Result Failure(std::string message)
{
    return Result::Failure(ErrorCode::TaskFailed, "hs_render_owned_view_probe", std::move(message));
}
inline bool SameIdentity(const FrameIdentity &a, const FrameIdentity &b)
{
    return a.actual_tick == b.actual_tick && a.render_frame_id == b.render_frame_id &&
        a.view_id == b.view_id && a.presentation_id == b.presentation_id &&
        a.provider_token == b.provider_token && a.provider_token_known == b.provider_token_known;
}
inline bool SameView(const RenderViewInput &a, const RenderViewInput &b)
{
    return a.id == b.id && a.role == b.role && a.camera.position == b.camera.position &&
        a.camera.target == b.camera.target && a.camera.up.x == b.camera.up.x &&
        a.camera.up.y == b.camera.up.y && a.camera.up.z == b.camera.up.z &&
        a.camera.vertical_fov_degrees == b.camera.vertical_fov_degrees &&
        a.near_plane == b.near_plane && a.far_plane == b.far_plane &&
        a.scene_origin == b.scene_origin && a.view_origin == b.view_origin &&
        a.output_extent == b.output_extent && a.render_capacity == b.render_capacity &&
        a.render_active == b.render_active && a.camera_cut == b.camera_cut;
}
inline bool SameStamp(const RenderViewFrameResult &a, const RenderViewFrameResult &b)
{
    return SameIdentity(a.identity, b.identity) && a.submission_id == b.submission_id &&
        a.rendered_frame == b.rendered_frame && a.history_generation == b.history_generation &&
        SameView(a.rendered_view, b.rendered_view) &&
        a.unjittered_view_projection == b.unjittered_view_projection;
}
inline bool SameRejectedResultEvidence(const RendererFrameResult &a, const RendererFrameResult &b)
{
    return a.frame == b.frame && SameIdentity(a.identity, b.identity) &&
        a.view_history_generation == b.view_history_generation && SameView(a.rendered_view, b.rendered_view) &&
        a.unjittered_view_projection == b.unjittered_view_projection &&
        a.views.size() == b.views.size() && std::ranges::equal(a.views, b.views, SameStamp) &&
        !a.vfx_world_step_submitted && a.vfx_world_tick == 0 && a.vfx_world_session_id == 0 &&
        a.presented_ui_packet_generation == 0;
}
using Matrix = std::array<double, 16>;
inline Matrix StampMatrix(const RenderViewFrameResult &stamp)
{
    Matrix matrix{};
    std::ranges::transform(stamp.unjittered_view_projection, matrix.begin(),
        [](float value) { return static_cast<double>(value); });
    return matrix;
}
inline Matrix ReadMatrix(const nlohmann::ordered_json &inputs, const char *name)
{
    return inputs.at(name).get<Matrix>();
}
inline bool SameMatrix(const Matrix &a, const Matrix &b, double tolerance = 1.0e-4)
{
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::abs(a[i] - b[i]) > tolerance) return false;
    return true;
}
inline Result CaptureSurfaceReadback(D3D12Renderer &renderer, ViewId view_id,
    const std::filesystem::path &path, std::uint64_t expected_frame,
    nlohmann::ordered_json &metadata)
{
    if (auto captured = renderer.Capture({.view_id = view_id, .path = path,
            .kind = CaptureKind::SurfaceDiagnostics}); !captured) return captured;
    std::ifstream input(path);
    if (!input) return Failure("Native surface readback metadata was not written.");
    input >> metadata;
    const auto &temporal = metadata.at("temporal_inputs");
    if (temporal.is_null() || temporal.at("view_id").get<std::uint32_t>() != view_id.value ||
        temporal.at("frame").get<std::uint64_t>() != expected_frame ||
        metadata.value("foreground_depth_pixels", std::uint64_t{}) == 0)
        return Failure("Surface readback does not identify the expected rendered view/frame or foreground pixels.");
    return Result::Success();
}
} // namespace owned_view_probe_detail

// Opt-in integration probe. The caller owns the ordinary internal renderer
// configuration and snapshot leases. This helper never requests a native SDK
// provider or manufactures a provider token. It performs exactly three frame
// packages with skipped-view history and readbacks, then one auxiliary-only overwrite.
inline Result RunOwnedViewProbe(D3D12Renderer &renderer,
    const RenderSnapshotPair &snapshots, const std::filesystem::path &artifacts,
    AssetId archer_asset_id, OwnedViewProbeEvidence &evidence)
{
    using namespace owned_view_probe_detail;
    evidence = {};
    if (!snapshots.has_current)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_render_owned_view_probe",
            "Owned-view probe requires the caller's current leased snapshot pair.");
    const auto provider = renderer.GetReconstructionStatus().effective_provider.value;
    if (provider != "internal_taau" && provider != "native_taa" && provider != "off")
        return Result::Failure(ErrorCode::InvalidArgument, "hs_render_owned_view_probe",
            "Owned-view probe requires ordinary internal reconstruction configuration.");
    const auto main = renderer.MainViewInput(snapshots.current.camera);
    auto preview = main;
    preview.id = kOwnedViewProbePreview; preview.role = RenderViewRole::Preview;
    // Keep a smaller preview and a Main-sized capture to exercise slot upload growth.
    const auto half = [](ViewExtent e) { return ViewExtent{std::max(1u, e.width / 2u), std::max(1u, e.height / 2u)}; };
    preview.output_extent = half(main.output_extent);
    preview.render_capacity = half(main.render_capacity);
    preview.render_active = half(main.render_active);
    preview.camera.position.x -= 3.0; preview.camera.target.x -= 1.0;
    preview.view_origin = preview.camera.position;
    auto capture = preview;
    capture.id = kOwnedViewProbeCapture; capture.role = RenderViewRole::Capture;
    capture.output_extent = main.output_extent;
    capture.render_capacity = main.render_capacity;
    capture.render_active = main.render_active;
    capture.camera = main.camera;
    capture.camera.position.x += 4.0; capture.camera.target.x += 2.0;
    capture.view_origin = capture.camera.position;
    std::array<RenderViewInput, 3> views{main, preview, capture};
    // Start in one large global basis; moving that basis mid-probe would be a real camera cut.
    for (auto &view : views)
    {
        view.scene_origin.x = 1.0e9;
        view.camera.position.x += 1.0e9;
        view.camera.target.x += 1.0e9;
        view.view_origin.x += 1.0e9;
    }
    const std::array<VfxDecalInput, 2> decals{{
        {.stable_id = 0x44454341u, .position = {-2, 0, 0}, .radius = 8,
         .linear_rgb = {0.7f, 0.3f, 0.1f}, .hdr = 1, .alpha = 0.7f, .normalized_age = 0.2f,
         .kind = VfxDecalKind::Fracture, .stable_seed = 31, .voronoi_cells = 64, .edge_glow = 0.3f},
        {.stable_id = 0x44454342u, .position = {2, 0, 0}, .radius = 8,
         .linear_rgb = {0.2f, 0.6f, 0.3f}, .hdr = 1, .alpha = 0.7f, .normalized_age = 0.2f,
         .kind = VfxDecalKind::Organic, .stable_seed = 47, .voronoi_cells = 64, .edge_glow = 0.3f}
    }};
    const std::array<float, 3> distortion_gap_pool{0.5f, 1.25f, 2.0f};
    const VfxLineSpawnCommand effect_line{.sequence = 0x4c494e45u,
        .start = {-.5f, 0, -2}, .end = {.5f, 0, -2}, .color = {1, .5f, .1f, 1},
        .width = .1f, .lifetime = .2f, .uv_repeat = 1};
    const auto annulus = [](std::uint64_t id, Float3 position) {
        VfxDistortionInput command{};
        command.stable_id = id;
        command.position = position;
        command.radius = 1.0f; // Keep both commands within the ordinary projected-area budget.
        command.strength = 0.2f;
        command.normalized_age = 0.2f;
        command.alpha = 0.7f;
        command.shape = VfxDistortionShape::GappedAnnulus;
        command.inner_radius = 0.25f;
        command.gap_half_width_degrees = 5.0f;
        return command;
    };
    std::array<VfxDistortionInput, 2> package0_distortions{
        annulus(0x44495331u, {-2, 0, 0}), annulus(0x44495332u, {2, 0, 0})};
    package0_distortions[0].gap_angles_degrees = {0.75f};
    package0_distortions[1].frame_gap_first = 1;
    package0_distortions[1].frame_gap_count = 1;
    std::array<VfxDistortionInput, 2> package2_distortions{
        annulus(0x44495333u, {-2, 0, 0}), annulus(0x44495334u, {2, 0, 0})};
    for (auto &command : package2_distortions)
    {
        command.frame_gap_first = 0;
        command.frame_gap_count = 2;
    }
    const std::array<float, 3> ground_gap_pool{0.5f, 1.25f, 2.0f};
    const auto ground_annulus = [](std::uint64_t id, Float3 position) {
        VfxGroundSpawnInput input{};
        input.effect_handle = 1; input.stable_id = id; input.hdr = 1;
        input.command.renderer = VfxRenderer::Ground;
        input.command.primitive = VfxPrimitive::ExactRing;
        input.command.position = position; input.command.start_size_min = 2;
        input.geometry.shape = VfxGroundShape::RingGapsBorder;
        input.geometry.inner_radius = 0.5f; input.geometry.outer_radius = 2;
        input.geometry.edge_width = 0.1f; input.geometry.gap_half_angle_radians = 0.1f;
        return input;
    };
    std::array<VfxGroundSpawnInput, 2> package0_ground{
        ground_annulus(0x47524e31u, {-2, 0, 0}), ground_annulus(0x47524e32u, {2, 0, 0})};
    package0_ground[0].geometry.gap_angles_radians = {0.75f};
    package0_ground[1].geometry.frame_gap_first = 1;
    package0_ground[1].geometry.frame_gap_count = 1;
    std::array<VfxGroundSpawnInput, 2> package2_ground{
        ground_annulus(0x47524e33u, {-2, 0, 0}), ground_annulus(0x47524e34u, {2, 0, 0})};
    for (auto &command : package2_ground) command.geometry.frame_gap_count = 2;
    const std::array<VfxRibbonOutputInput, 2> ribbon_outputs{{
        {.profile = VfxOutputProfile::RibbonAdd, .motion = VfxMotionKind::CurvedLinkTravelingHead,
         .color = {1, .3f, .1f, 1}, .width_head = .12f, .width_tail = .04f, .hdr = 1},
        {.profile = VfxOutputProfile::RibbonOit, .motion = VfxMotionKind::SoftWiderSheath,
         .color = {.2f, .5f, 1, .5f}, .width_head = .2f, .width_tail = .08f, .hdr = 1}
    }};
    VfxRibbonSourceInput owning_ribbon;
    owning_ribbon.owner_id = 0x52494242u;
    owning_ribbon.effect_handle = 1; owning_ribbon.source_id = 1;
    owning_ribbon.position = {.5f, 1, 0}; owning_ribbon.previous_position = {-.5f, 1, 0};
    owning_ribbon.analytic = true; owning_ribbon.control = {0, 1.25f, 0};
    owning_ribbon.segments = 8; owning_ribbon.normalized_age = .4f;
    owning_ribbon.outputs.assign(ribbon_outputs.begin(), ribbon_outputs.end());
    auto pooled_ribbon = owning_ribbon;
    pooled_ribbon.outputs.clear(); pooled_ribbon.frame_output_count = 2;
    std::vector<RenderInstance> moving_instances(snapshots.current.instances.begin(), snapshots.current.instances.end());
    const auto archer = std::ranges::find(moving_instances, RenderMesh::Archer, &RenderInstance::mesh);
    const auto clips = renderer.GetCharacterAnimationClips();
    if (archer == moving_instances.end() || clips.size() < 2 || !archer_asset_id.value)
        return Failure("Owned-view deformation probe requires an Archer and two actual cooked named clips.");
    archer->asset_id = archer_asset_id;
    const auto moving_archer_id = archer->stable_id;
    archer->animation_clip_id = clips[0].id;
    archer->animation_time_seconds = clips[0].duration_seconds * 0.2;
    auto second_archer = *archer;
    second_archer.stable_id = 0x41524348u;
    if (std::ranges::find(moving_instances, second_archer.stable_id,
            &RenderInstance::stable_id) != moving_instances.end())
        return Failure("Owned-view epoch target stable ID already exists in the admitted scene.");
    second_archer.position.x += 3;
    second_archer.animation_clip_id = clips[1].id;
    second_archer.animation_time_seconds = clips[1].duration_seconds * 0.6;
    evidence.motion_epoch_target_id = second_archer.stable_id;
    evidence.motion_epoch_before = second_archer.motion_epoch;
    evidence.motion_epoch_after = second_archer.motion_epoch + 1u;
    evidence.archer_clip_ids = {clips[0].id.value, clips[1].id.value};
    moving_instances.push_back(second_archer);
    auto package_snapshots = snapshots;
    package_snapshots.current.instances = moving_instances;
    std::error_code error;
    std::filesystem::create_directories(artifacts, error);
    if (error) return Failure("Cannot create owned-view capture directory: " + error.message());
    evidence.submissions_before = renderer.GetDiagnostics().total_rendered_frames;
    RendererFrameResult result;
    nlohmann::ordered_json preview_before_skip;
    nlohmann::ordered_json capture_origin_reset;
    for (std::size_t package = 0; package < evidence.frames.size(); ++package)
    {
        if (package == 1)
        {
            // Only Capture changes its basis; skipped Preview retains its own history.
            views[2].camera.position.x += 2.0;
            views[2].camera.target.x += 0.25;
            views[2].view_origin.x += 64.0;
            views[2].camera_cut = true;
        }
        else if (package == 2)
        {
            views[2].camera_cut = false;
            // Reappear Preview with a small camera movement. Its previous GPU
            // projection must come from package 0, not Main or the rebased B.
            views[1].camera.position.x += 0.5;
            views[1].camera.target.x += 0.5;
            views[1].view_origin.x += 0.5;
            auto changed_epoch = std::ranges::find(moving_instances, 0x41524348u,
                &RenderInstance::stable_id);
            if (changed_epoch == moving_instances.end())
                return Failure("Owned-view epoch discontinuity target was not retained in the packet scene.");
            changed_epoch->position.x += 0.75f;
            ++changed_epoch->revisions.transform;
            ++changed_epoch->motion_epoch;
        }
        if (package && !moving_instances.empty())
        {
            auto moving_archer = std::ranges::find(moving_instances, moving_archer_id,
                &RenderInstance::stable_id);
            if (moving_archer == moving_instances.end())
                return Failure("Owned-view motion target was not retained in the packet scene.");
            moving_archer->position.x += 0.25f;
            ++moving_archer->revisions.transform;
        }
        const auto admitted = package == 1 ? std::span<const VfxDecalInput>{} : std::span<const VfxDecalInput>{decals};
        const std::span<const VfxDistortionInput> admitted_distortions = package == 0
            ? std::span<const VfxDistortionInput>{package0_distortions}
            : package == 2 ? std::span<const VfxDistortionInput>{package2_distortions}
                           : std::span<const VfxDistortionInput>{};
        const std::vector<RenderViewInput> submitted_views = package == 0
            ? std::vector<RenderViewInput>{views[0], views[2], views[1]}
            : package == 1 ? std::vector<RenderViewInput>{views[0], views[2]}
                           : std::vector<RenderViewInput>{views[0], views[2], views[1]};
        const RenderVfxWorldBatch world{
            .session = package_snapshots.current.header.session_id,
            .stateful_target = package_snapshots.current.header.tick == 0 ? 0 : package_snapshots.current.header.tick - 1,
            .sequence = package == 1 ? 0u : package == 0 ? 1u : 2u};
        const ParticleSpawnCommand particle_birth{
            .sequence = 0x50415254u,
            .tick = world.stateful_target,
            .position = {0, 0, -2},
            .shape = ParticleShape::Point,
            .velocity_mode = ParticleVelocity::Direction,
            .facing = ParticleFacing::Camera,
            .renderer = VfxRenderer::Sprite,
            .primitive = VfxPrimitive::Soft,
            .direction = {0, 0, 1},
            .lifetime_min = 3600.0f,
            .lifetime_max = 3600.0f,
            .start_color = {1, 0.25f, 0.1f, 1},
            .end_color = {1, 0.25f, 0.1f, 0},
            .start_size_min = 0.25f,
            .start_size_max = 0.25f,
            .count = 1,
            .seed = 0x51a7u};
        const RenderFrameInput input{.views = submitted_views, .snapshots = package_snapshots,
            .vfx = {.particle_spawns = package == 0 ? std::span(&particle_birth, 1) :
                    std::span<const ParticleSpawnCommand>{},
                .effect_lines = world.sequence ? std::span(&effect_line, 1) : std::span<const VfxLineSpawnCommand>{},
                .ground = package == 1 ? std::span<const VfxGroundSpawnInput>{} :
                    package == 0 ? std::span<const VfxGroundSpawnInput>{package0_ground} :
                                   std::span<const VfxGroundSpawnInput>{package2_ground},
                .ribbons = {package == 0 ? std::span(&owning_ribbon, 1) : std::span(&pooled_ribbon, 1),
                               1, ribbon_outputs},
                .distortions = admitted_distortions, .decals = admitted,
                .world_batch = world, .distortion_gap_angles = distortion_gap_pool,
                .ground_gap_angles = ground_gap_pool},
            .delta_seconds = 1.0f / 60.0f};
        if (auto rendered = renderer.Render(input, result); !rendered) return rendered;
        if ((world.sequence == 0 && result.vfx_world_step_submitted) ||
            (world.sequence != 0 && (!result.vfx_world_step_submitted ||
                result.vfx_world_tick != world.stateful_target || result.vfx_world_session_id != world.session ||
                result.vfx_world_batch_sequence != world.sequence)))
            return Failure("World receipt must identify its frozen producer batch independently of snapshot tick and view order.");
        evidence.admitted_decals[package] = result.typed_vfx_decal_count;
        evidence.admitted_distortions[package] = result.typed_vfx_distortion_count;
        evidence.admitted_ribbons[package] = result.typed_vfx_ribbon_count;
        evidence.admitted_ground[package] = result.typed_vfx_ground_count;
        if (result.typed_vfx_ground_count != (package == 1 ? 0u : 2u))
            return Failure("Ground owning/pooled gap package did not retain its commands or clear the empty package.");
        if (result.typed_vfx_ribbon_count != ribbon_outputs.size())
            return Failure("Owning/pooled ribbon packet did not retain both analytic outputs.");
        if (result.typed_vfx_decal_count != admitted.size())
            return Failure("Decal Surface probe did not admit both authored command kinds or clear the empty package.");
        const auto expected_distortions = package == 1 ? 0u : 2u;
        if (result.typed_vfx_distortion_count != expected_distortions)
            return Failure("Distortion gap-pool package " + std::to_string(package) +
                " admitted " + std::to_string(result.typed_vfx_distortion_count) +
                " commands; expected " + std::to_string(expected_distortions) + ".");
        if (result.views.size() != submitted_views.size() || result.identity.view_id != kMainRenderViewId)
            return Failure("Frame package did not return its actual keyed submissions and Main result.");
        for (std::size_t view = 0; view < submitted_views.size(); ++view)
            if (result.views[view].identity.view_id != submitted_views[view].id)
                return Failure("Main must render first while auxiliary views retain their packet order.");
        const auto package_id = result.identity.render_frame_id;
        if (!package_id || (package && package_id != evidence.frames[package - 1][0].identity.render_frame_id + 1u))
            return Failure("Rejected preflight or sequential reuse changed the actual render-package identity.");
        for (std::size_t view = 0; view < submitted_views.size(); ++view)
        {
            const auto found = std::ranges::find_if(result.views, [&](const auto &stamp) {
                return stamp.identity.view_id == submitted_views[view].id;
            });
            if (found == result.views.end() ||
                std::ranges::count(result.views, submitted_views[view].id, [](const auto &stamp) { return stamp.identity.view_id; }) != 1 ||
                found->identity.render_frame_id != package_id || found->identity.actual_tick != snapshots.current.header.tick ||
                !found->submission_id || !found->rendered_frame || !found->history_generation ||
                !SameView(found->rendered_view, submitted_views[view]) ||
                !std::ranges::all_of(found->unjittered_view_projection, [](float v) { return std::isfinite(v); }) ||
                (view != 0 && found->identity.presentation_id != 0))
                return Failure("Returned view identity, camera, history, matrix or presentation ownership disagrees with its actual submission.");
            const auto ordinal = found->identity.view_id == kMainRenderViewId ? 0u :
                found->identity.view_id == kOwnedViewProbePreview ? 1u : 2u;
            evidence.frames[package][ordinal] = *found;
            const bool has_previous = package != 0 && !(package == 1 && ordinal == 1);
            if (has_previous)
            {
                const auto previous_package = package == 2 && ordinal == 1 ? 0u : package - 1u;
                const auto &old = evidence.frames[previous_package][ordinal];
                const auto increment = package == 1 && ordinal == 2 ? 1u : 0u;
                if (found->submission_id <= old.submission_id || found->rendered_frame != old.rendered_frame + 1u ||
                    found->history_generation != old.history_generation + increment)
                    return Failure("A successful view render must advance its own sequence; only Capture's explicit cut advances history generation.");
                const bool same_matrix = found->unjittered_view_projection == old.unjittered_view_projection;
                if (package == 1 && ordinal == 2 && same_matrix)
                    return Failure("Capture's large origin/camera discontinuity did not change its projected basis.");
                if (package == 2 && ordinal == 1 && same_matrix)
                    return Failure("Reappearing Preview camera movement did not update its current projection.");
            }
        }
        const auto &main_stamp = evidence.frames[package][0];
        if (result.frame != main_stamp.rendered_frame || result.view_history_generation != main_stamp.history_generation ||
            result.unjittered_view_projection != main_stamp.unjittered_view_projection)
            return Failure("Auxiliary rendering changed the public primary Main frame result.");
        if (package == 0)
        {
            const auto before = renderer.GetDiagnostics().total_rendered_frames;
            const auto accepted = result;
            auto duplicate = views; duplicate[2].id = duplicate[1].id;
            const RenderFrameInput duplicate_input{.views = duplicate, .snapshots = snapshots};
            const auto rejected_duplicate = renderer.Render(duplicate_input, result);
            evidence.duplicate_rejected = !rejected_duplicate && rejected_duplicate.Code() == ErrorCode::InvalidArgument;
            const bool duplicate_preserved = SameRejectedResultEvidence(result, accepted) &&
                renderer.GetDiagnostics().total_rendered_frames == before;
            auto nonfinite = views; nonfinite[2].view_origin.x = std::numeric_limits<double>::quiet_NaN();
            const RenderFrameInput nonfinite_input{.views = nonfinite, .snapshots = snapshots};
            const auto rejected_nonfinite = renderer.Render(nonfinite_input, result);
            evidence.nonfinite_origin_rejected = !rejected_nonfinite && rejected_nonfinite.Code() == ErrorCode::InvalidArgument;
            const bool nonfinite_preserved = SameRejectedResultEvidence(result, accepted) &&
                renderer.GetDiagnostics().total_rendered_frames == before;
            VfxFlashSpawnInput flash{};
            flash.size = flash.lifetime = flash.hdr = 1.0f;
            flash.interpolate_owner_position = true;
            flash.previous_position.x = std::numeric_limits<float>::quiet_NaN();
            RenderFrameInput bad_flash_input{.views = views, .snapshots = snapshots};
            bad_flash_input.vfx.flashes = std::span(&flash, 1);
            const auto rejected_flash = renderer.Render(bad_flash_input, result);
            evidence.nonfinite_flash_history_rejected = !rejected_flash &&
                rejected_flash.Code() == ErrorCode::InvalidArgument;
            auto bad_world_input = input;
            ++bad_world_input.vfx.world_batch->session;
            const auto rejected_world = renderer.Render(bad_world_input, result);
            if (rejected_world || rejected_world.Code() != ErrorCode::InvalidArgument ||
                !SameRejectedResultEvidence(result, accepted) || renderer.GetDiagnostics().total_rendered_frames != before)
                return Failure("A VFX batch from another session must be rejected before any native submission.");
            VfxLightInput bad_light{};
            bad_light.radius = 1.0f;
            bad_light.intensity = std::numeric_limits<float>::quiet_NaN();
            VfxDecalInput bad_decal{};
            bad_decal.radius = 1.0f;
            bad_decal.alpha = std::numeric_limits<float>::quiet_NaN();
            ParticleSpawnCommand bad_ballistic{};
            bad_ballistic.authored_ballistic = true;
            bad_ballistic.renderer = VfxRenderer::Mesh;
            bad_ballistic.mesh_index = 1;
            bad_ballistic.lifetime_min = bad_ballistic.lifetime_max = bad_ballistic.authored_hdr = 1.0f;
            bad_ballistic.authored_drag = std::numeric_limits<float>::quiet_NaN();
            ParticleSpawnCommand bad_selector{};
            bad_selector.mesh_index = 8;
            std::array<RenderFrameInput, 17> invalid_payloads;
            for (auto &invalid : invalid_payloads) { invalid.views = views; invalid.snapshots = snapshots; }
            invalid_payloads[0].vfx.lights = std::span(&bad_light, 1);
            invalid_payloads[1].vfx.decals = std::span(&bad_decal, 1);
            invalid_payloads[2].vfx.particle_spawns = std::span(&bad_ballistic, 1);
            invalid_payloads[3].vfx.particle_spawns = std::span(&bad_selector, 1);
            auto missing_gap_pool = annulus(0x494e5641u, {-2, 0, 0});
            missing_gap_pool.frame_gap_count = 1;
            auto out_of_range_gap_pool = annulus(0x494e5642u, {-2, 0, 0});
            out_of_range_gap_pool.frame_gap_first = 2;
            out_of_range_gap_pool.frame_gap_count = 2;
            auto mixed_gap_storage = annulus(0x494e5643u, {-2, 0, 0});
            mixed_gap_storage.gap_angles_degrees = {0.5f};
            mixed_gap_storage.frame_gap_count = 1;
            auto nonfinite_gap_pool = annulus(0x494e5644u, {-2, 0, 0});
            nonfinite_gap_pool.frame_gap_count = 1;
            const std::array<float, 1> nan_gap_pool{std::numeric_limits<float>::quiet_NaN()};
            invalid_payloads[4].vfx.distortions = std::span(&missing_gap_pool, 1);
            invalid_payloads[5].vfx.distortions = std::span(&out_of_range_gap_pool, 1);
            invalid_payloads[5].vfx.distortion_gap_angles = distortion_gap_pool;
            invalid_payloads[6].vfx.distortions = std::span(&mixed_gap_storage, 1);
            invalid_payloads[6].vfx.distortion_gap_angles = distortion_gap_pool;
            invalid_payloads[7].vfx.distortions = std::span(&nonfinite_gap_pool, 1);
            invalid_payloads[7].vfx.distortion_gap_angles = nan_gap_pool;
            std::array<VfxRibbonSourceInput, 4> bad_ribbons{pooled_ribbon, pooled_ribbon,
                pooled_ribbon, pooled_ribbon};
            bad_ribbons[1].frame_output_first = 1;
            bad_ribbons[2].outputs = {ribbon_outputs[0]};
            auto bad_ribbon_outputs = ribbon_outputs;
            bad_ribbon_outputs[0].width_head = std::numeric_limits<float>::quiet_NaN();
            for (std::size_t i = 0; i < bad_ribbons.size(); ++i)
                invalid_payloads[8 + i].vfx.ribbons = {std::span(&bad_ribbons[i], 1), 1,
                    i == 0 ? std::span<const VfxRibbonOutputInput>{} :
                    i == 3 ? std::span<const VfxRibbonOutputInput>{bad_ribbon_outputs} :
                             std::span<const VfxRibbonOutputInput>{ribbon_outputs}};
            std::array<VfxGroundSpawnInput, 4> bad_ground{package2_ground[0], package2_ground[0],
                package2_ground[0], package2_ground[0]};
            bad_ground[1].geometry.frame_gap_first = 2;
            bad_ground[2].geometry.gap_angles_radians = {0.5f};
            for (std::size_t i = 0; i < bad_ground.size(); ++i)
            {
                bad_ground[i].geometry.frame_gap_count = 2;
                invalid_payloads[12 + i].vfx.ground = std::span(&bad_ground[i], 1);
                invalid_payloads[12 + i].vfx.ground_gap_angles =
                    i == 0 ? std::span<const float>{} :
                    i == 3 ? std::span<const float>{nan_gap_pool} : std::span<const float>{ground_gap_pool};
            }
            bad_ground[3].geometry.frame_gap_count = 1;
            invalid_payloads[16].vfx.effect_lines = std::span(&effect_line, 1);
            invalid_payloads[16].vfx.world_batch = RenderVfxWorldBatch{
                .session = snapshots.current.header.session_id,
                .stateful_target = world.stateful_target};
            for (std::size_t invalid_index = 0; invalid_index < invalid_payloads.size(); ++invalid_index)
            {
                const auto &invalid = invalid_payloads[invalid_index];
                const auto rejected = renderer.Render(invalid, result);
                if (rejected || rejected.Code() != ErrorCode::InvalidArgument ||
                    !SameRejectedResultEvidence(result, accepted) || renderer.GetDiagnostics().total_rendered_frames != before)
                    return Failure("VFX admission must reject malformed payloads and frame-pool storage before any view submission.");
                if (invalid_index >= 4 && invalid_index < 8)
                    evidence.invalid_distortion_gap_rejected[invalid_index - 4] = true;
                else if (invalid_index >= 8 && invalid_index < 12)
                    evidence.invalid_ribbon_pool_rejected[invalid_index - 8] = true;
                else if (invalid_index >= 12 && invalid_index < 16)
                    evidence.invalid_ground_gap_rejected[invalid_index - 12] = true;
            }
            evidence.submissions_after_bad_preflight = renderer.GetDiagnostics().total_rendered_frames;
            evidence.rejected_outputs_unchanged = duplicate_preserved && nonfinite_preserved && SameRejectedResultEvidence(result, accepted) &&
                evidence.submissions_after_bad_preflight == before;
            if (!evidence.duplicate_rejected || !evidence.nonfinite_origin_rejected ||
                !evidence.nonfinite_flash_history_rejected || !evidence.rejected_outputs_unchanged ||
                !std::ranges::all_of(evidence.invalid_distortion_gap_rejected,
                    [](bool rejected) { return rejected; }) ||
                !std::ranges::all_of(evidence.invalid_ribbon_pool_rejected,
                    [](bool rejected) { return rejected; }) ||
                !std::ranges::all_of(evidence.invalid_ground_gap_rejected,
                    [](bool rejected) { return rejected; }))
                return Failure("Duplicate/nonfinite view or flash preflight was accepted or mutated submission/result evidence.");
        }
        if (package == 0)
        {
            if (auto captured = CaptureSurfaceReadback(renderer, kOwnedViewProbePreview,
                    artifacts / "owned-view-preview-before-skip.json",
                    evidence.frames[0][1].rendered_frame, preview_before_skip); !captured) return captured;
            if (!preview_before_skip.value("decal_surface_active", false))
                return Failure("Preview capture did not use its submitted decal Surface exports.");
            evidence.motion_readbacks["preview_before_skip"] = preview_before_skip;
        }
        else if (package == 1)
        {
            if (auto captured = CaptureSurfaceReadback(renderer, kOwnedViewProbeCapture,
                    artifacts / "owned-view-capture-origin-reset.json",
                    evidence.frames[1][2].rendered_frame, capture_origin_reset); !captured) return captured;
            if (capture_origin_reset.value("decal_surface_active", true))
                return Failure("Empty Capture frame retained a stale decal Surface export.");
            const auto &inputs = capture_origin_reset.at("temporal_inputs");
            const auto current = ReadMatrix(inputs, "current_vp");
            const auto previous_instance = ReadMatrix(inputs,
                "previous_instance_unjittered_view_projection");
            if (inputs.at("options").at(1).get<double>() > 0.5 ||
                !SameMatrix(previous_instance, current))
                return Failure("Capture camera_cut must invalidate instance history and use the current projection after rebasing.");
            evidence.motion_readbacks["capture_origin_reset"] = capture_origin_reset;
        }
        else if (package == 2)
        {
            nlohmann::ordered_json preview_after_skip;
            if (auto captured = CaptureSurfaceReadback(renderer, kOwnedViewProbePreview,
                    artifacts / "owned-view-preview-after-skip.json",
                    evidence.frames[2][1].rendered_frame, preview_after_skip); !captured) return captured;
            if (!preview_after_skip.value("decal_surface_active", false))
                return Failure("Reappearing Preview capture did not use its new decal Surface exports.");
            const auto previous = ReadMatrix(preview_after_skip.at("temporal_inputs"),
                "previous_instance_unjittered_view_projection");
            const auto old_preview_current = ReadMatrix(preview_before_skip.at("temporal_inputs"), "current_vp");
            const auto current_preview = ReadMatrix(preview_after_skip.at("temporal_inputs"), "current_vp");
            const auto unrelated_capture_current = ReadMatrix(capture_origin_reset.at("temporal_inputs"), "current_vp");
            if (!SameMatrix(previous, old_preview_current) ||
                !SameMatrix(old_preview_current, StampMatrix(evidence.frames[0][1])) ||
                SameMatrix(previous, unrelated_capture_current) ||
                SameMatrix(previous, current_preview) ||
                preview_after_skip.value("nonzero_motion_pixels", std::uint64_t{}) == 0)
                return Failure("Reappearing Preview did not retain its own old-basis projection and real nonzero motion after another view's origin reset.");
            evidence.motion_readbacks["preview_after_skip"] = std::move(preview_after_skip);
        }
    }
    evidence.final_frame = std::move(result);
    // The three submission banks still hold Main after package 2. Reuse its bank
    // with a real auxiliary submission before testing stale diagnostic rejection.
    const RenderVfxWorldBatch held_world{.session = package_snapshots.current.header.session_id,
        .stateful_target = package_snapshots.current.header.tick == 0 ? 0 : package_snapshots.current.header.tick - 1};
    const RenderFrameInput overwrite{.views = std::span(&views[1], 1), .snapshots = package_snapshots,
        .vfx = {.ground = package2_ground,
            .ribbons = {std::span(&pooled_ribbon, 1), 1, ribbon_outputs},
            .distortions = package2_distortions, .decals = decals,
            .world_batch = held_world, .distortion_gap_angles = distortion_gap_pool,
            .ground_gap_angles = ground_gap_pool},
        .delta_seconds = 1.0f / 60.0f};
    RendererFrameResult overwrite_result;
    if (auto submitted = renderer.Render(overwrite, overwrite_result); !submitted) return submitted;
    if (overwrite_result.views.size() != 1 || overwrite_result.views[0].identity.view_id != kOwnedViewProbePreview ||
        overwrite_result.views[0].submission_id <= evidence.frames[2][1].submission_id ||
        overwrite_result.vfx_world_step_submitted)
        return Failure("Auxiliary overwrite did not submit independently while holding the existing world batch.");
    evidence.overwrite_submission_id = overwrite_result.views[0].submission_id;
    evidence.submissions_after = renderer.GetDiagnostics().total_rendered_frames;
    if (evidence.submissions_after != evidence.submissions_before + 9u)
        return Failure("Owned-view probe did not produce eight scene-view submissions plus one auxiliary overwrite.");
    const auto stale_capture = renderer.Capture({.path = artifacts / "stale-main-geometry.json",
        .kind = CaptureKind::GeometryDiagnostics});
    evidence.stale_diagnostics_rejected = !stale_capture && stale_capture.Code() == ErrorCode::InvalidState;
    if (!evidence.stale_diagnostics_rejected)
        return Failure("Main geometry capture must reject shared frame buffers overwritten by an auxiliary view.");
    evidence.captures = {artifacts / "owned-view-preview-a.png", artifacts / "owned-view-capture-b.png"};
    if (auto captured = renderer.Capture({.view_id = kOwnedViewProbePreview, .path = evidence.captures[0]}); !captured) return captured;
    if (auto captured = renderer.Capture({.view_id = kOwnedViewProbeCapture, .path = evidence.captures[1]}); !captured) return captured;
    evidence.hdr_capture = artifacts / "owned-view-capture-b.hdr";
    if (auto captured = renderer.Capture({.view_id = kOwnedViewProbeCapture,
            .path = evidence.hdr_capture, .kind = CaptureKind::HdrColor}); !captured) return captured;
    const auto live_particle_draws = renderer.LastParticleCount();
    // The capture includes the one long-lived package birth plus one short-lived
    // converted effect-line particle in each nonempty world batch (packages 0 and 2).
    if (live_particle_draws != 3)
        return Failure("The package particle birth and two effect-line particles must survive the owned-view sequence; captured " +
            std::to_string(live_particle_draws) + " live draw instances, expected 3.");
    return Result::Success();
}

inline nlohmann::ordered_json OwnedViewProbeReceipt(const OwnedViewProbeEvidence &evidence)
{
    using Json = nlohmann::ordered_json;
    Json packages = Json::array();
    for (const auto &package : evidence.frames)
    {
        Json views = Json::array();
        for (const auto &stamp : package)
        {
            const auto &v = stamp.rendered_view;
            views.push_back({{"submitted", stamp.submission_id != 0},
                {"view_id", stamp.identity.view_id.value},
                {"role", v.role == RenderViewRole::Main ? "main" : v.role == RenderViewRole::Preview ? "preview" : "capture"},
                {"actual_tick", stamp.identity.actual_tick}, {"render_package_id", stamp.identity.render_frame_id},
                {"submission_id", stamp.submission_id}, {"view_render_sequence", stamp.rendered_frame},
                {"presentation_id", stamp.identity.presentation_id}, {"provider_token_known", stamp.identity.provider_token_known},
                {"provider_token", stamp.identity.provider_token_known ? Json(stamp.identity.provider_token) : Json(nullptr)},
                {"history_generation", stamp.history_generation}, {"camera_cut", v.camera_cut},
                {"scene_origin", {v.scene_origin.x, v.scene_origin.y, v.scene_origin.z}},
                {"view_origin", {v.view_origin.x, v.view_origin.y, v.view_origin.z}},
                {"camera_position", {v.camera.position.x, v.camera.position.y, v.camera.position.z}},
                {"camera_target", {v.camera.target.x, v.camera.target.y, v.camera.target.z}},
                {"output_extent", {v.output_extent.width, v.output_extent.height}},
                {"unjittered_view_projection", stamp.unjittered_view_projection}});
        }
        packages.push_back(std::move(views));
    }
    return {{"packages", std::move(packages)}, {"overwrite_submission_id", evidence.overwrite_submission_id},
        {"admitted_decals", evidence.admitted_decals},
        {"admitted_distortions", evidence.admitted_distortions},
        {"admitted_ribbons", evidence.admitted_ribbons},
        {"admitted_ground", evidence.admitted_ground},
        {"invalid_distortion_gap_rejected", evidence.invalid_distortion_gap_rejected},
        {"invalid_ribbon_pool_rejected", evidence.invalid_ribbon_pool_rejected},
        {"invalid_ground_gap_rejected", evidence.invalid_ground_gap_rejected},
        {"archer_clip_ids", evidence.archer_clip_ids},
        {"motion_epoch_break", {{"stable_id", evidence.motion_epoch_target_id},
            {"before", evidence.motion_epoch_before}, {"after", evidence.motion_epoch_after}}},
        {"motion_readbacks", evidence.motion_readbacks},
        {"submissions_before", evidence.submissions_before},
        {"submissions_after_bad_preflight", evidence.submissions_after_bad_preflight},
        {"submissions_after", evidence.submissions_after}, {"duplicate_rejected", evidence.duplicate_rejected},
        {"nonfinite_origin_rejected", evidence.nonfinite_origin_rejected},
        {"nonfinite_flash_history_rejected", evidence.nonfinite_flash_history_rejected},
        {"rejected_outputs_unchanged", evidence.rejected_outputs_unchanged},
        {"stale_diagnostics_rejected", evidence.stale_diagnostics_rejected},
        {"captures", {evidence.captures[0].filename().string(), evidence.captures[1].filename().string()}},
        {"hdr_capture", evidence.hdr_capture.filename().string()}};
}
} // namespace hs::render_test
