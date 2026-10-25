#include "release_audio_codegen.hpp"
#include <hs/runtime/audio_image.hpp>
#include <hs/core/cooked_format.hpp>
#include <hs/core/sha256.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

namespace hs::audio_content
{
namespace
{
void Require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
bool Arrow(std::string_view id)
{
    return id.starts_with("audio.common.arrow_impact") || id == "audio.skill.basic.impact" ||
        id == "audio.skill.piercing.pierce" || id == "audio.skill.charged.hit" ||
        id == "audio.skill.ricochet.hit" || id == "audio.skill.arrow_rain.impact";
}
std::uint32_t U32(std::string_view s, std::size_t p)
{
    std::uint32_t value{};
    for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t(static_cast<unsigned char>(s[p + i])) << (i * 8);
    return value;
}
std::uint16_t U16(std::string_view s, std::size_t p)
{
    return std::uint16_t(static_cast<unsigned char>(s[p]) | (unsigned(static_cast<unsigned char>(s[p + 1])) << 8));
}
}
ReleaseAudio BuildReleaseAudio(std::string_view catalog_text,
    const std::function<std::string(std::string_view)> &read_wave, std::uint64_t pcm_offset)
{
    const auto catalog = nlohmann::json::parse(catalog_text);
    Require(catalog.at("schema_version") == 1 && catalog.at("maximum_source_voices") == 64 &&
            catalog.at("category") == "audio_cues", "unsupported release audio catalog");
    ReleaseAudio result;
    std::vector<AudioCueRow> cues;
    std::vector<AudioSampleRow> samples;
    std::vector<bool> sample_preload;
    std::set<std::string> names, cue_names;
    std::set<std::uint64_t> ids;
    for (const auto &entry : catalog.at("entries"))
    {
        const auto id = entry.at("id").get<std::string>();
        const auto asset = entry.at("asset_id").get<std::string>();
        Require(!id.empty() && !asset.empty() && cue_names.insert(id).second &&
                (id == asset || cue_names.insert(asset).second), "duplicate release audio name");
        AudioCueRow cue;
        cue.id = MakeAssetId(asset);
        Require(cue.id.value && ids.insert(cue.id.value).second, "duplicate release audio identity");
        const auto bus = entry.at("submix").get<std::string>();
        Require(bus == "BGM" || bus == "SFX" || bus == "UI", "unsupported release audio bus");
        cue.bus = bus == "BGM" ? AudioBus::Bgm : bus == "UI" ? AudioBus::Ui : AudioBus::Sfx;
        const auto priority = entry.at("priority").get<std::string>();
        constexpr std::array priorities{"Other", "Enemy", "Player", "BossTelegraph", "UI"};
        const auto found = std::ranges::find(priorities, priority);
        Require(found != priorities.end(), "unsupported release audio priority");
        cue.priority = static_cast<AudioPriority>(found - priorities.begin());
        Require(entry.at("encoding") == "PCM" && !entry.at("streaming").get<bool>(), "unsupported release audio encoding");
        cue.spatial = entry.at("spatial").get<bool>(); cue.preload = entry.at("preload").get<bool>();
        cue.loop = entry.at("loop").get<bool>(); cue.arrow_impact_family = Arrow(asset);
        cue.max_simultaneous = std::max(1u, entry.at("max_simultaneous").get<std::uint32_t>());
        cue.retrigger_ms = entry.at("minimum_retrigger_ms").get<std::uint32_t>();
        cue.first_sample = static_cast<std::uint32_t>(samples.size());
        const auto &files = entry.at("files");
        Require(files.is_array() && !files.empty(), "release audio cue needs samples");
        for (const auto &file : files)
        {
            const auto name = file.get<std::string>();
            Require(name.ends_with(".wav") && name.find("..") == std::string::npos &&
                    name.find_first_of("/\\:") == std::string::npos && names.insert(name).second,
                    "duplicate or unsafe release WAV filename");
            const auto wave = read_wave(name);
            Require(wave.size() >= 44 && wave.size() - 8 <= UINT32_MAX &&
                    wave.compare(0, 4, "RIFF") == 0 && wave.compare(8, 8, "WAVEfmt ") == 0 &&
                    wave.compare(36, 4, "data") == 0, "release WAV must use canonical PCM chunks");
            const auto channels = U16(wave, 22), bits = U16(wave, 34);
            const auto rate = U32(wave, 24), bytes = U32(wave, 40);
            const bool stereo = id.starts_with("audio.bgm.") || id.starts_with("audio.ambience.");
            Require(U32(wave, 4) == wave.size() - 8 && U32(wave, 16) == 16 && U16(wave, 20) == 1 &&
                    channels == (stereo ? 2 : 1) && rate == 48000 && bits == 24 &&
                    U32(wave, 28) == rate * channels * 3 && U16(wave, 32) == channels * 3 &&
                    bytes > 0 && bytes % (channels * 3) == 0 &&
                    std::uint64_t(bytes) + 44 + (bytes & 1u) == wave.size() && (!cue.spatial || channels == 1),
                    "release WAV format or extent invalid");
            Require(result.pcm.size() <= UINT64_MAX - pcm_offset && bytes <= UINT64_MAX - pcm_offset - result.pcm.size(),
                    "release PCM extent overflow");
            samples.push_back({pcm_offset + result.pcm.size(), bytes, rate, channels, bits});
            sample_preload.push_back(cue.preload);
            result.pcm.append(wave.data() + 44, bytes);
            if (cue.preload) { result.preload_bytes += bytes; ++result.preload_samples; }
            else { result.transition_bytes += bytes; ++result.transition_samples; }
        }
        cue.sample_count = static_cast<std::uint32_t>(samples.size()) - cue.first_sample;
        cues.push_back(cue);
    }
    Require(!cues.empty(), "empty release audio image");
    std::vector<AudioPcmChunk> chunks;
    for (std::size_t first = 0; first < samples.size();)
    {
        auto end = first + 1;
        auto bytes = samples[first].bytes;
        if (sample_preload[first])
            while (end < samples.size() && sample_preload[end] && samples[end].offset == samples[first].offset + bytes)
                bytes += samples[end++].bytes;
        const auto pcm = std::as_bytes(std::span(result.pcm.data(), result.pcm.size())).subspan(
            static_cast<std::size_t>(samples[first].offset - pcm_offset), static_cast<std::size_t>(bytes));
        chunks.push_back({samples[first].offset, bytes, static_cast<std::uint32_t>(first),
            static_cast<std::uint32_t>(end - first), sample_preload[first], hs::Sha256(pcm)});
        first = end;
    }
    std::ranges::sort(cues, {}, [](const AudioCueRow &cue) { return cue.id.value; });
    result.source = "\n#include <hs/runtime/audio_image.hpp>\n#include <array>\nnamespace hs::shipping { namespace {\n";
    result.source += "constinit const std::array<AudioSampleRow," + std::to_string(samples.size()) + "> kAudioSamples{{\n";
    for (const auto &s : samples)
        result.source += "{" + std::to_string(s.offset) + "ull," + std::to_string(s.bytes) + "ull," +
            std::to_string(s.sample_rate) + "u," + std::to_string(s.channels) + "," + std::to_string(s.bits_per_sample) + "},\n";
    result.source += "}};\nconstinit const std::array<AudioCueRow," + std::to_string(cues.size()) + "> kAudioCues{{\n";
    for (const auto &c : cues)
        result.source += "{{" + std::to_string(c.id.value) + "ull}," + std::to_string(c.first_sample) + "u," +
            std::to_string(c.sample_count) + "u," + std::to_string(c.max_simultaneous) + "u," +
            std::to_string(c.retrigger_ms) + "u,AudioBus(" + std::to_string(unsigned(c.bus)) + "),AudioPriority(" +
            std::to_string(unsigned(c.priority)) + ")," + (c.spatial ? "true," : "false,") +
            (c.preload ? "true," : "false,") + (c.arrow_impact_family ? "true," : "false,") + (c.loop ? "true},\n" : "false},\n");
    result.source += "}};\nconstinit const std::array<AudioPcmChunk," + std::to_string(chunks.size()) + "> kAudioChunks{{\n";
    for (const auto &chunk : chunks)
    {
        result.source += "{" + std::to_string(chunk.offset) + "ull," + std::to_string(chunk.bytes) + "ull," +
            std::to_string(chunk.first_sample) + "u," + std::to_string(chunk.sample_count) + "u," +
            (chunk.preload ? "true,{" : "false,{");
        for (std::size_t index = 0; index < chunk.digest.size(); ++index)
        {
            if (index) result.source += ',';
            result.source += "std::byte{" + std::to_string(std::to_integer<unsigned>(chunk.digest[index])) + "}";
        }
        result.source += "}},\n";
    }
    result.source += "}};\nconstinit const AudioImage kAudio{kAudioCues,kAudioSamples,kAudioChunks};\n}\n"
        "const AudioImage &Audio() noexcept { return kAudio; }\n}\n";
    return result;
}
}
