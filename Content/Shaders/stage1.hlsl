cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProjection;
    float4 CameraTime;
    float4 LightDirectionIntensity;
    float4 LightColor;
    float4 ScreenSize;
    float4x4 ShadowViewProjection[3];
    row_major float4x4 ArcherBones[128];
    float4 RenderOptions;
    uint4 ParticleOptions;
};

cbuffer PassConstants : register(b1)
{
    uint PassValue;
};

struct InstanceData
{
    float4 Position;
    float4 Scale;
    uint Color;
    uint Mesh;
    float Yaw;
    float Padding;
};

struct ParticleData
{
    float4 PositionLife;
    float4 InitialPositionSpawnTime;
    float4 InitialVelocityMaxLife;
    float4 StartColorSize;
    float4 EndColorSize;
    float4 PhysicsSprite;
};

struct ParticleSpawnCommandGpu
{
    float4 PositionLifetime;
    float4 VelocitySpread;
    float4 StartColorSize;
    float4 EndColorSize;
    float4 Physics;
    uint4 Metadata;
};

StructuredBuffer<InstanceData> Instances : register(t0);
StructuredBuffer<ParticleData> Particles : register(t1);
StructuredBuffer<ParticleSpawnCommandGpu> ParticleSpawnCommands : register(t12);
StructuredBuffer<uint> DrawAliveIndices : register(t13);
RWStructuredBuffer<ParticleData> WritableParticles : register(u0);
RWByteAddressBuffer IndirectArguments : register(u1);
RWStructuredBuffer<uint> AliveInput : register(u2);
RWStructuredBuffer<uint> AliveOutput : register(u3);
RWStructuredBuffer<uint> DeadIndices : register(u4);
RWByteAddressBuffer ParticleCounters : register(u5);
Texture2D<float4> GBufferBase : register(t2);
Texture2D<float4> GBufferNormal : register(t3);
Texture2D<float4> GBufferPosition : register(t4);
Texture2DArray<float> ShadowMap : register(t5);
Texture2D<float4> HdrColor : register(t6);
Texture2D<float4> OitAccumulation : register(t7);
Texture2D<float> OitRevealage : register(t8);
Texture2D<float4> PostA : register(t9);
Texture2D<float4> PostB : register(t10);
Texture2D<float4> UiTexture : register(t11);
Texture2DArray<float4> ArcherDiffuse : register(t14);
Texture2DArray<float4> ArcherNormal : register(t15);
SamplerState LinearClamp : register(s0);
SamplerComparisonState ShadowCompare : register(s1);
SamplerState MaterialSampler : register(s2);

struct SceneInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    uint4 BoneIndices : BLENDINDICES;
    float4 BoneWeights : BLENDWEIGHT;
    float2 Uv : TEXCOORD;
    float4 Tangent : TANGENT;
    uint Material : MATERIAL;
};

struct SceneOutput
{
    float4 Position : SV_Position;
    float3 WorldNormal : NORMAL;
    float3 WorldPosition : TEXCOORD0;
    float4 Color : COLOR0;
    nointerpolation uint Mesh : TEXCOORD1;
    float2 Uv : TEXCOORD2;
    float4 WorldTangent : TEXCOORD3;
    nointerpolation uint Material : TEXCOORD4;
};

float4 UnpackColor(uint packed)
{
    return float4(
        (packed & 0xff) / 255.0,
        ((packed >> 8) & 0xff) / 255.0,
        ((packed >> 16) & 0xff) / 255.0,
        ((packed >> 24) & 0xff) / 255.0);
}

void SkinArcher(inout float3 position, inout float3 normal, inout float3 tangent,
                SceneInput input, uint mesh)
{
    if (mesh != 0)
    {
        return;
    }
    const float4x4 skin =
        ArcherBones[input.BoneIndices.x] * input.BoneWeights.x +
        ArcherBones[input.BoneIndices.y] * input.BoneWeights.y +
        ArcherBones[input.BoneIndices.z] * input.BoneWeights.z +
        ArcherBones[input.BoneIndices.w] * input.BoneWeights.w;
    position = mul(skin, float4(position, 1.0)).xyz;
    normal = normalize(mul((float3x3)skin, normal));
    tangent = normalize(mul((float3x3)skin, tangent));
}

