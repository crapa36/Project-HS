#pragma once

#include "renderer_sdk_frame.hpp"

#include <memory>
#include <optional>
#include <string_view>

namespace hs
{
enum class StreamlineFeature : std::uint8_t { SuperResolution, RayReconstruction, FrameGeneration, Reflex };
struct StreamlineFeatures
{
    bool super_resolution{}, ray_reconstruction{}, frame_generation{}, reflex{};
};
struct StreamlineInputExtent { ViewExtent optimal, minimum, maximum; };
struct StreamlineFrameGenerationCapabilities
{
    std::uint32_t maximum_multiplier{}, minimum_dimension{}, frames_actually_presented{};
    bool dynamic_supported{}, vsync_supported{};
};
struct StreamlineReflexState
{
    bool low_latency_available{}, latency_report_available{};
    std::uint32_t statistics_window_message{};
};

// One native Streamline 2.14.1 session. The default object allocates and loads
// nothing. All calls run on the renderer/present owner thread. Runtime calls
// InputSleep before input sampling and sends markers with this same render ID.
// Streamline owns the only presentation proxy when it is selected; another
// provider's swapchain/device proxy must never be passed to these methods.
class StreamlineSession
{
  public:
    StreamlineSession();
    ~StreamlineSession();
    StreamlineSession(const StreamlineSession &) = delete;
    StreamlineSession &operator=(const StreamlineSession &) = delete;

    // Call before creating any DXGI/device objects. DLLs and plugins are loaded
    // only from the executable's absolute "streamline" directory, with OTA off.
    [[nodiscard]] Result InitializeBeforeDevice(StreamlineFeatures features);
    [[nodiscard]] Result BindDevice(ID3D12Device *native_device);
    // Upgrade immediately after creation. Use the device proxy only for
    // CreateCommandQueue, the factory proxy for CreateSwapChain*, and the
    // swapchain proxy for all hooks listed by sl_hooks.h, including Present,
    // GetBuffer, GetCurrentBackBufferIndex, ResizeBuffers and fullscreen state.
    [[nodiscard]] Result UpgradeDevice(void **device);
    // A failed optional device/queue proxy bootstrap rejects FG only. SR/RR and
    // native/other-provider presentation can still use the native device/queue.
    void ReportPresentationBootstrapFailure(std::string_view reason);
    [[nodiscard]] Result UpgradeFactory(void **factory);
    // A swapchain created by the upgraded factory already is a proxy. This
    // method also accepts that proxy, without wrapping it a second time.
    [[nodiscard]] Result UpgradeSwapChain(void **swapchain);
    // Returns an AddRef'd native interface for graph/third-party SDK use. The
    // caller owns that reference; native queues must be used for graph work.
    [[nodiscard]] Result GetNativeInterface(void *proxy, void **native);
    [[nodiscard]] Result AttachPresentation(IDXGISwapChain4 *proxy, ID3D12CommandQueue *presenting_queue);

    [[nodiscard]] Result QueryInputExtent(const GraphicsModeId &quality, ViewExtent output,
                                         bool ray_reconstruction, StreamlineInputExtent &range);
    // Drain graph evaluation uses and call ReleaseReconstructionAfterCompletion
    // before changing this configuration. RR replaces SR and covered denoisers.
    [[nodiscard]] Result ConfigureReconstruction(const GraphicsModeId &quality, ViewExtent output,
                                                bool ray_reconstruction, std::uint32_t viewport_id = 0);
    // Inputs enter NON_PIXEL_SHADER_RESOURCE, output UNORDERED_ACCESS. Graph
    // completion retains their owners. Subsequent passes rebind all CL state.
    [[nodiscard]] Result ExecuteSuperResolution(ID3D12GraphicsCommandList *commands,
                                               const SdkReconstructionFrame &frame);
    // Explicit RR allocation records native initialization on the supplied
    // Direct command list. The caller must submit and drain that list before
    // calling CompleteRayReconstructionAllocationAfterCompletion; this method
    // never submits a queue. created is true only when a new native allocation
    // was entered (matching owned viewports reuse the existing allocation).
    [[nodiscard]] Result AllocateRayReconstructionResources(ID3D12GraphicsCommandList *commands,
                                                            const SdkReconstructionFrame &metadata,
                                                            bool &created);
    // Clears the frame-0 allocation tags after the caller has drained the
    // command list. It does not submit or wait for GPU work.
    [[nodiscard]] Result CompleteRayReconstructionAllocationAfterCompletion();
    [[nodiscard]] Result ExecuteRayReconstruction(ID3D12GraphicsCommandList *commands,
                                                 const SdkRayReconstructionFrame &frame);
    [[nodiscard]] Result SelectReconstructionViewAfterCompletion(std::uint32_t viewport_id);
    [[nodiscard]] Result ReleaseReconstructionAfterCompletion();
    // Native planned resident memory for the currently configured RR viewport.
    [[nodiscard]] Result QueryRayReconstructionMemory(std::uint64_t &bytes);

