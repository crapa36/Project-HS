#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>

namespace hs::content
{
// Validate the staged DDS against its authored catalog entry.
void ValidateVfxTexture(const nlohmann::json &entry, const std::filesystem::path &path);
}
