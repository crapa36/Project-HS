#pragma once

#include <hs/render_data/render_snapshot_values.hpp>

#include <vector>

namespace hs
{
// Immutable raster input shared by presentation packets and renderer bank caches.
struct UiDrawContent
{
    std::vector<UiModel> models;
};
}
