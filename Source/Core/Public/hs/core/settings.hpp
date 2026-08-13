#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

namespace hs
{

struct SettingsData
{
    static constexpr std::uint32_t kSchemaVersion = 1;

    std::uint32_t schema_version{kSchemaVersion};
    std::uint32_t width{1280};
    std::uint32_t height{720};
    bool borderless{};
    bool vsync{true};
    std::uint32_t frame_cap{60};
    std::uint32_t render_scale_percent{100};
    std::uint32_t shadow_resolution{1024};
    std::uint32_t particle_percentage{100};
    bool bloom{true};
    bool outline{true};
    float master_volume{1.0f};
    float bgm_volume{1.0f};
    float sfx_volume{1.0f};
    float ui_volume{1.0f};
    std::array<std::uint16_t, 4> skill_virtual_keys{'Q', 'W', 'E', 'R'};
};

enum class UiCommandKind : std::uint8_t
{
    SetBorderless,
    SetVsync,
    SetFrameCap,
    SetRenderScale,
    SetShadowResolution,
    SetParticlePercentage,
    SetBloom,
    SetOutline,
    SetMasterVolumePercent,
    SetBgmVolumePercent,
    SetSfxVolumePercent,
    SetUiVolumePercent,
    BeginSkillRebind,
    CancelSkillRebind,
};

struct UiCommand
{
    UiCommandKind kind{};
    std::uint32_t value{};
};

static_assert(std::is_trivially_copyable_v<SettingsData> &&
              std::is_standard_layout_v<SettingsData>);
static_assert(std::is_trivially_copyable_v<UiCommand> &&
              std::is_standard_layout_v<UiCommand>);

} // namespace hs
