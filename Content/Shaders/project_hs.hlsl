#include "render_binding_contract.hlsli"
#include "environment_wind.hlsli"

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
    uint4 SkinCacheOptions; // w: static instance generation for probe capture.
    row_major float4x4 InverseViewProjection;
    row_major float4x4 CurrentUnjitteredViewProjection;
    row_major float4x4 PreviousUnjitteredViewProjection;
    float4 SurfaceViewOrigin;
    float4 SurfaceJitterUv; // xy: current jitter UV; zw: previous jitter UV.
    float4 LodViewRight;
    float4 LodViewUp;
    float4 LodViewForward;
    float4 LodProjection; // x: perspective pixel scale; yzw: cascade pixels/world unit.
    float4 LodOptions; // pixel tolerance, hysteresis, near distance, fade step.
    uint4 LodClock; // frame, session epoch, valid view history, diagnostic flags.
    uint4 MaterialParameterBases[4]; // Same selected table used by raster and deferred.
    uint4 MaterialVaryingMasks[4];
    uint4 MaterialSidecarOffsets[2];
    uint4 MaterialSidecarInfo; // union mask, packed word count, plane count, reserved.
    float4 OutputSize; // Output extent in xy, reciprocal extent in zw.
    float4 PostOptions; // Bloom strength, outline width in output pixels, grading, sharpening.
    float4 MaterialQualityOptions; // POM march/refinement, packed binary16 terrain fade range, screen refraction quality.
    float4 DisplayOptions; // Native HDR enabled, paper white nits, peak nits, encoded DevTools in PostA.
    row_major float4x4 InverseUnjitteredViewProjection;
    float4 LightingOptions;
    uint4 HeroMaterialParameterBases[2];
    uint4 HeroMaterialVaryingMasks[2];
    uint4 HeroBindingRanges[7];
    float4 ProbeOptions; // cosine/sine yaw, maximum prefiltered LOD, enabled.
    float4 PreviousGrassBenders[32];
    float4 PreviousGrassState; // time, bender count, valid, reserved.
    float4 PreviousGrassOriginDelta; // current scene origin minus previous origin.
    uint4 SceneTableBounds; // Serialized dynamic scene byte length in x; reserved yzw.
    float4 GrassBenderGrid;
    uint4 GrassBenderMasks[16];
    float4 PreviousGrassBenderGrid;
    uint4 PreviousGrassBenderMasks[16];
    float4 ParticleTerrain; // Scene origin, canonical meadow collision enabled.
};

cbuffer PassConstants : register(b1)
{
    uint PassValue;
    // bit 0: static scene; bits 1-31: reflected material parameter slot base.
    uint SceneDrawMaterial;
};

#include "geometry_records.hlsli"
#include "material_parameters.hlsli"
#include "material_sidecar.hlsli"
#include "temporal_reconstruction.hlsli"
#include "display_transform.hlsli"
#include "volumetric_sampling.hlsli"

// Transparent transmission samples the opaque radiance before its camera
// prefix. The native frame publishes this alongside the volumetric textures.
Texture2D<float4> VmRawOpaqueHdr : register(t1, space24);

bool HsFogEnabled()
{
#ifdef HS_LOCAL_PROBE_CAPTURE
    return false;
#else
    return VmCounts.y != 0u;
#endif
}
float HsFogViewDepth(float3 world)
{
    return mul(float4(world - VmOriginNear.xyz, 0.0), VmWorldToView).z;
}
HsVolumetricTransport HsFogPrefix(float3 world, float2 pixel)
{
    if (!HsFogEnabled()) return HsVolumetricIdentity();
    return HsSampleVolumetricTransport(pixel * ScreenSize.zw, HsFogViewDepth(world));
}
float3 HsFogSurface(float3 raw, float3 world, float2 pixel)
{
    return HsApplyVolumetricTransport(raw, HsFogPrefix(world, pixel));
}
float3 HsFogEmission(float3 raw, float3 world, float2 pixel)
{
    return HsFogPrefix(world, pixel).transmittance * raw;
}
float3 HsFogSky(float3 raw, float2 uv)
{
    if (!HsFogEnabled()) return raw;
    return HsApplyVolumetricTransport(raw, HsSampleVolumetricTransport(uv, VmDepthRange.x));
}

// Terrain helper defaults remain usable by standalone material consumers.
#define HS_MATERIAL_POM_STEPS uint(MaterialQualityOptions.x)
#define HS_MATERIAL_POM_REFINEMENT_STEPS uint(MaterialQualityOptions.y)
#define HS_MATERIAL_DETAIL_FADE_RANGE float2(f16tof32(asuint(MaterialQualityOptions.z) & 0xffffu), f16tof32(asuint(MaterialQualityOptions.z) >> 16u))

#ifndef HS_SURFACE_SIDECAR_PLANES
#define HS_SURFACE_SIDECAR_PLANES 0
#endif

struct InstanceData
{
    float4 Position;
    float4 Scale;
    uint Color;
    uint Mesh;
    float Yaw;
    float Padding;
};
#include "gpu_scene_tables.hlsli"
ByteAddressBuffer SkinPreviousVertices : register(t3, space3);
StructuredBuffer<InstanceData> SkinPreviousInstances : register(t4, space3);
ByteAddressBuffer SkinCurrentVertices : register(t5, space3);

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
struct StaticDrawRef
{
    uint InstanceIndex;
    uint MeshFlags;
    float LodThreshold;
    uint InstanceGeneration;
};
StructuredBuffer<StaticDrawRef> StaticSceneDraws : register(t29, space3);
StructuredBuffer<ParticleData> Particles : register(t1);
ByteAddressBuffer VfxMeshAtlas : register(t0, space3);
StructuredBuffer<ParticleSpawnCommandGpu> ParticleSpawnCommands : register(t12);
StructuredBuffer<float> GroundGapAngles : register(t11, space3);
StructuredBuffer<uint> DrawAliveIndices : register(t13);
StructuredBuffer<uint> ParticleSpawnOwners : register(t17);
struct MonsterSkinMatrixElement
{
    row_major float4x4 value;
};
StructuredBuffer<MonsterSkinMatrixElement> MonsterSkinMatrices : register(t18);
RWStructuredBuffer<ParticleData> WritableParticles : register(u0);
RWByteAddressBuffer IndirectArguments : register(u1);
#ifdef HS_CAPTURE_HERO_WORK
// Diagnostic PS only: root4 is rebound to a separate bounded UAV, never the
// particle indirect buffer. Layout is paired with renderer_hero_work_math.hpp.
void HsHeroWorkAdd(uint counter,uint amount=1u)
{
    uint previous;
    IndirectArguments.InterlockedAdd(32u+counter*4u,amount,previous);
    if(previous>0xffffffffu-amount) IndirectArguments.InterlockedOr(24u,1u,previous);
}
void HsHeroWorkPixel(float2 position,bool accepted)
{
    const uint width=IndirectArguments.Load(8u),height=IndirectArguments.Load(12u);
    const uint2 pixel=uint2(position);
    uint previous;
    if(any(position<0.0) || pixel.x>=width || pixel.y>=height)
    { IndirectArguments.InterlockedOr(24u,2u,previous); return; }
    const uint index=pixel.y*width+pixel.x;
    const uint words=IndirectArguments.Load(16u),capacity=IndirectArguments.Load(20u);
    if(index>=capacity || (index>>5u)>=words)
    { IndirectArguments.InterlockedOr(24u,2u,previous); return; }
    const uint mask=1u<<(index&31u);
    IndirectArguments.InterlockedOr(256u+(accepted?words*4u:0u)+(index>>5u)*4u,mask,previous);
    if((previous&mask)==0u) HsHeroWorkAdd(accepted?17u:2u);
}
void HsHeroWorkEntry(float2 position,uint coverage)
{ HsHeroWorkAdd(0u); HsHeroWorkAdd(1u,countbits(coverage)); HsHeroWorkPixel(position,false); }
void HsHeroWorkAccepted(float2 position,uint coverage)
{ HsHeroWorkAdd(15u); HsHeroWorkAdd(16u,countbits(coverage)); HsHeroWorkPixel(position,true); }
#define HS_HERO_WORK_COVERAGE ,uint hsHeroCoverage:SV_Coverage
#define HS_HERO_WORK_ENTRY(position) HsHeroWorkEntry(position,hsHeroCoverage)
#define HS_HERO_WORK_ACCEPT(position) HsHeroWorkAccepted(position,hsHeroCoverage)
#define HS_HERO_WORK_COUNT(counter) HsHeroWorkAdd(counter)
#else
#define HS_HERO_WORK_COVERAGE
#define HS_HERO_WORK_ENTRY(position)
#define HS_HERO_WORK_ACCEPT(position)
#define HS_HERO_WORK_COUNT(counter)
#endif

RWStructuredBuffer<uint> AliveInput : register(u2);
RWStructuredBuffer<uint> AliveOutput : register(u3);
RWStructuredBuffer<uint> DeadIndices : register(u4);
RWByteAddressBuffer ParticleCounters : register(u5);
Texture2D<float4> GBufferBase : register(t2);
Texture2D<float2> GBufferNormal : register(t3);
Texture2D<uint2> GBufferMetadata : register(t4);
Texture2D<float> SurfaceDepth : register(t0, space6);
Texture2D<float2> SurfaceHistory : register(t1, space6);
Texture2D<uint4> SurfaceSidecar0 : register(t2, space6);
Texture2D<uint4> SurfaceSidecar1 : register(t3, space6);
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
Texture2D<float2> TemporalNormalInput : register(t0, space9);
Texture2D<uint2> TemporalMaterialInput : register(t1, space9);
Texture2D<float4> SceneWork : register(t2, space9);
Texture2D<float4> DisplayLinear : register(t3, space9);
Texture2D<float4> CompositeHdr : register(t4, space9);
Texture2D<float> VfxScreenReactive : register(t1, space32);
StructuredBuffer<float> CurrentExposure : register(t1, space10); // Current, previous.
Texture2D<float4> UiTexture : register(t11);
Texture2DArray<float4> ArcherDiffuse : register(t14);
Texture2DArray<float4> ArcherNormal : register(t15);
Texture2DArray<float> VfxMasks : register(t16);
Texture2D<float4> VfxGradientLut : register(t1, space3);
Texture2DArray<float> VfxStbnScalar : register(t17, space3);
Texture2DArray<float> VfxAuthoredMask : register(t7, space3);
Texture2D<float4> MonsterBasecolor : register(t0, space1);
Texture2D<float4> MonsterEmissive : register(t1, space1);
Texture2D<float4> MonsterRam : register(t2, space1);
Texture2DArray<float4> FamilyDiffuse : register(t3, space1);
Texture2DArray<float4> FamilyNormal : register(t4, space1);
SamplerComparisonState ShadowCompare : register(s1);
SamplerState MaterialSampler : register(s2);

#include "surface_layout.hlsli"

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
    float3 PreviousWorldPosition : TEXCOORD12;
    nointerpolation uint MaterialId : TEXCOORD13;
    nointerpolation uint SurfaceFlags : TEXCOORD14;
    nointerpolation float HistoryConfidence : TEXCOORD15;
    nointerpolation uint RasterDrawRecordId : TEXCOORD16;
    nointerpolation uint2 InstanceKey : TEXCOORD17;
    float4 ShadowClipDistance : SV_ClipDistance0;
};

#include "environment.hlsli"
#include "meadow_surface.hlsli"
#include "hero_material.hlsli"
#include "entity_surface.hlsli"
#ifdef HS_GENERATED_ENVIRONMENT_MATERIAL
#include "environment_material.generated.hlsli"
#endif
#ifdef HS_GENERATED_DEFERRED_MATERIALS
#include "deferred_materials.generated.hlsli"
#endif
#include "deferred_surface.hlsli"

// The native temporal consumer binds the service's immutable combined table.
// LodClock.w bit4 selects guide availability, independently of GI/reflections.
Texture2D<float> HeroGuideDepth : register(t14, space14);
Texture2D<float2> HeroGuideMotion : register(t15, space14);
Texture2D<float2> HeroGuideHistory : register(t16, space14);
Texture2D<float4> HeroGuideClassification : register(t17, space14);
Texture2D<uint2> HeroGuideMetadata : register(t18, space14);
Texture2D<uint> HeroGuideOverflow : register(t19, space14);
Texture2D<float2> HeroGuideTransport : register(t20, space14);

struct HsSceneGradients
{
    float2 uvDx;
    float2 uvDy;
    float3 worldDx;
    float3 worldDy;
    float3 stableDx;
    float3 stableDy;
};

HsSceneGradients RasterSceneGradients(SceneOutput input)
{
    HsSceneGradients gradients;
    gradients.uvDx = ddx(input.Uv);
    gradients.uvDy = ddy(input.Uv);
    gradients.worldDx = ddx(input.WorldPosition);
    gradients.worldDy = ddy(input.WorldPosition);
    gradients.stableDx = ddx(input.EnvironmentStableWorld);
    gradients.stableDy = ddy(input.EnvironmentStableWorld);
    return gradients;
}

float2 HsSaturateGradient(float2 value, float2 gradient)
{
    return float2((value.x > 0.0 && value.x < 1.0) ? gradient.x : 0.0,
                  (value.y > 0.0 && value.y < 1.0) ? gradient.y : 0.0);
}

