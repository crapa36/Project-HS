#ifndef HS_BSDF_LAYERS_HLSLI
#define HS_BSDF_LAYERS_HLSLI

#include "bsdf_metallic_roughness.hlsli"

float HsBsdfThinTransmissionWeight(HsSurfaceClosure surface)
{
    return saturate(surface.transmission_weight) * (1.0 - HsBsdfMetallic(surface));
}

float3 HsBsdfThinTransmissionColor(HsSurfaceClosure surface)
{
    return saturate(surface.base_color) * saturate(surface.transmission_color);
}

bool HsBsdfHasLayers(HsSurfaceClosure surface)
{
    return surface.coat_weight > 0.0 || surface.anisotropy != 0.0 ||
        (surface.sheen_weight > 0.0 && any(surface.sheen_color > 0.0)) ||
        HsBsdfThinTransmissionWeight(surface) > 0.0;
}

HsSurfaceClosure HsBsdfOrientThinSurface(HsSurfaceClosure surface, float3 view)
{
    // The thin sheet has identical scattering on either face. Reflection-only
    // surfaces retain the original one-sided contract.
    if (HsBsdfThinTransmissionWeight(surface) > 0.0 && dot(surface.normal, view) < 0.0)
    {
        surface.normal = -surface.normal;
    }
    return surface;
}

void HsBsdfAnisotropicFrame(HsSurfaceClosure surface, float3 normal,
                          out float3 tangent, out float3 bitangent)
{
    float3 projected = surface.tangent - normal * dot(normal, surface.tangent);
    float length_squared = dot(projected, projected);
    tangent = length_squared > 0.0 ? projected * rsqrt(length_squared) : HsBsdfTangent(normal);
    bitangent = cross(normal, tangent);
}

float2 HsBsdfAnisotropicAlpha(HsSurfaceClosure surface)
{
    float alpha = HsBsdfAlpha(surface);
    // Keep the geometric mean equal to the original alpha. The sign rotates
    // the long axis by 90 degrees; zero anisotropy uses the untouched base path.
    float aspect = sqrt(1.0 - 0.9 * abs(clamp(surface.anisotropy, -1.0, 1.0)));
    float2 axes = float2(alpha / aspect, alpha * aspect);
    return surface.anisotropy < 0.0 ? axes.yx : axes;
}

float HsBsdfAnisotropicD(float3 half_local, float2 alpha)
{
    float2 stretched = half_local.xy / alpha;
    float denominator = dot(stretched, stretched) + half_local.z * half_local.z;
    return 1.0 / (HsBsdfPi * alpha.x * alpha.y * denominator * denominator);
}

float HsBsdfAnisotropicSmithDenominator(float3 direction_local, float2 alpha)
{
    float2 stretched = alpha * direction_local.xy;
    return direction_local.z + sqrt(dot(stretched, stretched) + direction_local.z * direction_local.z);
}

float3 HsBsdfAnisotropicEvaluateOptical(HsSurfaceClosure surface, float3 wo, float3 wi, float base_fresnel_override)
{
    if (surface.anisotropy == 0.0)
    {
        return HsBsdfBaseEvaluateOptical(surface, wo, wi, base_fresnel_override);
    }
    float3 normal = normalize(surface.normal);
    float3 view = normalize(wo);
    float3 light = normalize(wi);
    float no_v = dot(normal, view);
    float no_l = dot(normal, light);
    float3 half_sum = view + light;
    float half_length_squared = dot(half_sum, half_sum);
    if (no_v <= 0.0 || no_l <= 0.0 || half_length_squared <= 0.0)
    {
        return 0.0;
    }
    float3 tangent, bitangent;
    HsBsdfAnisotropicFrame(surface, normal, tangent, bitangent);
    float2 alpha = HsBsdfAnisotropicAlpha(surface);
    float3 half_local = HsBsdfToLocal(half_sum * rsqrt(half_length_squared), tangent, bitangent, normal);
    float distribution = HsBsdfAnisotropicD(half_local, alpha);
    float view_denominator = HsBsdfAnisotropicSmithDenominator(HsBsdfToLocal(view, tangent, bitangent, normal), alpha);
    float light_denominator = HsBsdfAnisotropicSmithDenominator(HsBsdfToLocal(light, tangent, bitangent, normal), alpha);
    float3 f0 = HsBsdfF0(surface);
    float3 fresnel = HsBsdfOpticalReflectionFresnel(surface, saturate(0.5 * sqrt(half_length_squared)), base_fresnel_override);
    float3 diffuse_transmission = (1.0 - HsBsdfSchlickFresnel(f0, no_v)) * (1.0 - HsBsdfSchlickFresnel(f0, no_l));
    float3 diffuse = diffuse_transmission * ((1.0 - HsBsdfMetallic(surface)) * surface.base_color) * HsBsdfInvPi;
    return diffuse + distribution * fresnel / (view_denominator * light_denominator);
}

