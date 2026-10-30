#include "renderer_brixelizer_gi.hpp"
#include "renderer_brixelizer_scene.hpp"
#include "d3d12_resources.hpp"
#include <hs/render_data/texture_package.hpp>
#include <D3D12MemAlloc.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

#ifndef HS_WITH_BRIXELIZER
#define HS_WITH_BRIXELIZER 0
#endif
#if HS_WITH_BRIXELIZER
#include "renderer_brixelizer_backend.hpp"
#include <FidelityFX/host/ffx_brixelizer.h>
#include <FidelityFX/host/ffx_brixelizergi.h>
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#endif

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
Result Fail(ErrorCode code,std::string reason)
{return Result::Failure(code,"hs_renderer_brixelizer_gi",std::move(reason));}
Result Native(std::string_view what,HRESULT error)
{return Fail(ErrorCode::TaskFailed,std::string(what)+" failed (HRESULT "+std::to_string(std::int32_t(error))+").");}
bool Extent(ViewExtent extent)
{return extent.width&&extent.height&&extent.width<=D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION&&extent.height<=D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;}
bool Finite(const DirectX::XMFLOAT4X4 &m)
{for(const auto &row:m.m)for(float f:row)if(!std::isfinite(f))return false;return true;}
bool Finite(DirectX::XMFLOAT3 p)
{return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
constexpr std::uint32_t kStages=4,kLightingStages=3,kConstantStride=256;
constexpr std::string_view kShaderProducts[]{"render/shader/brixelizer_guides_cs.dxil",
    "render/shader/brixelizer_gi_resolve_cs.dxil","render/shader/brixelizer_cache_export_cs.dxil","render/shader/brixelizer_gi_compose_cs.dxil"};
enum Screen : std::uint32_t { Depth,Normal,Roughness,Motion,RawDiffuse,RawSpecular,Diffuse,Delta,
    Subsurface,Specular,CacheCurrent,CacheHistory,DepthHistory,NormalHistory,CacheInjection,Composed,ScreenCount };
constexpr DXGI_FORMAT kScreenFormats[]{DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT,
    DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32G32_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT};
using Allocation=std::shared_ptr<AllocationResource>;
Result Allocate(ID3D12Device *,D3D12MA::Allocator *allocator,const D3D12_RESOURCE_DESC &desc,
    bool upload,Allocation &output)
{
    auto allocation=std::make_shared<AllocationResource>();
    D3D12MA::ALLOCATION_DESC heap{};heap.HeapType=upload?D3D12_HEAP_TYPE_UPLOAD:D3D12_HEAP_TYPE_DEFAULT;
    auto error=allocator->CreateResource(&heap,&desc,upload?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COMMON,
        nullptr,&allocation->allocation,IID_PPV_ARGS(allocation->resource.ReleaseAndGetAddressOf()));
    if(FAILED(error))return Native("Allocate Brixelizer backing",error);
    allocation->graph_access=upload?Access::ComputeRead:Access::Common;output=std::move(allocation);return Result::Success();
}
Result Buffer(ID3D12Device *device,D3D12MA::Allocator *allocator,std::uint64_t bytes,bool upload,Allocation &output)
{
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;
    desc.Height=1;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags=upload?D3D12_RESOURCE_FLAG_NONE:D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return Allocate(device,allocator,desc,upload,output);
}
Result Texture(ID3D12Device *device,D3D12MA::Allocator *allocator,ViewExtent extent,DXGI_FORMAT format,
    Allocation &output,std::uint16_t depth=1,bool render_target=false)
{
    D3D12_RESOURCE_DESC desc{};desc.Dimension=depth==1?D3D12_RESOURCE_DIMENSION_TEXTURE2D:D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    desc.Width=extent.width;desc.Height=extent.height;desc.DepthOrArraySize=depth;desc.MipLevels=1;
    desc.Format=format;desc.SampleDesc.Count=1;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if(render_target)desc.Flags|=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return Allocate(device,allocator,desc,false,output);
}
std::uint64_t Bytes(const Allocation &allocation)
{return allocation&&allocation->allocation?allocation->allocation->GetSize():0;}
bool Guide(const RenderGraphBuilder &graph,const AmbientOcclusionGuide &g,ViewExtent extent,DXGI_FORMAT format,bool exact=false)
{
    if(!g.resource||!g.version.resource||graph.Resolve(g.version.resource)!=g.resource)return false;
    const auto d=g.resource->GetDesc();return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.DepthOrArraySize==1&&
        d.SampleDesc.Count==1&&d.MipLevels==1&&d.Format==format&&
        (exact?d.Width==extent.width&&d.Height==extent.height:d.Width>=extent.width&&d.Height>=extent.height);
}
}

struct BrixelizerStorage
{
    struct Slot
    {
        CompletionSet completion;
        std::vector<std::shared_ptr<const void>> owners;
        std::vector<ComPtr<ID3D12Resource>> resources;
        std::array<SceneShaderBinder,kStages> binders;
        SceneShaderBinder restore;
#if HS_WITH_BRIXELIZER
        std::shared_ptr<FfxBrixelizerBakedUpdateDescription> baked_update;
#endif
    };
    ComPtr<ID3D12Device> device;
    ComPtr<D3D12MA::Allocator> allocator;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>,kStages> pipelines;
    ComPtr<ID3D12PipelineState> cache_pipeline;
    ComPtr<ID3D12DescriptorHeap> cache_rtv;
    ComPtr<ID3D12DescriptorHeap> descriptors,exports;
    std::array<Allocation,ScreenCount> screens;
    std::array<Allocation,16> noise_textures,noise_uploads;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,16> noise_footprints{};
    bool noise_uploaded{};
    Allocation constants,atlas,brick_aabbs,scratch;
    std::vector<Allocation> trees,maps,retired_scratch;
    std::vector<Slot> slots;
    std::byte *mapped{};
    std::uint32_t descriptor_stride{};
    ViewExtent extent{};
    BrixelizerSettings settings{};
    BrixelizerStatus status{};
    SceneShaderReader shader_reader;
    std::span<const render_data::TextureDescriptor> texture_catalog;
    CompletionSet completion;
    std::uint64_t last_frame{},last_light{},last_scene{},successful_updates{};
    bool history_valid{},usable{true};
#if HS_WITH_BRIXELIZER
    // These opaque SDK CPU structures are tens of MiB; never put them on stack.
    std::unique_ptr<FfxBrixelizerContext> sdf;
    std::unique_ptr<FfxBrixelizerGIContext> gi;
    std::vector<std::max_align_t> backend_memory;
    FfxInterface backend{};
    brixelizer_native::BackendSlotsOwner backend_slots;
    bool sdf_created{},gi_created{};
    using BufferKey=std::pair<ID3D12Resource *,std::uint64_t>;
    struct RegisteredBuffer
    {
        std::uint32_t id{},pending_cascades{};
        ComPtr<ID3D12Resource> resource;
        std::vector<std::shared_ptr<const void>> owners;
        CompletionSet completion;
        bool retired{};
    };
    std::map<BufferKey,RegisteredBuffer> registered_buffers;
    std::vector<BufferKey> wanted_buffer_keys;
    std::vector<std::uint8_t> validated_geometry_flags;
    using InstanceKey=std::tuple<std::uint32_t,std::uint32_t,std::uint64_t>;
    struct StaticInstance
    {
        FfxBrixelizerInstanceID native_id{};
        BrixelizerSceneInstance instance{};
        SoftwareSceneGeometry geometry{};
    };
    std::map<InstanceKey,StaticInstance> static_instances;
    std::vector<InstanceKey> wanted_static_keys;
    std::vector<std::uint32_t> static_invalidation_masks;
#endif
    ~BrixelizerStorage()
    {
        if(mapped&&constants&&constants->resource)constants->resource->Unmap(0,nullptr);
#if HS_WITH_BRIXELIZER
        backend_slots.reset();
        if(gi_created)ffxBrixelizerGIContextDestroy(gi.get());
        if(sdf_created)ffxBrixelizerContextDestroy(sdf.get());
#endif
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(std::uint32_t slot,std::uint32_t stage,std::uint32_t offset)const
    {auto h=descriptors->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T((slot*kStages+stage)*kBrixelizerDescriptorCount+offset)*descriptor_stride;return h;}
    D3D12_CPU_DESCRIPTOR_HANDLE Export(std::uint32_t index)const
    {auto h=exports->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(index)*descriptor_stride;return h;}
    void Srv(std::uint32_t slot,std::uint32_t stage,std::uint32_t offset,ID3D12Resource *resource,DXGI_FORMAT format)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=format;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(resource,&d,Cpu(slot,stage,offset));
    }
    void Uav(std::uint32_t slot,std::uint32_t stage,std::uint32_t offset,ID3D12Resource *resource,DXGI_FORMAT format)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=format;d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(resource,nullptr,&d,Cpu(slot,stage,offset));
    }
    std::uint64_t GpuBytes()const
    {
        std::uint64_t total=Bytes(constants)+Bytes(atlas)+Bytes(brick_aabbs)+Bytes(scratch);
        for(const auto &x:screens)total+=Bytes(x);
        for(const auto &x:noise_textures)total+=Bytes(x);
        for(const auto &x:noise_uploads)total+=Bytes(x);
        for(const auto &x:trees)total+=Bytes(x);
        for(const auto &x:maps)total+=Bytes(x);
        for(const auto &x:retired_scratch)total+=Bytes(x);
#if HS_WITH_BRIXELIZER
        total+=brixelizer_native::NativeResourceBytes(backend);
        if(backend_slots)total+=brixelizer_native::BackendGpuBytes(*backend_slots);
#endif
        return total;
    }
};

BrixelizerNativeSupport QueryBrixelizerNativeSupport(ID3D12Device *device) noexcept
{
#if !HS_WITH_BRIXELIZER
    (void)device;return {false,false,"Pinned Brixelizer SDK is not compiled."};
#else
    if(!device)return {true,false,"D3D12 device is absent."};
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_6};
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 waves{};
    if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&model,sizeof(model)))||model.HighestShaderModel<D3D_SHADER_MODEL_6_6)
        return {true,false,"Brixelizer requires compute shader model6.6."};
    if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1,&waves,sizeof(waves)))||!waves.WaveOps||
        waves.WaveLaneCountMin>64||waves.WaveLaneCountMax<32)
        return {true,false,"Brixelizer requires native wave operations."};
    if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,&options,sizeof(options)))||options.ResourceBindingTier<D3D12_RESOURCE_BINDING_TIER_2)
        return {true,false,"Brixelizer native bindless backend requires resource binding tier2."};
    for(auto format:{DXGI_FORMAT_R8_UNORM,DXGI_FORMAT_R11G11B10_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT})
    {D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
        if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)))||
            !(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)||!(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD))
            return {true,false,"Brixelizer native atlas/GI format requires typed UAV loads and stores."};}
    return {true,true,"Pinned CS6.6 compute Brixelizer is supported."};
#endif
}

