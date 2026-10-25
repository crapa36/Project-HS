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
float3 HsLocalProbeShIrradiance(uint probe,float3 worldNormal,float3 kernel)
{
    const float3 n=normalize(worldNormal);
    const float basis[9]={0.28209479177387814,
        0.4886025119029199*n.y,0.4886025119029199*n.z,0.4886025119029199*n.x,
        1.0925484305920792*n.x*n.y,1.0925484305920792*n.y*n.z,
        0.31539156525252005*(3*n.z*n.z-1),1.0925484305920792*n.x*n.z,
        0.5462742152960396*(n.x*n.x-n.y*n.y)};
    float3 irradiance=0;
    [unroll] for(uint coefficient=0;coefficient<9;++coefficient)
        irradiance+=HsLocalProbeCosineSh[probe*9+coefficient].rgb*basis[coefficient]*kernel[coefficient==0?0:coefficient<4?1:2];
    return max(irradiance,0);
}
float3 HsLocalProbeShIrradiance(uint probe,float3 worldNormal)
{
    return HsLocalProbeShIrradiance(probe,worldNormal,1.0.xxx);
}
float3 HsLocalProbeIrradiance(float3 worldPosition,float3 worldNormal,float3 kernel)
{
    const uint count=HsLocalProbeCount(); float total=0; float3 local=0;
    for(uint i=0;i<count;++i)
    { const HsLocalProbeRecord p=HsLocalProbeRecords[i]; const float w=HsLocalProbeWeight(p,worldPosition); if(w>0) { total+=w; local+=w*HsLocalProbeShIrradiance((uint)p.parameters.y,worldNormal,kernel); } }
    const float cover=saturate(total);
    // A local capture already includes baseline environment. Local weights sum
    // to cover, and the one global contribution is weighted by 1-cover.
    return HsProbeIrradiance(worldNormal,kernel)*(1-cover)+(total>0?local*(cover/total):0);
}
float3 HsLocalProbeIrradiance(float3 worldPosition,float3 worldNormal)
{
    return HsLocalProbeIrradiance(worldPosition,worldNormal,1.0.xxx);
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
void HsLocalProbeEvaluateIbl(float3 world,float3 normal,float3 reflected,float2 roughness,
    bool need_coat,bool need_diffuse,bool local_specular,bool local_diffuse,float3 diffuse_kernel,
    out float3 base,out float3 coat,out float3 diffuse)
{
    float3 base_sum=0.0,coat_sum=0.0,diffuse_sum=0.0;
    float total=0.0;
    if(local_specular || (need_diffuse && local_diffuse))
    {
        const uint count=HsLocalProbeCount();
        [loop] for(uint i=0u;i<count;++i)
        {
            const HsLocalProbeRecord p=HsLocalProbeRecords[i];
            const float weight=HsLocalProbeWeight(p,world);
            if(weight<=0.0)continue;
            total+=weight;
            if(local_specular)
            {
                const float3 direction=HsLocalProbeProjectedDirection(p,world,reflected);
                base_sum+=weight*HsLocalProbeGgxCube.SampleLevel(LinearClamp,
                    float4(direction,p.parameters.y),roughness.x*p.parameters.x).rgb;
                if(need_coat)
                    coat_sum+=weight*HsLocalProbeGgxCube.SampleLevel(LinearClamp,
                        float4(direction,p.parameters.y),roughness.y*p.parameters.x).rgb;
            }
            if(need_diffuse && local_diffuse)
                diffuse_sum+=weight*HsLocalProbeShIrradiance((uint)p.parameters.y,normal,diffuse_kernel);
        }
    }
    const float cover=saturate(total),scale=total>0.0?cover/total:0.0;
    const float specular_global=local_specular?1.0-cover:1.0;
    const float diffuse_global=local_diffuse?1.0-cover:1.0;
    base=local_specular?base_sum*scale:0.0;
    coat=need_coat && local_specular?coat_sum*scale:0.0;
    diffuse=need_diffuse && local_diffuse?diffuse_sum*scale:0.0;
    if(specular_global>0.0)
    {
        base+=HsProbePrefiltered(reflected,roughness.x)*specular_global;
        if(need_coat)coat+=HsProbePrefiltered(reflected,roughness.y)*specular_global;
    }
    if(need_diffuse && diffuse_global>0.0)
        diffuse+=HsProbeIrradiance(normal,diffuse_kernel)*diffuse_global;
}

float4 HsLocalProbeWeights(float3 worldPosition,uint count)
{
    float4 weights=0;
    // Cache the common small set without reserving registers for all 64 probes.
    [unroll] for(uint i=0u;i<4u;++i)
        if(i<count) weights[i]=HsLocalProbeWeight(HsLocalProbeRecords[i],worldPosition);
    return weights;
}
struct HsLocalProbeBlend
{
    float4 weights;
    float2 normalization; // Global remainder, local scale after raw accumulation.
};
HsLocalProbeBlend HsLocalProbePrepareBlend(float4 weights,uint count)
{
    HsLocalProbeBlend blend=(HsLocalProbeBlend)0;
    blend.weights=weights;
    if(count>4u) return blend;
    float total=0;
    [unroll] for(uint i=0u;i<4u;++i)
        if(i<count && weights[i]>0) total+=weights[i];
    const float cover=saturate(total);
    blend.normalization=float2(1-cover,total>0?cover/total:0);
    return blend;
}
float3 HsLocalProbeRadiance(float3 worldPosition,float3 worldDirection,uint count,HsLocalProbeBlend blend)
{
    [branch] if(count<=4u)
    {
        float3 local=0;
        [unroll] for(uint i=0u;i<4u;++i)
            [branch] if(i<count && blend.weights[i]>0)
            {
                const HsLocalProbeRecord p=HsLocalProbeRecords[i];
                local+=blend.weights[i]*HsLocalProbeRawCube.SampleLevel(LinearClamp,
                    float4(HsLocalProbeProjectedDirection(p,worldPosition,worldDirection),p.parameters.y),0).rgb;
            }
        return HsProbeRadiance(worldDirection)*blend.normalization.x+local*blend.normalization.y;
    }
    float total=0; float3 local=0;
    for(uint i=0;i<count;++i)
    {
        const HsLocalProbeRecord p=HsLocalProbeRecords[i]; float w;
        [branch] if(i<4u) w=blend.weights[i];
        else w=HsLocalProbeWeight(p,worldPosition);
        if(w>0) { total+=w; local+=w*HsLocalProbeRawCube.SampleLevel(LinearClamp,
            float4(HsLocalProbeProjectedDirection(p,worldPosition,worldDirection),p.parameters.y),0).rgb; }
    }
    const float cover=saturate(total);
    return HsProbeRadiance(worldDirection)*(1-cover)+(total>0?local*(cover/total):0);
}
#endif
