#include <hs/core/cooked_format.hpp>
#include <hs/core/cooked_particle_effects.hpp>
#include <hs/game_rules/simulation_rules.hpp>
#include <hs/presentation/presentation_catalog.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <fbxsdk.h>
#include <nlohmann/json.hpp>
#include <DirectXTex.h>

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef HS_GAME_DATA_DIRECTORY
#define HS_GAME_DATA_DIRECTORY "ContentSource/GameData"
#endif

#ifndef HS_SCHEMA_FILE
#define HS_SCHEMA_FILE "Schemas/game.schema.json"
#endif

#ifndef HS_VFX_TEXTURE_DIRECTORY
#define HS_VFX_TEXTURE_DIRECTORY "ContentSource/Textures/VFX"
#endif

#ifndef HS_CHARACTER_MODEL
#define HS_CHARACTER_MODEL "ContentSource/Models/Characters/Archer/ErikaArcher.fbx"
#endif

#ifndef HS_CHARACTER_ANIMATION_DIRECTORY
#define HS_CHARACTER_ANIMATION_DIRECTORY "ContentSource/Animations/Characters/Archer"
#endif

namespace
{

using Json = nlohmann::json;

struct ComScope
{
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    ~ComScope()
    {
        if (SUCCEEDED(result))
        {
            CoUninitialize();
        }
    }
};

constexpr std::array<std::string_view, 13> kDocumentNames = {
    "audio_cues", "bosses",   "characters", "enemies", "level",
    "materials",  "particles", "relics",    "skills",  "spawn_schedule",
    "stats",      "ui_strings", "upgrades",
};

struct ContentSources
{
    std::unordered_map<std::string, Json> documents;
    std::string source_bytes;
    std::string all_source_bytes;
};

struct CharacterCookResult
{
    struct Material
    {
        std::filesystem::path diffuse;
        std::filesystem::path normal;
        std::string name;
    };

    std::vector<hs::SkinnedVertex> vertices;
    std::vector<hs::CharacterClipHeader> clips;
    std::vector<std::uint16_t> parents;
    std::vector<std::array<float, 16>> inverse_bind_matrices;
    std::vector<hs::CharacterLocalTransform> transforms;
    std::vector<float> upper_body_weights;
    std::uint32_t mesh_count{};
    std::uint32_t bone_count{};
    std::vector<Material> materials;
    std::array<float, 3> bounds_min{
        std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    std::array<float, 3> bounds_max{
        std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()};
};

struct DdsPixelFormat
{
    std::uint32_t size{32};
    std::uint32_t flags{0x41};
    std::uint32_t four_cc{};
    std::uint32_t rgb_bit_count{32};
    std::uint32_t red_mask{0x000000ff};
    std::uint32_t green_mask{0x0000ff00};
    std::uint32_t blue_mask{0x00ff0000};
    std::uint32_t alpha_mask{0xff000000};
};

struct DdsHeader
{
    std::uint32_t size{124};
    std::uint32_t flags{0x100F};
    std::uint32_t height{1};
    std::uint32_t width{1};
    std::uint32_t pitch{4};
    std::uint32_t depth{};
    std::uint32_t mip_count{1};
    std::array<std::uint32_t, 11> reserved{};
    DdsPixelFormat pixel_format;
    std::uint32_t caps{0x1000};
    std::array<std::uint32_t, 4> remaining_caps{};
};

[[noreturn]] void Fail(std::string_view file, std::string_view path,
                       std::string_view message)
{
    throw std::runtime_error(std::string(file) + ":" + std::string(path) + ": " +
                             std::string(message));
}

std::string ReadText(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        throw std::runtime_error(path.string() + ": cannot open file");
    }
    const auto size = stream.tellg();
    if (size <= 0)
    {
        throw std::runtime_error(path.string() + ": file is empty");
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    stream.seekg(0);
    if (!stream.read(text.data(), static_cast<std::streamsize>(size)))
    {
        throw std::runtime_error(path.string() + ": cannot read complete file");
    }
    return text;
}

const Json &RequireMember(const Json &object, std::string_view file,
                          std::string_view path, std::string_view key)
{
    if (!object.is_object())
    {
        Fail(file, path, "expected object");
    }
    const auto iterator = object.find(key);
    if (iterator == object.end())
    {
        Fail(file, std::string(path) + "/" + std::string(key), "missing member");
    }
    return *iterator;
}

const Json &RequireArray(const Json &object, std::string_view file,
                         std::string_view path, std::string_view key)
{
    const auto &value = RequireMember(object, file, path, key);
    if (!value.is_array())
    {
        Fail(file, std::string(path) + "/" + std::string(key), "expected array");
    }
    return value;
}

std::string RequireString(const Json &object, std::string_view file,
                          std::string_view path, std::string_view key)
{
    const auto &value = RequireMember(object, file, path, key);
    if (!value.is_string() || value.get_ref<const std::string &>().empty())
    {
        Fail(file, std::string(path) + "/" + std::string(key),
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
        Fail(file, std::string(path) + "/" + std::string(key), "expected number");
    }
    const auto number = value.get<double>();
    if (!std::isfinite(number))
    {
        Fail(file, std::string(path) + "/" + std::string(key),
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
        Fail(file, std::string(path) + "/" + std::string(key), "expected integer");
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
            Fail(file, std::string("$/") + key, "unexpected root member");
        }
    }
    if (!keys.empty())
    {
        Fail(file, std::string("$/") + *keys.begin(), "missing root member");
    }
}

void RequireCount(const Json &array, std::string_view file, std::string_view path,
                  std::size_t expected)
{
    if (!array.is_array() || array.size() != expected)
    {
        Fail(file, path, "expected exactly " + std::to_string(expected) + " entries");
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
            Fail(file, path + "/id", "invalid stable ID");
        }
        if (!ids.emplace(id).second)
        {
            Fail(file, path + "/id", "duplicate ID");
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
        Fail(file, path, "unknown reference '" + reference + "'");
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
                Fail(file, parameter_path + "/key", "duplicate parameter key");
            }
            found = &parameters[index]["value"];
        }
    }
    if (!found)
    {
        Fail(file, std::string(path) + "/parameters", "missing parameter '" +
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
        Fail(file, std::string(path) + "/parameters/" + std::string(key),
             "expected numeric parameter");
    }
    const auto number = value.get<double>();
    if (!std::isfinite(number))
    {
        Fail(file, std::string(path) + "/parameters/" + std::string(key),
             "parameter must be finite");
    }
    return number;
}

template <typename Number>
void RequirePositive(Number value, std::string_view file, std::string_view path)
{
    if (!(value > Number{}))
    {
        Fail(file, path, "must be greater than zero");
    }
}

void ValidateRoot(const Json &document, std::string_view name)
{
    if (!document.is_object())
    {
        Fail(name, "$", "expected object root");
    }
    if (RequireString(document, name, "$", "$schema") !=
        "../../Schemas/game.schema.json")
    {
        Fail(name, "$/\u0024schema", "unexpected schema reference");
    }
    const auto expected_version = name == "particles" ? 3 : 1;
    if (RequireInteger(document, name, "$", "schema_version") != expected_version)
    {
        Fail(name, "$/schema_version", "unsupported schema version");
    }
    if (RequireString(document, name, "$", "category") != name)
    {
        Fail(name, "$/category", "category must match file name");
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
            Fail("particles", "$/effects", std::string("required particle effect is missing: ") + id);
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
            Fail("skills", "$/entries/" + std::to_string(index) + "/id",
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
            Fail("enemies", path + "/id", "enemy order is part of cooked ABI");
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
            Fail("bosses", path + "/id", "boss order is part of cooked ABI");
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
        Fail("characters", "$/entries/0", "first version must contain RMB archer");
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
        Fail("skills", "$", "expected four active slots and four selected upgrades");
    }
    const auto &slots = RequireArray(skills, "skills", "$", "automatic_slot_order");
    RequireCount(slots, "skills", "$/automatic_slot_order", 4);
    constexpr std::array<std::string_view, 4> expected_slots = {"Q", "W", "E", "R"};
    for (std::size_t index = 0; index < expected_slots.size(); ++index)
    {
        if (!slots[index].is_string() || slots[index].get<std::string>() != expected_slots[index])
        {
            Fail("skills", "$/automatic_slot_order/" + std::to_string(index),
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
            Fail("upgrades", path + "/skill_id", "duplicate skill upgrade group");
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
                Fail("upgrades", entry_path + "/ordinal", "ordinal must be 1 through 8");
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
            Fail("relics", path + "/ordinal", "ordinal must be 1 through 12");
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
            Fail("level", path + "/source_id", "unknown enemy or boss reference");
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
        Fail("spawn_schedule", "$/tick_rate_hz", "fixed simulation rate must be 60 Hz");
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
            Fail("spawn_schedule", path, "intervals must be contiguous and positive");
        }
        previous_end = end;
    }
    if (previous_end != 900)
    {
        Fail("spawn_schedule", "$/continuous_intervals", "intervals must end at 900 seconds");
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
            Fail("spawn_schedule", path,
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
            Fail("spawn_schedule", path, "wave count and 60 Hz duration must agree");
        }
    }

    if (RequireInteger(particles, "particles", "$", "gpu_capacity") != 10'000 ||
        RequireInteger(audio, "audio_cues", "$", "maximum_source_voices") != 64)
    {
        Fail("content", "$", "particle and audio capacities do not match runtime contracts");
    }
    if (RequireString(ui, "ui_strings", "$", "locale") != "ko-KR")
    {
        Fail("ui_strings", "$/locale", "only ko-KR is supported");
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
                Fail("ui_strings", path + "/string_ids/" + std::to_string(index),
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
                Fail("skills", parameter_path + "/key", "duplicate parameter key");
            }
            RequireString(parameters[parameter_index], "skills", parameter_path, "unit");
            const auto &value =
                RequireMember(parameters[parameter_index], "skills", parameter_path, "value");
            if (value.is_number())
            {
                const auto number = value.get<double>();
                if (!std::isfinite(number) || number < 0.0)
                {
                    Fail("skills", parameter_path + "/value",
                         "numeric skill parameter must be finite and non-negative");
                }
            }
        }
    }
}

ContentSources LoadAndValidateSources()
{
    const std::filesystem::path root = HS_GAME_DATA_DIRECTORY;
    std::set<std::string, std::less<>> found_files;
    for (const auto &entry : std::filesystem::directory_iterator(root))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".json")
        {
            found_files.emplace(entry.path().stem().string());
        }
    }
    std::set<std::string, std::less<>> expected_files;
    for (const auto name : kDocumentNames)
    {
        expected_files.emplace(name);
    }
    if (found_files != expected_files)
    {
        throw std::runtime_error(root.string() +
                                 ": expected exactly the 13 declared category JSON files");
    }

