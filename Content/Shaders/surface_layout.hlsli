#ifndef HS_SURFACE_LAYOUT_HLSLI
#define HS_SURFACE_LAYOUT_HLSLI

#include "surface_metadata.hlsli"

// CompactDeferred v2: base RGBA8, oct normal RG16_SNORM, metadata R32_UINT,
// perceptual roughness/metalness/AO/reserved RGBA8, motion RG16_FLOAT,
// and expected previous jittered NDC depth/history confidence RG32_FLOAT.
static const uint HsSurfaceLayoutVersion = 5u;
// Zero is reserved by every material namespace and exactly representable by
// ClearRenderTargetView's float clear input for this integer render target.
static const uint HsSurfaceInvalidMaterial = 0u;
static const uint2 HsSurfaceInvalidMetadata = uint2(0u, 0u);

float2 HsSurfaceSignNotZero(float2 value)
{
    return float2(value.x >= 0.0 ? 1.0 : -1.0,
                  value.y >= 0.0 ? 1.0 : -1.0);
}

float2 HsEncodeSurfaceNormal(float3 normal)
{
    normal /= abs(normal.x) + abs(normal.y) + abs(normal.z);
    return normal.z >= 0.0 ? normal.xy :
        (1.0 - abs(normal.yx)) * HsSurfaceSignNotZero(normal.xy);
}

float3 HsDecodeSurfaceNormal(float2 packed)
{
    float3 normal = float3(packed, 1.0 - abs(packed.x) - abs(packed.y));
    if (normal.z < 0.0)
        normal.xy = (1.0 - abs(normal.yx)) * HsSurfaceSignNotZero(normal.xy);
    return normalize(normal);
}

int2 HsSurfacePixel(float2 raster_uv)
{
    return clamp(int2(raster_uv * ScreenSize.xy), int2(0, 0),
                 int2(ScreenSize.xy) - 1);
}

float2 HsSurfaceRasterUv(int2 pixel)
{
    return (float2(pixel) + 0.5) * ScreenSize.zw;
}

bool HsSurfaceValid(float depth, uint2 metadata)
{
    return depth > 0.0 && metadata.x != HsSurfaceInvalidMaterial;
}

float3 HsReconstructSurfaceRelative(float2 raster_uv, float depth)
{
    const float4 homogeneous = mul(
        float4(raster_uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0),
        InverseViewProjection);
    return homogeneous.xyz / homogeneous.w;
}

struct HsSurfaceSample
{
    int2 pixel;
    float depth;
    uint2 metadata;
};

HsSurfaceSample HsLoadSurface(float2 raster_uv)
{
    HsSurfaceSample surface;
    surface.pixel = HsSurfacePixel(raster_uv);
    surface.depth = SurfaceDepth.Load(int3(surface.pixel, 0));
    surface.metadata = HsUnpackSurfaceMetadata(GBufferMetadata.Load(int3(surface.pixel, 0)));
    return surface;
}

bool HsSurfaceValid(HsSurfaceSample surface)
{
    return HsSurfaceValid(surface.depth, surface.metadata);
}

float HsSurfaceOutlineMask(int2 pixel)
{
    pixel = clamp(pixel, int2(0, 0), int2(ScreenSize.xy) - 1);
    const float depth = SurfaceDepth.Load(int3(pixel, 0));
    const uint2 metadata = HsUnpackSurfaceMetadata(GBufferMetadata.Load(int3(pixel, 0)));
    return HsSurfaceValid(depth, metadata) && HsSurfaceOutlineGroup(metadata) != 0u ? 1.0 : 0.0;
}

float HsSampleSurfaceOutlineMask(float2 raster_uv)
{
    // Preserve the existing fractional outline footprint while loading integer
    // metadata exactly. Only the derived group mask is interpolated.
    const float2 position = raster_uv * ScreenSize.xy - 0.5;
    const int2 pixel = int2(floor(position));
    const float2 weight = frac(position);
    return lerp(lerp(HsSurfaceOutlineMask(pixel),
                     HsSurfaceOutlineMask(pixel + int2(1, 0)), weight.x),
                lerp(HsSurfaceOutlineMask(pixel + int2(0, 1)),
                     HsSurfaceOutlineMask(pixel + int2(1, 1)), weight.x), weight.y);
}

float3 HsSurfaceWorldPosition(HsSurfaceSample surface)
{
    return HsReconstructSurfaceRelative(HsSurfaceRasterUv(surface.pixel), surface.depth) +
        SurfaceViewOrigin.xyz;
}

float2 HsSurfaceClipUv(float4 clip)
{
    return clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
}

#endif
