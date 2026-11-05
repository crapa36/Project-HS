#pragma once

#include <hs/render_data/scene_probe.hpp>

namespace hs::render_data
{
// Node-major signed radial distances from the offline D3D cube capture.
// Negative samples are opaque exits. The GPU runtime consumes only the masks.
[[nodiscard]] std::vector<SceneProbeCell> BakeSceneProbeCells(const SceneProbeDescription &,
    std::span<const SceneProbeRecord> reflections, std::span<const float> signed_hits);
}
