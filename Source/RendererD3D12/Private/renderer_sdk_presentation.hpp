#pragma once

#include "renderer_sdk_frame.hpp"
#include "renderer_streamline.hpp"
#include <hs/renderer/renderer.hpp>
#include <memory>
#include <span>

namespace hs
{
class FsrSession;
enum class SdkPresentationOwner : std::uint8_t { None, Streamline, Fsr, Xess };
enum class SdkLatencyOwner : std::uint8_t { None, Reflex, Xell };
struct SdkPresentationDisplay
{
    ViewExtent render_capacity, output_extent;
    DXGI_FORMAT format{DXGI_FORMAT_R8G8B8A8_UNORM};
    DXGI_COLOR_SPACE_TYPE color_space{DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
    bool vsync{}, dynamic_resolution{};
    float target_fps{};
};

// One concrete owner of native presentation. Shared SR sessions are borrowed;
// XeSS-FG and its required XeLL context are owned here. Construction and the
// default Off + Auto request load, initialize and allocate no SDK objects.
class SdkPresentation
{
  public:
    SdkPresentation();
    ~SdkPresentation();
    SdkPresentation(const SdkPresentation &) = delete;
    SdkPresentation &operator=(const SdkPresentation &) = delete;
    // Resolve only while detached and after application GPU idle. Auto checks
    // actual native support; a rejected request stays Off with an explicit status.
    [[nodiscard]] Result Resolve(ID3D12Device *, const RendererPresentationRequest &,
        StreamlineSession *, FsrSession *, const SdkPresentationDisplay &, bool gpu_idle_acknowledged);
    // There must be no existing swapchain for this HWND. For Streamline, upgrade
    // the creation factory first and then AttachSLChain to its created proxy.
    [[nodiscard]] Result UpgradeCreationFactory(void **factory);
    [[nodiscard]] Result CreateSwapChain(ID3D12CommandQueue *, IDXGIFactory2 *, HWND,
        const DXGI_SWAP_CHAIN_DESC1 &, DXGI_COLOR_SPACE_TYPE,
        Microsoft::WRL::ComPtr<IDXGISwapChain4> &);
    [[nodiscard]] Result AttachSLChain(IDXGISwapChain4 *, ID3D12CommandQueue *);
    // Invoke before ANY graph queue can overwrite guide inputs. SL queues wait
    // its actual SDK fence; FSR waits SDK presents + graph fence. Intel ONLY_NOW
    // source copies are waited; this does not claim its present queue is drained.
    [[nodiscard]] Result BeforeReuse(std::span<ID3D12CommandQueue *const> graph_queues);
    [[nodiscard]] Result RecordDirect(ID3D12GraphicsCommandList *, const SdkPresentationFrame &);
    [[nodiscard]] Result NotifySubmission(const SdkFrameIdentity &, ID3D12CommandQueue *,
                                         ID3D12Fence *, std::uint64_t value);
    // This is the sole actual Present call. It adds Present markers once, calls
    // native proxy Present, and records SDK completion/status even if DXGI fails.
    [[nodiscard]] Result Present(IDXGISwapChain4 *, const SdkFrameIdentity &, UINT sync_interval, UINT flags);
    // Abandoning a prepared frame requires actual completion of all graph
    // queues first; native drain still retires SDK asynchronous work.
    [[nodiscard]] Result DrainBeforeSwapChainMutation(bool abandon_completed_prepare = false);
    // Caller completes ALL graph queues, resets unsubmitted lists, and releases
    // every external chain/backbuffer reference first. Intel native Destroy is
    // its internal present teardown boundary; XeLL is destroyed afterwards.
    [[nodiscard]] Result DestroyAfterGpuCompletion(bool gpu_idle_acknowledged);
    // Runtime calls these on this owner thread with one identity that is carried
    // through simulation/render/present. Sleep is before actual input sampling.
    [[nodiscard]] Result InputSleep(const SdkFrameIdentity &);
    [[nodiscard]] Result Marker(const SdkFrameIdentity &, SdkLatencyMarker);
    [[nodiscard]] const RendererPresentationStatus &Status() const noexcept;
    void ReportFrameGenerationPolicy(const RendererPresentationRequest &requested, bool cadence_suppressed, bool allowed);
    [[nodiscard]] const RendererStreamlinePresentCount &ActualPresentCount() const noexcept;
    [[nodiscard]] SdkPresentationOwner PresentationOwner() const noexcept;
    [[nodiscard]] SdkLatencyOwner LatencyOwner() const noexcept;
    [[nodiscard]] bool IsEnabled() const noexcept;
    [[nodiscard]] bool UsesFrameLimiter() const noexcept;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    RendererPresentationStatus status_;
    RendererStreamlinePresentCount actual_present_count_;
};
} // namespace hs
