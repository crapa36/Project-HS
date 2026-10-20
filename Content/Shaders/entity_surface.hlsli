#ifndef HS_ENTITY_SURFACE_HLSLI
#define HS_ENTITY_SURFACE_HLSLI

// The selected authored 64-cubed noise product shares its existing resident
// allocation. One suffix descriptor leaves the scene/root/sidecar ABI intact.
Texture3D<float4> EntitySurfaceNoise : register(t0, space35);

float4 HsEntitySurface(uint mesh, uint2 key, float3 world)
{
    if (mesh < 13u || mesh > 15u || key.x == 0u) return 0.0;
    uint4 h0, h1, h2, h3;
    const uint slot = key.x - 1u;
    if (!HsGpuSceneSlotValid(slot, h0, h1, h2, h3)) return 0.0;
    const uint address = h1.x + slot * h2.z;
    const uint4 instance0 = HsGpuSceneTables.Load4(address);
    const uint4 instance1 = HsGpuSceneTables.Load4(address + 16u);
    if (instance1.y != key.y || instance1.w == 0u ||
        HsGpuSceneTables.Load(h1.z + instance0.y * h3.x) != mesh) return 0.0;
    // A root SRV supplies an address, not a descriptor byte range.
    const uint scene_bytes = SceneTableBounds.x;
    const uint tail = h2.y + h0.z * h3.w;
    if (tail > scene_bytes || instance1.w - 1u > scene_bytes - tail) return 0.0;
    const uint group = tail + instance1.w - 1u;
    if (scene_bytes - group < 16u) return 0.0;
    const uint4 header = HsGpuSceneTables.Load4(group);
    if (header.x != 0x31534645u || header.z < 16u || header.z > scene_bytes - group ||
        header.y != (header.z - 16u) / 48u || (header.z - 16u) % 48u != 0u) return 0.0;

    const uint transform = h1.y + instance0.x * h2.w;
    const float4 position_yaw = asfloat(HsGpuSceneTables.Load4(transform));
    const float3 scale = asfloat(HsGpuSceneTables.Load4(transform + 16u)).xyz;
    const float radius = asfloat(HsGpuSceneTables.Load4(h2.x + instance0.w * h3.z)).w;
    if (any(scale == 0.0) || radius <= 0.0) return 0.0;
    const float3 offset = world - position_yaw.xyz;
    float sine_yaw, cosine_yaw;
    sincos(position_yaw.w, sine_yaw, cosine_yaw);
    const float3 local = float3(offset.x * cosine_yaw - offset.z * sine_yaw,
        offset.y, offset.x * sine_yaw + offset.z * cosine_yaw) / scale;
    uint noise_width, noise_height, noise_depth, gradient_width, gradient_rows;
    uint stbn_width, stbn_height, stbn_slices;
    EntitySurfaceNoise.GetDimensions(noise_width, noise_height, noise_depth);
    VfxGradientLut.GetDimensions(gradient_width, gradient_rows);
    VfxStbnScalar.GetDimensions(stbn_width, stbn_height, stbn_slices);
    float4 result = 0.0;
    for (uint i = 0u; i < header.y; ++i)
    {
        const uint command = group + 16u + i * 48u;
        const float4 color_alpha = asfloat(HsGpuSceneTables.Load4(command));
        const float4 shape = asfloat(HsGpuSceneTables.Load4(command + 16u));
        const uint4 identity = HsGpuSceneTables.Load4(command + 32u);
        const uint seed = identity.y;
        const float3 seed_offset = float3(seed, seed * 1664525u + 1013904223u,
            seed * 22695477u + 1u) / 4294967295.0;
        const float3 noise_uv = local * shape.y + seed_offset;
        const float noise = EntitySurfaceNoise.SampleLevel(MaterialSampler, noise_uv, 0).r;
        const uint2 noise_pixel = uint2(abs(noise_uv.xz) * float2(stbn_width, stbn_height));
        const float stbn = VfxStbnScalar.Load(int4(noise_pixel % uint2(stbn_width, stbn_height),
            seed % stbn_slices, 0));
        float directional = 0.5 + offset.y / (2.0 * radius);
        if (identity.z == 1u) directional = max(directional, length(offset.xz) / radius);
        const float distance = directional + noise - 0.5 - shape.x + (stbn - 0.5) / noise_width;
        const float coverage = saturate(1.0 - abs(distance) / shape.z);
        const float2 gradient_uv = float2((saturate(shape.w) * (gradient_width - 1u) + 0.5) / gradient_width,
            (identity.x + 0.5) / gradient_rows);
        const float4 gradient = VfxGradientLut.SampleLevel(LinearClamp, gradient_uv, 0);
        const float alpha = saturate(color_alpha.a * gradient.a * coverage);
        result.rgb += color_alpha.rgb * gradient.rgb * alpha;
        result.a = max(result.a, alpha);
    }
    return result;
}
#endif
