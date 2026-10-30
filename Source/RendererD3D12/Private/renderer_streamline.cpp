#include "renderer_streamline.hpp"
#include "renderer_diagnostics.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <map>
#include <string>
#include <utility>

#ifndef HS_WITH_STREAMLINE
#define HS_WITH_STREAMLINE 0
#endif

#if HS_WITH_STREAMLINE
#include <Windows.h>
#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_security.h>
static_assert(SL_VERSION_MAJOR == 2 && SL_VERSION_MINOR == 14 && SL_VERSION_PATCH == 1,
              "Project HS requires the frozen Streamline 2.14.1 ABI.");
#endif

namespace hs
{
namespace
{
constexpr std::string_view kExcluded = "Streamline 2.14.1 was excluded by HS_WITH_STREAMLINE=OFF.";
constexpr std::string_view kNotStarted = "Streamline session has not been requested or initialized.";
Result StreamlineFailure(ErrorCode code, std::string message)
{
    return Result::Failure(code, "hs_renderer_streamline", std::move(message));
}
#if HS_WITH_STREAMLINE
using Microsoft::WRL::ComPtr;
constexpr std::array<sl::Feature, 3> kNativeFeatures{
    sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex};
constexpr std::size_t Index(StreamlineFeature feature) { return static_cast<std::size_t>(feature); }
std::atomic<long> gStreamlinePresentError{};

void StreamlineLog(sl::LogType type, const char *message)
{
#if defined(HS_DEVELOPMENT_TOOLS)
    if (message) std::fprintf(stderr, "hs.streamline.sdk type=%u message=%s\n", static_cast<unsigned>(type), message);
#else
    static_cast<void>(type);
    static_cast<void>(message);
#endif
}
void StreamlineAPIError(const sl::APIError &error)
{
    gStreamlinePresentError.store(error.hres);
}
std::string NativeFailure(const char *operation, sl::Result result)
{
    const bool missing = result == sl::Result::eErrorFeatureMissing || result == sl::Result::eErrorFeatureFailedToLoad ||
        result == sl::Result::eErrorFeatureMissingDependency;
    return std::string(operation) + " failed with Streamline result " + std::to_string(static_cast<int>(result)) +
        (missing ? " (missing provider/plugin)." : ".");
}
bool Unsupported(sl::Result result)
{
    return result == sl::Result::eErrorDriverOutOfDate || result == sl::Result::eErrorOSOutOfDate ||
        result == sl::Result::eErrorOSDisabledHWS || result == sl::Result::eErrorNoSupportedAdapterFound ||
        result == sl::Result::eErrorAdapterNotSupported || result == sl::Result::eErrorFeatureNotSupported;
}
Result CandidateFailure(const char *operation, sl::Result native)
{
    // Planning/configuration failure belongs to this candidate, not the live
    // feature capability. The previous viewport must remain restorable after
    // drain; execution and resource-release failures still use Impl::Fail.
    return StreamlineFailure(Unsupported(native) || native == sl::Result::eErrorInvalidParameter
        ? ErrorCode::InvalidArgument : ErrorCode::TaskFailed, NativeFailure(operation, native));
}
bool Quality(std::string_view mode, sl::DLSSMode &native)
{
    if (mode == "native") native = sl::DLSSMode::eDLAA;
    else if (mode == "quality") native = sl::DLSSMode::eMaxQuality;
    else if (mode == "balanced") native = sl::DLSSMode::eBalanced;
    else if (mode == "performance") native = sl::DLSSMode::eMaxPerformance;
    else if (mode == "ultra_performance") native = sl::DLSSMode::eUltraPerformance;
    else if (mode == "ultra_quality") native = sl::DLSSMode::eUltraQuality;
    else return false;
    return true;
}
sl::float4x4 NativeMatrix(const DirectX::XMFLOAT4X4 &matrix)
{
    // Both APIs use row-major storage and row-vector multiplication. Copy the
    // actual rows explicitly; no ABI cast or shader-upload transposition.
    sl::float4x4 native;
    for (std::uint32_t row = 0; row != 4; ++row)
        native[row] = sl::float4(matrix.m[row][0], matrix.m[row][1], matrix.m[row][2], matrix.m[row][3]);
    return native;
}
sl::float4x4 NativeMatrix(DirectX::FXMMATRIX matrix)
{
    DirectX::XMFLOAT4X4 stored;
    DirectX::XMStoreFloat4x4(&stored, matrix);
    return NativeMatrix(stored);
}
bool FiniteMatrix(const DirectX::XMFLOAT4X4 &matrix)
{
    for (const auto &row : matrix.m)
        for (const auto element : row) if (!std::isfinite(element)) return false;
    return true;
}
bool FitsRect(ID3D12Resource *resource, SdkRect rectangle)
{
    if (!resource || !rectangle.width || !rectangle.height) return false;
    const auto desc = resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize == 1 &&
        desc.SampleDesc.Count == 1 && static_cast<std::uint64_t>(rectangle.x) + rectangle.width <= desc.Width &&
        static_cast<std::uint64_t>(rectangle.y) + rectangle.height <= desc.Height;
}
bool Format(ID3D12Resource *resource, std::initializer_list<DXGI_FORMAT> formats)
{
    return resource && std::find(formats.begin(), formats.end(), resource->GetDesc().Format) != formats.end();
}
bool FloatColor(ID3D12Resource *resource)
{
    return Format(resource, {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT});
}
bool MotionFormat(ID3D12Resource *resource)
{
    return Format(resource, {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32G32_FLOAT});
}
bool DepthFormat(ID3D12Resource *resource)
{
    return Format(resource, {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT});
}
bool ShaderReadable(D3D12_RESOURCE_STATES state)
{
    return (state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
}
sl::Extent NativeRect(SdkRect rectangle)
{
    return {rectangle.y, rectangle.x, rectangle.width, rectangle.height};
}
sl::Resource NativeResource(ID3D12Resource *resource, D3D12_RESOURCE_STATES state)
{
    sl::Resource native(sl::ResourceType::eTex2d, resource, static_cast<std::uint32_t>(state));
    if (resource)
    {
        const auto desc = resource->GetDesc();
        native.width = static_cast<std::uint32_t>(desc.Width);
        native.height = desc.Height;
        native.nativeFormat = static_cast<std::uint32_t>(desc.Format);
        native.mipLevels = desc.MipLevels;
        native.arrayLayers = desc.DepthOrArraySize;
    }
    return native;
}
#endif
} // namespace

struct StreamlineSession::Impl
{
#if HS_WITH_STREAMLINE
    struct FeatureState
    {
        ReconstructionAvailability availability{ReconstructionAvailability::Unavailable};
        std::string reason{"Streamline feature was not requested."};
        bool requested{};
    };
    std::array<FeatureState, 3> feature;
    HMODULE module{};
    DWORD owner_thread{};
    bool started{}, reconstruction_configured{}, reconstruction_evaluated{}, force_reset{true};
    bool sr_viewport_owned{};
    bool reflex_enabled{}, presentation_prepared{}, presentation_recreate_required{}, presented_without_fence{};
    bool has_frame{};
    SdkFrameIdentity identity;
    sl::FrameToken *frame_token{};
    sl::FrameToken *package_frame_token{};
    std::uint64_t package_render_id{};
    bool has_package_token{};
    std::uint64_t constants_frame{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t evaluated_frame{std::numeric_limits<std::uint64_t>::max()};
    std::uint32_t viewport{};
    GraphicsModeId quality;
    ViewExtent output, fg_input;
    StreamlineInputExtent input_range;
    ComPtr<ID3D12Device> device;
    ComPtr<IDXGISwapChain4> swapchain;
    ComPtr<ID3D12CommandQueue> presenting_queue;
    std::wstring plugin_directory;
    sl::DLSSOptions sr_options;
    struct ReconstructionView
    {
        bool reconstruction_configured{};
        bool reconstruction_evaluated{};
        bool sr_viewport_owned{};
        bool force_reset{true};
        bool has_frame{};
        SdkFrameIdentity identity{};
        sl::FrameToken * frame_token{};
        std::uint64_t constants_frame{std::numeric_limits<std::uint64_t>::max()};
        std::uint64_t evaluated_frame{std::numeric_limits<std::uint64_t>::max()};
        GraphicsModeId quality;
        ViewExtent output;
        StreamlineInputExtent input_range;
        sl::DLSSOptions sr_options;
    };
    std::map<std::uint32_t, ReconstructionView> reconstruction_views;
    sl::DLSSGOptions fg_options;
    StreamlineFrameGenerationCapabilities fg_capabilities;
    std::uint64_t fg_last_state_query_qpc{};
    ComPtr<ID3D12Fence> presentation_fence;
    std::uint64_t presentation_fence_value{};
    struct PresentationLease
    {
        SdkFrameIdentity identity;
        std::vector<std::shared_ptr<const void>> owners;
        std::array<ComPtr<ID3D12Resource>, 5> resources;
        ComPtr<ID3D12Fence> fence;
        std::uint64_t fence_value{};
    };
    PresentationLease pending;
    std::deque<PresentationLease> retired;

    PFun_slInit *initialize{};
    PFun_slShutdown *shutdown{};
    PFun_slSetD3DDevice *set_device{};
    PFun_slGetFeatureRequirements *get_requirements{};
    PFun_slIsFeatureSupported *is_supported{};
    PFun_slIsFeatureLoaded *is_loaded{};
    PFun_slGetFeatureFunction *get_function{};
    PFun_slGetNewFrameToken *get_frame_token{};
    PFun_slSetConstants *set_constants{};
    PFun_slSetTagForFrame *set_tags{};
    PFun_slEvaluateFeature *evaluate{};
    PFun_slFreeResources *free_resources{};
    PFun_slUpgradeInterface *upgrade{};
    PFun_slGetNativeInterface *get_native{};
    PFun_slDLSSGetOptimalSettings *sr_optimal{};
    PFun_slDLSSSetOptions *sr_set_options{};
    PFun_slDLSSGGetState *fg_get_state{};
    PFun_slDLSSGSetOptions *fg_set_options{};
    PFun_slReflexGetState *reflex_get_state{};
    PFun_slReflexSetOptions *reflex_set_options{};
    PFun_slReflexSleep *reflex_sleep{};
    PFun_slPCLSetMarker *pcl_marker{};
    PFun_slPCLSetOptions *pcl_options{};

    Result ClearPresentationTags()
    {
        if (!has_frame || !fg_set_options) return Result::Success();
        // eValidUntilPresent tags retain COM references until explicitly cleared,
        // independently of the SDK input-processing fence (DLSS-G guide 5.2).
        const std::array tags{
            sl::ResourceTag(nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent),
            sl::ResourceTag(nullptr, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent),
            sl::ResourceTag(nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent),
            sl::ResourceTag(nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent),
            sl::ResourceTag(nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent)};
        const auto native = set_tags(*frame_token, sl::ViewportHandle(0u), tags.data(),
                                    static_cast<std::uint32_t>(tags.size()), nullptr);
        if (native != sl::Result::eOk) return Fail(StreamlineFeature::FrameGeneration, "slSetTagForFrame(clear presentation)", native);
        return Result::Success();
    }

    Result RequireOwner() const
    {
        if (owner_thread != GetCurrentThreadId())
            return StreamlineFailure(ErrorCode::InvalidState, "Streamline API called outside the renderer/present owner thread.");
        if (!started) return StreamlineFailure(ErrorCode::InvalidState, "Streamline has not successfully initialized.");
        return Result::Success();
    }
    Result RequireFeature(StreamlineFeature selected) const
    {
        if (auto owner = RequireOwner(); !owner) return owner;
        if (!device) return StreamlineFailure(ErrorCode::InvalidState, "Bind the native device before using a Streamline feature.");
        const auto &state = feature[Index(selected)];
        if (state.availability != ReconstructionAvailability::Available)
            return StreamlineFailure(ErrorCode::InvalidState, state.reason);
        return Result::Success();
    }
    Result Fail(StreamlineFeature selected, const char *operation, sl::Result native)
    {
        auto &state = feature[Index(selected)];
        state.reason = NativeFailure(operation, native);
        state.availability = Unsupported(native) ? ReconstructionAvailability::Unsupported : ReconstructionAvailability::Unavailable;
        return StreamlineFailure(ErrorCode::TaskFailed, state.reason);
    }
    Result FailAll(std::string reason)
    {
        for (auto &state : feature) if (state.requested)
        {
            state.availability = ReconstructionAvailability::Unavailable;
            state.reason = reason;
        }
        return StreamlineFailure(ErrorCode::TaskFailed, std::move(reason));
    }
    template <typename Function> bool Load(Function &function, const char *name)
    {
        function = reinterpret_cast<Function>(GetProcAddress(module, name));
        return function != nullptr;
    }
    template <typename Function> Result FeatureFunction(StreamlineFeature selected, sl::Feature native_feature,
                                                       Function &function, const char *name)
    {
        void *address{};
        const auto native = get_function(native_feature, name, address);
        if (native != sl::Result::eOk) return Fail(selected, name, native);
        if (!address) return Fail(selected, name, sl::Result::eErrorFeatureMissing);
        function = reinterpret_cast<Function>(address);
        return Result::Success();
    }
    Result Upgrade(void **object)
    {
        if (auto owner = RequireOwner(); !owner) return owner;
        if (!object || !*object) return StreamlineFailure(ErrorCode::InvalidArgument, "Cannot upgrade a null native interface.");
        void *base{};
        auto native = get_native(*object, &base);
        if (native != sl::Result::eOk || !base)
            return StreamlineFailure(ErrorCode::TaskFailed, NativeFailure("slGetNativeInterface", native));
        const bool already_upgraded = base != *object;
        static_cast<IUnknown *>(base)->Release();
        if (already_upgraded) return Result::Success();
        auto *original = static_cast<IUnknown *>(*object);
        native = upgrade(object);
        if (native != sl::Result::eOk) return StreamlineFailure(ErrorCode::TaskFailed, NativeFailure("slUpgradeInterface", native));
        // The SDK proxy retains its own native reference; slUpgradeInterface
        // replaces the caller's pointer without consuming its owned reference.
        if (*object != original) original->Release();
        return Result::Success();
    }
    Result Constants(const SdkCamera &camera, SdkRect active)
    {
        if (constants_frame == identity.render_id) return Result::Success();
        if (!active.width || !active.height || !FiniteMatrix(camera.projection) || !FiniteMatrix(camera.inverse_projection) ||
            !FiniteMatrix(camera.current_unjittered_view_projection) || !FiniteMatrix(camera.previous_unjittered_view_projection) ||
            !std::isfinite(camera.near_plane) || camera.near_plane <= 0.0f || !std::isfinite(camera.far_plane) ||
            camera.far_plane <= camera.near_plane || !std::isfinite(camera.vertical_fov_radians) ||
            camera.vertical_fov_radians <= 0.0f || !std::isfinite(camera.jitter_pixels[0]) || !std::isfinite(camera.jitter_pixels[1]))
            return StreamlineFailure(ErrorCode::InvalidArgument, "Streamline requires finite unjittered camera matrices, frustum and jitter.");
        using namespace DirectX;
        XMVECTOR determinant;
        const auto current = XMLoadFloat4x4(&camera.current_unjittered_view_projection);
        const auto previous = XMLoadFloat4x4(&camera.previous_unjittered_view_projection);
        const auto inverse_current = XMMatrixInverse(&determinant, current);
        if (!std::isfinite(XMVectorGetX(determinant)) || XMVectorGetX(determinant) == 0.0f)
            return StreamlineFailure(ErrorCode::InvalidArgument, "Streamline current unjittered view projection is singular.");
        const auto current_to_previous = XMMatrixMultiply(inverse_current, previous);
        const auto previous_to_current = XMMatrixInverse(&determinant, current_to_previous);
        if (!std::isfinite(XMVectorGetX(determinant)) || XMVectorGetX(determinant) == 0.0f)
            return StreamlineFailure(ErrorCode::InvalidArgument, "Streamline previous unjittered view projection is singular.");
        sl::Constants constants;
        constants.cameraViewToClip = NativeMatrix(camera.projection);
        constants.clipToCameraView = NativeMatrix(camera.inverse_projection);
        constants.clipToPrevClip = NativeMatrix(current_to_previous);
        constants.prevClipToClip = NativeMatrix(previous_to_current);
        constants.jitterOffset = {camera.jitter_pixels[0], camera.jitter_pixels[1]};
        constants.mvecScale = {1.0f, 1.0f}; // PreviousUV - CurrentUV is already normalized UV displacement.
        constants.cameraPinholeOffset = {0.0f, 0.0f};
        constants.cameraPos = {camera.position.x, camera.position.y, camera.position.z};
        constants.cameraRight = {camera.right.x, camera.right.y, camera.right.z};
        constants.cameraUp = {camera.up.x, camera.up.y, camera.up.z};
        constants.cameraFwd = {camera.forward.x, camera.forward.y, camera.forward.z};
        constants.cameraNear = camera.near_plane;
        constants.cameraFar = camera.far_plane;
        constants.cameraFOV = camera.vertical_fov_radians;
        constants.cameraAspectRatio = static_cast<float>(active.width) / static_cast<float>(active.height);
        constants.depthInverted = camera.reversed_depth ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        constants.cameraMotionIncluded = sl::Boolean::eTrue;
        constants.motionVectors3D = sl::Boolean::eFalse;
        constants.reset = camera.reset_history || force_reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        constants.orthographicProjection = sl::Boolean::eFalse;
        constants.motionVectorsDilated = sl::Boolean::eFalse;
        constants.motionVectorsJittered = sl::Boolean::eFalse;
        if (!frame_token) return StreamlineFailure(ErrorCode::InvalidState, "Streamline constants require a valid frame token.");
        const auto native = set_constants(constants, *frame_token, sl::ViewportHandle(viewport));
        if (native != sl::Result::eOk) return StreamlineFailure(ErrorCode::TaskFailed, NativeFailure("slSetConstants", native));
        constants_frame = identity.render_id;
        return Result::Success();
    }
    Result ValidateReconstruction(const SdkReconstructionFrame &frame)
    {
        if (auto required = RequireFeature(StreamlineFeature::SuperResolution); !required)
            return required;
        if (!reconstruction_configured || frame.identity.viewport_id != viewport)
            return StreamlineFailure(ErrorCode::InvalidState, "Streamline reconstruction call does not match its configured SR path or viewport.");
        if (!FitsRect(frame.color, frame.active) || !FitsRect(frame.depth, frame.active) || !FitsRect(frame.motion, frame.active) ||
            !FitsRect(frame.output, frame.output_rect) || !FloatColor(frame.color) || !FloatColor(frame.output) ||
            !DepthFormat(frame.depth) || !MotionFormat(frame.motion) ||
            !(frame.output->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || frame.color == frame.output)
            return StreamlineFailure(ErrorCode::InvalidArgument, "Streamline requires separate float HDR input/UAV output, reversed device depth, and RG float unjittered motion at valid extents.");
        if (!ShaderReadable(frame.color_state) || !ShaderReadable(frame.depth_state) || !ShaderReadable(frame.motion_state) ||
            frame.output_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS ||
            (frame.exposure && !ShaderReadable(frame.exposure_state)) || (frame.responsive && !ShaderReadable(frame.responsive_state)))
            return StreamlineFailure(ErrorCode::InvalidArgument, "Streamline native packet states must reflect graph shader-readable inputs and UAV output.");
        if (frame.output_rect.width != output.width || frame.output_rect.height != output.height ||
            frame.active.width < input_range.minimum.width || frame.active.height < input_range.minimum.height ||
            frame.active.width > input_range.maximum.width || frame.active.height > input_range.maximum.height)
            return StreamlineFailure(ErrorCode::InvalidArgument, "Streamline input/output extents do not match the native optimal-settings range.");
        if (!std::isfinite(frame.input_pre_exposure) || frame.input_pre_exposure <= 0.0f ||
            !std::isfinite(frame.exposure_scale) || frame.exposure_scale <= 0.0f)
            return StreamlineFailure(ErrorCode::InvalidArgument, "Streamline pre-exposure and exposure scale must be positive and finite.");
        if (frame.exposure && (!FitsRect(frame.exposure, {0, 0, 1, 1}) || !Format(frame.exposure, {DXGI_FORMAT_R32_FLOAT})))
            return StreamlineFailure(ErrorCode::InvalidArgument, "Streamline exposure must be the explicit R32_FLOAT exposure texture.");
        if (frame.responsive && (!FitsRect(frame.responsive, frame.active) ||
            !Format(frame.responsive, {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R32_FLOAT})))
            return StreamlineFailure(ErrorCode::InvalidArgument, "Streamline responsive mask must be a scalar mask at input extent.");
        return Result::Success();
    }
    Result WaitLease(PresentationLease &lease)
    {
        if (!lease.fence || !lease.fence_value)
            return StreamlineFailure(ErrorCode::InvalidState, "DLSS-G input lease has no verified presenting-queue completion fence.");
        const auto completed = lease.fence->GetCompletedValue();
        if (completed == std::numeric_limits<std::uint64_t>::max())
            return StreamlineFailure(ErrorCode::TaskFailed, "DLSS-G presenting-queue completion fence reports device removal.");
        if (completed >= lease.fence_value) return Result::Success();
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return StreamlineFailure(ErrorCode::TaskFailed, "Cannot create the DLSS-G presenting-queue completion event.");
        const auto result = lease.fence->SetEventOnCompletion(lease.fence_value, event);
        if (FAILED(result))
        {
            CloseHandle(event);
            return StreamlineFailure(ErrorCode::TaskFailed, "Cannot register the DLSS-G presenting-queue completion fence event (HRESULT " + std::to_string(result) + ").");
        }
        const auto wait = WaitForSingleObject(event, INFINITE);
        CloseHandle(event);
        if (wait != WAIT_OBJECT_0) return StreamlineFailure(ErrorCode::TaskFailed, "Waiting for the DLSS-G presenting-queue completion fence failed.");
        if (lease.fence->GetCompletedValue() == std::numeric_limits<std::uint64_t>::max())
            return StreamlineFailure(ErrorCode::TaskFailed, "DLSS-G presenting-queue completion wait ended with device removal.");
        return Result::Success();
    }
    void RetireCompleted()
    {
        while (!retired.empty())
        {
            const auto complete = retired.front().fence->GetCompletedValue();
            if (complete == std::numeric_limits<std::uint64_t>::max() || complete < retired.front().fence_value) break;
            retired.pop_front();
        }
    }
#endif
};

StreamlineSession::StreamlineSession() = default;
StreamlineSession::~StreamlineSession()
{
    if (auto result = ShutdownAfterCompletion(); !result)
    {
        // Teardown cannot unload proxy/plugin code or release resources while
        // SDK work remains. Report the missing drain and retain the one session.
#if defined(HS_DEVELOPMENT_TOOLS)
        std::fprintf(stderr, "hs.streamline.shutdown error=%s\n", std::string(result.Message()).c_str());
#endif
        static_cast<void>(impl_.release());
    }
    else if (impl_)
    {
        // The session cannot inspect all external proxy references. Explicit
        // UnloadAfterProxyRelease is the renderer's proof that they are gone.
#if defined(HS_DEVELOPMENT_TOOLS)
        std::fprintf(stderr, "hs.streamline.shutdown error=UnloadAfterProxyRelease was not called; proxy DLL remains loaded\n");
#endif
        static_cast<void>(impl_.release());
    }
}

#if HS_WITH_STREAMLINE
Result StreamlineSession::InitializeBeforeDevice(StreamlineFeatures requested)
{
    if (!requested.super_resolution && !requested.frame_generation && !requested.reflex)
        return Result::Success();
    if (impl_) return StreamlineFailure(ErrorCode::InvalidState, "Shut down the old Streamline session before initializing another.");
    impl_ = std::make_unique<Impl>();
    auto &state = *impl_;
    state.owner_thread = GetCurrentThreadId();
    const std::array<bool, 3> features{requested.super_resolution,
        requested.frame_generation, requested.reflex || requested.frame_generation};
    std::vector<sl::Feature> native_features;
    for (std::size_t index = 0; index != features.size(); ++index) if (features[index])
    {
        state.feature[index].requested = true;
        state.feature[index].reason = "Streamline feature has not been checked on the actual native device.";
        native_features.push_back(kNativeFeatures[index]);
    }
    if (features[Index(StreamlineFeature::Reflex)]) native_features.push_back(sl::kFeaturePCL);
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) return state.FailAll("Cannot resolve the executable directory for the pinned Streamline runtime.");
    executable.resize(length);
    const auto directory = std::filesystem::path(executable).parent_path() / L"streamline";
    const auto library = directory / L"sl.interposer.dll";
    state.plugin_directory = directory.wstring();
    std::error_code file_error;
    if (!std::filesystem::is_regular_file(library, file_error))
        return state.FailAll("Cannot load missing pinned Streamline runtime " + library.string() + ".");
    if (!sl::security::verifyEmbeddedSignature(library.c_str()))
        return state.FailAll("Pinned Streamline interposer failed the official NVIDIA embedded-signature validation.");
    state.module = LoadLibraryExW(library.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!state.module) return state.FailAll("Cannot load pinned Streamline runtime " + library.string() +
        " (Win32 " + std::to_string(GetLastError()) + ").");
    if (!state.Load(state.initialize, "slInit") || !state.Load(state.shutdown, "slShutdown") ||
        !state.Load(state.set_device, "slSetD3DDevice") || !state.Load(state.get_requirements, "slGetFeatureRequirements") ||
        !state.Load(state.is_supported, "slIsFeatureSupported") || !state.Load(state.is_loaded, "slIsFeatureLoaded") ||
        !state.Load(state.get_function, "slGetFeatureFunction") || !state.Load(state.get_frame_token, "slGetNewFrameToken") ||
        !state.Load(state.set_constants, "slSetConstants") || !state.Load(state.set_tags, "slSetTagForFrame") ||
        !state.Load(state.evaluate, "slEvaluateFeature") ||
        !state.Load(state.free_resources, "slFreeResources") ||
        !state.Load(state.upgrade, "slUpgradeInterface") || !state.Load(state.get_native, "slGetNativeInterface"))
        return state.FailAll("Pinned Streamline interposer is missing a required official core export.");
    const wchar_t *paths[]{state.plugin_directory.c_str()};
    sl::Preferences preferences;
    preferences.pathsToPlugins = paths;
    preferences.numPathsToPlugins = 1;
    preferences.featuresToLoad = native_features.data();
    preferences.numFeaturesToLoad = static_cast<std::uint32_t>(native_features.size());
    preferences.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseDXGIFactoryProxy |
        sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "ProjectHS 0.2.0"; // Actual project(CMake) version, no fabricated NVIDIA application ID.
    preferences.applicationId = 0;
    // Stable Project HS identity for NGX's custom-engine ProjectID API.
    // Streamline 2.14.1 supports this GUID path without a NVIDIA application ID.
    preferences.projectId = "93760093-4afc-4bfe-92dc-e47f2d20a980";
    preferences.renderAPI = sl::RenderAPI::eD3D12;
    preferences.logMessageCallback = StreamlineLog;
    preferences.pathToLogsAndData = nullptr;
    gStreamlinePresentError.store(0);
    const auto result = state.initialize(preferences, sl::kSDKVersion);
    if (result != sl::Result::eOk) return state.FailAll(NativeFailure("slInit", result));
    state.started = true;
    return Result::Success();
}

Result StreamlineSession::BindDevice(ID3D12Device *device)
{
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    auto &state = *impl_;
    if (auto owner = state.RequireOwner(); !owner) return owner;
    if (!device || state.device) return StreamlineFailure(ErrorCode::InvalidArgument, "Streamline device is null or already bound.");
    const auto bound = state.set_device(device);
    if (bound != sl::Result::eOk) return state.FailAll(NativeFailure("slSetD3DDevice", bound));
    state.device = device;
    LUID luid = device->GetAdapterLuid();
    sl::AdapterInfo adapter;
    adapter.deviceLUID = reinterpret_cast<std::uint8_t *>(&luid);
    adapter.deviceLUIDSizeInBytes = sizeof(luid);
    for (std::size_t index = 0; index != state.feature.size(); ++index)
    {
        if (!state.feature[index].requested) continue;
        const auto selected = static_cast<StreamlineFeature>(index);
        sl::FeatureRequirements requirements;
        auto native = state.get_requirements(kNativeFeatures[index], requirements);
        if (native != sl::Result::eOk) { static_cast<void>(state.Fail(selected, "slGetFeatureRequirements", native)); continue; }
        if (!(requirements.flags & sl::FeatureRequirementFlags::eD3D12Supported))
        { static_cast<void>(state.Fail(selected, "slGetFeatureRequirements(D3D12)", sl::Result::eErrorFeatureNotSupported)); continue; }
        native = state.is_supported(kNativeFeatures[index], adapter);
        if (native != sl::Result::eOk) { static_cast<void>(state.Fail(selected, "slIsFeatureSupported", native)); continue; }
        bool loaded{};
        native = state.is_loaded(kNativeFeatures[index], loaded);
        if (native != sl::Result::eOk || !loaded)
        { static_cast<void>(state.Fail(selected, "slIsFeatureLoaded", native != sl::Result::eOk ? native : sl::Result::eErrorFeatureFailedToLoad)); continue; }
        state.feature[index].availability = ReconstructionAvailability::Available;
        state.feature[index].reason.clear();
    }
    auto load = [&](StreamlineFeature feature, auto &function, const char *name)
    { return state.FeatureFunction(feature, kNativeFeatures[Index(feature)], function, name); };
    if (Availability(StreamlineFeature::SuperResolution) == ReconstructionAvailability::Available)
    {
        static_cast<void>(load(StreamlineFeature::SuperResolution, state.sr_optimal, "slDLSSGetOptimalSettings"));
        static_cast<void>(load(StreamlineFeature::SuperResolution, state.sr_set_options, "slDLSSSetOptions"));
    }
    if (Availability(StreamlineFeature::Reflex) == ReconstructionAvailability::Available)
    {
        static_cast<void>(load(StreamlineFeature::Reflex, state.reflex_sleep, "slReflexSleep"));
        static_cast<void>(load(StreamlineFeature::Reflex, state.reflex_set_options, "slReflexSetOptions"));
        static_cast<void>(load(StreamlineFeature::Reflex, state.reflex_get_state, "slReflexGetState"));
        auto native = state.is_supported(sl::kFeaturePCL, adapter);
        bool loaded{};
        if (native == sl::Result::eOk) native = state.is_loaded(sl::kFeaturePCL, loaded);
        if (native != sl::Result::eOk || !loaded)
            static_cast<void>(state.Fail(StreamlineFeature::Reflex, "Reflex requires PCL", native != sl::Result::eOk ? native : sl::Result::eErrorFeatureMissingDependency));
        else
        {
            static_cast<void>(state.FeatureFunction(StreamlineFeature::Reflex, sl::kFeaturePCL, state.pcl_marker, "slPCLSetMarker"));
            static_cast<void>(state.FeatureFunction(StreamlineFeature::Reflex, sl::kFeaturePCL, state.pcl_options, "slPCLSetOptions"));
        }
        if (Availability(StreamlineFeature::Reflex) == ReconstructionAvailability::Available)
        {
            const sl::PCLOptions pcl;
            native = state.pcl_options(pcl);
            if (native != sl::Result::eOk) static_cast<void>(state.Fail(StreamlineFeature::Reflex, "slPCLSetOptions", native));
            else static_cast<void>(ConfigureReflex(false)); // Required even when low latency is Off.
        }
    }
    if (Availability(StreamlineFeature::FrameGeneration) == ReconstructionAvailability::Available)
    {
        static_cast<void>(load(StreamlineFeature::FrameGeneration, state.fg_set_options, "slDLSSGSetOptions"));
        static_cast<void>(load(StreamlineFeature::FrameGeneration, state.fg_get_state, "slDLSSGGetState"));
        if (Availability(StreamlineFeature::Reflex) != ReconstructionAvailability::Available)
            static_cast<void>(state.Fail(StreamlineFeature::FrameGeneration, "DLSS-G requires active Reflex", sl::Result::eErrorFeatureMissingDependency));
    }
    return Result::Success(); // Capability failures are reported individually for actual provider resolution.
}

Result StreamlineSession::UpgradeDevice(void **device)
{
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    const auto result = impl_->Upgrade(device);
    if (!result) ReportPresentationBootstrapFailure(result.Message());
    return result;
}
void StreamlineSession::ReportPresentationBootstrapFailure(std::string_view reason)
{
    if (!impl_) return;
    auto &feature = impl_->feature[Index(StreamlineFeature::FrameGeneration)];
    feature.availability = ReconstructionAvailability::Unavailable;
    feature.reason = reason;
}
Result StreamlineSession::UpgradeFactory(void **factory)
{
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    return impl_->Upgrade(factory);
}
Result StreamlineSession::UpgradeSwapChain(void **swapchain)
{
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    return impl_->Upgrade(swapchain);
}

Result StreamlineSession::GetNativeInterface(void *proxy, void **native)
{
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    if (auto owner = impl_->RequireOwner(); !owner) return owner;
    if (!proxy || !native || *native)
        return StreamlineFailure(ErrorCode::InvalidArgument, "Streamline native-interface query needs an input and an empty destination for its AddRef'd reference.");
    const auto result = impl_->get_native(proxy, native);
    if (result != sl::Result::eOk)
        return StreamlineFailure(ErrorCode::TaskFailed, NativeFailure("slGetNativeInterface", result));
    if (!*native) return StreamlineFailure(ErrorCode::TaskFailed, "Streamline returned no native interface.");
    return Result::Success();
}

Result StreamlineSession::AttachPresentation(IDXGISwapChain4 *proxy, ID3D12CommandQueue *queue)
{
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    auto &state = *impl_;
    if (auto owner = state.RequireOwner(); !owner) return owner;
    if (!state.device || !proxy || !queue || state.swapchain || state.presentation_fence ||
        state.presentation_prepared || !state.retired.empty())
        return StreamlineFailure(ErrorCode::InvalidState, "Streamline presentation attach requires a device, a recreated exclusive proxy swapchain and an idle presenting queue.");
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return StreamlineFailure(ErrorCode::InvalidArgument, "Streamline presenting queue must be Direct.");
    void *native{};
    const auto result = state.get_native(proxy, &native);
    if (result != sl::Result::eOk || !native || native == proxy)
        return StreamlineFailure(ErrorCode::InvalidArgument, "Streamline needs its actual upgraded swapchain for Present and common-plugin garbage collection.");
    static_cast<IUnknown *>(native)->Release();
    ComPtr<ID3D12Fence> presentation_fence;
    const auto fence_result = state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&presentation_fence));
    if (FAILED(fence_result))
        return StreamlineFailure(ErrorCode::TaskFailed,
            "Cannot create the DLSS-G presenting-queue completion fence (HRESULT " +
            std::to_string(fence_result) + ").");
    state.swapchain = proxy;
    state.presenting_queue = queue;
    state.presentation_fence = std::move(presentation_fence);
    state.presentation_fence_value = 0;
    state.presentation_recreate_required = false;
    state.force_reset = true;
    return Result::Success();
}

Result StreamlineSession::QueryInputExtent(const GraphicsModeId &quality, ViewExtent output,
                                         StreamlineInputExtent &range)
{
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    auto &state = *impl_;
    if (auto required = state.RequireFeature(StreamlineFeature::SuperResolution); !required) return required;
    sl::DLSSMode mode;
    if (!output.width || !output.height || !Quality(quality.value, mode))
        return StreamlineFailure(ErrorCode::InvalidArgument, "Invalid DLSS output extent or unsupported quality ID: " + quality.value);
    sl::DLSSOptions options;
    options.mode = mode; options.outputWidth = output.width; options.outputHeight = output.height;
    sl::DLSSOptimalSettings settings;
    const auto native = state.sr_optimal(options, settings);
    if (native != sl::Result::eOk) return CandidateFailure("slDLSSGetOptimalSettings", native);
    const StreamlineInputExtent candidate{{settings.optimalRenderWidth, settings.optimalRenderHeight},
        {settings.renderWidthMin, settings.renderHeightMin}, {settings.renderWidthMax, settings.renderHeightMax}};
    if (!candidate.optimal.width || !candidate.optimal.height || !candidate.minimum.width || !candidate.minimum.height ||
        candidate.minimum.width > candidate.optimal.width || candidate.minimum.height > candidate.optimal.height ||
        candidate.maximum.width < candidate.optimal.width || candidate.maximum.height < candidate.optimal.height)
        return StreamlineFailure(ErrorCode::TaskFailed, "Streamline returned invalid optimal/minimum/maximum input extents.");
    range = candidate;
    return Result::Success();
}

Result StreamlineSession::ConfigureReconstruction(const GraphicsModeId &quality, ViewExtent output,
                                                std::uint32_t viewport)
{
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    if (auto selected = SelectReconstructionViewAfterCompletion(viewport); !selected) return selected;
    auto &state = *impl_;
    if (state.reconstruction_configured || state.reconstruction_evaluated || state.sr_viewport_owned)
        return StreamlineFailure(ErrorCode::InvalidState, "Drain old DLSS graph uses and release reconstruction before reconfiguration.");
    StreamlineInputExtent range;
    if (auto queried = QueryInputExtent(quality, output, range); !queried) return queried;
    sl::DLSSMode mode;
    if (!Quality(quality.value, mode)) return StreamlineFailure(ErrorCode::InvalidArgument, "Unknown DLSS quality.");
    state.sr_options = sl::DLSSOptions{};
    state.sr_options.mode = mode;
    state.sr_options.outputWidth = output.width;
    state.sr_options.outputHeight = output.height;
    state.sr_options.colorBuffersHDR = sl::Boolean::eTrue;
    state.sr_options.useAutoExposure = sl::Boolean::eFalse;
    const auto native = state.sr_set_options(sl::ViewportHandle(viewport), state.sr_options);
    if (native != sl::Result::eOk) return CandidateFailure("slDLSSSetOptions", native);
    // Native options have changed; retain a releasable viewport if later allocation fails.
    state.viewport = viewport;
    state.reconstruction_configured = true;
    state.output = output;
    state.quality = quality;
    state.input_range = range;
    state.force_reset = true;
    return Result::Success();
}

Result StreamlineSession::ExecuteSuperResolution(ID3D12GraphicsCommandList *commands, const SdkReconstructionFrame &frame)
{
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    auto &state = *impl_;
    if (!commands) return StreamlineFailure(ErrorCode::InvalidArgument, "DLSS-SR requires a recording Direct command list.");
    if (auto valid = state.ValidateReconstruction(frame); !valid) return valid;
    if (auto begun = BeginFrame(frame.identity); !begun) return begun;
    if (state.evaluated_frame == frame.identity.render_id)
        return StreamlineFailure(ErrorCode::InvalidState, "DLSS reconstruction has already evaluated this rendered frame.");
    if (auto constants = state.Constants(frame.camera, frame.active); !constants) return constants;
    state.sr_options.preExposure = frame.input_pre_exposure;
    state.sr_options.exposureScale = frame.exposure_scale;
    auto native = state.sr_set_options(sl::ViewportHandle(state.viewport), state.sr_options);
    if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::SuperResolution, "slDLSSSetOptions", native);
    const auto input = NativeRect(frame.active), output = NativeRect(frame.output_rect);
    const sl::Extent exposure_extent{0, 0, 1, 1};
    std::array<sl::Resource, 6> resources;
    std::array<sl::ResourceTag, 6> tags;
    std::uint32_t count{};
    auto add = [&](ID3D12Resource *resource, sl::BufferType type, const sl::Extent &extent, D3D12_RESOURCE_STATES resource_state)
    {
        resources[count] = NativeResource(resource, resource_state);
        tags[count] = sl::ResourceTag(resource ? &resources[count] : nullptr, type, sl::ResourceLifecycle::eValidUntilEvaluate, &extent);
        ++count;
    };
    add(frame.color, sl::kBufferTypeScalingInputColor, input, frame.color_state);
    add(frame.output, sl::kBufferTypeScalingOutputColor, output, frame.output_state);
    add(frame.depth, sl::kBufferTypeDepth, input, frame.depth_state);
    add(frame.motion, sl::kBufferTypeMotionVectors, input, frame.motion_state);
    add(frame.exposure, sl::kBufferTypeExposure, exposure_extent, frame.exposure_state);
    add(frame.responsive, sl::kBufferTypeResponsivityMask, input, frame.responsive_state);
    native = state.set_tags(*state.frame_token, sl::ViewportHandle(state.viewport), tags.data(), count, commands);
    if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::SuperResolution, "slSetTagForFrame(DLSS-SR)", native);
    const sl::ViewportHandle viewport(state.viewport);
    const sl::BaseStructure *inputs[]{&viewport};
    // Evaluation can record work before reporting failure; teardown must still
    // require completion of this command list.
    // Evaluation may create the native viewport before reporting a failure;
    // retain ownership through cleanup once this call is entered.
    state.reconstruction_evaluated = true;
    state.sr_viewport_owned = true;
    native = state.evaluate(sl::kFeatureDLSS, *state.frame_token, inputs, 1, commands);
    if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::SuperResolution, "slEvaluateFeature(DLSS-SR)", native);
    state.evaluated_frame = frame.identity.render_id;
    state.force_reset = false;
    return Result::Success();
}

