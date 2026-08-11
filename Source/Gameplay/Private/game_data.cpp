#include <hs/gameplay/game_data.hpp>

#include <hs/core/cooked_format.hpp>

#include <cstring>
#include <span>
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

GameData GameData::Defaults() noexcept
{
    GameData data;
    data.skills = {
        SkillDefinition{0.0f, 1.0f, 32.0f, 18.0f, 0.18f, 0.0f, 0.0f, 1, 0},
        SkillDefinition{4.0f, 1.8f, 30.0f, 24.0f, 0.30f, 0.0f, 0.0f, 1, 255},
        SkillDefinition{5.5f, 1.7f, 25.0f, 16.0f, 0.18f, 0.0f, 0.0f, 9, 1},
        SkillDefinition{2.0f, 11.5f, 35.0f, 16.8f, 0.55f, 0.0f, 1.0f, 1, 10},
        SkillDefinition{6.5f, 2.8f, 22.0f, 18.0f, 0.25f, 3.0f, 0.0f, 1, 0},
        SkillDefinition{6.0f, 0.8f, 28.0f, 18.0f, 0.22f, 6.0f, 0.0f, 1, 5},
        SkillDefinition{9.0f, 0.7f, 0.0f, 20.0f, 0.0f, 4.0f, 3.0f, 1, 0},
        SkillDefinition{8.0f, 1.2f, 0.0f, 12.0f, 0.0f, 3.0f, 12.0f, 1, 0},
        SkillDefinition{7.0f, 3.5f, 32.0f, 16.0f, 0.22f, 0.0f, 0.2f, 1, 3},
    };
    data.enemies = {
        EnemyDefinition{15, 1.445f, 10, 1.0f, 0.35f, 1.2f, 0.0f, 0.0f},
        EnemyDefinition{12, 1.19f, 8, 12.0f, 0.5f, 2.444444f, 6.875f, 18.0f},
        EnemyDefinition{13, 3.825f, 25, 2.2f, 0.8f, 0.0f, 0.0f, 3.0f},
    };
    data.bosses = {
        BossDefinition{600, 1.8f},
        BossDefinition{1'375, 1.5f},
        BossDefinition{4'500, 1.2f},
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

std::uint64_t GameDataSchemaHash() noexcept
{
    return Fnv1a64("project_hs_game_data_v8");
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
    if (data.version != 2)
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
