#ifndef HS_MATERIAL_SURFACE_HLSLI
#define HS_MATERIAL_SURFACE_HLSLI

struct HsMaterialContext
{
    float2 uv, d_uv_dx, d_uv_dy;
    uint layer;
    float3 actual_world_position, d_actual_world_dx, d_actual_world_dy;
    float3 stable_world_position, d_stable_world_dx, d_stable_world_dy;
    float3 normal, shading_normal;
    float4 tangent;
    uint mesh, seed;
    uint parameter_base;
    float3 view_direction;
    float eye_distance;
    float3 sampled_base_color;
    float3 sampled_emission;
    float sampled_roughness, sampled_alpha, roughness_scale, sampled_ao, sampled_metallic;
    float metallic;
};

struct HsSurfaceClosure
{
    float3 base_color;
    float3 normal;
    float roughness;
    float metallic;
    float ao;
    float opacity;
    bool normal_is_world;
    float coat_weight;
    float coat_roughness;
    float anisotropy;
    float3 tangent;
    float sheen_weight;
    float3 sheen_color;
    float sheen_roughness;
    float transmission_weight;
    float3 transmission_color;
    float3 emission;
    bool unlit;
    float sss_weight;
    float3 sss_radius_m;
    float thickness_m;
    float solid_transmission_weight;
    float ior;
    float3 absorption_per_m;
};

typedef HsSurfaceClosure HsMaterialSurface;

static const float HsMaterialOpacityCutoff = 0.5;

HsSurfaceClosure HsMaterialMakeSurfaceClosure(float3 base_color, float3 normal,
                                               float roughness, float metallic,
                                               float ao, float opacity)
{
    HsSurfaceClosure result = (HsSurfaceClosure)0;
    result.base_color = base_color;
    result.normal = normal;
    result.roughness = roughness;
    result.metallic = metallic;
    result.ao = ao;
    result.opacity = opacity;
    result.ior = 1.5;
    return result;
}

// Layers share the base normal. Tangent is in the same space as that normal;
// the lighting consumer resolves both to world space before BSDF evaluation.
HsSurfaceClosure HsMaterialApplyCoat(HsSurfaceClosure base, float weight, float roughness)
{
    base.coat_weight = saturate(weight);
    base.coat_roughness = saturate(roughness);
    return base;
}

HsSurfaceClosure HsMaterialApplyAnisotropy(HsSurfaceClosure base, float anisotropy, float3 tangent)
{
    base.anisotropy = clamp(anisotropy, -1.0, 1.0);
    base.tangent = tangent;
    return base;
}

HsSurfaceClosure HsMaterialApplySheen(HsSurfaceClosure base, float weight, float3 color, float roughness)
{
    base.sheen_weight = saturate(weight);
    base.sheen_color = saturate(color);
    base.sheen_roughness = saturate(roughness);
    return base;
}

HsSurfaceClosure HsMaterialApplyThinTransmission(HsSurfaceClosure base, float weight, float3 color)
{
    base.transmission_weight = saturate(weight);
    base.transmission_color = saturate(color);
    return base;
}

HsSurfaceClosure HsMaterialApplyEmission(HsSurfaceClosure base, float3 emission)
{
    base.emission += max(emission, 0.0);
    return base;
}

// Optical distances are authored in metres; absorption is inverse metres.
// The current profile validates uniform inputs and mutually exclusive thin,
// subsurface and solid transport. The consumer owns diffusion/refraction.
HsSurfaceClosure HsMaterialApplySubsurface(HsSurfaceClosure base, float weight,
                                          float3 radius_m, float thickness_m)
{
    base.sss_weight = saturate(weight);
    base.sss_radius_m = max(radius_m, 0.0);
    base.thickness_m = max(thickness_m, 0.0);
    return base;
}

HsSurfaceClosure HsMaterialApplySolidTransmission(HsSurfaceClosure base, float weight,
    float ior, float3 absorption_per_m, float thickness_m)
{
    base.solid_transmission_weight = saturate(weight);
    base.ior = max(ior, 1.0);
    base.absorption_per_m = max(absorption_per_m, 0.0);
    base.thickness_m = max(thickness_m, 0.0);
    return base;
}

HsSurfaceClosure HsMaterialMakeUnlit(float3 color, float opacity)
{
    HsSurfaceClosure result = HsMaterialMakeSurfaceClosure(
        max(color, 0.0), float3(0.0, 0.0, 1.0), 1.0, 0.0, 1.0, saturate(opacity));
    result.unlit = true;
    return result;
}

#endif
