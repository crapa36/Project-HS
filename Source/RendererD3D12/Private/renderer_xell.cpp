#include "renderer_xell.hpp"

#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>

#ifndef HS_WITH_XELL
#define HS_WITH_XELL 0
#endif

#if HS_WITH_XELL
#include <Windows.h>
#include <xell/xell_d3d12.h>
#endif

namespace hs
{
namespace
{
Result XellFailure(ErrorCode code, std::string message)
{
    return Result::Failure(code, "hs_renderer_xell", std::move(message));
}
#if HS_WITH_XELL
bool SameIdentity(const SdkFrameIdentity &a, const SdkFrameIdentity &b)
{
    return a.render_id == b.render_id && a.presentation_id == b.presentation_id && a.viewport_id == b.viewport_id;
}
bool ValidIdentity(const SdkFrameIdentity &identity)
{
    return identity.viewport_id == 0 && identity.presentation_id <= std::numeric_limits<std::uint32_t>::max();
}
void XellLog(const char *message, xell_logging_level_t level)
{
#if defined(HS_DEVELOPMENT_TOOLS)
    if (message) std::fprintf(stderr, "hs.xell.sdk level=%u message=%s\n", static_cast<unsigned>(level), message);
#else
    static_cast<void>(message);
    static_cast<void>(level);
#endif
}
#endif
} // namespace

struct XellLatency::Impl
{
    ReconstructionAvailability availability{
#if HS_WITH_XELL
        ReconstructionAvailability::Unavailable
#else
        ReconstructionAvailability::CompiledOut
#endif
    };
    std::string failure_reason{
#if HS_WITH_XELL
        "XeLL context has not been initialized."
#else
        "XeLL was explicitly excluded by HS_WITH_XELL=OFF."
#endif
    };
    bool initialized{}, requires_fg{}, fg_active{}, low_latency{};
#if HS_WITH_XELL
    HMODULE module{};
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    xell_context_handle_t context{};
    xell_version_t version{};
    DWORD owner_thread{};
    std::uint32_t fg_users{}, markers{};
    bool has_frame{}, faulted{};
    SdkFrameIdentity frame;
    decltype(&xellGetVersion) get_version{};
    decltype(&xellD3D12CreateContext) create_context{};
    decltype(&xellSetLoggingCallback) set_logging{};
    decltype(&xellSetSleepMode) set_mode{};
    decltype(&xellGetSleepMode) get_mode{};
    decltype(&xellSleep) sleep{};
    decltype(&xellAddMarkerData) mark{};
    decltype(&xellDestroyContext) destroy_context{};

