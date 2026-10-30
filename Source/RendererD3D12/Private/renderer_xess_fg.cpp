#include "renderer_xess_fg.hpp"
#include "renderer_xell.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>

#ifndef HS_WITH_XESS_FG
#define HS_WITH_XESS_FG 0
#endif

#if HS_WITH_XESS_FG
#include <Windows.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#endif

namespace hs
{
namespace
{
Result FgFailure(ErrorCode code, std::string message)
{
    return Result::Failure(code, "hs_renderer_xess_fg", std::move(message));
}
#if HS_WITH_XESS_FG
bool SameIdentity(const SdkFrameIdentity &a, const SdkFrameIdentity &b)
{
    return a.render_id == b.render_id && a.presentation_id == b.presentation_id && a.viewport_id == b.viewport_id;
}
bool ValidIdentity(const SdkFrameIdentity &identity)
{
    return identity.viewport_id == 0 && identity.presentation_id <= std::numeric_limits<std::uint32_t>::max();
}
bool FitsRect(const D3D12_RESOURCE_DESC &description, const SdkRect &rect)
{
    return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && description.DepthOrArraySize == 1 &&
        description.SampleDesc.Count == 1 && description.MipLevels == 1 && rect.width && rect.height &&
        static_cast<std::uint64_t>(rect.x) + rect.width <= description.Width &&
        static_cast<std::uint64_t>(rect.y) + rect.height <= description.Height;
}
bool ValidColorSpace(DXGI_FORMAT format, DXGI_COLOR_SPACE_TYPE color_space)
{
    return ((format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM) &&
        color_space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) ||
        (format == DXGI_FORMAT_R10G10B10A2_UNORM && color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
}
bool FiniteMatrix(const DirectX::XMFLOAT4X4 &matrix)
{
    for (const auto &row : matrix.m) for (const auto value : row) if (!std::isfinite(value)) return false;
    const auto determinant = DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(DirectX::XMLoadFloat4x4(&matrix)));
    return std::isfinite(determinant) && determinant != 0.0f;
}
void FgLog(const char *message, xefg_swapchain_logging_level_t level, void *)
{
#if defined(HS_DEVELOPMENT_TOOLS)
    if (message) std::fprintf(stderr, "hs.xess_fg.sdk level=%u message=%s\n", static_cast<unsigned>(level), message);
#else
    static_cast<void>(message);
    static_cast<void>(level);
#endif
}
#endif
} // namespace

struct XessFrameGeneration::Impl
{
    ReconstructionAvailability availability{
#if HS_WITH_XESS_FG
        ReconstructionAvailability::Unavailable
#else
        ReconstructionAvailability::CompiledOut
#endif
    };
    std::string failure_reason{
#if HS_WITH_XESS_FG
        "XeSS-FG context has not been initialized."
#else
        "XeSS-FG was explicitly excluded by HS_WITH_XESS_FG=OFF."
#endif
    };
    XessFrameGenerationProperties properties;
    bool initialized{}, enabled{};
#if HS_WITH_XESS_FG
    HMODULE module{};
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> swap_chain;
    xefg_swapchain_handle_t context{};
    std::shared_ptr<void> latency_owner;
    DWORD owner_thread{};
    DXGI_SWAP_CHAIN_DESC1 description{};
    DXGI_COLOR_SPACE_TYPE color_space{};
    bool faulted{}, present_armed{};
    SdkFrameIdentity armed_frame;
    struct PendingFrame
    {
        SdkFrameIdentity identity;
        bool occupied{}, tags_complete{}, submitted{}, presented{};
        std::vector<std::shared_ptr<const void>> owners;
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 5> resources;
        Microsoft::WRL::ComPtr<ID3D12Fence> copy_fence;
        std::uint64_t copy_value{};
    };
    // Fixed-size slots based on the actual swap-chain BufferCount. Each slot's
    // source ownership ends at its ONLY_NOW copy completion, not Present status.
    std::vector<PendingFrame> pending;
    decltype(&xefgSwapChainGetVersion) get_version{};
    decltype(&xefgSwapChainD3D12CreateContext) create_context{};
    decltype(&xefgSwapChainGetProperties) get_properties{};
    decltype(&xefgSwapChainD3D12GetProperties) get_d3d12_properties{};
    decltype(&xefgSwapChainD3D12BuildPipelines) build_pipelines{};
    decltype(&xefgSwapChainD3D12InitFromSwapChainDesc) initialize{};
    decltype(&xefgSwapChainD3D12GetInitializationParameters) get_initialization{};
    decltype(&xefgSwapChainD3D12GetSwapChainPtr) get_swap_chain{};
    decltype(&xefgSwapChainSetLoggingCallback) set_logging{};
    decltype(&xefgSwapChainSetLatencyReduction) set_latency{};
    decltype(&xefgSwapChainSetEnabled) set_enabled{};
    decltype(&xefgSwapChainSetNumInterpolatedFrames) set_generated_frames{};
    decltype(&xefgSwapChainSetUiCompositionState) set_ui_composition{};
    decltype(&xefgSwapChainD3D12TagFrameResource) tag_resource{};
    decltype(&xefgSwapChainTagFrameConstants) tag_constants{};
    decltype(&xefgSwapChainSetPresentId) set_present_id{};
    decltype(&xefgSwapChainGetLastPresentStatus) get_present_status{};
    decltype(&xefgSwapChainDestroy) destroy_context{};

