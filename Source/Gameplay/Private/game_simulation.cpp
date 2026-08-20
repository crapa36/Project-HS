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
                                  const SimulationRules &data)
{
    if (impl_->initialized)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_gameplay",
                               "GameSimulation already initialized.");
    }
    impl_->config = config;
    impl_->data = data;
    impl_->phase = config.start_in_main_menu ? SessionPhase::MainMenu : SessionPhase::Playing;
    impl_->player.health = impl_->player.max_health = data.player_health;
    impl_->player.attack = data.player_attack;
    impl_->player.attack_speed = data.player_attack_speed;
    impl_->player.move_speed = data.player_move_speed;
    impl_->player.magnet_radius = data.player_magnet_radius;
    impl_->player.skill_levels[0] = 1;
    impl_->active_rules.Rebuild(impl_->player.relic_mask);
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
    impl_->input = input;
    for (const auto &action : input.ui_actions) ApplyUiAction(action);
    impl_->ProcessInput();
    if (impl_->phase != SessionPhase::Playing)
    {
        impl_->player.buffered_skill = SkillKind::Count;
        impl_->player.buffered_skill_expires = 0;
        impl_->checksum = impl_->CalculateChecksum();
        return {impl_->tick, impl_->checksum, impl_->phase};
    }
    (void)fixed_delta;
    ++impl_->tick;
    impl_->RunPipeline();
    return {impl_->tick, impl_->checksum, impl_->phase};
}

GameplayChecksum GameSimulation::ComputeChecksum() const
{
    return impl_->checksum;
}

const SimulationRules &GameSimulation::Rules() const noexcept
{
    return impl_->data;
}

