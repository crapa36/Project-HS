#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../AssetPipeline/action.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::string_view kActionId = "audio";
constexpr std::string_view kRecipeId = "project-hs.audio-copy";
constexpr std::uint32_t kRecipeVersion = 1;
constexpr std::uint32_t kActionKeySchemaVersion = 1;

[[noreturn]] void Fail(const std::string &message)
{
    throw std::runtime_error("hs_audio_content: " + message);
}

void Require(bool condition, const std::string &message)
{
    if (!condition)
        Fail(message);
}

namespace ap = hs::asset_pipeline;
using ap::RequireDirectory;
using ap::RequireRegularFile;
using ap::MakeDirectory;

const Json &Member(const Json &object, std::string_view key)
{
    Require(object.is_object() && object.contains(std::string(key)),
            "audio catalog missing field: " + std::string(key));
    return object.at(std::string(key));
}

std::string StringMember(const Json &object, std::string_view key)
{
    const auto &value = Member(object, key);
    Require(value.is_string(), "audio catalog field must be a string: " + std::string(key));
    return value.get<std::string>();
}

bool SafeWaveName(std::string_view name)
{
    if (name.size() <= 4 || !name.ends_with(".wav") || name.front() == '.')
        return false;
    for (const unsigned char ch : name)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.'))
            return false;
    if (name.find("..") != std::string_view::npos)
        return false;
    std::string stem(name.substr(0, name.find('.')));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char ch) {
        return static_cast<char>((ch >= 'a' && ch <= 'z') ? ch - 'a' + 'A' : ch);
    });
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL")
        return false;
    if (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
        stem[3] >= '1' && stem[3] <= '9')
        return false;
    return true;
}

void ValidateWave(const fs::path &path, bool stereo)
{
    RequireRegularFile(path);
    std::ifstream input(path, std::ios::binary);
    std::array<std::uint8_t, 44> header{};
    Require(static_cast<bool>(input.read(reinterpret_cast<char *>(header.data()), header.size())),
            "WAV header is truncated: " + path.string());
    const auto u16 = [&](std::size_t offset) {
        return static_cast<std::uint16_t>(header[offset] | (header[offset + 1] << 8));
    };
    const auto u32 = [&](std::size_t offset) {
        return static_cast<std::uint32_t>(header[offset] | (header[offset + 1] << 8) |
                                          (header[offset + 2] << 16) | (header[offset + 3] << 24));
    };
    const auto channels = static_cast<std::uint16_t>(stereo ? 2 : 1);
    const auto file_size = fs::file_size(path);
    Require(file_size >= header.size() &&
                std::memcmp(header.data(), "RIFF", 4) == 0 &&
                std::memcmp(header.data() + 8, "WAVEfmt ", 8) == 0 &&
                std::memcmp(header.data() + 36, "data", 4) == 0 &&
                u32(4) == file_size - 8 && u32(16) == 16 && u16(20) == 1 &&
                u16(22) == channels && u32(24) == 48000 &&
                u32(28) == 48000u * channels * 3 && u16(32) == channels * 3 &&
                u16(34) == 24 &&
                static_cast<std::uint64_t>(44) + u32(40) + (u32(40) & 1u) == file_size,
            "WAV must be canonical 48kHz 24-bit PCM with required channel count: " + path.string());
}

struct WaveReference
{
    std::string name;
    bool stereo = false;
};

std::vector<WaveReference> DiscoverWaves(const fs::path &catalog)
{
    RequireRegularFile(catalog);
    std::ifstream input(catalog, std::ios::binary);
    Require(input.is_open(), "cannot read audio catalog");
    const Json audio = Json::parse(input);
    const auto &schema_version = Member(audio, "schema_version");
    const auto &maximum_voices = Member(audio, "maximum_source_voices");
    Require(schema_version.is_number_integer() && schema_version == 1 &&
                StringMember(audio, "category") == "audio_cues" &&
                maximum_voices.is_number_integer() && maximum_voices == 64,
            "audio catalog schema or maximum voices changed");
    const auto &entries = Member(audio, "entries");
    Require(entries.is_array() && entries.size() == 107,
            "audio catalog must contain exactly 107 cues");
    std::set<std::string, std::less<>> ids;
    std::set<std::string, std::less<>> names;
    std::vector<WaveReference> waves;
    for (const auto &entry : entries)
    {
        const auto id = StringMember(entry, "id");
        Require(!id.empty() && ids.emplace(id).second, "duplicate or empty audio cue ID: " + id);
        const auto &streaming = Member(entry, "streaming");
        Require(StringMember(entry, "encoding") == "PCM" &&
                    streaming.is_boolean() && !streaming.get<bool>(),
                "audio cue must use non-streaming PCM: " + id);
        const auto &files = Member(entry, "files");
        Require(files.is_array() && !files.empty(), "audio cue has no files: " + id);
        const bool stereo = id.starts_with("audio.bgm.") ||
                            id.starts_with("audio.ambience.");
        for (const auto &file : files)
        {
            Require(file.is_string(), "audio WAV name must be a string: " + id);
            const auto name = file.get<std::string>();
            Require(SafeWaveName(name) && names.emplace(name).second,
                    "duplicate or unsafe WAV name: " + name);
            waves.push_back({name, stereo});
        }
    }
    Require(names.size() == 379, "audio catalog must reference exactly 379 unique WAV files");
    std::sort(waves.begin(), waves.end(), [](const auto &a, const auto &b) {
        return a.name < b.name;
    });
    return waves;
}

