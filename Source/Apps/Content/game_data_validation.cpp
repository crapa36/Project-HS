#include "content_cooker.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace hs::content
{

[[noreturn]] void ThrowValidationError(std::string_view file, std::string_view path,
                       std::string_view message)
{
    throw std::runtime_error(std::string(file) + ":" + std::string(path) + ": " +
                             std::string(message));
}

const Json &RequireMember(const Json &object, std::string_view file,
                          std::string_view path, std::string_view key)
{
    if (!object.is_object())
    {
        ThrowValidationError(file, path, "expected object");
    }
    const auto iterator = object.find(key);
    if (iterator == object.end())
    {
        ThrowValidationError(file, std::string(path) + "/" + std::string(key), "missing member");
    }
    return *iterator;
}

const Json &RequireArray(const Json &object, std::string_view file,
                         std::string_view path, std::string_view key)
{
    const auto &value = RequireMember(object, file, path, key);
    if (!value.is_array())
    {
        ThrowValidationError(file, std::string(path) + "/" + std::string(key), "expected array");
    }
    return value;
}

std::string RequireString(const Json &object, std::string_view file,
                          std::string_view path, std::string_view key)
{
    const auto &value = RequireMember(object, file, path, key);
    if (!value.is_string() || value.get_ref<const std::string &>().empty())
    {
        ThrowValidationError(file, std::string(path) + "/" + std::string(key),
             "expected non-empty string");
    }
    return value.get<std::string>();
}

double RequireNumber(const Json &object, std::string_view file,
                     std::string_view path, std::string_view key)
{
    const auto &value = RequireMember(object, file, path, key);
    if (!value.is_number())
    {
        ThrowValidationError(file, std::string(path) + "/" + std::string(key), "expected number");
    }
    const auto number = value.get<double>();
    if (!std::isfinite(number))
    {
        ThrowValidationError(file, std::string(path) + "/" + std::string(key),
             "number must be finite");
    }
    return number;
}

std::int64_t RequireInteger(const Json &object, std::string_view file,
                            std::string_view path, std::string_view key)
{
    const auto &value = RequireMember(object, file, path, key);
    if (!value.is_number_integer())
    {
        ThrowValidationError(file, std::string(path) + "/" + std::string(key), "expected integer");
    }
    return value.get<std::int64_t>();
}

void RequireExactKeys(const Json &object, std::string_view file,
                      std::initializer_list<std::string_view> expected)
{
    std::set<std::string, std::less<>> keys;
    for (const auto key : expected)
    {
        keys.emplace(key);
    }
    for (const auto &[key, unused] : object.items())
    {
        static_cast<void>(unused);
        if (!keys.erase(key))
        {
            ThrowValidationError(file, std::string("$/") + key, "unexpected root member");
        }
    }
    if (!keys.empty())
    {
        ThrowValidationError(file, std::string("$/") + *keys.begin(), "missing root member");
    }
}

void RequireCount(const Json &array, std::string_view file, std::string_view path,
                  std::size_t expected)
{
    if (!array.is_array() || array.size() != expected)
    {
        ThrowValidationError(file, path, "expected exactly " + std::to_string(expected) + " entries");
    }
}

bool IsStableId(std::string_view value)
{
    return !value.empty() && std::ranges::all_of(value, [](const char character) {
               const auto byte = static_cast<unsigned char>(character);
               return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
                      character == '.' || character == '_' || character == '-';
           });
}

std::unordered_set<std::string> CollectIds(const Json &entries, std::string_view file)
{
    std::unordered_set<std::string> ids;
    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        const auto path = "$/entries/" + std::to_string(index);
        const auto id = RequireString(entries[index], file, path, "id");
        if (!IsStableId(id))
        {
            ThrowValidationError(file, path + "/id", "invalid stable ID");
        }
        if (!ids.emplace(id).second)
        {
            ThrowValidationError(file, path + "/id", "duplicate ID");
        }
    }
    return ids;
}

void RequireReference(const std::unordered_set<std::string> &ids,
                      const std::string &reference, std::string_view file,
                      std::string_view path)
{
    if (!ids.contains(reference))
    {
        ThrowValidationError(file, path, "unknown reference '" + reference + "'");
    }
}

