#include <hs/presentation/projector.hpp>
#include <hs/core/cooked_format.hpp>

#include <array>
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
static_assert(static_cast<std::size_t>(DomainSignalKind::BossAnnouncement) ==
              kVfxAssets.size());

} // namespace

PresentationEvent ProjectPresentation(const DomainSignal &signal) noexcept
{
    PresentationEvent event;
    event.sequence = signal.sequence;
    event.tick = signal.tick;
    if (signal.kind == DomainSignalKind::BossAnnouncement)
    {
        event.kind = PresentationKind::Ui;
        event.asset = {0x626F73735F737061ull};
        return event;
    }
    if (signal.kind == DomainSignalKind::AbilityUsedAudio ||
        signal.kind == DomainSignalKind::ArrowReleasedAudio)
    {
        event.kind = PresentationKind::Audio;
        event.asset = {signal.kind == DomainSignalKind::AbilityUsedAudio
                           ? 0x736B696C6C5F7573ull
                           : 0x6172726F775F7368ull};
        return event;
    }
    event.kind = PresentationKind::Vfx;
    event.position = signal.position;
    event.asset = MakeAssetId(kVfxAssets[static_cast<std::size_t>(signal.kind)]);
    const auto has_target =
        (signal.flags & static_cast<std::uint8_t>(DomainSignalFlag::HasTarget)) != 0;
    event.parameters = EncodeVfxParameters(
        {signal.direction, signal.scale, signal.target,
         has_target ? static_cast<std::uint32_t>(VfxEventFlag::HasTarget) : 0u});
    return event;
}

} // namespace hs