HsMaterialContext EnvironmentMaterialContext(SceneOutput input, bool front,
                                             HsSceneGradients gradients,
                                             uint materialWord)
{
    HsMaterialContext ctx = (HsMaterialContext)0;
    ctx.uv = input.Uv;
    ctx.d_uv_dx = gradients.uvDx;
    ctx.d_uv_dy = gradients.uvDy;
    const float2 card_uv = saturate(input.Uv);
    const float2 card_dx = HsSaturateGradient(input.Uv, gradients.uvDx);
    const float2 card_dy = HsSaturateGradient(input.Uv, gradients.uvDy);
    ctx.actual_world_position = input.WorldPosition;
    ctx.d_actual_world_dx = gradients.worldDx;
    ctx.d_actual_world_dy = gradients.worldDy;
    ctx.stable_world_position = input.EnvironmentStableWorld;
    ctx.d_stable_world_dx = gradients.stableDx;
    ctx.d_stable_world_dy = gradients.stableDy;
    ctx.layer = input.Material;
    const bool foliage = input.Mesh == 17 || input.Mesh == 19;
    ctx.shading_normal = normalize(foliage && !front ? -input.WorldNormal : input.WorldNormal);
    ctx.tangent = input.WorldTangent;
    ctx.normal = float3(0, 0, 1);
    ctx.eye_distance = length(CameraTime.xyz - input.WorldPosition);
    ctx.mesh = input.Mesh;
    ctx.seed = input.EnvironmentSeed;
    ctx.view_direction = CameraTime.xyz - input.WorldPosition;
    ctx.parameter_base = materialWord >> 1;
    // Legacy sampled-value library calls use neutral values. Authored graphs
    // read their reflected declarations from MaterialParameters.
    ctx.roughness_scale = 1.0;
    ctx.metallic = 0.0;
    ctx.sampled_base_color = input.Color.rgb;
    ctx.sampled_roughness = 1;
    ctx.sampled_alpha = 1;
    ctx.sampled_ao = 1;
    if (foliage)
    {
        const float4 rect = input.Mesh == 19 ? GrassRects[input.Material] : LeafRects[input.Material];
        const float2 scale = rect.zw / 2048.0;
        ctx.uv = rect.xy / 2048.0 + card_uv * scale;
        ctx.d_uv_dx = card_dx * scale;
        ctx.d_uv_dy = card_dy * scale;
        ctx.layer = input.Mesh == 19 ? GrassPages[input.Material] : LeafPages[input.Material];
    }
#ifndef HS_GENERATED_ENVIRONMENT_MATERIAL
    if (input.Mesh == 16 || input.Mesh == 18)
    {
        EnvMaterial material = EnvOpaque(ctx.uv, ctx.d_uv_dx, ctx.d_uv_dy, ctx.layer,
                                        input.Mesh == 18, ctx.eye_distance);
        ctx.sampled_base_color = material.color;
        ctx.sampled_roughness = material.roughness;
        ctx.sampled_ao = material.ao;
        ctx.normal = material.normal;
    }
    else if (foliage)
    {
        const float3 uv = float3(ctx.uv, ctx.layer);
        const float4 color = input.Mesh == 19
            ? GrassBase.SampleGrad(FoliageClamp, uv, ctx.d_uv_dx, ctx.d_uv_dy)
            : LeafBase.SampleGrad(FoliageClamp, uv, ctx.d_uv_dx, ctx.d_uv_dy);
        ctx.sampled_base_color = color.rgb * (input.Mesh == 19u ? input.Color.rgb : float3(1,1,1));
        ctx.sampled_alpha = color.a;
        ctx.normal = EnvDecode(input.Mesh == 19
            ? GrassNormal.SampleGrad(FoliageClamp, uv, ctx.d_uv_dx, ctx.d_uv_dy).rg
            : LeafNormal.SampleGrad(FoliageClamp, uv, ctx.d_uv_dx, ctx.d_uv_dy).rg);
        ctx.sampled_roughness = input.Mesh == 19
            ? GrassRoughness.SampleGrad(FoliageClamp, uv, ctx.d_uv_dx, ctx.d_uv_dy)
            : LeafRoughness.SampleGrad(FoliageClamp, uv, ctx.d_uv_dx, ctx.d_uv_dy);
    }
#endif
    return ctx;
}

HsMaterialContext EnvironmentMaterialContext(SceneOutput input, bool front)
{
    return EnvironmentMaterialContext(input, front, RasterSceneGradients(input),
                                      SceneDrawMaterial);
}

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
        MonsterSkinMatrices[matrix_base + input.BoneIndices.x].value * input.BoneWeights.x +
        MonsterSkinMatrices[matrix_base + input.BoneIndices.y].value * input.BoneWeights.y +
        MonsterSkinMatrices[matrix_base + input.BoneIndices.z].value * input.BoneWeights.z +
        MonsterSkinMatrices[matrix_base + input.BoneIndices.w].value * input.BoneWeights.w;
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

#include "grass_deformation.hlsli"

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

struct HsSceneVertexContext
{
    InstanceData instance;
    StaticDrawRef draw;
    uint skinSlot;
    uint2 skinVertexOffsets;
    uint2 skinVertexStrides;
    bool cachedSkin;
    bool cachedSkinHistory;
    bool gpuSceneRefs;
    GeometryDrawRecord geometry;
    bool geometryValid;
    uint rasterRecordId;
    uint2 instanceKey;
};

SceneOutput EvaluateSceneVertex(SceneInput input, HsSceneVertexContext context,
                                uint vertex_id, bool evaluate_history = true)
{
    const InstanceData instance = context.instance;
    const StaticDrawRef draw = context.draw;
    const bool cached_skin = context.cachedSkin;
    const bool cached_skin_history = evaluate_history && context.cachedSkinHistory;
    const uint skin_slot = context.skinSlot;
    const uint mesh = instance.Mesh & 0xff;
    uint environment_seed = (mesh == 9 || mesh == 20 || mesh == 22) ? asuint(instance.Padding) : EnvCell(floor(instance.Position.xz * 16), 71);
    uint environment_entry;
    EnvironmentVertex(input, instance, vertex_id, environment_seed, environment_entry);
    float3 local_position = input.Position;
    float3 local_normal = input.Normal;
    float3 local_tangent = input.Tangent.xyz;
    if (cached_skin)
    {
        const uint cached_vertex = context.skinVertexOffsets.x + vertex_id;
        const uint cached_offset = cached_vertex * context.skinVertexStrides.x;
        local_position = asfloat(SkinCurrentVertices.Load3(cached_offset));
        local_normal = asfloat(SkinCurrentVertices.Load3(cached_offset + 12u));
        local_tangent = asfloat(SkinCurrentVertices.Load3(cached_offset + 24u));
    }
    else
    {
        SkinArcher(local_position, local_normal, local_tangent, input, mesh);
        SkinMonster(local_position, local_normal, local_tangent, input, instance, mesh);
    }
    // Sampling the charge timeline makes the bubbling reverse with a cancelled draw.
    if (!cached_skin && mesh == 12 && ((instance.Mesh >> 8) & 0xff) == 2)
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
    const float3 grass_local = local_position -
        float3(0, (instance.Mesh & 0x10000) != 0 ? 0.5 : 0.0, 0);
    float3 world = stable_world;
    const bool wind_deformed = HsEnvironmentWind(instance.Mesh);
    HsGrassDeformation grass_deformation = (HsGrassDeformation)0;
    if (wind_deformed)
    {
        const float3 local_height_gradient = float3(0.0,
            abs(instance.Scale.y) > 0.000001 ? rcp(instance.Scale.y) : 0.0, 0.0);
        grass_deformation = HsDeformGrass(stable_world, grass_local,
            local_height_gradient, CameraTime.w, GrassBenders, GrassBenderCount.x,
            GrassBenderGrid, GrassBenderMasks, true);
        world = grass_deformation.position;
    }
    float3 previous_world = world;
    const bool previous_static_grass = context.geometryValid &&
        (context.geometry.Flags & 12u) == 12u;
    const bool previous_dynamic_grass = context.gpuSceneRefs && context.geometryValid &&
        (context.geometry.Flags & 8u) != 0u && (draw.MeshFlags & 0x200000u) != 0u;
    const bool grass_history = evaluate_history && wind_deformed &&
        PreviousGrassState.z > 0.5 && SurfaceViewOrigin.w > 0.5 &&
        (previous_static_grass || previous_dynamic_grass);
    if (grass_history)
    {
        const float3 previous_stable = previous_dynamic_grass
            ? InstanceWorldPosition(SkinPreviousInstances[draw.InstanceIndex], local_position) + PreviousGrassOriginDelta.xyz
            : stable_world;
        // The immutable static lease retains old scene coordinates; dynamic transforms were rebased on the CPU.
        previous_world = HsDeformGrass(previous_stable, grass_local,
            float3(0, 0, 0), PreviousGrassState.x, PreviousGrassBenders,
            (uint)PreviousGrassState.y, PreviousGrassBenderGrid,
            PreviousGrassBenderMasks, false).position - PreviousGrassOriginDelta.xyz;
    }
    if (cached_skin_history)
    {
        const uint previous_vertex = context.skinVertexOffsets.y + vertex_id;
        const float3 previous_local = asfloat(
            SkinPreviousVertices.Load3(previous_vertex * context.skinVertexStrides.y));
        previous_world = InstanceWorldPosition(
            SkinPreviousInstances[skin_slot], previous_local);
    }
    const float3 scale_sign = float3(instance.Scale.x < 0.0 ? -1.0 : 1.0,
        instance.Scale.y < 0.0 ? -1.0 : 1.0, instance.Scale.z < 0.0 ? -1.0 : 1.0);
    local_normal /= scale_sign * max(abs(instance.Scale.xyz), 0.0001);
    local_tangent *= instance.Scale.xyz;
    float3 normal = normalize(float3(
        local_normal.x * cosine_yaw + local_normal.z * sine_yaw,
        local_normal.y,
        -local_normal.x * sine_yaw + local_normal.z * cosine_yaw));
    float3 tangent = normalize(float3(
        local_tangent.x * cosine_yaw + local_tangent.z * sine_yaw,
        local_tangent.y,
        -local_tangent.x * sine_yaw + local_tangent.z * cosine_yaw));

    float tangent_handedness = input.Tangent.w * scale_sign.x * scale_sign.y * scale_sign.z;
    if (wind_deformed)
        HsGrassDeformFrame(grass_deformation, normal, tangent, tangent_handedness);

    SceneOutput output;
    output.Position = mul(float4(world, 1.0), ViewProjection);
    output.WorldNormal = normal;
    output.WorldPosition = world;
    output.PreviousWorldPosition = previous_world;
    output.Color = UnpackColor(instance.Color);
    output.Mesh = mesh;
    output.Uv = input.Uv;
    output.WorldTangent = float4(tangent, tangent_handedness);
    output.Material = mesh==22u ? (input.Uv.y>=100.0?1u:0u) : (mesh == 17 || mesh == 19) ? environment_entry : input.Material;
    const bool environment = mesh == 9 || mesh == 20 || ((mesh >= 16 && mesh <= 19) || mesh == 21 || mesh == 22);
    const uint model = environment ? HsSurfaceModelEnvironment :
        (mesh >= 10 && mesh <= 12 ? HsSurfaceModelJelly : HsSurfaceModelCel);
    const uint foliage = mesh == 17 ? HsSurfaceFoliageLeaf :
        (mesh == 19 ? HsSurfaceFoliageGrass : 0u);
    output.SurfaceFlags = HsPackSurfaceFlags(model, foliage, mesh == 0 ? 1u : 0u);
    output.MaterialId = (mesh == 9 || mesh == 20) ? 0x1000000eu : 0x30000000u + mesh;
    output.HistoryConfidence = cached_skin_history || grass_history ? 1.0 : 0.0;
    if (context.geometryValid)
    {
        // Material binding identity is separate from a foliage atlas entry or
        // a character's vertex submaterial index.
        if ((context.geometry.Flags & 16u) == 0u)
            output.MaterialId = environment ? 0x10000000u + HsEnvironmentMaterialOrdinal(context.geometry.PageSlot / HsEnvironmentGeometryLodCount) :
                (HsHeroAssetIndex(mesh) != HsHeroInvalidOrdinal
                    ? HsHeroOpaqueMaterialId(mesh, input.Material)
                    : 0x20000000u + (context.geometry.PageSlot << 16u) + input.Material);
        if ((context.geometry.Flags & 32u) != 0u)
            output.MaterialId = context.geometry.Lod;
        if (environment && !wind_deformed && (context.geometry.Flags & 12u) == 12u)
            output.HistoryConfidence = 1.0;
    }
    // A resident previous page cannot establish the previous instance's LOD
    // coverage. GPU selection invalidates history for new cuts and all fades.
    if ((draw.MeshFlags & 0x100000u) != 0u)
        output.HistoryConfidence = 0.0;
    if (context.gpuSceneRefs && context.geometryValid && (context.geometry.Flags & 4u) != 0u &&
        HsHlodHistoryFlags(draw.InstanceIndex, draw.InstanceGeneration) != 0u)
        output.HistoryConfidence = 0.0;
    output.RasterDrawRecordId = context.rasterRecordId;
    output.InstanceKey = context.instanceKey;
    output.EnvironmentSeed = environment_seed;
    output.EnvironmentFade = instance.Scale.w;
    output.EnvironmentFlags = instance.Mesh & 0x30000;
    output.EnvironmentStableWorld = stable_world;
    output.AnimationClip = (instance.Mesh >> 8) & 0xff;
    output.AnimationProgress = saturate(instance.Padding);
    output.ShadowClipDistance = HsShadowClipDistance(world, PassValue);
    output.Charge = mesh == 12 && ((instance.Mesh >> 8) & 0xff) == 2
        ? saturate(instance.Padding) : 0.0;
    return output;
}

SceneOutput EvaluateSceneVertex(SceneInput input, uint instance_id, uint vertex_id,
                                bool gpu_scene_refs, bool evaluate_history = true)
{
    HsSceneVertexContext context = (HsSceneVertexContext)0;
    context.skinSlot = 0xffffffffu;
    context.rasterRecordId = 0xffffffffu;
    context.gpuSceneRefs = gpu_scene_refs;
    context.instanceKey = uint2(instance_id + 1u, 0u);
    if (gpu_scene_refs)
    {
        context.draw = StaticSceneDraws[instance_id];
        if (HsShadowIsDraw(PassValue))
        {
            // Shadow proxy coverage is complete, independently of camera LOD fades.
            context.draw.MeshFlags &= ~0x20000u;
            const uint mesh = HsLoadGpuSceneInstance(context.draw.InstanceIndex).Mesh & 0xffu;
            if ((mesh >= 16u && mesh <= 19u) || mesh == 21u || mesh == 22u) context.draw.LodThreshold = 1.0;
        }
        context.instanceKey = uint2(context.draw.InstanceIndex + 1u, context.draw.InstanceGeneration);
        context.instance = HsLoadGpuSceneInstance(context.draw.InstanceIndex);
        context.cachedSkin = (context.draw.MeshFlags & 0x40000u) != 0;
        context.cachedSkinHistory = (context.draw.MeshFlags & 0x80000u) != 0;
        if (context.cachedSkin)
        {
            const uint4 skin = HsLoadGpuSceneSkin(context.draw.InstanceIndex);
            context.skinSlot = skin.x;
            context.skinVertexOffsets = skin.zw;
            context.skinVertexStrides = HsLoadGpuSceneSkinStrides(context.draw.InstanceIndex);
        }
        context.instance.Mesh |= context.draw.MeshFlags;
        const uint resolved_mesh = context.instance.Mesh & 0xff;
        if (resolved_mesh >= 10 && resolved_mesh <= 15)
            context.instance.Padding = context.draw.LodThreshold;
        else
            context.instance.Scale.w = context.draw.LodThreshold;
    }
    else
    {
        context.instance = Instances[instance_id];
    }
    if (HsShadowIsDraw(PassValue))
    {
        const HsShadowDrawRecord shadow = HsShadowLoadDraw(PassValue);
        context.geometry = LoadGeometryDrawRecord(shadow.geometry_record);
        context.geometryValid = true;
        context.rasterRecordId = 0xffffffffu;
    }
    else if (PassValue >= 4u)
    {
        context.geometry = LoadGeometryDrawRecord(PassValue >> 2u);
        context.geometryValid = true;
        context.rasterRecordId = context.geometry.Reserved2 + instance_id -
            context.geometry.VisibleBase;
    }
    return EvaluateSceneVertex(input, context, vertex_id,
        evaluate_history && !HsShadowIsDraw(PassValue));
}

