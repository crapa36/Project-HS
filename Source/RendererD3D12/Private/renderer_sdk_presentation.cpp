#include "renderer_sdk_presentation.hpp"
#include "renderer_fsr.hpp"
#include "renderer_xell.hpp"
#include "renderer_xess_fg.hpp"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
Result Failure(ErrorCode code, std::string message)
{
    return Result::Failure(code, "hs_renderer_sdk_presentation", std::move(message));
}
bool SameIdentity(const SdkFrameIdentity &a, const SdkFrameIdentity &b)
{
    return a.render_id == b.render_id && a.presentation_id == b.presentation_id && a.viewport_id == b.viewport_id;
}
bool NativeDisplay(const SdkPresentationDisplay &display)
{
    return display.output_extent.width && display.output_extent.height &&
        display.render_capacity.width && display.render_capacity.height &&
        ((display.format == DXGI_FORMAT_R8G8B8A8_UNORM &&
          display.color_space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) ||
         (display.format == DXGI_FORMAT_R10G10B10A2_UNORM &&
          display.color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020));
}
std::string_view AvailabilityToken(ReconstructionAvailability availability)
{
    switch (availability)
    {
    case ReconstructionAvailability::Available: return "available";
    case ReconstructionAvailability::CompiledOut: return "compiled_out";
    case ReconstructionAvailability::Unsupported: return "unsupported";
    case ReconstructionAvailability::NotIntegrated: return "not_integrated";
    default: return "unavailable";
    }
}
Result WaitFence(ID3D12Fence *fence, std::uint64_t value)
{
    if (!fence || !value) return Failure(ErrorCode::InvalidState, "Presentation copy submission has no actual fence.");
    auto completed = fence->GetCompletedValue();
    if (completed == std::numeric_limits<std::uint64_t>::max())
        return Failure(ErrorCode::TaskFailed, "Presentation copy fence reports device removal.");
    if (completed >= value) return Result::Success();
    const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return Failure(ErrorCode::TaskFailed, "Cannot create presentation copy completion event.");
    const auto native = fence->SetEventOnCompletion(value, event);
    const auto waited = SUCCEEDED(native) ? WaitForSingleObject(event, INFINITE) : WAIT_FAILED;
    CloseHandle(event);
    if (FAILED(native) || waited != WAIT_OBJECT_0 || fence->GetCompletedValue() == std::numeric_limits<std::uint64_t>::max())
        return Failure(ErrorCode::TaskFailed, "Waiting for actual presentation source-copy completion failed.");
    return Result::Success();
}
} // namespace

struct SdkPresentation::Impl
{
    DWORD thread{GetCurrentThreadId()};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    IDXGISwapChain4 *chain{}; // Borrowed. Actual owner is renderer or selected SDK.
    StreamlineSession *sl{};
    FsrSession *fsr{};
    std::unique_ptr<XessFrameGeneration> xess;
    std::unique_ptr<XellLatency> xell;
    SdkPresentationOwner owner{SdkPresentationOwner::None};
    SdkLatencyOwner latency{SdkLatencyOwner::None};
    SdkPresentationDisplay display;
    bool prepared{}, submitted{}, drained{}, limiter{};
    SdkFrameIdentity identity;
    struct Copy { ComPtr<ID3D12Fence> fence; std::uint64_t value{}; };
    std::vector<Copy> copies;
    Result RequireOwner() const
    {
        return thread == GetCurrentThreadId() ? Result::Success() :
            Failure(ErrorCode::InvalidState, "SDK presentation and latency calls require their initialization thread.");
    }
    Result WaitCopies()
    {
        for (const auto &copy : copies)
            if (auto result = WaitFence(copy.fence.Get(), copy.value); !result) return result;
        copies.clear();
        return Result::Success();
    }
};

SdkPresentation::SdkPresentation() = default;
SdkPresentation::~SdkPresentation()
{
    if (impl_)
    {
        // An omitted teardown cannot prove that an SDK's internal queues or
        // callbacks stopped. Keep native code/leases alive rather than infer it.
#if defined(HS_DEVELOPMENT_TOOLS)
        OutputDebugStringW(L"SDK presentation requires explicit GPU-completed teardown; preserving native owners.\n");
#endif
        static_cast<void>(impl_.release());
    }
}

