#include "ambient_occlusion.hlsl"
Texture2D<uint> AdGtaoDensity : register(t6,space33);
uint AdGtaoStride(uint2 pixel)
{
    const uint2 first=pixel&~1u;
    if(any(first+1u>=uint2(AoSize.xy)))return 1u;
    const AoGuide anchor=AoGuideForSample(first);
    if(!anchor.valid)return 1u;
    [unroll]for(uint y=0u;y<2u;++y)[unroll]for(uint x=0u;x<2u;++x)
    {
        const AoGuide g=AoGuideForSample(first+uint2(x,y));
        if(!g.valid||any(g.material!=anchor.material)||
            abs(g.position.z-anchor.position.z)>anchor.position.z*0.005||dot(g.normal,anchor.normal)<0.995||
            AdGtaoDensity.Load(int3(uint2(g.pixel)/8u,0))!=2u)return 1u;
    }
    return 2u;
}
[numthreads(8,8,1)]
void AdaptiveGtaoHorizonCS(uint3 dispatch:SV_DispatchThreadID)
{
    if(any(dispatch.xy>=uint2(AoSize.xy)))return;
    const AoGuide g=AoGuideForSample(dispatch.xy);
    const bool anchor=AdGtaoStride(dispatch.xy)==1u||all((dispatch.xy&1u)==0u);
    const float visibility=g.valid&&anchor?AoHorizonVisibility(g,dispatch.xy):1.0;
    // Omitted values are initialized; the following resolve replaces them
    // before any temporal consumer, with current pixel depth and normal.
    AoRawOutput[dispatch.xy]=float4(visibility,g.valid?g.position.z:0.0,g.valid?AoEncodeNormal(g.normal):0.0);
}
[numthreads(8,8,1)]
void AdaptiveGtaoResolveCS(uint3 dispatch:SV_DispatchThreadID)
{
    if(any(dispatch.xy>=uint2(AoSize.xy)))return;
    const AoGuide g=AoGuideForSample(dispatch.xy);
    float visibility=1.0;
    if(g.valid)
    {
        if(AdGtaoStride(dispatch.xy)==1u||all((dispatch.xy&1u)==0u))visibility=AoRaw.Load(int3(dispatch.xy,0)).x;
        else
        {
            const uint2 anchor=dispatch.xy&~1u;const AoGuide a=AoGuideForSample(anchor);
            const float4 raw=AoRaw.Load(int3(anchor,0));
            const bool compatible=AdGtaoStride(anchor)==2u&&raw.y>0.0&&all(a.material==g.material)&&
                abs(raw.y-g.position.z)<=g.position.z*0.005&&dot(AoDecodeNormal(raw.zw),g.normal)>=0.995;
            // A failed current-guide compatibility executes the real kernel;
            // it does not expose an omitted white sample or stale history.
            visibility=compatible?raw.x:AoHorizonVisibility(g,dispatch.xy);
        }
    }
    AoRawOutput[dispatch.xy]=float4(visibility,g.valid?g.position.z:0.0,g.valid?AoEncodeNormal(g.normal):0.0);
}
