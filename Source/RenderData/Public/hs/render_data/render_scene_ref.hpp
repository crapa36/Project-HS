#pragma once

#include <hs/core/types.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace hs
{

struct StaticRenderScene;

// A scene asset and content generation travel with the immutable scene lease.
// The lease may be empty while a producer is preparing the corresponding
// generation; consumers must retain the shared pointer while using its data.
struct RenderSceneRef
{
    AssetId scene_asset_id{};
    std::string content_generation_id;
    std::shared_ptr<const StaticRenderScene> lease;
};

} // namespace hs
