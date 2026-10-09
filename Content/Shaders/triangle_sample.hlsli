#ifndef HS_TRIANGLE_SAMPLE_HLSLI
#define HS_TRIANGLE_SAMPLE_HLSLI

struct HsTriangleSample
{
    bool valid;
    float3 barycentric;
    // Derivatives are with respect to top-left raster pixel coordinates.
    float3 dx;
    float3 dy;
    float clipW;
    float ndcDepth;
};

float HsTriangleClipAbsMax(float4 clip)
{
    return max(max(abs(clip.x), abs(clip.y)),
               max(abs(clip.z), abs(clip.w)));
}

// This is the float shader counterpart of SampleTriangleAtNdc in
// hs/render_data/triangle_sample.hpp.  The homogeneous rows retain original
// clip-space w, including zero or negative values from a near-clipped source
// triangle; no per-vertex divide or small-w clamp is used.
HsTriangleSample HsSampleTriangle(float4 clip0, float4 clip1, float4 clip2,
                                  float2 sampleNdc, float2 viewportExtent)
{
    HsTriangleSample result = (HsTriangleSample)0;
    if (!all(isfinite(clip0)) || !all(isfinite(clip1)) ||
        !all(isfinite(clip2)) || !all(isfinite(sampleNdc)) ||
        !all(isfinite(viewportExtent)) || viewportExtent.x <= 0.0 ||
        viewportExtent.y <= 0.0)
        return result;

    const float commonScale = max(max(HsTriangleClipAbsMax(clip0),
                                      HsTriangleClipAbsMax(clip1)),
                                  HsTriangleClipAbsMax(clip2));
    if (!isfinite(commonScale) || commonScale == 0.0)
        return result;

    // Normalize all four clip components by one positive common factor. This
    // preserves NDC barycentrics and keeps cofactors from overflowing.
    clip0 /= commonScale;
    clip1 /= commonScale;
    clip2 /= commonScale;

    const float3 one = float3(1.0, 1.0, 1.0);
    const float3 v = float3(clip0.x - sampleNdc.x * clip0.w,
                            clip1.x - sampleNdc.x * clip1.w,
                            clip2.x - sampleNdc.x * clip2.w);
    const float3 u = float3(clip0.y - sampleNdc.y * clip0.w,
                            clip1.y - sampleNdc.y * clip1.w,
                            clip2.y - sampleNdc.y * clip2.w);
    if (!all(isfinite(v)) || !all(isfinite(u)))
        return result;

    // A has rows v,u,one. Since A*b=(0,0,1), b=cross(v,u)/det(A).
    // Differentiation gives A*db/dx=(W,0,0), A*db/dy=(0,W,0), so the
    // inverse columns are cross(u,one)/det and cross(one,v)/det.
    const float3 cofactors = cross(v, u);
    const float determinant = cofactors.x + cofactors.y + cofactors.z;
    if (!isfinite(determinant) || determinant == 0.0 ||
        !all(isfinite(cofactors)))
        return result;

    const float3 barycentric = cofactors / determinant;
    if (!all(isfinite(barycentric)))
        return result;

    const float3 clipWVertices = float3(clip0.w, clip1.w, clip2.w);
    const float3 clipZVertices = float3(clip0.z, clip1.z, clip2.z);
    const float sampleWNormalized = dot(barycentric, clipWVertices);
    if (!isfinite(sampleWNormalized) || sampleWNormalized <= 0.0)
        return result;

    const float sampleW = sampleWNormalized * commonScale;
    if (!isfinite(sampleW) || sampleW <= 0.0)
        return result;

    const float3 dNdcX =
        cross(u, one) * (sampleWNormalized / determinant);
    const float3 dNdcY =
        cross(one, v) * (sampleWNormalized / determinant);
    const float3 dPixelX = dNdcX * (2.0 / viewportExtent.x);
    const float3 dPixelY = dNdcY * (-2.0 / viewportExtent.y);
    const float ndcDepth = dot(barycentric, clipZVertices) /
                           sampleWNormalized;
    if (!all(isfinite(dNdcX)) || !all(isfinite(dNdcY)) ||
        !all(isfinite(dPixelX)) || !all(isfinite(dPixelY)) ||
        !isfinite(ndcDepth))
        return result;

    result.valid = true;
    result.barycentric = barycentric;
    result.dx = dPixelX;
    result.dy = dPixelY;
    result.clipW = sampleW;
    result.ndcDepth = ndcDepth;
    return result;
}

#endif
