#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace hs::audio_content
{
struct ReleaseAudio
{
    std::string source;
    std::string pcm;
    std::string identity;
    std::uint64_t preload_bytes{}, transition_bytes{};
    std::uint32_t preload_samples{}, transition_samples{};
};
// Build-only source validation and shipping::Audio() rows with PCM chunk digests.
// read_wave receives a validated plain WAV filename, never a runtime resolver.
[[nodiscard]] ReleaseAudio BuildReleaseAudio(std::string_view catalog,
    const std::function<std::string(std::string_view)> &read_wave, std::uint64_t pcm_offset);
}
