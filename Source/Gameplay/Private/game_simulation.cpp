#include "simulation_world.hpp"

namespace hs
{

using namespace gameplay_detail;


EnemyView GameSimulation::SimulationWorld::EnemyReadView(ConstEnemyRow enemy) const
{
        std::uint8_t statuses{};
        if (enemy.status.bleed_count != 0)
            statuses |= static_cast<std::uint8_t>(StatusFlag::Bleed);
        if (enemy.status.burn)
            statuses |= static_cast<std::uint8_t>(StatusFlag::Burn);
        if (!enemy.status.slows.empty())
            statuses |= static_cast<std::uint8_t>(StatusFlag::Slow);
        if (enemy.cold.marked_by_skill != SkillKind::Count &&
            enemy.cold.mark_expires >= tick)
            statuses |= static_cast<std::uint8_t>(StatusFlag::Mark);
        std::array<StatusEpisodeView, 4> episodes{};
        for(std::size_t i=0;i<episodes.size();++i) {
            episodes[i].started=enemy.cold.status_visual_episodes[i].started;
            episodes[i].generation=enemy.cold.status_visual_episodes[i].generation;
        }
        for(std::size_t i=0;i<enemy.status.bleed_count;++i)
            if(enemy.status.bleeds[i].expires>tick) episodes[0].expires=std::max(episodes[0].expires,enemy.status.bleeds[i].expires);
        if(enemy.status.burn&&enemy.status.burn->expires>tick) episodes[1].expires=enemy.status.burn->expires;
        for(const auto&slow:enemy.status.slows) if(slow.expires>tick) episodes[2].expires=std::max(episodes[2].expires,slow.expires);
        if(enemy.cold.marked_by_skill!=SkillKind::Count&&enemy.cold.mark_expires>=tick) episodes[3].expires=enemy.cold.mark_expires+1;
        return {enemy.id,
                        enemy.life.kind,
                        enemy.life.boss,
                        enemy.motion.position,
                        enemy.motion.velocity,
                        enemy.action.locked_aim,
                        enemy.action.warning_extent,
                        enemy.life.spawned_tick,
                        enemy.action.attacking ? enemy.action.attack_resolve - enemy.action.warning_ticks : Tick{},
                        enemy.action.attack_resolve,
                        enemy.action.boss_action_started,
                        enemy.action.boss_action_until,
                        enemy.action.boss_action_recoil,
                        enemy.life.health,
                        enemy.life.max_health,
                        statuses,
                        enemy.action.attacking,
                        false,
                        enemy.action.warning_sequence,
                        rules.enemies[static_cast<std::size_t>(enemy.life.kind)].ranged_projectile_radius,
                        enemy.life.boss ? rules.boss_common.collision_radius : rules.enemies[static_cast<std::size_t>(enemy.life.kind)].collision_radius,
                        episodes,
                        enemy.action.final_phase,
                        enemy.action.phase2_started,
                        enemy.action.invulnerable_until,
                        enemy.action.dash_started,
                        enemy.action.dash_until,
                        enemy.action.dash_origin};
}

BossActionView GameSimulation::SimulationWorld::BossActionReadView(const BossAction &action) const
{
        return {static_cast<BossActionViewKind>(action.kind),
                             action.boss_id, action.animation_started, action.due,
                             action.position,
                             action.direction, action.distance, action.arc_degrees,
                             action.angle_offset, action.radius, action.cast_id,
                             // Keep this aligned with enemy projectile spawn.
                              rules.boss_common.projectile_range,
                              action.safe_gap_count, action.safe_gap_degrees,
                              action.half_width,
                              action.warning_sequence, action.warning_started,
                              action.kind == BossActionKind::Dash
                                  ? rules.boss_common.collision_radius +
                                        rules.stats.player_collision_radius
                                  : 0.0f};
}

GameSimulation::GameSimulation() : impl_(std::make_unique<SimulationWorld>())
{
}

GameSimulation::~GameSimulation()
{
    assert(impl_->observation.active_borrows == 0 && "Read scope must end before its simulation owner.");
}

void GameSimulation::SimulationWorld::RequireOwnerThread() const
{
    const auto current = std::this_thread::get_id();
    if (!observation.owner_thread) observation.owner_thread = current;
    if (*observation.owner_thread != current)
        throw std::logic_error("Gameplay read/mutation must remain on its owner thread.");
}

GameSimulation::SimulationWorld::MutationScope::MutationScope(SimulationWorld &owner) : world(owner)
{
    world.RequireOwnerThread();
    if (world.observation.active_borrows != 0)
        throw std::logic_error("Gameplay mutation attempted while TickReadView is borrowed.");
    world.observation.small_facts_dirty = true;
    world.observation.checksum_dirty = true;
    ++world.observation.mutation_depth;
}
GameSimulation::SimulationWorld::MutationScope::~MutationScope() noexcept
{
    assert(world.observation.mutation_depth != 0);
    --world.observation.mutation_depth;
}
GameSimulation::ScopedTickReadView::ScopedTickReadView(SimulationWorld &owner, TickReadView view)
    : owner_(&owner), view_(std::move(view)) { ++owner_->observation.active_borrows; }
GameSimulation::ScopedTickReadView::ScopedTickReadView(ScopedTickReadView &&other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), view_(other.view_) {}
GameSimulation::ScopedTickReadView::~ScopedTickReadView()
{
    if (owner_) { assert(owner_->observation.active_borrows != 0); --owner_->observation.active_borrows; }
}
const TickReadView &GameSimulation::ScopedTickReadView::View() const &
{
    if (!owner_) throw std::logic_error("TickReadView scope was moved from.");
    owner_->RequireOwnerThread();
    return view_;
}
GameSimulation::ScopedTickReadView GameSimulation::BorrowTickReadView() const
{
    impl_->RequireOwnerThread();
    if (impl_->observation.mutation_depth != 0)
        throw std::logic_error("TickReadView cannot be borrowed during mutation.");
    if (impl_->initialized) impl_->UpdateChecksum();
    if (impl_->observation.small_facts_dirty) impl_->RefreshSmallReadFacts();
    return {*impl_, impl_->MakeTickReadView()};
}

void GameSimulation::SimulationWorld::RebuildNavigation()
{
    const auto obstacles = ArenaObstacles();
    navigation->player_navigation.Rebuild(rules.arena_boundary, obstacles,
                                          rules.stats.player_collision_radius);
    navigation->enemy_navigation.Rebuild(rules.arena_boundary, obstacles,
                                         rules.enemies.front().collision_radius);
    navigation->boss_navigation.Rebuild(rules.arena_boundary, obstacles,
                                        rules.boss_common.collision_radius);
    navigation->player_navigation_target = {-1, -1};
    navigation->enemy_navigation_target = {-1, -1};
}

Result GameSimulation::Initialize(const SimulationConfig &config)
{
    return Initialize(config, SimulationRules::Defaults());
}

Result GameSimulation::Initialize(const SimulationConfig &config,
                                  const SimulationRules &rules)
{
    SimulationWorld::MutationScope mutation(*impl_);
    if (impl_->initialized)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_gameplay",
                               "GameSimulation already initialized.");
    }
    if (rules.relics.movement_echo.position_history_age_ticks >=
        impl_->actors->player_position_history.max_size())
        return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay",
                               "Movement echo history age exceeds storage capacity.");
    impl_->config = config;
    impl_->timings = {};
    if (config.capture_timings)
        for (std::size_t sample = 0; sample < 64; ++sample)
        {
            SimulationWorld::TimingScope empty_scope(&impl_->timings.empty_scope_calibration);
        }
    impl_->rules = rules;
    impl_->RebuildNavigation();
    impl_->session_phase = config.start_in_main_menu ? SessionPhase::MainMenu : SessionPhase::Playing;
    impl_->InitializePlayerState();
    impl_->InitializeContentReadFacts();
    impl_->initialized = true;
    impl_->UpdateChecksum();
    return Result::Success();
}

