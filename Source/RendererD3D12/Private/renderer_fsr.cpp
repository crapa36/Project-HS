#include "renderer_fsr.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

#ifndef HS_WITH_FSR
#define HS_WITH_FSR 0
#endif

#if HS_WITH_FSR
#include <Windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <ffx_api.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>
#include <ffx_framegeneration.h>
#include <dx12/ffx_api_framegeneration_dx12.h>
#endif

namespace hs
{
namespace
{
constexpr std::size_t EffectIndex(FsrEffect effect) { return static_cast<std::size_t>(effect); }
Result FsrFailure(ErrorCode code, std::string message)
{
    return Result::Failure(code, "hs_renderer_fsr", std::move(message));
}
constexpr const char *CompiledOutReason = "AMD FSR was explicitly excluded by HS_WITH_FSR=OFF.";
constexpr const char *UninitializedReason = "AMD FSR session has not been initialized.";
const FsrUpscaleRequirements EmptyRequirements;

#if HS_WITH_FSR
using Microsoft::WRL::ComPtr;

bool ValidExtent(ViewExtent extent) { return extent.width && extent.height; }
bool Fits(ID3D12Resource *resource, SdkRect rect)
{
    if (!resource || !rect.width || !rect.height) return false;
    const auto desc = resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize == 1 &&
        desc.SampleDesc.Count == 1 && desc.MipLevels == 1 &&
        static_cast<std::uint64_t>(rect.x) + rect.width <= desc.Width &&
        static_cast<std::uint64_t>(rect.y) + rect.height <= desc.Height;
}
bool ReadState(D3D12_RESOURCE_STATES state)
{
    return state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE ||
        state == (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}
uint32_t NativeState(D3D12_RESOURCE_STATES state)
{
    if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) return FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    if (state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) return FFX_API_RESOURCE_STATE_COMPUTE_READ;
    if (state == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) return FFX_API_RESOURCE_STATE_PIXEL_READ;
    if (ReadState(state)) return FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    if (state == D3D12_RESOURCE_STATE_COPY_SOURCE) return FFX_API_RESOURCE_STATE_COPY_SRC;
    if (state == D3D12_RESOURCE_STATE_COPY_DEST) return FFX_API_RESOURCE_STATE_COPY_DEST;
    if (state == D3D12_RESOURCE_STATE_RENDER_TARGET) return FFX_API_RESOURCE_STATE_RENDER_TARGET;
    return FFX_API_RESOURCE_STATE_COMMON;
}
D3D12_RESOURCE_STATES EngineState(uint32_t state)
{
    if (state == FFX_API_RESOURCE_STATE_UNORDERED_ACCESS) return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (state == FFX_API_RESOURCE_STATE_COMPUTE_READ) return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (state == FFX_API_RESOURCE_STATE_PIXEL_READ) return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (state == FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ)
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (state == FFX_API_RESOURCE_STATE_COPY_SRC) return D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (state == FFX_API_RESOURCE_STATE_COPY_DEST) return D3D12_RESOURCE_STATE_COPY_DEST;
    if (state == FFX_API_RESOURCE_STATE_RENDER_TARGET) return D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (state == FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT) return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    if (state == FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT) return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    if (state == FFX_API_RESOURCE_STATE_GENERIC_READ)
        return D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (state == FFX_API_RESOURCE_STATE_PRESENT || state == FFX_API_RESOURCE_STATE_COMMON)
        return D3D12_RESOURCE_STATE_COMMON;
    return static_cast<D3D12_RESOURCE_STATES>(std::numeric_limits<uint32_t>::max());
}
FfxApiResource NativeResource(ID3D12Resource *resource, D3D12_RESOURCE_STATES state, bool depth = false)
{
    auto native = ffxApiGetResourceDX12(resource, NativeState(state));
    // The renderer's typeless depth allocation has an R32_FLOAT SRV. The SDK
    // resource description must describe that view, not the typeless allocation.
    if (depth && resource) native.description.format = FFX_API_SURFACE_FORMAT_R32_FLOAT;
    return native;
}
bool Quality(std::string_view name, uint32_t &quality)
{
    if (name == "native") quality = FFX_UPSCALE_QUALITY_MODE_NATIVEAA;
    else if (name == "quality") quality = FFX_UPSCALE_QUALITY_MODE_QUALITY;
    else if (name == "balanced") quality = FFX_UPSCALE_QUALITY_MODE_BALANCED;
    else if (name == "performance") quality = FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;
    else if (name == "ultra_performance") quality = FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE;
    else return false;
    return true;
}
bool CameraValid(const SdkCamera &camera)
{
    return camera.reversed_depth && std::isfinite(camera.near_plane) && camera.near_plane > 0 &&
        std::isfinite(camera.far_plane) && camera.far_plane > camera.near_plane &&
        std::isfinite(camera.vertical_fov_radians) && camera.vertical_fov_radians > 0 &&
        camera.vertical_fov_radians < 3.141592654f && std::isfinite(camera.delta_seconds) &&
        camera.delta_seconds >= 0 && std::isfinite(camera.jitter_pixels[0]) &&
        std::isfinite(camera.jitter_pixels[1]);
}
void FsrLog(uint32_t type, const wchar_t *message)
{
    if (message) std::fwprintf(stderr, L"hs.fsr.sdk level=%u message=%ls\n", type, message);
}

// Trust validation precedes execution of any DLL code. The package bootstrap
// also pins the publisher's release digest; this validates its signed publisher.
bool SignedByAmd(const std::filesystem::path &path)
{
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA trust{};
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT | WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result = WinVerifyTrust(nullptr, &policy, &trust);
    bool amd = false;
    if (result == ERROR_SUCCESS)
    {
        const auto *data = WTHelperProvDataFromStateData(trust.hWVTStateData);
        const auto *signer = data ? WTHelperGetProvSignerFromChain(const_cast<CRYPT_PROVIDER_DATA *>(data), 0, FALSE, 0) : nullptr;
        const auto *certificate = signer && signer->csCertChain ? signer->pasCertChain[0].pCert : nullptr;
        if (certificate)
        {
            wchar_t subject[256]{};
            if (CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, subject,
                                   static_cast<DWORD>(std::size(subject))) > 1)
                amd = std::wstring_view(subject) == L"Advanced Micro Devices";
        }
    }
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    static_cast<void>(WinVerifyTrust(nullptr, &policy, &trust));
    return amd;
}

struct NativeApi
{
    HMODULE module{};
    PfnFfxCreateContext create{};
    PfnFfxDestroyContext destroy{};
    PfnFfxQuery query{};
    PfnFfxConfigure configure{};
    PfnFfxDispatch dispatch{};

    Result Load(const std::filesystem::path &path)
    {
        if (module) return Result::Success();
        std::error_code file_error;
        if (!std::filesystem::is_regular_file(path, file_error))
            return FsrFailure(ErrorCode::TaskFailed, "Cannot load missing pinned FSR DLL: " + path.string());
        if (!SignedByAmd(path))
            return FsrFailure(ErrorCode::TaskFailed, "Pinned FSR DLL has no valid AMD Authenticode signature: " + path.string());
        module = LoadLibraryExW(path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) return FsrFailure(ErrorCode::TaskFailed, "Cannot load pinned signed FSR DLL: " + path.string());
        create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(module, "ffxCreateContext"));
        destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(module, "ffxDestroyContext"));
        query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(module, "ffxQuery"));
        configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(module, "ffxConfigure"));
        dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(module, "ffxDispatch"));
        if (!create || !destroy || !query || !configure || !dispatch)
        {
            Unload();
            return FsrFailure(ErrorCode::TaskFailed, "Pinned FSR DLL is missing a required C API export: " + path.string());
        }
        return Result::Success();
    }
    void Unload()
    {
        if (module) FreeLibrary(module);
        *this = {};
    }
};