float3 HsBsdfAnisotropicEvaluate(HsSurfaceClosure surface, float3 wo, float3 wi)
{
    return HsBsdfAnisotropicEvaluateOptical(surface, wo, wi, -1.0);
}

float HsBsdfAnisotropicPdf(HsSurfaceClosure surface, float3 wo, float3 wi)
{
    if (surface.anisotropy == 0.0)
    {
        return HsBsdfBasePdf(surface, wo, wi);
    }
    float3 normal = normalize(surface.normal);
    float3 view = normalize(wo);
    float3 light = normalize(wi);
    float no_v = dot(normal, view);
    float no_l = dot(normal, light);
    float3 half_sum = view + light;
    float half_length_squared = dot(half_sum, half_sum);
    if (no_v <= 0.0 || no_l <= 0.0 || half_length_squared <= 0.0)
    {
        return 0.0;
    }
    float3 half_vector = half_sum * rsqrt(half_length_squared);
    if (dot(normal, half_vector) <= 0.0 || dot(view, half_vector) <= 0.0)
    {
        return 0.0;
    }
    float3 tangent, bitangent;
    HsBsdfAnisotropicFrame(surface, normal, tangent, bitangent);
    float2 alpha = HsBsdfAnisotropicAlpha(surface);
    float distribution = HsBsdfAnisotropicD(HsBsdfToLocal(half_vector, tangent, bitangent, normal), alpha);
    float denominator = HsBsdfAnisotropicSmithDenominator(HsBsdfToLocal(view, tangent, bitangent, normal), alpha);
    float specular_pdf = distribution / (2.0 * denominator);
    return lerp(no_l * HsBsdfInvPi, specular_pdf, HsBsdfSpecularProbability(surface));
}

float3 HsBsdfSampleAnisotropicVisibleNormal(float3 view, float2 sample, float2 alpha,
                                         float3 tangent, float3 bitangent, float3 normal)
{
    // Heitz, Sampling the GGX Distribution of Visible Normals, JCGT 7(4),
    // 2018: anisotropic stretch, visible disk sample, then unstretch the normal.
    float3 local_view = HsBsdfToLocal(view, tangent, bitangent, normal);
    float3 stretched_view = normalize(float3(alpha * local_view.xy, local_view.z));
    float length_squared = dot(stretched_view.xy, stretched_view.xy);
    float3 tangent_one = length_squared > 0.0
        ? float3(-stretched_view.y, stretched_view.x, 0.0) * rsqrt(length_squared)
        : float3(1.0, 0.0, 0.0);
    float3 tangent_two = cross(stretched_view, tangent_one);
    float radius = sqrt(sample.x);
    float phi = 2.0 * HsBsdfPi * sample.y;
    float disk_one = radius * cos(phi);
    float disk_two = radius * sin(phi);
    float interpolation = 0.5 * (1.0 + stretched_view.z);
    disk_two = (1.0 - interpolation) * sqrt(max(0.0, 1.0 - disk_one * disk_one)) + interpolation * disk_two;
    float3 stretched_normal = disk_one * tangent_one + disk_two * tangent_two
        + sqrt(max(0.0, 1.0 - disk_one * disk_one - disk_two * disk_two)) * stretched_view;
    float3 local_normal = normalize(float3(alpha * stretched_normal.xy, max(0.0, stretched_normal.z)));
    return normalize(HsBsdfToWorld(local_normal, tangent, bitangent, normal));
}

