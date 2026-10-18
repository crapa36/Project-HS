// Native stochastic reflections. Hierarchy cells hold the nearest reversed-Z
// depth (MAX), independently of the farthest-depth culling pyramid.
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
    uint4 SkinCacheOptions;
    row_major float4x4 InverseViewProjection;
    row_major float4x4 CurrentUnjitteredViewProjection;
    row_major float4x4 PreviousUnjitteredViewProjection;
    float4 SurfaceViewOrigin;
    float4 SurfaceJitterUv;
    float4 LodViewRight;
    float4 LodViewUp;
    float4 LodViewForward;
    float4 LodProjection;
    float4 LodOptions;
    uint4 LodClock;
    uint4 MaterialParameterBases[4];
    uint4 MaterialVaryingMasks[4];
    uint4 MaterialSidecarOffsets[2];
    uint4 MaterialSidecarInfo;
    float4 OutputSize;
    float4 PostOptions;
    float4 MaterialQualityOptions;
    float4 DisplayOptions;
    row_major float4x4 InverseUnjitteredViewProjection;
    float4 LightingOptions;
    uint4 HeroMaterialParameterBases[2];
    uint4 HeroMaterialVaryingMasks[2];
    uint4 HeroBindingRanges[7];
    float4 ProbeOptions;
};
cbuffer ScreenReflectionsConstants : register(b0, space16)
{
    row_major float4x4 SsrInverseViewProjection;
    row_major float4x4 SsrWorldToView;
    row_major float4x4 SsrProjection;
    float4 SsrRenderSize, SsrTraceSize, SsrPreviousTraceSize;
    float4 SsrPreviousDepthParameters;
    float4 SsrJitterUv;
    float4 SsrTraceParameters; // distance, thickness, normal bias metres, roughness cutoff
    float4 SsrFilterParameters; // relative depth tolerance, normal cosine, history weight, roughness tolerance
    uint4 SsrBudget; // samples, hierarchy lookups, spatial radius, maximum hierarchy level
    uint4 SsrFrameInfo; // valid history, frame index, hierarchy count, enabled
    uint4 SsrHierarchyLevels[16]; // allocation width,height,element offset,reserved
};
cbuffer ScreenReflectionsPass : register(b1, space16)
{
    uint SsrHierarchyLevel;
    uint SsrAmbientVisibilityEnabled;
    uint SsrSidecarPlaneCount;
    uint SsrPassReserved;
};
Texture2D<float> SurfaceDepth : register(t0, space16);
Texture2D<float4> GBufferBase : register(t1, space16);
Texture2D<float2> GBufferNormal : register(t2, space16);
Texture2D<uint2> GBufferMetadata : register(t3, space16);
Texture2D<float4> GBufferMaterial : register(t4, space16);
Texture2D<float2> SsrMotion : register(t5, space16);
Texture2D<float2> SsrSurfaceHistory : register(t6, space16);
Texture2D<float4> SsrOpaqueHDR : register(t7, space16);
Texture2D<float4> SsrEnvironmentSpecular : register(t8, space16);
// t9 mirrors MaterialParameters in the descriptor table; the common evaluator
// uses its native t0/space8 root descriptor.
Texture2D<uint4> SurfaceSidecar0 : register(t10, space16);
Texture2D<uint4> SurfaceSidecar1 : register(t11, space16);
Texture2D<float> SsrAmbientVisibility : register(t12, space16);
RWTexture2D<float4> SsrRawOutput : register(u0, space16);
RWTexture2D<float4> SsrPreviousRadiance : register(u1, space16);
RWTexture2D<float4> SsrCurrentRadiance : register(u2, space16);
RWTexture2D<uint4> SsrPreviousValidation : register(u3, space16);
RWTexture2D<uint4> SsrCurrentValidation : register(u4, space16);
RWTexture2D<float4> SsrComposed : register(u5, space16);
RWStructuredBuffer<float> SsrClosestDepth : register(u6, space16);