TickResult GameSimulation::TickFixed(const InputFrame &input,
                                     std::chrono::nanoseconds fixed_delta)
{
    if (!impl_->initialized)
    {
        return {};
    }
    SimulationWorld::MutationScope mutation(*impl_);
    impl_->ResetTimings();
    auto tick_timing = impl_->Measure(SimulationTimingId::Tick);
    auto input_timing = impl_->Measure(SimulationTimingId::Input);
    impl_->current_input = input;
    for (const auto &action : input.ui_actions) impl_->ApplyUiAction(action);
    impl_->ProcessInput();
    input_timing.Finish();
    if (impl_->session_phase != SessionPhase::Playing)
    {
        impl_->actors->player.buffered_skill = SkillKind::Count;
        impl_->actors->player.buffered_skill_expires = 0;
        impl_->actors->player.buffered_skill_slot = 0xFF;
        impl_->UpdateChecksum();
        impl_->timings.tick = impl_->tick;
        impl_->timings.completed = impl_->config.capture_timings;
        return {impl_->tick, impl_->checksum, impl_->session_phase};
    }
    (void)fixed_delta;
    NextSimulationSequence(impl_->tick);
    impl_->timings.tick = impl_->tick;
    impl_->PruneVisualLinks();
    impl_->RunPipeline();
    impl_->timings.tick = impl_->tick;
    impl_->timings.completed = impl_->config.capture_timings;
    return {impl_->tick, impl_->checksum, impl_->session_phase};
}

