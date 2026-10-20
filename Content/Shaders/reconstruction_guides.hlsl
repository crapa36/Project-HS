cbuffer ReconstructionGuidesConstants : register(b0, space13)
{
    row_major float4x4 GuideInverseUnjitteredViewProjection;
    row_major float4x4 GuidePreviousUnjitteredViewProjection;
    float4 GuideCameraOrigin;
    float4 GuideRenderSize;
    float4 GuideOutputSize;
    float4 GuideJitterUv;
    float4 GuideReactiveParameters;
    uint4 GuideFrameInfo;
};
Texture2D<float> GuideDepth : register(t0, space13);
Texture2D<float2> GuideNativeMotion : register(t1, space13);
Texture2D<uint2> GuideMetadata : register(t2, space13);
Texture2D<float2> GuideSurfaceHistory : register(t3, space13);
Texture2D<float4> GuideComposite : register(t4, space13);
Texture2D<float4> GuideOpaque : register(t5, space13);
StructuredBuffer<float> GuideCurrentExposure : register(t6, space13);
Texture2D<float> GuideRevealage : register(t7, space13);
Texture2D<float4> GuideRawOutput : register(t8, space13);
Texture2D<float> GuideTransparentDepth : register(t9, space13);
Texture2D<float2> GuideTransparentMotion : register(t10, space13);
Texture2D<float2> GuideTransparentHistory : register(t11, space13);
Texture2D<float4> GuideTransparentClassification : register(t12, space13);
Texture2D<uint2> GuideTransparentMetadata : register(t13, space13);
Texture2D<uint> GuideTransparentOverflow : register(t14, space13);
Texture2D<float2> GuideTransparentTransport : register(t15, space13);
Texture2D<float> GuideScreenOverlayAlpha : register(t16, space13);
RWTexture2D<float2> GuideMotionOutput : register(u0, space13);
RWTexture2D<unorm float> GuideResponsiveOutput : register(u1, space13);
RWTexture2D<float> GuideExposureOutput : register(u2, space13);
RWTexture2D<float4> GuidePreExposedOutput : register(u3, space13);
RWTexture2D<float> GuideDepthOutput : register(u4, space13);
RWTexture2D<unorm float> GuideCompositionOutput : register(u5, space13);

bool GuideSelectTransparent(int2 pixel, float opaqueDepth, out float depth)
{
    depth = opaqueDepth;
    if (GuideFrameInfo.y == 0) return false;
    float front = GuideTransparentDepth.Load(int3(pixel, 0));
    float opacity = GuideTransparentClassification.Load(int3(pixel, 0)).z;
    if (front <= opaqueDepth || opacity <= 0.0 || GuideTransparentMetadata.Load(int3(pixel, 0)).x == 0)
        return false;
    depth = front;
    return true;
}

bool GuideSkyMotion(float2 currentUv, out float2 motion)
{
    float4 world = mul(float4(currentUv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0),
                       GuideInverseUnjitteredViewProjection);
    float3 ray = abs(world.w) > 1e-6 ? world.xyz / world.w - GuideCameraOrigin.xyz : world.xyz;
    float4 previous = mul(float4(ray, 0.0), GuidePreviousUnjitteredViewProjection);
    motion = 0.0;
    if (!all(isfinite(previous)) || previous.w <= 0.0) return false;
    float2 previousUv = previous.xy / previous.w * float2(0.5, -0.5) + 0.5;
    motion = previousUv - currentUv;
    return all(isfinite(motion));
}

