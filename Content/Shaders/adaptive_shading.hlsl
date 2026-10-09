cbuffer AdaptiveShadingConstants : register(b0,space33)
{
    uint4 AdExtent;
    float4 AdDepthParameters;
    uint4 AdPolicy;
};
Texture2D<float> AdDepth : register(t0,space33);
Texture2D<float2> AdNormal : register(t1,space33);
Texture2D<float4> AdBase : register(t2,space33);
Texture2D<float4> AdMaterial : register(t3,space33);
Texture2D<uint2> AdMetadata : register(t4,space33);
Texture2D<float2> AdMotion : register(t5,space33);
Texture2D<uint> AdDensity : register(t6,space33);
StructuredBuffer<uint2> AdMaterialPolicy : register(t7,space33);
RWTexture2D<uint> AdDensityOutput : register(u0,space33);
RWTexture2D<uint> AdRateOutput : register(u1,space33);
groupshared uint AdSensitive;
float3 AdDecodeNormal(float2 oct)
{
    float3 n=float3(oct,1.0-abs(oct.x)-abs(oct.y));
    if(n.z<0.0)n.xy=(1.0-abs(n.yx))*float2(n.x>=0.0?1.0:-1.0,n.y>=0.0?1.0:-1.0);
    return normalize(n);
}
bool AdEligible(uint material)
{
    uint first=0u,last=AdPolicy.x;
    [loop]while(first<last)
    {
        const uint middle=first+(last-first)/2u;const uint2 policy=AdMaterialPolicy[middle];
        if(policy.x<material)first=middle+1u;else last=middle;
    }
    if(first>=AdPolicy.x)return false;
    return AdMaterialPolicy[first].x==material&&AdMaterialPolicy[first].y==1u;
}
float AdLinearDepth(float hardware)
{
    const float denominator=hardware*AdDepthParameters.y-AdDepthParameters.x;
    return denominator!=0.0?(AdDepthParameters.z-hardware*AdDepthParameters.w)/denominator:0.0;
}
struct AdGuide{float depth;float3 normal;float4 base,material;uint2 key;bool valid;};
AdGuide AdLoad(int2 pixel)
{
    AdGuide g=(AdGuide)0;
    if(any(pixel<0)||any(pixel>=int2(AdExtent.xy)))return g;
    const float hardware=AdDepth.Load(int3(pixel,0));g.depth=AdLinearDepth(hardware);
    g.key=AdMetadata.Load(int3(pixel,0));g.normal=AdDecodeNormal(AdNormal.Load(int3(pixel,0)));
    g.base=AdBase.Load(int3(pixel,0));g.material=AdMaterial.Load(int3(pixel,0));
    const float2 motion=AdMotion.Load(int3(pixel,0))*float2(AdExtent.xy);
    g.valid=hardware>0.0&&hardware<=1.0&&g.depth>0.0&&isfinite(g.depth)&&all(isfinite(g.normal))&&
        all(isfinite(g.base))&&all(isfinite(g.material))&&all(isfinite(motion))&&
        g.key.x!=0u&&AdEligible(g.key.x)&&g.base.a>=0.999&&g.material.r>=0.45&&g.material.g<=0.05&&
        all(abs(motion)<=0.25);
    return g;
}
[numthreads(8,8,1)]
void AdaptiveShadingClassifyCS(uint3 group:SV_GroupID,uint3 thread:SV_GroupThreadID,uint lane:SV_GroupIndex)
{
    if(lane==0u)AdSensitive=AdPolicy.w!=0u?1u:0u;
    GroupMemoryBarrierWithGroupSync();
    const int2 origin=int2(group.xy*8u);const AdGuide center=AdLoad(origin+4);
    bool sensitive=!center.valid;
    // The100 pixels of the tile plus one-pixel halo are all visited. Active
    // bounds are checked before every guide load; partial/boundary tiles stay1x1.
    [unroll(2)]for(int y=int(thread.y)-1;y<=8;y+=8)
        [unroll(2)]for(int x=int(thread.x)-1;x<=8;x+=8)
        {
            const AdGuide g=AdLoad(origin+int2(x,y));
            sensitive=sensitive||!g.valid||any(g.key!=center.key)||
                abs(g.depth-center.depth)>center.depth*0.005||dot(g.normal,center.normal)<0.995||
                any(abs(g.base.rgb-center.base.rgb)>0.02)||any(abs(g.material-center.material)>0.025);
        }
    if(sensitive)InterlockedOr(AdSensitive,1u);
    GroupMemoryBarrierWithGroupSync();
    if(lane==0u)AdDensityOutput[group.xy]=AdSensitive!=0u?1u:2u;
}
[numthreads(8,8,1)]
void AdaptiveShadingRateCS(uint3 dispatch:SV_DispatchThreadID)
{
    uint width,height;AdRateOutput.GetDimensions(width,height);
    if(any(dispatch.xy>=uint2(width,height)))return;
    const uint2 first=dispatch.xy*AdExtent.w,last=min(first+AdExtent.w,AdExtent.xy);
    uint rate=0u; // D3D12_SHADING_RATE_1X1. No extra-rate hardware assumption.
    if(all(first<AdExtent.xy)&&all(last-first==AdExtent.w))
    {
        bool coarse=true;
        const uint2 begin=first/8u,end=(last+7u)/8u;
        [loop]for(uint y=begin.y;y<end.y;++y)[loop]for(uint x=begin.x;x<end.x;++x)
            coarse=coarse&&AdDensity.Load(int3(uint2(x,y),0))==2u;
        if(coarse)rate=5u; // Actual D3D12_SHADING_RATE_2X2 encoding.
    }
    AdRateOutput[dispatch.xy]=rate;
}
