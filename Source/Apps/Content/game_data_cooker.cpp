#include "content_cooker.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>

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
        ThrowValidationError(file, path, "integer value is outside cooked range");
    return static_cast<Type>(converted);
}

hs::Tick ToTicks(double seconds, std::string_view file, std::string_view path)
{
    if (!std::isfinite(seconds) || seconds < 0.0)
        ThrowValidationError(file, path, "duration must be finite and non-negative");
    return CheckedInteger<hs::Tick>(std::llround(seconds * 60.0), file, path);
}

namespace
{
using Json = nlohmann::json;

struct SpawnViewBounds
{
    float min_forward{};
    float max_forward{};
    float half_right{};
    float forward_x{};
    float forward_z{};
};

SpawnViewBounds DeriveMaxZoomView(const PresentationCamera &camera)
{
    constexpr float reference_aspect = 1920.0f / 1080.0f;
    constexpr float radians = 3.14159265358979323846f / 180.0f;
    const auto yaw = camera.yaw_degrees * radians;
    const auto pitch = camera.pitch_degrees * radians;
    const auto fov = camera.vertical_fov_degrees * radians;
    const auto distance = camera.distance_m * static_cast<float>(camera.maximum_distance_percent) / 100.0f;
    const auto forward_x = std::cos(pitch) * std::sin(yaw);
    const auto forward_y = -std::sin(pitch);
    const auto forward_z = std::cos(pitch) * std::cos(yaw);
    const auto horizontal_forward_x = std::sin(yaw);
    const auto horizontal_forward_z = std::cos(yaw);
    const auto right_x = horizontal_forward_z;
    const auto right_z = -horizontal_forward_x;
    const auto up_x = std::sin(pitch) * horizontal_forward_x;
    const auto up_y = std::cos(pitch);
    const auto up_z = std::sin(pitch) * horizontal_forward_z;
    const auto tangent = std::tan(fov * 0.5f);
    const auto eye_y = -forward_y * distance;
    SpawnViewBounds bounds{std::numeric_limits<float>::max(),
                           std::numeric_limits<float>::lowest(), 0.0f,
                           horizontal_forward_x, horizontal_forward_z};
    for (const auto x : {-1.0f, 1.0f})
    {
        for (const auto y : {-1.0f, 1.0f})
        {
            const auto ray_y = forward_y + up_y * y * tangent;
            const auto time = -eye_y / ray_y;
            const auto ray_x = forward_x + right_x * x * reference_aspect * tangent +
                               up_x * y * tangent;
            const auto ray_z = forward_z + right_z * x * reference_aspect * tangent +
                               up_z * y * tangent;
            const auto point_x = -forward_x * distance + ray_x * time;
            const auto point_z = -forward_z * distance + ray_z * time;
            const auto horizontal_forward = point_x * horizontal_forward_x +
                                            point_z * horizontal_forward_z;
            const auto horizontal_right = point_x * right_x + point_z * right_z;
            bounds.min_forward = std::min(bounds.min_forward, horizontal_forward);
            bounds.max_forward = std::max(bounds.max_forward, horizontal_forward);
            bounds.half_right = std::max(bounds.half_right, std::abs(horizontal_right));
        }
    }
    return bounds;
}

template <typename Expected>
void RequireParameterKeys(const Json &entry, std::string_view file, std::string_view path,
                          const Expected &expected)
{
    const auto &parameters = RequireArray(entry, file, path, "parameters");
    std::set<std::string, std::less<>> keys;
    for (const auto key : expected) keys.emplace(key);
    for (std::size_t index = 0; index < parameters.size(); ++index)
    {
        const auto parameter_path = std::string(path) + "/parameters/" + std::to_string(index);
        const auto key = RequireString(parameters[index], file, parameter_path, "key");
        RequireString(parameters[index], file, parameter_path, "unit");
        RequireMember(parameters[index], file, parameter_path, "value");
        if (!keys.erase(key))
            ThrowValidationError(file, parameter_path + "/key", "unexpected or duplicate parameter key");
    }
    if (!keys.empty())
        ThrowValidationError(file, std::string(path) + "/parameters", "missing parameter '" + *keys.begin() + "'");
}

void RequireParameterKeys(const Json &entry, std::string_view file, std::string_view path,
                          std::initializer_list<std::string_view> expected)
{
    RequireParameterKeys<std::initializer_list<std::string_view>>(entry, file, path,
                                                                  expected);
}

float Number(const Json &entry, std::string_view file, const std::string &path,
             std::string_view key)
{
    return static_cast<float>(ParameterNumber(entry, file, path, key));
}

template <typename Type>
Type Integer(const Json &entry, std::string_view file, const std::string &path,
             std::string_view key)
{
    return CheckedInteger<Type>(ParameterNumber(entry, file, path, key), file,
                                path + "/parameters/" + std::string(key));
}

template <std::size_t Size>
std::array<float, Size> Angles(const Json &entry, std::string_view file,
                               const std::string &path, std::string_view key)
{
    const auto &value = FindParameterValue(entry, file, path, key);
    if (!value.is_array() || value.size() != Size)
        ThrowValidationError(file, path + "/parameters/" + std::string(key), "angle array has an unexpected length");
    std::array<float, Size> result{};
    for (std::size_t index = 0; index < Size; ++index)
    {
        if (!value[index].is_number() || !std::isfinite(value[index].get<double>()))
            ThrowValidationError(file, path + "/parameters/" + std::string(key), "angle must be finite");
        result[index] = static_cast<float>(value[index].get<double>());
    }
    return result;
}

void CopyText(auto &destination, const std::string &source, std::string_view file,
              std::string_view path)
{
    if (source.size() >= destination.size())
        ThrowValidationError(file, path, "UTF-8 text exceeds cooked capacity");
    std::ranges::copy(source, destination.begin());
}

void RequireIdAndLogic(const Json &entry, std::string_view file, const std::string &path,
                       std::string_view expected_id, std::string_view expected_logic)
{
    if (RequireString(entry, file, path, "id") != expected_id ||
        RequireString(entry, file, path, "logic_id") != expected_logic)
        ThrowValidationError(file, path, "authored ID or logic order does not match the cooked ABI");
}

void RequireUpgrade(const Json &entry, std::string_view path, std::string_view logic,
                    std::initializer_list<std::string_view> keys)
{
    if (RequireInteger(entry, "upgrades", path, "ordinal") == 0 ||
        RequireString(entry, "upgrades", path, "logic_id") != logic)
        ThrowValidationError("upgrades", path, "upgrade ordinal or logic_id does not match the typed ABI");
    RequireParameterKeys(entry, "upgrades", path, keys);
}

} // namespace

