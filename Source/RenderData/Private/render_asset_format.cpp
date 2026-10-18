#include <hs/render_data/render_asset_format.hpp>
#include <hs/core/sha256.hpp>
#include <hs/render_data/material_schema.hpp>
#include <hs/render_data/geometry_package.hpp>
#include <hs/render_data/geometry_hierarchy.hpp>
#include <hs/render_data/character_animation_extension.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace hs::render_data
{
namespace
{
constexpr std::array<std::byte, 8> kMagic{std::byte{'H'}, std::byte{'S'},
    std::byte{'R'}, std::byte{'P'}, std::byte{'K'}, std::byte{'G'},
    std::byte{'1'}, std::byte{0}};
constexpr std::uint16_t kLittleEndian = 0x4c45;
constexpr std::size_t kHeaderSize = 96;
constexpr std::size_t kDirectoryStride = 80;
constexpr std::uint64_t kMaxPackageBytes = 64ull * 1024 * 1024;
constexpr std::uint64_t kMaxChunkBytes = 32ull * 1024 * 1024;
constexpr std::uint32_t kMaxChunks = 64;
constexpr std::uint32_t kMaxDependencies = 64;

void Require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error("render package: " + std::string(message));
}

std::uint64_t CheckedAdd(std::uint64_t a, std::uint64_t b)
{
    Require(a <= std::numeric_limits<std::uint64_t>::max() - b, "offset overflow");
    return a + b;
}

std::uint64_t CheckedMul(std::uint64_t a, std::uint64_t b)
{
    Require(!b || a <= std::numeric_limits<std::uint64_t>::max() / b, "size overflow");
    return a * b;
}

void Put(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value,
         std::size_t width)
{
    Require(offset <= bytes.size() && width <= bytes.size() - offset, "write range");
    for (std::size_t i = 0; i < width; ++i)
        bytes[offset + i] = std::byte((value >> (i * 8)) & 0xff);
}

std::uint64_t Get(std::span<const std::byte> bytes, std::uint64_t offset,
                  std::size_t width)
{
    Require(offset <= bytes.size() && width <= bytes.size() - offset, "read range");
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i)
        value |= std::uint64_t(std::to_integer<unsigned>(bytes[static_cast<std::size_t>(offset) + i])) << (i * 8);
    return value;
}

void ValidateVertex(const ExpandedVertex &vertex)
{
    const auto finite = [](float value) { return std::isfinite(value); };
    Require(std::ranges::all_of(vertex.position, finite) &&
            std::ranges::all_of(vertex.normal, finite) &&
            std::ranges::all_of(vertex.uv, finite) &&
            std::ranges::all_of(vertex.tangent, finite) &&
            std::ranges::all_of(vertex.bone_weights, finite),
            "nonfinite expanded vertex field");
    Require(std::ranges::all_of(vertex.bone_weights, [](float weight) {
                return weight >= 0.0f && weight <= 1.0f;
            }), "expanded vertex weight out of range");
    const float total_weight = vertex.bone_weights[0] + vertex.bone_weights[1] +
                               vertex.bone_weights[2] + vertex.bone_weights[3];
    Require(total_weight == 0.0f || std::abs(total_weight - 1.0f) <= 0.01f,
            "expanded vertex weights must be zero or normalized");
    Require(vertex.reserved == 0, "expanded vertex reserved field is invalid");
}

void PutFloat(std::span<std::byte> bytes, std::size_t offset, float value)
{
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    Put(bytes, offset, std::bit_cast<std::uint32_t>(value), 4);
}

float GetFloat(std::span<const std::byte> bytes, std::size_t offset)
{
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    return std::bit_cast<float>(static_cast<std::uint32_t>(Get(bytes, offset, 4)));
}

