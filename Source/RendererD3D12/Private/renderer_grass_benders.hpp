#pragma once

#include <hs/render_data/render_snapshot_values.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

namespace hs
{
struct GrassBenderSelection
{
    std::array<const RenderInstance *, 32> instances{};
    std::uint32_t count{};
};

// Packed as one float4 followed by sixteen uint4s in FrameConstants.
struct GrassBenderGrid
{
    Float4 world_to_cell{}; // origin xz, inverse cell size, enabled.
    std::array<std::array<std::uint32_t, 4>, 16> masks{};
};

// Each history frame builds its grid in the same coordinates as its benders.
template<class Bender, std::size_t Extent>
[[nodiscard]] inline GrassBenderGrid BuildGrassBenderGrid(std::span<Bender, Extent> benders)
{
    GrassBenderGrid result;
    const auto count = std::min<std::size_t>(benders.size(), 32);
    if (count <= 3) return result;
    double min_x = std::numeric_limits<double>::max(), min_z = min_x;
    double max_x = -min_x, max_z = max_x;
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto &bender = benders[index];
        if (!std::isfinite(bender.x) || !std::isfinite(bender.z) || !std::isfinite(bender.w)) return result;
        const auto radius = static_cast<double>(std::max(bender.w, 0.1f));
        min_x = std::min(min_x, bender.x - radius); max_x = std::max(max_x, bender.x + radius);
        min_z = std::min(min_z, bender.z - radius); max_z = std::max(max_z, bender.z + radius);
    }
    const auto extent = std::max(max_x - min_x, max_z - min_z);
    if (!(extent > 0.0)) return result;
    result.world_to_cell = {static_cast<float>(min_x), static_cast<float>(min_z), static_cast<float>(8.0 / extent), 1.0f};
    if (!std::isfinite(result.world_to_cell.x) || !std::isfinite(result.world_to_cell.y) ||
        !std::isfinite(result.world_to_cell.z) || result.world_to_cell.z < std::numeric_limits<float>::min())
        return {};
    const auto cell = [&](double value, double origin, double padding) {
        return static_cast<int>(std::clamp(std::floor((value - origin) * result.world_to_cell.z) + padding, 0.0, 7.0));
    };
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto &bender = benders[index];
        const auto radius = static_cast<double>(std::max(bender.w, 0.1f));
        // An extra cell covers CPU/GPU rounding at cell boundaries.
        const auto x0 = cell(bender.x - radius, result.world_to_cell.x, -1.0);
        const auto x1 = cell(bender.x + radius, result.world_to_cell.x, 1.0);
        const auto z0 = cell(bender.z - radius, result.world_to_cell.y, -1.0);
        const auto z1 = cell(bender.z + radius, result.world_to_cell.y, 1.0);
        for (auto z = z0; z <= z1; ++z)
            for (auto x = x0; x <= x1; ++x)
            {
                const auto flat = static_cast<std::size_t>(z * 8 + x);
                result.masks[flat / 4][flat % 4] |= std::uint32_t{1} << index;
            }
    }
    return result;
}
[[nodiscard]] inline GrassBenderSelection SelectGrassBenders(std::span<const RenderInstance> instances)
{
    GrassBenderSelection result;
    const auto player = std::ranges::find(instances, RenderMesh::Archer, &RenderInstance::mesh);
    if (player == instances.end()) return result;
    result.instances[result.count++] = &*player;
    struct Candidate { float distance; const RenderInstance *instance; };
    std::vector<Candidate> candidates;
    for (const auto &source : instances)
        if (source.mesh >= RenderMesh::MonsterMelee && source.mesh <= RenderMesh::BossFinal)
        {
            const auto dx = source.position.x - player->position.x;
            const auto dz = source.position.z - player->position.z;
            candidates.push_back({dx * dx + dz * dz, &source});
        }
    const auto count = std::min(candidates.size(), result.instances.size() - 1);
    std::partial_sort(candidates.begin(), candidates.begin() + count, candidates.end(),
        [](const auto &a, const auto &b) {
            if (a.distance != b.distance) return a.distance < b.distance;
            if (a.instance->stable_id != b.instance->stable_id)
                return a.instance->stable_id < b.instance->stable_id;
            return a.instance < b.instance;
        });
    for (std::size_t index = 0; index < count; ++index)
        result.instances[result.count++] = candidates[index].instance;
    return result;
}
} // namespace hs
