#pragma once

#include <hs/render_data/render_snapshot_storage.hpp>
#include <hs/game_domain/tick_read_view.hpp>
#include <hs/presentation/environment_layout.hpp>

#include <memory>
#include <string_view>

namespace hs
{
class ProcessSceneLayout final
{
    StaticRenderSceneStorage data_;
  public:
    ProcessSceneLayout(const ProcessSceneLayout &) = delete;
    ProcessSceneLayout &operator=(const ProcessSceneLayout &) = delete;
    ProcessSceneLayout(ProcessSceneLayout &&) = delete;
    ProcessSceneLayout &operator=(ProcessSceneLayout &&) = delete;
    const StaticRenderScene &exported_view;

  private:
    ProcessSceneLayout(std::string_view, EnvironmentLayout &&);
    friend std::shared_ptr<const ProcessSceneLayout> BuildProcessSceneAtLaunch(
        const TickReadView &, std::string_view);
};

// Build once at launch; the returned scene view aliases and retains this immutable owner.
[[nodiscard]] std::shared_ptr<const ProcessSceneLayout> BuildProcessSceneAtLaunch(
    const TickReadView &, std::string_view content_generation);
[[nodiscard]] inline std::shared_ptr<const StaticRenderScene> ProcessSceneView(
    const std::shared_ptr<const ProcessSceneLayout> &owner) noexcept
{
    return owner ? std::shared_ptr<const StaticRenderScene>(owner, &owner->exported_view) : nullptr;
}
} // namespace hs
