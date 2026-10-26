#pragma once

#include <hs/render_data/render_asset_format.hpp>
#include <hs/render_data/texture_package.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace hs::render_data
{
// TXV1 is deliberately separate from the established TXM1 2D metadata ABI.
// The volume package is currently restricted to authored RGBA8 data.
struct VolumeTextureDescription
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t depth{};
    std::uint32_t mip_count{};
    std::uint32_t format{kTextureFormatRgba8};
};

struct VolumeTextureSubresourceView
{
    std::uint32_t mip_level{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t depth{};
    std::uint32_t row_pitch{};
    std::uint32_t slice_pitch{};
    std::uint32_t volume_pitch{};
    std::span<const std::byte> bytes;
};

struct VolumeTexturePackageView
{
    VolumeTextureDescription description;
    std::vector<std::string> dependencies;
    std::vector<VolumeTextureSubresourceView> subresources;
};

// The payload is tightly packed mip-major RGBA8 volume data. Each mip's
// metadata records the exact row, depth-slice, and complete-volume pitches.
[[nodiscard]] std::vector<std::byte> BuildVolumeTexturePackage(
    AssetId asset_id, std::span<const std::string> dependencies,
    VolumeTextureDescription description, std::span<const std::byte> payload);

// Every returned span refers to caller-owned package bytes.
[[nodiscard]] VolumeTexturePackageView ReadVolumeTexturePackage(
    std::span<const std::byte> package_bytes, AssetId expected_asset_id);
[[nodiscard]] VolumeTexturePackageView ReadVolumeTexturePackage(const PackageView &package);
} // namespace hs::render_data
