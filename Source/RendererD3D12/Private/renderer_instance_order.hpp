#pragma once

#include "renderer_quality.hpp"
#include <hs/render_data/vfx_owned_mesh.hpp>
#include <algorithm>
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

inline void FilterRenderInstancesForOwnedVfx(std::vector<RenderInstance> &instances,
    std::span<const VfxGroundSpawnInput> ground, std::span<const VfxOwnedMeshInput> meshes,
    std::vector<std::pair<std::uint64_t, std::uint8_t>> &replacements)
{
    constexpr std::uint8_t area_body=1,area_owner=2,projectile_owner=4;
    replacements.clear();
    replacements.reserve(ground.size()+meshes.size());
    for(const auto &spawn:ground)
    {
        std::uint8_t mask{};
        if(spawn.source_visual_kind!=0xff&&spawn.geometry.shape==VfxGroundShape::Circle&&
            spawn.command.primitive==VfxPrimitive::LowFrequencyFill)mask|=area_body;
        if((spawn.source_visual_kind==0xff&&(spawn.geometry.shape==VfxGroundShape::LineBorder||
                spawn.geometry.shape==VfxGroundShape::ConeBorder))||
            spawn.geometry.shape==VfxGroundShape::RingGapsBorder||
            spawn.geometry.shape==VfxGroundShape::RingGapsPreviewBorder||
            spawn.geometry.shape==VfxGroundShape::CirclePreviewBorder)mask|=area_owner;
        if(mask)replacements.emplace_back(spawn.stable_id,mask);
    }
    for(const auto &head:meshes)replacements.emplace_back(head.owner_id,projectile_owner);
    if(replacements.empty())return;
    std::ranges::sort(replacements,{},&std::pair<std::uint64_t,std::uint8_t>::first);
    std::size_t count{};
    for(const auto entry:replacements)
        if(count&&replacements[count-1].first==entry.first)replacements[count-1].second|=entry.second;
        else replacements[count++]=entry;
    replacements.resize(count);
    const auto mask_for=[&](std::uint64_t id)->std::uint8_t {
        const auto found=std::ranges::lower_bound(replacements,id,{},&std::pair<std::uint64_t,std::uint8_t>::first);
        return found!=replacements.end()&&found->first==id?found->second:0;
    };
    std::erase_if(instances,[&](const RenderInstance &instance) {
        if(instance.mesh==RenderMesh::Area)
            return (instance.stable_id!=0&&(mask_for(instance.stable_id)&area_body)!=0)||
                (instance.vfx_owner_id!=0&&(mask_for(instance.vfx_owner_id)&area_owner)!=0);
        return (instance.mesh==RenderMesh::PlayerProjectile||instance.mesh==RenderMesh::EnemyProjectile)&&
            (mask_for(instance.stable_id)&projectile_owner)!=0;
    });
}

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
    float foliage_density, std::vector<std::uint32_t> &source_indices,
    std::vector<std::uint32_t> &scratch)
{
    if (selected_lods.size() != sources.size()) return std::nullopt;
    for (std::size_t index = 0; index < sources.size(); ++index)
        if (sources[index].mesh >= RenderMesh::MonsterMelee &&
            sources[index].mesh <= RenderMesh::BossFinal &&
            selected_lods[index] >= kMonsterLodCount)
            return std::nullopt;

    MonsterLodInstanceOrder result;
    result.ranges = BuildRenderInstanceOrder(sources, foliage_density, source_indices);
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
