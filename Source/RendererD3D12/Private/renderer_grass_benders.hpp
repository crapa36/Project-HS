#pragma once

#include <hs/core/render_snapshot.hpp>
#include <algorithm>
#include <array>
#include <span>
#include <vector>

namespace hs
{
struct GrassBenderSelection
{
    std::array<const RenderInstance *, 32> instances{};
    std::uint32_t count{};
};

[[nodiscard]] inline GrassBenderSelection SelectGrassBenders(std::span<const RenderInstance> instances)
{
    GrassBenderSelection result;
    const auto player = std::ranges::find(instances, RenderMesh::Archer, &RenderInstance::mesh);
    if (player == instances.end()) return result;
    result.instances[result.count++] = &*player;
    struct Candidate { float distance; const RenderInstance *instance; };
    std::vector<Candidate> candidates;
    for (const auto &source : instances)
        if (source.mesh >= RenderMesh::MonsterMelee && source.mesh <= RenderMesh::BossFinal)
        {
            const auto dx = source.position.x - player->position.x;
            const auto dz = source.position.z - player->position.z;
            candidates.push_back({dx * dx + dz * dz, &source});
        }
    const auto count = std::min(candidates.size(), result.instances.size() - 1);
    std::partial_sort(candidates.begin(), candidates.begin() + count, candidates.end(),
        [](const auto &a, const auto &b) {
            if (a.distance != b.distance) return a.distance < b.distance;
            if (a.instance->stable_id != b.instance->stable_id)
                return a.instance->stable_id < b.instance->stable_id;
            return a.instance < b.instance;
        });
    for (std::size_t index = 0; index < count; ++index)
        result.instances[result.count++] = candidates[index].instance;
    return result;
}
} // namespace hs
