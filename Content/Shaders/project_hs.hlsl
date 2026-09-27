cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProjection;
    float4x4 PreviousViewProjection;
    float4 TemporalOptions;
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
    uint4 AuthoredParticleClock;
    float4 VfxLightPositionRadius[32];
    float4 VfxLightColorIntensity[32];
    uint4 VfxLightCount;
    uint4 VfxDecalCount;
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

struct FresnelShellGpu
{
    float4 CurrentTransform[4];
    float4 PreviousTransform[4];
    float4 Color;
    float4 Geometry;
    float4 Motion;
    float4 Signature;
    uint4 Metadata;
    uint4 Identity;
};
StructuredBuffer<FresnelShellGpu> FresnelShellCommands : register(t25, space3);

struct ParticleData
{
    float4 PositionLife;
    float4 InitialPositionSpawnTime;
    float4 InitialVelocityMaxLife;
    float4 StartColor;
    float4 EndColor;
    float4 SizeRotation;
    float4 PhysicsMetadata;
    float4 CurrentVelocityDrag;
    float4 CollisionMaterial;
    uint4 Authored;
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
    float4 CurrentVelocityDrag;
    float4 CollisionMaterial;
    uint4 Authored;
};

StructuredBuffer<InstanceData> Instances : register(t0);
StructuredBuffer<ParticleData> Particles : register(t1);
ByteAddressBuffer VfxMeshAtlas : register(t0, space3);
StructuredBuffer<ParticleSpawnCommandGpu> ParticleSpawnCommands : register(t12);
StructuredBuffer<float> GroundGapAngles : register(t11, space3);
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
Texture2D<float4> BloomHalf : register(t22, space3);
Texture2D<float4> BloomQuarter : register(t23, space3);
Texture2D<float4> BloomHalfCombined : register(t24, space3);
Texture2D<float4> TemporalInput : register(t26, space3);
Texture2D<float4> TemporalOutput : register(t27, space3);
Texture2D<float4> UiTexture : register(t11);
Texture2DArray<float4> ArcherDiffuse : register(t14);
Texture2DArray<float4> ArcherNormal : register(t15);
Texture2DArray<float> VfxMasks : register(t16);
Texture2D<float4> VfxGradientLut : register(t1, space3);
Texture2DArray<float> VfxAuthoredMask : register(t7, space3);
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
    nointerpolation uint AnimationClip : TEXCOORD10;
    nointerpolation float AnimationProgress : TEXCOORD11;
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
    // Sampling the charge timeline makes the bubbling reverse with a cancelled draw.
    if (mesh == 12 && ((instance.Mesh >> 8) & 0xff) == 2)
    {
        const float progress = saturate(instance.Padding);
        const float phase = progress * 48.0;
        const float bubble = sin(input.Position.x * 43.0 + phase) *
            sin(input.Position.y * 37.0 - phase * 0.73) *
            sin(input.Position.z * 41.0 + phase * 0.61);
        local_position += local_normal * (0.006 * progress * bubble);
    }
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
    output.AnimationClip = (instance.Mesh >> 8) & 0xff;
    output.AnimationProgress = saturate(instance.Padding);
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

// Both passes share the facial alpha mask: replaced eyes must not leave opaque
// depth behind the jelly, and the new marks must stay solid in the depth pass.
float4 SampleFamilyColor(SceneOutput input)
{
    float4 color = FamilyDiffuse.Sample(MaterialSampler, float3(input.Uv, input.Mesh - 10));
    const bool dead = input.AnimationClip == 4;
    const float strain = input.AnimationClip == 2
        ? smoothstep(0.0, 0.35, input.AnimationProgress) : 0.0;
    if (!dead && strain <= 0.0) return color;
    const float2 authored_uv = float2((input.Uv.x - 0.015) / 0.72, 1.0 - input.Uv.y);
    const bool left_eye = authored_uv.x < 0.5;
    const float2 eye_center = float2(left_eye ? 0.434 : 0.566, 0.573);
    const float2 eye = authored_uv - eye_center;
    const float ellipse = length(eye / float2(0.0365, 0.081));
    const float region_aa = max(fwidth(ellipse), 0.001);
    const float region = 1.0 - smoothstep(1.0 - region_aa, 1.0 + region_aa, ellipse);
    if (region <= 0.0) return color;
    const float2 body_uv = float2((eye_center.x + (left_eye ? -0.09 : 0.09)) * 0.72 + 0.015,
                                1.0 - eye_center.y);
    float4 body = FamilyDiffuse.Sample(MaterialSampler, float3(body_uv, input.Mesh - 10));
    body.a = 0.68;
    const float2 q = eye / float2(0.032, 0.073);
    // Normalized diagonal strokes stay within the original eye's UV island.
    const float stroke_distance = dead ? min(abs(q.y - q.x), abs(q.y + q.x))
        : abs(q.y - (left_eye ? -0.48 : 0.48) * q.x);
    const float stroke_aa = max(fwidth(stroke_distance), 0.01);
    const float stroke = (1.0 - smoothstep(0.15 - stroke_aa, 0.15 + stroke_aa, stroke_distance)) *
        (1.0 - smoothstep(0.70, 0.90, max(abs(q.x), abs(q.y))));
    const float4 expression = lerp(body, float4(0.009, 0.006, 0.012, 1.0), stroke);
    return lerp(color, expression, region * (dead ? 1.0 : strain));
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
        const float4 base_color = family ? SampleFamilyColor(input)
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
float3 VfxLocalLighting(float3 base, float3 normal, float3 world)
{
    float3 illumination = 0.0;
    for (uint index = 0; index < VfxLightCount.x; ++index)
    {
        float3 delta = VfxLightPositionRadius[index].xyz - world;
        float squared_distance = dot(delta, delta);
        float radius = VfxLightPositionRadius[index].w;
        float edge = saturate(1.0 - squared_distance / (radius * radius));
        float diffuse = saturate(dot(normal, delta * rsqrt(max(squared_distance, 0.0001))));
        illumination += VfxLightColorIntensity[index].rgb * VfxLightColorIntensity[index].w *
            diffuse * edge * edge / (1.0 + squared_distance);
    }
    return base * illumination;
}

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
        + lerp(base, float3(0.70, 0.85, 1.0), 0.30) * fresnel * 0.32
        + VfxLocalLighting(base, normal, world);
}

struct VfxDecalGpu
{
    float4 PositionRadius;
    float4 AxisAlphaAge;
    float4 ColorHdr;
    float4 Fracture;
    float4 Slab;
    uint4 Metadata;
};
StructuredBuffer<VfxDecalGpu> VfxDecals : register(t19, space3);
Texture2DArray<float4> VfxFractureDecals : register(t20, space3);
Texture2DArray<float> VfxOrganicDecals : register(t21, space3);

float2 DecalCellRandom(float2 cell, uint seed)
{
    float2 value = float2(dot(cell,float2(127.1,311.7)),dot(cell,float2(269.5,183.3)));
    return frac(sin(value + float(seed%4093)) * 43758.5453);
}
float DecalVoronoiBranch(float2 local, float cell_count, uint seed, float footprint)
{
    const float frequency = sqrt(max(cell_count,1)/3.14159265);
    const float2 p = local*frequency;
    const float2 cell = floor(p);
    float first=1000,second=1000;
    float2 nearest=0;
    [unroll] for(int y=-1;y<=1;++y)
    [unroll] for(int x=-1;x<=1;++x)
    {
        const float2 id=cell+float2(x,y);
        const float2 delta=id+DecalCellRandom(id,seed)-p;
        const float distance=dot(delta,delta);
        if(distance<first){second=first;first=distance;nearest=id;}
        else second=min(second,distance);
    }
    const float edge=sqrt(second)-sqrt(first);
    const float aa=clamp(footprint*frequency,.002,.15);
    const float selection=DecalCellRandom(nearest,seed+71).x;
    const float density=lerp(.85,.22,saturate(length(local)));
    return (1-smoothstep(.035-aa,.035+aa,edge))*step(selection,density);
}

float3 ApplyGroundDecals(float3 world, float2 world_dx, float2 world_dy, inout float3 base, inout float3 normal)
{
    float3 emissive=0;
    // Derivatives come from the uniform deferred path, before material/projector branches.
    const float ground_facing=smoothstep(.45,.8,normal.y);
    if(ground_facing<=0) return emissive;
    uint gradient_width,gradient_rows;
    VfxGradientLut.GetDimensions(gradient_width,gradient_rows);
    [loop] for(uint index=0;index<VfxDecalCount.x;++index)
    {
        const VfxDecalGpu decal=VfxDecals[index];
        if(world.y<decal.Slab.x || world.y>decal.Slab.y) continue;
        const float2 delta=(world.xz-decal.PositionRadius.xz)/decal.PositionRadius.w;
        const float2 axis=decal.AxisAlphaAge.xy;
        const float2 across=float2(-axis.y,axis.x);
        const float2 local=float2(dot(delta,axis),dot(delta,across));
        const float radial=length(local);
        if(radial>=1) continue;
        const float2 uv=local*.5+.5;
        const float2 uv_dx=float2(dot(world_dx,axis),dot(world_dx,across))/(2*decal.PositionRadius.w);
        const float2 uv_dy=float2(dot(world_dy,axis),dot(world_dy,across))/(2*decal.PositionRadius.w);
        const float aa=max(length(uv_dx),length(uv_dy))*2;
        const float footprint=1-smoothstep(max(0.0,1-max(.04,aa)),1.0,radial);
        const float slab=smoothstep(decal.Slab.x,decal.Slab.x+.05,world.y)*(1-smoothstep(decal.Slab.y-.05,decal.Slab.y,world.y));
        const float2 gradient_uv=float2((saturate(decal.AxisAlphaAge.w)*(gradient_width-1)+.5)/gradient_width,
            (decal.Metadata.x+.5)/max(gradient_rows,1));
        const float4 gradient=VfxGradientLut.SampleLevel(LinearClamp,gradient_uv,0);
        const float3 tint=decal.ColorHdr.rgb*gradient.rgb;
        const float alpha=saturate(decal.AxisAlphaAge.z*gradient.a*footprint*slab*ground_facing);
        if(decal.Metadata.z==0)
        {
            const float3 sample_uv=float3(uv,decal.Metadata.y);
            const float4 structure=VfxFractureDecals.SampleGrad(LinearClamp,sample_uv,uv_dx,uv_dy);
            const float branch=DecalVoronoiBranch(local,decal.Fracture.y,decal.Metadata.w,aa);
            // Authored R/G/A remain the structural soot/crack/edge masks. Procedural branches add sparse cell edges.
            const float cracks=saturate(max(structure.g,branch*structure.a*.55));
            const float soot=saturate(structure.r*structure.a);
            const float coverage=alpha*max(soot,cracks);
            base=lerp(base,base*lerp(float3(.3,.3,.3),saturate(tint)*.45,.25),coverage);
            const float cooling=pow(1-saturate(decal.AxisAlphaAge.w),2);
            emissive+=tint*decal.ColorHdr.w*decal.Fracture.x*cracks*alpha*cooling;
            const float height_x=VfxFractureDecals.SampleGrad(LinearClamp,sample_uv+float3(1.0/512,0,0),uv_dx,uv_dy).b;
            const float height_z=VfxFractureDecals.SampleGrad(LinearClamp,sample_uv+float3(0,1.0/512,0),uv_dx,uv_dy).b;
            const float2 slope=clamp(float2(height_x-structure.b,height_z-structure.b)*(.003*512/(2*decal.PositionRadius.w)),-.15,.15)*coverage;
            const float2 tangent=axis*slope.x+across*slope.y;
            normal=normalize(normal-float3(tangent.x,0,tangent.y));
        }
        else
        {
            const float mask=VfxOrganicDecals.SampleGrad(LinearClamp,float3(uv,decal.Metadata.y),uv_dx,uv_dy);
            const float coverage=alpha*mask;
            // Organic residue changes the lit base; its authored low HDR remains a weak emissive contribution.
            base=lerp(base,base*lerp(float3(.12,.12,.12),saturate(tint),.55),coverage);
            emissive+=tint*decal.ColorHdr.w*coverage;
        }
    }
    return emissive;
}