Result StreamlineSession::SelectReconstructionViewAfterCompletion(std::uint32_t viewport)
{
    if (!impl_ || !impl_->started || impl_->viewport == viewport) return Result::Success();
    auto &state = *impl_;
    if (auto owner = state.RequireOwner(); !owner) return owner;
    auto exchange = [&](Impl::ReconstructionView &view) {
        using std::swap;
        swap(state.reconstruction_configured, view.reconstruction_configured);
        swap(state.reconstruction_evaluated, view.reconstruction_evaluated);
        swap(state.sr_viewport_owned, view.sr_viewport_owned);
        swap(state.force_reset, view.force_reset);
        swap(state.has_frame, view.has_frame);
        swap(state.identity, view.identity);
        swap(state.frame_token, view.frame_token);
        swap(state.constants_frame, view.constants_frame);
        swap(state.evaluated_frame, view.evaluated_frame);
        swap(state.quality, view.quality);
        swap(state.output, view.output);
        swap(state.input_range, view.input_range);
        swap(state.sr_options, view.sr_options);
    };
    exchange(state.reconstruction_views[state.viewport]);
    exchange(state.reconstruction_views[viewport]);
    state.viewport = viewport;
    return Result::Success();
}

Result StreamlineSession::ReleaseReconstructionAfterCompletion()
{
    if (!impl_) return Result::Success();
    auto &state = *impl_;
    if (auto owner = state.RequireOwner(); !owner) return owner;
    if (!state.device || (!state.reconstruction_configured && !state.reconstruction_evaluated && !state.sr_viewport_owned))
        return Result::Success();
    const sl::ViewportHandle viewport(state.viewport);
    if (state.sr_set_options)
    {
        auto options = state.sr_options;
        options.mode = sl::DLSSMode::eOff;
        const auto native = state.sr_set_options(viewport, options);
        if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::SuperResolution, "slDLSSSetOptions(Off)", native);
    }
    if (state.sr_viewport_owned)
    {
        const auto native = state.free_resources(sl::kFeatureDLSS, viewport);
        if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::SuperResolution, "slFreeResources(DLSS-SR)", native);
        state.sr_viewport_owned = false;
    }
    state.reconstruction_configured = false;
    state.reconstruction_evaluated = false;
    state.force_reset = true;
    return Result::Success();
}