HsBsdfSampleResult HsBsdfAnisotropicSample(HsSurfaceClosure surface, float3 wo, float u_lobe, float2 u_dir)
{
    if (surface.anisotropy == 0.0)
    {
        return HsBsdfBaseSample(surface, wo, u_lobe, u_dir);
    }
    HsBsdfSampleResult result = (HsBsdfSampleResult)0;
    float3 normal = normalize(surface.normal);
    float3 view = normalize(wo);
    if (dot(normal, view) <= 0.0)
    {
        return result;
    }
    float3 tangent, bitangent;
    HsBsdfAnisotropicFrame(surface, normal, tangent, bitangent);
    if (u_lobe < HsBsdfSpecularProbability(surface))
    {
        float3 half_vector = HsBsdfSampleAnisotropicVisibleNormal(view, u_dir, HsBsdfAnisotropicAlpha(surface), tangent, bitangent, normal);
        if (dot(view, half_vector) <= 0.0)
        {
            return result;
        }
        result.wi = reflect(-view, half_vector);
        if (dot(normal, result.wi) <= 0.0)
        {
            return (HsBsdfSampleResult)0;
        }
    }
    else
    {
        result.wi = HsBsdfSampleCosineHemisphere(u_dir, tangent, bitangent, normal);
    }
    result.wi = normalize(result.wi);
    result.f = HsBsdfAnisotropicEvaluate(surface, view, result.wi);
    result.pdf = HsBsdfAnisotropicPdf(surface, view, result.wi);
    result.valid = result.pdf > 0.0;
    if (!result.valid)
    {
        return (HsBsdfSampleResult)0;
    }
    return result;
}

HsSurfaceClosure HsBsdfCoatSurface(HsSurfaceClosure surface)
{
    return HsMaterialMakeSurfaceClosure(float3(0.04, 0.04, 0.04), surface.normal,
                                       surface.coat_roughness, 1.0, 1.0, 1.0);
}

float HsBsdfCoatFresnel(float cosine)
{
    return 0.04 + 0.96 * pow(1.0 - saturate(cosine), 5.0);
}

float3 HsBsdfSheenColor(HsSurfaceClosure surface)
{
    return saturate(surface.sheen_weight) * saturate(surface.sheen_color);
}

float HsBsdfSheenSubstrateScale(HsSurfaceClosure surface)
{
    float3 color = HsBsdfSheenColor(surface);
    // Estevez/Kulla 2017 layer by min(1-color*E(wo), 1-color*E(wi)).
    // Until a Charlie directional-albedo table is supplied, use the conservative
    // bound E<=1. This loses substrate energy but never substitutes the GGX DFG.
    return 1.0 - max(color.x, max(color.y, color.z));
}

float HsBsdfCharlieLambdaFit(float cosine, float alpha)
{
    float interpolation = (1.0 - alpha) * (1.0 - alpha);
    float a = lerp(21.5473, 25.3245, interpolation);
    float b = lerp(3.82987, 3.32435, interpolation);
    float c = lerp(0.19823, 0.16801, interpolation);
    float d = lerp(-1.97760, -1.27393, interpolation);
    float e = lerp(-4.32054, -4.85967, interpolation);
    return a / (1.0 + b * pow(cosine, c)) + d * cosine + e;
}

float HsBsdfCharlieLambda(float cosine, float alpha)
{
    cosine = saturate(cosine);
    return cosine < 0.5 ? exp(HsBsdfCharlieLambdaFit(cosine, alpha))
        : exp(2.0 * HsBsdfCharlieLambdaFit(0.5, alpha) - HsBsdfCharlieLambdaFit(1.0 - cosine, alpha));
}

float HsBsdfCharlieEvaluate(HsSurfaceClosure surface, float3 wo, float3 wi)
{
    float3 normal = normalize(surface.normal);
    float3 view = normalize(wo);
    float3 light = normalize(wi);
    float no_v = dot(normal, view);
    float no_l = dot(normal, light);
    float3 half_sum = view + light;
    float half_length_squared = dot(half_sum, half_sum);
    if (no_v <= 0.0 || no_l <= 0.0 || half_length_squared <= 0.0)
    {
        return 0.0;
    }
    // Production Friendly Microfacet Sheen BRDF, Estevez/Kulla 2017,
    // equations 2 and 3. Omit their non-reciprocal terminator modification.
    float alpha = max(surface.sheen_roughness * surface.sheen_roughness, 0.002);
    float inverse_alpha = 1.0 / alpha;
    float no_h = saturate(dot(normal, half_sum * rsqrt(half_length_squared)));
    float distribution = (2.0 + inverse_alpha) * pow(max(0.0, 1.0 - no_h * no_h), 0.5 * inverse_alpha) / (2.0 * HsBsdfPi);
    float visibility = 1.0 / ((1.0 + HsBsdfCharlieLambda(no_v, alpha) + HsBsdfCharlieLambda(no_l, alpha)) * 4.0 * no_v * no_l);
    return distribution * visibility;
}

