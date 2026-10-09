#ifndef HS_DISPLAY_TRANSFORM_HLSLI
#define HS_DISPLAY_TRANSFORM_HLSLI

float3 HsSrgbDecode(float3 encoded)
{
    encoded = saturate(encoded);
    return float3(encoded.x <= 0.04045 ? encoded.x / 12.92 : pow((encoded.x + 0.055) / 1.055, 2.4),
                  encoded.y <= 0.04045 ? encoded.y / 12.92 : pow((encoded.y + 0.055) / 1.055, 2.4),
                  encoded.z <= 0.04045 ? encoded.z / 12.92 : pow((encoded.z + 0.055) / 1.055, 2.4));
}

float3 HsSrgbEncode(float3 display_linear)
{
    display_linear = saturate(display_linear);
    return float3(display_linear.x <= 0.0031308 ? display_linear.x * 12.92 : 1.055 * pow(display_linear.x, 1.0 / 2.4) - 0.055,
                  display_linear.y <= 0.0031308 ? display_linear.y * 12.92 : 1.055 * pow(display_linear.y, 1.0 / 2.4) - 0.055,
                  display_linear.z <= 0.0031308 ? display_linear.z * 12.92 : 1.055 * pow(display_linear.z, 1.0 / 2.4) - 0.055);
}

float3 HsLinearRec709ToRec2020(float3 color)
{
    // D65 primaries conversion, derived from the BT.709 and BT.2020
    // RGB-to-XYZ matrices. No transfer function belongs in this transform.
    return float3(dot(color, float3(0.627403896, 0.329283038, 0.043313066)),
                  dot(color, float3(0.069097289, 0.919540395, 0.011362316)),
                  dot(color, float3(0.016391439, 0.088013308, 0.895595253)));
}

float3 HsPqEncodeNits(float3 nits)
{
    // SMPTE ST 2084 inverse EOTF; ITU-R BT.2100 Table 4. PQ is absolute
    // luminance in cd/m^2 with 10,000 nits represented by encoded value 1.
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    float3 power = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * power) / (1.0 + c3 * power), m2);
}

float3 HsPqDecodeNits(float3 encoded)
{
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    float3 power = pow(saturate(encoded), 1.0 / m2);
    return 10000.0 * pow(max(power - c1, 0.0) / (c2 - c3 * power), 1.0 / m1);
}

float3 HsDisplayToneMapSdr(float3 scene)
{
    // Preserve the existing ACES fitted scene transform, returning display
    // linear Rec.709. Encoding is a separate final stage after linear UI.
    scene = max(scene, 0.0);
    return saturate((scene * (2.51 * scene + 0.03)) / (scene * (2.43 * scene + 0.59) + 0.14));
}

float3 HsDisplayToneMapHdr(float3 scene, float paper_white_nits, float peak_nits)
{
    // A luminance shoulder preserves Rec.709 chromaticity and approaches the
    // display peak. Output units remain 1=paper white, including UI composition.
    scene = max(scene, 0.0);
    float peak_relative = max(peak_nits, paper_white_nits) / paper_white_nits;
    float luminance = dot(scene, float3(0.2126, 0.7152, 0.0722));
    return scene / (1.0 + luminance / peak_relative);
}

float3 HsDisplayEncodeSdr(float3 display_linear)
{
    return HsSrgbEncode(display_linear);
}

float3 HsDisplayEncodeHdr10(float3 display_linear, float paper_white_nits, float peak_nits)
{
    float3 nits = max(HsLinearRec709ToRec2020(display_linear), 0.0) * paper_white_nits;
    float maximum = max(nits.x, max(nits.y, nits.z));
    float peak = min(max(peak_nits, paper_white_nits), 10000.0);
    // Rec.709 fits inside Rec.2020. Compress out-of-peak colors uniformly to
    // preserve their chromaticity rather than clipping channels independently.
    if (maximum > peak) nits *= peak / maximum;
    return HsPqEncodeNits(nits);
}

float4 HsDisplayDecodePremultipliedUi(float4 encoded_premultiplied)
{
    float alpha = saturate(encoded_premultiplied.a);
    if (alpha <= 0.0) return 0.0;
    // D2D/WIC stores alpha-premultiplied encoded RGB. Decode the unassociated
    // color before premultiplication; applying sRGB directly to premultiplied
    // channels produces dark fringes at antialiased text and translucent edges.
    return float4(HsSrgbDecode(encoded_premultiplied.rgb / alpha) * alpha, alpha);
}

#endif
