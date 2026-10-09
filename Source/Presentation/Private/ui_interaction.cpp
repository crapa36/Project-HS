#include <hs/presentation/projector.hpp>

#include <algorithm>
#include <cmath>
#include <array>
#include <ranges>
#include <utility>
#include <charconv>
#include <type_traits>
#include <limits>

namespace hs
{
std::size_t GraphicsSettingsPageCount(std::uint8_t group)
{
    if (group >= 7) return 1;
    const auto count = std::ranges::count_if(GraphicsSettingDescriptors(), [&](const auto &d) {
        return static_cast<std::uint8_t>(d.group) == group;
    });
    return std::max<std::size_t>(1, (static_cast<std::size_t>(count) + kGraphicsUiRowsPerPage - 1) / kGraphicsUiRowsPerPage);
}
std::vector<const GraphicsSettingDescriptor *> GraphicsSettingsVisibleRows(const PresentationUiState &ui)
{
    std::vector<const GraphicsSettingDescriptor *> rows;
    if (ui.settings_group >= 7) return rows;
    const auto page = std::min<std::size_t>(ui.settings_page, GraphicsSettingsPageCount(ui.settings_group) - 1);
    std::size_t index{};
    for (const auto &d : GraphicsSettingDescriptors())
        if (static_cast<std::uint8_t>(d.group) == ui.settings_group)
        {
            if (index / kGraphicsUiRowsPerPage == page) rows.push_back(&d);
            ++index;
        }
    return rows;
}
std::string GraphicsSettingDisplayLabel(const GraphicsSettingDescriptor &descriptor)
{
    std::string label(descriptor.key);
    std::replace(label.begin(), label.end(), '_', ' ');
    if (!label.empty() && label.front() >= 'a' && label.front() <= 'z') label.front() -= 'a' - 'A';
    return label;
}
namespace
{
template<class T> std::vector<std::string> EnumChoices(const T &)
{
    std::vector<std::string> values;
    if constexpr (std::is_enum_v<T>)
        for (unsigned i = 0; i < 16; ++i)
        {
            const auto token = GraphicsEnumToken(static_cast<T>(i));
            if (!token.empty()) values.emplace_back(token);
        }
    return values;
}
double NumericStep(SettingId id)
{
    switch (id)
    {
    case SettingId::FrameLimitFps: return 30;
    case SettingId::TargetRenderFps: return 5;
    case SettingId::TextureBudgetMib: return 128;
    case SettingId::HdrPaperWhiteNits: return 10;
    case SettingId::HdrPeakNits: return 50;
    case SettingId::ManualScalePercent: case SettingId::DynamicMinPercent:
    case SettingId::DynamicMaxPercent: case SettingId::ShadowDistanceM:
    case SettingId::RtTraceDistanceM: return 5;
    case SettingId::Sharpening: case SettingId::BloomStrength:
    case SettingId::OutlineWidthPx: return .1;
    default: return 1;
    }
}
}
std::optional<std::string> NextGraphicsSettingToken(const GraphicsSettings &settings, SettingId id,
                                                   int direction, const GraphicsSettingsUiModel *model)
{
    const auto descriptors = GraphicsSettingDescriptors();
    const auto d = std::ranges::find(descriptors, id, &GraphicsSettingDescriptor::id);
    if (d == descriptors.end() || (direction != -1 && direction != 1)) return {};
    const auto current = GraphicsSettingToken(settings, id);
    std::vector<std::string> values;
    if (id == SettingId::FrameGenerationMultiplier)
    {
        if (!model || !model->frame_generation_maximum || *model->frame_generation_maximum < 2)
            return {}; // The pinned FSR API exposes no configurable multiplier.
        const auto maximum = *model->frame_generation_maximum;
        const auto next = direction < 0 ? (settings.frame_generation_multiplier > 2 ? settings.frame_generation_multiplier - 1 : 2)
            : (settings.frame_generation_multiplier < maximum ? settings.frame_generation_multiplier + 1 : maximum);
        const auto token = std::to_string(std::clamp(next, 2u, maximum));
        return token == current ? std::nullopt : std::optional(token);
    }
    if (d->type == GraphicsSettingType::Boolean) values = {"false", "true"};
    else if (d->type == GraphicsSettingType::Enum)
    {
        switch (id)
        {
#define HS_GRAPHICS_SETTING(number, name, field, type, kind, default_value, default_token, minimum, maximum, group, label_key) case SettingId::name: values = EnumChoices(settings.field); break;
#include <hs/core/graphics_settings.def>
#undef HS_GRAPHICS_SETTING
        }
    }
    else if (id == SettingId::AnisotropicFiltering) values = {"1", "2", "4", "8", "16"};
    else if (d->type == GraphicsSettingType::DisplayId || d->type == GraphicsSettingType::ProviderId ||
             d->type == GraphicsSettingType::ModeId || d->type == GraphicsSettingType::Extent)
    {
        if (d->type != GraphicsSettingType::ModeId) values.emplace_back("auto");
        if (model) for (const auto &choice : model->choices)
            if (choice.setting == id && std::ranges::find(values, choice.token) == values.end())
                values.push_back(choice.token);
        if (values.empty()) values.push_back(current);
    }
    else
    {
        double value = d->minimum;
        if (current != "auto")
        {
            const auto parsed = std::from_chars(current.data(), current.data() + current.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != current.data() + current.size() || !std::isfinite(value)) return {};
        }
        if (d->type == GraphicsSettingType::AutoFloat && direction < 0 && value <= d->minimum)
            return current == "auto" ? std::nullopt : std::optional(std::string("auto"));
        value = current == "auto" ? d->minimum : std::clamp(value + direction * NumericStep(id), d->minimum, d->maximum);
        std::array<char, 64> buffer{};
        std::to_chars_result written;
        if (d->type == GraphicsSettingType::Unsigned)
            written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), static_cast<std::uint32_t>(value));
        else written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), static_cast<float>(value), std::chars_format::general, 6);
        if (written.ec != std::errc{}) return {};
        const std::string token(buffer.data(), written.ptr);
        if (token == current) return {};
        auto candidate = settings;
        return SetGraphicsSettingToken(candidate, id, token) ? std::optional(token) : std::nullopt;
    }
    if (values.empty()) return {};
    const auto selected = std::ranges::find(values, current);
    const auto index = selected == values.end() ? (direction > 0 ? 0 : values.size() - 1)
        : static_cast<std::size_t>((static_cast<std::ptrdiff_t>(selected - values.begin()) + direction +
            static_cast<std::ptrdiff_t>(values.size())) % static_cast<std::ptrdiff_t>(values.size()));
    auto candidate = settings;
    return values[index] != current && SetGraphicsSettingToken(candidate, id, values[index]) ? std::optional(values[index]) : std::nullopt;
}

