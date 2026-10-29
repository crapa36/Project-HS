#pragma once

#include <hs/render_data/render_snapshot_values.hpp>
#include <hs/game_domain/arena_boundary.hpp>
#include <hs/game_domain/arena_obstacle.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hs
{
inline constexpr std::uint32_t kEnvironmentLayoutAbi = 2;
inline constexpr Float3 kEnvironmentSunDirection{-0.45f, -0.82f, 0.35f};

struct EnvironmentStaticMember
{
    std::uint64_t stable_id{}, object_id{};
    std::uint32_t instance_index{};
};

struct EnvironmentLayout
{
    std::uint32_t abi{kEnvironmentLayoutAbi};
    std::array<std::byte, 32> eligible_digest{};
    std::vector<RenderInstance> instances;
    // Obstacle, border and static ground-detail objects participate. Tree trunk/canopy share
    // object_id and must be replaced together. Ground and deformed grass stay
    // on their existing paths. Digest excludes session and grass seed.
    // Canonical stable-ID order; instance_index still points into original placement order.
    std::vector<EnvironmentStaticMember> eligible_static_members;
};

// Pure placement builder shared by Presentation and the explicit scene cook.
// IDs encode placement provenance, so grass omissions never rename trees.
[[nodiscard]] EnvironmentLayout BuildEnvironmentLayout(
    float arena_half_extent, const ArenaBoundary &boundary,
    std::span<const ArenaObstacle2D> obstacles, std::uint64_t seed);
} // namespace hs
