#include <hs/render_data/character_package.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string_view>

namespace hs::render_data
{
namespace
{
void Require(bool valid, std::string_view message)
{
    if (!valid) throw std::runtime_error("character package: " + std::string(message));
}

void Put(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value,
         std::size_t width)
{
    Require(offset <= bytes.size() && width <= bytes.size() - offset, "write range");
    for (std::size_t i = 0; i < width; ++i)
        bytes[offset + i] = std::byte((value >> (8 * i)) & 0xff);
}

std::uint32_t Get(std::span<const std::byte> bytes, std::size_t offset,
                  std::size_t width)
{
    Require(offset <= bytes.size() && width <= bytes.size() - offset, "read range");
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < width; ++i)
        value |= std::uint32_t(std::to_integer<unsigned>(bytes[offset + i])) << (8 * i);
    return value;
}

void PutFloat(std::span<std::byte> bytes, std::size_t offset, float value)
{
    Put(bytes, offset, std::bit_cast<std::uint32_t>(value), 4);
}

float GetFloat(std::span<const std::byte> bytes, std::size_t offset)
{
    return std::bit_cast<float>(Get(bytes, offset, 4));
}

template <typename Vertex>
void ValidateVertices(std::span<const Vertex> vertices, std::uint32_t bone_count,
                      std::uint32_t material_count)
{
    for (const auto &vertex : vertices)
    {
        Require(vertex.material_index < material_count && vertex.reserved == 0,
                "invalid character vertex material/reserved");
        const auto finite = [](float x) { return std::isfinite(x); };
        Require(std::ranges::all_of(vertex.position, finite) &&
                std::ranges::all_of(vertex.normal, finite) &&
                std::ranges::all_of(vertex.uv, finite) &&
                std::ranges::all_of(vertex.tangent, finite), "nonfinite character vertex");
        float sum = 0.0f;
        for (std::size_t i = 0; i < 4; ++i)
        {
            Require(vertex.bone_indices[i] < bone_count &&
                    std::isfinite(vertex.bone_weights[i]) &&
                    vertex.bone_weights[i] >= 0.0f && vertex.bone_weights[i] <= 1.0f,
                    "invalid vertex skin influence");
            sum += vertex.bone_weights[i];
        }
        Require(std::isfinite(sum) && std::abs(sum - 1.0f) <= 0.001f,
                "vertex skin weights are not normalized");
    }
}

template <typename Data, typename Vertex>
void Validate(const Data &data, std::span<const Vertex> vertices,
              bool validate_mask, bool embedded = true, std::size_t palette_count = 0)
{
    const auto pose_count = palette_count ? palette_count : data.transforms.size();
    Require(data.bone_count > 0 && data.bone_count <= kMaxCharacterBones &&
            data.material_count > 0 && data.material_count <= kMaxCharacterMaterials &&
            (!embedded || (!vertices.empty() && vertices.size() <= 3'000'000 &&
            vertices.size() % 3 == 0)) &&
            data.parents.size() == data.bone_count &&
            data.inverse_bind_matrices.size() == data.bone_count &&
            data.upper_body_weights.size() == data.bone_count &&
            data.clips.size() == static_cast<std::size_t>(CharacterAnimationClip::Count) &&
            pose_count > 0 && pose_count <= 800'000 &&
            (!palette_count || data.transforms.empty()) &&
            std::isfinite(data.ground_offset), "invalid character shape");
    ValidateVertices(vertices, data.bone_count, data.material_count);
    for (std::size_t i = 0; i < data.bone_count; ++i)
    {
        Require(data.parents[i] == std::numeric_limits<std::uint16_t>::max() ||
                data.parents[i] < i, "invalid parent hierarchy");
        Require(std::ranges::all_of(data.inverse_bind_matrices[i], [](float x) {
            return std::isfinite(x);
        }), "nonfinite inverse bind matrix");
        Require(std::isfinite(data.upper_body_weights[i]) &&
                data.upper_body_weights[i] >= 0.0f && data.upper_body_weights[i] <= 1.0f,
                "invalid upper body mask");
    }
    if (validate_mask)
        Require(std::ranges::any_of(data.upper_body_weights, [](float x) { return x > 0.0f; }) &&
                std::ranges::any_of(data.upper_body_weights, [](float x) { return x == 0.0f; }),
                "upper body mask lacks both regions");
    for (std::size_t i = 0; i < data.clips.size(); ++i)
    {
        const auto &clip = data.clips[i];
        Require(clip.clip == static_cast<CharacterAnimationClip>(i) &&
                clip.looping <= 1 && clip.reserved == 0 &&
                clip.frame_count >= 2 && std::isfinite(clip.duration_seconds) &&
                clip.duration_seconds > 0.0f &&
                std::uint64_t(clip.first_transform) +
                    std::uint64_t(clip.frame_count) * data.bone_count <= pose_count,
                "invalid animation clip");
    }
    for (const auto &transform : data.transforms)
    {
        const auto rotation_length = std::sqrt(std::inner_product(
            transform.rotation.begin(), transform.rotation.end(),
            transform.rotation.begin(), 0.0f));
        Require(std::ranges::all_of(transform.translation, [](float x) {
                    return std::isfinite(x);
                }) &&
                std::ranges::all_of(transform.scale, [](float x) {
                    return std::isfinite(x) && std::abs(x) > 0.0001f;
                }) &&
                std::isfinite(rotation_length) && rotation_length >= 0.999f &&
                rotation_length <= 1.001f,
                "invalid local transform");
    }
}

std::vector<std::byte> Bytes(std::size_t count, std::size_t stride)
{
    Require(count <= std::numeric_limits<std::size_t>::max() / stride,
            "chunk size overflow");
    return std::vector<std::byte>(count * stride);
}

void ValidateMonster(const MonsterCharacterData &data, std::uint32_t bones,
    std::span<const CharacterClipHeader> clips, const GeometryMetadata &geometry,
    std::span<const std::string> dependencies)
{
    Require(data.abi == kMonsterDeformationAbi && data.bone_count == bones && !(data.flags & ~3u) &&
        data.flags != (kMonsterBoundsPositionEnclosure | kMonsterDeformationSuicideBubble) &&
        std::isfinite(data.numerical_margin) && data.numerical_margin >= 0 &&
        data.bubble_amplitude == ((data.flags & kMonsterDeformationSuicideBubble) ? 0.006f : 0.0f) &&
        std::ranges::any_of(data.skeleton_binding, [](std::byte b) { return b != std::byte{}; }),
        "invalid monster deformation policy or binding");
    Require(!data.final_matrices.empty() && data.final_matrices.size() <= 800'000 &&
        !data.lods.empty() && data.lods.size() <= 16, "invalid monster deformation counts");
    std::uint64_t matrices{}, intervals{};
    for (const auto &clip : clips)
    {
        Require(clip.first_transform == matrices, "monster matrix clip ranges are not contiguous");
        matrices += std::uint64_t(clip.frame_count) * bones;
        intervals += clip.frame_count;
    }
    Require(matrices == data.final_matrices.size() && intervals * data.lods.size() == data.interval_bounds.size(),
        "monster matrix or interval range mismatch");
    for (const auto &matrix : data.final_matrices)
        Require(std::ranges::all_of(matrix, [](float value) { return std::isfinite(value); }), "nonfinite monster matrix");
    Require(EncodeGeometryMetadata(data.lods.front(), dependencies, {}) == EncodeGeometryMetadata(geometry, dependencies, {}),
        "monster LOD0 disagrees with character geometry");
    std::size_t row{};
    std::vector<std::uint16_t> pages;
    for (std::size_t lod = 0; lod < data.lods.size(); ++lod)
    {
        const auto &entry = data.lods[lod];
        Require(entry.lod == lod && (entry.flags & kGeometryFlagSkinned) &&
            entry.error_method_abi == (lod == 0 ? kGeometryErrorMethodAbi : kSkinnedPoseErrorMethodAbi) &&
            (lod == 0 || entry.geometric_error >= data.lods[lod - 1].geometric_error) &&
            (lod == 0 || (entry.vertex_count < data.lods[lod - 1].vertex_count &&
                          entry.index_count < data.lods[lod - 1].index_count)) &&
            std::ranges::find(pages, entry.page_dependency_index) == pages.end(), "invalid or duplicate monster LOD page");
        (void)EncodeGeometryMetadata(entry, dependencies, {});
        pages.push_back(entry.page_dependency_index);
        for (const auto &clip : clips)
            for (std::uint32_t frame = 0; frame < clip.frame_count; ++frame)
            {
                const auto &bound = data.interval_bounds[row++];
                Require(bound.lod == lod && bound.clip == clip.clip && bound.frame == frame, "monster interval order mismatch");
                for (std::size_t axis = 0; axis < 3; ++axis)
                    Require(std::isfinite(bound.minimum[axis]) && std::isfinite(bound.maximum[axis]) &&
                        bound.minimum[axis] <= bound.maximum[axis], "invalid monster interval bounds");
            }
    }
}

std::array<std::vector<std::byte>, 4> EncodeMonster(const MonsterCharacterData &data, std::uint32_t bones,
    std::span<const std::string> dependencies)
{
    std::array<std::vector<std::byte>, 4> rows{Bytes(1,64), Bytes(data.final_matrices.size(),64),
        Bytes(data.interval_bounds.size(),32), Bytes(data.lods.size(),kGeometryMetadataStride)};
    Put(rows[0],0,data.abi,4); Put(rows[0],4,data.flags,4); Put(rows[0],8,static_cast<std::uint32_t>(data.final_matrices.size()),4);
    Put(rows[0],12,static_cast<std::uint32_t>(data.interval_bounds.size()),4); Put(rows[0],16,static_cast<std::uint32_t>(data.lods.size()),4);
    Put(rows[0],20,bones,4); PutFloat(rows[0],24,data.numerical_margin); PutFloat(rows[0],28,data.bubble_amplitude);
    std::copy(data.skeleton_binding.begin(),data.skeleton_binding.end(),rows[0].begin()+32);
    for (std::size_t i=0;i<data.final_matrices.size();++i)
        for (std::size_t scalar=0;scalar<16;++scalar) PutFloat(rows[1],i*64+scalar*4,data.final_matrices[i][scalar]);
    for (std::size_t i=0;i<data.interval_bounds.size();++i)
    {
        const auto &b=data.interval_bounds[i]; const auto at=i*32;
        Put(rows[2],at,b.lod,2); Put(rows[2],at+2,static_cast<std::uint8_t>(b.clip),1); Put(rows[2],at+4,b.frame,4);
        for(std::size_t axis=0;axis<3;++axis) { PutFloat(rows[2],at+8+axis*4,b.minimum[axis]); PutFloat(rows[2],at+20+axis*4,b.maximum[axis]); }
    }
    for(std::size_t i=0;i<data.lods.size();++i)
    {
        const auto row=EncodeGeometryMetadata(data.lods[i],dependencies,{});
        std::copy(row.begin(),row.end(),rows[3].begin()+i*kGeometryMetadataStride);
    }
    return rows;
}

std::optional<MonsterCharacterData> DecodeMonster(const PackageView &package)
{
    const auto find=[&](ChunkType type)->const ChunkView * {
        const auto it=std::ranges::find(package.chunks,type,&ChunkView::type);
        return it==package.chunks.end()?nullptr:&*it;
    };
    const auto header=find(ChunkType::CharacterDeformationMetadata);
    if(!header) return std::nullopt;
    const auto matrices=find(ChunkType::CharacterFinalMatrices), bounds=find(ChunkType::CharacterIntervalBounds), lods=find(ChunkType::CharacterLodDirectory);
    Require(matrices && bounds && lods,"incomplete monster deformation chunks");
    const auto h=header->bytes;
    Require(Get(h,8,4)==matrices->count && Get(h,12,4)==bounds->count && Get(h,16,4)==lods->count &&
        Get(h,20,4)>0 && Get(h,20,4)<=kMaxCharacterBones,"monster chunk counts disagree");
    MonsterCharacterData data; data.abi=Get(h,0,4); data.flags=Get(h,4,4); data.bone_count=Get(h,20,4);
    data.numerical_margin=GetFloat(h,24); data.bubble_amplitude=GetFloat(h,28);
    std::copy_n(h.begin()+32,32,data.skeleton_binding.begin());
    data.final_matrices.resize(matrices->count);
    for(std::size_t i=0;i<data.final_matrices.size();++i)
        for(std::size_t scalar=0;scalar<16;++scalar) data.final_matrices[i][scalar]=GetFloat(matrices->bytes,i*64+scalar*4);
    for(std::size_t i=0;i<bounds->count;++i)
    {
        const auto at=i*32; CharacterIntervalBound b;
        b.lod=static_cast<std::uint16_t>(Get(bounds->bytes,at,2)); b.clip=static_cast<CharacterAnimationClip>(Get(bounds->bytes,at+2,1));
        Require(Get(bounds->bytes,at+3,1)==0,"nonzero monster bound reserved field"); b.frame=Get(bounds->bytes,at+4,4);
        for(std::size_t axis=0;axis<3;++axis) { b.minimum[axis]=GetFloat(bounds->bytes,at+8+axis*4); b.maximum[axis]=GetFloat(bounds->bytes,at+20+axis*4); }
        data.interval_bounds.push_back(b);
    }
    for(std::size_t i=0;i<lods->count;++i)
        data.lods.push_back(DecodeGeometryMetadata(lods->bytes.subspan(i*kGeometryMetadataStride,kGeometryMetadataStride),package.dependencies,{}));
    return data;
}
} // namespace

