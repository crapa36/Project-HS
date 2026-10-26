#pragma once

#include "renderer_sdk_frame.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace hs
{
struct DirectSrInputExtent { ViewExtent optimal, minimum, maximum; };

// Uses the pinned DirectSR Agility factory and its own compute queue. Execute
// submits asynchronous SDK work, so it is always an external graph operation.
class DirectSrReconstruction
{
  public:
    DirectSrReconstruction();
    ~DirectSrReconstruction();
    DirectSrReconstruction(const DirectSrReconstruction &) = delete;
    DirectSrReconstruction &operator=(const DirectSrReconstruction &) = delete;
    [[nodiscard]] Result Initialize(ID3D12Device *, ViewExtent output, const GraphicsModeId &quality);
    [[nodiscard]] Result ConfigureAfterCompletion(ViewExtent render_capacity);
    [[nodiscard]] Result RegisterExternal(PassBuilder &, const SdkReconstructionFrame &,
        std::uint32_t frame_slot, std::span<const ExternalResourceAccess>,
        std::vector<std::shared_ptr<const void>> owners);
    [[nodiscard]] Result BindExternal(PassBuilder &, const SdkReconstructionFrame &,
        std::uint32_t frame_slot, std::span<const ExternalResourceAccess>,
        std::vector<std::shared_ptr<const void>> owners);
    [[nodiscard]] Result Jitter(ViewExtent active, std::uint64_t successful_frame,
                                std::optional<DirectX::XMFLOAT2> &);
    [[nodiscard]] Result ResetAfterCompletion();
    [[nodiscard]] ReconstructionAvailability Availability() const noexcept;
    [[nodiscard]] std::string_view FailureReason() const noexcept;
    [[nodiscard]] const DirectSrInputExtent &InputRange() const noexcept;
    [[nodiscard]] bool Initialized() const noexcept;
  private:
    [[nodiscard]] Result RegisterExternalImpl(PassBuilder &,const SdkReconstructionFrame &,
        std::uint32_t,std::span<const ExternalResourceAccess>,
        std::vector<std::shared_ptr<const void>>,bool retained);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