Result StreamlineSession::BeginFrame(SdkFrameIdentity identity)
{
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    auto &state = *impl_;
    if (auto owner = state.RequireOwner(); !owner) return owner;
    if (!state.device) return StreamlineFailure(ErrorCode::InvalidState, "Streamline frame tokens require the bound native device.");
    if (identity.render_id > std::numeric_limits<std::uint32_t>::max())
        return StreamlineFailure(ErrorCode::InvalidArgument, "Streamline's pinned frame-token API requires a uint32 rendered frame ID.");
    if (state.has_frame && identity.render_id == state.identity.render_id)
    {
        if (identity.presentation_id != state.identity.presentation_id || identity.viewport_id != state.identity.viewport_id)
            return StreamlineFailure(ErrorCode::InvalidArgument, "Streamline render, present and viewport identity changed within one frame.");
        return Result::Success();
    }
    if (state.presentation_prepared || (state.has_frame && identity.render_id < state.identity.render_id))
        return StreamlineFailure(ErrorCode::InvalidState, "Complete the previous presentation before starting a monotonic Streamline rendered frame.");
    if (state.fg_options.mode != sl::DLSSGMode::eOff && identity.viewport_id == 0 && identity.render_id != identity.presentation_id)
        return StreamlineFailure(ErrorCode::InvalidArgument, "DLSS-G requires matching rendered/presented frame IDs and viewport 0.");
    const auto index = static_cast<std::uint32_t>(identity.render_id);
    if (!state.has_package_token || state.package_render_id != identity.render_id)
    {
        auto native = state.get_frame_token(state.package_frame_token, &index);
        if (native != sl::Result::eOk) return StreamlineFailure(ErrorCode::TaskFailed, NativeFailure("slGetNewFrameToken", native));
        state.package_render_id = identity.render_id;
        state.has_package_token = true;
    }
    state.frame_token = state.package_frame_token;
    if (!state.frame_token || static_cast<std::uint32_t>(*state.frame_token) != index)
        return StreamlineFailure(ErrorCode::TaskFailed, "Streamline did not return the requested native rendered-frame token.");
    state.identity = identity;
    state.has_frame = true;
    state.constants_frame = std::numeric_limits<std::uint64_t>::max();
    return Result::Success();
}

