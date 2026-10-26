#include "simulation_world.hpp"

#include <cassert>

namespace hs
{

using namespace gameplay_detail;

GameSimulation::SimulationWorld::SimulationWorld()
    : actors(std::make_unique<ActorState>()),
      combat_state(std::make_unique<CombatState>()),
      navigation(std::make_unique<NavigationState>()),
      progression(std::make_unique<ProgressionState>()),
      telemetry(std::make_unique<TelemetryState>())
{
    actors->enemies.reserve(1'024);
    actors->projectiles.reserve(3'072);
    actors->areas.reserve(256);
    actors->pending_enemy_spawns.reserve(256);
    actors->pending_projectile_spawns.reserve(512);
    actors->pending_area_spawns.reserve(128);
    progression->pending_boss_spawns.reserve(3);
    progression->pending_enemy_spawns_delayed.reserve(256);
    actors->pickups.reserve(1'536);
    progression->pickup_work.Reserve(1'536);
    combat_state->combat.Reserve(4'096);
    combat_state->scheduled_actions.reserve(512);
    combat_state->boss_actions.reserve(32);
    combat_state->cast_hits.reserve(1'024);
    combat_state->area_hits.reserve(1'024);
    combat_state->cast_hit_index.reserve(1'024);
    combat_state->area_hit_index.reserve(1'024);
    combat_state->cast_runtimes.reserve(512);
    combat_state->cast_runtime_index.reserve(512);
    telemetry->balance_observer.player_hit_casts.reserve(256);
    combat_state->domain_signals.reserve(512);
    combat_state->visual_links.reserve(64);
    combat_state->collision_query.rows.reserve(1'024);
    combat_state->collision_query.visited.reserve(1'024);
    combat_state->collision_hits.reserve(128);
    combat_state->live_area_ids.reserve(256);
}

void GameSimulation::SimulationWorld::InitializePlayerState()
{
    actors->player = {};
    actors->player_position_history.assign(
        static_cast<std::size_t>(rules.relics.movement_echo.position_history_age_ticks) + 1,
        actors->player.position);
    const auto &stats = rules.stats;
    actors->player.max_health = RoundDamage(stats.base_maximum_hp);
    actors->player.health = std::min(actors->player.max_health, RoundDamage(stats.base_current_hp));
    actors->player.attack = stats.base_attack_power;
    actors->player.attack_speed = stats.base_basic_attack_rate_per_second;
    actors->player.move_speed = stats.base_movement_speed_mps;
    actors->player.magnet_radius = stats.base_magnet_radius_m;

    const auto &initial = rules.character_initial;
    actors->player.level = initial.starting_level;
    actors->player.pending_stat_points = initial.starting_unspent_stat_points;
    actors->player.level_rerolls = rules.progression.level_initial_rerolls;
    actors->player.relic_rerolls = rules.progression.relic_initial_rerolls;
    actors->player.skill_levels[static_cast<std::size_t>(SkillKind::BasicAttack)] =
        initial.starting_basic_attack_level;

    const auto active_count = std::min<std::size_t>(
        {initial.starting_active_skill_count, rules.progression.active_slot_count,
         actors->player.loadout.size()});
    for (std::size_t slot = 0; slot < active_count; ++slot)
    {
        const auto skill = initial.starting_active_skill_ids[slot];
        if (skill <= SkillKind::BasicAttack || skill >= SkillKind::Count) continue;
        actors->player.loadout[slot] = skill;
        const auto skill_index = static_cast<std::size_t>(skill);
        actors->player.skill_levels[skill_index] = rules.skills[skill_index].starting_level;
    }

    const auto relic_count = std::min<std::size_t>(
        initial.starting_relic_count, initial.starting_relic_ids.size());
    for (std::size_t index = 0; index < relic_count; ++index)
    {
        const auto relic = initial.starting_relic_ids[index];
        if (relic < RelicKind::Count)
            actors->player.relic_mask |=
                RelicMask{1} << static_cast<unsigned>(relic);
    }
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

EntityId GameSimulation::SimulationWorld::AllocateEntityId()
{
    return {TakeSimulationIdentity(next_entity_id)};
}

float GameSimulation::SimulationWorld::EffectiveMoveSpeed() const noexcept
{
    const auto index = static_cast<std::size_t>(StatKind::MoveSpeed);
    return rules.stats.base_movement_speed_mps *
           (1.0f + rules.stats.allocations[index].amount_per_point * actors->player.stats[index]);
}

bool GameSimulation::SimulationWorld::IsBoss(ConstEnemyRow enemy) const noexcept
{
    return enemy.life.boss.has_value();
}

void GameSimulation::SimulationWorld::EmitSignal(DomainSignalKind kind, Float2 position,
                                                 std::uint8_t context)
{
    DomainSignal event;
    event.sequence = NextSimulationSequence(event_sequence);
    event.tick = tick;
    event.session_id = session_id;
    event.kind = kind;
    event.position = {position.x, 0.2f, position.y};
    event.context = context;
    combat_state->domain_signals.push_back(event);
}

void GameSimulation::SimulationWorld::EmitVfx(DomainSignalKind effect, Float2 position,
             Float2 direction, float scale,
             float height, std::uint8_t context, std::uint64_t source_entity_id,
             DomainSignalGeometry geometry)
{
    const auto length = std::hypot(direction.x, direction.y);
    if (length <= 0.0001f) direction = {0.0f, 1.0f};
    else direction = {direction.x / length, direction.y / length};
    DomainSignal event;
    event.sequence = NextSimulationSequence(event_sequence);
    event.tick = tick;
    event.session_id = session_id;
    event.kind = effect;
    event.position = {position.x, height, position.y};
    event.direction = {direction.x, 0.0f, direction.y};
    event.scale = scale;
    event.context = context;
    event.source_entity_id = source_entity_id;
    event.geometry = geometry;
    combat_state->domain_signals.push_back(event);
}

void GameSimulation::SimulationWorld::PruneVisualLinks()
{
    assert(observation.mutation_depth != 0);
    if (combat_state->visual_links_pruned_tick == tick) return;
    std::erase_if(combat_state->visual_links, [this](const VisualLinkView &link) { return link.expires <= tick; });
    combat_state->visual_links_pruned_tick = tick;
}

void GameSimulation::SimulationWorld::EmitVfxLine(DomainSignalKind effect, Float2 start, Float2 end,
                 float height)
{
    DomainSignal event;
    event.sequence = NextSimulationSequence(event_sequence);
    event.tick = tick;
    event.session_id = session_id;
    event.kind = effect;
    event.position = {start.x, height, start.y};
    event.target = {end.x, height, end.y};
    event.flags = static_cast<std::uint8_t>(DomainSignalFlag::HasTarget);
    combat_state->domain_signals.push_back(event);
    VisualLinkKind kind{};
    float width{};
    Tick duration{};
    // Preserve the existing authored cosmetic widths and round their lifetimes
    // (.12/.18/.15 seconds) upward to this simulation's 60 Hz tick contract.
    switch (effect)
    {
    case DomainSignalKind::RicochetLinked: kind = VisualLinkKind::Ricochet; width = .09f; duration = 8; break;
    case DomainSignalKind::BurnTransferred: kind = VisualLinkKind::BurnTransfer; width = .12f; duration = 11; break;
    case DomainSignalKind::RelicChainLinked: kind = VisualLinkKind::RelicChain; width = .10f; duration = 9; break;
    default: return;
    }
    PruneVisualLinks();
    combat_state->visual_links.push_back({event.sequence, kind, event.position, event.target,
                                        width, tick, tick + duration});
}

bool GameSimulation::SimulationWorld::SpawnEnemy(EnemyKind kind, Float2 position,
                std::uint64_t random_key)
{
    const auto completed_minutes = static_cast<std::uint32_t>(growth_ticks / Seconds(60.0f));
    const auto &definition = rules.enemies[static_cast<std::size_t>(kind)];
    const auto &scaling = rules.enemy_scaling;
    PreparedEnemy enemy;
    enemy.id = AllocateEntityId();
    enemy.cold.random_key = random_key != 0 ? random_key : TakeSimulationIdentity(next_enemy_random_key);
    enemy.life.kind = kind;
    enemy.motion.position = enemy.motion.previous_position = position;
    enemy.life.max_health = enemy.life.health = RoundDamage(
        static_cast<float>(definition.health) *
        (1.0f + scaling.hp_fraction_per_completed_minute * completed_minutes));
    enemy.cold.damage = RoundDamage(
        static_cast<float>(definition.damage) *
        (1.0f + scaling.damage_fraction_per_completed_minute * completed_minutes));
    enemy.cold.move_speed = definition.move_speed;
    enemy.cold.attack_range = definition.attack_range;
    enemy.action.warning_extent = kind == EnemyKind::Ranged
                               ? definition.projectile_range
                               : kind == EnemyKind::Suicide ? definition.suicide_explosion_radius : 0.0f;
    if (kind == EnemyKind::Suicide)
        enemy.cold.attack_range = definition.suicide_stop_distance;
    enemy.action.warning_ticks = definition.warning_ticks;
    enemy.cold.attack_cooldown_ticks = definition.attack_cooldown_ticks;
    enemy.life.spawned_tick = tick;
    enemy.status.slows.reserve(8);
    if (pipeline_phase == SimulationPhaseId::Spawn)
        actors->pending_enemy_spawns.push_back(std::move(enemy));
    else
        actors->enemies.push_back(std::move(enemy));
    ++Metrics().enemy_spawned[static_cast<std::size_t>(kind)];
    return true;
}

void GameSimulation::SimulationWorld::QueueEnemySpawn(EnemyKind kind, std::uint64_t random_key)
{
    bool used_fallback{};
    const auto position = SpawnPosition(random_key, used_fallback);
    if (!used_fallback)
    {
        SpawnEnemy(kind, position, random_key);
        return;
    }
    progression->pending_enemy_spawns_delayed.push_back({kind, position, random_key,
                                            tick + rules.spawn_placement.fallback_warning_ticks, PendingSimulationSequence(event_sequence), tick});
    DomainSignalGeometry geometry{};
    geometry.kind = DomainSignalGeometryKind::Circle;
    geometry.radius = rules.enemies[static_cast<std::size_t>(kind)].collision_radius;
    geometry.start_tick = tick;
    geometry.end_tick = progression->pending_enemy_spawns_delayed.back().due;
    geometry.source_id = progression->pending_enemy_spawns_delayed.back().warning_sequence;
    EmitVfx(DomainSignalKind::EnemySpawnWarning, position, {0.0f, 1.0f}, 1.0f, 0.025f,
            static_cast<std::uint8_t>(kind), 0, geometry);
}

bool GameSimulation::SimulationWorld::SpawnBoss(BossKind kind, Tick warning_ticks)
{
    const auto edge = rules.arena_half_extent - 1.0f;
    const auto position = ClampToArena(rules.arena_boundary,
                                       actors->player.position.x >= 0.0f ? Float2{-edge, -edge}
                                                                 : Float2{edge, edge}, 1.0f);
    progression->pending_boss_spawns.push_back({kind, position, tick + warning_ticks, PendingSimulationSequence(event_sequence), tick});
    DomainSignalGeometry geometry{};
    geometry.kind = DomainSignalGeometryKind::Circle;
    geometry.radius = rules.boss_common.collision_radius;
    geometry.start_tick = tick;
    geometry.end_tick = tick + warning_ticks;
    geometry.source_id = progression->pending_boss_spawns.back().warning_sequence;
    EmitVfx(DomainSignalKind::BossSpawnWarning, position, {0.0f, 1.0f}, 1.0f, 0.025f,
            static_cast<std::uint8_t>(kind), 0, geometry);
    return true;
}

void GameSimulation::SimulationWorld::CommitBossSpawns()
{
    for (const auto &pending : progression->pending_boss_spawns)
    {
        if (pending.due > tick) continue;
        const auto kind = pending.kind;
        const auto &definition = rules.bosses[static_cast<std::size_t>(kind)];
        PreparedEnemy boss;
        boss.id = AllocateEntityId();
        boss.cold.random_key = TakeSimulationIdentity(next_enemy_random_key);
        boss.life.boss = kind;
        boss.motion.position = boss.motion.previous_position = pending.position;
        boss.life.max_health = boss.life.health = definition.health;
        boss.cold.damage = 0;
        boss.cold.move_speed = definition.movement_speed;
        boss.cold.attack_range = definition.projectile_range;
        boss.life.spawned_tick = tick;
        boss.action.pattern_ready = tick + rules.boss_common.initial_pattern_delay_ticks;
        boss.status.slows.reserve(8);
        if (pipeline_phase == SimulationPhaseId::Spawn)
            actors->pending_enemy_spawns.push_back(std::move(boss));
        else
            actors->enemies.push_back(std::move(boss));
        ++Metrics().enemy_spawned[3u + static_cast<std::size_t>(kind)];
        EmitVfx(DomainSignalKind::BossSpawned, pending.position, {}, 1.0f, 0.3f,
                static_cast<std::uint8_t>(kind));
    }
    std::erase_if(progression->pending_boss_spawns,
                  [this](const PendingBossSpawn &pending) { return pending.due <= tick; });
}

void GameSimulation::SimulationWorld::CommitDelayedEnemySpawns()
{
    for (const auto &pending : progression->pending_enemy_spawns_delayed)
    {
        if (pending.due <= tick)
            SpawnEnemy(pending.kind, pending.position, pending.random_key);
    }
    std::erase_if(progression->pending_enemy_spawns_delayed,
                  [this](const PendingEnemySpawn &pending) { return pending.due <= tick; });
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
    assert(actors->pickups.empty() || actors->pickups.back().id.value < pickup.id.value);
    actors->pickups.push_back(pickup);
    progression->pickup_work.OnSpawn(pickup, rules.arena_half_extent);
    ++Metrics().pickup_drops[static_cast<std::size_t>(kind)];
    if (kind != PickupKind::Experience)
        EmitVfx(DomainSignalKind::PickupSpawned, pickup.position, {}, 1.0f,
                0.2f, static_cast<std::uint8_t>(kind), pickup.id.value);
}

void GameSimulation::SimulationWorld::StartSession()
{
    NextSimulationSequence(session_id);
    session_phase = SessionPhase::Playing;
    InitializePlayerState();
    growth_ticks = boss_fight_ticks = tick = 0;
    final_boss_spawned = false;
    spawn_accumulator = 0;
    telemetry->kills = progression->normal_chest_kills = progression->heal_pickup_misses = progression->magnet_pickup_misses = 0;
    progression->level_reroll_sequence = 0;
    progression->relic_reroll_sequence = 0;
    selection_input_guard_frames = 0;
    selection_waiting_for_release = false;
    next_enemy_attack_id = 1;
    next_enemy_random_key = 1;
    telemetry->damage_dealt = telemetry->damage_taken = telemetry->healing = 0;
    telemetry->damage_by_skill = {};
    telemetry->balance_observer.Reset(config.capture_detailed_balance);
    actors->enemies.clear();
    actors->projectiles.clear();
    actors->pending_enemy_spawns.clear();
    actors->pending_projectile_spawns.clear();
    actors->pending_area_spawns.clear();
    progression->pending_boss_spawns.clear();
    progression->pending_enemy_spawns_delayed.clear();
    actors->areas.clear();
    actors->pickups.clear();
    progression->pickup_work.Reset();
    combat_state->combat.Clear();
    combat_state->scheduled_actions.clear();
    combat_state->next_scheduled_sequence=0;
    combat_state->scheduled_actions_dirty=false;
    combat_state->boss_actions.clear();
    combat_state->next_boss_schedule_sequence=0;
    combat_state->boss_actions_dirty=false;
    combat_state->cast_hits.clear();
    combat_state->area_hits.clear();
    combat_state->cast_hit_index.clear();
    combat_state->area_hit_index.clear();
    combat_state->cast_runtimes.clear();
    combat_state->cast_runtime_index.clear();
    combat_state->domain_signals.clear();
    combat_state->visual_links.clear();
    combat_state->visual_links_pruned_tick.reset();
    combat_state->boss_death_visual_count = 0;
    progression->waves.clear();
    progression->cards = {};
    progression->card_count = 0;
}

void GameSimulation::SimulationWorld::ProcessInput()
{
    if (selection_input_guard_frames > 0) --selection_input_guard_frames;
    if (!current_input.held.basic_attack_held) selection_waiting_for_release = false;
    const auto aim_delta = Float2{current_input.held.aim_world.x - actors->player.position.x,
                                  current_input.held.aim_world.z - actors->player.position.y};
    if (LengthSquared(aim_delta) > 0.0001f)
    {
        actors->player.aim = Normalize(aim_delta, actors->player.aim);
    }
    if (current_input.held.move_held && session_phase == SessionPhase::Playing)
    {
        actors->player.move_target = ClampToArena(
            rules.arena_boundary,
            ProjectOutsideObstacles({current_input.held.move_target_world.x,
                                     current_input.held.move_target_world.z},
                                    rules.stats.player_collision_radius,
                                    ArenaObstacles()),
            rules.stats.player_collision_radius);
        actors->player.has_move_target = true;
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
            actors->player.has_move_target = false;
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
        const auto skill = actors->player.loadout[*skill_slot];
        if (edge.kind == EdgeKind::Released)
        {
            if (actors->player.buffered_skill == SkillKind::ChargedShot &&
                actors->player.buffered_skill_slot == *skill_slot)
            {
                actors->player.buffered_skill = SkillKind::Count;
                actors->player.buffered_skill_slot = 0xFF;
                actors->player.buffered_skill_expires = 0;
            }
            if (actors->player.charging && actors->player.charging_slot == *skill_slot)
            {
                ReleaseChargedShot();
            }
            continue;
        }
        if (skill == SkillKind::Count)
        {
            continue;
        }

        // A new press replaces the previous buffered intent, even when it succeeds now.
        actors->player.buffered_skill = SkillKind::Count;
        actors->player.buffered_skill_slot = 0xFF;
        actors->player.buffered_skill_expires = 0;
        if (TryBeginSkill(skill))
        {
            if (skill == SkillKind::ChargedShot)
                actors->player.charging_slot = static_cast<std::uint8_t>(*skill_slot);
        }
        else
        {
            actors->player.buffered_skill = skill;
            actors->player.buffered_skill_slot = static_cast<std::uint8_t>(*skill_slot);
            actors->player.buffered_skill_expires = tick + kInputBufferTicks;
        }
    }

    ConsumeBufferedSkill();
}

void GameSimulation::SimulationWorld::CollectEnemies(Float2 position, float radius,
                                                       EnemyQuery &query) const
{
    if(!std::isfinite(position.x)||!std::isfinite(position.y)||!std::isfinite(radius))
    { CollectEnemies({}, {}, query); return; }
    const auto extent=std::abs(radius),low=-rules.arena_half_extent;
    const auto high=low+kGridDimension*kGridCellSize;
    CollectEnemies({std::clamp(position.x-extent,low,high),std::clamp(position.y-extent,low,high)},
        {std::clamp(position.x+extent,low,high),std::clamp(position.y+extent,low,high)},query);
}

void GameSimulation::SimulationWorld::CollectEnemies(Float2 minimum,Float2 maximum,EnemyQuery &query,bool canonical_order) const
{
    // Only the current post-movement grid is safe to use; every other case scans canonical rows.
    if(pipeline_phase>=SimulationPhaseId::CollisionHit&&pipeline_phase<=SimulationPhaseId::DeathDrop&&
        std::isfinite(minimum.x)&&std::isfinite(minimum.y)&&std::isfinite(maximum.x)&&std::isfinite(maximum.y)&&
        combat_state->enemy_grid.IsCurrent(tick,actors->enemies.StructuralGeneration()))
    {
        query.Collect(combat_state->enemy_grid,minimum,maximum,rules.arena_half_extent,actors->enemies.size(),canonical_order);
        return;
    }
    for (const auto row : query.rows) query.visited[row] = 0;
    query.visited.resize(actors->enemies.size(), 0);
    query.rows.resize(actors->enemies.size());
    for (std::size_t row = 0; row < query.rows.size(); ++row) query.rows[row] = row;
}

std::optional<EnemyRow> GameSimulation::SimulationWorld::NearestEnemy(Float2 position, float radius,
                         std::span<const std::uint64_t> excluded)
{
    auto timing = Measure(SimulationTimingId::EnemyQueries);
    std::optional<EnemyRow> best{};
    auto best_distance = radius * radius;
    CollectEnemies(position, radius, combat_state->nearest_query);
    for (const auto row : combat_state->nearest_query.rows)
    {
        auto enemy = actors->enemies[row];
        if (enemy.life.dead || std::ranges::find(excluded, enemy.id.value) != excluded.end())
        {
            continue;
        }
        const auto distance = DistanceSquared(position, enemy.motion.position);
        if (distance < best_distance ||
            (distance == best_distance && best && enemy.id.value < best->id.value))
        {
            best.emplace(enemy);
            best_distance = distance;
        }
    }
    return best;
}

std::optional<ConstEnemyRow> GameSimulation::SimulationWorld::NearestEnemy(Float2 position, float radius) const
{
    auto timing = Measure(SimulationTimingId::EnemyQueries);
    std::optional<ConstEnemyRow> best{};
    auto best_distance = radius * radius;
    CollectEnemies(position, radius, combat_state->nearest_query);
    for (const auto row : combat_state->nearest_query.rows)
    {
        const auto enemy = actors->enemies[row];
        if (enemy.life.dead)
        {
            continue;
        }
        const auto distance = DistanceSquared(position, enemy.motion.position);
        if (distance < best_distance ||
            (distance == best_distance && best && enemy.id.value < best->id.value))
        {
            best.emplace(enemy);
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
        static constexpr std::array timing_ids{
            SimulationTimingId::SessionTimer, SimulationTimingId::Spawn, SimulationTimingId::SpawnBarrier,
            SimulationTimingId::AiIntent, SimulationTimingId::CastAttack, SimulationTimingId::AbilitySpawnBarrier,
            SimulationTimingId::Movement, SimulationTimingId::SpatialGrid, SimulationTimingId::CollisionHit,
            SimulationTimingId::DamageStatus, SimulationTimingId::DeathDrop, SimulationTimingId::XpCard,
            SimulationTimingId::CleanupBarrier, SimulationTimingId::GameplayHash};
        static_assert(timing_ids.size() == kSimulationPipeline.size());
        auto phase_timing = Measure(timing_ids[static_cast<std::size_t>(phase_id)]);
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
        case SimulationPhaseId::GameplayHash: UpdateChecksum(); break;
        }
    }
}

void GameSimulation::SimulationWorld::CommitSpawnBarrier()
{
    actors->enemies.AppendFrom(actors->pending_enemy_spawns);
    assert(std::ranges::none_of(actors->enemies, [](ConstEnemyRow enemy) {
        return enemy.id.value == 0;
    }));
}

void GameSimulation::SimulationWorld::CommitAbilitySpawnBarrier()
{
    actors->projectiles.AppendFrom(actors->pending_projectile_spawns);
    actors->areas.insert(actors->areas.end(), std::make_move_iterator(actors->pending_area_spawns.begin()),
                 std::make_move_iterator(actors->pending_area_spawns.end()));
    actors->pending_area_spawns.clear();
    assert(std::ranges::none_of(actors->projectiles, [](ConstProjectileRow projectile) {
        return projectile.id.value == 0;
    }));
}

void GameSimulation::SimulationWorld::CommitCleanupBarrier()
{
    actors->enemies.EraseIf([](ConstEnemyRow enemy) { return enemy.life.dead; });
    actors->projectiles.EraseIf([](ConstProjectileRow projectile) { return projectile.combat.dead; });
    actors->projectiles.AppendFrom(actors->pending_projectile_spawns);
    std::erase_if(actors->areas, [](const AreaActor &area) { return area.dead; });
    if (progression->pickup_work.NeedsCompaction())
    {
        std::erase_if(actors->pickups, [](const PickupActor &pickup) { return pickup.dead; });
        progression->pickup_work.AfterCompaction();
    }
    if (std::erase_if(combat_state->cast_hits, [this](const CastHitRecord &record) {
        return IsOldCast(record.cast_id, next_cast_id);
    }))
    {
        combat_state->cast_hit_index.clear();
        for (std::size_t i = 0; i < combat_state->cast_hits.size(); ++i)
        {
            const auto &record = combat_state->cast_hits[i];
            combat_state->cast_hit_index.emplace(std::pair{record.cast_id, record.target}, i);
        }
    }
    auto &live_casts = combat_state->cast_cleanup;
    live_casts.Prepare(combat_state->cast_runtimes, next_cast_id);
    if (!live_casts.entries.empty())
        for (const auto &projectile : actors->projectiles) live_casts.Observe(projectile.combat.cast_id);
    const auto removed_casts = std::erase_if(
        combat_state->cast_runtimes, [this, &live_casts](const CastRuntime &runtime) {
            if (!IsOldCast(runtime.cast_id, next_cast_id)) return false;
            if (runtime.cooldown_ticks_saved == 0) return true;
            return !live_casts.Contains(runtime.cast_id);
        });
    if (removed_casts != 0)
    {
        combat_state->cast_runtime_index.clear();
        for (std::size_t i = 0; i < combat_state->cast_runtimes.size(); ++i)
            combat_state->cast_runtime_index.emplace(combat_state->cast_runtimes[i].cast_id, i);
        assert(combat_state->cast_runtime_index.size() == combat_state->cast_runtimes.size());
    }
    auto &live_areas = combat_state->live_area_ids;
    live_areas.clear();
    if (!combat_state->area_hits.empty())
    {
        for (const auto &area : actors->areas) live_areas.push_back(area.id.value);
        std::ranges::sort(live_areas);
    }
    if (std::erase_if(combat_state->area_hits, [&](const AreaHitRecord &record) {
        return !std::ranges::binary_search(live_areas, record.area_id);
    }))
    {
        combat_state->area_hit_index.clear();
        for (std::size_t i = 0; i < combat_state->area_hits.size(); ++i)
        {
            const auto &record = combat_state->area_hits[i];
            combat_state->area_hit_index.emplace(std::pair{record.area_id, record.target}, i);
        }
    }
    RetireBalanceHitCauses();
}

void GameSimulation::SimulationWorld::RetireBalanceHitCauses()
{
    auto &observer = telemetry->balance_observer;
    observer.active_player_casts.clear();
    for (auto &casts : observer.active_enemy_casts) casts.clear();

    const auto add_player = [&](std::uint64_t cast) {
        BalanceObserver::AddCause(observer.active_player_casts, cast);
    };
    const auto add_enemy = [&](std::uint8_t source, std::uint64_t cast) {
        if (source < kEnemyArchetypeCount)
            BalanceObserver::AddCause(observer.active_enemy_casts[source],
                                      cast & 0x00FFFFFFFFFFFFFFull);
    };
    const auto collect_projectile = [&](ConstProjectileRow projectile) {
        if (projectile.combat.player_owned)
            add_player(projectile.combat.cast_id);
        else
            add_enemy(projectile.combat.source_enemy, projectile.combat.cast_id);
    };
    const auto collect_area = [&](const AreaActor &area) {
        if (area.source_enemy < kEnemyArchetypeCount)
            add_enemy(area.source_enemy, area.cast_id);
        else
            add_player(area.cast_id);
    };

    for (const auto &projectile : actors->projectiles) collect_projectile(projectile);
    for (const auto &projectile : actors->pending_projectile_spawns)
        collect_projectile(projectile);
    for (const auto &area : actors->areas) collect_area(area);
    for (const auto &area : actors->pending_area_spawns) collect_area(area);

    // Scheduled actions are player skill causes; boss actions are enemy causes.
    for (const auto &action : combat_state->scheduled_actions) add_player(action.cast_id);
    for (const auto &action : combat_state->boss_actions)
    {
        if (const auto boss = FindEnemy(action.boss_id))
            add_enemy(static_cast<std::uint8_t>(EnemyTelemetryIndex(*boss)),
                      action.cast_id);
    }

    const auto collect_enemy = [&](ConstEnemyRow enemy) {
        add_enemy(static_cast<std::uint8_t>(EnemyTelemetryIndex(enemy)),
                  enemy.action.attack_cast_id);
        // This cause can still be consulted by a later death/proc path.
        add_player(enemy.cold.last_damage_cast);
    };
    for (const auto &enemy : actors->enemies) collect_enemy(enemy);
    for (const auto &enemy : actors->pending_enemy_spawns) collect_enemy(enemy);

    add_player(actors->player.basic_attack_cast_id);
    add_player(actors->player.charge_cast_id);
    for (const auto &command : combat_state->combat.commands)
    {
        if (command.target == 0)
            add_enemy(command.source_enemy, command.cast_id);
        else if (command.skill < SkillKind::Count)
            add_player(command.cast_id);
    }
    observer.RetireInactiveCauses();
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
    if (!config.scenario.automatic_spawning)
    {
        // Optional development-policy extension preserves all existing default replay oracles.
        value(std::uint32_t{0x53504157});
        value(config.scenario.automatic_spawning);
    }
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
    value(progression->normal_chest_kills);
    value(progression->heal_pickup_misses);
    value(progression->magnet_pickup_misses);
    value(progression->level_reroll_sequence);
    value(progression->relic_reroll_sequence);
    value(final_boss_spawned);

    vector2(actors->player.position);
    vector2(actors->player.previous_position);
    vector2(actors->player.aim);
    vector2(actors->player.facing);
    vector2(actors->player.move_target);
    value(actors->player.has_move_target);
    value(actors->player.health);
    value(actors->player.max_health);
    value(actors->player.attack);
    value(actors->player.attack_speed);
    value(actors->player.move_speed);
    value(actors->player.magnet_radius);
    value(actors->player.level);
    value(actors->player.experience);
    value(actors->player.pending_levels);
    value(actors->player.pending_stat_points);
    value(actors->player.level_rerolls);
    value(actors->player.relic_rerolls);
    for (const auto item : actors->player.stats) value(item);
    for (const auto item : actors->player.loadout) value(item);
    for (const auto item : actors->player.skill_levels) value(item);
    for (const auto item : actors->player.upgrades) value(item);
    for (const auto item : actors->player.cooldowns) value(item);
    value(actors->player.relic_mask);
    value(actors->player.next_basic_attack);
    value(actors->player.basic_attack_cast_id);
    value(actors->player.basic_attack_release_tick);
    value(actors->player.basic_sequence);
    value(actors->player.basic_arrow_sequence);
    value(actors->player.active_basic_empower_until);
    value(actors->player.movement_since_echo);
    // History can affect a later relic acquisition, so hash it even without the relic.
    count(actors->player_position_history.size());
    for (const auto position : actors->player_position_history) vector2(position);
    value(actors->player.last_active);
    value(actors->player.last_active_tick);
    value(actors->player.alternating_refund_until);
    value(actors->player.alternating_refund_source);
    value(actors->player.damage_relic_ready);
    value(actors->player.bleed_heal_ready);
    value(actors->player.revives_used);
    value(actors->player.revive_invulnerable_until);
    value(actors->player.kill_cooldown_progress);
    value(actors->player.combat_hit_progress);
    value(actors->player.projectile_cadence_progress);
    value(actors->player.hit_streak_progress);
    value(actors->player.pre_damage_guard_ready);
    value(actors->player.area_resonance_ready);
    value(actors->player.pickup_reward_until);
    value(actors->player.low_health_survival_ready);
    value(actors->player.charging);
    value(actors->player.charging_skill);
    value(actors->player.charging_slot);
    value(actors->player.charge_start);
    // Reserve checksum changes for an active charge that actually owns a cast ID.
    if (actors->player.charge_cast_id != 0)
        value(actors->player.charge_cast_id);
    value(actors->player.retreat_until);
    vector2(actors->player.retreat_velocity);
    value(actors->player.active_cast_tick);
    value(actors->player.active_animation_start);
    value(actors->player.active_animation_until);
    value(actors->player.retreat_followup_tick);
    vector2(actors->player.retreat_followup_direction);
    value(actors->player.retreat_followup_cast);
    value(actors->player.retreat_upgrade_mask);
    value(actors->player.retreat_landing_pending);
    value(actors->player.forced_move_skill);
    value(actors->player.next_active_refund_until);
    value(actors->player.next_active_refund_source);
    value(actors->player.buffered_skill);
    value(actors->player.buffered_skill_slot);
    value(actors->player.buffered_skill_expires);

    count(actors->enemies.size());
    for (const auto &enemy : actors->enemies)
    {
        value(enemy.id.value);
        value(enemy.cold.random_key);
        value(enemy.life.kind);
        value(enemy.life.boss.has_value());
        if (enemy.life.boss) value(*enemy.life.boss);
        vector2(enemy.motion.position);
        vector2(enemy.motion.previous_position);
        vector2(enemy.motion.velocity);
        vector2(enemy.motion.displacement_per_tick);
        value(enemy.motion.displacement_ticks);
        value(enemy.life.health);
        value(enemy.life.max_health);
        value(enemy.cold.damage);
        value(enemy.cold.move_speed);
        value(enemy.cold.attack_range);
        value(enemy.action.warning_ticks);
        value(enemy.cold.attack_cooldown_ticks);
        value(enemy.action.next_attack);
        value(enemy.action.attack_resolve);
        value(enemy.action.attack_cast_id);
        vector2(enemy.action.locked_aim);
        value(enemy.life.spawned_tick);
        value(enemy.action.pattern_ready);
        value(enemy.action.last_pattern);
        value(enemy.action.repeat_count);
        value(enemy.action.final_phase);
        value(enemy.action.invulnerable_until);
        value(enemy.action.dash_until);
        value(enemy.action.dash_damage);
        value(enemy.action.dash_hit);
        value(enemy.action.phase_pattern_count);
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
        value(enemy.cold.last_damage_skill);
        value(enemy.cold.last_damage_origin);
        value(enemy.cold.last_damage_cast);
        value(enemy.cold.last_damage_upgrade);
        value(enemy.cold.last_damage_relic);
        value(enemy.cold.marked_by_skill);
        value(enemy.cold.marked_damage_coefficient);
        value(enemy.cold.mark_expires);
        value(enemy.cold.bleed_burn_ready);
        value(enemy.cold.different_skill_ready);
        value(enemy.cold.slow_synergy_ready);
        value(enemy.cold.boss_pressure_ready);
        value(enemy.cold.last_active_hit);
        value(enemy.cold.last_active_hit_tick);
        value(enemy.action.attacking);
        value(enemy.life.dead);
    }


    count(actors->projectiles.size());
    for (const auto &projectile : actors->projectiles)
    {
        value(projectile.id.value);
        value(projectile.combat.player_owned);
        vector2(projectile.motion.position);
        vector2(projectile.motion.previous_position);
        vector2(projectile.motion.velocity);
        value(projectile.combat.spawned_tick);
        value(projectile.motion.remaining_range);
        value(projectile.motion.radius);
        value(projectile.combat.damage);
        value(projectile.combat.skill);
        value(projectile.combat.upgrade_mask);
        value(projectile.combat.origin);
        value(projectile.combat.cast_id);
        value(projectile.combat.source_upgrade);
        value(projectile.combat.source_relic);
        value(projectile.combat.source_enemy);
        value(projectile.combat.pierce_remaining);
        value(projectile.combat.bounce_remaining);
        value(projectile.combat.hit_count);
        for (std::size_t index = 0; index < projectile.combat.hit_count; ++index)
            value(projectile.combat.hit_ids[index]);
        value(projectile.combat.explosion_radius);
        value(projectile.combat.explosion_damage);
        value(projectile.combat.explosion_source_upgrade);
        value(projectile.combat.bleed_stacks);
        value(projectile.combat.burn);
        value(projectile.combat.slow);
        value(projectile.combat.returning);
        value(projectile.combat.homing);
        value(projectile.combat.homing_target);
        value(projectile.combat.full_charge);
        value(projectile.combat.charge_ratio);
        value(projectile.combat.carried_burn_attack);
        value(projectile.combat.carried_burn_expires);
        value(projectile.combat.dead);
    }

    count(actors->areas.size());
    for (const auto &area : actors->areas)
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

    count(actors->pickups.size());
    for (const auto &pickup : actors->pickups)
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

    count(combat_state->combat.commands.size());
    for (const auto &event : combat_state->combat.commands)
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
    count(combat_state->scheduled_actions.size());
    for (const auto &action : combat_state->scheduled_actions)
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
    count(combat_state->boss_actions.size());
    for (const auto &action : combat_state->boss_actions)
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
    count(combat_state->cast_hits.size());
    for (const auto &record : combat_state->cast_hits)
    {
        value(record.cast_id);
        value(record.target);
        value(record.count);
    }
    count(combat_state->area_hits.size());
    for (const auto &record : combat_state->area_hits)
    {
        value(record.area_id);
        value(record.target);
        value(record.count);
    }
    count(combat_state->cast_runtimes.size());
    for (const auto &runtime : combat_state->cast_runtimes)
    {
        value(runtime.cast_id);
        value(runtime.skill);
        value(runtime.upgrade_mask);
        value(runtime.normal_hits);
        value(runtime.kills);
        value(runtime.transfer_count);
        value(runtime.spawn_count);
        // Unused budgets preserve the checksum stream of unaffected casts.
        if (runtime.cooldown_ticks_saved != 0) value(runtime.cooldown_ticks_saved);
        value(runtime.basic_relic_triggers);
        value(runtime.triggered);
        value(runtime.refund_triggered);
        value(runtime.full_charge);
        value(runtime.stored_burn_attack);
        value(runtime.stored_burn_remaining);
        value(runtime.has_stored_burn);
        value(runtime.terminal_target);
    }
    count(progression->waves.size());
    for (const auto &wave : progression->waves)
    {
        value(wave.start);
        value(wave.total);
        value(wave.emitted);
    }
    if (!progression->pending_boss_spawns.empty())
    {
        count(progression->pending_boss_spawns.size());
        for (const auto &pending : progression->pending_boss_spawns)
        {
            value(pending.kind);
            vector2(pending.position);
            value(pending.due);
        }
    }
    if (!progression->pending_enemy_spawns_delayed.empty())
    {
        count(progression->pending_enemy_spawns_delayed.size());
        for (const auto &pending : progression->pending_enemy_spawns_delayed)
        {
            value(pending.kind);
            vector2(pending.position);
            value(pending.random_key);
            value(pending.due);
        }
    }
    value(progression->card_count);
    for (std::size_t index = 0; index < progression->card_count; ++index)
    {
        value(progression->cards[index].kind);
        value(progression->cards[index].subject);
        value(progression->cards[index].upgrade);
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
    for (auto &cooldown : actors->player.cooldowns)
    {
        cooldown -= cooldown != 0;
    }
    if (actors->player.charging)
    {
        const auto &definition = rules.skills[
            static_cast<std::size_t>(SkillKind::ChargedShot)];
        const auto &upgrades = rules.upgrades.charged_shot;
        const auto mask = actors->player.upgrades[
            static_cast<std::size_t>(SkillKind::ChargedShot)];
        const auto maximum_ticks = HasUpgrade(mask, 1)
                                       ? upgrades.extended_full_charge_explosion
                                             .maximum_charge_time_ticks
                                       : definition.maximum_charge_time_ticks;
        if ((tick + kPlayerRenderId) % 6 == 0)
        {
            const bool faster_charge = HasUpgrade(mask, 2);
            if (faster_charge && actors->player.charge_cast_id == 0)
                actors->player.charge_cast_id = TakeSimulationIdentity(next_cast_id);
            auto elapsed = std::min(tick - actors->player.charge_start,
                                    maximum_ticks);
            if (faster_charge)
                elapsed = std::min(maximum_ticks, static_cast<Tick>(
                    elapsed / upgrades.faster_charge.charge_time_multiplier));
            const float ratio = static_cast<float>(elapsed) /
                static_cast<float>(maximum_ticks);
            EmitVfx(DomainSignalKind::ChargedShotPulse,
                    faster_charge ? actors->player.position :
                        Add(actors->player.position,
                            Multiply(actors->player.aim, 0.65f)),
                    actors->player.aim, 1.0f, 1.1f);
            if (faster_charge)
            {
                auto &pulse = combat_state->domain_signals.back();
                pulse.vfx_ratio01 = ratio;
                pulse.upgrade_skill = static_cast<std::uint8_t>(SkillKind::ChargedShot);
                pulse.upgrade_index = 1;
                pulse.upgrade_stage = UpgradeVisualStage::Spawn;
                pulse.upgrade_cast_id = actors->player.charge_cast_id;
                pulse.upgrade_owner_id = kPlayerRenderId;
            }
        }
        const auto ready_ticks = HasUpgrade(mask, 2)
                                     ? static_cast<Tick>(std::ceil(
                                           maximum_ticks * upgrades.faster_charge
                                                               .charge_time_multiplier))
                                     : maximum_ticks;
        if (tick - actors->player.charge_start == ready_ticks)
            EmitVfx(DomainSignalKind::ChargedShotReady,
                    Add(actors->player.position, Multiply(actors->player.aim, 0.65f)),
                    actors->player.aim, 1.0f, 1.1f);
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

Float2 GameSimulation::SimulationWorld::SpawnPosition(std::uint64_t salt, bool &used_fallback) const noexcept
{
    const auto &placement = rules.spawn_placement;
    constexpr std::uint32_t attempts = 32;
    for (std::uint32_t attempt = 0; attempt < attempts; ++attempt)
    {
        const auto key = salt + attempt;
        const auto angle = RandomUnit(key, 0x535041574Eull) * 2.0f * kPi;
        const auto distance = placement.minimum_player_distance_m +
                              RandomUnit(key, 0x44495354ull) *
                                  (placement.maximum_player_distance_m -
                                   placement.minimum_player_distance_m);
        const auto position = Add(actors->player.position,
                                  {std::cos(angle) * distance, std::sin(angle) * distance});
        if (placement.require_inside_arena && !ContainsArenaPoint(rules.arena_boundary, position, 1.0f))
            continue;
        const auto spawn_radius = rules.enemies.front().collision_radius;
        if (std::ranges::any_of(ArenaObstacles(), [&](const ArenaObstacle2D &obstacle) {
                const auto radius = obstacle.radius + spawn_radius;
                return DistanceSquared(position, obstacle.center) < radius * radius;
            }))
            continue;
        if (placement.require_outside_max_zoom_view)
        {
            const auto offset = Subtract(position, actors->player.position);
            const auto forward = offset.x * placement.max_zoom_view_forward_x +
                                 offset.y * placement.max_zoom_view_forward_z;
            const auto right = offset.x * placement.max_zoom_view_forward_z -
                               offset.y * placement.max_zoom_view_forward_x;
            if (forward >= placement.max_zoom_view_min_forward_m &&
                forward <= placement.max_zoom_view_max_forward_m &&
                std::abs(right) <= placement.max_zoom_view_half_right_m)
                continue;
        }
        used_fallback = false;
        return position;
    }

    const auto points = std::span(rules.arena_boundary.points.data(),
                                  rules.arena_boundary.count);
    const auto farthest = std::ranges::max_element(
        points, {}, [this](const Float2 &candidate) {
            return LengthSquared(Subtract(candidate, actors->player.position));
        });
    used_fallback = true;
    return farthest != points.end()
               ? ClampToArena(rules.arena_boundary, *farthest, 1.0f)
               : Float2{};
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
    if (!config.scenario.automatic_spawning) return;
    const auto final_boss_tick = !final_boss_spawned &&
        growth_ticks == rules.progression.boss_spawn_ticks[static_cast<std::size_t>(BossKind::Final)];
    if (!final_boss_tick)
        CommitDelayedEnemySpawns();
    CommitBossSpawns();
    if (!final_boss_spawned && !final_boss_tick)
    {
        const auto &stage = CurrentSpawnStage();
        spawn_accumulator += stage.per_second;
        while (spawn_accumulator >= 60.0f)
        {
            spawn_accumulator -= 60.0f;
            const auto salt = TakeSimulationIdentity(next_enemy_random_key);
            QueueEnemySpawn(ChooseEnemyKind(salt), salt);
        }
    }
    for (const auto &wave : rules.waves)
    {
        if (growth_ticks == wave.start_tick)
        {
            progression->waves.push_back({growth_ticks, wave.duration_ticks, wave.count, 0});
            DomainSignalGeometry geometry{};
            geometry.source_id = wave.start_tick;
            geometry.start_tick = geometry.end_tick = tick;
            EmitVfx(DomainSignalKind::WaveStarted, {}, {}, rules.arena_half_extent,
                    0.025f, 0, 0, geometry);
        }
    }
    for (auto &wave : progression->waves)
    {
        const auto elapsed = growth_ticks - wave.start;
        const auto desired = static_cast<std::uint16_t>(
            std::min<std::uint64_t>(wave.total,
                (static_cast<std::uint64_t>(elapsed + 1) * wave.total +
                 wave.duration - 1) / wave.duration));
        while (!final_boss_tick && wave.emitted < desired &&
               (!final_boss_spawned || !rules.progression.stop_normal_spawns_at_final_boss))
        {
            const auto salt = TakeSimulationIdentity(next_enemy_random_key);
            QueueEnemySpawn(ChooseEnemyKind(salt), salt);
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
        progression->waves.clear();
        progression->pending_enemy_spawns_delayed.clear();
        SpawnBoss(BossKind::Final, rules.boss_common.spawn_warning_ticks);
    }
}

float GameSimulation::SimulationWorld::SlowMultiplier(ConstEnemyRow enemy) const
{
    float multiplier = 1.0f;
    for (const auto &slow : enemy.status.slows)
    {
        if(slow.expires>tick) multiplier *= 1.0f-slow.reduction;
    }
    return multiplier;
}

void GameSimulation::SimulationWorld::BossAi(EnemyRow boss)
{
    auto timing = Measure(SimulationTimingId::BossAi);
    if (boss.action.boss_action_until != 0 && tick > boss.action.boss_action_until)
    {
        boss.action.boss_action_started = 0;
        boss.action.boss_action_until = 0;
        boss.action.boss_action_recoil = false;
    }
    if (tick < boss.action.invulnerable_until)
    {
        boss.motion.velocity = {};
        return;
    }
    if (tick < boss.action.dash_until) return;
    if (boss.action.dash_until != 0)
    {
        DomainSignalGeometry geometry{};
        geometry.kind = DomainSignalGeometryKind::Circle;
        // The dash sweeps this contact radius against the player's center.
        // Its endpoint visual must not invent a larger explosion footprint.
        geometry.radius = rules.boss_common.collision_radius + rules.stats.player_collision_radius;
        geometry.start_tick = geometry.end_tick = tick;
        geometry.source_id = boss.id.value;
        EmitVfx(DomainSignalKind::BossDashImpact, boss.motion.position,
                Normalize(boss.motion.velocity), 1.0f, 0.15f, 0, 0, geometry);
        boss.motion.velocity = {};
        boss.action.dash_until = 0;
        boss.action.dash_started = 0;
    }
    const auto kind = *boss.life.boss;
    const auto &definition = rules.bosses[static_cast<std::size_t>(kind)];
    const auto to_player = Subtract(actors->player.position, boss.motion.position);
    const auto distance = Length(to_player);
    const auto direction = Normalize(to_player);
    if (tick < boss.action.pattern_ready)
    {
        boss.motion.velocity = {};
        return;
    }
    const auto phase = boss.action.final_phase;
    const auto preferred_near = phase == 2
                                    ? definition.phase_two_preferred_distance_near
                                    : definition.preferred_distance_near;
    const auto preferred_far = phase == 2
                                   ? definition.phase_two_preferred_distance_far
                                   : definition.preferred_distance_far;
    if (kind == BossKind::TenMinute && distance > preferred_far)
    {
        const auto navigation_target = SegmentClear2D(
                                           boss.motion.position, actors->player.position,
                                           rules.boss_common.collision_radius,
                                           rules.arena_boundary, ArenaObstacles())
                                           ? actors->player.position
                                           : TimedNavigationWaypoint(navigation->boss_navigation,boss.motion.position,
                                                                           actors->player.position);
        boss.motion.velocity = Multiply(Normalize(Subtract(navigation_target, boss.motion.position)),
                                 boss.cold.move_speed * SlowMultiplier(boss));
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
        boss.motion.velocity = {};
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
        Random(boss.cold.random_key, 0x424F5353504154ull) % total_pattern_weight <
            rules.boss_common.preferred_pattern_weight;
    auto pattern = preferred_roll
                       ? preferred_pattern
                       : static_cast<std::uint8_t>((preferred_pattern + 1) % phase_pattern_count);
    if (pattern == boss.action.last_pattern &&
        boss.action.repeat_count >= rules.boss_common.maximum_same_pattern_repeats)
        pattern = static_cast<std::uint8_t>((pattern + 1) % phase_pattern_count);
    boss.action.repeat_count = pattern == boss.action.last_pattern ? boss.action.repeat_count + 1 : 1;
    boss.action.last_pattern = pattern;
    boss.motion.velocity = {};

    const auto player_velocity = Multiply(
        Subtract(actors->player.position, actors->player.previous_position), 60.0f);
    const auto cast_id = TakeSimulationIdentity(next_cast_id);
    ++Metrics().enemy_attack_attempts[EnemyTelemetryIndex(boss)];
    const auto add = [&](BossAction action) {
        if(combat_state->next_boss_schedule_sequence==std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Boss action ordering sequence exhausted.");
        action.schedule_sequence=combat_state->next_boss_schedule_sequence++;
        const auto previous = std::ranges::find_if(
            combat_state->boss_actions.rbegin(), combat_state->boss_actions.rend(), [&](const BossAction &candidate) {
                return candidate.cast_id == action.cast_id;
            });
        action.animation_started = previous == combat_state->boss_actions.rend() ? tick
                                   : previous->due == action.due
                                       ? previous->animation_started
                                       : previous->due;
        action.warning_sequence = PendingSimulationSequence(event_sequence);
        action.warning_started = tick;
        combat_state->boss_actions.push_back(action);
        combat_state->boss_actions_dirty=true;
        const auto context = static_cast<std::uint8_t>(kind);
        const auto position = action.kind == BossActionKind::Dash ||
                                      action.kind == BossActionKind::Volley
                                  ? boss.motion.position : action.position;
        const auto signal = action.kind == BossActionKind::Dash
                                ? DomainSignalKind::BossDashTelegraphed
                            : action.kind == BossActionKind::Volley
                                ? DomainSignalKind::BossVolleyTelegraphed
                            : action.kind == BossActionKind::Area
                                ? DomainSignalKind::BossAreaTelegraphed
                                : DomainSignalKind::BossShockwaveTelegraphed;
        if (action.kind == BossActionKind::Dash)
        {
            const auto dash_direction = LengthSquared(action.direction) > 0.0001f
                ? Normalize(action.direction) : Normalize(Subtract(actors->player.position, boss.motion.position));
            DomainSignalGeometry geometry{};
            geometry.kind = DomainSignalGeometryKind::Line;
            geometry.width = 2.0f * (rules.boss_common.collision_radius +
                                     rules.stats.player_collision_radius);
            geometry.range = action.distance;
            geometry.start_tick = tick;
            geometry.end_tick = action.due;
            geometry.source_id = action.warning_sequence;
            const auto end = Add(boss.motion.position, Multiply(dash_direction, action.distance));
            geometry.end_position = {end.x, 0.025f, end.y};
            EmitVfx(signal, boss.motion.position, dash_direction, 1.0f, 0.025f,
                    context, 0, geometry);
        }
        else if (action.kind == BossActionKind::Volley)
        {
            const auto aim_direction = LengthSquared(action.direction) > 0.0001f
                ? Normalize(action.direction) : Normalize(Subtract(actors->player.position, boss.motion.position));
            const auto volley_direction = Rotate(aim_direction, action.angle_offset);
            DomainSignalGeometry geometry{};
            geometry.kind = DomainSignalGeometryKind::Cone;
            geometry.range = rules.boss_common.projectile_range;
            geometry.half_angle_degrees = action.arc_degrees * 0.5f;
            geometry.start_tick = tick;
            geometry.end_tick = action.due;
            geometry.source_id = action.warning_sequence;
            EmitVfx(signal, boss.motion.position, volley_direction, 1.0f, 0.025f,
                    context, 0, geometry);
            if (action.angle_offset != 0.0f)
                EmitVfx(DomainSignalKind::BossVolleyOffsetTelegraphed,
                        boss.motion.position, volley_direction, 1.0f, 0.025f,
                        context, 0, geometry);
        }
        else if (action.kind == BossActionKind::Shockwave)
        {
            DomainSignalGeometry geometry{};
            geometry.kind = DomainSignalGeometryKind::RingGaps;
            geometry.inner_radius = action.distance;
            geometry.outer_radius = action.radius;
            geometry.start_tick = tick;
            geometry.end_tick = action.due;
            geometry.source_id = action.warning_sequence;
            geometry.gap_count = action.safe_gap_count;
            geometry.gap_half_width_degrees = action.safe_gap_degrees * 0.5f;
            geometry.gap_offset_degrees = static_cast<float>(action.cast_id % 360);
            EmitVfx(signal, action.position, {}, 1.0f, 0.025f, context, 0, geometry);
        }
        else
        {
            DomainSignalGeometry geometry{};
            geometry.kind = DomainSignalGeometryKind::Circle;
            geometry.radius = action.radius;
            geometry.start_tick = action.warning_started;
            geometry.end_tick = action.due;
            geometry.source_id = action.warning_sequence;
            EmitVfx(signal, position, {}, 1.0f, 0.025f, context, 0, geometry);
        }
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
        action.position = boss.motion.position;
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
            actors->player.position,
            Multiply(player_velocity,
                     static_cast<float>(selected.prediction_lead_ticks) * kTickSeconds));
        const Float2 clamped_center = ClampToArena(rules.arena_boundary, center,
                                                   selected.radius * 2.0f);
        const auto area_count = std::max<std::uint8_t>(1, selected.area_count);
        const auto angle_offset = RandomUnit(
            boss.cold.random_key, cast_id ^ 0x47524F554E44ull) * 360.0f;
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
        ++boss.action.phase_pattern_count;
    }

    const auto recovery = kind == BossKind::Final && boss.action.final_phase == 2
                              ? (boss.action.phase_pattern_count % 3 == 0
                                     ? definition.phase2_cycle_recovery_ticks
                                     : definition.phase2_pattern_interval_ticks)
                              : definition.recovery_ticks;
    boss.action.pattern_ready = last_due + recovery;
}

void GameSimulation::SimulationWorld::AiIntentPhase()
{
    const auto target_cell = navigation->enemy_navigation.Cell(actors->player.position);
    if (target_cell != navigation->enemy_navigation_target)
    {
        navigation->enemy_navigation.RequestFlow(actors->player.position);
        navigation->boss_navigation.RequestFlow(actors->player.position);
        navigation->enemy_navigation_target = target_cell;
    }
    auto enemy_ai_timing = Measure(SimulationTimingId::EnemyPerceptionDecision);
    for (auto enemy : actors->enemies)
    {
        if (enemy.life.dead)
        {
            continue;
        }
        if (enemy.life.boss)
        {
            BossAi(enemy);
            continue;
        }
        const auto distance = Length(Subtract(actors->player.position, enemy.motion.position));
        enemy.motion.velocity = {};
        if (enemy.action.attacking)
        {
            if (tick >= enemy.action.attack_resolve)
            {
                const auto &definition = rules.enemies[static_cast<std::size_t>(enemy.life.kind)];
                if (enemy.life.kind == EnemyKind::Melee)
                {
                    if (distance <= definition.attack_range &&
                        SegmentClear2D(enemy.motion.position, actors->player.position, 0.0f,
                                       rules.arena_boundary, ArenaObstacles()))
                    {
                        EmitVfx(DomainSignalKind::MeleeEnemyHit, actors->player.position,
                                enemy.action.locked_aim, 1.0f, 0.1f);
                        QueueDamage(0, enemy.cold.damage, SkillKind::Count,
                                   EffectOrigin::Original, enemy.action.attack_cast_id,
                                   0, false, 0.0f, 0, kNoTelemetrySource,
                                   kNoTelemetrySource,
                                   static_cast<std::uint8_t>(
                                       EnemyTelemetryIndex(enemy)), 0, enemy.action.locked_aim);
                    }
                }
                else if (enemy.life.kind == EnemyKind::Ranged)
                {
                    auto shot = FireProjectile(SkillKind::BasicAttack, enemy.motion.position,
                                                enemy.action.locked_aim,
                                                 static_cast<float>(enemy.cold.damage),
                                                 EffectOrigin::Original,
                                                 enemy.action.attack_cast_id, 0, false,
                                                 kNoTelemetrySource,
                                                 kNoTelemetrySource,
                                                 static_cast<std::uint8_t>(
                                                     EnemyTelemetryIndex(enemy)),
                                                 enemy.id.value);
                    if (shot)
                    {
                        shot->motion.velocity = Multiply(enemy.action.locked_aim,
                                                   definition.projectile_speed);
                        shot->motion.remaining_range = definition.projectile_range;
                    }
                }
                else if (distance <= definition.suicide_explosion_radius)
                {
                    DomainSignalGeometry explosion_geometry;
                    explosion_geometry.kind = DomainSignalGeometryKind::Circle;
                    explosion_geometry.radius = definition.suicide_explosion_radius;
                    EmitVfx(DomainSignalKind::SuicideEnemyExploded,
                            enemy.motion.position, {}, 1.0f, 0.15f, 0, enemy.id.value,
                            explosion_geometry);
                    if (SegmentClear2D(enemy.motion.position, actors->player.position, 0.0f,
                                       rules.arena_boundary, ArenaObstacles()))
                        QueueDamage(0, enemy.cold.damage, SkillKind::Count,
                               EffectOrigin::Original, enemy.action.attack_cast_id,
                               0, false, 0.0f, 0, kNoTelemetrySource,
                               kNoTelemetrySource,
                               static_cast<std::uint8_t>(EnemyTelemetryIndex(enemy)), 0,
                               Normalize(Subtract(actors->player.position, enemy.motion.position)));
                    enemy.life.dead = true;
                }
                enemy.action.attacking = false;
                enemy.action.next_attack = tick + enemy.cold.attack_cooldown_ticks;
            }
            continue;
        }
        if (tick >= enemy.action.next_attack &&
            ((enemy.life.kind == EnemyKind::Melee && distance <= enemy.cold.attack_range) ||
             (enemy.life.kind == EnemyKind::Ranged && distance <= enemy.cold.attack_range) ||
             (enemy.life.kind == EnemyKind::Suicide && distance <= enemy.cold.attack_range)))
        {
            const auto attack_direction = Normalize(Subtract(actors->player.position, enemy.motion.position));
            ++Metrics().enemy_attack_attempts[EnemyTelemetryIndex(enemy)];
            enemy.action.attack_cast_id = TakeSimulationIdentity(next_enemy_attack_id);
            enemy.action.attacking = true;
            enemy.action.attack_resolve = tick + enemy.action.warning_ticks;
            enemy.action.locked_aim = attack_direction;
            if (enemy.life.kind == EnemyKind::Melee)
            {
                DomainSignalGeometry geometry{};
                geometry.kind = DomainSignalGeometryKind::Circle;
                geometry.radius = rules.enemies[static_cast<std::size_t>(enemy.life.kind)].attack_range;
                geometry.start_tick = tick;
                geometry.end_tick = enemy.action.attack_resolve;
                geometry.source_id = enemy.action.attack_cast_id;
                EmitVfx(DomainSignalKind::MeleeEnemyWindup, enemy.motion.position,
                        attack_direction, 1.0f, 0.1f, 0, enemy.id.value, geometry);
            }
            else if (enemy.life.kind == EnemyKind::Ranged)
            {
                const auto &definition = rules.enemies[static_cast<std::size_t>(enemy.life.kind)];
                enemy.action.warning_sequence = PendingSimulationSequence(event_sequence);
                DomainSignalGeometry geometry{};
                geometry.kind = DomainSignalGeometryKind::Line;
                geometry.width = 2.0f * definition.ranged_projectile_radius;
                geometry.range = definition.projectile_range;
                geometry.start_tick = tick;
                geometry.end_tick = enemy.action.attack_resolve;
                geometry.source_id = enemy.action.warning_sequence;
                const auto end = Add(enemy.motion.position, Multiply(enemy.action.locked_aim, geometry.range));
                geometry.end_position = {end.x, 0.025f, end.y};
                EmitVfx(DomainSignalKind::RangedEnemyTelegraphed, enemy.motion.position,
                        enemy.action.locked_aim, 1.0f, 0.025f, 0, enemy.id.value, geometry);
            }
            else if (enemy.life.kind == EnemyKind::Suicide)
            {
                enemy.action.warning_sequence = PendingSimulationSequence(event_sequence);
                DomainSignalGeometry geometry{};
                geometry.kind = DomainSignalGeometryKind::Circle;
                geometry.radius = rules.enemies[static_cast<std::size_t>(enemy.life.kind)].suicide_explosion_radius;
                geometry.start_tick = tick;
                geometry.end_tick = enemy.action.attack_resolve;
                geometry.source_id = enemy.action.warning_sequence;
                EmitVfx(DomainSignalKind::SuicideEnemyCharging, enemy.motion.position,
                        attack_direction, 1.0f, 0.15f, 0, enemy.id.value, geometry);
            }
            continue;
        }
        if (enemy.life.kind != EnemyKind::Ranged || distance > enemy.cold.attack_range)
        {
            const auto radius = rules.enemies[static_cast<std::size_t>(enemy.life.kind)].collision_radius;
            const auto navigation_target = SegmentClear2D(enemy.motion.position, actors->player.position,
                                                          radius, rules.arena_boundary,
                                                          ArenaObstacles())
                                               ? actors->player.position
                                               : TimedNavigationWaypoint(navigation->enemy_navigation,enemy.motion.position,
                                                                                actors->player.position);
            const auto direction = Normalize(Subtract(navigation_target, enemy.motion.position));
            enemy.motion.velocity = direction;
        }
        enemy.motion.velocity = Multiply(enemy.motion.velocity, enemy.cold.move_speed * SlowMultiplier(enemy));
    }
}

} // namespace hs
