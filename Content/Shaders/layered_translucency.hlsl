#include "project_hs.hlsl"

cbuffer LayeredTranslucencyConstants : register(b0, space14)
{
    uint4 LayeredTranslucencyInfo; // current layer, layer budget, phase, reserved.
};
Texture2D<float> LayeredTranslucencyFrontDepth[4] : HS_TRANSLUCENCY_SRV_REGISTER(HS_TRANSLUCENCY_SRV_FRONT_DEPTH);
Texture2D<uint2> LayeredTranslucencyFrontIdentity[4] : HS_TRANSLUCENCY_SRV_REGISTER(HS_TRANSLUCENCY_SRV_FRONT_IDENTITY);
Texture2D<uint> LayeredTranslucencyFrontOpticalComponent[4] : HS_TRANSLUCENCY_SRV_REGISTER(HS_TRANSLUCENCY_SRV_FRONT_OPTICAL_COMPONENT);
Texture2D<float4> LayeredTranslucencyExitDepth : HS_TRANSLUCENCY_SRV_REGISTER(HS_TRANSLUCENCY_SRV_EXIT);
Texture2D<float4> LayeredTranslucencyBackground : HS_TRANSLUCENCY_SRV_REGISTER(HS_TRANSLUCENCY_SRV_BACKGROUND);

float3 HsLayeredTranslucencyBackground(float3 world, float3 direction, float optical_path,
    uint2 source_pixel, float3 background)
{
    // Rejected displaced lookups retain the valid source behind this interface.
    const float3 exit_world=world+direction*optical_path;
    const float4 exit_clip=mul(float4(exit_world,1.0),ViewProjection);
    if(exit_clip.w<=0.0) return background;
    const float2 uv=HsSurfaceClipUv(exit_clip);
    if(any(uv<0.0) || any(uv>=1.0)) return background;
    const uint2 sample_pixel=uint2(uv*ScreenSize.xy);
    // Both accepted and rejected lookups return the already-loaded color at this pixel.
    if(all(sample_pixel==source_pixel)) return background;
    float scene_depth=SurfaceDepth.Load(int3(sample_pixel,0));
    const uint next=LayeredTranslucencyInfo.x+1u;
    if(next<LayeredTranslucencyInfo.y)
    {
        const float next_depth=LayeredTranslucencyFrontDepth[next].Load(int3(sample_pixel,0));
        if(next_depth>scene_depth) scene_depth=next_depth;
    }
    // The far-to-near source has coverage from the next front or the opaque
    // surface. A displaced lookup may never pull a foreground silhouette.
    if(scene_depth>exit_clip.z/exit_clip.w) return background;
    if(scene_depth>0.0)
    {
        const float3 scene_world=HsReconstructSurfaceRelative(
            HsSurfaceRasterUv(int2(sample_pixel)),scene_depth)+SurfaceViewOrigin.xyz;
        if(dot(scene_world-exit_world,direction)<0.0) return background;
    }
    return LayeredTranslucencyBackground.Load(int3(sample_pixel,0)).rgb;
}

