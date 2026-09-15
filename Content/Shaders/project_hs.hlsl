cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProjection;
    float4 CameraTime;
    float4 LightDirectionIntensity;
    float4 LightColor;
    float4 ScreenSize;
    float4 CameraForwardSoftness;
    float4x4 ShadowViewProjection[3];
    float4 ShadowAtlasScaleOffset[3];
    float4 ShadowAtlasTexelSize;
    row_major float4x4 ArcherBones[128];
    uint4 MonsterAssetMeta[6];
    uint4 MonsterClipMeta[6][5];
    float4 RenderOptions;
    uint4 ParticleOptions;
    float4 GrassBenders[32];
    uint4 GrassBenderCount;
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
    float4 StartColor;
    float4 EndColor;
    float4 SizeRotation;
    float4 PhysicsMetadata;
};

struct ParticleSpawnCommandGpu
{
    float4 PositionLifetimeMin;
    float4 DirectionLifetimeMax;
    float4 ShapeExtentSpeedMin;
    float4 SpeedConeGravityStretch;
    float4 StartColor;
    float4 EndColor;
    float4 SizeRange;
    float4 RotationRange;
    uint4 Modes;
    uint4 Metadata;
};

StructuredBuffer<InstanceData> Instances : register(t0);
StructuredBuffer<ParticleData> Particles : register(t1);
StructuredBuffer<ParticleSpawnCommandGpu> ParticleSpawnCommands : register(t12);
StructuredBuffer<uint> DrawAliveIndices : register(t13);
StructuredBuffer<uint> ParticleSpawnOwners : register(t17);
StructuredBuffer<row_major float4x4> MonsterSkinMatrices : register(t18);
RWStructuredBuffer<ParticleData> WritableParticles : register(u0);
RWByteAddressBuffer IndirectArguments : register(u1);
RWStructuredBuffer<uint> AliveInput : register(u2);
RWStructuredBuffer<uint> AliveOutput : register(u3);
RWStructuredBuffer<uint> DeadIndices : register(u4);
RWByteAddressBuffer ParticleCounters : register(u5);
Texture2D<float4> GBufferBase : register(t2);
Texture2D<float4> GBufferNormal : register(t3);
Texture2D<float4> GBufferPosition : register(t4);
Texture2D<float> ShadowMap : register(t5);
Texture2D<float4> HdrColor : register(t6);
Texture2D<float4> OitAccumulation : register(t7);
Texture2D<float> OitRevealage : register(t8);
Texture2D<float4> PostA : register(t9);
Texture2D<float4> PostB : register(t10);
Texture2D<float4> UiTexture : register(t11);
Texture2DArray<float4> ArcherDiffuse : register(t14);
Texture2DArray<float4> ArcherNormal : register(t15);
Texture2DArray<float> VfxMasks : register(t16);
Texture2D<float4> MonsterBasecolor : register(t0, space1);
Texture2D<float4> MonsterEmissive : register(t1, space1);
Texture2D<float4> MonsterRam : register(t2, space1);
Texture2DArray<float4> FamilyDiffuse : register(t3, space1);
Texture2DArray<float4> FamilyNormal : register(t4, space1);
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
    nointerpolation float Charge : TEXCOORD5;
    nointerpolation uint EnvironmentSeed : TEXCOORD6;
    nointerpolation float EnvironmentFade : TEXCOORD7;
    nointerpolation uint EnvironmentFlags : TEXCOORD8;
    float3 EnvironmentStableWorld : TEXCOORD9;
};

#include "environment.hlsli"

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

