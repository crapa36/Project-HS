#include "project_hs.hlsl"
#include "geometry.hlsli"
#include "visibility_records.hlsli"

ByteAddressBuffer HsRasterRecords : register(t0, space7);
Texture2D<uint2> HsVisibility : register(t9, space7);
RWByteAddressBuffer HsClassifiedPixelsWrite : register(u9, space7);
RWByteAddressBuffer HsVariantCountsWrite : register(u10, space7);
RWByteAddressBuffer HsDispatchArgsWrite : register(u11, space7);

#include "visibility_classification.hlsli"

bool HsVisibilityPixelInBounds(uint2 pixel)
{
    return pixel.x < (uint)ScreenSize.x && pixel.y < (uint)ScreenSize.y;
}

bool HsLoadClassifiedPixel(uint2 pixel, out HsRasterRecord record,
                           out uint variant)
{
    record = (HsRasterRecord)0;
    variant = 0u;
    if (!HsVisibilityPixelInBounds(pixel))
        return false;
    const uint2 visibility = HsVisibility[pixel];
    if (!HsRasterIdValid(visibility.x) || visibility.y == HsVisibilityInvalid)
        return false;
    record = HsLoadRasterRecord(HsRasterRecords, visibility.x);
    if (!HsRasterRecordDomainValid(record))
        return false;
    const GeometryDrawRecord geometry = LoadGeometryDrawRecord(record.geometryRecordId);
    return HsClassifiedVariant(visibility, record, geometry, variant);
}

groupshared uint HsClassifyGroupCounts[HsVisibilityVariantCount];
groupshared uint HsClassifyGroupOffsets[HsVisibilityVariantCount];

[numthreads(8, 8, 1)]
void ClassifyCountCS(uint3 dispatchId : SV_DispatchThreadID,
                     uint groupIndex : SV_GroupIndex)
{
    for (uint index = groupIndex; index < HsVisibilityVariantCount; index += 64u)
        HsClassifyGroupCounts[index] = 0u;
    GroupMemoryBarrierWithGroupSync();

    HsRasterRecord record;
    uint variant;
    const bool valid = HsLoadClassifiedPixel(dispatchId.xy, record, variant);
    if (valid)
    {
        uint ignored;
        InterlockedAdd(HsClassifyGroupCounts[variant], 1u, ignored);
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint index = groupIndex; index < HsVisibilityVariantCount; index += 64u)
    {
        const uint count = HsClassifyGroupCounts[index];
        if (count != 0u)
        {
            uint ignored;
            HsVariantCountsWrite.InterlockedAdd(index * 4u, count, ignored);
        }
    }
}

[numthreads(1, 1, 1)]
void ClassifyPrefixCS(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x != 0u || dispatchId.y != 0u || dispatchId.z != 0u)
        return;
    uint offset = 0u;
    [unroll]
    for (uint variant = 0u; variant < HsVisibilityVariantCount; ++variant)
    {
        const uint count = HsVariantCountsWrite.Load(variant * 4u);
        HsVariantCountsWrite.Store((HsVisibilityOffsetBase + variant) * 4u,
                                   offset);
        HsVariantCountsWrite.Store((HsVisibilityCursorBase + variant) * 4u,
                                   0u);
        const uint groups = (count + 63u) / 64u;
        const uint groupX = min(groups, 65535u);
        const uint groupY = (groups + 65534u) / 65535u;
        HsDispatchArgsWrite.Store3(variant * 12u,
                                   uint3(groupX, groupY, 1u));
        offset += count;
    }
}

[numthreads(8, 8, 1)]
void ClassifyScatterCS(uint3 dispatchId : SV_DispatchThreadID,
                       uint groupIndex : SV_GroupIndex)
{
    for (uint index = groupIndex; index < HsVisibilityVariantCount; index += 64u)
    {
        HsClassifyGroupCounts[index] = 0u;
        HsClassifyGroupOffsets[index] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    HsRasterRecord record;
    uint variant;
    const bool valid = HsLoadClassifiedPixel(dispatchId.xy, record, variant);
    uint local = 0u;
    if (valid)
        InterlockedAdd(HsClassifyGroupCounts[variant], 1u, local);
    GroupMemoryBarrierWithGroupSync();

    for (uint index = groupIndex; index < HsVisibilityVariantCount; index += 64u)
    {
        const uint count = HsClassifyGroupCounts[index];
        if (count != 0u)
        {
            uint base;
            HsVariantCountsWrite.InterlockedAdd(
                (HsVisibilityCursorBase + index) * 4u, count, base);
            HsClassifyGroupOffsets[index] = base;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (valid)
    {
        const uint offset = HsVariantCountsWrite.Load(
            (HsVisibilityOffsetBase + variant) * 4u);
        const uint destination = offset + HsClassifyGroupOffsets[variant] + local;
        const uint capacity = (uint)ScreenSize.x * (uint)ScreenSize.y;
        if (destination < capacity)
            HsClassifiedPixelsWrite.Store(destination * 4u,
                dispatchId.x + dispatchId.y * (uint)ScreenSize.x);
    }
}
