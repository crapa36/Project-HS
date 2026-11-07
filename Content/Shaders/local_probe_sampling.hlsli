#ifndef HS_LOCAL_PROBE_SAMPLING_HLSLI
#define HS_LOCAL_PROBE_SAMPLING_HLSLI

#include "probe_ibl.hlsli"

#include "local_probe_grid.hlsli"

TextureCubeArray<float4> HsLocalProbeRawCube : register(t0, space23);
TextureCubeArray<float4> HsLocalProbeGgxCube : register(t1, space23);

uint HsLocalProbeCount()
{
    uint count,stride; HsLocalProbeRecords.GetDimensions(count,stride);
    if(count==0 || stride!=128) return 0;
    return min(count,(uint)HsLocalProbeRecords[0].parameters.w);
}
float HsLocalProbeWeight(HsLocalProbeRecord p,float3 world)
{
    const float3 inside=min(world-p.influence_min_fade.xyz,p.influence_max_bias.xyz-world);
    return p.position_weight.w*smoothstep(0,p.influence_min_fade.w,min(inside.x,min(inside.y,inside.z)));
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
    // Both consumers use SampleLevel: cube coordinates depend only on direction ratios.
    return worldPosition+d*max(exit,0)-p.position_weight.xyz;
}
float3 HsLocalProbeIrradiance(HsProbeGridCell c,float3 normal,float3 kernel)
{
    if(!c.inside) return HsProbeIrradiance(normal,kernel);
    float basis[9]; HsProbeShBasis(normalize(normal),basis);
    [unroll] for(uint k=0;k<9;++k) basis[k]*=kernel[k==0?0:k<4?1:2];
    return HsLocalProbeGridIrradiance(c,basis);
}
void HsLocalProbeIrradiancePair(HsProbeGridCell c,float3 normal,out float3 front,out float3 back)
{
    if(!c.inside)
    {
        front=HsProbeIrradiance(normal);
        back=HsProbeIrradiance(-normal);
        return;
    }
    float basis[9]; HsProbeShBasis(normalize(normal),basis);
    HsLocalProbeGridIrradiancePair(c,basis,front,back);
}
float3 HsLocalProbeIrradiance(float3 world,float3 normal,float3 kernel)
{
    return HsLocalProbeIrradiance(HsLocateProbeCell(world),normal,kernel);
}
float3 HsLocalProbeIrradiance(float3 world,float3 normal)
{
    return HsLocalProbeIrradiance(world,normal,1.0.xxx);
}

