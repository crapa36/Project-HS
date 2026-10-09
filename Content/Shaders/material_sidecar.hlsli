#ifndef HS_MATERIAL_SIDECAR_HLSLI
#define HS_MATERIAL_SIDECAR_HLSLI

#include "material_surface.hlsli"

// Consumer-selected packed layout. Family bits T/C/S/Thin/E are 1/2/4/8/16;
// their word counts are 2/1/2/2/1. offsets0.xyzw locate the first four families,
// offsets1.x locates Emission. The CPU layout supplies valid selected offsets
// in [0,7]; unselected offsets are unused. A family may cross the plane boundary.
// Precision is lossy: tangent oct coordinates and scalar fields use binary16,
// saturated tints use RGB10 UNORM, and positive HDR radiance uses R11G11B10
// UFLOAT (5 exponent bits, 6/6/5 fraction bits). R/G saturate at 65024, B at
// 64512. Values below the smallest subnormal round to zero. Pack finite authored
// closure values; source validation is the caller's responsibility.

void HsSidecarStoreWord(uint index, uint word, inout uint4 plane0, inout uint4 plane1)
{
    if (index < 4u) plane0[index] = word;
    else if (index < 8u) plane1[index - 4u] = word;
}

uint HsSidecarLoadWord(uint index, uint4 plane0, uint4 plane1)
{
    if (index < 4u) return plane0[index];
    if (index < 8u) return plane1[index - 4u];
    return 0u;
}

uint HsSidecarPackHalf2(float2 value)
{
    uint2 halves = f32tof16(value) & 0xffffu;
    return halves.x | (halves.y << 16u);
}

float2 HsSidecarUnpackHalf2(uint word)
{
    return f16tof32(uint2(word & 0xffffu, word >> 16u));
}

float2 HsSidecarSignNotZero(float2 value)
{
    return float2(value.x >= 0.0 ? 1.0 : -1.0, value.y >= 0.0 ? 1.0 : -1.0);
}

float2 HsSidecarEncodeTangent(float3 world_unit_tangent)
{
    float3 oct = world_unit_tangent /
        (abs(world_unit_tangent.x) + abs(world_unit_tangent.y) + abs(world_unit_tangent.z));
    return oct.z >= 0.0 ? oct.xy :
        (1.0 - abs(oct.yx)) * HsSidecarSignNotZero(oct.xy);
}

float3 HsSidecarDecodeTangent(float2 oct)
{
    float3 tangent = float3(oct, 1.0 - abs(oct.x) - abs(oct.y));
    if (tangent.z < 0.0)
        tangent.xy = (1.0 - abs(tangent.yx)) * HsSidecarSignNotZero(tangent.xy);
    return normalize(tangent);
}

uint HsSidecarPackTint(float3 tint)
{
    uint3 rgb = (uint3)round(saturate(tint) * 1023.0);
    return rgb.x | (rgb.y << 10u) | (rgb.z << 20u);
}

float3 HsSidecarUnpackTint(uint word)
{
    return float3(word & 1023u, (word >> 10u) & 1023u, (word >> 20u) & 1023u) / 1023.0;
}

uint HsSidecarRoundShiftEven(uint significand, uint shift)
{
    // The binary32 significand has at most 24 bits. Larger shifts are below
    // half of the destination's smallest subnormal, and therefore round to zero.
    if (shift > 24u) return 0u;
    if (shift == 0u) return significand;
    uint integral = significand >> shift;
    uint remainder = significand & ((1u << shift) - 1u);
    uint halfway = 1u << (shift - 1u);
    return integral + ((remainder > halfway ||
        (remainder == halfway && (integral & 1u) != 0u)) ? 1u : 0u);
}

uint HsSidecarPackUnsignedFloat(float value, uint fraction_bits)
{
    // R11G11B10 exponent rebias/finite saturation follows the representation
    // used by DirectXMath XMStoreFloat3PK. Rounding operates on the original
    // significand, retaining sticky bits across subnormal conversion.
    // Reference: https://github.com/microsoft/DirectXMath/blob/main/Inc/DirectXPackedVector.inl
    uint bits = asuint(value);
    if ((bits & 0x80000000u) != 0u) return 0u;
    uint maximum = (30u << fraction_bits) | ((1u << fraction_bits) - 1u);
    uint maximum_float = fraction_bits == 6u ? 0x477e0000u : 0x477c0000u;
    if (bits >= maximum_float) return maximum;
    uint exponent = bits >> 23u;
    if (exponent == 0u) return 0u;
    uint fraction = bits & 0x7fffffu;
    if (exponent < 113u)
        return HsSidecarRoundShiftEven(0x800000u | fraction, 136u - fraction_bits - exponent);
    return ((exponent - 112u) << fraction_bits) +
        HsSidecarRoundShiftEven(fraction, 23u - fraction_bits);
}

