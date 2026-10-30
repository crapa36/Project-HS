// MaterialIR decal domain, binding ABI1. The root-table suffix uses space36;
// it shares the renderer root signature without adding root parameters or MRTs.
#include "surface_metadata.hlsli"
#include "material_surface.hlsli"

cbuffer DecalConstants : register(b0, space36)
{
    float4x4 DecalInverseViewProjection;
    float4 DecalViewOrigin;
    float4 DecalRenderSize;
    uint4 DecalInfo; // command count, parameter slot count, gradient width/rows
    uint4 DecalReceivers[2]; // model mask, required flags, forbidden flags, normal filter
    uint4 DecalParameterBases;
};
Texture2D<float> DecalDepth : register(t0, space36);
Texture2D<uint> DecalMetadata : register(t1, space36);
Texture2D<float4> DecalBase : register(t2, space36);
Texture2D<float2> DecalNormal : register(t3, space36);
Texture2D<float4> DecalMaterial : register(t4, space36);
ByteAddressBuffer DecalCommands : register(t5, space36);
Texture2D<float4> DecalGradient : register(t6, space36);
StructuredBuffer<uint4> MaterialParameters : register(t7, space36);
// Generated texture declarations occupy t8/t9, one per authored decal graph.
RWTexture2D<float4> DecalOutBase : register(u0, space36);
RWTexture2D<float2> DecalOutNormal : register(u1, space36);
RWTexture2D<float4> DecalOutMaterial : register(u2, space36);
RWTexture2D<float4> DecalOutEmission : register(u3, space36);
SamplerState LinearClamp : register(s0);

// Decal-only context fields leave the common geometry context ABI unchanged.
struct HsDecalContext
{
    float2 uv, d_uv_dx, d_uv_dy;
    uint layer, seed, parameter_base;
    float3 sampled_base_color, normal;
    float sampled_roughness;
    float4 decal_tint_hdr, decal_projector;
    float3 decal_shape;
};

