#pragma once

#include <hs/renderer/renderer.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace hs::render_test
{
inline std::vector<RenderViewSample> ViewSamples(std::span<const RenderViewInput> views)
{
    std::vector<RenderViewSample> samples;
    samples.reserve(views.size());
    for (const auto &view : views) samples.push_back(view.Sample());
    return samples;
}

struct OwnedViewProbeEvidence
{
    // Package order; each row is Main, Preview A, Capture B. These are the
    // actual public submission stamps, including opaque provider provenance.
    std::array<std::array<RenderViewFrameResult, 3>, 3> frames;
    std::array<std::uint32_t, 3> admitted_decals{};
    std::array<std::uint32_t, 3> admitted_distortions{};
    std::array<std::uint32_t, 3> admitted_ribbons{};
    std::array<std::uint32_t, 3> admitted_ground{};
    std::array<std::uint32_t, 3> admitted_volumes{};
    std::array<std::uint64_t, 2> archer_clip_ids{};
    std::uint64_t motion_epoch_target_id{}, motion_epoch_before{}, motion_epoch_after{};
    RendererToolFrameResult final_frame;
    std::uint64_t submissions_before{}, submissions_after_bad_preflight{}, submissions_after{};
    std::uint64_t overwrite_submission_id{};
    bool duplicate_rejected{}, rejected_outputs_unchanged{}, stale_diagnostics_rejected{};
    bool ui_cache_pixels_valid{};
    std::array<std::filesystem::path, 2> captures;
    std::filesystem::path hdr_capture;
    nlohmann::ordered_json motion_readbacks{nlohmann::ordered_json::object()};
};

Result RunOwnedViewProbe(D3D12Renderer &renderer,
    const RenderSnapshotPair &snapshots, const std::filesystem::path &artifacts,
    AssetId archer_asset_id, OwnedViewProbeEvidence &evidence);

nlohmann::ordered_json OwnedViewProbeReceipt(const OwnedViewProbeEvidence &evidence);
} // namespace hs::render_test
