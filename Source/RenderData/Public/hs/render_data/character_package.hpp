#pragma once

#include <hs/core/cooked_format.hpp>
#include <hs/render_data/render_asset_format.hpp>
#include <hs/render_data/geometry_package.hpp>
#include <hs/render_data/material_schema.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hs::render_data
{
// Runtime-facing values are the existing character types. The package codec
// writes and reads each scalar explicitly; their C++ object layout is not a
// property of HSRPKG.
struct CharacterPackageData
{
    std::vector<SkinnedVertex> vertices;
    std::vector<CharacterClipHeader> clips;
    std::vector<std::uint16_t> parents;
    std::vector<std::array<float, 16>> inverse_bind_matrices;
    std::vector<float> upper_body_weights;
    std::vector<CharacterLocalTransform> transforms;
    std::uint32_t bone_count{};
    std::uint32_t material_count{};
    float ground_offset{};
};

inline constexpr std::uint32_t kMonsterDeformationAbi = 1;
// ABI1 proves position enclosure only for ordinary matrix skinning. Bubble
// still uses the existing runtime reduction because normal singularities are
// not qualified. Terminal frame rows enclose the clamped last frame, not wrap.
inline constexpr std::uint32_t kMonsterBoundsPositionEnclosure = 1;
inline constexpr std::uint32_t kMonsterDeformationSuicideBubble = 2;
struct CharacterIntervalBound
{
    std::uint16_t lod{};
    CharacterAnimationClip clip{};
    std::uint32_t frame{};
    std::array<float, 3> minimum{}, maximum{};
};
struct MonsterCharacterData
{
    std::uint32_t abi{kMonsterDeformationAbi};
    std::uint32_t bone_count{};
    std::uint32_t flags{};
    std::array<std::byte, 32> skeleton_binding{};
    float numerical_margin{};
    float bubble_amplitude{};
    // Stored exactly as the old XMStoreFloat4x4(transpose(skin)) upload rows.
    // Clip first_transform is the first matrix; frame-major, bone-major order.
    std::vector<std::array<float, 16>> final_matrices;
    std::vector<CharacterIntervalBound> interval_bounds;
    // Actual pages only, in contiguous LOD order starting at zero. No synthetic
    // reduced entry is admitted merely because a renderer can select an LOD.
    std::vector<GeometryMetadata> lods;
};

// Production metadata deliberately contains no embedded geometry. The page is
// required and validated separately before its vertices can be used.
struct CharacterMetadataPackageData
{
    GeometryMetadata geometry;
    std::vector<CharacterClipHeader> clips;
    std::vector<std::uint16_t> parents;
    std::vector<std::array<float, 16>> inverse_bind_matrices;
    std::vector<float> upper_body_weights;
    std::vector<CharacterLocalTransform> transforms;
    std::uint32_t bone_count{};
    std::uint32_t material_count{};
    float ground_offset{};
    std::vector<CharacterMaterialBinding> material_bindings;
    std::optional<MonsterCharacterData> monster;
};

// Build-only import boundary, using the same strict scalar predicates as the
// character codec before any geometry remap or publication.
void ValidateCharacterSource(const CharacterPackageData &character,
                             bool validate_upper_body_mask = true);

[[nodiscard]] std::vector<std::byte> BuildEmbeddedCharacterPackage(
    AssetId asset_id, std::span<const std::string> dependencies,
    const CharacterPackageData &character);
[[nodiscard]] CharacterPackageData DecodeEmbeddedCharacterPackage(
    std::span<const std::byte> bytes, AssetId expected_asset_id,
    bool validate_upper_body_mask = true);
// Production metadata requires one opaque binding for every FBX slot plus the
// jelly body's transparent binding. Old metadata without required chunk25 must
// be recooked; embedded interchange above keeps its existing seven chunks.
[[nodiscard]] std::vector<std::byte> BuildCharacterMetadataPackage(
    AssetId asset_id, std::span<const std::string> dependencies,
    const CharacterPackageData &character, const GeometryMetadata &geometry,
    std::span<const CharacterMaterialBinding> material_bindings = {},
    const MonsterCharacterData *monster = nullptr);
[[nodiscard]] CharacterMetadataPackageData DecodeCharacterMetadataPackage(
    std::span<const std::byte> bytes, AssetId expected_asset_id,
    bool validate_upper_body_mask = true);
[[nodiscard]] CharacterMetadataPackageData DecodeCharacterMetadataPackage(
    const PackageView &package, bool validate_upper_body_mask = true);
void ValidateCharacterGeometry(const CharacterMetadataPackageData &character,
                               const GeometryData &geometry);

// Backward-compatible embedded interchange aliases; production uses metadata.
[[nodiscard]] std::vector<std::byte> BuildCharacterPackage(
    AssetId asset_id, std::span<const std::string> dependencies,
    const CharacterPackageData &character);
[[nodiscard]] CharacterPackageData DecodeCharacterPackage(
    std::span<const std::byte> bytes, AssetId expected_asset_id,
    bool validate_upper_body_mask = true);
} // namespace hs::render_data