GameplayChecksum GameSimulation::ComputeChecksum() const
{
    impl_->RequireOwnerThread();
    if (impl_->observation.mutation_depth != 0)
        throw std::logic_error("Checksum cannot observe an active mutation.");
    if (!impl_->initialized) return impl_->checksum;
    impl_->UpdateChecksum();
    return impl_->config.capture_checksums
        ? impl_->checksum : impl_->CalculateChecksum();
}

const SimulationRules &GameSimulation::Rules() const noexcept
{
    return impl_->rules;
}

SessionProbe GameSimulation::GetSessionProbe() const
{
    auto read = BorrowTickReadView();
    return read.View().session;
}

SessionProbe GameSimulation::SimulationWorld::BuildSessionProbe() const
{
    SessionProbe probe;
    probe.tick = tick;
    probe.growth_ticks = growth_ticks;
    probe.boss_fight_ticks = boss_fight_ticks;
    probe.phase = session_phase;
    probe.level = actors->player.level;
    probe.experience = actors->player.experience;
    probe.experience_to_next = ExperienceForLevel(rules, actors->player.level);
    probe.health = actors->player.health;
    probe.max_health = actors->player.max_health;
    probe.player_position = actors->player.position;
    probe.aim_direction = actors->player.aim;
    probe.facing_direction = actors->player.facing;
    probe.boss_warning_count = static_cast<std::uint32_t>(combat_state->boss_actions.size());
    probe.pickup_count = static_cast<std::uint32_t>(actors->pickups.size());
    probe.kills = telemetry->kills;
    probe.damage_dealt = telemetry->damage_dealt;
    probe.damage_by_skill = telemetry->damage_by_skill;
    probe.damage_taken = telemetry->damage_taken;
    probe.healing = telemetry->healing;
    probe.level_rerolls_remaining = actors->player.level_rerolls;
    probe.relic_rerolls_remaining = actors->player.relic_rerolls;
    probe.pending_stat_points = actors->player.pending_stat_points;
    probe.active_skill_count = static_cast<std::uint8_t>(std::ranges::count_if(
        actors->player.loadout, [](SkillKind skill) { return skill != SkillKind::Count; }));
    probe.skill_loadout = actors->player.loadout;
    probe.skill_levels = actors->player.skill_levels;
    probe.upgrade_masks = actors->player.upgrades;
    for (std::size_t index = 0; index < probe.cooldown_ticks.size(); ++index)
    {
        probe.cooldown_ticks[index] = static_cast<std::uint32_t>(
            std::min<Tick>(actors->player.cooldowns[index],
                           std::numeric_limits<std::uint32_t>::max()));
    }
    probe.stat_points = actors->player.stats;
    probe.relic_mask = actors->player.relic_mask;
    probe.cards = progression->cards;
    probe.card_count = progression->card_count;
    probe.final_boss_spawned = final_boss_spawned;
    return probe;
}

