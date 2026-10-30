#include "release_audio_codegen.hpp"
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
    const auto release = hs::audio_content::BuildReleaseAudio(catalog, read, 64);
    const auto repeated = hs::audio_content::BuildReleaseAudio(catalog, read, 64);
    check(release.source == repeated.source && release.pcm == repeated.pcm &&
          release.identity == repeated.identity && !release.identity.empty());
    check(release.pcm == wave(2).substr(44) + wave(1).substr(44));
    check(release.preload_samples == 1 && release.transition_samples == 1 &&
          release.preload_bytes == 12 && release.transition_bytes == 12);
    auto grouped_catalog = catalog;
    grouped_catalog.replace(grouped_catalog.find("\"music.wav\""), std::string_view("\"music.wav\"").size(), "\"music.wav\",\"music2.wav\"");
    grouped_catalog.replace(grouped_catalog.find("\"impact.wav\""), std::string_view("\"impact.wav\"").size(), "\"impact.wav\",\"impact2.wav\"");
    const auto grouped = hs::audio_content::BuildReleaseAudio(grouped_catalog, read, 64);
    check(grouped.identity != release.identity);
    auto metadata_catalog = catalog;
    metadata_catalog.replace(metadata_catalog.find("\"minimum_retrigger_ms\":30"),
                             std::string_view("\"minimum_retrigger_ms\":30").size(),
                             "\"minimum_retrigger_ms\":31");
    const auto metadata_changed = hs::audio_content::BuildReleaseAudio(metadata_catalog, read, 64);
    check(metadata_changed.pcm == release.pcm && metadata_changed.identity != release.identity);
    check(grouped.pcm == wave(2).substr(44) + wave(2).substr(44) + wave(1).substr(44) + wave(1).substr(44));
    const auto relocated = hs::audio_content::BuildReleaseAudio(grouped_catalog, read, 256);
    check(relocated.identity != grouped.identity && relocated.pcm == grouped.pcm);
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
