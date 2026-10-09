#ifndef HS_RAY_SER
#define HS_RAY_SER 1
#endif
#include "ray_reflections.hlsl"
[shader("raygeneration")]
void HybridReflectionTraceRayGen()
{
    RtTraceSpecularPixel(DispatchRaysIndex().xy);
}
