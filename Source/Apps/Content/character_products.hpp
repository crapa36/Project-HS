#pragma once

#include "character_ir.hpp"
#include <nlohmann/json_fwd.hpp>
#include <string_view>

namespace hs::content
{
struct CharacterMaterialSelection
{
    std::uint16_t slot{};
    std::uint32_t hero_ordinal{};
};

struct CharacterMaterialDomainSelection
{
    std::uint32_t hero_ordinal{};
    render_data::CharacterMaterialDomain domain{};
    render_data::MaterialSurfaceLayout surface{};
};

struct CharacterProduct
{
    std::string logical_id;
    std::string filename;
    std::vector<std::string> dependencies;
    std::vector<std::byte> bytes;
};

// Resolve only material identity against imported slots. The selected package's
// cooked domain is supplied separately to BuildCharacterProducts.
[[nodiscard]] std::vector<CharacterMaterialSelection> PrepareCharacterMaterialSelections(
    const CharacterIR &character, std::string_view stem,
    const nlohmann::json &authoring);

// LOD0 and any certified skinned LOD pages, skeleton/clip metadata and optional
// named animation are produced from the same owned import. A reduced page is
// published only after interval-wide pose and appearance error qualification.
[[nodiscard]] std::vector<CharacterProduct> BuildCharacterProducts(
    const CharacterIR &character, std::string_view stem,
    std::span<const CharacterMaterialSelection> materials,
    std::span<const CharacterMaterialDomainSelection> material_domains);

[[nodiscard]] render_data::MonsterCharacterData CookMonsterCharacterData(
    const CharacterIR &character, const render_data::GeometryData &geometry,
    const render_data::GeometryMetadata &metadata, bool suicide_bubble);

struct PreparedCharacterBinding
{
    std::string logical_id;
    // Chunk views borrow the checked immutable metadata bytes supplied to Prepare.
    render_data::PackageView package;
    render_data::CharacterMetadataPackageData character;
};

[[nodiscard]] PreparedCharacterBinding PrepareCharacterBinding(
    std::string_view logical_id, std::span<const std::byte> metadata_bytes);

} // namespace hs::content