    template <typename Function> bool Load(Function &function, const char *name)
    {
        function = reinterpret_cast<Function>(GetProcAddress(module, name));
        if (function) return true;
        failure_reason = std::string("Pinned XeSS-FG DLL is missing export ") + name + ".";
        return false;
    }
    Result RequireOwner() const
    {
        if (owner_thread != GetCurrentThreadId())
            return FgFailure(ErrorCode::InvalidState, "XeSS-FG API called outside its initialization thread.");
        return Result::Success();
    }
    Result RequireReady() const
    {
        if (!initialized || faulted)
            return FgFailure(ErrorCode::InvalidState, failure_reason.empty() ? "XeSS-FG requires initialization." : failure_reason);
        return RequireOwner();
    }
    Result Fail(const char *operation, xefg_swapchain_result_t native)
    {
        failure_reason = std::string(operation) + " failed with XeSS-FG result " + std::to_string(static_cast<int>(native)) + ".";
        availability = native == XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE || native == XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DRIVER
            ? ReconstructionAvailability::Unsupported : ReconstructionAvailability::Unavailable;
        faulted = true;
        return FgFailure(ErrorCode::TaskFailed, failure_reason);
    }
    Result Check(const char *operation, xefg_swapchain_result_t native)
    {
        if (native < XEFG_SWAPCHAIN_RESULT_SUCCESS) return Fail(operation, native);
        if (native > XEFG_SWAPCHAIN_RESULT_SUCCESS)
#if defined(HS_DEVELOPMENT_TOOLS)
            std::fprintf(stderr, "hs.xess_fg.warning operation=%s native_result=%d\n", operation, static_cast<int>(native));
#endif
        return Result::Success();
    }
    bool SameDevice(ID3D12DeviceChild *child) const
    {
        Microsoft::WRL::ComPtr<ID3D12Device> candidate;
        return child && SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&candidate))) && candidate.Get() == device.Get();
    }
    PendingFrame *Find(const SdkFrameIdentity &identity)
    {
        for (auto &frame : pending) if (frame.occupied && SameIdentity(frame.identity, identity)) return &frame;
        return nullptr;
    }
    void Collect()
    {
        for (auto &frame : pending)
        {
            if (!frame.occupied || !frame.submitted) continue;
            const auto completed = frame.copy_fence->GetCompletedValue();
            // UINT64_MAX means device removal, not successful fence completion.
            if (completed == std::numeric_limits<std::uint64_t>::max() || completed < frame.copy_value) continue;
            frame.owners.clear();
            for (auto &resource : frame.resources) resource.Reset();
            if (frame.presented) frame = {};
        }
    }
#endif
};

XessFrameGeneration::XessFrameGeneration() : impl_(std::make_unique<Impl>()) {}
XessFrameGeneration::~XessFrameGeneration()
{
    if (auto reset = ResetAfterCompletion(false); !reset)
    {
#if defined(HS_DEVELOPMENT_TOOLS)
        std::fprintf(stderr, "hs.xess_fg.shutdown error=%s\n", std::string(reset.Message()).c_str());
#endif
        static_cast<void>(impl_.release());
    }
}

