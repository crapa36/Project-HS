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

    DiscMotion2D motion;
    const std::span<const ArenaObstacle2D> clear;
    (void)MoveDisc2D({0, 1.0451f}, {0, .025f}, .63f, boundary, clear, &motion);
    assert(SegmentDiscHitTime({-.2333333f, 0}, {.2333333f, 0}, motion.final_position, 1.07f) > 1);
    assert(MovingDiscHitTime({-.2333333f, 0}, {.2333333f, 0}, motion, 1.07f) <= 1);
    (void)MoveDisc2D({0, 1.094f}, {0, -.025f}, .63f, boundary, clear, &motion);
    assert(SegmentDiscHitTime({-.2333333f, 0}, {.2333333f, 0}, motion.final_position, 1.07f) <= 1);
    assert(MovingDiscHitTime({-.2333333f, 0}, {.2333333f, 0}, motion, 1.07f) > 1);
    (void)MoveDisc2D({6, 0}, {-12, 0}, 0.5f, boundary, clear, &motion);
    assert(SegmentDiscHitTime({0, 0}, {0, 0}, motion.final_position, 1) > 1);
    assert(std::abs(MovingDiscHitTime({0, 0}, {0, 0}, motion, 1) - 5.0f / 12.0f) < .00001f);
    // Co-moving discs never meet even when the target endpoint lies on the projectile segment.
    (void)MoveDisc2D({-2, 0}, {10, 0}, .5f, boundary, clear, &motion);
    assert(SegmentDiscHitTime({0, 0}, {10, 0}, motion.final_position, 1) <= 1);
    assert(MovingDiscHitTime({0, 0}, {10, 0}, motion, 1) > 1);

    (void)MoveDisc2D({-5, 0}, {10, 0}, .5f, boundary, clear, &motion);
    assert(MovingDiscHitTime({2.5f, -5}, {2.5f, 5}, motion, .1f, .5f) <= 1);
    assert(MovingDiscHitTime({2.5f, -5}, {2.5f, 5}, motion, .1f) > 1);
    (void)MoveDisc2D({0, 0}, {10, 0}, .5f, boundary, clear, &motion);
    assert(MovingDiscHitTime({5, -5}, {5, 5}, motion, .1f, .5f) > 1);
    assert(MovingDiscHitTime({5, -5}, {5, 5}, motion, .1f) <= 1);

    motion.Reset({-4, 0});
    motion.Record({-4, 0}, {4, 0}, 0, .4f);
    motion.Record({4, 0}, {-4, 0}, .4f, 1);
    assert(std::abs(MovingDiscHitTime({}, {}, motion, .1f) - .195f) < .00001f);
    assert(std::abs(MovingDiscHitTime({}, {}, motion, .1f, .5f) - .385f) < .00001f);

    DiscMotion2D slide;
    const Float2 slide_start{-10, -4};
    const auto slide_end = MoveDisc2D(slide_start, {20, 10}, 1, boundary, obstacles, &slide);
    assert(slide.count >= 2);
    const Float2 chord_mid{(slide_start.x + slide_end.x) * .5f, (slide_start.y + slide_end.y) * .5f};
    DiscMotion2D chord;
    chord.Reset(slide_end); chord.Record(slide_start, slide_end, 0, 1);
    assert(MovingDiscHitTime(chord_mid, chord_mid, chord, 1.01f) <= 1);
    assert(MovingDiscHitTime(chord_mid, chord_mid, slide, 1.01f) > 1);
    const auto knee = slide.segments[0].to;
    assert(MovingDiscHitTime(knee, knee, slide, 1.01f) <= 1);
    // Asking for a trace must not change the movement solver's positions.
    const auto untraced = MoveDisc2D(slide_start, {20, 10}, 1, boundary, obstacles);
    assert(slide_end.x == untraced.x && slide_end.y == untraced.y);

    (void)MoveDisc2D({-6, 0}, {}, .5f, boundary, clear, &motion);
    motion.final_position = {6, 0}; // An instantaneous separation/projection correction.
    assert(MovingDiscHitTime({0, 0}, {0, 0}, motion, 1) > 1);
    assert(MovingDiscHitTime({6, 0}, {6, 0}, motion, 1) == 1);
    (void)MoveDisc2D({6, 0}, {}, .5f, boundary, clear, &motion);
    for (const auto start : {Float2{-20, 0}, Float2{0, 0}, Float2{6, 0}})
        assert(MovingDiscHitTime(start, {20, 0}, motion, 1) ==
               SegmentDiscHitTime(start, {20, 0}, motion.final_position, 1));
    return 0;
}
