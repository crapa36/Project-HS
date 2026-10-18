#pragma once

#include <hs/core/render_snapshot.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

namespace hs
{
struct LocalProbeDrawSphere
{
    std::array<double, 3> center{};
    double radius{std::numeric_limits<double>::infinity()};
};
struct LocalProbeDraw
{
    std::uint32_t source_index{}, material_ordinal{};
    LocalProbeDrawSphere bounds;
    std::uint32_t page_ordinal{UINT32_MAX};
};
struct LocalProbeDrawRun
{
    std::uint32_t first_instance{}, instance_count{}, material_ordinal{};
    std::uint32_t page_ordinal{UINT32_MAX};
};
// Four ordinal bits leave 26 source bits while keeping both shadow-record bits clear.
constexpr std::uint32_t kLocalProbeSourceCapacity=1u<<26u;
[[nodiscard]] inline bool LocalProbeDrawRunValid(const LocalProbeDrawRun &run, std::uint64_t source_count)
{
    return source_count<=kLocalProbeSourceCapacity && run.material_ordinal<16u &&
        run.instance_count && run.first_instance<source_count &&
        run.instance_count<=source_count-run.first_instance;
}
[[nodiscard]] inline std::uint32_t LocalProbePassValue(const LocalProbeDrawRun &run)
{ return (run.first_instance<<4u)|run.material_ordinal; }

struct LocalProbeFrustum
{
    std::array<std::array<double, 4>, 6> planes{};
    bool valid{};
};

// Exact complete-page sphere, expanded about the source origin for every yaw.
// Capture banks use absolute static slots, time zero and no grass benders.
[[nodiscard]] inline LocalProbeDrawSphere LocalProbeWorldSphere(
    const RenderInstance &source, const std::array<float, 4> &local)
{
    LocalProbeDrawSphere result;
    const bool environment = (source.mesh >= RenderMesh::TreeTrunk && source.mesh <= RenderMesh::Grass) || source.mesh == RenderMesh::GroundDetail || source.mesh == RenderMesh::MeadowSurface;
    const bool terrain = source.mesh == RenderMesh::Ground || source.mesh == RenderMesh::DirtPatch;
    if ((!environment && !terrain) || !std::isfinite(source.yaw) || local[3] < 0)
        return result;
    const std::array<double, 3> position{source.position.x, source.position.y, source.position.z};
    const std::array<double, 3> scale{source.scale.x, source.scale.y, source.scale.z};
    for (std::size_t axis = 0; axis < 3; ++axis)
        if (!std::isfinite(position[axis]) || !std::isfinite(scale[axis]) || !std::isfinite(local[axis]))
            return result;
    if (!std::isfinite(local[3])) return result;
    result.center = position;
    // Match the persistent static GPU slot's terrain vertical offset.
    if (terrain) result.center[1] += scale[1] * 0.5;
    const double maximum_scale = std::max({std::abs(scale[0]), std::abs(scale[1]), std::abs(scale[2])});
    double radius = std::hypot(double(local[0]) * scale[0], double(local[1]) * scale[1],
                              double(local[2]) * scale[2]) + double(local[3]) * maximum_scale;
    // BendGrass's fixed world-space wind is bounded even at capture time zero.
    if (HasEnvironmentWind(source.mesh, source.environment_variant)) radius += std::hypot(0.035, 0.035 * 0.7);
    const double magnitude = std::max({1.0, std::abs(result.center[0]), std::abs(result.center[1]),
                                       std::abs(result.center[2]), radius});
    // Cover CPU/GPU float transform and clip-plane rounding at raster edges.
    radius += magnitude * std::numeric_limits<float>::epsilon() * 64.0;
    if (!std::isfinite(radius) || magnitude > std::numeric_limits<float>::max()) return {};
    for (double center : result.center)
        if (std::abs(center) + radius > std::numeric_limits<float>::max()) return {};
    result.radius = radius;
    return result;
}

[[nodiscard]] inline LocalProbeFrustum MakeLocalProbeFrustum(const std::array<float, 16> &matrix)
{
    LocalProbeFrustum result;
    for (float value : matrix) if (!std::isfinite(value)) return result;
    for (std::size_t plane = 0; plane < result.planes.size(); ++plane)
    {
        const std::size_t axis = plane / 2;
        const double sign = plane % 2 == 0 ? 1.0 : -1.0;
        const double w = plane == 4 ? 0.0 : 1.0;
        // Row-vector D3D clip inequalities: W +/- X/Y, Z and W-Z.
        for (std::size_t row = 0; row < 4; ++row)
            result.planes[plane][row] = sign * matrix[row * 4 + axis] + w * matrix[row * 4 + 3];
        auto &p = result.planes[plane];
        const double length = std::hypot(p[0], p[1], p[2]);
        if (!std::isfinite(length) || length <= 0.0) return {};
        for (double &value : p) value /= length;
    }
    result.valid = true;
    return result;
}

[[nodiscard]] inline bool LocalProbeIntersects(const LocalProbeDrawSphere &sphere,
                                               const LocalProbeFrustum &frustum)
{
    if (!frustum.valid || !std::isfinite(sphere.radius) || sphere.radius < 0) return true;
    for (double value : sphere.center) if (!std::isfinite(value)) return true;
    for (const auto &p : frustum.planes)
        if (sphere.center[0] * p[0] + sphere.center[1] * p[1] +
            sphere.center[2] * p[2] + p[3] < -sphere.radius)
            return false;
    return true;
}

// Keep canonical source/primitive order, including density and culling holes.
// nullptr is the full-scene shadow path: it never rejects a retained draw.
template <class Emit>
inline void ForEachLocalProbeDrawRun(std::span<const LocalProbeDraw> draws,
                                    const LocalProbeFrustum *frustum, Emit &&emit)
{
    LocalProbeDrawRun run{};
    const auto flush = [&] { if (run.instance_count) emit(run); run.instance_count = 0; };
    for (const auto &draw : draws)
    {
        if (frustum && !LocalProbeIntersects(draw.bounds, *frustum)) { flush(); continue; }
        if (run.instance_count && run.material_ordinal == draw.material_ordinal && run.page_ordinal == draw.page_ordinal &&
            std::uint64_t(run.first_instance) + run.instance_count == draw.source_index)
            ++run.instance_count;
        else
        {
            flush();
            run = {draw.source_index, 1, draw.material_ordinal, draw.page_ordinal};
        }
    }
    flush();
}
} // namespace hs
