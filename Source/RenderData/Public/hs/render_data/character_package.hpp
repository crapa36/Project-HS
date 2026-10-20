#pragma once

#include <hs/core/cooked_format.hpp>
#include <hs/render_data/render_asset_format.hpp>
#include <hs/render_data/geometry_package.hpp>
#include <hs/render_data/material_schema.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
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
};

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
    std::span<const CharacterMaterialBinding> material_bindings = {});
[[nodiscard]] CharacterMetadataPackageData DecodeCharacterMetadataPackage(
    std::span<const std::byte> bytes, AssetId expected_asset_id,
    bool validate_upper_body_mask = true);
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