std::optional<std::uint64_t> StreamlineSession::NativeFrameToken(SdkFrameIdentity identity) const noexcept
{
    if (!impl_ || !impl_->has_frame || !impl_->frame_token ||
        impl_->identity.render_id != identity.render_id || impl_->identity.viewport_id != identity.viewport_id)
        return std::nullopt;
    return static_cast<std::uint32_t>(*impl_->frame_token);
}

Result StreamlineSession::InputSleep(SdkFrameIdentity identity)
{
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    auto &state = *impl_;
    if (auto required = state.RequireFeature(StreamlineFeature::Reflex); !required) return required;
    if (auto begun = BeginFrame(identity); !begun) return begun;
    // The plugin uses Sleep and PCL even when its low latency mode is Off.
    // The composition root selects this one latency owner and calls before input.
    const auto native = state.reflex_sleep(*state.frame_token);
    if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::Reflex, "slReflexSleep", native);
    return Result::Success();
}

Result StreamlineSession::Marker(SdkFrameIdentity identity, SdkLatencyMarker marker)
{
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    auto &state = *impl_;
    if (auto required = state.RequireFeature(StreamlineFeature::Reflex); !required) return required;
    if (auto begun = BeginFrame(identity); !begun) return begun;
    sl::PCLMarker native_marker;
    switch (marker)
    {
    case SdkLatencyMarker::SimulationStart: native_marker = sl::PCLMarker::eSimulationStart; break;
    case SdkLatencyMarker::SimulationEnd: native_marker = sl::PCLMarker::eSimulationEnd; break;
    case SdkLatencyMarker::RenderSubmitStart: native_marker = sl::PCLMarker::eRenderSubmitStart; break;
    case SdkLatencyMarker::RenderSubmitEnd: native_marker = sl::PCLMarker::eRenderSubmitEnd; break;
    case SdkLatencyMarker::PresentStart: native_marker = sl::PCLMarker::ePresentStart; break;
    case SdkLatencyMarker::PresentEnd: native_marker = sl::PCLMarker::ePresentEnd; break;
    case SdkLatencyMarker::InputSample: native_marker = sl::PCLMarker::eControllerInputSample; break;
    default: return StreamlineFailure(ErrorCode::InvalidArgument, "Unknown Streamline latency marker.");
    }
    const auto native = state.pcl_marker(native_marker, *state.frame_token);
    if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::Reflex, "slPCLSetMarker", native);
    return Result::Success();
}

