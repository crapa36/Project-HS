#include "brixelizer_common.hlsli"
[numthreads(8,8,1)]
void BrixelizerGiComposeCS(uint3 dispatch : SV_DispatchThreadID)
{
    uint2 p=dispatch.xy;if(any(p>=uint2(BrixRenderSize.xy)))return;
    float4 hdr=BrixHdr.Load(int3(p,0));
    // This stage has a separate immutable descriptor bank: t9 is the actual
    // diffuse delta and t13 is the material-remodulated native specular.
    hdr.rgb+=BrixFallbackDiffuse.Load(int3(p,0)).rgb+
        BrixRawSpecular.Load(int3(p,0)).rgb-BrixEnvironmentSpecular.Load(int3(p,0)).rgb;
    BrixComposedOutput[p]=hdr;
}