namespace
{
std::vector<std::byte> BuildCharacterImpl(
    AssetId asset_id, std::span<const std::string> dependencies,
    const CharacterPackageData &character, const GeometryMetadata *geometry,
    std::span<const CharacterMaterialBinding> material_bindings = {},
    const MonsterCharacterData *monster = nullptr)
{
    Validate(character, std::span(character.vertices), false);
    Require(!monster || geometry,"embedded character cannot contain cooked monster deformation");
    if(monster) ValidateMonster(*monster,character.bone_count,character.clips,*geometry,dependencies);
    // Shared skeleton scalar encoding serves both layouts. Only the explicit
    // embedded layout allocates and serializes the expanded vertex chunk.
    std::vector<std::byte> geometry_bytes;
    if (geometry) geometry_bytes = EncodeGeometryMetadata(*geometry, dependencies, {});
    else
    {
        std::vector<ExpandedVertex> vertices;
        vertices.reserve(character.vertices.size());
        for (const auto &vertex : character.vertices)
            vertices.push_back({vertex.position, vertex.normal, vertex.bone_indices,
                                vertex.bone_weights, vertex.uv, vertex.tangent,
                                vertex.material_index, vertex.reserved});
        geometry_bytes = EncodeExpandedVertices(vertices);
    }
    auto metadata = Bytes(1, 16);
    Put(metadata, 0, character.bone_count, 4);
    Put(metadata, 4, character.material_count, 4);
    PutFloat(metadata, 8, character.ground_offset);
    Put(metadata, 12, static_cast<std::uint32_t>(character.transforms.size()), 4);
    auto parents = Bytes(character.bone_count, 2);
    auto inverse = Bytes(character.bone_count, 64);
    auto mask = Bytes(character.bone_count, 4);
    for (std::size_t i = 0; i < character.bone_count; ++i)
    {
        Put(parents, i * 2, character.parents[i], 2);
        for (std::size_t scalar = 0; scalar < 16; ++scalar)
            PutFloat(inverse, i * 64 + scalar * 4,
                     character.inverse_bind_matrices[i][scalar]);
        PutFloat(mask, i * 4, character.upper_body_weights[i]);
    }
    auto clips = Bytes(character.clips.size(), 16);
    for (std::size_t i = 0; i < character.clips.size(); ++i)
    {
        const auto &clip = character.clips[i];
        const auto offset = i * 16;
        Put(clips, offset, static_cast<std::uint8_t>(clip.clip), 1);
        Put(clips, offset + 1, clip.looping, 1);
        Put(clips, offset + 2, clip.reserved, 2);
        Put(clips, offset + 4, clip.first_transform, 4);
        Put(clips, offset + 8, clip.frame_count, 4);
        PutFloat(clips, offset + 12, clip.duration_seconds);
    }
    auto transforms = Bytes(monster ? 0 : character.transforms.size(), 40);
    for (std::size_t i = 0; !monster && i < character.transforms.size(); ++i)
    {
        const auto &transform = character.transforms[i];
        const auto offset = i * 40;
        for (std::size_t j = 0; j < 3; ++j)
        {
            PutFloat(transforms, offset + j * 4, transform.translation[j]);
            PutFloat(transforms, offset + 28 + j * 4, transform.scale[j]);
        }
        for (std::size_t j = 0; j < 4; ++j)
            PutFloat(transforms, offset + 12 + j * 4, transform.rotation[j]);
    }
    std::vector<ChunkSource> chunks{
        ChunkSource{geometry ? 13u : 1u, 1, true, geometry_bytes,
                    geometry ? 1u : character.vertices.size(),
                    geometry ? kGeometryMetadataStride : kExpandedVertexStride},
        ChunkSource{3, 1, true, metadata, 1, 16},
        ChunkSource{4, 1, true, parents, character.bone_count, 2},
        ChunkSource{5, 1, true, inverse, character.bone_count, 64},
        ChunkSource{6, 1, true, mask, character.bone_count, 4},
        ChunkSource{7, 1, true, clips, character.clips.size(), 16}};
    if(!monster) chunks.push_back({8,1,true,transforms,character.transforms.size(),40});
    std::vector<std::byte> binding_bytes;
    if (geometry)
    {
        binding_bytes = EncodeCharacterMaterialBindings(material_bindings, dependencies, character.material_count);
        chunks.push_back({static_cast<std::uint32_t>(ChunkType::CharacterMaterialBindings), 1, true,
            binding_bytes, material_bindings.size(), kCharacterMaterialBindingStride});
    }
    else Require(material_bindings.empty(), "embedded interchange cannot carry production material bindings");
    std::array<std::vector<std::byte>,4> deformation;
    if(monster)
    {
        Require(monster->final_matrices.size()==character.transforms.size(),"source and final palette counts disagree");
        deformation=EncodeMonster(*monster,character.bone_count,dependencies);
        chunks.push_back({37,1,true,deformation[0],1,64});
        chunks.push_back({38,1,true,deformation[1],monster->final_matrices.size(),64});
        chunks.push_back({39,1,true,deformation[2],monster->interval_bounds.size(),32});
        chunks.push_back({40,1,true,deformation[3],monster->lods.size(),kGeometryMetadataStride});
    }
    return BuildPackage(asset_id, dependencies, chunks);
}
} // namespace