Result WaitFence(ID3D12Fence *fence, std::uint64_t value)
{
    if (!fence || !value) return FsrFailure(ErrorCode::InvalidState, "FSR Prepare has no submitted graph fence.");
    const auto completed = fence->GetCompletedValue();
    if (completed == std::numeric_limits<std::uint64_t>::max())
        return FsrFailure(ErrorCode::TaskFailed, "Device removal prevents proving FSR graph completion.");
    if (completed >= value) return Result::Success();
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return FsrFailure(ErrorCode::TaskFailed, "Cannot create FSR graph fence wait event.");
    const HRESULT result = fence->SetEventOnCompletion(value, event);
    const DWORD wait = SUCCEEDED(result) ? WaitForSingleObject(event, INFINITE) : WAIT_FAILED;
    CloseHandle(event);
    if (FAILED(result) || wait != WAIT_OBJECT_0)
        return FsrFailure(ErrorCode::TaskFailed, "Cannot complete FSR graph fence wait.");
    return Result::Success();
}
#endif
} // namespace

struct FsrSession::Impl
{
    std::array<ReconstructionAvailability, 3> availability{
        ReconstructionAvailability::Unavailable, ReconstructionAvailability::Unavailable,
        ReconstructionAvailability::Unavailable};
    std::array<std::string, 3> failure;
    std::array<std::string, 3> selected;
    FsrUpscaleRequirements requirements;
#if HS_WITH_FSR
    ComPtr<ID3D12Device> device;
    DWORD owner_thread{};
    std::filesystem::path directory;
    NativeApi upscale_api, generation_api;
    ffxContext generation{}, presentation{};
    std::array<std::vector<FsrVariant>, 3> variants;
    ViewExtent generation_maximum{}, display{};
    std::recursive_mutex api_mutex;
    std::mutex lease_mutex;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGISwapChain4> swapchain;
    DXGI_SWAP_CHAIN_DESC1 swapchain_desc{};
    HWND window{};
    DXGI_COLOR_SPACE_TYPE color_space{};
    FsrCompositionCallback composition{};
    void *composition_context{};
    std::shared_ptr<const void> composition_owner;
    bool enabled{}, drained{true}, prepared{}, has_render_id{};
    std::uint64_t last_render_id{}, prepared_id{};
    std::atomic<std::uint32_t> observed_multiplier{};
    std::atomic<ffxReturnCode_t> callback_result{FFX_API_RETURN_OK};

    // Create descriptors contain pointers that the API may retain until destroy.
    // Keep all linked descriptors (and pointees) in the session, never on stack.
    ffxCreateBackendDX12Desc backend{};
    // Heap-owned descriptors keep retained pNext addresses stable for the
    // entire lifetime of each native view context, including view switches.
    struct UpscaleView
    {
        ffxContext upscale{};
        ViewExtent maximum_render{}, upscale_output{};
        std::uint64_t upscale_id{};
        bool reset_upscale{true};
        FsrUpscaleRequirements requirements;
        ffxCreateContextDescUpscale upscale_desc{};
        ffxCreateContextDescUpscaleVersion upscale_version{};
        ffxOverrideVersion upscale_override{};
    };
    std::uint32_t selected_viewport{};
    std::map<std::uint32_t, std::unique_ptr<UpscaleView>> upscale_views;
    UpscaleView &Upscale()
    {
        auto &view = upscale_views[selected_viewport];
        if (!view) view = std::make_unique<UpscaleView>();
        return *view;
    }
    ffxCreateContextDescFrameGeneration generation_desc{};
    ffxCreateContextDescFrameGenerationVersion generation_version{};
    ffxOverrideVersion generation_override{};
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 presentation_desc{};
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 presentation_version{};
    ffxOverrideVersion presentation_override{};
    IDXGISwapChain4 *created_swapchain{};

    struct Lease
    {
        SdkPresentationFrame frame;
        std::array<ComPtr<ID3D12Resource>, 5> resources;
        ComPtr<ID3D12Fence> graph_fence;
        std::uint64_t graph_value{};
        bool submitted{}, presented{};
    };
    std::vector<std::shared_ptr<Lease>> leases;

