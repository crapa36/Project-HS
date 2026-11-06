#ifndef HS_SCREEN_REFLECTIONS_ABI_HLSLI
#define HS_SCREEN_REFLECTIONS_ABI_HLSLI

#define HS_SSR_SPACE 16
#define HS_SSR_CONSTANT_REGISTER 0
#define HS_SSR_PASS_REGISTER 1
#define HS_SSR_JOIN_INNER(a,b) a##b
#define HS_SSR_JOIN(a,b) HS_SSR_JOIN_INNER(a,b)

// Native layout and HLSL field order are the same ABI.
#define HS_SSR_FIELDS(X) \
 X(Matrix,inverse_view_projection,SsrInverseViewProjection,Zero) \
 X(Matrix,world_to_view,SsrWorldToView,Zero) \
 X(Matrix,projection,SsrProjection,Zero) \
 X(Float4,render_size,SsrRenderSize,Zero) \
 X(Float4,trace_size,SsrTraceSize,Zero) \
 X(Float4,previous_trace_size,SsrPreviousTraceSize,Zero) \
 X(Float4,previous_depth_parameters,SsrPreviousDepthParameters,Zero) \
 X(Float4,jitter_uv,SsrJitterUv,Zero) \
 X(Float4,trace_parameters,SsrTraceParameters,Trace) \
 X(Float4,filter_parameters,SsrFilterParameters,Filter) \
 X(Uint4,budget,SsrBudget,Zero) \
 X(Uint4,frame_info,SsrFrameInfo,Zero) \
 X(Uint4Array,hierarchy_levels,SsrHierarchyLevels,Zero)
#define HS_SSR_PASS_FIELDS(X) \
 X(HierarchyLevel,hierarchy_level,SsrHierarchyLevel) \
 X(AmbientEnabled,ambient_enabled,SsrAmbientVisibilityEnabled) \
 X(SidecarCount,sidecar_count,SsrSidecarPlaneCount) \
 X(Reserved,reserved,SsrPassReserved)

// Each row binds a shader slot to the native descriptor and graph version.
// Buffer t9 is mirrored for the table but read through the material root SRV.
#define HS_SSR_SRVS(X) \
 X(Texture,Depth,0,float,SurfaceDepth,inputs.depth.version,inputs.depth.resource,DXGI_FORMAT_R32_FLOAT) \
 X(Texture,Base,1,float4,GBufferBase,inputs.base.version,inputs.base.resource,DXGI_FORMAT_R8G8B8A8_UNORM) \
 X(Texture,Normal,2,float2,GBufferNormal,inputs.normal.version,inputs.normal.resource,DXGI_FORMAT_R16G16_SNORM) \
 X(Texture,Metadata,3,uint,GBufferMetadata,inputs.metadata.version,inputs.metadata.resource,DXGI_FORMAT_R32_UINT) \
 X(Texture,Material,4,float4,GBufferMaterial,inputs.material.version,inputs.material.resource,DXGI_FORMAT_R8G8B8A8_UNORM) \
 X(Texture,Motion,5,float2,SsrMotion,inputs.motion.version,inputs.motion.resource,DXGI_FORMAT_R16G16_FLOAT) \
 X(Texture,SurfaceHistory,6,float2,SsrSurfaceHistory,inputs.surface_history.version,inputs.surface_history.resource,DXGI_FORMAT_R32G32_FLOAT) \
 X(Texture,Opaque,7,float4,SsrOpaqueHDR,inputs.opaque_hdr.version,inputs.opaque_hdr.resource,DXGI_FORMAT_R16G16B16A16_FLOAT) \
 X(Texture,Environment,8,float4,SsrEnvironmentSpecular,inputs.environment_specular.version,inputs.environment_specular.resource,DXGI_FORMAT_R16G16B16A16_FLOAT) \
 X(Buffer,Parameters,9,uint4,SsrParameters,inputs.material_parameters.version,inputs.material_parameters.resource,DXGI_FORMAT_UNKNOWN) \
 X(Texture,Sidecar0,10,uint4,SurfaceSidecar0,inputs.sidecars[0].version,sidecar_resource(0),sidecar_format(0)) \
 X(Texture,Sidecar1,11,uint4,SurfaceSidecar1,inputs.sidecars[1].version,sidecar_resource(1),sidecar_format(1)) \
 X(Texture,Ambient,12,float,SsrAmbientVisibility,inputs.ambient_visibility.version,inputs.ambient_visibility_enabled ? inputs.ambient_visibility.resource : nullptr,DXGI_FORMAT_R16_FLOAT)
