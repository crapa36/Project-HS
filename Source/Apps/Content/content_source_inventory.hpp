#pragma once

#include "content_paths.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hs::asset_pipeline { struct Input; }

namespace hs::content
{

inline constexpr std::array<std::string_view, 13> kDocumentNames = {
    "audio_cues", "bosses",   "characters", "enemies", "level",
    "materials",  "particles", "relics",    "skills",  "spawn_schedule",
    "stats",      "ui_strings", "upgrades",
};

[[nodiscard]] std::filesystem::path GameDataCategoryPath(std::string_view category);
[[nodiscard]] std::string ReadRequiredText(const std::filesystem::path &path);
void ValidateGameDataDirectory();
[[nodiscard]] std::uint64_t ScanSourceIdentity(bool include_presentation,
                                               std::vector<asset_pipeline::Input> &inputs);

} // namespace hs::content
