#pragma once

#include <hs/core/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hs
{

enum class PresentationKind : std::uint8_t
{
    Vfx,
    Audio,
    Ui,
};

enum class VfxEventFlag : std::uint32_t { None = 0, HasTarget = 1u << 0 };

enum class AudioEventAction : std::uint8_t { Play, Stop };

inline std::array<std::byte, 32> EncodeAudioAction(AudioEventAction action) noexcept
{
    std::array<std::byte, 32> bytes{};
    bytes[0] = static_cast<std::byte>(action);
    return bytes;
}

[[nodiscard]] inline AudioEventAction DecodeAudioAction(
    const std::array<std::byte, 32> &bytes) noexcept
{
    return static_cast<AudioEventAction>(bytes[0]);
}

struct VfxEventParameters
{
    Float3 direction{0.0f, 0.0f, 1.0f};
    float scale{1.0f};
    Float3 target{};
    std::uint32_t flags{};
};
static_assert(sizeof(VfxEventParameters) == 32);

inline std::array<std::byte, 32> EncodeVfxParameters(const VfxEventParameters &value) noexcept
{
    std::array<std::byte, 32> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

inline VfxEventParameters DecodeVfxParameters(
    const std::array<std::byte, 32> &bytes) noexcept
{
    VfxEventParameters value;
    std::memcpy(&value, bytes.data(), sizeof(value));
    return value;
}

struct PresentationEvent
{
    Sequence sequence{};
    Tick tick{};
    PresentationKind kind{};
    Float3 position{};
    AssetId asset{};
    std::array<std::byte, 32> parameters{};
};

} // namespace hs
