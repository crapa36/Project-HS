#include "../Content/Shaders/bsdf_metallic_roughness.hlsli"
static const float4 ProbeOptions=0;
SamplerState LinearClamp : register(s0);
Texture2D<float4> EnvironmentBrdfLut : register(t0);
SamplerState ProbeBrdfClamp : register(s1);
#include "../Content/Shaders/local_probe_sampling.hlsli"

RWStructuredBuffer<float4> output : register(u1);

static const uint HsProbeSampleCount = 32768;
static const float HsProbeTwoPi = 6.28318530717958647692;

uint HsProbeReverseBits(uint bits)
{
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
    bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
    bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
    return bits;
}

float2 HsProbeHammersley(uint index)
{
    const float inverse_count = 1.0 / float(HsProbeSampleCount);
    return float2((float(index) + 0.5) * inverse_count,
                  float(HsProbeReverseBits(index)) * 2.3283064365386962890625e-10);
}

float2 HsProbeIndependentReferenceSample(uint index)
{
    float radical_inverse_three=0.0;
    float place=1.0/3.0;
    for(uint value=index+1u;value>0u;value/=3u)
    {
        radical_inverse_three+=float(value%3u)*place;
        place/=3.0;
    }
    return float2(float(HsProbeReverseBits(index))*2.3283064365386962890625e-10,
        radical_inverse_three);
}

float HsProbeMax3(float3 value)
{
    return max(value.x, max(value.y, value.z));
}

float HsProbeMaxError(float current, float3 difference)
{
    return max(current, HsProbeMax3(abs(difference)));
}

float3 HsProbeUniformHemisphere(float2 sample)
{
    float z = sample.x;
    float radius = sqrt(max(0.0, 1.0 - z * z));
    float phi = HsProbeTwoPi * sample.y;
    return float3(radius * cos(phi), radius * sin(phi), z);
}

HsSurfaceClosure HsProbeSurface(uint case_index)
{
    HsSurfaceClosure surface = (HsSurfaceClosure)0;
    surface.normal = float3(0.0, 0.0, 1.0);
    surface.ao = 1.0;
    surface.opacity = 1.0;
    surface.normal_is_world = true;

    switch (case_index)
    {
    case 0:
        surface.base_color = float3(1.0, 1.0, 1.0);
        surface.metallic = 0.0;
        surface.roughness = 0.8;
        break;
    case 1:
        surface.base_color = float3(1.0, 1.0, 1.0);
        surface.metallic = 1.0;
        surface.roughness = 0.8;
        break;
    case 2:
        surface.base_color = float3(0.3, 0.5, 0.7);
        surface.metallic = 0.5;
        surface.roughness = 0.5;
        break;
    case 3:
        surface.base_color = float3(1.0, 1.0, 1.0);
        surface.metallic = 0.0;
        surface.roughness = 0.5;
        break;
    case 4:
        surface.base_color = float3(1.0, 1.0, 1.0);
        surface.metallic = 1.0;
        surface.roughness = 1.0;
        break;
    case 5:
        surface.base_color = float3(0.3, 0.5, 0.7);
        surface.metallic = 0.5;
        surface.roughness = 1.0;
        break;
    default:
        surface.base_color = float3(0.3, 0.5, 0.7);
        surface.metallic = 0.5;
        surface.roughness = 0.7;
        break;
    }
    return surface;
}

HsSurfaceClosure HsProbeLayeredRegressionSurface(uint case_index)
{
    HsSurfaceClosure surface = HsMaterialMakeSurfaceClosure(
        float3(0.45, 0.3, 0.2), float3(0.0, 0.0, 1.0), 0.65, 0.25, 1.0, 1.0);
    if (case_index == 1u)
    {
        surface.base_color = float3(1.0, 1.0, 1.0);
        surface.metallic = 1.0;
        surface.roughness = 0.8;
    }
    surface.normal_is_world = true;
    surface.ior = 1.5;
    if (case_index == 0u || case_index >= 4u)
        surface = HsMaterialApplyCoat(surface, 0.4, 0.22);
    if (case_index == 1u || case_index >= 4u)
        surface = HsMaterialApplyAnisotropy(surface, 0.7, float3(1.0, 0.0, 0.0));
    if (case_index == 2u || case_index >= 4u)
        surface = HsMaterialApplySheen(surface, 0.65, float3(0.8, 0.65, 0.5), 0.45);
    if (case_index == 3u || case_index >= 4u)
        surface = HsMaterialApplyThinTransmission(surface, 0.55, float3(0.9, 0.8, 0.7));
    if (case_index == 5u)
        surface.unlit = true;
    return surface;
}

