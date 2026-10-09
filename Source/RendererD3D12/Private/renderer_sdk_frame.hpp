#pragma once

#include "render_graph.hpp"
#include "reconstruction_plan.hpp"
#include <DirectXMath.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <memory>
#include <vector>

namespace hs
{
struct SdkRect { std::uint32_t x{}, y{}, width{}, height{}; };
struct SdkFrameIdentity
{
    std::uint64_t render_id{}, presentation_id{};
    std::uint32_t viewport_id{};
};
struct SdkCamera
{
    // Untransposed DirectX row-vector matrices in absolute world coordinates.
    DirectX::XMFLOAT4X4 view{}, projection{}, inverse_projection{}, inverse_view{},
        current_unjittered_view_projection{}, previous_unjittered_view_projection{}, previous_view{}, previous_projection{};
    DirectX::XMFLOAT3 position{}, right{}, up{}, forward{}, previous_position{};
    float near_plane{}, far_plane{}, vertical_fov_radians{}, delta_seconds{};
    float jitter_pixels[2]{};
    bool reversed_depth{true}, reset_history{};
};
struct SdkReconstructionFrame
{
    // Exact native formats and states are checked by the selected adapter.
    // Motion is unjittered PreviousUV-CurrentUV, depth is reversed device Z.
    ID3D12Resource *color{}, *depth{}, *motion{}, *responsive{}, *composition{}, *exposure{}, *output{};
    // Optional real RR producers. The overlay is premultiplied; the preceding
    // color has matching scene-linear precision. Missing tags stay absent.
    ID3D12Resource *transparency_overlay{}, *color_before_transparency{}, *sss_luminance{};
    D3D12_RESOURCE_STATES color_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
        depth_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}, motion_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
        responsive_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}, composition_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
        exposure_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}, output_state{D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    D3D12_RESOURCE_STATES transparency_overlay_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
        color_before_transparency_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
        sss_luminance_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    SdkRect active, output_rect;
    SdkFrameIdentity identity;
    SdkCamera camera;
    float input_pre_exposure{1.0f}, exposure_scale{1.0f};
};
struct SdkRayReconstructionFrame
{
    SdkReconstructionFrame reconstruction;
    // These are explicit native guide conversions, never overloaded GBuffer
    // channels. A provider may reject absent required signals.
    ID3D12Resource *diffuse_radiance{}, *specular_radiance{}, *diffuse_albedo{}, *specular_albedo{},
        *normal_roughness{}, *signed_view_z{}, *motion_z{}, *packed_motion{}, *diffuse_hit_distance{}, *specular_hit_distance{},
        *diffuse_output{}, *specular_output{};
    D3D12_RESOURCE_STATES diffuse_radiance_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
        specular_radiance_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}, diffuse_albedo_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
        specular_albedo_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}, normal_roughness_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
        signed_view_z_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}, motion_z_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
        packed_motion_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}, diffuse_hit_distance_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
        specular_hit_distance_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}, diffuse_output_state{D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
        specular_output_state{D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
};
struct SdkPresentationFrame
{
    SdkFrameIdentity identity;
    SdkCamera camera;
    ID3D12Resource *hudless{}, *ui{}, *depth{}, *motion{}, *back_buffer{};
    SdkRect render_rect, output_rect;
    DXGI_FORMAT output_format{DXGI_FORMAT_UNKNOWN};
    DXGI_COLOR_SPACE_TYPE color_space{DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
    D3D12_RESOURCE_STATES hudless_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}, ui_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
        depth_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}, motion_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
        back_buffer_state{D3D12_RESOURCE_STATE_PRESENT};
    float minimum_luminance{}, maximum_luminance{100.0f};
    std::vector<std::shared_ptr<const void>> owners;
};
enum class SdkLatencyMarker : std::uint8_t
{
    SimulationStart, SimulationEnd, RenderSubmitStart, RenderSubmitEnd, PresentStart, PresentEnd, InputSample
};
} // namespace hs