namespace
{
CharacterPackageData DecodeCommon(
    const PackageView &package,
    bool validate_upper_body_mask, bool embedded, const MonsterCharacterData *monster = nullptr)
{
    std::array<const ChunkView *, static_cast<std::size_t>(ChunkType::CharacterLodDirectory) + 1> chunks{};
    for (const auto &chunk : package.chunks)
    {
        const auto type = static_cast<std::uint32_t>(chunk.type);
        Require(type < chunks.size() && !chunks[type], "duplicate or unsupported chunk");
        chunks[type] = &chunk;
    }
    Require(package.chunks.size() == (embedded ? 7u : monster ? 11u : 8u) &&
            (embedded ? chunks[1] && !chunks[13] && !chunks[25] : chunks[13] && !chunks[1] && chunks[25]) && chunks[3] && chunks[4] &&
            chunks[5] && chunks[6] && chunks[7] && !chunks[2] &&
            (monster ? !chunks[8] && chunks[37] && chunks[38] && chunks[39] && chunks[40] : chunks[8] != nullptr),
            "missing or unexpected character chunk");
    for (const auto &chunk : package.chunks)
        Require(chunk.required, "optional character field is unsupported");
    const auto &meta = chunks[3]->bytes;
    CharacterPackageData character;
    character.bone_count = Get(meta, 0, 4);
    character.material_count = Get(meta, 4, 4);
    character.ground_offset = GetFloat(meta, 8);
    const auto transform_count = Get(meta, 12, 4);
    Require(character.bone_count > 0 && character.bone_count <= kMaxCharacterBones &&
            character.material_count > 0 && character.material_count <= kMaxCharacterMaterials &&
            chunks[4]->count == character.bone_count &&
            chunks[5]->count == character.bone_count &&
            chunks[6]->count == character.bone_count &&
            transform_count > 0 && transform_count <= 800'000 &&
            (monster ? monster->final_matrices.size() : chunks[8]->count) == transform_count,
            "character chunk counts disagree");
    if (embedded)
        for (const auto &vertex : DecodeExpandedVertices(chunks[1]->bytes))
            character.vertices.push_back({vertex.position, vertex.normal,
                vertex.bone_indices, vertex.bone_weights, vertex.uv, vertex.tangent,
                vertex.material_index, vertex.reserved});
    for (std::size_t i = 0; i < character.bone_count; ++i)
    {
        character.parents.push_back(static_cast<std::uint16_t>(Get(chunks[4]->bytes, i * 2, 2)));
        std::array<float, 16> matrix{};
        for (std::size_t j = 0; j < 16; ++j)
            matrix[j] = GetFloat(chunks[5]->bytes, i * 64 + j * 4);
        character.inverse_bind_matrices.push_back(matrix);
        character.upper_body_weights.push_back(GetFloat(chunks[6]->bytes, i * 4));
    }
    for (std::size_t i = 0; i < chunks[7]->count; ++i)
    {
        const auto offset = i * 16;
        character.clips.push_back({
            static_cast<CharacterAnimationClip>(Get(chunks[7]->bytes, offset, 1)),
            static_cast<std::uint8_t>(Get(chunks[7]->bytes, offset + 1, 1)),
            static_cast<std::uint16_t>(Get(chunks[7]->bytes, offset + 2, 2)),
            Get(chunks[7]->bytes, offset + 4, 4),
            Get(chunks[7]->bytes, offset + 8, 4),
            GetFloat(chunks[7]->bytes, offset + 12)});
    }
    if(!monster) character.transforms.reserve(transform_count);
    for (std::size_t i = 0; !monster && i < transform_count; ++i)
    {
        const auto offset = i * 40;
        CharacterLocalTransform transform{};
        for (std::size_t j = 0; j < 3; ++j)
        {
            transform.translation[j] = GetFloat(chunks[8]->bytes, offset + j * 4);
            transform.scale[j] = GetFloat(chunks[8]->bytes, offset + 28 + j * 4);
        }
        for (std::size_t j = 0; j < 4; ++j)
            transform.rotation[j] = GetFloat(chunks[8]->bytes, offset + 12 + j * 4);
        character.transforms.push_back(transform);
    }
    Validate(character, std::span<const SkinnedVertex>(character.vertices), validate_upper_body_mask, embedded,
        monster ? monster->final_matrices.size() : 0);
    return character;
}
}

