#include <hs/presentation/environment_layout.hpp>
#include <hs/presentation/meadow_surface.hpp>
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
        if (arena_half_extent >= 14.0f && mesh != RenderMesh::Ground && mesh != RenderMesh::MeadowSurface)
            position.y += MeadowGroundHeight({position.x, position.z});
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
    if (arena_half_extent >= 14.0f) {
        const float outer_width = ground_extent - 12.0f;
        const float outer_center = (ground_extent + 12.0f) * .5f;
        // Ground adds half-scale to its origin, then shapes cube height by 0.1.
        constexpr float outer_y = -.055f;
        for (std::uint32_t side = 0; side < 2; ++side) {
            const float sign = side ? 1.0f : -1.0f;
            add_environment({0, outer_y, sign * outer_center}, 0,
                {ground_extent * 2, .1f, outer_width}, 0xFF527E3Du,
                RenderMesh::Ground, 0, ProvenanceId(1) | (side + 1u));
            add_environment({sign * outer_center, outer_y, 0}, 0,
                {outer_width, .1f, 24}, 0xFF527E3Du,
                RenderMesh::Ground, 0, ProvenanceId(1) | (side + 3u));
        }
        // The authored depression and level water replace, rather than overlap, the flat slab.
        add_environment({0,0,0}, 0, {1,1,1}, 0xFFFFFFFFu,
                        RenderMesh::MeadowSurface, 0, ProvenanceId(8) | 1u);
        add_environment({0,0,0}, 0, {1,1,1}, 0xFFFFFFFFu,
                        RenderMesh::MeadowSurface, 1, ProvenanceId(8) | 2u);
    } else {
        add_environment({0.0f, -0.05f, 0.0f}, 0.0f,
                        {ground_extent * 2.0f, 0.1f, ground_extent * 2.0f},
                        0xFF527E3Du, RenderMesh::Ground, 0, ProvenanceId(1));
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
    // Sparse static detail patches share the immutable scene/HLOD path. Their
    // placement is independent of the grass seed, so runtime and scene cooking
    // agree without changing any existing tree/rock/grass provenance IDs.
    constexpr std::size_t kMaxDetailPatches = 320;
    std::size_t detail_count{};
    std::vector<std::pair<Float2, float>> prop_footprints;
    const auto clear_detail = [&](Float2 position, float radius, bool loading_bay = false) {
        if (!ContainsArenaPoint(boundary, position, radius + 1.0f) ||
            (!loading_bay && MeadowRoadCoverage(position, -radius) > 0.0f))
            return false;
        if (std::ranges::any_of(prop_footprints, [&](const auto &prop) {
                const auto clearance = radius + prop.second + 0.2f;
                return LengthSquared(Subtract(position, prop.first)) < clearance * clearance;
            })) return false;
        return std::ranges::none_of(obstacles, [&](const auto &obstacle) {
            const auto delta = Subtract(position, obstacle.center);
            const auto clearance = obstacle.radius + radius + 0.45f;
            return LengthSquared(delta) < clearance * clearance;
        });
    };
    const auto detail_patch = [&](Float2 position, std::uint32_t variant,
                                  std::uint32_t key, float size, float yaw) {
        // Keep small species low while making flowers, fallen branches and fences readable beside the player.
        constexpr std::array<float, kGroundDetailVariantCount> species_scale{1.5f, 1.5f, 2.0f, 1.0f, 1.0f, 1.5f, 1.5f,
            1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        size *= species_scale[variant];
        constexpr std::array<float, kGroundDetailVariantCount> footprint{.58f,.58f,.58f,.58f,.58f,.58f,1.05f,
            1.0f,1.3f,.75f,.75f,.65f,.65f,2.0f};
        const auto radius = footprint[variant] * size;
        if (detail_count >= kMaxDetailPatches || !clear_detail(position, radius, variant == 13u)) return;
        const auto id = ProvenanceId(5) | key;
        add_environment({position.x, 0.0f, position.y}, yaw, {size, size, size},
                        0xFFFFFFFFu, RenderMesh::GroundDetail, variant, id,
                        HasEnvironmentWind(RenderMesh::GroundDetail, variant) ? 0u : id);
        ++detail_count;
        if (variant >= 7u) prop_footprints.emplace_back(position, radius);
    };
    // Two small roadside stories: a broken cart with supplies, and a rooted stump with brush.
    // Place their footprints first so decorative scattering cannot intersect them.
    detail_patch({11.0f, -6.0f}, 6u, 0x200006u, .95f, 1.45f);
    detail_patch({11.0f, 3.0f}, 13u, 0x20000du, 1.0f, kPi * .5f);
    detail_patch({10.0f, -0.8f}, 9u, 0x200009u, 1.0f, -.15f);
    detail_patch({11.8f, -1.6f}, 12u, 0x20000cu, 1.0f, .6f);
    detail_patch({-11.0f, -3.0f}, 8u, 0x200008u, 1.0f, -.3f);
    detail_patch({-11.0f, 0.0f}, 7u, 0x200007u, 1.0f, .4f);
    detail_patch({-11.0f, 2.5f}, 10u, 0x20000au, 1.0f, -.4f);
    detail_patch({-8.8f, 2.5f}, 11u, 0x20000bu, 1.0f, .2f);
    // Stable, irregular ecological islands around shelter, not a uniform flower border.
    for (std::size_t patch = 0; patch < prop_footprints.size(); ++patch)
    {
        const auto [anchor, footprint] = prop_footprints[patch];
        for (std::uint32_t member = 0; member < 3; ++member)
        {
            const auto noise = EnvironmentHash(static_cast<std::uint32_t>(patch) * 31u + member + 0x45434fu);
            const auto angle = .8f + float(patch) * 2.4f + float(member) * .48f;
            const auto distance = footprint + 1.35f + EnvironmentUnit(noise) * .45f;
            detail_patch({anchor.x + std::cos(angle) * distance, anchor.y + std::sin(angle) * distance},
                         member == 0 ? static_cast<std::uint32_t>(patch % 3) : 3u,
                         0x300000u | (static_cast<std::uint32_t>(patch) << 8u) | member,
                         .8f + EnvironmentUnit(noise + 1) * .3f, angle);
        }
    }
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
            // A broad growth field joins neighbouring cells into islands and open clearings.
            // The seed still controls their fine silhouette and individual blade identities.
            const auto growth = .5f + .5f * std::sin(centre.x * .37f + std::sin(centre.y * .23f) * 1.8f) *
                                                   std::cos(centre.y * .31f - centre.x * .12f);
            if (EnvironmentUnit(cluster + 5) > .50f + growth * .45f) continue;
            const auto radius = .65f + growth * .45f + EnvironmentUnit(cluster + 3) * .25f;
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
                const auto blocked = std::ranges::any_of(
                    obstacles, [&](const ArenaObstacle2D &obstacle) {
                        const auto clearance = obstacle.radius + 0.45f;
                        return LengthSquared(Subtract(position, obstacle.center)) <
                               clearance * clearance;
                    });
                const auto prop_blocked = std::ranges::any_of(prop_footprints, [&](const auto &prop) {
                    const auto clearance = prop.second + .45f;
                    return LengthSquared(Subtract(position, prop.first)) < clearance * clearance;
                });
                const bool meadow = arena_half_extent >= 14.0f && MeadowContains(position);
                // Retain a thin, pressed strip between wheel tracks; the same road rule
                // continues outside the detailed terrain tile without a density seam.
                const bool worn_path = (MeadowRoadCoverage(position) > 0.0f &&
                    std::abs(position.x - MeadowCartCenter(position.y)) > .35f) ||
                    (meadow && (MeadowPressed(position) > .2f || std::isfinite(MeadowWaterLevel(position))));
                if (!ContainsArenaPoint(boundary, position, 2.0f) || worn_path || blocked || prop_blocked)
                    continue;
                const bool pressed = (meadow && MeadowPressed(position) > .02f) || MeadowRoadCoverage(position, -.25f) > .02f;
                const auto height = (meadow ? .14f + growth * .06f + EnvironmentUnit(variation + 3) * .02f
                                           : .18f + growth * .18f + EnvironmentUnit(variation + 3) * .04f) * (pressed ? .5f : 1.0f);
                const auto width = meadow ? .45f + EnvironmentUnit(variation + 3) * .2f
                                          : .55f + EnvironmentUnit(variation + 3) * .65f;
                const auto yaw = pressed
                    ? (position.x > 1.4f && position.x < 11.7f && std::abs(position.y - 3.0f) < 1.2f
                        ? kPi * .5f : std::atan2(.5525f * std::cos(position.y * .085f), 1.0f))
                    : EnvironmentUnit(variation + 4) * 2.0f * kPi;
                add_environment({position.x, 0.0f, position.y}, yaw,
                                {width, height, width},
                                (variation & 1u) ? 0xFF4F873Du : 0xFF68A34Au,
                                RenderMesh::Grass, variation % 4,
                                ProvenanceId(2) | (std::uint64_t(grid_z) << 32) |
                                    (std::uint64_t(grid_x) << 16) | member);
            }
        }
    }

    if (arena_half_extent > 8.0f)
    {
        const auto length = arena_half_extent * 2.0f - 8.0f;
        const auto rows = std::min(32u, std::max(1u, static_cast<std::uint32_t>(std::ceil(length / 6.0f))));
        for (std::uint32_t row = 0; row < rows; ++row)
            for (std::uint32_t side = 0; side < 2; ++side)
            {
                const auto key = row * 2u + side;
                const auto variation = EnvironmentHash(key + 0x44455441u);
                const auto z = -arena_half_extent + 4.0f + (float(row) + 0.5f) * length / float(rows) +
                    (EnvironmentUnit(variation) - 0.5f) * 3.8f;
                const auto sign = side == 0u ? -1.0f : 1.0f;
                const auto path_center = MeadowCartCenter(z);
                const Float2 center{path_center + sign * (3.0f + EnvironmentUnit(variation + 1) * 2.5f), z};
                // Each island has a dominant flower species and a low clover fringe.
                const auto members = 2u + EnvironmentHash(variation + 16u) % 3u;
                for (std::uint32_t member = 0; member < members; ++member)
                {
                    const auto angle = EnvironmentUnit(variation + member * 17u + 2u) * 2.0f * kPi;
                    const auto offset = 0.25f + EnvironmentUnit(variation + member * 17u + 3u) * 0.9f;
                    const Float2 position{center.x + std::cos(angle) * offset, center.y + std::sin(angle) * offset};
                    detail_patch(position, member + 1u == members ? 3u : (row + side * 2u) % 3u, (key << 8u) | member,
                                 0.8f + EnvironmentUnit(variation + member + 4u) * 0.35f, angle);
                }
                if ((variation & 1u) == 0u)
                    detail_patch({center.x + sign * 1.2f, center.y + 0.8f}, 4u, (key << 8u) | 4u,
                                 0.8f + EnvironmentUnit(variation + 7u) * 0.3f,
                                 EnvironmentUnit(variation + 8u) * 2.0f * kPi);
                if (row % 3u == 1u)
                    detail_patch({center.x + sign * 1.8f, center.y - 0.7f}, 5u, (key << 8u) | 5u,
                                 0.8f + EnvironmentUnit(variation + 9u) * 0.3f,
                                 EnvironmentUnit(variation + 10u) * 2.0f * kPi);
                // Broken, infrequent fence runs stay beyond the open path.
                if (row % 7u == 1u)
                    detail_patch({center.x + sign * 2.0f, center.y + 1.4f}, 6u, (key << 8u) | 6u,
                                 0.9f + EnvironmentUnit(variation + 11u) * 0.15f,
                                 kPi * 0.5f + (EnvironmentUnit(variation + 12u) - 0.5f) * 0.2f);
                const Float2 pebble{center.x - sign * 0.7f, center.y + 1.1f};
                if (clear_detail(pebble, 0.2f))
                {
                    const auto scale = 0.045f + EnvironmentUnit(variation + 13u) * 0.055f;
                    const auto id = ProvenanceId(6) | key;
                    add_environment({pebble.x, 0.0f, pebble.y}, EnvironmentUnit(variation + 14u) * 2.0f * kPi,
                                    {scale, scale * 0.7f, scale}, 0xFFFFFFFFu, RenderMesh::Rock, variation % 4u, id, id);
                }
                const Float2 tuft{center.x + sign * 0.55f, center.y - 1.0f};
                if (clear_detail(tuft, 0.45f))
                    add_environment({tuft.x, 0.0f, tuft.y}, EnvironmentUnit(variation + 15u) * 2.0f * kPi,
                                    {0.55f, 0.18f, 0.55f}, 0xFF68A34Au, RenderMesh::Grass, variation % 4u,
                                    ProvenanceId(7) | key);
            }
        for (std::size_t index = 0; index < std::min<std::size_t>(obstacles.size(), 64); ++index)
        {
            const auto &obstacle = obstacles[index];
            const auto variation = EnvironmentHash(static_cast<std::uint32_t>(index) + 0x4c495454u);
            const auto angle = EnvironmentUnit(variation) * 2.0f * kPi;
            const auto distance = obstacle.radius + 1.2f;
            const Float2 position{obstacle.center.x + std::cos(angle) * distance,
                                  obstacle.center.y + std::sin(angle) * distance};
            detail_patch(position, obstacle.kind == ArenaObstacleKind::Tree ? 4u : 5u,
                         0x100000u | static_cast<std::uint32_t>(index),
                         0.85f + EnvironmentUnit(variation + 1u) * 0.25f, angle);
        }
    }
    // HLOD hashes canonical stable-ID order, independent of interleaved placement categories.
    std::ranges::sort(layout.eligible_static_members, {}, &EnvironmentStaticMember::stable_id);
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