void SkinMonster(inout float3 position, inout float3 normal, inout float3 tangent,
                 SceneInput input, InstanceData instance, uint mesh)
{
    if (mesh < 10 || mesh > 15) return;
    const uint asset = mesh - 10;
    const uint clip = min((instance.Mesh >> 8) & 0xff, 4);
    const uint4 clip_meta = MonsterClipMeta[asset][clip];
    const uint frame = min((uint)(saturate(instance.Padding) * (clip_meta.y - 1)),
                           clip_meta.y - 1);
    const uint matrix_base = MonsterAssetMeta[asset].x + clip_meta.x +
                             frame * clip_meta.w;
    const float4x4 skin =
        MonsterSkinMatrices[matrix_base + input.BoneIndices.x] * input.BoneWeights.x +
        MonsterSkinMatrices[matrix_base + input.BoneIndices.y] * input.BoneWeights.y +
        MonsterSkinMatrices[matrix_base + input.BoneIndices.z] * input.BoneWeights.z +
        MonsterSkinMatrices[matrix_base + input.BoneIndices.w] * input.BoneWeights.w;
    position = mul(skin, float4(position, 1.0)).xyz;
    // Squash/stretch needs the inverse transpose, including for blended
    // matrices with shear. Normalization removes the determinant magnitude.
    const float3x3 linear_skin = (float3x3)skin;
    const float3x3 cofactor = float3x3(
        cross(linear_skin[1], linear_skin[2]),
        cross(linear_skin[2], linear_skin[0]),
        cross(linear_skin[0], linear_skin[1]));
    const float determinant = dot(linear_skin[0], cofactor[0]);
    normal = normalize(mul(cofactor, normal) * (determinant < 0.0 ? -1.0 : 1.0));
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
    else if (mesh == 5) // projectiles: arrow-like wedge
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
    else if (mesh == 16) // faceted tree trunk
    {
        position.xz *= lerp(0.72, 0.42, saturate(position.y + 0.5));
    }
    else if (mesh == 17) // faceted tree canopy
    {
        position.y = position.y * 0.72 + 0.55;
        position.xz *= lerp(0.95, 0.58, saturate(position.y));
    }
    else if (mesh == 18) // low-poly rock
    {
        position.y = (position.y + 0.5) * 0.62 - 0.5;
        position.xz *= lerp(0.82, 0.58, saturate(position.y + 0.5));
    }
    else if (mesh == 19) // thin grass blade
    {
        position.x *= 0.08;
        position.z *= 0.8;
        position.y = (position.y + 0.5) * 0.5 - 0.5;
    }
    else if (mesh == 20) // dirt patch
    {
        position.y *= 0.035;
    }
    return position;
}

float3 BendGrass(float3 world, float3 local_position)
{
    if (local_position.y <= -0.45) return world;
    float3 bend = 0.0;
    const uint count = min(GrassBenderCount.x, 32u);
    for (uint index = 0; index < count; ++index)
    {
        const float2 delta = world.xz - GrassBenders[index].xz;
        const float radius = max(GrassBenders[index].w, 0.1);
        const float weight = saturate(1.0 - length(delta) / radius);
        bend += float3(delta.x, 0.0, delta.y) * weight * 0.42 * saturate(local_position.y + 0.5);
    }
    const float wind = sin(CameraTime.w * 1.7 + world.x * 0.11 + world.z * 0.07) * 0.035;
    return world + bend + float3(wind, 0.0, wind * 0.7) * saturate(local_position.y + 0.5);
}

float3 InstanceWorldPosition(InstanceData instance, float3 position)
{
    float sine_yaw;
    float cosine_yaw;
    sincos(instance.Yaw, sine_yaw, cosine_yaw);
    const uint mesh = instance.Mesh & 255;
    const bool authored = (instance.Mesh & 0x10000) != 0;
    float3 local = ((authored || mesh == 16 || mesh == 17 || mesh == 19) ? position : ShapePlaceholder(position, mesh)) * instance.Scale.xyz;
    float3 rotated = float3(
        local.x * cosine_yaw + local.z * sine_yaw,
        local.y,
        -local.x * sine_yaw + local.z * cosine_yaw);
    const float3 world = instance.Position.xyz + rotated;
    return world;
}

SceneOutput SceneVS(SceneInput input, uint instance_id : SV_InstanceID, uint vertex_id : SV_VertexID)
{
    InstanceData instance = Instances[instance_id];
    const uint mesh = instance.Mesh & 0xff;
    uint environment_seed = (mesh == 9 || mesh == 20) ? asuint(instance.Padding) : EnvCell(floor(instance.Position.xz * 16), 71);
    uint environment_entry;
    EnvironmentVertex(input, instance, vertex_id, environment_seed, environment_entry);
    float3 local_position = input.Position;
    float3 local_normal = input.Normal;
    float3 local_tangent = input.Tangent.xyz;
    SkinArcher(local_position, local_normal, local_tangent, input, mesh);
    SkinMonster(local_position, local_normal, local_tangent, input, instance, mesh);
    float sine_yaw;
    float cosine_yaw;
    sincos(instance.Yaw, sine_yaw, cosine_yaw);
    const float3 stable_world = InstanceWorldPosition(instance, local_position);
    float3 world = mesh == 19 ? BendGrass(stable_world, local_position -
        float3(0, (instance.Mesh & 0x10000) != 0 ? 0.5 : 0.0, 0)) : stable_world;
    local_normal /= max(instance.Scale.xyz, 0.0001);
    local_tangent *= instance.Scale.xyz;
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
    output.Mesh = mesh;
    output.Uv = input.Uv;
    output.WorldTangent = float4(tangent, input.Tangent.w);
    output.Material = (mesh == 17 || mesh == 19) ? environment_entry : input.Material;
    output.EnvironmentSeed = environment_seed;
    output.EnvironmentFade = instance.Scale.w;
    output.EnvironmentFlags = instance.Mesh & 0x30000;
    output.EnvironmentStableWorld = stable_world;
    output.Charge = mesh == 12 && ((instance.Mesh >> 8) & 0xff) == 2
        ? saturate(instance.Padding) : 0.0;
    return output;
}

