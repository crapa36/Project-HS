#include "content_source_inventory.hpp"

#include <hs/core/cooked_format.hpp>

#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>

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
        if (name != "particles")
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
    const auto animation_root =
        std::filesystem::path(HS_CHARACTER_ANIMATION_DIRECTORY);
    append_asset("character/archer/model", HS_CHARACTER_MODEL);
    append_asset("character/archer/idle", animation_root / "Idle.fbx");
    append_asset("character/archer/run", animation_root / "RunForward.fbx");
    append_asset("character/archer/draw", animation_root / "DrawArrow.fbx");
    append_asset("character/archer/recoil", animation_root / "AimRecoil.fbx");
    append_asset("character/archer/death", animation_root / "DeathBackward.fbx");
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
