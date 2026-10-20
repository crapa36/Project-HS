#define HS_LIGHT_LIST_BUILD
#include "light_list.hlsli"

cbuffer LightListConstants : register(b0, space19)
{
    float4x4 WorldToView;
    float4x4 Projection;
    float4x4 InverseProjection;
    float4 CameraOriginNear;
    float4 ExtentFarLog;
    uint4 Grid;
    uint4 Limits;
};
RWByteAddressBuffer LightRanges : register(u0, space19);
RWStructuredBuffer<uint> LightInline : register(u1, space19);
RWStructuredBuffer<uint> LightSpill : register(u2, space19);
// Per-dispatch light/camera data is invariant across the group's 64 cells.
groupshared float4 HsLightViewBounds[HsLightMaskWords * 32u];

void HsClusterBounds(uint3 cell, out float3 lower, out float3 upper)
{
    const float slice_near = CameraOriginNear.w * exp2(ExtentFarLog.w * float(cell.z) / float(Grid.z));
    const float slice_far = cell.z + 1u == Grid.z ? ExtentFarLog.z :
        CameraOriginNear.w * exp2(ExtentFarLog.w * float(cell.z + 1u) / float(Grid.z));
    const float2 pixel_lower = float2(cell.xy * 16u);
    const float2 pixel_upper = min(pixel_lower + 16.0, ExtentFarLog.xy);
    lower = 3.402823466e+38; upper = -3.402823466e+38;
    [unroll] for (uint corner = 0u; corner < 4u; ++corner)
    {
        const float2 pixel = float2((corner & 1u) != 0u ? pixel_upper.x : pixel_lower.x,
            (corner & 2u) != 0u ? pixel_upper.y : pixel_lower.y);
        const float2 ndc = pixel / ExtentFarLog.xy * float2(2.0, -2.0) + float2(-1.0, 1.0);
        const float4 unprojected = mul(float4(ndc, 1.0, 1.0), InverseProjection);
        const float3 ray = unprojected.xyz / unprojected.w;
        const float3 near_corner = ray * (slice_near / ray.z), far_corner = ray * (slice_far / ray.z);
        lower = min(lower, min(near_corner, far_corner)); upper = max(upper, max(near_corner, far_corner));
    }
    // Outward error bound covers float unprojection, logarithmic slicing and
    // lookup at an adjacent pixel/slice boundary. It enlarges candidate sets.
    const float3 rounding_extent = max(abs(lower), abs(upper)) * 0.00002 + 0.00001;
    lower -= rounding_extent; upper += rounding_extent;
}

[numthreads(64, 1, 1)]
void BuildLightLists(uint3 dispatch : SV_DispatchThreadID, uint lane : SV_GroupIndex)
{
    [loop] for (uint index = Grid.w + lane; index < Limits.x; index += 64u)
    {
        const HsGpuLightRecord light = HsLightRecords[index];
        const float3 center = mul(float4(light.position_range.xyz - CameraOriginNear.xyz, 1.0), WorldToView).xyz;
        const float bound=light.position_range.w+(light.identity.w>=3u?light.shape_cone.y:0.0);
        HsLightViewBounds[index] = float4(center, bound * bound);
    }
    GroupMemoryBarrierWithGroupSync();
    const uint cell_index = dispatch.x + dispatch.y * (65535u * 64u);
    if (cell_index >= Limits.w) return;
    const uint3 cell = uint3(cell_index % Grid.x, (cell_index / Grid.x) % Grid.y, cell_index / (Grid.x * Grid.y));
    float3 lower, upper;
    HsClusterBounds(cell, lower, upper);
    uint inline_count = 0u, spill_count = 0u, total_count = 0u;
    uint4 membership = 0u;
    const bool world_query = !all(isfinite(lower)) || !all(isfinite(upper));
    bool capacity_overflow = false;
    [loop] for (uint index = Grid.w; index < Limits.x; ++index)
    {
        const float4 bounds = HsLightViewBounds[index];
        const float3 separation = max(max(lower - bounds.xyz, bounds.xyz - upper), 0.0);
        // A Spot's range sphere encloses its cone; area bounds include emitter extent.
        if (dot(separation, separation) > bounds.w) continue;
        if (inline_count < Limits.y) ++inline_count;
        else if (spill_count < Limits.z) ++spill_count;
        else capacity_overflow = true;
        membership[index / 32u] |= 1u << (index % 32u);
        ++total_count;
    }
    [unroll] for (uint word = 0u; word < HsLightMaskWords; ++word)
        LightInline[cell_index * HsLightMaskWords + word] = membership[word];
    if (cell_index == 0u) LightSpill[0] = 0u; // Retain the existing four-resource binding contract.
    LightRanges.Store4(HsLightHeaderBytes + cell_index * 16u,
        uint4(inline_count, spill_count, (world_query ? HsLightWorldQueryFlag : 0u) |
            (capacity_overflow ? HsLightCapacityOverflowFlag : 0u), total_count));
}