void Produce(const fs::path &source_root, const fs::path &build_root)
{
    Require(source_root.is_absolute() && build_root.is_absolute(),
            "source and build roots must be absolute");
    RequireDirectory(source_root);
    MakeDirectory(build_root);
    const auto actions = build_root / "Actions" / "audio";
    MakeDirectory(actions);
    const auto source_catalog = source_root / "ContentSource" / "GameData" /
                                "audio_cues.json";
    const auto source_audio = source_root / "ContentSource" / "Audio";
    RequireDirectory(source_audio);
    const auto stage = ap::CreateStage(actions);
    try
    {
        ap::ActionSpec spec;
        spec.action_id = std::string(kActionId);
        spec.recipe_id = std::string(kRecipeId);
        spec.recipe_version = kRecipeVersion;
        spec.namespace_path = "audio";
        spec.include_namespace_field = false; // Preserve the original Audio ActionResult schema.
        std::vector<ap::Product> products;
        auto catalog = ap::CopyAndHashInput("ContentSource/GameData/audio_cues.json",
                                             source_catalog, stage / "audio_cues.json");
        spec.inputs.push_back(catalog);
        products.push_back({"audio/audio_cues.json", "audio_cues.json",
                            catalog.digest, fs::file_size(stage / "audio_cues.json"), {}});
        const auto waves = DiscoverWaves(stage / "audio_cues.json");
        for (const auto &wave : waves)
        {
            const auto source = source_audio / wave.name;
            const auto staged = stage / wave.name;
            auto input = ap::CopyAndHashInput("ContentSource/Audio/" + wave.name,
                                               source, staged);
            ValidateWave(staged, wave.stereo);
            products.push_back({"audio/" + wave.name, wave.name, input.digest,
                                fs::file_size(staged), {}});
            spec.inputs.push_back(std::move(input));
        }
        std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);
        std::ranges::sort(products, {}, &ap::Product::logical_id);
        const auto action_key = ap::ActionKey(spec);
        const auto product_set_id = ap::ProductSetId(products);
        ap::Publish(build_root, spec, stage, std::move(products));
        std::cout << "audio action_key=" << action_key << " product_set_id="
                  << product_set_id << " files=" << waves.size() + 1 << '\n';
    }
    catch (...)
    {
        ap::RemoveStage(stage);
        throw;
    }
    ap::RemoveStage(stage);
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
#else
int main(int argc, char **argv)
#endif
{
    fs::path source_root;
    fs::path build_root;
    bool produce = false;
    try
    {
        for (int i = 1; i < argc; ++i)
        {
            const fs::path argument(argv[i]);
            if (argument == "--produce" && !produce)
                produce = true;
            else if (argument == "--source-root" && source_root.empty() && ++i < argc)
                source_root = fs::path(argv[i]);
            else if (argument == "--build-root" && build_root.empty() && ++i < argc)
                build_root = fs::path(argv[i]);
            else
                Fail("usage: hs_audio_content --produce --source-root <absolute> --build-root <absolute>");
        }
        Require(produce && !source_root.empty() && !build_root.empty(),
                "usage: hs_audio_content --produce --source-root <absolute> --build-root <absolute>");
        Produce(source_root.lexically_normal(), build_root.lexically_normal());
        return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure("hs_audio_content", error.what(),
                          {{"action_id", kActionId}, {"produce", produce},
                           {"source_root", ap::DiagnosticPath(source_root)},
                           {"build_root", ap::DiagnosticPath(build_root)}});
        return 1;
    }
}
