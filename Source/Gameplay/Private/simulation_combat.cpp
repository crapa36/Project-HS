#include "simulation_world.hpp"

namespace hs
{

using namespace gameplay_detail;

float GameSimulation::SimulationWorld::EffectiveAttack() const noexcept
{
    return data.player_attack *
           (1.0f + 0.06f * player.stats[static_cast<std::size_t>(StatKind::AttackPower)]);
}

float GameSimulation::SimulationWorld::EffectiveAttackSpeed() const noexcept
{
    return data.player_attack_speed *
           (1.0f + kAttackSpeedPerPoint *
                       player.stats[static_cast<std::size_t>(StatKind::AttackSpeed)]);
}

void GameSimulation::SimulationWorld::RecordUpgradeEffect(SkillKind skill, std::uint8_t upgrade,
                         UpgradeEffectMetric metric,
                         std::uint64_t amount) noexcept
{
    if (skill >= SkillKind::Count || upgrade >= kUpgradeCount || amount == 0) return;
    balance.upgrade_effects[static_cast<std::size_t>(skill)][upgrade]
                           [static_cast<std::size_t>(metric)] += amount;
}

void GameSimulation::SimulationWorld::RecordRelicEffect(RelicKind relic, UpgradeEffectMetric metric,
                       std::uint64_t amount) noexcept
{
    if (relic >= RelicKind::Count || amount == 0) return;
    balance.relic_effects[static_cast<std::size_t>(relic)]
                         [static_cast<std::size_t>(metric)] += amount;
}

void GameSimulation::SimulationWorld::RecordUpgradeRelicSynergy(SkillKind skill, std::uint8_t upgrade,
                               RelicKind relic,
                               UpgradeRelicSynergyMetric metric,
                               std::uint64_t amount) noexcept
{
    if (skill >= SkillKind::Count || upgrade >= kUpgradeCount ||
        relic >= RelicKind::Count || amount == 0)
        return;
    auto entries = std::span(balance.upgrade_relic_synergies.data(),
                             balance.upgrade_relic_synergy_count);
    auto found = std::ranges::find_if(entries, [&](const auto &entry) {
        return entry.skill == skill && entry.upgrade == upgrade &&
               entry.relic == relic;
    });
    UpgradeRelicSynergyTelemetry *entry{};
    if (found == entries.end())
    {
        if (balance.upgrade_relic_synergy_count ==
            balance.upgrade_relic_synergies.size())
            return;
        entry = &balance.upgrade_relic_synergies[
            balance.upgrade_relic_synergy_count++];
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
    balance.upgrade_damage[static_cast<std::size_t>(skill)][upgrade] += amount;
    ++balance.upgrade_triggers[static_cast<std::size_t>(skill)][upgrade];
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

float GameSimulation::SimulationWorld::EffectiveMagnetRadius() const noexcept
{
    return data.player_magnet_radius +
           0.4f * player.stats[static_cast<std::size_t>(StatKind::MagnetRadius)];
}

std::size_t GameSimulation::SimulationWorld::EnemyTelemetryIndex(const EnemyActor &enemy) noexcept
{
    return enemy.boss ? 3u + static_cast<std::size_t>(*enemy.boss)
                      : static_cast<std::size_t>(enemy.kind);
}

Tick GameSimulation::SimulationWorld::CooldownTicks(SkillKind skill) const noexcept
{
    const auto index = static_cast<std::size_t>(skill);
    auto ticks = static_cast<float>(data.skills[index].cooldown_ticks);
    const auto reduction =
        0.03f * player.stats[static_cast<std::size_t>(StatKind::CooldownReduction)];
    ticks *= 1.0f - reduction;
    if ((skill == SkillKind::PiercingShot || skill == SkillKind::ExplosiveArrow ||
         skill == SkillKind::RicochetArrow) &&
        HasUpgrade(player.upgrades[index], 8))
    {
        ticks *= skill == SkillKind::RicochetArrow ? 0.85f : 0.75f;
    }
    return std::max<Tick>(15, static_cast<Tick>(std::llround(ticks)));
}

EnemyActor *GameSimulation::SimulationWorld::FindEnemy(std::uint64_t id) noexcept
{
    const auto iterator = std::ranges::find(enemies, id, [](const EnemyActor &enemy) {
        return enemy.id.value;
    });
    return iterator == enemies.end() ? nullptr : &*iterator;
}

const EnemyActor *GameSimulation::SimulationWorld::FindEnemy(std::uint64_t id) const noexcept
{
    const auto iterator = std::ranges::find(enemies, id, [](const EnemyActor &enemy) {
        return enemy.id.value;
    });
    return iterator == enemies.end() ? nullptr : &*iterator;
}

std::uint32_t GameSimulation::SimulationWorld::NormalEnemyCount() const noexcept
{
    return static_cast<std::uint32_t>(std::ranges::count_if(
        enemies, [](const EnemyActor &enemy) { return !enemy.boss && !enemy.dead; }));
}

std::uint32_t GameSimulation::SimulationWorld::BossCount() const noexcept
{
    return static_cast<std::uint32_t>(std::ranges::count_if(
        enemies, [](const EnemyActor &enemy) { return enemy.boss && !enemy.dead; }));
}

std::uint32_t GameSimulation::SimulationWorld::ProjectileCount(bool player_owned) const noexcept
{
    return static_cast<std::uint32_t>(std::ranges::count_if(
        projectiles, [player_owned](const ProjectileActor &projectile) {
            return projectile.player_owned == player_owned && !projectile.dead;
        }));
}

ProjectileActor *GameSimulation::SimulationWorld::FireProjectile(SkillKind skill, Float2 position, Float2 direction,
                                float coefficient, EffectOrigin origin,
                                std::uint64_t cast_id,
                                std::uint8_t applied_upgrade_mask,
                                bool player_owned,
                                std::uint8_t source_upgrade,
                                std::uint8_t source_relic,
                                std::uint8_t source_enemy)
{
    assert(skill >= SkillKind::Count ||
           (applied_upgrade_mask &
            ~player.upgrades[static_cast<std::size_t>(skill)]) == 0);
    const auto &definition = data.skills[static_cast<std::size_t>(skill)];
    const auto projectile_speed =
        definition.projectile_speed > 0.0f
            ? definition.projectile_speed
            : data.skills[static_cast<std::size_t>(SkillKind::BasicAttack)]
                  .projectile_speed;
    ProjectileActor projectile;
    projectile.id = AllocateId();
    projectile.player_owned = player_owned;
    projectile.position = projectile.previous_position = position;
    projectile.spawned_tick = tick;
    const auto normalized = Normalize(direction);
    projectile.velocity = Multiply(
        normalized, player_owned ? projectile_speed
                                 : data.enemies[static_cast<std::size_t>(
                                       EnemyKind::Ranged)].projectile_speed);
    projectile.remaining_range = player_owned ? definition.range : 18.0f;
    projectile.radius = player_owned ? definition.collision_radius * 2.0f : 0.25f;
    projectile.damage = player_owned ? RoundDamage(EffectiveAttack() * coefficient)
                                     : RoundDamage(coefficient);
    projectile.skill = skill;
    projectile.upgrade_mask = applied_upgrade_mask;
    projectile.origin = origin;
    projectile.cast_id = cast_id;
    projectile.source_upgrade = source_upgrade;
    projectile.source_relic = source_relic;
    projectile.source_enemy = source_enemy;
    const auto full_skill_effect = origin == EffectOrigin::Original ||
                                   applied_upgrade_mask != 0;
    projectile.pierce_remaining = full_skill_effect ? definition.pierce_count : 0;
    if (skill == SkillKind::BasicAttack && HasUpgrade(projectile.upgrade_mask, 2))
    {
        ++projectile.pierce_remaining;
    }
    if (skill == SkillKind::MultiShot && HasUpgrade(projectile.upgrade_mask, 3))
    {
        ++projectile.pierce_remaining;
    }
    if (skill == SkillKind::RicochetArrow && full_skill_effect)
    {
        projectile.bounce_remaining = 5;
    }
    if (skill == SkillKind::ExplosiveArrow && full_skill_effect)
    {
        projectile.explosion_radius = 3.0f;
        projectile.explosion_damage = projectile.damage;
    }
    auto &storage = current_phase == SimulationPhaseId::CastAttack
                        ? pending_projectile_spawns
                        : projectiles;
    storage.push_back(projectile);
    if (player_owned && skill < SkillKind::Count &&
        skill != SkillKind::BasicAttack)
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
                Add(position, Multiply(normalized, 0.5f)), direction, 1.0f, 0.8f);
    RecordUpgradeEffect(skill, source_upgrade,
                        UpgradeEffectMetric::ProjectilesCreated);
    return &storage.back();
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
            ~player.upgrades[static_cast<std::size_t>(skill)]) == 0);
    AreaActor area;
    area.id = AllocateId();
    area.kind = kind;
    area.position = position;
    area.radius = radius;
    area.effect_radius = effect_radius > 0.0f ? effect_radius : radius;
    area.damage = RoundDamage(EffectiveAttack() * coefficient);
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
    auto &storage = current_phase == SimulationPhaseId::CastAttack
                        ? pending_area_spawns
                        : areas;
    storage.push_back(area);
    RecordUpgradeEffect(skill, source_upgrade,
                        UpgradeEffectMetric::AreasCreated);
    return &storage.back();
}

void GameSimulation::SimulationWorld::Schedule(const ScheduledAction &action)
{
    scheduled.push_back(action);
}

void GameSimulation::SimulationWorld::DealDamage(std::uint64_t target, std::int32_t amount, SkillKind skill,
                EffectOrigin origin, std::uint64_t cast_id,
                std::uint8_t bleed, bool burn,
                float slow, Tick slow_duration,
                std::uint8_t source_upgrade,
                std::uint8_t source_relic,
                std::uint8_t source_enemy)
{
    if (target == 0 && tick < player.revive_invulnerable_until)
        return;
    const auto proc = MakeProcContext(cast_id, origin);
    if (!proc.Allows(ProcPermission::Damage)) return;
    if (!proc.Allows(ProcPermission::Status))
    {
        bleed = 0;
        burn = false;
        slow = 0.0f;
        slow_duration = 0;
    }
    combat.Push({++damage_sequence, target, std::max(1, amount), skill,
                 origin, proc.root_cast, source_upgrade, source_relic, source_enemy,
                 bleed, burn, slow, slow_duration});
}

void GameSimulation::SimulationWorld::DamageArea(Float2 position, float radius, std::int32_t damage, SkillKind skill,
                EffectOrigin origin, std::uint64_t cast_id,
                std::uint8_t bleed, bool burn,
                float slow, Tick slow_duration,
                std::uint8_t source_upgrade,
                std::uint8_t source_relic)
{
    for (const auto &enemy : enemies)
    {
        if (!enemy.dead && DistanceSquared(position, enemy.position) <= radius * radius)
        {
            DealDamage(enemy.id.value, damage, skill, origin, cast_id, bleed, burn,
                       slow, slow_duration, source_upgrade, source_relic);
        }
    }
}

void GameSimulation::SimulationWorld::CastBasicAttack()
{
    if (!input.held.basic_attack_held || player.charging ||
        player.active_cast_tick > tick || tick < player.next_basic_attack)
    {
        return;
    }
    player.has_move_target = false;
    const auto attack_interval = std::max<Tick>(1, static_cast<Tick>(
        std::floor(60.0f / EffectiveAttackSpeed() + 0.5f)));
    const auto animation_ticks = std::max<Tick>(
        1, attack_interval > kAnimationBlendOutTicks
               ? attack_interval - kAnimationBlendOutTicks
               : 1);
    const auto release_ticks = AnimationMarkerTicks(kBasicArrowReleaseTicks,
                                                    animation_ticks);
    player.next_basic_attack = tick + attack_interval;
    ++balance.skill_uses[static_cast<std::size_t>(SkillKind::BasicAttack)];
    const auto base_interval = std::max<Tick>(
        1, static_cast<Tick>(std::floor(60.0f / data.player_attack_speed + 0.5f)));
    balance.stat_utility[static_cast<std::size_t>(StatKind::AttackSpeed)] +=
        base_interval - std::min(base_interval, attack_interval);
    player.basic_attack_animation_start = tick;
    player.basic_attack_animation_until = player.next_basic_attack;
    ++player.basic_sequence;
    player.facing = player.aim;
    const auto cast_id = next_cast_id++;
    const auto mask = player.upgrades[0];
    RuntimeForCast(cast_id, SkillKind::BasicAttack).upgrade_mask = mask;
    const auto fire = [&](Float2 direction, float coefficient,
                          std::uint8_t source_upgrade = kNoTelemetrySource) {
        Schedule({tick + release_ticks, ScheduledKind::Projectile,
                  SkillKind::BasicAttack, player.position, direction, coefficient,
                  0.0f, 0.0f, 1, mask, EffectOrigin::Original, cast_id,
                  source_upgrade});
    };

    if (HasUpgrade(mask, 8) && tick <= player.active_basic_empower_until)
    {
        fire(player.aim, 0.7f);
        fire(Rotate(player.aim, -15.0f), 0.7f, 7);
        fire(Rotate(player.aim, 15.0f), 0.7f, 7);
        player.active_basic_empower_until = 0;
    }
    else
    {
        fire(player.aim, 1.0f);
    }
    if (HasUpgrade(mask, 1) && player.basic_sequence % 3 == 0)
    {
        Schedule({tick + release_ticks + 5, ScheduledKind::Projectile,
                  SkillKind::BasicAttack,
                  player.position, player.aim, 0.7f, 0.0f, 0.0f, 1,
                  static_cast<std::uint8_t>(mask & ~std::uint8_t{1}),
                  EffectOrigin::Original, cast_id, 0});
    }
    DispatchBasicAttackRules(release_ticks, cast_id);
}

