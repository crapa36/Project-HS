#pragma once

#include <hs/core/result.hpp>
#include <hs/render_data/render_bulk.hpp>
#include <hs/render_data/texture_descriptor.hpp>
#include <hs/render_data/geometry_descriptor.hpp>
#include <hs/render_data/material_descriptor.hpp>
#include <hs/render_data/character_descriptor.hpp>
#include <hs/render_data/render_revision.hpp>
#include <hs/render_data/vfx_mesh_descriptor.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace hs
{
class ShippedContentOwner;
namespace shipping
{
// Emitted together with the immutable shipping image. The compiled header is
// trusted; startup compares its exact bytes without parsing domain content.
struct ShippingImageInfo
{
    std::array<std::byte, 64> header;
    const wchar_t *relative_path;
    std::uint64_t content_hash;
    std::uint64_t font_offset{}, font_bytes{};
    std::array<std::byte,32> font_digest{};
};
[[nodiscard]] const ShippingImageInfo &ImageInfo() noexcept;
[[nodiscard]] std::span<const render_data::RendererBulkDescriptor> RendererBulks() noexcept;
[[nodiscard]] std::span<const render_data::TextureDescriptor> RendererTextures() noexcept;
[[nodiscard]] std::span<const render_data::GeometryDescriptor> RendererGeometry() noexcept;
[[nodiscard]] std::span<const render_data::MaterialDescriptor> RendererMaterials() noexcept;
[[nodiscard]] std::span<const render_data::CharacterDescriptor> RendererCharacters() noexcept;
[[nodiscard]] std::span<const render_data::AnimationDescriptor> RendererAnimations() noexcept;
[[nodiscard]] const render_data::VfxMeshAtlasDescriptor &RendererVfxMeshAtlas() noexcept;
[[nodiscard]] std::span<const render_data::RendererSourceRevision> RendererSourceRevisions() noexcept;
[[nodiscard]] std::span<const render_data::RendererRevisionBulk> RendererRevisionBulks() noexcept;
// Bounded startup read; failure preserves the caller buffer.
[[nodiscard]] Result ReadFont(const ShippedContentOwner &, const ShippingImageInfo &, std::vector<std::byte> &);
// Failure leaves output attached to its previously opened file.
[[nodiscard]] Result OpenImage(const std::filesystem::path &directory,
                               const ShippingImageInfo &expected, ShippedContentOwner &output);
} // namespace shipping
} // namespace hs
