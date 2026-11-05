#pragma once

#include <hs/render_data/render_snapshot_values.hpp>

#include <vector>
#include <memory>

namespace hs
{
// Immutable raster input shared by presentation packets and renderer bank caches.
struct UiDrawContent
{
    std::vector<UiModel> models;
};

[[nodiscard]] inline const std::shared_ptr<const UiDrawContent> &EmptyUiDrawContent()
{
    static const auto empty = std::make_shared<const UiDrawContent>();
    return empty;
}
}
