#ifndef HS_RAY_SER
#define HS_RAY_SER 1
#endif
#include "ray_gi.hlsl"
[shader("raygeneration")]
void GITraceRayGen()
{
    RtTraceGiPixel(DispatchRaysIndex().xy);
}
