#ifndef HS_BSDF_METALLIC_ROUGHNESS_HLSLI
#define HS_BSDF_METALLIC_ROUGHNESS_HLSLI

#include "material_surface.hlsli"

static const float HsBsdfPi = 3.14159265358979323846;
static const float HsBsdfInvPi = 0.31830988618379067154;

struct HsBsdfSampleResult
{
    float3 wi;
    float3 f;
    float pdf;
    bool valid;
};

float HsBsdfLuminance(float3 value)
{
    return dot(value, float3(0.2126, 0.7152, 0.0722));
}

float HsBsdfSolidDielectricF0(HsSurfaceClosure surface)
{
    float ratio = (surface.ior - 1.0) / (surface.ior + 1.0);
    return ratio * ratio;
}

float3 HsBsdfF0(HsSurfaceClosure surface)
{
    float metallic = saturate(surface.metallic);
    float dielectric = lerp(0.04, HsBsdfSolidDielectricF0(surface),
        saturate(surface.solid_transmission_weight));
    return lerp(dielectric.xxx, surface.base_color, metallic);
}

float HsBsdfMetallic(HsSurfaceClosure surface)
{
    return saturate(surface.metallic);
}

float HsBsdfAlpha(HsSurfaceClosure surface)
{
    return max(surface.roughness * surface.roughness, 0.002);
}

float HsBsdfSpecularProbability(HsSurfaceClosure surface)
{
    float metallic = HsBsdfMetallic(surface);
    float specular_luminance = HsBsdfLuminance(HsBsdfF0(surface));
    float diffuse_luminance = HsBsdfLuminance((1.0 - metallic) * surface.base_color);
    float total_luminance = specular_luminance + diffuse_luminance;
    return total_luminance > 0.0 ? specular_luminance / total_luminance : 1.0;
}

float HsBsdfSmithG1(float no_x, float alpha_squared)
{
    return 2.0 * no_x / max(no_x + sqrt(alpha_squared + (1.0 - alpha_squared) * no_x * no_x), 0.0001);
}

float HsBsdfD(float no_h, float alpha)
{
    float alpha_squared = alpha * alpha;
    float denominator = no_h * no_h * (alpha_squared - 1.0) + 1.0;
    return alpha_squared / (HsBsdfPi * denominator * denominator);
}

float3 HsBsdfSchlickFresnel(float3 f0, float vo_h)
{
    return f0 + (1.0 - f0) * pow(1.0 - vo_h, 5.0);
}

float3 HsBsdfOpticalReflectionFresnel(HsSurfaceClosure surface, float vo_h, float base_fresnel_override)
{
    const float3 authored = HsBsdfSchlickFresnel(HsBsdfF0(surface), vo_h);
    // An optical interface event changes the reflected solid fraction's Fresnel,
    // never the authored material. Keep its diffuse substrate separate: the
    // solid transport caller applies that substrate's (1-solid) budget once.
    return base_fresnel_override >= 0.0 ? lerp(authored, saturate(base_fresnel_override).xxx,
        saturate(surface.solid_transmission_weight)) : authored;
}

float3 HsBsdfTangent(float3 normal)
{
    float3 reference = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    return normalize(cross(reference, normal));
}

float3 HsBsdfToLocal(float3 value, float3 tangent, float3 bitangent, float3 normal)
{
    return float3(dot(value, tangent), dot(value, bitangent), dot(value, normal));
}

float3 HsBsdfToWorld(float3 value, float3 tangent, float3 bitangent, float3 normal)
{
    return tangent * value.x + bitangent * value.y + normal * value.z;
}

