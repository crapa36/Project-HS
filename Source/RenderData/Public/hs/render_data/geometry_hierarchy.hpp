#pragma once

#include <hs/render_data/geometry_package.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hs::render_data
{
inline constexpr std::uint32_t kGeometryHierarchyAbi = 1;
inline constexpr std::uint32_t kGeometryHierarchyHeaderStride = 64;
inline constexpr std::uint32_t kGeometryHierarchyGroupStride = 96;
inline constexpr std::uint32_t kGeometryHierarchyClusterStride = 80;
inline constexpr std::uint32_t kGeometryHierarchyPageStride = 32;
inline constexpr std::uint32_t kNoGeometryHierarchyGroup = 0xffffffffu;
inline constexpr std::uint32_t kGeometryHierarchyPagePinned = 1;

enum class GeometryHierarchyErrorDomain : std::uint32_t
{
    Static = 0,
    // Bounds and errors describe the input reference geometry only. Skin/wind
    // consumers must provide their own conservative deformation contract.
    ReferencePose = 1,
};

struct GeometryRefinementGroup
{
    std::uint32_t parent{kNoGeometryHierarchyGroup};
    std::uint32_t first_child{}, child_count{};
    std::uint32_t first_cluster{}, cluster_count{};
    std::uint32_t first_source_primitive{}, source_primitive_count{};
    std::uint32_t material_partition{}; // actual MaterialDefinition partition
    std::uint32_t material_selector{}; // original ExpandedVertex atlas/material entry
    GeometryBounds bounds;
    float geometric_error{};
};

struct GeometryHierarchyCluster
{
    std::uint32_t page_index{}, first_index{}, index_count{};
    std::uint32_t first_meshlet{}, meshlet_count{}, material_partition{};
    std::uint32_t material_selector{};
    GeometryBounds bounds;
    float geometric_error{};
};

struct GeometryHierarchyPage
{
    std::string logical_id;
    std::uint32_t flags{};
    std::uint32_t vertex_count{}, index_count{}, meshlet_count{};
};

struct GeometryHierarchy
{
    std::uint32_t abi{kGeometryHierarchyAbi};
    std::uint32_t error_method_abi{kGeometryErrorMethodAbi};
    std::uint32_t geometry_flags{};
    GeometryHierarchyErrorDomain error_domain{GeometryHierarchyErrorDomain::Static};
    std::uint32_t source_triangle_count{};
    std::vector<GeometryRefinementGroup> groups;
    std::vector<GeometryHierarchyCluster> clusters;
    std::vector<GeometryHierarchyPage> pages;
    std::vector<std::uint32_t> root_groups;
    std::vector<std::uint32_t> child_groups;
    // An explicit permutation of original triangle occurrence IDs. A group's
    // interval is its exact fine patch; children partition that interval in
    // child_groups order. Identical triangles still have distinct IDs.
    std::vector<std::uint32_t> source_primitive_order;
};

// A cut selects either every coarse cluster of a group, or complete cuts of
// every immediate child. Residency of only some children cannot replace a
// parent. Root representations occupy separately pinned pages.
void ValidateGeometryHierarchy(const GeometryHierarchy &hierarchy);
// Checks payload ranges, meshlet/canonical indexed parity and conservative
// reference bounds. Supplying source also proves exact leaf occurrence payloads.
void ValidateGeometryHierarchyPages(const GeometryHierarchy &hierarchy,
    std::span<const GeometryData> pages,
    std::span<const ExpandedVertex> source = {});
// Individual runtime page validation proves directory/topology/material/bounds
// agreement. Exact original leaf occurrence payloads require the source-aware
// all-page validation above.
void ValidateGeometryHierarchyPage(const GeometryHierarchy &hierarchy,
    std::uint32_t page_index, const GeometryData &page);

// Required main-package reference v1: uint16 dependency index, uint16 zero.
[[nodiscard]] std::vector<std::byte> EncodeGeometryHierarchyReference(
    std::uint16_t dependency_index, std::span<const std::string> dependencies,
    std::string_view main_product_id = {});
[[nodiscard]] std::uint16_t DecodeGeometryHierarchyReference(
    std::span<const std::byte> bytes, std::span<const std::string> dependencies,
    std::string_view main_product_id = {});

// Portable little-endian GHI1 record: fixed header, groups, clusters, page
// directory, root IDs, child IDs, occurrence IDs, then page ID UTF-8 bytes.
// Group material_selector is at byte 76; cluster material_selector at byte 68.
// Reserved fields are zero; C++ object representations are never serialized.
[[nodiscard]] std::vector<std::byte> EncodeGeometryHierarchy(const GeometryHierarchy &hierarchy);
[[nodiscard]] GeometryHierarchy DecodeGeometryHierarchy(std::span<const std::byte> bytes);
} // namespace hs::render_data