// Authored-thickness weighted OIT source path. This has one opaque scene guide;
// measured front/back thickness and bounded multi-interface depth peeling are
// separate transport paths, not implied by this screen-space lookup.
float3 HsTranslucencyLighting(HsSurfaceClosure surface, float3 world,
    float2 pixel, float3 view, float3 normal, float3 transmitted,
    float measured_path, bool layer_transport, float3 layer_background, float3 world_dx, float3 world_dy)
{
#ifdef HS_CAPTURE_TRANSLUCENCY_DIAGNOSTICS
    if(IndirectArguments.Load(28u)==2u) { HsTranslucencyDiagnosticsAdd(27u); return 0.0; }
#endif
    surface.normal=normal;
    const float solid=saturate(surface.solid_transmission_weight);
    const bool total_internal=solid>0.0 && dot(transmitted,transmitted)==0.0;
    const float base_fresnel_override=total_internal ? 1.0 : -1.0;
    const HsBsdfDirectView prepared=HsBsdfPrepareDirectView(surface,view,normal);
    float3 diffuse,specular,ambient_specular,ambient_diffuse;
    EnvironmentLightingComponents(solid<1.0,surface,world,view,HsLightAllMask,
        ShadowVisibility(world,world_dx,world_dy),base_fresnel_override,prepared,diffuse,specular,ambient_specular,ambient_diffuse);
    if(solid==0.0) return HsFogSurface(diffuse+specular+surface.emission,world,pixel);
    const float nv=prepared.no_v;
    const float fresnel=total_internal ? 1.0 :
        HsBsdfSchlickFresnel(prepared.solid_f0.xxx,nv).x;
    if(total_internal)
    {
        // The actual solid interface has unit Fresnel under TIR. The shared
        // evaluator already applied it to the authored solid fraction, with
        // original GGX/anisotropy/layers/proposal/PDF and one ambient sample loop.
        return HsFogSurface(diffuse*(1.0-solid)+specular+surface.emission,world,pixel);
    }
    const float optical_path=measured_path>=0.0 ? measured_path :
        surface.thickness_m/max(abs(dot(transmitted,normal)),1e-4);
    float3 background=EnvironmentSkyRadiance(transmitted);
    float2 background_uv=pixel*ScreenSize.zw;
    float background_view_depth=VmDepthRange.x;
    const bool fog_enabled=HsFogEnabled();
    if(layer_transport) background=layer_background;
    else
    {
        const int3 source_pixel=int3(int2(pixel),0);
        const float source_depth=SurfaceDepth.Load(source_pixel);
        if(source_depth>0.0)
        {
            const float3 source_world=HsReconstructSurfaceRelative(
                HsSurfaceRasterUv(int2(pixel)),source_depth)+SurfaceViewOrigin.xyz;
            background=fog_enabled ? VmRawOpaqueHdr.Load(source_pixel).rgb : HdrColor.Load(source_pixel).rgb;
            background_view_depth=HsFogViewDepth(source_world);
        }
    }
    const float3 exit_world=world+transmitted*optical_path;
    const float4 exit_clip=mul(float4(exit_world,1.0),ViewProjection);
    if(layer_transport && MaterialQualityOptions.w>0.0)
        background=HsLayeredTranslucencyBackground(world,transmitted,optical_path,uint2(pixel),layer_background);
    else if(!layer_transport && MaterialQualityOptions.w>0.0 && exit_clip.w>0.0)
    {
        const float2 uv=HsSurfaceClipUv(exit_clip);
        if(all(uv>=0.0) && all(uv<1.0))
        {
            const uint2 sample_pixel=uint2(uv*ScreenSize.xy);
            const float depth=SurfaceDepth.Load(int3(sample_pixel,0));
            // Reverse-Z depth must lie behind the authored exit plane. Reject
            // foreground silhouettes instead of refracting through them.
            if(depth>0.0 && depth<=exit_clip.z/exit_clip.w)
            {
                const float3 opaque_world=HsReconstructSurfaceRelative(
                    HsSurfaceRasterUv(int2(sample_pixel)),depth)+SurfaceViewOrigin.xyz;
                if(dot(opaque_world-exit_world,transmitted)>=0.0)
                {
                    background_uv=uv;
                    if(fog_enabled)
                    {
                        background=VmRawOpaqueHdr.Load(int3(sample_pixel,0)).rgb;
                        background_view_depth=HsFogViewDepth(opaque_world);
                    }
                    else background=HdrColor.Load(int3(sample_pixel,0)).rgb;
                }
            }
        }
    }
    const float coat=saturate(surface.coat_weight);
    const float coat_transmission=prepared.coat_entry*
        (1.0-coat*HsBsdfCoatFresnel(abs(dot(transmitted,normal))));
    // The base closure supplies the authored interface/transmission tint.
    // Bulk Beer absorption remains the separately authored inverse-metre RGB.
    const float3 transmission_weight=saturate(surface.base_color)*HsBsdfSolidBeerLambert(surface,optical_path)*
        (solid*(1.0-fresnel)*coat_transmission);
    const float3 raw_surface=diffuse*(1.0-solid)+specular+surface.emission;
    if(!fog_enabled) return raw_surface+transmission_weight*background;
    const HsVolumetricTransport prefix=HsFogPrefix(world,pixel);
    if(layer_transport)
    {
        // B is already camera-domain radiance, including its own fog. This
        // bounded screen-refraction model uses the current camera prefix;
        // subtract that prefix once, with no transmittance division.
        return prefix.radiance+prefix.transmittance*raw_surface+
            transmission_weight*(background-prefix.radiance);
    }
    // Behind the interface, integrate only the front-to-background interval.
    // A missing opaque exit uses the global sky at the fog far distance.
    const HsVolumetricTransport interval=HsSampleVolumetricInterval(background_uv,
        HsFogViewDepth(world),background_view_depth);
    const float3 incoming=interval.radiance+interval.transmittance*background;
    return prefix.radiance+prefix.transmittance*(raw_surface+transmission_weight*incoming);
}

