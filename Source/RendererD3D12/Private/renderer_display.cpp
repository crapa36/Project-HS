#include "renderer_display.hpp"
#include "renderer_diagnostics.hpp"

#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string_view>
#include <utility>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;

std::string NativeOutputId(const LUID &luid, std::wstring_view device_name)
{
    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        device_name.data(), static_cast<int>(device_name.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string name(static_cast<std::size_t>(bytes), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            device_name.data(), static_cast<int>(device_name.size()), name.data(), bytes,
            nullptr, nullptr))
        return {};
    return std::format("dxgi:{:08x}{:08x}:{}", static_cast<std::uint32_t>(luid.HighPart),
                        luid.LowPart, name);
}

void UseSdr(DisplayState &state, DisplayUnavailableReason reason,
            DisplayAvailability availability)
{
    state.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    state.color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    state.hdr_active = false;
    state.availability = availability;
    state.reason = reason;
}
} // namespace

Result QueryDisplayState(IDXGIAdapter *adapter, HWND window,
                         const RendererConfig &config, DisplayState &state)
{
    if (!adapter || !window ||
        (config.hdr_mode != GraphicsHdrMode::Off && config.hdr_mode != GraphicsHdrMode::On &&
         config.hdr_mode != GraphicsHdrMode::Auto))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Display query requires an adapter, window, and valid HDR mode.");
    if (!std::isfinite(config.hdr_paper_white_nits) || config.hdr_paper_white_nits < 80.0f ||
        config.hdr_paper_white_nits > 400.0f ||
        (config.hdr_peak_nits && (!std::isfinite(*config.hdr_peak_nits) ||
            *config.hdr_peak_nits < 80.0f || *config.hdr_peak_nits > 10000.0f ||
            *config.hdr_peak_nits < config.hdr_paper_white_nits)))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "HDR paper white/peak must satisfy the graphics settings ranges.");
    if (config.display_output.value.empty())
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Display output ID cannot be empty.");

    const bool automatic = config.display_output.value == "auto";
    const auto window_monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    ComPtr<IDXGIFactory1> factory;
    auto queried = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(queried)) return HResultFailure("Query display adapter factory", queried);
    ComPtr<IDXGIOutput> selected;
    DXGI_OUTPUT_DESC selected_description{};
    std::string selected_id;
    // Outputs can belong to a different adapter than the rendering device on a
    // hybrid system. Select by actual monitor/ID, not by adapter output ordinal.
    for (UINT adapter_index = 0; !selected; ++adapter_index)
    {
        ComPtr<IDXGIAdapter1> output_adapter;
        queried = factory->EnumAdapters1(adapter_index, &output_adapter);
        if (queried == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(queried)) return HResultFailure("Enumerate display adapters", queried);
        DXGI_ADAPTER_DESC1 adapter_description{};
        queried = output_adapter->GetDesc1(&adapter_description);
        if (FAILED(queried)) return HResultFailure("Query display adapter ID", queried);
        for (UINT output_index = 0; ; ++output_index)
        {
            ComPtr<IDXGIOutput> output;
            queried = output_adapter->EnumOutputs(output_index, &output);
            if (queried == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(queried)) return HResultFailure("Enumerate display outputs", queried);
            DXGI_OUTPUT_DESC description{};
            queried = output->GetDesc(&description);
            if (FAILED(queried)) return HResultFailure("Query display output", queried);
            auto id = NativeOutputId(adapter_description.AdapterLuid, description.DeviceName);
            if (id.empty())
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "Display output device name is not valid UTF-16.");
            if (!(automatic ? description.Monitor == window_monitor
                            : id == config.display_output.value))
                continue;
            if (!description.AttachedToDesktop)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "Selected display output is not attached to the desktop.");
            selected = std::move(output);
            selected_description = description;
            selected_id = std::move(id);
            break;
        }
    }
    if (!selected && !automatic)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Unknown display output ID: " + config.display_output.value);

    DisplayState candidate;
    candidate.paper_white_nits = config.hdr_paper_white_nits;
    if (config.hdr_peak_nits) candidate.peak_nits = *config.hdr_peak_nits;
    const auto unavailable = config.hdr_mode == GraphicsHdrMode::On
        ? DisplayAvailability::Unsupported : DisplayAvailability::Available;
    if (!selected)
    {
        candidate.monitor = window_monitor;
        MONITORINFO monitor_info{sizeof(MONITORINFO)};
        if (window_monitor && GetMonitorInfoW(window_monitor, &monitor_info))
            candidate.output_rect = monitor_info.rcMonitor;
        UseSdr(candidate, DisplayUnavailableReason::NoOutput, unavailable);
        state = std::move(candidate);
        return Result::Success();
    }
    candidate.native_output_id = std::move(selected_id);
    candidate.output_rect = selected_description.DesktopCoordinates;
    candidate.monitor = selected_description.Monitor;
    if (config.hdr_mode == GraphicsHdrMode::Off)
    {
        state = std::move(candidate);
        return Result::Success();
    }
    ComPtr<IDXGIOutput6> output6;
    queried = selected.As(&output6);
    if (queried == E_NOINTERFACE)
    {
        UseSdr(candidate, DisplayUnavailableReason::ColorInfoUnavailable, unavailable);
        state = std::move(candidate);
        return Result::Success();
    }
    if (FAILED(queried)) return HResultFailure("Query IDXGIOutput6", queried);
    DXGI_OUTPUT_DESC1 description{};
    queried = output6->GetDesc1(&description);
    if (FAILED(queried)) return HResultFailure("Query display color capabilities", queried);
    if (description.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
    {
        UseSdr(candidate, DisplayUnavailableReason::HdrInactive, unavailable);
        state = std::move(candidate);
        return Result::Success();
    }
    if (!config.hdr_peak_nits)
    {
        // Auto derives its luminance ceiling from the actual output. Invalid
        // output data cannot turn a requested HDR path into a calibrated one.
        if (!std::isfinite(description.MaxLuminance) || description.MaxLuminance < 80.0f ||
            description.MaxLuminance > 10000.0f)
        {
            UseSdr(candidate, DisplayUnavailableReason::InvalidLuminance, unavailable);
            state = std::move(candidate);
            return Result::Success();
        }
        candidate.peak_nits = description.MaxLuminance;
    }
    candidate.paper_white_nits = std::min(candidate.paper_white_nits, candidate.peak_nits);
    if (std::isfinite(description.MinLuminance) && description.MinLuminance >= 0.0f)
        candidate.min_luminance_nits = std::min(description.MinLuminance, candidate.peak_nits);
    if (std::isfinite(description.MaxFullFrameLuminance) && description.MaxFullFrameLuminance > 0.0f)
        candidate.max_full_frame_nits = std::min(description.MaxFullFrameLuminance, candidate.peak_nits);
    candidate.format = DXGI_FORMAT_R10G10B10A2_UNORM;
    candidate.color_space = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    candidate.hdr_active = true;
    state = std::move(candidate);
    return Result::Success();
}

