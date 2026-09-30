#pragma once
#include <filesystem>
namespace hs::content {
void ValidateVfxMeshAtlas(const std::filesystem::path &source);
void CookVfxMeshAtlas(const std::filesystem::path &source,const std::filesystem::path &output);
}