// Frozen pre-refactor layered paths: keep this test oracle independent of the prepared API.
float HsProbeOriginalLayeredPdf(HsSurfaceClosure surface, float3 wo, float3 wi)
{
    if (surface.unlit)
        return 0.0;
    if (!HsBsdfHasLayers(surface))
        return HsBsdfBasePdf(surface, wo, wi);
    float3 view = normalize(wo);
    float3 light = normalize(wi);
    surface = HsBsdfOrientThinSurface(surface, view);
    float3 normal = normalize(surface.normal);
    float no_l = dot(normal, light);
    if (dot(normal, view) <= 0.0 || no_l == 0.0)
        return 0.0;
    float4 probabilities = HsBsdfLayerProbabilities(surface, view);
    if (no_l < 0.0)
        return probabilities.w * (-no_l) * HsBsdfInvPi;
    return probabilities.x * HsBsdfAnisotropicPdf(surface, view, light)
        + probabilities.y * HsBsdfBasePdf(HsBsdfCoatSurface(surface), view, light)
        + probabilities.z * (0.5 * HsBsdfInvPi);
}

HsBsdfSampleResult HsProbeOriginalLayeredSample(
    HsSurfaceClosure surface, float3 wo, float u_lobe, float2 u_dir)
{
    if (surface.unlit)
        return (HsBsdfSampleResult)0;
    if (!HsBsdfHasLayers(surface))
        return HsBsdfBaseSample(surface, wo, u_lobe, u_dir);
    HsBsdfSampleResult result = (HsBsdfSampleResult)0;
    float3 view = normalize(wo);
    surface = HsBsdfOrientThinSurface(surface, view);
    float3 normal = normalize(surface.normal);
    if (dot(normal, view) <= 0.0)
        return result;
    float4 probabilities = HsBsdfLayerProbabilities(surface, view);
    if (u_lobe < probabilities.x)
    {
        result = HsBsdfAnisotropicSample(surface, view, u_lobe / probabilities.x, u_dir);
        if (!result.valid)
            return result;
    }
    else if (u_lobe < probabilities.x + probabilities.y)
    {
        result = HsBsdfBaseSample(HsBsdfCoatSurface(surface), view, 0.0, u_dir);
        if (!result.valid)
            return result;
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
            result.wi = HsBsdfToWorld(
                float3(sine * cos(phi), sine * sin(phi), cosine), tangent, bitangent, normal);
        }
        else
        {
            result.wi = HsBsdfSampleCosineHemisphere(u_dir, tangent, bitangent, -normal);
        }
    }
    result.wi = normalize(result.wi);
    result.f = HsBsdfEvaluate(surface, view, result.wi);
    result.pdf = HsProbeOriginalLayeredPdf(surface, view, result.wi);
    result.valid = result.pdf > 0.0;
    if (!result.valid)
        return (HsBsdfSampleResult)0;
    return result;
}

