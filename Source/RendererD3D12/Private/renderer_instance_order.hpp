#pragma once

#include "renderer_quality.hpp"
#include <array>
#include <cstdint>
#include <optional>
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

inline constexpr std::size_t kMonsterLodCount = 3;
inline constexpr std::size_t kMonsterFamilyCount = kInstanceProjectileBucket - kInstanceMonsterBucketBegin;

struct MonsterLodInstanceOrder
{
    std::array<std::pair<std::size_t, std::size_t>, kInstanceBucketCount> ranges{};
    // Family-major, then LOD0/1/2. The three cuts still form each legacy
    // family range, while source_indices retain their canonical identities.
    std::array<std::pair<std::size_t, std::size_t>,
        kMonsterFamilyCount * kMonsterLodCount> monster_lod_ranges{};
};

// The explicit LOD stream is indexed by canonical source index. Only monster
// draw references are stably partitioned; every other bucket is unchanged.
[[nodiscard]] inline std::optional<MonsterLodInstanceOrder> BuildRenderInstanceOrderWithMonsterLods(
    std::span<const RenderInstance> sources, std::span<const std::uint16_t> selected_lods,
    float foliage_density, std::vector<std::uint32_t> &source_indices)
{
    if (selected_lods.size() != sources.size()) return std::nullopt;
    for (std::size_t index = 0; index < sources.size(); ++index)
        if (sources[index].mesh >= RenderMesh::MonsterMelee &&
            sources[index].mesh <= RenderMesh::BossFinal &&
            selected_lods[index] >= kMonsterLodCount)
            return std::nullopt;

    MonsterLodInstanceOrder result;
    result.ranges = BuildRenderInstanceOrder(sources, foliage_density, source_indices);
    std::vector<std::uint32_t> scratch;
    for (std::size_t family = 0; family < kMonsterFamilyCount; ++family)
    {
        const auto [begin, count] = result.ranges[kInstanceMonsterBucketBegin + family];
        std::array<std::size_t, kMonsterLodCount> counts{};
        for (std::size_t index = begin; index < begin + count; ++index)
            ++counts[selected_lods[source_indices[index]]];
        std::array<std::size_t, kMonsterLodCount> cursors{};
        std::size_t next = begin;
        for (std::size_t lod = 0; lod < kMonsterLodCount; ++lod)
        {
            result.monster_lod_ranges[family * kMonsterLodCount + lod] = {next, counts[lod]};
            cursors[lod] = next - begin;
            next += counts[lod];
        }
        scratch.resize(count);
        for (std::size_t index = begin; index < begin + count; ++index)
        {
            const auto source_index = source_indices[index];
            scratch[cursors[selected_lods[source_index]]++] = source_index;
        }
        for (std::size_t index = 0; index < count; ++index)
            source_indices[begin + index] = scratch[index];
    }
    return result;
}
} // namespace hs
