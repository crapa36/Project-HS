#pragma once

#include <filesystem>
#include <cstdint>

namespace hs { struct SelectedGeneration; }

namespace hs::scene_probe_bake
{
void BakeSceneProbe(const hs::SelectedGeneration &, const std::filesystem::path &output,
    const std::filesystem::path &artifacts, std::uint32_t maximum_cube_views,
    std::uint32_t maximum_iterations, double absolute_tolerance, double relative_tolerance);
int ProduceSceneProbe(int argc, char **argv);
}