#if HS_WITH_BRIXELIZER
namespace
{
Result Sdk(std::string_view operation,FfxErrorCode code)
{return code==FFX_OK?Result::Success():Fail(ErrorCode::TaskFailed,std::string(operation)+" failed (FfxErrorCode "+std::to_string(code)+").");}
FfxResource Resource(ID3D12Resource *resource,FfxResourceStates state,std::uint32_t stride=0)
{
    auto desc=ffxGetResourceDescriptionDX12(resource);
    if(stride)desc.stride=stride;
    return ffxGetResourceDX12(resource,desc,nullptr,state);
}
FfxBrixelizerResources SdfResources(BrixelizerStorage &s,FfxResourceStates state)
{
    FfxBrixelizerResources r{};r.sdfAtlas=Resource(s.atlas->resource.Get(),state);
    r.brickAABBs=Resource(s.brick_aabbs->resource.Get(),state,FFX_BRIXELIZER_BRICK_AABBS_STRIDE);
    for(std::size_t i=0;i<s.maps.size();++i)
    {
        r.cascadeResources[i].aabbTree=Resource(s.trees[i]->resource.Get(),state,FFX_BRIXELIZER_CASCADE_AABB_TREE_STRIDE);
        r.cascadeResources[i].brickMap=Resource(s.maps[i]->resource.Get(),state,FFX_BRIXELIZER_CASCADE_BRICK_MAP_STRIDE);
    }
    return r;
}
FfxBrixelizerGIInternalResolution Resolution(std::uint32_t percent)
{
    switch(percent){case 25:return FFX_BRIXELIZER_GI_INTERNAL_RESOLUTION_25_PERCENT;
    case 50:return FFX_BRIXELIZER_GI_INTERNAL_RESOLUTION_50_PERCENT;
    case 75:return FFX_BRIXELIZER_GI_INTERNAL_RESOLUTION_75_PERCENT;
    case 100:return FFX_BRIXELIZER_GI_INTERNAL_RESOLUTION_NATIVE;}
    throw std::runtime_error("Unsupported authored Brixelizer internal resolution.");
}
Result CreateGi(BrixelizerStorage &s)
{
    s.gi=std::make_unique<FfxBrixelizerGIContext>();
    FfxBrixelizerGIContextDescription desc{};desc.backendInterface=s.backend;
    desc.flags=FFX_BRIXELIZER_GI_FLAG_DEPTH_INVERTED;
    desc.internalResolution=Resolution(s.settings.internal_resolution_percent);desc.displaySize={s.extent.width,s.extent.height};
    brixelizer_native::ShaderReadScope shaders(s.shader_reader);
    auto result=Sdk("ffxBrixelizerGIContextCreate",ffxBrixelizerGIContextCreate(s.gi.get(),&desc));
    if(result)s.gi_created=true;return result;
}
Result CreateSdf(BrixelizerStorage &s)
{
    s.sdf=std::make_unique<FfxBrixelizerContext>();
    FfxBrixelizerContextDescription desc{};desc.backendInterface=s.backend;desc.numCascades=s.settings.logical_cascades;
    for(std::uint32_t i=0;i<desc.numCascades;++i)
    {desc.cascadeDescs[i].flags=static_cast<FfxBrixelizerCascadeFlag>(FFX_BRIXELIZER_CASCADE_STATIC|FFX_BRIXELIZER_CASCADE_DYNAMIC);
        desc.cascadeDescs[i].voxelSize=std::ldexp(s.settings.first_voxel_size_m,int(i));}
    brixelizer_native::ShaderReadScope shaders(s.shader_reader);
    auto result=Sdk("ffxBrixelizerContextCreate",ffxBrixelizerContextCreate(&desc,s.sdf.get()));
    if(result)s.sdf_created=true;return result;
}
bool SameStatic(const BrixelizerStorage::StaticInstance &old,const BrixelizerSceneInstance &in,const SoftwareSceneGeometry &g)
{
    const auto &a=old.instance;const auto &b=old.geometry;
    return a.material_id==in.material_id&&a.material_revision==in.material_revision&&a.deformation_revision==in.deformation_revision&&
        a.domain==in.domain&&std::memcmp(&a.world,&in.world,sizeof(a.world))==0&&
        std::memcmp(&a.bounds_min,&in.bounds_min,sizeof(a.bounds_min))==0&&std::memcmp(&a.bounds_max,&in.bounds_max,sizeof(a.bounds_max))==0&&
        b.generation==g.generation&&b.current_revision==g.current_revision&&b.vertices==g.vertices&&b.indices==g.indices&&
        b.vertex_byte_offset==g.vertex_byte_offset&&b.index_byte_offset==g.index_byte_offset&&b.vertex_stride==g.vertex_stride&&
        b.vertex_count==g.vertex_count&&b.index_count==g.index_count&&b.index_format==g.index_format;
}
Result ValidateGeometry(const RenderGraphBuilder *graph,const SoftwareSceneGeometry &g)
{
    const auto index_bytes=g.index_format==DXGI_FORMAT_R32_UINT?4u:g.index_format==DXGI_FORMAT_R16_UINT?2u:0u;
    if(!g.stable_key||!g.generation||g.vertex_count<3||g.index_count<3||g.index_count%3||g.vertex_stride<12||g.vertex_stride%4||
        g.vertex_byte_offset%4||!index_bytes||g.index_byte_offset%index_bytes||g.vertex_byte_offset>UINT32_MAX||g.index_byte_offset>UINT32_MAX||
        !g.vertices||!g.indices||(graph&&(!g.vertex_version.resource||!g.index_version.resource))||
        (graph&&(graph->Resolve(g.vertex_version.resource)!=g.vertices||
            graph->Resolve(g.index_version.resource)!=g.indices)))
        return Fail(ErrorCode::InvalidArgument,"Brixelizer requires actual generation-scoped indexed float3 geometry ranges.");
    for(const auto &entry:{std::tuple{g.vertices,g.vertex_byte_offset,std::uint64_t(g.vertex_count-1)*g.vertex_stride+12},
        std::tuple{g.indices,g.index_byte_offset,std::uint64_t(g.index_count)*index_bytes}})
    {
        const auto desc=std::get<0>(entry)->GetDesc();
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||std::get<1>(entry)>desc.Width||std::get<2>(entry)>desc.Width-std::get<1>(entry))
            return Fail(ErrorCode::InvalidArgument,"Brixelizer geometry range exceeds its native buffer.");
    }
    return Result::Success();
}
Result RegisterBuffer(BrixelizerStorage &s,ID3D12Resource *resource,std::uint64_t generation,
    const std::vector<std::shared_ptr<const void>> &owners,std::uint32_t &id)
{
    const BrixelizerStorage::BufferKey key{resource,generation};
    auto found=s.registered_buffers.find(key);
    if(found!=s.registered_buffers.end())
    {found->second.retired=false;found->second.pending_cascades=0;id=found->second.id;return Result::Success();}
    BrixelizerStorage::RegisteredBuffer registered{};registered.resource=resource;registered.owners=owners;
    FfxBrixelizerBufferDescription desc{Resource(resource,FFX_RESOURCE_STATE_COMPUTE_READ,4),&registered.id};
    if(auto result=Sdk("ffxBrixelizerRegisterBuffers",ffxBrixelizerRegisterBuffers(s.sdf.get(),&desc,1));!result)return result;
    id=registered.id;s.registered_buffers.emplace(key,std::move(registered));return Result::Success();
}
Result CreateInstance(BrixelizerStorage &s,const BrixelizerSceneInstance &in,const SoftwareSceneGeometry &g,
    const std::vector<std::shared_ptr<const void>> &owners,FfxBrixelizerInstanceID &id)
{
    FfxBrixelizerInstanceDescription desc{};desc.maxCascade=s.settings.logical_cascades-1;
    desc.aabb={{in.bounds_min.x,in.bounds_min.y,in.bounds_min.z},{in.bounds_max.x,in.bounds_max.y,in.bounds_max.z}};
    // Native 3x4 transforms column vectors; transpose DirectX row-vector world.
    for(std::uint32_t row=0;row<3;++row)for(std::uint32_t column=0;column<4;++column)
        desc.transform[row*4+column]=in.world.m[column][row];
    desc.indexFormat=g.index_format==DXGI_FORMAT_R32_UINT?FFX_INDEX_TYPE_UINT32:FFX_INDEX_TYPE_UINT16;
    if(auto result=RegisterBuffer(s,g.indices,g.generation,owners,desc.indexBuffer);!result)return result;
    if(auto result=RegisterBuffer(s,g.vertices,g.generation,owners,desc.vertexBuffer);!result)return result;
    desc.indexBufferOffset=std::uint32_t(g.index_byte_offset);desc.triangleCount=g.index_count/3;
    desc.vertexBufferOffset=std::uint32_t(g.vertex_byte_offset);desc.vertexStride=g.vertex_stride;desc.vertexCount=g.vertex_count;
    desc.vertexFormat=FFX_SURFACE_FORMAT_R32G32B32_FLOAT;
    desc.flags=g.dynamic?FFX_BRIXELIZER_INSTANCE_FLAG_DYNAMIC:FFX_BRIXELIZER_INSTANCE_FLAG_NONE;
    desc.outInstanceID=&id;
    return Sdk("ffxBrixelizerCreateInstances",ffxBrixelizerCreateInstances(s.sdf.get(),&desc,1));
}
}
#endif

