#include <hs/presentation/projector.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace hs
{
namespace
{
constexpr std::array<std::string_view, 64> kVfxAssets{
    "particle.basic_attack", "particle.boss.area.activate", "particle.boss.dash.impact", "particle.boss.dash.start", "particle.boss.phase_change", "particle.boss.shockwave.release", "particle.boss.spawn", "particle.boss.volley.release", "particle.common.enemy_death", "particle.common.explosion_large", "particle.common.explosion_small", "particle.common.heal", "particle.common.heavy_hit", "particle.common.hit", "particle.common.mark_apply", "particle.common.mark_trigger", "particle.common.player_death", "particle.common.player_hit", "particle.common.pull", "particle.common.push", "particle.enemy.melee.hit", "particle.enemy.melee.windup", "particle.enemy.ranged.release", "particle.enemy.suicide.charge", "particle.enemy.suicide.explosion", "particle.line.burn_transfer", "particle.line.relic_chain", "particle.line.ricochet", "particle.pickup.heal_collect", "particle.pickup.magnet_collect", "particle.pickup.relic_collect", "particle.pickup.xp_spawn", "particle.relic.bleed_burn_explosion", "particle.relic.combat_chain", "particle.relic.damage_push", "particle.relic.radial_arrows", "particle.skill.arrow_rain", "particle.skill.arrow_rain.area_pulse", "particle.skill.arrow_rain.impact", "particle.skill.charged_shot", "particle.skill.charged_shot.pulse", "particle.skill.charged_shot.ready", "particle.skill.damage_area.pulse", "particle.skill.explosive_arrow", "particle.skill.explosive_arrow.main", "particle.skill.explosive_arrow.secondary", "particle.skill.fire_area.pulse", "particle.skill.multishot", "particle.skill.piercing_shot", "particle.skill.piercing_shot.trail_pulse", "particle.skill.retreat_shot", "particle.skill.retreat_shot.land", "particle.skill.retreat_shot.move", "particle.skill.ricochet_arrow", "particle.skill.ricochet_arrow.hit", "particle.skill.trap", "particle.skill.trap.arm", "particle.skill.trap.trigger", "particle.status.bleed_apply", "particle.status.bleed_tick", "particle.status.burn_apply", "particle.status.burn_tick", "particle.status.slow_apply", "particle.status.slow_area"};
static_assert(static_cast<std::size_t>(DomainSignalKind::BossSpawnWarning) == kVfxAssets.size());

[[nodiscard]] AssetId Cue(std::string_view id) noexcept { return MakeAssetId(id); }
[[nodiscard]] std::string_view RelicTriggerVfx(std::uint8_t relic) noexcept
{
    switch (relic)
    {
    case static_cast<std::uint8_t>(RelicKind::ProjectileCadenceReward): return "particle.relic.projectile_cadence_reward";
    case static_cast<std::uint8_t>(RelicKind::PreDamageGuard): return "particle.relic.pre_damage_guard";
    case static_cast<std::uint8_t>(RelicKind::SlowSynergy): return "particle.relic.slow_synergy";
    case static_cast<std::uint8_t>(RelicKind::AreaResonance): return "particle.relic.area_resonance";
    case static_cast<std::uint8_t>(RelicKind::BossPressure): return "particle.relic.boss_pressure";
    case static_cast<std::uint8_t>(RelicKind::HitStreakReward): return "particle.relic.hit_streak_reward";
    case static_cast<std::uint8_t>(RelicKind::PickupReward): return "particle.relic.pickup_reward";
    case static_cast<std::uint8_t>(RelicKind::LowHealthSurvival): return "particle.relic.low_health_survival";
    default: return {};
    }
}
[[nodiscard]] std::string_view Release(std::uint8_t c) noexcept { switch (c) { case 1: return "audio.skill.piercing.release"; case 2: return "audio.skill.multishot.release"; case 3: return "audio.skill.charged.release"; case 4: return "audio.skill.explosive.release"; case 5: return "audio.skill.ricochet.release"; case 6: return "audio.skill.arrow_rain.cast"; case 7: return "audio.skill.trap.cast"; case 8: return "audio.skill.retreat.cast"; default: return "audio.skill.basic.release"; } }
[[nodiscard]] std::string_view Impact(std::uint8_t c) noexcept { switch (c) { case 1: return "audio.skill.piercing.pierce"; case 3: return "audio.skill.charged.hit"; case 4: return "audio.skill.explosive.main"; case 5: return "audio.skill.ricochet.hit"; default: return "audio.common.arrow_impact_light"; } }
void AddAudio(const DomainSignal &s, std::span<PresentationEvent> out, std::size_t &n, std::string_view cue, AudioEventAction action = AudioEventAction::Play) noexcept
{
    if (n >= out.size()) return;
    auto &e = out[n++]; e.sequence = s.sequence; e.tick = s.tick; e.kind = PresentationKind::Audio; e.position = s.position; e.asset = Cue(cue); e.parameters = EncodeAudioAction(action);
}
void AddVfx(const DomainSignal &s, std::span<PresentationEvent> out, std::size_t &n, std::string_view asset) noexcept
{
    if (n >= out.size()) return;
    auto &e = out[n++]; e.sequence = s.sequence; e.tick = s.tick; e.kind = PresentationKind::Vfx;
    e.position = s.position; e.asset = Cue(asset);
    const auto target = (s.flags & static_cast<std::uint8_t>(DomainSignalFlag::HasTarget)) != 0;
    e.parameters = EncodeVfxParameters({s.direction, s.scale, s.target,
        target ? static_cast<std::uint32_t>(VfxEventFlag::HasTarget) : 0u});
}
}

std::size_t ProjectDomainSignal(const DomainSignal &s, std::span<PresentationEvent> out) noexcept
{
    if (out.empty()) return 0;
    std::size_t n = 0;
    if (s.kind == DomainSignalKind::BossSpawnWarning || s.kind == DomainSignalKind::EnemySpawnWarning)
    {
        AddVfx(s, out, n, s.kind == DomainSignalKind::BossSpawnWarning ? "particle.boss.spawn" : "particle.enemy.spawn_warning");
        if (s.kind == DomainSignalKind::BossSpawnWarning)
        {
            AddAudio(s, out, n, "audio.boss.warning");
            AddAudio(s, out, n, "audio.stinger.boss_warning");
        }
        return n;
    }
    const auto index = static_cast<std::size_t>(s.kind);
    if (index < kVfxAssets.size())
    {
        auto &e = out[n++]; e.sequence = s.sequence; e.tick = s.tick; e.kind = PresentationKind::Vfx; e.position = s.position; e.asset = Cue(kVfxAssets[index]);
        const auto target = (s.flags & static_cast<std::uint8_t>(DomainSignalFlag::HasTarget)) != 0;
        e.parameters = EncodeVfxParameters({s.direction, s.scale, s.target, target ? static_cast<std::uint32_t>(VfxEventFlag::HasTarget) : 0u});
    }
    switch (s.kind)
    {
    case DomainSignalKind::BasicAttackImpact:
        AddAudio(s,out,n,"audio.skill.basic.impact"); AddAudio(s,out,n,"audio.common.arrow_impact_light"); break;
    case DomainSignalKind::BossAreaActivated: AddAudio(s,out,n,"audio.boss.area.activate"); break;
    case DomainSignalKind::BossDashImpact: AddAudio(s,out,n,"audio.boss.dash.impact"); break;
    case DomainSignalKind::BossDashStarted: AddAudio(s,out,n,"audio.boss.dash.start"); break;
    case DomainSignalKind::BossPhaseChanged: AddAudio(s,out,n,"audio.boss.phase_change"); break;
    case DomainSignalKind::BossShockwaveReleased: AddAudio(s,out,n,"audio.boss.shockwave.release"); break;
    case DomainSignalKind::BossSpawned:
        AddAudio(s,out,n,"audio.boss.spawn");
        if (s.context == 2) AddAudio(s,out,n,"audio.bgm.final_boss");
        break;
    case DomainSignalKind::BossVolleyReleased: AddAudio(s,out,n,"audio.boss.volley.release"); break;
    case DomainSignalKind::EnemyDied: AddAudio(s,out,n,"audio.enemy.death"); break;
    case DomainSignalKind::LargeExplosion: AddAudio(s,out,n,"audio.common.explosion_large"); break;
    case DomainSignalKind::SmallExplosion: AddAudio(s,out,n,"audio.common.explosion_small"); break;
    case DomainSignalKind::PlayerHealed: AddAudio(s,out,n,"audio.player.heal"); break;
    case DomainSignalKind::HeavyHit: AddAudio(s,out,n,"audio.common.heavy_hit"); break;
    case DomainSignalKind::ProjectileHit:
        if (s.context > 8) AddAudio(s,out,n,"audio.common.projectile_hit");
        else AddAudio(s,out,n,Impact(s.context));
        AddAudio(s,out,n,
            (s.context == 1 || s.context == 3 || s.context == 4) ? "audio.common.arrow_impact_heavy" : "audio.common.arrow_impact_light"); break;
    case DomainSignalKind::PlayerDied: AddAudio(s,out,n,"audio.player.death"); AddAudio(s,out,n,"audio.stinger.defeat"); break;
    case DomainSignalKind::PlayerDamaged: AddAudio(s,out,n,"audio.player.hit_body"); AddAudio(s,out,n,"audio.player.hit_vocal"); break;
    case DomainSignalKind::Pull: AddAudio(s,out,n,"audio.common.pull"); break;
    case DomainSignalKind::Push: AddAudio(s,out,n,"audio.common.push"); break;
    case DomainSignalKind::MeleeEnemyHit: AddAudio(s,out,n,"audio.enemy.melee.hit"); break;
    case DomainSignalKind::MeleeEnemyWindup: AddAudio(s,out,n,"audio.enemy.melee.windup"); break;
    case DomainSignalKind::RangedEnemyReleased: AddAudio(s,out,n,"audio.enemy.ranged.release"); break;
    case DomainSignalKind::SuicideEnemyCharging: AddAudio(s,out,n,"audio.enemy.suicide.charge"); break;
    case DomainSignalKind::SuicideEnemyExploded: AddAudio(s,out,n,"audio.enemy.suicide.explosion"); break;
    case DomainSignalKind::BurnTransferred: AddAudio(s,out,n,"audio.status.burn_transfer"); break;
    case DomainSignalKind::RelicChainLinked: AddAudio(s,out,n,"audio.relic.combat_chain"); break;
    case DomainSignalKind::RicochetLinked: AddAudio(s,out,n,"audio.skill.ricochet.bounce"); break;
    case DomainSignalKind::HealCollected: AddAudio(s,out,n,"audio.pickup.heal"); break;
    case DomainSignalKind::MagnetCollected: AddAudio(s,out,n,"audio.pickup.magnet"); break;
    case DomainSignalKind::RelicCollected: AddAudio(s,out,n,"audio.pickup.relic_chest"); break;
    case DomainSignalKind::ExperienceCollected: AddVfx(s,out,n,"particle.pickup.xp_collect"); AddAudio(s,out,n,"audio.pickup.xp_collect"); break;
    case DomainSignalKind::BleedBurnExploded: AddAudio(s,out,n,"audio.relic.bleed_burn_explosion"); break;
    case DomainSignalKind::CombatChainHit: AddAudio(s,out,n,"audio.relic.combat_chain"); break;
    case DomainSignalKind::DamagePush: AddAudio(s,out,n,"audio.relic.damage_push"); break;
    case DomainSignalKind::RadialArrowsCast: AddAudio(s,out,n,"audio.relic.radial_arrows"); break;
    case DomainSignalKind::ArrowRainCast: AddAudio(s,out,n,"audio.skill.arrow_rain.cast"); AddAudio(s,out,n,"audio.skill.arrow_rain.incoming"); break;
    case DomainSignalKind::ArrowRainImpact: AddAudio(s,out,n,"audio.skill.arrow_rain.impact"); break;
    case DomainSignalKind::ArrowRainPulse: AddAudio(s,out,n,"audio.skill.arrow_rain.incoming"); AddAudio(s,out,n,"audio.skill.arrow_rain.impact"); break;
    case DomainSignalKind::ChargedShotStarted: AddAudio(s,out,n,"audio.player.bow_draw_heavy"); AddAudio(s,out,n,"audio.skill.charged.start"); AddAudio(s,out,n,"audio.skill.charged.loop"); break;
    case DomainSignalKind::ChargedShotEnded: AddAudio(s,out,n,"audio.skill.charged.loop",AudioEventAction::Stop); break;
    case DomainSignalKind::ChargedShotReady: AddAudio(s,out,n,"audio.skill.charged.ready"); break;
    case DomainSignalKind::ExplosiveArrowMain: AddAudio(s,out,n,"audio.skill.explosive.main"); break;
    case DomainSignalKind::ExplosiveArrowSecondary: AddAudio(s,out,n,"audio.skill.explosive.secondary"); break;
    case DomainSignalKind::MultiShotCast: AddAudio(s,out,n,"audio.skill.multishot.release"); break;
    case DomainSignalKind::PiercingShotCast: AddAudio(s,out,n,"audio.skill.piercing.release"); break;
    case DomainSignalKind::RetreatShotCast: AddAudio(s,out,n,"audio.skill.retreat.cast"); break;
    case DomainSignalKind::RetreatLanded: AddAudio(s,out,n,"audio.skill.retreat.land"); AddAudio(s,out,n,"audio.player.land"); break;
    case DomainSignalKind::RetreatMoved: AddAudio(s,out,n,"audio.skill.retreat.move"); AddAudio(s,out,n,"audio.player.move_whoosh"); break;
    case DomainSignalKind::RicochetArrowCast: AddAudio(s,out,n,"audio.skill.ricochet.release"); break;
    case DomainSignalKind::RicochetArrowHit: AddAudio(s,out,n,"audio.skill.ricochet.hit"); break;
    case DomainSignalKind::TrapCast: AddAudio(s,out,n,"audio.skill.trap.cast"); break;
    case DomainSignalKind::TrapArmed: AddAudio(s,out,n,"audio.skill.trap.arm"); break;
    case DomainSignalKind::TrapTriggered: AddAudio(s,out,n,"audio.skill.trap.trigger"); break;
    case DomainSignalKind::BleedApplied: AddAudio(s,out,n,"audio.status.bleed_apply"); break;
    case DomainSignalKind::BurnApplied: AddAudio(s,out,n,"audio.status.burn_apply"); break;
    case DomainSignalKind::SlowApplied: AddAudio(s,out,n,"audio.status.slow_apply"); break;
    case DomainSignalKind::MarkApplied: AddAudio(s,out,n,"audio.status.mark_apply"); break;
    case DomainSignalKind::MarkTriggered: AddAudio(s,out,n,"audio.status.mark_trigger"); break;
    case DomainSignalKind::AbilityUsed: break;
    case DomainSignalKind::ArrowReleased:
        AddAudio(s,out,n,Release(s.context));
        AddAudio(s,out,n,(s.context == 1 || s.context == 3 || s.context == 4)
                              ? "audio.player.arrow_release_heavy"
                              : "audio.player.arrow_release_light"); break;
    case DomainSignalKind::BasicAttackStarted: AddAudio(s,out,n,"audio.player.bow_draw_short"); break;
    case DomainSignalKind::TrapDamaged: AddAudio(s,out,n,"audio.skill.trap.explosion"); break;
    case DomainSignalKind::CooldownSurged: AddVfx(s,out,n,"particle.relic.cooldown_surge"); AddAudio(s,out,n,"audio.relic.cooldown_surge"); break;
    case DomainSignalKind::TrackingArrowFired: AddVfx(s,out,n,"particle.relic.tracking_arrow"); AddAudio(s,out,n,"audio.relic.tracking_arrow"); break;
    case DomainSignalKind::AfterimageArrowFired: AddVfx(s,out,n,"particle.relic.afterimage_arrow"); AddAudio(s,out,n,"audio.relic.afterimage_arrow"); break;
    case DomainSignalKind::CooldownRefunded: AddVfx(s,out,n,"particle.relic.cooldown_refund"); AddAudio(s,out,n,"audio.relic.cooldown_refund"); break;
    case DomainSignalKind::PlayerRevived: AddVfx(s,out,n,"particle.relic.revive"); AddAudio(s,out,n,"audio.relic.revive"); break;
    case DomainSignalKind::BossDashTelegraphed: AddAudio(s,out,n,"audio.boss.dash.telegraph"); break;
    case DomainSignalKind::BossVolleyTelegraphed: AddAudio(s,out,n,"audio.boss.volley.telegraph"); break;
    case DomainSignalKind::BossAreaTelegraphed: AddAudio(s,out,n,"audio.boss.area.telegraph"); break;
    case DomainSignalKind::BossShockwaveTelegraphed: AddAudio(s,out,n,"audio.boss.shockwave.telegraph"); break;
    case DomainSignalKind::BossDied: AddVfx(s,out,n,"particle.boss.death"); AddAudio(s,out,n,"audio.boss.death"); break;
    case DomainSignalKind::SkillUnlocked: AddAudio(s,out,n,"audio.ui.unlock"); break;
    case DomainSignalKind::RelicTriggered:
        if (const auto asset = RelicTriggerVfx(s.context); !asset.empty()) AddVfx(s, out, n, asset);
        break;
    default: break;
    }
    return n;
}
}
