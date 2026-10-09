#ifndef HS_TEMPORAL_RECONSTRUCTION_HLSLI
#define HS_TEMPORAL_RECONSTRUCTION_HLSLI

// Color textures contain working radiance: Cscene * current_pre_exposure.
// Motion is previous_unjittered_uv-current_unjittered_uv, independent of jitter.
// All render texture Loads are clamped to the active rectangle at origin zero;
// allocation capacity can exceed render_extent. History textures use output_extent.
// input.uv and stored output/history are unjittered logical coordinates. Only
// current render samples use current_jitter_uv; history motion is unjittered.
struct HsTemporalInput
{
    float2 uv;
    uint2 render_extent;
    uint2 output_extent;
    float2 current_jitter_uv;
    float2 previous_jitter_uv;
    float current_depth;
    float expected_previous_depth;
    float3 current_normal;
    uint2 material_key;
    float2 motion_uv;
    float history_confidence;
    float reactive;
    float current_pre_exposure;
    float previous_pre_exposure;
    bool history_valid;
    float3 current_camera_origin;
    row_major float4x4 current_inverse_unjittered_view_projection;
    row_major float4x4 previous_unjittered_view_projection;
};

struct HsTemporalResult
{
    float4 color_depth;
    float2 normal_oct;
    uint2 material_key;
};

float2 HsTemporalSignNotZero(float2 value)
{
    return float2(value.x < 0.0 ? -1.0 : 1.0, value.y < 0.0 ? -1.0 : 1.0);
}

float2 HsTemporalEncodeNormal(float3 normal)
{
    normal /= abs(normal.x) + abs(normal.y) + abs(normal.z);
    return normal.z < 0.0 ? (1.0 - abs(normal.yx)) * HsTemporalSignNotZero(normal.xy) : normal.xy;
}

float3 HsTemporalDecodeNormal(float2 oct)
{
    float3 normal = float3(oct, 1.0 - abs(oct.x) - abs(oct.y));
    if (normal.z < 0.0)
    {
        normal.xy = (1.0 - abs(normal.yx)) * HsTemporalSignNotZero(normal.xy);
    }
    return normalize(normal);
}

float4 HsTemporalCatmullRomWeights(float fraction)
{
    float square = fraction * fraction;
    float cube = square * fraction;
    return float4(-0.5 * fraction + square - 0.5 * cube,
                   1.0 - 2.5 * square + 1.5 * cube,
                   0.5 * fraction + 2.0 * square - 1.5 * cube,
                  -0.5 * square + 0.5 * cube);
}

float3 HsTemporalReconstructColor(Texture2D<float4> color, float2 uv, uint2 active_extent)
{
    float2 pixel = uv * float2(active_extent) - 0.5;
    int2 base_pixel = int2(floor(pixel));
    float2 fraction = frac(pixel);
    float4 weights_x = HsTemporalCatmullRomWeights(fraction.x);
    float4 weights_y = HsTemporalCatmullRomWeights(fraction.y);
    float3 reconstructed = 0.0;
    float3 minimum = 3.402823466e+38;
    float3 maximum = -3.402823466e+38;
    [unroll] for (int y = 0; y < 4; ++y)
    {
        [unroll] for (int x = 0; x < 4; ++x)
        {
            int2 coordinate = clamp(base_pixel + int2(x - 1, y - 1), int2(0, 0), int2(active_extent) - 1);
            float3 value = color.Load(int3(coordinate, 0)).rgb;
            reconstructed += value * weights_x[x] * weights_y[y];
            minimum = min(minimum, value);
            maximum = max(maximum, value);
        }
    }
    // Catmull-Rom's negative weights can ring around emissive silhouettes.
    // Bound the reconstructed radiance by the footprint used for that sample.
    return max(clamp(reconstructed, minimum, maximum), 0.0);
}

bool HsTemporalSkyPreviousUv(HsTemporalInput input, out float2 previous_uv)
{
    float2 unjittered_uv = input.uv;
    float4 world = mul(float4(unjittered_uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0),
                       input.current_inverse_unjittered_view_projection);
    float3 ray = abs(world.w) > 0.000001
        ? world.xyz / world.w - input.current_camera_origin : world.xyz;
    float4 previous_clip = mul(float4(ray, 0.0), input.previous_unjittered_view_projection);
    previous_uv = 0.0;
    if (!all(isfinite(previous_clip)) || previous_clip.w <= 0.0) return false;
    previous_uv = previous_clip.xy / previous_clip.w * float2(0.5, -0.5) + 0.5;
    return all(isfinite(previous_uv));
}

