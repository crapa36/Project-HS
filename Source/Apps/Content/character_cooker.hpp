#pragma once

#include "character_ir.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace hs::content
{
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
    std::vector<NamedCharacterAnimationSource> named_animations;
};

bool ImportCharacterAsset(CharacterIR &character, std::string &error_message,
                          const CharacterAssetSource &source);
bool ImportCharacterMaterials(CharacterIR &character, std::string &error_message,
                              const CharacterAssetSource &source);
bool WriteCharacterTextures(const std::filesystem::path &output,
    const CharacterIR &character, const CharacterAssetSource &source,
    std::string &error_message);
bool CookMonsterMaterialTextures(const std::filesystem::path &output,
    std::string &error_message, const std::filesystem::path &texture_source);
// Includes the actual BC7 encoder/backend identity and fixed semantic policy;
// callers include it in the Action key before checking for current products.
[[nodiscard]] const std::string &CharacterTextureCookRecipeIdentity();
} // namespace hs::content
