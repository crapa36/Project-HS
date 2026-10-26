#pragma once

#include <hs/render_data/geometry_package.hpp>
#include <limits>

namespace hs::render_data
{
struct GeometryHandle
{
    std::uint32_t value{std::numeric_limits<std::uint32_t>::max()};
    explicit operator bool() const noexcept { return value != std::numeric_limits<std::uint32_t>::max(); }
};
struct GeometryByteRange
{
    std::uint64_t offset{}, byte_count{};
};
// Chunk order is the existing GeometryVertices..GeometryCanonicalPrimitiveRemap ABI.
struct GeometryDescriptor
{
    AssetId asset_id;
    std::uint32_t bulk_index{}, flags{}, vertex_count{}, index_count{}, meshlet_count{}, vertex_ref_count{};
    std::uint32_t max_material_index{}, max_bone_index{};
    GeometryBounds bounds;
    std::array<GeometryByteRange, 7> chunks;
    std::span<const AssetId> dependencies;
    std::span<const GeometrySubmesh> submeshes;
    std::span<const ExpandedVertex> ground_support;
    bool payload_ready{true};
};
struct GeometryDescriptorStorage
{
    AssetId asset_id;
    std::uint32_t bulk_index{}, flags{}, vertex_count{}, index_count{}, meshlet_count{}, vertex_ref_count{};
    std::uint32_t max_material_index{}, max_bone_index{};
    GeometryBounds bounds;
    std::array<GeometryByteRange, 7> chunks;
    std::vector<AssetId> dependencies;
    std::vector<GeometrySubmesh> submeshes;
    std::vector<ExpandedVertex> ground_support;
    bool payload_ready{true};
    [[nodiscard]] GeometryDescriptor View() const noexcept
    {
        return {asset_id, bulk_index, flags, vertex_count, index_count, meshlet_count, vertex_ref_count,
            max_material_index, max_bone_index, bounds, chunks, dependencies, submeshes, ground_support,
            payload_ready};
    }
};
struct GeometryGpuView
{
    std::array<std::span<const std::byte>, 7> chunks;
    std::span<const GeometrySubmesh> submeshes;
};

// The prepared page has already passed the existing semantic/topology checks.
[[nodiscard]] GeometryDescriptorStorage CompileGeometryDescriptor(
    std::span<const std::byte> bytes, const PreparedGeometryPage &, std::uint32_t bulk_index);
[[nodiscard]] GeometryDescriptorStorage CompileGeometryDescriptor(
    std::span<const std::byte> bytes, const PackageView &, const GeometryData &,
    std::uint32_t bulk_index);
[[nodiscard]] PreparedGeometryPage PrepareGeometryPagePayload(
    std::span<const std::byte> bytes, AssetId asset_id, std::uint32_t flags);
void ValidateGeometryDescriptor(const GeometryDescriptor &, std::uint64_t product_bytes);
void ValidateGeometryDescriptorMetadata(const GeometryDescriptor &, const GeometryMetadata &);
[[nodiscard]] GeometryGpuView BindGeometryDescriptor(
    std::span<const std::byte> authenticated_bytes, const GeometryDescriptor &);
[[nodiscard]] std::vector<ExpandedVertex> BuildGroundSupport(
    std::span<const ExpandedVertex> vertices, float ground_offset);
} // namespace hs::render_data