float3 ShapePlaceholder(float3 position, uint mesh)
{
    // Runtime-only primitives: boxes, tapered boxes, wedges, and planes.
    if (mesh == 2) // ranged enemy: narrow column
    {
        position.xz *= 0.72;
    }
    else if (mesh == 3) // suicide enemy: tapered warning shape
    {
        position.xz *= lerp(1.0, 0.28, saturate(position.y + 0.5));
    }
    else if (mesh == 5 || mesh == 6) // projectiles: arrow-like wedge
    {
        position.x *= lerp(0.25, 1.0, saturate(0.5 - position.z));
        position.y *= 0.55;
    }
    else if (mesh == 7) // area: ground plane
    {
        position.y *= 0.1;
    }
    else if (mesh == 8) // pickup: tilted marker
    {
        const float sine = 0.70710678;
        const float cosine = 0.70710678;
        position.xy = float2(position.x * cosine - position.y * sine,
                             position.x * sine + position.y * cosine);
    }
    else if (mesh == 9) // test ground
    {
        position.y *= 0.1;
    }
    return position;
}

float3 InstanceWorldPosition(InstanceData instance, float3 position)
{
    float sine_yaw;
    float cosine_yaw;
    sincos(instance.Yaw, sine_yaw, cosine_yaw);
    float3 local = ShapePlaceholder(position, instance.Mesh) * instance.Scale.xyz;
    float3 rotated = float3(
        local.x * cosine_yaw + local.z * sine_yaw,
        local.y,
        -local.x * sine_yaw + local.z * cosine_yaw);
    return instance.Position.xyz + rotated;
}

SceneOutput SceneVS(SceneInput input, uint instance_id : SV_InstanceID)
{
    InstanceData instance = Instances[instance_id];
    float3 local_position = input.Position;
    float3 local_normal = input.Normal;
    float3 local_tangent = input.Tangent.xyz;
    SkinArcher(local_position, local_normal, local_tangent, input, instance.Mesh);
    float sine_yaw;
    float cosine_yaw;
    sincos(instance.Yaw, sine_yaw, cosine_yaw);
    float3 world = InstanceWorldPosition(instance, local_position);
    local_normal /= max(instance.Scale.xyz, 0.0001);
    float3 normal = normalize(float3(
        local_normal.x * cosine_yaw + local_normal.z * sine_yaw,
        local_normal.y,
        -local_normal.x * sine_yaw + local_normal.z * cosine_yaw));
    float3 tangent = normalize(float3(
        local_tangent.x * cosine_yaw + local_tangent.z * sine_yaw,
        local_tangent.y,
        -local_tangent.x * sine_yaw + local_tangent.z * cosine_yaw));

    SceneOutput output;
    output.Position = mul(float4(world, 1.0), ViewProjection);
    output.WorldNormal = normal;
    output.WorldPosition = world;
    output.Color = UnpackColor(instance.Color);
    output.Mesh = instance.Mesh;
    output.Uv = input.Uv;
    output.WorldTangent = float4(tangent, input.Tangent.w);
    output.Material = input.Material;
    return output;
}

float4 ShadowVS(SceneInput input, uint instance_id : SV_InstanceID) : SV_Position
{
    InstanceData instance = Instances[instance_id];
    float3 local_position = input.Position;
    float3 local_normal = input.Normal;
    float3 local_tangent = input.Tangent.xyz;
    SkinArcher(local_position, local_normal, local_tangent, input, instance.Mesh);
    float3 world = InstanceWorldPosition(instance, local_position);
    return mul(float4(world, 1.0), ShadowViewProjection[PassValue]);
}

struct GBufferOutput
{
    float4 BaseColor : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Position : SV_Target2;
};

