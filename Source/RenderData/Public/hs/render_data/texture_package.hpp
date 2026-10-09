#pragma once

#include <hs/render_data/render_asset_format.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace hs::render_data
{
inline constexpr std::uint32_t kTextureFormatBc4 = 80;       // DXGI_FORMAT_BC4_UNORM
inline constexpr std::uint32_t kTextureFormatBc5 = 83;       // DXGI_FORMAT_BC5_UNORM
inline constexpr std::uint32_t kTextureFormatBc7 = 98;       // DXGI_FORMAT_BC7_UNORM
inline constexpr std::uint32_t kTextureFormatBc7Srgb = 99;   // DXGI_FORMAT_BC7_UNORM_SRGB
inline constexpr std::uint32_t kTextureFormatRgba8 = 28;     // DXGI_FORMAT_R8G8B8A8_UNORM
inline constexpr std::uint32_t kTextureFormatRgba8Srgb = 29; // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
inline constexpr std::uint32_t kTextureFormatR8 = 61;        // DXGI_FORMAT_R8_UNORM
inline constexpr std::uint32_t kTextureFormatRg8Snorm = 51;  // DXGI_FORMAT_R8G8_SNORM
inline constexpr std::uint32_t kTextureFormatR16Float = 54;  // DXGI_FORMAT_R16_FLOAT
inline constexpr std::uint32_t kTextureFormatRgba16Float = 10; // DXGI_FORMAT_R16G16B16A16_FLOAT
inline constexpr std::uint32_t kTextureFormatRgba32Float = 2; // DXGI_FORMAT_R32G32B32A32_FLOAT

struct TextureDescription
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t array_size{};
    std::uint32_t mip_count{};
    std::uint32_t format{kTextureFormatBc7Srgb};
};

struct TextureSubresourceView
{
    std::uint32_t array_slice{};
    std::uint32_t mip_level{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t row_pitch{};
    std::uint32_t slice_pitch{};
    std::span<const std::byte> bytes;
};

struct TexturePackageView
{
    TextureDescription description;
    std::vector<std::string> dependencies;
    std::vector<TextureSubresourceView> subresources;
};

// TextureMetadata v1: 40 little-endian bytes (TXM1, version, width, height,
// DXGI format, array size, mip count, record count, payload byte count), then
// 16 bytes per record (uint64 offset, uint32 row pitch, uint32 slice pitch).
// TextureSubresources is tightly packed BC4, BC5, BC7, RGBA8, R8, RG8_SNORM,
// R16_FLOAT, RGBA16_FLOAT, or RGBA32_FLOAT texels in
// array-slice-major, mip-minor order. No DDS header is stored. The returned pitches are source
// pitches; a D3D12 consumer may copy each row into its aligned upload footprint.
[[nodiscard]] std::vector<std::byte> BuildTexturePackage(
    AssetId asset_id, std::span<const std::string> dependencies,
    TextureDescription description, std::span<const std::byte> payload);

// Every subresource span refers to caller-owned package bytes. Keep those bytes
// alive and unmodified while using the returned view.
[[nodiscard]] TexturePackageView ReadTexturePackage(
    std::span<const std::byte> package_bytes, AssetId expected_asset_id);
} // namespace hs::render_data