Result GameSimulation::ApplyDebugCommand(const DebugCommand &command)
{
    SimulationWorld::MutationScope mutation(*impl_);
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
        for (auto enemy : impl_->actors->enemies)
        {
            if (enemy.life.boss == BossKind::Final)
            {
                enemy.life.health -= static_cast<std::int32_t>(command.value);
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
    impl_->UpdateChecksum();
    return Result::Success();
}

Result GameSimulation::ApplySimulationRules(const SimulationRules &rules)
{
    SimulationWorld::MutationScope mutation(*impl_);
    if (!impl_->initialized || impl_->session_phase != SessionPhase::MainMenu)
        return Result::Failure(ErrorCode::InvalidState, "hs_gameplay",
                               "Gameplay data Hot Reload requires the main menu.");
    if (rules.relics.movement_echo.position_history_age_ticks >=
        impl_->actors->player_position_history.max_size())
        return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay",
                               "Movement echo history age exceeds storage capacity.");
    impl_->rules = rules;
    impl_->RebuildNavigation();
    impl_->InitializeContentReadFacts();
    impl_->progression->pickup_work.Invalidate();
    return Result::Success();
}

SimulationObservation GameSimulation::GetObservation() const
{
    SimulationObservation observation;
    static_cast<SessionProbe &>(observation) = GetSessionProbe();
    observation.balance = impl_->Metrics();
    return observation;
}

SimulationDiagnostics GameSimulation::GetDiagnostics() const
{
    auto read = BorrowTickReadView();
    return {impl_->Metrics(), impl_->timings};
}

SimulationTimingDiagnostics GameSimulation::GetTimingDiagnostics() const
{
    impl_->RequireOwnerThread();
    return impl_->timings;
}

void GameSimulation::ApplyUiAction(const UiAction &action)
{
    SimulationWorld::MutationScope mutation(*impl_);
    impl_->ApplyUiAction(action);
    impl_->UpdateChecksum();
}

void GameSimulation::SimulationWorld::ApplyUiAction(const UiAction &action)
{
    switch (action.kind)
    {
    case UiActionKind::StartSession:
        if (session_phase == SessionPhase::MainMenu) StartSession();
        break;
    case UiActionKind::ReturnToMainMenu:
        session_phase = SessionPhase::MainMenu;
        break;
    case UiActionKind::Quit: session_phase = SessionPhase::QuitRequested; break;
    case UiActionKind::Reroll:
        if (selection_input_guard_frames > 0 ||
            selection_waiting_for_release) break;
        if (session_phase == SessionPhase::RelicSelection && actors->player.relic_rerolls > 0)
        {
            --actors->player.relic_rerolls;
            ++progression->relic_reroll_sequence;
            GenerateRelicCards(true);
        }
        else if (session_phase == SessionPhase::CardSelection &&
                 actors->player.level_rerolls > 0)
        {
            --actors->player.level_rerolls;
            ++progression->level_reroll_sequence;
            GenerateLevelCards(true);
        }
        break;
    case UiActionKind::SelectCard:
        if (selection_input_guard_frames == 0 &&
            !selection_waiting_for_release) (void)SelectCard(action.value);
        break;
    case UiActionKind::AssignStat:
        if (selection_input_guard_frames == 0 &&
            !selection_waiting_for_release && action.value < kStatCount)
            AssignStat(static_cast<StatKind>(action.value));
        break;
    case UiActionKind::Resume:
        if (session_phase == SessionPhase::Paused)
            session_phase = SessionPhase::Playing;
        break;
    case UiActionKind::SwapLoadoutSlots:
        if (action.value < actors->player.loadout.size() &&
            action.secondary < actors->player.loadout.size())
            std::swap(actors->player.loadout[action.value],
                      actors->player.loadout[action.secondary]);
        break;
    }
}

void GameSimulation::SimulationWorld::InitializeContentReadFacts()
{
    for (std::size_t index = 0; index < read_facts.skills.size(); ++index)
    {
        const auto &definition = rules.skills[index];
        read_facts.skills[index] = {0, 0, definition.range, definition.area_radius,
            definition.duration_ticks, definition.projectile_count, definition.pierce_count};
    }
    for (std::size_t index = 0; index < read_facts.waves.size(); ++index)
        read_facts.waves[index] = {rules.waves[index].start_tick, rules.waves[index].duration_ticks};
}

void GameSimulation::SimulationWorld::RefreshSmallReadFacts() const
{
    auto timing = Measure(SimulationTimingId::SmallFacts);
    assert(observation.active_borrows == 0);
    auto &facts = read_facts;
    facts.session = BuildSessionProbe();
    facts.charge_range = facts.charge_radius = 0.0f;
    facts.player = {actors->player.position,
                    actors->player.aim,
                    actors->player.facing,
                    actors->player.locomotion_blend,
                    actors->player.charging,
                    actors->player.charging_skill,
                    actors->player.charge_start,
                    Tick{},
                    actors->player.basic_attack_animation_start,
                    actors->player.basic_attack_animation_until,
                    actors->player.basic_attack_release_tick,
                    actors->player.basic_attack_cast_id,
                    actors->player.active_basic_empower_until != 0
                        ? actors->player.active_basic_empower_until -
                              rules.upgrades.basic_attack.post_active_three_arrow
                                  .activation_window_ticks
                        : Tick{},
                    actors->player.active_basic_empower_until,
                    actors->player.active_cast_tick,
                    actors->player.active_animation_start,
                    actors->player.active_animation_until,
                    actors->player.retreat_until,
                    actors->player.loadout,
                     actors->player.upgrades,
                     actors->player.health,
                     actors->player.max_health,
                     actors->player.revive_invulnerable_until,
                     actors->player.revive_invulnerable_started,
                     actors->player.forced_move_skill,
                     actors->player.movement_stopped_tick,
                     actors->player.locomotion_moving};
    facts.effective_attack = EffectiveAttack();
    facts.effective_attack_speed = EffectiveAttackSpeed();
    facts.effective_move_speed = EffectiveMoveSpeed();
    facts.effective_magnet_radius = EffectiveMagnetRadius();
    facts.charge_normal_ready_tick = {};
    facts.charge_ratio = {};
    for (std::size_t index = 0; index < facts.skills.size(); ++index)
    {
        const auto skill = static_cast<SkillKind>(index);
        facts.skills[index].effective_cooldown = index == 0 ? Tick{} : EffectiveCooldownTicks(skill);
        facts.skills[index].displayed_damage = RoundDamage(EffectiveAttack() * rules.skills[index].damage_coefficient);
    }
    if (actors->player.charging &&
        actors->player.charging_skill == SkillKind::ChargedShot)
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
        const auto ready_ticks = HasUpgrade(mask, 2)
                                     ? static_cast<Tick>(std::ceil(
                                           maximum_ticks * upgrades.faster_charge
                                                               .charge_time_multiplier))
                                     : maximum_ticks;
        const auto normal_ready_ticks = HasUpgrade(mask, 2)
                                            ? static_cast<Tick>(std::ceil(
                                                  definition.maximum_charge_time_ticks *
                                                  upgrades.faster_charge.charge_time_multiplier))
                                            : definition.maximum_charge_time_ticks;
        facts.charge_normal_ready_tick =
            actors->player.charge_start + normal_ready_ticks;
        facts.player.charge_full_ready_tick =
            actors->player.charge_start + ready_ticks;
        auto elapsed = std::min(tick - actors->player.charge_start, maximum_ticks);
        if (HasUpgrade(mask, 2))
            elapsed = std::min(
                maximum_ticks,
                static_cast<Tick>(elapsed /
                                  upgrades.faster_charge.charge_time_multiplier));
        const auto progress = static_cast<float>(elapsed) /
                              static_cast<float>(maximum_ticks);
        facts.charge_ratio = progress;
        facts.charge_range = std::lerp(definition.minimum_range,
                                       definition.maximum_range, progress);
        facts.charge_radius = std::lerp(definition.minimum_collision_radius,
                                        definition.maximum_collision_radius,
                                        progress);
    }

    const auto count = [](const auto &rows, auto eligible) { return static_cast<std::size_t>(std::ranges::count_if(rows, eligible)); };
    auto &probe = facts.session;
    facts.enemies = 0;
    for (std::size_t row = 0; row < actors->enemies.size(); ++row)
    {
        const auto &life = actors->enemies.life[row];
        const auto &action = actors->enemies.action[row];
        if (life.boss == BossKind::Final && action.final_phase == 2) probe.final_boss_phase_two = true;
        if (life.dead) continue;
        ++facts.enemies;
        if (life.boss) ++probe.boss_count; else ++probe.normal_enemy_count;
        if (life.boss && tick < action.dash_until) ++probe.boss_dashing_count;
    }
    facts.projectiles = 0;
    for (const auto &combat : actors->projectiles.combat)
    {
        if (combat.dead) continue;
        ++facts.projectiles;
        if (combat.player_owned) ++probe.player_projectile_count; else ++probe.enemy_projectile_count;
    }
    facts.areas = 0;
    for (const auto &area : actors->areas)
    {
        if (area.dead) continue;
        ++facts.areas;
        if (area.kind == AreaKind::EnemyDamage) ++probe.enemy_area_count;
    }
    facts.pickups = progression->pickup_work.NeedsCompaction()
        ? count(actors->pickups, [](const auto &p) { return !p.dead; })
        : actors->pickups.size();
    facts.visual_links = count(combat_state->visual_links, [&](const auto &v) { return v.started <= tick && tick < v.expires; });
    facts.mini_bombs = count(combat_state->scheduled_actions, [&](const auto &a) { return a.kind == ScheduledKind::Explosion && a.skill == SkillKind::ExplosiveArrow && a.source_upgrade == 1 && a.visual_owner_id != 0 && tick < a.due; });
    facts.deliveries = count(combat_state->scheduled_actions, [&](const auto &a) { return a.kind == ScheduledKind::Area && a.skill == SkillKind::ArrowRain && a.origin == EffectOrigin::Derived && a.source_upgrade == 0 && a.visual_owner_id != 0 && a.visual_started <= tick && tick < a.due; });
    facts.spawn_warnings = count(progression->pending_enemy_spawns_delayed, [&](const auto &a) { return a.warning_sequence != 0 && a.due > tick; }) + count(progression->pending_boss_spawns, [&](const auto &a) { return a.warning_sequence != 0 && a.due > tick; });
    observation.small_facts_dirty = false;
}