bool GameSimulation::SimulationWorld::CastSkill(SkillKind skill)
{
    const auto skill_index = static_cast<std::size_t>(skill);
    const auto cooldown_index = skill_index - 1;
    if (skill_index == 0 || skill_index >= kCombatSkillCount ||
        player.cooldowns[cooldown_index] != 0 || player.charging ||
        player.active_cast_tick > tick || tick < player.retreat_until)
    {
        return false;
    }
    player.cooldowns[cooldown_index] = CooldownTicks(skill) + 1;
    if (HasUpgrade(player.upgrades[skill_index], 8) &&
        (skill == SkillKind::PiercingShot || skill == SkillKind::ExplosiveArrow ||
         skill == SkillKind::RicochetArrow))
    {
        const auto stat_reduction =
            1.0f - 0.03f * player.stats[static_cast<std::size_t>(
                                 StatKind::CooldownReduction)];
        const auto without_upgrade = std::max<Tick>(
            15, static_cast<Tick>(std::llround(
                    data.skills[skill_index].cooldown_ticks * stat_reduction)));
        RecordUpgradeEffect(
            skill, 7, UpgradeEffectMetric::CooldownTicksSaved,
            without_upgrade - std::min(without_upgrade,
                                         player.cooldowns[cooldown_index] - 1));
    }
    ++balance.skill_uses[skill_index];
    const auto cooldown_reduction =
        0.03f * player.stats[static_cast<std::size_t>(StatKind::CooldownReduction)];
    if (cooldown_reduction > 0.0f)
    {
        const auto without_stat = static_cast<Tick>(std::llround(
            static_cast<double>(player.cooldowns[cooldown_index] - 1) /
            (1.0 - cooldown_reduction)));
        balance.stat_utility[static_cast<std::size_t>(StatKind::CooldownReduction)] +=
            without_stat - std::min(without_stat, player.cooldowns[cooldown_index] - 1);
    }
    player.active_animation_start = tick;
    player.basic_attack_animation_until = tick;
    constexpr std::array<Tick, kCombatSkillCount> recovery_ticks{
        0, 15, 18, 9, 18, 15, 15, 15, 13};
    player.active_cast_tick = tick + recovery_ticks[skill_index];
    player.active_animation_until = player.active_cast_tick + kAnimationBlendOutTicks;
    const auto cast_id = next_cast_id++;
    const auto mask = player.upgrades[skill_index];
    RuntimeForCast(cast_id, skill).upgrade_mask = mask;
    const auto skill_release_ticks = AnimationMarkerTicks(
        kBasicArrowReleaseTicks, recovery_ticks[skill_index]);
    player.facing = player.aim;
    const auto target_delta = Float2{input.held.aim_world.x - player.position.x,
                                     input.held.aim_world.z - player.position.y};
    const auto target_distance = Length(target_delta);
    const auto target = Add(player.position,
                            Multiply(Normalize(target_delta, player.aim),
                                     std::min(target_distance, data.skills[skill_index].range)));

    switch (data.skills[skill_index].handler)
    {
    case AbilityHandlerId::PiercingProjectile:
        Schedule({tick + skill_release_ticks, ScheduledKind::Projectile, skill,
                  player.position, player.aim,
                  data.skills[skill_index].damage_coefficient,
                  0.0f, 0.0f, 1, mask,
                  EffectOrigin::Original, cast_id});
        if (HasUpgrade(mask, 1))
        {
            Schedule({tick + skill_release_ticks + 5, ScheduledKind::Projectile,
                      skill, player.position,
                      player.aim, 5.0f, 0.0f, 0.0f, 1, 0,
                      EffectOrigin::Derived, cast_id, 0});
        }
        if (HasUpgrade(mask, 4))
        {
            const auto half_length = data.skills[skill_index].range * 0.5f;
            auto *area = SpawnArea(
                AreaKind::Damage, skill,
                Add(player.position, Multiply(player.aim, half_length)), 0.5f,
                0.4f, 2.0f, 0.0f, EffectOrigin::Derived, cast_id, 0,
                0.2f, 1.0f);
            area->direction = player.aim;
            area->half_length = half_length;
            area->source_upgrade = 3;
            EmitVfx(DomainSignalKind::PiercingTrailPulse,
                    area->position, player.aim, 1.0f, 0.12f);
        }
        break;
    case AbilityHandlerId::UniformFanProjectiles:
    {
        const auto miss_retarget_mask = static_cast<std::uint8_t>(
            mask & (std::uint8_t{1} << 7));
        Schedule({tick + skill_release_ticks, ScheduledKind::Volley, skill,
                  player.position, player.aim,
                  data.skills[skill_index].damage_coefficient,
                  0.0f, 0.0f, data.skills[skill_index].projectile_count, mask,
                  EffectOrigin::Original, cast_id});
        if (HasUpgrade(mask, 1))
        {
            Schedule({tick + 15, ScheduledKind::Volley, skill, player.position,
                      player.aim, 1.5f, 0.0f, 0.0f, 5, miss_retarget_mask,
                      EffectOrigin::Derived, cast_id, 0});
        }
        if (HasUpgrade(mask, 4))
        {
            for (const auto angle : {-15.0f, 0.0f, 15.0f})
            {
                Schedule({tick + skill_release_ticks, ScheduledKind::Projectile,
                          skill, player.position,
                          Rotate(Multiply(player.aim, -1.0f), angle), 1.0f,
                          0.0f, 0.0f, 1, miss_retarget_mask,
                          EffectOrigin::Derived, cast_id, 3});
            }
        }
        if (HasUpgrade(mask, 7))
        {
            for (const auto angle : {-31.25f, 31.25f})
                Schedule({tick + skill_release_ticks, ScheduledKind::Projectile,
                          skill, player.position, Rotate(player.aim, angle), 1.2f,
                          0.0f, 0.0f, 1, mask, EffectOrigin::Original, cast_id, 6});
        }
        break;
    }
    case AbilityHandlerId::ProjectileToAreaExplosion:
        Schedule({tick + skill_release_ticks, ScheduledKind::Projectile, skill,
                  player.position, player.aim,
                  data.skills[skill_index].damage_coefficient,
                  0.0f, 0.0f, 1, mask,
                  EffectOrigin::Original, cast_id});
        break;
    case AbilityHandlerId::NearestUnhitTargetRicochet:
        Schedule({tick + skill_release_ticks, ScheduledKind::Projectile, skill,
                  player.position, player.aim,
                  data.skills[skill_index].damage_coefficient,
                  0.0f, 0.0f, 1, mask,
                  EffectOrigin::Original, cast_id});
        break;
    case AbilityHandlerId::TargetedPeriodicArea:
        SpawnArea(AreaKind::Damage, skill, target, 4.0f,
                  data.skills[skill_index].damage_coefficient,
                  HasUpgrade(mask, 8) ? 5.0f : 3.0f, 0.4f,
                  EffectOrigin::Original, cast_id, mask,
                  HasUpgrade(mask, 6) ? 0.3f : 0.0f, 1.0f);
        EmitVfx(DomainSignalKind::ArrowRainCast, target, player.aim, 1.0f, 0.1f);
        if (HasUpgrade(mask, 1))
        {
            Schedule({tick + 60, ScheduledKind::Area, skill,
                      Add(target, Multiply(player.aim, 4.0f)), player.aim,
                      0.5f, 3.0f, 2.0f, 1, 0,
                      EffectOrigin::Derived, cast_id, 0});
        }
        break;
    case AbilityHandlerId::ForwardRollLeaveTrap:
    {
        const auto origin = player.position;
        if (HasUpgrade(mask, 1))
        {
            for (std::uint32_t index = 0; index < 3; ++index)
            {
                SpawnArea(AreaKind::Trap, skill,
                          Add(origin, Multiply(player.aim, 1.5f * index)),
                          2.0f, 0.9f, 12.0f, 0.6f,
                          EffectOrigin::Original, cast_id, mask,
                          0.3f, 3.0f, 3.0f,
                          false, 0);
            }
        }
        else
        {
            SpawnArea(AreaKind::Trap, skill, origin, 2.0f,
                      data.skills[skill_index].damage_coefficient,
                      12.0f, 0.6f,
                      EffectOrigin::Original, cast_id, mask, 0.3f, 3.0f, 3.0f);
        }
        player.retreat_until = tick + 15;
        player.retreat_velocity = Multiply(player.aim, 20.0f);
        player.retreat_followup_cast = cast_id;
        player.retreat_upgrade_mask = mask;
        player.retreat_landing_pending = true;
        player.forced_move_skill = SkillKind::Trap;
        EmitVfx(DomainSignalKind::TrapCast, origin);
        break;
    }
    case AbilityHandlerId::ForcedRetreatAndProjectile:
    {
        EmitVfx(DomainSignalKind::RetreatMoved, player.position,
                Multiply(player.aim, -1.0f), 1.0f, 0.1f);
        const auto retreat_release_ticks = AnimationMarkerTicks(
            4, recovery_ticks[skill_index]);
        Schedule({tick + retreat_release_ticks, ScheduledKind::Projectile, skill,
                   player.position, player.aim,
                   HasUpgrade(mask, 1)
                       ? 0.9f
                       : data.skills[skill_index].damage_coefficient,
                   0.0f, 0.0f, 1, mask,
                   EffectOrigin::Original, cast_id,
                   static_cast<std::uint8_t>(HasUpgrade(mask, 1)
                                                 ? 0
                                                 : kNoTelemetrySource)});
        if (HasUpgrade(mask, 1))
        {
            for (const auto angle : {-12.0f, 12.0f})
                Schedule({tick + retreat_release_ticks,
                          ScheduledKind::Projectile, skill, player.position,
                          Rotate(player.aim, angle), 0.9f, 0.0f, 0.0f, 1,
                          mask, EffectOrigin::Original, cast_id, 0});
        }
        player.retreat_until = tick + 13;
        player.retreat_velocity = Multiply(player.aim, -25.0f);
        player.retreat_followup_cast = cast_id;
        player.retreat_upgrade_mask = mask;
        player.retreat_landing_pending = true;
        player.forced_move_skill = SkillKind::RetreatShot;
        if (HasUpgrade(mask, 2))
        {
            SpawnArea(AreaKind::Trap, skill, player.position, 2.5f, 3.0f, 12.0f,
                      0.6f, EffectOrigin::Derived, cast_id, 0, 0.0f, 0.0f, 0.0f,
                      false, 1);
        }
        if (HasUpgrade(mask, 3))
        {
            SpawnArea(AreaKind::Slow, skill,
                      Add(player.position, Multiply(player.aim, -2.5f)), 2.5f,
                      0.0f, 2.0f, 0.0f, EffectOrigin::Derived, cast_id, 0, 0.35f,
                      1.0f, 0.0f, false, 2);
        }
        if (HasUpgrade(mask, 6))
        {
            player.next_active_refund_until = tick + 180;
            player.next_active_refund_source = skill;
        }
        if (HasUpgrade(mask, 8))
        {
            player.retreat_followup_tick = tick + 15;
            player.retreat_followup_direction = player.aim;
        }
        break;
    }
    case AbilityHandlerId::BasicProjectileCadence:
    case AbilityHandlerId::HoldReleaseLinearCharge:
        return false;
    }

    player.active_basic_empower_until = tick + 180;
    if (player.next_active_refund_until >= tick &&
        player.next_active_refund_source != SkillKind::Count &&
        player.next_active_refund_source != skill)
    {
        const auto before = player.cooldowns[cooldown_index];
        player.cooldowns[cooldown_index] -=
            player.cooldowns[cooldown_index] * 30 / 100;
        RecordUpgradeEffect(player.next_active_refund_source, 5,
                            UpgradeEffectMetric::CooldownTicksSaved,
                            before - player.cooldowns[cooldown_index]);
        player.next_active_refund_until = 0;
        player.next_active_refund_source = SkillKind::Count;
    }
    DispatchAbilityRules(skill, cooldown_index);
    player.last_active = skill;
    player.last_active_tick = tick;
    EmitSignal(DomainSignalKind::AbilityUsed, player.position);
    return true;
}

bool GameSimulation::SimulationWorld::TryBeginSkill(SkillKind skill)
{
    if (data.skills[static_cast<std::size_t>(skill)].handler ==
        AbilityHandlerId::HoldReleaseLinearCharge)
    {
        const auto cooldown = static_cast<std::size_t>(skill) - 1;
        if (player.cooldowns[cooldown] != 0 || player.charging ||
            player.active_cast_tick > tick || tick < player.retreat_until)
        {
            return false;
        }
        player.charging = true;
        player.charging_skill = skill;
        player.charge_start = tick;
        return true;
    }
    return CastSkill(skill);
}

void GameSimulation::SimulationWorld::ConsumeBufferedSkill()
{
    if (phase != SessionPhase::Playing ||
        player.buffered_skill == SkillKind::Count)
    {
        return;
    }
    if (tick > player.buffered_skill_expires)
    {
        player.buffered_skill = SkillKind::Count;
        player.buffered_skill_expires = 0;
        return;
    }
    if (TryBeginSkill(player.buffered_skill))
    {
        player.buffered_skill = SkillKind::Count;
        player.buffered_skill_expires = 0;
    }
}

void GameSimulation::SimulationWorld::ReleaseChargedShot()
{
    const auto mask = player.upgrades[static_cast<std::size_t>(SkillKind::ChargedShot)];
    const auto maximum_ticks = HasUpgrade(mask, 1) ? Seconds(1.4f) : Seconds(1.0f);
    auto elapsed = std::min(tick - player.charge_start, maximum_ticks);
    if (HasUpgrade(mask, 2))
    {
        const auto unaccelerated = elapsed;
        elapsed = std::min(maximum_ticks, static_cast<Tick>(elapsed / 0.65f));
        RecordUpgradeEffect(SkillKind::ChargedShot, 1,
                            UpgradeEffectMetric::ChargeTicksSaved,
                            elapsed - unaccelerated);
    }
    const auto ratio = static_cast<float>(elapsed) / static_cast<float>(maximum_ticks);
    const auto minimum = HasUpgrade(mask, 3) ? 11.5f : 5.0f;
    const auto maximum = HasUpgrade(mask, 1)
                             ? 14.0f
                             : data.skills[static_cast<std::size_t>(
                                   SkillKind::ChargedShot)].damage_coefficient;
    const auto coefficient = std::lerp(minimum, maximum, ratio);
    const auto cast_id = next_cast_id++;
    player.facing = player.aim;
    auto &runtime = RuntimeForCast(cast_id, SkillKind::ChargedShot);
    runtime.upgrade_mask = mask;
    runtime.full_charge = ratio >= 0.999f;
    if (auto *projectile = FireProjectile(SkillKind::ChargedShot, player.position,
                                          player.aim, coefficient,
                                          EffectOrigin::Original, cast_id, mask))
    {
        projectile->remaining_range = std::lerp(
            4.2f,
            data.skills[static_cast<std::size_t>(SkillKind::ChargedShot)].range,
            ratio);
        projectile->radius = std::lerp(0.5f, 1.1f, ratio);
        if (HasUpgrade(mask, 4))
        {
            projectile->pierce_remaining += 4;
        }
        if (HasUpgrade(mask, 5))
        {
            projectile->bleed_stacks = 3;
        }
        if (HasUpgrade(mask, 1) && ratio >= 0.999f)
        {
            projectile->explosion_radius = 2.0f;
            projectile->explosion_damage = RoundDamage(EffectiveAttack() * 5.0f);
            projectile->explosion_source_upgrade = 0;
        }
        projectile->full_charge = ratio >= 0.999f;
        projectile->charge_ratio = ratio;
    }
    player.cooldowns[static_cast<std::size_t>(SkillKind::ChargedShot) - 1] =
        CooldownTicks(SkillKind::ChargedShot) + 1;
    ++balance.skill_uses[static_cast<std::size_t>(SkillKind::ChargedShot)];
    const auto cooldown_reduction =
        0.03f * player.stats[static_cast<std::size_t>(StatKind::CooldownReduction)];
    if (cooldown_reduction > 0.0f)
    {
        const auto cooldown =
            player.cooldowns[static_cast<std::size_t>(SkillKind::ChargedShot) - 1] - 1;
        const auto without_stat = static_cast<Tick>(std::llround(
            static_cast<double>(cooldown) / (1.0 - cooldown_reduction)));
        balance.stat_utility[static_cast<std::size_t>(StatKind::CooldownReduction)] +=
            without_stat - std::min(without_stat, cooldown);
    }
    player.active_animation_start = tick;
    player.basic_attack_animation_until = tick;
    player.active_cast_tick = tick + 9;
    player.active_animation_until = player.active_cast_tick + kAnimationBlendOutTicks;
    CancelChargedShot();
    player.active_basic_empower_until = tick + 180;
}