Result XessFrameGeneration::Probe(ID3D12Device *device, std::uint32_t generated_frames,
    XessFrameGenerationProperties &properties)
{
    properties = {};
#if !HS_WITH_XESS_FG
    static_cast<void>(device); static_cast<void>(generated_frames);
    return FgFailure(ErrorCode::InvalidState, impl_->failure_reason);
#else
    if (impl_->context || impl_->module)
        return FgFailure(ErrorCode::InvalidState, "XeSS-FG Probe requires an object without an existing context/proxy.");
    if (!device || !generated_frames)
        return FgFailure(ErrorCode::InvalidArgument, "XeSS-FG Probe requires the actual device and a nonzero generated-frame count.");
    impl_->owner_thread = GetCurrentThreadId();
    impl_->device = device;
    const auto finish = [&](Result result) -> Result
    {
        // Query contexts have no recorded/submitted GPU work. Destroy remains a
        // required native boundary; never unload code behind a live context.
        const auto availability = impl_->availability;
        const auto reason = impl_->failure_reason;
        if (impl_->context)
        {
            if (auto destroyed = impl_->Check("xefgSwapChainDestroy (Probe)", impl_->destroy_context(impl_->context)); !destroyed) return destroyed;
            impl_->context = nullptr;
        }
        if (impl_->module && !FreeLibrary(impl_->module))
        {
            impl_->availability = ReconstructionAvailability::Unavailable;
            impl_->failure_reason = "Cannot unload XeSS-FG Probe runtime (Win32 " + std::to_string(GetLastError()) + ").";
            return FgFailure(ErrorCode::TaskFailed, impl_->failure_reason);
        }
        const auto queried = impl_->properties;
        impl_ = std::make_unique<Impl>(); impl_->availability = availability; impl_->failure_reason = reason;
        impl_->properties = queried;
        if (result) properties = queried;
        return result;
    };
    const auto failed = [&](ErrorCode code, std::string message, ReconstructionAvailability availability = ReconstructionAvailability::Unavailable) -> Result
    {
        impl_->availability = availability; impl_->failure_reason = std::move(message);
        return finish(FgFailure(code, impl_->failure_reason));
    };
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 adapter_description{};
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) || FAILED(adapter->GetDesc1(&adapter_description)))
        return failed(ErrorCode::TaskFailed, "Cannot query the actual XeSS-FG Probe device adapter.");
    impl_->properties.intel_device = adapter_description.VendorId == 0x8086;
    if (!impl_->properties.intel_device)
    {
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_4};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) || model.HighestShaderModel < D3D_SHADER_MODEL_6_4)
            return failed(ErrorCode::InvalidState, "Non-Intel XeSS-FG requires Shader Model 6.4 on the actual selected device.", ReconstructionAvailability::Unsupported);
    }
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) return failed(ErrorCode::TaskFailed, "Cannot resolve the pinned XeSS-FG Probe runtime directory.");
    executable.resize(length);
    const auto runtime = std::filesystem::path(executable).parent_path() / L"libxess_fg.dll";
    impl_->module = LoadLibraryExW(runtime.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!impl_->module) return failed(ErrorCode::TaskFailed, "Cannot load pinned XeSS-FG Probe runtime (Win32 " + std::to_string(GetLastError()) + ").");
    if (!impl_->Load(impl_->get_version, "xefgSwapChainGetVersion") || !impl_->Load(impl_->create_context, "xefgSwapChainD3D12CreateContext") ||
        !impl_->Load(impl_->get_properties, "xefgSwapChainGetProperties") || !impl_->Load(impl_->destroy_context, "xefgSwapChainDestroy"))
        return failed(ErrorCode::TaskFailed, impl_->failure_reason);
    xefg_swapchain_version_t version{};
    if (auto result = impl_->Check("xefgSwapChainGetVersion (Probe)", impl_->get_version(&version)); !result) return finish(result);
    if (auto result = impl_->Check("xefgSwapChainD3D12CreateContext (Probe)", impl_->create_context(device, &impl_->context)); !result) return finish(result);
    if (!impl_->context) return failed(ErrorCode::TaskFailed, "XeSS-FG Probe CreateContext returned no native context.");
    xefg_swapchain_properties_t native_properties{};
    if (auto result = impl_->Check("xefgSwapChainGetProperties (Probe)", impl_->get_properties(impl_->context, &native_properties)); !result) return finish(result);
    impl_->properties.maximum_generated_frames = native_properties.maxSupportedInterpolations;
    if (!native_properties.maxSupportedInterpolations || generated_frames > native_properties.maxSupportedInterpolations)
        return failed(ErrorCode::InvalidState, "Generated-frame count exceeds the actual XeSS-FG Probe properties.", ReconstructionAvailability::Unsupported);
    if (!impl_->properties.intel_device && (native_properties.maxSupportedInterpolations != 1 || generated_frames != 1))
        return failed(ErrorCode::InvalidState, "Pinned non-Intel XeSS-FG Probe did not confirm its one-generated-frame limit.", ReconstructionAvailability::Unsupported);
    impl_->availability = ReconstructionAvailability::Available; impl_->failure_reason.clear();
    return finish(Result::Success());
#endif
}

