#ifndef HS_LOCAL_SHADOW_HLSLI
#define HS_LOCAL_SHADOW_HLSLI
#include "shadow_filter.hlsli"

// Independent of light_list.hlsli: world consumers pass the complete light key.
// Byte offsets match renderer_local_shadow.hpp (64B metadata, 2096B record).
// A 2096B record exceeds D3D12's 2048B structured-element limit, so t1 is raw.
ByteAddressBuffer HsLocalShadowMetadata : register(t0,space27);
struct HsLocalShadowRecord
{
    uint4 identity,revision;
    float4 position_range,direction_cos_outer,parameters;
    float4 emitter_normal_two_sided;
    uint4 state;
};
struct HsLocalShadowDrawRecord
{
    uint geometry_record,source_ref,instance_index,instance_generation;
    uint page_entry,layer,draw_group,reserved;
};
ByteAddressBuffer HsLocalShadowRecords : register(t1,space27);
Texture2D<float> HsLocalShadowStaticBank : register(t2,space27);
Texture2D<float> HsLocalShadowDynamicBank : register(t3,space27);
StructuredBuffer<HsLocalShadowDrawRecord> HsLocalShadowDrawRecords : register(t4,space27);
static const uint HsLocalShadowRecordBytes=2096u;
HsLocalShadowRecord HsLocalShadowLoadRecord(uint index)
{
    const uint address=index*HsLocalShadowRecordBytes;
    HsLocalShadowRecord record;
    record.identity=HsLocalShadowRecords.Load4(address);
    record.revision=HsLocalShadowRecords.Load4(address+16u);
    record.position_range=asfloat(HsLocalShadowRecords.Load4(address+32u));
    record.direction_cos_outer=asfloat(HsLocalShadowRecords.Load4(address+48u));
    record.parameters=asfloat(HsLocalShadowRecords.Load4(address+64u));
    record.emitter_normal_two_sided=asfloat(HsLocalShadowRecords.Load4(address+2064u));
    record.state=HsLocalShadowRecords.Load4(address+2080u);
    return record;
}
float4x4 HsLocalShadowFaceMatrix(uint index,uint face)
{
    const uint address=index*HsLocalShadowRecordBytes+80u+face*64u;
    // Raw memory has no HLSL matrix major order. Construct the four actual
    // DirectX rows and use row-vector mul, matching the CPU untransposed matrix.
    return float4x4(asfloat(HsLocalShadowRecords.Load4(address)),asfloat(HsLocalShadowRecords.Load4(address+16u)),
        asfloat(HsLocalShadowRecords.Load4(address+32u)),asfloat(HsLocalShadowRecords.Load4(address+48u)));
}
float4 HsLocalShadowFaceTile(uint index,uint face)
{return asfloat(HsLocalShadowRecords.Load4(index*HsLocalShadowRecordBytes+1616u+face*16u));}
float3 HsLocalShadowSamplePosition(uint index,uint sample)
{return asfloat(HsLocalShadowRecords.Load4(index*HsLocalShadowRecordBytes+2000u+sample*16u)).xyz;}

