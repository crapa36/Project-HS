#include "project_hs.hlsl"
#define HS_RAY_OPACITY 1
#include "ray_hit.hlsli"
#if HS_WITH_NRD
#include "NRDConfig.hlsli"
#include "NRD.hlsli"
#endif
cbuffer RayAmbientOcclusionConstants : register(b0,space20)
{
    row_major float4x4 AoInverseViewProjection;
    row_major float4x4 AoWorldToView;
    row_major float4x4 AoProjection;
    float4 AoRenderSize, AoSize, AoHistorySize, AoPreviousDepthParameters;
    float4 AoJitterUv, AoParameters, AoFilterParameters, AoContactParameters, AoTowardLight;
    uint4 AoSampleCounts, AoFrameInfo;
    float4 AoWorldOrigin, AoRayParameters;
    float4 AoNrdParameters;
};
RaytracingAccelerationStructure AoScene : register(t0,space20);
Texture2D<float> AoDepth : register(t5,space20);
Texture2D<float2> AoNormal : register(t6,space20);
Texture2D<uint2> AoMetadata : register(t7,space20);
Texture2D<float2> AoMotion : register(t8,space20);
Texture2D<float2> AoSurfaceHistory : register(t9,space20);
Texture2D<float4> AoRaw : register(t10,space20);
Texture2D<float4> AoPrevious : register(t11,space20);
Texture2D<uint2> AoPreviousMetadata : register(t12,space20);
Texture2D<float4> AoTemporal : register(t13,space20);
RWTexture2D<float4> AoRawOutput : register(u1,space20);
RWTexture2D<float4> AoHistoryOutput : register(u2,space20);
RWTexture2D<uint2> AoMetadataOutput : register(u3,space20);
RWTexture2D<float> AoResolvedOutput : register(u4,space20);
#if HS_WITH_NRD
Texture2D<float> AoNrdClean : register(t0,space26);
RWTexture2D<float2> AoNrdMotionOutput : register(u0,space26);
RWTexture2D<float4> AoNrdNormalOutput : register(u1,space26);
RWTexture2D<float> AoNrdViewZOutput : register(u2,space26);
RWTexture2D<float> AoNrdHitDistanceOutput : register(u3,space26);
#endif

static const float AoPi = 3.14159265358979323846;

float2 AoSignNotZero(float2 v) { return float2(v.x >= 0 ? 1 : -1, v.y >= 0 ? 1 : -1); }
float3 AoDecodeNormal(float2 oct)
{
    float3 n = float3(oct, 1.0 - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * AoSignNotZero(n.xy);
    return normalize(n);
}
float2 AoEncodeNormal(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-8);
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * AoSignNotZero(n.xy);
}
uint AoHash(uint v)
{
    v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15;
    v *= 0x846ca68bu; return v ^ (v >> 16);
}
float AoRandom(uint2 pixel, uint salt)
{
    return (AoHash(pixel.x ^ AoHash(pixel.y + salt)) & 0x00ffffffu) / 16777216.0;
}
int2 AoClampPixel(int2 p, float2 size) { return clamp(p, int2(0, 0), int2(size) - 1); }
float3 AoViewPosition(int2 pixel, float depth)
{
    float2 uv = (float2(pixel) + 0.5) * AoRenderSize.zw;
    float4 world = mul(float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0),
                       AoInverseViewProjection);
    float3 relative = world.xyz / world.w;
    return mul(float4(relative, 0.0), AoWorldToView).xyz;
}
struct AoGuide
{
    int2 pixel;
    float3 position;
    float3 normal;
    uint2 material;
    bool valid;
};
AoGuide AoLoadGuide(int2 pixel)
{
    AoGuide g = (AoGuide)0;
    g.pixel = AoClampPixel(pixel, AoRenderSize.xy);
    float depth = AoDepth.Load(int3(g.pixel, 0));
    g.material = AoMetadata.Load(int3(g.pixel, 0));
    g.valid = depth > 0.0 && g.material.x != 0;
    if (g.valid)
    {
        g.position = AoViewPosition(g.pixel, depth);
        g.normal = AoDecodeNormal(AoNormal.Load(int3(g.pixel, 0)));
        g.valid = all(isfinite(g.position)) && g.position.z > 0.0;
    }
    return g;
}
AoGuide AoGuideForSample(uint2 aoPixel)
{
    float2 rasterPixel = ((float2(aoPixel) + 0.5) * AoSize.zw + AoJitterUv.xy) * AoRenderSize.xy;
    int2 first = int2(floor(rasterPixel - 0.5));
    bool halfResolution = AoRenderSize.x > AoSize.x || AoRenderSize.y > AoSize.y;
    AoGuide selected = AoLoadGuide(halfResolution ? first : int2(round(rasterPixel - 0.5)));
    // Half resolution selects the nearest surface of its footprint; full
    // resolution follows the jittered sample nearest its logical AO location.
    if (halfResolution)
    {
        [unroll] for (int y = 0; y < 2; ++y)
        [unroll] for (int x = 0; x < 2; ++x)
        {
            AoGuide candidate = AoLoadGuide(first + int2(x, y));
            if (candidate.valid && (!selected.valid || candidate.position.z < selected.position.z))
                selected = candidate;
        }
    }
    return selected;
}

