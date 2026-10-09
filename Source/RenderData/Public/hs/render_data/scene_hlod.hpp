#pragma once

#include <hs/render_data/geometry_package.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace hs::render_data
{
inline constexpr std::uint32_t kSceneHlodAbi = 1;
inline constexpr std::uint32_t kSceneHlodHeaderStride = 128;
inline constexpr std::uint32_t kSceneHlodGroupStride = 80;
inline constexpr std::uint32_t kSceneHlodMemberStride = 112;
inline constexpr std::uint32_t kSceneHlodProxyStride = 80;
inline constexpr std::uint32_t kSceneHlodPageStride = 32;
inline constexpr std::uint32_t kNoSceneHlodGroup = 0xffffffffu;
inline constexpr std::uint32_t kSceneHlodPagePinned = 1;

struct SceneHlodMember
{
    std::uint64_t stable_id{}, object_id{};
    AssetId asset_id{};
    std::uint32_t geometry_dependency{}, mesh_semantic{}, variant{}, tint_rgba{};
    Float3 position{}, scale{1, 1, 1};
    float yaw{};
    GeometryBounds bounds;
};

struct SceneHlodGroup
{
    std::uint32_t parent{kNoSceneHlodGroup}, first_child{}, child_count{};
    std::uint32_t first_member{}, member_count{}, first_proxy{}, proxy_count{};
    GeometryBounds bounds;
    float world_error{};
};

struct SceneHlodProxy
{
    std::uint32_t page_index{}, first_index{}, index_count{};
    std::uint32_t first_meshlet{}, meshlet_count{}, material_dependency{};
    std::uint32_t mesh_semantic{}, atlas_selector{}, tint_rgba{}, variant{};
    GeometryBounds bounds;
};

struct SceneHlodPage
{
    std::string logical_id;
    std::uint32_t flags{}, geometry_flags{};
    std::uint32_t vertex_count{}, index_count{}, meshlet_count{};
};

struct SceneHlod
{
    std::uint32_t abi{kSceneHlodAbi}, layout_abi{}, error_method_abi{kGeometryErrorMethodAbi};
    AssetId scene_asset_id{};
    std::array<std::byte, 32> layout_digest{};
    // Source geometry and material product IDs; page IDs are in their directory.
    std::vector<std::string> dependencies;
    // Spatial permutation of complete object records. Group intervals refer
    // directly to these stable IDs, children exactly partition each parent.
    std::vector<SceneHlodMember> members;
    std::vector<SceneHlodGroup> groups;
    std::vector<SceneHlodProxy> proxies;
    std::vector<SceneHlodPage> pages;
    std::vector<std::uint32_t> roots, children;
};

void ValidateSceneHlod(const SceneHlod &scene);
[[nodiscard]] std::array<std::byte, 32> ComputeSceneHlodLayoutDigest(
    std::uint32_t layout_abi, std::span<const SceneHlodMember> members);
void ValidateSceneHlodPage(const SceneHlod &scene, std::uint32_t page_index,
                          const GeometryData &page);
void ValidateSceneHlodPages(const SceneHlod &scene, std::span<const GeometryData> pages);
// SHL1: explicit little-endian fields, strides and checked component sizes.
// No object bytes, GPU ABI, or Presentation dependency are serialized.
[[nodiscard]] std::vector<std::byte> EncodeSceneHlod(const SceneHlod &scene);
[[nodiscard]] SceneHlod DecodeSceneHlod(std::span<const std::byte> bytes);
} // namespace hs::render_data
