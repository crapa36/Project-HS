#pragma once

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

#ifndef HS_GAME_DATA_DIRECTORY
#define HS_GAME_DATA_DIRECTORY "ContentSource/GameData"
#endif

#ifndef HS_SCHEMA_FILE
#define HS_SCHEMA_FILE "Schemas/game.schema.json"
#endif

#ifndef HS_VFX_TEXTURE_DIRECTORY
#define HS_VFX_TEXTURE_DIRECTORY "ContentSource/Textures/VFX"
#endif

#ifndef HS_CHARACTER_MODEL
#define HS_CHARACTER_MODEL "ContentSource/Models/Characters/Archer/ErikaArcher.fbx"
#endif

#ifndef HS_CHARACTER_ANIMATION_DIRECTORY
#define HS_CHARACTER_ANIMATION_DIRECTORY "ContentSource/Animations/Characters/Archer"
#endif

namespace hs::content
{

using Json = nlohmann::json;

inline constexpr std::array<std::string_view, 13> kDocumentNames = {
    "audio_cues", "bosses",   "characters", "enemies", "level",
    "materials",  "particles", "relics",    "skills",  "spawn_schedule",
    "stats",      "ui_strings", "upgrades",
};

struct ContentSources
{
    std::unordered_map<std::string, Json> documents;
    std::string source_bytes;
    std::string all_source_bytes;
};

struct BuiltGameData
{
    hs::SimulationRules simulation_rules{};
    hs::PresentationCatalog presentation{};
};

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
    std::array<float, 3> bounds_min{
        std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    std::array<float, 3> bounds_max{
        std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()};
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

ContentSources LoadAndValidateSources();
BuiltGameData BuildGameData(const ContentSources &sources);

bool AtomicWrite(const std::filesystem::path &path, std::span<const std::byte> bytes,
                 std::string &error_message);

bool WriteCookedParticleEffects(const std::filesystem::path &path, const Json &document,
                                std::vector<ParticleSpriteSource> &sprite_sources,
                                std::string &error_message);
bool WriteVfxMaskArray(const std::filesystem::path &path,
                       std::span<const ParticleSpriteSource> sprites,
                       std::string &error_message);

bool CookCharacterAsset(const std::filesystem::path &output,
                        CharacterCookResult &character,
                        std::string &error_message);

int RunContent(std::string_view mode);

} // namespace hs::content
