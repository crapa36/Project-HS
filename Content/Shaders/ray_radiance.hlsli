#ifndef HS_RAY_RADIANCE_HLSLI
#define HS_RAY_RADIANCE_HLSLI
#include "ray_native_trace.hlsli"
#include "ray_transport.hlsli"

bool RtCandidateIdentity(uint instanceId,out RayHitGeometry geometry)
{
    geometry=(RayHitGeometry)0;
    if(instanceId>=RtCounts.x)return false;
    RayInstanceMetadata instance=RayInstances[instanceId];
    if(instance.identity.w>=RtCounts.y)return false;
    geometry=RayGeometries[instance.identity.w];
    return all(geometry.identity.xy==instance.identity.xy)&&all(geometry.mesh==instance.mesh);
}
bool RtUnsupported(HsRayHitContext hit)
{
    if((hit.geometry.material.z&32u)!=0u)return true;
    if(HsHeroAssetIndex(hit.vertex.Mesh)!=HsHeroInvalidOrdinal)
        return !HsHeroOpaqueOrdinalValid(HsHeroOrdinal(HsHeroOpaqueMaterialId(hit.vertex.Mesh,hit.vertex.Material)));
    return false;
}
float3 RtBiasedOrigin(float3 world,float3 geometricNormal,float3 direction,float bias)
{
    return world+geometricNormal*(dot(geometricNormal,direction)>=0.0?bias:-bias)+direction*bias;
}
float RtShadowVisibility(float3 world,float3 normal,float3 direction,float distance,float footprint,
    float bias,out float confidence)
{
    confidence=1.0;
    if(RtCounts.x==0u)return 1.0;
    RayDesc ray;ray.Origin=RtBiasedOrigin(world,normal,direction,bias);ray.Direction=direction;
    ray.TMin=bias;ray.TMax=max(bias,distance-2.0*bias);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES|RAY_FLAG_FORCE_NON_OPAQUE|RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH HS_RAY_QUERY_OMM_FLAGS> query;
    query.TraceRayInline(RtScene,RAY_FLAG_NONE,255u,ray);
    [loop]while(query.Proceed())
    {
        if(query.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE)continue;
        RayHitGeometry geometry;
        if(!RtCandidateIdentity(query.CandidateInstanceID(),geometry)){confidence=0.0;continue;}
        HsRayHitContext hit;
        if(!HsRayLoadHitContext(query.CandidateInstanceID(),query.CandidatePrimitiveIndex(),
            query.CandidateTriangleBarycentrics(),query.CandidateTriangleFrontFace(),direction,
            footprint,query.CandidateTriangleRayT(),hit))continue;
        if(RtUnsupported(hit)){confidence=0.0;continue;}
        if(HsRayCovered(hit))
            query.CommitNonOpaqueTriangleHit();
    }
    if(query.CommittedStatus()==COMMITTED_TRIANGLE_HIT){confidence=1.0;return 0.0;}
    return 1.0;
}
float3 RtHitDirectLighting(HsSurfaceClosure closure,HsRayHitContext hit,float3 view,
    uint2 pixel,uint sample,uint lightSamples,float bias,float footprint,out float confidence)
{
    confidence=1.0;
    HsLightGrid grid=HsLoadLightGrid();uint count=grid.limits.x;
    if(count==0u||closure.unlit)return 0.0;
    float3 result=0.0;
    // Select from every authoritative light record, independent of camera cells.
    // Uniform selection has an explicit count compensation; no light is dropped
    // by a quality budget, and all authored emitter domains retain their PDF.
    uint samples=max(lightSamples,1u);
    [loop]for(uint i=0u;i<samples;++i)
    {
        uint index=min((uint)(RtRandom(pixel,sample*31u+i,11u)*float(count)),count-1u);
        HsGpuLightRecord light=HsLightRecords[index];float3 direction,radiance;
        float distance=RtSpecularParameters.x,pdf=1.0;
        if(light.identity.w>=3u)
        {
            float range;
            if(!HsAreaLightVisible(light,hit.vertex.WorldPosition,range))continue;
            float2 u=float2(RtRandom(pixel,sample*31u+i,12u),RtRandom(pixel,sample*31u+i,13u));
            float3 light_position=HsAreaPoint(light,u);
            if(!HsAreaSampleDirection(light,hit.vertex.WorldPosition,u,direction,pdf))continue;
            distance=length(light_position-hit.vertex.WorldPosition);
            radiance=light.color_intensity.rgb*light.color_intensity.w*range;
        }
        else
        {
            if(!HsLightIncident(index,hit.vertex.WorldPosition,direction,radiance))continue;
            distance=light.identity.w==0u?max(RtGiParameters.x,RtSpecularParameters.x):
                length(light.position_range.xyz-hit.vertex.WorldPosition);
        }
        float shadowConfidence=1.0;
        float visibility=1.0;
        if((light.metadata.z&1u)!=0u)
            visibility=RtShadowVisibility(hit.vertex.WorldPosition,hit.geometricNormal,direction,
                distance,footprint,bias,shadowConfidence);
        confidence=min(confidence,shadowConfidence);
        float3 diffuse,specular;HsBsdfEvaluateComponents(closure,view,direction,diffuse,specular);
        result+=(diffuse+specular)*radiance*(abs(dot(closure.normal,direction))*visibility*float(count)/(pdf*float(samples)));
    }
    return result;
}
float3 RtTraceRadiance(float3 world,float3 normal,float3 direction,float maximumDistance,
    float bias,float coneSpread,uint2 pixel,uint sample,uint lightSamples,
    out float hitDistance,out float confidence,out bool secondaryHit)
{
    hitDistance=maximumDistance;confidence=1.0;secondaryHit=false;
    if(RtCounts.x==0u)return EnvironmentSkyRadiance(direction);
    RayDesc ray;ray.Origin=RtBiasedOrigin(world,normal,direction,bias);ray.Direction=direction;
    ray.TMin=bias;ray.TMax=maximumDistance;
    float unsupportedDistance=maximumDistance+1.0;
    // Radiance requires the closest covered hit. First-hit termination is only
    // valid for the separate binary shadow query above.
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES|RAY_FLAG_FORCE_NON_OPAQUE HS_RAY_QUERY_OMM_FLAGS> query;
    query.TraceRayInline(RtScene,RAY_FLAG_NONE,255u,ray);
    [loop]while(query.Proceed())
    {
        if(query.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE)continue;
        RayHitGeometry geometry;
        if(!RtCandidateIdentity(query.CandidateInstanceID(),geometry))
        {unsupportedDistance=min(unsupportedDistance,query.CandidateTriangleRayT());continue;}
        float footprint=max(bias,query.CandidateTriangleRayT()*coneSpread);
        HsRayHitContext hit;
        if(!HsRayLoadHitContext(query.CandidateInstanceID(),query.CandidatePrimitiveIndex(),
            query.CandidateTriangleBarycentrics(),query.CandidateTriangleFrontFace(),direction,footprint,query.CandidateTriangleRayT(),hit))continue;
        if(RtUnsupported(hit)){unsupportedDistance=min(unsupportedDistance,query.CandidateTriangleRayT());continue;}
        if(HsRayCovered(hit))
            query.CommitNonOpaqueTriangleHit();
    }
    float closest=query.CommittedStatus()==COMMITTED_TRIANGLE_HIT?query.CommittedRayT():maximumDistance;
    // An unsupported transparent interface behind the closest opaque hit does
    // not invalidate that path. A nearer interface explicitly rejects it.
    if(unsupportedDistance<closest){confidence=0.0;hitDistance=unsupportedDistance;return 0.0;}
    if(query.CommittedStatus()!=COMMITTED_TRIANGLE_HIT)return EnvironmentSkyRadiance(direction);
    secondaryHit=true;
    hitDistance=query.CommittedRayT();HsRayHitContext hit;
    if(!HsRayLoadHitContext(query.CommittedInstanceID(),query.CommittedPrimitiveIndex(),
        query.CommittedTriangleBarycentrics(),query.CommittedTriangleFrontFace(),direction,
        max(bias,hitDistance*coneSpread),hitDistance,hit)){confidence=0.0;return 0.0;}
    HS_RAY_REORDER(query)
    HsSurfaceClosure closure=HsRayEvaluateSurface(hit);
    float3 view=-direction;closure=HsBsdfOrientThinSurface(closure,view);
    if(closure.unlit)return max(closure.base_color+closure.emission,0.0);
    float directConfidence;
    float3 direct=RtHitDirectLighting(closure,hit,view,pixel,sample,lightSamples,bias,
        max(bias,hitDistance*coneSpread),directConfidence);
    confidence=min(confidence,directConfidence);
    float3 probeDiffuse,probeSpecular;
    // This terminates the secondary path in the authored probe approximation.
    // It does not consume primary-view GI/reflection or an undeclared history.
    EnvironmentIndirectLightingComponents(closure,hit.vertex.WorldPosition,view,-1.0,true,true,probeDiffuse,probeSpecular);
    float3 radiance=direct+probeDiffuse+probeSpecular+closure.emission;
    if(!all(isfinite(radiance))){confidence=0.0;return 0.0;}
    return max(radiance,0.0);
}
#endif