float2 HsDecalSignNotZero(float2 v)
{ return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
float3 HsDecalDecodeNormal(float2 p)
{
    float3 n = float3(p, 1.0 - abs(p.x) - abs(p.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * HsDecalSignNotZero(n.xy);
    return normalize(n);
}
float2 HsDecalEncodeNormal(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * HsDecalSignNotZero(n.xy);
}
float3 HsDecalWorld(uint2 pixel, float depth)
{
    const float2 uv = (float2(pixel) + 0.5) * DecalRenderSize.zw;
    const float4 h = mul(float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0),
        DecalInverseViewProjection);
    return h.xyz / h.w + DecalViewOrigin.xyz;
}

// Authored fracture math moved from the old lighting-local ground hook. Both
// functions operate on explicit typed graph operands and return one Surface.
float2 HsDecalCellRandom(float2 cell, uint seed)
{
    const float2 value = float2(dot(cell, float2(127.1, 311.7)), dot(cell, float2(269.5, 183.3)));
    return frac(sin(value + float(seed % 4093u)) * 43758.5453);
}
float HsDecalVoronoiBranch(float2 local, float cells, uint seed, float footprint)
{
    const float frequency = sqrt(max(cells, 1.0) / 3.14159265);
    const float2 p = local * frequency, cell = floor(p);
    float first = 1000.0, second = 1000.0;
    float2 nearest = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        const float2 id = cell + float2(x, y);
        const float2 delta = id + HsDecalCellRandom(id, seed) - p;
        const float distance = dot(delta, delta);
        if (distance < first) { second = first; first = distance; nearest = id; }
        else second = min(second, distance);
    }
    const float edge = sqrt(second) - sqrt(first);
    const float aa = clamp(footprint * frequency, 0.002, 0.15);
    const float selection = HsDecalCellRandom(nearest, seed + 71u).x;
    const float density = lerp(0.85, 0.22, saturate(length(local)));
    return (1.0 - smoothstep(0.035 - aa, 0.035 + aa, edge)) * step(selection, density);
}
HsSurfaceClosure HsDecalFracture(Texture2DArray<float4> source_texture, SamplerState source_sampler,
    float2 uv, uint layer, float2 dx, float2 dy, float3 base, float3 normal, float roughness,
    float4 tint_hdr, float4 projector, float3 shape, uint seed, float target_roughness, float roughness_blend)
{
    const float3 sample_uv = float3(uv, layer);
    const float4 structure = source_texture.SampleGrad(source_sampler, sample_uv, dx, dy);
    const float aa = max(length(dx), length(dy)) * 2.0;
    const float branch = HsDecalVoronoiBranch(uv * 2.0 - 1.0, shape.z, seed, aa);
    const float cracks = saturate(max(structure.g, branch * structure.a * 0.55));
    const float soot = saturate(structure.r * structure.a);
    const float coverage = projector.w * max(soot, cracks);
    const float3 color = lerp(base, base * lerp(0.3.xxx, saturate(tint_hdr.rgb) * 0.45, 0.25), coverage);
    uint width, height, layers; source_texture.GetDimensions(width, height, layers);
    const float height_x = source_texture.SampleGrad(source_sampler, sample_uv + float3(1.0 / width, 0, 0), dx, dy).b;
    const float height_z = source_texture.SampleGrad(source_sampler, sample_uv + float3(0, 1.0 / height, 0), dx, dy).b;
    const float2 slope = clamp(float2(height_x - structure.b, height_z - structure.b) *
        (0.003 * float2(width, height) / (2.0 * projector.z)), -0.15, 0.15) * coverage;
    const float2 across = float2(-projector.y, projector.x);
    const float2 tangent = projector.xy * slope.x + across * slope.y;
    HsSurfaceClosure result = HsMaterialMakeSurfaceClosure(color,
        normalize(normal - float3(tangent.x, 0, tangent.y)),
        lerp(roughness, saturate(target_roughness), coverage * saturate(roughness_blend)), 0.0, 1.0, coverage);
    const float cooling = 1.0 - saturate(shape.x);
    result.emission = tint_hdr.rgb * tint_hdr.w * shape.y * cracks * projector.w * cooling * cooling;
    return result;
}
HsSurfaceClosure HsDecalOrganic(Texture2DArray<float4> source_texture, SamplerState source_sampler,
    float2 uv, uint layer, float2 dx, float2 dy, float3 base, float3 normal, float roughness,
    float4 tint_hdr, float4 projector, float3 shape, uint seed, float target_roughness, float roughness_blend)
{
    const float mask = source_texture.SampleGrad(source_sampler, float3(uv, layer), dx, dy).r;
    const float coverage = projector.w * mask;
    const float3 color = lerp(base, base * lerp(0.12.xxx, saturate(tint_hdr.rgb), 0.55), coverage);
    HsSurfaceClosure result = HsMaterialMakeSurfaceClosure(color, normal,
        lerp(roughness, saturate(target_roughness), coverage * saturate(roughness_blend)), 0.0, 1.0, coverage);
    result.emission = tint_hdr.rgb * tint_hdr.w * coverage;
    return result;
}
#include "decal_materials.generated.hlsli"

bool HsDecalReceiver(uint2 metadata, uint4 receiver)
{
    const uint model = (metadata.y & 0xfeu) >> 1u;
    return model < 32u && (receiver.x & (1u << model)) != 0u &&
        (metadata.y & receiver.y) == receiver.y && (metadata.y & receiver.z) == 0u;
}

[numthreads(8, 8, 1)]
void DecalSurfaceCS(uint3 dispatch : SV_DispatchThreadID)
{
    const uint2 pixel = dispatch.xy;
    if (any(pixel >= uint2(DecalRenderSize.xy))) return;
    float4 base = DecalBase.Load(int3(pixel, 0));
    const float2 packed_normal = DecalNormal.Load(int3(pixel, 0));
    float4 material = DecalMaterial.Load(int3(pixel, 0));
    float3 normal = HsDecalDecodeNormal(packed_normal), emission = 0.0;
    const float ground_facing = smoothstep(0.45, 0.8, normal.y);
    const float depth = DecalDepth.Load(int3(pixel, 0));
    const uint packed_metadata = DecalMetadata.Load(int3(pixel, 0));
    const uint2 metadata = HsUnpackSurfaceMetadata(packed_metadata);
    bool modified = false;
    if (depth > 0.0 && metadata.x != 0u)
    {
        const float3 world = HsDecalWorld(pixel, depth);
        // Reconstruct neighboring positions at their own depth outside material
        // and projector branches. A receiver discontinuity contributes zero
        // derivative, so sampling cannot smear a distant surface into this one.
        const uint2 px = uint2(min(pixel.x + 1u, uint(DecalRenderSize.x) - 1u), pixel.y);
        const uint2 py = uint2(pixel.x, min(pixel.y + 1u, uint(DecalRenderSize.y) - 1u));
        const float depth_x = DecalDepth.Load(int3(px, 0)), depth_y = DecalDepth.Load(int3(py, 0));
        const float2 world_dx = depth_x > 0.0 && DecalMetadata.Load(int3(px, 0)) == packed_metadata
            ? (HsDecalWorld(px, depth_x) - world).xz : 0.0;
        const float2 world_dy = depth_y > 0.0 && DecalMetadata.Load(int3(py, 0)) == packed_metadata
            ? (HsDecalWorld(py, depth_y) - world).xz : 0.0;
        [loop] for (uint index = 0u; index < DecalInfo.x; ++index)
        {
            const uint offset = index * 96u;
            const float4 position_radius = asfloat(DecalCommands.Load4(offset));
            const float4 axis_alpha_age = asfloat(DecalCommands.Load4(offset + 16u));
            const float4 color_hdr = asfloat(DecalCommands.Load4(offset + 32u));
            const float4 fracture = asfloat(DecalCommands.Load4(offset + 48u));
            const float4 slab = asfloat(DecalCommands.Load4(offset + 64u));
            const uint4 identity = DecalCommands.Load4(offset + 80u); // gradient, slice, material ordinal, seed
            if (identity.z >= 2u || !HsDecalReceiver(metadata, DecalReceivers[identity.z]) ||
                world.y < slab.x || world.y > slab.y) continue;
            const float2 axis = axis_alpha_age.xy, across = float2(-axis.y, axis.x);
            const float2 delta = (world.xz - position_radius.xz) / position_radius.w;
            const float2 local = float2(dot(delta, axis), dot(delta, across));
            const float radial = length(local);
            if (radial >= 1.0) continue;
            const float2 dx = float2(dot(world_dx, axis), dot(world_dx, across)) / (2.0 * position_radius.w);
            const float2 dy = float2(dot(world_dy, axis), dot(world_dy, across)) / (2.0 * position_radius.w);
            const float aa = max(length(dx), length(dy)) * 2.0;
            const float footprint = 1.0 - smoothstep(max(0.0, 1.0 - max(0.04, aa)), 1.0, radial);
            const float slab_alpha = smoothstep(slab.x, slab.x + 0.05, world.y) *
                (1.0 - smoothstep(slab.y - 0.05, slab.y, world.y));
            const float facing = DecalReceivers[identity.z].w == 1u ? ground_facing : 1.0;
            const float2 gradient_uv = float2((saturate(axis_alpha_age.w) * (DecalInfo.z - 1u) + 0.5) / DecalInfo.z,
                (identity.x + 0.5) / DecalInfo.w);
            const float4 gradient = DecalGradient.SampleLevel(LinearClamp, gradient_uv, 0.0);
            HsDecalContext ctx = (HsDecalContext)0;
            ctx.uv = local * 0.5 + 0.5; ctx.d_uv_dx = dx; ctx.d_uv_dy = dy;
            ctx.layer = identity.y; ctx.seed = identity.w; ctx.parameter_base = DecalParameterBases[identity.z];
            ctx.sampled_base_color = base.rgb; ctx.normal = normal; ctx.sampled_roughness = material.r;
            ctx.decal_tint_hdr = float4(color_hdr.rgb * gradient.rgb, color_hdr.w);
            ctx.decal_projector = float4(axis, position_radius.w,
                saturate(axis_alpha_age.z * gradient.a * footprint * slab_alpha * facing));
            if (ctx.decal_projector.w <= 0.0) continue;
            ctx.decal_shape = float3(axis_alpha_age.w, fracture.x, fracture.y);
            const HsSurfaceClosure result = HsDecalEvaluate(ctx, identity.z);
            if (result.opacity <= 0.0) continue;
            base.rgb = result.base_color; normal = result.normal; material.r = result.roughness;
            emission += result.emission; modified = true;
        }
    }
    DecalOutBase[pixel] = base;
    DecalOutNormal[pixel] = modified ? HsDecalEncodeNormal(normal) : packed_normal;
    DecalOutMaterial[pixel] = material;
    DecalOutEmission[pixel] = float4(emission, 0.0);
}
