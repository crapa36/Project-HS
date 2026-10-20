#pragma once

#include "content_paths.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

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
    std::uint64_t gameplay_source_hash{14695981039346656037ull};
    std::uint64_t all_source_hash{14695981039346656037ull};
    struct Input
    {
        std::string logical_name;
        std::filesystem::path path;
    };
    std::vector<Input> inputs;
};

using IdentityChunkObserver = std::function<void(
    std::string_view logical_name, const std::filesystem::path &path,
    std::string_view bytes, bool last_chunk)>;

[[nodiscard]] bool IsGameDataCategory(std::string_view value) noexcept;
[[nodiscard]] std::filesystem::path GameDataCategoryPath(std::string_view category);
[[nodiscard]] std::string ReadRequiredText(const std::filesystem::path &path);
void ValidateGameDataDirectory();
[[nodiscard]] ContentSourceInventory LoadContentSourceInventory();
[[nodiscard]] ContentSourceInventory ScanLegacyIdentity(
    bool include_presentation, const IdentityChunkObserver &observer = {});
[[nodiscard]] std::uint64_t ComputeGameplaySourceHash(
    const ContentSourceInventory &sources) noexcept;
[[nodiscard]] std::uint64_t ComputeContentSourceHash(
    const ContentSourceInventory &sources) noexcept;

} // namespace hs::content