float4 HsBsdfLayerProbabilities(HsSurfaceClosure surface, float3 view)
{
    float no_v = saturate(dot(normalize(surface.normal), view));
    float coat = saturate(surface.coat_weight) * HsBsdfCoatFresnel(no_v);
    float substrate = (1.0 - coat) * HsBsdfSheenSubstrateScale(surface);
    float transmission = HsBsdfThinTransmissionWeight(surface);
    float base_energy = max(0.0, HsBsdfLuminance(HsBsdfF0(surface))
        + HsBsdfLuminance((1.0 - HsBsdfMetallic(surface)) * surface.base_color));
    // These are proposal probabilities, not extra BRDF factors. Pdf and Sample
    // use the same view-dependent mixture and retain rejected VNDF reflections.
    float4 weights = float4(substrate * (1.0 - transmission) * base_energy,
                            coat, HsBsdfLuminance(HsBsdfSheenColor(surface)),
                            substrate * transmission * HsBsdfLuminance(HsBsdfThinTransmissionColor(surface)));
    float total = dot(weights, float4(1.0, 1.0, 1.0, 1.0));
    return total > 0.0 ? weights / total : float4(1.0, 0.0, 0.0, 0.0);
}

float3 HsBsdfEvaluateOptical(HsSurfaceClosure surface, float3 wo, float3 wi, float base_fresnel_override)
{
    if (surface.unlit)
    {
        return 0.0;
    }
    if (!HsBsdfHasLayers(surface))
    {
        return HsBsdfBaseEvaluateOptical(surface, wo, wi, base_fresnel_override);
    }
    float3 view = normalize(wo);
    float3 light = normalize(wi);
    surface = HsBsdfOrientThinSurface(surface, view);
    float3 normal = normalize(surface.normal);
    float no_v = dot(normal, view);
    float no_l = dot(normal, light);
    if (no_v <= 0.0 || no_l == 0.0)
    {
        return 0.0;
    }
    float transmission = HsBsdfThinTransmissionWeight(surface);
    float3 substrate = no_l > 0.0
        ? (1.0 - transmission) * HsBsdfAnisotropicEvaluateOptical(surface, view, light, base_fresnel_override)
        : transmission * HsBsdfThinTransmissionColor(surface) * HsBsdfInvPi;
    substrate *= HsBsdfSheenSubstrateScale(surface);
    if (no_l > 0.0 && surface.sheen_weight > 0.0 && any(surface.sheen_color > 0.0))
    {
        substrate += HsBsdfSheenColor(surface) * HsBsdfCharlieEvaluate(surface, view, light);
    }
    float coat_weight = saturate(surface.coat_weight);
    // The inherited coat normal attenuates entry and exit symmetrically. For
    // transmission the exit cosine is measured on the opposite sheet face.
    float coat_transmission = (1.0 - coat_weight * HsBsdfCoatFresnel(no_v))
        * (1.0 - coat_weight * HsBsdfCoatFresnel(abs(no_l)));
    float3 result = substrate * coat_transmission;
    if (no_l > 0.0 && coat_weight > 0.0)
    {
        result += coat_weight * HsBsdfBaseEvaluate(HsBsdfCoatSurface(surface), view, light);
    }
    return result;
}

float3 HsBsdfEvaluate(HsSurfaceClosure surface, float3 wo, float3 wi)
{
    return HsBsdfEvaluateOptical(surface, wo, wi, -1.0);
}