CharacterPackageData DecodeEmbeddedCharacterPackage(std::span<const std::byte> bytes,
    AssetId asset, bool validate_mask)
{ return DecodeCommon(ReadPackage(bytes, asset), validate_mask, true); }

void ValidateCharacterSource(const CharacterPackageData &character, bool validate_mask)
{ Validate(character, std::span(character.vertices), validate_mask); }

std::vector<std::byte> BuildEmbeddedCharacterPackage(AssetId asset,
    std::span<const std::string> deps, const CharacterPackageData &character)
{ return BuildCharacterImpl(asset, deps, character, nullptr); }

std::vector<std::byte> BuildCharacterPackage(AssetId asset,
    std::span<const std::string> deps, const CharacterPackageData &character)
{ return BuildEmbeddedCharacterPackage(asset, deps, character); }
CharacterPackageData DecodeCharacterPackage(std::span<const std::byte> bytes,
    AssetId asset, bool validate_mask)
{ return DecodeEmbeddedCharacterPackage(bytes, asset, validate_mask); }

std::vector<std::byte> BuildCharacterMetadataPackage(AssetId asset,
    std::span<const std::string> deps, const CharacterPackageData &character,
    const GeometryMetadata &geometry, std::span<const CharacterMaterialBinding> material_bindings,
    const MonsterCharacterData *monster)
{
    Require((geometry.flags & kGeometryFlagSkinned) && geometry.index_count == character.vertices.size() &&
            geometry.max_material_index < character.material_count, "character geometry metadata mismatch");
    Require(geometry.page_dependency_index < deps.size(), "missing character page dependency");
    const auto &page_id = deps[geometry.page_dependency_index];
    Require(page_id.starts_with("render/geometry/character/") && page_id.ends_with(".rpage"), "invalid character page product");
    const auto metadata_id = "render/" + page_id.substr(16,page_id.size()-16-6) + ".rpkg";
    Require(MakeAssetId(metadata_id).value == asset.value, "character page product identity mismatch");
    (void)EncodeGeometryMetadata(geometry, deps, metadata_id);
    return BuildCharacterImpl(asset, deps, character, &geometry, material_bindings, monster);
}

