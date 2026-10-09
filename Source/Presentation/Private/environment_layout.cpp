#include <hs/presentation/environment_layout.hpp>
#include <hs/core/sha256.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <numbers>
#include <ranges>
#include <stdexcept>
#include <string>

namespace hs
{
namespace
{
constexpr float kPi = std::numbers::pi_v<float>;
void Require(bool condition, const char *reason)
{
    if (!condition) throw std::runtime_error("environment layout: " + std::string(reason));
}
constexpr std::uint64_t ProvenanceId(std::uint64_t category) noexcept
{
    return (6ull << 60) | (category << 56);
}
Float2 Add(Float2 a, Float2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
Float2 Subtract(Float2 a, Float2 b) noexcept { return {a.x - b.x, a.y - b.y}; }
Float2 Multiply(Float2 v, float s) noexcept { return {v.x * s, v.y * s}; }
float LengthSquared(Float2 v) noexcept { return v.x * v.x + v.y * v.y; }
Float2 Normalize(Float2 v) noexcept
{
    const auto length = std::sqrt(LengthSquared(v));
    return length > .0001f ? Multiply(v, 1.0f / length) : Float2{0, 1};
}
std::uint32_t EnvironmentHash(std::uint32_t value) noexcept
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}
float EnvironmentUnit(std::uint32_t value) noexcept
{
    return static_cast<float>(EnvironmentHash(value) & 0xffffu) / 65535.0f;
}
void HashWord(Sha256Stream &digest, std::uint32_t value)
{
    std::array<std::byte, 4> bytes;
    for (std::size_t lane = 0; lane < 4; ++lane)
        bytes[lane] = std::byte((value >> (lane * 8)) & 255u);
    digest.Add(bytes);
}
void HashId(Sha256Stream &digest, std::uint64_t value)
{
    HashWord(digest, static_cast<std::uint32_t>(value));
    HashWord(digest, static_cast<std::uint32_t>(value >> 32));
}
} // namespace

EnvironmentLayout BuildEnvironmentLayout(
    float arena_half_extent, const ArenaBoundary &boundary,
    std::span<const ArenaObstacle2D> obstacles, std::uint64_t seed)
{
    Require(std::isfinite(arena_half_extent) && arena_half_extent >= 0.0f,
            "invalid arena extent");
    Require(boundary.count <= boundary.points.size() &&
            obstacles.size() <= kArenaObstacleMaxCount, "layout input count");
    for (std::uint32_t edge = 0; edge < boundary.count; ++edge)
        Require(std::isfinite(boundary.points[edge].x) && std::isfinite(boundary.points[edge].y),
                "nonfinite boundary point");
    for (const auto &obstacle : obstacles)
        Require(std::isfinite(obstacle.center.x) && std::isfinite(obstacle.center.y) &&
                std::isfinite(obstacle.radius) && obstacle.radius > 0.0f &&
                (obstacle.kind == ArenaObstacleKind::Tree || obstacle.kind == ArenaObstacleKind::Rock),
                "invalid obstacle");
    EnvironmentLayout layout;
    const auto add_environment = [&](Float3 position, float yaw, Float3 scale,
                                     std::uint32_t color, RenderMesh mesh, std::uint32_t variant, std::uint64_t stable_id, std::uint64_t object_id = 0) {
        RenderInstance instance{
            position, yaw, scale, color, mesh, stable_id, 0,
            static_cast<std::uint32_t>(seed ^ (seed >> 32)), variant};
        PopulateRenderInstanceRevisions(instance);
        Require(layout.instances.size() < 3'000'000, "layout allocation cap");
        if (object_id != 0)
            layout.eligible_static_members.push_back({stable_id, object_id,
                static_cast<std::uint32_t>(layout.instances.size())});
        layout.instances.push_back(instance);
    };
    const auto add_tree = [&](Float2 position, std::uint32_t variation,
                              float size_multiplier, std::uint64_t object_id, bool fixed_scale = false) {
        const auto size = size_multiplier * (fixed_scale ? 1.0f : 0.85f + EnvironmentUnit(variation) * 0.3f);
        const auto yaw = EnvironmentUnit(variation + 2) * 2.0f * kPi;
        const auto variant = variation % 3;
        add_environment({position.x, 0.0f, position.y}, yaw, {size, size, size},
                        0xFFFFFFFFu, RenderMesh::TreeTrunk, variant, object_id | 1u, object_id);
        add_environment({position.x, 0.0f, position.y}, yaw, {size, size, size},
                        0xFFFFFFFFu, RenderMesh::TreeCanopy, variant, object_id | 2u, object_id);
    };

    // Continuous slab covers authored trees outside the playable boundary.
    const auto ground_extent = arena_half_extent + 16.0f;
    add_environment({0.0f, -0.05f, 0.0f}, 0.0f,
                    {ground_extent * 2.0f, 0.1f, ground_extent * 2.0f},
                    0xFF527E3Du, RenderMesh::Ground, 0, ProvenanceId(1));

    constexpr float kGrassClusterSpacing = 2.15f;
    const auto grass_limit = arena_half_extent - 2.0f;
    const auto grass_seed = EnvironmentHash(static_cast<std::uint32_t>(seed) ^
                                            EnvironmentHash(static_cast<std::uint32_t>(seed >> 32)));
    Require(std::ceil(double(grass_limit) * 2.0 / kGrassClusterSpacing) + 1.0 <= 65535.0,
            "grass provenance coordinate range");
    std::uint32_t grass_index{}, grid_z{};
    for (float z = -grass_limit; z <= grass_limit; z += kGrassClusterSpacing, ++grid_z)
    {
        std::uint32_t grid_x{};
        for (float x = -grass_limit; x <= grass_limit; x += kGrassClusterSpacing, ++grid_x)
        {
            const auto cluster = EnvironmentHash(grass_seed ^ (grass_index++ + 0x47524153u));
            // Omitted patches and widely jittered centres leave irregular natural gaps.
            if (EnvironmentUnit(cluster) < 0.2f) continue;
            const Float2 centre{x + (EnvironmentUnit(cluster + 1) - 0.5f) * 2.0f,
                                z + (EnvironmentUnit(cluster + 2) - 0.5f) * 2.0f};
            const auto radius = 0.7f + EnvironmentUnit(cluster + 3) * 0.65f;
            const auto count = 10u + EnvironmentHash(cluster + 4) % 7u;
            for (std::uint32_t member = 0; member < count; ++member)
            {
                const auto variation = EnvironmentHash(cluster ^ (member + 1) * 0x9e3779b9u);
                const auto angle = EnvironmentUnit(variation) * 2.0f * kPi;
                // Product sampling concentrates growth inside each patch with a soft edge.
                const auto distance = radius * std::sqrt(EnvironmentUnit(variation + 1) *
                                                          EnvironmentUnit(variation + 2));
                const Float2 position{centre.x + std::cos(angle) * distance,
                                      centre.y + std::sin(angle) * distance};
                const auto path_center = std::sin(position.y * 0.085f) * 6.5f;
                const auto blocked = std::ranges::any_of(
                    obstacles, [&](const ArenaObstacle2D &obstacle) {
                        const auto clearance = obstacle.radius + 0.45f;
                        return LengthSquared(Subtract(position, obstacle.center)) <
                               clearance * clearance;
                    });
                if (!ContainsArenaPoint(boundary, position, 2.0f) ||
                    std::abs(position.x - path_center) < 4.8f || blocked)
                    continue;
                const auto height = 0.18f + EnvironmentUnit(variation + 3) * 0.22f;
                const auto width = 0.55f + EnvironmentUnit(variation + 3) * 0.65f;
                add_environment({position.x, 0.0f, position.y},
                                EnvironmentUnit(variation + 4) * 2.0f * kPi,
                                {width, height, width},
                                (variation & 1u) ? 0xFF4F873Du : 0xFF68A34Au,
                                RenderMesh::Grass, variation % 4,
                                ProvenanceId(2) | (std::uint64_t(grid_z) << 32) |
                                    (std::uint64_t(grid_x) << 16) | member);
            }
        }
    }

    for (std::size_t index = 0; index < obstacles.size(); ++index)
    {
        const auto &obstacle = obstacles[index];
        const auto variation = EnvironmentHash(static_cast<std::uint32_t>(index) +
                                               0x524F434Bu);
        if (obstacle.kind == ArenaObstacleKind::Tree)
            add_tree(obstacle.center, variation,
                     obstacle.radius / 1.0409733f, ProvenanceId(3) | (std::uint64_t(index) << 8), true);
        else
        {
            constexpr std::array rock_radius{1.202624f, 1.155475f, 1.373369f, 1.245674f};
            const auto scale = obstacle.radius / rock_radius[variation % 4];
            add_environment({obstacle.center.x, 0.0f, obstacle.center.y},
                            EnvironmentUnit(variation + 3) * 2.0f * kPi,
                            {scale, scale, scale},
                            (variation & 1u) ? 0xFF76766Fu : 0xFF8A897Du,
                            RenderMesh::Rock, variation % 4,
                            ProvenanceId(3) | (std::uint64_t(index) << 8) | 1u,
                            ProvenanceId(3) | (std::uint64_t(index) << 8));
        }
    }

    for (std::uint32_t edge = 0; edge < boundary.count; ++edge)
    {
        const auto from = boundary.points[edge];
        const auto to = boundary.points[
            (edge + 1) % boundary.count];
        const auto segment = Subtract(to, from);
        const auto length = std::sqrt(LengthSquared(segment));
        Require(std::isfinite(length) && length > 0.0f && std::ceil(double(length) / 2.6) <= 0xffffff,
                "border provenance tree range");
        const auto tree_count = std::max(1u, static_cast<std::uint32_t>(
                                                std::ceil(length / 2.6f)));
        for (std::uint32_t row = 0; row < 3; ++row)
        {
            for (std::uint32_t tree = 0; tree < tree_count; ++tree)
            {
                const auto variation = EnvironmentHash(
                    edge * 1'013u + row * 313u + tree * 37u + 0x54524545u);
                const auto t = (static_cast<float>(tree) + 0.5f +
                                (EnvironmentUnit(variation) - 0.5f) * 0.55f) /
                               static_cast<float>(tree_count);
                auto position = Add(from, Multiply(segment, t));
                const auto outward = Normalize(position);
                const auto tangent = Normalize(segment);
                position = Add(position,
                               Add(Multiply(outward, 1.0f + row * 1.75f),
                                   Multiply(tangent,
                                            (EnvironmentUnit(variation + 1) - 0.5f) *
                                                1.25f)));
                add_tree(position, variation,
                         0.82f + EnvironmentUnit(variation + 2) * 0.48f +
                             static_cast<float>(row) * 0.08f,
                         ProvenanceId(4) | (std::uint64_t(edge) << 48) |
                             (std::uint64_t(row) << 40) | (std::uint64_t(tree) << 8));
            }
        }
    }
    Sha256Stream digest;
    HashWord(digest, kEnvironmentLayoutAbi);
    HashWord(digest, static_cast<std::uint32_t>(layout.eligible_static_members.size()));
    for (const auto &member : layout.eligible_static_members)
    {
        const auto &instance = layout.instances[member.instance_index];
        HashId(digest, member.stable_id);
        HashId(digest, member.object_id);
        HashWord(digest, static_cast<std::uint32_t>(instance.mesh));
        HashWord(digest, instance.environment_variant);
        HashWord(digest, instance.color_rgba);
        for (const float value : {instance.position.x, instance.position.y, instance.position.z,
                                 instance.yaw, instance.scale.x, instance.scale.y, instance.scale.z})
            HashWord(digest, std::bit_cast<std::uint32_t>(value == 0.0f ? 0.0f : value));
    }
    layout.eligible_digest = digest.Finish();
    return layout;
}

} // namespace hs
