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
#include <string>
#include <string_view>

namespace hs
{
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
        bool pending{}, separate_target{}, reserved{}, copy_upload{};
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
  private:
    [[nodiscard]] Result AddPendingPassesImpl(RenderGraphBuilder &, ExternalQueueBinding,
        std::uint32_t, std::vector<TextureStreamingGraphBinding> &,
        TextureStreamingLayout *, const TextureStreamingLayout *);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