BuiltGameData BuildGameData(const ContentSources &sources)
{
    BuiltGameData content{};
    auto &data = content.simulation_rules;
    auto &presentation = content.presentation;
    data.version = 6;

    const auto &stats_document = sources.documents.at("stats");
    const auto &stat = stats_document["entries"][0];
    const auto &base = stat["base_values"];
    data.stats.maximum_points_per_stat = CheckedInteger<std::uint8_t>(
        RequireInteger(stat, "stats", "$/entries/0", "maximum_points_per_stat"), "stats", "$/entries/0/maximum_points_per_stat");
    data.stats.base_maximum_hp = static_cast<float>(RequireNumber(base, "stats", "$/entries/0/base_values", "maximum_hp"));
    data.stats.base_current_hp = static_cast<float>(RequireNumber(base, "stats", "$/entries/0/base_values", "current_hp"));
    data.stats.base_attack_power = static_cast<float>(RequireNumber(base, "stats", "$/entries/0/base_values", "attack_power"));
    data.stats.base_basic_attack_rate_per_second = static_cast<float>(RequireNumber(base, "stats", "$/entries/0/base_values", "basic_attack_rate_per_second"));
    data.stats.base_movement_speed_mps = static_cast<float>(RequireNumber(base, "stats", "$/entries/0/base_values", "movement_speed_mps"));
    data.stats.base_magnet_radius_m = static_cast<float>(RequireNumber(base, "stats", "$/entries/0/base_values", "magnet_radius_m"));
    data.stats.base_global_cooldown_reduction = static_cast<float>(RequireNumber(base, "stats", "$/entries/0/base_values", "global_cooldown_reduction"));
    data.player_health = CheckedInteger<std::int32_t>(data.stats.base_maximum_hp, "stats", "$/entries/0/base_values/maximum_hp");
    data.player_attack = data.stats.base_attack_power;
    data.player_attack_speed = data.stats.base_basic_attack_rate_per_second;
    data.player_move_speed = data.stats.base_movement_speed_mps;
    data.player_magnet_radius = data.stats.base_magnet_radius_m;
    const auto &allocations = stat["allocations"];
    constexpr std::array allocation_ids{
        "stat.maximum_hp", "stat.movement_speed", "stat.attack_power",
        "stat.basic_attack_speed", "stat.cooldown_reduction",
        "stat.magnet_radius"};
    constexpr std::array allocation_targets{
        "maximum_hp", "movement_speed", "attack_power", "basic_attack_only",
        "global_cooldown_reduction", "magnet_radius"};
    for (std::size_t index = 0; index < allocations.size(); ++index)
    {
        const auto path = "$/entries/0/allocations/" + std::to_string(index);
        if (RequireString(allocations[index], "stats", path, "id") !=
                allocation_ids[index] ||
            RequireString(allocations[index], "stats", path, "applies_to") !=
                allocation_targets[index])
            ThrowValidationError("stats", path,
                                 "stat allocation order is part of the cooked ABI");
        data.stats.allocations[index].amount_per_point = static_cast<float>(RequireNumber(allocations[index], "stats", path, "amount_per_point"));
        data.stats.allocations[index].immediate_current_hp_restore_per_point = static_cast<float>(RequireNumber(allocations[index], "stats", path, "immediate_current_hp_restore_per_point"));
    }
    const auto &combat = stat["combat"];
    data.stats.combat.minimum_final_damage = CheckedInteger<std::int32_t>(RequireInteger(combat, "stats", "$/entries/0/combat", "minimum_final_damage"), "stats", "$/entries/0/combat/minimum_final_damage");
    data.stats.combat.minimum_cooldown_ticks = ToTicks(RequireNumber(combat, "stats", "$/entries/0/combat", "minimum_cooldown_seconds"), "stats", "$/entries/0/combat/minimum_cooldown_seconds");
    data.stats.combat.skill_cooldown_upgrade_multiplier = static_cast<float>(RequireNumber(combat, "stats", "$/entries/0/combat", "skill_cooldown_upgrade_multiplier"));
    data.stats.combat.maximum_basic_attacks_per_tick = CheckedInteger<std::uint8_t>(RequireInteger(combat, "stats", "$/entries/0/combat", "maximum_basic_attacks_per_tick"), "stats", "$/entries/0/combat/maximum_basic_attacks_per_tick");
    data.stats.combat.critical_hits = RequireMember(combat, "stats", "$/entries/0/combat", "critical_hits").get<bool>();
    data.stats.combat.armor = RequireMember(combat, "stats", "$/entries/0/combat", "armor").get<bool>();
    data.stats.combat.hit_invulnerability = RequireMember(combat, "stats", "$/entries/0/combat", "hit_invulnerability").get<bool>();
    data.stats.combat.player_hit_knockback = RequireMember(combat, "stats", "$/entries/0/combat", "player_hit_knockback").get<bool>();
    data.stats.first_status_tick_delay_ticks = ToTicks(RequireNumber(combat, "stats", "$/entries/0/combat", "first_tick_delay_seconds"), "stats", "$/entries/0/combat/first_tick_delay_seconds");
    data.stats.player_collision_radius = static_cast<float>(RequireNumber(combat, "stats", "$/entries/0/combat", "player_collision_radius"));

    const auto &statuses = stat["statuses"];
    const auto status_path = [](std::size_t index) { return "$/entries/0/statuses/" + std::to_string(index); };
    if (RequireString(statuses[1], "stats", status_path(1), "id") != "status.bleed" || RequireString(statuses[2], "stats", status_path(2), "id") != "status.burn")
        ThrowValidationError("stats", "$/entries/0/statuses", "status order is part of the cooked ABI");
    data.statuses.first_tick_delay_ticks = data.stats.first_status_tick_delay_ticks;
    data.statuses.tick_interval_ticks = ToTicks(ParameterNumber(statuses[1], "stats", status_path(1), "tick_interval"), "stats", status_path(1) + "/parameters/tick_interval");
    data.status_tick_interval = data.statuses.tick_interval_ticks;
    data.statuses.bleed.maximum_stacks = Integer<std::uint8_t>(statuses[1], "stats", status_path(1), "maximum_stacks");
    if (data.statuses.bleed.maximum_stacks == 0 ||
        data.statuses.bleed.maximum_stacks > 5)
        ThrowValidationError("stats", status_path(1) + "/parameters/maximum_stacks",
                             "maximum_stacks must fit the fixed bleed capacity");
    data.statuses.bleed.duration_ticks = ToTicks(ParameterNumber(statuses[1], "stats", status_path(1), "duration"), "stats", status_path(1) + "/parameters/duration");
    data.statuses.bleed.tick_interval_ticks = ToTicks(ParameterNumber(statuses[1], "stats", status_path(1), "tick_interval"), "stats", status_path(1) + "/parameters/tick_interval");
    data.statuses.bleed.attack_power_multiplier_per_tick = Number(statuses[1], "stats", status_path(1), "attack_power_multiplier_per_tick");
    data.bleed_duration = data.statuses.bleed.duration_ticks;
    data.bleed_tick_coefficient = data.statuses.bleed.attack_power_multiplier_per_tick;
    data.statuses.burn.maximum_instances = Integer<std::uint8_t>(statuses[2], "stats", status_path(2), "maximum_instances");
    if (data.statuses.burn.maximum_instances != 1)
        ThrowValidationError("stats", status_path(2) + "/parameters/maximum_instances",
                             "single strongest burn requires exactly one instance");
    data.statuses.burn.duration_ticks = ToTicks(ParameterNumber(statuses[2], "stats", status_path(2), "duration"), "stats", status_path(2) + "/parameters/duration");
    data.statuses.burn.tick_interval_ticks = ToTicks(ParameterNumber(statuses[2], "stats", status_path(2), "tick_interval"), "stats", status_path(2) + "/parameters/tick_interval");
    data.statuses.burn.attack_power_multiplier_per_tick = Number(statuses[2], "stats", status_path(2), "attack_power_multiplier_per_tick");
    data.burn_duration = data.statuses.burn.duration_ticks;
    data.burn_tick_coefficient = data.statuses.burn.attack_power_multiplier_per_tick;
    if (data.statuses.bleed.tick_interval_ticks != data.statuses.burn.tick_interval_ticks)
        ThrowValidationError("stats", "$/entries/0/statuses", "bleed and burn tick intervals must match");

    const auto &level = sources.documents.at("level")["entries"][0];
    const auto &experience = level["experience"];
    const auto &required_xp = experience["required_xp"];
    data.growth.required_xp_base = static_cast<float>(RequireNumber(required_xp, "level", "$/entries/0/experience/required_xp", "base"));
    data.growth.required_xp_linear = static_cast<float>(RequireNumber(required_xp, "level", "$/entries/0/experience/required_xp", "linear"));
    data.growth.required_xp_quadratic = static_cast<float>(RequireNumber(required_xp, "level", "$/entries/0/experience/required_xp", "quadratic"));
    const auto &pickup = experience["pickup"];
    data.growth.experience_pickup_speed = static_cast<float>(RequireNumber(pickup, "level", "$/entries/0/experience/pickup", "speed_mps"));
    data.growth.experience_pickup_radius = static_cast<float>(RequireNumber(pickup, "level", "$/entries/0/experience/pickup", "collision_radius_m"));
    data.growth.utility_pickup_base_chance = static_cast<float>(RequireNumber(experience, "level", "$/entries/0/experience", "utility_pickup_base_chance"));
    data.growth.utility_pickup_miss_increment = static_cast<float>(RequireNumber(experience, "level", "$/entries/0/experience", "utility_pickup_miss_increment"));
    data.growth.heal_pickup_chance_multiplier = static_cast<float>(RequireNumber(experience, "level", "$/entries/0/experience", "heal_pickup_chance_multiplier"));
    data.growth.magnet_pickup_chance_multiplier = static_cast<float>(RequireNumber(experience, "level", "$/entries/0/experience", "magnet_pickup_chance_multiplier"));
    data.utility_pickup_base_chance = data.growth.utility_pickup_base_chance;
    data.utility_pickup_miss_increment = data.growth.utility_pickup_miss_increment;
    data.heal_pickup_chance_multiplier = data.growth.heal_pickup_chance_multiplier;
    data.magnet_pickup_chance_multiplier = data.growth.magnet_pickup_chance_multiplier;
    auto &progression = data.progression;
    progression.required_xp_base = data.growth.required_xp_base;
    progression.required_xp_linear = data.growth.required_xp_linear;
    progression.required_xp_quadratic = data.growth.required_xp_quadratic;
    const auto &rewards = RequireArray(experience, "level", "$/entries/0/experience", "rewards");
    constexpr std::array reward_ids{"enemy.melee", "enemy.ranged", "enemy.suicide", "boss.mid_5m", "boss.mid_10m", "boss.final_15m"};
    for (const auto &reward : rewards) {
        const auto id = RequireString(reward, "level", "$/entries/0/experience/rewards", "source_id");
        const auto xp = CheckedInteger<std::uint32_t>(RequireInteger(reward, "level", "$/entries/0/experience/rewards", "xp"), "level", "$/entries/0/experience/rewards/xp");
        auto it = std::ranges::find(reward_ids, id);
        if (it == reward_ids.end()) ThrowValidationError("level", "$/entries/0/experience/rewards", "unknown reward source");
        const auto index = static_cast<std::size_t>(it - reward_ids.begin());
        if (index < 3) progression.enemy_xp[index] = xp; else progression.boss_xp[index - 3] = xp;
    }
    const auto &cards = RequireMember(level, "level", "$/entries/0", "cards");
    progression.card_candidate_count = CheckedInteger<std::uint8_t>(RequireInteger(cards, "level", "$/entries/0/cards", "candidate_count"), "level", "$/entries/0/cards/candidate_count");
    progression.card_choose_count = CheckedInteger<std::uint8_t>(RequireInteger(cards, "level", "$/entries/0/cards", "choose_count"), "level", "$/entries/0/cards/choose_count");
    progression.active_slot_count = CheckedInteger<std::uint8_t>(RequireInteger(cards, "level", "$/entries/0/cards", "active_slot_count"), "level", "$/entries/0/cards/active_slot_count");
    progression.upgrades_selected_per_skill = CheckedInteger<std::uint8_t>(RequireInteger(cards, "level", "$/entries/0/cards", "upgrades_selected_per_skill"), "level", "$/entries/0/cards/upgrades_selected_per_skill");
    progression.base_stat_points_per_level = CheckedInteger<std::uint8_t>(RequireInteger(cards, "level", "$/entries/0/cards", "base_stat_points_per_level"), "level", "$/entries/0/cards/base_stat_points_per_level");
    progression.fallback_extra_stat_points = CheckedInteger<std::uint8_t>(RequireInteger(cards, "level", "$/entries/0/cards", "extra_stat_points_per_stat_card"), "level", "$/entries/0/cards/extra_stat_points_per_stat_card");
    progression.fallback_stat_point_cards = RequireMember(cards, "level", "$/entries/0/cards", "fallback_stat_point_cards").get<bool>();
    const auto &rerolls = RequireMember(level, "level", "$/entries/0", "rerolls");
    progression.level_initial_rerolls = CheckedInteger<std::uint8_t>(RequireInteger(rerolls, "level", "$/entries/0/rerolls", "level_initial_count"), "level", "$/entries/0/rerolls/level_initial_count");
    progression.relic_initial_rerolls = CheckedInteger<std::uint8_t>(RequireInteger(rerolls, "level", "$/entries/0/rerolls", "relic_initial_count"), "level", "$/entries/0/rerolls/relic_initial_count");
    progression.stop_normal_spawns_at_final_boss = RequireMember(RequireMember(level, "level", "$/entries/0", "session"), "level", "$/entries/0/session", "stop_new_normal_enemy_spawns_at_final_boss").get<bool>();
    const auto &session = RequireMember(level, "level", "$/entries/0", "session");
    progression.boss_spawn_ticks = {ToTicks(RequireNumber(session, "level", "$/entries/0/session", "first_mid_boss_seconds"), "level", "$/entries/0/session/first_mid_boss_seconds"), ToTicks(RequireNumber(session, "level", "$/entries/0/session", "second_mid_boss_seconds"), "level", "$/entries/0/session/second_mid_boss_seconds"), ToTicks(RequireNumber(session, "level", "$/entries/0/session", "final_boss_seconds"), "level", "$/entries/0/session/final_boss_seconds")};
    const auto &arena = level["arena"];
    const auto width = RequireNumber(arena, "level", "$/entries/0/arena", "width_m");
    if (width != RequireNumber(arena, "level", "$/entries/0/arena", "depth_m"))
        ThrowValidationError("level", "$/entries/0/arena", "GameData v1 requires a square arena");
    data.arena_half_extent = static_cast<float>(width * 0.5);

    const auto &character = sources.documents.at("characters")["entries"][0];
    const auto &camera = character["camera"];
    presentation.camera = {
        static_cast<float>(RequireNumber(camera, "characters", "$/entries/0/camera", "yaw_degrees")),
        static_cast<float>(RequireNumber(camera, "characters", "$/entries/0/camera", "pitch_degrees")),
        static_cast<float>(RequireNumber(camera, "characters", "$/entries/0/camera", "vertical_fov_degrees")),
        static_cast<float>(RequireNumber(camera, "characters", "$/entries/0/camera", "distance_m")),
        CheckedInteger<std::uint32_t>(RequireInteger(camera, "characters", "$/entries/0/camera", "minimum_distance_percent"), "characters", "$/entries/0/camera/minimum_distance_percent"),
        CheckedInteger<std::uint32_t>(RequireInteger(camera, "characters", "$/entries/0/camera", "maximum_distance_percent"), "characters", "$/entries/0/camera/maximum_distance_percent")};
    if (presentation.camera.minimum_distance_percent > presentation.camera.maximum_distance_percent)
        ThrowValidationError("characters", "$/entries/0/camera", "minimum distance must not exceed maximum distance");
    const auto spawn_view = DeriveMaxZoomView(presentation.camera);
    data.character_initial.starting_level = CheckedInteger<std::uint8_t>(RequireInteger(character, "characters", "$/entries/0", "starting_level"), "characters", "$/entries/0/starting_level");
    data.character_initial.starting_basic_attack_level = CheckedInteger<std::uint8_t>(RequireInteger(character, "characters", "$/entries/0", "starting_basic_attack_level"), "characters", "$/entries/0/starting_basic_attack_level");
    data.character_initial.starting_unspent_stat_points = CheckedInteger<std::uint8_t>(RequireInteger(character, "characters", "$/entries/0", "starting_unspent_stat_points"), "characters", "$/entries/0/starting_unspent_stat_points");
    data.character_initial.starting_level_rerolls = CheckedInteger<std::uint8_t>(RequireInteger(character, "characters", "$/entries/0", "starting_level_rerolls"), "characters", "$/entries/0/starting_level_rerolls");
    data.character_initial.starting_relic_rerolls = CheckedInteger<std::uint8_t>(RequireInteger(character, "characters", "$/entries/0", "starting_relic_rerolls"), "characters", "$/entries/0/starting_relic_rerolls");
    const auto skill_kind = [](std::string_view id) {
        constexpr std::array ids{"skill.basic_attack", "skill.piercing_shot", "skill.multishot", "skill.charged_shot", "skill.explosive_arrow", "skill.ricochet_arrow", "skill.arrow_rain", "skill.trap", "skill.retreat_shot"};
        for (std::size_t i = 0; i < ids.size(); ++i) if (ids[i] == id) return static_cast<SkillKind>(i);
        return SkillKind::Count;
    };
    const auto relic_kind = [](std::string_view id) {
        constexpr std::array ids{"relic.bleed_kill_heal", "relic.burn_propagation", "relic.kill_cooldown_surge", "relic.bleed_burn_explosion", "relic.radial_basic_attack", "relic.basic_kill_tracker", "relic.movement_echo", "relic.alternating_skills", "relic.different_skill_tracker", "relic.damage_knockback", "relic.once_revive", "relic.combat_hit_chain", "relic.projectile_cadence_reward", "relic.pre_damage_guard", "relic.slow_synergy", "relic.area_resonance", "relic.boss_pressure", "relic.hit_streak_reward", "relic.pickup_reward", "relic.low_health_survival"};
        for (std::size_t i = 0; i < ids.size(); ++i) if (ids[i] == id) return static_cast<RelicKind>(i);
        return RelicKind::Count;
    };
    data.character_initial.starting_active_skill_ids.fill(SkillKind::Count);
    data.character_initial.starting_relic_ids.fill(RelicKind::Count);
    const auto &starting_skills = RequireArray(character, "characters", "$/entries/0", "starting_active_skill_ids");
    const auto &starting_relics = RequireArray(character, "characters", "$/entries/0", "starting_relic_ids");
    if (starting_skills.size() > data.character_initial.starting_active_skill_ids.size() || starting_relics.size() > data.character_initial.starting_relic_ids.size())
        ThrowValidationError("characters", "$/entries/0", "starting selection exceeds cooked capacity");
    for (std::size_t i = 0; i < starting_skills.size(); ++i) {
        if (!starting_skills[i].is_string()) ThrowValidationError("characters", "$/entries/0/starting_active_skill_ids", "starting skill must be a string ID");
        const auto id = starting_skills[i].get<std::string>();
        const auto kind = skill_kind(id); if (kind == SkillKind::Count) ThrowValidationError("characters", "$/entries/0/starting_active_skill_ids", "unknown skill ID");
        for (std::size_t j = 0; j < i; ++j) if (data.character_initial.starting_active_skill_ids[j] == kind) ThrowValidationError("characters", "$/entries/0/starting_active_skill_ids", "duplicate starting skill");
        data.character_initial.starting_active_skill_ids[i] = kind;
    }
    for (std::size_t i = 0; i < starting_relics.size(); ++i) {
        if (!starting_relics[i].is_string()) ThrowValidationError("characters", "$/entries/0/starting_relic_ids", "starting relic must be a string ID");
        const auto id = starting_relics[i].get<std::string>();
        const auto kind = relic_kind(id); if (kind == RelicKind::Count) ThrowValidationError("characters", "$/entries/0/starting_relic_ids", "unknown relic ID");
        for (std::size_t j = 0; j < i; ++j) if (data.character_initial.starting_relic_ids[j] == kind) ThrowValidationError("characters", "$/entries/0/starting_relic_ids", "duplicate starting relic");
        data.character_initial.starting_relic_ids[i] = kind;
    }
    data.character_initial.starting_active_skill_count = CheckedInteger<std::uint8_t>(starting_skills.size(), "characters", "$/entries/0/starting_active_skill_ids");
    data.character_initial.starting_relic_count = CheckedInteger<std::uint8_t>(starting_relics.size(), "characters", "$/entries/0/starting_relic_ids");
    const auto &movement = character["movement"];
    data.character_initial.movement_stop_distance_m = static_cast<float>(RequireNumber(movement, "characters", "$/entries/0/movement", "stop_distance_m"));

    const auto &skill_document = sources.documents.at("skills");
    const auto acquisition_level = CheckedInteger<std::uint8_t>(
        RequireInteger(skill_document, "skills", "$", "acquisition_level"),
        "skills", "$/acquisition_level");
    const auto maximum_level = CheckedInteger<std::uint8_t>(
        RequireInteger(skill_document, "skills", "$", "maximum_level"),
        "skills", "$/maximum_level");
    const auto upgrades_per_skill = CheckedInteger<std::uint8_t>(
        RequireInteger(skill_document, "skills", "$", "upgrade_choices_per_skill"),
        "skills", "$/upgrade_choices_per_skill");
    const auto &skills = skill_document["entries"];
    constexpr std::array skill_ids{"skill.basic_attack", "skill.piercing_shot", "skill.multishot", "skill.charged_shot", "skill.explosive_arrow", "skill.ricochet_arrow", "skill.arrow_rain", "skill.trap", "skill.retreat_shot"};
    constexpr std::array skill_logics{"basic_projectile_cadence", "piercing_projectile", "uniform_fan_projectiles", "hold_release_linear_charge", "projectile_to_area_explosion", "nearest_unhit_target_ricochet", "targeted_periodic_area", "forward_roll_leave_trap", "forced_retreat_and_projectile"};
    const std::array<std::set<std::string, std::less<>>, 9> skill_keys{{
        {"damage_multiplier", "projectile_speed", "range", "collision_radius", "pierce", "starting_level", "maximum_level", "selected_upgrades_per_session"},
        {"cooldown", "damage_multiplier", "projectile_speed", "range", "collision_radius", "pierce", "damage_decay_per_pierce", "minimum_damage_fraction", "maximum_hits_per_target"},
        {"cooldown", "fan_angle", "projectile_count", "damage_multiplier_per_arrow", "projectile_speed", "range", "collision_radius", "pierce_per_arrow", "maximum_hits_per_target"},
        {"maximum_charge_time", "minimum_damage_multiplier", "maximum_damage_multiplier", "minimum_range", "maximum_range", "minimum_collision_radius", "maximum_collision_radius", "projectile_speed", "pierce", "movement_speed_multiplier_while_charging", "cooldown"},
        {"cooldown", "projectile_speed", "range", "collision_radius", "explosion_radius", "explosion_damage_multiplier"},
        {"cooldown", "damage_multiplier", "projectile_speed", "collision_radius", "initial_range", "ricochet_search_radius", "maximum_ricochets"},
        {"target_range", "cooldown", "activation_delay", "radius", "duration", "tick_interval", "damage_multiplier_per_tick", "total_damage_ticks"},
        {"forward_roll_distance", "forward_roll_duration", "cooldown", "activation_delay", "active_duration", "detection_radius", "explosion_radius", "damage_multiplier", "slow_fraction", "slow_duration"},
        {"forced_move_duration", "forced_move_distance", "damage_multiplier", "cooldown", "projectile_speed", "collision_radius", "projectile_range", "pierce"}}};
    for (std::size_t index = 0; index < skills.size(); ++index)
    {
        const auto path = "$/entries/" + std::to_string(index);
        if (RequireString(skills[index], "skills", path, "id") != skill_ids[index] || RequireString(skills[index], "skills", path, "logic_id") != skill_logics[index])
            ThrowValidationError("skills", path, "skill ID/logic order is part of the cooked ABI");
        RequireParameterKeys(skills[index], "skills", path, skill_keys[index]);
        auto &s = data.skills[index];
        s.handler = static_cast<AbilityHandlerId>(index);
        s.starting_level = acquisition_level;
        s.maximum_level = maximum_level;
        s.selected_upgrades_per_session = upgrades_per_skill;
        const auto n = [&](std::string_view key) { return Number(skills[index], "skills", path, key); };
        const auto ticks = [&](std::string_view key) { return ToTicks(n(key), "skills", path + "/parameters/" + std::string(key)); };
        if (index == 0) { s.damage_coefficient = n("damage_multiplier"); s.damage_multiplier = s.damage_coefficient; s.projectile_speed = n("projectile_speed"); s.range = n("range"); s.collision_radius = n("collision_radius"); s.pierce_count = Integer<std::uint8_t>(skills[index], "skills", path, "pierce"); if (Integer<std::uint8_t>(skills[index], "skills", path, "starting_level") != acquisition_level || Integer<std::uint8_t>(skills[index], "skills", path, "maximum_level") != maximum_level || Integer<std::uint8_t>(skills[index], "skills", path, "selected_upgrades_per_session") != upgrades_per_skill) ThrowValidationError("skills", path + "/parameters", "basic attack level limits must match the skills-wide values"); }
        if (index == 1) { s.cooldown_ticks = ticks("cooldown"); s.damage_coefficient = n("damage_multiplier"); s.damage_multiplier = s.damage_coefficient; s.projectile_speed = n("projectile_speed"); s.range = n("range"); s.collision_radius = n("collision_radius"); s.pierce_count = Integer<std::uint8_t>(skills[index], "skills", path, "pierce"); s.pierce_damage_decay_fraction = n("damage_decay_per_pierce"); s.minimum_damage_fraction = n("minimum_damage_fraction"); s.maximum_hits_per_target = Integer<std::uint8_t>(skills[index], "skills", path, "maximum_hits_per_target"); }
        if (index == 2) { s.cooldown_ticks = ticks("cooldown"); s.fan_angle_degrees = n("fan_angle"); s.projectile_count = Integer<std::uint8_t>(skills[index], "skills", path, "projectile_count"); s.damage_coefficient = n("damage_multiplier_per_arrow"); s.damage_multiplier_per_arrow = s.damage_coefficient; s.projectile_speed = n("projectile_speed"); s.range = n("range"); s.collision_radius = n("collision_radius"); s.pierce_per_arrow = Integer<std::uint8_t>(skills[index], "skills", path, "pierce_per_arrow"); s.pierce_count = s.pierce_per_arrow; s.maximum_hits_per_target = Integer<std::uint8_t>(skills[index], "skills", path, "maximum_hits_per_target"); }
        if (index == 3) { s.maximum_charge_time_ticks = ticks("maximum_charge_time"); s.duration_ticks = s.maximum_charge_time_ticks; s.minimum_damage_multiplier = n("minimum_damage_multiplier"); s.maximum_damage_multiplier = n("maximum_damage_multiplier"); s.damage_coefficient = s.maximum_damage_multiplier; s.minimum_range = n("minimum_range"); s.maximum_range = n("maximum_range"); s.range = s.maximum_range; s.minimum_collision_radius = n("minimum_collision_radius"); s.maximum_collision_radius = n("maximum_collision_radius"); s.collision_radius = s.maximum_collision_radius; s.projectile_speed = n("projectile_speed"); s.pierce_count = Integer<std::uint8_t>(skills[index], "skills", path, "pierce"); s.movement_speed_multiplier_while_charging = n("movement_speed_multiplier_while_charging"); s.cooldown_ticks = ticks("cooldown"); }
        if (index == 4) { s.cooldown_ticks = ticks("cooldown"); s.projectile_speed = n("projectile_speed"); s.range = n("range"); s.collision_radius = n("collision_radius"); s.explosion_radius = n("explosion_radius"); s.area_radius = s.explosion_radius; s.explosion_damage_multiplier = n("explosion_damage_multiplier"); s.damage_coefficient = s.explosion_damage_multiplier; }
        if (index == 5) { s.cooldown_ticks = ticks("cooldown"); s.damage_coefficient = n("damage_multiplier"); s.damage_multiplier = s.damage_coefficient; s.projectile_speed = n("projectile_speed"); s.collision_radius = n("collision_radius"); s.initial_range = n("initial_range"); s.range = s.initial_range; s.ricochet_search_radius = n("ricochet_search_radius"); s.area_radius = s.ricochet_search_radius; s.maximum_ricochets = Integer<std::uint8_t>(skills[index], "skills", path, "maximum_ricochets"); s.pierce_count = s.maximum_ricochets; }
        if (index == 6) { s.target_range = n("target_range"); s.range = s.target_range; s.cooldown_ticks = ticks("cooldown"); s.activation_delay_ticks = ticks("activation_delay"); s.area_radius = n("radius"); s.duration_ticks = ticks("duration"); s.tick_interval_ticks = ticks("tick_interval"); s.damage_coefficient = n("damage_multiplier_per_tick"); s.total_damage_ticks = Integer<std::uint8_t>(skills[index], "skills", path, "total_damage_ticks"); }
        if (index == 7) { s.forward_roll_distance = n("forward_roll_distance"); s.range = s.forward_roll_distance; s.forward_roll_duration_ticks = ticks("forward_roll_duration"); s.cooldown_ticks = ticks("cooldown"); s.activation_delay_ticks = ticks("activation_delay"); s.active_duration_ticks = ticks("active_duration"); s.duration_ticks = s.active_duration_ticks; s.detection_radius = n("detection_radius"); s.explosion_radius = n("explosion_radius"); s.area_radius = s.explosion_radius; s.damage_coefficient = n("damage_multiplier"); s.slow_fraction = n("slow_fraction"); s.slow_duration_ticks = ticks("slow_duration"); }
        if (index == 8) { s.forced_move_duration_ticks = ticks("forced_move_duration"); s.duration_ticks = s.forced_move_duration_ticks; s.forced_move_distance = n("forced_move_distance"); s.damage_coefficient = n("damage_multiplier"); s.damage_multiplier = s.damage_coefficient; s.cooldown_ticks = ticks("cooldown"); s.projectile_speed = n("projectile_speed"); s.collision_radius = n("collision_radius"); s.projectile_range = n("projectile_range"); s.range = s.projectile_range; s.pierce_count = Integer<std::uint8_t>(skills[index], "skills", path, "pierce"); }
    }

    const auto &upgrade_groups = sources.documents.at("upgrades")["groups"];
    constexpr std::array upgrade_skill_ids{"skill.basic_attack", "skill.piercing_shot", "skill.multishot", "skill.charged_shot", "skill.explosive_arrow", "skill.ricochet_arrow", "skill.arrow_rain", "skill.trap", "skill.retreat_shot"};
    constexpr std::array upgrade_logics = std::array<std::array<std::string_view, 8>, 9>{{
        {{"third_attack_delayed_basic_attack", "additional_pierce", "first_hit_split", "fourth_direct_arrow_bleed", "fourth_direct_arrow_burn", "first_hit_slow_and_reduce_longest_active_cooldown", "missed_arrow_reacquire", "post_active_three_arrow_replacement"}},
        {{"delayed_followup_arrow", "range_end_split", "apply_bleed", "damaging_slow_trail", "per_three_pierces_perpendicular_arrows", "align_hit_normal_enemy", "apply_and_transfer_burn_to_next_hit", "cooldown_multiplier"}},
        {{"delayed_second_fan", "original_arrow_first_hit_split", "additional_pierce_per_arrow", "rear_fan", "first_cast_hit_bleed", "apply_burn_and_transfer_arrow", "two_additional_outer_arrows", "missed_arrow_retarget"}},
        {{"extended_full_charge_explosion", "faster_charge", "increase_minimum_charge_damage", "additional_pierce", "charge_bleed_full_charge_rupture", "boss_or_fifth_pierce_split", "first_hit_burn_explosion", "full_charge_multi_kill_cooldown_refund"}},
        {{"delayed_reexplosion", "three_delayed_satellite_bombs", "eight_direction_fragments", "explosion_leaves_burning_area", "apply_bleed_and_blood_explosions", "pre_explosion_pull", "direct_hit_mark_other_active_explosion", "cooldown_multiplier"}},
        {{"return_to_player_rehit", "first_ricochet_branch_chain", "apply_bleed_and_extend_ricochets", "apply_and_copy_burn_to_next_target", "kill_small_arrows", "original_ricochet_kill_new_chain", "ricochet_cooldown_reduction", "cooldown_multiplier"}},
        {{"delayed_forward_secondary_area", "first_damage_tick_pull", "third_area_hit_bleed", "apply_burn_and_create_fire_areas", "tracking_arrow_each_damage_tick", "area_slow", "area_kill_arrow_outside", "extend_area_and_leave_slow"}},
        {{"roll_path_traps", "landing_slow_area", "single_reactivation", "trigger_bleed", "trigger_burn_and_fire_area", "pre_explosion_pull_and_slow", "trigger_mark_other_active_damage", "trap_kill_small_trap"}},
        {{"replace_with_three_arrows", "small_trap_at_start", "slow_trail", "apply_bleed_and_tracking_arrow", "landing_damage_and_push", "next_other_active_cooldown_refund", "boss_or_three_normal_hits_heal", "delayed_second_retreat_and_arrow"}}
    }};
    const auto group = [&](std::size_t index) -> const Json & { return upgrade_groups[index]; };
    for (std::size_t index = 0; index < upgrade_groups.size(); ++index)
    {
        const auto group_path = "$/groups/" + std::to_string(index);
        if (RequireString(group(index), "upgrades", group_path, "skill_id") != upgrade_skill_ids[index])
            ThrowValidationError("upgrades", group_path + "/skill_id", "upgrade group order is part of the cooked ABI");
        const auto &entries = RequireArray(group(index), "upgrades", group_path, "entries");
        for (std::size_t ordinal = 0; ordinal < entries.size(); ++ordinal)
        {
            const auto path = group_path + "/entries/" + std::to_string(ordinal);
            if (RequireInteger(entries[ordinal], "upgrades", path, "ordinal") != static_cast<std::int64_t>(ordinal + 1) || RequireString(entries[ordinal], "upgrades", path, "logic_id") != upgrade_logics[index][ordinal])
                ThrowValidationError("upgrades", path, "upgrade ordinal or logic_id does not match the typed ABI");
        }
    }
    const auto U = [&](std::size_t g, std::size_t i) -> const Json & { return upgrade_groups[g]["entries"][i]; };
    const auto upath = [](std::size_t g, std::size_t i) { return "$/groups/" + std::to_string(g) + "/entries/" + std::to_string(i); };
    auto &u = data.upgrades;
    RequireParameterKeys(U(0,0), "upgrades", upath(0,0), {"cadence_interval","delay","damage_multiplier"}); u.basic_attack.third_attack_delayed = {Integer<std::uint32_t>(U(0,0),"upgrades",upath(0,0),"cadence_interval"), ToTicks(Number(U(0,0),"upgrades",upath(0,0),"delay"),"upgrades",upath(0,0)+"/parameters/delay"), Number(U(0,0),"upgrades",upath(0,0),"damage_multiplier")};
    RequireParameterKeys(U(0,1), "upgrades", upath(0,1), {"additional_targets"}); u.basic_attack.additional_pierce = {Integer<std::uint32_t>(U(0,1),"upgrades",upath(0,1),"additional_targets")};
    RequireParameterKeys(U(0,2), "upgrades", upath(0,2), {"angles","projectile_count","damage_multiplier"}); u.basic_attack.first_hit_split = {Angles<2>(U(0,2),"upgrades",upath(0,2),"angles"), Integer<std::uint32_t>(U(0,2),"upgrades",upath(0,2),"projectile_count"), Number(U(0,2),"upgrades",upath(0,2),"damage_multiplier")};
    RequireParameterKeys(U(0,3), "upgrades", upath(0,3), {"direct_arrow_interval","bleed_stacks"}); u.basic_attack.direct_arrow_bleed = {Integer<std::uint32_t>(U(0,3),"upgrades",upath(0,3),"direct_arrow_interval"), Integer<std::uint32_t>(U(0,3),"upgrades",upath(0,3),"bleed_stacks")};
    RequireParameterKeys(U(0,4), "upgrades", upath(0,4), {"direct_arrow_interval"}); u.basic_attack.direct_arrow_burn = {Integer<std::uint32_t>(U(0,4),"upgrades",upath(0,4),"direct_arrow_interval")};
    RequireParameterKeys(U(0,5), "upgrades", upath(0,5), {"slow_fraction","slow_duration","cooldown_reduction"}); u.basic_attack.first_hit_slow_and_cooldown = {Number(U(0,5),"upgrades",upath(0,5),"slow_fraction"), ToTicks(Number(U(0,5),"upgrades",upath(0,5),"slow_duration"),"upgrades",upath(0,5)+"/parameters/slow_duration"), ToTicks(Number(U(0,5),"upgrades",upath(0,5),"cooldown_reduction"),"upgrades",upath(0,5)+"/parameters/cooldown_reduction")};
    RequireParameterKeys(U(0,6), "upgrades", upath(0,6), {"damage_multiplier","maximum_retargets","collision_radius_multiplier"}); u.basic_attack.missed_arrow_reacquire = {Number(U(0,6),"upgrades",upath(0,6),"damage_multiplier"), Integer<std::uint32_t>(U(0,6),"upgrades",upath(0,6),"maximum_retargets"), Number(U(0,6),"upgrades",upath(0,6),"collision_radius_multiplier")};
    RequireParameterKeys(U(0,7), "upgrades", upath(0,7), {"activation_window","angles","projectile_count","damage_multiplier_per_arrow"}); u.basic_attack.post_active_three_arrow = {ToTicks(Number(U(0,7),"upgrades",upath(0,7),"activation_window"),"upgrades",upath(0,7)+"/parameters/activation_window"), Angles<3>(U(0,7),"upgrades",upath(0,7),"angles"), Integer<std::uint32_t>(U(0,7),"upgrades",upath(0,7),"projectile_count"), Number(U(0,7),"upgrades",upath(0,7),"damage_multiplier_per_arrow")};

    // Remaining typed groups use the same exact-key boundary and direct JSON conversions.
    const std::array<std::array<std::set<std::string, std::less<>>, 8>, 8> keys{{
        {{ {"delay","damage_multiplier"}, {"angles","projectile_count","damage_multiplier","search_radius"}, {"bleed_stacks"}, {"width","duration","tick_interval","damage_multiplier_per_tick","slow_fraction","slow_duration"}, {"normal_enemy_pierce_interval","projectile_count","angle_from_direction","damage_multiplier"}, {"move_distance"}, {"maximum_transfer_targets"}, {"cooldown_multiplier"} }},
        {{ {"delay","fan_angle","projectile_count","damage_multiplier_per_arrow"}, {"angles","projectile_count_per_original","damage_multiplier"}, {"additional_pierce"}, {"projectile_count","damage_multiplier","fan_angle"}, {"bleed_stacks"}, {"damage_multiplier","maximum_transfers_per_cast"}, {"additional_projectiles","outer_angles","damage_multiplier"}, {"damage_multiplier","maximum_retargets","search_radius"} }},
        {{ {"maximum_charge_time","maximum_damage_multiplier","full_charge_end_explosion_damage_multiplier","explosion_radius"}, {"charge_time_multiplier"}, {"minimum_damage_multiplier"}, {"additional_pierce"}, {"bleed_stacks","existing_bleed_stacks_for_rupture","rupture_damage_multiplier"}, {"normal_enemy_pierce_count","angles","projectile_count","damage_multiplier"}, {"radius","damage_multiplier"}, {"minimum_normal_enemy_kills","current_cooldown_refund_fraction"} }},
        {{ {"delay","radius","damage_multiplier"}, {"bomb_count","angular_spacing","placement_radius","delay","explosion_radius","damage_multiplier"}, {"direction_count","damage_multiplier","collision_radius_multiplier","pierce"}, {"radius","duration","tick_interval","damage_multiplier_per_tick"}, {"radius","damage_multiplier","maximum_explosions_per_cast","bleed_stacks"}, {"duration","pull_radius"}, {"immediate_explosion_damage_multiplier","mark_explosion_damage_multiplier","mark_duration","mark_explosion_radius"}, {"cooldown_multiplier"} }},
        {{ {"damage_multiplier","maximum_rehit_targets"}, {"damage_multiplier","maximum_targets"}, {"additional_ricochets_per_bleeding_hit","maximum_additional_ricochets"}, {}, {"damage_multiplier","arrows_per_kill","maximum_arrows_per_cast","search_radius"}, {"new_chain_targets","damage_multiplier","search_radius"}, {"cooldown_reduction_per_ricochet","maximum_reduction"}, {"cooldown_multiplier"} }},
        {{ {"delay","forward_offset","radius","duration","damage_multiplier_per_tick"}, {"pull_radius"}, {"hit_ordinal","bleed_stacks"}, {"radius","duration","tick_interval","damage_multiplier_per_tick","maximum_areas_per_cast"}, {"search_radius","damage_multiplier"}, {"slow_fraction","slow_duration"}, {"damage_multiplier","maximum_triggers_per_cast"}, {"area_duration","additional_damage_ticks","post_area_duration","post_area_slow_fraction","post_area_slow_duration"} }},
        {{ {"trap_count","damage_multiplier_per_trap","spacing"}, {"radius","slow_fraction","duration"}, {"reactivation_delay","maximum_reactivations"}, {"bleed_stacks"}, {"area_radius","area_duration","tick_interval","damage_multiplier_per_tick"}, {"pull_radius","slow_fraction","slow_duration"}, {"immediate_damage_multiplier","mark_explosion_damage_multiplier","mark_duration","mark_explosion_radius"}, {"detection_radius","explosion_radius","activation_delay","active_duration","damage_multiplier","maximum_small_traps_per_cast"} }},
        {{ {"projectile_count","damage_multiplier_per_arrow","angles"}, {"damage_multiplier","detection_radius","explosion_radius","activation_delay","active_duration"}, {"duration","slow_fraction","radius","slow_duration"}, {"damage_multiplier","maximum_triggers_per_cast","search_radius","bleed_stacks"}, {"radius","damage_multiplier","push_distance"}, {"activation_window","cooldown_refund_fraction"}, {"minimum_normal_enemy_hits","maximum_hp_heal_fraction"}, {"delay_after_first_move","additional_move_distance","damage_multiplier"} }}
    }};
    for (std::size_t g = 1; g < 9; ++g) for (std::size_t i = 0; i < 8; ++i) RequireParameterKeys(U(g,i), "upgrades", upath(g,i), keys[g-1][i]);
    // Assigning the remaining groups is intentionally explicit per typed field in the
    // following compact conversions; no generic upgrade table is retained.
    auto tick = [&](std::size_t g, std::size_t i, std::string_view key) { return ToTicks(Number(U(g,i),"upgrades",upath(g,i),key),"upgrades",upath(g,i)+"/parameters/"+std::string(key)); };
    u.piercing_shot.delayed_followup={tick(1,0,"delay"),Number(U(1,0),"upgrades",upath(1,0),"damage_multiplier")}; u.piercing_shot.range_end_split={Angles<2>(U(1,1),"upgrades",upath(1,1),"angles"),Integer<std::uint32_t>(U(1,1),"upgrades",upath(1,1),"projectile_count"),Number(U(1,1),"upgrades",upath(1,1),"damage_multiplier"),Number(U(1,1),"upgrades",upath(1,1),"search_radius")}; u.piercing_shot.apply_bleed={Integer<std::uint32_t>(U(1,2),"upgrades",upath(1,2),"bleed_stacks")}; u.piercing_shot.damaging_slow_trail={Number(U(1,3),"upgrades",upath(1,3),"width"),tick(1,3,"duration"),tick(1,3,"tick_interval"),Number(U(1,3),"upgrades",upath(1,3),"damage_multiplier_per_tick"),Number(U(1,3),"upgrades",upath(1,3),"slow_fraction"),tick(1,3,"slow_duration")}; u.piercing_shot.per_three_pierces_perpendicular={Integer<std::uint32_t>(U(1,4),"upgrades",upath(1,4),"normal_enemy_pierce_interval"),Integer<std::uint32_t>(U(1,4),"upgrades",upath(1,4),"projectile_count"),Number(U(1,4),"upgrades",upath(1,4),"angle_from_direction"),Number(U(1,4),"upgrades",upath(1,4),"damage_multiplier")}; u.piercing_shot.align_hit_normal_enemy={Number(U(1,5),"upgrades",upath(1,5),"move_distance")}; u.piercing_shot.apply_and_transfer_burn={Integer<std::uint32_t>(U(1,6),"upgrades",upath(1,6),"maximum_transfer_targets")}; u.piercing_shot.cooldown_multiplier={Number(U(1,7),"upgrades",upath(1,7),"cooldown_multiplier")};
    u.multishot.delayed_second_fan={tick(2,0,"delay"),Number(U(2,0),"upgrades",upath(2,0),"fan_angle"),Integer<std::uint32_t>(U(2,0),"upgrades",upath(2,0),"projectile_count"),Number(U(2,0),"upgrades",upath(2,0),"damage_multiplier_per_arrow")}; u.multishot.original_arrow_first_hit_split={Angles<2>(U(2,1),"upgrades",upath(2,1),"angles"),Integer<std::uint32_t>(U(2,1),"upgrades",upath(2,1),"projectile_count_per_original"),Number(U(2,1),"upgrades",upath(2,1),"damage_multiplier")}; u.multishot.additional_pierce_per_arrow={Integer<std::uint32_t>(U(2,2),"upgrades",upath(2,2),"additional_pierce")}; u.multishot.rear_fan={Integer<std::uint32_t>(U(2,3),"upgrades",upath(2,3),"projectile_count"),Number(U(2,3),"upgrades",upath(2,3),"damage_multiplier"),Number(U(2,3),"upgrades",upath(2,3),"fan_angle")}; u.multishot.first_cast_hit_bleed={Integer<std::uint32_t>(U(2,4),"upgrades",upath(2,4),"bleed_stacks")}; u.multishot.apply_burn_and_transfer_arrow={Number(U(2,5),"upgrades",upath(2,5),"damage_multiplier"),Integer<std::uint32_t>(U(2,5),"upgrades",upath(2,5),"maximum_transfers_per_cast")}; u.multishot.two_additional_outer_arrows={Integer<std::uint32_t>(U(2,6),"upgrades",upath(2,6),"additional_projectiles"),Angles<2>(U(2,6),"upgrades",upath(2,6),"outer_angles"),Number(U(2,6),"upgrades",upath(2,6),"damage_multiplier")}; u.multishot.missed_arrow_retarget={Number(U(2,7),"upgrades",upath(2,7),"damage_multiplier"),Integer<std::uint32_t>(U(2,7),"upgrades",upath(2,7),"maximum_retargets"),Number(U(2,7),"upgrades",upath(2,7),"search_radius")};
    u.charged_shot.extended_full_charge_explosion={tick(3,0,"maximum_charge_time"),Number(U(3,0),"upgrades",upath(3,0),"maximum_damage_multiplier"),Number(U(3,0),"upgrades",upath(3,0),"full_charge_end_explosion_damage_multiplier"),Number(U(3,0),"upgrades",upath(3,0),"explosion_radius")}; u.charged_shot.faster_charge={Number(U(3,1),"upgrades",upath(3,1),"charge_time_multiplier")}; u.charged_shot.increase_minimum_charge_damage={Number(U(3,2),"upgrades",upath(3,2),"minimum_damage_multiplier")}; u.charged_shot.additional_pierce={Integer<std::uint32_t>(U(3,3),"upgrades",upath(3,3),"additional_pierce")}; u.charged_shot.charge_bleed_full_charge_rupture={Integer<std::uint32_t>(U(3,4),"upgrades",upath(3,4),"bleed_stacks"),Integer<std::uint32_t>(U(3,4),"upgrades",upath(3,4),"existing_bleed_stacks_for_rupture"),Number(U(3,4),"upgrades",upath(3,4),"rupture_damage_multiplier")}; u.charged_shot.boss_or_fifth_pierce_split={Integer<std::uint32_t>(U(3,5),"upgrades",upath(3,5),"normal_enemy_pierce_count"),Angles<8>(U(3,5),"upgrades",upath(3,5),"angles"),Integer<std::uint32_t>(U(3,5),"upgrades",upath(3,5),"projectile_count"),Number(U(3,5),"upgrades",upath(3,5),"damage_multiplier")}; u.charged_shot.first_hit_burn_explosion={Number(U(3,6),"upgrades",upath(3,6),"radius"),Number(U(3,6),"upgrades",upath(3,6),"damage_multiplier")}; u.charged_shot.full_charge_multi_kill_cooldown_refund={Integer<std::uint32_t>(U(3,7),"upgrades",upath(3,7),"minimum_normal_enemy_kills"),Number(U(3,7),"upgrades",upath(3,7),"current_cooldown_refund_fraction")};
    u.explosive_arrow.delayed_reexplosion={tick(4,0,"delay"),Number(U(4,0),"upgrades",upath(4,0),"radius"),Number(U(4,0),"upgrades",upath(4,0),"damage_multiplier")}; u.explosive_arrow.three_delayed_satellite_bombs={Integer<std::uint32_t>(U(4,1),"upgrades",upath(4,1),"bomb_count"),Number(U(4,1),"upgrades",upath(4,1),"angular_spacing"),Number(U(4,1),"upgrades",upath(4,1),"placement_radius"),tick(4,1,"delay"),Number(U(4,1),"upgrades",upath(4,1),"explosion_radius"),Number(U(4,1),"upgrades",upath(4,1),"damage_multiplier")}; u.explosive_arrow.eight_direction_fragments={Integer<std::uint32_t>(U(4,2),"upgrades",upath(4,2),"direction_count"),Number(U(4,2),"upgrades",upath(4,2),"damage_multiplier"),Number(U(4,2),"upgrades",upath(4,2),"collision_radius_multiplier"),Integer<std::uint32_t>(U(4,2),"upgrades",upath(4,2),"pierce")}; u.explosive_arrow.explosion_leaves_burning_area={Number(U(4,3),"upgrades",upath(4,3),"radius"),tick(4,3,"duration"),tick(4,3,"tick_interval"),Number(U(4,3),"upgrades",upath(4,3),"damage_multiplier_per_tick")}; u.explosive_arrow.apply_bleed_and_blood_explosions={Number(U(4,4),"upgrades",upath(4,4),"radius"),Number(U(4,4),"upgrades",upath(4,4),"damage_multiplier"),Integer<std::uint32_t>(U(4,4),"upgrades",upath(4,4),"maximum_explosions_per_cast"),Integer<std::uint32_t>(U(4,4),"upgrades",upath(4,4),"bleed_stacks")}; u.explosive_arrow.pre_explosion_pull={tick(4,5,"duration"),Number(U(4,5),"upgrades",upath(4,5),"pull_radius")}; u.explosive_arrow.direct_hit_mark_other_active_explosion={Number(U(4,6),"upgrades",upath(4,6),"immediate_explosion_damage_multiplier"),Number(U(4,6),"upgrades",upath(4,6),"mark_explosion_damage_multiplier"),tick(4,6,"mark_duration"),Number(U(4,6),"upgrades",upath(4,6),"mark_explosion_radius")}; u.explosive_arrow.cooldown_multiplier={Number(U(4,7),"upgrades",upath(4,7),"cooldown_multiplier")};
    u.ricochet_arrow.return_to_player_rehit={Number(U(5,0),"upgrades",upath(5,0),"damage_multiplier"),Integer<std::uint32_t>(U(5,0),"upgrades",upath(5,0),"maximum_rehit_targets")}; u.ricochet_arrow.first_ricochet_branch_chain={Number(U(5,1),"upgrades",upath(5,1),"damage_multiplier"),Integer<std::uint32_t>(U(5,1),"upgrades",upath(5,1),"maximum_targets")}; u.ricochet_arrow.apply_bleed_and_extend_ricochets={Integer<std::uint32_t>(U(5,2),"upgrades",upath(5,2),"additional_ricochets_per_bleeding_hit"),Integer<std::uint32_t>(U(5,2),"upgrades",upath(5,2),"maximum_additional_ricochets")}; u.ricochet_arrow.apply_and_copy_burn_to_next_target={1}; u.ricochet_arrow.kill_small_arrows={Number(U(5,4),"upgrades",upath(5,4),"damage_multiplier"),Integer<std::uint32_t>(U(5,4),"upgrades",upath(5,4),"arrows_per_kill"),Integer<std::uint32_t>(U(5,4),"upgrades",upath(5,4),"maximum_arrows_per_cast"),Number(U(5,4),"upgrades",upath(5,4),"search_radius")}; u.ricochet_arrow.original_ricochet_kill_new_chain={Integer<std::uint32_t>(U(5,5),"upgrades",upath(5,5),"new_chain_targets"),Number(U(5,5),"upgrades",upath(5,5),"damage_multiplier"),Number(U(5,5),"upgrades",upath(5,5),"search_radius")}; u.ricochet_arrow.ricochet_cooldown_reduction={tick(5,6,"cooldown_reduction_per_ricochet"),tick(5,6,"maximum_reduction")}; u.ricochet_arrow.cooldown_multiplier={Number(U(5,7),"upgrades",upath(5,7),"cooldown_multiplier")};
    u.arrow_rain.delayed_forward_secondary_area={tick(6,0,"delay"),Number(U(6,0),"upgrades",upath(6,0),"forward_offset"),Number(U(6,0),"upgrades",upath(6,0),"radius"),tick(6,0,"duration"),Number(U(6,0),"upgrades",upath(6,0),"damage_multiplier_per_tick")}; u.arrow_rain.first_damage_tick_pull={Number(U(6,1),"upgrades",upath(6,1),"pull_radius")}; u.arrow_rain.third_area_hit_bleed={Integer<std::uint32_t>(U(6,2),"upgrades",upath(6,2),"hit_ordinal"),Integer<std::uint32_t>(U(6,2),"upgrades",upath(6,2),"bleed_stacks")}; u.arrow_rain.apply_burn_and_create_fire_areas={Number(U(6,3),"upgrades",upath(6,3),"radius"),tick(6,3,"duration"),tick(6,3,"tick_interval"),Number(U(6,3),"upgrades",upath(6,3),"damage_multiplier_per_tick"),Integer<std::uint32_t>(U(6,3),"upgrades",upath(6,3),"maximum_areas_per_cast")}; u.arrow_rain.tracking_arrow_each_damage_tick={Number(U(6,4),"upgrades",upath(6,4),"search_radius"),Number(U(6,4),"upgrades",upath(6,4),"damage_multiplier")}; u.arrow_rain.area_slow={Number(U(6,5),"upgrades",upath(6,5),"slow_fraction"),tick(6,5,"slow_duration")}; u.arrow_rain.area_kill_arrow_outside={Number(U(6,6),"upgrades",upath(6,6),"damage_multiplier"),Integer<std::uint32_t>(U(6,6),"upgrades",upath(6,6),"maximum_triggers_per_cast")}; u.arrow_rain.extend_area_and_leave_slow={tick(6,7,"area_duration"),Integer<std::uint32_t>(U(6,7),"upgrades",upath(6,7),"additional_damage_ticks"),tick(6,7,"post_area_duration"),Number(U(6,7),"upgrades",upath(6,7),"post_area_slow_fraction"),tick(6,7,"post_area_slow_duration")};
    u.trap.roll_path_traps={Integer<std::uint32_t>(U(7,0),"upgrades",upath(7,0),"trap_count"),Number(U(7,0),"upgrades",upath(7,0),"damage_multiplier_per_trap"),Number(U(7,0),"upgrades",upath(7,0),"spacing")}; u.trap.landing_slow_area={Number(U(7,1),"upgrades",upath(7,1),"radius"),Number(U(7,1),"upgrades",upath(7,1),"slow_fraction"),tick(7,1,"duration")}; u.trap.single_reactivation={tick(7,2,"reactivation_delay"),Integer<std::uint32_t>(U(7,2),"upgrades",upath(7,2),"maximum_reactivations")}; u.trap.trigger_bleed={Integer<std::uint32_t>(U(7,3),"upgrades",upath(7,3),"bleed_stacks")}; u.trap.trigger_burn_and_fire_area={Number(U(7,4),"upgrades",upath(7,4),"area_radius"),tick(7,4,"area_duration"),tick(7,4,"tick_interval"),Number(U(7,4),"upgrades",upath(7,4),"damage_multiplier_per_tick")}; u.trap.pre_explosion_pull_and_slow={Number(U(7,5),"upgrades",upath(7,5),"pull_radius"),Number(U(7,5),"upgrades",upath(7,5),"slow_fraction"),tick(7,5,"slow_duration")}; u.trap.trigger_mark_other_active_damage={Number(U(7,6),"upgrades",upath(7,6),"immediate_damage_multiplier"),Number(U(7,6),"upgrades",upath(7,6),"mark_explosion_damage_multiplier"),tick(7,6,"mark_duration"),Number(U(7,6),"upgrades",upath(7,6),"mark_explosion_radius")}; u.trap.trap_kill_small_trap={Number(U(7,7),"upgrades",upath(7,7),"detection_radius"),Number(U(7,7),"upgrades",upath(7,7),"explosion_radius"),tick(7,7,"activation_delay"),tick(7,7,"active_duration"),Number(U(7,7),"upgrades",upath(7,7),"damage_multiplier"),Integer<std::uint32_t>(U(7,7),"upgrades",upath(7,7),"maximum_small_traps_per_cast")};
    u.retreat_shot.replace_with_three_arrows={Integer<std::uint32_t>(U(8,0),"upgrades",upath(8,0),"projectile_count"),Number(U(8,0),"upgrades",upath(8,0),"damage_multiplier_per_arrow"),Angles<3>(U(8,0),"upgrades",upath(8,0),"angles")}; u.retreat_shot.small_trap_at_start={Number(U(8,1),"upgrades",upath(8,1),"damage_multiplier"),Number(U(8,1),"upgrades",upath(8,1),"detection_radius"),Number(U(8,1),"upgrades",upath(8,1),"explosion_radius"),tick(8,1,"activation_delay"),tick(8,1,"active_duration")}; u.retreat_shot.slow_trail={tick(8,2,"duration"),Number(U(8,2),"upgrades",upath(8,2),"slow_fraction"),Number(U(8,2),"upgrades",upath(8,2),"radius"),tick(8,2,"slow_duration")}; u.retreat_shot.apply_bleed_and_tracking_arrow={Number(U(8,3),"upgrades",upath(8,3),"damage_multiplier"),Integer<std::uint32_t>(U(8,3),"upgrades",upath(8,3),"maximum_triggers_per_cast"),Number(U(8,3),"upgrades",upath(8,3),"search_radius"),Integer<std::uint32_t>(U(8,3),"upgrades",upath(8,3),"bleed_stacks")}; u.retreat_shot.landing_damage_and_push={Number(U(8,4),"upgrades",upath(8,4),"radius"),Number(U(8,4),"upgrades",upath(8,4),"damage_multiplier"),Number(U(8,4),"upgrades",upath(8,4),"push_distance")}; u.retreat_shot.next_other_active_cooldown_refund={tick(8,5,"activation_window"),Number(U(8,5),"upgrades",upath(8,5),"cooldown_refund_fraction")}; u.retreat_shot.boss_or_three_normal_hits_heal={Integer<std::uint32_t>(U(8,6),"upgrades",upath(8,6),"minimum_normal_enemy_hits"),Number(U(8,6),"upgrades",upath(8,6),"maximum_hp_heal_fraction")}; u.retreat_shot.delayed_second_retreat_and_arrow={tick(8,7,"delay_after_first_move"),Number(U(8,7),"upgrades",upath(8,7),"additional_move_distance"),Number(U(8,7),"upgrades",upath(8,7),"damage_multiplier")};

    const auto &enemy_document = sources.documents.at("enemies");
    data.enemy_scaling = {EnemyScalingMachine::CompletedMinutes, static_cast<float>(RequireNumber(enemy_document["spawn_scaling"], "enemies", "$/spawn_scaling", "hp_fraction_per_completed_minute")), static_cast<float>(RequireNumber(enemy_document["spawn_scaling"], "enemies", "$/spawn_scaling", "damage_fraction_per_completed_minute")), RequireMember(enemy_document["spawn_scaling"], "enemies", "$/spawn_scaling", "updates_existing_enemies").get<bool>(), RequireMember(enemy_document["spawn_scaling"], "enemies", "$/spawn_scaling", "applies_to_bosses").get<bool>()};
    const float common_enemy_radius = static_cast<float>(RequireNumber(enemy_document["common"], "enemies", "$/common", "collision_radius"));
    const auto &enemy_entries = enemy_document["entries"];
    constexpr std::array enemy_ids{"enemy.melee", "enemy.ranged", "enemy.suicide"};
    constexpr std::array enemy_logics{"straight_chase_melee", "approach_and_shoot_projectile", "straight_chase_self_destruct"};
    constexpr std::array enemy_meshes{"monster.mesh.slime", "monster.mesh.cactus",
                                      "monster.mesh.swarm09"};
    for (std::size_t index = 0; index < enemy_entries.size(); ++index)
    {
        const auto path = "$/entries/" + std::to_string(index);
        RequireIdAndLogic(enemy_entries[index], "enemies", path, enemy_ids[index], enemy_logics[index]);
        if (RequireString(enemy_entries[index], "enemies", path, "mesh_asset_id") !=
            enemy_meshes[index])
            ThrowValidationError("enemies", path + "/mesh_asset_id",
                                 "enemy role requires its selected monster asset");
        auto &enemy = data.enemies[index];
        enemy.health = CheckedInteger<std::int32_t>(RequireInteger(enemy_entries[index], "enemies", path, "base_hp"), "enemies", path + "/base_hp");
        enemy.move_speed = static_cast<float>(RequireNumber(enemy_entries[index], "enemies", path, "movement_speed_mps"));
        enemy.collision_radius = common_enemy_radius;
        if (index == 0) { RequireParameterKeys(enemy_entries[index], "enemies", path, {"attack_range","telegraph_duration","base_damage","reattack_interval"}); enemy.damage=Integer<std::int32_t>(enemy_entries[index],"enemies",path,"base_damage"); enemy.attack_range=Number(enemy_entries[index],"enemies",path,"attack_range"); enemy.warning_ticks=ToTicks(Number(enemy_entries[index],"enemies",path,"telegraph_duration"),"enemies",path+"/parameters/telegraph_duration"); enemy.attack_cooldown_ticks=ToTicks(Number(enemy_entries[index],"enemies",path,"reattack_interval"),"enemies",path+"/parameters/reattack_interval"); }
        if (index == 1) { RequireParameterKeys(enemy_entries[index], "enemies", path, {"maximum_hold_distance","telegraph_duration","base_damage","projectile_speed","projectile_range","projectile_collision_radius","reattack_interval"}); enemy.damage=Integer<std::int32_t>(enemy_entries[index],"enemies",path,"base_damage"); enemy.attack_range=Number(enemy_entries[index],"enemies",path,"maximum_hold_distance"); enemy.warning_ticks=ToTicks(Number(enemy_entries[index],"enemies",path,"telegraph_duration"),"enemies",path+"/parameters/telegraph_duration"); enemy.attack_cooldown_ticks=ToTicks(Number(enemy_entries[index],"enemies",path,"reattack_interval"),"enemies",path+"/parameters/reattack_interval"); enemy.projectile_speed=Number(enemy_entries[index],"enemies",path,"projectile_speed"); enemy.projectile_range=Number(enemy_entries[index],"enemies",path,"projectile_range"); enemy.projectile_collision_radius=Number(enemy_entries[index],"enemies",path,"projectile_collision_radius"); enemy.ranged_projectile_radius=enemy.projectile_collision_radius; }
        if (index == 2) { RequireParameterKeys(enemy_entries[index], "enemies", path, {"stop_distance","telegraph_duration","explosion_radius","base_damage"}); enemy.damage=Integer<std::int32_t>(enemy_entries[index],"enemies",path,"base_damage"); enemy.attack_range=Number(enemy_entries[index],"enemies",path,"stop_distance"); enemy.suicide_stop_distance=enemy.attack_range; enemy.warning_ticks=ToTicks(Number(enemy_entries[index],"enemies",path,"telegraph_duration"),"enemies",path+"/parameters/telegraph_duration"); enemy.suicide_explosion_radius=Number(enemy_entries[index],"enemies",path,"explosion_radius"); enemy.projectile_range=enemy.suicide_explosion_radius; }
    }
    for (std::size_t index = 0; index < enemy_entries.size(); ++index) {
        const auto path = "$/entries/" + std::to_string(index);
        const auto xp = CheckedInteger<std::uint32_t>(RequireInteger(enemy_entries[index], "enemies", path, "xp_reward"), "enemies", path + "/xp_reward");
        if (xp != progression.enemy_xp[index]) ThrowValidationError("enemies", path + "/xp_reward", "enemy XP disagrees with level progression");
    }

    const auto &relic_document = sources.documents.at("relics");
    const auto &box = relic_document["box_rules"];
    data.relic_drop.guaranteed_boxes_per_mid_boss = CheckedInteger<std::uint32_t>(RequireInteger(box,"relics","$/box_rules","guaranteed_boxes_per_mid_boss"),"relics","$/box_rules/guaranteed_boxes_per_mid_boss");
    data.relic_drop.mid_boss_maximum_hp_heal_fraction = static_cast<float>(RequireNumber(box,"relics","$/box_rules","mid_boss_maximum_hp_heal_fraction"));
    data.relic_drop.normal_enemy_base_probability = static_cast<float>(RequireNumber(box,"relics","$/box_rules","normal_enemy_base_probability_percent") / 100.0);
    data.relic_drop.normal_enemy_probability_increment_per_kill = static_cast<float>(RequireNumber(box,"relics","$/box_rules","normal_enemy_probability_increment_per_kill_percent") / 100.0);
    data.relic_drop.normal_enemy_probability_cap = static_cast<float>(RequireNumber(box,"relics","$/box_rules","normal_enemy_probability_cap_percent") / 100.0);
    data.relic_drop.maximum_choices_per_box = CheckedInteger<std::uint32_t>(RequireInteger(box,"relics","$/box_rules","maximum_choices_per_box"),"relics","$/box_rules/maximum_choices_per_box");
    data.relic_drop.hard_pity=box["hard_pity"].get<bool>(); data.relic_drop.reset_kill_counter_on_box_spawn=box["reset_kill_counter_on_box_spawn"].get<bool>(); data.relic_drop.stable_id_sort_before_seeded_shuffle=box["stable_id_sort_before_seeded_shuffle"].get<bool>(); data.relic_drop.stop_normal_box_rolls_after_all_acquired=box["stop_normal_box_rolls_after_all_acquired"].get<bool>();
    const auto &healing = relic_document["healing_pickup"];
    data.relic_drop.healing_pickup_probability = static_cast<float>(RequireNumber(healing,"relics","$/healing_pickup","normal_enemy_drop_probability_percent") / 100.0); data.relic_drop.healing_pickup_maximum_hp_heal_fraction=static_cast<float>(RequireNumber(healing,"relics","$/healing_pickup","maximum_hp_heal_fraction")); data.relic_drop.healing_pickup_independent_from_other_rewards=healing["independent_from_other_rewards"].get<bool>(); data.relic_drop.healing_pickup_can_coexist_with_xp_and_relic_box=healing["can_coexist_with_xp_and_relic_box"].get<bool>(); data.relic_chest_base_chance=data.relic_drop.normal_enemy_base_probability; data.relic_chest_miss_increment=data.relic_drop.normal_enemy_probability_increment_per_kill;
    const auto &relics = relic_document["entries"];
    for (std::size_t index=0; index<relics.size(); ++index) { const auto path="$/entries/"+std::to_string(index); CopyText(presentation.relic_names[index],RequireString(relics[index],"relics",path,"display_name"),"relics",path+"/display_name"); CopyText(presentation.relic_rules[index],RequireString(relics[index],"relics",path,"rule"),"relics",path+"/rule"); }
    const auto relic_number = [&](std::size_t index,std::string_view key){return Number(relics[index],"relics","$/entries/"+std::to_string(index),key);}; const auto relic_ticks=[&](std::size_t index,std::string_view key){return ToTicks(relic_number(index,key),"relics","$/entries/"+std::to_string(index)+"/parameters/"+std::string(key));}; const auto relic_count=[&](std::size_t index,std::string_view key){return CheckedInteger<std::uint32_t>(relic_number(index,key),"relics","$/entries/"+std::to_string(index)+"/parameters/"+std::string(key));};
    data.relics.bleed_kill_heal={relic_number(0,"maximum_hp_heal_fraction"),relic_ticks(0,"internal_cooldown")}; data.relics.burn_propagation={relic_number(1,"search_radius"),relic_number(1,"copied_burn_strength"),relic_count(1,"maximum_targets")}; data.relics.kill_cooldown_surge={relic_count(2,"kills_per_trigger"),relic_ticks(2,"all_active_cooldown_reduction")}; data.relics.bleed_burn_explosion={relic_number(3,"radius"),relic_number(3,"damage_multiplier"),relic_ticks(3,"per_target_cooldown")}; data.relics.radial_basic_attack={relic_count(4,"cadence_interval"),relic_count(4,"direction_count"),relic_number(4,"damage_multiplier")}; data.relics.basic_kill_tracker={relic_number(5,"search_radius"),relic_number(5,"damage_multiplier"),relic_count(5,"maximum_triggers_per_original_attack")}; data.relics.movement_echo={relic_number(6,"required_cumulative_distance"),relic_ticks(6,"position_history_age"),relic_number(6,"damage_multiplier")}; data.relics.alternating_skills={relic_ticks(7,"window"),relic_number(7,"cooldown_refund_fraction")}; data.relics.different_skill_tracker={relic_ticks(8,"window"),relic_number(8,"damage_multiplier"),relic_ticks(8,"per_target_cooldown")}; data.relics.damage_knockback={relic_number(9,"radius"),relic_number(9,"push_distance"),relic_number(9,"slow_fraction"),relic_ticks(9,"slow_duration"),relic_ticks(9,"cooldown")}; data.relics.once_revive={relic_number(10,"revive_hp_fraction"),relic_ticks(10,"invulnerability_duration"),relic_count(10,"maximum_triggers_per_session")}; data.relics.combat_hit_chain={relic_count(11,"direct_hits_per_trigger"),relic_number(11,"search_radius"),relic_count(11,"maximum_targets"),relic_number(11,"damage_multiplier")}; data.relics.projectile_cadence_reward={relic_count(12,"hits_per_trigger"),relic_ticks(12,"cooldown_reduction")}; data.relics.pre_damage_guard={relic_number(13,"damage_reduction_fraction"),relic_ticks(13,"cooldown")}; data.relics.slow_synergy={relic_number(14,"damage_multiplier"),relic_ticks(14,"per_target_cooldown")}; data.relics.area_resonance={relic_number(15,"damage_multiplier"),relic_ticks(15,"cooldown")}; data.relics.boss_pressure={relic_number(16,"damage_multiplier"),relic_ticks(16,"per_target_cooldown")}; data.relics.hit_streak_reward={relic_count(17,"direct_hits_per_trigger"),relic_number(17,"damage_multiplier")}; data.relics.pickup_reward={relic_number(18,"attack_power_fraction"),relic_ticks(18,"duration")}; data.relics.low_health_survival={relic_number(19,"health_threshold_fraction"),relic_number(19,"damage_reduction_fraction"),relic_ticks(19,"cooldown")};

    const auto &spawn = sources.documents.at("spawn_schedule"); const auto &intervals=spawn["continuous_intervals"]; const auto &compositions=spawn["compositions"]; const auto rate_at=[&](std::int64_t seconds){for(const auto &interval:intervals) if(seconds>=interval["start_seconds"].get<std::int64_t>()&&seconds<interval["end_seconds"].get<std::int64_t>()) return static_cast<float>(interval["rate_per_second"].get<double>()); ThrowValidationError("spawn_schedule","$/continuous_intervals","composition is outside intervals");}; for(std::size_t index=0;index<compositions.size();++index){const auto seconds=RequireInteger(compositions[index],"spawn_schedule","$/compositions/"+std::to_string(index),"start_seconds"); data.spawn_stages[index]={CheckedInteger<std::uint16_t>(seconds/60,"spawn_schedule","$/compositions/start_seconds"),ToTicks(static_cast<double>(seconds),"spawn_schedule","$/compositions/start_seconds"),rate_at(seconds),{CheckedInteger<std::uint8_t>(RequireInteger(compositions[index],"spawn_schedule","$/compositions/"+std::to_string(index),"melee_weight"),"spawn_schedule","$/compositions/melee_weight"),CheckedInteger<std::uint8_t>(RequireInteger(compositions[index],"spawn_schedule","$/compositions/"+std::to_string(index),"ranged_weight"),"spawn_schedule","$/compositions/ranged_weight"),CheckedInteger<std::uint8_t>(RequireInteger(compositions[index],"spawn_schedule","$/compositions/"+std::to_string(index),"suicide_weight"),"spawn_schedule","$/compositions/suicide_weight")}};}
    const auto &waves=spawn["waves"]; for(std::size_t index=0;index<waves.size();++index){const auto path="$/waves/"+std::to_string(index); const auto seconds=RequireNumber(waves[index],"spawn_schedule",path,"start_seconds"); const auto duration=ToTicks(RequireNumber(waves[index],"spawn_schedule",path,"duration_seconds"),"spawn_schedule",path+"/duration_seconds"); if(duration!=CheckedInteger<hs::Tick>(RequireInteger(waves[index],"spawn_schedule",path,"duration_ticks"),"spawn_schedule",path+"/duration_ticks")) ThrowValidationError("spawn_schedule",path,"duration ticks disagree with seconds"); data.waves[index]={CheckedInteger<std::uint16_t>(seconds/60.0,"spawn_schedule",path+"/start_seconds"),ToTicks(seconds,"spawn_schedule",path+"/start_seconds"),CheckedInteger<std::uint16_t>(RequireInteger(waves[index],"spawn_schedule",path,"total_count"),"spawn_schedule",path+"/total_count"),duration};}
    const auto &placement=sources.documents.at("spawn_schedule")["placement"]; data.spawn_placement={static_cast<float>(RequireNumber(placement,"spawn_schedule","$/placement","minimum_player_distance_m")),static_cast<float>(RequireNumber(placement,"spawn_schedule","$/placement","maximum_player_distance_m")),placement["require_inside_arena"].get<bool>(),placement["require_outside_max_zoom_view"].get<bool>(),ToTicks(RequireNumber(placement,"spawn_schedule","$/placement","fallback_warning_seconds"),"spawn_schedule","$/placement/fallback_warning_seconds"),SpawnFallbackLocation::FarthestArenaEdge,spawn_view.min_forward,spawn_view.max_forward,spawn_view.half_right,spawn_view.forward_x,spawn_view.forward_z};

    // Boss fields and pattern PODs are loaded by the same fixed key/logic contract;
    // the remaining runtime fields are populated in the next integration pass.
    const auto &boss_document=sources.documents.at("bosses"); const auto &common_boss=boss_document["common"]; data.boss_common={static_cast<float>(RequireNumber(common_boss,"bosses","$/common","collision_radius")),ToTicks(RequireNumber(common_boss,"bosses","$/common","spawn_warning_seconds"),"bosses","$/common/spawn_warning_seconds"),ToTicks(RequireNumber(common_boss,"bosses","$/common","initial_pattern_delay_seconds"),"bosses","$/common/initial_pattern_delay_seconds"),CheckedInteger<std::uint32_t>(RequireInteger(common_boss,"bosses","$/common","preferred_pattern_weight"),"bosses","$/common/preferred_pattern_weight"),CheckedInteger<std::uint32_t>(RequireInteger(common_boss,"bosses","$/common","other_pattern_weight"),"bosses","$/common/other_pattern_weight"),CheckedInteger<std::uint32_t>(RequireInteger(common_boss,"bosses","$/common","maximum_same_pattern_repeats"),"bosses","$/common/maximum_same_pattern_repeats"),common_boss["body_contact_damage"].get<bool>(),common_boss["time_scaling"].get<bool>(),common_boss["affected_by_damage"].get<bool>(),common_boss["affected_by_bleed"].get<bool>(),common_boss["affected_by_burn"].get<bool>(),common_boss["affected_by_slow"].get<bool>(),common_boss["immune_to_pull"].get<bool>(),common_boss["immune_to_push"].get<bool>(),common_boss["immune_to_alignment_move"].get<bool>(),static_cast<float>(RequireNumber(common_boss,"bosses","$/common","projectile_range")),static_cast<float>(RequireNumber(common_boss,"bosses","$/common","projectile_collision_radius"))};
    const auto &bosses=boss_document["entries"]; constexpr std::array boss_ids{"boss.mid_5m","boss.mid_10m","boss.final_15m"}; for(std::size_t index=0;index<bosses.size();++index){const auto path="$/entries/"+std::to_string(index); if(RequireString(bosses[index],"bosses",path,"id")!=boss_ids[index]) ThrowValidationError("bosses",path+"/id","boss order is part of the cooked ABI"); auto &boss=data.bosses[index]; boss.health=CheckedInteger<std::int32_t>(RequireInteger(bosses[index],"bosses",path,"hp"),"bosses",path+"/hp"); boss.spawn_growth_ticks=ToTicks(RequireNumber(bosses[index],"bosses",path,"spawn_growth_seconds"),"bosses",path+"/spawn_growth_seconds"); boss.target_kill_ticks=ToTicks(RequireNumber(bosses[index],"bosses",path,"target_kill_seconds"),"bosses",path+"/target_kill_seconds"); boss.movement_speed=static_cast<float>(RequireNumber(bosses[index],"bosses",path,"movement_speed_mps")); boss.collision_radius=data.boss_common.collision_radius; boss.projectile_range=data.boss_common.projectile_range; boss.projectile_collision_radius=data.boss_common.projectile_collision_radius; const auto &thresholds=bosses[index]["preference_thresholds"]; boss.preferred_distance_near=static_cast<float>(RequireNumber(thresholds,"bosses",path+"/preference_thresholds","near_m")); boss.preferred_distance_far=static_cast<float>(RequireNumber(thresholds,"bosses",path+"/preference_thresholds","far_m")); boss.phase_two_preferred_distance_near=static_cast<float>(RequireNumber(thresholds,"bosses",path+"/preference_thresholds","phase_two_near_m")); boss.phase_two_preferred_distance_far=static_cast<float>(RequireNumber(thresholds,"bosses",path+"/preference_thresholds","phase_two_far_m")); const auto &single=bosses[index]["single_pattern_recovery_seconds"]; const auto &interval=bosses[index]["phase2_pattern_interval_seconds"]; if(single.is_number()) boss.recovery_ticks=ToTicks(single.get<double>(),"bosses",path+"/single_pattern_recovery_seconds"); if(interval.is_number()){boss.phase2_pattern_interval_ticks=ToTicks(interval.get<double>(),"bosses",path+"/phase2_pattern_interval_seconds"); if(!single.is_number()) boss.recovery_ticks=boss.phase2_pattern_interval_ticks;} const auto &cycle=bosses[index]["phase2_cycle_recovery_seconds"]; if(cycle.is_number()) boss.phase2_cycle_recovery_ticks=ToTicks(cycle.get<double>(),"bosses",path+"/phase2_cycle_recovery_seconds"); const auto &reward=bosses[index]["reward"]; boss.reward={CheckedInteger<std::uint32_t>(RequireInteger(reward,"bosses",path+"/reward","relic_chest_count"),"bosses",path+"/reward/relic_chest_count"),static_cast<float>(RequireNumber(reward,"bosses",path+"/reward","maximum_hp_heal_fraction"))}; const auto &transition=bosses[index]["phase_transition"]; if(!transition.is_null()){boss.has_phase_transition=true; boss.phase_transition={static_cast<float>(RequireNumber(transition,"bosses",path+"/phase_transition","hp_fraction")),ToTicks(RequireNumber(transition,"bosses",path+"/phase_transition","invulnerability_seconds"),"bosses",path+"/phase_transition/invulnerability_seconds"),transition["remove_enemy_projectiles"].get<bool>(),transition["remove_enemy_areas"].get<bool>(),transition["keep_normal_enemies"].get<bool>(),transition["keep_mid_bosses"].get<bool>(),transition["keep_pickups"].get<bool>(),transition["keep_player_projectiles"].get<bool>(),transition["keep_player_areas"].get<bool>()};}}
    constexpr std::array boss_meshes{"monster.mesh.turtle_shell",
                                     "monster.mesh.chest_monster",
                                     "monster.mesh.beholder"};
    for (std::size_t index = 0; index < bosses.size(); ++index)
    {
        const auto path = "$/entries/" + std::to_string(index);
        if (RequireString(bosses[index], "bosses", path, "mesh_asset_id") !=
            boss_meshes[index])
            ThrowValidationError("bosses", path + "/mesh_asset_id",
                                 "boss role requires its selected monster asset");
    }
    for (std::size_t i = 0; i < bosses.size(); ++i)
    {
        const auto path = "$/entries/" + std::to_string(i);
        if (data.bosses[i].spawn_growth_ticks != progression.boss_spawn_ticks[i])
            ThrowValidationError("bosses", path + "/spawn_growth_seconds", "boss spawn time disagrees with level progression");
        const auto xp = CheckedInteger<std::uint32_t>(RequireInteger(bosses[i], "bosses", path, "xp_reward"), "bosses", path + "/xp_reward");
        if (xp != progression.boss_xp[i]) ThrowValidationError("bosses", path + "/xp_reward", "boss XP disagrees with level progression");
    }

    // Boss pattern values are authored as typed POD parameters.  Keep the
    // source order stable and reject unknown IDs rather than silently leaving
    // zero-filled pattern slots in the cooked ABI.
    for (std::size_t boss_index = 0; boss_index < bosses.size(); ++boss_index)
    {
        const auto pattern_path = [&](std::size_t index) {
            return "$/entries/" + std::to_string(boss_index) + "/patterns/" + std::to_string(index);
        };
        const auto &patterns = RequireArray(bosses[boss_index], "bosses",
                                            "$/entries/" + std::to_string(boss_index), "patterns");
        if (patterns.size() > data.bosses[boss_index].patterns.size())
            ThrowValidationError("bosses", pattern_path(0), "pattern count exceeds cooked capacity");
        data.bosses[boss_index].pattern_count = CheckedInteger<std::uint8_t>(patterns.size(), "bosses", pattern_path(0));
        for (std::size_t pattern_index = 0; pattern_index < patterns.size(); ++pattern_index)
        {
            const auto path = pattern_path(pattern_index);
            const auto id = RequireString(patterns[pattern_index], "bosses", path, "id");
            const auto logic = RequireString(patterns[pattern_index], "bosses", path, "logic_id");
            auto &out = data.bosses[boss_index].patterns[pattern_index];
            const auto n = [&](std::string_view key) { return Number(patterns[pattern_index], "bosses", path, key); };
            const auto i = [&](std::string_view key) { return Integer<std::uint8_t>(patterns[pattern_index], "bosses", path, key); };
            const auto d = [&](std::string_view key) { return ToTicks(n(key), "bosses", path + "/parameters/" + std::string(key)); };
            out.phase = static_cast<std::uint8_t>(id.find("phase2") != std::string::npos ? 2 : 1);
            const auto expected_logic = id.find("shockwave") != std::string::npos ? "expanding_shockwave_with_safe_gaps" :
                                        id.find("double_charge") != std::string::npos ? "double_retargeted_charge" :
                                        id.find("double_fan") != std::string::npos ? "double_offset_fan_projectiles" :
                                        id.find("ground_areas") != std::string::npos ? "predicted_position_ground_areas" :
                                        id.find("fan") != std::string::npos ? "fan_projectiles" : "line_charge";
            if (logic != expected_logic) ThrowValidationError("bosses", path + "/logic_id", "pattern logic does not match its typed ID");
            out.telegraph_duration_ticks = d("telegraph_duration");
            if (id.find("charge") != std::string::npos && id.find("double") == std::string::npos)
            {
                if (logic != "line_charge") ThrowValidationError("bosses", path + "/logic_id", "unexpected charge logic");
                RequireParameterKeys(patterns[pattern_index], "bosses", path, {"telegraph_duration", "distance", "speed", "damage"});
                out.logic = BossPatternLogic::LineCharge; out.distance = n("distance"); out.speed = n("speed");
                out.damage = Integer<std::int32_t>(patterns[pattern_index], "bosses", path, "damage");
            }
            else if (id.find("shockwave") != std::string::npos)
            {
                RequireParameterKeys(patterns[pattern_index], "bosses", path, {"telegraph_duration", "start_radius", "end_radius", "safe_gap_count", "safe_gap_angle", "duration", "tick_interval", "half_width", "damage"});
                out.logic = BossPatternLogic::ExpandingShockwaveWithSafeGaps; out.start_radius=n("start_radius"); out.end_radius=n("end_radius");
                out.safe_gap_count=i("safe_gap_count"); out.safe_gap_angle_degrees=n("safe_gap_angle"); out.shockwave_duration_ticks=d("duration"); out.tick_interval_ticks=d("tick_interval");
                out.shockwave_half_width=n("half_width"); out.damage=Integer<std::int32_t>(patterns[pattern_index], "bosses", path, "damage");
                if (out.safe_gap_count == 0 || out.shockwave_half_width <= 0.0f)
                    ThrowValidationError("bosses", path + "/parameters",
                                         "shockwave gaps and half width must be positive");
            }
            else if (logic == "fan_projectiles")
            {
                RequireParameterKeys(patterns[pattern_index], "bosses", path, {"telegraph_duration", "fan_angle", "projectile_count", "projectile_speed", "damage"});
                out.logic=BossPatternLogic::FanProjectiles; out.fan_angle_degrees=n("fan_angle"); out.projectile_count=i("projectile_count");
                out.projectile_speed=n("projectile_speed"); out.damage=Integer<std::int32_t>(patterns[pattern_index], "bosses", path, "damage");
            }
            else if (logic == "predicted_position_ground_areas")
            {
                const bool phase2 = out.phase == 2;
                RequireParameterKeys(patterns[pattern_index], "bosses", path,
                    phase2 ? std::initializer_list<std::string_view>{"area_count", "radius", "telegraph_duration", "duration", "tick_interval", "damage_per_tick", "placement_radius"}
                           : std::initializer_list<std::string_view>{"area_count", "radius", "telegraph_duration", "duration", "tick_interval", "damage_per_tick", "placement_radius", "prediction_lead"});
                out.logic=BossPatternLogic::PredictedPositionGroundAreas; out.area_count=i("area_count"); out.radius=n("radius"); out.duration_ticks=d("duration");
                out.tick_interval_ticks=d("tick_interval"); out.damage_per_tick=Integer<std::int32_t>(patterns[pattern_index], "bosses", path, "damage_per_tick"); out.ground_placement_radius=n("placement_radius");
                if (!phase2) out.prediction_lead_ticks=d("prediction_lead");
            }
            else if (logic == "double_retargeted_charge")
            {
                RequireParameterKeys(patterns[pattern_index], "bosses", path, {"telegraph_duration", "charge_count", "interval", "distance", "speed", "damage"});
                out.logic=BossPatternLogic::DoubleRetargetedCharge; out.charge_count=i("charge_count"); out.interval_ticks=d("interval"); out.distance=n("distance"); out.speed=n("speed"); out.damage=Integer<std::int32_t>(patterns[pattern_index], "bosses", path, "damage");
            }
            else if (logic == "double_offset_fan_projectiles")
            {
                RequireParameterKeys(patterns[pattern_index], "bosses", path, {"telegraph_duration", "volley_count", "interval", "projectiles_per_volley", "second_volley_angle_offset", "projectile_speed", "damage"});
                out.logic=BossPatternLogic::DoubleOffsetFanProjectiles; out.volley_count=i("volley_count"); out.interval_ticks=d("interval"); out.projectiles_per_volley=i("projectiles_per_volley"); out.second_volley_angle_offset_degrees=n("second_volley_angle_offset"); out.projectile_speed=n("projectile_speed"); out.damage=Integer<std::int32_t>(patterns[pattern_index], "bosses", path, "damage");
            }
            else ThrowValidationError("bosses", path + "/id", "unknown boss pattern ID");
        }
    }
    return content;
}

} // namespace hs::content
