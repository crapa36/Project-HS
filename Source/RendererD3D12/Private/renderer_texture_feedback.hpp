#pragma once

#include "renderer_texture_streaming.hpp"
#include "../../../Content/Shaders/terrain_page_abi.hlsli"
#include <hs/render_data/material_schema.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace hs
{
inline constexpr std::uint32_t kTextureFeedbackBindingCount = static_cast<std::uint32_t>(render_data::kAuthoredMaterialNames.size() * render_data::kMaterialTextureSlots);
inline constexpr std::uint32_t kTextureFeedbackDescriptorCount = 1 + kTextureFeedbackBindingCount;
namespace texture_feedback_detail
{
inline constexpr std::uint32_t kNoMipDemand = ~std::uint32_t{};
[[nodiscard]] std::array<std::uint32_t, 2> DecodeTerrainMipRequests(std::uint32_t word) noexcept;
// Scans one logical row only; row-pitch padding is excluded by the caller.
[[nodiscard]] std::uint32_t MinimumRequestedMip(std::span<const std::byte> row,
                                                std::uint32_t mip_count) noexcept;
}
struct TextureFeedbackGraphBinding
{
    ResourceVersion feedback;
    bool opacity_sampling{true};
};
struct TextureFeedbackLayout
{
    struct Owner
    {
        TextureStreamId texture;
        TextureHandle imported{};
        std::uint32_t width{}, height{}, slices{}, mip_count{};
        bool opacity_sampling{};
    };
    BufferHandle terrain_requests{};
    ResourceVersion metadata_version{};
    std::vector<Owner> owners;
    std::uint32_t clear_pass_index{}, decode_pass_index{};
};
class RendererTextureFeedback
{
  public:
    RendererTextureFeedback() = default;
    RendererTextureFeedback(RendererTextureFeedback &&) noexcept = default;
    RendererTextureFeedback &operator=(RendererTextureFeedback &&) noexcept = default;
    RendererTextureFeedback(const RendererTextureFeedback &) = delete;
    RendererTextureFeedback &operator=(const RendererTextureFeedback &) = delete;
    [[nodiscard]] Result Initialize(ID3D12Device *, D3D12MA::Allocator *, bool hardware,
                                    std::uint32_t frame_count);
    // The caller has waited this complete frame bank. Metadata and physical
    // feedback pairs cannot change while any queue still reads that bank.
    [[nodiscard]] Result PrepareFrame(std::uint32_t, const CompletionSet &completed,
        std::span<const TextureStreamId> material_slots, std::span<const TextureStreamId> terrain_planes, const RendererTextureStreaming &,
        ResidencyManager &);
    [[nodiscard]] Result WriteDescriptors(std::uint32_t, ID3D12DescriptorHeap *, ID3D12DescriptorHeap *,
        D3D12_CPU_DESCRIPTOR_HANDLE first, D3D12_GPU_DESCRIPTOR_HANDLE gpu_first,
        D3D12_CPU_DESCRIPTOR_HANDLE terrain_cpu, D3D12_GPU_DESCRIPTOR_HANDLE terrain_gpu, UINT stride);
    [[nodiscard]] Result ImportMetadata(RenderGraphBuilder &, std::uint32_t frame,
        TextureFeedbackLayout &, bool restore, ResourceVersion &) const;
    [[nodiscard]] Result AddClearPass(RenderGraphBuilder &, std::uint32_t frame,
        TextureFeedbackLayout &, bool restore, std::vector<TextureFeedbackGraphBinding> &);
    static void DeclareSampling(PassBuilder &, std::vector<TextureFeedbackGraphBinding> &,
        bool opacity_only = false);
    [[nodiscard]] Result AddDecodePass(RenderGraphBuilder &, std::uint32_t frame,
        TextureFeedbackLayout &, bool restore, std::span<const TextureFeedbackGraphBinding>);
    [[nodiscard]] bool MatchesLayout(std::uint32_t frame, const TextureFeedbackLayout &) const noexcept;
    void MarkSubmitted(std::uint32_t frame, const CompletionSet &) noexcept;
    [[nodiscard]] Result Consume(std::uint32_t frame, const CompletionSet &completed,
        std::uint64_t demand_frame, RendererTextureStreaming &);
#ifdef HS_DEVELOPMENT_TOOLS
    // Caller supplies a completed SubmittedFrame-validated bank; planes are borrowed until the next PrepareFrame.
    [[nodiscard]] Result ReadTerrainRequests(std::uint32_t frame, const CompletionSet &completed,
        std::span<std::uint32_t, HS_TERRAIN_PAGE_TILE_CAPACITY> words,
        std::span<const TextureStreamId> &planes) const;
#endif
  private:
    struct FeedbackOwner;
    struct FeedbackFrame;
    void SampleAllocatorStats(ResidencyManager &);
    ResidencyTicket RegisterNativeAllocation(ResidencyManager &, ResidencyCategory, std::uint64_t,
                                            bool pinned, D3D12_HEAP_TYPE);
    Result Allocate(AllocationResource &, const D3D12_RESOURCE_DESC &, D3D12_HEAP_TYPE, D3D12_RESOURCE_STATES);
    Result MakeOwner(FeedbackOwner &, const TextureStreamView &, ResidencyManager &);
    static void ReleaseAccounting(const FeedbackFrame &, ResidencyManager &) noexcept;

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12Device8> device8_;
    D3D12MA::Allocator *allocator_{};
    bool hardware_{}, unified_memory_{};
    std::vector<std::shared_ptr<FeedbackFrame>> frames_;
};
} // namespace hs