Result StreamlineSession::ConfigureReflex(bool enabled, bool boost, std::uint32_t frame_limit_us)
{
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    auto &state = *impl_;
    if (auto required = state.RequireFeature(StreamlineFeature::Reflex); !required) return required;
    if (!enabled && state.fg_options.mode != sl::DLSSGMode::eOff)
        return StreamlineFailure(ErrorCode::InvalidState, "DLSS-G requires Reflex enabled through its entire presentation lifetime.");
    sl::ReflexState native_state;
    auto native = state.reflex_get_state(native_state);
    if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::Reflex, "slReflexGetState", native);
    if (enabled && !native_state.lowLatencyAvailable)
        return state.Fail(StreamlineFeature::Reflex, "slReflexGetState(lowLatencyAvailable)", sl::Result::eErrorFeatureNotSupported);
    sl::ReflexOptions options;
    options.mode = enabled ? (boost ? sl::ReflexMode::eLowLatencyWithBoost : sl::ReflexMode::eLowLatency) : sl::ReflexMode::eOff;
    options.frameLimitUs = frame_limit_us;
    native = state.reflex_set_options(options);
    if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::Reflex, "slReflexSetOptions", native);
    state.reflex_enabled = enabled;
    return Result::Success();
}

Result StreamlineSession::QueryReflexState(StreamlineReflexState &result)
{
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    auto &state = *impl_;
    if (auto required = state.RequireFeature(StreamlineFeature::Reflex); !required) return required;
    sl::ReflexState native_state;
    const auto native = state.reflex_get_state(native_state);
    if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::Reflex, "slReflexGetState", native);
    result = {native_state.lowLatencyAvailable, native_state.latencyReportAvailable, native_state.statsWindowMessage};
    return Result::Success();
}

