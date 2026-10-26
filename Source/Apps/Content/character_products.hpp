#pragma once

#include "character_ir.hpp"
#include <string_view>

namespace hs::content
{
struct CharacterMaterialSelection
{
    std::uint16_t slot{};
    std::uint32_t hero_ordinal{};
    render_data::HeroMaterialDomain domain{};
};

struct CharacterProduct
{
    std::string logical_id;
    std::string filename;
    std::vector<std::string> dependencies;
    std::vector<std::byte> bytes;
};

// Resolve authoring rules once against the imported material slots. No JSON
// binding sidecar is written for another producer to parse again.
[[nodiscard]] std::vector<CharacterMaterialSelection> PrepareCharacterMaterialSelections(
    const CharacterIR &character, std::string_view stem,
    const nlohmann::json &authoring);

// LOD0 and any certified skinned LOD pages, skeleton/clip metadata and optional
// named animation are produced from the same owned import. A reduced page is
// published only after interval-wide pose and appearance error qualification.
[[nodiscard]] std::vector<CharacterProduct> BuildCharacterProducts(
    const CharacterIR &character, std::string_view stem,
    std::span<const CharacterMaterialSelection> materials);

[[nodiscard]] render_data::MonsterCharacterData CookMonsterCharacterData(
    const CharacterIR &character, const render_data::GeometryData &geometry,
    const render_data::GeometryMetadata &metadata, bool suicide_bubble);

struct CharacterDependencyProduct
{
    std::string_view logical_id;
    std::span<const std::byte> bytes;
};

struct PreparedCharacterBinding
{
    std::string logical_id;
    // Chunk views borrow the checked immutable metadata bytes supplied to Prepare.
    render_data::PackageView package;
    render_data::CharacterMetadataPackageData character;
};

[[nodiscard]] PreparedCharacterBinding PrepareCharacterBinding(
    std::string_view logical_id, std::span<const std::byte> metadata_bytes);

// Build/image-link boundary after all referenced shader/material/texture products
// exist. Callers supply bytes from their checked immutable product snapshots.
// Requires the referenced page, material closures, slot textures, optional PBR
// textures and Archer animation; no source file or legacy exchange is reopened.
void ValidateCharacterProductBindings(const PreparedCharacterBinding &prepared,
    std::span<const CharacterDependencyProduct> products);
} // namespace hs::content
