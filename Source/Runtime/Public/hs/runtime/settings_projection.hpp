#pragma once

#include <hs/core/settings.hpp>
#include <hs/runtime/save_store.hpp>

#include <algorithm>
#include <type_traits>
#include <utility>

namespace hs
{

// SettingsData remains the legacy simulation/UI/audio projection. Full graphics
// intent is passed separately to the renderer, without changing this gameplay ABI.
[[nodiscard]] inline SettingsData ProjectSettingsDocument(const SettingsDocument &document,
                                                         const GraphicsSettings &graphics)
{
    SettingsData effective;
    if (graphics.output_extent)
    {
        effective.width = graphics.output_extent->width;
        effective.height = graphics.output_extent->height;
    }
    effective.borderless = graphics.display_mode == GraphicsDisplayMode::BorderlessFullscreen;
    effective.vsync = graphics.vsync;
    effective.frame_cap = graphics.frame_limit_fps == 0
        ? 0 : std::clamp(graphics.frame_limit_fps, 1u, 1000u);
    if (graphics.resolution_mode == GraphicsResolutionMode::Manual &&
        graphics.reconstruction_provider.value == "internal_taau")
        effective.render_scale_percent = static_cast<std::uint32_t>(
            std::clamp(graphics.manual_scale_percent, 75.0f, 100.0f));
    if (graphics.shadow_quality == GraphicsQuality::High ||
        graphics.shadow_quality == GraphicsQuality::Ultra)
        effective.shadow_resolution = 2048;
    if (graphics.particle_quality == GraphicsQuality::Low)
        effective.particle_percentage = 50;
    else if (graphics.particle_quality == GraphicsQuality::Medium)
        effective.particle_percentage = 75;
    effective.bloom = graphics.bloom;
    effective.outline = graphics.outline;
    effective.master_volume = document.master_volume;
    effective.bgm_volume = document.bgm_volume;
    effective.sfx_volume = document.sfx_volume;
    effective.ui_volume = document.ui_volume;
    effective.skill_virtual_keys = document.skill_virtual_keys;
    effective.screen_overlay_intensity = document.screen_overlay_intensity;
    return effective;
}

[[nodiscard]] inline SettingsData ProjectSettingsDocument(const SettingsDocument &document)
{
    return ProjectSettingsDocument(document, document.graphics);
}

[[nodiscard]] inline SettingsDocument LegacyDefaultSettingsDocument()
{
    const SettingsData defaults;
    SettingsDocument document;
    document.graphics.preset = GraphicsPreset::Custom;
    document.graphics.display_mode = GraphicsDisplayMode::Windowed;
    document.graphics.output_extent = GraphicsExtent{defaults.width, defaults.height};
    document.graphics.vsync = defaults.vsync;
    document.graphics.frame_limit_fps = defaults.frame_cap;
    document.graphics.resolution_mode = GraphicsResolutionMode::Manual;
    document.graphics.reconstruction_provider.value = "internal_taau";
    document.graphics.manual_scale_percent = static_cast<float>(defaults.render_scale_percent);
    document.graphics.shadow_quality = GraphicsQuality::Medium;
    document.graphics.particle_quality = GraphicsQuality::High;
    return document;
}

template<class T> inline void ApplyPresetQuality(T &field, GraphicsQuality quality)
{
    if constexpr (std::is_same_v<T, GraphicsQuality>) field = quality;
    else if constexpr (std::is_same_v<T, GraphicsOptionalQuality>)
        field = static_cast<GraphicsOptionalQuality>(static_cast<std::uint8_t>(quality) + 1);
}

inline void ApplyGraphicsPreset(GraphicsSettings &graphics)
{
    const auto preset = graphics.preset;
    if (preset == GraphicsPreset::Custom) return;
    const auto quality = preset == GraphicsPreset::Low ? GraphicsQuality::Low
        : preset == GraphicsPreset::Medium ? GraphicsQuality::Medium
        : preset == GraphicsPreset::High ? GraphicsQuality::High : GraphicsQuality::Ultra;
#define HS_GRAPHICS_SETTING(id, name, field, type, kind, default_value, default_token, minimum, maximum, group, label_key) ApplyPresetQuality(graphics.field, quality);
#include <hs/core/graphics_settings.def>
#undef HS_GRAPHICS_SETTING
    graphics.ao_mode = GraphicsAoMode::GTAO;
    graphics.gi_mode = preset == GraphicsPreset::Ultra ? GraphicsGiMode::SoftwareHybrid : GraphicsGiMode::Probe;
    graphics.reflection_mode = preset == GraphicsPreset::Low ? GraphicsReflectionMode::Probe : GraphicsReflectionMode::ScreenSpace;
    graphics.shadow_mode = GraphicsShadowMode::Raster;
    if (preset == GraphicsPreset::Low) graphics.volumetric_quality = GraphicsOptionalQuality::Off;
}

// Complete validation and allocation occur in a local candidate. Invalid edits
// leave requested intent untouched, including unknown optional schema2 values.
inline bool ApplySettingsEdit(SettingsDocument &document, const UiCommand &command)
{
    auto candidate = document;
    auto &graphics = candidate.graphics;
    bool graphics_edit = true;
    switch (command.kind)
    {
    case UiCommandKind::SetGraphicsSetting:
    {
        const auto end = std::find(command.graphics_token.begin(), command.graphics_token.end(), '\0');
        if (end == command.graphics_token.end() || end == command.graphics_token.begin() ||
            !SetGraphicsSettingToken(graphics, command.graphics_setting,
                std::string_view(command.graphics_token.data(), static_cast<std::size_t>(end - command.graphics_token.begin()))))
            return false;
        if (command.graphics_setting == SettingId::Preset) ApplyGraphicsPreset(graphics);
        else graphics.preset = GraphicsPreset::Custom;
        break;
    }
    case UiCommandKind::SetBorderless:
        if (command.value > 1) return false;
        graphics.display_mode = command.value ? GraphicsDisplayMode::BorderlessFullscreen
                                              : GraphicsDisplayMode::Windowed;
        break;
    case UiCommandKind::SetVsync: if (command.value > 1) return false; graphics.vsync = command.value != 0; break;
    case UiCommandKind::SetFrameCap: if (command.value > 1000) return false; graphics.frame_limit_fps = command.value; break;
    case UiCommandKind::SetRenderScale:
        if (command.value < 50 || command.value > 200) return false;
        graphics.resolution_mode = GraphicsResolutionMode::Manual;
        graphics.reconstruction_provider.value = "internal_taau";
        graphics.manual_scale_percent = static_cast<float>(command.value);
        break;
    case UiCommandKind::SetShadowResolution:
        if (command.value != 1024 && command.value != 2048) return false;
        graphics.shadow_quality = command.value == 1024 ? GraphicsQuality::Medium
                                                         : GraphicsQuality::High;
        break;
    case UiCommandKind::SetParticlePercentage:
        if (command.value != 50 && command.value != 75 && command.value != 100) return false;
        graphics.particle_quality = command.value == 50 ? GraphicsQuality::Low
            : command.value == 75 ? GraphicsQuality::Medium : GraphicsQuality::High;
        break;
    case UiCommandKind::SetBloom: if (command.value > 1) return false; graphics.bloom = command.value != 0; break;
    case UiCommandKind::SetOutline: if (command.value > 1) return false; graphics.outline = command.value != 0; break;
    case UiCommandKind::SetMasterVolumePercent:
        if (command.value > 100) return false;
        candidate.master_volume = static_cast<float>(command.value) / 100.0f; graphics_edit = false; break;
    case UiCommandKind::SetBgmVolumePercent:
        if (command.value > 100) return false;
        candidate.bgm_volume = static_cast<float>(command.value) / 100.0f; graphics_edit = false; break;
    case UiCommandKind::SetSfxVolumePercent:
        if (command.value > 100) return false;
        candidate.sfx_volume = static_cast<float>(command.value) / 100.0f; graphics_edit = false; break;
    case UiCommandKind::SetUiVolumePercent:
        if (command.value > 100) return false;
        candidate.ui_volume = static_cast<float>(command.value) / 100.0f; graphics_edit = false; break;
    case UiCommandKind::SetVfxScreenOverlayIntensityPercent:
        if (command.value > 100) return false;
        candidate.screen_overlay_intensity = static_cast<float>(command.value) / 100.0f;
        graphics_edit = false; break;
    case UiCommandKind::BeginSkillRebind:
    case UiCommandKind::CancelSkillRebind:
        return false;
    default: return false;
    }
    if (graphics_edit && command.kind != UiCommandKind::SetGraphicsSetting)
        graphics.preset = GraphicsPreset::Custom;
    if (!ValidateGraphicsSettings(graphics).empty()) return false;
    document = std::move(candidate);
    return true;
}

} // namespace hs
