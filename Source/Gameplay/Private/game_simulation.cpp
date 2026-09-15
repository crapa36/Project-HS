#include "simulation_world.hpp"

namespace hs
{

using namespace gameplay_detail;


GameSimulation::GameSimulation() : impl_(std::make_unique<SimulationWorld>())
{
}

GameSimulation::~GameSimulation() = default;

Result GameSimulation::Initialize(const SimulationConfig &config)
{
    return Initialize(config, SimulationRules::Defaults());
}

Result GameSimulation::Initialize(const SimulationConfig &config,
                                  const SimulationRules &rules)
{
    if (impl_->initialized)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_gameplay",
                               "GameSimulation already initialized.");
    }
    impl_->config = config;
    impl_->rules = rules;
    const auto obstacles = impl_->ArenaObstacles();
    impl_->navigation->player_navigation.Rebuild(rules.arena_boundary, obstacles,
                                     rules.stats.player_collision_radius);
    impl_->navigation->enemy_navigation.Rebuild(rules.arena_boundary, obstacles,
                                     rules.enemies.front().collision_radius);
    impl_->navigation->boss_navigation.Rebuild(rules.arena_boundary, obstacles,
                                    rules.boss_common.collision_radius);
    impl_->session_phase = config.start_in_main_menu ? SessionPhase::MainMenu : SessionPhase::Playing;
    impl_->InitializePlayerState();
    impl_->initialized = true;
    impl_->checksum = impl_->CalculateChecksum();
    return Result::Success();
}

TickResult GameSimulation::TickFixed(const InputFrame &input,
                                     std::chrono::nanoseconds fixed_delta)
{
    if (!impl_->initialized)
    {
        return {};
    }
    impl_->current_input = input;
    for (const auto &action : input.ui_actions) ApplyUiAction(action);
    impl_->ProcessInput();
    if (impl_->session_phase != SessionPhase::Playing)
    {
        impl_->actors->player.buffered_skill = SkillKind::Count;
        impl_->actors->player.buffered_skill_expires = 0;
        impl_->actors->player.buffered_skill_slot = 0xFF;
        impl_->checksum = impl_->CalculateChecksum();
        return {impl_->tick, impl_->checksum, impl_->session_phase};
    }
    (void)fixed_delta;
    ++impl_->tick;
    impl_->RunPipeline();
    return {impl_->tick, impl_->checksum, impl_->session_phase};
}

GameplayChecksum GameSimulation::ComputeChecksum() const
{
    return impl_->checksum;
}

const SimulationRules &GameSimulation::Rules() const noexcept
{
    return impl_->rules;
}

