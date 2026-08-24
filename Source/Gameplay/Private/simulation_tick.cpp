#include "simulation_world.hpp"

namespace hs
{

using namespace gameplay_detail;

GameSimulation::SimulationWorld::SimulationWorld()
{
    enemies.reserve(1'024);
    projectiles.reserve(3'072);
    areas.reserve(256);
    pending_enemy_spawns.reserve(256);
    pending_projectile_spawns.reserve(512);
    pending_area_spawns.reserve(128);
    pending_boss_spawns.reserve(3);
    pickups.reserve(1'536);
    combat.Reserve(4'096);
    scheduled_actions.reserve(512);
    boss_actions.reserve(32);
    cast_hits.reserve(1'024);
    area_hits.reserve(1'024);
    cast_runtimes.reserve(512);
    balance_observer.enemy_hit_casts.reserve(256);
    balance_observer.player_hit_casts.reserve(256);
    domain_signals.reserve(512);
    collision_candidates.reserve(128);
}

void GameSimulation::SimulationWorld::InitializePlayerState()
{
    player = {};
    const auto &stats = rules.stats;
    player.max_health = RoundDamage(stats.base_maximum_hp);
    player.health = std::min(player.max_health, RoundDamage(stats.base_current_hp));
    player.attack = stats.base_attack_power;
    player.attack_speed = stats.base_basic_attack_rate_per_second;
    player.move_speed = stats.base_movement_speed_mps;
    player.magnet_radius = stats.base_magnet_radius_m;

    const auto &initial = rules.character_initial;
    player.level = initial.starting_level;
    player.pending_stat_points = initial.starting_unspent_stat_points;
    player.level_rerolls = rules.progression.level_initial_rerolls;
    player.relic_rerolls = rules.progression.relic_initial_rerolls;
    player.skill_levels[static_cast<std::size_t>(SkillKind::BasicAttack)] =
        initial.starting_basic_attack_level;

    const auto active_count = std::min<std::size_t>(
        {initial.starting_active_skill_count, rules.progression.active_slot_count,
         player.loadout.size()});
    for (std::size_t slot = 0; slot < active_count; ++slot)
    {
        const auto skill = initial.starting_active_skill_ids[slot];
        if (skill <= SkillKind::BasicAttack || skill >= SkillKind::Count) continue;
        player.loadout[slot] = skill;
        const auto skill_index = static_cast<std::size_t>(skill);
        player.skill_levels[skill_index] = rules.skills[skill_index].starting_level;
    }

    const auto relic_count = std::min<std::size_t>(
        initial.starting_relic_count, initial.starting_relic_ids.size());
    for (std::size_t index = 0; index < relic_count; ++index)
    {
        const auto relic = initial.starting_relic_ids[index];
        if (relic < RelicKind::Count)
            player.relic_mask |= static_cast<std::uint16_t>(1u << static_cast<unsigned>(relic));
    }
    relic_rules.Rebuild(player.relic_mask);
}

std::uint64_t GameSimulation::SimulationWorld::Random(std::uint64_t entity, std::uint64_t purpose) const noexcept
{
    return Mix(config.seed ^ Mix(tick) ^ Mix(entity) ^ Mix(purpose));
}

float GameSimulation::SimulationWorld::RandomUnit(std::uint64_t entity, std::uint64_t purpose) const noexcept
{
    return static_cast<float>((Random(entity, purpose) >> 40) & 0xFFFFFFu) /
           static_cast<float>(0x1000000u);
}

EntityId GameSimulation::SimulationWorld::AllocateEntityId() noexcept
{
    return {next_entity_id++};
}

float GameSimulation::SimulationWorld::EffectiveMoveSpeed() const noexcept
{
    const auto index = static_cast<std::size_t>(StatKind::MoveSpeed);
    return rules.stats.base_movement_speed_mps *
           (1.0f + rules.stats.allocations[index].amount_per_point * player.stats[index]);
}

bool GameSimulation::SimulationWorld::IsBoss(const EnemyActor &enemy) const noexcept
{
    return enemy.boss.has_value();
}

void GameSimulation::SimulationWorld::EmitSignal(DomainSignalKind kind, Float2 position,
                                                 std::uint8_t context)
{
    DomainSignal event;
    event.sequence = ++event_sequence;
    event.tick = tick;
    event.kind = kind;
    event.position = {position.x, 0.2f, position.y};
    event.context = context;
    domain_signals.push_back(event);
}

void GameSimulation::SimulationWorld::EmitVfx(DomainSignalKind effect, Float2 position,
             Float2 direction, float scale,
             float height, std::uint8_t context)
{
    const auto length = std::hypot(direction.x, direction.y);
    if (length <= 0.0001f) direction = {0.0f, 1.0f};
    else direction = {direction.x / length, direction.y / length};
    DomainSignal event;
    event.sequence = ++event_sequence;
    event.tick = tick;
    event.kind = effect;
    event.position = {position.x, height, position.y};
    event.direction = {direction.x, 0.0f, direction.y};
    event.scale = scale;
    event.context = context;
    domain_signals.push_back(event);
}

void GameSimulation::SimulationWorld::EmitVfxLine(DomainSignalKind effect, Float2 start, Float2 end,
                 float height)
{
    DomainSignal event;
    event.sequence = ++event_sequence;
    event.tick = tick;
    event.kind = effect;
    event.position = {start.x, height, start.y};
    event.target = {end.x, height, end.y};
    event.flags = static_cast<std::uint8_t>(DomainSignalFlag::HasTarget);
    domain_signals.push_back(event);
}

bool GameSimulation::SimulationWorld::SpawnEnemy(EnemyKind kind, Float2 position,
                std::uint64_t random_key)
{
    const auto completed_minutes = static_cast<std::uint32_t>(growth_ticks / Seconds(60.0f));
    const auto &definition = rules.enemies[static_cast<std::size_t>(kind)];
    const auto &scaling = rules.enemy_scaling;
    EnemyActor enemy;
    enemy.id = AllocateEntityId();
    enemy.random_key = random_key != 0 ? random_key : next_enemy_random_key++;
    enemy.kind = kind;
    enemy.position = enemy.previous_position = position;
    enemy.max_health = enemy.health = RoundDamage(
        static_cast<float>(definition.health) *
        (1.0f + scaling.hp_fraction_per_completed_minute * completed_minutes));
    enemy.damage = RoundDamage(
        static_cast<float>(definition.damage) *
        (1.0f + scaling.damage_fraction_per_completed_minute * completed_minutes));
    enemy.move_speed = definition.move_speed;
    enemy.attack_range = definition.attack_range;
    enemy.warning_extent = kind == EnemyKind::Ranged
                               ? definition.projectile_range
                               : kind == EnemyKind::Suicide ? definition.suicide_explosion_radius : 0.0f;
    if (kind == EnemyKind::Suicide)
        enemy.attack_range = definition.suicide_stop_distance;
    enemy.warning_ticks = definition.warning_ticks;
    enemy.attack_cooldown_ticks = definition.attack_cooldown_ticks;
    enemy.spawned_tick = tick;
    enemy.status.slows.reserve(8);
    if (pipeline_phase == SimulationPhaseId::Spawn)
        pending_enemy_spawns.push_back(std::move(enemy));
    else
        enemies.push_back(std::move(enemy));
    ++balance.enemy_spawned[static_cast<std::size_t>(kind)];
    return true;
}

bool GameSimulation::SimulationWorld::SpawnBoss(BossKind kind, Tick warning_ticks)
{
    const auto edge = rules.arena_half_extent - 1.0f;
    const auto position = player.position.x >= 0.0f ? Float2{-edge, -edge}
                                                    : Float2{edge, edge};
    pending_boss_spawns.push_back({kind, position, tick + warning_ticks});
    EmitSignal(DomainSignalKind::BossSpawnWarning, position,
               static_cast<std::uint8_t>(kind));
    return true;
}

void GameSimulation::SimulationWorld::CommitBossSpawns()
{
    for (const auto &pending : pending_boss_spawns)
    {
        if (pending.due > tick) continue;
        const auto kind = pending.kind;
        const auto &definition = rules.bosses[static_cast<std::size_t>(kind)];
        EnemyActor boss;
        boss.id = AllocateEntityId();
        boss.random_key = next_enemy_random_key++;
        boss.boss = kind;
        boss.position = boss.previous_position = pending.position;
        boss.max_health = boss.health = definition.health;
        boss.damage = 0;
        boss.move_speed = definition.movement_speed;
        boss.attack_range = definition.projectile_range;
        boss.spawned_tick = tick;
        boss.pattern_ready = tick + rules.boss_common.initial_pattern_delay_ticks;
        boss.status.slows.reserve(8);
        if (pipeline_phase == SimulationPhaseId::Spawn)
            pending_enemy_spawns.push_back(std::move(boss));
        else
            enemies.push_back(std::move(boss));
        ++balance.enemy_spawned[3u + static_cast<std::size_t>(kind)];
        EmitVfx(DomainSignalKind::BossSpawned, pending.position, {}, 1.0f, 0.3f,
                static_cast<std::uint8_t>(kind));
    }
    std::erase_if(pending_boss_spawns,
                  [this](const PendingBossSpawn &pending) { return pending.due <= tick; });
}

void GameSimulation::SimulationWorld::SpawnPickup(PickupKind kind, Float2 position, std::uint32_t value,
                 bool guaranteed)
{
    PickupActor pickup;
    pickup.id = AllocateEntityId();
    pickup.kind = kind;
    pickup.position = position;
    pickup.value = value;
    pickup.spawned_tick = tick;
    pickup.guaranteed_boss_chest = guaranteed;
    pickups.push_back(pickup);
    ++balance.pickup_drops[static_cast<std::size_t>(kind)];
}

void GameSimulation::SimulationWorld::StartSession()
{
    session_phase = SessionPhase::Playing;
    InitializePlayerState();
    growth_ticks = boss_fight_ticks = tick = 0;
    final_boss_spawned = false;
    spawn_accumulator = 0;
    kills = normal_chest_kills = heal_pickup_misses = magnet_pickup_misses = 0;
    level_reroll_sequence = 0;
    relic_reroll_sequence = 0;
    selection_input_guard_frames = 0;
    selection_waiting_for_release = false;
    next_enemy_attack_id = 1;
    next_enemy_random_key = 1;
    damage_dealt = damage_taken = healing = 0;
    damage_by_skill = {};
    balance_observer.Reset();
    enemies.clear();
    projectiles.clear();
    pending_enemy_spawns.clear();
    pending_projectile_spawns.clear();
    pending_area_spawns.clear();
    pending_boss_spawns.clear();
    areas.clear();
    pickups.clear();
    combat.Clear();
    scheduled_actions.clear();
    boss_actions.clear();
    cast_hits.clear();
    area_hits.clear();
    cast_runtimes.clear();
    domain_signals.clear();
    waves.clear();
    cards = {};
    card_count = 0;
}

void GameSimulation::SimulationWorld::ProcessInput()
{
    if (selection_input_guard_frames > 0) --selection_input_guard_frames;
    if (!current_input.held.basic_attack_held) selection_waiting_for_release = false;
    const auto aim_delta = Float2{current_input.held.aim_world.x - player.position.x,
                                  current_input.held.aim_world.z - player.position.y};
    if (LengthSquared(aim_delta) > 0.0001f)
    {
        player.aim = Normalize(aim_delta, player.aim);
    }
    if (current_input.held.move_held && session_phase == SessionPhase::Playing)
    {
        player.move_target = {current_input.held.move_target_world.x,
                              current_input.held.move_target_world.z};
        player.has_move_target = true;
    }
    for (const auto &edge : current_input.ordered_edges)
    {
        if (edge.action == GameAction::CharacterPage &&
            edge.kind == EdgeKind::Pressed)
        {
            if (session_phase == SessionPhase::Playing)
            {
                CancelChargedShot();
                session_phase = SessionPhase::Paused;
            }
            else if (session_phase == SessionPhase::Paused)
            {
                session_phase = SessionPhase::Playing;
            }
            continue;
        }
        if (edge.action == GameAction::Pause && edge.kind == EdgeKind::Pressed)
        {
            if (session_phase == SessionPhase::Playing)
            {
                CancelChargedShot();
                session_phase = SessionPhase::Paused;
            }
            else if (session_phase == SessionPhase::Paused)
            {
                session_phase = SessionPhase::Playing;
            }
            continue;
        }
        if (session_phase != SessionPhase::Playing)
        {
            continue;
        }
        if (edge.action == GameAction::BasicAttack && edge.kind == EdgeKind::Pressed)
        {
            player.has_move_target = false;
            continue;
        }
        const auto skill_slot = [](GameAction action) -> std::optional<std::size_t> {
            switch (action)
            {
            case GameAction::SkillQ: return 0;
            case GameAction::SkillW: return 1;
            case GameAction::SkillE: return 2;
            case GameAction::SkillR: return 3;
            default: return std::nullopt;
            }
        }(edge.action);
        if (!skill_slot)
        {
            continue;
        }
        const auto skill = player.loadout[*skill_slot];
        if (edge.kind == EdgeKind::Released)
        {
            if (player.buffered_skill == skill)
            {
                player.buffered_skill = SkillKind::Count;
                player.buffered_skill_expires = 0;
            }
            if (player.charging && player.charging_slot == *skill_slot)
            {
                ReleaseChargedShot();
            }
            continue;
        }
        if (skill == SkillKind::Count)
        {
            continue;
        }

        if (TryBeginSkill(skill))
        {
            if (skill == SkillKind::ChargedShot)
                player.charging_slot = static_cast<std::uint8_t>(*skill_slot);
        }
        else
        {
            player.buffered_skill = skill;
            player.buffered_skill_expires = tick + kInputBufferTicks;
        }
    }

    ConsumeBufferedSkill();
}

EnemyActor *GameSimulation::SimulationWorld::NearestEnemy(Float2 position, float radius,
                         std::span<const std::uint64_t> excluded) noexcept
{
    EnemyActor *best{};
    auto best_distance = radius * radius;
    for (auto &enemy : enemies)
    {
        if (enemy.dead || std::ranges::find(excluded, enemy.id.value) != excluded.end())
        {
            continue;
        }
        const auto distance = DistanceSquared(position, enemy.position);
        if (distance < best_distance ||
            (distance == best_distance && best && enemy.id.value < best->id.value))
        {
            best = &enemy;
            best_distance = distance;
        }
    }
    return best;
}

const EnemyActor *GameSimulation::SimulationWorld::NearestEnemy(Float2 position, float radius) const noexcept
{
    const EnemyActor *best{};
    auto best_distance = radius * radius;
    for (const auto &enemy : enemies)
    {
        if (enemy.dead)
        {
            continue;
        }
        const auto distance = DistanceSquared(position, enemy.position);
        if (distance < best_distance ||
            (distance == best_distance && best && enemy.id.value < best->id.value))
        {
            best = &enemy;
            best_distance = distance;
        }
    }
    return best;
}

void GameSimulation::SimulationWorld::RunPipeline()
{
    for (const auto phase_id : gameplay_detail::kSimulationPipeline)
    {
        pipeline_phase = phase_id;
        switch (phase_id)
        {
        case SimulationPhaseId::SessionTimer: SessionTimerPhase(); break;
        case SimulationPhaseId::Spawn: SpawnPhase(); break;
        case SimulationPhaseId::SpawnBarrier: CommitSpawnBarrier(); break;
        case SimulationPhaseId::AiIntent: AiIntentPhase(); break;
        case SimulationPhaseId::CastAttack: CastAttackPhase(); break;
        case SimulationPhaseId::AbilitySpawnBarrier:
            CommitAbilitySpawnBarrier();
            break;
        case SimulationPhaseId::Movement: MovementPhase(); break;
        case SimulationPhaseId::SpatialGrid: SpatialGridPhase(); break;
        case SimulationPhaseId::CollisionHit: CollisionHitPhase(); break;
        case SimulationPhaseId::DamageStatus: DamageStatusPhase(); break;
        case SimulationPhaseId::DeathDrop: DeathDropPhase(); break;
        case SimulationPhaseId::XpCard: XpCardPhase(); break;
        case SimulationPhaseId::CleanupBarrier: CommitCleanupBarrier(); break;
        case SimulationPhaseId::GameplayHash: checksum = CalculateChecksum(); break;
        }
    }
}

void GameSimulation::SimulationWorld::CommitSpawnBarrier()
{
    enemies.insert(enemies.end(),
                   std::make_move_iterator(pending_enemy_spawns.begin()),
                   std::make_move_iterator(pending_enemy_spawns.end()));
    pending_enemy_spawns.clear();
    assert(std::ranges::none_of(enemies, [](const EnemyActor &enemy) {
        return enemy.id.value == 0;
    }));
}

void GameSimulation::SimulationWorld::CommitAbilitySpawnBarrier()
{
    projectiles.insert(projectiles.end(),
                       std::make_move_iterator(pending_projectile_spawns.begin()),
                       std::make_move_iterator(pending_projectile_spawns.end()));
    pending_projectile_spawns.clear();
    areas.insert(areas.end(), std::make_move_iterator(pending_area_spawns.begin()),
                 std::make_move_iterator(pending_area_spawns.end()));
    pending_area_spawns.clear();
    assert(std::ranges::none_of(projectiles, [](const ProjectileActor &projectile) {
        return projectile.id.value == 0;
    }));
}

void GameSimulation::SimulationWorld::CommitCleanupBarrier()
{
    std::erase_if(enemies, [](const EnemyActor &enemy) { return enemy.dead; });
    std::erase_if(projectiles, [](const ProjectileActor &projectile) { return projectile.dead; });
    std::erase_if(areas, [](const AreaActor &area) { return area.dead; });
    std::erase_if(pickups, [](const PickupActor &pickup) { return pickup.dead; });
    std::erase_if(cast_hits, [this](const CastHitRecord &record) {
        return record.cast_id + 256 < next_cast_id;
    });
    std::erase_if(cast_runtimes, [this](const CastRuntime &runtime) {
        return runtime.cast_id + 256 < next_cast_id;
    });
    std::erase_if(area_hits, [this](const AreaHitRecord &record) {
        return std::ranges::none_of(areas, [&](const AreaActor &area) {
            return area.id.value == record.area_id;
        });
    });
}

GameplayChecksum GameSimulation::SimulationWorld::CalculateChecksum() const
{
    GameplayChecksum hash = 14695981039346656037ull;
    const auto value = [&]<typename T>(const T &item) {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
        HashBytes(hash, &item, sizeof(item));
    };
    const auto vector2 = [&](Float2 item) {
        value(item.x);
        value(item.y);
    };
    const auto count = [&](std::size_t size) {
        value(static_cast<std::uint64_t>(size));
    };

    value(config.seed);
    value(config.scenario.player_stationary);
    value(config.scenario.player_invulnerable);
    value(config.scenario.progression_enabled);
    value(config.scenario.auto_collect_progression);
    value(tick);
    value(growth_ticks);
    value(boss_fight_ticks);
    value(session_phase);
    value(selection_input_guard_frames);
    value(selection_waiting_for_release);
    value(next_entity_id);
    value(next_enemy_random_key);
    value(next_cast_id);
    value(next_enemy_attack_id);
    value(damage_sequence);
    value(spawn_accumulator);
    value(normal_chest_kills);
    value(heal_pickup_misses);
    value(magnet_pickup_misses);
    value(level_reroll_sequence);
    value(relic_reroll_sequence);
    value(final_boss_spawned);

    vector2(player.position);
    vector2(player.previous_position);
    vector2(player.aim);
    vector2(player.facing);
    vector2(player.move_target);
    value(player.has_move_target);
    value(player.health);
    value(player.max_health);
    value(player.attack);
    value(player.attack_speed);
    value(player.move_speed);
    value(player.magnet_radius);
    value(player.level);
    value(player.experience);
    value(player.pending_levels);
    value(player.pending_stat_points);
    value(player.level_rerolls);
    value(player.relic_rerolls);
    for (const auto item : player.stats) value(item);
    for (const auto item : player.loadout) value(item);
    for (const auto item : player.skill_levels) value(item);
    for (const auto item : player.upgrades) value(item);
    for (const auto item : player.cooldowns) value(item);
    value(player.relic_mask);
    value(player.next_basic_attack);
    value(player.basic_sequence);
    value(player.basic_arrow_sequence);
    value(player.active_basic_empower_until);
    value(player.movement_since_echo);
    vector2(player.one_second_ago);
    value(player.last_position_sample);
    value(player.last_active);
    value(player.last_active_tick);
    value(player.alternating_refund_until);
    value(player.alternating_refund_source);
    value(player.damage_relic_ready);
    value(player.bleed_heal_ready);
    value(player.revives_used);
    value(player.revive_invulnerable_until);
    value(player.kill_cooldown_progress);
    value(player.combat_hit_progress);
    value(player.charging);
    value(player.charging_skill);
    value(player.charging_slot);
    value(player.charge_start);
    value(player.retreat_until);
    vector2(player.retreat_velocity);
    value(player.active_cast_tick);
    value(player.active_animation_start);
    value(player.active_animation_until);
    value(player.retreat_followup_tick);
    vector2(player.retreat_followup_direction);
    value(player.retreat_followup_cast);
    value(player.retreat_upgrade_mask);
    value(player.retreat_landing_pending);
    value(player.forced_move_skill);
    value(player.next_active_refund_until);
    value(player.next_active_refund_source);
    value(player.buffered_skill);
    value(player.buffered_skill_expires);

    count(enemies.size());
    for (const auto &enemy : enemies)
    {
        value(enemy.id.value);
        value(enemy.random_key);
        value(enemy.kind);
        value(enemy.boss.has_value());
        if (enemy.boss) value(*enemy.boss);
        vector2(enemy.position);
        vector2(enemy.previous_position);
        vector2(enemy.velocity);
        vector2(enemy.displacement_per_tick);
        value(enemy.displacement_ticks);
        value(enemy.health);
        value(enemy.max_health);
        value(enemy.damage);
        value(enemy.move_speed);
        value(enemy.attack_range);
        value(enemy.warning_ticks);
        value(enemy.attack_cooldown_ticks);
        value(enemy.next_attack);
        value(enemy.attack_resolve);
        value(enemy.attack_cast_id);
        vector2(enemy.locked_aim);
        value(enemy.spawned_tick);
        value(enemy.pattern_ready);
        value(enemy.last_pattern);
        value(enemy.repeat_count);
        value(enemy.final_phase);
        value(enemy.invulnerable_until);
        value(enemy.dash_until);
        value(enemy.dash_damage);
        value(enemy.dash_hit);
        value(enemy.phase_pattern_count);
        value(enemy.status.bleed_count);
        for (std::size_t index = 0; index < enemy.status.bleed_count; ++index)
        {
            const auto &bleed = enemy.status.bleeds[index];
            value(bleed.attack_snapshot);
            value(bleed.expires);
            value(bleed.next_tick);
            value(bleed.source_skill);
            value(bleed.source_upgrade);
            value(bleed.source_relic);
        }
        value(enemy.status.burn.has_value());
        if (enemy.status.burn)
        {
            value(enemy.status.burn->attack_snapshot);
            value(enemy.status.burn->expires);
            value(enemy.status.burn->next_tick);
            value(enemy.status.burn->propagated);
            value(enemy.status.burn->source_skill);
            value(enemy.status.burn->source_upgrade);
            value(enemy.status.burn->source_relic);
        }
        count(enemy.status.slows.size());
        for (const auto &slow : enemy.status.slows)
        {
            value(slow.reduction);
            value(slow.expires);
            value(slow.source_skill);
            value(slow.source_upgrade);
            value(slow.source_relic);
        }
        value(enemy.last_damage_skill);
        value(enemy.last_damage_origin);
        value(enemy.last_damage_cast);
        value(enemy.last_damage_upgrade);
        value(enemy.marked_by_skill);
        value(enemy.marked_damage_coefficient);
        value(enemy.mark_expires);
        value(enemy.bleed_burn_ready);
        value(enemy.different_skill_ready);
        value(enemy.last_active_hit);
        value(enemy.last_active_hit_tick);
        value(enemy.attacking);
        value(enemy.dead);
    }


    count(projectiles.size());
    for (const auto &projectile : projectiles)
    {
        value(projectile.id.value);
        value(projectile.player_owned);
        vector2(projectile.position);
        vector2(projectile.previous_position);
        vector2(projectile.velocity);
        value(projectile.spawned_tick);
        value(projectile.remaining_range);
        value(projectile.radius);
        value(projectile.damage);
        value(projectile.skill);
        value(projectile.upgrade_mask);
        value(projectile.origin);
        value(projectile.cast_id);
        value(projectile.source_upgrade);
        value(projectile.source_relic);
        value(projectile.source_enemy);
        value(projectile.pierce_remaining);
        value(projectile.bounce_remaining);
        value(projectile.hit_count);
        for (std::size_t index = 0; index < projectile.hit_count; ++index)
            value(projectile.hit_ids[index]);
        value(projectile.explosion_radius);
        value(projectile.explosion_damage);
        value(projectile.explosion_source_upgrade);
        value(projectile.bleed_stacks);
        value(projectile.burn);
        value(projectile.slow);
        value(projectile.returning);
        value(projectile.homing);
        value(projectile.homing_target);
        value(projectile.full_charge);
        value(projectile.charge_ratio);
        value(projectile.carried_burn_attack);
        value(projectile.carried_burn_expires);
        value(projectile.dead);
    }

    count(areas.size());
    for (const auto &area : areas)
    {
        value(area.id.value);
        value(area.kind);
        vector2(area.position);
        vector2(area.direction);
        value(area.radius);
        value(area.half_length);
        value(area.effect_radius);
        value(area.damage);
        value(area.active_tick);
        value(area.next_tick);
        value(area.expires);
        value(area.interval);
        value(area.skill);
        value(area.upgrade_mask);
        value(area.origin);
        value(area.cast_id);
        value(area.source_upgrade);
        value(area.source_relic);
        value(area.source_enemy);
        value(area.slow_reduction);
        value(area.slow_duration);
        value(area.trigger_count);
        value(area.ring_inner_radius);
        value(area.ring_outer_radius);
        value(area.safe_gap_degrees);
        value(area.safe_gap_count);
        value(area.applies_burn);
        value(area.dead);
    }

    count(pickups.size());
    for (const auto &pickup : pickups)
    {
        value(pickup.id.value);
        value(pickup.kind);
        vector2(pickup.position);
        value(pickup.value);
        value(pickup.spawned_tick);
        value(pickup.guaranteed_boss_chest);
        value(pickup.attracted_by_magnet_stat);
        value(pickup.globally_attracted);
        value(pickup.dead);
    }

    count(combat.commands.size());
    for (const auto &event : combat.commands)
    {
        value(event.sequence);
        value(event.target);
        value(event.amount);
        value(event.skill);
        value(event.origin);
        value(event.cast_id);
        value(event.source_upgrade);
        value(event.source_relic);
        value(event.source_enemy);
        value(event.bleed_stacks);
        value(event.burn);
        value(event.slow_reduction);
        value(event.slow_duration);
        value(event.amplified_count);
        for (std::size_t index = 0; index < event.amplified_count; ++index)
        {
            value(event.amplified_upgrades[index]);
            value(event.amplified_damage[index]);
        }
    }
    count(scheduled_actions.size());
    for (const auto &action : scheduled_actions)
    {
        value(action.due);
        value(action.kind);
        value(action.skill);
        vector2(action.position);
        vector2(action.direction);
        value(action.damage_coefficient);
        value(action.radius);
        value(action.duration);
        value(action.projectile_count);
        value(action.upgrade_mask);
        value(action.origin);
        value(action.cast_id);
        value(action.source_upgrade);
        value(action.source_relic);
    }
    count(boss_actions.size());
    for (const auto &action : boss_actions)
    {
        value(action.due);
        value(action.kind);
        value(action.boss_id);
        vector2(action.position);
        vector2(action.direction);
        value(action.speed);
        value(action.distance);
        value(action.damage);
        value(action.projectile_count);
        value(action.arc_degrees);
        value(action.angle_offset);
        value(action.radius);
        value(action.duration);
        value(action.interval);
        value(action.safe_gap_count);
        value(action.safe_gap_degrees);
        value(action.cast_id);
    }
    count(cast_hits.size());
    for (const auto &record : cast_hits)
    {
        value(record.cast_id);
        value(record.target);
        value(record.count);
    }
    count(area_hits.size());
    for (const auto &record : area_hits)
    {
        value(record.area_id);
        value(record.target);
        value(record.count);
    }
    count(cast_runtimes.size());
    for (const auto &runtime : cast_runtimes)
    {
        value(runtime.cast_id);
        value(runtime.skill);
        value(runtime.upgrade_mask);
        value(runtime.normal_hits);
        value(runtime.kills);
        value(runtime.transfer_count);
        value(runtime.spawn_count);
        value(runtime.basic_relic_triggers);
        value(runtime.triggered);
        value(runtime.refund_triggered);
        value(runtime.full_charge);
        value(runtime.stored_burn_attack);
        value(runtime.stored_burn_remaining);
        value(runtime.has_stored_burn);
        value(runtime.terminal_target);
    }
    count(waves.size());
    for (const auto &wave : waves)
    {
        value(wave.start);
        value(wave.total);
        value(wave.emitted);
    }
    if (!pending_boss_spawns.empty())
    {
        count(pending_boss_spawns.size());
        for (const auto &pending : pending_boss_spawns)
        {
            value(pending.kind);
            vector2(pending.position);
            value(pending.due);
        }
    }
    value(card_count);
    for (std::size_t index = 0; index < card_count; ++index)
    {
        value(cards[index].kind);
        value(cards[index].subject);
        value(cards[index].upgrade);
    }
    return hash;
}

void GameSimulation::SimulationWorld::SessionTimerPhase()
{
    if (session_phase != SessionPhase::Playing)
    {
        return;
    }
    if (!final_boss_spawned || !rules.progression.stop_normal_spawns_at_final_boss)
    {
        ++growth_ticks;
    }
    else
    {
        ++boss_fight_ticks;
    }
    for (auto &cooldown : player.cooldowns)
    {
        cooldown -= cooldown != 0;
    }
    if (player.charging && (tick + kPlayerRenderId) % 6 == 0)
        EmitVfx(DomainSignalKind::ChargedShotPulse,
                Add(player.position, Multiply(player.aim, 0.65f)),
                player.aim, 1.0f, 1.1f);
    if (player.charging)
    {
        const auto &definition = rules.skills[
            static_cast<std::size_t>(SkillKind::ChargedShot)];
        const auto &upgrades = rules.upgrades.charged_shot;
        const auto mask = player.upgrades[
            static_cast<std::size_t>(SkillKind::ChargedShot)];
        const auto maximum_ticks = HasUpgrade(mask, 1)
                                       ? upgrades.extended_full_charge_explosion
                                             .maximum_charge_time_ticks
                                       : definition.maximum_charge_time_ticks;
        const auto ready_ticks = HasUpgrade(mask, 2)
                                     ? static_cast<Tick>(std::ceil(
                                           maximum_ticks * upgrades.faster_charge
                                                               .charge_time_multiplier))
                                     : maximum_ticks;
        if (tick - player.charge_start == ready_ticks)
            EmitVfx(DomainSignalKind::ChargedShotReady,
                    Add(player.position, Multiply(player.aim, 0.65f)),
                    player.aim, 1.0f, 1.1f);
    }
}

const SpawnStage &GameSimulation::SimulationWorld::CurrentSpawnStage() const noexcept
{
    const SpawnStage *selected = &rules.spawn_stages.front();
    for (const auto &stage : rules.spawn_stages)
    {
        if (stage.start_tick <= growth_ticks)
        {
            selected = &stage;
        }
    }
    return *selected;
}

Float2 GameSimulation::SimulationWorld::SpawnPosition(std::uint64_t salt) const noexcept
{
    const auto angle = RandomUnit(salt, 0x535041574Eull) * 2.0f * kPi;
    const auto &placement = rules.spawn_placement;
    const auto distance = placement.minimum_player_distance_m +
                          RandomUnit(salt, 0x44495354ull) *
                              (placement.maximum_player_distance_m -
                               placement.minimum_player_distance_m);
    auto position = Add(
        player.position,
        {std::cos(angle) * distance, std::sin(angle) * distance});
    const auto extent = rules.arena_half_extent - 1.0f;
    position.x = std::clamp(position.x, -extent, extent);
    position.y = std::clamp(position.y, -extent, extent);
    return position;
}

EnemyKind GameSimulation::SimulationWorld::ChooseEnemyKind(std::uint64_t salt) const noexcept
{
    const auto &stage = CurrentSpawnStage();
    const auto roll = Random(salt, 0x4B494E44ull) % 100;
    if (roll < stage.weights[0]) return EnemyKind::Melee;
    if (roll < stage.weights[0] + stage.weights[1]) return EnemyKind::Ranged;
    return EnemyKind::Suicide;
}

void GameSimulation::SimulationWorld::SpawnPhase()
{
    if (session_phase != SessionPhase::Playing)
    {
        return;
    }
    CommitBossSpawns();
    if (!final_boss_spawned)
    {
        const auto &stage = CurrentSpawnStage();
        spawn_accumulator += stage.per_second;
        while (spawn_accumulator >= 60.0f)
        {
            spawn_accumulator -= 60.0f;
            const auto salt = next_enemy_random_key++;
            SpawnEnemy(ChooseEnemyKind(salt), SpawnPosition(salt), salt);
        }
    }
    for (const auto &wave : rules.waves)
    {
        if (growth_ticks == wave.start_tick)
        {
            waves.push_back({growth_ticks, wave.duration_ticks, wave.count, 0});
        }
    }
    for (auto &wave : waves)
    {
        const auto elapsed = growth_ticks - wave.start;
        const auto desired = static_cast<std::uint16_t>(
            std::min<std::uint64_t>(wave.total,
                (static_cast<std::uint64_t>(elapsed + 1) * wave.total +
                 wave.duration - 1) / wave.duration));
        while (wave.emitted < desired &&
               (!final_boss_spawned || !rules.progression.stop_normal_spawns_at_final_boss))
        {
            const auto salt = next_enemy_random_key++;
            SpawnEnemy(ChooseEnemyKind(salt), SpawnPosition(salt), salt);
            ++wave.emitted;
        }
    }
    if (growth_ticks == rules.progression.boss_spawn_ticks[static_cast<std::size_t>(BossKind::FiveMinute)])
    {
        SpawnBoss(BossKind::FiveMinute, rules.boss_common.spawn_warning_ticks);
    }
    if (growth_ticks == rules.progression.boss_spawn_ticks[static_cast<std::size_t>(BossKind::TenMinute)])
    {
        SpawnBoss(BossKind::TenMinute, rules.boss_common.spawn_warning_ticks);
    }
    if (!final_boss_spawned &&
        growth_ticks == rules.progression.boss_spawn_ticks[static_cast<std::size_t>(BossKind::Final)])
    {
        final_boss_spawned = true;
        spawn_accumulator = 0;
        waves.clear();
        SpawnBoss(BossKind::Final, rules.boss_common.spawn_warning_ticks);
    }
}

float GameSimulation::SimulationWorld::SlowMultiplier(EnemyActor &enemy)
{
    std::erase_if(enemy.status.slows,
                  [this](const SlowEffect &slow) { return slow.expires <= tick; });
    float multiplier = 1.0f;
    for (const auto &slow : enemy.status.slows)
    {
        multiplier *= 1.0f - slow.reduction;
    }
    return multiplier;
}

void GameSimulation::SimulationWorld::BossAi(EnemyActor &boss)
{
    if (tick < boss.invulnerable_until)
    {
        boss.velocity = {};
        return;
    }
    if (tick < boss.dash_until) return;
    if (boss.dash_until != 0)
    {
        EmitVfx(DomainSignalKind::BossDashImpact, boss.position,
                Normalize(boss.velocity), 1.0f, 0.15f);
        boss.velocity = {};
        boss.dash_until = 0;
    }
    const auto kind = *boss.boss;
    const auto &definition = rules.bosses[static_cast<std::size_t>(kind)];
    const auto to_player = Subtract(player.position, boss.position);
    const auto distance = Length(to_player);
    const auto direction = Normalize(to_player);
    if (tick < boss.pattern_ready)
    {
        boss.velocity = {};
        return;
    }
    const auto phase = boss.final_phase;
    const auto preferred_near = phase == 2
                                    ? definition.phase_two_preferred_distance_near
                                    : definition.preferred_distance_near;
    const auto preferred_far = phase == 2
                                   ? definition.phase_two_preferred_distance_far
                                   : definition.preferred_distance_far;
    if (kind == BossKind::TenMinute && distance > preferred_far)
    {
        boss.velocity = Multiply(direction, boss.move_speed * SlowMultiplier(boss));
        return;
    }

    std::array<const BossPatternDefinition *, 3> phase_patterns{};
    std::size_t phase_pattern_count{};
    for (std::size_t index = 0;
         index < std::min<std::size_t>(definition.pattern_count, definition.patterns.size());
         ++index)
    {
        if (definition.patterns[index].phase == phase &&
            phase_pattern_count < phase_patterns.size())
            phase_patterns[phase_pattern_count++] = &definition.patterns[index];
    }
    if (phase_pattern_count == 0)
    {
        boss.velocity = {};
        return;
    }

    const auto preferred = kind == BossKind::TenMinute
                               ? static_cast<std::uint8_t>(distance <= preferred_near ? 1 : 0)
                               : static_cast<std::uint8_t>(
                                     distance <= preferred_near ? 2
                                     : distance <= preferred_far ? 1 : 0);
    const auto preferred_pattern = static_cast<std::uint8_t>(
        std::min<std::size_t>(preferred, phase_pattern_count - 1));
    const auto total_pattern_weight = rules.boss_common.preferred_pattern_weight +
                                      rules.boss_common.other_pattern_weight;
    const auto preferred_roll = total_pattern_weight != 0 &&
        Random(boss.random_key, 0x424F5353504154ull) % total_pattern_weight <
            rules.boss_common.preferred_pattern_weight;
    auto pattern = preferred_roll
                       ? preferred_pattern
                       : static_cast<std::uint8_t>((preferred_pattern + 1) % phase_pattern_count);
    if (pattern == boss.last_pattern &&
        boss.repeat_count >= rules.boss_common.maximum_same_pattern_repeats)
        pattern = static_cast<std::uint8_t>((pattern + 1) % phase_pattern_count);
    boss.repeat_count = pattern == boss.last_pattern ? boss.repeat_count + 1 : 1;
    boss.last_pattern = pattern;
    boss.velocity = {};

    const auto player_velocity = Multiply(
        Subtract(player.position, player.previous_position), 60.0f);
    const auto cast_id = next_cast_id++;
    ++balance.enemy_attack_attempts[EnemyTelemetryIndex(boss)];
    const auto add = [&](BossAction action) {
        boss_actions.push_back(action);
        const auto context = static_cast<std::uint8_t>(kind);
        const auto position = action.kind == BossActionKind::Dash ||
                                      action.kind == BossActionKind::Volley
                                  ? boss.position : action.position;
        const auto signal = action.kind == BossActionKind::Dash
                                ? DomainSignalKind::BossDashTelegraphed
                            : action.kind == BossActionKind::Volley
                                ? DomainSignalKind::BossVolleyTelegraphed
                            : action.kind == BossActionKind::Area
                                ? DomainSignalKind::BossAreaTelegraphed
                                : DomainSignalKind::BossShockwaveTelegraphed;
        EmitSignal(signal, position, context);
    };
    Tick last_due{};
    const auto &selected = *phase_patterns[pattern];
    const auto add_dash = [&](Tick due, Float2 dash_direction,
                              const BossPatternDefinition &source) {
        BossAction action{};
        action.due = due;
        action.kind = BossActionKind::Dash;
        action.boss_id = boss.id.value;
        action.direction = dash_direction;
        action.speed = source.speed;
        action.distance = source.distance;
        action.damage = source.damage;
        action.cast_id = cast_id;
        add(action);
    };
    const auto add_volley = [&](Tick due, Float2 volley_direction, float angle_offset,
                                const BossPatternDefinition &source) {
        BossAction action{};
        action.due = due;
        action.kind = BossActionKind::Volley;
        action.boss_id = boss.id.value;
        action.direction = volley_direction;
        action.speed = source.projectile_speed;
        action.damage = source.damage;
        action.projectile_count = source.projectile_count != 0
                                      ? source.projectile_count
                                      : source.projectiles_per_volley;
        action.arc_degrees = source.fan_angle_degrees;
        action.angle_offset = angle_offset;
        action.cast_id = cast_id;
        add(action);
    };
    const auto add_area = [&](Tick due, Float2 position,
                              const BossPatternDefinition &source) {
        BossAction action{};
        action.due = due;
        action.kind = BossActionKind::Area;
        action.boss_id = boss.id.value;
        action.position = position;
        action.damage = source.damage_per_tick != 0 ? source.damage_per_tick : source.damage;
        action.radius = source.radius;
        action.duration = static_cast<float>(source.duration_ticks) * kTickSeconds;
        action.interval = source.tick_interval_ticks;
        action.cast_id = cast_id;
        add(action);
    };
    const auto add_shockwave = [&](Tick due, const BossPatternDefinition &source) {
        BossAction action{};
        action.due = due;
        action.kind = BossActionKind::Shockwave;
        action.boss_id = boss.id.value;
        action.position = boss.position;
        action.distance = source.start_radius;
        action.radius = source.end_radius;
        action.damage = source.damage;
        action.duration = static_cast<float>(source.shockwave_duration_ticks) * kTickSeconds;
        action.interval = source.tick_interval_ticks;
        action.half_width = std::max(0.0f, source.shockwave_half_width);
        action.safe_gap_count = source.safe_gap_count;
        action.safe_gap_degrees = source.safe_gap_angle_degrees;
        action.cast_id = cast_id;
        add(action);
    };

    last_due = tick + selected.telegraph_duration_ticks;
    switch (selected.logic)
    {
    case BossPatternLogic::LineCharge:
        add_dash(last_due, direction, selected);
        break;
    case BossPatternLogic::ExpandingShockwaveWithSafeGaps:
        add_shockwave(last_due, selected);
        break;
    case BossPatternLogic::FanProjectiles:
        add_volley(last_due, direction, 0.0f, selected);
        break;
    case BossPatternLogic::PredictedPositionGroundAreas:
    {
        const auto center = Add(
            player.position,
            Multiply(player_velocity,
                     static_cast<float>(selected.prediction_lead_ticks) * kTickSeconds));
        const auto safe_extent = rules.arena_half_extent - selected.radius * 2.0f;
        const Float2 clamped_center{
            std::clamp(center.x, -safe_extent, safe_extent),
            std::clamp(center.y, -safe_extent, safe_extent)};
        const auto area_count = std::max<std::uint8_t>(1, selected.area_count);
        const auto angle_offset = RandomUnit(
            boss.random_key, cast_id ^ 0x47524F554E44ull) * 360.0f;
        for (std::uint32_t index = 0; index < area_count; ++index)
        {
            const auto position = Add(
                clamped_center,
                Multiply(Rotate({0.0f, 1.0f}, angle_offset +
                                 360.0f * index / static_cast<float>(area_count)),
                         selected.ground_placement_radius));
            add_area(last_due, position, selected);
        }
        break;
    }
    case BossPatternLogic::DoubleRetargetedCharge:
        for (std::uint32_t index = 0;
             index < std::max<std::uint8_t>(1, selected.charge_count); ++index)
        {
            if (index != 0)
            {
                // The authored interval starts after the preceding dash has
                // travelled its configured distance.  Keeping this delay in
                // the scheduling phase preserves the original sequential
                // double-charge pattern.
                const auto travel_ticks = selected.speed > 0.0f
                                               ? Seconds(selected.distance / selected.speed)
                                               : Tick{};
                last_due += travel_ticks + selected.interval_ticks;
            }
            add_dash(last_due, index == 0 ? direction : Float2{}, selected);
        }
        break;
    case BossPatternLogic::DoubleOffsetFanProjectiles:
        for (std::uint32_t index = 0;
             index < std::max<std::uint8_t>(1, selected.volley_count); ++index)
        {
            if (index != 0) last_due += selected.interval_ticks;
            add_volley(last_due, direction,
                       index == 0 ? 0.0f : selected.second_volley_angle_offset_degrees,
                       selected);
        }
        break;
    }
    if (selected.logic == BossPatternLogic::DoubleRetargetedCharge ||
        selected.logic == BossPatternLogic::DoubleOffsetFanProjectiles)
    {
        ++boss.phase_pattern_count;
    }

    const auto recovery = kind == BossKind::Final && boss.final_phase == 2
                              ? (boss.phase_pattern_count % 3 == 0
                                     ? definition.phase2_cycle_recovery_ticks
                                     : definition.phase2_pattern_interval_ticks)
                              : definition.recovery_ticks;
    boss.pattern_ready = last_due + recovery;
}

void GameSimulation::SimulationWorld::AiIntentPhase()
{
    for (auto &enemy : enemies)
    {
        if (enemy.dead)
        {
            continue;
        }
        if (enemy.boss)
        {
            BossAi(enemy);
            continue;
        }
        const auto direction = Normalize(Subtract(player.position, enemy.position));
        const auto distance = Length(Subtract(player.position, enemy.position));
        enemy.velocity = {};
        if (enemy.attacking)
        {
            if (tick >= enemy.attack_resolve)
            {
                const auto &definition = rules.enemies[static_cast<std::size_t>(enemy.kind)];
                if (enemy.kind == EnemyKind::Melee)
                {
                    if (distance <= definition.attack_range)
                    {
                        EmitVfx(DomainSignalKind::MeleeEnemyHit, player.position,
                                enemy.locked_aim, 1.0f, 0.1f);
                        QueueDamage(0, enemy.damage, SkillKind::Count,
                                   EffectOrigin::Original, enemy.attack_cast_id,
                                   0, false, 0.0f, 0, kNoTelemetrySource,
                                   kNoTelemetrySource,
                                   static_cast<std::uint8_t>(
                                       EnemyTelemetryIndex(enemy)));
                    }
                }
                else if (enemy.kind == EnemyKind::Ranged)
                {
                    auto *shot = FireProjectile(SkillKind::BasicAttack, enemy.position,
                                                enemy.locked_aim,
                                                 static_cast<float>(enemy.damage),
                                                 EffectOrigin::Original,
                                                 enemy.attack_cast_id, 0, false,
                                                 kNoTelemetrySource,
                                                 kNoTelemetrySource,
                                                 static_cast<std::uint8_t>(
                                                     EnemyTelemetryIndex(enemy)));
                    if (shot)
                    {
                        shot->velocity = Multiply(enemy.locked_aim,
                                                   definition.projectile_speed);
                        shot->remaining_range = definition.projectile_range;
                    }
                }
                else if (distance <= definition.suicide_explosion_radius)
                {
                    EmitVfx(DomainSignalKind::SuicideEnemyExploded,
                            enemy.position, {}, 1.0f, 0.15f);
                    QueueDamage(0, enemy.damage, SkillKind::Count,
                               EffectOrigin::Original, enemy.attack_cast_id,
                               0, false, 0.0f, 0, kNoTelemetrySource,
                               kNoTelemetrySource,
                               static_cast<std::uint8_t>(EnemyTelemetryIndex(enemy)));
                    enemy.dead = true;
                }
                enemy.attacking = false;
                enemy.next_attack = tick + enemy.attack_cooldown_ticks;
            }
            continue;
        }
        if (tick >= enemy.next_attack &&
            ((enemy.kind == EnemyKind::Melee && distance <= enemy.attack_range) ||
             (enemy.kind == EnemyKind::Ranged && distance <= enemy.attack_range) ||
             (enemy.kind == EnemyKind::Suicide && distance <= enemy.attack_range)))
        {
            ++balance.enemy_attack_attempts[EnemyTelemetryIndex(enemy)];
            enemy.attack_cast_id = next_enemy_attack_id++;
            enemy.attacking = true;
            enemy.attack_resolve = tick + enemy.warning_ticks;
            enemy.locked_aim = direction;
            if (enemy.kind == EnemyKind::Melee)
                EmitVfx(DomainSignalKind::MeleeEnemyWindup, enemy.position,
                        direction, 1.0f, 0.1f);
            else if (enemy.kind == EnemyKind::Suicide)
                EmitVfx(DomainSignalKind::SuicideEnemyCharging, enemy.position,
                        direction, 1.0f, 0.15f);
            continue;
        }
        if (enemy.kind == EnemyKind::Ranged)
        {
            if (distance > enemy.attack_range) enemy.velocity = direction;
        }
        else
        {
            enemy.velocity = direction;
        }
        enemy.velocity = Multiply(enemy.velocity, enemy.move_speed * SlowMultiplier(enemy));
    }
}

} // namespace hs
