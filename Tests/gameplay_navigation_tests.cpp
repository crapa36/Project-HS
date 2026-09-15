#include "navigation_grid.hpp"
#include <cassert>
#include <array>

int main()
{
    using namespace hs;
    using namespace hs::gameplay_detail;
    ArenaBoundary boundary{};
    boundary.count = 4;
    boundary.points = {Float2{-50, -50}, Float2{-50, 50},
                       Float2{50, 50}, Float2{50, -50}};
    std::array<ArenaObstacle2D, 1> obstacles{{
        {ArenaObstacleKind::Rock, {0, 0}, 4}}};

    NavigationGrid grid;
    grid.Rebuild(boundary, obstacles, 1);
    grid.RebuildFlow({40, 0});
    const Float2 waypoint = grid.NextWaypoint({-40, 0}, {40, 0});
    assert(waypoint.x > -40 && waypoint.y != 0);

    auto position = Float2{-40, 0};
    for (int step = 0; step < 2000 && NavigationDistanceSquared(position, {40, 0}) > 1.0f; ++step)
    {
        const auto target = SegmentClear2D(position, {40, 0}, 1, boundary, obstacles)
            ? Float2{40, 0} : grid.NextWaypoint(position, {40, 0});
        Float2 delta{target.x - position.x, target.y - position.y};
        const auto length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
        if (length > 0.2f) { delta.x *= 0.2f / length; delta.y *= 0.2f / length; }
        position = MoveDisc2D(position, delta, 1, boundary, obstacles);
    }
    assert(NavigationDistanceSquared(position, {40, 0}) <= 1.0f);

    const Float2 moved = MoveDisc2D({-10, 0}, {30, 0}, 1, boundary, obstacles);
    assert(moved.x < -5.0f);
    const Float2 inward = MoveDisc2D({-5, 0}, {12, 0}, 1, boundary, obstacles);
    assert(inward.x <= -5.0f);
    const Float2 tangent = MoveDisc2D({-5, 0}, {0, 8}, 1, boundary, obstacles);
    assert(tangent.y > 7.9f && std::abs(tangent.x + 5.0f) < 0.01f);
    const Float2 projected = ProjectOutsideObstacles({0, 0}, 1, obstacles);
    assert(projected.x * projected.x + projected.y * projected.y >= 25.0f);
    // Sweeps must not tunnel and must order actor contacts before opaque terrain.
    const auto wall = FirstObstacleHitTime({-20, 0}, {20, 0}, 0.5f, obstacles);
    assert(std::abs(wall - 0.3875f) < 0.00001f);
    assert(std::abs(FirstObstacleHitTime({20, 0}, {-20, 0}, 0.5f, obstacles) - wall) < 0.00001f);
    const auto front = SegmentDiscHitTime({-20, 0}, {20, 0}, {-10, 0}, 1.0f);
    const auto behind = SegmentDiscHitTime({-20, 0}, {20, 0}, {10, 0}, 1.0f);
    assert(front < wall && behind > wall);
    assert(FirstObstacleHitTime({-1000, 0}, {1000, 0}, 0.5f, obstacles) < 1.0f);
    assert(FirstObstacleHitTime({-20, 5}, {20, 5}, 0.5f, obstacles) > 1.0f);
    assert(FirstObstacleHitTime({0, 0}, {0, 0}, 0.5f, obstacles) == 0.0f);
    assert(!SegmentClear2D({-5, 0}, {5, 0}, 0.0f, boundary, obstacles));
    assert(SegmentClear2D({-5, 5}, {5, 5}, 0.0f, boundary, obstacles));
    obstacles[0].kind = ArenaObstacleKind::Tree;
    assert(FirstObstacleHitTime({-20, 0}, {20, 0}, 0.5f, obstacles) == wall);
    return 0;
}
