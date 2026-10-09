#pragma once
#include <hs/render_data/character_package.hpp>
#include <hs/render_data/character_animation_extension.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace hs::character_interchange
{
struct ImportedCharacter
{
    // Complete validated clip and socket data is retained separately from the
    // existing five-slot compatibility projection. This describes importer
    // capability and does not imply that an authored Elin handoff is present.
    nlohmann::json source;
    render_data::CharacterPackageData runtime;
    render_data::CharacterAnimationExtensionData animation_extension;
    std::vector<std::string> limitations;
};
[[nodiscard]] ImportedCharacter Load(const std::filesystem::path &manifest);
}
