// Cosine-weighted horizon AO in view space; guide/history sampling is restricted
// to the active rectangle, independently of texture allocation capacity.
#include "surface_metadata.hlsli"

cbuffer AmbientOcclusionConstants : register(b0, space12)
{
    row_major float4x4 AoInverseProjection;
    row_major float4x4 AoWorldToView;
    row_major float4x4 AoProjection;
    float4 AoRenderSize;
    float4 AoSize;
    float4 AoHistorySize;
    float4 AoPreviousDepthParameters;
    float4 AoJitterUv;
    float4 AoParameters;
    float4 AoFilterParameters;
    float4 AoContactParameters;
    float4 AoTowardLight;
    uint4 AoSampleCounts;
    uint4 AoFrameInfo;
};

Texture2D<float> AoDepth : register(t0, space12);
Texture2D<float2> AoNormal : register(t1, space12);
Texture2D<uint> AoMetadata : register(t2, space12);
Texture2D<float2> AoMotion : register(t3, space12);
Texture2D<float2> AoSurfaceHistory : register(t4, space12);
Texture2D<float4> AoRaw : register(t5, space12);
Texture2D<float4> AoPrevious : register(t6, space12);
Texture2D<uint> AoPreviousMetadata : register(t7, space12);
Texture2D<float4> AoTemporal : register(t8, space12);
Texture2D<uint> AoPreparedInverseW : register(t9, space12);
RWTexture2D<float4> AoRawOutput : register(u0, space12);
RWTexture2D<float4> AoHistoryOutput : register(u1, space12);
RWTexture2D<float> AoResolvedOutput : register(u2, space12);
RWTexture2D<float> AoContactOutput : register(u3, space12);
RWTexture2D<uint> AoMetadataOutput : register(u4, space12);

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
    float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    if (AoParameters.w > 0.5)
    {
        float4 view;
        view.x = mad(ndc.x, AoInverseProjection[0][0], AoInverseProjection[3][0]);
        view.y = mad(ndc.y, AoInverseProjection[1][1], AoInverseProjection[3][1]);
        view.z = mad(depth, AoInverseProjection[2][2], AoInverseProjection[3][2]);
        view.w = mad(depth, AoInverseProjection[2][3], AoInverseProjection[3][3]);
        return view.xyz * rcp(view.w);
    }
    float4 view = mul(float4(ndc, depth, 1.0), AoInverseProjection);
    return view.xyz / view.w;
}
struct AoGuide
{
    int2 pixel;
    float3 position;
    float3 normal;
    uint material;
    bool valid;
};
AoGuide AoLoadGuide(int2 pixel)
{
    AoGuide g = (AoGuide)0;
    g.pixel = AoClampPixel(pixel, AoRenderSize.xy);
    float depth = AoDepth.Load(int3(g.pixel, 0));
    g.material = AoMetadata.Load(int3(g.pixel, 0));
    g.valid = depth > 0.0 && (g.material & HsSurfaceMetadataMaterialMask) != 0u;
    if (g.valid)
    {
        g.position = AoViewPosition(g.pixel, depth);
        g.normal = AoDecodeNormal(AoNormal.Load(int3(g.pixel, 0)));
        g.valid = all(isfinite(g.position)) && g.position.z > 0.0;
    }
    return g;
}
bool AoLoadHorizonPosition(int2 pixel, out float3 position)
{
    if (AoParameters.w > 1.5)
    {
        const uint packedInverseW = AoPreparedInverseW.Load(int3(pixel, 0));
        if (packedInverseW == 0u) { position = 0.0; return false; }
        const float inverseW = asfloat(packedInverseW);
        const float2 uv = (float2(pixel) + 0.5) * AoRenderSize.zw;
        const float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
        position.x = mad(ndc.x, AoInverseProjection[0][0], AoInverseProjection[3][0]) * inverseW;
        position.y = mad(ndc.y, AoInverseProjection[1][1], AoInverseProjection[3][1]) * inverseW;
        position.z = AoInverseProjection[3][2] * inverseW;
        return all(isfinite(position)) && position.z > 0.0;
    }
    float depth = AoDepth.Load(int3(pixel, 0));
    uint material = AoMetadata.Load(int3(pixel, 0));
    position = 0.0;
    if (depth <= 0.0 || (material & HsSurfaceMetadataMaterialMask) == 0u) return false;
    position = AoViewPosition(pixel, depth);
    return all(isfinite(position)) && position.z > 0.0;
}

