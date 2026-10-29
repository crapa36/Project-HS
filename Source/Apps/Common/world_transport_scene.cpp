#include "world_transport_scene.hpp"
#include <hs/render_data/render_snapshot_storage.hpp>

#include <hs/game_rules/simulation_rules_io.hpp>
#include <hs/presentation/environment_layout.hpp>

#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace hs::app
{
std::shared_ptr<const StaticRenderScene> WorldTransportScene(const SelectedGeneration &generation)
{
    std::filesystem::path rules_path;
    const auto required = generation.Require("game-data/simulation-rules", rules_path);
    if (!required) throw std::runtime_error(std::string(required.Message()));
    SimulationRules rules;
    const auto loaded = LoadSimulationRules(rules_path, rules);
    if (!loaded) throw std::runtime_error(std::string(loaded.Message()));
    if (rules.arena_obstacle_count > rules.arena_obstacles.size())
        throw std::runtime_error("simulation rules contain an invalid obstacle count");
    auto layout = BuildEnvironmentLayout(rules.arena_half_extent, rules.arena_boundary,
        std::span(rules.arena_obstacles).first(rules.arena_obstacle_count), 0);
    if (layout.eligible_static_members.empty())
        throw std::runtime_error("production arena has no HLOD eligible static members");
    auto scene = std::make_unique<StaticRenderSceneStorage>();
    scene->content_generation_id = generation.id;
    scene->scene_asset_id = MakeAssetId("render/hlod/arena.shl1");
    scene->layout_abi = layout.abi;
    scene->eligible_layout_digest = layout.eligible_digest;
    for (const auto &member : layout.eligible_static_members)
        scene->eligible_static_members.push_back({member.stable_id, member.object_id, member.instance_index});
    scene->instances = std::move(layout.instances);
    return FreezeStaticRenderScene(std::move(scene));
}
}
