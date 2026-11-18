#include "frame_constants.hlsli"
#include "particle_bindings.hlsli"
#include "meadow_surface.hlsli"

StructuredBuffer<uint> ParticleSpawnOwners : register(t17);
RWStructuredBuffer<ParticleData> WritableParticles : register(u0);
RWByteAddressBuffer IndirectArguments : register(u1);
RWStructuredBuffer<uint> AliveInput : register(u2);
RWStructuredBuffer<uint> AliveOutput : register(u3);
RWStructuredBuffer<uint> DeadIndices : register(u4);
RWByteAddressBuffer ParticleCounters : register(u5);
#include "terrain_page_abi.hlsli"

uint ParticleHash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352d;
    value ^= value >> 15;
    value *= 0x846ca68b;
    return value ^ (value >> 16);
}

float ParticleRandom(uint seed)
{
    return (ParticleHash(seed) & 0x00ffffff) / 16777216.0;
}

float3 BallisticVelocity(float3 velocity, float drag, float gravity, float dt)
{
    return velocity * exp(-drag * dt) + float3(0, gravity * dt, 0);
}
void IntegrateFlatBallisticStep(inout ParticleData particle, float dt)
{
    const float floor_y = particle.CollisionMaterial.y + particle.CollisionMaterial.z;
    float3 position = particle.PositionLife.xyz;
    float3 velocity = particle.CurrentVelocityDrag.xyz;
    position.y = max(position.y, floor_y);
    const float drag = particle.CurrentVelocityDrag.w;
    const float gravity = particle.PhysicsMetadata.x;
    const float restitution = particle.CollisionMaterial.x;
    float3 next_velocity = BallisticVelocity(velocity, drag, gravity, dt);
    float3 next_position = position + (velocity + next_velocity) * (0.5 * dt);
    if (next_position.y < floor_y)
    {
        const float acceleration = (next_velocity.y - velocity.y) / dt;
        const float height = position.y - floor_y;
        float hit_time;
        if (height == 0.0)
            hit_time = velocity.y > 0.0 && acceleration < 0.0 ? -2.0 * velocity.y / acceleration : 0.0;
        else if (acceleration == 0.0)
            hit_time = -height / velocity.y;
        else
        {
            const float root = sqrt(max(0.0, velocity.y * velocity.y - 2.0 * acceleration * height));
            hit_time = 2.0 * height / (-velocity.y + root);
        }
        hit_time = clamp(hit_time, 0.0, dt);
        const float3 impact_velocity = lerp(velocity, next_velocity, hit_time / dt);
        position += (velocity + impact_velocity) * (0.5 * hit_time);
        position.y = floor_y;
        velocity = impact_velocity;
        velocity.y = max(0.0, -velocity.y * restitution);
        const float remaining = dt - hit_time;
        next_velocity = BallisticVelocity(velocity, drag, gravity, remaining);
        next_position = position + (velocity + next_velocity) * (0.5 * remaining);
        // Contact normal constraint handles resting contact and a sub-step return to the plane.
        if (next_position.y < floor_y)
        {
            next_position.y = floor_y;
            next_velocity.y = max(0.0, next_velocity.y);
        }
    }
    particle.PositionLife.xyz = next_position;
    particle.CurrentVelocityDrag.xyz = next_velocity;
}

// Section transforms and particles share scene-relative coordinates.
float ParticleCollisionFloor(ParticleData particle, float2 position)
{
    float floor_y = particle.CollisionMaterial.y;
    bool found = false;
    [loop] for (uint index=0u; index<asuint(ParticleTerrain.w); ++index)
    {
        const HsTerrainPageSection section=HsTerrainSections[index];
        if (section.mesh!=22u || any(section.span==0)) continue;
        const float2 delta=position-section.center;
        const float2 local=float2(dot(delta,float2(section.rotation.x,-section.rotation.y)),
            dot(delta,section.rotation.yx))*24.0/section.span;
        if (any(abs(local)>12.0)) continue;
        const float height=particle.CollisionMaterial.y+section.heightTransform.x+section.heightTransform.y*MeadowGroundHeight(local);
        floor_y=found ? max(floor_y,height) : height;
        found=true;
    }
    return floor_y+particle.CollisionMaterial.z;
}
float3 ParticleTerrainNormal(ParticleData particle, float2 position)
{
    const float e = 0.002;
    return normalize(float3(ParticleCollisionFloor(particle,position-float2(e,0))-ParticleCollisionFloor(particle,position+float2(e,0)),
        2.0 * e, ParticleCollisionFloor(particle,position-float2(0,e))-ParticleCollisionFloor(particle,position+float2(0,e))));
}
float3 ParticleBallisticPosition(float3 position, float3 velocity, float3 acceleration, float time)
{
    return position + velocity * time + acceleration * (0.5 * time * time);
}
void IntegrateBallisticStep(inout ParticleData particle, float dt)
{
    if (asuint(ParticleTerrain.w) == 0u)
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
    const float upper_floor=particle.CollisionMaterial.y+ParticleTerrain.x+particle.CollisionMaterial.z;
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
        const float3 normal = ParticleTerrainNormal(particle,position.xz);
        velocity -= normal * ((1.0 + particle.CollisionMaterial.x) * min(dot(velocity, normal), 0.0));
        const float remaining = dt - upper;
        next_velocity = BallisticVelocity(velocity, drag, gravity, remaining);
        next_position = position + (velocity + next_velocity) * (0.5 * remaining);
        const float floor_y = ParticleCollisionFloor(particle, next_position.xz);
        if (next_position.y <= floor_y)
        {
            next_position.y = floor_y;
            const float3 contact_normal = ParticleTerrainNormal(particle,next_position.xz);
            next_velocity -= contact_normal * min(dot(next_velocity, contact_normal), 0.0);
        }
    }
    particle.PositionLife.xyz = next_position;
    particle.CurrentVelocityDrag.xyz = next_velocity;
}

