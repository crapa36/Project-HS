#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "content_cooker.hpp"

#include <Windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace hs::content
{

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
    const auto payload = std::as_bytes(std::span(&data, 1));
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

int RunContent(std::string_view mode)
{
        const auto sources = LoadAndValidateSources();
        const auto data = BuildGameData(sources);
        const auto gameplay_hash = ComputeGameplaySourceHash(sources.inventory);
        const auto source_hash = ComputeContentSourceHash(sources.inventory);
        if (mode == "--validate-only")
        {
            std::cout << "content.validated documents=" << kDocumentNames.size()
                      << " source_hash=" << source_hash << '\n';
            return 0;
        }

        const std::filesystem::path output = HS_COOKED_DIRECTORY;
        std::error_code filesystem_error;
        std::filesystem::create_directories(output, filesystem_error);
        if (filesystem_error)
        {
            throw std::runtime_error(output.string() + ": " + filesystem_error.message());
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

        CharacterCookResult character;
        if (!CookCharacterAsset(output, character, error_message))
        {
            throw std::runtime_error(error_message);
        }

        constexpr std::string_view shaders[] = {
            "scene_vs.dxil",      "scene_ps.dxil",    "shadow_vs.dxil",
            "particle_vs.dxil",   "particle_ps.dxil", "particle_cs.dxil",
            "fullscreen_vs.dxil", "deferred_ps.dxil", "composite_ps.dxil",
            "bloom_ps.dxil",      "tonemap_ps.dxil",  "outline_ps.dxil",
            "fxaa_ps.dxil",       "ui_ps.dxil",
        };
        for (const auto shader : shaders)
        {
            const auto source = std::filesystem::path(HS_SHADER_DIRECTORY) / shader;
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
                 << "simulation_rules=simulation_rules.hsbin\n"
                 << "presentation_catalog=presentation_catalog.hsbin\n"
                 << "particle_effects=particle_effects.hsbin\n"
                 << "vfx_masks=vfx_masks.dds\n"
                 << "source_hash=" << source_hash << '\n'
                 << "mesh=archer.meshbin\n";
        for (std::size_t index = 0; index < character.materials.size(); ++index)
        {
            manifest << "texture=archer_diffuse_" << index << ".dds\n"
                     << "texture=archer_normal_" << index << ".dds\n";
        }
        for (const auto shader : shaders)
        {
            manifest << "shader=" << shader << '\n';
        }
        if (!manifest.good())
        {
            throw std::runtime_error("manifest.txt: write failed");
        }
        std::cout << "content.cooked documents=" << kDocumentNames.size()
                  << " meshes=" << character.mesh_count
                  << " vertices=" << character.vertices.size()
                  << " bones=" << character.bone_count
                  << " clips=" << character.clips.size()
                  << " source_hash=" << source_hash
                  << '\n';
        return 0;
}

} // namespace hs::content
