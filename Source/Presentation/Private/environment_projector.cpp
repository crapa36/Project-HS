#include <hs/presentation/process_scene.hpp>

#include <algorithm>
#include <bit>
#include <stdexcept>
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

ProcessSceneLayout::ProcessSceneLayout(const TickReadView &model, std::string_view generation,
                                       EnvironmentLayout &&prepared)
    : launch_seed(model.seed), exported_view(data_.exported_view), arena_half_extent_(model.arena_half_extent),
      boundary_(model.arena_boundary), obstacles_(model.arena_obstacles.begin(), model.arena_obstacles.end())
{
    data_.scene_asset_id = MakeAssetId("render/hlod/arena.shl1");
    data_.content_generation_id = generation;
    data_.layout_abi = prepared.abi;
    data_.eligible_layout_digest = prepared.eligible_digest;
    data_.instances = std::move(prepared.instances);
    data_.eligible_static_members.reserve(prepared.eligible_static_members.size());
    for (const auto &member : prepared.eligible_static_members)
        data_.eligible_static_members.push_back({member.stable_id, member.object_id, member.instance_index});
    data_.static_keys.reserve(data_.instances.size());
    for (auto &instance : data_.instances)
    {
        PopulateRenderInstanceRevisions(instance);
        if (!instance.stable_id) throw std::runtime_error("Process scene contains an anonymous static instance.");
        data_.static_keys.push_back(MakeRenderSceneInstanceKey(instance.stable_id, 0));
    }
    std::ranges::sort(data_.static_keys, {}, &RenderSceneInstanceKey::stable_id);
    if (std::adjacent_find(data_.static_keys.begin(), data_.static_keys.end()) != data_.static_keys.end())
        throw std::runtime_error("Process scene contains duplicate static identities.");
    for (const auto &member : data_.eligible_static_members)
        if (member.instance_index >= data_.instances.size() ||
            data_.instances[member.instance_index].stable_id != member.stable_id)
            throw std::runtime_error("Process scene eligible member does not identify its actual instance.");
    data_.BindExportedView();
}

bool ProcessSceneLayout::Matches(const TickReadView &model, std::string_view generation) const noexcept
{
    if (data_.content_generation_id != generation || data_.layout_abi != kEnvironmentLayoutAbi || launch_seed != model.seed ||
        !SameBits(arena_half_extent_, model.arena_half_extent) ||
        boundary_.count != model.arena_boundary.count || obstacles_.size() != model.arena_obstacles.size())
        return false;
    for (std::size_t index = 0; index < boundary_.points.size(); ++index)
        if (!SamePoint(boundary_.points[index], model.arena_boundary.points[index])) return false;
    for (std::size_t index = 0; index < obstacles_.size(); ++index)
    {
        const auto &left = obstacles_[index];
        const auto &right = model.arena_obstacles[index];
        if (left.kind != right.kind || !SamePoint(left.center, right.center) ||
            !SameBits(left.radius, right.radius)) return false;
    }
    return true;
}

std::shared_ptr<const ProcessSceneLayout> BuildProcessSceneAtLaunch(
    const TickReadView &model, std::string_view content_generation)
{
    auto prepared = BuildEnvironmentLayout(model.arena_half_extent, model.arena_boundary,
                                            model.arena_obstacles, model.seed);
    std::unique_ptr<ProcessSceneLayout> candidate(new ProcessSceneLayout(model, content_generation, std::move(prepared)));
    return std::shared_ptr<const ProcessSceneLayout>(std::move(candidate));
}
} // namespace hs