float4 SampleArcherDiffuse(uint material, float2 uv)
{
    const uint slice = min(material, max(ParticleOptions.w, 1u) - 1u);
    return ArcherDiffuse.SampleLevel(MaterialSampler, float3(uv, slice), 0);
}

float3 SampleArcherNormal(uint material, float2 uv)
{
    const uint slice = min(material, max(ParticleOptions.w, 1u) - 1u);
    return ArcherNormal.SampleLevel(MaterialSampler, float3(uv, slice), 0).xyz;
}

GBufferOutput ScenePS(SceneOutput input)
{
    GBufferOutput output;
    output.BaseColor = input.Color;
    float3 world_normal = normalize(input.WorldNormal);
    if (input.Mesh == 0)
    {
        const uint material = input.Material;
        const float4 base_color = SampleArcherDiffuse(material, input.Uv);
        clip(base_color.a - 0.2);
        output.BaseColor = float4(base_color.rgb, 1.0);
        const float3 tangent_normal =
            SampleArcherNormal(material, input.Uv) * 2.0 - 1.0;
        const float3 tangent = normalize(input.WorldTangent.xyz);
        const float3 bitangent =
            normalize(cross(world_normal, tangent)) * input.WorldTangent.w;
        world_normal = normalize(tangent * tangent_normal.x +
                                 bitangent * tangent_normal.y +
                                 world_normal * tangent_normal.z);
    }
    if (input.Mesh == 9)
    {
        const float2 distance_to_line =
            abs(frac(input.WorldPosition.xz + 0.5) - 0.5) /
            max(fwidth(input.WorldPosition.xz), 0.0001);
        const float grid = 1.0 - saturate(min(distance_to_line.x,
                                              distance_to_line.y));
        output.BaseColor.rgb = lerp(input.Color.rgb, 1.0, grid);
    }
    output.Normal = float4(world_normal * 0.5 + 0.5, 1.0);
    output.Position = float4(input.WorldPosition, input.Mesh == 0 ? 1.0 : 0.0);
    return output;
}

struct FullScreenOutput
{
    float4 Position : SV_Position;
    float2 Uv : TEXCOORD0;
};

FullScreenOutput FullScreenVS(uint vertex_id : SV_VertexID)
{
    static const float2 positions[3] = {
        float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0)
    };
    FullScreenOutput output;
    output.Position = float4(positions[vertex_id], 0.0, 1.0);
    output.Uv = float2(positions[vertex_id].x * 0.5 + 0.5, 0.5 - positions[vertex_id].y * 0.5);
    return output;
}

float ShadowVisibility(float3 world)
{
    float distance_from_target = length(world.xz);
    uint cascade = distance_from_target < 9.0 ? 0 : (distance_from_target < 18.0 ? 1 : 2);
    float4 shadow_position = mul(float4(world, 1.0), ShadowViewProjection[cascade]);
    shadow_position.xyz /= shadow_position.w;
    float2 shadow_uv =
        float2(shadow_position.x * 0.5 + 0.5, 0.5 - shadow_position.y * 0.5);
    if (any(shadow_uv < 0.0) || any(shadow_uv > 1.0))
    {
        return 1.0;
    }
    return ShadowMap.SampleCmpLevelZero(
        ShadowCompare, float3(shadow_uv, cascade), shadow_position.z + 0.001);
}

float4 DeferredPS(FullScreenOutput input) : SV_Target0
{
    float4 base = GBufferBase.SampleLevel(LinearClamp, input.Uv, 0);
    float3 normal = normalize(GBufferNormal.SampleLevel(LinearClamp, input.Uv, 0).xyz * 2.0 - 1.0);
    float3 world = GBufferPosition.SampleLevel(LinearClamp, input.Uv, 0).xyz;
    float3 light = normalize(-LightDirectionIntensity.xyz);
    float diffuse = dot(normal, light);
    float stepped_diffuse = diffuse > 0.55 ? 1.0 : (diffuse > 0.05 ? 0.62 : 0.28);
    float rim = pow(1.0 - saturate(normal.y), 3.0);
    float visibility = base.a > 0.0 ? lerp(0.42, 1.0, ShadowVisibility(world)) : 1.0;
    float3 color = base.rgb *
                       (0.35 + stepped_diffuse * visibility * LightColor.rgb) +
                   base.rgb * rim * 0.22;
    float3 arena = float3(0.035, 0.055, 0.085);
    return float4(lerp(arena, color, base.a), 1.0);
}

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