    Result RequireOwner() const
    {
        if (owner_thread != GetCurrentThreadId())
            return FsrFailure(ErrorCode::InvalidState, "FSR API called outside the renderer/present owner thread.");
        return Result::Success();
    }
    NativeApi &Api(FsrEffect effect)
    {
        if (effect == FsrEffect::SuperResolution) return upscale_api;
        return generation_api;
    }
    Result Fail(FsrEffect effect, const char *operation, ffxReturnCode_t code)
    {
        failure[EffectIndex(effect)] = std::string(operation) + " failed with native FSR return code " + std::to_string(code) + ".";
        availability[EffectIndex(effect)] = code == FFX_API_RETURN_NO_PROVIDER
            ? ReconstructionAvailability::Unsupported : ReconstructionAvailability::Unavailable;
        return FsrFailure(ErrorCode::TaskFailed, failure[EffectIndex(effect)]);
    }
    Result Load(FsrEffect effect)
    {
        const wchar_t *file = effect == FsrEffect::SuperResolution ? L"amd_fidelityfx_upscaler_dx12.dll" :
            L"amd_fidelityfx_framegeneration_dx12.dll";
        auto result = Api(effect).Load(directory / file);
        if (!result) failure[EffectIndex(effect)] = result.Message();
        return result;
    }
    uint64_t CreateType(FsrEffect effect) const
    {
        switch (effect)
        {
        case FsrEffect::SuperResolution: return FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        case FsrEffect::FrameGeneration: return FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        case FsrEffect::Presentation: return FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
        }
        return 0;
    }
    Result ValidateVariant(FsrEffect effect, uint64_t id)
    {
        if (!id) return Result::Success();
        const auto &available = variants[EffectIndex(effect)];
        if (std::none_of(available.begin(), available.end(), [id](const auto &variant) { return variant.id == id; }))
            return FsrFailure(ErrorCode::InvalidArgument, "FSR version override was not queried for this effect and device.");
        return Result::Success();
    }
    Result Selected(FsrEffect effect, ffxContext &context)
    {
        ffxQueryGetProviderVersion query{};
        query.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        const auto code = Api(effect).query(&context, &query.header);
        if (code != FFX_API_RETURN_OK) return Fail(effect, "ffxQuery(GetProviderVersion)", code);
        if (!query.versionId || !query.versionName)
            return FsrFailure(ErrorCode::TaskFailed, "FSR returned no selected provider identity.");
        selected[EffectIndex(effect)] = query.versionName;
        availability[EffectIndex(effect)] = ReconstructionAvailability::Available;
        failure[EffectIndex(effect)].clear();
        return Result::Success();
    }
    Result Texture(ID3D12Resource *resource, SdkRect rect, DXGI_FORMAT expected,
                   D3D12_RESOURCE_STATES state, bool output, const char *name)
    {
        if (!Fits(resource, rect) || resource->GetDesc().Format != expected ||
            (output ? state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS : !ReadState(state)) ||
            (output && !(resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
            return FsrFailure(ErrorCode::InvalidArgument, std::string("Invalid FSR ") + name + " format, rectangle, flags or incoming state.");
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{expected};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
            !(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) ||
            (output ? !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) : !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD)))
            return FsrFailure(ErrorCode::InvalidArgument, std::string("Device lacks FSR ") + name + " format support.");
        return Result::Success();
    }
    Result Depth(ID3D12Resource *resource, SdkRect rect, D3D12_RESOURCE_STATES state)
    {
        if (!Fits(resource, rect) || !ReadState(state))
            return FsrFailure(ErrorCode::InvalidArgument, "FSR inverted depth has invalid dimensions or incoming state.");
        const auto format = resource->GetDesc().Format;
        if (format != DXGI_FORMAT_R32_FLOAT && format != DXGI_FORMAT_R32_TYPELESS && format != DXGI_FORMAT_D32_FLOAT)
            return FsrFailure(ErrorCode::InvalidArgument, "FSR inverted depth requires a 32-bit float depth SRV.");
        return Result::Success();
    }
    std::shared_ptr<Lease> FindLease(uint64_t frame_id)
    {
        std::lock_guard lock(lease_mutex);
        const auto found = std::find_if(leases.begin(), leases.end(), [frame_id](const auto &lease)
            { return lease->frame.identity.render_id == frame_id; });
        return found == leases.end() ? nullptr : *found;
    }
    static ffxReturnCode_t Generate(ffxDispatchDescFrameGeneration *params, void *context)
    {
        auto &self = *static_cast<Impl *>(context);
        std::lock_guard lock(self.api_mutex);
        const auto lease = params ? self.FindLease(params->frameID) : nullptr;
        if (!params || !lease || !lease->submitted || !self.generation || !self.enabled ||
            !params->numGeneratedFrames || params->numGeneratedFrames > std::size(params->outputs))
        {
            self.callback_result.store(FFX_API_RETURN_ERROR_PARAMETER);
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        params->reset = lease->frame.camera.reset_history;
        params->minMaxLuminance[0] = lease->frame.minimum_luminance;
        params->minMaxLuminance[1] = lease->frame.maximum_luminance;
        const auto result = self.generation_api.dispatch(&self.generation, &params->header);
        if (result != FFX_API_RETURN_OK) self.callback_result.store(result);
        if (result == FFX_API_RETURN_OK) self.observed_multiplier.store(params->numGeneratedFrames + 1);
        return result;
    }
    static ffxReturnCode_t Compose(ffxCallbackDescFrameGenerationPresent *params, void *context)
    {
        auto &self = *static_cast<Impl *>(context);
        const auto lease = params ? self.FindLease(params->frameID) : nullptr;
        if (!params || !lease || !self.composition) return FFX_API_RETURN_ERROR_PARAMETER;
        FsrCompositionCall call;
        call.commands = static_cast<ID3D12GraphicsCommandList *>(params->commandList);
        call.source = static_cast<ID3D12Resource *>(params->currentBackBuffer.resource);
        call.ui = lease->frame.ui;
        call.output = static_cast<ID3D12Resource *>(params->outputSwapChainBuffer.resource);
        call.source_state = EngineState(params->currentBackBuffer.state);
        call.ui_state = lease->frame.ui_state;
        call.output_state = EngineState(params->outputSwapChainBuffer.state);
        if (call.source_state == static_cast<D3D12_RESOURCE_STATES>(std::numeric_limits<uint32_t>::max()) ||
            call.output_state == static_cast<D3D12_RESOURCE_STATES>(std::numeric_limits<uint32_t>::max()))
        {
            self.callback_result.store(FFX_API_RETURN_ERROR_PARAMETER);
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        call.identity = lease->frame.identity;
        call.generated = params->isGeneratedFrame;
        if (!self.composition(call, self.composition_context))
        {
            self.callback_result.store(FFX_API_RETURN_ERROR_RUNTIME_ERROR);
            return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
        }
        return FFX_API_RETURN_OK;
    }
    Result WaitLeases(bool allow_cancelled)
    {
        std::vector<std::shared_ptr<Lease>> pending;
        {
            std::lock_guard lock(lease_mutex);
            pending = leases;
        }
        for (const auto &lease : pending)
        {
            if (!lease->submitted)
            {
                if (allow_cancelled) continue; // Caller has reset these unsubmitted lists.
                return FsrFailure(ErrorCode::InvalidState, "Submit or reset the outstanding FSR Prepare list before draining.");
            }
            if (auto result = WaitFence(lease->graph_fence.Get(), lease->graph_value); !result) return result;
        }
        std::lock_guard lock(lease_mutex);
        leases.clear();
        return Result::Success();
    }
#endif
};

FsrSession::FsrSession() = default;
FsrSession::~FsrSession()
{
    // Completion is a renderer shutdown precondition, never inferred in a
    // destructor. If omitted, retain the native contexts, DLLs and GPU owners.
    if (impl_)
    {
        std::fprintf(stderr, "hs.fsr.shutdown error=ShutdownAfterCompletion was not completed; retaining SDK owners.\n");
        static_cast<void>(impl_.release());
    }
}

Result FsrSession::Initialize(ID3D12Device *device)
{
#if !HS_WITH_FSR
    static_cast<void>(device);
    return FsrFailure(ErrorCode::InvalidState, CompiledOutReason);
#else
    if (impl_) return FsrFailure(ErrorCode::InvalidState, "FSR session is already initialized.");
    if (!device) return FsrFailure(ErrorCode::InvalidArgument, "FSR requires the renderer's native D3D12 device.");
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size())
        return FsrFailure(ErrorCode::TaskFailed, "Cannot resolve the pinned FSR runtime directory.");
    executable.resize(length);
    impl_ = std::make_unique<Impl>();
    impl_->device = device;
    impl_->owner_thread = GetCurrentThreadId();
    impl_->directory = std::filesystem::path(executable).parent_path() / L"fsr";
    impl_->backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    impl_->backend.device = device;
    return Result::Success();
#endif
}

Result FsrSession::QueryVariants(FsrEffect effect, std::vector<FsrVariant> &variants)
{
    variants.clear();
#if !HS_WITH_FSR
    static_cast<void>(effect);
    return FsrFailure(ErrorCode::InvalidState, CompiledOutReason);
#else
    if (!impl_) return FsrFailure(ErrorCode::InvalidState, UninitializedReason);
    if (auto result = impl_->RequireOwner(); !result) return result;
    std::lock_guard lock(impl_->api_mutex);
    if (auto result = impl_->Load(effect); !result) return result;
    uint64_t count{};
    ffxQueryDescGetVersions query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = impl_->CreateType(effect);
    query.device = impl_->device.Get();
    query.outputCount = &count;
    auto code = impl_->Api(effect).query(nullptr, &query.header);
    if (code != FFX_API_RETURN_OK) return impl_->Fail(effect, "ffxQuery(GetVersions count)", code);
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(const char *))
        return FsrFailure(ErrorCode::TaskFailed, "Invalid FSR variant count.");
    std::vector<uint64_t> ids(static_cast<std::size_t>(count));
    std::vector<const char *> names(static_cast<std::size_t>(count));
    const auto capacity = count;
    query.versionIds = ids.data();
    query.versionNames = names.data();
    code = impl_->Api(effect).query(nullptr, &query.header);
    if (code != FFX_API_RETURN_OK) return impl_->Fail(effect, "ffxQuery(GetVersions)", code);
    if (count > capacity) return FsrFailure(ErrorCode::TaskFailed, "FSR version enumeration exceeded its queried capacity.");
    for (std::size_t index = 0; index < count; ++index)
    {
        if (!ids[index] || !names[index]) return FsrFailure(ErrorCode::TaskFailed, "FSR returned an invalid native variant identity.");
        variants.push_back({ids[index], names[index]});
    }
    impl_->variants[EffectIndex(effect)] = variants;
    impl_->availability[EffectIndex(effect)] = variants.empty()
        ? ReconstructionAvailability::Unsupported : ReconstructionAvailability::Available;
    impl_->failure[EffectIndex(effect)] = variants.empty() ? "The signed FSR SDK reports no supported variants for this device." : "";
    return Result::Success();
#endif
}

Result FsrSession::SelectReconstructionViewAfterCompletion(std::uint32_t viewport)
{
#if !HS_WITH_FSR
    static_cast<void>(viewport);
    return Result::Success();
#else
    if (!impl_) return Result::Success();
    if (auto r = impl_->RequireOwner(); !r) return r;
    std::lock_guard lock(impl_->api_mutex);
    impl_->selected_viewport = viewport;
    return Result::Success();
#endif
}

