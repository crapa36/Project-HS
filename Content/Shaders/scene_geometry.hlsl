#include "project_hs.hlsl"
#include "scene_geometry.hlsli"
cbuffer SceneGeometryConstants : register(b0,space20) { uint4 SceneGeometryInfo; };
[numthreads(64,1,1)]
void SceneGeometryCS(uint3 dispatch : SV_DispatchThreadID)
{
    uint job=PassValue+dispatch.y;
    if(job>=SceneGeometryInfo.x) return;
    SceneGeometryRecord g=SceneDeformJobs[job];
    uint vertexId=dispatch.x+dispatch.z*(65535u*64u);
    if(vertexId>=g.range.z||(g.material.y&1)==0) return;
    SceneOutput vertex=HsSceneGeometryVertex(g,job,vertexId);
    // Software scene positions retain local coordinates; grass bending and
    // canonical placeholder shapes have already run through raster evaluation.
    HsSceneGeometryStorePositions(g,vertexId,SceneDrawInstances[job],vertex.WorldPosition);
}