Result StreamlineSession::QueryFrameGenerationCapabilities(StreamlineFrameGenerationCapabilities &capabilities)
{
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    auto &state = *impl_;
    if (auto required = state.RequireFeature(StreamlineFeature::FrameGeneration); !required) return required;
    sl::DLSSGState native_state;
    const auto native = state.fg_get_state(sl::ViewportHandle(0u), native_state, nullptr);
    if (native != sl::Result::eOk) { state.fg_last_state_query_qpc = 0; return state.Fail(StreamlineFeature::FrameGeneration, "slDLSSGGetState", native); }
    // Capability queries consume the same SDK counter; the next interval starts here.
    state.fg_last_state_query_qpc = RendererDiagnosticsCounter();
    capabilities = {native_state.numFramesToGenerateMax ? native_state.numFramesToGenerateMax + 1 : 0,
        native_state.minWidthOrHeight, native_state.numFramesActuallyPresented,
        native_state.bIsDynamicMFGSupported == sl::Boolean::eTrue, native_state.bIsVsyncSupportAvailable == sl::Boolean::eTrue};
    state.fg_capabilities = capabilities;
    return Result::Success();
}

Result StreamlineSession::ConfigureFrameGeneration(GraphicsFrameGenerationMode mode, std::uint32_t multiplier, float target_fps,
                                                 ViewExtent dynamic_resolution_optimal)
{
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    auto &state = *impl_;
    if (auto owner = state.RequireOwner(); !owner) return owner;
    if (!state.device || !state.fg_set_options)
        return StreamlineFailure(ErrorCode::InvalidState, "DLSS-G native options are unavailable.");
    if (mode != GraphicsFrameGenerationMode::Off)
        if (auto required = state.RequireFeature(StreamlineFeature::FrameGeneration); !required) return required;
    if (state.swapchain || state.presentation_fence || state.presentation_prepared || !state.retired.empty())
        return StreamlineFailure(ErrorCode::InvalidState, "Drain DLSS-G and detach/destroy its swapchain before changing frame generation options, then recreate it.");
    if (!std::isfinite(target_fps) || target_fps < 0.0f)
        return StreamlineFailure(ErrorCode::InvalidArgument, "DLSS-G target frame rate must be finite and nonnegative.");
    if ((dynamic_resolution_optimal.width == 0) != (dynamic_resolution_optimal.height == 0))
        return StreamlineFailure(ErrorCode::InvalidArgument, "DLSS-G dynamic resolution must provide both dimensions of a fixed processing extent.");
    StreamlineFrameGenerationCapabilities capabilities;
    if (mode != GraphicsFrameGenerationMode::Off)
        if (auto queried = QueryFrameGenerationCapabilities(capabilities); !queried) return queried;
    sl::DLSSGOptions options;
    switch (mode)
    {
    case GraphicsFrameGenerationMode::Off: options.mode = sl::DLSSGMode::eOff; break;
    case GraphicsFrameGenerationMode::Auto: options.mode = sl::DLSSGMode::eAuto; break;
    case GraphicsFrameGenerationMode::Fixed: options.mode = sl::DLSSGMode::eOn; break;
    case GraphicsFrameGenerationMode::Dynamic: options.mode = sl::DLSSGMode::eDynamic; break;
    default: return StreamlineFailure(ErrorCode::InvalidArgument, "Unknown DLSS-G mode.");
    }
    if (mode != GraphicsFrameGenerationMode::Off)
    {
        if (mode == GraphicsFrameGenerationMode::Dynamic && !capabilities.dynamic_supported)
            return StreamlineFailure(ErrorCode::InvalidArgument, "The pinned native DLSS-G state reports Dynamic MFG unsupported.");
        if (mode != GraphicsFrameGenerationMode::Dynamic && (multiplier < 2 || multiplier > capabilities.maximum_multiplier))
            return StreamlineFailure(ErrorCode::InvalidArgument, "Requested DLSS-G multiplier exceeds the actual native device range.");
        if (!state.reflex_enabled)
            return StreamlineFailure(ErrorCode::InvalidState, "Enable the selected Reflex latency owner before enabling DLSS-G.");
        if (state.reconstruction_configured && state.viewport != 0)
            return StreamlineFailure(ErrorCode::InvalidArgument, "DLSS-G and reconstruction must share viewport 0.");
        options.numFramesToGenerate = mode == GraphicsFrameGenerationMode::Dynamic ? 1 : multiplier - 1;
    }
    options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
    if (dynamic_resolution_optimal.width)
    {
        options.flags |= sl::DLSSGFlags::eDynamicResolutionEnabled;
        options.dynamicResWidth = dynamic_resolution_optimal.width;
        options.dynamicResHeight = dynamic_resolution_optimal.height;
    }
    options.dynamicTargetFrameRate = target_fps;
    options.enableUserInterfaceRecomposition = sl::Boolean::eTrue;
    options.onErrorCallback = StreamlineAPIError;
    const auto native = state.fg_set_options(sl::ViewportHandle(0u), options);
    if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::FrameGeneration, "slDLSSGSetOptions", native);
    state.fg_options = options;
    state.fg_last_state_query_qpc = 0; // Do not bridge mode changes with an old counter window.
    state.fg_input = {};
    state.presentation_recreate_required = true;
    return Result::Success();
}

