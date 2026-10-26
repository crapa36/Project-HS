#include "simulation_world.hpp"
#include <limits>
#include <set>
#include <tuple>

namespace hs
{

using namespace gameplay_detail;
namespace
{
float MaximumEnemyCollisionRadius(const SimulationRules &rules) noexcept
{
    auto maximum=rules.boss_common.collision_radius;
    for(const auto &enemy:rules.enemies) maximum=std::max(maximum,enemy.collision_radius);
    return maximum;
}

void BeginStatusEpisode(EnemyRow enemy, std::size_t index, Tick tick) noexcept
{
    bool active=false;
    if(index==0) for(std::size_t i=0;i<enemy.status.bleed_count;++i) active|=enemy.status.bleeds[i].expires>tick;
    else if(index==1) active=enemy.status.burn&&enemy.status.burn->expires>tick;
    else if(index==2) for(const auto&slow:enemy.status.slows) active|=slow.expires>tick;
    else active=enemy.cold.marked_by_skill!=SkillKind::Count&&enemy.cold.mark_expires>=tick;
    auto &episode=enemy.cold.status_visual_episodes[index];
    if(!active||episode.generation==0){episode.started=tick;++episode.generation;}
}

DomainSignalGeometry ExplosionGeometry(float radius) noexcept
{
    DomainSignalGeometry geometry{};
    geometry.kind = DomainSignalGeometryKind::Circle;
    geometry.radius = radius;
    return geometry;
}

DomainSignalGeometry ProjectileImpactGeometry(ConstProjectileRow projectile) noexcept
{
    DomainSignalGeometry geometry{};
    geometry.kind = DomainSignalGeometryKind::Projectile;
    geometry.velocity = {projectile.motion.velocity.x, 0.0f, projectile.motion.velocity.y};
    geometry.radius = projectile.motion.radius;
    geometry.source_id = projectile.id.value;
    return geometry;
}

DomainSignalKind ProjectileImpactKind(ConstProjectileRow projectile) noexcept
{
    if (projectile.combat.player_owned && projectile.combat.skill == SkillKind::ChargedShot)
        return DomainSignalKind::ChargedShotProjectileImpact;
    if (!projectile.combat.player_owned && projectile.combat.source_enemy != kNoTelemetrySource)
    {
        if (projectile.combat.source_enemy >= kBossTelemetryBase)
            return DomainSignalKind::BossVolleyProjectileImpact;
        if (projectile.combat.source_enemy == static_cast<std::uint8_t>(EnemyKind::Ranged))
            return DomainSignalKind::EnemyRangedProjectileImpact;
    }
    return DomainSignalKind::Count;
}

void BindStatusVisual(DomainSignal &signal, ConstEnemyRow enemy,
                      SkillKind skill, std::uint8_t source_upgrade,
                      std::size_t episode_index) noexcept
{
    // A status application is owned by the enemy's actual status episode. It
    // can be applied by a transferred burn or an area before damage flush, so
    // do not borrow the enemy's last damage cast as its activation identity.
    signal.source_entity_id = enemy.id.value;
    signal.status_episode_generation = enemy.cold.status_visual_episodes[episode_index].generation;
    if (skill >= SkillKind::Count || source_upgrade >= kUpgradeCount) return;
    signal.upgrade_skill = static_cast<std::uint8_t>(skill);
    signal.upgrade_index = source_upgrade;
    signal.upgrade_stage = UpgradeVisualStage::Resolve;
    signal.upgrade_owner_id = enemy.id.value;
}

void BindUpgradeLink(VisualLinkView &link, SkillKind skill,
                     std::uint8_t source_upgrade, std::uint64_t cast_id) noexcept
{
    link.skill = skill;
    link.source_upgrade = source_upgrade;
    link.cast_id = cast_id;
}

} // namespace

void GameSimulation::SimulationWorld::EmitUpgradeVisual(
    SkillKind skill, std::uint8_t ordinal, UpgradeVisualStage stage,
    Float2 position, Float2 direction, std::uint64_t cast_id,
    std::uint64_t owner_id, DomainSignalGeometry geometry)
{
    assert(ordinal >= 1 && ordinal <= kUpgradeCount);
    DomainSignal signal;
    signal.sequence = NextSimulationSequence(event_sequence);
    signal.tick = tick;
    signal.session_id = session_id;
    signal.kind = DomainSignalKind::UpgradeVisual;
    signal.position = {position.x, 1.05f, position.y};
    signal.direction = {direction.x, 0.0f, direction.y};
    signal.upgrade_skill = static_cast<std::uint8_t>(skill);
    signal.upgrade_index = static_cast<std::uint8_t>(ordinal - 1u);
    signal.upgrade_stage = stage;
    signal.upgrade_cast_id = cast_id;
    signal.upgrade_owner_id = owner_id;
    signal.geometry = geometry;
    combat_state->domain_signals.push_back(signal);
}

float GameSimulation::SimulationWorld::EffectiveAttack() const noexcept
{
    const auto index = static_cast<std::size_t>(StatKind::AttackPower);
    const auto pickup_bonus = tick < actors->player.pickup_reward_until
                                  ? rules.relics.pickup_reward.attack_power_fraction
                                  : 0.0f;
    return rules.stats.base_attack_power *
           (1.0f + rules.stats.allocations[index].amount_per_point * actors->player.stats[index]) *
           (1.0f + pickup_bonus);
}

float GameSimulation::SimulationWorld::EffectiveAttackSpeed() const noexcept
{
    const auto index = static_cast<std::size_t>(StatKind::AttackSpeed);
    return rules.stats.base_basic_attack_rate_per_second *
           (1.0f + rules.stats.allocations[index].amount_per_point * actors->player.stats[index]);
}

void GameSimulation::SimulationWorld::RecordUpgradeEffect(SkillKind skill, std::uint8_t upgrade,
                         UpgradeEffectMetric metric,
                         std::uint64_t amount) noexcept
{
    if (skill >= SkillKind::Count || upgrade >= kUpgradeCount || amount == 0) return;
    Metrics().upgrade_effects[static_cast<std::size_t>(skill)][upgrade]
                           [static_cast<std::size_t>(metric)] += amount;
}

void GameSimulation::SimulationWorld::RecordRelicEffect(RelicKind relic, UpgradeEffectMetric metric,
                       std::uint64_t amount) noexcept
{
    if (relic >= RelicKind::Count || amount == 0) return;
    Metrics().relic_effects[static_cast<std::size_t>(relic)]
                         [static_cast<std::size_t>(metric)] += amount;
}

void GameSimulation::SimulationWorld::RecordUpgradeRelicSynergy(SkillKind skill, std::uint8_t upgrade,
                               RelicKind relic,
                               UpgradeRelicSynergyMetric metric,
                               std::uint64_t amount) noexcept
{
    if (!Metrics().detailed_balance_captured) return;
    if (skill >= SkillKind::Count || upgrade >= kUpgradeCount ||
        relic >= RelicKind::Count || amount == 0)
        return;
    auto entries = std::span(Metrics().upgrade_relic_synergies.data(),
                             Metrics().upgrade_relic_synergy_count);
    auto found = std::ranges::find_if(entries, [&](const auto &entry) {
        return entry.skill == skill && entry.upgrade == upgrade &&
               entry.relic == relic;
    });
    UpgradeRelicSynergyTelemetry *entry{};
    if (found == entries.end())
    {
        if (Metrics().upgrade_relic_synergy_count ==
            Metrics().upgrade_relic_synergies.size())
            return;
        entry = &Metrics().upgrade_relic_synergies[
            Metrics().upgrade_relic_synergy_count++];
        entry->skill = skill;
        entry->upgrade = upgrade;
        entry->relic = relic;
    }
    else
    {
        entry = &*found;
    }
    entry->metrics[static_cast<std::size_t>(metric)] += amount;
}

void GameSimulation::SimulationWorld::RecordUpgradeDamage(SkillKind skill, std::uint8_t upgrade,
                         std::uint64_t amount) noexcept
{
    if (skill >= SkillKind::Count || upgrade >= kUpgradeCount || amount == 0)
        return;
    Metrics().upgrade_damage[static_cast<std::size_t>(skill)][upgrade] += amount;
    ++Metrics().upgrade_triggers[static_cast<std::size_t>(skill)][upgrade];
    RecordUpgradeEffect(skill, upgrade, UpgradeEffectMetric::DamageAmplified,
                        amount);
}

void GameSimulation::SimulationWorld::RecordUpgradeDisplacement(SkillKind skill, std::uint8_t upgrade,
                               Float2 before, Float2 after) noexcept
{
    RecordUpgradeEffect(
        skill, upgrade, UpgradeEffectMetric::DisplacementMillimetres,
        static_cast<std::uint64_t>(std::llround(DistanceSquared(before, after) > 0.0f
                                                    ? Length(Subtract(after, before)) * 1000.0f
                                                    : 0.0f)));
}

void GameSimulation::SimulationWorld::QueueEnemyDisplacement(
    EnemyRow enemy, Float2 displacement) noexcept
{
    constexpr Tick kDisplacementTicks = 8;
    const auto remaining = Multiply(enemy.motion.displacement_per_tick,
                                    static_cast<float>(enemy.motion.displacement_ticks));
    enemy.motion.displacement_per_tick = Multiply(Add(remaining, displacement),
                                           1.0f / kDisplacementTicks);
    enemy.motion.displacement_ticks = kDisplacementTicks;
}

float GameSimulation::SimulationWorld::EffectiveMagnetRadius() const noexcept
{
    const auto index = static_cast<std::size_t>(StatKind::MagnetRadius);
    return rules.stats.base_magnet_radius_m +
           rules.stats.allocations[index].amount_per_point * actors->player.stats[index];
}

std::size_t GameSimulation::SimulationWorld::EnemyTelemetryIndex(ConstEnemyRow enemy) noexcept
{
    return enemy.life.boss ? 3u + static_cast<std::size_t>(*enemy.life.boss)
                      : static_cast<std::size_t>(enemy.life.kind);
}

Tick GameSimulation::SimulationWorld::EffectiveCooldownTicks(SkillKind skill) const noexcept
{
    const auto index = static_cast<std::size_t>(skill);
    auto ticks = static_cast<float>(rules.skills[index].cooldown_ticks);
    const auto reduction_index = static_cast<std::size_t>(StatKind::CooldownReduction);
    const auto reduction = rules.stats.allocations[reduction_index].amount_per_point *
                           actors->player.stats[reduction_index];
    ticks *= 1.0f - reduction;
    if (HasUpgrade(actors->player.upgrades[index], 8))
    {
        const auto multiplier =
            skill == SkillKind::PiercingShot
                ? rules.upgrades.piercing_shot.cooldown_multiplier.cooldown_multiplier
                : skill == SkillKind::ExplosiveArrow
                      ? rules.upgrades.explosive_arrow.cooldown_multiplier.cooldown_multiplier
                      : skill == SkillKind::RicochetArrow
                            ? rules.upgrades.ricochet_arrow.cooldown_multiplier.cooldown_multiplier
                            : 1.0f;
        ticks *= multiplier;
    }
    return std::max<Tick>(rules.stats.combat.minimum_cooldown_ticks,
                          static_cast<Tick>(std::llround(ticks)));
}

std::optional<EnemyRow> GameSimulation::SimulationWorld::FindEnemy(std::uint64_t id) noexcept
{
    const auto &ids = actors->enemies.ids;
    const auto found = std::ranges::lower_bound(ids, id, {}, &EntityId::value);
    if (found == ids.end() || found->value != id) return std::nullopt;
    return actors->enemies[static_cast<std::size_t>(found - ids.begin())];
}

std::optional<ConstEnemyRow> GameSimulation::SimulationWorld::FindEnemy(std::uint64_t id) const noexcept
{
    const auto &store = std::as_const(actors->enemies);
    const auto found = std::ranges::lower_bound(store.ids, id, {}, &EntityId::value);
    if (found == store.ids.end() || found->value != id) return std::nullopt;
    return store[static_cast<std::size_t>(found - store.ids.begin())];
}

std::uint32_t GameSimulation::SimulationWorld::NormalEnemyCount() const noexcept
{
    return static_cast<std::uint32_t>(std::ranges::count_if(
        actors->enemies, [](ConstEnemyRow enemy) { return !enemy.life.boss && !enemy.life.dead; }));
}

std::uint32_t GameSimulation::SimulationWorld::BossCount() const noexcept
{
    return static_cast<std::uint32_t>(std::ranges::count_if(
        actors->enemies, [](ConstEnemyRow enemy) { return enemy.life.boss && !enemy.life.dead; }));
}

std::uint32_t GameSimulation::SimulationWorld::ProjectileCount(bool player_owned) const noexcept
{
    return static_cast<std::uint32_t>(std::ranges::count_if(
        actors->projectiles, [player_owned](ConstProjectileRow projectile) {
            return projectile.combat.player_owned == player_owned && !projectile.combat.dead;
        }));
}

std::optional<ProjectileRow> GameSimulation::SimulationWorld::FireProjectile(SkillKind skill, Float2 position, Float2 direction,
                                float coefficient, EffectOrigin origin,
                                std::uint64_t cast_id,
                                std::uint8_t applied_upgrade_mask,
                                bool player_owned,
                                std::uint8_t source_upgrade,
                                std::uint8_t source_relic,
                                std::uint8_t source_enemy,
                                std::uint64_t source_entity_id)
{
    assert(skill >= SkillKind::Count ||
           (applied_upgrade_mask &
            ~actors->player.upgrades[static_cast<std::size_t>(skill)]) == 0);
    const auto &definition = rules.skills[static_cast<std::size_t>(skill)];
    const auto projectile_speed =
        definition.projectile_speed > 0.0f
            ? definition.projectile_speed
            : rules.skills[static_cast<std::size_t>(SkillKind::BasicAttack)]
                  .projectile_speed;
    PreparedProjectile projectile;
    projectile.id = AllocateEntityId();
    projectile.combat.player_owned = player_owned;
    projectile.motion.position = projectile.motion.previous_position = position;
    projectile.combat.spawned_tick = tick;
    const auto normalized = Normalize(direction);
    projectile.motion.velocity = Multiply(
        normalized, player_owned ? projectile_speed
                                 : rules.enemies[static_cast<std::size_t>(
                                       EnemyKind::Ranged)].projectile_speed);
    projectile.motion.remaining_range = player_owned
                                     ? definition.range
                                     : rules.enemies[static_cast<std::size_t>(EnemyKind::Ranged)]
                                           .projectile_range;
    projectile.motion.radius = player_owned
                             ? definition.collision_radius
                             : rules.enemies[static_cast<std::size_t>(EnemyKind::Ranged)]
                                   .ranged_projectile_radius;
    projectile.combat.damage = player_owned ? RoundFinalDamage(EffectiveAttack() * coefficient, rules)
                                     : RoundFinalDamage(coefficient, rules);
    projectile.combat.skill = skill;
    projectile.combat.upgrade_mask = applied_upgrade_mask;
    projectile.combat.origin = origin;
    projectile.combat.cast_id = cast_id;
    projectile.combat.source_upgrade = source_upgrade;
    projectile.combat.source_relic = source_relic;
    projectile.combat.source_enemy = source_enemy;
    const auto full_skill_effect = origin == EffectOrigin::Original ||
                                   applied_upgrade_mask != 0;
    projectile.combat.pierce_remaining = full_skill_effect ? definition.pierce_count : 0;
    if (skill == SkillKind::BasicAttack && HasUpgrade(projectile.combat.upgrade_mask, 2))
    {
        ++projectile.combat.pierce_remaining;
    }
    if (skill == SkillKind::MultiShot && HasUpgrade(projectile.combat.upgrade_mask, 3))
    {
        ++projectile.combat.pierce_remaining;
    }
    if (skill == SkillKind::RicochetArrow && full_skill_effect)
    {
        projectile.combat.bounce_remaining = definition.pierce_count;
    }
    if (skill == SkillKind::ExplosiveArrow && full_skill_effect)
    {
        projectile.combat.explosion_radius = definition.area_radius;
        projectile.combat.explosion_damage = projectile.combat.damage;
    }
    const auto defer_spawn = pipeline_phase == SimulationPhaseId::CastAttack ||
                             pipeline_phase == SimulationPhaseId::CollisionHit ||
                             pipeline_phase == SimulationPhaseId::DeathDrop;
    auto &storage = defer_spawn
                        ? actors->pending_projectile_spawns
                        : actors->projectiles;
    storage.push_back(std::move(projectile));
    if (player_owned && origin == EffectOrigin::Original &&
        skill < SkillKind::Count && skill != SkillKind::MultiShot)
    {
        EmitVfx(DomainSignalKind::ArrowReleased,
                Add(position, Multiply(normalized, 0.65f)), normalized,
                1.0f, 1.05f, static_cast<std::uint8_t>(skill));
    }
    if (player_owned && skill < SkillKind::Count &&
        skill != SkillKind::BasicAttack && skill != SkillKind::MultiShot &&
        origin == EffectOrigin::Original)
    {
        constexpr std::array<DomainSignalKind, kCombatSkillCount> effects{
            DomainSignalKind::BasicAttackImpact, DomainSignalKind::PiercingShotCast,
            DomainSignalKind::MultiShotCast, DomainSignalKind::ChargedShotCast,
            DomainSignalKind::ExplosiveArrowCast, DomainSignalKind::RicochetArrowCast,
            DomainSignalKind::ArrowRainCast, DomainSignalKind::TrapCast,
            DomainSignalKind::RetreatShotCast};
        const auto release_position = Add(position, Multiply(normalized, 0.65f));
        EmitVfx(effects[static_cast<std::size_t>(skill)], release_position, direction,
                origin == EffectOrigin::Original ? 1.0f : 0.6f, 1.1f);
    }
    else if (!player_owned && source_enemy ==
                                  static_cast<std::uint8_t>(EnemyKind::Ranged))
        EmitVfx(DomainSignalKind::RangedEnemyReleased,
                Add(position, Multiply(normalized, 0.5f)), direction, 1.0f, 0.8f,
                source_enemy, source_entity_id);
    RecordUpgradeEffect(skill, source_upgrade,
                        UpgradeEffectMetric::ProjectilesCreated);
    return storage.back();
}

AreaActor *GameSimulation::SimulationWorld::SpawnArea(AreaKind kind, SkillKind skill, Float2 position, float radius,
                     float coefficient, float duration, float activation_delay,
                     EffectOrigin origin, std::uint64_t cast_id,
                     std::uint8_t applied_upgrade_mask,
                     float slow_reduction, float slow_duration,
                     float effect_radius, bool applies_burn,
                     std::uint8_t source_upgrade,
                     std::uint8_t source_relic,
                     std::uint8_t source_enemy)
{
    assert(skill >= SkillKind::Count ||
           (applied_upgrade_mask &
            ~actors->player.upgrades[static_cast<std::size_t>(skill)]) == 0);
    AreaActor area;
    area.id = AllocateEntityId();
    area.kind = kind;
    area.position = position;
    area.radius = radius;
    area.effect_radius = effect_radius > 0.0f ? effect_radius : radius;
    area.damage = RoundFinalDamage(EffectiveAttack() * coefficient, rules);
    area.active_tick = tick + Seconds(activation_delay);
    area.next_tick = area.active_tick;
    area.expires = area.active_tick + Seconds(duration);
    area.skill = skill;
    area.upgrade_mask = applied_upgrade_mask;
    area.origin = origin;
    area.cast_id = cast_id;
    area.source_upgrade = source_upgrade;
    area.source_relic = source_relic;
    area.source_enemy = source_enemy;
    area.slow_reduction = slow_reduction;
    area.slow_duration = Seconds(slow_duration);
    area.applies_burn = applies_burn;
    auto &storage = pipeline_phase == SimulationPhaseId::CastAttack
                        ? actors->pending_area_spawns
                        : actors->areas;
    storage.push_back(area);
    RecordUpgradeEffect(skill, source_upgrade,
                        UpgradeEffectMetric::AreasCreated);
    return &storage.back();
}

void GameSimulation::SimulationWorld::ScheduleAction(const ScheduledAction &action)
{
    auto scheduled = action;
    if(combat_state->next_scheduled_sequence==std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Scheduled action ordering sequence exhausted.");
    scheduled.schedule_sequence=combat_state->next_scheduled_sequence++;
    if (action.kind == ScheduledKind::Area && action.skill == SkillKind::ArrowRain &&
        action.origin == EffectOrigin::Derived && action.source_upgrade == 0)
    {
        // Keep the actual schedule's identity/clock without allocating an
        // entity, emitting a success event, or changing damage/RNG state.
        scheduled.visual_started = tick;
        std::uint64_t identity = 14695981039346656037ull;
        for (const auto value : {action.cast_id, static_cast<std::uint64_t>(action.due),
                                std::uint64_t{0x4152524f57524149}})
            for (unsigned byte = 0; byte < 8; ++byte)
            {
                identity ^= (value >> (byte * 8)) & 255u;
                identity *= 1099511628211ull;
            }
        scheduled.visual_owner_id = identity ? identity : 1;
    }
    if (action.kind == ScheduledKind::Explosion && action.skill == SkillKind::ExplosiveArrow &&
        action.source_upgrade == 1)
    {
        scheduled.visual_started = tick;
        scheduled.visual_owner_id = PendingSimulationSequence(event_sequence);
        for (const auto stage : {UpgradeVisualStage::Spawn, UpgradeVisualStage::Telegraph})
        {
            DomainSignal signal;
            signal.sequence = NextSimulationSequence(event_sequence); signal.tick = tick; signal.session_id = session_id;
            signal.kind = DomainSignalKind::UpgradeVisual;
            signal.position = {action.position.x, .15f, action.position.y};
            signal.upgrade_skill = static_cast<std::uint8_t>(action.skill);
            signal.upgrade_index = action.source_upgrade; signal.upgrade_stage = stage;
            signal.upgrade_cast_id = action.cast_id; signal.upgrade_owner_id = scheduled.visual_owner_id;
            signal.geometry = ExplosionGeometry(action.radius);
            signal.geometry.source_id = scheduled.visual_owner_id;
            signal.geometry.start_tick = tick; signal.geometry.end_tick = action.due;
            combat_state->domain_signals.push_back(signal);
        }
    }
    combat_state->scheduled_actions.push_back(scheduled);
    combat_state->scheduled_actions_dirty=true;
}

void GameSimulation::SimulationWorld::QueueDamage(std::uint64_t target, std::int32_t amount, SkillKind skill,
                EffectOrigin origin, std::uint64_t cast_id,
                std::uint8_t bleed, bool burn,
                float slow, Tick slow_duration,
                std::uint8_t source_upgrade,
                std::uint8_t source_relic,
                std::uint8_t source_enemy,
                std::uint64_t originating_area_id, Float2 incoming_direction,
                bool shockwave_contact, Float2 impact_position)
{
    if (target == 0 && tick < actors->player.revive_invulnerable_until)
    {
        RecordRelicEffect(RelicKind::OnceRevive,
                          UpgradeEffectMetric::DamagePrevented,
                          static_cast<std::uint64_t>(std::max(0, amount)));
        return;
    }
    const auto proc = MakeProcContext(cast_id, origin);
    if (!proc.Allows(ProcPermission::Damage)) return;
    if (!proc.Allows(ProcPermission::Status))
    {
        bleed = 0;
        burn = false;
        slow = 0.0f;
        slow_duration = 0;
    }
    combat_state->combat.Enqueue({++damage_sequence, target,
                  std::max(rules.stats.combat.minimum_final_damage, amount), skill,
                 origin, proc.root_cast, source_upgrade, source_relic, source_enemy,
                 bleed, burn, slow, slow_duration, {}, {}, 0,
                 originating_area_id, incoming_direction, impact_position,
                 shockwave_contact});
}

void GameSimulation::SimulationWorld::QueueAreaDamage(Float2 position, float radius, std::int32_t damage, SkillKind skill,
                EffectOrigin origin, std::uint64_t cast_id,
                std::uint8_t bleed, bool burn,
                float slow, Tick slow_duration,
                std::uint8_t source_upgrade,
                std::uint8_t source_relic,
                std::uint64_t originating_area_id)
{
    CollectEnemies(position, radius, combat_state->area_query);
    for (const auto row : combat_state->area_query.rows)
    {
        const auto enemy = actors->enemies[row];
        if (!enemy.life.dead && DistanceSquared(position, enemy.motion.position) <= radius * radius &&
            SegmentClear2D(position, enemy.motion.position, 0.0f, rules.arena_boundary, ArenaObstacles()))
        {
            QueueDamage(enemy.id.value, damage, skill, origin, cast_id, bleed, burn,
                       slow, slow_duration, source_upgrade, source_relic,
                       kNoTelemetrySource, originating_area_id);
        }
    }
}

void GameSimulation::SimulationWorld::CastBasicAttack()
{
    if (!current_input.held.basic_attack_held || actors->player.charging ||
        actors->player.active_cast_tick > tick || tick < actors->player.next_basic_attack)
    {
        return;
    }
    const auto &definition = rules.skills[static_cast<std::size_t>(SkillKind::BasicAttack)];
    const auto &upgrades = rules.upgrades.basic_attack;
    actors->player.has_move_target = false;
    EmitSignal(DomainSignalKind::BasicAttackStarted, actors->player.position,
               static_cast<std::uint8_t>(SkillKind::BasicAttack));
    const auto attack_interval = std::max<Tick>(1, static_cast<Tick>(
        std::floor(60.0f / EffectiveAttackSpeed() + 0.5f)));
    const auto animation_ticks = std::max<Tick>(
        1, attack_interval > kAnimationBlendOutTicks
               ? attack_interval - kAnimationBlendOutTicks
               : 1);
    const auto release_ticks = AnimationMarkerTicks(kBasicArrowReleaseTicks,
                                                    animation_ticks);
    actors->player.next_basic_attack = tick + attack_interval;
    ++Metrics().skill_uses[static_cast<std::size_t>(SkillKind::BasicAttack)];
    const auto base_interval = std::max<Tick>(
        1, static_cast<Tick>(std::floor(
               60.0f / rules.stats.base_basic_attack_rate_per_second + 0.5f)));
    Metrics().stat_utility[static_cast<std::size_t>(StatKind::AttackSpeed)] +=
        base_interval - std::min(base_interval, attack_interval);
    actors->player.basic_attack_animation_start = tick;
    actors->player.basic_attack_animation_until = actors->player.next_basic_attack;
    ++actors->player.basic_sequence;
    actors->player.facing = actors->player.aim;
    const auto cast_id = TakeSimulationIdentity(next_cast_id);
    actors->player.basic_attack_cast_id = cast_id;
    actors->player.basic_attack_release_tick = tick + release_ticks;
    const auto mask = actors->player.upgrades[0];
    FindOrCreateCastRuntime(cast_id, SkillKind::BasicAttack).upgrade_mask = mask;
    const auto fire = [&](Float2 direction, float coefficient,
                          std::uint8_t source_upgrade = kNoTelemetrySource,
                          bool empowered = false, float fan_angle = 0.0f) {
        auto arrow = ScheduledAction{tick + release_ticks, ScheduledKind::Projectile,
                  SkillKind::BasicAttack, actors->player.position, direction, coefficient,
                  0.0f, 0.0f, 1, mask, EffectOrigin::Original, cast_id,
                  source_upgrade};
        if (empowered)
        {
            arrow.visual_owner_id = cast_id;
            arrow.visual_fan_angle = fan_angle;
        }
        ScheduleAction(arrow);
    };

    if (HasUpgrade(mask, 8) && tick <= actors->player.active_basic_empower_until)
    {
        const auto &post_active = upgrades.post_active_three_arrow;
        for (std::uint32_t index = 0; index < post_active.projectile_count; ++index)
        {
            const auto angle = post_active.angles_degrees[index];
            fire(Rotate(actors->player.aim, angle), post_active.damage_multiplier_per_arrow,
                 angle == 0.0f ? kNoTelemetrySource : 7, true, angle);
        }
        actors->player.active_basic_empower_until = 0;
    }
    else
    {
        fire(actors->player.aim, definition.damage_coefficient);
    }
    const auto &third_attack = upgrades.third_attack_delayed;
    if (HasUpgrade(mask, 1) &&
        actors->player.basic_sequence % third_attack.cadence_interval == 0)
    {
        ScheduleAction({tick + release_ticks + third_attack.delay_ticks, ScheduledKind::Projectile,
                  SkillKind::BasicAttack,
                  actors->player.position, actors->player.aim, third_attack.damage_multiplier, 0.0f, 0.0f, 1,
                  static_cast<std::uint8_t>(mask & ~std::uint8_t{1}),
                  EffectOrigin::Original, cast_id, 0});
    }
    DispatchBasicAttackRules(release_ticks, cast_id);
}

bool GameSimulation::SimulationWorld::CastSkill(SkillKind skill)
{
    const auto skill_index = static_cast<std::size_t>(skill);
    const auto cooldown_index = skill_index - 1;
    const auto &definition = rules.skills[skill_index];
    if (skill_index == 0 || skill_index >= kCombatSkillCount ||
        actors->player.cooldowns[cooldown_index] != 0 || actors->player.charging ||
        actors->player.active_cast_tick > tick || tick < actors->player.retreat_until)
    {
        return false;
    }
    actors->player.cooldowns[cooldown_index] = EffectiveCooldownTicks(skill) + 1;
    if (HasUpgrade(actors->player.upgrades[skill_index], 8) &&
        (skill == SkillKind::PiercingShot || skill == SkillKind::ExplosiveArrow ||
         skill == SkillKind::RicochetArrow))
    {
        const auto stat_reduction =
            1.0f - rules.stats.allocations[static_cast<std::size_t>(
                                 StatKind::CooldownReduction)].amount_per_point *
                       actors->player.stats[static_cast<std::size_t>(StatKind::CooldownReduction)];
        const auto without_upgrade = std::max<Tick>(
            rules.stats.combat.minimum_cooldown_ticks,
            static_cast<Tick>(std::llround(
                definition.cooldown_ticks * stat_reduction)));
        RecordUpgradeEffect(
            skill, 7, UpgradeEffectMetric::CooldownTicksSaved,
            without_upgrade - std::min(without_upgrade,
                                         actors->player.cooldowns[cooldown_index] - 1));
    }
    ++Metrics().skill_uses[skill_index];
    const auto cooldown_reduction =
        rules.stats.allocations[static_cast<std::size_t>(StatKind::CooldownReduction)]
                .amount_per_point *
        actors->player.stats[static_cast<std::size_t>(StatKind::CooldownReduction)];
    if (cooldown_reduction > 0.0f)
    {
        const auto without_stat = static_cast<Tick>(std::llround(
            static_cast<double>(actors->player.cooldowns[cooldown_index] - 1) /
            (1.0 - cooldown_reduction)));
        Metrics().stat_utility[static_cast<std::size_t>(StatKind::CooldownReduction)] +=
            without_stat - std::min(without_stat, actors->player.cooldowns[cooldown_index] - 1);
    }
    actors->player.active_animation_start = tick;
    actors->player.basic_attack_animation_until = tick;
    // A successful skill supersedes the pending basic attack release and its
    // cast-owned followups. Input observes the last completed simulation tick.
    if (actors->player.basic_attack_cast_id != 0 && tick < actors->player.basic_attack_release_tick)
    {
        const auto basic_cast = actors->player.basic_attack_cast_id;
        std::erase_if(combat_state->scheduled_actions,
                      [basic_cast](const ScheduledAction &action) {
                          return action.skill == SkillKind::BasicAttack &&
                                 action.cast_id == basic_cast;
                      });
        if (combat_state->scheduled_actions.empty())
            combat_state->next_scheduled_sequence = 0;
        actors->player.basic_attack_cast_id = 0;
        actors->player.basic_attack_release_tick = 0;
    }
    constexpr std::array<Tick, kCombatSkillCount> recovery_ticks{
        0, 15, 18, 9, 18, 15, 15, 15, 13};
    actors->player.active_cast_tick = tick + recovery_ticks[skill_index];
    actors->player.active_animation_until = actors->player.active_cast_tick + kAnimationBlendOutTicks;
    const auto cast_id = TakeSimulationIdentity(next_cast_id);
    const auto mask = actors->player.upgrades[skill_index];
    FindOrCreateCastRuntime(cast_id, skill).upgrade_mask = mask;
    const auto skill_release_ticks = AnimationMarkerTicks(
        kSkillArrowReleaseTicks, recovery_ticks[skill_index]);
    actors->player.facing = actors->player.aim;
    const auto target_delta = Float2{current_input.held.aim_world.x - actors->player.position.x,
                                     current_input.held.aim_world.z - actors->player.position.y};
    const auto target_distance = Length(target_delta);
    const auto target = Add(actors->player.position,
                            Multiply(Normalize(target_delta, actors->player.aim),
                                     std::min(target_distance, rules.skills[skill_index].range)));

    switch (rules.skills[skill_index].handler)
    {
    case AbilityHandlerId::PiercingProjectile:
        ScheduleAction({tick + skill_release_ticks, ScheduledKind::Projectile, skill,
                  actors->player.position, actors->player.aim,
                  definition.damage_coefficient,
                  0.0f, 0.0f, 1, mask,
                  EffectOrigin::Original, cast_id});
        if (HasUpgrade(mask, 1))
        {
            const auto &followup = rules.upgrades.piercing_shot.delayed_followup;
            ScheduleAction({tick + skill_release_ticks + followup.delay_ticks, ScheduledKind::Projectile,
                      skill, actors->player.position,
                      actors->player.aim, followup.damage_multiplier, 0.0f, 0.0f, 1, 0,
                      EffectOrigin::Derived, cast_id, 0});
        }
        if (HasUpgrade(mask, 4))
        {
            const auto &trail = rules.upgrades.piercing_shot.damaging_slow_trail;
            const auto half_length = definition.range * 0.5f;
            auto *area = SpawnArea(
                AreaKind::Damage, skill,
                Add(actors->player.position, Multiply(actors->player.aim, half_length)), trail.width * 0.5f,
                trail.damage_multiplier_per_tick, static_cast<float>(trail.duration_ticks) /
                    60.0f, 0.0f, EffectOrigin::Derived, cast_id, 0,
                trail.slow_fraction, static_cast<float>(trail.slow_duration_ticks) / 60.0f);
            area->interval = trail.tick_interval_ticks;
            area->direction = actors->player.aim;
            area->half_length = half_length;
            area->source_upgrade = 3;
        }
        break;
    case AbilityHandlerId::UniformFanProjectiles:
    {
        const auto &multishot = rules.upgrades.multishot;
        const auto miss_retarget_mask = static_cast<std::uint8_t>(
            mask & (std::uint8_t{1} << 7));
        ScheduleAction({tick + skill_release_ticks, ScheduledKind::Volley, skill,
                  actors->player.position, actors->player.aim,
                  definition.damage_coefficient,
                  0.0f, 0.0f, rules.skills[skill_index].projectile_count, mask,
                  EffectOrigin::Original, cast_id});
        if (HasUpgrade(mask, 1))
        {
            const auto &second_fan = multishot.delayed_second_fan;
            ScheduleAction({tick + second_fan.delay_ticks, ScheduledKind::Volley,
                      skill, actors->player.position, actors->player.aim,
                      second_fan.damage_multiplier_per_arrow, 0.0f, 0.0f,
                      static_cast<std::uint8_t>(second_fan.projectile_count),
                      miss_retarget_mask,
                      EffectOrigin::Derived, cast_id, 0});
        }
        if (HasUpgrade(mask, 4))
        {
            const auto &rear_fan = multishot.rear_fan;
            const auto count = rear_fan.projectile_count;
            const auto step = count > 1
                                  ? rear_fan.fan_angle_degrees /
                                        static_cast<float>(count - 1)
                                  : 0.0f;
            for (std::uint32_t index = 0; index < count; ++index)
            {
                const auto angle = -rear_fan.fan_angle_degrees * 0.5f +
                                   step * static_cast<float>(index);
                auto rear_arrow = ScheduledAction{tick + skill_release_ticks, ScheduledKind::Projectile,
                          skill, actors->player.position,
                          Rotate(Multiply(actors->player.aim, -1.0f), angle),
                          rear_fan.damage_multiplier,
                          0.0f, 0.0f, 1, miss_retarget_mask,
                          EffectOrigin::Derived, cast_id, 3};
                rear_arrow.visual_fan_angle = angle;
                ScheduleAction(rear_arrow);
            }
        }
        if (HasUpgrade(mask, 7))
        {
            const auto &outer = multishot.two_additional_outer_arrows;
            for (std::uint32_t index = 0; index < outer.additional_projectiles; ++index)
            {
                const auto angle = outer.outer_angles_degrees[index];
                auto outer_arrow = ScheduledAction{tick + skill_release_ticks, ScheduledKind::Projectile,
                          skill, actors->player.position, Rotate(actors->player.aim, angle),
                          outer.damage_multiplier,
                          0.0f, 0.0f, 1, mask, EffectOrigin::Original, cast_id, 6};
                outer_arrow.visual_fan_angle = angle;
                ScheduleAction(outer_arrow);
            }
        }
        break;
    }
    case AbilityHandlerId::ProjectileToAreaExplosion:
        ScheduleAction({tick + skill_release_ticks, ScheduledKind::Projectile, skill,
                  actors->player.position, actors->player.aim,
                  definition.damage_coefficient,
                  0.0f, 0.0f, 1, mask,
                  EffectOrigin::Original, cast_id});
        break;
    case AbilityHandlerId::NearestUnhitTargetRicochet:
        ScheduleAction({tick + skill_release_ticks, ScheduledKind::Projectile, skill,
                  actors->player.position, actors->player.aim,
                  definition.damage_coefficient,
                  0.0f, 0.0f, 1, mask,
                  EffectOrigin::Original, cast_id});
        break;
    case AbilityHandlerId::TargetedPeriodicArea:
    {
        const auto &arrow_rain = rules.upgrades.arrow_rain;
        auto *area = SpawnArea(AreaKind::Damage, skill, target, definition.area_radius,
                  definition.damage_coefficient,
                  HasUpgrade(mask, 8)
                      ? static_cast<float>(arrow_rain.extend_area_and_leave_slow.area_duration_ticks) / 60.0f
                      : static_cast<float>(definition.duration_ticks) / 60.0f,
                  static_cast<float>(definition.activation_delay_ticks) / 60.0f,
                  EffectOrigin::Original, cast_id, mask,
                  HasUpgrade(mask, 6) ? arrow_rain.area_slow.slow_fraction : 0.0f,
                  HasUpgrade(mask, 6)
                      ? static_cast<float>(arrow_rain.area_slow.slow_duration_ticks) / 60.0f
                      : 0.0f);
        area->interval = definition.tick_interval_ticks;
        EmitVfx(DomainSignalKind::ArrowRainCast, target, actors->player.aim, 1.0f, 0.1f);
        if (HasUpgrade(mask, 1))
        {
            const auto &secondary = arrow_rain.delayed_forward_secondary_area;
            ScheduleAction({tick + secondary.delay_ticks, ScheduledKind::Area, skill,
                      Add(target, Multiply(actors->player.aim, secondary.forward_offset)), actors->player.aim,
                      secondary.damage_multiplier_per_tick, secondary.radius,
                      static_cast<float>(secondary.duration_ticks) / 60.0f, 1, 0,
                      EffectOrigin::Derived, cast_id, 0});
        }
        break;
    }
    case AbilityHandlerId::ForwardRollLeaveTrap:
    {
        const auto &trap = rules.upgrades.trap;
        const auto &trap_definition = definition;
        const auto origin = actors->player.position;
        if (HasUpgrade(mask, 1))
        {
            const auto &roll = trap.roll_path_traps;
            bool cast_cue_emitted = false;
            for (std::uint32_t index = 0; index < roll.trap_count; ++index)
            {
                auto *area = SpawnArea(AreaKind::Trap, skill,
                          Add(origin, Multiply(actors->player.aim,
                               roll.spacing * static_cast<float>(index))),
                          trap_definition.detection_radius,
                          roll.damage_multiplier_per_trap,
                          static_cast<float>(trap_definition.active_duration_ticks) / 60.0f,
                          static_cast<float>(trap_definition.activation_delay_ticks) / 60.0f,
                          EffectOrigin::Original, cast_id, mask,
                          trap_definition.slow_fraction,
                          static_cast<float>(trap_definition.slow_duration_ticks) / 60.0f,
                          trap_definition.area_radius,
                          false, 0);
                if (area)
                {
                    if (!cast_cue_emitted)
                    {
                        EmitVfx(DomainSignalKind::TrapCast, area->position,
                                actors->player.aim, 0.8f, 1.05f);
                        cast_cue_emitted = true;
                        auto &signal = combat_state->domain_signals.back();
                        signal.upgrade_skill = static_cast<std::uint8_t>(skill);
                        signal.upgrade_index = 0;
                        signal.upgrade_stage = UpgradeVisualStage::Spawn;
                        signal.upgrade_cast_id = area->cast_id;
                        signal.upgrade_owner_id = area->id.value;
                    }
                    else
                    {
                        EmitUpgradeVisual(skill, 1, UpgradeVisualStage::Spawn,
                                          area->position, actors->player.aim,
                                          area->cast_id, area->id.value);
                        combat_state->domain_signals.back().scale = 0.8f;
                    }
                }
            }
        }
        else
        {
            SpawnArea(AreaKind::Trap, skill, origin, trap_definition.detection_radius,
                      definition.damage_coefficient,
                      static_cast<float>(trap_definition.active_duration_ticks) / 60.0f,
                      static_cast<float>(trap_definition.activation_delay_ticks) / 60.0f,
                      EffectOrigin::Original, cast_id, mask,
                      trap_definition.slow_fraction,
                      static_cast<float>(trap_definition.slow_duration_ticks) / 60.0f,
                      trap_definition.area_radius);
            EmitVfx(DomainSignalKind::TrapCast, origin);
        }
        actors->player.retreat_until = tick + trap_definition.forward_roll_duration_ticks + 1;
        actors->player.retreat_velocity = Multiply(
            actors->player.aim, trap_definition.forward_roll_distance /
                            (static_cast<float>(trap_definition.forward_roll_duration_ticks) *
                             kTickSeconds));
        actors->player.retreat_followup_cast = cast_id;
        actors->player.retreat_upgrade_mask = mask;
        actors->player.retreat_landing_pending = true;
        actors->player.forced_move_skill = SkillKind::Trap;
        break;
    }
    case AbilityHandlerId::ForcedRetreatAndProjectile:
    {
        const auto &retreat = rules.upgrades.retreat_shot;
        EmitVfx(DomainSignalKind::RetreatMoved, actors->player.position,
                Multiply(actors->player.aim, -1.0f), 1.0f, 0.1f);
        const auto retreat_release_ticks = AnimationMarkerTicks(
            4, recovery_ticks[skill_index]);
        auto first_arrow = ScheduledAction{tick + retreat_release_ticks, ScheduledKind::Projectile, skill,
                   actors->player.position,
                   HasUpgrade(mask, 1)
                       ? Rotate(actors->player.aim,
                                retreat.replace_with_three_arrows.angles_degrees[0])
                       : actors->player.aim,
                   HasUpgrade(mask, 1)
                       ? retreat.replace_with_three_arrows.damage_multiplier_per_arrow
                       : definition.damage_coefficient,
                   0.0f, 0.0f, 1, mask,
                   EffectOrigin::Original, cast_id,
                   static_cast<std::uint8_t>(HasUpgrade(mask, 1)
                                                 ? 0
                                                 : kNoTelemetrySource)};
        first_arrow.visual_fan_angle = HasUpgrade(mask, 1)
                                           ? retreat.replace_with_three_arrows.angles_degrees[0]
                                           : 0.0f;
        ScheduleAction(first_arrow);
        if (HasUpgrade(mask, 1))
        {
            const auto count = retreat.replace_with_three_arrows.projectile_count;
            for (std::uint32_t index = 1; index < count; ++index)
            {
                const auto angle = retreat.replace_with_three_arrows.angles_degrees[index];
                auto extra_arrow = ScheduledAction{tick + retreat_release_ticks,
                          ScheduledKind::Projectile, skill, actors->player.position,
                          Rotate(actors->player.aim, angle),
                          retreat.replace_with_three_arrows.damage_multiplier_per_arrow,
                          0.0f, 0.0f, 1,
                           mask, EffectOrigin::Original, cast_id, 0};
                extra_arrow.visual_fan_angle = angle;
                ScheduleAction(extra_arrow);
            }
        }
        actors->player.retreat_until = tick + definition.forced_move_duration_ticks + 1;
        actors->player.retreat_velocity = Multiply(
            actors->player.aim, -definition.forced_move_distance /
                            (static_cast<float>(definition.forced_move_duration_ticks) *
                             kTickSeconds));
        actors->player.retreat_followup_cast = cast_id;
        actors->player.retreat_upgrade_mask = mask;
        actors->player.retreat_landing_pending = true;
        actors->player.forced_move_skill = SkillKind::RetreatShot;
        if (HasUpgrade(mask, 2))
        {
            const auto &small_trap = retreat.small_trap_at_start;
            SpawnArea(AreaKind::Trap, skill, actors->player.position,
                      small_trap.detection_radius,
                      small_trap.damage_multiplier,
                       static_cast<float>(small_trap.active_duration_ticks) / 60.0f,
                       static_cast<float>(small_trap.activation_delay_ticks) / 60.0f,
                      EffectOrigin::Derived, cast_id, 0, 0.0f, 0.0f,
                       small_trap.explosion_radius,
                      false, 1);
        }
        if (HasUpgrade(mask, 3))
        {
            const auto &slow = retreat.slow_trail;
            const auto trail_radius = slow.radius;
            const auto trail = SpawnArea(AreaKind::Slow, skill,
                       Add(actors->player.position, Multiply(actors->player.retreat_velocity,
                           static_cast<float>(definition.forced_move_duration_ticks) *
                           kTickSeconds * 0.5f)), trail_radius,
                       0.0f, static_cast<float>(slow.duration_ticks) / 60.0f, 0.0f,
                      EffectOrigin::Derived, cast_id, 0, slow.slow_fraction,
                       static_cast<float>(slow.slow_duration_ticks) / 60.0f,
                       0.0f, false, 2);
            if (trail)
            {
                trail->direction = Normalize(actors->player.retreat_velocity);
                trail->half_length = definition.forced_move_distance * 0.5f;
            }
        }
        if (HasUpgrade(mask, 6))
        {
            actors->player.next_active_refund_until =
                tick + retreat.next_other_active_cooldown_refund.activation_window_ticks;
            actors->player.next_active_refund_source = skill;
        }
        if (HasUpgrade(mask, 8))
        {
            actors->player.retreat_followup_tick =
                tick + retreat.delayed_second_retreat_and_arrow.delay_after_first_move_ticks;
            actors->player.retreat_followup_direction = actors->player.aim;
        }
        break;
    }
    case AbilityHandlerId::BasicProjectileCadence:
    case AbilityHandlerId::HoldReleaseLinearCharge:
        return false;
    }

    actors->player.active_basic_empower_until =
        tick + rules.upgrades.basic_attack.post_active_three_arrow.activation_window_ticks;
    if (actors->player.next_active_refund_until >= tick &&
        actors->player.next_active_refund_source != SkillKind::Count &&
        actors->player.next_active_refund_source != skill)
    {
        const auto before = actors->player.cooldowns[cooldown_index];
        actors->player.cooldowns[cooldown_index] -= static_cast<Tick>(
            actors->player.cooldowns[cooldown_index] *
            rules.upgrades.retreat_shot.next_other_active_cooldown_refund
                .cooldown_refund_fraction);
        const auto saved = before - actors->player.cooldowns[cooldown_index];
        RecordUpgradeEffect(actors->player.next_active_refund_source, 5,
                            UpgradeEffectMetric::CooldownTicksSaved,
                            saved);
        if (saved > 0)
        {
            EmitUpgradeVisual(actors->player.next_active_refund_source, 6,
                              UpgradeVisualStage::Resolve, actors->player.position,
                              actors->player.aim, cast_id, kPlayerRenderId);
        }
        actors->player.next_active_refund_until = 0;
        actors->player.next_active_refund_source = SkillKind::Count;
    }
    DispatchAbilityRules(skill, cooldown_index);
    actors->player.last_active = skill;
    actors->player.last_active_tick = tick;
    EmitSignal(DomainSignalKind::AbilityUsed, actors->player.position,
               static_cast<std::uint8_t>(skill));
    return true;
}

bool GameSimulation::SimulationWorld::TryBeginSkill(SkillKind skill)
{
    if (rules.skills[static_cast<std::size_t>(skill)].handler ==
        AbilityHandlerId::HoldReleaseLinearCharge)
    {
        const auto cooldown = static_cast<std::size_t>(skill) - 1;
        if (actors->player.cooldowns[cooldown] != 0 || actors->player.charging ||
            actors->player.active_cast_tick > tick || tick < actors->player.retreat_until)
        {
            return false;
        }
        if (actors->player.basic_attack_cast_id != 0 &&
            tick < actors->player.basic_attack_release_tick)
        {
            const auto basic_cast = actors->player.basic_attack_cast_id;
            std::erase_if(combat_state->scheduled_actions, [basic_cast](const auto &action) {
                return action.cast_id == basic_cast;
            });
            if (combat_state->scheduled_actions.empty())
                combat_state->next_scheduled_sequence = 0;
            actors->player.basic_attack_cast_id = 0;
            actors->player.basic_attack_release_tick = 0;
        }
        actors->player.basic_attack_animation_until = tick;
        actors->player.charging = true;
        actors->player.charging_skill = skill;
        actors->player.charge_start = tick;
        actors->player.charge_cast_id = HasUpgrade(
            actors->player.upgrades[static_cast<std::size_t>(skill)], 2)
            ? TakeSimulationIdentity(next_cast_id) : 0;
        EmitSignal(DomainSignalKind::ChargedShotStarted, actors->player.position,
                   static_cast<std::uint8_t>(skill));
        return true;
    }
    return CastSkill(skill);
}

void GameSimulation::SimulationWorld::ConsumeBufferedSkill()
{
    if (session_phase != SessionPhase::Playing ||
        actors->player.buffered_skill == SkillKind::Count)
    {
        return;
    }
    if (tick > actors->player.buffered_skill_expires)
    {
        actors->player.buffered_skill = SkillKind::Count;
        actors->player.buffered_skill_expires = 0;
        actors->player.buffered_skill_slot = 0xFF;
        return;
    }
    const auto buffered = actors->player.buffered_skill;
    const auto buffered_slot = actors->player.buffered_skill_slot;
    if (TryBeginSkill(buffered))
    {
        if (buffered == SkillKind::ChargedShot)
            actors->player.charging_slot = buffered_slot;
        actors->player.buffered_skill = SkillKind::Count;
        actors->player.buffered_skill_expires = 0;
        actors->player.buffered_skill_slot = 0xFF;
    }
}

void GameSimulation::SimulationWorld::ReleaseChargedShot()
{
    const auto mask = actors->player.upgrades[static_cast<std::size_t>(SkillKind::ChargedShot)];
    const auto &definition = rules.skills[static_cast<std::size_t>(SkillKind::ChargedShot)];
    const auto &upgrades = rules.upgrades.charged_shot;
    const auto maximum_ticks = HasUpgrade(mask, 1)
                                   ? upgrades.extended_full_charge_explosion.maximum_charge_time_ticks
                                   : definition.maximum_charge_time_ticks;
    auto elapsed = std::min(tick - actors->player.charge_start, maximum_ticks);
    if (HasUpgrade(mask, 2))
    {
        const auto unaccelerated = elapsed;
        elapsed = std::min(
            maximum_ticks,
            static_cast<Tick>(elapsed / upgrades.faster_charge.charge_time_multiplier));
        RecordUpgradeEffect(SkillKind::ChargedShot, 1,
                            UpgradeEffectMetric::ChargeTicksSaved,
                            elapsed - unaccelerated);
    }
    const auto ratio = static_cast<float>(elapsed) / static_cast<float>(maximum_ticks);
    const auto minimum = HasUpgrade(mask, 3)
                             ? upgrades.increase_minimum_charge_damage.minimum_damage_multiplier
                             : definition.minimum_damage_multiplier;
    const auto maximum = HasUpgrade(mask, 1)
                             ? upgrades.extended_full_charge_explosion.maximum_damage_multiplier
                             : definition.maximum_damage_multiplier;
    const auto coefficient = std::lerp(minimum, maximum, ratio);
    const auto cast_id = actors->player.charge_cast_id != 0
        ? actors->player.charge_cast_id : TakeSimulationIdentity(next_cast_id);
    actors->player.facing = actors->player.aim;
    auto &runtime = FindOrCreateCastRuntime(cast_id, SkillKind::ChargedShot);
    runtime.upgrade_mask = mask;
    runtime.full_charge = ratio >= 0.999f;
    if (auto projectile = FireProjectile(SkillKind::ChargedShot, actors->player.position,
                                          actors->player.aim, coefficient,
                                          EffectOrigin::Original, cast_id, mask))
    {
        projectile->motion.remaining_range = std::lerp(
            definition.minimum_range, definition.maximum_range,
            ratio);
        projectile->motion.radius = std::lerp(definition.minimum_collision_radius,
                                       definition.maximum_collision_radius, ratio);
        if (HasUpgrade(mask, 4))
        {
            projectile->combat.pierce_remaining +=
                static_cast<std::uint16_t>(upgrades.additional_pierce.additional_pierce);
        }
        if (HasUpgrade(mask, 5))
        {
            projectile->combat.bleed_stacks = static_cast<std::uint8_t>(
                upgrades.charge_bleed_full_charge_rupture.bleed_stacks);
        }
        if (HasUpgrade(mask, 1) && ratio >= 0.999f)
        {
            projectile->combat.explosion_radius = upgrades.extended_full_charge_explosion.explosion_radius;
            projectile->combat.explosion_damage = RoundFinalDamage(
                EffectiveAttack() *
                upgrades.extended_full_charge_explosion
                    .full_charge_end_explosion_damage_multiplier, rules);
            projectile->combat.explosion_source_upgrade = 0;
        }
        projectile->combat.full_charge = ratio >= 0.999f;
        projectile->combat.charge_ratio = ratio;
    }
    actors->player.cooldowns[static_cast<std::size_t>(SkillKind::ChargedShot) - 1] =
        EffectiveCooldownTicks(SkillKind::ChargedShot) + 1;
    ++Metrics().skill_uses[static_cast<std::size_t>(SkillKind::ChargedShot)];
    const auto cooldown_reduction =
        rules.stats.allocations[static_cast<std::size_t>(StatKind::CooldownReduction)]
                .amount_per_point *
        actors->player.stats[static_cast<std::size_t>(StatKind::CooldownReduction)];
    if (cooldown_reduction > 0.0f)
    {
        const auto cooldown =
            actors->player.cooldowns[static_cast<std::size_t>(SkillKind::ChargedShot) - 1] - 1;
        const auto without_stat = static_cast<Tick>(std::llround(
            static_cast<double>(cooldown) / (1.0 - cooldown_reduction)));
        Metrics().stat_utility[static_cast<std::size_t>(StatKind::CooldownReduction)] +=
            without_stat - std::min(without_stat, cooldown);
    }
    actors->player.active_animation_start = tick;
    actors->player.basic_attack_animation_until = tick;
    actors->player.active_cast_tick = tick + 9;
    actors->player.active_animation_until = actors->player.active_cast_tick + kAnimationBlendOutTicks;
    CancelChargedShot(true);
    actors->player.active_basic_empower_until =
        tick + rules.upgrades.basic_attack.post_active_three_arrow.activation_window_ticks;
}

void GameSimulation::SimulationWorld::CastAttackPhase()
{
    CastBasicAttack();
    auto boss_actions_timing = Measure(SimulationTimingId::ScheduledBossActions);
    if(combat_state->boss_actions_dirty)
    {
        std::ranges::sort(combat_state->boss_actions,[](const BossAction &a,const BossAction &b) {
            return a.due!=b.due?a.due<b.due:a.schedule_sequence<b.schedule_sequence;
        });
        combat_state->boss_actions_dirty=false;
    }
    std::size_t boss_actions_executed{};
    while (boss_actions_executed < combat_state->boss_actions.size() &&
           combat_state->boss_actions[boss_actions_executed].due <= tick)
    {
        const auto action = combat_state->boss_actions[boss_actions_executed++];
        auto boss = FindEnemy(action.boss_id);
        if (!boss || boss->life.dead) continue;
        boss->action.attack_cast_id = action.cast_id;
        boss->action.boss_action_started = action.animation_started;
        boss->action.boss_action_until = action.due;
        boss->action.boss_action_recoil = action.kind == BossActionKind::Area ||
                                   action.kind == BossActionKind::Shockwave;
        auto direction = action.direction;
        if (LengthSquared(direction) <= 0.0001f)
            direction = Normalize(Subtract(actors->player.position, boss->motion.position));
        if (action.kind == BossActionKind::Dash)
        {
            if (LengthSquared(action.direction) <= 0.0001f)
            {
                DomainSignalGeometry retarget{};
                retarget.source_id = action.cast_id;
                retarget.start_tick = retarget.end_tick = tick;
                EmitVfx(DomainSignalKind::BossDashRetargeted, boss->motion.position,
                        direction, 1.0f, 1.0f, 0, boss->id.value, retarget);
            }
            EmitVfx(DomainSignalKind::BossDashStarted, boss->motion.position,
                    direction, 1.0f, 0.12f);
            boss->motion.velocity = Multiply(direction, action.speed);
            boss->action.dash_started = tick;
            boss->action.dash_origin = boss->motion.position;
            boss->action.dash_until = tick + Seconds(action.distance / action.speed);
            boss->action.dash_damage = action.damage;
            boss->action.dash_hit = false;
        }
        else if (action.kind == BossActionKind::Volley)
        {
            DomainSignalGeometry geometry{};
            geometry.kind = DomainSignalGeometryKind::Cone;
            geometry.range = rules.boss_common.projectile_range;
            geometry.half_angle_degrees = action.arc_degrees * 0.5f;
            geometry.start_tick = tick;
            geometry.end_tick = tick;
            geometry.source_id = action.cast_id;
            EmitVfx(DomainSignalKind::BossVolleyReleased, boss->motion.position,
                    Rotate(direction, action.angle_offset), 1.0f, 1.0f, 0, 0, geometry);
            for (std::uint32_t index = 0; index < action.projectile_count; ++index)
            {
                const auto angle = action.projectile_count == 1 ? action.angle_offset :
                    -action.arc_degrees * 0.5f +
                    action.arc_degrees * index /
                        static_cast<float>(action.projectile_count - 1) +
                    action.angle_offset;
                if (auto shot = FireProjectile(
                        SkillKind::BasicAttack, boss->motion.position,
                         Rotate(direction, angle), static_cast<float>(action.damage),
                         EffectOrigin::Original, action.cast_id, 0, false,
                         kNoTelemetrySource, kNoTelemetrySource,
                         static_cast<std::uint8_t>(EnemyTelemetryIndex(*boss))))
                {
                    shot->motion.velocity = Multiply(Normalize(shot->motion.velocity), action.speed);
                    shot->motion.remaining_range = rules.boss_common.projectile_range;
                }
            }
        }
        else if (action.kind == BossActionKind::Area)
        {
            DomainSignalGeometry geometry{};
            geometry.kind = DomainSignalGeometryKind::Circle;
            geometry.radius = action.radius;
            geometry.start_tick = tick;
            geometry.end_tick = tick + Seconds(action.duration);
            geometry.source_id = action.cast_id;
            EmitVfx(DomainSignalKind::BossAreaActivated, action.position, {},
                    action.radius / 2.2f, 0.12f, 0, 0, geometry);
            SpawnArea(AreaKind::EnemyDamage, SkillKind::Count, action.position,
                      action.radius,
                      static_cast<float>(action.damage) /
                          std::max(EffectiveAttack(), 1.0f),
                       action.duration, 0.0f, EffectOrigin::Original,
                       action.cast_id, 0, 0.0f, 0.0f, 0.0f, false,
                       kNoTelemetrySource, kNoTelemetrySource,
                       static_cast<std::uint8_t>(EnemyTelemetryIndex(*boss)));
        }
        else
        {
            DomainSignalGeometry geometry{};
            geometry.kind = DomainSignalGeometryKind::RingGaps;
            geometry.inner_radius = std::max(0.0f, action.distance - action.half_width);
            geometry.outer_radius = action.distance + action.half_width;
            geometry.gap_count = action.safe_gap_count;
            geometry.gap_offset_degrees = static_cast<float>(action.cast_id % 360);
            geometry.gap_half_width_degrees = action.safe_gap_degrees * 0.5f;
            geometry.start_tick = tick;
            geometry.end_tick = tick + Seconds(action.duration);
            geometry.source_id = action.cast_id;
            EmitVfx(DomainSignalKind::BossShockwaveReleased, action.position, {},
                    action.radius / 9.75f, 0.15f, 0, 0, geometry);
            auto *area = SpawnArea(
                AreaKind::EnemyDamage, SkillKind::Count, action.position,
                action.radius,
                static_cast<float>(action.damage) /
                    std::max(EffectiveAttack(), 1.0f),
                action.duration, 0.0f, EffectOrigin::Original,
                action.cast_id, 0, 0.0f, 0.0f, 0.0f, false,
                kNoTelemetrySource, kNoTelemetrySource,
                static_cast<std::uint8_t>(EnemyTelemetryIndex(*boss)));
            area->ring_inner_radius = action.distance;
            area->ring_outer_radius = action.radius;
            area->ring_half_width = action.half_width;
            area->safe_gap_count = action.safe_gap_count;
            area->safe_gap_degrees = action.safe_gap_degrees;
            area->interval = std::max<Tick>(1, action.interval);
        }
    }
    combat_state->boss_actions.erase(combat_state->boss_actions.begin(),
                       combat_state->boss_actions.begin() + boss_actions_executed);
    if(combat_state->boss_actions.empty()) combat_state->next_boss_schedule_sequence=0;
    boss_actions_timing.Finish();
    auto scheduled_actions_timing = Measure(SimulationTimingId::ScheduledActions);
    if(combat_state->scheduled_actions_dirty)
    {
        std::ranges::sort(combat_state->scheduled_actions,[](const ScheduledAction &a,const ScheduledAction &b) {
            return a.due!=b.due?a.due<b.due:a.schedule_sequence<b.schedule_sequence;
        });
        combat_state->scheduled_actions_dirty=false;
    }
    std::size_t executed{};
    std::set<std::tuple<std::uint64_t, SkillKind, std::uint8_t>> emitted_fan_casts;
    while (executed < combat_state->scheduled_actions.size() && combat_state->scheduled_actions[executed].due <= tick)
    {
        const auto action = combat_state->scheduled_actions[executed++];
        if (action.kind == ScheduledKind::Projectile)
        {
                auto projectile = FireProjectile(
                    action.skill, action.position, action.direction,
                    action.damage_coefficient, action.origin, action.cast_id,
                    action.upgrade_mask, true,
                    action.source_upgrade, action.source_relic);
            const auto rear_fan = action.skill == SkillKind::MultiShot &&
                                  action.origin == EffectOrigin::Derived &&
                                  action.source_upgrade == 3;
            const auto triple_release = action.skill == SkillKind::RetreatShot &&
                                        action.origin == EffectOrigin::Original &&
                                        action.source_upgrade == 0 &&
                                        HasUpgrade(action.upgrade_mask, 1);
            const auto outer_fan = action.skill == SkillKind::MultiShot &&
                                   action.origin == EffectOrigin::Original &&
                                   action.source_upgrade == 6;
            const auto basic_triple = action.skill == SkillKind::BasicAttack &&
                                      action.origin == EffectOrigin::Original &&
                                      HasUpgrade(action.upgrade_mask, 8) &&
                                      action.visual_owner_id == action.cast_id;
            if (projectile && (rear_fan || triple_release || outer_fan || basic_triple) &&
                emitted_fan_casts.emplace(action.cast_id, action.skill,
                                          basic_triple ? std::uint8_t{7}
                                                       : action.source_upgrade).second)
            {
                DomainSignalGeometry geometry{};
                geometry.kind = DomainSignalGeometryKind::Cone;
                geometry.range = rules.skills[static_cast<std::size_t>(action.skill)].range;
                if (rear_fan)
                    geometry.half_angle_degrees =
                        std::abs(rules.upgrades.multishot.rear_fan.fan_angle_degrees) * 0.5f;
                else if (triple_release)
                    for (const auto angle :
                         rules.upgrades.retreat_shot.replace_with_three_arrows.angles_degrees)
                        geometry.half_angle_degrees =
                            std::max(geometry.half_angle_degrees, std::abs(angle));
                else if (basic_triple)
                    for (const auto angle :
                         rules.upgrades.basic_attack.post_active_three_arrow.angles_degrees)
                        geometry.half_angle_degrees =
                            std::max(geometry.half_angle_degrees, std::abs(angle));
                else
                {
                    const auto &outer = rules.upgrades.multishot.two_additional_outer_arrows;
                    for (std::uint32_t index = 0; index < outer.additional_projectiles; ++index)
                        geometry.half_angle_degrees =
                            std::max(geometry.half_angle_degrees,
                                     std::abs(outer.outer_angles_degrees[index]));
                }
                geometry.start_tick = tick;
                geometry.end_tick = tick;
                geometry.source_id = projectile->id.value;
                EmitUpgradeVisual(action.skill,
                                  rear_fan ? 4 : triple_release ? 1 : outer_fan ? 7 : 8,
                                  UpgradeVisualStage::Spawn, projectile->motion.position,
                                  Rotate(action.direction, -action.visual_fan_angle),
                                  projectile->combat.cast_id, projectile->id.value, geometry);
            }
            if (projectile && action.skill == SkillKind::BasicAttack &&
                action.origin == EffectOrigin::Original)
            {
                if (action.source_upgrade == 0 && !HasUpgrade(action.upgrade_mask, 1))
                    EmitUpgradeVisual(SkillKind::BasicAttack, 1,
                                      UpgradeVisualStage::Spawn, projectile->motion.position,
                                      Normalize(projectile->motion.velocity), projectile->combat.cast_id,
                                      projectile->id.value);
                if (action.source_upgrade == 0)
                {
                    ++actors->player.basic_sequence;
                    ++Metrics().skill_uses[
                        static_cast<std::size_t>(SkillKind::BasicAttack)];
                }
                ++actors->player.basic_arrow_sequence;
                if (HasUpgrade(action.upgrade_mask, 4) &&
                    actors->player.basic_arrow_sequence %
                            rules.upgrades.basic_attack.direct_arrow_bleed.direct_arrow_interval ==
                        0)
                {
                    projectile->combat.bleed_stacks = static_cast<std::uint8_t>(
                        rules.upgrades.basic_attack.direct_arrow_bleed.bleed_stacks);
                }
                if (HasUpgrade(action.upgrade_mask, 5) &&
                    actors->player.basic_arrow_sequence %
                            rules.upgrades.basic_attack.direct_arrow_burn.direct_arrow_interval ==
                        0)
                {
                    projectile->combat.burn = true;
                }
            }
            if (projectile && action.skill == SkillKind::PiercingShot &&
                action.source_upgrade == 0)
            {
                if (const auto target = NearestEnemy(
                        action.position,
                        rules.skills[static_cast<std::size_t>(SkillKind::PiercingShot)].range))
                {
                    projectile->combat.homing = true;
                    projectile->combat.homing_target = target->id.value;
                }
                if (action.origin == EffectOrigin::Derived)
                    EmitUpgradeVisual(SkillKind::PiercingShot, 1,
                                      UpgradeVisualStage::Spawn,
                                      projectile->motion.position,
                                      Normalize(projectile->motion.velocity),
                                      projectile->combat.cast_id, projectile->id.value);
            }
            if (projectile && action.skill == SkillKind::RicochetArrow &&
                action.origin == EffectOrigin::Original)
            {
                projectile->combat.homing = true;
                if (const auto target = NearestEnemy(
                        action.position,
                        rules.skills[static_cast<std::size_t>(SkillKind::RicochetArrow)].initial_range))
                    projectile->combat.homing_target = target->id.value;
            }
        }
        else if (action.kind == ScheduledKind::Volley)
        {
            if (action.skill == SkillKind::MultiShot &&
                action.origin == EffectOrigin::Original)
            {
                const auto &skill_rules = rules.skills[static_cast<std::size_t>(
                    SkillKind::MultiShot)];
                auto effective_half_angle = std::abs(skill_rules.fan_angle_degrees) * 0.5f;
                if (HasUpgrade(action.upgrade_mask, 7))
                {
                    const auto &outer = rules.upgrades.multishot.two_additional_outer_arrows;
                    for (std::uint32_t index = 0; index < outer.additional_projectiles; ++index)
                        effective_half_angle = std::max(
                            effective_half_angle, std::abs(outer.outer_angles_degrees[index]));
                }
                DomainSignalGeometry geometry{};
                geometry.kind = DomainSignalGeometryKind::Cone;
                geometry.range = skill_rules.range;
                geometry.half_angle_degrees = effective_half_angle;
                geometry.start_tick = tick;
                geometry.end_tick = tick;
                geometry.source_id = action.cast_id;
                EmitSignal(DomainSignalKind::ArrowReleased, action.position,
                           static_cast<std::uint8_t>(action.skill));
                EmitVfx(DomainSignalKind::MultiShotCast, action.position,
                        action.direction, 1.0f, 1.1f, 0, 0, geometry);
            }
            const auto fan_angle = action.skill == SkillKind::MultiShot
                                       ? rules.skills[static_cast<std::size_t>(
                                             SkillKind::MultiShot)].fan_angle_degrees
                                       : 0.0f;
            const auto fan_step = action.projectile_count > 1
                                       ? fan_angle /
                                             static_cast<float>(action.projectile_count - 1)
                                       : 0.0f;
            std::uint64_t visual_owner_id{};
            bool spawned_projectile{};
            for (std::uint32_t index = 0; index < action.projectile_count; ++index)
            {
                if (auto projectile = FireProjectile(
                        action.skill, action.position,
                        Rotate(action.direction,
                               -fan_angle * 0.5f + fan_step * index),
                        action.damage_coefficient, action.origin, action.cast_id,
                        action.upgrade_mask, true, action.source_upgrade,
                        action.source_relic))
                {
                    if (!spawned_projectile)
                    {
                        visual_owner_id = projectile->id.value;
                        spawned_projectile = true;
                    }
                }
            }
            if (spawned_projectile && action.skill == SkillKind::MultiShot &&
                action.origin == EffectOrigin::Derived && action.source_upgrade == 0)
            {
                const auto &skill_rules = rules.skills[static_cast<std::size_t>(
                    SkillKind::MultiShot)];
                const auto &second_fan = rules.upgrades.multishot.delayed_second_fan;
                DomainSignalGeometry geometry{};
                geometry.kind = DomainSignalGeometryKind::Cone;
                geometry.range = skill_rules.range;
                geometry.half_angle_degrees = std::abs(second_fan.fan_angle_degrees) * 0.5f;
                geometry.start_tick = tick;
                geometry.end_tick = tick;
                geometry.source_id = visual_owner_id;
                EmitUpgradeVisual(SkillKind::MultiShot, 1, UpgradeVisualStage::Spawn,
                                  action.position, action.direction, action.cast_id,
                                  visual_owner_id, geometry);
            }
        }
        else if (action.kind == ScheduledKind::Explosion)
        {
            RecordUpgradeEffect(action.skill, action.source_upgrade,
                                UpgradeEffectMetric::ExplosionsCreated);
            QueueAreaDamage(action.position, action.radius,
                       RoundFinalDamage(EffectiveAttack() * action.damage_coefficient, rules),
                       action.skill, action.origin, action.cast_id, 0, false, 0.0f, 0,
                       action.source_upgrade, action.source_relic);
            if (action.skill == SkillKind::ExplosiveArrow && action.source_upgrade == 0)
                EmitVfx(DomainSignalKind::ExplosiveArrowSecondary,
                        action.position, {}, action.radius / 3.0f, 0.15f, 0, 0, ExplosionGeometry(action.radius));
            else
                EmitVfx(DomainSignalKind::SmallExplosion,
                        action.position, {}, action.radius, 0.15f, 0, 0, ExplosionGeometry(action.radius));
            if (action.skill == SkillKind::ExplosiveArrow && action.source_upgrade <= 1)
            {
                auto &signal = combat_state->domain_signals.back();
                signal.upgrade_skill = static_cast<std::uint8_t>(action.skill);
                signal.upgrade_index = action.source_upgrade; signal.upgrade_stage = UpgradeVisualStage::Resolve;
                signal.upgrade_cast_id = action.cast_id;
                signal.upgrade_owner_id = action.visual_owner_id != 0 ? action.visual_owner_id : signal.sequence;
                signal.geometry.source_id = signal.upgrade_owner_id;
                signal.geometry.start_tick = signal.geometry.end_tick = tick;
            }
        }
        else
        {
            auto *area = SpawnArea(AreaKind::Damage, action.skill, action.position,
                                   action.radius, action.damage_coefficient,
                                   action.duration, 0.0f, action.origin,
                                   action.cast_id, action.upgrade_mask,
                                   0.0f, 0.0f, 0.0f, false,
                                   action.source_upgrade, action.source_relic);
            if (action.skill == SkillKind::ArrowRain)
                area->interval = rules.skills[static_cast<std::size_t>(
                    SkillKind::ArrowRain)].tick_interval_ticks;
            else if (action.skill == SkillKind::ExplosiveArrow &&
                     action.source_upgrade == 3)
                area->interval = rules.upgrades.explosive_arrow
                                     .explosion_leaves_burning_area.tick_interval_ticks;
            else if (action.skill == SkillKind::Trap && action.source_upgrade == 4)
                area->interval = rules.upgrades.trap.trigger_burn_and_fire_area
                                     .tick_interval_ticks;
        }
    }
    combat_state->scheduled_actions.erase(combat_state->scheduled_actions.begin(), combat_state->scheduled_actions.begin() + executed);
    if(combat_state->scheduled_actions.empty()) combat_state->next_scheduled_sequence=0;
}

void GameSimulation::SimulationWorld::MovementPhase()
{
    auto player_timing = Measure(SimulationTimingId::PlayerMovement);
    actors->player.previous_position = actors->player.position;
    actors->player.tick_motion.Reset(actors->player.position);
    const auto stationary_attack = current_input.held.basic_attack_held ||
                                   tick < actors->player.active_cast_tick;
    if (actors->player.retreat_landing_pending && tick >= actors->player.retreat_until)
    {
        if (actors->player.forced_move_skill == SkillKind::RetreatShot)
            EmitVfx(DomainSignalKind::RetreatLanded, actors->player.position,
                    actors->player.facing,
                    HasUpgrade(actors->player.retreat_upgrade_mask, 5) ? 5.0f : 1.0f,
                    0.025f);
        if (actors->player.forced_move_skill == SkillKind::Trap &&
            HasUpgrade(actors->player.retreat_upgrade_mask, 2))
        {
            const auto &landing = rules.upgrades.trap.landing_slow_area;
            SpawnArea(AreaKind::Slow, SkillKind::Trap, actors->player.position,
                      landing.radius, 0.0f,
                      static_cast<float>(landing.duration_ticks) / 60.0f, 0.0f,
                      EffectOrigin::Derived,
                      actors->player.retreat_followup_cast, 0, landing.slow_fraction,
                      static_cast<float>(landing.duration_ticks) / 60.0f, 0.0f,
                      false, 1);
        }
        else if (actors->player.forced_move_skill == SkillKind::RetreatShot &&
                 HasUpgrade(actors->player.retreat_upgrade_mask, 5))
        {
            const auto &landing = rules.upgrades.retreat_shot.landing_damage_and_push;
            EmitVfx(DomainSignalKind::Push, actors->player.position, {}, 5.0f, 0.12f);
            DomainSignalGeometry geometry{};
            geometry.kind = DomainSignalGeometryKind::Circle;
            geometry.radius = landing.radius;
            geometry.source_id = kPlayerRenderId;
            EmitUpgradeVisual(SkillKind::RetreatShot, 5,
                              UpgradeVisualStage::Resolve,
                              actors->player.position, actors->player.facing,
                              actors->player.retreat_followup_cast,
                              kPlayerRenderId, geometry);
            QueueAreaDamage(actors->player.position, landing.radius,
                         RoundFinalDamage(EffectiveAttack() * landing.damage_multiplier, rules),
                         SkillKind::RetreatShot, EffectOrigin::Derived,
                        actors->player.retreat_followup_cast, 0, false, 0.0f, 0, 4);
            for (auto enemy : actors->enemies)
            {
                if (!enemy.life.dead && !enemy.life.boss &&
                    DistanceSquared(actors->player.position, enemy.motion.position) <=
                        landing.radius * landing.radius)
                {
                    const auto before = enemy.motion.position;
                    const auto displacement = Multiply(
                        Normalize(Subtract(enemy.motion.position, actors->player.position)),
                        landing.push_distance);
                    QueueEnemyDisplacement(enemy, displacement);
                    RecordUpgradeDisplacement(SkillKind::RetreatShot, 4, before,
                                              Add(before, displacement));
                }
            }
        }
        actors->player.retreat_landing_pending = false;
        actors->player.forced_move_skill = SkillKind::Count;
    }
    const auto second_retreat = actors->player.retreat_followup_tick != 0 &&
                                tick >= actors->player.retreat_followup_tick;
    if (second_retreat)
    {
        EmitVfx(DomainSignalKind::RetreatMoved, actors->player.position,
                Multiply(actors->player.retreat_followup_direction, -1.0f), 1.0f, 0.1f);
        const auto before = actors->player.position;
        if (!config.scenario.player_stationary)
        {
            actors->player.position = MoveDisc2D(actors->player.position,
                                  Multiply(actors->player.retreat_followup_direction,
                                           -rules.upgrades.retreat_shot
                                                .delayed_second_retreat_and_arrow
                                                .additional_move_distance),
                                  rules.stats.player_collision_radius,
                                  rules.arena_boundary, ArenaObstacles(), &actors->player.tick_motion);
            actors->player.position = ClampToArena(rules.arena_boundary, actors->player.position,
                                                   rules.stats.player_collision_radius);
        }
        RecordUpgradeDisplacement(SkillKind::RetreatShot, 7, before,
                                  actors->player.position);
        const auto actual_second_move = Subtract(actors->player.position, before);
        if (LengthSquared(actual_second_move) > 0.0f)
        {
            DomainSignalGeometry geometry{};
            geometry.kind = DomainSignalGeometryKind::Line;
            geometry.width = rules.stats.player_collision_radius * 2.0f;
            geometry.source_id = kPlayerRenderId;
            geometry.start_tick = geometry.end_tick = tick;
            geometry.end_position = {actors->player.position.x, 1.05f,
                                     actors->player.position.y};
            EmitUpgradeVisual(SkillKind::RetreatShot, 8,
                              UpgradeVisualStage::Spawn, before,
                              Normalize(actual_second_move),
                              actors->player.retreat_followup_cast,
                              kPlayerRenderId, geometry);
        }
        auto direction = actors->player.retreat_followup_direction;
        auto target = NearestEnemy(actors->player.position,
                                    rules.skills[static_cast<std::size_t>(
                                        SkillKind::RetreatShot)].range);
        if (target)
            direction = Normalize(Subtract(target->motion.position, actors->player.position));
        if (auto arrow = FireProjectile(
                SkillKind::RetreatShot, actors->player.position, direction,
                rules.upgrades.retreat_shot.delayed_second_retreat_and_arrow
                    .damage_multiplier,
                EffectOrigin::Derived, actors->player.retreat_followup_cast, 0, true, 7);
            arrow && target)
        {
            arrow->combat.homing = true;
            arrow->combat.homing_target = target->id.value;
        }
        actors->player.retreat_followup_tick = 0;
    }
    // The follow-up owns this tick's movement, including the position used to fire.
    if (!second_retreat && tick < actors->player.retreat_until && !config.scenario.player_stationary)
    {
        actors->player.position = MoveDisc2D(actors->player.position,
                                     Multiply(actors->player.retreat_velocity, kTickSeconds),
                                     rules.stats.player_collision_radius,
                                     rules.arena_boundary, ArenaObstacles(), &actors->player.tick_motion);
    }
    else if (!second_retreat && actors->player.has_move_target && !stationary_attack)
    {
        const auto direct_path = SegmentClear2D(actors->player.position, actors->player.move_target,
                                                rules.stats.player_collision_radius,
                                                rules.arena_boundary, ArenaObstacles());
        const auto target_cell = navigation->player_navigation.Cell(actors->player.move_target);
        if (!direct_path && target_cell != navigation->player_navigation_target)
        {
            auto navigation_timing = Measure(SimulationTimingId::NavigationQuery);
            navigation->player_navigation.RebuildFlow(actors->player.move_target);
            navigation->player_navigation_target = target_cell;
        }
        const auto waypoint = direct_path
                                  ? actors->player.move_target
                                  : TimedNavigationWaypoint(navigation->player_navigation,actors->player.position,
                                                                   actors->player.move_target);
        const auto delta = Subtract(waypoint, actors->player.position);
        const auto charge_multiplier = actors->player.charging
                                           ? rules.skills[static_cast<std::size_t>(
                                                 SkillKind::ChargedShot)]
                                                 .movement_speed_multiplier_while_charging
                                           : 1.0f;
        const auto step = EffectiveMoveSpeed() * charge_multiplier * kTickSeconds;
        if (LengthSquared(delta) > step * step)
        {
            actors->player.position = MoveDisc2D(actors->player.position,
                                         Multiply(Normalize(delta), step),
                                         rules.stats.player_collision_radius,
                                         rules.arena_boundary, ArenaObstacles(), &actors->player.tick_motion);
            actors->player.facing = Normalize(delta, actors->player.facing);
        }
        else
        {
            actors->player.position = MoveDisc2D(actors->player.position, delta,
                                         rules.stats.player_collision_radius,
                                         rules.arena_boundary, ArenaObstacles(), &actors->player.tick_motion);
            if (LengthSquared(Subtract(actors->player.move_target, actors->player.position)) <= step * step)
                actors->player.has_move_target = false;
        }
    }
    if (stationary_attack)
    {
        actors->player.facing = actors->player.aim;
    }
    actors->player.position = ClampToArena(rules.arena_boundary, actors->player.position,
                                   rules.stats.player_collision_radius);
    actors->player.tick_motion.final_position = actors->player.position;
    actors->player.movement_since_echo += Length(Subtract(actors->player.position,
                                                   actors->player.previous_position));
    const auto moved = Length(Subtract(actors->player.position, actors->player.previous_position));
    const auto move_points = actors->player.stats[static_cast<std::size_t>(StatKind::MoveSpeed)];
    if (move_points != 0 && moved > 0.0f && tick >= actors->player.retreat_until)
    {
        const auto extra = moved *
                           (1.0f - 1.0f /
                                         (1.0f + rules.stats.allocations[
                                             static_cast<std::size_t>(StatKind::MoveSpeed)]
                                             .amount_per_point * move_points));
        Metrics().stat_utility[static_cast<std::size_t>(StatKind::MoveSpeed)] +=
            static_cast<std::uint64_t>(std::llround(extra * 1000.0f));
    }
    const bool forced_motion = tick < actors->player.retreat_until;
    const bool locomotion_moving = !forced_motion && moved > 0.0f;
    if (forced_motion || locomotion_moving)
        actors->player.movement_stopped_tick = 0;
    else if (actors->player.locomotion_moving)
        actors->player.movement_stopped_tick = tick;
    actors->player.locomotion_moving = locomotion_moving;
    const auto locomotion_target =
        LengthSquared(Subtract(actors->player.position, actors->player.previous_position)) > 0.000001f
            ? 1.0f : 0.0f;
    const auto locomotion_step = 1.0f / (Seconds(0.12f));
    actors->player.locomotion_blend += std::clamp(locomotion_target - actors->player.locomotion_blend,
                                           -locomotion_step, locomotion_step);
    actors->player_position_history[tick % actors->player_position_history.size()] =
        actors->player.position;

    player_timing.Finish();
    auto enemies_timing = Measure(SimulationTimingId::EnemyMovement);
    for (auto enemy : actors->enemies)
    {
        if (enemy.life.dead) continue;
        std::erase_if(enemy.status.slows,
                      [this](const SlowEffect &slow) {
                          return slow.expires <= tick;
                      });
        for (const auto &slow : enemy.status.slows)
        {
            RecordUpgradeEffect(slow.source_skill, slow.source_upgrade,
                                UpgradeEffectMetric::SlowActiveTicks);
            if (slow.source_relic < kRelicCount)
                RecordRelicEffect(static_cast<RelicKind>(slow.source_relic),
                                  UpgradeEffectMetric::EffectActiveTicks);
        }
        enemy.motion.previous_position = enemy.motion.position;
        if (enemy.motion.displacement_ticks > 0)
        {
            enemy.motion.velocity = Multiply(enemy.motion.displacement_per_tick, 1.0f / kTickSeconds);
            const auto radius = enemy.life.boss ? rules.boss_common.collision_radius
                                           : rules.enemies[static_cast<std::size_t>(enemy.life.kind)].collision_radius;
            enemy.motion.position = MoveDisc2D(enemy.motion.position, enemy.motion.displacement_per_tick,
                                        radius, rules.arena_boundary, ArenaObstacles(), &enemy.motion.tick_motion);
            if (--enemy.motion.displacement_ticks == 0) enemy.motion.displacement_per_tick = {};
        }
        else
        {
            const auto radius = enemy.life.boss ? rules.boss_common.collision_radius
                                           : rules.enemies[static_cast<std::size_t>(enemy.life.kind)].collision_radius;
            enemy.motion.position = MoveDisc2D(enemy.motion.position,
                                        Multiply(enemy.motion.velocity, kTickSeconds), radius,
                                        rules.arena_boundary, ArenaObstacles(), &enemy.motion.tick_motion);
        }
        const auto enemy_radius = enemy.life.boss ? rules.boss_common.collision_radius
                                             : rules.enemies[static_cast<std::size_t>(enemy.life.kind)].collision_radius;
        enemy.motion.position = ClampToArena(rules.arena_boundary, enemy.motion.position, enemy_radius);
        enemy.motion.tick_motion.final_position = enemy.motion.position;
        if (enemy.life.boss && tick < enemy.action.dash_until && !enemy.action.dash_hit &&
            SegmentCircle(enemy.motion.previous_position, enemy.motion.position,
                          actors->player.position,
                          rules.boss_common.collision_radius +
                              rules.stats.player_collision_radius) &&
            SegmentClear2D(enemy.motion.position, actors->player.position, 0.0f,
                           rules.arena_boundary, ArenaObstacles()))
        {
            QueueDamage(0, enemy.action.dash_damage, SkillKind::Count,
                       EffectOrigin::Original, enemy.action.attack_cast_id, 0, false,
                       0.0f, 0, kNoTelemetrySource, kNoTelemetrySource,
                       static_cast<std::uint8_t>(EnemyTelemetryIndex(enemy)), 0,
                       Normalize(Subtract(actors->player.position, enemy.motion.position)));
            enemy.action.dash_hit = true;
        }
        if (enemy.life.boss && enemy.action.attacking && tick >= enemy.action.attack_resolve)
        {
            const auto contact_radius =
                rules.boss_common.collision_radius + rules.stats.player_collision_radius;
            if (DistanceSquared(enemy.motion.position, actors->player.position) <=
                contact_radius * contact_radius &&
                SegmentClear2D(enemy.motion.position, actors->player.position, 0.0f,
                               rules.arena_boundary, ArenaObstacles()))
            {
                QueueDamage(0, enemy.cold.damage, SkillKind::Count,
                           EffectOrigin::Original, enemy.action.attack_cast_id, 0, false,
                           0.0f, 0, kNoTelemetrySource, kNoTelemetrySource,
                           static_cast<std::uint8_t>(EnemyTelemetryIndex(enemy)), 0,
                           Normalize(Subtract(actors->player.position, enemy.motion.position)));
            }
            enemy.action.attacking = false;
            enemy.motion.velocity = {};
        }
    }
    enemies_timing.Finish();
    auto projectiles_timing = Measure(SimulationTimingId::ProjectileMovement);
    for (auto projectile : actors->projectiles)
    {
        if (projectile.combat.dead) continue;
        projectile.motion.previous_position = projectile.motion.position;
        if (projectile.combat.homing && projectile.combat.homing_target != 0)
        {
            if (const auto target = FindEnemy(projectile.combat.homing_target))
            {
                projectile.motion.velocity = Multiply(
                    Normalize(Subtract(target->motion.position, projectile.motion.position)),
                    Length(projectile.motion.velocity));
            }
        }
        const auto step = Multiply(projectile.motion.velocity, kTickSeconds);
        projectile.motion.position = Add(projectile.motion.position, step);
        projectile.motion.remaining_range -= Length(step);
    }
}

void GameSimulation::SimulationWorld::SpatialGridPhase()
{
    auto build_timing = Measure(SimulationTimingId::SeparationBuild);
    // Keep pair order, but avoid striding through large actor records for every pair.
    auto &positions = combat_state->separation_positions;
    positions.resize(actors->enemies.size());
    for (std::size_t index = 0; index < actors->enemies.size(); ++index)
        positions[index] = actors->enemies[index].motion.position;
    const auto rebuild = [&](bool swept) {
        combat_state->enemy_grid.BeginBuild();
        for (std::size_t index = 0; index < actors->enemies.size(); ++index)
        {
            if (actors->enemies[index].life.dead || (!swept && actors->enemies[index].life.boss)) continue;
            const auto enemy = actors->enemies[index];
            auto minimum = enemy.motion.position, maximum = enemy.motion.position;
            if (swept)
                for (std::uint8_t segment = 0; segment < enemy.motion.tick_motion.count; ++segment)
                    for (const auto point : {enemy.motion.tick_motion.segments[segment].from,
                                             enemy.motion.tick_motion.segments[segment].to})
                    {
                        minimum.x = std::min(minimum.x, point.x); minimum.y = std::min(minimum.y, point.y);
                        maximum.x = std::max(maximum.x, point.x); maximum.y = std::max(maximum.y, point.y);
                    }
            const auto minimum_x = SpatialGridCoordinate(minimum.x, rules.arena_half_extent);
            const auto minimum_y = SpatialGridCoordinate(minimum.y, rules.arena_half_extent);
            const auto maximum_x = SpatialGridCoordinate(maximum.x, rules.arena_half_extent);
            const auto maximum_y = SpatialGridCoordinate(maximum.y, rules.arena_half_extent);
            combat_state->enemy_grid.AddRange(index, minimum_x, maximum_x, minimum_y, maximum_y);
        }
        combat_state->enemy_grid.FinishBuild();
    };
    const auto separate = [this, &positions](std::size_t left, std::size_t right) {
        const auto delta = Subtract(positions[left], positions[right]);
        const auto distance_squared = LengthSquared(delta);
        if (distance_squared >= 0.64f) return;
        const auto direction = distance_squared > 0.0000000001f ? Normalize(delta)
            : Float2{actors->enemies[left].id.value < actors->enemies[right].id.value
                         ? -1.0f : 1.0f, 0.0f};
        const auto push = Multiply(direction,
                                   (0.8f - std::sqrt(distance_squared)) * 0.5f);
        positions[left] = Add(positions[left], push);
        positions[right] = Subtract(positions[right], push);
    };

    rebuild(false);
    build_timing.Finish();
    auto separation_timing = Measure(SimulationTimingId::SeparationSolve);
    constexpr std::array<std::pair<int, int>, 4> kForwardNeighbors{{
        {1, 0}, {-1, 1}, {0, 1}, {1, 1},
    }};
    for (int y = 0; y < kGridDimension; ++y)
    {
        for (int x = 0; x < kGridDimension; ++x)
        {
            const auto &cell = combat_state->enemy_grid[static_cast<std::size_t>(y * kGridDimension + x)];
            for (std::size_t left = 0; left < cell.size(); ++left)
            {
                for (std::size_t right = left + 1; right < cell.size(); ++right)
                {
                    separate(cell[left], cell[right]);
                }
            }
            for (const auto [offset_x, offset_y] : kForwardNeighbors)
            {
                const auto neighbor_x = x + offset_x;
                const auto neighbor_y = y + offset_y;
                if (neighbor_x < 0 || neighbor_x >= kGridDimension ||
                    neighbor_y >= kGridDimension)
                {
                    continue;
                }
                const auto &neighbor = combat_state->enemy_grid[static_cast<std::size_t>(
                    neighbor_y * kGridDimension + neighbor_x)];
                for (const auto left : cell)
                {
                    for (const auto right : neighbor) separate(left, right);
                }
            }
        }
    }
    for (std::size_t index = 0; index < actors->enemies.size(); ++index)
    {
        auto enemy = actors->enemies[index];
        enemy.motion.position = positions[index];
        const auto radius = enemy.life.boss ? rules.boss_common.collision_radius
                                       : rules.enemies[static_cast<std::size_t>(enemy.life.kind)].collision_radius;
        enemy.motion.position = ProjectOutsideObstacles(
            ClampToArena(rules.arena_boundary, enemy.motion.position, radius),
            radius, ArenaObstacles());
        enemy.motion.tick_motion.final_position = enemy.motion.position;
    }
    // Separation uses final centers once; only projectile collision needs swept bounds.
    separation_timing.Finish();
    auto swept_timing = Measure(SimulationTimingId::SweptGridBuild);
    rebuild(true);
    combat_state->enemy_grid.MarkBuilt(tick,actors->enemies.StructuralGeneration());
}

bool GameSimulation::SimulationWorld::AlreadyHit(ConstProjectileRow projectile, std::uint64_t target) const noexcept
{
    const auto hits = std::span(projectile.combat.hit_ids);
    return std::ranges::find(hits, target) != hits.end();
}

void GameSimulation::SimulationWorld::RecordHit(ProjectileRow projectile, std::uint64_t target)
{
    projectile.combat.hit_ids.push_back(target);
    ++projectile.combat.hit_count;
}

std::uint8_t GameSimulation::SimulationWorld::IncrementCastHit(std::uint64_t cast, std::uint64_t target)
{
    const auto key = std::pair{cast, target};
    const auto [found, inserted] = combat_state->cast_hit_index.try_emplace(
        key, combat_state->cast_hits.size());
    if (!inserted)
        return ++combat_state->cast_hits[found->second].count;
    combat_state->cast_hits.push_back({cast, target, 1});
    return 1;
}

std::uint8_t GameSimulation::SimulationWorld::IncrementAreaHit(std::uint64_t area, std::uint64_t target)
{
    const auto key = std::pair{area, target};
    const auto [found, inserted] = combat_state->area_hit_index.try_emplace(
        key, combat_state->area_hits.size());
    if (!inserted)
        return ++combat_state->area_hits[found->second].count;
    combat_state->area_hits.push_back({area, target, 1});
    return 1;
}

CastRuntime &GameSimulation::SimulationWorld::FindOrCreateCastRuntime(std::uint64_t cast, SkillKind skill)
{
    const auto iterator = std::ranges::find(combat_state->cast_runtimes, cast,
                                            &CastRuntime::cast_id);
    if (iterator != combat_state->cast_runtimes.end()) return *iterator;
    combat_state->cast_runtimes.push_back({cast, skill});
    return combat_state->cast_runtimes.back();
}

bool GameSimulation::SimulationWorld::OnProjectileHit(ProjectileRow projectile,
                                                       EnemyRow enemy,
                                                       Float2 contact_position)
{
    RecordHit(projectile, enemy.id.value);
    if (HasRelic(actors->player.relic_mask, RelicKind::ProjectileCadenceReward))
        HandleProjectileCadenceReward();
    const auto cast_hit = IncrementCastHit(projectile.combat.cast_id, enemy.id.value);
    auto &runtime = FindOrCreateCastRuntime(projectile.combat.cast_id, projectile.combat.skill);
    if (!enemy.life.boss) ++runtime.normal_hits;
    auto damage = projectile.combat.damage;
    if (projectile.combat.skill == SkillKind::PiercingShot)
    {
        const auto &piercing = rules.skills[static_cast<std::size_t>(
            SkillKind::PiercingShot)];
        const auto multiplier = std::max(
            piercing.minimum_damage_fraction,
            1.0f - piercing.pierce_damage_decay_fraction *
                       (projectile.combat.hit_count - 1));
        damage = RoundFinalDamage(projectile.combat.damage * multiplier, rules);
    }
    auto bleed = projectile.combat.bleed_stacks;
    auto burn = projectile.combat.burn;
    auto slow = 0.0f;
    auto slow_duration = Tick{};
    auto damage_upgrade = projectile.combat.source_upgrade;
    if (damage_upgrade == kNoTelemetrySource &&
        projectile.combat.skill == SkillKind::BasicAttack &&
        HasUpgrade(projectile.combat.upgrade_mask, 2) && projectile.combat.hit_count > 1)
    {
        damage_upgrade = 1;
        EmitUpgradeVisual(SkillKind::BasicAttack, 2, UpgradeVisualStage::Resolve,
                          contact_position, Normalize(projectile.motion.velocity),
                          projectile.combat.cast_id, projectile.id.value);
    }
    if (damage_upgrade == kNoTelemetrySource &&
        projectile.combat.skill == SkillKind::MultiShot)
    {
        if (HasUpgrade(projectile.combat.upgrade_mask, 3) && projectile.combat.hit_count > 2)
        {
            damage_upgrade = 2;
            EmitUpgradeVisual(SkillKind::MultiShot, 3, UpgradeVisualStage::Resolve,
                              contact_position, Normalize(projectile.motion.velocity),
                              projectile.combat.cast_id, projectile.id.value);
        }
    }
    if (damage_upgrade == kNoTelemetrySource &&
        projectile.combat.skill == SkillKind::ChargedShot &&
        HasUpgrade(projectile.combat.upgrade_mask, 4) && projectile.combat.hit_count > 4)
    {
        damage_upgrade = 3;
        EmitUpgradeVisual(SkillKind::ChargedShot, 4, UpgradeVisualStage::Resolve,
                          contact_position, Normalize(projectile.motion.velocity),
                          projectile.combat.cast_id, projectile.id.value);
    }
    if (damage_upgrade == kNoTelemetrySource &&
        projectile.combat.skill == SkillKind::RicochetArrow &&
        HasUpgrade(projectile.combat.upgrade_mask, 3) && projectile.combat.hit_count > 6)
        damage_upgrade = 2;
    if (damage_upgrade < kUpgradeCount &&
        projectile.combat.source_upgrade == kNoTelemetrySource)
        RecordUpgradeEffect(projectile.combat.skill, damage_upgrade,
                            UpgradeEffectMetric::ExtraTargetsHit);
    if (projectile.combat.skill == SkillKind::PiercingShot && HasUpgrade(projectile.combat.upgrade_mask, 3))
    {
        bleed = std::max<std::uint8_t>(bleed, 2);
    }
    if (projectile.combat.skill == SkillKind::MultiShot &&
        HasUpgrade(projectile.combat.upgrade_mask, 5) && cast_hit == 1)
    {
        bleed = std::max<std::uint8_t>(bleed, 1);
    }
    const auto multi_hit_limit = projectile.combat.skill == SkillKind::MultiShot
        ? 2u
        : std::numeric_limits<unsigned>::max();
    if (cast_hit <= multi_hit_limit)
    {
        if (projectile.combat.carried_burn_attack > 0.0f)
        {
            ApplyBurn(enemy, projectile.combat.carried_burn_attack, false,
                      projectile.combat.carried_burn_expires > tick
                          ? projectile.combat.carried_burn_expires - tick
                          : 1,
                      projectile.combat.skill, 3);
            projectile.combat.carried_burn_attack = 0.0f;
        }
        if (projectile.combat.skill == SkillKind::ChargedShot && projectile.combat.full_charge &&
            HasUpgrade(projectile.combat.upgrade_mask, 5) &&
            enemy.status.bleed_count >=
                rules.statuses.bleed.maximum_stacks)
        {
            QueueDamage(enemy.id.value,
                       RoundFinalDamage(EffectiveAttack() * rules.upgrades.charged_shot
                                       .charge_bleed_full_charge_rupture.rupture_damage_multiplier,
                                       rules),
                       projectile.combat.skill, EffectOrigin::Derived,
                       projectile.combat.cast_id, 0, false, 0.0f, 0, 4);
        }
        if (projectile.combat.skill != SkillKind::ExplosiveArrow ||
            projectile.combat.explosion_radius <= 0.0f)
        {
            QueueDamage(enemy.id.value, damage, projectile.combat.skill, projectile.combat.origin,
                       projectile.combat.cast_id, bleed, burn, slow, slow_duration,
                       damage_upgrade, projectile.combat.source_relic);
            if (projectile.combat.skill == SkillKind::ChargedShot)
            {
                auto &event = combat_state->combat.commands.back();
                const auto &charged = rules.skills[static_cast<std::size_t>(
                    SkillKind::ChargedShot)];
                const auto &charge_upgrades = rules.upgrades.charged_shot;
                const auto add_amplifier = [&](std::uint8_t upgrade,
                                               float coefficient) {
                    if (coefficient <= 0.0f ||
                        event.amplified_count == event.amplified_damage.size())
                        return;
                    const auto index = event.amplified_count++;
                    event.amplified_upgrades[index] = upgrade;
                    event.amplified_damage[index] =
                        RoundFinalDamage(EffectiveAttack() * coefficient, rules);
                };
                const auto decay = 1.0f;
                if (HasUpgrade(projectile.combat.upgrade_mask, 1))
                    add_amplifier(
                        0, (charge_upgrades.extended_full_charge_explosion
                                .maximum_damage_multiplier -
                            charged.maximum_damage_multiplier) *
                               projectile.combat.charge_ratio * decay);
                if (HasUpgrade(projectile.combat.upgrade_mask, 3))
                    add_amplifier(2,
                                   (charge_upgrades.increase_minimum_charge_damage
                                        .minimum_damage_multiplier -
                                    charged.minimum_damage_multiplier) *
                                      (1.0f - projectile.combat.charge_ratio) * decay);
            }
        }
    }

    if (projectile.combat.skill == SkillKind::BasicAttack &&
        HasUpgrade(projectile.combat.upgrade_mask, 6) && projectile.combat.hit_count == 1)
    {
        const auto &slow_rule = rules.upgrades.basic_attack.first_hit_slow_and_cooldown;
        BeginStatusEpisode(enemy, 2, tick);
        enemy.status.slows.push_back({slow_rule.slow_fraction,
                                      tick + slow_rule.slow_duration_ticks,
                                      SkillKind::BasicAttack, 5});
        EmitVfx(DomainSignalKind::SlowApplied, enemy.motion.position, {},
                enemy.life.boss ? 1.4f : 0.75f, 0.025f);
        BindStatusVisual(combat_state->domain_signals.back(), enemy,
                         SkillKind::BasicAttack, 5, 2);
        RecordUpgradeEffect(SkillKind::BasicAttack, 5,
                            UpgradeEffectMetric::SlowApplications);
        RecordUpgradeEffect(SkillKind::BasicAttack, 5,
                            UpgradeEffectMetric::SlowTargetTicks,
                             slow_rule.slow_duration_ticks);
        const auto longest = std::ranges::max_element(actors->player.cooldowns);
        if (longest != actors->player.cooldowns.end())
        {
            const auto before = *longest;
            *longest = *longest > slow_rule.cooldown_reduction_ticks
                           ? *longest - slow_rule.cooldown_reduction_ticks
                           : 0;
            RecordUpgradeEffect(SkillKind::BasicAttack, 5,
                                UpgradeEffectMetric::CooldownTicksSaved,
                                before - *longest);
            if (before > *longest)
                EmitUpgradeVisual(SkillKind::BasicAttack, 6,
                                  UpgradeVisualStage::Spawn, actors->player.position,
                                  actors->player.aim, projectile.combat.cast_id,
                                  kPlayerRenderId);
        }
    }
    if (projectile.combat.skill == SkillKind::PiercingShot &&
        HasUpgrade(projectile.combat.upgrade_mask, 6) && !enemy.life.boss)
    {
        const auto &align = rules.upgrades.piercing_shot.align_hit_normal_enemy;
        const auto before = enemy.motion.position;
        const auto displacement = Multiply(Normalize(projectile.motion.velocity),
                                           align.move_distance);
        QueueEnemyDisplacement(enemy, displacement);
        EmitVfx(DomainSignalKind::Push, before,
                Normalize(projectile.motion.velocity), 1.0f,
                enemy.life.boss ? 1.4f : 0.75f);
        EmitUpgradeVisual(SkillKind::PiercingShot, 6,
                          UpgradeVisualStage::Resolve, before,
                          Normalize(projectile.motion.velocity), projectile.combat.cast_id,
                          projectile.id.value);
        RecordUpgradeDisplacement(SkillKind::PiercingShot, 5, before,
                                  Add(before, displacement));
    }
    if (projectile.combat.skill == SkillKind::PiercingShot &&
        HasUpgrade(projectile.combat.upgrade_mask, 7))
    {
        if (runtime.transfer_count == 0)
        {
            if (!enemy.status.burn)
                ApplyBurn(enemy, EffectiveAttack(), false, rules.statuses.burn.duration_ticks,
                          projectile.combat.skill, 6);
            runtime.has_stored_burn = true;
            runtime.stored_burn_attack = enemy.status.burn->attack_snapshot;
            runtime.stored_burn_remaining = enemy.status.burn->expires > tick
                ? enemy.status.burn->expires - tick
                : 1;
            runtime.transfer_count = 1;
        }
        else if (runtime.transfer_count <
                     1 + rules.upgrades.piercing_shot.apply_and_transfer_burn
                             .maximum_transfer_targets &&
                 runtime.has_stored_burn)
        {
            const auto source = projectile.combat.hit_ids.size() >= 2
                                     ? FindEnemy(projectile.combat.hit_ids[projectile.combat.hit_ids.size() - 2])
                                     : std::nullopt;
            EmitVfxLine(DomainSignalKind::BurnTransferred,
                        source ? source->motion.position : projectile.motion.previous_position,
                        enemy.motion.position);
            BindUpgradeLink(combat_state->visual_links.back(), projectile.combat.skill,
                            6, projectile.combat.cast_id);
            ApplyBurn(enemy, runtime.stored_burn_attack, false,
                      runtime.stored_burn_remaining, projectile.combat.skill, 6);
            ++runtime.transfer_count;
        }
    }
    if (projectile.combat.skill == SkillKind::ChargedShot &&
        HasUpgrade(projectile.combat.upgrade_mask, 6) && !runtime.triggered &&
        (enemy.life.boss || runtime.normal_hits >= 3))
    {
        runtime.triggered = true;
        const auto direction = Normalize(projectile.motion.velocity);
        const auto &split = rules.upgrades.charged_shot.boss_or_fifth_pierce_split;
        for (std::uint32_t index = 0; index < split.projectile_count; ++index)
            if (auto child = FireProjectile(projectile.combat.skill, projectile.motion.position,
                           Rotate(direction, split.angles_degrees[index]),
                           split.damage_multiplier,
                           EffectOrigin::Derived, projectile.combat.cast_id, 0, true, 5))
                EmitUpgradeVisual(SkillKind::ChargedShot, 6,
                                  UpgradeVisualStage::Spawn, child->motion.position,
                                  Normalize(child->motion.velocity), projectile.combat.cast_id,
                                  child->id.value);
    }
    if (projectile.combat.skill == SkillKind::ChargedShot &&
        HasUpgrade(projectile.combat.upgrade_mask, 7) && runtime.transfer_count == 0)
    {
        if (!enemy.status.burn)
            ApplyBurn(enemy, EffectiveAttack(), false, rules.statuses.burn.duration_ticks,
                       projectile.combat.skill, 6);
        runtime.transfer_count = 1;
        RecordUpgradeEffect(SkillKind::ChargedShot, 6,
                            UpgradeEffectMetric::ExplosionsCreated);
        const auto &burn_explosion = rules.upgrades.charged_shot.first_hit_burn_explosion;
        QueueAreaDamage(enemy.motion.position, burn_explosion.radius,
                   RoundFinalDamage(EffectiveAttack() * burn_explosion.damage_multiplier, rules), projectile.combat.skill,
                   EffectOrigin::Derived, projectile.combat.cast_id, 0, false, 0.0f, 0, 6);
        EmitVfx(DomainSignalKind::LargeExplosion, enemy.motion.position, {},
                burn_explosion.radius, 0.15f, 0, 0, ExplosionGeometry(burn_explosion.radius));
        auto &burn_signal = combat_state->domain_signals.back();
        burn_signal.upgrade_skill = static_cast<std::uint8_t>(projectile.combat.skill);
        burn_signal.upgrade_index = 6;
        burn_signal.upgrade_stage = UpgradeVisualStage::Resolve;
        burn_signal.upgrade_cast_id = projectile.combat.cast_id;
        burn_signal.upgrade_owner_id = projectile.id.value;
        burn_signal.geometry.source_id = burn_signal.upgrade_owner_id;
    }
    if (projectile.combat.skill == SkillKind::MultiShot &&
        HasUpgrade(projectile.combat.upgrade_mask, 6) &&
        projectile.combat.origin == EffectOrigin::Original &&
        runtime.transfer_count < rules.upgrades.multishot.apply_burn_and_transfer_arrow
                                  .maximum_transfers_per_cast)
    {
        if (!enemy.status.burn)
            ApplyBurn(enemy, EffectiveAttack(), false, rules.statuses.burn.duration_ticks,
                       projectile.combat.skill, 5);
        std::optional<EnemyRow> target{};
        auto best = std::numeric_limits<float>::max();
        for (auto candidate : actors->enemies)
        {
            if (candidate.life.dead || candidate.id.value == enemy.id.value ||
                candidate.status.burn)
                continue;
            const auto distance = DistanceSquared(enemy.motion.position, candidate.motion.position);
            if (distance < best)
            {
                best = distance;
                target.emplace(candidate);
            }
        }
        if (target)
        {
            if (auto arrow = FireProjectile(
                    projectile.combat.skill, enemy.motion.position,
                    Normalize(Subtract(target->motion.position, enemy.motion.position)),
                    rules.upgrades.multishot.apply_burn_and_transfer_arrow
                        .damage_multiplier,
                    EffectOrigin::Derived, projectile.combat.cast_id,
                    static_cast<std::uint8_t>(projectile.combat.upgrade_mask &
                                              (std::uint8_t{1} << 7)),
                    true, 5))
            {
                arrow->combat.homing = true;
                arrow->combat.homing_target = target->id.value;
                arrow->combat.burn = true;
                ++runtime.transfer_count;
                EmitVfxLine(DomainSignalKind::BurnTransferred,
                            enemy.motion.position, target->motion.position);
                BindUpgradeLink(combat_state->visual_links.back(), projectile.combat.skill,
                                5, projectile.combat.cast_id);
                EmitUpgradeVisual(SkillKind::MultiShot, 6,
                                  UpgradeVisualStage::Spawn, arrow->motion.position,
                                  Normalize(arrow->motion.velocity), projectile.combat.cast_id,
                                  arrow->id.value);
            }
        }
    }
    if (projectile.combat.skill == SkillKind::ExplosiveArrow &&
        projectile.combat.origin == EffectOrigin::Original &&
        HasUpgrade(projectile.combat.upgrade_mask, 7))
    {
        const auto &mark = rules.upgrades.explosive_arrow.direct_hit_mark_other_active_explosion;
        BeginStatusEpisode(enemy, 3, tick);
        enemy.cold.marked_by_skill = projectile.combat.skill;
        enemy.cold.marked_damage_coefficient = mark.mark_explosion_damage_multiplier;
        enemy.cold.mark_expires = tick + mark.mark_duration_ticks;
        EmitVfx(DomainSignalKind::MarkApplied, enemy.motion.position, {},
                enemy.life.boss ? 1.4f : 0.75f, 0.025f);
        BindStatusVisual(combat_state->domain_signals.back(), enemy,
                         SkillKind::ExplosiveArrow, 6, 3);
        RecordUpgradeEffect(SkillKind::ExplosiveArrow, 6,
                            UpgradeEffectMetric::ExplosionsCreated);
        QueueAreaDamage(enemy.motion.position,
                   rules.skills[static_cast<std::size_t>(SkillKind::ExplosiveArrow)]
                       .area_radius,
                    RoundFinalDamage(EffectiveAttack() * mark.immediate_explosion_damage_multiplier, rules),
                   projectile.combat.skill,
                   EffectOrigin::Derived, projectile.combat.cast_id, 0, false,
                    0.0f, 0, 6);
        EmitUpgradeVisual(SkillKind::ExplosiveArrow, 7,
                          UpgradeVisualStage::Resolve, enemy.motion.position, {},
                          projectile.combat.cast_id, projectile.id.value);
        RecordUpgradeEffect(SkillKind::ExplosiveArrow, 6,
                            UpgradeEffectMetric::MarksApplied);
    }
    if (projectile.combat.skill == SkillKind::RetreatShot &&
        HasUpgrade(projectile.combat.upgrade_mask, 4) &&
        runtime.spawn_count < rules.upgrades.retreat_shot.apply_bleed_and_tracking_arrow
                                  .maximum_triggers_per_cast)
    {
        const auto &tracking = rules.upgrades.retreat_shot.apply_bleed_and_tracking_arrow;
        if (enemy.status.bleed_count == 0)
            ApplyBleed(enemy, EffectiveAttack(), 1, projectile.combat.skill, 3);
        if (auto target = NearestEnemy(
                enemy.motion.position,
                rules.skills[static_cast<std::size_t>(SkillKind::RetreatShot)].projectile_range,
                std::span(projectile.combat.hit_ids)))
        {
            if (auto arrow = FireProjectile(projectile.combat.skill, enemy.motion.position,
                    Normalize(Subtract(target->motion.position, enemy.motion.position)),
                    tracking.damage_multiplier,
                    EffectOrigin::Derived, projectile.combat.cast_id, 0, true, 3))
            {
                arrow->combat.homing = true;
                arrow->combat.homing_target = target->id.value;
                ++runtime.spawn_count;
                EmitUpgradeVisual(SkillKind::RetreatShot, 4,
                                  UpgradeVisualStage::Spawn, arrow->motion.position,
                                  Normalize(arrow->motion.velocity), projectile.combat.cast_id,
                                  arrow->id.value);
            }
        }
    }
    if (projectile.combat.skill == SkillKind::RetreatShot &&
        HasUpgrade(projectile.combat.upgrade_mask, 7) && !runtime.triggered &&
        (enemy.life.boss || runtime.normal_hits >=
             rules.upgrades.retreat_shot.boss_or_three_normal_hits_heal
                 .minimum_normal_enemy_hits))
    {
        runtime.triggered = true;
        RecordUpgradeEffect(
            SkillKind::RetreatShot, 6, UpgradeEffectMetric::Activations);
        RecordUpgradeEffect(
            SkillKind::RetreatShot, 6, UpgradeEffectMetric::Healing,
            Heal(RoundDamage(actors->player.max_health * rules.upgrades.retreat_shot
                                 .boss_or_three_normal_hits_heal.maximum_hp_heal_fraction)));
        EmitUpgradeVisual(SkillKind::RetreatShot, 7,
                          UpgradeVisualStage::Resolve, actors->player.position,
                          {}, projectile.combat.cast_id, projectile.id.value);
    }

    if (projectile.combat.origin == EffectOrigin::Original)
    {
        const auto direction = Normalize(projectile.motion.velocity);
        if (projectile.combat.skill == SkillKind::BasicAttack &&
            HasUpgrade(projectile.combat.upgrade_mask, 3) && projectile.combat.hit_count == 1)
        {
            const auto &split = rules.upgrades.basic_attack.first_hit_split;
            std::uint64_t first_child_id{};
            Float2 first_child_position{};
            Float2 first_child_velocity{};
            for (std::uint32_t index = 0; index < split.projectile_count; ++index)
            {
                if (auto child = FireProjectile(
                        projectile.combat.skill, projectile.motion.position,
                        Rotate(direction, split.angles_degrees[index]),
                        split.damage_multiplier, EffectOrigin::Derived,
                        projectile.combat.cast_id, 0, true, 2); child && first_child_id == 0)
                {
                    first_child_id = child->id.value;
                    first_child_position = child->motion.position;
                    first_child_velocity = child->motion.velocity;
                }
            }
            if (first_child_id != 0)
                EmitUpgradeVisual(SkillKind::BasicAttack, 3,
                                  UpgradeVisualStage::Spawn, first_child_position,
                                  Normalize(first_child_velocity), projectile.combat.cast_id,
                                  first_child_id);
        }
        if (projectile.combat.skill == SkillKind::MultiShot &&
            HasUpgrade(projectile.combat.upgrade_mask, 2) && projectile.combat.hit_count == 1)
        {
            const auto &split = rules.upgrades.multishot.original_arrow_first_hit_split;
            for (std::uint32_t index = 0; index < split.projectile_count_per_original; ++index)
                if (auto child = FireProjectile(projectile.combat.skill, projectile.motion.position,
                               Rotate(direction, split.angles_degrees[index]),
                               split.damage_multiplier, EffectOrigin::Derived, projectile.combat.cast_id,
                               static_cast<std::uint8_t>(projectile.combat.upgrade_mask &
                                                         (std::uint8_t{1} << 7)),
                               true, 1))
                    EmitUpgradeVisual(SkillKind::MultiShot, 2,
                                      UpgradeVisualStage::Spawn, child->motion.position,
                                      Normalize(child->motion.velocity), projectile.combat.cast_id,
                                      child->id.value);
        }
        if (projectile.combat.skill == SkillKind::PiercingShot &&
            HasUpgrade(projectile.combat.upgrade_mask, 5) &&
            projectile.combat.hit_count % rules.upgrades.piercing_shot
                                      .per_three_pierces_perpendicular.normal_enemy_pierce_interval == 0)
        {
            const auto &split = rules.upgrades.piercing_shot.per_three_pierces_perpendicular;
            for (std::uint32_t index = 0; index < split.projectile_count; ++index)
            {
                const auto angle = index == 0 ? -split.angle_from_direction
                                              : split.angle_from_direction;
                if (auto child = FireProjectile(projectile.combat.skill, projectile.motion.position, Rotate(direction, angle),
                               split.damage_multiplier, EffectOrigin::Derived, projectile.combat.cast_id,
                               0, true, 4))
                    EmitUpgradeVisual(SkillKind::PiercingShot, 5,
                                      UpgradeVisualStage::Spawn, child->motion.position,
                                      Normalize(child->motion.velocity), projectile.combat.cast_id,
                                      child->id.value);
            }
        }
    }
    const auto hit_height = enemy.life.boss ? 1.4f : 0.75f;
    if (const auto impact = ProjectileImpactKind(projectile);
        impact != DomainSignalKind::Count)
        EmitVfx(impact, contact_position,
                {projectile.motion.velocity.x, projectile.motion.velocity.y}, 1.0f,
                projectile.combat.player_owned ? 1.05f : 0.45f, 0, 0,
                ProjectileImpactGeometry(projectile));
    constexpr auto hit_scale = 1.0f;
    const auto hit_direction = Normalize(projectile.motion.velocity);
    if (projectile.combat.skill == SkillKind::ChargedShot && projectile.combat.full_charge)
        EmitVfx(DomainSignalKind::HeavyHit, enemy.motion.position,
                hit_direction, hit_scale, hit_height);
    else if (projectile.combat.skill == SkillKind::RicochetArrow)
        EmitVfx(DomainSignalKind::RicochetArrowHit, enemy.motion.position,
                hit_direction, hit_scale, hit_height);
    else if (projectile.combat.skill == SkillKind::BasicAttack)
        EmitVfx(DomainSignalKind::BasicAttackImpact, enemy.motion.position,
                hit_direction, hit_scale, hit_height);
    else
        EmitVfx(DomainSignalKind::ProjectileHit, enemy.motion.position,
                hit_direction, hit_scale, hit_height,
                static_cast<std::uint8_t>(projectile.combat.skill));

    if (projectile.combat.skill == SkillKind::RicochetArrow && !projectile.combat.returning && projectile.combat.bounce_remaining > 0)
    {
        if (HasUpgrade(projectile.combat.upgrade_mask, 3))
        {
            if (enemy.status.bleed_count == 0)
                ApplyBleed(enemy, EffectiveAttack(), 1, projectile.combat.skill, 2);
        }
        const auto &ricochet_upgrades = rules.upgrades.ricochet_arrow;
        if (HasUpgrade(projectile.combat.upgrade_mask, 3) &&
            runtime.transfer_count < ricochet_upgrades.apply_bleed_and_extend_ricochets
                                      .maximum_additional_ricochets)
        {
            const auto previous_bounces = projectile.combat.bounce_remaining;
            projectile.combat.bounce_remaining = static_cast<std::uint8_t>(
                projectile.combat.bounce_remaining + ricochet_upgrades.apply_bleed_and_extend_ricochets
                                                   .additional_ricochets_per_bleeding_hit);
            if (projectile.combat.bounce_remaining > previous_bounces)
                projectile.episodes.bleed_extend_ticks.push_back(tick);
            runtime.transfer_count = static_cast<std::uint8_t>(
                runtime.transfer_count + ricochet_upgrades.apply_bleed_and_extend_ricochets
                                             .additional_ricochets_per_bleeding_hit);
            RecordUpgradeEffect(SkillKind::RicochetArrow, 2,
                                UpgradeEffectMetric::ExtraBounces);
        }
        --projectile.combat.bounce_remaining;
        std::optional<EnemyRow> next{};
        auto best_bleed = false;
        const auto search_radius = rules.skills[static_cast<std::size_t>(SkillKind::RicochetArrow)]
                                       .ricochet_search_radius;
        auto best_distance = search_radius * search_radius;
        const auto excluded = std::span(projectile.combat.hit_ids);
        auto &query = combat_state->nearest_query;
        CollectEnemies(projectile.motion.position, search_radius, query);
        for (const auto row : query.rows)
        {
            auto candidate = actors->enemies[row];
            if (candidate.life.dead ||
                std::ranges::find(excluded, candidate.id.value) != excluded.end())
                continue;
            const auto distance = DistanceSquared(projectile.motion.position, candidate.motion.position);
            if (distance > search_radius * search_radius) continue;
            const auto bleeding = candidate.status.bleed_count > 0;
            if (!next || (bleeding && !best_bleed) ||
                (bleeding == best_bleed && distance < best_distance))
            {
                next.emplace(candidate);
                best_bleed = bleeding;
                best_distance = distance;
            }
        }
        if (next)
        {
            EmitVfxLine(DomainSignalKind::RicochetLinked, enemy.motion.position, next->motion.position);
            if (projectile.combat.origin == EffectOrigin::Original &&
                HasUpgrade(projectile.combat.upgrade_mask, 2) && projectile.combat.hit_count == 1)
                BindUpgradeLink(combat_state->visual_links.back(), projectile.combat.skill,
                                1, projectile.combat.cast_id);
            else if (projectile.combat.origin == EffectOrigin::Derived && projectile.combat.source_upgrade == 5)
                BindUpgradeLink(combat_state->visual_links.back(), projectile.combat.skill,
                                5, projectile.combat.cast_id);
            if (projectile.combat.origin == EffectOrigin::Original &&
                HasUpgrade(projectile.combat.upgrade_mask, 7))
            {
                Tick *longest{};
                for (std::size_t index = 0; index < actors->player.cooldowns.size(); ++index)
                {
                    if (index == static_cast<std::size_t>(
                                     SkillKind::RicochetArrow) - 1)
                        continue;
                    if (!longest || actors->player.cooldowns[index] > *longest)
                        longest = &actors->player.cooldowns[index];
                }
                if (longest)
                {
                    const auto before = *longest;
                    const auto maximum = ricochet_upgrades.ricochet_cooldown_reduction
                                             .maximum_reduction_ticks;
                    const auto available = maximum > runtime.cooldown_ticks_saved
                        ? maximum - runtime.cooldown_ticks_saved : 0;
                    const auto reduction = std::min(
                        ricochet_upgrades.ricochet_cooldown_reduction
                            .cooldown_reduction_per_ricochet_ticks,
                        available);
                    *longest = before > reduction ? before - reduction : 0;
                    const auto saved = before - *longest;
                    runtime.cooldown_ticks_saved += saved;
                    RecordUpgradeEffect(SkillKind::RicochetArrow, 6,
                                        UpgradeEffectMetric::CooldownTicksSaved,
                                        saved);
                    if (saved > 0)
                    {
                        EmitUpgradeVisual(SkillKind::RicochetArrow, 7,
                                          UpgradeVisualStage::Resolve,
                                          actors->player.position, actors->player.aim,
                                          projectile.combat.cast_id, kPlayerRenderId);
                    }
                }
            }
            if (HasUpgrade(projectile.combat.upgrade_mask, 4) &&
                ricochet_upgrades.apply_and_copy_burn_to_next_target.enabled)
            {
                if (!enemy.status.burn)
                    ApplyBurn(enemy, EffectiveAttack(), false, rules.statuses.burn.duration_ticks,
                              projectile.combat.skill, 3);
                EmitVfxLine(DomainSignalKind::BurnTransferred,
                            enemy.motion.position, next->motion.position);
                BindUpgradeLink(combat_state->visual_links.back(), projectile.combat.skill,
                                3, projectile.combat.cast_id);
                projectile.combat.carried_burn_attack = enemy.status.burn->attack_snapshot;
                projectile.combat.carried_burn_expires = enemy.status.burn->expires;
            }
            if (projectile.combat.origin == EffectOrigin::Original &&
                HasUpgrade(projectile.combat.upgrade_mask, 2) && projectile.combat.hit_count == 1)
            {
                if (auto branch = FireProjectile(
                        projectile.combat.skill, projectile.motion.position,
                         Normalize(Subtract(next->motion.position, projectile.motion.position)),
                         ricochet_upgrades.first_ricochet_branch_chain.damage_multiplier,
                        EffectOrigin::Derived, projectile.combat.cast_id, 0, true, 1))
                {
                    branch->combat.homing = true;
                    branch->combat.homing_target = next->id.value;
                    branch->combat.bounce_remaining = static_cast<std::uint8_t>(
                        ricochet_upgrades.first_ricochet_branch_chain.maximum_targets - 1);
                    EmitUpgradeVisual(SkillKind::RicochetArrow, 2,
                                      UpgradeVisualStage::Spawn, branch->motion.position,
                                      Normalize(branch->motion.velocity), projectile.combat.cast_id,
                                      branch->id.value);
                }
            }
            projectile.combat.homing = true;
            projectile.combat.homing_target = next->id.value;
            projectile.motion.velocity = Multiply(Normalize(Subtract(next->motion.position,
                                                              projectile.motion.position)),
                                            rules.skills[static_cast<std::size_t>(SkillKind::RicochetArrow)]
                                                .projectile_speed);
            projectile.motion.remaining_range = search_radius;
            return true;
        }
        if (projectile.combat.origin == EffectOrigin::Original)
        {
            runtime.terminal_target = enemy.id.value;
        }
    }
    if (projectile.combat.skill == SkillKind::RicochetArrow &&
        HasUpgrade(projectile.combat.upgrade_mask, 1) && !projectile.combat.returning)
    {
        const auto &ricochet_upgrades = rules.upgrades.ricochet_arrow;
        projectile.combat.returning = true;
        projectile.episodes.return_started_tick = tick;
        projectile.episodes.return_start_position = projectile.motion.position;
        projectile.combat.source_upgrade = 0;
        projectile.combat.homing = false;
        projectile.combat.homing_target = 0;
        projectile.combat.hit_count = 0;
        projectile.combat.hit_ids.clear();
        projectile.combat.pierce_remaining = static_cast<std::uint8_t>(
            ricochet_upgrades.return_to_player_rehit.maximum_rehit_targets);
        projectile.combat.damage = RoundFinalDamage(EffectiveAttack() *
                                             ricochet_upgrades.return_to_player_rehit.damage_multiplier,
                                             rules);
        projectile.motion.remaining_range = std::max(0.1f,
            Length(Subtract(actors->player.position, projectile.motion.position)));
        projectile.motion.velocity = Multiply(
            Normalize(Subtract(actors->player.position, projectile.motion.position)),
            rules.skills[static_cast<std::size_t>(SkillKind::RicochetArrow)].projectile_speed);
        return true;
    }
    if (projectile.combat.pierce_remaining > 0 && (!enemy.life.boss || projectile.combat.pierce_remaining != 255))
    {
        --projectile.combat.pierce_remaining;
    }
    else if (projectile.combat.pierce_remaining == 255 && !enemy.life.boss)
    {
    }
    else
    {
        projectile.combat.dead = true;
    }
    return false;
}

void GameSimulation::SimulationWorld::ExplodeProjectile(ProjectileRow projectile)
{
    if (projectile.combat.explosion_radius <= 0.0f)
    {
        return;
    }
    if (projectile.combat.origin == EffectOrigin::Original &&
        projectile.combat.skill == SkillKind::ExplosiveArrow &&
        HasUpgrade(projectile.combat.upgrade_mask, 6))
    {
        const auto &pull = rules.upgrades.explosive_arrow.pre_explosion_pull;
        EmitVfx(DomainSignalKind::Pull, projectile.motion.position, {},
                pull.pull_radius, 0.12f);
        EmitUpgradeVisual(SkillKind::ExplosiveArrow, 6,
                          UpgradeVisualStage::Resolve, projectile.motion.position, {},
                          projectile.combat.cast_id, projectile.id.value,
                          ExplosionGeometry(pull.pull_radius));
        for (auto enemy : actors->enemies)
        {
            if (!enemy.life.dead && !enemy.life.boss &&
                DistanceSquared(projectile.motion.position, enemy.motion.position) <=
                    pull.pull_radius * pull.pull_radius &&
                    SegmentClear2D(projectile.motion.position, enemy.motion.position, 0.0f,
                                   rules.arena_boundary, ArenaObstacles()))
            {
                const auto before = enemy.motion.position;
                const auto delta = Subtract(projectile.motion.position, enemy.motion.position);
                const auto displacement = Multiply(Normalize(delta),
                                                    std::min(pull.pull_radius, Length(delta)));
                QueueEnemyDisplacement(enemy, displacement);
                RecordUpgradeDisplacement(SkillKind::ExplosiveArrow, 5, before,
                                          Add(before, displacement));
            }
        }
    }
    const auto explosion_source =
        projectile.combat.explosion_source_upgrade < kUpgradeCount
            ? projectile.combat.explosion_source_upgrade
            : projectile.combat.source_upgrade;
    RecordUpgradeEffect(projectile.combat.skill, explosion_source,
                        UpgradeEffectMetric::ExplosionsCreated);
    QueueAreaDamage(projectile.motion.position, projectile.combat.explosion_radius,
               projectile.combat.explosion_damage != 0 ? projectile.combat.explosion_damage
                                                : projectile.combat.damage,
               projectile.combat.skill, projectile.combat.origin, projectile.combat.cast_id, 0, false, 0.0f, 0,
               explosion_source,
               projectile.combat.source_relic);
    if (projectile.combat.origin == EffectOrigin::Original &&
        projectile.combat.skill == SkillKind::ExplosiveArrow)
    {
        if (HasUpgrade(projectile.combat.upgrade_mask, 1))
        {
            const auto &reexplosion = rules.upgrades.explosive_arrow.delayed_reexplosion;
            ScheduleAction({tick + reexplosion.delay_ticks, ScheduledKind::Explosion,
                      projectile.combat.skill, projectile.motion.position, {}, reexplosion.damage_multiplier,
                      reexplosion.radius, 0.0f, 1,
                      0, EffectOrigin::Derived,
                      projectile.combat.cast_id, 0});
        }
        if (HasUpgrade(projectile.combat.upgrade_mask, 2))
        {
            const auto &satellite = rules.upgrades.explosive_arrow.three_delayed_satellite_bombs;
            for (std::uint32_t index = 0; index < satellite.bomb_count; ++index)
            {
                const auto position = Add(projectile.motion.position, Multiply(
                    Rotate({0.0f, 1.0f}, index * satellite.angular_spacing), satellite.placement_radius));
                if (!SegmentClear2D(projectile.motion.position, position, 0.0f,
                                    rules.arena_boundary, ArenaObstacles()))
                    continue;
                ScheduleAction({tick + satellite.delay_ticks, ScheduledKind::Explosion, projectile.combat.skill,
                          position,
                          {}, satellite.damage_multiplier, satellite.explosion_radius, 0.0f, 1, 0,
                          EffectOrigin::Derived, projectile.combat.cast_id, 1});
            }
        }
        if (HasUpgrade(projectile.combat.upgrade_mask, 3))
        {
            const auto &fragments = rules.upgrades.explosive_arrow.eight_direction_fragments;
            const auto step = 360.0f / static_cast<float>(fragments.direction_count);
            for (std::uint32_t index = 0; index < fragments.direction_count; ++index)
            {
                if (auto fragment = FireProjectile(
                        projectile.combat.skill, projectile.motion.position,
                        Rotate({0.0f, 1.0f}, index * step), fragments.damage_multiplier,
                        EffectOrigin::Derived, projectile.combat.cast_id, 0, true, 2))
                {
                    fragment->motion.radius *= fragments.collision_radius_multiplier;
                    fragment->combat.pierce_remaining = static_cast<std::uint8_t>(fragments.pierce);

                    // The shard visual is emitted only after the projectile has
                    // been fully initialized.  Its owner is the actual child
                    // projectile, so presentation can keep one stable identity
                    // for the lifetime of this cosmetic activation.
                    DomainSignal signal;
                    signal.sequence = NextSimulationSequence(event_sequence);
                    signal.tick = tick;
                    signal.session_id = session_id;
                    signal.kind = DomainSignalKind::UpgradeVisual;
                    signal.position = {fragment->motion.position.x, 1.05f, fragment->motion.position.y};
                    const auto fragment_direction = Normalize(fragment->motion.velocity);
                    signal.direction = {fragment_direction.x, 0.0f, fragment_direction.y};
                    signal.upgrade_skill = static_cast<std::uint8_t>(projectile.combat.skill);
                    signal.upgrade_index = 2;
                    signal.upgrade_stage = UpgradeVisualStage::Spawn;
                    signal.upgrade_cast_id = projectile.combat.cast_id;
                    signal.upgrade_owner_id = fragment->id.value;
                    combat_state->domain_signals.push_back(signal);
                }
            }
        }
        if (HasUpgrade(projectile.combat.upgrade_mask, 4))
        {
            const auto &fire = rules.upgrades.explosive_arrow.explosion_leaves_burning_area;
            SpawnArea(AreaKind::Damage, projectile.combat.skill, projectile.motion.position,
                      fire.radius, fire.damage_multiplier_per_tick,
                      static_cast<float>(fire.duration_ticks) / 60.0f, 0.0f,
                      EffectOrigin::Derived,
                      projectile.combat.cast_id, 0, 0.0f, 0.0f, 0.0f, true, 3);
        }
        if (HasUpgrade(projectile.combat.upgrade_mask, 5))
        {
            const auto &blood = rules.upgrades.explosive_arrow.apply_bleed_and_blood_explosions;
            std::uint8_t blood_explosions{};
            for (auto enemy : actors->enemies)
            {
                if (blood_explosions >= blood.maximum_explosions_per_cast) break;
                if (!enemy.life.dead &&
                    DistanceSquared(projectile.motion.position, enemy.motion.position) <=
                        blood.radius * blood.radius &&
                    SegmentClear2D(projectile.motion.position, enemy.motion.position, 0.0f,
                                   rules.arena_boundary, ArenaObstacles()))
                {
                    if (enemy.status.bleed_count == 0)
                        ApplyBleed(enemy, EffectiveAttack(), static_cast<std::uint8_t>(blood.bleed_stacks),
                                   projectile.combat.skill, 4);
                    QueueAreaDamage(enemy.motion.position, blood.radius,
                                RoundFinalDamage(EffectiveAttack() * blood.damage_multiplier, rules),
                               projectile.combat.skill, EffectOrigin::Derived,
                               projectile.combat.cast_id, 0, false, 0.0f, 0, 4);
                    EmitVfx(DomainSignalKind::SmallExplosion, enemy.motion.position,
                            {}, blood.radius, 0.15f, 0, 0, ExplosionGeometry(blood.radius));
                    auto &blood_signal = combat_state->domain_signals.back();
                    blood_signal.upgrade_skill = static_cast<std::uint8_t>(projectile.combat.skill);
                    blood_signal.upgrade_index = 4;
                    blood_signal.upgrade_stage = UpgradeVisualStage::Resolve;
                    blood_signal.upgrade_cast_id = projectile.combat.cast_id;
                    blood_signal.upgrade_owner_id = projectile.id.value;
                    blood_signal.geometry.source_id = blood_signal.upgrade_owner_id;
                    RecordUpgradeEffect(SkillKind::ExplosiveArrow, 4,
                                        UpgradeEffectMetric::ExplosionsCreated);
                    ++blood_explosions;
                }
            }
        }
    }
    if (projectile.combat.skill == SkillKind::ExplosiveArrow &&
        projectile.combat.origin == EffectOrigin::Original)
        EmitVfx(DomainSignalKind::ExplosiveArrowMain, projectile.motion.position, {},
                projectile.combat.explosion_radius / 3.0f, 0.15f, 0, 0, ExplosionGeometry(projectile.combat.explosion_radius));
    else
        EmitVfx(DomainSignalKind::LargeExplosion, projectile.motion.position, {},
                projectile.combat.explosion_radius, 0.15f, 0, 0, ExplosionGeometry(projectile.combat.explosion_radius));
    if (projectile.combat.skill == SkillKind::ChargedShot &&
        projectile.combat.origin == EffectOrigin::Original &&
        projectile.combat.explosion_source_upgrade == 0)
    {
        auto &end_signal = combat_state->domain_signals.back();
        end_signal.upgrade_skill = static_cast<std::uint8_t>(projectile.combat.skill);
        end_signal.upgrade_index = 0;
        end_signal.upgrade_stage = UpgradeVisualStage::Resolve;
        end_signal.upgrade_cast_id = projectile.combat.cast_id;
        end_signal.upgrade_owner_id = projectile.id.value;
        end_signal.geometry.source_id = end_signal.upgrade_owner_id;
    }
}

void GameSimulation::SimulationWorld::CollisionHitPhase()
{
    auto projectile_timing = Measure(SimulationTimingId::ProjectileCollision);
    const auto initial_projectile_count = actors->projectiles.size();
    for (std::size_t projectile_index = 0;
         projectile_index < initial_projectile_count; ++projectile_index)
    {
        auto projectile = actors->projectiles[projectile_index];
        if (projectile.combat.dead) continue;
        auto segment_start = projectile.motion.previous_position;
        auto segment_end = projectile.motion.position;
        auto remaining_seconds = kTickSeconds;
        auto elapsed_fraction = 0.0f;
        std::uint64_t redirect_target{};
        std::uint32_t redirects{};
        for (;;)
        {
            const auto wall_time = FirstObstacleHitTime(segment_start,
                segment_end, projectile.motion.radius, ArenaObstacles());
            const auto reachable_end = wall_time <= 1.0f
                ? Add(segment_start, Multiply(Subtract(segment_end, segment_start), wall_time))
                : segment_end;
            bool redirected{};
            if (projectile.combat.player_owned)
            {
                const auto largest_enemy_radius = std::max(
                    rules.boss_common.collision_radius,
                    std::max(rules.enemies[static_cast<std::size_t>(EnemyKind::Melee)].collision_radius,
                             std::max(rules.enemies[static_cast<std::size_t>(EnemyKind::Ranged)].collision_radius,
                                      rules.enemies[static_cast<std::size_t>(EnemyKind::Suicide)].collision_radius)));
                const auto margin = projectile.motion.radius + largest_enemy_radius;
                CollectEnemies(
                    {std::min(segment_start.x, reachable_end.x) - margin,
                     std::min(segment_start.y, reachable_end.y) - margin},
                    {std::max(segment_start.x, reachable_end.x) + margin,
                     std::max(segment_start.y, reachable_end.y) + margin},
                    combat_state->collision_query,false);
                const auto &candidates = combat_state->collision_query.rows;
                auto &hits = combat_state->collision_hits;
                hits.clear();
                for (const auto index : candidates)
                {
                    const auto enemy = actors->enemies[index];
                    if(enemy.life.dead||AlreadyHit(projectile,enemy.id.value)) continue;
                    const auto radius = enemy.life.boss ? rules.boss_common.collision_radius
                        : rules.enemies[static_cast<std::size_t>(enemy.life.kind)].collision_radius;
                    const auto hit_time=MovingDiscHitTime(segment_start,segment_end,
                        enemy.motion.tick_motion,projectile.motion.radius+radius,elapsed_fraction);
                    if(hit_time<=1.0f&&hit_time<wall_time) hits.push_back({index,hit_time});
                }
                std::ranges::sort(hits, [&](const auto &left, const auto &right) {
                    if (left.time != right.time) return left.time < right.time;
                    return actors->enemies[left.enemy].id.value < actors->enemies[right.enemy].id.value;
                });
                // Hit displacement is deferred over eight ticks; this segment's motion stays fixed.
                for (const auto hit : hits)
                {
                    auto enemy = actors->enemies[hit.enemy];
                    if (enemy.life.dead || AlreadyHit(projectile, enemy.id.value)) continue;
                    const auto actor_time = hit.time;
                    // A return clears hit_ids, but its departure contact is not a new hit.
                    if (actor_time == 0.0f && enemy.id.value == redirect_target) continue;
                    if (actor_time <= 1.0f && actor_time < wall_time)
                    {
                        const auto contact_position = Add(
                            segment_start,
                            Multiply(Subtract(segment_end, segment_start),
                                     actor_time));
                        projectile.motion.position = contact_position;
                        if (OnProjectileHit(projectile, enemy, contact_position))
                        {
                            elapsed_fraction += (1.0f - elapsed_fraction) * actor_time;
                            remaining_seconds *= 1.0f - actor_time;
                            redirect_target = enemy.id.value;
                            redirected = true;
                            break;
                        }
                        if (projectile.combat.dead)
                        {
                            ExplodeProjectile(projectile);
                            break;
                        }
                    }
                }
            }
            else if (const auto actor_time = MovingDiscHitTime(segment_start,
                     segment_end, actors->player.tick_motion,
                     projectile.motion.radius + rules.stats.player_collision_radius);
                 actor_time <= 1.0f && actor_time < wall_time)
            {
                const auto contact_position = Add(
                    segment_start,
                    Multiply(Subtract(segment_end, segment_start), actor_time));
                if (const auto impact = ProjectileImpactKind(projectile);
                    impact != DomainSignalKind::Count)
                    EmitVfx(impact, contact_position,
                            {projectile.motion.velocity.x, projectile.motion.velocity.y}, 1.0f,
                            0.45f, 0, 0, ProjectileImpactGeometry(projectile));
                QueueDamage(0, projectile.combat.damage, SkillKind::Count,
                           EffectOrigin::Original, projectile.combat.cast_id, 0, false, 0.0f, 0,
                           kNoTelemetrySource, kNoTelemetrySource,
                           projectile.combat.source_enemy, 0, Normalize(projectile.motion.velocity));
                projectile.combat.dead = true;
            }
            if (projectile.combat.dead) break;
            if (redirected)
            {
                // Stop at the last verified contact if zero-time redirects exhaust the bound.
                if (++redirects >= 64 || remaining_seconds <= 0.0f) break;
                segment_start = projectile.motion.position;
                const auto step = Multiply(projectile.motion.velocity, remaining_seconds);
                segment_end = Add(segment_start, step);
                projectile.motion.remaining_range -= Length(step);
                continue;
            }
            projectile.motion.position = reachable_end;
            if (wall_time <= 1.0f)
            {
                if (const auto impact = ProjectileImpactKind(projectile);
                    impact != DomainSignalKind::Count)
                    EmitVfx(impact, projectile.motion.position,
                            {projectile.motion.velocity.x, projectile.motion.velocity.y}, 1.0f,
                            projectile.combat.player_owned ? 1.05f : 0.45f, 0, 0,
                            ProjectileImpactGeometry(projectile));
                ExplodeProjectile(projectile);
                projectile.combat.dead = true;
            }
            break;
        }
        if (projectile.combat.dead) continue;
        if (projectile.motion.remaining_range <= 0.0f)
        {
            if (projectile.combat.skill == SkillKind::PiercingShot &&
                projectile.combat.origin == EffectOrigin::Original &&
                HasUpgrade(projectile.combat.upgrade_mask, 2))
            {
                const auto direction = Normalize(projectile.motion.velocity);
                const auto &split = rules.upgrades.piercing_shot.range_end_split;
                for (std::uint32_t index = 0; index < split.projectile_count; ++index)
                {
                    const auto angle = split.angles_degrees[index];
                    auto child_direction = Rotate(direction, angle);
                    const auto target = NearestEnemy(projectile.motion.position, split.search_radius);
                    if (target)
                        child_direction = Normalize(Subtract(target->motion.position,
                                                             projectile.motion.position));
                    if (auto branch = FireProjectile(
                            projectile.combat.skill, projectile.motion.position, child_direction,
                             split.damage_multiplier, EffectOrigin::Derived, projectile.combat.cast_id, 0,
                            true, 1))
                    {
                        if (target)
                        {
                            branch->combat.homing = true;
                            branch->combat.homing_target = target->id.value;
                        }
                        EmitUpgradeVisual(SkillKind::PiercingShot, 2,
                                          UpgradeVisualStage::Spawn, branch->motion.position,
                                          Normalize(branch->motion.velocity), projectile.combat.cast_id,
                                          branch->id.value);
                    }
                }
                projectile.combat.dead = true;
            }
            else if (projectile.combat.skill == SkillKind::MultiShot &&
                     HasUpgrade(projectile.combat.upgrade_mask, 8) &&
                     projectile.combat.hit_count == 0)
            {
                const auto &retarget = rules.upgrades.multishot.missed_arrow_retarget;
                if (auto target = NearestEnemy(projectile.motion.position, retarget.search_radius))
                {
                    if (auto arrow = FireProjectile(
                            projectile.combat.skill, projectile.motion.position,
                            Normalize(Subtract(target->motion.position,
                                               projectile.motion.position)),
                             retarget.damage_multiplier, EffectOrigin::Derived,
                            projectile.combat.cast_id, 0, true, 7))
                    {
                        arrow->combat.homing = true;
                        arrow->combat.homing_target = target->id.value;
                        EmitUpgradeVisual(SkillKind::MultiShot, 8,
                                          UpgradeVisualStage::Spawn, arrow->motion.position,
                                          Normalize(arrow->motion.velocity), projectile.combat.cast_id,
                                          arrow->id.value);
                    }
                }
                projectile.combat.dead = true;
            }
            else if (projectile.combat.skill == SkillKind::RicochetArrow &&
                     !projectile.combat.returning &&
                     HasUpgrade(projectile.combat.upgrade_mask, 1))
            {
                projectile.combat.returning = true;
                projectile.episodes.return_started_tick = tick;
                projectile.episodes.return_start_position = projectile.motion.position;
                projectile.combat.source_upgrade = 0;
                projectile.combat.hit_count = 0;
                projectile.combat.hit_ids.clear();
                projectile.combat.pierce_remaining = static_cast<std::uint8_t>(
                    rules.upgrades.ricochet_arrow.return_to_player_rehit.maximum_rehit_targets);
                projectile.motion.remaining_range = std::max(
                    0.1f, Length(Subtract(actors->player.position, projectile.motion.position)));
                projectile.motion.velocity = Multiply(
                    Normalize(Subtract(actors->player.position, projectile.motion.position)),
                    rules.skills[static_cast<std::size_t>(SkillKind::RicochetArrow)].projectile_speed);
                projectile.combat.damage = RoundFinalDamage(EffectiveAttack() *
                    rules.upgrades.ricochet_arrow.return_to_player_rehit.damage_multiplier, rules);
                projectile.combat.source_upgrade = 0;
            }
            else if (projectile.combat.skill == SkillKind::RicochetArrow &&
                     projectile.combat.returning)
            {
                projectile.combat.dead = true;
            }
            else if (projectile.combat.skill == SkillKind::BasicAttack &&
                HasUpgrade(projectile.combat.upgrade_mask, 7) && !projectile.combat.returning &&
                projectile.combat.hit_count == 0)
            {
                if (auto target = NearestEnemy(projectile.motion.position,
                                                rules.skills[static_cast<std::size_t>(SkillKind::BasicAttack)].range))
                {
                    projectile.combat.returning = true;
                    projectile.episodes.return_started_tick = tick;
                    projectile.episodes.return_start_position = projectile.motion.position;
                    projectile.combat.source_upgrade = 6;
                    projectile.combat.damage = RoundFinalDamage(EffectiveAttack() *
                        rules.upgrades.basic_attack.missed_arrow_reacquire.damage_multiplier, rules);
                    projectile.motion.remaining_range = rules.skills[static_cast<std::size_t>(SkillKind::BasicAttack)].range;
                    projectile.motion.velocity = Multiply(
                        Normalize(Subtract(target->motion.position, projectile.motion.position)),
                        rules.skills[static_cast<std::size_t>(SkillKind::BasicAttack)].projectile_speed);
                    projectile.motion.radius *= 2.0f;
                    projectile.combat.homing = true;
                    projectile.combat.homing_target = target->id.value;
                }
                else
                {
                    projectile.combat.dead = true;
                }
            }
            else
            {
                ExplodeProjectile(projectile);
                projectile.combat.dead = true;
            }
        }
    }

    projectile_timing.Finish();
    auto area_timing = Measure(SimulationTimingId::AreaCollision);
    std::vector<ScheduledAction> pending_areas;
    const auto initial_area_count = actors->areas.size();
    for (std::size_t area_index = 0; area_index < initial_area_count; ++area_index)
    {
        auto &area = actors->areas[area_index];
        if (area.dead || tick < area.active_tick || tick < area.next_tick) continue;
        if (area.kind == AreaKind::Trap && tick == area.active_tick)
            EmitVfx(DomainSignalKind::TrapArmed, area.position, {},
                    area.radius / 2.0f, 0.12f);
        if (area.kind == AreaKind::EnemyDamage)
        {
            if (area.ring_outer_radius <= 0.0f && area.source_enemy >= kBossTelemetryBase &&
                area.source_enemy != kNoTelemetrySource)
            {
                DomainSignalGeometry geometry{};
                geometry.kind = DomainSignalGeometryKind::Circle;
                geometry.radius = area.radius;
                geometry.start_tick = geometry.end_tick = tick;
                geometry.source_id = area.id.value;
                EmitVfx(DomainSignalKind::BossAreaTicked, area.position, {},
                        1.0f, 0.12f, 0, 0, geometry);
            }
            if (area.ring_outer_radius > 0.0f)
            {
                const auto duration = std::max<Tick>(area.expires - area.active_tick, 1);
                const auto progress = std::clamp(
                    static_cast<float>(tick - area.active_tick) /
                        static_cast<float>(duration),
                    0.0f, 1.0f);
                const auto radius = std::lerp(area.ring_inner_radius,
                                              area.ring_outer_radius, progress);
                const auto delta = Subtract(actors->player.position, area.position);
                auto angle = std::atan2(delta.y, delta.x) * 180.0f / kPi;
                if (angle < 0.0f) angle += 360.0f;
                bool safe = false;
                if (area.safe_gap_count != 0)
                {
                    const auto spacing = 360.0f / area.safe_gap_count;
                    const auto offset = static_cast<float>(area.cast_id % 360);
                    const auto nearest_gap = std::fmod(
                        angle - offset + spacing * 0.5f + 360.0f, spacing) -
                        spacing * 0.5f;
                    safe = std::abs(nearest_gap) <= area.safe_gap_degrees * 0.5f;
                }
                if (!safe &&
                    std::abs(Length(delta) - radius) <= area.ring_half_width &&
                    SegmentClear2D(area.position, actors->player.position, 0.0f,
                                   rules.arena_boundary, ArenaObstacles()) &&
                    IncrementAreaHit(area.id.value, 0) == 1)
                {
                    QueueDamage(0, area.damage, SkillKind::Count,
                               area.origin, area.cast_id, 0, false, 0.0f, 0,
                               kNoTelemetrySource, kNoTelemetrySource,
                               area.source_enemy, 0,
                               Normalize(delta), true, actors->player.position);
                }
            }
            else if (DistanceSquared(area.position, actors->player.position) <=
                     area.radius * area.radius &&
                     SegmentClear2D(area.position, actors->player.position, 0.0f,
                                    rules.arena_boundary, ArenaObstacles()))
            {
                QueueDamage(0, area.damage, SkillKind::Count,
                           area.origin, area.cast_id, 0, false, 0.0f, 0,
                           kNoTelemetrySource, kNoTelemetrySource,
                           area.source_enemy, 0,
                           Normalize(Subtract(actors->player.position, area.position)));
            }
        }
        else if (area.kind == AreaKind::Trap)
        {
            std::optional<EnemyRow> visible_target{};
            auto nearest_distance = area.radius * area.radius;
            CollectEnemies(area.position,area.radius,combat_state->area_query);
            for (const auto row:combat_state->area_query.rows)
            {
                auto candidate=actors->enemies[row];
                const auto distance = DistanceSquared(area.position, candidate.motion.position);
                if (!candidate.life.dead && distance <= nearest_distance &&
                    SegmentClear2D(area.position, candidate.motion.position, 0.0f,
                                   rules.arena_boundary, ArenaObstacles()))
                {
                    visible_target.emplace(candidate);
                    nearest_distance = distance;
                }
            }
            if (auto target = visible_target)
            {
                bool pull_displaced = false;
                if (HasUpgrade(area.upgrade_mask, 6))
                {
                    EmitVfx(DomainSignalKind::Pull, area.position, {},
                            area.effect_radius, 0.12f);
                    CollectEnemies(area.position,area.effect_radius,combat_state->area_query);
                    for (const auto row:combat_state->area_query.rows)
                    {
                        auto enemy=actors->enemies[row];
                        if (!enemy.life.dead && !enemy.life.boss &&
                            DistanceSquared(area.position, enemy.motion.position) <=
                                area.effect_radius * area.effect_radius &&
                            SegmentClear2D(area.position, enemy.motion.position, 0.0f,
                                           rules.arena_boundary, ArenaObstacles()))
                        {
                            const auto delta = Subtract(area.position, enemy.motion.position);
                            const auto before = enemy.motion.position;
                            const auto displacement = Multiply(
                             Normalize(delta), std::min(
                                 rules.upgrades.trap.pre_explosion_pull_and_slow.pull_radius,
                                 Length(delta)));
                            QueueEnemyDisplacement(enemy, displacement);
                            RecordUpgradeDisplacement(SkillKind::Trap, 5, before,
                                                      Add(before, displacement));
                            pull_displaced |= LengthSquared(displacement) > 0.000001f;
                        }
                    }
                }
                if (HasUpgrade(area.upgrade_mask, 7))
                {
                    BeginStatusEpisode(*target, 3, tick);
                    target->cold.marked_by_skill = SkillKind::Trap;
                    const auto &mark = rules.upgrades.trap.trigger_mark_other_active_damage;
                    target->cold.marked_damage_coefficient = mark.mark_explosion_damage_multiplier;
                    target->cold.mark_expires = tick + mark.mark_duration_ticks;
                    EmitVfx(DomainSignalKind::MarkApplied, target->motion.position,
                            {}, target->life.boss ? 1.4f : 0.75f, 0.025f);
                    BindStatusVisual(combat_state->domain_signals.back(), *target,
                                     SkillKind::Trap, 6, 3);
                    RecordUpgradeEffect(SkillKind::Trap, 6,
                                        UpgradeEffectMetric::ExplosionsCreated);
                    QueueDamage(target->id.value,
                        RoundFinalDamage(EffectiveAttack() * rules.upgrades.trap
                                            .trigger_mark_other_active_damage.immediate_damage_multiplier,
                                         rules),
                               SkillKind::Trap, EffectOrigin::Derived,
                               area.cast_id, 0, false, 0.0f, 0, 6);
                    RecordUpgradeEffect(SkillKind::Trap, 6,
                                        UpgradeEffectMetric::MarksApplied);
                }
                const auto damage_source = area.trigger_count > 0 &&
                                                   HasUpgrade(area.upgrade_mask, 3)
                                               ? std::uint8_t{2}
                                               : area.source_upgrade;
                QueueAreaDamage(area.position, area.effect_radius, area.damage, area.skill,
                           area.origin, area.cast_id,
                           HasUpgrade(area.upgrade_mask, 4) ? 3 : 0,
                           HasUpgrade(area.upgrade_mask, 5),
                           HasUpgrade(area.upgrade_mask, 6) ? rules.upgrades.trap.pre_explosion_pull_and_slow.slow_fraction : area.slow_reduction,
                           HasUpgrade(area.upgrade_mask, 6) ? rules.upgrades.trap.pre_explosion_pull_and_slow.slow_duration_ticks
                                                             : area.slow_duration,
                           damage_source, area.source_relic, area.id.value);
                DomainSignalGeometry trap_damage_geometry{};
                trap_damage_geometry.kind = DomainSignalGeometryKind::Circle;
                trap_damage_geometry.radius = area.radius;
                trap_damage_geometry.start_tick = trap_damage_geometry.end_tick = tick;
                trap_damage_geometry.source_id = area.id.value;
                EmitVfx(DomainSignalKind::TrapDamaged, area.position, {},
                        1.0f, 0.28f, static_cast<std::uint8_t>(area.skill), area.id.value,
                        trap_damage_geometry);
                auto &trap_damage_signal = combat_state->domain_signals.back();
                trap_damage_signal.upgrade_cast_id = area.cast_id;
                trap_damage_signal.upgrade_owner_id = area.id.value;
                if (HasUpgrade(area.upgrade_mask, 5))
                {
                    pending_areas.push_back(
                        {tick, ScheduledKind::Area, area.skill, target->motion.position, {},
                          rules.upgrades.trap.trigger_burn_and_fire_area.damage_multiplier_per_tick,
                          rules.upgrades.trap.trigger_burn_and_fire_area.area_radius,
                          static_cast<float>(rules.upgrades.trap.trigger_burn_and_fire_area.area_duration_ticks) / 60.0f,
                          1, 0,
                         EffectOrigin::Derived, area.cast_id, 4});
                }
                ++area.trigger_count;
                if (HasUpgrade(area.upgrade_mask, 3) && area.trigger_count == 1)
                {
                    area.next_tick = tick + rules.upgrades.trap.single_reactivation.reactivation_delay_ticks;
                    auto geometry = ExplosionGeometry(area.effect_radius);
                    geometry.start_tick = tick;
                    geometry.end_tick = area.next_tick;
                    geometry.source_id = area.id.value;
                    EmitUpgradeVisual(SkillKind::Trap, 3,
                                      UpgradeVisualStage::Resolve, area.position, {},
                                      area.cast_id, area.id.value, geometry);
                }
                else
                {
                    area.dead = true;
                }
                if (pull_displaced)
                {
                    auto geometry = ExplosionGeometry(area.effect_radius);
                    geometry.start_tick = geometry.end_tick = tick;
                    geometry.source_id = area.id.value;
                    EmitUpgradeVisual(SkillKind::Trap, 6,
                                      UpgradeVisualStage::Resolve, area.position, {},
                                      area.cast_id, area.id.value, geometry);
                }
                EmitVfx(DomainSignalKind::TrapTriggered, area.position, {},
                        area.effect_radius, 0.025f, 0, 0, ExplosionGeometry(area.effect_radius));
            }
            continue;
        }
        else if (area.kind == AreaKind::Slow)
        {
            const auto trail_from = Subtract(
                area.position, Multiply(area.direction, area.half_length));
            const auto trail_to = Add(
                area.position, Multiply(area.direction, area.half_length));
            const auto margin=area.radius+MaximumEnemyCollisionRadius(rules);
            CollectEnemies({std::min(trail_from.x,trail_to.x)-margin,std::min(trail_from.y,trail_to.y)-margin},
                {std::max(trail_from.x,trail_to.x)+margin,std::max(trail_from.y,trail_to.y)+margin},
                combat_state->area_query);
            for (const auto row:combat_state->area_query.rows)
            {
                auto enemy=actors->enemies[row];
                if (enemy.life.dead) continue;
                Float2 visibility_origin = area.position;
                bool inside = DistanceSquared(area.position, enemy.motion.position) <=
                              area.radius * area.radius;
                if (area.half_length > 0.0f)
                {
                    const auto segment = Subtract(trail_to, trail_from);
                    const auto offset = Subtract(enemy.motion.position, trail_from);
                    const auto length_squared = LengthSquared(segment);
                    const auto along = length_squared > 0.0f
                        ? std::clamp((offset.x * segment.x + offset.y * segment.y) /
                                         length_squared, 0.0f, 1.0f)
                        : 0.0f;
                    visibility_origin = Add(trail_from, Multiply(segment, along));
                    const auto enemy_radius = enemy.life.boss
                        ? rules.boss_common.collision_radius
                        : rules.enemies[static_cast<std::size_t>(enemy.life.kind)]
                              .collision_radius;
                    inside = SegmentCircle(trail_from, trail_to, enemy.motion.position,
                                           area.radius + enemy_radius);
                }
                if (inside &&
                    SegmentClear2D(visibility_origin, enemy.motion.position, 0.0f,
                                   rules.arena_boundary, ArenaObstacles()))
                {
                    BeginStatusEpisode(enemy, 2, tick);
                    enemy.status.slows.push_back(
                        {area.slow_reduction, tick + area.slow_duration,
                         area.skill, area.source_upgrade, area.source_relic});
                    EmitVfx(DomainSignalKind::SlowApplied, enemy.motion.position, {},
                            enemy.life.boss ? 1.4f : 0.75f, 0.025f);
                    BindStatusVisual(combat_state->domain_signals.back(), enemy,
                                     area.skill, area.source_upgrade, 2);
                    RecordUpgradeEffect(area.skill, area.source_upgrade,
                                        UpgradeEffectMetric::SlowApplications);
                    RecordUpgradeEffect(area.skill, area.source_upgrade,
                                        UpgradeEffectMetric::SlowTargetTicks,
                                        area.slow_duration);
                }
            }
        }
        else
        {
            if (area.skill == SkillKind::ArrowRain &&
                area.origin == EffectOrigin::Original)
            {
                DomainSignalGeometry geometry{};
                geometry.kind = DomainSignalGeometryKind::Circle;
                geometry.radius = area.radius;
                geometry.start_tick = area.active_tick;
                geometry.end_tick = area.expires;
                geometry.source_id = area.id.value;
                EmitVfx(area.trigger_count == 0
                            ? DomainSignalKind::ArrowRainImpact
                            : DomainSignalKind::ArrowRainPulse,
                        area.position, {}, area.radius / 4.0f, 0.12f, 0, 0,
                        geometry);
                if (HasUpgrade(area.upgrade_mask, 8) && area.trigger_count >= 6)
                    RecordUpgradeEffect(area.skill, 7,
                                        UpgradeEffectMetric::DurationTicksAdded,
                                        area.interval);
                auto &runtime = FindOrCreateCastRuntime(area.cast_id, area.skill);
                bool pull_cue_emitted = false;
                CollectEnemies(area.position,area.radius,combat_state->area_query);
                for (const auto row:combat_state->area_query.rows)
                {
                    auto enemy=actors->enemies[row];
                    if (enemy.life.dead || DistanceSquared(area.position, enemy.motion.position) >
                                          area.radius * area.radius ||
                        !SegmentClear2D(area.position, enemy.motion.position, 0.0f,
                                        rules.arena_boundary, ArenaObstacles()))
                    {
                        continue;
                    }
                    if (area.trigger_count == 0 &&
                        HasUpgrade(area.upgrade_mask, 2) && !enemy.life.boss)
                    {
                        const auto delta = Subtract(area.position, enemy.motion.position);
                        const auto before = enemy.motion.position;
                        const auto displacement = Multiply(
                            Normalize(delta), std::min(
                                rules.upgrades.arrow_rain.first_damage_tick_pull.pull_radius,
                                Length(delta)));
                        QueueEnemyDisplacement(enemy, displacement);
                        RecordUpgradeDisplacement(SkillKind::ArrowRain, 1, before,
                                                  Add(before, displacement));
                        if (LengthSquared(displacement) > 0.000001f)
                        {
                            if (!pull_cue_emitted)
                            {
                                EmitVfx(DomainSignalKind::Pull, area.position, {},
                                        area.radius, 0.12f);
                                pull_cue_emitted = true;
                            }
                            DomainSignalGeometry pull_geometry{};
                            pull_geometry.kind = DomainSignalGeometryKind::Line;
                            pull_geometry.width = 0.12f;
                            pull_geometry.source_id = enemy.id.value;
                            const auto destination = Add(before, displacement);
                            pull_geometry.end_position =
                                {destination.x, 1.05f, destination.y};
                            EmitUpgradeVisual(SkillKind::ArrowRain, 2,
                                              UpgradeVisualStage::Resolve, before,
                                              Normalize(displacement), area.cast_id,
                                              enemy.id.value, pull_geometry);
                        }
                    }
                    const auto hit_count = IncrementAreaHit(area.id.value,
                                                            enemy.id.value);
                    if (HasUpgrade(area.upgrade_mask, 4) && !enemy.status.burn)
                        ApplyBurn(enemy, EffectiveAttack(), false, rules.statuses.burn.duration_ticks,
                                  area.skill, 3);
                    const auto damage_source = HasUpgrade(area.upgrade_mask, 8) &&
                                                       area.trigger_count >= 6
                                                   ? std::uint8_t{7}
                                                   : area.source_upgrade;
                    QueueDamage(enemy.id.value, area.damage, area.skill, area.origin,
                               area.cast_id,
                                HasUpgrade(area.upgrade_mask, 3) && hit_count % 2 == 0 ? 3 : 0,
                               false, area.slow_reduction, area.slow_duration,
                               damage_source, area.source_relic);
                    if (HasUpgrade(area.upgrade_mask, 4) && enemy.status.burn &&
                         runtime.spawn_count < rules.upgrades.arrow_rain
                                               .apply_burn_and_create_fire_areas.maximum_areas_per_cast)
                    {
                        pending_areas.push_back(
                            {tick, ScheduledKind::Area, area.skill, enemy.motion.position, {},
                              rules.upgrades.arrow_rain.apply_burn_and_create_fire_areas.damage_multiplier_per_tick,
                              rules.upgrades.arrow_rain.apply_burn_and_create_fire_areas.radius,
                              static_cast<float>(rules.upgrades.arrow_rain.apply_burn_and_create_fire_areas.duration_ticks) / 60.0f,
                              1, 0,
                              EffectOrigin::Derived, area.cast_id, 3});
                        ++runtime.spawn_count;
                    }
                }
                if (HasUpgrade(area.upgrade_mask, 5))
                {
                    const auto &tracking = rules.upgrades.arrow_rain.tracking_arrow_each_damage_tick;
                    if (auto target = NearestEnemy(area.position, tracking.search_radius))
                    {
                        if (auto arrow = FireProjectile(
                                area.skill, area.position,
                                Normalize(Subtract(target->motion.position, area.position)),
                                tracking.damage_multiplier, EffectOrigin::Derived, area.cast_id, 0, true, 4))
                        {
                            arrow->combat.homing = true;
                            arrow->combat.homing_target = target->id.value;
                            EmitUpgradeVisual(SkillKind::ArrowRain, 5,
                                              UpgradeVisualStage::Spawn,
                                              arrow->motion.position,
                                              Normalize(arrow->motion.velocity), area.cast_id,
                                              arrow->id.value);
                        }
                    }
                }
            }
            else
            {
                if (area.half_length <= 0.0f)
                {
                    const auto fire_area = area.applies_burn ||
                        (area.skill == SkillKind::ExplosiveArrow &&
                         area.source_upgrade == 3) ||
                        (area.skill == SkillKind::Trap &&
                         area.source_upgrade == 4) ||
                        (area.skill == SkillKind::ArrowRain &&
                         area.source_upgrade == 3);
                    DomainSignalGeometry geometry{};
                    geometry.kind = DomainSignalGeometryKind::Circle;
                    geometry.radius = area.radius;
                    geometry.start_tick = area.active_tick;
                    geometry.end_tick = area.expires;
                    geometry.source_id = area.id.value;
                    EmitVfx(fire_area ? DomainSignalKind::FireAreaPulse
                                      : DomainSignalKind::DamageAreaPulse,
                            area.position, {}, area.radius / 1.5f, 0.12f, 0, 0,
                            geometry);
                    if (fire_area && area.skill == SkillKind::ExplosiveArrow &&
                        area.source_upgrade == 3 && area.origin == EffectOrigin::Derived)
                    {
                        auto &signal = combat_state->domain_signals.back();
                        signal.upgrade_skill = static_cast<std::uint8_t>(area.skill);
                        signal.upgrade_index = 3;
                        signal.upgrade_stage = UpgradeVisualStage::Resolve;
                        signal.upgrade_cast_id = area.cast_id;
                        signal.upgrade_owner_id = area.id.value;
                    }
                }
                if (area.half_length > 0.0f)
                {
                    const auto from = Subtract(
                        area.position, Multiply(area.direction, area.half_length));
                    const auto to = Add(
                        area.position, Multiply(area.direction, area.half_length));
                    const auto margin=area.radius+MaximumEnemyCollisionRadius(rules);
                    CollectEnemies({std::min(from.x,to.x)-margin,std::min(from.y,to.y)-margin},
                        {std::max(from.x,to.x)+margin,std::max(from.y,to.y)+margin},combat_state->area_query);
                    for (const auto row:combat_state->area_query.rows)
                    {
                        const auto enemy=actors->enemies[row];
                        if (enemy.life.dead) continue;
                        const auto enemy_radius = enemy.life.boss
                                                      ? rules.boss_common.collision_radius
                                                      : rules.enemies[static_cast<std::size_t>(enemy.life.kind)]
                                                            .collision_radius;
                        if (SegmentCircle(from, to, enemy.motion.position,
                                          area.radius + enemy_radius) &&
                            SegmentClear2D(from, enemy.motion.position, 0.0f,
                                           rules.arena_boundary, ArenaObstacles()))
                        {
                            QueueDamage(enemy.id.value, area.damage, area.skill,
                                       area.origin, area.cast_id, 0,
                                       area.applies_burn, area.slow_reduction,
                                       area.slow_duration, area.source_upgrade,
                                       area.source_relic);
                        }
                    }
                }
                else
                {
                    QueueAreaDamage(area.position, area.radius, area.damage, area.skill,
                               area.origin, area.cast_id, 0, area.applies_burn,
                               area.slow_reduction, area.slow_duration,
                               area.source_upgrade, area.source_relic);
                }
            }
        }
        ++area.trigger_count;
        area.next_tick += area.interval;
        if (area.next_tick >= area.expires)
        {
            area.dead = true;
            if (area.skill == SkillKind::ArrowRain &&
                area.origin == EffectOrigin::Original &&
                HasUpgrade(area.upgrade_mask, 8))
            {
                const auto &slow = rules.upgrades.arrow_rain.extend_area_and_leave_slow;
                SpawnArea(AreaKind::Slow, area.skill, area.position, area.radius,
                          0.0f, static_cast<float>(slow.post_area_duration_ticks) / 60.0f,
                          0.0f, EffectOrigin::Derived,
                          area.cast_id, 0, slow.post_area_slow_fraction,
                          static_cast<float>(slow.post_area_slow_duration_ticks) / 60.0f,
                          0.0f, false, 7);
            }
        }
    }
    for (const auto &pending : pending_areas)
    {
        SpawnArea(AreaKind::Damage, pending.skill, pending.position,
                  pending.radius, pending.damage_coefficient, pending.duration,
                  0.0f, pending.origin, pending.cast_id, pending.upgrade_mask,
                  0.0f, 0.0f, 0.0f, false,
                  pending.source_upgrade, pending.source_relic);
    }
}

void GameSimulation::SimulationWorld::ApplyBleed(EnemyRow enemy, float attack, std::uint8_t stacks,
                SkillKind source_skill,
                std::uint8_t source_upgrade,
                std::uint8_t source_relic)
{
    if (stacks != 0 && rules.statuses.bleed.maximum_stacks != 0) BeginStatusEpisode(enemy, 0, tick);
    if (stacks != 0)
    {
        EmitVfx(DomainSignalKind::BleedApplied, enemy.motion.position);
        if (rules.statuses.bleed.maximum_stacks != 0)
            BindStatusVisual(combat_state->domain_signals.back(), enemy,
                             source_skill, source_upgrade, 0);
    }
    RecordUpgradeEffect(source_skill, source_upgrade,
                        UpgradeEffectMetric::BleedStacksApplied, stacks);
    for (std::uint8_t stack = 0; stack < stacks; ++stack)
    {
        BleedEffect effect{attack, tick + rules.statuses.bleed.duration_ticks + 1,
                           tick + rules.statuses.first_tick_delay_ticks, source_skill,
                           source_upgrade, source_relic};
        const auto maximum_stacks = std::min<std::size_t>(
            rules.statuses.bleed.maximum_stacks, enemy.status.bleeds.size());
        if (maximum_stacks == 0) break;
        enemy.status.AddBleed(effect, maximum_stacks);
    }
}

void GameSimulation::SimulationWorld::ApplyBurn(EnemyRow enemy, float attack, bool propagated,
               Tick duration,
               SkillKind source_skill,
               std::uint8_t source_upgrade,
               std::uint8_t source_relic)
{
    if (rules.statuses.burn.maximum_instances == 0) return;
    BeginStatusEpisode(enemy, 1, tick);
    EmitVfx(DomainSignalKind::BurnApplied, enemy.motion.position);
    BindStatusVisual(combat_state->domain_signals.back(), enemy,
                     source_skill, source_upgrade, 1);
    RecordUpgradeEffect(source_skill, source_upgrade,
                        UpgradeEffectMetric::BurnApplications);
    const BurnEffect incoming{attack, tick + duration + 1,
                              tick + rules.statuses.first_tick_delay_ticks,
                              source_skill, propagated, source_upgrade, source_relic};
    if (!enemy.status.burn || incoming.attack_snapshot > enemy.status.burn->attack_snapshot)
    {
        enemy.status.burn = incoming;
    }
    else
    {
        enemy.status.burn->expires = tick + duration + 1;
    }
}

void GameSimulation::SimulationWorld::DamageStatusPhase()
{
    auto status_timing = Measure(SimulationTimingId::StatusUpdate);
    if (tick < actors->player.pickup_reward_until)
        RecordRelicEffect(RelicKind::PickupReward,
                          UpgradeEffectMetric::EffectActiveTicks);
    for (auto enemy : actors->enemies)
    {
        if (enemy.life.dead) continue;
        for (std::size_t index = 0; index < enemy.status.bleed_count;)
        {
            auto &bleed = enemy.status.bleeds[index];
            if (bleed.expires <= tick)
            {
                enemy.status.bleeds[index] =
                    enemy.status.bleeds[--enemy.status.bleed_count];
                continue;
            }
            RecordUpgradeEffect(bleed.source_skill, bleed.source_upgrade,
                                UpgradeEffectMetric::BleedActiveTicks);
            if (bleed.source_relic < kRelicCount)
                RecordRelicEffect(static_cast<RelicKind>(bleed.source_relic),
                                  UpgradeEffectMetric::EffectActiveTicks);
            if (bleed.next_tick <= tick)
            {
                EmitVfx(DomainSignalKind::BleedTicked, enemy.motion.position,
                        {}, 1.0f, 0.3f, 0, enemy.id.value);
                combat_state->domain_signals.back().status_episode_generation =
                    enemy.cold.status_visual_episodes[0].generation;
                QueueDamage(enemy.id.value,
                           RoundFinalDamage(bleed.attack_snapshot *
                                            rules.statuses.bleed.attack_power_multiplier_per_tick,
                                            rules),
                           bleed.source_skill, EffectOrigin::DamageOverTime, 0,
                           0, false, 0.0f, 0,
                           bleed.source_upgrade, bleed.source_relic);
                bleed.next_tick += rules.statuses.bleed.tick_interval_ticks;
            }
            ++index;
        }
        if (enemy.status.burn)
        {
            if (enemy.status.burn->expires <= tick)
            {
                enemy.status.burn.reset();
            }
            else if (enemy.status.burn->next_tick <= tick)
            {
                EmitVfx(DomainSignalKind::BurnTicked, enemy.motion.position,
                        {}, 1.0f, 0.3f, 0, enemy.id.value);
                combat_state->domain_signals.back().status_episode_generation =
                    enemy.cold.status_visual_episodes[1].generation;
                QueueDamage(enemy.id.value,
                           RoundFinalDamage(enemy.status.burn->attack_snapshot *
                                            rules.statuses.burn.attack_power_multiplier_per_tick,
                                            rules),
                           enemy.status.burn->source_skill,
                           EffectOrigin::DamageOverTime, 0, 0, false, 0.0f, 0,
                           enemy.status.burn->source_upgrade,
                           enemy.status.burn->source_relic);
                enemy.status.burn->next_tick += rules.statuses.burn.tick_interval_ticks;
            }
            if (enemy.status.burn)
                RecordUpgradeEffect(enemy.status.burn->source_skill,
                                    enemy.status.burn->source_upgrade,
                                    UpgradeEffectMetric::BurnActiveTicks);
            if (enemy.status.burn && enemy.status.burn->source_relic < kRelicCount)
                RecordRelicEffect(
                    static_cast<RelicKind>(enemy.status.burn->source_relic),
                    UpgradeEffectMetric::EffectActiveTicks);
        }
    }

    status_timing.Finish();
    auto damage_timing = Measure(SimulationTimingId::DamageFlush);
    combat_state->combat.SortByTargetAndSequence();
    const auto sorted_count = combat_state->combat.commands.size();
    std::uint64_t resolved_target{};
    std::optional<EnemyRow> resolved_enemy{};
    for (std::size_t event_index = 0; event_index < combat_state->combat.commands.size(); ++event_index)
    {
        auto event = combat_state->combat.commands[event_index];
        if (event.target == 0)
        {
            event.amount = ApplyIncomingDamageRelics(event.amount);
            auto source_enemy = event.source_enemy;
            if (source_enemy == kNoTelemetrySource)
            {
                if (const auto source = FindEnemy(event.cast_id))
                    source_enemy = static_cast<std::uint8_t>(EnemyTelemetryIndex(*source));
            }
            const auto applied_damage = config.scenario.player_invulnerable
                ? static_cast<std::uint64_t>(event.amount)
                : static_cast<std::uint64_t>(
                      std::min(event.amount, std::max(actors->player.health, 0)));
            if (!config.scenario.player_invulnerable)
                actors->player.health -= event.amount;
            telemetry->damage_taken += event.amount;
            EmitVfx(DomainSignalKind::PlayerDamaged, actors->player.position, {},
                    1.0f, 0.1f);
            if (applied_damage > 0)
            {
                DomainSignalGeometry contact{};
                contact.source_id = event.cast_id;
                contact.start_tick = contact.end_tick = tick;
                EmitVfx(DomainSignalKind::PlayerHitDirection, actors->player.position,
                        event.incoming_direction, 1.0f, 0.1f, 0, 0, contact);
                if (event.shockwave_contact)
                    EmitVfx(DomainSignalKind::BossShockwaveContact, event.impact_position,
                            event.incoming_direction, 1.0f, 0.1f, 0, 0, contact);
            }
            if (source_enemy < kEnemyArchetypeCount)
            {
                if (telemetry->balance_observer.RecordEnemyHit(
                        source_enemy, event.cast_id & 0x00FFFFFFFFFFFFFFull))
                {
                    ++Metrics().enemy_hits[source_enemy];
                }
                Metrics().enemy_damage[source_enemy] += applied_damage;
            }
            DispatchPlayerDamagedRules();
            continue;
        }
        // The initial target groups share a lookup; proc tails retain insertion order.
        if (event_index >= sorted_count || event.target != resolved_target)
        {
            resolved_target = event.target;
            resolved_enemy.reset();
            if (auto found = FindEnemy(event.target)) resolved_enemy.emplace(*found);
        }
        auto enemy = resolved_enemy;
        if (!enemy || enemy->life.dead || enemy->life.health <= 0 ||
            tick < enemy->action.invulnerable_until)
        {
            continue;
        }
        const auto health_before = std::max(enemy->life.health, 0);
        const auto applied_damage = std::min(event.amount, health_before);
        enemy->life.health -= event.amount;
        telemetry->damage_dealt += applied_damage;
        if (event.skill < SkillKind::Count)
        {
            const auto skill_index = static_cast<std::size_t>(event.skill);
            telemetry->damage_by_skill[skill_index] += applied_damage;
            if (enemy->life.boss)
                Metrics().skill_boss_damage[skill_index] += applied_damage;
            ++Metrics().skill_hit_events[skill_index];
            if (event.cast_id != 0)
            {
                if (BalanceObserver::RecordHit(
                        telemetry->balance_observer.player_hit_casts, event.cast_id))
                {
                    ++Metrics().skill_casts_with_hit[skill_index];
                }
            }
            if (event.source_upgrade < kUpgradeCount)
            {
                Metrics().upgrade_damage[skill_index][event.source_upgrade] += applied_damage;
                ++Metrics().upgrade_triggers[skill_index][event.source_upgrade];
            }
            for (std::size_t index = 0; index < event.amplified_count; ++index)
            {
                const auto attributed = static_cast<std::uint64_t>(std::llround(
                    static_cast<double>(applied_damage) *
                    event.amplified_damage[index] /
                    std::max(1, event.amount)));
                RecordUpgradeDamage(event.skill,
                                    event.amplified_upgrades[index], attributed);
            }
        }
        if (event.source_relic < kRelicCount)
        {
            Metrics().relic_damage[event.source_relic] += applied_damage;
            ++Metrics().relic_triggers[event.source_relic];
            if (event.origin == EffectOrigin::DamageOverTime)
            {
                const auto relic = static_cast<RelicKind>(event.source_relic);
                RecordRelicEffect(relic, UpgradeEffectMetric::DamageOverTime,
                                  applied_damage);
                RecordRelicEffect(relic, UpgradeEffectMetric::DamageOverTimeEvents);
            }
        }
        if (event.skill < SkillKind::Count && event.source_upgrade < kUpgradeCount &&
            event.source_relic < kRelicCount)
        {
            const auto relic = static_cast<RelicKind>(event.source_relic);
            RecordUpgradeRelicSynergy(
                event.skill, event.source_upgrade, relic,
                UpgradeRelicSynergyMetric::Damage, applied_damage);
            RecordUpgradeRelicSynergy(
                event.skill, event.source_upgrade, relic,
                UpgradeRelicSynergyMetric::DamageEvents);
        }
        bool pickup_made_lethal{};
        if (tick < actors->player.pickup_reward_until && applied_damage != 0)
        {
            const auto pickup_multiplier =
                1.0 + rules.relics.pickup_reward.attack_power_fraction;
            const auto without_pickup = static_cast<std::uint64_t>(std::llround(
                static_cast<double>(event.amount) / pickup_multiplier));
            const auto applied = static_cast<std::uint64_t>(applied_damage);
            const auto without_pickup_applied = std::min<std::uint64_t>(
                without_pickup, static_cast<std::uint64_t>(health_before));
            const auto incremental = applied - std::min(applied, without_pickup_applied);
            pickup_made_lethal = event.source_relic >= kRelicCount &&
                                 event.amount >= health_before &&
                                 without_pickup < static_cast<std::uint64_t>(health_before);
            Metrics().relic_damage[static_cast<std::size_t>(RelicKind::PickupReward)] += incremental;
            RecordRelicEffect(RelicKind::PickupReward,
                              UpgradeEffectMetric::DamageAmplified, incremental);
            if (event.skill < SkillKind::Count && event.source_upgrade < kUpgradeCount)
                RecordUpgradeRelicSynergy(event.skill, event.source_upgrade,
                                          RelicKind::PickupReward,
                                          UpgradeRelicSynergyMetric::Damage,
                                          incremental);
        }
        if (event.origin == EffectOrigin::Original) Metrics().direct_damage += applied_damage;
        else if (event.origin == EffectOrigin::Derived) Metrics().derived_damage += applied_damage;
        else Metrics().damage_over_time += applied_damage;
        const auto attack_points =
            actors->player.stats[static_cast<std::size_t>(StatKind::AttackPower)];
        if (attack_points != 0)
        {
            const auto multiplier = 1.0 + 0.06 * attack_points;
            const auto without_stat = static_cast<std::uint64_t>(
                std::llround(static_cast<double>(applied_damage) / multiplier));
            Metrics().stat_utility[static_cast<std::size_t>(StatKind::AttackPower)] +=
                static_cast<std::uint64_t>(applied_damage) -
                std::min(static_cast<std::uint64_t>(applied_damage), without_stat);
        }
        enemy->cold.last_damage_skill = event.skill;
        enemy->cold.last_damage_origin = event.origin;
        enemy->cold.last_damage_cast = event.cast_id;
        enemy->cold.last_damage_upgrade = event.source_upgrade;
        enemy->cold.last_damage_relic = pickup_made_lethal
            ? static_cast<std::uint8_t>(RelicKind::PickupReward)
            : event.source_relic;
        DispatchAfterDamageRules(*enemy, event);
        if (event.bleed_stacks)
        {
            auto source_upgrade = event.source_upgrade;
            if (event.skill == SkillKind::BasicAttack) source_upgrade = 3;
            else if (event.skill == SkillKind::PiercingShot) source_upgrade = 2;
            else if (event.skill == SkillKind::MultiShot) source_upgrade = 4;
            else if (event.skill == SkillKind::ChargedShot) source_upgrade = 4;
            else if (event.skill == SkillKind::Trap) source_upgrade = 3;
            else if (event.skill == SkillKind::ArrowRain) source_upgrade = 2;
            Float2 trap_position{};
            std::uint64_t trap_owner_id{};
            bool trap_context_found = false;
            if (event.skill == SkillKind::Trap)
            {
                const DomainSignal *context_signal = nullptr;
                if (event.originating_area_id != 0)
                {
                    for (const auto &signal : combat_state->domain_signals)
                    {
                        if (signal.kind != DomainSignalKind::TrapDamaged ||
                            signal.tick != tick || signal.upgrade_cast_id != event.cast_id ||
                            signal.upgrade_owner_id != event.originating_area_id)
                            continue;
                        context_signal = &signal;
                        break;
                    }
                }
                if (context_signal)
                {
                    trap_position = {context_signal->position.x, context_signal->position.z};
                    trap_owner_id = context_signal->upgrade_owner_id;
                    trap_context_found = true;
                }
            }
            ApplyBleed(*enemy, EffectiveAttack(), event.bleed_stacks, event.skill,
                       source_upgrade, event.source_relic);
            if (event.skill == SkillKind::Trap && source_upgrade == 3 &&
                rules.statuses.bleed.maximum_stacks != 0 && trap_context_found &&
                !std::ranges::any_of(combat_state->domain_signals,
                                     [&](const auto &signal) {
                    return signal.kind == DomainSignalKind::UpgradeVisual &&
                           signal.upgrade_skill ==
                               static_cast<std::uint8_t>(SkillKind::Trap) &&
                           signal.upgrade_index == 3 &&
                           signal.upgrade_stage == UpgradeVisualStage::Resolve &&
                           signal.upgrade_cast_id == event.cast_id &&
                           signal.upgrade_owner_id == trap_owner_id &&
                           signal.tick == tick;
                }))
            {
                EmitUpgradeVisual(SkillKind::Trap, 4,
                                  UpgradeVisualStage::Resolve, trap_position, {},
                                  event.cast_id, trap_owner_id);
            }
        }
        if (event.burn)
        {
            const auto had_bleed = enemy->status.bleed_count != 0;
            auto source_upgrade = event.source_upgrade;
            if (event.skill == SkillKind::BasicAttack) source_upgrade = 4;
            else if (event.skill == SkillKind::ExplosiveArrow) source_upgrade = 3;
            else if (event.skill == SkillKind::Trap) source_upgrade = 4;
            else if (event.skill == SkillKind::ArrowRain) source_upgrade = 3;
            ApplyBurn(*enemy, EffectiveAttack(), false, rules.statuses.burn.duration_ticks, event.skill,
                      source_upgrade, event.source_relic);
            if (had_bleed && event.origin == EffectOrigin::Original)
                DispatchStatusAppliedRules(*enemy, event, source_upgrade);
        }
        if (event.slow_reduction > 0.0f)
        {
            auto source_upgrade = event.source_upgrade;
            if (event.skill == SkillKind::ArrowRain) source_upgrade = 5;
            else if (event.skill == SkillKind::Trap) source_upgrade = 5;
            BeginStatusEpisode(*enemy, 2, tick);
            enemy->status.slows.push_back(
                {event.slow_reduction, tick + event.slow_duration,
                 event.skill, source_upgrade, event.source_relic});
            EmitVfx(DomainSignalKind::SlowApplied, enemy->motion.position, {},
                    enemy->life.boss ? 1.4f : 0.75f, 0.025f);
            BindStatusVisual(combat_state->domain_signals.back(), *enemy,
                             event.skill, source_upgrade, 2);
            RecordUpgradeEffect(event.skill, source_upgrade,
                                UpgradeEffectMetric::SlowApplications);
            RecordUpgradeEffect(event.skill, source_upgrade,
                                UpgradeEffectMetric::SlowTargetTicks,
                                event.slow_duration);
        }
        const auto original_player_hit = event.skill < SkillKind::Count &&
                                         event.origin == EffectOrigin::Original;
        if (original_player_hit && enemy->cold.marked_by_skill != SkillKind::Count &&
            enemy->cold.marked_by_skill != event.skill && tick <= enemy->cold.mark_expires)
        {
            const auto coefficient = enemy->cold.marked_damage_coefficient;
            const auto marked_skill = enemy->cold.marked_by_skill;
            enemy->cold.marked_by_skill = SkillKind::Count;
            enemy->cold.mark_expires = 0;
            EmitVfx(DomainSignalKind::MarkTriggered, enemy->motion.position, {},
                    enemy->life.boss ? 1.4f : 0.75f, 0.025f);
            if (marked_skill == SkillKind::Trap)
            {
                auto &signal = combat_state->domain_signals.back();
                signal.upgrade_skill = static_cast<std::uint8_t>(SkillKind::Trap);
                signal.upgrade_index = 6;
                signal.upgrade_stage = UpgradeVisualStage::Resolve;
                signal.upgrade_cast_id = event.cast_id;
                signal.upgrade_owner_id = enemy->id.value;
            }
            RecordUpgradeEffect(marked_skill, 6,
                                UpgradeEffectMetric::ExplosionsCreated);
             QueueAreaDamage(enemy->motion.position, rules.skills[static_cast<std::size_t>(marked_skill)].area_radius,
                        RoundFinalDamage(EffectiveAttack() * coefficient, rules), marked_skill,
                       EffectOrigin::Derived, event.cast_id, 0, false, 0.0f, 0,
                       6);
        }
    }
    combat_state->combat.Clear();
}

std::uint64_t GameSimulation::SimulationWorld::Heal(std::int32_t amount,
                                                    bool emit_signal)
{
    const auto before = actors->player.health;
    actors->player.health = std::min(actors->player.max_health, actors->player.health + amount);
    const auto applied = static_cast<std::uint64_t>(
        std::max(0, actors->player.health - before));
    telemetry->healing += applied;
    if (applied != 0 && emit_signal)
        EmitVfx(DomainSignalKind::PlayerHealed, actors->player.position);
    return applied;
}

void GameSimulation::SimulationWorld::CancelChargedShot(bool released) noexcept
{
    if (actors->player.charging)
    {
        EmitSignal(DomainSignalKind::ChargedShotEnded, actors->player.position,
                   static_cast<std::uint8_t>(SkillKind::ChargedShot));
        if (!released)
        {
            DomainSignalGeometry attachment{};
            attachment.kind = DomainSignalGeometryKind::EntityAttachment;
            attachment.radius = rules.stats.player_collision_radius;
            attachment.source_id = kPlayerRenderId;
            attachment.start_tick = actors->player.charge_start;
            attachment.end_tick = tick;
            EmitVfx(DomainSignalKind::ChargedShotCancelled, actors->player.position,
                    actors->player.aim, 1.0f, 1.05f,
                    static_cast<std::uint8_t>(SkillKind::ChargedShot),
                    kPlayerRenderId, attachment);
        }
    }
    actors->player.charging = false;
    actors->player.charging_skill = SkillKind::Count;
    actors->player.charging_slot = 0xFF;
    actors->player.charge_cast_id = 0;
}

std::uint32_t GameSimulation::SimulationWorld::EnemyExperience(ConstEnemyRow enemy) const noexcept
{
    if (enemy.life.boss)
    {
        return rules.progression.boss_xp[static_cast<std::size_t>(*enemy.life.boss)];
    }
    return rules.progression.enemy_xp[static_cast<std::size_t>(enemy.life.kind)];
}

void GameSimulation::SimulationWorld::HandleEnemyDeath(EnemyRow enemy)
{
    EmitVfx(DomainSignalKind::EnemyDied, enemy.motion.position,
            enemy.action.attacking ? enemy.action.locked_aim : enemy.motion.velocity,
            enemy.life.boss ? 1.5f : 1.0f, 0.3f,
            static_cast<std::uint8_t>(enemy.life.kind), enemy.life.boss ? 0 : enemy.id.value);
    if (enemy.life.boss)
    {
        EmitVfx(DomainSignalKind::BossDied, enemy.motion.position, {0.0f, 1.0f}, 1.0f, 0.0f,
                static_cast<std::uint8_t>(*enemy.life.boss), enemy.id.value);
        combat_state->domain_signals.back().status_episode_generation = enemy.action.phase2_started;
        assert(combat_state->boss_death_visual_count < combat_state->boss_death_visuals.size());
        if (combat_state->boss_death_visual_count < combat_state->boss_death_visuals.size())
        {
            auto &capture = combat_state->boss_death_visuals[combat_state->boss_death_visual_count++];
            capture = {};
            capture.sequence = event_sequence;
            capture.tick = tick;
            capture.enemy = EnemyReadView(enemy);
            const auto action = std::find_if(combat_state->boss_actions.begin(), combat_state->boss_actions.end(),
                [&](const auto &value) { return value.boss_id == enemy.id.value; });
            if (action != combat_state->boss_actions.end())
            {
                capture.has_action = true;
                capture.action = BossActionReadView(*action);
            }
        }
    }
    ++telemetry->kills;
    const auto enemy_index = EnemyTelemetryIndex(enemy);
    ++Metrics().enemy_killed[enemy_index];
    Metrics().enemy_lifetime_ticks[enemy_index] += tick - enemy.life.spawned_tick;
    if (enemy.cold.last_damage_skill < SkillKind::Count)
        ++Metrics().skill_kills[static_cast<std::size_t>(enemy.cold.last_damage_skill)];
    if (enemy.cold.last_damage_relic < kRelicCount)
    {
        ++Metrics().relic_kills[enemy.cold.last_damage_relic];
        if (enemy.cold.last_damage_origin == EffectOrigin::DamageOverTime)
            RecordRelicEffect(static_cast<RelicKind>(enemy.cold.last_damage_relic),
                              UpgradeEffectMetric::DamageOverTimeKills);
    }
    RecordUpgradeEffect(enemy.cold.last_damage_skill, enemy.cold.last_damage_upgrade,
                        UpgradeEffectMetric::Kills);
    CastRuntime *runtime{};
    if (enemy.cold.last_damage_cast != 0)
    {
        const auto iterator = std::ranges::find(combat_state->cast_runtimes,
                                                enemy.cold.last_damage_cast,
                                                &CastRuntime::cast_id);
        if (iterator != combat_state->cast_runtimes.end()) runtime = &*iterator;
    }
    const auto xp = EnemyExperience(enemy);
    if (xp)
    {
        ++Metrics().pickup_drop_attempts[static_cast<std::size_t>(PickupKind::Experience)];
        SpawnPickup(PickupKind::Experience, enemy.motion.position, xp);
    }
    if (enemy.life.boss)
    {
        if (*enemy.life.boss != BossKind::Final &&
            config.scenario.progression_enabled)
        {
            ++Metrics().pickup_drop_attempts[
                static_cast<std::size_t>(PickupKind::RelicChest)];
            SpawnPickup(PickupKind::RelicChest, enemy.motion.position,
                        rules.relic_drop.guaranteed_boxes_per_mid_boss, true);
            Heal(RoundDamage(actors->player.max_health * rules.relic_drop
                                           .mid_boss_maximum_hp_heal_fraction));
        }
    }
    else
    {
        if (runtime)
        {
            ++runtime->kills;
            if (runtime->skill == SkillKind::ChargedShot && runtime->full_charge &&
                HasUpgrade(runtime->upgrade_mask, 8) &&
                runtime->kills >= rules.upgrades.charged_shot.full_charge_multi_kill_cooldown_refund
                                   .minimum_normal_enemy_kills && !runtime->refund_triggered)
            {
                auto &cooldown = actors->player.cooldowns[
                    static_cast<std::size_t>(SkillKind::ChargedShot) - 1];
                const auto before = cooldown;
                cooldown -= static_cast<Tick>(cooldown * rules.upgrades.charged_shot
                                                   .full_charge_multi_kill_cooldown_refund
                                                   .current_cooldown_refund_fraction);
                const auto saved = before - cooldown;
                RecordUpgradeEffect(SkillKind::ChargedShot, 7,
                                    UpgradeEffectMetric::CooldownTicksSaved,
                                    saved);
                if (saved > 0)
                {
                    EmitUpgradeVisual(SkillKind::ChargedShot, 8,
                                      UpgradeVisualStage::Resolve,
                                      actors->player.position, actors->player.aim,
                                      runtime->cast_id, kPlayerRenderId);
                }
                runtime->refund_triggered = true;
            }
            if (runtime->skill == SkillKind::RicochetArrow &&
                HasUpgrade(runtime->upgrade_mask, 5) &&
                enemy.cold.last_damage_origin == EffectOrigin::Original &&
                runtime->spawn_count < rules.upgrades.ricochet_arrow.kill_small_arrows
                                             .maximum_arrows_per_cast)
            {
                std::array<std::uint64_t, 4> excluded{enemy.id.value};
                const auto &small = rules.upgrades.ricochet_arrow.kill_small_arrows;
                std::uint64_t first_small_arrow_id{};
                Float2 first_small_arrow_position{};
                Float2 first_small_arrow_velocity{};
                for (std::size_t index = 0; index < small.arrows_per_kill; ++index)
                {
                    if (runtime->spawn_count >= small.maximum_arrows_per_cast) break;
                    auto target = NearestEnemy(
                        enemy.motion.position, small.search_radius,
                        std::span(excluded.data(), index + 1));
                    if (!target) break;
                    if (auto arrow = FireProjectile(
                            SkillKind::RicochetArrow, enemy.motion.position,
                            Normalize(Subtract(target->motion.position, enemy.motion.position)),
                             small.damage_multiplier, EffectOrigin::Derived, runtime->cast_id, 0,
                            true, 4))
                    {
                        arrow->combat.homing = true;
                        arrow->combat.homing_target = target->id.value;
                        excluded[index + 1] = target->id.value;
                        ++runtime->spawn_count;
                        if (first_small_arrow_id == 0)
                        {
                            first_small_arrow_id = arrow->id.value;
                            first_small_arrow_position = arrow->motion.position;
                            first_small_arrow_velocity = arrow->motion.velocity;
                        }
                    }
                }
                if (first_small_arrow_id != 0)
                    EmitUpgradeVisual(SkillKind::RicochetArrow, 5,
                                      UpgradeVisualStage::Spawn,
                                      first_small_arrow_position,
                                      Normalize(first_small_arrow_velocity),
                                      runtime->cast_id, first_small_arrow_id);
            }
            if (runtime->skill == SkillKind::RicochetArrow &&
                enemy.cold.last_damage_origin == EffectOrigin::Original &&
                HasUpgrade(runtime->upgrade_mask, 6) &&
                !runtime->refund_triggered)
            {
                const auto &chain = rules.upgrades.ricochet_arrow.original_ricochet_kill_new_chain;
                if (auto target = NearestEnemy(enemy.motion.position, chain.search_radius))
                {
                    if (auto arrow = FireProjectile(
                            SkillKind::RicochetArrow, enemy.motion.position,
                             Normalize(Subtract(target->motion.position, enemy.motion.position)),
                               chain.damage_multiplier, EffectOrigin::Derived, runtime->cast_id, 0, true, 5))
                    {
                        arrow->combat.homing = true;
                        arrow->combat.homing_target = target->id.value;
                        arrow->combat.bounce_remaining = static_cast<std::uint8_t>(chain.new_chain_targets - 1);
                        runtime->refund_triggered = true;
                        EmitUpgradeVisual(SkillKind::RicochetArrow, 6,
                                          UpgradeVisualStage::Spawn, arrow->motion.position,
                                          Normalize(arrow->motion.velocity), runtime->cast_id,
                                          arrow->id.value);
                    }
                }
            }
            if (runtime->skill == SkillKind::ArrowRain &&
                HasUpgrade(runtime->upgrade_mask, 7) &&
                enemy.cold.last_damage_origin == EffectOrigin::Original &&
                runtime->transfer_count < rules.upgrades.arrow_rain.area_kill_arrow_outside
                                             .maximum_triggers_per_cast)
            {
                const auto source = std::ranges::find_if(actors->areas, [&](const AreaActor &area) {
                    return !area.dead && area.cast_id == runtime->cast_id &&
                           area.skill == SkillKind::ArrowRain &&
                           area.origin == EffectOrigin::Original;
                });
                if (source != actors->areas.end())
                {
                    std::optional<EnemyRow> target{};
                    auto best = std::numeric_limits<float>::max();
                    for (auto candidate : actors->enemies)
                    {
                        if (candidate.life.dead ||
                            DistanceSquared(source->position, candidate.motion.position) <=
                                source->radius * source->radius)
                        {
                            continue;
                        }
                        const auto distance = DistanceSquared(enemy.motion.position,
                                                              candidate.motion.position);
                        if (distance < best)
                        {
                            target.emplace(candidate);
                            best = distance;
                        }
                    }
                    if (target)
                    {
                        if (auto arrow = FireProjectile(
                                SkillKind::ArrowRain, enemy.motion.position,
                                 Normalize(Subtract(target->motion.position, enemy.motion.position)),
                                   rules.upgrades.arrow_rain.area_kill_arrow_outside.damage_multiplier,
                                   EffectOrigin::Derived, runtime->cast_id, 0, true, 6))
                        {
                            arrow->combat.homing = true;
                            arrow->combat.homing_target = target->id.value;
                            ++runtime->transfer_count;
                            EmitUpgradeVisual(SkillKind::ArrowRain, 7,
                                              UpgradeVisualStage::Spawn, arrow->motion.position,
                                              Normalize(arrow->motion.velocity), runtime->cast_id,
                                              arrow->id.value);
                        }
                    }
                }
            }
            if (runtime->skill == SkillKind::Trap &&
                HasUpgrade(runtime->upgrade_mask, 8) &&
                enemy.cold.last_damage_origin == EffectOrigin::Original &&
                runtime->spawn_count < rules.upgrades.trap.trap_kill_small_trap
                                             .maximum_small_traps_per_cast)
            {
                const auto &small = rules.upgrades.trap.trap_kill_small_trap;
                if (auto target = NearestEnemy(
                        enemy.motion.position, std::numeric_limits<float>::max()))
                {
                    if (auto *small_trap = SpawnArea(AreaKind::Trap, SkillKind::Trap, target->motion.position,
                              small.detection_radius, small.damage_multiplier,
                              static_cast<float>(small.active_duration_ticks) / 60.0f,
                              static_cast<float>(small.activation_delay_ticks) / 60.0f,
                              EffectOrigin::Derived, runtime->cast_id, 0,
                              0.0f, 0.0f, small.explosion_radius, false, 7))
                    {
                        EmitUpgradeVisual(SkillKind::Trap, 8,
                                          UpgradeVisualStage::Spawn,
                                          small_trap->position, {}, runtime->cast_id,
                                          small_trap->id.value);
                    }
                    ++runtime->spawn_count;
                }
            }
        }
        ++progression->normal_chest_kills;
        ++Metrics().pickup_drop_attempts[static_cast<std::size_t>(PickupKind::Heal)];
        ++Metrics().pickup_drop_attempts[static_cast<std::size_t>(PickupKind::Magnet)];
        const auto utility_drop = [&](std::uint32_t &misses,
                                      std::uint64_t purpose,
                                      float chance_multiplier) {
            const auto chance = std::min(
                1.0f, (rules.growth.utility_pickup_base_chance +
                       rules.growth.utility_pickup_miss_increment * misses) *
                          chance_multiplier);
            const auto dropped = RandomUnit(enemy.cold.random_key, purpose) < chance;
            misses = dropped ? 0 : misses + 1;
            return dropped;
        };
        if (RandomUnit(enemy.cold.random_key, 0x4845414Cull) <
            rules.relic_drop.healing_pickup_probability)
        {
            progression->heal_pickup_misses = 0;
            SpawnPickup(PickupKind::Heal, enemy.motion.position,
                        RoundDamage(actors->player.max_health * rules.relic_drop
                                                       .healing_pickup_maximum_hp_heal_fraction));
        }
        else
            ++progression->heal_pickup_misses;
        if (utility_drop(progression->magnet_pickup_misses, 0x4D41474E4554ull,
                         rules.growth.magnet_pickup_chance_multiplier))
        {
            SpawnPickup(PickupKind::Magnet, enemy.motion.position, 1);
        }
        if (config.scenario.progression_enabled &&
            (!rules.relic_drop.stop_normal_box_rolls_after_all_acquired ||
             actors->player.relic_mask != kAllRelicsMask))
        {
            ++Metrics().pickup_drop_attempts[
                static_cast<std::size_t>(PickupKind::RelicChest)];
            const auto chance = std::min(
                rules.relic_drop.normal_enemy_probability_cap,
                rules.relic_drop.normal_enemy_base_probability +
                          rules.relic_drop.normal_enemy_probability_increment_per_kill * progression->normal_chest_kills);
            if (RandomUnit(enemy.cold.random_key, 0x4348455354ull) < chance)
            {
                SpawnPickup(PickupKind::RelicChest, enemy.motion.position, 1);
                progression->normal_chest_kills = 0;
            }
        }
    }
    DispatchEnemyKilledRules(enemy, runtime);
}

void GameSimulation::SimulationWorld::DeathDropPhase()
{
    bool final_dead{};
    for (auto enemy : actors->enemies)
    {
        if (!enemy.life.dead && enemy.life.health <= 0)
        {
            enemy.life.dead = true;
            HandleEnemyDeath(enemy);
            final_dead |= enemy.life.boss && *enemy.life.boss == BossKind::Final;
        }
        if (enemy.life.boss && *enemy.life.boss == BossKind::Final && enemy.action.final_phase == 1 &&
            enemy.life.health > 0 &&
            enemy.life.health <= enemy.life.max_health * rules.bosses[static_cast<std::size_t>(
                BossKind::Final)].phase_transition.hp_fraction)
        {
            const auto &transition = rules.bosses[static_cast<std::size_t>(
                BossKind::Final)].phase_transition;
            enemy.action.final_phase = 2;
            enemy.action.phase2_started = tick;
            enemy.action.invulnerable_until = tick + transition.invulnerability_ticks;
            enemy.action.pattern_ready = enemy.action.invulnerable_until;
            enemy.action.dash_until = 0;
            enemy.action.dash_started = 0;
            enemy.motion.velocity = {};
            std::erase_if(combat_state->boss_actions, [&](const BossAction &action) {
                return action.boss_id == enemy.id.value;
            });
            if (combat_state->boss_actions.empty())
                combat_state->next_boss_schedule_sequence = 0;
            if (transition.remove_enemy_projectiles)
                actors->projectiles.EraseIf([](ConstProjectileRow projectile) {
                    return !projectile.combat.player_owned;
                });
            if (transition.remove_enemy_areas)
                std::erase_if(actors->areas, [](const AreaActor &area) {
                    return area.kind == AreaKind::EnemyDamage;
                });
            EmitVfx(DomainSignalKind::BossPhaseChanged, enemy.motion.position,
                    {}, 1.0f, 1.25f, 0, enemy.id.value);
            combat_state->domain_signals.back().status_episode_generation = enemy.action.phase2_started;
        }
    }
    if (final_dead)
    {
        session_phase = SessionPhase::Victory;
        EmitSignal(DomainSignalKind::SessionVictory, actors->player.position);
    }
    else if (actors->player.health <= 0)
    {
        const auto revived = DispatchPlayerDeathRules();
        if (!revived)
        {
            EmitVfx(DomainSignalKind::PlayerDied, actors->player.position, {},
                    1.0f, 0.2f);
            session_phase = SessionPhase::Defeat;
            EmitSignal(DomainSignalKind::SessionDefeat, actors->player.position);
        }
    }
}

void GameSimulation::SimulationWorld::HandleRadialBasicAttack(Tick release_ticks, std::uint64_t cast_id)
{
    const auto &relic = rules.relics.radial_basic_attack;
    if (actors->player.basic_sequence % relic.cadence_interval != 0) return;
    EmitVfx(DomainSignalKind::RadialArrowsCast, actors->player.position,
            actors->player.aim, 1.0f, 0.15f);
    RecordRelicEffect(RelicKind::RadialBasicAttack,
                      UpgradeEffectMetric::Activations);
    RecordRelicEffect(RelicKind::RadialBasicAttack,
                      UpgradeEffectMetric::ProjectilesCreated,
                      relic.direction_count);
    for (std::uint32_t index = 0; index < relic.direction_count; ++index)
    {
        ScheduleAction({tick + release_ticks, ScheduledKind::Projectile,
                  SkillKind::BasicAttack, actors->player.position,
                  Rotate({0.0f, 1.0f}, index * 360.0f / relic.direction_count),
                  relic.damage_multiplier, 0.0f, 0.0f, 1, 0,
                  EffectOrigin::Derived, cast_id, kNoTelemetrySource,
                  static_cast<std::uint8_t>(RelicKind::RadialBasicAttack)});
    }
}

void GameSimulation::SimulationWorld::HandleMovementEcho(Tick release_ticks, std::uint64_t cast_id)
{
    const auto &relic = rules.relics.movement_echo;
    if (actors->player.movement_since_echo < relic.required_distance) return;
    // CastAttack precedes Movement: tick-age is a completed movement sample
    // for positive ages. Age zero uses the current pre-movement position.
    const auto history_tick = tick > relic.position_history_age_ticks
                                  ? tick - relic.position_history_age_ticks : 0;
    const auto echo_position = relic.position_history_age_ticks == 0
                                  ? actors->player.position
                                  : actors->player_position_history[
                                        history_tick % actors->player_position_history.size()];
    RecordRelicEffect(RelicKind::MovementEcho, UpgradeEffectMetric::Activations);
    RecordRelicEffect(RelicKind::MovementEcho,
                      UpgradeEffectMetric::ProjectilesCreated);
    ScheduleAction({tick + release_ticks, ScheduledKind::Projectile,
              SkillKind::BasicAttack, echo_position, actors->player.aim,
              relic.damage_multiplier, 0.0f, 0.0f, 1, 0,
              EffectOrigin::Derived, cast_id, kNoTelemetrySource,
              static_cast<std::uint8_t>(RelicKind::MovementEcho)});
    EmitVfx(DomainSignalKind::AfterimageArrowFired, echo_position,
            actors->player.aim, 1.0f, 0.3f,
            static_cast<std::uint8_t>(RelicKind::MovementEcho));
    actors->player.movement_since_echo = 0.0f;
}

void GameSimulation::SimulationWorld::HandleAlternatingSkills(SkillKind skill, std::size_t cooldown_index)
{
    const auto &relic = rules.relics.alternating_skills;
    if (actors->player.last_active == SkillKind::Count || actors->player.last_active == skill ||
        tick - actors->player.last_active_tick > relic.window_ticks) return;
    const auto before = actors->player.cooldowns[cooldown_index];
    actors->player.cooldowns[cooldown_index] -= static_cast<Tick>(
        actors->player.cooldowns[cooldown_index] * relic.cooldown_refund_fraction);
    EmitSignal(DomainSignalKind::CooldownRefunded, actors->player.position,
               static_cast<std::uint8_t>(RelicKind::AlternatingSkills));
    RecordRelicEffect(RelicKind::AlternatingSkills,
                      UpgradeEffectMetric::Activations);
    RecordRelicEffect(RelicKind::AlternatingSkills,
                      UpgradeEffectMetric::CooldownTicksSaved,
                      before - actors->player.cooldowns[cooldown_index]);
}

void GameSimulation::SimulationWorld::HandleDamageKnockback()
{
    if (tick < actors->player.damage_relic_ready) return;
    const auto &relic = rules.relics.damage_knockback;
    EmitVfx(DomainSignalKind::DamagePush, actors->player.position, {}, 1.0f, 0.15f);
    std::uint64_t affected{};
    for (auto enemy : actors->enemies)
    {
        if (enemy.life.dead || enemy.life.boss ||
            DistanceSquared(actors->player.position, enemy.motion.position) >
                relic.radius * relic.radius) continue;
        const auto direction = Normalize(Subtract(enemy.motion.position, actors->player.position));
        const auto displacement = Multiply(direction, relic.push_distance);
        QueueEnemyDisplacement(enemy, displacement);
        BeginStatusEpisode(enemy, 2, tick);
        enemy.status.slows.push_back(
            {relic.slow_fraction, tick + relic.slow_duration_ticks,
             SkillKind::Count, kNoEffectSource,
             static_cast<std::uint8_t>(RelicKind::DamageKnockback)});
        RecordRelicEffect(
            RelicKind::DamageKnockback,
            UpgradeEffectMetric::DisplacementMillimetres,
            static_cast<std::uint64_t>(std::llround(
                Length(displacement) * 1000.0f)));
        ++affected;
    }
    RecordRelicEffect(RelicKind::DamageKnockback,
                      UpgradeEffectMetric::Activations);
    RecordRelicEffect(RelicKind::DamageKnockback,
                      UpgradeEffectMetric::SlowApplications, affected);
    RecordRelicEffect(RelicKind::DamageKnockback,
                      UpgradeEffectMetric::SlowTargetTicks,
                      affected * relic.slow_duration_ticks);
    actors->player.damage_relic_ready = tick + relic.cooldown_ticks;
}

void GameSimulation::SimulationWorld::HandleCombatHitChain(const DamageCommand &event)
{
    if (event.skill >= SkillKind::Count ||
        event.origin != EffectOrigin::Original) return;
    const auto &relic = rules.relics.combat_hit_chain;
    ++actors->player.combat_hit_progress;
    if (actors->player.combat_hit_progress < relic.direct_hits_per_trigger) return;
    actors->player.combat_hit_progress -= relic.direct_hits_per_trigger;
    RecordRelicEffect(RelicKind::CombatHitChain,
                      UpgradeEffectMetric::Activations);
    std::vector<std::uint64_t> selected;
    selected.reserve(relic.maximum_targets);
    auto &query = combat_state->chain_query;
    CollectEnemies(actors->player.position, relic.search_radius, query);
    while (selected.size() < relic.maximum_targets)
    {
        std::optional<EnemyRow> target{};
        auto best_distance = relic.search_radius * relic.search_radius;
        for (const auto row : query.rows)
        {
            auto candidate = actors->enemies[row];
            if (candidate.life.dead || candidate.life.health <= 0) continue;
            const auto already_selected =
                std::ranges::find(selected, candidate.id.value) != selected.end();
            const auto distance = DistanceSquared(actors->player.position,
                                                   candidate.motion.position);
            if (!already_selected && distance <= best_distance &&
                SegmentClear2D(actors->player.position, candidate.motion.position, 0.0f,
                               rules.arena_boundary, ArenaObstacles()))
            {
                target.emplace(candidate);
                best_distance = distance;
            }
        }
        if (!target) break;
        selected.push_back(target->id.value);
        QueueDamage(target->id.value,
                   RoundFinalDamage(EffectiveAttack() * relic.damage_multiplier, rules),
                   SkillKind::Count, EffectOrigin::Derived, event.cast_id, 0,
                   false, 0.0f, 0, kNoTelemetrySource,
                   static_cast<std::uint8_t>(RelicKind::CombatHitChain));
        EmitVfx(DomainSignalKind::CombatChainHit, target->motion.position,
                {}, 0.5f, 0.15f);
    }
    RecordRelicEffect(RelicKind::CombatHitChain,
                      UpgradeEffectMetric::ExtraTargetsHit, selected.size());
}

void GameSimulation::SimulationWorld::HandleProjectileCadenceReward()
{
    const auto &relic = rules.relics.projectile_cadence_reward;
    if (++actors->player.projectile_cadence_progress < relic.hits_per_trigger) return;
    actors->player.projectile_cadence_progress -= relic.hits_per_trigger;
    std::uint64_t saved{};
    for (auto &cooldown : actors->player.cooldowns)
    {
        const auto reduction = std::min(cooldown, relic.cooldown_reduction_ticks);
        cooldown -= reduction;
        saved += reduction;
    }
    RecordRelicEffect(RelicKind::ProjectileCadenceReward,
                      UpgradeEffectMetric::Activations);
    EmitSignal(DomainSignalKind::RelicTriggered, actors->player.position,
               static_cast<std::uint8_t>(RelicKind::ProjectileCadenceReward));
    RecordRelicEffect(RelicKind::ProjectileCadenceReward,
                      UpgradeEffectMetric::CooldownTicksSaved, saved);
}

std::int32_t GameSimulation::SimulationWorld::ApplyIncomingDamageRelics(
    std::int32_t amount)
{
    constexpr auto hook_mask = RelicBit(RelicKind::PreDamageGuard) |
                               RelicBit(RelicKind::LowHealthSurvival);
    if ((actors->player.relic_mask & hook_mask) == 0) return amount;
    const auto reduce = [&](RelicKind relic, float reduction, Tick cooldown, Tick &ready) {
        if (tick < ready) return;
        const auto before_reduction = amount;
        amount = RoundFinalDamage(amount * (1.0f - reduction), rules);
        ready = tick + cooldown;
        RecordRelicEffect(relic, UpgradeEffectMetric::Activations);
        EmitSignal(DomainSignalKind::RelicTriggered, actors->player.position,
                   static_cast<std::uint8_t>(relic));
        RecordRelicEffect(relic, UpgradeEffectMetric::DamagePrevented,
                          static_cast<std::uint64_t>(before_reduction - amount));
    };
    if (HasRelic(actors->player.relic_mask, RelicKind::PreDamageGuard))
        reduce(RelicKind::PreDamageGuard,
               rules.relics.pre_damage_guard.damage_reduction_fraction,
               rules.relics.pre_damage_guard.cooldown_ticks,
               actors->player.pre_damage_guard_ready);
    if (HasRelic(actors->player.relic_mask, RelicKind::LowHealthSurvival) &&
        !(actors->player.health > actors->player.max_health *
                                 rules.relics.low_health_survival.health_threshold_fraction))
        reduce(RelicKind::LowHealthSurvival,
               rules.relics.low_health_survival.damage_reduction_fraction,
               rules.relics.low_health_survival.cooldown_ticks,
               actors->player.low_health_survival_ready);
    return amount;
}

void GameSimulation::SimulationWorld::HandleSlowSynergy(
    EnemyRow enemy, const DamageCommand &event)
{
    if (event.origin != EffectOrigin::Original || tick < enemy.cold.slow_synergy_ready ||
        !std::ranges::any_of(enemy.status.slows,
                             [this](const SlowEffect &slow) { return slow.expires > tick; }))
        return;
    const auto &relic = rules.relics.slow_synergy;
    enemy.cold.slow_synergy_ready = tick + relic.per_target_cooldown_ticks;
    QueueDamage(enemy.id.value,
                RoundFinalDamage(EffectiveAttack() * relic.damage_multiplier, rules),
                event.skill, EffectOrigin::Derived, event.cast_id, 0, false,
                0.0f, 0, event.source_upgrade,
                static_cast<std::uint8_t>(RelicKind::SlowSynergy));
    RecordRelicEffect(RelicKind::SlowSynergy, UpgradeEffectMetric::Activations);
    EmitSignal(DomainSignalKind::RelicTriggered, enemy.motion.position,
               static_cast<std::uint8_t>(RelicKind::SlowSynergy));
}

void GameSimulation::SimulationWorld::HandleAreaResonance(
    EnemyRow enemy, const DamageCommand &event)
{
    if (event.origin != EffectOrigin::Original || tick < actors->player.area_resonance_ready ||
        (event.skill != SkillKind::ArrowRain && event.skill != SkillKind::Trap))
        return;
    const auto &relic = rules.relics.area_resonance;
    actors->player.area_resonance_ready = tick + relic.cooldown_ticks;
    QueueDamage(enemy.id.value,
                RoundFinalDamage(EffectiveAttack() * relic.damage_multiplier, rules),
                event.skill, EffectOrigin::Derived, event.cast_id, 0, false,
                0.0f, 0, event.source_upgrade,
                static_cast<std::uint8_t>(RelicKind::AreaResonance));
    RecordRelicEffect(RelicKind::AreaResonance, UpgradeEffectMetric::Activations);
    EmitSignal(DomainSignalKind::RelicTriggered, enemy.motion.position,
               static_cast<std::uint8_t>(RelicKind::AreaResonance));
}

void GameSimulation::SimulationWorld::HandleBossPressure(
    EnemyRow enemy, const DamageCommand &event)
{
    if (!enemy.life.boss || event.origin != EffectOrigin::Original ||
        tick < enemy.cold.boss_pressure_ready)
        return;
    const auto &relic = rules.relics.boss_pressure;
    enemy.cold.boss_pressure_ready = tick + relic.per_target_cooldown_ticks;
    QueueDamage(enemy.id.value,
                RoundFinalDamage(EffectiveAttack() * relic.damage_multiplier, rules),
                event.skill, EffectOrigin::Derived, event.cast_id, 0, false,
                0.0f, 0, event.source_upgrade,
                static_cast<std::uint8_t>(RelicKind::BossPressure));
    RecordRelicEffect(RelicKind::BossPressure, UpgradeEffectMetric::Activations);
    EmitSignal(DomainSignalKind::RelicTriggered, enemy.motion.position,
               static_cast<std::uint8_t>(RelicKind::BossPressure));
}

void GameSimulation::SimulationWorld::HandleHitStreakReward(
    EnemyRow enemy, const DamageCommand &event)
{
    if (event.skill >= SkillKind::Count || event.origin != EffectOrigin::Original)
        return;
    const auto &relic = rules.relics.hit_streak_reward;
    if (++actors->player.hit_streak_progress < relic.direct_hits_per_trigger) return;
    actors->player.hit_streak_progress -= relic.direct_hits_per_trigger;
    QueueDamage(enemy.id.value,
                RoundFinalDamage(EffectiveAttack() * relic.damage_multiplier, rules),
                event.skill, EffectOrigin::Derived, event.cast_id, 0, false,
                0.0f, 0, event.source_upgrade,
                static_cast<std::uint8_t>(RelicKind::HitStreakReward));
    RecordRelicEffect(RelicKind::HitStreakReward, UpgradeEffectMetric::Activations);
    EmitSignal(DomainSignalKind::RelicTriggered, enemy.motion.position,
               static_cast<std::uint8_t>(RelicKind::HitStreakReward));
}

void GameSimulation::SimulationWorld::HandlePickupReward(PickupKind kind)
{
    if (kind != PickupKind::Experience && kind != PickupKind::Heal) return;
    actors->player.pickup_reward_until =
        std::max(actors->player.pickup_reward_until, tick + rules.relics.pickup_reward.duration_ticks);
    RecordRelicEffect(RelicKind::PickupReward, UpgradeEffectMetric::Activations);
    EmitSignal(DomainSignalKind::RelicTriggered, actors->player.position,
               static_cast<std::uint8_t>(RelicKind::PickupReward));
}

void GameSimulation::SimulationWorld::HandleBleedBurnExplosion(EnemyRow enemy,
                              const DamageCommand &event,
                              std::uint8_t source_upgrade)
{
    if (tick < enemy.cold.bleed_burn_ready) return;
    const auto &relic = rules.relics.bleed_burn_explosion;
    enemy.cold.bleed_burn_ready = tick + relic.per_target_cooldown_ticks;
    QueueAreaDamage(enemy.motion.position, relic.radius,
                RoundFinalDamage(EffectiveAttack() * relic.damage_multiplier, rules),
               event.skill, EffectOrigin::Derived, event.cast_id, 0,
               false, 0.0f, 0, source_upgrade,
               static_cast<std::uint8_t>(RelicKind::BleedBurnExplosion));
    EmitVfx(DomainSignalKind::BleedBurnExploded, enemy.motion.position,
            {}, 1.0f, 0.15f);
    RecordUpgradeRelicSynergy(
        event.skill, source_upgrade, RelicKind::BleedBurnExplosion,
        UpgradeRelicSynergyMetric::Activations);
    std::array<std::array<bool, kUpgradeCount>, kCombatSkillCount> recorded{};
    for (std::size_t bleed = 0; bleed < enemy.status.bleed_count; ++bleed)
    {
        const auto &source = enemy.status.bleeds[bleed];
        if (source.source_skill >= SkillKind::Count ||
            source.source_upgrade >= kUpgradeCount ||
            std::exchange(recorded[static_cast<std::size_t>(source.source_skill)]
                                  [source.source_upgrade], true)) continue;
        RecordUpgradeRelicSynergy(
            source.source_skill, source.source_upgrade,
            RelicKind::BleedBurnExplosion,
            UpgradeRelicSynergyMetric::Activations);
    }
}

void GameSimulation::SimulationWorld::HandleDifferentSkillTracker(EnemyRow enemy,
                                 const DamageCommand &event)
{
    if (event.skill <= SkillKind::BasicAttack ||
        event.skill >= SkillKind::Count ||
        event.origin != EffectOrigin::Original) return;
    const auto &relic = rules.relics.different_skill_tracker;
    if (enemy.cold.last_active_hit != SkillKind::Count &&
        enemy.cold.last_active_hit != event.skill &&
        tick - enemy.cold.last_active_hit_tick <= relic.window_ticks &&
        tick >= enemy.cold.different_skill_ready)
    {
        EmitVfxLine(DomainSignalKind::RelicChainLinked,
                    actors->player.position, enemy.motion.position);
        if (auto arrow = FireProjectile(
                event.skill, actors->player.position,
                Normalize(Subtract(enemy.motion.position, actors->player.position)),
                relic.damage_multiplier, EffectOrigin::Derived,
                event.cast_id, 0, true, kNoTelemetrySource,
                static_cast<std::uint8_t>(RelicKind::DifferentSkillTracker)))
        {
            arrow->combat.homing = true;
            arrow->combat.homing_target = enemy.id.value;
            EmitVfx(DomainSignalKind::TrackingArrowFired, actors->player.position,
                    Normalize(Subtract(enemy.motion.position, actors->player.position)), 1.0f,
                    0.3f,
                    static_cast<std::uint8_t>(RelicKind::DifferentSkillTracker));
            RecordRelicEffect(RelicKind::DifferentSkillTracker,
                              UpgradeEffectMetric::Activations);
            RecordRelicEffect(RelicKind::DifferentSkillTracker,
                              UpgradeEffectMetric::ProjectilesCreated);
        }
        enemy.cold.different_skill_ready = tick + relic.per_target_cooldown_ticks;
    }
    enemy.cold.last_active_hit = event.skill;
    enemy.cold.last_active_hit_tick = tick;
}

void GameSimulation::SimulationWorld::HandleKillCooldownSurge()
{
    const auto &relic = rules.relics.kill_cooldown_surge;
    ++actors->player.kill_cooldown_progress;
    if (actors->player.kill_cooldown_progress < relic.kills_per_trigger) return;
    actors->player.kill_cooldown_progress -= relic.kills_per_trigger;
    std::uint64_t saved{};
    for (auto &cooldown : actors->player.cooldowns)
    {
        const auto before = cooldown;
        cooldown -= std::min<Tick>(cooldown, relic.cooldown_reduction_ticks);
        saved += before - cooldown;
    }
    RecordRelicEffect(RelicKind::KillCooldownSurge,
                      UpgradeEffectMetric::Activations);
    RecordRelicEffect(RelicKind::KillCooldownSurge,
                      UpgradeEffectMetric::CooldownTicksSaved, saved);
    EmitSignal(DomainSignalKind::CooldownSurged, actors->player.position,
               static_cast<std::uint8_t>(RelicKind::KillCooldownSurge));
}

void GameSimulation::SimulationWorld::HandleBasicKillTracker(EnemyRow enemy, CastRuntime *runtime)
{
    if (!runtime || runtime->skill != SkillKind::BasicAttack ||
        enemy.cold.last_damage_origin != EffectOrigin::Original) return;
    const auto &relic = rules.relics.basic_kill_tracker;
    if (runtime->basic_relic_triggers >=
        relic.maximum_triggers_per_attack) return;
    std::optional<EnemyRow> target{};
    auto best = relic.search_radius * relic.search_radius;
    for (auto candidate : actors->enemies)
    {
        if (candidate.life.dead || candidate.id.value == enemy.id.value ||
            combat_state->cast_hit_index.contains(
                std::pair{runtime->cast_id, candidate.id.value})) continue;
        const auto distance = DistanceSquared(enemy.motion.position, candidate.motion.position);
        if (distance < best)
        {
            target.emplace(candidate);
            best = distance;
        }
    }
    if (!target) return;
    if (auto arrow = FireProjectile(
            SkillKind::BasicAttack, enemy.motion.position,
            Normalize(Subtract(target->motion.position, enemy.motion.position)),
            relic.damage_multiplier, EffectOrigin::Derived,
            runtime->cast_id, 0, true, kNoTelemetrySource,
            static_cast<std::uint8_t>(RelicKind::BasicKillTracker)))
    {
        arrow->combat.homing = true;
        arrow->combat.homing_target = target->id.value;
        EmitVfx(DomainSignalKind::TrackingArrowFired, enemy.motion.position,
                Normalize(Subtract(target->motion.position, enemy.motion.position)), 1.0f,
                0.3f,
                static_cast<std::uint8_t>(RelicKind::BasicKillTracker));
        ++runtime->basic_relic_triggers;
        RecordRelicEffect(RelicKind::BasicKillTracker,
                          UpgradeEffectMetric::Activations);
        RecordRelicEffect(RelicKind::BasicKillTracker,
                          UpgradeEffectMetric::ProjectilesCreated);
    }
}

void GameSimulation::SimulationWorld::HandleBleedKillHeal(ConstEnemyRow enemy)
{
    if (enemy.status.bleed_count == 0 || tick < actors->player.bleed_heal_ready) return;
    const auto &relic = rules.relics.bleed_kill_heal;
    const auto healed = Heal(RoundDamage(
        actors->player.max_health * relic.maximum_hp_heal_fraction));
    RecordRelicEffect(RelicKind::BleedKillHeal,
                      UpgradeEffectMetric::Activations);
    RecordRelicEffect(RelicKind::BleedKillHeal,
                      UpgradeEffectMetric::Healing, healed);
    std::array<std::array<bool, kUpgradeCount>, kCombatSkillCount> recorded{};
    for (std::size_t bleed = 0; bleed < enemy.status.bleed_count; ++bleed)
    {
        const auto &source = enemy.status.bleeds[bleed];
        if (source.source_skill >= SkillKind::Count ||
            source.source_upgrade >= kUpgradeCount ||
            std::exchange(recorded[static_cast<std::size_t>(source.source_skill)]
                                  [source.source_upgrade], true)) continue;
        RecordUpgradeRelicSynergy(source.source_skill, source.source_upgrade,
                                  RelicKind::BleedKillHeal,
                                  UpgradeRelicSynergyMetric::Healing, healed);
    }
    actors->player.bleed_heal_ready = tick + relic.internal_cooldown_ticks;
}

void GameSimulation::SimulationWorld::HandleBurnPropagation(ConstEnemyRow enemy)
{
    if (!enemy.status.burn || enemy.status.burn->propagated) return;
    const auto &relic = rules.relics.burn_propagation;
    std::vector<std::uint64_t> excluded{enemy.id.value};
    excluded.reserve(static_cast<std::size_t>(relic.maximum_targets) + 1);
    for (std::uint32_t index = 0; index < relic.maximum_targets; ++index)
    {
        auto target = NearestEnemy(enemy.motion.position, relic.search_radius,
                                    std::span(excluded));
        if (!target) break;
        EmitVfxLine(DomainSignalKind::BurnTransferred,
                    enemy.motion.position, target->motion.position);
        ApplyBurn(*target,
                  enemy.status.burn->attack_snapshot * relic.copied_burn_strength,
                  true,
                  enemy.status.burn->expires > tick
                      ? enemy.status.burn->expires - tick : 1,
                  enemy.status.burn->source_skill,
                  enemy.status.burn->source_upgrade,
                  static_cast<std::uint8_t>(RelicKind::BurnPropagation));
        RecordRelicEffect(RelicKind::BurnPropagation,
                          UpgradeEffectMetric::Activations);
        RecordRelicEffect(RelicKind::BurnPropagation,
                          UpgradeEffectMetric::BurnApplications);
        RecordUpgradeRelicSynergy(
            enemy.status.burn->source_skill,
            enemy.status.burn->source_upgrade,
            RelicKind::BurnPropagation,
            UpgradeRelicSynergyMetric::BurnApplications);
        excluded.push_back(target->id.value);
    }
}

bool GameSimulation::SimulationWorld::HandleOnceRevive()
{
    const auto &relic = rules.relics.once_revive;
    if (actors->player.revives_used >= relic.maximum_triggers_per_session) return false;
    ++actors->player.revives_used;
    actors->player.health = std::max(
        1, RoundDamage(actors->player.max_health * relic.health_fraction));
    actors->player.revive_invulnerable_until = tick + relic.invulnerability_ticks;
    actors->player.revive_invulnerable_started = tick;
    RecordRelicEffect(RelicKind::OnceRevive, UpgradeEffectMetric::Activations);
    RecordRelicEffect(RelicKind::OnceRevive, UpgradeEffectMetric::Healing,
                      static_cast<std::uint64_t>(actors->player.health));
    EmitVfx(DomainSignalKind::PlayerHealed, actors->player.position, {}, 1.5f, 0.5f);
    EmitSignal(DomainSignalKind::PlayerRevived, actors->player.position,
               static_cast<std::uint8_t>(RelicKind::OnceRevive));
    return true;
}

void GameSimulation::SimulationWorld::DispatchBasicAttackRules(Tick release_ticks, std::uint64_t cast_id)
{
    constexpr auto hook_mask = RelicBit(RelicKind::RadialBasicAttack) | RelicBit(RelicKind::MovementEcho);
    if ((actors->player.relic_mask & hook_mask) == 0) return;
    if (HasRelic(actors->player.relic_mask, RelicKind::RadialBasicAttack))
        HandleRadialBasicAttack(release_ticks, cast_id);
    if (HasRelic(actors->player.relic_mask, RelicKind::MovementEcho))
        HandleMovementEcho(release_ticks, cast_id);
}

void GameSimulation::SimulationWorld::DispatchAbilityRules(SkillKind skill, std::size_t cooldown_index)
{
    if (HasRelic(actors->player.relic_mask, RelicKind::AlternatingSkills))
        HandleAlternatingSkills(skill, cooldown_index);
}

void GameSimulation::SimulationWorld::DispatchPlayerDamagedRules()
{
    if (HasRelic(actors->player.relic_mask, RelicKind::DamageKnockback))
        HandleDamageKnockback();
}

void GameSimulation::SimulationWorld::DispatchAfterDamageRules(EnemyRow enemy, const DamageCommand &event)
{
    constexpr auto hook_mask = RelicBit(RelicKind::DifferentSkillTracker) |
                               RelicBit(RelicKind::CombatHitChain) | RelicBit(RelicKind::SlowSynergy) |
                               RelicBit(RelicKind::AreaResonance) | RelicBit(RelicKind::BossPressure) |
                               RelicBit(RelicKind::HitStreakReward);
    if ((actors->player.relic_mask & hook_mask) == 0) return;
    if (HasRelic(actors->player.relic_mask, RelicKind::DifferentSkillTracker))
        HandleDifferentSkillTracker(enemy, event);
    if (HasRelic(actors->player.relic_mask, RelicKind::CombatHitChain))
        HandleCombatHitChain(event);
    if (HasRelic(actors->player.relic_mask, RelicKind::SlowSynergy))
        HandleSlowSynergy(enemy, event);
    if (HasRelic(actors->player.relic_mask, RelicKind::AreaResonance))
        HandleAreaResonance(enemy, event);
    if (HasRelic(actors->player.relic_mask, RelicKind::BossPressure))
        HandleBossPressure(enemy, event);
    if (HasRelic(actors->player.relic_mask, RelicKind::HitStreakReward))
        HandleHitStreakReward(enemy, event);
}

void GameSimulation::SimulationWorld::DispatchStatusAppliedRules(EnemyRow enemy,
                                const DamageCommand &event,
                                std::uint8_t source_upgrade)
{
    if (HasRelic(actors->player.relic_mask, RelicKind::BleedBurnExplosion))
        HandleBleedBurnExplosion(enemy, event, source_upgrade);
}

void GameSimulation::SimulationWorld::DispatchEnemyKilledRules(EnemyRow enemy, CastRuntime *runtime)
{
    constexpr auto hook_mask = RelicBit(RelicKind::BleedKillHeal) | RelicBit(RelicKind::BurnPropagation) |
                               RelicBit(RelicKind::KillCooldownSurge) | RelicBit(RelicKind::BasicKillTracker);
    if ((actors->player.relic_mask & hook_mask) == 0) return;
    if (HasRelic(actors->player.relic_mask, RelicKind::BleedKillHeal))
        HandleBleedKillHeal(enemy);
    if (HasRelic(actors->player.relic_mask, RelicKind::BurnPropagation))
        HandleBurnPropagation(enemy);
    if (HasRelic(actors->player.relic_mask, RelicKind::KillCooldownSurge))
        HandleKillCooldownSurge();
    if (HasRelic(actors->player.relic_mask, RelicKind::BasicKillTracker))
        HandleBasicKillTracker(enemy, runtime);
}

bool GameSimulation::SimulationWorld::DispatchPlayerDeathRules()
{
    return HasRelic(actors->player.relic_mask, RelicKind::OnceRevive) && HandleOnceRevive();
}

} // namespace hs
