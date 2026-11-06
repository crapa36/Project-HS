#ifndef HS_TERRAIN_PAGE_ABI_HLSLI
#define HS_TERRAIN_PAGE_ABI_HLSLI

// Preserve the former 2048-slice terrain domain with three planes per tile.
#define HS_TERRAIN_PAGE_TILE_CAPACITY 682
#define HS_TERRAIN_PAGE_PLANE_CAPACITY (3 * HS_TERRAIN_PAGE_TILE_CAPACITY)

#define HS_TERRAIN_GPU_SECTION_FIELDS(F2, U1, U2) \
    F2(center) F2(span) F2(rotation) \
    U1(mesh) U1(seed) U1(pageSlice) U1(pageEdge) U2(tileCount)

#endif
