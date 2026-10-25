#include "audio_catalog.hpp"
#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/core/cooked_format.hpp>
#include <hs/core/process_info.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_set>
namespace hs {
using namespace audio_detail;
namespace {
[[nodiscard]] bool IsArrowImpact(std::string_view id) noexcept
{
    return id.starts_with("audio.common.arrow_impact") ||
           id == "audio.skill.basic.impact" ||
           id == "audio.skill.piercing.pierce" ||
           id == "audio.skill.charged.hit" ||
           id == "audio.skill.ricochet.hit" ||
           id == "audio.skill.arrow_rain.impact";
}

using Json = nlohmann::json;

[[nodiscard]] std::uint16_t U16(const std::vector<std::byte> &b, std::size_t p) noexcept
{
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(b[p]) |
                                      (std::to_integer<unsigned>(b[p + 1]) << 8));
}
[[nodiscard]] std::uint32_t U32(const std::vector<std::byte> &b, std::size_t p) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned>(b[p]) |
                                      (std::to_integer<unsigned>(b[p + 1]) << 8) |
                                      (std::to_integer<unsigned>(b[p + 2]) << 16) |
                                      (std::to_integer<unsigned>(b[p + 3]) << 24));
}

[[nodiscard]] std::uint16_t ReadU16(const std::byte *bytes) noexcept
{
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[0]) |
                                      (std::to_integer<unsigned>(bytes[1]) << 8));
}

[[nodiscard]] std::uint32_t ReadU32(const std::byte *bytes) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[0]) |
                                      (std::to_integer<unsigned>(bytes[1]) << 8) |
                                      (std::to_integer<unsigned>(bytes[2]) << 16) |
                                      (std::to_integer<unsigned>(bytes[3]) << 24));
}

[[nodiscard]] Result LoadWaveHeader(const std::filesystem::path &path,
                                     AudioSample &out)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return Result::Failure(ErrorCode::TaskFailed, "audio", "WAV open failed");
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length < 12 || static_cast<std::uint64_t>(length) > std::numeric_limits<std::uint32_t>::max() + 8ull)
        return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV size invalid");
    const auto file_size = static_cast<std::uint64_t>(length);
    std::array<std::byte, 12> riff{};
    input.seekg(0);
    input.read(reinterpret_cast<char *>(riff.data()), static_cast<std::streamsize>(riff.size()));
    if (!input || std::memcmp(riff.data(), "RIFF", 4) != 0 ||
        std::memcmp(riff.data() + 8, "WAVE", 4) != 0)
        return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV RIFF header invalid");
    if (ReadU32(riff.data() + 4) != file_size - 8)
        return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV RIFF size invalid");

    std::uint64_t cursor = 12;
    std::vector<std::byte> fmt;
    std::uint64_t data_offset{};
    std::uint64_t data_size{};
    while (cursor + 8 <= file_size)
    {
        input.seekg(static_cast<std::streamoff>(cursor));
        std::array<std::byte, 8> chunk_header{};
        input.read(reinterpret_cast<char *>(chunk_header.data()),
                   static_cast<std::streamsize>(chunk_header.size()));
        if (!input)
            return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV chunk header truncated");
        const auto chunk_size = static_cast<std::uint64_t>(ReadU32(chunk_header.data() + 4));
        const auto begin = cursor + 8;
        if (chunk_size > file_size - begin)
            return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV chunk exceeds file");
        if (std::memcmp(chunk_header.data(), "fmt ", 4) == 0)
        {
            if (chunk_size > 4096)
                return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV fmt chunk too large");
            fmt.resize(static_cast<std::size_t>(chunk_size));
            input.read(reinterpret_cast<char *>(fmt.data()),
                       static_cast<std::streamsize>(fmt.size()));
        }
        else if (std::memcmp(chunk_header.data(), "data", 4) == 0 && data_size == 0)
        {
            data_offset = begin;
            data_size = chunk_size;
        }
        const auto padded_end = begin + chunk_size + (chunk_size & 1u);
        if (padded_end > file_size)
            return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV chunk padding exceeds file");
        cursor = padded_end;
    }
    if (cursor != file_size)
        return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV chunk table truncated");
    if (fmt.size() < 16 || data_size == 0)
        return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV fmt/data missing");
    const auto channels = U16(fmt, 2);
    const auto sample_rate = U32(fmt, 4);
    const auto block_align = U16(fmt, 12);
    const auto bits = U16(fmt, 14);
    if (U16(fmt, 0) != WAVE_FORMAT_PCM || (channels != 1 && channels != 2) ||
        sample_rate != 48000 || bits != 24 || block_align != channels * 3 ||
        U32(fmt, 8) != sample_rate * block_align || data_size % block_align != 0)
        return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV must be PCM 48kHz 24-bit mono/stereo");
    out.path = path;
    out.read_developer = LoadDeveloperWavePayload;
    out.data_offset = data_offset;
    out.data_size = data_size;
    out.format = {WAVE_FORMAT_PCM, channels, sample_rate, sample_rate * block_align,
                   block_align, bits, 0};
    return Result::Success();
}