SceneOutput SceneVS(SceneInput input, uint instance_id : SV_InstanceID, uint vertex_id : SV_VertexID)
{
    return EvaluateSceneVertex(input, GeometryVisibleInstance(instance_id), vertex_id,
                               (SceneDrawMaterial & 1u) != 0);
}

SceneOutput HeroCurrentVS(SceneInput input, uint instance_id : SV_InstanceID, uint vertex_id : SV_VertexID)
{
    // Only the nearest composite writes motion guides; other Hero draws need the current pose.
    return EvaluateSceneVertex(input, GeometryVisibleInstance(instance_id), vertex_id,
                               (SceneDrawMaterial & 1u) != 0, false);
}

SceneOutput ShadowVS(SceneInput input, uint instance_id : SV_InstanceID, uint vertex_id : SV_VertexID)
{
    // Shadows consume current coverage only; previous deformation is for motion vectors.
    SceneOutput output = EvaluateSceneVertex(input, GeometryVisibleInstance(instance_id), vertex_id,
        (SceneDrawMaterial & 1u) != 0, false);
    // Record IDs start at 1; legacy procedural draws retain cascades 0..2.
    if (HsShadowIsDraw(PassValue))
        output.Position = HsShadowProject(output.WorldPosition, PassValue);
    else
    {
        const uint cascade = PassValue >= 4u ? (PassValue & 3u) - 1u : PassValue;
        output.Position = mul(float4(output.WorldPosition, 1.0), ShadowViewProjection[cascade]);
    }
    return output;
}
void EnvironmentShadowPS(SceneOutput input, bool masked, bool front)
{
    if(input.Mesh==22u && input.Material==1u) discard;
    HsShadowRasterClip(input.Position.xy, PassValue);
    const HsMaterialContext material_context = EnvironmentMaterialContext(input, front);
    EnvironmentLodClip(input);
    if (masked) clip(HsMaterialEvaluateOpacity(material_context) - HsMaterialOpacityCutoff);
}

void ShadowPS(SceneOutput input, bool front : SV_IsFrontFace)
{
    if(input.Mesh==22u && input.Material==1u) discard;
    HsShadowRasterClip(input.Position.xy, PassValue);
    const HsMaterialContext material_context = EnvironmentMaterialContext(input, front);
    EnvironmentLodClip(input);
    if (input.Mesh == 17 || input.Mesh == 19)
        clip(HsMaterialEvaluateOpacity(material_context) - HsMaterialOpacityCutoff);
    else if (!front) discard;
}

void EnvironmentOpaqueShadowPS(SceneOutput input, bool front : SV_IsFrontFace)
{
    EnvironmentShadowPS(input, false, front);
}

void EnvironmentMaskedShadowPS(SceneOutput input, bool front : SV_IsFrontFace)
{
    EnvironmentShadowPS(input, true, front);
}

struct GBufferOutput
{
    float4 BaseColor : SV_Target0;
    float2 Normal : SV_Target1;
    uint2 Metadata : SV_Target2;
    float4 Material : SV_Target3;
    float2 Motion : SV_Target4;
    float2 History : SV_Target5;
#if HS_SURFACE_SIDECAR_PLANES > 0
    uint4 Sidecar0 : SV_Target6;
#endif
#if HS_SURFACE_SIDECAR_PLANES > 1
    uint4 Sidecar1 : SV_Target7;
#endif
};

float4 SampleArcherDiffuse(uint material, float2 uv, HsSceneGradients gradients)
{
    const uint slice = min(material, max(ParticleOptions.w, 1u) - 1u);
    return ArcherDiffuse.SampleGrad(MaterialSampler, float3(uv, slice), gradients.uvDx, gradients.uvDy);
}

float3 SampleArcherNormal(uint material, float2 uv, HsSceneGradients gradients)
{
    const uint slice = min(material, max(ParticleOptions.w, 1u) - 1u);
    return ArcherNormal.SampleGrad(MaterialSampler, float3(uv, slice), gradients.uvDx, gradients.uvDy).xyz;
}

float3 SampleMonsterPbr(float2 uv, HsSceneGradients gradients, out float3 ram)
{
    const float3 basecolor = MonsterBasecolor.SampleGrad(MaterialSampler, uv, gradients.uvDx, gradients.uvDy).rgb;
    ram = MonsterRam.SampleGrad(MaterialSampler, uv, gradients.uvDx, gradients.uvDy).rgb;
    const float roughness = ram.r;
    const float metallic = ram.b;
    const float3 lit_base = basecolor * lerp(0.75, 1.0, roughness);
    const float3 metal_tint = lerp(float3(1.0, 1.0, 1.0), basecolor, metallic);
    return lit_base * metal_tint;
}

// Both passes share the facial alpha mask: replaced eyes must not leave opaque
// depth behind the jelly, and the new marks must stay solid in the depth pass.
float4 SampleFamilyColor(SceneOutput input, HsSceneGradients gradients)
{
    const float3 family_uv = float3(input.Uv, input.Mesh - 10);
    float4 color = FamilyDiffuse.SampleGrad(MaterialSampler, family_uv,
                                            gradients.uvDx, gradients.uvDy);
    const bool dead = input.AnimationClip == 4;
    const float strain = input.AnimationClip == 2
        ? smoothstep(0.0, 0.35, input.AnimationProgress) : 0.0;
    if (!dead && strain <= 0.0) return color;
    const float2 authored_uv = float2((input.Uv.x - 0.015) / 0.72, 1.0 - input.Uv.y);
    const float2 authored_dx = float2(gradients.uvDx.x / 0.72, -gradients.uvDx.y);
    const float2 authored_dy = float2(gradients.uvDy.x / 0.72, -gradients.uvDy.y);
    const bool left_eye = authored_uv.x < 0.5;
    const float2 eye_center = float2(left_eye ? 0.434 : 0.566, 0.573);
    const float2 eye = authored_uv - eye_center;
    const float2 eye_dx = authored_dx;
    const float2 eye_dy = authored_dy;
    const float2 ellipse_eye = eye / float2(0.0365, 0.081);
    const float2 ellipse_dx_eye = eye_dx / float2(0.0365, 0.081);
    const float2 ellipse_dy_eye = eye_dy / float2(0.0365, 0.081);
    const float ellipse = length(ellipse_eye);
    float2 ellipse_gradient = 0.0;
    if (ellipse > 0.0)
        ellipse_gradient = ellipse_eye / ellipse;
    const float ellipse_dx = dot(ellipse_gradient, ellipse_dx_eye);
    const float ellipse_dy = dot(ellipse_gradient, ellipse_dy_eye);
    const float region_aa = max(abs(ellipse_dx) + abs(ellipse_dy), 0.001);
    const float region = 1.0 - smoothstep(1.0 - region_aa, 1.0 + region_aa, ellipse);
    if (region <= 0.0) return color;
    const float2 body_uv = float2((eye_center.x + (left_eye ? -0.09 : 0.09)) * 0.72 + 0.015,
                                1.0 - eye_center.y);
    // This is a fixed authored body-color anchor, not the varying surface UV.
    float4 body = FamilyDiffuse.SampleGrad(MaterialSampler,
                                           float3(body_uv, input.Mesh - 10),
                                           float2(0.0, 0.0), float2(0.0, 0.0));
    body.a = 0.68;
    const float2 q = eye / float2(0.032, 0.073);
    const float2 q_dx = eye_dx / float2(0.032, 0.073);
    const float2 q_dy = eye_dy / float2(0.032, 0.073);
    // Normalized diagonal strokes stay within the original eye's UV island.
    float stroke_distance;
    float stroke_dx;
    float stroke_dy;
    if (dead)
    {
        const float diagonal_a = q.y - q.x;
        const float diagonal_b = q.y + q.x;
        const float2 diagonal_a_dxdy = float2(q_dx.y - q_dx.x, q_dy.y - q_dy.x);
        const float2 diagonal_b_dxdy = float2(q_dx.y + q_dx.x, q_dy.y + q_dy.x);
        if (abs(diagonal_a) <= abs(diagonal_b))
        {
            stroke_distance = abs(diagonal_a);
            stroke_dx = diagonal_a == 0.0 ? 0.0 : sign(diagonal_a) * diagonal_a_dxdy.x;
            stroke_dy = diagonal_a == 0.0 ? 0.0 : sign(diagonal_a) * diagonal_a_dxdy.y;
        }
        else
        {
            stroke_distance = abs(diagonal_b);
            stroke_dx = diagonal_b == 0.0 ? 0.0 : sign(diagonal_b) * diagonal_b_dxdy.x;
            stroke_dy = diagonal_b == 0.0 ? 0.0 : sign(diagonal_b) * diagonal_b_dxdy.y;
        }
    }
    else
    {
        const float slope = left_eye ? -0.48 : 0.48;
        const float diagonal = q.y - slope * q.x;
        stroke_distance = abs(diagonal);
        stroke_dx = diagonal == 0.0 ? 0.0 : sign(diagonal) * (q_dx.y - slope * q_dx.x);
        stroke_dy = diagonal == 0.0 ? 0.0 : sign(diagonal) * (q_dy.y - slope * q_dy.x);
    }
    const float stroke_aa = max(abs(stroke_dx) + abs(stroke_dy), 0.01);
    const float stroke = (1.0 - smoothstep(0.15 - stroke_aa, 0.15 + stroke_aa, stroke_distance)) *
        (1.0 - smoothstep(0.70, 0.90, max(abs(q.x), abs(q.y))));
    const float4 expression = lerp(body, float4(0.009, 0.006, 0.012, 1.0), stroke);
    return lerp(color, expression, region * (dead ? 1.0 : strain));
}

float4 SampleFamilyColor(SceneOutput input)
{
    return SampleFamilyColor(input, RasterSceneGradients(input));
}

#ifdef HS_GENERATED_HERO_MATERIAL
HsMaterialContext HsHeroMaterialContext(SceneOutput input, bool front,
    HsSceneGradients gradients, uint ordinal)
{
    HsMaterialContext ctx = (HsMaterialContext)0;
    ctx.uv = input.Uv;
    ctx.d_uv_dx = gradients.uvDx;
    ctx.d_uv_dy = gradients.uvDy;
    ctx.actual_world_position = input.WorldPosition;
    ctx.d_actual_world_dx = gradients.worldDx;
    ctx.d_actual_world_dy = gradients.worldDy;
    ctx.stable_world_position = input.WorldPosition;
    ctx.d_stable_world_dx = gradients.worldDx;
    ctx.d_stable_world_dy = gradients.worldDy;
    ctx.mesh = input.Mesh;
    ctx.seed = input.EnvironmentSeed;
    ctx.eye_distance = length(CameraTime.xyz - input.WorldPosition);
    ctx.view_direction = normalize(CameraTime.xyz - input.WorldPosition);
    ctx.parameter_base = HeroMaterialParameterBases[ordinal / 4u][ordinal % 4u];
    ctx.sampled_ao = 1.0;
    ctx.sampled_alpha = 1.0;
    ctx.sampled_roughness = 0.3;
    float3 normal = normalize(input.WorldNormal);
    float3 tangent = input.WorldTangent.xyz - normal * dot(normal, input.WorldTangent.xyz);
    const float tangent_length_squared = dot(tangent, tangent);
    tangent = tangent_length_squared > 0.0 ? tangent * rsqrt(tangent_length_squared) : HsBsdfTangent(normal);
    const float3 bitangent = cross(normal, tangent) * input.WorldTangent.w;
    ctx.normal = normal;
    ctx.tangent = float4(tangent, input.WorldTangent.w);
    if (input.Mesh == 0u || (input.Mesh >= 10u && input.Mesh <= 12u))
    {
        const bool family = input.Mesh >= 10u;
        const uint slice = family ? input.Mesh - 10u : input.Material;
        float4 color;
        float4 normal_map;
        if (family)
        {
            color = SampleFamilyColor(input, gradients);
            normal_map = FamilyNormal.SampleGrad(MaterialSampler, float3(input.Uv, slice), gradients.uvDx, gradients.uvDy);
            if (input.Mesh == 12u)
            {
                const float3 charge_color = input.Charge < 0.5
                    ? lerp(float3(1.0, 0.26, 0.025), float3(1.0, 0.065, 0.012), input.Charge * 2.0)
                    : lerp(float3(1.0, 0.065, 0.012), float3(0.95, 0.008, 0.018), input.Charge * 2.0 - 1.0);
                color.rgb *= charge_color / float3(1.0, 0.26, 0.025);
            }
        }
        else
        {
            color = ArcherDiffuse.SampleGrad(MaterialSampler, float3(input.Uv, slice), gradients.uvDx, gradients.uvDy);
            normal_map = float4(ArcherNormal.SampleGrad(MaterialSampler, float3(input.Uv, slice), gradients.uvDx, gradients.uvDy).xyz, 0.3);
        }
        ctx.sampled_base_color = color.rgb;
        ctx.sampled_alpha = color.a;
        ctx.sampled_roughness = normal_map.a;
        float3 tangent_normal = normal_map.xyz * 2.0 - 1.0;
        // Existing family UV.v conversion requires the Blender green-channel flip.
        if (family) tangent_normal.y = -tangent_normal.y;
        normal = normalize(tangent * tangent_normal.x + bitangent * tangent_normal.y + normal * tangent_normal.z);
    }
    else if (input.Mesh >= 13u && input.Mesh <= 15u)
    {
        ctx.sampled_base_color = MonsterBasecolor.SampleGrad(MaterialSampler, input.Uv, gradients.uvDx, gradients.uvDy).rgb;
        const float3 ram = MonsterRam.SampleGrad(MaterialSampler, input.Uv, gradients.uvDx, gradients.uvDy).rgb;
        ctx.sampled_roughness = ram.r;
        ctx.sampled_metallic = ram.b;
        ctx.sampled_ao = ram.g;
        ctx.sampled_emission = MonsterEmissive.SampleGrad(MaterialSampler, input.Uv, gradients.uvDx, gradients.uvDy).rgb;
    }
    ctx.shading_normal = front ? normal : -normal;
    return ctx;
}

