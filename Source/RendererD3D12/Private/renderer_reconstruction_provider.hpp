#pragma once

#include "renderer_fsr.hpp"
#include "renderer_streamline.hpp"
#include "renderer_xess.hpp"
#include "renderer_directsr.hpp"

#include <wrl/client.h>
#include <memory>
#include <string>

namespace hs
{
// One selected reconstruction path. XeSS-SR is private to this owner; DLSS and
// FSR borrow the renderer's single sessions also used by presentation. Preparing
// a candidate never replaces either shared session's live reconstruction state.
class ReconstructionProvider
{
  public:
    [[nodiscard]] Result Prepare(std::string_view provider, ID3D12Device *device,
        StreamlineSession *streamline, FsrSession *fsr, const GraphicsModeId &quality,
        ViewExtent output);
    // Shared SDK configuration changes happen only after the renderer drains
    // graph work. The old provider can be configured again to restore its state.
    void SetViewportId(std::uint32_t viewport) noexcept { viewport_id_ = viewport; }
    [[nodiscard]] Result ConfigureAfterCompletion(ViewExtent render_capacity);
    [[nodiscard]] Result ResetAfterCompletion();
    [[nodiscard]] Result Execute(ID3D12GraphicsCommandList *commands, const SdkReconstructionFrame &frame);
    [[nodiscard]] bool UsesExternalQueue() const noexcept { return kind_ == Kind::DirectSr; }
    [[nodiscard]] Result RegisterExternal(PassBuilder &, const SdkReconstructionFrame &, std::uint32_t frame_slot,
        std::span<const ExternalResourceAccess>, std::vector<std::shared_ptr<const void>> owners);
    [[nodiscard]] Result NativeJitter(ViewExtent active, std::uint64_t successful_frame,
                                     std::optional<DirectX::XMFLOAT2> &);
    [[nodiscard]] bool Initialized() const noexcept { return configured_; }
    [[nodiscard]] bool NeedsCompositionGuide() const noexcept { return composition_guide_; }
    [[nodiscard]] ReconstructionAvailability Availability() const noexcept;
    [[nodiscard]] std::string_view FailureReason() const noexcept;
    [[nodiscard]] const StreamlineInputExtent &InputRange() const noexcept { return input_; }
    [[nodiscard]] std::string_view ProviderId() const noexcept;
    [[nodiscard]] ViewExtent RenderCapacity() const noexcept { return capacity_; }
  private:
    enum class Kind { None, Xess, Dlss, Fsr, DirectSr };
    Kind kind_{Kind::None};
    std::uint32_t viewport_id_{};
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    StreamlineSession *streamline_{};
    FsrSession *fsr_{};
    std::unique_ptr<XessReconstruction> xess_;
    std::unique_ptr<DirectSrReconstruction> directsr_;
    GraphicsModeId quality_;
    ViewExtent output_, capacity_;
    StreamlineInputExtent input_;
    ReconstructionAvailability availability_{ReconstructionAvailability::Unavailable};
    std::string failure_;
    bool configured_{}, configuration_attempted_{}, composition_guide_{};
};
} // namespace hs
