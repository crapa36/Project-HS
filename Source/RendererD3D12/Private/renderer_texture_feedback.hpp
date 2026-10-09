#pragma once

#include "renderer_texture_streaming.hpp"
#include <span>

namespace hs
{
inline constexpr std::uint32_t kTextureFeedbackBindingCount = 90;
inline constexpr std::uint32_t kTextureFeedbackDescriptorCount = 1 + kTextureFeedbackBindingCount;
struct TextureFeedbackGraphBinding
{
    TextureStreamId texture;
    ResourceVersion feedback;
};
class RendererTextureFeedback
{
  public:
    RendererTextureFeedback();
    ~RendererTextureFeedback();
    RendererTextureFeedback(const RendererTextureFeedback &) = delete;
    RendererTextureFeedback &operator=(const RendererTextureFeedback &) = delete;
    [[nodiscard]] Result Initialize(ID3D12Device *, D3D12MA::Allocator *, bool hardware,
                                    std::uint32_t frame_count);
    // The caller has waited this complete frame bank. Metadata and physical
    // feedback pairs cannot change while any queue still reads that bank.
    [[nodiscard]] Result PrepareFrame(std::uint32_t, const CompletionSet &completed,
        std::span<const TextureStreamId> material_slots, const RendererTextureStreaming &,
        ResidencyManager &);
    [[nodiscard]] Result WriteDescriptors(std::uint32_t, ID3D12DescriptorHeap *,
        D3D12_CPU_DESCRIPTOR_HANDLE first, D3D12_GPU_DESCRIPTOR_HANDLE gpu_first, UINT stride);
    [[nodiscard]] ResourceVersion ImportMetadata(RenderGraphBuilder &, std::uint32_t frame) const;
    [[nodiscard]] Result AddClearPass(RenderGraphBuilder &, std::uint32_t frame,
        std::uint32_t pass_id, std::vector<TextureFeedbackGraphBinding> &);
    static void DeclareSampling(PassBuilder &, std::vector<TextureFeedbackGraphBinding> &);
    [[nodiscard]] Result AddDecodePass(RenderGraphBuilder &, std::uint32_t frame,
        std::uint32_t pass_id, std::span<const TextureFeedbackGraphBinding>);
    void MarkSubmitted(std::uint32_t frame, const CompletionSet &) noexcept;
    [[nodiscard]] Result Consume(std::uint32_t frame, const CompletionSet &completed,
        std::uint64_t demand_frame, RendererTextureStreaming &);
    void SwapState(RendererTextureFeedback &) noexcept;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
