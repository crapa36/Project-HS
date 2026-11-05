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
cbuffer Page : register(b0) { HsTerrainSection Section; uint SectionIndex, Edge; };

[numthreads(8,8,1)]
void BakeCS(uint3 id : SV_DispatchThreadID)
{
    if(any(id.xy>=Edge)) return;
    float2 local=((id.xy+.5)/float(Edge)-.5)*Section.span;
    float2 c=Section.rotation;
    float2 xAxis=float2(c.x,-c.y), zAxis=c.yx;
    float2 world=Section.center+local.x*xAxis+local.y*zAxis;
    float2 dx=xAxis*Section.span.x/Edge, dy=zAxis*Section.span.y/Edge;
    float2 controls=0;
    if(Section.mesh==22u) {
        uint packed=MeadowControls[id.y*Edge+id.x];
        controls=float2(f16tof32(packed&65535u),f16tof32(packed>>16));
    }
    HsMaterialTerrainResult m=EnvTerrain(Sources[0],Sources[1],Sources[2],Sources[3],Sources[4],Sources[5],
        Wrap,Clamp,float3(world.x,0,world.y),float3(dx.x,0,dx.y),float3(dy.x,0,dy.y),
        Section.mesh,Section.seed,float3(0,1,0),0,0,1,2,3,4,5,controls);
    uint slice=SectionIndex*3u;
    Target[uint3(id.xy,slice)]=float4(HsSrgbEncode(m.base_color),m.roughness);
    Target[uint3(id.xy,slice+1)]=float4(m.normal.xy*.5+.5,m.ao,saturate(m.residual_depth/.05));
    Target[uint3(id.xy,slice+2)]=m.layer_weights;
}
[numthreads(8,8,1)]
void MipCS(uint3 id : SV_DispatchThreadID)
{
    if(any(id.xy>=Edge)) return;
    float4 value=0;
    float3 normal=0;
    uint plane=id.z%3u;
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
