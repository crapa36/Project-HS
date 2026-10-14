#pragma once

#include <hs/core/render_snapshot.hpp>

#include <cstddef>
#include <span>
#include <unordered_map>

namespace hs
{

class RenderSceneExtractor
{
  public:
    // Build prepares the next submitted scene; history changes only at Commit.
    [[nodiscard]] bool Build(std::span<const RenderInstance> instances,
                             RenderSceneDelta &delta, bool reset_history = false);
    // Adopt stages renderer history for a SceneDelta already published by
    // Presentation. It validates the same identity constraints without
    // reconstructing a second delta.
    [[nodiscard]] bool Adopt(std::span<const RenderInstance> instances,
                             bool reset_history = false);
    void Commit() noexcept;
    void Abort() noexcept;
    void Clear() noexcept;

  private:
    using KeyHash = RenderSceneInstanceKeyHash;

    std::unordered_map<RenderSceneInstanceKey, RenderInstance::Revisions, KeyHash> previous_;
    std::unordered_map<RenderSceneInstanceKey, RenderInstance::Revisions, KeyHash> pending_;
    bool pending_valid_{};
};

} // namespace hs