UiInteraction ResolveUiInteraction(const SessionProbe &session,
                                   PresentationUiState &ui,
                                   const SettingsData &settings,
                                   Float2 cursor_normalized,
                                   const GraphicsSettingsUiModel *graphics_ui)
{
    const auto cursor = Float2{(cursor_normalized.x + 1.0f) * 960.0f,
                               (1.0f - cursor_normalized.y) * 540.0f};
    const auto clicked = [&](float x, float y, float width, float height) {
        return cursor.x >= x && cursor.x <= x + width &&
               cursor.y >= y && cursor.y <= y + height;
    };
    const auto action = [](UiActionKind kind, std::uint8_t value = 0,
                           std::uint8_t secondary = 0) {
        return UiInteraction{UiAction{kind, value, secondary}, std::nullopt};
    };
    const auto command = [](UiCommandKind kind, std::uint32_t value) {
        return UiInteraction{std::nullopt, UiCommand{kind, value}};
    };
    const auto &probe = session;

    if (ui.page == UiPage::MainMenuSettings || ui.page == UiPage::PauseSettings)
    {
        for (std::uint8_t group = 0; group < 8; ++group)
            if (clicked(210.0f + group * 190.0f, 175, 180, 48))
            { ui.settings_group = group; ui.settings_page = 0; return {}; }
        if (clicked(1250, 900, 160, 56))
        { if (ui.settings_page != 0) --ui.settings_page; return {}; }
        if (clicked(1450, 900, 160, 56))
        { if (ui.settings_page + 1 < GraphicsSettingsPageCount(ui.settings_group)) ++ui.settings_page; return {}; }
        if (clicked(210, 900, 300, 56))
        { ui.page = UiPage::Root; return {std::nullopt, UiCommand{UiCommandKind::CancelSkillRebind, 0}}; }
        if (ui.settings_group < 7)
        {
            const GraphicsSettings defaults;
            const auto &requested = graphics_ui ? graphics_ui->requested : defaults;
            const auto rows = GraphicsSettingsVisibleRows(ui);
            for (std::size_t index = 0; index < rows.size(); ++index)
            {
                const auto y = kGraphicsUiRowY + index * kGraphicsUiRowStride;
                const int direction = clicked(1450, y, 64, 48) ? -1 : clicked(1550, y, 64, 48) ? 1 : 0;
                if (!direction) continue;
                const auto token = NextGraphicsSettingToken(requested, rows[index]->id, direction, graphics_ui);
                if (!token || token->size() >= UiCommand{}.graphics_token.size()) return {std::nullopt, std::nullopt, true};
                UiCommand edit{UiCommandKind::SetGraphicsSetting};
                edit.graphics_setting = rows[index]->id;
                std::copy(token->begin(), token->end(), edit.graphics_token.begin());
                return {std::nullopt, edit};
            }
            return {};
        }
        constexpr std::array volume_kinds{
            UiCommandKind::SetMasterVolumePercent, UiCommandKind::SetBgmVolumePercent,
            UiCommandKind::SetSfxVolumePercent, UiCommandKind::SetUiVolumePercent};
        const std::array volumes{settings.master_volume, settings.bgm_volume,
                                 settings.sfx_volume, settings.ui_volume};
        for (std::size_t index = 0; index < volumes.size(); ++index)
        {
            const auto y = kGraphicsUiRowY + index * kGraphicsUiRowStride;
            if (!clicked(1450, y, 64, 48) && !clicked(1550, y, 64, 48)) continue;
            const auto delta = cursor.x < 1514 ? -0.1f : 0.1f;
            const auto value = std::round(std::clamp(volumes[index] + delta,
                                                     0.0f, 1.0f) * 10.0f) / 10.0f;
            return command(volume_kinds[index],
                           static_cast<std::uint32_t>(std::lround(value * 100.0f)));
        }
        if (clicked(1450, kGraphicsUiRowY + 4 * kGraphicsUiRowStride, 64, 48) ||
            clicked(1550, kGraphicsUiRowY + 4 * kGraphicsUiRowStride, 64, 48))
        {
            const auto delta = cursor.x < 1514 ? -10 : 10;
            return command(UiCommandKind::SetVfxScreenOverlayIntensityPercent,
                static_cast<std::uint32_t>(std::clamp(static_cast<int>(std::lround(settings.screen_overlay_intensity * 100)) + delta, 0, 100)));
        }
        for (std::uint8_t slot = 0; slot < 4; ++slot)
            if (clicked(950, kGraphicsUiRowY + (slot + 5) * kGraphicsUiRowStride, 664, 48))
                return command(UiCommandKind::BeginSkillRebind, slot);
        return {};
    }

    if (probe.phase == SessionPhase::MainMenu)
    {
        if (ui.page == UiPage::Collection)
        {
            for (std::uint8_t skill = 0; skill < kCombatSkillCount; ++skill)
                if (clicked(210, 150.0f + skill * 70.0f, 360, 56))
                {
                    ui.selected_collection_skill = skill;
                    return {};
                }
            if (clicked(210, 900, 360, 56)) { ui.page = UiPage::Root; return {}; }
        }
        else
        {
            if (clicked(760, 270, 400, 92)) return action(UiActionKind::StartSession);
            if (clicked(760, 420, 400, 92)) { ui.page = UiPage::Collection; return {}; }
            if (clicked(760, 570, 400, 92)) { ui.page = UiPage::MainMenuSettings; return {}; }
            if (clicked(760, 720, 400, 92)) return action(UiActionKind::Quit);
        }
    }
    if (probe.phase == SessionPhase::CardSelection ||
        probe.phase == SessionPhase::RelicSelection)
    {
        if (clicked(760, 790, 400, 72)) return action(UiActionKind::Reroll);
        for (std::uint8_t index = 0; index < 3; ++index)
            if (clicked(360.0f + index * 420.0f, 300, 360, 420))
                return action(UiActionKind::SelectCard, index);
    }
    if (probe.phase == SessionPhase::StatAllocation)
    {
        for (std::uint8_t index = 0; index < kStatCount; ++index)
            if (clicked(390.0f + (index % 3) * 400.0f,
                        310.0f + (index / 3) * 260.0f, 340, 180))
                return action(UiActionKind::AssignStat, index);
    }
    if (probe.phase == SessionPhase::Paused &&
        (ui.page == UiPage::CharacterOverview ||
         ui.page == UiPage::CharacterSkills ||
         ui.page == UiPage::CharacterStats))
    {
        if (clicked(350, 140, 280, 58)) { ui.page = UiPage::CharacterOverview; return {}; }
        if (clicked(650, 140, 280, 58)) { ui.page = UiPage::CharacterSkills; return {}; }
        if (clicked(950, 140, 280, 58)) { ui.page = UiPage::CharacterStats; return {}; }
        if (clicked(1520, 140, 120, 58))
        {
            ui = {};
            return action(UiActionKind::Resume);
        }
        if (ui.page == UiPage::CharacterSkills)
        {
            for (std::uint8_t slot = 0; slot < 4; ++slot)
                if (clicked(780.0f + slot * 195.0f, 225, 180, 54))
                {
                    if (ui.loadout_source_slot == 0xFF)
                    {
                        if (probe.skill_loadout[slot] != SkillKind::Count)
                            ui.loadout_source_slot = slot;
                        return {};
                    }
                    if (ui.loadout_source_slot == slot)
                    {
                        ui.loadout_source_slot = 0xFF;
                        return {};
                    }
                    const auto source = std::exchange(ui.loadout_source_slot,
                                                      std::uint8_t{0xFF});
                    return action(UiActionKind::SwapLoadoutSlots, source, slot);
                }
            for (std::uint8_t skill = 0; skill < kCombatSkillCount; ++skill)
                if (clicked(350, 240.0f + skill * 78.0f, 360, 64))
                {
                    if (probe.skill_levels[skill] == 0)
                        return {std::nullopt, std::nullopt, true};
                    ui.selected_character_skill = skill;
                    return {};
                }
        }
    }
    if (probe.phase == SessionPhase::Paused && ui.page == UiPage::Root)
    {
        if (clicked(760, 420, 400, 72)) return action(UiActionKind::Resume);
        if (clicked(760, 520, 400, 72)) { ui.page = UiPage::PauseSettings; return {}; }
        if (clicked(760, 620, 400, 72)) return action(UiActionKind::Quit);
    }
    if (probe.phase == SessionPhase::Victory || probe.phase == SessionPhase::Defeat)
        return action(UiActionKind::ReturnToMainMenu);
    return {};
}

} // namespace hs