[numthreads(256, 1, 1)]
void ParticleCS(uint3 dispatch_id : SV_DispatchThreadID)
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
            ParticleCounters.Store(8, capacity);
            IndirectArguments.Store4(0, uint4(6, 0, 0, 0));
        }
        return;
    }

    if (PassValue == 1)
    {
        if (index == 0)
        {
            ParticleCounters.Store(4, 0);
            IndirectArguments.Store4(0, uint4(6, 0, 0, 0));
        }
        return;
    }

    if (PassValue == 2)
    {
        uint alive_count = ParticleCounters.Load(0);
        if (index >= alive_count)
        {
            return;
        }

        uint particle_index = AliveInput[index];
        ParticleData particle = WritableParticles[particle_index];
        float age = max(CameraTime.w - particle.InitialPositionSpawnTime.w, 0.0);
        particle.PositionLife.xyz =
            particle.InitialPositionSpawnTime.xyz + particle.InitialVelocityMaxLife.xyz * age;
        particle.PositionLife.y += 0.5 * particle.PhysicsSprite.x * age * age;
        particle.PositionLife.w = particle.InitialVelocityMaxLife.w - age;
        if (particle.PositionLife.w > 0.0)
        {
            uint output_index;
            ParticleCounters.InterlockedAdd(4, 1, output_index);
            AliveOutput[output_index] = particle_index;
            WritableParticles[particle_index] = particle;
        }
        else
        {
            uint dead_index;
            ParticleCounters.InterlockedAdd(8, 1, dead_index);
            DeadIndices[dead_index] = particle_index;
        }
        return;
    }

    if (PassValue == 3)
    {
        if (index >= ParticleOptions.z)
        {
            return;
        }

        uint command_index = 0;
        uint command_particle = index;
        for (; command_index < ParticleOptions.y; ++command_index)
        {
            uint command_count = ParticleSpawnCommands[command_index].Metadata.z;
            if (command_particle < command_count)
            {
                break;
            }
            command_particle -= command_count;
        }
        if (command_index >= ParticleOptions.y)
        {
            return;
        }

        ParticleSpawnCommandGpu command = ParticleSpawnCommands[command_index];
        float age = command.Physics.w;
        if (age >= command.PositionLifetime.w)
        {
            return;
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
            return;
        }

        uint particle_index = DeadIndices[old_dead_count - 1];
        uint seed = command.Metadata.w ^ command_particle;
        float angle = ParticleRandom(seed) * 6.2831853;
        float vertical = lerp(-0.15, 1.0, ParticleRandom(seed + 1));
        float radial = sqrt(ParticleRandom(seed + 2));
        float3 velocity = command.VelocitySpread.xyz +
                          float3(cos(angle) * radial, vertical, sin(angle) * radial) *
                              command.VelocitySpread.w;

        ParticleData particle;
        particle.InitialPositionSpawnTime =
            float4(command.PositionLifetime.xyz, CameraTime.w - age);
        particle.InitialVelocityMaxLife = float4(velocity, command.PositionLifetime.w);
        particle.PositionLife.xyz = command.PositionLifetime.xyz + velocity * age;
        particle.PositionLife.y += 0.5 * command.Physics.x * age * age;
        particle.PositionLife.w = command.PositionLifetime.w - age;
        particle.StartColorSize = command.StartColorSize;
        particle.EndColorSize = command.EndColorSize;
        uint sprite_metadata =
            min(command.Metadata.x, 0xffff) | (min(command.Metadata.y, 0xffff) << 16);
        particle.PhysicsSprite =
            float4(command.Physics.x, command.Physics.y, command.Physics.z,
                   asfloat(sprite_metadata));
        WritableParticles[particle_index] = particle;

        uint output_index;
        ParticleCounters.InterlockedAdd(4, 1, output_index);
        AliveOutput[output_index] = particle_index;
        return;
    }

    if (PassValue == 4 && index == 0)
    {
        uint alive_count = ParticleCounters.Load(4);
        ParticleCounters.Store(0, alive_count);
        IndirectArguments.Store4(0, uint4(6, alive_count, 0, 0));
    }
}

