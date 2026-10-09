#include "renderer_feature_resolve.hpp"

#include <d3d12.h>
#include <wrl/client.h>

namespace hs
{
RendererFeatureHardware QueryRendererFeatureHardware(ID3D12Device *device) noexcept
{
    RendererFeatureHardware hardware;
    if (!device) return hardware;

    Microsoft::WRL::ComPtr<ID3D12Device5> rayDevice;
    const bool device5 = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(rayDevice.ReleaseAndGetAddressOf())));
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
    const bool options5 = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
        &options, sizeof(options)));
    // This query must not be short-circuited behind mesh tier, RT tier or master
    // settings: inline RayQuery shaders have their own shader-model requirement.
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_5};
    const bool shader65 = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,
        &shaderModel, sizeof(shaderModel))) && shaderModel.HighestShaderModel >= D3D_SHADER_MODEL_6_5;
    hardware.ray_scene = device5 && options5 && options.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0;
    hardware.inline_ray_queries = hardware.ray_scene && shader65 && options.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
    D3D12_FEATURE_DATA_SHADER_MODEL shader60{D3D_SHADER_MODEL_6_0},shader69{D3D_SHADER_MODEL_6_9};
    hardware.compute_shader60=SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&shader60,sizeof(shader60)))&&
        shader60.HighestShaderModel>=D3D_SHADER_MODEL_6_0;
    hardware.shader_model69=SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&shader69,sizeof(shader69)))&&
        shader69.HighestShaderModel>=D3D_SHADER_MODEL_6_9;
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 vrs{};
    if(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6,&vrs,sizeof(vrs))))
    {hardware.vrs_tier=UINT(vrs.VariableShadingRateTier);hardware.vrs_tile_size=vrs.ShadingRateImageTileSize;
        hardware.vrs_additional_rates=vrs.AdditionalShadingRatesSupported!=FALSE;}
    hardware.opacity_micromap_arrays=hardware.ray_scene&&options.RaytracingTier>=D3D12_RAYTRACING_TIER_1_2;
    D3D12_FEATURE_DATA_D3D12_OPTIONS22 ser{};
    hardware.ser_actually_reorders=hardware.ray_scene&&hardware.shader_model69&&
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS22,&ser,sizeof(ser)))&&ser.ShaderExecutionReorderingActuallyReorders;
    return hardware;
}

namespace
{
RendererFeatureFallbackReason RayRejection(const RendererFeatureRequest &request,
    const RendererFeatureCapabilities &capabilities, bool inlineQueries, bool consumer) noexcept
{
    if (!request.ray_tracing_enabled) return RendererFeatureFallbackReason::RayTracingDisabled;
    if (!capabilities.hardware.ray_scene || (inlineQueries && !capabilities.hardware.inline_ray_queries))
        return RendererFeatureFallbackReason::UnsupportedHardware;
    if (!consumer) return RendererFeatureFallbackReason::ConsumerUnavailable;
    return RendererFeatureFallbackReason::None;
}
} // namespace

RendererFeaturePlan ResolveRendererFeatures(const RendererFeatureRequest &request,
    const RendererFeatureCapabilities &capabilities) noexcept
{
    RendererFeaturePlan plan;
    plan.ao_mode = request.ao_mode;
    plan.gi_mode = request.gi_mode;
    plan.reflection_mode = request.reflection_mode;
    plan.shadow_mode = request.shadow_mode;

    if (request.ao_mode == GraphicsAoMode::Auto || request.ao_mode == GraphicsAoMode::RTAO)
    {
        plan.ao_reason = RayRejection(request, capabilities, true, capabilities.rtao_consumer);
        if (plan.ao_reason == RendererFeatureFallbackReason::None)
        {
            plan.ao_mode = GraphicsAoMode::RTAO;
            if (request.ao_mode == GraphicsAoMode::Auto) plan.ao_reason = RendererFeatureFallbackReason::AutoSelection;
        }
        else plan.ao_mode = GraphicsAoMode::GTAO;
    }

    if (request.gi_mode == GraphicsGiMode::RayTraced)
    {
        plan.gi_reason = RayRejection(request, capabilities, true, capabilities.rtgi_consumer);
        if (plan.gi_reason != RendererFeatureFallbackReason::None)
            plan.gi_mode = capabilities.software_gi_consumer ? GraphicsGiMode::SoftwareHybrid : GraphicsGiMode::Probe;
    }
    else if (request.gi_mode == GraphicsGiMode::SoftwareHybrid && !capabilities.software_gi_consumer)
    {
        plan.gi_mode = GraphicsGiMode::Probe;
        plan.gi_reason = RendererFeatureFallbackReason::ConsumerUnavailable;
    }

    if (request.reflection_mode == GraphicsReflectionMode::HybridRT)
    {
        plan.reflection_reason = RayRejection(request, capabilities, true, capabilities.rt_reflection_consumer);
        if (plan.reflection_reason != RendererFeatureFallbackReason::None)
            plan.reflection_mode = GraphicsReflectionMode::ScreenSpace;
    }
    if (request.shadow_mode == GraphicsShadowMode::HybridRT)
    {
        plan.shadow_reason = RayRejection(request, capabilities, false, capabilities.rt_shadow_consumer);
        if (plan.shadow_reason != RendererFeatureFallbackReason::None)
            plan.shadow_mode = GraphicsShadowMode::Raster;
    }

    plan.ray_scene_enabled = plan.ao_mode == GraphicsAoMode::RTAO || plan.gi_mode == GraphicsGiMode::RayTraced ||
        plan.reflection_mode == GraphicsReflectionMode::HybridRT || plan.shadow_mode == GraphicsShadowMode::HybridRT;
    plan.software_gi_enabled = plan.gi_mode == GraphicsGiMode::SoftwareHybrid;
    plan.canonical_geometry_enabled = plan.ray_scene_enabled || plan.software_gi_enabled;
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
