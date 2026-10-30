#ifndef HS_GEOMETRY_HLSLI
#define HS_GEOMETRY_HLSLI

#include "geometry_records.hlsli"

SceneInput LoadGeometryVertex(uint descriptor, uint vertex_id)
{
    const uint offset = vertex_id * 76u;
    SceneInput input;
    input.Position = asfloat(GeometryBuffers[NonUniformResourceIndex(descriptor)].Load3(offset));
    input.Normal = asfloat(GeometryBuffers[NonUniformResourceIndex(descriptor)].Load3(offset + 12u));
    const uint2 bones = GeometryBuffers[NonUniformResourceIndex(descriptor)].Load2(offset + 24u);
    input.BoneIndices = uint4(bones.x & 65535u, bones.x >> 16u,
                        bones.y & 65535u, bones.y >> 16u);
    input.BoneWeights = asfloat(GeometryBuffers[NonUniformResourceIndex(descriptor)].Load4(offset + 32u));
    input.Uv = asfloat(GeometryBuffers[NonUniformResourceIndex(descriptor)].Load2(offset + 48u));
    input.Tangent = asfloat(GeometryBuffers[NonUniformResourceIndex(descriptor)].Load4(offset + 56u));
    input.Material = GeometryBuffers[NonUniformResourceIndex(descriptor)].Load(offset + 72u) & 65535u;
    return input;
}

struct GeometryPrimitive
{
    uint CanonicalPrimitive : SV_PrimitiveID;
};

void EmitGeometryMesh(uint3 group_id, uint thread_id, bool shadow,
                      out SceneOutput vertices[64], out uint3 triangles[124],
                      out GeometryPrimitive primitives[124])
{
    const bool cached_shadow = HsShadowIsDraw(PassValue);
    const uint record_id = cached_shadow ? HsShadowLoadDraw(PassValue).geometry_record : PassValue >> 2u;
    GeometryDrawRecord record = LoadGeometryDrawRecord(record_id);
    if (cached_shadow)
    {
        record.InstanceCount = 1u;
        record.VisibleBase = HsShadowLoadDraw(PassValue).source_ref;
    }
    const uint group = group_id.x + group_id.y * 65535u;
    // InstanceCount is the actual GPU-compacted count for indirect draws. The
    // CPU capacity only chose a dispatch that fits; padded groups emit nothing.
    const bool active = record_id != 0u && record.MeshletCount != 0u &&
        group < record.MeshletCount * record.InstanceCount;
    const uint descriptors = 1u + record.PageSlot * 4u;
    uint4 header = uint4(0u, 0u, 0u, 0u);
    if (active)
    {
        const uint meshlet = record.FirstMeshlet + group % record.MeshletCount;
        header = GeometryBuffers[NonUniformResourceIndex(descriptors + 2u)].Load4(meshlet * 64u);
    }
    const uint first_ref = header.x;
    const uint vertex_count = header.y;
    const uint first_triangle = header.z;
    const uint triangle_count = header.w;
    // One uniform call per group, including the zero-output padded groups.
    SetMeshOutputCounts(vertex_count, triangle_count);
    if (!active) return;
    const uint visible = record.VisibleBase + group / record.MeshletCount;
    if (thread_id < vertex_count)
    {
        const uint unique_vertex = GeometryBuffers[NonUniformResourceIndex(descriptors + 3u)].Load(
            (first_ref + thread_id) * 4u);
        const SceneInput input = LoadGeometryVertex(descriptors, unique_vertex);
        SceneOutput output = EvaluateSceneVertex(input, visible, unique_vertex,
            (record.Flags & 1u) != 0u, !shadow, PassValue);
        if (shadow)
            output.Position = cached_shadow ? HsShadowProject(output.WorldPosition, PassValue)
                : mul(float4(output.WorldPosition, 1.0), ShadowViewProjection[(PassValue & 3u) - 1u]);
        vertices[thread_id] = output;
    }
    for (uint primitive_index = thread_id; primitive_index < triangle_count; primitive_index += 64u)
    {
        const uint canonical_slot = first_triangle + primitive_index;
        const uint packed = GeometryBuffers[NonUniformResourceIndex(descriptors + 3u)].Load(
            record.TriangleOffset + canonical_slot * 4u);
        triangles[primitive_index] = uint3(packed & 255u, (packed >> 8u) & 255u, (packed >> 16u) & 255u);
        primitives[primitive_index].CanonicalPrimitive =
            GeometryBuffers[NonUniformResourceIndex(descriptors + 3u)].Load(
                record.RemapOffset + canonical_slot * 4u);
    }
}
#endif
