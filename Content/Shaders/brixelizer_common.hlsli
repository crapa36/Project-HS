#ifndef HS_BRIXELIZER_COMMON_HLSLI
#define HS_BRIXELIZER_COMMON_HLSLI
#include "project_hs.hlsl"
cbuffer BrixelizerConstants : register(b0,space31)
{
    row_major float4x4 BrixInverseViewProjection;
    float4 BrixRenderSize,BrixJitterUv,BrixWorldOrigin,BrixCacheParameters;
    uint4 BrixFrameInfo;
};
Texture2D<float> BrixDepth : register(t0,space31);
Texture2D<float2> BrixNormal : register(t1,space31);
Texture2D<uint2> BrixMetadata : register(t2,space31);
Texture2D<float2> BrixMotion : register(t3,space31);
Texture2D<float4> BrixBase : register(t4,space31);
Texture2D<float4> BrixMaterial : register(t5,space31);
Texture2D<uint4> BrixSidecar0 : register(t6,space31);
Texture2D<uint4> BrixSidecar1 : register(t7,space31);
Texture2D<float4> BrixCacheInjection : register(t8,space31);
Texture2D<float4> BrixFallbackDiffuse : register(t9,space31);
Texture2D<float4> BrixEnvironmentSpecular : register(t10,space31);
Texture2D<float4> BrixOriginalSubsurface : register(t11,space31);
Texture2D<float4> BrixRawDiffuse : register(t12,space31);
Texture2D<float4> BrixRawSpecular : register(t13,space31);
Texture2D<float4> BrixHdr : register(t14,space31);
RWTexture2D<float> BrixDepthOutput : register(u0,space31);
RWTexture2D<float4> BrixNormalOutput : register(u1,space31);
RWTexture2D<float> BrixRoughnessOutput : register(u2,space31);
RWTexture2D<float2> BrixMotionOutput : register(u3,space31);
RWTexture2D<float4> BrixDiffuseOutput : register(u4,space31);
RWTexture2D<float4> BrixDeltaOutput : register(u5,space31);
RWTexture2D<float4> BrixSubsurfaceOutput : register(u6,space31);
RWTexture2D<float4> BrixSpecularOutput : register(u7,space31);
RWTexture2D<float4> BrixComposedOutput : register(u8,space31);

HsSurfaceClosure BrixClosure(uint2 p)
{
    uint4 p0=BrixFrameInfo.z>0u?BrixSidecar0.Load(int3(p,0)):0u;
    uint4 p1=BrixFrameInfo.z>1u?BrixSidecar1.Load(int3(p,0)):0u;
    return HsReflectionDecodeClosure(BrixMetadata.Load(int3(p,0)),BrixBase.Load(int3(p,0)),
        BrixNormal.Load(int3(p,0)),BrixMaterial.Load(int3(p,0)),p0,p1,p);
}
bool BrixSurfaceValid(uint2 p)
{return BrixDepth.Load(int3(p,0))>0.0 && BrixMetadata.Load(int3(p,0)).x!=0u;}
float3 BrixRelativePosition(uint2 p)
{
    float2 uv=(float2(p)+0.5)*BrixRenderSize.zw;
    float4 h=mul(float4(uv*float2(2,-2)+float2(-1,1),BrixDepth.Load(int3(p,0)),1),BrixInverseViewProjection);
    return h.xyz/h.w;
}
// SDK1.1.4 sample clean GI signals are remodulated once. These factors are
// restricted to the isotropic base closure that the native guide contract
// represents. Authored layers retain the exact original probe/environment.
bool HsBrixelizerMaterialFactors(HsSurfaceClosure c,float3 view,out float3 diffuse,out float3 specular)
{
    diffuse=0.0;specular=0.0;
    if(c.unlit||c.coat_weight!=0.0||c.anisotropy!=0.0||c.sheen_weight!=0.0||
        c.transmission_weight!=0.0||c.solid_transmission_weight!=0.0)return false;
    diffuse=c.base_color*(1.0-saturate(c.metallic));
    float2 ab=EnvironmentBrdfLut.SampleLevel(LinearClamp,float2(saturate(dot(c.normal,view)),saturate(c.roughness)),0).xy;
    specular=HsBsdfF0(c)*ab.x+ab.y;
    return all(isfinite(diffuse))&&all(isfinite(specular));
}
#endif
