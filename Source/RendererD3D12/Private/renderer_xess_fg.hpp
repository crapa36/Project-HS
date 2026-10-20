#pragma once

#include "renderer_sdk_frame.hpp"

#include <memory>
#include <string_view>

namespace hs
{
class XellLatency;

struct XessFrameGenerationProperties
{
    std::uint32_t maximum_generated_frames{}, initialized_maximum_generated_frames{};
    std::uint32_t descriptor_count{};
    std::uint64_t temporary_buffer_bytes{}, temporary_texture_bytes{}, constant_buffer_bytes{};
    bool intel_device{};
};

struct XessFrameGenerationPresentStatus
{
    // Actual SDK results, including warnings; frames_queued is not GPU completion.
    std::uint32_t frames_queued{};
    std::int32_t native_query_result{}, native_generation_result{};
    bool frame_generation_enabled{}, available{};
};

// One concrete XeSS-FG proxy swap chain, exclusively selected by the coordinator.
// The adapter owns the proxy. SwapChain() returns a borrowed pointer used for ALL
// DXGI interactions; never wrap another provider's proxy or keep a native bypass.
class XessFrameGeneration
{
  public:
    XessFrameGeneration();
    ~XessFrameGeneration();
    XessFrameGeneration(const XessFrameGeneration &) = delete;
    XessFrameGeneration &operator=(const XessFrameGeneration &) = delete;

    // Nonmutating selection preflight: creates/queries/destroys only an SDK
    // context. No proxy, SDK pipelines or GPU pools. Generic maximum count and
    // actual adapter/SM support are returned; extent-dependent heap properties
    // are queried by InitializeFromDescription. Availability/FailureReason hold
    // the native outcome. A failed Destroy keeps ownership and blocks retry.
    [[nodiscard]] Result Probe(ID3D12Device *device, std::uint32_t generated_frames,
                                XessFrameGenerationProperties &properties);

    // The window must have NO existing swap chain. Stop/drain the old provider,
    // release its back buffers and sole native chain, then call this method.
    // Intel's InitFromSwapChain is deliberately not exposed: it consumes the old
    // chain (refcount must be 1), and offers no documented rollback on failure.
    // Initialization preflights native context/properties/pipelines before native
    // creation. Failure requires explicit ResetAfterCompletion before retrying.
    [[nodiscard]] Result InitializeFromDescription(ID3D12Device *device, ID3D12CommandQueue *queue,
        IDXGIFactory2 *factory, HWND window, const DXGI_SWAP_CHAIN_DESC1 &description,
        DXGI_COLOR_SPACE_TYPE color_space, XellLatency &latency, std::uint32_t generated_frames,
        bool gpu_idle_acknowledged);
    [[nodiscard]] Result SetEnabled(bool enabled);
    [[nodiscard]] Result SetGeneratedFrames(std::uint32_t count);
    // All guides enter COPY_SOURCE and remain in that state. SDK RV_ONLY_NOW
    // records private copies into this Direct list. view/projection are row-major
    // and unjittered; UV Previous-Current motion is scaled to output pixels.
    // HUD-less and premultiplied UI use the exact swap-chain format/color space.
    // Required owners include graph/heap leases; copied source COM refs are also
    // retained. At most BufferCount frames may await copy completion/presentation.
    [[nodiscard]] Result TagFrame(ID3D12GraphicsCommandList *commands, const SdkPresentationFrame &frame);
    // Call after submitting the tagging list on the exact initialization queue and
    // signalling this fence AFTER that submission. It certifies SDK source copies,
    // never the SDK's internally queued presentation work. No CPU wait is added.
    [[nodiscard]] Result NotifyTagSubmission(const SdkFrameIdentity &identity,
        ID3D12CommandQueue *queue, ID3D12Fence *copy_fence, std::uint64_t copy_fence_value);
    [[nodiscard]] Result SetPresentId(const SdkFrameIdentity &identity);
    // Call after the matching real Present/Present1, including unsuccessful DXGI
    // results. Present status describes frames queued, not frames displayed.
    [[nodiscard]] Result CompletePresent(const SdkFrameIdentity &identity,
                                         XessFrameGenerationPresentStatus &status);
    // Stop all SDK calls, complete submitted application/tagging work, reset any
    // unsubmitted tagging lists, and release external proxy/back-buffer refs.
    // Intel exposes no internal present-queue fence/drain API. Successful native
    // Destroy is the SDK-owned internal presentation teardown boundary. This
    // method keeps context/DLL/XeLL owners if native destruction fails.
    [[nodiscard]] Result ResetAfterCompletion(bool gpu_idle_acknowledged);
    [[nodiscard]] IDXGISwapChain4 *SwapChain() const noexcept;
    [[nodiscard]] const XessFrameGenerationProperties &Properties() const noexcept;
    [[nodiscard]] ReconstructionAvailability Availability() const noexcept;
    [[nodiscard]] std::string_view FailureReason() const noexcept;
    [[nodiscard]] bool Initialized() const noexcept;
    [[nodiscard]] bool Enabled() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
