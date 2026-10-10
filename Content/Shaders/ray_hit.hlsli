#ifndef HS_RAY_HIT_HLSLI
#define HS_RAY_HIT_HLSLI
// Included after project_hs: exact raster vertex/context and authored material
// evaluators remain shared. DXR InstanceID indexes the32B scene metadata.
struct RayHitGeometry
{
    uint4 source; // vertex descriptor, index descriptor, first index, index count
    uint4 range; // source byte offset, stride, vertex count, deformed position offset
    uint4 identity; // persistent scene slot, generation, skin slot, mesh ID
    uint4 material; // environment ordinal, parameter word, flags, geometry generation
    uint4 mesh; // exact TLAS metadata mesh key and generation, both64-bit
};
struct RayInstanceMetadata { uint4 identity; uint4 mesh; };
StructuredBuffer<RayInstanceMetadata> RayInstances : register(t1,space20);
StructuredBuffer<RayHitGeometry> RayGeometries : register(t2,space20);
StructuredBuffer<RayHitGeometry> RayDeformJobs : register(t3,space20);
ByteAddressBuffer RayPositions : register(t4,space20);
StructuredBuffer<InstanceData> RayDrawInstances : register(t14,space20);
RWByteAddressBuffer RayPositionOutput : register(u0,space20);

SceneInput HsRayCanonicalVertex(RayHitGeometry g,uint vertex)
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
HsSceneVertexContext HsRayVertexContext(RayHitGeometry g,uint geometry)
{
    HsSceneVertexContext c=(HsSceneVertexContext)0;
    c.instance=RayDrawInstances[geometry];
    c.skinSlot=g.identity.z;
    c.cachedSkin=(g.material.z&2)!=0;
    c.cachedSkinHistory=false;
    c.geometryValid=true;
    c.geometry.Flags=(g.material.z&16)!=0?16u:0u;
    c.geometry.PageSlot=g.material.x<15?g.material.x*3:0;
    c.instanceKey=g.identity.xy;
    return c;
}
SceneOutput HsRayVertex(RayHitGeometry g,uint geometry,uint vertex)
{
    return EvaluateSceneVertex(HsRayCanonicalVertex(g,vertex),HsRayVertexContext(g,geometry),vertex);
}
float3 HsRayWorldToLocal(InstanceData instance,float3 world)
{
    float s,c; sincos(instance.Yaw,s,c);
    float3 p=world-instance.Position.xyz;
    return float3(c*p.x-s*p.z,p.y,s*p.x+c*p.z)/instance.Scale.xyz;
}