bool HsHeroSceneCovered(SceneOutput input, bool front, HsSceneGradients gradients)
{
    const uint ordinal = HsHeroOrdinal(input.MaterialId);
    if (!front || !HsHeroOpaqueOrdinalValid(ordinal) || HsHeroAssetIndex(input.Mesh) == HsHeroInvalidOrdinal)
        return false;
    return HsHeroEvaluateOpacity(HsHeroMaterialContext(input, front, gradients, ordinal), ordinal) >= HsMaterialOpacityCutoff;
}

HsSurfaceClosure HsHeroClosureToWorld(HsSurfaceClosure surface, HsMaterialContext ctx)
{
    if (!surface.normal_is_world)
    {
        const float3 n = ctx.shading_normal;
        const float3 t = normalize(ctx.tangent.xyz - n * dot(n, ctx.tangent.xyz));
        const float3 b = cross(n, t) * ctx.tangent.w;
        surface.normal = t * surface.normal.x + b * surface.normal.y + n * surface.normal.z;
        surface.tangent = t * surface.tangent.x + b * surface.tangent.y + n * surface.tangent.z;
    }
    surface.normal = normalize(surface.normal);
    surface.normal_is_world = true;
    return surface;
}
#endif

bool HsEnvironmentLodCovered(SceneOutput input)
{
    if (((input.Mesh < 16 || input.Mesh > 19) && input.Mesh != 21) ||
        (input.EnvironmentFlags & 0x10000) == 0u)
        return true;
    const uint2 cell = (uint2)input.Position.xy;
    const uint hash = EnvHash(cell.x ^ EnvHash(cell.y));
    const float sample_value = (hash & 0xffffff) / 16777216.0;
    const bool below = sample_value < saturate(input.EnvironmentFade);
    const bool complement = (input.EnvironmentFlags & 0x20000) != 0u;
    return below != complement;
}

bool HsSceneCovered(SceneOutput input, bool front, bool environment_opaque,
                    HsSceneGradients gradients, uint materialWord,
                    bool defer_environment_opacity = false)
{
    if (input.Mesh==22u && input.Material==1u) return false;
    if (!HsEnvironmentLodCovered(input) ||
        (!front && input.Mesh != 17 && input.Mesh != 19) ||
        input.Mesh == 6)
        return false;

    if (input.Mesh == 0 || (input.Mesh >= 10 && input.Mesh <= 12))
    {
        const bool family = input.Mesh >= 10;
        const float4 base_color = family
            ? SampleFamilyColor(input, gradients)
            : SampleArcherDiffuse(input.Material, input.Uv, gradients);
        if (base_color.a < (family ? 0.98 : 0.2))
            return false;
    }

    if (input.Mesh == 9 || input.Mesh == 20 ||
        ((input.Mesh >= 16 && input.Mesh <= 19) || input.Mesh == 21 || input.Mesh == 22))
    {
        const HsMaterialContext material_context =
            EnvironmentMaterialContext(input, front, gradients, materialWord);
        if (!defer_environment_opacity && !environment_opaque && (input.Mesh == 17 || input.Mesh == 19))
        {
            if (HsMaterialEvaluateOpacity(material_context) < HsMaterialOpacityCutoff)
                return false;
        }
    }
    return true;
}

// Integral of a unit-period line centered at each integer. Integrating over
// the explicit pixel footprint keeps the diagnostic itself free of aliasing.
float2 HsGridLineIntegral(float2 position, float half_width)
{
    const float2 phase = frac(position);
    return floor(position) * (2.0 * half_width) +
        min(phase, half_width) + max(phase - (1.0 - half_width), 0.0);
}

float HsGridCoverage(float2 position, float2 footprint, float half_width)
{
    footprint = max(footprint, 0.000001);
    const float2 coverage = saturate((
        HsGridLineIntegral(position + footprint * 0.5, half_width) -
        HsGridLineIntegral(position - footprint * 0.5, half_width)) / footprint);
    return 1.0 - (1.0 - coverage.x) * (1.0 - coverage.y);
}

GBufferOutput HsEvaluateSceneSurface(SceneOutput input, bool front,
                                     bool environment_opaque,
                                     HsSceneGradients gradients,
                                     uint materialWord,
                                     bool cover_environment_surface = false)
{
    const HsMaterialContext material_context =
        EnvironmentMaterialContext(input, front, gradients, materialWord);
    GBufferOutput output;
    output.BaseColor = input.Color;
    output.Material = float4(0.3, 0.0, 1.0, 0.0);
    output.Metadata = uint2(input.MaterialId, input.SurfaceFlags);
    output.Motion = 0.0;
    output.History = 0.0;
#if HS_SURFACE_SIDECAR_PLANES > 0
    output.Sidecar0 = 0u;
#endif
#if HS_SURFACE_SIDECAR_PLANES > 1
    output.Sidecar1 = 0u;
#endif
    const float4 current_clip = mul(float4(input.WorldPosition, 1.0), ViewProjection);
    const float4 previous_clip = mul(float4(input.PreviousWorldPosition, 1.0),
                                     PreviousViewProjection);
    const float4 current_unjittered_clip = mul(float4(input.WorldPosition, 1.0),
        CurrentUnjitteredViewProjection);
    const float4 previous_unjittered_clip = mul(float4(input.PreviousWorldPosition, 1.0),
        PreviousUnjitteredViewProjection);
    const bool history_valid = input.HistoryConfidence > 0.0 &&
        SurfaceViewOrigin.w > 0.5 &&
        current_clip.w > 0.0 && previous_clip.w > 0.0 &&
        current_unjittered_clip.w > 0.0 && previous_unjittered_clip.w > 0.0 &&
        all(isfinite(current_clip)) && all(isfinite(previous_clip)) &&
        all(isfinite(current_unjittered_clip)) && all(isfinite(previous_unjittered_clip));
    if (history_valid)
    {
        const float2 motion = HsSurfaceClipUv(previous_unjittered_clip) -
            HsSurfaceClipUv(current_unjittered_clip);
        const float previous_depth = previous_clip.z / previous_clip.w;
        if (all(isfinite(motion)) && isfinite(previous_depth))
        {
            output.Motion = motion;
            output.History = float2(previous_depth, input.HistoryConfidence);
        }
    }
    float3 world_normal = normalize(input.WorldNormal);
    float roughness = 0.3;
    if (input.Mesh == 0 || (input.Mesh >= 10 && input.Mesh <= 12))
    {
        const bool family = input.Mesh >= 10;
        const float3 uv = float3(input.Uv, input.Mesh - 10);
        const float4 base_color = family ? SampleFamilyColor(input, gradients)
            : SampleArcherDiffuse(input.Material, input.Uv, gradients);
        output.BaseColor = float4(base_color.rgb, 1.0);
        const float4 normal_map = family
            ? FamilyNormal.SampleGrad(MaterialSampler, uv,
                                      gradients.uvDx, gradients.uvDy)
            : float4(SampleArcherNormal(input.Material, input.Uv, gradients), 0.3);
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
    output.Material.r = roughness;
    if (input.Mesh >= 13 && input.Mesh <= 15)
    {
        float3 ram;
        output.BaseColor = float4(SampleMonsterPbr(input.Uv, gradients, ram), 1.0);
        output.Material = float4(ram.r, ram.b, ram.g, 0.0);
    }
    if ((LodClock.w & 1u) != 0 && (input.Mesh == 9 || input.Mesh == 20))
    {
        const float2 position = input.EnvironmentStableWorld.xz;
        const float2 footprint = abs(gradients.stableDx.xz) + abs(gradients.stableDy.xz);
        const float minor = HsGridCoverage(position, footprint, 0.025);
        const float major = HsGridCoverage(position / 5.0, footprint / 5.0, 0.012);
        output.BaseColor = float4(lerp(lerp(float3(0.045, 0.055, 0.065),
            float3(0.65, 0.65, 0.65), minor), float3(0.95, 0.75, 0.30), major), 1.0);
        world_normal = float3(0.0, 1.0, 0.0);
        output.Material = float4(1.0, 0.0, 1.0, 0.0);
    }
    else if (input.Mesh == 9 || input.Mesh == 20 || ((input.Mesh >= 16 && input.Mesh <= 19) || input.Mesh == 21 || input.Mesh == 22))
    {
        EnvMaterial m = (EnvMaterial)0;
#ifdef HS_GENERATED_ENVIRONMENT_MATERIAL
        HsSurfaceClosure surface;
        if (cover_environment_surface)
            surface = HsMaterialEvaluateCoveredSurface(material_context);
        else
            surface = HsMaterialEvaluateSurface(material_context);
#else
        HsSurfaceClosure surface = HsMaterialEvaluateSurface(material_context);
#endif
        if (input.Mesh == 19u)
            surface.base_color *= MeadowDryGrassGain(input.EnvironmentStableWorld.xz);
        {
            m.color = surface.base_color;
            m.roughness = surface.roughness;
            m.ao = surface.ao;
            if (surface.normal_is_world)
                m.normal = normalize(surface.normal);
            else
            {
                const float3 n = material_context.shading_normal;
                const float3 t = normalize(material_context.tangent.xyz -
                    n * dot(n, material_context.tangent.xyz));
                const float3 b = normalize(cross(n, t)) * material_context.tangent.w;
                m.normal = normalize(t * surface.normal.x + b * surface.normal.y + n * surface.normal.z);
                surface.tangent = t * surface.tangent.x + b * surface.tangent.y + n * surface.tangent.z;
            }
        }
        output.BaseColor=float4(m.color,1);world_normal=m.normal;
        output.Material=float4(m.roughness,saturate(surface.metallic),m.ao,0.0);
#if HS_SURFACE_SIDECAR_PLANES > 0
        const uint ordinal = input.MaterialId - 0x10000000u;
        const uint varying_mask = ordinal < HsAuthoredMaterialCount
            ? MaterialVaryingMasks[ordinal / 4u][ordinal % 4u] : 0u;
        if (varying_mask != 0u)
        {
            surface.normal = world_normal;
            surface.normal_is_world = true;
            if ((varying_mask & 1u) != 0u)
            {
                const float tangent_length_squared = dot(surface.tangent, surface.tangent);
                // The zero authored direction has the BSDF's default tangent.
                surface.tangent = tangent_length_squared > 0.0
                    ? surface.tangent * rsqrt(tangent_length_squared)
                    : HsBsdfTangent(world_normal);
            }
            uint4 plane0, plane1;
            HsPackMaterialSidecars(surface, varying_mask,
                MaterialSidecarOffsets[0], MaterialSidecarOffsets[1], plane0, plane1);
            output.Sidecar0 = plane0;
#if HS_SURFACE_SIDECAR_PLANES > 1
            output.Sidecar1 = plane1;
#endif
        }
#endif
    }
    output.Normal = HsEncodeSurfaceNormal(world_normal);
    return output;
}

#ifdef HS_GENERATED_HERO_MATERIAL
GBufferOutput HsHeroEvaluateSceneSurface(SceneOutput input, HsMaterialContext ctx,
    uint ordinal)
{
    HsSurfaceClosure surface = HsHeroClosureToWorld(HsHeroEvaluateSurface(ctx, ordinal), ctx);
    // Use the actual primary surface in both raster and Visibility evaluation.
    const float3 entity_world = HsReconstructSurfaceRelative(
        HsSurfaceRasterUv(int2(input.Position.xy)), input.Position.z) + SurfaceViewOrigin.xyz;
    const float4 entity_surface = HsEntitySurface(input.Mesh, input.InstanceKey, entity_world);
    surface.emission += entity_surface.rgb;
    const float3 normal = surface.normal;
    GBufferOutput output = (GBufferOutput)0;
    output.BaseColor = float4(surface.base_color, 1.0);
    output.Normal = HsEncodeSurfaceNormal(normal);
    output.Metadata = uint2(input.MaterialId, input.SurfaceFlags);
    output.Material = float4(surface.roughness, saturate(surface.metallic), surface.ao, 0.0);
    const float4 current_clip = mul(float4(input.WorldPosition, 1.0), ViewProjection);
    const float4 previous_clip = mul(float4(input.PreviousWorldPosition, 1.0), PreviousViewProjection);
    const float4 current_unjittered = mul(float4(input.WorldPosition, 1.0), CurrentUnjitteredViewProjection);
    const float4 previous_unjittered = mul(float4(input.PreviousWorldPosition, 1.0), PreviousUnjitteredViewProjection);
    const bool history_valid = input.HistoryConfidence > 0.0 && SurfaceViewOrigin.w > 0.5 &&
        current_clip.w > 0.0 && previous_clip.w > 0.0 && current_unjittered.w > 0.0 && previous_unjittered.w > 0.0 &&
        all(isfinite(current_clip)) && all(isfinite(previous_clip)) &&
        all(isfinite(current_unjittered)) && all(isfinite(previous_unjittered));
    if (history_valid)
    {
        const float2 motion = HsSurfaceClipUv(previous_unjittered) - HsSurfaceClipUv(current_unjittered);
        const float previous_depth = previous_clip.z / previous_clip.w;
        if (all(isfinite(motion)) && isfinite(previous_depth))
        {
            output.Motion = motion;
            output.History = float2(previous_depth, input.HistoryConfidence * (1.0 - entity_surface.a));
        }
    }
#if HS_SURFACE_SIDECAR_PLANES > 0
    const uint varying_mask = HeroMaterialVaryingMasks[ordinal / 4u][ordinal % 4u];
    if ((varying_mask & 1u) != 0u)
    {
        const float tangent_length_squared = dot(surface.tangent, surface.tangent);
        surface.tangent = tangent_length_squared > 0.0
            ? surface.tangent * rsqrt(tangent_length_squared) : HsBsdfTangent(normal);
    }
    uint4 plane0, plane1;
    HsPackMaterialSidecars(surface, varying_mask, MaterialSidecarOffsets[0], MaterialSidecarOffsets[1], plane0, plane1);
    output.Sidecar0 = plane0;
#if HS_SURFACE_SIDECAR_PLANES > 1
    output.Sidecar1 = plane1;
#endif
#endif
    return output;
}

GBufferOutput HsHeroEvaluateSceneSurface(SceneOutput input, bool front,
    HsSceneGradients gradients)
{
    const uint ordinal = HsHeroOrdinal(input.MaterialId);
    return HsHeroEvaluateSceneSurface(input,
        HsHeroMaterialContext(input, front, gradients, ordinal), ordinal);
}

GBufferOutput HeroScenePS(SceneOutput input, bool front : SV_IsFrontFace)
{
    const HsSceneGradients gradients = RasterSceneGradients(input);
    const uint ordinal = HsHeroOrdinal(input.MaterialId);
    if (!front || !HsHeroOpaqueOrdinalValid(ordinal) || HsHeroAssetIndex(input.Mesh) == HsHeroInvalidOrdinal) discard;
    const HsMaterialContext ctx = HsHeroMaterialContext(input, front, gradients, ordinal);
    if (HsHeroEvaluateOpacity(ctx, ordinal) < HsMaterialOpacityCutoff) discard;
    return HsHeroEvaluateSceneSurface(input, ctx, ordinal);
}

void HeroShadowPS(SceneOutput input, bool front : SV_IsFrontFace)
{
    if(input.Mesh==22u && input.Material==1u) discard;
    HsShadowRasterClip(input.Position.xy, PassValue);
    const HsSceneGradients gradients = RasterSceneGradients(input);
    if (!HsHeroSceneCovered(input, front, gradients)) discard;
}
#endif

GBufferOutput ScenePSImpl(SceneOutput input, bool front, bool environment_opaque)
{
    // Derivatives must be captured before coverage can diverge this raster
    // quad. Shared visibility paths pass their own explicit gradients.
    const HsSceneGradients gradients = RasterSceneGradients(input);
    // Only authored masked raster evaluates coverage and surface together.
    // Visibility/shadow keep their opacity-only evaluator and resolve stays unclipped.
    bool cover_environment_surface = false;
#ifdef HS_GENERATED_ENVIRONMENT_MATERIAL
    cover_environment_surface = !environment_opaque && (input.Mesh == 17 || input.Mesh == 19);
#endif
    if (!HsSceneCovered(input, front, environment_opaque, gradients,
                        SceneDrawMaterial, cover_environment_surface))
        discard;
    return HsEvaluateSceneSurface(input, front, environment_opaque, gradients,
                                  SceneDrawMaterial, cover_environment_surface);
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
    float cached_visibility;
    if (HsVirtualShadowLookup(world, cached_visibility)) return cached_visibility;
    // Only a CPU-proven complete VSM plan may omit CSM. Never read the old or
    // uncreated atlas in that mode; this guard does not establish coverage.
#ifndef HS_LOCAL_PROBE_CAPTURE
    if (HsShadowMetadata.Load(324u) != 0u && asfloat(HsShadowMetadata.Load(348u)) == 0.0)
        return 1.0;
#endif
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
        const uint samples = uint(ShadowAtlasTexelSize.w);
        [loop] for (uint sample_index = 0; sample_index < samples; ++sample_index)
        {
                const float2 sample_uv = clamp(
                    atlas_uv + float2(HsShadowFilterOffsets[sample_index]) * ShadowAtlasTexelSize.xy, lower, upper);
                filtered += ShadowMap.SampleCmpLevelZero(ShadowCompare, sample_uv, projected.z);
        }
        const float edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
        visibility = lerp(visibility, filtered / float(samples), smoothstep(0.02, 0.12, edge));
    }
    return visibility;
}