SessionProbe GameSimulation::GetSessionProbe() const noexcept
{
    SessionProbe probe;
    probe.tick = impl_->tick;
    probe.growth_ticks = impl_->growth_ticks;
    probe.boss_fight_ticks = impl_->boss_fight_ticks;
    probe.phase = impl_->session_phase;
    probe.level = impl_->actors->player.level;
    probe.experience = impl_->actors->player.experience;
    probe.experience_to_next = ExperienceForLevel(impl_->rules, impl_->actors->player.level);
    probe.health = impl_->actors->player.health;
    probe.max_health = impl_->actors->player.max_health;
    probe.player_position = impl_->actors->player.position;
    probe.aim_direction = impl_->actors->player.aim;
    probe.facing_direction = impl_->actors->player.facing;
    probe.normal_enemy_count = impl_->NormalEnemyCount();
    probe.boss_count = impl_->BossCount();
    probe.boss_warning_count = static_cast<std::uint32_t>(impl_->combat_state->boss_actions.size());
    probe.boss_dashing_count = static_cast<std::uint32_t>(
        std::ranges::count_if(impl_->actors->enemies, [&](const EnemyActor &enemy) {
            return enemy.boss.has_value() && !enemy.dead && impl_->tick < enemy.dash_until;
        }));
    probe.enemy_area_count = static_cast<std::uint32_t>(
        std::ranges::count_if(impl_->actors->areas, [](const AreaActor &area) {
            return !area.dead && area.kind == AreaKind::EnemyDamage;
        }));
    probe.player_projectile_count = impl_->ProjectileCount(true);
    probe.enemy_projectile_count = impl_->ProjectileCount(false);
    probe.pickup_count = static_cast<std::uint32_t>(impl_->actors->pickups.size());
    probe.kills = impl_->telemetry->kills;
    probe.damage_dealt = impl_->telemetry->damage_dealt;
    probe.damage_by_skill = impl_->telemetry->damage_by_skill;
    probe.damage_taken = impl_->telemetry->damage_taken;
    probe.healing = impl_->telemetry->healing;
    probe.level_rerolls_remaining = impl_->actors->player.level_rerolls;
    probe.relic_rerolls_remaining = impl_->actors->player.relic_rerolls;
    probe.pending_stat_points = impl_->actors->player.pending_stat_points;
    probe.active_skill_count = static_cast<std::uint8_t>(std::ranges::count_if(
        impl_->actors->player.loadout, [](SkillKind skill) { return skill != SkillKind::Count; }));
    probe.skill_loadout = impl_->actors->player.loadout;
    probe.skill_levels = impl_->actors->player.skill_levels;
    probe.upgrade_masks = impl_->actors->player.upgrades;
    for (std::size_t index = 0; index < probe.cooldown_ticks.size(); ++index)
    {
        probe.cooldown_ticks[index] = static_cast<std::uint32_t>(
            std::min<Tick>(impl_->actors->player.cooldowns[index],
                           std::numeric_limits<std::uint32_t>::max()));
    }
    probe.stat_points = impl_->actors->player.stats;
    probe.relic_mask = impl_->actors->player.relic_mask;
    probe.cards = impl_->progression->cards;
    probe.card_count = impl_->progression->card_count;
    probe.final_boss_spawned = impl_->final_boss_spawned;
    probe.final_boss_phase_two = std::ranges::any_of(impl_->actors->enemies, [](const EnemyActor &enemy) {
        return enemy.boss == BossKind::Final && enemy.final_phase == 2;
    });
    return probe;
}