    const auto schema_text = ReadText(HS_SCHEMA_FILE);
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
    for (const auto name : kDocumentNames)
    {
        const auto path = root / (std::string(name) + ".json");
        auto text = ReadText(path);
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
        sources.all_source_bytes.append(name);
        sources.all_source_bytes.push_back('\0');
        sources.all_source_bytes.append(text);
        sources.all_source_bytes.push_back('\0');
        if (name != "particles")
        {
            sources.source_bytes.append(name);
            sources.source_bytes.push_back('\0');
            sources.source_bytes.append(text);
            sources.source_bytes.push_back('\0');
        }
        sources.documents.emplace(name, std::move(document));
    }
    const auto append_asset = [&](std::string_view name,
                                  const std::filesystem::path &path) {
        sources.source_bytes.append(name);
        sources.source_bytes.push_back('\0');
        sources.source_bytes.append(ReadText(path));
        sources.source_bytes.push_back('\0');
        sources.all_source_bytes.append(name);
        sources.all_source_bytes.push_back('\0');
        sources.all_source_bytes.append(ReadText(path));
        sources.all_source_bytes.push_back('\0');
    };
    const auto animation_root =
        std::filesystem::path(HS_CHARACTER_ANIMATION_DIRECTORY);
    append_asset("character/archer/model", HS_CHARACTER_MODEL);
    append_asset("character/archer/idle", animation_root / "Idle.fbx");
    append_asset("character/archer/run", animation_root / "RunForward.fbx");
    append_asset("character/archer/draw", animation_root / "DrawArrow.fbx");
    append_asset("character/archer/recoil", animation_root / "AimRecoil.fbx");
    append_asset("character/archer/death", animation_root / "DeathBackward.fbx");
    ValidateDocuments(sources);
    return sources;
}

template <typename Type, typename Source>
Type CheckedInteger(Source value, std::string_view file, std::string_view path)
{
    const auto converted = static_cast<long double>(value);
    if (!std::isfinite(converted) ||
        converted < static_cast<long double>((std::numeric_limits<Type>::min)()) ||
        converted > static_cast<long double>((std::numeric_limits<Type>::max)()) ||
        std::floor(converted) != converted)
    {
        Fail(file, path, "integer value is outside cooked range");
    }
    return static_cast<Type>(converted);
}

hs::Tick ToTicks(double seconds, std::string_view file, std::string_view path)
{
    if (!std::isfinite(seconds) || seconds < 0.0)
        Fail(file, path, "duration must be finite and non-negative");
    return CheckedInteger<hs::Tick>(std::llround(seconds * 60.0), file, path);
}

struct BuiltGameData
{
    hs::SimulationRules simulation_rules{};
    hs::PresentationCatalog presentation{};
};

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
        Fail("stats", "$/entries/0/statuses", "bleed and burn tick intervals must match");

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
            Fail("relics", path, "UTF-8 text exceeds cooked capacity");
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
        Fail("level", "$/entries/0/arena", "GameData v1 requires a square arena");
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
            Fail("skills", path + "/logic_id",
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
            Fail("bosses", path, "boss needs a pattern recovery interval");
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
        Fail("spawn_schedule", "$/continuous_intervals", "composition is outside intervals");
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

bool AtomicWrite(const std::filesystem::path &path, std::span<const std::byte> bytes,
                 std::string &error_message)
{
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream.write(reinterpret_cast<const char *>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size())))
        {
            error_message = "cannot write temporary file";
            return false;
        }
    }
    const auto handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        error_message = "cannot open temporary file for flush";
        DeleteFileW(temporary.c_str());
        return false;
    }
    const auto flushed = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!flushed || !MoveFileExW(temporary.c_str(), path.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        error_message = "atomic replacement failed with Win32 error " +
                        std::to_string(GetLastError());
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

template <typename T>
bool WriteCookedTable(const std::filesystem::path &path, const T &data,
                      std::uint64_t schema_hash, std::uint64_t source_hash,
                      std::string &error_message)
{
    static_assert(std::is_trivially_copyable_v<T>);
    const auto payload = std::as_bytes(std::span(&data, 1));
    hs::CookedHeader header;
    header.schema_hash = schema_hash;
    header.source_hash = source_hash;
    header.table_count = 1;
    header.payload_size = static_cast<std::uint32_t>(payload.size());
    header.payload_crc32 = hs::Crc32(payload);

    std::vector<std::byte> file(sizeof(header) + payload.size());
    std::memcpy(file.data(), &header, sizeof(header));
    std::memcpy(file.data() + sizeof(header), payload.data(), payload.size());
    return AtomicWrite(path, file, error_message);
}

template <typename T, std::size_t N>
T ParseEnum(std::string_view value, const std::array<std::pair<std::string_view, T>, N> &values,
            std::string_view path)
{
    const auto found = std::ranges::find(values, value, &std::pair<std::string_view, T>::first);
    if (found == values.end()) Fail("particles", path, "unknown enum value");
    return found->second;
}

struct ParticleSpriteSource
{
    std::string id;
    std::filesystem::path file;
    std::uint8_t frame_columns{1};
    std::uint8_t frame_rows{1};
};

bool WriteCookedParticleEffects(const std::filesystem::path &path, const Json &document,
                                std::vector<ParticleSpriteSource> &sprite_sources,
                                std::string &error_message)
{
    constexpr std::array<std::pair<std::string_view, hs::ParticleShape>, 5> shapes{{
        std::pair{std::string_view{"point"}, hs::ParticleShape::Point}, {std::string_view{"sphere"}, hs::ParticleShape::Sphere},
        {std::string_view{"disc"}, hs::ParticleShape::Disc}, {std::string_view{"ring"}, hs::ParticleShape::Ring},
        {std::string_view{"line"}, hs::ParticleShape::Line}}};
    constexpr std::array<std::pair<std::string_view, hs::ParticleVelocity>, 5> velocities{{
        std::pair{std::string_view{"direction"}, hs::ParticleVelocity::Direction}, {std::string_view{"cone"}, hs::ParticleVelocity::Cone},
        {std::string_view{"radial"}, hs::ParticleVelocity::Radial}, {std::string_view{"inward"}, hs::ParticleVelocity::Inward},
        {std::string_view{"upward"}, hs::ParticleVelocity::Upward}}};
    constexpr std::array<std::pair<std::string_view, hs::ParticleFacing>, 3> facings{{
        std::pair{std::string_view{"camera"}, hs::ParticleFacing::Camera}, {std::string_view{"velocity"}, hs::ParticleFacing::Velocity},
        {std::string_view{"ground"}, hs::ParticleFacing::Ground}}};
    constexpr std::array<std::pair<std::string_view, hs::VfxRenderer>, 4> renderers{{
        std::pair{std::string_view{"sprite"}, hs::VfxRenderer::Sprite},
        {std::string_view{"ground"}, hs::VfxRenderer::Ground},
        {std::string_view{"segment"}, hs::VfxRenderer::Segment},
        {std::string_view{"mesh"}, hs::VfxRenderer::Mesh}}};
    constexpr std::array<std::pair<std::string_view, hs::VfxPrimitive>, 17> primitives{{
        std::pair{std::string_view{"soft"}, hs::VfxPrimitive::Soft},
        {std::string_view{"disc"}, hs::VfxPrimitive::Disc},
        {std::string_view{"ring"}, hs::VfxPrimitive::Ring},
        {std::string_view{"sector"}, hs::VfxPrimitive::Sector},
        {std::string_view{"chevron"}, hs::VfxPrimitive::Chevron},
        {std::string_view{"rune"}, hs::VfxPrimitive::Rune},
        {std::string_view{"cracks"}, hs::VfxPrimitive::Cracks},
        {std::string_view{"arrow"}, hs::VfxPrimitive::Arrow},
        {std::string_view{"shard"}, hs::VfxPrimitive::Shard},
        {std::string_view{"ember"}, hs::VfxPrimitive::Ember},
        {std::string_view{"spike"}, hs::VfxPrimitive::Spike},
        {std::string_view{"shock_shell"}, hs::VfxPrimitive::ShockShell},
        {std::string_view{"solid_trail"}, hs::VfxPrimitive::SolidTrail},
        {std::string_view{"dashed_ricochet"}, hs::VfxPrimitive::DashedRicochet},
        {std::string_view{"fire_transfer"}, hs::VfxPrimitive::FireTransfer},
        {std::string_view{"relic_chain"}, hs::VfxPrimitive::RelicChain},
        {std::string_view{"dash_wake"}, hs::VfxPrimitive::DashWake}}};
    const auto read_values = [](const Json &object, std::string_view key, std::size_t count,
                                std::string_view path) {
        const auto &array = RequireArray(object, "particles", path, key);
        if (array.size() != count) Fail("particles", std::string(path) + "/" + std::string(key), "wrong vector size");
        std::array<float, 4> values{};
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!array[i].is_number()) Fail("particles", path, "expected number");
            values[i] = array[i].get<float>();
            if (!std::isfinite(values[i])) Fail("particles", path, "non-finite number");
        }
        return values;
    };
    std::vector<hs::CookedVfxDefinition> definitions;
    std::vector<hs::CookedParticleEmitter> emitters;
    std::vector<hs::CookedParticleSprite> cooked_sprites;
    std::unordered_map<std::string, hs::ParticleSprite> sprite_indices;
    std::unordered_map<std::uint64_t, std::string> sprite_hashes;
    sprite_sources.clear();
    const auto &sprites = RequireArray(document, "particles", "$", "sprites");
    if (sprites.size() > std::numeric_limits<hs::ParticleSprite>::max())
        Fail("particles", "$/sprites", "too many sprites");
    for (std::size_t index = 0; index < sprites.size(); ++index)
    {
        const auto sprite_path = "$/sprites/" + std::to_string(index);
        RequireExactKeys(sprites[index], "particles", {"id", "file", "frames"});
        ParticleSpriteSource sprite;
        sprite.id = RequireString(sprites[index], "particles", sprite_path, "id");
        sprite.file = RequireString(sprites[index], "particles", sprite_path, "file");
        const auto frames = read_values(sprites[index], "frames", 2, sprite_path);
        sprite.frame_columns = static_cast<std::uint8_t>(frames[0]);
        sprite.frame_rows = static_cast<std::uint8_t>(frames[1]);
        if (frames[0] != sprite.frame_columns || frames[1] != sprite.frame_rows ||
            !sprite_indices.emplace(sprite.id, static_cast<hs::ParticleSprite>(index)).second)
            Fail("particles", sprite_path, "invalid or duplicate sprite");
        const auto sprite_hash = hs::MakeAssetId("particle_sprite." + sprite.id).value;
        if (!sprite_hashes.emplace(sprite_hash, sprite.id).second)
            Fail("particles", sprite_path, "sprite hash collision");
        cooked_sprites.push_back({sprite_hash, static_cast<hs::ParticleSprite>(index),
                                  sprite.frame_columns, sprite.frame_rows});
        sprite_sources.push_back(std::move(sprite));
    }
    std::unordered_map<std::uint64_t, std::string> hashes;
    const auto &effects = RequireArray(document, "particles", "$", "effects");
    for (std::size_t effect_index = 0; effect_index < effects.size(); ++effect_index)
    {
        const auto base = "$/effects/" + std::to_string(effect_index);
        const auto id = RequireString(effects[effect_index], "particles", base, "id");
        if (!id.starts_with("particle.")) Fail("particles", base + "/id", "effect ID must start with particle.");
        const auto hash = hs::MakeAssetId(id).value;
        if (!hashes.emplace(hash, id).second) Fail("particles", base + "/id", "duplicate ID or hash collision");
        hs::CookedVfxDefinition definition;
        definition.effect_id = hash;
        const auto kind = RequireString(effects[effect_index], "particles", base, "kind");
        if (kind == "line")
        {
            RequireExactKeys(effects[effect_index], "particles", {"id", "kind", "color", "width", "lifetime", "sprite", "uv_repeat", "scroll_speed", "primitive"});
            definition.kind = hs::VfxDefinitionKind::Line;
            const auto color = read_values(effects[effect_index], "color", 4, base);
            definition.line_color = {color[0], color[1], color[2], color[3]};
            definition.line_width = static_cast<float>(RequireNumber(effects[effect_index], "particles", base, "width"));
            definition.line_lifetime = static_cast<float>(RequireNumber(effects[effect_index], "particles", base, "lifetime"));
            const auto sprite_name = RequireString(effects[effect_index], "particles", base, "sprite");
            const auto sprite = sprite_indices.find(sprite_name);
            if (sprite == sprite_indices.end()) Fail("particles", base + "/sprite", "unknown sprite");
            definition.line_sprite = sprite->second;
            definition.line_frame_columns = sprite_sources[sprite->second].frame_columns;
            definition.line_frame_rows = sprite_sources[sprite->second].frame_rows;
            definition.line_uv_repeat = static_cast<float>(RequireNumber(effects[effect_index], "particles", base, "uv_repeat"));
            definition.line_scroll_speed = static_cast<float>(RequireNumber(effects[effect_index], "particles", base, "scroll_speed"));
            definition.line_primitive = ParseEnum(
                RequireString(effects[effect_index], "particles", base, "primitive"),
                primitives, base + "/primitive");
            if (definition.line_primitive < hs::VfxPrimitive::SolidTrail)
                Fail("particles", base + "/primitive", "line requires a segment primitive");
            if (definition.line_width <= 0 || definition.line_lifetime <= 0 || definition.line_uv_repeat <= 0) Fail("particles", base, "line range invalid");
        }
        else if (kind == "particles")
        {
            RequireExactKeys(effects[effect_index], "particles", {"id", "kind", "emitters"});
            definition.kind = hs::VfxDefinitionKind::Particles;
            definition.first_emitter = static_cast<std::uint32_t>(emitters.size());
            const auto &source_emitters = RequireArray(effects[effect_index], "particles", base, "emitters");
            if (source_emitters.empty() || source_emitters.size() > std::numeric_limits<std::uint16_t>::max())
                Fail("particles", base + "/emitters", "invalid emitter count");
            definition.emitter_count = static_cast<std::uint16_t>(source_emitters.size());
            for (std::size_t emitter_index = 0; emitter_index < source_emitters.size(); ++emitter_index)
            {
                const auto emitter_path = base + "/emitters/" + std::to_string(emitter_index);
                const auto &source = source_emitters[emitter_index];
                RequireExactKeys(source, "particles", {"sprite","renderer","primitive","shape","velocity","facing","local_offset","shape_extent","local_direction","start_color","end_color","lifetime","speed","cone_degrees","start_size","end_size","gravity","rotation","angular_velocity","stretch","count"});
                hs::CookedParticleEmitter emitter;
                const auto sprite_name = RequireString(source,"particles",emitter_path,"sprite");
                const auto sprite = sprite_indices.find(sprite_name);
                if (sprite == sprite_indices.end()) Fail("particles", emitter_path + "/sprite", "unknown sprite");
                emitter.sprite = sprite->second;
                emitter.frame_columns = sprite_sources[sprite->second].frame_columns;
                emitter.frame_rows = sprite_sources[sprite->second].frame_rows;
                emitter.shape = ParseEnum(RequireString(source,"particles",emitter_path,"shape"), shapes, emitter_path);
                emitter.velocity = ParseEnum(RequireString(source,"particles",emitter_path,"velocity"), velocities, emitter_path);
                emitter.facing = ParseEnum(RequireString(source,"particles",emitter_path,"facing"), facings, emitter_path);
                emitter.renderer = ParseEnum(RequireString(source,"particles",emitter_path,"renderer"), renderers, emitter_path + "/renderer");
                emitter.primitive = ParseEnum(RequireString(source,"particles",emitter_path,"primitive"), primitives, emitter_path + "/primitive");
                const auto offset=read_values(source,"local_offset",3,emitter_path), extent=read_values(source,"shape_extent",3,emitter_path), direction=read_values(source,"local_direction",3,emitter_path);
                const auto start_color=read_values(source,"start_color",4,emitter_path), end_color=read_values(source,"end_color",4,emitter_path);
                const auto lifetime=read_values(source,"lifetime",2,emitter_path), speed=read_values(source,"speed",2,emitter_path), start_size=read_values(source,"start_size",2,emitter_path), end_size=read_values(source,"end_size",2,emitter_path), rotation=read_values(source,"rotation",2,emitter_path), angular=read_values(source,"angular_velocity",2,emitter_path);
                emitter.local_offset={offset[0],offset[1],offset[2]}; emitter.shape_extent={extent[0],extent[1],extent[2]}; emitter.local_direction={direction[0],direction[1],direction[2]};
                emitter.start_color={start_color[0],start_color[1],start_color[2],start_color[3]}; emitter.end_color={end_color[0],end_color[1],end_color[2],end_color[3]};
                emitter.lifetime_min=lifetime[0]; emitter.lifetime_max=lifetime[1]; emitter.speed_min=speed[0]; emitter.speed_max=speed[1];
                emitter.start_size_min=start_size[0]; emitter.start_size_max=start_size[1]; emitter.end_size_min=end_size[0]; emitter.end_size_max=end_size[1];
                emitter.rotation_min=rotation[0]; emitter.rotation_max=rotation[1]; emitter.angular_velocity_min=angular[0]; emitter.angular_velocity_max=angular[1];
                emitter.cone_degrees=static_cast<float>(RequireNumber(source,"particles",emitter_path,"cone_degrees")); emitter.gravity=static_cast<float>(RequireNumber(source,"particles",emitter_path,"gravity")); emitter.stretch=static_cast<float>(RequireNumber(source,"particles",emitter_path,"stretch")); emitter.count=static_cast<std::uint32_t>(RequireInteger(source,"particles",emitter_path,"count"));
                const auto direction_length = std::sqrt(direction[0]*direction[0]+direction[1]*direction[1]+direction[2]*direction[2]);
                if (lifetime[0] <= 0 || lifetime[0] > lifetime[1] || speed[0] < 0 || speed[0] > speed[1] || start_size[0] < 0 || start_size[0] > start_size[1] || end_size[0] < 0 || end_size[0] > end_size[1] || rotation[0] > rotation[1] || angular[0] > angular[1] || emitter.count == 0 || emitter.cone_degrees < 0 || emitter.cone_degrees > 180 || emitter.stretch < 1 || ((emitter.velocity == hs::ParticleVelocity::Direction || emitter.velocity == hs::ParticleVelocity::Cone) && direction_length <= 0.0001f) || (emitter.velocity == hs::ParticleVelocity::Inward && emitter.shape == hs::ParticleShape::Point) || (emitter.renderer == hs::VfxRenderer::Sprite && emitter.primitive != hs::VfxPrimitive::Soft) || (emitter.renderer == hs::VfxRenderer::Ground && (emitter.primitive < hs::VfxPrimitive::Disc || emitter.primitive > hs::VfxPrimitive::Cracks)) || (emitter.renderer == hs::VfxRenderer::Segment && emitter.primitive < hs::VfxPrimitive::SolidTrail) || (emitter.renderer == hs::VfxRenderer::Mesh && (emitter.primitive < hs::VfxPrimitive::Arrow || emitter.primitive > hs::VfxPrimitive::ShockShell)))
                    Fail("particles", emitter_path, "emitter contract violation");
                emitters.push_back(emitter);
            }
        }
        else Fail("particles", base + "/kind", "unknown effect kind");
        definitions.push_back(definition);
    }
    std::ranges::sort(definitions, {}, &hs::CookedVfxDefinition::effect_id);
    std::ranges::sort(cooked_sprites, {}, &hs::CookedParticleSprite::sprite_id);
    std::vector<std::byte> payload(definitions.size()*sizeof(definitions.front()) +
                                   emitters.size()*sizeof(emitters.front()) +
                                   cooked_sprites.size()*sizeof(cooked_sprites.front()));
    std::memcpy(payload.data(), definitions.data(), definitions.size()*sizeof(definitions.front()));
    std::memcpy(payload.data()+definitions.size()*sizeof(definitions.front()), emitters.data(), emitters.size()*sizeof(emitters.front()));
    std::memcpy(payload.data()+definitions.size()*sizeof(definitions.front())+
                    emitters.size()*sizeof(emitters.front()),
                cooked_sprites.data(), cooked_sprites.size()*sizeof(cooked_sprites.front()));
    hs::CookedParticleEffectsHeader header;
    header.effect_count=static_cast<std::uint32_t>(definitions.size()); header.emitter_count=static_cast<std::uint32_t>(emitters.size()); header.sprite_count=static_cast<std::uint32_t>(sprite_sources.size()); header.schema_hash=hs::kParticleEffectsSchemaHash; header.payload_hash=hs::Fnv1a64(payload);
    std::vector<std::byte> file(sizeof(header)+payload.size()); std::memcpy(file.data(),&header,sizeof(header)); std::memcpy(file.data()+sizeof(header),payload.data(),payload.size());
    return AtomicWrite(path,file,error_message);
}

