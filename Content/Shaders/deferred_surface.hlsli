#ifndef HS_DEFERRED_SURFACE_HLSLI
#define HS_DEFERRED_SURFACE_HLSLI

// Include after FrameConstants, compact surface resources, and the generated
// deferred evaluators. This reconstructs the authored closure without sampling
// author textures again. All consumers receive the same resolved decal Surface;
// its emission is appended after authored sidecars, exactly once.

Texture2D<float4> HsDecalSurfaceEmission : register(t13, space36);
HsSurfaceClosure HsApplyDecalSurfaceEmission(HsSurfaceClosure closure,uint2 activePixel)
{
    // Empty frames retain capacity but do not write or read the export images.
    if(VfxDecalCount.x>0u)
        closure.emission+=HsDecalSurfaceEmission.Load(int3(activePixel,0)).rgb;
    return closure;
}

// Compatibility material paths retain their artist-authored ambient diffuse
// baseline; callers retain their separate direct lighting, rim/emission and
// unlit terms.
float3 HsDeferredIndirectDiffuse(float3 radiance)
{
    return radiance;
}

// This auxiliary is exactly the environment-specular term included in opaque
// lighting. SSR subtracts it once before composing its replacement.
float4 HsDeferredEnvironmentSpecular(float3 ambient_specular)
{
    return float4(ambient_specular,1.0);
}

#ifdef HS_GENERATED_DEFERRED_MATERIALS
HsSurfaceClosure HsReflectionDecodeClosure(uint2 metadata,float4 base,float2 packed_normal,float4 material,
    uint4 plane0,uint4 plane1,uint2 activePixel)
{
    HsSurfaceClosure closure = HsMaterialMakeSurfaceClosure(base.rgb,
        HsDecodeSurfaceNormal(packed_normal),
        material.r, material.g, material.b, 1.0);
    closure.normal_is_world = true;
    HsMaterialContext ctx = (HsMaterialContext)0;
    uint varying_mask = 0u;
    const uint environment = metadata.x - 0x10000000u;
    const uint hero = metadata.x - HsHeroMaterialIdBase;
    if (environment < HsAuthoredMaterialCount)
    {
        // The diagnostic grid terrain is produced without authored layers.
        if (!((LodClock.w & 1u) != 0u && environment == 14u))
        {
            ctx.parameter_base = MaterialParameterBases[environment / 4u][environment % 4u];
            closure = HsMaterialApplyDeferredLayers(closure, ctx, environment);
            varying_mask = MaterialVaryingMasks[environment / 4u][environment % 4u];
        }
    }
    else if (HsHeroOpaqueOrdinalValid(hero))
    {
        ctx.parameter_base = HeroMaterialParameterBases[hero / 4u][hero % 4u];
        closure = HsHeroApplyDeferredLayers(closure, ctx, hero);
        varying_mask = HeroMaterialVaryingMasks[hero / 4u][hero % 4u];
    }
    if (varying_mask != 0u)
    {
        closure = HsApplyMaterialSidecars(closure, varying_mask,
            MaterialSidecarOffsets[0], MaterialSidecarOffsets[1], plane0, plane1);
    }
    return HsApplyDecalSurfaceEmission(closure,activePixel);
}

HsSurfaceClosure HsReflectionLoadClosure(uint2 activePixel)
{
    uint4 plane0=0u,plane1=0u;
    if(MaterialSidecarInfo.z>0u) plane0=SurfaceSidecar0.Load(int3(activePixel,0));
    if(MaterialSidecarInfo.z>1u) plane1=SurfaceSidecar1.Load(int3(activePixel,0));
    return HsReflectionDecodeClosure(HsUnpackSurfaceMetadata(GBufferMetadata.Load(int3(activePixel,0))),
        GBufferBase.Load(int3(activePixel,0)),GBufferNormal.Load(int3(activePixel,0)),
        GBufferMaterial.Load(int3(activePixel,0)),plane0,plane1,activePixel);
}
#endif

#endif