    [[nodiscard]] Result BeginFrame(SdkFrameIdentity identity);
    [[nodiscard]] std::optional<std::uint64_t> NativeFrameToken(SdkFrameIdentity identity) const noexcept;
    [[nodiscard]] Result InputSleep(SdkFrameIdentity identity);
    [[nodiscard]] Result Marker(SdkFrameIdentity identity, SdkLatencyMarker marker);
    [[nodiscard]] Result ConfigureReflex(bool enabled, bool boost = false, std::uint32_t frame_limit_us = 0);
    [[nodiscard]] Result QueryReflexState(StreamlineReflexState &state);
    [[nodiscard]] Result QueryFrameGenerationCapabilities(StreamlineFrameGenerationCapabilities &capabilities);
    // Mode changes require DrainPresentation plus actual swapchain destruction
    // and recreation. Configure only while detached, then AttachPresentation.
    // A nonzero dynamic_resolution_optimal enables native dynamic resolution
    // with that fixed processing size; this differs from Dynamic MFG mode.
    [[nodiscard]] Result ConfigureFrameGeneration(GraphicsFrameGenerationMode mode,
                                                 std::uint32_t multiplier, float target_fps = 0.0f,
                                                 ViewExtent dynamic_resolution_optimal = {});
    // Present inputs enter NON_PIXEL_SHADER_RESOURCE, backbuffer PRESENT.
    // owners must keep graph backing allocations alive and unchanged, rather
    // than just retaining an ID3D12Resource. UI color is premultiplied alpha.
    [[nodiscard]] Result PreparePresent(const SdkPresentationFrame &frame);
    // Call on the present thread immediately after the actual proxy Present.
    // The presenting queue signal is the retirement boundary for FG inputs;
    // returned Present and graph fences alone do not retire them. Each lease
    // retains both COM resources and owners. The optional output captures the
    // post-Present SDK frame count without consuming it a second time.
    [[nodiscard]] Result PresentCompleted(SdkFrameIdentity identity,
                                         std::uint32_t *frames_actually_presented = nullptr);
    // Call before any queue can overwrite/reuse a previously tagged input.
    [[nodiscard]] Result WaitForPresentationInputs(ID3D12CommandQueue *queue);
    // Waits on the presenting-queue fences, keeps the in-flight lease count bounded.
    [[nodiscard]] Result DrainPresentation();
    // Caller drains the graph and SDK inputs before detach/teardown. Shutdown
    // must run while the DXGI/D3D12 objects and their proxy code are still alive.
    // Detach sets native FG Off before resize/fullscreen/destroy operations.
    [[nodiscard]] Result DetachPresentationAfterCompletion();
    // Shuts down SDK plugins but retains the DLL and native device owner.
    [[nodiscard]] Result ShutdownAfterCompletion();
    // After Shutdown, release every renderer-held SL device/factory/queue/
    // swapchain proxy, then call this. Only then is unloading proxy code safe.
    [[nodiscard]] Result UnloadAfterProxyRelease();

    [[nodiscard]] ReconstructionAvailability Availability(StreamlineFeature feature) const noexcept;
    [[nodiscard]] std::string_view FailureReason(StreamlineFeature feature) const noexcept;
    [[nodiscard]] bool Initialized() const noexcept;
    [[nodiscard]] bool FrameGenerationEnabled() const noexcept;
    [[nodiscard]] bool ReconstructionConfigured() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