Result StreamlineSession::PreparePresent(const SdkPresentationFrame &frame)
{
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    auto &state = *impl_;
    if (auto owner = state.RequireOwner(); !owner) return owner;
    if (!state.swapchain || !state.presenting_queue || state.presentation_recreate_required)
        return StreamlineFailure(ErrorCode::InvalidState, "Attach the recreated Streamline proxy before presentation.");
    if (state.fg_options.mode == sl::DLSSGMode::eOff) return Result::Success();
    if (auto required = state.RequireFeature(StreamlineFeature::FrameGeneration); !required) return required;
    if (!state.reflex_enabled || state.presentation_prepared)
        return StreamlineFailure(ErrorCode::InvalidState, "DLSS-G needs active Reflex and completion of the previous prepared presentation.");
    if (frame.identity.render_id != frame.identity.presentation_id || frame.identity.viewport_id != 0)
        return StreamlineFailure(ErrorCode::InvalidArgument, "DLSS-G requires matching render/presentation IDs at viewport 0.");
    if (frame.owners.empty() || std::any_of(frame.owners.begin(), frame.owners.end(), [](const auto &owner) { return !owner; }))
        return StreamlineFailure(ErrorCode::InvalidArgument, "DLSS-G presentation inputs require actual allocation owners through SDK completion.");
    if (!FitsRect(frame.hudless, frame.output_rect) || !FitsRect(frame.ui, frame.output_rect) ||
        !FitsRect(frame.depth, frame.render_rect) || !FitsRect(frame.motion, frame.render_rect) ||
        !FitsRect(frame.back_buffer, frame.output_rect) || !DepthFormat(frame.depth) || !MotionFormat(frame.motion) ||
        frame.back_buffer->GetDesc().Format != frame.output_format || frame.hudless->GetDesc().Format != frame.output_format ||
        !Format(frame.ui, {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
                          DXGI_FORMAT_R32G32B32A32_FLOAT}))
        return StreamlineFailure(ErrorCode::InvalidArgument, "DLSS-G needs valid undilated depth/motion, matching display color HUDless/backbuffer and a premultiplied UI buffer with sufficient alpha precision.");
    if (!ShaderReadable(frame.hudless_state) || !ShaderReadable(frame.ui_state) || !ShaderReadable(frame.depth_state) ||
        !ShaderReadable(frame.motion_state) || frame.back_buffer_state != D3D12_RESOURCE_STATE_PRESENT)
        return StreamlineFailure(ErrorCode::InvalidArgument, "DLSS-G explicit input states must be shader-readable and the backbuffer state must be Present.");
    const bool pq = frame.output_format == DXGI_FORMAT_R10G10B10A2_UNORM &&
        frame.color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    const bool sdr = (frame.output_format == DXGI_FORMAT_R8G8B8A8_UNORM || frame.output_format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        frame.output_format == DXGI_FORMAT_R10G10B10A2_UNORM) && frame.color_space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if (!pq && !sdr)
        return StreamlineFailure(ErrorCode::InvalidArgument, "Pinned DLSS-G supports SDR or UINT10/HDR10 PQ; FP16/scRGB is explicitly unsupported.");
    if (frame.output_rect.width < state.fg_capabilities.minimum_dimension || frame.output_rect.height < state.fg_capabilities.minimum_dimension)
        return StreamlineFailure(ErrorCode::InvalidArgument, "DLSS-G output is smaller than the native supported minimum dimension.");
    if (!(state.fg_options.flags & sl::DLSSGFlags::eDynamicResolutionEnabled) && state.fg_input.width &&
        (state.fg_input.width != frame.render_rect.width || state.fg_input.height != frame.render_rect.height))
        return StreamlineFailure(ErrorCode::InvalidState, "DLSS-G render extent changed without the native dynamic-resolution options. Drain and reconfigure its swapchain.");
    if (auto begun = BeginFrame(frame.identity); !begun) return begun;
    if (auto constants = state.Constants(frame.camera, frame.render_rect); !constants) return constants;
    state.RetireCompleted();
    // Six is the SDK's own finite frame-token capacity. At capacity, apply
    // backpressure at the true SDK fence rather than accumulating old leases.
    if (state.retired.size() >= sl::MAX_FRAMES_IN_FLIGHT - 1)
    {
        if (auto waited = state.WaitLease(state.retired.front()); !waited) return waited;
        state.RetireCompleted();
    }
    state.pending = {};
    state.pending.identity = frame.identity;
    state.pending.owners = frame.owners;
    state.pending.resources = {ComPtr<ID3D12Resource>(frame.hudless), ComPtr<ID3D12Resource>(frame.ui),
        ComPtr<ID3D12Resource>(frame.depth), ComPtr<ID3D12Resource>(frame.motion), ComPtr<ID3D12Resource>(frame.back_buffer)};
    state.presentation_prepared = true; // Retain even if tagging reports a partial failure.
    state.presented_without_fence = false;
    const auto render = NativeRect(frame.render_rect), output = NativeRect(frame.output_rect);
    std::array<sl::Resource, 5> resources{
        NativeResource(frame.hudless, frame.hudless_state),
        NativeResource(frame.ui, frame.ui_state),
        NativeResource(frame.depth, frame.depth_state),
        NativeResource(frame.motion, frame.motion_state),
        NativeResource(frame.back_buffer, frame.back_buffer_state)};
    std::array<sl::ResourceTag, 5> tags{
        sl::ResourceTag(&resources[0], sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &output),
        sl::ResourceTag(&resources[1], sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, &output),
        sl::ResourceTag(&resources[2], sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &render),
        sl::ResourceTag(&resources[3], sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &render),
        sl::ResourceTag(&resources[4], sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &output)};
    const auto native = state.set_tags(*state.frame_token, sl::ViewportHandle(0u), tags.data(), static_cast<std::uint32_t>(tags.size()), nullptr);
    if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::FrameGeneration, "slSetTagForFrame(DLSS-G)", native);
    state.fg_input = {frame.render_rect.width, frame.render_rect.height};
    return Result::Success();
}

Result StreamlineSession::PresentCompleted(SdkFrameIdentity identity, std::uint32_t *frames_actually_presented,
                                          RendererStreamlinePresentCount *observation)
{
    if (observation) *observation = {};
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    auto &state = *impl_;
    if (auto owner = state.RequireOwner(); !owner) return owner;
    if (state.fg_options.mode == sl::DLSSGMode::eOff)
    {
        state.force_reset = false;
        return Result::Success();
    }
    if (!state.presentation_prepared || state.pending.identity.render_id != identity.render_id ||
        state.pending.identity.presentation_id != identity.presentation_id || state.pending.identity.viewport_id != identity.viewport_id)
        return StreamlineFailure(ErrorCode::InvalidState, "DLSS-G Present completion does not match its tagged frame identity.");
    state.presented_without_fence = true;
    sl::DLSSGState native_state;
    auto native = state.fg_get_state(sl::ViewportHandle(0u), native_state, nullptr);
    const auto observed_qpc = RendererDiagnosticsCounter();
    const auto previous_query_qpc = std::exchange(state.fg_last_state_query_qpc, observed_qpc);
    if (native != sl::Result::eOk)
    {
        state.fg_last_state_query_qpc = 0;
        return state.Fail(StreamlineFeature::FrameGeneration, "slDLSSGGetState(after Present)", native);
    }
    if (observation)
    {
        LARGE_INTEGER frequency{};
        const bool frequency_valid = QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0;
        *observation = {previous_query_qpc, observed_qpc,
            frequency_valid ? static_cast<std::uint64_t>(frequency.QuadPart) : 0,
            native_state.numFramesActuallyPresented,
            frequency_valid && previous_query_qpc > 0 && observed_qpc > previous_query_qpc};
    }
    if (frames_actually_presented) *frames_actually_presented = native_state.numFramesActuallyPresented;
    if (!state.presenting_queue || !state.presentation_fence)
    {
        state.presented_without_fence = true;
        return StreamlineFailure(ErrorCode::InvalidState,
            "DLSS-G presented inputs have no attached presenting queue completion fence.");
    }
    if (state.presentation_fence_value == std::numeric_limits<std::uint64_t>::max())
    {
        state.presented_without_fence = true;
        return StreamlineFailure(ErrorCode::TaskFailed,
            "DLSS-G presenting queue completion fence value exhausted.");
    }
    const auto fence_value = ++state.presentation_fence_value;
    const auto signal = state.presenting_queue->Signal(state.presentation_fence.Get(), fence_value);
    if (FAILED(signal))
    {
        state.presented_without_fence = true;
        return StreamlineFailure(ErrorCode::TaskFailed,
            "Cannot signal the DLSS-G presenting queue completion fence (HRESULT " +
            std::to_string(signal) + ").");
    }
    state.pending.fence = state.presentation_fence;
    state.pending.fence_value = fence_value;
    state.retired.push_back(std::move(state.pending));
    state.pending = {};
    state.presentation_prepared = false;
    state.presented_without_fence = false;
    state.force_reset = false;
    state.fg_capabilities.frames_actually_presented = native_state.numFramesActuallyPresented;
    state.RetireCompleted();
    if (native_state.status != sl::DLSSGStatus::eOk)
    {
        auto &feature = state.feature[Index(StreamlineFeature::FrameGeneration)];
        feature.availability = ReconstructionAvailability::Unavailable;
        feature.reason = "DLSS-G reported native runtime status bits " + std::to_string(static_cast<std::uint32_t>(native_state.status)) + ". Drain SDK inputs and recreate presentation before disabling it.";
        return StreamlineFailure(ErrorCode::TaskFailed, feature.reason);
    }
    const auto api_error = gStreamlinePresentError.exchange(0);
    if (FAILED(api_error))
    {
        auto &feature = state.feature[Index(StreamlineFeature::FrameGeneration)];
        feature.availability = ReconstructionAvailability::Unavailable;
        feature.reason = "DLSS-G asynchronous presentation reported HRESULT " + std::to_string(api_error) + ".";
        return StreamlineFailure(ErrorCode::TaskFailed, feature.reason);
    }
    return Result::Success();
}

