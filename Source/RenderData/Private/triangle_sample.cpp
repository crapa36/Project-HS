#include <hs/render_data/triangle_sample.hpp>

#include <algorithm>
#include <cmath>

namespace hs::render_data
{
namespace
{
using Vec3 = std::array<double, 3>;

Vec3 Cross(const Vec3 &a, const Vec3 &b)
{
    return {a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
}

bool Finite(const Vec3 &v)
{
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

Vec3 Scale(const Vec3 &v, double factor)
{
    return {v[0] * factor, v[1] * factor, v[2] * factor};
}
} // namespace

TriangleSample SampleTriangleAtNdc(
    const std::array<TriangleClipVertex, 3> &vertices,
    double ndc_x, double ndc_y, double viewport_width,
    double viewport_height)
{
    TriangleSample result;
    if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y) ||
        !std::isfinite(viewport_width) || !std::isfinite(viewport_height) ||
        viewport_width <= 0.0 || viewport_height <= 0.0)
        return result;

    double common_scale = 0.0;
    for (const auto &vertex : vertices)
    {
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) ||
            !std::isfinite(vertex.z) || !std::isfinite(vertex.w))
            return result;
        common_scale = std::max(common_scale, std::abs(vertex.x));
        common_scale = std::max(common_scale, std::abs(vertex.y));
        common_scale = std::max(common_scale, std::abs(vertex.z));
        common_scale = std::max(common_scale, std::abs(vertex.w));
    }
    if (!std::isfinite(common_scale) || common_scale == 0.0)
        return result;

    // Keep the homogeneous rows near unit scale.  A common scale cancels in
    // the barycentric solve.  Its w factor is retained below when reporting
    // clip_w in the caller's original coordinate scale.
    std::array<TriangleClipVertex, 3> normalized = vertices;
    for (auto &vertex : normalized)
    {
        vertex.x /= common_scale;
        vertex.y /= common_scale;
        vertex.z /= common_scale;
        vertex.w /= common_scale;
    }

    const Vec3 one{1.0, 1.0, 1.0};
    const Vec3 v{
        normalized[0].x - ndc_x * normalized[0].w,
        normalized[1].x - ndc_x * normalized[1].w,
        normalized[2].x - ndc_x * normalized[2].w};
    const Vec3 u{
        normalized[0].y - ndc_y * normalized[0].w,
        normalized[1].y - ndc_y * normalized[1].w,
        normalized[2].y - ndc_y * normalized[2].w};
    if (!Finite(v) || !Finite(u))
        return result;

    // A has rows v, u, one.  Since A*b=(0,0,1), b is the cross(v,u) column
    // divided by det(A).  Differentiating that equation gives
    // A*db/dx=(W,0,0) and A*db/dy=(0,W,0), where W=dot(b,w).  Thus the
    // first two inverse columns are cross(u,one)/det and cross(one,v)/det.
    const Vec3 k = Cross(v, u);
    const double determinant = k[0] + k[1] + k[2];
    if (!std::isfinite(determinant) || determinant == 0.0 || !Finite(k))
        return result;

    const Vec3 barycentric = Scale(k, 1.0 / determinant);
    if (!Finite(barycentric))
        return result;

    const Vec3 normalized_w{
        normalized[0].w, normalized[1].w, normalized[2].w};
    const Vec3 normalized_z{
        normalized[0].z, normalized[1].z, normalized[2].z};
    const double sample_w_normalized =
        barycentric[0] * normalized_w[0] +
        barycentric[1] * normalized_w[1] +
        barycentric[2] * normalized_w[2];
    if (!std::isfinite(sample_w_normalized) || sample_w_normalized <= 0.0)
        return result;

    const double sample_w = sample_w_normalized * common_scale;
    if (!std::isfinite(sample_w) || sample_w <= 0.0)
        return result;

    const Vec3 d_ndc_x = Scale(Cross(u, one), sample_w_normalized /
                                                 determinant);
    const Vec3 d_ndc_y = Scale(Cross(one, v), sample_w_normalized /
                                                 determinant);
    const Vec3 d_pixel_x = Scale(d_ndc_x, 2.0 / viewport_width);
    const Vec3 d_pixel_y = Scale(d_ndc_y, -2.0 / viewport_height);
    if (!Finite(d_ndc_x) || !Finite(d_ndc_y) || !Finite(d_pixel_x) ||
        !Finite(d_pixel_y))
        return result;

    // Use the normalized pair for depth too; the common homogeneous scale
    // cancels and this avoids reintroducing overflow in the z numerator.
    const double ndc_depth =
        (barycentric[0] * normalized_z[0] +
         barycentric[1] * normalized_z[1] +
         barycentric[2] * normalized_z[2]) /
        sample_w_normalized;
    if (!std::isfinite(ndc_depth))
        return result;

    result.valid = true;
    result.barycentric = barycentric;
    result.derivative_x = d_pixel_x;
    result.derivative_y = d_pixel_y;
    result.clip_w = sample_w;
    result.ndc_depth = ndc_depth;
    return result;
}
} // namespace hs::render_data
