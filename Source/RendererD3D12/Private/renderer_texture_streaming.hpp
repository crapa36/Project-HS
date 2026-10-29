#pragma once

#include "renderer_asset_io.hpp"
#include "d3d12_resources.hpp"
#include <hs/render_data/texture_package.hpp>
#include <hs/render_data/texture_descriptor.hpp>

#include <filesystem>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hs
{
[[nodiscard]] constexpr bool TextureHostFootprintFitsBudget(std::uint64_t metadata_bytes,
    std::uint64_t payload_bytes, std::uint64_t budget_bytes) noexcept
{
    return metadata_bytes <= budget_bytes && payload_bytes <= budget_bytes - metadata_bytes;
}
struct TextureStreamId
{
    std::uint32_t value{};
    explicit operator bool() const noexcept { return value != 0; }
    friend bool operator==(TextureStreamId, TextureStreamId) = default;
};
[[nodiscard]] constexpr std::uint32_t NextTextureTransactionTarget(std::uint32_t actual_mip,
    std::uint32_t desired_mip, bool reserved, bool reserved_published, std::uint32_t tail_mip) noexcept
{
    if (reserved && !reserved_published) return tail_mip;
    return reserved && desired_mip < actual_mip
        ? std::max(desired_mip, actual_mip - 1u) : desired_mip;
}
[[nodiscard]] constexpr bool TextureSoftwareTransactionSuperseded(
    std::uint32_t transaction_target_mip, std::uint32_t desired_mip, bool budget_constrained) noexcept
{ return budget_constrained && desired_mip > transaction_target_mip; }
struct TextureStreamingCapabilities
{
    bool tiled_resources{}, sampler_feedback{}, packed_mip_arrays{}, unified_memory{};
};
// Select whole authenticated subresources under a host-payload limit. A single
// subresource is never split because its digest authenticates the full range.
[[nodiscard]] inline std::size_t TextureReadChunkEnd(
    std::span<const std::uint32_t> subresources,
    std::span<const render_data::TextureSubresourceLayout> layouts,
    std::size_t first, std::uint64_t byte_budget) noexcept
{
    if (first > subresources.size()) return first;
    std::uint64_t bytes{};
    auto end = first;
    while (end < subresources.size())
    {
        const auto index = subresources[end];
        if (index >= layouts.size() || !layouts[index].has_digest || !layouts[index].byte_count ||
            layouts[index].byte_count > byte_budget - bytes) break;
        bytes += layouts[index].byte_count;
        ++end;
    }
    return end;
}
[[nodiscard]] inline bool TextureUploadPackAppend(std::uint64_t packed_bytes,
    std::uint64_t footprint_bytes, std::uint64_t placement_alignment,
    std::uint64_t window_width, std::uint64_t &next_packed_bytes) noexcept
{
    if (!footprint_bytes || !placement_alignment) return false;
    const auto remainder = packed_bytes % placement_alignment;
    const auto padding = remainder ? placement_alignment - remainder : 0u;
    if (packed_bytes > UINT64_MAX - padding) return false;
    const auto offset = packed_bytes + padding;
    if (footprint_bytes > UINT64_MAX - offset) return false;
    next_packed_bytes = offset + footprint_bytes;
    return next_packed_bytes <= window_width;
}
[[nodiscard]] inline std::size_t TextureReadChunkEndWithinUploadWindow(
    std::span<const std::uint32_t> subresources,
    std::span<const render_data::TextureSubresourceLayout> layouts,
    std::span<const std::uint64_t> footprint_bytes,
    std::size_t first, std::uint64_t payload_budget,
    std::uint64_t placement_alignment, std::uint64_t window_width) noexcept
{
    if (first > subresources.size() || footprint_bytes.size() != subresources.size()) return first;
    std::uint64_t payload{}, packed{};
    auto end = first;
    while (end < subresources.size())
    {
        const auto index = subresources[end];
        if (index >= layouts.size() || !layouts[index].has_digest || !layouts[index].byte_count ||
            layouts[index].byte_count > payload_budget - payload) break;
        std::uint64_t next{};
        if (!TextureUploadPackAppend(packed, footprint_bytes[end], placement_alignment, window_width, next)) break;
        payload += layouts[index].byte_count;
        packed = next;
        ++end;
    }
    return end;
}
// Find the first mip whose complete remaining authenticated payload fits beside
// prepared metadata. Registration validates the immutable descriptor; keep the
// checks here so malformed ranges cannot be mistaken for optional quality.
[[nodiscard]] inline bool TextureReadMinimumHostMip(
    const render_data::TextureDescription &description,
    std::span<const render_data::TextureSubresourceLayout> layouts,
    std::uint64_t metadata_bytes, std::uint64_t host_budget,
    std::uint32_t &minimum_mip) noexcept
{
    const auto subresource_count = static_cast<std::uint64_t>(description.array_size) * description.mip_count;
    if (!description.array_size || !description.mip_count || subresource_count != layouts.size()) return false;
    const bool metadata_fits = metadata_bytes < host_budget;
    const auto payload_budget = metadata_fits ? host_budget - metadata_bytes : 0u;
    minimum_mip = 0;
    for (std::uint32_t slice = 0; slice < description.array_size; ++slice)
        for (std::uint32_t mip = 0; mip < description.mip_count; ++mip)
        {
            const auto &layout = layouts[static_cast<std::size_t>(slice) * description.mip_count + mip];
            if (!layout.has_digest || !layout.byte_count) return false;
            if (!metadata_fits || layout.byte_count > payload_budget)
                minimum_mip = std::max(minimum_mip, mip + 1u);
        }
    return true;
}
// Clamp requested software detail using the intrinsic host ceiling. Other
// owners' temporary holds are accounted separately when admitting each chunk.
[[nodiscard]] inline std::uint32_t TextureReadTargetMip(
    const render_data::TextureDescription &description,
    std::span<const render_data::TextureSubresourceLayout> layouts,
    std::uint32_t requested, std::uint32_t tail, std::uint64_t metadata_bytes,
    std::uint64_t host_budget, std::uint32_t fallback) noexcept
{
    std::uint32_t minimum_mip{};
    if (requested > tail || !TextureReadMinimumHostMip(description, layouts,
            metadata_bytes, host_budget, minimum_mip))
        return fallback;
    const auto target = std::max(requested, minimum_mip);
    return target <= tail ? target : fallback;
}
[[nodiscard]] constexpr std::uint32_t TextureEffectiveHostTargetMip(
    std::uint32_t requested, std::uint32_t tail, std::uint32_t minimum_host_mip,
    std::uint32_t fallback, std::uint32_t resident_mip, bool has_published_backing) noexcept
{
    if (minimum_host_mip > tail) return fallback;
    const auto target = std::max(requested, minimum_host_mip);
    // A host ceiling limits transfers. It does not justify evicting detail
    // already published at a finer mip than the cap permits us to reload.
    return has_published_backing && minimum_host_mip > requested
        ? std::min(target, resident_mip) : target;
}
[[nodiscard]] inline bool TextureBuildBudgetVictimOrder(
    std::span<const std::uint32_t> priorities,
    std::span<const std::uint32_t> desired_mips,
    std::span<const std::uint32_t> tail_mips,
    std::vector<std::uint32_t> &indices)
{
    if (priorities.size() != desired_mips.size() || priorities.size() != tail_mips.size()) return false;
    indices.clear();
    for (std::uint32_t index = 0; index < priorities.size(); ++index)
        if (desired_mips[index] < tail_mips[index]) indices.push_back(index);
    std::ranges::sort(indices, [&](std::uint32_t left, std::uint32_t right) {
        return priorities[left] != priorities[right] ? priorities[left] < priorities[right] : left < right;
    });
    return true;
}
[[nodiscard]] inline bool TextureCoarsenOneMip(
    std::uint32_t &mip, std::uint32_t tail_mip, std::span<const std::uint64_t> mip_costs,
    std::uint64_t &planned_bytes) noexcept
{
    if (mip >= tail_mip || mip_costs.size() <= tail_mip || mip_costs[mip] < mip_costs[mip + 1u]) return false;
    const auto released = mip_costs[mip] - mip_costs[mip + 1u];
    if (planned_bytes < released) return false;
    planned_bytes -= released;
    ++mip;
    return true;
}
[[nodiscard]] inline bool TextureAccumulateBudgetBytes(std::uint64_t &total,
                                                       std::uint64_t bytes) noexcept
{
    if (total > UINT64_MAX - bytes) return false;
    total += bytes;
    return true;
}
struct TextureStreamRegistration
{
    std::string generation, logical_id;
    RendererAssetSource source;
    std::shared_ptr<const void> generation_lease;
    render_data::TextureDescription description;
    render_data::TextureHandle compiled_texture;
    Microsoft::WRL::ComPtr<ID3D12Resource> fallback;
    std::uint32_t fallback_first_mip{};
    std::uint64_t fallback_allocation_bytes{};
    ResidencyTicket fallback_residency;
    Access fallback_graph_access{Access::Undefined};
    bool pinned{};
    NativeResourceContract fallback_contract{};
};
struct TextureStreamView
{
    ID3D12Resource *resource{};
    render_data::TextureDescription logical_description;
    std::uint32_t first_mip{}, resident_min_mip{};
    std::uint64_t revision{};
    CompletionSet completion;
    Access access{Access::Undefined};
    bool reserved{};
};
struct TextureStreamingGraphBinding
{
    TextureStreamId texture;
    ResourceVersion sampled;
    std::string_view logical_id;
    std::uint32_t resident_mip{}, required_mip{}, effective_desired_mip{};
    bool ready{};
};
struct TextureStreamingLayout
{
    struct Entry
    {
        TextureHandle sampled{}, candidate{};
        ResourceVersion sampled_version{};
        render_data::TextureDescription description{};
        std::uint32_t first_mip{}, resident_min_mip{}, target_mip{};
        std::uint32_t map_count{}, unmap_count{}, pass_index{}, pass_id{};
        std::vector<std::uint32_t> mapped_mips, unmapped_mips;
        Access access{Access::Undefined};
        bool pending{}, separate_target{}, reserved{}, copy_upload{}, software_copy{};
    };
    std::vector<Entry> entries;
    std::uint32_t first_pass_id{};
    QueueHint queue{QueueHint::Copy};
};
// uint4 GPU ABI, indexed by the actual material texture binding slot.
struct TextureMipMetadata
{
    std::uint32_t first_mip{}, resident_min_mip{}, mip_count{}, feedback_enabled{};
};
static_assert(sizeof(TextureMipMetadata) == 16);

// These are the actual producer lifecycle counters. Descriptor revision is
// intentionally separate from publication count because fallback coarsening
// can revise a descriptor without publishing a streamed upload.
struct TextureStreamingStatistics
{
    std::uint64_t requests{}, uploads{}, publications{}, reclaims{}, revision{};
    std::uint8_t state{};
};
struct TextureStreamingProductStatus
{
    bool registered{}, pending{}, publication_complete{};
    std::uint64_t requests{}, uploads{}, publications{}, reclaims{}, revision{};
    std::uint32_t actual_resident_mip{}, effective_desired_mip{}, target_mip{};
};
class RendererTextureStreaming
{
  public:
    RendererTextureStreaming();
    ~RendererTextureStreaming();
    RendererTextureStreaming(const RendererTextureStreaming &) = delete;
    RendererTextureStreaming &operator=(const RendererTextureStreaming &) = delete;
    [[nodiscard]] Result Initialize(ID3D12Device *, D3D12MA::Allocator *,
        GraphicsQuality, std::uint32_t finest_mip, std::uint32_t budget_mib, TextureStreamingCapabilities &,
        std::uint32_t frame_count, std::span<const render_data::TextureDescriptor> texture_catalog);
    [[nodiscard]] Result Register(TextureStreamRegistration, ResidencyManager &, TextureStreamId &);
    [[nodiscard]] Result SupplyDescriptor(std::string logical_id, render_data::TextureDescriptorStorage);
    [[nodiscard]] Result ValidateConfiguration(GraphicsQuality, std::uint32_t budget_mib) const;
    [[nodiscard]] Result Configure(GraphicsQuality, std::uint32_t finest_mip, std::uint32_t budget_mib);
    void Demand(TextureStreamId, std::uint32_t logical_first_mip, std::uint32_t priority,
                std::uint64_t frame_number) noexcept;
    [[nodiscard]] Result Pump(std::uint64_t frame_number, const CompletionSet &completed,
        RendererAssetIo &, StreamingManager &, ResidencyManager &);
    // Owns map/unmap, copy commands, and their retained backing. The graph
    // bridges completion onto its existing owned Copy queue timeline.
    [[nodiscard]] Result AddPendingPasses(RenderGraphBuilder &, ExternalQueueBinding,
        std::uint32_t first_pass_id, std::vector<TextureStreamingGraphBinding> &,
        TextureStreamingLayout *layout = nullptr);
    [[nodiscard]] bool MatchesLayout(std::uint32_t first_pass_id,
        const TextureStreamingLayout &) const noexcept;
    [[nodiscard]] Result BindRetained(RenderGraphBuilder &, ExternalQueueBinding,
        std::uint32_t first_pass_id, const TextureStreamingLayout &,
        std::vector<TextureStreamingGraphBinding> &);
    void MarkSubmitted(const CompletionSet &, StreamingManager &) noexcept;
    [[nodiscard]] bool HasStartedWork() const noexcept;
    [[nodiscard]] TextureStreamView View(TextureStreamId) const noexcept;
    [[nodiscard]] bool HardwareBacked(TextureStreamId) const noexcept;
    void MarkUse(TextureStreamId, const CompletionSet &) noexcept;
    void CancelReads(RendererAssetIo &, StreamingManager &) noexcept;
    void SwapState(RendererTextureStreaming &) noexcept;
    [[nodiscard]] std::uint64_t PhysicalBytes() const noexcept;
    [[nodiscard]] std::uint32_t Count() const noexcept;
    [[nodiscard]] TextureStreamingStatistics Statistics(const StreamingManager &,
        const CompletionSet &) const noexcept;
    [[nodiscard]] TextureStreamingProductStatus ProductStatus(std::string_view logical_id,
        const StreamingManager &, const CompletionSet &) const noexcept;
    [[nodiscard]] std::optional<bool> RequiredSourceMipFitsMaximumHostBudget(
        TextureStreamId, std::uint32_t required_mip) const noexcept;
  private:
    [[nodiscard]] Result AddPendingPassesImpl(RenderGraphBuilder &, ExternalQueueBinding,
        std::uint32_t, std::vector<TextureStreamingGraphBinding> &,
        TextureStreamingLayout *, const TextureStreamingLayout *);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
