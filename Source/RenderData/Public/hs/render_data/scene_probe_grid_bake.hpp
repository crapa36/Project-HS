#pragma once

#include <hs/render_data/scene_probe.hpp>

namespace hs::render_data
{
// Node-major signed radial distances from the offline D3D cube capture.
// Negative samples are opaque exits. The GPU runtime consumes only the masks.
[[nodiscard]] std::vector<SceneProbeCell> BakeSceneProbeCells(const SceneProbeDescription &,
    std::span<const SceneProbeRecord> reflections, std::span<const float> signed_hits);

enum class SceneProbeIterationState { Invalid, Unconverged, Converged };
// Inputs are node-major cosine-convolved SH9. Signed higher bands are valid;
// L0 is a nonnegative integral of captured scene-linear incident radiance.
[[nodiscard]] SceneProbeIterationState CompareSceneProbeIterations(
    std::span<const Float4> previous, std::span<const Float4> next,
    double absolute_tolerance, double relative_tolerance) noexcept;
}
