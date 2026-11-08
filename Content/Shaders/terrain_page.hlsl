#define HS_TERRAIN_PAGE_BAKE

struct HsTextureGradState { uint unused; };
HsTextureGradState HsTexturePrepareGrad(uint binding) { return (HsTextureGradState)0; }
float4 HsTextureSampleGradPrepared(Texture2DArray<float4> tex,uint slot,SamplerState samp,
    float3 uv,float2 dx,float2 dy,HsTextureGradState state) { return tex.SampleGrad(samp,uv,dx,dy); }
#include "material_terrain.hlsli"
#include "display_transform.hlsli"

Texture2DArray<float4> Sources[6] : register(t0);
Texture2DArray<float4> Previous : register(t6);
StructuredBuffer<uint> MeadowControls : register(t7);
SamplerState Wrap : register(s0);
SamplerState Clamp : register(s1);
RWTexture2DArray<float4> Target : register(u0);
cbuffer Page : register(b0) { HsTerrainSection Section; uint PlaneIndex, Edge; float2 ParentCenter, ParentSpan; };

void Bake(uint3 id, uint plane)
{
    if(any(id.xy>=Edge)) return;
    // Continue this receiver's material outside its geometric boundary, as
    // helper lanes do. Do not mix a neighbouring draw's mesh or seed.
    const float apron=float(Edge)/16.0;
    const float interior=float(Edge)-2*apron;
    float2 local=((id.xy+.5-apron)/interior-.5)*Section.span;
    float2 c=Section.rotation;
    float2 xAxis=float2(c.x,-c.y), zAxis=c.yx;
    float2 world=Section.center+local.x*xAxis+local.y*zAxis;
    const float2 parentDelta=world-ParentCenter;
    const float2 meadowLocal=Section.mesh==22u && all(ParentSpan!=0)
        ? float2(dot(parentDelta,xAxis),dot(parentDelta,zAxis))*24.0/ParentSpan : world;
    float2 dx=xAxis*Section.span.x/interior, dy=zAxis*Section.span.y/interior;
    float2 controls=0;
    if(Section.mesh==22u) {
        uint packed=MeadowControls[id.y*Edge+id.x];
        controls=float2(f16tof32(packed&65535u),f16tof32(packed>>16));
    }
    HsMaterialTerrainResult m=EnvTerrain(Sources[0],Sources[1],Sources[2],Sources[3],Sources[4],Sources[5],
        Wrap,Clamp,float3(world.x,0,world.y),float3(dx.x,0,dx.y),float3(dy.x,0,dy.y),
        Section.mesh,Section.seed,float3(0,1,0),0,0,1,2,3,4,5,controls,meadowLocal);
    if(plane==0u) Target[uint3(id.xy,0)]=float4(HsSrgbEncode(m.base_color),m.roughness);
    else if(plane==1u) Target[uint3(id.xy,0)]=float4(m.normal.xy*.5+.5,m.ao,saturate(m.residual_depth/.05));
    else Target[uint3(id.xy,0)]=m.layer_weights;
}
[numthreads(8,8,1)]
void BakeColorCS(uint3 id : SV_DispatchThreadID) { Bake(id, 0u); }
[numthreads(8,8,1)]
void BakeBaseCS(uint3 id : SV_DispatchThreadID) { Bake(id, 1u); }
[numthreads(8,8,1)]
void BakeWeightCS(uint3 id : SV_DispatchThreadID) { Bake(id, 2u); }
[numthreads(8,8,1)]
void MipCS(uint3 id : SV_DispatchThreadID)
{
    if(any(id.xy>=Edge)) return;
    float4 value=0;
    float3 normal=0;
    uint plane=PlaneIndex;
    [unroll] for(uint y=0;y<2;++y) [unroll] for(uint x=0;x<2;++x) {
        float4 s=Previous.Load(int4(id.xy*2+uint2(x,y),id.z,0));
        if(plane==0) s.rgb=HsSrgbDecode(s.rgb);
        else if(plane==1) {
            float2 n=s.rg*2-1;
            normal+=float3(n,sqrt(saturate(1-dot(n,n))));
        }
        value+=s*.25;
    }
    if(plane==0) value.rgb=HsSrgbEncode(value.rgb);
    else if(plane==1) value.rg=normalize(normal).xy*.5+.5;
    Target[id]=value;
}
