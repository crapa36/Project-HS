#pragma once

#include <hs/core/result.hpp>
#include <hs/render_data/character_descriptor.hpp>
#include <hs/render_data/render_revision.hpp>
#include <hs/render_data/vfx_mesh_descriptor.hpp>
#include <deque>
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

struct RendererCatalogData
{
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

    [[nodiscard]] RendererCatalogView View() const noexcept
    { return {bulks, textures, geometry, materials, characters, animations,
              has_atlas ? &atlas : nullptr, source_revisions, revision_bulks}; }
};

[[nodiscard]] std::vector<std::byte> EncodeRendererCatalog(RendererCatalogView);
[[nodiscard]] Result DecodeRendererCatalog(std::span<const std::byte>, std::uint64_t pack_size,
                                            RendererCatalogData &);
}
