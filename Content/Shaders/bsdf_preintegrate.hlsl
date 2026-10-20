#include "bsdf_metallic_roughness.hlsli"

RWTexture2D<float2> output : register(u0);

uint HsPreintegrateReverseBits(uint bits)
{
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
    bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
    bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
    return bits;
}

float2 HsPreintegrateHammersley(uint index, uint sample_count)
{
    float inverse_count = 1.0 / float(sample_count);
    return float2((float(index) + 0.5) * inverse_count,
                  float(HsPreintegrateReverseBits(index)) * 2.3283064365386962890625e-10);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    uint width;
    uint height;
    output.GetDimensions(width, height);
    if (dispatch_thread_id.x >= width || dispatch_thread_id.y >= height)
    {
        return;
    }

    const uint sample_count = 128;
    float no_v = (float(dispatch_thread_id.x) + 0.5) / float(width);
    float roughness = (float(dispatch_thread_id.y) + 0.5) / float(height);
    float3 normal = float3(0.0, 0.0, 1.0);
    float3 view = float3(sqrt(max(0.0, 1.0 - no_v * no_v)), 0.0, no_v);

    HsSurfaceClosure surface = (HsSurfaceClosure)0;
    surface.base_color = float3(1.0, 1.0, 1.0);
    surface.normal = normal;
    surface.roughness = roughness;
    surface.metallic = 1.0;
    surface.ao = 1.0;
    surface.opacity = 1.0;
    surface.normal_is_world = true;

    float2 accumulated = float2(0.0, 0.0);
    [loop]
    for (uint sample_index = 0; sample_index < sample_count; ++sample_index)
    {
        float2 sample_direction = HsPreintegrateHammersley(sample_index, sample_count);
        HsBsdfSampleResult sample = HsBsdfSample(surface, view, 0.0, sample_direction);
        if (!sample.valid)
        {
            continue;
        }

        float pdf = HsBsdfPdf(surface, view, sample.wi);
        float cosine = saturate(dot(normal, sample.wi));
        if (pdf <= 0.0 || cosine <= 0.0)
        {
            continue;
        }

        float3 half_vector_sum = view + sample.wi;
        float half_vector_length_squared = dot(half_vector_sum, half_vector_sum);
        if (half_vector_length_squared <= 0.0)
        {
            continue;
        }
        float3 half_vector = half_vector_sum * rsqrt(half_vector_length_squared);
        float fresnel_weight = pow(1.0 - saturate(dot(view, half_vector)), 5.0);
        float weighted_brdf = sample.f.x * cosine / pdf;
        accumulated += weighted_brdf * float2(1.0 - fresnel_weight, fresnel_weight);
    }

    output[dispatch_thread_id.xy] = accumulated / float(sample_count);
}
