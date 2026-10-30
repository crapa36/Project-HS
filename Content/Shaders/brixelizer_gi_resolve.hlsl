#include "brixelizer_common.hlsli"
[numthreads(8,8,1)]
void BrixelizerGiResolveCS(uint3 dispatch : SV_DispatchThreadID)
{
    uint2 p=dispatch.xy;if(any(p>=uint2(BrixRenderSize.xy)))return;
    float3 fallback=BrixFallbackDiffuse.Load(int3(p,0)).rgb;
    float3 originalSss=BrixOriginalSubsurface.Load(int3(p,0)).rgb;
    float3 environment=BrixEnvironmentSpecular.Load(int3(p,0)).rgb;
    float3 diffuseFactor=0.0,specularFactor=0.0;
    bool supported=false;
    // SurfaceLayout thin-sheet covers masked leaves/grass. Hero catalog ordinal2
    // is authored archer_cutout (alpha_mode=mask), without coverage triangles.
    uint2 metadata=HsUnpackSurfaceMetadata(BrixMetadata.Load(int3(p,0)));
    bool domainSupported=(metadata.y&HsSurfaceFlagThinSheet)==0u&&HsHeroOrdinal(metadata.x)!=2u;
    if(BrixSurfaceValid(p)&&domainSupported)
    {HsSurfaceClosure c=BrixClosure(p);float3 relative=BrixRelativePosition(p);
        if(all(isfinite(relative)))supported=HsBrixelizerMaterialFactors(c,normalize(-relative),diffuseFactor,specularFactor);}
    float3 diffuse=supported?max(BrixRawDiffuse.Load(int3(p,0)).rgb,0.0)*diffuseFactor:fallback;
    float3 specular=supported?max(BrixRawSpecular.Load(int3(p,0)).rgb,0.0)*specularFactor:environment;
    supported=supported&&all(isfinite(diffuse))&&all(isfinite(specular));
    if(!supported){diffuse=fallback;specular=environment;}
    if(LightingOptions.w<=0.0)specular=environment;
    float3 delta=diffuse-fallback;
    BrixDiffuseOutput[p]=float4(diffuse,supported?1.0:0.0);
    BrixDeltaOutput[p]=float4(delta,supported?1.0:0.0);
    BrixSubsurfaceOutput[p]=float4(max(originalSss+delta,0.0),supported?1.0:0.0);
    BrixSpecularOutput[p]=float4(specular,supported?1.0:0.0);
}
