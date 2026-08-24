#include <hs/game_rules/simulation_rules.hpp>

#include <hs/core/cooked_format.hpp>

#include <bit>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

namespace hs
{
namespace
{

template <typename... Tags>
constexpr SkillTagMask TagMask(Tags... tags) noexcept
{
    return static_cast<SkillTagMask>((0u | ... | static_cast<unsigned>(tags)));
}

using enum SkillTag;

constexpr std::array<SkillTagMask, kCombatSkillCount> kSkillTags{
    0, 0, 0, 0, 0, 0, 0, TagMask(Slow), 0};

constexpr std::array<std::array<SkillTagMask, kUpgradeCount>, kCombatSkillCount>
    kUpgradeTags{{
        {{0, 0, 0, TagMask(Bleed), TagMask(Burn), TagMask(Slow), 0, 0}},
        {{0, 0, TagMask(Bleed), TagMask(Slow), 0, 0, TagMask(Burn), 0}},
        {{0, 0, 0, 0, TagMask(Bleed), TagMask(Burn), 0, 0}},
        {{0, 0, 0, 0, TagMask(Bleed), TagMask(Burn), 0, 0}},
        {{0, 0, 0, TagMask(Burn), TagMask(Bleed), 0, 0, 0}},
        {{0, 0, TagMask(Bleed), TagMask(Burn), 0, 0, 0, 0}},
        {{0, 0, TagMask(Bleed), TagMask(Burn), 0, TagMask(Slow), 0,
          TagMask(Slow)}},
        {{0, TagMask(Slow), 0, TagMask(Bleed), TagMask(Burn), TagMask(Slow), 0,
          0}},
        {{0, 0, TagMask(Slow), TagMask(Bleed), 0, 0, 0, 0}}
    }};

constexpr std::array<SkillTagMask, kRelicCount> kRelicPrerequisites{
    TagMask(Bleed), TagMask(Burn), 0, TagMask(Bleed, Burn),
    0, 0, 0, 0, 0, 0, 0, 0};

class CanonicalHash
{
  public:
    template <typename T>
    void Add(T value) noexcept
    {
        if constexpr (std::is_enum_v<T>)
            Add(static_cast<std::underlying_type_t<T>>(value));
        else if constexpr (std::is_same_v<T, float>)
            Add(std::bit_cast<std::uint32_t>(value));
        else if constexpr (std::is_same_v<T, bool>)
            Add(static_cast<std::uint8_t>(value ? 1u : 0u));
        else
        {
            using Unsigned = std::make_unsigned_t<T>;
            const auto bits = static_cast<Unsigned>(value);
            for (std::size_t byte = 0; byte < sizeof(T); ++byte)
            {
                value_ ^= static_cast<std::uint8_t>(bits >> (byte * 8));
                value_ *= 1099511628211ull;
            }
        }
    }

    [[nodiscard]] std::uint64_t Value() const noexcept { return value_; }

