#ifndef HS_TEXTURE_STREAMING_HLSLI
#define HS_TEXTURE_STREAMING_HLSLI

#include "render_binding_contract.hlsli"

// One immutable uint4 per actual material binding, in the waited frame bank.
// x=physical mip base, y=published logical resident MinMip, z=logical mip count,
// w=the physical target has its paired native feedback map in this bank.
StructuredBuffer<uint4> HsTextureMipMetadata : register(t0, space32);
#if defined(HS_SAMPLER_FEEDBACK) && HS_SAMPLER_FEEDBACK
FeedbackTexture2DArray<SAMPLER_FEEDBACK_MIN_MIP> HsTextureFeedback[HS_RENDER_AUTHORED_MATERIAL_COUNT * HS_RENDER_MATERIAL_TEXTURE_SLOT_COUNT] : register(u0, space32);
#endif

float4 HsTextureSampleGrad(Texture2DArray<float4> texture, uint binding,
    SamplerState materialSampler, float3 location, float2 dx, float2 dy)
{
    uint4 mip = HsTextureMipMetadata[binding];
#if defined(HS_SAMPLER_FEEDBACK) && HS_SAMPLER_FEEDBACK
    if (mip.w != 0u)
        HsTextureFeedback[binding].WriteSamplerFeedbackGrad(texture, materialSampler, location, dx, dy);
#endif
    // Record ideal demand before the clamp. Reduced software resources use
    // their actual dimensions for gradients; only explicit levels need rebasing.
    float clamp_mip = float(mip.y - min(mip.x, mip.y));
    return texture.SampleGrad(materialSampler, location, dx, dy, int2(0, 0), clamp_mip);
}

float4 HsTextureSampleLevel(Texture2DArray<float4> texture, uint binding,
    SamplerState materialSampler, float3 location, float logical_level)
{
    uint4 mip = HsTextureMipMetadata[binding];
#if defined(HS_SAMPLER_FEEDBACK) && HS_SAMPLER_FEEDBACK
    if (mip.w != 0u)
        HsTextureFeedback[binding].WriteSamplerFeedbackLevel(texture, materialSampler, location, logical_level);
#endif
    return texture.SampleLevel(materialSampler, location,
        max(0.0, max(logical_level, float(mip.y)) - float(mip.x)));
}
#endif
