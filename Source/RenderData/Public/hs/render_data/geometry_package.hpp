#pragma once

#include <hs/render_data/render_asset_format.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hs::render_data
{
inline constexpr std::uint32_t kGeometryAbi = 1;
// Shared CPU/shader descriptor capacity; residency byte budgets remain independent.
inline constexpr std::uint32_t kGeometryPageCapacity = 256;
inline constexpr std::uint32_t kGeometryBuffersPerPage = 4;
inline constexpr std::uint32_t kGeometryDescriptorCount = 1 + kGeometryPageCapacity * kGeometryBuffersPerPage;
inline constexpr std::uint32_t kGeometryErrorMethodAbi = 1;
inline constexpr std::uint32_t kGeometryMetadataStride = 96;
inline constexpr std::uint32_t kGeometrySubmeshStride = 20;
inline constexpr std::uint32_t kGeometryMeshletStride = 64;
inline constexpr std::uint32_t kGeometryFlagSkinned = 1;
inline constexpr std::uint32_t kGeometryFlagTwoSided = 2;
inline constexpr std::uint32_t kGeometryFlagWind = 4;
inline constexpr std::uint32_t kGeometryMeshletConeValid = 1;
inline constexpr std::uint16_t kNoGeometryDependency = 0xffff;
inline constexpr std::uint32_t kMaxGeometryVertices = 3'000'000;
inline constexpr std::uint32_t kMaxGeometryIndices = 3'000'000;
inline constexpr std::uint32_t kMaxGeometryMeshletVertices = 64;
inline constexpr std::uint32_t kMaxGeometryMeshletTriangles = 124;

struct GeometryBounds
{
    std::array<float, 3> minimum{}, maximum{};
    std::array<float, 4> sphere{}; // center.xyz, outward-rounded radius
};
struct GeometryMetadata
{
    std::uint32_t abi{kGeometryAbi}, flags{};
    std::uint32_t vertex_count{}, index_count{}, submesh_count{}, meshlet_count{};
    std::uint32_t vertex_ref_count{}, max_material_index{};
    std::uint16_t page_dependency_index{kNoGeometryDependency};
    std::uint16_t fallback_dependency_index{kNoGeometryDependency}, lod{};
    std::uint16_t terminal_fallback_dependency_index{kNoGeometryDependency};
    GeometryBounds bounds;
    float geometric_error{};
    std::uint32_t error_method_abi{kGeometryErrorMethodAbi};
};
struct GeometrySubmesh
{
    std::uint32_t first_index{}, index_count{}, first_meshlet{}, meshlet_count{}, material_index{};
};
struct GeometryMeshlet
{
    std::uint32_t first_vertex_ref{}, vertex_ref_count{}, first_triangle{}, triangle_count{}, submesh_index{};
    std::array<float, 4> sphere{};
    std::array<float, 4> cone{0.0f, 0.0f, 0.0f, 1.0f}; // normalized axis.xyz, cutoff
    std::uint32_t flags{};
};
struct GeometryData
{
    std::vector<ExpandedVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<GeometrySubmesh> submeshes;
    std::vector<GeometryMeshlet> meshlets;
    std::vector<std::uint32_t> meshlet_vertex_refs, meshlet_triangles, canonical_primitive_remap;
    GeometryBounds bounds;
    std::uint32_t flags{};
};

// Metadata field offsets: eight uint32 0..31; four uint16 32..39;
// minimum 40, maximum 52, sphere 64; geometric error 80, method ABI 84;
// reserved uint64 88 (zero). Meshlet: five uint32 0..19, sphere 20,
// cone 36, flags 52, reserved uint64 56 (zero). Object layout is never serialized.
[[nodiscard]] std::vector<std::byte> EncodeGeometryMetadata(
    const GeometryMetadata &metadata, std::span<const std::string> dependencies,
    std::string_view metadata_product_id);
[[nodiscard]] GeometryMetadata DecodeGeometryMetadata(
    std::span<const std::byte> bytes, std::span<const std::string> dependencies,
    std::string_view metadata_product_id);
void ValidateGeometryData(const GeometryData &geometry);
void ValidateGeometryPage(const GeometryMetadata &metadata, const GeometryData &geometry);
[[nodiscard]] std::vector<std::byte> BuildGeometryPage(
    AssetId page_asset_id, std::span<const std::string> dependencies,
    const GeometryData &geometry);
[[nodiscard]] GeometryData DecodeGeometryPage(
    std::span<const std::byte> bytes, AssetId page_asset_id, const GeometryMetadata &metadata);
// Geometry pages contain seven topology chunks. Hierarchy consumers supply the
// validated geometry flags; reference bounds are reconstructed from the payload.
[[nodiscard]] GeometryData DecodeGeometryPagePayload(
    std::span<const std::byte> bytes, AssetId page_asset_id, std::uint32_t flags);
} // namespace hs::render_data