TickReadView GameSimulation::SimulationWorld::MakeTickReadView() const
{
    const auto &f = read_facts;
    EnemyReadColumns enemies(this, actors->enemies.size(), f.enemies,
        [](const void *owner, std::size_t row, EnemyView &v) {
            const auto &w = *static_cast<const SimulationWorld *>(owner);
            const auto e = std::as_const(w.actors->enemies)[row];
            if (e.life.dead) return false;
            v = w.EnemyReadView(e); return true;
        });
    ProjectileReadColumns projectiles(this, actors->projectiles.size(), f.projectiles,
        [](const void *owner, std::size_t row, ProjectileReadValue &v) {
            const auto &w = *static_cast<const SimulationWorld *>(owner);
            const auto p = std::as_const(w.actors->projectiles)[row];
            if (p.combat.dead) return false;
            v = {p.id, p.combat.player_owned, p.motion.position, p.motion.velocity,
                p.combat.spawned_tick, p.combat.skill, false, p.motion.radius, p.combat.charge_ratio,
                p.combat.origin, p.combat.source_upgrade, p.motion.previous_position,
                p.motion.remaining_range, p.combat.cast_id, p.combat.source_relic, p.combat.source_enemy,
                p.combat.returning, p.combat.homing, p.episodes.return_started_tick,
                p.episodes.return_start_position, p.combat.upgrade_mask, p.episodes.bleed_extend_ticks};
            return true;
        });
    TickRows<AreaView> areas(this, actors->areas.size(), f.areas,
        [](const void *owner, std::size_t row, AreaView &v) {
            const auto &w = *static_cast<const SimulationWorld *>(owner);
            const auto &a = w.actors->areas[row]; if (a.dead) return false;
            v = {a.id, static_cast<AreaViewKind>(a.kind), a.position, a.direction, a.radius,
                a.half_length, a.active_tick, a.expires, a.skill, a.origin, a.cast_id, a.source_upgrade,
                a.ring_inner_radius, a.ring_outer_radius, a.safe_gap_degrees, a.safe_gap_count,
                a.applies_burn, a.slow_reduction > 0.0f, false, a.ring_half_width, a.upgrade_mask}; return true;
        });
    TickRows<PickupView> pickups(this, actors->pickups.size(), f.pickups,
        [](const void *owner, std::size_t row, PickupView &v) {
            const auto &w = *static_cast<const SimulationWorld *>(owner);
            const auto &p = w.actors->pickups[row]; if (p.dead) return false;
            v = {p.id, p.kind, p.position, false, p.spawned_tick, p.visual_motion_started,
                p.visual_motion_updated == w.tick && p.visual_motion_started != 0,
                p.visual_motion_initial_distance}; return true;
        });
    TickRows<BossActionView> boss_actions(this, combat_state->boss_actions.size(), combat_state->boss_actions.size(),
        [](const void *owner, std::size_t row, BossActionView &v) {
            const auto &w = *static_cast<const SimulationWorld *>(owner);
            v = w.BossActionReadView(w.combat_state->boss_actions[row]); return true;
        });
    TickRows<VisualLinkView> links(this, combat_state->visual_links.size(), f.visual_links,
        [](const void *owner, std::size_t row, VisualLinkView &v) {
            const auto &w = *static_cast<const SimulationWorld *>(owner);
            const auto &link = w.combat_state->visual_links[row];
            if (!(link.started <= w.tick && w.tick < link.expires)) return false;
            v = link; return true;
        });
    TickRows<MiniBombView> bombs(this, combat_state->scheduled_actions.size(), f.mini_bombs,
        [](const void *owner, std::size_t row, MiniBombView &v) {
            const auto &w = *static_cast<const SimulationWorld *>(owner);
            const auto &a = w.combat_state->scheduled_actions[row];
            if (!(a.kind == ScheduledKind::Explosion && a.skill == SkillKind::ExplosiveArrow && a.source_upgrade == 1 && a.visual_owner_id != 0 && w.tick < a.due)) return false;
            v = {a.visual_owner_id, a.cast_id, a.position, a.radius, a.visual_started, a.due}; return true;
        });
    const auto delayed_count = progression->pending_enemy_spawns_delayed.size();
    TickRows<SpawnWarningView> warnings(this, delayed_count + progression->pending_boss_spawns.size(), f.spawn_warnings,
        [](const void *owner, std::size_t row, SpawnWarningView &v) {
            const auto &w = *static_cast<const SimulationWorld *>(owner);
            const auto count = w.progression->pending_enemy_spawns_delayed.size();
            if (row < count) {
                const auto &p = w.progression->pending_enemy_spawns_delayed[row];
                if (p.warning_sequence == 0 || p.due <= w.tick) return false;
                v = {p.warning_sequence, p.position, w.rules.enemies[static_cast<std::size_t>(p.kind)].collision_radius, p.warning_started, p.due, false};
            } else {
                const auto &p = w.progression->pending_boss_spawns[row - count];
                if (p.warning_sequence == 0 || p.due <= w.tick) return false;
                v = {p.warning_sequence, p.position, w.rules.boss_common.collision_radius, p.warning_started, p.due, true};
            } return true;
        });
    TickRows<ScheduledAreaDeliveryView> deliveries(this, combat_state->scheduled_actions.size(), f.deliveries,
        [](const void *owner, std::size_t row, ScheduledAreaDeliveryView &v) {
            const auto &w = *static_cast<const SimulationWorld *>(owner);
            const auto &a = w.combat_state->scheduled_actions[row];
            if (!(a.kind == ScheduledKind::Area && a.skill == SkillKind::ArrowRain && a.origin == EffectOrigin::Derived && a.source_upgrade == 0 && a.visual_owner_id != 0 && a.visual_started <= w.tick && w.tick < a.due)) return false;
            v = {a.visual_owner_id, a.cast_id, a.position, a.radius, a.visual_started, a.due, a.skill, a.source_upgrade}; return true;
        });
    const auto &m = Metrics();
    return {tick, checksum, config.seed, f.session, f.player, enemies, projectiles, areas, pickups, boss_actions,
        f.effective_attack, f.effective_attack_speed, f.effective_move_speed, f.effective_magnet_radius,
        rules.arena_half_extent, rules.arena_boundary, ArenaObstacles(), f.charge_range, f.charge_radius,
        f.charge_normal_ready_tick, f.charge_ratio, f.skills, f.waves,
        {m.direct_damage, m.derived_damage, m.damage_over_time, m.upgrade_damage, m.relic_damage, m.relic_triggers, m.relic_kills, m.relic_effects},
        session_id, links, bombs, warnings, deliveries,
        std::span(combat_state->boss_death_visuals).first(combat_state->boss_death_visual_count), combat_state->domain_signals};
}

