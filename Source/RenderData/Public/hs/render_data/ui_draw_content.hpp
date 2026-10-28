#pragma once

#include <hs/render_data/render_snapshot.hpp>

#include <vector>

namespace hs
{
// Immutable raster input shared by presentation packets and renderer bank caches.
struct UiDrawContent
{
    std::vector<UiModel> models;
};
}