float4 DeferredPS(FullScreenOutput input) : SV_Target0
{
    float4 base = GBufferBase.SampleLevel(LinearClamp, input.Uv, 0);
    float4 normal_material = GBufferNormal.SampleLevel(LinearClamp, input.Uv, 0);
    float3 normal = normalize(normal_material.xyz * 2.0 - 1.0);
    float3 world = GBufferPosition.SampleLevel(LinearClamp, input.Uv, 0).xyz;
    float4 environment_material = GBufferMaterial.SampleLevel(LinearClamp,input.Uv,0);
    const float2 decal_world_dx=ddx(world.xz),decal_world_dy=ddy(world.xz);
    if (environment_material.a > 0.5)
    {
        const float3 decal_emissive=ApplyGroundDecals(world,decal_world_dx,decal_world_dy,base.rgb,normal);
        return float4(EnvironmentLighting(base.rgb,normal,world,environment_material,ShadowVisibility(world)) + VfxLocalLighting(base.rgb,normal,world)+decal_emissive,1);
    }
    if (normal_material.a >= 2.0)
        return float4(JellyLighting(base.rgb, normal, world, normal_material.a - 2.0, 0.0), 1.0);
    float3 light = normalize(-LightDirectionIntensity.xyz);
    float diffuse = dot(normal, light);
    float stepped_diffuse = diffuse > 0.55 ? 1.0 : (diffuse > 0.05 ? 0.62 : 0.28);
    float rim = pow(1.0 - saturate(normal.y), 3.0);
    float visibility = base.a > 0.0 ? lerp(0.42, 1.0, ShadowVisibility(world)) : 1.0;
    float3 color = base.rgb *
                       (0.35 + stepped_diffuse * visibility * LightColor.rgb) +
                   base.rgb * rim * 0.22 + VfxLocalLighting(base.rgb, normal, world);
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

float3 BallisticVelocity(float3 velocity, float drag, float gravity, float dt)
{
    return velocity * exp(-drag * dt) + float3(0, gravity * dt, 0);
}
void IntegrateBallisticStep(inout ParticleData particle, float dt)
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
            ParticleCounters.Store(12, 0);
            ParticleCounters.Store(16, 0);
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
            ParticleCounters.Store(12, 0);
            ParticleCounters.Store(16, 0);
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
        float age = command.Authored.x != 0
            ? max(0.0, (float(AuthoredParticleClock.x-command.Authored.z)-asfloat(command.Authored.y))/60.0)
            : asfloat(command.Metadata.w);
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
            particle.PositionLife.y = max(particle.PositionLife.y, command.CollisionMaterial.y + command.CollisionMaterial.z);
            particle.CurrentVelocityDrag = command.CurrentVelocityDrag;
            particle.CollisionMaterial = command.CollisionMaterial;
            particle.Authored = uint4(1, command.Authored.z, command.Authored.z, command.Authored.w);
            CatchUpBallistic(particle, AuthoredParticleClock.x);
        }
        WritableParticles[particle_index] = particle;

        uint output_index;
        ParticleCounters.InterlockedAdd(4, 1, output_index);
        AliveOutput[output_index] = particle_index;
        return;
    }

    if (PassValue == 4)
    {
        if (index >= ParticleCounters.Load(4)) return;
        uint particle_index = AliveOutput[index];
        uint metadata = asuint(WritableParticles[particle_index].PhysicsMetadata.z);
        uint bin = ((metadata >> 8u) & 0xffu) == 3u ? 1u : 0u;
        uint draw_index;
        ParticleCounters.InterlockedAdd(12 + bin * 4, 1, draw_index);
        AliveOutput[capacity * (1 + bin) + draw_index] = particle_index;
        return;
    }

    if (PassValue == 5 && index == 0)
    {
        uint alive_count = ParticleCounters.Load(4);
        ParticleCounters.Store(0, alive_count);
        uint vertex_count = 6;
        [unroll] for (uint mesh = 0; mesh < 7; ++mesh)
            vertex_count = max(vertex_count, VfxMeshAtlas.Load(24 + mesh * 12));
        IndirectArguments.Store4(0, uint4(6, ParticleCounters.Load(12), 0, 0));
        IndirectArguments.Store4(16, uint4(vertex_count, ParticleCounters.Load(16), 0, 0));
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
    nointerpolation float3 GroundMotion : TEXCOORD11;
    nointerpolation float2 GroundGradient : TEXCOORD12;
    nointerpolation float4 RingGeometry : TEXCOORD13;
    nointerpolation uint2 GapRange : TEXCOORD14;
    nointerpolation float3 PreviewTiming : TEXCOORD15;
    nointerpolation uint AuthoredBallistic : TEXCOORD16;
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
    ParticleData particle = Particles[DrawAliveIndices[ParticleOptions.x * (1 + PassValue) + instance_id]];
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
        float3 current_velocity = particle.Authored.x != 0 ? particle.CurrentVelocityDrag.xyz :
            particle.InitialVelocityMaxLife.xyz + float3(0, particle.PhysicsMetadata.x * elapsed, 0);
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
        // Keep the legacy ShockShell geometry until its dedicated surface-shell profile is implemented.
        uint selector = visual_metadata >> 24u;
        bool legacy_shell = selector == 0u && primitive == 11u;
        uint mesh = selector == 0u ? (primitive == 7u ? 0u : primitive == 10u ? 6u : 1u) : selector - 1u;
        float3 local;
        float3 local_normal;
        float2 mesh_uv;
        if (legacy_shell)
        {
            uint source_vertex = vertex_id < 24u ? vertex_id : 0u;
            uint3 face = OctahedronFaces[source_vertex / 3u];
            uint corner_index = source_vertex % 3u;
            uint vertex = corner_index == 0u ? face.x : corner_index == 1u ? face.y : face.z;
            local = OctahedronVertices[vertex];
            local_normal = normalize(local);
            mesh_uv = local.xy * 0.5 + 0.5;
        }
        else
        {
            uint first = VfxMeshAtlas.Load(20 + mesh * 12);
            uint count = VfxMeshAtlas.Load(24 + mesh * 12);
            uint source_vertex = vertex_id < count ? vertex_id : 0u;
            uint address = 100 + (first + source_vertex) * 48;
            local = asfloat(VfxMeshAtlas.Load3(address));
            local_normal = asfloat(VfxMeshAtlas.Load3(address + 12));
            mesh_uv = asfloat(VfxMeshAtlas.Load2(address + 24));
        }
        local.xy = float2(local.x * cosine - local.y * sine,
                          local.x * sine + local.y * cosine);
        local_normal.xy = float2(local_normal.x * cosine - local_normal.y * sine,
                                 local_normal.x * sine + local_normal.y * cosine);
        if (particle.Authored.x != 0) local.z -= 0.5;
        float3 velocity = particle.Authored.x != 0 ? particle.CurrentVelocityDrag.xyz :
            particle.InitialVelocityMaxLife.xyz + float3(0, particle.PhysicsMetadata.x * elapsed, 0);
        float3 forward = length(velocity) > 0.0001 ? normalize(velocity) : float3(0, 1, 0);
        float3 reference = abs(forward.y) < 0.95 ? float3(0, 1, 0) : float3(1, 0, 0);
        float3 mesh_x = normalize(cross(reference, forward));
        float3 mesh_y = normalize(cross(forward, mesh_x));
        float3 scaled_local = local * size;
        world = particle.PositionLife.xyz + mesh_x * scaled_local.x +
                mesh_y * scaled_local.y + forward * scaled_local.z;
        normal = normalize(mesh_x * local_normal.x + mesh_y * local_normal.y + forward * local_normal.z);
        corner = mesh_uv * 2.0 - 1.0;
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
    // ExactRing stores gameplay-radius / quad-half-extent in the otherwise
    // unused Ground stretch lane. Segment keeps its authored repeat/scroll.
    output.SegmentMetadata = renderer == 1u && primitive == 18u
        ? float2(particle.PhysicsMetadata.y, 0.0) : particle.SizeRotation.zw;
    output.GroundMotion = 0.0;
    output.GroundGradient = particle.Authored.x != 0 ? float2(particle.Authored.w, particle.CollisionMaterial.w) : 0.0;
    output.RingGeometry = 0.0;
    output.GapRange = 0;
    output.PreviewTiming = 0.0;
    output.AuthoredBallistic = particle.Authored.x;
    return output;
}

// Persistent ground outputs are drawn directly from the frame's command upload.
// They do not consume a slot in the simulated particle pool.
ParticleOutput GroundRingVS(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    static const float2 corners[6] = {
        float2(-1, -1), float2(-1, 1), float2(1, 1),
        float2(-1, -1), float2(1, 1), float2(1, -1)
    };
    const ParticleSpawnCommandGpu command = ParticleSpawnCommands[instance_id];
    const float2 corner = corners[vertex_id];
    const float extent = command.SizeRange.x;
    const bool is_annulus = (command.Metadata.z >= 5 && command.Metadata.z <= 8) || command.Metadata.z == 10 || command.Metadata.z == 11;
    const bool is_safe_sector = command.Metadata.z == 9;
    const bool is_hex = command.Metadata.z == 12;
    const bool is_authored_additive = command.Metadata.z >= 13 && command.Metadata.z <= 18;
    const bool is_axial_fracture = command.Metadata.z == 15;
    const bool is_repeating_chevron = command.Metadata.z == 16;
    const bool is_state_ring = command.Metadata.z == 19;
    const bool is_polar_rune = command.Metadata.z == 20;
    const bool is_line = command.Metadata.z > 0 && command.Metadata.z <= 4;
    const bool is_cone = command.Metadata.z == 3 || command.Metadata.z == 4;
    const float2 half_extent = is_safe_sector ? command.ShapeExtentSpeedMin.yy : is_annulus ? (command.ShapeExtentSpeedMin.y + command.ShapeExtentSpeedMin.z).xx : is_axial_fracture
        ? float2(max(command.ShapeExtentSpeedMin.x, command.ShapeExtentSpeedMin.z * 3.0),
                 command.ShapeExtentSpeedMin.y * 0.5 + command.ShapeExtentSpeedMin.z) : is_repeating_chevron
        ? command.ShapeExtentSpeedMin.xy + command.ShapeExtentSpeedMin.z : is_line
        ? (is_cone ? command.ShapeExtentSpeedMin.xx : command.ShapeExtentSpeedMin.xy) + command.ShapeExtentSpeedMin.z : extent.xx;
    const float2 local = corner * half_extent;
    const float3 forward = (is_line || is_safe_sector || is_axial_fracture || is_repeating_chevron)
        ? command.DirectionLifetimeMax.xyz : float3(0,0,1);
    const float3 right = float3(forward.z,0,-forward.x);
    const float3 world = command.PositionLifetimeMin.xyz + right * local.x + forward * local.y;

    ParticleOutput output;
    output.Position = mul(float4(world, 1.0), ViewProjection);
    output.Uv = corner * 0.5 + 0.5;
    output.LocalUv = (is_line || is_annulus || is_safe_sector || is_hex || is_authored_additive || is_state_ring || is_polar_rune) ? local : corner;
    output.Color = command.StartColor;
    output.WorldPosition = world;
    output.Facing = 0u;
    output.Sprite = 0u;
    output.Progress = 0.0;
    output.FrameGrid = uint2(1u, 1u);
    output.Normal = float3(0.0, 1.0, 0.0);
    output.Renderer = 1u;
    output.Primitive = command.Metadata.z != 0 ? 19u + command.Metadata.z : command.Metadata.x;
    output.SegmentMetadata = is_line ? command.SpeedConeGravityStretch.xy : float2(command.SpeedConeGravityStretch.w, 0.0);
    output.GroundMotion = is_line ? command.ShapeExtentSpeedMin.xyz : command.RotationRange.xyz;
    output.GroundGradient = float2(command.Metadata.y, command.SizeRange.y);
    output.RingGeometry = (is_annulus || is_safe_sector || is_hex || is_authored_additive || is_state_ring || is_polar_rune)
        ? command.ShapeExtentSpeedMin : 0.0;
    output.GapRange = (is_annulus || is_safe_sector || is_hex || is_repeating_chevron || is_state_ring || is_polar_rune)
        ? command.Modes.xy : uint2(0, 0);
    output.PreviewTiming = command.EndColor.xyz;
    output.AuthoredBallistic = 0;
    return output;
}

// Authored owner-follow meshes use the same transient command upload as the
// persistent ground outputs.  The packet's direction is a velocity in metres
// per second; only its orientation is used here so the owner position remains
// authoritative on the CPU side.
ParticleOutput OwnerMeshVS(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    const ParticleSpawnCommandGpu command = ParticleSpawnCommands[PassValue];
    const uint mesh = min(max(command.Metadata.x, 1u), 7u) - 1u;
    const uint first = VfxMeshAtlas.Load(20 + mesh * 12);
    const uint count = VfxMeshAtlas.Load(24 + mesh * 12);
    const uint source_vertex = count > 0u ? min(vertex_id, count - 1u) : 0u;
    const uint address = 100 + (first + source_vertex) * 48;
    const float3 local = asfloat(VfxMeshAtlas.Load3(address));
    const float3 local_normal = asfloat(VfxMeshAtlas.Load3(address + 12));
    const float2 mesh_uv = asfloat(VfxMeshAtlas.Load2(address + 24));

    const float3 forward = length(command.DirectionLifetimeMax.xyz) > 0.0001
        ? normalize(command.DirectionLifetimeMax.xyz) : float3(0, 0, 1);
    const float3 reference = abs(forward.y) < 0.95 ? float3(0, 1, 0) : float3(1, 0, 0);
    const float3 axis_x = normalize(cross(reference, forward));
    const float3 axis_y = normalize(cross(forward, axis_x));
    const float3 scale = max(command.ShapeExtentSpeedMin.xyz, float3(0.00001, 0.00001, 0.00001));
    const float3 world = command.PositionLifetimeMin.xyz +
        axis_x * (local.x * scale.x) + axis_y * (local.y * scale.y) +
        forward * ((local.z - 0.5) * scale.z);
    const float3 normal = normalize(axis_x * (local_normal.x / scale.x) + axis_y * (local_normal.y / scale.y) +
                                    forward * (local_normal.z / scale.z));

    ParticleOutput output;
    output.Position = mul(float4(world, 1.0), ViewProjection);
    output.Uv = mesh_uv;
    output.LocalUv = mesh_uv * 2.0 - 1.0;
    output.Color = command.StartColor;
    output.WorldPosition = world;
    output.Facing = 0u;
    output.Sprite = 0u;
    output.Progress = saturate(1.0 - local.z);
    output.FrameGrid = uint2(1u, 1u);
    output.Normal = normal;
    output.Renderer = 4u;
    output.Primitive = mesh;
    output.SegmentMetadata = float2(command.RotationRange.x, 0.0);
    output.GroundMotion = 0.0;
    output.GroundGradient = float2(command.Metadata.y, command.SizeRange.y);
    output.RingGeometry = 0.0;
    output.GapRange = 0;
    output.PreviewTiming = 0.0;
    output.AuthoredBallistic = 0;
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
        base = SampleFamilyColor(input);
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
    const float alpha = base.a * 0.52;
    const float z = saturate(length(input.WorldPosition - CameraTime.xyz) / 180.0);
    const float weight = clamp(pow(min(1.0, alpha * 10.0) + 0.01, 3.0) *
        1e8 * pow(1.0 - z * 0.9, 3.0), 1e-2, 3e3);
    OitOutput output;
    output.Accumulation = float4(color * alpha * weight, alpha * weight);
    output.Revealage = alpha.xxxx;
    return output;
}

