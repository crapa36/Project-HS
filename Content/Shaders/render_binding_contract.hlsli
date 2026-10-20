#ifndef HS_RENDER_BINDING_CONTRACT_HLSLI
#define HS_RENDER_BINDING_CONTRACT_HLSLI

// Producers obtain these definitions from the public RenderData catalog.
#if !defined(HS_RENDER_AUTHORED_MATERIAL_COUNT) || !defined(HS_RENDER_HERO_MATERIAL_COUNT) || \
    !defined(HS_RENDER_ENVIRONMENT_GEOMETRY_COUNT) || !defined(HS_RENDER_ENVIRONMENT_GEOMETRY_LOD_COUNT) || \
    !defined(HS_RENDER_MATERIAL_TEXTURE_SLOT_COUNT) || !defined(HS_RENDER_GROUND_DETAIL_GEOMETRY_BEGIN) || \
    !defined(HS_RENDER_GROUND_DETAIL_MATERIAL_ORDINAL) || \
    !defined(HS_RENDER_GEOMETRY_PAGE_CAPACITY) || !defined(HS_RENDER_GEOMETRY_BUFFERS_PER_PAGE)
#error Missing RenderData binding definitions; compile through the shared producer contract.
#endif

static const uint HsGeometryPageCapacity = HS_RENDER_GEOMETRY_PAGE_CAPACITY;
static const uint HsGeometryDescriptorCount = 1u + HS_RENDER_GEOMETRY_PAGE_CAPACITY * HS_RENDER_GEOMETRY_BUFFERS_PER_PAGE;
static const uint HsAuthoredMaterialCount = HS_RENDER_AUTHORED_MATERIAL_COUNT;
static const uint HsHeroMaterialCount = HS_RENDER_HERO_MATERIAL_COUNT;
static const uint HsHeroVisibilityVariantBegin = 1u + HsAuthoredMaterialCount;
static const uint HsVisibilityVariantCount = HsHeroVisibilityVariantBegin + HsHeroMaterialCount;
static const uint HsEnvironmentGeometryLodCount = HS_RENDER_ENVIRONMENT_GEOMETRY_LOD_COUNT;
static const uint HsEnvironmentGeometryPageCount = HS_RENDER_ENVIRONMENT_GEOMETRY_COUNT * HS_RENDER_ENVIRONMENT_GEOMETRY_LOD_COUNT;

uint HsEnvironmentMaterialOrdinal(uint geometry_ordinal)
{
    return geometry_ordinal < HS_RENDER_GROUND_DETAIL_GEOMETRY_BEGIN
        ? geometry_ordinal : geometry_ordinal == HS_RENDER_ENVIRONMENT_GEOMETRY_COUNT-2u ? 14u : HS_RENDER_GROUND_DETAIL_MATERIAL_ORDINAL;
}

#endif
