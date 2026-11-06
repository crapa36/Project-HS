#include <hs/render_data/terrain_surface.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace hs::render_data
{
namespace
{
constexpr std::uint32_t kMagic = 0x31465354; // TSF1
constexpr std::size_t kHeader = 68, kStride = 32, kMaxSections = 64;
void Require(bool valid, std::string_view reason)
{
    if (!valid) throw std::runtime_error("terrain surface: " + std::string(reason));
}
void Put(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value)
{
    Require(offset <= bytes.size() && 4 <= bytes.size() - offset, "write bounds");
    for (std::size_t i = 0; i < 4; ++i) bytes[offset + i] = std::byte((value >> (8 * i)) & 255u);
}
std::uint32_t Get(std::span<const std::byte> bytes, std::size_t offset)
{
    Require(offset <= bytes.size() && 4 <= bytes.size() - offset, "read bounds");
    std::uint32_t value{};
    for (std::size_t i = 0; i < 4; ++i)
        value |= std::uint32_t(std::to_integer<unsigned>(bytes[offset + i])) << (8 * i);
    return value;
}
void ValidateSection(const TerrainSurfaceSection &s)
{
    for (const float value : {s.x, s.z, s.span_x, s.span_z, s.cosine, s.sine})
        Require(std::isfinite(value), "nonfinite section");
    Require(s.span_x != 0 && s.span_z != 0, "zero section span");
}
void ValidateShape(TextureDescription d, std::size_t count)
{
    Require(count > 0 && count <= kMaxSections && d.width > 0 && d.width <= 16384 &&
        d.width == d.height && std::has_single_bit(d.width) &&
        (d.format == kTextureFormatRgba8 || d.format == kTextureFormatBc7) &&
        d.array_size == count * 3 && d.mip_count == std::bit_width(d.width),
        "invalid section or texture shape");
}
}

std::vector<TerrainSurfaceTile> BuildTerrainSurfaceTiles(std::span<const TerrainSurfaceSection> sections)
{
    std::vector<TerrainSurfaceTile> tiles;
    for (std::size_t source = 0; source < sections.size(); ++source)
    {
        const auto &s = sections[source];
        ValidateSection(s);
        const auto nx = std::ceil(std::abs(static_cast<double>(s.span_x)) / 24.0);
        const auto ny = std::ceil(std::abs(static_cast<double>(s.span_z)) / 24.0);
        Require(source <= std::numeric_limits<std::uint32_t>::max() && nx <= kMaxSections && ny <= kMaxSections &&
            nx * ny <= kMaxSections - tiles.size(), "too many terrain tiles");
        const auto count_x = static_cast<std::uint32_t>(nx), count_y = static_cast<std::uint32_t>(ny);
        const auto span_x = static_cast<double>(s.span_x) / count_x;
        const auto span_z = static_cast<double>(s.span_z) / count_y;
        for (std::uint32_t y = 0; y < count_y; ++y)
            for (std::uint32_t x = 0; x < count_x; ++x)
            {
                const auto dx = span_x * (x + 0.5 - count_x * 0.5);
                const auto dz = span_z * (y + 0.5 - count_y * 0.5);
                const auto center_x = static_cast<double>(s.x) + s.cosine * dx + s.sine * dz;
                const auto center_z = static_cast<double>(s.z) - s.sine * dx + s.cosine * dz;
                const auto max_float = static_cast<double>(std::numeric_limits<float>::max());
                Require(std::isfinite(center_x) && std::isfinite(center_z) &&
                    std::abs(center_x) <= max_float && std::abs(center_z) <= max_float,
                    "terrain tile center overflow");
                auto tile = s;
                tile.x = static_cast<float>(center_x);
                tile.z = static_cast<float>(center_z);
                tile.span_x = static_cast<float>(span_x);
                tile.span_z = static_cast<float>(span_z);
                ValidateSection(tile);
                tiles.push_back({tile, static_cast<std::uint32_t>(source), x, y, count_x, count_y});
            }
    }
    return tiles;
}

namespace
{
std::vector<std::byte> BuildTerrainSurface(AssetId asset_id,
    std::span<const std::string> section_products, std::span<const TerrainSurfaceSection> sections,
    TextureDescription description, const std::array<std::byte, 32> &source_identity, std::uint32_t wire_version)
{
    ValidateShape(description, sections.size());
    Require(wire_version == 3 || sections.size() <= kTerrainSurfaceMaxPageSections,
        "split section count exceeds dependency limit");
    Require(section_products.size() == sections.size() * (wire_version == 2 ? 3 : 1),
        "section product count mismatch");
    std::vector<std::string> dependencies(section_products.begin(), section_products.end());
    std::ranges::sort(dependencies);
    Require(std::adjacent_find(dependencies.begin(), dependencies.end()) == dependencies.end(),
        "duplicate section product");
    std::vector<std::byte> encoded(kHeader + sections.size() * kStride + section_products.size() * 4);
    Put(encoded, 0, kMagic); Put(encoded, 4, wire_version); Put(encoded, 8, static_cast<std::uint32_t>(sections.size()));
    Put(encoded, 12, wire_version == 2 ? kTerrainSurfaceLeafLayoutVersion : kTerrainSurfaceLayoutVersion);
    Put(encoded, 16, description.width); Put(encoded, 20, description.height);
    Put(encoded, 24, description.array_size); Put(encoded, 28, description.mip_count);
    Put(encoded, 32, description.format);
    std::copy(source_identity.begin(), source_identity.end(), encoded.begin() + 36);
    const auto indices = kHeader + sections.size() * kStride;
    for (std::size_t i = 0; i < sections.size(); ++i)
    {
        const auto &s = sections[i]; ValidateSection(s);
        const auto offset = kHeader + i * kStride;
        const std::array values{s.x, s.z, s.span_x, s.span_z, s.cosine, s.sine};
        for (std::size_t j = 0; j < values.size(); ++j)
            Put(encoded, offset + j * 4, std::bit_cast<std::uint32_t>(values[j]));
        Put(encoded, offset + 24, s.mesh); Put(encoded, offset + 28, s.seed);
    }
    for (std::size_t i = 0; i < section_products.size(); ++i)
    {
        const auto it = std::lower_bound(dependencies.begin(), dependencies.end(), section_products[i]);
        Put(encoded, indices + i * 4, static_cast<std::uint32_t>(it - dependencies.begin()));
    }
    const std::array chunks{ChunkSource{static_cast<std::uint32_t>(ChunkType::TerrainSurfaceSections),
        static_cast<std::uint16_t>(wire_version), true, encoded, encoded.size(), 1}};
    return BuildPackage(asset_id, dependencies, chunks);
}
}

std::vector<std::byte> BuildTerrainSurfacePackage(AssetId asset_id,
    std::span<const std::string> section_products, std::span<const TerrainSurfaceSection> sections,
    TextureDescription description, const std::array<std::byte, 32> &source_identity)
{
    return BuildTerrainSurface(asset_id, section_products, sections, description, source_identity, 2);
}

std::vector<std::byte> BuildTerrainSurfaceIndex(AssetId asset_id,
    std::span<const std::string> leaf_products, std::span<const TerrainSurfaceSection> tiles,
    TextureDescription description, const std::array<std::byte, 32> &source_identity)
{
    return BuildTerrainSurface(asset_id, leaf_products, tiles, description, source_identity, 3);
}

TerrainSurfacePackageView ReadTerrainSurfacePackage(std::span<const std::byte> package_bytes,
                                                    AssetId expected_asset_id)
{
    const auto package = ReadPackage(package_bytes, expected_asset_id);
    Require(package.chunks.size() == 1 && package.chunks[0].type == ChunkType::TerrainSurfaceSections &&
        package.chunks[0].required, "missing sections");
    const auto bytes = package.chunks[0].bytes;
    Require(bytes.size() >= kHeader && Get(bytes, 0) == kMagic,
        "invalid section header");
    const auto wire_version = Get(bytes, 4);
    const auto stored_layout = Get(bytes, 12);
    Require((wire_version == 1 && package.chunks[0].version == 1 && stored_layout <= 3) ||
        (wire_version == 2 && package.chunks[0].version == 2 && stored_layout == 4) ||
        (wire_version == 3 && package.chunks[0].version == 3 && stored_layout == 5),
        "unsupported terrain surface version or layout");
    const auto count = Get(bytes, 8);
    const auto product_count = std::size_t(count) * (wire_version == 2 ? 3 : 1);
    Require(count > 0 && count <= (wire_version == 2 ? kTerrainSurfaceMaxPageSections : kMaxSections) &&
        bytes.size() == kHeader + std::size_t(count) * kStride + product_count * 4 &&
        package.dependencies.size() == product_count, "invalid section count");
    TerrainSurfacePackageView result;
    result.layout_version = stored_layout == 0 ? 1 : stored_layout;
    result.description = {Get(bytes, 16), Get(bytes, 20), Get(bytes, 24), Get(bytes, 28), Get(bytes, 32)};
    std::copy_n(bytes.begin() + 36, result.source_identity.size(), result.source_identity.begin());
    ValidateShape(result.description, count);
    result.sections.reserve(count); result.section_products.reserve(product_count);
    std::vector<bool> used(product_count);
    const auto indices = kHeader + std::size_t(count) * kStride;
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto offset = kHeader + i * kStride;
        const auto value = [&](std::size_t index) { return std::bit_cast<float>(Get(bytes, offset + index * 4)); };
        TerrainSurfaceSection s{value(0), value(1), value(2), value(3), value(4), value(5),
            Get(bytes, offset + 24), Get(bytes, offset + 28)};
        ValidateSection(s);
        result.sections.push_back(s);
    }
    for (std::size_t i = 0; i < product_count; ++i)
    {
        const auto index = Get(bytes, indices + i * 4);
        Require(index < product_count && !used[index], "invalid section product index");
        used[index] = true;
        result.section_products.push_back(package.dependencies[index]);
    }
    return result;
}
} // namespace hs::render_data
