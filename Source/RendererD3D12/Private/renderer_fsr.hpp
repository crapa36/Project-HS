#pragma once

#include "renderer_sdk_frame.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hs
{
enum class FsrEffect : std::uint8_t { SuperResolution, FrameGeneration, Presentation };
struct FsrVariant
{
    // IDs and names are copied together from ffxQueryDescGetVersions for this
    // device. IDs are opaque; never persist, invent, or infer them from names.
    std::uint64_t id{};
    std::string name;
};
struct FsrUpscaleRequirements
{
    bool exposure{}, reactive{}, composition{};
    bool optional_exposure{}, optional_reactive{}, optional_composition{};
};
struct FsrCompositionCall
{
    ID3D12GraphicsCommandList *commands{};
    ID3D12Resource *source{}, *ui{}, *output{};
    D3D12_RESOURCE_STATES source_state{}, ui_state{}, output_state{};
    SdkFrameIdentity identity;
    bool generated{};
};
using FsrCompositionCallback = Result (*)(const FsrCompositionCall &, void *);

// Concrete signed AMD FSR SDK 2.3.0 D3D12 session. A default object allocates and
// loads nothing. Only the opted-in effects load their DLL from executable/fsr.
// Every public call runs on the renderer/present owner thread. The session
// serializes FG Prepare/Present/destruction with the generation callback; UI
// callbacks can run on the SDK presentation thread and must only record commands.
class FsrSession
{
  public:
    FsrSession();
    ~FsrSession();
    FsrSession(const FsrSession &) = delete;
    FsrSession &operator=(const FsrSession &) = delete;

    [[nodiscard]] Result Initialize(ID3D12Device *device);
    [[nodiscard]] Result QueryVariants(FsrEffect effect, std::vector<FsrVariant> &variants);
    // Zero variant_id requests the SDK's default; other values must have been
    // returned by this session's query for exactly this effect and device.
    [[nodiscard]] Result SelectReconstructionViewAfterCompletion(std::uint32_t viewport_id);
    [[nodiscard]] Result ConfigureUpscaling(ViewExtent maximum_render, ViewExtent output,
        std::uint64_t variant_id = 0);
    // The native context-free quality query exposes an optimal extent, not a
    // supported minimum/maximum domain. It does not mutate an upscale context.
    [[nodiscard]] Result QueryInputExtent(const GraphicsModeId &quality, ViewExtent output,
        ViewExtent &optimal, std::uint64_t variant_id = 0);
    [[nodiscard]] Result QueryUpscaleRequirements(FsrUpscaleRequirements &, std::uint64_t variant_id = 0);
    [[nodiscard]] Result ExecuteSuperResolution(ID3D12GraphicsCommandList *commands,
        const SdkReconstructionFrame &frame);
    [[nodiscard]] const FsrUpscaleRequirements &UpscaleRequirements() const noexcept;

    // Caller has completed ALL graph lists using SR, or reset unsubmitted
    // lists. Resizing/changing variants requires release before configure.
    [[nodiscard]] Result ReleaseReconstructionAfterCompletion(bool gpu_idle_acknowledged);

    // There must be NO existing swapchain for this HWND. Owns both the SDK FG
    // context and SDK swapchain context. Every DXGI interaction uses SwapChain.
    // A custom composition callback copies source and composites premultiplied
    // UI, restores its incoming states, and keeps its owner alive until drain.
    // Without a callback the SDK composites the registered UI texture itself.
    [[nodiscard]] Result CreatePresentationFromDescription(ID3D12CommandQueue *queue,
        IDXGIFactory2 *factory, HWND window, const DXGI_SWAP_CHAIN_DESC1 &description,
        DXGI_COLOR_SPACE_TYPE color_space, ViewExtent maximum_render,
        std::uint64_t generation_variant_id = 0, std::uint64_t swapchain_variant_id = 0,
        FsrCompositionCallback composition = nullptr, void *composition_context = nullptr,
        std::shared_ptr<const void> composition_owner = {});
    [[nodiscard]] Result SetFrameGenerationEnabled(bool enabled);
    // Configure + PrepareV2 once per real render ID, in that order. Input depth
    // is inverted device Z; UV Previous-Current motion is scaled to render pixels.
    // IDs ascend; gaps explicitly reset generation. The current back_buffer
    // contains the display-encoded HUD-less scene, before UI composition.
    // owners retain backing graph allocations unchanged, in addition to COM refs.
    [[nodiscard]] Result PreparePresent(ID3D12GraphicsCommandList *commands,
        const SdkPresentationFrame &frame);
    // Fence is signalled AFTER submission of the exact Prepare list on gameQueue.
    // It proves application guide work completion; it does not retire SDK work.
    [[nodiscard]] Result NotifyPrepareSubmission(const SdkFrameIdentity &identity,
        ID3D12CommandQueue *queue, ID3D12Fence *fence, std::uint64_t value);
    // Sole native Present entry point; calls the SDK proxy under the same lock as
    // Prepare. It never retires leases merely because Present returned.
    [[nodiscard]] Result Present(std::uint32_t sync_interval, std::uint32_t flags);
    // Call before overwriting/reusing tagged inputs. Uses the SDK's documented
    // WaitForPresents dispatch plus submitted graph fences before retiring leases.
    [[nodiscard]] Result WaitForPresentationInputs();
    // Disables FG and UI callbacks/HUDless/UI, flushes SDK interpolation and UI
    // work via Configure, then waits graph Prepare fences. Required before resize.
    [[nodiscard]] Result DrainPresentation(bool completed_prepare_abandoned = false);
    // Stop submits, reset unsubmitted lists, complete graph uses and release every
    // borrowed proxy/backbuffer reference first. Failed destruction retains owners.
    [[nodiscard]] Result DestroyPresentationAfterCompletion(bool gpu_idle_acknowledged);
    [[nodiscard]] Result ShutdownAfterCompletion(bool gpu_idle_acknowledged);

    [[nodiscard]] IDXGISwapChain4 *SwapChain() const noexcept;
    [[nodiscard]] ReconstructionAvailability Availability(FsrEffect effect) const noexcept;
    [[nodiscard]] std::string_view FailureReason(FsrEffect effect) const noexcept;
    [[nodiscard]] std::string_view SelectedVariantName(FsrEffect effect) const noexcept;
    // SDK 2.3 exposes no maximum-multiplier query or requested multiplier key.
    // This is the actual native callback's count + real frame, zero until called.
    [[nodiscard]] std::uint32_t ObservedFrameMultiplier() const noexcept;
    [[nodiscard]] bool Initialized() const noexcept;
    [[nodiscard]] bool FrameGenerationEnabled() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