float3 AoWorldPosition(AoGuide g)
{
    float depth=AoDepth.Load(int3(g.pixel,0));
    float2 uv=(float2(g.pixel)+0.5)*AoRenderSize.zw;
    float4 world=mul(float4(uv*float2(2,-2)+float2(-1,1),depth,1),AoInverseViewProjection);
    return world.xyz/world.w+AoWorldOrigin.xyz;
}
float AoRayVisibility(AoGuide g,uint2 pixel,out float normalizedHitDistance)
{
    float3 origin=AoWorldPosition(g);
    float3 tangent=normalize(abs(g.normal.z)<0.999?cross(float3(0,0,1),g.normal):cross(float3(0,1,0),g.normal));
    float3 bitangent=cross(g.normal,tangent);
    float bias=AoRayParameters.x;
    uint samples=AoSampleCounts.x;
    float rotation=AoRandom(pixel,AoFrameInfo.y*0x9e3779b9u);
    float radialJitter=AoRandom(pixel,AoFrameInfo.y*0x85ebca6bu+0x51e2du);
    float footprint=g.position.z*max(AoRenderSize.z/max(abs(AoProjection[0][0]),1e-6),
                                     AoRenderSize.w/max(abs(AoProjection[1][1]),1e-6))*2;
    float visible=0;normalizedHitDistance=0.0;
    [loop] for(uint sample=0;sample<samples;++sample)
    {
        float u=(float(sample)+radialJitter)/float(samples);
        float phi=2*AoPi*frac(rotation+float(sample)*0.6180339887498948);
        float3 direction=normalize(tangent*(sqrt(u)*cos(phi))+bitangent*(sqrt(u)*sin(phi))+g.normal*sqrt(1-u));
        RayDesc ray;
        ray.Origin=origin+g.normal*bias;
        ray.Direction=direction;ray.TMin=bias*0.25;ray.TMax=AoParameters.x;
        // Every triangle is nonopaque. A hit becomes a blocker only after its
        // actual authored alpha and sidedness have been evaluated.
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES|RAY_FLAG_FORCE_NON_OPAQUE> query;
        // Native hit-distance denoising needs the closest accepted blocker;
        // the binary internal estimator may terminate at any accepted blocker.
        query.TraceRayInline(AoScene,AoNrdParameters.w!=0.0?RAY_FLAG_NONE:RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,0xff,ray);
        while(query.Proceed())
        {
            if(query.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE &&
               HsRayCovered(query.CandidateInstanceID(),query.CandidatePrimitiveIndex(),
                 query.CandidateTriangleBarycentrics(),query.CandidateTriangleFrontFace(),
                 direction,footprint+query.CandidateTriangleRayT()*AoRayParameters.y))
                query.CommitNonOpaqueTriangleHit();
        }
        visible+=query.CommittedStatus()==COMMITTED_NOTHING?1.0:0.0;
#if HS_WITH_NRD
        // A miss is infinite distance, not the finite query radius. Normalize
        // actual accepted secondary hit distances before averaging COS rays.
        float distance=query.CommittedStatus()==COMMITTED_NOTHING?NRD_FP16_MAX:query.CommittedRayT();
        normalizedHitDistance+=REBLUR_FrontEnd_GetNormHitDist(distance,g.position.z,AoNrdParameters.xyz,1.0);
#endif
    }
    normalizedHitDistance/=float(samples);
    return lerp(1.0,visible/float(samples),AoParameters.y);
}
[numthreads(8,8,1)]
void RTAOTraceCS(uint3 dispatch : SV_DispatchThreadID)
{
    if(any(dispatch.xy>=uint2(AoSize.xy)))return;
    AoGuide g=AoGuideForSample(dispatch.xy);
    float normalizedHitDistance=1.0;
    float visibility=g.valid?AoRayVisibility(g,dispatch.xy,normalizedHitDistance):1.0;
    AoRawOutput[dispatch.xy]=float4(visibility,g.valid?g.position.z:0.0,
                                  g.valid?AoEncodeNormal(g.normal):float2(0,0));
#if HS_WITH_NRD
    if(AoNrdParameters.w!=0.0)
    {
        float2 guideUv=(float2(g.pixel)+0.5)*AoRenderSize.zw-AoJitterUv.xy;
        float2 signalUv=(float2(dispatch.xy)+0.5)*AoSize.zw;
        AoNrdMotionOutput[dispatch.xy]=g.valid?guideUv+AoMotion.Load(int3(g.pixel,0))-signalUv:0.0;
        AoNrdNormalOutput[dispatch.xy]=NRD_FrontEnd_PackNormalAndRoughness(g.valid?g.normal:float3(0,0,1),1.0,0.0);
        AoNrdViewZOutput[dispatch.xy]=g.valid?g.position.z:2000.0;
        AoNrdHitDistanceOutput[dispatch.xy]=normalizedHitDistance;
    }
#endif
}
float AoPreviousViewDepth(float deviceDepth)
{
    float4 p = AoPreviousDepthParameters;
    float denominator = deviceDepth * p.y - p.x;
    return abs(denominator) > 1e-8 ? (p.z - deviceDepth * p.w) / denominator : 0.0;
}
bool AoHistoryCompatible(float4 history, uint2 material, float expectedDepth, AoGuide g)
{
    return history.y > 0.0 && expectedDepth > 0.0 && all(material == g.material) &&
        abs(history.y - expectedDepth) <= max(0.01, expectedDepth * AoFilterParameters.x) &&
        dot(AoDecodeNormal(history.zw), g.normal) >= AoFilterParameters.y;
}
[numthreads(8, 8, 1)]
void RTAOTemporalCS(uint3 dispatch : SV_DispatchThreadID)
{
    if (any(dispatch.xy >= uint2(AoSize.xy))) return;
    int2 pixel = int2(dispatch.xy);
    float4 current = AoRaw.Load(int3(pixel, 0));
    AoGuide g = AoGuideForSample(dispatch.xy);
    float result = current.x;
    float2 surfaceHistory = AoSurfaceHistory.Load(int3(g.pixel, 0));
    float2 motion = AoMotion.Load(int3(g.pixel, 0));
    float2 currentUv = (float2(g.pixel) + 0.5) * AoRenderSize.zw - AoJitterUv.xy;
    float2 previousUv = currentUv + motion;
    if (AoFrameInfo.x != 0 && g.valid && surfaceHistory.y > 0.0 &&
        all(isfinite(motion)) && all(previousUv >= 0.0) && all(previousUv < 1.0))
    {
        float expectedDepth = AoPreviousViewDepth(surfaceHistory.x);
        float2 previousPixel = previousUv * AoHistorySize.xy - 0.5;
        int2 first = int2(floor(previousPixel));
        float2 fraction = frac(previousPixel);
        float sum = 0.0, weightSum = 0.0;
        [unroll] for (int y = 0; y < 2; ++y)
        [unroll] for (int x = 0; x < 2; ++x)
        {
            int2 p = first + int2(x, y);
            if (any(p < 0) || any(p >= int2(AoHistorySize.xy))) continue;
            float4 history = AoPrevious.Load(int3(p, 0));
            uint2 material = AoPreviousMetadata.Load(int3(p, 0));
            if (!AoHistoryCompatible(history, material, expectedDepth, g)) continue;
            float weight = (x == 0 ? 1.0 - fraction.x : fraction.x) *
                           (y == 0 ? 1.0 - fraction.y : fraction.y);
            sum += history.x * weight; weightSum += weight;
        }
        if (weightSum > 1e-5)
        {
            float lo = current.x, hi = current.x;
            [unroll] for (int y = -1; y <= 1; ++y)
            [unroll] for (int x = -1; x <= 1; ++x)
            {
                float4 neighbour = AoRaw.Load(int3(AoClampPixel(pixel + int2(x, y), AoSize.xy), 0));
                if (neighbour.y <= 0.0 || abs(neighbour.y - current.y) > max(0.01, current.y * AoFilterParameters.x) ||
                    dot(AoDecodeNormal(neighbour.zw), g.normal) < AoFilterParameters.y) continue;
                lo = min(lo, neighbour.x); hi = max(hi, neighbour.x);
            }
            float history = clamp(sum / weightSum, lo, hi);
            result = lerp(current.x, history, saturate(AoFilterParameters.z * surfaceHistory.y * weightSum));
        }
    }
    AoHistoryOutput[pixel] = float4(result, current.yzw);
    AoMetadataOutput[pixel] = g.valid ? g.material : uint2(0, 0);
}

