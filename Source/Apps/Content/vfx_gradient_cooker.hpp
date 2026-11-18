#pragma once
#include <filesystem>
#include <map>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <utility>
#include <cstdint>

namespace hs::content
{
using VfxGradientRows = std::map<std::pair<std::string, std::string>, std::uint32_t>;

VfxGradientRows CookVfxGradientDefaults(const nlohmann::json &spec,
    const std::filesystem::path &staged_dds);
}
