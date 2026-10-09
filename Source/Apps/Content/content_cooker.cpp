#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "content_cooker.hpp"
#include "../CharacterContent/character_sources.hpp"
#include "environment_texture_cooker.hpp"
#include "environment_mesh_cooker.hpp"
#include "vfx_mesh_cooker.hpp"
#include "vfx_runtime_cooker.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace hs::content
{

std::unordered_map<std::string, std::filesystem::path> AudioProducts(
    const std::filesystem::path &build_root)
{
    std::ifstream stream(build_root / "Actions/audio/result.json", std::ios::binary);
    if (!stream)
        throw std::runtime_error("audio ActionResult is missing; build hs_audio_products first");
    const auto result = Json::parse(stream);
    if (result.at("schema_version") != 1 || result.at("action_id") != "audio")
        throw std::runtime_error("audio ActionResult schema/action mismatch");
    const auto product_set_id = result.at("product_set_id").get<std::string>();
    if (product_set_id.size() != 64 ||
        !std::ranges::all_of(product_set_id, [](const unsigned char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        }))
        throw std::runtime_error("audio ActionResult has an invalid ProductSetId");

    std::unordered_map<std::string, std::filesystem::path> products;
    for (const auto &product : result.at("products"))
    {
        const auto logical_id = product.at("logical_id").get<std::string>();
        if (!logical_id.starts_with("audio/"))
            throw std::runtime_error("audio ActionResult contains a foreign product");
        const auto name = logical_id.substr(6);
        if (name.empty() || name.find_first_of("/\\:") != std::string::npos ||
            name == "." || name == "..")
            throw std::runtime_error("audio ActionResult contains an unsafe product name");
        const auto expected_locator = (std::filesystem::path("Products") / "audio" /
                                       product_set_id / name).generic_string();
        if (product.at("locator") != expected_locator)
            throw std::runtime_error("audio ActionResult product locator mismatch");
        const auto path = build_root / expected_locator;
        if (!std::filesystem::is_regular_file(path) ||
            std::filesystem::is_symlink(std::filesystem::symlink_status(path)) ||
            std::filesystem::file_size(path) != product.at("size").get<std::uint64_t>() ||
            !products.emplace(name, path).second)
            throw std::runtime_error("audio ActionResult product missing, corrupt, or duplicated: " + name);
    }
    if (products.size() != 380 || !products.contains("audio_cues.json"))
        throw std::runtime_error("audio ActionResult product set is incomplete");
    return products;
}

std::filesystem::path NormalizeForComparison(const std::filesystem::path &path)
{
    std::error_code filesystem_error;
    const auto normalized = std::filesystem::weakly_canonical(path, filesystem_error);
    if (!filesystem_error)
        return normalized;
    filesystem_error.clear();
    const auto absolute = std::filesystem::absolute(path, filesystem_error);
    if (filesystem_error)
        throw std::runtime_error(path.string() + ": cannot normalize output path");
    return absolute.lexically_normal();
}

std::wstring FoldPath(const std::filesystem::path &path)
{
    auto text = path.lexically_normal().generic_wstring();
    std::ranges::transform(text, text.begin(),
                           [](const wchar_t character) {
                               return std::towlower(character);
                           });
    return text;
}

bool IsPathWithin(const std::filesystem::path &path, const std::filesystem::path &root)
{
    const auto path_text = FoldPath(path);
    const auto root_text = FoldPath(root);
    return path_text == root_text ||
           (path_text.size() > root_text.size() && path_text.starts_with(root_text) &&
            path_text[root_text.size()] == L'/');
}

void ValidateLegacyOracleOutput(const std::filesystem::path &output)
{
    if (output.empty() || !output.is_absolute())
    {
        throw std::runtime_error("legacy oracle output root must be an absolute path");
    }

    const auto normalized = NormalizeForComparison(output);
    const auto source_root = NormalizeForComparison(paths::SourceRoot());
    const auto build_root = NormalizeForComparison(paths::BuildRoot());
    const std::array<std::filesystem::path, 3> source_roots{
        source_root / "ContentSource", source_root / "Schemas", source_root / "Content/Shaders"};
    for (const auto &root : source_roots)
    {
        if (IsPathWithin(normalized, root))
            throw std::runtime_error("legacy oracle output root is under an authoritative source root: " +
                                     output.string());
    }

    const std::array<std::filesystem::path, 5> reserved_build_roots{
        build_root / "Actions", build_root / "Products", build_root / "Generations",
        build_root / "GenerationState", build_root / "Cooked"};
    for (const auto &root : reserved_build_roots)
    {
        if (IsPathWithin(normalized, root))
            throw std::runtime_error("legacy oracle output root is under a reserved build root: " +
                                     output.string());
    }
}

bool AtomicWrite(const std::filesystem::path &path, std::span<const std::byte> bytes,
                 std::string &error_message)
{
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream.write(reinterpret_cast<const char *>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size())))
        {
            error_message = "cannot write temporary file";
            return false;
        }
    }
    const auto handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        error_message = "cannot open temporary file for flush";
        DeleteFileW(temporary.c_str());
        return false;
    }
    const auto flushed = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!flushed || !MoveFileExW(temporary.c_str(), path.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        error_message = "atomic replacement failed with Win32 error " +
                        std::to_string(GetLastError());
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

template <typename T>
bool WriteCookedTable(const std::filesystem::path &path, const T &data,
                      std::uint64_t schema_hash, std::uint64_t source_hash,
                      std::string &error_message)
{
    static_assert(std::is_trivially_copyable_v<T>);
    std::vector<std::byte> canonical;
    std::span<const std::byte> payload;
    if constexpr (std::is_same_v<T, hs::SimulationRules>)
    {
        canonical = hs::CanonicalSimulationRulesBytes(data);
        if (canonical.size() != sizeof(data))
        {
            error_message = "canonical simulation payload size differs from cooked ABI";
            return false;
        }
        payload = std::span<const std::byte>(canonical.data(), canonical.size());
    }
    else
        payload = std::as_bytes(std::span(&data, 1));
    hs::CookedHeader header;
    header.schema_hash = schema_hash;
    header.source_hash = source_hash;
    header.table_count = 1;
    header.payload_size = static_cast<std::uint32_t>(payload.size());
    header.payload_crc32 = hs::Crc32(payload);

    std::vector<std::byte> file(sizeof(header) + payload.size());
    std::memcpy(file.data(), &header, sizeof(header));
    std::memcpy(file.data() + sizeof(header), payload.data(), payload.size());
    return AtomicWrite(path, file, error_message);
}

int RunContent(std::string_view mode, const std::filesystem::path &output_root)
{
        if (mode != "--validate-only" && mode != "--legacy-oracle")
            throw std::runtime_error("unsupported content mode");
        if (mode == "--legacy-oracle")
            ValidateLegacyOracleOutput(output_root);
        const auto environment_source = std::filesystem::path(paths::GameData()).parent_path() / "Textures/Environment";
        const auto environment_mesh_source = environment_source.parent_path().parent_path() / "Models/Environment/environment_meshes.json";
        const auto vfx_mesh_source=std::filesystem::path(paths::GameData()).parent_path()/"VFX/Meshes/vfx_mesh_atlas.json";
        ValidateVfxMeshAtlas(vfx_mesh_source);
        ValidateEnvironmentTextures(environment_source);
        ValidateEnvironmentMeshes(environment_mesh_source);
        const auto sources = LoadAndValidateSources();
        const auto data = BuildGameData(sources);
        const auto gameplay_hash = ComputeGameplaySourceHash(sources.inventory);
        const auto source_hash = ComputeContentSourceHash(sources.inventory);
        if (mode == "--validate-only")
        {
            std::cout << "content.validated documents=" << kDocumentNames.size()
                      << " source_hash=" << source_hash
                      << " gameplay_source_hash=" << gameplay_hash << '\n';
            return 0;
        }

        const std::filesystem::path output = output_root;
        std::error_code filesystem_error;
        std::filesystem::create_directories(output, filesystem_error);
        if (filesystem_error)
        {
            throw std::runtime_error(output.string() + ": " + filesystem_error.message());
        }

        CookVfxMeshAtlas(vfx_mesh_source, output/"vfx_mesh_atlas.hsbin");
        CookVfxRuntimeContent(std::filesystem::path(paths::GameData()).parent_path().parent_path(),output);
        CookEnvironmentTextures(environment_source, output);
        CookEnvironmentMeshes(environment_mesh_source, output);
        const auto audio_products = AudioProducts(paths::BuildRoot());
        const auto audio_output = output / "Audio";
        std::filesystem::create_directories(audio_output, filesystem_error);
        if (filesystem_error)
            throw std::runtime_error(audio_output.string() + ": " + filesystem_error.message());
        const auto &audio_document = sources.documents.at("audio_cues");
        std::filesystem::copy_file(audio_products.at("audio_cues.json"),
                                   audio_output / "audio_cues.json",
                                   std::filesystem::copy_options::overwrite_existing,
                                   filesystem_error);
        if (filesystem_error)
            throw std::runtime_error((audio_output / "audio_cues.json").string() + ": " +
                                     filesystem_error.message());
        for (const auto &entry : audio_document.at("entries"))
            for (const auto &file : entry.at("files"))
            {
                const auto name = file.get<std::string>();
                std::filesystem::copy_file(audio_products.at(name), audio_output / name,
                                           std::filesystem::copy_options::overwrite_existing,
                                           filesystem_error);
                if (filesystem_error)
                    throw std::runtime_error((audio_output / name).string() + ": " +
                                             filesystem_error.message());
            }

        std::string error_message;
        if (!WriteCookedTable(output / "simulation_rules.hsbin", data.simulation_rules,
                              hs::SimulationRulesSchemaHash(), gameplay_hash,
                              error_message))
        {
            throw std::runtime_error("simulation_rules.hsbin: " + error_message);
        }
        if (!WriteCookedTable(output / "presentation_catalog.hsbin",
                              data.presentation, hs::PresentationCatalogSchemaHash(),
                              source_hash, error_message))
        {
            throw std::runtime_error("presentation_catalog.hsbin: " + error_message);
        }
        std::vector<ParticleSpriteSource> particle_sprites;
        if (!WriteCookedParticleEffects(output / "particle_effects.hsbin",
                                        sources.documents.at("particles"),
                                        particle_sprites, error_message))
        {
            throw std::runtime_error("particle_effects.hsbin: " + error_message);
        }
        if (!WriteVfxMaskArray(output / "vfx_masks.dds", particle_sprites,
                               error_message))
            throw std::runtime_error("vfx_masks.dds: " + error_message);
        hs::SimulationRules loaded_rules{};
        hs::PresentationCatalog loaded_presentation{};
        std::uint64_t loaded_hash{};
        if (const auto result = hs::LoadSimulationRules(
                output / "simulation_rules.hsbin", loaded_rules, &loaded_hash);
            !result)
        {
            throw std::runtime_error("simulation_rules.hsbin self-check failed: " +
                                     std::string(result.Message()));
        }
        const auto &rules = data.simulation_rules;
        if (loaded_hash != gameplay_hash ||
            loaded_rules.player_health != rules.player_health ||
            loaded_rules.skills[1].damage_coefficient !=
                rules.skills[1].damage_coefficient ||
            loaded_rules.relics.bleed_burn_explosion.radius !=
                rules.relics.bleed_burn_explosion.radius ||
            loaded_rules.relics.damage_knockback.cooldown_ticks !=
                rules.relics.damage_knockback.cooldown_ticks)
        {
            throw std::runtime_error("simulation_rules.hsbin self-check mismatch");
        }
        if (const auto result = hs::LoadPresentationCatalog(
                output / "presentation_catalog.hsbin", loaded_presentation);
            !result || loaded_presentation.relic_rules[3] !=
                           data.presentation.relic_rules[3])
        {
            throw std::runtime_error("presentation_catalog.hsbin self-check mismatch");
        }

        const auto character_sources = CharacterAssetSources(paths::SourceRoot());
        std::size_t archer_material_count{};
        for (const auto &source : character_sources)
        {
            CharacterCookResult character;
            if (!CookCharacterAsset(output, character, error_message, source))
                throw std::runtime_error(error_message);
            if (source.output_name == "archer")
                archer_material_count = character.materials.size();
        }
        if (!CookMonsterMaterialTextures(output, error_message))
            throw std::runtime_error(error_message);

        constexpr std::string_view shaders[] = {
            "scene_vs.dxil",      "scene_ps.dxil", "slime_ps.dxil",    "shadow_vs.dxil", "shadow_ps.dxil",
            "particle_vs.dxil",   "ground_ring_vs.dxil", "vfx_flash_vs.dxil", "vfx_flash_ps.dxil",
            "particle_ps.dxil", "particle_cs.dxil",
            "fullscreen_vs.dxil", "deferred_ps.dxil", "composite_ps.dxil",
            "bloom_ps.dxil",      "tonemap_ps.dxil",  "outline_ps.dxil",
            "fxaa_ps.dxil",       "ui_ps.dxil",
        };
        for (const auto shader : shaders)
        {
            const auto source = std::filesystem::path(paths::Shaders()) / shader;
            const auto destination = output / shader;
            std::filesystem::copy_file(source, destination,
                                       std::filesystem::copy_options::overwrite_existing,
                                       filesystem_error);
            if (filesystem_error)
            {
                throw std::runtime_error("Shader Cook failed: " + std::string(shader) +
                                         ": " + filesystem_error.message());
            }
        }

        std::ofstream manifest(output / "manifest.txt", std::ios::trunc);
        manifest << "fbx_sdk=2020.3.7-vs2022\n"
                 << "audio_catalog=Audio/audio_cues.json\n"
                 << "simulation_rules=simulation_rules.hsbin\n"
                 << "presentation_catalog=presentation_catalog.hsbin\n"
                 << "particle_effects=particle_effects.hsbin\n"
                 << "vfx_masks=vfx_masks.dds\n"
                 << "source_hash=" << source_hash << '\n'
                 << "mesh=archer.meshbin\n"
                 << "mesh=enemy_melee.meshbin\n"
                 << "mesh=enemy_ranged.meshbin\n"
                 << "mesh=enemy_suicide.meshbin\n"
                 << "mesh=enemy_gel_projectile.meshbin\n"
                 << "mesh=boss_5m.meshbin\n"
                 << "mesh=boss_10m.meshbin\n"
                 << "mesh=boss_final.meshbin\n";
        for (std::size_t index = 0; index < archer_material_count; ++index)
             manifest << "texture=archer_diffuse_" << index << ".dds\n"
                      << "texture=archer_normal_" << index << ".dds\n";
        for (const auto family : {"melee", "ranged", "suicide", "gel_projectile"})
            manifest << "texture=enemy_" << family << "_diffuse_0.dds\n"
                     << "texture=enemy_" << family << "_normal_0.dds\n";
        manifest << "texture=monster_basecolor.dds\n"
                 << "texture=monster_emissive.dds\n"
                 << "texture=monster_ram.dds\n";
        for (const auto &entry : std::filesystem::directory_iterator(output))
            if (entry.path().filename().string().starts_with("environment_") && (entry.path().extension() == ".dds" || entry.path().extension() == ".meshbin"))
                manifest << (entry.path().extension() == ".dds" ? "texture=" : "mesh=") << entry.path().filename().string() << '\n';
        for (const auto shader : shaders)
        {
            manifest << "shader=" << shader << '\n';
        }
        for (const auto &entry : audio_document.at("entries"))
            for (const auto &file : entry.at("files"))
                manifest << "audio=Audio/" << file.get<std::string>() << '\n';
        if (!manifest.good())
        {
            throw std::runtime_error("manifest.txt: write failed");
        }
        std::cout << "content.cooked documents=" << kDocumentNames.size()
                  << " character_assets=" << character_sources.size()
                  << " source_hash=" << source_hash
                  << '\n';
        return 0;
}

} // namespace hs::content