void CatchUpBallistic(inout ParticleData particle, uint now)
{
    const uint steps = now - particle.Authored.y;
    for (uint step = 0; step < steps; ++step)
    {
        const float fraction = particle.Authored.y == particle.Authored.z ? particle.InitialPositionSpawnTime.w : 0.0;
        IntegrateBallisticStep(particle, (1.0 - fraction) / 60.0);
        particle.Authored.y += 1;
    }
    const float age = (float(now - particle.Authored.z) - particle.InitialPositionSpawnTime.w) / 60.0;
    particle.PositionLife.w = particle.InitialVelocityMaxLife.w - max(0.0, age);
}

uint ParticleDrawBin(uint metadata)
{
    if (((metadata >> 8u) & 0xffu) != 3u) return 0u;
    const uint primitive = (metadata >> 16u) & 0xffu;
    const uint selector = metadata >> 24u;
    if (selector != 0u) return selector;
    return primitive == 11u ? 8u : primitive == 7u ? 1u : primitive == 10u ? 7u : 2u;
}

// Group-local ranks reserve each global list once; dead-slot admission is unchanged.
groupshared uint ParticleGroupCounts[10];
groupshared uint ParticleGroupBases[10];
groupshared uint ParticleGroupAliveBase;
void AppendParticleGroup(uint lane, bool active, bool alive, uint particle_index, uint bin)
{
    if (lane < 10u) ParticleGroupCounts[lane] = 0u;
    GroupMemoryBarrierWithGroupSync();
    uint rank = 0u;
    const uint category = alive ? bin : 9u;
    if (active) InterlockedAdd(ParticleGroupCounts[category], 1u, rank);
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0u)
    {
        uint alive_count = 0u;
        [unroll] for (uint i = 0u; i < 9u; ++i) alive_count += ParticleGroupCounts[i];
        ParticleGroupAliveBase = 0u;
        if (alive_count != 0u) ParticleCounters.InterlockedAdd(4u, alive_count, ParticleGroupAliveBase);
    }
    if (lane < 10u && ParticleGroupCounts[lane] != 0u)
        ParticleCounters.InterlockedAdd(lane == 9u ? 8u : 12u + lane * 4u,
            ParticleGroupCounts[lane], ParticleGroupBases[lane]);
    GroupMemoryBarrierWithGroupSync();
    if (!active) return;
    if (!alive) { DeadIndices[ParticleGroupBases[9] + rank] = particle_index; return; }
    uint prefix = 0u;
    [unroll] for (uint i = 0u; i < 9u; ++i)
        if (i < bin) prefix += ParticleGroupCounts[i];
    AliveOutput[ParticleGroupAliveBase + prefix + rank] = particle_index;
    AliveOutput[ParticleOptions.x * (1u + bin) + ParticleGroupBases[bin] + rank] = particle_index;
}

