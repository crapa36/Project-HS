#include "project_hs.hlsl"
#define HS_RAY_OPACITY 1
#include "ray_hit.hlsli"
#if HS_WITH_NRD
#include "NRDConfig.hlsli"
#include "NRD.hlsli"
#endif
cbuffer RayShadowConstants : register(b0,space28)
{
    row_major float4x4 RsInverseViewProjection,RsWorldToView,RsProjection;
    float4 RsRenderSize,RsTraceSize,RsPreviousTraceSize,RsPreviousDepthParameters;
    float4 RsJitterUv,RsWorldOrigin,RsLightPositionRange,RsLightDirectionCos,RsEmitter;
    float4 RsAreaTangentHalfWidth,RsAreaBitangentHalfHeight;
    uint4 RsIdentity,RsFrame,RsControl,RsBudget;
};
RaytracingAccelerationStructure RsScene : register(t0,space20);
Texture2D<float> RsDepth : register(t4,space28);
Texture2D<float2> RsNormal : register(t5,space28);
Texture2D<float2> RsMotion : register(t6,space28);
Texture2D<uint2> RsMaterial : register(t7,space28);
Texture2D<float2> RsSurfaceHistory : register(t8,space28);
Texture2DArray<float4> RsRaw : register(t9,space28);
Texture2DArray<float4> RsPrevious : register(t10,space28);
Texture2DArray<uint4> RsPreviousValidation : register(t11,space28);
Texture2D<float> RsNativeClean : register(t12,space28);
Texture2DArray<float4> RsTemporal : register(t13,space28);
Texture2DArray<float> RsBlueNoise : register(t14,space28);
RWTexture2DArray<float4> RsRawOutput : register(u0,space28);
RWTexture2D<float2> RsMotionOutput : register(u1,space28);
RWTexture2D<float4> RsNormalOutput : register(u2,space28);
RWTexture2D<float> RsViewZOutput : register(u3,space28);
RWTexture2D<float> RsPenumbraOutput : register(u4,space28);
RWTexture2DArray<float4> RsHistoryOutput : register(u5,space28);
RWTexture2DArray<uint4> RsValidationOutput : register(u6,space28);
RWTexture2DArray<float> RsVisibilityOutput : register(u7,space28);
RWTexture2DArray<float> RsConfidenceOutput : register(u8,space28);