// The baker rejects cells containing more than eight candidates, never truncates them.
struct HsLocalProbeBlend
{
    uint count;
    uint indices[8];
    float weights[8];
    float global_weight;
};
HsLocalProbeBlend HsLocalProbePrepareBlend(float3 world,HsProbeGridCell c)
{
    HsLocalProbeBlend blend=(HsLocalProbeBlend)0;
    blend.global_weight=1;
    if(!c.dims.x) return blend;
    uint2 candidates=asuint(HsLocalProbeCosineSh[c.metadata+4u].xy);
    float total=0;
    [unroll] for(uint word=0;word<2;++word)
    {
        uint bits=candidates[word];
        [loop] while(bits)
        {
            const uint id=word*32u+firstbitlow(bits); bits&=bits-1u;
            const float w=HsLocalProbeWeight(HsLocalProbeRecords[id],world);
            if(w>0)
            {
                blend.indices[blend.count]=id; blend.weights[blend.count]=w;
                ++blend.count; total+=w;
            }
        }
    }
    const float cover=saturate(total);
    blend.global_weight=1-cover;
    [loop] for(uint i=0;i<blend.count;++i) blend.weights[i]*=cover/total;
    return blend;
}
HsLocalProbeBlend HsLocalProbePrepareBlend(float3 world,bool enabled)
{
    if(enabled) return HsLocalProbePrepareBlend(world,HsLocateProbeCell(world));
    HsLocalProbeBlend blend=(HsLocalProbeBlend)0;
    blend.global_weight=1;
    return blend;
}
void HsLocalProbeEvaluateIbl(float3 world,HsProbeGridCell cell,float3 normal,float3 reflected,float2 roughness,
    bool need_coat,bool need_diffuse,bool local_specular,bool local_diffuse,float3 diffuse_kernel,
    out float3 base,out float3 coat,out float3 diffuse)
{
    diffuse=0;
    if(need_diffuse) diffuse=local_diffuse?HsLocalProbeIrradiance(cell,normal,diffuse_kernel):HsProbeIrradiance(normal,diffuse_kernel);
    float3 local_base=0,local_coat=0;
    float total=0;
    if(local_specular && cell.dims.x)
    {
        const uint2 candidates=asuint(HsLocalProbeCosineSh[cell.metadata+4u].xy);
        [unroll] for(uint word=0;word<2;++word)
        {
            uint bits=candidates[word];
            [loop] while(bits)
            {
                const uint id=word*32u+firstbitlow(bits);bits&=bits-1u;
                const HsLocalProbeRecord p=HsLocalProbeRecords[id];
                const float weight=HsLocalProbeWeight(p,world);
                if(weight>0)
                {
                    const float next_total=total+weight;
                    const float previous_weight=total/next_total,current_weight=weight/next_total;
                    const float4 dir=float4(HsLocalProbeProjectedDirection(p,world,reflected),id);
                    local_base=previous_weight*local_base+current_weight*
                        HsLocalProbeGgxCube.SampleLevel(LinearClamp,dir,roughness.x*p.parameters.x).rgb;
                    if(need_coat) local_coat=previous_weight*local_coat+current_weight*
                        HsLocalProbeGgxCube.SampleLevel(LinearClamp,dir,roughness.y*p.parameters.x).rgb;
                    total=next_total;
                }
            }
        }
    }
    const float coverage=saturate(total);
    base=0;coat=0;
    if(coverage<1)
    {
        base=(1-coverage)*HsProbePrefiltered(reflected,roughness.x);
        if(need_coat) coat=(1-coverage)*HsProbePrefiltered(reflected,roughness.y);
    }
    // The running convex mean avoids both a candidate array and an unnormalized
    // HDR sum; the global/local coverage still uses the original total weight.
    base+=local_base*coverage;
    if(need_coat) coat+=local_coat*coverage;
}
void HsLocalProbeEvaluateIbl(float3 world,float3 normal,float3 reflected,float2 roughness,
    bool need_coat,bool need_diffuse,bool local_specular,bool local_diffuse,float3 diffuse_kernel,
    out float3 base,out float3 coat,out float3 diffuse)
{
    HsProbeGridCell cell=(HsProbeGridCell)0;
    if(local_specular || (need_diffuse && local_diffuse)) cell=HsLocateProbeCell(world);
    HsLocalProbeEvaluateIbl(world,cell,normal,reflected,roughness,need_coat,need_diffuse,
        local_specular,local_diffuse,diffuse_kernel,base,coat,diffuse);
}
float3 HsLocalProbePrefiltered(float3 world,HsProbeGridCell cell,float3 direction,float roughness)
{
    float3 base,coat,diffuse;
    HsLocalProbeEvaluateIbl(world,cell,0,direction,roughness.xx,false,false,true,false,0,base,coat,diffuse);
    return base;
}
float3 HsLocalProbeRadiance(float3 world,float3 direction,HsLocalProbeBlend blend)
{
    float3 result=0;
    if(blend.global_weight>0) result=blend.global_weight*HsProbeRadiance(direction);
    [loop] for(uint i=0;i<blend.count;++i)
    {
        const uint id=blend.indices[i];
        const float4 d=float4(HsLocalProbeProjectedDirection(HsLocalProbeRecords[id],world,direction),id);
        result+=blend.weights[i]*HsLocalProbeRawCube.SampleLevel(LinearClamp,d,0).rgb;
    }
    return result;
}
#endif