    template <typename Function> bool Load(Function &function, const char *name)
    {
        function = reinterpret_cast<Function>(GetProcAddress(module, name));
        if (function) return true;
        failure_reason = std::string("Pinned XeLL DLL is missing export ") + name + ".";
        return false;
    }
    Result RequireOwner() const
    {
        if (owner_thread != GetCurrentThreadId())
            return XellFailure(ErrorCode::InvalidState, "XeLL API called outside its initialization thread.");
        return Result::Success();
    }
    Result RequireReady() const
    {
        if (!initialized || faulted)
            return XellFailure(ErrorCode::InvalidState, failure_reason.empty() ? "XeLL requires initialization." : failure_reason);
        return RequireOwner();
    }
    Result Fail(const char *operation, xell_result_t result)
    {
        failure_reason = std::string(operation) + " failed with XeLL result " + std::to_string(static_cast<int>(result)) + ".";
        availability = result == XELL_RESULT_ERROR_UNSUPPORTED_DEVICE || result == XELL_RESULT_ERROR_UNSUPPORTED_DRIVER
            ? ReconstructionAvailability::Unsupported : ReconstructionAvailability::Unavailable;
        faulted = true;
        return XellFailure(ErrorCode::TaskFailed, failure_reason);
    }
    Result ValidateIdentity(const SdkFrameIdentity &identity) const
    {
        if (auto ready = RequireReady(); !ready) return ready;
        if (!ValidIdentity(identity) || !has_frame || !SameIdentity(frame, identity))
            return XellFailure(ErrorCode::InvalidArgument, "XeLL requires the exact frame identity passed to Sleep.");
        return Result::Success();
    }
#endif
};

XellLatency::XellLatency() : impl_(std::make_shared<Impl>()) {}
XellLatency::~XellLatency()
{
    // The coordinator performs explicit idle shutdown. On a contract violation,
    // retain native owners rather than unload code that can still be executing.
    if (auto reset = ResetAfterCompletion(false); !reset)
    {
#if defined(HS_DEVELOPMENT_TOOLS)
        std::fprintf(stderr, "hs.xell.shutdown error=%s\n", std::string(reset.Message()).c_str());
#endif
        static_cast<void>(new std::shared_ptr<Impl>(std::move(impl_)));
    }
}

Result XellLatency::Initialize(ID3D12Device *device)
{
#if !HS_WITH_XELL
    static_cast<void>(device);
    return XellFailure(ErrorCode::InvalidState, impl_->failure_reason);
#else
    if (impl_->context || impl_->module)
        return XellFailure(ErrorCode::InvalidState, "Reset the completed XeLL context before initializing it again.");
    if (!device) return XellFailure(ErrorCode::InvalidArgument, "XeLL requires a D3D12 device.");
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 adapter_description{};
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) ||
        FAILED(adapter->GetDesc1(&adapter_description)))
        return XellFailure(ErrorCode::TaskFailed, "Cannot query the actual XeLL device adapter.");
    impl_->requires_fg = adapter_description.VendorId != 0x8086;
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size())
        return XellFailure(ErrorCode::TaskFailed, "Cannot resolve the executable directory for the pinned XeLL DLL.");
    executable.resize(length);
    const auto runtime = std::filesystem::path(executable).parent_path() / L"libxell.dll";
    impl_->owner_thread = GetCurrentThreadId();
    impl_->module = LoadLibraryExW(runtime.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!impl_->module)
    {
        impl_->failure_reason = "Cannot load pinned XeLL runtime " + runtime.string() +
            " (Win32 " + std::to_string(GetLastError()) + ").";
        return XellFailure(ErrorCode::TaskFailed, impl_->failure_reason);
    }
    if (!impl_->Load(impl_->get_version, "xellGetVersion") ||
        !impl_->Load(impl_->create_context, "xellD3D12CreateContext") ||
        !impl_->Load(impl_->set_logging, "xellSetLoggingCallback") ||
        !impl_->Load(impl_->set_mode, "xellSetSleepMode") ||
        !impl_->Load(impl_->get_mode, "xellGetSleepMode") ||
        !impl_->Load(impl_->sleep, "xellSleep") ||
        !impl_->Load(impl_->mark, "xellAddMarkerData") ||
        !impl_->Load(impl_->destroy_context, "xellDestroyContext"))
        return XellFailure(ErrorCode::TaskFailed, impl_->failure_reason);
    auto native = impl_->get_version(&impl_->version);
    if (native != XELL_RESULT_SUCCESS) return impl_->Fail("xellGetVersion", native);
    native = impl_->create_context(device, &impl_->context);
    if (native != XELL_RESULT_SUCCESS) return impl_->Fail("xellD3D12CreateContext", native);
    if (!impl_->context)
    {
        impl_->failure_reason = "xellD3D12CreateContext returned success without a context.";
        impl_->faulted = true;
        return XellFailure(ErrorCode::TaskFailed, impl_->failure_reason);
    }
    impl_->device = device;
    native = impl_->set_logging(impl_->context, XELL_LOGGING_LEVEL_WARNING, XellLog);
    if (native != XELL_RESULT_SUCCESS) return impl_->Fail("xellSetLoggingCallback", native);
    xell_sleep_params_t current{};
    native = impl_->get_mode(impl_->context, &current);
    if (native != XELL_RESULT_SUCCESS) return impl_->Fail("xellGetSleepMode", native);
    impl_->low_latency = current.bLowLatencyMode != 0;
    impl_->initialized = true;
    impl_->availability = ReconstructionAvailability::Available;
    impl_->failure_reason.clear();
    return Result::Success();
#endif
}

Result XellLatency::SetModeAfterCompletion(XellMode mode, bool gpu_idle_acknowledged)
{
#if !HS_WITH_XELL
    static_cast<void>(mode); static_cast<void>(gpu_idle_acknowledged);
    return XellFailure(ErrorCode::InvalidState, impl_->failure_reason);
#else
    if (auto ready = impl_->RequireReady(); !ready) return ready;
    if (!gpu_idle_acknowledged)
        return XellFailure(ErrorCode::InvalidState, "XeLL sleep-mode changes require acknowledged GPU idle.");
    if (impl_->fg_active && !mode.low_latency)
        return XellFailure(ErrorCode::InvalidState, "Disable XeSS-FG before disabling XeLL latency reduction.");
    xell_sleep_params_t native_mode{};
    native_mode.bLowLatencyMode = mode.low_latency ? 1U : 0U;
    native_mode.minimumIntervalUs = mode.minimum_interval_us;
    auto native = impl_->set_mode(impl_->context, &native_mode);
    if (native != XELL_RESULT_SUCCESS) return impl_->Fail("xellSetSleepMode", native);
    xell_sleep_params_t applied{};
    native = impl_->get_mode(impl_->context, &applied);
    if (native != XELL_RESULT_SUCCESS) return impl_->Fail("xellGetSleepMode", native);
    if (applied.bLowLatencyMode != native_mode.bLowLatencyMode ||
        applied.minimumIntervalUs != native_mode.minimumIntervalUs || applied.bLowLatencyBoost)
    {
        impl_->failure_reason = "xellGetSleepMode did not confirm the requested mode.";
        impl_->faulted = true;
        impl_->availability = ReconstructionAvailability::Unavailable;
        return XellFailure(ErrorCode::TaskFailed, impl_->failure_reason);
    }
    impl_->low_latency = applied.bLowLatencyMode != 0;
    impl_->has_frame = false;
    impl_->markers = 0;
    return Result::Success();
#endif
}

