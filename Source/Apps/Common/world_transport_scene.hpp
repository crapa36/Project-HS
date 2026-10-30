#pragma once

#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/render_data/static_shadow_archive.hpp>

#include <cstdint>
#include <memory>

namespace hs::app
{
std::shared_ptr<const StaticRenderScene> WorldTransportScene(
    const SelectedGeneration &generation, std::uint64_t seed);
}