FbxScene *LoadFbx(FbxManager &manager, const std::filesystem::path &path,
                  std::string_view scene_name)
{
    auto *scene = FbxScene::Create(&manager, std::string(scene_name).c_str());
    auto *importer = FbxImporter::Create(&manager, "");
    const auto initialized = importer->Initialize(path.string().c_str(), -1,
                                                  manager.GetIOSettings());
    const auto imported = initialized && importer->Import(scene);
    const auto error = std::string(importer->GetStatus().GetErrorString());
    importer->Destroy();
    if (!imported)
    {
        scene->Destroy();
        throw std::runtime_error(path.string() + ": FBX import failed: " + error);
    }
    FbxAxisSystem::DirectX.ConvertScene(scene);
    FbxSystemUnit::m.ConvertScene(scene);
    return scene;
}

void VisitNodes(FbxNode *node, const auto &visitor)
{
    visitor(node);
    for (int index = 0; index < node->GetChildCount(); ++index)
    {
        VisitNodes(node->GetChild(index), visitor);
    }
}

FbxAMatrix GeometryTransform(FbxNode &node)
{
    FbxAMatrix matrix;
    matrix.SetT(node.GetGeometricTranslation(FbxNode::eSourcePivot));
    matrix.SetR(node.GetGeometricRotation(FbxNode::eSourcePivot));
    matrix.SetS(node.GetGeometricScaling(FbxNode::eSourcePivot));
    return matrix;
}

