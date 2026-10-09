#include "ray_transport.hlsli"
[numthreads(8,8,1)]
void TransportComposeCS(uint3 dispatch:SV_DispatchThreadID)
{
    uint2 pixel=dispatch.xy;if(any(pixel>=uint2(RtRenderSize.xy)))return;
    int3 p=int3(pixel,0);float4 hdr=RtHdr.Load(p);
    if((RtFrameInfo.w&1u)!=0u)
    {
        float3 fallback=RtFallbackDiffuse.Load(p).rgb;float4 diffuse=RtGiClean.Load(p);
        hdr.rgb+=((RtFrameInfo.w&32u)!=0u?lerp(fallback,diffuse.rgb,saturate(diffuse.a)):diffuse.rgb)-fallback;
    }
    if((RtFrameInfo.w&2u)!=0u)
    {
        float3 environment=RtEnvironmentSpecular.Load(p).rgb;
        float4 specular=RtSpecularClean.Load(p);
        hdr.rgb+=lerp(environment,specular.rgb,saturate(specular.a))-environment;
    }
    RtComposed[pixel]=float4(clamp(hdr.rgb,0.0,65000.0),hdr.a);
}
