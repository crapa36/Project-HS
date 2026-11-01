#pragma once

#include <hs/render_data/render_snapshot_values.hpp>
#include <hs/render_data/vfx_frame_input.hpp>
#include <hs/render_data/vfx_owned_mesh.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

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
    const PreparedVfxMeshRecipeView &recipes,
    std::span<const std::uint32_t> vertex_counts, std::size_t first) noexcept
{
    assert(first <= commands.size());
    if (first == commands.size()) return {first, 0, 0};

    const auto recipe_rows = recipes.Rows();
    assert(recipes.IsAdmitted() && commands[first].recipe_index < recipe_rows.size());
    const auto mesh_index = recipe_rows[commands[first].recipe_index].mesh_index;
    assert(mesh_index != 0 && mesh_index <= vertex_counts.size());
    const auto vertex_count = vertex_counts[mesh_index - 1];
    auto end = first + 1;
    while (end < commands.size())
    {
        assert(commands[end].recipe_index < recipe_rows.size());
        const auto next_mesh_index = recipe_rows[commands[end].recipe_index].mesh_index;
        assert(next_mesh_index != 0 && next_mesh_index <= vertex_counts.size());
        if (vertex_counts[next_mesh_index - 1] != vertex_count) break;
        ++end;
    }
    return {first, end - first, vertex_count};
}

} // namespace hs::renderer_detail
