#include "ray_transport.hlsli"

cbuffer RayReconstructionConstants : register(b0,space30)
{
    row_major float4x4 RrPreviousProjection;
    row_major float4x4 RrPreviousViewProjection;
    float4 RrSignalSize;
    uint4 RrMode; // fused DLSS, specular, reset history, actual screen alpha present
};
Texture2D<float4> RrRaw : register(t0,space30);
Texture2D<float2> RrMetricCount : register(t1,space30);
Texture2D<float4> RrNativeClean : register(t2,space30);
Texture2D<float4> RrFactors : register(t3,space30);
Texture2D<float2> RrSpecularMetricOrDistance : register(t4,space30);
Texture2D<float> RrDepthOrExposure : register(t5,space30);
Texture2D<float> RrScreenOverlayAlpha : register(t6,space30);
RWTexture2D<float4> RrRadianceOutput : register(u0,space30);
RWTexture2D<float4> RrFactorsOutput : register(u1,space30);
RWTexture2D<float> RrViewZOutput : register(u2,space30);
RWTexture2D<float4> RrPackedMotionOutput : register(u3,space30);
RWTexture2D<float4> RrNormalRoughnessOutput : register(u4,space30);
RWTexture2D<float4> RrDiffuseAlbedoOutput : register(u5,space30);
RWTexture2D<float4> RrSpecularAlbedoOutput : register(u6,space30);
RWTexture2D<float> RrDiffuseHitDistanceOutput : register(u7,space30);
RWTexture2D<float> RrSpecularHitDistanceOutput : register(u8,space30);
RWTexture2D<float> RrOpaqueDepthOutput : register(u9,space30);
RWTexture2D<float2> RrOpaqueMotionOutput : register(u10,space30);
RWTexture2D<float4> RrResolvedOutput : register(u11,space30);
RWTexture2D<float4> RrDeltaOutput : register(u12,space30);
RWTexture2D<float4> RrAdjustedSubsurfaceOutput : register(u13,space30);
RWTexture2D<unorm float> RrResponsiveOutput : register(u14,space30);