const Json &FindParameterValue(const Json &entry, std::string_view file,
                               std::string_view path, std::string_view key)
{
    const auto &parameters = RequireArray(entry, file, path, "parameters");
    const Json *found = nullptr;
    for (std::size_t index = 0; index < parameters.size(); ++index)
    {
        const auto parameter_path = std::string(path) + "/parameters/" +
                                    std::to_string(index);
        const auto parameter_key =
            RequireString(parameters[index], file, parameter_path, "key");
        RequireString(parameters[index], file, parameter_path, "unit");
        RequireMember(parameters[index], file, parameter_path, "value");
        if (parameter_key == key)
        {
            if (found)
            {
                ThrowValidationError(file, parameter_path + "/key", "duplicate parameter key");
            }
            found = &parameters[index]["value"];
        }
    }
    if (!found)
    {
        ThrowValidationError(file, std::string(path) + "/parameters", "missing parameter '" +
                                                           std::string(key) + "'");
    }
    return *found;
}

double ParameterNumber(const Json &entry, std::string_view file, std::string_view path,
                       std::string_view key)
{
    const auto &value = FindParameterValue(entry, file, path, key);
    if (!value.is_number())
    {
        ThrowValidationError(file, std::string(path) + "/parameters/" + std::string(key),
             "expected numeric parameter");
    }
    const auto number = value.get<double>();
    if (!std::isfinite(number))
    {
        ThrowValidationError(file, std::string(path) + "/parameters/" + std::string(key),
             "parameter must be finite");
    }
    return number;
}

void ValidateRoot(const Json &document, std::string_view name)
{
    if (!document.is_object())
    {
        ThrowValidationError(name, "$", "expected object root");
    }
    if (RequireString(document, name, "$", "$schema") !=
        "../../Schemas/game.schema.json")
    {
        ThrowValidationError(name, "$/\u0024schema", "unexpected schema reference");
    }
    const auto expected_version = name == "particles" ? 3 : 1;
    if (RequireInteger(document, name, "$", "schema_version") != expected_version)
    {
        ThrowValidationError(name, "$/schema_version", "unsupported schema version");
    }
    if (RequireString(document, name, "$", "category") != name)
    {
        ThrowValidationError(name, "$/category", "category must match file name");
    }
}

