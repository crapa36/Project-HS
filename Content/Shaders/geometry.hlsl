#include "project_hs.hlsl"
#include "geometry.hlsli"

// A cluster list spans physical pages. Pull its canonical index/vertex using
// the GPU draw record instead of depending on one IA binding for the list.
SceneOutput ClusterSceneVS(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    const GeometryDrawRecord record = LoadGeometryDrawRecord(HsShadowIsDraw(PassValue)
        ? HsShadowLoadDraw(PassValue).geometry_record : PassValue >> 2u);
    const uint descriptors = 1u + record.PageSlot * 4u;
    const uint canonical_vertex = GeometryBuffers[NonUniformResourceIndex(descriptors + 1u)].Load(
        (record.FirstIndex + vertex_id) * 4u);
    return SceneVS(LoadGeometryVertex(descriptors, canonical_vertex), instance_id, canonical_vertex);
}

SceneOutput ClusterShadowVS(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    SceneOutput output = ClusterSceneVS(vertex_id, instance_id);
    output.Position = HsShadowIsDraw(PassValue)
        ? HsShadowProject(output.WorldPosition, PassValue)
        : mul(float4(output.WorldPosition, 1.0), ShadowViewProjection[(PassValue & 3u) - 1u]);
    return output;
}

[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void SceneMS(uint3 group_id : SV_GroupID, uint thread_id : SV_GroupIndex,
             out vertices SceneOutput vertices[64], out indices uint3 triangles[124],
             out primitives GeometryPrimitive primitives[124])
{
    EmitGeometryMesh(group_id, thread_id, false, vertices, triangles, primitives);
}

[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void ShadowMS(uint3 group_id : SV_GroupID, uint thread_id : SV_GroupIndex,
              out vertices SceneOutput vertices[64], out indices uint3 triangles[124],
              out primitives GeometryPrimitive primitives[124])
{
    EmitGeometryMesh(group_id, thread_id, true, vertices, triangles, primitives);
}
