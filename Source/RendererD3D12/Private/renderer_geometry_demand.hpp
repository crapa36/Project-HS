#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <vector>

namespace hs
{
// Lower bound on the minor singular value of the ground-space pixel Jacobian.
// Degenerate or nonfinite inputs request the finest texture mip.
[[nodiscard]] inline float ConservativeGroundTextureFootprint(double dx_x,double dx_z,double dy_x,double dy_z) noexcept
{
    const double norm=std::hypot(std::hypot(dx_x,dx_z),std::hypot(dy_x,dy_z));
    const double determinant=std::abs(dx_x*dy_z-dx_z*dy_x);
    if (!std::isfinite(norm) || !std::isfinite(determinant) || norm<=0 || determinant<=0) return 0;
    const double lower=determinant/norm;
    if (!std::isfinite(lower) || lower<=0 || lower>std::numeric_limits<float>::max()) return 0;
    return std::nextafter(static_cast<float>(lower),0.0f);
}
struct GeometryDemandPoint
{
    std::array<float, 3> position;
    float scale;
    std::uint32_t instance;
    bool wind;
};
struct GeometryDemandCell
{
    std::array<float, 3> minimum, maximum;
    float max_scale;
    bool any_wind;
    std::vector<GeometryDemandPoint> points;
    bool valid_bounds{};
};
inline std::vector<GeometryDemandCell> BuildGeometryDemandCells(std::span<const GeometryDemandPoint> points)
{
    std::vector<GeometryDemandCell> cells;
    std::map<std::array<double, 3>, std::size_t> indices;
    for (const auto &point : points)
    {
        const bool finite = std::isfinite(point.scale) &&
            std::ranges::all_of(point.position, [](float x) { return std::isfinite(x); });
        if (!finite)
        {
            const auto nan = std::numeric_limits<float>::quiet_NaN();
            cells.push_back({{nan, nan, nan}, {nan, nan, nan}, point.scale, point.wind, {point}});
            continue;
        }
        std::array<double, 3> key{};
        for (std::size_t axis = 0; axis < 3; ++axis) key[axis] = std::floor(double(point.position[axis]) / 16.0);
        const auto [entry, inserted] = indices.emplace(key, cells.size());
        if (inserted) cells.push_back({point.position, point.position, std::abs(point.scale), point.wind, {}});
        auto &cell = cells[entry->second];
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            cell.minimum[axis] = std::min(cell.minimum[axis], point.position[axis]);
            cell.maximum[axis] = std::max(cell.maximum[axis], point.position[axis]);
        }
        cell.max_scale = std::max(cell.max_scale, std::abs(point.scale));
        cell.any_wind |= point.wind;
        cell.points.push_back(point);
    }
    for (auto &cell : cells)
        cell.valid_bounds = std::isfinite(cell.max_scale) &&
            std::ranges::all_of(cell.minimum, [](float x) { return std::isfinite(x); }) &&
            std::ranges::all_of(cell.maximum, [](float x) { return std::isfinite(x); }) &&
            cell.minimum[0] <= cell.maximum[0] && cell.minimum[1] <= cell.maximum[1] &&
            cell.minimum[2] <= cell.maximum[2];
    return cells;
}

