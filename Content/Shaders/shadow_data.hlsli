#ifndef HS_SHADOW_DATA_HLSLI
#define HS_SHADOW_DATA_HLSLI
#include "shadow_filter.hlsli"

// The graphics root uses its existing table32 suffix. Keep the raw metadata
// offsets identical to VirtualShadowConstants; this include adds no CBV/root DWORD.
ByteAddressBuffer HsShadowMetadata : register(t0, space21);
struct HsShadowPageEntry
{
    int x, y;
    uint level, physical;
    uint epoch, flags, reserved0, reserved1;
};
struct HsShadowDrawRecord
{
    uint geometry_record, source_ref, instance_index, instance_generation;
    uint page_entry, layer, draw_group, reserved;
};
StructuredBuffer<HsShadowPageEntry> HsShadowPageTable : register(t1, space21);
Texture2D<float> HsShadowStaticBank : register(t2, space21);
Texture2D<float> HsShadowDynamicBank : register(t3, space21);
StructuredBuffer<HsShadowDrawRecord> HsShadowDrawRecords : register(t4, space21);

bool HsVirtualShadowIsDraw(uint pass) { return (pass & 0x80000000u) != 0u; }
HsShadowDrawRecord HsVirtualShadowLoadDraw(uint pass)
{
    return HsShadowDrawRecords[pass & 0x7fffffffu];
}
float4 HsShadowFloat4(uint offset) { return asfloat(HsShadowMetadata.Load4(offset)); }
int4 HsShadowWindow(uint level) { return asint(HsShadowMetadata.Load4(208u + 16u * level)); }
float4 HsShadowClipmap(uint level) { return HsShadowFloat4(160u + 16u * level); }
float3 HsShadowLightPosition(float3 world)
{
    return float3(dot(world, HsShadowFloat4(64u).xyz),
        dot(world, HsShadowFloat4(80u).xyz), dot(world, HsShadowFloat4(96u).xyz));
}
float4 HsVirtualShadowClipDistance(float3 world, uint pass)
{
    if (!HsVirtualShadowIsDraw(pass)) return 1.0;
    const HsShadowPageEntry page = HsShadowPageTable[HsVirtualShadowLoadDraw(pass).page_entry];
    const float4 clipmap = HsShadowClipmap(page.level);
    const float2 local = HsShadowLightPosition(world).xy - float2(page.x, page.y) * clipmap.x;
    const float guard = 2.0 * clipmap.y;
    return float4(local + guard, clipmap.x + guard - local);
}
float4 HsVirtualShadowProject(float3 world, uint pass)
{
    const HsShadowPageEntry page = HsShadowPageTable[HsVirtualShadowLoadDraw(pass).page_entry];
    const float4 bank = HsShadowFloat4(112u);
    const float4 depth = HsShadowFloat4(128u);
    const float3 light = HsShadowLightPosition(world);
    const float2 local = (light.xy - float2(page.x, page.y) * HsShadowClipmap(page.level).x)
        / HsShadowClipmap(page.level).y;
    const uint columns = uint(bank.z);
    // D3D viewport flips NDC Y; absolute page Y increases with light-up.
    const float2 pixel = float2(page.physical % columns, page.physical / columns) * 132.0
        + float2(2.0 + local.x, 130.0 - local.y);
    return float4(pixel / bank.xy * float2(2.0, -2.0) + float2(-1.0, 1.0),
        1.0 - (light.z - depth.x) * depth.y, 1.0);
}
void HsVirtualShadowRasterClip(float2 pixel, uint pass)
{
    if (!HsVirtualShadowIsDraw(pass)) return;
    const HsShadowPageEntry page = HsShadowPageTable[HsVirtualShadowLoadDraw(pass).page_entry];
    const uint columns = uint(HsShadowFloat4(112u).z);
    const float2 origin = float2(page.physical % columns, page.physical / columns) * 132.0;
    clip(float4(pixel - origin, origin + 132.0 - pixel));
}