std::array<float, 16> RuntimeMatrix(const FbxAMatrix &matrix)
{
    std::array<float, 16> output{};
    for (std::size_t row = 0; row < 4; ++row)
    {
        for (std::size_t column = 0; column < 4; ++column)
        {
            output[row * 4 + column] =
                static_cast<float>(matrix.Get(static_cast<int>(column),
                                              static_cast<int>(row)));
        }
    }
    return output;
}

std::string BoneName(const FbxNode &node)
{
    return node.GetNameOnly().Buffer();
}

struct BoneSource
{
    std::string name;
    FbxNode *node{};
    FbxAMatrix global_bind;
    std::size_t parent{std::numeric_limits<std::size_t>::max()};
};

std::filesystem::path MaterialTexture(FbxSurfaceMaterial &material,
                                      const char *property_name)
{
    const auto property = material.FindProperty(property_name);
    if (!property.IsValid())
    {
        return {};
    }
    auto *texture = property.GetSrcObject<FbxFileTexture>(0);
    if (!texture)
    {
        return {};
    }
    const auto filename = std::filesystem::path(texture->GetFileName()).filename();
    const auto path = std::filesystem::path(HS_CHARACTER_MODEL).parent_path() /
                      "ErikaArcher.fbm" / filename;
    if (!std::filesystem::is_regular_file(path))
    {
        throw std::runtime_error("Archer material texture is missing: " + path.string());
    }
    return path;
}

std::uint16_t GatherMaterial(FbxSurfaceMaterial &material,
                             CharacterCookResult &output)
{
    auto diffuse = MaterialTexture(material, FbxSurfaceMaterial::sDiffuse);
    auto normal = MaterialTexture(material, FbxSurfaceMaterial::sNormalMap);
    if (normal.empty())
    {
        normal = MaterialTexture(material, FbxSurfaceMaterial::sBump);
    }
    const auto name = std::string(material.GetName());
    const auto existing = std::ranges::find_if(
        output.materials, [&](const CharacterCookResult::Material &candidate) {
            return candidate.diffuse == diffuse && candidate.normal == normal &&
                   candidate.name == name;
        });
    if (existing != output.materials.end())
    {
        return static_cast<std::uint16_t>(existing - output.materials.begin());
    }
    if (output.materials.size() == hs::kMaxCharacterMaterials)
    {
        throw std::runtime_error("Archer exceeds the 8 material limit");
    }
    output.materials.push_back({std::move(diffuse), std::move(normal), name});
    return static_cast<std::uint16_t>(output.materials.size() - 1);
}

