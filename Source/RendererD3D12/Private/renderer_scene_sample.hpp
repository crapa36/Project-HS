#pragma once

#include <hs/core/render_snapshot.hpp>

#include <array>
#include <bit>

namespace hs
{

inline const AnimationPoseRef *FindLastRenderPose(std::span<const AnimationPoseRef> poses,
                                                 std::uint32_t source_index) noexcept
{
    const AnimationPoseRef *result{};
    for (const auto &pose : poses)
        if (pose.instance_index == source_index) result = &pose;
    return result;
}

// Exact live inputs; supplied development revisions cannot conceal scalar edits.
struct RenderPoseKey
{
    std::array<std::uint64_t, 18> values{};
    bool operator==(const RenderPoseKey &) const noexcept = default;
};

inline RenderPoseKey MakeRenderPoseKey(const RenderInstance &instance,
                                       const AnimationPoseRef *pose) noexcept
{
    const AnimationPoseRef empty{};
    const auto &p = pose ? *pose : empty;
    return {{instance.asset_id.value, static_cast<std::uint64_t>(instance.mesh),
        instance.animation_clip_id.value,
        std::bit_cast<std::uint64_t>(instance.animation_time_seconds),
        std::bit_cast<std::uint32_t>(instance.animation_playback_rate),
        instance.revisions.pose, p.revision, static_cast<std::uint64_t>(p.clip),
        std::bit_cast<std::uint32_t>(p.normalized_time),
        std::bit_cast<std::uint32_t>(p.playback_rate),
        static_cast<std::uint64_t>(p.secondary_clip),
        std::bit_cast<std::uint32_t>(p.secondary_normalized_time),
        std::bit_cast<std::uint32_t>(p.secondary_playback_rate),
        std::bit_cast<std::uint32_t>(p.secondary_weight),
        static_cast<std::uint64_t>(p.upper_body_clip),
        std::bit_cast<std::uint32_t>(p.upper_body_normalized_time),
        std::bit_cast<std::uint32_t>(p.upper_body_playback_rate),
        std::bit_cast<std::uint32_t>(p.upper_body_weight)}};
}

using RenderContentKey = std::array<std::uint64_t, 6>;
inline RenderContentKey MakeRenderContentKey(const RenderInstance &instance) noexcept
{
    return {instance.asset_id.value, static_cast<std::uint64_t>(instance.mesh),
        instance.environment_seed, instance.environment_variant,
        instance.vfx_owner_id, instance.revisions.content};
}

} // namespace hs