struct BrixelizerGiSystem::Impl
{
    std::shared_ptr<BrixelizerStorage> storage;
    struct Imported { Allocation allocation;ResourceHandle handle; };
    struct Pending
    {
        const RenderGraphBuilder *graph{};
        std::uint32_t slot{},updated_cascade{};
        std::uint64_t frame{},scene_revision{},light_revision{};
        bool prepared{};
        const SoftwareSceneGeometry *prepared_geometries{};
        const BrixelizerSceneInstance *prepared_instances{};
        BrixelizerFrameRecordData record;
        BrixelizerGraphLayout *capture{};
        const BrixelizerGraphLayout *retained{};
        bool import_failed{};
        bool lighting{},composite{},cache_injection{},noise_upload{};
        BrixelizerConstants constants{};
        AmbientOcclusionGuide original_environment;
        ResourceVersion constant_version;
        std::vector<Imported> imports;
        std::vector<ResourceVersion> sdf_versions;
        std::vector<ResourceVersion> sdf_read_versions;
        std::array<ResourceVersion,ScreenCount> screens{};
#if HS_WITH_BRIXELIZER
        std::shared_ptr<FfxBrixelizerBakedUpdateDescription> baked;
#endif
    } pending;
    ResourceVersion Import(RenderGraphBuilder &graph,const Allocation &allocation,std::string_view name)
    {
        const auto desc=allocation->resource->GetDesc();ResourceHandle handle;
        const auto index=pending.imports.size();
        if(pending.retained)
        {
            if(index>=pending.retained->imports.size()){pending.import_failed=true;return {};}
            handle=pending.retained->imports[index];
            const bool bound=desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER
                ? graph.BindBuffer(handle,{allocation->resource.Get(),allocation->graph_completion,
                    allocation->graph_queue,{},allocation->NativeContract()},allocation->graph_access)
                : graph.BindTexture(handle,{allocation->resource.Get(),allocation->graph_completion,
                    allocation->graph_queue,{},allocation->NativeContract()},allocation->graph_access);
            pending.import_failed|=!bound;
        }
        else if(desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER)
            handle=graph.ImportBuffer({allocation->resource.Get(),allocation->graph_completion,
                allocation->graph_queue,{},allocation->NativeContract()},allocation->graph_access,name);
        else handle=graph.ImportTexture({allocation->resource.Get(),allocation->graph_completion,
            allocation->graph_queue,{},allocation->NativeContract()},allocation->graph_access,name);
        pending.imports.push_back({allocation,handle});
        if(pending.capture)pending.capture->imports.push_back(handle);
        return {handle,0};
    }
    std::optional<PassBuilder> Pass(RenderGraphBuilder &graph,std::size_t slot,
        std::uint32_t id,std::string_view name)
    {
        if(pending.retained)return graph.BindPass(pending.retained->passes[slot],id);
        if(pending.capture)pending.capture->passes[slot]=graph.PassCount();
        return graph.AddPass(id,name,QueueHint::Direct);
    }
};
namespace
{
std::vector<SceneGeometryReadToken> UniqueGeometryReads(
    const std::vector<SceneGeometryReadToken> &reads)
{
    std::vector<SceneGeometryReadToken> unique;
    unique.reserve(reads.size());
    for (const auto &read : reads) if (read.version.resource) unique.push_back(read);
    const auto key=[](const SceneGeometryReadToken &read) {
        return std::tuple{read.version.resource.value,read.version.resource.kind,
            read.version.ordinal,read.access};
    };
    std::ranges::sort(unique,[&](const auto &left,const auto &right) {
        return key(left)<key(right);
    });
    unique.erase(std::unique(unique.begin(),unique.end()),unique.end());
    return unique;
}
}
bool MatchesBrixelizerGraphLayout(const BrixelizerFrameRecordData &current,
    const BrixelizerGraphLayout &layout) noexcept
{
    const auto &saved=layout.record;
    // The SDF pass declares each native geometry ResourceVersion only once.
    // Leaf and instance multiplicity changes SDK update data, not graph uses.
    return layout.valid&&current.frame_slot==saved.frame_slot&&current.scratch_width==saved.scratch_width&&
        current.physical_cascades==saved.physical_cascades&&current.extent==saved.extent&&
        current.history_valid==saved.history_valid&&current.noise_upload==saved.noise_upload&&
        current.unique_geometry_reads==saved.unique_geometry_reads;
}
BrixelizerGiSystem::BrixelizerGiSystem():impl_(std::make_unique<Impl>()) {}
BrixelizerGiSystem::~BrixelizerGiSystem()=default;
std::uint64_t BrixelizerCandidate::AllocatedBytes()const noexcept {return storage_?storage_->GpuBytes():0;}
std::uint64_t BrixelizerGiSystem::AllocatedBytes()const noexcept {return impl_->storage?impl_->storage->GpuBytes():0;}
float BrixelizerGiSystem::FinestVoxelSizeMeters()const noexcept
{
    const auto *storage=impl_->storage.get();
    return storage&&storage->usable&&storage->status.native_ready
        ? storage->settings.first_voxel_size_m : 0.0f;
}
CompletionSet BrixelizerGiSystem::RequiredCompletion()const noexcept {return impl_->storage?impl_->storage->completion:CompletionSet{};}
BrixelizerStatus BrixelizerGiSystem::Status()const
{
    if(auto s=impl_->storage){auto status=s->status;status.gpu_bytes=s->GpuBytes();return status;}
    BrixelizerStatus status{};status.compiled=HS_WITH_BRIXELIZER!=0;status.reason="Brixelizer SoftwareHybrid GI is Off.";return status;
}
Result BrixelizerGiSystem::Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,ID3D12RootSignature *root,
    ViewExtent extent,std::uint32_t frame_slots,const BrixelizerSettings &settings,const SceneShaderReader &reader,
    std::span<const render_data::TextureDescriptor> texture_catalog,BrixelizerCandidate &candidate)const
{
    candidate={};
    if(!settings.enabled)return Result::Success();
#if !HS_WITH_BRIXELIZER
    (void)device;(void)allocator;(void)root;(void)extent;(void)frame_slots;(void)reader;(void)texture_catalog;
    return Fail(ErrorCode::InvalidState,"Native Brixelizer was excluded by HS_WITH_BRIXELIZER=OFF.");
#else
    if(!device||!allocator||!root||!reader||!Extent(extent)||!frame_slots||frame_slots>=FFX_MAX_QUEUED_FRAMES||
        !settings.logical_cascades||settings.logical_cascades>FFX_BRIXELIZER_MAX_CASCADES/3||
        !std::isfinite(settings.first_voxel_size_m)||settings.first_voxel_size_m<=0||!settings.max_references||
        !settings.triangle_swap_bytes||!settings.max_bricks_per_bake||!settings.memory_budget_bytes)
        return Fail(ErrorCode::InvalidArgument,"Invalid fixed Brixelizer extent, frame ring, cascade settings or memory budget.");
    const auto support=QueryBrixelizerNativeSupport(device);
    if(!support.supported)return Fail(ErrorCode::InvalidState,std::string(support.reason));
    auto s=std::make_shared<BrixelizerStorage>();s->device=device;s->allocator=allocator;s->root=root;s->extent=extent;
    s->settings=settings;s->slots.resize(frame_slots);s->shader_reader=reader;s->texture_catalog=texture_catalog;s->status.compiled=true;s->status.enabled=true;
    for(auto &slot:s->slots)slot.baked_update=std::make_shared_for_overwrite<FfxBrixelizerBakedUpdateDescription>();
    if(ffxBrixelizerGetEffectVersion()!=FFX_SDK_MAKE_VERSION(1,0,0)||ffxBrixelizerGIGetEffectVersion()!=FFX_SDK_MAKE_VERSION(1,0,1))
        return Fail(ErrorCode::InvalidState,"Linked Brixelizer effect versions do not match the pinned SDK1.1.4 contract.");
    const auto backend_bytes=ffxGetScratchMemorySizeDX12(2);
    s->backend_memory.resize((backend_bytes+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t));
    if(auto result=Sdk("ffxGetInterfaceDX12",ffxGetInterfaceDX12(&s->backend,ffxGetDeviceDX12(device),s->backend_memory.data(),backend_bytes,2));!result)return result;
    brixelizer_native::RequireSelectedShaderProducts(s->backend);
    if(auto result=CreateSdf(*s);!result)return result;
    if(auto result=CreateGi(*s);!result)return result;
    FfxBrixelizerRawContext *raw_context{};
    if(auto result=Sdk("ffxBrixelizerGetRawContext",ffxBrixelizerGetRawContext(s->sdf.get(),&raw_context));!result)return result;
    if(auto result=brixelizer_native::CreateSlots(s->backend,raw_context,frame_slots,s->backend_slots);!result)return result;
    s->status.cpu_bytes=backend_bytes+sizeof(FfxBrixelizerContext)+sizeof(FfxBrixelizerGIContext)+
        s->slots.size()*sizeof(FfxBrixelizerBakedUpdateDescription);
    s->status.physical_cascades=settings.logical_cascades*3;
    if(auto result=Texture(device,allocator,{FFX_BRIXELIZER_STATIC_CONFIG_SDF_ATLAS_SIZE,FFX_BRIXELIZER_STATIC_CONFIG_SDF_ATLAS_SIZE},
        DXGI_FORMAT_R8_UNORM,s->atlas,FFX_BRIXELIZER_STATIC_CONFIG_SDF_ATLAS_SIZE);!result)return result;
    if(auto result=Buffer(device,allocator,FFX_BRIXELIZER_BRICK_AABBS_SIZE,false,s->brick_aabbs);!result)return result;
    s->maps.resize(s->status.physical_cascades);s->trees.resize(s->status.physical_cascades);
    for(std::uint32_t i=0;i<s->status.physical_cascades;++i)
    {
        if(auto result=Buffer(device,allocator,FFX_BRIXELIZER_CASCADE_BRICK_MAP_SIZE,false,s->maps[i]);!result)return result;
        if(auto result=Buffer(device,allocator,FFX_BRIXELIZER_CASCADE_AABB_TREE_SIZE,false,s->trees[i]);!result)return result;
    }
    for(std::uint32_t i=0;i<ScreenCount;++i)
        if(auto result=Texture(device,allocator,extent,kScreenFormats[i],s->screens[i],1,i==CacheInjection);!result)return result;
    for(std::uint32_t i=0;i<16;++i)
    {
        const auto id="render/texture/brixelizer_noise_"+std::to_string(i)+".rpkg";
        std::vector<std::byte> package;if(auto result=reader(id,package);!result)return result;
        render_data::TexturePackageView view;
        try
        {
            if(texture_catalog.empty()) view=render_data::ReadTexturePackage(package,MakeAssetId(id));
            else
            {
                const auto asset=MakeAssetId(id);
                const auto found=std::lower_bound(texture_catalog.begin(),texture_catalog.end(),asset.value,
                    [](const auto &entry,std::uint64_t value){return entry.asset_id.value<value;});
                if(found==texture_catalog.end() || found->asset_id.value!=asset.value)
                    throw std::runtime_error("Compiled Brixelizer noise texture is absent");
                view=render_data::BindTextureDescriptor(package,*found);
            }
        }
        catch(const std::exception &e){return Fail(ErrorCode::InvalidArgument,std::string("Invalid selected Brixelizer noise package: ")+e.what());}
        const auto &d=view.description;
        if(d.width!=128||d.height!=128||d.array_size!=1||d.mip_count!=1||d.format!=render_data::kTextureFormatRgba16Float||
            view.subresources.size()!=1||view.subresources[0].row_pitch!=1024||view.subresources[0].slice_pitch!=131072||view.subresources[0].bytes.size()!=131072)
            return Fail(ErrorCode::InvalidArgument,"Selected Brixelizer noise must be exact128x128 RGBA16F pinned-sampler output.");
        if(auto result=Texture(device,allocator,{128,128},DXGI_FORMAT_R16G16B16A16_FLOAT,s->noise_textures[i]);!result)return result;
        const auto desc=s->noise_textures[i]->resource->GetDesc();UINT64 bytes{};UINT rows{};UINT64 row_bytes{};
        device->GetCopyableFootprints(&desc,0,1,0,&s->noise_footprints[i],&rows,&row_bytes,&bytes);
        if(auto result=Buffer(device,allocator,bytes,true,s->noise_uploads[i]);!result)return result;
        std::byte *mapped{};const auto map_error=s->noise_uploads[i]->resource->Map(0,nullptr,reinterpret_cast<void **>(&mapped));
        if(FAILED(map_error))return Native("Map Brixelizer noise upload",map_error);
        for(UINT y=0;y<rows;++y)std::memcpy(mapped+s->noise_footprints[i].Offset+SIZE_T(y)*s->noise_footprints[i].Footprint.RowPitch,
            view.subresources[0].bytes.data()+SIZE_T(y)*1024,SIZE_T(row_bytes));
        s->noise_uploads[i]->resource->Unmap(0,nullptr);
    }
    if(auto result=Buffer(device,allocator,std::uint64_t(frame_slots)*kStages*kConstantStride,true,s->constants);!result)return result;
    auto error=s->constants->resource->Map(0,nullptr,reinterpret_cast<void **>(&s->mapped));if(FAILED(error))return Native("Map Brixelizer constants",error);
    s->descriptor_stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=frame_slots*kStages*kBrixelizerDescriptorCount;
    error=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(s->descriptors.ReleaseAndGetAddressOf()));if(FAILED(error))return Native("Create Brixelizer descriptor banks",error);
    heap.NumDescriptors=ScreenCount;error=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(s->exports.ReleaseAndGetAddressOf()));if(FAILED(error))return Native("Create Brixelizer output views",error);
    for(std::uint32_t i=0;i<ScreenCount;++i)
    {D3D12_SHADER_RESOURCE_VIEW_DESC desc{};desc.Format=kScreenFormats[i];desc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;desc.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(s->screens[i]->resource.Get(),&desc,s->Export(i));}
    for(std::uint32_t i=0;i<kStages;++i)
    {std::vector<std::byte> bytes;if(auto result=reader(kShaderProducts[i],bytes);!result)return result;
        if(bytes.empty())return Fail(ErrorCode::InvalidArgument,"Selected Generation has empty Brixelizer adapter DXIL.");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=root;desc.CS={bytes.data(),bytes.size()};
        error=device->CreateComputePipelineState(&desc,IID_PPV_ARGS(s->pipelines[i].ReleaseAndGetAddressOf()));if(FAILED(error))return Native("Create Brixelizer adapter pipeline",error);}
    std::vector<std::byte> vs,ps;
    if(auto result=reader("render/shader/fullscreen_vs.dxil",vs);!result)return result;
    if(auto result=reader("render/shader/brixelizer_cache_direct_ps.dxil",ps);!result)return result;
    if(vs.empty()||ps.empty())return Fail(ErrorCode::InvalidArgument,"Selected Generation lacks actual deferred cache injection shaders.");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};graphics.pRootSignature=root;
    graphics.VS={vs.data(),vs.size()};graphics.PS={ps.data(),ps.size()};graphics.SampleMask=UINT_MAX;
    graphics.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;graphics.NumRenderTargets=1;
    graphics.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;graphics.SampleDesc.Count=1;
    graphics.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    auto &blend=graphics.BlendState.RenderTarget[0];blend.SrcBlend=D3D12_BLEND_ONE;blend.DestBlend=D3D12_BLEND_ZERO;
    blend.BlendOp=D3D12_BLEND_OP_ADD;blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlendAlpha=D3D12_BLEND_ZERO;
    blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.LogicOp=D3D12_LOGIC_OP_NOOP;
    graphics.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;graphics.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    graphics.RasterizerState.DepthClipEnable=TRUE;graphics.DepthStencilState.DepthEnable=FALSE;
    graphics.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;graphics.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    graphics.DepthStencilState.StencilReadMask=graphics.DepthStencilState.StencilWriteMask=D3D12_DEFAULT_STENCIL_READ_MASK;
    graphics.DepthStencilState.FrontFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
    graphics.DepthStencilState.BackFace=graphics.DepthStencilState.FrontFace;
    error=device->CreateGraphicsPipelineState(&graphics,IID_PPV_ARGS(s->cache_pipeline.ReleaseAndGetAddressOf()));
    if(FAILED(error))return Native("Create actual deferred cache injection pipeline",error);
    D3D12_DESCRIPTOR_HEAP_DESC rtv{};rtv.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;rtv.NumDescriptors=1;
    error=device->CreateDescriptorHeap(&rtv,IID_PPV_ARGS(s->cache_rtv.ReleaseAndGetAddressOf()));if(FAILED(error))return Native("Create cache injection RTV",error);
    device->CreateRenderTargetView(s->screens[CacheInjection]->resource.Get(),nullptr,s->cache_rtv->GetCPUDescriptorHandleForHeapStart());
    // Query real bake scratch with exactly the selected update budgets. Bake
    // mutates CPU invalidation scheduling, so use its result for the first frame
    // only after scene registration; here it contains no instance invalidations.
    auto baked=std::make_unique<FfxBrixelizerBakedUpdateDescription>();FfxBrixelizerUpdateDescription update{};
    std::size_t scratch_bytes{};update.resources=SdfResources(*s,FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    update.maxReferences=settings.max_references;update.triangleSwapSize=settings.triangle_swap_bytes;
    update.maxBricksPerBake=settings.max_bricks_per_bake;update.outScratchBufferSize=&scratch_bytes;
    if(auto result=Sdk("ffxBrixelizerBakeUpdate",ffxBrixelizerBakeUpdate(s->sdf.get(),&update,baked.get()));!result)return result;
    if(!scratch_bytes)return Fail(ErrorCode::TaskFailed,"Pinned BakeUpdate returned zero scratch bytes.");
    if(auto result=Buffer(device,allocator,scratch_bytes,false,s->scratch);!result)return result;
    s->status.scratch_bytes=scratch_bytes;
    if(s->GpuBytes()+AllocatedBytes()>settings.memory_budget_bytes)
        return Fail(ErrorCode::InvalidState,"Actual Brixelizer native pools, scratch and old/candidate overlap exceed the selected memory budget.");
    s->status.native_ready=true;s->status.reason="Pinned native Brixelizer prepared; GPU submission is unverified.";
    candidate.storage_=std::move(s);return Result::Success();
#endif
}
Result BrixelizerGiSystem::Commit(BrixelizerCandidate &&candidate,const CompletionSet &completed)
{
    if(impl_->pending.graph)return Fail(ErrorCode::InvalidState,"Complete the pending Brixelizer graph before publication.");
    if(auto old=impl_->storage;old&&!old->completion.IsSatisfiedBy(completed))return Fail(ErrorCode::InvalidState,"Brixelizer publication requires all old queue uses completed.");
    impl_->storage=std::move(candidate.storage_);return Result::Success();
}
Result BrixelizerGiSystem::PrepareFrameRecordData(const BrixelizerSceneInputs &in,BrixelizerFrameRecordData &out)
{
    out={};auto s=impl_->storage;if(!s)return Result::Success();
#if !HS_WITH_BRIXELIZER
    (void)in;return Fail(ErrorCode::InvalidState,"Brixelizer native package is unavailable.");
#else
    if(!s->usable||impl_->pending.graph||impl_->pending.prepared)
        return Fail(ErrorCode::InvalidState,"Brixelizer is faulted or already has a prepared frame.");
    if(s->noise_uploaded)for(auto &upload:s->noise_uploads)if(upload&&upload->graph_completion.IsSatisfiedBy(in.completed))upload.reset();
    if(in.frame_slot>=s->slots.size()||!s->slots[in.frame_slot].completion.IsSatisfiedBy(in.completed)||!Finite(in.sdf_center)||
        (s->successful_updates&&in.frame_index<=s->last_frame)||in.frame_index>UINT32_MAX||in.owners.empty()||
        in.geometries.size()>UINT32_MAX||in.instances.size()>UINT32_MAX)
        return Fail(ErrorCode::InvalidArgument,"Brixelizer scene requires a completed frame slot, monotonic frame and backed owners.");
    // Validate every input before mutating SDK CPU identity and invalidations.
    auto &wanted_static=s->wanted_static_keys;
    wanted_static.clear();wanted_static.reserve(in.instances.size());
    auto &wanted_buffers=s->wanted_buffer_keys;
    wanted_buffers.clear();wanted_buffers.reserve(in.geometries.size()*2u);
    auto &validated_geometries=s->validated_geometry_flags;
    validated_geometries.assign(in.geometries.size(),0);
    BrixelizerStatus counts=s->status;counts.static_instances=counts.dynamic_instances=0;
    counts.unsupported_masked=in.excluded_masked_count;counts.unsupported_thin=in.excluded_thin_count;
    counts.excluded_translucent=in.excluded_translucent_count;
    for(const auto &instance:in.instances)
    {
        if(instance.domain==BrixelizerGeometryDomain::Translucent)
        {if(auto result=AddBrixelizerExcludedCount(counts.excluded_translucent,1);!result)return result;continue;}
        if(instance.domain==BrixelizerGeometryDomain::UnsupportedMasked)
        {if(auto result=AddBrixelizerExcludedCount(counts.unsupported_masked,1);!result)return result;continue;}
        if(instance.domain==BrixelizerGeometryDomain::UnsupportedThin)
        {if(auto result=AddBrixelizerExcludedCount(counts.unsupported_thin,1);!result)return result;continue;}
        if(instance.geometry_index>=in.geometries.size()||!instance.generation||!Finite(instance.world)||
            !Finite(instance.bounds_min)||!Finite(instance.bounds_max)||instance.bounds_min.x>instance.bounds_max.x||
            instance.bounds_min.y>instance.bounds_max.y||instance.bounds_min.z>instance.bounds_max.z)
            return Fail(ErrorCode::InvalidArgument,"Invalid Brixelizer scene instance identity, world transform or conservative bounds.");
        const auto &g=in.geometries[instance.geometry_index];
        if (!validated_geometries[instance.geometry_index])
        {
            if (auto result=ValidateGeometry(nullptr,g);!result)return result;
            validated_geometries[instance.geometry_index] = 1;
            wanted_buffers.push_back({g.vertices,g.generation});wanted_buffers.push_back({g.indices,g.generation});
        }
        if(!g.opaque&&instance.domain!=BrixelizerGeometryDomain::AuthoredCoverage)
            return Fail(ErrorCode::InvalidArgument,"Masked/thin leaves need authored coverage triangles or an explicit unsupported domain.");
        const BrixelizerStorage::InstanceKey key{instance.stable_gpu_scene_slot,instance.generation,g.stable_key};
        if(!g.dynamic)wanted_static.push_back(key);
        if(g.dynamic)++counts.dynamic_instances;else ++counts.static_instances;
    }
    // Sorted reusable keys retain the same tuple/pair ordering without one
    // separately allocated tree node per current scene key.
    std::ranges::sort(wanted_static);
    if(std::adjacent_find(wanted_static.begin(),wanted_static.end())!=wanted_static.end())
        return Fail(ErrorCode::InvalidArgument,"Duplicate static Brixelizer identity.");
    std::ranges::sort(wanted_buffers);
    wanted_buffers.erase(std::unique(wanted_buffers.begin(),wanted_buffers.end()),wanted_buffers.end());
    std::size_t invalidations{};
    for(const auto &[key,instance]:s->static_instances)
        if(!std::binary_search(wanted_static.begin(),wanted_static.end(),key))++invalidations;
    for(const auto &instance:in.instances)
    {
        if(instance.domain!=BrixelizerGeometryDomain::Opaque&&instance.domain!=BrixelizerGeometryDomain::AuthoredCoverage)continue;
        const auto &g=in.geometries[instance.geometry_index];if(g.dynamic)continue;
        auto old=s->static_instances.find({instance.stable_gpu_scene_slot,instance.generation,g.stable_key});
        if(old==s->static_instances.end())++invalidations;else if(!SameStatic(old->second,instance,g))invalidations+=2;
    }
    std::size_t buffer_count=s->registered_buffers.size();
    for(const auto &key:wanted_buffers)if(!s->registered_buffers.contains(key))++buffer_count;
    if(buffer_count>FFX_BRIXELIZER_MAX_INSTANCES||std::uint64_t(counts.static_instances)+counts.dynamic_instances+
        s->static_invalidation_masks.size()+invalidations>FFX_BRIXELIZER_MAX_INSTANCES)
        return Fail(ErrorCode::InvalidState,"Native buffer IDs or pending old/new static invalidations exceed the pinned capacity before mutation.");
    impl_->pending={};auto &p=impl_->pending;p.slot=in.frame_slot;p.frame=in.frame_index;p.scene_revision=in.scene_revision;
    p.prepared_geometries=in.geometries.data();p.prepared_instances=in.instances.data();
    s->static_invalidation_masks.insert(s->static_invalidation_masks.end(),invalidations,(1u<<s->settings.logical_cascades)-1u);
    p.updated_cascade=ffxBrixelizerRawGetCascadeToUpdate(std::uint32_t(in.frame_index),s->settings.logical_cascades);
    auto &slot=s->slots[in.frame_slot];slot.owners=in.owners;slot.resources.clear();slot.binders={};slot.restore={};
    std::erase_if(s->retired_scratch,[&](const Allocation &x){return x->graph_completion.IsSatisfiedBy(in.completed);});
    auto mutation_failure=[&](Result result){s->usable=false;s->status.faulted=true;s->status.native_ready=false;
        s->status.reason="Native scene mutation failed; CompleteSubmission then drained reset is required.";return result;};
    // Retire only after all referring instances were removed, every native
    // cascade invalidation was submitted and every snapshot/queue lease retired.
    for(auto it=s->registered_buffers.begin();it!=s->registered_buffers.end();)
    {
        auto &buffer=it->second;
        if(std::binary_search(wanted_buffers.begin(),wanted_buffers.end(),it->first))
        {buffer.retired=false;buffer.pending_cascades=0;}
        if(buffer.retired&&!buffer.pending_cascades&&buffer.completion.IsSatisfiedBy(in.completed))
        {if(auto result=Sdk("ffxBrixelizerUnregisterBuffers",ffxBrixelizerUnregisterBuffers(s->sdf.get(),&buffer.id,1));!result)return mutation_failure(result);
            it=s->registered_buffers.erase(it);}else ++it;
    }
    for(auto it=s->static_instances.begin();it!=s->static_instances.end();)
    {
        if(!std::binary_search(wanted_static.begin(),wanted_static.end(),it->first))
        {if(auto result=Sdk("ffxBrixelizerDeleteInstances",ffxBrixelizerDeleteInstances(s->sdf.get(),&it->second.native_id,1));!result)return mutation_failure(result);
            it=s->static_instances.erase(it);}else ++it;
    }
    for(const auto &instance:in.instances)
    {
        if(instance.domain!=BrixelizerGeometryDomain::Opaque&&instance.domain!=BrixelizerGeometryDomain::AuthoredCoverage)continue;
        const auto &g=in.geometries[instance.geometry_index];const BrixelizerStorage::InstanceKey key{instance.stable_gpu_scene_slot,instance.generation,g.stable_key};
        auto found=s->static_instances.find(key);
        if(!g.dynamic&&found!=s->static_instances.end()&&SameStatic(found->second,instance,g))continue;
        if(found!=s->static_instances.end())
        {if(auto result=Sdk("ffxBrixelizerDeleteInstances",ffxBrixelizerDeleteInstances(s->sdf.get(),&found->second.native_id,1));!result)return mutation_failure(result);
            s->static_instances.erase(found);}
        FfxBrixelizerInstanceID id{};
        if(auto result=CreateInstance(*s,instance,g,in.owners,id);!result)return mutation_failure(result);
        if(!g.dynamic)s->static_instances.emplace(key,BrixelizerStorage::StaticInstance{id,instance,g});
    }
    for(auto &[key,buffer]:s->registered_buffers)
        if(!std::binary_search(wanted_buffers.begin(),wanted_buffers.end(),key)&&!buffer.retired)
        {buffer.retired=true;buffer.pending_cascades=(1u<<s->settings.logical_cascades)-1u;buffer.completion=s->completion;}
    s->status=std::move(counts);
    FfxBrixelizerUpdateDescription update{};update.resources=SdfResources(*s,FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    update.frameIndex=std::uint32_t(in.frame_index);update.sdfCenter[0]=in.sdf_center.x;update.sdfCenter[1]=in.sdf_center.y;update.sdfCenter[2]=in.sdf_center.z;
    update.maxReferences=s->settings.max_references;update.triangleSwapSize=s->settings.triangle_swap_bytes;update.maxBricksPerBake=s->settings.max_bricks_per_bake;
    // Retained callbacks keep their object; the SDK clears an exclusive one.
    std::size_t scratch_bytes{};update.outScratchBufferSize=&scratch_bytes;
    p.baked=slot.baked_update.use_count()==1?slot.baked_update:
        std::make_shared_for_overwrite<FfxBrixelizerBakedUpdateDescription>();
    if(auto result=Sdk("ffxBrixelizerBakeUpdate",ffxBrixelizerBakeUpdate(s->sdf.get(),&update,p.baked.get()));!result)return mutation_failure(result);
    if(scratch_bytes>Bytes(s->scratch))
    {
        Allocation next;if(auto result=Buffer(s->device.Get(),s->allocator.Get(),scratch_bytes,false,next);!result)return mutation_failure(result);
        if(s->GpuBytes()+Bytes(next)>s->settings.memory_budget_bytes)return mutation_failure(Fail(ErrorCode::InvalidState,"Queried native scratch growth exceeds actual memory budget including retained old backing."));
        s->retired_scratch.push_back(s->scratch);s->scratch=std::move(next);
    }
    s->status.scratch_bytes=scratch_bytes;
    const auto certify=[&](const Allocation &allocation)->Result
    {return allocation?allocation->EnsureNativeContract(s->device.Get()):Result::Success();};
    for(const auto &allocation:s->screens)if(auto result=certify(allocation);!result)return mutation_failure(result);
    for(const auto &allocation:s->noise_textures)if(auto result=certify(allocation);!result)return mutation_failure(result);
    for(const auto &allocation:s->noise_uploads)if(auto result=certify(allocation);!result)return mutation_failure(result);
    for(const auto &allocation:{s->constants,s->atlas,s->brick_aabbs,s->scratch})
        if(auto result=certify(allocation);!result)return mutation_failure(result);
    for(const auto &allocation:s->trees)if(auto result=certify(allocation);!result)return mutation_failure(result);
    for(const auto &allocation:s->maps)if(auto result=certify(allocation);!result)return mutation_failure(result);
    p.record.frame_slot=in.frame_slot;p.record.frame_index=in.frame_index;p.record.scene_revision=in.scene_revision;
    p.record.static_instances=counts.static_instances;p.record.dynamic_instances=counts.dynamic_instances;
    p.record.updated_cascade=p.updated_cascade;p.record.scratch_width=s->scratch->resource->GetDesc().Width;
    p.record.geometry_count=std::uint32_t(in.geometries.size());p.record.instance_count=std::uint32_t(in.instances.size());
    p.record.physical_cascades=s->status.physical_cascades;p.record.extent=s->extent;
    p.record.history_valid=s->history_valid;p.record.noise_upload=!s->noise_uploaded;
    p.record.geometry_reads.reserve(in.geometries.size()*2);
    for(const auto &geometry:in.geometries)
    {p.record.geometry_reads.push_back({geometry.vertex_version,Access::ComputeRead});
     p.record.geometry_reads.push_back({geometry.index_version,Access::ComputeRead});}
    p.record.unique_geometry_reads=UniqueGeometryReads(p.record.geometry_reads);
    p.prepared=true;out=p.record;return Result::Success();
#endif
}
Result BrixelizerGiSystem::RegisterScene(RenderGraphBuilder &graph,const BrixelizerSceneInputs &in,
    BrixelizerSceneOutputs &out,BrixelizerGraphLayout *capture,const BrixelizerGraphLayout *retained)
{
    out={};auto s=impl_->storage;if(!s)return Result::Success();
#if !HS_WITH_BRIXELIZER
    (void)graph;(void)in;return Fail(ErrorCode::InvalidState,"Brixelizer native package is unavailable.");
#else
    auto &p=impl_->pending;
    if(!s->usable||!p.prepared||p.graph||p.slot!=in.frame_slot||p.frame!=in.frame_index||
        p.scene_revision!=in.scene_revision||p.prepared_geometries!=in.geometries.data()||
        p.prepared_instances!=in.instances.data()||p.record.geometry_count!=in.geometries.size()||
        p.record.instance_count!=in.instances.size())
        return Fail(ErrorCode::InvalidState,"Brixelizer scene does not match its once-prepared frame inputs.");
    if((capture&&retained)||(retained&&!MatchesBrixelizerGraphLayout(p.record,*retained)))
        return Fail(ErrorCode::InvalidState,"Brixelizer graph layout changed after current SDK preparation.");
    if(capture)
    {
        *capture={};capture->record=p.record;
    }
    p.capture=capture;p.retained=retained;
    for(const auto &geometry:in.geometries)
        if(auto result=ValidateGeometry(&graph,geometry);!result)return result;
    p.graph=&graph;
    const auto counts=s->status;
    auto pass=impl_->Pass(graph,0,261,"BrixelizerSdfUpdate");
    if(!pass)return Fail(ErrorCode::InvalidState,"Brixelizer SDF pass does not match retained plan.");
    if(retained){p.sdf_versions=retained->sdf_versions;p.sdf_read_versions=retained->sdf_read_versions;}
    std::size_t sdf_index{};
    std::vector<ExternalResourceAccess> native_accesses;
    const auto add_native_access = [&](ResourceHandle resource, Access access) -> Result {
        if (!resource) return Result::Success();
        const auto found = std::ranges::find_if(native_accesses, [&](const auto &entry) {
            return entry.resource.value == resource.value && entry.resource.kind == resource.kind;
        });
        if (found != native_accesses.end())
        {
            if (found->incoming != access || found->outgoing != access)
                return Fail(ErrorCode::InvalidState, "Brixelizer native pass has conflicting access tags for one physical resource.");
            return Result::Success();
        }
        native_accesses.push_back({resource, access, access});
        return Result::Success();
    };
    for(const auto &read:p.record.unique_geometry_reads)
    {
        if(!retained)pass->Read(read.version,read.access);
        if(auto result=add_native_access(read.version.resource,read.access);!result)return result;
    }
    const auto add_sdf = [&](const Allocation &allocation, bool read_by_gi) -> Result {
        const auto imported=impl_->Import(graph,allocation,"BrixelizerSdfBacking");
        const auto index=sdf_index++;
        const auto version=retained?retained->sdf_versions[index]:
            pass->ReadWrite(imported,Access::UnorderedWrite);
        if(!retained){p.sdf_versions.push_back(version);if(read_by_gi)p.sdf_read_versions.push_back(version);}
        return add_native_access(version.resource,Access::UnorderedWrite);
    };
    if(auto result=add_sdf(s->atlas,true);!result)return result;
    if(auto result=add_sdf(s->brick_aabbs,true);!result)return result;
    if(auto result=add_sdf(s->scratch,false);!result)return result;
    for(std::uint32_t i=0;i<s->status.physical_cascades;++i)
    {
        const auto tree_import=impl_->Import(graph,s->trees[i],"BrixelizerCascadeTree");
        const auto tree=retained?retained->sdf_versions[sdf_index++]:pass->ReadWrite(tree_import,Access::UnorderedWrite);
        if(!retained)++sdf_index;
        if(!retained){p.sdf_versions.push_back(tree);p.sdf_read_versions.push_back(tree);}
        if(auto result=add_native_access(tree.resource,Access::UnorderedWrite);!result)return result;
        const auto map_import=impl_->Import(graph,s->maps[i],"BrixelizerCascadeMap");
        const auto map=retained?retained->sdf_versions[sdf_index++]:pass->ReadWrite(map_import,Access::UnorderedWrite);
        if(!retained)++sdf_index;
        if(!retained){p.sdf_versions.push_back(map);p.sdf_read_versions.push_back(map);}
        if(auto result=add_native_access(map.resource,Access::UnorderedWrite);!result)return result;
    }
    // First native update initializes atlas/cascade resources; the graph models
    // the complete native write boundary. Subsequent updates preserve contents.
    if(capture){capture->sdf_versions=p.sdf_versions;capture->sdf_read_versions=p.sdf_read_versions;}
    if(p.import_failed)return Fail(ErrorCode::InvalidState,"Brixelizer SDF native import failed retained binding.");
    pass->SetExecuteOnOwner([s,baked=p.baked,slot_index=p.slot,scratch=s->scratch,
        native_accesses=std::move(native_accesses)](RenderPassContext &ctx)->Result
    {
        Result batch=Result::Success();
        const auto execute_native = [&]() -> Result {
            if(auto result=brixelizer_native::BeginFrame(*s->backend_slots,slot_index);!result)return result;
            if(auto result=brixelizer_native::BeginBatch(*s->backend_slots);!result)return result;
            const auto error=ffxBrixelizerUpdate(s->sdf.get(),baked.get(),Resource(scratch->resource.Get(),FFX_RESOURCE_STATE_UNORDERED_ACCESS,4),
                ffxGetCommandListDX12(static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList())));
            batch=brixelizer_native::BatchResult(*s->backend_slots);
            brixelizer_native::EndBatch(*s->backend_slots);
            static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList())->SetComputeRootSignature(s->root.Get());
            if(error!=FFX_OK)return Sdk("ffxBrixelizerUpdate",error);return batch;
        };
        const auto result=ctx.RecordLegacyResourceCommands(native_accesses,execute_native);
        auto *cmd=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());
        const auto restored=s->slots[slot_index].restore?s->slots[slot_index].restore(cmd):Result::Success();
        if(!result||!batch||!restored){s->usable=false;s->status.faulted=true;s->status.native_ready=false;}
        if(!result)return result;if(!batch)return batch;return restored;
    });
    out={p.sdf_versions.front(),counts.static_instances+counts.dynamic_instances};return Result::Success();
