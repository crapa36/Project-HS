#pragma once

#include "renderer_sdk_presentation.hpp"
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace hs
{
struct PresentationGuideResource { ResourceVersion version; ID3D12Resource *resource{}; };
struct PresentationGuidesInputs
{
    // Display-linear Rec.709 after tone map/outline/FXAA, without ANY UI.
    // The UI bitmap is D2D/WIC premultiplied encoded sRGB BGRA8.
    PresentationGuideResource display_linear, encoded_ui, depth, motion;
    PresentationGuideResource transparent_depth, transparent_motion, transparent_classification, transparent_metadata;
    ViewExtent render_active;
    std::uint32_t frame_slot{};
    CompletionSet completed;
    float paper_white_nits{200}, peak_nits{1000};
    SdkCamera camera;
    bool motion_contains_sky{};
    bool transparent_guides_enabled{};
    // Render ImGui onto a separate RGBA16F transparent UI surface on its owner.
    std::function<Result(ID3D12GraphicsCommandList *)> draw_devtools;
};
struct PresentationGuidesOutputs
{
    PresentationGuideResource hudless, ui, composite, depth, motion;
    std::shared_ptr<const void> owner;
};
struct PresentationGuidesTargets;
class PresentationGuidesCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
  private:
    std::shared_ptr<PresentationGuidesTargets> targets_;
    friend class PresentationGuides;
};
// Created only for active FG. Native guide conversion is separate from SR/RR:
// undilated reversed device Z and PreviousUV-CurrentUV are copied exactly at the
// render extent. Motion scaling remains the native adapter's responsibility.
class PresentationGuides
{
  public:
    using ShaderReader = std::function<Result(std::string_view, std::vector<std::byte> &)>;
    PresentationGuides();
    ~PresentationGuides();
    [[nodiscard]] Result Prepare(ID3D12Device *, D3D12MA::Allocator *, ViewExtent render_capacity,
        ViewExtent output, DXGI_FORMAT output_format, DXGI_COLOR_SPACE_TYPE,
        SdkPresentationOwner, std::uint32_t frame_slots, const ShaderReader &, PresentationGuidesCandidate &) const;
    [[nodiscard]] Result Commit(PresentationGuidesCandidate &&, const CompletionSet &completed);
    // The coordinator's BeforeReuse must have run before this call: graph
    // completion alone does not permit reuse of SL/FSR SDK inputs.
    [[nodiscard]] Result Register(RenderGraphBuilder &, const PresentationGuidesInputs &,
        PresentationGuidesOutputs &, std::uint32_t color_pass_id, std::uint32_t guides_pass_id,
        std::uint32_t devtools_pass_id);
    // First copy composite (SL/XeFG) or hudless (FSR) to the native backbuffer.
    // Then register this with that resulting backbuffer version. It declares
    // SDK states and runs native tagging/PrepareV2 once on the Direct owner.
    [[nodiscard]] Result RegisterTagPass(RenderGraphBuilder &, const PresentationGuidesOutputs &,
        PresentationGuideResource back_buffer, SdkPresentationFrame frame, SdkPresentation &,
        std::uint32_t pass_id);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &, const CompletionSet &, bool success);
    // Drain and destroy SDK presentation first, so its retained owners have
    // actually retired, then complete all graph queues before calling reset.
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &completed);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    // Called only after all old submissions have reliably completed.
    void SwapState(PresentationGuides &other) noexcept { impl_.swap(other.impl_); }
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
