#include <hs/presentation/projector.hpp>

#include <algorithm>
#include <cmath>
#include <array>
#include <ranges>
#include <utility>

namespace hs
{
UiInteraction ResolveUiInteraction(const SessionProbe &session,
                                   PresentationUiState &ui,
                                   const SettingsData &settings,
                                   Float2 cursor_normalized)
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
        if (clicked(500, 250, 420, 56))
            return command(UiCommandKind::SetBorderless, !settings.borderless);
        if (clicked(500, 320, 420, 56))
            return command(UiCommandKind::SetVsync, !settings.vsync);
        if (clicked(500, 390, 420, 56))
        {
            constexpr std::array caps{30u, 60u, 120u, 0u};
            const auto current = std::ranges::find(caps, settings.frame_cap);
            const auto index = current == caps.end() ? 0u
                : static_cast<unsigned>(current - caps.begin() + 1) % caps.size();
            return command(UiCommandKind::SetFrameCap, caps[index]);
        }
        if (clicked(500, 460, 420, 56))
            return command(UiCommandKind::SetRenderScale,
                           settings.render_scale_percent == 100 ? 75 : 100);
        if (clicked(500, 530, 420, 56))
            return command(UiCommandKind::SetShadowResolution,
                           settings.shadow_resolution == 2048 ? 1024 : 2048);
        if (clicked(500, 600, 420, 56))
            return command(UiCommandKind::SetParticlePercentage,
                           settings.particle_percentage == 100 ? 50 : 100);
        if (clicked(500, 670, 420, 56))
            return command(UiCommandKind::SetBloom, !settings.bloom);
        if (clicked(500, 740, 420, 56))
            return command(UiCommandKind::SetOutline, !settings.outline);
        constexpr std::array volume_kinds{
            UiCommandKind::SetMasterVolumePercent, UiCommandKind::SetBgmVolumePercent,
            UiCommandKind::SetSfxVolumePercent, UiCommandKind::SetUiVolumePercent};
        const std::array volumes{settings.master_volume, settings.bgm_volume,
                                 settings.sfx_volume, settings.ui_volume};
        for (std::size_t index = 0; index < volumes.size(); ++index)
        {
            if (!clicked(1000, 250.0f + index * 70.0f, 420, 56)) continue;
            const auto delta = cursor.x < 1210 ? -0.1f : 0.1f;
            const auto value = std::round(std::clamp(volumes[index] + delta,
                                                     0.0f, 1.0f) * 10.0f) / 10.0f;
            return command(volume_kinds[index],
                           static_cast<std::uint32_t>(std::lround(value * 100.0f)));
        }
        for (std::uint8_t slot = 0; slot < 4; ++slot)
            if (clicked(1000, 550.0f + slot * 70.0f, 420, 56))
                return command(UiCommandKind::BeginSkillRebind, slot);
        if (clicked(760, 870, 400, 64))
        {
            ui.page = UiPage::Root;
            return {std::nullopt, UiCommand{UiCommandKind::CancelSkillRebind, 0}};
        }
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
                    if (probe.skill_levels[skill] > 0) ui.selected_character_skill = skill;
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