float4 SampleVfxGradient(float row, float coordinate)
{
    uint width, rows;
    VfxGradientLut.GetDimensions(width, rows);
    // Texel-centred rows prevent style bleeding; X interpolates authored stops.
    const float2 uv = float2((saturate(coordinate) * (width - 1) + 0.5) / width,
                             (row + 0.5) / rows);
    return VfxGradientLut.SampleLevel(LinearClamp, uv, 0);
}

float4 GroundGradient(ParticleOutput input, float coordinate)
{
    const float4 gradient = SampleVfxGradient(input.GroundGradient.x, coordinate);
    return float4(gradient.rgb * input.GroundGradient.y,
                  gradient.a * input.Color.a);
}

#include "vfx_ribbon.hlsli"

float GroundSectorDistance(float2 p, float radius, float half_angle)
{
    const float radial = length(p);
    if (half_angle >= 3.14159265) return radial-radius;
    const float angle = abs(atan2(p.x,p.y));
    const float2 edge = float2(sin(half_angle),cos(half_angle));
    const float2 mirrored = float2(abs(p.x),p.y);
    const float edge_distance = length(mirrored-edge*clamp(dot(mirrored,edge),0.0,radius));
    const float arc_distance = angle <= half_angle ? abs(radial-radius) : length(mirrored-edge*radius);
    const bool inside = radial <= radius && angle <= half_angle;
    return min(edge_distance,arc_distance)*(inside ? -1.0 : 1.0);
}

float GroundMarkerSegmentDistance(float2 p, float2 a, float2 b)
{
    const float2 segment = b - a;
    return length(p - a - segment * saturate(dot(p - a, segment) / max(dot(segment, segment), 0.000001)));
}

float GroundExactRingMask(ParticleOutput input, out float gradient_coordinate)
{
    // ExactRing stores gameplay radius / quad half extent in SegmentMetadata.
    const float radius = length(input.LocalUv);
    const float hitbox_radius = input.SegmentMetadata.x;
    const float half_width = (1.0 - hitbox_radius) * 0.5;
    const float aa = max(fwidth(radius), 0.0001) * 0.5;
    float mask = 1.0 - smoothstep(half_width - aa, half_width + aa,
                                  abs(radius - hitbox_radius));
    const float phase = frac(CameraTime.w * input.GroundMotion.x);
    const float pulse_radius = hitbox_radius * (1.0 - phase * input.GroundMotion.z);
    const float inward_band = 1.0 - smoothstep(half_width - aa, half_width + aa,
                                               abs(radius - pulse_radius));
    const float inside = 1.0 - smoothstep(hitbox_radius - half_width - aa,
                                         hitbox_radius - half_width, radius);
    mask = max(mask, inward_band * inside * input.GroundMotion.y * (1.0 - phase));
    float distance_to_band = abs(radius - hitbox_radius);
    if (inside * input.GroundMotion.y > 0.0)
        distance_to_band = min(distance_to_band, abs(radius - pulse_radius));
    gradient_coordinate = distance_to_band / max(half_width, 0.00001);
    return mask;
}

float GroundHexConstellationMask(ParticleOutput input, out float gradient_coordinate)
{
    const float progress = input.PreviewTiming.x;
    const float collapse = smoothstep(0.70, 1.0, progress);
    const float radius = input.RingGeometry.y * (1.0 - collapse);
    const float half_width = input.RingGeometry.z * 0.5;
    const float aa = max(fwidth(input.LocalUv.x) + fwidth(input.LocalUv.y), 0.0001) * 0.5;
    float mask = 0.0;
    float distance_to_edge = 1000000.0;
    [unroll] for (uint segment = 0; segment < 6; ++segment)
    {
        const float angle = float(segment) * 1.0471975512;
        const float2 a = radius * float2(cos(angle), sin(angle));
        const float2 b = radius * float2(cos(angle + 1.0471975512),
                                          sin(angle + 1.0471975512));
        const float lit = input.PreviewTiming.y > 0.5
            ? saturate(progress * (6.0 / 0.70) - float(segment))
            : saturate(progress * 8.0);
        if (lit > 0.0)
        {
            const float distance = GroundMarkerSegmentDistance(
                input.LocalUv, a, lerp(a, b, lit));
            mask = max(mask, 1.0 - smoothstep(half_width - aa,
                                              half_width + aa, distance));
            distance_to_edge = min(distance_to_edge, distance);
        }
    }
    gradient_coordinate = distance_to_edge / max(half_width, 0.00001);
    return mask * (1.0 - collapse);
}

float GroundBossSignatureMask(ParticleOutput input, out float gradient_coordinate)
{
    const float accent_time = smoothstep(0.0, 0.12, input.PreviewTiming.x) *
        (1.0 - smoothstep(0.65, 1.0, input.PreviewTiming.x));
    if (input.Primitive == 18)
    {
        // Keep the gameplay circle on the same exact-ring SDF as other ground rings.
        float border_coordinate;
        const float border = GroundExactRingMask(input, border_coordinate);
        const float radius = length(input.LocalUv);
        const float gameplay_radius = input.SegmentMetadata.x;
        const float half_width = (1.0 - gameplay_radius) * 0.5;
        const float aa = max(fwidth(radius), 0.0001) * 0.5;
        const float accent_distance = abs(radius - gameplay_radius * 0.78);
        const float accent = (1.0 - smoothstep(half_width * 0.5 - aa,
            half_width * 0.5 + aa, accent_distance)) *
            (1.0 - smoothstep(gameplay_radius - aa, gameplay_radius, radius));
        gradient_coordinate = min(border_coordinate,
            accent_distance / max(half_width, 0.00001));
        return max(border, accent * 0.45 * accent_time);
    }
    if (input.Primitive == 22)
    {
        const float range = input.GroundMotion.x;
        const float width = input.GroundMotion.z;
        const float distance = GroundSectorDistance(input.LocalUv, range,
                                                     input.GroundMotion.y);
        const float aa = max(fwidth(distance), 0.0001);
        const float border = 1.0 - smoothstep(width * 0.5 - aa,
                                               width * 0.5 + aa, abs(distance));
        const float accent_distance = abs(distance + min(range * 0.14, width * 3.0));
        const float accent = (1.0 - smoothstep(width * 0.5 - aa,
            width * 0.5 + aa, accent_distance)) *
            (1.0 - smoothstep(-aa, aa, distance));
        gradient_coordinate = min(abs(distance), accent_distance) / max(width, 0.00001);
        return max(border, accent * 0.45 * accent_time);
    }

    const float radius = length(input.LocalUv);
    const float inner = input.RingGeometry.x;
    const float outer = input.RingGeometry.y;
    const float width = input.RingGeometry.z;
    const float distance_to_border = inner > 0.0
        ? min(radius - inner, outer - radius) : outer - radius;
    gradient_coordinate = distance_to_border / max(width, 0.00001);
    if (radius < inner || radius > outer) return 0.0;
    const float aa = max(fwidth(radius), 0.0001);
    const float angle = atan2(input.LocalUv.y, input.LocalUv.x);
    float gap_coverage = 1.0;
    for (uint gap = 0; gap < input.GapRange.y; ++gap)
    {
        const float delta = abs(atan2(sin(angle - GroundGapAngles[input.GapRange.x + gap]),
                                      cos(angle - GroundGapAngles[input.GapRange.x + gap])));
        const float gap_distance = (delta - input.RingGeometry.w) * radius;
        if (gap_distance < 0.0) return 0.0;
        gap_coverage = min(gap_coverage, saturate(gap_distance / aa));
    }
    const float inside = saturate(distance_to_border / aa);
    const float border = 1.0 - smoothstep(width * 0.5 - aa,
                                           width * 0.5 + aa, distance_to_border);
    const float accent_distance = abs(radius - (inner + outer) * 0.5);
    const float accent = 1.0 - smoothstep(width * 0.5 - aa,
                                          width * 0.5 + aa, accent_distance);
    gradient_coordinate = min(distance_to_border, accent_distance) / max(width, 0.00001);
    return max(border, accent * 0.45 * accent_time) * inside * gap_coverage;
}

float GroundCrossRingMask(ParticleOutput input, out float gradient_coordinate)
{
    const float2 p = input.LocalUv;
    const float radius = input.RingGeometry.y;
    const float edge = input.RingGeometry.z;
    const float progress = input.PreviewTiming.x;
    const float contraction = smoothstep(0.0, 0.62, progress);
    const float release = smoothstep(0.62, 1.0, progress);
    const float ring_radius = lerp(lerp(radius, radius * 0.48, contraction),
                                   radius * 0.82, release);
    const float radial = length(p);
    const float aa = max(fwidth(radial), 0.0001);
    const float ring_distance = abs(radial - ring_radius);
    const float ring = 1.0 - smoothstep(edge * 0.5 - aa,
                                        edge * 0.5 + aa, ring_distance);
    const float2 q = abs(p);
    const float half_bar = edge * 0.9;
    const float arm = radius * 0.32;
    const float vertical = max(q.x - half_bar, q.y - arm);
    const float horizontal = max(q.y - half_bar, q.x - arm);
    const float cross_distance = min(vertical, horizontal);
    const float cross = (1.0 - smoothstep(-aa, aa, cross_distance)) *
        smoothstep(0.08, 0.28, progress);
    gradient_coordinate = min(ring_distance, abs(cross_distance)) /
        max(edge, 0.00001);
    return max(ring, cross * 0.72) * (1.0 - smoothstep(radius, radius + aa, radial));
}

float GroundBrokenHexMask(ParticleOutput input, out float gradient_coordinate)
{
    const float2 p = input.LocalUv;
    const float gameplay_radius = input.RingGeometry.y;
    const float edge = input.RingGeometry.z;
    const float break_half_angle = input.RingGeometry.w;
    const float break_direction = input.PreviewTiming.y;
    const float snap = smoothstep(0.0, 0.24, input.PreviewTiming.x);
    const float hex_radius = gameplay_radius * (1.0 - 0.12 * snap);
    const float radial = length(p);
    const float aa = max(fwidth(p.x) + fwidth(p.y), 0.0001) * 0.5;
    gradient_coordinate = 1.0;
    if (radial > gameplay_radius) return 0.0;
    const float angle_to_break = atan2(sin(atan2(p.y, p.x) - break_direction),
                                       cos(atan2(p.y, p.x) - break_direction));
    float hex_distance = 1000000.0;
    [unroll] for (uint segment = 0; segment < 6; ++segment)
    {
        const float angle = float(segment) * 1.0471975512;
        const float2 a = hex_radius * float2(cos(angle), sin(angle));
        const float2 b = hex_radius * float2(cos(angle + 1.0471975512),
                                              sin(angle + 1.0471975512));
        hex_distance = min(hex_distance, GroundMarkerSegmentDistance(p, a, b));
    }
    const float break_clip = smoothstep(break_half_angle - aa / max(radial, 0.01),
                                         break_half_angle + aa / max(radial, 0.01),
                                         abs(angle_to_break));
    const float border = (1.0 - smoothstep(edge * 0.5 - aa,
                                            edge * 0.5 + aa, hex_distance)) * break_clip;
    const float2 crack_a = gameplay_radius * 0.70 *
        float2(cos(break_direction - break_half_angle),
               sin(break_direction - break_half_angle));
    const float2 crack_b = gameplay_radius * 0.96 *
        float2(cos(break_direction - break_half_angle),
               sin(break_direction - break_half_angle));
    const float2 crack_c = gameplay_radius * 0.70 *
        float2(cos(break_direction + break_half_angle),
               sin(break_direction + break_half_angle));
    const float2 crack_d = gameplay_radius * 0.96 *
        float2(cos(break_direction + break_half_angle),
               sin(break_direction + break_half_angle));
    const float crack_distance = min(GroundMarkerSegmentDistance(p, crack_a, crack_b),
                                     GroundMarkerSegmentDistance(p, crack_c, crack_d));
    const float fracture = (1.0 - smoothstep(edge * 0.35 - aa,
                                              edge * 0.35 + aa, crack_distance)) * snap;
    gradient_coordinate = min(hex_distance, crack_distance) / max(edge, 0.00001);
    return max(border, fracture * 0.85);
}

