#pragma once

#include <hs/runtime/application.hpp>
#include <hs/runtime/settings_projection.hpp>

namespace hs
{

[[nodiscard]] inline SettingsData PrepareStartupGraphicsSettings(
    SettingsDocument &requested, const ApplicationConfig &config)
{
    // Smoke/capture runs retain their process display defaults, independent of a saved profile.
    if (config.resolution_explicit || config.smoke)
    {
        requested.graphics.output_extent = GraphicsExtent{config.width, config.height};
        requested.graphics.preset = GraphicsPreset::Custom;
    }
    if (config.borderless_explicit || config.smoke)
        (void)ApplySettingsEdit(requested, {UiCommandKind::SetBorderless, config.borderless ? 1u : 0u});
    if (config.vsync_explicit || config.smoke)
        (void)ApplySettingsEdit(requested, {UiCommandKind::SetVsync, config.vsync ? 1u : 0u});
    if (config.frame_cap_explicit || config.smoke)
        (void)ApplySettingsEdit(requested, {UiCommandKind::SetFrameCap, config.frame_cap});
    if (config.render_scale_explicit)
        (void)ApplySettingsEdit(requested, {UiCommandKind::SetRenderScale, config.render_scale_percent});
    if (config.shadow_resolution_explicit)
        (void)ApplySettingsEdit(requested, {UiCommandKind::SetShadowResolution, config.shadow_resolution});
    if (config.particle_percentage_explicit)
        (void)ApplySettingsEdit(requested, {UiCommandKind::SetParticlePercentage, config.particle_percentage});
    requested.graphics.bloom = config.bloom;
    requested.graphics.outline = config.outline;
    auto effective = ProjectSettingsDocument(requested);
    if (config.mute_audio) effective.master_volume = 0.0f;
    return effective;
}

} // namespace hs
