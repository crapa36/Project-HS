#pragma once

#include <filesystem>

namespace hs::content::paths
{

[[nodiscard]] std::filesystem::path SourceRoot();
[[nodiscard]] std::filesystem::path BuildRoot();

[[nodiscard]] std::filesystem::path GameData();
[[nodiscard]] std::filesystem::path Audio();
[[nodiscard]] std::filesystem::path Schema();
[[nodiscard]] std::filesystem::path VfxTextures();
[[nodiscard]] std::filesystem::path ArcherModel();
[[nodiscard]] std::filesystem::path ArcherAnimations();
[[nodiscard]] std::filesystem::path MonsterModels();
[[nodiscard]] std::filesystem::path MonsterAnimations();
[[nodiscard]] std::filesystem::path MonsterTextures();

} // namespace hs::content::paths
