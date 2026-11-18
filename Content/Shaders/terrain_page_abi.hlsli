#ifndef HS_TERRAIN_PAGE_ABI_HLSLI
#define HS_TERRAIN_PAGE_ABI_HLSLI

// Current terrain codec capacity is 64 tiles with three planes per tile.
#define HS_TERRAIN_PAGE_TILE_CAPACITY 64
#define HS_TERRAIN_PAGE_PLANE_CAPACITY (3 * HS_TERRAIN_PAGE_TILE_CAPACITY)
#define HS_TERRAIN_PAGE_SPACE 42
#define HS_TERRAIN_SECTION_SRV 0
#define HS_TERRAIN_MIP_SRV 1
#define HS_TERRAIN_PLANE_SRV 2
#define HS_TERRAIN_SRV_DESCRIPTOR_COUNT (HS_TERRAIN_PLANE_SRV + HS_TERRAIN_PAGE_PLANE_CAPACITY)
#define HS_TERRAIN_REQUEST_UAV 0
#define HS_TERRAIN_REQUEST_DESCRIPTOR HS_TERRAIN_SRV_DESCRIPTOR_COUNT
#define HS_TERRAIN_REQUEST_MIP_BITS 16
#define HS_TERRAIN_DESCRIPTOR_COUNT (HS_TERRAIN_REQUEST_DESCRIPTOR + 1)
#define HS_TERRAIN_JOIN_INNER(a,b) a##b
#define HS_TERRAIN_JOIN(a,b) HS_TERRAIN_JOIN_INNER(a,b)

#define HS_TERRAIN_GPU_SECTION_FIELDS(F2, U1, U2) \
    F2(center) F2(span) F2(rotation) \
    U1(mesh) U1(seed) U1(pageSlice) U1(pageEdge) U2(tileCount) F2(heightTransform)

// qualityMipPlusOne is zero until the plane is available to this frame.
#define HS_TERRAIN_GPU_PLANE_MIP_FIELDS(U1) \
    U1(firstMip) U1(residentMinMip) U1(mipCount) U1(qualityMipPlusOne)

#if !defined(__cplusplus) && !defined(HS_TERRAIN_PAGE_BAKE)
struct HsTerrainPageSection
{
#define HS_TERRAIN_F2(name) float2 name;
#define HS_TERRAIN_U1(name) uint name;
#define HS_TERRAIN_U2(name) uint2 name;
    HS_TERRAIN_GPU_SECTION_FIELDS(HS_TERRAIN_F2, HS_TERRAIN_U1, HS_TERRAIN_U2)
#undef HS_TERRAIN_F2
#undef HS_TERRAIN_U1
#undef HS_TERRAIN_U2
};
StructuredBuffer<HsTerrainPageSection> HsTerrainSections : register(HS_TERRAIN_JOIN(t,HS_TERRAIN_SECTION_SRV),HS_TERRAIN_JOIN(space,HS_TERRAIN_PAGE_SPACE));
#endif

#endif