Result GameSimulation::ApplyDebugCommand(const DebugCommand &command)
{
    if (!impl_->initialized)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_gameplay",
                               "Simulation is not initialized.");
    }
    switch (command.kind)
    {
    case DebugCommandKind::StartSession:
        impl_->StartSession();
        break;
    case DebugCommandKind::SetGrowthTick:
        impl_->growth_ticks = command.value;
        break;
    case DebugCommandKind::DamagePlayer:
        impl_->actors->player.health -= static_cast<std::int32_t>(command.value);
        break;
    case DebugCommandKind::HealPlayer:
        impl_->actors->player.health = std::min(
            impl_->actors->player.max_health,
            impl_->actors->player.health + static_cast<std::int32_t>(command.value));
        break;
    case DebugCommandKind::DamageFinalBoss:
        for (auto &enemy : impl_->actors->enemies)
        {
            if (enemy.boss == BossKind::Final)
            {
                enemy.health -= static_cast<std::int32_t>(command.value);
                break;
            }
        }
        break;
    case DebugCommandKind::GrantExperience:
        impl_->actors->player.experience += static_cast<std::uint32_t>(command.value);
        break;
    case DebugCommandKind::GrantSkill:
    {
        const auto skill = static_cast<SkillKind>(command.value);
        if (skill <= SkillKind::BasicAttack || skill >= SkillKind::Count)
        {
            return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay",
                                   "Debug skill is invalid.");
        }
        if (impl_->actors->player.skill_levels[static_cast<std::size_t>(skill)] == 0)
        {
            impl_->actors->player.skill_levels[static_cast<std::size_t>(skill)] =
                impl_->rules.skills[static_cast<std::size_t>(skill)].starting_level;
            const auto slot = std::ranges::find(impl_->actors->player.loadout, SkillKind::Count);
            if (slot != impl_->actors->player.loadout.end()) *slot = skill;
        }
        break;
    }
    case DebugCommandKind::GrantUpgrade:
        if (command.value >= kCombatSkillCount || command.secondary >= kUpgradeCount)
        {
            return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay",
                                   "Debug upgrade is invalid.");
        }
        impl_->actors->player.upgrades[command.value] |= 1u << command.secondary;
        impl_->actors->player.skill_levels[command.value] = static_cast<std::uint8_t>(
            impl_->rules.skills[command.value].starting_level +
            std::popcount(impl_->actors->player.upgrades[command.value]));
        break;
    case DebugCommandKind::GrantRelic:
        if (command.value >= kRelicCount)
        {
            return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay",
                                   "Debug relic is invalid.");
        }
        impl_->actors->player.relic_mask |= RelicMask{1} << command.value;
        impl_->combat_state->relic_rules.Rebuild(impl_->actors->player.relic_mask);
        break;
    case DebugCommandKind::SpawnEnemy:
        impl_->SpawnEnemy(static_cast<EnemyKind>(command.value % 3), command.position);
        break;
    case DebugCommandKind::SpawnBoss:
        if (command.value >= 3 ||
            !impl_->SpawnBoss(static_cast<BossKind>(command.value), 0))
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_gameplay",
                                   "Debug boss could not be spawned.");
        }
        impl_->CommitBossSpawns();
        break;
    case DebugCommandKind::SelectCard:
        if (!impl_->SelectCard(static_cast<std::size_t>(command.value)))
        {
            return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay",
                                   "Debug card index is invalid.");
        }
        break;
    case DebugCommandKind::AssignStat:
        impl_->AssignStat(static_cast<StatKind>(command.value % kStatCount));
        break;
    case DebugCommandKind::SetStat:
        if (command.value >= kStatCount ||
            command.secondary > impl_->rules.stats.maximum_points_per_stat)
        {
            return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay",
                                   "Debug stat or level is invalid.");
        }
        impl_->actors->player.stats[command.value] =
            static_cast<std::uint8_t>(command.secondary);
        if (command.value == static_cast<std::uint64_t>(StatKind::MaxHealth))
        {
            const auto &allocation = impl_->rules.stats.allocations[command.value];
            impl_->actors->player.max_health = RoundDamage(
                impl_->rules.stats.base_maximum_hp *
                (1.0f + allocation.amount_per_point * command.secondary));
            impl_->actors->player.health = impl_->actors->player.max_health;
        }
        break;
    case DebugCommandKind::Reroll:
    {
        auto &rerolls = impl_->session_phase == SessionPhase::RelicSelection
                            ? impl_->actors->player.relic_rerolls
                            : impl_->actors->player.level_rerolls;
        auto &sequence = impl_->session_phase == SessionPhase::RelicSelection
                             ? impl_->progression->relic_reroll_sequence
                             : impl_->progression->level_reroll_sequence;
        if (rerolls == 0 || impl_->progression->card_count == 0)
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_gameplay",
                                   "No reroll is available.");
        }
        --rerolls;
        ++sequence;
        if (impl_->session_phase == SessionPhase::RelicSelection) impl_->GenerateRelicCards(true);
        else impl_->GenerateLevelCards(true);
        break;
    }
    case DebugCommandKind::TogglePause:
        impl_->session_phase = impl_->session_phase == SessionPhase::Paused ? SessionPhase::Playing
                                                            : SessionPhase::Paused;
        break;
    }
    impl_->checksum = impl_->CalculateChecksum();
    return Result::Success();
}

Result GameSimulation::ApplySimulationRules(const SimulationRules &rules)
{
    if (!impl_->initialized || impl_->session_phase != SessionPhase::MainMenu)
        return Result::Failure(ErrorCode::InvalidState, "hs_gameplay",
                               "Gameplay data Hot Reload requires the main menu.");
    impl_->rules = rules;
    return Result::Success();
}

SimulationObservation GameSimulation::GetObservation() const noexcept
{
    SimulationObservation observation;
    static_cast<SessionProbe &>(observation) = GetSessionProbe();
    observation.balance = impl_->Metrics();
    return observation;
}

SimulationDiagnostics GameSimulation::GetDiagnostics() const noexcept
{
    return {impl_->Metrics()};
}

