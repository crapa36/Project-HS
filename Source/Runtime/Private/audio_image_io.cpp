#include <hs/runtime/audio_image_io.hpp>
#include <hs/core/canonical_reader.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace hs
{
Result DecodeAudioImage(std::span<const std::byte> bytes, std::uint64_t pack_size,
                        AudioImageData &output)
{
    try
    {
        const auto require = [](bool condition, const char *message) {
            if (!condition) throw std::runtime_error(message);
        };
        CanonicalReader in(bytes, "project_hs.release_audio", 1);
        const auto pcm_offset = in.Scalar<std::uint64_t>();
        require(pcm_offset <= pack_size, "Audio PCM starts outside the pack.");
        AudioImageData decoded;
        const auto cue_count = in.Count(48);
        require(cue_count && cue_count <= UINT32_MAX, "Invalid audio cue count.");
        decoded.cues.resize(cue_count);
        for (auto &cue : decoded.cues)
        {
            in.Fields(cue.id.value, cue.first_sample, cue.sample_count, cue.max_simultaneous,
                      cue.retrigger_ms, cue.bus, cue.priority, cue.spatial, cue.preload,
                      cue.arrow_impact_family, cue.loop);
            require(cue.id.value && cue.sample_count && cue.max_simultaneous &&
                cue.bus >= AudioBus::Bgm && cue.bus <= AudioBus::Ui &&
                cue.priority <= AudioPriority::Ui, "Invalid audio cue.");
        }
        const auto sample_count = in.Count(28);
        require(sample_count && sample_count <= UINT32_MAX, "Invalid audio sample count.");
        decoded.samples.resize(sample_count);
        std::uint64_t end = pcm_offset;
        for (auto &sample : decoded.samples)
        {
            in.Fields(sample.offset, sample.bytes, sample.sample_rate, sample.channels, sample.bits_per_sample);
            require(sample.offset == end && sample.bytes && sample.sample_rate == 48000 &&
                (sample.channels == 1 || sample.channels == 2) && sample.bits_per_sample == 24 &&
                sample.bytes % (std::uint64_t(sample.channels) * 3) == 0 &&
                sample.bytes <= pack_size - end && sample.bytes <= std::numeric_limits<std::size_t>::max(),
                "Invalid audio sample format or pack range.");
            end += sample.bytes;
        }
        std::vector<std::uint8_t> coverage(sample_count, 0);
        for (std::size_t i = 0; i < decoded.cues.size(); ++i)
        {
            const auto &cue = decoded.cues[i];
            require(!i || decoded.cues[i - 1].id.value < cue.id.value, "Audio cues are not sorted and unique.");
            require(cue.first_sample <= sample_count && cue.sample_count <= sample_count - cue.first_sample,
                "Audio cue sample range is invalid.");
            for (std::size_t j = cue.first_sample; j < std::size_t(cue.first_sample) + cue.sample_count; ++j)
            {
                require(!coverage[j] && (!cue.spatial || decoded.samples[j].channels == 1),
                    "Audio cue sample is duplicated or has invalid spatial channels.");
                coverage[j] = cue.preload ? 2 : 1;
            }
        }
        require(std::ranges::none_of(coverage, [](auto value) { return value == 0; }),
            "Audio cue samples do not cover the sample table.");
        const auto chunk_count = in.Count(68);
        require(chunk_count && chunk_count <= sample_count, "Invalid audio chunk count.");
        decoded.chunks.resize(chunk_count);
        std::size_t next_sample{};
        std::uint64_t preload_total{};
        for (auto &chunk : decoded.chunks)
        {
            in.Fields(chunk.offset, chunk.bytes, chunk.first_sample, chunk.sample_count, chunk.preload);
            const auto digest = in.Blob();
            require(digest.size() == chunk.digest.size(), "Invalid audio chunk digest size.");
            std::copy(digest.begin(), digest.end(), chunk.digest.begin());
            require(chunk.sample_count && chunk.first_sample == next_sample &&
                chunk.sample_count <= sample_count - next_sample &&
                (chunk.preload || chunk.sample_count == 1), "Invalid audio chunk sample range.");
            require(chunk.offset == decoded.samples[next_sample].offset, "Audio chunk start does not match its sample.");
            std::uint64_t chunk_bytes{};
            for (std::size_t j = next_sample; j < next_sample + chunk.sample_count; ++j)
            {
                require(coverage[j] == (chunk.preload ? 2 : 1) &&
                    decoded.samples[j].bytes <= UINT64_MAX - chunk_bytes,
                    "Audio chunk preload or size disagrees with its samples.");
                chunk_bytes += decoded.samples[j].bytes;
            }
            require(chunk.bytes == chunk_bytes && chunk.bytes <= std::numeric_limits<std::size_t>::max(),
                "Audio chunk byte extent disagrees with its samples.");
            if (chunk.preload)
            {
                require(chunk.bytes <= std::numeric_limits<std::size_t>::max() - preload_total,
                    "Audio preload size overflows host storage.");
                preload_total += chunk.bytes;
            }
            next_sample += chunk.sample_count;
        }
        require(next_sample == sample_count, "Audio chunks do not cover the sample table.");
        in.Finish();
        output = std::move(decoded);
        return Result::Success();
    }
    catch (const std::runtime_error &error)
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_audio_image", error.what());
    }
}
}