float HsProbePreparedLayeredRegressionError(uint case_index)
{
    HsSurfaceClosure surface = HsProbeLayeredRegressionSurface(case_index);
    surface.normal = float3(0.17, 0.31, 0.73);
    const float3 normal = normalize(surface.normal);
    float no_v = case_index == 6u ? 0.0001 : case_index == 1u ? 0.2 : 0.8;
    float3 front_view = (HsBsdfTangent(normal) * sqrt(max(0.0, 1.0 - no_v * no_v)) +
        normal * no_v) * 1.37;
    HsBsdfSamplingContext front_sampling = HsBsdfPrepareSampling(surface, front_view);
    HsBsdfSamplingContext back_sampling = HsBsdfPrepareSampling(surface, -front_view);
    HsBsdfPdfContext front_pdf = HsBsdfPreparePdf(surface, front_view);
    HsBsdfPdfContext back_pdf = HsBsdfPreparePdf(surface, -front_view);
    float error = 0.0;
    [loop]
    for (uint index = 0u; index < 16u; ++index)
    {
        const bool back_face = (index & 1u) != 0u;
        float3 wo = front_view;
        HsBsdfSamplingContext sampling = front_sampling;
        HsBsdfPdfContext pdf_context = front_pdf;
        if (back_face)
        {
            wo = -front_view;
            sampling = back_sampling;
            pdf_context = back_pdf;
        }
        const float u_lobe = frac((float(index) + 0.5) * 0.6180339887498948482);
        const float2 u_dir = float2((float(index) + 0.5) / 16.0,
            float(HsProbeReverseBits(index)) * 2.3283064365386962890625e-10);
        const HsBsdfSampleResult original = HsProbeOriginalLayeredSample(surface, wo, u_lobe, u_dir);
        const HsBsdfSampleResult prepared = HsBsdfSamplePrepared(surface, wo, sampling, u_lobe, u_dir);
        const HsBsdfSampleResult raw = HsBsdfSample(surface, wo, u_lobe, u_dir);
        error = max(error, original.valid != prepared.valid ? 1.0 : 0.0);
        error = max(error, original.valid != raw.valid ? 1.0 : 0.0);
        error = max(error, abs(original.pdf - prepared.pdf));
        error = max(error, abs(original.pdf - raw.pdf));
        error = HsProbeMaxError(error, original.wi - prepared.wi);
        error = HsProbeMaxError(error, original.wi - raw.wi);
        error = HsProbeMaxError(error, original.f - prepared.f);
        error = HsProbeMaxError(error, original.f - raw.f);
        const float3 wi = HsProbeUniformHemisphere(u_dir);
        const float original_pdf = HsProbeOriginalLayeredPdf(surface, wo, wi);
        const float prepared_pdf = HsBsdfPdfPrepared(surface, wo, pdf_context, wi);
        const float raw_pdf = HsBsdfPdf(surface, wo, wi);
        error = max(error, abs(original_pdf - prepared_pdf));
        error = max(error, abs(original_pdf - raw_pdf));
    }
    return error;
}

float HsProbeLobeCoordinate(uint index)
{
    return frac((float(index) + 0.5) * 0.6180339887498948482);
}

float HsProbeLocalBlendError(uint case_index)
{
    const uint count=case_index==0u?0u:case_index<4u?1u:case_index<6u?4u:5u;
    const float4 weights=case_index==1u?0:case_index<4u?float4(case_index==2u?.35:1,0,0,0):
        case_index==4u?float4(.1,.2,.05,.15):float4(.5,1,.25,2);
    const HsLocalProbeBlend blend=HsLocalProbePrepareBlend(weights,count);
    float error=max(max(abs(blend.weights.x-weights.x),abs(blend.weights.y-weights.y)),
        max(abs(blend.weights.z-weights.z),abs(blend.weights.w-weights.w)));
    // The five-probe path must keep raw cached weights for the generic loop.
    if(count>4u) return max(error,max(abs(blend.normalization.x),abs(blend.normalization.y)));
    float total=0; float3 local=0;
    [loop] for(uint i=0u;i<count;++i)
        if(weights[i]>0) { total+=weights[i]; local+=weights[i]*float3(i+1u,2u*i+1u,4u-i); }
    const float3 global=float3(.25,.5,.75);
    const float cover=saturate(total);
    const float3 reference=global*(1-cover)+(total>0?local*(cover/total):0);
    const float3 prepared=global*blend.normalization.x+local*blend.normalization.y;
    return HsProbeMaxError(error,prepared-reference);
}

static const uint HsProbeLegacyCaseCount = 7u;
static const uint HsProbeCoatedCaseCount = 72u;
static const uint HsProbeFresnelCaseCount = 14u;
static const uint HsProbeTotalCaseCount = HsProbeLegacyCaseCount + HsProbeCoatedCaseCount + HsProbeFresnelCaseCount;
static const uint HsProbeCoatedReferenceSamples = 65536u;

struct HsProbeCoatedCase
{
    HsSurfaceClosure surface;
    float no_v;
    float fresnel_override;
};

float HsProbeLutCenter(float value)
{
    const uint cell = min(uint(value * 128.0), 127u);
    return (float(cell) + 0.5) / 128.0;
}

float HsProbeRoughnessCenter(float value)
{
    const uint cell = min(uint(saturate(value) * 256.0), 255u);
    return (float(cell) + 0.5) / 256.0;
}

