#pragma once
#include <hs/core/types.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace hs
{
enum class AudioBus : std::uint8_t { Master, Bgm, Sfx, Ui };
enum class AudioPriority : std::uint8_t { Other, Enemy, Player, BossWarning, Ui };
struct AudioSampleRow
{
    std::uint64_t offset{}, bytes{};
    std::uint32_t sample_rate{};
    std::uint16_t channels{}, bits_per_sample{};
};
struct AudioCueRow
{
    AssetId id{};
    std::uint32_t first_sample{}, sample_count{}, max_simultaneous{}, retrigger_ms{};
    AudioBus bus{};
    AudioPriority priority{};
    bool spatial{}, preload{}, arrow_impact_family{}, loop{};
};
// One verified preload run or one independently evictable transition sample.
struct AudioPcmChunk
{
    std::uint64_t offset{}, bytes{};
    std::uint32_t first_sample{}, sample_count{};
    bool preload{};
    std::array<std::byte, 32> digest{};
};
struct AudioImage
{
    std::span<const AudioCueRow> cues;
    std::span<const AudioSampleRow> samples;
    std::span<const AudioPcmChunk> chunks;
};
namespace shipping { [[nodiscard]] const AudioImage &Audio() noexcept; }
}
