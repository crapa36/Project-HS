#ifndef HS_RAY_TRANSPORT_HLSLI
#define HS_RAY_TRANSPORT_HLSLI
#include "project_hs.hlsl"
#define HS_RAY_OPACITY 1
#define HS_RAY_RADIANCE 1
#include "ray_hit.hlsli"
cbuffer RayTransportConstants : register(b0,space22)
{
    row_major float4x4 RtInverseViewProjection,RtWorldToView,RtProjection;
    float4 RtRenderSize,RtGiSize,RtSpecularSize,RtPreviousGiSize,RtPreviousSpecularSize;
    float4 RtPreviousDepthParameters,RtJitterUv,RtWorldOrigin;
    float4 RtGiParameters,RtSpecularParameters,RtFilterParameters;
    uint4 RtGiBudget,RtSpecularBudget,RtFrameInfo;
    uint4 RtSceneMaterialRevisions,RtLightProbeRevisions,RtCounts;
    uint4 RtHierarchyLevels[16];
    float4 RtNrdParameters;
};
RaytracingAccelerationStructure RtScene : register(t0,space20);
Texture2D<float> RtDepth : register(t5,space20);
Texture2D<float2> RtNormal : register(t6,space20);
Texture2D<uint2> RtMetadata : register(t7,space20);
Texture2D<float2> RtMotion : register(t8,space20);
Texture2D<float2> RtSurfaceHistory : register(t9,space20);
Texture2D<float4> RtBase : register(t0,space22);
Texture2D<float4> RtMaterial : register(t1,space22);
Texture2D<float4> RtHdr : register(t2,space22);
Texture2D<float4> RtFallbackDiffuse : register(t3,space22);
Texture2D<float4> RtEnvironmentSpecular : register(t4,space22);
Texture2D<float4> RtOriginalSubsurface : register(t5,space22);
Texture2D<uint4> RtSidecar0 : register(t6,space22);
Texture2D<uint4> RtSidecar1 : register(t7,space22);
StructuredBuffer<float> RtClosestDepth : register(t8,space22);
Texture2D<float4> RtGiRaw : register(t9,space22);
Texture2D<float4> RtGiRawStats : register(t10,space22);
Texture2D<float4> RtGiPrevious : register(t11,space22);
Texture2D<float4> RtGiPreviousStats : register(t12,space22);
Texture2D<uint4> RtGiPreviousValidation : register(t13,space22);
Texture2D<float4> RtGiTemporal : register(t14,space22);
Texture2D<float4> RtGiTemporalStats : register(t15,space22);
Texture2D<float4> RtGiClean : register(t16,space22);
Texture2D<float4> RtSpecularRaw : register(t17,space22);
Texture2D<float4> RtSpecularRawStats : register(t18,space22);
Texture2D<float4> RtSpecularPrevious : register(t19,space22);
Texture2D<float4> RtSpecularPreviousStats : register(t20,space22);
Texture2D<uint4> RtSpecularPreviousValidation : register(t21,space22);
Texture2D<float4> RtSpecularTemporal : register(t22,space22);
Texture2D<float4> RtSpecularTemporalStats : register(t23,space22);
Texture2D<float4> RtSpecularClean : register(t24,space22);
Texture2D<float2> RtRawMetricCount : register(t25,space22);
RWStructuredBuffer<float> RtClosestDepthOutput : register(u0,space22);
RWTexture2D<float4> RtRawOutput : register(u1,space22);
RWTexture2D<float4> RtRawStatsOutput : register(u2,space22);
RWTexture2D<float4> RtHistoryOutput : register(u3,space22);
RWTexture2D<float4> RtHistoryStatsOutput : register(u4,space22);
RWTexture2D<uint4> RtValidationOutput : register(u5,space22);
RWTexture2D<float4> RtResolvedOutput : register(u6,space22);
// GI writes its full-resolution replacement delta here. Specular writes stats.
RWTexture2D<float4> RtResolvedAuxiliary : register(u7,space22);
RWTexture2D<float4> RtAdjustedSubsurface : register(u8,space22);
RWTexture2D<float4> RtComposed : register(u9,space22);
RWTexture2D<float2> RtRawMetricCountOutput : register(u10,space22);