enum class GeometryDemandCellVisibility { Outside, Partial, Inside };
struct GeometryDemandView
{
    std::array<std::array<double, 4>, 6> planes{};
    std::array<std::array<double, 4>, 3> axes{};
    float projection{}, near_plane{};
    std::size_t plane_count{};
    bool planes_valid{}, view_valid{};
};
template<class Planes>
GeometryDemandView PrepareGeometryDemandView(const Planes &planes, const float (&view)[4][4],
    float projection, float near_plane)
{
    GeometryDemandView result;
    result.projection = projection;
    result.near_plane = near_plane;
    std::size_t count{};
    result.planes_valid = true;
    for (const auto &plane : planes)
    {
        if (count == result.planes.size()) { result.planes_valid = false; break; }
        auto &p = result.planes[count++];
        p = {plane.x, plane.y, plane.z, plane.w};
        for (const auto value : p) result.planes_valid &= std::isfinite(value);
    }
    result.plane_count = count;
    result.planes_valid &= count != 0;
    result.view_valid = std::isfinite(projection) && projection >= 0 &&
        std::isfinite(near_plane) && near_plane > 0 &&
        view[0][3] == 0 && view[1][3] == 0 && view[2][3] == 0 && view[3][3] == 1;
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        auto &a = result.axes[axis];
        a = {view[0][axis], view[1][axis], view[2][axis], view[3][axis]};
        for (const auto value : a) result.view_valid &= std::isfinite(value);
    }
    return result;
}
namespace geometry_demand_detail
{
// Covers separate float products/additions, FMA ordering and FTZ. Deliberately
// loose: uncertain cells fall back to the original per-instance calculation.
inline double Pad(double magnitude)
{
    if (!std::isfinite(magnitude) || magnitude > double(std::numeric_limits<float>::max()) / 16)
        return std::numeric_limits<double>::infinity();
    return magnitude * (64.0 * std::numeric_limits<float>::epsilon()) +
        64.0 * std::numeric_limits<float>::min();
}
inline bool Interval(const GeometryDemandCell &cell, const std::array<double, 4> &a,
                     double &low, double &high)
{
    low = high = a[3];
    double magnitude = std::abs(a[3]);
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        const double lo = cell.minimum[axis], hi = cell.maximum[axis];
        const double x = lo * a[axis], y = hi * a[axis];
        low += std::min(x, y); high += std::max(x, y);
        magnitude += std::max(std::abs(x), std::abs(y));
    }
    const double pad = Pad(magnitude);
    low -= pad; high += pad;
    return std::isfinite(low) && std::isfinite(high);
}
}
inline GeometryDemandCellVisibility ClassifyGeometryDemandCell(
    const GeometryDemandCell &cell, float maximum_radius, const GeometryDemandView &view)
{
    if (!cell.valid_bounds || !view.planes_valid || !std::isfinite(maximum_radius) || maximum_radius < 0)
        return GeometryDemandCellVisibility::Partial;
    bool inside = true, outside = false;
    for (std::size_t i = 0; i < view.plane_count; ++i)
    {
        double low{}, high{};
        if (!geometry_demand_detail::Interval(cell, view.planes[i], low, high))
            return GeometryDemandCellVisibility::Partial;
        outside |= high < -double(maximum_radius);
        inside &= low >= 0;
    }
    return outside ? GeometryDemandCellVisibility::Outside :
        inside ? GeometryDemandCellVisibility::Inside : GeometryDemandCellVisibility::Partial;
}
inline double GeometryDemandCellErrorUpperBound(const GeometryDemandCell &cell,
    float maximum_radius, float maximum_error, const GeometryDemandView &view)
{
    const double infinity = std::numeric_limits<double>::infinity();
    if (!cell.valid_bounds || !view.view_valid || !std::isfinite(maximum_radius) || maximum_radius < 0 ||
        !std::isfinite(maximum_error) || maximum_error < 0) return infinity;
    double low[3]{}, high[3]{};
    for (std::size_t axis = 0; axis < 3; ++axis)
        if (!geometry_demand_detail::Interval(cell, view.axes[axis], low[axis], high[axis])) return infinity;
    const double extent = double(maximum_radius) + maximum_error;
    const double distance = low[2] - extent - geometry_demand_detail::Pad(std::abs(low[2]) + extent);
    if (distance <= view.near_plane) return infinity;
    const double x = std::max(std::abs(low[0]), std::abs(high[0])) + extent;
    const double y = std::max(std::abs(low[1]), std::abs(high[1])) + extent;
    const double rho = std::hypot(x, y) / distance;
    const double numerator = double(maximum_error) * view.projection * std::sqrt(1 + rho * rho);
    const double result = numerator / distance;
    // Float intermediates may overflow even when the double result is finite.
    if (!std::isfinite(geometry_demand_detail::Pad(x * x + y * y + rho * rho +
        numerator)) || !std::isfinite(result)) return infinity;
    return result + geometry_demand_detail::Pad(result) * 4;
}
} // namespace hs
