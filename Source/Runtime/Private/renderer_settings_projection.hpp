#pragma once

#include <hs/runtime/settings_projection.hpp>
#include <hs/renderer/renderer.hpp>

#include <algorithm>
#include <cmath>

namespace hs
{

[[nodiscard]] inline RendererConfig ProjectRendererConfig(const SettingsData &legacy,
                                                         const GraphicsSettings &graphics)
{
    RendererConfig config;
    config.vsync = graphics.vsync;
    config.bloom = graphics.bloom;
    config.outline = graphics.outline;
    config.render_scale_percent = legacy.render_scale_percent;
    if (graphics.resolution_mode == GraphicsResolutionMode::Manual)
        config.render_scale_percent = static_cast<std::uint32_t>(
            std::lround(std::clamp(graphics.manual_scale_percent, 50.0f, 200.0f)));
    config.dynamic_resolution = graphics.dynamic_resolution;
    config.target_render_fps = graphics.target_render_fps;
    config.dynamic_min_percent = graphics.dynamic_min_percent;
    config.dynamic_max_percent = graphics.dynamic_max_percent;
    config.vfx_quality = graphics.particle_quality == GraphicsQuality::Low ? VfxQuality::Low
        : graphics.particle_quality == GraphicsQuality::Medium ? VfxQuality::Medium : VfxQuality::High;
    config.anisotropic_filtering = graphics.anisotropic_filtering;
    config.bloom_strength = graphics.bloom_strength;
    config.outline_width_px = graphics.outline_width_px;
    config.color_grading = graphics.color_grading;
    config.motion_blur = graphics.motion_blur;
    config.adaptive_shading = graphics.adaptive_shading;
    // This field belongs to the legacy UI/audio settings document, not the
    // GraphicsSettings snapshot. Preserve the configured value across requests.
    config.screen_overlay_intensity = legacy.screen_overlay_intensity;
    config.depth_of_field = graphics.depth_of_field;
    config.shadow_distance_m = graphics.shadow_distance_m;
    config.texture_quality = graphics.texture_quality;
    config.texture_budget_mib = graphics.texture_budget_mib;
    config.geometry_quality = graphics.geometry_quality;
    config.foliage_quality = graphics.foliage_quality;
    config.material_quality = graphics.material_quality;
    config.pom_quality = graphics.pom_quality;
    config.lighting_quality = graphics.lighting_quality;
    config.ao_mode = graphics.ao_mode;
    config.ao_quality = graphics.ao_quality;
    config.contact_shadows = graphics.contact_shadows;
    config.reflection_mode = graphics.reflection_mode;
    config.reflection_quality = graphics.reflection_quality;
    config.transparency_quality = graphics.transparency_quality;
    config.refraction_quality = graphics.refraction_quality;
    config.volumetric_quality = graphics.volumetric_quality;
    config.ray_tracing_enabled = graphics.ray_tracing_enabled;
    config.rt_trace_distance_m = graphics.rt_trace_distance_m;
    config.gi_mode = graphics.gi_mode;
    config.gi_quality = graphics.gi_quality;
    config.shadow_mode = graphics.shadow_mode;
    config.shadow_quality = graphics.shadow_quality;
    // This controls the existing internal temporal path independently of VFX
    // quality. Unsupported provider requests remain intact in the snapshot.
    config.temporal_reconstruction = graphics.reconstruction_provider.value != "off";
    config.reconstruction_request = RendererReconstructionRequest{
        graphics.reconstruction_provider, graphics.reconstruction_quality,
        graphics.resolution_mode, graphics.manual_scale_percent};
    config.presentation_request = {graphics.frame_generation_provider,
        graphics.frame_generation_mode, graphics.frame_generation_multiplier,
        graphics.low_latency, graphics.frame_limit_fps};
    config.ray_reconstruction_provider = graphics.ray_reconstruction_provider;
    config.neural_ray_reconstruction = graphics.neural_ray_reconstruction;
    config.sharpening = graphics.sharpening;
    config.hdr_mode = graphics.hdr_mode;
    config.hdr_paper_white_nits = graphics.hdr_paper_white_nits;
    config.hdr_peak_nits = graphics.hdr_peak_nits;
    config.display_output = graphics.display_output;
    // RendererConfig owns the initial full requested snapshot; the render
    // worker applies its native projection before preparation.
    config.requested_graphics = graphics;
    config.width = legacy.width;
    config.height = legacy.height;
    return config;
}

[[nodiscard]] inline RendererConfig ProjectRendererConfig(const SettingsData &legacy)
{
    RendererConfig config;
    config.vsync = legacy.vsync;
    config.bloom = legacy.bloom;
    config.outline = legacy.outline;
    config.render_scale_percent = legacy.render_scale_percent;
    config.shadow_resolution = legacy.shadow_resolution;
    config.particle_percentage = legacy.particle_percentage;
    config.screen_overlay_intensity = legacy.screen_overlay_intensity;
    config.width = legacy.width;
    config.height = legacy.height;
    return config;
}

} // namespace hs