float HsSidecarUnpackUnsignedFloat(uint word, uint fraction_bits)
{
    uint fraction = word & ((1u << fraction_bits) - 1u);
    uint exponent = word >> fraction_bits;
    if (exponent == 0u)
        return (float)fraction * exp2(-14.0 - (float)fraction_bits);
    // The finite saturating packer never emits exponent 31, but decode the
    // format's Inf/NaN encodings faithfully when supplied by another producer.
    if (exponent == 31u)
        return asfloat(0x7f800000u | (fraction << (23u - fraction_bits)));
    return asfloat(((exponent + 112u) << 23u) | (fraction << (23u - fraction_bits)));
}

uint HsSidecarPackEmission(float3 radiance)
{
    return HsSidecarPackUnsignedFloat(radiance.x, 6u) |
        (HsSidecarPackUnsignedFloat(radiance.y, 6u) << 11u) |
        (HsSidecarPackUnsignedFloat(radiance.z, 5u) << 22u);
}

float3 HsSidecarUnpackEmission(uint word)
{
    return float3(HsSidecarUnpackUnsignedFloat(word & 0x7ffu, 6u),
        HsSidecarUnpackUnsignedFloat((word >> 11u) & 0x7ffu, 6u),
        HsSidecarUnpackUnsignedFloat((word >> 22u) & 0x3ffu, 5u));
}

void HsPackMaterialSidecars(HsSurfaceClosure surface, uint mask,
    uint4 offsets0, uint4 offsets1, out uint4 plane0, out uint4 plane1)
{
    plane0 = 0u;
    plane1 = 0u;
    if ((mask & 1u) != 0u)
    {
        // The surface producer has already resolved authored tangent space to
        // a world-space unit tangent before reaching this storage boundary.
        HsSidecarStoreWord(offsets0.x, HsSidecarPackHalf2(HsSidecarEncodeTangent(surface.tangent)), plane0, plane1);
        HsSidecarStoreWord(offsets0.x + 1u, HsSidecarPackHalf2(float2(clamp(surface.anisotropy, -1.0, 1.0), 0.0)), plane0, plane1);
    }
    if ((mask & 2u) != 0u)
        HsSidecarStoreWord(offsets0.y, HsSidecarPackHalf2(saturate(float2(surface.coat_weight, surface.coat_roughness))), plane0, plane1);
    if ((mask & 4u) != 0u)
    {
        HsSidecarStoreWord(offsets0.z, HsSidecarPackTint(surface.sheen_color), plane0, plane1);
        HsSidecarStoreWord(offsets0.z + 1u, HsSidecarPackHalf2(saturate(float2(surface.sheen_weight, surface.sheen_roughness))), plane0, plane1);
    }
    if ((mask & 8u) != 0u)
    {
        HsSidecarStoreWord(offsets0.w, HsSidecarPackTint(surface.transmission_color), plane0, plane1);
        HsSidecarStoreWord(offsets0.w + 1u, HsSidecarPackHalf2(float2(saturate(surface.transmission_weight), 0.0)), plane0, plane1);
    }
    if ((mask & 16u) != 0u)
    {
        float3 radiance = surface.emission;
        if (surface.unlit) radiance += surface.base_color;
        HsSidecarStoreWord(offsets1.x, HsSidecarPackEmission(radiance), plane0, plane1);
    }
}

HsSurfaceClosure HsApplyMaterialSidecars(HsSurfaceClosure base, uint mask,
    uint4 offsets0, uint4 offsets1, uint4 plane0, uint4 plane1)
{
    if ((mask & 1u) != 0u)
    {
        base.tangent = HsSidecarDecodeTangent(HsSidecarUnpackHalf2(HsSidecarLoadWord(offsets0.x, plane0, plane1)));
        base.anisotropy = HsSidecarUnpackHalf2(HsSidecarLoadWord(offsets0.x + 1u, plane0, plane1)).x;
    }
    if ((mask & 2u) != 0u)
    {
        float2 coat = HsSidecarUnpackHalf2(HsSidecarLoadWord(offsets0.y, plane0, plane1));
        base.coat_weight = coat.x;
        base.coat_roughness = coat.y;
    }
    if ((mask & 4u) != 0u)
    {
        base.sheen_color = HsSidecarUnpackTint(HsSidecarLoadWord(offsets0.z, plane0, plane1));
        float2 sheen = HsSidecarUnpackHalf2(HsSidecarLoadWord(offsets0.z + 1u, plane0, plane1));
        base.sheen_weight = sheen.x;
        base.sheen_roughness = sheen.y;
    }
    if ((mask & 8u) != 0u)
    {
        base.transmission_color = HsSidecarUnpackTint(HsSidecarLoadWord(offsets0.w, plane0, plane1));
        base.transmission_weight = HsSidecarUnpackHalf2(HsSidecarLoadWord(offsets0.w + 1u, plane0, plane1)).x;
    }
    if ((mask & 16u) != 0u)
    {
        base.emission = HsSidecarUnpackEmission(HsSidecarLoadWord(offsets1.x, plane0, plane1));
        if (base.unlit) base.base_color = 0.0;
    }
    return base;
}

#endif
