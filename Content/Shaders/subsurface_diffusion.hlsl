#include "material_parameters.hlsli"
#include "material_sidecar.hlsli"
#include "deferred_materials.generated.hlsli"

cbuffer SubsurfaceConstants : register(b0, space13)
{
    row_major float4x4 SssInverseViewProjection;
    float4 SssRenderSize;
    uint4 SssMaterialParameterBases[4];
    uint4 SssHeroMaterialParameterBases[2];
    uint4 SssQualityOptions; // x: maximum half-kernel samples.
};
Texture2D<float> SssDepth : register(t0, space13);
Texture2D<float2> SssNormal : register(t1, space13);
Texture2D<uint2> SssMetadata : register(t2, space13);
Texture2D<float4> SssOriginalDiffuse : register(t3, space13);
Texture2D<float4> SssHdr : register(t4, space13);
Texture2D<float4> SssHorizontal : register(t5, space13);
RWTexture2D<float4> SssHorizontalOutput : register(u0, space13);
RWTexture2D<float4> SssCorrectedOutput : register(u1, space13);

struct SssGuide
{
    float3 position, normal;
    uint material;
    bool valid;
};
float3 SssPosition(float2 pixel, float depth)
{
    const float2 uv=(pixel+0.5)*SssRenderSize.zw;
    const float4 p=mul(float4(uv*float2(2.0,-2.0)+float2(-1.0,1.0),depth,1.0),
        SssInverseViewProjection);
    return p.xyz/p.w;
}
SssGuide SssLoadGuide(int2 pixel)
{
    SssGuide guide=(SssGuide)0;
    if(any(pixel<0) || any(pixel>=int2(SssRenderSize.xy))) return guide;
    const float depth=SssDepth.Load(int3(pixel,0));
    guide.material=SssMetadata.Load(int3(pixel,0)).x;
    guide.valid=depth>0.0 && guide.material!=0u;
    if(guide.valid)
    {
        guide.position=SssPosition(float2(pixel),depth);
        guide.normal=HsSidecarDecodeTangent(SssNormal.Load(int3(pixel,0)));
        guide.valid=all(isfinite(guide.position)) && all(isfinite(guide.normal));
    }
    return guide;
}
HsSurfaceClosure SssLoadProfile(uint material)
{
    HsSurfaceClosure profile=HsMaterialMakeSurfaceClosure(0.0,float3(0,0,1),0.5,0.0,1.0,1.0);
    HsMaterialContext ctx=(HsMaterialContext)0;
    const uint environment=material-0x10000000u,hero=material-0x20000000u;
    if(environment<HsAuthoredMaterialCount)
    {
        ctx.parameter_base=SssMaterialParameterBases[environment/4u][environment%4u];
        profile=HsMaterialApplyDeferredLayers(profile,ctx,environment);
    }
    else if(hero<7u && hero!=5u)
    {
        ctx.parameter_base=SssHeroMaterialParameterBases[hero/4u][hero%4u];
        profile=HsHeroApplyDeferredLayers(profile,ctx,hero);
    }
    return profile;
}