SessionProbe GameSimulation::GetSessionView() const noexcept
{
    SessionProbe probe;
    probe.tick = impl_->tick;
    probe.growth_ticks = impl_->growth_ticks;
    probe.boss_fight_ticks = impl_->boss_fight_ticks;
    probe.phase = impl_->phase;
    probe.level = impl_->player.level;
    probe.experience = impl_->player.experience;
    probe.experience_to_next = ExperienceForLevel(impl_->player.level);
    probe.health = impl_->player.health;
    probe.max_health = impl_->player.max_health;
    probe.player_position = impl_->player.position;
    probe.aim_direction = impl_->player.aim;
    probe.facing_direction = impl_->player.facing;
    probe.normal_enemy_count = impl_->NormalEnemyCount();
    probe.boss_count = impl_->BossCount();
    probe.boss_warning_count = static_cast<std::uint32_t>(impl_->boss_actions.size());
    probe.boss_dashing_count = static_cast<std::uint32_t>(
        std::ranges::count_if(impl_->enemies, [&](const EnemyActor &enemy) {
            return enemy.boss.has_value() && !enemy.dead && impl_->tick < enemy.dash_until;
        }));
    probe.enemy_area_count = static_cast<std::uint32_t>(
        std::ranges::count_if(impl_->areas, [](const AreaActor &area) {
            return !area.dead && area.kind == AreaKind::EnemyDamage;
        }));
    probe.player_projectile_count = impl_->ProjectileCount(true);
    probe.enemy_projectile_count = impl_->ProjectileCount(false);
    probe.pickup_count = static_cast<std::uint32_t>(impl_->pickups.size());
    probe.kills = impl_->kills;
    probe.damage_dealt = impl_->damage_dealt;
    probe.damage_by_skill = impl_->damage_by_skill;
    probe.damage_taken = impl_->damage_taken;
    probe.healing = impl_->healing;
    probe.level_rerolls_remaining = impl_->player.level_rerolls;
    probe.relic_rerolls_remaining = impl_->player.relic_rerolls;
    probe.pending_stat_points = impl_->player.pending_stat_points;
    probe.active_skill_count = static_cast<std::uint8_t>(std::ranges::count_if(
        impl_->player.loadout, [](SkillKind skill) { return skill != SkillKind::Count; }));
    probe.skill_loadout = impl_->player.loadout;
    probe.skill_levels = impl_->player.skill_levels;
    probe.upgrade_masks = impl_->player.upgrades;
    for (std::size_t index = 0; index < probe.cooldown_ticks.size(); ++index)
    {
        probe.cooldown_ticks[index] = static_cast<std::uint32_t>(
            std::min<Tick>(impl_->player.cooldowns[index],
                           std::numeric_limits<std::uint32_t>::max()));
    }
    probe.stat_points = impl_->player.stats;
    probe.relic_mask = impl_->player.relic_mask;
    probe.cards = impl_->cards;
    probe.card_count = impl_->card_count;
    probe.final_boss_spawned = impl_->final_boss_spawned;
    probe.final_boss_phase_two = std::ranges::any_of(impl_->enemies, [](const EnemyActor &enemy) {
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
        impl_->player.health -= static_cast<std::int32_t>(command.value);
        break;
    case DebugCommandKind::HealPlayer:
        impl_->player.health = std::min(
            impl_->player.max_health,
            impl_->player.health + static_cast<std::int32_t>(command.value));
        break;
    case DebugCommandKind::DamageFinalBoss:
        for (auto &enemy : impl_->enemies)
        {
            if (enemy.boss == BossKind::Final)
            {
                enemy.health -= static_cast<std::int32_t>(command.value);
                break;
            }
        }
        break;
    case DebugCommandKind::GrantExperience:
        impl_->player.experience += static_cast<std::uint32_t>(command.value);
        break;
    case DebugCommandKind::GrantSkill:
    {
        const auto skill = static_cast<SkillKind>(command.value);
        if (skill <= SkillKind::BasicAttack || skill >= SkillKind::Count)
        {
            return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay",
                                   "Debug skill is invalid.");
        }
        if (impl_->player.skill_levels[static_cast<std::size_t>(skill)] == 0)
        {
            impl_->player.skill_levels[static_cast<std::size_t>(skill)] = 1;
            const auto slot = std::ranges::find(impl_->player.loadout, SkillKind::Count);
            if (slot != impl_->player.loadout.end()) *slot = skill;
        }
        break;
    }
    case DebugCommandKind::GrantUpgrade:
        if (command.value >= kCombatSkillCount || command.secondary >= kUpgradeCount)
        {
            return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay",
                                   "Debug upgrade is invalid.");
        }
        impl_->player.upgrades[command.value] |= 1u << command.secondary;
        impl_->player.skill_levels[command.value] = static_cast<std::uint8_t>(
            1 + std::popcount(impl_->player.upgrades[command.value]));
        break;
    case DebugCommandKind::GrantRelic:
        if (command.value >= kRelicCount)
        {
            return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay",
                                   "Debug relic is invalid.");
        }
        impl_->player.relic_mask |= 1u << command.value;
        impl_->active_rules.Rebuild(impl_->player.relic_mask);
        break;
    case DebugCommandKind::SpawnEnemy:
        impl_->SpawnEnemy(static_cast<EnemyKind>(command.value % 3), command.position);
        break;
    case DebugCommandKind::SpawnBoss:
        if (command.value >= 3 ||
            !impl_->SpawnBoss(static_cast<BossKind>(command.value)))
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_gameplay",
                                   "Debug boss could not be spawned.");
        }
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
        if (command.value >= kStatCount || command.secondary > 10)
        {
            return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay",
                                   "Debug stat or level is invalid.");
        }
        impl_->player.stats[command.value] =
            static_cast<std::uint8_t>(command.secondary);
        if (command.value == static_cast<std::uint64_t>(StatKind::MaxHealth))
        {
            impl_->player.max_health = RoundDamage(
                100.0f * (1.0f + 0.08f * command.secondary));
            impl_->player.health = impl_->player.max_health;
        }
        break;
    case DebugCommandKind::Reroll:
    {
        auto &rerolls = impl_->phase == SessionPhase::RelicSelection
                            ? impl_->player.relic_rerolls
                            : impl_->player.level_rerolls;
        auto &sequence = impl_->phase == SessionPhase::RelicSelection
                             ? impl_->relic_reroll_sequence
                             : impl_->level_reroll_sequence;
        if (rerolls == 0 || impl_->card_count == 0)
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_gameplay",
                                   "No reroll is available.");
        }
        --rerolls;
        ++sequence;
        if (impl_->phase == SessionPhase::RelicSelection) impl_->GenerateRelicCards(true);
        else impl_->GenerateLevelCards(true);
        break;
    }
    case DebugCommandKind::TogglePause:
        impl_->phase = impl_->phase == SessionPhase::Paused ? SessionPhase::Playing
                                                            : SessionPhase::Paused;
        break;
    }
    impl_->checksum = impl_->CalculateChecksum();
    return Result::Success();
}

Result GameSimulation::ApplySimulationRules(const SimulationRules &data)
{
    if (!impl_->initialized || impl_->phase != SessionPhase::MainMenu)
        return Result::Failure(ErrorCode::InvalidState, "hs_gameplay",
                               "Gameplay data Hot Reload requires the main menu.");
    impl_->data = data;
    return Result::Success();
}

SimulationObservation GameSimulation::Probe() const noexcept
{
    SimulationObservation observation;
    static_cast<SessionProbe &>(observation) = GetSessionView();
    observation.balance = impl_->balance;
    return observation;
}

SimulationDiagnostics GameSimulation::GetDiagnostics() const noexcept
{
    return {impl_->balance};
}

