#include "renderer_xess.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>

#ifndef HS_WITH_XESS
#define HS_WITH_XESS 0
#endif

#if HS_WITH_XESS
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <xess/xess_d3d12.h>
#endif

namespace hs
{
namespace
{
Result XessFailure(ErrorCode code, std::string message)
{
    return Result::Failure(code, "hs_renderer_xess", std::move(message));
}

#if HS_WITH_XESS
bool QualitySetting(std::string_view mode, xess_quality_settings_t &quality)
{
    if (mode == "native") quality = XESS_QUALITY_SETTING_AA;
    else if (mode == "ultra_quality_plus") quality = XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS;
    else if (mode == "ultra_quality") quality = XESS_QUALITY_SETTING_ULTRA_QUALITY;
    else if (mode == "quality") quality = XESS_QUALITY_SETTING_QUALITY;
    else if (mode == "balanced") quality = XESS_QUALITY_SETTING_BALANCED;
    else if (mode == "performance") quality = XESS_QUALITY_SETTING_PERFORMANCE;
    else if (mode == "ultra_performance") quality = XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
    else return false;
    return true;
}

std::string NativeFailure(const char *operation, xess_result_t result)
{
    return std::string(operation) + " failed with XeSS result " + std::to_string(static_cast<int>(result)) + ".";
}

bool ValidExtent(ViewExtent extent)
{
    return extent.width != 0 && extent.height != 0;
}

bool FitsRect(const D3D12_RESOURCE_DESC &description, XessRect rect)
{
    return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        description.DepthOrArraySize == 1 && description.SampleDesc.Count == 1 &&
        rect.width != 0 && rect.height != 0 &&
        static_cast<std::uint64_t>(rect.x) + rect.width <= description.Width &&
        static_cast<std::uint64_t>(rect.y) + rect.height <= description.Height;
}

void XessLog(const char *message, xess_logging_level_t level)
{
    if (message) std::fprintf(stderr, "hs.xess.sdk level=%u message=%s\n",
                              static_cast<unsigned>(level), message);
}
#endif
} // namespace

struct XessReconstruction::Impl
{
    ReconstructionAvailability availability{
#if HS_WITH_XESS
        ReconstructionAvailability::Unavailable
#else
        ReconstructionAvailability::CompiledOut
#endif
    };
    std::string failure_reason{
#if HS_WITH_XESS
        "XeSS-SR context has not been initialized."
#else
        "XeSS-SR was explicitly excluded by HS_WITH_XESS=OFF."
#endif
    };
    std::string driver_warning;
    bool initialized{};
#if HS_WITH_XESS
    HMODULE module{};
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    xess_context_handle_t context{};
    DWORD owner_thread{};
    ViewExtent output_extent;
    GraphicsModeId quality;
    XessReconstructionFlags flags;
    XessInputExtent input_extent;
    float motion_scale[2]{};
    bool force_reset{true};
    // decltype uses the official pinned SDK ABI; no local shadow structs or enums.
    decltype(&xessGetVersion) get_version{};
    decltype(&xessD3D12CreateContext) create_context{};
    decltype(&xessD3D12BuildPipelines) build_pipelines{};
    decltype(&xessD3D12Init) initialize{};
    decltype(&xessD3D12Execute) execute{};
    decltype(&xessGetOptimalInputResolution) query_input_extent{};
    decltype(&xessSetJitterScale) set_jitter_scale{};
    decltype(&xessSetVelocityScale) set_velocity_scale{};
    decltype(&xessIsOptimalDriver) query_driver{};
    decltype(&xessSetLoggingCallback) set_logging{};
    decltype(&xessDestroyContext) destroy_context{};

    template <typename Function>
    bool Load(Function &function, const char *name)
    {
        function = reinterpret_cast<Function>(GetProcAddress(module, name));
        if (function) return true;
        failure_reason = std::string("Pinned XeSS-SR DLL is missing export ") + name + ".";
        return false;
    }

    Result RequireOwner() const
    {
        if (owner_thread != GetCurrentThreadId())
            return XessFailure(ErrorCode::InvalidState, "XeSS-SR API called outside its initialization thread.");
        return Result::Success();
    }

