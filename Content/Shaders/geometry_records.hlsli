#ifndef HS_GEOMETRY_RECORDS_HLSLI
#define HS_GEOMETRY_RECORDS_HLSLI
#include "render_binding_contract.hlsli"
#include "shadow_data.hlsli"
#include "scene_hlod_sampling.hlsli"

// Descriptor zero is the frame's records. Each immutable page owns four raw
// descriptors: canonical vertices, indices, meshlets and packed auxiliary.
ByteAddressBuffer GeometryBuffers[HsGeometryDescriptorCount] : register(t0, space5);

struct GeometryDrawRecord
{
    uint PageSlot, FirstIndex, IndexCount, FirstMeshlet;
    uint MeshletCount, VisibleBase, InstanceCount, Flags;
    uint TriangleOffset, RemapOffset, GeometryGeneration, Lod;
    uint MeshId, Reserved0, Reserved1, Reserved2;
};

GeometryDrawRecord LoadGeometryDrawRecord(uint id)
{
    const uint offset = id * 64u;
    const uint4 a = GeometryBuffers[0].Load4(offset);
    const uint4 b = GeometryBuffers[0].Load4(offset + 16u);
    const uint4 c = GeometryBuffers[0].Load4(offset + 32u);
    const uint4 d = GeometryBuffers[0].Load4(offset + 48u);
    GeometryDrawRecord record;
    record.PageSlot = a.x; record.FirstIndex = a.y;
    record.IndexCount = a.z; record.FirstMeshlet = a.w;
    record.MeshletCount = b.x; record.VisibleBase = b.y;
    record.InstanceCount = b.z; record.Flags = b.w;
    record.TriangleOffset = c.x; record.RemapOffset = c.y;
    record.GeometryGeneration = c.z; record.Lod = c.w;
    record.MeshId = d.x; record.Reserved0 = d.y;
    record.Reserved1 = d.z; record.Reserved2 = d.w;
    return record;
}

uint GeometryShadowPass(uint local_instance)
{
    // Indexed VSM commands store a tagged record base and use StartInstanceLocation 0.
    // Mesh/local shadow draws remain one record per command and keep their original pass.
    return HsVirtualShadowIsDraw(PassValue) ? PassValue + local_instance : PassValue;
}

uint GeometryVisibleInstance(uint local_instance)
{
    if (HsShadowIsDraw(PassValue)) return HsShadowLoadDraw(GeometryShadowPass(local_instance)).source_ref;
    // SV_InstanceID excludes native StartInstanceLocation. Geometry draws
    // address their structured instance/ref range explicitly in both frontends.
    return PassValue >= 4u
        ? LoadGeometryDrawRecord(PassValue >> 2u).VisibleBase + local_instance
        : local_instance;
}
#endif