Result FsrSession::ConfigureUpscaling(ViewExtent maximum_render, ViewExtent output, std::uint64_t variant_id)
{
#if !HS_WITH_FSR
    static_cast<void>(maximum_render); static_cast<void>(output); static_cast<void>(variant_id);
    return FsrFailure(ErrorCode::InvalidState, CompiledOutReason);
#else
    if (!impl_) return FsrFailure(ErrorCode::InvalidState, UninitializedReason);
    if (auto result = impl_->RequireOwner(); !result) return result;
    std::lock_guard lock(impl_->api_mutex);
    if (impl_->Upscale().upscale) return FsrFailure(ErrorCode::InvalidState, "Release the completed FSR upscale context before reconfiguration.");
    if (!ValidExtent(maximum_render) || !ValidExtent(output) || maximum_render.width > output.width || maximum_render.height > output.height)
        return FsrFailure(ErrorCode::InvalidArgument, "Invalid FSR maximum render/output extents.");
    if (auto result = impl_->Load(FsrEffect::SuperResolution); !result) return result;
    if (auto result = impl_->ValidateVariant(FsrEffect::SuperResolution, variant_id); !result) return result;
    impl_->Upscale().upscale_desc = {};
    impl_->Upscale().upscale_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    impl_->Upscale().upscale_desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
        FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION | FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    impl_->Upscale().upscale_desc.maxRenderSize = {maximum_render.width, maximum_render.height};
    impl_->Upscale().upscale_desc.maxUpscaleSize = {output.width, output.height};
    impl_->Upscale().upscale_desc.fpMessage = FsrLog;
    impl_->Upscale().upscale_version = {};
    impl_->Upscale().upscale_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    impl_->Upscale().upscale_version.version = FFX_UPSCALER_VERSION;
    impl_->Upscale().upscale_override = {};
    impl_->Upscale().upscale_override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    impl_->Upscale().upscale_override.versionId = variant_id;
    impl_->Upscale().upscale_desc.header.pNext = &impl_->Upscale().upscale_version.header;
    impl_->Upscale().upscale_version.header.pNext = variant_id ? &impl_->Upscale().upscale_override.header : &impl_->backend.header;
    impl_->Upscale().upscale_override.header.pNext = &impl_->backend.header;
    auto code = impl_->upscale_api.create(&impl_->Upscale().upscale, &impl_->Upscale().upscale_desc.header, nullptr);
    if (code != FFX_API_RETURN_OK) return impl_->Fail(FsrEffect::SuperResolution, "ffxCreateContext(Upscale)", code);
    impl_->Upscale().maximum_render = maximum_render;
    impl_->Upscale().upscale_output = output;
    impl_->Upscale().upscale_id = variant_id;
    impl_->Upscale().reset_upscale = true;
    if (auto result = impl_->Selected(FsrEffect::SuperResolution, impl_->Upscale().upscale); !result) return result;
    ffxQueryDescUpscaleGetResourceRequirements requirements{};
    requirements.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_RESOURCE_REQUIREMENTS;
    code = impl_->upscale_api.query(&impl_->Upscale().upscale, &requirements.header);
    if (code != FFX_API_RETURN_OK) return impl_->Fail(FsrEffect::SuperResolution, "ffxQuery(UpscaleGetResourceRequirements)", code);
    const auto required = requirements.required_resources;
    const auto optional = requirements.optional_resources;
    impl_->Upscale().requirements = {
        (required & FFX_API_QUERY_RESOURCE_INPUT_EXPOSURE) != 0,
        (required & FFX_API_QUERY_RESOURCE_INPUT_REACTIVEMASK) != 0,
        (required & FFX_API_QUERY_RESOURCE_INPUT_TRANSPARENCYCOMPOSITION) != 0,
        (optional & FFX_API_QUERY_RESOURCE_INPUT_EXPOSURE) != 0,
        (optional & FFX_API_QUERY_RESOURCE_INPUT_REACTIVEMASK) != 0,
        (optional & FFX_API_QUERY_RESOURCE_INPUT_TRANSPARENCYCOMPOSITION) != 0};
    return Result::Success();
#endif
}

Result FsrSession::QueryInputExtent(const GraphicsModeId &quality, ViewExtent output, ViewExtent &optimal, std::uint64_t variant)
{
    optimal = {};
#if !HS_WITH_FSR
    static_cast<void>(quality); static_cast<void>(output); static_cast<void>(variant);
    return FsrFailure(ErrorCode::InvalidState, CompiledOutReason);
#else
    if (!impl_) return FsrFailure(ErrorCode::InvalidState, UninitializedReason);
    if (auto result = impl_->RequireOwner(); !result) return result;
    std::lock_guard lock(impl_->api_mutex);
    uint32_t mode{};
    if (!Quality(quality.value, mode) || !ValidExtent(output))
        return FsrFailure(ErrorCode::InvalidArgument, "Unknown FSR quality mode or empty output extent.");
    if (auto result = impl_->Load(FsrEffect::SuperResolution); !result) return result;
    if (auto result = impl_->ValidateVariant(FsrEffect::SuperResolution, variant); !result) return result;
    ffxQueryDescUpscaleGetRenderResolutionFromQualityMode query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
    query.displayWidth = output.width;
    query.displayHeight = output.height;
    query.qualityMode = mode;
    query.pOutRenderWidth = &optimal.width;
    query.pOutRenderHeight = &optimal.height;
    ffxOverrideVersion override{};
    override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    override.versionId = variant;
    override.header.pNext = &impl_->backend.header;
    query.header.pNext = variant ? &override.header : &impl_->backend.header;
    const auto code = impl_->upscale_api.query(nullptr, &query.header);
    if (code != FFX_API_RETURN_OK) return impl_->Fail(FsrEffect::SuperResolution, "ffxQuery(GetRenderResolutionFromQualityMode)", code);
    if (!ValidExtent(optimal) || optimal.width > output.width || optimal.height > output.height)
        return FsrFailure(ErrorCode::InvalidArgument, "FSR returned an invalid quality extent.");
    return Result::Success();
#endif
}

Result FsrSession::QueryUpscaleRequirements(FsrUpscaleRequirements &output, std::uint64_t variant)
{
    output = {};
#if !HS_WITH_FSR
    static_cast<void>(variant);
    return FsrFailure(ErrorCode::InvalidState, CompiledOutReason);
#else
    if (!impl_) return FsrFailure(ErrorCode::InvalidState, UninitializedReason);
    if (auto result = impl_->RequireOwner(); !result) return result;
    std::lock_guard lock(impl_->api_mutex);
    if (auto result = impl_->Load(FsrEffect::SuperResolution); !result) return result;
    if (auto result = impl_->ValidateVariant(FsrEffect::SuperResolution, variant); !result) return result;
    ffxOverrideVersion override{};
    override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    override.versionId = variant;
    override.header.pNext = &impl_->backend.header;
    ffxQueryDescUpscaleGetResourceRequirements query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_RESOURCE_REQUIREMENTS;
    query.header.pNext = variant ? &override.header : &impl_->backend.header;
    const auto code = impl_->upscale_api.query(nullptr, &query.header);
    if (code != FFX_API_RETURN_OK) return impl_->Fail(FsrEffect::SuperResolution, "ffxQuery(UpscaleGetResourceRequirements candidate)", code);
    output = {
        (query.required_resources & FFX_API_QUERY_RESOURCE_INPUT_EXPOSURE) != 0,
        (query.required_resources & FFX_API_QUERY_RESOURCE_INPUT_REACTIVEMASK) != 0,
        (query.required_resources & FFX_API_QUERY_RESOURCE_INPUT_TRANSPARENCYCOMPOSITION) != 0,
        (query.optional_resources & FFX_API_QUERY_RESOURCE_INPUT_EXPOSURE) != 0,
        (query.optional_resources & FFX_API_QUERY_RESOURCE_INPUT_REACTIVEMASK) != 0,
        (query.optional_resources & FFX_API_QUERY_RESOURCE_INPUT_TRANSPARENCYCOMPOSITION) != 0};
    return Result::Success();
#endif
}