std::uint16_t PolygonMaterial(FbxMesh &mesh, int polygon,
                              std::span<const std::uint16_t> node_materials)
{
    auto *element = mesh.GetElementMaterial();
    if (!element || node_materials.empty())
    {
        return 0;
    }
    const auto mapped = element->GetMappingMode() == FbxGeometryElement::eAllSame
                            ? 0
                            : polygon;
    const auto local = element->GetIndexArray().GetAt(mapped);
    if (local < 0 || static_cast<std::size_t>(local) >= node_materials.size())
    {
        throw std::runtime_error("Archer polygon has an invalid material index");
    }
    return node_materials[static_cast<std::size_t>(local)];
}

void GatherModel(FbxScene &scene, CharacterCookResult &output,
                 std::vector<BoneSource> &bones)
{
    FbxGeometryConverter converter(scene.GetFbxManager());
    if (!converter.Triangulate(&scene, true, false))
    {
        throw std::runtime_error("Archer FBX triangulation failed");
    }

    std::vector<FbxNode *> mesh_nodes;
    std::unordered_set<FbxNode *> weighted_bones;
    std::unordered_map<FbxNode *, FbxAMatrix> bind_by_bone;
    VisitNodes(scene.GetRootNode(), [&](FbxNode *node) {
        auto *mesh = node->GetMesh();
        if (!mesh)
        {
            return;
        }
        mesh_nodes.push_back(node);
        for (int skin_index = 0;
             skin_index < mesh->GetDeformerCount(FbxDeformer::eSkin); ++skin_index)
        {
            auto *skin = static_cast<FbxSkin *>(
                mesh->GetDeformer(skin_index, FbxDeformer::eSkin));
            for (int cluster_index = 0; cluster_index < skin->GetClusterCount();
                 ++cluster_index)
            {
                auto *cluster = skin->GetCluster(cluster_index);
                auto *link = cluster->GetLink();
                if (!link || cluster->GetLinkMode() == FbxCluster::eAdditive)
                {
                    throw std::runtime_error(
                        "Archer FBX requires non-additive linked skin clusters");
                }
                weighted_bones.insert(link);
                FbxAMatrix link_bind;
                cluster->GetTransformLinkMatrix(link_bind);
                bind_by_bone.try_emplace(link, link_bind);
            }
        }
    });
    if (mesh_nodes.empty() || weighted_bones.empty())
    {
        throw std::runtime_error("Archer FBX has no skinned mesh");
    }

    VisitNodes(scene.GetRootNode(), [&](FbxNode *node) {
        if (weighted_bones.contains(node))
        {
            bones.push_back(
                {BoneName(*node), node, bind_by_bone.at(node)});
        }
    });
    if (bones.empty() || bones.size() > hs::kMaxCharacterBones)
    {
        throw std::runtime_error("Archer skeleton bone count must be 1..128");
    }
    std::unordered_map<FbxNode *, std::uint16_t> bone_indices;
    for (std::size_t index = 0; index < bones.size(); ++index)
    {
        if (!bone_indices.emplace(bones[index].node,
                                  static_cast<std::uint16_t>(index)).second)
        {
            throw std::runtime_error("Archer skeleton contains a duplicate bone");
        }
    }
    for (auto &bone : bones)
    {
        for (auto *parent = bone.node->GetParent(); parent; parent = parent->GetParent())
        {
            if (const auto iterator = bone_indices.find(parent);
                iterator != bone_indices.end())
            {
                bone.parent = iterator->second;
                break;
            }
        }
    }
    output.parents.reserve(bones.size());
    output.inverse_bind_matrices.reserve(bones.size());
    for (const auto &bone : bones)
    {
        if (bone.parent != std::numeric_limits<std::size_t>::max() &&
            bone.parent >= output.parents.size())
        {
            throw std::runtime_error("Archer skeleton parent order is invalid");
        }
        output.parents.push_back(
            bone.parent == std::numeric_limits<std::size_t>::max()
                ? std::numeric_limits<std::uint16_t>::max()
                : static_cast<std::uint16_t>(bone.parent));
        output.inverse_bind_matrices.push_back(
            RuntimeMatrix(bone.global_bind.Inverse()));
    }
    for (auto *node : mesh_nodes)
    {
        auto *mesh = node->GetMesh();
        FbxStringList uv_sets;
        mesh->GetUVSetNames(uv_sets);
        if (uv_sets.GetCount() == 0)
        {
            throw std::runtime_error("Archer mesh has no UV set");
        }
        std::vector<std::uint16_t> node_materials;
        node_materials.reserve(static_cast<std::size_t>(node->GetMaterialCount()));
        for (int material_index = 0; material_index < node->GetMaterialCount();
             ++material_index)
        {
            auto *material = node->GetMaterial(material_index);
            if (!material)
            {
                throw std::runtime_error("Archer node has an empty material slot");
            }
            node_materials.push_back(GatherMaterial(*material, output));
        }
        if (node_materials.empty())
        {
            if (output.materials.empty())
            {
                output.materials.push_back({{}, {}, "Default"});
            }
            node_materials.push_back(0);
        }
        std::vector<std::vector<std::pair<std::uint16_t, float>>> weights(
            static_cast<std::size_t>(mesh->GetControlPointsCount()));
        FbxAMatrix mesh_bind;
        bool has_mesh_bind{};
        for (int skin_index = 0;
             skin_index < mesh->GetDeformerCount(FbxDeformer::eSkin); ++skin_index)
        {
            auto *skin = static_cast<FbxSkin *>(
                mesh->GetDeformer(skin_index, FbxDeformer::eSkin));
            for (int cluster_index = 0; cluster_index < skin->GetClusterCount();
                 ++cluster_index)
            {
                auto *cluster = skin->GetCluster(cluster_index);
                if (!has_mesh_bind)
                {
                    cluster->GetTransformMatrix(mesh_bind);
                    has_mesh_bind = true;
                }
                const auto bone = bone_indices.at(cluster->GetLink());
                const auto *control_points = cluster->GetControlPointIndices();
                const auto *control_weights = cluster->GetControlPointWeights();
                for (int weight_index = 0;
                     weight_index < cluster->GetControlPointIndicesCount(); ++weight_index)
                {
                    const auto control_point = control_points[weight_index];
                    if (control_point < 0 || control_point >= mesh->GetControlPointsCount() ||
                        control_weights[weight_index] <= 0.0)
                    {
                        continue;
                    }
                    weights[static_cast<std::size_t>(control_point)].push_back(
                        {bone, static_cast<float>(control_weights[weight_index])});
                }
            }
        }
        if (!has_mesh_bind)
        {
            throw std::runtime_error("Archer mesh has no skin bind transform");
        }
        mesh_bind *= GeometryTransform(*node);
        const auto normal_matrix = mesh_bind.Inverse().Transpose();
        for (int polygon = 0; polygon < mesh->GetPolygonCount(); ++polygon)
        {
            if (mesh->GetPolygonSize(polygon) != 3)
            {
                throw std::runtime_error("Archer FBX contains a non-triangle polygon");
            }
            std::array<hs::SkinnedVertex, 3> triangle{};
            for (int corner = 0; corner < 3; ++corner)
            {
                const auto control_point = mesh->GetPolygonVertex(polygon, corner);
                const auto position = mesh_bind.MultT(mesh->GetControlPointAt(control_point));
                FbxVector4 normal;
                if (!mesh->GetPolygonVertexNormal(polygon, corner, normal))
                {
                    throw std::runtime_error("Archer FBX contains a vertex without a normal");
                }
                normal[3] = 0.0;
                normal = normal_matrix.MultT(normal);
                normal.Normalize();

                auto &vertex = triangle[static_cast<std::size_t>(corner)];
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    vertex.position[axis] =
                        static_cast<float>(position[static_cast<int>(axis)]);
                    vertex.normal[axis] =
                        static_cast<float>(normal[static_cast<int>(axis)]);
                    output.bounds_min[axis] =
                        std::min(output.bounds_min[axis], vertex.position[axis]);
                    output.bounds_max[axis] =
                        std::max(output.bounds_max[axis], vertex.position[axis]);
                }
                auto influences = weights[static_cast<std::size_t>(control_point)];
                std::ranges::sort(influences, std::greater{}, &decltype(influences)::value_type::second);
                if (influences.size() > vertex.bone_indices.size())
                {
                    influences.resize(vertex.bone_indices.size());
                }
                if (influences.empty())
                {
                    influences.push_back({0, 1.0f});
                }
                float total{};
                for (const auto &influence : influences)
                {
                    total += influence.second;
                }
                for (std::size_t index = 0; index < influences.size(); ++index)
                {
                    vertex.bone_indices[index] = influences[index].first;
                    vertex.bone_weights[index] = influences[index].second / total;
                }
                FbxVector2 uv;
                bool unmapped{};
                if (!mesh->GetPolygonVertexUV(polygon, corner, uv_sets[0], uv,
                                              unmapped) || unmapped)
                {
                    throw std::runtime_error("Archer polygon has an unmapped UV");
                }
                vertex.uv = {static_cast<float>(uv[0]),
                             1.0f - static_cast<float>(uv[1])};
                vertex.material_index =
                    PolygonMaterial(*mesh, polygon, node_materials);
            }
            const auto edge1 = FbxVector4(
                triangle[1].position[0] - triangle[0].position[0],
                triangle[1].position[1] - triangle[0].position[1],
                triangle[1].position[2] - triangle[0].position[2]);
            const auto edge2 = FbxVector4(
                triangle[2].position[0] - triangle[0].position[0],
                triangle[2].position[1] - triangle[0].position[1],
                triangle[2].position[2] - triangle[0].position[2]);
            const auto duv1 = FbxVector2(triangle[1].uv[0] - triangle[0].uv[0],
                                         triangle[1].uv[1] - triangle[0].uv[1]);
            const auto duv2 = FbxVector2(triangle[2].uv[0] - triangle[0].uv[0],
                                         triangle[2].uv[1] - triangle[0].uv[1]);
            const auto denominator = duv1[0] * duv2[1] - duv1[1] * duv2[0];
            FbxVector4 tangent{1.0, 0.0, 0.0, 0.0};
            FbxVector4 bitangent{0.0, 0.0, 1.0, 0.0};
            if (std::abs(denominator) > 1e-10)
            {
                const auto inverse = 1.0 / denominator;
                tangent = (edge1 * duv2[1] - edge2 * duv1[1]) * inverse;
                bitangent = (edge2 * duv1[0] - edge1 * duv2[0]) * inverse;
                tangent.Normalize();
                bitangent.Normalize();
            }
            for (auto &vertex : triangle)
            {
                FbxVector4 vertex_normal(vertex.normal[0], vertex.normal[1],
                                         vertex.normal[2], 0.0);
                auto orthogonal_tangent =
                    tangent - vertex_normal * vertex_normal.DotProduct(tangent);
                orthogonal_tangent.Normalize();
                const auto handedness =
                    vertex_normal.CrossProduct(orthogonal_tangent).DotProduct(bitangent) < 0.0
                        ? -1.0f
                        : 1.0f;
                vertex.tangent = {static_cast<float>(orthogonal_tangent[0]),
                                  static_cast<float>(orthogonal_tangent[1]),
                                  static_cast<float>(orthogonal_tangent[2]), handedness};
                output.vertices.push_back(vertex);
            }
        }
    }
    output.mesh_count = static_cast<std::uint32_t>(mesh_nodes.size());
    output.bone_count = static_cast<std::uint32_t>(bones.size());
}

