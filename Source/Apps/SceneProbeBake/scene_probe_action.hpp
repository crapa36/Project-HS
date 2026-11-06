#pragma once

#include <filesystem>

namespace hs { struct SelectedGeneration; }

namespace hs::scene_probe_bake
{
void BakeSceneProbe(const hs::SelectedGeneration &, const std::filesystem::path &output,
    const std::filesystem::path &artifacts);
int ProduceSceneProbe(int argc, char **argv);
}
