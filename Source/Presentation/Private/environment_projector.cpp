#include "environment_projector.hpp"
#include <hs/presentation/environment_layout.hpp>
#include <hs/core/cooked_format.hpp>

#include <bit>
#include <utility>

namespace hs
{
namespace
{
bool SameBits(float left, float right) noexcept
{
    return std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right);
}
bool SamePoint(Float2 left, Float2 right) noexcept
{
    return SameBits(left.x, right.x) && SameBits(left.y, right.y);
}
} // namespace

std::shared_ptr<const StaticRenderScene> BuildStaticEnvironmentScene(
    const GameReadModel &model, std::string_view content_generation_id)
{
    auto layout = BuildEnvironmentLayout(model.arena_half_extent, model.arena_boundary,
                                         model.arena_obstacles, model.seed);
    auto scene = std::make_shared<StaticRenderScene>();
    scene->scene_asset_id = MakeAssetId("render/hlod/arena.shl1");
    scene->content_generation_id = content_generation_id;
    scene->layout_abi = layout.abi;
    scene->eligible_layout_digest = layout.eligible_digest;
    for (const auto &member : layout.eligible_static_members)
        scene->eligible_static_members.push_back({member.stable_id, member.object_id, member.instance_index});
    scene->instances = std::move(layout.instances);
    return scene;
}

StaticEnvironmentSceneCache::StaticEnvironmentSceneCache(
    std::string content_generation_id)
    : content_generation_id_(std::move(content_generation_id))
{
}

bool StaticEnvironmentSceneCache::SameLayout(const Layout &layout,
                                              const GameReadModel &model) noexcept
{
    if (layout.session_id != model.session_id || layout.seed != model.seed ||
        !SameBits(layout.arena_half_extent, model.arena_half_extent) ||
        layout.boundary.count != model.arena_boundary.count ||
        layout.obstacles.size() != model.arena_obstacles.size())
        return false;
    for (std::size_t index = 0; index < layout.boundary.points.size(); ++index)
        if (!SamePoint(layout.boundary.points[index], model.arena_boundary.points[index]))
            return false;
    for (std::size_t index = 0; index < layout.obstacles.size(); ++index)
    {
        const auto &left = layout.obstacles[index];
        const auto &right = model.arena_obstacles[index];
        if (left.kind != right.kind || !SamePoint(left.center, right.center) ||
            !SameBits(left.radius, right.radius))
            return false;
    }
    return true;
}

std::shared_ptr<const StaticRenderScene> StaticEnvironmentSceneCache::Resolve(
    const GameReadModel &model)
{
    if (layout_ && SameLayout(*layout_, model)) return scene_;
    auto new_scene = BuildStaticEnvironmentScene(model, content_generation_id_);
    Layout new_layout{model.session_id, model.seed, model.arena_half_extent,
                      model.arena_boundary,
                      {model.arena_obstacles.begin(), model.arena_obstacles.end()}};
    layout_ = std::move(new_layout);
    scene_ = std::move(new_scene);
    return scene_;
}
} // namespace hs