void GameSimulation::ApplyUiAction(const UiAction &action)
{
    switch (action.kind)
    {
    case UiActionKind::StartSession:
        if (impl_->session_phase == SessionPhase::MainMenu) impl_->StartSession();
        break;
    case UiActionKind::ReturnToMainMenu:
        impl_->session_phase = SessionPhase::MainMenu;
        break;
    case UiActionKind::Quit: impl_->session_phase = SessionPhase::QuitRequested; break;
    case UiActionKind::Reroll:
        if (impl_->selection_input_guard_frames > 0 ||
            impl_->selection_waiting_for_release) break;
        if (impl_->session_phase == SessionPhase::RelicSelection && impl_->actors->player.relic_rerolls > 0)
        {
            --impl_->actors->player.relic_rerolls;
            ++impl_->progression->relic_reroll_sequence;
            impl_->GenerateRelicCards(true);
        }
        else if (impl_->session_phase == SessionPhase::CardSelection &&
                 impl_->actors->player.level_rerolls > 0)
        {
            --impl_->actors->player.level_rerolls;
            ++impl_->progression->level_reroll_sequence;
            impl_->GenerateLevelCards(true);
        }
        break;
    case UiActionKind::SelectCard:
        if (impl_->selection_input_guard_frames == 0 &&
            !impl_->selection_waiting_for_release) (void)impl_->SelectCard(action.value);
        break;
    case UiActionKind::AssignStat:
        if (impl_->selection_input_guard_frames == 0 &&
            !impl_->selection_waiting_for_release && action.value < kStatCount)
            impl_->AssignStat(static_cast<StatKind>(action.value));
        break;
    case UiActionKind::Resume:
        impl_->session_phase = SessionPhase::Playing;
        break;
    case UiActionKind::SwapLoadoutSlots:
        if (action.value < impl_->actors->player.loadout.size() &&
            action.secondary < impl_->actors->player.loadout.size())
            std::swap(impl_->actors->player.loadout[action.value],
                      impl_->actors->player.loadout[action.secondary]);
        break;
    }
    impl_->checksum = impl_->CalculateChecksum();
}

