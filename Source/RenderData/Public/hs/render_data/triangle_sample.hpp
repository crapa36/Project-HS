#pragma once

#include <array>

namespace hs::render_data
{
// A triangle vertex in the original homogeneous clip space.  The w component
// is intentionally allowed to be zero or negative: callers may pass a
// primitive before near-plane clipping.
struct TriangleClipVertex
{
    double x{};
    double y{};
    double z{};
    double w{};
};

struct TriangleSample
{
    bool valid{};
    std::array<double, 3> barycentric{};
    // Derivatives are with respect to top-left raster pixel coordinates.
    std::array<double, 3> derivative_x{};
    std::array<double, 3> derivative_y{};
    // clip_w is the interpolated homogeneous w in the input coordinate scale.
    double clip_w{};
    double ndc_depth{};
};

// Sample a triangle at an NDC target.  viewport_width and viewport_height are
// the positive pixel extents used to convert NDC derivatives to raster
// derivatives.  An invalid result is returned for nonfinite inputs, a
// degenerate triangle, or a sample whose interpolated clip w is not positive.
[[nodiscard]] TriangleSample SampleTriangleAtNdc(
    const std::array<TriangleClipVertex, 3> &vertices,
    double ndc_x, double ndc_y, double viewport_width,
    double viewport_height);
} // namespace hs::render_data
