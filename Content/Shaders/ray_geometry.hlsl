#include "project_hs.hlsl"
#include "ray_hit.hlsli"
cbuffer RayGeometryConstants : register(b0,space20) { uint4 RayGeometryInfo; };
[numthreads(64,1,1)]
void RayGeometryCS(uint3 dispatch : SV_DispatchThreadID)
{
    uint job=PassValue;
    if(job>=RayGeometryInfo.x) return;
    RayHitGeometry g=RayDeformJobs[job];
    uint vertexId=dispatch.x+dispatch.y*(65535u*64u);
    if(vertexId>=g.range.z||(g.material.z&1)==0) return;
    SceneOutput vertex=HsRayVertex(g,job,vertexId);
    // BLAS stays in local coordinates; world-dependent grass bending and
    // canonical placeholder shapes have already run through raster evaluation.
    float3 local=HsRayWorldToLocal(RayDrawInstances[job],vertex.WorldPosition);
    RayPositionOutput.Store3(g.range.w+vertexId*12,asuint(local));
}
