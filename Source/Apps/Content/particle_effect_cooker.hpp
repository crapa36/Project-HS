#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace hs::content
{

void WriteCookedParticleEffects(const std::filesystem::path &path, const nlohmann::json &document);
bool WriteVfxMaskArray(const std::filesystem::path &path, std::span<const std::string> filenames,
                       const std::map<std::string, std::string> &png_bytes, std::string &error_message);

} // namespace hs::content
