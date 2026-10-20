#pragma once

#include <hs/core/render_snapshot.hpp>
#include <hs/render_data/scene_hlod.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hs::geometry_content
{
struct SceneHlodCookMember
{
    RenderInstance instance;
    std::uint64_t object_id{};
};

struct SceneHlodCookSource
{
    AssetId asset_id{};
    std::string logical_id;
    const render_data::GeometryData *geometry{};
    // Authored ExpandedVertex selector -> actual cooked material product ID.
    std::span<const std::string> material_products;
};

struct SceneHlodCookOptions
{
    std::uint32_t objects_per_leaf{4}, children_per_group{4};
    float reduction_ratio{0.5f};
    std::uint32_t max_page_triangles{4096};
    // A replacement must be loadable as one complete bounded streaming unit.
    // Oversize parents retain their independently loadable child forest.
    std::uint32_t max_group_pages{32};
    // Bounded refinement for conservative geometry error used by runtime LOD.
    // Large exhaustive qualification passes must not block normal asset builds.
    std::uint64_t error_refinement_budget{4096};
    // Zero selects the bounded TaskSystem default; one keeps qualification serial.
    std::uint32_t error_worker_count{};
};

struct CookedSceneHlod
{
    render_data::SceneHlod scene;
    // Same indices as scene.pages, emitted with existing BuildGeometryPage.
    std::vector<render_data::GeometryData> pages;
};

// Requires explicit eligible records and real decoded source pages. Deformed
// sources are rejected. Spatial grouping preserves complete object membership;
// transforms and inverse-transpose normals are baked into existing vertex ABI.
[[nodiscard]] CookedSceneHlod CookSceneHlod(
    AssetId scene_asset_id, std::uint32_t layout_abi,
    const std::array<std::byte, 32> &layout_digest,
    std::span<const SceneHlodCookMember> members,
    std::span<const SceneHlodCookSource> sources,
    std::string_view logical_page_prefix,
    SceneHlodCookOptions options = {});
} // namespace hs::geometry_content
