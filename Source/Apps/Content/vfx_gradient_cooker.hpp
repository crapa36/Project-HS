#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>

namespace hs::content
{
// Preserve supplied rows and append pairs introduced by default expansion.
// Returns the complete cooked row map; source specification remains unchanged.
nlohmann::json CookVfxGradientDefaults(const nlohmann::json &spec,
    const std::filesystem::path &source_dds, const std::filesystem::path &output_dds);
}
