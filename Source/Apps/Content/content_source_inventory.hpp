#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#ifndef HS_GAME_DATA_DIRECTORY
#define HS_GAME_DATA_DIRECTORY "ContentSource/GameData"
#endif

#ifndef HS_CHARACTER_MODEL
#define HS_CHARACTER_MODEL "ContentSource/Models/Characters/Archer/ErikaArcher.fbx"
#endif

#ifndef HS_CHARACTER_ANIMATION_DIRECTORY
#define HS_CHARACTER_ANIMATION_DIRECTORY "ContentSource/Animations/Characters/Archer"
#endif

#ifndef HS_MONSTER_MODEL_DIRECTORY
#define HS_MONSTER_MODEL_DIRECTORY "ContentSource/Models/Monsters"
#endif

#ifndef HS_MONSTER_ANIMATION_DIRECTORY
#define HS_MONSTER_ANIMATION_DIRECTORY "ContentSource/Animations/Monsters"
#endif

#ifndef HS_AUDIO_DIRECTORY
#define HS_AUDIO_DIRECTORY "ContentSource/Audio"
#endif

#ifndef HS_MONSTER_TEXTURE_DIRECTORY
#define HS_MONSTER_TEXTURE_DIRECTORY "ContentSource/Textures/Monsters/PBR"
#endif

namespace hs::content
{

inline constexpr std::array<std::string_view, 13> kDocumentNames = {
    "audio_cues", "bosses",   "characters", "enemies", "level",
    "materials",  "particles", "relics",    "skills",  "spawn_schedule",
    "stats",      "ui_strings", "upgrades",
};

struct ContentSourceInventory
{
    std::array<std::string, kDocumentNames.size()> document_text;
    std::string gameplay_source_bytes;
    std::string all_source_bytes;
};

[[nodiscard]] bool IsGameDataCategory(std::string_view value) noexcept;
[[nodiscard]] std::filesystem::path GameDataCategoryPath(std::string_view category);
[[nodiscard]] std::string ReadRequiredText(const std::filesystem::path &path);
void ValidateGameDataDirectory();
[[nodiscard]] ContentSourceInventory LoadContentSourceInventory();
[[nodiscard]] std::uint64_t ComputeGameplaySourceHash(
    const ContentSourceInventory &sources) noexcept;
[[nodiscard]] std::uint64_t ComputeContentSourceHash(
    const ContentSourceInventory &sources) noexcept;

} // namespace hs::content