#ifdef HS_RAY_OPACITY
#include "ray_materials.generated.hlsli"
// Ray-cone projected footprint with a triangle Jacobian. This supplies explicit
// gradients to the same authored SampleGrad evaluation as raster; ray lanes do
// not have coherent screen-space ddx/ddy. Degenerate UVs remain finite.
HsSceneGradients HsRayGradients(SceneOutput a,SceneOutput b,SceneOutput c,
    float3 rayDirection,float footprint)
{
    HsSceneGradients g=(HsSceneGradients)0;
    float3 e1=b.WorldPosition-a.WorldPosition,e2=c.WorldPosition-a.WorldPosition;
    float3 n=cross(e1,e2); float area2=dot(n,n);
    if(area2<1e-20) return g;
    n*=rsqrt(area2);
    float3 t=normalize(abs(rayDirection.z)<0.999?cross(float3(0,0,1),rayDirection):cross(float3(0,1,0),rayDirection));
    float3 bt=cross(rayDirection,t);
    float incidence=dot(n,rayDirection);
    incidence=abs(incidence)>1e-6?incidence:(incidence<0?-1e-6:1e-6);
    // Project the circular cone cross-section onto the hit plane. Only its
    // grazing axis stretches; retaining two gradients preserves anisotropy.
    g.worldDx=(t-rayDirection*(dot(n,t)/incidence))*footprint;
    g.worldDy=(bt-rayDirection*(dot(n,bt)/incidence))*footprint;
    float aa=dot(e1,e1),ab=dot(e1,e2),bb=dot(e2,e2);
    float inv=1.0/max(aa*bb-ab*ab,1e-20);
    float2 dx=float2(dot(g.worldDx,e1)*bb-dot(g.worldDx,e2)*ab,
                     dot(g.worldDx,e2)*aa-dot(g.worldDx,e1)*ab)*inv;
    float2 dy=float2(dot(g.worldDy,e1)*bb-dot(g.worldDy,e2)*ab,
                     dot(g.worldDy,e2)*aa-dot(g.worldDy,e1)*ab)*inv;
    g.uvDx=(b.Uv-a.Uv)*dx.x+(c.Uv-a.Uv)*dx.y;
    g.uvDy=(b.Uv-a.Uv)*dy.x+(c.Uv-a.Uv)*dy.y;
    g.stableDx=(b.EnvironmentStableWorld-a.EnvironmentStableWorld)*dx.x+
               (c.EnvironmentStableWorld-a.EnvironmentStableWorld)*dx.y;
    g.stableDy=(b.EnvironmentStableWorld-a.EnvironmentStableWorld)*dy.x+
               (c.EnvironmentStableWorld-a.EnvironmentStableWorld)*dy.y;
    return g;
}
struct HsRayHitContext
{
    RayHitGeometry geometry;
    uint geometryIndex;
    SceneOutput vertex;
    HsSceneGradients gradients;
    float3 geometricNormal;
    float3 outgoing;
    float distance;
    bool front;
};
bool HsRayLoadHitContext(uint instanceId,uint primitive,float2 bary,bool front,
    float3 direction,float footprint,float distance,out HsRayHitContext hit)
{
    hit=(HsRayHitContext)0;
    uint instances,stride; RayInstances.GetDimensions(instances,stride);
    if(instanceId>=instances) return false;
    RayInstanceMetadata instance=RayInstances[instanceId];
    uint geometry=instance.identity.w;
    uint geometries; RayGeometries.GetDimensions(geometries,stride);
    if(geometry>=geometries) return false;
    RayHitGeometry g=RayGeometries[geometry];
    if(any(g.identity.xy!=instance.identity.xy)||any(g.mesh!=instance.mesh)) return false;
    if(!front&&(g.material.z&4)==0) return false;
    if(primitive>=g.source.w/3) return false;
    uint3 indices=GeometryBuffers[NonUniformResourceIndex(g.source.y)].Load3((g.source.z+primitive*3)*4);
    if(any(indices>=g.range.z)) return false;
    SceneOutput a=HsRayVertex(g,geometry,indices.x);
    SceneOutput b=HsRayVertex(g,geometry,indices.y);
    SceneOutput c=HsRayVertex(g,geometry,indices.z);
    SceneOutput h=a;
    float3 w=float3(1-bary.x-bary.y,bary);
    h.WorldPosition=a.WorldPosition*w.x+b.WorldPosition*w.y+c.WorldPosition*w.z;
    h.EnvironmentStableWorld=a.EnvironmentStableWorld*w.x+b.EnvironmentStableWorld*w.y+c.EnvironmentStableWorld*w.z;
    h.WorldNormal=normalize(a.WorldNormal*w.x+b.WorldNormal*w.y+c.WorldNormal*w.z);
    h.WorldTangent=float4(normalize(a.WorldTangent.xyz*w.x+b.WorldTangent.xyz*w.y+c.WorldTangent.xyz*w.z),a.WorldTangent.w);
    h.Uv=a.Uv*w.x+b.Uv*w.y+c.Uv*w.z;
    HsSceneGradients gradients=HsRayGradients(a,b,c,direction,footprint);
    hit.geometry=g; hit.geometryIndex=geometry; hit.vertex=h;
    hit.gradients=gradients; hit.front=front;
    hit.outgoing=-normalize(direction);hit.distance=distance;
    float3 geometric=cross(b.WorldPosition-a.WorldPosition,c.WorldPosition-a.WorldPosition);
    if(dot(geometric,geometric)<1e-20) return false;
    hit.geometricNormal=normalize(geometric)*(front?1.0:-1.0);
    return true;
}
bool HsRayLoadHitContext(uint instanceId,uint primitive,float2 bary,bool front,
    float3 direction,float footprint,out HsRayHitContext hit)
{return HsRayLoadHitContext(instanceId,primitive,bary,front,direction,footprint,0.0,hit);}
bool HsRayCovered(HsRayHitContext hit)
{
    RayHitGeometry g=hit.geometry;
    if(g.identity.w==6u) return false; // projectile shells have no opaque raster/AO coverage
    if((g.material.z&32)!=0) return false;
    SceneOutput h=hit.vertex;
    HsSceneGradients gradients=hit.gradients;
    if((LodClock.w&1u)!=0u&&(h.Mesh==9u||h.Mesh==20u)) return true;
    if(g.material.x<15)
    {
        HsMaterialContext ctx=EnvironmentMaterialContext(h,hit.front,gradients,g.material.y);
        ctx.view_direction=hit.outgoing;ctx.eye_distance=hit.distance;
        return (g.material.y&1u)==0u||HsRayEnvironmentEvaluateOpacity(ctx,g.material.x)>=HsMaterialOpacityCutoff;
    }
    if(HsHeroAssetIndex(h.Mesh)!=HsHeroInvalidOrdinal)
    {
        // Canonical vertex material is the authored FBX binding slot. Its hero
        // ordinal can differ inside one instance/BLAS; never use instance ID.
        uint ordinal=HsHeroOrdinal(HsHeroOpaqueMaterialId(h.Mesh,h.Material));
        if(!HsHeroOpaqueOrdinalValid(ordinal)) return false;
        h.MaterialId=HsHeroOpaqueMaterialId(h.Mesh,h.Material);
        HsMaterialContext ctx=HsHeroMaterialContext(h,hit.front,gradients,ordinal);
        ctx.view_direction=hit.outgoing;ctx.eye_distance=hit.distance;
        return HsHeroEvaluateOpacity(ctx,ordinal)>=HsMaterialOpacityCutoff;
    }
    return true; // canonical opaque placeholder domain has no authored alpha graph
}
bool HsRayCovered(uint instanceId,uint primitive,float2 bary,bool front,
    float3 direction,float footprint)
{
    HsRayHitContext hit;
    return HsRayLoadHitContext(instanceId,primitive,bary,front,direction,footprint,hit)&&HsRayCovered(hit);
}
#ifdef HS_RAY_RADIANCE
HsSurfaceClosure HsRayEvaluateSurface(HsRayHitContext hit)
{
    SceneOutput h=hit.vertex;
    RayHitGeometry g=hit.geometry;
    if(g.material.x<15u && !((LodClock.w&1u)!=0u&&(h.Mesh==9u||h.Mesh==20u)))
    {
        HsMaterialContext ctx=EnvironmentMaterialContext(h,hit.front,hit.gradients,g.material.y);
        ctx.view_direction=hit.outgoing;ctx.eye_distance=hit.distance;
        return HsHeroClosureToWorld(HsRayEnvironmentEvaluateSurface(ctx,g.material.x),ctx);
    }
    if(HsHeroAssetIndex(h.Mesh)!=HsHeroInvalidOrdinal)
    {
        uint ordinal=HsHeroOrdinal(HsHeroOpaqueMaterialId(h.Mesh,h.Material));
        h.MaterialId=HsHeroOpaqueMaterialId(h.Mesh,h.Material);
        HsMaterialContext ctx=HsHeroMaterialContext(h,hit.front,hit.gradients,ordinal);
        ctx.view_direction=hit.outgoing;ctx.eye_distance=hit.distance;
        HsSurfaceClosure surface=HsHeroClosureToWorld(HsHeroEvaluateSurface(ctx,ordinal),ctx);
        // Ray identity stores a zero-based slot; raster entity keys reserve zero.
        // Static ray slots carry the high bit and have no dynamic surface payload.
        if((g.identity.x&0x80000000u)==0u)
            surface.emission+=HsEntitySurface(h.Mesh,uint2(g.identity.x+1u,g.identity.y),h.WorldPosition).rgb;
        return surface;
    }
    // Diagnostic terrain and canonical runtime primitives retain their raster
    // material evaluation, including procedural grid gradients and tint.
    GBufferOutput sample=HsEvaluateSceneSurface(h,hit.front,false,hit.gradients,g.material.y);
    HsSurfaceClosure surface=HsMaterialMakeSurfaceClosure(sample.BaseColor.rgb,
        HsDecodeSurfaceNormal(sample.Normal),sample.Material.r,sample.Material.g,sample.Material.b,1.0);
    surface.normal_is_world=true;
    return surface;
}
#endif
#endif
#endif