SceneOutput ShadowVS(SceneInput input, uint instance_id : SV_InstanceID, uint vertex_id : SV_VertexID)
{
    SceneOutput output = SceneVS(input, instance_id, vertex_id);
    output.Position = mul(float4(output.WorldPosition, 1.0), ShadowViewProjection[PassValue]);
    return output;
}
void ShadowPS(SceneOutput input, bool front : SV_IsFrontFace)
{
    EnvironmentLodClip(input);
    if (input.Mesh == 17 || input.Mesh == 19) clip(FoliageColor(input.Mesh, input.Material, input.Uv).a - 0.5);
    else if (!front) discard;
}

struct GBufferOutput
{
    float4 BaseColor : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Position : SV_Target2;
    float4 Material : SV_Target3;
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

float3 SampleMonsterPbr(float2 uv)
{
    const float3 basecolor = MonsterBasecolor.SampleLevel(MaterialSampler, uv, 0).rgb;
    const float3 emissive = MonsterEmissive.SampleLevel(MaterialSampler, uv, 0).rgb;
    const float3 ram = MonsterRam.SampleLevel(MaterialSampler, uv, 0).rgb;
    const float roughness = ram.r;
    const float metallic = ram.b;
    const float3 lit_base = basecolor * lerp(0.75, 1.0, roughness);
    const float3 metal_tint = lerp(float3(1.0, 1.0, 1.0), basecolor, metallic);
    return lit_base * metal_tint + emissive * 80.0;
}

GBufferOutput ScenePS(SceneOutput input, bool front : SV_IsFrontFace)
{
    EnvironmentLodClip(input);
    GBufferOutput output;
    output.BaseColor = input.Color;
    output.Material = 0;
    if (!front && input.Mesh != 17 && input.Mesh != 19) discard;
    float3 world_normal = normalize(input.WorldNormal);
    float roughness = 0.3;
    if (input.Mesh == 6) discard; // gel projectiles use the transparent pass
    if (input.Mesh == 0 || (input.Mesh >= 10 && input.Mesh <= 12))
    {
        const bool family = input.Mesh >= 10;
        const float3 uv = float3(input.Uv, input.Mesh - 10);
        const float4 base_color = family ? FamilyDiffuse.Sample(MaterialSampler, uv)
            : SampleArcherDiffuse(input.Material, input.Uv);
        clip(base_color.a - (family ? 0.98 : 0.2));
        output.BaseColor = float4(base_color.rgb, 1.0);
        const float4 normal_map = family ? FamilyNormal.Sample(MaterialSampler, uv)
            : float4(SampleArcherNormal(input.Material, input.Uv), 0.3);
        roughness = normal_map.a;
        float3 tangent_normal = normal_map.xyz * 2.0 - 1.0;
        // Blender +Y normals need a green flip after the cooker's UV.v flip.
        if (family) tangent_normal.y = -tangent_normal.y;
        const float3 tangent = normalize(input.WorldTangent.xyz -
            world_normal * dot(world_normal, input.WorldTangent.xyz));
        const float3 bitangent = normalize(cross(world_normal, tangent)) * input.WorldTangent.w;
        world_normal = normalize(tangent * tangent_normal.x +
            bitangent * tangent_normal.y + world_normal * tangent_normal.z);
    }
    if (input.Mesh >= 13 && input.Mesh <= 15)
        output.BaseColor = float4(SampleMonsterPbr(input.Uv), 1.0);
    if (input.Mesh == 9 || input.Mesh == 20 || (input.Mesh >= 16 && input.Mesh <= 19))
    {
        EnvMaterial m = (EnvMaterial)0;
        float flag = 1;
        if (input.Mesh == 9 || input.Mesh == 20)
            m = EnvTerrain(input.WorldPosition, input.Mesh, input.EnvironmentSeed);
        else if (input.Mesh == 16 || input.Mesh == 18)
        {
            m = EnvOpaque(input.Uv, input.Material, input.Mesh == 18, length(CameraTime.xyz-input.WorldPosition));
            {
                float3 t = normalize(input.WorldTangent.xyz-world_normal*dot(world_normal,input.WorldTangent.xyz));
                float3 b = normalize(cross(world_normal,t))*input.WorldTangent.w;
                m.normal = normalize(t*m.normal.x+b*m.normal.y+world_normal*m.normal.z);
            }
        }
        else
        {
            float4 color = FoliageColor(input.Mesh,input.Material,input.Uv);
            clip(color.a-0.5);
            float3 uv = FoliageUv(input.Mesh,input.Material,input.Uv);
            float3 n = EnvDecode(input.Mesh==19 ? GrassNormal.Sample(FoliageClamp,uv).rg : LeafNormal.Sample(FoliageClamp,uv).rg);
            if (!front) world_normal = -world_normal;
            float3 t = normalize(input.WorldTangent.xyz-world_normal*dot(world_normal,input.WorldTangent.xyz));
            float3 b = normalize(cross(world_normal,t))*input.WorldTangent.w;
            m.color=color.rgb;m.normal=normalize(t*n.x+b*n.y+world_normal*n.z);m.ao=1;
            m.roughness=input.Mesh==19 ? GrassRoughness.Sample(FoliageClamp,uv) : LeafRoughness.Sample(FoliageClamp,uv);
            flag=input.Mesh==19 ? 0.6 : 0.75;
        }
        output.BaseColor=float4(m.color,1);world_normal=m.normal;
        output.Material=float4(m.roughness,m.ao,0,flag);
    }
    output.Normal = float4(world_normal * 0.5 + 0.5,
        input.Mesh >= 10 && input.Mesh <= 12 ? 2.0 + roughness : 1.0);
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
    float visibility = 1.0;
    // Coarse coverage supplies the edge of each finer map, including the far fade.
    [unroll] for (int cascade = 2; cascade >= 0; --cascade)
    {
        float4 projected = mul(float4(world, 1.0), ShadowViewProjection[cascade]);
        projected.xyz /= projected.w;
        const float2 uv = projected.xy * float2(0.5, -0.5) + 0.5;
        if (any(uv < 0.0) || any(uv > 1.0) || projected.z < 0.0 || projected.z > 1.0)
            continue;
        const float4 tile = ShadowAtlasScaleOffset[cascade];
        const float2 atlas_uv = uv * tile.xy + tile.zw;
        const float2 lower = tile.zw + ShadowAtlasTexelSize.xy * 0.5;
        const float2 upper = tile.zw + tile.xy - ShadowAtlasTexelSize.xy * 0.5;
        float filtered = 0.0;
        [unroll] for (int y = -1; y <= 1; ++y)
        {
            [unroll] for (int x = -1; x <= 1; ++x)
            {
                const float2 sample_uv = clamp(
                    atlas_uv + float2(x, y) * ShadowAtlasTexelSize.xy, lower, upper);
                filtered += ShadowMap.SampleCmpLevelZero(ShadowCompare, sample_uv, projected.z);
            }
        }
        const float edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
        visibility = lerp(visibility, filtered / 9.0, smoothstep(0.02, 0.12, edge));
    }
    return visibility;
}

// Shared lighting for opaque family features and transparent jelly bodies.
float3 JellyLighting(float3 base, float3 normal, float3 world, float roughness, float charge)
{
    const float3 view = normalize(CameraTime.xyz - world);
    const float3 light = normalize(-LightDirectionIntensity.xyz);
    const float3 halfway = normalize(view + light);
    const float nv = saturate(dot(normal, view));
    const float nl = dot(normal, light);
    const float visibility = lerp(0.42, 1.0, ShadowVisibility(world));
    const float wrapped = saturate((nl + 0.4) / 1.4);
    const float fresnel = 0.035 + 0.965 * pow(1.0 - nv, 5.0);
    const float gloss = lerp(160.0, 18.0, saturate(roughness));
    const float highlight = pow(saturate(dot(normal, halfway)), gloss);
    const float thickness = pow(nv, 1.5);
    const float internal = thickness * (0.05 + 0.15 * saturate(-nl) + charge * 0.45);
    return base * (0.30 + (saturate(nl) * visibility + wrapped * 0.25) * LightColor.rgb + internal)
        + LightColor.rgb * highlight * (0.35 + fresnel) * visibility
        + lerp(base, float3(0.70, 0.85, 1.0), 0.30) * fresnel * 0.32;
}

float4 DeferredPS(FullScreenOutput input) : SV_Target0
{
    float4 base = GBufferBase.SampleLevel(LinearClamp, input.Uv, 0);
    float4 normal_material = GBufferNormal.SampleLevel(LinearClamp, input.Uv, 0);
    float3 normal = normalize(normal_material.xyz * 2.0 - 1.0);
    float3 world = GBufferPosition.SampleLevel(LinearClamp, input.Uv, 0).xyz;
    float4 environment_material = GBufferMaterial.SampleLevel(LinearClamp,input.Uv,0);
    if (environment_material.a > 0.5)
        return float4(EnvironmentLighting(base.rgb,normal,world,environment_material,ShadowVisibility(world)),1);
    if (normal_material.a >= 2.0)
        return float4(JellyLighting(base.rgb, normal, world, normal_material.a - 2.0, 0.0), 1.0);
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
            IndirectArguments.Store4(0, uint4(24, 0, 0, 0));
        }
        return;
    }

    if (PassValue == 1)
    {
        if (index == 0)
        {
            ParticleCounters.Store(4, 0);
            IndirectArguments.Store4(0, uint4(24, 0, 0, 0));
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
        particle.PositionLife.y += 0.5 * particle.PhysicsMetadata.x * age * age;
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

        uint command_index = ParticleSpawnOwners[index];
        if (command_index >= ParticleOptions.y)
        {
            return;
        }

        ParticleSpawnCommandGpu command = ParticleSpawnCommands[command_index];
        uint command_particle = index - command.Metadata.z;
        uint seed = command.Metadata.y ^ command_particle;
        float age = asfloat(command.Metadata.w);
        float lifetime = lerp(command.PositionLifetimeMin.w,
                              command.DirectionLifetimeMax.w,
                              ParticleRandom(seed));
        if (age >= lifetime)
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
        float3 velocity = velocity_direction * speed;
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
        IndirectArguments.Store4(0, uint4(24, alive_count, 0, 0));
    }
}

