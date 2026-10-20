#pragma once

#include <hs/renderer/renderer.hpp>

#include <Windows.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <string>

namespace hs
{
enum class DisplayAvailability : std::uint8_t { Available, Unsupported };
enum class DisplayUnavailableReason : std::uint8_t
{
    None, NoOutput, ColorInfoUnavailable, HdrInactive, InvalidLuminance,
    ColorSpaceUnsupported
};

// Effective native output state. RendererConfig retains the requested HDR mode
// and display ID even when the actual output can only use SDR.
struct DisplayState
{
    DXGI_FORMAT format{DXGI_FORMAT_R8G8B8A8_UNORM};
    DXGI_COLOR_SPACE_TYPE color_space{DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
    bool hdr_active{};
    float paper_white_nits{200.0f};
    float peak_nits{1000.0f};
    float min_luminance_nits{};
    float max_full_frame_nits{};
    std::string native_output_id;
    RECT output_rect{};
    HMONITOR monitor{};
    DisplayAvailability availability{DisplayAvailability::Available};
    DisplayUnavailableReason reason{DisplayUnavailableReason::None};
};

// IDs use dxgi:<adapter LUID high/low as 16 hex digits>:<UTF-8 DeviceName>.
// Auto chooses the output under HWND, including cross-adapter presentation.
// These functions never change the operating system's HDR mode or move a window.
[[nodiscard]] Result QueryDisplayState(IDXGIAdapter *adapter, HWND window,
                                        const RendererConfig &config, DisplayState &state);

// Negotiate before publishing back buffers, RTVs, or format-dependent pipelines.
// A PQ candidate without PRESENT support becomes SDR/ColorSpaceUnsupported. The
// caller must recreate buffers if the resulting state.format changed; applying
// color space alone does not change the swap-chain buffer format.
[[nodiscard]] Result ApplyDisplayState(IDXGISwapChain4 *swap_chain, DisplayState &state);
} // namespace hs