Result FsrSession::ExecuteSuperResolution(ID3D12GraphicsCommandList *commands, const SdkReconstructionFrame &frame)
{
#if !HS_WITH_FSR
    static_cast<void>(commands); static_cast<void>(frame);
    return FsrFailure(ErrorCode::InvalidState, CompiledOutReason);
#else
    if (!impl_ || !impl_->Upscale().upscale) return FsrFailure(ErrorCode::InvalidState, "FSR upscale context is not configured.");
    if (auto result = impl_->RequireOwner(); !result) return result;
    std::lock_guard lock(impl_->api_mutex);
    // This ABI has no input/output origin fields. Reject nonzero origins rather
    // than silently dispatching a different rectangle from the render graph.
    if (!commands || frame.active.x || frame.active.y || frame.output_rect.x || frame.output_rect.y ||
        !frame.active.width || !frame.active.height || frame.active.width > impl_->Upscale().maximum_render.width ||
        frame.active.height > impl_->Upscale().maximum_render.height || frame.output_rect.width != impl_->Upscale().upscale_output.width ||
        frame.output_rect.height != impl_->Upscale().upscale_output.height || !CameraValid(frame.camera) ||
        !std::isfinite(frame.input_pre_exposure) || frame.input_pre_exposure <= 0)
        return FsrFailure(ErrorCode::InvalidArgument, "FSR requires zero-origin render/output rectangles, matching extents and valid inverted-depth camera/pre-exposure.");
    if (auto result = impl_->Texture(frame.color, frame.active, DXGI_FORMAT_R16G16B16A16_FLOAT, frame.color_state, false, "scene-linear Rec.709 color"); !result) return result;
    if (auto result = impl_->Depth(frame.depth, frame.active, frame.depth_state); !result) return result;
    if (auto result = impl_->Texture(frame.motion, frame.active, DXGI_FORMAT_R16G16_FLOAT, frame.motion_state, false, "UV motion"); !result) return result;
    if (auto result = impl_->Texture(frame.output, frame.output_rect, DXGI_FORMAT_R16G16B16A16_FLOAT, frame.output_state, true, "upscale output"); !result) return result;
    if (frame.output == frame.color || frame.output == frame.depth || frame.output == frame.motion)
        return FsrFailure(ErrorCode::InvalidArgument, "FSR upscale output must not alias its input textures.");
    const auto &required = impl_->Upscale().requirements;
    if ((required.exposure && !frame.exposure) || (required.reactive && !frame.responsive) || (required.composition && !frame.composition))
        return FsrFailure(ErrorCode::InvalidArgument, "A resource required by the selected native FSR variant is absent.");
    if (frame.exposure)
    {
        if (auto result = impl_->Texture(frame.exposure, {0, 0, 1, 1}, DXGI_FORMAT_R32_FLOAT, frame.exposure_state, false, "exposure"); !result) return result;
        const auto desc = frame.exposure->GetDesc();
        if (desc.Width != 1 || desc.Height != 1)
            return FsrFailure(ErrorCode::InvalidArgument, "FSR exposure texture must be exactly 1x1.");
    }
    if (frame.responsive)
        if (auto result = impl_->Texture(frame.responsive, frame.active, DXGI_FORMAT_R8_UNORM, frame.responsive_state, false, "reactive mask"); !result) return result;
    if (frame.composition)
        if (auto result = impl_->Texture(frame.composition, frame.active, DXGI_FORMAT_R8_UNORM, frame.composition_state, false, "composition mask"); !result) return result;
    ffxDispatchDescUpscale dispatch{};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = commands;
    dispatch.color = NativeResource(frame.color, frame.color_state);
    dispatch.depth = NativeResource(frame.depth, frame.depth_state, true);
    dispatch.motionVectors = NativeResource(frame.motion, frame.motion_state);
    dispatch.exposure = NativeResource(frame.exposure, frame.exposure_state);
    dispatch.reactive = NativeResource(frame.responsive, frame.responsive_state);
    dispatch.transparencyAndComposition = NativeResource(frame.composition, frame.composition_state);
    dispatch.output = NativeResource(frame.output, frame.output_state);
    dispatch.jitterOffset = {frame.camera.jitter_pixels[0], frame.camera.jitter_pixels[1]};
    // Render-resolution unjittered UV motion -> render-resolution pixel motion.
    // Do not use output width unless display-resolution MV was enabled at create.
    dispatch.motionVectorScale = {static_cast<float>(frame.active.width), static_cast<float>(frame.active.height)};
    dispatch.renderSize = {frame.active.width, frame.active.height};
    dispatch.upscaleSize = {frame.output_rect.width, frame.output_rect.height};
    dispatch.frameTimeDelta = frame.camera.delta_seconds * 1000.0f;
    dispatch.preExposure = frame.input_pre_exposure;
    dispatch.reset = impl_->Upscale().reset_upscale || frame.camera.reset_history;
    dispatch.cameraNear = frame.camera.near_plane;
    dispatch.cameraFar = frame.camera.far_plane;
    dispatch.cameraFovAngleVertical = frame.camera.vertical_fov_radians;
    dispatch.viewSpaceToMetersFactor = 1.0f;
    // HDR here means scene-linear float input; never select a gamma/PQ flag.
    const auto code = impl_->upscale_api.dispatch(&impl_->Upscale().upscale, &dispatch.header);
    if (code != FFX_API_RETURN_OK) return impl_->Fail(FsrEffect::SuperResolution, "ffxDispatch(Upscale)", code);
    impl_->Upscale().reset_upscale = false;
    return Result::Success();
#endif
}

const FsrUpscaleRequirements &FsrSession::UpscaleRequirements() const noexcept
{
#if HS_WITH_FSR
    if (!impl_) return EmptyRequirements;
    const auto found = impl_->upscale_views.find(impl_->selected_viewport);
    return found != impl_->upscale_views.end() ? found->second->requirements : EmptyRequirements;
#else
    return EmptyRequirements;
#endif
}

Result FsrSession::ReleaseReconstructionAfterCompletion(bool gpu_idle_acknowledged)
{
#if !HS_WITH_FSR
    static_cast<void>(gpu_idle_acknowledged);
    return Result::Success();
#else
    if (!impl_) return Result::Success();
    if (auto result = impl_->RequireOwner(); !result) return result;
    std::lock_guard lock(impl_->api_mutex);
    if (impl_->Upscale().upscale && !gpu_idle_acknowledged)
        return FsrFailure(ErrorCode::InvalidState, "FSR SR destruction requires completed graph work and reset unsubmitted lists.");
    if (impl_->Upscale().upscale)
    {
        const auto code = impl_->upscale_api.destroy(&impl_->Upscale().upscale, nullptr);
        if (code != FFX_API_RETURN_OK) return impl_->Fail(FsrEffect::SuperResolution, "ffxDestroyContext(Upscale)", code);
        impl_->Upscale().upscale = {};
    }
    impl_->Upscale().requirements = {};
    impl_->Upscale().maximum_render = impl_->Upscale().upscale_output = {};
    impl_->selected[EffectIndex(FsrEffect::SuperResolution)].clear();
    return Result::Success();
#endif
}