float3 HsTranslucencyLighting(HsSurfaceClosure surface, float3 world, float2 pixel, bool front,
    float3 world_dx, float3 world_dy)
{
    const float3 view=normalize(CameraTime.xyz-world);
    float3 normal=normalize(surface.normal);
    if(dot(normal,view)<0.0) normal=-normal;
    const float eta=front ? 1.0/surface.ior : surface.ior;
    const float3 transmitted=refract(-view,normal,eta);
    return HsTranslucencyLighting(surface,world,pixel,view,normal,transmitted,-1.0,false,0.0,world_dx,world_dy);
}

HsSurfaceClosure HsWaterSurfaceClosure(float authored_thickness)
{
    const float thickness=max(0.001,authored_thickness);
    HsSurfaceClosure water=HsMaterialMakeSurfaceClosure(float3(.97,.985,.96),float3(0,1,0),.18,0,1,1);
    water.solid_transmission_weight=1; water.ior=1.333;
    // Suspended soil removes blue first; clear-water absorption made shallow mud look icy.
    water.absorption_per_m=float3(3.0,8.0,16.0);water.thickness_m=thickness;
    water.normal_is_world=true;return water;
}

HsSurfaceClosure HsTranslucencyTransportSurface(SceneOutput input, bool front, HsSceneGradients gradients)
{
    HS_TRANSLUCENCY_DIAGNOSTICS_COUNT(18u);
    if(input.MaterialBindings.y == HS_MATERIAL_PROGRAM_WATER)
    {
        HS_TRANSLUCENCY_DIAGNOSTICS_COUNT(14u);
        return HsWaterSurfaceClosure(input.Uv.x);
    }
    const uint ordinal=input.MaterialBindings.y;
    if(!HsCharacterTranslucentOrdinalValid(ordinal)) { HS_TRANSLUCENCY_DIAGNOSTICS_COUNT(11u); discard; }
    // discard does not let DXC narrow the later dynamic material switch.
    const HsMaterialContext ctx=HsCharacterMaterialContext(input,front,gradients,ordinal,
        HsCharacterSurfaceContextDemand(ordinal) | HsContextShadingNormal | HsContextTangent);
    HS_TRANSLUCENCY_DIAGNOSTICS_COUNT(13u);
    const HsSurfaceClosure surface=HsCharacterClosureToWorld(HsCharacterEvaluateSurface(ctx,ordinal),ctx);
#ifdef HS_CAPTURE_TRANSLUCENCY_DIAGNOSTICS
    if(surface.opacity-0.001<0.0) HsTranslucencyDiagnosticsAdd(12u);
#endif
    clip(surface.opacity-0.001);
    return surface;
}

void HsTranslucencyClipOpacity(SceneOutput input, bool front, HsSceneGradients gradients)
{
    if(input.MaterialBindings.y == HS_MATERIAL_PROGRAM_WATER) return;
    const uint ordinal=input.MaterialBindings.y;
    if(!HsCharacterTranslucentOrdinalValid(ordinal)) { HS_TRANSLUCENCY_DIAGNOSTICS_COUNT(11u); discard; }
    const HsMaterialContext ctx=HsCharacterMaterialContext(input,front,gradients,ordinal,
        HsCharacterOpacityContextDemand(ordinal));
    const float opacity=HsCharacterEvaluateOpacity(ctx,ordinal);
#ifdef HS_CAPTURE_TRANSLUCENCY_DIAGNOSTICS
    if(opacity-0.001<0.0) HsTranslucencyDiagnosticsAdd(12u);
#endif
    clip(opacity-0.001);
}

float4 HsWeightedTranslucencyAccumulation(float3 color, float alpha, float3 world)
{
    const float z = saturate(length(world - CameraTime.xyz) / 180.0);
    const float weight = clamp(pow(min(1.0, alpha * 10.0) + 0.01, 3.0) *
        1e8 * pow(1.0 - z * 0.9, 3.0), 1e-2, 3e3);
    return float4(color * alpha * weight, alpha * weight);
}

