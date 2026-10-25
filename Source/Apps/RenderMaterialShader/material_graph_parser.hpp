#pragma once

#include <hs/render_data/material_ir.hpp>

#include <string_view>

namespace hs::material_content
{
struct MaterialDefinition
{
    render_data::RockMaterial material;
    render_data::PreparedMaterial prepared;
};
[[nodiscard]] MaterialDefinition ParseMaterialDefinition(
    std::string_view name, std::string_view json_text);
// Content-only JSON parser; RenderData owns the typed graph validation and emitter.
[[nodiscard]] bool IsEnvironmentMaterialName(std::string_view name);
[[nodiscard]] bool IsAuthoredMaterialName(std::string_view name);
[[nodiscard]] bool IsHeroMaterialName(std::string_view name);
[[nodiscard]] bool IsDecalMaterialName(std::string_view name);
[[nodiscard]] render_data::MaterialGraph ParseDecalMaterialGraph(
    std::string_view name, std::string_view json_text);
[[nodiscard]] render_data::PreparedMaterial ParsePreparedDecalMaterial(
    std::string_view name, std::string_view json_text);
[[nodiscard]] bool IsMaskedEnvironmentMaterial(std::string_view name);
[[nodiscard]] render_data::MaterialGraph ParseEnvironmentMaterialGraph(
    std::string_view name, std::string_view json_text);
[[nodiscard]] render_data::MaterialGraph ParseRock0MaterialGraph(std::string_view json_text);
[[nodiscard]] render_data::MaterialGraph ParseLeaf0MaterialGraph(std::string_view json_text);
[[nodiscard]] render_data::MaterialGraph ParseTerrainMaterialGraph(std::string_view json_text);
} // namespace hs::material_content