void GameSimulation::SimulationWorld::CastAttackPhase()
{
    CastBasicAttack();
    std::ranges::sort(boss_actions, {}, &BossAction::due);
    std::size_t boss_actions_executed{};
    while (boss_actions_executed < boss_actions.size() &&
           boss_actions[boss_actions_executed].due <= tick)
    {
        const auto action = boss_actions[boss_actions_executed++];
        auto *boss = FindEnemy(action.boss_id);
        if (!boss || boss->dead) continue;
        boss->attack_cast_id = action.cast_id;
        auto direction = action.direction;
        if (LengthSquared(direction) <= 0.0001f)
            direction = Normalize(Subtract(player.position, boss->position));
        if (action.kind == BossActionKind::Dash)
        {
            EmitVfx(DomainSignalKind::BossDashStarted, boss->position,
                    direction, 1.0f, 0.12f);
            boss->velocity = Multiply(direction, action.speed);
            boss->dash_until = tick + Seconds(action.distance / action.speed);
            boss->dash_damage = action.damage;
            boss->dash_hit = false;
        }
        else if (action.kind == BossActionKind::Volley)
        {
            EmitVfx(DomainSignalKind::BossVolleyReleased, boss->position,
                    direction, 1.0f, 1.0f);
            for (std::uint32_t index = 0; index < action.projectile_count; ++index)
            {
                const auto angle = action.projectile_count == 1 ? action.angle_offset :
                    -action.arc_degrees * 0.5f +
                    action.arc_degrees * index /
                        static_cast<float>(action.projectile_count - 1) +
                    action.angle_offset;
                if (auto *shot = FireProjectile(
                        SkillKind::BasicAttack, boss->position,
                         Rotate(direction, angle), static_cast<float>(action.damage),
                         EffectOrigin::Original, action.cast_id, 0, false,
                         kNoTelemetrySource, kNoTelemetrySource,
                         static_cast<std::uint8_t>(EnemyTelemetryIndex(*boss))))
                {
                    shot->velocity = Multiply(Normalize(shot->velocity), action.speed);
                    shot->remaining_range = 24.0f;
                }
            }
        }
        else if (action.kind == BossActionKind::Area)
        {
            EmitVfx(DomainSignalKind::BossAreaActivated, action.position, {},
                    action.radius / 2.2f, 0.12f);
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
            EmitVfx(DomainSignalKind::BossShockwaveReleased, action.position, {},
                    action.radius / 9.75f, 0.15f);
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
            area->safe_gap_count = 4;
            area->safe_gap_degrees = 25.0f;
            area->interval = 1;
        }
    }
    boss_actions.erase(boss_actions.begin(),
                       boss_actions.begin() + boss_actions_executed);
    std::ranges::sort(scheduled, {}, &ScheduledAction::due);
    std::size_t executed{};
    while (executed < scheduled.size() && scheduled[executed].due <= tick)
    {
        const auto action = scheduled[executed++];
        if (action.kind == ScheduledKind::Projectile)
        {
                auto *projectile = FireProjectile(
                    action.skill, action.position, action.direction,
                    action.damage_coefficient, action.origin, action.cast_id,
                    action.upgrade_mask, true,
                    action.source_upgrade, action.source_relic);
            if (projectile && action.skill == SkillKind::BasicAttack &&
                action.origin == EffectOrigin::Original)
            {
                if (action.source_upgrade == 0)
                {
                    ++player.basic_sequence;
                    ++balance.skill_uses[
                        static_cast<std::size_t>(SkillKind::BasicAttack)];
                }
                ++player.basic_arrow_sequence;
                if (HasUpgrade(action.upgrade_mask, 4) &&
                    player.basic_arrow_sequence % 3 == 0)
                {
                    projectile->bleed_stacks = 1;
                }
                if (HasUpgrade(action.upgrade_mask, 5) &&
                    player.basic_arrow_sequence % 4 == 0)
                {
                    projectile->burn = true;
                }
                EmitSignal(DomainSignalKind::ArrowReleased, action.position);
            }
            if (projectile && action.skill == SkillKind::PiercingShot &&
                action.source_upgrade == 0)
            {
                if (const auto *target = NearestEnemy(action.position, 24.0f))
                {
                    projectile->homing = true;
                    projectile->homing_target = target->id.value;
                }
            }
            if (projectile && action.skill == SkillKind::RicochetArrow &&
                action.origin == EffectOrigin::Original)
            {
                projectile->homing = true;
                if (const auto *target = NearestEnemy(action.position, 18.0f))
                    projectile->homing_target = target->id.value;
            }
        }
        else if (action.kind == ScheduledKind::Volley)
        {
            for (std::uint32_t index = 0; index < action.projectile_count; ++index)
            {
                FireProjectile(action.skill, action.position,
                               Rotate(action.direction, -25.0f + index * 12.5f),
                               action.damage_coefficient, action.origin, action.cast_id,
                               action.upgrade_mask, true, action.source_upgrade,
                               action.source_relic);
            }
        }
        else if (action.kind == ScheduledKind::Explosion)
        {
            RecordUpgradeEffect(action.skill, action.source_upgrade,
                                UpgradeEffectMetric::ExplosionsCreated);
            DamageArea(action.position, action.radius,
                       RoundDamage(EffectiveAttack() * action.damage_coefficient),
                       action.skill, action.origin, action.cast_id, 0, false, 0.0f, 0,
                       action.source_upgrade, action.source_relic);
            EmitVfx(action.skill == SkillKind::ExplosiveArrow &&
                            action.source_upgrade == 0
                        ? DomainSignalKind::ExplosiveArrowSecondary
                        : DomainSignalKind::SmallExplosion,
                    action.position, {}, action.radius / 3.0f, 0.15f);
        }
        else
        {
            SpawnArea(AreaKind::Damage, action.skill, action.position, action.radius,
                       action.damage_coefficient, action.duration, 0.0f,
                       action.origin, action.cast_id, action.upgrade_mask,
                       0.0f, 0.0f, 0.0f, false,
                       action.source_upgrade, action.source_relic);
        }
    }
    scheduled.erase(scheduled.begin(), scheduled.begin() + executed);
}

void GameSimulation::SimulationWorld::MovementPhase()
{
    player.previous_position = player.position;
    const auto stationary_attack = input.held.basic_attack_held ||
                                   tick < player.active_cast_tick;
    if (player.retreat_landing_pending && tick >= player.retreat_until)
    {
        if (player.forced_move_skill == SkillKind::RetreatShot)
            EmitVfx(DomainSignalKind::RetreatLanded, player.position,
                    player.facing, 1.0f, 0.12f);
        if (player.forced_move_skill == SkillKind::Trap &&
            HasUpgrade(player.retreat_upgrade_mask, 2))
        {
            SpawnArea(AreaKind::Slow, SkillKind::Trap, player.position, 3.0f,
                      0.0f, 3.0f, 0.0f, EffectOrigin::Derived,
                      player.retreat_followup_cast, 0, 0.45f, 3.0f, 0.0f,
                      false, 1);
        }
        else if (player.forced_move_skill == SkillKind::RetreatShot &&
                 HasUpgrade(player.retreat_upgrade_mask, 5))
        {
            EmitVfx(DomainSignalKind::Push, player.position, {}, 1.0f, 0.12f);
            DamageArea(player.position, 5.0f,
                        RoundDamage(EffectiveAttack() * 3.0f),
                       SkillKind::RetreatShot, EffectOrigin::Derived,
                       player.retreat_followup_cast, 0, false, 0.0f, 0, 4);
            for (auto &enemy : enemies)
            {
                if (!enemy.dead && !enemy.boss &&
                    DistanceSquared(player.position, enemy.position) <= 25.0f)
                {
                    const auto before = enemy.position;
                    enemy.position = Add(
                        enemy.position,
                        Multiply(Normalize(Subtract(enemy.position, player.position)),
                                 3.0f));
                    RecordUpgradeDisplacement(SkillKind::RetreatShot, 4, before,
                                              enemy.position);
                }
            }
        }
        player.retreat_landing_pending = false;
        player.forced_move_skill = SkillKind::Count;
    }
    if (player.retreat_followup_tick != 0 && tick >= player.retreat_followup_tick)
    {
        const auto before = player.position;
        if (!config.scenario.player_stationary)
            player.position = Add(player.position,
                                  Multiply(player.retreat_followup_direction, -2.5f));
        RecordUpgradeDisplacement(SkillKind::RetreatShot, 7, before,
                                  player.position);
        auto direction = player.retreat_followup_direction;
        auto *target = NearestEnemy(player.position,
                                    data.skills[static_cast<std::size_t>(
                                        SkillKind::RetreatShot)].range);
        if (target)
            direction = Normalize(Subtract(target->position, player.position));
        if (auto *arrow = FireProjectile(
                SkillKind::RetreatShot, player.position, direction, 2.0f,
                EffectOrigin::Derived, player.retreat_followup_cast, 0, true, 7);
            arrow && target)
        {
            arrow->homing = true;
            arrow->homing_target = target->id.value;
        }
        player.retreat_followup_tick = 0;
    }
    if (tick < player.retreat_until && !config.scenario.player_stationary)
    {
        player.position = Add(player.position, Multiply(player.retreat_velocity, kTickSeconds));
    }
    else if (player.has_move_target && !stationary_attack)
    {
        const auto delta = Subtract(player.move_target, player.position);
        const auto charge_multiplier = player.charging ? 0.7f : 1.0f;
        const auto step = EffectiveMoveSpeed() * charge_multiplier * kTickSeconds;
        if (LengthSquared(delta) > step * step)
        {
            player.position = Add(player.position,
                                  Multiply(Normalize(delta), step));
            player.facing = Normalize(delta, player.facing);
        }
        else
        {
            player.position = player.move_target;
            player.has_move_target = false;
        }
    }
    if (stationary_attack)
    {
        player.facing = player.aim;
    }
    player.position.x = std::clamp(player.position.x, -data.arena_half_extent,
                                   data.arena_half_extent);
    player.position.y = std::clamp(player.position.y, -data.arena_half_extent,
                                   data.arena_half_extent);
    player.movement_since_echo += Length(Subtract(player.position,
                                                   player.previous_position));
    const auto moved = Length(Subtract(player.position, player.previous_position));
    const auto move_points = player.stats[static_cast<std::size_t>(StatKind::MoveSpeed)];
    if (move_points != 0 && moved > 0.0f && tick >= player.retreat_until)
    {
        const auto extra = moved * (1.0f - 1.0f / (1.0f + 0.03f * move_points));
        balance.stat_utility[static_cast<std::size_t>(StatKind::MoveSpeed)] +=
            static_cast<std::uint64_t>(std::llround(extra * 1000.0f));
    }
    const auto locomotion_target =
        LengthSquared(Subtract(player.position, player.previous_position)) > 0.000001f
            ? 1.0f : 0.0f;
    const auto locomotion_step = 1.0f / (Seconds(0.12f));
    player.locomotion_blend += std::clamp(locomotion_target - player.locomotion_blend,
                                           -locomotion_step, locomotion_step);
    if (tick - player.last_position_sample >=
        data.relics.movement_echo.position_history_age_ticks)
    {
        player.one_second_ago = player.previous_position;
        player.last_position_sample = tick;
    }

    for (auto &enemy : enemies)
    {
        if (enemy.dead) continue;
        std::erase_if(enemy.status.slows,
                      [this](const SlowEffect &slow) {
                          return slow.expires <= tick;
                      });
        for (const auto &slow : enemy.status.slows)
            RecordUpgradeEffect(slow.source_skill, slow.source_upgrade,
                                UpgradeEffectMetric::SlowActiveTicks);
        enemy.previous_position = enemy.position;
        enemy.position = Add(enemy.position, Multiply(enemy.velocity, kTickSeconds));
        enemy.position.x = std::clamp(enemy.position.x, -data.arena_half_extent,
                                      data.arena_half_extent);
        enemy.position.y = std::clamp(enemy.position.y, -data.arena_half_extent,
                                      data.arena_half_extent);
        if (enemy.boss && tick < enemy.dash_until && !enemy.dash_hit &&
            SegmentCircle(enemy.previous_position, enemy.position,
                          player.position, 1.5f))
        {
            DealDamage(0, enemy.dash_damage, SkillKind::Count,
                       EffectOrigin::Original, enemy.attack_cast_id, 0, false,
                       0.0f, 0, kNoTelemetrySource, kNoTelemetrySource,
                       static_cast<std::uint8_t>(EnemyTelemetryIndex(enemy)));
            enemy.dash_hit = true;
        }
        if (enemy.boss && enemy.attacking && tick >= enemy.attack_resolve)
        {
            if (DistanceSquared(enemy.position, player.position) <= 2.25f)
            {
                DealDamage(0, enemy.damage, SkillKind::Count,
                           EffectOrigin::Original, enemy.attack_cast_id, 0, false,
                           0.0f, 0, kNoTelemetrySource, kNoTelemetrySource,
                           static_cast<std::uint8_t>(EnemyTelemetryIndex(enemy)));
            }
            enemy.attacking = false;
            enemy.velocity = {};
        }
    }
    for (auto &projectile : projectiles)
    {
        if (projectile.dead) continue;
        projectile.previous_position = projectile.position;
        if (projectile.homing && projectile.homing_target != 0)
        {
            if (const auto *target = FindEnemy(projectile.homing_target))
            {
                projectile.velocity = Multiply(
                    Normalize(Subtract(target->position, projectile.position)),
                    Length(projectile.velocity));
            }
        }
        const auto step = Multiply(projectile.velocity, kTickSeconds);
        projectile.position = Add(projectile.position, step);
        projectile.remaining_range -= Length(step);
    }
}

void GameSimulation::SimulationWorld::SpatialGridPhase()
{
    const auto coordinate = [this](float value) {
        return std::clamp(static_cast<int>(std::floor(
                              (value + data.arena_half_extent) / kGridCellSize)),
                          0, kGridDimension - 1);
    };
    const auto rebuild = [&]() {
        for (auto &cell : enemy_grid) cell.clear();
        for (std::size_t index = 0; index < enemies.size(); ++index)
        {
            if (enemies[index].dead) continue;
            const auto x = coordinate(enemies[index].position.x);
            const auto y = coordinate(enemies[index].position.y);
            enemy_grid[static_cast<std::size_t>(y * kGridDimension + x)].push_back(index);
        }
    };
    const auto separate = [this](std::size_t left, std::size_t right) {
        if (enemies[left].dead || enemies[right].dead || enemies[left].boss ||
            enemies[right].boss)
        {
            return;
        }
        const auto delta = Subtract(enemies[left].position, enemies[right].position);
        const auto distance_squared = LengthSquared(delta);
        if (distance_squared <= 0.0001f || distance_squared >= 0.64f) return;
        const auto push = Multiply(Normalize(delta),
                                   (0.8f - std::sqrt(distance_squared)) * 0.5f);
        enemies[left].position = Add(enemies[left].position, push);
        enemies[right].position = Subtract(enemies[right].position, push);
    };

    rebuild();
    constexpr std::array<std::pair<int, int>, 4> kForwardNeighbors{{
        {1, 0}, {-1, 1}, {0, 1}, {1, 1},
    }};
    for (int y = 0; y < kGridDimension; ++y)
    {
        for (int x = 0; x < kGridDimension; ++x)
        {
            const auto &cell = enemy_grid[static_cast<std::size_t>(y * kGridDimension + x)];
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
                const auto &neighbor = enemy_grid[static_cast<std::size_t>(
                    neighbor_y * kGridDimension + neighbor_x)];
                for (const auto left : cell)
                {
                    for (const auto right : neighbor) separate(left, right);
                }
            }
        }
    }
    for (auto &enemy : enemies)
    {
        enemy.position.x = std::clamp(enemy.position.x, -data.arena_half_extent,
                                      data.arena_half_extent);
        enemy.position.y = std::clamp(enemy.position.y, -data.arena_half_extent,
                                      data.arena_half_extent);
    }
    rebuild();
}

bool GameSimulation::SimulationWorld::AlreadyHit(const ProjectileActor &projectile, std::uint64_t target) const noexcept
{
    const auto hits = std::span(projectile.hit_ids);
    return std::ranges::find(hits, target) != hits.end();
}

void GameSimulation::SimulationWorld::RecordHit(ProjectileActor &projectile, std::uint64_t target)
{
    projectile.hit_ids.push_back(target);
    ++projectile.hit_count;
}

std::uint8_t GameSimulation::SimulationWorld::IncrementCastHit(std::uint64_t cast, std::uint64_t target)
{
    const auto iterator = std::ranges::find_if(cast_hits, [=](const CastHitRecord &record) {
        return record.cast_id == cast && record.target == target;
    });
    if (iterator != cast_hits.end())
    {
        return ++iterator->count;
    }
    cast_hits.push_back({cast, target, 1});
    return 1;
}