void ValidateDocuments(const ContentSources &sources)
{
    const auto &audio = sources.documents.at("audio_cues");
    const auto &bosses = sources.documents.at("bosses");
    const auto &characters = sources.documents.at("characters");
    const auto &enemies = sources.documents.at("enemies");
    const auto &level = sources.documents.at("level");
    const auto &materials = sources.documents.at("materials");
    const auto &particles = sources.documents.at("particles");
    const auto &relics = sources.documents.at("relics");
    const auto &skills = sources.documents.at("skills");
    const auto &spawn = sources.documents.at("spawn_schedule");
    const auto &stats = sources.documents.at("stats");
    const auto &ui = sources.documents.at("ui_strings");
    const auto &upgrades = sources.documents.at("upgrades");

    RequireExactKeys(audio, "audio_cues",
                     {"$schema", "schema_version", "category", "maximum_source_voices",
                      "submixes", "priority_order", "pause_behavior",
                      "silent_mode_when_device_missing", "entries"});
    RequireExactKeys(bosses, "bosses",
                     {"$schema", "schema_version", "category", "common", "entries"});
    RequireExactKeys(characters, "characters",
                     {"$schema", "schema_version", "category", "entries"});
    RequireExactKeys(enemies, "enemies",
                     {"$schema", "schema_version", "category", "spawn_scaling",
                      "common_rules", "entries"});
    RequireExactKeys(level, "level",
                     {"$schema", "schema_version", "category", "entries"});
    RequireExactKeys(materials, "materials",
                     {"$schema", "schema_version", "category", "entries",
                      "rendering_rules"});
    RequireExactKeys(particles, "particles",
                     {"$schema", "schema_version", "category", "gpu_capacity", "sprites", "effects",
                      "rules"});
    RequireExactKeys(relics, "relics",
                     {"$schema", "schema_version", "category",
                      "average_relics_per_session_target", "maximum_relic_slots",
                      "duplicate_acquisition_allowed", "all_unlocked_in_first_version",
                      "box_rules", "healing_pickup", "entries"});
    RequireExactKeys(skills, "skills",
                     {"$schema", "schema_version", "category", "active_skill_limit",
                      "automatic_slot_order", "acquisition_level", "maximum_level",
                      "upgrade_choices_per_skill", "common_rules", "entries"});
    RequireExactKeys(spawn, "spawn_schedule",
                     {"$schema", "schema_version", "category", "tick_rate_hz",
                      "continuous_intervals", "compositions", "waves", "placement",
                      "rules"});
    RequireExactKeys(stats, "stats",
                     {"$schema", "schema_version", "category", "entries"});
    RequireExactKeys(ui, "ui_strings",
                     {"$schema", "schema_version", "category", "locale",
                      "font_asset_id", "layout", "screen_flow", "screens", "entries",
                      "hidden_hud_items", "accessibility_rules"});
    RequireExactKeys(upgrades, "upgrades",
                     {"$schema", "schema_version", "category", "groups"});

    const auto &character_entries = RequireArray(characters, "characters", "$", "entries");
    const auto &skill_entries = RequireArray(skills, "skills", "$", "entries");
    const auto &enemy_entries = RequireArray(enemies, "enemies", "$", "entries");
    const auto &boss_entries = RequireArray(bosses, "bosses", "$", "entries");
    const auto &level_entries = RequireArray(level, "level", "$", "entries");
    const auto &stat_entries = RequireArray(stats, "stats", "$", "entries");
    const auto &material_entries = RequireArray(materials, "materials", "$", "entries");
    const auto &particle_entries = RequireArray(particles, "particles", "$", "effects");
    const auto &relic_entries = RequireArray(relics, "relics", "$", "entries");
    const auto &audio_entries = RequireArray(audio, "audio_cues", "$", "entries");
    const auto &ui_entries = RequireArray(ui, "ui_strings", "$", "entries");
    const auto &upgrade_groups = RequireArray(upgrades, "upgrades", "$", "groups");

    RequireCount(character_entries, "characters", "$/entries", 1);
    RequireCount(skill_entries, "skills", "$/entries", 9);
    RequireCount(enemy_entries, "enemies", "$/entries", 3);
    RequireCount(boss_entries, "bosses", "$/entries", 3);
    RequireCount(level_entries, "level", "$/entries", 1);
    RequireCount(stat_entries, "stats", "$/entries", 1);
    RequireCount(relic_entries, "relics", "$/entries", 12);
    RequireCount(upgrade_groups, "upgrades", "$/groups", 9);
    RequireCount(material_entries, "materials", "$/entries", 11);
    RequireCount(audio_entries, "audio_cues", "$/entries", 8);
    RequireCount(ui_entries, "ui_strings", "$/entries", 28);

    const auto character_ids = CollectIds(character_entries, "characters");
    const auto skill_ids = CollectIds(skill_entries, "skills");
    const auto enemy_ids = CollectIds(enemy_entries, "enemies");
    const auto boss_ids = CollectIds(boss_entries, "bosses");
    const auto stat_ids = CollectIds(stat_entries, "stats");
    const auto material_ids = CollectIds(material_entries, "materials");
    const auto particle_ids = CollectIds(particle_entries, "particles");
    constexpr std::array required_particle_ids{
        "particle.basic_attack", "particle.boss.area.activate", "particle.boss.dash.impact",
        "particle.boss.dash.start", "particle.boss.phase_change", "particle.boss.shockwave.release",
        "particle.boss.spawn", "particle.boss.volley.release",
        "particle.common.enemy_death", "particle.common.explosion_large", "particle.common.explosion_small",
        "particle.common.heal", "particle.common.heavy_hit", "particle.common.hit", "particle.common.mark_apply",
        "particle.common.mark_trigger", "particle.common.player_death", "particle.common.player_hit",
        "particle.common.pull", "particle.common.push", "particle.enemy.melee.hit",
        "particle.enemy.melee.windup", "particle.enemy.ranged.release",
        "particle.enemy.suicide.charge", "particle.enemy.suicide.explosion",
        "particle.line.burn_transfer", "particle.line.relic_chain", "particle.line.ricochet",
        "particle.pickup.heal_collect", "particle.pickup.magnet_collect", "particle.pickup.relic_collect",
        "particle.pickup.xp_spawn", "particle.relic.bleed_burn_explosion",
        "particle.relic.combat_chain", "particle.relic.damage_push",
        "particle.relic.radial_arrows", "particle.skill.arrow_rain", "particle.skill.arrow_rain.area_pulse",
        "particle.skill.arrow_rain.impact", "particle.skill.charged_shot", "particle.skill.charged_shot.pulse",
        "particle.skill.charged_shot.ready", "particle.skill.explosive_arrow",
        "particle.skill.explosive_arrow.main", "particle.skill.explosive_arrow.secondary",
        "particle.skill.damage_area.pulse", "particle.skill.fire_area.pulse",
        "particle.skill.multishot", "particle.skill.piercing_shot", "particle.skill.piercing_shot.trail_pulse",
        "particle.skill.retreat_shot", "particle.skill.retreat_shot.land",
        "particle.skill.retreat_shot.move", "particle.skill.ricochet_arrow",
        "particle.skill.trap.idle",
        "particle.skill.ricochet_arrow.hit", "particle.skill.trap", "particle.skill.trap.arm",
        "particle.skill.trap.trigger", "particle.status.bleed_apply", "particle.status.bleed_tick",
        "particle.status.burn_apply", "particle.status.burn_tick", "particle.status.slow_apply",
        "particle.status.slow_area"};
    for (const auto id : required_particle_ids)
        if (!particle_ids.contains(id))
            ThrowValidationError("particles", "$/effects", std::string("required particle effect is missing: ") + id);
    const auto relic_ids = CollectIds(relic_entries, "relics");
    const auto audio_ids = CollectIds(audio_entries, "audio_cues");
    const auto ui_ids = CollectIds(ui_entries, "ui_strings");
    static_cast<void>(character_ids);
    static_cast<void>(relic_ids);
    static_cast<void>(audio_ids);

    constexpr std::array<std::string_view, 9> expected_skills = {
        "skill.basic_attack",   "skill.piercing_shot", "skill.multishot",
        "skill.charged_shot",   "skill.explosive_arrow", "skill.ricochet_arrow",
        "skill.arrow_rain",     "skill.trap",          "skill.retreat_shot",
    };
    constexpr std::array<std::string_view, 3> expected_enemies = {
        "enemy.melee", "enemy.ranged", "enemy.suicide"};
    constexpr std::array<std::string_view, 3> expected_bosses = {
        "boss.mid_5m", "boss.mid_10m", "boss.final_15m"};
    for (std::size_t index = 0; index < expected_skills.size(); ++index)
    {
        if (RequireString(skill_entries[index], "skills",
                          "$/entries/" + std::to_string(index), "id") !=
            expected_skills[index])
        {
            ThrowValidationError("skills", "$/entries/" + std::to_string(index) + "/id",
                 "skill order is part of cooked ABI");
        }
        RequireReference(material_ids,
                         RequireString(skill_entries[index], "skills",
                                       "$/entries/" + std::to_string(index), "material_id"),
                         "skills", "$/entries/" + std::to_string(index) + "/material_id");
        RequireReference(particle_ids,
                         RequireString(skill_entries[index], "skills",
                                       "$/entries/" + std::to_string(index), "particle_id"),
                         "skills", "$/entries/" + std::to_string(index) + "/particle_id");
    }
    for (std::size_t index = 0; index < expected_enemies.size(); ++index)
    {
        const auto path = "$/entries/" + std::to_string(index);
        if (RequireString(enemy_entries[index], "enemies", path, "id") !=
            expected_enemies[index])
        {
            ThrowValidationError("enemies", path + "/id", "enemy order is part of cooked ABI");
        }
        RequirePositive(RequireInteger(enemy_entries[index], "enemies", path, "base_hp"),
                        "enemies", path + "/base_hp");
        RequirePositive(
            RequireNumber(enemy_entries[index], "enemies", path, "movement_speed_mps"),
            "enemies", path + "/movement_speed_mps");
        RequireReference(material_ids,
                         RequireString(enemy_entries[index], "enemies", path, "material_id"),
                         "enemies", path + "/material_id");
    }
    for (std::size_t index = 0; index < expected_bosses.size(); ++index)
    {
        const auto path = "$/entries/" + std::to_string(index);
        if (RequireString(boss_entries[index], "bosses", path, "id") !=
            expected_bosses[index])
        {
            ThrowValidationError("bosses", path + "/id", "boss order is part of cooked ABI");
        }
        RequirePositive(RequireInteger(boss_entries[index], "bosses", path, "hp"),
                        "bosses", path + "/hp");
        RequireReference(material_ids,
                         RequireString(boss_entries[index], "bosses", path, "material_id"),
                         "bosses", path + "/material_id");
    }

    const auto &character = character_entries[0];
    if (RequireString(character, "characters", "$/entries/0", "id") !=
            "character.archer" ||
        RequireString(RequireMember(character, "characters", "$/entries/0", "movement"),
                      "characters", "$/entries/0/movement", "input") !=
            "RMB")
    {
        ThrowValidationError("characters", "$/entries/0", "first version must contain RMB archer");
    }
    RequireReference(stat_ids,
                     RequireString(character, "characters", "$/entries/0", "stat_profile_id"),
                     "characters", "$/entries/0/stat_profile_id");
    RequireReference(material_ids,
                     RequireString(character, "characters", "$/entries/0", "material_id"),
                     "characters", "$/entries/0/material_id");

    if (RequireInteger(skills, "skills", "$", "active_skill_limit") != 4 ||
        RequireInteger(skills, "skills", "$", "upgrade_choices_per_skill") != 4)
    {
        ThrowValidationError("skills", "$", "expected four active slots and four selected upgrades");
    }
    const auto &slots = RequireArray(skills, "skills", "$", "automatic_slot_order");
    RequireCount(slots, "skills", "$/automatic_slot_order", 4);
    constexpr std::array<std::string_view, 4> expected_slots = {"Q", "W", "E", "R"};
    for (std::size_t index = 0; index < expected_slots.size(); ++index)
    {
        if (!slots[index].is_string() || slots[index].get<std::string>() != expected_slots[index])
        {
            ThrowValidationError("skills", "$/automatic_slot_order/" + std::to_string(index),
                 "slot order must be Q, W, E, R");
        }
    }

    std::unordered_set<std::string> upgraded_skills;
    for (std::size_t group_index = 0; group_index < upgrade_groups.size(); ++group_index)
    {
        const auto path = "$/groups/" + std::to_string(group_index);
        const auto skill_id =
            RequireString(upgrade_groups[group_index], "upgrades", path, "skill_id");
        RequireReference(skill_ids, skill_id, "upgrades", path + "/skill_id");
        if (!upgraded_skills.emplace(skill_id).second)
        {
            ThrowValidationError("upgrades", path + "/skill_id", "duplicate skill upgrade group");
        }
        const auto &entries =
            RequireArray(upgrade_groups[group_index], "upgrades", path, "entries");
        RequireCount(entries, "upgrades", path + "/entries", 8);
        for (std::size_t entry_index = 0; entry_index < entries.size(); ++entry_index)
        {
            const auto entry_path = path + "/entries/" + std::to_string(entry_index);
            if (RequireInteger(entries[entry_index], "upgrades", entry_path, "ordinal") !=
                static_cast<std::int64_t>(entry_index + 1))
            {
                ThrowValidationError("upgrades", entry_path + "/ordinal", "ordinal must be 1 through 8");
            }
            RequireString(entries[entry_index], "upgrades", entry_path, "logic_id");
            RequireArray(entries[entry_index], "upgrades", entry_path, "parameters");
        }
    }

    for (std::size_t index = 0; index < relic_entries.size(); ++index)
    {
        const auto path = "$/entries/" + std::to_string(index);
        if (RequireInteger(relic_entries[index], "relics", path, "ordinal") !=
            static_cast<std::int64_t>(index + 1))
        {
            ThrowValidationError("relics", path + "/ordinal", "ordinal must be 1 through 12");
        }
    }

    const auto &base = RequireMember(stat_entries[0], "stats", "$/entries/0", "base_values");
    RequirePositive(RequireNumber(base, "stats", "$/entries/0/base_values", "maximum_hp"),
                    "stats", "$/entries/0/base_values/maximum_hp");
    RequirePositive(RequireNumber(base, "stats", "$/entries/0/base_values", "attack_power"),
                    "stats", "$/entries/0/base_values/attack_power");
    RequirePositive(
        RequireNumber(base, "stats", "$/entries/0/base_values",
                      "basic_attack_rate_per_second"),
        "stats", "$/entries/0/base_values/basic_attack_rate_per_second");
    RequireCount(RequireArray(stat_entries[0], "stats", "$/entries/0", "allocations"),
                 "stats", "$/entries/0/allocations", 6);
    RequireCount(RequireArray(stat_entries[0], "stats", "$/entries/0", "statuses"),
                 "stats", "$/entries/0/statuses", 3);

    const auto &arena = RequireMember(level_entries[0], "level", "$/entries/0", "arena");
    RequirePositive(RequireNumber(arena, "level", "$/entries/0/arena", "width_m"),
                    "level", "$/entries/0/arena/width_m");
    RequirePositive(RequireNumber(arena, "level", "$/entries/0/arena", "depth_m"),
                    "level", "$/entries/0/arena/depth_m");
    const auto &experience =
        RequireMember(level_entries[0], "level", "$/entries/0", "experience");
    const auto &rewards = RequireArray(experience, "level", "$/entries/0/experience", "rewards");
    RequireCount(rewards, "level", "$/entries/0/experience/rewards", 6);
    for (std::size_t index = 0; index < rewards.size(); ++index)
    {
        const auto path = "$/entries/0/experience/rewards/" + std::to_string(index);
        const auto reference = RequireString(rewards[index], "level", path, "source_id");
        if (!enemy_ids.contains(reference) && !boss_ids.contains(reference))
        {
            ThrowValidationError("level", path + "/source_id", "unknown enemy or boss reference");
        }
    }

    const auto &intervals =
        RequireArray(spawn, "spawn_schedule", "$", "continuous_intervals");
    const auto &compositions = RequireArray(spawn, "spawn_schedule", "$", "compositions");
    const auto &waves = RequireArray(spawn, "spawn_schedule", "$", "waves");
    RequireCount(intervals, "spawn_schedule", "$/continuous_intervals", 6);
    RequireCount(compositions, "spawn_schedule", "$/compositions", 7);
    RequireCount(waves, "spawn_schedule", "$/waves", 5);
    if (RequireInteger(spawn, "spawn_schedule", "$", "tick_rate_hz") != 60)
    {
        ThrowValidationError("spawn_schedule", "$/tick_rate_hz", "fixed simulation rate must be 60 Hz");
    }
    std::int64_t previous_end = 0;
    for (std::size_t index = 0; index < intervals.size(); ++index)
    {
        const auto path = "$/continuous_intervals/" + std::to_string(index);
        const auto start =
            RequireInteger(intervals[index], "spawn_schedule", path, "start_seconds");
        const auto end =
            RequireInteger(intervals[index], "spawn_schedule", path, "end_seconds");
        const auto rate =
            RequireNumber(intervals[index], "spawn_schedule", path, "rate_per_second");
        if (start != previous_end || end <= start || rate <= 0)
        {
            ThrowValidationError("spawn_schedule", path, "intervals must be contiguous and positive");
        }
        previous_end = end;
    }
    if (previous_end != 900)
    {
        ThrowValidationError("spawn_schedule", "$/continuous_intervals", "intervals must end at 900 seconds");
    }
    std::int64_t previous_start = -1;
    for (std::size_t index = 0; index < compositions.size(); ++index)
    {
        const auto path = "$/compositions/" + std::to_string(index);
        const auto start =
            RequireInteger(compositions[index], "spawn_schedule", path, "start_seconds");
        const auto melee =
            RequireInteger(compositions[index], "spawn_schedule", path, "melee_weight");
        const auto ranged =
            RequireInteger(compositions[index], "spawn_schedule", path, "ranged_weight");
        const auto suicide =
            RequireInteger(compositions[index], "spawn_schedule", path, "suicide_weight");
        if (start <= previous_start || melee < 0 || ranged < 0 || suicide < 0 ||
            melee + ranged + suicide != 100)
        {
            ThrowValidationError("spawn_schedule", path,
                 "composition starts must increase and weights must total 100");
        }
        previous_start = start;
    }
    for (std::size_t index = 0; index < waves.size(); ++index)
    {
        const auto path = "$/waves/" + std::to_string(index);
        const auto seconds =
            RequireInteger(waves[index], "spawn_schedule", path, "duration_seconds");
        const auto ticks =
            RequireInteger(waves[index], "spawn_schedule", path, "duration_ticks");
        if (RequireInteger(waves[index], "spawn_schedule", path, "total_count") <= 0 ||
            seconds <= 0 || ticks != seconds * 60)
        {
            ThrowValidationError("spawn_schedule", path, "wave count and 60 Hz duration must agree");
        }
    }

    if (RequireInteger(particles, "particles", "$", "gpu_capacity") != 10'000 ||
        RequireInteger(audio, "audio_cues", "$", "maximum_source_voices") != 64)
    {
        ThrowValidationError("content", "$", "particle and audio capacities do not match runtime contracts");
    }
    if (RequireString(ui, "ui_strings", "$", "locale") != "ko-KR")
    {
        ThrowValidationError("ui_strings", "$/locale", "only ko-KR is supported");
    }
    const auto &screens = RequireArray(ui, "ui_strings", "$", "screens");
    for (std::size_t screen_index = 0; screen_index < screens.size(); ++screen_index)
    {
        const auto path = "$/screens/" + std::to_string(screen_index);
        const auto &string_ids = RequireArray(screens[screen_index], "ui_strings", path,
                                              "string_ids");
        for (std::size_t index = 0; index < string_ids.size(); ++index)
        {
            if (!string_ids[index].is_string())
            {
                ThrowValidationError("ui_strings", path + "/string_ids/" + std::to_string(index),
                     "expected string ID");
            }
            RequireReference(ui_ids, string_ids[index].get<std::string>(), "ui_strings",
                             path + "/string_ids/" + std::to_string(index));
        }
    }

    for (std::size_t index = 0; index < skill_entries.size(); ++index)
    {
        const auto path = "$/entries/" + std::to_string(index);
        const auto &parameters = RequireArray(skill_entries[index], "skills", path, "parameters");
        std::unordered_set<std::string> keys;
        for (std::size_t parameter_index = 0; parameter_index < parameters.size();
             ++parameter_index)
        {
            const auto parameter_path =
                path + "/parameters/" + std::to_string(parameter_index);
            const auto key =
                RequireString(parameters[parameter_index], "skills", parameter_path, "key");
            if (!keys.emplace(key).second)
            {
                ThrowValidationError("skills", parameter_path + "/key", "duplicate parameter key");
            }
            RequireString(parameters[parameter_index], "skills", parameter_path, "unit");
            const auto &value =
                RequireMember(parameters[parameter_index], "skills", parameter_path, "value");
            if (value.is_number())
            {
                const auto number = value.get<double>();
                if (!std::isfinite(number) || number < 0.0)
                {
                    ThrowValidationError("skills", parameter_path + "/value",
                         "numeric skill parameter must be finite and non-negative");
                }
            }
        }
    }
}