Result XessFrameGeneration::InitializeFromDescription(ID3D12Device *device, ID3D12CommandQueue *queue,
    IDXGIFactory2 *factory, HWND window, const DXGI_SWAP_CHAIN_DESC1 &description,
    DXGI_COLOR_SPACE_TYPE color_space, XellLatency &latency, std::uint32_t generated_frames,
    bool gpu_idle_acknowledged)
{
#if !HS_WITH_XESS_FG
    static_cast<void>(device); static_cast<void>(queue); static_cast<void>(factory); static_cast<void>(window);
    static_cast<void>(description); static_cast<void>(color_space); static_cast<void>(latency);
    static_cast<void>(generated_frames); static_cast<void>(gpu_idle_acknowledged);
    return FgFailure(ErrorCode::InvalidState, impl_->failure_reason);
#else
    if (impl_->context || impl_->module)
        return FgFailure(ErrorCode::InvalidState, "Reset the completed XeSS-FG context before initializing it again.");
    if (!gpu_idle_acknowledged)
        return FgFailure(ErrorCode::InvalidState, "XeSS-FG swap-chain creation requires acknowledged old-provider/GPU shutdown.");
    if (!device || !queue || !factory || !window || !description.Width || !description.Height ||
        description.BufferCount < 2 || description.BufferCount > 16 || description.SampleDesc.Count != 1 ||
        (description.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD && description.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL) ||
        !ValidColorSpace(description.Format, color_space) || !generated_frames ||
        queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return FgFailure(ErrorCode::InvalidArgument, "XeSS-FG requires a Direct queue, flip-model SDR/HDR10 swap chain, and nonzero generated-frame count.");
    impl_->device = device;
    if (!impl_->SameDevice(queue))
        return FgFailure(ErrorCode::InvalidArgument, "XeSS-FG presentation queue belongs to another device.");
    Microsoft::WRL::ComPtr<IDXGIFactory4> adapter_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 adapter_description{};
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&adapter_factory))) ||
        FAILED(adapter_factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) ||
        FAILED(adapter->GetDesc1(&adapter_description)))
        return FgFailure(ErrorCode::TaskFailed, "Cannot query the actual XeSS-FG device adapter.");
    impl_->properties.intel_device = adapter_description.VendorId == 0x8086;
    if (!impl_->properties.intel_device)
    {
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_4};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) ||
            model.HighestShaderModel < D3D_SHADER_MODEL_6_4)
        {
            impl_->availability = ReconstructionAvailability::Unsupported;
            impl_->failure_reason = "Non-Intel XeSS-FG requires Shader Model 6.4 on the actual selected device.";
            return FgFailure(ErrorCode::InvalidState, impl_->failure_reason);
        }
        if (generated_frames != 1)
            return FgFailure(ErrorCode::InvalidArgument, "Pinned XeSS-FG supports exactly one generated frame on non-Intel GPUs.");
    }
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size())
        return FgFailure(ErrorCode::TaskFailed, "Cannot resolve the executable directory for the pinned XeSS-FG DLL.");
    executable.resize(length);
    const auto runtime = std::filesystem::path(executable).parent_path() / L"libxess_fg.dll";
    impl_->owner_thread = GetCurrentThreadId();
    impl_->module = LoadLibraryExW(runtime.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!impl_->module)
    {
        impl_->failure_reason = "Cannot load pinned XeSS-FG runtime " + runtime.string() +
            " (Win32 " + std::to_string(GetLastError()) + ").";
        return FgFailure(ErrorCode::TaskFailed, impl_->failure_reason);
    }
    if (!impl_->Load(impl_->get_version, "xefgSwapChainGetVersion") ||
        !impl_->Load(impl_->create_context, "xefgSwapChainD3D12CreateContext") ||
        !impl_->Load(impl_->get_properties, "xefgSwapChainGetProperties") ||
        !impl_->Load(impl_->get_d3d12_properties, "xefgSwapChainD3D12GetProperties") ||
        !impl_->Load(impl_->build_pipelines, "xefgSwapChainD3D12BuildPipelines") ||
        !impl_->Load(impl_->initialize, "xefgSwapChainD3D12InitFromSwapChainDesc") ||
        !impl_->Load(impl_->get_initialization, "xefgSwapChainD3D12GetInitializationParameters") ||
        !impl_->Load(impl_->get_swap_chain, "xefgSwapChainD3D12GetSwapChainPtr") ||
        !impl_->Load(impl_->set_logging, "xefgSwapChainSetLoggingCallback") ||
        !impl_->Load(impl_->set_latency, "xefgSwapChainSetLatencyReduction") ||
        !impl_->Load(impl_->set_enabled, "xefgSwapChainSetEnabled") ||
        !impl_->Load(impl_->set_generated_frames, "xefgSwapChainSetNumInterpolatedFrames") ||
        !impl_->Load(impl_->set_ui_composition, "xefgSwapChainSetUiCompositionState") ||
        !impl_->Load(impl_->tag_resource, "xefgSwapChainD3D12TagFrameResource") ||
        !impl_->Load(impl_->tag_constants, "xefgSwapChainTagFrameConstants") ||
        !impl_->Load(impl_->set_present_id, "xefgSwapChainSetPresentId") ||
        !impl_->Load(impl_->get_present_status, "xefgSwapChainGetLastPresentStatus") ||
        !impl_->Load(impl_->destroy_context, "xefgSwapChainDestroy"))
        return FgFailure(ErrorCode::TaskFailed, impl_->failure_reason);
    xefg_swapchain_version_t version{};
    if (auto result = impl_->Check("xefgSwapChainGetVersion", impl_->get_version(&version)); !result) return result;
    if (auto result = impl_->Check("xefgSwapChainD3D12CreateContext", impl_->create_context(device, &impl_->context)); !result) return result;
    if (!impl_->context)
        return FgFailure(ErrorCode::TaskFailed, "xefgSwapChainD3D12CreateContext returned success without a context.");
    if (auto result = impl_->Check("xefgSwapChainSetLoggingCallback",
        impl_->set_logging(impl_->context, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, FgLog, nullptr)); !result) return result;
    void *latency_context{};
    if (auto result = latency.AttachFrameGeneration(device, latency_context, impl_->latency_owner); !result) return result;
    if (auto result = impl_->Check("xefgSwapChainSetLatencyReduction", impl_->set_latency(impl_->context, latency_context)); !result) return result;
    xefg_swapchain_properties_t native_properties{};
    if (auto result = impl_->Check("xefgSwapChainGetProperties", impl_->get_properties(impl_->context, &native_properties)); !result) return result;
    impl_->properties.maximum_generated_frames = native_properties.maxSupportedInterpolations;
    if (!native_properties.maxSupportedInterpolations || generated_frames > native_properties.maxSupportedInterpolations)
        return FgFailure(ErrorCode::InvalidArgument, "Generated-frame count exceeds the actual XeSS-FG context properties.");
    if (!impl_->properties.intel_device && native_properties.maxSupportedInterpolations != 1)
        return FgFailure(ErrorCode::TaskFailed, "Pinned non-Intel XeSS-FG properties did not confirm its documented one-generated-frame limit.");
    xefg_swapchain_d3d12_init_params_t initialization{};
    initialization.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH;
    initialization.maxInterpolatedFrames = impl_->properties.maximum_generated_frames;
    initialization.creationNodeMask = 1;
    initialization.visibleNodeMask = 1;
    initialization.uiMode = XEFG_SWAPCHAIN_UI_MODE_HUDLESS_UITEXTURE;
    if (auto result = impl_->Check("xefgSwapChainD3D12GetProperties", impl_->get_d3d12_properties(impl_->context,
        &initialization, description.Width, description.Height, description.Format, &native_properties)); !result) return result;
    impl_->properties.descriptor_count = native_properties.requiredDescriptorCount;
    impl_->properties.temporary_buffer_bytes = native_properties.tempBufferHeapSize;
    impl_->properties.temporary_texture_bytes = native_properties.tempTextureHeapSize;
    impl_->properties.constant_buffer_bytes = native_properties.constantBufferSize;
    if (auto result = impl_->Check("xefgSwapChainD3D12BuildPipelines",
        impl_->build_pipelines(impl_->context, nullptr, 1, initialization.initFlags)); !result) return result;
    impl_->queue = queue;
    if (auto result = impl_->Check("xefgSwapChainD3D12InitFromSwapChainDesc",
        impl_->initialize(impl_->context, window, &description, nullptr, queue, factory, &initialization)); !result) return result;
    xefg_swapchain_d3d12_init_params_t applied{};
    if (auto result = impl_->Check("xefgSwapChainD3D12GetInitializationParameters",
        impl_->get_initialization(impl_->context, &applied)); !result) return result;
    impl_->properties.initialized_maximum_generated_frames = applied.maxInterpolatedFrames;
    if (applied.pApplicationSwapChain || applied.maxInterpolatedFrames != initialization.maxInterpolatedFrames ||
        applied.initFlags != initialization.initFlags || applied.uiMode != initialization.uiMode)
        return FgFailure(ErrorCode::TaskFailed, "XeSS-FG returned initialization parameters different from the requested configuration.");
    if (auto result = impl_->Check("xefgSwapChainD3D12GetSwapChainPtr",
        impl_->get_swap_chain(impl_->context, IID_PPV_ARGS(&impl_->swap_chain))); !result) return result;
    if (!impl_->swap_chain) return FgFailure(ErrorCode::TaskFailed, "XeSS-FG returned no proxy swap chain.");
    UINT color_support{};
    if (FAILED(impl_->swap_chain->CheckColorSpaceSupport(color_space, &color_support)) ||
        !(color_support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) ||
        FAILED(impl_->swap_chain->SetColorSpace1(color_space)))
        return FgFailure(ErrorCode::TaskFailed, "XeSS-FG proxy cannot present the requested output color space.");
    if (auto result = impl_->Check("xefgSwapChainSetUiCompositionState",
        impl_->set_ui_composition(impl_->context, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED)); !result) return result;
    if (auto result = impl_->Check("xefgSwapChainSetNumInterpolatedFrames",
        impl_->set_generated_frames(impl_->context, generated_frames)); !result) return result;
    if (auto result = impl_->Check("xefgSwapChainSetEnabled", impl_->set_enabled(impl_->context, 0)); !result) return result;
    impl_->description = description;
    impl_->color_space = color_space;
    impl_->pending.resize(description.BufferCount);
    impl_->initialized = true;
    impl_->availability = ReconstructionAvailability::Available;
    impl_->failure_reason.clear();
    return Result::Success();
