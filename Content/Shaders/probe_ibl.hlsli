#ifndef HS_PROBE_IBL_HLSLI
#define HS_PROBE_IBL_HLSLI

// Same authored scene-linear Rec709 radiance supplies the background and IBL.
// Exposure/display conversion are later passes. The cube excludes the sun disk.
TextureCube<float4> HsProbeRawCube : register(t0, space15);
TextureCube<float4> HsProbeGgxCube : register(t1, space15);
StructuredBuffer<float4> HsProbeCosineSh : register(t2, space15);

float3 HsProbeLocalDirection(float3 worldDirection)
{
    // Inverse of the authored positive +Y rotation: local probe -> world.
    const float3 d=normalize(worldDirection);
    return float3(ProbeOptions.x*d.x-ProbeOptions.y*d.z,d.y,
                  ProbeOptions.y*d.x+ProbeOptions.x*d.z);
}
float3 HsProbeRadiance(float3 worldDirection)
{
    return HsProbeRawCube.SampleLevel(LinearClamp,HsProbeLocalDirection(worldDirection),0).rgb;
}
float3 HsProbePrefiltered(float3 worldDirection,float perceptualRoughness)
{
    return HsProbeGgxCube.SampleLevel(LinearClamp,HsProbeLocalDirection(worldDirection),
        saturate(perceptualRoughness)*ProbeOptions.z).rgb;
}
float3 HsProbeIrradiance(float3 worldNormal, float3 kernel)
{
    const float3 n=HsProbeLocalDirection(worldNormal);
    const float basis[9]={0.28209479177387814,
        0.4886025119029199*n.y,0.4886025119029199*n.z,0.4886025119029199*n.x,
        1.0925484305920792*n.x*n.y,1.0925484305920792*n.y*n.z,
        0.31539156525252005*(3*n.z*n.z-1),1.0925484305920792*n.x*n.z,
        0.5462742152960396*(n.x*n.x-n.y*n.y)};
    float3 irradiance=0;
    [unroll] for(uint i=0;i<9;++i)
        irradiance+=HsProbeCosineSh[i].rgb*basis[i]*kernel[i==0?0:i<4?1:2];
    // SH truncation can produce a small negative reconstruction. Irradiance is
    // nonnegative; the baker stores signed coefficients without clamping them.
    return max(irradiance,0.0);
}
float3 HsProbeIrradiance(float3 worldNormal)
{
    return HsProbeIrradiance(worldNormal,1.0.xxx);
}
#endif
