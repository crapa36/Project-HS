#pragma once

#include <hs/presentation/process_scene.hpp>

#include <memory>
#include <string>
#include <utility>

namespace hs::runtime_detail
{
class ProcessSceneFactory
{
  public:
    explicit ProcessSceneFactory(std::string generation) : generation_(std::move(generation)) {}
    [[nodiscard]] std::shared_ptr<const StaticRenderScene> Resolve(const TickReadView &model)
    {
        if (!owner_ || !owner_->Matches(model, generation_))
            owner_ = BuildProcessSceneAtLaunch(model, generation_);
        return ProcessSceneView(owner_);
    }
    [[nodiscard]] std::shared_ptr<const StaticRenderScene> Resolve(const GameReadModel &model)
    { return Resolve(development::BorrowOwnedReadModel(model)); }

  private:
    std::string generation_;
    std::shared_ptr<const ProcessSceneLayout> owner_;
};
} // namespace hs::runtime_detail
