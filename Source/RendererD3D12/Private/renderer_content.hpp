#pragma once

#include "renderer_asset_source.hpp"
#include "renderer_development_catalog.hpp"
#include "renderer_packed_assets.hpp"
#include <hs/renderer/renderer.hpp>
#include <hs/render_data/renderer_catalog.hpp>

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

inline Result ValidatePackedVfxMeshCatalog(const RendererConfig &config)
{
    if (!config.packed_vfx_mesh_atlas ||
        config.packed_vfx_mesh_atlas->asset_id.value != MakeAssetId("vfx/vfx_mesh_atlas.hsbin").value)
        return Result::Failure(ErrorCode::InvalidArgument, "RendererPack", "Compiled VFX atlas identity is absent.");
    return Result::Success();
}

inline Result ValidatePackedCatalog(const RendererConfig &config)
{
    const render_data::RendererCatalogView view{config.packed_bulks, config.packed_textures,
        config.packed_geometry, config.packed_materials, config.packed_characters,
        config.packed_animations, config.packed_vfx_mesh_atlas,
        config.packed_source_revisions, config.packed_revision_bulks};
    if (config.packed_content && (!config.packed_content->Identity() || view.bulks.empty()))
        return Result::Failure(ErrorCode::InvalidArgument, "RendererPack", "Pack owner or catalogue is absent.");
    const auto same = [](auto a, auto b) { return a.size() == b.size() && (a.empty() || a.data() == b.data()); };
    const auto matches = [&](const render_data::RendererCatalogView &prepared) {
        return same(view.bulks,prepared.bulks) && same(view.textures,prepared.textures) &&
            same(view.geometry,prepared.geometry) && same(view.materials,prepared.materials) &&
            same(view.characters,prepared.characters) && same(view.animations,prepared.animations) &&
            view.atlas == prepared.atlas && same(view.source_revisions,prepared.source_revisions) &&
            same(view.revision_bulks,prepared.revision_bulks);
    };
    // Both factories own immutable metadata; only owner/binding identity remains dynamic.
    if (config.prepared_renderer_catalog)
    {
        if (config.development_catalog || !config.packed_content ||
            config.packed_content->Size() != config.prepared_renderer_catalog->PackSize() ||
            !matches(config.prepared_renderer_catalog->View()))
            return Result::Failure(ErrorCode::InvalidArgument,"RendererPack","Prepared catalogue owner or bindings differ.");
        return Result::Success();
    }
    if (config.development_catalog)
    {
        if (config.packed_content || !matches(config.development_catalog->View()))
            return Result::Failure(ErrorCode::InvalidArgument,"RendererPack","Development catalogue owner or bindings differ.");
        return Result::Success();
    }
    const auto valid = render_data::ValidateRendererCatalog(view,
        config.packed_content ? std::optional{config.packed_content->Size()} : std::nullopt);
    return valid ? Result::Success() : Result::Failure(ErrorCode::InvalidArgument,"RendererPack",std::string(valid.Message()));
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

inline render_data::GeometryHandle FindRendererGeometry(const RendererConfig &config, AssetId asset)
{
    const auto entries = config.packed_geometry;
    const auto found = std::lower_bound(entries.begin(), entries.end(), asset.value,
        [](const auto &entry, std::uint64_t value) { return entry.asset_id.value < value; });
    if (found == entries.end() || found->asset_id.value != asset.value) return {};
    return {static_cast<std::uint32_t>(found - entries.begin())};
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

inline const render_data::RendererBulkDescriptor *FindCanonicalRendererBulk(const RendererConfig &config, std::string_view id)
{
    const auto entries = config.packed_bulks;
    const auto asset = MakeAssetId(id).value;
    const auto found = std::lower_bound(entries.begin(), entries.end(), asset,
        [](const auto &entry, std::uint64_t value) { return entry.asset_id.value < value; });
    return found != entries.end() && found->asset_id.value == asset ? &*found : nullptr;
}

inline std::string EffectiveRendererProductId(const RendererConfig &config, std::string_view id)
{
    if (config.selected_texture_feedback && id.starts_with("render/shader/") && id.ends_with(".dxil") &&
        !id.ends_with("_feedback.dxil"))
    {
        auto feedback = std::string(id.substr(0, id.size() - 5)) + "_feedback.dxil";
        if (config.packed_content ? FindCanonicalRendererBulk(config, feedback) != nullptr :
            config.product_paths.contains(feedback)) return feedback;
    }
    return std::string(id);
}

inline const render_data::RendererBulkDescriptor *FindRendererBulk(const RendererConfig &config, std::string_view id)
{
    return FindCanonicalRendererBulk(config, EffectiveRendererProductId(config, id));
}

inline Result ResolveRendererAsset(const RendererConfig &config, std::string_view id, RendererAssetSource &output,
    bool select_feedback = true)
{
    const auto logical = select_feedback ? EffectiveRendererProductId(config, id) : std::string(id);
    RendererAssetSource source;
    if (config.packed_content)
    {
        const auto *bulk = FindCanonicalRendererBulk(config, logical);
        if (!bulk)
            return Result::Failure(ErrorCode::InvalidState, "RendererPack", "Missing compiled renderer bulk: " + std::string(id));
        source = RendererAssetSource::Packed(config.packed_content, bulk->range);
    }
    else
    {
        std::filesystem::path path;
        if (auto result = config.ResolveProduct(logical, path); !result) return result;
        std::optional<LooseRendererAsset::WholeFileProof> proof;
        if (config.development_catalog)
        {
            const auto asset = MakeAssetId(logical).value;
            const auto found = std::lower_bound(config.packed_bulks.begin(), config.packed_bulks.end(), asset,
                [](const auto &entry, std::uint64_t value) { return entry.asset_id.value < value; });
            if (found != config.packed_bulks.end() && found->asset_id.value == asset)
                if (found->range.byte_count)
                    proof = LooseRendererAsset::WholeFileProof{
                        found->range.byte_count, found->range.expected_digest};
        }
        source = RendererAssetSource::Loose(std::move(path), std::move(proof));
    }
    output = std::move(source);
    return Result::Success();
}

inline bool RendererIsSourceRevision(const RendererConfig &config, std::string_view id)
{
    const auto asset = MakeAssetId(id).value;
    if (!asset) return false;
    if (config.packed_content)
    {
        const auto entries = config.packed_source_revisions;
        const auto found = std::lower_bound(entries.begin(), entries.end(), asset,
            [](const auto &entry, std::uint64_t value) { return entry.asset_id.value < value; });
        return found != entries.end() && found->asset_id.value == asset && found->product_id == id;
    }
    return config.development_catalog && config.development_catalog->HasSourceRevision(id);
}

inline bool RendererIsSourceRevision(const RendererConfig &config, AssetId asset)
{
    if (!asset.value) return false;
    if (config.packed_content)
    {
        const auto entries = config.packed_source_revisions;
        const auto found = std::lower_bound(entries.begin(), entries.end(), asset.value,
            [](const auto &entry, std::uint64_t value) { return entry.asset_id.value < value; });
        return found != entries.end() && found->asset_id.value == asset.value;
    }
    return config.development_catalog && config.development_catalog->HasSourceRevision(asset);
}

inline bool RendererIsAdmittedRuntimeProduct(const RendererConfig &config, std::string_view id)
{
    const auto asset = MakeAssetId(id);
    if (!asset.value || RendererIsSourceRevision(config, id)) return false;
    if (config.packed_content) return FindRendererBulk(config, id) != nullptr;
    if (config.development_catalog) return config.development_catalog->HasRuntimeProduct(asset);
    // Without an admitted runtime or provenance table, preserve the legacy leased
    // Generation behavior: ResolveProduct and its digest map authenticate the edge.
    return config.product_paths.contains(std::string(id));
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
        source = RendererAssetSource::Packed(config.packed_content, bulk->range);
    }
    else if (auto result = ResolveRendererAsset(config, program.product_id, source); !result) return result;
    return source.Read(output);
}

inline std::string RendererProductDigest(const RendererConfig &config, std::string_view id, bool select_feedback = true)
{
    const auto logical = select_feedback ? EffectiveRendererProductId(config, id) : std::string(id);
    if (config.packed_content)
    {
        const auto *bulk = FindCanonicalRendererBulk(config, logical);
        const std::array<std::byte, 32> *expected = bulk ? &bulk->range.expected_digest : nullptr;
        if (!bulk)
        {
            const auto asset = MakeAssetId(logical).value;
            const auto entries = config.packed_source_revisions;
            const auto found = std::lower_bound(entries.begin(), entries.end(), asset,
                [](const auto &entry, std::uint64_t value) { return entry.asset_id.value < value; });
            if (found == entries.end() || found->asset_id.value != asset || found->product_id != logical) return {};
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
    const auto found = config.product_digests.find(logical);
    return found == config.product_digests.end() ? std::string{} : found->second;
}

} // namespace hs
