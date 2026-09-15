#pragma once

#include <hs/core/types.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace hs
{

inline constexpr std::size_t kArenaBoundaryMaxPoints = 24;

struct ArenaBoundary
{
    std::uint8_t count{};
    std::array<Float2, kArenaBoundaryMaxPoints> points{};
};

[[nodiscard]] inline bool ContainsArenaPoint(const ArenaBoundary &boundary,
                                              Float2 point, float inset = 0.0f) noexcept
{
    if (boundary.count < 3) return false;
    for (std::size_t i = 0; i < boundary.count; ++i)
    {
        const Float2 a = boundary.points[i];
        const Float2 b = boundary.points[(i + 1) % boundary.count];
        const float cross = (b.x - a.x) * (point.y - a.y) -
                            (b.y - a.y) * (point.x - a.x);
        const float edge = std::max(0.0f, inset) *
                           std::sqrt((b.x - a.x) * (b.x - a.x) +
                                     (b.y - a.y) * (b.y - a.y));
        if (cross > -edge) return false;
    }
    return true;
}

[[nodiscard]] inline Float2 ClampToArena(const ArenaBoundary &boundary,
                                          Float2 point, float inset = 0.0f) noexcept
{
    if (boundary.count < 3) return point;
    if (ContainsArenaPoint(boundary, point, inset)) return point;
    std::array<Float2, kArenaBoundaryMaxPoints> vertices{};
    const auto offset = std::max(0.0f, inset);
    for (std::size_t i = 0; i < boundary.count; ++i)
    {
        const auto previous = boundary.points[(i + boundary.count - 1) % boundary.count];
        const auto current = boundary.points[i];
        const auto next = boundary.points[(i + 1) % boundary.count];
        const Float2 first_direction{current.x - previous.x, current.y - previous.y};
        const Float2 second_direction{next.x - current.x, next.y - current.y};
        const auto first_length = std::sqrt(first_direction.x * first_direction.x +
                                            first_direction.y * first_direction.y);
        const auto second_length = std::sqrt(second_direction.x * second_direction.x +
                                             second_direction.y * second_direction.y);
        const Float2 first_origin{previous.x + first_direction.y / first_length * offset,
                                  previous.y - first_direction.x / first_length * offset};
        const Float2 second_origin{current.x + second_direction.y / second_length * offset,
                                   current.y - second_direction.x / second_length * offset};
        const auto denominator = first_direction.x * second_direction.y -
                                 first_direction.y * second_direction.x;
        const Float2 origins{second_origin.x - first_origin.x,
                             second_origin.y - first_origin.y};
        const auto t = (origins.x * second_direction.y -
                        origins.y * second_direction.x) / denominator;
        vertices[i] = {first_origin.x + first_direction.x * t,
                       first_origin.y + first_direction.y * t};
    }
    Float2 best = vertices[0];
    float best_distance = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < boundary.count; ++i)
    {
        const Float2 a = vertices[i];
        const Float2 b = vertices[(i + 1) % boundary.count];
        const Float2 d{b.x - a.x, b.y - a.y};
        const float length2 = d.x * d.x + d.y * d.y;
        const float t = length2 > 0.0f ? std::clamp(((point.x - a.x) * d.x +
            (point.y - a.y) * d.y) / length2, 0.0f, 1.0f) : 0.0f;
        const Float2 candidate{a.x + d.x * t, a.y + d.y * t};
        const float dx = candidate.x - point.x;
        const float dy = candidate.y - point.y;
        const float distance = dx * dx + dy * dy;
        if (distance < best_distance) { best = candidate; best_distance = distance; }
    }
    return best;
}

} // namespace hs
