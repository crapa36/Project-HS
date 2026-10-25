#pragma once

#include <hs/core/render_snapshot.hpp>
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
    const std::uint64_t launch_seed;
    const StaticRenderScene &exported_view;
    [[nodiscard]] bool Matches(const TickReadView &, std::string_view generation) const noexcept;

  private:
    ProcessSceneLayout(const TickReadView &, std::string_view, EnvironmentLayout &&);
    friend std::shared_ptr<const ProcessSceneLayout> BuildProcessSceneAtLaunch(
        const TickReadView &, std::string_view);
    float arena_half_extent_{};
    ArenaBoundary boundary_{};
    std::vector<ArenaObstacle2D> obstacles_;
};

// Pure preparation; Runtime's keyed factory owns reuse and publication.
[[nodiscard]] std::shared_ptr<const ProcessSceneLayout> BuildProcessSceneAtLaunch(
    const TickReadView &, std::string_view content_generation);
[[nodiscard]] inline std::shared_ptr<const StaticRenderScene> ProcessSceneView(
    const std::shared_ptr<const ProcessSceneLayout> &owner) noexcept
{
    return owner ? std::shared_ptr<const StaticRenderScene>(owner, &owner->exported_view) : nullptr;
}
} // namespace hs