// Separate the existing reflection BSDF's diffuse substrate from its specular
// lobes. Skin diffusion must consume only diffuse; coat/sheen/GGX stay sharp.
void HsBsdfEvaluateComponentsOptical(HsSurfaceClosure surface, float3 wo, float3 wi, float base_fresnel_override,
                              out float3 diffuse, out float3 specular)
{
    float3 total = HsBsdfEvaluateOptical(surface, wo, wi, base_fresnel_override);
    float3 view = normalize(wo), light = normalize(wi);
    surface = HsBsdfOrientThinSurface(surface, view);
    float3 normal = normalize(surface.normal);
    float no_v = dot(normal, view), no_l = dot(normal, light);
    diffuse = 0.0;
    if (!surface.unlit && no_v > 0.0 && no_l < 0.0)
    {
        // The only opposite-hemisphere lobe is cosine thin-sheet diffuse
        // transmission. It must not enter the reflection replacement target.
        diffuse = total;
    }
    if (!surface.unlit && no_v > 0.0 && no_l > 0.0)
    {
        float3 f0 = HsBsdfF0(surface);
        diffuse = (1.0 - HsBsdfSchlickFresnel(f0, no_v)) *
            (1.0 - HsBsdfSchlickFresnel(f0, no_l)) *
            ((1.0 - HsBsdfMetallic(surface)) * surface.base_color) * HsBsdfInvPi;
        diffuse *= (1.0 - HsBsdfThinTransmissionWeight(surface)) * HsBsdfSheenSubstrateScale(surface);
        float coat = saturate(surface.coat_weight);
        diffuse *= (1.0 - coat * HsBsdfCoatFresnel(no_v)) *
                   (1.0 - coat * HsBsdfCoatFresnel(no_l));
    }
    specular = max(total - diffuse, 0.0);
}

void HsBsdfEvaluateComponents(HsSurfaceClosure surface, float3 wo, float3 wi,
                              out float3 diffuse, out float3 specular)
{
    HsBsdfEvaluateComponentsOptical(surface, wo, wi, -1.0, diffuse, specular);
}

// Author thickness is the low path optical distance. Hero front/back depths
// can supply the actual refracted path instead; opacity remains coverage.
float3 HsBsdfSolidBeerLambert(HsSurfaceClosure surface, float optical_path_m)
{
    return exp(-max(surface.absorption_per_m, 0.0) * max(optical_path_m, 0.0));
}

float HsBsdfPdf(HsSurfaceClosure surface, float3 wo, float3 wi)
{
    if (surface.unlit)
    {
        return 0.0;
    }
    if (!HsBsdfHasLayers(surface))
    {
        return HsBsdfBasePdf(surface, wo, wi);
    }
    float3 view = normalize(wo);
    float3 light = normalize(wi);
    surface = HsBsdfOrientThinSurface(surface, view);
    float3 normal = normalize(surface.normal);
    float no_l = dot(normal, light);
    if (dot(normal, view) <= 0.0 || no_l == 0.0)
    {
        return 0.0;
    }
    float4 probabilities = HsBsdfLayerProbabilities(surface, view);
    if (no_l < 0.0)
    {
        return probabilities.w * (-no_l) * HsBsdfInvPi;
    }
    // The primary Charlie reference recommends uniform hemisphere sampling.
    return probabilities.x * HsBsdfAnisotropicPdf(surface, view, light)
        + probabilities.y * HsBsdfBasePdf(HsBsdfCoatSurface(surface), view, light)
        + probabilities.z * (0.5 * HsBsdfInvPi);
}

