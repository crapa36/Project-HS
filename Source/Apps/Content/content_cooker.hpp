#pragma once

#include "content_source_inventory.hpp"

#include <hs/core/cooked_format.hpp>
#include <hs/core/cooked_particle_effects.hpp>
#include <hs/game_rules/simulation_rules.hpp>
#include <hs/presentation/presentation_catalog.hpp>


#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hs::content
{

using Json = nlohmann::json;

struct ContentSources
{
    std::unordered_map<std::string, Json> documents;
    ContentSourceInventory inventory;
};

struct BuiltGameData
{
    hs::SimulationRules simulation_rules{};
    hs::PresentationCatalog presentation{};
};

struct ParticleSpriteSource
{
    std::string id;
    std::filesystem::path file;
    std::uint8_t frame_columns{1};
    std::uint8_t frame_rows{1};
};

[[noreturn]] void ThrowValidationError(std::string_view file, std::string_view path,
                                       std::string_view message);

const Json &RequireMember(const Json &object, std::string_view file,
                          std::string_view path, std::string_view key);
const Json &RequireArray(const Json &object, std::string_view file,
                         std::string_view path, std::string_view key);
std::string RequireString(const Json &object, std::string_view file,
                          std::string_view path, std::string_view key);
double RequireNumber(const Json &object, std::string_view file,
                     std::string_view path, std::string_view key);
std::int64_t RequireInteger(const Json &object, std::string_view file,
                            std::string_view path, std::string_view key);
void RequireExactKeys(const Json &object, std::string_view file,
                      std::initializer_list<std::string_view> expected);
void RequireCount(const Json &array, std::string_view file, std::string_view path,
                  std::size_t expected);
bool IsStableId(std::string_view value);
std::unordered_set<std::string> CollectIds(const Json &entries, std::string_view file);
void RequireReference(const std::unordered_set<std::string> &ids,
                      const std::string &reference, std::string_view file,
                      std::string_view path);
const Json &FindParameterValue(const Json &entry, std::string_view file,
                               std::string_view path, std::string_view key);
double ParameterNumber(const Json &entry, std::string_view file, std::string_view path,
                       std::string_view key);

template <typename Number>
void RequirePositive(Number value, std::string_view file, std::string_view path)
{
    if (!(value > Number{}))
    {
        ThrowValidationError(file, path, "must be greater than zero");
    }
}

ContentSources LoadAndValidateGameDataDocuments();
ContentSources LoadScopedGameDataSources(std::span<const std::string_view> names);
BuiltGameData BuildGameData(const ContentSources &sources);
hs::PresentationCatalog BuildPresentationCatalog(const ContentSources &sources);

bool AtomicWrite(const std::filesystem::path &path, std::span<const std::byte> bytes,
                 std::string &error_message);

bool WriteCookedParticleEffects(const std::filesystem::path &path, const Json &document,
                                std::vector<ParticleSpriteSource> &sprite_sources,
                                std::string &error_message);
bool WriteVfxMaskArray(const std::filesystem::path &path,
                       std::span<const ParticleSpriteSource> sprites,
                       std::string &error_message);

bool CookMonsterMaterialTextures(const std::filesystem::path &output,
                                 std::string &error_message);

} // namespace hs::content