FbxNode *FindBone(FbxScene &scene, std::string_view name)
{
    FbxNode *result{};
    VisitNodes(scene.GetRootNode(), [&](FbxNode *node) {
        if (!result && BoneName(*node) == name)
        {
            result = node;
        }
    });
    return result;
}

void GatherAnimation(FbxScene &scene, const std::vector<BoneSource> &model_bones,
                     hs::CharacterAnimationClip clip, bool looping,
                     CharacterCookResult &output)
{
    auto *stack = scene.GetSrcObjectCount<FbxAnimStack>() > 0
                      ? scene.GetSrcObject<FbxAnimStack>(0)
                      : nullptr;
    if (!stack)
    {
        throw std::runtime_error("Animation FBX has no animation stack");
    }
    scene.SetCurrentAnimationStack(stack);
    const auto *take = scene.GetTakeInfo(stack->GetName());
    FbxTimeSpan span;
    if (take)
    {
        span = take->mLocalTimeSpan;
    }
    else
    {
        scene.GetGlobalSettings().GetTimelineDefaultTimeSpan(span);
    }
    const auto duration = span.GetDuration().GetSecondDouble();
    if (!(duration > 0.0) || duration > 30.0)
    {
        throw std::runtime_error("Animation duration must be within 0..30 seconds");
    }
    std::vector<FbxNode *> animation_bones;
    std::vector<FbxAMatrix> animation_global_bind;
    animation_bones.reserve(model_bones.size());
    animation_global_bind.reserve(model_bones.size());
    for (const auto &bone : model_bones)
    {
        auto *animation_bone = FindBone(scene, bone.name);
        if (!animation_bone)
        {
            throw std::runtime_error("Animation skeleton is missing bone: " + bone.name);
        }
        animation_bones.push_back(animation_bone);
        animation_global_bind.push_back(
            animation_bone->EvaluateGlobalTransform(
                FBXSDK_TIME_INFINITE, FbxNode::eSourcePivot, false, true));
    }

    const auto frame_count =
        std::max<std::uint32_t>(2, static_cast<std::uint32_t>(std::ceil(duration * 60.0)) + 1);
    hs::CharacterClipHeader header;
    header.clip = clip;
    header.looping = looping;
    header.first_transform = static_cast<std::uint32_t>(output.transforms.size());
    header.frame_count = frame_count;
    header.duration_seconds = static_cast<float>(duration);
    output.clips.push_back(header);

    auto *evaluator = scene.GetAnimationEvaluator();
    std::vector<FbxAMatrix> model_local_bind(model_bones.size());
    std::vector<FbxAMatrix> animation_local_bind(model_bones.size());
    for (std::size_t bone_index = 0; bone_index < model_bones.size(); ++bone_index)
    {
        const auto parent = model_bones[bone_index].parent;
        if (parent == std::numeric_limits<std::size_t>::max())
        {
            model_local_bind[bone_index] = model_bones[bone_index].global_bind;
            animation_local_bind[bone_index] = animation_global_bind[bone_index];
        }
        else
        {
            model_local_bind[bone_index] =
                model_bones[parent].global_bind.Inverse() *
                model_bones[bone_index].global_bind;
            animation_local_bind[bone_index] =
                animation_global_bind[parent].Inverse() *
                animation_global_bind[bone_index];
        }
    }

    std::vector<FbxAMatrix> current_globals(model_bones.size());
    std::vector<FbxAMatrix> target_globals(model_bones.size());
    std::vector<FbxAMatrix> corrected_globals(model_bones.size());
    for (std::uint32_t frame = 0; frame < frame_count; ++frame)
    {
        FbxTime sample_time;
        sample_time.SetSecondDouble(span.GetStart().GetSecondDouble() +
                                    duration * static_cast<double>(frame) /
                                        static_cast<double>(frame_count - 1));
        for (std::size_t bone_index = 0; bone_index < model_bones.size(); ++bone_index)
        {
            current_globals[bone_index] = evaluator->GetNodeGlobalTransform(
                animation_bones[bone_index], sample_time);
            const auto parent = model_bones[bone_index].parent;
            const auto current_local =
                parent == std::numeric_limits<std::size_t>::max()
                    ? current_globals[bone_index]
                    : current_globals[parent].Inverse() * current_globals[bone_index];
            const auto target_local = model_local_bind[bone_index] *
                                      animation_local_bind[bone_index].Inverse() *
                                      current_local;
            target_globals[bone_index] =
                parent == std::numeric_limits<std::size_t>::max()
                    ? target_local
                    : target_globals[parent] * target_local;
        }
        const auto corrected_root = target_globals.front();
        const auto root_delta =
            corrected_root.GetT() - model_bones.front().global_bind.GetT();
        for (std::size_t bone_index = 0; bone_index < model_bones.size(); ++bone_index)
        {
            corrected_globals[bone_index] = target_globals[bone_index];
            auto translation = corrected_globals[bone_index].GetT();
            translation[0] -= root_delta[0];
            translation[2] -= root_delta[2];
            corrected_globals[bone_index].SetT(translation);
        }
        for (std::size_t bone_index = 0; bone_index < model_bones.size(); ++bone_index)
        {
            const auto parent = model_bones[bone_index].parent;
            const auto local = parent == std::numeric_limits<std::size_t>::max()
                                   ? corrected_globals[bone_index]
                                   : corrected_globals[parent].Inverse() *
                                         corrected_globals[bone_index];
            const auto translation = local.GetT();
            const auto rotation = local.GetQ();
            const auto scale = local.GetS();
            hs::CharacterLocalTransform transform;
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                transform.translation[axis] = static_cast<float>(translation[axis]);
                transform.rotation[axis] = static_cast<float>(rotation[axis]);
                transform.scale[axis] = static_cast<float>(scale[axis]);
            }
            transform.rotation[3] = static_cast<float>(rotation[3]);
            const auto rotation_length = std::sqrt(
                std::inner_product(transform.rotation.begin(), transform.rotation.end(),
                                   transform.rotation.begin(), 0.0f));
            if (!std::isfinite(rotation_length) || rotation_length < 0.999f ||
                rotation_length > 1.001f ||
                !std::ranges::all_of(transform.translation, [](float value) {
                    return std::isfinite(value);
                }) ||
                !std::ranges::all_of(transform.scale, [](float value) {
                    return std::isfinite(value) && std::abs(value) > 0.0001f;
                }))
            {
                throw std::runtime_error(
                    "Animation local transform is invalid");
            }
            output.transforms.push_back(transform);
        }
    }
}

