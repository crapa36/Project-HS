#pragma once

#include "renderer_quality.hpp"
#include <array>
#include <span>
#include <utility>
#include <vector>

namespace hs
{
inline constexpr std::size_t kInstanceArcherBucket = 0;
inline constexpr std::size_t kInstanceMonsterBucketBegin = 1;
inline constexpr std::size_t kInstanceProjectileBucket = 7;
inline constexpr std::size_t kInstanceEnvironmentBucketBegin = 8;
inline constexpr std::size_t kInstanceTerrainBucket = 24 + kGroundDetailVariantCount;
inline constexpr std::size_t kInstanceOtherBucket = kInstanceTerrainBucket + 1;
inline constexpr std::size_t kInstanceBucketCount = kInstanceOtherBucket + 1;

// Only draw references are reordered. Canonical indices also identify anonymous
// instances and foliage, so retain their original values and order within buckets.
[[nodiscard]] inline auto BuildRenderInstanceOrder(
    std::span<const RenderInstance> sources, float foliage_density,
    std::vector<std::uint32_t> &source_indices)
{
    const auto bucket_for = [](const RenderInstance &source) -> std::size_t {
        if (source.mesh == RenderMesh::Archer) return kInstanceArcherBucket;
        if (source.mesh >= RenderMesh::MonsterMelee && source.mesh <= RenderMesh::BossFinal)
            return kInstanceMonsterBucketBegin + static_cast<std::size_t>(source.mesh) -
                   static_cast<std::size_t>(RenderMesh::MonsterMelee);
        if (source.mesh == RenderMesh::EnemyProjectile) return kInstanceProjectileBucket;
        if (source.mesh == RenderMesh::MeadowSurface)
            return kInstanceEnvironmentBucketBegin + 14 + kGroundDetailVariantCount + source.environment_variant % 2;
        if (source.mesh == RenderMesh::GroundDetail)
            return kInstanceEnvironmentBucketBegin + 14 + source.environment_variant % kGroundDetailVariantCount;
        if (source.mesh >= RenderMesh::TreeTrunk && source.mesh <= RenderMesh::Grass)
        {
            constexpr std::array<std::size_t, 4> offsets{0, 3, 6, 10}, counts{3, 3, 4, 4};
            const auto type = static_cast<std::size_t>(source.mesh) -
                              static_cast<std::size_t>(RenderMesh::TreeTrunk);
            return kInstanceEnvironmentBucketBegin + offsets[type] +
                   source.environment_variant % counts[type];
        }
        return source.mesh == RenderMesh::Ground || source.mesh == RenderMesh::DirtPatch
            ? kInstanceTerrainBucket : kInstanceOtherBucket;
    };
    std::array<std::pair<std::size_t, std::size_t>, kInstanceBucketCount> ranges{};
    for (std::size_t index = 0; index < sources.size(); ++index)
        if (RetainDecorativeFoliage(sources[index], index, foliage_density))
            ++ranges[bucket_for(sources[index])].second;
    std::array<std::size_t, kInstanceBucketCount> cursors{};
    std::size_t count{};
    for (std::size_t bucket = 0; bucket < ranges.size(); ++bucket)
    {
        ranges[bucket].first = cursors[bucket] = count;
        count += ranges[bucket].second;
    }
    source_indices.resize(count);
    for (std::size_t index = 0; index < sources.size(); ++index)
        if (RetainDecorativeFoliage(sources[index], index, foliage_density))
            source_indices[cursors[bucket_for(sources[index])]++] =
                static_cast<std::uint32_t>(index);
    return ranges;
}
} // namespace hs