std::uint8_t GameSimulation::SimulationWorld::IncrementAreaHit(std::uint64_t area, std::uint64_t target)
{
    const auto iterator = std::ranges::find_if(area_hits, [=](const AreaHitRecord &record) {
        return record.area_id == area && record.target == target;
    });
    if (iterator != area_hits.end()) return ++iterator->count;
    area_hits.push_back({area, target, 1});
    return 1;
}

CastRuntime &GameSimulation::SimulationWorld::RuntimeForCast(std::uint64_t cast, SkillKind skill)
{
    const auto iterator = std::ranges::find(cast_runtime, cast,
                                            &CastRuntime::cast_id);
    if (iterator != cast_runtime.end()) return *iterator;
    cast_runtime.push_back({cast, skill});
    return cast_runtime.back();
}

void GameSimulation::SimulationWorld::OnProjectileHit(ProjectileActor &projectile, EnemyActor &enemy)
{
    RecordHit(projectile, enemy.id.value);
    const auto cast_hit = IncrementCastHit(projectile.cast_id, enemy.id.value);
    auto &runtime = RuntimeForCast(projectile.cast_id, projectile.skill);
    if (!enemy.boss) ++runtime.normal_hits;
    auto damage = projectile.damage;
    if (projectile.skill == SkillKind::PiercingShot)
    {
        const auto multiplier = std::max(0.55f, 1.0f - 0.15f *
                                                       (projectile.hit_count - 1));
        damage = RoundDamage(projectile.damage * multiplier);
    }
    auto bleed = projectile.bleed_stacks;
    auto burn = projectile.burn;
    auto slow = 0.0f;
    auto slow_duration = Tick{};
    auto damage_upgrade = projectile.source_upgrade;
    if (damage_upgrade == kNoTelemetrySource &&
        projectile.skill == SkillKind::BasicAttack &&
        HasUpgrade(projectile.upgrade_mask, 2) && projectile.hit_count > 1)
        damage_upgrade = 1;
    if (damage_upgrade == kNoTelemetrySource &&
        projectile.skill == SkillKind::MultiShot)
    {
        if (HasUpgrade(projectile.upgrade_mask, 3) && projectile.hit_count > 2)
            damage_upgrade = 2;
    }
    if (damage_upgrade == kNoTelemetrySource &&
        projectile.skill == SkillKind::ChargedShot &&
        HasUpgrade(projectile.upgrade_mask, 4) && projectile.hit_count > 4)
        damage_upgrade = 3;
    if (damage_upgrade == kNoTelemetrySource &&
        projectile.skill == SkillKind::RicochetArrow &&
        HasUpgrade(projectile.upgrade_mask, 3) && projectile.hit_count > 6)
        damage_upgrade = 2;
    if (damage_upgrade < kUpgradeCount &&
        projectile.source_upgrade == kNoTelemetrySource)
        RecordUpgradeEffect(projectile.skill, damage_upgrade,
                            UpgradeEffectMetric::ExtraTargetsHit);
    if (projectile.skill == SkillKind::PiercingShot && HasUpgrade(projectile.upgrade_mask, 3))
    {
        bleed = std::max<std::uint8_t>(bleed, 2);
    }
    if (projectile.skill == SkillKind::MultiShot &&
        HasUpgrade(projectile.upgrade_mask, 5) && cast_hit == 1)
    {
        bleed = std::max<std::uint8_t>(bleed, 1);
    }
    const auto multi_hit_limit = projectile.skill == SkillKind::MultiShot
        ? 2u
        : std::numeric_limits<unsigned>::max();
    if (cast_hit <= multi_hit_limit)
    {
        if (projectile.carried_burn_attack > 0.0f)
        {
            ApplyBurn(enemy, projectile.carried_burn_attack, false,
                      projectile.carried_burn_expires > tick
                          ? projectile.carried_burn_expires - tick
                          : 1,
                      projectile.skill, 3);
            projectile.carried_burn_attack = 0.0f;
        }
        if (projectile.skill == SkillKind::ChargedShot && projectile.full_charge &&
            HasUpgrade(projectile.upgrade_mask, 5) &&
            enemy.status.bleed_count == 5)
        {
            DealDamage(enemy.id.value, RoundDamage(EffectiveAttack() * 1.5f),
                       projectile.skill, EffectOrigin::Derived,
                       projectile.cast_id, 0, false, 0.0f, 0, 4);
        }
        if (projectile.skill != SkillKind::ExplosiveArrow ||
            projectile.explosion_radius <= 0.0f)
        {
            DealDamage(enemy.id.value, damage, projectile.skill, projectile.origin,
                       projectile.cast_id, bleed, burn, slow, slow_duration,
                       damage_upgrade, projectile.source_relic);
            if (projectile.skill == SkillKind::ChargedShot &&
                projectile.charge_ratio > 0.0f)
            {
                auto &event = combat.commands.back();
                const auto add_amplifier = [&](std::uint8_t upgrade,
                                               float coefficient) {
                    if (coefficient <= 0.0f ||
                        event.amplified_count == event.amplified_damage.size())
                        return;
                    const auto index = event.amplified_count++;
                    event.amplified_upgrades[index] = upgrade;
                    event.amplified_damage[index] =
                        RoundDamage(EffectiveAttack() * coefficient);
                };
                const auto decay = 1.0f;
                if (HasUpgrade(projectile.upgrade_mask, 1))
                    add_amplifier(0, 3.0f * projectile.charge_ratio * decay);
                if (HasUpgrade(projectile.upgrade_mask, 3))
                    add_amplifier(2,
                                  7.0f * (1.0f - projectile.charge_ratio) * decay);
            }
        }
    }

    if (projectile.skill == SkillKind::BasicAttack &&
        HasUpgrade(projectile.upgrade_mask, 6) && projectile.hit_count == 1)
    {
        enemy.status.slows.push_back({0.2f, tick + Seconds(1.0f),
                                      SkillKind::BasicAttack, 5});
        RecordUpgradeEffect(SkillKind::BasicAttack, 5,
                            UpgradeEffectMetric::SlowApplications);
        RecordUpgradeEffect(SkillKind::BasicAttack, 5,
                            UpgradeEffectMetric::SlowTargetTicks,
                            Seconds(1.0f));
        const auto longest = std::ranges::max_element(player.cooldowns);
        if (longest != player.cooldowns.end())
        {
            const auto before = *longest;
            *longest = *longest > 12 ? *longest - 12 : 0;
            RecordUpgradeEffect(SkillKind::BasicAttack, 5,
                                UpgradeEffectMetric::CooldownTicksSaved,
                                before - *longest);
        }
    }
    if (projectile.skill == SkillKind::PiercingShot &&
        HasUpgrade(projectile.upgrade_mask, 6) && !enemy.boss)
    {
        const auto before = enemy.position;
        enemy.position = Add(enemy.position,
                             Multiply(Normalize(projectile.velocity), 1.5f));
        RecordUpgradeDisplacement(SkillKind::PiercingShot, 5, before,
                                  enemy.position);
    }
    if (projectile.skill == SkillKind::PiercingShot &&
        HasUpgrade(projectile.upgrade_mask, 7))
    {
        if (runtime.transfer_count == 0)
        {
            if (!enemy.status.burn)
                ApplyBurn(enemy, EffectiveAttack(), false, Seconds(4.0f),
                          projectile.skill, 6);
            runtime.has_stored_burn = true;
            runtime.stored_burn_attack = enemy.status.burn->attack_snapshot;
            runtime.stored_burn_remaining = enemy.status.burn->expires > tick
                ? enemy.status.burn->expires - tick
                : 1;
            runtime.transfer_count = 1;
        }
        else if (runtime.transfer_count < 4 && runtime.has_stored_burn)
        {
            ApplyBurn(enemy, runtime.stored_burn_attack, false,
                      runtime.stored_burn_remaining, projectile.skill, 6);
            ++runtime.transfer_count;
        }
    }
    if (projectile.skill == SkillKind::ChargedShot &&
        HasUpgrade(projectile.upgrade_mask, 6) && !runtime.triggered &&
        (enemy.boss || runtime.normal_hits >= 3))
    {
        runtime.triggered = true;
        const auto direction = Normalize(projectile.velocity);
        for (const auto angle : {-35.0f, -25.0f, -15.0f, -5.0f,
                                 5.0f, 15.0f, 25.0f, 35.0f})
            FireProjectile(projectile.skill, projectile.position,
                           Rotate(direction, angle), 3.5f,
                           EffectOrigin::Derived, projectile.cast_id, 0, true, 5);
    }
    if (projectile.skill == SkillKind::ChargedShot &&
        HasUpgrade(projectile.upgrade_mask, 7) && runtime.transfer_count == 0)
    {
        if (!enemy.status.burn)
            ApplyBurn(enemy, EffectiveAttack(), false, Seconds(4.0f),
                       projectile.skill, 6);
        runtime.transfer_count = 1;
        RecordUpgradeEffect(SkillKind::ChargedShot, 6,
                            UpgradeEffectMetric::ExplosionsCreated);
        DamageArea(enemy.position, 3.5f,
                   RoundDamage(EffectiveAttack() * 2.0f), projectile.skill,
                   EffectOrigin::Derived, projectile.cast_id, 0, false, 0.0f, 0, 6);
    }
    if (projectile.skill == SkillKind::MultiShot &&
        HasUpgrade(projectile.upgrade_mask, 6) &&
        projectile.origin == EffectOrigin::Original && runtime.transfer_count < 3)
    {
        if (!enemy.status.burn)
            ApplyBurn(enemy, EffectiveAttack(), false, Seconds(4.0f),
                       projectile.skill, 5);
        EnemyActor *target{};
        auto best = std::numeric_limits<float>::max();
        for (auto &candidate : enemies)
        {
            if (candidate.dead || candidate.id.value == enemy.id.value ||
                candidate.status.burn)
                continue;
            const auto distance = DistanceSquared(enemy.position, candidate.position);
            if (distance < best)
            {
                best = distance;
                target = &candidate;
            }
        }
        if (target)
        {
            if (auto *arrow = FireProjectile(
                    projectile.skill, enemy.position,
                    Normalize(Subtract(target->position, enemy.position)), 0.7f,
                    EffectOrigin::Derived, projectile.cast_id,
                    static_cast<std::uint8_t>(projectile.upgrade_mask &
                                              (std::uint8_t{1} << 7)),
                    true, 5))
            {
                arrow->homing = true;
                arrow->homing_target = target->id.value;
                arrow->burn = true;
                ++runtime.transfer_count;
            }
        }
    }
    if (projectile.skill == SkillKind::ExplosiveArrow &&
        projectile.origin == EffectOrigin::Original &&
        HasUpgrade(projectile.upgrade_mask, 7))
    {
        enemy.marked_by_skill = projectile.skill;
        enemy.marked_damage_coefficient = 4.0f;
        enemy.mark_expires = tick + Seconds(8.0f);
        EmitVfx(DomainSignalKind::MarkApplied, enemy.position, {},
                1.0f, enemy.boss ? 1.4f : 0.75f);
        RecordUpgradeEffect(SkillKind::ExplosiveArrow, 6,
                            UpgradeEffectMetric::ExplosionsCreated);
        DamageArea(enemy.position, 2.0f,
                   RoundDamage(EffectiveAttack() * 3.0f), projectile.skill,
                   EffectOrigin::Derived, projectile.cast_id, 0, false,
                   0.0f, 0, 6);
        RecordUpgradeEffect(SkillKind::ExplosiveArrow, 6,
                            UpgradeEffectMetric::MarksApplied);
    }
    if (projectile.skill == SkillKind::RetreatShot &&
        HasUpgrade(projectile.upgrade_mask, 4) && runtime.spawn_count < 3)
    {
        if (enemy.status.bleed_count == 0)
            ApplyBleed(enemy, EffectiveAttack(), 1, projectile.skill, 3);
        if (auto *target = NearestEnemy(enemy.position, 8.0f,
                std::span(projectile.hit_ids)))
        {
            if (auto *arrow = FireProjectile(projectile.skill, enemy.position,
                    Normalize(Subtract(target->position, enemy.position)), 1.5f,
                    EffectOrigin::Derived, projectile.cast_id, 0, true, 3))
            {
                arrow->homing = true;
                arrow->homing_target = target->id.value;
                ++runtime.spawn_count;
            }
        }
    }
    if (projectile.skill == SkillKind::RetreatShot &&
        HasUpgrade(projectile.upgrade_mask, 7) && !runtime.triggered &&
        (enemy.boss || runtime.normal_hits >= 3))
    {
        runtime.triggered = true;
        RecordUpgradeEffect(
            SkillKind::RetreatShot, 6, UpgradeEffectMetric::Activations);
        RecordUpgradeEffect(
            SkillKind::RetreatShot, 6, UpgradeEffectMetric::Healing,
            Heal(RoundDamage(player.max_health * 0.05f)));
    }

    if (projectile.origin == EffectOrigin::Original)
    {
        const auto direction = Normalize(projectile.velocity);
        if (projectile.skill == SkillKind::BasicAttack &&
            HasUpgrade(projectile.upgrade_mask, 3) && projectile.hit_count == 1)
        {
            FireProjectile(projectile.skill, projectile.position, Rotate(direction, -30.0f),
                           0.5f, EffectOrigin::Derived, projectile.cast_id, 0, true, 2);
            FireProjectile(projectile.skill, projectile.position, Rotate(direction, 30.0f),
                           0.5f, EffectOrigin::Derived, projectile.cast_id, 0, true, 2);
        }
        if (projectile.skill == SkillKind::MultiShot &&
            HasUpgrade(projectile.upgrade_mask, 2) && projectile.hit_count == 1)
        {
            FireProjectile(projectile.skill, projectile.position, Rotate(direction, -25.0f),
                           0.35f, EffectOrigin::Derived, projectile.cast_id,
                           static_cast<std::uint8_t>(projectile.upgrade_mask &
                                                     (std::uint8_t{1} << 7)),
                           true, 1);
            FireProjectile(projectile.skill, projectile.position, Rotate(direction, 25.0f),
                           0.35f, EffectOrigin::Derived, projectile.cast_id,
                           static_cast<std::uint8_t>(projectile.upgrade_mask &
                                                     (std::uint8_t{1} << 7)),
                           true, 1);
        }
        if (projectile.skill == SkillKind::PiercingShot &&
            HasUpgrade(projectile.upgrade_mask, 5) && projectile.hit_count % 3 == 0)
        {
            FireProjectile(projectile.skill, projectile.position, Rotate(direction, -90.0f),
                           2.0f, EffectOrigin::Derived, projectile.cast_id, 0, true, 4);
            FireProjectile(projectile.skill, projectile.position, Rotate(direction, 90.0f),
                           2.0f, EffectOrigin::Derived, projectile.cast_id, 0, true, 4);
        }
    }
    const auto hit_height = enemy.boss ? 1.4f : 0.75f;
    const auto hit_scale = enemy.boss ? 1.35f : 1.0f;
    const auto hit_direction = Normalize(projectile.velocity);
    if (projectile.skill == SkillKind::ChargedShot && projectile.full_charge)
        EmitVfx(DomainSignalKind::HeavyHit, enemy.position,
                hit_direction, hit_scale, hit_height);
    else if (projectile.skill == SkillKind::RicochetArrow)
        EmitVfx(DomainSignalKind::RicochetArrowHit, enemy.position,
                hit_direction, hit_scale, hit_height);
    else if (projectile.skill == SkillKind::BasicAttack)
        EmitVfx(DomainSignalKind::BasicAttackImpact, enemy.position,
                hit_direction, hit_scale, hit_height);
    else
        EmitVfx(DomainSignalKind::ProjectileHit, enemy.position,
                hit_direction, hit_scale, hit_height);

    if (projectile.skill == SkillKind::RicochetArrow && projectile.bounce_remaining > 0)
    {
        if (HasUpgrade(projectile.upgrade_mask, 3))
        {
            if (enemy.status.bleed_count == 0)
                ApplyBleed(enemy, EffectiveAttack(), 1, projectile.skill, 2);
        }
        if (HasUpgrade(projectile.upgrade_mask, 3) && runtime.transfer_count < 3)
        {
            ++projectile.bounce_remaining;
            ++runtime.transfer_count;
            RecordUpgradeEffect(SkillKind::RicochetArrow, 2,
                                UpgradeEffectMetric::ExtraBounces);
        }
        --projectile.bounce_remaining;
        EnemyActor *next{};
        auto best_bleed = false;
        auto best_distance = 36.0f;
        const auto excluded = std::span(projectile.hit_ids);
        for (auto &candidate : enemies)
        {
            if (candidate.dead ||
                std::ranges::find(excluded, candidate.id.value) != excluded.end())
                continue;
            const auto distance = DistanceSquared(projectile.position, candidate.position);
            if (distance > 36.0f) continue;
            const auto bleeding = candidate.status.bleed_count > 0;
            if (!next || (bleeding && !best_bleed) ||
                (bleeding == best_bleed && distance < best_distance))
            {
                next = &candidate;
                best_bleed = bleeding;
                best_distance = distance;
            }
        }
        if (next)
        {
            EmitVfxLine(DomainSignalKind::RicochetLinked, projectile.position,
                        next->position);
            if (projectile.origin == EffectOrigin::Original &&
                HasUpgrade(projectile.upgrade_mask, 7))
            {
                Tick *longest{};
                for (std::size_t index = 0; index < player.cooldowns.size(); ++index)
                {
                    if (index == static_cast<std::size_t>(
                                     SkillKind::RicochetArrow) - 1)
                        continue;
                    if (!longest || player.cooldowns[index] > *longest)
                        longest = &player.cooldowns[index];
                }
                if (longest)
                {
                    const auto before = *longest;
                    *longest = before > 12 ? before - 12 : 0;
                    RecordUpgradeEffect(SkillKind::RicochetArrow, 6,
                                        UpgradeEffectMetric::CooldownTicksSaved,
                                        before - *longest);
                }
            }
            if (HasUpgrade(projectile.upgrade_mask, 4))
            {
                if (!enemy.status.burn)
                    ApplyBurn(enemy, EffectiveAttack(), false, Seconds(4.0f),
                              projectile.skill, 3);
                projectile.carried_burn_attack = enemy.status.burn->attack_snapshot;
                projectile.carried_burn_expires = enemy.status.burn->expires;
            }
            if (projectile.origin == EffectOrigin::Original &&
                HasUpgrade(projectile.upgrade_mask, 2) && projectile.hit_count == 1)
            {
                if (auto *branch = FireProjectile(
                        projectile.skill, projectile.position,
                        Normalize(Subtract(next->position, projectile.position)), 1.0f,
                        EffectOrigin::Derived, projectile.cast_id, 0, true, 1))
                {
                    branch->homing = true;
                    branch->homing_target = next->id.value;
                    branch->bounce_remaining = 2;
                }
            }
            projectile.homing = true;
            projectile.homing_target = next->id.value;
            projectile.velocity = Multiply(Normalize(Subtract(next->position,
                                                              projectile.position)),
                                           data.skills[5].projectile_speed);
            projectile.remaining_range = 6.0f;
            return;
        }
        if (projectile.origin == EffectOrigin::Original)
        {
            runtime.terminal_target = enemy.id.value;
        }
    }
    if (projectile.skill == SkillKind::RicochetArrow &&
        HasUpgrade(projectile.upgrade_mask, 1) && !projectile.returning)
    {
        projectile.returning = true;
        projectile.source_upgrade = 0;
        projectile.homing = false;
        projectile.homing_target = 0;
        projectile.hit_count = 0;
        projectile.hit_ids.clear();
        projectile.pierce_remaining = 5;
        projectile.damage = RoundDamage(EffectiveAttack() * 0.8f);
        projectile.remaining_range = std::max(0.1f,
            Length(Subtract(player.position, projectile.position)));
        projectile.velocity = Multiply(
            Normalize(Subtract(player.position, projectile.position)),
            data.skills[5].projectile_speed);
        return;
    }
    if (projectile.pierce_remaining > 0 && (!enemy.boss || projectile.pierce_remaining != 255))
    {
        --projectile.pierce_remaining;
    }
    else if (projectile.pierce_remaining == 255 && !enemy.boss)
    {
    }
    else
    {
        projectile.dead = true;
    }
}