ExpandedVertex DecodeVertex(std::span<const std::byte> bytes, std::size_t offset)
{
    ExpandedVertex vertex{};
    for (std::size_t i = 0; i < 3; ++i)
    {
        vertex.position[i] = GetFloat(bytes, offset + i * 4);
        vertex.normal[i] = GetFloat(bytes, offset + 12 + i * 4);
    }
    for (std::size_t i = 0; i < 4; ++i)
    {
        vertex.bone_indices[i] = static_cast<std::uint16_t>(Get(bytes, offset + 24 + i * 2, 2));
        vertex.bone_weights[i] = GetFloat(bytes, offset + 32 + i * 4);
        vertex.tangent[i] = GetFloat(bytes, offset + 56 + i * 4);
    }
    for (std::size_t i = 0; i < 2; ++i)
        vertex.uv[i] = GetFloat(bytes, offset + 48 + i * 4);
    vertex.material_index = static_cast<std::uint16_t>(Get(bytes, offset + 72, 2));
    vertex.reserved = static_cast<std::uint16_t>(Get(bytes, offset + 74, 2));
    ValidateVertex(vertex);
    return vertex;
}

void ValidateEncodedVertices(std::span<const std::byte> bytes)
{
    Require(!bytes.empty() && bytes.size() % kExpandedVertexStride == 0,
            "invalid expanded vertex byte count");
    for (std::size_t offset = 0; offset < bytes.size(); offset += kExpandedVertexStride)
        (void)DecodeVertex(bytes, offset);
}

bool ValidId(std::string_view id)
{
    if (id.empty() || id.size() > 128 || id.front() == '/' || id.back() == '/') return false;
    bool separator = false;
    for (unsigned char ch : id)
    {
        if (ch == '/')
        {
            if (separator) return false;
            separator = true;
        }
        else
        {
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') ||
                  ch == '_' || ch == '-' || ch == '.')) return false;
            separator = false;
        }
    }
    return id.find("..") == std::string_view::npos;
}

constexpr auto kLastKnownChunk = static_cast<std::uint32_t>(ChunkType::AutoFgCadence);

bool Known(std::uint32_t type)
{
    return type >= static_cast<std::uint32_t>(ChunkType::ExpandedVertices) &&
           type <= kLastKnownChunk && (type < 37 || type > 42);
}

