#pragma once

#include "geometry_cooker.hpp"
#include <hs/render_data/geometry_hierarchy.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace hs::geometry_content
{
struct GeometryHierarchyCookOptions
{
    std::uint32_t target_group_size{4};
    float reduction_ratio{0.5f};
    std::uint32_t max_page_triangles{4096};
    std::uint32_t max_page_meshlets{64};
    GeometryErrorChecks error_checks;
};

struct CookedGeometryHierarchy
{
    render_data::GeometryHierarchy hierarchy;
    // Same indices as hierarchy.pages; compatible with BuildGeometryPage.
    std::vector<render_data::GeometryData> pages;
};

// logical_page_prefix is e.g. render/hierarchy/trunk_0. Page IDs append
// /group_<stable group ID>/page_<stable page ordinal>.rpage. No authored LOD
// payload is used as a cluster parent. Index-only simplification keeps complete
// original ExpandedVertex records and preserves material/atlas boundaries.
[[nodiscard]] CookedGeometryHierarchy CookGeometryHierarchy(
    std::span<const render_data::ExpandedVertex> expanded, std::uint32_t flags,
    std::string_view logical_page_prefix,
    std::span<const std::uint16_t> material_mapping = {},
    GeometryHierarchyCookOptions options = {});
} // namespace hs::geometry_content
