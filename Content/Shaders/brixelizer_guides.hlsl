#include "brixelizer_common.hlsli"
[numthreads(8,8,1)]
void BrixelizerGuidesCS(uint3 dispatch : SV_DispatchThreadID)
{
    uint2 p=dispatch.xy;if(any(p>=uint2(BrixRenderSize.xy)))return;
    bool valid=BrixSurfaceValid(p);float3 normal=float3(0,0,1);float roughness=1.0;
    if(valid){HsSurfaceClosure c=BrixClosure(p);normal=normalize(c.normal);roughness=saturate(c.roughness);}
    // Exact active-resolution footprint, reversed hardware depth. SDK loads
    // normal XYZ directly with unpackMul1/add0 and linear alpha with flagfalse.
    BrixDepthOutput[p]=valid?BrixDepth.Load(int3(p,0)):0.0;
    BrixNormalOutput[p]=float4(normal,0.0);
    BrixRoughnessOutput[p]=roughness*roughness;
    BrixMotionOutput[p]=valid?BrixMotion.Load(int3(p,0))+BrixJitterUv.zw-BrixJitterUv.xy:0.0;
}
