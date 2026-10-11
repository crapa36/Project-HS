#ifndef HS_LIGHT_LIST_HLSLI
#define HS_LIGHT_LIST_HLSLI

// 128-byte record ABI from renderer_light_list.hpp. Authored photometric units
// are converted once on CPU using the explicit 683 lm/W white-equivalent policy.
struct HsGpuLightRecord
{
    float4 position_range;
    float4 direction_cos_outer;
    float4 color_intensity;
    float4 shape_cone;
    uint4 identity;
    uint4 metadata;
    float4 area_tangent_half_width;
    float4 area_bitangent_half_height;
};
StructuredBuffer<HsGpuLightRecord> HsLightRecords : register(t0, space19);
static const uint HsLightDirectionalMask = 1u;
static const uint HsLightLocalMask = 30u;
static const uint HsLightAllMask = 31u;
static const uint HsLightWorldQueryFlag = 1u;
static const uint HsLightHeaderBytes = 256u;
static const uint HsLightMaskWords = 4u; // The CPU rejects more than128 authoritative lights.

#ifndef HS_LIGHT_LIST_BUILD
#include "ltc_area.hlsli"
#include "local_shadow.hlsli"
#include "ray_shadow_data.hlsli"
float HsLocalShadowVisibility(HsGpuLightRecord light,float3 world,float3 normal)
{
    float visibility=1.0;
    if((light.metadata.z&1u)!=0u)
        HsLocalShadowLookup(light.identity,light.metadata.xy,world,normal,visibility);
    return visibility;
}
float HsLightShadowVisibility(HsGpuLightRecord light,float3 world,float3 normal,bool world_query)
{
    const float raster=HsLocalShadowVisibility(light,world,normal);
#if defined(HS_LOCAL_PROBE_CAPTURE)
    return raster; // Capture queries static world depth, never main-view RT visibility.
#else
    if(world_query||(light.metadata.z&1u)==0u)return raster;
    float ray,confidence;
    if(HsRayShadowLookup(light.identity,light.metadata.xy,world,normal,ray,confidence))
        return lerp(raster,ray,confidence); // Same light: one alternative/confidence blend.
    return raster;
#endif
}
// t1 starts with the immutable 256-byte LightListConstants header. Each cell
// then owns uint4(inline count, spill count, exact world-query flag, total count).
ByteAddressBuffer HsLightRanges : register(t1, space19);
StructuredBuffer<uint> HsLightInline : register(t2, space19);
StructuredBuffer<uint> HsLightSpill : register(t3, space19);

struct HsLightGrid
{
    float4x4 world_to_view;
    float4x4 projection;
    float4 camera_origin_near;
    float4 extent_far_log;
    uint4 grid;
    uint4 limits;
};
HsLightGrid HsLoadLightGrid()
{
    HsLightGrid grid;
    // CPU stores transposed DirectX matrices for column-major CBVs. A raw SRV
    // has no matrix layout, so reconstruct the same logical matrices explicitly.
    grid.world_to_view = transpose(float4x4(asfloat(HsLightRanges.Load4(0u)),
        asfloat(HsLightRanges.Load4(16u)), asfloat(HsLightRanges.Load4(32u)), asfloat(HsLightRanges.Load4(48u))));
    grid.projection = transpose(float4x4(asfloat(HsLightRanges.Load4(64u)),
        asfloat(HsLightRanges.Load4(80u)), asfloat(HsLightRanges.Load4(96u)), asfloat(HsLightRanges.Load4(112u))));
    grid.camera_origin_near = asfloat(HsLightRanges.Load4(192u));
    grid.extent_far_log = asfloat(HsLightRanges.Load4(208u));
    grid.grid = HsLightRanges.Load4(224u);
    grid.limits = HsLightRanges.Load4(240u);
    return grid;
}

