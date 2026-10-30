#pragma once

#include <hs/renderer/renderer.hpp>
#include <hs/render_data/texture_package.hpp>
#include <hs/render_data/surface_layout.hpp>
#include <hs/core/sha256.hpp>
#include <hs/core/cooked_format.hpp>
#include "checked_binary_read.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace hs
{
// Selected Generation owns loose bytes. Startup admits small metadata; geometry
// and texture payload semantics are admitted at their actual read boundary.
// Admit* consumes the exact bytes whose selected SHA ReadProduct/RendererAssetSource/AssetIo verified.
class RendererDevelopmentCatalog
{
    struct TextureSlot
    {
        std::mutex mutex;
        std::optional<render_data::TextureDescriptorStorage> proof;
        std::optional<render_data::TextureDescriptorStorage> value;
    };
    struct GeometrySlot
    {
        std::mutex mutex;
        std::optional<render_data::GeometryDescriptorStorage> proof;
        std::optional<render_data::GeometryDescriptor> proof_view;
        std::optional<render_data::GeometryDescriptorStorage> value;
        std::optional<render_data::GeometryDescriptor> view;
        std::uint32_t bone_limit{UINT32_MAX};
    };
    struct CharacterSlot
    {
        std::mutex mutex;
        std::optional<render_data::CharacterDescriptorStorage> value;
        std::optional<render_data::CharacterDescriptor> view;
    };
    struct AnimationSlot
    {
        std::mutex mutex;
        std::optional<render_data::AnimationDescriptorStorage> value;
        std::optional<render_data::AnimationDescriptor> view;
    };
    std::unique_ptr<TextureSlot[]> texture_slots_;
    std::unique_ptr<GeometrySlot[]> geometry_slots_;
    std::unique_ptr<CharacterSlot[]> character_slots_;
    std::unique_ptr<AnimationSlot[]> animation_slots_;
    mutable std::mutex descriptor_mutex_;
    mutable std::vector<render_data::GeometryDescriptor> geometry_views_;
    mutable std::vector<render_data::CharacterDescriptor> character_views_;
    std::vector<std::filesystem::path> paths_;
    std::vector<render_data::TextureDescriptor> texture_views_;
    std::vector<render_data::MaterialDescriptorStorage> material_storage_;
    std::vector<render_data::MaterialDescriptor> material_views_;
    std::unordered_set<std::uint64_t> closure_ids_;
    std::unordered_set<std::uint64_t> source_revision_assets_;

public:
    std::vector<render_data::RendererBulkDescriptor> bulks;

    void Bind(RendererConfig &config) const noexcept
    {
        config.packed_bulks = bulks;
        config.packed_textures = texture_views_;
        config.packed_materials = material_views_;
    }
    [[nodiscard]] bool Contains(AssetId asset) const noexcept { return closure_ids_.contains(asset.value); }
    [[nodiscard]] bool HasSourceRevision(std::string_view id) const
    {
        return HasSourceRevision(MakeAssetId(id));
    }
    [[nodiscard]] bool HasSourceRevision(AssetId asset) const noexcept
    {
        return source_revision_assets_.contains(asset.value);
    }
    [[nodiscard]] bool HasRuntimeProduct(AssetId asset) const noexcept
    {
        const auto found = std::lower_bound(bulks.begin(), bulks.end(), asset.value,
            [](const auto &entry, std::uint64_t value) { return entry.asset_id.value < value; });
        return found != bulks.end() && found->asset_id.value == asset.value;
    }

    [[nodiscard]] std::uint32_t BulkIndex(AssetId asset) const
    {
        const auto found = std::lower_bound(bulks.begin(), bulks.end(), asset.value,
            [](const auto &entry, std::uint64_t value) { return entry.asset_id.value < value; });
        if (found == bulks.end() || found->asset_id.value != asset.value)
            throw std::runtime_error("Developer renderer product is absent from Generation");
        return static_cast<std::uint32_t>(found - bulks.begin());
    }

    [[nodiscard]] std::vector<std::byte> ReadProduct(AssetId asset, std::uint64_t limit) const
    {
        const auto index = BulkIndex(asset);
        const auto &range = bulks[index].range;
        if (!range.byte_count || range.byte_count > limit)
            throw std::runtime_error("Developer metadata product exceeds its bound");
        std::vector<std::byte> bytes;
        const auto result = ReadCheckedBinary(paths_[index], bytes, range.byte_count);
        if (!result || Sha256(bytes) != range.expected_digest)
            throw std::runtime_error("Developer metadata product changed after Generation selection");
        return bytes;
    }

    [[nodiscard]] render_data::TextureDescriptor AdmitTexture(AssetId asset,
        std::span<const std::byte> authenticated_bytes) const
    {
        const auto index = BulkIndex(asset);
        const auto &bulk = bulks[index];
        if (bulk.range.format != render_data::RenderBulkFormat::Package ||
            authenticated_bytes.size() != bulk.range.byte_count)
            throw std::runtime_error("Developer texture differs from selected Generation");
        auto &slot = texture_slots_[index];
        std::scoped_lock lock(slot.mutex);
        if (!slot.value)
        {
            auto admitted = render_data::CompileTextureDescriptor(authenticated_bytes, asset, index);
            if (slot.proof)
            {
                const auto expected = slot.proof->View(), actual = admitted.View();
                const auto &a = expected.description, &b = actual.description;
                if (expected.asset_id.value != actual.asset_id.value || expected.bulk_index != actual.bulk_index ||
                    expected.dimension != actual.dimension || expected.depth != actual.depth ||
                    a.width != b.width || a.height != b.height || a.array_size != b.array_size ||
                    a.mip_count != b.mip_count || a.format != b.format ||
                    !std::ranges::equal(expected.dependencies, actual.dependencies,
                        std::ranges::equal_to{}, &AssetId::value, &AssetId::value) ||
                    expected.subresources.size() != actual.subresources.size())
                    throw std::runtime_error("Developer texture payload differs from its metadata proof");
                for (std::size_t i = 0; i < expected.subresources.size(); ++i)
                {
                    const auto &left = expected.subresources[i], &right = actual.subresources[i];
                    if (left.offset != right.offset || left.row_pitch != right.row_pitch ||
                        left.slice_pitch != right.slice_pitch || left.byte_count != right.byte_count ||
                        left.has_digest != right.has_digest || left.digest != right.digest)
                        throw std::runtime_error("Developer texture subresource differs from its metadata proof");
                }
            }
            slot.value.emplace(std::move(admitted));
        }
        return slot.value->View();
    }

    [[nodiscard]] render_data::TextureDescriptor ProveTexture(AssetId asset) const
    {
        const auto index = BulkIndex(asset);
        const auto &bulk = bulks[index];
        if (bulk.range.format != render_data::RenderBulkFormat::Package)
            throw std::runtime_error("Developer texture proof references a non-package product");
        auto &slot = texture_slots_[index];
        std::scoped_lock lock(slot.mutex);
        if (slot.value) return slot.value->View();
        if (!slot.proof)
        {
            std::ifstream input(paths_[index], std::ios::binary | std::ios::ate);
            if (!input || input.tellg() < 0 ||
                static_cast<std::uint64_t>(input.tellg()) != bulk.range.byte_count)
                throw std::runtime_error("Developer texture file changed before metadata proof");
            const auto read = [&](std::uint64_t offset, std::size_t count) {
                if (offset > bulk.range.byte_count || count > bulk.range.byte_count - offset ||
                    offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
                    throw std::runtime_error("Developer texture proof range exceeds product");
                std::vector<std::byte> bytes(count);
                input.seekg(static_cast<std::streamoff>(offset));
                input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(count));
                if (!input || input.gcount() != static_cast<std::streamsize>(count))
                    throw std::runtime_error("Developer texture metadata proof read failed");
                return bytes;
            };
            constexpr std::size_t kMaxPrefix = 16 * 1024;
            const auto prefix = read(0, static_cast<std::size_t>(std::min<std::uint64_t>(
                kMaxPrefix, bulk.range.byte_count)));
            const auto directory = render_data::ReadPackageDirectory(prefix, bulk.range.byte_count, asset);
            // TXV1 volume metadata is at most 40 + 15 * 20 bytes, below the TXM1 v2 bound.
            if (directory.chunks.empty() ||
                directory.chunks.front().byte_count > render_data::kMaxTextureMetadataBytes)
                throw std::runtime_error("Developer texture metadata proof is unbounded");
            const auto &metadata = directory.chunks.front();
            const auto bytes = read(metadata.offset, static_cast<std::size_t>(metadata.byte_count));
            std::uint32_t magic{};
            if (bytes.size() >= sizeof(magic)) std::memcpy(&magic, bytes.data(), sizeof(magic));
            slot.proof.emplace(magic == 0x31565854u ?
                render_data::CompileVolumeTextureMetadataDescriptor(directory, bytes, index, bulk.range.byte_count) :
                render_data::CompileTextureMetadataDescriptor(directory, bytes, index, bulk.range.byte_count));
        }
        return slot.proof->View();
    }

    [[nodiscard]] std::optional<render_data::TextureDescriptor> FindTexture(AssetId asset) const
    {
        const auto index = BulkIndex(asset);
        auto &slot = texture_slots_[index];
        std::scoped_lock lock(slot.mutex);
        if (slot.value) return slot.value->View();
        if (slot.proof) return slot.proof->View();
        return std::nullopt;
    }

    [[nodiscard]] const render_data::GeometryDescriptor &AdmitGeometry(AssetId asset,
        std::span<const std::byte> authenticated_bytes, const render_data::PreparedGeometryPage &prepared) const
    { return AdmitGeometry(asset, authenticated_bytes, prepared.package, prepared.geometry); }

    [[nodiscard]] const render_data::GeometryDescriptor &AdmitGeometry(AssetId asset,
        std::span<const std::byte> authenticated_bytes, const render_data::PackageView &package,
        const render_data::GeometryData &geometry) const
    {
        const auto index = BulkIndex(asset);
        const auto &bulk = bulks[index];
        if (bulk.range.format != render_data::RenderBulkFormat::GeometryPage ||
            authenticated_bytes.size() != bulk.range.byte_count)
            throw std::runtime_error("Developer geometry differs from selected Generation");
        auto &slot = geometry_slots_[index];
        std::scoped_lock lock(slot.mutex);
        if (!slot.value)
        {
            auto admitted = render_data::CompileGeometryDescriptor(authenticated_bytes, package, geometry, index);
            if (admitted.max_bone_index >= slot.bone_limit)
                throw std::runtime_error("Developer geometry exceeds admitted character bone range");
            if (slot.proof)
            {
                const auto &proof = *slot.proof_view;
                if (admitted.flags != proof.flags || admitted.vertex_count != proof.vertex_count ||
                    admitted.index_count != proof.index_count || admitted.meshlet_count != proof.meshlet_count ||
                    admitted.vertex_ref_count != proof.vertex_ref_count ||
                    admitted.submeshes.size() != proof.submeshes.size() ||
                    admitted.max_material_index != proof.max_material_index ||
                    admitted.bounds.minimum != proof.bounds.minimum ||
                    admitted.bounds.maximum != proof.bounds.maximum ||
                    admitted.bounds.sphere != proof.bounds.sphere)
                    throw std::runtime_error("Developer geometry payload differs from metadata proof");
            }
            slot.value.emplace(std::move(admitted));
            slot.view.emplace(slot.value->View());
            {
                std::scoped_lock table_lock(descriptor_mutex_);
                geometry_views_[index] = *slot.view;
            }
        }
        return *slot.view;
    }

    [[nodiscard]] const render_data::GeometryDescriptor &ProveGeometry(
        AssetId asset, const render_data::GeometryMetadata &metadata, std::uint32_t bone_limit) const
    {
        const auto index = BulkIndex(asset);
        if (bulks[index].range.format != render_data::RenderBulkFormat::GeometryPage)
            throw std::runtime_error("Developer geometry proof references a non-page product");
        auto &slot = geometry_slots_[index];
        std::scoped_lock lock(slot.mutex);
        if (!bone_limit || bone_limit > kMaxCharacterBones)
            throw std::runtime_error("Developer geometry proof has invalid bone limit");
        slot.bone_limit = std::min(slot.bone_limit, bone_limit);
        if (slot.value)
        {
            render_data::ValidateGeometryDescriptorMetadata(*slot.view, metadata);
            if (slot.view->max_bone_index >= slot.bone_limit)
                throw std::runtime_error("Developer geometry exceeds admitted character bone range");
            return *slot.view;
        }
        if (!slot.proof)
        {
            render_data::GeometryDescriptorStorage proof;
            proof.asset_id = asset;
            proof.bulk_index = index;
            proof.flags = metadata.flags;
            proof.vertex_count = metadata.vertex_count;
            proof.index_count = metadata.index_count;
            proof.meshlet_count = metadata.meshlet_count;
            proof.vertex_ref_count = metadata.vertex_ref_count;
            proof.max_material_index = metadata.max_material_index;
            proof.bounds = metadata.bounds;
            proof.submeshes.resize(metadata.submesh_count);
            proof.payload_ready = false;
            slot.proof.emplace(std::move(proof));
            slot.proof_view.emplace(slot.proof->View());
            {
                std::scoped_lock table_lock(descriptor_mutex_);
                geometry_views_[index] = *slot.proof_view;
            }
        }
        render_data::ValidateGeometryDescriptorMetadata(*slot.proof_view, metadata);
        return *slot.proof_view;
    }

    [[nodiscard]] const render_data::GeometryDescriptor *FindGeometry(AssetId asset) const
    {
        const auto index = BulkIndex(asset);
        auto &slot = geometry_slots_[index];
        std::scoped_lock lock(slot.mutex);
        return slot.view ? &*slot.view : nullptr;
    }

    [[nodiscard]] const render_data::CharacterDescriptor &AdmitCharacter(
        AssetId asset, std::span<const std::byte> bytes, const render_data::PackageView &package,
        const render_data::CharacterMetadataPackageData &prepared) const
    {
        const auto index = BulkIndex(asset);
        if (bulks[index].range.format != render_data::RenderBulkFormat::Package ||
            bytes.size() != bulks[index].range.byte_count)
            throw std::runtime_error("Developer character differs from selected Generation");
        auto &slot = character_slots_[index];
        std::scoped_lock lock(slot.mutex);
        if (!slot.value)
        {
            const auto prove = [&](const render_data::GeometryMetadata &metadata) {
                if (metadata.page_dependency_index >= package.dependencies.size())
                    throw std::runtime_error("Developer character geometry dependency is absent");
                static_cast<void>(ProveGeometry(MakeAssetId(package.dependencies[metadata.page_dependency_index]),
                    metadata, prepared.bone_count));
            };
            prove(prepared.geometry);
            if (prepared.monster)
                for (const auto &lod : prepared.monster->lods) prove(lod);
            std::scoped_lock table_lock(descriptor_mutex_);
            slot.value.emplace(render_data::CompileCharacterDescriptor(bytes, package, prepared,
                index, bulks, geometry_views_, material_views_));
            slot.view.emplace(slot.value->View());
            character_views_[index] = *slot.view;
        }
        return *slot.view;
    }

    [[nodiscard]] const render_data::AnimationDescriptor &AdmitAnimation(
        AssetId asset, std::span<const std::byte> bytes, const render_data::PackageView &package,
        const render_data::CharacterAnimationExtensionData &prepared) const
    {
        const auto index = BulkIndex(asset);
        if (bulks[index].range.format != render_data::RenderBulkFormat::Package ||
            bytes.size() != bulks[index].range.byte_count)
            throw std::runtime_error("Developer animation differs from selected Generation");
        const auto character_index = BulkIndex(prepared.character_asset_id);
        auto &character = character_slots_[character_index];
        auto &slot = animation_slots_[index];
        std::scoped_lock character_lock(character.mutex);
        std::scoped_lock animation_lock(slot.mutex);
        if (!character.value) throw std::runtime_error("Developer animation precedes character admission");
        if (!slot.value)
        {
            slot.value.emplace(render_data::CompileAnimationDescriptor(bytes, package, prepared,
                index, bulks, render_data::CharacterHandle{character_index}, *character.value));
            slot.view.emplace(slot.value->View());
            render_data::ValidateAnimationDescriptor(*slot.view, bulks, character_views_);
        }
        return *slot.view;
    }

    [[nodiscard]] const render_data::CharacterDescriptor *FindCharacter(AssetId asset) const
    {
        auto &slot = character_slots_[BulkIndex(asset)];
        std::scoped_lock lock(slot.mutex);
        return slot.view ? &*slot.view : nullptr;
    }

    [[nodiscard]] const render_data::AnimationDescriptor *FindAnimation(AssetId asset) const
    {
        auto &slot = animation_slots_[BulkIndex(asset)];
        std::scoped_lock lock(slot.mutex);
        return slot.view ? &*slot.view : nullptr;
    }

    static std::shared_ptr<const RendererDevelopmentCatalog> Build(const RendererConfig &config)
    {
        if (config.packed_content || !config.content_generation_lease || config.product_paths.empty() ||
            config.product_paths.size() != config.product_digests.size())
            throw std::runtime_error("Developer catalog requires one leased, complete Generation");
        auto result = std::make_shared<RendererDevelopmentCatalog>();
        std::vector<std::pair<AssetId, std::string>> products;
        for (const auto &[id, path] : config.product_paths)
        {
            if (!result->closure_ids_.insert(MakeAssetId(id).value).second)
                throw std::runtime_error("Developer Generation AssetId collision");
            if (id.starts_with("render/") || id == "vfx/vfx_mesh_atlas.hsbin")
                products.emplace_back(MakeAssetId(id), id);
            else
            {
                const auto digest = config.product_digests.find(id);
                if (!MakeAssetId(id).value || digest == config.product_digests.end() ||
                    digest->second.size() != 71 || !digest->second.starts_with("sha256:") ||
                    !std::ranges::all_of(std::string_view(digest->second).substr(7), [](char value) {
                        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
                    }))
                    throw std::runtime_error("Invalid developer source revision identity: " + id);
                result->source_revision_assets_.insert(MakeAssetId(id).value);
            }
        }
        std::ranges::sort(products, {}, [](const auto &entry) { return entry.first.value; });
        if (products.empty() || products.size() > UINT32_MAX)
            throw std::runtime_error("Developer renderer product closure is empty or too large");
        const auto format = [](std::string_view id) {
            using F = render_data::RenderBulkFormat;
            if (id.ends_with(".rpkg")) return F::Package;
            if (id.ends_with(".rpage")) return F::GeometryPage;
            if (id.ends_with(".ghbin")) return F::GeometryHierarchy;
            if (id.ends_with(".shl1")) return F::SceneHlod;
            if (id.ends_with(".index")) return F::StaticShadowIndex;
            if (id.ends_with(".pages")) return F::StaticShadowPages;
            if (id.ends_with(".dds")) return F::DdsTexture;
            if (id.ends_with(".spkg")) return F::SceneProbe;
            if (id == "vfx/vfx_mesh_atlas.hsbin") return F::VfxMeshAtlas;
            if (id.ends_with(".dxil")) return F::ShaderDxil;
            if (id == render_data::SurfaceLayout::abi_product) return F::SurfaceLayoutAbi;
            throw std::runtime_error("Unsupported developer renderer product format: " + std::string(id));
        };
        for (const auto &[asset, id] : products)
        {
            if (!asset.value || (!result->bulks.empty() && asset.value <= result->bulks.back().asset_id.value))
                throw std::runtime_error("Developer renderer AssetId collision");
            const auto &path = config.product_paths.at(id);
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            const auto &digest = config.product_digests.at(id);
            if (!path.is_absolute() || error || !size || size > std::numeric_limits<std::size_t>::max() ||
                digest.size() != 71 || !digest.starts_with("sha256:"))
                throw std::runtime_error("Invalid developer renderer product identity: " + id);
            std::array<std::byte, 32> hash{};
            for (std::size_t i = 0; i < hash.size(); ++i)
            {
                unsigned byte{};
                const auto first = digest.data() + 7 + i * 2;
                const auto [end, status] = std::from_chars(first, first + 2, byte, 16);
                if (status != std::errc{} || end != first + 2)
                    throw std::runtime_error("Invalid developer renderer product digest: " + id);
                hash[i] = static_cast<std::byte>(byte);
            }
            result->bulks.push_back({asset, {0, size, format(id), hash}});
            result->paths_.push_back(path);
        }
        result->texture_slots_ = std::make_unique<TextureSlot[]>(result->bulks.size());
        result->geometry_slots_ = std::make_unique<GeometrySlot[]>(result->bulks.size());
        result->character_slots_ = std::make_unique<CharacterSlot[]>(result->bulks.size());
        result->animation_slots_ = std::make_unique<AnimationSlot[]>(result->bulks.size());
        result->geometry_views_.resize(result->bulks.size());
        result->character_views_.resize(result->bulks.size());
        for (const auto &[asset, id] : products)
            if (id.starts_with("render/texture/") && id.ends_with(".rpkg"))
                result->texture_views_.push_back(result->ProveTexture(asset));
        std::vector<AssetId> bulk_assets;
        bulk_assets.reserve(result->bulks.size());
        for (const auto &bulk : result->bulks) bulk_assets.push_back(bulk.asset_id);
        for (const auto &[asset, id] : products)
        {
            if (!render_data::IsMaterialDescriptorProduct(id)) continue;
            const auto parameter = render_data::MaterialParametersProduct(id);
            const auto material_bytes = result->ReadProduct(asset, 1024 * 1024);
            const auto parameter_bytes = result->ReadProduct(MakeAssetId(parameter), 1024 * 1024);
            result->material_storage_.push_back(render_data::CompileMaterialDescriptor(
                id, material_bytes, parameter_bytes, result->BulkIndex(asset),
                bulk_assets, result->texture_views_));
        }
        result->material_views_.reserve(result->material_storage_.size());
        for (const auto &entry : result->material_storage_) result->material_views_.push_back(entry.View());
        return result;
    }
};
} // namespace hs
