#pragma once

#include <hs/render_data/texture_package.hpp>

#include <array>

namespace hs::render_data
{
inline constexpr std::uint32_t kTerrainSurfaceLayoutVersion = 6;
inline constexpr std::uint32_t kTerrainSurfacePageEdge = 4096;
inline constexpr std::uint32_t kTerrainSurfacePageMips = 13;
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
    // Layout 6: three direct plane-product references per section.
    std::vector<std::string> section_products;
    std::array<std::byte, 32> source_identity{};
    std::uint32_t layout_version{};
};

// TSF1 wire 4/layout 6 keeps 32-byte little-endian section records and three
// direct plane-product references per section, for up to 64 sections.
// Layout 6 uses 4096 pages with a 256-texel continuation around the interior.
// Depth is an averaged appearance value, not a conservative height bound.
[[nodiscard]] std::vector<std::byte> BuildTerrainSurfacePackage(
    AssetId asset_id, std::span<const std::string> section_products,
    std::span<const TerrainSurfaceSection> sections, TextureDescription description,
    const std::array<std::byte, 32> &source_identity);

// Section records and logical product IDs are decoded into owned storage.
[[nodiscard]] TerrainSurfacePackageView ReadTerrainSurfacePackage(
    std::span<const std::byte> package_bytes, AssetId expected_asset_id);
} // namespace hs::render_data
