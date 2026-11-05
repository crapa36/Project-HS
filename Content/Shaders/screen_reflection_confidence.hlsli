// Confidence weights RGB once, before filtering; null rays still count in N.
float4 HsSsrContribution(float3 radiance, float confidence)
{
    return float4(radiance * confidence, confidence);
}

float4 HsSsrLimitHistory(float4 reflection, float currentConfidence)
{
    float confidence = min(reflection.a, currentConfidence);
    reflection.rgb *= reflection.a > 0.0 ? confidence / reflection.a : 0.0;
    reflection.a = confidence;
    return reflection;
}

float3 HsSsrCompose(float3 opaque, float3 environment, float4 reflection)
{
    return opaque - environment * saturate(reflection.a) + reflection.rgb;
}