void GameSimulation::CopyReadModelForDevelopment(GameReadModelStorage &model) const
{
    auto scope = BorrowTickReadView();
    const auto &view = scope.View();
    model.Clear(); model.tick = view.tick; model.checksum = view.checksum; model.seed = view.seed;
    model.session = view.session; model.session_id = view.session_id; model.player = view.player;
    model.effective_attack = view.effective_attack; model.effective_attack_speed = view.effective_attack_speed;
    model.effective_move_speed = view.effective_move_speed; model.effective_magnet_radius = view.effective_magnet_radius;
    model.arena_half_extent = view.arena_half_extent; model.arena_boundary = view.arena_boundary;
    model.arena_obstacle_count = static_cast<std::uint32_t>(view.arena_obstacles.size());
    std::ranges::copy(view.arena_obstacles, model.arena_obstacles.begin());
    model.charge_range = view.charge_range; model.charge_radius = view.charge_radius;
    model.charge_normal_ready_tick = view.charge_normal_ready_tick; model.charge_ratio = view.charge_ratio;
    std::ranges::copy(view.skills, model.skills.begin()); std::ranges::copy(view.waves, model.waves.begin());
    model.summary = {view.summary.direct_damage, view.summary.derived_damage, view.summary.damage_over_time,
        view.summary.upgrade_damage, view.summary.relic_damage, view.summary.relic_triggers, view.summary.relic_kills, view.summary.relic_effects};
    for (const auto &e : view.enemies) model.AddEnemy(e);
    for (const auto &p : view.projectiles) model.AddProjectile({p.id, p.player_owned, p.position, p.velocity, p.spawned_tick, p.skill, p.dead, p.radius, p.charge_ratio, p.origin, p.source_upgrade, p.previous_position, p.remaining_range, p.cast_id, p.source_relic, p.source_enemy, p.returning, p.homing, p.return_started_tick, p.return_start_position, p.upgrade_mask, {p.bleed_extend_ticks.begin(), p.bleed_extend_ticks.end()}});
    for (const auto &a : view.areas) model.AddArea(a);
    for (const auto &p : view.pickups) model.AddPickup(p);
    for (const auto &a : view.boss_actions) model.AddBossAction(a);
    for (const auto &v : view.visual_links) model.AddVisualLink(v);
    for (const auto &v : view.mini_bombs) model.AddMiniBomb(v);
    for (const auto &v : view.spawn_warnings) model.AddSpawnWarning(v);
    for (const auto &v : view.scheduled_area_deliveries) model.AddScheduledAreaDelivery(v);
    for (const auto &v : view.boss_death_visuals) model.AddBossDeathVisual(v);
}

std::span<const DomainSignal> GameSimulation::PendingDomainSignals() const noexcept
{
    return impl_->combat_state->domain_signals;
}

void GameSimulation::ClearDomainSignals()
{
    impl_->RequireOwnerThread();
    if (impl_->observation.active_borrows != 0 || impl_->observation.mutation_depth != 0)
        throw std::logic_error("Domain signals cannot clear during a read/mutation scope.");
    impl_->combat_state->domain_signals.clear();
}

Result GameSimulation::Shutdown()
{
    SimulationWorld::MutationScope mutation(*impl_);
    if (!impl_->initialized)
    {
        return Result::Success();
    }
    impl_->initialized = false;
    return Result::Success();
}

} // namespace hs
