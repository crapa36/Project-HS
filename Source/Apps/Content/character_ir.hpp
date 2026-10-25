#pragma once

#include <hs/render_data/character_animation_extension.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <limits>

namespace hs::content
{
// Owned importer result. FBX scenes and interchange bytes do not escape the
// adapter; final geometry, skeleton, animation and bindings borrow these arrays.
// The five-slot projection remains available to the explicit export leaf.
struct CharacterIR : render_data::CharacterPackageData
{
    struct Material
    {
        std::filesystem::path diffuse;
        std::filesystem::path normal;
        std::string name;
    };

    std::vector<Material> materials;
    std::vector<std::string> bone_names;
    render_data::CharacterAnimationExtensionData animation_extension;
    // Interchange-only source meaning, including markers, curves and unsupported
    // authored features. It is retained without claiming runtime support.
    nlohmann::json source_metadata;
    std::uint32_t mesh_count{};
    std::array<float, 3> bounds_min{
        std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    std::array<float, 3> bounds_max{
        std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()};
};
} // namespace hs::content