Result XellLatency::Sleep(const SdkFrameIdentity &identity)
{
#if !HS_WITH_XELL
    static_cast<void>(identity);
    return XellFailure(ErrorCode::InvalidState, impl_->failure_reason);
#else
    if (auto ready = impl_->RequireReady(); !ready) return ready;
    if (!ValidIdentity(identity))
        return XellFailure(ErrorCode::InvalidArgument, "XeLL requires viewport 0 and a uint32 presentation ID.");
    if (impl_->requires_fg && impl_->low_latency && !impl_->fg_active)
        return XellFailure(ErrorCode::InvalidState, "Non-Intel XeLL latency reduction requires enabled XeSS-FG.");
    if (impl_->has_frame && (identity.presentation_id <= impl_->frame.presentation_id || (impl_->markers & 0x3fU) != 0x3fU))
        return XellFailure(ErrorCode::InvalidState, "XeLL Sleep requires a new increasing ID and all six required previous frame markers.");
    const auto native = impl_->sleep(impl_->context, static_cast<std::uint32_t>(identity.presentation_id));
    if (native != XELL_RESULT_SUCCESS) return impl_->Fail("xellSleep", native);
    impl_->frame = identity;
    impl_->has_frame = true;
    impl_->markers = 0;
    return Result::Success();
#endif
}

Result XellLatency::Mark(const SdkFrameIdentity &identity, SdkLatencyMarker marker)
{
#if !HS_WITH_XELL
    static_cast<void>(identity); static_cast<void>(marker);
    return XellFailure(ErrorCode::InvalidState, impl_->failure_reason);
#else
    if (auto valid = impl_->ValidateIdentity(identity); !valid) return valid;
    xell_latency_marker_type_t native_marker{};
    std::uint32_t index{}, prerequisite{};
    switch (marker)
    {
    case SdkLatencyMarker::SimulationStart: native_marker = XELL_SIMULATION_START; index = 0; break;
    case SdkLatencyMarker::SimulationEnd: native_marker = XELL_SIMULATION_END; index = 1; prerequisite = 1U; break;
    case SdkLatencyMarker::RenderSubmitStart: native_marker = XELL_RENDERSUBMIT_START; index = 2; prerequisite = 1U; break;
    case SdkLatencyMarker::RenderSubmitEnd: native_marker = XELL_RENDERSUBMIT_END; index = 3; prerequisite = 4U; break;
    case SdkLatencyMarker::PresentStart: native_marker = XELL_PRESENT_START; index = 4; prerequisite = 1U; break;
    case SdkLatencyMarker::PresentEnd: native_marker = XELL_PRESENT_END; index = 5; prerequisite = 16U; break;
    case SdkLatencyMarker::InputSample: native_marker = XELL_INPUT_SAMPLE; index = 6; prerequisite = 1U; break;
    default: return XellFailure(ErrorCode::InvalidArgument, "Unknown XeLL latency marker.");
    }
    const auto bit = 1U << index;
    if ((impl_->markers & bit) || (impl_->markers & prerequisite) != prerequisite)
        return XellFailure(ErrorCode::InvalidState, "Duplicate XeLL marker or an END marker without its START.");
    const auto native = impl_->mark(impl_->context, static_cast<std::uint32_t>(identity.presentation_id), native_marker);
    if (native != XELL_RESULT_SUCCESS) return impl_->Fail("xellAddMarkerData", native);
    impl_->markers |= bit;
    return Result::Success();
#endif
}

Result XellLatency::ValidatePresentIdentity(const SdkFrameIdentity &identity) const
{
#if !HS_WITH_XELL
    static_cast<void>(identity);
    return XellFailure(ErrorCode::InvalidState, impl_->failure_reason);
#else
    if (auto valid = impl_->ValidateIdentity(identity); !valid) return valid;
    if ((impl_->markers & 0x1fU) != 0x1fU || (impl_->markers & 0x20U))
        return XellFailure(ErrorCode::InvalidState, "XeLL presentation requires simulation/render markers and PresentStart before PresentEnd.");
    return Result::Success();
#endif
}

