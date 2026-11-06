#pragma once

#include <hs/render_data/texture_package.hpp>

#include <array>

namespace hs::render_data
{
inline constexpr std::uint32_t kTerrainSurfaceLayoutVersion = 5;
inline constexpr std::uint32_t kTerrainSurfaceLeafLayoutVersion = 4;
inline constexpr std::uint32_t kTerrainSurfacePageEdge = 4096;
inline constexpr std::uint32_t kTerrainSurfacePageMips = 13;
inline constexpr std::size_t kTerrainSurfaceMaxPageSections = 21;
struct TerrainSurfaceSection
{
    float x{}, z{}, span_x{}, span_z{}, cosine{}, sine{};
    std::uint32_t mesh{}, seed{};
    bool operator==(const TerrainSurfaceSection &) const = default;
};
static_assert(sizeof(TerrainSurfaceSection) == 32);

struct TerrainSurfaceTile
{
    TerrainSurfaceSection section;
    std::uint32_t source_index{}, x{}, y{}, count_x{}, count_y{};
};
// Source order, then row-major y/x. Section centers and signed spans preserve
// the parent's rotation and coverage; invalid inputs or more than 64 tiles fail.
[[nodiscard]] std::vector<TerrainSurfaceTile> BuildTerrainSurfaceTiles(
    std::span<const TerrainSurfaceSection> sections);

struct TerrainSurfacePackageView
{
    std::vector<TerrainSurfaceSection> sections;
    TextureDescription description;
    // Layout 1-3: one product per section; 4: three planes per section; 5: one leaf metadata product per tile.
    std::vector<std::string> section_products;
    std::array<std::byte, 32> source_identity{};
    std::uint32_t layout_version{};
};

// TSF1 v2 keeps 32-byte little-endian section records and three dependency
// indices per section. Each plane is an independent bounded texture package.
// Layout 4 uses 4096 pages with a 256-texel continuation around the interior.
// Depth is an averaged appearance value, not a conservative height bound.
[[nodiscard]] std::vector<std::byte> BuildTerrainSurfacePackage(
    AssetId asset_id, std::span<const std::string> section_products,
    std::span<const TerrainSurfaceSection> sections, TextureDescription description,
    const std::array<std::byte, 32> &source_identity);

// Layout 5 indexes one layout-4 leaf metadata package per tile.
[[nodiscard]] std::vector<std::byte> BuildTerrainSurfaceIndex(
    AssetId asset_id, std::span<const std::string> leaf_products,
    std::span<const TerrainSurfaceSection> tiles, TextureDescription description,
    const std::array<std::byte, 32> &source_identity);

// Section records and logical product IDs are decoded into owned storage.
[[nodiscard]] TerrainSurfacePackageView ReadTerrainSurfacePackage(
    std::span<const std::byte> package_bytes, AssetId expected_asset_id);
} // namespace hs::render_data
