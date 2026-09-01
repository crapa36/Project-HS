#pragma once

#include <hs/core/result.hpp>
#include <hs/game_domain/game_types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace hs
{

enum class SkillTag : std::uint16_t
{
    Bleed = 1u << 0,
    Burn = 1u << 1,
    Slow = 1u << 2,
};

using SkillTagMask = std::uint16_t;

enum class AbilityHandlerId : std::uint8_t
{
    BasicProjectileCadence,
    PiercingProjectile,
    UniformFanProjectiles,
    HoldReleaseLinearCharge,
    ProjectileToAreaExplosion,
    NearestUnhitTargetRicochet,
    TargetedPeriodicArea,
    ForwardRollLeaveTrap,
    ForcedRetreatAndProjectile,
};

enum class EnemyScalingMachine : std::uint8_t
{
    CompletedMinutes,
};

enum class BossPatternLogic : std::uint8_t
{
    LineCharge,
    ExpandingShockwaveWithSafeGaps,
    FanProjectiles,
    PredictedPositionGroundAreas,
    DoubleRetargetedCharge,
    DoubleOffsetFanProjectiles,
};

enum class SpawnFallbackLocation : std::uint8_t
{
    FarthestArenaEdge,
};

[[nodiscard]] SkillTagMask SkillTags(SkillKind skill) noexcept;
[[nodiscard]] SkillTagMask UpgradeTags(SkillKind skill,
                                       std::uint8_t zero_based_upgrade) noexcept;
[[nodiscard]] SkillTagMask RelicPrerequisiteTags(RelicKind relic) noexcept;

// Fixed-layout superset of the nine skill parameter sets. Unused fields are zero.
struct SkillDefinition
{
    Tick cooldown_ticks{};
    float damage_coefficient{};
    float projectile_speed{};
    float range{};
    float collision_radius{};
    float area_radius{};
    Tick duration_ticks{};
    std::uint8_t projectile_count{};
    std::uint8_t pierce_count{};
    AbilityHandlerId handler{};

    float damage_multiplier{};
    float fan_angle_degrees{};
    float damage_multiplier_per_arrow{};
    std::uint8_t pierce_per_arrow{};
    std::uint8_t maximum_hits_per_target{};

    Tick maximum_charge_time_ticks{};
    float minimum_damage_multiplier{};
    float maximum_damage_multiplier{};
    float minimum_range{};
    float maximum_range{};
    float minimum_collision_radius{};
    float maximum_collision_radius{};
    float movement_speed_multiplier_while_charging{};
    float pierce_damage_decay_fraction{};
    float minimum_damage_fraction{};

    float explosion_damage_multiplier{};
    float explosion_radius{};
    float initial_range{};
    float ricochet_search_radius{};
    std::uint8_t maximum_ricochets{};
    float target_range{};
    Tick activation_delay_ticks{};
    Tick tick_interval_ticks{};
    std::uint8_t total_damage_ticks{};
    Tick forward_roll_duration_ticks{};
    float forward_roll_distance{};
    Tick active_duration_ticks{};
    float detection_radius{};
    float slow_fraction{};
    Tick slow_duration_ticks{};
    float forced_move_distance{};
    Tick forced_move_duration_ticks{};
    float projectile_range{};

    std::uint8_t starting_level{};
    std::uint8_t maximum_level{};
    std::uint8_t selected_upgrades_per_session{};
};

struct EnemyDefinition
{
    std::int32_t health{};
    float move_speed{};
    std::int32_t damage{};
    float attack_range{};
    Tick warning_ticks{};
    Tick attack_cooldown_ticks{};
    float projectile_speed{};
    float projectile_range{};
    float collision_radius{};
    float projectile_collision_radius{};
    // Explicit authored fields; legacy range fields remain aliases until all
    // gameplay call sites consume the typed values.
    float ranged_projectile_radius{};
    float suicide_stop_distance{};
    float suicide_explosion_radius{};
};

struct StatAllocationDefinition
{
    float amount_per_point{};
    float immediate_current_hp_restore_per_point{};
};

struct CombatRules
{
    std::int32_t minimum_final_damage{};
    Tick minimum_cooldown_ticks{};
    float skill_cooldown_upgrade_multiplier{};
    std::uint8_t maximum_basic_attacks_per_tick{};
    bool critical_hits{};
    bool armor{};
    bool hit_invulnerability{};
    bool player_hit_knockback{};
};

struct StatRules
{
    std::uint8_t maximum_points_per_stat{};
    float base_maximum_hp{};
    float base_current_hp{};
    float base_attack_power{};
    float base_basic_attack_rate_per_second{};
    float base_movement_speed_mps{};
    float base_magnet_radius_m{};
    float base_global_cooldown_reduction{};
    std::array<StatAllocationDefinition, kStatCount> allocations{};
    CombatRules combat{};
    Tick first_status_tick_delay_ticks{};
    float player_collision_radius{};
};

struct StatusDefinition
{
    std::uint8_t maximum_stacks{};
    std::uint8_t maximum_instances{};
    Tick duration_ticks{};
    Tick tick_interval_ticks{};
    float attack_power_multiplier_per_tick{};
};

struct StatusRules
{
    Tick first_tick_delay_ticks{};
    Tick tick_interval_ticks{};
    StatusDefinition bleed{};
    StatusDefinition burn{};
};

struct GrowthRules
{
    float required_xp_base{};
    float required_xp_linear{};
    float required_xp_quadratic{};
    float experience_pickup_speed{};
    float experience_pickup_radius{};
    float utility_pickup_base_chance{};
    float utility_pickup_miss_increment{};
    float heal_pickup_chance_multiplier{};
    float magnet_pickup_chance_multiplier{};
};

// Progression values authored in level.json. GrowthRules keeps the legacy
// aliases used by existing gameplay code while this block is the canonical
// fixed-layout attachment for progression/cooker data.
struct ProgressionRules
{
    std::array<std::uint32_t, 3> enemy_xp{};
    std::array<std::uint32_t, 3> boss_xp{};
    float required_xp_base{};
    float required_xp_linear{};
    float required_xp_quadratic{};
    std::uint8_t card_candidate_count{};
    std::uint8_t card_choose_count{};
    std::uint8_t active_slot_count{};
    std::uint8_t upgrades_selected_per_skill{};
    std::uint8_t base_stat_points_per_level{};
    std::uint8_t fallback_extra_stat_points{};
    bool fallback_stat_point_cards{};
    std::uint8_t level_initial_rerolls{};
    std::uint8_t relic_initial_rerolls{};
    std::array<Tick, 3> boss_spawn_ticks{};
    bool stop_normal_spawns_at_final_boss{};
};

struct CharacterInitial
{
    std::uint8_t starting_level{};
    std::uint8_t starting_basic_attack_level{};
    std::uint8_t starting_unspent_stat_points{};
    std::uint8_t starting_level_rerolls{};
    std::uint8_t starting_relic_rerolls{};
    std::uint8_t starting_active_skill_count{};
    std::uint8_t starting_relic_count{};
    std::array<SkillKind, kCombatSkillCount> starting_active_skill_ids{};
    std::array<RelicKind, kRelicCount> starting_relic_ids{};
    float movement_stop_distance_m{};
};

struct EnemyScaling
{
    EnemyScalingMachine machine{};
    float hp_fraction_per_completed_minute{};
    float damage_fraction_per_completed_minute{};
    bool updates_existing_enemies{};
    bool applies_to_bosses{};
};

struct RelicDropRules
{
    std::uint32_t guaranteed_boxes_per_mid_boss{};
    float mid_boss_maximum_hp_heal_fraction{};
    float normal_enemy_base_probability{};
    float normal_enemy_probability_increment_per_kill{};
    float normal_enemy_probability_cap{};
    std::uint32_t maximum_choices_per_box{};
    float healing_pickup_probability{};
    float healing_pickup_maximum_hp_heal_fraction{};
    bool hard_pity{};
    bool reset_kill_counter_on_box_spawn{};
    bool stable_id_sort_before_seeded_shuffle{};
    bool stop_normal_box_rolls_after_all_acquired{};
    bool healing_pickup_independent_from_other_rewards{};
    bool healing_pickup_can_coexist_with_xp_and_relic_box{};
};

struct SpawnPlacement
{
    float minimum_player_distance_m{};
    float maximum_player_distance_m{};
    bool require_inside_arena{};
    bool require_outside_max_zoom_view{};
    Tick fallback_warning_ticks{};
    SpawnFallbackLocation fallback_location{};
    float max_zoom_view_min_forward_m{};
    float max_zoom_view_max_forward_m{};
    float max_zoom_view_half_right_m{};
    float max_zoom_view_forward_x{};
    float max_zoom_view_forward_z{};
};

struct BossPhaseTransition
{
    float hp_fraction{};
    Tick invulnerability_ticks{};
    bool remove_enemy_projectiles{};
    bool remove_enemy_areas{};
    bool keep_normal_enemies{};
    bool keep_mid_bosses{};
    bool keep_pickups{};
    bool keep_player_projectiles{};
    bool keep_player_areas{};
};

struct BossReward
{
    std::uint32_t relic_chest_count{};
    float maximum_hp_heal_fraction{};
};

// Typed superset for the six authored pattern logics.
struct BossPatternDefinition
{
    BossPatternLogic logic{};
    std::uint8_t phase{};
    Tick telegraph_duration_ticks{};
    float distance{};
    float speed{};
    std::int32_t damage{};
    float start_radius{};
    float end_radius{};
    std::uint8_t safe_gap_count{};
    float safe_gap_angle_degrees{};
    Tick shockwave_duration_ticks{};
    float shockwave_half_width{};
    float fan_angle_degrees{};
    std::uint8_t projectile_count{};
    float projectile_speed{};
    std::uint8_t area_count{};
    float radius{};
    Tick duration_ticks{};
    Tick tick_interval_ticks{};
    std::int32_t damage_per_tick{};
    std::uint8_t charge_count{};
    Tick interval_ticks{};
    std::uint8_t volley_count{};
    std::uint8_t projectiles_per_volley{};
    float second_volley_angle_offset_degrees{};
    Tick prediction_lead_ticks{};
    float ground_placement_radius{};
};

struct BossCommonRules
{
    float collision_radius{};
    Tick spawn_warning_ticks{};
    Tick initial_pattern_delay_ticks{};
    std::uint32_t preferred_pattern_weight{};
    std::uint32_t other_pattern_weight{};
    std::uint32_t maximum_same_pattern_repeats{};
    bool body_contact_damage{};
    bool time_scaling{};
    bool affected_by_damage{};
    bool affected_by_bleed{};
    bool affected_by_burn{};
    bool affected_by_slow{};
    bool immune_to_pull{};
    bool immune_to_push{};
    bool immune_to_alignment_move{};
    float projectile_range{};
    float projectile_collision_radius{};
};

struct BossDefinition
{
    std::int32_t health{};
    Tick recovery_ticks{};
    Tick phase2_pattern_interval_ticks{};
    Tick phase2_cycle_recovery_ticks{};
    Tick spawn_growth_ticks{};
    Tick target_kill_ticks{};
    float movement_speed{};
    float collision_radius{};
    float projectile_range{};
    float projectile_collision_radius{};
    float preferred_distance_near{};
    float preferred_distance_far{};
    float phase_two_preferred_distance_near{};
    float phase_two_preferred_distance_far{};
    std::array<BossPatternDefinition, 6> patterns{};
    std::uint8_t pattern_count{};
    BossPhaseTransition phase_transition{};
    bool has_phase_transition{};
    BossReward reward{};
};

struct SpawnStage
{
    std::uint16_t start_minute{};
    Tick start_tick{};
    float per_second{};
    std::array<std::uint8_t, 3> weights{};
};

struct WaveDefinition
{
    std::uint16_t minute{};
    Tick start_tick{};
    std::uint16_t count{};
    Tick duration_ticks{};
};

struct RelicDefinitions
{
    struct BleedKillHeal
    {
        float maximum_hp_heal_fraction{};
        Tick internal_cooldown_ticks{};
    } bleed_kill_heal;
    struct BurnPropagation
    {
        float search_radius{};
        float copied_burn_strength{};
        std::uint32_t maximum_targets{};
    } burn_propagation;
    struct KillCooldownSurge
    {
        std::uint32_t kills_per_trigger{};
        Tick cooldown_reduction_ticks{};
    } kill_cooldown_surge;
    struct BleedBurnExplosion
    {
        float radius{};
        float damage_multiplier{};
        Tick per_target_cooldown_ticks{};
    } bleed_burn_explosion;
    struct RadialBasicAttack
    {
        std::uint32_t cadence_interval{};
        std::uint32_t direction_count{};
        float damage_multiplier{};
    } radial_basic_attack;
    struct BasicKillTracker
    {
        float search_radius{};
        float damage_multiplier{};
        std::uint32_t maximum_triggers_per_attack{};
    } basic_kill_tracker;
    struct MovementEcho
    {
        float required_distance{};
        Tick position_history_age_ticks{};
        float damage_multiplier{};
    } movement_echo;
    struct AlternatingSkills
    {
        Tick window_ticks{};
        float cooldown_refund_fraction{};
    } alternating_skills;
    struct DifferentSkillTracker
    {
        Tick window_ticks{};
        float damage_multiplier{};
        Tick per_target_cooldown_ticks{};
    } different_skill_tracker;
    struct DamageKnockback
    {
        float radius{};
        float push_distance{};
        float slow_fraction{};
        Tick slow_duration_ticks{};
        Tick cooldown_ticks{};
    } damage_knockback;
    struct OnceRevive
    {
        float health_fraction{};
        Tick invulnerability_ticks{};
        std::uint32_t maximum_triggers_per_session{};
    } once_revive;
    struct CombatHitChain
    {
        std::uint32_t direct_hits_per_trigger{};
        float search_radius{};
        std::uint32_t maximum_targets{};
        float damage_multiplier{};
    } combat_hit_chain;
    struct ProjectileCadenceReward { std::uint32_t hits_per_trigger{}; Tick cooldown_reduction_ticks{}; } projectile_cadence_reward;
    struct PreDamageGuard { float damage_reduction_fraction{}; Tick cooldown_ticks{}; } pre_damage_guard;
    struct SlowSynergy { float damage_multiplier{}; Tick per_target_cooldown_ticks{}; } slow_synergy;
    struct AreaResonance { float damage_multiplier{}; Tick cooldown_ticks{}; } area_resonance;
    struct BossPressure { float damage_multiplier{}; Tick per_target_cooldown_ticks{}; } boss_pressure;
    struct HitStreakReward { std::uint32_t direct_hits_per_trigger{}; float damage_multiplier{}; } hit_streak_reward;
    struct PickupReward { float attack_power_fraction{}; Tick duration_ticks{}; } pickup_reward;
    struct LowHealthSurvival { float health_threshold_fraction{}; float damage_reduction_fraction{}; Tick cooldown_ticks{}; } low_health_survival;
};

struct BasicAttackUpgrades
{
    struct ThirdAttackDelayed { std::uint32_t cadence_interval{}; Tick delay_ticks{}; float damage_multiplier{}; } third_attack_delayed;
    struct AdditionalPierce { std::uint32_t additional_targets{}; } additional_pierce;
    struct FirstHitSplit { std::array<float, 2> angles_degrees{}; std::uint32_t projectile_count{}; float damage_multiplier{}; } first_hit_split;
    struct DirectArrowBleed { std::uint32_t direct_arrow_interval{}; std::uint32_t bleed_stacks{}; } direct_arrow_bleed;
    struct DirectArrowBurn { std::uint32_t direct_arrow_interval{}; } direct_arrow_burn;
    struct FirstHitSlowAndCooldown { float slow_fraction{}; Tick slow_duration_ticks{}; Tick cooldown_reduction_ticks{}; } first_hit_slow_and_cooldown;
    struct MissedArrowReacquire { float damage_multiplier{}; std::uint32_t maximum_retargets{}; float collision_radius_multiplier{}; } missed_arrow_reacquire;
    struct PostActiveThreeArrow { Tick activation_window_ticks{}; std::array<float, 3> angles_degrees{}; std::uint32_t projectile_count{}; float damage_multiplier_per_arrow{}; } post_active_three_arrow;
};

struct PiercingShotUpgrades
{
    struct DelayedFollowup { Tick delay_ticks{}; float damage_multiplier{}; } delayed_followup;
    struct RangeEndSplit { std::array<float, 2> angles_degrees{}; std::uint32_t projectile_count{}; float damage_multiplier{}; float search_radius{}; } range_end_split;
    struct ApplyBleed { std::uint32_t bleed_stacks{}; } apply_bleed;
    struct DamagingSlowTrail { float width{}; Tick duration_ticks{}; Tick tick_interval_ticks{}; float damage_multiplier_per_tick{}; float slow_fraction{}; Tick slow_duration_ticks{}; } damaging_slow_trail;
    struct PerThreePiercesPerpendicular { std::uint32_t normal_enemy_pierce_interval{}; std::uint32_t projectile_count{}; float angle_from_direction{}; float damage_multiplier{}; } per_three_pierces_perpendicular;
    struct AlignHitNormalEnemy { float move_distance{}; } align_hit_normal_enemy;
    struct ApplyAndTransferBurn { std::uint32_t maximum_transfer_targets{}; } apply_and_transfer_burn;
    struct CooldownMultiplier { float cooldown_multiplier{}; } cooldown_multiplier;
};

struct MultishotUpgrades
{
    struct DelayedSecondFan { Tick delay_ticks{}; float fan_angle_degrees{}; std::uint32_t projectile_count{}; float damage_multiplier_per_arrow{}; } delayed_second_fan;
    struct OriginalArrowFirstHitSplit { std::array<float, 2> angles_degrees{}; std::uint32_t projectile_count_per_original{}; float damage_multiplier{}; } original_arrow_first_hit_split;
    struct AdditionalPiercePerArrow { std::uint32_t additional_pierce{}; } additional_pierce_per_arrow;
    struct RearFan { std::uint32_t projectile_count{}; float damage_multiplier{}; float fan_angle_degrees{}; } rear_fan;
    struct FirstCastHitBleed { std::uint32_t bleed_stacks{}; } first_cast_hit_bleed;
    struct ApplyBurnAndTransferArrow { float damage_multiplier{}; std::uint32_t maximum_transfers_per_cast{}; } apply_burn_and_transfer_arrow;
    struct TwoAdditionalOuterArrows { std::uint32_t additional_projectiles{}; std::array<float, 2> outer_angles_degrees{}; float damage_multiplier{}; } two_additional_outer_arrows;
    struct MissedArrowRetarget { float damage_multiplier{}; std::uint32_t maximum_retargets{}; float search_radius{}; } missed_arrow_retarget;
};

struct ChargedShotUpgrades
{
    struct ExtendedFullChargeExplosion { Tick maximum_charge_time_ticks{}; float maximum_damage_multiplier{}; float full_charge_end_explosion_damage_multiplier{}; float explosion_radius{}; } extended_full_charge_explosion;
    struct FasterCharge { float charge_time_multiplier{}; } faster_charge;
    struct IncreaseMinimumChargeDamage { float minimum_damage_multiplier{}; } increase_minimum_charge_damage;
    struct AdditionalPierce { std::uint32_t additional_pierce{}; } additional_pierce;
    struct ChargeBleedFullChargeRupture { std::uint32_t bleed_stacks{}; std::uint32_t existing_bleed_stacks_for_rupture{}; float rupture_damage_multiplier{}; } charge_bleed_full_charge_rupture;
    struct BossOrFifthPierceSplit { std::uint32_t normal_enemy_pierce_count{}; std::array<float, 8> angles_degrees{}; std::uint32_t projectile_count{}; float damage_multiplier{}; } boss_or_fifth_pierce_split;
    struct FirstHitBurnExplosion { float radius{}; float damage_multiplier{}; } first_hit_burn_explosion;
    struct FullChargeMultiKillCooldownRefund { std::uint32_t minimum_normal_enemy_kills{}; float current_cooldown_refund_fraction{}; } full_charge_multi_kill_cooldown_refund;
};

struct ExplosiveArrowUpgrades
{
    struct DelayedReexplosion { Tick delay_ticks{}; float radius{}; float damage_multiplier{}; } delayed_reexplosion;
    struct ThreeDelayedSatelliteBombs { std::uint32_t bomb_count{}; float angular_spacing{}; float placement_radius{}; Tick delay_ticks{}; float explosion_radius{}; float damage_multiplier{}; } three_delayed_satellite_bombs;
    struct EightDirectionFragments { std::uint32_t direction_count{}; float damage_multiplier{}; float collision_radius_multiplier{}; std::uint32_t pierce{}; } eight_direction_fragments;
    struct ExplosionLeavesBurningArea { float radius{}; Tick duration_ticks{}; Tick tick_interval_ticks{}; float damage_multiplier_per_tick{}; } explosion_leaves_burning_area;
    struct ApplyBleedAndBloodExplosions { float radius{}; float damage_multiplier{}; std::uint32_t maximum_explosions_per_cast{}; std::uint32_t bleed_stacks{}; } apply_bleed_and_blood_explosions;
    struct PreExplosionPull { Tick duration_ticks{}; float pull_radius{}; } pre_explosion_pull;
    struct DirectHitMarkOtherActiveExplosion { float immediate_explosion_damage_multiplier{}; float mark_explosion_damage_multiplier{}; Tick mark_duration_ticks{}; float mark_explosion_radius{}; } direct_hit_mark_other_active_explosion;
    struct CooldownMultiplier { float cooldown_multiplier{}; } cooldown_multiplier;
};

struct RicochetArrowUpgrades
{
    struct ReturnToPlayerRehit { float damage_multiplier{}; std::uint32_t maximum_rehit_targets{}; } return_to_player_rehit;
    struct FirstRicochetBranchChain { float damage_multiplier{}; std::uint32_t maximum_targets{}; } first_ricochet_branch_chain;
    struct ApplyBleedAndExtendRicochets { std::uint32_t additional_ricochets_per_bleeding_hit{}; std::uint32_t maximum_additional_ricochets{}; } apply_bleed_and_extend_ricochets;
    struct ApplyAndCopyBurnToNextTarget { std::uint8_t enabled{}; } apply_and_copy_burn_to_next_target;
    struct KillSmallArrows { float damage_multiplier{}; std::uint32_t arrows_per_kill{}; std::uint32_t maximum_arrows_per_cast{}; float search_radius{}; } kill_small_arrows;
    struct OriginalRicochetKillNewChain { std::uint32_t new_chain_targets{}; float damage_multiplier{}; float search_radius{}; } original_ricochet_kill_new_chain;
    struct RicochetCooldownReduction { Tick cooldown_reduction_per_ricochet_ticks{}; Tick maximum_reduction_ticks{}; } ricochet_cooldown_reduction;
    struct CooldownMultiplier { float cooldown_multiplier{}; } cooldown_multiplier;
};

struct ArrowRainUpgrades
{
    struct DelayedForwardSecondaryArea { Tick delay_ticks{}; float forward_offset{}; float radius{}; Tick duration_ticks{}; float damage_multiplier_per_tick{}; } delayed_forward_secondary_area;
    struct FirstDamageTickPull { float pull_radius{}; } first_damage_tick_pull;
    struct ThirdAreaHitBleed { std::uint32_t hit_ordinal{}; std::uint32_t bleed_stacks{}; } third_area_hit_bleed;
    struct ApplyBurnAndCreateFireAreas { float radius{}; Tick duration_ticks{}; Tick tick_interval_ticks{}; float damage_multiplier_per_tick{}; std::uint32_t maximum_areas_per_cast{}; } apply_burn_and_create_fire_areas;
    struct TrackingArrowEachDamageTick { float search_radius{}; float damage_multiplier{}; } tracking_arrow_each_damage_tick;
    struct AreaSlow { float slow_fraction{}; Tick slow_duration_ticks{}; } area_slow;
    struct AreaKillArrowOutside { float damage_multiplier{}; std::uint32_t maximum_triggers_per_cast{}; } area_kill_arrow_outside;
    struct ExtendAreaAndLeaveSlow { Tick area_duration_ticks{}; std::uint32_t additional_damage_ticks{}; Tick post_area_duration_ticks{}; float post_area_slow_fraction{}; Tick post_area_slow_duration_ticks{}; } extend_area_and_leave_slow;
};

struct TrapUpgrades
{
    struct RollPathTraps { std::uint32_t trap_count{}; float damage_multiplier_per_trap{}; float spacing{}; } roll_path_traps;
    struct LandingSlowArea { float radius{}; float slow_fraction{}; Tick duration_ticks{}; } landing_slow_area;
    struct SingleReactivation { Tick reactivation_delay_ticks{}; std::uint32_t maximum_reactivations{}; } single_reactivation;
    struct TriggerBleed { std::uint32_t bleed_stacks{}; } trigger_bleed;
    struct TriggerBurnAndFireArea { float area_radius{}; Tick area_duration_ticks{}; Tick tick_interval_ticks{}; float damage_multiplier_per_tick{}; } trigger_burn_and_fire_area;
    struct PreExplosionPullAndSlow { float pull_radius{}; float slow_fraction{}; Tick slow_duration_ticks{}; } pre_explosion_pull_and_slow;
    struct TriggerMarkOtherActiveDamage { float immediate_damage_multiplier{}; float mark_explosion_damage_multiplier{}; Tick mark_duration_ticks{}; float mark_explosion_radius{}; } trigger_mark_other_active_damage;
    struct TrapKillSmallTrap { float detection_radius{}; float explosion_radius{}; Tick activation_delay_ticks{}; Tick active_duration_ticks{}; float damage_multiplier{}; std::uint32_t maximum_small_traps_per_cast{}; } trap_kill_small_trap;
};

struct RetreatShotUpgrades
{
    struct ReplaceWithThreeArrows { std::uint32_t projectile_count{}; float damage_multiplier_per_arrow{}; std::array<float, 3> angles_degrees{}; } replace_with_three_arrows;
    struct SmallTrapAtStart { float damage_multiplier{}; float detection_radius{}; float explosion_radius{}; Tick activation_delay_ticks{}; Tick active_duration_ticks{}; } small_trap_at_start;
    struct SlowTrail { Tick duration_ticks{}; float slow_fraction{}; float radius{}; Tick slow_duration_ticks{}; } slow_trail;
    struct ApplyBleedAndTrackingArrow { float damage_multiplier{}; std::uint32_t maximum_triggers_per_cast{}; float search_radius{}; std::uint32_t bleed_stacks{}; } apply_bleed_and_tracking_arrow;
    struct LandingDamageAndPush { float radius{}; float damage_multiplier{}; float push_distance{}; } landing_damage_and_push;
    struct NextOtherActiveCooldownRefund { Tick activation_window_ticks{}; float cooldown_refund_fraction{}; } next_other_active_cooldown_refund;
    struct BossOrThreeNormalHitsHeal { std::uint32_t minimum_normal_enemy_hits{}; float maximum_hp_heal_fraction{}; } boss_or_three_normal_hits_heal;
    struct DelayedSecondRetreatAndArrow { Tick delay_after_first_move_ticks{}; float additional_move_distance{}; float damage_multiplier{}; } delayed_second_retreat_and_arrow;
};

struct UpgradeRules
{
    BasicAttackUpgrades basic_attack{};
    PiercingShotUpgrades piercing_shot{};
    MultishotUpgrades multishot{};
    ChargedShotUpgrades charged_shot{};
    ExplosiveArrowUpgrades explosive_arrow{};
    RicochetArrowUpgrades ricochet_arrow{};
    ArrowRainUpgrades arrow_rain{};
    TrapUpgrades trap{};
    RetreatShotUpgrades retreat_shot{};
};

struct SimulationRules
{
    std::uint32_t version{};
    float arena_half_extent{};
    std::int32_t player_health{};
    float player_attack{};
    float player_attack_speed{};
    float player_move_speed{};
    float player_magnet_radius{};
    float utility_pickup_base_chance{};
    float utility_pickup_miss_increment{};
    float heal_pickup_chance_multiplier{};
    float magnet_pickup_chance_multiplier{};
    float relic_chest_base_chance{};
    float relic_chest_miss_increment{};
    Tick status_tick_interval{};
    Tick bleed_duration{};
    float bleed_tick_coefficient{};
    Tick burn_duration{};
    float burn_tick_coefficient{};

    StatRules stats{};
    StatusRules statuses{};
    GrowthRules growth{};
    ProgressionRules progression{};
    CharacterInitial character_initial{};
    EnemyScaling enemy_scaling{};
    RelicDropRules relic_drop{};
    SpawnPlacement spawn_placement{};
    BossCommonRules boss_common{};
    UpgradeRules upgrades{};

    std::array<SkillDefinition, kCombatSkillCount> skills{};
    std::array<EnemyDefinition, 3> enemies{};
    std::array<BossDefinition, 3> bosses{};
    std::array<SpawnStage, 7> spawn_stages{};
    std::array<WaveDefinition, 5> waves{};
    RelicDefinitions relics{};
    [[nodiscard]] static SimulationRules Defaults() noexcept;
};

[[nodiscard]] std::uint64_t SimulationRulesSchemaHash() noexcept;
[[nodiscard]] std::uint64_t SimulationRulesHash(const SimulationRules &rules) noexcept;
[[nodiscard]] Result LoadSimulationRules(const std::filesystem::path &path,
                                         SimulationRules &rules,
                                         std::uint64_t *content_hash = nullptr);

} // namespace hs
