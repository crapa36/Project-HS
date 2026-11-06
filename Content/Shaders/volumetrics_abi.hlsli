#ifndef HS_VOLUMETRICS_ABI_HLSLI
#define HS_VOLUMETRICS_ABI_HLSLI
#define HS_VM_SPACE 24
#define HS_VM_CONSTANT_REGISTER 0
#define HS_VM_JOIN_INNER(a,b) a##b
#define HS_VM_JOIN(a,b) HS_VM_JOIN_INNER(a,b)
#define HS_VM_GRADIENT_REGISTER 1
#define HS_VM_GRADIENT_SPACE 3
// External owners retain their own shader declarations; these are this pass's table ranges.
#define HS_VM_EXTERNAL_RANGES(X) \
 X(Gradient,4,1,HS_VM_GRADIENT_REGISTER,HS_VM_GRADIENT_SPACE,29) \
 X(Light,5,3,0,19,30) \
 X(Shadow,6,4,0,21,33) \
 X(ShadowRoot,7,2,5,21,37) \
 X(LocalShadow,8,4,0,27,39) \
 X(ProbeIrradiance,9,1,2,23,27) \
 X(ProbeRecords,10,1,4,23,28)
#define HS_VM_EXTERNAL_LIGHTING_READS(X) X(Lights) X(Probes) X(Shadows) X(LocalShadows) X(CompactShadow)
#define HS_VM_EXTERNAL_HERO_READS(X) X(Gradient)

#define HS_VM_FIELDS(X) \
 X(Matrix,inverse_view_projection,VmInverseViewProjection,Zero) \
 X(Matrix,world_to_view,VmWorldToView,Zero) \
 X(Matrix,previous_view_projection,VmPreviousViewProjection,Zero) \
 X(Matrix,previous_world_to_view,VmPreviousWorldToView,Zero) \
 X(MatrixArray,shadow_view_projection,VmShadowViewProjection,Zero) \
 X(Float4Array,shadow_atlas_scale_offset,VmShadowAtlasScaleOffset,Zero) \
 X(Float4,render_size,VmRenderSize,Zero) X(Float4,froxel_size,VmFroxelSize,Zero) \
 X(Float4,capacity,VmCapacity,Zero) X(Float4,previous_render_size,VmPreviousRenderSize,Zero) \
 X(Float4,previous_froxel_size,VmPreviousFroxelSize,Zero) X(Float4,origin_near,VmOriginNear,Zero) \
 X(Float4,previous_origin_near,VmPreviousOriginNear,Zero) X(Float4,depth_range,VmDepthRange,Zero) \
 X(Float4,filter_parameters,VmFilterParameters,Filter) X(Float4,shadow_parameters,VmShadowParameters,Zero) \
 X(Uint4,shadow_identity,VmShadowIdentity,Zero) X(Uint4,frame_info,VmFrameInfo,Zero) \
 X(Uint4,counts,VmCounts,Zero) X(Uint4,hero_info,VmHeroInfo,Zero)

// Slot, descriptor offset, shader type/name, graph version, native resource, format.
#define HS_VM_LOCAL_SRVS(X) \
 X(Texture2D,Depth,0,1,float,VmDepth,in.depth.version,in.depth.resource,kScalar) \
 X(Texture2D,Hdr,1,2,float4,VmHdr,in.hdr.version,in.hdr.resource,kHdr) \
 X(Buffer,Volumes,2,3,VmVolume,VmVolumes,p.volumes,s->volumes.resource.Get(),DXGI_FORMAT_UNKNOWN) \
 X(Texture3D,Medium,3,4,float4,VmMedium,p.medium,s->medium.resource.Get(),kSignal) \
 X(Texture3D,Phase,4,5,float,VmPhase,p.phase,s->phase.resource.Get(),kScalar) \
 X(Texture3D,Raw,5,6,float4,VmRawScattering,p.raw,s->raw.resource.Get(),kSignal) \
 X(Texture3D,Previous,6,7,float4,VmPreviousScattering,p.previous,p.history?s->history[s->read_index].resource.Get():nullptr,kSignal) \
 X(Texture2D,PreviousDepth,7,8,float,VmPreviousDepth,p.previous_depth,p.history?s->depth_history[s->read_index].resource.Get():nullptr,kScalar) \
 X(Texture3D,Current,8,9,float4,VmTemporalScattering,p.current,s->history[p.write].resource.Get(),kSignal) \
 X(Texture2D,CompactShadow,12,13,float,VmCompactShadow,in.compact_shadow.atlas.version,in.compact_shadow.identity.w?in.compact_shadow.atlas.resource:nullptr,kScalar) \
 X(Buffer,Heroes,14,24,VmHeroVolume,VmHeroes,p.heroes,s->heroes.resource.Get(),DXGI_FORMAT_UNKNOWN) \
 X(Texture2DArray,BlueNoise,15,25,float,VmHeroBlueNoise,in.hero_blue_noise.version,p.heroes_state.empty()?nullptr:in.hero_blue_noise.resource,DXGI_FORMAT_R8_UNORM) \
 X(Texture3DSpecial,NoiseVolume,16,26,float4,VmHeroNoiseVolume,in.hero_noise_volume.version,p.heroes_state.empty()?nullptr:in.hero_noise_volume.resource,DXGI_FORMAT_R8G8B8A8_UNORM)