bool HsVirtualShadowLookupLevel(float3 light, uint level, out float visibility)
{
    visibility = 1.0;
    const float4 map = HsShadowClipmap(level);
    const int2 key = int2(floor(light.xy / map.x));
    const int4 window = HsShadowWindow(level);
    const int2 cell = key - window.xy;
    if (any(cell < 0) || any(cell >= window.z)) return false;
    const uint index = uint(window.w + cell.y * window.z + cell.x);
    const HsShadowPageEntry page = HsShadowPageTable[index];
    if (page.x != key.x || page.y != key.y || page.level != level ||
        page.epoch != HsShadowMetadata.Load(300u) || page.physical == 0xffffffffu ||
        (page.flags & 24u) != 16u) return false; // published, no overflow
    const float4 bank = HsShadowFloat4(112u);
    const float4 depth = HsShadowFloat4(128u);
    const float receiver = 1.0 - (light.z - depth.x) * depth.y;
    if (receiver < 0.0 || receiver > 1.0) return false;
    const uint columns = uint(bank.z);
    const int2 origin = int2(page.physical % columns, page.physical / columns) * 132;
    const float2 local = (light.xy - float2(key) * map.x) / map.y;
    const int2 center = origin + int2(floor(float2(2.0 + local.x, 130.0 - local.y)));
    float sum = 0.0;
    const float receiver_bias = max(depth.z, map.y * 0.35 * depth.y);
    const uint samples = HsShadowMetadata.Load(332u);
    [loop] for (uint sample_index = 0; sample_index < samples; ++sample_index)
        {
            // Guard texels contain this page's enlarged projection. Never sample
            // a neighboring physical tile, even at floating-point boundaries.
            const int2 pixel = clamp(center + HsShadowFilterOffsets[sample_index], origin, origin + 131);
            const float static_caster = HsShadowStaticBank.Load(int3(pixel, 0));
#if defined(HS_LOCAL_PROBE_CAPTURE)
            const float caster = static_caster;
#else
            const float caster = max(static_caster, HsShadowDynamicBank.Load(int3(pixel, 0)));
#endif
            sum += receiver + receiver_bias >= caster ? 1.0 : 0.0;
        }
    visibility = sum / float(samples);
    return true;
}
bool HsVirtualShadowLookup(float3 world, uint2 stable_id, uint generation, out float visibility)
{
    visibility = 1.0;
    const uint4 identity = HsShadowMetadata.Load4(288u);
    if (HsShadowMetadata.Load(324u) == 0u || any(identity.xy != stable_id) ||
        identity.z != generation) return false;
    const float3 light = HsShadowLightPosition(world);
    [unroll] for (uint level = 0u; level < 3u; ++level)
    {
        const int4 window = HsShadowWindow(level);
        const float side = HsShadowClipmap(level).x;
        const int2 key = int2(floor(light.xy / side));
        const int2 local = key - window.xy;
        if (any(local < 0) || any(local >= window.z)) continue;
        float value;
        if (!HsVirtualShadowLookupLevel(light, level, value)) return false;
        const float2 cell = light.xy / side - float2(window.xy);
        const float edge = min(min(cell.x, cell.y), min(window.z - cell.x, window.z - cell.y));
        if (level < 2u && edge < 1.0)
        {
            float coarser;
            if (HsVirtualShadowLookupLevel(light, level + 1u, coarser))
                value = lerp(coarser, value, smoothstep(0.0, 1.0, edge));
        }
        visibility = value;
        return true;
    }
    return false;
}
bool HsVirtualShadowLookup(float3 world, out float visibility)
{
    const uint4 identity = HsShadowMetadata.Load4(288u);
    return HsVirtualShadowLookup(world, identity.xy, identity.z, visibility);
}
#include "local_shadow.hlsli"
// Shared geometry frontends retain the existing virtual route and add the
// independent local route. No draw may set both reserved pass bits.
bool HsShadowIsDraw(uint pass)
{return HsVirtualShadowIsDraw(pass)||HsLocalShadowIsDraw(pass);}
HsShadowDrawRecord HsShadowLoadDraw(uint pass)
{
    if(HsVirtualShadowIsDraw(pass))return HsVirtualShadowLoadDraw(pass);
    const HsLocalShadowDrawRecord local=HsLocalShadowLoadDraw(pass);
    HsShadowDrawRecord result;
    result.geometry_record=local.geometry_record;result.source_ref=local.source_ref;
    result.instance_index=local.instance_index;result.instance_generation=local.instance_generation;
    result.page_entry=local.page_entry;result.layer=local.layer;result.draw_group=local.draw_group;result.reserved=local.reserved;
    return result;
}
float4 HsShadowClipDistance(float3 world,uint pass)
{return HsLocalShadowIsDraw(pass)?HsLocalShadowClipDistance(world,pass):HsVirtualShadowClipDistance(world,pass);}
float4 HsShadowProject(float3 world,uint pass)
{return HsLocalShadowIsDraw(pass)?HsLocalShadowProject(world,pass):HsVirtualShadowProject(world,pass);}
void HsShadowRasterClip(float2 pixel,uint pass)
{if(HsLocalShadowIsDraw(pass))HsLocalShadowRasterClip(pixel,pass);else HsVirtualShadowRasterClip(pixel,pass);}
#endif
