#pragma once

#include <hs/core/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace hs::render_data
{
inline constexpr std::uint16_t kRenderPackageVersion = 1;
inline constexpr std::uint32_t kExpandedVertexStride = 76;
inline constexpr std::uint32_t kMaterialMetadataStride = 40;
// v1-v3 material chunks used a 36-byte record. They remain readable through
// the explicit historical decoder, while newly encoded records use v4/40.
inline constexpr std::uint32_t kHistoricalMaterialMetadataStride = 36;
inline constexpr std::uint32_t kMaterialProgramStride = 4;
inline constexpr std::uint32_t kMaterialBindingStride = 12;
inline constexpr std::uint32_t kMaterialSurfaceLayoutStride = 16;
inline constexpr std::uint32_t kGeometryHierarchyReferenceStride = 4;
inline constexpr std::uint32_t kCharacterMaterialBindingStride = 8;

// ExpandedVertices v1 uses these fields, never a C++ struct's object bytes.
// Offsets: position 0, normal 12, bone indices 24, weights 32, UV 48,
// tangent 56, material index 72, reserved 74. All scalars are little-endian.
struct ExpandedVertex
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<std::uint16_t, 4> bone_indices{};
    std::array<float, 4> bone_weights{};
    std::array<float, 2> uv{};
    std::array<float, 4> tangent{};
    std::uint16_t material_index{};
    std::uint16_t reserved{};
};

void ValidateExpandedVertices(std::span<const ExpandedVertex> vertices);
[[nodiscard]] std::vector<std::byte> EncodeExpandedVertices(
    std::span<const ExpandedVertex> vertices);
[[nodiscard]] std::vector<ExpandedVertex> DecodeExpandedVertices(
    std::span<const std::byte> bytes);

enum class ChunkType : std::uint32_t
{
    ExpandedVertices = 1,
    MaterialMetadata = 2,
    CharacterMetadata = 3,
    CharacterParents = 4,
    CharacterInverseBind = 5,
    CharacterUpperBodyMask = 6,
    CharacterClips = 7,
    CharacterLocalTransforms = 8,
    TextureMetadata = 9,
    TextureSubresources = 10,
    MaterialProgram = 11,
    MaterialBindings = 12,
    MaterialParameters = 21,
    MaterialParameterLayout = 22,
    MaterialSurfaceLayout = 23,
    GeometryHierarchyReference = 24,
    CharacterMaterialBindings = 25,
    LightingProbeMetadata = 26,
    LightingProbeRadiance = 27,
    LightingProbeIrradiance = 28,
    LtcLutMetadata = 29,
    LtcLutMatrix = 30,
    LtcLutAmplitude = 31,
    VolumetricVolumes = 32,
    CharacterAnimationMetadata = 33,
    CharacterNamedClips = 34,
    CharacterAnimationNames = 35,
    CharacterAnimationSockets = 36,
    CharacterDeformationMetadata = 37,
    CharacterFinalMatrices = 38,
    CharacterIntervalBounds = 39,
    CharacterLodDirectory = 40,
    DecalMaterialMetadata = 43,
    QualityProfiles = 44,
    AutoFgCadence = 45,
    GeometryMetadata = 13,
    GeometryVertices = 14,
    GeometryIndices = 15,
    GeometrySubmeshes = 16,
    GeometryMeshlets = 17,
    GeometryMeshletVertexRefs = 18,
    GeometryMeshletTriangles = 19,
    GeometryCanonicalPrimitiveRemap = 20,
};

struct ChunkSource
{
    std::uint32_t type{};
    std::uint16_t version{1};
    bool required{true};
    std::span<const std::byte> bytes;
    std::uint64_t count{};
    std::uint32_t stride{};
};

struct ChunkView
{
    ChunkType type{};
    std::uint16_t version{};
    bool required{};
    std::span<const std::byte> bytes;
    std::uint64_t count{};
    std::uint32_t stride{};
};

// Views refer to the caller-owned package bytes; retain them while using ChunkView.
struct PackageView
{
    AssetId asset_id{};
    std::vector<std::string> dependencies;
    std::vector<ChunkView> chunks;
};

// Portable little-endian v1 package. Throws std::runtime_error on invalid input.
[[nodiscard]] std::vector<std::byte> BuildPackage(
    AssetId asset_id, std::span<const std::string> dependencies,
    std::span<const ChunkSource> chunks);
[[nodiscard]] PackageView ReadPackage(std::span<const std::byte> bytes,
                                      AssetId expected_asset_id);
[[nodiscard]] std::array<std::byte, 32> Sha256(std::span<const std::byte> bytes);
} // namespace hs::render_data
