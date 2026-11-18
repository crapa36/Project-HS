#pragma once

#include <hs/game_rules/simulation_rules.hpp>
#include <hs/render_data/static_shadow_archive.hpp>

#include <cstdint>
#include <memory>
#include <string_view>

#if defined(HS_OFFLINE_PROBE_BAKER)
#include <hs/asset_manifest/selected_generation.hpp>
#endif

namespace hs::app
{
#if defined(HS_OFFLINE_PROBE_BAKER)
std::shared_ptr<const StaticRenderScene> WorldTransportScene(
    const SelectedGeneration &generation, std::uint64_t seed);
#endif
std::shared_ptr<const StaticRenderScene> WorldTransportScene(
    const SimulationRules &rules, std::string_view content_id, std::uint64_t seed);
}
