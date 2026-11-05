#ifndef HS_STATIC_VISIBILITY_ABI_HLSLI
#define HS_STATIC_VISIBILITY_ABI_HLSLI

#define HS_VISIBILITY_RANGE_FIELDS(X) \
    X(begin,Begin) X(count,Count) X(index_count,IndexCount) \
    X(argument_stride,ArgumentStride) X(draw_record_base,DrawRecordBase) \
    X(meshlet_count,MeshletCount) X(flags,Flags) X(reserved,Reserved)
#define HS_VISIBILITY_MESH_ELIGIBLE 2u
#define HS_VISIBILITY_LOD_FAMILY 4u
#define HS_VISIBILITY_EMPTY_SOURCE 8u
#define HS_VISIBILITY_STATIC_SOURCE 16u

#ifdef HS_STATIC_VISIBILITY_NATIVE
struct GpuStaticVisibilityRange
{
#define HS_VISIBILITY_FIELD(native,shader) std::uint32_t native{};
    HS_VISIBILITY_RANGE_FIELDS(HS_VISIBILITY_FIELD)
#undef HS_VISIBILITY_FIELD
};
inline constexpr auto kVisibilityMeshEligible = HS_VISIBILITY_MESH_ELIGIBLE;
inline constexpr auto kVisibilityLodFamily = HS_VISIBILITY_LOD_FAMILY;
inline constexpr auto kVisibilityEmptySource = HS_VISIBILITY_EMPTY_SOURCE;
inline constexpr auto kVisibilityStaticSource = HS_VISIBILITY_STATIC_SOURCE;
static_assert(sizeof(GpuStaticVisibilityRange) == 32);
#else
struct VisibilityRange
{
#define HS_VISIBILITY_FIELD(native,shader) uint shader;
    HS_VISIBILITY_RANGE_FIELDS(HS_VISIBILITY_FIELD)
#undef HS_VISIBILITY_FIELD
};
static const uint HsVisibilityMeshEligible = HS_VISIBILITY_MESH_ELIGIBLE;
static const uint HsVisibilityLodFamily = HS_VISIBILITY_LOD_FAMILY;
static const uint HsVisibilityEmptySource = HS_VISIBILITY_EMPTY_SOURCE;
static const uint HsVisibilityStaticSource = HS_VISIBILITY_STATIC_SOURCE;
#endif
#undef HS_VISIBILITY_RANGE_FIELDS
#endif