void GameSimulation::SimulationWorld::ExplodeProjectile(ProjectileActor &projectile)
{
    if (projectile.explosion_radius <= 0.0f)
    {
        return;
    }
    if (projectile.origin == EffectOrigin::Original &&
        projectile.skill == SkillKind::ExplosiveArrow &&
        HasUpgrade(projectile.upgrade_mask, 6))
    {
        EmitVfx(DomainSignalKind::Pull, projectile.position, {},
                projectile.explosion_radius, 0.12f);
        for (auto &enemy : enemies)
        {
            if (!enemy.dead && !enemy.boss &&
                DistanceSquared(projectile.position, enemy.position) <= 6.25f)
            {
                const auto before = enemy.position;
                enemy.position = Add(enemy.position,
                    Multiply(Normalize(Subtract(projectile.position, enemy.position)),
                             std::min(2.5f, Length(Subtract(projectile.position,
                                                            enemy.position)))));
                RecordUpgradeDisplacement(SkillKind::ExplosiveArrow, 5, before,
                                          enemy.position);
            }
        }
    }
    const auto explosion_source =
        projectile.explosion_source_upgrade < kUpgradeCount
            ? projectile.explosion_source_upgrade
            : projectile.source_upgrade;
    RecordUpgradeEffect(projectile.skill, explosion_source,
                        UpgradeEffectMetric::ExplosionsCreated);
    DamageArea(projectile.position, projectile.explosion_radius,
               projectile.explosion_damage != 0 ? projectile.explosion_damage
                                                : projectile.damage,
               projectile.skill, projectile.origin, projectile.cast_id, 0, false, 0.0f, 0,
               explosion_source,
               projectile.source_relic);
    if (projectile.origin == EffectOrigin::Original &&
        projectile.skill == SkillKind::ExplosiveArrow)
    {
        if (HasUpgrade(projectile.upgrade_mask, 1))
        {
            Schedule({tick + 18, ScheduledKind::Explosion, projectile.skill,
                      projectile.position, {}, 3.0f, 4.5f, 0.0f, 1,
                      0, EffectOrigin::Derived,
                      projectile.cast_id, 0});
        }
        if (HasUpgrade(projectile.upgrade_mask, 2))
        {
            for (std::uint32_t index = 0; index < 3; ++index)
            {
                Schedule({tick + 9, ScheduledKind::Explosion, projectile.skill,
                          Add(projectile.position, Multiply(
                              Rotate({0.0f, 1.0f}, index * 120.0f), 1.5f)),
                          {}, 1.5f, 4.5f, 0.0f, 1, 0,
                          EffectOrigin::Derived, projectile.cast_id, 1});
            }
        }
        if (HasUpgrade(projectile.upgrade_mask, 3))
        {
            for (std::uint32_t index = 0; index < 8; ++index)
            {
                if (auto *fragment = FireProjectile(
                        projectile.skill, projectile.position,
                        Rotate({0.0f, 1.0f}, index * 45.0f), 0.7f,
                        EffectOrigin::Derived, projectile.cast_id, 0, true, 2))
                {
                    fragment->radius *= 2.5f;
                    fragment->pierce_remaining = 1;
                }
            }
        }
        if (HasUpgrade(projectile.upgrade_mask, 4))
        {
            SpawnArea(AreaKind::Damage, projectile.skill, projectile.position,
                      3.0f, 0.35f, 4.0f, 0.0f, EffectOrigin::Derived,
                      projectile.cast_id, 0, 0.0f, 0.0f, 0.0f, true, 3);
        }
        if (HasUpgrade(projectile.upgrade_mask, 5))
        {
            std::uint8_t blood_explosions{};
            for (auto &enemy : enemies)
            {
                if (blood_explosions == 8) break;
                if (!enemy.dead &&
                    DistanceSquared(projectile.position, enemy.position) <= 9.0f)
                {
                    if (enemy.status.bleed_count == 0)
                        ApplyBleed(enemy, EffectiveAttack(), 1, projectile.skill, 4);
                    DamageArea(enemy.position, 2.0f,
                               RoundDamage(EffectiveAttack() * 1.2f),
                               projectile.skill, EffectOrigin::Derived,
                               projectile.cast_id, 0, false, 0.0f, 0, 4);
                    EmitVfx(DomainSignalKind::SmallExplosion, enemy.position,
                            {}, 1.0f, 0.15f);
                    RecordUpgradeEffect(SkillKind::ExplosiveArrow, 4,
                                        UpgradeEffectMetric::ExplosionsCreated);
                    ++blood_explosions;
                }
            }
        }
    }
    EmitVfx(projectile.skill == SkillKind::ExplosiveArrow &&
                    projectile.origin == EffectOrigin::Original
                ? DomainSignalKind::ExplosiveArrowMain
                : DomainSignalKind::LargeExplosion,
            projectile.position, {}, projectile.explosion_radius / 3.0f, 0.15f);
}

