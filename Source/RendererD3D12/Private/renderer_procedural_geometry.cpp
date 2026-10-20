#include "renderer_impl.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace hs
{
namespace
{
render_data::GeometryBounds ProceduralGeometryBounds(
    std::span<const render_data::ExpandedVertex> vertices)
{
    render_data::GeometryBounds bounds{};
    if (vertices.empty()) return bounds;

    bounds.minimum = bounds.maximum = vertices.front().position;
    for (const auto &vertex : vertices)
    {
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            bounds.minimum[axis] = std::min(bounds.minimum[axis], vertex.position[axis]);
            bounds.maximum[axis] = std::max(bounds.maximum[axis], vertex.position[axis]);
        }
    }

    double radius = 0.0;
    for (std::size_t axis = 0; axis < 3; ++axis)
        bounds.sphere[axis] = 0.5f * (bounds.minimum[axis] + bounds.maximum[axis]);
    for (const auto &vertex : vertices)
    {
        double distance_squared = 0.0;
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            const double delta = static_cast<double>(vertex.position[axis]) -
                static_cast<double>(bounds.sphere[axis]);
            distance_squared += delta * delta;
        }
        radius = std::max(radius, std::sqrt(distance_squared));
    }
    bounds.sphere[3] = std::nextafterf(
        static_cast<float>(radius), std::numeric_limits<float>::infinity());
    return bounds;
}
} // namespace

Result D3D12Renderer::Impl::CreateProceduralGeometry(
    ID3D12GraphicsCommandList *list, std::vector<AllocationResource> &uploads)
{
    if (!list)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Procedural geometry requires a command list.");
    if (procedural_geometry.vertices.resource || procedural_geometry.indices.resource ||
        procedural_geometry.meshlets.resource || procedural_geometry.auxiliary.resource ||
        procedural_geometry.page_slot != UINT32_MAX)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Procedural geometry page destination is already occupied.");

    static_assert(kCubeVertices.size() == 36);
    render_data::GeometryData geometry;
    geometry.vertices.reserve(kCubeVertices.size());
    for (const auto &source : kCubeVertices)
    {
        render_data::ExpandedVertex vertex{};
        vertex.position = source.position;
        vertex.normal = source.normal;
        vertex.bone_indices = source.bone_indices;
        vertex.bone_weights = source.bone_weights;
        vertex.uv = source.uv;
        vertex.tangent = source.tangent;
        vertex.material_index = source.material_index;
        vertex.reserved = source.reserved;
        geometry.vertices.push_back(vertex);
    }

    geometry.indices.resize(geometry.vertices.size());
    std::iota(geometry.indices.begin(), geometry.indices.end(), 0u);
    geometry.bounds = ProceduralGeometryBounds(geometry.vertices);

    const auto material_index = geometry.vertices.front().material_index;
    geometry.submeshes.push_back({0, static_cast<std::uint32_t>(geometry.indices.size()), 0, 1,
                                  material_index});
    geometry.meshlet_vertex_refs = geometry.indices;
    for (std::uint32_t primitive = 0; primitive < geometry.indices.size() / 3; ++primitive)
    {
        const auto first = primitive * 3;
        geometry.meshlet_triangles.push_back(first | ((first + 1) << 8) |
                                             ((first + 2) << 16));
        geometry.canonical_primitive_remap.push_back(primitive);
    }
    geometry.meshlets.push_back({0,
                                 static_cast<std::uint32_t>(geometry.meshlet_vertex_refs.size()),
                                 0,
                                 static_cast<std::uint32_t>(geometry.meshlet_triangles.size()),
                                 0,
                                 geometry.bounds.sphere,
                                 {0.0f, 0.0f, 0.0f, 1.0f},
                                 0});

    render_data::GeometryMetadata metadata;
    metadata.abi = render_data::kGeometryAbi;
    metadata.flags = geometry.flags;
    metadata.vertex_count = static_cast<std::uint32_t>(geometry.vertices.size());
    metadata.index_count = static_cast<std::uint32_t>(geometry.indices.size());
    metadata.submesh_count = static_cast<std::uint32_t>(geometry.submeshes.size());
    metadata.meshlet_count = static_cast<std::uint32_t>(geometry.meshlets.size());
    metadata.vertex_ref_count = static_cast<std::uint32_t>(geometry.meshlet_vertex_refs.size());
    metadata.max_material_index = 0;
    for (const auto &vertex : geometry.vertices)
        metadata.max_material_index = std::max(metadata.max_material_index,
                                               static_cast<std::uint32_t>(vertex.material_index));
    metadata.page_dependency_index = render_data::kNoGeometryDependency;
    metadata.fallback_dependency_index = render_data::kNoGeometryDependency;
    metadata.lod = 0;
    metadata.terminal_fallback_dependency_index = render_data::kNoGeometryDependency;
    metadata.bounds = geometry.bounds;
    metadata.geometric_error = 0.0f;
    metadata.error_method_abi = render_data::kGeometryErrorMethodAbi;

    const auto page_asset = MakeAssetId("render/internal/procedural_cube.rpage");
    const std::vector<std::string> dependencies;
    std::vector<std::byte> package_bytes;
    try
    {
        // BuildGeometryPage validates the native canonical data. CreateGeometryPage's decode
        // validates the complete page against metadata before allocating or recording it.
        package_bytes = render_data::BuildGeometryPage(page_asset, dependencies, geometry);
    }
    catch (const std::exception &error)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               std::string("Invalid procedural geometry: ") + error.what());
    }

    return CreateGeometryPage(procedural_geometry, metadata, package_bytes, page_asset,
                              kProceduralGeometryPage, list, uploads);
}
} // namespace hs
