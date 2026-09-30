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

enum class PresentationGeometryKind : std::uint8_t
{
    None,
    Circle,
    Line,
    Cone,
    RingGaps,
    Projectile,
};

// Explicit gameplay shape, kept outside the retained 32-byte event payload.
struct PresentationGeometry
{
    PresentationGeometryKind kind{PresentationGeometryKind::None};
    float radius{};
    float inner_radius{};
    float outer_radius{};
    float width{};
    float range{};
    float half_angle_degrees{};
    std::uint8_t gap_count{};
    float gap_offset_degrees{};
    float gap_half_width_degrees{};
    Float3 velocity{};
    Tick start_tick{};
    Tick end_tick{};
    std::uint64_t source_id{};
    Float3 end_position{};
};

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
    float vfx_ratio01{-1.0f};
    std::uint64_t status_episode_generation{};
    PresentationGeometry geometry{};
    std::uint64_t session_id{};
    // Opaque domain source identity and stage (0 none, 1 spawn, 2 telegraph,
    // 3 resolve); Runtime interprets these against the cooked upgrade binding.
    std::uint8_t upgrade_skill{0xFF}, upgrade_index{0xFF}, upgrade_stage{};
    std::uint64_t upgrade_cast_id{}, upgrade_owner_id{};
};

} // namespace hs
