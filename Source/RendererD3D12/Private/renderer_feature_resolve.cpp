#include "renderer_feature_resolve.hpp"

#include <d3d12.h>

namespace hs
{
RendererFeatureHardware QueryRendererFeatureHardware(ID3D12Device *device) noexcept
{
    RendererFeatureHardware hardware;
    if (!device) return hardware;

    D3D12_FEATURE_DATA_SHADER_MODEL shader60{D3D_SHADER_MODEL_6_0};
    hardware.compute_shader60=SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&shader60,sizeof(shader60)))&&
        shader60.HighestShaderModel>=D3D_SHADER_MODEL_6_0;
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 vrs{};
    if(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6,&vrs,sizeof(vrs))))
    {hardware.vrs_tier=UINT(vrs.VariableShadingRateTier);hardware.vrs_tile_size=vrs.ShadingRateImageTileSize;
        hardware.vrs_additional_rates=vrs.AdditionalShadingRatesSupported!=FALSE;}
    return hardware;
}

RendererFeaturePlan ResolveRendererFeatures(const RendererFeatureRequest &request,
    const RendererFeatureCapabilities &capabilities) noexcept
{
    RendererFeaturePlan plan;
    plan.ao_mode = request.ao_mode;
    plan.gi_mode = request.gi_mode;
    plan.reflection_mode = request.reflection_mode;
    plan.shadow_mode = request.shadow_mode;

    if (request.ao_mode == GraphicsAoMode::Auto) plan.ao_mode = GraphicsAoMode::GTAO;
    if (request.gi_mode == GraphicsGiMode::SoftwareHybrid && !capabilities.software_gi_consumer)
    {
        plan.gi_mode = GraphicsGiMode::Probe;
        plan.gi_reason = RendererFeatureFallbackReason::ConsumerUnavailable;
    }
    plan.software_gi_enabled = plan.gi_mode == GraphicsGiMode::SoftwareHybrid;
    if(request.adaptive_shading==GraphicsAutoToggle::Off)
        plan.adaptive_raster_reason=plan.adaptive_compute_reason=RendererFeatureFallbackReason::RequestedOff;
    else
    {
        plan.adaptive_raster_reason=capabilities.hardware.vrs_tier==0?RendererFeatureFallbackReason::UnsupportedHardware:
            capabilities.hardware.vrs_tier<2||!capabilities.hardware.vrs_tile_size||!capabilities.adaptive_raster_consumer?
                RendererFeatureFallbackReason::ConsumerUnavailable:RendererFeatureFallbackReason::None;
        plan.adaptive_compute_reason=!capabilities.hardware.compute_shader60?RendererFeatureFallbackReason::UnsupportedHardware:
            !capabilities.adaptive_compute_consumer||plan.ao_mode!=GraphicsAoMode::GTAO?RendererFeatureFallbackReason::ConsumerUnavailable:
                RendererFeatureFallbackReason::None;
        plan.adaptive_raster_enabled=plan.adaptive_raster_reason==RendererFeatureFallbackReason::None;
        plan.adaptive_compute_enabled=plan.adaptive_compute_reason==RendererFeatureFallbackReason::None;
    }
    return plan;
}
} // namespace hs