struct ParticleOutput
{
    float4 Position : SV_Position;
    float2 Uv : TEXCOORD0;
    float2 LocalUv : TEXCOORD1;
    float4 Color : COLOR0;
    float3 WorldPosition : TEXCOORD2;
    nointerpolation uint Facing : TEXCOORD3;
    nointerpolation uint Sprite : TEXCOORD4;
    float Progress : TEXCOORD5;
    nointerpolation uint2 FrameGrid : TEXCOORD6;
    float3 Normal : TEXCOORD7;
    nointerpolation uint Renderer : TEXCOORD8;
    nointerpolation uint Primitive : TEXCOORD9;
    nointerpolation float2 SegmentMetadata : TEXCOORD10;
};

static const float3 OctahedronVertices[6] = {
    float3(0, 1, 0), float3(0, -1, 0), float3(-1, 0, 0),
    float3(1, 0, 0), float3(0, 0, -1), float3(0, 0, 1)
};

static const uint3 OctahedronFaces[8] = {
    uint3(0, 5, 3), uint3(0, 3, 4), uint3(0, 4, 2), uint3(0, 2, 5),
    uint3(1, 3, 5), uint3(1, 4, 3), uint3(1, 2, 4), uint3(1, 5, 2)
};

ParticleOutput ParticleVS(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    static const float2 corners[6] = {
        float2(-1, -1), float2(-1, 1), float2(1, 1),
        float2(-1, -1), float2(1, 1), float2(1, -1)
    };
    ParticleData particle = Particles[DrawAliveIndices[instance_id]];
    uint visual_metadata = asuint(particle.PhysicsMetadata.z);
    uint facing = visual_metadata & 0xffu;
    uint renderer = (visual_metadata >> 8u) & 0xffu;
    uint primitive = (visual_metadata >> 16u) & 0xffu;
    float2 corner = vertex_id < 6 ? corners[vertex_id] : 0;
    float progress =
        saturate(1.0 - particle.PositionLife.w / max(particle.InitialVelocityMaxLife.w, 0.0001));
    float size = lerp(particle.SizeRotation.x, particle.SizeRotation.y, progress);
    float elapsed = particle.InitialVelocityMaxLife.w - particle.PositionLife.w;
    // Segment SizeRotation.zw carries authored UV repeat/scroll metadata, not rotation.
    float rotation = renderer == 2u ? 0.0 : particle.SizeRotation.z + particle.SizeRotation.w * elapsed;
    float sine;
    float cosine;
    sincos(rotation, sine, cosine);
    float2 rotated = float2(
        corner.x * cosine - corner.y * sine, corner.x * sine + corner.y * cosine);
    float3 camera_forward = normalize(CameraForwardSoftness.xyz);
    float3 axis_x = normalize(cross(float3(0, 1, 0), camera_forward));
    float3 axis_y = normalize(cross(camera_forward, axis_x));
    if (renderer == 1)
    {
        axis_x = float3(1, 0, 0);
        axis_y = float3(0, 0, 1);
    }
    else if (renderer == 2)
    {
        float3 segment_direction = particle.InitialVelocityMaxLife.xyz;
        segment_direction.y = 0.0;
        axis_y = length(segment_direction) > 0.0001
            ? normalize(segment_direction)
            : float3(0, 0, 1);
        axis_x = float3(axis_y.z, 0, -axis_y.x);
    }
    else if (facing == 1)
    {
        float3 current_velocity = particle.InitialVelocityMaxLife.xyz +
            float3(0, particle.PhysicsMetadata.x * elapsed, 0);
        float3 projected = current_velocity - camera_forward * dot(current_velocity, camera_forward);
        if (length(projected) > 0.0001) axis_y = normalize(projected);
        axis_x = normalize(cross(axis_y, camera_forward));
    }
    float stretch = facing == 1 || renderer == 2 ? particle.PhysicsMetadata.y : 1.0;
    float3 world = particle.PositionLife.xyz + axis_x * rotated.x * size +
                   axis_y * rotated.y * size * stretch;
    float3 normal = -camera_forward;
    if (renderer == 1 || renderer == 2) normal = float3(0, 1, 0);

    if (renderer == 3)
    {
        uint3 face = OctahedronFaces[min(vertex_id / 3, 7u)];
        uint vertex = vertex_id % 3;
        uint source_index = vertex == 0 ? face.x : vertex == 1 ? face.y : face.z;
        float3 local = OctahedronVertices[source_index];
        float3 scale = float3(0.45, 0.45, 1.25);
        if (primitive == 8) scale = float3(0.38, 0.32, 0.9);
        else if (primitive == 9) scale = float3(0.42, 0.55, 0.42);
        else if (primitive == 10) scale = float3(0.32, 0.8, 0.32);
        else if (primitive == 11) scale = 1.0;
        float3 velocity = particle.InitialVelocityMaxLife.xyz +
                          float3(0, particle.PhysicsMetadata.x * elapsed, 0);
        float3 forward = length(velocity) > 0.0001 ? normalize(velocity) : float3(0, 1, 0);
        float3 reference = abs(forward.y) < 0.95 ? float3(0, 1, 0) : float3(1, 0, 0);
        float3 mesh_x = normalize(cross(reference, forward));
        float3 mesh_y = normalize(cross(forward, mesh_x));
        float3 scaled_local = local * scale * size;
        world = particle.PositionLife.xyz + mesh_x * scaled_local.x +
                mesh_y * scaled_local.y + forward * scaled_local.z;
        normal = normalize(mesh_x * local.x + mesh_y * local.y + forward * local.z);
        corner = local.xy;
    }

    ParticleOutput output;
    output.Position = mul(float4(world, 1.0), ViewProjection);
    output.Uv = corner * 0.5 + 0.5;
    output.LocalUv = corner;
    output.Color = lerp(particle.StartColor, particle.EndColor, progress);
    output.WorldPosition = world;
    output.Facing = facing;
    uint sprite_metadata = asuint(particle.PhysicsMetadata.w);
    output.Sprite = sprite_metadata & 0xffffu;
    output.FrameGrid = max(uint2((sprite_metadata >> 16u) & 0xffu,
                                 (sprite_metadata >> 24u) & 0xffu), 1u);
    output.Progress = progress;
    output.Normal = normal;
    output.Renderer = renderer;
    output.Primitive = primitive;
    output.SegmentMetadata = particle.SizeRotation.zw;
    return output;
}