void GameSimulation::ApplyUiAction(const UiAction &action)
{
    switch (action.kind)
    {
    case UiActionKind::StartSession:
        if (impl_->phase == SessionPhase::MainMenu) impl_->StartSession();
        break;
    case UiActionKind::ReturnToMainMenu:
        impl_->phase = SessionPhase::MainMenu;
        break;
    case UiActionKind::Quit: impl_->phase = SessionPhase::QuitRequested; break;
    case UiActionKind::Reroll:
        if (impl_->selection_input_guard_frames > 0 ||
            impl_->selection_waiting_for_release) break;
        if (impl_->phase == SessionPhase::RelicSelection && impl_->player.relic_rerolls > 0)
        {
            --impl_->player.relic_rerolls;
            ++impl_->relic_reroll_sequence;
            impl_->GenerateRelicCards(true);
        }
        else if (impl_->phase == SessionPhase::CardSelection &&
                 impl_->player.level_rerolls > 0)
        {
            --impl_->player.level_rerolls;
            ++impl_->level_reroll_sequence;
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
        impl_->phase = SessionPhase::Playing;
        break;
    case UiActionKind::SwapLoadoutSlots:
        if (action.value < impl_->player.loadout.size() &&
            action.secondary < impl_->player.loadout.size())
            std::swap(impl_->player.loadout[action.value],
                      impl_->player.loadout[action.secondary]);
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
    model.session = GetSessionView();
    model.player = {impl_->player.position,
                    impl_->player.aim,
                    impl_->player.facing,
                    impl_->player.locomotion_blend,
                    impl_->player.charging,
                    impl_->player.charging_skill,
                    impl_->player.charge_start,
                    impl_->player.basic_attack_animation_start,
                    impl_->player.basic_attack_animation_until,
                    impl_->player.active_cast_tick,
                    impl_->player.active_animation_start,
                    impl_->player.active_animation_until,
                    impl_->player.retreat_until,
                    impl_->player.loadout,
                    impl_->player.upgrades};
    model.effective_attack = impl_->EffectiveAttack();
    model.effective_attack_speed = impl_->EffectiveAttackSpeed();
    model.effective_move_speed = impl_->EffectiveMoveSpeed();
    model.effective_magnet_radius = impl_->EffectiveMagnetRadius();
    model.arena_half_extent = impl_->data.arena_half_extent;
    for (std::size_t index = 0; index < model.skills.size(); ++index)
    {
        const auto skill = static_cast<SkillKind>(index);
        const auto &definition = impl_->data.skills[index];
        model.skills[index] = {
            index == 0 ? Tick{} : impl_->CooldownTicks(skill),
            RoundDamage(impl_->EffectiveAttack() * definition.damage_coefficient),
            definition.range, definition.area_radius, definition.duration_ticks,
            definition.projectile_count, definition.pierce_count};
    }
    for (std::size_t index = 0; index < model.waves.size(); ++index)
        model.waves[index] = {
            static_cast<Tick>(impl_->data.waves[index].minute) * Seconds(60.0f),
            impl_->data.waves[index].duration_ticks};
    if (impl_->player.charging &&
        impl_->player.charging_skill == SkillKind::ChargedShot)
    {
        const auto mask = impl_->player.upgrades[
            static_cast<std::size_t>(SkillKind::ChargedShot)];
        const auto maximum_ticks = HasUpgrade(mask, 1) ? Seconds(1.4f) : Seconds(1.0f);
        auto elapsed = std::min(impl_->tick - impl_->player.charge_start, maximum_ticks);
        if (HasUpgrade(mask, 2))
            elapsed = std::min(maximum_ticks, static_cast<Tick>(elapsed / 0.65f));
        model.charge_range = std::lerp(
            4.2f, model.skills[static_cast<std::size_t>(SkillKind::ChargedShot)]
                      .effective_range,
            static_cast<float>(elapsed) / static_cast<float>(maximum_ticks));
    }
    model.summary = {impl_->balance.direct_damage,
                     impl_->balance.derived_damage,
                     impl_->balance.damage_over_time,
                     impl_->balance.upgrade_damage};

    for (const auto &enemy : impl_->enemies)
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
                        enemy.health,
                        enemy.max_health,
                        statuses,
                        enemy.attacking,
                        false});
    }
    for (const auto &projectile : impl_->projectiles)
    {
        if (projectile.dead) continue;
        model.AddProjectile({projectile.id, projectile.player_owned,
                             projectile.position, projectile.velocity,
                             projectile.spawned_tick, projectile.skill, false});
    }
    for (const auto &area : impl_->areas)
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
                       false});
    }
    for (const auto &pickup : impl_->pickups)
    {
        if (!pickup.dead)
            model.AddPickup({pickup.id, pickup.kind, pickup.position, false});
    }
    for (const auto &action : impl_->boss_actions)
    {
        model.AddBossAction({static_cast<BossActionViewKind>(action.kind),
                             action.boss_id, action.position,
                             action.direction, action.distance, action.arc_degrees,
                             action.angle_offset, action.radius, action.cast_id});
    }
}

std::span<const DomainSignal> GameSimulation::PendingDomainSignals() const noexcept
{
    return impl_->domain_signals;
}

void GameSimulation::ClearDomainSignals() noexcept
{
    impl_->domain_signals.clear();
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