[numthreads(8, 8, 1)]
void ReconstructionGuidesCS(uint3 dispatch : SV_DispatchThreadID)
{
    if (any(dispatch.xy >= uint2(GuideRenderSize.xy))) return;
    int2 pixel = int2(dispatch.xy);
    float2 rasterUv = (float2(pixel) + 0.5) * GuideRenderSize.zw;
    float2 unjitteredUv = rasterUv - GuideJitterUv.xy;
    float currentDepth;
    bool transparent = GuideSelectTransparent(pixel, GuideDepth.Load(int3(pixel, 0)), currentDepth);
    bool surface = transparent || (currentDepth > 0.0 && GuideMetadata.Load(int3(pixel, 0)).x != 0);
    float2 motion = transparent ? GuideTransparentMotion.Load(int3(pixel, 0))
        : surface ? GuideNativeMotion.Load(int3(pixel, 0)) : float2(0.0, 0.0);
    bool motionValid = all(isfinite(motion));
    if (!surface) motionValid = GuideSkyMotion(unjitteredUv, motion);
    if (!motionValid) motion = 0.0;
    // SDK low-resolution depth and MV describe the same nearest real surface.
    // Both guides remain undilated in the jittered active rectangle.
    GuideDepthOutput[pixel] = currentDepth;
    GuideMotionOutput[pixel] = motion;

    // XeSS RPM has unjittered input coordinates; other SR masks align with the
    // jittered color/depth sample. Never sample the capacity edge.
    float2 maskUv = rasterUv + (GuideFrameInfo.w != 0 ? GuideJitterUv.xy : float2(0.0, 0.0));
    int2 maskPixel = clamp(int2(maskUv * GuideRenderSize.xy), int2(0, 0), int2(GuideRenderSize.xy) - 1);
    float exposure = GuideCurrentExposure[0];
    float3 composite = GuideComposite.Load(int3(maskPixel, 0)).rgb * exposure;
    float3 opaque = GuideOpaque.Load(int3(maskPixel, 0)).rgb * exposure;
    float3 difference = abs(composite - opaque);
    float colorReactive = max(difference.r, max(difference.g, difference.b)) /
                          max(1.0, max(composite.r, max(composite.g, composite.b)));
    float coverage = 1.0 - saturate(GuideRevealage.Load(int3(maskPixel, 0)));
    float maskDepth;
    bool maskTransparent = GuideSelectTransparent(maskPixel, GuideDepth.Load(int3(maskPixel, 0)), maskDepth);
    bool maskSurface = maskTransparent || (maskDepth > 0.0 && GuideMetadata.Load(int3(maskPixel, 0)).x != 0);
    float confidence = maskTransparent ? saturate(GuideTransparentHistory.Load(int3(maskPixel, 0)).y)
        : maskSurface ? saturate(GuideSurfaceHistory.Load(int3(maskPixel, 0)).y) : 1.0;
    float2 maskMotion = maskTransparent ? GuideTransparentMotion.Load(int3(maskPixel, 0))
        : maskSurface ? GuideNativeMotion.Load(int3(maskPixel, 0)) : float2(0.0, 0.0);
    bool maskMotionValid = maskSurface ? all(isfinite(maskMotion)) : GuideSkyMotion(rasterUv, maskMotion);
    float responsive = max(coverage * GuideReactiveParameters.x, colorReactive * GuideReactiveParameters.y);
    if (GuideReactiveParameters.z > 0.0)
        responsive = max(responsive, GuideScreenOverlayAlpha.Load(int3(maskPixel, 0)));
    if (maskTransparent)
        responsive = max(responsive, GuideTransparentClassification.Load(int3(maskPixel, 0)).z * (1.0 - confidence));
    if (maskTransparent)
    {
        const float2 transport = GuideTransparentTransport.Load(int3(maskPixel, 0));
        responsive = max(responsive, max(transport.x * colorReactive, transport.y * (1.0 - confidence)));
    }
    if (GuideFrameInfo.y != 0 && GuideTransparentOverflow.Load(int3(maskPixel, 0)) != 0)
        responsive = 1.0;
    responsive = max(responsive, 1.0 - confidence);
    if (!maskMotionValid || GuideFrameInfo.x == 0) responsive = 1.0;
    GuideResponsiveOutput[pixel] = saturate(responsive);
    // FSR's composition mask identifies pixels whose color contains transparent
    // or refractive composition and should not retain ordinary opaque history.
    // Coverage is independent of the responsive/confidence/color-change mask.
    if (GuideFrameInfo.z != 0)
    {
        float composition = 1.0 - saturate(GuideRevealage.Load(int3(pixel, 0)));
        if (GuideReactiveParameters.z > 0.0)
            composition = max(composition, GuideScreenOverlayAlpha.Load(int3(pixel, 0)));
        if (transparent)
        {
            composition = max(composition, saturate(GuideTransparentClassification.Load(int3(pixel, 0)).z));
            composition = max(composition, saturate(GuideTransparentTransport.Load(int3(pixel, 0)).x));
        }
        if (GuideFrameInfo.y != 0 && GuideTransparentOverflow.Load(int3(pixel, 0)) != 0) composition = 1.0;
        GuideCompositionOutput[pixel] = saturate(composition);
    }
    if (all(dispatch.xy == uint2(0, 0))) GuideExposureOutput[uint2(0, 0)] = exposure;
}

[numthreads(8, 8, 1)]
void PreExposeReconstructionCS(uint3 dispatch : SV_DispatchThreadID)
{
    if (any(dispatch.xy >= uint2(GuideOutputSize.xy))) return;
    // Native SR preserves input radiometry; downstream post operates on
    // current pre-exposed radiance. Alpha is color alpha, never engine depth history.
    float3 color = GuideRawOutput.Load(int3(dispatch.xy, 0)).rgb * GuideCurrentExposure[0];
    GuidePreExposedOutput[dispatch.xy] = float4(color, 1.0);
}
