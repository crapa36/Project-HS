#pragma once
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include "vfx_gradient_cooker.hpp"

namespace hs::content
{
void CookVfxProgram(const nlohmann::json &spec,
    const VfxGradientRows &gradient_rows, const std::filesystem::path &output_file);
}
