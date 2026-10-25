#ifndef HS_SCENE_GEOMETRY_HLSLI
#define HS_SCENE_GEOMETRY_HLSLI
// Software scene deformation shares the exact raster vertex evaluation.
struct SceneGeometryRecord
{
    uint4 source; // vertex descriptor, index descriptor, first index, index count
    uint4 range; // source byte offset, stride, vertex count, deformed position offset
    uint4 identity; // persistent scene slot, generation, skin slot, mesh ID
    uint2 material; // environment ordinal, flags
};
StructuredBuffer<SceneGeometryRecord> SceneDeformJobs : register(t3,space20);
StructuredBuffer<InstanceData> SceneDrawInstances : register(t14,space20);
RWByteAddressBuffer ScenePositionOutput : register(u0,space20);

SceneInput HsSceneGeometryCanonicalVertex(SceneGeometryRecord g,uint vertex)
{
    uint offset=g.range.x+vertex*g.range.y;
    SceneInput v;
    v.Position=asfloat(GeometryBuffers[NonUniformResourceIndex(g.source.x)].Load3(offset));
    v.Normal=asfloat(GeometryBuffers[NonUniformResourceIndex(g.source.x)].Load3(offset+12));
    uint2 bones=GeometryBuffers[NonUniformResourceIndex(g.source.x)].Load2(offset+24);
    v.BoneIndices=uint4(bones.x&65535,bones.x>>16,bones.y&65535,bones.y>>16);
    v.BoneWeights=asfloat(GeometryBuffers[NonUniformResourceIndex(g.source.x)].Load4(offset+32));
    v.Uv=asfloat(GeometryBuffers[NonUniformResourceIndex(g.source.x)].Load2(offset+48));
    v.Tangent=asfloat(GeometryBuffers[NonUniformResourceIndex(g.source.x)].Load4(offset+56));
    v.Material=GeometryBuffers[NonUniformResourceIndex(g.source.x)].Load(offset+72)&65535;
    return v;
}
HsSceneVertexContext HsSceneGeometryVertexContext(SceneGeometryRecord g,uint geometry)
{
    HsSceneVertexContext c=(HsSceneVertexContext)0;
    c.instance=SceneDrawInstances[geometry];
    c.skinSlot=g.identity.z;
    c.cachedSkin=(g.material.y&2)!=0;
    if(c.cachedSkin)
    {
        c.skinVertexOffsets=HsLoadGpuSceneSkin(g.identity.x).zw;
        c.skinVertexStrides=HsLoadGpuSceneSkinStrides(g.identity.x);
    }
    c.cachedSkinHistory=false;
    c.geometryValid=true;
    c.geometry.Flags=(g.material.y&16)!=0?16u:0u;
    c.geometry.PageSlot=g.material.x<HsAuthoredMaterialCount?g.material.x*HsEnvironmentGeometryLodCount:0u;
    c.instanceKey=g.identity.xy;
    return c;
}
SceneOutput HsSceneGeometryVertex(SceneGeometryRecord g,uint geometry,uint vertex)
{
    HsSceneVertexContext context=HsSceneGeometryVertexContext(g,geometry);
    return EvaluateSceneVertex(HsSceneGeometryCanonicalVertex(g,vertex),context,vertex,false);
}
float3 HsSceneGeometryWorldToLocal(InstanceData instance,float3 world)
{
    float s,c; sincos(instance.Yaw,s,c);
    float3 p=world-instance.Position.xyz;
    return float3(c*p.x-s*p.z,p.y,s*p.x+c*p.z)/instance.Scale.xyz;
}

void HsSceneGeometryStorePositions(SceneGeometryRecord g,uint vertex,InstanceData instance,float3 world)
{
    ScenePositionOutput.Store3(g.range.w+vertex*12u,asuint(HsSceneGeometryWorldToLocal(instance,world)));
}

#endif
