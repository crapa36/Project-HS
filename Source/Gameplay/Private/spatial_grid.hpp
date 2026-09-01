#pragma once

#include <hs/core/types.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace hs::gameplay_detail
{

constexpr int kGridDimension = 30;
constexpr float kGridCellSize = 4.0f;
constexpr std::size_t kGridCellCount = kGridDimension * kGridDimension;
using EnemySpatialGrid = std::array<std::vector<std::size_t>, kGridCellCount>;

inline int SpatialGridCoordinate(float value, float arena_half_extent) noexcept
{
    return std::clamp(static_cast<int>(std::floor(
                          (value + arena_half_extent) / kGridCellSize)),
                      0, kGridDimension - 1);
}

inline void CollectSpatialGridCandidates(const EnemySpatialGrid &grid,
                                         Float2 minimum, Float2 maximum,
                                         float arena_half_extent,
                                         std::vector<std::size_t> &out)
{
    out.clear();
    const auto minimum_x = SpatialGridCoordinate(minimum.x, arena_half_extent);
    const auto maximum_x = SpatialGridCoordinate(maximum.x, arena_half_extent);
    const auto minimum_y = SpatialGridCoordinate(minimum.y, arena_half_extent);
    const auto maximum_y = SpatialGridCoordinate(maximum.y, arena_half_extent);
    for (auto y = minimum_y; y <= maximum_y; ++y)
        for (auto x = minimum_x; x <= maximum_x; ++x)
        {
            const auto &cell = grid[static_cast<std::size_t>(
                y * kGridDimension + x)];
            out.insert(out.end(), cell.begin(), cell.end());
        }
}

} // namespace hs::gameplay_detail