    Result Fail(const char *operation, xess_result_t result)
    {
        failure_reason = NativeFailure(operation, result);
        availability = result == XESS_RESULT_ERROR_UNSUPPORTED_DEVICE ||
                       result == XESS_RESULT_ERROR_UNSUPPORTED_DRIVER
            ? ReconstructionAvailability::Unsupported : ReconstructionAvailability::Unavailable;
        return XessFailure(ErrorCode::TaskFailed, failure_reason);
    }
#endif
};

XessReconstruction::XessReconstruction() : impl_(std::make_unique<Impl>()) {}

XessReconstruction::~XessReconstruction()
{
    // Renderer shutdown owns the completion guarantee. Keep the native context,
    // DLL and device owner alive if destruction itself fails; unloading its code
    // while an SDK context still exists is not safe recovery.
    if (auto reset = ResetAfterCompletion(); !reset)
    {
        std::fprintf(stderr, "hs.xess.shutdown error=%s\n", std::string(reset.Message()).c_str());
        static_cast<void>(impl_.release());
    }
}

Result XessReconstruction::ProbeDevice(ID3D12Device *device)
{
#if !HS_WITH_XESS
    static_cast<void>(device);
    return XessFailure(ErrorCode::InvalidState, impl_->failure_reason);
#else
    if (impl_->context || impl_->module)
        return XessFailure(ErrorCode::InvalidState, "Reset the completed XeSS-SR context before initializing it again.");
    if (!device)
        return XessFailure(ErrorCode::InvalidArgument, "XeSS-SR capability query requires the actual device.");

    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size())
        return XessFailure(ErrorCode::TaskFailed, "Cannot resolve the executable directory for the pinned XeSS-SR DLL.");
    executable.resize(length);
    const auto runtime = std::filesystem::path(executable).parent_path() / L"libxess.dll";
    impl_->owner_thread = GetCurrentThreadId();
    impl_->module = LoadLibraryExW(runtime.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!impl_->module)
    {
        const auto load_error = GetLastError();
        impl_->availability = ReconstructionAvailability::Unavailable;
        impl_->failure_reason = "Cannot load pinned XeSS-SR runtime " + runtime.string() +
            " (Win32 " + std::to_string(load_error) + ").";
        return XessFailure(ErrorCode::TaskFailed, impl_->failure_reason);
    }
    if (!impl_->Load(impl_->get_version, "xessGetVersion") ||
        !impl_->Load(impl_->create_context, "xessD3D12CreateContext") ||
        !impl_->Load(impl_->build_pipelines, "xessD3D12BuildPipelines") ||
        !impl_->Load(impl_->initialize, "xessD3D12Init") ||
        !impl_->Load(impl_->execute, "xessD3D12Execute") ||
        !impl_->Load(impl_->query_input_extent, "xessGetOptimalInputResolution") ||
        !impl_->Load(impl_->set_jitter_scale, "xessSetJitterScale") ||
        !impl_->Load(impl_->set_velocity_scale, "xessSetVelocityScale") ||
        !impl_->Load(impl_->query_driver, "xessIsOptimalDriver") ||
        !impl_->Load(impl_->set_logging, "xessSetLoggingCallback") ||
        !impl_->Load(impl_->destroy_context, "xessDestroyContext"))
    {
        if (!FreeLibrary(impl_->module))
            impl_->failure_reason += " FreeLibrary also failed (Win32 " + std::to_string(GetLastError()) + ").";
        else impl_->module = nullptr;
        return XessFailure(ErrorCode::TaskFailed, impl_->failure_reason);
    }
    impl_->device = device;
    auto result = impl_->create_context(device, &impl_->context);
    if (result < XESS_RESULT_SUCCESS) return impl_->Fail("xessD3D12CreateContext", result);
    if (!impl_->context)
        return XessFailure(ErrorCode::TaskFailed, "xessD3D12CreateContext returned success without a context.");
    result = impl_->set_logging(impl_->context, XESS_LOGGING_LEVEL_WARNING, XessLog);
    if (result < XESS_RESULT_SUCCESS) return impl_->Fail("xessSetLoggingCallback", result);
    result = impl_->query_driver(impl_->context);
    if (result < XESS_RESULT_SUCCESS) return impl_->Fail("xessIsOptimalDriver", result);
    if (result == XESS_RESULT_WARNING_OLD_DRIVER)
        impl_->driver_warning = "XeSS-SR reports an older driver; the supported path remains enabled.";
    xess_version_t version{};
    result = impl_->get_version(&version);
    if (result < XESS_RESULT_SUCCESS) return impl_->Fail("xessGetVersion", result);
    std::fprintf(stderr, "hs.xess.runtime version=%u.%u.%u\n",
                 static_cast<unsigned>(version.major), static_cast<unsigned>(version.minor),
                 static_cast<unsigned>(version.patch));
    impl_->availability = ReconstructionAvailability::Available;
    impl_->failure_reason.clear();
    return Result::Success();
#endif
}

