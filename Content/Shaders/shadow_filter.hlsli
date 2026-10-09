#ifndef HS_SHADOW_FILTER_HLSLI
#define HS_SHADOW_FILTER_HLSLI
static const int2 HsShadowFilterOffsets[16] = {
    int2(-1,-1), int2(1,1), int2(-1,1), int2(1,-1),
    int2(0,-1), int2(0,1), int2(-1,0), int2(1,0),
    int2(-2,-1), int2(2,1), int2(-1,2), int2(1,-2),
    int2(-2,1), int2(2,-1), int2(1,2), int2(-1,-2)};
#endif