Result FsrSession::CreatePresentationFromDescription(ID3D12CommandQueue *queue, IDXGIFactory2 *factory,
    HWND window, const DXGI_SWAP_CHAIN_DESC1 &description, DXGI_COLOR_SPACE_TYPE color_space,
    ViewExtent maximum_render, std::uint64_t generation_variant_id, std::uint64_t swapchain_variant_id,
    FsrCompositionCallback composition, void *composition_context, std::shared_ptr<const void> composition_owner)
{
#if !HS_WITH_FSR
    static_cast<void>(queue); static_cast<void>(factory); static_cast<void>(window); static_cast<void>(description);
    static_cast<void>(color_space); static_cast<void>(maximum_render); static_cast<void>(generation_variant_id);
    static_cast<void>(swapchain_variant_id); static_cast<void>(composition); static_cast<void>(composition_context);
    static_cast<void>(composition_owner);
    return FsrFailure(ErrorCode::InvalidState, CompiledOutReason);
#else
    if (!impl_) return FsrFailure(ErrorCode::InvalidState, UninitializedReason);
    if (auto result = impl_->RequireOwner(); !result) return result;
    std::lock_guard lock(impl_->api_mutex);
    if (impl_->generation || impl_->presentation || impl_->swapchain)
        return FsrFailure(ErrorCode::InvalidState, "Destroy the completed FSR presentation before recreating it.");
    if (!queue || !factory || !window || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        !ValidExtent(maximum_render) || !description.Width || !description.Height ||
        description.Width > static_cast<UINT>(std::numeric_limits<int32_t>::max()) ||
        description.Height > static_cast<UINT>(std::numeric_limits<int32_t>::max()) ||
        maximum_render.width > description.Width || maximum_render.height > description.Height ||
        description.SampleDesc.Count != 1 || description.BufferCount < 2 ||
        (color_space != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 &&
         color_space != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 &&
         color_space != DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) ||
        (composition && !composition_owner))
        return FsrFailure(ErrorCode::InvalidArgument, "Invalid FSR presentation queue, HWND, extent, color space or callback owner.");
    if (auto result = impl_->Load(FsrEffect::FrameGeneration); !result) return result;
    if (auto result = impl_->ValidateVariant(FsrEffect::FrameGeneration, generation_variant_id); !result) return result;
    if (auto result = impl_->ValidateVariant(FsrEffect::Presentation, swapchain_variant_id); !result) return result;
    const auto native_format = ffxApiGetSurfaceFormatDX12(description.Format);
    if (native_format == FFX_API_SURFACE_FORMAT_UNKNOWN)
        return FsrFailure(ErrorCode::InvalidArgument, "The FSR SDK cannot represent this backbuffer format.");
    impl_->generation_desc = {};
    impl_->generation_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    impl_->generation_desc.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
    if (color_space != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)
        impl_->generation_desc.flags |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
    // Optical flow/interpolation runs on gameQueue. The SDK's own UI/present
    // queue still paces presentation; it must be drained separately from graph.
    impl_->generation_desc.displaySize = {description.Width, description.Height};
    impl_->generation_desc.maxRenderSize = {maximum_render.width, maximum_render.height};
    impl_->generation_desc.backBufferFormat = native_format;
    impl_->generation_version = {};
    impl_->generation_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    impl_->generation_version.version = FFX_FRAMEGENERATION_VERSION;
    impl_->generation_override = {};
    impl_->generation_override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    impl_->generation_override.versionId = generation_variant_id;
    impl_->generation_desc.header.pNext = &impl_->generation_version.header;
    impl_->generation_version.header.pNext = generation_variant_id ? &impl_->generation_override.header : &impl_->backend.header;
    impl_->generation_override.header.pNext = &impl_->backend.header;
    auto code = impl_->generation_api.create(&impl_->generation, &impl_->generation_desc.header, nullptr);
    if (code != FFX_API_RETURN_OK) return impl_->Fail(FsrEffect::FrameGeneration, "ffxCreateContext(FrameGeneration)", code);
    if (auto result = impl_->Selected(FsrEffect::FrameGeneration, impl_->generation); !result) return result;
    impl_->queue = queue;
    impl_->factory = factory;
    impl_->window = window;
    impl_->swapchain_desc = description;
    impl_->presentation_desc = {};
    impl_->presentation_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    impl_->presentation_desc.swapchain = &impl_->created_swapchain;
    impl_->presentation_desc.hwnd = window;
    impl_->presentation_desc.desc = &impl_->swapchain_desc;
    impl_->presentation_desc.dxgiFactory = factory;
    impl_->presentation_desc.gameQueue = queue;
    impl_->presentation_version = {};
    impl_->presentation_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    impl_->presentation_version.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
    impl_->presentation_override = {};
    impl_->presentation_override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    impl_->presentation_override.versionId = swapchain_variant_id;
    impl_->presentation_desc.header.pNext = &impl_->presentation_version.header;
    impl_->presentation_version.header.pNext = swapchain_variant_id ? &impl_->presentation_override.header : &impl_->backend.header;
    impl_->presentation_override.header.pNext = &impl_->backend.header;
    code = impl_->generation_api.create(&impl_->presentation, &impl_->presentation_desc.header, nullptr);
    // Preserve an output reference even on native failure, for explicit cleanup.
    if (impl_->created_swapchain) impl_->swapchain.Attach(impl_->created_swapchain);
    if (code != FFX_API_RETURN_OK) return impl_->Fail(FsrEffect::Presentation, "ffxCreateContext(FrameGenerationSwapChainForHwnd)", code);
    if (!impl_->swapchain) return FsrFailure(ErrorCode::TaskFailed, "FSR swapchain creation returned no proxy.");
    if (auto result = impl_->Selected(FsrEffect::Presentation, impl_->presentation); !result) return result;
    const HRESULT color_result = impl_->swapchain->SetColorSpace1(color_space);
    if (FAILED(color_result))
        return FsrFailure(ErrorCode::TaskFailed, "FSR proxy SetColorSpace1 failed with HRESULT " + std::to_string(color_result) + ".");
    impl_->display = {description.Width, description.Height};
    impl_->generation_maximum = maximum_render;
    impl_->color_space = color_space;
    impl_->composition = composition;
    impl_->composition_context = composition_context;
    impl_->composition_owner = std::move(composition_owner);
    impl_->enabled = false;
    impl_->drained = true;
    impl_->callback_result.store(FFX_API_RETURN_OK);
    impl_->observed_multiplier.store(0);
    return Result::Success();
#endif
}

Result FsrSession::SetFrameGenerationEnabled(bool enabled)
{
#if !HS_WITH_FSR
    static_cast<void>(enabled);
    return FsrFailure(ErrorCode::InvalidState, CompiledOutReason);
#else
    if (!impl_ || !impl_->generation || !impl_->presentation)
        return FsrFailure(ErrorCode::InvalidState, "FSR presentation is not created.");
    if (auto result = impl_->RequireOwner(); !result) return result;
    std::lock_guard lock(impl_->api_mutex);
    if (enabled == impl_->enabled) return Result::Success();
    if (!enabled) return DrainPresentation();
    if (impl_->prepared) return FsrFailure(ErrorCode::InvalidState, "Cannot change FSR mode between Prepare and Present.");
    impl_->enabled = true;
    return Result::Success();
#endif
}

