#pragma once

#include <hs/render_data/texture_package.hpp>

#include <array>

namespace hs::render_data
{
inline constexpr std::uint32_t kTerrainSurfaceLayoutVersion = 2;
struct TerrainSurfaceSection
{
    float x{}, z{}, span_x{}, span_z{}, cosine{}, sine{};
    std::uint32_t mesh{}, seed{};
    bool operator==(const TerrainSurfaceSection &) const = default;
};
static_assert(sizeof(TerrainSurfaceSection) == 32);

struct TerrainSurfacePackageView
{
    std::vector<TerrainSurfaceSection> sections;
    TextureDescription description;
    std::vector<std::string> section_products;
    std::array<std::byte, 32> source_identity{};
    std::uint32_t layout_version{};
};

// TSF1 v1 stores 32-byte little-endian section records followed by indices into
// sorted package dependencies, preserving the caller's section-product order.
// Layout 2: color RGB/roughness A; base normal RG/AO B/residual depth A (0..0.05 m);
// four terrain layer weights in RGBA. Each section uses three array slices.
[[nodiscard]] std::vector<std::byte> BuildTerrainSurfacePackage(
    AssetId asset_id, std::span<const std::string> section_products,
    std::span<const TerrainSurfaceSection> sections, TextureDescription description,
    const std::array<std::byte, 32> &source_identity);

// Section records and logical product IDs are decoded into owned storage.
[[nodiscard]] TerrainSurfacePackageView ReadTerrainSurfacePackage(
    std::span<const std::byte> package_bytes, AssetId expected_asset_id);
} // namespace hs::render_data
