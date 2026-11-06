#ifndef HS_DECAL_SURFACE_ABI_HLSLI
#define HS_DECAL_SURFACE_ABI_HLSLI

#define HS_DECAL_SPACE 36
#define HS_DECAL_CONSTANT_REGISTER 0
#define HS_DECAL_TEXTURE_REGISTER_BASE 8
#define HS_DECAL_EXPORT_REGISTER_BASE 10
#define HS_DECAL_EMISSION_REGISTER 13
#define HS_DECAL_COMMAND_STRIDE 96
#define HS_DECAL_JOIN_INNER(a,b) a##b
#define HS_DECAL_JOIN(a,b) HS_DECAL_JOIN_INNER(a,b)

#define HS_DECAL_FIELDS(X) \
 X(Matrix,inverse_view_projection,DecalInverseViewProjection) \
 X(Float4,view_origin,DecalViewOrigin) \
 X(Float4,render_size,DecalRenderSize) \
 X(Uint4,info,DecalInfo) \
 X(Uint4Array,receivers,DecalReceivers) \
 X(Uint4,parameter_bases,DecalParameterBases)

// Generated material functions own t8/t9 declarations; the rows still own
// their native slots and graph reads.
#define HS_DECAL_SRVS(X) \
 X(Texture2D,Depth,0,float,DecalDepth,input.depth) \
 X(Texture2D,Metadata,1,uint,DecalMetadata,input.metadata) \
 X(Texture2D,Base,2,float4,DecalBase,input.base) \
 X(Texture2D,Normal,3,float2,DecalNormal,input.normal) \
 X(Texture2D,Material,4,float4,DecalMaterial,input.material) \
 X(ByteAddress,Commands,5,uint,DecalCommands,input.commands) \
 X(Texture2D,Gradient,6,float4,DecalGradient,input.gradient) \
 X(Structured,Parameters,7,uint4,MaterialParameters,input.parameters) \
 X(Generated,Texture0,8,float4,HsDecalTexture_0,input.textures[0]) \
 X(Generated,Texture1,9,float4,HsDecalTexture_1,input.textures[1])
#define HS_DECAL_UAVS(X) \
 X(Base,0,float4,DecalOutBase,DXGI_FORMAT_R8G8B8A8_UNORM) \
 X(Normal,1,float2,DecalOutNormal,DXGI_FORMAT_R16G16_SNORM) \
 X(Material,2,float4,DecalOutMaterial,DXGI_FORMAT_R8G8B8A8_UNORM) \
 X(Emission,3,float4,DecalOutEmission,DXGI_FORMAT_R16G16B16A16_FLOAT)