[numthreads(8, 8, 1)]
void AoPrepareInverseWCS(uint3 dispatch : SV_DispatchThreadID)
{
    if (any(dispatch.xy >= uint2(AoSize.xy))) return;
    const int2 pixel = int2(dispatch.xy);
    const float depth = AoDepth.Load(int3(pixel, 0));
    const uint material = AoMetadata.Load(int3(pixel, 0));
    if (depth <= 0.0 || (material & HsSurfaceMetadataMaterialMask) == 0u)
    {
        AoMetadataOutput[pixel] = 0u;
        return;
    }
    const float2 uv = (float2(pixel) + 0.5) * AoRenderSize.zw;
    const float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    const float viewX = mad(ndc.x, AoInverseProjection[0][0], AoInverseProjection[3][0]);
    const float viewY = mad(ndc.y, AoInverseProjection[1][1], AoInverseProjection[3][1]);
    const float viewNumeratorZ = mad(depth, AoInverseProjection[2][2], AoInverseProjection[3][2]);
    const float viewW = mad(depth, AoInverseProjection[2][3], AoInverseProjection[3][3]);
    const float inverseW = rcp(viewW);
    const float3 position = float3(viewX, viewY, viewNumeratorZ) * inverseW;
    if (!all(isfinite(position)) || position.z <= 0.0 || !isfinite(inverseW) || inverseW <= 0.0)
    {
        AoMetadataOutput[pixel] = 0u;
        return;
    }
    AoMetadataOutput[pixel] = asuint(inverseW);
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
            if (x == 0 && y == 0) continue;
            AoGuide candidate = AoLoadGuide(first + int2(x, y));
            if (candidate.valid && (!selected.valid || candidate.position.z < selected.position.z))
                selected = candidate;
        }
    }
    return selected;
}
float AoArcIntegral(float h, float n)
{
    return (cos(n) + 2.0 * h * sin(n) - cos(2.0 * h - n)) * 0.25;
}
float AoHorizonVisibility(AoGuide g, uint2 pixel)
{
    float radius = max(AoParameters.x, 1e-4);
    float radiusSquared = radius * radius;
    float falloffStart = radius * saturate(AoParameters.z);
    float falloffRange = max(radius - falloffStart, 1e-4);
    float3 view = normalize(-g.position);
    float3 normal = normalize(mul(float4(g.normal, 0.0), AoWorldToView).xyz);
    float2 radiusPixels = abs(float2(AoProjection[0][0], AoProjection[1][1])) *
                          AoRenderSize.xy * (0.5 * radius / g.position.z);
    uint slices = max(AoSampleCounts.x, 1u), steps = max(AoSampleCounts.y, 1u);
    float rotation = AoRandom(pixel, AoFrameInfo.y * 0x9e3779b9u) * AoPi;
    float visibility = 0.0;
    [loop] for (uint slice = 0; slice < slices; ++slice)
    {
        float phi = rotation + (float(slice) + 0.5) * (AoPi / float(slices));
        float2 direction = float2(cos(phi), sin(phi));
        float3 tangent = float3(direction.x / max(abs(AoProjection[0][0]), 1e-6),
                                -direction.y / max(abs(AoProjection[1][1]), 1e-6), 0.0);
        tangent = normalize(tangent - view * dot(tangent, view));
        float3 axis = normalize(cross(tangent, view));
        float3 projectedNormal = normal - axis * dot(normal, axis);
        float projectedLength = length(projectedNormal);
        if (projectedLength < 1e-5) continue;
        projectedNormal /= projectedLength;
        float n = atan2(dot(projectedNormal, tangent), dot(projectedNormal, view));
        float2 horizon = -1.0;
        float phase = AoRandom(pixel, slice + AoFrameInfo.y * 13u);
        [loop] for (uint step = 0; step < steps; ++step)
        {
            float s = (float(step) + 0.5 + 0.5 * phase) / float(steps);
            float2 offset = direction * radiusPixels * s * s;
            [unroll] for (uint side = 0; side < 2; ++side)
            {
                int2 samplePixel = int2(round(float2(g.pixel) + offset * (side == 0 ? -1.0 : 1.0)));
                if (any(samplePixel < 0) || any(samplePixel >= int2(AoRenderSize.xy))) continue;
                if (all(samplePixel == g.pixel)) continue;
                float3 otherPosition;
                if (!AoLoadHorizonPosition(samplePixel, otherPosition)) continue;
                float3 delta = otherPosition - g.position;
                float distanceSquared = dot(delta, delta);
                if (distanceSquared < 1e-8 || distanceSquared >= radiusSquared) continue;
                float distance = sqrt(distanceSquared);
                float weight = 1.0 - saturate((distance - falloffStart) / falloffRange);
                float cosHorizon = lerp(-1.0, dot(delta / distance, view), weight);
                horizon[side] = max(horizon[side], cosHorizon);
            }
        }
        float h0 = clamp(-acos(clamp(horizon.x, -1.0, 1.0)), n - AoPi * 0.5, n + AoPi * 0.5);
        float h1 = clamp( acos(clamp(horizon.y, -1.0, 1.0)), n - AoPi * 0.5, n + AoPi * 0.5);
        visibility += projectedLength * (AoArcIntegral(h0, n) + AoArcIntegral(h1, n));
    }
    return lerp(1.0, saturate(visibility / float(slices)), saturate(AoParameters.y));
}