#define HS_SSR_UAVS(X) \
 X(Texture,Raw,0,float4,SsrRawOutput,raw,storage->raw.resource.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT) \
 X(Texture,Previous,1,float4,SsrPreviousRadiance,previous,storage->history[storage->read_index].resource.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT) \
 X(Texture,Current,2,float4,SsrCurrentRadiance,current,storage->history[writeIndex].resource.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT) \
 X(Texture,PreviousValidation,3,uint4,SsrPreviousValidation,previousValidation,storage->validation_history[storage->read_index].resource.Get(),DXGI_FORMAT_R32G32B32A32_UINT) \
 X(Texture,Validation,4,uint4,SsrCurrentValidation,validation,storage->validation_history[writeIndex].resource.Get(),DXGI_FORMAT_R32G32B32A32_UINT) \
 X(Texture,Composed,5,float4,SsrComposed,composed,storage->composed.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT) \
 X(Buffer,Hierarchy,6,float,SsrClosestDepth,hierarchy,storage->hierarchy.resource.Get(),DXGI_FORMAT_UNKNOWN)

// Conditional lists are consumed only when the matching topology is present.
#define HS_SSR_SURFACE_READS(R) R(Depth) R(Base) R(Normal) R(Metadata) R(Material) R(Parameters)
#define HS_SSR_TRACE_READS(R) R(Opaque) R(Environment)
#define HS_SSR_TRACE_WRITES(W) W(Hierarchy) W(Raw)
#define HS_SSR_TEMPORAL_READS(R,U) U(Raw) R(Motion) R(SurfaceHistory)
#define HS_SSR_HISTORY_READS(U) U(Previous) U(PreviousValidation)
#define HS_SSR_TEMPORAL_WRITES(W) W(Current) W(Validation)
#define HS_SSR_SPATIAL_READS(R,U) U(Current) U(Validation) R(Opaque) R(Environment)
#define HS_SSR_SPATIAL_WRITES(W) W(Composed)

#ifdef HS_SSR_NATIVE
#define HS_SSR_TYPE_Matrix DirectX::XMFLOAT4X4
#define HS_SSR_TYPE_Float4 DirectX::XMFLOAT4
#define HS_SSR_TYPE_Uint4 DirectX::XMUINT4
#define HS_SSR_NATIVE_FIELD_Matrix(name,init) HS_SSR_TYPE_Matrix name HS_SSR_DEFAULT_##init;
#define HS_SSR_NATIVE_FIELD_Float4(name,init) HS_SSR_TYPE_Float4 name HS_SSR_DEFAULT_##init;
#define HS_SSR_NATIVE_FIELD_Uint4(name,init) HS_SSR_TYPE_Uint4 name HS_SSR_DEFAULT_##init;
#define HS_SSR_NATIVE_FIELD_Uint4Array(name,init) std::array<DirectX::XMUINT4,16> name HS_SSR_DEFAULT_##init;
#define HS_SSR_DEFAULT_Zero {}
#define HS_SSR_DEFAULT_Trace {50.0f,0.15f,0.03f,0.75f}
#define HS_SSR_DEFAULT_Filter {0.03f,0.85f,0.9f,0.08f}
struct alignas(16) ScreenReflectionsConstants {
#define HS_SSR_FIELD(type,native,shader,init) HS_SSR_NATIVE_FIELD_##type(native,init)
 HS_SSR_FIELDS(HS_SSR_FIELD)
#undef HS_SSR_FIELD
};
enum SsrSrvSlot : UINT {
#define HS_SSR_SLOT(kind,name,slot,...) SsrSrv##name = slot,
 HS_SSR_SRVS(HS_SSR_SLOT)
#undef HS_SSR_SLOT
 SsrSrvCount
};
enum SsrUavSlot : UINT {
#define HS_SSR_SLOT(kind,name,slot,...) SsrUav##name = slot,
 HS_SSR_UAVS(HS_SSR_SLOT)
#undef HS_SSR_SLOT
 SsrUavCount
};
enum SsrRoot : UINT { SsrRootConstants, SsrRootSrvs, SsrRootUavs, SsrRootPass, SsrRootFrame, SsrRootMaterial, SsrRootCount };
enum SsrPassIndex : UINT {
#define HS_SSR_PASS_INDEX(name,native,shader) SsrPass##name,
 HS_SSR_PASS_FIELDS(HS_SSR_PASS_INDEX)
#undef HS_SSR_PASS_INDEX
 SsrPassCount
};
struct SsrPassConstants {
#define HS_SSR_PASS_FIELD(name,native,shader) std::uint32_t native{};
 HS_SSR_PASS_FIELDS(HS_SSR_PASS_FIELD)
#undef HS_SSR_PASS_FIELD
};
inline constexpr UINT kSsrSpace = HS_SSR_SPACE, kSsrConstantRegister = HS_SSR_CONSTANT_REGISTER,
    kSsrPassRegister = HS_SSR_PASS_REGISTER;
