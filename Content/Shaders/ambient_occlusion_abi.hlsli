#ifndef HS_AMBIENT_OCCLUSION_ABI_HLSLI
#define HS_AMBIENT_OCCLUSION_ABI_HLSLI
#define HS_AO_SPACE 12
#define HS_AO_CBV_REGISTER 0
#define HS_AO_JOIN_INNER(a,b) a##b
#define HS_AO_JOIN(a,b) HS_AO_JOIN_INNER(a,b)

// The native and shader names share one field order and default contract.
#define HS_AO_FIELDS(X) \
 X(Matrix,inverse_projection,AoInverseProjection,Zero) \
 X(Matrix,world_to_view,AoWorldToView,Zero) \
 X(Matrix,projection,AoProjection,Zero) \
 X(Float4,render_size,AoRenderSize,Zero) \
 X(Float4,ao_size,AoSize,Zero) \
 X(Float4,history_size,AoHistorySize,Zero) \
 X(Float4,previous_depth_parameters,AoPreviousDepthParameters,Zero) \
 X(Float4,jitter_uv,AoJitterUv,Zero) \
 X(Float4,gtao_parameters,AoParameters,Gtao) \
 X(Float4,filter_parameters,AoFilterParameters,Filter) \
 X(Float4,contact_parameters,AoContactParameters,Contact) \
 X(Float4,toward_light,AoTowardLight,Zero) \
 X(Uint4,sample_counts,AoSampleCounts,Zero) \
 X(Uint4,frame_info,AoFrameInfo,Zero)

// Binding entries also specify the native resource and view format. These
// expressions are used only by the native descriptor writer.
#define HS_AO_SRVS(X) \
 X(Depth,0,float,AoDepth,inputs.depth.resource,DXGI_FORMAT_R32_FLOAT) \
 X(Normal,1,float2,AoNormal,inputs.normal.resource,DXGI_FORMAT_R16G16_SNORM) \
 X(Metadata,2,uint,AoMetadata,inputs.metadata.resource,DXGI_FORMAT_R32_UINT) \
 X(Motion,3,float2,AoMotion,storage->ao_enabled ? inputs.motion.resource : nullptr,DXGI_FORMAT_R16G16_FLOAT) \
 X(SurfaceHistory,4,float2,AoSurfaceHistory,storage->ao_enabled ? inputs.surface_history.resource : nullptr,DXGI_FORMAT_R32G32_FLOAT) \
 X(Raw,5,float4,AoRaw,storage->raw.resource.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT) \
 X(Previous,6,float4,AoPrevious,historyValid ? storage->history[storage->read_index].resource.Get() : nullptr,DXGI_FORMAT_R32G32B32A32_FLOAT) \
 X(PreviousMetadata,7,uint,AoPreviousMetadata,historyValid ? storage->metadata_history[storage->read_index].resource.Get() : nullptr,DXGI_FORMAT_R32_UINT) \
 X(Temporal,8,float4,AoTemporal,storage->history[writeIndex].resource.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT) \
 X(PreparedInverseW,9,uint,AoPreparedInverseW,storage->metadata_history[writeIndex].resource.Get(),DXGI_FORMAT_R32_UINT)
#define HS_AO_UAVS(X) \
 X(RawOutput,0,float4,AoRawOutput,storage->raw.resource.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT) \
 X(HistoryOutput,1,float4,AoHistoryOutput,storage->history[writeIndex].resource.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT) \
 X(ResolvedOutput,2,float,AoResolvedOutput,storage->resolved.resource.Get(),DXGI_FORMAT_R16_FLOAT) \
 X(ContactOutput,3,float,AoContactOutput,storage->contact.resource.Get(),DXGI_FORMAT_R16_FLOAT) \
 X(MetadataOutput,4,uint,AoMetadataOutput,storage->metadata_history[writeIndex].resource.Get(),DXGI_FORMAT_R32_UINT)
#define HS_AO_ADAPTIVE(X) X(Adaptive,6,33,uint,AdGtaoDensity,inputs.adaptive_density.resource,DXGI_FORMAT_R8_UINT)

