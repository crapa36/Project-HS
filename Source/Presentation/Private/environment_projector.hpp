#pragma once

#include <hs/presentation/projector.hpp>

#include <memory>
#include <string_view>

namespace hs
{
[[nodiscard]] std::shared_ptr<const StaticRenderScene> BuildStaticEnvironmentScene(
    const GameReadModel &model, std::string_view content_generation_id);
}