struct ParticleOutput
{
    float4 Position : SV_Position;
    float2 Uv : TEXCOORD0;
    float2 LocalUv : TEXCOORD1;
    float4 Color : COLOR0;
};

ParticleOutput ParticleVS(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    static const float2 corners[6] = {
        float2(-1, -1), float2(-1, 1), float2(1, 1),
        float2(-1, -1), float2(1, 1), float2(1, -1)
    };
    ParticleData particle = Particles[DrawAliveIndices[instance_id]];
    float2 corner = corners[vertex_id];
    float progress =
        saturate(1.0 - particle.PositionLife.w / max(particle.InitialVelocityMaxLife.w, 0.0001));
    float size = lerp(particle.StartColorSize.w, particle.EndColorSize.w, progress);
    float elapsed = particle.InitialVelocityMaxLife.w - particle.PositionLife.w;
    float rotation = particle.PhysicsSprite.y + particle.PhysicsSprite.z * elapsed;
    float sine;
    float cosine;
    sincos(rotation, sine, cosine);
    float2 rotated = float2(
        corner.x * cosine - corner.y * sine, corner.x * sine + corner.y * cosine);
    float3 world =
        particle.PositionLife.xyz + float3(rotated.x * size, rotated.y * size, 0.0);

    uint sprite_metadata = asuint(particle.PhysicsSprite.w);
    uint sprite_index = sprite_metadata & 0xffff;
    uint sprite_count = max(sprite_metadata >> 16, 1);
    uint frame = (sprite_index + (uint)(progress * sprite_count)) % sprite_count;
    uint grid = (uint)ceil(sqrt((float)sprite_count));
    float2 cell = float2(frame % grid, frame / grid);

    ParticleOutput output;
    output.Position = mul(float4(world, 1.0), ViewProjection);
    output.Uv = (cell + corner * 0.5 + 0.5) / grid;
    output.LocalUv = corner;
    output.Color = lerp(particle.StartColorSize, particle.EndColorSize, progress);
    return output;
}

struct OitOutput
{
    float4 Accumulation : SV_Target0;
    float4 Revealage : SV_Target1;
};

OitOutput ParticlePS(ParticleOutput input)
{
    float falloff = saturate(1.0 - dot(input.LocalUv, input.LocalUv));
    float alpha = input.Color.a * falloff;
    OitOutput output;
    output.Accumulation = float4(input.Color.rgb * alpha, alpha);
    output.Revealage = alpha.xxxx;
    return output;
}

float4 CompositePS(FullScreenOutput input) : SV_Target0
{
    float3 opaque = HdrColor.SampleLevel(LinearClamp, input.Uv, 0).rgb;
    float4 accumulation = OitAccumulation.SampleLevel(LinearClamp, input.Uv, 0);
    float revealage = OitRevealage.SampleLevel(LinearClamp, input.Uv, 0);
    float3 transparent = accumulation.rgb / max(accumulation.a, 0.0001);
    return float4(lerp(opaque, transparent, saturate(1.0 - revealage)), 1.0);
}

float4 BloomPS(FullScreenOutput input) : SV_Target0
{
    float2 texel = ScreenSize.zw;
    float3 center = PostA.SampleLevel(LinearClamp, input.Uv, 0).rgb;
    float3 bloom = 0.0;
    bloom += PostA.SampleLevel(LinearClamp, input.Uv + float2(texel.x * 2.0, 0), 0).rgb;
    bloom += PostA.SampleLevel(LinearClamp, input.Uv - float2(texel.x * 2.0, 0), 0).rgb;
    bloom += PostA.SampleLevel(LinearClamp, input.Uv + float2(0, texel.y * 2.0), 0).rgb;
    bloom += PostA.SampleLevel(LinearClamp, input.Uv - float2(0, texel.y * 2.0), 0).rgb;
    bloom *= 0.25;
    bloom *= saturate(max(max(bloom.r, bloom.g), bloom.b) - 0.45);
    return float4(center + bloom * (RenderOptions.y > 0.5 ? 0.35 : 0.0), 1.0);
}

