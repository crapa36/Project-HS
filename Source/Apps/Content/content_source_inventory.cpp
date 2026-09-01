#include "content_source_inventory.hpp"

#include <hs/core/cooked_format.hpp>

#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>
#include <vector>

namespace hs::content
{

bool IsGameDataCategory(const std::string_view value) noexcept
{
    return std::ranges::find(kDocumentNames, value) != kDocumentNames.end();
}

std::filesystem::path GameDataCategoryPath(const std::string_view category)
{
    if (!IsGameDataCategory(category))
    {
        throw std::runtime_error("unknown category '" + std::string(category) + "'");
    }
    return std::filesystem::path(HS_GAME_DATA_DIRECTORY) /
           (std::string(category) + ".json");
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
    const std::filesystem::path root = HS_GAME_DATA_DIRECTORY;
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

ContentSourceInventory LoadContentSourceInventory()
{
    ContentSourceInventory sources;
    for (std::size_t index = 0; index < kDocumentNames.size(); ++index)
    {
        const auto name = kDocumentNames[index];
        const auto text = ReadRequiredText(GameDataCategoryPath(name));
        sources.document_text[index] = text;

        sources.all_source_bytes.append(name);
        sources.all_source_bytes.push_back('\0');
        sources.all_source_bytes.append(text);
        sources.all_source_bytes.push_back('\0');
        if (name != "particles" && name != "audio_cues")
        {
            sources.gameplay_source_bytes.append(name);
            sources.gameplay_source_bytes.push_back('\0');
            sources.gameplay_source_bytes.append(text);
            sources.gameplay_source_bytes.push_back('\0');
        }
    }

    const auto append_asset = [&](const std::string_view name,
                                  const std::filesystem::path &path) {
        const auto text = ReadRequiredText(path);
        sources.gameplay_source_bytes.append(name);
        sources.gameplay_source_bytes.push_back('\0');
        sources.gameplay_source_bytes.append(text);
        sources.gameplay_source_bytes.push_back('\0');
        sources.all_source_bytes.append(name);
        sources.all_source_bytes.push_back('\0');
        sources.all_source_bytes.append(text);
                             sources.all_source_bytes.push_back('\0');
    };
    const auto append_presentation_asset = [&](const std::string_view name,
                                               const std::filesystem::path &path) {
        const auto bytes = ReadRequiredText(path);
        sources.all_source_bytes.append(name);
        sources.all_source_bytes.push_back('\0');
        sources.all_source_bytes.append(bytes);
        sources.all_source_bytes.push_back('\0');
    };
    const auto animation_root =
        std::filesystem::path(HS_CHARACTER_ANIMATION_DIRECTORY);
    append_asset("character/archer/model", HS_CHARACTER_MODEL);
    append_asset("character/archer/idle", animation_root / "Idle.fbx");
    append_asset("character/archer/run", animation_root / "RunForward.fbx");
    append_asset("character/archer/draw", animation_root / "DrawArrow.fbx");
    append_asset("character/archer/recoil", animation_root / "AimRecoil.fbx");
    append_asset("character/archer/death", animation_root / "DeathBackward.fbx");

    const auto monster_models = std::filesystem::path(HS_MONSTER_MODEL_DIRECTORY);
    const auto monster_animations = std::filesystem::path(HS_MONSTER_ANIMATION_DIRECTORY);
    const auto append_monster = [&](std::string_view id, std::string_view model,
                                    std::initializer_list<std::string_view> clips) {
        append_presentation_asset(std::string(id) + "/model", monster_models / model);
        for (const auto clip : clips)
            append_presentation_asset(
                std::string(id) + "/" + std::string(clip),
                monster_animations / std::string(id.substr(id.find('/') + 1)) /
                    std::string(clip));
    };
    append_monster("enemy/melee", "Slime_SK.fbx",
                   {"IdleBattle.fbx", "RunFWD.fbx", "Attack01.fbx", "Attack02.fbx", "Die.fbx"});
    append_monster("enemy/ranged", "Cactus_SK.fbx",
                   {"IdleBattle.fbx", "RunFWD.fbx", "Attack01.fbx", "Attack02.fbx", "Die.fbx"});
    append_monster("enemy/suicide", "Swarm09_SK.fbx",
                   {"Idle.fbx", "MoveFWD.fbx", "Attack.fbx", "Die.fbx"});
    append_monster("boss/5m", "TurtleShell_SK.fbx",
                   {"IdleBattle.fbx", "Run.fbx", "Attack01.fbx", "Attack02.fbx", "Die.fbx"});
    append_monster("boss/10m", "ChestMonster_SK.fbx",
                   {"IdleBattle.fbx", "Run.fbx", "Attack01.fbx", "Attack02.fbx", "Die.fbx"});
    append_monster("boss/final", "Beholder_SK.fbx",
                   {"IdleBattle.fbx", "Run.fbx", "Attack01.fbx", "Attack03.fbx", "Die.fbx"});

    const auto monster_textures = std::filesystem::path(HS_MONSTER_TEXTURE_DIRECTORY);
    append_presentation_asset("texture/monster/basecolor",
                              monster_textures / "BasecolorDefault_TEX.png");
    append_presentation_asset("texture/monster/emissive",
                              monster_textures / "Emissive_TEX.png");
    append_presentation_asset("texture/monster/ram",
                              monster_textures / "RAM_TEX.png");

    std::vector<std::filesystem::path> audio_files;
    for (const auto &entry : std::filesystem::directory_iterator(HS_AUDIO_DIRECTORY))
        if (entry.is_regular_file() && entry.path().extension() == ".wav")
            audio_files.push_back(entry.path());
    std::ranges::sort(audio_files);
    for (const auto &path : audio_files)
    {
        const auto bytes = ReadRequiredText(path);
        sources.all_source_bytes.append("audio/");
        sources.all_source_bytes.append(path.filename().string());
        sources.all_source_bytes.push_back('\0');
        sources.all_source_bytes.append(bytes);
        sources.all_source_bytes.push_back('\0');
    }
    return sources;
}

std::uint64_t ComputeGameplaySourceHash(
    const ContentSourceInventory &sources) noexcept
{
    return hs::Fnv1a64(sources.gameplay_source_bytes);
}

std::uint64_t ComputeContentSourceHash(
    const ContentSourceInventory &sources) noexcept
{
    return hs::Fnv1a64(sources.all_source_bytes);
}

} // namespace hs::content
