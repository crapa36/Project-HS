#pragma once
#include <hs/render_data/quality_profiles.hpp>

#include <hs/renderer/renderer.hpp>

namespace hs
{

// RendererOptions is deliberately private. Runtime and Core carry the full
// GraphicsSettings snapshot; this is the renderer's native projection used by
// the existing prepare/commit transaction.
struct RendererOptions
{
    float screen_overlay_intensity{1.0f};
    GraphicsAutoToggle adaptive_shading{GraphicsAutoToggle::Auto};
    bool vsync{true};
    bool bloom{true};
    bool outline{true};
    std::uint32_t render_scale_percent{100};
    bool dynamic_resolution{};
    std::uint32_t target_render_fps{60};
    float dynamic_min_percent{50.0f};
    float dynamic_max_percent{100.0f};
    std::uint32_t shadow_resolution{1024};
    std::uint32_t particle_percentage{100};
    VfxQuality vfx_quality{VfxQuality::High};
    std::uint32_t anisotropic_filtering{16};
    float bloom_strength{1.0f};
    float outline_width_px{1.0f};
    bool color_grading{true};
    GraphicsMotionBlur motion_blur{GraphicsMotionBlur::Off};
    bool depth_of_field{};
    float shadow_distance_m{160.0f};
    GraphicsQuality texture_quality{GraphicsQuality::Medium};
    std::uint32_t texture_budget_mib{};
    GraphicsQuality geometry_quality{GraphicsQuality::Medium};
    GraphicsQuality foliage_quality{GraphicsQuality::Medium};
    GraphicsQuality material_quality{GraphicsQuality::Medium};
    GraphicsOptionalQuality pom_quality{GraphicsOptionalQuality::Medium};
    GraphicsQuality lighting_quality{GraphicsQuality::Medium};
    GraphicsAoMode ao_mode{GraphicsAoMode::GTAO};
    GraphicsQuality ao_quality{GraphicsQuality::Medium};
    bool contact_shadows{true};
    GraphicsReflectionMode reflection_mode{GraphicsReflectionMode::ScreenSpace};
    GraphicsQuality reflection_quality{GraphicsQuality::Medium};
    GraphicsQuality transparency_quality{GraphicsQuality::Medium};
    GraphicsOptionalQuality refraction_quality{GraphicsOptionalQuality::Medium};
    GraphicsOptionalQuality volumetric_quality{GraphicsOptionalQuality::Medium};
    GraphicsGiMode gi_mode{GraphicsGiMode::Probe};
    GraphicsQuality gi_quality{GraphicsQuality::Medium};
    GraphicsShadowMode shadow_mode{GraphicsShadowMode::Raster};
    GraphicsQuality shadow_quality{GraphicsQuality::Medium};
    bool temporal_reconstruction{true};
    std::optional<RendererReconstructionRequest> reconstruction_request;
    RendererPresentationRequest presentation_request;
    float sharpening{0.2f};
    GraphicsHdrMode hdr_mode{GraphicsHdrMode::Auto};
    float hdr_paper_white_nits{200.0f};
    GraphicsAutoFloat hdr_peak_nits;
    GraphicsDisplayId display_output{"auto"};
};

[[nodiscard]] RendererOptions ProjectRendererOptions(
    const GraphicsSettings &, const RendererConfig &legacy_defaults,
    const render_data::QualityProfiles &);

// Apply the private native projection to the legacy renderer configuration
// before device/pipeline preparation. This keeps startup and the existing
// render-thread ApplyOptions transaction on the same field mapping.
void ApplyProjectedRendererOptions(RendererConfig &, const RendererOptions &);

} // namespace hs
