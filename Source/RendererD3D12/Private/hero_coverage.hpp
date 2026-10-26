#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace hs::renderer_detail
{
struct HeroCoverageProjection
{
    std::uint32_t width{}, height{};
    double scale_x{}, scale_y{}, jitter_x{}, jitter_y{}, near_plane{}, far_plane{};
};
struct HeroProjectedBounds
{
    enum class Kind { Unknown, Outside, Visible };
    Kind kind{Kind::Unknown};
    std::int32_t left{}, top{}, right{}, bottom{};
};

// The caller supplies a current conservative view-space sphere, including deformation.
inline HeroProjectedBounds ProjectHeroSphere(const HeroCoverageProjection &view,
    double x, double y, double z, double radius)
{
    if (!view.width || !view.height || view.width > INT32_MAX || view.height > INT32_MAX ||
        !std::isfinite(view.scale_x) || !std::isfinite(view.scale_y) ||
        view.scale_x <= 0 || view.scale_y <= 0 ||
        !std::isfinite(view.jitter_x) || !std::isfinite(view.jitter_y) ||
        !std::isfinite(view.near_plane) || !std::isfinite(view.far_plane) ||
        view.near_plane <= 0 || view.far_plane <= view.near_plane ||
        !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        !std::isfinite(radius) || radius <= 0)
        return {};
    if (z + radius < view.near_plane || z - radius > view.far_plane)
        return {HeroProjectedBounds::Kind::Outside};
    if (z - radius <= view.near_plane || z + radius >= view.far_plane)
        return {};
    const auto project_axis = [&](double center, double scale, double jitter) {
        const std::array ratios{(center - radius) / (z - radius),
            (center - radius) / (z + radius), (center + radius) / (z - radius),
            (center + radius) / (z + radius)};
        const auto [low, high] = std::minmax_element(ratios.begin(), ratios.end());
        return std::pair{*low * scale + jitter, *high * scale + jitter};
    };
    const auto [left, right] = project_axis(x, view.scale_x, view.jitter_x);
    const auto [bottom, top] = project_axis(y, view.scale_y, view.jitter_y);
    if (!std::isfinite(left) || !std::isfinite(right) ||
        !std::isfinite(top) || !std::isfinite(bottom)) return {};
    const double pixel_left = (left + 1.0) * view.width * 0.5 - 2.0;
    const double pixel_right = (right + 1.0) * view.width * 0.5 + 2.0;
    const double pixel_top = (1.0 - top) * view.height * 0.5 - 2.0;
    const double pixel_bottom = (1.0 - bottom) * view.height * 0.5 + 2.0;
    if (pixel_right < 0 || pixel_bottom < 0 ||
        pixel_left > view.width || pixel_top > view.height)
        return {HeroProjectedBounds::Kind::Outside};
    return {HeroProjectedBounds::Kind::Visible,
        static_cast<std::int32_t>(std::floor(std::clamp(pixel_left, 0.0, double(view.width)))),
        static_cast<std::int32_t>(std::floor(std::clamp(pixel_top, 0.0, double(view.height)))),
        static_cast<std::int32_t>(std::ceil(std::clamp(pixel_right, 0.0, double(view.width)))),
        static_cast<std::int32_t>(std::ceil(std::clamp(pixel_bottom, 0.0, double(view.height))))};
}
} // namespace hs::renderer_detail