void GameSimulation::WriteReadModel(GameReadModelStorage &model) const
{
    model.Clear();
    model.tick = impl_->tick;
    model.checksum = impl_->checksum;
    model.seed = impl_->config.seed;
    model.session = GetSessionProbe();
    model.player = {impl_->actors->player.position,
                    impl_->actors->player.aim,
                    impl_->actors->player.facing,
                    impl_->actors->player.locomotion_blend,
                    impl_->actors->player.charging,
                    impl_->actors->player.charging_skill,
                    impl_->actors->player.charge_start,
                    impl_->actors->player.basic_attack_animation_start,
                    impl_->actors->player.basic_attack_animation_until,
                    impl_->actors->player.active_cast_tick,
                    impl_->actors->player.active_animation_start,
                    impl_->actors->player.active_animation_until,
                    impl_->actors->player.retreat_until,
                    impl_->actors->player.loadout,
                    impl_->actors->player.upgrades};
    model.effective_attack = impl_->EffectiveAttack();
    model.effective_attack_speed = impl_->EffectiveAttackSpeed();
    model.effective_move_speed = impl_->EffectiveMoveSpeed();
    model.effective_magnet_radius = impl_->EffectiveMagnetRadius();
    model.arena_half_extent = impl_->rules.arena_half_extent;
    model.arena_boundary = impl_->rules.arena_boundary;
    model.arena_obstacle_count = impl_->rules.arena_obstacle_count;
    model.arena_obstacles = impl_->rules.arena_obstacles;
    for (std::size_t index = 0; index < model.skills.size(); ++index)
    {
        const auto skill = static_cast<SkillKind>(index);
        const auto &definition = impl_->rules.skills[index];
        model.skills[index] = {
            index == 0 ? Tick{} : impl_->EffectiveCooldownTicks(skill),
            RoundDamage(impl_->EffectiveAttack() * definition.damage_coefficient),
            definition.range, definition.area_radius, definition.duration_ticks,
            definition.projectile_count, definition.pierce_count};
    }
    for (std::size_t index = 0; index < model.waves.size(); ++index)
        model.waves[index] = {
            impl_->rules.waves[index].start_tick,
            impl_->rules.waves[index].duration_ticks};
    if (impl_->actors->player.charging &&
        impl_->actors->player.charging_skill == SkillKind::ChargedShot)
    {
        const auto &definition = impl_->rules.skills[
            static_cast<std::size_t>(SkillKind::ChargedShot)];
        const auto &upgrades = impl_->rules.upgrades.charged_shot;
        const auto mask = impl_->actors->player.upgrades[
            static_cast<std::size_t>(SkillKind::ChargedShot)];
        const auto maximum_ticks = HasUpgrade(mask, 1)
                                       ? upgrades.extended_full_charge_explosion
                                             .maximum_charge_time_ticks
                                       : definition.maximum_charge_time_ticks;
        auto elapsed = std::min(impl_->tick - impl_->actors->player.charge_start, maximum_ticks);
        if (HasUpgrade(mask, 2))
            elapsed = std::min(
                maximum_ticks,
                static_cast<Tick>(elapsed /
                                  upgrades.faster_charge.charge_time_multiplier));
        const auto progress = static_cast<float>(elapsed) /
                              static_cast<float>(maximum_ticks);
        model.charge_range = std::lerp(definition.minimum_range,
                                       definition.maximum_range, progress);
        model.charge_radius = std::lerp(definition.minimum_collision_radius,
                                        definition.maximum_collision_radius,
                                        progress);
    }
    model.summary = {impl_->Metrics().direct_damage,
                     impl_->Metrics().derived_damage,
                     impl_->Metrics().damage_over_time,
                     impl_->Metrics().upgrade_damage,
                     impl_->Metrics().relic_damage,
                     impl_->Metrics().relic_triggers,
                     impl_->Metrics().relic_kills,
                     impl_->Metrics().relic_effects};

    for (const auto &enemy : impl_->actors->enemies)
    {
        if (enemy.dead) continue;
        std::uint8_t statuses{};
        if (enemy.status.bleed_count != 0)
            statuses |= static_cast<std::uint8_t>(StatusFlag::Bleed);
        if (enemy.status.burn)
            statuses |= static_cast<std::uint8_t>(StatusFlag::Burn);
        if (!enemy.status.slows.empty())
            statuses |= static_cast<std::uint8_t>(StatusFlag::Slow);
        if (enemy.marked_by_skill != SkillKind::Count &&
            enemy.mark_expires >= impl_->tick)
            statuses |= static_cast<std::uint8_t>(StatusFlag::Mark);
        model.AddEnemy({enemy.id,
                        enemy.kind,
                        enemy.boss,
                        enemy.position,
                        enemy.velocity,
                        enemy.locked_aim,
                        enemy.warning_extent,
                        enemy.spawned_tick,
                        enemy.attacking ? enemy.attack_resolve - enemy.warning_ticks : Tick{},
                        enemy.attack_resolve,
                        enemy.boss_action_started,
                        enemy.boss_action_until,
                        enemy.boss_action_recoil,
                        enemy.health,
                        enemy.max_health,
                        statuses,
                        enemy.attacking,
                        false});
    }
    for (const auto &projectile : impl_->actors->projectiles)
    {
        if (projectile.dead) continue;
        model.AddProjectile({projectile.id, projectile.player_owned,
                             projectile.position, projectile.velocity,
                             projectile.spawned_tick, projectile.skill, false,
                             projectile.radius, projectile.charge_ratio,
                             projectile.origin, projectile.source_upgrade});
    }
    for (const auto &area : impl_->actors->areas)
    {
        if (area.dead) continue;
        model.AddArea({area.id,
                       static_cast<AreaViewKind>(area.kind),
                       area.position,
                       area.direction,
                       area.radius,
                       area.half_length,
                       area.active_tick,
                       area.expires,
                       area.skill,
                       area.origin,
                       area.cast_id,
                       area.source_upgrade,
                       area.ring_inner_radius,
                       area.ring_outer_radius,
                       area.safe_gap_degrees,
                       area.safe_gap_count,
                       area.applies_burn,
                       area.slow_reduction > 0.0f,
                       false});
    }
    for (const auto &pickup : impl_->actors->pickups)
    {
        if (!pickup.dead)
            model.AddPickup({pickup.id, pickup.kind, pickup.position, false});
    }
    for (const auto &action : impl_->combat_state->boss_actions)
    {
        model.AddBossAction({static_cast<BossActionViewKind>(action.kind),
                             action.boss_id, action.animation_started, action.due,
                             action.position,
                             action.direction, action.distance, action.arc_degrees,
                             action.angle_offset, action.radius, action.cast_id});
    }
}

std::span<const DomainSignal> GameSimulation::PendingDomainSignals() const noexcept
{
    return impl_->combat_state->domain_signals;
}

void GameSimulation::ClearDomainSignals() noexcept
{
    impl_->combat_state->domain_signals.clear();
}

Result GameSimulation::Shutdown()
{
    if (!impl_->initialized)
    {
        return Result::Success();
    }
    impl_->initialized = false;
    return Result::Success();
}

} // namespace hs
