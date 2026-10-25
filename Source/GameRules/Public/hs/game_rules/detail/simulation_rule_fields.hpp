#pragma once

#include <hs/game_rules/simulation_rules.hpp>

namespace hs::detail
{
template <typename Visitor>
void VisitSimulationRuleFields(const Float2 &value, Visitor &&visit)
{
    visit("x", value.x);
    visit("y", value.y);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ArenaBoundary &value, Visitor &&visit)
{
    visit("count", value.count);
    visit("points", value.points);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ArenaObstacle2D &value, Visitor &&visit)
{
    visit("kind", value.kind);
    visit("center", value.center);
    visit("radius", value.radius);
}

template <typename Visitor>
void VisitSimulationRuleFields(const SkillDefinition &value, Visitor &&visit)
{
    visit("cooldown_ticks", value.cooldown_ticks);
    visit("damage_coefficient", value.damage_coefficient);
    visit("projectile_speed", value.projectile_speed);
    visit("range", value.range);
    visit("collision_radius", value.collision_radius);
    visit("area_radius", value.area_radius);
    visit("duration_ticks", value.duration_ticks);
    visit("projectile_count", value.projectile_count);
    visit("pierce_count", value.pierce_count);
    visit("handler", value.handler);
    visit("damage_multiplier", value.damage_multiplier);
    visit("fan_angle_degrees", value.fan_angle_degrees);
    visit("damage_multiplier_per_arrow", value.damage_multiplier_per_arrow);
    visit("pierce_per_arrow", value.pierce_per_arrow);
    visit("maximum_hits_per_target", value.maximum_hits_per_target);
    visit("maximum_charge_time_ticks", value.maximum_charge_time_ticks);
    visit("minimum_damage_multiplier", value.minimum_damage_multiplier);
    visit("maximum_damage_multiplier", value.maximum_damage_multiplier);
    visit("minimum_range", value.minimum_range);
    visit("maximum_range", value.maximum_range);
    visit("minimum_collision_radius", value.minimum_collision_radius);
    visit("maximum_collision_radius", value.maximum_collision_radius);
    visit("movement_speed_multiplier_while_charging", value.movement_speed_multiplier_while_charging);
    visit("pierce_damage_decay_fraction", value.pierce_damage_decay_fraction);
    visit("minimum_damage_fraction", value.minimum_damage_fraction);
    visit("explosion_damage_multiplier", value.explosion_damage_multiplier);
    visit("explosion_radius", value.explosion_radius);
    visit("initial_range", value.initial_range);
    visit("ricochet_search_radius", value.ricochet_search_radius);
    visit("maximum_ricochets", value.maximum_ricochets);
    visit("target_range", value.target_range);
    visit("activation_delay_ticks", value.activation_delay_ticks);
    visit("tick_interval_ticks", value.tick_interval_ticks);
    visit("total_damage_ticks", value.total_damage_ticks);
    visit("forward_roll_duration_ticks", value.forward_roll_duration_ticks);
    visit("forward_roll_distance", value.forward_roll_distance);
    visit("active_duration_ticks", value.active_duration_ticks);
    visit("detection_radius", value.detection_radius);
    visit("slow_fraction", value.slow_fraction);
    visit("slow_duration_ticks", value.slow_duration_ticks);
    visit("forced_move_distance", value.forced_move_distance);
    visit("forced_move_duration_ticks", value.forced_move_duration_ticks);
    visit("projectile_range", value.projectile_range);
    visit("starting_level", value.starting_level);
    visit("maximum_level", value.maximum_level);
    visit("selected_upgrades_per_session", value.selected_upgrades_per_session);
}

template <typename Visitor>
void VisitSimulationRuleFields(const EnemyDefinition &value, Visitor &&visit)
{
    visit("health", value.health);
    visit("move_speed", value.move_speed);
    visit("damage", value.damage);
    visit("attack_range", value.attack_range);
    visit("warning_ticks", value.warning_ticks);
    visit("attack_cooldown_ticks", value.attack_cooldown_ticks);
    visit("projectile_speed", value.projectile_speed);
    visit("projectile_range", value.projectile_range);
    visit("collision_radius", value.collision_radius);
    visit("projectile_collision_radius", value.projectile_collision_radius);
    visit("ranged_projectile_radius", value.ranged_projectile_radius);
    visit("suicide_stop_distance", value.suicide_stop_distance);
    visit("suicide_explosion_radius", value.suicide_explosion_radius);
}

template <typename Visitor>
void VisitSimulationRuleFields(const StatAllocationDefinition &value, Visitor &&visit)
{
    visit("amount_per_point", value.amount_per_point);
    visit("immediate_current_hp_restore_per_point", value.immediate_current_hp_restore_per_point);
}

template <typename Visitor>
void VisitSimulationRuleFields(const CombatRules &value, Visitor &&visit)
{
    visit("minimum_final_damage", value.minimum_final_damage);
    visit("minimum_cooldown_ticks", value.minimum_cooldown_ticks);
    visit("skill_cooldown_upgrade_multiplier", value.skill_cooldown_upgrade_multiplier);
    visit("maximum_basic_attacks_per_tick", value.maximum_basic_attacks_per_tick);
    visit("critical_hits", value.critical_hits);
    visit("armor", value.armor);
    visit("hit_invulnerability", value.hit_invulnerability);
    visit("player_hit_knockback", value.player_hit_knockback);
}

template <typename Visitor>
void VisitSimulationRuleFields(const StatRules &value, Visitor &&visit)
{
    visit("maximum_points_per_stat", value.maximum_points_per_stat);
    visit("base_maximum_hp", value.base_maximum_hp);
    visit("base_current_hp", value.base_current_hp);
    visit("base_attack_power", value.base_attack_power);
    visit("base_basic_attack_rate_per_second", value.base_basic_attack_rate_per_second);
    visit("base_movement_speed_mps", value.base_movement_speed_mps);
    visit("base_magnet_radius_m", value.base_magnet_radius_m);
    visit("base_global_cooldown_reduction", value.base_global_cooldown_reduction);
    visit("allocations", value.allocations);
    visit("combat", value.combat);
    visit("first_status_tick_delay_ticks", value.first_status_tick_delay_ticks);
    visit("player_collision_radius", value.player_collision_radius);
}

template <typename Visitor>
void VisitSimulationRuleFields(const StatusDefinition &value, Visitor &&visit)
{
    visit("maximum_stacks", value.maximum_stacks);
    visit("maximum_instances", value.maximum_instances);
    visit("duration_ticks", value.duration_ticks);
    visit("tick_interval_ticks", value.tick_interval_ticks);
    visit("attack_power_multiplier_per_tick", value.attack_power_multiplier_per_tick);
}

template <typename Visitor>
void VisitSimulationRuleFields(const StatusRules &value, Visitor &&visit)
{
    visit("first_tick_delay_ticks", value.first_tick_delay_ticks);
    visit("tick_interval_ticks", value.tick_interval_ticks);
    visit("bleed", value.bleed);
    visit("burn", value.burn);
}

template <typename Visitor>
void VisitSimulationRuleFields(const GrowthRules &value, Visitor &&visit)
{
    visit("required_xp_base", value.required_xp_base);
    visit("required_xp_linear", value.required_xp_linear);
    visit("required_xp_quadratic", value.required_xp_quadratic);
    visit("experience_pickup_speed", value.experience_pickup_speed);
    visit("experience_pickup_radius", value.experience_pickup_radius);
    visit("utility_pickup_base_chance", value.utility_pickup_base_chance);
    visit("utility_pickup_miss_increment", value.utility_pickup_miss_increment);
    visit("heal_pickup_chance_multiplier", value.heal_pickup_chance_multiplier);
    visit("magnet_pickup_chance_multiplier", value.magnet_pickup_chance_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ProgressionRules &value, Visitor &&visit)
{
    visit("enemy_xp", value.enemy_xp);
    visit("boss_xp", value.boss_xp);
    visit("required_xp_base", value.required_xp_base);
    visit("required_xp_linear", value.required_xp_linear);
    visit("required_xp_quadratic", value.required_xp_quadratic);
    visit("card_candidate_count", value.card_candidate_count);
    visit("card_choose_count", value.card_choose_count);
    visit("active_slot_count", value.active_slot_count);
    visit("upgrades_selected_per_skill", value.upgrades_selected_per_skill);
    visit("base_stat_points_per_level", value.base_stat_points_per_level);
    visit("fallback_extra_stat_points", value.fallback_extra_stat_points);
    visit("fallback_stat_point_cards", value.fallback_stat_point_cards);
    visit("level_initial_rerolls", value.level_initial_rerolls);
    visit("relic_initial_rerolls", value.relic_initial_rerolls);
    visit("boss_spawn_ticks", value.boss_spawn_ticks);
    visit("stop_normal_spawns_at_final_boss", value.stop_normal_spawns_at_final_boss);
}

template <typename Visitor>
void VisitSimulationRuleFields(const CharacterInitial &value, Visitor &&visit)
{
    visit("starting_level", value.starting_level);
    visit("starting_basic_attack_level", value.starting_basic_attack_level);
    visit("starting_unspent_stat_points", value.starting_unspent_stat_points);
    visit("starting_level_rerolls", value.starting_level_rerolls);
    visit("starting_relic_rerolls", value.starting_relic_rerolls);
    visit("starting_active_skill_count", value.starting_active_skill_count);
    visit("starting_relic_count", value.starting_relic_count);
    visit("starting_active_skill_ids", value.starting_active_skill_ids);
    visit("starting_relic_ids", value.starting_relic_ids);
    visit("movement_stop_distance_m", value.movement_stop_distance_m);
}

template <typename Visitor>
void VisitSimulationRuleFields(const EnemyScaling &value, Visitor &&visit)
{
    visit("machine", value.machine);
    visit("hp_fraction_per_completed_minute", value.hp_fraction_per_completed_minute);
    visit("damage_fraction_per_completed_minute", value.damage_fraction_per_completed_minute);
    visit("updates_existing_enemies", value.updates_existing_enemies);
    visit("applies_to_bosses", value.applies_to_bosses);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDropRules &value, Visitor &&visit)
{
    visit("guaranteed_boxes_per_mid_boss", value.guaranteed_boxes_per_mid_boss);
    visit("mid_boss_maximum_hp_heal_fraction", value.mid_boss_maximum_hp_heal_fraction);
    visit("normal_enemy_base_probability", value.normal_enemy_base_probability);
    visit("normal_enemy_probability_increment_per_kill", value.normal_enemy_probability_increment_per_kill);
    visit("normal_enemy_probability_cap", value.normal_enemy_probability_cap);
    visit("maximum_choices_per_box", value.maximum_choices_per_box);
    visit("healing_pickup_probability", value.healing_pickup_probability);
    visit("healing_pickup_maximum_hp_heal_fraction", value.healing_pickup_maximum_hp_heal_fraction);
    visit("hard_pity", value.hard_pity);
    visit("reset_kill_counter_on_box_spawn", value.reset_kill_counter_on_box_spawn);
    visit("stable_id_sort_before_seeded_shuffle", value.stable_id_sort_before_seeded_shuffle);
    visit("stop_normal_box_rolls_after_all_acquired", value.stop_normal_box_rolls_after_all_acquired);
    visit("healing_pickup_independent_from_other_rewards", value.healing_pickup_independent_from_other_rewards);
    visit("healing_pickup_can_coexist_with_xp_and_relic_box", value.healing_pickup_can_coexist_with_xp_and_relic_box);
}

template <typename Visitor>
void VisitSimulationRuleFields(const SpawnPlacement &value, Visitor &&visit)
{
    visit("minimum_player_distance_m", value.minimum_player_distance_m);
    visit("maximum_player_distance_m", value.maximum_player_distance_m);
    visit("require_inside_arena", value.require_inside_arena);
    visit("require_outside_max_zoom_view", value.require_outside_max_zoom_view);
    visit("fallback_warning_ticks", value.fallback_warning_ticks);
    visit("fallback_location", value.fallback_location);
    visit("max_zoom_view_min_forward_m", value.max_zoom_view_min_forward_m);
    visit("max_zoom_view_max_forward_m", value.max_zoom_view_max_forward_m);
    visit("max_zoom_view_half_right_m", value.max_zoom_view_half_right_m);
    visit("max_zoom_view_forward_x", value.max_zoom_view_forward_x);
    visit("max_zoom_view_forward_z", value.max_zoom_view_forward_z);
}

template <typename Visitor>
void VisitSimulationRuleFields(const BossPhaseTransition &value, Visitor &&visit)
{
    visit("hp_fraction", value.hp_fraction);
    visit("invulnerability_ticks", value.invulnerability_ticks);
    visit("remove_enemy_projectiles", value.remove_enemy_projectiles);
    visit("remove_enemy_areas", value.remove_enemy_areas);
    visit("keep_normal_enemies", value.keep_normal_enemies);
    visit("keep_mid_bosses", value.keep_mid_bosses);
    visit("keep_pickups", value.keep_pickups);
    visit("keep_player_projectiles", value.keep_player_projectiles);
    visit("keep_player_areas", value.keep_player_areas);
}

template <typename Visitor>
void VisitSimulationRuleFields(const BossReward &value, Visitor &&visit)
{
    visit("relic_chest_count", value.relic_chest_count);
    visit("maximum_hp_heal_fraction", value.maximum_hp_heal_fraction);
}

template <typename Visitor>
void VisitSimulationRuleFields(const BossPatternDefinition &value, Visitor &&visit)
{
    visit("logic", value.logic);
    visit("phase", value.phase);
    visit("telegraph_duration_ticks", value.telegraph_duration_ticks);
    visit("distance", value.distance);
    visit("speed", value.speed);
    visit("damage", value.damage);
    visit("start_radius", value.start_radius);
    visit("end_radius", value.end_radius);
    visit("safe_gap_count", value.safe_gap_count);
    visit("safe_gap_angle_degrees", value.safe_gap_angle_degrees);
    visit("shockwave_duration_ticks", value.shockwave_duration_ticks);
    visit("shockwave_half_width", value.shockwave_half_width);
    visit("fan_angle_degrees", value.fan_angle_degrees);
    visit("projectile_count", value.projectile_count);
    visit("projectile_speed", value.projectile_speed);
    visit("area_count", value.area_count);
    visit("radius", value.radius);
    visit("duration_ticks", value.duration_ticks);
    visit("tick_interval_ticks", value.tick_interval_ticks);
    visit("damage_per_tick", value.damage_per_tick);
    visit("charge_count", value.charge_count);
    visit("interval_ticks", value.interval_ticks);
    visit("volley_count", value.volley_count);
    visit("projectiles_per_volley", value.projectiles_per_volley);
    visit("second_volley_angle_offset_degrees", value.second_volley_angle_offset_degrees);
    visit("prediction_lead_ticks", value.prediction_lead_ticks);
    visit("ground_placement_radius", value.ground_placement_radius);
}

template <typename Visitor>
void VisitSimulationRuleFields(const BossCommonRules &value, Visitor &&visit)
{
    visit("collision_radius", value.collision_radius);
    visit("spawn_warning_ticks", value.spawn_warning_ticks);
    visit("initial_pattern_delay_ticks", value.initial_pattern_delay_ticks);
    visit("preferred_pattern_weight", value.preferred_pattern_weight);
    visit("other_pattern_weight", value.other_pattern_weight);
    visit("maximum_same_pattern_repeats", value.maximum_same_pattern_repeats);
    visit("body_contact_damage", value.body_contact_damage);
    visit("time_scaling", value.time_scaling);
    visit("affected_by_damage", value.affected_by_damage);
    visit("affected_by_bleed", value.affected_by_bleed);
    visit("affected_by_burn", value.affected_by_burn);
    visit("affected_by_slow", value.affected_by_slow);
    visit("immune_to_pull", value.immune_to_pull);
    visit("immune_to_push", value.immune_to_push);
    visit("immune_to_alignment_move", value.immune_to_alignment_move);
    visit("projectile_range", value.projectile_range);
    visit("projectile_collision_radius", value.projectile_collision_radius);
}

template <typename Visitor>
void VisitSimulationRuleFields(const BossDefinition &value, Visitor &&visit)
{
    visit("health", value.health);
    visit("recovery_ticks", value.recovery_ticks);
    visit("phase2_pattern_interval_ticks", value.phase2_pattern_interval_ticks);
    visit("phase2_cycle_recovery_ticks", value.phase2_cycle_recovery_ticks);
    visit("spawn_growth_ticks", value.spawn_growth_ticks);
    visit("target_kill_ticks", value.target_kill_ticks);
    visit("movement_speed", value.movement_speed);
    visit("collision_radius", value.collision_radius);
    visit("projectile_range", value.projectile_range);
    visit("projectile_collision_radius", value.projectile_collision_radius);
    visit("preferred_distance_near", value.preferred_distance_near);
    visit("preferred_distance_far", value.preferred_distance_far);
    visit("phase_two_preferred_distance_near", value.phase_two_preferred_distance_near);
    visit("phase_two_preferred_distance_far", value.phase_two_preferred_distance_far);
    visit("patterns", value.patterns);
    visit("pattern_count", value.pattern_count);
    visit("phase_transition", value.phase_transition);
    visit("has_phase_transition", value.has_phase_transition);
    visit("reward", value.reward);
}

template <typename Visitor>
void VisitSimulationRuleFields(const SpawnStage &value, Visitor &&visit)
{
    visit("start_minute", value.start_minute);
    visit("start_tick", value.start_tick);
    visit("per_second", value.per_second);
    visit("weights", value.weights);
}

template <typename Visitor>
void VisitSimulationRuleFields(const WaveDefinition &value, Visitor &&visit)
{
    visit("minute", value.minute);
    visit("start_tick", value.start_tick);
    visit("count", value.count);
    visit("duration_ticks", value.duration_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDefinitions &value, Visitor &&visit)
{
    visit("bleed_kill_heal", value.bleed_kill_heal);
    visit("burn_propagation", value.burn_propagation);
    visit("kill_cooldown_surge", value.kill_cooldown_surge);
    visit("bleed_burn_explosion", value.bleed_burn_explosion);
    visit("radial_basic_attack", value.radial_basic_attack);
    visit("basic_kill_tracker", value.basic_kill_tracker);
    visit("movement_echo", value.movement_echo);
    visit("alternating_skills", value.alternating_skills);
    visit("different_skill_tracker", value.different_skill_tracker);
    visit("damage_knockback", value.damage_knockback);
    visit("once_revive", value.once_revive);
    visit("combat_hit_chain", value.combat_hit_chain);
    visit("projectile_cadence_reward", value.projectile_cadence_reward);
    visit("pre_damage_guard", value.pre_damage_guard);
    visit("slow_synergy", value.slow_synergy);
    visit("area_resonance", value.area_resonance);
    visit("boss_pressure", value.boss_pressure);
    visit("hit_streak_reward", value.hit_streak_reward);
    visit("pickup_reward", value.pickup_reward);
    visit("low_health_survival", value.low_health_survival);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDefinitions::BleedKillHeal &value, Visitor &&visit)
{
    visit("maximum_hp_heal_fraction", value.maximum_hp_heal_fraction);
    visit("internal_cooldown_ticks", value.internal_cooldown_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDefinitions::BurnPropagation &value, Visitor &&visit)
{
    visit("search_radius", value.search_radius);
    visit("copied_burn_strength", value.copied_burn_strength);
    visit("maximum_targets", value.maximum_targets);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDefinitions::KillCooldownSurge &value, Visitor &&visit)
{
    visit("kills_per_trigger", value.kills_per_trigger);
    visit("cooldown_reduction_ticks", value.cooldown_reduction_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDefinitions::BleedBurnExplosion &value, Visitor &&visit)
{
    visit("radius", value.radius);
    visit("damage_multiplier", value.damage_multiplier);
    visit("per_target_cooldown_ticks", value.per_target_cooldown_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDefinitions::RadialBasicAttack &value, Visitor &&visit)
{
    visit("cadence_interval", value.cadence_interval);
    visit("direction_count", value.direction_count);
    visit("damage_multiplier", value.damage_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDefinitions::BasicKillTracker &value, Visitor &&visit)
{
    visit("search_radius", value.search_radius);
    visit("damage_multiplier", value.damage_multiplier);
    visit("maximum_triggers_per_attack", value.maximum_triggers_per_attack);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDefinitions::MovementEcho &value, Visitor &&visit)
{
    visit("required_distance", value.required_distance);
    visit("position_history_age_ticks", value.position_history_age_ticks);
    visit("damage_multiplier", value.damage_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDefinitions::AlternatingSkills &value, Visitor &&visit)
{
    visit("window_ticks", value.window_ticks);
    visit("cooldown_refund_fraction", value.cooldown_refund_fraction);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDefinitions::DifferentSkillTracker &value, Visitor &&visit)
{
    visit("window_ticks", value.window_ticks);
    visit("damage_multiplier", value.damage_multiplier);
    visit("per_target_cooldown_ticks", value.per_target_cooldown_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDefinitions::DamageKnockback &value, Visitor &&visit)
{
    visit("radius", value.radius);
    visit("push_distance", value.push_distance);
    visit("slow_fraction", value.slow_fraction);
    visit("slow_duration_ticks", value.slow_duration_ticks);
    visit("cooldown_ticks", value.cooldown_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDefinitions::OnceRevive &value, Visitor &&visit)
{
    visit("health_fraction", value.health_fraction);
    visit("invulnerability_ticks", value.invulnerability_ticks);
    visit("maximum_triggers_per_session", value.maximum_triggers_per_session);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDefinitions::CombatHitChain &value, Visitor &&visit)
{
    visit("direct_hits_per_trigger", value.direct_hits_per_trigger);
    visit("search_radius", value.search_radius);
    visit("maximum_targets", value.maximum_targets);
    visit("damage_multiplier", value.damage_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDefinitions::ProjectileCadenceReward &value, Visitor &&visit)
{
    visit("hits_per_trigger", value.hits_per_trigger);
    visit("cooldown_reduction_ticks", value.cooldown_reduction_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDefinitions::PreDamageGuard &value, Visitor &&visit)
{
    visit("damage_reduction_fraction", value.damage_reduction_fraction);
    visit("cooldown_ticks", value.cooldown_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDefinitions::SlowSynergy &value, Visitor &&visit)
{
    visit("damage_multiplier", value.damage_multiplier);
    visit("per_target_cooldown_ticks", value.per_target_cooldown_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDefinitions::AreaResonance &value, Visitor &&visit)
{
    visit("damage_multiplier", value.damage_multiplier);
    visit("cooldown_ticks", value.cooldown_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDefinitions::BossPressure &value, Visitor &&visit)
{
    visit("damage_multiplier", value.damage_multiplier);
    visit("per_target_cooldown_ticks", value.per_target_cooldown_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDefinitions::HitStreakReward &value, Visitor &&visit)
{
    visit("direct_hits_per_trigger", value.direct_hits_per_trigger);
    visit("damage_multiplier", value.damage_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDefinitions::PickupReward &value, Visitor &&visit)
{
    visit("attack_power_fraction", value.attack_power_fraction);
    visit("duration_ticks", value.duration_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RelicDefinitions::LowHealthSurvival &value, Visitor &&visit)
{
    visit("health_threshold_fraction", value.health_threshold_fraction);
    visit("damage_reduction_fraction", value.damage_reduction_fraction);
    visit("cooldown_ticks", value.cooldown_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const BasicAttackUpgrades &value, Visitor &&visit)
{
    visit("third_attack_delayed", value.third_attack_delayed);
    visit("additional_pierce", value.additional_pierce);
    visit("first_hit_split", value.first_hit_split);
    visit("direct_arrow_bleed", value.direct_arrow_bleed);
    visit("direct_arrow_burn", value.direct_arrow_burn);
    visit("first_hit_slow_and_cooldown", value.first_hit_slow_and_cooldown);
    visit("missed_arrow_reacquire", value.missed_arrow_reacquire);
    visit("post_active_three_arrow", value.post_active_three_arrow);
}

template <typename Visitor>
void VisitSimulationRuleFields(const BasicAttackUpgrades::ThirdAttackDelayed &value, Visitor &&visit)
{
    visit("cadence_interval", value.cadence_interval);
    visit("delay_ticks", value.delay_ticks);
    visit("damage_multiplier", value.damage_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const BasicAttackUpgrades::AdditionalPierce &value, Visitor &&visit)
{
    visit("additional_targets", value.additional_targets);
}

template <typename Visitor>
void VisitSimulationRuleFields(const BasicAttackUpgrades::FirstHitSplit &value, Visitor &&visit)
{
    visit("angles_degrees", value.angles_degrees);
    visit("projectile_count", value.projectile_count);
    visit("damage_multiplier", value.damage_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const BasicAttackUpgrades::DirectArrowBleed &value, Visitor &&visit)
{
    visit("direct_arrow_interval", value.direct_arrow_interval);
    visit("bleed_stacks", value.bleed_stacks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const BasicAttackUpgrades::DirectArrowBurn &value, Visitor &&visit)
{
    visit("direct_arrow_interval", value.direct_arrow_interval);
}

template <typename Visitor>
void VisitSimulationRuleFields(const BasicAttackUpgrades::FirstHitSlowAndCooldown &value, Visitor &&visit)
{
    visit("slow_fraction", value.slow_fraction);
    visit("slow_duration_ticks", value.slow_duration_ticks);
    visit("cooldown_reduction_ticks", value.cooldown_reduction_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const BasicAttackUpgrades::MissedArrowReacquire &value, Visitor &&visit)
{
    visit("damage_multiplier", value.damage_multiplier);
    visit("maximum_retargets", value.maximum_retargets);
    visit("collision_radius_multiplier", value.collision_radius_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const BasicAttackUpgrades::PostActiveThreeArrow &value, Visitor &&visit)
{
    visit("activation_window_ticks", value.activation_window_ticks);
    visit("angles_degrees", value.angles_degrees);
    visit("projectile_count", value.projectile_count);
    visit("damage_multiplier_per_arrow", value.damage_multiplier_per_arrow);
}

template <typename Visitor>
void VisitSimulationRuleFields(const PiercingShotUpgrades &value, Visitor &&visit)
{
    visit("delayed_followup", value.delayed_followup);
    visit("range_end_split", value.range_end_split);
    visit("apply_bleed", value.apply_bleed);
    visit("damaging_slow_trail", value.damaging_slow_trail);
    visit("per_three_pierces_perpendicular", value.per_three_pierces_perpendicular);
    visit("align_hit_normal_enemy", value.align_hit_normal_enemy);
    visit("apply_and_transfer_burn", value.apply_and_transfer_burn);
    visit("cooldown_multiplier", value.cooldown_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const PiercingShotUpgrades::DelayedFollowup &value, Visitor &&visit)
{
    visit("delay_ticks", value.delay_ticks);
    visit("damage_multiplier", value.damage_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const PiercingShotUpgrades::RangeEndSplit &value, Visitor &&visit)
{
    visit("angles_degrees", value.angles_degrees);
    visit("projectile_count", value.projectile_count);
    visit("damage_multiplier", value.damage_multiplier);
    visit("search_radius", value.search_radius);
}

template <typename Visitor>
void VisitSimulationRuleFields(const PiercingShotUpgrades::ApplyBleed &value, Visitor &&visit)
{
    visit("bleed_stacks", value.bleed_stacks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const PiercingShotUpgrades::DamagingSlowTrail &value, Visitor &&visit)
{
    visit("width", value.width);
    visit("duration_ticks", value.duration_ticks);
    visit("tick_interval_ticks", value.tick_interval_ticks);
    visit("damage_multiplier_per_tick", value.damage_multiplier_per_tick);
    visit("slow_fraction", value.slow_fraction);
    visit("slow_duration_ticks", value.slow_duration_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const PiercingShotUpgrades::PerThreePiercesPerpendicular &value, Visitor &&visit)
{
    visit("normal_enemy_pierce_interval", value.normal_enemy_pierce_interval);
    visit("projectile_count", value.projectile_count);
    visit("angle_from_direction", value.angle_from_direction);
    visit("damage_multiplier", value.damage_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const PiercingShotUpgrades::AlignHitNormalEnemy &value, Visitor &&visit)
{
    visit("move_distance", value.move_distance);
}

template <typename Visitor>
void VisitSimulationRuleFields(const PiercingShotUpgrades::ApplyAndTransferBurn &value, Visitor &&visit)
{
    visit("maximum_transfer_targets", value.maximum_transfer_targets);
}

template <typename Visitor>
void VisitSimulationRuleFields(const PiercingShotUpgrades::CooldownMultiplier &value, Visitor &&visit)
{
    visit("cooldown_multiplier", value.cooldown_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const MultishotUpgrades &value, Visitor &&visit)
{
    visit("delayed_second_fan", value.delayed_second_fan);
    visit("original_arrow_first_hit_split", value.original_arrow_first_hit_split);
    visit("additional_pierce_per_arrow", value.additional_pierce_per_arrow);
    visit("rear_fan", value.rear_fan);
    visit("first_cast_hit_bleed", value.first_cast_hit_bleed);
    visit("apply_burn_and_transfer_arrow", value.apply_burn_and_transfer_arrow);
    visit("two_additional_outer_arrows", value.two_additional_outer_arrows);
    visit("missed_arrow_retarget", value.missed_arrow_retarget);
}

template <typename Visitor>
void VisitSimulationRuleFields(const MultishotUpgrades::DelayedSecondFan &value, Visitor &&visit)
{
    visit("delay_ticks", value.delay_ticks);
    visit("fan_angle_degrees", value.fan_angle_degrees);
    visit("projectile_count", value.projectile_count);
    visit("damage_multiplier_per_arrow", value.damage_multiplier_per_arrow);
}

template <typename Visitor>
void VisitSimulationRuleFields(const MultishotUpgrades::OriginalArrowFirstHitSplit &value, Visitor &&visit)
{
    visit("angles_degrees", value.angles_degrees);
    visit("projectile_count_per_original", value.projectile_count_per_original);
    visit("damage_multiplier", value.damage_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const MultishotUpgrades::AdditionalPiercePerArrow &value, Visitor &&visit)
{
    visit("additional_pierce", value.additional_pierce);
}

template <typename Visitor>
void VisitSimulationRuleFields(const MultishotUpgrades::RearFan &value, Visitor &&visit)
{
    visit("projectile_count", value.projectile_count);
    visit("damage_multiplier", value.damage_multiplier);
    visit("fan_angle_degrees", value.fan_angle_degrees);
}

template <typename Visitor>
void VisitSimulationRuleFields(const MultishotUpgrades::FirstCastHitBleed &value, Visitor &&visit)
{
    visit("bleed_stacks", value.bleed_stacks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const MultishotUpgrades::ApplyBurnAndTransferArrow &value, Visitor &&visit)
{
    visit("damage_multiplier", value.damage_multiplier);
    visit("maximum_transfers_per_cast", value.maximum_transfers_per_cast);
}

template <typename Visitor>
void VisitSimulationRuleFields(const MultishotUpgrades::TwoAdditionalOuterArrows &value, Visitor &&visit)
{
    visit("additional_projectiles", value.additional_projectiles);
    visit("outer_angles_degrees", value.outer_angles_degrees);
    visit("damage_multiplier", value.damage_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const MultishotUpgrades::MissedArrowRetarget &value, Visitor &&visit)
{
    visit("damage_multiplier", value.damage_multiplier);
    visit("maximum_retargets", value.maximum_retargets);
    visit("search_radius", value.search_radius);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ChargedShotUpgrades &value, Visitor &&visit)
{
    visit("extended_full_charge_explosion", value.extended_full_charge_explosion);
    visit("faster_charge", value.faster_charge);
    visit("increase_minimum_charge_damage", value.increase_minimum_charge_damage);
    visit("additional_pierce", value.additional_pierce);
    visit("charge_bleed_full_charge_rupture", value.charge_bleed_full_charge_rupture);
    visit("boss_or_fifth_pierce_split", value.boss_or_fifth_pierce_split);
    visit("first_hit_burn_explosion", value.first_hit_burn_explosion);
    visit("full_charge_multi_kill_cooldown_refund", value.full_charge_multi_kill_cooldown_refund);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ChargedShotUpgrades::ExtendedFullChargeExplosion &value, Visitor &&visit)
{
    visit("maximum_charge_time_ticks", value.maximum_charge_time_ticks);
    visit("maximum_damage_multiplier", value.maximum_damage_multiplier);
    visit("full_charge_end_explosion_damage_multiplier", value.full_charge_end_explosion_damage_multiplier);
    visit("explosion_radius", value.explosion_radius);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ChargedShotUpgrades::FasterCharge &value, Visitor &&visit)
{
    visit("charge_time_multiplier", value.charge_time_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ChargedShotUpgrades::IncreaseMinimumChargeDamage &value, Visitor &&visit)
{
    visit("minimum_damage_multiplier", value.minimum_damage_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ChargedShotUpgrades::AdditionalPierce &value, Visitor &&visit)
{
    visit("additional_pierce", value.additional_pierce);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ChargedShotUpgrades::ChargeBleedFullChargeRupture &value, Visitor &&visit)
{
    visit("bleed_stacks", value.bleed_stacks);
    visit("existing_bleed_stacks_for_rupture", value.existing_bleed_stacks_for_rupture);
    visit("rupture_damage_multiplier", value.rupture_damage_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ChargedShotUpgrades::BossOrFifthPierceSplit &value, Visitor &&visit)
{
    visit("normal_enemy_pierce_count", value.normal_enemy_pierce_count);
    visit("angles_degrees", value.angles_degrees);
    visit("projectile_count", value.projectile_count);
    visit("damage_multiplier", value.damage_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ChargedShotUpgrades::FirstHitBurnExplosion &value, Visitor &&visit)
{
    visit("radius", value.radius);
    visit("damage_multiplier", value.damage_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ChargedShotUpgrades::FullChargeMultiKillCooldownRefund &value, Visitor &&visit)
{
    visit("minimum_normal_enemy_kills", value.minimum_normal_enemy_kills);
    visit("current_cooldown_refund_fraction", value.current_cooldown_refund_fraction);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ExplosiveArrowUpgrades &value, Visitor &&visit)
{
    visit("delayed_reexplosion", value.delayed_reexplosion);
    visit("three_delayed_satellite_bombs", value.three_delayed_satellite_bombs);
    visit("eight_direction_fragments", value.eight_direction_fragments);
    visit("explosion_leaves_burning_area", value.explosion_leaves_burning_area);
    visit("apply_bleed_and_blood_explosions", value.apply_bleed_and_blood_explosions);
    visit("pre_explosion_pull", value.pre_explosion_pull);
    visit("direct_hit_mark_other_active_explosion", value.direct_hit_mark_other_active_explosion);
    visit("cooldown_multiplier", value.cooldown_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ExplosiveArrowUpgrades::DelayedReexplosion &value, Visitor &&visit)
{
    visit("delay_ticks", value.delay_ticks);
    visit("radius", value.radius);
    visit("damage_multiplier", value.damage_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ExplosiveArrowUpgrades::ThreeDelayedSatelliteBombs &value, Visitor &&visit)
{
    visit("bomb_count", value.bomb_count);
    visit("angular_spacing", value.angular_spacing);
    visit("placement_radius", value.placement_radius);
    visit("delay_ticks", value.delay_ticks);
    visit("explosion_radius", value.explosion_radius);
    visit("damage_multiplier", value.damage_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ExplosiveArrowUpgrades::EightDirectionFragments &value, Visitor &&visit)
{
    visit("direction_count", value.direction_count);
    visit("damage_multiplier", value.damage_multiplier);
    visit("collision_radius_multiplier", value.collision_radius_multiplier);
    visit("pierce", value.pierce);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ExplosiveArrowUpgrades::ExplosionLeavesBurningArea &value, Visitor &&visit)
{
    visit("radius", value.radius);
    visit("duration_ticks", value.duration_ticks);
    visit("tick_interval_ticks", value.tick_interval_ticks);
    visit("damage_multiplier_per_tick", value.damage_multiplier_per_tick);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ExplosiveArrowUpgrades::ApplyBleedAndBloodExplosions &value, Visitor &&visit)
{
    visit("radius", value.radius);
    visit("damage_multiplier", value.damage_multiplier);
    visit("maximum_explosions_per_cast", value.maximum_explosions_per_cast);
    visit("bleed_stacks", value.bleed_stacks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ExplosiveArrowUpgrades::PreExplosionPull &value, Visitor &&visit)
{
    visit("duration_ticks", value.duration_ticks);
    visit("pull_radius", value.pull_radius);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ExplosiveArrowUpgrades::DirectHitMarkOtherActiveExplosion &value, Visitor &&visit)
{
    visit("immediate_explosion_damage_multiplier", value.immediate_explosion_damage_multiplier);
    visit("mark_explosion_damage_multiplier", value.mark_explosion_damage_multiplier);
    visit("mark_duration_ticks", value.mark_duration_ticks);
    visit("mark_explosion_radius", value.mark_explosion_radius);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ExplosiveArrowUpgrades::CooldownMultiplier &value, Visitor &&visit)
{
    visit("cooldown_multiplier", value.cooldown_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RicochetArrowUpgrades &value, Visitor &&visit)
{
    visit("return_to_player_rehit", value.return_to_player_rehit);
    visit("first_ricochet_branch_chain", value.first_ricochet_branch_chain);
    visit("apply_bleed_and_extend_ricochets", value.apply_bleed_and_extend_ricochets);
    visit("apply_and_copy_burn_to_next_target", value.apply_and_copy_burn_to_next_target);
    visit("kill_small_arrows", value.kill_small_arrows);
    visit("original_ricochet_kill_new_chain", value.original_ricochet_kill_new_chain);
    visit("ricochet_cooldown_reduction", value.ricochet_cooldown_reduction);
    visit("cooldown_multiplier", value.cooldown_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RicochetArrowUpgrades::ReturnToPlayerRehit &value, Visitor &&visit)
{
    visit("damage_multiplier", value.damage_multiplier);
    visit("maximum_rehit_targets", value.maximum_rehit_targets);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RicochetArrowUpgrades::FirstRicochetBranchChain &value, Visitor &&visit)
{
    visit("damage_multiplier", value.damage_multiplier);
    visit("maximum_targets", value.maximum_targets);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RicochetArrowUpgrades::ApplyBleedAndExtendRicochets &value, Visitor &&visit)
{
    visit("additional_ricochets_per_bleeding_hit", value.additional_ricochets_per_bleeding_hit);
    visit("maximum_additional_ricochets", value.maximum_additional_ricochets);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RicochetArrowUpgrades::ApplyAndCopyBurnToNextTarget &value, Visitor &&visit)
{
    visit("enabled", value.enabled);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RicochetArrowUpgrades::KillSmallArrows &value, Visitor &&visit)
{
    visit("damage_multiplier", value.damage_multiplier);
    visit("arrows_per_kill", value.arrows_per_kill);
    visit("maximum_arrows_per_cast", value.maximum_arrows_per_cast);
    visit("search_radius", value.search_radius);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RicochetArrowUpgrades::OriginalRicochetKillNewChain &value, Visitor &&visit)
{
    visit("new_chain_targets", value.new_chain_targets);
    visit("damage_multiplier", value.damage_multiplier);
    visit("search_radius", value.search_radius);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RicochetArrowUpgrades::RicochetCooldownReduction &value, Visitor &&visit)
{
    visit("cooldown_reduction_per_ricochet_ticks", value.cooldown_reduction_per_ricochet_ticks);
    visit("maximum_reduction_ticks", value.maximum_reduction_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RicochetArrowUpgrades::CooldownMultiplier &value, Visitor &&visit)
{
    visit("cooldown_multiplier", value.cooldown_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ArrowRainUpgrades &value, Visitor &&visit)
{
    visit("delayed_forward_secondary_area", value.delayed_forward_secondary_area);
    visit("first_damage_tick_pull", value.first_damage_tick_pull);
    visit("third_area_hit_bleed", value.third_area_hit_bleed);
    visit("apply_burn_and_create_fire_areas", value.apply_burn_and_create_fire_areas);
    visit("tracking_arrow_each_damage_tick", value.tracking_arrow_each_damage_tick);
    visit("area_slow", value.area_slow);
    visit("area_kill_arrow_outside", value.area_kill_arrow_outside);
    visit("extend_area_and_leave_slow", value.extend_area_and_leave_slow);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ArrowRainUpgrades::DelayedForwardSecondaryArea &value, Visitor &&visit)
{
    visit("delay_ticks", value.delay_ticks);
    visit("forward_offset", value.forward_offset);
    visit("radius", value.radius);
    visit("duration_ticks", value.duration_ticks);
    visit("damage_multiplier_per_tick", value.damage_multiplier_per_tick);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ArrowRainUpgrades::FirstDamageTickPull &value, Visitor &&visit)
{
    visit("pull_radius", value.pull_radius);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ArrowRainUpgrades::ThirdAreaHitBleed &value, Visitor &&visit)
{
    visit("hit_ordinal", value.hit_ordinal);
    visit("bleed_stacks", value.bleed_stacks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ArrowRainUpgrades::ApplyBurnAndCreateFireAreas &value, Visitor &&visit)
{
    visit("radius", value.radius);
    visit("duration_ticks", value.duration_ticks);
    visit("tick_interval_ticks", value.tick_interval_ticks);
    visit("damage_multiplier_per_tick", value.damage_multiplier_per_tick);
    visit("maximum_areas_per_cast", value.maximum_areas_per_cast);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ArrowRainUpgrades::TrackingArrowEachDamageTick &value, Visitor &&visit)
{
    visit("search_radius", value.search_radius);
    visit("damage_multiplier", value.damage_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ArrowRainUpgrades::AreaSlow &value, Visitor &&visit)
{
    visit("slow_fraction", value.slow_fraction);
    visit("slow_duration_ticks", value.slow_duration_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ArrowRainUpgrades::AreaKillArrowOutside &value, Visitor &&visit)
{
    visit("damage_multiplier", value.damage_multiplier);
    visit("maximum_triggers_per_cast", value.maximum_triggers_per_cast);
}

template <typename Visitor>
void VisitSimulationRuleFields(const ArrowRainUpgrades::ExtendAreaAndLeaveSlow &value, Visitor &&visit)
{
    visit("area_duration_ticks", value.area_duration_ticks);
    visit("additional_damage_ticks", value.additional_damage_ticks);
    visit("post_area_duration_ticks", value.post_area_duration_ticks);
    visit("post_area_slow_fraction", value.post_area_slow_fraction);
    visit("post_area_slow_duration_ticks", value.post_area_slow_duration_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const TrapUpgrades &value, Visitor &&visit)
{
    visit("roll_path_traps", value.roll_path_traps);
    visit("landing_slow_area", value.landing_slow_area);
    visit("single_reactivation", value.single_reactivation);
    visit("trigger_bleed", value.trigger_bleed);
    visit("trigger_burn_and_fire_area", value.trigger_burn_and_fire_area);
    visit("pre_explosion_pull_and_slow", value.pre_explosion_pull_and_slow);
    visit("trigger_mark_other_active_damage", value.trigger_mark_other_active_damage);
    visit("trap_kill_small_trap", value.trap_kill_small_trap);
}

template <typename Visitor>
void VisitSimulationRuleFields(const TrapUpgrades::RollPathTraps &value, Visitor &&visit)
{
    visit("trap_count", value.trap_count);
    visit("damage_multiplier_per_trap", value.damage_multiplier_per_trap);
    visit("spacing", value.spacing);
}

template <typename Visitor>
void VisitSimulationRuleFields(const TrapUpgrades::LandingSlowArea &value, Visitor &&visit)
{
    visit("radius", value.radius);
    visit("slow_fraction", value.slow_fraction);
    visit("duration_ticks", value.duration_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const TrapUpgrades::SingleReactivation &value, Visitor &&visit)
{
    visit("reactivation_delay_ticks", value.reactivation_delay_ticks);
    visit("maximum_reactivations", value.maximum_reactivations);
}

template <typename Visitor>
void VisitSimulationRuleFields(const TrapUpgrades::TriggerBleed &value, Visitor &&visit)
{
    visit("bleed_stacks", value.bleed_stacks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const TrapUpgrades::TriggerBurnAndFireArea &value, Visitor &&visit)
{
    visit("area_radius", value.area_radius);
    visit("area_duration_ticks", value.area_duration_ticks);
    visit("tick_interval_ticks", value.tick_interval_ticks);
    visit("damage_multiplier_per_tick", value.damage_multiplier_per_tick);
}

template <typename Visitor>
void VisitSimulationRuleFields(const TrapUpgrades::PreExplosionPullAndSlow &value, Visitor &&visit)
{
    visit("pull_radius", value.pull_radius);
    visit("slow_fraction", value.slow_fraction);
    visit("slow_duration_ticks", value.slow_duration_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const TrapUpgrades::TriggerMarkOtherActiveDamage &value, Visitor &&visit)
{
    visit("immediate_damage_multiplier", value.immediate_damage_multiplier);
    visit("mark_explosion_damage_multiplier", value.mark_explosion_damage_multiplier);
    visit("mark_duration_ticks", value.mark_duration_ticks);
    visit("mark_explosion_radius", value.mark_explosion_radius);
}

template <typename Visitor>
void VisitSimulationRuleFields(const TrapUpgrades::TrapKillSmallTrap &value, Visitor &&visit)
{
    visit("detection_radius", value.detection_radius);
    visit("explosion_radius", value.explosion_radius);
    visit("activation_delay_ticks", value.activation_delay_ticks);
    visit("active_duration_ticks", value.active_duration_ticks);
    visit("damage_multiplier", value.damage_multiplier);
    visit("maximum_small_traps_per_cast", value.maximum_small_traps_per_cast);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RetreatShotUpgrades &value, Visitor &&visit)
{
    visit("replace_with_three_arrows", value.replace_with_three_arrows);
    visit("small_trap_at_start", value.small_trap_at_start);
    visit("slow_trail", value.slow_trail);
    visit("apply_bleed_and_tracking_arrow", value.apply_bleed_and_tracking_arrow);
    visit("landing_damage_and_push", value.landing_damage_and_push);
    visit("next_other_active_cooldown_refund", value.next_other_active_cooldown_refund);
    visit("boss_or_three_normal_hits_heal", value.boss_or_three_normal_hits_heal);
    visit("delayed_second_retreat_and_arrow", value.delayed_second_retreat_and_arrow);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RetreatShotUpgrades::ReplaceWithThreeArrows &value, Visitor &&visit)
{
    visit("projectile_count", value.projectile_count);
    visit("damage_multiplier_per_arrow", value.damage_multiplier_per_arrow);
    visit("angles_degrees", value.angles_degrees);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RetreatShotUpgrades::SmallTrapAtStart &value, Visitor &&visit)
{
    visit("damage_multiplier", value.damage_multiplier);
    visit("detection_radius", value.detection_radius);
    visit("explosion_radius", value.explosion_radius);
    visit("activation_delay_ticks", value.activation_delay_ticks);
    visit("active_duration_ticks", value.active_duration_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RetreatShotUpgrades::SlowTrail &value, Visitor &&visit)
{
    visit("duration_ticks", value.duration_ticks);
    visit("slow_fraction", value.slow_fraction);
    visit("radius", value.radius);
    visit("slow_duration_ticks", value.slow_duration_ticks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RetreatShotUpgrades::ApplyBleedAndTrackingArrow &value, Visitor &&visit)
{
    visit("damage_multiplier", value.damage_multiplier);
    visit("maximum_triggers_per_cast", value.maximum_triggers_per_cast);
    visit("search_radius", value.search_radius);
    visit("bleed_stacks", value.bleed_stacks);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RetreatShotUpgrades::LandingDamageAndPush &value, Visitor &&visit)
{
    visit("radius", value.radius);
    visit("damage_multiplier", value.damage_multiplier);
    visit("push_distance", value.push_distance);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RetreatShotUpgrades::NextOtherActiveCooldownRefund &value, Visitor &&visit)
{
    visit("activation_window_ticks", value.activation_window_ticks);
    visit("cooldown_refund_fraction", value.cooldown_refund_fraction);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RetreatShotUpgrades::BossOrThreeNormalHitsHeal &value, Visitor &&visit)
{
    visit("minimum_normal_enemy_hits", value.minimum_normal_enemy_hits);
    visit("maximum_hp_heal_fraction", value.maximum_hp_heal_fraction);
}

template <typename Visitor>
void VisitSimulationRuleFields(const RetreatShotUpgrades::DelayedSecondRetreatAndArrow &value, Visitor &&visit)
{
    visit("delay_after_first_move_ticks", value.delay_after_first_move_ticks);
    visit("additional_move_distance", value.additional_move_distance);
    visit("damage_multiplier", value.damage_multiplier);
}

template <typename Visitor>
void VisitSimulationRuleFields(const UpgradeRules &value, Visitor &&visit)
{
    visit("basic_attack", value.basic_attack);
    visit("piercing_shot", value.piercing_shot);
    visit("multishot", value.multishot);
    visit("charged_shot", value.charged_shot);
    visit("explosive_arrow", value.explosive_arrow);
    visit("ricochet_arrow", value.ricochet_arrow);
    visit("arrow_rain", value.arrow_rain);
    visit("trap", value.trap);
    visit("retreat_shot", value.retreat_shot);
}

template <typename Visitor>
void VisitSimulationRuleFields(const SimulationRules &value, Visitor &&visit)
{
    visit("version", value.version);
    visit("arena_half_extent", value.arena_half_extent);
    visit("arena_boundary", value.arena_boundary);
    visit("arena_obstacles", value.arena_obstacles);
    visit("arena_obstacle_count", value.arena_obstacle_count);
    visit("player_health", value.player_health);
    visit("player_attack", value.player_attack);
    visit("player_attack_speed", value.player_attack_speed);
    visit("player_move_speed", value.player_move_speed);
    visit("player_magnet_radius", value.player_magnet_radius);
    visit("utility_pickup_base_chance", value.utility_pickup_base_chance);
    visit("utility_pickup_miss_increment", value.utility_pickup_miss_increment);
    visit("heal_pickup_chance_multiplier", value.heal_pickup_chance_multiplier);
    visit("magnet_pickup_chance_multiplier", value.magnet_pickup_chance_multiplier);
    visit("relic_chest_base_chance", value.relic_chest_base_chance);
    visit("relic_chest_miss_increment", value.relic_chest_miss_increment);
    visit("status_tick_interval", value.status_tick_interval);
    visit("bleed_duration", value.bleed_duration);
    visit("bleed_tick_coefficient", value.bleed_tick_coefficient);
    visit("burn_duration", value.burn_duration);
    visit("burn_tick_coefficient", value.burn_tick_coefficient);
    visit("stats", value.stats);
    visit("statuses", value.statuses);
    visit("growth", value.growth);
    visit("progression", value.progression);
    visit("character_initial", value.character_initial);
    visit("enemy_scaling", value.enemy_scaling);
    visit("relic_drop", value.relic_drop);
    visit("spawn_placement", value.spawn_placement);
    visit("boss_common", value.boss_common);
    visit("upgrades", value.upgrades);
    visit("skills", value.skills);
    visit("enemies", value.enemies);
    visit("bosses", value.bosses);
    visit("spawn_stages", value.spawn_stages);
    visit("waves", value.waves);
    visit("relics", value.relics);
}


} // namespace hs::detail
