#pragma once

#include <hs/render_data/texture_package.hpp>
#include <hs/render_data/volume_texture_package.hpp>

#include <array>
#include <limits>

namespace hs::render_data
{
enum class TextureDimension : std::uint32_t { Texture2DArray, Texture3D };

struct TextureHandle
{
    std::uint32_t value{std::numeric_limits<std::uint32_t>::max()};
    explicit operator bool() const noexcept { return value != std::numeric_limits<std::uint32_t>::max(); }
};

struct TextureSubresourceLayout
{
    std::uint64_t offset{}; // Relative to the authenticated product bytes.
    std::uint32_t row_pitch{}, slice_pitch{};
    std::uint32_t byte_count{};
    std::array<std::byte, 32> digest{}; // TXM2 2D subresource; volume descriptors have no range digest.
    bool has_digest{};
};

// Generated constants refer to other generated arrays; object layouts are never serialized.
struct TextureDescriptor
{
    AssetId asset_id;
    std::uint32_t bulk_index{};
    TextureDescription description;
    std::span<const AssetId> dependencies;
    std::span<const TextureSubresourceLayout> subresources;
    TextureDimension dimension{TextureDimension::Texture2DArray};
    std::uint32_t depth{1};
};

// Used by the content linker and development admission, not the shipping reader.
struct TextureDescriptorStorage
{
    AssetId asset_id;
    std::uint32_t bulk_index{};
    TextureDescription description;
    std::vector<AssetId> dependencies;
    std::vector<TextureSubresourceLayout> subresources;
    TextureDimension dimension{TextureDimension::Texture2DArray};
    std::uint32_t depth{1};
    [[nodiscard]] TextureDescriptor View() const noexcept
    { return {asset_id, bulk_index, description, dependencies, subresources, dimension, depth}; }
};

[[nodiscard]] TextureDescriptorStorage CompileTextureDescriptor(
    std::span<const std::byte> bytes, AssetId asset_id, std::uint32_t bulk_index);
// Only establishes bounded shape/ranges from a leased package prefix and its
// small metadata chunk. Complete product admission is required before binding bytes.
[[nodiscard]] TextureDescriptorStorage CompileTextureMetadataDescriptor(
    const PackageDirectory &directory, std::span<const std::byte> metadata,
    std::uint32_t bulk_index, std::uint64_t product_bytes);
[[nodiscard]] TextureDescriptorStorage CompileVolumeTextureMetadataDescriptor(
    const PackageDirectory &directory, std::span<const std::byte> metadata,
    std::uint32_t bulk_index, std::uint64_t product_bytes);

// Bind already authenticated bytes to admitted, compiled metadata without parsing a package.
// The returned subresources borrow bytes. Dependencies stay as typed IDs in the descriptor.
[[nodiscard]] TexturePackageView BindTextureDescriptor(
    std::span<const std::byte> bytes, const TextureDescriptor &descriptor);
void ValidateTextureDescriptor(const TextureDescriptor &, std::uint64_t product_bytes);
[[nodiscard]] VolumeTexturePackageView BindVolumeTextureDescriptor(
    std::span<const std::byte> bytes, const TextureDescriptor &descriptor);
void ValidateVolumeTextureDescriptor(const TextureDescriptor &, std::uint64_t product_bytes);
} // namespace hs::render_data