struct RsGuide
{
    int2 pixel;float3 world,normal;float view_z;uint2 material;bool valid;
};
RsGuide RsLoadGuide(int2 pixel)
{
    RsGuide g=(RsGuide)0;g.pixel=clamp(pixel,int2(0,0),int2(RsRenderSize.xy)-1);
    const float depth=RsDepth.Load(int3(g.pixel,0));g.material=RsMaterial.Load(int3(g.pixel,0));
    g.valid=depth>0.0&&g.material.x!=0u;
    if(g.valid)
    {
        const float2 uv=(float2(g.pixel)+0.5)*RsRenderSize.zw;
        const float4 h=mul(float4(uv*float2(2,-2)+float2(-1,1),depth,1),RsInverseViewProjection);
        const float3 relative=h.xyz/h.w;g.world=relative+RsWorldOrigin.xyz;
        g.view_z=mul(float4(relative,0),RsWorldToView).z;
        g.normal=HsDecodeSurfaceNormal(RsNormal.Load(int3(g.pixel,0)));
        g.valid=all(isfinite(g.world))&&isfinite(g.view_z)&&g.view_z>0.0;
    }
    return g;
}
RsGuide RsTraceGuide(uint2 p)
{
    const float2 raster=((float2(p)+0.5)*RsTraceSize.zw+RsJitterUv.xy)*RsRenderSize.xy;
    const bool half=any(RsRenderSize.xy>RsTraceSize.xy);
    const int2 first=int2(floor(raster-0.5));RsGuide result=RsLoadGuide(half?first:int2(round(raster-0.5)));
    if(half)[unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x)
    {
        const RsGuide candidate=RsLoadGuide(first+int2(x,y));
        if(candidate.valid&&(!result.valid||candidate.view_z<result.view_z))result=candidate;
    }
    return result;
}
uint RsPackNormal(float3 normal)
{
    const uint2 q=(uint2)round(saturate(HsEncodeSurfaceNormal(normal)*0.5+0.5)*65535.0);
    return q.x|(q.y<<16);
}
float3 RsUnpackNormal(uint packed)
{return HsDecodeSurfaceNormal(float2(packed&65535u,packed>>16)/65535.0*2.0-1.0);}
float2 RsSample(uint2 p,uint sample)
{
    // Reuse the authored resident STBN; SIGMA's recommendation is a bounded
    // 4-8-frame pattern. Two shifted scalar dimensions sample the emitter cone.
    const uint frame=RsFrame.x&7u;const uint2 shift=uint2(RsIdentity.x,RsIdentity.y^RsIdentity.z)&63u;
    const uint2 pixel=(p+shift)&63u;
    const float x=RsBlueNoise.Load(int4(pixel,frame,0));
    const float y=RsBlueNoise.Load(int4((pixel+uint2(29,17))&63u,(frame+3u)&7u,0));
    return min(frac(float2(x,y)+float(sample)*float2(0.61803398875,0.41421356237)),0.999999);
}
bool RsLightRay(RsGuide g,float2 sample,out float3 direction,out float distance,out float center_distance,out float sample_weight)
{
    const float3 origin=g.world+g.normal*RsEmitter.y;
    const float3 toward=RsLightPositionRange.xyz-origin;
    center_distance=length(toward);direction=0.0;distance=0.0;sample_weight=1.0;
    if(RsIdentity.w==3u||RsIdentity.w==4u)
    {
        if(center_distance<=RsEmitter.y||center_distance>RsLightPositionRange.w)return false;
        float2 coordinate=sample*2.0-1.0;
        if(RsIdentity.w==4u)
        {
            float radius=sqrt(sample.x),angle=6.28318530718*sample.y;
            coordinate=radius*float2(cos(angle),sin(angle));
        }
        float3 endpoint=RsLightPositionRange.xyz+
            RsAreaTangentHalfWidth.xyz*(coordinate.x*RsAreaTangentHalfWidth.w)+
            RsAreaBitangentHalfHeight.xyz*(coordinate.y*RsAreaBitangentHalfHeight.w);
        float3 delta=endpoint-origin;distance=length(delta);
        if(distance<=RsEmitter.y*2.0)return false;
        direction=delta/distance;
        float emitter_cosine=dot(RsLightDirectionCos.xyz,-direction);
        if(RsLightDirectionCos.w>0.5)emitter_cosine=abs(emitter_cosine);
        sample_weight=max(emitter_cosine,0.0)*max(dot(g.normal,direction),0.0)/(distance*distance);
        return sample_weight>0.0;
    }
    const float radius=RsEmitter.x;
    if(center_distance<=max(radius+RsEmitter.y,1e-5)||center_distance>RsLightPositionRange.w)return false;
    const float3 axis=toward/center_distance;
    if(RsIdentity.w==2u&&dot(-axis,RsLightDirectionCos.xyz)<RsLightDirectionCos.w)return false;
    const float3 tangent=normalize(abs(axis.z)<0.999?cross(float3(0,0,1),axis):cross(float3(0,1,0),axis));
    const float3 bitangent=cross(axis,tangent);
    const float cos_max=sqrt(max(0.0,1.0-radius*radius/(center_distance*center_distance)));
    const float cosine=lerp(1.0,cos_max,sample.x);const float sine=sqrt(max(0.0,1.0-cosine*cosine));
    const float angle=6.28318530718*sample.y;
    direction=normalize(axis*cosine+(tangent*cos(angle)+bitangent*sin(angle))*sine);
    const float projection=dot(toward,direction);
    distance=radius>0.0?projection-sqrt(max(0.0,projection*projection-dot(toward,toward)+radius*radius)):center_distance;
    return distance>RsEmitter.y*2.0;
}
float RsVisibility(RsGuide g,uint2 pixel,out float confidence,out float packed_penumbra)
{
    confidence=0.0;packed_penumbra=0.0;if(!g.valid||g.view_z>=RsEmitter.w)return 1.0;
    float visible=0.0,confidence_sum=0.0,certified_weight=0.0;
    [loop]for(uint sample=0u;sample<RsBudget.x;++sample)
    {
        float3 direction;float distance,center_distance,sample_weight;
        if(!RsLightRay(g,RsSample(pixel,sample),direction,distance,center_distance,sample_weight))continue;
        // Official SIGMA convention: NoL<=0 has distance0. Those pixels retain
        // raster confidence rather than suppressing thin-sheet back lighting.
        if(dot(g.normal,direction)<=0.0)continue;
        RayDesc ray;ray.Origin=g.world+g.normal*RsEmitter.y;ray.Direction=direction;
        ray.TMin=RsEmitter.y*0.25;ray.TMax=min(distance-RsEmitter.y,asfloat(RsBudget.w));
        const float footprint=g.view_z*2.0*max(RsRenderSize.z/max(abs(RsProjection[0][0]),1e-6),
            RsRenderSize.w/max(abs(RsProjection[1][1]),1e-6));
        float unsupported=ray.TMax+1.0;
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES|RAY_FLAG_FORCE_NON_OPAQUE> query;
        // Penumbra requires the CLOSEST accepted occluder, never first-hit termination.
        query.TraceRayInline(RsScene,RAY_FLAG_NONE,255u,ray);
        [loop]while(query.Proceed())
        {
            if(query.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE)continue;
            const uint instance_id=query.CandidateInstanceID();
            if(instance_id>=RsControl.z){unsupported=min(unsupported,query.CandidateTriangleRayT());continue;}
            const RayInstanceMetadata metadata=RayInstances[instance_id];
            if(metadata.identity.w>=RsControl.w){unsupported=min(unsupported,query.CandidateTriangleRayT());continue;}
            const RayHitGeometry geometry=RayGeometries[metadata.identity.w];
            if(any(geometry.identity.xy!=metadata.identity.xy)||any(geometry.mesh!=metadata.mesh))
            {unsupported=min(unsupported,query.CandidateTriangleRayT());continue;}
            HsRayHitContext hit;
            if(!HsRayLoadHitContext(instance_id,query.CandidatePrimitiveIndex(),query.CandidateTriangleBarycentrics(),
                query.CandidateTriangleFrontFace(),direction,footprint+query.CandidateTriangleRayT()*RsEmitter.z,
                query.CandidateTriangleRayT(),hit))continue;
            if((geometry.material.z&32u)!=0u||(HsHeroAssetIndex(hit.vertex.Mesh)!=HsHeroInvalidOrdinal&&
                !HsHeroOpaqueOrdinalValid(HsHeroOrdinal(HsHeroOpaqueMaterialId(hit.vertex.Mesh,hit.vertex.Material)))))
            {unsupported=min(unsupported,query.CandidateTriangleRayT());continue;}
            if(HsRayCovered(hit))query.CommitNonOpaqueTriangleHit();
        }
        const bool occluded=query.CommittedStatus()==COMMITTED_TRIANGLE_HIT;
        const float hit=occluded?query.CommittedRayT():65504.0;
        const float sample_confidence=unsupported<(occluded?hit:ray.TMax)?0.0:1.0;
        visible+=(occluded?0.0:1.0)*sample_confidence*sample_weight;
        certified_weight+=sample_confidence*sample_weight;confidence_sum+=sample_confidence;
#if HS_WITH_NRD
        if(RsFrame.z!=0u)packed_penumbra=SIGMA_FrontEnd_PackPenumbra(hit,center_distance,2.0*RsEmitter.x);
#endif
    }
    confidence=confidence_sum/float(RsBudget.x);
    // The visibility factor is conditional on certified front-facing emitter
    // samples. Skipped/unsupported samples lower confidence and retain raster;
    // they must not dim an otherwise unoccluded finite emitter.
    return certified_weight>0.0?visible/certified_weight:1.0;
}
[numthreads(8,8,1)]
void RayShadowTraceCS(uint3 thread:SV_DispatchThreadID)
{
    if(any(thread.xy>=uint2(RsTraceSize.xy)))return;
    const RsGuide g=RsTraceGuide(thread.xy);float confidence,penumbra;
    const float visibility=RsVisibility(g,thread.xy,confidence,penumbra);
    RsRawOutput[uint3(thread.xy,RsControl.y)]=float4(visibility,confidence,g.valid?g.view_z:0.0,0.0);
#if HS_WITH_NRD
    if(RsFrame.z!=0u)
    {
        const float2 guide_uv=(float2(g.pixel)+0.5)*RsRenderSize.zw-RsJitterUv.xy;
        const float2 signal_uv=(float2(thread.xy)+0.5)*RsTraceSize.zw;
        RsMotionOutput[thread.xy]=g.valid?guide_uv+RsMotion.Load(int3(g.pixel,0))-signal_uv:0.0;
        RsNormalOutput[thread.xy]=NRD_FrontEnd_PackNormalAndRoughness(g.valid?g.normal:float3(0,0,1),1.0,0.0);
        RsViewZOutput[thread.xy]=g.valid?g.view_z:RsEmitter.w*2.0;
        RsPenumbraOutput[thread.xy]=penumbra;
    }
#endif
}
float RsPreviousDepth(float depth)
{
    const float4 p=RsPreviousDepthParameters;const float denominator=depth*p.y-p.x;
    return abs(denominator)>1e-10?(p.z-depth*p.w)/denominator:0.0;
}
[numthreads(8,8,1)]
void RayShadowTemporalCS(uint3 thread:SV_DispatchThreadID)
{
    if(any(thread.xy>=uint2(RsTraceSize.xy)))return;
    const uint3 p=uint3(thread.xy,RsControl.y);const RsGuide g=RsTraceGuide(thread.xy);
    float4 value=RsRaw.Load(int4(p,0));
    const float2 surface=RsSurfaceHistory.Load(int3(g.pixel,0));
    if(RsControl.x!=0u&&g.valid&&value.y>0.0&&surface.y>0.0)
    {
        const float2 motion=RsMotion.Load(int3(g.pixel,0));
        const float2 previous_uv=(float2(g.pixel)+0.5)*RsRenderSize.zw-RsJitterUv.xy+motion;
        const float expected=RsPreviousDepth(surface.x);
        if(all(isfinite(motion))&&all(previous_uv>=0.0)&&all(previous_uv<1.0)&&expected>0.0)
        {
            const int2 previous_pixel=int2(floor(previous_uv*RsPreviousTraceSize.xy));
            const uint4 validation=RsPreviousValidation.Load(int4(previous_pixel,RsControl.y,0));
            const float4 old=RsPrevious.Load(int4(previous_pixel,RsControl.y,0));
            if(all(validation.xy==g.material)&&abs(asfloat(validation.z)-expected)<=max(0.01,expected*0.015)&&
                dot(RsUnpackNormal(validation.w),g.normal)>=0.9&&old.y>0.0)
            {
                float lo=value.x,hi=value.x;
                [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x)
                {
                    const int2 q=clamp(int2(thread.xy)+int2(x,y),int2(0,0),int2(RsTraceSize.xy)-1);
                    const RsGuide neighbour=RsTraceGuide(uint2(q));const float4 raw=RsRaw.Load(int4(q,RsControl.y,0));
                    if(!neighbour.valid||any(neighbour.material!=g.material)||dot(neighbour.normal,g.normal)<0.9||
                        abs(neighbour.view_z-g.view_z)>max(0.01,g.view_z*0.015)||raw.y<=0.0)continue;
                    lo=min(lo,raw.x);hi=max(hi,raw.x);
                }
                const float weight=min(0.9,float(RsBudget.z)/(float(RsBudget.z)+1.0))*saturate(surface.y*old.y);
                value.x=lerp(value.x,clamp(old.x,lo,hi),weight);
            }
        }
    }
    RsHistoryOutput[p]=value;
    RsValidationOutput[p]=g.valid?uint4(g.material,asuint(g.view_z),RsPackNormal(g.normal)):0u;
}
[numthreads(8,8,1)]
void RayShadowResolveCS(uint3 thread:SV_DispatchThreadID)
{
    if(any(thread.xy>=uint2(RsRenderSize.xy)))return;
    const RsGuide g=RsLoadGuide(int2(thread.xy));float visibility=1.0,confidence=0.0;
    if(g.valid&&g.view_z<RsEmitter.w)
    {
        const float2 sample_pixel=((float2(thread.xy)+0.5)*RsRenderSize.zw-RsJitterUv.xy)*RsTraceSize.xy-0.5;
        const int2 center=int2(floor(sample_pixel+0.5));float sum=0.0,total=0.0,confidence_sum=0.0;
        [loop]for(int y=-int(RsBudget.y);y<=int(RsBudget.y);++y)
        [loop]for(int x=-int(RsBudget.y);x<=int(RsBudget.y);++x)
        {
            const int2 p=center+int2(x,y);if(any(p<0)||any(p>=int2(RsTraceSize.xy)))continue;
            const RsGuide source=RsTraceGuide(uint2(p));
            if(!source.valid||any(source.material!=g.material)||source.view_z>=RsEmitter.w||dot(source.normal,g.normal)<0.9)continue;
            const float4 raw=RsRaw.Load(int4(p,RsControl.y,0));float value;
#if HS_WITH_NRD
            if(RsFrame.z!=0u)value=saturate(SIGMA_BackEnd_UnpackShadow(RsNativeClean.Load(int3(p,0))));
            else
#endif
                value=RsTemporal.Load(int4(p,RsControl.y,0)).x;
            if(raw.y<=0.0)continue;
            const float2 delta=float2(p)-sample_pixel;
            const float weight=exp2(-0.75*dot(delta,delta))*exp2(-abs(source.view_z-g.view_z)/max(0.01,g.view_z*0.01))*
                pow(saturate(dot(source.normal,g.normal)),16.0);
            sum+=value*weight;total+=weight;confidence_sum+=raw.y*weight;
        }
        if(total>1e-5){visibility=saturate(sum/total);confidence=saturate(confidence_sum/total);}
    }
    const uint3 output=uint3(thread.xy,RsControl.y);RsVisibilityOutput[output]=visibility;RsConfidenceOutput[output]=confidence;
}
