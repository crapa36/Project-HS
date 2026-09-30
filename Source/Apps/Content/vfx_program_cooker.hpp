#pragma once
#include <hs/renderer/vfx_program.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>

namespace hs::content
{
// Requires validated v4 spec. Returned JSON is tooling/debug information only.
// Numeric registry mappings and unresolved authored prose are explicit in this manifest.
nlohmann::json CookVfxProgram(const nlohmann::json &spec,
    const nlohmann::json &gradient_rows, const std::filesystem::path &output_file);
}