ContentSources LoadAndValidateSources()
{
    ValidateGameDataDirectory();
    const auto schema_text = ReadRequiredText(HS_SCHEMA_FILE);
    Json schema;
    try
    {
        schema = Json::parse(schema_text);
    }
    catch (const Json::exception &exception)
    {
        throw std::runtime_error(std::string(HS_SCHEMA_FILE) + ": " + exception.what());
    }
    if (!schema.is_object() || schema.value("$schema", std::string{}) !=
                                   "https://json-schema.org/draft/2020-12/schema" ||
        !schema.contains("oneOf") || !schema["oneOf"].is_array() ||
        schema["oneOf"].size() != kDocumentNames.size() || !schema.contains("$defs") ||
        !schema["$defs"].is_object())
    {
        throw std::runtime_error(std::string(HS_SCHEMA_FILE) +
                                 ": expected complete draft 2020-12 category schema");
    }

    ContentSources sources;
    sources.inventory = LoadContentSourceInventory();
    for (std::size_t index = 0; index < kDocumentNames.size(); ++index)
    {
        const auto name = kDocumentNames[index];
        const auto path = GameDataCategoryPath(name);
        const auto &text = sources.inventory.document_text[index];
        Json document;
        try
        {
            document = Json::parse(text);
        }
        catch (const Json::exception &exception)
        {
            throw std::runtime_error(path.string() + ": " + exception.what());
        }
        ValidateRoot(document, name);
        sources.documents.emplace(name, std::move(document));
    }
    ValidateDocuments(sources);
    return sources;
}

} // namespace hs::content
