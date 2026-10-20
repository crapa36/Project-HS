#include "brixelizer_common.hlsli"
[numthreads(8,8,1)]
void BrixelizerCacheExportCS(uint3 dispatch : SV_DispatchThreadID)
{
    uint2 p=dispatch.xy;if(any(p>=uint2(BrixRenderSize.xy)))return;
    // Actual deferred DirectDiffuse+Emission only. No specular, transparent,
    // UI or post signal enters retained native cache. First dispatch explicitly
    // consumes this seeded current cache; subsequent dispatches consume previous.
    float3 radiance=BrixCacheInjection.Load(int3(p,0)).rgb;
    if(!all(isfinite(radiance)))radiance=0.0;
    BrixDiffuseOutput[p]=float4(min(max(radiance,0.0),BrixCacheParameters.x),1.0);
}
