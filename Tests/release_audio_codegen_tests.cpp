#include "release_audio_codegen.hpp"
#include <hs/runtime/audio_image_io.hpp>
#include <hs/core/sha256.hpp>
#include <algorithm>
#include <span>
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
    check(release.pcm == repeated.pcm && release.identity == repeated.identity && !release.identity.empty());
    check(release.pcm == wave(2).substr(44) + wave(1).substr(44));
    check(release.preload_samples == 1 && release.transition_samples == 1 &&
          release.preload_bytes == 12 && release.transition_bytes == 12);
    const auto bytes = [](const std::string &value) {
        return std::as_bytes(std::span(value.data(), value.size()));
    };
    hs::AudioImageData decoded;
    check(hs::DecodeAudioImage(bytes(release.identity), 64 + release.pcm.size(), decoded).Succeeded());
    const auto image = decoded.View();
    check(image.cues.size() == 2 && image.samples.size() == 2 && image.chunks.size() == 2);
    check(image.cues[0].id.value < image.cues[1].id.value &&
          image.samples[0].offset == 64 && image.samples[1].offset == 76 &&
          image.samples[0].channels == 2 && image.samples[1].channels == 1 &&
          image.samples[0].bits_per_sample == 24 && image.samples[0].sample_rate == 48000 &&
          !image.chunks[0].preload && image.chunks[1].preload);
    for (const auto &chunk : image.chunks)
        check(chunk.digest == hs::Sha256(bytes(release.pcm).subspan(
            static_cast<std::size_t>(chunk.offset - 64), static_cast<std::size_t>(chunk.bytes))));
    const auto rejects = [&](std::string bad, std::uint64_t pack_size = 88) {
        hs::AudioImageData previous = decoded;
        const auto id = previous.cues[0].id.value;
        const auto digest = previous.chunks[0].digest;
        return !hs::DecodeAudioImage(bytes(bad), pack_size, previous) &&
            previous.cues.size() == 2 && previous.cues[0].id.value == id &&
            previous.samples.size() == 2 && previous.samples[0].offset == 64 &&
            previous.chunks.size() == 2 && previous.chunks[0].digest == digest;
    };
    auto bad = release.identity;
    bad.pop_back(); check(rejects(bad));
    bad = release.identity; bad.push_back('\0'); check(rejects(bad));
    bad = release.identity; bad[8] ^= 1; check(rejects(bad));
    check(rejects(release.identity, 87));
    constexpr std::size_t header = 8 + sizeof("project_hs.release_audio") - 1 + 4 + 8;
    constexpr std::size_t cues = header + 8;
    constexpr std::size_t samples = cues + 2 * 48 + 8;
    constexpr std::size_t chunks = samples + 2 * 28 + 8;
    bad = release.identity;
    std::copy_n(bad.begin() + cues, 8, bad.begin() + cues + 48); // duplicate ID
    check(rejects(bad));
    bad = release.identity; bad[samples + 16] = 0; check(rejects(bad)); // sample rate
    bad = release.identity; bad[samples] = 65; check(rejects(bad)); // sample offset
    bad = release.identity; bad[chunks + 20] = 2; check(rejects(bad)); // transition chunk count
    bad = release.identity; bad[chunks + 28] = 31; check(rejects(bad)); // digest length
    auto grouped_catalog = catalog;
    grouped_catalog.replace(grouped_catalog.find("\"music.wav\""), std::string_view("\"music.wav\"").size(), "\"music.wav\",\"music2.wav\"");
    grouped_catalog.replace(grouped_catalog.find("\"impact.wav\""), std::string_view("\"impact.wav\"").size(), "\"impact.wav\",\"impact2.wav\"");
    const auto grouped = hs::audio_content::BuildReleaseAudio(grouped_catalog, read, 64);
    check(grouped.identity != release.identity);
    hs::AudioImageData grouped_image;
    check(hs::DecodeAudioImage(bytes(grouped.identity), 64 + grouped.pcm.size(), grouped_image).Succeeded());
    check(grouped_image.samples.size() == 4 && grouped_image.chunks.size() == 3 &&
          grouped_image.chunks.back().preload && grouped_image.chunks.back().sample_count == 2 &&
          grouped_image.chunks.back().bytes == 24);
    auto metadata_catalog = catalog;
    metadata_catalog.replace(metadata_catalog.find("\"minimum_retrigger_ms\":30"),
                             std::string_view("\"minimum_retrigger_ms\":30").size(),
                             "\"minimum_retrigger_ms\":31");
    const auto metadata_changed = hs::audio_content::BuildReleaseAudio(metadata_catalog, read, 64);
    check(metadata_changed.pcm == release.pcm && metadata_changed.identity != release.identity);
    check(grouped.pcm == wave(2).substr(44) + wave(2).substr(44) + wave(1).substr(44) + wave(1).substr(44));
    const auto relocated = hs::audio_content::BuildReleaseAudio(grouped_catalog, read, 256);
    check(relocated.identity != grouped.identity && relocated.pcm == grouped.pcm);
    check(hs::DecodeAudioImage(bytes(relocated.identity), 256 + relocated.pcm.size(), grouped_image).Succeeded() &&
          grouped_image.samples.front().offset == 256);
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
