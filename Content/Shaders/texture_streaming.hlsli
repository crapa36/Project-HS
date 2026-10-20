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

// Prepared state is valid for the immutable metadata bank of this draw.
struct HsTextureGradState
{
    float clamp_mip;
    uint feedback_enabled;
};

HsTextureGradState HsTexturePrepareGrad(uint binding)
{
    const uint4 mip = HsTextureMipMetadata[binding];
    HsTextureGradState state;
    state.clamp_mip = float(mip.y - min(mip.x, mip.y));
    state.feedback_enabled = mip.w;
    return state;
}

float4 HsTextureSampleGradPrepared(Texture2DArray<float4> texture, uint binding,
    SamplerState materialSampler, float3 location, float2 dx, float2 dy,
    HsTextureGradState state)
{
#if defined(HS_SAMPLER_FEEDBACK) && HS_SAMPLER_FEEDBACK
    if (state.feedback_enabled != 0u)
        HsTextureFeedback[binding].WriteSamplerFeedbackGrad(texture, materialSampler, location, dx, dy);
#endif
    // Record ideal demand before the clamp, with the original per-sample footprint.
    return texture.SampleGrad(materialSampler, location, dx, dy, int2(0, 0), state.clamp_mip);
}

float4 HsTextureSampleGrad(Texture2DArray<float4> texture, uint binding,
    SamplerState materialSampler, float3 location, float2 dx, float2 dy)
{
    return HsTextureSampleGradPrepared(texture, binding, materialSampler, location, dx, dy,
        HsTexturePrepareGrad(binding));
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
