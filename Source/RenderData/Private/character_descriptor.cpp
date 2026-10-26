#include <hs/render_data/character_descriptor.hpp>
#include <hs/core/sha256.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace hs::render_data
{
namespace
{
void Require(bool valid, const char *message)
{ if (!valid) throw std::runtime_error(std::string("character descriptor: ") + message); }

const RendererBulkDescriptor &Parent(AssetId id, std::uint32_t index, std::span<const RendererBulkDescriptor> bulks)
{
    Require(id.value && index < bulks.size() && bulks[index].asset_id.value == id.value &&
            bulks[index].range.format == RenderBulkFormat::Package && bulks[index].range.byte_count &&
            bulks[index].range.byte_count <= UINT64_MAX - bulks[index].range.offset, "invalid package bulk reference");
    return bulks[index];
}
BulkRange ChunkRange(std::span<const std::byte> bytes, const PackageView &package, ChunkType type,
    const RendererBulkDescriptor &bulk)
{
    Require(bytes.size() == bulk.range.byte_count && package.asset_id.value == bulk.asset_id.value,
            "prepared package and bulk disagree");
    const auto found = std::ranges::find(package.chunks, type, &ChunkView::type);
    Require(found != package.chunks.end() && found->required, "required sample chunk is absent");
    const auto begin = reinterpret_cast<std::uintptr_t>(bytes.data());
    const auto address = reinterpret_cast<std::uintptr_t>(found->bytes.data());
    Require(address >= begin && address - begin <= bytes.size() && found->bytes.size() <= bytes.size() - (address - begin),
            "sample chunk does not borrow supplied bytes");
    const auto offset = static_cast<std::uint64_t>(address - begin);
    Require(offset <= UINT64_MAX - bulk.range.offset, "absolute sample offset overflows");
    return {bulk.range.offset + offset, found->bytes.size(), RenderBulkFormat::Package, hs::Sha256(found->bytes)};
}
std::uint64_t Rows(const BulkRange &range, const RendererBulkDescriptor &bulk, std::uint32_t stride)
{
    Require(range.format == RenderBulkFormat::Package && range.byte_count && range.byte_count % stride == 0 &&
            range.byte_count / stride <= 800'000 && range.offset >= bulk.range.offset &&
            range.offset - bulk.range.offset <= bulk.range.byte_count &&
            range.byte_count <= bulk.range.byte_count - (range.offset - bulk.range.offset) &&
            range.byte_count <= std::numeric_limits<std::size_t>::max(), "sample subrange is invalid");
    return range.byte_count / stride;
}
template<class Descriptor>
std::uint32_t Find(AssetId id, std::span<const Descriptor> descriptors)
{
    const auto found = std::ranges::find_if(descriptors, [&](const auto &entry) { return entry.asset_id.value == id.value; });
    Require(found != descriptors.end() && found - descriptors.begin() < UINT32_MAX, "referenced descriptor is absent");
    return static_cast<std::uint32_t>(found - descriptors.begin());
}
void Skeleton(std::uint32_t bones, std::span<const std::uint16_t> parents,
    std::span<const std::array<float,16>> inverse)
{
    Require(bones > 0 && bones <= kMaxCharacterBones && parents.size() == bones && inverse.size() == bones,
            "invalid skeleton metadata counts");
    for (std::size_t i=0;i<bones;++i)
    {
        Require(parents[i] == UINT16_MAX || parents[i] < i, "invalid ordered parent");
        Require(std::ranges::all_of(inverse[i], [](float value) { return std::isfinite(value); }), "nonfinite inverse bind");
    }
}
void Transform(const CharacterLocalTransform &value)
{
    const auto finite = [](float lane) { return std::isfinite(lane); };
    Require(std::ranges::all_of(value.translation,finite) && std::ranges::all_of(value.rotation,finite) &&
            std::ranges::all_of(value.scale,finite), "nonfinite socket transform");
    float length{}; for (const auto lane:value.rotation) length += lane*lane;
    length = std::sqrt(length);
    Require(length >= 0.999f && length <= 1.001f &&
            std::ranges::all_of(value.scale, [](float lane) { return std::abs(lane) > 0.0001f; }), "invalid socket rotation or scale");
}
} // namespace

CharacterDescriptor CharacterDescriptorStorage::View() const noexcept
{
    auto result = value;
    result.geometry = data.geometry;
    result.dependencies = dependencies; result.material_bindings = data.material_bindings; result.materials = materials;
    result.bone_count = data.bone_count; result.material_count = data.material_count; result.ground_offset = data.ground_offset;
    result.clips = data.clips; result.parents = data.parents; result.inverse_bind_matrices = data.inverse_bind_matrices;
    result.upper_body_weights = data.upper_body_weights;
    if (data.monster)
    {
        const auto &monster = *data.monster;
        result.deformation_abi = monster.abi; result.deformation_flags = monster.flags; result.skeleton_binding = monster.skeleton_binding;
        result.numerical_margin = monster.numerical_margin; result.bubble_amplitude = monster.bubble_amplitude;
        result.lods = monster.lods; result.interval_bounds = monster.interval_bounds;
    }
    return result;
}

AnimationDescriptor AnimationDescriptorStorage::View() const noexcept
{
    auto result = value;
    result.character_asset_id = data.character_asset_id; result.sample_rate_hz = data.sample_rate_hz;
    result.bone_names = bone_names; result.parents = data.parents; result.inverse_bind_matrices = data.inverse_bind_matrices;
    result.clips = clips; result.sockets = sockets;
    return result;
}

CharacterDescriptorStorage CompileCharacterDescriptor(std::span<const std::byte> bytes, const PackageView &package,
    CharacterMetadataPackageData prepared, std::uint32_t bulk_index, std::span<const RendererBulkDescriptor> bulks,
    std::span<const GeometryDescriptor> geometry, std::span<const MaterialDescriptor> materials)
{
    const auto &bulk = Parent(package.asset_id,bulk_index,bulks);
    CharacterDescriptorStorage result;
    result.value.asset_id = package.asset_id; result.value.bulk_index = bulk_index;
    result.data = std::move(prepared);
    for (const auto &dependency:package.dependencies) result.dependencies.push_back(MakeAssetId(dependency));
    Require(result.data.geometry.page_dependency_index < result.dependencies.size(), "character page dependency is absent");
    result.value.geometry_page = {Find(result.dependencies[result.data.geometry.page_dependency_index],geometry)};
    for (const auto &binding:result.data.material_bindings)
    {
        Require(binding.dependency_index < result.dependencies.size(), "character material dependency is absent");
        result.materials.push_back({Find(result.dependencies[binding.dependency_index],materials)});
    }
    if (result.data.monster)
    {
        result.value.final_matrices = ChunkRange(bytes,package,ChunkType::CharacterFinalMatrices,bulk);
        Require(result.value.final_matrices.byte_count / 64 == result.data.monster->final_matrices.size() &&
                result.value.final_matrices.byte_count % 64 == 0, "prepared matrix count differs from original chunk");
        decltype(result.data.monster->final_matrices){}.swap(result.data.monster->final_matrices);
    }
    else
    {
        result.value.transforms = ChunkRange(bytes,package,ChunkType::CharacterLocalTransforms,bulk);
        Require(result.value.transforms.byte_count / sizeof(PackedCharacterTransform) == result.data.transforms.size() &&
                result.value.transforms.byte_count % sizeof(PackedCharacterTransform) == 0, "prepared transform count differs from original chunk");
    }
    decltype(result.data.transforms){}.swap(result.data.transforms);
    ValidateCharacterDescriptor(result.View(),bulks,geometry,materials);
    return result;
}

AnimationDescriptorStorage CompileAnimationDescriptor(std::span<const std::byte> bytes, const PackageView &package,
    CharacterAnimationExtensionData prepared, std::uint32_t bulk_index, std::span<const RendererBulkDescriptor> bulks,
    CharacterHandle character, const CharacterDescriptorStorage &character_storage)
{
    const auto target = character_storage.View();
    // Decode already admitted each sample; only the ordered cross-product binding remains.
    Require(static_cast<bool>(character) && target.asset_id.value && prepared.character_asset_id.value == target.asset_id.value &&
            prepared.bone_names.size() == target.bone_count && std::ranges::equal(prepared.parents,target.parents) &&
            std::ranges::equal(prepared.inverse_bind_matrices,target.inverse_bind_matrices), "ordered animation character binding mismatch");
    AnimationDescriptorStorage result;
    result.value.asset_id = package.asset_id; result.value.bulk_index = bulk_index; result.value.character = character;
    result.value.transforms = ChunkRange(bytes,package,ChunkType::CharacterLocalTransforms,Parent(package.asset_id,bulk_index,bulks));
    Require(result.value.transforms.byte_count / sizeof(PackedCharacterTransform) == prepared.transforms.size() &&
            result.value.transforms.byte_count % sizeof(PackedCharacterTransform) == 0, "prepared animation count differs from original chunk");
    result.data = std::move(prepared);
    decltype(result.data.transforms){}.swap(result.data.transforms);
    for (const auto &name:result.data.bone_names) result.bone_names.push_back(name);
    for (const auto &clip:result.data.clips)
        result.clips.push_back({clip.id,clip.name,clip.first_transform,clip.frame_count,clip.duration_seconds,clip.looping});
    for (const auto &socket:result.data.sockets) result.sockets.push_back({socket.name,socket.bone_index,socket.local_transform});
    return result;
}

void ValidateCharacterDescriptor(const CharacterDescriptor &value, std::span<const RendererBulkDescriptor> bulks,
    std::span<const GeometryDescriptor> geometry, std::span<const MaterialDescriptor> materials)
{
    const auto &bulk = Parent(value.asset_id,value.bulk_index,bulks);
    Skeleton(value.bone_count,value.parents,value.inverse_bind_matrices);
    Require(value.material_count > 0 && value.material_count <= kMaxCharacterMaterials && std::isfinite(value.ground_offset) &&
            value.upper_body_weights.size() == value.bone_count && value.clips.size() == static_cast<std::size_t>(CharacterAnimationClip::Count),
            "invalid character metadata counts");
    Require(std::ranges::all_of(value.upper_body_weights, [](float weight) { return std::isfinite(weight) && weight >= 0 && weight <= 1; }),
            "invalid upper body weights");
    if (value.asset_id.value == MakeAssetId("render/character/archer.rpkg").value)
        Require(std::ranges::any_of(value.upper_body_weights, [](float weight) { return weight > 0; }) &&
                std::ranges::any_of(value.upper_body_weights, [](float weight) { return weight == 0; }), "Archer mask lacks both regions");
    Require(value.geometry_page.value < geometry.size() && value.geometry.page_dependency_index < value.dependencies.size(),
            "invalid character geometry handle");
    const auto &page = geometry[value.geometry_page.value];
    Require(page.asset_id.value == value.dependencies[value.geometry.page_dependency_index].value &&
            (page.flags & kGeometryFlagSkinned) && (!page.payload_ready || page.max_bone_index < value.bone_count) &&
            page.max_material_index < value.material_count,
            "character page identity or skin/material extent mismatch");
    ValidateGeometryDescriptorMetadata(page,value.geometry);
    Require(value.material_bindings.size() == value.materials.size(), "character material handle count mismatch");
    std::array<bool,kMaxCharacterMaterials> opaque{}, translucent{};
    for (std::size_t i=0;i<value.materials.size();++i)
    {
        const auto &binding = value.material_bindings[i];
        Require(binding.slot < value.material_count && binding.dependency_index < value.dependencies.size() &&
                binding.hero_ordinal < kHeroMaterialNames.size() && value.materials[i].value < materials.size(), "invalid FBX material binding");
        const auto &material = materials[value.materials[i].value];
        Require(material.kind == MaterialKind::Hero && material.domain == binding.domain &&
                material.asset_id.value == value.dependencies[binding.dependency_index].value && material.asset_id.value ==
                MakeAssetId("render/material/" + std::string(kHeroMaterialNames[binding.hero_ordinal]) + "/material.rpkg").value,
                "character material identity or domain mismatch");
        auto &seen = binding.domain == HeroMaterialDomain::Opaque ? opaque : translucent;
        Require(binding.domain <= HeroMaterialDomain::Translucent && !seen[binding.slot], "duplicate character material domain");
        seen[binding.slot] = true;
        Require((binding.domain == HeroMaterialDomain::Translucent) == (kHeroMaterialNames[binding.hero_ordinal] == "slime_body"),
                "character Hero ordinal/domain mismatch");
    }
    for (std::size_t slot=0;slot<value.material_count;++slot) Require(opaque[slot], "missing opaque FBX material slot");
    for (const auto &binding:value.material_bindings)
    {
        if (binding.domain == HeroMaterialDomain::Translucent)
            Require(std::ranges::any_of(value.material_bindings,[&](const auto &other) { return other.slot == binding.slot &&
                other.domain == HeroMaterialDomain::Opaque && kHeroMaterialNames[other.hero_ordinal] == "slime_features"; }),
                "jelly body lacks opaque feature binding");
        else if (kHeroMaterialNames[binding.hero_ordinal] == "slime_features") Require(translucent[binding.slot], "jelly feature lacks body binding");
    }
    const bool monster = value.deformation_abi != 0;
    Require(monster ? !value.transforms.byte_count : !value.final_matrices.byte_count, "mixed character sample representations");
    const auto rows = Rows(monster ? value.final_matrices : value.transforms,bulk,monster ? 64u : 40u);
    std::uint64_t next{}, intervals{};
    for (std::size_t i=0;i<value.clips.size();++i)
    {
        const auto &clip = value.clips[i];
        Require(static_cast<std::size_t>(clip.clip) == i && clip.looping <= 1 && !clip.reserved && clip.frame_count >= 2 &&
                std::isfinite(clip.duration_seconds) && clip.duration_seconds > 0 && clip.first_transform <= rows &&
                std::uint64_t(clip.frame_count)*value.bone_count <= rows-clip.first_transform, "invalid character clip range");
        if (monster) Require(clip.first_transform == next, "monster palette clip ranges are not contiguous");
        next += std::uint64_t(clip.frame_count)*value.bone_count; intervals += clip.frame_count;
    }
    if (!monster) { Require(value.lods.empty() && value.interval_bounds.empty() && !value.deformation_flags, "TRS character has deformation metadata"); return; }
    Require(value.deformation_abi == kMonsterDeformationAbi && !(value.deformation_flags & ~3u) && value.deformation_flags != 3 &&
            std::isfinite(value.numerical_margin) && value.numerical_margin >= 0 &&
            value.bubble_amplitude == ((value.deformation_flags & kMonsterDeformationSuicideBubble) ? 0.006f : 0.0f) &&
            std::ranges::any_of(value.skeleton_binding, [](std::byte lane) { return lane != std::byte{}; }) &&
            !value.lods.empty() && value.lods.size() <= 16 && next == rows && intervals*value.lods.size() == value.interval_bounds.size(),
            "invalid monster deformation metadata");
    std::size_t row{};
    std::unordered_set<std::uint16_t> pages;
    for (std::size_t lod=0;lod<value.lods.size();++lod)
    {
        const auto &metadata = value.lods[lod];
        Require(metadata.lod == lod &&
            metadata.error_method_abi == (lod == 0 ? kGeometryErrorMethodAbi : kSkinnedPoseErrorMethodAbi) &&
            (lod == 0 || metadata.geometric_error >= value.lods[lod - 1].geometric_error) &&
            (lod == 0 || (metadata.vertex_count < value.lods[lod - 1].vertex_count &&
                          metadata.index_count < value.lods[lod - 1].index_count)) &&
            metadata.page_dependency_index < value.dependencies.size() && pages.insert(metadata.page_dependency_index).second,
                "invalid monster LOD reference");
        const auto &lod_page = geometry[Find(value.dependencies[metadata.page_dependency_index],geometry)];
        ValidateGeometryDescriptorMetadata(lod_page,metadata);
        Require((lod_page.flags & kGeometryFlagSkinned) &&
            (!lod_page.payload_ready || lod_page.max_bone_index < value.bone_count) &&
            lod_page.max_material_index < value.material_count,
                "monster LOD skin or material range mismatch");
        for (const auto &clip:value.clips) for (std::uint32_t frame=0;frame<clip.frame_count;++frame)
        {
            const auto &bound = value.interval_bounds[row++];
            Require(bound.lod == lod && bound.clip == clip.clip && bound.frame == frame, "monster interval order mismatch");
            for (std::size_t axis=0;axis<3;++axis) Require(std::isfinite(bound.minimum[axis]) && std::isfinite(bound.maximum[axis]) &&
                bound.minimum[axis] <= bound.maximum[axis], "invalid monster interval bounds");
        }
    }
    const auto &first = value.lods.front();
    Require(first.abi == value.geometry.abi && first.flags == value.geometry.flags &&
            first.vertex_count == value.geometry.vertex_count && first.index_count == value.geometry.index_count &&
            first.submesh_count == value.geometry.submesh_count && first.meshlet_count == value.geometry.meshlet_count &&
            first.vertex_ref_count == value.geometry.vertex_ref_count && first.max_material_index == value.geometry.max_material_index &&
            first.fallback_dependency_index == value.geometry.fallback_dependency_index && first.lod == value.geometry.lod &&
            first.terminal_fallback_dependency_index == value.geometry.terminal_fallback_dependency_index &&
            first.geometric_error == value.geometry.geometric_error && first.error_method_abi == value.geometry.error_method_abi &&
            first.page_dependency_index == value.geometry.page_dependency_index && first.bounds.minimum == value.geometry.bounds.minimum &&
            first.bounds.maximum == value.geometry.bounds.maximum && first.bounds.sphere == value.geometry.bounds.sphere,
            "monster LOD0 differs from direct geometry");
}

void ValidateAnimationDescriptor(const AnimationDescriptor &value, std::span<const RendererBulkDescriptor> bulks,
    std::span<const CharacterDescriptor> characters)
{
    const auto &bulk = Parent(value.asset_id,value.bulk_index,bulks);
    Require(value.character.value < characters.size(), "invalid animation character handle");
    const auto &character = characters[value.character.value];
    Require(value.character_asset_id.value == character.asset_id.value && value.bone_names.size() == character.bone_count &&
            std::ranges::equal(value.parents,character.parents) && std::ranges::equal(value.inverse_bind_matrices,character.inverse_bind_matrices),
            "ordered animation skeleton binding mismatch");
    Skeleton(character.bone_count,value.parents,value.inverse_bind_matrices);
    Require(value.sample_rate_hz == 0 || value.sample_rate_hz == 60, "unsupported animation sample rate");
    Require(!value.clips.empty() && value.clips.size() <= 128 && value.sockets.size() <= 128, "invalid named animation counts");
    std::unordered_set<std::string_view> names;
    for (auto name:value.bone_names) Require(!name.empty() && names.insert(name).second, "invalid compiled bone name");
    Require(std::ranges::count(value.parents,UINT16_MAX) == 1, "animation skeleton must have one root");
    names.clear(); std::unordered_set<std::uint64_t> ids;
    const auto rows = Rows(value.transforms,bulk,40);
    std::uint64_t next{};
    for (const auto &clip:value.clips)
    {
        Require(clip.id.value && ids.insert(clip.id.value).second && !clip.name.empty() && names.insert(clip.name).second &&
                clip.first_transform == next && clip.frame_count >= 2 && std::isfinite(clip.duration_seconds) && clip.duration_seconds > 0 &&
                next <= rows && std::uint64_t(clip.frame_count)*character.bone_count <= rows-next, "invalid named clip identity or range");
        next += std::uint64_t(clip.frame_count)*character.bone_count;
    }
    Require(next == rows, "unreferenced animation samples");
    names.clear();
    for (const auto &socket:value.sockets)
    {
        Require(!socket.name.empty() && names.insert(socket.name).second && socket.bone_index < character.bone_count, "invalid animation socket");
        Transform(socket.local_transform);
    }
}
} // namespace hs::render_data
