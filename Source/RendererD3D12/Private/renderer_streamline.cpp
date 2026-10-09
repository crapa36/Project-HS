#include "renderer_streamline.hpp"

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
#include <sl_dlss_d.h>
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
constexpr std::array<sl::Feature, 4> kNativeFeatures{
    sl::kFeatureDLSS, sl::kFeatureDLSS_RR, sl::kFeatureDLSS_G, sl::kFeatureReflex};
constexpr std::size_t Index(StreamlineFeature feature) { return static_cast<std::size_t>(feature); }
std::atomic<long> gStreamlinePresentError{};

void StreamlineLog(sl::LogType type, const char *message)
{
    if (message) std::fprintf(stderr, "hs.streamline.sdk type=%u message=%s\n", static_cast<unsigned>(type), message);
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
bool ScalarFloat(ID3D12Resource *resource)
{
    return Format(resource, {DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R32_FLOAT});
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
    std::array<FeatureState, 4> feature;
    HMODULE module{};
    DWORD owner_thread{};
    bool started{}, reconstruction_configured{}, reconstruction_evaluated{}, ray_reconstruction{}, force_reset{true};
    bool sr_viewport_owned{}, rr_viewport_owned{}, rr_allocation_owned{};
    bool rr_allocation_tags_pending{}, rr_memory_query_pending{}, rr_memory_cached{};
    std::uint64_t rr_memory_bytes{};
    GraphicsModeId rr_memory_quality;
    ViewExtent rr_memory_output, rr_memory_input;
    bool rr_memory_ray{};
    sl::FrameToken *rr_allocation_frame_token{};
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
    ViewExtent output, rr_input, fg_input;
    StreamlineInputExtent input_range;
    ComPtr<ID3D12Device> device;
    ComPtr<IDXGISwapChain4> swapchain;
    ComPtr<ID3D12CommandQueue> presenting_queue;
    std::wstring plugin_directory;
    sl::DLSSOptions sr_options;
    sl::DLSSDOptions rr_options;
    struct ReconstructionView
    {
        bool reconstruction_configured{};
        bool reconstruction_evaluated{};
        bool sr_viewport_owned{};
        bool rr_viewport_owned{}, rr_allocation_owned{};
        bool rr_allocation_tags_pending{}, rr_memory_query_pending{}, rr_memory_cached{};
        std::uint64_t rr_memory_bytes{};
        GraphicsModeId rr_memory_quality;
        ViewExtent rr_memory_output, rr_memory_input;
        bool rr_memory_ray{};
        sl::FrameToken *rr_allocation_frame_token{};
        bool ray_reconstruction{};
        bool force_reset{true};
        bool has_frame{};
        SdkFrameIdentity identity{};
        sl::FrameToken * frame_token{};
        std::uint64_t constants_frame{std::numeric_limits<std::uint64_t>::max()};
        std::uint64_t evaluated_frame{std::numeric_limits<std::uint64_t>::max()};
        GraphicsModeId quality;
        ViewExtent output;
        ViewExtent rr_input;
        StreamlineInputExtent input_range;
        sl::DLSSOptions sr_options;
        sl::DLSSDOptions rr_options;
    };
    std::map<std::uint32_t, ReconstructionView> reconstruction_views;
    sl::DLSSGOptions fg_options;
    StreamlineFrameGenerationCapabilities fg_capabilities;
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
    PFun_slAllocateResources *allocate_resources{};
    PFun_slFreeResources *free_resources{};
    PFun_slUpgradeInterface *upgrade{};
    PFun_slGetNativeInterface *get_native{};
    PFun_slDLSSGetOptimalSettings *sr_optimal{};
    PFun_slDLSSSetOptions *sr_set_options{};
    PFun_slDLSSDGetOptimalSettings *rr_optimal{};
    PFun_slDLSSDSetOptions *rr_set_options{};
    PFun_slDLSSDGetState *rr_get_state{};
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
    Result ValidateReconstruction(const SdkReconstructionFrame &frame, bool ray)
    {
        if (auto required = RequireFeature(ray ? StreamlineFeature::RayReconstruction : StreamlineFeature::SuperResolution); !required)
            return required;
        if (!reconstruction_configured || ray_reconstruction != ray || frame.identity.viewport_id != viewport)
            return StreamlineFailure(ErrorCode::InvalidState, "Streamline reconstruction call does not match its configured SR/RR path or viewport.");
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
        if (ray && rr_input.width && (frame.active.width != rr_input.width || frame.active.height != rr_input.height))
            return StreamlineFailure(ErrorCode::InvalidState, "DLSS-RR does not support live dynamic input resolution. Drain and reconfigure its resources.");
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
    bool RrMemoryMatchesConfiguration() const noexcept
    {
        return rr_memory_cached && rr_memory_ray == ray_reconstruction &&
            rr_memory_quality == quality && rr_memory_output == output &&
            rr_memory_input == input_range.optimal;
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
        std::fprintf(stderr, "hs.streamline.shutdown error=%s\n", std::string(result.Message()).c_str());
        static_cast<void>(impl_.release());
    }
    else if (impl_)
    {
        // The session cannot inspect all external proxy references. Explicit
        // UnloadAfterProxyRelease is the renderer's proof that they are gone.
        std::fprintf(stderr, "hs.streamline.shutdown error=UnloadAfterProxyRelease was not called; proxy DLL remains loaded\n");
        static_cast<void>(impl_.release());
    }
}

#if HS_WITH_STREAMLINE
Result StreamlineSession::InitializeBeforeDevice(StreamlineFeatures requested)
{
    if (!requested.super_resolution && !requested.ray_reconstruction && !requested.frame_generation && !requested.reflex)
        return Result::Success();
    if (impl_) return StreamlineFailure(ErrorCode::InvalidState, "Shut down the old Streamline session before initializing another.");
    impl_ = std::make_unique<Impl>();
    auto &state = *impl_;
    state.owner_thread = GetCurrentThreadId();
    const std::array<bool, 4> features{requested.super_resolution || requested.ray_reconstruction, requested.ray_reconstruction,
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
        !state.Load(state.evaluate, "slEvaluateFeature") || !state.Load(state.allocate_resources, "slAllocateResources") ||
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
    std::fprintf(stderr, "hs.streamline.runtime version=2.14.1 commit=2122257e0fce486f91b385aa63b9a09b0a34b363 ota=off\n");
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
    if (Availability(StreamlineFeature::RayReconstruction) == ReconstructionAvailability::Available)
    {
        static_cast<void>(load(StreamlineFeature::RayReconstruction, state.rr_optimal, "slDLSSDGetOptimalSettings"));
        static_cast<void>(load(StreamlineFeature::RayReconstruction, state.rr_set_options, "slDLSSDSetOptions"));
        static_cast<void>(load(StreamlineFeature::RayReconstruction, state.rr_get_state, "slDLSSDGetState"));
        if (Availability(StreamlineFeature::SuperResolution) != ReconstructionAvailability::Available)
            static_cast<void>(state.Fail(StreamlineFeature::RayReconstruction, "DLSS-RR requires compatible DLSS options", sl::Result::eErrorFeatureMissingDependency));
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

Result StreamlineSession::QueryInputExtent(const GraphicsModeId &quality, ViewExtent output, bool ray,
                                         StreamlineInputExtent &range)
{
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    auto &state = *impl_;
    const auto selected = ray ? StreamlineFeature::RayReconstruction : StreamlineFeature::SuperResolution;
    if (auto required = state.RequireFeature(selected); !required) return required;
    sl::DLSSMode mode;
    if (!output.width || !output.height || !Quality(quality.value, mode))
        return StreamlineFailure(ErrorCode::InvalidArgument, "Invalid DLSS output extent or unsupported quality ID: " + quality.value);
    sl::Result native;
    StreamlineInputExtent candidate;
    if (ray)
    {
        sl::DLSSDOptions options;
        options.mode = mode; options.outputWidth = output.width; options.outputHeight = output.height;
        sl::DLSSDOptimalSettings settings;
        native = state.rr_optimal(options, settings);
        candidate = {{settings.optimalRenderWidth, settings.optimalRenderHeight}, {settings.renderWidthMin, settings.renderHeightMin},
            {settings.renderWidthMax, settings.renderHeightMax}};
    }
    else
    {
        sl::DLSSOptions options;
        options.mode = mode; options.outputWidth = output.width; options.outputHeight = output.height;
        sl::DLSSOptimalSettings settings;
        native = state.sr_optimal(options, settings);
        candidate = {{settings.optimalRenderWidth, settings.optimalRenderHeight}, {settings.renderWidthMin, settings.renderHeightMin},
            {settings.renderWidthMax, settings.renderHeightMax}};
    }
    if (native != sl::Result::eOk) return CandidateFailure(ray ? "slDLSSDGetOptimalSettings" : "slDLSSGetOptimalSettings", native);
    if (!candidate.optimal.width || !candidate.optimal.height || !candidate.minimum.width || !candidate.minimum.height ||
        candidate.minimum.width > candidate.optimal.width || candidate.minimum.height > candidate.optimal.height ||
        candidate.maximum.width < candidate.optimal.width || candidate.maximum.height < candidate.optimal.height)
        return StreamlineFailure(ErrorCode::TaskFailed, "Streamline returned invalid optimal/minimum/maximum input extents.");
    range = candidate;
    return Result::Success();
}

Result StreamlineSession::QueryRayReconstructionMemory(std::uint64_t &bytes)
{
    bytes = 0;
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    auto &state = *impl_;
    if (auto r = state.RequireFeature(StreamlineFeature::RayReconstruction); !r) return r;
    if (!state.reconstruction_configured)
        return StreamlineFailure(ErrorCode::InvalidState, "Configure the adopted RR viewport before querying its native memory estimate.");
    if (state.rr_memory_query_pending)
    {
        if (!state.rr_allocation_owned || state.rr_allocation_tags_pending || state.reconstruction_evaluated || !state.rr_get_state)
            return StreamlineFailure(ErrorCode::InvalidState, "DLSS RR memory state is queryable only after a drained native allocation and before evaluation.");
        sl::DLSSDState native;
        const auto result = state.rr_get_state(sl::ViewportHandle(state.viewport), native);
        if (result != sl::Result::eOk) return CandidateFailure("slDLSSDGetState(memory)", result);
        if (!native.estimatedVRAMUsageInBytes)
            return StreamlineFailure(ErrorCode::TaskFailed, "DLSS RR returned no positive native resident memory estimate.");
        state.rr_memory_bytes = native.estimatedVRAMUsageInBytes;
        state.rr_memory_quality = state.quality;
        state.rr_memory_output = state.output;
        state.rr_memory_input = state.input_range.optimal;
        state.rr_memory_ray = state.ray_reconstruction;
        state.rr_memory_cached = true;
        state.rr_memory_query_pending = false;
        bytes = state.rr_memory_bytes;
        return Result::Success();
    }
    if (!state.rr_memory_cached || !state.RrMemoryMatchesConfiguration() ||
        (!state.rr_allocation_owned && !state.rr_viewport_owned))
        return StreamlineFailure(ErrorCode::InvalidState, "DLSS RR memory estimate requires a cached receipt from this owned configured viewport.");
    bytes = state.rr_memory_bytes;
    return Result::Success();
}

Result StreamlineSession::ConfigureReconstruction(const GraphicsModeId &quality, ViewExtent output, bool ray,
                                                std::uint32_t viewport)
{
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    if (auto selected = SelectReconstructionViewAfterCompletion(viewport); !selected) return selected;
    auto &state = *impl_;
    if (state.reconstruction_configured || state.reconstruction_evaluated ||
        state.sr_viewport_owned || state.rr_viewport_owned || state.rr_allocation_owned)
        return StreamlineFailure(ErrorCode::InvalidState, "Drain old DLSS graph uses and release reconstruction before reconfiguration.");
    state.rr_memory_query_pending = false;
    state.rr_memory_cached = false;
    state.rr_memory_bytes = 0;
    state.rr_memory_quality = {};
    state.rr_memory_output = {};
    state.rr_memory_input = {};
    state.rr_memory_ray = false;
    state.rr_allocation_frame_token = nullptr;
    StreamlineInputExtent range;
    if (auto queried = QueryInputExtent(quality, output, ray, range); !queried) return queried;
    sl::DLSSMode mode;
    if (!Quality(quality.value, mode)) return StreamlineFailure(ErrorCode::InvalidArgument, "Unknown DLSS quality.");
    state.sr_options = sl::DLSSOptions{};
    state.sr_options.mode = mode;
    state.sr_options.outputWidth = output.width;
    state.sr_options.outputHeight = output.height;
    state.sr_options.colorBuffersHDR = sl::Boolean::eTrue;
    state.sr_options.useAutoExposure = sl::Boolean::eFalse;
    auto native = state.sr_set_options(sl::ViewportHandle(viewport), state.sr_options);
    if (native != sl::Result::eOk) return CandidateFailure("slDLSSSetOptions", native);
    // SR options have mutated the shared viewport even if the following RR
    // options fail. Make the partial configuration explicitly releasable.
    state.viewport = viewport;
    state.reconstruction_configured = true;
    if (state.rr_set_options)
    {
        state.rr_options = sl::DLSSDOptions{};
        state.rr_options.mode = ray ? mode : sl::DLSSMode::eOff;
        state.rr_options.outputWidth = output.width;
        state.rr_options.outputHeight = output.height;
        state.rr_options.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
        native = state.rr_set_options(sl::ViewportHandle(viewport), state.rr_options);
        if (native != sl::Result::eOk) return CandidateFailure("slDLSSDSetOptions", native);
    }
    state.output = output;
    state.quality = quality;
    state.ray_reconstruction = ray;
    state.viewport = viewport;
    state.input_range = range;
    state.rr_input = {};
    state.force_reset = true;
    state.reconstruction_configured = true;
    return Result::Success();
}

Result StreamlineSession::ExecuteSuperResolution(ID3D12GraphicsCommandList *commands, const SdkReconstructionFrame &frame)
{
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    auto &state = *impl_;
    if (!commands) return StreamlineFailure(ErrorCode::InvalidArgument, "DLSS-SR requires a recording Direct command list.");
    if (auto valid = state.ValidateReconstruction(frame, false); !valid) return valid;
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

Result StreamlineSession::AllocateRayReconstructionResources(ID3D12GraphicsCommandList *commands,
                                                            const SdkReconstructionFrame &metadata,
                                                            bool &created)
{
    created = false;
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    auto &state = *impl_;
    if (!commands) return StreamlineFailure(ErrorCode::InvalidArgument, "DLSS-RR allocation requires a recording Direct command list.");
    if (auto required = state.RequireFeature(StreamlineFeature::RayReconstruction); !required) return required;
    if (!state.reconstruction_configured || !state.ray_reconstruction || !state.rr_set_options || !state.allocate_resources)
        return StreamlineFailure(ErrorCode::InvalidState, "Configure DLSS-RR before allocating its native resources.");
    if (state.rr_allocation_tags_pending)
        return StreamlineFailure(ErrorCode::InvalidState, "Complete the previous DLSS-RR allocation command list before allocating again.");
    const auto optimal = state.input_range.optimal;
    if (!optimal.width || !optimal.height || !state.output.width || !state.output.height ||
        metadata.active.width != optimal.width || metadata.active.height != optimal.height ||
        metadata.output_rect.width != state.output.width || metadata.output_rect.height != state.output.height ||
        !FitsRect(metadata.color, metadata.active) || !FitsRect(metadata.depth, metadata.active) ||
        !FitsRect(metadata.motion, metadata.active) || !FitsRect(metadata.output, metadata.output_rect) ||
        metadata.color == metadata.output ||
        !Format(metadata.color, {DXGI_FORMAT_R16G16B16A16_FLOAT}) ||
        !Format(metadata.output, {DXGI_FORMAT_R16G16B16A16_FLOAT}) ||
        !Format(metadata.depth, {DXGI_FORMAT_R32_FLOAT}) ||
        !Format(metadata.motion, {DXGI_FORMAT_R16G16_FLOAT}) ||
        !(metadata.output->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
        metadata.color_state != D3D12_RESOURCE_STATE_COMMON ||
        metadata.depth_state != D3D12_RESOURCE_STATE_COMMON ||
        metadata.motion_state != D3D12_RESOURCE_STATE_COMMON ||
        metadata.output_state != D3D12_RESOURCE_STATE_COMMON)
        return StreamlineFailure(ErrorCode::InvalidArgument,
            "DLSS-RR allocation requires separate RGBA16F input/output, R32F depth, RG16F motion, optimal extents, and COMMON metadata states.");
    if (state.rr_allocation_owned || state.rr_viewport_owned)
        return Result::Success();

    const std::uint32_t allocation_frame = 0;
    sl::FrameToken *token{};
    auto native = state.get_frame_token(token, &allocation_frame);
    if (native != sl::Result::eOk || !token)
        return StreamlineFailure(ErrorCode::TaskFailed, NativeFailure("slGetNewFrameToken(RR allocation frame 0)", native));
    // Native creation consumes the depth/MV flags, never a rendered camera.
    // Keep unused allocation metadata finite; real evaluation publishes its
    // strict camera constants with the actual rendered-frame token later.
    sl::Constants allocation_constants;
    const auto unused_matrix = NativeMatrix(DirectX::XMFLOAT4X4{});
    allocation_constants.cameraViewToClip = unused_matrix;
    allocation_constants.clipToCameraView = unused_matrix;
    allocation_constants.clipToPrevClip = unused_matrix;
    allocation_constants.prevClipToClip = unused_matrix;
    allocation_constants.jitterOffset = {0, 0};
    allocation_constants.mvecScale = {1, 1};
    allocation_constants.cameraPinholeOffset = {0, 0};
    allocation_constants.cameraPos = allocation_constants.cameraUp =
        allocation_constants.cameraRight = allocation_constants.cameraFwd = sl::float3{0, 0, 0};
    allocation_constants.cameraNear = allocation_constants.cameraFar = allocation_constants.cameraFOV = 0;
    allocation_constants.cameraAspectRatio = float(optimal.width) / float(optimal.height);
    allocation_constants.depthInverted = sl::Boolean::eTrue;
    allocation_constants.cameraMotionIncluded = sl::Boolean::eTrue;
    allocation_constants.motionVectors3D = sl::Boolean::eFalse;
    allocation_constants.reset = sl::Boolean::eTrue;
    allocation_constants.orthographicProjection = sl::Boolean::eFalse;
    allocation_constants.motionVectorsDilated = sl::Boolean::eFalse;
    allocation_constants.motionVectorsJittered = sl::Boolean::eFalse;
    native = state.set_constants(allocation_constants, *token, sl::ViewportHandle(state.viewport));
    if (native != sl::Result::eOk)
        return StreamlineFailure(ErrorCode::TaskFailed, NativeFailure("slSetConstants(RR allocation)", native));

    const sl::ViewportHandle viewport(state.viewport);
    const sl::Extent input{0, 0, optimal.width, optimal.height};
    const sl::Extent output{0, 0, state.output.width, state.output.height};
    std::array<sl::Resource, 4> resources{
        NativeResource(metadata.color, metadata.color_state),
        NativeResource(metadata.output, metadata.output_state),
        NativeResource(metadata.depth, metadata.depth_state),
        NativeResource(metadata.motion, metadata.motion_state)};
    std::array<sl::ResourceTag, 4> tags{
        sl::ResourceTag(&resources[0], sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &input),
        sl::ResourceTag(&resources[1], sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &output),
        sl::ResourceTag(&resources[2], sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &input),
        sl::ResourceTag(&resources[3], sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &input)};
    native = state.set_tags(*token, viewport, tags.data(), static_cast<std::uint32_t>(tags.size()), commands);
    if (native != sl::Result::eOk)
        return state.Fail(StreamlineFeature::RayReconstruction, "slSetTagForFrame(DLSS-RR allocation)", native);
    state.rr_allocation_frame_token = token;
    state.rr_allocation_tags_pending = true;
    // slAllocateResources may record initialization work before reporting a
    // failure. Keep the viewport owned so the caller can drain, clear tags,
    // and perform the normal native rollback path.
    state.rr_allocation_owned = true;
    created = true;
    native = state.allocate_resources(commands, sl::kFeatureDLSS_RR, viewport);
    if (native != sl::Result::eOk)
        return state.Fail(StreamlineFeature::RayReconstruction, "slAllocateResources(DLSS-RR)", native);
    state.rr_memory_query_pending = true;
    return Result::Success();
}

Result StreamlineSession::CompleteRayReconstructionAllocationAfterCompletion()
{
    if (!impl_) return Result::Success();
    auto &state = *impl_;
    if (auto owner = state.RequireOwner(); !owner) return owner;
    if (!state.rr_allocation_tags_pending) return Result::Success();
    if (!state.rr_allocation_frame_token)
        return StreamlineFailure(ErrorCode::InvalidState, "DLSS-RR allocation tags have no retained frame-0 token.");
    const sl::ViewportHandle viewport(state.viewport);
    const std::array tags{
        sl::ResourceTag(nullptr, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(nullptr, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent)};
    const auto native = state.set_tags(*state.rr_allocation_frame_token, viewport, tags.data(),
                                       static_cast<std::uint32_t>(tags.size()), nullptr);
    if (native != sl::Result::eOk)
        return state.Fail(StreamlineFeature::RayReconstruction, "slSetTagForFrame(clear RR allocation)", native);
    state.rr_allocation_tags_pending = false;
    state.rr_allocation_frame_token = nullptr;
    return Result::Success();
}

Result StreamlineSession::ExecuteRayReconstruction(ID3D12GraphicsCommandList *commands, const SdkRayReconstructionFrame &ray)
{
    if (!impl_) return StreamlineFailure(ErrorCode::InvalidState, std::string(kNotStarted));
    auto &state = *impl_;
    const auto &frame = ray.reconstruction;
    if (!commands) return StreamlineFailure(ErrorCode::InvalidArgument, "DLSS-RR requires a recording Direct command list.");
    if (auto valid = state.ValidateReconstruction(frame, true); !valid) return valid;
    if (!FitsRect(ray.diffuse_albedo, frame.active) || !FitsRect(ray.specular_albedo, frame.active) ||
        !FitsRect(ray.normal_roughness, frame.active) || !FitsRect(ray.specular_hit_distance, frame.active) ||
        !FloatColor(ray.diffuse_albedo) || !FloatColor(ray.specular_albedo) || !FloatColor(ray.normal_roughness) ||
        !ScalarFloat(ray.specular_hit_distance) ||
        (ray.diffuse_hit_distance && (!FitsRect(ray.diffuse_hit_distance, frame.active) || !ScalarFloat(ray.diffuse_hit_distance))) ||
        !FiniteMatrix(frame.camera.view) || !FiniteMatrix(frame.camera.inverse_view))
        return StreamlineFailure(ErrorCode::InvalidArgument, "DLSS-RR requires actual noisy color, linear diffuse/specular albedo, normalized world normals + linear roughness, specular hit distance and absolute view transforms.");
    if (!ShaderReadable(ray.diffuse_albedo_state) || !ShaderReadable(ray.specular_albedo_state) ||
        !ShaderReadable(ray.normal_roughness_state) || !ShaderReadable(ray.specular_hit_distance_state) ||
        (ray.diffuse_hit_distance && !ShaderReadable(ray.diffuse_hit_distance_state)))
        return StreamlineFailure(ErrorCode::InvalidArgument, "DLSS-RR explicit guide states must be shader-readable in the graph.");
    for(const auto optional:std::array{
        std::pair{frame.transparency_overlay,frame.transparency_overlay_state},
        std::pair{frame.color_before_transparency,frame.color_before_transparency_state}})
        if(optional.first&&(!FitsRect(optional.first,frame.active)||!FloatColor(optional.first)||
            optional.first->GetDesc().Format!=frame.color->GetDesc().Format||!ShaderReadable(optional.second)))
            return StreamlineFailure(ErrorCode::InvalidArgument,"DLSS-RR transparency tags require actual matching-format input images and readable states.");
    if(frame.sss_luminance&&(!FitsRect(frame.sss_luminance,frame.active)||!ScalarFloat(frame.sss_luminance)||!ShaderReadable(frame.sss_luminance_state)))
        return StreamlineFailure(ErrorCode::InvalidArgument,"DLSS-RR SSS guide requires actual luminosity difference at the input extent.");
    if(frame.transparency_overlay&&frame.transparency_overlay->GetDesc().Format!=DXGI_FORMAT_R16G16B16A16_FLOAT&&
        frame.transparency_overlay->GetDesc().Format!=DXGI_FORMAT_R32G32B32A32_FLOAT)
        return StreamlineFailure(ErrorCode::InvalidArgument,"DLSS-RR premultiplied transparency overlay requires actual RGB and alpha channels.");
    // The noisy integrated ray-traced color is reconstruction.color. Separate
    // diffuse/specular radiance and NRD-specific depth/motion guides do not have
    // that meaning and are never substituted for the RR inputs.
    if (auto begun = BeginFrame(frame.identity); !begun) return begun;
    if (state.evaluated_frame == frame.identity.render_id)
        return StreamlineFailure(ErrorCode::InvalidState, "DLSS-RR replaces SR; this rendered frame was already reconstructed.");
    if (auto constants = state.Constants(frame.camera, frame.active); !constants) return constants;
    state.sr_options.preExposure = frame.input_pre_exposure;
    state.sr_options.exposureScale = frame.exposure_scale;
    auto native = state.sr_set_options(sl::ViewportHandle(state.viewport), state.sr_options);
    if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::SuperResolution, "slDLSSSetOptions(RR companion)", native);
    state.rr_options.preExposure = frame.input_pre_exposure;
    state.rr_options.exposureScale = frame.exposure_scale;
    state.rr_options.worldToCameraView = NativeMatrix(frame.camera.view);
    state.rr_options.cameraViewToWorld = NativeMatrix(frame.camera.inverse_view);
    native = state.rr_set_options(sl::ViewportHandle(state.viewport), state.rr_options);
    if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::RayReconstruction, "slDLSSDSetOptions", native);
    const auto input = NativeRect(frame.active), output = NativeRect(frame.output_rect);
    const sl::Extent exposure_extent{0, 0, 1, 1};
    std::array<sl::Resource, 14> resources;
    std::array<sl::ResourceTag, 14> tags;
    std::uint32_t count{};
    auto add = [&](ID3D12Resource *resource, sl::BufferType type, const sl::Extent &extent,
                   D3D12_RESOURCE_STATES resource_state,
                   sl::ResourceLifecycle lifecycle = sl::ResourceLifecycle::eValidUntilEvaluate)
    {
        resources[count] = NativeResource(resource, resource_state);
        tags[count] = sl::ResourceTag(resource ? &resources[count] : nullptr, type, lifecycle, &extent);
        ++count;
    };
    add(frame.color, sl::kBufferTypeScalingInputColor, input, frame.color_state, sl::ResourceLifecycle::eValidUntilPresent);
    add(frame.output, sl::kBufferTypeScalingOutputColor, output, frame.output_state, sl::ResourceLifecycle::eValidUntilPresent);
    add(frame.depth, sl::kBufferTypeDepth, input, frame.depth_state, sl::ResourceLifecycle::eValidUntilPresent);
    add(frame.motion, sl::kBufferTypeMotionVectors, input, frame.motion_state, sl::ResourceLifecycle::eValidUntilPresent);
    add(frame.exposure, sl::kBufferTypeExposure, exposure_extent, frame.exposure_state);
    add(frame.responsive, sl::kBufferTypeResponsivityMask, input, frame.responsive_state);
    add(ray.diffuse_albedo, sl::kBufferTypeAlbedo, input, ray.diffuse_albedo_state);
    add(ray.specular_albedo, sl::kBufferTypeSpecularAlbedo, input, ray.specular_albedo_state);
    add(ray.normal_roughness, sl::kBufferTypeNormalRoughness, input, ray.normal_roughness_state);
    add(ray.specular_hit_distance, sl::kBufferTypeSpecularHitDistance, input, ray.specular_hit_distance_state);
    add(ray.diffuse_hit_distance, sl::kBufferTypeDiffuseHitDistance, input, ray.diffuse_hit_distance_state);
    add(frame.transparency_overlay,sl::kBufferTypeTransparencyLayer,input,frame.transparency_overlay_state);
    add(frame.color_before_transparency,sl::kBufferTypeColorBeforeTransparency,input,frame.color_before_transparency_state);
    add(frame.sss_luminance,sl::kBufferTypeScreenSpaceSubsurfaceScatteringGuide,input,frame.sss_luminance_state);
    native = state.set_tags(*state.frame_token, sl::ViewportHandle(state.viewport), tags.data(), count, commands);
    if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::RayReconstruction, "slSetTagForFrame(DLSS-RR)", native);
    const sl::ViewportHandle viewport(state.viewport);
    const sl::BaseStructure *inputs[]{&viewport};
    // Evaluation may create the native viewport before reporting a failure;
    // retain ownership through cleanup once this call is entered.
    state.reconstruction_evaluated = true;
    state.rr_viewport_owned = true;
    native = state.evaluate(sl::kFeatureDLSS_RR, *state.frame_token, inputs, 1, commands);
    const auto evaluation = native;
    // Explicit allocation and EvaluateUntilEvaluate tags otherwise leave the
    // four mandatory resources pinned until Present, which makes the provider
    // clone them. The renderer retains the graph owners through GPU completion;
    // release the SDK's Present-lifetime references immediately after recording
    // the synchronous evaluation, including when evaluation reports failure.
    const std::array clear_present_tags{
        sl::ResourceTag(nullptr, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(nullptr, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent)};
    const auto cleared = state.set_tags(*state.frame_token, viewport, clear_present_tags.data(),
                                        static_cast<std::uint32_t>(clear_present_tags.size()), nullptr);
    if (evaluation != sl::Result::eOk)
        return state.Fail(StreamlineFeature::RayReconstruction, "slEvaluateFeature(DLSS-RR)", evaluation);
    if (cleared != sl::Result::eOk)
        return state.Fail(StreamlineFeature::RayReconstruction, "slSetTagForFrame(clear RR inputs)", cleared);
    state.rr_input = {frame.active.width, frame.active.height};
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
        swap(state.rr_viewport_owned, view.rr_viewport_owned);
        swap(state.ray_reconstruction, view.ray_reconstruction);
        swap(state.force_reset, view.force_reset);
        swap(state.has_frame, view.has_frame);
        swap(state.identity, view.identity);
        swap(state.frame_token, view.frame_token);
        swap(state.rr_allocation_frame_token, view.rr_allocation_frame_token);
        swap(state.constants_frame, view.constants_frame);
        swap(state.evaluated_frame, view.evaluated_frame);
        swap(state.quality, view.quality);
        swap(state.output, view.output);
        swap(state.rr_input, view.rr_input);
        swap(state.input_range, view.input_range);
        swap(state.sr_options, view.sr_options);
        swap(state.rr_options, view.rr_options);
        swap(state.rr_allocation_owned, view.rr_allocation_owned);
        swap(state.rr_allocation_tags_pending, view.rr_allocation_tags_pending);
        swap(state.rr_memory_query_pending, view.rr_memory_query_pending);
        swap(state.rr_memory_cached, view.rr_memory_cached);
        swap(state.rr_memory_bytes, view.rr_memory_bytes);
        swap(state.rr_memory_quality, view.rr_memory_quality);
        swap(state.rr_memory_output, view.rr_memory_output);
        swap(state.rr_memory_input, view.rr_memory_input);
        swap(state.rr_memory_ray, view.rr_memory_ray);
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
    if (!state.device || (!state.reconstruction_configured && !state.reconstruction_evaluated &&
                          !state.sr_viewport_owned && !state.rr_viewport_owned && !state.rr_allocation_owned))
        return Result::Success();
    if (state.rr_allocation_tags_pending)
        if (auto cleared = CompleteRayReconstructionAllocationAfterCompletion(); !cleared) return cleared;
    const sl::ViewportHandle viewport(state.viewport);
    if (state.rr_set_options)
    {
        auto options = state.rr_options;
        options.mode = sl::DLSSMode::eOff;
        auto native = state.rr_set_options(viewport, options);
        if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::RayReconstruction, "slDLSSDSetOptions(Off)", native);
    }
    if (state.rr_viewport_owned || state.rr_allocation_owned)
    {
        const auto native = state.free_resources(sl::kFeatureDLSS_RR, viewport);
        if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::RayReconstruction, "slFreeResources(DLSS-RR)", native);
        state.rr_viewport_owned = false;
        state.rr_allocation_owned = false;
        state.rr_memory_query_pending = false;
        state.rr_memory_cached = false;
        state.rr_memory_bytes = 0;
        state.rr_memory_quality = {};
        state.rr_memory_output = {};
        state.rr_memory_input = {};
        state.rr_memory_ray = false;
    }
    if (state.sr_set_options)
    {
        auto options = state.sr_options;
        options.mode = sl::DLSSMode::eOff;
        auto native = state.sr_set_options(viewport, options);
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
    state.rr_input = {};
    state.rr_memory_query_pending = false;
    state.rr_memory_cached = false;
    state.rr_memory_bytes = 0;
    state.rr_memory_quality = {};
    state.rr_memory_output = {};
    state.rr_memory_input = {};
    state.rr_memory_ray = false;
    state.rr_allocation_frame_token = nullptr;
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
    if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::FrameGeneration, "slDLSSGGetState", native);
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

Result StreamlineSession::PresentCompleted(SdkFrameIdentity identity, std::uint32_t *frames_actually_presented)
{
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
    if (native != sl::Result::eOk) return state.Fail(StreamlineFeature::FrameGeneration, "slDLSSGGetState(after Present)", native);
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
    if (!features.super_resolution && !features.ray_reconstruction && !features.frame_generation && !features.reflex)
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
Result StreamlineSession::QueryInputExtent(const GraphicsModeId &, ViewExtent, bool, StreamlineInputExtent &)
{ return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::ConfigureReconstruction(const GraphicsModeId &, ViewExtent, bool, std::uint32_t)
{ return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::ExecuteSuperResolution(ID3D12GraphicsCommandList *, const SdkReconstructionFrame &)
{ return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::AllocateRayReconstructionResources(ID3D12GraphicsCommandList *, const SdkReconstructionFrame &, bool &created)
{ created = false; return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::CompleteRayReconstructionAllocationAfterCompletion()
{ return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::ExecuteRayReconstruction(ID3D12GraphicsCommandList *, const SdkRayReconstructionFrame &)
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
Result StreamlineSession::PresentCompleted(SdkFrameIdentity, std::uint32_t *)
{ return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
Result StreamlineSession::WaitForPresentationInputs(ID3D12CommandQueue *) { return Result::Success(); }
Result StreamlineSession::DrainPresentation() { return Result::Success(); }
Result StreamlineSession::SelectReconstructionViewAfterCompletion(std::uint32_t) { return Result::Success(); }
Result StreamlineSession::ReleaseReconstructionAfterCompletion() { return Result::Success(); }
Result StreamlineSession::QueryRayReconstructionMemory(std::uint64_t &bytes) { bytes = 0; return StreamlineFailure(ErrorCode::InvalidState, std::string(kExcluded)); }
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