#include "material_parameters.hlsli"
#include "material_sidecar.hlsli"
#include "surface_layout.hlsli"
#include "bsdf_metallic_roughness.hlsli"
#include "hero_material.hlsli"
#ifdef HS_GENERATED_DEFERRED_MATERIALS
#include "deferred_materials.generated.hlsli"
#endif
#include "deferred_surface.hlsli"

uint2 SsrLevelActive(uint level)
{
    uint scale = 1u << level;
    return (uint2(SsrRenderSize.xy) + scale - 1u) / scale;
}
uint SsrDepthIndex(uint2 pixel, uint level)
{
    return SsrHierarchyLevels[level].z + pixel.y * SsrHierarchyLevels[level].x + pixel.x;
}
[numthreads(8, 8, 1)]
void ScreenReflectionDepthInitCS(uint3 dispatch : SV_DispatchThreadID)
{
    if (any(dispatch.xy >= uint2(SsrRenderSize.xy))) return;
    SsrClosestDepth[SsrDepthIndex(dispatch.xy, 0)] = SurfaceDepth.Load(int3(dispatch.xy, 0));
}
[numthreads(8, 8, 1)]
void ScreenReflectionDepthReduceCS(uint3 dispatch : SV_DispatchThreadID)
{
    uint level = SsrHierarchyLevel;
    if (level == 0u || level >= SsrFrameInfo.z || any(dispatch.xy >= SsrLevelActive(level))) return;
    uint2 previousActive = SsrLevelActive(level - 1u);
    float nearest = 0.0;
    [unroll] for (uint y = 0; y < 2; ++y)
    [unroll] for (uint x = 0; x < 2; ++x)
    {
        uint2 p = dispatch.xy * 2u + uint2(x, y);
        if (all(p < previousActive)) nearest = max(nearest, SsrClosestDepth[SsrDepthIndex(p, level - 1u)]);
    }
    SsrClosestDepth[SsrDepthIndex(dispatch.xy, level)] = nearest;
}
uint SsrHash(uint value)
{
    value ^= value >> 16; value *= 0x7feb352du; value ^= value >> 15;
    value *= 0x846ca68bu; return value ^ (value >> 16);
}
float SsrRandom(uint2 pixel, uint sample, uint dimension)
{
    uint seed = SsrHash(pixel.x ^ SsrHash(pixel.y) ^ SsrHash(SsrFrameInfo.y) ^
                        SsrHash(sample * 17u + dimension * 0x9e3779b9u));
    return float(seed & 0x00ffffffu) / 16777216.0;
}
float SsrViewDepth(float deviceDepth, float4 parameters)
{
    float denominator = deviceDepth * parameters.y - parameters.x;
    return abs(denominator) > 1e-8 ? (parameters.z - deviceDepth * parameters.w) / denominator : 0.0;
}
float4 SsrCurrentDepthParameters()
{
    return float4(SsrProjection[2][2], SsrProjection[2][3], SsrProjection[3][2], SsrProjection[3][3]);
}
float3 SsrRelativePosition(int2 pixel, float depth)
{
    float2 uv = (float2(pixel) + 0.5) * SsrRenderSize.zw;
    float4 world = mul(float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0), SsrInverseViewProjection);
    return world.xyz / world.w;
}
float SsrReflectionRoughness(HsSurfaceClosure closure)
{
    float roughness = closure.roughness;
    if (closure.coat_weight > 0.0) roughness = min(roughness, closure.coat_roughness);
    if (closure.sheen_weight > 0.0) roughness = min(roughness, closure.sheen_roughness);
    return saturate(roughness);
}
struct SsrGuide
{
    int2 pixel;
    float depth, view_depth, roughness;
    float3 relative, normal;
    uint2 metadata;
    bool valid;
};
SsrGuide SsrLoadGuide(int2 pixel)
{
    SsrGuide g = (SsrGuide)0;
    g.pixel = clamp(pixel, int2(0, 0), int2(SsrRenderSize.xy) - 1);
    g.depth = SurfaceDepth.Load(int3(g.pixel, 0));
    g.metadata = GBufferMetadata.Load(int3(g.pixel, 0));
    g.valid = HsSurfaceValid(g.depth, g.metadata);
    if (g.valid)
    {
        g.relative = SsrRelativePosition(g.pixel, g.depth);
        g.view_depth = mul(float4(g.relative, 0.0), SsrWorldToView).z;
        HsSurfaceClosure closure = HsReflectionLoadClosure(uint2(g.pixel));
        g.normal = normalize(closure.normal);
        g.roughness = SsrReflectionRoughness(closure);
        // The cel surface has no authored indirect-specular term to replace.
        g.valid = !closure.unlit && all(isfinite(g.relative)) && g.view_depth > 0.0 &&
            (HsSurfaceModel(g.metadata) == HsSurfaceModelEnvironment ||
             HsHeroOpaqueOrdinalValid(g.metadata.x - HsHeroMaterialIdBase));
    }
    return g;
}
SsrGuide SsrGuideForTrace(uint2 tracePixel)
{
    float2 rasterPixel = ((float2(tracePixel) + 0.5) * SsrTraceSize.zw + SsrJitterUv.xy) * SsrRenderSize.xy;
    bool halfResolution = any(SsrTraceSize.xy < SsrRenderSize.xy);
    int2 first = int2(floor(rasterPixel - 0.5));
    SsrGuide result = SsrLoadGuide(halfResolution ? first : int2(floor(rasterPixel)));
    if (halfResolution)
    {
        [unroll] for (int y = 0; y < 2; ++y)
        [unroll] for (int x = 0; x < 2; ++x)
        {
            SsrGuide candidate = SsrLoadGuide(first + int2(x, y));
            if (candidate.valid && (!result.valid || candidate.depth > result.depth)) result = candidate;
        }
    }
    return result;
}
uint SsrPackNormalRoughness(float3 normal, float roughness)
{
    uint2 oct = uint2(round(saturate(HsEncodeSurfaceNormal(normal) * 0.5 + 0.5) * 1023.0));
    return oct.x | (oct.y << 10u) | (uint(round(saturate(roughness) * 4095.0)) << 20u);
}
float3 SsrUnpackNormal(uint packed)
{
    return HsDecodeSurfaceNormal(float2(packed & 1023u, (packed >> 10u) & 1023u) / 1023.0 * 2.0 - 1.0);
}
float SsrUnpackRoughness(uint packed) { return float(packed >> 20u) / 4095.0; }
bool SsrCompatible(uint4 validation, SsrGuide g, float expectedDepth)
{
    float previousDepth = asfloat(validation.x);
    return g.valid && expectedDepth > 0.0 && previousDepth > 0.0 && all(validation.zw == g.metadata) &&
        abs(previousDepth - expectedDepth) <= max(0.01, expectedDepth * SsrFilterParameters.x) &&
        dot(SsrUnpackNormal(validation.y), g.normal) >= SsrFilterParameters.y &&
        abs(SsrUnpackRoughness(validation.y) - g.roughness) <= SsrFilterParameters.w;
}