CharacterMetadataPackageData DecodeCharacterMetadataPackage(std::span<const std::byte> bytes,
    AssetId asset, bool validate_mask)
{
    return DecodeCharacterMetadataPackage(ReadPackage(bytes, asset), validate_mask);
}

CharacterMetadataPackageData DecodeCharacterMetadataPackage(const PackageView &view,
    bool validate_mask)
{
    auto monster = DecodeMonster(view);
    auto source = DecodeCommon(view, validate_mask, false, monster ? &*monster : nullptr);
    const auto found = std::ranges::find(view.chunks, ChunkType::GeometryMetadata, &ChunkView::type);
    Require(found != view.chunks.end(), "missing production geometry metadata");
    const auto geometry = DecodeGeometryMetadata(found->bytes, view.dependencies, {});
    const auto &page_id = view.dependencies[geometry.page_dependency_index];
    Require(page_id.starts_with("render/geometry/character/"), "invalid character page namespace");
    const auto metadata_id = "render/" + page_id.substr(16,page_id.size()-16-6) + ".rpkg";
    Require(MakeAssetId(metadata_id).value == view.asset_id.value, "character page product identity mismatch");
    Require((geometry.flags & kGeometryFlagSkinned) && geometry.max_material_index < source.material_count,
            "invalid character geometry metadata");
    const auto binding_chunk = std::ranges::find(view.chunks, ChunkType::CharacterMaterialBindings, &ChunkView::type);
    Require(binding_chunk != view.chunks.end(), "missing production material bindings");
    auto material_bindings = DecodeCharacterMaterialBindings(binding_chunk->bytes, view.dependencies, source.material_count);
    if(monster) ValidateMonster(*monster,source.bone_count,source.clips,geometry,view.dependencies);
    return {geometry, std::move(source.clips),std::move(source.parents),
            std::move(source.inverse_bind_matrices),std::move(source.upper_body_weights),
            std::move(source.transforms),source.bone_count,source.material_count,source.ground_offset,
            std::move(material_bindings),std::move(monster)};
}