HsTemporalResult HsResolveTemporal(
    HsTemporalInput input, Texture2D<float4> scene_color,
    Texture2D<float4> history_color_depth, Texture2D<float2> history_normal_oct,
    Texture2D<uint2> history_material, SamplerState linear_clamp)
{
    HsTemporalResult result = (HsTemporalResult)0;
    bool surface_valid = input.current_depth > 0.0;
    float2 current_render_uv = input.uv + input.current_jitter_uv;
    float3 current = HsTemporalReconstructColor(scene_color, current_render_uv, input.render_extent);
    result.color_depth = float4(current, surface_valid ? input.current_depth : 0.0);
    result.normal_oct = surface_valid ? HsTemporalEncodeNormal(normalize(input.current_normal)) : 0.0;
    result.material_key = surface_valid ? input.material_key : uint2(0, 0);
    if (!input.history_valid || input.previous_pre_exposure <= 0.0 ||
        input.current_pre_exposure <= 0.0 ||
        (surface_valid && input.history_confidence <= 0.0)) return result;

    float2 previous_uv;
    if (surface_valid)
    {
        previous_uv = input.uv + input.motion_uv;
    }
    else if (!HsTemporalSkyPreviousUv(input, previous_uv))
    {
        return result;
    }
    float2 half_texel = 0.5 / float2(input.output_extent);
    if (!all(isfinite(previous_uv)) || any(previous_uv < half_texel) || any(previous_uv > 1.0 - half_texel))
        return result;
    int2 previous_pixel = min(int2(previous_uv * float2(input.output_extent)), int2(input.output_extent) - 1);
    float history_depth = history_color_depth.Load(int3(previous_pixel, 0)).a;
    if (surface_valid)
    {
        if (history_depth <= 0.0 || abs(history_depth - input.expected_previous_depth) > 0.002)
            return result;
        uint2 previous_material = history_material.Load(int3(previous_pixel, 0));
        if (any(previous_material != input.material_key)) return result;
        float3 previous_normal = HsTemporalDecodeNormal(history_normal_oct.Load(int3(previous_pixel, 0)));
        if (dot(normalize(input.current_normal), previous_normal) < 0.85) return result;
    }
    else if (history_depth > 0.0)
    {
        return result;
    }

    float3 history = history_color_depth.SampleLevel(linear_clamp, previous_uv, 0).rgb
        * (input.current_pre_exposure / input.previous_pre_exposure);
    if (!all(isfinite(history))) return result;
    int2 current_pixel = clamp(int2(current_render_uv * float2(input.render_extent)),
        int2(0, 0), int2(input.render_extent) - 1);
    float3 minimum = current;
    float3 maximum = current;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            int2 coordinate = clamp(current_pixel + int2(x, y), int2(0, 0), int2(input.render_extent) - 1);
            float3 neighbor = scene_color.Load(int3(coordinate, 0)).rgb;
            minimum = min(minimum, neighbor);
            maximum = max(maximum, neighbor);
        }
    }
    float3 clipped_history = clamp(history, minimum, maximum);
    float change = saturate(length(current - clipped_history) / max(1.0, length(current)));
    float confidence = surface_valid ? saturate(input.history_confidence) : 1.0;
    float history_weight = 0.88 * confidence * (1.0 - saturate(input.reactive)) * (1.0 - change);
    result.color_depth.rgb = lerp(current, clipped_history, history_weight);
    return result;
}

// Optional output-space sharpening is separate from reconstruction so provider
// sharpening and this stage cannot both be enabled by the same resolved plan.
float3 HsSharpenTemporal(Texture2D<float4> reconstructed, SamplerState linear_clamp,
                        float2 uv, uint2 output_extent, float strength)
{
    float2 texel = 1.0 / float2(output_extent);
    float3 center = reconstructed.SampleLevel(linear_clamp, uv, 0).rgb;
    float3 north = reconstructed.SampleLevel(linear_clamp, uv + float2(0.0, -texel.y), 0).rgb;
    float3 south = reconstructed.SampleLevel(linear_clamp, uv + float2(0.0, texel.y), 0).rgb;
    float3 east = reconstructed.SampleLevel(linear_clamp, uv + float2(texel.x, 0.0), 0).rgb;
    float3 west = reconstructed.SampleLevel(linear_clamp, uv + float2(-texel.x, 0.0), 0).rgb;
    float3 minimum = min(center, min(min(north, south), min(east, west)));
    float3 maximum = max(center, max(max(north, south), max(east, west)));
    float3 sharpened = center + saturate(strength) * (center - 0.25 * (north + south + east + west));
    return max(clamp(sharpened, minimum, maximum), 0.0);
}

#endif
