#include "content_cooker.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace hs::content
{

template <typename Type, typename Source>
Type CheckedInteger(Source value, std::string_view file, std::string_view path)
{
    const auto converted = static_cast<long double>(value);
    if (!std::isfinite(converted) ||
        converted < static_cast<long double>((std::numeric_limits<Type>::min)()) ||
        converted > static_cast<long double>((std::numeric_limits<Type>::max)()) ||
        std::floor(converted) != converted)
    {
        ThrowValidationError(file, path, "integer value is outside cooked range");
    }
    return static_cast<Type>(converted);
}

hs::Tick ToTicks(double seconds, std::string_view file, std::string_view path)
{
    if (!std::isfinite(seconds) || seconds < 0.0)
        ThrowValidationError(file, path, "duration must be finite and non-negative");
    return CheckedInteger<hs::Tick>(std::llround(seconds * 60.0), file, path);
}

BuiltGameData BuildGameData(const ContentSources &sources)
{
    BuiltGameData content{};
    auto &data = content.simulation_rules;
    auto &presentation = content.presentation;
    const auto defaults = hs::SimulationRules::Defaults();
    data.version = defaults.version;
    data.skills = defaults.skills;
    data.enemies = defaults.enemies;
    data.bosses = defaults.bosses;
    data.spawn_stages = defaults.spawn_stages;
    data.waves = defaults.waves;

    const auto &stats = sources.documents.at("stats")["entries"][0]["base_values"];
    data.player_health = CheckedInteger<std::int32_t>(
        RequireNumber(stats, "stats", "$/entries/0/base_values", "maximum_hp"), "stats",
        "$/entries/0/base_values/maximum_hp");
    data.player_attack = static_cast<float>(
        RequireNumber(stats, "stats", "$/entries/0/base_values", "attack_power"));
    data.player_attack_speed = static_cast<float>(RequireNumber(
        stats, "stats", "$/entries/0/base_values", "basic_attack_rate_per_second"));
    data.player_move_speed = static_cast<float>(
        RequireNumber(stats, "stats", "$/entries/0/base_values", "movement_speed_mps"));
    data.player_magnet_radius = static_cast<float>(
        RequireNumber(stats, "stats", "$/entries/0/base_values", "magnet_radius_m"));

    const auto &statuses = sources.documents.at("stats")["entries"][0]["statuses"];
    data.status_tick_interval = ToTicks(
        ParameterNumber(statuses[1], "stats", "$/entries/0/statuses/1", "tick_interval"),
        "stats", "$/entries/0/statuses/1/parameters/tick_interval");
    data.bleed_duration = ToTicks(
        ParameterNumber(statuses[1], "stats", "$/entries/0/statuses/1", "duration"),
        "stats", "$/entries/0/statuses/1/parameters/duration");
    data.bleed_tick_coefficient = static_cast<float>(ParameterNumber(
        statuses[1], "stats", "$/entries/0/statuses/1",
        "attack_power_multiplier_per_tick"));
    data.burn_duration = ToTicks(
        ParameterNumber(statuses[2], "stats", "$/entries/0/statuses/2", "duration"),
        "stats", "$/entries/0/statuses/2/parameters/duration");
    data.burn_tick_coefficient = static_cast<float>(ParameterNumber(
        statuses[2], "stats", "$/entries/0/statuses/2",
        "attack_power_multiplier_per_tick"));
    const auto burn_tick_interval = ToTicks(
        ParameterNumber(statuses[2], "stats", "$/entries/0/statuses/2", "tick_interval"),
        "stats", "$/entries/0/statuses/2/parameters/tick_interval");
    if (burn_tick_interval != data.status_tick_interval)
        ThrowValidationError("stats", "$/entries/0/statuses", "bleed and burn tick intervals must match");

    const auto &experience = sources.documents.at("level")["entries"][0]["experience"];
    data.utility_pickup_base_chance = static_cast<float>(RequireNumber(
        experience, "level", "$/entries/0/experience", "utility_pickup_base_chance"));
    data.utility_pickup_miss_increment = static_cast<float>(RequireNumber(
        experience, "level", "$/entries/0/experience", "utility_pickup_miss_increment"));
    data.heal_pickup_chance_multiplier = static_cast<float>(RequireNumber(
        experience, "level", "$/entries/0/experience",
        "heal_pickup_chance_multiplier"));
    data.magnet_pickup_chance_multiplier = static_cast<float>(RequireNumber(
        experience, "level", "$/entries/0/experience",
        "magnet_pickup_chance_multiplier"));

    const auto &relic_boxes = sources.documents.at("relics")["box_rules"];
    data.relic_chest_base_chance = static_cast<float>(RequireNumber(
        relic_boxes, "relics", "$/box_rules",
        "normal_enemy_base_probability_percent") / 100.0);
    data.relic_chest_miss_increment = static_cast<float>(RequireNumber(
        relic_boxes, "relics", "$/box_rules",
        "normal_enemy_probability_increment_per_kill_percent") / 100.0);

    const auto &relics = sources.documents.at("relics")["entries"];
    const auto relic_number = [&](std::size_t index, std::string_view key) {
        const auto path = "$/entries/" + std::to_string(index);
        const auto value = static_cast<float>(
            ParameterNumber(relics[index], "relics", path, key));
        RequirePositive(value, "relics", path + "/parameters/" + std::string(key));
        return value;
    };
    const auto relic_count = [&](std::size_t index, std::string_view key) {
        return CheckedInteger<std::uint32_t>(
            relic_number(index, key), "relics",
            "$/entries/" + std::to_string(index) + "/parameters/" +
                std::string(key));
    };
    const auto copy_text = [&](auto &destination, const std::string &source,
                               std::string_view path) {
        if (source.size() >= destination.size())
            ThrowValidationError("relics", path, "UTF-8 text exceeds cooked capacity");
        std::ranges::copy(source, destination.begin());
    };
    for (std::size_t index = 0; index < relics.size(); ++index)
    {
        const auto path = "$/entries/" + std::to_string(index);
        copy_text(presentation.relic_names[index],
                  RequireString(relics[index], "relics", path, "display_name"),
                  path + "/display_name");
        copy_text(presentation.relic_rules[index],
                  RequireString(relics[index], "relics", path, "rule"),
                  path + "/rule");
    }
    const auto relic_ticks = [&](std::size_t index, std::string_view key) {
        return ToTicks(relic_number(index, key), "relics",
                       "$/entries/" + std::to_string(index) + "/parameters/" +
                           std::string(key));
    };
    data.relics.bleed_kill_heal = {
        relic_number(0, "maximum_hp_heal_fraction"),
        relic_ticks(0, "internal_cooldown")};
    data.relics.burn_propagation = {
        relic_number(1, "search_radius"), relic_number(1, "copied_burn_strength"),
        relic_count(1, "maximum_targets")};
    data.relics.kill_cooldown_surge = {
        relic_count(2, "kills_per_trigger"),
        relic_ticks(2, "all_active_cooldown_reduction")};
    data.relics.bleed_burn_explosion = {
        relic_number(3, "radius"), relic_number(3, "damage_multiplier"),
        relic_ticks(3, "per_target_cooldown")};
    data.relics.radial_basic_attack = {
        relic_count(4, "cadence_interval"), relic_count(4, "direction_count"),
        relic_number(4, "damage_multiplier")};
    data.relics.basic_kill_tracker = {
        relic_number(5, "search_radius"), relic_number(5, "damage_multiplier"),
        relic_count(5, "maximum_triggers_per_original_attack")};
    data.relics.movement_echo = {
        relic_number(6, "required_cumulative_distance"),
        relic_ticks(6, "position_history_age"),
        relic_number(6, "damage_multiplier")};
    data.relics.alternating_skills = {
        relic_ticks(7, "window"), relic_number(7, "cooldown_refund_fraction")};
    data.relics.different_skill_tracker = {
        relic_ticks(8, "window"), relic_number(8, "damage_multiplier"),
        relic_ticks(8, "per_target_cooldown")};
    data.relics.damage_knockback = {
        relic_number(9, "radius"), relic_number(9, "push_distance"),
        relic_number(9, "slow_fraction"), relic_ticks(9, "slow_duration"),
        relic_ticks(9, "cooldown")};
    data.relics.once_revive = {
        relic_number(10, "revive_hp_fraction"),
        relic_ticks(10, "invulnerability_duration"),
        relic_count(10, "maximum_triggers_per_session")};
    data.relics.combat_hit_chain = {
        relic_count(11, "direct_hits_per_trigger"),
        relic_number(11, "search_radius"), relic_count(11, "maximum_targets"),
        relic_number(11, "damage_multiplier")};

    const auto &arena = sources.documents.at("level")["entries"][0]["arena"];
    const auto width = RequireNumber(arena, "level", "$/entries/0/arena", "width_m");
    const auto depth = RequireNumber(arena, "level", "$/entries/0/arena", "depth_m");
    if (width != depth)
    {
        ThrowValidationError("level", "$/entries/0/arena", "GameData v1 requires a square arena");
    }
    data.arena_half_extent = static_cast<float>(width * 0.5);

    const auto &skills = sources.documents.at("skills")["entries"];
    constexpr std::array<std::pair<std::string_view, hs::AbilityHandlerId>,
                         hs::kCombatSkillCount> ability_handlers{{
        {"basic_projectile_cadence", hs::AbilityHandlerId::BasicProjectileCadence},
        {"piercing_projectile", hs::AbilityHandlerId::PiercingProjectile},
        {"uniform_fan_projectiles", hs::AbilityHandlerId::UniformFanProjectiles},
        {"hold_release_linear_charge", hs::AbilityHandlerId::HoldReleaseLinearCharge},
        {"projectile_to_area_explosion", hs::AbilityHandlerId::ProjectileToAreaExplosion},
        {"nearest_unhit_target_ricochet", hs::AbilityHandlerId::NearestUnhitTargetRicochet},
        {"targeted_periodic_area", hs::AbilityHandlerId::TargetedPeriodicArea},
        {"forward_roll_leave_trap", hs::AbilityHandlerId::ForwardRollLeaveTrap},
        {"forced_retreat_and_projectile", hs::AbilityHandlerId::ForcedRetreatAndProjectile},
    }};
    for (std::size_t index = 0; index < skills.size(); ++index)
    {
        const auto path = "$/entries/" + std::to_string(index);
        const auto logic = RequireString(skills[index], "skills", path, "logic_id");
        if (logic != ability_handlers[index].first)
            ThrowValidationError("skills", path + "/logic_id",
                 "logic_id does not match the typed ability handler");
        data.skills[index].handler = ability_handlers[index].second;
    }
    const auto number = [&](std::size_t index, std::string_view key) {
        return static_cast<float>(ParameterNumber(
            skills[index], "skills", "$/entries/" + std::to_string(index), key));
    };
    const auto skill_ticks = [&](std::size_t index, std::string_view key) {
        return ToTicks(number(index, key), "skills",
                       "$/entries/" + std::to_string(index) + "/parameters/" +
                           std::string(key));
    };
    data.skills[0].damage_coefficient = number(0, "damage_multiplier");
    data.skills[0].projectile_speed = number(0, "projectile_speed");
    data.skills[0].range = number(0, "range");
    data.skills[0].collision_radius = number(0, "collision_radius");
    data.skills[0].pierce_count = CheckedInteger<std::uint8_t>(
        number(0, "pierce"), "skills", "$/entries/0/pierce");

    data.skills[1].cooldown_ticks = skill_ticks(1, "cooldown");
    data.skills[1].damage_coefficient = number(1, "damage_multiplier");
    data.skills[1].projectile_speed = number(1, "projectile_speed");
    data.skills[1].range = number(1, "range");
    data.skills[1].collision_radius = number(1, "collision_radius");

    data.skills[2].cooldown_ticks = skill_ticks(2, "cooldown");
    data.skills[2].damage_coefficient = number(2, "damage_multiplier_per_arrow");
    data.skills[2].projectile_speed = number(2, "projectile_speed");
    data.skills[2].range = number(2, "range");
    data.skills[2].projectile_count = CheckedInteger<std::uint8_t>(
        number(2, "projectile_count"), "skills", "$/entries/2/projectile_count");
    data.skills[2].pierce_count = CheckedInteger<std::uint8_t>(
        number(2, "pierce_per_arrow"), "skills", "$/entries/2/pierce_per_arrow");

    data.skills[3].cooldown_ticks = skill_ticks(3, "cooldown");
    data.skills[3].damage_coefficient = number(3, "maximum_damage_multiplier");
    data.skills[3].projectile_speed = number(3, "projectile_speed");
    data.skills[3].range = number(3, "maximum_range");
    data.skills[3].collision_radius = number(3, "maximum_collision_radius");
    data.skills[3].duration_ticks = skill_ticks(3, "maximum_charge_time");
    data.skills[3].pierce_count = CheckedInteger<std::uint8_t>(
        number(3, "pierce"), "skills", "$/entries/3/pierce");

    data.skills[4].cooldown_ticks = skill_ticks(4, "cooldown");
    data.skills[4].damage_coefficient = number(4, "explosion_damage_multiplier");
    data.skills[4].projectile_speed = number(4, "projectile_speed");
    data.skills[4].range = number(4, "range");
    data.skills[4].area_radius = number(4, "explosion_radius");

    data.skills[5].cooldown_ticks = skill_ticks(5, "cooldown");
    data.skills[5].damage_coefficient = number(5, "damage_multiplier");
    data.skills[5].range = number(5, "initial_range");
    data.skills[5].area_radius = number(5, "ricochet_search_radius");
    data.skills[5].pierce_count = CheckedInteger<std::uint8_t>(
        number(5, "maximum_ricochets"), "skills", "$/entries/5/maximum_ricochets");

    data.skills[6].cooldown_ticks = skill_ticks(6, "cooldown");
    data.skills[6].damage_coefficient = number(6, "damage_multiplier_per_tick");
    data.skills[6].range = number(6, "target_range");
    data.skills[6].area_radius = number(6, "radius");
    data.skills[6].duration_ticks = skill_ticks(6, "duration");

    data.skills[7].cooldown_ticks = skill_ticks(7, "cooldown");
    data.skills[7].damage_coefficient = number(7, "damage_multiplier");
    data.skills[7].range = number(7, "forward_roll_distance");
    data.skills[7].area_radius = number(7, "explosion_radius");
    data.skills[7].duration_ticks = skill_ticks(7, "active_duration");

    data.skills[8].cooldown_ticks = skill_ticks(8, "cooldown");
    data.skills[8].damage_coefficient = number(8, "damage_multiplier");
    data.skills[8].range = number(8, "projectile_range");
    data.skills[8].duration_ticks = skill_ticks(8, "forced_move_duration");
    data.skills[8].pierce_count = CheckedInteger<std::uint8_t>(
        number(8, "pierce"), "skills", "$/entries/8/pierce");

    const auto &enemies = sources.documents.at("enemies")["entries"];
    for (std::size_t index = 0; index < enemies.size(); ++index)
    {
        const auto path = "$/entries/" + std::to_string(index);
        data.enemies[index].health = CheckedInteger<std::int32_t>(
            RequireInteger(enemies[index], "enemies", path, "base_hp"), "enemies",
            path + "/base_hp");
        data.enemies[index].move_speed = static_cast<float>(
            RequireNumber(enemies[index], "enemies", path, "movement_speed_mps"));
    }
    data.enemies[0].damage = CheckedInteger<std::int32_t>(
        ParameterNumber(enemies[0], "enemies", "$/entries/0", "base_damage"), "enemies",
        "$/entries/0/base_damage");
    data.enemies[0].attack_range = static_cast<float>(
        ParameterNumber(enemies[0], "enemies", "$/entries/0", "attack_range"));
    data.enemies[0].warning_ticks = ToTicks(
        ParameterNumber(enemies[0], "enemies", "$/entries/0", "telegraph_duration"),
        "enemies", "$/entries/0/parameters/telegraph_duration");
    data.enemies[0].attack_cooldown_ticks = ToTicks(
        ParameterNumber(enemies[0], "enemies", "$/entries/0", "reattack_interval"),
        "enemies", "$/entries/0/parameters/reattack_interval");

    data.enemies[1].damage = CheckedInteger<std::int32_t>(
        ParameterNumber(enemies[1], "enemies", "$/entries/1", "base_damage"), "enemies",
        "$/entries/1/base_damage");
    data.enemies[1].attack_range = static_cast<float>(ParameterNumber(
        enemies[1], "enemies", "$/entries/1", "maximum_hold_distance"));
    data.enemies[1].warning_ticks = ToTicks(
        ParameterNumber(enemies[1], "enemies", "$/entries/1", "telegraph_duration"),
        "enemies", "$/entries/1/parameters/telegraph_duration");
    data.enemies[1].attack_cooldown_ticks = ToTicks(
        ParameterNumber(enemies[1], "enemies", "$/entries/1", "reattack_interval"),
        "enemies", "$/entries/1/parameters/reattack_interval");
    data.enemies[1].projectile_speed = static_cast<float>(
        ParameterNumber(enemies[1], "enemies", "$/entries/1", "projectile_speed"));
    data.enemies[1].projectile_range = static_cast<float>(
        ParameterNumber(enemies[1], "enemies", "$/entries/1", "projectile_range"));

    data.enemies[2].damage = CheckedInteger<std::int32_t>(
        ParameterNumber(enemies[2], "enemies", "$/entries/2", "base_damage"), "enemies",
        "$/entries/2/base_damage");
    data.enemies[2].attack_range = static_cast<float>(
        ParameterNumber(enemies[2], "enemies", "$/entries/2", "stop_distance"));
    data.enemies[2].warning_ticks = ToTicks(
        ParameterNumber(enemies[2], "enemies", "$/entries/2", "telegraph_duration"),
        "enemies", "$/entries/2/parameters/telegraph_duration");
    data.enemies[2].projectile_range = static_cast<float>(
        ParameterNumber(enemies[2], "enemies", "$/entries/2", "explosion_radius"));

    const auto &bosses = sources.documents.at("bosses")["entries"];
    for (std::size_t index = 0; index < bosses.size(); ++index)
    {
        const auto path = "$/entries/" + std::to_string(index);
        data.bosses[index].health = CheckedInteger<std::int32_t>(
            RequireInteger(bosses[index], "bosses", path, "hp"), "bosses", path + "/hp");
        const auto &single = bosses[index]["single_pattern_recovery_seconds"];
        const auto &phase2 = bosses[index]["phase2_pattern_interval_seconds"];
        if (single.is_number())
        {
            data.bosses[index].recovery_ticks =
                ToTicks(single.get<double>(), "bosses", path + "/single_pattern_recovery_seconds");
        }
        else if (phase2.is_number())
        {
            data.bosses[index].recovery_ticks =
                ToTicks(phase2.get<double>(), "bosses", path + "/phase2_pattern_interval_seconds");
        }
        else
        {
            ThrowValidationError("bosses", path, "boss needs a pattern recovery interval");
        }
    }

    const auto &spawn = sources.documents.at("spawn_schedule");
    const auto &intervals = spawn["continuous_intervals"];
    const auto &compositions = spawn["compositions"];
    const auto rate_at = [&](std::int64_t seconds) {
        for (const auto &interval : intervals)
        {
            if (seconds >= interval["start_seconds"].get<std::int64_t>() &&
                seconds < interval["end_seconds"].get<std::int64_t>())
            {
                return interval["rate_per_second"].get<float>();
            }
        }
        ThrowValidationError("spawn_schedule", "$/continuous_intervals", "composition is outside intervals");
    };
    for (std::size_t index = 0; index < compositions.size(); ++index)
    {
        const auto &composition = compositions[index];
        const auto seconds = composition["start_seconds"].get<std::int64_t>();
        data.spawn_stages[index].start_minute = CheckedInteger<std::uint16_t>(
            seconds / 60, "spawn_schedule", "$/compositions/start_seconds");
        data.spawn_stages[index].per_second = rate_at(seconds);
        data.spawn_stages[index].weights = {
            CheckedInteger<std::uint8_t>(composition["melee_weight"].get<double>(),
                                         "spawn_schedule", "$/compositions/melee_weight"),
            CheckedInteger<std::uint8_t>(composition["ranged_weight"].get<double>(),
                                         "spawn_schedule", "$/compositions/ranged_weight"),
            CheckedInteger<std::uint8_t>(composition["suicide_weight"].get<double>(),
                                         "spawn_schedule", "$/compositions/suicide_weight"),
        };
    }
    const auto &waves = spawn["waves"];
    for (std::size_t index = 0; index < waves.size(); ++index)
    {
        data.waves[index].minute = CheckedInteger<std::uint16_t>(
            waves[index]["start_seconds"].get<double>() / 60.0, "spawn_schedule",
            "$/waves/start_seconds");
        data.waves[index].count = CheckedInteger<std::uint16_t>(
            waves[index]["total_count"].get<double>(), "spawn_schedule",
            "$/waves/total_count");
        data.waves[index].duration_ticks = CheckedInteger<hs::Tick>(
            waves[index]["duration_ticks"].get<double>(), "spawn_schedule",
            "$/waves/duration_ticks");
    }
    return content;
}

} // namespace hs::content
