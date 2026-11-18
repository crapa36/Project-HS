#pragma once
#include <hs/runtime/audio_engine.hpp>
#include <Windows.h>
#include <mmreg.h>
#include <atomic>
#include <memory>
#include <vector>
#include <optional>

namespace hs::audio_detail
{
enum class SampleState : std::uint8_t { Empty, Queued, Ready, Failed };
struct AudioSample
{
    std::filesystem::path path; // Developer adapter only; shipping uses the shared pack range.
    std::vector<std::byte> bytes;
    std::span<const std::byte> pcm;
    WAVEFORMATEX format{};
    std::uint64_t data_offset{}, data_size{};
    const AudioPcmChunk *chunk{};
    std::atomic<SampleState> state{SampleState::Empty};
    std::atomic<std::uint32_t> native_pins{}; // Transition buffers only.
    Result (*read_developer)(AudioSample &){};
};
struct PreparedAudio
{
    std::vector<AudioCueRow> cues;
    std::span<const AudioCueRow> packed_cues;
    std::unique_ptr<AudioSample[]> samples;
    std::size_t sample_count{};
    std::unique_ptr<std::byte[]> resident_pcm;
    const ShippedContentOwner *content{};
};
[[nodiscard]] Result LoadDeveloperWavePayload(AudioSample &sample);
}