Result FsrSession::PreparePresent(ID3D12GraphicsCommandList *commands, const SdkPresentationFrame &frame)
{
#if !HS_WITH_FSR
    static_cast<void>(commands); static_cast<void>(frame);
    return FsrFailure(ErrorCode::InvalidState, CompiledOutReason);
#else
    if (!impl_ || !impl_->generation || !impl_->presentation || !impl_->swapchain)
        return FsrFailure(ErrorCode::InvalidState, "FSR presentation is not created.");
    if (auto result = impl_->RequireOwner(); !result) return result;
    std::lock_guard lock(impl_->api_mutex);
    if (impl_->prepared) return FsrFailure(ErrorCode::InvalidState, "Present the prepared FSR render frame exactly once before preparing another.");
    if (!commands || frame.owners.empty() || frame.render_rect.x || frame.render_rect.y ||
        !frame.render_rect.width || !frame.render_rect.height || frame.render_rect.width > impl_->generation_maximum.width ||
        frame.render_rect.height > impl_->generation_maximum.height || frame.output_rect.x || frame.output_rect.y ||
        frame.output_rect.width != impl_->display.width || frame.output_rect.height != impl_->display.height ||
        frame.output_format != impl_->swapchain_desc.Format || frame.color_space != impl_->color_space ||
        !CameraValid(frame.camera) || !std::isfinite(frame.minimum_luminance) || frame.minimum_luminance < 0 ||
        !std::isfinite(frame.maximum_luminance) || frame.maximum_luminance <= frame.minimum_luminance ||
        (impl_->has_render_id && frame.identity.render_id <= impl_->last_render_id))
        return FsrFailure(ErrorCode::InvalidArgument, "FSR requires unique ascending real render IDs, converted presentation resources, owners and valid camera/luminance/extents.");
    if (auto result = impl_->Depth(frame.depth, frame.render_rect, frame.depth_state); !result) return result;
    if (auto result = impl_->Texture(frame.motion, frame.render_rect, DXGI_FORMAT_R16G16_FLOAT, frame.motion_state, false, "FG UV motion"); !result) return result;
    if (!Fits(frame.back_buffer, frame.output_rect) || frame.back_buffer->GetDesc().Format != frame.output_format ||
        frame.back_buffer_state != D3D12_RESOURCE_STATE_PRESENT)
        return FsrFailure(ErrorCode::InvalidArgument, "FSR backbuffer must be the current full-size proxy buffer in PRESENT state.");
    ComPtr<ID3D12Resource> current_buffer;
    if (FAILED(impl_->swapchain->GetBuffer(impl_->swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&current_buffer))) ||
        current_buffer.Get() != frame.back_buffer)
        return FsrFailure(ErrorCode::InvalidArgument, "FSR packet does not reference the current SDK proxy backbuffer.");
    if (auto result = impl_->Texture(frame.hudless, frame.output_rect, frame.output_format, frame.hudless_state, false, "display-encoded HUD-less image"); !result) return result;
    if (auto result = impl_->Texture(frame.ui, frame.output_rect, frame.output_format, frame.ui_state, false, "display-encoded premultiplied UI"); !result) return result;
    auto lease = std::make_shared<Impl::Lease>();
    lease->frame = frame;
    lease->resources[0] = frame.hudless;
    lease->resources[1] = frame.ui;
    lease->resources[2] = frame.depth;
    lease->resources[3] = frame.motion;
    lease->resources[4] = frame.back_buffer;
    {
        std::lock_guard lease_lock(impl_->lease_mutex);
        impl_->leases.push_back(lease);
    }
    // Separate UI image mode: the renderer has copied the HUD-less image into
    // the real backbuffer. The SDK compositor (or custom callback) adds UI once.
    ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 ui{};
    ui.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
    if (!impl_->composition) ui.uiResource = NativeResource(frame.ui, frame.ui_state);
    ui.flags = FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA;
    auto code = impl_->generation_api.configure(&impl_->presentation, &ui.header);
    if (code != FFX_API_RETURN_OK) return impl_->Fail(FsrEffect::Presentation, "ffxConfigure(RegisterUiResource)", code);
    ffxConfigureDescFrameGeneration configuration{};
    configuration.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    configuration.swapChain = impl_->swapchain.Get();
    configuration.frameGenerationEnabled = impl_->enabled;
    configuration.frameGenerationCallback = impl_->enabled ? &Impl::Generate : nullptr;
    configuration.frameGenerationCallbackUserContext = impl_.get();
    configuration.presentCallback = impl_->composition ? &Impl::Compose : nullptr;
    configuration.presentCallbackUserContext = impl_.get();
    configuration.generationRect = {0, 0, static_cast<int32_t>(frame.output_rect.width), static_cast<int32_t>(frame.output_rect.height)};
    configuration.frameID = frame.identity.render_id;
    code = impl_->generation_api.configure(&impl_->generation, &configuration.header);
    if (code != FFX_API_RETURN_OK) return impl_->Fail(FsrEffect::FrameGeneration, "ffxConfigure(FrameGeneration)", code);
    const bool reset_generation = impl_->drained;
    impl_->drained = false;
    if (impl_->enabled)
    {
        ffxDispatchDescFrameGenerationPrepareV2 prepare{};
        prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        prepare.commandList = commands;
        prepare.frameID = frame.identity.render_id;
        prepare.renderSize = {frame.render_rect.width, frame.render_rect.height};
        prepare.jitterOffset = {frame.camera.jitter_pixels[0], frame.camera.jitter_pixels[1]};
        prepare.motionVectorScale = {static_cast<float>(frame.render_rect.width), static_cast<float>(frame.render_rect.height)};
        prepare.frameTimeDelta = frame.camera.delta_seconds * 1000.0f;
        prepare.reset = frame.camera.reset_history || !impl_->has_render_id ||
            frame.identity.render_id != impl_->last_render_id + 1 || reset_generation;
        prepare.cameraNear = frame.camera.near_plane;
        prepare.cameraFar = frame.camera.far_plane;
        prepare.cameraFovAngleVertical = frame.camera.vertical_fov_radians;
        prepare.viewSpaceToMetersFactor = 1.0f;
        prepare.depth = NativeResource(frame.depth, frame.depth_state, true);
        prepare.motionVectors = NativeResource(frame.motion, frame.motion_state);
        std::memcpy(prepare.cameraPosition, &frame.camera.position, sizeof(prepare.cameraPosition));
        std::memcpy(prepare.cameraUp, &frame.camera.up, sizeof(prepare.cameraUp));
        std::memcpy(prepare.cameraRight, &frame.camera.right, sizeof(prepare.cameraRight));
        std::memcpy(prepare.cameraForward, &frame.camera.forward, sizeof(prepare.cameraForward));
        code = impl_->generation_api.dispatch(&impl_->generation, &prepare.header);
        if (code != FFX_API_RETURN_OK) return impl_->Fail(FsrEffect::FrameGeneration, "ffxDispatch(FrameGenerationPrepareV2)", code);
    }
    impl_->prepared_id = frame.identity.render_id;
    impl_->prepared = true;
    impl_->last_render_id = frame.identity.render_id;
    impl_->has_render_id = true;
    return Result::Success();
#endif
}

Result FsrSession::NotifyPrepareSubmission(const SdkFrameIdentity &identity, ID3D12CommandQueue *queue,
    ID3D12Fence *fence, std::uint64_t value)
{
#if !HS_WITH_FSR
    static_cast<void>(identity); static_cast<void>(queue); static_cast<void>(fence); static_cast<void>(value);
    return FsrFailure(ErrorCode::InvalidState, CompiledOutReason);
#else
    if (!impl_) return FsrFailure(ErrorCode::InvalidState, UninitializedReason);
    if (auto result = impl_->RequireOwner(); !result) return result;
    std::lock_guard lock(impl_->api_mutex);
    const auto lease = impl_->FindLease(identity.render_id);
    if (!lease || !impl_->prepared || impl_->prepared_id != identity.render_id || lease->submitted ||
        queue != impl_->queue.Get() || !fence || !value ||
        lease->frame.identity.presentation_id != identity.presentation_id || lease->frame.identity.viewport_id != identity.viewport_id)
        return FsrFailure(ErrorCode::InvalidArgument, "FSR Prepare submission does not match the prepared frame, queue or graph fence.");
    lease->graph_fence = fence;
    lease->graph_value = value;
    lease->submitted = true;
    return Result::Success();
#endif
}

Result FsrSession::Present(std::uint32_t sync_interval, std::uint32_t flags)
{
#if !HS_WITH_FSR
    static_cast<void>(sync_interval); static_cast<void>(flags);
    return FsrFailure(ErrorCode::InvalidState, CompiledOutReason);
#else
    if (!impl_ || !impl_->swapchain) return FsrFailure(ErrorCode::InvalidState, "FSR presentation is not created.");
    if (auto result = impl_->RequireOwner(); !result) return result;
    std::lock_guard lock(impl_->api_mutex);
    const auto lease = impl_->FindLease(impl_->prepared_id);
    if (!impl_->prepared || !lease || !lease->submitted || lease->presented)
        return FsrFailure(ErrorCode::InvalidState, "FSR Present requires a matching submitted Prepare exactly once.");
    const HRESULT result = impl_->swapchain->Present(sync_interval, flags);
    lease->presented = true;
    impl_->prepared = false;
    // Composition may still be pending on the SDK thread. Preserve owners and
    // report its result on the next SDK drain as well as any immediate failure.
    if (FAILED(result))
        return FsrFailure(ErrorCode::TaskFailed, "FSR proxy Present failed with HRESULT " + std::to_string(result) + ".");
    const auto callback_result = impl_->callback_result.load();
    if (callback_result != FFX_API_RETURN_OK)
        return impl_->Fail(FsrEffect::FrameGeneration, "FSR presentation callback", callback_result);
    return Result::Success();
#endif
}

Result FsrSession::WaitForPresentationInputs()
{
#if !HS_WITH_FSR
    return Result::Success();
#else
    if (!impl_ || !impl_->presentation) return Result::Success();
    if (auto result = impl_->RequireOwner(); !result) return result;
    std::lock_guard lock(impl_->api_mutex);
    if (impl_->prepared)
        return FsrFailure(ErrorCode::InvalidState, "Present the submitted FSR frame before retiring its SDK leases.");
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait{};
    wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
    const auto code = impl_->generation_api.dispatch(&impl_->presentation, &wait.header);
    if (code != FFX_API_RETURN_OK) return impl_->Fail(FsrEffect::Presentation, "ffxDispatch(SwapChainWaitForPresents)", code);
    if (auto result = impl_->WaitLeases(false); !result) return result;
    const auto callback_result = impl_->callback_result.load();
    if (callback_result != FFX_API_RETURN_OK)
        return impl_->Fail(FsrEffect::FrameGeneration, "FSR asynchronous presentation callback", callback_result);
    return Result::Success();
#endif
}