Result StreamlineSession::WaitForPresentationInputs(ID3D12CommandQueue *queue)
{
    if (!impl_) return Result::Success();
    auto &state = *impl_;
    if (auto owner = state.RequireOwner(); !owner) return owner;
    if (!queue) return StreamlineFailure(ErrorCode::InvalidArgument, "DLSS-G reuse waits require the queue that will modify the inputs.");
    if (state.presentation_prepared)
        return StreamlineFailure(ErrorCode::InvalidState, "DLSS-G input reuse attempted before matching actual Present completion.");
    state.RetireCompleted();
    for (const auto &lease : state.retired)
    {
        const auto native = queue->Wait(lease.fence.Get(), lease.fence_value);
        if (FAILED(native)) return StreamlineFailure(ErrorCode::TaskFailed,
            "Cannot enqueue DLSS-G presenting-queue completion fence wait (HRESULT " + std::to_string(native) + ").");
    }
    return Result::Success();
}

Result StreamlineSession::DrainPresentation()
{
    if (!impl_) return Result::Success();
    auto &state = *impl_;
    if (auto owner = state.RequireOwner(); !owner) return owner;
    if (state.presentation_prepared)
        return StreamlineFailure(ErrorCode::InvalidState, state.presented_without_fence
            ? "DLSS-G presented inputs have no verified presenting-queue completion fence; their native retirement boundary is unresolved."
            : "DLSS-G presentation tags are pending; complete the actual Present before draining input leases.");
    for (auto &lease : state.retired)
        if (auto waited = state.WaitLease(lease); !waited) return waited;
    state.retired.clear();
    return Result::Success();
}

Result StreamlineSession::DetachPresentationAfterCompletion()
{
    if (!impl_) return Result::Success();
    auto &state = *impl_;
    if (auto drained = DrainPresentation(); !drained) return drained;
    if (auto cleared = state.ClearPresentationTags(); !cleared) return cleared;
    // Stop the asynchronous pacer before the host performs any resolution,
    // fullscreen or swapchain destruction operation (DLSS-G guide section 12).
    if (state.fg_set_options)
    {
        auto options = state.fg_options;
        options.mode = sl::DLSSGMode::eOff;
        const auto native = state.fg_set_options(sl::ViewportHandle(0u), options);
        if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::FrameGeneration, "slDLSSGSetOptions(detach Off)", native);
        state.fg_options = options;
    }
    state.presentation_fence.Reset();
    state.presentation_fence_value = 0;
    state.swapchain.Reset();
    state.presenting_queue.Reset();
    state.presentation_recreate_required = true;
    return Result::Success();
}

Result StreamlineSession::ShutdownAfterCompletion()
{
    if (!impl_) return Result::Success();
    auto &state = *impl_;
    if (state.owner_thread != GetCurrentThreadId())
        return StreamlineFailure(ErrorCode::InvalidState, "Streamline shutdown called outside its initialization thread.");
    if (state.started)
    {
        if (auto drained = DrainPresentation(); !drained) return drained;
        if (auto cleared = state.ClearPresentationTags(); !cleared) return cleared;
        std::vector<std::uint32_t> viewports;
        for (const auto &[viewport, _] : state.reconstruction_views) viewports.push_back(viewport);
        for (const auto viewport : viewports)
        {
            if (auto selected = SelectReconstructionViewAfterCompletion(viewport); !selected) return selected;
            if (auto released = ReleaseReconstructionAfterCompletion(); !released) return released;
        }
        if (auto selected = SelectReconstructionViewAfterCompletion(0); !selected) return selected;
        if (auto released = ReleaseReconstructionAfterCompletion(); !released) return released;
        state.reconstruction_views.clear();
        if (state.fg_set_options)
        {
            sl::DLSSGOptions options;
            options.mode = sl::DLSSGMode::eOff;
            auto native = state.fg_set_options(sl::ViewportHandle(0u), options);
            if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::FrameGeneration, "slDLSSGSetOptions(shutdown)", native);
            state.fg_options = options;
            native = state.free_resources(sl::kFeatureDLSS_G, sl::ViewportHandle(0u));
            if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::FrameGeneration, "slFreeResources(DLSS-G)", native);
        }
        if (state.reflex_set_options)
        {
            const sl::ReflexOptions options;
            const auto native = state.reflex_set_options(options);
            if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::Reflex, "slReflexSetOptions(shutdown)", native);
            state.reflex_enabled = false;
        }
        const auto native = state.shutdown();
        if (native != sl::Result::eOk) return state.FailAll(NativeFailure("slShutdown", native));
        state.started = false;
        for (auto &feature : state.feature) if (feature.requested)
        {
            feature.availability = ReconstructionAvailability::Unavailable;
            feature.reason = "Streamline session has shut down.";
        }
    }
    // Plugins are shut down with the native device alive. Keep interposer code
    // loaded until the renderer has released its remaining proxy references.
    state.presentation_fence.Reset();
    state.presentation_fence_value = 0;
    state.swapchain.Reset();
    state.presenting_queue.Reset();
    return Result::Success();
}

Result StreamlineSession::UnloadAfterProxyRelease()
{
    if (!impl_) return Result::Success();
    auto &state = *impl_;
    if (state.owner_thread != GetCurrentThreadId() || state.started || state.swapchain || state.presenting_queue ||
        state.presentation_fence)
        return StreamlineFailure(ErrorCode::InvalidState, "Shutdown Streamline and release every renderer-held proxy before unloading its DLL.");
    state.device.Reset();
    if (state.module && !FreeLibrary(state.module))
        return state.FailAll("Cannot unload the drained Streamline interposer (Win32 " + std::to_string(GetLastError()) + ").");
    state.module = nullptr;
    impl_.reset();
    return Result::Success();
}

ReconstructionAvailability StreamlineSession::Availability(StreamlineFeature feature) const noexcept
{
    return impl_ ? impl_->feature[Index(feature)].availability : ReconstructionAvailability::Unavailable;
}
std::string_view StreamlineSession::FailureReason(StreamlineFeature feature) const noexcept
{
    return impl_ ? std::string_view(impl_->feature[Index(feature)].reason) : kNotStarted;
}
bool StreamlineSession::Initialized() const noexcept { return impl_ && impl_->started; }
bool StreamlineSession::FrameGenerationEnabled() const noexcept
{
    return impl_ && impl_->fg_options.mode != sl::DLSSGMode::eOff;
}
bool StreamlineSession::ReconstructionConfigured() const noexcept
{
    return impl_ && impl_->reconstruction_configured;
}

#else
Result StreamlineSession::InitializeBeforeDevice(StreamlineFeatures features)
{
    if (!features.super_resolution && !features.frame_generation && !features.reflex)
        return Result::Success();
    return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded));
}
Result StreamlineSession::BindDevice(ID3D12Device *) { return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::UpgradeDevice(void **) { return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
void StreamlineSession::ReportPresentationBootstrapFailure(std::string_view) {}
Result StreamlineSession::UpgradeFactory(void **) { return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::UpgradeSwapChain(void **) { return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::GetNativeInterface(void *, void **) { return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::AttachPresentation(IDXGISwapChain4 *, ID3D12CommandQueue *)
{ return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::QueryInputExtent(const GraphicsModeId &, ViewExtent, StreamlineInputExtent &)
{ return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::ConfigureReconstruction(const GraphicsModeId &, ViewExtent, std::uint32_t)
{ return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::ExecuteSuperResolution(ID3D12GraphicsCommandList *, const SdkReconstructionFrame &)
{ return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
std::optional<std::uint64_t> StreamlineSession::NativeFrameToken(SdkFrameIdentity) const noexcept { return std::nullopt; }
Result StreamlineSession::BeginFrame(SdkFrameIdentity) { return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::InputSleep(SdkFrameIdentity) { return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::Marker(SdkFrameIdentity, SdkLatencyMarker) { return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::ConfigureReflex(bool, bool, std::uint32_t) { return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::QueryReflexState(StreamlineReflexState &) { return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::QueryFrameGenerationCapabilities(StreamlineFrameGenerationCapabilities &)
{ return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::ConfigureFrameGeneration(GraphicsFrameGenerationMode, std::uint32_t, float, ViewExtent)
{ return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::PreparePresent(const SdkPresentationFrame &) { return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::PresentCompleted(SdkFrameIdentity, std::uint32_t *, RendererStreamlinePresentCount *observation)
{ if (observation) *observation = {}; return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::WaitForPresentationInputs(ID3D12CommandQueue *) { return Result::Success(); }
Result StreamlineSession::DrainPresentation() { return Result::Success(); }
Result StreamlineSession::SelectReconstructionViewAfterCompletion(std::uint32_t) { return Result::Success(); }
Result StreamlineSession::ReleaseReconstructionAfterCompletion() { return Result::Success(); }
Result StreamlineSession::DetachPresentationAfterCompletion() { return Result::Success(); }
Result StreamlineSession::ShutdownAfterCompletion() { return Result::Success(); }
Result StreamlineSession::UnloadAfterProxyRelease() { return Result::Success(); }
ReconstructionAvailability StreamlineSession::Availability(StreamlineFeature) const noexcept
{ return ReconstructionAvailability::CompiledOut; }
std::string_view StreamlineSession::FailureReason(StreamlineFeature) const noexcept { return kExcluded; }
bool StreamlineSession::Initialized() const noexcept { return false; }
bool StreamlineSession::FrameGenerationEnabled() const noexcept { return false; }
bool StreamlineSession::ReconstructionConfigured() const noexcept { return false; }
#endif
} // namespace hs