bool WriteCharacterAsset(const std::filesystem::path &path,
                         const CharacterCookResult &asset,
                         std::string &error_message)
{
    hs::CharacterAssetHeader header;
    header.vertex_count = static_cast<std::uint32_t>(asset.vertices.size());
    header.bone_count = asset.bone_count;
    header.clip_count = static_cast<std::uint32_t>(asset.clips.size());
    header.material_count = static_cast<std::uint32_t>(asset.materials.size());
    header.clips_offset = header.vertices_offset +
                          header.vertex_count * sizeof(hs::SkinnedVertex);
    header.parents_offset = header.clips_offset +
                            header.clip_count * sizeof(hs::CharacterClipHeader);
    header.inverse_bind_matrices_offset =
        header.parents_offset + header.bone_count * sizeof(std::uint16_t);
    header.upper_body_weights_offset =
        header.inverse_bind_matrices_offset +
        header.bone_count * sizeof(asset.inverse_bind_matrices.front());
    header.transforms_offset =
        header.upper_body_weights_offset + header.bone_count * sizeof(float);
    header.payload_size = header.transforms_offset - sizeof(header) +
                          static_cast<std::uint32_t>(
                              asset.transforms.size() * sizeof(asset.transforms.front()));
    header.bounds_min = asset.bounds_min;
    header.bounds_max = asset.bounds_max;

    std::vector<std::byte> file(sizeof(header) + header.payload_size);
    auto *cursor = file.data() + sizeof(header);
    const auto vertices_size = asset.vertices.size() * sizeof(asset.vertices.front());
    std::memcpy(cursor, asset.vertices.data(), vertices_size);
    cursor += vertices_size;
    const auto clips_size = asset.clips.size() * sizeof(asset.clips.front());
    std::memcpy(cursor, asset.clips.data(), clips_size);
    cursor += clips_size;
    const auto parents_size = asset.parents.size() * sizeof(asset.parents.front());
    std::memcpy(cursor, asset.parents.data(), parents_size);
    cursor += parents_size;
    const auto inverse_bind_size =
        asset.inverse_bind_matrices.size() * sizeof(asset.inverse_bind_matrices.front());
    std::memcpy(cursor, asset.inverse_bind_matrices.data(), inverse_bind_size);
    cursor += inverse_bind_size;
    const auto weights_size = asset.upper_body_weights.size() * sizeof(float);
    std::memcpy(cursor, asset.upper_body_weights.data(), weights_size);
    cursor += weights_size;
    const auto transforms_size = asset.transforms.size() * sizeof(asset.transforms.front());
    std::memcpy(cursor, asset.transforms.data(), transforms_size);
    header.payload_crc32 = hs::Crc32(
        std::span(file.data() + sizeof(header), header.payload_size));
    std::memcpy(file.data(), &header, sizeof(header));
    return AtomicWrite(path, file, error_message);
}

bool WriteDds(IWICImagingFactory &factory, const std::filesystem::path &path,
              const std::filesystem::path &source, std::array<std::uint8_t, 4> fallback,
              std::string &error_message)
{
    constexpr std::uint32_t magic = 0x20534444;
    DdsHeader header;
    std::vector<std::byte> pixels;
    if (source.empty())
    {
        pixels.resize(fallback.size());
        std::memcpy(pixels.data(), fallback.data(), fallback.size());
    }
    else
    {
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        auto result = factory.CreateDecoderFromFilename(
            source.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
            &decoder);
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        if (SUCCEEDED(result))
        {
            result = decoder->GetFrame(0, &frame);
        }
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(result))
        {
            result = factory.CreateFormatConverter(&converter);
        }
        if (SUCCEEDED(result))
        {
            result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                           WICBitmapDitherTypeNone, nullptr, 0,
                                           WICBitmapPaletteTypeCustom);
        }
        UINT width{};
        UINT height{};
        if (SUCCEEDED(result))
        {
            result = converter->GetSize(&width, &height);
        }
        const auto byte_count = static_cast<std::uint64_t>(width) * height * 4;
        if (FAILED(result) || width == 0 || height == 0 ||
            byte_count > std::numeric_limits<UINT>::max())
        {
            error_message = "cannot decode texture: " + source.string();
            return false;
        }
        pixels.resize(static_cast<std::size_t>(byte_count));
        result = converter->CopyPixels(nullptr, width * 4,
                                       static_cast<UINT>(pixels.size()),
                                       reinterpret_cast<BYTE *>(pixels.data()));
        if (FAILED(result))
        {
            error_message = "cannot copy texture pixels: " + source.string();
            return false;
        }
        header.width = width;
        header.height = height;
        header.pitch = width * 4;
    }
    std::vector<std::byte> file(sizeof(magic) + sizeof(header) + pixels.size());
    auto *cursor = file.data();
    std::memcpy(cursor, &magic, sizeof(magic));
    cursor += sizeof(magic);
    std::memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);
    std::memcpy(cursor, pixels.data(), pixels.size());
    return AtomicWrite(path, file, error_message);
}

bool WriteVfxMaskArray(const std::filesystem::path &path,
                       std::span<const ParticleSpriteSource> sprites,
                       std::string &error_message)
{
    DirectX::ScratchImage masks;
    auto result = masks.Initialize2D(DXGI_FORMAT_R8_UNORM, 512, 512,
                                     sprites.size(), 1);
    if (FAILED(result))
    {
        error_message = "cannot allocate VFX mask array";
        return false;
    }
    for (std::size_t slice = 0; slice < sprites.size(); ++slice)
    {
        DirectX::TexMetadata metadata;
        DirectX::ScratchImage source;
        const auto source_path = std::filesystem::path(HS_VFX_TEXTURE_DIRECTORY) /
                                 sprites[slice].file;
        result = DirectX::LoadFromWICFile(source_path.c_str(),
                                         DirectX::WIC_FLAGS_NONE,
                                         &metadata, source);
        DirectX::ScratchImage rgba;
        if (SUCCEEDED(result))
            result = DirectX::Convert(
                source.GetImages(), source.GetImageCount(), source.GetMetadata(),
                DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT, 0.0f,
                rgba);
        const auto *input = rgba.GetImage(0, 0, 0);
        auto *output = masks.GetImage(0, slice, 0);
        if (FAILED(result) || !input || !output || metadata.width != 512 ||
            metadata.height != 512)
        {
            error_message = "invalid VFX mask: " + source_path.string();
            return false;
        }
        for (std::size_t row = 0; row < 512; ++row)
            for (std::size_t column = 0; column < 512; ++column)
                output->pixels[row * output->rowPitch + column] =
                    input->pixels[row * input->rowPitch + column * 4 + 3];
    }
    DirectX::ScratchImage mipmaps;
    result = DirectX::GenerateMipMaps(
        masks.GetImages(), masks.GetImageCount(), masks.GetMetadata(),
        DirectX::TEX_FILTER_DEFAULT, 0, mipmaps);
    DirectX::ScratchImage compressed;
    if (SUCCEEDED(result))
        result = DirectX::Compress(
            mipmaps.GetImages(), mipmaps.GetImageCount(), mipmaps.GetMetadata(),
            DXGI_FORMAT_BC4_UNORM, DirectX::TEX_COMPRESS_DEFAULT, 0.5f,
            compressed);
    if (SUCCEEDED(result))
        result = DirectX::SaveToDDSFile(
            compressed.GetImages(), compressed.GetImageCount(),
            compressed.GetMetadata(), DirectX::DDS_FLAGS_NONE, path.c_str());
    if (FAILED(result))
    {
        error_message = "cannot cook BC4 VFX mask array";
        return false;
    }
    return true;
}

