#ifndef HS_ENVIRONMENT_WIND_HLSLI
#define HS_ENVIRONMENT_WIND_HLSLI
// Bits 8..15 carry the archetype only for ground detail; hero animation uses the same field separately.
bool HsEnvironmentWind(uint packed_mesh)
{
    const uint mesh = packed_mesh & 255u;
    const uint variant = (packed_mesh >> 8u) & 255u;
    return mesh == 19u || (mesh == 21u && (variant <= 3u || variant == 7u));
}
#endif
