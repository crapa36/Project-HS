#pragma once
#include <filesystem>
#include <string_view>
namespace hs::content {
void CookVfxMeshAtlas(std::string_view source_json,const std::filesystem::path &output);
}