Result XessReconstruction::Initialize(ID3D12Device *device, ViewExtent output,
                                     const GraphicsModeId &quality,
                                     XessReconstructionFlags flags)
{
#if !HS_WITH_XESS
    static_cast<void>(device); static_cast<void>(output); static_cast<void>(quality); static_cast<void>(flags);
    return XessFailure(ErrorCode::InvalidState, impl_->failure_reason);
#else
    xess_quality_settings_t quality_setting{};
    if (!ValidExtent(output) || !QualitySetting(quality.value, quality_setting))
        return XessFailure(ErrorCode::InvalidArgument, "Invalid XeSS-SR output extent or quality mode.");
    if (auto probed = ProbeDevice(device); !probed) return probed;

    XessInputExtent range;
    if (auto queried = QueryInputExtent(quality, output, range); !queried) return queried;
    std::uint32_t native_flags = XESS_INIT_FLAG_NONE;
    if (flags.inverted_depth) native_flags |= XESS_INIT_FLAG_INVERTED_DEPTH;
    if (flags.responsive_pixel_mask) native_flags |= XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK;
    if (flags.exposure_scale_texture) native_flags |= XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE;
    // Low-res unjittered MV, explicit exposure, SDK-owned descriptor heap.
    // Pipeline build and initialization use exactly the same native flags.
    auto result = impl_->build_pipelines(impl_->context, nullptr, true, native_flags);
    if (result < XESS_RESULT_SUCCESS) return impl_->Fail("xessD3D12BuildPipelines", result);
    xess_d3d12_init_params_t initialization{};
    initialization.outputResolution = {output.width, output.height};
    initialization.qualitySetting = quality_setting;
    initialization.initFlags = native_flags;
    initialization.creationNodeMask = 1;
    initialization.visibleNodeMask = 1;
    result = impl_->initialize(impl_->context, &initialization);
    if (result < XESS_RESULT_SUCCESS) return impl_->Fail("xessD3D12Init", result);
    result = impl_->set_jitter_scale(impl_->context, 1.0f, 1.0f);
    if (result < XESS_RESULT_SUCCESS) return impl_->Fail("xessSetJitterScale", result);
    impl_->output_extent = output;
    impl_->quality = quality;
    impl_->flags = flags;
    impl_->input_extent = range;
    impl_->initialized = true;
    impl_->force_reset = true;
    impl_->availability = ReconstructionAvailability::Available;
    impl_->failure_reason.clear();
    return Result::Success();
#endif
}

Result XessReconstruction::QueryInputExtent(const GraphicsModeId &quality, ViewExtent output,
                                          XessInputExtent &extent)
{
#if !HS_WITH_XESS
    static_cast<void>(quality); static_cast<void>(output); static_cast<void>(extent);
    return XessFailure(ErrorCode::InvalidState, impl_->failure_reason);
#else
    if (!impl_->context)
        return XessFailure(ErrorCode::InvalidState, "XeSS-SR input extent query requires a created context.");
    if (auto owner = impl_->RequireOwner(); !owner) return owner;
    xess_quality_settings_t quality_setting{};
    if (!ValidExtent(output) || !QualitySetting(quality.value, quality_setting))
        return XessFailure(ErrorCode::InvalidArgument, "Invalid XeSS-SR output extent or quality mode.");
    const xess_2d_t target{output.width, output.height};
    xess_2d_t optimal{}, minimum{}, maximum{};
    const auto result = impl_->query_input_extent(impl_->context, &target, quality_setting,
                                                &optimal, &minimum, &maximum);
    if (result < XESS_RESULT_SUCCESS) return impl_->Fail("xessGetOptimalInputResolution", result);
    const XessInputExtent candidate{{optimal.x, optimal.y}, {minimum.x, minimum.y}, {maximum.x, maximum.y}};
    if (!ValidExtent(candidate.minimum) || !ValidExtent(candidate.optimal) || !ValidExtent(candidate.maximum) ||
        candidate.minimum.width > candidate.optimal.width || candidate.optimal.width > candidate.maximum.width ||
        candidate.minimum.height > candidate.optimal.height || candidate.optimal.height > candidate.maximum.height)
        return XessFailure(ErrorCode::TaskFailed, "XeSS-SR returned an invalid input extent range.");
    extent = candidate;
    return Result::Success();
#endif
}

