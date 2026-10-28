#pragma once

#include <hs/render_data/render_snapshot.hpp>
#include <hs/render_data/vfx_frame_input.hpp>
#include <hs/render_data/vfx_owned_mesh.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace hs::renderer_detail
{
struct VfxOwnedMeshDrawRun
{
    std::size_t first{};
    std::size_t count{};
    std::uint32_t vertex_count{};
};

// Mesh indices are admitted as 1-based indices into vertex_counts before batching.
inline VfxOwnedMeshDrawRun NextVfxOwnedMeshDrawRun(
    std::span<const VfxOwnedMeshInput> commands,
    std::span<const std::uint32_t> vertex_counts, std::size_t first) noexcept
{
    assert(first <= commands.size());
    if (first == commands.size()) return {first, 0, 0};

    const auto mesh_index = commands[first].mesh_index;
    const auto vertex_count = vertex_counts[mesh_index - 1];
    auto end = first + 1;
    while (end < commands.size())
    {
        const auto next_mesh_index = commands[end].mesh_index;
        if (vertex_counts[next_mesh_index - 1] != vertex_count) break;
        ++end;
    }
    return {first, end - first, vertex_count};
}

using VfxGroundCoverage =
    std::tuple<std::uint64_t, std::uint8_t, std::uint8_t, std::uint32_t>;
using VfxStatusCoverage = std::pair<std::uint64_t, std::uint8_t>;

inline void PrepareVfxGroundCoverage(std::span<const VfxGroundSpawnInput> ground_spawns,
                                    bool preview_enabled,
                                    std::vector<VfxGroundCoverage> &coverage)
{
    coverage.clear();
    coverage.reserve(ground_spawns.size());
    for (const auto &ground : ground_spawns)
    {
        std::uint32_t primitive{};
        switch (ground.geometry.shape)
        {
        case VfxGroundShape::PolarRune:
        case VfxGroundShape::LineBorder:
            break;
        case VfxGroundShape::Circle:
            if (preview_enabled || ground.command.primitive != VfxPrimitive::ExactRing) continue;
            primitive = static_cast<std::uint32_t>(VfxPrimitive::ExactRing);
            break;
        default:
            continue;
        }
        coverage.emplace_back(ground.stable_id, ground.source_visual_kind,
                              static_cast<std::uint8_t>(ground.geometry.shape), primitive);
    }
    std::ranges::sort(coverage);
    coverage.erase(std::unique(coverage.begin(), coverage.end()), coverage.end());
}

inline bool HasVfxGroundCoverage(std::span<const VfxGroundCoverage> coverage,
                                 std::uint64_t stable_id, std::uint8_t kind,
                                 VfxGroundShape shape,
                                 VfxPrimitive primitive = VfxPrimitive{})
{
    const auto normalized_primitive = shape == VfxGroundShape::Circle
        ? static_cast<std::uint32_t>(primitive) : 0u;
    return std::ranges::binary_search(coverage,
        VfxGroundCoverage{stable_id, kind, static_cast<std::uint8_t>(shape), normalized_primitive});
}

inline void PrepareVfxStatusCoverage(std::span<const PersistentVfxVisual> visuals,
                                     std::span<const VfxGroundCoverage> ground_coverage,
                                     std::vector<VfxStatusCoverage> &coverage)
{
    coverage.clear();
    for (const auto &visual : visuals)
    {
        if (visual.kind != PersistentVfxKind::EnemySlowStatus &&
            visual.kind != PersistentVfxKind::EnemyMarkStatus) continue;
        const auto kind = static_cast<std::uint8_t>(visual.kind);
        if (HasVfxGroundCoverage(ground_coverage, visual.stable_id, kind,
                                 VfxGroundShape::PolarRune))
            coverage.emplace_back(visual.entity_render_id, kind);
    }
    std::ranges::sort(coverage);
    coverage.erase(std::unique(coverage.begin(), coverage.end()), coverage.end());
}

inline bool HasVfxStatusCoverage(std::span<const VfxStatusCoverage> coverage,
                                 std::uint64_t entity_id, std::uint8_t kind)
{
    return std::ranges::binary_search(coverage, VfxStatusCoverage{entity_id, kind});
}

struct PreviousVfxVisualRow
{
    const PersistentVfxVisual *visual{};
    std::size_t source_order{};
};

inline void PreparePreviousVfxVisualIndex(std::span<const PersistentVfxVisual> visuals,
                                          std::vector<PreviousVfxVisualRow> &index)
{
    index.clear();
    index.reserve(visuals.size());
    for (std::size_t row = 0; row < visuals.size(); ++row)
        index.push_back({&visuals[row], row});
    std::ranges::sort(index, [](const auto &left, const auto &right) {
        return std::tie(left.visual->stable_id, left.visual->kind, left.source_order) <
               std::tie(right.visual->stable_id, right.visual->kind, right.source_order);
    });
}

inline const PersistentVfxVisual *FindPreviousVfxVisual(
    std::span<const PreviousVfxVisualRow> index, std::uint64_t stable_id,
    PersistentVfxKind kind) noexcept
{
    const auto found = std::ranges::lower_bound(index, std::pair{stable_id, kind},
        std::ranges::less{}, [](const PreviousVfxVisualRow &candidate) {
            return std::pair{candidate.visual->stable_id, candidate.visual->kind};
        });
    return found != index.end() && found->visual->stable_id == stable_id &&
           found->visual->kind == kind ? found->visual : nullptr;
}
} // namespace hs::renderer_detail