// Directional material albedo under unit incident radiance. The advanced
// closure uses its actual independent-component proposals and conditional PDF,
// including null VNDF rays in the fixed denominator. No layer is substituted
// with base color or with the NRD material approximation.
void RrMaterialFactors(HsSurfaceClosure c,float3 v,out float3 diffuse,out float3 specular)
{
    c=HsBsdfOrientThinSurface(c,v);diffuse=0.0;specular=0.0;
    if(c.unlit)return;
    bool advanced=c.coat_weight>0.0||c.anisotropy!=0.0||
        (c.sheen_weight>0.0&&any(c.sheen_color>0.0));
    if(advanced)
    {
        const uint count=32u;
        [loop]for(uint i=0u;i<count;++i)
        {
            float u=frac((float(i)+0.5)*0.6180339887498948482);
            float2 uv=float2((float(i)+0.5)/float(count),float(HsBsdfReverseBits(i))*2.3283064365386962890625e-10);
            HsBsdfSampleResult d=HsBsdfNrdSampleComponent(c,v,u,uv,false);
            if(d.valid&&d.pdf>0.0)diffuse+=d.f*(abs(dot(c.normal,d.wi))/d.pdf);
            HsBsdfSampleResult s=HsBsdfNrdSampleComponent(c,v,u,uv,true);
            if(s.valid&&s.pdf>0.0)specular+=s.f*(abs(dot(c.normal,s.wi))/s.pdf);
        }
        diffuse*=saturate(c.ao)/float(count);specular*=saturate(c.ao)/float(count);
    }
    else
    {
        float nv=saturate(dot(c.normal,v));float3 f0=HsBsdfF0(c);
        float thin=HsBsdfThinTransmissionWeight(c);
        diffuse=c.base_color*(1.0-HsBsdfSchlickFresnel(f0,nv))*(1.0-f0)*(20.0/21.0)*
            (1.0-saturate(c.metallic))*(1.0-thin);
        diffuse+=HsBsdfThinTransmissionColor(c)*thin;
        float2 ab=EnvironmentBrdfLut.SampleLevel(LinearClamp,float2(nv,saturate(c.roughness)),0);
        specular=(f0*ab.x+ab.y)*(1.0-thin);
        diffuse*=saturate(c.ao);specular*=saturate(c.ao);
    }
    // Native guides are FP16. Quantize BEFORE division so restore uses the
    // identical stored value; a nonrepresentable active component is excluded,
    // not hidden behind an arbitrary minimum albedo.
    diffuse=f16tof32(f32tof16(saturate(diffuse)));
    specular=f16tof32(f32tof16(saturate(specular)));
}
float RrPreviousZ(float depth)
{
    float denominator=depth*RrPreviousProjection[2][3]-RrPreviousProjection[2][2];
    return abs(denominator)>1e-8?(RrPreviousProjection[3][2]-depth*RrPreviousProjection[3][3])/denominator:0.0;
}
float RrRoughness(HsSurfaceClosure c)
{
    float roughness=c.roughness;
    if(c.coat_weight>0.0)roughness=min(roughness,c.coat_roughness);
    if(c.sheen_weight>0.0&&any(c.sheen_color>0.0))roughness=min(roughness,c.sheen_roughness);
    return saturate(roughness);
}
void RrPack(uint2 p,bool specular)
{
    if(any(p>=uint2(RrSignalSize.xy)))return;
    RtGuide g=RtSignalGuide(p,RrSignalSize);float4 raw=RrRaw.Load(int3(p,0));
    float2 metric=RrMetricCount.Load(int3(p,0));float3 diffuse=0.0,spec=0.0,normal=float3(0,0,1);float roughness=1.0;
    if(g.valid)
    {
        HsSurfaceClosure c=HsBsdfOrientThinSurface(RtPrimaryClosure(g),normalize(-g.relative));
        normal=normalize(c.normal);roughness=RrRoughness(c);RrMaterialFactors(c,normalize(-g.relative),diffuse,spec);
    }
    float3 factor=specular?spec:diffuse;
    bool representable=all(or(factor>0.0,raw.rgb==0.0))&&all(isfinite(factor));
    bool active=g.valid&&raw.a>0.0&&metric.y>0.0&&representable&&any(factor>0.0);
    float3 signal=float3(factor.x>0.0?raw.x/factor.x:0.0,factor.y>0.0?raw.y/factor.y:0.0,factor.z>0.0?raw.z/factor.z:0.0);
    RrRadianceOutput[p]=float4(active?signal:0.0,active?metric.x:-1.0);
    RrFactorsOutput[p]=float4(factor,active?raw.a:0.0);
    RrDiffuseAlbedoOutput[p]=float4(diffuse,1.0);RrSpecularAlbedoOutput[p]=float4(spec,1.0);
    RrNormalRoughnessOutput[p]=float4(HsEncodeSurfaceNormal(normal)*0.5+0.5,roughness,0.0); // native default material0
    RrViewZOutput[p]=g.valid?g.viewDepth:0.0;
    // Both native grids use the same normalized jitter. Their unjittered
    // centers subtract it on both sides, so it cancels from the displacement.
    float2 rasterUv=(float2(g.pixel)+0.5)*RtRenderSize.zw;
    float2 motion=rasterUv+RtMotion.Load(int3(g.pixel,0))-(float2(p)+0.5)*RrSignalSize.zw;
    float2 previous=RtSurfaceHistory.Load(int3(g.pixel,0));float previousZ=RrPreviousZ(previous.x);
    bool history=previous.y>0.0&&previousZ>0.0&&isfinite(previousZ);
    RrPackedMotionOutput[p]=float4(g.valid&&(history||RrMode.z!=0u)?motion:float2(2,2),history?previousZ-g.viewDepth:0.0,0.0);
}
float4 RrClean(int2 p)
{
    float4 factor=RrFactors.Load(int3(p,0));
    if(factor.a<=0.0)return 0.0; // SDK excluded pixels need not have written output.
    return float4(max(RrNativeClean.Load(int3(p,0)).rgb,0.0)*factor.rgb,factor.a);
}
void RrResolve(uint2 p,bool specular)
{
    if(any(p>=uint2(RtRenderSize.xy)))return;
    RtGuide g=RtLoadGuide(int2(p));float2 location=((float2(p)+0.5)*RtRenderSize.zw-RtJitterUv.xy)*RrSignalSize.xy-0.5;
    int2 origin=int2(round(location));float4 sum=0.0;float total=0.0;
    [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x)
    {
        int2 q=origin+int2(x,y);if(any(q<0)||any(q>=int2(RrSignalSize.xy)))continue;
        RtGuide n=RtSignalGuide(uint2(q),RrSignalSize);float4 value=RrClean(q);
        float weight=RtGuideWeight(g,n,specular)*exp(-0.5*dot(float2(q)-location,float2(q)-location))*saturate(value.a);
        sum+=value*weight;total+=weight;
    }
    float4 value=total>1e-5?sum/total:0.0;
    if(specular)RrResolvedOutput[p]=value;
    else
    {
        float3 fallback=RtFallbackDiffuse.Load(int3(p,0)).rgb;
        float3 replacement=lerp(fallback,max(value.rgb,0.0),saturate(value.a));float3 delta=replacement-fallback;
        RrResolvedOutput[p]=float4(replacement,g.valid?1.0:0.0);RrDeltaOutput[p]=float4(delta,g.valid?1.0:0.0);
        float4 original=RtOriginalSubsurface.Load(int3(p,0));RrAdjustedSubsurfaceOutput[p]=float4(original.rgb+delta,original.a);
    }
}
[numthreads(8,8,1)]void RRDiffusePackCS(uint3 p:SV_DispatchThreadID){RrPack(p.xy,false);}
[numthreads(8,8,1)]void RRSpecularPackCS(uint3 p:SV_DispatchThreadID){RrPack(p.xy,true);}
[numthreads(8,8,1)]void RRDiffuseResolveCS(uint3 p:SV_DispatchThreadID){RrResolve(p.xy,false);}
[numthreads(8,8,1)]void RRSpecularResolveCS(uint3 p:SV_DispatchThreadID){RrResolve(p.xy,true);}
[numthreads(8,8,1)]void DLSSRRGuidesCS(uint3 dispatch:SV_DispatchThreadID)
{
    uint2 p=dispatch.xy;if(any(p>=uint2(RtRenderSize.xy)))return;
    if(all(p==uint2(0,0)))RrViewZOutput[p]=CurrentExposure[0];
    RtGuide g=RtSignalGuide(p,RtRenderSize);float3 diffuse=0.0,specular=0.0,normal=float3(0,0,1);float roughness=1.0;
    if(g.valid)
    {
        HsSurfaceClosure c=HsBsdfOrientThinSurface(RtPrimaryClosure(g),normalize(-g.relative));
        normal=normalize(c.normal);roughness=RrRoughness(c);RrMaterialFactors(c,normalize(-g.relative),diffuse,specular);
    }
    RrDiffuseAlbedoOutput[p]=float4(diffuse,1.0);RrSpecularAlbedoOutput[p]=float4(specular,1.0);
    RrNormalRoughnessOutput[p]=float4(normal,roughness); // DLSS expects world XYZ, not FSR oct
    float2 d=RrMetricCount.Load(int3(p,0)),s=RrSpecularMetricOrDistance.Load(int3(p,0));
    RrDiffuseHitDistanceOutput[p]=g.valid&&d.y>0.0?d.x:0.0;
    RrSpecularHitDistanceOutput[p]=g.valid&&s.y>0.0?s.x:0.0;
    RrOpaqueDepthOutput[p]=RtDepth.Load(int3(g.pixel,0));
    float responsive=RrMode.z!=0u?1.0:g.valid?1.0-saturate(RtSurfaceHistory.Load(int3(g.pixel,0)).y):0.0;
    if(RrMode.w!=0u)responsive=max(responsive,RrScreenOverlayAlpha.Load(int3(p,0)));
    RrResponsiveOutput[p]=saturate(responsive);
    float2 rasterUv=(float2(g.pixel)+0.5)*RtRenderSize.zw;
    float2 signalUv=(float2(p)+0.5)*RtRenderSize.zw;
    if(g.valid)RrOpaqueMotionOutput[p]=rasterUv+RtMotion.Load(int3(g.pixel,0))-signalUv;
    else
    {
        // Dense sky motion projects a direction (w=0), excluding translation.
        float4 h=mul(float4(signalUv*float2(2,-2)+float2(-1,1),0,1),RtInverseViewProjection);
        float4 previous=mul(float4(normalize(h.xyz),0),RrPreviousViewProjection);
        RrOpaqueMotionOutput[p]=RrMode.z==0u&&previous.w>0.0?
            previous.xy/previous.w*float2(0.5,-0.5)+0.5-(signalUv-RtJitterUv.xy):0.0;
    }
}
[numthreads(8,8,1)]void RRPreExposureCS(uint3 dispatch:SV_DispatchThreadID)
{
    uint2 p=dispatch.xy;if(any(p>=uint2(RrSignalSize.xy)))return;
    RrResolvedOutput[p]=float4(RrNativeClean.Load(int3(p,0)).rgb*RrDepthOrExposure.Load(int3(0,0,0)),1.0);
}