bool HsLocalShadowIsDraw(uint pass) {return (pass&0xc0000000u)==0x40000000u;}
HsLocalShadowDrawRecord HsLocalShadowLoadDraw(uint pass)
{return HsLocalShadowDrawRecords[pass&0x3fffffffu];}
float4 HsLocalShadowClipDistance(float3 world,uint pass)
{
    if(!HsLocalShadowIsDraw(pass))return 1.0;
    const HsLocalShadowDrawRecord draw=HsLocalShadowLoadDraw(pass);
    const float4 tile=HsLocalShadowFaceTile(draw.page_entry,draw.reserved);
    const float4 projected=mul(float4(world,1.0),HsLocalShadowFaceMatrix(draw.page_entry,draw.reserved));
    const float enlarged=1.0+2.0*tile.w/tile.z;
    return float4(projected.xy+enlarged*projected.w,enlarged*projected.w-projected.xy);
}
float4 HsLocalShadowProject(float3 world,uint pass)
{
    const HsLocalShadowDrawRecord draw=HsLocalShadowLoadDraw(pass);
    const float4 tile=HsLocalShadowFaceTile(draw.page_entry,draw.reserved);
    const float4 bank=asfloat(HsLocalShadowMetadata.Load4(16u));
    const float4 h=mul(float4(world,1.0),HsLocalShadowFaceMatrix(draw.page_entry,draw.reserved));
    // Retain homogeneous Z/W (perspective depth) while embedding the face in
    // the full-bank viewport. The custom clip distance admits its guard region.
    const float2 pixel_w=(tile.xy+tile.w+0.5*tile.z)*h.w+h.xy*float2(0.5,-0.5)*tile.z;
    return float4(pixel_w/bank.xy*float2(2,-2)+float2(-1,1)*h.w,h.z,h.w);
}
void HsLocalShadowRasterClip(float2 pixel,uint pass)
{
    if(!HsLocalShadowIsDraw(pass))return;
    const HsLocalShadowDrawRecord draw=HsLocalShadowLoadDraw(pass);
    const float4 tile=HsLocalShadowFaceTile(draw.page_entry,draw.reserved);
    clip(float4(pixel-tile.xy,tile.xy+tile.z+2.0*tile.w-pixel));
}
uint HsLocalShadowCubeFace(float3 direction)
{
    const float3 a=abs(direction);
    if(a.x>=a.y&&a.x>=a.z)return direction.x>=0.0?0u:1u;
    if(a.y>=a.z)return direction.y>=0.0?2u:3u;
    return direction.z>=0.0?4u:5u;
}
bool HsLocalShadowFaceVisibility(uint index,HsLocalShadowRecord record,uint face,float3 biased_world,out float visibility)
{
    visibility=1.0;
    if(face>=record.revision.w||(record.state.x&(1u<<face))==0u)return false;
    const float4 projected=mul(float4(biased_world,1),HsLocalShadowFaceMatrix(index,face));
    if(projected.w<=0.0)return false;
    const float3 ndc=projected.xyz/projected.w;
    if(any(abs(ndc.xy)>1.0)||ndc.z<0.0||ndc.z>1.0)return false;
    const float4 tile=HsLocalShadowFaceTile(index,face);
    const int2 origin=int2(tile.xy);
    const int2 center=int2(floor(tile.xy+tile.w+(ndc.xy*float2(0.5,-0.5)+0.5)*tile.z));
    float sum=0.0;
    const uint samples=uint(record.parameters.w);
    [loop]for(uint sample_index=0;sample_index<samples;++sample_index)
    {
        const int2 p=clamp(center+HsShadowFilterOffsets[sample_index],origin,origin+int(tile.z+2.0*tile.w)-1);
        const float static_depth=HsLocalShadowStaticBank.Load(int3(p,0));
#if defined(HS_LOCAL_PROBE_CAPTURE)
        const float depth=static_depth;
#else
        const float depth=max(static_depth,HsLocalShadowDynamicBank.Load(int3(p,0)));
#endif
        sum+=ndc.z+record.parameters.y>=depth?1.0:0.0;
    }
    visibility=sum/float(samples);return true;
}
bool HsLocalShadowLookup(uint4 identity,uint2 revision,float3 world,float3 normal,out float visibility)
{
    visibility=1.0;
    const uint4 counts=HsLocalShadowMetadata.Load4(0u);
    if(counts.w==0u||identity.w<1u||identity.w>4u)return false;
    [loop]for(uint index=0u;index<counts.x;++index)
    {
        const HsLocalShadowRecord record=HsLocalShadowLoadRecord(index);
        if(any(record.identity!=identity)||any(record.revision.xy!=revision)||record.state.y==0u)continue;
        const float3 delta=world-record.position_range.xyz;
        if(dot(delta,delta)>record.position_range.w*record.position_range.w)return false;
        const float3 n=dot(normal,normal)>1e-10?normalize(normal):0.0;
        const float3 biased_world=world+n*record.parameters.z;
        if(identity.w<3u)
        {
            const uint face=identity.w==1u?HsLocalShadowCubeFace(biased_world-record.position_range.xyz):0u;
            return HsLocalShadowFaceVisibility(index,record,face,biased_world,visibility);
        }
        const uint samples=record.revision.z;
        if((samples!=1u&&samples!=2u&&samples!=4u)||record.revision.w!=samples*6u||
            record.state.x!=((1u<<record.revision.w)-1u))return false;
        float weighted_visibility=0.0,total_weight=0.0;
        [loop]for(uint sample=0u;sample<samples;++sample)
        {
            const float3 toward=HsLocalShadowSamplePosition(index,sample)-biased_world;
            const float distance_squared=dot(toward,toward);
            if(distance_squared<=0.0||!isfinite(distance_squared))continue;
            const float3 direction=toward*rsqrt(distance_squared);
            float emitter_cosine=dot(record.emitter_normal_two_sided.xyz,-direction);
            if(record.emitter_normal_two_sided.w>0.5)emitter_cosine=abs(emitter_cosine);
            // World/froxel queries do not invent a receiving surface normal;
            // their actual directional response is evaluated by their caller.
            const float receiver_cosine=dot(n,n)>0.0?max(dot(n,direction),0.0):1.0;
            const float weight=max(emitter_cosine,0.0)*receiver_cosine/distance_squared;
            if(weight<=0.0||!isfinite(weight))continue;
            const uint face=sample*6u+HsLocalShadowCubeFace(-toward);
            float sample_visibility;
            if(!HsLocalShadowFaceVisibility(index,record,face,biased_world,sample_visibility))return false;
            weighted_visibility+=weight*sample_visibility;total_weight+=weight;
        }
        // Equal-area sample weights, radiance, emitter area and range taper
        // cancel in this normalized scalar. Actual area BSDF/LTC energy remains
        // in light_list.hlsli and is multiplied by this visibility exactly once.
        if(total_weight<=0.0)return false;
        visibility=saturate(weighted_visibility/total_weight);return true;
    }
    return false;
}
// Identity-only callers still check the cached record's full generation/type.
// Prefer the revision-aware overload whenever the authoritative light is held.
bool HsLocalShadowLookup(uint4 identity,float3 world,float3 normal,out float visibility)
{
    const uint4 counts=HsLocalShadowMetadata.Load4(0u);
    visibility=1.0;if(counts.w==0u)return false;
    [loop]for(uint i=0u;i<counts.x;++i)
        if(all(HsLocalShadowRecords.Load4(i*HsLocalShadowRecordBytes)==identity))
            return HsLocalShadowLookup(identity,HsLocalShadowRecords.Load2(i*HsLocalShadowRecordBytes+16u),world,normal,visibility);
    return false;
}
#endif