[[nodiscard]] AudioBus ParseBus(const std::string &value) noexcept
{
    if (value == "BGM") return AudioBus::Bgm;
    if (value == "UI") return AudioBus::Ui;
    return AudioBus::Sfx;
}
[[nodiscard]] AudioPriority ParsePriority(const std::string &value) noexcept
{
    if (value == "UI") return AudioPriority::Ui;
    if (value == "BossTelegraph") return AudioPriority::BossWarning;
    if (value == "Player") return AudioPriority::Player;
    if (value == "Enemy") return AudioPriority::Enemy;
    return AudioPriority::Other;
}

[[nodiscard]] bool IsKnownBus(const std::string &value) noexcept
{
    return value == "BGM" || value == "SFX" || value == "UI";
}
[[nodiscard]] bool IsKnownPriority(const std::string &value) noexcept
{
    return value == "Other" || value == "Enemy" || value == "Player" ||
           value == "BossTelegraph" || value == "UI";
}

}
namespace audio_detail {
[[nodiscard]] Result LoadDeveloperWavePayload(AudioSample &sample)
{
    if ((sample.state.load(std::memory_order_acquire) == SampleState::Ready)) return Result::Success();
    if (sample.data_size > std::numeric_limits<std::size_t>::max() ||
        sample.data_size > std::numeric_limits<UINT32>::max())
        return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV payload too large");
    std::ifstream input(sample.path, std::ios::binary);
    if (!input) return Result::Failure(ErrorCode::TaskFailed, "audio", "WAV open failed");
    input.seekg(static_cast<std::streamoff>(sample.data_offset));
    sample.bytes.resize(static_cast<std::size_t>(sample.data_size));
    input.read(reinterpret_cast<char *>(sample.bytes.data()),
               static_cast<std::streamsize>(sample.bytes.size()));
    if (!input)
    {
        sample.bytes.clear();
        return Result::Failure(ErrorCode::TaskFailed, "audio", "WAV payload read failed");
    }
    sample.pcm = sample.bytes;
    return Result::Success();
}

}
Result AudioEngine::Initialize(const SettingsData &settings)
{
    SelectedGeneration generation;
    if (auto loaded = LoadSelectedGeneration(
            CurrentExecutableDirectory() / "GenerationState/runtime-full.json",
            generation); !loaded)
        return loaded;
    const auto initialized = Initialize(settings, generation.product_paths);
    if (initialized)
        RetainGeneration(std::move(generation));
    return initialized;
}

Result AudioEngine::Initialize(const SettingsData &settings,
                               const std::filesystem::path &audio_directory)
{
    return InitializeResolved(settings, [&](std::string_view logical_id,
                                            std::filesystem::path &path) {
        path = audio_directory / std::string(logical_id.substr(6));
        return Result::Success();
    });
}

Result AudioEngine::Initialize(
    const SettingsData &settings,
    const std::unordered_map<std::string, std::filesystem::path> &product_paths)
{
    return InitializeResolved(settings, [&](std::string_view logical_id,
                                            std::filesystem::path &path) {
        const auto found = product_paths.find(std::string(logical_id));
        if (found == product_paths.end())
            return Result::Failure(ErrorCode::TaskFailed, "audio",
                                   "audio product missing: " + std::string(logical_id));
        if (!found->second.is_absolute())
            return Result::Failure(ErrorCode::InvalidArgument, "audio",
                                   "audio product path is not absolute: " + std::string(logical_id));
        path = found->second;
        return Result::Success();
    });
}