bool CookCharacterAsset(const std::filesystem::path &output,
                        CharacterCookResult &character,
                        std::string &error_message)
{
    auto *manager = FbxManager::Create();
    if (!manager)
    {
        error_message = "FBX manager creation failed";
        return false;
    }
    manager->SetIOSettings(FbxIOSettings::Create(manager, IOSROOT));
    try
    {
        auto *model = LoadFbx(*manager, HS_CHARACTER_MODEL, "ArcherModel");
        std::vector<BoneSource> bones;
        GatherModel(*model, character, bones);
        character.upper_body_weights.resize(bones.size());
        for (std::size_t bone_index = 0; bone_index < bones.size(); ++bone_index)
        {
            auto ancestor = bone_index;
            while (ancestor != std::numeric_limits<std::size_t>::max())
            {
                auto name = bones[ancestor].name;
                std::ranges::transform(name, name.begin(), [](unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                });
                if (name.contains("spine") || name.contains("chest") ||
                    name.contains("neck") || name.contains("head") ||
                    name.contains("clavicle") || name.contains("shoulder") ||
                    name.contains("arm") || name.contains("hand"))
                {
                    character.upper_body_weights[bone_index] = 1.0f;
                    break;
                }
                ancestor = bones[ancestor].parent;
            }
        }
        model->Destroy();

        const auto animation_root = std::filesystem::path(HS_CHARACTER_ANIMATION_DIRECTORY);
        for (const auto &[filename, clip, looping] :
             std::array{
                 std::tuple{"Idle.fbx", hs::CharacterAnimationClip::Idle, true},
                 std::tuple{"RunForward.fbx", hs::CharacterAnimationClip::Run, true},
                 std::tuple{"DrawArrow.fbx", hs::CharacterAnimationClip::Draw, false},
                 std::tuple{"AimRecoil.fbx", hs::CharacterAnimationClip::Recoil, false},
                 std::tuple{"DeathBackward.fbx", hs::CharacterAnimationClip::Death, false},
             })
        {
            auto *animation = LoadFbx(*manager, animation_root / filename, filename);
            GatherAnimation(*animation, bones, clip, looping, character);
            animation->Destroy();
        }
        if (!WriteCharacterAsset(output / "archer.meshbin", character,
                                 error_message))
        {
            manager->Destroy();
            return false;
        }
    }
    catch (const std::exception &exception)
    {
        error_message = exception.what();
        manager->Destroy();
        return false;
    }
    manager->Destroy();

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
    {
        error_message = "WIC factory creation failed";
        return false;
    }
    for (std::size_t index = 0; index < character.materials.size(); ++index)
    {
        const auto suffix = std::to_string(index) + ".dds";
        const auto &material = character.materials[index];
        if (!WriteDds(*factory.Get(), output / ("archer_diffuse_" + suffix),
                      material.diffuse, {255, 255, 255, 255}, error_message) ||
            !WriteDds(*factory.Get(), output / ("archer_normal_" + suffix),
                      material.normal, {128, 128, 255, 255}, error_message))
        {
            return false;
        }
        std::cout << "content.material index=" << index << " name=" << material.name
                  << " diffuse="
                  << (material.diffuse.empty() ? "fallback" : material.diffuse.filename().string())
                  << " normal="
                  << (material.normal.empty() ? "fallback" : material.normal.filename().string())
                  << '\n';
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    const ComScope com;
    if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE)
    {
        std::cerr << "content.error COM initialization failed\n";
        return 1;
    }
    const auto mode = argc == 2 ? std::string_view(argv[1]) : std::string_view{};
    if (mode != "--validate-only" && mode != "--cook")
    {
        std::cerr << "usage: hs_content --validate-only | --cook\n";
        return 2;
    }

    try
    {
        const auto sources = LoadAndValidateSources();
        const auto data = BuildGameData(sources);
        const auto gameplay_hash = hs::Fnv1a64(sources.source_bytes);
        const auto source_hash = hs::Fnv1a64(sources.all_source_bytes);
        if (mode == "--validate-only")
        {
            std::cout << "content.validated documents=" << kDocumentNames.size()
                      << " source_hash=" << source_hash << '\n';
            return 0;
        }

        const std::filesystem::path output = HS_COOKED_DIRECTORY;
        std::error_code filesystem_error;
        std::filesystem::create_directories(output, filesystem_error);
        if (filesystem_error)
        {
            throw std::runtime_error(output.string() + ": " + filesystem_error.message());
        }

        std::string error_message;
        if (!WriteCookedTable(output / "simulation_rules.hsbin", data.simulation_rules,
                              hs::SimulationRulesSchemaHash(), gameplay_hash,
                              error_message))
        {
            throw std::runtime_error("simulation_rules.hsbin: " + error_message);
        }
        if (!WriteCookedTable(output / "presentation_catalog.hsbin",
                              data.presentation, hs::PresentationCatalogSchemaHash(),
                              source_hash, error_message))
        {
            throw std::runtime_error("presentation_catalog.hsbin: " + error_message);
        }
        std::vector<ParticleSpriteSource> particle_sprites;
        if (!WriteCookedParticleEffects(output / "particle_effects.hsbin",
                                        sources.documents.at("particles"),
                                        particle_sprites, error_message))
        {
            throw std::runtime_error("particle_effects.hsbin: " + error_message);
        }
        if (!WriteVfxMaskArray(output / "vfx_masks.dds", particle_sprites,
                               error_message))
            throw std::runtime_error("vfx_masks.dds: " + error_message);
        hs::SimulationRules loaded_rules{};
        hs::PresentationCatalog loaded_presentation{};
        std::uint64_t loaded_hash{};
        if (const auto result = hs::LoadSimulationRules(
                output / "simulation_rules.hsbin", loaded_rules, &loaded_hash);
            !result)
        {
            throw std::runtime_error("simulation_rules.hsbin self-check failed: " +
                                     std::string(result.Message()));
        }
        const auto &rules = data.simulation_rules;
        if (loaded_hash != gameplay_hash ||
            loaded_rules.player_health != rules.player_health ||
            loaded_rules.skills[1].damage_coefficient !=
                rules.skills[1].damage_coefficient ||
            loaded_rules.relics.bleed_burn_explosion.radius !=
                rules.relics.bleed_burn_explosion.radius ||
            loaded_rules.relics.damage_knockback.cooldown_ticks !=
                rules.relics.damage_knockback.cooldown_ticks)
        {
            throw std::runtime_error("simulation_rules.hsbin self-check mismatch");
        }
        if (const auto result = hs::LoadPresentationCatalog(
                output / "presentation_catalog.hsbin", loaded_presentation);
            !result || loaded_presentation.relic_rules[3] !=
                           data.presentation.relic_rules[3])
        {
            throw std::runtime_error("presentation_catalog.hsbin self-check mismatch");
        }

        CharacterCookResult character;
        if (!CookCharacterAsset(output, character, error_message))
        {
            throw std::runtime_error(error_message);
        }

        constexpr std::string_view shaders[] = {
            "scene_vs.dxil",      "scene_ps.dxil",    "shadow_vs.dxil",
            "particle_vs.dxil",   "particle_ps.dxil", "particle_cs.dxil",
            "fullscreen_vs.dxil", "deferred_ps.dxil", "composite_ps.dxil",
            "bloom_ps.dxil",      "tonemap_ps.dxil",  "outline_ps.dxil",
            "fxaa_ps.dxil",       "ui_ps.dxil",
        };
        for (const auto shader : shaders)
        {
            const auto source = std::filesystem::path(HS_SHADER_DIRECTORY) / shader;
            const auto destination = output / shader;
            std::filesystem::copy_file(source, destination,
                                       std::filesystem::copy_options::overwrite_existing,
                                       filesystem_error);
            if (filesystem_error)
            {
                throw std::runtime_error("Shader Cook failed: " + std::string(shader) +
                                         ": " + filesystem_error.message());
            }
        }

        std::ofstream manifest(output / "manifest.txt", std::ios::trunc);
        manifest << "fbx_sdk=2020.3.7-vs2022\n"
                 << "simulation_rules=simulation_rules.hsbin\n"
                 << "presentation_catalog=presentation_catalog.hsbin\n"
                 << "particle_effects=particle_effects.hsbin\n"
                 << "vfx_masks=vfx_masks.dds\n"
                 << "source_hash=" << source_hash << '\n'
                 << "mesh=archer.meshbin\n";
        for (std::size_t index = 0; index < character.materials.size(); ++index)
        {
            manifest << "texture=archer_diffuse_" << index << ".dds\n"
                     << "texture=archer_normal_" << index << ".dds\n";
        }
        for (const auto shader : shaders)
        {
            manifest << "shader=" << shader << '\n';
        }
        if (!manifest.good())
        {
            throw std::runtime_error("manifest.txt: write failed");
        }
        std::cout << "content.cooked documents=" << kDocumentNames.size()
                  << " meshes=" << character.mesh_count
                  << " vertices=" << character.vertices.size()
                  << " bones=" << character.bone_count
                  << " clips=" << character.clips.size()
                  << " source_hash=" << source_hash
                  << '\n';
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << "content.error " << exception.what() << '\n';
        return 1;
    }
}