void ValidateCharacterGeometry(const CharacterMetadataPackageData &character,
                               const GeometryData &geometry)
{
    ValidateGeometryPage(character.geometry, geometry);
    Require(geometry.flags & kGeometryFlagSkinned, "character page is not skinned");
    Validate(character, std::span(geometry.vertices), false, false,
        character.monster ? character.monster->final_matrices.size() : 0);
}

void ValidateCharacterGeometry(const CharacterMetadataPackageData &character,
                               const PreparedGeometryPage &page)
{
    const auto &geometry = page.geometry;
    const auto &metadata = character.geometry;
    Require(page.package.asset_id.value != 0 && geometry.flags == metadata.flags &&
            geometry.vertices.size() == metadata.vertex_count && geometry.indices.size() == metadata.index_count &&
            geometry.submeshes.size() == metadata.submesh_count && geometry.meshlets.size() == metadata.meshlet_count &&
            geometry.meshlet_vertex_refs.size() == metadata.vertex_ref_count &&
            geometry.bounds.minimum == metadata.bounds.minimum && geometry.bounds.maximum == metadata.bounds.maximum &&
            geometry.bounds.sphere == metadata.bounds.sphere, "prepared character page disagrees with metadata");
    std::uint32_t max_material = 0;
    for (const auto &submesh : geometry.submeshes) max_material = std::max(max_material, submesh.material_index);
    Require(max_material == metadata.max_material_index, "prepared character material maximum differs");
    Require(geometry.flags & kGeometryFlagSkinned, "character page is not skinned");
    Validate(character, std::span(geometry.vertices), false, false,
        character.monster ? character.monster->final_matrices.size() : 0);
}
} // namespace hs::render_data