OitOutput CharacterTranslucentPS(SceneOutput input, bool front : SV_IsFrontFace)
{
    const HsSceneGradients gradients = RasterSceneGradients(input);
    if(!front && input.OpticalComponentId!=0u) discard;
    HsSurfaceClosure surface = HsTranslucencyTransportSurface(input, front, gradients);
    const float3 color = HsTranslucencyLighting(surface, input.WorldPosition, input.Position.xy, front, gradients.worldDx, gradients.worldDy);
    const float alpha = saturate(surface.opacity);
    OitOutput output;
    output.Accumulation = HsWeightedTranslucencyAccumulation(color, alpha, input.WorldPosition);
    output.Revealage = alpha.xxxx;
    return output;
}

bool HsLayeredTranslucencySelectedFront(SceneOutput input,uint layer)
{
    const int3 pixel=int3(int2(input.Position.xy),0);
    return input.Position.z==LayeredTranslucencyFrontDepth[layer].Load(pixel) &&
        all(input.InstanceKey==LayeredTranslucencyFrontIdentity[layer].Load(pixel)) &&
        input.OpticalComponentId==LayeredTranslucencyFrontOpticalComponent[layer].Load(pixel);
}

struct LayeredTranslucencyPeelFrontOutput
{
    uint2 InstanceKey : SV_Target0;
    uint OpticalComponentId : SV_Target1;
};

LayeredTranslucencyPeelFrontOutput LayeredTranslucencyPeelFrontPS(SceneOutput input HS_TRANSLUCENCY_DIAGNOSTICS_COVERAGE)
{
    const HsSceneGradients gradients=RasterSceneGradients(input);
    HS_TRANSLUCENCY_DIAGNOSTICS_ENTRY(input.Position.xy);
    if(input.InstanceKey.x==0u) { HS_TRANSLUCENCY_DIAGNOSTICS_COUNT(4u); discard; }
    const int3 pixel=int3(int2(input.Position.xy),0);
    const float opaque_depth=SurfaceDepth.Load(pixel);
    if(input.Position.z<=opaque_depth) { HS_TRANSLUCENCY_DIAGNOSTICS_COUNT(5u); discard; }
    if(LayeredTranslucencyInfo.x>0u)
    {
        const float previous=LayeredTranslucencyFrontDepth[LayeredTranslucencyInfo.x-1u].Load(pixel);
        if(previous<=0.0 || input.Position.z>=previous) { HS_TRANSLUCENCY_DIAGNOSTICS_COUNT(6u); discard; }
    }
    HsTranslucencyClipOpacity(input,true,gradients);
    HS_TRANSLUCENCY_DIAGNOSTICS_ACCEPT(input.Position.xy);
    LayeredTranslucencyPeelFrontOutput output;
    output.InstanceKey=input.InstanceKey;
    output.OpticalComponentId=input.OpticalComponentId;
    return output;
}

float4 LayeredTranslucencyGatherExitDepthPS(SceneOutput input HS_TRANSLUCENCY_DIAGNOSTICS_COVERAGE):SV_Target0
{
    const HsSceneGradients gradients=RasterSceneGradients(input);
    HS_TRANSLUCENCY_DIAGNOSTICS_ENTRY(input.Position.xy);
    if(input.OpticalComponentId==0u) { discard; return 0.0; }
    const int3 pixel=int3(int2(input.Position.xy),0);
    float4 exits=0.0;
    [unroll] for(uint layer=0u;layer<4u;++layer)
    {
        if(layer>=LayeredTranslucencyInfo.y) break;
        const float entry=LayeredTranslucencyFrontDepth[layer].Load(pixel);
        if(entry>0.0 && input.Position.z<entry &&
            all(input.InstanceKey==LayeredTranslucencyFrontIdentity[layer].Load(pixel)) &&
            input.OpticalComponentId==LayeredTranslucencyFrontOpticalComponent[layer].Load(pixel))
            exits[layer]=input.Position.z;
    }
    if(!any(exits>0.0)) discard;
    HsTranslucencyClipOpacity(input,false,gradients);
    HS_TRANSLUCENCY_DIAGNOSTICS_ACCEPT(input.Position.xy);
    return exits;
}

