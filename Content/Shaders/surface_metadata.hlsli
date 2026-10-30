#ifndef HS_SURFACE_METADATA_HLSLI
#define HS_SURFACE_METADATA_HLSLI

static const uint HsSurfaceFlagValid = 1u;
static const uint HsSurfaceModelShift = 1u;
static const uint HsSurfaceModelMask = 0xfeu;
static const uint HsSurfaceModelCel = 1u;
static const uint HsSurfaceModelEnvironment = 2u;
static const uint HsSurfaceModelJelly = 3u;
static const uint HsSurfaceFoliageMask = 3u << 8u;
static const uint HsSurfaceFoliageLeaf = 1u << 8u;
static const uint HsSurfaceFoliageGrass = 2u << 8u;
static const uint HsSurfaceFlagThinSheet = 1u << 10u;
static const uint HsSurfaceOutlineShift = 16u;

uint HsPackSurfaceFlags(uint model, uint foliage, uint outline_group)
{
    return HsSurfaceFlagValid | (model << HsSurfaceModelShift) | foliage |
        (foliage != 0u ? HsSurfaceFlagThinSheet : 0u) |
        (outline_group << HsSurfaceOutlineShift);
}

uint HsSurfaceModel(uint2 metadata)
{
    return (metadata.y & HsSurfaceModelMask) >> HsSurfaceModelShift;
}

uint HsSurfaceOutlineGroup(uint2 metadata)
{
    return metadata.y >> HsSurfaceOutlineShift;
}

static const uint HsSurfaceMetadataMaterialMask = 0x0fffffffu;

// ABI 2 relocates the original namespace nibble into physical bits 24-27.

uint HsSurfaceMetadataTuple(uint flags)
{
    if (flags == (HsSurfaceFlagValid | (HsSurfaceModelCel << HsSurfaceModelShift))) return 1u;
    if (flags == (HsSurfaceFlagValid | (HsSurfaceModelCel << HsSurfaceModelShift) | (1u << HsSurfaceOutlineShift))) return 2u;
    if (flags == (HsSurfaceFlagValid | (HsSurfaceModelJelly << HsSurfaceModelShift))) return 3u;
    if (flags == (HsSurfaceFlagValid | (HsSurfaceModelEnvironment << HsSurfaceModelShift))) return 4u;
    if (flags == (HsSurfaceFlagValid | (HsSurfaceModelEnvironment << HsSurfaceModelShift) |
                   HsSurfaceFoliageLeaf | HsSurfaceFlagThinSheet)) return 5u;
    if (flags == (HsSurfaceFlagValid | (HsSurfaceModelEnvironment << HsSurfaceModelShift) |
                   HsSurfaceFoliageGrass | HsSurfaceFlagThinSheet)) return 6u;
    return 0u;
}

uint2 HsUnpackSurfaceMetadata(uint packed)
{
    const uint tuple = (packed >> 28u) & 0x7u;
    uint flags = 0u;
    if (tuple == 1u) flags = 3u;
    else if (tuple == 2u) flags = 65539u;
    else if (tuple == 3u) flags = 7u;
    else if (tuple == 4u) flags = 5u;
    else if (tuple == 5u) flags = 1285u;
    else if (tuple == 6u) flags = 1541u;
    const uint material = (packed & 0x00ffffffu) | ((packed & 0x0f000000u) << 4u);
    return uint2(material, flags);
}

uint HsPackSurfaceMetadata(uint2 logical)
{
    return (logical.x & 0x00ffffffu) | ((logical.x >> 4u) & 0x0f000000u) |
        (HsSurfaceMetadataTuple(logical.y) << 28u);
}

#endif