float3 AcesToneMap(float3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

float4 ToneMapPS(FullScreenOutput input) : SV_Target0
{
    return float4(AcesToneMap(PostB.SampleLevel(LinearClamp, input.Uv, 0).rgb), 1.0);
}

float4 OutlinePS(FullScreenOutput input) : SV_Target0
{
    float2 texel = ScreenSize.zw * 0.675;
    float center_mask = GBufferPosition.SampleLevel(LinearClamp, input.Uv, 0).a;
    float edge = 0.0;
    static const float2 offsets[8] = {
        float2(-1, -1), float2(0, -1), float2(1, -1), float2(-1, 0),
        float2(1, 0), float2(-1, 1), float2(0, 1), float2(1, 1)
    };
    for (uint index = 0; index < 8; ++index)
    {
        float neighbor_mask = GBufferPosition.SampleLevel(
            LinearClamp, input.Uv + offsets[index] * texel, 0).a;
        edge = max(edge, abs(center_mask - neighbor_mask));
    }
    float3 color = PostA.SampleLevel(LinearClamp, input.Uv, 0).rgb;
    float outline = RenderOptions.z > 0.5 ? smoothstep(0.1, 0.9, edge) : 0.0;
    return float4(lerp(color, color * 0.08, outline), 1.0);
}

float4 FxaaPS(FullScreenOutput input) : SV_Target0
{
    float2 texel = ScreenSize.zw;
    float3 center = PostB.SampleLevel(LinearClamp, input.Uv, 0).rgb;
    float3 north = PostB.SampleLevel(LinearClamp, input.Uv + float2(0, texel.y), 0).rgb;
    float3 south = PostB.SampleLevel(LinearClamp, input.Uv - float2(0, texel.y), 0).rgb;
    float3 east = PostB.SampleLevel(LinearClamp, input.Uv + float2(texel.x, 0), 0).rgb;
    float3 west = PostB.SampleLevel(LinearClamp, input.Uv - float2(texel.x, 0), 0).rgb;
    float luma_center = dot(center, float3(0.299, 0.587, 0.114));
    float luma_range = max(max(dot(north, float3(0.299, 0.587, 0.114)),
                               dot(south, float3(0.299, 0.587, 0.114))),
                           max(dot(east, float3(0.299, 0.587, 0.114)),
                               dot(west, float3(0.299, 0.587, 0.114)))) -
                       min(min(dot(north, float3(0.299, 0.587, 0.114)),
                               dot(south, float3(0.299, 0.587, 0.114))),
                           min(dot(east, float3(0.299, 0.587, 0.114)),
                               dot(west, float3(0.299, 0.587, 0.114))));
    float3 filtered = (north + south + east + west + center * 2.0) / 6.0;
    return float4(lerp(center, filtered, saturate((luma_range - 0.08) * 4.0)), 1.0);
}

float4 UiPS(FullScreenOutput input) : SV_Target0
{
    const float reference_aspect = 1920.0 / 1080.0;
    const float output_aspect = ScreenSize.x / ScreenSize.y;
    float2 reference_uv = input.Uv;
    if (output_aspect > reference_aspect)
    {
        reference_uv.x = (reference_uv.x - 0.5) * output_aspect / reference_aspect + 0.5;
    }
    else
    {
        reference_uv.y = (reference_uv.y - 0.5) * reference_aspect / output_aspect + 0.5;
    }
    if (any(reference_uv < 0.0) || any(reference_uv > 1.0)) discard;
    return UiTexture.SampleLevel(LinearClamp, reference_uv, 0);
}