#endif
}

Result XessFrameGeneration::SetEnabled(bool enabled)
{
#if !HS_WITH_XESS_FG
    static_cast<void>(enabled);
    return FgFailure(ErrorCode::InvalidState, impl_->failure_reason);
#else
    if (auto ready = impl_->RequireReady(); !ready) return ready;
    if (impl_->present_armed)
        return FgFailure(ErrorCode::InvalidState, "Complete the armed XeSS-FG Present before changing enable state.");
    if (enabled)
    {
        if (auto result = XellLatency::ValidateFrameGeneration(impl_->latency_owner, nullptr); !result) return result;
        BOOL fullscreen{};
        if (FAILED(impl_->swap_chain->GetFullscreenState(&fullscreen, nullptr)) || fullscreen)
            return FgFailure(ErrorCode::InvalidState, "XeSS-FG cannot be enabled in exclusive fullscreen.");
    }
    if (auto result = impl_->Check("xefgSwapChainSetEnabled", impl_->set_enabled(impl_->context, enabled ? 1U : 0U)); !result) return result;
    impl_->enabled = enabled;
    XellLatency::SetFrameGenerationActive(impl_->latency_owner, enabled);
    return Result::Success();
#endif
}

Result XessFrameGeneration::SetGeneratedFrames(std::uint32_t count)
{
#if !HS_WITH_XESS_FG
    static_cast<void>(count);
    return FgFailure(ErrorCode::InvalidState, impl_->failure_reason);
#else
    if (auto ready = impl_->RequireReady(); !ready) return ready;
    if (!count || count > impl_->properties.initialized_maximum_generated_frames ||
        (!impl_->properties.intel_device && count != 1))
        return FgFailure(ErrorCode::InvalidArgument, "Generated-frame count exceeds the actual initialized XeSS-FG limit.");
    if (impl_->present_armed)
        return FgFailure(ErrorCode::InvalidState, "Complete the armed XeSS-FG Present before changing generated-frame count.");
    return impl_->Check("xefgSwapChainSetNumInterpolatedFrames", impl_->set_generated_frames(impl_->context, count));
#endif
}