bool SpawnParticle(uint index, out uint particle_index, out uint bin)
{
    particle_index = 0u; bin = 0u;
    if (index >= ParticleOptions.z)
    {
        return false;
    }

    uint command_index = ParticleSpawnOwners[index];
    if (command_index >= ParticleOptions.y)
    {
        return false;
    }

    ParticleSpawnCommandGpu command = ParticleSpawnCommands[command_index];
    uint command_particle = index - command.Metadata.z;
    uint seed = command.Metadata.y ^ command_particle;
    float age = command.Authored.x != 0
        ? max(0.0, (float(AuthoredParticleClock.x-command.Authored.z)-asfloat(command.Authored.y))/60.0)
        : asfloat(command.Metadata.w);
    float lifetime = lerp(command.PositionLifetimeMin.w,
                          command.DirectionLifetimeMax.w,
                          ParticleRandom(seed));
    if (age >= lifetime)
    {
        return false;
    }

    uint old_dead_count = ParticleCounters.Load(8);
    while (old_dead_count != 0)
    {
        uint observed_dead_count;
        ParticleCounters.InterlockedCompareExchange(
            8, old_dead_count, old_dead_count - 1, observed_dead_count);
        if (observed_dead_count == old_dead_count)
        {
            break;
        }
        old_dead_count = observed_dead_count;
    }
    if (old_dead_count == 0)
    {
        return false;
    }

    particle_index = DeadIndices[old_dead_count - 1];
    float u = ParticleRandom(seed + 1);
    float v = ParticleRandom(seed + 2);
    float angle = v * 6.2831853;
    float sphere_y = 2.0 * u - 1.0;
    float sphere_r = sqrt(max(0.0, 1.0 - sphere_y * sphere_y));
    float3 sphere_direction = float3(cos(angle) * sphere_r, sphere_y,
                                     sin(angle) * sphere_r);
    float3 forward = normalize(command.DirectionLifetimeMax.xyz);
    float3 right = normalize(abs(forward.y) < 0.99
        ? cross(float3(0, 1, 0), forward)
        : cross(float3(1, 0, 0), forward));
    float3 up = cross(forward, right);
    float3 local = 0;
    if (command.Modes.x == 1)
        local = sphere_direction * command.ShapeExtentSpeedMin.x *
                pow(ParticleRandom(seed + 3), 1.0 / 3.0);
    else if (command.Modes.x == 2)
        local = float3(cos(angle), 0.0, sin(angle)) *
                command.ShapeExtentSpeedMin.x * sqrt(u);
    else if (command.Modes.x == 3)
        local = float3(cos(angle), 0.0, sin(angle)) *
                command.ShapeExtentSpeedMin.x;
    else if (command.Modes.x == 4)
        local = right * lerp(-command.ShapeExtentSpeedMin.x,
                              command.ShapeExtentSpeedMin.x, u);
    float3 initial_position = command.PositionLifetimeMin.xyz + local;
    float3 velocity_direction = forward;
    if (command.Modes.y == 1)
    {
        float cosine = lerp(1.0, cos(command.SpeedConeGravityStretch.y), u);
        float sine = sqrt(max(0.0, 1.0 - cosine * cosine));
        velocity_direction = normalize(forward * cosine +
            (right * cos(angle) + up * sin(angle)) * sine);
    }
    else if (command.Modes.y == 2)
        velocity_direction = length(local) > 0.0001 ? normalize(local) : sphere_direction;
    else if (command.Modes.y == 3)
        velocity_direction = length(local) > 0.0001 ? -normalize(local) : -forward;
    else if (command.Modes.y == 4)
    {
        float cosine = u;
        float sine = sqrt(max(0.0, 1.0 - cosine * cosine));
        velocity_direction = float3(cos(angle) * sine, cosine, sin(angle) * sine);
    }
    float speed = lerp(command.ShapeExtentSpeedMin.w,
                       command.SpeedConeGravityStretch.x,
                       ParticleRandom(seed + 4));
    float3 velocity = command.Authored.x != 0 ? command.CurrentVelocityDrag.xyz : velocity_direction * speed;
    float initial_rotation = lerp(command.RotationRange.x, command.RotationRange.y,
                                  ParticleRandom(seed + 7));
    uint visual_metadata = command.Modes.z;
    uint facing = visual_metadata & 0xffu;
    uint renderer = (visual_metadata >> 8u) & 0xffu;
    if (renderer == 1u && facing == 1u && length(velocity.xz) > 0.0001)
        initial_rotation = atan2(-velocity.x, velocity.z);

    ParticleData particle;
    particle.InitialPositionSpawnTime =
        float4(initial_position, CameraTime.w - age);
    particle.InitialVelocityMaxLife = float4(velocity, lifetime);
    particle.PositionLife.xyz = initial_position + velocity * age;
    particle.PositionLife.y += 0.5 * command.SpeedConeGravityStretch.z * age * age;
    particle.PositionLife.w = lifetime - age;
    particle.StartColor = command.StartColor;
    particle.EndColor = command.EndColor;
    particle.SizeRotation = float4(
        lerp(command.SizeRange.x, command.SizeRange.y, ParticleRandom(seed + 5)),
        lerp(command.SizeRange.z, command.SizeRange.w, ParticleRandom(seed + 6)),
        initial_rotation,
        lerp(command.RotationRange.z, command.RotationRange.w, ParticleRandom(seed + 8)));
    particle.PhysicsMetadata = float4(command.SpeedConeGravityStretch.z,
        command.SpeedConeGravityStretch.w, asfloat(command.Modes.z),
        asfloat(command.Modes.w));
    particle.CurrentVelocityDrag = 0;
    particle.CollisionMaterial = 0;
    particle.Authored = 0;
    if (command.Authored.x != 0)
    {
        particle.InitialPositionSpawnTime = float4(command.PositionLifetimeMin.xyz, asfloat(command.Authored.y));
        particle.PositionLife.xyz = command.PositionLifetimeMin.xyz;
        particle.CurrentVelocityDrag = command.CurrentVelocityDrag;
        particle.CollisionMaterial = command.CollisionMaterial;
        particle.PositionLife.y = max(particle.PositionLife.y,
            ParticleCollisionFloor(particle, particle.PositionLife.xz));
        particle.Authored = uint4(1, command.Authored.z, command.Authored.z, command.Authored.w);
        CatchUpBallistic(particle, AuthoredParticleClock.x);
    }
    WritableParticles[particle_index] = particle;

    bin = ParticleDrawBin(visual_metadata);
    return true;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void ParticleCS(uint3 dispatch_id : SV_DispatchThreadID, uint lane : SV_GroupIndex)
{
    uint index = dispatch_id.x;
    uint capacity = ParticleOptions.x;

    if (PassValue == 0)
    {
        if (index < capacity)
        {
            DeadIndices[index] = capacity - index - 1;
        }
        if (index == 0)
        {
            ParticleCounters.Store(0, 0);
            ParticleCounters.Store(4, 0);
            [unroll] for (uint bin = 0u; bin < 9u; ++bin)
                ParticleCounters.Store(12u + bin * 4u, 0u);
            ParticleCounters.Store(8, capacity);
        }
        return;
    }

    if (PassValue == 1)
    {
        if (index == 0)
        {
            ParticleCounters.Store(4, 0);
            [unroll] for (uint bin = 0u; bin < 9u; ++bin)
                ParticleCounters.Store(12u + bin * 4u, 0u);
        }
        return;
    }

    if (PassValue == 2)
    {
        const bool active = index < ParticleCounters.Load(0);
        bool alive = false;
        uint particle_index = 0u, bin = 0u;
        if (active)
        {
            particle_index = AliveInput[index];
            ParticleData particle = WritableParticles[particle_index];
            if (particle.Authored.x != 0)
            {
                const float age = max(0.0, (float(AuthoredParticleClock.x - particle.Authored.z) - particle.InitialPositionSpawnTime.w) / 60.0);
                particle.PositionLife.w = particle.InitialVelocityMaxLife.w - age;
                if (particle.PositionLife.w > 0) CatchUpBallistic(particle, AuthoredParticleClock.x);
            }
            else
            {
                float age = max(CameraTime.w - particle.InitialPositionSpawnTime.w, 0.0);
                particle.PositionLife.xyz = particle.InitialPositionSpawnTime.xyz + particle.InitialVelocityMaxLife.xyz * age;
                particle.PositionLife.y += 0.5 * particle.PhysicsMetadata.x * age * age;
                particle.PositionLife.w = particle.InitialVelocityMaxLife.w - age;
            }
            alive = particle.PositionLife.w > 0.0;
            if (alive)
            {
                bin = ParticleDrawBin(asuint(particle.PhysicsMetadata.z));
                WritableParticles[particle_index].PositionLife = particle.PositionLife;
                if (particle.Authored.x != 0u)
                {
                    WritableParticles[particle_index].CurrentVelocityDrag = particle.CurrentVelocityDrag;
                    WritableParticles[particle_index].Authored = particle.Authored;
                }
            }
        }
        AppendParticleGroup(lane, active, alive, particle_index, bin);
        return;
    }
    if (PassValue == 3)
    {
        uint particle_index, bin;
        const bool spawned = SpawnParticle(index, particle_index, bin);
        AppendParticleGroup(lane, spawned, true, particle_index, bin);
        return;
    }

    if (PassValue == 5 && index == 0)
    {
        uint alive_count = ParticleCounters.Load(4);
        ParticleCounters.Store(0, alive_count);
        [unroll] for (uint bin = 0u; bin < 9u; ++bin)
        {
            uint vertex_count = 6u;
            if (bin == 8u) vertex_count = 24u;
            else if (bin != 0u) vertex_count = VfxMeshAtlas.Load(24u + (bin - 1u) * 12u);
            const uint argument = bin * 20u;
            IndirectArguments.Store(argument, bin);
            IndirectArguments.Store4(argument + 4u,
                uint4(vertex_count, ParticleCounters.Load(12u + bin * 4u), 0u, 0u));
        }
    }
}