#endif
}
Result BrixelizerGiSystem::RegisterCacheInjection(RenderGraphBuilder &graph,const BrixelizerCacheInjectionInputs &in,AmbientOcclusionGuide &out)
{
    out={};auto s=impl_->storage;if(!s)return Result::Success();
    auto &p=impl_->pending;
    if(p.graph!=&graph||p.lighting||p.cache_injection||p.slot!=in.frame_slot||!s->usable||!in.bind_graphics||in.owners.empty()||
        in.render_active.width!=s->extent.width||in.render_active.height!=s->extent.height)
        return Fail(ErrorCode::InvalidArgument,"Cache injection requires matching scene/frame and actual deferred graphics bindings.");
    for(const auto &g:in.reads)if(!g.resource||!g.version.resource||graph.Resolve(g.version.resource)!=g.resource)
        return Fail(ErrorCode::InvalidArgument,"Cache injection read lacks actual graph backing.");
    auto &slot=s->slots[p.slot];slot.owners.insert(slot.owners.end(),in.owners.begin(),in.owners.end());
    std::vector<SceneGeometryReadToken> reads;reads.reserve(in.reads.size());
    for(const auto &g:in.reads)reads.push_back({g.version,g.access});
    if(p.retained&&reads!=p.retained->cache_reads)
        return Fail(ErrorCode::InvalidState,"Brixelizer cache closure reads changed after plan selection.");
    if(p.capture){p.capture->cache_reads=std::move(reads);p.capture->cache_injection=true;}
    auto pass=impl_->Pass(graph,1,266,"BrixelizerCacheDirectDiffuseEmission");
    if(!pass)return Fail(ErrorCode::InvalidState,"Brixelizer cache injection pass does not match retained plan.");
    for(const auto &g:in.reads){if(!p.retained)pass->Read(g.version,g.access);slot.resources.emplace_back(g.resource);}
    const auto target=impl_->Import(graph,s->screens[CacheInjection],"BrixelizerCacheInjection");
    if(p.capture)p.capture->initial_screens[CacheInjection]=target;
    p.screens[CacheInjection]=p.retained?p.retained->screens[CacheInjection]:pass->Write(target,Access::RenderTarget);
    if(p.capture)p.capture->screens[CacheInjection]=p.screens[CacheInjection];
    if(p.import_failed)return Fail(ErrorCode::InvalidState,"Brixelizer cache native import failed retained binding.");
    // A freshly allocated/resized cache target has no published graph state.
    // CompleteSubmission records both its actual completion lease and final
    // access on this allocation, so the allocation itself is the lifetime
    // boundary for the one required first-use discard.
    const auto &cache_allocation=*s->screens[CacheInjection];
    const bool first_storage_use=cache_allocation.graph_completion.direct==0&&
        cache_allocation.graph_completion.compute==0&&
        cache_allocation.graph_completion.copy==0;
    pass->SetExecuteChecked([s,bind=in.bind_graphics,first_storage_use](RenderPassContext &ctx)->Result
    {
        auto *cmd=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());cmd->SetGraphicsRootSignature(s->root.Get());
        if(auto result=bind(cmd);!result)return result;
        cmd->SetPipelineState(s->cache_pipeline.Get());const auto rtv=s->cache_rtv->GetCPUDescriptorHandleForHeapStart();
        cmd->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
        const D3D12_VIEWPORT viewport{0,0,float(s->extent.width),float(s->extent.height),0,1};
        const D3D12_RECT scissor{0,0,LONG(s->extent.width),LONG(s->extent.height)};
        cmd->RSSetViewports(1,&viewport);cmd->RSSetScissorRects(1,&scissor);
        if(first_storage_use)cmd->DiscardResource(s->screens[CacheInjection]->resource.Get(),nullptr);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd->DrawInstanced(3,1,0,0);return Result::Success();
    });
    p.cache_injection=true;out={p.screens[CacheInjection],s->screens[CacheInjection]->resource.Get()};return Result::Success();
}
Result BrixelizerGiSystem::RegisterLighting(RenderGraphBuilder &graph,const BrixelizerLightingInputs &in,
    BrixelizerConstants constants,BrixelizerLightingOutputs &out)
{
    out={};auto s=impl_->storage;if(!s)return Result::Success();
#if !HS_WITH_BRIXELIZER
    (void)graph;(void)in;(void)constants;return Fail(ErrorCode::InvalidState,"Brixelizer native package is unavailable.");
#else
    auto &p=impl_->pending;
    if(p.graph!=&graph||p.lighting||!s->usable||p.slot!=in.frame_slot||p.frame!=in.frame_index)
        return Fail(ErrorCode::InvalidState,"Brixelizer lighting requires the matching registered native scene.");
    if(in.render_active.width!=s->extent.width||in.render_active.height!=s->extent.height||
        !in.make_bindings||!in.restore_bindings||!in.camera.reversed_depth||in.camera.reset_history||in.sidecar_plane_count>2||
        !Finite(in.camera.view)||!Finite(in.camera.projection)||!Finite(in.camera.previous_view)||!Finite(in.camera.previous_projection)||
        !Finite(constants.inverse_view_projection)||!Finite(in.camera.position)||in.owners.empty())
        return Fail(ErrorCode::InvalidArgument,"Brixelizer fixed extent/reset/camera/closure contract is invalid; drained recreation is required on discontinuities.");
    const auto size=s->extent;
    if(!(Guide(graph,in.depth,size,DXGI_FORMAT_R32_TYPELESS)||Guide(graph,in.depth,size,DXGI_FORMAT_D32_FLOAT)||Guide(graph,in.depth,size,DXGI_FORMAT_R32_FLOAT))||
        !Guide(graph,in.normal,size,DXGI_FORMAT_R16G16_SNORM)||!Guide(graph,in.metadata,size,DXGI_FORMAT_R32G32_UINT)||
        !Guide(graph,in.motion,size,DXGI_FORMAT_R16G16_FLOAT)||!Guide(graph,in.base,size,DXGI_FORMAT_R8G8B8A8_UNORM)||
        !Guide(graph,in.material,size,DXGI_FORMAT_R8G8B8A8_UNORM)||!Guide(graph,in.cache_injection,size,DXGI_FORMAT_R16G16B16A16_FLOAT)||
        !Guide(graph,in.fallback_diffuse,size,DXGI_FORMAT_R16G16B16A16_FLOAT)||!Guide(graph,in.environment_specular,size,DXGI_FORMAT_R16G16B16A16_FLOAT)||
        !Guide(graph,in.subsurface_diffuse,size,DXGI_FORMAT_R16G16B16A16_FLOAT))
        return Fail(ErrorCode::InvalidArgument,"Brixelizer needs matching real depth/normal/motion/material and unexposed radiometry products.");
    for(std::uint32_t i=0;i<in.sidecar_plane_count;++i)
        if(!Guide(graph,in.sidecars[i],size,DXGI_FORMAT_R32_UINT)&&!Guide(graph,in.sidecars[i],size,DXGI_FORMAT_R32G32_UINT)&&
            !Guide(graph,in.sidecars[i],size,DXGI_FORMAT_R32G32B32A32_UINT))return Fail(ErrorCode::InvalidArgument,"Invalid Brixelizer material sidecar.");
    if(!in.environment_cube.resource||!in.environment_cube.version.resource||graph.Resolve(in.environment_cube.version.resource)!=in.environment_cube.resource)
        return Fail(ErrorCode::InvalidArgument,"Brixelizer environment cube is absent.");
    const auto cube=in.environment_cube.resource->GetDesc();
    if(cube.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||cube.DepthOrArraySize!=6||cube.Width!=cube.Height||cube.SampleDesc.Count!=1)
        return Fail(ErrorCode::InvalidArgument,"Brixelizer needs an actual environment cube.");
    AmbientOcclusionGuide noise_guide=in.noise;
    std::array<ResourceVersion,16> pinned_noise_versions{};
    if(p.retained&&p.retained->noise_override!=in.noise.version)
        return Fail(ErrorCode::InvalidState,"Brixelizer noise binding changed after plan selection.");
    if(p.capture)p.capture->noise_override=in.noise.version;
    if(noise_guide.resource)
    {const auto noise=noise_guide.resource->GetDesc();
        if(!noise_guide.version.resource||graph.Resolve(noise_guide.version.resource)!=noise_guide.resource||noise.Width!=128||noise.Height!=128||
            noise.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||noise.DepthOrArraySize!=1||noise.SampleDesc.Count!=1||noise.MipLevels!=1||
            (noise.Format!=DXGI_FORMAT_R8G8_UNORM&&noise.Format!=DXGI_FORMAT_R16G16_FLOAT&&noise.Format!=DXGI_FORMAT_R32G32_FLOAT&&noise.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT))
            return Fail(ErrorCode::InvalidArgument,"Noise override must be genuine128x128 two-channel Texture2D.");}
    else
    {
        std::array<ResourceVersion,16> textures;
        for(std::uint32_t i=0;i<16;++i)textures[i]=impl_->Import(graph,s->noise_textures[i],"BrixelizerPinnedNoise");
        if(!s->noise_uploaded)
        {
            auto upload=impl_->Pass(graph,2,259,"BrixelizerUploadPinnedNoise");
            if(!upload)return Fail(ErrorCode::InvalidState,"Brixelizer pinned noise upload does not match retained plan.");
            for(std::uint32_t i=0;i<16;++i)
            {const auto source=impl_->Import(graph,s->noise_uploads[i],"BrixelizerNoiseUpload");
             if(!p.retained){upload->Read(source,Access::CopySource);textures[i]=upload->Write(textures[i],Access::CopyDest);}
             else textures[i]=p.retained->noise_versions[i];}
            upload->SetExecuteChecked([s](RenderPassContext &ctx)->Result
            {auto *cmd=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());
                for(std::uint32_t i=0;i<16;++i)
                {D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=s->noise_uploads[i]->resource.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    src.PlacedFootprint=s->noise_footprints[i];dst.pResource=s->noise_textures[i]->resource.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);}return Result::Success();});
            p.noise_upload=true;
        }
        if(p.capture)p.capture->noise_versions=textures;
        if(!p.retained)for(auto texture:textures)graph.SetFinalAccess(texture,Access::ComputeRead);
        if(p.retained&&s->noise_uploaded)textures=p.retained->noise_versions;
        pinned_noise_versions=textures;
        const auto index=std::uint32_t(in.frame_index%16);noise_guide={textures[index],s->noise_textures[index]->resource.Get()};
    }
    if(s->history_valid&&in.light_revision!=s->last_light)
        return Fail(ErrorCode::InvalidState,"Native GI light revision changed; drained GI history recreation is required before registration.");
    p.light_revision=in.light_revision;
    auto &slot=s->slots[p.slot];slot.owners.insert(slot.owners.end(),in.owners.begin(),in.owners.end());slot.restore=in.restore_bindings;
    const std::array<AmbientOcclusionGuide,12> guides{in.depth,in.normal,in.metadata,in.motion,in.base,in.material,in.sidecars[0],in.sidecars[1],
        in.cache_injection,in.fallback_diffuse,in.environment_specular,in.subsurface_diffuse};
    std::vector<SceneGeometryReadToken> lighting_reads;lighting_reads.reserve(in.reads.size());
    for(const auto &read:in.reads)lighting_reads.push_back({read.version,read.access});
    std::array<ResourceVersion,12> guide_versions{};
    for(std::size_t i=0;i<guides.size();++i)guide_versions[i]=guides[i].version;
    if(p.retained&&(lighting_reads!=p.retained->lighting_reads||
        guide_versions!=p.retained->lighting_guides||
        in.environment_cube.version!=p.retained->environment_cube||
        in.sidecar_plane_count!=p.retained->sidecar_count))
        return Fail(ErrorCode::InvalidState,"Brixelizer current lighting closure changed after plan selection.");
    if(p.capture)
    {p.capture->lighting_reads=std::move(lighting_reads);p.capture->lighting_guides=guide_versions;
     p.capture->environment_cube=in.environment_cube.version;p.capture->sidecar_count=in.sidecar_plane_count;}
    constexpr DXGI_FORMAT guide_formats[]{DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16_SNORM,DXGI_FORMAT_R32G32_UINT,DXGI_FORMAT_R16G16_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R32G32B32A32_UINT,DXGI_FORMAT_R32G32B32A32_UINT,
        DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT};
    constants.render_size={float(size.width),float(size.height),1.0f/float(size.width),1.0f/float(size.height)};
    constants.frame_info={std::uint32_t(in.frame_index),s->history_valid?1u:0u,in.sidecar_plane_count,0u};
    p.constants=constants;p.original_environment=in.environment_specular;
    for(std::uint32_t stage=0;stage<kLightingStages;++stage)
    {
        const auto cb_offset=std::uint64_t(p.slot*kStages+stage)*kConstantStride;
        std::memcpy(s->mapped+cb_offset,&constants,sizeof(constants));
        D3D12_CONSTANT_BUFFER_VIEW_DESC cb{s->constants->resource->GetGPUVirtualAddress()+cb_offset,kConstantStride};
        s->device->CreateConstantBufferView(&cb,s->Cpu(p.slot,stage,0));
        for(std::uint32_t i=0;i<guides.size();++i)
        {
            auto format=guide_formats[i];if((i==6||i==7)&&guides[i].resource)format=guides[i].resource->GetDesc().Format;
            s->Srv(p.slot,stage,1+i,guides[i].resource,format);
            if(guides[i].resource)slot.resources.emplace_back(guides[i].resource);
        }
        s->Srv(p.slot,stage,13,s->screens[RawDiffuse]->resource.Get(),kScreenFormats[RawDiffuse]);
        s->Srv(p.slot,stage,14,s->screens[RawSpecular]->resource.Get(),kScreenFormats[RawSpecular]);
        s->Srv(p.slot,stage,15,nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT);
        for(std::uint32_t u=0;u<9;++u)s->Uav(p.slot,stage,16+u,nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT);
        if(stage==0)for(std::uint32_t u=0;u<4;++u)s->Uav(p.slot,stage,16+u,s->screens[u]->resource.Get(),kScreenFormats[u]);
        if(stage==1)for(std::uint32_t u=0;u<4;++u)s->Uav(p.slot,stage,20+u,s->screens[Diffuse+u]->resource.Get(),kScreenFormats[Diffuse+u]);
        if(stage==2)s->Uav(p.slot,stage,20,s->screens[CacheCurrent]->resource.Get(),kScreenFormats[CacheCurrent]);
        if(auto result=in.make_bindings(s->Cpu(p.slot,stage,0),kBrixelizerDescriptorCount,slot.binders[stage]);!result)return result;
    }
    for(std::uint32_t i=0;i<ScreenCount;++i)if(!p.screens[i].resource)
    {const auto imported=impl_->Import(graph,s->screens[i],"BrixelizerScreenBacking");
     if(p.capture)p.capture->initial_screens[i]=imported;
     p.screens[i]=p.retained?p.retained->initial_screens[i]:imported;}
    auto cb=impl_->Import(graph,s->constants,"BrixelizerAdapterConstants");
    p.constant_version=p.retained?p.retained->constant_version:cb;
    if(p.capture)p.capture->constant_version=p.constant_version;
    if(p.import_failed)return Fail(ErrorCode::InvalidState,"Brixelizer lighting native import failed retained binding.");
    auto run=[s,slot_index=p.slot,size](std::uint32_t stage)
    {return [s,slot_index,size,stage](RenderPassContext &ctx)->Result
        {auto *cmd=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());cmd->SetComputeRootSignature(s->root.Get());cmd->SetPipelineState(s->pipelines[stage].Get());
            if(auto result=s->slots[slot_index].binders[stage](cmd);!result)return result;
            cmd->Dispatch((size.width+7)/8,(size.height+7)/8,1);return Result::Success();};};
    auto guide_pass=impl_->Pass(graph,3,260,"BrixelizerGuides");
    if(!guide_pass)return Fail(ErrorCode::InvalidState,"Brixelizer guide pass does not match retained plan.");
    if(!p.retained)guide_pass->Read(cb,Access::ComputeRead);
    for(const auto &r:in.reads){if(!p.retained)guide_pass->Read(r.version,r.access);slot.resources.emplace_back(r.resource);}
    if(!p.retained)
    {for(std::uint32_t i=0;i<8;++i)if(guides[i].version.resource)guide_pass->Read(guides[i].version,Access::ComputeRead);
     for(std::uint32_t i=0;i<4;++i)p.screens[i]=guide_pass->Write(p.screens[i],Access::UnorderedWrite);}
    else for(std::uint32_t i=0;i<4;++i)p.screens[i]=p.retained->screens[i];
    guide_pass->SetExecuteChecked(run(0));
    auto cache_pass=impl_->Pass(graph,4,264,"BrixelizerCacheInjectionExport");
    if(!cache_pass)return Fail(ErrorCode::InvalidState,"Brixelizer cache export pass does not match retained plan.");
    if(!p.retained)
    {cache_pass->Read(cb,Access::ComputeRead);cache_pass->Read(in.cache_injection.version,Access::ComputeRead);
     p.screens[CacheCurrent]=cache_pass->Write(p.screens[CacheCurrent],Access::UnorderedWrite);}
    else p.screens[CacheCurrent]=p.retained->screens[CacheCurrent];
    cache_pass->SetExecuteChecked(run(2));
    auto native_pass=impl_->Pass(graph,5,262,"BrixelizerNativeGi");
    if(!native_pass)return Fail(ErrorCode::InvalidState,"Brixelizer native GI pass does not match retained plan.");
    std::set<std::tuple<std::uint32_t,ResourceKind,std::uint32_t>> native_graph_reads;
    std::vector<ExternalResourceAccess> native_accesses;
    const auto add_native_read = [&](ResourceVersion version) -> Result {
        if (!version.resource) return Result::Success();
        if (native_graph_reads.emplace(version.resource.value,version.resource.kind,version.ordinal).second)
            if(!p.retained)native_pass->Read(version,Access::ComputeRead);
        const auto found = std::ranges::find_if(native_accesses, [&](const auto &entry) {
            return entry.resource.value == version.resource.value && entry.resource.kind == version.resource.kind;
        });
        if (found != native_accesses.end())
        {
            if (found->incoming != Access::ComputeRead || found->outgoing != Access::ComputeRead)
                return Fail(ErrorCode::InvalidState, "Brixelizer GI pass has conflicting read tags for one physical resource.");
            return Result::Success();
        }
        native_accesses.push_back({version.resource,Access::ComputeRead,Access::ComputeRead});
        return Result::Success();
    };
    for(const auto version:p.sdf_read_versions)if(auto result=add_native_read(version);!result)return result;
    for(std::uint32_t i=0;i<4;++i)if(auto result=add_native_read(p.screens[i]);!result)return result;
    if(auto result=add_native_read(s->history_valid?p.screens[CacheHistory]:p.screens[CacheCurrent]);!result)return result;
    if(auto result=add_native_read(s->history_valid?p.screens[DepthHistory]:p.screens[Depth]);!result)return result;
    if(auto result=add_native_read(s->history_valid?p.screens[NormalHistory]:p.screens[Normal]);!result)return result;
    if(auto result=add_native_read(in.environment_cube.version);!result)return result;
    // The native dispatch chooses one pinned noise texture by frame index.
    // Declare the fixed 16-resource read boundary so a retained plan never
    // preserves last frame's selected index while the SDK reads a new one.
    if(in.noise.resource)
    {if(auto result=add_native_read(noise_guide.version);!result)return result;}
    else for(const auto version:pinned_noise_versions)
        if(auto result=add_native_read(version);!result)return result;
    const auto add_native_write = [&](ResourceHandle resource) -> Result {
        if (!resource) return Result::Success();
        const auto found = std::ranges::find_if(native_accesses, [&](const auto &entry) {
            return entry.resource.value == resource.value && entry.resource.kind == resource.kind;
        });
        if (found != native_accesses.end())
        {
            if (found->incoming != Access::UnorderedWrite || found->outgoing != Access::UnorderedWrite)
                return Fail(ErrorCode::InvalidState, "Brixelizer GI pass has conflicting read/write tags for one physical resource.");
            return Result::Success();
        }
        native_accesses.push_back({resource,Access::UnorderedWrite,Access::UnorderedWrite});
        return Result::Success();
    };
    if(auto result=add_native_write(p.screens[RawDiffuse].resource);!result)return result;
    if(auto result=add_native_write(p.screens[RawSpecular].resource);!result)return result;
    if(!p.retained)
    {p.screens[RawDiffuse]=native_pass->Write(p.screens[RawDiffuse],Access::UnorderedWrite);
     p.screens[RawSpecular]=native_pass->Write(p.screens[RawSpecular],Access::UnorderedWrite);}
    else
    {p.screens[RawDiffuse]=p.retained->screens[RawDiffuse];
     p.screens[RawSpecular]=p.retained->screens[RawSpecular];}
    const bool history=s->history_valid;auto camera=in.camera;const auto restore=in.restore_bindings;
    ComPtr<ID3D12Resource> environment=in.environment_cube.resource,noise_resource=noise_guide.resource;
    slot.resources.push_back(environment);slot.resources.push_back(noise_resource);
    native_pass->SetExecuteOnOwner([s,camera,history,environment,noise_resource,restore,
        native_accesses=std::move(native_accesses)](RenderPassContext &ctx)->Result
    {
        FfxBrixelizerGIDispatchDescription desc{};
        std::memcpy(desc.view,&camera.view,sizeof(camera.view));std::memcpy(desc.projection,&camera.projection,sizeof(camera.projection));
        std::memcpy(desc.prevView,&camera.previous_view,sizeof(camera.previous_view));std::memcpy(desc.prevProjection,&camera.previous_projection,sizeof(camera.previous_projection));
        desc.cameraPosition[0]=camera.position.x;desc.cameraPosition[1]=camera.position.y;desc.cameraPosition[2]=camera.position.z;
        desc.startCascade=s->settings.logical_cascades*2;desc.endCascade=s->settings.logical_cascades*3-1;
        // A quarter-voxel ray offset avoids unstable self-hits on the SDF surface.
        desc.rayPushoff=desc.specularRayPushoff=0.25f;desc.sdfSolveEps=desc.specularSDFSolveEps=0.001f;desc.tMin=0.0f;desc.tMax=1000.0f;
        auto read_screen=[&](Screen index){return Resource(s->screens[index]->resource.Get(),FFX_RESOURCE_STATE_COMPUTE_READ);};
        desc.environmentMap=Resource(environment.Get(),FFX_RESOURCE_STATE_COMPUTE_READ);desc.environmentMap.description.type=FFX_RESOURCE_TYPE_TEXTURE_CUBE;
        desc.prevLitOutput=read_screen(history?CacheHistory:CacheCurrent);desc.depth=read_screen(Depth);desc.normal=read_screen(Normal);
        desc.historyDepth=read_screen(history?DepthHistory:Depth);desc.historyNormal=read_screen(history?NormalHistory:Normal);
        desc.roughness=read_screen(Roughness);desc.motionVectors=read_screen(Motion);desc.noiseTexture=Resource(noise_resource.Get(),FFX_RESOURCE_STATE_COMPUTE_READ);
        desc.normalsUnpackMul=1.0f;desc.normalsUnpackAdd=0.0f;desc.isRoughnessPerceptual=false;desc.roughnessChannel=0;
        desc.roughnessThreshold=1.0f;desc.environmentMapIntensity=1.0f;desc.motionVectorScale={1.0f,1.0f};
        const auto resources=SdfResources(*s,FFX_RESOURCE_STATE_COMPUTE_READ);desc.sdfAtlas=resources.sdfAtlas;desc.bricksAABBs=resources.brickAABBs;
        for(std::uint32_t i=0;i<s->status.physical_cascades;++i)
        {desc.cascadeAABBTrees[i]=resources.cascadeResources[i].aabbTree;desc.cascadeBrickMaps[i]=resources.cascadeResources[i].brickMap;}
        desc.outputDiffuseGI=Resource(s->screens[RawDiffuse]->resource.Get(),FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        desc.outputSpecularGI=Resource(s->screens[RawSpecular]->resource.Get(),FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        Result batch=Result::Success();
        const auto execute_native = [&]() -> Result {
            if(auto result=Sdk("ffxBrixelizerGetRawContext",ffxBrixelizerGetRawContext(s->sdf.get(),&desc.brixelizerContext));!result)return result;
            if(auto result=brixelizer_native::BeginBatch(*s->backend_slots);!result)return result;
            auto *cmd=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());
            const auto error=ffxBrixelizerGIContextDispatch(s->gi.get(),&desc,ffxGetCommandListDX12(cmd));
            batch=brixelizer_native::BatchResult(*s->backend_slots);
            brixelizer_native::EndBatch(*s->backend_slots);
            cmd->SetComputeRootSignature(s->root.Get());
            if(error!=FFX_OK)return Sdk("ffxBrixelizerGIContextDispatch",error);return batch;
        };
        const auto result=ctx.RecordLegacyResourceCommands(native_accesses,execute_native);
        // Restore only after the legacy/enhanced handoff has finished so the
        // static renderer tables are not rebound before the handoff barriers.
        const auto restored=restore(static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList()));
        if(!result||!batch||!restored){s->usable=false;s->status.faulted=true;s->status.native_ready=false;}
        if(!result)return result;if(!batch)return batch;return restored;
    });
    auto resolve=impl_->Pass(graph,6,263,"BrixelizerMaterialResolve");
    if(!resolve)return Fail(ErrorCode::InvalidState,"Brixelizer material resolve pass does not match retained plan.");
    if(!p.retained)
    {resolve->Read(cb,Access::ComputeRead);
     for(const auto &r:in.reads)resolve->Read(r.version,r.access);
     for(const auto &g:guides)if(g.version.resource)resolve->Read(g.version,Access::ComputeRead);
     resolve->Read(p.screens[RawDiffuse],Access::ComputeRead);resolve->Read(p.screens[RawSpecular],Access::ComputeRead);
     for(std::uint32_t i=Diffuse;i<=Specular;++i)p.screens[i]=resolve->Write(p.screens[i],Access::UnorderedWrite);}
    else for(std::uint32_t i=Diffuse;i<=Specular;++i)p.screens[i]=p.retained->screens[i];
    resolve->SetExecuteChecked(run(1));
    auto histories=impl_->Pass(graph,7,265,"BrixelizerRetainHistories");
    if(!histories)return Fail(ErrorCode::InvalidState,"Brixelizer histories pass does not match retained plan.");
    for(auto pair:{std::pair{CacheCurrent,CacheHistory},std::pair{Depth,DepthHistory},std::pair{Normal,NormalHistory}})
    {if(!p.retained){histories->Read(p.screens[pair.first],Access::CopySource);
     p.screens[pair.second]=histories->Write(p.screens[pair.second],Access::CopyDest);}
     else p.screens[pair.second]=p.retained->screens[pair.second];}
    if(!p.retained){graph.AddOrderingDependency(262,265);graph.AddOrderingDependency(264,262);}
    histories->SetExecuteChecked([s](RenderPassContext &ctx)->Result
    {auto *cmd=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());for(auto pair:{std::pair{CacheCurrent,CacheHistory},std::pair{Depth,DepthHistory},std::pair{Normal,NormalHistory}})
        cmd->CopyResource(s->screens[pair.second]->resource.Get(),s->screens[pair.first]->resource.Get());return Result::Success();});
    if(!p.retained)for(std::uint32_t i=0;i<ScreenCount;++i)graph.SetFinalAccess(p.screens[i],Access::ComputeRead);
    if(p.capture){p.capture->screens=p.screens;p.capture->lighting=true;}
    auto output=[&](Screen index){return BrixelizerTexture{p.screens[index],s->screens[index]->resource.Get(),s->Export(index)};};
    out={output(Diffuse),output(Delta),output(Subsurface),output(Specular),output(RawDiffuse),output(RawSpecular),output(CacheCurrent)};
    p.lighting=true;return Result::Success();
