#pragma once

#include <hs/core/result.hpp>
#include <hs/runtime/audio_image.hpp>

#include <span>
#include <vector>

namespace hs
{
struct AudioImageData
{
    std::vector<AudioCueRow> cues;
    std::vector<AudioSampleRow> samples;
    std::vector<AudioPcmChunk> chunks;

    [[nodiscard]] AudioImage View() const noexcept { return {cues, samples, chunks}; }
};

[[nodiscard]] Result DecodeAudioImage(std::span<const std::byte> bytes,
                                      std::uint64_t pack_size, AudioImageData &output);
}