Result XellLatency::AttachFrameGeneration(ID3D12Device *device, void *&context, std::shared_ptr<void> &owner)
{
#if !HS_WITH_XELL
    static_cast<void>(device); static_cast<void>(context); static_cast<void>(owner);
    return XellFailure(ErrorCode::InvalidState, impl_->failure_reason);
#else
    if (auto ready = impl_->RequireReady(); !ready) return ready;
    if (device != impl_->device.Get() || impl_->fg_users || owner)
        return XellFailure(ErrorCode::InvalidState, "XeSS-FG requires the same XeLL device and an exclusive FG context.");
    if (impl_->version.major < 1 || (impl_->version.major == 1 && impl_->version.minor < 3))
        return XellFailure(ErrorCode::InvalidState, "XeSS-FG requires XeLL 1.3.0 or later.");
    owner = impl_;
    context = impl_->context;
    ++impl_->fg_users;
    return Result::Success();
#endif
}

Result XellLatency::ValidateFrameGeneration(const std::shared_ptr<void> &owner, const SdkFrameIdentity *identity)
{
#if !HS_WITH_XELL
    static_cast<void>(owner); static_cast<void>(identity);
    return XellFailure(ErrorCode::InvalidState, "XeLL was explicitly excluded by HS_WITH_XELL=OFF.");
#else
    if (!owner) return XellFailure(ErrorCode::InvalidState, "XeSS-FG has no retained XeLL context.");
    const auto state = std::static_pointer_cast<Impl>(owner);
    if (auto ready = state->RequireReady(); !ready) return ready;
    xell_sleep_params_t mode{};
    const auto native = state->get_mode(state->context, &mode);
    if (native != XELL_RESULT_SUCCESS) return state->Fail("xellGetSleepMode", native);
    state->low_latency = mode.bLowLatencyMode != 0;
    if (!state->low_latency)
        return XellFailure(ErrorCode::InvalidState, "Enable XeLL latency reduction before enabling/presenting XeSS-FG.");
    if (identity)
    {
        if (auto valid = state->ValidateIdentity(*identity); !valid) return valid;
        if ((state->markers & 0x1fU) != 0x1fU || (state->markers & 0x20U))
            return XellFailure(ErrorCode::InvalidState, "XeSS-FG present ID must match the actual XeLL PresentStart frame.");
    }
    return Result::Success();
#endif
}

void XellLatency::SetFrameGenerationActive(const std::shared_ptr<void> &owner, bool active)
{
#if HS_WITH_XELL
    if (owner) std::static_pointer_cast<Impl>(owner)->fg_active = active;
#else
    static_cast<void>(owner); static_cast<void>(active);
#endif
}
void XellLatency::DetachFrameGeneration(std::shared_ptr<void> &owner)
{
#if HS_WITH_XELL
    if (owner)
    {
        const auto state = std::static_pointer_cast<Impl>(owner);
        state->fg_active = false;
        --state->fg_users;
        owner.reset();
    }
#else
    static_cast<void>(owner);
#endif
}

Result XellLatency::ResetAfterCompletion(bool gpu_idle_acknowledged)
{
#if HS_WITH_XELL
    if (impl_->context || impl_->module)
    {
        if (auto owner = impl_->RequireOwner(); !owner) return owner;
        if (impl_->fg_users)
            return XellFailure(ErrorCode::InvalidState, "Destroy XeSS-FG before destroying its XeLL context.");
        if (impl_->context)
        {
            if (!gpu_idle_acknowledged)
                return XellFailure(ErrorCode::InvalidState, "XeLL destruction requires acknowledged GPU idle.");
            const auto native = impl_->destroy_context(impl_->context);
            if (native != XELL_RESULT_SUCCESS) return impl_->Fail("xellDestroyContext", native);
            impl_->context = nullptr;
        }
        if (impl_->module && !FreeLibrary(impl_->module))
            return XellFailure(ErrorCode::TaskFailed, "Cannot unload pinned XeLL runtime (Win32 " + std::to_string(GetLastError()) + ").");
    }
    impl_ = std::make_shared<Impl>();
#else
    static_cast<void>(gpu_idle_acknowledged);
#endif
    return Result::Success();
}

ReconstructionAvailability XellLatency::Availability() const noexcept
{
    if (impl_->initialized && impl_->requires_fg && !impl_->fg_active &&
        impl_->availability == ReconstructionAvailability::Available) return ReconstructionAvailability::Unavailable;
    return impl_->availability;
}
std::string_view XellLatency::FailureReason() const noexcept
{
    if (impl_->failure_reason.empty() && impl_->initialized && impl_->requires_fg && !impl_->fg_active)
        return "Non-Intel XeLL latency reduction requires enabled XeSS-FG.";
    return impl_->failure_reason;
}
bool XellLatency::Initialized() const noexcept { return impl_->initialized; }
bool XellLatency::ConfiguredLowLatency() const noexcept { return impl_->low_latency; }
bool XellLatency::RequiresFrameGeneration() const noexcept { return impl_->requires_fg; }
} // namespace hs