// Projection maps a 3D line to a line in UV and device depth. Walking that
// projected segment gives perspective-correct cell intersections, unlike a
// fixed world-distance stride that can jump across small foreground surfaces.
float SsrBoundExit(float start, float delta)
{
    if (delta > 1e-8) return (1.0 - start) / delta;
    if (delta < -1e-8) return -start / delta;
    return 1.0;
}
bool SsrTraceRay(SsrGuide source, float3 directionWorld, out int2 hitPixel, out float confidence)
{
    hitPixel = 0; confidence = 0.0;
    float3 start = mul(float4(source.relative + source.normal * SsrTraceParameters.z, 0.0), SsrWorldToView).xyz;
    float3 direction = normalize(mul(float4(directionWorld, 0.0), SsrWorldToView).xyz);
    float distance = SsrTraceParameters.x;
    float nearDepth = max(SsrViewDepth(1.0, SsrCurrentDepthParameters()), 1e-4);
    if (direction.z < 0.0) distance = min(distance, (start.z - nearDepth * 1.001) / -direction.z);
    if (distance <= 0.0) return false;
    float4 startClip = mul(float4(start, 1.0), SsrProjection);
    float4 endClip = mul(float4(start + direction * distance, 1.0), SsrProjection);
    if (startClip.w <= 0.0 || endClip.w <= 0.0) return false;
    float3 a = startClip.xyz / startClip.w;
    float3 b = endClip.xyz / endClip.w;
    a.xy = a.xy * float2(0.5, -0.5) + 0.5;
    b.xy = b.xy * float2(0.5, -0.5) + 0.5;
    if (any(a.xy < 0.0) || any(a.xy >= 1.0)) return false;
    float3 delta = b - a;
    float exit = min(1.0, min(SsrBoundExit(a.x, delta.x), SsrBoundExit(a.y, delta.y)));
    exit = min(exit, SsrBoundExit(a.z, delta.z));
    float pixelSpan = max(abs(delta.x) * SsrRenderSize.x, abs(delta.y) * SsrRenderSize.y);
    float epsilon = 1e-3 / max(pixelSpan, 1.0);
    float cursor = epsilon;
    uint maximumLevel = min(SsrBudget.w, SsrFrameInfo.z - 1u);
    uint level = 0;
    [loop] for (uint iteration = 0; iteration < SsrBudget.y && cursor < exit; ++iteration)
    {
        float3 sample_position = a + delta * cursor;
        if (any(sample_position.xy < 0.0) || any(sample_position.xy >= 1.0)) break;
        float cellScale = float(1u << level);
        float2 cells = sample_position.xy * SsrRenderSize.xy / cellScale;
        uint2 cell = uint2(floor(cells));
        if (any(cell >= SsrLevelActive(level))) break;
        float2 boundary = (float2(cell) + float2(delta.x >= 0.0 ? 1.0 : 0.0, delta.y >= 0.0 ? 1.0 : 0.0)) *
                          cellScale * SsrRenderSize.zw;
        float nextX = abs(delta.x) > 1e-8 ? (boundary.x - a.x) / delta.x : exit;
        float nextY = abs(delta.y) > 1e-8 ? (boundary.y - a.y) / delta.y : exit;
        float next = min(exit, min(nextX, nextY));
        next = max(next, cursor + epsilon);
        float nearest = SsrClosestDepth[SsrDepthIndex(cell, level)];
        float endDepth = a.z + delta.z * min(next, exit);
        // Both ends in front of the closest depth guarantee an empty segment.
        bool inFront = nearest == 0.0 || min(sample_position.z, endDepth) > nearest;
        if (!inFront && level > 0)
        {
            --level;
            continue;
        }
        if (!inFront)
        {
            float sample = abs(delta.z) > 1e-8 ? clamp((nearest - a.z) / delta.z, cursor, min(next, exit)) : cursor;
            float rayDepth = SsrViewDepth(a.z + delta.z * sample, SsrCurrentDepthParameters());
            float sceneDepth = SsrViewDepth(nearest, SsrCurrentDepthParameters());
            float gap = rayDepth - sceneDepth;
            int2 pixel = int2(cell);
            if (any(pixel != source.pixel) && gap >= -1e-4 && gap <= SsrTraceParameters.y)
            {
                float2 uv = (float2(pixel) + 0.5) * SsrRenderSize.zw;
                float3 hitNormal = HsDecodeSurfaceNormal(GBufferNormal.Load(int3(pixel, 0)));
                uint2 hitMetadata = GBufferMetadata.Load(int3(pixel, 0));
                if (hitMetadata.x != 0u && dot(hitNormal, -directionWorld) > 0.0)
                {
                    float edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
                    confidence = saturate(edge / 0.05) * (1.0 - smoothstep(SsrTraceParameters.y * 0.5, SsrTraceParameters.y, max(gap, 0.0)));
                    hitPixel = pixel;
                    return confidence > 0.0;
                }
            }
        }
        cursor = next + epsilon;
        level = min(level + 1u, maximumLevel);
    }
    return false;
}
[numthreads(8, 8, 1)]
void ScreenReflectionTraceCS(uint3 dispatch : SV_DispatchThreadID)
{
    if (any(dispatch.xy >= uint2(SsrTraceSize.xy))) return;
    SsrGuide g = SsrGuideForTrace(dispatch.xy);
    float3 accumulated = 0.0;
    float accumulatedConfidence = 0.0;
    if (g.valid && g.roughness < SsrTraceParameters.w)
    {
        HsSurfaceClosure closure = HsReflectionLoadClosure(uint2(g.pixel));
        float3 view = normalize(-g.relative);
        closure = HsBsdfOrientThinSurface(closure, view);
        g.normal = normalize(closure.normal);
        float ambient = SsrAmbientVisibilityEnabled != 0u ? SsrAmbientVisibility.Load(int3(g.pixel, 0)) : 1.0;
        float visibility = saturate(closure.ao * ambient);
        uint samples = max(SsrBudget.x, 1u);
        [loop] for (uint sampleIndex = 0; sampleIndex < samples; ++sampleIndex)
        {
            HsBsdfSampleResult sample = HsBsdfSample(closure, view, SsrRandom(dispatch.xy, sampleIndex, 0),
                float2(SsrRandom(dispatch.xy, sampleIndex, 1), SsrRandom(dispatch.xy, sampleIndex, 2)));
            float cosine = dot(normalize(closure.normal), sample.wi);
            if (!sample.valid || sample.pdf <= 0.0 || cosine <= 0.0) continue;
            int2 hitPixel; float confidence;
            if (!SsrTraceRay(g, sample.wi, hitPixel, confidence)) continue;
            float3 diffuse, specular;
            HsBsdfEvaluateComponents(closure, view, sample.wi, diffuse, specular);
            float pdf = HsBsdfPdf(closure, view, sample.wi);
            if (pdf <= 0.0) continue;
            // Trace the lighting snapshot before replacement, excluding its
            // view-dependent environment specular. There is no SSR feedback.
            float3 radiance = max(SsrOpaqueHDR.Load(int3(hitPixel, 0)).rgb -
                                  SsrEnvironmentSpecular.Load(int3(hitPixel, 0)).rgb, 0.0);
            float3 contribution = radiance * specular * (cosine * visibility / pdf);
            if (!all(isfinite(contribution))) continue;
            accumulated += contribution;
            accumulatedConfidence += confidence;
        }
        // Include null VNDF, opposite-sheet and miss samples in the original
        // sampling denominator; resampling or survivor normalization is biased.
        accumulated /= float(samples);
        accumulatedConfidence /= float(samples);
        accumulatedConfidence *= 1.0 - smoothstep(SsrTraceParameters.w * 0.8, SsrTraceParameters.w, g.roughness);
    }
    SsrRawOutput[dispatch.xy] = float4(max(accumulated, 0.0), saturate(accumulatedConfidence));
}
[numthreads(8, 8, 1)]
void ScreenReflectionTemporalCS(uint3 dispatch : SV_DispatchThreadID)
{
    if (any(dispatch.xy >= uint2(SsrTraceSize.xy))) return;
    uint2 pixel = dispatch.xy;
    SsrGuide g = SsrGuideForTrace(pixel);
    float4 current = SsrRawOutput[pixel], result = current;
    float2 motion = SsrMotion.Load(int3(g.pixel, 0));
    float2 surfaceHistory = SsrSurfaceHistory.Load(int3(g.pixel, 0));
    // MV is previous-current unjittered UV. Histories are sampled on their
    // logical unjittered grid; each stored guide came from its jittered raster.
    float2 previousUv = (float2(g.pixel) + 0.5) * SsrRenderSize.zw - SsrJitterUv.xy + motion;
    if (SsrFrameInfo.x != 0u && g.valid && surfaceHistory.y > 0.0 && all(isfinite(motion)) &&
        all(previousUv >= 0.0) && all(previousUv < 1.0))
    {
        float expectedDepth = SsrViewDepth(surfaceHistory.x, SsrPreviousDepthParameters);
        float2 previousPixel = previousUv * SsrPreviousTraceSize.xy - 0.5;
        int2 first = int2(floor(previousPixel));
        float2 fraction = frac(previousPixel);
        float4 history = 0.0; float weightSum = 0.0;
        [unroll] for (int y = 0; y < 2; ++y)
        [unroll] for (int x = 0; x < 2; ++x)
        {
            int2 p = first + int2(x, y);
            if (any(p < 0) || any(p >= int2(SsrPreviousTraceSize.xy))) continue;
            if (!SsrCompatible(SsrPreviousValidation[p], g, expectedDepth)) continue;
            float weight = (x == 0 ? 1.0 - fraction.x : fraction.x) * (y == 0 ? 1.0 - fraction.y : fraction.y);
            history += SsrPreviousRadiance[p] * weight; weightSum += weight;
        }
        if (weightSum > 1e-5)
        {
            float3 lo = current.rgb, hi = current.rgb;
            [unroll] for (int y = -1; y <= 1; ++y)
            [unroll] for (int x = -1; x <= 1; ++x)
            {
                int2 p = clamp(int2(pixel) + int2(x, y), int2(0, 0), int2(SsrTraceSize.xy) - 1);
                SsrGuide other = SsrGuideForTrace(uint2(p));
                if (!other.valid || any(other.metadata != g.metadata) ||
                    abs(other.view_depth - g.view_depth) > max(0.01, g.view_depth * SsrFilterParameters.x) ||
                    dot(other.normal, g.normal) < SsrFilterParameters.y ||
                    abs(other.roughness - g.roughness) > SsrFilterParameters.w) continue;
                float3 value = SsrRawOutput[p].rgb;
                lo = min(lo, value); hi = max(hi, value);
            }
            history /= weightSum; history.rgb = clamp(history.rgb, lo, hi);
            float blend = saturate(SsrFilterParameters.z * surfaceHistory.y * weightSum);
            result = lerp(current, history, blend);
            // New offscreen/disoccluded misses immediately restore environment.
            result.a = min(result.a, current.a);
        }
    }
    SsrCurrentRadiance[pixel] = result;
    SsrCurrentValidation[pixel] = g.valid
        ? uint4(asuint(g.view_depth), SsrPackNormalRoughness(g.normal, g.roughness), g.metadata) : uint4(0, 0, 0, 0);
}
[numthreads(8, 8, 1)]
void ScreenReflectionSpatialCS(uint3 dispatch : SV_DispatchThreadID)
{
    if (any(dispatch.xy >= uint2(SsrRenderSize.xy))) return;
    SsrGuide g = SsrLoadGuide(int2(dispatch.xy));
    float4 sum = 0.0; float weightSum = 0.0;
    if (g.valid)
    {
        float2 samplePixel = ((float2(dispatch.xy) + 0.5) * SsrRenderSize.zw - SsrJitterUv.xy) * SsrTraceSize.xy - 0.5;
        int2 center = int2(floor(samplePixel + 0.5));
        int radius = int(SsrBudget.z);
        [loop] for (int y = -radius; y <= radius; ++y)
        [loop] for (int x = -radius; x <= radius; ++x)
        {
            int2 p = center + int2(x, y);
            if (any(p < 0) || any(p >= int2(SsrTraceSize.xy))) continue;
            uint4 guide = SsrCurrentValidation[p];
            if (!SsrCompatible(guide, g, g.view_depth)) continue;
            float2 delta = float2(p) - samplePixel;
            float depthWeight = exp2(-abs(asfloat(guide.x) - g.view_depth) / max(0.01, g.view_depth * SsrFilterParameters.x));
            float normalWeight = pow(saturate(dot(SsrUnpackNormal(guide.y), g.normal)), 16.0);
            float weight = exp2(-0.75 * dot(delta, delta)) * depthWeight * normalWeight;
            sum += SsrCurrentRadiance[p] * weight; weightSum += weight;
        }
    }
    const float4 reflection = weightSum > 1e-5 ? sum / weightSum : float4(0, 0, 0, 0);
    int3 pixel = int3(dispatch.xy, 0);
    float4 opaque = SsrOpaqueHDR.Load(pixel);
    float3 environment = SsrEnvironmentSpecular.Load(pixel).rgb;
    float3 replaced = opaque.rgb - environment + lerp(environment, reflection.rgb, saturate(reflection.a));
    SsrComposed[dispatch.xy] = float4(clamp(replaced, 0.0, 65000.0), opaque.a);
}