// NRD independent primary signals condition the existing proposal family on
// the component being integrated. No diffuse/specular Bernoulli holes remain.
// This does not change HsBsdfSample or any raster/secondary-path estimator.
float2 HsBsdfNrdDiffuseProbabilities(HsSurfaceClosure surface,float3 view)
{
    float4 layers=HsBsdfLayerProbabilities(surface,view);
    float2 weights=float2(layers.x*(1.0-HsBsdfSpecularProbability(surface)),layers.w);
    float total=weights.x+weights.y;return total>0.0?weights/total:0.0;
}
float3 HsBsdfNrdSpecularProbabilities(HsSurfaceClosure surface,float3 view)
{
    float4 layers=HsBsdfLayerProbabilities(surface,view);
    float3 weights=float3(layers.x*HsBsdfSpecularProbability(surface),layers.y,layers.z);
    float total=weights.x+weights.y+weights.z;return total>0.0?weights/total:0.0;
}
float HsBsdfNrdAnisotropicSpecularPdf(HsSurfaceClosure surface,float3 wo,float3 wi)
{
    if(surface.anisotropy==0.0)return HsBsdfNrdBaseSpecularPdf(surface,wo,wi);
    float3 normal=normalize(surface.normal),view=normalize(wo),light=normalize(wi),halfSum=view+light;
    float halfLength=dot(halfSum,halfSum);
    if(dot(normal,view)<=0.0||dot(normal,light)<=0.0||halfLength<=0.0)return 0.0;
    float3 halfVector=halfSum*rsqrt(halfLength);
    if(dot(normal,halfVector)<=0.0||dot(view,halfVector)<=0.0)return 0.0;
    float3 tangent,bitangent;HsBsdfAnisotropicFrame(surface,normal,tangent,bitangent);
    float2 alpha=HsBsdfAnisotropicAlpha(surface);
    float denominator=HsBsdfAnisotropicSmithDenominator(HsBsdfToLocal(view,tangent,bitangent,normal),alpha);
    return HsBsdfAnisotropicD(HsBsdfToLocal(halfVector,tangent,bitangent,normal),alpha)/(2.0*denominator);
}
float HsBsdfNrdComponentPdf(HsSurfaceClosure surface,float3 wo,float3 wi,bool specular)
{
    float3 view=normalize(wo),light=normalize(wi);surface=HsBsdfOrientThinSurface(surface,view);
    float noL=dot(normalize(surface.normal),light);if(surface.unlit||dot(surface.normal,view)<=0.0||noL==0.0)return 0.0;
    if(!specular)
    {
        float2 probabilities=HsBsdfNrdDiffuseProbabilities(surface,view);
        return (noL>0.0?probabilities.x:probabilities.y)*abs(noL)*HsBsdfInvPi;
    }
    if(noL<=0.0)return 0.0;
    float3 probabilities=HsBsdfNrdSpecularProbabilities(surface,view);
    return probabilities.x*HsBsdfNrdAnisotropicSpecularPdf(surface,view,light)+
        probabilities.y*HsBsdfNrdBaseSpecularPdf(HsBsdfCoatSurface(surface),view,light)+probabilities.z*(0.5*HsBsdfInvPi);
}
HsBsdfSampleResult HsBsdfNrdSampleComponent(HsSurfaceClosure surface,float3 wo,float uLobe,float2 uDirection,bool specular)
{
    HsBsdfSampleResult result=(HsBsdfSampleResult)0;float3 view=normalize(wo);
    surface=HsBsdfOrientThinSurface(surface,view);float3 normal=normalize(surface.normal);
    if(surface.unlit||dot(normal,view)<=0.0)return result;
    float3 tangent=HsBsdfTangent(normal),bitangent=cross(normal,tangent);
    if(!specular)
    {
        float2 probabilities=HsBsdfNrdDiffuseProbabilities(surface,view);
        if(probabilities.x+probabilities.y==0.0)return result;
        result.wi=HsBsdfSampleCosineHemisphere(uDirection,tangent,bitangent,uLobe<probabilities.x?normal:-normal);
    }
    else
    {
        float3 probabilities=HsBsdfNrdSpecularProbabilities(surface,view);
        if(probabilities.x+probabilities.y+probabilities.z==0.0)return result;
        if(uLobe<probabilities.x)
        {
            float3 halfVector;
            if(surface.anisotropy==0.0)halfVector=HsBsdfSampleVisibleNormal(surface,view,uDirection,tangent,bitangent,normal);
            else{HsBsdfAnisotropicFrame(surface,normal,tangent,bitangent);
                halfVector=HsBsdfSampleAnisotropicVisibleNormal(view,uDirection,HsBsdfAnisotropicAlpha(surface),tangent,bitangent,normal);}
            if(dot(view,halfVector)<=0.0)return result;result.wi=reflect(-view,halfVector);
        }
        else if(uLobe<probabilities.x+probabilities.y)
        {
            float3 halfVector=HsBsdfSampleVisibleNormal(HsBsdfCoatSurface(surface),view,uDirection,tangent,bitangent,normal);
            if(dot(view,halfVector)<=0.0)return result;result.wi=reflect(-view,halfVector);
        }
        else
        {
            float cosine=uDirection.x,sine=sqrt(max(0.0,1.0-cosine*cosine)),phi=2.0*HsBsdfPi*uDirection.y;
            result.wi=HsBsdfToWorld(float3(sine*cos(phi),sine*sin(phi),cosine),tangent,bitangent,normal);
        }
        // Rejected VNDF rays retain the fixed denominator; never retry them.
        if(dot(normal,result.wi)<=0.0)return (HsBsdfSampleResult)0;
    }
    result.wi=normalize(result.wi);result.pdf=HsBsdfNrdComponentPdf(surface,view,result.wi,specular);
    float3 diffuse,specularValue;HsBsdfEvaluateComponents(surface,view,result.wi,diffuse,specularValue);
    result.f=specular?specularValue:diffuse;result.valid=result.pdf>0.0;return result;
}

