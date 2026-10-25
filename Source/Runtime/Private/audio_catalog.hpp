#pragma once
#include <hs/runtime/audio_engine.hpp>
#include <Windows.h>
#include <mmreg.h>
#include <atomic>
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
    std::uint64_t requested_qpc{}; // Set before the reader queue's mutex publication.
    const AudioPcmChunk *chunk{};
    std::atomic<SampleState> state{SampleState::Empty};
    bool preload{};
    std::atomic<std::uint32_t> native_pins{}; // Transition buffers only.
    Result (*read_developer)(AudioSample &){};
};
struct DeveloperAudioCatalog
{
    std::vector<AudioCueRow> cues;
    std::unique_ptr<AudioSample[]> samples;
    std::size_t sample_count{};
};
[[nodiscard]] Result LoadDeveloperWavePayload(AudioSample &sample);
}