struct OitOutput
{
    float4 Accumulation : SV_Target0;
    float4 Revealage : SV_Target1;
};

float FlameHash(float2 p)
{
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

float FlameNoise(float2 p)
{
    float2 cell = floor(p);
    float2 f = smoothstep(0.0, 1.0, frac(p));
    float a = FlameHash(cell);
    float b = FlameHash(cell + float2(1, 0));
    float c = FlameHash(cell + float2(0, 1));
    float d = FlameHash(cell + float2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float FlameFbm(float2 p)
{
    float value = 0.0;
    float amplitude = 0.55;
    [unroll] for (uint octave = 0; octave < 4; ++octave)
    {
        value += FlameNoise(p) * amplitude;
        p = p * 2.03 + float2(17.0, 9.0);
        amplitude *= 0.5;
    }
    return value / 0.9125;
}

float FlameDensity(float2 uv, float progress, bool ground)
{
    float2 p = uv;
    float t = CameraTime.w * (ground ? 1.6 : 2.4) + progress * 4.0;
    float warp = FlameFbm(p * (ground ? 3.0 : 2.8) + float2(t * 0.18, -t * 0.12));
    p += (warp - 0.5) * (ground ? 0.18 : 0.24);
    float density;
    if (ground)
    {
        float radius = length(p);
        float bed = 1.0 - smoothstep(0.72, 0.98, radius);
        float tongues = smoothstep(0.18, 0.62, FlameFbm(p * 4.5 + float2(t, -t * 0.7)));
        density = bed * tongues;
    }
    else
    {
        float height = saturate(p.y * 0.5 + 0.5);
        float taper = lerp(1.0, 0.22, height);
        float width = abs(p.x) / max(taper, 0.08);
        float body = 1.0 - smoothstep(0.45, 1.0, width);
        float tip = 1.0 - smoothstep(0.72, 1.0, height);
        density = body * tip * smoothstep(0.15, 0.72, FlameFbm(p * 3.4 + float2(t * 0.3, -t)));
    }
    float aa = max(fwidth(density), 0.002);
    return smoothstep(0.15 - aa, 0.62 + aa, density);
}

OitOutput SlimePS(SceneOutput input)
{
    float4 base = float4(0.08, 0.68, 0.12, 0.72);
    float3 normal = normalize(input.WorldNormal);
    float roughness = 0.25;
    if (input.Mesh != 6)
    {
        const float3 uv = float3(input.Uv, input.Mesh - 10);
        base = FamilyDiffuse.Sample(MaterialSampler, uv);
        if (base.a >= 0.98) discard; // opaque eyes and ornaments wrote depth already
        const float4 normal_map = FamilyNormal.Sample(MaterialSampler, uv);
        roughness = normal_map.a;
        float3 tn = normal_map.xyz * 2.0 - 1.0;
        tn.y = -tn.y;
        const float3 tangent = normalize(input.WorldTangent.xyz - normal * dot(normal, input.WorldTangent.xyz));
        const float3 bitangent = normalize(cross(normal, tangent)) * input.WorldTangent.w;
        normal = normalize(tangent * tn.x + bitangent * tn.y + normal * tn.z);
        if (input.Mesh == 12)
        {
            const float3 charge_color = input.Charge < 0.5
                ? lerp(float3(1.0, 0.26, 0.025), float3(1.0, 0.065, 0.012), input.Charge * 2.0)
                : lerp(float3(1.0, 0.065, 0.012), float3(0.95, 0.008, 0.018), input.Charge * 2.0 - 1.0);
            base.rgb *= charge_color / float3(1.0, 0.26, 0.025);
        }
    }
    clip(base.a - 0.001);
    const float3 color = JellyLighting(base.rgb, normal, input.WorldPosition, roughness, input.Charge);
    // Keep authored opaque features in the depth pass; the jelly body transmits the floor.
    const float alpha = base.a * 0.45;
    const float z = saturate(length(input.WorldPosition - CameraTime.xyz) / 180.0);
    const float weight = clamp(pow(min(1.0, alpha * 10.0) + 0.01, 3.0) *
        1e8 * pow(1.0 - z * 0.9, 3.0), 1e-2, 3e3);
    OitOutput output;
    output.Accumulation = float4(color * alpha * weight, alpha * weight);
    output.Revealage = alpha.xxxx;
    return output;
}

OitOutput ParticlePS(ParticleOutput input)
{
    float mask = 1.0;
    if (input.Renderer == 0)
    {
        if (input.Primitive == 17)
        {
            mask = FlameDensity(input.LocalUv, input.Progress, false);
        }
        else
        {
            float2 mask_uv = input.Uv;
            uint frame_count = input.FrameGrid.x * input.FrameGrid.y;
            if (frame_count > 1)
            {
                uint frame = min((uint)(input.Progress * frame_count), frame_count - 1u);
                mask_uv = (mask_uv + float2(frame % input.FrameGrid.x,
                                            frame / input.FrameGrid.x)) /
                          float2(input.FrameGrid);
            }
            mask = VfxMasks.Sample(MaterialSampler, float3(mask_uv, input.Sprite));
        }
    }
    else if (input.Renderer == 1)
    {
        float2 p = input.LocalUv;
        if (input.Primitive == 17)
        {
            mask = FlameDensity(p, input.Progress, true);
        }
        else
        {
        float radius = length(p);
        if (input.Primitive == 1)
            mask = 1.0 - smoothstep(0.82, 1.0, radius);
        else if (input.Primitive == 2)
            mask = 1.0 - smoothstep(0.01375, 0.0325, abs(radius - 0.82));
        else if (input.Primitive == 3)
        {
            float angle = abs(atan2(p.x, max(p.y, 0.0001)));
            mask = (1.0 - smoothstep(0.52, 0.64, angle)) *
                   (1.0 - smoothstep(0.82, 1.0, radius)) * step(0.0, p.y);
        }
        else if (input.Primitive == 4)
        {
            float arm = abs(abs(p.x) - (0.25 + 0.45 * (p.y * 0.5 + 0.5)));
            mask = (1.0 - smoothstep(0.06, 0.14, arm)) *
                   (1.0 - smoothstep(0.82, 1.0, radius));
        }
        else if (input.Primitive == 5)
        {
            float ring = 1.0 - smoothstep(0.01125, 0.0275, abs(radius - 0.74));
            float spokes = 1.0 - smoothstep(0.035, 0.09,
                abs(sin(atan2(p.y, p.x) * 4.0)) * radius);
            mask = max(ring, spokes * smoothstep(0.2, 0.3, radius) *
                             (1.0 - smoothstep(0.65, 0.8, radius)));
        }
        else if (input.Primitive == 7)
        {
            float shaft = (1.0 - smoothstep(0.07, 0.15, abs(p.x))) *
                          step(-0.62, p.y) * step(p.y, 0.36);
            float head = (1.0 - smoothstep(0.04, 0.12,
                abs(abs(p.x) - (0.42 - p.y) * 0.48))) *
                step(0.20, p.y) * step(p.y, 0.72);
            mask = max(shaft, head);
        }
        else
        {
            float angle = atan2(p.y, p.x);
            float crack = abs(sin(angle * 7.0 + radius * 11.0 +
                                  sin(angle * 3.0) * 1.8));
            mask = (1.0 - smoothstep(0.1, 0.3, crack)) *
                   smoothstep(0.12, 0.25, radius) *
                   (1.0 - smoothstep(0.82, 1.0, radius));
        }
        }
    }
    else if (input.Renderer == 2)
    {
        float half_width = input.Primitive == 16
            ? lerp(0.08, 0.68, saturate(input.LocalUv.y * 0.5 + 0.5))
            : 0.68;
        float across = 1.0 - smoothstep(half_width, min(half_width + 0.32, 1.0),
                                        abs(input.LocalUv.x));
        float ends = 1.0 - smoothstep(0.82, 1.0, abs(input.LocalUv.y));
        mask = across * ends;
        float repeat = max(input.SegmentMetadata.x, 0.01);
        float scroll = input.SegmentMetadata.y;
        float2 ribbon_uv = float2(input.LocalUv.x * 0.5 + 0.5,
                                  (input.LocalUv.y * 0.5 + 0.5) * repeat + CameraTime.w * scroll);
        uint frame_count = input.FrameGrid.x * input.FrameGrid.y;
        if (frame_count > 1)
        {
            uint frame = min((uint)(input.Progress * frame_count), frame_count - 1u);
            ribbon_uv = (ribbon_uv + float2(frame % input.FrameGrid.x,
                                            frame / input.FrameGrid.x)) /
                        float2(input.FrameGrid);
        }
        mask *= VfxMasks.Sample(MaterialSampler, float3(ribbon_uv, input.Sprite));
        mask *= 1.0 - smoothstep(0.86, 1.0, abs(input.LocalUv.y));
        if (input.Primitive == 13)
            mask *= step(0.42, frac((input.LocalUv.y * 0.5 + 0.5) * 7.0 + CameraTime.w * 5.0));
        else if (input.Primitive == 14)
            mask *= 0.55 + 0.45 * sin((input.LocalUv.y + CameraTime.w * 3.0) * 9.0);
        else if (input.Primitive == 15)
            mask *= 0.6 + 0.4 * step(0.5, frac((input.LocalUv.y * 0.5 + 0.5) * 5.0));
        else if (input.Primitive == 16)
            mask *= saturate(input.LocalUv.y * 0.5 + 0.5);
    }
    if (input.Primitive == 17)
    {
        float core = saturate(1.0 - abs(input.LocalUv.x) * 1.7) * saturate(input.LocalUv.y * 0.5 + 0.5);
        input.Color.rgb = lerp(float3(4.2, 0.12, 0.01), float3(7.0, 2.2, 0.18), core);
    }
    float alpha = input.Color.a * mask;
    if (input.Renderer == 3)
    {
        float3 view_direction = normalize(CameraTime.xyz - input.WorldPosition);
        float facing_light = saturate(dot(normalize(input.Normal),
                                          normalize(-LightDirectionIntensity.xyz)));
        float lighting = 0.35 + 0.65 * facing_light;
        if (input.Primitive == 11)
        {
            float fresnel = pow(1.0 - saturate(abs(dot(input.Normal, view_direction))), 1.5);
            alpha *= 0.25 + 0.75 * fresnel;
        }
        input.Color.rgb *= lighting;
    }
    clip(alpha - 0.001);
    if (input.Renderer == 0 || input.Renderer == 3)
    {
        float2 screen_uv = input.Position.xy / ScreenSize.xy;
        float valid = GBufferNormal.SampleLevel(LinearClamp, screen_uv, 0).a;
        float3 scene_world = GBufferPosition.SampleLevel(LinearClamp, screen_uv, 0).xyz;
        float scene_depth = dot(scene_world - CameraTime.xyz, CameraForwardSoftness.xyz);
        float particle_depth = dot(input.WorldPosition - CameraTime.xyz, CameraForwardSoftness.xyz);
        alpha *= valid > 0.0 ? saturate((scene_depth - particle_depth) / CameraForwardSoftness.w) : 1.0;
    }
    float linear_depth = length(input.WorldPosition - CameraTime.xyz);
    float z = saturate(linear_depth / 180.0);
    float weight = clamp(pow(min(1.0, alpha * 10.0) + 0.01, 3.0) *
                         1e8 * pow(1.0 - z * 0.9, 3.0), 1e-2, 3e3);
    OitOutput output;
    output.Accumulation = float4(input.Color.rgb * alpha * weight, alpha * weight);
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