Result XessFrameGeneration::TagFrame(ID3D12GraphicsCommandList *commands, const SdkPresentationFrame &frame)
{
#if !HS_WITH_XESS_FG
    static_cast<void>(commands); static_cast<void>(frame);
    return FgFailure(ErrorCode::InvalidState, impl_->failure_reason);
#else
    if (auto ready = impl_->RequireReady(); !ready) return ready;
    if (!impl_->enabled)
        return FgFailure(ErrorCode::InvalidState, "XeSS-FG tags require an enabled context.");
    if (!commands || commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || !impl_->SameDevice(commands) ||
        !ValidIdentity(frame.identity) || !frame.hudless || !frame.ui || !frame.depth || !frame.motion ||
        !frame.back_buffer || frame.owners.empty() ||
        std::any_of(frame.owners.begin(), frame.owners.end(), [](const auto &owner) { return !owner; }))
        return FgFailure(ErrorCode::InvalidArgument, "XeSS-FG requires a Direct list, full native presentation packet, and resource owners.");
    if (frame.hudless_state != D3D12_RESOURCE_STATE_COPY_SOURCE || frame.ui_state != D3D12_RESOURCE_STATE_COPY_SOURCE ||
        frame.depth_state != D3D12_RESOURCE_STATE_COPY_SOURCE || frame.motion_state != D3D12_RESOURCE_STATE_COPY_SOURCE ||
        frame.back_buffer_state != D3D12_RESOURCE_STATE_PRESENT)
        return FgFailure(ErrorCode::InvalidArgument, "XeSS-FG ONLY_NOW guides must enter COPY_SOURCE and the proxy back buffer must enter PRESENT.");
    if (frame.output_format != impl_->description.Format || frame.color_space != impl_->color_space ||
        !FiniteMatrix(frame.camera.view) || !FiniteMatrix(frame.camera.projection) || !frame.camera.reversed_depth ||
        !std::isfinite(frame.camera.delta_seconds * 1000.0f) || frame.camera.delta_seconds < 0.0f ||
        !std::isfinite(frame.camera.jitter_pixels[0]) || !std::isfinite(frame.camera.jitter_pixels[1]) ||
        std::abs(frame.camera.jitter_pixels[0]) > 0.5f || std::abs(frame.camera.jitter_pixels[1]) > 0.5f)
        return FgFailure(ErrorCode::InvalidArgument, "XeSS-FG frame output format/color space, unjittered camera, or timing is invalid.");
    const std::array<ID3D12Resource *, 5> resources{frame.hudless, frame.ui, frame.depth, frame.motion, frame.back_buffer};
    for (std::size_t i = 0; i != resources.size(); ++i)
    {
        if (!impl_->SameDevice(resources[i]))
            return FgFailure(ErrorCode::InvalidArgument, "XeSS-FG packet contains a resource from another device.");
        for (std::size_t j = i + 1; j != resources.size(); ++j)
            if (resources[i] == resources[j])
                return FgFailure(ErrorCode::InvalidArgument, "XeSS-FG guide resources must not alias each other.");
    }
    const auto depth = frame.depth->GetDesc();
    const auto motion = frame.motion->GetDesc();
    if (!FitsRect(depth, frame.render_rect) || !FitsRect(motion, frame.render_rect) ||
        depth.Width != motion.Width || depth.Height != motion.Height || motion.Format != DXGI_FORMAT_R16G16_FLOAT ||
        (depth.Format != DXGI_FORMAT_R32_TYPELESS && depth.Format != DXGI_FORMAT_D32_FLOAT && depth.Format != DXGI_FORMAT_R32_FLOAT))
        return FgFailure(ErrorCode::InvalidArgument, "XeSS-FG requires matched single-sample R32 depth/RG16F motion and valid render rectangles.");
    for (auto *resource : {frame.hudless, frame.ui, frame.back_buffer})
    {
        const auto description = resource->GetDesc();
        if (!FitsRect(description, frame.output_rect) || description.Width != impl_->description.Width ||
            description.Height != impl_->description.Height || description.Format != frame.output_format)
            return FgFailure(ErrorCode::InvalidArgument, "XeSS-FG HUD-less/UI/back buffer must match the output dimensions, format and rectangle.");
    }
    Microsoft::WRL::ComPtr<ID3D12Resource> current_back_buffer;
    if (FAILED(impl_->swap_chain->GetBuffer(impl_->swap_chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&current_back_buffer))) ||
        current_back_buffer.Get() != frame.back_buffer)
        return FgFailure(ErrorCode::InvalidArgument, "XeSS-FG packet back buffer is not the current selected proxy buffer.");
    impl_->Collect();
    if (impl_->Find(frame.identity))
        return FgFailure(ErrorCode::InvalidState, "XeSS-FG frame identity has already been tagged.");
    auto slot = std::find_if(impl_->pending.begin(), impl_->pending.end(), [](const auto &candidate) { return !candidate.occupied; });
    if (slot == impl_->pending.end())
        return FgFailure(ErrorCode::InvalidState, "XeSS-FG BufferCount source-copy slots are still in use; retire completed copies before tagging.");
    // Take ownership before the FIRST native tag; even a partial failure can
    // leave SDK copy commands referring to these source resources.
    slot->identity = frame.identity;
    slot->occupied = true;
    slot->owners = frame.owners;
    for (std::size_t i = 0; i != resources.size(); ++i) slot->resources[i] = resources[i];
    const auto present_id = static_cast<std::uint32_t>(frame.identity.presentation_id);
    const auto tag = [&](ID3D12Resource *resource, xefg_swapchain_resource_type_t type, const SdkRect &rect) -> Result
    {
        xefg_swapchain_d3d12_resource_data_t data{};
        data.type = type;
        data.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
        data.resourceBase = {rect.x, rect.y};
        data.resourceSize = {rect.width, rect.height};
        data.pResource = resource;
        data.incomingState = D3D12_RESOURCE_STATE_COPY_SOURCE;
        return impl_->Check("xefgSwapChainD3D12TagFrameResource", impl_->tag_resource(impl_->context, commands, present_id, &data));
    };
    if (auto result = tag(frame.hudless, XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, frame.output_rect); !result) return result;
    if (auto result = tag(frame.ui, XEFG_SWAPCHAIN_RES_UI, frame.output_rect); !result) return result;
    if (auto result = tag(frame.depth, XEFG_SWAPCHAIN_RES_DEPTH, frame.render_rect); !result) return result;
    if (auto result = tag(frame.motion, XEFG_SWAPCHAIN_RES_MOTION_VECTOR, frame.render_rect); !result) return result;
    // BACKBUFFER tags carry only letterbox coordinates; the SDK ignores other
    // members and obtains the actual image through its exclusively-owned proxy.
    if (auto result = tag(nullptr, XEFG_SWAPCHAIN_RES_BACKBUFFER, frame.output_rect); !result) return result;
    xefg_swapchain_frame_constant_data_t constants{};
    std::memcpy(constants.viewMatrix, frame.camera.view.m, sizeof(constants.viewMatrix));
    std::memcpy(constants.projectionMatrix, frame.camera.projection.m, sizeof(constants.projectionMatrix));
    constants.jitterOffsetX = frame.camera.jitter_pixels[0];
    constants.jitterOffsetY = frame.camera.jitter_pixels[1];
    constants.motionVectorScaleX = static_cast<float>(frame.output_rect.width);
    constants.motionVectorScaleY = static_cast<float>(frame.output_rect.height);
    constants.resetHistory = frame.camera.reset_history ? 1U : 0U;
    constants.frameRenderTime = frame.camera.delta_seconds * 1000.0f;
    if (auto result = impl_->Check("xefgSwapChainTagFrameConstants", impl_->tag_constants(impl_->context, present_id, &constants)); !result) return result;
    slot->tags_complete = true;
    return Result::Success();
