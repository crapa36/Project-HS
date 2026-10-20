#pragma once

#include <hs/renderer/render_frame_input.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>

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
    std::array<std::uint64_t, 2> archer_clip_ids{};
    RendererFrameResult final_frame;
    std::uint64_t submissions_before{}, submissions_after_bad_preflight{}, submissions_after{};
    bool duplicate_rejected{}, nonfinite_origin_rejected{}, rejected_outputs_unchanged{}, stale_diagnostics_rejected{};
    std::array<std::filesystem::path, 2> captures;
    std::filesystem::path hdr_capture;
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
inline bool SameResultEvidence(const RendererFrameResult &a, const RendererFrameResult &b)
{
    return a.frame == b.frame && SameIdentity(a.identity, b.identity) &&
        a.view_history_generation == b.view_history_generation && SameView(a.rendered_view, b.rendered_view) &&
        a.unjittered_view_projection == b.unjittered_view_projection &&
        a.views.size() == b.views.size() && std::ranges::equal(a.views, b.views, SameStamp);
}
} // namespace owned_view_probe_detail

// Opt-in integration probe. The caller owns the ordinary internal renderer
// configuration and snapshot leases. This helper never requests a native SDK
// provider or manufactures a provider token. It performs exactly three frame
// packages, two rejected preflights and two captures of persistent view output.
inline Result RunOwnedViewProbe(D3D12Renderer &renderer,
    const RenderSnapshotExchange::ReadPair &snapshots, const std::filesystem::path &artifacts,
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
    // Preserve the configured active/capacity ratio without imposing native
    // provider eligibility or consuming full Main-sized auxiliary images.
    const auto half = [](ViewExtent e) { return ViewExtent{std::max(1u, e.width / 2u), std::max(1u, e.height / 2u)}; };
    preview.output_extent = half(main.output_extent);
    preview.render_capacity = half(main.render_capacity);
    preview.render_active = half(main.render_active);
    preview.camera.position.x -= 3.0; preview.camera.target.x -= 1.0;
    preview.view_origin = preview.camera.position;
    auto capture = preview;
    capture.id = kOwnedViewProbeCapture; capture.role = RenderViewRole::Capture;
    capture.camera = main.camera;
    capture.camera.position.x += 4.0; capture.camera.target.x += 2.0;
    capture.view_origin = capture.camera.position;
    std::array<RenderViewInput, 3> views{main, preview, capture};
    const std::array<VfxDecalInput, 2> decals{{
        {.stable_id = 0x44454341u, .position = {-2, 0, 0}, .radius = 8,
         .linear_rgb = {0.7f, 0.3f, 0.1f}, .hdr = 1, .alpha = 0.7f, .normalized_age = 0.2f,
         .kind = VfxDecalKind::Fracture, .stable_seed = 31, .voronoi_cells = 64, .edge_glow = 0.3f},
        {.stable_id = 0x44454342u, .position = {2, 0, 0}, .radius = 8,
         .linear_rgb = {0.2f, 0.6f, 0.3f}, .hdr = 1, .alpha = 0.7f, .normalized_age = 0.2f,
         .kind = VfxDecalKind::Organic, .stable_seed = 47, .voronoi_cells = 64, .edge_glow = 0.3f}
    }};
    std::vector<RenderInstance> moving_instances(snapshots.current.instances.begin(), snapshots.current.instances.end());
    const auto archer = std::ranges::find(moving_instances, RenderMesh::Archer, &RenderInstance::mesh);
    const auto clips = renderer.GetCharacterAnimationClips();
    if (archer == moving_instances.end() || clips.size() < 2 || !archer_asset_id.value)
        return Failure("Owned-view deformation probe requires an Archer and two actual cooked named clips.");
    archer->asset_id = archer_asset_id;
    archer->animation_clip_id = clips[0].id;
    archer->animation_time_seconds = clips[0].duration_seconds * 0.2;
    auto second_archer = *archer;
    second_archer.stable_id = 0x41524348u;
    second_archer.position.x += 3;
    second_archer.animation_clip_id = clips[1].id;
    second_archer.animation_time_seconds = clips[1].duration_seconds * 0.6;
    evidence.archer_clip_ids = {clips[0].id.value, clips[1].id.value};
    moving_instances.push_back(second_archer);
    auto package_snapshots = snapshots;
    package_snapshots.current.instances = moving_instances;
    evidence.submissions_before = renderer.GetDiagnostics().total_rendered_frames;
    RendererFrameResult result;
    for (std::size_t package = 0; package < evidence.frames.size(); ++package)
    {
        if (package == 1)
        {
            // Keep the scene coordinate convention shared. Only B changes its
            // camera and view-relative origin, with one explicit discontinuity.
            views[2].camera.position.x += 2.0;
            views[2].camera.target.x += 0.25;
            views[2].view_origin.x += 64.0;
            views[2].camera_cut = true;
        }
        else if (package == 2) views[2].camera_cut = false;
        if (package && !moving_instances.empty())
        {
            moving_instances.front().position.x += 0.75f;
            ++moving_instances.front().revisions.transform;
        }
        const auto admitted = package == 1 ? std::span<const VfxDecalInput>{} : std::span<const VfxDecalInput>{decals};
        const std::array submitted_views = package == 0 ? views :
            package == 1 ? std::array{views[1], views[0], views[2]} : std::array{views[1], views[2], views[0]};
        const RenderFrameInput input{.views = submitted_views, .snapshots = package_snapshots,
            .vfx = {.decals = admitted}, .delta_seconds = 1.0f / 60.0f};
        if (auto rendered = renderer.Render(input, result); !rendered) return rendered;
        evidence.admitted_decals[package] = result.typed_vfx_decal_count;
        if (result.typed_vfx_decal_count != admitted.size())
            return Failure("Decal Surface probe did not admit both authored command kinds or clear the empty package.");
        if (result.views.size() != views.size() || result.identity.view_id != kMainRenderViewId)
            return Failure("Frame package did not return all actual keyed view submissions and its Main result.");
        for (std::size_t view = 0; view < views.size(); ++view)
            if (result.views[view].identity.view_id != views[view].id)
                return Failure("Main must render first while auxiliary views retain their packet order.");
        const auto package_id = result.identity.render_frame_id;
        if (!package_id || (package && package_id != evidence.frames[package - 1][0].identity.render_frame_id + 1u))
            return Failure("Rejected preflight or sequential reuse changed the actual render-package identity.");
        for (std::size_t view = 0; view < views.size(); ++view)
        {
            const auto found = std::ranges::find_if(result.views, [&](const auto &stamp) {
                return stamp.identity.view_id == views[view].id;
            });
            if (found == result.views.end() ||
                std::ranges::count(result.views, views[view].id, [](const auto &stamp) { return stamp.identity.view_id; }) != 1 ||
                found->identity.render_frame_id != package_id || found->identity.actual_tick != snapshots.current.header.tick ||
                !found->submission_id || !found->rendered_frame || !found->history_generation ||
                !SameView(found->rendered_view, views[view]) ||
                !std::ranges::all_of(found->unjittered_view_projection, [](float v) { return std::isfinite(v); }) ||
                (view != 0 && found->identity.presentation_id != 0))
                return Failure("Returned view identity, camera, history, matrix or presentation ownership disagrees with its actual submission.");
            evidence.frames[package][view] = *found;
            if (package)
            {
                const auto &old = evidence.frames[package - 1][view];
                const auto increment = package == 1 && view == 2 ? 1u : 0u;
                if (found->submission_id != old.submission_id + views.size() || found->rendered_frame != old.rendered_frame + 1u ||
                    found->history_generation != old.history_generation + increment)
                    return Failure("Only the cut Capture view may advance history generation; each accepted package must render every view once.");
                const bool same_matrix = found->unjittered_view_projection == old.unjittered_view_projection;
                if (same_matrix == (increment != 0))
                    return Failure("Capture camera/origin discontinuity or unaffected Main/Preview matrix reuse was not isolated by ViewId.");
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
            const bool duplicate_preserved = SameResultEvidence(result, accepted) &&
                renderer.GetDiagnostics().total_rendered_frames == before;
            auto nonfinite = views; nonfinite[2].view_origin.x = std::numeric_limits<double>::quiet_NaN();
            const RenderFrameInput nonfinite_input{.views = nonfinite, .snapshots = snapshots};
            const auto rejected_nonfinite = renderer.Render(nonfinite_input, result);
            evidence.nonfinite_origin_rejected = !rejected_nonfinite && rejected_nonfinite.Code() == ErrorCode::InvalidArgument;
            evidence.submissions_after_bad_preflight = renderer.GetDiagnostics().total_rendered_frames;
            evidence.rejected_outputs_unchanged = duplicate_preserved && SameResultEvidence(result, accepted) &&
                evidence.submissions_after_bad_preflight == before;
            if (!evidence.duplicate_rejected || !evidence.nonfinite_origin_rejected || !evidence.rejected_outputs_unchanged)
                return Failure("Duplicate/nonfinite preflight was accepted or mutated submission/result evidence.");
        }
    }
    evidence.final_frame = std::move(result);
    evidence.submissions_after = renderer.GetDiagnostics().total_rendered_frames;
    if (evidence.submissions_after != evidence.submissions_before + 9u)
        return Failure("Owned-view probe did not produce exactly nine actual view submissions in three accepted packages.");
    const auto stale_capture = renderer.Capture({.path = artifacts / "stale-main-geometry.json",
        .kind = CaptureKind::GeometryDiagnostics});
    evidence.stale_diagnostics_rejected = !stale_capture && stale_capture.Code() == ErrorCode::InvalidState;
    if (!evidence.stale_diagnostics_rejected)
        return Failure("Main geometry capture must reject shared frame buffers overwritten by an auxiliary view.");
    std::error_code error;
    std::filesystem::create_directories(artifacts, error);
    if (error) return Failure("Cannot create owned-view capture directory: " + error.message());
    evidence.captures = {artifacts / "owned-view-preview-a.png", artifacts / "owned-view-capture-b.png"};
    if (auto captured = renderer.Capture({.view_id = kOwnedViewProbePreview, .path = evidence.captures[0]}); !captured) return captured;
    if (auto captured = renderer.Capture({.view_id = kOwnedViewProbeCapture, .path = evidence.captures[1]}); !captured) return captured;
    evidence.hdr_capture = artifacts / "owned-view-capture-b.hdr";
    if (auto captured = renderer.Capture({.view_id = kOwnedViewProbeCapture,
            .path = evidence.hdr_capture, .kind = CaptureKind::HdrColor}); !captured) return captured;
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
            views.push_back({{"view_id", stamp.identity.view_id.value},
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
    return {{"packages", std::move(packages)}, {"admitted_decals", evidence.admitted_decals},
        {"archer_clip_ids", evidence.archer_clip_ids},
        {"submissions_before", evidence.submissions_before},
        {"submissions_after_bad_preflight", evidence.submissions_after_bad_preflight},
        {"submissions_after", evidence.submissions_after}, {"duplicate_rejected", evidence.duplicate_rejected},
        {"nonfinite_origin_rejected", evidence.nonfinite_origin_rejected},
        {"rejected_outputs_unchanged", evidence.rejected_outputs_unchanged},
        {"stale_diagnostics_rejected", evidence.stale_diagnostics_rejected},
        {"captures", {evidence.captures[0].filename().string(), evidence.captures[1].filename().string()}},
        {"hdr_capture", evidence.hdr_capture.filename().string()}};
}
} // namespace hs::render_test