struct HsLightQuery
{
    uint mask;
    uint directional_cursor;
    uint directional_count;
    uint light_count;
    uint local_cursor;
    uint4 local_mask;
    bool world_query;
};
HsLightQuery HsBeginLightQuery(float3 world, uint type_mask, bool force_world_query)
{
    const HsLightGrid grid = HsLoadLightGrid();
    HsLightQuery query = (HsLightQuery)0;
    query.mask = type_mask;
    query.directional_count = grid.grid.w;
    query.light_count = grid.limits.x;
    query.world_query = force_world_query;
    if ((type_mask & HsLightLocalMask) == 0u) return query;
    const float3 view = mul(float4(world - grid.camera_origin_near.xyz, 1.0), grid.world_to_view).xyz;
    const float4 clip = mul(float4(view, 1.0), grid.projection);
    if (!all(isfinite(view)) || !all(isfinite(clip)) || clip.w <= 0.0 ||
        view.z <= grid.camera_origin_near.w || view.z >= grid.extent_far_log.z)
        query.world_query = true;
    const float2 uv = clip.w > 0.0 ? float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5) : -1.0;
    if (any(uv < 0.0) || any(uv >= 1.0)) query.world_query = true;
    if (!query.world_query)
    {
        const uint2 tile = min((uint2)(uv * grid.extent_far_log.xy) / 16u, grid.grid.xy - 1u);
        const uint slice = min((uint)(log2(view.z / grid.camera_origin_near.w) * float(grid.grid.z) /
            grid.extent_far_log.w), grid.grid.z - 1u);
        const uint cell = (slice * grid.grid.y + tile.y) * grid.grid.x + tile.x;
        const uint4 range = HsLightRanges.Load4(HsLightHeaderBytes + cell * 16u);
        query.world_query = (range.z & HsLightWorldQueryFlag) != 0u;
        if (!query.world_query)
        {
            const uint base = cell * HsLightMaskWords;
            query.local_mask = uint4(HsLightInline[base], HsLightInline[base + 1u],
                HsLightInline[base + 2u], HsLightInline[base + 3u]);
        }
    }
    // An overflow cell ignores its partial inline/spill lists and scans every
    // authoritative local record exactly once. Off-frustum queries do the same.
    query.local_cursor = query.world_query ? query.directional_count : 0u;
    return query;
}
bool HsNextLight(inout HsLightQuery query, out uint light_index)
{
    light_index = 0u;
    [loop] while ((query.mask & HsLightDirectionalMask) != 0u && query.directional_cursor < query.directional_count)
    {
        light_index = query.directional_cursor++;
#ifdef HS_LOCAL_PROBE_CAPTURE
        if ((HsLightRecords[light_index].metadata.w & 8u) != 0u) continue;
#endif
        return true;
    }
    if ((query.mask & HsLightLocalMask) == 0u) return false;
    [loop] while (query.world_query ? query.local_cursor < query.light_count :
        query.local_cursor < HsLightMaskWords)
    {
        uint index;
        if (query.world_query) index = query.local_cursor++;
        else
        {
            const uint word = query.local_mask[query.local_cursor];
            if (word == 0u) { ++query.local_cursor; continue; }
            // Lowest word/bit first preserves the original stable record order.
            index = query.local_cursor * 32u + uint(firstbitlow(word));
            query.local_mask[query.local_cursor] = word & (word - 1u);
        }
#ifdef HS_LOCAL_PROBE_CAPTURE
        if ((HsLightRecords[index].metadata.w & 8u) != 0u) continue;
#endif
        if ((query.mask & (1u << HsLightRecords[index].identity.w)) != 0u)
        {
            light_index = index;
            return true;
        }
    }
    return false;
}

// Returns incident radiometric intensity after spatial/angular attenuation.
// The positive finite emitter radius bounds inverse-square intensity at origin;
// the zero-distance point itself has no incident direction and no surface lobe.
bool HsLightIncident(uint index, float3 world, out float3 direction, out float3 incident)
{
    const HsGpuLightRecord light = HsLightRecords[index];
    direction = 0.0; incident = 0.0;
    if (light.color_intensity.w <= 0.0) return false;
    if (light.identity.w >= 3u) return false; // Area emitters require their actual domain integral.
    if (light.identity.w == 0u)
    {
        direction = -light.direction_cos_outer.xyz;
        incident = light.color_intensity.rgb * light.color_intensity.w;
        return true;
    }
    const float3 delta = light.position_range.xyz - world;
    const float distance_squared = dot(delta, delta);
    const float range_squared = light.position_range.w * light.position_range.w;
    if (!isfinite(distance_squared) || distance_squared <= 0.0 || distance_squared >= range_squared) return false;
    direction = delta * rsqrt(distance_squared);
    const float edge = saturate(1.0 - distance_squared / range_squared);
    float attenuation = edge * edge / ((light.metadata.w & 1u) != 0u ?
        1.0 + distance_squared : max(distance_squared, light.shape_cone.x));
    if (light.identity.w == 2u)
    {
        const float cosine = dot(-direction, light.direction_cos_outer.xyz);
        const float ramp = saturate((cosine - light.direction_cos_outer.w) * light.shape_cone.z);
        attenuation *= ramp * ramp;
    }
    incident = light.color_intensity.rgb * light.color_intensity.w * attenuation;
    return attenuation > 0.0;
}

// Used only by artistic directional response controls. Direct surface energy
// is evaluated by HsLightListEvaluateComponents below, not by this accessor.
bool HsPrimaryDirectional(out float3 toward_light, out float3 incident)
{
    const uint count = HsLightRanges.Load4(224u).w;
    toward_light = float3(0.0, 1.0, 0.0); incident = 0.0;
    if (count == 0u) return false;
    uint owner = 0u;
    [loop] for (uint index = 0u; index < count; ++index)
        if ((HsLightRecords[index].metadata.w & 2u) != 0u) { owner = index; break; }
    return HsLightIncident(owner, 0.0, toward_light, incident);
}