Result SdkPresentation::Resolve(ID3D12Device *device, const RendererPresentationRequest &request,
    StreamlineSession *sl, FsrSession *fsr, const SdkPresentationDisplay &display, bool gpuIdle)
{
    if (impl_) return Failure(ErrorCode::InvalidState, "Destroy the detached presentation configuration before resolving another request.");
    status_ = {};
    actual_present_count_ = {};
    status_.requested = request;
    status_.availability = "available";
    if (request.mode == GraphicsFrameGenerationMode::Off && request.low_latency != GraphicsAutoToggle::On)
        return Result::Success();
    if (!device || !gpuIdle || !NativeDisplay(display) || !std::isfinite(display.target_fps) || display.target_fps < 0)
        return Failure(ErrorCode::InvalidArgument, "Presentation resolution requires GPU idle, a native device and valid SDR/HDR10 display extents.");
    if (request.provider.value != "auto" && request.provider.value != "dlss" && request.provider.value != "fsr" &&
        request.provider.value != "xess" && request.provider.value != "off")
        return Failure(ErrorCode::InvalidArgument, "Unknown native frame-generation provider.");
    auto next = std::make_unique<Impl>();
    next->device = device; next->sl = sl; next->fsr = fsr; next->display = display;
    const auto interval = request.frame_limit_fps ?
        static_cast<std::uint32_t>(std::max<std::uint64_t>(1, 1000000ULL / request.frame_limit_fps)) : 0;
    std::string rejected;
    std::string rejectedAvailability{"unsupported"};
    const auto reject = [&](std::string text, ReconstructionAvailability availability = ReconstructionAvailability::Unsupported) {
        if (!rejected.empty()) rejected += " "; rejected += std::move(text);
        rejectedAvailability = AvailabilityToken(availability == ReconstructionAvailability::Available ?
            ReconstructionAvailability::Unsupported : availability);
    };
    const auto wants = [&](std::string_view provider) { return request.provider.value == "auto" || request.provider.value == provider; };
    const auto reflex = [&]() -> Result {
        if (!sl || !sl->Initialized()) return Failure(ErrorCode::InvalidState, "The shared Reflex session was not initialized before device creation.");
        StreamlineReflexState state;
        if (auto result = sl->QueryReflexState(state); !result) return result;
        if (!state.low_latency_available) return Failure(ErrorCode::InvalidState, "Native Reflex state reports low latency unavailable.");
        if (auto result = sl->ConfigureReflex(true, false, interval); !result) return result;
        next->latency = SdkLatencyOwner::Reflex; next->limiter = interval != 0;
        return Result::Success();
    };
    const auto xell = [&](bool forFg) -> Result {
        next->xell = std::make_unique<XellLatency>();
        if (auto result = next->xell->Initialize(device); !result) return result;
        if (!forFg && next->xell->RequiresFrameGeneration())
            return Failure(ErrorCode::InvalidState, "Native XeLL warning requires enabled XeSS-FG; standalone latency is unavailable.");
        if (auto result = next->xell->SetModeAfterCompletion({true, interval}, true); !result) return result;
        next->latency = SdkLatencyOwner::Xell; next->limiter = interval != 0;
        return Result::Success();
    };
    if (request.provider.value == "off" && request.mode != GraphicsFrameGenerationMode::Off)
        reject("Frame generation provider Off cannot enable a native generation mode.");
    if (request.mode != GraphicsFrameGenerationMode::Off && wants("dlss"))
    {
        StreamlineFrameGenerationCapabilities cap;
        Result result = sl && sl->Initialized() ? sl->QueryFrameGenerationCapabilities(cap) :
            Failure(ErrorCode::InvalidState, "DLSS-G shared session was not initialized before device creation.");
        if (result && (display.output_extent.width < cap.minimum_dimension || display.output_extent.height < cap.minimum_dimension ||
            (display.vsync && !cap.vsync_supported) ||
            (request.mode == GraphicsFrameGenerationMode::Dynamic && !cap.dynamic_supported) ||
            (request.mode != GraphicsFrameGenerationMode::Dynamic && (request.multiplier < 2 || request.multiplier > cap.maximum_multiplier))))
            result = Failure(ErrorCode::InvalidArgument, "DLSS-G native dimension, vsync, mode or multiplier capability rejects this request.");
        if (result && request.low_latency == GraphicsAutoToggle::Off)
            result = Failure(ErrorCode::InvalidArgument, "DLSS-G requires its sole Reflex latency owner enabled.");
        if (result) result = reflex();
        if (result)
        {
            result = sl->ConfigureFrameGeneration(request.mode, request.multiplier, display.target_fps,
                                                  display.dynamic_resolution ? display.render_capacity : ViewExtent{});
            if (!result)
            {
                // A failed mutating SDK call is not a safe candidate fallback.
                // Preserve the shared native owner for explicit teardown.
                next->owner = SdkPresentationOwner::Streamline;
                status_.availability = "unavailable"; status_.fallback_reason = result.Message();
                impl_ = std::move(next); return result;
            }
        }
        if (result)
        {
            next->owner = SdkPresentationOwner::Streamline;
            status_.effective_provider.value = "dlss"; status_.maximum_multiplier = cap.maximum_multiplier;
        }
        else
        {
            reject(std::string(result.Message()), sl ? sl->Availability(StreamlineFeature::FrameGeneration) : ReconstructionAvailability::Unavailable);
            if (next->latency == SdkLatencyOwner::Reflex)
            {
                if (auto disabled = sl->ConfigureReflex(false); !disabled) return disabled;
                next->latency = SdkLatencyOwner::None; next->limiter = false;
            }
        }
    }
    if (request.mode != GraphicsFrameGenerationMode::Off && next->owner == SdkPresentationOwner::None && wants("xess"))
    {
        XessFrameGenerationProperties cap;
        next->xess = std::make_unique<XessFrameGeneration>();
        Result result = request.mode == GraphicsFrameGenerationMode::Dynamic ?
            Failure(ErrorCode::InvalidArgument, "Pinned XeSS-FG exposes fixed generated-frame count, not a Dynamic mode.") :
            request.multiplier < 2 ? Failure(ErrorCode::InvalidArgument, "XeSS-FG needs at least one generated frame.") :
            next->xess->Probe(device, request.multiplier - 1, cap);
        if (result && request.low_latency == GraphicsAutoToggle::Off)
            result = Failure(ErrorCode::InvalidArgument, "XeSS-FG requires its sole XeLL latency owner enabled.");
        if (result) result = xell(true);
        if (result)
        {
            next->owner = SdkPresentationOwner::Xess;
            status_.effective_provider.value = "xess"; status_.maximum_multiplier = cap.maximum_generated_frames + 1;
        }
        else
        {
            reject(std::string(result.Message()), next->xess->Availability());
            if (next->xell)
            {
                if (auto reset = next->xell->ResetAfterCompletion(true); !reset) { impl_ = std::move(next); return reset; }
                next->xell.reset(); next->latency = SdkLatencyOwner::None; next->limiter = false;
            }
            // Probe destroys its CPU-only context before returning. If native
            // Destroy itself failed, Reset is the explicit remaining boundary;
            // retain this owner and stop selection when it cannot complete.
            if (auto reset = next->xess->ResetAfterCompletion(true); !reset) { impl_ = std::move(next); return reset; }
            next->xess.reset();
        }
    }
    if (request.mode != GraphicsFrameGenerationMode::Off && next->owner == SdkPresentationOwner::None && wants("fsr"))
    {
        std::vector<FsrVariant> fg, swap;
        Result result = request.mode == GraphicsFrameGenerationMode::Dynamic || request.mode == GraphicsFrameGenerationMode::Fixed ?
            Failure(ErrorCode::InvalidArgument, "Pinned FSR exposes no configurable multiplier or Dynamic mode; select Auto for its native callback count.") :
            !fsr || !fsr->Initialized() ? Failure(ErrorCode::InvalidState, "The shared FSR session is not bound to the native device.") :
            fsr->QueryVariants(FsrEffect::FrameGeneration, fg);
        if (result) result = fsr->QueryVariants(FsrEffect::Presentation, swap);
        if (result && (fg.empty() || swap.empty())) result = Failure(ErrorCode::InvalidState, "The signed FSR DLLs report no supported interpolation/swapchain pair.");
        if (result)
        {
            next->owner = SdkPresentationOwner::Fsr; status_.effective_provider.value = "fsr";
            // The public FSR ABI has no maximum-multiplier query. Zero is unknown;
            // ObservedFrameMultiplier comes from actual generation callbacks.
            status_.maximum_multiplier = 0;
        }
        else reject(std::string(result.Message()), fsr ? fsr->Availability(FsrEffect::FrameGeneration) : ReconstructionAvailability::Unavailable);
    }
    if (next->latency == SdkLatencyOwner::None && request.low_latency == GraphicsAutoToggle::On)
    {
        Result result = request.provider.value != "xess" ? reflex() :
            Failure(ErrorCode::InvalidState, "Reflex is not selected by this request.");
        if (!result && (wants("xess") || request.provider.value == "auto")) result = xell(false);
        if (!result)
        {
            reject(std::string(result.Message()));
            if (next->xell)
            {
                if (auto reset = next->xell->ResetAfterCompletion(true); !reset) { impl_ = std::move(next); return reset; }
                next->xell.reset();
            }
        }
    }
    status_.low_latency_active = next->latency != SdkLatencyOwner::None;
    if (next->owner == SdkPresentationOwner::None && request.mode != GraphicsFrameGenerationMode::Off)
        status_.availability = rejectedAvailability;
    if (request.low_latency == GraphicsAutoToggle::On && next->latency == SdkLatencyOwner::None)
    {
        status_.availability = "unavailable";
        next->owner = SdkPresentationOwner::None; status_.effective_provider.value = "off";
    }
    status_.fallback_reason = std::move(rejected);
    if (next->owner != SdkPresentationOwner::None || next->latency != SdkLatencyOwner::None) impl_ = std::move(next);
    return Result::Success();
}

