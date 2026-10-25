#pragma once

#include "renderer_scene_sample.hpp"

#include <hs/core/render_snapshot.hpp>

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace hs
{

struct SkinCacheSample
{
    RenderPoseKey current_pose{};
    RenderPoseKey previous_pose{};
    CharacterAnimationClip current_clip{CharacterAnimationClip::Idle};
    CharacterAnimationClip previous_clip{CharacterAnimationClip::Idle};
    float current_time{};
    float previous_time{};
    float conservative_radius{};
    bool history_valid{};
};

class SkinCache
{
  public:
    // Resolve always compares against the last committed deformation history.
    void BeginFrame(bool reset_history = false);
    [[nodiscard]] SkinCacheSample Resolve(
        RenderSceneInstanceKey key, const RenderInstance &instance,
        const AnimationPoseRef *pose, float undeformed_radius);
    void EndFrame();
    void Commit() noexcept;
    void Abort() noexcept;
    void Clear() noexcept;
    [[nodiscard]] std::size_t ActiveCount() const noexcept { return active_count_; }
    [[nodiscard]] std::size_t HistoryValidCount() const noexcept
    {
        return history_valid_count_;
    }

  private:
    using KeyHash = RenderSceneInstanceKeyHash;
    struct Entry
    {
        RenderPoseKey pose{};
        RenderContentKey content{};
        CharacterAnimationClip clip{CharacterAnimationClip::Idle};
        float time{};
    };

    std::unordered_map<RenderSceneInstanceKey, Entry, KeyHash> entries_;
    std::unordered_map<RenderSceneInstanceKey, Entry, KeyHash> pending_;
    bool preparing_{};
    bool reset_history_{};
    std::size_t active_count_{};
    std::size_t history_valid_count_{};
};

} // namespace hs
