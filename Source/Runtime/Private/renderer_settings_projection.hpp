#pragma once

#include <hs/runtime/settings_projection.hpp>
#include <hs/game_domain/game_types.hpp>
#include <hs/renderer/renderer.hpp>

#include <string>

namespace hs
{

[[nodiscard]] inline bool AllowGameplayFrameGeneration(SessionPhase phase) noexcept
{
    return phase == SessionPhase::Playing;
}

// A GetState delta is telemetry, not a newly selected generation multiplier.
// Keep option receipts/config revisions stable when SDK counts vary per Present.
[[nodiscard]] inline std::string FrameGenerationMultiplierPolicyToken(const RendererPresentationStatus &status)
{
    if (status.active_mode == GraphicsFrameGenerationMode::Off) return "0";
    if (status.effective_provider.value == "fsr") return "native";
    if (status.active_mode == GraphicsFrameGenerationMode::Dynamic) return "dynamic";
    return std::to_string(status.requested.multiplier);
}

[[nodiscard]] inline RendererConfig ProjectRendererConfig(const SettingsData &legacy,
                                                         const GraphicsSettings &graphics)
{
    RendererConfig config;
    config.width = legacy.width;
    config.height = legacy.height;
    // Native graphics options are projected once by RendererD3D12 during
    // Initialize. Keep the legacy scale as its non-manual fallback.
    config.render_scale_percent = legacy.render_scale_percent;
    // Overlay intensity is owned by the legacy UI/audio settings document.
    config.screen_overlay_intensity = legacy.screen_overlay_intensity;
    // Keep the complete request; the render worker projects it before device
    // creation. The caller may still set a resolved Auto display output.
    config.requested_graphics = graphics;
    return config;
}

} // namespace hs
