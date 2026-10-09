#ifndef HS_RAY_SHADOW_DATA_HLSLI
#define HS_RAY_SHADOW_DATA_HLSLI
// Canonical opaque exports are separate from each trace's private space28.
// World/transparent consumers never use this screen-associated alternative.
ByteAddressBuffer HsRayShadowMetadata : register(t0,space29);
struct HsRayShadowRecord
{
    uint4 identity,revision;
    float4 position_range,direction_cos_outer,emitter;
    uint4 state;
};
StructuredBuffer<HsRayShadowRecord> HsRayShadowRecords : register(t1,space29);
Texture2DArray<float> HsRayShadowVisibility : register(t2,space29);
Texture2DArray<float> HsRayShadowConfidence : register(t3,space29);
Texture2D<float> HsRayShadowDepth : register(t4,space29);
Texture2D<float2> HsRayShadowNormal : register(t5,space29);
float3 HsRayShadowDecodeNormal(float2 oct)
{
    float3 n=float3(oct,1.0-abs(oct.x)-abs(oct.y));
    if(n.z<0.0)n.xy=(1.0-abs(n.yx))*float2(n.x>=0.0?1.0:-1.0,n.y>=0.0?1.0:-1.0);
    return normalize(n);
}
bool HsRayShadowLookup(uint4 identity,uint2 revision,float3 world,float3 normal,out float visibility,out float confidence)
{
    visibility=1.0;confidence=0.0;
    const uint4 counts=HsRayShadowMetadata.Load4(96u);
    if(counts.y==0u||identity.w<1u||identity.w>4u||dot(normal,normal)<1e-10)return false;
    const float4x4 world_to_clip=float4x4(asfloat(HsRayShadowMetadata.Load4(0)),asfloat(HsRayShadowMetadata.Load4(16)),
        asfloat(HsRayShadowMetadata.Load4(32)),asfloat(HsRayShadowMetadata.Load4(48)));
    const float4 clip=mul(float4(world,1),world_to_clip);if(clip.w<=0.0)return false;
    const float4 render=asfloat(HsRayShadowMetadata.Load4(64u));
    const float2 uv=clip.xy/clip.w*float2(0.5,-0.5)+0.5+asfloat(HsRayShadowMetadata.Load4(80u)).xy;
    if(any(uv<0.0)||any(uv>=1.0))return false;
    const int2 pixel=int2(floor(uv*render.xy));const float depth=HsRayShadowDepth.Load(int3(pixel,0));
    const float denominator=depth-asfloat(counts.z);
    const float view_z=abs(denominator)>1e-10?asfloat(counts.w)/denominator:0.0;
    // This visibility belongs to the actual opaque guide surface. A nearby
    // transparent layer or a secondary hit must not inherit its screen value.
    if(depth<=0.0||view_z<=0.0||!isfinite(view_z)||abs(view_z-clip.w)>max(0.002,clip.w*0.002)||
        dot(normalize(normal),HsRayShadowDecodeNormal(HsRayShadowNormal.Load(int3(pixel,0))))<0.9)return false;
    [loop]for(uint i=0u;i<counts.x;++i)
    {
        const HsRayShadowRecord record=HsRayShadowRecords[i];
        if(any(record.identity!=identity)||any(record.revision.xy!=revision)||record.state.y==0u)continue;
        const int3 p=int3(pixel,record.state.x);
        confidence=saturate(HsRayShadowConfidence.Load(int4(p,0)));
        visibility=saturate(HsRayShadowVisibility.Load(int4(p,0)));return confidence>0.0;
    }
    return false;
}
#endif