uint RtHash(uint x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}
float RtRandom(uint2 p,uint sample,uint dimension)
{
    uint seed=RtHash(p.x^RtHash(p.y)^RtHash(RtFrameInfo.z)^RtHash(sample*23u+dimension*7919u));
    return (seed&0x00ffffffu)*5.9604644775390625e-8;
}
int2 RtClampPixel(int2 p){return clamp(p,int2(0,0),int2(RtRenderSize.xy)-1);}
float3 RtRelative(int2 p,float depth)
{
    float2 uv=(float2(p)+0.5)*RtRenderSize.zw;
    float4 h=mul(float4(uv*float2(2,-2)+float2(-1,1),depth,1),RtInverseViewProjection);
    return h.xyz/h.w;
}
struct RtGuide
{
    int2 pixel;
    float3 relative,normal;
    float viewDepth,roughness;
    uint2 identity;
    bool valid;
};
RtGuide RtLoadGuide(int2 p)
{
    RtGuide g=(RtGuide)0;g.pixel=RtClampPixel(p);
    float depth=RtDepth.Load(int3(g.pixel,0));g.identity=RtMetadata.Load(int3(g.pixel,0));
    g.valid=depth>0.0&&g.identity.x!=0u;
    if(g.valid)
    {
        g.relative=RtRelative(g.pixel,depth);g.normal=HsDecodeSurfaceNormal(RtNormal.Load(int3(g.pixel,0)));
        g.viewDepth=mul(float4(g.relative,0),RtWorldToView).z;
        g.roughness=saturate(RtMaterial.Load(int3(g.pixel,0)).r);
        g.valid=all(isfinite(g.relative))&&g.viewDepth>0.0;
    }
    return g;
}
RtGuide RtSignalGuide(uint2 p,float4 size)
{
    float2 raster=((float2(p)+0.5)*size.zw+RtJitterUv.xy)*RtRenderSize.xy;
    int2 first=int2(floor(raster-0.5));
    bool half=any(RtRenderSize.xy>size.xy);
    RtGuide result=RtLoadGuide(half?first:int2(round(raster-0.5)));
    if(half) [unroll] for(int y=0;y<2;++y) [unroll] for(int x=0;x<2;++x)
    {
        RtGuide g=RtLoadGuide(first+int2(x,y));
        if(g.valid&&(!result.valid||g.viewDepth<result.viewDepth))result=g;
    }
    return result;
}
HsSurfaceClosure RtPrimaryClosure(RtGuide g)
{
    uint4 p0=MaterialSidecarInfo.z>0u?RtSidecar0.Load(int3(g.pixel,0)):0u;
    uint4 p1=MaterialSidecarInfo.z>1u?RtSidecar1.Load(int3(g.pixel,0)):0u;
    return HsReflectionDecodeClosure(g.identity,RtBase.Load(int3(g.pixel,0)),
        RtNormal.Load(int3(g.pixel,0)),RtMaterial.Load(int3(g.pixel,0)),p0,p1,uint2(g.pixel));
}
uint RtPackNormalRoughness(float3 normal,float roughness)
{
    float2 n=saturate(HsEncodeSurfaceNormal(normal)*0.5+0.5);
    uint2 q=(uint2)round(n*1023.0);uint r=(uint)round(saturate(roughness)*4095.0);
    return q.x|(q.y<<10)|(r<<20);
}
uint4 RtValidation(RtGuide g)
{return uint4(g.identity,asuint(g.viewDepth),RtPackNormalRoughness(g.normal,g.roughness));}
bool RtHistoryMatches(RtGuide g,uint4 previous,float expected)
{
    if(any(previous.xy!=g.identity)||expected<=0.0||!isfinite(expected))return false;
    float priorDepth=asfloat(previous.z);
    if(!isfinite(priorDepth)||abs(priorDepth-expected)>max(0.002,expected*RtFilterParameters.x))return false;
    float2 oct=float2(previous.w&1023u,(previous.w>>10)&1023u)/1023.0*2.0-1.0;
    float roughness=float(previous.w>>20)/4095.0;
    return dot(HsDecodeSurfaceNormal(oct),g.normal)>=RtFilterParameters.y&&
        abs(roughness-g.roughness)<=RtFilterParameters.w;
}
float RtPreviousViewDepth(float depth)
{
    float denominator=depth*RtPreviousDepthParameters.y-RtPreviousDepthParameters.x;
    return abs(denominator)>1e-8?(RtPreviousDepthParameters.z-depth*RtPreviousDepthParameters.w)/denominator:-1.0;
}
float RtGuideWeight(RtGuide a,RtGuide b,bool specular)
{
    if(!a.valid||!b.valid||any(a.identity!=b.identity))return 0.0;
    float normal=dot(a.normal,b.normal);
    if(normal<RtFilterParameters.y)return 0.0;
    float depth=abs(a.viewDepth-b.viewDepth)/max(a.viewDepth,0.001);
    if(depth>RtFilterParameters.x)return 0.0;
    float rough=abs(a.roughness-b.roughness);
    if(specular&&rough>RtFilterParameters.w)return 0.0;
    return exp(-depth/max(RtFilterParameters.x*0.25,1e-5))*pow(saturate(normal),16.0);
}
float4 RtRaw(bool specular,int2 p){return specular?RtSpecularRaw.Load(int3(p,0)):RtGiRaw.Load(int3(p,0));}
float4 RtRawStats(bool specular,int2 p){return specular?RtSpecularRawStats.Load(int3(p,0)):RtGiRawStats.Load(int3(p,0));}
float4 RtTemporal(bool specular,int2 p){return specular?RtSpecularTemporal.Load(int3(p,0)):RtGiTemporal.Load(int3(p,0));}
float4 RtTemporalStats(bool specular,int2 p){return specular?RtSpecularTemporalStats.Load(int3(p,0)):RtGiTemporalStats.Load(int3(p,0));}
void RtFilterTemporal(uint2 p,bool specular)
{
    float4 size=specular?RtSpecularSize:RtGiSize;
    if(any(p>=uint2(size.xy)))return;
    RtGuide g=RtSignalGuide(p,size);float4 value=RtRaw(specular,int2(p)),stats=RtRawStats(specular,int2(p));
    float4 previousSize=specular?RtPreviousSpecularSize:RtPreviousGiSize;
    bool history=specular?RtFrameInfo.y!=0u:RtFrameInfo.x!=0u;
    float2 surface=RtSurfaceHistory.Load(int3(g.pixel,0));
    if(g.valid&&value.a>0.0&&history&&surface.y>0.0)
    {
        float2 currentUv=(float2(g.pixel)+0.5)*RtRenderSize.zw-RtJitterUv.xy;
        float2 motion=RtMotion.Load(int3(g.pixel,0));
        float2 previousUv=currentUv+motion;
        if(!all(isfinite(motion))||any(previousUv<0.0)||any(previousUv>=1.0))
        {
            RtHistoryOutput[p]=value;RtHistoryStatsOutput[p]=stats;RtValidationOutput[p]=RtValidation(g);return;
        }
        float2 hp=previousUv*previousSize.xy-0.5;int2 origin=int2(floor(hp));float2 fraction=frac(hp);
        float expected=RtPreviousViewDepth(surface.x);float4 old=0.0,oldStats=0.0;float total=0.0;
        [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x)
        {
            int2 q=origin+int2(x,y);if(any(q<0)||any(q>=int2(previousSize.xy)))continue;
            uint4 validation=specular?RtSpecularPreviousValidation.Load(int3(q,0)):RtGiPreviousValidation.Load(int3(q,0));
            if(!RtHistoryMatches(g,validation,expected))continue;
            float4 v=specular?RtSpecularPrevious.Load(int3(q,0)):RtGiPrevious.Load(int3(q,0));
            float4 s=specular?RtSpecularPreviousStats.Load(int3(q,0)):RtGiPreviousStats.Load(int3(q,0));
            float weight=(x?fraction.x:1-fraction.x)*(y?fraction.y:1-fraction.y)*saturate(v.a);
            old+=v*weight;oldStats+=s*weight;total+=weight;
        }
        if(total>1e-5)
        {
            float3 lo=value.rgb,hi=value.rgb;
            [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x)
            {
                int2 q=clamp(int2(p)+int2(x,y),int2(0,0),int2(size.xy)-1);
                RtGuide neighbor=RtSignalGuide(uint2(q),size);
                if(RtGuideWeight(g,neighbor,specular)<=0.0)continue;
                float4 v=RtRaw(specular,q);if(v.a<=0.0)continue;lo=min(lo,v.rgb);hi=max(hi,v.rgb);
            }
            old/=total;oldStats/=total;
            float variance=max(stats.z-stats.y*stats.y,0.0);float sigma=sqrt(variance);
            old.rgb=clamp(old.rgb,lo-sigma,hi+sigma);
            float hitWeight=specular?exp(-abs(oldStats.x-stats.x)/max(0.1,stats.x*0.1)):1.0;
            float blend=saturate(RtFilterParameters.z)*saturate(surface.y)*saturate(total)*hitWeight;
            value.rgb=lerp(value.rgb,old.rgb,blend);stats.xyz=lerp(stats.xyz,oldStats.xyz,blend);
        }
    }
    if(!g.valid){value=0.0;stats=0.0;}
    RtHistoryOutput[p]=value;RtHistoryStatsOutput[p]=stats;RtValidationOutput[p]=g.valid?RtValidation(g):0u;
}
void RtFilterSpatial(uint2 p,bool specular)
{
    if(any(p>=uint2(RtRenderSize.xy)))return;
    RtGuide g=RtLoadGuide(int2(p));float4 size=specular?RtSpecularSize:RtGiSize;
    float2 location=((float2(p)+0.5)*RtRenderSize.zw-RtJitterUv.xy)*size.xy-0.5;
    int2 origin=int2(round(location));float4 sum=0.0,stats=0.0;float total=0.0;
    int radius=int(specular?RtSpecularBudget.z:RtGiBudget.z);
    [loop]for(int y=-radius;y<=radius;++y)[loop]for(int x=-radius;x<=radius;++x)
    {
        int2 q=origin+int2(x,y);if(any(q<0)||any(q>=int2(size.xy)))continue;
        RtGuide n=RtSignalGuide(uint2(q),size);float4 value=RtTemporal(specular,q);
        float weight=RtGuideWeight(g,n,specular)*exp(-0.5*dot(float2(q)-location,float2(q)-location))*saturate(value.a);
        sum+=value*weight;stats+=RtTemporalStats(specular,q)*weight;total+=weight;
    }
    float4 value=total>1e-5?sum/total:0.0;float4 moments=total>1e-5?stats/total:0.0;
    if(specular){RtResolvedOutput[p]=value;RtResolvedAuxiliary[p]=moments;}
    else
    {
        float4 fallback=RtFallbackDiffuse.Load(int3(p,0));
        float3 replacement=lerp(fallback.rgb,max(value.rgb,0.0),saturate(value.a));
        float3 delta=replacement-fallback.rgb;
        RtResolvedOutput[p]=float4(replacement,g.valid?1.0:0.0);
        RtResolvedAuxiliary[p]=float4(delta,g.valid?1.0:0.0);
        float4 original=RtOriginalSubsurface.Load(int3(p,0));
        RtAdjustedSubsurface[p]=float4(original.rgb+delta,original.a);
    }
}
#endif
