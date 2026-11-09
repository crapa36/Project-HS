#pragma once

#include <hs/core/result.hpp>
#include <hs/render_data/character_descriptor.hpp>
#include <hs/render_data/render_revision.hpp>
#include <hs/render_data/vfx_mesh_descriptor.hpp>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

namespace hs::render_data
{
struct RendererCatalogView
{
    std::span<const RendererBulkDescriptor> bulks;
    std::span<const TextureDescriptor> textures;
    std::span<const GeometryDescriptor> geometry;
    std::span<const MaterialDescriptor> materials;
    std::span<const CharacterDescriptor> characters;
    std::span<const AnimationDescriptor> animations;
    const VfxMeshAtlasDescriptor *atlas{};
    std::span<const RendererSourceRevision> source_revisions;
    std::span<const RendererRevisionBulk> revision_bulks;
};

class RendererCatalogData
{
public:
    ~RendererCatalogData() = default;
    RendererCatalogData(const RendererCatalogData &) = delete;
    RendererCatalogData &operator=(const RendererCatalogData &) = delete;
    [[nodiscard]] RendererCatalogView View() const noexcept
    { return {bulks, textures, geometry, materials, characters, animations,
              has_atlas ? &atlas : nullptr, source_revisions, revision_bulks}; }
    [[nodiscard]] std::uint64_t PackSize() const noexcept { return pack_size_; }
private:
    RendererCatalogData() = default;
    friend Result DecodeRendererCatalog(std::span<const std::byte>, std::uint64_t,
                                       std::shared_ptr<const RendererCatalogData> &);
    std::uint64_t pack_size_{};
    std::vector<RendererBulkDescriptor> bulks;
    std::vector<TextureDescriptorStorage> texture_storage;
    std::vector<GeometryDescriptorStorage> geometry_storage;
    std::vector<MaterialDescriptorStorage> material_storage;
    std::vector<CharacterDescriptorStorage> character_storage;
    std::vector<AnimationDescriptorStorage> animation_storage;
    std::vector<TextureDescriptor> textures;
    std::vector<GeometryDescriptor> geometry;
    std::vector<MaterialDescriptor> materials;
    std::vector<CharacterDescriptor> characters;
    std::vector<AnimationDescriptor> animations;
    VfxMeshAtlasDescriptor atlas{};
    bool has_atlas{};
    std::deque<std::string> revision_strings;
    std::vector<RendererSourceRevision> source_revisions;
    std::vector<RendererRevisionBulk> revision_bulks;

};

[[nodiscard]] std::vector<std::byte> EncodeRendererCatalog(RendererCatalogView);
// No pack bound for loose admitted metadata; a wire catalogue always supplies its pack size.
[[nodiscard]] Result ValidateRendererCatalog(RendererCatalogView, std::optional<std::uint64_t> pack_size);
[[nodiscard]] Result DecodeRendererCatalog(std::span<const std::byte>, std::uint64_t pack_size,
                                            std::shared_ptr<const RendererCatalogData> &);
}
