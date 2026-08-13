#include <hs/presentation/projector.hpp>
#include <hs/core/cooked_format.hpp>

#include <array>
#include <algorithm>
#include <ranges>
#include <utility>
#include <cmath>
#include <string_view>

namespace hs
{
namespace
{

constexpr std::array<std::string_view, 64> kVfxAssets{
    "particle.basic_attack", "particle.boss.area.activate",
    "particle.boss.dash.impact", "particle.boss.dash.start",
    "particle.boss.phase_change", "particle.boss.shockwave.release",
    "particle.boss.spawn", "particle.boss.volley.release",
    "particle.common.enemy_death", "particle.common.explosion_large",
    "particle.common.explosion_small", "particle.common.heal",
    "particle.common.heavy_hit", "particle.common.hit",
    "particle.common.mark_apply", "particle.common.mark_trigger",
    "particle.common.player_death", "particle.common.player_hit",
    "particle.common.pull", "particle.common.push",
    "particle.enemy.melee.hit", "particle.enemy.melee.windup",
    "particle.enemy.ranged.release", "particle.enemy.suicide.charge",
    "particle.enemy.suicide.explosion", "particle.line.burn_transfer",
    "particle.line.relic_chain", "particle.line.ricochet",
    "particle.pickup.heal_collect", "particle.pickup.magnet_collect",
    "particle.pickup.relic_collect", "particle.pickup.xp_spawn",
    "particle.relic.bleed_burn_explosion", "particle.relic.combat_chain",
    "particle.relic.damage_push", "particle.relic.radial_arrows",
    "particle.skill.arrow_rain", "particle.skill.arrow_rain.area_pulse",
    "particle.skill.arrow_rain.impact", "particle.skill.charged_shot",
    "particle.skill.charged_shot.pulse", "particle.skill.charged_shot.ready",
    "particle.skill.damage_area.pulse", "particle.skill.explosive_arrow",
    "particle.skill.explosive_arrow.main",
    "particle.skill.explosive_arrow.secondary", "particle.skill.fire_area.pulse",
    "particle.skill.multishot", "particle.skill.piercing_shot",
    "particle.skill.piercing_shot.trail_pulse", "particle.skill.retreat_shot",
    "particle.skill.retreat_shot.land", "particle.skill.retreat_shot.move",
    "particle.skill.ricochet_arrow", "particle.skill.ricochet_arrow.hit",
    "particle.skill.trap", "particle.skill.trap.arm", "particle.skill.trap.trigger",
    "particle.status.bleed_apply", "particle.status.bleed_tick",
    "particle.status.burn_apply", "particle.status.burn_tick",
    "particle.status.slow_apply", "particle.status.slow_area"};
static_assert(static_cast<std::size_t>(DomainSignalKind::BossSpawnWarning) ==
              kVfxAssets.size());

} // namespace

std::size_t ProjectPresentation(const DomainSignal &signal,
                                std::span<PresentationEvent> output) noexcept
{
    if (output.empty()) return 0;
    auto &event = output.front();
    event.sequence = signal.sequence;
    event.tick = signal.tick;
    if (signal.kind == DomainSignalKind::BossSpawnWarning)
    {
        event.kind = PresentationKind::Ui;
        event.asset = {0x626F73735F737061ull};
        return 1;
    }
    if (signal.kind == DomainSignalKind::AbilityUsed ||
        signal.kind == DomainSignalKind::ArrowReleased)
    {
        event.kind = PresentationKind::Audio;
        event.asset = {signal.kind == DomainSignalKind::AbilityUsed
                           ? 0x736B696C6C5F7573ull
                           : 0x6172726F775F7368ull};
        return 1;
    }
    event.kind = PresentationKind::Vfx;
    event.position = signal.position;
    event.asset = MakeAssetId(kVfxAssets[static_cast<std::size_t>(signal.kind)]);
    const auto has_target =
        (signal.flags & static_cast<std::uint8_t>(DomainSignalFlag::HasTarget)) != 0;
    event.parameters = EncodeVfxParameters(
        {signal.direction, signal.scale, signal.target,
         has_target ? static_cast<std::uint32_t>(VfxEventFlag::HasTarget) : 0u});
    return 1;
}

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

    if (ui.page == 2 || ui.page == 6)
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
            ui.page = 0;
            return {std::nullopt, UiCommand{UiCommandKind::CancelSkillRebind, 0}};
        }
        return {};
    }

    if (probe.phase == SessionPhase::MainMenu)
    {
        if (ui.page == 1)
        {
            for (std::uint8_t skill = 0; skill < kCombatSkillCount; ++skill)
                if (clicked(210, 150.0f + skill * 70.0f, 360, 56))
                {
                    ui.collection_skill = skill;
                    return {};
                }
            if (clicked(210, 900, 360, 56)) { ui.page = 0; return {}; }
        }
        else
        {
            if (clicked(760, 270, 400, 92)) return action(UiActionKind::StartSession);
            if (clicked(760, 420, 400, 92)) { ui.page = 1; return {}; }
            if (clicked(760, 570, 400, 92)) { ui.page = 2; return {}; }
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
    if (probe.phase == SessionPhase::Paused && ui.page >= 3 && ui.page <= 5)
    {
        if (clicked(350, 140, 280, 58)) { ui.page = 3; return {}; }
        if (clicked(650, 140, 280, 58)) { ui.page = 4; return {}; }
        if (clicked(950, 140, 280, 58)) { ui.page = 5; return {}; }
        if (clicked(1520, 140, 120, 58))
        {
            ui = {};
            return action(UiActionKind::Resume);
        }
        if (ui.page == 4)
        {
            for (std::uint8_t slot = 0; slot < 4; ++slot)
                if (clicked(780.0f + slot * 195.0f, 225, 180, 54))
                {
                    if (ui.loadout_source == 0xFF)
                    {
                        if (probe.skill_loadout[slot] != SkillKind::Count)
                            ui.loadout_source = slot;
                        return {};
                    }
                    if (ui.loadout_source == slot)
                    {
                        ui.loadout_source = 0xFF;
                        return {};
                    }
                    const auto source = std::exchange(ui.loadout_source,
                                                      std::uint8_t{0xFF});
                    return action(UiActionKind::SwapLoadoutSlots, source, slot);
                }
            for (std::uint8_t skill = 0; skill < kCombatSkillCount; ++skill)
                if (clicked(350, 240.0f + skill * 78.0f, 360, 64))
                {
                    if (probe.skill_levels[skill] > 0) ui.character_skill = skill;
                    return {};
                }
        }
    }
    if (probe.phase == SessionPhase::Paused && ui.page == 0)
    {
        if (clicked(760, 420, 400, 72)) return action(UiActionKind::Resume);
        if (clicked(760, 520, 400, 72)) { ui.page = 6; return {}; }
        if (clicked(760, 620, 400, 72)) return action(UiActionKind::Quit);
    }
    if (probe.phase == SessionPhase::Victory || probe.phase == SessionPhase::Defeat)
        return action(UiActionKind::ReturnToMainMenu);
    return {};
}

} // namespace hs
