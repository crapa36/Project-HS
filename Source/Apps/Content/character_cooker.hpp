#pragma once

#include "content_cooker.hpp"
#include <hs/render_data/character_animation_extension.hpp>

namespace hs::content
{
struct CharacterCookResult
{
    struct Material
    {
        std::filesystem::path diffuse;
        std::filesystem::path normal;
        std::string name;
    };

    std::vector<hs::SkinnedVertex> vertices;
    std::vector<hs::CharacterClipHeader> clips;
    std::vector<std::uint16_t> parents;
    std::vector<std::array<float, 16>> inverse_bind_matrices;
    std::vector<hs::CharacterLocalTransform> transforms;
    std::vector<float> upper_body_weights;
    std::uint32_t mesh_count{};
    std::uint32_t bone_count{};
    std::vector<Material> materials;
    hs::render_data::CharacterAnimationExtensionData animation_extension;
    std::array<float, 3> bounds_min{
        std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    std::array<float, 3> bounds_max{
        std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()};
};

struct NamedCharacterAnimationSource
{
    std::filesystem::path file;
    std::string authored_name;
    bool looping{};
};

struct CharacterAssetSource
{
    std::filesystem::path model;
    std::array<std::filesystem::path, 5> animations;
    std::string output_name;
    std::string diagnostic_name;
    bool allow_missing_material_textures{};
    bool seal_eye_socket{};
    bool generate_material_mips{};
    std::vector<NamedCharacterAnimationSource> named_animations;
};

bool CookCharacterAsset(const std::filesystem::path &output,
                        CharacterCookResult &character,
                        std::string &error_message);
bool CookCharacterAsset(const std::filesystem::path &output,
                        CharacterCookResult &character,
                        std::string &error_message,
                        const CharacterAssetSource &source);
bool WriteCharacterMaterialBindings(const std::filesystem::path &output,
    const CharacterCookResult &character, const CharacterAssetSource &source,
    const std::filesystem::path &authoring_path, std::string &error_message);
} // namespace hs::content