float3 HsBsdfBaseEvaluateOptical(HsSurfaceClosure surface, float3 wo, float3 wi, float base_fresnel_override)
{
    float3 normal = normalize(surface.normal);
    float3 view = normalize(wo);
    float3 light = normalize(wi);
    float raw_no_v = dot(normal, view);
    float raw_no_l = dot(normal, light);
    if (raw_no_v <= 0.0 || raw_no_l <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    float3 half_vector_sum = view + light;
    float half_vector_length_squared = dot(half_vector_sum, half_vector_sum);
    if (half_vector_length_squared <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    float3 half_vector = half_vector_sum * rsqrt(half_vector_length_squared);
    float no_v = raw_no_v;
    float no_l = raw_no_l;
    float no_h = saturate(dot(normal, half_vector));
    // For unit directions VoH = LoH = |wo + wi| / 2. Computing it
    // symmetrically preserves reciprocity at grazing angles in float math.
    float vo_h = saturate(0.5 * sqrt(half_vector_length_squared));
    float alpha = HsBsdfAlpha(surface);
    float alpha_squared = alpha * alpha;
    float distribution = HsBsdfD(no_h, alpha);
    float view_smith_denominator = no_v + sqrt(alpha_squared + (1.0 - alpha_squared) * no_v * no_v);
    float light_smith_denominator = no_l + sqrt(alpha_squared + (1.0 - alpha_squared) * no_l * no_l);
    float metallic = HsBsdfMetallic(surface);
    float3 fresnel = HsBsdfOpticalReflectionFresnel(surface, vo_h, base_fresnel_override);
    // A substrate path crosses the dielectric interface on entry and exit.
    // Use both macro-normal Fresnel transmissions for this conservative
    // single-scatter diffuse approximation; half-vector Fresnel belongs to
    // the reflected microfacet path and overcounts diffuse energy at grazing.
    // Hammon, PBR Diffuse Lighting for GGX+Smith Microsurfaces, GDC 2017.
    float3 diffuse_transmission =
        (1.0 - HsBsdfSchlickFresnel(HsBsdfF0(surface), no_v)) *
        (1.0 - HsBsdfSchlickFresnel(HsBsdfF0(surface), no_l));
    float3 diffuse = diffuse_transmission * ((1.0 - metallic) * surface.base_color) * HsBsdfInvPi;
    float3 specular = distribution * fresnel / (view_smith_denominator * light_smith_denominator);
    return diffuse + specular;
}

float3 HsBsdfBaseEvaluate(HsSurfaceClosure surface, float3 wo, float3 wi)
{
    return HsBsdfBaseEvaluateOptical(surface, wo, wi, -1.0);
}

float HsBsdfBasePdf(HsSurfaceClosure surface, float3 wo, float3 wi)
{
    float3 normal = normalize(surface.normal);
    float3 view = normalize(wo);
    float3 light = normalize(wi);
    float raw_no_v = dot(normal, view);
    float raw_no_l = dot(normal, light);
    if (raw_no_v <= 0.0 || raw_no_l <= 0.0)
    {
        return 0.0;
    }

    float3 half_vector_sum = view + light;
    float half_vector_length_squared = dot(half_vector_sum, half_vector_sum);
    if (half_vector_length_squared <= 0.0)
    {
        return 0.0;
    }

    float3 half_vector = half_vector_sum * rsqrt(half_vector_length_squared);
    float no_v = raw_no_v;
    float no_l = raw_no_l;
    float no_h = saturate(dot(normal, half_vector));
    float vo_h = saturate(dot(view, half_vector));
    if (no_h <= 0.0 || vo_h <= 0.0)
    {
        return 0.0;
    }

    float alpha = HsBsdfAlpha(surface);
    float distribution = HsBsdfD(no_h, alpha);
    float alpha_squared = alpha * alpha;
    float smith_denominator = no_v + sqrt(alpha_squared + (1.0 - alpha_squared) * no_v * no_v);
    // Heitz, "Sampling the GGX Distribution of Visible Normals", JCGT 7(4), 2018,
    // and PBRT v4's GGX reflection sampler use this solid-angle density:
    // p(wi|wo) = D(m) G1(wo) / (4 NoV) after the reflection Jacobian.
    // Invalid VNDF reflections remain null samples in HsBsdfSample; never resample.
    float specular_pdf = distribution / (2.0 * smith_denominator);
    float diffuse_pdf = no_l * HsBsdfInvPi;
    float specular_probability = HsBsdfSpecularProbability(surface);
    return lerp(diffuse_pdf, specular_pdf, specular_probability);
}

float3 HsBsdfSampleVisibleNormal(HsSurfaceClosure surface, float3 view, float2 sample, float3 tangent, float3 bitangent, float3 normal)
{
    float alpha = HsBsdfAlpha(surface);
    float3 view_local = HsBsdfToLocal(view, tangent, bitangent, normal);
    float3 stretched_view = normalize(float3(alpha * view_local.x, alpha * view_local.y, view_local.z));
    float lensq = stretched_view.x * stretched_view.x + stretched_view.y * stretched_view.y;
    float3 tangent_one = lensq > 0.0
        ? float3(-stretched_view.y, stretched_view.x, 0.0) * rsqrt(lensq)
        : float3(1.0, 0.0, 0.0);
    float3 tangent_two = cross(stretched_view, tangent_one);

    float radius = sqrt(sample.x);
    float phi = 2.0 * HsBsdfPi * sample.y;
    float disk_one = radius * cos(phi);
    float disk_two = radius * sin(phi);
    float interpolation = 0.5 * (1.0 + stretched_view.z);
    disk_two = (1.0 - interpolation) * sqrt(max(0.0, 1.0 - disk_one * disk_one)) + interpolation * disk_two;
    float3 stretched_normal = disk_one * tangent_one + disk_two * tangent_two
        + sqrt(max(0.0, 1.0 - dot(float2(disk_one, disk_two), float2(disk_one, disk_two)))) * stretched_view;
    float3 normal_local = normalize(float3(alpha * stretched_normal.x, alpha * stretched_normal.y, max(0.0, stretched_normal.z)));
    return normalize(HsBsdfToWorld(normal_local, tangent, bitangent, normal));
}

float3 HsBsdfSampleCosineHemisphere(float2 sample, float3 tangent, float3 bitangent, float3 normal)
{
    float radius = sqrt(sample.x);
    float phi = 2.0 * HsBsdfPi * sample.y;
    float3 local = float3(radius * cos(phi), radius * sin(phi), sqrt(max(0.0, 1.0 - sample.x)));
    return normalize(HsBsdfToWorld(local, tangent, bitangent, normal));
}



HsBsdfSampleResult HsBsdfBaseSample(HsSurfaceClosure surface, float3 wo, float u_lobe, float2 u_dir)
{
    HsBsdfSampleResult result;
    result.wi = float3(0.0, 0.0, 0.0);
    result.f = float3(0.0, 0.0, 0.0);
    result.pdf = 0.0;
    result.valid = false;

    float3 normal = normalize(surface.normal);
    float3 view = normalize(wo);
    if (dot(normal, view) <= 0.0)
    {
        return result;
    }

    float3 tangent = HsBsdfTangent(normal);
    float3 bitangent = cross(normal, tangent);
    float specular_probability = HsBsdfSpecularProbability(surface);
    bool choose_specular = u_lobe < specular_probability;
    float3 light;
    if (choose_specular)
    {
        float3 half_vector = HsBsdfSampleVisibleNormal(surface, view, u_dir, tangent, bitangent, normal);
        float view_half = dot(view, half_vector);
        if (view_half <= 0.0)
        {
            return result;
        }
        light = reflect(-view, half_vector);
        if (dot(normal, light) <= 0.0)
        {
            return result;
        }
    }
    else
    {
        light = HsBsdfSampleCosineHemisphere(u_dir, tangent, bitangent, normal);
    }

    result.wi = normalize(light);
    result.f = HsBsdfBaseEvaluate(surface, view, result.wi);
    result.pdf = HsBsdfBasePdf(surface, view, result.wi);
    result.valid = result.pdf > 0.0;
    if (!result.valid)
    {
        result.wi = float3(0.0, 0.0, 0.0);
        result.f = float3(0.0, 0.0, 0.0);
        result.pdf = 0.0;
    }
    return result;
}

#include "bsdf_layers.hlsli"

#endif