[numthreads(8, 8, 1)]
void RTAOResolveCS(uint3 dispatch : SV_DispatchThreadID)
{
    if (any(dispatch.xy >= uint2(AoRenderSize.xy))) return;
    AoGuide g = AoLoadGuide(int2(dispatch.xy));
    float visibility = 1.0;
    if (g.valid)
    {
        float2 samplePixel = ((float2(dispatch.xy) + 0.5) * AoRenderSize.zw - AoJitterUv.xy) * AoSize.xy - 0.5;
        int2 center = int2(floor(samplePixel + 0.5));
        int radius = int(AoSampleCounts.w);
        float sum = 0.0, weightSum = 0.0;
        [loop] for (int y = -radius; y <= radius; ++y)
        [loop] for (int x = -radius; x <= radius; ++x)
        {
            int2 p = center + int2(x, y);
            if (any(p < 0) || any(p >= int2(AoSize.xy))) continue;
            float4 sample;
#if HS_WITH_NRD
            if(AoNrdParameters.w!=0.0)
            {
                AoGuide guide=AoGuideForSample(uint2(p));
                float visibility=1.0;
                if(guide.valid)visibility=guide.position.z>=1000.0?AoRaw.Load(int3(p,0)).x:
                    lerp(1.0,saturate(AoNrdClean.Load(int3(p,0))),AoParameters.y);
                sample=float4(visibility,guide.valid?guide.position.z:0.0,AoEncodeNormal(guide.normal));
            }
            else
#endif
                sample=AoTemporal.Load(int3(p,0));
            if (sample.y <= 0.0) continue;
            float2 delta = float2(p) - samplePixel;
            float spatial = exp2(-0.75 * dot(delta, delta));
            float depthWeight = exp2(-abs(sample.y - g.position.z) /
                                    max(0.01, g.position.z * AoFilterParameters.w));
            float normalWeight = pow(saturate(dot(AoDecodeNormal(sample.zw), g.normal)), 16.0);
            float weight = spatial * depthWeight * normalWeight;
            sum += sample.x * weight; weightSum += weight;
        }
        if (weightSum > 1e-5) visibility = saturate(sum / weightSum);
    }
    AoResolvedOutput[dispatch.xy] = visibility;
}
