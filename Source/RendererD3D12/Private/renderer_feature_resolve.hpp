#pragma once

#include <hs/core/graphics_settings.hpp>
#include <cstdint>

struct ID3D12Device;

namespace hs
{
struct RendererFeatureHardware
{
    bool ray_scene{};          // Device5 + DXR tier >= 1.0.
    bool inline_ray_queries{}; // Device5 + DXR tier >= 1.1 + shader model >= 6.5.
    std::uint32_t vrs_tier{}, vrs_tile_size{};
    bool vrs_additional_rates{};
    bool compute_shader60{}, shader_model69{};
    bool opacity_micromap_arrays{}; // Actual native DXR1.2 array/geometry ABI, independent of compiler/products.
    bool ser_actually_reorders{}; // DXR + SM6.9 + actual OPTIONS22 capability.
};

// Probe hardware independently of requested settings, active service storage and
// mesh shader support. Unsupported interfaces/features leave the capability off.
[[nodiscard]] RendererFeatureHardware QueryRendererFeatureHardware(ID3D12Device *device) noexcept;

struct RendererFeatureCapabilities
{
    RendererFeatureHardware hardware;
    // Explicit availability of integrated consumers, never inferred from enum
    // values or an acceleration-structure service. Keep absent consumers false.
    // Consumer availability also requires its additional native prerequisites;
    // the centralized hardware contract below only guarantees AS/inline support.
    bool rtao_consumer{};
    bool rtgi_consumer{};
    bool rt_reflection_consumer{};
    bool rt_shadow_consumer{};
    bool software_gi_consumer{};
    bool adaptive_raster_consumer{}, adaptive_compute_consumer{};
};

struct RendererFeatureRequest
{
    bool ray_tracing_enabled{};
    GraphicsAoMode ao_mode{GraphicsAoMode::GTAO};
    GraphicsGiMode gi_mode{GraphicsGiMode::Probe};
    GraphicsReflectionMode reflection_mode{GraphicsReflectionMode::ScreenSpace};
    GraphicsShadowMode shadow_mode{GraphicsShadowMode::Raster};
    GraphicsAutoToggle adaptive_shading{GraphicsAutoToggle::Auto};
};

enum class RendererFeatureFallbackReason : std::uint8_t
{
    None,
    AutoSelection,
    RayTracingDisabled,
    UnsupportedHardware,
    ConsumerUnavailable,
    RequestedOff,
};

struct RendererFeaturePlan
{
    GraphicsAoMode ao_mode{GraphicsAoMode::GTAO};
    GraphicsGiMode gi_mode{GraphicsGiMode::Probe};
    GraphicsReflectionMode reflection_mode{GraphicsReflectionMode::ScreenSpace};
    GraphicsShadowMode shadow_mode{GraphicsShadowMode::Raster};
    bool ray_scene_enabled{};
    bool software_gi_enabled{};
    bool canonical_geometry_enabled{};
    bool adaptive_raster_enabled{}, adaptive_compute_enabled{};
    RendererFeatureFallbackReason adaptive_raster_reason{RendererFeatureFallbackReason::None};
    RendererFeatureFallbackReason adaptive_compute_reason{RendererFeatureFallbackReason::None};
    RendererFeatureFallbackReason ao_reason{RendererFeatureFallbackReason::None};
    RendererFeatureFallbackReason gi_reason{RendererFeatureFallbackReason::None};
    RendererFeatureFallbackReason reflection_reason{RendererFeatureFallbackReason::None};
    RendererFeatureFallbackReason shadow_reason{RendererFeatureFallbackReason::None};
};

// Pure resolution of validated public enum requests. The request remains the
// persistence/apply authority; preparations and frame recording use this plan.
// Reasons prefer master Off, then hardware support, then consumer availability.
// Auto AO selects eligible RTAO, otherwise GTAO. RTGI falls back to an actually
// available software GI consumer, otherwise Probe. No speculative AS allocation.
[[nodiscard]] RendererFeaturePlan ResolveRendererFeatures(const RendererFeatureRequest &request,
    const RendererFeatureCapabilities &capabilities) noexcept;
} // namespace hs