HsProbeCoatedCase HsProbeMakeCoatedCase(uint index)
{
    const uint grid_index = min(index, 23u);
    const uint no_v_index = grid_index / 8u;
    const uint roughness_index = (grid_index / 2u) % 4u;
    const float no_v_targets[3] = {0.95, 0.5, 0.02};
    const float roughness_targets[4] = {0.02, 0.2, 0.5, 0.9};
    HsProbeCoatedCase value;
    value.surface = HsMaterialMakeSurfaceClosure(float3(0.43, 0.27, 0.18),
        float3(0,0,1), HsProbeLutCenter(roughness_targets[roughness_index]), 0.0, 1.0, 1.0);
    value.surface.normal_is_world = true;
    value.surface.coat_weight = (grid_index & 1u) == 0u ? 0.38 : 1.0;
    value.surface.coat_roughness = HsProbeLutCenter(roughness_targets[(roughness_index + 1u) % 4u]);
    value.surface.ior = 1.5;
    value.no_v = HsProbeLutCenter(no_v_targets[no_v_index]);
    value.fresnel_override = -1.0;
    if (index < 24u)
        return value;

    if (index < 32u)
    {
        // Exercise metal mixtures, IOR-derived F0, partial solid budgets, and TIR override.
        const uint edge = index - 24u;
        value.no_v = HsProbeLutCenter(edge % 3u == 0u ? 0.02 : edge % 3u == 1u ? 0.5 : 0.95);
        value.surface.roughness = HsProbeLutCenter(roughness_targets[(edge + 1u) % 4u]);
        value.surface.coat_roughness = HsProbeLutCenter(roughness_targets[(edge + 2u) % 4u]);
        value.surface.metallic = edge == 7u ? 1.0 : edge % 2u == 0u ? 0.35 : 0.85;
        value.surface.solid_transmission_weight = edge < 2u ? 0.0 : edge < 4u ? 0.5 : 1.0;
        value.surface.ior = edge % 2u == 0u ? 1.333 : 1.5;
        value.surface.coat_weight = edge % 2u == 0u ? 0.38 : 1.0;
        value.fresnel_override = edge >= 2u ? 1.0 : -1.0;
        return value;
    }

    // Exercise the new grazing range across roughness/coat roughness and F0 extremes.
    const uint grazing_case = index - 32u;
    const uint grazing_index = grazing_case / 8u;
    const uint grazing_roughness_index = (grazing_case / 2u) % 4u;
    const float grazing_no_v[5] = {0.001, 0.002, 0.004, 0.008, 0.012};
    value.no_v = grazing_no_v[grazing_index];
    value.surface.roughness = HsProbeRoughnessCenter(roughness_targets[grazing_roughness_index]);
    value.surface.coat_roughness = HsProbeRoughnessCenter(roughness_targets[(grazing_roughness_index + 1u) % 4u]);
    value.surface.metallic = ((grazing_index + grazing_roughness_index) & 1u) != 0u ? 1.0 : 0.0;
    value.surface.ior = ((grazing_index + grazing_roughness_index) & 1u) != 0u ? 1.333 : 1.5;
    value.surface.solid_transmission_weight = 0.0;
    value.surface.coat_weight = (grazing_case & 1u) == 0u ? 0.38 : 1.0;
    value.fresnel_override = -1.0;
    return value;
}

