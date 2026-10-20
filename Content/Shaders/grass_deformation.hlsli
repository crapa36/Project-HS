#ifndef HS_GRASS_DEFORMATION_HLSLI
#define HS_GRASS_DEFORMATION_HLSLI

uint HsGrassCandidateMask(float2 world, uint bender_count, float4 grid, uint4 masks[16])
{
    const uint count = min(bender_count, 32u);
    const uint all_benders = count == 32u ? 0xffffffffu : ((1u << count) - 1u);
    if (count <= 3u || grid.w < 0.5) return all_benders;
    // Match the CPU's subtract-then-scale coordinates even far from the origin.
    precise float2 cell = (world - grid.xy) * grid.z;
    // Outside the encoded grid retains the complete ordered reference loop.
    if (any(cell < 0.0) || any(cell >= 8.0)) return all_benders;
    const uint flat = (uint)cell.y * 8u + (uint)cell.x;
    return masks[flat >> 2u][flat & 3u] & all_benders;
}

struct HsGrassDeformation
{
    float3 position;
    float3 dx;
    float3 dy;
    float3 dz;
};

HsGrassDeformation HsDeformGrass(float3 world, float3 local_position,
    float3 local_height_gradient, float time, float4 benders[32], uint bender_count,
    float4 grid, uint4 masks[16], bool evaluate_frame)
{
    HsGrassDeformation result;
    result.position = world;
    result.dx = float3(1, 0, 0);
    result.dy = float3(0, 1, 0);
    result.dz = float3(0, 0, 1);
    if (local_position.y <= -0.45) return result;
    const float height = saturate(local_position.y + 0.5);
    float3 bend = 0.0;
    float3 height_displacement = 0.0;
    uint candidates = HsGrassCandidateMask(world.xz, bender_count, grid, masks);
    while (candidates != 0u)
    {
        const uint index = firstbitlow(candidates);
        candidates &= candidates - 1u;
        const float2 delta = world.xz - benders[index].xz;
        const float radius = max(benders[index].w, 0.1);
        if (max(abs(delta.x), abs(delta.y)) > radius) continue;
        const float distance = length(delta);
        const float weight = saturate(1.0 - distance / radius);
        const float3 displacement = float3(delta.x, 0.0, delta.y) * weight * 0.42;
        bend += displacement * height;
        if (evaluate_frame)
        {
            height_displacement += displacement;
            if (distance < radius)
            {
                const float radial = distance > 0.0 ? rcp(radius * distance) : 0.0;
                const float xx = 0.42 * (weight - delta.x * delta.x * radial);
                const float xz = -0.42 * delta.x * delta.y * radial;
                const float zz = 0.42 * (weight - delta.y * delta.y * radial);
                result.dx += float3(xx, 0.0, xz) * height;
                result.dz += float3(xz, 0.0, zz) * height;
            }
        }
    }
    const float phase = time * 1.7 + world.x * 0.11 + world.z * 0.07;
    const float wind = sin(phase) * 0.035;
    const float3 wind_displacement = float3(wind, 0.0, wind * 0.7);
    result.position = world + bend + wind_displacement * height;
    if (evaluate_frame)
    {
        const float wind_gradient = cos(phase) * 0.035;
        result.dx += float3(1, 0, 0.7) * (wind_gradient * 0.11 * height);
        result.dz += float3(1, 0, 0.7) * (wind_gradient * 0.07 * height);
        const float3 height_gradient = local_position.y < 0.5 ? local_height_gradient : 0.0;
        height_displacement += wind_displacement;
        result.dx += height_displacement * height_gradient.x;
        result.dy += height_displacement * height_gradient.y;
        result.dz += height_displacement * height_gradient.z;
    }
    return result;
}

void HsGrassDeformFrame(HsGrassDeformation deformation, inout float3 normal,
    inout float3 tangent, inout float handedness)
{
    const float3 cofactor_x = cross(deformation.dy, deformation.dz);
    const float determinant = dot(deformation.dx, cofactor_x);
    // A locally collapsed/fold boundary has no well-defined inverse frame.
    if (!isfinite(determinant) || abs(determinant) < 0.000001) return;
    float3 bent_normal = normal.x * cofactor_x + normal.y * cross(deformation.dz, deformation.dx) +
        normal.z * cross(deformation.dx, deformation.dy);
    bent_normal *= determinant < 0.0 ? -1.0 : 1.0;
    const float normal_length = dot(bent_normal, bent_normal);
    if (!all(isfinite(bent_normal)) || !isfinite(normal_length) || normal_length < 0.000000000001) return;
    bent_normal *= rsqrt(normal_length);
    float3 bent_tangent = deformation.dx * tangent.x + deformation.dy * tangent.y + deformation.dz * tangent.z;
    bent_tangent -= bent_normal * dot(bent_normal, bent_tangent);
    const float tangent_length = dot(bent_tangent, bent_tangent);
    if (!all(isfinite(bent_tangent)) || !isfinite(tangent_length) || tangent_length < 0.000000000001) return;
    normal = bent_normal;
    tangent = bent_tangent * rsqrt(tangent_length);
    handedness *= determinant < 0.0 ? -1.0 : 1.0;
}

#endif