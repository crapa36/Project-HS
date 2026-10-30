#include "ambient_occlusion.hlsl"
Texture2D<uint> AdGtaoDensity : register(t6,space33);
groupshared AoGuide AdGtaoGuides[64];
groupshared uint AdGtaoDensities[64];
groupshared float4 AdGtaoRaw[64];
uint AdGtaoIndex(uint2 local) { return local.y*8u+local.x; }
void AdGtaoLoadTile(uint2 pixel,uint2 local)
{
    AoGuide g=(AoGuide)0;
    if(all(pixel<uint2(AoSize.xy)))g=AoGuideForSample(pixel);
    const uint index=AdGtaoIndex(local);
    AdGtaoGuides[index]=g;
    AdGtaoDensities[index]=g.valid?AdGtaoDensity.Load(int3(uint2(g.pixel)/8u,0)):0u;
    // Every lane participates, including the partial tile outside the active extent.
    GroupMemoryBarrierWithGroupSync();
}
uint AdGtaoStride(uint2 pixel,uint2 local)
{
    const uint2 first=pixel&~1u;
    if(any(first+1u>=uint2(AoSize.xy)))return 1u;
    const uint2 firstLocal=local&~1u;
    const AoGuide anchor=AdGtaoGuides[AdGtaoIndex(firstLocal)];
    if(!anchor.valid)return 1u;
    [unroll]for(uint y=0u;y<2u;++y)[unroll]for(uint x=0u;x<2u;++x)
    {
        const uint index=AdGtaoIndex(firstLocal+uint2(x,y));
        const AoGuide g=AdGtaoGuides[index];
        if(!g.valid||any(g.material!=anchor.material)||
            abs(g.position.z-anchor.position.z)>anchor.position.z*0.005||dot(g.normal,anchor.normal)<0.995||
            AdGtaoDensities[index]!=2u)return 1u;
    }
    return 2u;
}
[numthreads(8,8,1)]
void AdaptiveGtaoCS(uint3 dispatch:SV_DispatchThreadID,uint3 local:SV_GroupThreadID)
{
    AdGtaoLoadTile(dispatch.xy,local.xy);
    const uint index=AdGtaoIndex(local.xy);
    const bool inBounds=all(dispatch.xy<uint2(AoSize.xy));
    const AoGuide g=AdGtaoGuides[AdGtaoIndex(local.xy)];
    bool anchor=false;
    float candidate=1.0;
    if(inBounds)
    {
        anchor=AdGtaoStride(dispatch.xy,local.xy)==1u||all((dispatch.xy&1u)==0u);
        candidate=g.valid&&anchor?AoHorizonVisibility(g,dispatch.xy):1.0;
        AdGtaoRaw[index]=float4(candidate,g.valid?g.position.z:0.0,g.valid?AoEncodeNormal(g.normal):0.0);
    }
    else AdGtaoRaw[index]=0.0;
    GroupMemoryBarrierWithGroupSync();
    if(!inBounds)return;

    float visibility=1.0;
    if(g.valid)
    {
        if(anchor)visibility=candidate;
        else
        {
            const AoGuide a=AdGtaoGuides[AdGtaoIndex(local.xy&~1u)];
            const float4 raw=AdGtaoRaw[AdGtaoIndex(local.xy&~1u)];
            // Preserve the encoded normal/depth contract used by the former resolve pass.
            const bool compatible=raw.y>0.0&&all(a.material==g.material)&&
                abs(raw.y-g.position.z)<=g.position.z*0.005&&dot(AoDecodeNormal(raw.zw),g.normal)>=0.995;
            visibility=compatible?raw.x:AoHorizonVisibility(g,dispatch.xy);
        }
    }
    AoRawOutput[dispatch.xy]=float4(visibility,g.valid?g.position.z:0.0,g.valid?AoEncodeNormal(g.normal):0.0);
}