float3 HsProbeCoatedReference(HsSurfaceClosure surface, float no_v, float base_fresnel_override,
    out float3 diffuse_reference)
{
    const float3 normal = float3(0,0,1);
    const float3 view = float3(sqrt(max(0.0,1.0-no_v*no_v)),0,no_v);
    const float coat_weight = saturate(surface.coat_weight);
    const float entry = 1.0 - coat_weight * HsBsdfCoatFresnel(no_v);
    HsSurfaceClosure unit_base = surface;
    unit_base.base_color = 1.0;
    unit_base.metallic = 1.0;
    unit_base.solid_transmission_weight = 0.0;
    unit_base.roughness = surface.roughness;
    const float3 tangent = HsBsdfTangent(normal);
    const float3 bitangent = cross(normal,tangent);
    const float alpha = HsBsdfAlpha(unit_base);
    const float alpha_squared = alpha * alpha;
    const float view_denom = no_v + sqrt(alpha_squared + (1.0-alpha_squared)*no_v*no_v);
    float3 base_integral = 0.0;
    float coat_integral = 0.0;
    float3 diffuse_integral = 0.0;
    [loop]
    for (uint sample_index=0u;sample_index<HsProbeCoatedReferenceSamples;++sample_index)
    {
        const float2 sample = HsProbeIndependentReferenceSample(sample_index);
        const float3 half_vector = HsBsdfSampleVisibleNormal(unit_base,view,sample,tangent,bitangent,normal);
        const float view_half = saturate(dot(view,half_vector));
        if (view_half > 0.0)
        {
            const float3 light = normalize(reflect(-view,half_vector));
            const float no_l = dot(normal,light);
            if (no_l > 0.0)
            {
                const float no_h = saturate(dot(normal,half_vector));
                const float distribution = HsBsdfD(no_h,alpha);
                const float light_denom = no_l + sqrt(alpha_squared+(1.0-alpha_squared)*no_l*no_l);
                const float spec_pdf = distribution/(2.0*view_denom);
                float3 authored_f = HsBsdfSchlickFresnel(HsBsdfF0(surface),view_half);
                if (base_fresnel_override >= 0.0)
                    authored_f = lerp(authored_f,saturate(base_fresnel_override).xxx,
                        saturate(surface.solid_transmission_weight));
                const float3 base_brdf = distribution*authored_f/(view_denom*light_denom);
                const float coat_exit = 1.0-coat_weight*HsBsdfCoatFresnel(no_l);
                base_integral += base_brdf*(entry*coat_exit*no_l/spec_pdf);
            }
        }

        const float3 uniform_light = HsProbeUniformHemisphere(sample);
        const float no_l_uniform = uniform_light.z;
        const float3 f0 = HsBsdfF0(surface);
        const float3 view_transmission = 1.0-HsBsdfSchlickFresnel(f0,no_v);
        const float3 light_transmission = 1.0-HsBsdfSchlickFresnel(f0,no_l_uniform);
        const float coat_exit_uniform = 1.0-coat_weight*HsBsdfCoatFresnel(no_l_uniform);
        const float3 diffuse_brdf = view_transmission*light_transmission*
            ((1.0-HsBsdfMetallic(surface))*surface.base_color)*HsBsdfInvPi;
        diffuse_integral += diffuse_brdf*(entry*coat_exit_uniform*no_l_uniform*(2.0*HsBsdfPi));
    }

    HsSurfaceClosure coat_surface=HsBsdfCoatSurface(surface);
    coat_surface.roughness=surface.coat_roughness;
    const float coat_alpha=HsBsdfAlpha(coat_surface);
    const float coat_alpha_squared=coat_alpha*coat_alpha;
    const float coat_view_denom=no_v+sqrt(coat_alpha_squared+
        (1.0-coat_alpha_squared)*no_v*no_v);
    [loop]
    for (uint sample_index=0u;sample_index<HsProbeCoatedReferenceSamples;++sample_index)
    {
        const float2 sample=HsProbeIndependentReferenceSample(sample_index);
        const float3 half_vector=HsBsdfSampleVisibleNormal(coat_surface,view,sample,tangent,bitangent,normal);
        const float view_half=saturate(dot(view,half_vector));
        if(view_half<=0.0) continue;
        const float3 light=normalize(reflect(-view,half_vector));
        const float no_l=dot(normal,light);
        if(no_l<=0.0) continue;
        const float no_h=saturate(dot(normal,half_vector));
        const float distribution=HsBsdfD(no_h,coat_alpha);
        const float light_denom=no_l+sqrt(coat_alpha_squared+
            (1.0-coat_alpha_squared)*no_l*no_l);
        const float spec_pdf=distribution/(2.0*coat_view_denom);
        const float fresnel=0.04+0.96*pow(1.0-view_half,5.0);
        const float brdf=distribution*fresnel/(coat_view_denom*light_denom);
        coat_integral+=coat_weight*brdf*(no_l/spec_pdf);
    }
    diffuse_reference = diffuse_integral/float(HsProbeCoatedReferenceSamples);
    return base_integral/float(HsProbeCoatedReferenceSamples)+
        coat_integral/float(HsProbeCoatedReferenceSamples);
}

