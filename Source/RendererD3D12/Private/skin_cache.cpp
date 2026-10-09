#include "skin_cache.hpp"

#include <algorithm>
#include <cmath>

namespace hs
{

std::size_t SkinCache::KeyHash::operator()(RenderSceneInstanceKey key) const noexcept
{
    auto value = key.stable_id;
    value ^= static_cast<std::uint64_t>(key.fallback_ordinal) +
             0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u);
    return static_cast<std::size_t>(value ^ (value >> 32u));
}

void SkinCache::BeginFrame(bool reset_history)
{
    Abort();
    preparing_ = true;
    reset_history_ = reset_history;
    active_count_ = 0;
    history_valid_count_ = 0;
}

SkinCacheSample SkinCache::Resolve(RenderSceneInstanceKey key,
                                   const RenderInstance &instance,
                                   const AnimationPoseRef *pose,
                                   float undeformed_radius)
{
    if (key.stable_id != 0) key.fallback_ordinal = 0;
    const auto pose_revision = instance.animation_clip_id.value != 0
        ? DeriveNamedAnimationPoseRevision(instance)
        : pose ? (pose->revision != 0 ? pose->revision : DeriveAnimationPoseRevision(*pose))
               : instance.revisions.pose;
    const auto clip = pose ? pose->clip : CharacterAnimationClip::Idle;
    const auto time = pose ? pose->normalized_time : 0.0f;

    SkinCacheSample sample;
    sample.current_pose_revision = pose_revision;
    sample.current_clip = clip;
    sample.current_time = time;
    sample.previous_pose_revision = pose_revision;
    sample.previous_clip = clip;
    sample.previous_time = time;
    const auto scale = std::max({std::abs(instance.scale.x), std::abs(instance.scale.y),
                                 std::abs(instance.scale.z)});
    sample.conservative_radius = std::max(undeformed_radius * scale, 0.001f);

    const auto found = reset_history_ ? entries_.end() : entries_.find(key);
    if (found != entries_.end())
    {
        const auto &entry = found->second;
        sample.previous_pose_revision = entry.pose_revision;
        sample.previous_clip = entry.clip;
        sample.previous_time = entry.time;
        sample.history_valid =
            key.stable_id != 0 && entry.content_revision == instance.revisions.content;
    }
    const auto [entry, inserted] = pending_.insert_or_assign(
        key, Entry{pose_revision, instance.revisions.content, clip, time});
    static_cast<void>(entry);
    if (inserted)
    {
        ++active_count_;
        if (sample.history_valid) ++history_valid_count_;
    }
    return sample;
}

void SkinCache::EndFrame()
{
    // Missing entries are removed only when this preparation is submitted.
}

void SkinCache::Commit() noexcept
{
    if (!preparing_) return;
    entries_.swap(pending_);
    pending_.clear();
    preparing_ = false;
}

void SkinCache::Abort() noexcept
{
    pending_.clear();
    preparing_ = false;
}

void SkinCache::Clear() noexcept
{
    entries_.clear();
    Abort();
    active_count_ = 0;
    history_valid_count_ = 0;
}

} // namespace hs
