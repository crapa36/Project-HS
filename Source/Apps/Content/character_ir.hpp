#pragma once

#include <hs/render_data/character_animation_extension.hpp>

#include <filesystem>
#include <limits>

namespace hs::content
{
// Owned FBX import supplying the geometry, skeleton, animation and bindings.
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
    std::uint32_t mesh_count{};
    std::array<float, 3> bounds_min{
        std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    std::array<float, 3> bounds_max{
        std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()};
};
} // namespace hs::content