void GameSimulation::SimulationWorld::CollisionHitPhase()
{
    const auto initial_projectile_count = projectiles.size();
    for (std::size_t projectile_index = 0;
         projectile_index < initial_projectile_count; ++projectile_index)
    {
        auto &projectile = projectiles[projectile_index];
        if (projectile.dead) continue;
        if (projectile.player_owned)
        {
            const auto coordinate = [this](float value) {
                return std::clamp(static_cast<int>(std::floor(
                                      (value + data.arena_half_extent) /
                                      kGridCellSize)),
                                  0, kGridDimension - 1);
            };
            constexpr float kLargestEnemyRadius = 1.1f;
            const auto margin = projectile.radius + kLargestEnemyRadius;
            const auto minimum_x = coordinate(std::min(projectile.previous_position.x,
                                                        projectile.position.x) - margin);
            const auto maximum_x = coordinate(std::max(projectile.previous_position.x,
                                                        projectile.position.x) + margin);
            const auto minimum_y = coordinate(std::min(projectile.previous_position.y,
                                                        projectile.position.y) - margin);
            const auto maximum_y = coordinate(std::max(projectile.previous_position.y,
                                                        projectile.position.y) + margin);
            collision_candidates.clear();
            for (auto y = minimum_y; y <= maximum_y; ++y)
            {
                for (auto x = minimum_x; x <= maximum_x; ++x)
                {
                    const auto &cell = enemy_grid[static_cast<std::size_t>(
                        y * kGridDimension + x)];
                    collision_candidates.insert(collision_candidates.end(), cell.begin(),
                                                cell.end());
                }
            }
            const auto travel = Subtract(projectile.position,
                                         projectile.previous_position);
            const auto travel_length_squared = LengthSquared(travel);
            std::ranges::sort(collision_candidates, [&](std::size_t left,
                                                        std::size_t right) {
                const auto projection = [&](std::size_t index) {
                    if (travel_length_squared <= 0.000001f) return 0.0f;
                    const auto offset = Subtract(enemies[index].position,
                                                 projectile.previous_position);
                    return (offset.x * travel.x + offset.y * travel.y) /
                           travel_length_squared;
                };
                const auto left_projection = projection(left);
                const auto right_projection = projection(right);
                if (left_projection != right_projection)
                    return left_projection < right_projection;
                return enemies[left].id.value < enemies[right].id.value;
            });
            for (const auto enemy_index : collision_candidates)
            {
                auto &enemy = enemies[enemy_index];
                if (enemy.dead || AlreadyHit(projectile, enemy.id.value)) continue;
                const auto combined = projectile.radius + (enemy.boss ? 1.1f : 0.45f);
                if (SegmentCircle(projectile.previous_position, projectile.position,
                                  enemy.position, combined))
                {
                    OnProjectileHit(projectile, enemy);
                    if (projectile.dead)
                    {
                        ExplodeProjectile(projectile);
                        break;
                    }
                }
            }
        }
        else if (SegmentCircle(projectile.previous_position, projectile.position,
                               player.position, projectile.radius + 0.4f))
        {
            DealDamage(0, projectile.damage, SkillKind::Count,
                       EffectOrigin::Original, projectile.cast_id, 0, false, 0.0f, 0,
                       kNoTelemetrySource, kNoTelemetrySource,
                       projectile.source_enemy);
            projectile.dead = true;
        }
        if (projectile.remaining_range <= 0.0f)
        {
            if (projectile.skill == SkillKind::PiercingShot &&
                projectile.origin == EffectOrigin::Original &&
                HasUpgrade(projectile.upgrade_mask, 2))
            {
                const auto direction = Normalize(projectile.velocity);
                for (const auto angle : {-20.0f, 20.0f})
                {
                    auto child_direction = Rotate(direction, angle);
                    const auto *target = NearestEnemy(projectile.position, 12.0f);
                    if (target)
                        child_direction = Normalize(Subtract(target->position,
                                                             projectile.position));
                    if (auto *branch = FireProjectile(
                            projectile.skill, projectile.position, child_direction,
                            0.9f, EffectOrigin::Derived, projectile.cast_id, 0,
                            true, 1); branch && target)
                    {
                        branch->homing = true;
                        branch->homing_target = target->id.value;
                    }
                }
                projectile.dead = true;
            }
            else if (projectile.skill == SkillKind::MultiShot &&
                     HasUpgrade(projectile.upgrade_mask, 8) &&
                     projectile.hit_count == 0)
            {
                if (auto *target = NearestEnemy(projectile.position, 12.0f))
                {
                    if (auto *arrow = FireProjectile(
                            projectile.skill, projectile.position,
                            Normalize(Subtract(target->position,
                                               projectile.position)),
                            0.55f, EffectOrigin::Derived,
                            projectile.cast_id, 0, true, 7))
                    {
                        arrow->homing = true;
                        arrow->homing_target = target->id.value;
                    }
                }
                projectile.dead = true;
            }
            else if (projectile.skill == SkillKind::RicochetArrow &&
                     !projectile.returning &&
                     HasUpgrade(projectile.upgrade_mask, 1))
            {
                projectile.returning = true;
                projectile.source_upgrade = 0;
                projectile.hit_count = 0;
                projectile.hit_ids.clear();
                projectile.pierce_remaining = 5;
                projectile.remaining_range = std::max(
                    0.1f, Length(Subtract(player.position, projectile.position)));
                projectile.velocity = Multiply(
                    Normalize(Subtract(player.position, projectile.position)),
                    data.skills[5].projectile_speed);
                projectile.damage = RoundDamage(EffectiveAttack() * 2.5f);
                projectile.source_upgrade = 0;
            }
            else if (projectile.skill == SkillKind::RicochetArrow &&
                     projectile.returning)
            {
                projectile.dead = true;
            }
            else if (projectile.skill == SkillKind::BasicAttack &&
                HasUpgrade(projectile.upgrade_mask, 7) && !projectile.returning &&
                projectile.hit_count == 0)
            {
                if (auto *target = NearestEnemy(projectile.position,
                                                data.skills[0].range))
                {
                    projectile.returning = true;
                    projectile.source_upgrade = 6;
                    projectile.damage = RoundDamage(EffectiveAttack() * 1.75f);
                    projectile.remaining_range = data.skills[0].range;
                    projectile.velocity = Multiply(
                        Normalize(Subtract(target->position, projectile.position)),
                        data.skills[0].projectile_speed);
                    projectile.radius *= 2.0f;
                    projectile.homing = true;
                    projectile.homing_target = target->id.value;
                }
                else
                {
                    projectile.dead = true;
                }
            }
            else
            {
                ExplodeProjectile(projectile);
                projectile.dead = true;
            }
        }
    }

    std::vector<ScheduledAction> pending_areas;
    const auto initial_area_count = areas.size();
    for (std::size_t area_index = 0; area_index < initial_area_count; ++area_index)
    {
        auto &area = areas[area_index];
        if (area.dead || tick < area.active_tick || tick < area.next_tick) continue;
        if (area.kind == AreaKind::Trap && tick == area.active_tick)
            EmitVfx(DomainSignalKind::TrapArmed, area.position, {},
                    area.radius / 2.0f, 0.12f);
        if (area.kind == AreaKind::EnemyDamage)
        {
            if (area.ring_outer_radius > 0.0f)
            {
                const auto duration = std::max<Tick>(area.expires - area.active_tick, 1);
                const auto progress = std::clamp(
                    static_cast<float>(tick - area.active_tick) /
                        static_cast<float>(duration),
                    0.0f, 1.0f);
                const auto radius = std::lerp(area.ring_inner_radius,
                                              area.ring_outer_radius, progress);
                const auto delta = Subtract(player.position, area.position);
                auto angle = std::atan2(delta.y, delta.x) * 180.0f / kPi;
                if (angle < 0.0f) angle += 360.0f;
                const auto spacing = 360.0f / area.safe_gap_count;
                const auto offset = static_cast<float>(area.cast_id % 360);
                auto nearest_gap = std::fmod(angle - offset + spacing * 0.5f + 360.0f,
                                             spacing) - spacing * 0.5f;
                const auto safe = std::abs(nearest_gap) <= area.safe_gap_degrees * 0.5f;
                if (!safe && std::abs(Length(delta) - radius) <= 0.75f &&
                    IncrementAreaHit(area.id.value, 0) == 1)
                {
                    DealDamage(0, area.damage, SkillKind::Count,
                               area.origin, area.cast_id, 0, false, 0.0f, 0,
                               kNoTelemetrySource, kNoTelemetrySource,
                               area.source_enemy);
                }
            }
            else if (DistanceSquared(area.position, player.position) <=
                     area.radius * area.radius)
            {
                DealDamage(0, area.damage, SkillKind::Count,
                           area.origin, area.cast_id, 0, false, 0.0f, 0,
                           kNoTelemetrySource, kNoTelemetrySource,
                           area.source_enemy);
            }
        }
        else if (area.kind == AreaKind::Trap)
        {
            if (auto *target = NearestEnemy(area.position, area.radius))
            {
                if (HasUpgrade(area.upgrade_mask, 6))
                {
                    EmitVfx(DomainSignalKind::Pull, area.position, {},
                            area.effect_radius, 0.12f);
                    for (auto &enemy : enemies)
                    {
                        if (!enemy.dead && !enemy.boss &&
                            DistanceSquared(area.position, enemy.position) <=
                                area.effect_radius * area.effect_radius)
                        {
                            const auto delta = Subtract(area.position, enemy.position);
                            const auto before = enemy.position;
                            enemy.position = Add(enemy.position, Multiply(
                                Normalize(delta), std::min(2.0f, Length(delta))));
                            RecordUpgradeDisplacement(SkillKind::Trap, 5, before,
                                                      enemy.position);
                        }
                    }
                }
                if (HasUpgrade(area.upgrade_mask, 7))
                {
                    target->marked_by_skill = SkillKind::Trap;
                    target->marked_damage_coefficient = 4.0f;
                    target->mark_expires = tick + Seconds(8.0f);
                    EmitVfx(DomainSignalKind::MarkApplied, target->position,
                            {}, 1.0f, target->boss ? 1.4f : 0.75f);
                    RecordUpgradeEffect(SkillKind::Trap, 6,
                                        UpgradeEffectMetric::ExplosionsCreated);
                    DealDamage(target->id.value,
                               RoundDamage(EffectiveAttack() * 3.0f),
                               SkillKind::Trap, EffectOrigin::Derived,
                               area.cast_id, 0, false, 0.0f, 0, 6);
                    RecordUpgradeEffect(SkillKind::Trap, 6,
                                        UpgradeEffectMetric::MarksApplied);
                }
                const auto damage_source = area.trigger_count > 0 &&
                                                   HasUpgrade(area.upgrade_mask, 3)
                                               ? std::uint8_t{2}
                                               : area.source_upgrade;
                DamageArea(area.position, area.effect_radius, area.damage, area.skill,
                           area.origin, area.cast_id,
                           HasUpgrade(area.upgrade_mask, 4) ? 3 : 0,
                           HasUpgrade(area.upgrade_mask, 5),
                           HasUpgrade(area.upgrade_mask, 6) ? 0.6f : area.slow_reduction,
                           HasUpgrade(area.upgrade_mask, 6) ? Seconds(2.0f)
                                                            : area.slow_duration,
                           damage_source, area.source_relic);
                if (HasUpgrade(area.upgrade_mask, 5))
                {
                    pending_areas.push_back(
                        {tick, ScheduledKind::Area, area.skill, target->position, {},
                         0.7f, 1.5f, 2.0f, 1, 0,
                         EffectOrigin::Derived, area.cast_id, 4});
                }
                ++area.trigger_count;
                if (HasUpgrade(area.upgrade_mask, 3) && area.trigger_count == 1)
                {
                    area.next_tick = tick + 120;
                }
                else
                {
                    area.dead = true;
                }
                EmitVfx(DomainSignalKind::TrapTriggered, target->position);
            }
            continue;
        }
        else if (area.kind == AreaKind::Slow)
        {
            EmitVfx(DomainSignalKind::SlowArea, area.position, {},
                    area.radius, 0.03f);
            for (auto &enemy : enemies)
            {
                if (!enemy.dead &&
                    DistanceSquared(area.position, enemy.position) <= area.radius * area.radius)
                {
                    enemy.status.slows.push_back(
                        {area.slow_reduction, tick + area.slow_duration,
                         area.skill, area.source_upgrade, area.source_relic});
                    EmitVfx(DomainSignalKind::SlowApplied, enemy.position, {},
                            enemy.boss ? 1.4f : 0.75f, 0.12f);
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
                EmitVfx(area.trigger_count == 0
                            ? DomainSignalKind::ArrowRainImpact
                            : DomainSignalKind::ArrowRainPulse,
                        area.position, {}, area.radius / 4.0f, 0.12f);
                if (area.trigger_count == 0 && HasUpgrade(area.upgrade_mask, 2))
                    EmitVfx(DomainSignalKind::Pull, area.position, {},
                            area.radius / 4.0f, 0.12f);
                if (HasUpgrade(area.upgrade_mask, 8) && area.trigger_count >= 6)
                    RecordUpgradeEffect(area.skill, 7,
                                        UpgradeEffectMetric::DurationTicksAdded,
                                        area.interval);
                auto &runtime = RuntimeForCast(area.cast_id, area.skill);
                for (auto &enemy : enemies)
                {
                    if (enemy.dead || DistanceSquared(area.position, enemy.position) >
                                          area.radius * area.radius)
                    {
                        continue;
                    }
                    if (area.trigger_count == 0 &&
                        HasUpgrade(area.upgrade_mask, 2) && !enemy.boss)
                    {
                        const auto delta = Subtract(area.position, enemy.position);
                        const auto before = enemy.position;
                        enemy.position = Add(enemy.position, Multiply(
                            Normalize(delta), std::min(2.0f, Length(delta))));
                        RecordUpgradeDisplacement(SkillKind::ArrowRain, 1, before,
                                                  enemy.position);
                    }
                    const auto hit_count = IncrementAreaHit(area.id.value,
                                                            enemy.id.value);
                    if (HasUpgrade(area.upgrade_mask, 4) && !enemy.status.burn)
                        ApplyBurn(enemy, EffectiveAttack(), false, Seconds(4.0f),
                                  area.skill, 3);
                    const auto damage_source = HasUpgrade(area.upgrade_mask, 8) &&
                                                       area.trigger_count >= 6
                                                   ? std::uint8_t{7}
                                                   : area.source_upgrade;
                    DealDamage(enemy.id.value, area.damage, area.skill, area.origin,
                               area.cast_id,
                                HasUpgrade(area.upgrade_mask, 3) && hit_count % 2 == 0 ? 3 : 0,
                               false, area.slow_reduction, area.slow_duration,
                               damage_source, area.source_relic);
                    if (HasUpgrade(area.upgrade_mask, 4) && enemy.status.burn &&
                        runtime.spawn_count < 4)
                    {
                        pending_areas.push_back(
                            {tick, ScheduledKind::Area, area.skill, enemy.position, {},
                              0.3f, 1.5f, 2.0f, 1, 0,
                              EffectOrigin::Derived, area.cast_id, 3});
                        ++runtime.spawn_count;
                    }
                }
                if (HasUpgrade(area.upgrade_mask, 5))
                {
                    if (auto *target = NearestEnemy(area.position, 7.0f))
                    {
                        if (auto *arrow = FireProjectile(
                                area.skill, area.position,
                                Normalize(Subtract(target->position, area.position)),
                                4.0f, EffectOrigin::Derived, area.cast_id, 0, true, 4))
                        {
                            arrow->homing = true;
                            arrow->homing_target = target->id.value;
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
                    EmitVfx(fire_area ? DomainSignalKind::FireAreaPulse
                                      : DomainSignalKind::DamageAreaPulse,
                            area.position, {}, area.radius / 1.5f, 0.12f);
                }
                if (area.half_length > 0.0f)
                {
                    const auto from = Subtract(
                        area.position, Multiply(area.direction, area.half_length));
                    const auto to = Add(
                        area.position, Multiply(area.direction, area.half_length));
                    for (const auto &enemy : enemies)
                    {
                        if (enemy.dead) continue;
                        const auto enemy_radius = enemy.boss ? 1.1f : 0.45f;
                        if (SegmentCircle(from, to, enemy.position,
                                          area.radius + enemy_radius))
                        {
                            DealDamage(enemy.id.value, area.damage, area.skill,
                                       area.origin, area.cast_id, 0,
                                       area.applies_burn, area.slow_reduction,
                                       area.slow_duration, area.source_upgrade,
                                       area.source_relic);
                        }
                    }
                }
                else
                {
                    DamageArea(area.position, area.radius, area.damage, area.skill,
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
                SpawnArea(AreaKind::Slow, area.skill, area.position, area.radius,
                          0.0f, 3.0f, 0.0f, EffectOrigin::Derived,
                          area.cast_id, 0, 0.4f, 1.0f, 0.0f, false, 7);
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

void GameSimulation::SimulationWorld::ApplyBleed(EnemyActor &enemy, float attack, std::uint8_t stacks,
                SkillKind source_skill,
                std::uint8_t source_upgrade,
                std::uint8_t source_relic)
{
    if (stacks != 0) EmitVfx(DomainSignalKind::BleedApplied, enemy.position);
    RecordUpgradeEffect(source_skill, source_upgrade,
                        UpgradeEffectMetric::BleedStacksApplied, stacks);
    for (std::uint8_t stack = 0; stack < stacks; ++stack)
    {
        BleedEffect effect{attack, tick + data.bleed_duration + 1,
                           tick + Seconds(0.1f), source_skill,
                           source_upgrade, source_relic};
        if (enemy.status.bleed_count < enemy.status.bleeds.size())
        {
            enemy.status.bleeds[enemy.status.bleed_count++] = effect;
        }
        else
        {
            const auto oldest = std::ranges::min_element(
                enemy.status.bleeds, {}, &BleedEffect::expires);
            *oldest = effect;
        }
    }
}

void GameSimulation::SimulationWorld::ApplyBurn(EnemyActor &enemy, float attack, bool propagated,
               Tick duration,
               SkillKind source_skill,
               std::uint8_t source_upgrade,
               std::uint8_t source_relic)
{
    EmitVfx(DomainSignalKind::BurnApplied, enemy.position);
    RecordUpgradeEffect(source_skill, source_upgrade,
                        UpgradeEffectMetric::BurnApplications);
    const BurnEffect incoming{attack, tick + duration + 1, tick + Seconds(0.1f),
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
    for (auto &enemy : enemies)
    {
        if (enemy.dead) continue;
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
            if (bleed.next_tick <= tick)
            {
                EmitVfx(DomainSignalKind::BleedTicked, enemy.position);
                DealDamage(enemy.id.value,
                           RoundDamage(bleed.attack_snapshot *
                                       data.bleed_tick_coefficient),
                           bleed.source_skill, EffectOrigin::DamageOverTime, 0,
                           0, false, 0.0f, 0,
                           bleed.source_upgrade, bleed.source_relic);
                bleed.next_tick += data.status_tick_interval;
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
                EmitVfx(DomainSignalKind::BurnTicked, enemy.position);
                DealDamage(enemy.id.value,
                           RoundDamage(enemy.status.burn->attack_snapshot *
                                       data.burn_tick_coefficient),
                           enemy.status.burn->source_skill,
                           EffectOrigin::DamageOverTime, 0, 0, false, 0.0f, 0,
                           enemy.status.burn->source_upgrade,
                           enemy.status.burn->source_relic);
                enemy.status.burn->next_tick += data.status_tick_interval;
            }
            if (enemy.status.burn)
                RecordUpgradeEffect(enemy.status.burn->source_skill,
                                    enemy.status.burn->source_upgrade,
                                    UpgradeEffectMetric::BurnActiveTicks);
        }
    }

    combat.Order();
    for (std::size_t event_index = 0; event_index < combat.commands.size(); ++event_index)
    {
        const auto event = combat.commands[event_index];
        if (event.target == 0)
        {
            auto source_enemy = event.source_enemy;
            if (source_enemy == kNoTelemetrySource)
            {
                if (const auto *source = FindEnemy(event.cast_id))
                    source_enemy = static_cast<std::uint8_t>(EnemyTelemetryIndex(*source));
            }
            const auto applied_damage = config.scenario.player_invulnerable
                ? static_cast<std::uint64_t>(event.amount)
                : static_cast<std::uint64_t>(
                      std::min(event.amount, std::max(player.health, 0)));
            if (!config.scenario.player_invulnerable)
                player.health -= event.amount;
            damage_taken += event.amount;
            EmitVfx(DomainSignalKind::PlayerDamaged, player.position, {},
                    1.0f, 0.1f);
            if (source_enemy < kEnemyArchetypeCount)
            {
                const auto hit_key = (static_cast<std::uint64_t>(source_enemy) << 56) |
                                     (event.cast_id & 0x00FFFFFFFFFFFFFFull);
                if (std::ranges::find(observer.enemy_hit_casts, hit_key) ==
                    observer.enemy_hit_casts.end())
                {
                    observer.enemy_hit_casts.push_back(hit_key);
                    ++balance.enemy_hits[source_enemy];
                }
                balance.enemy_damage[source_enemy] += applied_damage;
            }
            DispatchPlayerDamagedRules();
            continue;
        }
        auto *enemy = FindEnemy(event.target);
        if (!enemy || enemy->dead || tick < enemy->invulnerable_until)
        {
            continue;
        }
        const auto applied_damage = std::min(event.amount, std::max(enemy->health, 0));
        enemy->health -= event.amount;
        damage_dealt += applied_damage;
        if (event.skill < SkillKind::Count)
        {
            const auto skill_index = static_cast<std::size_t>(event.skill);
            damage_by_skill[skill_index] += applied_damage;
            if (enemy->boss)
                balance.skill_boss_damage[skill_index] += applied_damage;
            ++balance.skill_hit_events[skill_index];
            if (event.cast_id != 0)
            {
                if (std::ranges::find(observer.player_hit_casts, event.cast_id) ==
                    observer.player_hit_casts.end())
                {
                    observer.player_hit_casts.push_back(event.cast_id);
                    ++balance.skill_casts_with_hit[skill_index];
                }
            }
            if (event.source_upgrade < kUpgradeCount)
            {
                balance.upgrade_damage[skill_index][event.source_upgrade] += applied_damage;
                ++balance.upgrade_triggers[skill_index][event.source_upgrade];
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
            balance.relic_damage[event.source_relic] += applied_damage;
            ++balance.relic_triggers[event.source_relic];
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
        if (event.origin == EffectOrigin::Original) balance.direct_damage += applied_damage;
        else if (event.origin == EffectOrigin::Derived) balance.derived_damage += applied_damage;
        else balance.damage_over_time += applied_damage;
        const auto attack_points =
            player.stats[static_cast<std::size_t>(StatKind::AttackPower)];
        if (attack_points != 0)
        {
            const auto multiplier = 1.0 + 0.06 * attack_points;
            const auto without_stat = static_cast<std::uint64_t>(
                std::llround(static_cast<double>(applied_damage) / multiplier));
            balance.stat_utility[static_cast<std::size_t>(StatKind::AttackPower)] +=
                static_cast<std::uint64_t>(applied_damage) -
                std::min(static_cast<std::uint64_t>(applied_damage), without_stat);
        }
        enemy->last_damage_skill = event.skill;
        enemy->last_damage_origin = event.origin;
        enemy->last_damage_cast = event.cast_id;
        enemy->last_damage_upgrade = event.source_upgrade;
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
            ApplyBleed(*enemy, EffectiveAttack(), event.bleed_stacks, event.skill,
                       source_upgrade, event.source_relic);
        }
        if (event.burn)
        {
            const auto had_bleed = enemy->status.bleed_count != 0;
            auto source_upgrade = event.source_upgrade;
            if (event.skill == SkillKind::BasicAttack) source_upgrade = 4;
            else if (event.skill == SkillKind::ExplosiveArrow) source_upgrade = 3;
            else if (event.skill == SkillKind::Trap) source_upgrade = 4;
            else if (event.skill == SkillKind::ArrowRain) source_upgrade = 3;
            ApplyBurn(*enemy, EffectiveAttack(), false, data.burn_duration, event.skill,
                      source_upgrade, event.source_relic);
            if (had_bleed && event.origin == EffectOrigin::Original)
                DispatchStatusAppliedRules(*enemy, event, source_upgrade);
        }
        if (event.slow_reduction > 0.0f)
        {
            auto source_upgrade = event.source_upgrade;
            if (event.skill == SkillKind::ArrowRain) source_upgrade = 5;
            else if (event.skill == SkillKind::Trap) source_upgrade = 5;
            enemy->status.slows.push_back(
                {event.slow_reduction, tick + event.slow_duration,
                 event.skill, source_upgrade, event.source_relic});
            EmitVfx(DomainSignalKind::SlowApplied, enemy->position, {},
                    1.0f, enemy->boss ? 1.4f : 0.75f);
            RecordUpgradeEffect(event.skill, source_upgrade,
                                UpgradeEffectMetric::SlowApplications);
            RecordUpgradeEffect(event.skill, source_upgrade,
                                UpgradeEffectMetric::SlowTargetTicks,
                                event.slow_duration);
        }
        const auto original_player_hit = event.skill < SkillKind::Count &&
                                         event.origin == EffectOrigin::Original;
        if (original_player_hit && enemy->marked_by_skill != SkillKind::Count &&
            enemy->marked_by_skill != event.skill && tick <= enemy->mark_expires)
        {
            const auto coefficient = enemy->marked_damage_coefficient;
            const auto marked_skill = enemy->marked_by_skill;
            enemy->marked_by_skill = SkillKind::Count;
            enemy->mark_expires = 0;
            EmitVfx(DomainSignalKind::MarkTriggered, enemy->position, {},
                    1.0f, enemy->boss ? 1.4f : 0.75f);
            RecordUpgradeEffect(marked_skill, 6,
                                UpgradeEffectMetric::ExplosionsCreated);
            DamageArea(enemy->position, 2.0f,
                       RoundDamage(EffectiveAttack() * coefficient), marked_skill,
                       EffectOrigin::Derived, event.cast_id, 0, false, 0.0f, 0,
                       6);
        }
    }
    combat.Clear();
}

std::uint64_t GameSimulation::SimulationWorld::Heal(std::int32_t amount)
{
    const auto before = player.health;
    player.health = std::min(player.max_health, player.health + amount);
    const auto applied = static_cast<std::uint64_t>(
        std::max(0, player.health - before));
    healing += applied;
    if (applied != 0) EmitVfx(DomainSignalKind::PlayerHealed, player.position);
    return applied;
}

void GameSimulation::SimulationWorld::CancelChargedShot() noexcept
{
    player.charging = false;
    player.charging_skill = SkillKind::Count;
    player.charging_slot = 0xFF;
}

std::uint32_t GameSimulation::SimulationWorld::EnemyExperience(const EnemyActor &enemy) const noexcept
{
    if (enemy.boss)
    {
        if (*enemy.boss == BossKind::FiveMinute) return 100;
        if (*enemy.boss == BossKind::TenMinute) return 200;
        return 0;
    }
    return enemy.kind == EnemyKind::Melee ? 1u : 2u;
}

void GameSimulation::SimulationWorld::HandleEnemyDeath(EnemyActor &enemy)
{
    EmitVfx(DomainSignalKind::EnemyDied, enemy.position,
            {0.0f, 1.0f}, enemy.boss ? 1.5f : 1.0f);
    ++kills;
    const auto enemy_index = EnemyTelemetryIndex(enemy);
    ++balance.enemy_killed[enemy_index];
    balance.enemy_lifetime_ticks[enemy_index] += tick - enemy.spawned_tick;
    if (enemy.last_damage_skill < SkillKind::Count)
        ++balance.skill_kills[static_cast<std::size_t>(enemy.last_damage_skill)];
    RecordUpgradeEffect(enemy.last_damage_skill, enemy.last_damage_upgrade,
                        UpgradeEffectMetric::Kills);
    CastRuntime *runtime{};
    if (enemy.last_damage_cast != 0)
    {
        const auto iterator = std::ranges::find(cast_runtime,
                                                enemy.last_damage_cast,
                                                &CastRuntime::cast_id);
        if (iterator != cast_runtime.end()) runtime = &*iterator;
    }
    const auto xp = EnemyExperience(enemy);
    if (xp)
    {
        ++balance.pickup_drop_attempts[static_cast<std::size_t>(PickupKind::Experience)];
        SpawnPickup(PickupKind::Experience, enemy.position, xp);
    }
    if (enemy.boss)
    {
        if (*enemy.boss != BossKind::Final &&
            config.scenario.progression_enabled)
        {
            ++balance.pickup_drop_attempts[
                static_cast<std::size_t>(PickupKind::RelicChest)];
            SpawnPickup(PickupKind::RelicChest, enemy.position, 1, true);
            Heal(RoundDamage(player.max_health * 0.25f));
        }
    }
    else
    {
        if (runtime)
        {
            ++runtime->kills;
            if (runtime->skill == SkillKind::ChargedShot && runtime->full_charge &&
                HasUpgrade(runtime->upgrade_mask, 8) &&
                runtime->kills >= 3 && !runtime->refund_triggered)
            {
                auto &cooldown = player.cooldowns[
                    static_cast<std::size_t>(SkillKind::ChargedShot) - 1];
                const auto before = cooldown;
                cooldown -= cooldown * 40 / 100;
                RecordUpgradeEffect(SkillKind::ChargedShot, 7,
                                    UpgradeEffectMetric::CooldownTicksSaved,
                                    before - cooldown);
                runtime->refund_triggered = true;
            }
            if (runtime->skill == SkillKind::RicochetArrow &&
                HasUpgrade(runtime->upgrade_mask, 5) &&
                enemy.last_damage_origin == EffectOrigin::Original &&
                runtime->spawn_count < 9)
            {
                std::array<std::uint64_t, 4> excluded{enemy.id.value};
                for (std::size_t index = 0; index < 3; ++index)
                {
                    if (runtime->spawn_count == 9) break;
                    auto *target = NearestEnemy(
                        enemy.position, 12.0f,
                        std::span(excluded.data(), index + 1));
                    if (!target) break;
                    if (auto *arrow = FireProjectile(
                            SkillKind::RicochetArrow, enemy.position,
                            Normalize(Subtract(target->position, enemy.position)),
                            1.0f, EffectOrigin::Derived, runtime->cast_id, 0,
                            true, 4))
                    {
                        arrow->homing = true;
                        arrow->homing_target = target->id.value;
                        excluded[index + 1] = target->id.value;
                        ++runtime->spawn_count;
                    }
                }
            }
            if (runtime->skill == SkillKind::RicochetArrow &&
                enemy.last_damage_origin == EffectOrigin::Original &&
                HasUpgrade(runtime->upgrade_mask, 6) &&
                !runtime->refund_triggered)
            {
                if (auto *target = NearestEnemy(enemy.position, 12.0f))
                {
                    if (auto *arrow = FireProjectile(
                            SkillKind::RicochetArrow, enemy.position,
                             Normalize(Subtract(target->position, enemy.position)),
                              2.4f, EffectOrigin::Derived, runtime->cast_id, 0, true, 5))
                    {
                        arrow->homing = true;
                        arrow->homing_target = target->id.value;
                        arrow->bounce_remaining = 2;
                        runtime->refund_triggered = true;
                    }
                }
            }
            if (runtime->skill == SkillKind::ArrowRain &&
                HasUpgrade(runtime->upgrade_mask, 7) &&
                enemy.last_damage_origin == EffectOrigin::Original &&
                runtime->transfer_count < 6)
            {
                const auto source = std::ranges::find_if(areas, [&](const AreaActor &area) {
                    return !area.dead && area.cast_id == runtime->cast_id &&
                           area.skill == SkillKind::ArrowRain &&
                           area.origin == EffectOrigin::Original;
                });
                if (source != areas.end())
                {
                    EnemyActor *target{};
                    auto best = std::numeric_limits<float>::max();
                    for (auto &candidate : enemies)
                    {
                        if (candidate.dead ||
                            DistanceSquared(source->position, candidate.position) <=
                                source->radius * source->radius)
                        {
                            continue;
                        }
                        const auto distance = DistanceSquared(enemy.position,
                                                              candidate.position);
                        if (distance < best)
                        {
                            target = &candidate;
                            best = distance;
                        }
                    }
                    if (target)
                    {
                        if (auto *arrow = FireProjectile(
                                SkillKind::ArrowRain, enemy.position,
                                 Normalize(Subtract(target->position, enemy.position)),
                                  2.0f, EffectOrigin::Derived, runtime->cast_id, 0, true, 6))
                        {
                            arrow->homing = true;
                            arrow->homing_target = target->id.value;
                            ++runtime->transfer_count;
                        }
                    }
                }
            }
            if (runtime->skill == SkillKind::Trap &&
                HasUpgrade(runtime->upgrade_mask, 8) &&
                enemy.last_damage_origin == EffectOrigin::Original &&
                runtime->spawn_count < 3)
            {
                if (auto *target = NearestEnemy(enemy.position, 120.0f))
                {
                    SpawnArea(AreaKind::Trap, SkillKind::Trap, target->position,
                              1.5f, 5.0f, 12.0f, 0.6f,
                              EffectOrigin::Derived, runtime->cast_id, 0,
                              0.3f, 3.0f, 2.0f, false, 7);
                    ++runtime->spawn_count;
                }
            }
        }
        ++normal_chest_kills;
        ++balance.pickup_drop_attempts[static_cast<std::size_t>(PickupKind::Heal)];
        ++balance.pickup_drop_attempts[static_cast<std::size_t>(PickupKind::Magnet)];
        const auto utility_drop = [&](std::uint32_t &misses,
                                      std::uint64_t purpose,
                                      float chance_multiplier) {
            const auto chance = std::min(
                1.0f, (data.utility_pickup_base_chance +
                       data.utility_pickup_miss_increment * misses) *
                          chance_multiplier);
            const auto dropped = RandomUnit(enemy.random_key, purpose) < chance;
            misses = dropped ? 0 : misses + 1;
            return dropped;
        };
        if (utility_drop(heal_pickup_misses, 0x4845414Cull,
                         data.heal_pickup_chance_multiplier))
        {
            SpawnPickup(PickupKind::Heal, enemy.position, 8);
        }
        if (utility_drop(magnet_pickup_misses, 0x4D41474E4554ull,
                         data.magnet_pickup_chance_multiplier))
        {
            SpawnPickup(PickupKind::Magnet, enemy.position, 1);
        }
        if (config.scenario.progression_enabled &&
            player.relic_mask != 0x0FFFu)
        {
            ++balance.pickup_drop_attempts[
                static_cast<std::size_t>(PickupKind::RelicChest)];
            const auto chance = std::min(
                1.0f, data.relic_chest_base_chance +
                          data.relic_chest_miss_increment * normal_chest_kills);
            if (RandomUnit(enemy.random_key, 0x4348455354ull) < chance)
            {
                SpawnPickup(PickupKind::RelicChest, enemy.position, 1);
                normal_chest_kills = 0;
            }
        }
    }
    DispatchEnemyKilledRules(enemy, runtime);
}

void GameSimulation::SimulationWorld::DeathDropPhase()
{
    bool final_dead{};
    for (auto &enemy : enemies)
    {
        if (!enemy.dead && enemy.health <= 0)
        {
            enemy.dead = true;
            HandleEnemyDeath(enemy);
            final_dead |= enemy.boss && *enemy.boss == BossKind::Final;
        }
        if (enemy.boss && *enemy.boss == BossKind::Final && enemy.final_phase == 1 &&
            enemy.health > 0 && enemy.health * 2 <= enemy.max_health)
        {
            enemy.final_phase = 2;
            enemy.invulnerable_until = tick + 90;
            enemy.pattern_ready = enemy.invulnerable_until;
            enemy.dash_until = 0;
            enemy.velocity = {};
            std::erase_if(boss_actions, [&](const BossAction &action) {
                return action.boss_id == enemy.id.value;
            });
            std::erase_if(projectiles, [](const ProjectileActor &projectile) {
                return !projectile.player_owned;
            });
            std::erase_if(areas, [](const AreaActor &area) {
                return area.kind == AreaKind::EnemyDamage;
            });
            EmitVfx(DomainSignalKind::BossPhaseChanged, enemy.position,
                    {}, 1.0f, 1.25f);
        }
    }
    if (final_dead)
    {
        phase = SessionPhase::Victory;
    }
    else if (player.health <= 0)
    {
        const auto revived = DispatchPlayerDeathRules();
        if (!revived)
        {
            EmitVfx(DomainSignalKind::PlayerDied, player.position, {},
                    1.0f, 0.2f);
            phase = SessionPhase::Defeat;
        }
    }
}

void GameSimulation::SimulationWorld::HandleRadialBasicAttack(Tick release_ticks, std::uint64_t cast_id)
{
    const auto &relic = data.relics.radial_basic_attack;
    if (player.basic_sequence % relic.cadence_interval != 0) return;
    EmitVfx(DomainSignalKind::RadialArrowsCast, player.position,
            player.aim, 1.0f, 0.15f);
    RecordRelicEffect(RelicKind::RadialBasicAttack,
                      UpgradeEffectMetric::Activations);
    RecordRelicEffect(RelicKind::RadialBasicAttack,
                      UpgradeEffectMetric::ProjectilesCreated,
                      relic.direction_count);
    for (std::uint32_t index = 0; index < relic.direction_count; ++index)
    {
        Schedule({tick + release_ticks, ScheduledKind::Projectile,
                  SkillKind::BasicAttack, player.position,
                  Rotate({0.0f, 1.0f}, index * 360.0f / relic.direction_count),
                  relic.damage_multiplier, 0.0f, 0.0f, 1, 0,
                  EffectOrigin::Derived, cast_id, kNoTelemetrySource,
                  static_cast<std::uint8_t>(RelicKind::RadialBasicAttack)});
    }
}

void GameSimulation::SimulationWorld::HandleMovementEcho(Tick release_ticks, std::uint64_t cast_id)
{
    const auto &relic = data.relics.movement_echo;
    if (player.movement_since_echo < relic.required_distance) return;
    RecordRelicEffect(RelicKind::MovementEcho, UpgradeEffectMetric::Activations);
    RecordRelicEffect(RelicKind::MovementEcho,
                      UpgradeEffectMetric::ProjectilesCreated);
    Schedule({tick + release_ticks, ScheduledKind::Projectile,
              SkillKind::BasicAttack, player.one_second_ago, player.aim,
              relic.damage_multiplier, 0.0f, 0.0f, 1, 0,
              EffectOrigin::Derived, cast_id, kNoTelemetrySource,
              static_cast<std::uint8_t>(RelicKind::MovementEcho)});
    player.movement_since_echo = 0.0f;
}

void GameSimulation::SimulationWorld::HandleAlternatingSkills(SkillKind skill, std::size_t cooldown_index)
{
    const auto &relic = data.relics.alternating_skills;
    if (player.last_active == SkillKind::Count || player.last_active == skill ||
        tick - player.last_active_tick > relic.window_ticks) return;
    const auto before = player.cooldowns[cooldown_index];
    player.cooldowns[cooldown_index] -= static_cast<Tick>(
        player.cooldowns[cooldown_index] * relic.cooldown_refund_fraction);
    RecordRelicEffect(RelicKind::AlternatingSkills,
                      UpgradeEffectMetric::Activations);
    RecordRelicEffect(RelicKind::AlternatingSkills,
                      UpgradeEffectMetric::CooldownTicksSaved,
                      before - player.cooldowns[cooldown_index]);
}

void GameSimulation::SimulationWorld::HandleDamageKnockback()
{
    if (tick < player.damage_relic_ready) return;
    const auto &relic = data.relics.damage_knockback;
    EmitVfx(DomainSignalKind::DamagePush, player.position, {}, 1.0f, 0.15f);
    std::uint64_t affected{};
    for (auto &enemy : enemies)
    {
        if (enemy.dead || enemy.boss ||
            DistanceSquared(player.position, enemy.position) >
                relic.radius * relic.radius) continue;
        const auto before = enemy.position;
        const auto direction = Normalize(Subtract(enemy.position, player.position));
        enemy.position = Add(enemy.position, Multiply(direction, relic.push_distance));
        enemy.status.slows.push_back(
            {relic.slow_fraction, tick + relic.slow_duration_ticks});
        RecordRelicEffect(
            RelicKind::DamageKnockback,
            UpgradeEffectMetric::DisplacementMillimetres,
            static_cast<std::uint64_t>(std::llround(
                Length(Subtract(enemy.position, before)) * 1000.0f)));
        ++affected;
    }
    RecordRelicEffect(RelicKind::DamageKnockback,
                      UpgradeEffectMetric::Activations);
    RecordRelicEffect(RelicKind::DamageKnockback,
                      UpgradeEffectMetric::SlowApplications, affected);
    RecordRelicEffect(RelicKind::DamageKnockback,
                      UpgradeEffectMetric::SlowTargetTicks,
                      affected * relic.slow_duration_ticks);
    player.damage_relic_ready = tick + relic.cooldown_ticks;
}

void GameSimulation::SimulationWorld::HandleCombatHitChain(const EffectCommand &event)
{
    if (event.skill >= SkillKind::Count ||
        event.origin != EffectOrigin::Original) return;
    const auto &relic = data.relics.combat_hit_chain;
    ++player.combat_hit_progress;
    if (player.combat_hit_progress < relic.direct_hits_per_trigger) return;
    player.combat_hit_progress -= relic.direct_hits_per_trigger;
    RecordRelicEffect(RelicKind::CombatHitChain,
                      UpgradeEffectMetric::Activations);
    std::vector<std::uint64_t> selected;
    selected.reserve(relic.maximum_targets);
    while (selected.size() < relic.maximum_targets)
    {
        EnemyActor *target{};
        auto best_distance = relic.search_radius * relic.search_radius;
        for (auto &candidate : enemies)
        {
            if (candidate.dead || candidate.health <= 0) continue;
            const auto already_selected =
                std::ranges::find(selected, candidate.id.value) != selected.end();
            const auto distance = DistanceSquared(player.position,
                                                   candidate.position);
            if (!already_selected && distance <= best_distance)
            {
                target = &candidate;
                best_distance = distance;
            }
        }
        if (!target) break;
        selected.push_back(target->id.value);
        DealDamage(target->id.value,
                   RoundDamage(EffectiveAttack() * relic.damage_multiplier),
                   SkillKind::Count, EffectOrigin::Derived, event.cast_id, 0,
                   false, 0.0f, 0, kNoTelemetrySource,
                   static_cast<std::uint8_t>(RelicKind::CombatHitChain));
        EmitVfx(DomainSignalKind::CombatChainHit, target->position,
                {}, 0.5f, 0.15f);
    }
    RecordRelicEffect(RelicKind::CombatHitChain,
                      UpgradeEffectMetric::ExtraTargetsHit, selected.size());
}

void GameSimulation::SimulationWorld::HandleBleedBurnExplosion(EnemyActor &enemy,
                              const EffectCommand &event,
                              std::uint8_t source_upgrade)
{
    if (tick < enemy.bleed_burn_ready) return;
    const auto &relic = data.relics.bleed_burn_explosion;
    enemy.bleed_burn_ready = tick + relic.per_target_cooldown_ticks;
    DamageArea(enemy.position, relic.radius,
               RoundDamage(EffectiveAttack() * relic.damage_multiplier),
               event.skill, EffectOrigin::Derived, event.cast_id, 0,
               false, 0.0f, 0, source_upgrade,
               static_cast<std::uint8_t>(RelicKind::BleedBurnExplosion));
    EmitVfx(DomainSignalKind::BleedBurnExploded, enemy.position,
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

void GameSimulation::SimulationWorld::HandleDifferentSkillTracker(EnemyActor &enemy,
                                 const EffectCommand &event)
{
    if (event.skill <= SkillKind::BasicAttack ||
        event.skill >= SkillKind::Count ||
        event.origin != EffectOrigin::Original) return;
    const auto &relic = data.relics.different_skill_tracker;
    if (enemy.last_active_hit != SkillKind::Count &&
        enemy.last_active_hit != event.skill &&
        tick - enemy.last_active_hit_tick <= relic.window_ticks &&
        tick >= enemy.different_skill_ready)
    {
        EmitVfxLine(DomainSignalKind::RelicChainLinked,
                    player.position, enemy.position);
        if (auto *arrow = FireProjectile(
                event.skill, player.position,
                Normalize(Subtract(enemy.position, player.position)),
                relic.damage_multiplier, EffectOrigin::Derived,
                event.cast_id, 0, true, kNoTelemetrySource,
                static_cast<std::uint8_t>(RelicKind::DifferentSkillTracker)))
        {
            arrow->homing = true;
            arrow->homing_target = enemy.id.value;
            RecordRelicEffect(RelicKind::DifferentSkillTracker,
                              UpgradeEffectMetric::Activations);
            RecordRelicEffect(RelicKind::DifferentSkillTracker,
                              UpgradeEffectMetric::ProjectilesCreated);
        }
        enemy.different_skill_ready = tick + relic.per_target_cooldown_ticks;
    }
    enemy.last_active_hit = event.skill;
    enemy.last_active_hit_tick = tick;
}

void GameSimulation::SimulationWorld::HandleKillCooldownSurge()
{
    const auto &relic = data.relics.kill_cooldown_surge;
    ++player.kill_cooldown_progress;
    if (player.kill_cooldown_progress < relic.kills_per_trigger) return;
    player.kill_cooldown_progress -= relic.kills_per_trigger;
    std::uint64_t saved{};
    for (auto &cooldown : player.cooldowns)
    {
        const auto before = cooldown;
        cooldown -= std::min<Tick>(cooldown, relic.cooldown_reduction_ticks);
        saved += before - cooldown;
    }
    RecordRelicEffect(RelicKind::KillCooldownSurge,
                      UpgradeEffectMetric::Activations);
    RecordRelicEffect(RelicKind::KillCooldownSurge,
                      UpgradeEffectMetric::CooldownTicksSaved, saved);
}

void GameSimulation::SimulationWorld::HandleBasicKillTracker(EnemyActor &enemy, CastRuntime *runtime)
{
    if (!runtime || runtime->skill != SkillKind::BasicAttack ||
        enemy.last_damage_origin != EffectOrigin::Original) return;
    const auto &relic = data.relics.basic_kill_tracker;
    if (runtime->basic_relic_triggers >=
        relic.maximum_triggers_per_attack) return;
    EnemyActor *target{};
    auto best = relic.search_radius * relic.search_radius;
    for (auto &candidate : enemies)
    {
        if (candidate.dead || candidate.id.value == enemy.id.value ||
            std::ranges::any_of(cast_hits, [&](const CastHitRecord &hit) {
                return hit.cast_id == runtime->cast_id &&
                       hit.target == candidate.id.value;
            })) continue;
        const auto distance = DistanceSquared(enemy.position, candidate.position);
        if (distance < best)
        {
            target = &candidate;
            best = distance;
        }
    }
    if (!target) return;
    if (auto *arrow = FireProjectile(
            SkillKind::BasicAttack, enemy.position,
            Normalize(Subtract(target->position, enemy.position)),
            relic.damage_multiplier, EffectOrigin::Derived,
            runtime->cast_id, 0, true, kNoTelemetrySource,
            static_cast<std::uint8_t>(RelicKind::BasicKillTracker)))
    {
        arrow->homing = true;
        arrow->homing_target = target->id.value;
        ++runtime->basic_relic_triggers;
        RecordRelicEffect(RelicKind::BasicKillTracker,
                          UpgradeEffectMetric::Activations);
        RecordRelicEffect(RelicKind::BasicKillTracker,
                          UpgradeEffectMetric::ProjectilesCreated);
    }
}

void GameSimulation::SimulationWorld::HandleBleedKillHeal(const EnemyActor &enemy)
{
    if (enemy.status.bleed_count == 0 || tick < player.bleed_heal_ready) return;
    const auto &relic = data.relics.bleed_kill_heal;
    const auto healed = Heal(RoundDamage(
        player.max_health * relic.maximum_hp_heal_fraction));
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
    player.bleed_heal_ready = tick + relic.internal_cooldown_ticks;
}

void GameSimulation::SimulationWorld::HandleBurnPropagation(const EnemyActor &enemy)
{
    if (!enemy.status.burn || enemy.status.burn->propagated) return;
    const auto &relic = data.relics.burn_propagation;
    std::vector<std::uint64_t> excluded{enemy.id.value};
    excluded.reserve(static_cast<std::size_t>(relic.maximum_targets) + 1);
    for (std::uint32_t index = 0; index < relic.maximum_targets; ++index)
    {
        auto *target = NearestEnemy(enemy.position, relic.search_radius,
                                    std::span(excluded));
        if (!target) break;
        EmitVfxLine(DomainSignalKind::BurnTransferred,
                    enemy.position, target->position);
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
    const auto &relic = data.relics.once_revive;
    if (player.revives_used >= relic.maximum_triggers_per_session) return false;
    ++player.revives_used;
    player.health = std::max(
        1, RoundDamage(player.max_health * relic.health_fraction));
    player.revive_invulnerable_until = tick + relic.invulnerability_ticks;
    RecordRelicEffect(RelicKind::OnceRevive, UpgradeEffectMetric::Activations);
    RecordRelicEffect(RelicKind::OnceRevive, UpgradeEffectMetric::Healing,
                      static_cast<std::uint64_t>(player.health));
    EmitVfx(DomainSignalKind::PlayerHealed, player.position, {}, 1.5f, 0.5f);
    return true;
}

void GameSimulation::SimulationWorld::DispatchBasicAttackRules(Tick release_ticks, std::uint64_t cast_id)
{
    gameplay_detail::DispatchRules(active_rules, RuleHook::OnCast,
        [&](const ActiveRule &rule) {
            if (rule.handler == RuleHandlerId::RadialBasicAttack)
                HandleRadialBasicAttack(release_ticks, cast_id);
        });
    gameplay_detail::DispatchRules(active_rules, RuleHook::OnDistanceMoved,
        [&](const ActiveRule &rule) {
            if (rule.handler == RuleHandlerId::MovementEcho)
                HandleMovementEcho(release_ticks, cast_id);
        });
}

void GameSimulation::SimulationWorld::DispatchAbilityRules(SkillKind skill, std::size_t cooldown_index)
{
    gameplay_detail::DispatchRules(active_rules, RuleHook::OnAbilityUsed,
        [&](const ActiveRule &rule) {
            if (rule.handler == RuleHandlerId::AlternatingSkills)
                HandleAlternatingSkills(skill, cooldown_index);
        });
}

void GameSimulation::SimulationWorld::DispatchPlayerDamagedRules()
{
    gameplay_detail::DispatchRules(active_rules, RuleHook::OnPlayerDamaged,
        [&](const ActiveRule &rule) {
            if (rule.handler == RuleHandlerId::DamageKnockback)
                HandleDamageKnockback();
        });
}

void GameSimulation::SimulationWorld::DispatchAfterDamageRules(EnemyActor &enemy, const EffectCommand &event)
{
    gameplay_detail::DispatchRules(active_rules, RuleHook::AfterDamage,
        [&](const ActiveRule &rule) {
            switch (rule.handler)
            {
            case RuleHandlerId::DifferentSkillTracker:
                HandleDifferentSkillTracker(enemy, event);
                break;
            case RuleHandlerId::CombatHitChain:
                HandleCombatHitChain(event);
                break;
            default: break;
            }
        });
}

void GameSimulation::SimulationWorld::DispatchStatusAppliedRules(EnemyActor &enemy,
                                const EffectCommand &event,
                                std::uint8_t source_upgrade)
{
    gameplay_detail::DispatchRules(active_rules, RuleHook::OnStatusApplied,
        [&](const ActiveRule &rule) {
            if (rule.handler == RuleHandlerId::BleedBurnExplosion)
                HandleBleedBurnExplosion(enemy, event, source_upgrade);
        });
}

void GameSimulation::SimulationWorld::DispatchEnemyKilledRules(EnemyActor &enemy, CastRuntime *runtime)
{
    gameplay_detail::DispatchRules(active_rules, RuleHook::OnEnemyKilled,
        [&](const ActiveRule &rule) {
            switch (rule.handler)
            {
            case RuleHandlerId::BleedKillHeal: HandleBleedKillHeal(enemy); break;
            case RuleHandlerId::BurnPropagation: HandleBurnPropagation(enemy); break;
            case RuleHandlerId::KillCooldownSurge: HandleKillCooldownSurge(); break;
            case RuleHandlerId::BasicKillTracker:
                HandleBasicKillTracker(enemy, runtime);
                break;
            default: break;
            }
        });
}

bool GameSimulation::SimulationWorld::DispatchPlayerDeathRules()
{
    bool revived{};
    gameplay_detail::DispatchRules(active_rules, RuleHook::OnPlayerDeath,
        [&](const ActiveRule &rule) {
            if (rule.handler == RuleHandlerId::OnceRevive)
                revived |= HandleOnceRevive();
        });
    return revived;
}

} // namespace hs