#define HS_VM_SAMPLING_SRVS(X) \
 X(Texture3D,Filtered,9,10,float4,VmFilteredScattering,p.filtered,s->filtered.resource.Get(),kSignal) \
 X(Texture3D,Luminance,10,11,float4,VmCumulativeL,p.luminance,s->luminance.resource.Get(),kSignal) \
 X(Texture3D,Transmittance,11,12,float,VmCumulativeT,p.transmittance,s->transmittance.resource.Get(),kScalar) \
 X(Texture2D,ColumnDepth,13,14,float,VmColumnDepth,p.current_depth,s->depth_history[p.write].resource.Get(),kScalar)
#define HS_VM_SRVS(X) HS_VM_LOCAL_SRVS(X) HS_VM_SAMPLING_SRVS(X)
#define HS_VM_UAVS(X) \
 X(Texture3D,Medium,0,15,float4,VmMediumOutput,p.medium,s->medium.resource.Get(),kSignal) \
 X(Texture3D,Phase,1,16,float,VmPhaseOutput,p.phase,s->phase.resource.Get(),kScalar) \
 X(Texture3D,Raw,2,17,float4,VmRawOutput,p.raw,s->raw.resource.Get(),kSignal) \
 X(Texture3D,Current,3,18,float4,VmTemporalOutput,p.current,s->history[p.write].resource.Get(),kSignal) \
 X(Texture2D,ColumnDepth,4,19,float,VmDepthOutput,p.current_depth,s->depth_history[p.write].resource.Get(),kScalar) \
 X(Texture3D,Filtered,5,20,float4,VmFilteredOutput,p.filtered,s->filtered.resource.Get(),kSignal) \
 X(Texture3D,Luminance,6,21,float4,VmLuminanceOutput,p.luminance,s->luminance.resource.Get(),kSignal) \
 X(Texture3D,Transmittance,7,22,float,VmTransmittanceOutput,p.transmittance,s->transmittance.resource.Get(),kScalar) \
 X(Texture2D,Resolved,8,23,float4,VmResolvedOutput,p.resolved,s->resolved.resource.Get(),kHdr)

#define HS_VM_DENSITY_READS(R) R(Depth) R(Volumes)
#define HS_VM_DENSITY_WRITES(W) W(Medium) W(Phase) W(ColumnDepth)
#define HS_VM_HERO_READS(R) R(Heroes) R(ColumnDepth) R(BlueNoise)
#define HS_VM_HERO_WRITES(W) W(Medium) W(Phase) W(Raw)
#define HS_VM_LIGHTING_READS(R) R(Medium) R(Phase)
#define HS_VM_TEMPORAL_READS(R) R(Raw) R(ColumnDepth)
#define HS_VM_HISTORY_READS(R) R(Previous) R(PreviousDepth)
#define HS_VM_SPATIAL_READS(R) R(Current) R(ColumnDepth)
#define HS_VM_INTEGRATE_READS(R) R(Filtered)
#define HS_VM_INTEGRATE_WRITES(W) W(Luminance) W(Transmittance)
#define HS_VM_RESOLVE_READS(R) R(Depth) R(Hdr) R(Filtered) R(ColumnDepth) R(Luminance) R(Transmittance)