#endif
}

Result XessFrameGeneration::NotifyTagSubmission(const SdkFrameIdentity &identity, ID3D12CommandQueue *queue,
    ID3D12Fence *copy_fence, std::uint64_t copy_fence_value)
{
#if !HS_WITH_XESS_FG
    static_cast<void>(identity); static_cast<void>(queue); static_cast<void>(copy_fence); static_cast<void>(copy_fence_value);
    return FgFailure(ErrorCode::InvalidState, impl_->failure_reason);
#else
    if (auto owner = impl_->RequireOwner(); !owner) return owner;
    auto *frame = impl_->Find(identity);
    if (!frame || frame->submitted || queue != impl_->queue.Get() || !copy_fence || !impl_->SameDevice(copy_fence) ||
        !copy_fence_value || copy_fence_value == std::numeric_limits<std::uint64_t>::max())
        return FgFailure(ErrorCode::InvalidArgument, "XeSS-FG copy completion requires one exact tagging-queue submission/fence for the tagged frame.");
    frame->copy_fence = copy_fence;
    frame->copy_value = copy_fence_value;
    frame->submitted = true;
    impl_->Collect();
    return Result::Success();
#endif
}

Result XessFrameGeneration::SetPresentId(const SdkFrameIdentity &identity)
{
#if !HS_WITH_XESS_FG
    static_cast<void>(identity);
    return FgFailure(ErrorCode::InvalidState, impl_->failure_reason);
#else
    if (auto ready = impl_->RequireReady(); !ready) return ready;
    const auto *frame = impl_->Find(identity);
    if (!impl_->enabled || impl_->present_armed || !ValidIdentity(identity) || !frame ||
        !frame->tags_complete || !frame->submitted || frame->presented)
        return FgFailure(ErrorCode::InvalidState, "XeSS-FG Present requires one fully tagged/submitted frame with an unarmed matching identity.");
    if (auto result = XellLatency::ValidateFrameGeneration(impl_->latency_owner, &identity); !result) return result;
    if (auto result = impl_->Check("xefgSwapChainSetPresentId", impl_->set_present_id(impl_->context,
        static_cast<std::uint32_t>(identity.presentation_id))); !result) return result;
    impl_->present_armed = true;
    impl_->armed_frame = identity;
    return Result::Success();
#endif
}

