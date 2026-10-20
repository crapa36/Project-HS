#pragma once

#include "renderer_asset_io.hpp"
#include "d3d12_resources.hpp"
#include <hs/render_data/texture_package.hpp>

#include <filesystem>
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
struct TextureStreamingCapabilities
{
    bool tiled_resources{}, sampler_feedback{}, packed_mip_arrays{};
};
struct TextureStreamRegistration
{
    std::string generation, logical_id;
    std::filesystem::path path;
    std::shared_ptr<const void> generation_lease;
    render_data::TextureDescription description;
    Microsoft::WRL::ComPtr<ID3D12Resource> fallback;
    std::uint32_t fallback_first_mip{};
    std::uint64_t fallback_allocation_bytes{};
    ResidencyTicket fallback_residency;
    Access fallback_graph_access{Access::Undefined};
    bool pinned{};
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
        std::uint32_t frame_count);
    [[nodiscard]] Result Register(TextureStreamRegistration, ResidencyManager &, TextureStreamId &);
    [[nodiscard]] Result ValidateConfiguration(GraphicsQuality, std::uint32_t budget_mib) const;
    [[nodiscard]] Result Configure(GraphicsQuality, std::uint32_t finest_mip, std::uint32_t budget_mib);
    void Demand(TextureStreamId, std::uint32_t logical_first_mip, std::uint32_t priority,
                std::uint64_t frame_number) noexcept;
    [[nodiscard]] Result Pump(std::uint64_t frame_number, const CompletionSet &completed,
        RendererAssetIo &, StreamingManager &, ResidencyManager &);
    // Owns map/unmap, copy commands, and their retained backing. The graph
    // bridges completion onto its existing owned Copy queue timeline.
    [[nodiscard]] Result AddPendingPasses(RenderGraphBuilder &, ExternalQueueBinding,
        std::uint32_t first_pass_id, std::vector<TextureStreamingGraphBinding> &);
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
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