float HsProbeCoatedKernelError(float coat_weight)
{
    const float3 kernel = HsBsdfCoatedDiffuseKernel(coat_weight);
    precise float3 moments = 0.0;
    precise float3 compensation = 0.0;
    [loop]
    for (uint index=0u;index<HsProbeCoatedReferenceSamples;++index)
    {
        const float z = (float(index) + 0.5) / float(HsProbeCoatedReferenceSamples);
        const float q = pow(1.0-z,5.0);
        const float coat_exit = 1.0-saturate(coat_weight)*(0.04+0.96*q);
        const float substrate_exit = 1.0-q;
        const float weighted = 2.0*z*substrate_exit*coat_exit;
        const float3 term=weighted*float3(1.0,z,z*z)-compensation;
        const float3 next=moments+term;
        compensation=(next-moments)-term;
        moments=next;
    }
    const float inverse = 1.0/float(HsProbeCoatedReferenceSamples);
    const float3 reference = float3(moments[0],1.5*moments[1],6.0*moments[2]-2.0*moments[0])*inverse;
    return HsProbeMax3(abs(reference-kernel));
}

float HsProbeCoatedEligibilityError()
{
    HsSurfaceClosure surface = HsMaterialMakeSurfaceClosure(float3(.5,.3,.2),float3(0,0,1),.5,0,1,1);
    surface.coat_weight=.4;
    float error = HsBsdfCanUseCoatedIbl(surface,1.0/1024.0) ? 0.0 : 1.0;
    error = max(error,HsBsdfCanUseCoatedIbl(surface,1.0/1024.0-1e-5) ? 1.0 : 0.0);
    surface.coat_weight=0.0; error=max(error,HsBsdfCanUseCoatedIbl(surface,.5)?1.0:0.0);
    surface.coat_weight=.4; surface.anisotropy=.01; error=max(error,HsBsdfCanUseCoatedIbl(surface,.5)?1.0:0.0);
    surface.anisotropy=0; surface.sheen_weight=.5; surface.sheen_color=.5;
    error=max(error,HsBsdfCanUseCoatedIbl(surface,.5)?1.0:0.0);
    surface.sheen_weight=0; surface.sheen_color=0; surface.transmission_weight=.1;
    error=max(error,HsBsdfCanUseCoatedIbl(surface,.5)?1.0:0.0);
    surface.transmission_weight=0; surface.unlit=true;
    error=max(error,HsBsdfCanUseCoatedIbl(surface,.5)?1.0:0.0);
    return error;
}

float HsProbeCoatZeroError(HsSurfaceClosure source, float no_v, float fresnel_override,
    float4 base_dfg)
{
    source.coat_weight=0.0;
    float3 diffuse,substrate; float coat;
    HsBsdfCoatedIblFactors(source,no_v,base_dfg,base_dfg.xy,fresnel_override,
        diffuse,substrate,coat);
    const float3 f0=HsBsdfF0(source);
    float3 expected=f0*base_dfg.x+base_dfg.y;
    if(fresnel_override>=0.0)
        expected=lerp(expected,saturate(fresnel_override).xxx*(base_dfg.x+base_dfg.y),
            saturate(source.solid_transmission_weight));
    const float3 expected_diffuse=(1.0-HsBsdfSchlickFresnel(f0,no_v))*(1.0-f0)*
        (1.0-HsBsdfMetallic(source))*source.base_color;
    return max(max(HsProbeMax3(abs(substrate-expected)),HsProbeMax3(abs(diffuse-expected_diffuse))),abs(coat));
}

float4 HsProbeCoatedIbl(uint case_index)
{
    const HsProbeCoatedCase test_case=HsProbeMakeCoatedCase(case_index);
    const float no_v=test_case.no_v;
    const float roughness=test_case.surface.roughness;
    const float lut_no_v=sqrt(saturate(no_v));
    const float4 base_dfg=EnvironmentBrdfLut.SampleLevel(ProbeBrdfClamp,float2(lut_no_v,roughness),0);
    const float2 coat_dfg=EnvironmentBrdfLut.SampleLevel(ProbeBrdfClamp,
        float2(lut_no_v,test_case.surface.coat_roughness),0).xy;
    float3 diffuse_factor,base_specular; float coat_specular;
    HsBsdfCoatedIblFactors(test_case.surface,no_v,base_dfg,coat_dfg,
        test_case.fresnel_override,diffuse_factor,base_specular,coat_specular);
    float3 reference_diffuse;
    const float3 reference_specular=HsProbeCoatedReference(test_case.surface,no_v,
        test_case.fresnel_override,reference_diffuse);
    const float3 kernel=HsBsdfCoatedDiffuseKernel(test_case.surface.coat_weight);
    const float3 actual_diffuse=diffuse_factor*kernel.x;
    const float kernel_error=HsProbeCoatedKernelError(test_case.surface.coat_weight);
    const float eligibility_error=HsProbeCoatedEligibilityError();
    const float coat_zero_error=HsProbeCoatZeroError(test_case.surface,no_v,
        test_case.fresnel_override,base_dfg);
    return float4(HsProbeMax3(abs(base_specular+coat_specular-reference_specular)),
        HsProbeMax3(abs(actual_diffuse-reference_diffuse)),kernel_error,
        max(eligibility_error,coat_zero_error));
}

