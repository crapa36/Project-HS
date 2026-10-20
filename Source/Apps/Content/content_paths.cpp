#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "content_paths.hpp"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <string>

namespace hs::content::paths
{

namespace
{

std::filesystem::path ExecutablePath()
{
    std::wstring buffer(260, L'\0');
    for (;;)
    {
        const auto length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            throw std::runtime_error("cannot locate the content executable");
        if (length < buffer.size())
        {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        if (buffer.size() >= 32768)
            throw std::runtime_error("content executable path is too long");
        buffer.resize(buffer.size() * 2);
    }
}

struct BuildConfig
{
    std::filesystem::path source_root;
    std::filesystem::path build_root;
};

BuildConfig LoadBuildConfig()
{
    const auto build_root = ExecutablePath().parent_path().lexically_normal();
    const auto config_path = build_root / "hs_build_config.json";
    std::ifstream stream(config_path, std::ios::binary);
    if (!stream)
        throw std::runtime_error(config_path.string() + ": cannot open build configuration");

    try
    {
        const auto config = nlohmann::json::parse(stream);
        const auto source_root = std::filesystem::u8path(
            config.at("source_root").get<std::string>()).lexically_normal();
        if (!source_root.is_absolute())
            throw std::runtime_error("source_root must be absolute");
        return {source_root, build_root};
    }
    catch (const nlohmann::json::exception &exception)
    {
        throw std::runtime_error(config_path.string() + ": " + exception.what());
    }
}

const BuildConfig &Config()
{
    static const BuildConfig config = LoadBuildConfig();
    return config;
}

} // namespace

std::filesystem::path SourceRoot() { return Config().source_root; }
std::filesystem::path BuildRoot() { return Config().build_root; }

std::filesystem::path GameData() { return SourceRoot() / "ContentSource/GameData"; }
std::filesystem::path Audio() { return SourceRoot() / "ContentSource/Audio"; }
std::filesystem::path Schema() { return SourceRoot() / "Schemas/game.schema.json"; }
std::filesystem::path VfxTextures() { return SourceRoot() / "ContentSource/Textures/VFX"; }
std::filesystem::path ArcherModel()
{
    return SourceRoot() / "ContentSource/Models/Characters/Archer/ErikaArcher.fbx";
}
std::filesystem::path ArcherAnimations()
{
    return SourceRoot() / "ContentSource/Animations/Characters/Archer";
}
std::filesystem::path MonsterModels()
{
    return SourceRoot() / "ContentSource/Models/Monsters";
}
std::filesystem::path MonsterAnimations()
{
    return SourceRoot() / "ContentSource/Animations/Monsters";
}
std::filesystem::path MonsterTextures()
{
    return SourceRoot() / "ContentSource/Textures/Monsters/PBR";
}

} // namespace hs::content::paths
