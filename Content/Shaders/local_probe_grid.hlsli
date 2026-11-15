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
StructuredBuffer<float4> HsLocalProbeData : register(t2, space23);
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
    c.metadata=7u*c.dims.x*c.dims.y*c.dims.z+index*5u;
    return c;
}
void HsAccumulateProbeShBlock(inout float3 value,float4 row,uint block,float basis[9],float odd_sign)
{
    switch(block)
    {
    case 0: value+=row.xyz*basis[0]; value.x+=row.w*(basis[1]*odd_sign); break;
    case 1: value.yz+=row.xy*(basis[1]*odd_sign); value.xy+=row.zw*(basis[2]*odd_sign); break;
    case 2: value.z+=row.x*(basis[2]*odd_sign); value+=row.yzw*(basis[3]*odd_sign); break;
    case 3: value+=row.xyz*basis[4]; value.x+=row.w*basis[5]; break;
    case 4: value.yz+=row.xy*basis[5]; value.xy+=row.zw*basis[6]; break;
    case 5: value.z+=row.x*basis[6]; value+=row.yzw*basis[7]; break;
    case 6: value+=row.xyz*basis[8]; break;
    }
}
void HsLocalProbeGridIrradianceComponents(HsProbeGridCell c,float basis[9],
    bool need_back,out float3 front,out float3 back)
{
    const uint3 sub=min(uint3(c.fraction*4),3u);
    const uint index=sub.x+4*sub.y+16*sub.z;
    const uint4 packed=asuint(HsLocalProbeData[c.metadata+index/16u]);
    const uint mask=(packed[(index%16u)/4u]>>(8u*(index%4u)))&255u;
    front=0; back=0;
    if(!mask) return; // No reachable samples means no diffuse sky through walls.
    float total=0;
    [unroll] for(uint corner=0;corner<8;++corner)
    {
        if(!(mask&(1u<<corner))) continue;
        const uint3 bit=(corner>>uint3(0,1,2))&1u;
        const float3 w=lerp(1-c.fraction,c.fraction,float3(bit));
        const float weight=w.x*w.y*w.z;
        if(weight==0) continue;
        const uint3 node=c.cell+bit;
        const uint first=7u*(node.x+c.dims.x*(node.y+c.dims.y*node.z));
        float3 front_value=0,back_value=0;
        [unroll] for(uint block=0;block<7;++block)
        {
            const float4 row=HsLocalProbeData[first+block];
            HsAccumulateProbeShBlock(front_value,row,block,basis,1.0);
            if(need_back) HsAccumulateProbeShBlock(back_value,row,block,basis,-1.0);
        }
        front+=weight*front_value;
        if(need_back) back+=weight*back_value;
        total+=weight;
    }
    front=total>0?max(front/total,0):0;
    if(need_back) back=total>0?max(back/total,0):0;
}
float3 HsLocalProbeGridIrradiance(HsProbeGridCell c,float basis[9])
{
    float3 front,back;
    HsLocalProbeGridIrradianceComponents(c,basis,false,front,back);
    return front;
}
void HsLocalProbeGridIrradiancePair(HsProbeGridCell c,float basis[9],
    out float3 front,out float3 back)
{
    HsLocalProbeGridIrradianceComponents(c,basis,true,front,back);
}
#endif
