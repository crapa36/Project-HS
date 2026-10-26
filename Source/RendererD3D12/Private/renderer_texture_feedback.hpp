#pragma once

#include "renderer_texture_streaming.hpp"
#include <hs/render_data/material_schema.hpp>
#include <span>

namespace hs
{
inline constexpr std::uint32_t kTextureFeedbackBindingCount = static_cast<std::uint32_t>(render_data::kAuthoredMaterialNames.size() * render_data::kMaterialTextureSlots);
inline constexpr std::uint32_t kTextureFeedbackDescriptorCount = 1 + kTextureFeedbackBindingCount;
struct TextureFeedbackGraphBinding
{
    TextureStreamId texture;
    ResourceVersion feedback;
};
struct TextureFeedbackLayout
{
    struct Owner
    {
        TextureStreamId texture;
        TextureHandle imported{};
        ResourceVersion cleared{}, sampled{};
        std::uint32_t width{}, height{}, slices{}, mip_count{};
    };
    BufferHandle metadata{};
    ResourceVersion metadata_version{};
    std::vector<Owner> owners;
    std::uint32_t clear_pass_index{}, clear_pass_id{};
    std::uint32_t decode_pass_index{}, decode_pass_id{};
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
    [[nodiscard]] Result WriteDescriptors(std::uint32_t, ID3D12DescriptorHeap *, ID3D12DescriptorHeap *,
        D3D12_CPU_DESCRIPTOR_HANDLE first, D3D12_GPU_DESCRIPTOR_HANDLE gpu_first, UINT stride);
    [[nodiscard]] ResourceVersion ImportMetadata(RenderGraphBuilder &, std::uint32_t frame) const;
    [[nodiscard]] Result ImportMetadata(RenderGraphBuilder &, std::uint32_t frame,
        ResourceVersion &, TextureFeedbackLayout *) const;
    [[nodiscard]] Result BindRetainedMetadata(RenderGraphBuilder &, std::uint32_t frame,
        const TextureFeedbackLayout &, ResourceVersion &) const;
    [[nodiscard]] Result AddClearPass(RenderGraphBuilder &, std::uint32_t frame,
        std::uint32_t pass_id, std::vector<TextureFeedbackGraphBinding> &,
        TextureFeedbackLayout *layout = nullptr);
    [[nodiscard]] Result BindRetainedClear(RenderGraphBuilder &, std::uint32_t frame,
        std::uint32_t pass_id, const TextureFeedbackLayout &,
        std::vector<TextureFeedbackGraphBinding> &);
    static void DeclareSampling(PassBuilder &, std::vector<TextureFeedbackGraphBinding> &);
    [[nodiscard]] Result AddDecodePass(RenderGraphBuilder &, std::uint32_t frame,
        std::uint32_t pass_id, std::span<const TextureFeedbackGraphBinding>,
        TextureFeedbackLayout *layout = nullptr);
    [[nodiscard]] Result BindRetainedDecode(RenderGraphBuilder &, std::uint32_t frame,
        std::uint32_t pass_id, const TextureFeedbackLayout &,
        std::span<const TextureFeedbackGraphBinding>);
    [[nodiscard]] bool MatchesLayout(std::uint32_t frame, std::uint32_t clear_pass_id,
        std::uint32_t decode_pass_id, const TextureFeedbackLayout &) const noexcept;
    void MarkSubmitted(std::uint32_t frame, const CompletionSet &) noexcept;
    [[nodiscard]] Result Consume(std::uint32_t frame, const CompletionSet &completed,
        std::uint64_t demand_frame, RendererTextureStreaming &);
    void SwapState(RendererTextureFeedback &) noexcept;
  private:
    [[nodiscard]] Result AddClearPassImpl(RenderGraphBuilder &, std::uint32_t,
        std::uint32_t, std::vector<TextureFeedbackGraphBinding> &,
        TextureFeedbackLayout *, const TextureFeedbackLayout *);
    [[nodiscard]] Result AddDecodePassImpl(RenderGraphBuilder &, std::uint32_t,
        std::uint32_t, std::span<const TextureFeedbackGraphBinding>,
        TextureFeedbackLayout *, const TextureFeedbackLayout *);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
