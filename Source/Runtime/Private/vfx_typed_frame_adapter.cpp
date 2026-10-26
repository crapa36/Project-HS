#include "vfx_typed_frame_adapter.hpp"
#include "vfx_generation_rebind.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace hs::runtime_detail
{
namespace
{
constexpr std::uint32_t Hash32(std::string_view value) noexcept
{
    std::uint32_t hash = 2166136261u;
    for (const unsigned char character : value)
    {
        hash ^= character;
        hash *= 16777619u;
    }
    return hash;
}

constexpr std::uint64_t Hash64(std::string_view value) noexcept
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char character : value)
    {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    return hash;
}

constexpr std::uint32_t kPointPayload = Hash32("PointEventPayload");
constexpr std::uint32_t kContextPayload = Hash32("PresentationContextPayload");
constexpr std::uint32_t kLinkPayload = Hash32("SourceTargetPayload");
constexpr std::uint32_t kEntityPayload = Hash32("EntityAttachmentPayload");
constexpr std::uint32_t kCirclePayload = Hash32("CircleAreaPayload");
constexpr std::uint32_t kLinePayload = Hash32("LineAreaPayload");
constexpr std::uint32_t kConePayload = Hash32("ConePayload");
constexpr std::uint32_t kRingGapsPayload = Hash32("RingWithGapsPayload");
constexpr std::uint32_t kSafeSectorPayload = Hash32("SafeGapSectorPayload");
constexpr std::uint64_t kSafeMarkerEffect = Hash64("particle.boss.shockwave.safe_gap_marker");
constexpr std::uint32_t kProjectilePathPayload = Hash32("ProjectilePathPayload");
constexpr std::uint32_t kProjectilePayload = Hash32("ProjectilePayload");
constexpr std::uint64_t kBossDashWarningEffect = Hash64("particle.boss.dash.telegraph");
constexpr std::uint64_t kBossVolleyWarningEffect = Hash64("particle.boss.volley.telegraph");
constexpr std::uint64_t kRangedWarningEffect = Hash64("particle.enemy.ranged.telegraph_line");
constexpr std::uint64_t kBleedTickEffect = Hash64("particle.status.bleed_tick");
constexpr std::uint64_t kBurnTickEffect = Hash64("particle.status.burn_tick");
constexpr std::uint64_t kArrowRainPullEffect = Hash64("particle.upgrade.arrow_rain.pull");
constexpr std::uint64_t kPickupXpCollectEffect = Hash64("particle.pickup.xp_collect");
constexpr std::uint64_t kPickupHealCollectEffect = Hash64("particle.pickup.heal_collect");
constexpr std::uint64_t kPickupMagnetCollectEffect = Hash64("particle.pickup.magnet_collect");
constexpr std::uint64_t kPickupRelicCollectEffect = Hash64("particle.pickup.relic_collect");
constexpr std::uint64_t kPickupXpIdleEffect = Hash64("particle.pickup.xp.idle");
constexpr std::uint64_t kPickupHealIdleEffect = Hash64("particle.pickup.heal.idle");
constexpr std::uint64_t kPickupMagnetIdleEffect = Hash64("particle.pickup.magnet.idle");
constexpr std::uint64_t kPickupRelicIdleEffect = Hash64("particle.pickup.relic_chest.idle");
constexpr std::uint64_t kChargedOverchargeLoopEffect = Hash64("particle.upgrade.charged.overcharge_loop");
constexpr std::uint64_t kArrowRainIncomingEffect = Hash64("particle.upgrade.arrow_rain.incoming_arrow");
constexpr std::uint64_t kMultishotRetargetEffect = Hash64("particle.upgrade.retarget");
constexpr std::uint64_t kRicochetBleedExtendEffect = Hash64("particle.upgrade.ricochet.bleed_extend");

struct PersistentBinding
{
    PersistentVfxKind kind;
    std::uint64_t effect_id;
    std::uint32_t payload_kind;
};

struct UpgradeSlowAreaAlias
{
    std::uint8_t skill;
    std::uint8_t source_upgrade;
    std::uint64_t skill_id;
    std::uint32_t ordinal;
    std::uint64_t effect_id;
};

struct DamageTrailSupplement
{
    std::uint8_t skill;
    std::uint8_t source_upgrade;
    std::uint64_t skill_id;
    std::uint32_t ordinal;
    std::uint64_t effect_id;
    bool fixed_duration;
};

// Core keeps the gameplay skill field opaque; these are its stable SkillKind
// ordinals from the gameplay ABI.
constexpr std::uint8_t kArrowRainSkill = 6;
constexpr std::uint8_t kTrapSkill = 7;
constexpr std::uint8_t kPiercingShotSkill = 1;
constexpr std::uint8_t kMultishotSkill = 2;
constexpr std::uint8_t kRicochetSkill = 5;
constexpr std::uint8_t kRetreatShotSkill = 8;

constexpr std::array kUpgradeSlowAreaAliases{
    UpgradeSlowAreaAlias{kArrowRainSkill, 7, Hash64("skill.arrow_rain"), 8,
                         Hash64("particle.upgrade.arrow_rain.finish_slow")},
    UpgradeSlowAreaAlias{kTrapSkill, 1, Hash64("skill.trap"), 2,
                         Hash64("particle.upgrade.trap.land_slow")},
};

constexpr std::array kDamageTrailSupplements{
    DamageTrailSupplement{kRetreatShotSkill, 2, Hash64("skill.retreat_shot"), 3,
                          Hash64("particle.upgrade.retreat.slow_trail"), false},
    DamageTrailSupplement{kPiercingShotSkill, 3, Hash64("skill.piercing_shot"), 4,
                          Hash64("particle.skill.piercing_shot.trail_pulse"), true},
};

constexpr std::array kPersistentBindings{
    PersistentBinding{PersistentVfxKind::EnemyBleedStatus, Hash64("persistent.status.bleed"), kEntityPayload},
    PersistentBinding{PersistentVfxKind::EnemyBurnStatus, Hash64("persistent.status.burn"), kEntityPayload},
    PersistentBinding{PersistentVfxKind::EnemySlowStatus, Hash64("persistent.status.slow"), kEntityPayload},
    PersistentBinding{PersistentVfxKind::EnemyMarkStatus, Hash64("persistent.status.mark"), kEntityPayload},
    PersistentBinding{PersistentVfxKind::RicochetLink, Hash64("particle.line.ricochet"), kLinkPayload},
    PersistentBinding{PersistentVfxKind::RicochetReturnLink, Hash64("particle.upgrade.ricochet.return"), kLinkPayload},
    PersistentBinding{PersistentVfxKind::BurnTransferLink, Hash64("particle.line.burn_transfer"), kLinkPayload},
    PersistentBinding{PersistentVfxKind::RelicChainLink, Hash64("particle.line.relic_chain"), kLinkPayload},
    PersistentBinding{PersistentVfxKind::TrapPending, Hash64("persistent.trap.pending"), kCirclePayload},
    PersistentBinding{PersistentVfxKind::TrapArmed, Hash64("persistent.trap.armed"), kCirclePayload},
    PersistentBinding{PersistentVfxKind::FireArea, Hash64("persistent.area.fire"), kCirclePayload},
    // The snapshot projector's SlowArea is the status area effect. Keep this
    // binding explicit because persistent.area.slow is a separate authored
    // effect for upgrade authored areas.
    PersistentBinding{PersistentVfxKind::SlowArea, Hash64("particle.status.slow_area"), kCirclePayload},
    PersistentBinding{PersistentVfxKind::UpgradeSlowArea, Hash64("persistent.area.slow"), kCirclePayload},
    PersistentBinding{PersistentVfxKind::ArrowRainArea, Hash64("persistent.area.arrow_rain"), kCirclePayload},
    PersistentBinding{PersistentVfxKind::DamageTrail, Hash64("persistent.trail.damage"), kLinePayload},
    PersistentBinding{PersistentVfxKind::ChargeGuide, Hash64("persistent.charge_guide"), kLinePayload},
    PersistentBinding{PersistentVfxKind::RangeIndicator, Hash64("persistent.range_indicator"), kCirclePayload},
    PersistentBinding{PersistentVfxKind::ProjectileTrail, Hash64("persistent.projectile_trail"), kProjectilePathPayload},
    PersistentBinding{PersistentVfxKind::ProjectileTrailOuter, Hash64("persistent.projectile_trail_outer"), kProjectilePathPayload},
    PersistentBinding{PersistentVfxKind::RicochetProjectileTrail, Hash64("persistent.ricochet_projectile_trail"), kProjectilePathPayload},
    PersistentBinding{PersistentVfxKind::BossAreaActive, Hash64("particle.boss.area.active_loop"), kCirclePayload},
    PersistentBinding{PersistentVfxKind::BossShockwaveWavefront, Hash64("particle.boss.shockwave.wavefront"), kRingGapsPayload},
    PersistentBinding{PersistentVfxKind::BossDashWake, Hash64("persistent.boss.dash_wake"), kLinePayload},
    PersistentBinding{PersistentVfxKind::BossPhase2Aura, Hash64("persistent.boss.phase2_aura"), kEntityPayload},
    PersistentBinding{PersistentVfxKind::BossPhaseTransition, Hash64("persistent.boss.phase_transition_invulnerable"), kEntityPayload},
    PersistentBinding{PersistentVfxKind::PlayerInvulnerableLoop, Hash64("particle.player.invulnerable_loop"), kEntityPayload},
    PersistentBinding{PersistentVfxKind::PlayerBowDraw, Hash64("particle.player.bow_draw"), kEntityPayload},
    PersistentBinding{PersistentVfxKind::ChargedFullReady, Hash64("particle.upgrade.charged.full_ready"), kEntityPayload},
    PersistentBinding{PersistentVfxKind::ChargedOverchargeLoop, kChargedOverchargeLoopEffect, kEntityPayload},
    PersistentBinding{PersistentVfxKind::SuicideCountdownLoop, Hash64("particle.enemy.suicide.countdown_loop"), kEntityPayload},
    PersistentBinding{PersistentVfxKind::EmpoweredReady, Hash64("particle.upgrade.empowered_ready"), kEntityPayload},
    PersistentBinding{PersistentVfxKind::MultishotRetarget, kMultishotRetargetEffect, kProjectilePathPayload},
    PersistentBinding{PersistentVfxKind::BasicArrowReturn, Hash64("particle.upgrade.arrow.return"), kProjectilePathPayload},
    PersistentBinding{PersistentVfxKind::RicochetBleedExtend, kRicochetBleedExtendEffect, kEntityPayload},
    PersistentBinding{PersistentVfxKind::PickupXpIdle, kPickupXpIdleEffect, kEntityPayload},
    PersistentBinding{PersistentVfxKind::PickupHealIdle, kPickupHealIdleEffect, kEntityPayload},
    PersistentBinding{PersistentVfxKind::PickupMagnetIdle, kPickupMagnetIdleEffect, kEntityPayload},
    PersistentBinding{PersistentVfxKind::PickupRelicIdle, kPickupRelicIdleEffect, kEntityPayload},
    PersistentBinding{PersistentVfxKind::PickupXpMagnetTrail, Hash64("particle.pickup.xp.magnet_trail"), kLinkPayload},
    PersistentBinding{PersistentVfxKind::PlayerLowHealthVignette, Hash64("post.player.low_health_vignette"), kContextPayload},
};

const VfxEffectLookupRecord *FindLookup(const VfxProgramView &cooked,
                                        std::uint64_t effect_id) noexcept
{
    const auto found = std::find_if(cooked.effect_lookup.begin(), cooked.effect_lookup.end(),
        [effect_id](const VfxEffectLookupRecord &entry) { return entry.effect_id == effect_id; });
    return found == cooked.effect_lookup.end() ? nullptr : &*found;
}

const VfxEffectRecord *FindEffect(const VfxProgramView &cooked,
                                  std::uint32_t handle) noexcept
{
    if (handle == 0 || handle > cooked.effects.size()) return nullptr;
    const auto &effect = cooked.effects[handle - 1];
    return effect.handle == handle ? &effect : nullptr;
}

const UpgradeSlowAreaAlias *FindUpgradeSlowAreaAlias(
    const PersistentVfxVisual &visual) noexcept
{
    if (visual.kind != PersistentVfxKind::UpgradeSlowArea || visual.cast_id == 0)
        return nullptr;
    const auto found = std::find_if(kUpgradeSlowAreaAliases.begin(),
                                    kUpgradeSlowAreaAliases.end(),
        [&visual](const UpgradeSlowAreaAlias &alias) {
            return alias.skill == visual.skill &&
                alias.source_upgrade == visual.source_upgrade;
        });
    return found == kUpgradeSlowAreaAliases.end() ? nullptr : &*found;
}

const DamageTrailSupplement *FindDamageTrailSupplement(
    const PersistentVfxVisual &visual) noexcept
{
    if (visual.kind != PersistentVfxKind::DamageTrail || visual.cast_id == 0)
        return nullptr;
    const auto found = std::find_if(kDamageTrailSupplements.begin(),
                                    kDamageTrailSupplements.end(),
        [&visual](const DamageTrailSupplement &supplement) {
            return supplement.skill == visual.skill &&
                supplement.source_upgrade == visual.source_upgrade;
        });
    return found == kDamageTrailSupplements.end() ? nullptr : &*found;
}

bool UpgradeBindingContains(const VfxProgramView &cooked,
                            std::uint64_t skill_id,
                            std::uint32_t ordinal,
                            std::uint32_t effect_handle) noexcept
{
    const auto binding = std::find_if(cooked.upgrade_bindings.begin(),
                                      cooked.upgrade_bindings.end(),
        [skill_id, ordinal](const VfxUpgradeBindingRecord &candidate) {
            return candidate.skill_id == skill_id &&
                candidate.ordinal == ordinal;
        });
    if (binding == cooked.upgrade_bindings.end() ||
        binding->sequence.first > cooked.upgrade_sequences.size() ||
        binding->sequence.count >
            cooked.upgrade_sequences.size() - binding->sequence.first)
        return false;
    const auto sequence = std::span(cooked.upgrade_sequences).subspan(
        binding->sequence.first, binding->sequence.count);
    return std::find(sequence.begin(), sequence.end(), effect_handle) !=
        sequence.end();
}

bool Finite(const Float3 &value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool Finite(const VfxEventParameters &value) noexcept
{
    return Finite(value.direction) && Finite(value.target) &&
        std::isfinite(value.scale);
}

bool Finite(const PresentationGeometry &value) noexcept
{
    return std::isfinite(value.radius) && std::isfinite(value.inner_radius) &&
        std::isfinite(value.outer_radius) && std::isfinite(value.width) &&
        std::isfinite(value.range) && std::isfinite(value.half_angle_degrees) &&
        std::isfinite(value.gap_offset_degrees) &&
        std::isfinite(value.gap_half_width_degrees) && Finite(value.end_position) &&
        Finite(value.velocity);
}

bool MakeEventGeometry(const PresentationEvent &event,
                       const VfxEventParameters &parameters,
                       std::uint32_t payload_kind,
                       VfxPayload &payload) noexcept
{
    const auto &geometry = event.geometry;
    if (geometry.kind == PresentationGeometryKind::None)
        return false;
    if (!Finite(geometry)) return false;
    const auto direction = parameters.direction;
    const auto direction_length = std::sqrt(direction.x * direction.x +
                                            direction.y * direction.y +
                                            direction.z * direction.z);
    switch (geometry.kind)
    {
    case PresentationGeometryKind::Circle:
        if (payload_kind != kCirclePayload || geometry.radius <= 0.0f ||
            geometry.outer_radius != 0.0f || geometry.inner_radius != 0.0f)
            return false;
        payload = VfxCirclePayload{event.position, direction, geometry.radius,
                                   0.0f, 0.0f, 0};
        return true;
    case PresentationGeometryKind::Line:
    {
        if (geometry.width <= 0.0f ||
            (geometry.end_position.x == event.position.x &&
             geometry.end_position.y == event.position.y &&
             geometry.end_position.z == event.position.z))
            return false;
        if (payload_kind == kLinePayload)
        {
            if (direction_length <= 0.0001f) return false;
            payload = VfxLinePayload{event.position, geometry.end_position,
                                     direction, geometry.width, 0.0f, 0};
            return true;
        }
        if (payload_kind != kLinkPayload || geometry.source_id == 0 ||
            event.upgrade_owner_id == 0 || event.upgrade_cast_id == 0 ||
            geometry.source_id != event.upgrade_owner_id)
            return false;
        const auto link_length = std::hypot(
            geometry.end_position.x - event.position.x,
            std::hypot(geometry.end_position.y - event.position.y,
                       geometry.end_position.z - event.position.z));
        if (!std::isfinite(link_length) || link_length <= 0.0001f)
            return false;
        payload = VfxLinkPayload{event.position, geometry.end_position,
                                 geometry.source_id, event.upgrade_owner_id,
                                 0.0f, geometry.width, 0};
        return true;
    }
    case PresentationGeometryKind::Cone:
        if (payload_kind != kConePayload || direction_length <= 0.0001f ||
            geometry.range <= 0.0f ||
            geometry.half_angle_degrees <= 0.0f ||
            geometry.half_angle_degrees > 180.0f)
            return false;
        payload = VfxConePayload{event.position, direction, geometry.range,
                                 geometry.half_angle_degrees, 0.0f, 0};
        return true;
    case PresentationGeometryKind::RingGaps:
        if (payload_kind != kRingGapsPayload || geometry.inner_radius < 0.0f ||
            geometry.outer_radius <= geometry.inner_radius ||
            (geometry.gap_count > 0 &&
                (geometry.gap_half_width_degrees <= 0.0f ||
                 geometry.gap_half_width_degrees > 180.0f)))
            return false;
        {
            VfxRingGapsPayload ring;
            ring.center = event.position;
            ring.inner_radius = geometry.inner_radius;
            ring.outer_radius = geometry.outer_radius;
            ring.gap_half_width_degrees = geometry.gap_half_width_degrees;
            ring.lifetime01 = 0.0f;
            ring.gap_angles_degrees.reserve(geometry.gap_count);
            if (geometry.gap_count > 0)
            {
                const auto spacing = 360.0f / static_cast<float>(geometry.gap_count);
                for (std::uint8_t i = 0; i < geometry.gap_count; ++i)
                    ring.gap_angles_degrees.push_back(
                        geometry.gap_offset_degrees + spacing * static_cast<float>(i));
            }
            payload = std::move(ring);
        }
        return true;
    case PresentationGeometryKind::Projectile:
        if (payload_kind != kProjectilePayload || !Finite(geometry.velocity) ||
            geometry.radius <= 0.0f ||
            std::hypot(geometry.velocity.x,
                       std::hypot(geometry.velocity.y, geometry.velocity.z)) <= 0.0001f)
            return false;
        payload = VfxProjectilePayload{geometry.velocity,
            {geometry.radius, geometry.radius, geometry.radius},
            geometry.radius, 0.0f, 0};
        return true;
    case PresentationGeometryKind::None:
    default:
        return false;
    }
}

VfxTransform Transform(const Float3 &position, float yaw) noexcept
{
    const float cosine = std::cos(yaw);
    const float sine = std::sin(yaw);
    VfxTransform value = kVfxIdentityTransform;
    // XMFLOAT4X4 and the renderer's transform ABI use _41/_42/_43 for the
    // translation row. This also keeps identity and translation stable for
    // consumers that only read the owner envelope.
    value[0] = cosine;
    value[2] = -sine;
    value[8] = sine;
    value[10] = cosine;
    value[12] = position.x;
    value[13] = position.y;
    value[14] = position.z;
    return value;
}

std::uint32_t EventSeed(Sequence sequence) noexcept
{
    const auto low = static_cast<std::uint32_t>(sequence);
    const auto high = static_cast<std::uint32_t>(sequence >> 32);
    return low ^ (high * 0x9e3779b9u);
}

std::uint32_t PersistentSeed(std::uint64_t stable_id,
                             VfxEffectHandle effect) noexcept
{
    return static_cast<std::uint32_t>(stable_id) ^
        static_cast<std::uint32_t>(stable_id >> 32) ^ (effect * 0x9e3779b9u);
}

std::uint64_t SupplementalStableId(std::uint64_t stable_id,
                                   std::uint64_t effect_id) noexcept
{
    // Keep the pulse in a separate renderer state slot while deriving its
    // identity from the same authoritative area owner.
    return stable_id ^ effect_id ^ 0xd6e8feb86659fd93ull;
}

void Diagnose(VfxTypedFrameInputs &result, std::uint64_t effect_id)
{
    result.unsupported_effect_ids.push_back(effect_id);
}

std::optional<float> Lifetime(const PersistentVfxVisual &visual,
                             Tick current_tick) noexcept
{
    if (visual.kind == PersistentVfxKind::PickupXpMagnetTrail)
    {
        if (visual.active_tick > current_tick || current_tick >= visual.expires ||
            !std::isfinite(visual.radius) || visual.radius <= 0 ||
            !std::isfinite(visual.length) || visual.length < 0) return {};
        return std::clamp(1.0f - visual.length / visual.radius, 0.0f,
                          std::nextafter(1.0f, 0.0f));
    }
    if (visual.kind == PersistentVfxKind::BossShockwaveWavefront &&
        current_tick < visual.active_tick)
        return std::nullopt;
    if (visual.kind == PersistentVfxKind::ChargeGuide)
        return visual.active_tick <= current_tick ? std::optional<float>{0.0f} : std::nullopt;
    if (visual.kind == PersistentVfxKind::BossPhase2Aura)
        return visual.active_tick <= current_tick ? std::optional<float>{0.5f} : std::nullopt;
    if (visual.kind == PersistentVfxKind::PlayerBowDraw && visual.expires == 0)
        return visual.active_tick <= current_tick ? std::optional<float>{0.0f} : std::nullopt;
    if (visual.kind == PersistentVfxKind::ChargedOverchargeLoop)
    {
        if (visual.active_tick > current_tick ||
            visual.expires <= visual.active_tick || current_tick >= visual.expires ||
            !std::isfinite(visual.charge_ratio) || visual.charge_ratio < 0.0f ||
            visual.charge_ratio > 1.0f)
            return std::nullopt;
        return visual.charge_ratio;
    }
    if (visual.kind == PersistentVfxKind::PickupXpIdle ||
        visual.kind == PersistentVfxKind::PickupHealIdle ||
        visual.kind == PersistentVfxKind::PickupMagnetIdle ||
        visual.kind == PersistentVfxKind::PickupRelicIdle)
        return visual.active_tick <= current_tick ? std::optional<float>{0.0f} : std::nullopt;
    if (visual.projectile_owner_id != 0)
    {
        if (visual.projectile_spawned_tick > current_tick)
            return std::nullopt;
        return 0.0f; // live actor presence is the authoritative lifetime.
    }
    if (visual.expires <= visual.active_tick ||
        current_tick >= visual.expires)
        return std::nullopt;
    const auto duration = visual.expires - visual.active_tick;
    const auto elapsed = current_tick > visual.active_tick
        ? current_tick - visual.active_tick : 0;
    return std::clamp(static_cast<float>(elapsed) /
                          static_cast<float>(duration), 0.0f, 1.0f);
}
const PersistentVfxVisual *FindPrevious(
    std::span<const PersistentVfxVisual> previous,
    const PersistentVfxVisual &visual,
    const PreparedPairFacts *pair_facts) noexcept
{
    if (pair_facts) return pair_facts->Previous(visual, previous);
    const auto found = std::find_if(previous.begin(), previous.end(),
        [&visual](const PersistentVfxVisual &candidate) {
            return candidate.stable_id == visual.stable_id &&
                candidate.kind == visual.kind;
        });
    return found == previous.end() ? nullptr : &*found;
}

bool MakeRingGaps(const PersistentVfxVisual &visual, float age, VfxPayload &payload)
{
    if (!Finite(visual.position) || !std::isfinite(visual.ring_inner_radius) ||
        !std::isfinite(visual.ring_outer_radius) || visual.ring_inner_radius < 0.0f ||
        visual.ring_outer_radius <= visual.ring_inner_radius ||
        !std::isfinite(visual.gap_offset_degrees) ||
        !std::isfinite(visual.gap_half_angle_degrees) ||
        (visual.gap_count > 0 && (visual.gap_half_angle_degrees <= 0.0f ||
                                   visual.gap_half_angle_degrees > 180.0f)))
        return false;
    VfxRingGapsPayload ring;
    ring.center = visual.position;
    ring.inner_radius = visual.ring_inner_radius;
    ring.outer_radius = visual.ring_outer_radius;
    ring.gap_half_width_degrees = visual.gap_half_angle_degrees;
    ring.lifetime01 = age;
    ring.gap_angles_degrees.reserve(visual.gap_count);
    for (std::uint32_t index = 0; index < visual.gap_count; ++index)
    {
        const auto degrees = visual.gap_offset_degrees +
            360.0f * static_cast<float>(index) / static_cast<float>(visual.gap_count);
        ring.gap_angles_degrees.push_back(std::fmod(std::fmod(degrees, 360.0f) + 360.0f, 360.0f));
    }
    payload = std::move(ring);
    return true;
}

bool MakeCircle(const PersistentVfxVisual &visual, float age,
                VfxPayload &payload) noexcept
{
    if (!Finite(visual.position) || !std::isfinite(visual.yaw) ||
        !std::isfinite(visual.radius) || visual.radius <= 0.0f)
        return false;
    VfxCirclePayload circle;
    circle.center = visual.position;
    circle.direction = {std::sin(visual.yaw), 0.0f, std::cos(visual.yaw)};
    circle.radius = visual.radius;
    circle.lifetime01 = age;
    payload = std::move(circle);
    return true;
}

bool MakeLine(const PersistentVfxVisual &visual, float age,
              VfxPayload &payload) noexcept
{
    if (!Finite(visual.position) || !std::isfinite(visual.yaw) ||
        !std::isfinite(visual.radius) || !std::isfinite(visual.length) ||
        visual.radius <= 0.0f || visual.length <= 0.0f)
        return false;
    const Float3 direction{std::sin(visual.yaw), 0.0f, std::cos(visual.yaw)};
    VfxLinePayload line;
    line.start = {visual.position.x - direction.x * visual.length * 0.5f,
                  visual.position.y - direction.y * visual.length * 0.5f,
                  visual.position.z - direction.z * visual.length * 0.5f};
    line.end = {visual.position.x + direction.x * visual.length * 0.5f,
                visual.position.y + direction.y * visual.length * 0.5f,
                visual.position.z + direction.z * visual.length * 0.5f};
    line.direction = direction;
    line.width = visual.radius * 2.0f;
    line.lifetime01 = age;
    payload = std::move(line);
    return true;
}

bool MakeProjectilePath(const PersistentVfxVisual &visual,
                        const PersistentVfxVisual *, float,
                        VfxPayload &payload) noexcept
{
    if (visual.projectile_owner_id == 0 ||
        !Finite(visual.projectile_current_position) ||
        !Finite(visual.projectile_previous_position) ||
        !Finite(visual.projectile_velocity) ||
        !std::isfinite(visual.projectile_hitbox_radius) ||
        visual.projectile_hitbox_radius <= 0.0f)
        return false;
    VfxProjectilePathPayload path;
    path.current_position = visual.projectile_current_position;
    path.previous_position = visual.projectile_previous_position;
    path.velocity = visual.projectile_velocity;
    if (visual.kind == PersistentVfxKind::BasicArrowReturn)
    {
        if (!Finite(visual.return_start_position)) return false;
        path.path_origin_position = visual.return_start_position;
    }
    path.projectile_radius = visual.projectile_hitbox_radius;
    path.lifetime01 = 0.0f;
    path.state_flags = visual.projectile_state_flags;
    payload = std::move(path);
    return true;
}

bool MakeProjectileHead(const PersistentVfxVisual &visual,
                        VfxPayload &payload) noexcept
{
    if (!Finite(visual.projectile_current_position) ||
        !Finite(visual.projectile_previous_position) ||
        !Finite(visual.projectile_velocity) ||
        !std::isfinite(visual.projectile_hitbox_radius) ||
        visual.projectile_hitbox_radius <= 0.0f)
        return false;
    VfxProjectilePayload projectile;
    projectile.velocity = visual.projectile_velocity;
    projectile.hitbox_half_extents = {
        visual.projectile_hitbox_radius, visual.projectile_hitbox_radius,
        visual.projectile_hitbox_radius};
    projectile.hitbox_radius = visual.projectile_hitbox_radius;
    projectile.lifetime01 = 0.0f;
    projectile.state_flags = visual.projectile_state_flags;
    payload = std::move(projectile);
    return true;
}

std::optional<float> DeliveryHeadRadius(const VfxProgramView &cooked,
                                       const VfxEffectRecord &effect) noexcept
{
    if (effect.sources.first > cooked.sources.size() || effect.sources.count >
        cooked.sources.size() - effect.sources.first) return {};
    std::optional<float> radius;
    for (std::size_t source_index = effect.sources.first;
         source_index < effect.sources.first + effect.sources.count; ++source_index)
    {
        const auto &source = cooked.sources[source_index];
        if (source.effect != effect.handle || source.type != VfxSourceType::HistoryRibbon ||
            source.stable_id != Hash32("directional_trail"))
            continue;
        if (source.outputs.first > cooked.outputs.size() || source.outputs.count >
            cooked.outputs.size() - source.outputs.first) return {};
        for (const auto &output : std::span(cooked.outputs).subspan(
                 source.outputs.first, source.outputs.count))
        {
            if (output.source != source_index || output.profile != VfxOutputProfile::RibbonAdd ||
                output.shape != Hash32("camera_facing_ribbon") ||
                output.motion != VfxMotionKind::HistoryRibbonTaperedFlow) continue;
            if (output.parameters.first > cooked.parameters.size() || output.parameters.count >
                cooked.parameters.size() - output.parameters.first) return {};
            for (const auto &parameter : std::span(cooked.parameters).subspan(
                     output.parameters.first, output.parameters.count))
            {
                if (parameter.key != Hash32("width_head")) continue;
                if (radius || parameter.type != VfxParameterType::Float) return {};
                const auto value = std::bit_cast<float>(parameter.bits);
                if (!std::isfinite(value) || value <= 0) return {};
                radius = value;
            }
        }
    }
    return radius;
}

bool MakeSafeSector(const VfxRingGapsPayload &ring, std::uint32_t index, VfxPayload &payload)
{
    if (index >= ring.gap_angles_degrees.size()) return false;
    const auto radians = ring.gap_angles_degrees[index] * 0.017453292519943295f;
    payload = VfxSafeSectorPayload{ring.center, {std::cos(radians), 0.0f, std::sin(radians)},
        ring.inner_radius, ring.outer_radius, ring.gap_half_width_degrees, ring.lifetime01};
    return true;
}

std::optional<PersistentBinding> WarningBinding(std::uint64_t effect_id) noexcept
{
    if (effect_id == Hash64("particle.enemy.spawn_warning"))
        return PersistentBinding{PersistentVfxKind::EnemySpawnWarning, effect_id, kCirclePayload};
    if (effect_id == Hash64("particle.boss.spawn_warning"))
        return PersistentBinding{PersistentVfxKind::BossSpawnWarning, effect_id, kCirclePayload};
    if (effect_id == Hash64("particle.boss.area.telegraph"))
        return PersistentBinding{PersistentVfxKind::BossAreaWarning, effect_id, kCirclePayload};
    if (effect_id == Hash64("particle.enemy.suicide.telegraph_radius") ||
        effect_id == Hash64("particle.enemy.suicide.charge"))
        return PersistentBinding{PersistentVfxKind::SuicideEnemyWarning, effect_id, kCirclePayload};
    if (effect_id == Hash64("particle.upgrade.explosive.mini_bomb.telegraph"))
        return PersistentBinding{PersistentVfxKind::MiniBombWarning, effect_id, kCirclePayload};
    if (effect_id == kSafeMarkerEffect)
        return PersistentBinding{PersistentVfxKind::BossShockwaveWarning, effect_id, kSafeSectorPayload};
    if (effect_id == Hash64("particle.boss.shockwave.telegraph"))
        return PersistentBinding{PersistentVfxKind::BossShockwaveWarning, effect_id, kRingGapsPayload};
    if (effect_id == kBossDashWarningEffect)
        return PersistentBinding{PersistentVfxKind::BossDashWarning, effect_id, kLinePayload};
    if (effect_id == kBossVolleyWarningEffect)
        return PersistentBinding{PersistentVfxKind::BossVolleyWarning, effect_id, kConePayload};
    if (effect_id == kRangedWarningEffect)
        return PersistentBinding{PersistentVfxKind::RangedEnemyWarning, effect_id, kLinePayload};
    return std::nullopt;
}

bool WarningShapeMatches(const PersistentBinding &binding, const VfxPayload &payload) noexcept
{
    if (binding.payload_kind == kCirclePayload)
        return std::holds_alternative<VfxCirclePayload>(payload);
    if (binding.payload_kind == kSafeSectorPayload)
        return std::holds_alternative<VfxSafeSectorPayload>(payload);
    if (binding.payload_kind == kRingGapsPayload)
        return std::holds_alternative<VfxRingGapsPayload>(payload);
    return binding.payload_kind == kLinePayload ? std::holds_alternative<VfxLinePayload>(payload)
                                               : std::holds_alternative<VfxConePayload>(payload);
}

bool IsStatusTickEffect(std::uint64_t effect_id) noexcept
{
    return effect_id == kBleedTickEffect || effect_id == kBurnTickEffect;
}

bool IsPickupCollectEffect(std::uint64_t effect_id) noexcept
{
    return effect_id == kPickupXpCollectEffect ||
        effect_id == kPickupHealCollectEffect ||
        effect_id == kPickupMagnetCollectEffect ||
        effect_id == kPickupRelicCollectEffect;
}

bool FiniteWarningOwner(const PersistentVfxVisual &owner, Tick current_tick) noexcept
{
    return owner.active_tick <= current_tick && current_tick < owner.expires &&
        owner.expires > owner.active_tick && Finite(owner.position) &&
        std::isfinite(owner.yaw) && std::isfinite(owner.radius) && owner.radius > 0.0f;
}

} // namespace

std::size_t PreparedPairFacts::VisualHash::operator()(const VisualKey &key) const noexcept
{
    return std::hash<std::uint64_t>{}(key.stable_id ^
        (static_cast<std::uint64_t>(key.kind) << 56));
}

std::size_t PreparedPairFacts::CaptureHash::operator()(const CaptureKey &key) const noexcept
{
    auto hash = std::hash<std::uint64_t>{}(key.session_id);
    const auto mix = [&](std::uint64_t value) {
        hash ^= std::hash<std::uint64_t>{}(value) + 0x9e3779b97f4a7c15ull +
            (hash << 6) + (hash >> 2);
    };
    mix(key.sequence); mix(key.tick); mix(key.entity_id);
    return hash;
}

void PreparedPairFacts::Prepare(Key key,
    std::span<const PersistentVfxVisual> previous,
    std::span<const PersistentVfxVisual> current,
    std::span<const RenderInstance> instances,
    std::span<const CapturedRenderInstance> captures)
{
    key.previous_data = previous.data(); key.previous_count = previous.size();
    key.current_data = current.data(); key.current_count = current.size();
    key.instance_data = instances.data(); key.instance_count = instances.size();
    key.capture_data = captures.data(); key.capture_count = captures.size();
    if (prepared_ && key_ == key) return;
    key_ = key;
    prepared_ = true;
    previous_.clear(); current_.clear(); instances_.clear(); captures_.clear();
    previous_.reserve(previous.size()); current_.reserve(current.size());
    instances_.reserve(instances.size()); captures_.reserve(captures.size());
    for (std::size_t i = 0; i < previous.size(); ++i)
        previous_.try_emplace({previous[i].stable_id, previous[i].kind}, i);
    for (std::size_t i = 0; i < current.size(); ++i)
        current_.try_emplace({current[i].stable_id, current[i].kind}, i);
    for (std::size_t i = 0; i < instances.size(); ++i)
        instances_.try_emplace(instances[i].stable_id, i);
    for (std::size_t i = 0; i < captures.size(); ++i)
        captures_.try_emplace({captures[i].session_id, captures[i].sequence,
                               captures[i].tick, captures[i].source_entity_id}, i);
}

const PersistentVfxVisual *PreparedPairFacts::Previous(
    const PersistentVfxVisual &visual,
    std::span<const PersistentVfxVisual> rows) const noexcept
{
    const auto found = previous_.find({visual.stable_id, visual.kind});
    return found != previous_.end() && found->second < rows.size() ? &rows[found->second] : nullptr;
}

const PersistentVfxVisual *PreparedPairFacts::Current(
    std::uint64_t stable_id, PersistentVfxKind kind,
    std::span<const PersistentVfxVisual> rows) const noexcept
{
    const auto found = current_.find({stable_id, kind});
    return found != current_.end() && found->second < rows.size() ? &rows[found->second] : nullptr;
}

const RenderInstance *PreparedPairFacts::Instance(
    std::uint64_t stable_id, std::span<const RenderInstance> rows) const noexcept
{
    const auto found = instances_.find(stable_id);
    return found != instances_.end() && found->second < rows.size() ? &rows[found->second] : nullptr;
}

const CapturedRenderInstance *PreparedPairFacts::Capture(
    std::uint64_t session_id, Sequence sequence, Tick tick, std::uint64_t entity_id,
    std::span<const CapturedRenderInstance> rows) const noexcept
{
    const auto found = captures_.find({session_id, sequence, tick, entity_id});
    return found != captures_.end() && found->second < rows.size() ? &rows[found->second] : nullptr;
}

VfxTypedFrameInputs BuildVfxTypedFrameInputs(
    const VfxProgramView &cooked,
    std::span<const PresentationEvent> events,
    std::span<const PersistentVfxVisual> current,
    std::span<const PersistentVfxVisual> previous,
    Tick current_tick,
    const PreparedPairFacts *pair_facts)
{
    VfxTypedFrameInputs result;
    result.events.reserve(events.size());
    result.persistent.reserve(current.size());

    for (const auto &event : events)
    {
        if (event.kind != PresentationKind::Vfx) continue;
        const auto effect_id = event.asset.value;
        const auto *lookup = FindLookup(cooked, effect_id);
        const auto *effect = lookup ? FindEffect(cooked, lookup->handle) : nullptr;
        if (effect && effect->input_mode == 1 &&
            (effect_id == Hash64("particle.upgrade.retreat.second_dash") ||
             effect_id == Hash64("particle.upgrade.retreat.afterimage")))
            continue; // Captured success geometry is owned by VfxUpgradeMovementState.
        if (effect && effect->input_mode == 1 &&
            (effect->payload_kind == kProjectilePayload ||
             (effect->payload_kind == kLinkPayload &&
              (effect_id == Hash64("particle.line.ricochet") || effect_id == Hash64("particle.line.burn_transfer") ||
               effect_id == Hash64("particle.line.relic_chain")))))
            continue; // Authoritative snapshot presence owns these visuals.
        if (!lookup || !effect || effect->input_mode != 0)
        {
            Diagnose(result, effect_id);
            continue;
        }
        const bool pickup_collect = IsPickupCollectEffect(effect_id);
        if ((effect_id == kArrowRainPullEffect &&
             (effect->payload_kind != kLinkPayload ||
               event.geometry.kind != PresentationGeometryKind::Line)) ||
            (effect->payload_kind == kLinkPayload &&
             effect_id != kArrowRainPullEffect && !pickup_collect) ||
            (pickup_collect &&
             (effect->payload_kind != kLinkPayload ||
              event.geometry.kind != PresentationGeometryKind::None ||
              event.sequence == 0)))
        {
            Diagnose(result, effect_id);
            continue;
        }
        const auto parameters = DecodeVfxParameters(event.parameters);
        if (!Finite(event.position) || !Finite(parameters))
        {
            Diagnose(result, effect_id);
            continue;
        }
        VfxEventInput input;
        input.effect_handle = effect->handle;
        input.sequence = event.sequence;
        input.status_episode_generation = event.status_episode_generation;
        input.event_tick = event.tick;
        input.world_transform = Transform(event.position, 0.0f);
        input.stable_seed = EventSeed(event.sequence);
        if (pickup_collect)
        {
            if ((parameters.flags & static_cast<std::uint32_t>(VfxEventFlag::HasTarget)) == 0)
            {
                Diagnose(result, effect_id);
                continue;
            }
            VfxLinkPayload link{};
            link.source_position = event.position;
            link.target_position = parameters.target;
            input.payload = link;
        }
        else if (effect->payload_kind == kPointPayload)
        {
            if (parameters.scale <= 0.0f)
            {
                Diagnose(result, effect_id);
                continue;
            }
            VfxPointPayload point;
            point.position = event.position;
            point.direction = parameters.direction;
            point.authored_scale = parameters.scale;
            input.payload = std::move(point);
        }
        else if (effect->payload_kind == kEntityPayload &&
                 effect_id == Hash64("particle.skill.charged_shot.cancel"))
        {
            if (event.sequence == 0 ||
                event.geometry.kind != PresentationGeometryKind::EntityAttachment ||
                event.geometry.source_id == 0 ||
                !std::isfinite(event.geometry.radius) || event.geometry.radius <= 0 ||
                event.geometry.end_tick != event.tick ||
                event.geometry.start_tick > event.geometry.end_tick)
            {
                Diagnose(result, effect_id);
                continue;
            }
            input.world_transform = Transform(event.position,
                std::atan2(parameters.direction.x, parameters.direction.z));
            input.payload = VfxEntityPayload{event.geometry.radius, 0, 1, 0,
                                            event.geometry.source_id};
        }
        else if (effect->payload_kind == kContextPayload)
        {
            VfxContextPayload context;
            context.owner_id = event.geometry.source_id;
            context.direction = parameters.direction;
            context.ratio01 = std::clamp(parameters.scale, 0.0f, 1.0f);
            if (effect_id == Hash64("particle.boss.phase_change") ||
                effect_id == Hash64("particle.boss.death") ||
                effect_id == Hash64("particle.session.wave_start"))
            {
                // LocalVolume component bounds use the original authored
                // presentation scale, independently of ratio-based outputs.
                if (parameters.scale <= 0.0f)
                {
                    Diagnose(result, effect_id);
                    continue;
                }
                context.scalar0 = parameters.scale;
            }
            if (effect_id == Hash64("particle.skill.charged_shot.pulse") &&
                event.upgrade_stage != 0)
            {
                if (!std::isfinite(event.vfx_ratio01) || event.vfx_ratio01 < 0.0f ||
                    event.vfx_ratio01 > 1.0f || event.upgrade_owner_id == 0)
                {
                    Diagnose(result, effect_id);
                    continue;
                }
                context.owner_id = event.upgrade_owner_id;
                context.ratio01 = event.vfx_ratio01;
            }
            input.payload = std::move(context);
        }
        else if (event.geometry.kind != PresentationGeometryKind::None)
        {
            if (!MakeEventGeometry(event, parameters, effect->payload_kind,
                                   input.payload))
            {
                Diagnose(result, effect_id);
                continue;
            }
        }
        else
        {
            // EventStream does not carry a Circle/Cone/RingGaps or path
            // payload. In particular, scale is never reinterpreted as radius.
            Diagnose(result, effect_id);
            continue;
        }
        if (IsStatusTickEffect(effect_id))
            input.geometry_owner_id = event.geometry.source_id;
        if (const auto warning = WarningBinding(effect_id))
        {
            if (event.geometry.source_id == 0 ||
                event.geometry.end_tick <= event.geometry.start_tick ||
                !WarningShapeMatches(*warning, input.payload))
            {
                Diagnose(result, effect_id);
                continue;
            }
            input.geometry_owner_id = event.geometry.source_id;
            input.geometry_effect_id = effect_id;
            input.geometry_start_tick = event.geometry.start_tick;
            input.geometry_end_tick = event.geometry.end_tick;
        }
        if (effect_id == Hash64("particle.boss.shockwave.telegraph") && input.geometry_owner_id != 0)
        {
            const auto &ring = std::get<VfxRingGapsPayload>(input.payload);
            if (!ring.gap_angles_degrees.empty())
            {
                const auto *marker_lookup = FindLookup(cooked, kSafeMarkerEffect);
                const auto *marker = marker_lookup ? FindEffect(cooked, marker_lookup->handle) : nullptr;
                if (!marker || marker->input_mode != 0 || marker->payload_kind != kSafeSectorPayload ||
                    marker->timing_kind != 1)
                    Diagnose(result, kSafeMarkerEffect);
                else
                {
                    for (std::uint32_t index = 0; index < ring.gap_angles_degrees.size(); ++index)
                    {
                        auto sector = input;
                        sector.effect_handle = marker->handle;
                        sector.geometry_effect_id = kSafeMarkerEffect;
                        sector.geometry_sector_index = index;
                        sector.stable_seed ^= 0x9e3779b9u * (index + 1);
                        if (MakeSafeSector(ring, index, sector.payload))
                            result.events.push_back(std::move(sector));
                    }
                }
            }
        }
        result.events.push_back(std::move(input));
    }

    for (const auto &visual : current)
    {
        if (visual.kind == PersistentVfxKind::WaveTimer)
        {
            constexpr auto id = Hash64("particle.session.wave_warning");
            const auto *lookup = FindLookup(cooked, id);
            const auto *effect = lookup ? FindEffect(cooked, lookup->handle) : nullptr;
            if (!effect || effect->input_mode != 0 || effect->timing_kind != 0 ||
                effect->payload_kind != kContextPayload ||
                !std::isfinite(effect->seconds) || effect->seconds <= 0 || effect->seconds > 60 ||
                visual.effect_asset.value != id || visual.stable_id == 0 ||
                !Finite(visual.position) || !std::isfinite(visual.radius) || visual.radius <= 0 ||
                visual.active_tick > current_tick || visual.source_horizon_tick <= current_tick)
            { Diagnose(result, id); continue; }
            const auto window = static_cast<Tick>(std::ceil(effect->seconds * 60.0f));
            if (visual.source_horizon_tick - current_tick > window) continue;
            const auto start = visual.source_horizon_tick > window
                ? visual.source_horizon_tick - window : Tick{};
            VfxPersistentInput input{};
            input.stable_id = visual.stable_id;
            input.effect_handle = effect->handle;
            input.source_visual_kind = static_cast<std::uint8_t>(visual.kind);
            input.current_transform = input.previous_transform = Transform(visual.position, 0);
            input.elapsed_seconds = static_cast<float>(current_tick - start) / 60.0f;
            input.normalized_age = input.elapsed_seconds / effect->seconds;
            if (input.normalized_age >= 1) continue;
            input.source_duration_seconds = effect->seconds;
            input.stable_seed = PersistentSeed(visual.stable_id, effect->handle);
            input.payload = VfxContextPayload{visual.stable_id, {}, input.normalized_age,
                                              visual.radius, 0};
            result.persistent.push_back(std::move(input));
            continue;
        }
        if (visual.kind == PersistentVfxKind::ScheduledAreaDelivery)
        {
            const auto *lookup = FindLookup(cooked, kArrowRainIncomingEffect);
            const auto *effect = lookup ? FindEffect(cooked, lookup->handle) : nullptr;
            const auto head_radius = effect ? DeliveryHeadRadius(cooked, *effect) :
                std::optional<float>{};
            if (!effect || effect->input_mode != 1 || effect->timing_kind != 0 ||
                effect->payload_kind != kProjectilePayload ||
                !std::isfinite(effect->seconds) || effect->seconds <= 0 || effect->seconds > 60 ||
                !head_radius || !UpgradeBindingContains(cooked, Hash64("skill.arrow_rain"), 1,
                                                       effect->handle) ||
                visual.effect_asset.value != kArrowRainIncomingEffect ||
                visual.skill != kArrowRainSkill || visual.source_upgrade != 0 ||
                visual.stable_id == 0 || visual.projectile_owner_id == 0 || visual.cast_id == 0 ||
                visual.active_tick > current_tick || visual.expires <= visual.active_tick ||
                !Finite(visual.position) || !std::isfinite(visual.radius) || visual.radius <= 0)
            {
                Diagnose(result, kArrowRainIncomingEffect);
                continue;
            }
            // Gameplay supplies only its admitted schedule and landing circle.
            // The cooked visual recipe owns the final approach window; absence
            // of the real pending action ends every owner-follow output.
            const auto window_ticks = static_cast<Tick>(
                std::ceil(static_cast<double>(effect->seconds) * 60.0));
            const auto started = std::max(visual.active_tick,
                visual.expires > window_ticks ? visual.expires - window_ticks : Tick{});
            if (current_tick < started || current_tick >= visual.expires) continue;
            const auto duration_ticks = visual.expires - started;
            const auto height = visual.radius * 2.0f;
            const auto position_at = [&](Tick sample_tick) {
                const auto remaining = static_cast<float>(visual.expires - sample_tick) /
                    static_cast<float>(duration_ticks);
                return Float3{visual.position.x, visual.position.y + height * remaining,
                              visual.position.z};
            };
            const auto position = position_at(current_tick);
            const auto previous_position = position_at(current_tick > started ? current_tick - 1 : started);
            const Float3 velocity{0, -height * 60.0f / static_cast<float>(duration_ticks), 0};
            VfxPersistentInput input;
            input.stable_id = visual.stable_id;
            input.effect_handle = effect->handle;
            input.current_transform = Transform(position, 0);
            input.previous_transform = Transform(previous_position, 0);
            input.elapsed_seconds = static_cast<float>(current_tick - started) / 60.0f;
            input.normalized_age = std::clamp(input.elapsed_seconds / effect->seconds, 0.0f, 1.0f);
            input.stable_seed = PersistentSeed(visual.projectile_owner_id, effect->handle);
            input.source_visual_kind = static_cast<std::uint8_t>(visual.kind);
            // This is an authored delivery head size, not the damage radius.
            // The full landing Circle remains on the schedule/secondary area.
            input.payload = VfxProjectilePayload{velocity,
                {*head_radius, *head_radius, *head_radius}, *head_radius,
                input.normalized_age, 0};
            result.persistent.push_back(std::move(input));
            continue;
        }
        if (visual.kind == PersistentVfxKind::BossDashWarning ||
            visual.kind == PersistentVfxKind::BossVolleyWarning ||
            visual.kind == PersistentVfxKind::RangedEnemyWarning ||
            visual.kind == PersistentVfxKind::MiniBombWarning ||
            visual.kind == PersistentVfxKind::EnemySpawnWarning ||
            visual.kind == PersistentVfxKind::BossSpawnWarning ||
            visual.kind == PersistentVfxKind::BossAreaWarning ||
            visual.kind == PersistentVfxKind::SuicideEnemyWarning ||
            visual.kind == PersistentVfxKind::BossShockwaveWarning)
            continue; // Warning visuals are owner updates for EventStream effects.
        if (visual.kind == PersistentVfxKind::ProjectileHead)
        {
            const auto *lookup = FindLookup(cooked, visual.effect_asset.value);
            const auto *effect = lookup ? FindEffect(cooked, lookup->handle) : nullptr;
            if (!lookup || !effect || effect->input_mode != 1 ||
                effect->payload_kind != kProjectilePayload ||
                visual.projectile_owner_id == 0 ||
                visual.projectile_spawned_tick > current_tick)
            {
                Diagnose(result, visual.effect_asset.value);
                continue;
            }
            if (visual.skill == 3 && visual.source_upgrade == 2 &&
                (!UpgradeBindingContains(cooked, Hash64("skill.charged_shot"), 3,
                                         effect->handle) || visual.cast_id == 0))
            {
                Diagnose(result, visual.effect_asset.value);
                continue;
            }
            VfxPersistentInput input;
            input.stable_id = visual.stable_id;
            input.effect_handle = effect->handle;
            input.current_transform = Transform(visual.projectile_current_position,
                                                visual.yaw);
            input.previous_transform = Transform(
                visual.projectile_previous_position,
                std::atan2(visual.projectile_velocity.x,
                           visual.projectile_velocity.z));
            input.elapsed_seconds = static_cast<float>(current_tick -
                                                       visual.projectile_spawned_tick) /
                                    60.0f;
            input.normalized_age = 0.0f;
            input.state_flags = visual.projectile_state_flags;
            input.stable_seed = PersistentSeed(visual.projectile_owner_id,
                                               effect->handle);
            if (!MakeProjectileHead(visual, input.payload))
            {
                Diagnose(result, visual.effect_asset.value);
                continue;
            }
            result.persistent.push_back(std::move(input));
            continue;
        }
        const auto base_binding = std::find_if(kPersistentBindings.begin(), kPersistentBindings.end(),
            [&visual](const PersistentBinding &candidate) { return candidate.kind == visual.kind; });
        if (base_binding == kPersistentBindings.end())
        {
            Diagnose(result, 0);
            continue;
        }
        PersistentBinding binding = *base_binding;
        const auto *alias = FindUpgradeSlowAreaAlias(visual);
        if (alias)
            binding.effect_id = alias->effect_id;
        const auto *lookup = FindLookup(cooked, binding.effect_id);
        const auto *effect = lookup ? FindEffect(cooked, lookup->handle) : nullptr;
        if (alias && (!lookup || !effect ||
                      !UpgradeBindingContains(cooked, alias->skill_id,
                                              alias->ordinal, effect->handle)))
        {
            // An authored alias is selected by authoritative area metadata. A
            // malformed or missing cooked membership must not fall back to the
            // legacy effect, which would render the same area with the wrong
            // authored recipe.
            Diagnose(result, binding.effect_id);
            continue;
        }
        const bool bow_draw_stamp =
            visual.kind == PersistentVfxKind::PlayerBowDraw;
        const bool fixed_status_stamp =
            visual.kind == PersistentVfxKind::ChargedFullReady ||
            visual.kind == PersistentVfxKind::MultishotRetarget ||
            visual.kind == PersistentVfxKind::BasicArrowReturn ||
            visual.kind == PersistentVfxKind::RicochetBleedExtend;
        const bool empowered_ready_stamp =
            visual.kind == PersistentVfxKind::EmpoweredReady;
        const auto age = fixed_status_stamp
            ? std::optional<float>{0.0f}
            : Lifetime(visual, current_tick);
        const bool pickup_motion = visual.kind == PersistentVfxKind::PickupXpMagnetTrail;
        if (!lookup || !effect || effect->input_mode != (pickup_motion ? 0u : 1u) ||
            effect->payload_kind != binding.payload_kind || !age)
        {
            Diagnose(result, binding.effect_id);
            continue;
        }
        if (visual.kind == PersistentVfxKind::UpgradeSlowArea &&
            visual.skill == kArrowRainSkill && visual.source_upgrade != 7 &&
            !UpgradeBindingContains(cooked, Hash64("skill.arrow_rain"), 6, effect->handle))
        {
            Diagnose(result, binding.effect_id);
            continue;
        }
        if ((visual.kind == PersistentVfxKind::BurnTransferLink ||
             visual.kind == PersistentVfxKind::RicochetLink) && visual.cast_id != 0)
        {
            const auto skill_id = visual.skill == kPiercingShotSkill
                ? Hash64("skill.piercing_shot")
                : visual.skill == kMultishotSkill ? Hash64("skill.multishot")
                : visual.skill == kRicochetSkill ? Hash64("skill.ricochet_arrow") : 0;
            if (!skill_id || visual.source_upgrade >= 8 ||
                !UpgradeBindingContains(cooked, skill_id, visual.source_upgrade + 1u,
                                        effect->handle))
            {
                Diagnose(result, binding.effect_id);
                continue;
            }
        }
        if ((visual.kind == PersistentVfxKind::RicochetReturnLink &&
             (visual.skill != kRicochetSkill || visual.source_upgrade != 0 ||
              visual.cast_id == 0 ||
              !UpgradeBindingContains(cooked, Hash64("skill.ricochet_arrow"), 1,
                                      effect->handle))) ||
            (visual.kind == PersistentVfxKind::ChargedFullReady &&
             visual.source_upgrade == 0 &&
             !UpgradeBindingContains(cooked, Hash64("skill.charged_shot"), 1,
                                     effect->handle)) ||
            (visual.kind == PersistentVfxKind::BasicArrowReturn &&
             (visual.skill != 0 || visual.source_upgrade != 6 ||
              !UpgradeBindingContains(cooked, Hash64("skill.basic_attack"), 7,
                                      effect->handle))) ||
            (visual.kind == PersistentVfxKind::ChargedFullReady &&
             visual.source_upgrade == 4 &&
             !UpgradeBindingContains(cooked, Hash64("skill.charged_shot"), 5,
                                     effect->handle)) ||
            (visual.kind == PersistentVfxKind::MultishotRetarget &&
             (visual.skill != kMultishotSkill || visual.source_upgrade != 7 ||
              !UpgradeBindingContains(cooked, Hash64("skill.multishot"), 8, effect->handle))) ||
            (visual.kind == PersistentVfxKind::RicochetBleedExtend &&
             (visual.skill != kRicochetSkill || visual.source_upgrade != 2 ||
              !UpgradeBindingContains(cooked, Hash64("skill.ricochet_arrow"), 3, effect->handle))) ||
            (visual.kind == PersistentVfxKind::ChargedOverchargeLoop &&
             (visual.skill != 3 || visual.source_upgrade != 0 ||
              effect->binding_timing != Hash32("charge_ratio") ||
              !UpgradeBindingContains(cooked, Hash64("skill.charged_shot"), 1,
                                      effect->handle))))
        {
            Diagnose(result, binding.effect_id);
            continue;
        }
        if ((visual.kind == PersistentVfxKind::MultishotRetarget ||
             visual.kind == PersistentVfxKind::BasicArrowReturn ||
             visual.kind == PersistentVfxKind::RicochetBleedExtend) &&
            (visual.projectile_owner_id == 0 ||
             visual.expires <= visual.active_tick || current_tick >= visual.expires ||
             visual.projectile_spawned_tick > visual.active_tick))
        {
            Diagnose(result, binding.effect_id);
            continue;
        }

        float source_duration_seconds = 0.0f;
        if (visual.kind == PersistentVfxKind::FireArea)
        {
            if (visual.expires <= visual.active_tick) { Diagnose(result, binding.effect_id); continue; }
            source_duration_seconds = static_cast<float>(visual.expires - visual.active_tick) / 60.0f;
        }
        else if (bow_draw_stamp)
        {
            if (visual.source_horizon_tick <= visual.active_tick)
            {
                Diagnose(result, binding.effect_id);
                continue;
            }
            source_duration_seconds = static_cast<float>(
                visual.source_horizon_tick - visual.active_tick) / 60.0f;
            if (!std::isfinite(source_duration_seconds) ||
                source_duration_seconds <= 0.0f)
            {
                Diagnose(result, binding.effect_id);
                continue;
            }
        }
        else if (empowered_ready_stamp)
        {
            if (visual.expires <= visual.active_tick)
            {
                Diagnose(result, binding.effect_id);
                continue;
            }
            source_duration_seconds = static_cast<float>(
                visual.expires - visual.active_tick) / 60.0f;
            if (!std::isfinite(source_duration_seconds) ||
                source_duration_seconds <= 0.0f)
            {
                Diagnose(result, binding.effect_id);
                continue;
            }
        }
        else if (visual.kind == PersistentVfxKind::ChargedOverchargeLoop)
        {
            if (visual.expires <= visual.active_tick)
            {
                Diagnose(result, binding.effect_id);
                continue;
            }
            source_duration_seconds = static_cast<float>(
                visual.expires - visual.active_tick) / 60.0f;
            if (!std::isfinite(source_duration_seconds) ||
                source_duration_seconds <= 0.0f)
            {
                Diagnose(result, binding.effect_id);
                continue;
            }
        }

        VfxPersistentInput input;
        input.stable_id = visual.stable_id;
        input.status_episode_generation = visual.status_episode_generation;
        if (visual.kind == PersistentVfxKind::BossPhase2Aura ||
            visual.kind == PersistentVfxKind::BossPhaseTransition)
            input.status_episode_generation = visual.active_tick;
        input.effect_handle = effect->handle;
        input.source_visual_kind = static_cast<std::uint8_t>(visual.kind);
        const auto current_position = visual.projectile_owner_id != 0
            ? visual.projectile_current_position : visual.position;
        input.current_transform = Transform(current_position, visual.yaw);
        const auto *prior = FindPrevious(previous, visual, pair_facts);
        input.previous_transform = visual.projectile_owner_id != 0
            ? Transform(visual.projectile_previous_position,
                        std::atan2(visual.projectile_velocity.x,
                                   visual.projectile_velocity.z))
            : (prior ? Transform(prior->position, prior->yaw) : input.current_transform);
        input.elapsed_seconds = visual.projectile_owner_id != 0 && !fixed_status_stamp
            ? static_cast<float>(current_tick - visual.projectile_spawned_tick) / 60.0f
            : static_cast<float>(current_tick > visual.active_tick
                                    ? current_tick - visual.active_tick : 0) / 60.0f;
        input.normalized_age = *age;
        if (fixed_status_stamp)
        {
            if (current_tick < visual.active_tick || effect->timing_kind != 0 ||
                !std::isfinite(effect->seconds) || effect->seconds <= 0.0f ||
                input.elapsed_seconds >= effect->seconds)
            {
                Diagnose(result, binding.effect_id);
                continue;
            }
            input.normalized_age = std::clamp(
                input.elapsed_seconds / effect->seconds, 0.0f, 1.0f);
            source_duration_seconds = effect->seconds;
        }
        else if (bow_draw_stamp && visual.expires == 0)
        {
            input.normalized_age = std::clamp(
                input.elapsed_seconds / source_duration_seconds,
                0.0f, 1.0f - 1.0e-4f);
        }
        input.source_duration_seconds = source_duration_seconds;
        if (visual.kind == PersistentVfxKind::RicochetReturnLink)
        {
            const auto start_distance = std::hypot(
                visual.return_start_position.x - visual.link_target_position.x,
                visual.return_start_position.z - visual.link_target_position.z);
            const auto remaining_distance = std::hypot(
                visual.position.x - visual.link_target_position.x,
                visual.position.z - visual.link_target_position.z);
            if (!std::isfinite(start_distance) || start_distance <= 0.0001f ||
                !std::isfinite(remaining_distance))
                continue;
            input.normalized_age = std::clamp(
                1.0f - remaining_distance / start_distance, 0.0f, 1.0f - 1.0e-4f);
        }
        input.state_flags = visual.projectile_state_flags;
        input.stable_seed = PersistentSeed(visual.stable_id, effect->handle);
        bool geometry_ok = false;
        if (binding.payload_kind == kLinkPayload)
        {
            const float length = std::hypot(std::hypot(visual.link_target_position.x - visual.position.x,
                visual.link_target_position.z - visual.position.z), visual.link_target_position.y - visual.position.y);
            geometry_ok = effect->timing_kind == 1 && visual.active_tick <= current_tick && current_tick < visual.expires &&
                visual.expires > visual.active_tick && Finite(visual.position) && Finite(visual.link_target_position) &&
                std::isfinite(length) && length > .0001f && std::isfinite(visual.link_width) && visual.link_width > 0;
            if (geometry_ok)
            {
                VfxLinkPayload link;
                link.source_position = visual.position; link.target_position = visual.link_target_position;
                link.width = visual.link_width; link.lifetime01 = *age;
                input.payload = link;
                input.stable_seed = PersistentSeed(visual.stable_id, static_cast<std::uint32_t>(binding.effect_id));
            }
        }
        else if (binding.payload_kind == kEntityPayload)
        {
            geometry_ok=(fixed_status_stamp ? effect->timing_kind == 0 &&
                std::isfinite(effect->seconds) && effect->seconds > 0.0f
                : effect->timing_kind == 1) && visual.effect_asset.value==binding.effect_id&&
                visual.active_tick<=current_tick&&Finite(visual.position)&&std::isfinite(visual.yaw)&&
                std::isfinite(visual.radius)&&visual.radius>0&&std::isfinite(visual.entity_health_fraction)&&
                visual.entity_health_fraction>=0&&visual.entity_health_fraction<=1&&
                visual.entity_render_id != 0 &&
                (visual.kind != PersistentVfxKind::RicochetBleedExtend ||
                 (Finite(visual.projectile_current_position) &&
                  std::isfinite(visual.projectile_hitbox_radius) &&
                  visual.projectile_hitbox_radius > 0.0f)) &&
                (visual.kind != PersistentVfxKind::ChargedOverchargeLoop ||
                 (visual.skill == 3 && visual.source_upgrade == 0 &&
                  visual.expires > visual.active_tick && current_tick < visual.expires &&
                  std::isfinite(visual.charge_ratio) && visual.charge_ratio >= 0.0f &&
                  visual.charge_ratio <= 1.0f));
            if(geometry_ok) {
                input.payload=VfxEntityPayload{visual.radius,input.normalized_age,visual.entity_health_fraction,
                                               visual.entity_state_flags,visual.entity_render_id};
                input.state_flags=visual.entity_state_flags;
                input.stable_seed=PersistentSeed(visual.stable_id,static_cast<std::uint32_t>(binding.effect_id));
            }
        }
        else if (binding.payload_kind == kContextPayload)
        {
            geometry_ok = visual.kind == PersistentVfxKind::PlayerLowHealthVignette &&
                std::isfinite(visual.entity_health_fraction) &&
                visual.entity_health_fraction >= 0.0f && visual.entity_health_fraction <= 0.35f;
            if (geometry_ok)
                input.payload = VfxContextPayload{visual.entity_render_id, {}, visual.entity_health_fraction, 0.0f, 0.0f};
            // Low-health presence is renewed each snapshot. Its pulse follows
            // the advancing simulation phase, not that one-tick presence start.
            input.elapsed_seconds = static_cast<float>(current_tick) / 60.0f;
        }
        else if (binding.payload_kind == kCirclePayload)
            geometry_ok = MakeCircle(visual, *age, input.payload);
        else if (binding.payload_kind == kRingGapsPayload)
            geometry_ok = MakeRingGaps(visual, *age, input.payload);
        else if (binding.payload_kind == kLinePayload)
            geometry_ok = MakeLine(visual, *age, input.payload);
        else if (binding.payload_kind == kProjectilePathPayload)
            geometry_ok = MakeProjectilePath(visual, prior, *age, input.payload);
        if (geometry_ok && (visual.kind == PersistentVfxKind::MultishotRetarget ||
                            visual.kind == PersistentVfxKind::BasicArrowReturn))
        {
            geometry_ok = visual.effect_asset.value == binding.effect_id &&
                (visual.kind == PersistentVfxKind::BasicArrowReturn ||
                 visual.projectile_spawned_tick == visual.active_tick);
            if (geometry_ok)
                std::get<VfxProjectilePathPayload>(input.payload).lifetime01 =
                    input.normalized_age;
        }
        if (!geometry_ok)
        {
            Diagnose(result, binding.effect_id);
            continue;
        }
        result.persistent.push_back(std::move(input));

        const auto append_area_recipe = [&](std::uint64_t effect_id,
                                            std::uint64_t skill_id,
                                            std::uint32_t ordinal) {
            const auto *recipe_lookup = FindLookup(cooked, effect_id);
            const auto *recipe = recipe_lookup
                ? FindEffect(cooked, recipe_lookup->handle) : nullptr;
            if (!recipe || recipe->input_mode != 1 || recipe->timing_kind != 1 ||
                recipe->payload_kind != kCirclePayload ||
                !UpgradeBindingContains(cooked, skill_id, ordinal, recipe->handle) ||
                visual.cast_id == 0)
            {
                Diagnose(result, effect_id);
                return;
            }
            // Distinct recipes reuse the area's actual geometry and clock,
            // while keeping independent stable identity and visual-only seed.
            auto recipe_input = result.persistent.back();
            recipe_input.effect_handle = recipe->handle;
            recipe_input.stable_id = SupplementalStableId(visual.stable_id, effect_id);
            recipe_input.stable_seed = PersistentSeed(recipe_input.stable_id,
                                                      recipe->handle);
            result.persistent.push_back(std::move(recipe_input));
        };
        if (alias)
            append_area_recipe(base_binding->effect_id, alias->skill_id, alias->ordinal);
        if (visual.kind == PersistentVfxKind::ArrowRainArea &&
            visual.skill == kArrowRainSkill && visual.source_upgrade == 0)
            append_area_recipe(Hash64("particle.upgrade.arrow_rain.secondary_area"),
                               Hash64("skill.arrow_rain"), 1);
        if (visual.kind == PersistentVfxKind::FireArea && visual.skill == kArrowRainSkill &&
            visual.source_upgrade == 3)
            append_area_recipe(Hash64("particle.upgrade.arrow_rain.fire_patch"),
                               Hash64("skill.arrow_rain"), 4);
        if (visual.kind == PersistentVfxKind::FireArea && visual.skill == kTrapSkill &&
            visual.source_upgrade == 4)
            append_area_recipe(Hash64("particle.upgrade.trap.fire_variant"),
                               Hash64("skill.trap"), 5);
        if ((visual.kind == PersistentVfxKind::TrapPending ||
             visual.kind == PersistentVfxKind::TrapArmed) &&
            visual.skill == kRetreatShotSkill && visual.source_upgrade == 1)
            append_area_recipe(Hash64("particle.upgrade.retreat.start_trap"),
                               Hash64("skill.retreat_shot"), 2);
        const auto *supplement = FindDamageTrailSupplement(visual);
        if (!supplement) continue;

        const auto *supplement_lookup = FindLookup(cooked, supplement->effect_id);
        const auto *supplement_effect = supplement_lookup
            ? FindEffect(cooked, supplement_lookup->handle) : nullptr;
        const bool exact_binding =
            UpgradeBindingContains(cooked, supplement->skill_id,
                                   supplement->ordinal, effect->handle) &&
            supplement_effect && supplement_effect->input_mode == 1 &&
            supplement_effect->payload_kind == kLinePayload &&
            (supplement->fixed_duration
                 ? supplement_effect->timing_kind == 0 &&
                       std::isfinite(supplement_effect->seconds) &&
                       supplement_effect->seconds > 0.0f
                 : supplement_effect->timing_kind == 1) &&
            UpgradeBindingContains(cooked, supplement->skill_id,
                                   supplement->ordinal,
                                   supplement_effect->handle);
        if (!exact_binding)
        {
            // The generic damage trail remains valid on its own. A qualified
            // supplement is emitted only when both authored line recipes are
            // exact members of the selected upgrade binding.
            Diagnose(result, supplement->effect_id);
            continue;
        }

        auto supplemental_input = result.persistent.back();
        supplemental_input.effect_handle = supplement_effect->handle;
        supplemental_input.stable_id = SupplementalStableId(
            visual.stable_id, supplement->effect_id);
        if (supplement->fixed_duration)
        {
            const auto elapsed_ticks = current_tick >= visual.active_tick
                ? current_tick - visual.active_tick : 0;
            supplemental_input.elapsed_seconds =
                static_cast<float>(elapsed_ticks) / 60.0f;
            if (supplemental_input.elapsed_seconds >= supplement_effect->seconds)
                continue;
            supplemental_input.normalized_age = std::clamp(
                supplemental_input.elapsed_seconds /
                    supplement_effect->seconds,
                0.0f, 1.0f);
            std::get<VfxLinePayload>(supplemental_input.payload).lifetime01 =
                supplemental_input.normalized_age;
        }
        supplemental_input.stable_seed = PersistentSeed(
            supplemental_input.stable_id, supplemental_input.effect_handle);
        result.persistent.push_back(std::move(supplemental_input));
    }

    return result;
}

std::uint64_t VfxUpgradeMovementState::Rebind(const VfxProgramView &previous,
                                           const VfxProgramView &next) noexcept
{
    const auto count = owners_.size();
    std::erase_if(owners_, [&](Owner &owner) {
        if (RebindVfxGenerationHandle(previous, next, owner.input.effect_handle, 1u) !=
            VfxGenerationRebindOutcome::Retained) return true;
        const auto &effect = next.effects[owner.input.effect_handle - 1u];
        if (effect.timing_kind != 0 || !std::isfinite(effect.seconds) || effect.seconds <= 0 ||
            !UpgradeBindingContains(next, Hash64("skill.retreat_shot"), 8, effect.handle)) return true;
        owner.seconds = effect.seconds;
        owner.input.source_duration_seconds = effect.seconds;
        return false;
    });
    return count - owners_.size();
}

std::vector<VfxPersistentInput> VfxUpgradeMovementState::Emit(
    const VfxProgramView &cooked,
    std::span<const PresentationEvent> dispatched_events,
    Tick current_tick)
{
    std::erase_if(owners_, [current_tick](const Owner &owner) {
        return current_tick < owner.started ||
            static_cast<float>(current_tick - owner.started) / 60.0f >= owner.seconds;
    });
    for (const auto &event : dispatched_events)
    {
        const bool dash = event.asset.value == Hash64("particle.upgrade.retreat.second_dash");
        const bool afterimage = event.asset.value == Hash64("particle.upgrade.retreat.afterimage");
        if ((!dash && !afterimage) || event.kind != PresentationKind::Vfx ||
            event.upgrade_skill != kRetreatShotSkill || event.upgrade_index != 7 ||
            event.upgrade_stage != 1 || event.sequence == 0 ||
            event.upgrade_cast_id == 0 || event.upgrade_owner_id == 0 ||
            event.tick > current_tick || !Finite(event.position) ||
            !Finite(event.geometry) || event.geometry.kind != PresentationGeometryKind::Line ||
            event.geometry.source_id != event.upgrade_owner_id || event.geometry.width <= 0 ||
            event.geometry.start_tick != event.tick || event.geometry.end_tick != event.tick)
            continue;
        const auto *lookup = FindLookup(cooked, event.asset.value);
        const auto *effect = lookup ? FindEffect(cooked, lookup->handle) : nullptr;
        if (!effect || effect->input_mode != 1 || effect->timing_kind != 0 ||
            effect->payload_kind != (dash ? kLinePayload : kContextPayload) ||
            !std::isfinite(effect->seconds) || effect->seconds <= 0 ||
            !UpgradeBindingContains(cooked, Hash64("skill.retreat_shot"), 8, effect->handle) ||
            static_cast<float>(current_tick - event.tick) / 60.0f >= effect->seconds)
            continue;
        const auto end = event.geometry.end_position;
        const Float3 delta{end.x - event.position.x, end.y - event.position.y,
                           end.z - event.position.z};
        const float length = std::hypot(delta.x, std::hypot(delta.y, delta.z));
        if (!std::isfinite(length) || length <= 0.0001f) continue;
        const Float3 direction{delta.x / length, delta.y / length, delta.z / length};
        const auto stable_id = SupplementalStableId(
            event.upgrade_cast_id ^ event.sequence ^ event.upgrade_owner_id,
            event.asset.value);
        if (std::any_of(owners_.begin(), owners_.end(), [stable_id](const Owner &owner) {
                return owner.input.stable_id == stable_id;
            }))
            continue;
        Owner owner{};
        owner.started = event.tick;
        owner.seconds = effect->seconds;
        owner.input.stable_id = stable_id;
        owner.input.effect_handle = effect->handle;
        owner.input.source_visual_kind = 0xFF;
        owner.input.current_transform = Transform(event.position,
                                                  std::atan2(direction.x, direction.z));
        owner.input.previous_transform = owner.input.current_transform;
        owner.input.stable_seed = PersistentSeed(stable_id, effect->handle);
        owner.input.source_duration_seconds = effect->seconds;
        if (dash)
            owner.input.payload = VfxLinePayload{event.position, end, direction,
                                                 event.geometry.width, 0.0f, 0};
        else
            owner.input.payload = VfxContextPayload{event.upgrade_owner_id, direction,
                                                    0.0f, length, event.geometry.width};
        owners_.push_back(std::move(owner));
    }
    std::vector<VfxPersistentInput> result;
    result.reserve(owners_.size());
    for (const auto &owner : owners_)
    {
        auto input = owner.input;
        input.elapsed_seconds = static_cast<float>(current_tick - owner.started) / 60.0f;
        input.normalized_age = std::clamp(input.elapsed_seconds / owner.seconds, 0.0f, 1.0f);
        if (auto *line = std::get_if<VfxLinePayload>(&input.payload))
            line->lifetime01 = input.normalized_age;
        else
            std::get<VfxContextPayload>(input.payload).ratio01 = input.normalized_age;
        result.push_back(std::move(input));
    }
    return result;
}

void RefreshVfxGroundEventOwners(std::vector<VfxEventInput> &events,
                                 std::span<const PersistentVfxVisual> owners,
                                 Tick current_tick)
{
    std::vector<VfxEventInput> refreshed;
    refreshed.reserve(events.size());
    for (auto &event : events)
    {
        if (event.geometry_owner_id == 0)
        {
            refreshed.push_back(std::move(event));
            continue;
        }
        if (event.event_tick > current_tick)
        {
            refreshed.push_back(std::move(event));
            continue;
        }
        const auto binding = WarningBinding(event.geometry_effect_id);
        if (!binding || !WarningShapeMatches(*binding, event.payload)) continue;
        const bool line = binding->payload_kind == kLinePayload;
        const auto expected_kind = binding->kind;
        const auto found = std::find_if(owners.begin(), owners.end(),
            [&](const PersistentVfxVisual &owner) {
                return owner.stable_id == event.geometry_owner_id &&
                    owner.kind == expected_kind;
            });
        if (found == owners.end() || !FiniteWarningOwner(*found, current_tick)) continue;
        const auto &owner = *found;
        const float age = static_cast<float>(current_tick - owner.active_tick) /
            static_cast<float>(owner.expires - owner.active_tick);
        if (!std::isfinite(age) || age < 0.0f || age >= 1.0f) continue;
        event.world_transform = Transform(owner.position, owner.yaw);
        event.geometry_start_tick = owner.active_tick;
        event.geometry_end_tick = owner.expires;
        if (line)
        {
            if (!std::isfinite(owner.length) || owner.length <= 0.0f) continue;
            const Float3 direction{std::sin(owner.yaw), 0.0f, std::cos(owner.yaw)};
            auto &payload = std::get<VfxLinePayload>(event.payload);
            payload.start = {owner.position.x - direction.x * owner.length * 0.5f,
                             owner.position.y - direction.y * owner.length * 0.5f,
                             owner.position.z - direction.z * owner.length * 0.5f};
            payload.end = {owner.position.x + direction.x * owner.length * 0.5f,
                           owner.position.y + direction.y * owner.length * 0.5f,
                           owner.position.z + direction.z * owner.length * 0.5f};
            payload.direction = direction;
            payload.width = owner.radius * 2.0f;
            payload.lifetime01 = age;
        }
        else if (binding->payload_kind == kSafeSectorPayload)
        {
            VfxPayload ring_payload;
            if (!MakeRingGaps(owner, age, ring_payload) ||
                !MakeSafeSector(std::get<VfxRingGapsPayload>(ring_payload), event.geometry_sector_index, event.payload))
                continue;
        }
        else if (binding->payload_kind == kRingGapsPayload)
        {
            if (!MakeRingGaps(owner, age, event.payload)) continue;
        }
        else if (binding->payload_kind == kCirclePayload)
        {
            if (!MakeCircle(owner, age, event.payload)) continue;
        }
        else
        {
            if (!std::isfinite(owner.cone_half_angle_degrees) ||
                owner.cone_half_angle_degrees <= 0.0f ||
                owner.cone_half_angle_degrees > 180.0f)
                continue;
            const Float3 direction{std::sin(owner.yaw), 0.0f, std::cos(owner.yaw)};
            auto &payload = std::get<VfxConePayload>(event.payload);
            payload.origin = owner.position;
            payload.direction = direction;
            payload.range = owner.radius;
            payload.half_angle_degrees = owner.cone_half_angle_degrees;
            payload.lifetime01 = age;
        }
        refreshed.push_back(std::move(event));
    }
    events = std::move(refreshed);
}

void RebindVfxGroundEventOwners(const VfxProgramView &cooked,
                                std::vector<VfxEventInput> &events)
{
    std::vector<VfxEventInput> rebound;
    rebound.reserve(events.size());
    for (auto &event : events)
    {
        if (event.geometry_owner_id == 0)
        {
            rebound.push_back(std::move(event));
            continue;
        }
        const auto binding = WarningBinding(event.geometry_effect_id);
        if (!binding || !WarningShapeMatches(*binding, event.payload)) continue;
        const auto *lookup = FindLookup(cooked, binding->effect_id);
        const auto *effect = lookup ? FindEffect(cooked, lookup->handle) : nullptr;
        const auto expected_payload = binding->payload_kind;
        if (!lookup || !effect || effect->input_mode != 0 ||
            effect->payload_kind != expected_payload ||
            (binding->effect_id == kSafeMarkerEffect && effect->timing_kind != 1))
            continue;
        event.effect_handle = effect->handle;
        rebound.push_back(std::move(event));
    }
    events = std::move(rebound);
}

} // namespace hs::runtime_detail
