#ifndef HS_SHADOW_FILTER_HLSLI
#define HS_SHADOW_FILTER_HLSLI
SamplerState LinearClamp : register(s0);
static const int2 HsShadowFilterOffsets[16] = {
    int2(-1,-1), int2(1,1), int2(-1,1), int2(1,-1),
    int2(0,-1), int2(0,1), int2(-1,0), int2(1,0),
    int2(-2,-1), int2(2,1), int2(-1,2), int2(1,-2),
    int2(-2,1), int2(2,-1), int2(1,2), int2(-1,-2)};
// XY: 2x2 gather's upper-left offset; Z/W: active xyzw lanes for 8/12 taps.
// The six-entry suffix covers 16 taps with its Z masks.
static const int4 HsShadowGatherFootprints[10] = {
    int4(-2,-1,6,14), int4(1,0,9,11), int4(-1,1,12,13), int4(0,-2,3,7),
    int4(-2,-2,7,0), int4(0,0,7,0), int4(-2,1,14,0),
    int4(1,-2,11,0), int4(-1,-1,5,0), int4(1,1,5,0)};
#endif
