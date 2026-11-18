#include "content_source_inventory.hpp"
#include "../AssetPipeline/action.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <stdexcept>
#include <vector>

namespace hs::content
{

namespace
{

void AppendHash(std::uint64_t &hash, const std::string_view bytes) noexcept
{
    const auto *end = bytes.data() + bytes.size();
    for (auto cursor = bytes.data(); cursor != end; ++cursor)
    {
        hash ^= static_cast<unsigned char>(*cursor);
        hash *= 1099511628211ull;
    }
}

void AppendNamedFile(std::uint64_t &hash, std::vector<asset_pipeline::Input> &inputs,
                     const std::string_view name, const std::filesystem::path &path)
{
    asset_pipeline::RequireRegularFile(path);
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        throw std::runtime_error(path.string() + ": cannot open file");
    const auto size = stream.tellg();
    if (size <= 0)
        throw std::runtime_error(path.string() + ": file is empty");

    AppendHash(hash, name);
    AppendHash(hash, std::string_view("\0", 1));
    asset_pipeline::Sha256Stream digest;
    stream.seekg(0);
    std::array<char, 64 * 1024> buffer;
    auto remaining = static_cast<std::uint64_t>(size);
    while (remaining > 0)
    {
        const auto count = static_cast<std::streamsize>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        if (!stream.read(buffer.data(), count))
            throw std::runtime_error(path.string() + ": cannot read complete file");
        const std::string_view chunk(buffer.data(), static_cast<std::size_t>(count));
        AppendHash(hash, chunk);
        digest.Add(std::as_bytes(std::span(chunk.data(), chunk.size())));
        remaining -= static_cast<std::uint64_t>(count);
    }
    AppendHash(hash, std::string_view("\0", 1));
    inputs.push_back({std::string(name), path, digest.Finish()});
}

} // namespace

std::filesystem::path GameDataCategoryPath(const std::string_view category)
{
    return paths::GameData() / (std::string(category) + ".json");
}

std::string ReadRequiredText(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        throw std::runtime_error(path.string() + ": cannot open file");
    }
    const auto size = stream.tellg();
    if (size <= 0)
    {
        throw std::runtime_error(path.string() + ": file is empty");
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    stream.seekg(0);
    if (!stream.read(text.data(), static_cast<std::streamsize>(size)))
    {
        throw std::runtime_error(path.string() + ": cannot read complete file");
    }
    return text;
}

void ValidateGameDataDirectory()
{
    const std::filesystem::path root = paths::GameData();
    std::set<std::string, std::less<>> found_files;
    for (const auto &entry : std::filesystem::directory_iterator(root))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".json")
        {
            found_files.emplace(entry.path().stem().string());
        }
    }
    std::set<std::string, std::less<>> expected_files;
    for (const auto name : kDocumentNames)
    {
        expected_files.emplace(name);
    }
    if (found_files != expected_files)
    {
        throw std::runtime_error(root.string() +
                                 ": expected exactly the 13 declared category JSON files");
    }
}

