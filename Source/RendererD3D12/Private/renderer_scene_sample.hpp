#pragma once

#include <hs/render_data/render_snapshot_values.hpp>
#include "render_instance_lookup.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <numbers>
#include <vector>

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

// One immutable display sample is shared by all views in the rendered package.
class RenderSceneSample
{
  public:
    template <class ClipLoops>
    void Prepare(const RenderSnapshotPair &pair, float alpha, ClipLoops clip_loops)
    {
        instances_.clear();
        poses_.clear();
        const auto &current = pair.current;
        const auto &previous = pair.previous;
        const bool between = pair.has_previous && alpha < 1.0f;
        if (!between)
        {
            instances_.assign(current.instances.begin(), current.instances.end());
            poses_.assign(current.poses.begin(), current.poses.end());
            return;
        }
        previous_.Update(previous.instances);
        current_.Update(current.instances);
        current_poses_.assign(current.instances.size(), nullptr);
        previous_poses_.assign(previous.instances.size(), nullptr);
        for (const auto &pose : current.poses)
            if (pose.instance_index < current_poses_.size()) current_poses_[pose.instance_index] = &pose;
        for (const auto &pose : previous.poses)
            if (pose.instance_index < previous_poses_.size()) previous_poses_[pose.instance_index] = &pose;
        const auto append = [&](const RenderInstance &instance, const AnimationPoseRef *pose) {
            const auto index = static_cast<std::uint32_t>(instances_.size());
            instances_.push_back(instance);
            if (pose)
            {
                poses_.push_back(*pose);
                poses_.back().instance_index = index;
            }
        };
        for (std::size_t index = 0; index < current.instances.size(); ++index)
        {
            auto instance = current.instances[index];
            const auto *old = previous_.Find(instance.stable_id);
            // A new identity is born at the current endpoint, never before it.
            if (instance.stable_id && !old) continue;
            const auto *pose = current_poses_[index];
            AnimationPoseRef evaluated_pose;
            if (old)
            {
                const auto previous_index = static_cast<std::size_t>(old - previous.instances.data());
                const auto *old_pose = previous_poses_[previous_index];
                if (MakeRenderContentKey(*old) != MakeRenderContentKey(instance) ||
                    old->motion_epoch != instance.motion_epoch)
                {
                    append(*old, old_pose);
                    continue;
                }
                instance.position = {std::lerp(old->position.x, instance.position.x, alpha),
                    std::lerp(old->position.y, instance.position.y, alpha),
                    std::lerp(old->position.z, instance.position.z, alpha)};
                instance.scale = {std::lerp(old->scale.x, instance.scale.x, alpha),
                    std::lerp(old->scale.y, instance.scale.y, alpha),
                    std::lerp(old->scale.z, instance.scale.z, alpha)};
                instance.yaw = old->yaw + std::remainder(instance.yaw - old->yaw,
                    2.0f * std::numbers::pi_v<float>) * alpha;
                instance.color_rgba = old->color_rgba;
                instance.status_visual_mask = old->status_visual_mask;
                instance.material_id = old->material_id;
                instance.revisions.material = old->revisions.material;
                if (old->animation_clip_id.value != instance.animation_clip_id.value)
                {
                    instance.animation_clip_id = old->animation_clip_id;
                    instance.animation_time_seconds = old->animation_time_seconds;
                    instance.animation_playback_rate = old->animation_playback_rate;
                }
                else if (old->animation_playback_rate == instance.animation_playback_rate &&
                    old->animation_time_seconds <= instance.animation_time_seconds)
                    instance.animation_time_seconds = std::lerp(old->animation_time_seconds,
                        instance.animation_time_seconds, static_cast<double>(alpha));
                if (pose && old_pose)
                {
                    if (pose->clip != old_pose->clip || pose->secondary_clip != old_pose->secondary_clip ||
                        pose->upper_body_clip != old_pose->upper_body_clip)
                        pose = old_pose;
                    else
                    {
                        evaluated_pose = *pose;
                        const auto time = [&](CharacterAnimationClip clip, float from, float to) {
                            const bool looping = clip_loops(instance, clip);
                            if (to < from && looping) to += 1.0f;
                            if (to < from) return to;
                            const auto value = std::lerp(from, to, alpha);
                            return looping && value >= 1.0f ? value - std::floor(value) : value;
                        };
                        if (pose->playback_rate == old_pose->playback_rate)
                            evaluated_pose.normalized_time = time(pose->clip,
                                old_pose->normalized_time, pose->normalized_time);
                        if (pose->secondary_playback_rate == old_pose->secondary_playback_rate)
                            evaluated_pose.secondary_normalized_time = time(pose->secondary_clip,
                                old_pose->secondary_normalized_time, pose->secondary_normalized_time);
                        if (pose->upper_body_playback_rate == old_pose->upper_body_playback_rate)
                            evaluated_pose.upper_body_normalized_time = time(pose->upper_body_clip,
                                old_pose->upper_body_normalized_time, pose->upper_body_normalized_time);
                        evaluated_pose.secondary_weight = std::lerp(old_pose->secondary_weight,
                            pose->secondary_weight, alpha);
                        evaluated_pose.upper_body_weight = std::lerp(old_pose->upper_body_weight,
                            pose->upper_body_weight, alpha);
                        pose = &evaluated_pose;
                    }
                }
            }
            append(instance, pose);
        }
        for (std::size_t index = 0; index < previous.instances.size(); ++index)
        {
            const auto &instance = previous.instances[index];
            if (instance.stable_id && !current_.Find(instance.stable_id))
                append(instance, previous_poses_[index]);
        }
    }

    [[nodiscard]] RenderSnapshot View(RenderSnapshot snapshot) const noexcept
    {
        snapshot.instances = instances_;
        snapshot.poses = poses_;
        return snapshot;
    }

  private:
    RenderInstanceLookup previous_, current_;
    std::vector<RenderInstance> instances_;
    std::vector<AnimationPoseRef> poses_;
    std::vector<const AnimationPoseRef *> current_poses_, previous_poses_;
};

} // namespace hs