Result SdkPresentation::UpgradeCreationFactory(void **factory)
{
    if (!impl_ || impl_->owner != SdkPresentationOwner::Streamline) return Result::Success();
    if (auto result = impl_->RequireOwner(); !result) return result;
    return impl_->sl->UpgradeFactory(factory);
}
Result SdkPresentation::CreateSwapChain(ID3D12CommandQueue *queue, IDXGIFactory2 *factory, HWND window,
    const DXGI_SWAP_CHAIN_DESC1 &description, DXGI_COLOR_SPACE_TYPE colorSpace, ComPtr<IDXGISwapChain4> &chain)
{
    if (!impl_ || impl_->owner == SdkPresentationOwner::None || impl_->owner == SdkPresentationOwner::Streamline)
        return Failure(ErrorCode::InvalidState, "Only the selected Intel/AMD owner creates its SDK chain from a fresh description.");
    if (auto result = impl_->RequireOwner(); !result) return result;
    if (chain || impl_->chain || !queue || !factory || !window || description.Format != impl_->display.format ||
        description.Width != impl_->display.output_extent.width || description.Height != impl_->display.output_extent.height ||
        colorSpace != impl_->display.color_space)
        return Failure(ErrorCode::InvalidArgument, "Release the old chain; native creation must match the resolved output description.");
    Result result = impl_->owner == SdkPresentationOwner::Fsr ?
        impl_->fsr->CreatePresentationFromDescription(queue, factory, window, description, colorSpace, impl_->display.render_capacity) :
        impl_->xess->InitializeFromDescription(impl_->device.Get(), queue, factory, window, description, colorSpace,
                                             *impl_->xell, status_.requested.multiplier - 1, true);
    if (!result) { status_.availability = "unavailable"; status_.fallback_reason = result.Message(); return result; }
    result = impl_->owner == SdkPresentationOwner::Fsr ? impl_->fsr->SetFrameGenerationEnabled(true) : impl_->xess->SetEnabled(true);
    if (!result) { status_.availability = "unavailable"; status_.fallback_reason = result.Message(); return result; }
    chain = impl_->owner == SdkPresentationOwner::Fsr ? impl_->fsr->SwapChain() : impl_->xess->SwapChain();
    impl_->chain = chain.Get(); impl_->queue = queue;
    if (impl_->owner == SdkPresentationOwner::Xess)
    {
        const auto &actual = impl_->xess->Properties();
        status_.allocated_bytes = actual.temporary_buffer_bytes + actual.temporary_texture_bytes + actual.constant_buffer_bytes;
    }
    status_.active_mode = impl_->owner == SdkPresentationOwner::Xess ? GraphicsFrameGenerationMode::Fixed : GraphicsFrameGenerationMode::Auto;
    return Result::Success();
}
Result SdkPresentation::AttachSLChain(IDXGISwapChain4 *chain, ID3D12CommandQueue *queue)
{
    if (!impl_ || impl_->owner != SdkPresentationOwner::Streamline || impl_->chain)
        return Failure(ErrorCode::InvalidState, "A resolved detached Streamline owner is required.");
    if (auto result = impl_->RequireOwner(); !result) return result;
    if (auto result = impl_->sl->AttachPresentation(chain, queue); !result)
    { status_.availability = "unavailable"; status_.fallback_reason = result.Message(); return result; }
    impl_->chain = chain; impl_->queue = queue; status_.active_mode = status_.requested.mode;
    return Result::Success();
}
Result SdkPresentation::BeforeReuse(std::span<ID3D12CommandQueue *const> queues)
{
    if (!impl_) return Result::Success();
    if (auto result = impl_->RequireOwner(); !result) return result;
    if (impl_->prepared) return Failure(ErrorCode::InvalidState, "Present the prepared frame before reusing its guide inputs.");
    if (impl_->owner == SdkPresentationOwner::Streamline)
    {
        if (queues.empty()) return Failure(ErrorCode::InvalidArgument, "Streamline reuse requires all actual graph queues.");
        for (auto *queue : queues)
        {
            if (!queue) return Failure(ErrorCode::InvalidArgument, "Presentation reuse requires each actual graph queue.");
            if (auto result = impl_->sl->WaitForPresentationInputs(queue); !result) return result;
        }
    }
    if (impl_->owner == SdkPresentationOwner::Fsr) return impl_->fsr->WaitForPresentationInputs();
    if (impl_->owner == SdkPresentationOwner::Xess) return impl_->WaitCopies();
    return Result::Success();
}
Result SdkPresentation::RecordDirect(ID3D12GraphicsCommandList *commands, const SdkPresentationFrame &frame)
{
    if (!impl_ || impl_->owner == SdkPresentationOwner::None) return Result::Success();
    if (auto result = impl_->RequireOwner(); !result) return result;
    if (!impl_->chain || impl_->prepared || impl_->drained || !commands || commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return Failure(ErrorCode::InvalidState, "One active chain and an unprepared Direct presentation list are required.");
    // Keep state even on a partially successful SDK tag. Failed graph recording
    // must be reset and explicitly torn down before another configuration.
    impl_->prepared = true; impl_->submitted = false; impl_->identity = frame.identity;
    switch (impl_->owner)
    {
    case SdkPresentationOwner::Streamline: return impl_->sl->PreparePresent(frame);
    case SdkPresentationOwner::Fsr: return impl_->fsr->PreparePresent(commands, frame);
    case SdkPresentationOwner::Xess: return impl_->xess->TagFrame(commands, frame);
    default: return Result::Success();
    }
}
Result SdkPresentation::NotifySubmission(const SdkFrameIdentity &identity, ID3D12CommandQueue *queue,
    ID3D12Fence *fence, std::uint64_t value)
{
    if (!impl_ || impl_->owner == SdkPresentationOwner::None) return Result::Success();
    if (auto result = impl_->RequireOwner(); !result) return result;
    if (!impl_->prepared || impl_->submitted || !SameIdentity(impl_->identity, identity) || queue != impl_->queue.Get() || !fence || !value)
        return Failure(ErrorCode::InvalidState, "Presentation notification requires the exact once-submitted Direct tagging list/fence/identity.");
    Result result = Result::Success();
    if (impl_->owner == SdkPresentationOwner::Fsr) result = impl_->fsr->NotifyPrepareSubmission(identity, queue, fence, value);
    if (impl_->owner == SdkPresentationOwner::Xess)
    {
        result = impl_->xess->NotifyTagSubmission(identity, queue, fence, value);
        if (result) impl_->copies.push_back({ComPtr<ID3D12Fence>(fence), value});
    }
    if (result) impl_->submitted = true;
    return result;
}
Result SdkPresentation::Present(IDXGISwapChain4 *chain, const SdkFrameIdentity &identity, UINT sync, UINT flags)
{
    actual_present_count_ = {};
    if (!chain) return Failure(ErrorCode::InvalidArgument, "Present requires the sole current swapchain.");
    if (impl_)
    {
        if (auto result = impl_->RequireOwner(); !result) return result;
        if (impl_->owner != SdkPresentationOwner::None && (chain != impl_->chain || !impl_->prepared || !impl_->submitted || !SameIdentity(identity, impl_->identity)))
            return Failure(ErrorCode::InvalidState, "Present must match the exact prepared/submitted native proxy frame.");
    }
    if (auto result = Marker(identity, SdkLatencyMarker::PresentStart); !result) return result;
    if (impl_ && impl_->owner == SdkPresentationOwner::Xess)
        if (auto result = impl_->xess->SetPresentId(identity); !result) return result;
    Result sdk = Result::Success();
    HRESULT native = S_OK;
    std::uint32_t streamline_frames_actually_presented{};
    RendererStreamlinePresentCount observation;
    if (impl_ && impl_->owner == SdkPresentationOwner::Fsr) sdk = impl_->fsr->Present(sync, flags);
    else native = chain->Present(sync, flags);
    if (impl_ && impl_->owner == SdkPresentationOwner::Streamline)
        sdk = impl_->sl->PresentCompleted(identity, &streamline_frames_actually_presented, &observation);
    if (impl_ && impl_->owner == SdkPresentationOwner::Xess)
    {
        XessFrameGenerationPresentStatus actual;
        sdk = impl_->xess->CompletePresent(identity, actual);
        status_.observed_multiplier = actual.available ? actual.frames_queued : 0;
        if (!actual.frame_generation_enabled) status_.active_mode = GraphicsFrameGenerationMode::Off;
    }
    if (impl_ && impl_->owner == SdkPresentationOwner::Fsr) status_.observed_multiplier = impl_->fsr->ObservedFrameMultiplier();
    if (impl_ && impl_->owner == SdkPresentationOwner::Streamline && sdk)
        status_.observed_multiplier = streamline_frames_actually_presented;
    const auto marker = Marker(identity, SdkLatencyMarker::PresentEnd);
    if (impl_) { impl_->prepared = false; impl_->submitted = false; }
    if (!sdk) { status_.availability = "unavailable"; status_.fallback_reason = sdk.Message(); return sdk; }
    if (!marker) return marker;
    if (FAILED(native)) return Failure(ErrorCode::TaskFailed, "Native swapchain Present failed (HRESULT " + std::to_string(static_cast<std::int32_t>(native)) + ").");
    actual_present_count_ = observation;
    return Result::Success();
}
Result SdkPresentation::DrainBeforeSwapChainMutation(bool abandonCompletedPrepare)
{
    if (!impl_) return Result::Success();
    if (auto result = impl_->RequireOwner(); !result) return result;
    if (impl_->prepared && !abandonCompletedPrepare)
        return Failure(ErrorCode::InvalidState, "Complete Present or establish graph GPU completion before abandoning this prepared frame.");
    if (impl_->owner == SdkPresentationOwner::Streamline)
    {
        if (auto result = impl_->sl->DrainPresentation(); !result) return result;
        if (auto result = impl_->sl->DetachPresentationAfterCompletion(); !result) return result;
    }
    if (impl_->owner == SdkPresentationOwner::Fsr)
        if (auto result = impl_->fsr->DrainPresentation(abandonCompletedPrepare); !result) return result;
    if (impl_->owner == SdkPresentationOwner::Xess)
    {
        if (auto result = impl_->WaitCopies(); !result) return result;
        if (auto result = impl_->xess->SetEnabled(false); !result) return result;
        // This only stops generation and retires copied sources. The caller must
        // release external refs and destroy the native SDK context before a new
        // chain. Intel has no public internal present queue fence/drain API.
    }
    impl_->prepared = impl_->submitted = false;
    impl_->drained = true; status_.active_mode = GraphicsFrameGenerationMode::Off;
    return Result::Success();
}
Result SdkPresentation::DestroyAfterGpuCompletion(bool gpuIdle)
{
    if (!impl_) return Result::Success();
    if (auto result = impl_->RequireOwner(); !result) return result;
    if (!gpuIdle) return Failure(ErrorCode::InvalidState, "SDK teardown requires all graph queues completed and unsubmitted lists reset.");
    if (impl_->owner == SdkPresentationOwner::Fsr)
        if (auto result = impl_->fsr->DestroyPresentationAfterCompletion(true); !result) return result;
    if (impl_->xess)
        if (auto result = impl_->xess->ResetAfterCompletion(true); !result) return result;
    if (impl_->owner == SdkPresentationOwner::Streamline && !impl_->drained)
    {
        if (auto result = impl_->sl->DrainPresentation(); !result) return result;
        if (auto result = impl_->sl->DetachPresentationAfterCompletion(); !result) return result;
    }
    if (impl_->latency == SdkLatencyOwner::Reflex)
        if (auto result = impl_->sl->ConfigureReflex(false); !result) return result;
    if (impl_->xell)
        if (auto result = impl_->xell->ResetAfterCompletion(true); !result) return result;
    impl_.reset(); status_.active_mode = GraphicsFrameGenerationMode::Off; status_.low_latency_active = false;
    return Result::Success();
}
Result SdkPresentation::InputSleep(const SdkFrameIdentity &identity)
{
    if (!impl_) return Result::Success();
    if (auto result = impl_->RequireOwner(); !result) return result;
    switch (impl_->latency)
    {
    case SdkLatencyOwner::Reflex: return impl_->sl->InputSleep(identity);
    case SdkLatencyOwner::Xell: return impl_->xell->Sleep(identity);
    default: return Result::Success();
    }
}
Result SdkPresentation::Marker(const SdkFrameIdentity &identity, SdkLatencyMarker marker)
{
    if (!impl_) return Result::Success();
    if (auto result = impl_->RequireOwner(); !result) return result;
    switch (impl_->latency)
    {
    case SdkLatencyOwner::Reflex: return impl_->sl->Marker(identity, marker);
    case SdkLatencyOwner::Xell: return impl_->xell->Mark(identity, marker);
    default: return Result::Success();
    }
}
const RendererPresentationStatus &SdkPresentation::Status() const noexcept { return status_; }
const RendererStreamlinePresentCount &SdkPresentation::ActualPresentCount() const noexcept { return actual_present_count_; }
void SdkPresentation::ReportFrameGenerationPolicy(const RendererPresentationRequest &requested, bool suppressed, bool allowed)
{
    status_.requested = requested;
    if (!allowed && requested.mode != GraphicsFrameGenerationMode::Off)
        status_.fallback_reason = "Frame generation is Off outside active gameplay (menus, pause or loading).";
    else if (suppressed)
        status_.fallback_reason = "Auto frame generation is Off because completed base render cadence is below the authored quality profile threshold.";
}
SdkPresentationOwner SdkPresentation::PresentationOwner() const noexcept { return impl_ ? impl_->owner : SdkPresentationOwner::None; }
SdkLatencyOwner SdkPresentation::LatencyOwner() const noexcept { return impl_ ? impl_->latency : SdkLatencyOwner::None; }
bool SdkPresentation::IsEnabled() const noexcept { return status_.active_mode != GraphicsFrameGenerationMode::Off; }
bool SdkPresentation::UsesFrameLimiter() const noexcept { return impl_ && impl_->limiter; }
} // namespace hs