// Shared lighting for opaque family features and transparent jelly bodies.
float3 VfxLocalLighting(float3 base, float3 normal, float3 world)
{
    HsSurfaceClosure surface=HsMaterialMakeSurfaceClosure(base,normal,1.0,0.0,1.0,1.0);
    surface.normal_is_world=true;
    return HsLightListEvaluate(surface,world,CameraTime.xyz,1.0,HsLightLocalMask,true);
}

float3 VfxLocalLighting(HsSurfaceClosure surface, float3 world)
{
    return HsLightListEvaluate(surface,world,CameraTime.xyz,1.0,HsLightLocalMask,true);
}

float3 JellyLighting(float3 base, float3 normal, float3 world, float roughness, float charge)
{
    const float3 view = normalize(CameraTime.xyz - world);
    float3 light,primary_incident;
    HsPrimaryDirectional(light,primary_incident);
    const float nv = saturate(dot(normal, view));
    const float nl = dot(normal, light);
    const float visibility = lerp(0.42, 1.0, ShadowVisibility(world));
    const float fresnel = 0.035 + 0.965 * pow(1.0 - nv, 5.0);
    const float thickness = pow(nv, 1.5);
    const float internal = thickness * (0.05 + 0.15 * saturate(-nl) + charge * 0.45);
    HsSurfaceClosure surface=HsMaterialMakeSurfaceClosure(base,normal,roughness,0.0,1.0,1.0);
    surface.normal_is_world=true;
    return HsDeferredIndirectDiffuse(base*0.30)+base*internal+lerp(base,float3(0.70,0.85,1.0),0.30)*fresnel*0.32+
        HsLightListEvaluate(surface,world,CameraTime.xyz,visibility,HsLightAllMask,true);
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

struct HsDeferredOutput
{
    float4 Radiance : SV_Target0;
    float4 SubsurfaceDiffuse : SV_Target1;
    float4 EnvironmentSpecular : SV_Target2;
    float4 IndirectDiffuse : SV_Target3;
};

void HsDeferredLocalLightingComponents(HsSurfaceClosure closure, float3 world, float base_fresnel_override,bool world_query,
    out float3 diffuse, out float3 specular)
{
    HsLightListEvaluateComponentsOptical(closure,world,CameraTime.xyz,1.0,
        HsLightLocalMask,world_query,base_fresnel_override,diffuse,specular);
}
void HsDeferredLocalLightingComponents(HsSurfaceClosure closure,float3 world,float base_fresnel_override,
    out float3 diffuse,out float3 specular)
{
    HsDeferredLocalLightingComponents(closure,world,base_fresnel_override,false,diffuse,specular);
}
void HsDeferredLocalLightingComponents(HsSurfaceClosure closure,float3 world,
    out float3 diffuse,out float3 specular)
{
    HsDeferredLocalLightingComponents(closure,world,-1.0,diffuse,specular);
}

HsDeferredOutput DeferredPS(FullScreenOutput input)
{
    HsDeferredOutput output=(HsDeferredOutput)0;
    const HsSurfaceSample surface = HsLoadSurface(input.Uv);
    const bool valid = HsSurfaceValid(surface);
    float3 world = valid ? HsSurfaceWorldPosition(surface) : 0.0;
    const float3 arena = float3(0.035, 0.055, 0.085);
    output.Radiance=float4(arena,1.0);
    if (!valid)
    {
        if(ProbeOptions.w>0.0)
        {
            const float3 near_world=HsReconstructSurfaceRelative(input.Uv,1.0)+SurfaceViewOrigin.xyz;
            output.Radiance=float4(EnvironmentSkyRadiance(normalize(near_world-CameraTime.xyz)),1.0);
        }
        return output;
    }
    float4 base = GBufferBase.Load(int3(surface.pixel, 0));
    float3 normal = HsDecodeSurfaceNormal(GBufferNormal.Load(int3(surface.pixel, 0)));
    const float4 material = GBufferMaterial.Load(int3(surface.pixel, 0));
    const uint model = HsSurfaceModel(surface.metadata);
    const uint hero_ordinal=HsHeroOrdinal(surface.metadata.x);
    if (model == HsSurfaceModelEnvironment || HsHeroOpaqueOrdinalValid(hero_ordinal))
    {
#ifdef HS_GENERATED_DEFERRED_MATERIALS
        HsSurfaceClosure closure=HsReflectionLoadClosure(uint2(surface.pixel));
#else
        HsSurfaceClosure closure = HsMaterialMakeSurfaceClosure(
            base.rgb, normal, material.r, material.g, material.b, 1.0);
        closure.normal_is_world = true;
        closure=HsApplyDecalSurfaceEmission(closure,uint2(surface.pixel));
#endif
        if(closure.unlit)
        {
            output.Radiance=float4(closure.base_color+closure.emission,1.0);
            return output;
        }
        closure.ao *= LightingOptions.x > 0.0 ? SceneAmbientOcclusion.Load(int3(surface.pixel, 0)) : 1.0;
        const float contact = LightingOptions.y > 0.0 ? SceneContactShadow.Load(int3(surface.pixel, 0)) : 1.0;
        float3 diffuse,specular,ambient_specular,ambient_diffuse,local_diffuse,local_specular;
        EnvironmentLightingComponents(closure,world,ShadowVisibility(world)*contact,-1.0,
            diffuse,specular,ambient_specular,ambient_diffuse);
        HsDeferredLocalLightingComponents(closure,world,local_diffuse,local_specular);
        diffuse+=local_diffuse;
        specular+=local_specular;
        output.Radiance=float4(diffuse+specular+closure.emission,1.0);
        if(closure.sss_weight>0.0) output.SubsurfaceDiffuse=float4(diffuse,1.0);
        output.EnvironmentSpecular=HsDeferredEnvironmentSpecular(ambient_specular);
        output.IndirectDiffuse=float4(ambient_diffuse,1.0);
        return output;
    }
    if (model == HsSurfaceModelJelly)
    {
        output.Radiance=float4(JellyLighting(base.rgb,normal,world,material.r,0.0),1.0);
        return output;
    }
    float rim = pow(1.0 - saturate(normal.y), 3.0);
    float visibility = base.a > 0.0 ? lerp(0.42, 1.0, ShadowVisibility(world)) : 1.0;
    visibility *= LightingOptions.y > 0.0 ? SceneContactShadow.Load(int3(surface.pixel, 0)) : 1.0;
    const float ambient = LightingOptions.x > 0.0 ? SceneAmbientOcclusion.Load(int3(surface.pixel, 0)) : 1.0;
    HsSurfaceClosure closure=HsMaterialMakeSurfaceClosure(base.rgb,normal,1.0,0.0,ambient,1.0);
    closure.normal_is_world=true;
    float3 color=HsDeferredIndirectDiffuse(base.rgb*(0.35*ambient))+base.rgb*rim*0.22+
        HsLightListEvaluate(closure,world,CameraTime.xyz,visibility,HsLightAllMask,false);
    output.Radiance=float4(lerp(arena,color,base.a),1.0);
    output.IndirectDiffuse=float4(HsDeferredIndirectDiffuse(base.rgb*(0.35*ambient))*base.a,1.0);
    return output;
}

// Native software GI receives an independent scene-linear light cache seed.
// Evaluate the same direct lighting and emission as DeferredPS, without its
// probe diffuse, environment specular, exposure, transparent or post terms.
float4 BrixelizerCacheDirectPS(FullScreenOutput input) : SV_Target0
{
    const HsSurfaceSample surface = HsLoadSurface(input.Uv);
    const bool valid = HsSurfaceValid(surface);
    float3 world = valid ? HsSurfaceWorldPosition(surface) : 0.0;
    if (!valid) return 0.0;
    const float4 base = GBufferBase.Load(int3(surface.pixel, 0));
    const uint model = HsSurfaceModel(surface.metadata);
    const uint hero_ordinal = HsHeroOrdinal(surface.metadata.x);
    if (model == HsSurfaceModelJelly) return 0.0;
    if (model != HsSurfaceModelEnvironment && !HsHeroOpaqueOrdinalValid(hero_ordinal))
    {
        const float3 normal = HsDecodeSurfaceNormal(GBufferNormal.Load(int3(surface.pixel, 0)));
        const float ambient = LightingOptions.x > 0.0 ? SceneAmbientOcclusion.Load(int3(surface.pixel, 0)) : 1.0;
        HsSurfaceClosure cel = HsMaterialMakeSurfaceClosure(base.rgb, normal, 1.0, 0.0, ambient, 1.0);
        cel.normal_is_world = true;
        float visibility = base.a > 0.0 ? lerp(0.42, 1.0, ShadowVisibility(world)) : 1.0;
        visibility *= LightingOptions.y > 0.0 ? SceneContactShadow.Load(int3(surface.pixel, 0)) : 1.0;
        float3 diffuse, specular;
        HsLightListEvaluateComponentsOptical(cel, world, CameraTime.xyz, visibility,
            HsLightAllMask, false, -1.0, diffuse, specular);
        return float4(diffuse * base.a, 1.0);
    }
#ifdef HS_GENERATED_DEFERRED_MATERIALS
    HsSurfaceClosure closure = HsReflectionLoadClosure(uint2(surface.pixel));
#else
    const float3 normal = HsDecodeSurfaceNormal(GBufferNormal.Load(int3(surface.pixel, 0)));
    const float4 material = GBufferMaterial.Load(int3(surface.pixel, 0));
    HsSurfaceClosure closure = HsMaterialMakeSurfaceClosure(base.rgb, normal,
        material.r, material.g, material.b, 1.0);
    closure.normal_is_world = true;
    closure=HsApplyDecalSurfaceEmission(closure,uint2(surface.pixel));
#endif
    if (closure.unlit) return float4(closure.base_color + closure.emission, 1.0);
    closure.ao *= LightingOptions.x > 0.0 ? SceneAmbientOcclusion.Load(int3(surface.pixel, 0)) : 1.0;
    const float contact = LightingOptions.y > 0.0 ? SceneContactShadow.Load(int3(surface.pixel, 0)) : 1.0;
    const float visibility = ShadowVisibility(world) * contact;
    float3 diffuse, specular;
    // The directional mask plus local mask is exactly the deferred direct set.
    HsLightListEvaluateComponentsOptical(closure, world, CameraTime.xyz, visibility,
        HsLightDirectionalMask, false, -1.0, diffuse, specular);
    float3 local_diffuse, local_specular;
    HsDeferredLocalLightingComponents(closure, world, local_diffuse, local_specular);
    const float coverage = model == HsSurfaceModelEnvironment || HsHeroOpaqueOrdinalValid(hero_ordinal) ? 1.0 : base.a;
    return float4((diffuse + local_diffuse + closure.emission) * coverage, 1.0);
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
#include "particle_collision.hlsli"

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
    float3 camera_forward = LodViewForward.xyz;
    float3 axis_x = LodViewRight.xyz;
    float3 axis_y = LodViewUp.xyz;
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
    float2 corner = corners[vertex_id % 6u];
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
    float2 local = corner * half_extent;
    // Circumscribed outside, inscribed hole: the existing analytic mask keeps the exact radii.
    const bool bounded_annulus = (command.Metadata.z == 5u || command.Metadata.z == 6u) &&
        command.ShapeExtentSpeedMin.x > command.ShapeExtentSpeedMin.y * 0.45;
    if (bounded_annulus)
    {
        const uint segment = vertex_id / 6u;
        const uint segment_vertex = vertex_id % 6u;
        const bool next = segment_vertex == 2u || segment_vertex == 4u || segment_vertex == 5u;
        const bool outer_vertex = segment_vertex == 1u || segment_vertex == 2u || segment_vertex == 4u;
        const float angle = float(segment + (next ? 1u : 0u)) * (-6.28318530718 / 32.0);
        const float radius = outer_vertex ? command.ShapeExtentSpeedMin.y / 0.99518472667 + 0.0001
                                         : max(0.0, command.ShapeExtentSpeedMin.x - 0.0001);
        local = float2(cos(angle), sin(angle)) * radius;
        corner = local / half_extent;
    }
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
    float local_z = local.z, longitudinal_derivative = 1.0;
    const float head_ratio = command.RotationRange.y;
    if (head_ratio > 0.0)
    {
        const float minimum_z = command.EndColor.x, hinge_z = command.EndColor.y, maximum_z = command.EndColor.z;
        const float target_hinge = lerp(minimum_z, maximum_z, 1.0 - head_ratio);
        const bool tail = local.z < hinge_z;
        longitudinal_derivative = tail ? (target_hinge - minimum_z) / (hinge_z - minimum_z)
                                      : (maximum_z - target_hinge) / (maximum_z - hinge_z);
        local_z = tail ? minimum_z + (local.z - minimum_z) * longitudinal_derivative
                       : target_hinge + (local.z - hinge_z) * longitudinal_derivative;
    }
    const float3 normal = normalize(axis_x * (local_normal.x / scale.x) + axis_y * (local_normal.y / scale.y) +
                                    forward * (local_normal.z / (scale.z * longitudinal_derivative)));
    const float3 world = command.PositionLifetimeMin.xyz +
        axis_x * (local.x * scale.x) + axis_y * (local.y * scale.y) +
        forward * ((local_z - 0.5) * scale.z) + normal * command.RotationRange.z;

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
    const HsSceneGradients gradients = RasterSceneGradients(input);
    float4 base = float4(0.08, 0.68, 0.12, 0.72);
    float3 normal = normalize(input.WorldNormal);
    float roughness = 0.25;
    if (input.Mesh != 6)
    {
        const float3 uv = float3(input.Uv, input.Mesh - 10);
        base = SampleFamilyColor(input, gradients);
        if (base.a >= 0.98) discard; // opaque eyes and ornaments wrote depth already
        const float4 normal_map = FamilyNormal.SampleGrad(MaterialSampler, uv, gradients.uvDx, gradients.uvDy);
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
    const float3 color = HsFogSurface(
        JellyLighting(base.rgb, normal, input.WorldPosition, roughness, input.Charge),
        input.WorldPosition, input.Position.xy);
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

#ifdef HS_GENERATED_HERO_MATERIAL
cbuffer HeroTransparencyConstants : register(b0, space14)
{
    uint4 HeroLayerInfo; // current layer, layer budget, phase, reserved.
};
Texture2D<float> HeroFrontDepth[4] : register(t0, space14);
Texture2D<uint2> HeroFrontIdentity[4] : register(t4, space14);
Texture2D<float> HeroExitDepth[4] : register(t8, space14);
Texture2D<float4> HeroLayerBackground : register(t12, space14);
Texture2D<uint> HeroOverflow : register(t13, space14);
RWTexture2D<uint> HeroOverflowCounter : register(u0, space14);

float3 HsHeroLayerBackground(float3 world, float3 direction, float optical_path, float2 pixel)
{
    // Rejected displaced lookups retain the valid source behind this interface.
    float3 background=HeroLayerBackground.Load(int3(int2(pixel),0)).rgb;
    const float3 exit_world=world+direction*optical_path;
    const float4 exit_clip=mul(float4(exit_world,1.0),ViewProjection);
    if(exit_clip.w<=0.0) return background;
    const float2 uv=HsSurfaceClipUv(exit_clip);
    if(any(uv<0.0) || any(uv>=1.0)) return background;
    const uint2 sample_pixel=uint2(uv*ScreenSize.xy);
    float scene_depth=SurfaceDepth.Load(int3(sample_pixel,0));
    const uint next=HeroLayerInfo.x+1u;
    if(next<HeroLayerInfo.y)
    {
        const float next_depth=HeroFrontDepth[next].Load(int3(sample_pixel,0));
        if(next_depth>scene_depth) scene_depth=next_depth;
    }
    // The far-to-near source has coverage from the next front or the opaque
    // surface. A displaced lookup may never pull a foreground silhouette.
    if(scene_depth>exit_clip.z/exit_clip.w) return background;
    if(scene_depth>0.0)
    {
        const float3 scene_world=HsReconstructSurfaceRelative(
            HsSurfaceRasterUv(int2(sample_pixel)),scene_depth)+SurfaceViewOrigin.xyz;
        if(dot(scene_world-exit_world,direction)<0.0) return background;
    }
    return HeroLayerBackground.Load(int3(sample_pixel,0)).rgb;
}

// Authored-thickness weighted OIT source path. This has one opaque scene guide;
// measured front/back thickness and bounded multi-interface depth peeling are
// separate transport paths, not implied by this screen-space lookup.
float3 HsHeroTranslucentLighting(HsSurfaceClosure surface, float3 world,
    float2 pixel, bool front, float measured_path, bool layer_transport)
{
#ifdef HS_CAPTURE_HERO_WORK
    if(IndirectArguments.Load(28u)==2u) { HsHeroWorkAdd(27u); return 0.0; }
#endif
    const float3 view=normalize(CameraTime.xyz-world);
    float3 normal=normalize(surface.normal);
    if(dot(normal,view)<0.0) normal=-normal;
    surface.normal=normal;
    const float solid=saturate(surface.solid_transmission_weight);
    const float eta=front ? 1.0/surface.ior : surface.ior;
    const float3 transmitted=refract(-view,normal,eta);
    const bool total_internal=solid>0.0 && dot(transmitted,transmitted)==0.0;
    const float base_fresnel_override=total_internal ? 1.0 : -1.0;
    float3 diffuse,specular,ambient_specular,ambient_diffuse,local_diffuse,local_specular;
    EnvironmentLightingComponents(solid<1.0,surface,world,ShadowVisibility(world),base_fresnel_override,
        diffuse,specular,ambient_specular,ambient_diffuse);
    HsDeferredLocalLightingComponents(surface,world,base_fresnel_override,true,local_diffuse,local_specular);
    diffuse+=local_diffuse;
    specular+=local_specular;
    if(solid==0.0) return HsFogSurface(diffuse+specular+surface.emission,world,pixel);
    const float nv=saturate(dot(normal,view));
    const float fresnel=total_internal ? 1.0 :
        HsBsdfSchlickFresnel(HsBsdfSolidDielectricF0(surface).xxx,nv).x;
    if(total_internal)
    {
        // The actual solid interface has unit Fresnel under TIR. The shared
        // evaluator already applied it to the authored solid fraction, with
        // original GGX/anisotropy/layers/proposal/PDF and one ambient sample loop.
        return HsFogSurface(diffuse*(1.0-solid)+specular+surface.emission,world,pixel);
    }
    const float optical_path=measured_path>=0.0 ? measured_path :
        surface.thickness_m/max(abs(dot(transmitted,normal)),1e-4);
    float3 background=EnvironmentSkyRadiance(transmitted);
    float2 background_uv=pixel*ScreenSize.zw;
    float background_view_depth=VmDepthRange.x;
    const bool fog_enabled=HsFogEnabled();
    if(layer_transport) background=HeroLayerBackground.Load(int3(int2(pixel),0)).rgb;
    else
    {
        const int3 source_pixel=int3(int2(pixel),0);
        const float source_depth=SurfaceDepth.Load(source_pixel);
        if(source_depth>0.0)
        {
            const float3 source_world=HsReconstructSurfaceRelative(
                HsSurfaceRasterUv(int2(pixel)),source_depth)+SurfaceViewOrigin.xyz;
            background=fog_enabled ? VmRawOpaqueHdr.Load(source_pixel).rgb : HdrColor.Load(source_pixel).rgb;
            background_view_depth=HsFogViewDepth(source_world);
        }
    }
    const float3 exit_world=world+transmitted*optical_path;
    const float4 exit_clip=mul(float4(exit_world,1.0),ViewProjection);
    if(layer_transport && MaterialQualityOptions.w>0.0)
        background=HsHeroLayerBackground(world,transmitted,optical_path,pixel);
    else if(!layer_transport && MaterialQualityOptions.w>0.0 && exit_clip.w>0.0)
    {
        const float2 uv=HsSurfaceClipUv(exit_clip);
        if(all(uv>=0.0) && all(uv<1.0))
        {
            background_uv=uv;
            const uint2 sample_pixel=uint2(uv*ScreenSize.xy);
            const float depth=SurfaceDepth.Load(int3(sample_pixel,0));
            // Reverse-Z depth must lie behind the authored exit plane. Reject
            // foreground silhouettes instead of refracting through them.
            if(depth>0.0 && depth<=exit_clip.z/exit_clip.w)
            {
                const float3 opaque_world=HsReconstructSurfaceRelative(
                    HsSurfaceRasterUv(int2(sample_pixel)),depth)+SurfaceViewOrigin.xyz;
                if(dot(opaque_world-exit_world,transmitted)>=0.0)
                {
                    if(fog_enabled)
                    {
                        background=VmRawOpaqueHdr.Load(int3(sample_pixel,0)).rgb;
                        background_view_depth=HsFogViewDepth(opaque_world);
                    }
                    else background=HdrColor.Load(int3(sample_pixel,0)).rgb;
                }
            }
        }
    }
    const float coat=saturate(surface.coat_weight);
    const float coat_transmission=(1.0-coat*HsBsdfCoatFresnel(nv))*
        (1.0-coat*HsBsdfCoatFresnel(abs(dot(transmitted,normal))));
    // The base closure supplies the authored interface/transmission tint.
    // Bulk Beer absorption remains the separately authored inverse-metre RGB.
    const float3 transmission_weight=saturate(surface.base_color)*HsBsdfSolidBeerLambert(surface,optical_path)*
        (solid*(1.0-fresnel)*coat_transmission);
    const float3 raw_surface=diffuse*(1.0-solid)+specular+surface.emission;
    if(!fog_enabled) return raw_surface+transmission_weight*background;
    const HsVolumetricTransport prefix=HsFogPrefix(world,pixel);
    if(layer_transport)
    {
        // B is already camera-domain radiance, including its own fog. This
        // bounded screen-refraction model uses the current camera prefix;
        // subtract that prefix once, with no transmittance division.
        return prefix.radiance+prefix.transmittance*raw_surface+
            transmission_weight*(background-prefix.radiance);
    }
    // Behind the interface, integrate only the front-to-background interval.
    // A missing opaque exit uses the global sky at the fog far distance.
    const HsVolumetricTransport interval=HsSampleVolumetricInterval(background_uv,
        HsFogViewDepth(world),background_view_depth);
    const float3 incoming=interval.radiance+interval.transmittance*background;
    return prefix.radiance+prefix.transmittance*(raw_surface+transmission_weight*incoming);
}

float3 HsHeroTranslucentLighting(HsSurfaceClosure surface, float3 world, float2 pixel, bool front)
{
    return HsHeroTranslucentLighting(surface,world,pixel,front,-1.0,false);
}

HsSurfaceClosure HsHeroTransportSurface(SceneOutput input, bool front, HsSceneGradients gradients)
{
    HS_HERO_WORK_COUNT(18u);
    if(input.Mesh==22u && input.Material==1u)
    {
        HS_HERO_WORK_COUNT(14u);
        const float thickness=max(0.001,input.Uv.x);
        HsSurfaceClosure water=HsMaterialMakeSurfaceClosure(float3(.97,.985,.96),float3(0,1,0),.18,0,1,1);
        water.solid_transmission_weight=1; water.ior=1.333;
        // Suspended soil removes blue first; clear-water absorption made shallow mud look icy.
        water.absorption_per_m=float3(3.0,8.0,16.0);water.thickness_m=thickness;
        water.normal_is_world=true;return water;
    }
    const uint transport_ordinal=5u;
    const uint ordinal=HsHeroBindingOrdinals(input.Mesh,input.Material).y;
    if(ordinal!=transport_ordinal) { HS_HERO_WORK_COUNT(11u); discard; }
    // discard does not let DXC narrow the later dynamic material switch.
    const HsMaterialContext ctx=HsHeroMaterialContext(input,front,gradients,transport_ordinal);
    HS_HERO_WORK_COUNT(13u);
    const HsSurfaceClosure surface=HsHeroClosureToWorld(HsHeroEvaluateSurface(ctx,transport_ordinal),ctx);
#ifdef HS_CAPTURE_HERO_WORK
    if(surface.opacity-0.001<0.0) HsHeroWorkAdd(12u);
#endif
    clip(surface.opacity-0.001);
    return surface;
}

bool HsHeroSelectedFront(SceneOutput input,uint layer)
{
    const int3 pixel=int3(int2(input.Position.xy),0);
    return input.Position.z==HeroFrontDepth[layer].Load(pixel) &&
        all(input.InstanceKey==HeroFrontIdentity[layer].Load(pixel));
}

uint2 HeroPeelFrontPS(SceneOutput input,bool front:SV_IsFrontFace HS_HERO_WORK_COVERAGE):SV_Target0
{
    const HsSceneGradients gradients=RasterSceneGradients(input);
    HS_HERO_WORK_ENTRY(input.Position.xy);
#ifdef HS_CAPTURE_HERO_WORK
    if(!front) { HsHeroWorkAdd(3u); discard; }
    if(input.InstanceKey.x==0u) { HsHeroWorkAdd(4u); discard; }
#else
    if(!front || input.InstanceKey.x==0u) discard;
#endif
    const int3 pixel=int3(int2(input.Position.xy),0);
    const float opaque_depth=SurfaceDepth.Load(pixel);
    if(input.Position.z<=opaque_depth) { HS_HERO_WORK_COUNT(5u); discard; }
    if(HeroLayerInfo.x>0u)
    {
        const float previous=HeroFrontDepth[HeroLayerInfo.x-1u].Load(pixel);
        if(previous<=0.0 || input.Position.z>=previous) { HS_HERO_WORK_COUNT(6u); discard; }
    }
    const HsSurfaceClosure surface=HsHeroTransportSurface(input,front,gradients);
    HS_HERO_WORK_ACCEPT(input.Position.xy);
    return input.InstanceKey;
}

void HeroPeelExitPS(SceneOutput input,bool front:SV_IsFrontFace HS_HERO_WORK_COVERAGE)
{
    const HsSceneGradients gradients=RasterSceneGradients(input);
    HS_HERO_WORK_ENTRY(input.Position.xy);
    if(front) { HS_HERO_WORK_COUNT(3u); discard; }
    const int3 pixel=int3(int2(input.Position.xy),0);
    const uint layer=HeroLayerInfo.x;
    const float entry=HeroFrontDepth[layer].Load(pixel);
#ifdef HS_CAPTURE_HERO_WORK
    if(entry<=0.0) { HsHeroWorkAdd(7u); discard; }
    if(input.Position.z>=entry) { HsHeroWorkAdd(8u); discard; }
    if(any(input.InstanceKey!=HeroFrontIdentity[layer].Load(pixel))) { HsHeroWorkAdd(9u); discard; }
#else
    if(entry<=0.0 || input.Position.z>=entry ||
        any(input.InstanceKey!=HeroFrontIdentity[layer].Load(pixel))) discard;
#endif
    const HsSurfaceClosure surface=HsHeroTransportSurface(input,front,gradients);
    HS_HERO_WORK_ACCEPT(input.Position.xy);
}

[earlydepthstencil]
OitOutput HeroOitTailPS(SceneOutput input,bool front:SV_IsFrontFace HS_HERO_WORK_COVERAGE)
{
    const HsSceneGradients gradients=RasterSceneGradients(input);
    HS_HERO_WORK_ENTRY(input.Position.xy);
    // The read-only opaque DSV rejects z <= opaque before this shader runs.
    if(!front) { HS_HERO_WORK_COUNT(3u); discard; }
    [loop] for(uint layer=0u;layer<HeroLayerInfo.y;++layer)
        if(HsHeroSelectedFront(input,layer)) { HS_HERO_WORK_COUNT(10u); discard; }
    const HsSurfaceClosure surface=HsHeroTransportSurface(input,front,gradients);
    HS_HERO_WORK_ACCEPT(input.Position.xy);
    uint ignored;
    InterlockedAdd(HeroOverflowCounter[int2(input.Position.xy)],1u,ignored);
    const float3 color=HsHeroTranslucentLighting(surface,input.WorldPosition,input.Position.xy,front);
    const float alpha=saturate(surface.opacity);
    const float z=saturate(length(input.WorldPosition-CameraTime.xyz)/180.0);
    const float weight=clamp(pow(min(1.0,alpha*10.0)+0.01,3.0)*1e8*pow(1.0-z*0.9,3.0),1e-2,3e3);
    OitOutput output;
    output.Accumulation=float4(color*alpha*weight,alpha*weight);
    output.Revealage=alpha.xxxx;
    return output;
}

struct HeroTransparentGuideOutput
{
    float Depth;
    float2 Motion;
    float2 History;
    float4 Classification;
    uint2 Metadata;
    float2 Transport;
};

HsSurfaceClosure HsHeroSelectedSurface(SceneOutput input,bool front,uint hsHeroCoverage)
{
    const HsSceneGradients gradients=RasterSceneGradients(input);
    HS_HERO_WORK_ENTRY(input.Position.xy);
    const uint layer=HeroLayerInfo.x;
#ifdef HS_CAPTURE_HERO_WORK
    if(!front) { HsHeroWorkAdd(3u); discard; }
    if(input.Position.z!=HeroFrontDepth[layer].Load(int3(int2(input.Position.xy),0)))
    { HsHeroWorkAdd(8u); discard; }
    if(any(input.InstanceKey!=HeroFrontIdentity[layer].Load(int3(int2(input.Position.xy),0))))
    { HsHeroWorkAdd(9u); discard; }
#else
    if(!front || !HsHeroSelectedFront(input,layer)) discard;
#endif
    const HsSurfaceClosure surface=HsHeroTransportSurface(input,front,gradients);
    HS_HERO_WORK_ACCEPT(input.Position.xy);
    return surface;
}

HeroTransparentGuideOutput HsHeroSurfaceGuides(SceneOutput input,HsSurfaceClosure surface)
{
    HeroTransparentGuideOutput output=(HeroTransparentGuideOutput)0;
    output.Depth=input.Position.z;
    output.Classification=float4(HsEncodeSurfaceNormal(surface.normal),saturate(surface.opacity),surface.roughness);
    output.Metadata=input.InstanceKey;
    output.Transport=float2(surface.solid_transmission_weight>0.0?1.0:0.0,any(surface.emission>0.0)?1.0:0.0);
    const float4 current=mul(float4(input.WorldPosition,1.0),CurrentUnjitteredViewProjection);
    const float4 previous=mul(float4(input.PreviousWorldPosition,1.0),PreviousUnjitteredViewProjection);
    const float4 previous_jittered=mul(float4(input.PreviousWorldPosition,1.0),PreviousViewProjection);
    const bool valid=input.HistoryConfidence>0.0 && SurfaceViewOrigin.w>0.5 &&
        current.w>0.0 && previous.w>0.0 && previous_jittered.w>0.0 &&
        all(isfinite(current)) && all(isfinite(previous)) && all(isfinite(previous_jittered)) &&
        HeroOverflow.Load(int3(int2(input.Position.xy),0))==0u;
    if(valid)
    {
        output.Motion=HsSurfaceClipUv(previous)-HsSurfaceClipUv(current);
        output.History=float2(previous_jittered.z/previous_jittered.w,input.HistoryConfidence);
    }
    return output;
}

float4 HsHeroCompositeSurface(SceneOutput input,bool front,HsSurfaceClosure surface)
{
    const uint layer=HeroLayerInfo.x;
    const float3 view=normalize(CameraTime.xyz-input.WorldPosition);
    const float3 geometric_normal=normalize(input.WorldNormal);
    float3 normal=normalize(surface.normal);
    if(dot(normal,view)<0.0) normal=-normal;
    const float3 transmitted=refract(-view,normal,1.0/surface.ior);
    const int3 pixel=int3(int2(input.Position.xy),0);
    const float exit_depth=HeroExitDepth[layer].Load(pixel);
    float path=-1.0;
    if(exit_depth>0.0 && dot(transmitted,transmitted)>0.0)
    {
        const float3 exit_world=HsReconstructSurfaceRelative(
            HsSurfaceRasterUv(int2(input.Position.xy)),exit_depth)+SurfaceViewOrigin.xyz;
        const float denominator=abs(dot(transmitted,geometric_normal));
        if(denominator>0.0)
            path=length(exit_world-input.WorldPosition)*abs(dot(view,geometric_normal))/denominator;
    }
    // The paired exit provides a measured local slab. Open geometry uses only
    // the authored metre-valued thickness; another instance can never supply it.
    const float3 color=HsHeroTranslucentLighting(surface,input.WorldPosition,input.Position.xy,front,path,true);
    const float3 background=HeroLayerBackground.Load(pixel).rgb;
    return float4(lerp(background,color,saturate(surface.opacity)),1.0);
}

struct HeroCompositeGuideOutput
{
    float4 Color:SV_Target0;
    float Depth:SV_Target1;
    float2 Motion:SV_Target2;
    float2 History:SV_Target3;
    float4 Classification:SV_Target4;
    uint2 Metadata:SV_Target5;
    float2 Transport:SV_Target6;
};

// The existing guide product is the final layer0 composite. Reuse its original
// surface before transport lighting or view-facing normal orientation.
[earlydepthstencil]
HeroCompositeGuideOutput HeroTransparentGuidePS(SceneOutput input,bool front:SV_IsFrontFace HS_HERO_WORK_COVERAGE)
{
#ifdef HS_CAPTURE_HERO_WORK
    const HsSurfaceClosure surface=HsHeroSelectedSurface(input,front,hsHeroCoverage);
#else
    const HsSurfaceClosure surface=HsHeroSelectedSurface(input,front,0u);
#endif
    const HeroTransparentGuideOutput guides=HsHeroSurfaceGuides(input,surface);
    HeroCompositeGuideOutput output;
    output.Color=HsHeroCompositeSurface(input,front,surface);
    output.Depth=guides.Depth;
    output.Motion=guides.Motion;
    output.History=guides.History;
    output.Classification=guides.Classification;
    output.Metadata=guides.Metadata;
    output.Transport=guides.Transport;
    return output;
}

[earlydepthstencil]
float4 HeroLayerCompositePS(SceneOutput input,bool front:SV_IsFrontFace HS_HERO_WORK_COVERAGE):SV_Target0
{
#ifdef HS_CAPTURE_HERO_WORK
    const HsSurfaceClosure surface=HsHeroSelectedSurface(input,front,hsHeroCoverage);
#else
    const HsSurfaceClosure surface=HsHeroSelectedSurface(input,front,0u);
#endif
    return HsHeroCompositeSurface(input,front,surface);
}

OitOutput HeroTranslucentPS(SceneOutput input, bool front : SV_IsFrontFace)
{
    const HsSceneGradients gradients = RasterSceneGradients(input);
    HsSurfaceClosure surface = HsHeroTransportSurface(input, front, gradients);
    const float3 color = HsHeroTranslucentLighting(surface, input.WorldPosition, input.Position.xy, front);
    const float alpha = saturate(surface.opacity);
    const float z = saturate(length(input.WorldPosition - CameraTime.xyz) / 180.0);
    const float weight = clamp(pow(min(1.0, alpha * 10.0) + 0.01, 3.0) *
        1e8 * pow(1.0 - z * 0.9, 3.0), 1e-2, 3e3);
    OitOutput output;
    output.Accumulation = float4(color * alpha * weight, alpha * weight);
    output.Revealage = alpha.xxxx;
    return output;
}
#endif

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
        if (input.Primitive == 11)
        {
            float fresnel = pow(1.0 - saturate(abs(dot(input.Normal, view_direction))), 1.5);
            alpha *= 0.25 + 0.75 * fresnel;
        }
        HsSurfaceClosure surface=HsMaterialMakeSurfaceClosure(input.Color.rgb,normalize(input.Normal),1.0,0.0,1.0,1.0);
        surface.normal_is_world=true;
        input.Color.rgb=HsDeferredIndirectDiffuse(input.Color.rgb*0.35)+HsLightListEvaluate(surface,input.WorldPosition,
            CameraTime.xyz,1.0,HsLightAllMask,true)*0.65;
    }
    clip(alpha - 0.001);
    if (input.Renderer == 0 || input.Renderer == 3)
    {
        float2 screen_uv = input.Position.xy / ScreenSize.xy;
        const HsSurfaceSample surface = HsLoadSurface(screen_uv);
        if (HsSurfaceValid(surface))
        {
            float3 scene_world = HsSurfaceWorldPosition(surface);
            float scene_depth = dot(scene_world - CameraTime.xyz, CameraForwardSoftness.xyz);
            float particle_depth = dot(input.WorldPosition - CameraTime.xyz, CameraForwardSoftness.xyz);
            alpha *= saturate((scene_depth - particle_depth) / CameraForwardSoftness.w);
        }
    }
    float linear_depth = length(input.WorldPosition - CameraTime.xyz);
    float z = saturate(linear_depth / 180.0);
    float weight = clamp(pow(min(1.0, alpha * 10.0) + 0.01, 3.0) *
                         1e8 * pow(1.0 - z * 0.9, 3.0), 1e-2, 3e3);
    OitOutput output;
    output.Accumulation = float4(HsFogSurface(input.Color.rgb, input.WorldPosition,
        input.Position.xy) * alpha * weight, alpha * weight);
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
    return float4(HsFogEmission(color.rgb, input.WorldPosition, input.Position.xy) * alpha, alpha);
}

OitOutput OwnerMeshPS(ParticleOutput input)
{
    const float4 gradient = SampleVfxGradient(input.GroundGradient.x, saturate(input.Progress));
    const float alpha = input.Color.a * gradient.a;
    clip(alpha - 0.001);

    const float3 normal = normalize(input.Normal);
    const float3 view_direction = normalize(CameraTime.xyz - input.WorldPosition);
    const float fresnel = pow(1.0 - saturate(abs(dot(normal, view_direction))), 1.5);
    const float3 base=gradient.rgb*input.GroundGradient.y;
    HsSurfaceClosure surface=HsMaterialMakeSurfaceClosure(base,normal,1.0,0.0,1.0,1.0);
    surface.normal_is_world=true;
    const float3 direct=HsLightListEvaluate(surface,input.WorldPosition,CameraTime.xyz,1.0,HsLightAllMask,true);
    const float3 color=(HsDeferredIndirectDiffuse(base*0.35)+direct*0.65)*(1.0-0.35*saturate(input.SegmentMetadata.x))+
        base*fresnel*input.SegmentMetadata.x;
    const float linear_depth = length(input.WorldPosition - CameraTime.xyz);
    const float z = saturate(linear_depth / 180.0);
    const float weight = clamp(pow(min(1.0, alpha * 10.0) + 0.01, 3.0) *
                              1e8 * pow(1.0 - z * 0.9, 3.0), 1e-2, 3e3);
    OitOutput output;
    output.Accumulation = float4(HsFogSurface(color, input.WorldPosition, input.Position.xy) * alpha * weight,
        alpha * weight);
    output.Revealage = alpha.xxxx;
    return output;
}

Texture2D<float4> VfxDistortionVectors : register(t15, space3);
Texture2D<float2> VfxFlowCurl : register(t16, space3);
struct DistortionGpu
{
    float4 PositionRadius;
    float4 DirectionStrength;
    float4 PhaseAlphaShapeSeed;
    float4 Geometry;
    uint4 GapRange;
};
StructuredBuffer<DistortionGpu> VfxDistortions : register(t18, space3);

float2 HsActiveRenderTextureUv(float2 logical_uv, uint2 physical_extent)
{
    return clamp(logical_uv * ScreenSize.xy, 0.5, ScreenSize.xy - 0.5) /
        float2(physical_extent);
}

float4 CompositePS(FullScreenOutput input) : SV_Target0
{
    uint distortion_width, distortion_height;
    VfxDistortionVectors.GetDimensions(distortion_width, distortion_height);
    const float4 vectors = VfxDistortionVectors.SampleLevel(LinearClamp,
        HsActiveRenderTextureUv(input.Uv, uint2(distortion_width, distortion_height)), 0);
    // Normalize overlapping coverage while retaining single-source fade strength.
    float2 offset = clamp(vectors.xy/max(vectors.w,1.0),-0.04,0.04);
    const float2 border = min(input.Uv,1.0-input.Uv);
    offset *= smoothstep(0.0,0.04,min(border.x,border.y));
    float2 warped_uv = saturate(input.Uv + offset);
    // Reject displaced samples which cross foreground geometry in front of the distortion.
    const HsSurfaceSample warped_surface = HsLoadSurface(warped_uv);
    float visibility = 1.0;
    if (HsSurfaceValid(warped_surface) && vectors.w > 0.0)
    {
        const float3 warped_world = HsSurfaceWorldPosition(warped_surface);
        const float warped_depth = dot(warped_world-CameraTime.xyz,CameraForwardSoftness.xyz);
        const float source_depth = vectors.z/vectors.w;
        visibility = saturate((warped_depth-source_depth)/0.35);
    }
    warped_uv = saturate(input.Uv + offset*visibility);
    uint hdr_width, hdr_height, accumulation_width, accumulation_height;
    uint revealage_width, revealage_height;
    HdrColor.GetDimensions(hdr_width, hdr_height);
    OitAccumulation.GetDimensions(accumulation_width, accumulation_height);
    OitRevealage.GetDimensions(revealage_width, revealage_height);
    float3 opaque = HdrColor.SampleLevel(LinearClamp,
        HsActiveRenderTextureUv(warped_uv, uint2(hdr_width, hdr_height)), 0).rgb;
    float4 accumulation = OitAccumulation.SampleLevel(LinearClamp,
        HsActiveRenderTextureUv(input.Uv, uint2(accumulation_width, accumulation_height)), 0);
    float revealage = OitRevealage.SampleLevel(LinearClamp,
        HsActiveRenderTextureUv(input.Uv, uint2(revealage_width, revealage_height)), 0);
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
    output.Accumulation = float4(HsFogSurface(color, input.WorldPosition, input.Position.xy) * alpha * weight,
        alpha * weight);
    output.Revealage = alpha.xxxx;
    return output;
}

GBufferOutput ScenePS(SceneOutput input, bool front : SV_IsFrontFace)
{
    return ScenePSImpl(input, front, false);
}

GBufferOutput EnvironmentOpaquePS(SceneOutput input, bool front : SV_IsFrontFace)
{
    return ScenePSImpl(input, front, true);
}

GBufferOutput EnvironmentMaskedPS(SceneOutput input, bool front : SV_IsFrontFace)
{
    return ScenePSImpl(input, front, false);
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
    float3 axis_x = LodViewRight.xyz;
    float3 axis_y = LodViewUp.xyz;
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
    const HsSurfaceSample surface = HsLoadSurface(screen_uv);
    if (HsSurfaceValid(surface))
    {
        float3 scene_world = HsSurfaceWorldPosition(surface);
        float scene_depth = dot(scene_world - CameraTime.xyz, CameraForwardSoftness.xyz);
        float flash_depth = dot(input.WorldPosition - CameraTime.xyz, CameraForwardSoftness.xyz);
        coverage *= saturate((scene_depth - flash_depth) / CameraForwardSoftness.w);
    }
    return float4(HsFogEmission(input.Color.rgb, input.WorldPosition, input.Position.xy) *
        input.Color.a * coverage, 0.0);
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
        const float3 forward = LodViewForward.xyz;
        const float3 right = LodViewRight.xyz;
        const float3 up = LodViewUp.xyz;
        // Shadow palette colors the scattering; only baked emissive follows the hot gradient.
        const float3 scattering=HsLightListSixWay(input.WorldPosition,right,up,forward,
            positive.rgb,negative.rgb,HsLightAllMask,false);
        color = (HsDeferredIndirectDiffuse(input.Color.rgb * 0.18) + input.Color.rgb * scattering +
                 gradient.rgb * input.Gradient.y * negative.a) * detail;
        // Artistic mask modulates RGB only; baked positive alpha is the smoke silhouette.
        alpha *= positive.a * input.SurfaceData.y;
    }
    else
    {
        const uint octaves = input.SurfaceMetadata.x;
        const float2 flow = float2(0.0, -seconds * input.SurfaceData.z);
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
    const HsSurfaceSample surface = HsLoadSurface(screen_uv);
    if (HsSurfaceValid(surface))
    {
        const float3 scene = HsSurfaceWorldPosition(surface);
        const float scene_depth = dot(scene - CameraTime.xyz, CameraForwardSoftness.xyz);
        const float sprite_depth = dot(input.WorldPosition - CameraTime.xyz, CameraForwardSoftness.xyz);
        alpha *= saturate((scene_depth - sprite_depth) / CameraForwardSoftness.w);
    }
    alpha = saturate(alpha);
    const float z = saturate(length(input.WorldPosition - CameraTime.xyz) / 180.0);
    const float weight = clamp(pow(min(1.0, alpha * 10.0) + 0.01, 3.0) * 1e8 * pow(1.0 - z * 0.9, 3.0), 1e-2, 3e3);
    OitOutput output;
    output.Accumulation = float4(HsFogSurface(color, input.WorldPosition, input.Position.xy) * alpha * weight,
        alpha * weight);
    output.Revealage = alpha.xxxx;
    return output;
}

float3 BloomBrightPass(float3 color)
{
    const float brightness = max(max(color.r, color.g), color.b);
    const float knee = smoothstep(0.8, 1.2, brightness);
    return color * knee * saturate((brightness - 0.6) / max(brightness, 0.0001));
}

float4 PreExposurePS(FullScreenOutput input) : SV_Target0
{
    return float4(CompositeHdr.Load(int3(HsSurfacePixel(input.Uv), 0)).rgb * CurrentExposure[0], 1);
}

struct TemporalMrtOutput
{
    float4 ColorDepth : SV_Target0;
    float2 Normal : SV_Target1;
    uint2 Material : SV_Target2;
};

TemporalMrtOutput TemporalPS(FullScreenOutput input)
{
    // Output/history coordinates are unjittered. Current color and guides live
    // in the jittered active render rectangle inside the capacity allocation.
    const float2 current_uv = input.Uv + SurfaceJitterUv.xy;
    const HsSurfaceSample surface = HsLoadSurface(current_uv);
    const bool valid = HsSurfaceValid(surface);
    const float2 surface_history = SurfaceHistory.Load(int3(surface.pixel, 0));
    const float current_exposure = CurrentExposure[0];
    const float3 composite = CompositeHdr.Load(int3(surface.pixel, 0)).rgb * current_exposure;
    const float3 opaque = HdrColor.Load(int3(surface.pixel, 0)).rgb * current_exposure;
    const float3 difference = abs(composite - opaque);

    HsTemporalInput resolve = (HsTemporalInput)0;
    resolve.uv = input.Uv;
    resolve.render_extent = uint2(ScreenSize.xy);
    resolve.output_extent = uint2(OutputSize.xy);
    resolve.current_jitter_uv = SurfaceJitterUv.xy;
    resolve.previous_jitter_uv = SurfaceJitterUv.zw;
    resolve.current_depth = valid ? surface.depth : 0.0;
    resolve.expected_previous_depth = surface_history.x;
    resolve.current_normal = valid
        ? HsDecodeSurfaceNormal(GBufferNormal.Load(int3(surface.pixel, 0))) : float3(0, 0, 1);
    resolve.material_key = valid ? surface.metadata : uint2(0, 0);
    resolve.motion_uv = GBufferMotion.Load(int3(surface.pixel, 0));
    resolve.history_confidence = valid ? surface_history.y : 1.0;
    resolve.reactive = saturate(max(difference.r, max(difference.g, difference.b)) /
        max(1.0, max(composite.r, max(composite.g, composite.b))));
    if ((LodClock.w & 8u) != 0u)
        resolve.reactive = max(resolve.reactive, VfxScreenReactive.Load(int3(surface.pixel, 0)));
    if((LodClock.w&4u)!=0u)
    {
        const int3 guide_pixel=int3(surface.pixel,0);
        const float hero_depth=HeroGuideDepth.Load(guide_pixel);
        const float4 hero_class=HeroGuideClassification.Load(guide_pixel);
        if(hero_depth>resolve.current_depth && hero_class.z>0.0)
        {
            const float2 hero_history=HeroGuideHistory.Load(guide_pixel);
            resolve.current_depth=hero_depth;
            resolve.expected_previous_depth=hero_history.x;
            resolve.history_confidence=hero_history.y;
            resolve.motion_uv=HeroGuideMotion.Load(guide_pixel);
            resolve.current_normal=HsDecodeSurfaceNormal(hero_class.xy);
            resolve.material_key=HeroGuideMetadata.Load(guide_pixel);
            resolve.reactive=max(resolve.reactive,hero_class.z*(1.0-saturate(hero_history.y)));
        }
        if(HeroGuideOverflow.Load(guide_pixel)>0u) resolve.reactive=1.0;
    }
    resolve.current_pre_exposure = current_exposure;
    resolve.previous_pre_exposure = CurrentExposure[1];
    resolve.history_valid = TemporalOptions.x > 0.5 && TemporalOptions.y > 0.5;
    resolve.current_camera_origin = CameraTime.xyz;
    resolve.current_inverse_unjittered_view_projection = InverseUnjitteredViewProjection;
    resolve.previous_unjittered_view_projection = PreviousUnjitteredViewProjection;

    const HsTemporalResult resolved = HsResolveTemporal(resolve, SceneWork, TemporalInput,
        TemporalNormalInput, TemporalMaterialInput);
    TemporalMrtOutput output;
    output.ColorDepth = resolved.color_depth;
    output.Normal = resolved.normal_oct;
    output.Material = resolved.material_key;
    return output;
}

float3 BloomSource(float2 uv)
{
    return PostOptions.w > 0.0
        ? HsSharpenTemporal(TemporalOutput, LinearClamp, uv, uint2(OutputSize.xy), PostOptions.w)
        : TemporalOutput.SampleLevel(LinearClamp, uv, 0).rgb;
}

float4 BloomExtractPS(FullScreenOutput input) : SV_Target0
{
    const float2 texel = OutputSize.zw * 0.5;
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
    return float4(center + (RenderOptions.y > 0.5 ? halo * max(PostOptions.x, 0.0) : 0.0), 1.0);
}

float4 ToneMapPS(FullScreenOutput input) : SV_Target0
{
    // Apply sharpening once here when the graph bypasses bloom composition.
    const float3 scene = RenderOptions.y > 0.5 || PostOptions.z > 0.5
        ? PostB.SampleLevel(LinearClamp, input.Uv, 0).rgb : BloomSource(input.Uv);
    // No artistic grading transform exists in the current shader. The grading
    // toggle never bypasses the required scene-to-display tone map.
    const float3 display_linear = DisplayOptions.x > 0.5
        ? HsDisplayToneMapHdr(scene, DisplayOptions.y, DisplayOptions.z)
        : HsDisplayToneMapSdr(scene);
    return float4(display_linear, 1.0);
}

float4 OutlinePS(FullScreenOutput input) : SV_Target0
{
    float2 texel = OutputSize.zw * max(PostOptions.y, 0.0);
    const float2 guide_uv = input.Uv + SurfaceJitterUv.xy;
    const float center_mask = HsSampleSurfaceOutlineMask(guide_uv);
    float edge = 0.0;
    static const float2 offsets[8] = {
        float2(-1, -1), float2(0, -1), float2(1, -1), float2(-1, 0),
        float2(1, 0), float2(-1, 1), float2(0, 1), float2(1, 1)
    };
    for (uint index = 0; index < 8; ++index)
    {
        const float neighbor_mask = HsSampleSurfaceOutlineMask(guide_uv + offsets[index] * texel);
        edge = max(edge, abs(center_mask - neighbor_mask));
    }
    float3 color = PostA.SampleLevel(LinearClamp, input.Uv, 0).rgb;
    float outline = RenderOptions.z > 0.5 ? smoothstep(0.1, 0.9, edge) : 0.0;
    return float4(lerp(color, color * 0.08, outline), 1.0);
}

float4 FxaaPS(FullScreenOutput input) : SV_Target0
{
    float2 texel = OutputSize.zw;
    float3 center = PostB.SampleLevel(LinearClamp, input.Uv, 0).rgb;
    if (TemporalOptions.x > 0.5 || (LodClock.w & 2u) != 0) return float4(center, 1.0);
    float3 north = PostB.SampleLevel(LinearClamp, input.Uv + float2(0, texel.y), 0).rgb;
    float3 south = PostB.SampleLevel(LinearClamp, input.Uv - float2(0, texel.y), 0).rgb;
    float3 east = PostB.SampleLevel(LinearClamp, input.Uv + float2(texel.x, 0), 0).rgb;
    float3 west = PostB.SampleLevel(LinearClamp, input.Uv - float2(texel.x, 0), 0).rgb;
    float luma_center = dot(center, float3(0.299, 0.587, 0.114));
    float luma_max = max(max(dot(north, float3(0.299, 0.587, 0.114)),
                               dot(south, float3(0.299, 0.587, 0.114))),
                           max(dot(east, float3(0.299, 0.587, 0.114)),
                               dot(west, float3(0.299, 0.587, 0.114))));
    float luma_min = min(min(dot(north, float3(0.299, 0.587, 0.114)),
                               dot(south, float3(0.299, 0.587, 0.114))),
                           min(dot(east, float3(0.299, 0.587, 0.114)),
                               dot(west, float3(0.299, 0.587, 0.114))));
    float luma_range = max(luma_center, luma_max) - min(luma_center, luma_min);
    float3 filtered = (north + south + east + west + center * 2.0) / 6.0;
    return float4(lerp(center, filtered, saturate((luma_range - 0.08) * 4.0)), 1.0);
}

float4 UiPS(FullScreenOutput input) : SV_Target0
{
    // CPU rasterization already maps the authored logical canvas into an
    // output-sized bitmap, including its transparent letterbox margins.
    float4 ui = HsDisplayDecodePremultipliedUi(UiTexture.SampleLevel(LinearClamp, input.Uv, 0));
    if (DisplayOptions.w > 0.5)
    {
        const float4 devtools = HsDisplayDecodePremultipliedUi(PostA.SampleLevel(LinearClamp, input.Uv, 0));
        ui = ui * (1.0 - devtools.a) + devtools;
    }
    const float3 scene = DisplayLinear.SampleLevel(LinearClamp, input.Uv, 0).rgb;
    const float3 display_linear = scene * (1.0 - ui.a) + ui.rgb;
    const float3 encoded = DisplayOptions.x > 0.5
        ? HsDisplayEncodeHdr10(display_linear, DisplayOptions.y, DisplayOptions.z)
        : HsDisplayEncodeSdr(display_linear);
    return float4(encoded, 1.0);
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
    const float3 right = LodViewRight.xyz;
    const float3 up = LodViewUp.xyz;
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

FresnelShellOutput FresnelShellVS(SceneInput input, uint vertex_id : SV_VertexID,
                                  uint instance_id : SV_InstanceID)
{
    // Indexed draws name the selected instance in the complete instance/ref
    // buffer, preserving the opaque surface's skin slot and unique vertex ID.
    const SceneOutput scene = SceneVS(input, instance_id, vertex_id);
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
    const uint visible_instance = GeometryVisibleInstance(instance_id);
    if ((SceneDrawMaterial & 1u) != 0u)
        output.Center = HsLoadGpuSceneInstance(StaticSceneDraws[visible_instance].InstanceIndex).Position.xyz;
    else
        output.Center = Instances[visible_instance].Position.xyz;
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
    const HsSurfaceSample surface = HsLoadSurface(screen_uv);
    const float depth = dot(input.World-CameraTime.xyz,CameraForwardSoftness.xyz);
    if(HsSurfaceValid(surface))
    {
        const float3 scene = HsSurfaceWorldPosition(surface);
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
