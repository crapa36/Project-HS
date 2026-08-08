#pragma once

#include <hs/core/result.hpp>
#include <hs/gameplay/gameplay_types.hpp>

#include <array>
#include <cstdint>
#include <filesystem>

namespace hs
{

struct SkillDefinition
{
    float cooldown_seconds{};
    float damage_coefficient{};
    float projectile_speed{};
    float range{};
    float collision_radius{};
    float area_radius{};
    float duration_seconds{};
    std::uint8_t projectile_count{1};
    std::uint8_t pierce_count{};
};

struct EnemyDefinition
{
    std::int32_t health{};
    float move_speed{};
    std::int32_t damage{};
    float attack_range{};
    float warning_seconds{};
    float attack_cooldown_seconds{};
    float projectile_speed{};
    float projectile_range{};
};

struct BossDefinition
{
    std::int32_t health{};
    float recovery_seconds{};
};

struct SpawnStage
{
    std::uint16_t start_minute{};
    float per_second{};
    std::array<std::uint8_t, 3> weights{};
};

struct WaveDefinition
{
    std::uint16_t minute{};
    std::uint16_t count{};
};

struct GameData
{
    std::uint32_t version{1};
    float arena_half_extent{60.0f};
    std::int32_t player_health{100};
    float player_attack{10.0f};
    float player_attack_speed{1.5f};
    float player_move_speed{5.0f};
    float player_magnet_radius{3.0f};
    float utility_pickup_base_chance{0.01f};
    float utility_pickup_miss_increment{0.001f};
    std::array<SkillDefinition, kCombatSkillCount> skills{};
    std::array<EnemyDefinition, 3> enemies{};
    std::array<BossDefinition, 3> bosses{};
    std::array<SpawnStage, 7> spawn_stages{};
    std::array<WaveDefinition, 5> waves{};

    [[nodiscard]] static GameData Defaults() noexcept;
};

[[nodiscard]] std::uint64_t GameDataSchemaHash() noexcept;
[[nodiscard]] Result LoadCookedGameData(const std::filesystem::path &path,
                                        GameData &data,
                                        std::uint64_t *content_hash = nullptr);

} // namespace hs
