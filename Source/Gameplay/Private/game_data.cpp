#include <hs/gameplay/game_data.hpp>

#include <hs/core/cooked_format.hpp>

#include <cstring>
#include <span>
#include <vector>

namespace hs
{

GameData GameData::Defaults() noexcept
{
    GameData data;
    data.skills = {
        SkillDefinition{0.0f, 1.0f, 32.0f, 18.0f, 0.18f, 0.0f, 0.0f, 1, 0},
        SkillDefinition{4.5f, 3.0f, 30.0f, 24.0f, 0.30f, 0.0f, 0.0f, 1, 255},
        SkillDefinition{5.5f, 1.2f, 25.0f, 16.0f, 0.18f, 0.0f, 0.0f, 5, 1},
        SkillDefinition{7.0f, 4.5f, 35.0f, 28.0f, 0.55f, 0.0f, 1.5f, 1, 255},
        SkillDefinition{6.5f, 2.8f, 22.0f, 18.0f, 0.25f, 3.0f, 0.0f, 1, 0},
        SkillDefinition{6.0f, 1.8f, 28.0f, 18.0f, 0.22f, 6.0f, 0.0f, 1, 5},
        SkillDefinition{9.0f, 0.7f, 0.0f, 20.0f, 0.0f, 4.0f, 3.0f, 1, 0},
        SkillDefinition{8.0f, 2.2f, 0.0f, 12.0f, 0.0f, 3.0f, 12.0f, 1, 0},
        SkillDefinition{7.0f, 2.2f, 32.0f, 16.0f, 0.22f, 0.0f, 0.2f, 1, 1},
    };
    data.enemies = {
        EnemyDefinition{15, 1.445f, 10, 1.0f, 0.35f, 1.2f, 0.0f, 0.0f},
        EnemyDefinition{12, 1.19f, 8, 12.0f, 0.5f, 2.2f, 5.5f, 18.0f},
        EnemyDefinition{13, 1.9125f, 25, 2.2f, 0.8f, 0.0f, 0.0f, 3.0f},
    };
    data.bosses = {
        BossDefinition{600, 1.8f},
        BossDefinition{1'375, 1.5f},
        BossDefinition{4'500, 1.2f},
    };
    data.spawn_stages = {
        SpawnStage{0, 0.5f, {100, 0, 0}}, SpawnStage{2, 1.0f, {80, 20, 0}},
        SpawnStage{4, 1.75f, {70, 20, 10}}, SpawnStage{6, 2.75f, {60, 25, 15}},
        SpawnStage{9, 4.0f, {50, 30, 20}}, SpawnStage{12, 5.0f, {45, 30, 25}},
        SpawnStage{14, 5.0f, {40, 30, 30}},
    };
    data.waves = {WaveDefinition{3, 30}, WaveDefinition{6, 45},
                  WaveDefinition{9, 65}, WaveDefinition{12, 85},
                  WaveDefinition{14, 100}};
    return data;
}

std::uint64_t GameDataSchemaHash() noexcept
{
    return Fnv1a64("project_hs_game_data_v4");
}

Result LoadCookedGameData(const std::filesystem::path &path, GameData &data,
                          std::uint64_t *content_hash)
{
    CookedHeader header;
    std::vector<std::byte> payload;
    if (auto result = ReadCookedPayload(path, GameDataSchemaHash(), header, payload); !result)
    {
        return result;
    }
    if (payload.size() != sizeof(GameData))
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay",
                               "Cooked game data has an unexpected table size.");
    }
    std::memcpy(&data, payload.data(), sizeof(data));
    if (data.version != 1)
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