HsBsdfSampleResult HsBsdfSample(HsSurfaceClosure surface, float3 wo, float u_lobe, float2 u_dir)
{
    if (surface.unlit)
    {
        return (HsBsdfSampleResult)0;
    }
    if (!HsBsdfHasLayers(surface))
    {
        return HsBsdfBaseSample(surface, wo, u_lobe, u_dir);
    }
    HsBsdfSampleResult result = (HsBsdfSampleResult)0;
    float3 view = normalize(wo);
    surface = HsBsdfOrientThinSurface(surface, view);
    float3 normal = normalize(surface.normal);
    if (dot(normal, view) <= 0.0)
    {
        return result;
    }
    float4 probabilities = HsBsdfLayerProbabilities(surface, view);
    if (u_lobe < probabilities.x)
    {
        result = HsBsdfAnisotropicSample(surface, view, u_lobe / probabilities.x, u_dir);
        if (!result.valid)
        {
            return result;
        }
    }
    else if (u_lobe < probabilities.x + probabilities.y)
    {
        result = HsBsdfBaseSample(HsBsdfCoatSurface(surface), view, 0.0, u_dir);
        if (!result.valid)
        {
            return result;
        }
    }
    else
    {
        float3 tangent = HsBsdfTangent(normal);
        float3 bitangent = cross(normal, tangent);
        if (u_lobe < probabilities.x + probabilities.y + probabilities.z)
        {
            float cosine = u_dir.x;
            float sine = sqrt(max(0.0, 1.0 - cosine * cosine));
            float phi = 2.0 * HsBsdfPi * u_dir.y;
            result.wi = HsBsdfToWorld(float3(sine * cos(phi), sine * sin(phi), cosine), tangent, bitangent, normal);
        }
        else
        {
            result.wi = HsBsdfSampleCosineHemisphere(u_dir, tangent, bitangent, -normal);
        }
    }
    result.wi = normalize(result.wi);
    result.f = HsBsdfEvaluate(surface, view, result.wi);
    result.pdf = HsBsdfPdf(surface, view, result.wi);
    result.valid = result.pdf > 0.0;
    if (!result.valid)
    {
        return (HsBsdfSampleResult)0;
    }
    return result;
}

float3 HsBsdfIndirectSampleWeight(HsSurfaceClosure surface, HsBsdfSampleResult sample)
{
    if (!sample.valid || sample.pdf <= 0.0)
    {
        return 0.0;
    }
    return sample.f * abs(dot(normalize(surface.normal), sample.wi)) / sample.pdf;
}

uint HsBsdfReverseBits(uint bits)
{
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
    bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
    return ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
}

float3 HsBsdfIntegrateAnalyticSky(HsSurfaceClosure surface, float3 wo, float3 sky_bottom, float3 sky_top)
{
    // Integrate the actual layered BSDF, including the Charlie and opposite
    // sheet lobes. A GGX AB lookup is not the cloth directional albedo.
    const uint sample_count = 32;
    float3 accumulated = 0.0;
    [loop]
    for (uint index = 0; index < sample_count; ++index)
    {
        float u_lobe = frac((float(index) + 0.5) * 0.6180339887498948482);
        float2 u_dir = float2((float(index) + 0.5) / float(sample_count),
                             float(HsBsdfReverseBits(index)) * 2.3283064365386962890625e-10);
        HsBsdfSampleResult sample = HsBsdfSample(surface, wo, u_lobe, u_dir);
        float3 sky = lerp(sky_bottom, sky_top, saturate(sample.wi.y * 0.5 + 0.5));
        accumulated += sky * HsBsdfIndirectSampleWeight(surface, sample);
    }
    // Keep null VNDF samples in the denominator; never renormalize survivors.
    return accumulated / float(sample_count);
}

#endif
