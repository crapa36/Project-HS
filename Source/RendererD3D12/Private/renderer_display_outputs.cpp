#include <hs/renderer/renderer.hpp>

#include "renderer_diagnostics.hpp"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cmath>
#include <format>
#include <string_view>
#include <utility>

namespace hs
{
Result EnumerateRendererDisplays(std::vector<RendererDisplayOutput> &outputs)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory1> factory;
    auto queried = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(queried)) return HResultFailure("Create display enumeration factory", queried);
    std::vector<RendererDisplayOutput> candidate;
    for (UINT adapter_index = 0; ; ++adapter_index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        queried = factory->EnumAdapters1(adapter_index, &adapter);
        if (queried == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(queried)) return HResultFailure("Enumerate native display adapters", queried);
        DXGI_ADAPTER_DESC1 adapter_description{};
        queried = adapter->GetDesc1(&adapter_description);
        if (FAILED(queried)) return HResultFailure("Query native display adapter", queried);
        for (UINT output_index = 0; ; ++output_index)
        {
            ComPtr<IDXGIOutput> output;
            queried = adapter->EnumOutputs(output_index, &output);
            if (queried == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(queried)) return HResultFailure("Enumerate native display outputs", queried);
            DXGI_OUTPUT_DESC description{};
            queried = output->GetDesc(&description);
            if (FAILED(queried)) return HResultFailure("Query native display output", queried);
            if (!description.AttachedToDesktop) continue;
            const auto &rect = description.DesktopCoordinates;
            if (rect.right <= rect.left || rect.bottom <= rect.top)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "Attached display output has an empty desktop rectangle.");
            const std::wstring_view device_name(description.DeviceName);
            const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                device_name.data(), static_cast<int>(device_name.size()), nullptr, 0, nullptr, nullptr);
            if (bytes <= 0)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "Display output device name is not valid UTF-16.");
            std::string name(static_cast<std::size_t>(bytes), '\0');
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, device_name.data(),
                    static_cast<int>(device_name.size()), name.data(), bytes, nullptr, nullptr))
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "Cannot encode native display output ID.");
            RendererDisplayOutput native;
            native.id = std::format("dxgi:{:08x}{:08x}:{}",
                static_cast<std::uint32_t>(adapter_description.AdapterLuid.HighPart),
                adapter_description.AdapterLuid.LowPart, name);
            native.left = rect.left;
            native.top = rect.top;
            native.width = static_cast<std::uint32_t>(static_cast<std::int64_t>(rect.right) - rect.left);
            native.height = static_cast<std::uint32_t>(static_cast<std::int64_t>(rect.bottom) - rect.top);
            ComPtr<IDXGIOutput6> output6;
            queried = output.As(&output6);
            if (queried != E_NOINTERFACE && FAILED(queried))
                return HResultFailure("Query native display HDR interface", queried);
            if (output6)
            {
                DXGI_OUTPUT_DESC1 color{};
                queried = output6->GetDesc1(&color);
                if (FAILED(queried)) return HResultFailure("Query native display HDR state", queried);
                native.hdr_active = color.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                if (std::isfinite(color.MaxLuminance) && color.MaxLuminance > 0.0f)
                    native.peak_nits = color.MaxLuminance;
            }
            candidate.push_back(std::move(native));
        }
    }
    outputs = std::move(candidate);
    return Result::Success();
}
} // namespace hs
