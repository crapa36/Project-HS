#ifndef HS_LOCAL_PROBE_SAMPLING_HLSLI
#define HS_LOCAL_PROBE_SAMPLING_HLSLI

#include "probe_ibl.hlsli"

struct HsLocalProbeRecord
{
    float4 position_weight;
    float4 influence_min_fade;
    float4 influence_max_bias;
    float4 projection_min_visibility_fade;
    float4 projection_max_capture_distance;
    float4 parameters; // GGX maximum mip, cube index, enabled, active probe count.
    float4 reserved0,reserved1;
};
TextureCubeArray<float4> HsLocalProbeRawCube : register(t0, space23);
TextureCubeArray<float4> HsLocalProbeGgxCube : register(t1, space23);
StructuredBuffer<float4> HsLocalProbeCosineSh : register(t2, space23);
// Exact first-hit texel lookup avoids mixing a near wall with a far miss.
Texture2DArray<float> HsLocalProbeDistanceFaces : register(t3, space23);
StructuredBuffer<HsLocalProbeRecord> HsLocalProbeRecords : register(t4, space23);

uint HsLocalProbeCount()
{
    uint count,stride; HsLocalProbeRecords.GetDimensions(count,stride);
    if(count==0 || stride!=128) return 0;
    return min(count,(uint)HsLocalProbeRecords[0].parameters.w);
}
float HsLocalProbeFirstHit(uint cube,float3 d)
{
    const float3 a=abs(d); uint face; float2 uv;
    if(a.x>=a.y && a.x>=a.z)
    { face=d.x>=0?0:1; uv=float2(d.x>=0?-d.z:d.z,-d.y)/a.x; }
    else if(a.y>=a.z)
    { face=d.y>=0?2:3; uv=float2(d.x,d.y>=0?d.z:-d.z)/a.y; }
    else { face=d.z>=0?4:5; uv=float2(d.z>=0?d.x:-d.x,-d.y)/a.z; }
    uint width,height,layers; HsLocalProbeDistanceFaces.GetDimensions(width,height,layers);
    const uint2 pixel=min((uint2)(saturate(uv*0.5+0.5)*float2(width,height)),uint2(width-1,height-1));
    return HsLocalProbeDistanceFaces.Load(int4(pixel,cube*6+face,0));
}
float HsLocalProbeWeight(HsLocalProbeRecord p,float3 worldPosition)
{
    if(p.parameters.z==0) return 0;
    const float3 inside=min(worldPosition-p.influence_min_fade.xyz,p.influence_max_bias.xyz-worldPosition);
    const float edge=min(inside.x,min(inside.y,inside.z));
    if(edge<=0) return 0;
    float weight=p.position_weight.w*smoothstep(0,p.influence_min_fade.w,edge);
    const float3 delta=worldPosition-p.position_weight.xyz; const float distance=length(delta);
    if(distance>0)
    {
        const float hit=HsLocalProbeFirstHit((uint)p.parameters.y,delta);
        // Captured sky/miss is fully visible; an actual occluder attenuates
        // receivers beyond the first hit using authored world-unit bias/fade.
        if(hit<p.projection_max_capture_distance.w)
            weight*=1-smoothstep(hit+p.influence_max_bias.w,
                hit+p.influence_max_bias.w+p.projection_min_visibility_fade.w,distance);
    }
    return weight;
}
float3 HsLocalProbeProjectedDirection(HsLocalProbeRecord p,float3 worldPosition,float3 worldDirection)
{
    const float3 d=normalize(worldDirection);
    // Influence is contained in the projection box. Compute the ray's forward
    // exit from that box from the receiver, then look up from the probe center.
    float exit=1e30;
    [unroll] for(uint axis=0;axis<3;++axis)
        if(abs(d[axis])>1e-8)
            exit=min(exit,((d[axis]>0?p.projection_max_capture_distance[axis]:p.projection_min_visibility_fade[axis])-worldPosition[axis])/d[axis]);
    return normalize(worldPosition+d*max(exit,0)-p.position_weight.xyz);
}
float3 HsLocalProbeShIrradiance(uint probe,float3 worldNormal)
{
    const float3 n=normalize(worldNormal);
    const float basis[9]={0.28209479177387814,
        0.4886025119029199*n.y,0.4886025119029199*n.z,0.4886025119029199*n.x,
        1.0925484305920792*n.x*n.y,1.0925484305920792*n.y*n.z,
        0.31539156525252005*(3*n.z*n.z-1),1.0925484305920792*n.x*n.z,
        0.5462742152960396*(n.x*n.x-n.y*n.y)};
    float3 irradiance=0;
    [unroll] for(uint coefficient=0;coefficient<9;++coefficient)
        irradiance+=HsLocalProbeCosineSh[probe*9+coefficient].rgb*basis[coefficient];
    return max(irradiance,0);
}
float3 HsLocalProbeIrradiance(float3 worldPosition,float3 worldNormal)
{
    const uint count=HsLocalProbeCount(); float total=0; float3 local=0;
    for(uint i=0;i<count;++i)
    { const HsLocalProbeRecord p=HsLocalProbeRecords[i]; const float w=HsLocalProbeWeight(p,worldPosition); if(w>0) { total+=w; local+=w*HsLocalProbeShIrradiance((uint)p.parameters.y,worldNormal); } }
    const float cover=saturate(total);
    // A local capture already includes baseline environment. Local weights sum
    // to cover, and the one global contribution is weighted by 1-cover.
    return HsProbeIrradiance(worldNormal)*(1-cover)+(total>0?local*(cover/total):0);
}
float3 HsLocalProbePrefiltered(float3 worldPosition,float3 worldDirection,float perceptualRoughness)
{
    const uint count=HsLocalProbeCount(); float total=0; float3 local=0;
    for(uint i=0;i<count;++i)
    {
        const HsLocalProbeRecord p=HsLocalProbeRecords[i]; const float w=HsLocalProbeWeight(p,worldPosition);
        if(w>0) { total+=w; local+=w*HsLocalProbeGgxCube.SampleLevel(LinearClamp,
            float4(HsLocalProbeProjectedDirection(p,worldPosition,worldDirection),p.parameters.y),saturate(perceptualRoughness)*p.parameters.x).rgb; }
    }
    const float cover=saturate(total);
    return HsProbePrefiltered(worldDirection,perceptualRoughness)*(1-cover)+(total>0?local*(cover/total):0);
}
float3 HsLocalProbeRadiance(float3 worldPosition,float3 worldDirection)
{
    const uint count=HsLocalProbeCount(); float total=0; float3 local=0;
    for(uint i=0;i<count;++i)
    {
        const HsLocalProbeRecord p=HsLocalProbeRecords[i]; const float w=HsLocalProbeWeight(p,worldPosition);
        if(w>0) { total+=w; local+=w*HsLocalProbeRawCube.SampleLevel(LinearClamp,
            float4(HsLocalProbeProjectedDirection(p,worldPosition,worldDirection),p.parameters.y),0).rgb; }
    }
    const float cover=saturate(total);
    return HsProbeRadiance(worldDirection)*(1-cover)+(total>0?local*(cover/total):0);
}
#endif