// Graph versions correspond to the resource selected by each binding role.
#define HS_AO_GRAPH_Depth inputs.depth.version
#define HS_AO_GRAPH_Normal inputs.normal.version
#define HS_AO_GRAPH_Metadata inputs.metadata.version
#define HS_AO_GRAPH_Motion inputs.motion.version
#define HS_AO_GRAPH_SurfaceHistory inputs.surface_history.version
#define HS_AO_GRAPH_Raw raw
#define HS_AO_GRAPH_Previous previous
#define HS_AO_GRAPH_PreviousMetadata previousMetadata
#define HS_AO_GRAPH_Temporal history
#define HS_AO_GRAPH_PreparedInverseW metadata
#define HS_AO_GRAPH_Adaptive inputs.adaptive_density.version
#define HS_AO_GRAPH_RawOutput raw
#define HS_AO_GRAPH_HistoryOutput history
#define HS_AO_GRAPH_ResolvedOutput resolved
#define HS_AO_GRAPH_ContactOutput contact
#define HS_AO_GRAPH_MetadataOutput metadata
#define HS_AO_GUIDES(R) R(Depth) R(Normal) R(Metadata)
#define HS_AO_PREPARE_READS(R) R(Depth) R(Metadata)
#define HS_AO_PREPARE_WRITES(W) W(MetadataOutput)
#define HS_AO_HORIZON_PREPARED(R) R(PreparedInverseW)
#define HS_AO_HORIZON_ADAPTIVE(R) R(Adaptive)
#define HS_AO_HORIZON_WRITES(W) W(RawOutput)
#define HS_AO_TEMPORAL_READS(R) R(Raw) R(Motion) R(SurfaceHistory)
#define HS_AO_HISTORY_READS(R) R(Previous) R(PreviousMetadata)
#define HS_AO_TEMPORAL_WRITES(W) W(HistoryOutput) W(MetadataOutput)
#define HS_AO_BILATERAL_READS(R) R(Temporal)
#define HS_AO_BILATERAL_WRITES(W) W(ResolvedOutput)
#define HS_AO_CONTACT_WRITES(W) W(ContactOutput)

#ifdef HS_AO_NATIVE
#define HS_AO_TYPE_Matrix DirectX::XMFLOAT4X4
#define HS_AO_TYPE_Float4 DirectX::XMFLOAT4
#define HS_AO_TYPE_Uint4 DirectX::XMUINT4
#define HS_AO_DEFAULT_Zero {}
#define HS_AO_DEFAULT_Gtao {1.0f,1.0f,0.8f,0.0f}
#define HS_AO_DEFAULT_Filter {0.03f,0.85f,0.9f,0.02f}
#define HS_AO_DEFAULT_Contact {0.75f,0.1f,0.02f,0.0f}
struct alignas(16) AmbientOcclusionConstants {
#define HS_AO_FIELD(type,native,shader,init) HS_AO_TYPE_##type native HS_AO_DEFAULT_##init;
 HS_AO_FIELDS(HS_AO_FIELD)
#undef HS_AO_FIELD
};
enum AoSrvSlot : UINT {
#define HS_AO_SLOT(name,slot,...) AoSrv##name = slot,
 HS_AO_SRVS(HS_AO_SLOT)
#undef HS_AO_SLOT
 AoSrvCount
};
enum AoUavSlot : UINT {
#define HS_AO_SLOT(name,slot,...) AoUav##name = slot,
 HS_AO_UAVS(HS_AO_SLOT)
#undef HS_AO_SLOT
 AoUavCount
};
enum AoRoot : UINT { AoRootConstants, AoRootSrvs, AoRootUavs, AoRootAdaptive, AoRootCount };
inline constexpr UINT kAoRegisterSpace = HS_AO_SPACE, kAoConstantsRegister = HS_AO_CBV_REGISTER;
#define HS_AO_ADAPTIVE_CONSTANTS(name,slot,space,...) inline constexpr UINT kAoAdaptiveRegister = slot, kAoAdaptiveSpace = space, AoSrv##name = slot;
HS_AO_ADAPTIVE(HS_AO_ADAPTIVE_CONSTANTS)
#undef HS_AO_ADAPTIVE_CONSTANTS
static_assert(sizeof(AmbientOcclusionConstants) == 368);
static_assert(offsetof(AmbientOcclusionConstants, render_size) == 192);
static_assert(offsetof(AmbientOcclusionConstants, sample_counts) == 336);
#else
#define HS_AO_TYPE_Matrix row_major float4x4
#define HS_AO_TYPE_Float4 float4
#define HS_AO_TYPE_Uint4 uint4
cbuffer AmbientOcclusionConstants : register(HS_AO_JOIN(b,HS_AO_CBV_REGISTER), HS_AO_JOIN(space,HS_AO_SPACE)) {
#define HS_AO_FIELD(type,native,shader,init) HS_AO_TYPE_##type shader;
 HS_AO_FIELDS(HS_AO_FIELD)
#undef HS_AO_FIELD
};
#define HS_AO_SRV(name,slot,type,shader,resource,format) Texture2D<type> shader : register(t##slot, HS_AO_JOIN(space,HS_AO_SPACE));
HS_AO_SRVS(HS_AO_SRV)
#undef HS_AO_SRV
#define HS_AO_UAV(name,slot,type,shader,resource,format) RWTexture2D<type> shader : register(u##slot, HS_AO_JOIN(space,HS_AO_SPACE));
HS_AO_UAVS(HS_AO_UAV)
#undef HS_AO_UAV
#define HS_AO_DECLARE_ADAPTIVE(name,slot,regspace,type,shader,resource,format) Texture2D<type> shader : register(t##slot, space##regspace);
#endif
#undef HS_AO_TYPE_Matrix
#undef HS_AO_TYPE_Float4
#undef HS_AO_TYPE_Uint4
#endif