[earlydepthstencil]
OitOutput LayeredTranslucencyAccumulateUnpeeledPS(SceneOutput input HS_TRANSLUCENCY_DIAGNOSTICS_COVERAGE)
{
    const HsSceneGradients gradients=RasterSceneGradients(input);
    HS_TRANSLUCENCY_DIAGNOSTICS_ENTRY(input.Position.xy);
    // The read-only opaque DSV rejects z <= opaque before this shader runs.
    if(input.InstanceKey.x==0u) { HS_TRANSLUCENCY_DIAGNOSTICS_COUNT(4u); discard; }
    [loop] for(uint layer=0u;layer<LayeredTranslucencyInfo.y;++layer)
        if(HsLayeredTranslucencySelectedFront(input,layer)) { HS_TRANSLUCENCY_DIAGNOSTICS_COUNT(10u); discard; }
    const HsSurfaceClosure surface=HsTranslucencyTransportSurface(input,true,gradients);
    HS_TRANSLUCENCY_DIAGNOSTICS_ACCEPT(input.Position.xy);
    const float3 color=HsTranslucencyLighting(surface,input.WorldPosition,input.Position.xy,true,gradients.worldDx,gradients.worldDy);
    const float alpha=saturate(surface.opacity);
    OitOutput output;
    output.Accumulation=HsWeightedTranslucencyAccumulation(color,alpha,input.WorldPosition);
    output.Revealage=float4(0.0,1.0,0.0,alpha);
    return output;
}

struct LayeredTranslucencyFrontGuideOutput
{
    float2 Motion;
    float2 History;
    float4 Classification;
    float2 Transport;
};

HsSurfaceClosure HsLayeredTranslucencySelectedSurface(SceneOutput input,uint hsHeroCoverage,
    HsSceneGradients gradients)
{
    HS_TRANSLUCENCY_DIAGNOSTICS_ENTRY(input.Position.xy);
    const uint layer=LayeredTranslucencyInfo.x;
    // A cleared front has zero identity and cannot authorize a composite.
    // Match the front peel's reserved-key rejection before its depth match.
    if(input.InstanceKey.x==0u) discard;
#ifdef HS_CAPTURE_TRANSLUCENCY_DIAGNOSTICS
    if(input.Position.z!=LayeredTranslucencyFrontDepth[layer].Load(int3(int2(input.Position.xy),0)))
    { HsTranslucencyDiagnosticsAdd(8u); discard; }
    if(any(input.InstanceKey!=LayeredTranslucencyFrontIdentity[layer].Load(int3(int2(input.Position.xy),0))))
    { HsTranslucencyDiagnosticsAdd(9u); discard; }
    if(input.OpticalComponentId!=LayeredTranslucencyFrontOpticalComponent[layer].Load(int3(int2(input.Position.xy),0)))
    { HsTranslucencyDiagnosticsAdd(9u); discard; }
#else
    // Composite/guide PSOs use early depth equality against this same
    // read-only front DSV. Keep the identity match for coincident instances.
    if(any(input.InstanceKey!=LayeredTranslucencyFrontIdentity[layer].Load(int3(int2(input.Position.xy),0))) ||
        input.OpticalComponentId!=LayeredTranslucencyFrontOpticalComponent[layer].Load(int3(int2(input.Position.xy),0))) discard;
#endif
    const HsSurfaceClosure surface=HsTranslucencyTransportSurface(input,true,gradients);
    HS_TRANSLUCENCY_DIAGNOSTICS_ACCEPT(input.Position.xy);
    return surface;
}

LayeredTranslucencyFrontGuideOutput HsTranslucencySurfaceGuides(SceneOutput input,HsSurfaceClosure surface,bool unpeeled_coverage)
{
    LayeredTranslucencyFrontGuideOutput output=(LayeredTranslucencyFrontGuideOutput)0;
    output.Classification=float4(HsEncodeSurfaceNormal(surface.normal),saturate(surface.opacity),surface.roughness);
    output.Transport=float2(surface.solid_transmission_weight>0.0?1.0:0.0,any(surface.emission>0.0)?1.0:0.0);
    const float2 current_uv=input.Position.xy*ScreenSize.zw-SurfaceJitterUv.xy;
    const float4 previous=input.PreviousUnjitteredClipPosition;
    const bool valid=input.HistoryConfidence>0.0 && SurfaceViewOrigin.w>0.5 &&
        input.Position.w>0.0 && previous.w>0.0 &&
        all(isfinite(input.Position)) && all(isfinite(current_uv)) && all(isfinite(previous)) &&
        !unpeeled_coverage;
    if(valid)
    {
        const float2 motion=HsSurfaceClipUv(previous)-current_uv;
        const float previous_depth=previous.z/previous.w;
        if(all(isfinite(motion))&&isfinite(previous_depth))
        {
            output.Motion=motion;
            output.History=float2(previous_depth,input.HistoryConfidence);
        }
    }
    return output;
}