Result ApplyDisplayState(IDXGISwapChain4 *swap_chain, DisplayState &state)
{
    if (!swap_chain)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Display application requires a swap chain.");
    if (state.monitor && !state.native_output_id.empty())
    {
        ComPtr<IDXGIOutput> containing;
        auto queried = swap_chain->GetContainingOutput(&containing);
        if (FAILED(queried)) return HResultFailure("Query swap-chain output", queried);
        DXGI_OUTPUT_DESC description{};
        queried = containing->GetDesc(&description);
        if (FAILED(queried)) return HResultFailure("Query swap-chain output monitor", queried);
        if (description.Monitor != state.monitor)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Window must be placed on the selected display output before applying display state.");
    }
    UINT support{};
    auto applied = swap_chain->CheckColorSpaceSupport(state.color_space, &support);
    if (FAILED(applied)) return HResultFailure("Check swap-chain color space", applied);
    if (!(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
    {
        if (!state.hdr_active)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Swap chain cannot present the required SDR color space.");
        UseSdr(state, DisplayUnavailableReason::ColorSpaceUnsupported,
               DisplayAvailability::Unsupported);
        applied = swap_chain->CheckColorSpaceSupport(state.color_space, &support);
        if (FAILED(applied)) return HResultFailure("Check SDR swap-chain color space", applied);
        if (!(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Swap chain cannot present the SDR fallback color space.");
    }
    applied = swap_chain->SetColorSpace1(state.color_space);
    if (FAILED(applied)) return HResultFailure("Set swap-chain color space", applied);
    if (!state.hdr_active)
    {
        applied = swap_chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr);
        return SUCCEEDED(applied) ? Result::Success() : HResultFailure("Clear swap-chain HDR metadata", applied);
    }

    // Rec.2020/D65 coordinates are encoded in 1/50000 units. DXGI's maximum
    // mastering luminance is whole nits; its minimum is in 1/10000 nit units.
    DXGI_HDR_METADATA_HDR10 metadata{};
    metadata.RedPrimary[0] = 35400; metadata.RedPrimary[1] = 14600;
    metadata.GreenPrimary[0] = 8500; metadata.GreenPrimary[1] = 39850;
    metadata.BluePrimary[0] = 6550; metadata.BluePrimary[1] = 2300;
    metadata.WhitePoint[0] = 15635; metadata.WhitePoint[1] = 16450;
    metadata.MaxMasteringLuminance = static_cast<UINT>(std::lround(state.peak_nits));
    metadata.MinMasteringLuminance = static_cast<UINT>(std::lround(state.min_luminance_nits * 10000.0f));
    // The tone mapper bounds every pixel by peak_nits. Without measured content
    // statistics the same bound is conservative for both MaxCLL and MaxFALL;
    // the panel's full-frame capability is not a measured content MaxFALL.
    metadata.MaxContentLightLevel = static_cast<UINT16>(std::ceil(state.peak_nits));
    metadata.MaxFrameAverageLightLevel = metadata.MaxContentLightLevel;
    applied = swap_chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(metadata), &metadata);
    return SUCCEEDED(applied) ? Result::Success() : HResultFailure("Set swap-chain HDR10 metadata", applied);
}
} // namespace hs