float GroundAxialFractureMask(ParticleOutput input, out float gradient_coordinate)
{
    const float2 p = input.LocalUv;
    const float hitbox_radius = input.RingGeometry.x;
    const float length_world = input.RingGeometry.y;
    const float width = input.RingGeometry.z;
    const float extent = length_world * 0.5 *
        lerp(0.55, 1.0, smoothstep(0.0, 0.32, input.PreviewTiming.x));
    const float offset = min(hitbox_radius * 0.32, length_world * 0.08);
    const float2 a = float2(-offset * 0.5, -extent);
    const float2 b = float2(offset, -extent * 0.25);
    const float2 c = float2(-offset, extent * 0.35);
    const float2 d = float2(offset * 0.35, extent);
    float distance = min(GroundMarkerSegmentDistance(p, a, b),
                         GroundMarkerSegmentDistance(p, b, c));
    distance = min(distance, GroundMarkerSegmentDistance(p, c, d));
    const float2 branch_end = float2(min(hitbox_radius, width * 2.0), extent * 0.42);
    const float branch = GroundMarkerSegmentDistance(p, c, branch_end);
    const float aa = max(fwidth(p.x) + fwidth(p.y), 0.0001) * 0.5;
    const float core = 1.0 - smoothstep(width * 0.5 - aa,
                                         width * 0.5 + aa, distance);
    const float branch_mask = (1.0 - smoothstep(width * 0.3 - aa,
                                                width * 0.3 + aa, branch)) * 0.55;
    gradient_coordinate = min(distance, branch) / max(width, 0.00001);
    return max(core, branch_mask);
}

float GroundRepeatingChevronMask(ParticleOutput input, out float gradient_coordinate)
{
    const float2 p = input.LocalUv;
    const float half_width = input.RingGeometry.x;
    const float half_length = input.RingGeometry.y;
    const float edge = input.RingGeometry.z;
    const float spacing = input.RingGeometry.w;
    gradient_coordinate = 1.0;
    if (abs(p.x) >= half_width || abs(p.y) >= half_length) return 0.0;
    const float aa = max(fwidth(p.x) + fwidth(p.y), 0.0001) * 0.5;
    float distance = 1000000.0;
    [unroll] for (uint index = 0; index < 7; ++index)
    {
        const float y = half_length - spacing * (float(index) + 0.5);
        const float2 left = float2(-half_width * 0.65, y - spacing * 0.20);
        const float2 crest = float2(0.0, y + spacing * 0.18);
        const float2 right = float2(half_width * 0.65, y - spacing * 0.20);
        distance = min(distance, GroundMarkerSegmentDistance(p, left, crest));
        distance = min(distance, GroundMarkerSegmentDistance(p, crest, right));
    }
    const float lane = saturate(min(half_width - abs(p.x),
                                    half_length - abs(p.y)) / aa);
    const float appear = smoothstep(0.0, 0.18, input.PreviewTiming.x);
    const float dissipate = 1.0 - smoothstep(0.72, 1.0, input.PreviewTiming.x);
    gradient_coordinate = distance / max(edge, 0.00001);
    return (1.0 - smoothstep(edge * 0.5 - aa,
                             edge * 0.5 + aa, distance)) * lane * appear * dissipate;
}

float GroundBrokenCrownMask(ParticleOutput input, out float gradient_coordinate)
{
    const float2 p = input.LocalUv;
    const float inner = input.RingGeometry.x;
    const float outer = input.RingGeometry.y;
    const float width = input.RingGeometry.z;
    const float radial = length(p);
    const float aa = max(fwidth(radial), 0.0001);
    gradient_coordinate = 1.0;
    if (radial < inner || radial > outer) return 0.0;
    const float phase = frac(input.PreviewTiming.y);
    const float angle = atan2(p.y, p.x) + phase * 0.55;
    const float sector = frac(angle * (6.0 / 6.28318530718));
    const float arc = smoothstep(0.10, 0.14, sector) *
        (1.0 - smoothstep(0.82, 0.87, sector));
    const float rim_distance = outer - radial;
    const float rim = 1.0 - smoothstep(width * 0.5 - aa,
                                       width * 0.5 + aa, rim_distance);
    const float pulse = 0.5 + 0.5 * cos(phase * 6.28318530718);
    const float tooth_radius = lerp(outer - width * 1.5, inner + width, pulse);
    const float tooth_distance = abs(radial - tooth_radius);
    const float tooth_angle = abs(sector - 0.5);
    const float tooth = (1.0 - smoothstep(width * 0.5 - aa,
                                          width * 0.5 + aa, tooth_distance)) *
        (1.0 - smoothstep(0.14, 0.27, tooth_angle));
    gradient_coordinate = min(rim_distance, tooth_distance) / max(width, 0.00001);
    return max(rim * arc, tooth * 0.65) *
        saturate(min(radial - inner, outer - radial) / aa);
}