#ifdef HS_BSDF_METALLIC_ROUGHNESS_HLSLI
void HsLightListEvaluateComponentsOptical(HsSurfaceClosure closure, float3 world, float3 camera_world,
    float primary_directional_visibility, uint type_mask, bool world_query,
    float base_fresnel_override,
    out float3 diffuse, out float3 specular)
{
    diffuse = 0.0; specular = 0.0;
    const float3 view = normalize(camera_world - world);
    closure = HsBsdfOrientThinSurface(closure, view);
    HsLightQuery query = HsBeginLightQuery(world, type_mask, world_query);
    uint index;
    [loop] while (HsNextLight(query, index))
    {
        if(HsLightRecords[index].identity.w>=3u)
        {
            float3 area_diffuse,area_specular;
            HsAreaEvaluateComponents(HsLightRecords[index],closure,world,view,base_fresnel_override,area_diffuse,area_specular);
            const float visibility=HsLightShadowVisibility(HsLightRecords[index],world,closure.normal,world_query);
            diffuse+=area_diffuse*visibility; specular+=area_specular*visibility; continue;
        }
        float3 light, radiance;
        if (!HsLightIncident(index, world, light, radiance)) continue;
        float3 light_diffuse, light_specular;
        HsBsdfEvaluateComponentsOptical(closure, view, light, base_fresnel_override, light_diffuse, light_specular);
        const float visibility = (HsLightRecords[index].metadata.w & 2u) != 0u ? primary_directional_visibility :
            HsLightShadowVisibility(HsLightRecords[index],world,closure.normal,world_query);
        const float3 incident = abs(dot(closure.normal, light)) * radiance * visibility;
        diffuse += light_diffuse * incident;
        specular += light_specular * incident;
    }
}
void HsLightListEvaluateComponents(HsSurfaceClosure closure, float3 world, float3 camera_world,
    float primary_directional_visibility, uint type_mask, bool world_query,
    out float3 diffuse, out float3 specular)
{
    HsLightListEvaluateComponentsOptical(closure, world, camera_world, primary_directional_visibility,
        type_mask, world_query, -1.0, diffuse, specular);
}
float3 HsLightListEvaluate(HsSurfaceClosure closure, float3 world, float3 camera_world,
    float primary_directional_visibility, uint type_mask, bool world_query)
{
    float3 diffuse, specular;
    HsLightListEvaluateComponents(closure, world, camera_world, primary_directional_visibility,
        type_mask, world_query, diffuse, specular);
    return diffuse + specular;
}
#endif

// Volumetric smoke keeps its authored six-way response rather than inventing a
// surface BSDF. It shares the same candidate traversal and incident-light units.
float3 HsLightListSixWay(float3 world, float3 right, float3 up, float3 forward,
    float3 positive_response, float3 negative_response, uint type_mask, bool world_query)
{
    float3 result = 0.0;
    HsLightQuery query = HsBeginLightQuery(world, type_mask, world_query);
    uint index;
    [loop] while (HsNextLight(query, index))
    {
        if(HsLightRecords[index].identity.w>=3u)
        {
            const HsGpuLightRecord area=HsLightRecords[index]; float range_weight;
            if(!HsAreaLightVisible(area,world,range_weight)) continue;
            const float visibility=HsLocalShadowVisibility(area,world,0.0);
            [loop] for(uint sample=0u;sample<HsAreaSampleCount;++sample)
            {
                float3 direction; float pdf;
                if(!HsAreaSampleDirection(area,world,float2((float(sample)+0.5)/float(HsAreaSampleCount),
                    HsAreaRadicalInverse(sample)),direction,pdf)) continue;
                const float3 local=float3(dot(direction,right),dot(direction,up),dot(direction,forward));
                const float3 positive=max(local,0.0),negative=max(-local,0.0);
                const float response=dot(positive_response,positive*positive)+dot(negative_response,negative*negative);
                result+=area.color_intensity.rgb*area.color_intensity.w*range_weight*visibility*response/(pdf*float(HsAreaSampleCount));
            }
            continue;
        }
        float3 direction, incident;
        if (!HsLightIncident(index, world, direction, incident)) continue;
        const float3 local = float3(dot(direction, right), dot(direction, up), dot(direction, forward));
        const float3 positive = max(local, 0.0), negative = max(-local, 0.0);
        const float response = dot(positive_response, positive * positive) + dot(negative_response, negative * negative);
        result += incident * response * HsLocalShadowVisibility(HsLightRecords[index],world,0.0);
    }
    return result;
}
#endif // HS_LIGHT_LIST_BUILD
#endif // HS_LIGHT_LIST_HLSLI
