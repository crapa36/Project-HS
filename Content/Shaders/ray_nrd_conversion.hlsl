#include "ray_transport.hlsli"
#if HS_WITH_NRD
#include "NRDConfig.hlsli"
#include "NRD.hlsli"

// Appended to the existing ray table. These allocations belong to the exact
// signal resolution; none is an alias of a raster GBuffer channel.
Texture2D<float4> RtNrdClean : register(t0,space26);
Texture2D<float4> RtNrdFactors : register(t1,space26);
RWTexture2D<float2> RtNrdMotionOutput : register(u0,space26);
RWTexture2D<float4> RtNrdNormalOutput : register(u1,space26);
RWTexture2D<float> RtNrdViewZOutput : register(u2,space26);
RWTexture2D<float4> RtNrdPackedOutput : register(u3,space26);
RWTexture2D<float4> RtNrdFactorsOutput : register(u4,space26);

float RtNrdRoughness(HsSurfaceClosure closure,bool specular)
{
    if(!specular)return 1.0;
    float roughness=closure.roughness;
    if(closure.coat_weight>0.0)roughness=min(roughness,closure.coat_roughness);
    if(closure.sheen_weight>0.0)roughness=min(roughness,closure.sheen_roughness);
    return saturate(roughness);
}
void RtNrdPack(uint2 p,bool specular)
{
    float4 size=specular?RtSpecularSize:RtGiSize;if(any(p>=uint2(size.xy)))return;
    RtGuide g=RtSignalGuide(p,size);float4 raw=RtRaw(specular,int2(p)),stats=RtRawStats(specular,int2(p));
    float3 factor=1.0;float roughness=1.0;
    if(g.valid)
    {
        HsSurfaceClosure closure=RtPrimaryClosure(g);float3 view=normalize(-g.relative);
        closure=HsBsdfOrientThinSurface(closure,view);
        g.normal=closure.normal;roughness=RtNrdRoughness(closure,specular);
        float3 diffuseFactor,specularFactor;
        NRD_MaterialFactors(closure.normal,view,closure.base_color*(1.0-saturate(closure.metallic)),
            HsBsdfF0(closure),closure.roughness,diffuseFactor,specularFactor);
        factor=specular?specularFactor:diffuseFactor;
    }
    // Advanced anisotropic/coat/sheen/thin-sheet estimators remain untouched:
    // division and later multiplication use exactly the stored closure factor.
    float3 radiance=g.valid?raw.rgb/factor:0.0;
    // REBLUR diffuse statistics already contain the per-path normalized
    // average. Specular statistics retain the native helper's raw minimum.
    float hitDistance=max(stats.x,0.0);float4 packed=0.0;
    if(g.valid&&raw.a>0.0)
        packed=RtNrdParameters.w==1.0?
            REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance,
                specular?(hitDistance>0.0?REBLUR_FrontEnd_GetNormHitDist(hitDistance,g.viewDepth,RtNrdParameters.xyz,roughness):0.0):hitDistance,true):
            RELAX_FrontEnd_PackRadianceAndHitDist(radiance,hitDistance,true);
    float2 guideUv=(float2(g.pixel)+0.5)*RtRenderSize.zw-RtJitterUv.xy;
    float2 signalUv=(float2(p)+0.5)*size.zw;
    RtNrdMotionOutput[p]=g.valid?guideUv+RtMotion.Load(int3(g.pixel,0))-signalUv:0.0;
    RtNrdNormalOutput[p]=NRD_FrontEnd_PackNormalAndRoughness(g.valid?g.normal:float3(0,0,1),roughness,0.0);
    RtNrdViewZOutput[p]=g.valid&&raw.a>0.0?g.viewDepth:2000.0;
    RtNrdPackedOutput[p]=packed;RtNrdFactorsOutput[p]=float4(factor,g.valid?raw.a:0.0);
}
float4 RtNrdValue(int2 p,bool specular)
{
    float4 factor=RtNrdFactors.Load(int3(p,0));
    RtGuide guide=RtSignalGuide(uint2(p),specular?RtSpecularSize:RtGiSize);
    // Do not read unwritten native output for excluded/no-signal geometry.
    if(factor.a<=0.0||guide.viewDepth>=1000.0)return RtRaw(specular,p);
    float4 value=RtNrdClean.Load(int3(p,0));
    float3 radiance=RtNrdParameters.w==1.0?REBLUR_BackEnd_UnpackRadianceAndNormHitDist(value).rgb:
        RELAX_BackEnd_UnpackRadiance(value).rgb;
    return float4(max(radiance*factor.rgb,0.0),factor.a);
}
void RtNrdResolve(uint2 p,bool specular)
{
    if(any(p>=uint2(RtRenderSize.xy)))return;
    RtGuide g=RtLoadGuide(int2(p));float4 size=specular?RtSpecularSize:RtGiSize;
    float2 location=((float2(p)+0.5)*RtRenderSize.zw-RtJitterUv.xy)*size.xy-0.5;
    int2 origin=int2(round(location));float4 sum=0.0,statistics=0.0;float total=0.0;
    int radius=int(specular?RtSpecularBudget.z:RtGiBudget.z);
    [loop]for(int y=-radius;y<=radius;++y)[loop]for(int x=-radius;x<=radius;++x)
    {
        int2 q=origin+int2(x,y);if(any(q<0)||any(q>=int2(size.xy)))continue;
        RtGuide n=RtSignalGuide(uint2(q),size);float4 value=RtNrdValue(q,specular);
        float weight=RtGuideWeight(g,n,specular)*exp(-0.5*dot(float2(q)-location,float2(q)-location))*saturate(value.a);
        sum+=value*weight;statistics+=RtRawStats(specular,q)*weight;total+=weight;
    }
    float4 value=total>1e-5?sum/total:0.0;
    if(specular){RtResolvedOutput[p]=value;RtResolvedAuxiliary[p]=total>1e-5?statistics/total:0.0;}
    else
    {
        float4 fallback=RtFallbackDiffuse.Load(int3(p,0));
        float3 replacement=lerp(fallback.rgb,max(value.rgb,0.0),saturate(value.a));float3 delta=replacement-fallback.rgb;
        RtResolvedOutput[p]=float4(replacement,g.valid?1.0:0.0);RtResolvedAuxiliary[p]=float4(delta,g.valid?1.0:0.0);
        float4 original=RtOriginalSubsurface.Load(int3(p,0));RtAdjustedSubsurface[p]=float4(original.rgb+delta,original.a);
    }
}
[numthreads(8,8,1)]void RtGiNrdPackCS(uint3 d:SV_DispatchThreadID){RtNrdPack(d.xy,false);}
[numthreads(8,8,1)]void RtGiNrdResolveCS(uint3 d:SV_DispatchThreadID){RtNrdResolve(d.xy,false);}
[numthreads(8,8,1)]void RtSpecularNrdPackCS(uint3 d:SV_DispatchThreadID){RtNrdPack(d.xy,true);}
[numthreads(8,8,1)]void RtSpecularNrdResolveCS(uint3 d:SV_DispatchThreadID){RtNrdResolve(d.xy,true);}
#endif
