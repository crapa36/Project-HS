#ifndef HS_TERRAIN_PAGE_ABI_HLSLI
#define HS_TERRAIN_PAGE_ABI_HLSLI

// Preserve the former 2048-slice terrain domain with three planes per tile.
#define HS_TERRAIN_PAGE_TILE_CAPACITY 682
#define HS_TERRAIN_PAGE_PLANE_CAPACITY (3 * HS_TERRAIN_PAGE_TILE_CAPACITY)
#define HS_TERRAIN_PAGE_SPACE 42
#define HS_TERRAIN_SECTION_SRV 0
#define HS_TERRAIN_MIP_SRV 1
#define HS_TERRAIN_PLANE_SRV 2
#define HS_TERRAIN_DESCRIPTOR_COUNT (HS_TERRAIN_PLANE_SRV + HS_TERRAIN_PAGE_PLANE_CAPACITY)
#define HS_TERRAIN_JOIN_INNER(a,b) a##b
#define HS_TERRAIN_JOIN(a,b) HS_TERRAIN_JOIN_INNER(a,b)

#define HS_TERRAIN_GPU_SECTION_FIELDS(F2, U1, U2) \
    F2(center) F2(span) F2(rotation) \
    U1(mesh) U1(seed) U1(pageSlice) U1(pageEdge) U2(tileCount)

#endif
