#include "../Source/Apps/ContentLinker/renderer_identity.hpp"

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
using namespace hs;
using namespace hs::render_data;

void Check(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename Encode>
std::vector<std::byte> Bytes(Encode encode)
{
    CanonicalBytes bytes("shipping-renderer-identity-fixture", 1);
    encode(bytes);
    return std::move(bytes).Take();
}
} // namespace

void TestShippingRendererIdentity()
{
    std::array<TextureSubresourceLayout, 1> texture_layouts_a{}, texture_layouts_b{};
    std::memset(&texture_layouts_a[0], 0xa5, sizeof(TextureSubresourceLayout));
    std::memset(&texture_layouts_b[0], 0x5a, sizeof(TextureSubresourceLayout));
    const auto set_layout = [](TextureSubresourceLayout &layout) {
        layout.offset = 128;
        layout.row_pitch = 16;
        layout.slice_pitch = 64;
        layout.byte_count = 64;
        layout.digest.fill(std::byte{0x3c});
        layout.has_digest = true;
    };
    set_layout(texture_layouts_a[0]);
    set_layout(texture_layouts_b[0]);
    const std::array<AssetId, 2> texture_dependencies{{{11}, {22}}};
    TextureDescriptor texture{};
    texture.asset_id = {7};
    texture.bulk_index = 2;
    texture.description = {8, 4, 1, 1, 98};
    texture.dependencies = texture_dependencies;
    texture.subresources = texture_layouts_a;
    const auto texture_bytes = Bytes([&](CanonicalBytes &b) { content_identity::Texture(b, texture); });
    texture.subresources = texture_layouts_b;
    Check(texture_bytes == Bytes([&](CanonicalBytes &b) { content_identity::Texture(b, texture); }),
          "texture identity ignores TextureSubresourceLayout object padding");
    std::array<AssetId, 2> reordered_texture_dependencies{{{22}, {11}}};
    texture.dependencies = reordered_texture_dependencies;
    Check(texture_bytes != Bytes([&](CanonicalBytes &b) { content_identity::Texture(b, texture); }),
          "texture identity preserves dependency order");

    std::array<MaterialParameterSlot, 1> defaults{};
    defaults[0].words[0] = 0x1234;
    std::array<MaterialProgramDescriptor, 1> programs{};
    programs[0].bulk_index = 3;
    programs[0].asset_id = {33};
    programs[0].product_id = "program-ab";
    MaterialDescriptor material{};
    material.asset_id = {44};
    material.bulk_index = 5;
    material.defaults = defaults;
    material.programs = programs;
    const auto material_bytes = Bytes([&](CanonicalBytes &b) { content_identity::Material(b, material); });
    programs[0].product_id = "program-a";
    Check(material_bytes != Bytes([&](CanonicalBytes &b) { content_identity::Material(b, material); }),
          "material identity includes length-prefixed program product IDs");

    const std::array<GeometrySubmesh, 2> submeshes{{{0, 3, 0, 1, 2}, {3, 6, 1, 2, 4}}};
    GeometryDescriptor geometry{};
    geometry.asset_id = {55};
    geometry.bulk_index = 6;
    geometry.vertex_count = 9;
    geometry.chunks[0] = {256, 108};
    geometry.submeshes = submeshes;
    const auto geometry_bytes = Bytes([&](CanonicalBytes &b) { content_identity::Geometry(b, geometry); });
    const std::array<GeometrySubmesh, 2> reordered_submeshes{{submeshes[1], submeshes[0]}};
    geometry.submeshes = reordered_submeshes;
    Check(geometry_bytes != Bytes([&](CanonicalBytes &b) { content_identity::Geometry(b, geometry); }),
          "geometry identity preserves indexed submesh order");

    const std::array<std::uint16_t, 2> parents{{0, 1}};
    CharacterDescriptor character{};
    character.asset_id = {66};
    character.bulk_index = 7;
    character.parents = parents;
    character.bone_count = 2;
    const auto character_bytes = Bytes([&](CanonicalBytes &b) { content_identity::Character(b, character); });
    const std::array<std::uint16_t, 2> reordered_parents{{1, 0}};
    character.parents = reordered_parents;
    Check(character_bytes != Bytes([&](CanonicalBytes &b) { content_identity::Character(b, character); }),
          "character identity preserves skeleton parent order");

    const std::array<std::string_view, 2> bone_names{{"root", "arm"}};
    std::array<AnimationClipDescriptor, 1> clips{};
    clips[0].id = {77};
    clips[0].name = "walk";
    clips[0].frame_count = 2;
    clips[0].duration_seconds = 1.0f;
    AnimationDescriptor animation{};
    animation.asset_id = {77};
    animation.bone_names = bone_names;
    animation.clips = clips;
    const auto animation_bytes = Bytes([&](CanonicalBytes &b) { content_identity::Animation(b, animation); });
    clips[0].name = "wal";
    Check(animation_bytes != Bytes([&](CanonicalBytes &b) { content_identity::Animation(b, animation); }),
          "animation identity includes length-prefixed clip names");
    const std::array<std::string_view, 2> reordered_bones{{"arm", "root"}};
    animation.bone_names = reordered_bones;
    clips[0].name = "walk";
    Check(animation_bytes != Bytes([&](CanonicalBytes &b) { content_identity::Animation(b, animation); }),
          "animation identity preserves bone index order");

    BulkRange bulk{};
    bulk.offset = 512;
    bulk.byte_count = 64;
    bulk.format = RenderBulkFormat::DdsTexture;
    bulk.expected_digest[0] = std::byte{0x91};
    const auto bulk_bytes = Bytes([&](CanonicalBytes &b) { content_identity::Bulk(b, bulk); });
    bulk.expected_digest[0] = std::byte{0x92};
    Check(bulk_bytes != Bytes([&](CanonicalBytes &b) { content_identity::Bulk(b, bulk); }),
          "bulk identity includes its expected digest");

    VfxMeshAtlasDescriptor atlas{};
    atlas.asset_id = {88};
    atlas.meshes[0].vertex_count = 3;
    atlas.meshes[1].vertex_count = 6;
    const auto atlas_bytes = Bytes([&](CanonicalBytes &b) { content_identity::Atlas(b, atlas); });
    std::swap(atlas.meshes[0], atlas.meshes[1]);
    Check(atlas_bytes != Bytes([&](CanonicalBytes &b) { content_identity::Atlas(b, atlas); }),
          "atlas identity preserves mesh table order");
}
