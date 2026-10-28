#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../AssetPipeline/action.hpp"
#include "../Content/release_audio_codegen.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
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
constexpr std::uint32_t kRecipeVersion = 3;
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

std::vector<std::string> DiscoverWaves(std::string_view catalog)
{
    const Json audio = Json::parse(catalog);
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
    std::vector<std::string> waves;
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
        for (const auto &file : files)
        {
            Require(file.is_string(), "audio WAV name must be a string: " + id);
            const auto name = file.get<std::string>();
            Require(SafeWaveName(name) && names.emplace(name).second,
                    "duplicate or unsafe WAV name: " + name);
            waves.push_back(name);
        }
    }
    Require(names.size() == 379, "audio catalog must reference exactly 379 unique WAV files");
    std::ranges::sort(waves);
    return waves;
}

bool Current(const fs::path &build_root, const ap::ActionSpec &spec, std::string_view key)
{
    const auto result_path = build_root / "Actions/audio/result.json";
    if (!fs::exists(result_path)) return false;
    try
    {
    const ap::ActionResultSnapshot snapshot(build_root, "audio");
    Require(snapshot.ActionId() == spec.action_id, "cached audio Action ID changed");
    if (snapshot.ActionKeyValue() != key) return false;
    Require(snapshot.ProductCount() == spec.inputs.size() + 3, "cached audio product set changed");
    std::vector<ap::Product> products;
    fs::path directory;
    const auto verify = [&](const std::string &id, const std::string &filename, std::string_view source_digest = {}) {
        const auto &metadata = snapshot.ProductMetadata(id);
        Require(!metadata.contains("dependencies") || metadata.at("dependencies") == Json::array(),
                "cached audio product dependencies changed: " + id);
        ap::UpstreamProduct dependency;
        fs::path path;
        snapshot.ReadProduct(id, dependency, path);
        Require(path.filename() == filename && (source_digest.empty() || dependency.digest == source_digest),
                "cached audio product identity changed: " + id);
        directory = path.parent_path();
        products.push_back({id, filename, dependency.digest, dependency.size, {}});
    };
    for (const auto &input : spec.inputs)
    {
        const auto filename = input.path.filename().string();
        verify("audio/" + filename, filename, input.digest);
    }
    verify("audio/release-cpp", "release_audio.cpp");
    verify("audio/release-pcm", "release_audio.pcm");
    verify("audio/release-identity", "release_audio.identity");
    std::ranges::sort(products, {}, &ap::Product::logical_id);
    Require(ap::ProductSetId(products) == directory.filename().string(), "cached audio ProductSet identity changed");
    std::size_t count{};
    for (const auto &entry : fs::directory_iterator(directory))
    {
        RequireRegularFile(entry.path());
        ++count;
    }
    Require(count == products.size(), "cached audio ProductSet has unexpected files");
    if (!fs::exists(result_path.parent_path() / "result.d")) return false;
    RequireRegularFile(result_path.parent_path() / "result.d");
    return true;
    }
    catch (const std::exception &)
    {
        // Invalid cached products must reach Publish's lease-aware repair path.
        return false;
    }
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
    const auto catalog_bytes = ap::ReadBytes(source_catalog);
    const auto catalog_digest = ap::Sha256Bytes(std::as_bytes(std::span(catalog_bytes.data(), catalog_bytes.size())));
    const auto waves = DiscoverWaves(catalog_bytes);
    ap::ActionSpec spec;
    spec.action_id = std::string(kActionId);
    spec.recipe_id = std::string(kRecipeId);
    spec.recipe_version = kRecipeVersion;
    spec.namespace_path = "audio";
    spec.include_namespace_field = false; // Preserve the original Audio ActionResult schema.
    spec.inputs.push_back({"ContentSource/GameData/audio_cues.json", source_catalog, catalog_digest});
    for (const auto &wave : waves)
        spec.inputs.push_back(ap::HashInput("ContentSource/Audio/" + wave, source_audio / wave));
    std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);
    const auto action_key = ap::ActionKey(spec);
    // This checks the captured inputs, not an atomic cross-file author revision.
    if (Current(build_root, spec, action_key))
    {
        ap::TouchActionResult(actions / "result.json");
        std::cout << "audio current files=" << waves.size() + 4 << '\n';
        return;
    }
    const auto stage = ap::CreateStage(actions);
    try
    {
        std::vector<ap::Product> products;
        ap::WriteBytes(stage / "audio_cues.json", std::as_bytes(std::span(catalog_bytes.data(), catalog_bytes.size())));
        products.push_back({"audio/audio_cues.json", "audio_cues.json",
                            catalog_digest, catalog_bytes.size(), {}});
        for (const auto &wave : waves)
        {
            const auto source = source_audio / wave;
            const auto staged = stage / wave;
            auto input = ap::CopyAndHashInput("ContentSource/Audio/" + wave,
                                               source, staged);
            const auto expected = std::ranges::lower_bound(spec.inputs, input.logical_id, {}, &ap::Input::logical_id);
            Require(expected != spec.inputs.end() && expected->logical_id == input.logical_id &&
                        expected->digest == input.digest, "audio source changed during preparation: " + wave);
            products.push_back({"audio/" + wave, wave, input.digest,
                                fs::file_size(staged), {}});
        }
        const auto release = hs::audio_content::BuildReleaseAudio(
            ap::ReadBytes(stage / "audio_cues.json"),
            [&](std::string_view name) { return ap::ReadBytes(stage / std::string(name)); }, 64);
        ap::WriteBytes(stage / "release_audio.cpp", std::as_bytes(std::span(release.source.data(), release.source.size())));
        ap::WriteBytes(stage / "release_audio.pcm", std::as_bytes(std::span(release.pcm.data(), release.pcm.size())));
        ap::WriteBytes(stage / "release_audio.identity", std::as_bytes(std::span(release.identity.data(), release.identity.size())));
        products.push_back(ap::HashProduct("audio/release-cpp", "release_audio.cpp", stage / "release_audio.cpp"));
        products.push_back(ap::HashProduct("audio/release-pcm", "release_audio.pcm", stage / "release_audio.pcm"));
        products.push_back(ap::HashProduct("audio/release-identity", "release_audio.identity", stage / "release_audio.identity"));
        std::cout << "shipping-audio PCM preload_samples=" << release.preload_samples
                  << " preload_bytes=" << release.preload_bytes
                  << " transition_samples=" << release.transition_samples
                  << " transition_bytes=" << release.transition_bytes
                  << " total_bytes=" << release.pcm.size() << '\n';
        std::ranges::sort(products, {}, &ap::Product::logical_id);
        const auto product_set_id = ap::ProductSetId(products);
        ap::Publish(build_root, spec, stage, std::move(products));
        std::cout << "audio action_key=" << action_key << " product_set_id="
                  << product_set_id << " files=" << waves.size() + 4 << '\n';
    }
    catch (...)
    {
        ap::RemoveStageAfterFailure(stage);
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
