// Meter the pre-post, pre-UI SceneHDR in its active render rectangle. Scene
// radiance is pre-exposed; divide by E[current] before luminance measurement.
// Buffers contain [current,previous] and initialize both to 1 on view reset.
// Dispatch ClearHistogram(1,1,1), BuildHistogram(ceil(w/16),ceil(h/16),1),
// ReduceExposure(1,1,1), with UAV ordering between these graph operations.
// E[next] becomes E[current] only for the following render frame.
cbuffer HsExposureConstants : register(b0, space10)
{
    uint2 HsExposureActiveExtent;
    float HsExposureDeltaSeconds;
    uint HsExposureReset;
    float HsExposureMinLogLuminance;
    float HsExposureMaxLogLuminance;
    float HsExposureLowerPercentile;
    float HsExposureUpperPercentile;
    float HsExposureTargetGrey;
    float HsExposureMinimum;
    float HsExposureMaximum;
    float HsExposureAdaptationUp;
    float HsExposureAdaptationDown;
    float3 HsExposurePadding;
};

Texture2D<float4> HsExposureScene : register(t0, space10);
StructuredBuffer<float> HsExposureCurrent : register(t1, space10);
RWStructuredBuffer<float> HsExposureNext : register(u0, space10);
RWStructuredBuffer<uint> HsExposureHistogram : register(u1, space10);

groupshared uint HsExposureGroupHistogram[256];

[numthreads(256, 1, 1)]
void ClearHistogram(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (dispatch_id.x < 256) HsExposureHistogram[dispatch_id.x] = 0;
}

[numthreads(16, 16, 1)]
void BuildHistogram(uint3 dispatch_id : SV_DispatchThreadID, uint group_index : SV_GroupIndex)
{
    HsExposureGroupHistogram[group_index] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (all(dispatch_id.xy < HsExposureActiveExtent))
    {
        float current_exposure = max(HsExposureCurrent[0], 0.000001);
        float3 radiance = HsExposureScene.Load(int3(dispatch_id.xy, 0)).rgb / current_exposure;
        float luminance = dot(max(radiance, 0.0), float3(0.2126, 0.7152, 0.0722));
        if (isfinite(luminance))
        {
            // Bin zero represents black; the other 255 bins span log luminance.
            uint bin = 0;
            if (luminance > 0.0)
            {
                float log_luminance = log2(luminance);
                float normalized = saturate((log_luminance - HsExposureMinLogLuminance)
                    / (HsExposureMaxLogLuminance - HsExposureMinLogLuminance));
                bin = 1 + min(uint(normalized * 255.0), 254u);
            }
            InterlockedAdd(HsExposureGroupHistogram[bin], 1);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint count = HsExposureGroupHistogram[group_index];
    if (count != 0) InterlockedAdd(HsExposureHistogram[group_index], count);
}

[numthreads(1, 1, 1)]
void ReduceExposure(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (any(dispatch_id != 0)) return;
    HsExposureNext[1] = HsExposureCurrent[0];
    float current = clamp(HsExposureCurrent[0], HsExposureMinimum, HsExposureMaximum);
    uint population = 0;
    [loop] for (uint bin = 1; bin < 256; ++bin)
        population += HsExposureHistogram[bin];
    if (population == 0)
    {
        HsExposureNext[0] = current;
        return;
    }

    float lower_count = float(population) * HsExposureLowerPercentile;
    float upper_count = float(population) * HsExposureUpperPercentile;
    uint cumulative = 0;
    float weighted_log_luminance = 0.0;
    float retained_count = 0.0;
    [loop] for (uint bin = 1; bin < 256; ++bin)
    {
        uint count = HsExposureHistogram[bin];
        uint end = cumulative + count;
        // Percentile boundaries can fall inside a bin. Retain its fractional
        // population rather than dropping an entire dark/bright boundary bin.
        float retained = max(0.0, min(float(end), upper_count) - max(float(cumulative), lower_count));
        float bin_log_luminance = lerp(HsExposureMinLogLuminance, HsExposureMaxLogLuminance,
                                      (float(bin) - 0.5) / 255.0);
        weighted_log_luminance += bin_log_luminance * retained;
        retained_count += retained;
        cumulative = end;
    }
    if (retained_count <= 0.0)
    {
        HsExposureNext[0] = current;
        return;
    }

    float average_log_luminance = weighted_log_luminance / retained_count;
    float desired_log_exposure = clamp(log2(HsExposureTargetGrey) - average_log_luminance,
                                       log2(HsExposureMinimum), log2(HsExposureMaximum));
    float current_log_exposure = log2(current);
    float rate = desired_log_exposure > current_log_exposure
        ? HsExposureAdaptationUp : HsExposureAdaptationDown;
    float blend = HsExposureReset != 0 ? 1.0
        : 1.0 - exp(-max(rate, 0.0) * max(HsExposureDeltaSeconds, 0.0));
    HsExposureNext[0] = exp2(lerp(current_log_exposure, desired_log_exposure, blend));
}