float GroundClosedCrownMask(ParticleOutput input, out float gradient_coordinate)
{
    const float radius = length(input.LocalUv);
    const float boundary = input.RingGeometry.y;
    const float width = input.RingGeometry.z;
    const float aa = max(fwidth(radius), 0.0001);
    const float distance = abs(radius - boundary);
    const float rim = 1.0 - smoothstep(width * 0.5 - aa,
                                       width * 0.5 + aa, distance);
    const float pulse = 0.90 + 0.10 * cos(input.PreviewTiming.y * 6.28318530718);
    gradient_coordinate = distance / max(width, 0.00001);
    return rim * pulse;
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
        if (input.Primitive == 38)
        {
            const float radius = length(p);
            const float inner = input.RingGeometry.x;
            const float outer = input.RingGeometry.y;
            const float center = (inner + outer) * 0.5;
            const float width = outer - inner;
            const float radial_distance = abs(radius - center);
            const float radial_aa = max(fwidth(radius), 0.0001);
            const float ring = 1.0 - smoothstep(width * 0.5 - radial_aa,
                                                 width * 0.5 + radial_aa, radial_distance);
            if (input.GapRange.y == 1)
            {
                // Charge progress reveals the six existing ring segments in order.
                const float coordinate = frac((atan2(p.y, p.x) + 3.14159265359) /
                    6.28318530718) * float(input.GapRange.x);
                const float segment_index = floor(coordinate);
                const float local = frac(coordinate);
                const float lit = saturate(input.PreviewTiming.x *
                    float(input.GapRange.x) - segment_index);
                const float aa = max(fwidth(local), 0.001);
                const float border = smoothstep(0.035 - aa, 0.035 + aa,
                    min(local, 1.0 - local));
                const float reveal = 1.0 - smoothstep(lit - aa, lit + aa, local);
                mask = ring * border * reveal;
            }
            else
            {
                const float angle = atan2(p.y, p.x) + input.PreviewTiming.y *
                    (0.09 * 6.28318530718);
                const float edge = abs(sin(angle * (0.5 * input.GapRange.x)));
                const float angular_aa = max(fwidth(edge), 0.001);
                const float segment = smoothstep(0.18 - angular_aa, 0.18 + angular_aa, edge);
                mask = ring * segment;
            }
            const float4 gradient = SampleVfxGradient(input.GroundGradient.x,
                saturate(radial_distance / max(width * 0.5, 0.0001)));
            input.Color = float4(input.Color.rgb * gradient.rgb,
                                  input.Color.a * gradient.a);
        }
        else if (input.Primitive == 39)
        {
            const float radius = length(p);
            const float outer = input.RingGeometry.y;
            const float edge = input.RingGeometry.z;
            const float aa = max(fwidth(radius), 0.0001);
            const float clip_footprint = 1.0 - smoothstep(outer - aa, outer + aa, radius);
            const float rings = float(input.GapRange.y);
            const float ring_step = outer * 0.42 / max(rings - 1.0, 1.0);
            const float nearest = clamp(round((radius - outer * 0.54) / ring_step),
                                        0.0, rings - 1.0);
            const float ring_distance = abs(radius - (outer * 0.54 + nearest * ring_step));
            const float ring = 1.0 - smoothstep(edge * 0.5 - aa,
                                                edge * 0.5 + aa, ring_distance);
            const float angle = atan2(p.y, p.x) - input.PreviewTiming.y;
            const float spoke_distance = abs(sin(angle * (0.5 * float(input.GapRange.x)))) * radius;
            const float spoke = (1.0 - smoothstep(edge * 0.35 - aa,
                                                  edge * 0.35 + aa, spoke_distance)) *
                                smoothstep(outer * 0.48, outer * 0.58, radius);
            mask = max(ring, spoke * 0.75) * clip_footprint;
            input.Color = GroundGradient(input,
                saturate(min(ring_distance, spoke_distance) / max(edge, 0.0001)));
        }
        else if (input.Primitive == 28)
        {
            const float radius = length(p);
            const float angle = abs(atan2(p.x, p.y));
            const float inner = input.RingGeometry.x;
            const float outer = input.RingGeometry.y;
            const float half_angle = input.RingGeometry.w;
            // Safety geometry remains authoritative: all decoration is clipped to it.
            if (radius < inner || radius > outer || angle > half_angle) discard;
            const float aa = max(fwidth(radius), 0.0001);
            const float radial_clip = saturate(min(radius - inner, outer - radius) / aa);
            const float angular_clip = half_angle >= 3.14159265 ? 1.0 : saturate((half_angle - angle) * radius / aa);
            const float band = outer - inner;
            const float marker_radius = inner + band * 0.55;
            const float2 local = p - float2(0, marker_radius);
            const float size = min(0.30, band * 0.15);
            const float width = input.RingGeometry.z;
            // Fixed double chevrons point outwards along the authoritative direction.
            float arrow_distance = GroundMarkerSegmentDistance(local, float2(-size, -size * 0.3), float2(0, size * 0.7));
            arrow_distance = min(arrow_distance, GroundMarkerSegmentDistance(local, float2(size, -size * 0.3), float2(0, size * 0.7)));
            arrow_distance = min(arrow_distance, GroundMarkerSegmentDistance(local, float2(-size, -size * 1.1), float2(0, -size * 0.1)));
            arrow_distance = min(arrow_distance, GroundMarkerSegmentDistance(local, float2(size, -size * 1.1), float2(0, -size * 0.1)));
            const float arrow = 1.0 - smoothstep(width * 0.5, width * 0.5 + aa, arrow_distance);
            const float decoration_radius = size * 1.35;
            const float local_radius = length(local);
            const float local_angle = atan2(local.y, local.x) + input.PreviewTiming.y;
            const float spoke_spacing = 6.28318530718 / float(input.GapRange.x);
            const float spoke_delta = abs(frac(local_angle / spoke_spacing + 0.5) - 0.5) * spoke_spacing;
            const float spoke_distance = abs(sin(spoke_delta)) * local_radius;
            const float spokes = (1.0 - smoothstep(width * 0.2, width * 0.2 + aa, spoke_distance)) *
                smoothstep(size * 0.25, size * 0.4, local_radius) * (1.0 - smoothstep(decoration_radius - aa, decoration_radius, local_radius));
            const float ring_spacing = decoration_radius / float(input.GapRange.y);
            const float nearest_ring = clamp(round(local_radius / ring_spacing), 1.0, float(input.GapRange.y));
            const float ring_distance = abs(local_radius - nearest_ring * ring_spacing);
            const float rings = 1.0 - smoothstep(width * 0.2, width * 0.2 + aa, ring_distance);
            mask = max(arrow, max(spokes, rings) * 0.16) * radial_clip * angular_clip;
            input.Color = GroundGradient(input, saturate(local_radius / max(decoration_radius, 0.0001)));
        }
        else if (input.Primitive == 26 || input.Primitive == 27 || input.Primitive == 29 || input.Primitive == 30)
        {
            const float radius = length(p);
            const float angle = atan2(p.y, p.x);
            const float start_radius = input.RingGeometry.x;
            const float final_radius = input.RingGeometry.y;
            const float width = input.RingGeometry.z;
            const float aa = max(fwidth(radius), 0.0001);
            // Gaps remain in world space while the small tick offset settles.
            float gap_coverage = 1.0;
            for (uint gap = 0; gap < input.GapRange.y; ++gap)
            {
                const float delta = abs(atan2(sin(angle - GroundGapAngles[input.GapRange.x + gap]),
                                             cos(angle - GroundGapAngles[input.GapRange.x + gap])));
                const float gap_distance = (delta - input.RingGeometry.w) * radius;
                if (gap_distance < 0.0) discard;
                gap_coverage = min(gap_coverage, saturate(gap_distance / aa));
            }
            if (input.Primitive == 26 || input.Primitive == 29)
            {
                const float start_edge = 1.0 - smoothstep(width * 0.5, width * 0.5 + aa, abs(radius - start_radius));
                const float final_edge = 1.0 - smoothstep(width * 0.5, width * 0.5 + aa, abs(radius - final_radius));
                const float arc = angle * final_radius;
                const float dash_distance = abs(frac(arc / 0.55 + 0.5) - 0.5) * 0.55;
                const float dash_aa = max(fwidth(arc), 0.0001);
                const float dashed = 1.0 - smoothstep(0.1375 - dash_aa, 0.1375 + dash_aa, dash_distance);
                const float oscillation = 0.5 + 0.5 * cos(input.PreviewTiming.y * 6.28318530718);
                mask = (input.Primitive == 29 ? final_edge : max(start_edge, final_edge * dashed)) * gap_coverage * (0.6 + 0.4 * oscillation);
                input.Color = GroundGradient(input, saturate((radius - start_radius) / (final_radius - start_radius)));
            }
            else
            {
                const uint tick_count = asuint(input.PreviewTiming.z);
                const float settle = smoothstep(0.0, 0.8, input.PreviewTiming.x);
                const float spacing = 6.28318530718 / float(tick_count);
                const float offset = spacing * 0.18 * (1.0 - settle);
                const float tick_angle = round((angle - offset) / spacing) * spacing + offset;
                const float2 radial = float2(cos(tick_angle), sin(tick_angle));
                const float2 tangent = float2(-radial.y, radial.x);
                // A 20 cm radial bar ends at the final boundary; width is 1.2 cm.
                const float2 local = float2(dot(p, tangent), dot(p, radial) - (final_radius - 0.1));
                const float2 q = abs(local) - float2(0.006, 0.1);
                const float distance = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
                const float tick_aa = max(fwidth(distance), 0.0001);
                mask = (1.0 - smoothstep(-tick_aa, tick_aa, distance)) * gap_coverage;
                input.Color = GroundGradient(input, saturate((radius - start_radius) / (final_radius - start_radius)));
            }
        }
        else if (input.Primitive == 24 || input.Primitive == 25)
        {
            const float radius = length(p);
            const float inner = input.RingGeometry.x;
            const float outer = input.RingGeometry.y;
            const float edge = input.RingGeometry.z;
            const float band = outer - inner;
            const float distance_to_edge = inner > 0.0 ? min(radius - inner, outer - radius) : outer - radius;
            const float aa = max(fwidth(radius), 0.0001);
            // Discard all safe-space pixels. Neither border glow nor body crosses a gap.
            if (radius < inner || radius > outer) discard;
            const float angle = atan2(p.y, p.x);
            float gap_coverage = 1.0;
            for (uint gap = 0; gap < input.GapRange.y; ++gap)
            {
                float delta = abs(atan2(sin(angle - GroundGapAngles[input.GapRange.x + gap]),
                                       cos(angle - GroundGapAngles[input.GapRange.x + gap])));
                float gap_distance = (delta - input.RingGeometry.w) * radius;
                if (gap_distance < 0.0) discard;
                gap_coverage = min(gap_coverage, saturate(gap_distance / aa));
            }
            const float inside = saturate(distance_to_edge / aa) * gap_coverage;
            if (input.Primitive == 24)
            {
                float leading = 1.0 - smoothstep(edge, edge + aa, outer - radius);
                float trailing_edge = inner > 0.0 ? (1.0 - smoothstep(edge, edge + aa, radius - inner)) * 0.45 : 0.0;
                // Authored wavefront keeps a short, 20 cm inward glow behind the leading edge.
                float trailing_glow = 0.35 * exp(-max(0.0, outer - radius - edge) / 0.20);
                float boundary = max(max(leading, trailing_edge), trailing_glow);
                float phase = frac(CameraTime.w * input.GroundMotion.x);
                float pulse_radius = outer - phase * band * input.GroundMotion.z;
                float pulse = 1.0 - smoothstep(edge * 0.5, edge * 0.5 + aa, abs(radius - pulse_radius));
                mask = max(boundary, pulse * input.GroundMotion.y * (1.0 - phase)) * inside;
                input.Color = GroundGradient(input, saturate(distance_to_edge / max(edge, 0.0001)));
            }
            else
            {
                float radial = (radius - inner) / band;
                float phase = CameraTime.w * input.SegmentMetadata.x;
                float2 flow = float2(cos(angle + radial), sin(angle + radial)) * (radial * 2.0 + phase);
                mask = inside * (0.45 + 0.55 * FlameNoise(flow));
                input.Color = GroundGradient(input, radial);
            }
        }
        else if (input.Primitive == 22 || input.Primitive == 23)
        {
            const float range = input.GroundMotion.x;
            const float half_angle = input.GroundMotion.y;
            const float width = input.GroundMotion.z;
            const float distance = GroundSectorDistance(p,range,half_angle);
            const float aa = max(fwidth(distance),0.0001);
            const float inside = 1.0-smoothstep(-aa,aa,distance);
            const float radius = length(p);
            const float angle = atan2(p.x,p.y);
            if (input.Primitive == 22)
            {
                const float border=1.0-smoothstep(width*0.5-aa,width*0.5+aa,abs(distance));
                const float travel=radius+abs(angle)*0.15- CameraTime.w*input.SegmentMetadata.y;
                const float repeat_distance=abs(frac(travel/input.SegmentMetadata.x+0.5)-0.5)*input.SegmentMetadata.x;
                const float pattern_aa=max(fwidth(travel),0.0001);
                const float marks=(1.0-smoothstep(width*0.5-pattern_aa,width*0.5+pattern_aa,repeat_distance))*inside;
                mask=max(border,marks*0.45);
                input.Color=GroundGradient(input,saturate(abs(distance)/max(width,0.0001)));
            }
            else
            {
                const float pattern=(radius/range+angle/(2.0*half_angle)*0.35-CameraTime.w*input.SegmentMetadata.y)*input.SegmentMetadata.x;
                const float phase=abs(frac(pattern+0.5)-0.5);
                const float pattern_aa=max(fwidth(pattern),0.0001);
                mask=inside*(1.0-smoothstep(0.16-pattern_aa,0.16+pattern_aa,phase));
                input.Color=GroundGradient(input,saturate(radius/range));
            }
        }
        else if (input.Primitive == 20 || input.Primitive == 21)
        {
            // Local coordinates and extents are metres: neither endpoint nor
            // width changes when the border/hatch pattern moves.
            const float2 half_extent = input.GroundMotion.xy;
            const float edge_width = input.GroundMotion.z;
            const float2 q = abs(p) - half_extent;
            const float box_distance = length(max(q,0.0)) + min(max(q.x,q.y),0.0);
            const float aa = max(fwidth(box_distance),0.0001);
            const float inside = 1.0 - smoothstep(-aa,aa,box_distance);
            if (input.Primitive == 20)
            {
                const float border = 1.0 - smoothstep(edge_width*0.5-aa,edge_width*0.5+aa,abs(box_distance));
                const float spacing = input.SegmentMetadata.x;
                const float travel = p.y + half_extent.y + abs(p.x)*0.65 - CameraTime.w*input.SegmentMetadata.y;
                const float repeat_distance = abs(frac(travel/spacing+0.5)-0.5)*spacing;
                const float chevron_aa = max(fwidth(travel),0.0001);
                const float chevron = (1.0-smoothstep(edge_width*0.5-chevron_aa,edge_width*0.5+chevron_aa,repeat_distance))*inside;
                mask=max(border,chevron*0.45);
                input.Color=GroundGradient(input,saturate(abs(box_distance)/max(edge_width,0.0001)));
            }
            else
            {
                const float2 uv=p/(2.0*half_extent)+0.5;
                const float pattern=(uv.y+uv.x*0.35-CameraTime.w*input.SegmentMetadata.y)*input.SegmentMetadata.x;
                const float phase=abs(frac(pattern+0.5)-0.5);
                const float hatch_aa=max(fwidth(pattern),0.0001);
                mask=inside*(1.0-smoothstep(0.16-hatch_aa,0.16+hatch_aa,phase));
                input.Color=GroundGradient(input,saturate(abs(p.x)/half_extent.x));
            }
        }
        else if (input.Primitive == 17)
        {
            mask = FlameDensity(p, input.Progress, true);
        }
        else if (input.Primitive == 18)
        {
            float gradient_coordinate;
            mask = GroundExactRingMask(input, gradient_coordinate);
            input.Color = GroundGradient(input, gradient_coordinate);
        }
        else if (input.Primitive == 19)
        {
            // Slow inward flow is cosmetic and stays inside the exact boundary.
            // The authored alpha/HDR keeps the body less prominent than its edge.
            const float radius = length(p);
            const float angle = atan2(p.y, p.x);
            const float phase = CameraTime.w * input.SegmentMetadata.x;
            const float2 flow = float2(cos(angle + radius), sin(angle + radius)) *
                                (radius * 2.0 + phase);
            const float body = 1.0 - smoothstep(0.72, 0.98, radius);
            mask = body * (0.45 + 0.55 * FlameNoise(flow));
            input.Color = GroundGradient(input, radius);
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
    if (input.Renderer == 3 && input.AuthoredBallistic != 0)
    {
        const float4 gradient = SampleVfxGradient(input.GroundGradient.x, input.Progress);
        input.Color = float4(gradient.rgb * input.GroundGradient.y, gradient.a * input.Color.a);
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

float4 GroundAddPS(ParticleOutput input) : SV_Target0
{
    float gradient_coordinate;
    float mask;
    if (input.PreviewTiming.y > 0.5 &&
        (input.Primitive == 18 || input.Primitive == 22 || input.Primitive == 24))
        mask = GroundBossSignatureMask(input, gradient_coordinate);
    else if (input.Primitive == 31)
        mask = GroundHexConstellationMask(input, gradient_coordinate);
    else if (input.Primitive == 32)
        mask = GroundCrossRingMask(input, gradient_coordinate);
    else if (input.Primitive == 33)
        mask = GroundBrokenHexMask(input, gradient_coordinate);
    else if (input.Primitive == 34)
        mask = GroundAxialFractureMask(input, gradient_coordinate);
    else if (input.Primitive == 35)
        mask = GroundRepeatingChevronMask(input, gradient_coordinate);
    else if (input.Primitive == 36)
        mask = GroundBrokenCrownMask(input, gradient_coordinate);
    else if (input.Primitive == 37)
        mask = GroundClosedCrownMask(input, gradient_coordinate);
    else
        mask = GroundExactRingMask(input, gradient_coordinate);
    if (input.Primitive == 31 && input.GapRange.y != 0xffffffffu)
        mask *= lerp(1.0, VfxAuthoredMask.Sample(LinearClamp,
                          float3(input.Uv, input.GapRange.y)), input.RingGeometry.w);
    const float4 color = GroundGradient(input, gradient_coordinate);
    const float alpha = color.a * mask;
    clip(alpha - 0.001);
    return float4(color.rgb * alpha, alpha);
}

OitOutput OwnerMeshPS(ParticleOutput input)
{
    const float4 gradient = SampleVfxGradient(input.GroundGradient.x, saturate(input.Progress));
    const float alpha = input.Color.a * gradient.a;
    clip(alpha - 0.001);

    const float3 normal = normalize(input.Normal);
    const float3 view_direction = normalize(CameraTime.xyz - input.WorldPosition);
    const float diffuse = 0.35 + 0.65 * saturate(dot(normal, normalize(-LightDirectionIntensity.xyz)));
    const float fresnel = pow(1.0 - saturate(abs(dot(normal, view_direction))), 1.5);
    const float lit = diffuse * (1.0 - 0.35 * saturate(input.SegmentMetadata.x)) +
                      fresnel * input.SegmentMetadata.x;
    const float3 color = gradient.rgb * input.GroundGradient.y * lit;
    const float linear_depth = length(input.WorldPosition - CameraTime.xyz);
    const float z = saturate(linear_depth / 180.0);
    const float weight = clamp(pow(min(1.0, alpha * 10.0) + 0.01, 3.0) *
                              1e8 * pow(1.0 - z * 0.9, 3.0), 1e-2, 3e3);
    OitOutput output;
    output.Accumulation = float4(color * alpha * weight, alpha * weight);
    output.Revealage = alpha.xxxx;
    return output;
}

Texture2D<float4> VfxDistortionVectors : register(t15, space3);
Texture2D<float2> VfxFlowCurl : register(t16, space3);
Texture2DArray<float> VfxStbnScalar : register(t17, space3);
struct DistortionGpu
{
    float4 PositionRadius;
    float4 DirectionStrength;
    float4 PhaseAlphaShapeSeed;
    float4 Geometry;
    uint4 GapRange;
};
StructuredBuffer<DistortionGpu> VfxDistortions : register(t18, space3);

float4 CompositePS(FullScreenOutput input) : SV_Target0
{
    const float4 vectors = VfxDistortionVectors.SampleLevel(LinearClamp,input.Uv,0);
    // Normalize overlapping coverage while retaining single-source fade strength.
    float2 offset = clamp(vectors.xy/max(vectors.w,1.0),-0.04,0.04);
    const float2 border = min(input.Uv,1.0-input.Uv);
    offset *= smoothstep(0.0,0.04,min(border.x,border.y));
    float2 warped_uv = saturate(input.Uv + offset);
    // Reject displaced samples which cross foreground geometry in front of the distortion.
    const float valid = GBufferNormal.SampleLevel(LinearClamp,warped_uv,0).a;
    const float3 warped_world = GBufferPosition.SampleLevel(LinearClamp,warped_uv,0).xyz;
    const float warped_depth = dot(warped_world-CameraTime.xyz,CameraForwardSoftness.xyz);
    const float source_depth = vectors.z/max(vectors.w,0.00001);
    const float visibility = valid > 0 && vectors.w > 0 ? saturate((warped_depth-source_depth)/0.35) : 1;
    warped_uv = saturate(input.Uv + offset*visibility);
    float3 opaque = HdrColor.SampleLevel(LinearClamp, warped_uv, 0).rgb;
    float4 accumulation = OitAccumulation.SampleLevel(LinearClamp, input.Uv, 0);
    float revealage = OitRevealage.SampleLevel(LinearClamp, input.Uv, 0);
    float3 transparent = accumulation.rgb / max(accumulation.a, 0.0001);
    return float4(lerp(opaque, transparent, saturate(1.0 - revealage)), 1.0);
}

Texture2D<float4> VfxNoiseBasis : register(t8, space3);
Texture2D<float> VfxCurveLut : register(t9, space3);
Texture2DArray<float4> VfxSmokePositive : register(t12, space3);
Texture2DArray<float4> VfxSmokeNegative : register(t13, space3);
Texture2DArray<float2> VfxSmokeMotion : register(t14, space3);

struct FresnelShellOutput
{
    float4 Position : SV_Position;
    float3 WorldNormal : NORMAL;
    float3 WorldPosition : TEXCOORD0;
    nointerpolation float3 Center : TEXCOORD1;
};

OitOutput FresnelShellPS(FresnelShellOutput input)
{
    const FresnelShellGpu shell = FresnelShellCommands[0];
    const float3 offset = input.WorldPosition - input.Center;
    const float radial = length(offset.xz) / shell.Geometry.x;
    const float3 normal = normalize(input.WorldNormal);
    const float3 view = normalize(CameraTime.xyz - input.WorldPosition);
    const bool crown = shell.Metadata.x == 0;
    const bool player = shell.Metadata.x == 2;
    const float power = crown ? 1.6 : shell.Signature.x;
    const float rim = pow(1.0 - saturate(abs(dot(normal, view))), power);
    const float progress = saturate(shell.Geometry.y);
    // These two cooked motions carry their kind but no numeric motion parameters.
    const float pulse_rate = shell.Motion.x > 0.0 ? shell.Motion.x : 0.8;
    const float pulse = 0.5 + 0.5 * sin(shell.Geometry.z * pulse_rate * 6.2831853);
    float coverage = 1.0;
    if (crown)
    {
        const float angle = atan2(offset.z, offset.x) / 6.2831853;
        const float sector = frac((angle - shell.Geometry.z * shell.Motion.w) * shell.Metadata.y);
        const float sector_aa = max(fwidth(sector), 0.003);
        // Six separate shell sectors, with inward teeth at their rotating edges.
        const float gap = 0.065 + 0.035 * (1.0 - saturate(radial)) + 0.025 * pulse;
        const float edge = min(sector, 1.0 - sector);
        coverage *= smoothstep(gap - sector_aa, gap + sector_aa, edge);
    }
    else if (shell.Metadata.z != 0)
    {
        // The invulnerability shell remains closed until cracks open near its exact end.
        const float opening = smoothstep(0.78, 1.0, progress);
        const float2 noise_uv = input.WorldPosition.xz * 1.7 + float2(offset.y * 0.31, -offset.y * 0.27);
        const float fissure = VfxNoiseBasis.Sample(MaterialSampler, noise_uv).r;
        coverage *= 1.0 - opening * smoothstep(0.70, 0.82, fissure);
    }
    if (player)
    {
        const float noise = VfxNoiseBasis.Sample(MaterialSampler,
            input.WorldPosition.xz * 3.0 + shell.Geometry.z * 0.11).r;
        coverage *= (0.82 + 0.18 * pulse) *
            lerp(1.0, 0.75 + 0.5 * noise, shell.Signature.y);
    }
    const float4 gradient = SampleVfxGradient(shell.Metadata.w, progress);
    const float alpha = saturate(shell.Color.a * gradient.a * coverage * rim);
    clip(alpha - 0.001);
    const float3 color = shell.Color.rgb * gradient.rgb * shell.Geometry.w;
    const float z = saturate(length(input.WorldPosition - CameraTime.xyz) / 180.0);
    const float weight = clamp(pow(min(1.0, alpha * 10.0) + 0.01, 3.0) *
        1e8 * pow(1.0 - z * 0.9, 3.0), 1e-2, 3e3);
    OitOutput output;
    output.Accumulation = float4(color * alpha * weight, alpha * weight);
    output.Revealage = alpha.xxxx;
    return output;
}

float ImpactCurve(uint row, float age)
{
    if (row == 0xffffffff) return 1.0;
    float coordinate = saturate(age) * 255.0;
    uint left = (uint)coordinate;
    return lerp(VfxCurveLut.Load(int3(left, row, 0)),
                VfxCurveLut.Load(int3(min(left + 1, 255), row, 0)), frac(coordinate));
}

float ImpactSegmentDistance(float2 p, float2 a, float2 b)
{
    float2 ab = b - a;
    return length(p - (a + ab * saturate(dot(p - a, ab) / max(dot(ab, ab), 0.00000001))));
}

struct VfxFlashOutput
{
    float4 Position : SV_Position;
    float2 LocalUv : TEXCOORD0;
    float4 Color : COLOR0;
    float3 WorldPosition : TEXCOORD1;
    nointerpolation float3 Gradient : TEXCOORD2;
    nointerpolation uint2 ShapeMask : TEXCOORD3;
    nointerpolation float2 Detail : TEXCOORD4;
    nointerpolation float2 Extent : TEXCOORD5;
    nointerpolation float4 SurfaceData : TEXCOORD6;
    nointerpolation uint4 SurfaceMetadata : TEXCOORD7;
};

VfxFlashOutput VfxFlashVS(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    static const float2 corners[6] = {
        float2(-1, -1), float2(-1, 1), float2(1, 1),
        float2(-1, -1), float2(1, 1), float2(1, -1)
    };
    ParticleSpawnCommandGpu flash = ParticleSpawnCommands[instance_id];
    float2 corner = corners[vertex_id];
    float3 forward = normalize(CameraForwardSoftness.xyz);
    float3 axis_x = normalize(cross(float3(0, 1, 0), forward));
    float3 axis_y = normalize(cross(forward, axis_x));
    uint shape = flash.Metadata.x;
    float age = flash.SizeRange.z;
    float size = flash.SizeRange.x * ImpactCurve(flash.Metadata.z, age);
    float2 aim = float2(dot(flash.DirectionLifetimeMax.xyz, axis_x), dot(flash.DirectionLifetimeMax.xyz, axis_y));
    aim = dot(aim, aim) > 0.000001 ? normalize(aim) : float2(1, 0);
    float3 along = axis_x * aim.x + axis_y * aim.y;
    float3 across = axis_x * -aim.y + axis_y * aim.x;
    float3 center = flash.PositionLifetimeMin.xyz;
    // Motion is independent of coverage shape; stationary flashes have zero initial motion.
    {
        float seconds = age * flash.DirectionLifetimeMax.w;
        float drag = flash.EndColor.w;
        float travel_time = drag > 0.00001 ? (1.0 - exp(-drag * seconds)) / drag : seconds;
        float phase = flash.RotationRange.x;
        float angle = phase + flash.RotationRange.y * seconds;
        center += flash.EndColor.xyz + flash.SpeedConeGravityStretch.xyz * travel_time;
        center.xz += flash.SpeedConeGravityStretch.w *
            (float2(cos(angle), sin(angle)) - float2(cos(phase), sin(phase)));
    }
    if (shape == 1)
    {
        // Two simulation frames to snap out perpendicular to incoming aim.
        size *= lerp(0.2, 1.0, saturate(age * flash.DirectionLifetimeMax.w * 30.0));
        axis_x = across;
        axis_y = along;
    }
    else if (shape == 4)
    {
        axis_x = along;
        axis_y = across;
        center += along * flash.SizeRange.x * 0.3 * age;
    }
    else if (shape == 6)
    {
        size *= lerp(1.0, 0.3, age);
        center.y += flash.SizeRange.x * 0.65 * age;
    }
    else if (shape >= 13 && shape <= 15)
    {
        axis_x = along;
        axis_y = across;
        float seconds = age * flash.DirectionLifetimeMax.w;
        if (shape == 13)
            size *= lerp(0.35, 1.0, saturate(seconds * 30.0));
        else if (shape == 14)
            size *= lerp(1.0, 0.25, saturate((seconds - 1.0 / 60.0) /
                max(flash.DirectionLifetimeMax.w - 1.0 / 60.0, 0.0001)));
    }
    else if (shape == 16)
    {
        axis_x = along;
        axis_y = across;
    }
    const float projected_aim = length(float2(dot(flash.DirectionLifetimeMax.xyz, axis_x),
                                               dot(flash.DirectionLifetimeMax.xyz, axis_y)));
    float2 extent = shape == 16
        ? size * float2(2.5 * max(saturate(projected_aim), 0.25), 0.20)
        : size * float2(flash.ShapeExtentSpeedMin.x, 1.0);
    float2 padded_extent = extent + ((shape == 1 || shape == 3 || shape == 4 ||
                                      (shape >= 13 && shape <= 15)) ? 0.02 : 0.0);
    // Ground bursts are bottom-anchored after their authored size curve.  Centered
    // billboards otherwise put half their geometry below terrain, creating a straight
    // depth-cut edge through the explosion.  Keep ordinary contact flashes centered.
    if (flash.PositionLifetimeMin.w > 0.5)
        center.y += abs(axis_x.y) * padded_extent.x + abs(axis_y.y) * padded_extent.y;
    float3 world = center + axis_x * corner.x * padded_extent.x + axis_y * corner.y * padded_extent.y;
    VfxFlashOutput output;
    output.Position = mul(float4(world, 1.0), ViewProjection);
    output.LocalUv = corner * padded_extent / max(extent, 0.000001);
    output.Color = flash.StartColor;
    output.WorldPosition = world;
    output.Gradient = float3(flash.Metadata.y, flash.SizeRange.yz);
    output.ShapeMask = uint2(shape, flash.Metadata.w);
    output.Detail = flash.ShapeExtentSpeedMin.yz;
    output.Extent = extent;
    output.SurfaceData = float4(flash.RotationRange.z, flash.ShapeExtentSpeedMin.w, flash.RotationRange.w, age * flash.DirectionLifetimeMax.w);
    output.SurfaceMetadata = flash.Modes;
    return output;
}

float4 VfxFlashPS(VfxFlashOutput input) : SV_Target0
{
    const float4 gradient = SampleVfxGradient(input.Gradient.x, input.Gradient.z);
    const uint shape = input.ShapeMask.x;
    input.Color = float4((shape == 16 ? input.Color.rgb : input.Gradient.y) * gradient.rgb,
                         gradient.a * input.Color.a);
    float radius = length(input.LocalUv);
    float coverage = saturate((1.0 - radius) / max(fwidth(radius), 0.025));
    coverage *= saturate(1.0 - radius * radius);
    float2 p = input.LocalUv;
    float age = input.Gradient.z;
    float2 world_local = p * input.Extent;
    if (shape == 1 || shape == 3 || shape == 4 || shape == 6 || (shape >= 13 && shape <= 16))
    {
        float distance;
        if (shape == 1)
        {
            float2 arm = input.Extent * 0.70;
            float2 secondary = input.Extent * float2(0.55, -0.55);
            distance = min(ImpactSegmentDistance(world_local, -arm, arm) - 0.012,
                           ImpactSegmentDistance(world_local, -secondary, secondary) - 0.009);
        }
        else if (shape == 3)
        {
            float frame = ImpactSegmentDistance(abs(world_local), float2(input.Extent.x * 0.7, 0),
                float2(0, input.Extent.y * 0.7)) - 0.014;
            float stem = ImpactSegmentDistance(world_local, float2(0, -0.32 * input.Extent.y),
                float2(0, 0.26 * input.Extent.y)) - 0.012;
            distance = min(frame, stem);
        }
        else if (shape == 4)
        {
            float horizontal = ImpactSegmentDistance(world_local, float2(-0.85 * input.Extent.x, 0),
                float2(0.8 * input.Extent.x, 0)) - 0.018 * (1.0 - saturate(p.x));
            float2 arm = input.Extent * 0.45;
            float2 secondary = arm * float2(1, -1);
            float spikes = min(ImpactSegmentDistance(world_local, -arm, arm),
                ImpactSegmentDistance(world_local, -secondary, secondary)) - 0.009;
            distance = min(horizontal, spikes);
        }
        else if (shape == 13)
        {
            // Three inward teeth retain metre-wide strokes at any camera distance.
            distance = 100.0;
            [unroll] for (uint tooth = 0; tooth < 3; ++tooth)
            {
                float angle = tooth * 2.09439510239;
                float2 radial = float2(cos(angle), sin(angle));
                float2 side = float2(-radial.y, radial.x);
                float2 tip = radial * input.Extent * 0.15;
                float2 heel = radial * input.Extent * 0.72;
                distance = min(distance, min(
                    ImpactSegmentDistance(world_local, heel + side * input.Extent * 0.22, tip),
                    ImpactSegmentDistance(world_local, heel - side * input.Extent * 0.22, tip)) - 0.009);
            }
        }
        else if (shape == 14)
        {
            float2 crest = abs(world_local);
            float frame = ImpactSegmentDistance(crest, float2(input.Extent.x * 0.86, 0),
                float2(input.Extent.x * 0.05, input.Extent.y * 0.60)) - 0.012;
            float shaft = ImpactSegmentDistance(world_local, float2(-0.9 * input.Extent.x, 0),
                float2(0.92 * input.Extent.x, 0)) - 0.008;
            distance = min(frame, shaft);
        }
        else if (shape == 15)
        {
            float split = smoothstep(0.0, 0.35, age);
            float2 q = float2(abs(world_local.x), world_local.y);
            float axial = ImpactSegmentDistance(q, float2(split * input.Extent.x * 0.2, 0),
                float2(input.Extent.x * 0.92, 0)) - 0.006;
            float arms = ImpactSegmentDistance(abs(world_local), input.Extent * float2(0.04, 0.62),
                input.Extent * float2(0.38, 0.04)) - 0.007;
            distance = min(axial, arms);
        }
        else if (shape == 16)
        {
            const float taper = smoothstep(-0.9, 0.7, p.x);
            const float half_width = input.Extent.y * lerp(0.25, 0.9, taper);
            distance = ImpactSegmentDistance(world_local,
                float2(-input.Extent.x * 0.9, 0),
                float2(input.Extent.x * 0.75, 0)) - half_width;
        }
        else
        {
            float angle = atan2(p.y, p.x);
            float star_radius = 0.56 + 0.2 * cos(angle * 5.0);
            distance = min(radius - star_radius * 0.68, abs(radius - 0.85) - 0.035);
        }
        coverage = saturate(0.5 - distance / max(fwidth(distance), 0.0001));
    }
    if (shape == 2)
    {
        float2 seed_uv = frac(input.Detail.y * float2(0.0137, 0.0293));
        float noise = VfxNoiseBasis.Sample(MaterialSampler, p * 0.7 + seed_uv + age * 0.08).r * 0.57;
        noise += VfxNoiseBasis.Sample(MaterialSampler, p * 1.4 + seed_uv - age * 0.1).g * 0.29;
        noise += VfxNoiseBasis.Sample(MaterialSampler, p * 2.8 + seed_uv).b * 0.14;
        float edge = radius + (noise - 0.5) * 0.32;
        coverage = saturate((0.95 - edge) / max(fwidth(edge), 0.025)) * saturate(1.0 - radius * radius);
    }
    if (shape == 7)
    {
        // Two offset discs intersect to form a soft leaf with pointed ends.
        float leaf = max(length(float2(p.x * 0.7, p.y) - float2(0.38, 0)),
                         length(float2(p.x * 0.7, p.y) + float2(0.38, 0))) - 0.85;
        coverage = saturate(0.5 - leaf / max(fwidth(leaf), 0.05));
        coverage *= saturate(1.0 - abs(p.y) * 0.5);
    }
    else if (shape == 8 || shape == 9)
    {
        float diamond = (abs(p.x) + abs(p.y) - 0.9) * 0.70710678;
        coverage = saturate(0.5 - diamond / max(fwidth(diamond), 0.035));
    }
    if (shape != 0) coverage *= 1.0 - smoothstep(0.72, 1.0, age);
    if (input.ShapeMask.y != 0xffffffff)
    {
        float mask = VfxAuthoredMask.Sample(LinearClamp, float3(p * 0.5 + 0.5, input.ShapeMask.y));
        coverage *= lerp(1.0, mask, input.Detail.x);
    }
    float2 screen_uv = input.Position.xy * ScreenSize.zw;
    float valid = GBufferNormal.SampleLevel(LinearClamp, screen_uv, 0).a;
    if (valid > 0.0)
    {
        float3 scene_world = GBufferPosition.SampleLevel(LinearClamp, screen_uv, 0).xyz;
        float scene_depth = dot(scene_world - CameraTime.xyz, CameraForwardSoftness.xyz);
        float flash_depth = dot(input.WorldPosition - CameraTime.xyz, CameraForwardSoftness.xyz);
        coverage *= saturate((scene_depth - flash_depth) / CameraForwardSoftness.w);
    }
    return float4(input.Color.rgb * input.Color.a * coverage, 0.0);
}

float AuthoredSpriteFbm(float2 uv, uint octaves)
{
    float value = 0.0;
    float amplitude = 0.5333333;
    float normalization = 0.0;
    for (uint octave = 0; octave < octaves; ++octave)
    {
        float4 basis = VfxNoiseBasis.Sample(MaterialSampler, uv);
        value += basis[octave % 4] * amplitude;
        normalization += amplitude;
        uv = uv * 2.03 + float2(0.17, 0.31);
        amplitude *= 0.5;
    }
    return value / max(normalization, 0.0001);
}

OitOutput VfxSpriteOitPS(VfxFlashOutput input)
{
    const float age = input.Gradient.z;
    const float seconds = input.SurfaceData.w;
    const float2 p = input.LocalUv;
    const float4 gradient = SampleVfxGradient(input.Gradient.x, age);
    float3 color = gradient.rgb * input.Gradient.y;
    float alpha = gradient.a * input.Color.a;
    const uint shape = input.ShapeMask.x;
    const float2 seed_uv = frac(input.Detail.y * float2(0.0137, 0.0293));
    float detail = 1.0;
    if (input.ShapeMask.y != 0xffffffff)
        detail = lerp(1.0, VfxAuthoredMask.Sample(LinearClamp, float3(p * 0.5 + 0.5, input.ShapeMask.y)), input.Detail.x);
    if (shape == 11)
    {
        const uint first_frame = input.SurfaceMetadata.y;
        const uint frame_count = input.SurfaceMetadata.z;
        const float clip_time = min(seconds * asfloat(input.SurfaceMetadata.w), float(frame_count - 1));
        const uint frame0 = first_frame + (uint)clip_time;
        const uint frame1 = min(frame0 + 1, first_frame + frame_count - 1);
        const float blend = frac(clip_time);
        const float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
        // Motion vectors encode this frame's displacement to the next, in units of 8 pixels.
        const float2 displacement = VfxSmokeMotion.Sample(LinearClamp, float3(uv, frame0)) * (8.0 / 128.0) * input.SurfaceData.z;
        const float2 uv0 = uv - displacement * blend;
        const float2 uv1 = uv + displacement * (1.0 - blend);
        const float4 positive = lerp(VfxSmokePositive.Sample(LinearClamp, float3(uv0, frame0)),
                                     VfxSmokePositive.Sample(LinearClamp, float3(uv1, frame1)), blend);
        const float4 negative = lerp(VfxSmokeNegative.Sample(LinearClamp, float3(uv0, frame0)),
                                     VfxSmokeNegative.Sample(LinearClamp, float3(uv1, frame1)), blend);
        const float3 forward = normalize(CameraForwardSoftness.xyz);
        const float3 right = normalize(cross(float3(0, 1, 0), forward));
        const float3 up = normalize(cross(forward, right));
        const float3 light = normalize(-LightDirectionIntensity.xyz);
        const float3 local_light = float3(dot(light, right), dot(light, up), dot(light, forward));
        const float3 positive_weight = max(local_light, 0.0);
        const float3 negative_weight = max(-local_light, 0.0);
        const float response = dot(positive.rgb, positive_weight * positive_weight) + dot(negative.rgb, negative_weight * negative_weight);
        // Shadow palette colors the scattering; only baked emissive follows the hot gradient.
        color = (input.Color.rgb * (0.18 + response * LightDirectionIntensity.w) +
                 gradient.rgb * input.Gradient.y * negative.a) * detail;
        for (uint index = 0; index < VfxLightCount.x; ++index)
        {
            const float3 delta = VfxLightPositionRadius[index].xyz - input.WorldPosition;
            const float distance_squared = dot(delta, delta);
            const float3 toward_light = delta * rsqrt(max(distance_squared, 0.0001));
            const float3 local = float3(dot(toward_light, right), dot(toward_light, up), dot(toward_light, forward));
            const float3 pos_weight = max(local, 0.0), neg_weight = max(-local, 0.0);
            const float scattering = dot(positive.rgb, pos_weight * pos_weight) + dot(negative.rgb, neg_weight * neg_weight);
            const float radius = VfxLightPositionRadius[index].w;
            const float edge = saturate(1.0 - distance_squared / (radius * radius));
            color += input.Color.rgb * VfxLightColorIntensity[index].rgb * VfxLightColorIntensity[index].w *
                scattering * edge * edge / (1.0 + distance_squared) * detail;
        }
        // Artistic mask modulates RGB only; baked positive alpha is the smoke silhouette.
        alpha *= positive.a * input.SurfaceData.y;
    }
    else
    {
        const uint octaves = input.SurfaceMetadata.x;
        const float2 flow = float2(0.0, -seconds * 0.85);
        const float2 uv = p * 0.65 + seed_uv + flow;
        const float2 warp = float2(AuthoredSpriteFbm(uv, octaves), AuthoredSpriteFbm(uv + float2(.37,.61), octaves)) - 0.5;
        const float noise = AuthoredSpriteFbm(uv + warp * input.SurfaceData.x, octaves);
        float coverage;
        if (shape == 10)
        {
            const float height = p.y * 0.5 + 0.5;
            const float width = lerp(0.78, 0.10, saturate(height));
            const float lobe = abs(p.x + warp.x * input.SurfaceData.x) / width;
            const float vertical = abs(p.y + 0.08) / 0.95;
            const float edge = max(lobe, vertical) + (noise - 0.5) * 0.7;
            coverage = (1.0 - smoothstep(0.72, 1.0, edge)) *
                smoothstep(-1.0, -0.88, p.y);
            color *= 0.65 + 0.8 * noise;
        }
        else
        {
            const float radius = length(p);
            const float angle = atan2(p.y, p.x);
            const float turbulence = AuthoredSpriteFbm(float2(angle * 0.7, radius * 2.5 - seconds) + seed_uv, octaves);
            const float edge = radius + (turbulence - 0.5) * 0.26;
            coverage = (1.0 - smoothstep(0.65, 1.0, edge)) * (0.35 + 0.65 * noise);
        }
        alpha *= coverage * input.SurfaceData.y * detail;
    }
    alpha *= 1.0 - smoothstep(0.72, 1.0, age);
    const float2 screen_uv = input.Position.xy * ScreenSize.zw;
    const float valid = GBufferNormal.SampleLevel(LinearClamp, screen_uv, 0).a;
    if (valid > 0)
    {
        const float3 scene = GBufferPosition.SampleLevel(LinearClamp, screen_uv, 0).xyz;
        const float scene_depth = dot(scene - CameraTime.xyz, CameraForwardSoftness.xyz);
        const float sprite_depth = dot(input.WorldPosition - CameraTime.xyz, CameraForwardSoftness.xyz);
        alpha *= saturate((scene_depth - sprite_depth) / CameraForwardSoftness.w);
    }
    alpha = saturate(alpha);
    const float z = saturate(length(input.WorldPosition - CameraTime.xyz) / 180.0);
    const float weight = clamp(pow(min(1.0, alpha * 10.0) + 0.01, 3.0) * 1e8 * pow(1.0 - z * 0.9, 3.0), 1e-2, 3e3);
    OitOutput output;
    output.Accumulation = float4(color * alpha * weight, alpha * weight);
    output.Revealage = alpha.xxxx;
    return output;
}

float3 BloomBrightPass(float3 color)
{
    const float brightness = max(max(color.r, color.g), color.b);
    const float knee = smoothstep(0.8, 1.2, brightness);
    return color * knee * saturate((brightness - 0.6) / max(brightness, 0.0001));
}

float4 TemporalPS(FullScreenOutput input) : SV_Target0
{
    const float3 current = PostA.SampleLevel(LinearClamp, input.Uv, 0).rgb;
    const int2 pixel = clamp(int2(input.Uv * ScreenSize.xy), int2(0, 0),
                             int2(ScreenSize.xy) - 1);
    const float4 position = GBufferPosition.Load(int3(pixel, 0));
    const float4 normal = GBufferNormal.Load(int3(pixel, 0));
    if (normal.a < 0.5)
        return float4(current, 0.0);

    const float4 current_clip = mul(float4(position.xyz, 1.0), ViewProjection);
    const float current_depth = current_clip.z / max(current_clip.w, 0.0001);
    if (TemporalOptions.y < 0.5)
        return float4(current, current_depth);

    const float4 previous_clip = mul(float4(position.xyz, 1.0), PreviousViewProjection);
    if (previous_clip.w <= 0.0001)
        return float4(current, current_depth);
    const float2 previous_uv = float2(previous_clip.x / previous_clip.w * 0.5 + 0.5,
                                      0.5 - previous_clip.y / previous_clip.w * 0.5);
    if (any(previous_uv < 0.0) || any(previous_uv > 1.0))
        return float4(current, current_depth);

    const float4 history = TemporalInput.SampleLevel(LinearClamp, previous_uv, 0);
    const float expected_depth = previous_clip.z / previous_clip.w;
    if (abs(history.a - expected_depth) > 0.002)
        return float4(current, current_depth);

    float3 minimum = current;
    float3 maximum = current;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const float3 neighbor = PostA.SampleLevel(LinearClamp,
                input.Uv + float2(x, y) * ScreenSize.zw, 0).rgb;
            minimum = min(minimum, neighbor);
            maximum = max(maximum, neighbor);
        }
    }
    const float3 bounded_history = clamp(history.rgb, minimum, maximum);
    const float3 opaque = HdrColor.SampleLevel(LinearClamp, input.Uv, 0).rgb;
    const float reactive = saturate(max(max(abs(current.r - opaque.r),
                                            abs(current.g - opaque.g)),
                                        abs(current.b - opaque.b)) /
                                    max(1.0, max(max(current.r, current.g), current.b)));
    const float change = saturate(length(current - bounded_history) /
                                  max(1.0, length(current)));
    const float history_weight = 0.88 * (1.0 - reactive) * (1.0 - change);
    return float4(lerp(current, bounded_history, history_weight), current_depth);
}

float3 BloomSource(float2 uv)
{
    return TemporalOptions.x > 0.5
        ? TemporalOutput.SampleLevel(LinearClamp, uv, 0).rgb
        : PostA.SampleLevel(LinearClamp, uv, 0).rgb;
}

float4 BloomExtractPS(FullScreenOutput input) : SV_Target0
{
    const float2 texel = ScreenSize.zw * 0.5;
    float3 bright = 0.0;
    bright += BloomBrightPass(BloomSource(input.Uv + float2(-texel.x, -texel.y)));
    bright += BloomBrightPass(BloomSource(input.Uv + float2(texel.x, -texel.y)));
    bright += BloomBrightPass(BloomSource(input.Uv + float2(-texel.x, texel.y)));
    bright += BloomBrightPass(BloomSource(input.Uv + float2(texel.x, texel.y)));
    return float4(bright * 0.25, 1.0);
}

float4 BloomDownsamplePS(FullScreenOutput input) : SV_Target0
{
    uint width, height;
    BloomHalf.GetDimensions(width, height);
    const float2 texel = 0.5 / float2(width, height);
    float3 color = 0.0;
    color += BloomHalf.SampleLevel(LinearClamp, input.Uv + float2(-texel.x, -texel.y), 0).rgb;
    color += BloomHalf.SampleLevel(LinearClamp, input.Uv + float2(texel.x, -texel.y), 0).rgb;
    color += BloomHalf.SampleLevel(LinearClamp, input.Uv + float2(-texel.x, texel.y), 0).rgb;
    color += BloomHalf.SampleLevel(LinearClamp, input.Uv + float2(texel.x, texel.y), 0).rgb;
    return float4(color * 0.25, 1.0);
}

float4 BloomUpsamplePS(FullScreenOutput input) : SV_Target0
{
    uint width, height;
    BloomQuarter.GetDimensions(width, height);
    const float2 texel = 1.0 / float2(width, height);
    float3 wide = BloomQuarter.SampleLevel(LinearClamp, input.Uv, 0).rgb * 4.0;
    wide += BloomQuarter.SampleLevel(LinearClamp, input.Uv + float2(texel.x, 0), 0).rgb * 2.0;
    wide += BloomQuarter.SampleLevel(LinearClamp, input.Uv - float2(texel.x, 0), 0).rgb * 2.0;
    wide += BloomQuarter.SampleLevel(LinearClamp, input.Uv + float2(0, texel.y), 0).rgb * 2.0;
    wide += BloomQuarter.SampleLevel(LinearClamp, input.Uv - float2(0, texel.y), 0).rgb * 2.0;
    wide += BloomQuarter.SampleLevel(LinearClamp, input.Uv + texel, 0).rgb;
    wide += BloomQuarter.SampleLevel(LinearClamp, input.Uv - texel, 0).rgb;
    wide += BloomQuarter.SampleLevel(LinearClamp, input.Uv + float2(texel.x, -texel.y), 0).rgb;
    wide += BloomQuarter.SampleLevel(LinearClamp, input.Uv + float2(-texel.x, texel.y), 0).rgb;
    const float3 near = BloomHalf.SampleLevel(LinearClamp, input.Uv, 0).rgb;
    return float4(near * 0.65 + wide * (0.35 / 16.0), 1.0);
}

float4 BloomPS(FullScreenOutput input) : SV_Target0
{
    const float3 center = BloomSource(input.Uv);
    const float3 wide = BloomHalfCombined.SampleLevel(LinearClamp, input.Uv, 0).rgb;
    // Keep the diffuse halo below the exact additive ground silhouette.
    const float3 halo = min(max(wide, 0.0) * 0.35, max(center, 0.0) * 0.18 + 0.08);
    return float4(center + (RenderOptions.y > 0.5 ? halo : 0.0), 1.0);
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
    if (TemporalOptions.x > 0.5) return float4(center, 1.0);
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

// Analytic silhouette with authored vector/noise detail; no temporal-history reconstruction.
struct VfxDistortionOutput
{
    float4 Position : SV_Position;
    float2 Local : TEXCOORD0;
    float3 World : TEXCOORD1;
    nointerpolation float4 Params : TEXCOORD2;
    nointerpolation float3 AimStrength : TEXCOORD3;
    nointerpolation float4 CenterRadius : TEXCOORD4;
    nointerpolation float4 Geometry : TEXCOORD5;
    nointerpolation uint2 GapRange : TEXCOORD6;
    nointerpolation float2 DirectionXZ : TEXCOORD7;
};
VfxDistortionOutput VfxDistortionVS(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    static const float2 corners[6] = {float2(-1,-1),float2(-1,1),float2(1,1),float2(-1,-1),float2(1,1),float2(1,-1)};
    const DistortionGpu source = VfxDistortions[instance_id];
    const float3 forward = normalize(CameraForwardSoftness.xyz);
    const float3 right = normalize(cross(abs(forward.y) > .999 ? float3(0,0,1) : float3(0,1,0),forward));
    const float3 up = normalize(cross(forward,right));
    float2 aim = float2(dot(source.DirectionStrength.xyz,right),dot(source.DirectionStrength.xyz,up));
    aim = dot(aim,aim) > .00001 ? normalize(aim) : float2(1,0);
    const uint shape = (uint)source.PhaseAlphaShapeSeed.z;
    const float2 local = corners[vertex_id];
    const float2 extent = shape == 3 ? float2(1,.35) : float2(1,1);
    const float2 plane = aim*(local.x*extent.x) + float2(-aim.y,aim.x)*(local.y*extent.y);
    VfxDistortionOutput output;
    output.World = shape >= 5
        ? source.PositionRadius.xyz + float3(local.x*source.PositionRadius.w,0,local.y*source.PositionRadius.w)
        : source.PositionRadius.xyz + (right*plane.x+up*plane.y)*source.PositionRadius.w;
    output.Position = mul(float4(output.World,1),ViewProjection);
    output.Local = local;
    output.Params = source.PhaseAlphaShapeSeed;
    output.AimStrength = float3(aim,source.DirectionStrength.w);
    output.CenterRadius = source.PositionRadius;
    output.Geometry = source.Geometry;
    output.GapRange = source.GapRange.xy;
    const float2 horizontal = source.DirectionStrength.xz;
    output.DirectionXZ = dot(horizontal,horizontal) > .000001 ? normalize(horizontal) : float2(0,1);
    return output;
}

FresnelShellOutput FresnelShellVS(SceneInput input, uint vertex_id : SV_VertexID)
{
    // Root SRV t0 is rebound to the selected boss instance before each draw.
    const SceneOutput scene = SceneVS(input, 0, vertex_id);
    const FresnelShellGpu shell = FresnelShellCommands[0];
    const bool crown = shell.Metadata.x == 0;
    const bool player = shell.Metadata.x == 2;
    const float progress = saturate(shell.Geometry.y);
    const float pulse_rate = shell.Motion.x > 0.0 ? shell.Motion.x : 0.8;
    const float pulse = 0.5 + 0.5 * sin(shell.Geometry.z * pulse_rate * 6.2831853);
    // Move the same skinned surface toward its opaque depth as the shell contracts.
    const float extent = min(shell.Geometry.x * (player ? 0.04 : 0.06), player ? 0.04 : 0.08);
    const float displacement = 0.003 + extent *
        (player ? 0.35 + 0.65 * pulse : crown ? 0.25 + 0.75 * pulse : 1.0 - progress);
    const float3 world = scene.WorldPosition + normalize(scene.WorldNormal) * displacement;
    FresnelShellOutput output;
    output.Position = mul(float4(world, 1.0), ViewProjection);
    output.WorldNormal = scene.WorldNormal;
    output.WorldPosition = world;
    output.Center = Instances[0].Position.xyz;
    return output;
}
float4 VfxDistortionPS(VfxDistortionOutput input) : SV_Target0
{
    const float r = length(input.Local);
    const float age = saturate(input.Params.x);
    const uint shape = (uint)input.Params.z;
    const uint seed = (uint)input.Params.w;
    float coverage = 1.0-smoothstep(.72,1.0,r);
    float2 flow_vector = input.Local/max(r,.001);
    if(shape == 5 || shape == 6)
    {
        const float2 relative = input.World.xz - input.CenterRadius.xz;
        const float distance_world = length(relative);
        const float aa = max(fwidth(distance_world),.0001);
        if (distance_world > input.CenterRadius.w) discard;
        coverage = saturate((input.CenterRadius.w-distance_world)/aa);
        if (shape == 5)
        {
            const float2 radial = distance_world > .0001 ? relative/distance_world : input.DirectionXZ;
            const float angle = abs(atan2(input.DirectionXZ.x*radial.y-input.DirectionXZ.y*radial.x,
                                           dot(input.DirectionXZ,radial)));
            if (angle > input.Geometry.y) discard;
            coverage *= saturate((input.Geometry.y-angle)*max(distance_world,.001)/aa);
        }
        else
        {
            if (distance_world < input.Geometry.x) discard;
            coverage *= saturate((distance_world-input.Geometry.x)/aa);
            const float angle = atan2(relative.y,relative.x);
            for (uint gap = 0; gap < input.GapRange.y; ++gap)
            {
                const float authored = GroundGapAngles[input.GapRange.x+gap];
                const float delta = abs(atan2(sin(angle-authored),cos(angle-authored)));
                if (delta < input.Geometry.z) discard;
                coverage *= saturate((delta-input.Geometry.z)*max(distance_world,.001)/aa);
            }
        }
    }
    else if(shape == 0)
        coverage *= exp(-pow((r-lerp(.12,.86,age))*12,2));
    else if(shape == 1 || shape == 2)
        coverage *= exp(-pow((r-lerp(.08,.7,age))*6,2));
    else if(shape == 3)
        coverage *= saturate(.8+.2*input.Local.x);
    else
        coverage *= exp(-r*r*3);
    const float2 uv = input.Local*.5+.5;
    const float2 seed_offset = float2(seed%31,seed%23)*.071;
    const float2 flow = VfxFlowCurl.SampleLevel(MaterialSampler,uv*1.7+seed_offset+float2(0,-age*.35),0);
    const float noise = VfxNoiseBasis.SampleLevel(MaterialSampler,uv*2.3+seed_offset+age*.17,0).r;
    flow_vector += flow*(shape == 1 || shape == 3 ? .45 : .15);
    coverage *= lerp(.8,1.0,noise);
    const float2 screen_uv = input.Position.xy*ScreenSize.zw;
    const float valid = GBufferNormal.SampleLevel(LinearClamp,screen_uv,0).a;
    const float depth = dot(input.World-CameraTime.xyz,CameraForwardSoftness.xyz);
    if(valid > 0)
    {
        const float3 scene = GBufferPosition.SampleLevel(LinearClamp,screen_uv,0).xyz;
        const float clearance = dot(scene-CameraTime.xyz,CameraForwardSoftness.xyz)-depth;
        coverage *= shape >= 5 ? saturate((clearance+.05)/.08) : saturate(clearance/.35);
    }
    const float2 edge = min(screen_uv,1-screen_uv);
    coverage *= smoothstep(0,.025,min(edge.x,edge.y));
    // Stable per-source STBN modulation avoids frame-index shimmer and makes no temporal claim.
    const float stbn = VfxStbnScalar.Load(int4((uint2)input.Position.xy%64,seed%64,0));
    coverage *= (.98+.02*stbn)*input.Params.y*sin(3.14159265*age);
    const float2 aim = input.AimStrength.xy;
    const float2 screen_vector = aim*flow_vector.x+float2(-aim.y,aim.x)*flow_vector.y;
    const float2 offset = screen_vector*float2(1,-1)*min(input.AimStrength.z,.04)*coverage;
    return float4(offset,max(depth,0)*coverage,coverage);
}
