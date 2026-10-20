#pragma once
#include <hs/render_data/geometry_package.hpp>
#include <cstdint>
#include <span>
namespace hs::geometry_content
{
struct GeometryErrorBounds { double lower{}, upper{}; };
[[nodiscard]] render_data::GeometryData CookGeometry(
    std::span<const render_data::ExpandedVertex> expanded, std::uint32_t flags);
// Qualified method ABI 1: symmetric material-aware distance to closed triangle
// surfaces, with adaptive Lipschitz upper bounds (including degenerate faces).
// ExpandedVertex::material_index may be a shader/atlas entry rather than the
// actual MaterialDefinition identity. When supplied, material_mapping maps
// each authored entry to that identity; an empty mapping preserves identity.
[[nodiscard]] GeometryErrorBounds MeasureGeometryError(
    std::span<const render_data::ExpandedVertex> fine,
    std::span<const render_data::ExpandedVertex> coarse,
    std::span<const std::uint16_t> material_mapping = {},
    std::uint64_t refinement_budget = 4096);
}
