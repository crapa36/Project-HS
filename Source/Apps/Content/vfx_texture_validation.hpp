#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>

namespace hs::content
{
// Validate source DDS files mapped from catalog runtime destinations.
void ValidateVfxTextures(const nlohmann::json &spec, const std::filesystem::path &root);
}
