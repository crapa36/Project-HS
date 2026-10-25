#include "release_audio_codegen.hpp"
#include <hs/core/sha256.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

void TestReleaseAudioCodegen()
{
    const auto check = [](bool condition) { if (!condition) throw std::runtime_error("release audio codegen regression"); };
    const std::string catalog = R"({"schema_version":1,"category":"audio_cues","maximum_source_voices":64,"entries":[
      {"id":"audio.bgm.test","asset_id":"audio.bgm.test","submix":"BGM","priority":"Other","spatial":false,"preload":false,"loop":true,"encoding":"PCM","streaming":false,"max_simultaneous":1,"minimum_retrigger_ms":40,"files":["music.wav"]},
      {"id":"audio.skill.basic.impact","asset_id":"audio.skill.basic.impact","submix":"SFX","priority":"Player","spatial":true,"preload":true,"loop":false,"encoding":"PCM","streaming":false,"max_simultaneous":6,"minimum_retrigger_ms":30,"files":["impact.wav"]}]})";
    const auto wave = [](unsigned channels) {
        std::string bytes(56, '\0');
        bytes.replace(0, 4, "RIFF"); bytes.replace(8, 8, "WAVEfmt "); bytes.replace(36, 4, "data");
        const auto put = [&](unsigned offset, unsigned value, unsigned width) {
            for (unsigned i = 0; i < width; ++i) bytes[offset + i] = static_cast<char>((value >> (8 * i)) & 255);
        };
        put(4, 48, 4); put(16, 16, 4); put(20, 1, 2); put(22, channels, 2);
        put(24, 48000, 4); put(28, 48000 * channels * 3, 4); put(32, channels * 3, 2);
        put(34, 24, 2); put(40, 12, 4);
        for (unsigned i = 44; i < 56; ++i) bytes[i] = static_cast<char>(i + channels);
        return bytes;
    };
    const auto read = [&](std::string_view name) { return wave(name.starts_with("music") ? 2 : 1); };
    const auto digest_text = [](std::string_view pcm) {
        std::string text;
        for (const auto value : hs::Sha256(std::as_bytes(std::span(pcm.data(), pcm.size()))))
        {
            if (!text.empty()) text += ',';
            text += "std::byte{" + std::to_string(std::to_integer<unsigned>(value)) + "}";
        }
        return text;
    };
    const auto release = hs::audio_content::BuildReleaseAudio(catalog, read, 64);
    const auto repeated = hs::audio_content::BuildReleaseAudio(catalog, read, 64);
    check(release.source == repeated.source && release.pcm == repeated.pcm);
    check(release.pcm == wave(2).substr(44) + wave(1).substr(44));
    check(release.preload_samples == 1 && release.transition_samples == 1 &&
          release.preload_bytes == 12 && release.transition_bytes == 12);
    check(release.source.find("{64ull,12ull,48000u,2,24}") != std::string::npos &&
          release.source.find("{76ull,12ull,48000u,1,24}") != std::string::npos);
    check(release.source.find("music.wav") == std::string::npos && release.source.find("impact.wav") == std::string::npos);
    check(release.source.find("true,true,true,false}") != std::string::npos); // Spatial/preload/arrow/loop.
    check(release.source.find("std::array<AudioPcmChunk,2>") != std::string::npos);
    check(release.source.find("{64ull,12ull,0u,1u,false,{" + digest_text(wave(2).substr(44)) + "}}") != std::string::npos);
    check(release.source.find("{76ull,12ull,1u,1u,true,{" + digest_text(wave(1).substr(44)) + "}}") != std::string::npos);
    auto grouped_catalog = catalog;
    grouped_catalog.replace(grouped_catalog.find("\"music.wav\""), std::string_view("\"music.wav\"").size(), "\"music.wav\",\"music2.wav\"");
    grouped_catalog.replace(grouped_catalog.find("\"impact.wav\""), std::string_view("\"impact.wav\"").size(), "\"impact.wav\",\"impact2.wav\"");
    const auto grouped = hs::audio_content::BuildReleaseAudio(grouped_catalog, read, 64);
    check(grouped.pcm == wave(2).substr(44) + wave(2).substr(44) + wave(1).substr(44) + wave(1).substr(44));
    check(grouped.source.find("std::array<AudioPcmChunk,3>") != std::string::npos &&
          grouped.source.find("{64ull,12ull,0u,1u,false,{") != std::string::npos &&
          grouped.source.find("{76ull,12ull,1u,1u,false,{") != std::string::npos &&
          grouped.source.find("{88ull,24ull,2u,2u,true,{" + digest_text(wave(1).substr(44) + wave(1).substr(44)) + "}}") != std::string::npos);
    const auto relocated = hs::audio_content::BuildReleaseAudio(grouped_catalog, read, 256);
    check(relocated.pcm == grouped.pcm && relocated.source.find(
        "{280ull,24ull,2u,2u,true,{" + digest_text(wave(1).substr(44) + wave(1).substr(44)) + "}}") != std::string::npos);
    bool rejected = false;
    try
    {
        (void)hs::audio_content::BuildReleaseAudio(catalog, [&](std::string_view name) {
            auto bytes = read(name); bytes[40] = 13; return bytes;
        }, 64);
    }
    catch (const std::exception &) { rejected = true; }
    check(rejected);
}
