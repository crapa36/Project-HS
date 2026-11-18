#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace hs::renderer_detail
{
struct TranslucencyCoverageProjection
{
    std::uint32_t width{}, height{};
    double scale_x{}, scale_y{}, jitter_x{}, jitter_y{}, near_plane{}, far_plane{};

    bool Valid() const
    {
        return width && height && width <= INT32_MAX && height <= INT32_MAX &&
            std::isfinite(scale_x) && scale_x > 0 && std::isfinite(scale_y) && scale_y > 0 &&
            std::isfinite(jitter_x) && std::isfinite(jitter_y) &&
            std::isfinite(near_plane) && near_plane > 0 && std::isfinite(far_plane) && far_plane > near_plane;
    }
};
struct TranslucencyProjectedBounds
{
    enum class Kind { Unknown, Outside, Visible };
    Kind kind{Kind::Unknown};
    std::int32_t left{}, top{}, right{}, bottom{};
};

// The caller supplies a current conservative view-space sphere, including deformation.
inline TranslucencyProjectedBounds ProjectTranslucencySphere(const TranslucencyCoverageProjection &view,
    double x, double y, double z, double radius)
{
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        !std::isfinite(radius) || radius <= 0)
        return {};
    if (z + radius < view.near_plane || z - radius > view.far_plane)
        return {TranslucencyProjectedBounds::Kind::Outside};
    if (z - radius <= view.near_plane || z + radius >= view.far_plane)
        return {};
    const double depth_product = (z - radius) * (z + radius);
    const double inverse_depth_product = 1.0 / depth_product;
    const auto project_axis = [&](double center, double scale, double jitter) {
        // The two tangent slopes solve (center - slope*z)^2 = radius^2 * (1 + slope^2).
        // Recover the smaller root from their product to avoid cancellation near an axis.
        const double magnitude = std::abs(center);
        const double q = magnitude * z + radius * std::sqrt(magnitude * magnitude + depth_product);
        const double outer = q * inverse_depth_product;
        const double inner = (magnitude - radius) * (magnitude + radius) / q;
        const auto [low, high] = center < 0.0 ? std::pair{-outer, -inner} : std::pair{inner, outer};
        return std::pair{low * scale + jitter, high * scale + jitter};
    };
    const auto [left, right] = project_axis(x, view.scale_x, view.jitter_x);
    const auto [bottom, top] = project_axis(y, view.scale_y, view.jitter_y);
    if (!std::isfinite(left) || !std::isfinite(right) ||
        !std::isfinite(top) || !std::isfinite(bottom)) return {};
    const double pixel_left = (left + 1.0) * view.width * 0.5 - 2.0;
    const double pixel_right = (right + 1.0) * view.width * 0.5 + 2.0;
    const double pixel_top = (1.0 - top) * view.height * 0.5 - 2.0;
    const double pixel_bottom = (1.0 - bottom) * view.height * 0.5 + 2.0;
    if (pixel_right <= 0 || pixel_bottom <= 0 ||
        pixel_left >= view.width || pixel_top >= view.height)
        return {TranslucencyProjectedBounds::Kind::Outside};
    return {TranslucencyProjectedBounds::Kind::Visible,
        static_cast<std::int32_t>(std::floor(std::clamp(pixel_left, 0.0, double(view.width)))),
        static_cast<std::int32_t>(std::floor(std::clamp(pixel_top, 0.0, double(view.height)))),
        static_cast<std::int32_t>(std::ceil(std::clamp(pixel_right, 0.0, double(view.width)))),
        static_cast<std::int32_t>(std::ceil(std::clamp(pixel_bottom, 0.0, double(view.height))))};
}
} // namespace hs::renderer_detail
