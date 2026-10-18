#pragma once

#include <hs/core/graphics_settings.hpp>
#include <array>
#include <cstdint>

struct ID3D12Device;

namespace hs
{
struct RendererFeatureHardware
{
    std::uint32_t vrs_tier{}, vrs_tile_size{};
    bool vrs_additional_rates{};
    bool compute_shader60{};
};

// Probe hardware independently of requested settings, active service storage and
// mesh shader support. Unsupported interfaces/features leave the capability off.
[[nodiscard]] RendererFeatureHardware QueryRendererFeatureHardware(ID3D12Device *device) noexcept;

struct RendererFeatureCapabilities
{
    RendererFeatureHardware hardware;
    // Explicit availability of integrated consumers, including their native
    // prerequisites. Keep absent consumers false.
    bool software_gi_consumer{};
    bool adaptive_raster_consumer{}, adaptive_compute_consumer{};
};

struct RendererFeatureRequest
{
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
    bool software_gi_enabled{};
    bool adaptive_raster_enabled{}, adaptive_compute_enabled{};
    RendererFeatureFallbackReason adaptive_raster_reason{RendererFeatureFallbackReason::None};
    RendererFeatureFallbackReason adaptive_compute_reason{RendererFeatureFallbackReason::None};
    RendererFeatureFallbackReason ao_reason{RendererFeatureFallbackReason::None};
    RendererFeatureFallbackReason gi_reason{RendererFeatureFallbackReason::None};
    RendererFeatureFallbackReason reflection_reason{RendererFeatureFallbackReason::None};
    RendererFeatureFallbackReason shadow_reason{RendererFeatureFallbackReason::None};
};

// Preserve the complete native/software transport input contract, even without SSS materials.
[[nodiscard]] constexpr std::array<bool, 3> LightingAuxiliaryTargets(
    const RendererFeaturePlan &plan, bool have_subsurface) noexcept
{
    const bool transport = plan.software_gi_enabled;
    return {have_subsurface || transport,
        plan.reflection_mode != GraphicsReflectionMode::Off || transport, transport};
}

// Pure resolution of validated raster/software requests; Auto AO uses GTAO.
[[nodiscard]] RendererFeaturePlan ResolveRendererFeatures(const RendererFeatureRequest &request,
    const RendererFeatureCapabilities &capabilities) noexcept;
} // namespace hs