#ifdef HS_DECAL_NATIVE
#define HS_DECAL_FIELD_Matrix(name) DirectX::XMFLOAT4X4 name{};
#define HS_DECAL_FIELD_Float4(name) DirectX::XMFLOAT4 name{};
#define HS_DECAL_FIELD_Uint4(name) DirectX::XMUINT4 name{};
#define HS_DECAL_FIELD_Uint4Array(name) std::array<DirectX::XMUINT4,2> name{};
struct alignas(16) DecalSurfaceConstants {
#define HS_DECAL_FIELD(type,native,shader) HS_DECAL_FIELD_##type(native)
 HS_DECAL_FIELDS(HS_DECAL_FIELD)
#undef HS_DECAL_FIELD
};
enum DecalSurfaceSrvSlot : std::uint32_t {
#define HS_DECAL_SLOT(kind,name,slot,...) DecalSurfaceSrv##name = slot,
 HS_DECAL_SRVS(HS_DECAL_SLOT)
#undef HS_DECAL_SLOT
 DecalSurfaceSrvCount
};
enum DecalSurfaceUavSlot : std::uint32_t {
#define HS_DECAL_SLOT(name,slot,...) DecalSurfaceUav##name = slot,
 HS_DECAL_UAVS(HS_DECAL_SLOT)
#undef HS_DECAL_SLOT
 DecalSurfaceUavCount
};
inline constexpr std::uint32_t kDecalSurfaceCbvOffset = 0;
inline constexpr std::uint32_t kDecalSurfaceSrvOffset = kDecalSurfaceCbvOffset + 1;
inline constexpr std::uint32_t kDecalSurfaceSrvCount = DecalSurfaceSrvCount;
inline constexpr std::uint32_t kDecalSurfaceUavOffset = kDecalSurfaceSrvOffset + kDecalSurfaceSrvCount;
inline constexpr std::uint32_t kDecalSurfaceUavCount = DecalSurfaceUavCount;
inline constexpr std::uint32_t kDecalSurfaceExportOffset = kDecalSurfaceUavOffset + kDecalSurfaceUavCount;
inline constexpr std::uint32_t kDecalSurfaceExportCount = DecalSurfaceUavCount;
inline constexpr std::uint32_t kDecalSurfaceDescriptorCount = kDecalSurfaceExportOffset + kDecalSurfaceExportCount;
inline constexpr std::uint32_t kDecalSurfaceRootTable = 32;
inline constexpr std::uint32_t kDecalSurfaceCommandStride = HS_DECAL_COMMAND_STRIDE;
static_assert(sizeof(DecalSurfaceConstants) == 160);
static_assert(offsetof(DecalSurfaceConstants, receivers) == 112);
#undef HS_DECAL_FIELD_Matrix
#undef HS_DECAL_FIELD_Float4
#undef HS_DECAL_FIELD_Uint4
#undef HS_DECAL_FIELD_Uint4Array
#elif defined(HS_DECAL_SHADER)
#define HS_DECAL_FIELD_Matrix(name) float4x4 name;
#define HS_DECAL_FIELD_Float4(name) float4 name;
#define HS_DECAL_FIELD_Uint4(name) uint4 name;
#define HS_DECAL_FIELD_Uint4Array(name) uint4 name[2];
cbuffer DecalConstants : register(HS_DECAL_JOIN(b,HS_DECAL_CONSTANT_REGISTER), HS_DECAL_JOIN(space,HS_DECAL_SPACE)) {
#define HS_DECAL_FIELD(type,native,shader) HS_DECAL_FIELD_##type(shader)
 HS_DECAL_FIELDS(HS_DECAL_FIELD)
#undef HS_DECAL_FIELD
};
#define HS_DECAL_DECLARE_Texture2D(slot,type,name) Texture2D<type> name : register(HS_DECAL_JOIN(t,slot), HS_DECAL_JOIN(space,HS_DECAL_SPACE));
#define HS_DECAL_DECLARE_ByteAddress(slot,type,name) ByteAddressBuffer name : register(HS_DECAL_JOIN(t,slot), HS_DECAL_JOIN(space,HS_DECAL_SPACE));
#define HS_DECAL_DECLARE_Structured(slot,type,name) StructuredBuffer<type> name : register(HS_DECAL_JOIN(t,slot), HS_DECAL_JOIN(space,HS_DECAL_SPACE));
#define HS_DECAL_DECLARE_Generated(slot,type,name)
#define HS_DECAL_DECLARE(kind,name,slot,type,shader,...) HS_DECAL_DECLARE_##kind(slot,type,shader)
HS_DECAL_SRVS(HS_DECAL_DECLARE)
#undef HS_DECAL_DECLARE
#define HS_DECAL_DECLARE(name,slot,type,shader,...) RWTexture2D<type> shader : register(HS_DECAL_JOIN(u,slot), HS_DECAL_JOIN(space,HS_DECAL_SPACE));
HS_DECAL_UAVS(HS_DECAL_DECLARE)
#undef HS_DECAL_DECLARE
#undef HS_DECAL_DECLARE_Texture2D
#undef HS_DECAL_DECLARE_ByteAddress
#undef HS_DECAL_DECLARE_Structured
#undef HS_DECAL_DECLARE_Generated
#undef HS_DECAL_FIELD_Matrix
#undef HS_DECAL_FIELD_Float4
#undef HS_DECAL_FIELD_Uint4
#undef HS_DECAL_FIELD_Uint4Array
#endif
#endif
