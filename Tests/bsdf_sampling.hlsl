#include "../Content/Shaders/bsdf_metallic_roughness.hlsli"

RWStructuredBuffer<float4> output : register(u0);

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

float HsProbeLobeCoordinate(uint index)
{
    return frac((float(index) + 0.5) * 0.6180339887498948482);
}

[numthreads(6, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    const uint case_count = 7;
    uint case_index = dispatch_thread_id.x;
    if (case_index >= case_count)
    {
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
    float max_error = 0.0;

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
