#pragma once
#include <hs/render_data/geometry_package.hpp>
#include <cstdint>
#include <span>
namespace hs::geometry_content
{
struct GeometryErrorBounds
{
    double lower{}, upper{};
    std::uint64_t oracle_queries{};
    bool qualification_used{};
};
enum class GeometryErrorCheckMode { Normal, Exhaustive };
struct GeometryErrorQualificationKey
{
    std::uint32_t bvh_abi{}, distance_abi{}, geometry_format_abi{}, material_policy{};
    std::uint32_t compiler{}, target{}, floating_point_contract{};
    std::int32_t rounding_mode{};
    std::uint64_t compiler_version{};
    bool operator==(const GeometryErrorQualificationKey &) const noexcept = default;
};
// An independently accepted GT09 result, supplied by the build caller. No
// production qualification is assumed from the method ABI or unit-test success.
// The record must outlive the synchronous cook, including its joined workers.
struct GeometryErrorQualification
{
    GeometryErrorQualificationKey key;
    double maximum_absolute_coordinate{};
    double minimum_positive_edge_squared{}, minimum_positive_area_squared{};
    std::uint64_t maximum_triangles_per_surface{};
    bool includes_degenerate_triangles{};
};
struct GeometryErrorChecks
{
    GeometryErrorCheckMode mode{GeometryErrorCheckMode::Normal};
    const GeometryErrorQualification *qualification{};
};
[[nodiscard]] GeometryErrorQualificationKey GeometryErrorImplementationKey() noexcept;
// Preserve complete vertex field identity without a portable encode/decode roundtrip.
void IndexVertices(std::span<const render_data::ExpandedVertex> expanded,
                   std::vector<render_data::ExpandedVertex> &vertices,
                   std::vector<unsigned> &indices);
[[nodiscard]] render_data::GeometryData CookGeometry(
    std::span<const render_data::ExpandedVertex> expanded, std::uint32_t flags);
// Method ABI 1: symmetric material-aware distance to closed triangle
// surfaces, with adaptive Lipschitz upper bounds (including degenerate faces).
// ExpandedVertex::material_index may be a shader/atlas entry rather than the
// actual MaterialDefinition identity. When supplied, material_mapping maps
// each authored entry to that identity; an empty mapping preserves identity.
[[nodiscard]] GeometryErrorBounds MeasureGeometryError(
    std::span<const render_data::ExpandedVertex> fine,
    std::span<const render_data::ExpandedVertex> coarse,
    std::span<const std::uint16_t> material_mapping = {},
    std::uint64_t refinement_budget = 4096,
    GeometryErrorChecks checks = {});
}