[numthreads(8, 8, 1)]
void GTAOHorizonCS(uint3 dispatch : SV_DispatchThreadID)
{
    if (any(dispatch.xy >= uint2(AoSize.xy))) return;
    AoGuide g = AoGuideForSample(dispatch.xy);
    float visibility = g.valid ? AoHorizonVisibility(g, dispatch.xy) : 1.0;
    AoRawOutput[dispatch.xy] = float4(visibility, g.valid ? g.position.z : 0.0,
                                    g.valid ? AoEncodeNormal(g.normal) : float2(0.0, 0.0));
}

float AoPreviousViewDepth(float deviceDepth)
{
    float4 p = AoPreviousDepthParameters;
    float denominator = deviceDepth * p.y - p.x;
    return abs(denominator) > 1e-8 ? (p.z - deviceDepth * p.w) / denominator : 0.0;
}
bool AoHistoryCompatible(float4 history, uint material, float expectedDepth, AoGuide g)
{
    return history.y > 0.0 && expectedDepth > 0.0 && material == g.material &&
        abs(history.y - expectedDepth) <= max(0.01, expectedDepth * AoFilterParameters.x) &&
        dot(AoDecodeNormal(history.zw), g.normal) >= AoFilterParameters.y;
}
[numthreads(8, 8, 1)]
void GTAOTemporalCS(uint3 dispatch : SV_DispatchThreadID)
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
            uint material = AoPreviousMetadata.Load(int3(p, 0));
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
    AoMetadataOutput[pixel] = g.valid ? g.material : 0u;
}

[numthreads(8, 8, 1)]
void GTAOBilateralCS(uint3 dispatch : SV_DispatchThreadID)
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
            float4 sample = AoTemporal.Load(int3(p, 0));
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

[numthreads(8, 8, 1)]
void ContactShadowCS(uint3 dispatch : SV_DispatchThreadID)
{
    if (any(dispatch.xy >= uint2(AoRenderSize.xy))) return;
    AoGuide g = AoLoadGuide(int2(dispatch.xy));
    float visibility = 1.0;
    if (g.valid && dot(AoTowardLight.xyz, AoTowardLight.xyz) > 1e-8)
    {
        float3 light = normalize(mul(float4(AoTowardLight.xyz, 0.0), AoWorldToView).xyz);
        float3 normal = normalize(mul(float4(g.normal, 0.0), AoWorldToView).xyz);
        float bias = max(AoContactParameters.z, 0.0);
        float3 start = g.position + normal * (dot(normal, light) < 0.0 ? -bias : bias);
        uint steps = max(AoSampleCounts.z, 1u);
        float phase = AoRandom(dispatch.xy, 0x51e2du);
        [loop] for (uint step = 0; step < steps; ++step)
        {
            float distance = (float(step) + 0.5 + 0.5 * phase) * AoContactParameters.x / float(steps);
            float3 sample_position = start + light * distance;
            float4 clip = mul(float4(sample_position, 1.0), AoProjection);
            if (clip.w <= 0.0) break;
            float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
            if (any(uv < 0.0) || any(uv >= 1.0)) break;
            AoGuide other = AoLoadGuide(int2(uv * AoRenderSize.xy));
            if (!other.valid) continue;
            float gap = sample_position.z - other.position.z;
            float thickness = max(AoContactParameters.y, 1e-4);
            if (gap > max(0.001, bias * 0.5) && gap < thickness)
            {
                float occlusion = (1.0 - smoothstep(thickness * 0.5, thickness, gap)) *
                                  (1.0 - smoothstep(AoContactParameters.x * 0.8, AoContactParameters.x, distance));
                visibility = min(visibility, 1.0 - occlusion);
            }
        }
    }
    AoContactOutput[dispatch.xy] = visibility;
}