[numthreads(6, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    const uint case_count = HsProbeTotalCaseCount;
    uint case_index = dispatch_thread_id.x;
    if (case_index >= case_count)
    {
        return;
    }

    if (case_index >= HsProbeLegacyCaseCount + HsProbeCoatedCaseCount)
    {
        // Raw production outputs; the host compares against independent double
        // pow applied to the rounded float32 subtraction. The last two cover
        // normalized-dot rounding outside [0,1] and coat's existing clamp.
        const uint bits[14] = {0u, 0x00800000u, 0x38d1b717u, 0x3c800000u,
            0x3e7fffffu, 0x3e800000u, 0x3e800001u,
            0x3effffffu, 0x3f000000u, 0x3f000001u,
            0x3f7fffffu, 0x3f800000u, 0xb4000000u, 0x3f800001u};
        const float cosine = asfloat(bits[case_index-HsProbeLegacyCaseCount-HsProbeCoatedCaseCount]);
        output[case_index] = float4(HsBsdfSchlickFresnel(float3(0.0,0.04,1.0),cosine),
            HsBsdfCoatFresnel(cosine));
        return;
    }
    if (case_index >= HsProbeLegacyCaseCount)
    {
        output[case_index]=HsProbeCoatedIbl(case_index-HsProbeLegacyCaseCount);
        return;
    }

    HsSurfaceClosure surface = HsProbeSurface(case_index);
    float no_v = case_index == 0 || case_index == 1 || case_index == 2 || case_index == 4
        ? 0.8
        : case_index == 3 || case_index == 5 ? 0.2 : 0.0001;
    float3 normal = surface.normal;
    float3 view = float3(sqrt(max(0.0, 1.0 - no_v * no_v)), 0.0, no_v);
    float pdf_integral = 0.0;
    float valid_count = 0.0;
    float energy = 0.0;
    float max_error = HsProbeLocalBlendError(case_index);
    max_error = max(max_error, HsProbePreparedLayeredRegressionError(case_index));

    HsBsdfSampleResult below_sample = HsBsdfSample(surface, -normal, 0.0, float2(0.25, 0.75));
    max_error = max(max_error, below_sample.valid ? 1.0 : 0.0);
    max_error = max(max_error, abs(below_sample.pdf));
    max_error = HsProbeMaxError(max_error, below_sample.f);
    max_error = max(max_error, HsBsdfPdf(surface, -normal, normal));
    max_error = HsProbeMaxError(max_error, HsBsdfEvaluate(surface, -normal, normal));

    [loop]
    for (uint index = 0; index < HsProbeSampleCount; ++index)
    {
        float2 sample = HsProbeHammersley(index);
        float3 uniform_direction = HsProbeUniformHemisphere(sample);
        float uniform_pdf = HsBsdfPdf(surface, view, uniform_direction);
        pdf_integral += uniform_pdf * HsProbeTwoPi;

        HsBsdfSampleResult sampled = HsBsdfSample(
            surface, view, HsProbeLobeCoordinate(index), sample);
        if (!sampled.valid)
        {
            continue;
        }

        valid_count += 1.0;
        float matched_pdf = HsBsdfPdf(surface, view, sampled.wi);
        float3 matched_f = HsBsdfEvaluate(surface, view, sampled.wi);
        max_error = max(max_error, abs(sampled.pdf - matched_pdf));
        max_error = HsProbeMaxError(max_error, sampled.f - matched_f);

        float3 swapped_f = HsBsdfEvaluate(surface, sampled.wi, view);
        max_error = HsProbeMaxError(max_error, matched_f - swapped_f);

        float cosine = saturate(dot(normal, sampled.wi));
        if (matched_pdf > 0.0 && cosine > 0.0)
        {
            energy += HsProbeMax3(matched_f) * cosine / matched_pdf;
        }
    }

    output[case_index] = float4(
        pdf_integral / float(HsProbeSampleCount),
        valid_count / float(HsProbeSampleCount),
        energy / float(HsProbeSampleCount),
        max_error);
}