#endif
}
Result BrixelizerGiSystem::RegisterComposite(RenderGraphBuilder &graph,const BrixelizerCompositeInputs &in,BrixelizerTexture &out)
{
    out={};auto s=impl_->storage;if(!s)return Result::Success();auto &p=impl_->pending;
    if(p.graph!=&graph||!p.lighting||p.composite||!s->usable||in.frame_slot!=p.slot||!in.make_bindings||in.owners.empty()||
        in.render_active.width!=s->extent.width||in.render_active.height!=s->extent.height||
        !Guide(graph,in.hdr,s->extent,DXGI_FORMAT_R16G16B16A16_FLOAT))
        return Fail(ErrorCode::InvalidArgument,"Brixelizer composite requires this frame's real HDR and completed native lighting registration.");
    if(p.retained&&in.hdr.version!=p.retained->composite_hdr)
        return Fail(ErrorCode::InvalidState,"Brixelizer HDR producer changed after plan selection.");
    if(p.capture)p.capture->composite_hdr=in.hdr.version;
    constexpr std::uint32_t stage=3;
    const auto cb_offset=std::uint64_t(p.slot*kStages+stage)*kConstantStride;
    std::memcpy(s->mapped+cb_offset,&p.constants,sizeof(p.constants));
    const D3D12_CONSTANT_BUFFER_VIEW_DESC cb{s->constants->resource->GetGPUVirtualAddress()+cb_offset,kConstantStride};
    s->device->CreateConstantBufferView(&cb,s->Cpu(p.slot,stage,0));
    for(std::uint32_t i=0;i<15;++i)s->Srv(p.slot,stage,1+i,nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT);
    s->Srv(p.slot,stage,10,s->screens[Delta]->resource.Get(),kScreenFormats[Delta]);
    s->Srv(p.slot,stage,11,p.original_environment.resource,DXGI_FORMAT_R16G16B16A16_FLOAT);
    s->Srv(p.slot,stage,14,s->screens[Specular]->resource.Get(),kScreenFormats[Specular]);
    s->Srv(p.slot,stage,15,in.hdr.resource,DXGI_FORMAT_R16G16B16A16_FLOAT);
    for(std::uint32_t u=0;u<9;++u)s->Uav(p.slot,stage,16+u,nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT);
    s->Uav(p.slot,stage,24,s->screens[Composed]->resource.Get(),kScreenFormats[Composed]);
    auto &slot=s->slots[p.slot];
    if(auto result=in.make_bindings(s->Cpu(p.slot,stage,0),kBrixelizerDescriptorCount,slot.binders[stage]);!result)return result;
    slot.owners.insert(slot.owners.end(),in.owners.begin(),in.owners.end());slot.resources.emplace_back(in.hdr.resource);
    auto pass=impl_->Pass(graph,8,267,"BrixelizerComposeHDR");
    if(!pass)return Fail(ErrorCode::InvalidState,"Brixelizer composite pass does not match retained plan.");
    if(!p.retained)
    {pass->Read(p.constant_version,Access::ComputeRead);
     pass->Read(in.hdr.version,Access::ComputeRead);pass->Read(p.screens[Delta],Access::ComputeRead);
     pass->Read(p.screens[Specular],Access::ComputeRead);pass->Read(p.original_environment.version,Access::ComputeRead);
     p.screens[Composed]=pass->Write(p.screens[Composed],Access::UnorderedWrite);}
    else p.screens[Composed]=p.retained->screens[Composed];
    pass->SetExecuteChecked([s,slot_index=p.slot](RenderPassContext &ctx)->Result
    {auto *cmd=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());cmd->SetComputeRootSignature(s->root.Get());
        cmd->SetPipelineState(s->pipelines[3].Get());if(auto result=s->slots[slot_index].binders[3](cmd);!result)return result;
        cmd->Dispatch((s->extent.width+7)/8,(s->extent.height+7)/8,1);return Result::Success();});
    if(!p.retained)graph.SetFinalAccess(p.screens[Composed],Access::ComputeRead);
    if(p.retained&&p.imports.size()!=p.retained->imports.size())
        return Fail(ErrorCode::InvalidState,"Brixelizer retained native import sequence changed.");
    if(p.capture){p.capture->screens[Composed]=p.screens[Composed];p.capture->composite=true;p.capture->valid=true;}
    p.composite=true;
    out={p.screens[Composed],s->screens[Composed]->resource.Get(),s->Export(Composed)};return Result::Success();
}
Result BrixelizerGiSystem::CompleteSubmission(const RenderGraphBuilder &graph,const CompletionSet &completion,bool success)
{
    auto &p=impl_->pending;auto s=impl_->storage;
    if(!p.graph){if(p.prepared&&!success)AbandonPreparedFrame();return Result::Success();}
    if(p.graph!=&graph)return Fail(ErrorCode::InvalidArgument,"Brixelizer completion refers to another graph.");
    auto &slot=s->slots[p.slot];slot.completion.Merge(completion);s->completion.Merge(completion);
    for(const auto &import:p.imports)
    {const auto actual=graph.ResourceCompletion(import.handle);slot.completion.Merge(actual);s->completion.Merge(actual);
        import.allocation->graph_completion.Merge(completion);import.allocation->graph_completion.Merge(actual);
        if(success){import.allocation->graph_access=graph.FinalAccess(import.handle);import.allocation->graph_queue=graph.FinalQueue(import.handle);}}
#if HS_WITH_BRIXELIZER
    for(auto &[key,buffer]:s->registered_buffers)
    {buffer.completion.Merge(slot.completion);if(success&&buffer.retired)buffer.pending_cascades&=~(1u<<p.updated_cascade);}
    if(success)
    {for(auto &mask:s->static_invalidation_masks)mask&=~(1u<<p.updated_cascade);std::erase(s->static_invalidation_masks,0u);}
#endif
    if(success&&p.lighting&&p.composite&&s->usable)
    {s->history_valid=true;s->last_frame=p.frame;s->last_scene=p.scene_revision;s->last_light=p.light_revision;++s->successful_updates;
        s->status.scene_revision=p.scene_revision;s->status.light_revision=p.light_revision;
        if(p.noise_upload)s->noise_uploaded=true;
        s->status.reason="Pinned Brixelizer native GI recorded and submitted; GPU validation is unverified.";}
    else
    {s->history_valid=false;s->usable=false;s->status.faulted=true;s->status.native_ready=false;
        s->status.reason="Native Brixelizer graph failed or was incomplete; drain every submitted queue and recreate before reuse.";}
    p={};return Result::Success();
}
void BrixelizerGiSystem::AbandonPreparedFrame() noexcept
{
    auto &p=impl_->pending;
    if(!p.prepared||p.graph)return;
    if(auto s=impl_->storage)
    {
        s->usable=false;s->history_valid=false;s->status.faulted=true;s->status.native_ready=false;
        s->status.reason="Prepared native SDF state was abandoned before graph submission; drained recreation is required.";
    }
    p={};
}
Result BrixelizerGiSystem::ResetHistoryAfterCompletion(const CompletionSet &completed,ViewExtent extent,bool rebuild_sdf)
{
    auto s=impl_->storage;if(!s)return Result::Success();
    if(impl_->pending.graph||!s->completion.IsSatisfiedBy(completed)||!Extent(extent))
        return Fail(ErrorCode::InvalidState,"Native GI history recreation requires no pending graph and actual completion of every queue use.");
#if !HS_WITH_BRIXELIZER
    (void)rebuild_sdf;return Fail(ErrorCode::InvalidState,"Brixelizer native package is unavailable.");
#else
    if(!s->usable&&!rebuild_sdf)return Fail(ErrorCode::InvalidState,"Faulted native Brixelizer requires complete SDF/GI replacement.");
    if(rebuild_sdf)
    {
        BrixelizerCandidate next;
        // Drained replacement no longer has staging overlap with submitted old
        // resources. Prepare still includes the resident overlap in its budget.
        if(auto result=Prepare(s->device.Get(),s->allocator.Get(),s->root.Get(),extent,std::uint32_t(s->slots.size()),s->settings,s->shader_reader,s->texture_catalog,next);!result)return result;
        return Commit(std::move(next),completed);
    }
    if(s->gi_created)
    {if(auto result=Sdk("ffxBrixelizerGIContextDestroy",ffxBrixelizerGIContextDestroy(s->gi.get()));!result)return result;s->gi_created=false;}
    s->history_valid=false;s->extent=extent;
    for(std::uint32_t i=0;i<ScreenCount;++i)
    {Allocation next;if(auto result=Texture(s->device.Get(),s->allocator.Get(),extent,kScreenFormats[i],next,1,i==CacheInjection);!result){s->usable=false;s->status.faulted=true;s->status.native_ready=false;return result;}
        s->screens[i]=std::move(next);D3D12_SHADER_RESOURCE_VIEW_DESC desc{};desc.Format=kScreenFormats[i];desc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;desc.Texture2D.MipLevels=1;
        s->device->CreateShaderResourceView(s->screens[i]->resource.Get(),&desc,s->Export(i));}
    s->device->CreateRenderTargetView(s->screens[CacheInjection]->resource.Get(),nullptr,s->cache_rtv->GetCPUDescriptorHandleForHeapStart());
    if(auto result=CreateGi(*s);!result){s->usable=false;s->status.faulted=true;s->status.native_ready=false;return result;}
    if(s->GpuBytes()>s->settings.memory_budget_bytes){s->usable=false;s->status.faulted=true;s->status.native_ready=false;
        return Fail(ErrorCode::InvalidState,"Recreated native GI extent exceeds the measured memory budget.");}
    for(auto &slot:s->slots){slot.owners.clear();slot.resources.clear();slot.binders={};}
    s->retired_scratch.clear();s->status.native_ready=true;s->status.reason="Drained fixed-extent native GI history recreated; SDF retained.";return Result::Success();
#endif
}
Result BrixelizerGiSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if(impl_->pending.graph)return Fail(ErrorCode::InvalidState,"Complete the pending Brixelizer graph before reset.");
    if(auto s=impl_->storage;s&&!s->completion.IsSatisfiedBy(completed))return Fail(ErrorCode::InvalidState,"Brixelizer reset requires every submitted Direct/Compute/Copy use completed.");
    if(impl_->pending.prepared)AbandonPreparedFrame();
    impl_->pending={};impl_->storage.reset();return Result::Success();
}
} // namespace hs
