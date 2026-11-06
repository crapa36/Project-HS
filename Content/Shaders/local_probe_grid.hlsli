#ifndef HS_LOCAL_PROBE_GRID_HLSLI
#define HS_LOCAL_PROBE_GRID_HLSLI

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
StructuredBuffer<float4> HsLocalProbeCosineSh : register(t2, space23);
// SH9 rows are followed by five float4-sized cell records (packed as uint32).
StructuredBuffer<HsLocalProbeRecord> HsLocalProbeRecords : register(t4, space23);

float HsLocalProbeIsotropicBasis()
{
    return 0.0897935610625833; // Y00 / pi for cosine-convolved SH.
}
struct HsProbeGridCell
{
    uint3 dims,cell;
    uint metadata;
    float3 fraction;
    bool inside;
};
HsProbeGridCell HsLocateProbeCell(float3 world)
{
    HsProbeGridCell c=(HsProbeGridCell)0;
    const HsLocalProbeRecord header=HsLocalProbeRecords[0];
    const uint packed=(uint)header.reserved0.w;
    if(!packed) return c;
    c.dims=(packed>>uint3(0,8,16))&255u;
    const float3 q=(world-header.reserved0.xyz)/header.reserved1.xyz;
    // Extend the lowest layer down to the ground below the capture near plane.
    c.inside=all(q>=float3(0,-.5,0)) && all(q<=float3(c.dims-1u));
    const float3 position=clamp(q,0.0,float3(c.dims-1u));
    c.cell=min(uint3(position),c.dims-2u);
    c.fraction=position-float3(c.cell);
    const uint index=c.cell.x+(c.dims.x-1u)*(c.cell.y+(c.dims.y-1u)*c.cell.z);
    c.metadata=uint(header.reserved1.w)*9u+index*5u;
    return c;
}
float3 HsLocalProbeGridIrradiance(HsProbeGridCell c,float basis[9])
{
    const uint3 sub=min(uint3(c.fraction*4),3u);
    const uint index=sub.x+4*sub.y+16*sub.z;
    const uint4 packed=asuint(HsLocalProbeCosineSh[c.metadata+index/16u]);
    const uint mask=(packed[(index%16u)/4u]>>(8u*(index%4u)))&255u;
    if(!mask) return 0; // Interior with no reachable samples receives no diffuse sky through walls.
    float3 irradiance=0; float total=0;
    [loop] for(uint corner=0;corner<8;++corner)
    {
        if(!(mask&(1u<<corner))) continue;
        const uint3 bit=(corner>>uint3(0,1,2))&1u;
        const float3 w=lerp(1-c.fraction,c.fraction,float3(bit));
        const float weight=w.x*w.y*w.z;
        if(weight==0) continue;
        const uint3 node=c.cell+bit;
        const uint row=9u*(node.x+c.dims.x*(node.y+c.dims.y*node.z));
        float3 value=0;
        [unroll] for(uint k=0;k<9;++k) value+=basis[k]*HsLocalProbeCosineSh[row+k].rgb;
        irradiance+=weight*value; total+=weight;
    }
    return total>0?max(irradiance/total,0):0;
}
#endif
