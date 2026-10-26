#pragma once

#include "renderer_asset_source.hpp"
#include "renderer_development_catalog.hpp"
#include "renderer_packed_assets.hpp"
#include <hs/renderer/renderer.hpp>

#include <cmath>
#include <stdexcept>

namespace hs
{
inline render_data::CharacterHandle FindRendererCharacter(const RendererConfig &config, AssetId asset)
{
    const auto entries = config.packed_characters;
    const auto found = std::lower_bound(entries.begin(), entries.end(), asset.value,
        [](const auto &entry, std::uint64_t value) { return entry.asset_id.value < value; });
    if (found == entries.end() || found->asset_id.value != asset.value) return {};
    return {static_cast<std::uint32_t>(found - entries.begin())};
}

inline const render_data::AnimationDescriptor *FindRendererAnimation(const RendererConfig &config, AssetId asset)
{
    const auto entries = config.packed_animations;
    const auto found = std::lower_bound(entries.begin(), entries.end(), asset.value,
        [](const auto &entry, std::uint64_t value) { return entry.asset_id.value < value; });
    return found == entries.end() || found->asset_id.value != asset.value ? nullptr : &*found;
}

inline Result ValidatePackedCharacterCatalog(const RendererConfig &config)
{
    try
    {
        if (config.packed_characters.size() > UINT32_MAX || config.packed_animations.size() > UINT32_MAX)
            throw std::runtime_error("Character or animation table exceeds its handle ABI.");
        std::uint64_t previous{};
        for (const auto &character : config.packed_characters)
        {
            if (!character.asset_id.value || character.asset_id.value <= previous)
                throw std::runtime_error("Compiled character table has invalid IDs.");
            render_data::ValidateCharacterDescriptor(character, config.packed_bulks, config.packed_geometry, config.packed_materials);
            previous = character.asset_id.value;
        }
        previous = 0;
        for (const auto &animation : config.packed_animations)
        {
            if (!animation.asset_id.value || animation.asset_id.value <= previous)
                throw std::runtime_error("Compiled animation table has invalid IDs.");
            render_data::ValidateAnimationDescriptor(animation, config.packed_bulks, config.packed_characters);
            previous = animation.asset_id.value;
        }
        return Result::Success();
    }
    catch (const std::exception &error)
    { return Result::Failure(ErrorCode::InvalidArgument, "RendererPack", error.what()); }
}

inline Result ValidatePackedVfxMeshCatalog(const RendererConfig &config)
{
    const auto *atlas = config.packed_vfx_mesh_atlas;
    const auto asset = MakeAssetId("vfx/vfx_mesh_atlas.hsbin");
    const auto bulk = std::lower_bound(config.packed_bulks.begin(), config.packed_bulks.end(), asset.value,
        [](const auto &entry, std::uint64_t value) { return entry.asset_id.value < value; });
    if (!atlas || atlas->asset_id.value != asset.value || bulk == config.packed_bulks.end() ||
        bulk->asset_id.value != asset.value || bulk->range.format != render_data::RenderBulkFormat::VfxMeshAtlas)
        return Result::Failure(ErrorCode::InvalidArgument, "RendererPack", "Compiled VFX atlas identity or bulk is absent.");
    std::uint64_t vertices{};
    for (const auto &mesh : atlas->meshes)
    {
        if (!mesh.vertex_count || mesh.vertex_count % 3 || !std::isfinite(mesh.transverse_radius) ||
            mesh.transverse_radius <= 0 || !std::isfinite(mesh.minimum_z) ||
            !std::isfinite(mesh.widest_z) || !std::isfinite(mesh.maximum_z) ||
            mesh.minimum_z >= mesh.maximum_z || mesh.widest_z < mesh.minimum_z ||
            mesh.widest_z > mesh.maximum_z)
            return Result::Failure(ErrorCode::InvalidArgument, "RendererPack", "Compiled VFX atlas mesh bounds are invalid.");
        vertices += mesh.vertex_count;
    }
    if (vertices > (64ull * 1024 * 1024 / 4 - 25) / 12 ||
        bulk->range.byte_count != 4 * (25 + vertices * 12))
        return Result::Failure(ErrorCode::InvalidArgument, "RendererPack", "Compiled VFX atlas size differs from mesh table.");
    return Result::Success();
}

inline render_data::MaterialHandle FindRendererMaterial(const RendererConfig &config, AssetId asset)
{
    const auto entries = config.packed_materials;
    const auto found = std::lower_bound(entries.begin(), entries.end(), asset.value,
        [](const auto &entry, std::uint64_t value) { return entry.asset_id.value < value; });
    if (found == entries.end() || found->asset_id.value != asset.value) return {};
    return {static_cast<std::uint32_t>(found - entries.begin())};
}

inline std::string MaterialLayoutDigest(const render_data::MaterialDescriptor &material)
{
    return render_data::DecodeMaterialParameterLayout(material.parameter_layout);
}

inline Result ValidatePackedMaterialCatalog(const RendererConfig &config)
{
    try
    {
        std::vector<AssetId> bulk_assets;
        bulk_assets.reserve(config.packed_bulks.size());
        for (const auto &bulk : config.packed_bulks) bulk_assets.push_back(bulk.asset_id);
        std::uint64_t previous{};
        if (config.packed_materials.size() > UINT32_MAX) throw std::runtime_error("Material handle table exceeds its ABI.");
        for (const auto &material : config.packed_materials)
        {
            if (!material.asset_id.value || material.asset_id.value <= previous || material.bulk_index >= config.packed_bulks.size())
                throw std::runtime_error("Compiled material table has invalid IDs or bulk handles.");
            const auto &bulk = config.packed_bulks[material.bulk_index];
            if (bulk.asset_id.value != material.asset_id.value || bulk.range.format != render_data::RenderBulkFormat::Package)
                throw std::runtime_error("Compiled material references a different bulk product.");
            render_data::ValidateMaterialDescriptor(material, bulk_assets, config.packed_textures);
            for (const auto &program : material.programs)
                if (program.bulk_index >= config.packed_bulks.size() ||
                    config.packed_bulks[program.bulk_index].range.format != render_data::RenderBulkFormat::ShaderDxil)
                    throw std::runtime_error("Compiled material program is not a shader bulk.");
            previous = material.asset_id.value;
        }
        return Result::Success();
    }
    catch (const std::exception &error)
    { return Result::Failure(ErrorCode::InvalidArgument, "RendererPack", error.what()); }
}

inline render_data::GeometryHandle FindRendererGeometry(const RendererConfig &config, AssetId asset)
{
    const auto entries = config.packed_geometry;
    const auto found = std::lower_bound(entries.begin(), entries.end(), asset.value,
        [](const auto &entry, std::uint64_t value) { return entry.asset_id.value < value; });
    if (found == entries.end() || found->asset_id.value != asset.value) return {};
    return {static_cast<std::uint32_t>(found - entries.begin())};
}

inline Result ValidatePackedGeometryCatalog(const RendererConfig &config)
{
    try
    {
        std::uint64_t previous{};
        if (config.packed_geometry.size() > UINT32_MAX) throw std::runtime_error("Geometry handle table exceeds its ABI.");
        for (const auto &geometry : config.packed_geometry)
        {
            if (!geometry.asset_id.value || geometry.asset_id.value <= previous || geometry.bulk_index >= config.packed_bulks.size())
                throw std::runtime_error("Compiled geometry table has invalid IDs or bulk handles.");
            const auto &bulk = config.packed_bulks[geometry.bulk_index];
            if (bulk.asset_id.value != geometry.asset_id.value || bulk.range.format != render_data::RenderBulkFormat::GeometryPage)
                throw std::runtime_error("Compiled geometry references a different bulk product.");
            render_data::ValidateGeometryDescriptor(geometry, bulk.range.byte_count);
            previous = geometry.asset_id.value;
        }
        return Result::Success();
    }
    catch (const std::exception &error)
    { return Result::Failure(ErrorCode::InvalidArgument, "RendererPack", error.what()); }
}

inline render_data::TextureHandle FindRendererTexture(const RendererConfig &config, AssetId asset)
{
    const auto entries = config.packed_textures;
    const auto found = std::lower_bound(entries.begin(), entries.end(), asset.value,
        [](const auto &entry, std::uint64_t value) { return entry.asset_id.value < value; });
    if (found == entries.end() || found->asset_id.value != asset.value) return {};
    return {static_cast<std::uint32_t>(found - entries.begin())};
}

inline render_data::TexturePackageView ReadRendererTexturePackage(const RendererConfig &config,
    std::string_view id, std::span<const std::byte> bytes)
{
    const auto asset = MakeAssetId(id);
    if (config.development_catalog)
        return render_data::BindTextureDescriptor(bytes, config.development_catalog->AdmitTexture(asset, bytes));
    if (!config.packed_content) return render_data::ReadTexturePackage(bytes, asset);
    const auto handle = FindRendererTexture(config, asset);
    if (!handle) throw std::runtime_error("Missing compiled texture descriptor: " + std::string(id));
    return render_data::BindTextureDescriptor(bytes, config.packed_textures[handle.value]);
}

inline render_data::VolumeTexturePackageView ReadRendererVolumeTexturePackage(const RendererConfig &config,
    std::string_view id, std::span<const std::byte> bytes)
{
    const auto asset = MakeAssetId(id);
    if (config.development_catalog)
        return render_data::BindVolumeTextureDescriptor(bytes, config.development_catalog->AdmitTexture(asset, bytes));
    if (!config.packed_content) return render_data::ReadVolumeTexturePackage(bytes, asset);
    const auto handle = FindRendererTexture(config, asset);
    if (!handle) throw std::runtime_error("Missing compiled volume texture descriptor: " + std::string(id));
    return render_data::BindVolumeTextureDescriptor(bytes, config.packed_textures[handle.value]);
}

template <class TextureView>
inline bool RendererTextureDependencyMatches(const RendererConfig &config, std::string_view id,
    const TextureView &view, std::string_view dependency)
{
    if (config.development_catalog)
    {
        const auto descriptor = config.development_catalog->FindTexture(MakeAssetId(id));
        return descriptor && descriptor->dependencies.size() == 1 &&
            descriptor->dependencies[0].value == MakeAssetId(dependency).value;
    }
    if (!config.packed_content) return view.dependencies.size() == 1 && view.dependencies[0] == dependency;
    const auto handle = FindRendererTexture(config, MakeAssetId(id));
    if (!handle) return false;
    const auto dependencies = config.packed_textures[handle.value].dependencies;
    return dependencies.size() == 1 && dependencies[0].value == MakeAssetId(dependency).value;
}

inline Result ValidatePackedTextureCatalog(const RendererConfig &config)
{
    try
    {
        std::uint64_t previous{};
        if (config.packed_textures.size() > UINT32_MAX) throw std::runtime_error("Texture handle table exceeds its ABI.");
        for (const auto &texture : config.packed_textures)
        {
            if (!texture.asset_id.value || texture.asset_id.value <= previous || texture.bulk_index >= config.packed_bulks.size())
                throw std::runtime_error("Compiled texture table has invalid IDs or bulk handles.");
            const auto &bulk = config.packed_bulks[texture.bulk_index];
            if (bulk.asset_id.value != texture.asset_id.value || bulk.range.format != render_data::RenderBulkFormat::Package)
                throw std::runtime_error("Compiled texture references a different bulk product.");
            render_data::ValidateTextureDescriptor(texture, bulk.range.byte_count);
            previous = texture.asset_id.value;
        }
        return Result::Success();
    }
    catch (const std::exception &error)
    { return Result::Failure(ErrorCode::InvalidArgument, "RendererPack", error.what()); }
}

inline const render_data::RendererBulkDescriptor *FindRendererBulk(const RendererConfig &config, std::string_view id)
{
    const auto entries = config.packed_bulks;
    const auto find = [&](std::string_view logical) -> const render_data::RendererBulkDescriptor * {
        const auto asset = MakeAssetId(logical).value;
        const auto found = std::lower_bound(entries.begin(), entries.end(), asset,
            [](const auto &entry, std::uint64_t value) { return entry.asset_id.value < value; });
        return found != entries.end() && found->asset_id.value == asset ? &*found : nullptr;
    };
    if (config.selected_texture_feedback && id.starts_with("render/shader/") && id.ends_with(".dxil") &&
        !id.ends_with("_feedback.dxil"))
        if (const auto *feedback = find(std::string(id.substr(0, id.size() - 5)) + "_feedback.dxil")) return feedback;
    return find(id);
}

inline Result ResolveRendererAsset(const RendererConfig &config, std::string_view id, RendererAssetSource &output)
{
    RendererAssetSource source;
    if (config.packed_content)
    {
        const auto *bulk = FindRendererBulk(config, id);
        if (!bulk)
            return Result::Failure(ErrorCode::InvalidState, "RendererPack", "Missing compiled renderer bulk: " + std::string(id));
        source.pack = config.packed_content;
        source.range = bulk->range;
    }
    else
    {
        if (auto result = config.ResolveProduct(id, source.path); !result) return result;
        // Developer feedback selection can redirect a shader ID to its variant path.
        if (config.development_catalog && !id.ends_with(".dxil"))
        {
            const auto asset = MakeAssetId(id).value;
            const auto found = std::lower_bound(config.packed_bulks.begin(), config.packed_bulks.end(), asset,
                [](const auto &entry, std::uint64_t value) { return entry.asset_id.value < value; });
            if (found != config.packed_bulks.end() && found->asset_id.value == asset)
                source.range = found->range;
        }
    }
    output = std::move(source);
    return Result::Success();
}

inline Result ReadRendererMaterialProgram(const RendererConfig &config,
    const render_data::MaterialProgramDescriptor &program, std::vector<std::byte> &output)
{
    RendererAssetSource source;
    if (config.packed_content)
    {
        if (program.bulk_index >= config.packed_bulks.size())
            return Result::Failure(ErrorCode::InvalidState, "RendererPack", "Invalid material program bulk index.");
        const auto *bulk = &config.packed_bulks[program.bulk_index];
        if (!program.asset_id.value || bulk->asset_id.value != program.asset_id.value ||
            MakeAssetId(program.product_id).value != program.asset_id.value ||
            bulk->range.format != render_data::RenderBulkFormat::ShaderDxil)
            return Result::Failure(ErrorCode::InvalidState, "RendererPack", "Material program identity or format mismatch.");
        // Match the existing optional feedback substitution, after admitting the base handle.
        if (config.selected_texture_feedback)
            bulk = FindRendererBulk(config, program.product_id);
        if (!bulk || bulk->range.format != render_data::RenderBulkFormat::ShaderDxil)
            return Result::Failure(ErrorCode::InvalidState, "RendererPack", "Invalid selected material shader bulk.");
        source.pack = config.packed_content;
        source.range = bulk->range;
    }
    else if (auto result = ResolveRendererAsset(config, program.product_id, source); !result) return result;
    return source.Read(output);
}

inline std::string RendererProductDigest(const RendererConfig &config, std::string_view id)
{
    if (config.packed_content)
    {
        const auto *bulk = FindRendererBulk(config, id);
        const std::array<std::byte, 32> *expected = bulk ? &bulk->range.expected_digest : nullptr;
        if (!bulk)
        {
            const auto asset = MakeAssetId(id).value;
            const auto entries = config.packed_source_revisions;
            const auto found = std::lower_bound(entries.begin(), entries.end(), asset,
                [](const auto &entry, std::uint64_t value) { return entry.asset_id.value < value; });
            if (found == entries.end() || found->asset_id.value != asset || found->product_id != id) return {};
            expected = &found->digest;
        }
        constexpr char hex[] = "0123456789abcdef";
        std::string digest = "sha256:";
        digest.reserve(71);
        for (const auto byte : *expected)
        {
            const auto value = std::to_integer<unsigned>(byte);
            digest += hex[value >> 4];
            digest += hex[value & 15];
        }
        return digest;
    }
    const auto found = config.product_digests.find(std::string(id));
    return found == config.product_digests.end() ? std::string{} : found->second;
}

inline Result ValidatePackedRevisionCatalog(const RendererConfig &config)
{
    try
    {
        std::uint64_t previous{};
        for (const auto &source : config.packed_source_revisions)
        {
            const auto bulk = std::lower_bound(config.packed_bulks.begin(), config.packed_bulks.end(), source.asset_id.value,
                [](const auto &entry, std::uint64_t value) { return entry.asset_id.value < value; });
            if (source.product_id.empty() || !source.asset_id.value || source.asset_id.value <= previous ||
                MakeAssetId(source.product_id).value != source.asset_id.value ||
                (bulk != config.packed_bulks.end() && bulk->asset_id.value == source.asset_id.value))
                throw std::runtime_error("Compiled source revision has invalid name, identity, ordering or bulk overlap.");
            previous = source.asset_id.value;
        }
        if (config.packed_revision_bulks.size() > config.packed_bulks.size())
            throw std::runtime_error("Compiled bulk revisions exceed the bulk catalog.");
        std::vector<bool> seen(config.packed_bulks.size());
        for (const auto &entry : config.packed_revision_bulks)
        {
            if (entry.product_id.empty() || !render_data::IsRendererMaterialRevisionProduct(entry.product_id) ||
                entry.bulk_index >= config.packed_bulks.size() || seen[entry.bulk_index])
                throw std::runtime_error("Compiled bulk revision has an invalid name, predicate or reference.");
            const auto &bulk = config.packed_bulks[entry.bulk_index];
            using F = render_data::RenderBulkFormat;
            const auto id = entry.product_id;
            const auto format = id.ends_with(".rpkg") ? F::Package : id.ends_with(".rpage") ? F::GeometryPage :
                id.ends_with(".ghbin") ? F::GeometryHierarchy : id.ends_with(".shl1") ? F::SceneHlod :
                id.ends_with(".dds") ? F::DdsTexture : F::ShaderDxil;
            if (MakeAssetId(id).value != bulk.asset_id.value || bulk.range.format != format ||
                (format == F::ShaderDxil && !id.ends_with(".dxil")))
                throw std::runtime_error("Compiled bulk revision identity or format differs from its product.");
            seen[entry.bulk_index] = true;
        }
        return Result::Success();
    }
    catch (const std::exception &error)
    { return Result::Failure(ErrorCode::InvalidArgument, "RendererPack", error.what()); }
}

} // namespace hs