// Separable screen-space approximation, not an exact radial BSSRDF. Each
// normalized 1D kernel uses the two diffusion lengths d and 3d of the
// Christensen/Burley profile. Taking their product loses radial symmetry;
// finite support (9d) and a quality-selected maximum half-kernel sample count
// are explicit limits. The authored radius, thickness and kernel normalization
// remain independent of that quality budget.
// RGB d, blend weight and slab thickness are authored metre-valued parameters.
float SssKernel(float distance_m,float radius_m)
{
    // The profile's common 1/radius factor cancels during normalization. Omit
    // it so an authored sub-pixel radius cannot overflow the center weight.
    return exp(-distance_m/radius_m)+exp(-distance_m/(3.0*radius_m));
}
float SssSampleChannel(int2 pixel,uint channel,bool vertical)
{
    return vertical ? SssHorizontal.Load(int3(pixel,0))[channel] :
        SssOriginalDiffuse.Load(int3(pixel,0))[channel];
}
float3 SssDiffuse(int2 pixel,SssGuide center,HsSurfaceClosure profile,bool vertical)
{
    const int2 axis=vertical ? int2(0,1) : int2(1,0);
    const float depth=SssDepth.Load(int3(pixel,0));
    const float pixel_m=length(SssPosition(float2(pixel+axis),depth)-center.position);
    const uint max_half_samples=clamp(SssQualityOptions.x,2u,8u);
    float3 result=0.0;
    [unroll] for(uint channel=0u;channel<3u;++channel)
    {
        const float radius=profile.sss_radius_m[channel];
        result[channel]=SssSampleChannel(pixel,channel,vertical);
        if(radius<=0.0 || pixel_m<=0.0) continue;
        const int extent=vertical ? int(SssRenderSize.y) : int(SssRenderSize.x);
        const int reach=int(min(ceil(9.0*radius/pixel_m),float(extent)));
        const int steps=min(int(max_half_samples),reach);
        if(steps==0) continue;
        float total=0.0,normalization=0.0;
        [loop] for(int index=-steps;index<=steps;++index)
        {
            const int offset=int(round(float(index)*float(reach)/float(steps)));
            const int2 neighbor_pixel=pixel+axis*offset;
            // The center guide is identical across the channel kernels.
            SssGuide neighbor=center;
            if(offset!=0) neighbor=SssLoadGuide(neighbor_pixel);
            // Same material means the same uniform radius/weight/thickness
            // profile and selected parameter base. Never cross another profile.
            if(!neighbor.valid || neighbor.material!=center.material) continue;
            const float3 delta=neighbor.position-center.position;
            const float plane_distance=abs(dot(delta,center.normal));
            const float normal_weight=saturate(dot(center.normal,neighbor.normal));
            if(plane_distance>profile.thickness_m || normal_weight==0.0) continue;
            const float weight=SssKernel(length(delta),radius)*normal_weight*
                exp(-plane_distance/profile.thickness_m);
            total+=SssSampleChannel(neighbor_pixel,channel,vertical)*weight;
            normalization+=weight;
        }
        if(normalization>0.0) result[channel]=total/normalization;
    }
    return result;
}
bool SssProfileValid(HsSurfaceClosure profile)
{
    return !profile.unlit && profile.sss_weight>0.0 && profile.thickness_m>0.0 &&
        any(profile.sss_radius_m>0.0);
}
[numthreads(8,8,1)]
void SubsurfaceHorizontalCS(uint3 dispatch : SV_DispatchThreadID)
{
    const int2 pixel=int2(dispatch.xy);
    if(any(pixel>=int2(SssRenderSize.xy))) return;
    const SssGuide guide=SssLoadGuide(pixel);
    const HsSurfaceClosure profile=SssLoadProfile(guide.material);
    float3 diffuse=SssOriginalDiffuse.Load(int3(pixel,0)).rgb;
    if(guide.valid && SssProfileValid(profile)) diffuse=SssDiffuse(pixel,guide,profile,false);
    SssHorizontalOutput[pixel]=float4(diffuse,1.0);
}
[numthreads(8,8,1)]
void SubsurfaceVerticalCS(uint3 dispatch : SV_DispatchThreadID)
{
    const int2 pixel=int2(dispatch.xy);
    if(any(pixel>=int2(SssRenderSize.xy))) return;
    const SssGuide guide=SssLoadGuide(pixel);
    const HsSurfaceClosure profile=SssLoadProfile(guide.material);
    float4 hdr=SssHdr.Load(int3(pixel,0));
    if(guide.valid && SssProfileValid(profile))
    {
        const float3 original=SssOriginalDiffuse.Load(int3(pixel,0)).rgb;
        const float3 blurred=SssDiffuse(pixel,guide,profile,true);
        hdr.rgb+=saturate(profile.sss_weight)*(blurred-original);
    }
    SssCorrectedOutput[pixel]=hdr;
}