Result FsrSession::DrainPresentation(bool completedPrepareAbandoned)
{
#if !HS_WITH_FSR
    static_cast<void>(completedPrepareAbandoned);
    return Result::Success();
#else
    if (!impl_ || !impl_->presentation || !impl_->generation) return Result::Success();
    if (auto result = impl_->RequireOwner(); !result) return result;
    std::lock_guard lock(impl_->api_mutex);
    if (impl_->prepared && !completedPrepareAbandoned)
        return FsrFailure(ErrorCode::InvalidState, "Complete the prepared FSR frame before draining presentation.");
    if (completedPrepareAbandoned) impl_->prepared = false;
    ffxConfigureDescFrameGeneration disabled{};
    disabled.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    disabled.swapChain = impl_->swapchain.Get();
    // Disable generation first. Null callbacks/HUDLessColor make this the SDK's
    // documented waitForPresents GPU flush boundary before UI/context teardown.
    disabled.frameID = impl_->last_render_id;
    auto code = impl_->generation_api.configure(&impl_->generation, &disabled.header);
    if (code != FFX_API_RETURN_OK) return impl_->Fail(FsrEffect::FrameGeneration, "ffxConfigure(DisableGenerationAndUiAndFlush)", code);
    impl_->enabled = false;
    ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 ui{};
    ui.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
    code = impl_->generation_api.configure(&impl_->presentation, &ui.header);
    if (code != FFX_API_RETURN_OK) return impl_->Fail(FsrEffect::Presentation, "ffxConfigure(UnregisterUiResource)", code);
    if (auto result = impl_->WaitLeases(completedPrepareAbandoned); !result) return result;
    impl_->drained = true;
    const auto callback_result = impl_->callback_result.load();
    if (callback_result != FFX_API_RETURN_OK)
        return impl_->Fail(FsrEffect::FrameGeneration, "FSR asynchronous presentation callback", callback_result);
    return Result::Success();
#endif
}

Result FsrSession::DestroyPresentationAfterCompletion(bool gpu_idle_acknowledged)
{
#if !HS_WITH_FSR
    static_cast<void>(gpu_idle_acknowledged);
    return Result::Success();
#else
    if (!impl_) return Result::Success();
    if (auto result = impl_->RequireOwner(); !result) return result;
    std::lock_guard lock(impl_->api_mutex);
    if ((impl_->generation || impl_->presentation || impl_->swapchain) && !gpu_idle_acknowledged)
        return FsrFailure(ErrorCode::InvalidState, "FSR presentation destruction requires completed graph work, reset unsubmitted lists and released external proxy references.");
    // Explicit completion acknowledgement also certifies any unsubmitted Prepare
    // list was reset; remove its CPU expectation before the SDK disable/flush.
    impl_->prepared = false;
    if (impl_->generation && impl_->presentation)
    {
        ffxConfigureDescFrameGeneration disabled{};
        disabled.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        disabled.swapChain = impl_->swapchain.Get();
        // Disable generation first. Null callbacks/HUDLessColor make this the
        // documented waitForPresents GPU flush boundary before UI teardown.
        disabled.frameID = impl_->last_render_id;
        auto code = impl_->generation_api.configure(&impl_->generation, &disabled.header);
        if (code != FFX_API_RETURN_OK) return impl_->Fail(FsrEffect::FrameGeneration, "ffxConfigure(DisableGenerationAndUiAndFlush)", code);
        impl_->enabled = false;
        ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 ui{};
        ui.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
        code = impl_->generation_api.configure(&impl_->presentation, &ui.header);
        if (code != FFX_API_RETURN_OK) return impl_->Fail(FsrEffect::Presentation, "ffxConfigure(UnregisterUiResource)", code);
    }
    if (auto result = impl_->WaitLeases(true); !result) return result;
    if (impl_->generation)
    {
        const auto code = impl_->generation_api.destroy(&impl_->generation, nullptr);
        if (code != FFX_API_RETURN_OK) return impl_->Fail(FsrEffect::FrameGeneration, "ffxDestroyContext(FrameGeneration)", code);
        impl_->generation = {};
    }
    if (impl_->presentation)
    {
        const auto code = impl_->generation_api.destroy(&impl_->presentation, nullptr);
        if (code != FFX_API_RETURN_OK) return impl_->Fail(FsrEffect::Presentation, "ffxDestroyContext(FrameGenerationSwapChain)", code);
        impl_->presentation = {};
    }
    // SDK context destruction precedes the final proxy COM release; that release
    // runs the swapchain's worker-thread/GPU teardown while its DLL is still live.
    impl_->swapchain.Reset();
    impl_->created_swapchain = nullptr;
    impl_->queue.Reset();
    impl_->factory.Reset();
    impl_->composition = nullptr;
    impl_->composition_context = nullptr;
    impl_->composition_owner.reset();
    impl_->enabled = impl_->prepared = impl_->has_render_id = false;
    impl_->drained = true;
    impl_->selected[EffectIndex(FsrEffect::FrameGeneration)].clear();
    impl_->selected[EffectIndex(FsrEffect::Presentation)].clear();
    return Result::Success();
#endif
}

Result FsrSession::ShutdownAfterCompletion(bool gpu_idle_acknowledged)
{
#if !HS_WITH_FSR
    static_cast<void>(gpu_idle_acknowledged);
    return Result::Success();
#else
    if (!impl_) return Result::Success();
    if (auto result = impl_->RequireOwner(); !result) return result;
    if (auto result = DestroyPresentationAfterCompletion(gpu_idle_acknowledged); !result) return result;
    std::vector<std::uint32_t> viewports;
    for (const auto &[viewport, _] : impl_->upscale_views) viewports.push_back(viewport);
    for (const auto viewport : viewports)
    {
        impl_->selected_viewport = viewport;
        if (auto result = ReleaseReconstructionAfterCompletion(gpu_idle_acknowledged); !result) return result;
    }
    impl_->selected_viewport = 0;
    if (auto result = ReleaseReconstructionAfterCompletion(gpu_idle_acknowledged); !result) return result;
    impl_->upscale_views.clear();
    impl_->upscale_api.Unload();
    impl_->generation_api.Unload();
    impl_.reset();
    return Result::Success();
#endif
}

IDXGISwapChain4 *FsrSession::SwapChain() const noexcept
{
#if HS_WITH_FSR
    return impl_ ? impl_->swapchain.Get() : nullptr;
#else
    return nullptr;
#endif
}

ReconstructionAvailability FsrSession::Availability(FsrEffect effect) const noexcept
{
#if HS_WITH_FSR
    return impl_ ? impl_->availability[EffectIndex(effect)] : ReconstructionAvailability::Unavailable;
#else
    static_cast<void>(effect);
    return ReconstructionAvailability::CompiledOut;
#endif
}

std::string_view FsrSession::FailureReason(FsrEffect effect) const noexcept
{
#if HS_WITH_FSR
    return impl_ ? std::string_view(impl_->failure[EffectIndex(effect)]) : UninitializedReason;
#else
    static_cast<void>(effect);
    return CompiledOutReason;
#endif
}

std::string_view FsrSession::SelectedVariantName(FsrEffect effect) const noexcept
{
    return impl_ ? std::string_view(impl_->selected[EffectIndex(effect)]) : std::string_view{};
}

std::uint32_t FsrSession::ObservedFrameMultiplier() const noexcept
{
#if HS_WITH_FSR
    return impl_ ? impl_->observed_multiplier.load() : 0;
#else
    return 0;
#endif
}

bool FsrSession::Initialized() const noexcept { return impl_ != nullptr; }
bool FsrSession::FrameGenerationEnabled() const noexcept
{
#if HS_WITH_FSR
    return impl_ && impl_->enabled;
#else
    return false;
#endif
}
} // namespace hs
