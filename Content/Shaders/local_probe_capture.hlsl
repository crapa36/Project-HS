// Static transport capture shares the authored forward closure and opacity
// context. Common IBL explicitly selects the global baseline under this define.
#define HS_LOCAL_PROBE_CAPTURE 1
#include "project_hs.hlsl"

SceneOutput LocalProbeCaptureVS(SceneInput input,uint instance_id : SV_InstanceID,uint vertex_id : SV_VertexID)
{
    HsSceneVertexContext context=(HsSceneVertexContext)0;
    context.skinSlot=0xffffffffu; context.rasterRecordId=0xffffffffu;
    context.instanceKey=uint2(instance_id+1u,SkinCacheOptions.w);
    uint4 h0,h1,h2,h3;
    const bool valid=HsGpuSceneSlotValid(instance_id,h0,h1,h2,h3);
    // A capture draw is the exact source slot, not a main-view visible-ref index.
    const uint generation=valid ? HsGpuSceneTables.Load(h1.x+instance_id*h2.z+20u) : 0u;
    context.instance=HsLoadGpuSceneInstance(instance_id);
    context.draw.InstanceIndex=instance_id; context.draw.InstanceGeneration=generation;
    context.instance.Scale.w=1.0;
    SceneOutput output=EvaluateSceneVertex(input,context,vertex_id);
    output.MaterialId=0x10000000u+PassValue;
    output.EnvironmentFade=1.0; output.EnvironmentFlags&=~0x20000u;
    output.HistoryConfidence=0.0; output.ShadowClipDistance=1.0;
    if(!valid || generation!=SkinCacheOptions.w)
        output.Position=float4(0,0,-1,1); // Invalid generation has no coverage.
    return output;
}

struct LocalProbeCaptureOutput
{
    float4 Radiance : SV_Target0;
    float FirstHitDistance : SV_Target1;
};
float HsCaptureStaticShadowVisibility(float3 world)
{
    if(ShadowAtlasTexelSize.x<=0.0) return 1.0; // No shadow-requested directional owner.
    // This full-scene map is rendered from the exact retained static slots and
    // complete pages. Its camera and residency do not depend on the main view.
    const float4 clip=mul(float4(world,1),ShadowViewProjection[0]);
    const float3 projected=clip.xyz/clip.w;
    const float2 uv=projected.xy*float2(0.5,-0.5)+0.5;
    if(any(uv<0.0) || any(uv>1.0) || projected.z<0.0 || projected.z>1.0) return 1.0;
    const float2 lower=ShadowAtlasTexelSize.xy*0.5;
    const float2 upper=1.0-lower;
    float visibility=0.0;
    const uint samples=uint(ShadowAtlasTexelSize.w);
    [loop]for(uint sample_index=0;sample_index<samples;++sample_index)
        visibility+=ShadowMap.SampleCmpLevelZero(ShadowCompare,
            clamp(uv+float2(HsShadowFilterOffsets[sample_index])*ShadowAtlasTexelSize.xy,lower,upper),projected.z);
    return visibility/float(samples);
}
void CaptureStaticShadowPS(SceneOutput input,bool front : SV_IsFrontFace)
{
    const bool opaque=input.Mesh!=17u && input.Mesh!=19u;
    if(!HsSceneCovered(input,front,opaque,RasterSceneGradients(input),SceneDrawMaterial)) discard;
}
LocalProbeCaptureOutput HsCaptureStaticSurface(SceneOutput input,bool front,bool opaque)
{
    const float distance=length(input.WorldPosition-CameraTime.xyz);
    // The authored miss/far distance is radial, while a cube face's projection
    // clips along its axis. Hits beyond the radial bound remain global sky.
    if(distance>TemporalOptions.z) discard;
    const HsSceneGradients gradients=RasterSceneGradients(input);
    if(!HsSceneCovered(input,front,opaque,gradients,SceneDrawMaterial)) discard;
    const HsMaterialContext context=EnvironmentMaterialContext(input,front,gradients,SceneDrawMaterial);
    HsSurfaceClosure surface=HsMaterialEvaluateSurface(context);
    if(!surface.normal_is_world)
    {
        const float3 n=context.shading_normal;
        const float3 t=normalize(context.tangent.xyz-n*dot(n,context.tangent.xyz));
        const float3 b=normalize(cross(n,t))*context.tangent.w;
        surface.normal=normalize(t*surface.normal.x+b*surface.normal.y+n*surface.normal.z);
        surface.tangent=t*surface.tangent.x+b*surface.tangent.y+n*surface.tangent.z;
    }
    surface.normal=normalize(surface.normal); surface.normal_is_world=true;
    float3 radiance=surface.base_color;
    if(!surface.unlit)
    {
        float3 diffuse,specular,ambient_specular,ambient_diffuse,local_diffuse,local_specular;
        EnvironmentLightingComponents(surface,input.WorldPosition,HsCaptureStaticShadowVisibility(input.WorldPosition),-1.0,
            diffuse,specular,ambient_specular,ambient_diffuse);
        // Capture is independent of the main camera grid; scan the same actual
        // authoritative light records once for all local light shapes/shadows.
        HsLightListEvaluateComponentsOptical(surface,input.WorldPosition,CameraTime.xyz,1.0,
            HsLightLocalMask,true,-1.0,local_diffuse,local_specular);
        radiance=diffuse+specular+local_diffuse+local_specular;
    }
    LocalProbeCaptureOutput output;
    output.Radiance=float4(max(radiance+surface.emission,0.0),1.0);
    output.FirstHitDistance=distance;
    return output;
}
LocalProbeCaptureOutput LocalProbeOpaqueCapturePS(SceneOutput input,bool front : SV_IsFrontFace)
{ return HsCaptureStaticSurface(input,front,true); }
LocalProbeCaptureOutput LocalProbeMaskedCapturePS(SceneOutput input,bool front : SV_IsFrontFace)
{ return HsCaptureStaticSurface(input,front,false); }
LocalProbeCaptureOutput LocalProbeSkyCapturePS(FullScreenOutput input)
{
    const float4 projected=mul(float4(input.Uv*float2(2,-2)+float2(-1,1),1,1),InverseUnjitteredViewProjection);
    const float3 near_world=projected.xyz/projected.w;
    LocalProbeCaptureOutput output;
    output.Radiance=float4(EnvironmentSkyRadiance(normalize(near_world-CameraTime.xyz)),1);
    output.FirstHitDistance=TemporalOptions.z;
    return output;
}