  private:
    std::uint64_t value_{14695981039346656037ull};
};

template <typename T, std::size_t Size, typename AddElement>
void AddArray(CanonicalHash &hash, const std::array<T, Size> &values,
              AddElement add_element) noexcept
{
    for (const auto &value : values) add_element(hash, value);
}

void SetSkill(SkillDefinition &skill, AbilityHandlerId handler, Tick cooldown,
              float damage, float speed, float range, float collision,
              float area, Tick duration, std::uint8_t projectiles,
              std::uint8_t pierce) noexcept
{
    skill.cooldown_ticks = cooldown;
    skill.damage_coefficient = damage;
    skill.projectile_speed = speed;
    skill.range = range;
    skill.collision_radius = collision;
    skill.area_radius = area;
    skill.duration_ticks = duration;
    skill.projectile_count = projectiles;
    skill.pierce_count = pierce;
    skill.handler = handler;
    skill.damage_multiplier = damage;
}

void HashSkill(CanonicalHash &hash, const SkillDefinition &skill) noexcept
{
    hash.Add(skill.cooldown_ticks);
    hash.Add(skill.damage_coefficient);
    hash.Add(skill.projectile_speed);
    hash.Add(skill.range);
    hash.Add(skill.collision_radius);
    hash.Add(skill.area_radius);
    hash.Add(skill.duration_ticks);
    hash.Add(skill.projectile_count);
    hash.Add(skill.pierce_count);
    hash.Add(skill.handler);
    hash.Add(skill.damage_multiplier);
    hash.Add(skill.fan_angle_degrees);
    hash.Add(skill.damage_multiplier_per_arrow);
    hash.Add(skill.pierce_per_arrow);
    hash.Add(skill.maximum_hits_per_target);
    hash.Add(skill.maximum_charge_time_ticks);
    hash.Add(skill.minimum_damage_multiplier);
    hash.Add(skill.maximum_damage_multiplier);
    hash.Add(skill.minimum_range);
    hash.Add(skill.maximum_range);
    hash.Add(skill.minimum_collision_radius);
    hash.Add(skill.maximum_collision_radius);
    hash.Add(skill.movement_speed_multiplier_while_charging);
    hash.Add(skill.pierce_damage_decay_fraction);
    hash.Add(skill.minimum_damage_fraction);
    hash.Add(skill.explosion_damage_multiplier);
    hash.Add(skill.explosion_radius);
    hash.Add(skill.initial_range);
    hash.Add(skill.ricochet_search_radius);
    hash.Add(skill.maximum_ricochets);
    hash.Add(skill.target_range);
    hash.Add(skill.activation_delay_ticks);
    hash.Add(skill.tick_interval_ticks);
    hash.Add(skill.total_damage_ticks);
    hash.Add(skill.forward_roll_duration_ticks);
    hash.Add(skill.forward_roll_distance);
    hash.Add(skill.active_duration_ticks);
    hash.Add(skill.detection_radius);
    hash.Add(skill.slow_fraction);
    hash.Add(skill.slow_duration_ticks);
    hash.Add(skill.forced_move_distance);
    hash.Add(skill.forced_move_duration_ticks);
    hash.Add(skill.projectile_range);
    hash.Add(skill.starting_level);
    hash.Add(skill.maximum_level);
    hash.Add(skill.selected_upgrades_per_session);
}

void HashUpgrades(CanonicalHash &hash, const UpgradeRules &u) noexcept
{
    const auto &b = u.basic_attack;
    hash.Add(b.third_attack_delayed.cadence_interval);
    hash.Add(b.third_attack_delayed.delay_ticks);
    hash.Add(b.third_attack_delayed.damage_multiplier);
    hash.Add(b.additional_pierce.additional_targets);
    for (const auto value : b.first_hit_split.angles_degrees) hash.Add(value);
    hash.Add(b.first_hit_split.projectile_count);
    hash.Add(b.first_hit_split.damage_multiplier);
    hash.Add(b.direct_arrow_bleed.direct_arrow_interval);
    hash.Add(b.direct_arrow_bleed.bleed_stacks);
    hash.Add(b.direct_arrow_burn.direct_arrow_interval);
    hash.Add(b.first_hit_slow_and_cooldown.slow_fraction);
    hash.Add(b.first_hit_slow_and_cooldown.slow_duration_ticks);
    hash.Add(b.first_hit_slow_and_cooldown.cooldown_reduction_ticks);
    hash.Add(b.missed_arrow_reacquire.damage_multiplier);
    hash.Add(b.missed_arrow_reacquire.maximum_retargets);
    hash.Add(b.missed_arrow_reacquire.collision_radius_multiplier);
    hash.Add(b.post_active_three_arrow.activation_window_ticks);
    for (const auto value : b.post_active_three_arrow.angles_degrees) hash.Add(value);
    hash.Add(b.post_active_three_arrow.projectile_count);
    hash.Add(b.post_active_three_arrow.damage_multiplier_per_arrow);

    const auto &p = u.piercing_shot;
    hash.Add(p.delayed_followup.delay_ticks);
    hash.Add(p.delayed_followup.damage_multiplier);
    for (const auto value : p.range_end_split.angles_degrees) hash.Add(value);
    hash.Add(p.range_end_split.projectile_count);
    hash.Add(p.range_end_split.damage_multiplier);
    hash.Add(p.range_end_split.search_radius);
    hash.Add(p.apply_bleed.bleed_stacks);
    hash.Add(p.damaging_slow_trail.width);
    hash.Add(p.damaging_slow_trail.duration_ticks);
    hash.Add(p.damaging_slow_trail.tick_interval_ticks);
    hash.Add(p.damaging_slow_trail.damage_multiplier_per_tick);
    hash.Add(p.damaging_slow_trail.slow_fraction);
    hash.Add(p.damaging_slow_trail.slow_duration_ticks);
    hash.Add(p.per_three_pierces_perpendicular.normal_enemy_pierce_interval);
    hash.Add(p.per_three_pierces_perpendicular.projectile_count);
    hash.Add(p.per_three_pierces_perpendicular.angle_from_direction);
    hash.Add(p.per_three_pierces_perpendicular.damage_multiplier);
    hash.Add(p.align_hit_normal_enemy.move_distance);
    hash.Add(p.apply_and_transfer_burn.maximum_transfer_targets);
    hash.Add(p.cooldown_multiplier.cooldown_multiplier);

    const auto &m = u.multishot;
    hash.Add(m.delayed_second_fan.delay_ticks);
    hash.Add(m.delayed_second_fan.fan_angle_degrees);
    hash.Add(m.delayed_second_fan.projectile_count);
    hash.Add(m.delayed_second_fan.damage_multiplier_per_arrow);
    for (const auto value : m.original_arrow_first_hit_split.angles_degrees) hash.Add(value);
    hash.Add(m.original_arrow_first_hit_split.projectile_count_per_original);
    hash.Add(m.original_arrow_first_hit_split.damage_multiplier);
    hash.Add(m.additional_pierce_per_arrow.additional_pierce);
    hash.Add(m.rear_fan.projectile_count);
    hash.Add(m.rear_fan.damage_multiplier);
    hash.Add(m.rear_fan.fan_angle_degrees);
    hash.Add(m.first_cast_hit_bleed.bleed_stacks);
    hash.Add(m.apply_burn_and_transfer_arrow.damage_multiplier);
    hash.Add(m.apply_burn_and_transfer_arrow.maximum_transfers_per_cast);
    hash.Add(m.two_additional_outer_arrows.additional_projectiles);
    for (const auto value : m.two_additional_outer_arrows.outer_angles_degrees) hash.Add(value);
    hash.Add(m.two_additional_outer_arrows.damage_multiplier);
    hash.Add(m.missed_arrow_retarget.damage_multiplier);
    hash.Add(m.missed_arrow_retarget.maximum_retargets);
    hash.Add(m.missed_arrow_retarget.search_radius);

    const auto &c = u.charged_shot;
    hash.Add(c.extended_full_charge_explosion.maximum_charge_time_ticks);
    hash.Add(c.extended_full_charge_explosion.maximum_damage_multiplier);
    hash.Add(c.extended_full_charge_explosion.full_charge_end_explosion_damage_multiplier);
    hash.Add(c.extended_full_charge_explosion.explosion_radius);
    hash.Add(c.faster_charge.charge_time_multiplier);
    hash.Add(c.increase_minimum_charge_damage.minimum_damage_multiplier);
    hash.Add(c.additional_pierce.additional_pierce);
    hash.Add(c.charge_bleed_full_charge_rupture.bleed_stacks);
    hash.Add(c.charge_bleed_full_charge_rupture.existing_bleed_stacks_for_rupture);
    hash.Add(c.charge_bleed_full_charge_rupture.rupture_damage_multiplier);
    hash.Add(c.boss_or_fifth_pierce_split.normal_enemy_pierce_count);
    for (const auto value : c.boss_or_fifth_pierce_split.angles_degrees) hash.Add(value);
    hash.Add(c.boss_or_fifth_pierce_split.projectile_count);
    hash.Add(c.boss_or_fifth_pierce_split.damage_multiplier);
    hash.Add(c.first_hit_burn_explosion.radius);
    hash.Add(c.first_hit_burn_explosion.damage_multiplier);
    hash.Add(c.full_charge_multi_kill_cooldown_refund.minimum_normal_enemy_kills);
    hash.Add(c.full_charge_multi_kill_cooldown_refund.current_cooldown_refund_fraction);

    const auto &e = u.explosive_arrow;
    hash.Add(e.delayed_reexplosion.delay_ticks);
    hash.Add(e.delayed_reexplosion.radius);
    hash.Add(e.delayed_reexplosion.damage_multiplier);
    hash.Add(e.three_delayed_satellite_bombs.bomb_count);
    hash.Add(e.three_delayed_satellite_bombs.angular_spacing);
    hash.Add(e.three_delayed_satellite_bombs.placement_radius);
    hash.Add(e.three_delayed_satellite_bombs.delay_ticks);
    hash.Add(e.three_delayed_satellite_bombs.explosion_radius);
    hash.Add(e.three_delayed_satellite_bombs.damage_multiplier);
    hash.Add(e.eight_direction_fragments.direction_count);
    hash.Add(e.eight_direction_fragments.damage_multiplier);
    hash.Add(e.eight_direction_fragments.collision_radius_multiplier);
    hash.Add(e.eight_direction_fragments.pierce);
    hash.Add(e.explosion_leaves_burning_area.radius);
    hash.Add(e.explosion_leaves_burning_area.duration_ticks);
    hash.Add(e.explosion_leaves_burning_area.tick_interval_ticks);
    hash.Add(e.explosion_leaves_burning_area.damage_multiplier_per_tick);
    hash.Add(e.apply_bleed_and_blood_explosions.radius);
    hash.Add(e.apply_bleed_and_blood_explosions.damage_multiplier);
    hash.Add(e.apply_bleed_and_blood_explosions.maximum_explosions_per_cast);
    hash.Add(e.apply_bleed_and_blood_explosions.bleed_stacks);
    hash.Add(e.pre_explosion_pull.duration_ticks);
    hash.Add(e.pre_explosion_pull.pull_radius);
    hash.Add(e.direct_hit_mark_other_active_explosion.immediate_explosion_damage_multiplier);
    hash.Add(e.direct_hit_mark_other_active_explosion.mark_explosion_damage_multiplier);
    hash.Add(e.direct_hit_mark_other_active_explosion.mark_duration_ticks);
    hash.Add(e.direct_hit_mark_other_active_explosion.mark_explosion_radius);
    hash.Add(e.cooldown_multiplier.cooldown_multiplier);

    const auto &r = u.ricochet_arrow;
    hash.Add(r.return_to_player_rehit.damage_multiplier);
    hash.Add(r.return_to_player_rehit.maximum_rehit_targets);
    hash.Add(r.first_ricochet_branch_chain.damage_multiplier);
    hash.Add(r.first_ricochet_branch_chain.maximum_targets);
    hash.Add(r.apply_bleed_and_extend_ricochets.additional_ricochets_per_bleeding_hit);
    hash.Add(r.apply_bleed_and_extend_ricochets.maximum_additional_ricochets);
    hash.Add(r.apply_and_copy_burn_to_next_target.enabled);
    hash.Add(r.kill_small_arrows.damage_multiplier);
    hash.Add(r.kill_small_arrows.arrows_per_kill);
    hash.Add(r.kill_small_arrows.maximum_arrows_per_cast);
    hash.Add(r.kill_small_arrows.search_radius);
    hash.Add(r.original_ricochet_kill_new_chain.new_chain_targets);
    hash.Add(r.original_ricochet_kill_new_chain.damage_multiplier);
    hash.Add(r.original_ricochet_kill_new_chain.search_radius);
    hash.Add(r.ricochet_cooldown_reduction.cooldown_reduction_per_ricochet_ticks);
    hash.Add(r.ricochet_cooldown_reduction.maximum_reduction_ticks);
    hash.Add(r.cooldown_multiplier.cooldown_multiplier);

    const auto &a = u.arrow_rain;
    hash.Add(a.delayed_forward_secondary_area.delay_ticks);
    hash.Add(a.delayed_forward_secondary_area.forward_offset);
    hash.Add(a.delayed_forward_secondary_area.radius);
    hash.Add(a.delayed_forward_secondary_area.duration_ticks);
    hash.Add(a.delayed_forward_secondary_area.damage_multiplier_per_tick);
    hash.Add(a.first_damage_tick_pull.pull_radius);
    hash.Add(a.third_area_hit_bleed.hit_ordinal);
    hash.Add(a.third_area_hit_bleed.bleed_stacks);
    hash.Add(a.apply_burn_and_create_fire_areas.radius);
    hash.Add(a.apply_burn_and_create_fire_areas.duration_ticks);
    hash.Add(a.apply_burn_and_create_fire_areas.tick_interval_ticks);
    hash.Add(a.apply_burn_and_create_fire_areas.damage_multiplier_per_tick);
    hash.Add(a.apply_burn_and_create_fire_areas.maximum_areas_per_cast);
    hash.Add(a.tracking_arrow_each_damage_tick.search_radius);
    hash.Add(a.tracking_arrow_each_damage_tick.damage_multiplier);
    hash.Add(a.area_slow.slow_fraction);
    hash.Add(a.area_slow.slow_duration_ticks);
    hash.Add(a.area_kill_arrow_outside.damage_multiplier);
    hash.Add(a.area_kill_arrow_outside.maximum_triggers_per_cast);
    hash.Add(a.extend_area_and_leave_slow.area_duration_ticks);
    hash.Add(a.extend_area_and_leave_slow.additional_damage_ticks);
    hash.Add(a.extend_area_and_leave_slow.post_area_duration_ticks);
    hash.Add(a.extend_area_and_leave_slow.post_area_slow_fraction);
    hash.Add(a.extend_area_and_leave_slow.post_area_slow_duration_ticks);

    const auto &t = u.trap;
    hash.Add(t.roll_path_traps.trap_count);
    hash.Add(t.roll_path_traps.damage_multiplier_per_trap);
    hash.Add(t.roll_path_traps.spacing);
    hash.Add(t.landing_slow_area.radius);
    hash.Add(t.landing_slow_area.slow_fraction);
    hash.Add(t.landing_slow_area.duration_ticks);
    hash.Add(t.single_reactivation.reactivation_delay_ticks);
    hash.Add(t.single_reactivation.maximum_reactivations);
    hash.Add(t.trigger_bleed.bleed_stacks);
    hash.Add(t.trigger_burn_and_fire_area.area_radius);
    hash.Add(t.trigger_burn_and_fire_area.area_duration_ticks);
    hash.Add(t.trigger_burn_and_fire_area.tick_interval_ticks);
    hash.Add(t.trigger_burn_and_fire_area.damage_multiplier_per_tick);
    hash.Add(t.pre_explosion_pull_and_slow.pull_radius);
    hash.Add(t.pre_explosion_pull_and_slow.slow_fraction);
    hash.Add(t.pre_explosion_pull_and_slow.slow_duration_ticks);
    hash.Add(t.trigger_mark_other_active_damage.immediate_damage_multiplier);
    hash.Add(t.trigger_mark_other_active_damage.mark_explosion_damage_multiplier);
    hash.Add(t.trigger_mark_other_active_damage.mark_duration_ticks);
    hash.Add(t.trigger_mark_other_active_damage.mark_explosion_radius);
    hash.Add(t.trap_kill_small_trap.detection_radius);
    hash.Add(t.trap_kill_small_trap.explosion_radius);
    hash.Add(t.trap_kill_small_trap.activation_delay_ticks);
    hash.Add(t.trap_kill_small_trap.active_duration_ticks);
    hash.Add(t.trap_kill_small_trap.damage_multiplier);
    hash.Add(t.trap_kill_small_trap.maximum_small_traps_per_cast);

    const auto &q = u.retreat_shot;
    hash.Add(q.replace_with_three_arrows.projectile_count);
    hash.Add(q.replace_with_three_arrows.damage_multiplier_per_arrow);
    for (const auto value : q.replace_with_three_arrows.angles_degrees) hash.Add(value);
    hash.Add(q.small_trap_at_start.damage_multiplier);
    hash.Add(q.small_trap_at_start.detection_radius);
    hash.Add(q.small_trap_at_start.explosion_radius);
    hash.Add(q.small_trap_at_start.activation_delay_ticks);
    hash.Add(q.small_trap_at_start.active_duration_ticks);
    hash.Add(q.slow_trail.duration_ticks);
    hash.Add(q.slow_trail.slow_fraction);
    hash.Add(q.slow_trail.radius);
    hash.Add(q.slow_trail.slow_duration_ticks);
    hash.Add(q.apply_bleed_and_tracking_arrow.damage_multiplier);
    hash.Add(q.apply_bleed_and_tracking_arrow.maximum_triggers_per_cast);
    hash.Add(q.apply_bleed_and_tracking_arrow.search_radius);
    hash.Add(q.apply_bleed_and_tracking_arrow.bleed_stacks);
    hash.Add(q.landing_damage_and_push.radius);
    hash.Add(q.landing_damage_and_push.damage_multiplier);
    hash.Add(q.landing_damage_and_push.push_distance);
    hash.Add(q.next_other_active_cooldown_refund.activation_window_ticks);
    hash.Add(q.next_other_active_cooldown_refund.cooldown_refund_fraction);
    hash.Add(q.boss_or_three_normal_hits_heal.minimum_normal_enemy_hits);
    hash.Add(q.boss_or_three_normal_hits_heal.maximum_hp_heal_fraction);
    hash.Add(q.delayed_second_retreat_and_arrow.delay_after_first_move_ticks);
    hash.Add(q.delayed_second_retreat_and_arrow.additional_move_distance);
    hash.Add(q.delayed_second_retreat_and_arrow.damage_multiplier);
}

void HashPattern(CanonicalHash &hash, const BossPatternDefinition &pattern) noexcept
{
    hash.Add(pattern.logic);
    hash.Add(pattern.phase);
    hash.Add(pattern.telegraph_duration_ticks);
    hash.Add(pattern.distance);
    hash.Add(pattern.speed);
    hash.Add(pattern.damage);
    hash.Add(pattern.start_radius);
    hash.Add(pattern.end_radius);
    hash.Add(pattern.safe_gap_count);
    hash.Add(pattern.safe_gap_angle_degrees);
    hash.Add(pattern.shockwave_duration_ticks);
    hash.Add(pattern.shockwave_half_width);
    hash.Add(pattern.fan_angle_degrees);
    hash.Add(pattern.projectile_count);
    hash.Add(pattern.projectile_speed);
    hash.Add(pattern.area_count);
    hash.Add(pattern.radius);
    hash.Add(pattern.duration_ticks);
    hash.Add(pattern.tick_interval_ticks);
    hash.Add(pattern.damage_per_tick);
    hash.Add(pattern.charge_count);
    hash.Add(pattern.interval_ticks);
    hash.Add(pattern.volley_count);
    hash.Add(pattern.projectiles_per_volley);
    hash.Add(pattern.second_volley_angle_offset_degrees);
    hash.Add(pattern.prediction_lead_ticks);
    hash.Add(pattern.ground_placement_radius);
}

void HashBossTransition(CanonicalHash &hash, const BossPhaseTransition &transition) noexcept
{
    hash.Add(transition.hp_fraction);
    hash.Add(transition.invulnerability_ticks);
    hash.Add(transition.remove_enemy_projectiles);
    hash.Add(transition.remove_enemy_areas);
    hash.Add(transition.keep_normal_enemies);
    hash.Add(transition.keep_mid_bosses);
    hash.Add(transition.keep_pickups);
    hash.Add(transition.keep_player_projectiles);
    hash.Add(transition.keep_player_areas);
}

void HashRelics(CanonicalHash &hash, const RelicDefinitions &r) noexcept
{
    hash.Add(r.bleed_kill_heal.maximum_hp_heal_fraction);
    hash.Add(r.bleed_kill_heal.internal_cooldown_ticks);
    hash.Add(r.burn_propagation.search_radius);
    hash.Add(r.burn_propagation.copied_burn_strength);
    hash.Add(r.burn_propagation.maximum_targets);
    hash.Add(r.kill_cooldown_surge.kills_per_trigger);
    hash.Add(r.kill_cooldown_surge.cooldown_reduction_ticks);
    hash.Add(r.bleed_burn_explosion.radius);
    hash.Add(r.bleed_burn_explosion.damage_multiplier);
    hash.Add(r.bleed_burn_explosion.per_target_cooldown_ticks);
    hash.Add(r.radial_basic_attack.cadence_interval);
    hash.Add(r.radial_basic_attack.direction_count);
    hash.Add(r.radial_basic_attack.damage_multiplier);
    hash.Add(r.basic_kill_tracker.search_radius);
    hash.Add(r.basic_kill_tracker.damage_multiplier);
    hash.Add(r.basic_kill_tracker.maximum_triggers_per_attack);
    hash.Add(r.movement_echo.required_distance);
    hash.Add(r.movement_echo.position_history_age_ticks);
    hash.Add(r.movement_echo.damage_multiplier);
    hash.Add(r.alternating_skills.window_ticks);
    hash.Add(r.alternating_skills.cooldown_refund_fraction);
    hash.Add(r.different_skill_tracker.window_ticks);
    hash.Add(r.different_skill_tracker.damage_multiplier);
    hash.Add(r.different_skill_tracker.per_target_cooldown_ticks);
    hash.Add(r.damage_knockback.radius);
    hash.Add(r.damage_knockback.push_distance);
    hash.Add(r.damage_knockback.slow_fraction);
    hash.Add(r.damage_knockback.slow_duration_ticks);
    hash.Add(r.damage_knockback.cooldown_ticks);
    hash.Add(r.once_revive.health_fraction);
    hash.Add(r.once_revive.invulnerability_ticks);
    hash.Add(r.once_revive.maximum_triggers_per_session);
    hash.Add(r.combat_hit_chain.direct_hits_per_trigger);
    hash.Add(r.combat_hit_chain.search_radius);
    hash.Add(r.combat_hit_chain.maximum_targets);
    hash.Add(r.combat_hit_chain.damage_multiplier);
}

} // namespace

SkillTagMask SkillTags(SkillKind skill) noexcept
{
    const auto index = static_cast<std::size_t>(skill);
    return index < kSkillTags.size() ? kSkillTags[index] : 0;
}

SkillTagMask UpgradeTags(SkillKind skill, std::uint8_t zero_based_upgrade) noexcept
{
    const auto skill_index = static_cast<std::size_t>(skill);
    return skill_index < kUpgradeTags.size() &&
                   zero_based_upgrade < kUpgradeTags[skill_index].size()
               ? kUpgradeTags[skill_index][zero_based_upgrade]
               : 0;
}

SkillTagMask RelicPrerequisiteTags(RelicKind relic) noexcept
{
    const auto index = static_cast<std::size_t>(relic);
    return index < kRelicPrerequisites.size() ? kRelicPrerequisites[index] : 0;
}

SimulationRules SimulationRules::Defaults() noexcept
{
    SimulationRules data{};
    data.version = 5;
    data.arena_half_extent = 60.0f;
    data.player_health = 100;
    data.player_attack = 10.0f;
    data.player_attack_speed = 0.8f;
    data.player_move_speed = 5.0f;
    data.player_magnet_radius = 3.0f;
    data.utility_pickup_base_chance = 0.01f;
    data.utility_pickup_miss_increment = 0.001f;
    data.heal_pickup_chance_multiplier = 0.5f;
    data.magnet_pickup_chance_multiplier = 0.25f;
    data.relic_chest_base_chance = 0.00001f;
    data.relic_chest_miss_increment = 0.000004f;
    data.status_tick_interval = 30;
    data.bleed_duration = 240;
    data.bleed_tick_coefficient = 0.20f;
    data.burn_duration = 240;
    data.burn_tick_coefficient = 0.35f;

    data.stats.maximum_points_per_stat = 10;
    data.stats.base_maximum_hp = 100.0f;
    data.stats.base_current_hp = 100.0f;
    data.stats.base_attack_power = 10.0f;
    data.stats.base_basic_attack_rate_per_second = 0.8f;
    data.stats.base_movement_speed_mps = 5.0f;
    data.stats.base_magnet_radius_m = 3.0f;
    data.stats.base_global_cooldown_reduction = 0.0f;
    data.stats.allocations = {{{0.08f, 8.0f}, {0.1f, 0.0f}, {0.1f, 0.0f},
                               {0.2f, 0.0f}, {0.05f, 0.0f}, {1.0f, 0.0f}}};
    data.stats.combat = {1, 15, 0.75f, 1, false, false, false, false};
    data.stats.first_status_tick_delay_ticks = 6;
    data.stats.player_collision_radius = 0.4f;

    data.statuses.first_tick_delay_ticks = 6;
    data.statuses.tick_interval_ticks = 30;
    data.statuses.bleed = {5, 0, 240, 30, 0.20f};
    data.statuses.burn = {0, 1, 240, 30, 0.35f};

    data.growth = {12.0f, 3.5f, 0.75f, 15.0f, 0.6f, 0.01f, 0.001f, 0.5f,
                   0.25f};
    data.progression.enemy_xp = {1, 2, 2};
    data.progression.boss_xp = {100, 200, 0};
    data.progression.required_xp_base = data.growth.required_xp_base;
    data.progression.required_xp_linear = data.growth.required_xp_linear;
    data.progression.required_xp_quadratic = data.growth.required_xp_quadratic;
    data.progression.card_candidate_count = 3;
    data.progression.card_choose_count = 1;
    data.progression.active_slot_count = 4;
    data.progression.upgrades_selected_per_skill = 4;
    data.progression.base_stat_points_per_level = 1;
    data.progression.fallback_extra_stat_points = 1;
    data.progression.fallback_stat_point_cards = true;
    data.progression.level_initial_rerolls = 3;
    data.progression.relic_initial_rerolls = 3;
    data.progression.boss_spawn_ticks = {18'000, 36'000, 54'000};
    data.progression.stop_normal_spawns_at_final_boss = true;
    data.character_initial.starting_level = 1;
    data.character_initial.starting_basic_attack_level = 1;
    data.character_initial.starting_level_rerolls = 3;
    data.character_initial.starting_relic_rerolls = 3;
    data.character_initial.movement_stop_distance_m = 0.15f;
    data.character_initial.starting_active_skill_ids.fill(SkillKind::Count);
    data.character_initial.starting_relic_ids.fill(RelicKind::Count);

    data.enemy_scaling = {EnemyScalingMachine::CompletedMinutes, 0.15f, 0.05f,
                          false, false};
    data.relic_drop = {1, 0.25f, 0.00001f, 0.000004f, 1.0f, 3, 0.01f, 0.08f,
                       false, true, true, true, true, true};
    data.spawn_placement = {20.0f, 30.0f, true, true, 30,
                            SpawnFallbackLocation::FarthestArenaEdge};
    data.boss_common = {1.1f, 90, 90, 3, 1, 2, false, false, true, true,
                        true, true, true, true, true, 24.0f, 0.25f};

    SetSkill(data.skills[0], AbilityHandlerId::BasicProjectileCadence, 0, 1.0f,
             32.0f, 18.0f, 0.36f, 0.0f, 0, 1, 0);
    data.skills[0].starting_level = 1;
    data.skills[0].maximum_level = 5;
    data.skills[0].selected_upgrades_per_session = 4;
    SetSkill(data.skills[1], AbilityHandlerId::PiercingProjectile, 240, 1.8f,
             30.0f, 24.0f, 0.60f, 0.0f, 0, 1, 255);
    data.skills[1].maximum_hits_per_target = 1;
    data.skills[1].pierce_damage_decay_fraction = 0.15f;
    data.skills[1].minimum_damage_fraction = 0.55f;
    SetSkill(data.skills[2], AbilityHandlerId::UniformFanProjectiles, 330, 1.7f,
             25.0f, 16.0f, 0.36f, 0.0f, 0, 9, 1);
    data.skills[2].fan_angle_degrees = 50.0f;
    data.skills[2].damage_multiplier_per_arrow = 1.7f;
    data.skills[2].pierce_per_arrow = 1;
    data.skills[2].maximum_hits_per_target = 2;
    SetSkill(data.skills[3], AbilityHandlerId::HoldReleaseLinearCharge, 240, 5.0f,
             35.0f, 16.8f, 0.88f, 0.0f, 60, 1, 12);
    data.skills[3].maximum_charge_time_ticks = 60;
    data.skills[3].minimum_damage_multiplier = 2.0f;
    data.skills[3].maximum_damage_multiplier = 5.0f;
    data.skills[3].minimum_range = 4.2f;
    data.skills[3].maximum_range = 16.8f;
    data.skills[3].minimum_collision_radius = 0.4f;
    data.skills[3].maximum_collision_radius = 0.88f;
    data.skills[3].movement_speed_multiplier_while_charging = 0.7f;
    SetSkill(data.skills[4], AbilityHandlerId::ProjectileToAreaExplosion, 390, 2.8f,
             22.0f, 18.0f, 0.50f, 3.0f, 0, 1, 0);
    data.skills[4].explosion_radius = 3.0f;
    data.skills[4].explosion_damage_multiplier = 2.8f;
    SetSkill(data.skills[5], AbilityHandlerId::NearestUnhitTargetRicochet, 360, 0.8f,
             28.0f, 18.0f, 0.44f, 6.0f, 0, 1, 5);
    data.skills[5].initial_range = 18.0f;
    data.skills[5].ricochet_search_radius = 6.0f;
    data.skills[5].maximum_ricochets = 5;
    SetSkill(data.skills[6], AbilityHandlerId::TargetedPeriodicArea, 540, 0.7f,
             0.0f, 20.0f, 0.0f, 4.0f, 180, 1, 0);
    data.skills[6].target_range = 20.0f;
    data.skills[6].activation_delay_ticks = 24;
    data.skills[6].tick_interval_ticks = 30;
    data.skills[6].total_damage_ticks = 6;
    SetSkill(data.skills[7], AbilityHandlerId::ForwardRollLeaveTrap, 480, 1.2f,
             0.0f, 12.0f, 0.0f, 3.0f, 720, 1, 0);
    data.skills[7].forward_roll_distance = 5.0f;
    data.skills[7].forward_roll_duration_ticks = 15;
    data.skills[7].activation_delay_ticks = 36;
    data.skills[7].active_duration_ticks = 720;
    data.skills[7].detection_radius = 2.0f;
    data.skills[7].slow_fraction = 0.3f;
    data.skills[7].slow_duration_ticks = 180;
    SetSkill(data.skills[8], AbilityHandlerId::ForcedRetreatAndProjectile, 420, 3.5f,
             32.0f, 16.0f, 0.44f, 0.0f, 12, 1, 3);
    data.skills[8].forced_move_duration_ticks = 12;
    data.skills[8].forced_move_distance = 5.0f;
    data.skills[8].projectile_range = 16.0f;

    data.enemies = {
        EnemyDefinition{15, 1.445f, 10, 1.0f, 21, 72, 0.0f, 0.0f, 0.45f, 0.0f,
                        0.0f, 0.0f, 0.0f},
        EnemyDefinition{12, 1.19f, 8, 12.0f, 30, 147, 6.875f, 18.0f, 0.45f,
                        0.25f, 0.25f, 0.0f, 0.0f},
        EnemyDefinition{13, 3.825f, 25, 2.2f, 48, 0, 0.0f, 3.0f, 0.45f, 0.0f,
                        0.0f, 2.2f, 3.0f},
    };

    data.bosses[0].health = 600;
    data.bosses[0].recovery_ticks = 108;
    data.bosses[0].phase2_pattern_interval_ticks = 0;
    data.bosses[0].phase2_cycle_recovery_ticks = 0;
    data.bosses[0].spawn_growth_ticks = 18'000;
    data.bosses[0].target_kill_ticks = 3'600;
    data.bosses[0].movement_speed = 2.4f;
    data.bosses[0].collision_radius = 1.1f;
    data.bosses[0].projectile_range = 24.0f;
    data.bosses[0].projectile_collision_radius = 0.25f;
    data.bosses[0].preferred_distance_near = 7.0f;
    data.bosses[0].preferred_distance_far = 7.0f;
    data.bosses[0].patterns[0] = {BossPatternLogic::LineCharge, 1, 54, 18.0f, 12.0f, 18};
    data.bosses[0].patterns[1] = {BossPatternLogic::ExpandingShockwaveWithSafeGaps, 1,
                                  81, 0.0f, 0.0f, 15, 3.0f, 9.75f, 4, 25.0f,
                                  45, 0.75f};
    data.bosses[0].pattern_count = 2;
    data.bosses[0].reward = {1, 0.25f};

    data.bosses[1].health = 1'375;
    data.bosses[1].recovery_ticks = 90;
    data.bosses[1].phase2_pattern_interval_ticks = 0;
    data.bosses[1].phase2_cycle_recovery_ticks = 0;
    data.bosses[1].spawn_growth_ticks = 36'000;
    data.bosses[1].target_kill_ticks = 3'600;
    data.bosses[1].movement_speed = 4.8f;
    data.bosses[1].collision_radius = 1.1f;
    data.bosses[1].projectile_range = 24.0f;
    data.bosses[1].projectile_collision_radius = 0.25f;
    data.bosses[1].preferred_distance_near = 12.0f;
    data.bosses[1].preferred_distance_far = 18.0f;
    data.bosses[1].patterns[0].logic = BossPatternLogic::FanProjectiles;
    data.bosses[1].patterns[0].phase = 1;
    data.bosses[1].patterns[0].telegraph_duration_ticks = 36;
    data.bosses[1].patterns[0].damage = 12;
    data.bosses[1].patterns[0].fan_angle_degrees = 70.0f;
    data.bosses[1].patterns[0].projectile_count = 7;
    data.bosses[1].patterns[0].projectile_speed = 4.5f;
    data.bosses[1].patterns[1].logic = BossPatternLogic::PredictedPositionGroundAreas;
    data.bosses[1].patterns[1].phase = 1;
    data.bosses[1].patterns[1].telegraph_duration_ticks = 60;
    data.bosses[1].patterns[1].area_count = 3;
    data.bosses[1].patterns[1].radius = 2.2f;
    data.bosses[1].patterns[1].duration_ticks = 180;
    data.bosses[1].patterns[1].tick_interval_ticks = 30;
    data.bosses[1].patterns[1].damage_per_tick = 6;
    data.bosses[1].patterns[1].prediction_lead_ticks = 45;
    data.bosses[1].patterns[1].ground_placement_radius = 2.2f;
    data.bosses[1].pattern_count = 2;
    data.bosses[1].reward = {1, 0.25f};

    data.bosses[2].health = 4'500;
    data.bosses[2].recovery_ticks = 72;
    data.bosses[2].phase2_pattern_interval_ticks = 72;
    data.bosses[2].phase2_cycle_recovery_ticks = 120;
    data.bosses[2].spawn_growth_ticks = 54'000;
    data.bosses[2].target_kill_ticks = 9'000;
    data.bosses[2].movement_speed = 2.4f;
    data.bosses[2].collision_radius = 1.1f;
    data.bosses[2].projectile_range = 24.0f;
    data.bosses[2].projectile_collision_radius = 0.25f;
    data.bosses[2].preferred_distance_near = 7.0f;
    data.bosses[2].preferred_distance_far = 14.0f;
    data.bosses[2].phase_two_preferred_distance_near = 7.0f;
    data.bosses[2].phase_two_preferred_distance_far = 14.0f;
    data.bosses[2].patterns[0] = {BossPatternLogic::LineCharge, 1, 54, 20.0f, 14.0f, 25};
    data.bosses[2].patterns[1].logic = BossPatternLogic::FanProjectiles;
    data.bosses[2].patterns[1].phase = 1;
    data.bosses[2].patterns[1].telegraph_duration_ticks = 42;
    data.bosses[2].patterns[1].damage = 14;
    data.bosses[2].patterns[1].fan_angle_degrees = 90.0f;
    data.bosses[2].patterns[1].projectile_count = 9;
    data.bosses[2].patterns[1].projectile_speed = 5.0f;
    data.bosses[2].patterns[2] = {BossPatternLogic::ExpandingShockwaveWithSafeGaps, 1,
                                  54, 0.0f, 0.0f, 20, 4.0f, 14.0f, 4, 25.0f,
                                  45, 0.75f};
    data.bosses[2].patterns[3].logic = BossPatternLogic::DoubleRetargetedCharge;
    data.bosses[2].patterns[3].phase = 2;
    data.bosses[2].patterns[3].telegraph_duration_ticks = 36;
    data.bosses[2].patterns[3].distance = 20.0f;
    data.bosses[2].patterns[3].speed = 14.0f;
    data.bosses[2].patterns[3].damage = 25;
    data.bosses[2].patterns[3].charge_count = 2;
    data.bosses[2].patterns[3].interval_ticks = 24;
    data.bosses[2].patterns[4].logic = BossPatternLogic::DoubleOffsetFanProjectiles;
    data.bosses[2].patterns[4].phase = 2;
    data.bosses[2].patterns[4].telegraph_duration_ticks = 36;
    data.bosses[2].patterns[4].damage = 14;
    data.bosses[2].patterns[4].projectile_speed = 5.0f;
    data.bosses[2].patterns[4].interval_ticks = 21;
    data.bosses[2].patterns[4].volley_count = 2;
    data.bosses[2].patterns[4].projectiles_per_volley = 11;
    data.bosses[2].patterns[4].second_volley_angle_offset_degrees = 8.0f;
    data.bosses[2].patterns[5].logic = BossPatternLogic::PredictedPositionGroundAreas;
    data.bosses[2].patterns[5].phase = 2;
    data.bosses[2].patterns[5].telegraph_duration_ticks = 48;
    data.bosses[2].patterns[5].area_count = 5;
    data.bosses[2].patterns[5].radius = 2.5f;
    data.bosses[2].patterns[5].duration_ticks = 240;
    data.bosses[2].patterns[5].tick_interval_ticks = 30;
    data.bosses[2].patterns[5].damage_per_tick = 10;
    data.bosses[2].patterns[5].ground_placement_radius = 3.0f;
    data.bosses[2].pattern_count = 6;
    data.bosses[2].phase_transition = {0.5f, 90, true, true, true, true, true, true, true};
    data.bosses[2].has_phase_transition = true;

    data.spawn_stages = {
        SpawnStage{0, 0, 0.9f, {100, 0, 0}},
        SpawnStage{2, 7'200, 1.2f, {80, 20, 0}},
        SpawnStage{4, 14'400, 2.1f, {74, 21, 5}},
        SpawnStage{6, 21'600, 3.3f, {65, 27, 8}},
        SpawnStage{9, 32'400, 4.8f, {56, 34, 10}},
        SpawnStage{12, 43'200, 4.8f, {52, 35, 13}},
        SpawnStage{14, 50'400, 4.8f, {49, 36, 15}},
    };
    data.waves = {WaveDefinition{3, 10'800, 30, 1'200},
                  WaveDefinition{6, 21'600, 45, 1'200},
                  WaveDefinition{9, 32'400, 65, 1'200},
                  WaveDefinition{12, 43'200, 85, 1'200},
                  WaveDefinition{14, 50'400, 100, 1'200}};

    auto &r = data.relics;
    r.bleed_kill_heal = {0.005f, 60};
    r.burn_propagation = {5.0f, 0.4f, 2};
    r.kill_cooldown_surge = {10, 60};
    r.bleed_burn_explosion = {5.0f, 2.0f, 120};
    r.radial_basic_attack = {6, 8, 0.45f};
    r.basic_kill_tracker = {8.0f, 1.0f, 1};
    r.movement_echo = {6.0f, 60, 0.8f};
    r.alternating_skills = {240, 0.15f};
    r.different_skill_tracker = {120, 5.0f, 120};
    r.damage_knockback = {4.0f, 3.0f, 0.5f, 120, 6};
    r.once_revive = {0.5f, 60, 1};
    r.combat_hit_chain = {12, 30.0f, 4, 2.5f};

    auto &u = data.upgrades;
    u.basic_attack.third_attack_delayed = {3, 5, 0.7f};
    u.basic_attack.additional_pierce = {1};
    u.basic_attack.first_hit_split = {{-30.0f, 30.0f}, 2, 0.5f};
    u.basic_attack.direct_arrow_bleed = {3, 1};
    u.basic_attack.direct_arrow_burn = {4};
    u.basic_attack.first_hit_slow_and_cooldown = {0.2f, 60, 12};
    u.basic_attack.missed_arrow_reacquire = {1.0f, 1, 2.0f};
    u.basic_attack.post_active_three_arrow = {180, {-15.0f, 0.0f, 15.0f}, 3, 0.7f};
    u.piercing_shot.delayed_followup = {5, 5.0f};
    u.piercing_shot.range_end_split = {{-20.0f, 20.0f}, 2, 0.9f, 12.0f};
    u.piercing_shot.apply_bleed = {2};
    u.piercing_shot.damaging_slow_trail = {1.0f, 120, 30, 0.4f, 0.2f, 60};
    u.piercing_shot.per_three_pierces_perpendicular = {3, 2, 90.0f, 2.0f};
    u.piercing_shot.align_hit_normal_enemy = {3.0f};
    u.piercing_shot.apply_and_transfer_burn = {3};
    u.piercing_shot.cooldown_multiplier = {0.75f};
    u.multishot.delayed_second_fan = {15, 50.0f, 5, 1.5f};
    u.multishot.original_arrow_first_hit_split = {{-25.0f, 25.0f}, 2, 0.35f};
    u.multishot.additional_pierce_per_arrow = {1};
    u.multishot.rear_fan = {3, 1.0f, 30.0f};
    u.multishot.first_cast_hit_bleed = {1};
    u.multishot.apply_burn_and_transfer_arrow = {0.7f, 3};
    u.multishot.two_additional_outer_arrows = {2, {-31.25f, 31.25f}, 1.2f};
    u.multishot.missed_arrow_retarget = {0.55f, 1, 12.0f};
    u.charged_shot.extended_full_charge_explosion = {84, 6.0f, 5.0f, 2.0f};
    u.charged_shot.faster_charge = {0.65f};
    u.charged_shot.increase_minimum_charge_damage = {5.0f};
    u.charged_shot.additional_pierce = {4};
    u.charged_shot.charge_bleed_full_charge_rupture = {3, 5, 1.5f};
    u.charged_shot.boss_or_fifth_pierce_split = {3, {-35, -25, -15, -5, 5, 15, 25, 35}, 8, 3.5f};
    u.charged_shot.first_hit_burn_explosion = {3.5f, 2.0f};
    u.charged_shot.full_charge_multi_kill_cooldown_refund = {3, 0.4f};
    u.explosive_arrow.delayed_reexplosion = {18, 4.5f, 3.0f};
    u.explosive_arrow.three_delayed_satellite_bombs = {3, 120.0f, 1.5f, 9, 4.5f, 1.5f};
    u.explosive_arrow.eight_direction_fragments = {8, 1.0f, 2.5f, 1};
    u.explosive_arrow.explosion_leaves_burning_area = {3.0f, 240, 30, 0.35f};
    u.explosive_arrow.apply_bleed_and_blood_explosions = {2.0f, 1.2f, 8, 1};
    u.explosive_arrow.pre_explosion_pull = {15, 2.5f};
    u.explosive_arrow.direct_hit_mark_other_active_explosion = {3.0f, 4.0f, 240, 2.0f};
    u.explosive_arrow.cooldown_multiplier = {0.75f};
    u.ricochet_arrow.return_to_player_rehit = {1.0f, 5};
    u.ricochet_arrow.first_ricochet_branch_chain = {1.0f, 3};
    u.ricochet_arrow.apply_bleed_and_extend_ricochets = {1, 3};
    u.ricochet_arrow.apply_and_copy_burn_to_next_target = {1};
    u.ricochet_arrow.kill_small_arrows = {1.0f, 3, 9, 12.0f};
    u.ricochet_arrow.original_ricochet_kill_new_chain = {3, 2.4f, 12.0f};
    u.ricochet_arrow.ricochet_cooldown_reduction = {12, 60};
    u.ricochet_arrow.cooldown_multiplier = {0.85f};
    u.arrow_rain.delayed_forward_secondary_area = {60, 4.0f, 3.0f, 120, 0.5f};
    u.arrow_rain.first_damage_tick_pull = {2.0f};
    u.arrow_rain.third_area_hit_bleed = {2, 3};
    u.arrow_rain.apply_burn_and_create_fire_areas = {1.5f, 120, 30, 0.3f, 4};
    u.arrow_rain.tracking_arrow_each_damage_tick = {7.0f, 1.0f};
    u.arrow_rain.area_slow = {0.3f, 60};
    u.arrow_rain.area_kill_arrow_outside = {2.0f, 6};
    u.arrow_rain.extend_area_and_leave_slow = {300, 4, 180, 0.4f, 60};
    u.trap.roll_path_traps = {3, 0.9f, 1.5f};
    u.trap.landing_slow_area = {3.0f, 0.45f, 180};
    u.trap.single_reactivation = {120, 1};
    u.trap.trigger_bleed = {3};
    u.trap.trigger_burn_and_fire_area = {1.5f, 120, 30, 0.7f};
    u.trap.pre_explosion_pull_and_slow = {2.0f, 0.6f, 120};
    u.trap.trigger_mark_other_active_damage = {3.0f, 4.0f, 240, 2.0f};
    u.trap.trap_kill_small_trap = {1.5f, 2.0f, 36, 720, 5.0f, 3};
    u.retreat_shot.replace_with_three_arrows = {3, 0.9f, {-12.0f, 0.0f, 12.0f}};
    u.retreat_shot.small_trap_at_start = {3.0f, 2.5f, 2.5f, 36, 720};
    u.retreat_shot.slow_trail = {120, 0.35f, 2.5f, 60};
    u.retreat_shot.apply_bleed_and_tracking_arrow = {1.5f, 3, 8.0f, 1};
    u.retreat_shot.landing_damage_and_push = {5.0f, 3.0f, 4.0f};
    u.retreat_shot.next_other_active_cooldown_refund = {180, 0.3f};
    u.retreat_shot.boss_or_three_normal_hits_heal = {3, 0.05f};
    u.retreat_shot.delayed_second_retreat_and_arrow = {15, 2.5f, 2.0f};
    return data;
}

std::uint64_t SimulationRulesSchemaHash() noexcept
{
    return Fnv1a64("project_hs_simulation_rules_v3");
}

std::uint64_t SimulationRulesHash(const SimulationRules &rules) noexcept
{
    CanonicalHash hash;
    hash.Add(rules.version);
    hash.Add(rules.arena_half_extent);

    const auto &s = rules.stats;
    hash.Add(s.maximum_points_per_stat);
    hash.Add(s.base_maximum_hp);
    hash.Add(s.base_current_hp);
    hash.Add(s.base_attack_power);
    hash.Add(s.base_basic_attack_rate_per_second);
    hash.Add(s.base_movement_speed_mps);
    hash.Add(s.base_magnet_radius_m);
    hash.Add(s.base_global_cooldown_reduction);
    for (const auto &allocation : s.allocations)
    {
        hash.Add(allocation.amount_per_point);
        hash.Add(allocation.immediate_current_hp_restore_per_point);
    }
    hash.Add(s.combat.minimum_final_damage);
    hash.Add(s.combat.minimum_cooldown_ticks);
    hash.Add(s.combat.skill_cooldown_upgrade_multiplier);
    hash.Add(s.combat.maximum_basic_attacks_per_tick);
    hash.Add(s.combat.critical_hits);
    hash.Add(s.combat.armor);
    hash.Add(s.combat.hit_invulnerability);
    hash.Add(s.combat.player_hit_knockback);
    hash.Add(s.first_status_tick_delay_ticks);
    hash.Add(s.player_collision_radius);

    hash.Add(rules.statuses.first_tick_delay_ticks);
    hash.Add(rules.statuses.tick_interval_ticks);
    hash.Add(rules.statuses.bleed.maximum_stacks);
    hash.Add(rules.statuses.bleed.maximum_instances);
    hash.Add(rules.statuses.bleed.duration_ticks);
    hash.Add(rules.statuses.bleed.tick_interval_ticks);
    hash.Add(rules.statuses.bleed.attack_power_multiplier_per_tick);
    hash.Add(rules.statuses.burn.maximum_stacks);
    hash.Add(rules.statuses.burn.maximum_instances);
    hash.Add(rules.statuses.burn.duration_ticks);
    hash.Add(rules.statuses.burn.tick_interval_ticks);
    hash.Add(rules.statuses.burn.attack_power_multiplier_per_tick);

    const auto &g = rules.growth;
    hash.Add(g.required_xp_base);
    hash.Add(g.required_xp_linear);
    hash.Add(g.required_xp_quadratic);
    hash.Add(g.experience_pickup_speed);
    hash.Add(g.experience_pickup_radius);
    hash.Add(g.utility_pickup_base_chance);
    hash.Add(g.utility_pickup_miss_increment);
    hash.Add(g.heal_pickup_chance_multiplier);
    hash.Add(g.magnet_pickup_chance_multiplier);

    const auto &progression = rules.progression;
    for (const auto value : progression.enemy_xp) hash.Add(value);
    for (const auto value : progression.boss_xp) hash.Add(value);
    hash.Add(progression.required_xp_base);
    hash.Add(progression.required_xp_linear);
    hash.Add(progression.required_xp_quadratic);
    hash.Add(progression.card_candidate_count);
    hash.Add(progression.card_choose_count);
    hash.Add(progression.active_slot_count);
    hash.Add(progression.upgrades_selected_per_skill);
    hash.Add(progression.base_stat_points_per_level);
    hash.Add(progression.fallback_extra_stat_points);
    hash.Add(progression.fallback_stat_point_cards);
    hash.Add(progression.level_initial_rerolls);
    hash.Add(progression.relic_initial_rerolls);
    for (const auto value : progression.boss_spawn_ticks) hash.Add(value);
    hash.Add(progression.stop_normal_spawns_at_final_boss);

    const auto &initial = rules.character_initial;
    hash.Add(initial.starting_level);
    hash.Add(initial.starting_basic_attack_level);
    hash.Add(initial.starting_unspent_stat_points);
    hash.Add(initial.starting_level_rerolls);
    hash.Add(initial.starting_relic_rerolls);
    hash.Add(initial.starting_active_skill_count);
    hash.Add(initial.starting_relic_count);
    for (const auto value : initial.starting_active_skill_ids) hash.Add(value);
    for (const auto value : initial.starting_relic_ids) hash.Add(value);
    hash.Add(initial.movement_stop_distance_m);

    hash.Add(rules.enemy_scaling.machine);
    hash.Add(rules.enemy_scaling.hp_fraction_per_completed_minute);
    hash.Add(rules.enemy_scaling.damage_fraction_per_completed_minute);
    hash.Add(rules.enemy_scaling.updates_existing_enemies);
    hash.Add(rules.enemy_scaling.applies_to_bosses);

    const auto &drop = rules.relic_drop;
    hash.Add(drop.guaranteed_boxes_per_mid_boss);
    hash.Add(drop.mid_boss_maximum_hp_heal_fraction);
    hash.Add(drop.normal_enemy_base_probability);
    hash.Add(drop.normal_enemy_probability_increment_per_kill);
    hash.Add(drop.normal_enemy_probability_cap);
    hash.Add(drop.maximum_choices_per_box);
    hash.Add(drop.healing_pickup_probability);
    hash.Add(drop.healing_pickup_maximum_hp_heal_fraction);
    hash.Add(drop.hard_pity);
    hash.Add(drop.reset_kill_counter_on_box_spawn);
    hash.Add(drop.stable_id_sort_before_seeded_shuffle);
    hash.Add(drop.stop_normal_box_rolls_after_all_acquired);
    hash.Add(drop.healing_pickup_independent_from_other_rewards);
    hash.Add(drop.healing_pickup_can_coexist_with_xp_and_relic_box);

    const auto &placement = rules.spawn_placement;
    hash.Add(placement.minimum_player_distance_m);
    hash.Add(placement.maximum_player_distance_m);
    hash.Add(placement.require_inside_arena);
    hash.Add(placement.require_outside_max_zoom_view);
    hash.Add(placement.fallback_warning_ticks);
    hash.Add(placement.fallback_location);

    const auto &common = rules.boss_common;
    hash.Add(common.collision_radius);
    hash.Add(common.spawn_warning_ticks);
    hash.Add(common.initial_pattern_delay_ticks);
    hash.Add(common.preferred_pattern_weight);
    hash.Add(common.other_pattern_weight);
    hash.Add(common.maximum_same_pattern_repeats);
    hash.Add(common.body_contact_damage);
    hash.Add(common.time_scaling);
    hash.Add(common.affected_by_damage);
    hash.Add(common.affected_by_bleed);
    hash.Add(common.affected_by_burn);
    hash.Add(common.affected_by_slow);
    hash.Add(common.immune_to_pull);
    hash.Add(common.immune_to_push);
    hash.Add(common.immune_to_alignment_move);
    hash.Add(common.projectile_range);
    hash.Add(common.projectile_collision_radius);

    HashUpgrades(hash, rules.upgrades);
    AddArray(hash, rules.skills, [](CanonicalHash &output, const SkillDefinition &skill) {
        HashSkill(output, skill);
    });
    AddArray(hash, rules.enemies, [](CanonicalHash &output, const EnemyDefinition &enemy) {
        output.Add(enemy.health);
        output.Add(enemy.move_speed);
        output.Add(enemy.damage);
        output.Add(enemy.attack_range);
        output.Add(enemy.warning_ticks);
        output.Add(enemy.attack_cooldown_ticks);
        output.Add(enemy.projectile_speed);
        output.Add(enemy.projectile_range);
        output.Add(enemy.collision_radius);
        output.Add(enemy.projectile_collision_radius);
        output.Add(enemy.ranged_projectile_radius);
        output.Add(enemy.suicide_stop_distance);
        output.Add(enemy.suicide_explosion_radius);
    });
    AddArray(hash, rules.bosses, [](CanonicalHash &output, const BossDefinition &boss) {
        output.Add(boss.health);
        output.Add(boss.recovery_ticks);
        output.Add(boss.phase2_pattern_interval_ticks);
        output.Add(boss.phase2_cycle_recovery_ticks);
        output.Add(boss.spawn_growth_ticks);
        output.Add(boss.target_kill_ticks);
        output.Add(boss.movement_speed);
        output.Add(boss.collision_radius);
        output.Add(boss.projectile_range);
        output.Add(boss.projectile_collision_radius);
        output.Add(boss.preferred_distance_near);
        output.Add(boss.preferred_distance_far);
        output.Add(boss.phase_two_preferred_distance_near);
        output.Add(boss.phase_two_preferred_distance_far);
        for (const auto &pattern : boss.patterns) HashPattern(output, pattern);
        output.Add(boss.pattern_count);
        HashBossTransition(output, boss.phase_transition);
        output.Add(boss.has_phase_transition);
        output.Add(boss.reward.relic_chest_count);
        output.Add(boss.reward.maximum_hp_heal_fraction);
    });
    AddArray(hash, rules.spawn_stages,
             [](CanonicalHash &output, const SpawnStage &stage) {
                 output.Add(stage.start_minute);
                 output.Add(stage.start_tick);
                 output.Add(stage.per_second);
                 for (const auto weight : stage.weights) output.Add(weight);
             });
    AddArray(hash, rules.waves, [](CanonicalHash &output, const WaveDefinition &wave) {
        output.Add(wave.minute);
        output.Add(wave.start_tick);
        output.Add(wave.count);
        output.Add(wave.duration_ticks);
    });
    HashRelics(hash, rules.relics);
    return hash.Value();
}

Result LoadSimulationRules(const std::filesystem::path &path,
                           SimulationRules &rules,
                           std::uint64_t *content_hash)
{
    static_assert(std::is_trivially_copyable_v<SimulationRules>);
    static_assert(std::is_standard_layout_v<SimulationRules>);
    CookedHeader header;
    std::vector<std::byte> payload;
    if (auto result = ReadCookedPayload(path, SimulationRulesSchemaHash(), header, payload); !result)
    {
        return result;
    }
    if (payload.size() != sizeof(SimulationRules))
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay",
                               "Cooked game data has an unexpected table size.");
    }
    std::memcpy(&rules, payload.data(), sizeof(rules));
    if (rules.version != 5)
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay",
                               "Cooked game data version is unsupported.");
    }
    if (content_hash)
    {
        *content_hash = header.source_hash;
    }
    return Result::Success();
}

} // namespace hs