void ValidateKnown(std::uint32_t type, std::uint16_t version,
                   std::uint64_t size, std::uint64_t count, std::uint32_t stride)
{
    Require(version == (type == static_cast<std::uint32_t>(ChunkType::QualityProfiles) ||
                               type == static_cast<std::uint32_t>(ChunkType::DecalMaterialMetadata) ? 2 : 1),
            "unsupported known chunk version");
    if (type == static_cast<std::uint32_t>(ChunkType::ExpandedVertices))
        Require(stride == kExpandedVertexStride && count > 0 && count <= 3'000'000 &&
                count % 3 == 0 && size == CheckedMul(count, stride), "invalid vertex chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::MaterialMetadata))
        Require((stride == kMaterialMetadataStride ||
                 stride == kHistoricalMaterialMetadataStride) &&
                count == 1 && size == stride,
                "invalid material chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::CharacterMetadata))
        Require(stride == 16 && count == 1 && size == stride, "invalid character metadata chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::CharacterParents))
        Require(stride == 2 && count <= 128 && size == CheckedMul(count, stride),
                "invalid character parents chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::CharacterInverseBind))
        Require(stride == 64 && count <= 128 && size == CheckedMul(count, stride),
                "invalid character inverse bind chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::CharacterUpperBodyMask))
        Require(stride == 4 && count <= 128 && size == CheckedMul(count, stride),
                "invalid character upper body mask chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::CharacterClips))
        Require(stride == 16 && count == 5 && size == CheckedMul(count, stride),
                "invalid character clips chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::CharacterLocalTransforms))
        Require(stride == 40 && count <= 1'000'000 && size == CheckedMul(count, stride),
                "invalid character transforms chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::CharacterAnimationMetadata))
        Require(stride == kCharacterAnimationMetadataStride && count == 1 && size == stride,
                "invalid character animation metadata chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::CharacterNamedClips))
        Require(stride == kCharacterNamedClipStride && count > 0 && count <= 128 &&
                size == CheckedMul(count, stride), "invalid named character clips chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::CharacterAnimationNames))
        Require(stride == 1 && count > 0 && count <= 128 * 1024 && size == count,
                "invalid character animation names chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::CharacterAnimationSockets))
        Require(stride == kCharacterAnimationSocketStride && count > 0 && count <= 128 &&
                size == CheckedMul(count, stride), "invalid character animation sockets chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::TextureMetadata))
        Require(count == 1 && stride == size && size >= 56 && size <= 40 + 4096 * 16,
                "invalid texture metadata chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::TextureSubresources))
        Require((stride == 1 || stride == 8 || stride == 16) &&
                count > 0 && size == CheckedMul(count, stride),
                "invalid texture subresources chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::MaterialProgram))
        Require(stride == kMaterialProgramStride && (count == 1 || count == 2 || count == 4) &&
                size == CheckedMul(count, stride),
                "invalid material program chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::MaterialBindings))
        Require(stride == kMaterialBindingStride && (count == 4 || count == 6) &&
                size == CheckedMul(count, stride), "invalid material bindings chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::MaterialParameters))
        Require(stride == 1 && count <= 4096u && count == size,
                "invalid material parameters chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::MaterialParameterLayout))
        Require(stride == 32 && count == 1 && size == count * stride,
                "invalid material parameter layout chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::MaterialSurfaceLayout))
        Require(stride == kMaterialSurfaceLayoutStride && count == 1 && size == stride,
                "invalid material surface layout chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::DecalMaterialMetadata))
        Require(stride == 28 && count == 1 && size == stride,
                "invalid decal material metadata chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::GeometryHierarchyReference))
        Require(stride == kGeometryHierarchyReferenceStride && count == 1 && size == stride,
                "invalid geometry hierarchy reference chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::CharacterMaterialBindings))
        Require(stride == kCharacterMaterialBindingStride && count > 0 && count <= 131070u &&
                size == CheckedMul(count, stride), "invalid character material binding chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::LightingProbeMetadata))
        Require(stride == 64 && count == 1 && size == stride, "invalid lighting probe metadata chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::LightingProbeRadiance))
        Require(stride == 16 && count > 0 && count <= 1'000'000 &&
                size == CheckedMul(count, stride), "invalid lighting probe radiance chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::LightingProbeIrradiance))
        Require(stride == 16 && count == 9 && size == 144, "invalid lighting probe irradiance chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::LtcLutMetadata))
        Require(stride == 64 && count == 1 && size == stride, "invalid LTC metadata chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::LtcLutMatrix) ||
             type == static_cast<std::uint32_t>(ChunkType::LtcLutAmplitude))
        Require(stride == 16 && count == 4096 && size == count * stride, "invalid LTC LUT chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::VolumetricVolumes))
        Require(stride == 112 && count <= 256 && size == count * stride, "invalid volumetric medium chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::QualityProfiles))
        Require(stride == 140 && count == 4 && size == count * stride, "invalid quality profiles chunk");
    else if (type == static_cast<std::uint32_t>(ChunkType::AutoFgCadence))
        Require(stride == 12 && count == 1 && size == stride, "invalid Auto FG cadence chunk");
    else if (type == 13)
        Require(stride == kGeometryMetadataStride && count == 1 && size == stride,
                "invalid geometry metadata chunk");
    else if (type == 14)
        Require(stride == kExpandedVertexStride && count > 0 && count <= kMaxGeometryVertices &&
                size == CheckedMul(count, stride), "invalid unique geometry vertices chunk");
    else if (type == 15)
        Require(stride == 4 && count > 0 && count <= kMaxGeometryIndices && count % 3 == 0 &&
                size == CheckedMul(count, stride), "invalid geometry indices chunk");
    else if (type >= 16 && type <= 20)
    {
        const auto expected_stride = type == 16 ? kGeometrySubmeshStride :
                                     type == 17 ? kGeometryMeshletStride : 4;
        Require(stride == expected_stride && count > 0 &&
                count <= (type == 18 ? kMaxGeometryIndices : kMaxGeometryIndices / 3) &&
                size == CheckedMul(count, stride), "invalid geometry component chunk");
    }
}
} // namespace

std::vector<std::byte> EncodeExpandedVertices(std::span<const ExpandedVertex> vertices)
{
    Require(!vertices.empty() &&
            vertices.size() <= std::numeric_limits<std::size_t>::max() / kExpandedVertexStride,
            "invalid expanded vertex count");
    std::vector<std::byte> bytes(vertices.size() * kExpandedVertexStride);
    for (std::size_t index = 0; index < vertices.size(); ++index)
    {
        const auto &vertex = vertices[index];
        ValidateVertex(vertex);
        const auto offset = index * kExpandedVertexStride;
        for (std::size_t i = 0; i < 3; ++i)
        {
            PutFloat(bytes, offset + i * 4, vertex.position[i]);
            PutFloat(bytes, offset + 12 + i * 4, vertex.normal[i]);
        }
        for (std::size_t i = 0; i < 4; ++i)
        {
            Put(bytes, offset + 24 + i * 2, vertex.bone_indices[i], 2);
            PutFloat(bytes, offset + 32 + i * 4, vertex.bone_weights[i]);
            PutFloat(bytes, offset + 56 + i * 4, vertex.tangent[i]);
        }
        for (std::size_t i = 0; i < 2; ++i)
            PutFloat(bytes, offset + 48 + i * 4, vertex.uv[i]);
        Put(bytes, offset + 72, vertex.material_index, 2);
        Put(bytes, offset + 74, vertex.reserved, 2);
    }
    return bytes;
}

std::vector<ExpandedVertex> DecodeExpandedVertices(std::span<const std::byte> bytes)
{
    Require(!bytes.empty() && bytes.size() % kExpandedVertexStride == 0,
            "invalid expanded vertex byte count");
    std::vector<ExpandedVertex> vertices;
    vertices.reserve(bytes.size() / kExpandedVertexStride);
    for (std::size_t offset = 0; offset < bytes.size(); offset += kExpandedVertexStride)
        vertices.push_back(DecodeVertex(bytes, offset));
    return vertices;
}

std::array<std::byte, 32> Sha256(std::span<const std::byte> bytes)
{
    return hs::Sha256(bytes);
}

std::vector<std::byte> BuildPackage(AssetId asset_id,
                                    std::span<const std::string> dependencies,
                                    std::span<const ChunkSource> chunks)
{
    Require(asset_id.value != 0 && dependencies.size() <= kMaxDependencies &&
            !chunks.empty() && chunks.size() <= kMaxChunks, "invalid package shape");
    std::uint64_t dependencies_size = 0;
    std::string_view previous;
    for (const auto &id : dependencies)
    {
        Require(ValidId(id) && (previous.empty() || previous < id),
                "dependency IDs must be sorted and unique");
        dependencies_size = CheckedAdd(dependencies_size, 2 + id.size());
        previous = id;
    }
    const auto directory_offset = CheckedAdd(kHeaderSize, dependencies_size);
    const auto directory_end = CheckedAdd(directory_offset,
                                         CheckedMul(chunks.size(), kDirectoryStride));
    auto total = directory_end;
    for (const auto &chunk : chunks)
    {
        Require(chunk.bytes.size() <= kMaxChunkBytes && chunk.stride != 0 &&
                chunk.count != 0 && CheckedMul(chunk.count, chunk.stride) == chunk.bytes.size(),
                "invalid chunk size or stride");
        if (Known(chunk.type))
        {
            ValidateKnown(chunk.type, chunk.version,
                          chunk.bytes.size(), chunk.count, chunk.stride);
            if (chunk.type == static_cast<std::uint32_t>(ChunkType::ExpandedVertices) || chunk.type == 14)
                ValidateEncodedVertices(chunk.bytes);
            else if (chunk.type == static_cast<std::uint32_t>(ChunkType::GeometryHierarchyReference))
                (void)DecodeGeometryHierarchyReference(chunk.bytes, dependencies);
        }
        total = CheckedAdd(total, chunk.bytes.size());
    }
    Require(total <= kMaxPackageBytes, "package exceeds size limit");
    std::vector<std::byte> bytes(static_cast<std::size_t>(total));
    std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
    Put(bytes, 8, kRenderPackageVersion, 2);
    Put(bytes, 10, kLittleEndian, 2);
    Put(bytes, 12, kHeaderSize, 4);
    Put(bytes, 16, asset_id.value, 8);
    Put(bytes, 56, directory_offset, 8);
    Put(bytes, 64, chunks.size(), 4);
    Put(bytes, 68, kDirectoryStride, 4);
    Put(bytes, 72, kHeaderSize, 8);
    Put(bytes, 80, dependencies.size(), 4);
    Put(bytes, 88, total, 8);
    std::size_t cursor = kHeaderSize;
    for (const auto &id : dependencies)
    {
        Put(bytes, cursor, id.size(), 2);
        cursor += 2;
        std::memcpy(bytes.data() + cursor, id.data(), id.size());
        cursor += id.size();
    }
    cursor = static_cast<std::size_t>(directory_end);
    for (std::size_t i = 0; i < chunks.size(); ++i)
    {
        const auto &chunk = chunks[i];
        const auto entry = static_cast<std::size_t>(directory_offset) + i * kDirectoryStride;
        Put(bytes, entry, chunk.type, 4);
        Put(bytes, entry + 4, chunk.version, 2);
        Put(bytes, entry + 6, chunk.required ? 1 : 0, 2);
        Put(bytes, entry + 8, cursor, 8);
        Put(bytes, entry + 16, chunk.bytes.size(), 8);
        Put(bytes, entry + 24, chunk.bytes.size(), 8); // v1 is uncompressed.
        Put(bytes, entry + 32, chunk.count, 8);
        Put(bytes, entry + 40, chunk.stride, 4);
        const auto digest = Sha256(chunk.bytes);
        std::copy(digest.begin(), digest.end(), bytes.begin() + entry + 48);
        std::copy(chunk.bytes.begin(), chunk.bytes.end(), bytes.begin() + cursor);
        cursor += chunk.bytes.size();
    }
    const auto digest = Sha256(std::span<const std::byte>(bytes).subspan(kHeaderSize));
    std::copy(digest.begin(), digest.end(), bytes.begin() + 24);
    return bytes;
}

PackageView ReadPackage(std::span<const std::byte> bytes, AssetId expected_asset_id)
{
    Require(bytes.size() >= kHeaderSize && bytes.size() <= kMaxPackageBytes,
            "invalid package size");
    Require(std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) &&
            Get(bytes, 8, 2) == kRenderPackageVersion &&
            Get(bytes, 10, 2) == kLittleEndian &&
            Get(bytes, 12, 4) == kHeaderSize, "header magic, version, or endian mismatch");
    const AssetId asset{Get(bytes, 16, 8)};
    Require(asset.value != 0 && asset.value == expected_asset_id.value,
            "asset ID mismatch");
    Require(Get(bytes, 84, 4) == 0 && Get(bytes, 88, 8) == bytes.size(),
            "reserved header or package size mismatch");
    const auto digest = Sha256(bytes.subspan(kHeaderSize));
    Require(std::equal(digest.begin(), digest.end(), bytes.begin() + 24),
            "package content hash mismatch");
    const auto directory_offset = Get(bytes, 56, 8);
    const auto count = Get(bytes, 64, 4);
    const auto stride = Get(bytes, 68, 4);
    const auto dependency_offset = Get(bytes, 72, 8);
    const auto dependency_count = Get(bytes, 80, 4);
    Require(count > 0 && count <= kMaxChunks && stride == kDirectoryStride &&
            dependency_count <= kMaxDependencies && dependency_offset == kHeaderSize &&
            directory_offset <= bytes.size(), "invalid directory or dependency metadata");
    const auto directory_end = CheckedAdd(directory_offset, CheckedMul(count, stride));
    Require(directory_end <= bytes.size(), "directory exceeds package");
    PackageView view{asset};
    std::uint64_t cursor = dependency_offset;
    std::string previous;
    for (std::uint64_t i = 0; i < dependency_count; ++i)
    {
        Require(cursor <= directory_offset && directory_offset - cursor >= 2,
                "truncated dependency");
        const auto length = Get(bytes, cursor, 2);
        cursor = CheckedAdd(cursor, 2);
        Require(length > 0 && length <= 128 && length <= directory_offset - cursor,
                "invalid dependency length");
        std::string id(reinterpret_cast<const char *>(bytes.data() + cursor),
                       static_cast<std::size_t>(length));
        Require(ValidId(id) && (previous.empty() || previous < id),
                "invalid dependency ID order");
        view.dependencies.push_back(std::move(id));
        previous = view.dependencies.back();
        cursor = CheckedAdd(cursor, length);
    }
    Require(cursor == directory_offset, "dependency range does not end at directory");
    std::uint64_t previous_end = directory_end;
    std::array<bool, static_cast<std::size_t>(kLastKnownChunk) + 1> saw_known{};
    for (std::uint64_t i = 0; i < count; ++i)
    {
        const auto entry = directory_offset + i * stride;
        const auto type = static_cast<std::uint32_t>(Get(bytes, entry, 4));
        const auto version = static_cast<std::uint16_t>(Get(bytes, entry + 4, 2));
        const auto flags = Get(bytes, entry + 6, 2);
        const auto offset = Get(bytes, entry + 8, 8);
        const auto stored_size = Get(bytes, entry + 16, 8);
        const auto raw_size = Get(bytes, entry + 24, 8);
        const auto item_count = Get(bytes, entry + 32, 8);
        const auto item_stride = static_cast<std::uint32_t>(Get(bytes, entry + 40, 4));
        Require(flags <= 1 && Get(bytes, entry + 44, 4) == 0 &&
                offset >= previous_end && stored_size > 0 &&
                stored_size <= kMaxChunkBytes && raw_size <= kMaxChunkBytes &&
                item_count > 0 && item_stride > 0 &&
                CheckedMul(item_count, item_stride) == raw_size &&
                CheckedAdd(offset, stored_size) <= bytes.size(), "invalid chunk directory entry");
        previous_end = CheckedAdd(offset, stored_size);
        const auto chunk_bytes = bytes.subspan(static_cast<std::size_t>(offset),
                                               static_cast<std::size_t>(stored_size));
        const auto chunk_hash = Sha256(chunk_bytes);
        Require(std::equal(chunk_hash.begin(), chunk_hash.end(), bytes.begin() + entry + 48),
                "chunk hash mismatch");
        if (!Known(type) || version != (type == static_cast<std::uint32_t>(ChunkType::QualityProfiles) ||
                               type == static_cast<std::uint32_t>(ChunkType::DecalMaterialMetadata) ? 2 : 1))
        {
            Require(flags == 0, Known(type) ? "unsupported required chunk version" : "unknown required chunk");
            continue;
        }
        Require(stored_size == raw_size, "compressed known chunk is unsupported");
        ValidateKnown(type, version, raw_size, item_count, item_stride);
        Require(!saw_known[type], "duplicate known chunk");
        saw_known[type] = true;
        if (type == static_cast<std::uint32_t>(ChunkType::ExpandedVertices) || type == 14)
            ValidateEncodedVertices(chunk_bytes);
        else if (type == static_cast<std::uint32_t>(ChunkType::MaterialMetadata))
            (void)DecodeRockMaterial(chunk_bytes, view.dependencies);
        else if (type == static_cast<std::uint32_t>(ChunkType::MaterialProgram))
            (void)DecodeMaterialPrograms(chunk_bytes, view.dependencies);
        else if (type == static_cast<std::uint32_t>(ChunkType::MaterialParameters))
            (void)DecodeMaterialParameters(chunk_bytes);
        else if (type == static_cast<std::uint32_t>(ChunkType::MaterialSurfaceLayout))
            (void)DecodeMaterialSurfaceLayout(chunk_bytes);
        else if (type == static_cast<std::uint32_t>(ChunkType::GeometryHierarchyReference))
            (void)DecodeGeometryHierarchyReference(chunk_bytes, view.dependencies);
        view.chunks.push_back({static_cast<ChunkType>(type), version, flags == 1, chunk_bytes,
                               item_count, item_stride});
    }
    Require(previous_end == bytes.size(), "trailing bytes after chunks");
    return view;
}
} // namespace hs::render_data
