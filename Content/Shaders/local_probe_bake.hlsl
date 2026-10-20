// Native static-scene cube convolution. Capture itself uses the renderer's
// common material/opacity path and outputs clean HDR radiance plus hit distance.
// No local probe sampling, camera exposure or screen histories are used.
cbuffer HsLocalProbeBakeConstants : register(b0, space23)
{
    uint BakeEdge, BakeMipCount, BakeProbeCount, BakeSampleCount;
    uint BakeWriteMip, BakeWriteEdge, BakeWriteProbe, BakeReserved;
};
TextureCubeArray<float4> HsLocalBakeRawCube : register(t0, space23);
Texture2DArray<float4> HsLocalBakeRawFaces : register(t5, space23);
RWTexture2DArray<float4> HsLocalBakeGgx : register(u0, space23);
RWStructuredBuffer<float4> HsLocalBakeSh : register(u1, space23);
SamplerState HsLocalBakeLinearClamp : register(s0, space23);
static const float HsLocalBakePi=3.14159265358979323846;

float3 HsLocalBakeDirection(uint face,float2 xy)
{
    if(face==0) return normalize(float3(1,-xy.y,-xy.x));
    if(face==1) return normalize(float3(-1,-xy.y,xy.x));
    if(face==2) return normalize(float3(xy.x,1,xy.y));
    if(face==3) return normalize(float3(xy.x,-1,-xy.y));
    if(face==4) return normalize(float3(xy.x,-xy.y,1));
    return normalize(float3(-xy.x,-xy.y,-1));
}
float HsLocalBakeBasis(uint coefficient,float3 n)
{
    if(coefficient==0) return 0.28209479177387814;
    if(coefficient==1) return 0.4886025119029199*n.y;
    if(coefficient==2) return 0.4886025119029199*n.z;
    if(coefficient==3) return 0.4886025119029199*n.x;
    if(coefficient==4) return 1.0925484305920792*n.x*n.y;
    if(coefficient==5) return 1.0925484305920792*n.y*n.z;
    if(coefficient==6) return 0.31539156525252005*(3*n.z*n.z-1);
    if(coefficient==7) return 1.0925484305920792*n.x*n.z;
    return 0.5462742152960396*(n.x*n.x-n.y*n.y);
}
float HsLocalBakeArea(float2 p) { return atan2(p.x*p.y,sqrt(dot(p,p)+1)); }
groupshared float4 HsLocalBakeSums[64];
// One group per SH coefficient, one completed probe per graph. Parallel texel
// reduction integrates exact cubemap solid angles, then cosine-convolves SH9.
[numthreads(64,1,1)]
void CSSh(uint3 group:SV_GroupID,uint lane:SV_GroupIndex)
{
    const uint coefficient=group.x,probe=BakeWriteProbe;
    if(coefficient>=9 || probe>=BakeProbeCount) return;
    float4 sum=0;
    const uint faceTexels=BakeEdge*BakeEdge,totalTexels=faceTexels*6;
    for(uint texel=lane;texel<totalTexels;texel+=64)
    {
        const uint face=texel/faceTexels,within=texel%faceTexels;
        const uint2 pixel=uint2(within%BakeEdge,within/BakeEdge);
        const float2 low=2.0*float2(pixel)/BakeEdge-1.0,high=2.0*float2(pixel+1)/BakeEdge-1.0;
        const float omega=HsLocalBakeArea(high)-HsLocalBakeArea(float2(low.x,high.y))-
            HsLocalBakeArea(float2(high.x,low.y))+HsLocalBakeArea(low);
        const float3 direction=HsLocalBakeDirection(face,(low+high)*0.5);
        const float3 radiance=HsLocalBakeRawFaces.Load(int4(pixel,probe*6+face,0)).rgb;
        sum+=float4(radiance*(omega*HsLocalBakeBasis(coefficient,direction)),omega);
    }
    HsLocalBakeSums[lane]=sum; GroupMemoryBarrierWithGroupSync();
    [unroll] for(uint stride=32;stride>0;stride>>=1)
    {
        if(lane<stride) HsLocalBakeSums[lane]+=HsLocalBakeSums[lane+stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if(lane==0)
    {
        const float convolution=coefficient==0?HsLocalBakePi:(coefficient<4?2*HsLocalBakePi/3:HsLocalBakePi/4);
        HsLocalBakeSh[probe*9+coefficient]=float4(HsLocalBakeSums[0].rgb*(4*HsLocalBakePi/HsLocalBakeSums[0].w)*convolution,0);
    }
}

// Same N=V=R GGX split-sum convolution as the portable global probe producer.
// alpha=perceptualRoughness^2; NDF samples are normalized by accumulated NdotL.
[numthreads(8,8,1)]
void CSGgx(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=BakeWriteEdge || id.y>=BakeWriteEdge || id.z>=6 || BakeWriteProbe>=BakeProbeCount) return;
    const uint layer=BakeWriteProbe*6+id.z;
    const float3 n=HsLocalBakeDirection(id.z,2.0*(float2(id.xy)+0.5)/BakeWriteEdge-1.0);
    if(BakeWriteMip==0)
    {
        HsLocalBakeGgx[uint3(id.xy,layer)]=float4(HsLocalBakeRawFaces.Load(int4(id.xy,layer,0)).rgb,1); return;
    }
    const float roughness=float(BakeWriteMip)/(BakeMipCount-1),alpha=roughness*roughness;
    const float3 tangent=normalize(cross(abs(n.z)<0.999?float3(0,0,1):float3(1,0,0),n));
    const float3 bitangent=cross(n,tangent); float3 sum=0; float total=0;
    for(uint sampleIndex=0;sampleIndex<BakeSampleCount;++sampleIndex)
    {
        const float u=reversebits(sampleIndex)*2.3283064365386963e-10;
        const float phi=2*HsLocalBakePi*(float(sampleIndex)+0.5)/BakeSampleCount;
        const float cosTheta=sqrt((1-u)/(1+(alpha*alpha-1)*u));
        const float sinTheta=sqrt(max(0,1-cosTheta*cosTheta));
        const float3 h=tangent*(sinTheta*cos(phi))+bitangent*(sinTheta*sin(phi))+n*cosTheta;
        const float3 l=2*dot(n,h)*h-n; const float nl=saturate(dot(n,l));
        if(nl>0) { sum+=HsLocalBakeRawCube.SampleLevel(HsLocalBakeLinearClamp,float4(l,BakeWriteProbe),0).rgb*nl; total+=nl; }
    }
    HsLocalBakeGgx[uint3(id.xy,layer)]=float4(sum/total,1);
}
