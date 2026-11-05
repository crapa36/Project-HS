#pragma once

#include "renderer_transparency.hpp"
#include "renderer_translucency_diagnostics_math.hpp"
#include <memory>

namespace hs
{
// Bounded, capture-only native storage. Each phase owns its own graph readback
// destination, so instrumentation does not serialize otherwise independent phases.
class TranslucencyDiagnosticsCapture
{
  public:
    TranslucencyDiagnosticsCapture();
    ~TranslucencyDiagnosticsCapture();
    [[nodiscard]] Result Initialize(ID3D12Device *, D3D12MA::Allocator *, std::uint32_t slots, ViewExtent capacity, TranslucencyDiagnosticsMode);
    void Pipeline(std::uint32_t, const D3D12_GRAPHICS_PIPELINE_STATE_DESC &,
        std::span<const std::byte> vertex, std::span<const std::byte> pixel, std::string_view pixel_product);
    [[nodiscard]] Result Begin(RenderGraphBuilder &, const LayeredTranslucencyInputs &,
        TranslucencyDiagnosticsRetainedLayout *layout = nullptr);
    void DeclarePhase(RenderGraphBuilder &, PassBuilder &, std::uint32_t slot,
        std::uint32_t phase, TranslucencyDiagnosticsRetainedLayout *layout = nullptr);
    [[nodiscard]] Result BindRetainedBegin(RenderGraphBuilder &, const LayeredTranslucencyInputs &,
        const TranslucencyDiagnosticsRetainedLayout &);
    [[nodiscard]] Result BindRetainedPhase(RenderGraphBuilder &, std::uint32_t slot,
        std::uint32_t phase, const TranslucencyDiagnosticsRetainedLayout &);
    void Record(ID3D12GraphicsCommandList *, ID3D12PipelineState *, const LayeredTranslucencyInputs &,
        LayeredTranslucencyPhase, std::uint32_t layer, std::uint32_t phase, D3D12_GPU_DESCRIPTOR_HANDLE);
    void Submitted(const RenderGraphBuilder &, std::uint32_t slot, const CompletionSet &, bool success);
    [[nodiscard]] Result Collect(const CompletionSet &, std::vector<RendererTranslucencyDiagnosticsSample> &);
    [[nodiscard]] RendererTranslucencyDiagnosticsStatus Status() const;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