float4 HsLayeredTranslucencyCompositeSurface(SceneOutput input,HsSurfaceClosure surface,
    HsSceneGradients gradients)
{
    const uint layer=LayeredTranslucencyInfo.x;
    const float3 to_eye=CameraTime.xyz-input.WorldPosition;
    const float3 view=normalize(to_eye);
    float3 normal=normalize(surface.normal);
    if(dot(normal,view)<0.0) normal=-normal;
    const float3 transmitted=refract(-view,normal,1.0/surface.ior);
    const int3 pixel=int3(int2(input.Position.xy),0);
    const float exit_depth=LayeredTranslucencyExitDepth.Load(pixel)[layer];
    float path=-1.0;
    if(exit_depth>0.0 && dot(transmitted,transmitted)>0.0)
    {
        const float2 exit_uv=HsSurfaceRasterUv(int2(input.Position.xy));
        const float exit_inverse_w=mul(float4(exit_uv*float2(2.0,-2.0)+float2(-1.0,1.0),exit_depth,1.0),
            InverseViewProjection).w;
        const float3 geometric_normal=normalize(input.WorldNormal);
        const float denominator=abs(dot(transmitted,geometric_normal));
        if(denominator>0.0)
            // Clip-W ratios give the ray distance; its camera length cancels the view cosine.
            path=abs((input.Position.w-exit_inverse_w)*dot(to_eye,geometric_normal))/(exit_inverse_w*denominator);
    }
    // The paired exit provides a measured local slab. Open geometry uses only
    // the authored metre-valued thickness; another instance can never supply it.
    const float3 background=LayeredTranslucencyBackground.Load(pixel).rgb;
    const float3 color=HsTranslucencyLighting(surface,input.WorldPosition,input.Position.xy,
        view,normal,transmitted,path,true,background,gradients.worldDx,gradients.worldDy);
    return float4(lerp(background,color,saturate(surface.opacity)),1.0);
}

struct LayeredTranslucencyCompositeGuideOutput
{
    float4 Color:SV_Target0;
    float2 Motion:SV_Target1;
    float2 History:SV_Target2;
    float4 Classification:SV_Target3;
    float2 Transport:SV_Target4;
};

// The existing guide product is the final layer0 composite. Reuse its original
// surface before transport lighting or view-facing normal orientation.
[earlydepthstencil]
LayeredTranslucencyCompositeGuideOutput LayeredTranslucencyCompositeFrontPS(SceneOutput input HS_TRANSLUCENCY_DIAGNOSTICS_COVERAGE)
{
    const HsSceneGradients gradients=RasterSceneGradients(input);
#ifdef HS_CAPTURE_TRANSLUCENCY_DIAGNOSTICS
    const HsSurfaceClosure surface=HsLayeredTranslucencySelectedSurface(input,hsHeroCoverage,gradients);
#else
    const HsSurfaceClosure surface=HsLayeredTranslucencySelectedSurface(input,0u,gradients);
#endif
    const LayeredTranslucencyFrontGuideOutput guides=HsTranslucencySurfaceGuides(input,surface,
        OitRevealage.Load(int3(int2(input.Position.xy),0)).g>0.0);
    LayeredTranslucencyCompositeGuideOutput output;
    output.Color=HsLayeredTranslucencyCompositeSurface(input,surface,gradients);
    output.Motion=guides.Motion;
    output.History=guides.History;
    output.Classification=guides.Classification;
    output.Transport=guides.Transport;
    return output;
}

[earlydepthstencil]
float4 LayeredTranslucencyCompositeLayerPS(SceneOutput input HS_TRANSLUCENCY_DIAGNOSTICS_COVERAGE):SV_Target0
{
    const HsSceneGradients gradients=RasterSceneGradients(input);
#ifdef HS_CAPTURE_TRANSLUCENCY_DIAGNOSTICS
    const HsSurfaceClosure surface=HsLayeredTranslucencySelectedSurface(input,hsHeroCoverage,gradients);
#else
    const HsSurfaceClosure surface=HsLayeredTranslucencySelectedSurface(input,0u,gradients);
#endif
    return HsLayeredTranslucencyCompositeSurface(input,surface,gradients);
}
