#ifndef HS_PARTICLE_COLLISION_HLSLI
#define HS_PARTICLE_COLLISION_HLSLI
#include "meadow_surface.hlsli"

// ParticleTerrain.xyz is the scene origin; collision_material.y remains the world-zero floor.
float ParticleCollisionFloor(ParticleData particle, float2 position)
{
    const float plane = particle.CollisionMaterial.y + particle.CollisionMaterial.z;
    return ParticleTerrain.w > 0.5
        ? plane + MeadowGroundHeight(position + ParticleTerrain.xz) - ParticleTerrain.y : plane;
}
float3 ParticleTerrainNormal(float2 position)
{
    const float2 p = position + ParticleTerrain.xz;
    const float e = 0.002;
    return normalize(float3(MeadowGroundHeight(p - float2(e, 0)) - MeadowGroundHeight(p + float2(e, 0)),
        2.0 * e, MeadowGroundHeight(p - float2(0, e)) - MeadowGroundHeight(p + float2(0, e))));
}
float3 ParticleBallisticPosition(float3 position, float3 velocity, float3 acceleration, float time)
{
    return position + velocity * time + acceleration * (0.5 * time * time);
}
void IntegrateBallisticStep(inout ParticleData particle, float dt)
{
    if (ParticleTerrain.w <= 0.5)
    {
        IntegrateFlatBallisticStep(particle, dt);
        return;
    }
    float3 position = particle.PositionLife.xyz;
    float3 velocity = particle.CurrentVelocityDrag.xyz;
    const float drag = particle.CurrentVelocityDrag.w;
    const float gravity = particle.PhysicsMetadata.x;
    float3 next_velocity = BallisticVelocity(velocity, drag, gravity, dt);
    const float3 acceleration = (next_velocity - velocity) / dt;
    float3 next_position = position + (velocity + next_velocity) * (0.5 * dt);
    float minimum_y = min(position.y, next_position.y);
    if (acceleration.y > 0.0)
    {
        const float turn = clamp(-velocity.y / acceleration.y, 0.0, dt);
        minimum_y = min(minimum_y, ParticleBallisticPosition(position, velocity, acceleration, turn).y);
    }
    // The authored meadow's macro relief plus largest bank stays below 0.55 m.
    const float upper_floor = particle.CollisionMaterial.y + particle.CollisionMaterial.z + 0.55 - ParticleTerrain.y;
    if (minimum_y > upper_floor)
    {
        particle.PositionLife.xyz = next_position;
        particle.CurrentVelocityDrag.xyz = next_velocity;
        return;
    }
    position.y = max(position.y, ParticleCollisionFloor(particle, position.xz));
    next_position = position + (velocity + next_velocity) * (0.5 * dt);
    // shortcut: 64 samples cap the 2 cm target and resolve one impact; use CCD for faster/grazing or multiple impacts per tick.
    const uint steps = uint(clamp(ceil(max(length(velocity), length(next_velocity)) * dt / 0.02), 1.0, 64.0));
    float lower = 0.0, upper = dt;
    bool hit = false;
    [loop] for (uint step = 1u; step <= steps; ++step)
    {
        const float time = dt * float(step) / float(steps);
        const float3 probe = ParticleBallisticPosition(position, velocity, acceleration, time);
        if (probe.y < ParticleCollisionFloor(particle, probe.xz))
        {
            upper = time;
            hit = true;
            break;
        }
        lower = time;
    }
    if (hit)
    {
        [unroll] for (uint refine = 0u; refine < 6u; ++refine)
        {
            const float time = (lower + upper) * 0.5;
            const float3 probe = ParticleBallisticPosition(position, velocity, acceleration, time);
            if (probe.y < ParticleCollisionFloor(particle, probe.xz)) upper = time;
            else lower = time;
        }
        position = ParticleBallisticPosition(position, velocity, acceleration, upper);
        position.y = ParticleCollisionFloor(particle, position.xz);
        velocity = lerp(velocity, next_velocity, upper / dt);
        const float3 normal = ParticleTerrainNormal(position.xz);
        velocity -= normal * ((1.0 + particle.CollisionMaterial.x) * min(dot(velocity, normal), 0.0));
        const float remaining = dt - upper;
        next_velocity = BallisticVelocity(velocity, drag, gravity, remaining);
        next_position = position + (velocity + next_velocity) * (0.5 * remaining);
        const float floor_y = ParticleCollisionFloor(particle, next_position.xz);
        if (next_position.y <= floor_y)
        {
            next_position.y = floor_y;
            const float3 contact_normal = ParticleTerrainNormal(next_position.xz);
            next_velocity -= contact_normal * min(dot(next_velocity, contact_normal), 0.0);
        }
    }
    particle.PositionLife.xyz = next_position;
    particle.CurrentVelocityDrag.xyz = next_velocity;
}
#endif