std::uint64_t ScanSourceIdentity(bool include_presentation,
                                 std::vector<asset_pipeline::Input> &inputs)
{
    std::uint64_t hash = 14695981039346656037ull;
    const auto append_asset = [&](std::string_view name, const std::filesystem::path &path) {
        AppendNamedFile(hash, inputs, name, path);
    };
    for (const auto name : kDocumentNames)
    {
        if (include_presentation || (name != "particles" && name != "audio_cues"))
            append_asset(name, GameDataCategoryPath(name));
    }

    const auto animation_root =
        std::filesystem::path(paths::ArcherAnimations());
    append_asset("character/archer/model", paths::ArcherModel());
    append_asset("character/archer/idle", animation_root / "Idle.fbx");
    append_asset("character/archer/run", animation_root / "RunForward.fbx");
    append_asset("character/archer/draw", animation_root / "DrawArrow.fbx");
    append_asset("character/archer/recoil", animation_root / "AimRecoil.fbx");
    append_asset("character/archer/death", animation_root / "DeathBackward.fbx");

    if (!include_presentation)
        return hash;

    const auto monster_models = std::filesystem::path(paths::MonsterModels());
    const auto monster_animations = std::filesystem::path(paths::MonsterAnimations());
    const auto append_monster = [&](std::string_view id, std::string_view model,
                                    std::initializer_list<std::string_view> clips) {
        append_asset(std::string(id) + "/model", monster_models / model);
        for (const auto clip : clips)
            append_asset(
                std::string(id) + "/" + std::string(clip),
                monster_animations / std::string(id.substr(id.find('/') + 1)) /
                    std::string(clip));
    };
    for (const auto variant : {"Melee", "Ranged", "Suicide", "Projectile"})
    {
        const auto stem = std::string(variant) == "Projectile"
                              ? std::string("SlimeGelProjectile")
                              : std::string("Slime") + variant;
        const auto id = std::string("slime_family/") + variant;
        const auto directory = monster_models / "SlimeFamily" / variant;
        append_asset(id + "/model", directory / (stem + ".fbx"));
        for (const auto suffix : {"_BaseColor.png", "_Normal.png"})
            append_asset(id + suffix, directory / (stem + ".fbm") / (stem + suffix));
        if (std::string_view(variant) != "Projectile")
            for (const auto clip : {"Idle.fbx", "Run.fbx", "Draw.fbx", "Recoil.fbx", "Death.fbx"})
                append_asset(id + "/" + clip,
                    monster_animations / "SlimeFamily" / variant / clip);
    }
    append_monster("boss/5m", "TurtleShell_SK.fbx",
                   {"IdleBattle.fbx", "Run.fbx", "Attack01.fbx", "Attack02.fbx", "Die.fbx"});
    append_monster("boss/10m", "ChestMonster_SK.fbx",
                   {"IdleBattle.fbx", "Run.fbx", "Attack01.fbx", "Attack02.fbx", "Die.fbx"});
    append_monster("boss/final", "Beholder_SK.fbx",
                   {"IdleBattle.fbx", "Run.fbx", "Attack01.fbx", "Attack03.fbx", "Die.fbx"});

    const auto monster_textures = std::filesystem::path(paths::MonsterTextures());
    append_asset("texture/monster/basecolor",
                              monster_textures / "BasecolorDefault_TEX.png");
    append_asset("texture/monster/emissive",
                              monster_textures / "Emissive_TEX.png");
    append_asset("texture/monster/ram",
                              monster_textures / "RAM_TEX.png");

    const auto environment_root = std::filesystem::path(paths::GameData()).parent_path() / "Textures/Environment";
    append_asset("environment/meshes", environment_root.parent_path().parent_path() / "Models/Environment/environment_meshes.json");
    append_asset("environment/meadow-surface", environment_root.parent_path().parent_path() / "Models/Environment/meadow_surface.json");
    append_asset("environment/meadow-meshes", environment_root.parent_path().parent_path() / "Models/Environment/meadow_surface_meshes.json");
    std::vector<std::filesystem::path> environment_files;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(environment_root))
        if (entry.is_regular_file() && (entry.path().extension() == ".png" || entry.path().extension() == ".json"))
            environment_files.push_back(entry.path());
    std::ranges::sort(environment_files);
    for (const auto &path : environment_files)
        append_asset("environment/" + path.lexically_relative(environment_root).generic_string(), path);

    std::vector<std::filesystem::path> audio_files;
    for (const auto &entry : std::filesystem::directory_iterator(paths::Audio()))
        if (entry.is_regular_file() && entry.path().extension() == ".wav")
            audio_files.push_back(entry.path());
    std::ranges::sort(audio_files);
    for (const auto &path : audio_files)
        append_asset("audio/" + path.filename().string(), path);
    return hash;
}

} // namespace hs::content
