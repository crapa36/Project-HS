#pragma once

#include <hs/game_domain/arena_boundary.hpp>
#include <hs/game_domain/arena_obstacle.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <span>
#include <utility>
#include <vector>

namespace hs::gameplay_detail
{
inline constexpr int kNavigationGridDimension = 121;
inline constexpr int kNavigationGridCells = kNavigationGridDimension * kNavigationGridDimension;
inline constexpr int kUnreachableNavigationDistance = std::numeric_limits<int>::max();

inline float NavigationDistanceSquared(Float2 a, Float2 b) noexcept
{
    const auto x = a.x - b.x, y = a.y - b.y;
    return x * x + y * y;
}

struct NavigationGrid
{
    std::array<std::uint8_t, kNavigationGridCells> blocked{};
    std::array<int, kNavigationGridCells> distance{};
    Float2 origin{-60.5f, -60.5f};
    float cell_size{1.0f};
    int width{kNavigationGridDimension};
    int height{kNavigationGridDimension};

    int Index(int x, int y) const noexcept { return y * width + x; }
    bool In(int x, int y) const noexcept { return x >= 0 && y >= 0 && x < width && y < height; }
    Float2 Center(int x, int y) const noexcept
    {
        return {origin.x + (static_cast<float>(x) + 0.5f) * cell_size,
                origin.y + (static_cast<float>(y) + 0.5f) * cell_size};
    }
    std::pair<int, int> Cell(Float2 p) const noexcept
    {
        return {static_cast<int>(std::floor((p.x - origin.x) / cell_size)),
                static_cast<int>(std::floor((p.y - origin.y) / cell_size))};
    }
    void Rebuild(const ArenaBoundary &boundary, std::span<const ArenaObstacle2D> obstacles,
                 float clearance = 0.0f) noexcept
    {
        distance.fill(kUnreachableNavigationDistance);
        const auto cell_padding = cell_size * 0.70710678f;
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
        {
            const auto point = Center(x, y);
            auto clear = ContainsArenaPoint(boundary, point, clearance + cell_padding);
            for (const auto &obstacle : obstacles)
            {
                const auto radius = clearance + cell_padding + obstacle.radius;
                if (NavigationDistanceSquared(point, obstacle.center) < radius * radius) clear = false;
            }
            blocked[Index(x, y)] = clear ? 0 : 1;
        }
    }
    int NearestClearCell(Float2 point) const noexcept
    {
        const auto [x, y] = Cell(point);
        if (In(x, y) && !blocked[Index(x, y)]) return Index(x, y);
        auto best = -1;
        auto best_distance = std::numeric_limits<float>::max();
        for (int index = 0; index < width * height; ++index)
        {
            if (blocked[index]) continue;
            const auto candidate = NavigationDistanceSquared(Center(index % width, index / width), point);
            if (candidate < best_distance || (candidate == best_distance && index < best))
            {
                best = index;
                best_distance = candidate;
            }
        }
        return best;
    }
    void RebuildFlow(Float2 target) noexcept
    {
        distance.fill(kUnreachableNavigationDistance);
        const auto target_index = NearestClearCell(target);
        if (target_index < 0) return;
        using Entry = std::pair<int, int>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<>> pending;
        distance[target_index] = 0;
        pending.emplace(0, target_index);
        while (!pending.empty())
        {
            const auto [cost, current] = pending.top(); pending.pop();
            if (cost != distance[current]) continue;
            const auto x = current % width, y = current / width;
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
            {
                if (dx == 0 && dy == 0) continue;
                const auto nx = x + dx, ny = y + dy;
                if (!In(nx, ny)) continue;
                const auto next = Index(nx, ny);
                if (blocked[next] || (dx != 0 && dy != 0 &&
                    (blocked[Index(x + dx, y)] || blocked[Index(x, y + dy)]))) continue;
                const auto next_cost = cost + (dx != 0 && dy != 0 ? 14 : 10);
                if (next_cost >= distance[next]) continue;
                distance[next] = next_cost;
                pending.emplace(next_cost, next);
            }
        }
    }
    Float2 NextWaypoint(Float2 from, Float2 fallback) const noexcept
    {
        const auto [x, y] = Cell(from);
        if (!In(x, y)) return fallback;
        auto best = Index(x, y);
        if (distance[best] == kUnreachableNavigationDistance) return fallback;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
        {
            if (dx == 0 && dy == 0) continue;
            const auto nx = x + dx, ny = y + dy;
            if (!In(nx, ny)) continue;
            const auto next = Index(nx, ny);
            if (blocked[next] || (dx != 0 && dy != 0 &&
                (blocked[Index(x + dx, y)] || blocked[Index(x, y + dy)]))) continue;
            if (distance[next] < distance[best] || (distance[next] == distance[best] && next < best)) best = next;
        }
        return best == Index(x, y) ? fallback : Center(best % width, best / width);
    }
};

// Earliest normalized sweep contact; 2 means no contact on this segment.
inline float SegmentDiscHitTime(Float2 from, Float2 to, Float2 center, float radius) noexcept
{
    const float dx = to.x - from.x, dy = to.y - from.y;
    const float ox = from.x - center.x, oy = from.y - center.y;
    const float c = ox * ox + oy * oy - radius * radius;
    if (c <= 0.0f) return 0.0f;
    const float a = dx * dx + dy * dy;
    if (a <= 0.0f) return 2.0f;
    const float b = ox * dx + oy * dy;
    const float discriminant = b * b - a * c;
    if (b >= 0.0f || discriminant < 0.0f) return 2.0f;
    const float t = (-b - std::sqrt(discriminant)) / a;
    return t >= 0.0f && t <= 1.0f ? t : 2.0f;
}

inline float FirstObstacleHitTime(Float2 from, Float2 to, float radius,
                                  std::span<const ArenaObstacle2D> obstacles) noexcept
{
    float first = 2.0f;
    for (const auto &obstacle : obstacles)
        first = std::min(first, SegmentDiscHitTime(from, to, obstacle.center,
                                                 radius + obstacle.radius));
    return first;
}

inline bool SegmentClear2D(Float2 from, Float2 to, float radius,
                           const ArenaBoundary &boundary,
                           std::span<const ArenaObstacle2D> obstacles) noexcept
{
    if (!ContainsArenaPoint(boundary, from, radius) || !ContainsArenaPoint(boundary, to, radius)) return false;
    const Float2 segment{to.x - from.x, to.y - from.y};
    const auto length_squared = segment.x * segment.x + segment.y * segment.y;
    for (const auto &obstacle : obstacles)
    {
        const Float2 offset{obstacle.center.x - from.x, obstacle.center.y - from.y};
        const auto t = length_squared > 0.0f ? std::clamp((offset.x * segment.x + offset.y * segment.y) / length_squared, 0.0f, 1.0f) : 0.0f;
        const Float2 nearest{from.x + segment.x * t, from.y + segment.y * t};
        const auto combined = radius + obstacle.radius;
        if (NavigationDistanceSquared(nearest, obstacle.center) < combined * combined) return false;
    }
    return true;
}

inline Float2 ProjectOutsideObstacles(Float2 point, float radius,
                                      std::span<const ArenaObstacle2D> obstacles) noexcept
{
    for (int pass = 0; pass < 3; ++pass)
    {
        auto moved = false;
        for (const auto &obstacle : obstacles)
        {
            auto x = point.x - obstacle.center.x, y = point.y - obstacle.center.y;
            const auto combined = radius + obstacle.radius;
            const auto squared = x * x + y * y;
            if (squared >= combined * combined) continue;
            auto distance = std::sqrt(squared);
            if (distance < 1.0e-5f) { x = 1.0f; y = 0.0f; distance = 1.0f; }
            point = {obstacle.center.x + x / distance * combined,
                     obstacle.center.y + y / distance * combined};
            moved = true;
        }
        if (!moved) break;
    }
    return point;
}

inline Float2 MoveDisc2D(Float2 from, Float2 delta, float radius,
                         const ArenaBoundary &boundary,
                         std::span<const ArenaObstacle2D> obstacles) noexcept
{
    auto position = ClampToArena(boundary, ProjectOutsideObstacles(from, radius, obstacles), radius);
    auto remaining = delta;
    for (int iteration = 0; iteration < 3; ++iteration)
    {
        auto hit_time = 1.0f;
        const ArenaObstacle2D *hit = nullptr;
        for (const auto &obstacle : obstacles)
        {
            const Float2 offset{position.x - obstacle.center.x, position.y - obstacle.center.y};
            const auto combined = radius + obstacle.radius;
            const auto a = remaining.x * remaining.x + remaining.y * remaining.y;
            const auto b = 2.0f * (offset.x * remaining.x + offset.y * remaining.y);
            const auto c = offset.x * offset.x + offset.y * offset.y - combined * combined;
            const auto discriminant = b * b - 4.0f * a * c;
            if (a <= 1.0e-8f || discriminant < 0.0f) continue;
            const auto candidate = (-b - std::sqrt(discriminant)) / (2.0f * a);
            const auto starts_on_surface_moving_inward =
                c <= 1.0e-4f && offset.x * remaining.x + offset.y * remaining.y < 0.0f;
            if (((candidate > 1.0e-5f) || starts_on_surface_moving_inward) &&
                candidate < hit_time)
            {
                hit_time = std::max(0.0f, candidate);
                hit = &obstacle;
            }
        }
        if (!hit) { position = {position.x + remaining.x, position.y + remaining.y}; break; }
        const auto safe_time = std::max(0.0f, hit_time - 1.0e-4f);
        position = {position.x + remaining.x * safe_time, position.y + remaining.y * safe_time};
        remaining = {remaining.x * (1.0f - hit_time), remaining.y * (1.0f - hit_time)};
        const auto normal_length = std::sqrt(NavigationDistanceSquared(position, hit->center));
        if (normal_length <= 1.0e-6f) break;
        const Float2 normal{(position.x - hit->center.x) / normal_length,
                            (position.y - hit->center.y) / normal_length};
        const auto into_surface = remaining.x * normal.x + remaining.y * normal.y;
        if (into_surface < 0.0f) { remaining.x -= normal.x * into_surface; remaining.y -= normal.y * into_surface; }
    }
    return ProjectOutsideObstacles(ClampToArena(boundary, position, radius), radius, obstacles);
}
} // namespace hs::gameplay_detail
