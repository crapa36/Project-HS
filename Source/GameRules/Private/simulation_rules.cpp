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
        {{0, 0, 0, 0, TagMask(Bleed), 0, TagMask(Burn), 0}},
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
    SimulationRules data;
    data.skills = {
        SkillDefinition{0, 1.0f, 32.0f, 18.0f, 0.36f, 0.0f, 0, 1, 0, AbilityHandlerId::BasicProjectileCadence},
        SkillDefinition{240, 1.8f, 30.0f, 24.0f, 0.60f, 0.0f, 0, 1, 255, AbilityHandlerId::PiercingProjectile},
        SkillDefinition{330, 1.7f, 25.0f, 16.0f, 0.36f, 0.0f, 0, 9, 1, AbilityHandlerId::UniformFanProjectiles},
        SkillDefinition{240, 23.0f, 35.0f, 16.8f, 0.88f, 0.0f, 60, 1, 12, AbilityHandlerId::HoldReleaseLinearCharge},
        SkillDefinition{390, 2.8f, 22.0f, 18.0f, 0.50f, 3.0f, 0, 1, 0, AbilityHandlerId::ProjectileToAreaExplosion},
        SkillDefinition{360, 0.8f, 28.0f, 18.0f, 0.44f, 6.0f, 0, 1, 5, AbilityHandlerId::NearestUnhitTargetRicochet},
        SkillDefinition{540, 0.7f, 0.0f, 20.0f, 0.0f, 4.0f, 180, 1, 0, AbilityHandlerId::TargetedPeriodicArea},
        SkillDefinition{480, 1.2f, 0.0f, 12.0f, 0.0f, 3.0f, 720, 1, 0, AbilityHandlerId::ForwardRollLeaveTrap},
        SkillDefinition{420, 3.5f, 32.0f, 16.0f, 0.44f, 0.0f, 12, 1, 3, AbilityHandlerId::ForcedRetreatAndProjectile},
    };
    data.enemies = {
        EnemyDefinition{15, 1.445f, 10, 1.0f, 21, 72, 0.0f, 0.0f},
        EnemyDefinition{12, 1.19f, 8, 12.0f, 30, 147, 6.875f, 18.0f},
        EnemyDefinition{13, 3.825f, 25, 2.2f, 48, 0, 0.0f, 3.0f},
    };
    data.bosses = {
        BossDefinition{600, 108},
        BossDefinition{1'375, 90},
        BossDefinition{4'500, 72},
    };
    data.spawn_stages = {
        SpawnStage{0, 0.9f, {100, 0, 0}}, SpawnStage{2, 1.2f, {80, 20, 0}},
        SpawnStage{4, 2.1f, {74, 21, 5}}, SpawnStage{6, 3.3f, {65, 27, 8}},
        SpawnStage{9, 4.8f, {56, 34, 10}}, SpawnStage{12, 4.8f, {52, 35, 13}},
        SpawnStage{14, 4.8f, {49, 36, 15}},
    };
    data.waves = {WaveDefinition{3, 30, 1'200}, WaveDefinition{6, 45, 1'200},
                  WaveDefinition{9, 65, 1'200}, WaveDefinition{12, 85, 1'200},
                  WaveDefinition{14, 100, 1'200}};
    return data;
}

std::uint64_t SimulationRulesSchemaHash() noexcept
{
    return Fnv1a64("project_hs_simulation_rules_v2");
}

std::uint64_t SimulationRulesHash(const SimulationRules &rules) noexcept
{
    CanonicalHash hash;
    hash.Add(rules.version);
    hash.Add(rules.arena_half_extent);
    hash.Add(rules.player_health);
    hash.Add(rules.player_attack);
    hash.Add(rules.player_attack_speed);
    hash.Add(rules.player_move_speed);
    hash.Add(rules.player_magnet_radius);
    hash.Add(rules.utility_pickup_base_chance);
    hash.Add(rules.utility_pickup_miss_increment);
    hash.Add(rules.heal_pickup_chance_multiplier);
    hash.Add(rules.magnet_pickup_chance_multiplier);
    hash.Add(rules.relic_chest_base_chance);
    hash.Add(rules.relic_chest_miss_increment);
    hash.Add(rules.status_tick_interval);
    hash.Add(rules.bleed_duration);
    hash.Add(rules.bleed_tick_coefficient);
    hash.Add(rules.burn_duration);
    hash.Add(rules.burn_tick_coefficient);
    AddArray(hash, rules.skills, [](CanonicalHash &output, const SkillDefinition &skill) {
        output.Add(skill.cooldown_ticks);
        output.Add(skill.damage_coefficient);
        output.Add(skill.projectile_speed);
        output.Add(skill.range);
        output.Add(skill.collision_radius);
        output.Add(skill.area_radius);
        output.Add(skill.duration_ticks);
        output.Add(skill.projectile_count);
        output.Add(skill.pierce_count);
        output.Add(skill.handler);
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
    });
    AddArray(hash, rules.bosses, [](CanonicalHash &output, const BossDefinition &boss) {
        output.Add(boss.health);
        output.Add(boss.recovery_ticks);
    });
    AddArray(hash, rules.spawn_stages,
             [](CanonicalHash &output, const SpawnStage &stage) {
                 output.Add(stage.start_minute);
                 output.Add(stage.per_second);
                 for (const auto weight : stage.weights) output.Add(weight);
             });
    AddArray(hash, rules.waves, [](CanonicalHash &output, const WaveDefinition &wave) {
        output.Add(wave.minute);
        output.Add(wave.count);
        output.Add(wave.duration_ticks);
    });

    const auto &relics = rules.relics;
    hash.Add(relics.bleed_kill_heal.maximum_hp_heal_fraction);
    hash.Add(relics.bleed_kill_heal.internal_cooldown_ticks);
    hash.Add(relics.burn_propagation.search_radius);
    hash.Add(relics.burn_propagation.copied_burn_strength);
    hash.Add(relics.burn_propagation.maximum_targets);
    hash.Add(relics.kill_cooldown_surge.kills_per_trigger);
    hash.Add(relics.kill_cooldown_surge.cooldown_reduction_ticks);
    hash.Add(relics.bleed_burn_explosion.radius);
    hash.Add(relics.bleed_burn_explosion.damage_multiplier);
    hash.Add(relics.bleed_burn_explosion.per_target_cooldown_ticks);
    hash.Add(relics.radial_basic_attack.cadence_interval);
    hash.Add(relics.radial_basic_attack.direction_count);
    hash.Add(relics.radial_basic_attack.damage_multiplier);
    hash.Add(relics.basic_kill_tracker.search_radius);
    hash.Add(relics.basic_kill_tracker.damage_multiplier);
    hash.Add(relics.basic_kill_tracker.maximum_triggers_per_attack);
    hash.Add(relics.movement_echo.required_distance);
    hash.Add(relics.movement_echo.position_history_age_ticks);
    hash.Add(relics.movement_echo.damage_multiplier);
    hash.Add(relics.alternating_skills.window_ticks);
    hash.Add(relics.alternating_skills.cooldown_refund_fraction);
    hash.Add(relics.different_skill_tracker.window_ticks);
    hash.Add(relics.different_skill_tracker.damage_multiplier);
    hash.Add(relics.different_skill_tracker.per_target_cooldown_ticks);
    hash.Add(relics.damage_knockback.radius);
    hash.Add(relics.damage_knockback.push_distance);
    hash.Add(relics.damage_knockback.slow_fraction);
    hash.Add(relics.damage_knockback.slow_duration_ticks);
    hash.Add(relics.damage_knockback.cooldown_ticks);
    hash.Add(relics.once_revive.health_fraction);
    hash.Add(relics.once_revive.invulnerability_ticks);
    hash.Add(relics.once_revive.maximum_triggers_per_session);
    hash.Add(relics.combat_hit_chain.direct_hits_per_trigger);
    hash.Add(relics.combat_hit_chain.search_radius);
    hash.Add(relics.combat_hit_chain.maximum_targets);
    hash.Add(relics.combat_hit_chain.damage_multiplier);
    return hash.Value();
}

Result LoadSimulationRules(const std::filesystem::path &path,
                           SimulationRules &rules,
                           std::uint64_t *content_hash)
{
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
    if (rules.version != 4)
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
