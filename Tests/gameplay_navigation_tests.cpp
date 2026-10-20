#include "navigation_grid.hpp"
#include <cassert>
#include <array>
#include <queue>
#include <bit>

namespace
{
using namespace hs;
using namespace hs::gameplay_detail;
// Frozen scalar solver checks exact endpoints and swept traces after loop hoisting.
hs::Float2 ReferenceMoveDisc2D(Float2 from, Float2 delta, float radius,
                         const ArenaBoundary &boundary,
                         std::span<const ArenaObstacle2D> obstacles,
                         DiscMotion2D *motion = nullptr) noexcept
{
    auto position = ClampToArena(boundary, ProjectOutsideObstacles(from, radius, obstacles), radius);
    if (motion) motion->Reset(position);
    auto elapsed = 0.0f;
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
        if (!hit)
        {
            const Float2 next{position.x + remaining.x, position.y + remaining.y};
            if (motion) motion->Record(position, next, elapsed, 1.0f);
            position = next;
            elapsed = 1.0f;
            break;
        }
        const auto safe_time = std::max(0.0f, hit_time - 1.0e-4f);
        const Float2 next{position.x + remaining.x * safe_time, position.y + remaining.y * safe_time};
        const auto contact_time = elapsed + (1.0f - elapsed) * hit_time;
        if (motion) motion->Record(position, next, elapsed, contact_time);
        position = next;
        elapsed = contact_time;
        remaining = {remaining.x * (1.0f - hit_time), remaining.y * (1.0f - hit_time)};
        const auto normal_length = std::sqrt(NavigationDistanceSquared(position, hit->center));
        if (normal_length <= 1.0e-6f) break;
        const Float2 normal{(position.x - hit->center.x) / normal_length,
                            (position.y - hit->center.y) / normal_length};
        const auto into_surface = remaining.x * normal.x + remaining.y * normal.y;
        if (into_surface < 0.0f) { remaining.x -= normal.x * into_surface; remaining.y -= normal.y * into_surface; }
    }
    const auto result = ProjectOutsideObstacles(ClampToArena(boundary, position, radius), radius, obstacles);
    if (motion)
    {
        motion->Record(position, position, elapsed, 1.0f);
        motion->final_position = result;
    }
    return result;
}

auto HeapFlow(const hs::gameplay_detail::NavigationGrid &grid, hs::Float2 target)
{
    using namespace hs::gameplay_detail;
    std::array<int, kNavigationGridCells> distance;
    distance.fill(kUnreachableNavigationDistance);
    const auto start = grid.NearestClearCell(target);
    if (start < 0) return distance;
    std::priority_queue<std::pair<int, int>, std::vector<std::pair<int, int>>, std::greater<>> pending;
    distance[start] = 0;
    pending.emplace(0, start);
    while (!pending.empty())
    {
        const auto [cost, current] = pending.top(); pending.pop();
        if (cost != distance[current]) continue;
        const auto x = current % grid.width, y = current / grid.width;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
        {
            if (dx == 0 && dy == 0) continue;
            const auto nx = x + dx, ny = y + dy;
            if (!grid.In(nx, ny)) continue;
            const auto next = grid.Index(nx, ny);
            if (grid.blocked[next] || (dx != 0 && dy != 0 &&
                (grid.blocked[grid.Index(x + dx, y)] || grid.blocked[grid.Index(x, y + dy)]))) continue;
            const auto next_cost = cost + (dx != 0 && dy != 0 ? 14 : 10);
            if (next_cost >= distance[next]) continue;
            distance[next] = next_cost;
            pending.emplace(next_cost, next);
        }
    }
    return distance;
}

void CheckMovementEquivalence()
{
    ArenaBoundary boundary{};
    boundary.count = 4;
    boundary.points = {Float2{-50, -50}, Float2{-50, 50}, Float2{50, 50}, Float2{50, -50}};
    const std::array<ArenaObstacle2D, 3> obstacles{{
        {ArenaObstacleKind::Rock, {0, 0}, 4},
        {ArenaObstacleKind::Rock, {7, 1}, 3},
        {ArenaObstacleKind::Rock, {-8, -3}, 2}}};
    const auto same = [](Float2 a, Float2 b) {
        return std::bit_cast<std::uint32_t>(a.x) == std::bit_cast<std::uint32_t>(b.x) &&
               std::bit_cast<std::uint32_t>(a.y) == std::bit_cast<std::uint32_t>(b.y);
    };
    std::uint32_t state = 53197;
    const auto random = [&] { state = state * 1664525u + 1013904223u; return (static_cast<float>(state >> 16) / 65535.0f - .5f) * 120.0f; };
    for (int index = 0; index < 2048; ++index)
    {
        const Float2 from{random(), random()};
        Float2 delta{random(), random()};
        if (index % 4 == 0) delta = {};
        if (index % 4 == 1) delta = {0.00005f, -0.00005f};
        const auto radius = index % 2 ? .5f : 2.5f; // Ordinary and boss-sized discs.
        const auto active = std::span(obstacles).first(index % 4);
        DiscMotion2D expected, actual;
        const auto a = ReferenceMoveDisc2D(from, delta, radius, boundary, active, &expected);
        const auto b = MoveDisc2D(from, delta, radius, boundary, active, &actual);
        assert(same(a, b) && same(expected.final_position, actual.final_position));
        assert(expected.count == actual.count);
        for (std::uint8_t segment = 0; segment < expected.count; ++segment)
        {
            const auto &left = expected.segments[segment], &right = actual.segments[segment];
            assert(same(left.from, right.from) && same(left.to, right.to));
            assert(std::bit_cast<std::uint32_t>(left.begin) == std::bit_cast<std::uint32_t>(right.begin));
            assert(std::bit_cast<std::uint32_t>(left.end) == std::bit_cast<std::uint32_t>(right.end));
        }
    }
}

void CheckFlowEquivalence()
{
    using namespace hs::gameplay_detail;
    NavigationGrid grid;
    std::uint32_t random = 0x91352u;
    for (int scenario = 0; scenario < 24; ++scenario)
    {
        grid.width = scenario % 3 == 0 ? 1 : kNavigationGridDimension;
        grid.height = scenario % 3 == 1 ? 1 : kNavigationGridDimension;
        for (auto &blocked : grid.blocked)
        {
            random = random * 1664525u + 1013904223u;
            blocked = scenario == 0 ? 1 : scenario < 4 ? 0 : (random >> 24) < 64;
        }
        if (scenario == 23)
            for (int y = 0; y < grid.height; ++y) for (int x = 0; x < grid.width; ++x)
                grid.blocked[grid.Index(x, y)] =
                    y % 2 != 0 && x != (y % 4 == 1 ? grid.width - 1 : 0);
        for (const auto target : {hs::Float2{-80, -80}, hs::Float2{0, 0}, hs::Float2{49, 49}})
        {
            const auto expected = HeapFlow(grid, target);
            grid.RebuildFlow(target);
            assert(grid.distance == expected);
        }
    }
}
}

int main()
{
    CheckMovementEquivalence();
    CheckFlowEquivalence();
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
