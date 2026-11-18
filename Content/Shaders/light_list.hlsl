#define HS_LIGHT_LIST_BUILD
#include "light_list.hlsli"

cbuffer LightListConstants : register(b0, space19)
{
#define HS_LIGHT_LIST_SHADER_TYPE_Float4 float4
#define HS_LIGHT_LIST_SHADER_TYPE_Uint4 uint4
#define HS_LIGHT_LIST_SHADER_TYPE_Matrix float4x4
#define HS_LIGHT_LIST_DECLARE_CONSTANT(kind,native,shader,offset) HS_LIGHT_LIST_SHADER_TYPE_##kind shader;
    HS_LIGHT_LIST_CONSTANT_FIELDS(HS_LIGHT_LIST_DECLARE_CONSTANT)
#undef HS_LIGHT_LIST_DECLARE_CONSTANT
#undef HS_LIGHT_LIST_SHADER_TYPE_Matrix
#undef HS_LIGHT_LIST_SHADER_TYPE_Uint4
#undef HS_LIGHT_LIST_SHADER_TYPE_Float4
};
RWByteAddressBuffer LightGrid : register(u0, space19);
// Per-dispatch light/camera data is invariant across the group's 64 cells.
groupshared float4 HsLightViewBounds[HsLightMaskWords * 32u];

void HsClusterBounds(uint3 cell, out float3 lower, out float3 upper)
{
    const float slice_near = CameraOriginNear.w * exp2(ExtentFarLog.w * float(cell.z) / float(Grid.z));
    const float slice_far = cell.z + 1u == Grid.z ? ExtentFarLog.z :
        CameraOriginNear.w * exp2(ExtentFarLog.w * float(cell.z + 1u) / float(Grid.z));
    const float2 pixel_lower = float2(cell.xy * HS_LIGHT_LIST_TILE_PIXELS);
    const float2 pixel_upper = min(pixel_lower + float(HS_LIGHT_LIST_TILE_PIXELS), ExtentFarLog.xy);
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
        const float bound=light.position_range.w;
        HsLightViewBounds[index] = float4(center, bound * bound);
    }
    GroupMemoryBarrierWithGroupSync();
    const uint cell_index = dispatch.x + dispatch.y * (65535u * 64u);
    if (cell_index >= Limits.w) return;
    const uint3 cell = uint3(cell_index % Grid.x, (cell_index / Grid.x) % Grid.y, cell_index / (Grid.x * Grid.y));
    float3 lower, upper;
    HsClusterBounds(cell, lower, upper);
    uint4 membership = 0u;
    const bool world_query = !all(isfinite(lower)) || !all(isfinite(upper));
    [loop] for (uint index = Grid.w; index < Limits.x; ++index)
    {
        const float4 bounds = HsLightViewBounds[index];
        const float3 separation = max(max(lower - bounds.xyz, bounds.xyz - upper), 0.0);
        // Invalid cell bounds conservatively retain every local light.
        if (!world_query && dot(separation, separation) > bounds.w) continue;
        membership[index / 32u] |= 1u << (index % 32u);
    }
    LightGrid.Store4(HsLightHeaderBytes + cell_index * HS_LIGHT_LIST_MASK_BYTES, membership);
}
