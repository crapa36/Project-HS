#pragma once
#include "../Content/character_ir.hpp"
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
    content::CharacterIR runtime;
    std::vector<std::string> limitations;
};
[[nodiscard]] ImportedCharacter Load(const std::filesystem::path &manifest);
}