Result XessReconstruction::Execute(ID3D12GraphicsCommandList *commands,
                                  const XessReconstructionInputs &inputs)
{
#if !HS_WITH_XESS
    static_cast<void>(commands); static_cast<void>(inputs);
    return XessFailure(ErrorCode::InvalidState, impl_->failure_reason);
#else
    if (!impl_->initialized)
        return XessFailure(ErrorCode::InvalidState, "XeSS-SR Execute requires successful initialization.");
    if (auto owner = impl_->RequireOwner(); !owner) return owner;
    if (!commands || commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        !inputs.pre_exposed_color || !inputs.depth || !inputs.motion || !inputs.output ||
        (impl_->flags.responsive_pixel_mask && !inputs.responsive) ||
        (impl_->flags.exposure_scale_texture && !inputs.exposure_scale_texture))
        return XessFailure(ErrorCode::InvalidArgument, "XeSS-SR Execute requires a Direct list and all initialized guides.");
    if (inputs.pre_exposed_color == inputs.output || inputs.depth == inputs.output ||
        inputs.motion == inputs.output || inputs.responsive == inputs.output ||
        inputs.exposure_scale_texture == inputs.output)
        return XessFailure(ErrorCode::InvalidArgument, "XeSS-SR output must not alias an input resource.");
    for (unsigned axis = 0; axis != 2; ++axis)
        if (!std::isfinite(inputs.jitter_pixels[axis]) || std::abs(inputs.jitter_pixels[axis]) > 0.5f ||
            !std::isfinite(inputs.motion_scale[axis]) || inputs.motion_scale[axis] <= 0.0f)
            return XessFailure(ErrorCode::InvalidArgument, "XeSS-SR jitter or motion scale is invalid.");
    if (!impl_->flags.exposure_scale_texture &&
        (!std::isfinite(inputs.exposure_scale) || inputs.exposure_scale <= 0.0f))
        return XessFailure(ErrorCode::InvalidArgument, "XeSS-SR exposure scale must be positive and finite.");
    if (inputs.output_rect.width != impl_->output_extent.width ||
        inputs.output_rect.height != impl_->output_extent.height ||
        inputs.active_rect.width < impl_->input_extent.minimum.width ||
        inputs.active_rect.width > impl_->input_extent.maximum.width ||
        inputs.active_rect.height < impl_->input_extent.minimum.height ||
        inputs.active_rect.height > impl_->input_extent.maximum.height)
        return XessFailure(ErrorCode::InvalidArgument, "XeSS-SR active/output extent is outside its queried context range.");
    const auto color = inputs.pre_exposed_color->GetDesc();
    const auto depth = inputs.depth->GetDesc();
    const auto motion = inputs.motion->GetDesc();
    const auto output = inputs.output->GetDesc();
    if (!FitsRect(color, inputs.active_rect) || color.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        !FitsRect(depth, inputs.active_rect) ||
        (depth.Format != DXGI_FORMAT_R32_TYPELESS && depth.Format != DXGI_FORMAT_D32_FLOAT && depth.Format != DXGI_FORMAT_R32_FLOAT) ||
        !FitsRect(motion, inputs.active_rect) || motion.Format != DXGI_FORMAT_R16G16_FLOAT ||
        !FitsRect(output, inputs.output_rect) || output.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        !(output.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        return XessFailure(ErrorCode::InvalidArgument, "XeSS-SR input/output resource format, capacity or UAV flag is invalid.");
    if (inputs.responsive && impl_->flags.responsive_pixel_mask)
    {
        const auto responsive = inputs.responsive->GetDesc();
        if (!FitsRect(responsive, inputs.active_rect) ||
            (responsive.Format != DXGI_FORMAT_R8_UNORM && responsive.Format != DXGI_FORMAT_R16_FLOAT &&
             responsive.Format != DXGI_FORMAT_R32_FLOAT))
            return XessFailure(ErrorCode::InvalidArgument, "XeSS-SR responsive mask format or active rectangle is invalid.");
    }
    if (impl_->flags.exposure_scale_texture)
    {
        const auto exposure = inputs.exposure_scale_texture->GetDesc();
        if (!FitsRect(exposure, {0, 0, 1, 1}) || exposure.Width != 1 || exposure.Height != 1 ||
            exposure.Format != DXGI_FORMAT_R32_FLOAT)
            return XessFailure(ErrorCode::InvalidArgument, "XeSS-SR exposure texture must be a 1x1 R32_FLOAT texture.");
    }
    if (impl_->motion_scale[0] != inputs.motion_scale[0] || impl_->motion_scale[1] != inputs.motion_scale[1])
    {
        const auto scaled = impl_->set_velocity_scale(impl_->context, inputs.motion_scale[0], inputs.motion_scale[1]);
        if (scaled < XESS_RESULT_SUCCESS) return impl_->Fail("xessSetVelocityScale", scaled);
        impl_->motion_scale[0] = inputs.motion_scale[0];
        impl_->motion_scale[1] = inputs.motion_scale[1];
    }
    xess_d3d12_execute_params_t execution{};
    execution.pColorTexture = inputs.pre_exposed_color;
    execution.pDepthTexture = inputs.depth;
    execution.pVelocityTexture = inputs.motion;
    execution.pResponsivePixelMaskTexture = impl_->flags.responsive_pixel_mask ? inputs.responsive : nullptr;
    execution.pExposureScaleTexture = impl_->flags.exposure_scale_texture ? inputs.exposure_scale_texture : nullptr;
    execution.pOutputTexture = inputs.output;
    execution.jitterOffsetX = inputs.jitter_pixels[0];
    execution.jitterOffsetY = inputs.jitter_pixels[1];
    execution.exposureScale = inputs.exposure_scale;
    execution.resetHistory = inputs.reset_history || impl_->force_reset;
    execution.inputWidth = inputs.active_rect.width;
    execution.inputHeight = inputs.active_rect.height;
    execution.inputColorBase = {inputs.active_rect.x, inputs.active_rect.y};
    execution.inputDepthBase = execution.inputColorBase;
    execution.inputMotionVectorBase = execution.inputColorBase;
    execution.inputResponsiveMaskBase = execution.inputColorBase;
    execution.outputColorBase = {inputs.output_rect.x, inputs.output_rect.y};
    const auto result = impl_->execute(impl_->context, commands, &execution);
    if (result < XESS_RESULT_SUCCESS) return impl_->Fail("xessD3D12Execute", result);
    impl_->force_reset = false;
    return Result::Success();
#endif
}

Result XessReconstruction::ResetAfterCompletion()
{
#if HS_WITH_XESS
    if (impl_->context || impl_->module)
    {
        if (auto owner = impl_->RequireOwner(); !owner) return owner;
        if (impl_->context)
        {
            const auto destroyed = impl_->destroy_context(impl_->context);
            if (destroyed < XESS_RESULT_SUCCESS) return impl_->Fail("xessDestroyContext", destroyed);
            impl_->context = nullptr;
        }
        if (impl_->module && !FreeLibrary(impl_->module))
        {
            impl_->initialized = false;
            impl_->availability = ReconstructionAvailability::Unavailable;
            impl_->failure_reason = "FreeLibrary for XeSS-SR failed (Win32 " + std::to_string(GetLastError()) + ").";
            return XessFailure(ErrorCode::TaskFailed, impl_->failure_reason);
        }
        impl_->module = nullptr;
        impl_->device.Reset();
        impl_->owner_thread = 0;
    }
    impl_->initialized = false;
    impl_->force_reset = true;
    impl_->motion_scale[0] = impl_->motion_scale[1] = 0.0f;
    impl_->availability = ReconstructionAvailability::Unavailable;
    impl_->failure_reason = "XeSS-SR context has not been initialized.";
    impl_->driver_warning.clear();
#endif
    return Result::Success();
}

ReconstructionAvailability XessReconstruction::Availability() const noexcept { return impl_->availability; }
std::string_view XessReconstruction::FailureReason() const noexcept { return impl_->failure_reason; }
std::string_view XessReconstruction::DriverWarning() const noexcept { return impl_->driver_warning; }
bool XessReconstruction::Initialized() const noexcept { return impl_->initialized; }
} // namespace hs