static_assert(sizeof(SsrPassConstants) == SsrPassCount * sizeof(std::uint32_t));
static_assert(sizeof(ScreenReflectionsConstants) == 592);
static_assert(offsetof(ScreenReflectionsConstants, hierarchy_levels) == 336);
#else
#define HS_SSR_HLSL_FIELD_Matrix(name) row_major float4x4 name;
#define HS_SSR_HLSL_FIELD_Float4(name) float4 name;
#define HS_SSR_HLSL_FIELD_Uint4(name) uint4 name;
#define HS_SSR_HLSL_FIELD_Uint4Array(name) uint4 name[16];
cbuffer ScreenReflectionsConstants : register(HS_SSR_JOIN(b,HS_SSR_CONSTANT_REGISTER), HS_SSR_JOIN(space,HS_SSR_SPACE)) {
#define HS_SSR_FIELD(type,native,shader,init) HS_SSR_HLSL_FIELD_##type(shader)
 HS_SSR_FIELDS(HS_SSR_FIELD)
#undef HS_SSR_FIELD
};
cbuffer ScreenReflectionsPass : register(HS_SSR_JOIN(b,HS_SSR_PASS_REGISTER), HS_SSR_JOIN(space,HS_SSR_SPACE)) {
#define HS_SSR_PASS_FIELD(name,native,shader) uint shader;
 HS_SSR_PASS_FIELDS(HS_SSR_PASS_FIELD)
#undef HS_SSR_PASS_FIELD
};
#define HS_SSR_HLSL_SRV_Texture(name,slot,type,shader) Texture2D<type> shader : register(t##slot, HS_SSR_JOIN(space,HS_SSR_SPACE));
#define HS_SSR_HLSL_SRV_Buffer(name,slot,type,shader)
#define HS_SSR_HLSL_SRV(kind,name,slot,type,shader,version,resource,format) HS_SSR_HLSL_SRV_##kind(name,slot,type,shader)
HS_SSR_SRVS(HS_SSR_HLSL_SRV)
#undef HS_SSR_HLSL_SRV
#define HS_SSR_HLSL_UAV_Texture(name,slot,type,shader) RWTexture2D<type> shader : register(u##slot, HS_SSR_JOIN(space,HS_SSR_SPACE));
#define HS_SSR_HLSL_UAV_Buffer(name,slot,type,shader) RWStructuredBuffer<type> shader : register(u##slot, HS_SSR_JOIN(space,HS_SSR_SPACE));
#define HS_SSR_HLSL_UAV(kind,name,slot,type,shader,version,resource,format) HS_SSR_HLSL_UAV_##kind(name,slot,type,shader)
HS_SSR_UAVS(HS_SSR_HLSL_UAV)
#undef HS_SSR_HLSL_UAV
#endif
#endif