Result AudioEngine::InitializeResolved(const SettingsData &settings,
                                       const ResolveAudioPath &resolve_path)
{
    Shutdown();
    DeveloperAudioCatalog prepared;
    try
    {
        std::filesystem::path catalog_path;
        if (auto resolved = resolve_path("audio/audio_cues.json", catalog_path); !resolved)
            return resolved;
        std::ifstream catalog_file(catalog_path);
        if (!catalog_file)
            return Result::Failure(ErrorCode::TaskFailed, "audio", "audio catalog open failed");
        const auto catalog = Json::parse(catalog_file);
        if (!catalog.is_object() || !catalog.contains("entries") || !catalog.at("entries").is_array())
            return Result::Failure(ErrorCode::InvalidArgument, "audio", "audio catalog entries invalid");
        for (const auto &item : catalog.at("entries"))
        {
            const auto &files = item.at("files");
            if (!files.is_array() || files.empty())
                return Result::Failure(ErrorCode::InvalidArgument, "audio", "audio cue has no files");
            prepared.sample_count += files.size();
        }
        prepared.samples = std::make_unique<AudioSample[]>(prepared.sample_count);
        std::size_t sample_index{};
        std::unordered_set<std::string> cue_names;
        std::unordered_set<std::uint64_t> cue_hashes;
        std::unordered_set<std::string> file_names;
        for (const auto &item : catalog.at("entries"))
        {
            const auto id = item.at("id").get<std::string>();
            const auto asset_name = item.at("asset_id").get<std::string>();
            if (!cue_names.insert(id).second ||
                (asset_name != id && !cue_names.insert(asset_name).second))
                return Result::Failure(ErrorCode::InvalidArgument, "audio", "duplicate audio cue name");
            const auto hash = MakeAssetId(asset_name).value;
            if (hash == 0 || !cue_hashes.insert(hash).second)
                return Result::Failure(ErrorCode::InvalidArgument, "audio", "duplicate or empty audio cue id");
            AudioCueRow cue;
            cue.first_sample = static_cast<std::uint32_t>(sample_index);
            cue.id = {hash};
            const auto submix = item.at("submix").get<std::string>();
            const auto priority_name = item.at("priority").get<std::string>();
            if (!IsKnownBus(submix) || !IsKnownPriority(priority_name) ||
                item.at("encoding").get<std::string>() != "PCM" ||
                item.at("streaming").get<bool>())
                return Result::Failure(ErrorCode::InvalidArgument, "audio", "unsupported audio catalog value");
            cue.bus = ParseBus(submix);
            cue.priority = ParsePriority(priority_name);
            cue.spatial = item.at("spatial").get<bool>();
            cue.preload = item.at("preload").get<bool>();
            cue.arrow_impact_family = IsArrowImpact(asset_name);
            cue.loop = item.at("loop").get<bool>();
            cue.max_simultaneous = std::max(1u, item.at("max_simultaneous").get<std::uint32_t>());
            cue.retrigger_ms = item.at("minimum_retrigger_ms").get<std::uint32_t>();
            const auto &files = item.at("files");
            if (!files.is_array() || files.empty())
                return Result::Failure(ErrorCode::InvalidArgument, "audio", "audio cue has no files");
            for (const auto &file : files)
            {
                const auto name = file.get<std::string>();
                const std::filesystem::path relative{name};
                if (name.empty() || relative.is_absolute() || relative.has_parent_path() ||
                    relative.filename().string() != name || relative.extension() != ".wav")
                    return Result::Failure(ErrorCode::InvalidArgument, "audio", "unsafe audio filename");
                if (!file_names.insert(name).second)
                    return Result::Failure(ErrorCode::InvalidArgument, "audio", "duplicate audio filename");
                std::filesystem::path wave_path;
                if (auto resolved = resolve_path("audio/" + name, wave_path); !resolved)
                    return resolved;
                auto &sample = prepared.samples[sample_index++];
                if (auto loaded = LoadWaveHeader(wave_path, sample); !loaded)
                    return loaded;
                if (cue.spatial && sample.format.nChannels != 1)
                    return Result::Failure(ErrorCode::InvalidArgument, "audio", "spatial audio must be mono");
                if (cue.preload)
                {
                    if (auto loaded = LoadDeveloperWavePayload(sample); !loaded)
                        return loaded;
                    sample.state.store(SampleState::Ready, std::memory_order_release);
                }

            }
            cue.sample_count = static_cast<std::uint32_t>(sample_index) - cue.first_sample;
            prepared.cues.push_back(cue);
        }
    }
    catch (const std::exception &error)
    {
        return Result::Failure(ErrorCode::InvalidArgument, "audio", error.what());
    }

    return InitializePrepared(settings, std::move(prepared));
}
}
