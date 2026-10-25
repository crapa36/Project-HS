#pragma once

#include <hs/core/types.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hs::gameplay_detail
{

constexpr int kGridDimension = 30;
constexpr float kGridCellSize = 4.0f;
constexpr std::size_t kGridCellCount = kGridDimension * kGridDimension;
struct EnemySpatialGrid
{
    struct Entry
    {
        std::size_t actor;
        int minimum_x, maximum_x, minimum_y, maximum_y;
    };
    std::array<std::size_t, kGridCellCount + 1> offsets{};
    std::vector<Entry> entries;
    std::vector<std::size_t> rows;

    void BeginBuild()
    {
        offsets.fill(0);
        entries.clear();
    }
    void AddRange(std::size_t actor, int minimum_x, int maximum_x,
                  int minimum_y, int maximum_y)
    {
        entries.push_back({actor, minimum_x, maximum_x, minimum_y, maximum_y});
        for (auto y = minimum_y; y <= maximum_y; ++y)
            for (auto x = minimum_x; x <= maximum_x; ++x)
                ++offsets[static_cast<std::size_t>(y * kGridDimension + x) + 1];
    }
    void FinishBuild()
    {
        for (std::size_t cell = 1; cell < offsets.size(); ++cell)
            offsets[cell] += offsets[cell - 1];
        rows.resize(offsets.back());
        auto cursor = offsets;
        // Scatter the prepared ranges in actor order, including swept-cell repeats.
        for (const auto &entry : entries)
            for (auto y = entry.minimum_y; y <= entry.maximum_y; ++y)
                for (auto x = entry.minimum_x; x <= entry.maximum_x; ++x)
                    rows[cursor[static_cast<std::size_t>(y * kGridDimension + x)]++] = entry.actor;
    }
    std::span<const std::size_t> operator[](std::size_t cell) const noexcept
    {
        return std::span(rows).subspan(offsets[cell], offsets[cell + 1] - offsets[cell]);
    }
};

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

// Each query owns its marks and output, so nested skill queries cannot overwrite it.
struct EnemyQuery
{
    std::vector<std::size_t> rows;
    std::vector<std::uint8_t> visited;

    void Collect(const EnemySpatialGrid &grid, Float2 minimum, Float2 maximum,
                 float arena_half_extent, std::size_t actor_count)
    {
        for (const auto row : rows) visited[row] = 0;
        rows.clear();
        visited.resize(actor_count, 0);
        const auto x0 = SpatialGridCoordinate(minimum.x, arena_half_extent);
        const auto x1 = SpatialGridCoordinate(maximum.x, arena_half_extent);
        const auto y0 = SpatialGridCoordinate(minimum.y, arena_half_extent);
        const auto y1 = SpatialGridCoordinate(maximum.y, arena_half_extent);
        for (auto y = y0; y <= y1; ++y)
            for (auto x = x0; x <= x1; ++x)
                for (const auto row : grid[static_cast<std::size_t>(y * kGridDimension + x)])
                    if (!visited[row]) { visited[row] = 1; rows.push_back(row); }
        std::ranges::sort(rows);
    }
};

} // namespace hs::gameplay_detail