#ifdef HS_VM_NATIVE
#define HS_VM_DEFAULT_Zero {}
#define HS_VM_DEFAULT_Filter {0.85f,0.03f,0.0f,0.0f}
#define HS_VM_NATIVE_Matrix(n,i) DirectX::XMFLOAT4X4 n HS_VM_DEFAULT_##i;
#define HS_VM_NATIVE_MatrixArray(n,i) std::array<DirectX::XMFLOAT4X4,3> n HS_VM_DEFAULT_##i;
#define HS_VM_NATIVE_Float4(n,i) DirectX::XMFLOAT4 n HS_VM_DEFAULT_##i;
#define HS_VM_NATIVE_Float4Array(n,i) std::array<DirectX::XMFLOAT4,3> n HS_VM_DEFAULT_##i;
#define HS_VM_NATIVE_Uint4(n,i) DirectX::XMUINT4 n HS_VM_DEFAULT_##i;
struct alignas(16) VolumetricConstants {
#define HS_VM_FIELD(t,n,h,i) HS_VM_NATIVE_##t(n,i)
 HS_VM_FIELDS(HS_VM_FIELD)
#undef HS_VM_FIELD
};
enum VolumetricSrvSlot : unsigned {
#define HS_VM_SLOT(k,n,s,o,t,h,v,r,f) VmSrv##n=s,
 HS_VM_SRVS(HS_VM_SLOT)
#undef HS_VM_SLOT
 VmSrvCount=17
};
enum VolumetricUavSlot : unsigned {
#define HS_VM_SLOT(k,n,s,o,t,h,v,r,f) VmUav##n=s,
 HS_VM_UAVS(HS_VM_SLOT)
#undef HS_VM_SLOT
 VmUavCount=9
};
enum VolumetricDescriptorOffset : unsigned {
#define HS_VM_SLOT(k,n,s,o,t,h,v,r,f) VmSrvDescriptor##n=o,
 HS_VM_SRVS(HS_VM_SLOT)
#undef HS_VM_SLOT
#define HS_VM_SLOT(k,n,s,o,t,h,v,r,f) VmUavDescriptor##n=o,
 HS_VM_UAVS(HS_VM_SLOT)
#undef HS_VM_SLOT
};
enum VolumetricExternalOffset : unsigned {
#define HS_VM_RANGE(name,index,count,base,space,offset) VmExternal##name=offset, VmExternalCount##name=count,
 HS_VM_EXTERNAL_RANGES(HS_VM_RANGE)
#undef HS_VM_RANGE
};
static_assert(sizeof(VolumetricConstants)==720);
static_assert(offsetof(VolumetricConstants,render_size)==496);
#else
#define HS_VM_HLSL_Matrix(n) row_major float4x4 n;
#define HS_VM_HLSL_MatrixArray(n) row_major float4x4 n[3];
#define HS_VM_HLSL_Float4(n) float4 n;
#define HS_VM_HLSL_Float4Array(n) float4 n[3];
#define HS_VM_HLSL_Uint4(n) uint4 n;
cbuffer VolumetricConstants : register(HS_VM_JOIN(b,HS_VM_CONSTANT_REGISTER),HS_VM_JOIN(space,HS_VM_SPACE)) {
#define HS_VM_FIELD(t,n,h,i) HS_VM_HLSL_##t(h)
 HS_VM_FIELDS(HS_VM_FIELD)
#undef HS_VM_FIELD
};
#define HS_VM_HLSL_SRV_Texture2D(type,h,s) Texture2D<type> h : register(t##s,HS_VM_JOIN(space,HS_VM_SPACE));
#define HS_VM_HLSL_SRV_Texture3D(type,h,s) Texture3D<type> h : register(t##s,HS_VM_JOIN(space,HS_VM_SPACE));
#define HS_VM_HLSL_SRV_Texture3DSpecial(type,h,s) Texture3D<type> h : register(t##s,HS_VM_JOIN(space,HS_VM_SPACE));
#define HS_VM_HLSL_SRV_Texture2DArray(type,h,s) Texture2DArray<type> h : register(t##s,HS_VM_JOIN(space,HS_VM_SPACE));
#define HS_VM_HLSL_SRV_Buffer(type,h,s) StructuredBuffer<type> h : register(t##s,HS_VM_JOIN(space,HS_VM_SPACE));
#define HS_VM_HLSL_SRV(k,n,s,o,t,h,v,r,f) HS_VM_HLSL_SRV_##k(t,h,s)
HS_VM_SAMPLING_SRVS(HS_VM_HLSL_SRV)
#define HS_VM_HLSL_UAV_Texture2D(t,h,s) RWTexture2D<t> h : register(u##s,HS_VM_JOIN(space,HS_VM_SPACE));
#define HS_VM_HLSL_UAV_Texture3D(t,h,s) RWTexture3D<t> h : register(u##s,HS_VM_JOIN(space,HS_VM_SPACE));
#define HS_VM_HLSL_UAV(k,n,s,o,t,h,v,r,f) HS_VM_HLSL_UAV_##k(t,h,s)
#endif
#endif