Result XessFrameGeneration::CompletePresent(const SdkFrameIdentity &identity, XessFrameGenerationPresentStatus &status)
{
#if !HS_WITH_XESS_FG
    static_cast<void>(identity); static_cast<void>(status);
    return FgFailure(ErrorCode::InvalidState, impl_->failure_reason);
#else
    if (auto ready = impl_->RequireReady(); !ready) return ready;
    auto *frame = impl_->Find(identity);
    if (!impl_->present_armed || !SameIdentity(impl_->armed_frame, identity) || !frame)
        return FgFailure(ErrorCode::InvalidState, "XeSS-FG status requires the exact armed Present identity.");
    xefg_swapchain_present_status_t native_status{};
    const auto native = impl_->get_present_status(impl_->context, &native_status);
    const bool available = native >= XEFG_SWAPCHAIN_RESULT_SUCCESS && native != XEFG_SWAPCHAIN_RESULT_WARNING_MISSING_PRESENT_STATUS;
    status = {native_status.framesPresented, static_cast<std::int32_t>(native),
        static_cast<std::int32_t>(native_status.frameGenResult), native_status.isFrameGenEnabled != 0, available};
    impl_->present_armed = false;
    frame->presented = true;
    impl_->Collect();
    if (auto result = impl_->Check("xefgSwapChainGetLastPresentStatus", native); !result) return result;
    if (native == XEFG_SWAPCHAIN_RESULT_WARNING_MISSING_PRESENT_STATUS) return Result::Success();
    if (auto result = impl_->Check("XeSS-FG interpolation", native_status.frameGenResult); !result) return result;
    // Native status can report passthrough after entering exclusive fullscreen.
    impl_->enabled = native_status.isFrameGenEnabled != 0;
    XellLatency::SetFrameGenerationActive(impl_->latency_owner, impl_->enabled);
    return Result::Success();
#endif
}

Result XessFrameGeneration::ResetAfterCompletion(bool gpu_idle_acknowledged)
{
#if HS_WITH_XESS_FG
    if (impl_->context || impl_->module)
    {
        if (auto owner = impl_->RequireOwner(); !owner) return owner;
        if (impl_->context)
        {
            if (!gpu_idle_acknowledged)
                return FgFailure(ErrorCode::InvalidState, "XeSS-FG destruction requires acknowledged application/tagging GPU idle and reset unsubmitted lists.");
            for (const auto &frame : impl_->pending)
            {
                if (!frame.occupied || !frame.submitted) continue;
                const auto completed = frame.copy_fence->GetCompletedValue();
                if (completed == std::numeric_limits<std::uint64_t>::max() || completed < frame.copy_value)
                    return FgFailure(ErrorCode::InvalidState, "XeSS-FG source-copy completion has not been observed.");
            }
            // ONLY_NOW copies have completed; release application source/heap
            // owners and back-buffer refs before asking the SDK to destroy its
            // exclusively-owned proxy and internal presentation resources.
            impl_->pending.clear();
            impl_->swap_chain.Reset();
            if (auto result = impl_->Check("xefgSwapChainDestroy", impl_->destroy_context(impl_->context)); !result) return result;
            impl_->context = nullptr;
            XellLatency::DetachFrameGeneration(impl_->latency_owner);
        }
        if (impl_->module && !FreeLibrary(impl_->module))
            return FgFailure(ErrorCode::TaskFailed, "Cannot unload pinned XeSS-FG runtime (Win32 " + std::to_string(GetLastError()) + ").");
    }
    impl_ = std::make_unique<Impl>();
#else
    static_cast<void>(gpu_idle_acknowledged);
#endif
    return Result::Success();
}

IDXGISwapChain4 *XessFrameGeneration::SwapChain() const noexcept
{
#if HS_WITH_XESS_FG
    return impl_->initialized ? impl_->swap_chain.Get() : nullptr;
#else
    return nullptr;
#endif
}
const XessFrameGenerationProperties &XessFrameGeneration::Properties() const noexcept { return impl_->properties; }
ReconstructionAvailability XessFrameGeneration::Availability() const noexcept { return impl_->availability; }
std::string_view XessFrameGeneration::FailureReason() const noexcept { return impl_->failure_reason; }
bool XessFrameGeneration::Initialized() const noexcept { return impl_->initialized; }
bool XessFrameGeneration::Enabled() const noexcept { return impl_->enabled; }
} // namespace hs
