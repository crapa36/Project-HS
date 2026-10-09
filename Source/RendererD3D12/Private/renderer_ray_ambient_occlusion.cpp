#include "renderer_ray_ambient_occlusion.hpp"
#include "renderer_ray_transport.hpp"
#include "d3d12_resources.hpp"
#include <D3D12MemAlloc.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
Result Fail(ErrorCode c,std::string m){return Result::Failure(c,"hs_renderer_ray_ambient_occlusion",std::move(m));}
Result Native(std::string_view op,HRESULT hr)
{return Fail(ErrorCode::TaskFailed,std::string(op)+" failed (HRESULT "+std::to_string(std::int32_t(hr))+").");}
bool Extent(ViewExtent e){return e.width&&e.height&&e.width<=D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION&&e.height<=D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;}
ViewExtent AoExtent(ViewExtent e,const render_data::QualityProfile &p)
{return {std::max(1u,UINT(std::ceil(e.width*p.ao_axis_ratio))),std::max(1u,UINT(std::ceil(e.height*p.ao_axis_ratio))) };}
DirectX::XMFLOAT4 Size(ViewExtent e){return {float(e.width),float(e.height),1.0f/e.width,1.0f/e.height};}
Result Texture(D3D12MA::Allocator *a,ViewExtent e,DXGI_FORMAT format,AllocationResource &r)
{
    D3D12MA::ALLOCATION_DESC ad{};ad.HeapType=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};
    d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=e.width;d.Height=e.height;
    d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Format=format;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto hr=a->CreateResource(&ad,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,&r.allocation,IID_PPV_ARGS(r.resource.ReleaseAndGetAddressOf()));
    if(FAILED(hr))return Native("Create RTAO texture",hr);r.graph_access=Access::Common;return Result::Success();
}
bool Guide(const RenderGraphBuilder &g,const AmbientOcclusionGuide &r,ViewExtent e,DXGI_FORMAT format)
{
    if(!r.resource||!r.version.resource||r.version.resource.kind!=ResourceKind::Texture||g.Resolve(r.version.resource)!=r.resource)return false;
    auto d=r.resource->GetDesc();return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.DepthOrArraySize==1&&d.SampleDesc.Count==1&&
        d.Width>=e.width&&d.Height>=e.height&&d.Format==format;
}
bool BufferVersion(const RenderGraphBuilder &g,ResourceVersion v,ID3D12Resource *r)
{return r&&v.resource&&v.resource.kind==ResourceKind::Buffer&&g.Resolve(v.resource)==r&&r->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER;}
} // namespace

struct RayAmbientOcclusionStorage
{
    struct ProducerOwner{std::vector<ComPtr<D3D12MA::Allocation>> allocations;std::vector<ComPtr<ID3D12Resource>> resources;};
    ComPtr<ID3D12Device> device;ComPtr<D3D12MA::Allocator> allocator;ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>,3> pipelines;
    ComPtr<ID3D12DescriptorHeap> descriptors,export_descriptor;
    AllocationResource constants,raw,resolved;
    AllocationResource native_motion,native_normal,native_view_z,native_hit_distance;
    std::unique_ptr<NrdSystem> nrd;
    std::shared_ptr<ProducerOwner> native_owner;
    UINT descriptor_count{kRayDescriptorCount};
    RayDenoiserRequest denoiser_request;
    std::array<AllocationResource,2> history,metadata;
    std::byte *mapped{};ViewExtent capacity,ao_capacity,previous_active;
    GraphicsQuality quality{};render_data::QualityProfile profile;std::uint32_t stride{},read_index{};
    bool enabled{},history_valid{},usable{true};CompletionSet completion;
    struct Slot{CompletionSet completion;RayShaderBinder binder;std::vector<ComPtr<ID3D12Resource>> reads;};
    std::vector<Slot> slots;
    ~RayAmbientOcclusionStorage(){if(mapped&&constants.resource)constants.resource->Unmap(0,nullptr);}
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT slot,UINT i)const
    {auto h=descriptors->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(slot*descriptor_count+i)*stride;return h;}
    void Srv(UINT s,UINT i,ID3D12Resource *r,DXGI_FORMAT f)
    {D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=f;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
     d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;device->CreateShaderResourceView(r,&d,Cpu(s,i));}
    void Uav(UINT s,UINT i,ID3D12Resource *r,DXGI_FORMAT f)
    {D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=f;d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(r,nullptr,&d,Cpu(s,i));}
    void Structured(UINT s,UINT i,ID3D12Resource *r,UINT count,UINT size)
    {D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
     d.Buffer.NumElements=count;d.Buffer.StructureByteStride=size;device->CreateShaderResourceView(r,&d,Cpu(s,i));}
};
struct RayAmbientOcclusionSystem::Impl
{
    std::shared_ptr<RayAmbientOcclusionStorage> storage;
    struct Import{AllocationResource *resource{};ResourceHandle handle;};
    struct Pending{const RenderGraphBuilder *graph{};std::vector<Import> imports;ViewExtent active;UINT slot{},write_index{};bool native_registered{};}pending;
};
RayAmbientOcclusionSystem::RayAmbientOcclusionSystem():impl_(std::make_unique<Impl>()){}
RayAmbientOcclusionSystem::~RayAmbientOcclusionSystem()=default;
RayDenoiserStatus RayAmbientOcclusionSystem::GetDenoiserStatus()const
{
    auto s=impl_->storage;auto status=DescribeRayDenoiser(s?s->denoiser_request:RayDenoiserRequest{});
    if(s&&s->nrd){status.active_method=NrdRadianceMethod::Reblur;status.availability=s->nrd->Availability();status.reason=s->nrd->FailureReason();}
    return status;
}
std::uint64_t RayAmbientOcclusionSystem::AllocatedBytes()const noexcept
{
    std::uint64_t bytes{};if(auto s=impl_->storage)
    {
        for(auto *r:{&s->constants,&s->raw,&s->resolved,&s->history[0],&s->history[1],&s->metadata[0],&s->metadata[1],
            &s->native_motion,&s->native_normal,&s->native_view_z,&s->native_hit_distance})
            if(r->allocation)bytes+=r->allocation->GetSize();
        if(s->nrd)bytes+=s->nrd->AllocatedBytes();
    }return bytes;
}
Result RayAmbientOcclusionSystem::Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,ID3D12RootSignature *root,
    ViewExtent capacity,GraphicsQuality quality,const render_data::QualityProfile &profile,bool enabled,UINT frames,const RayShaderReader &reader,RayAmbientOcclusionCandidate &candidate,RayDenoiserRequest denoiser)const
{
    if(!device||!allocator||!Extent(capacity)||!frames||frames>UINT_MAX/kRayTransportSuffixCount||
        (quality!=GraphicsQuality::Low&&quality!=GraphicsQuality::Medium&&quality!=GraphicsQuality::High&&quality!=GraphicsQuality::Ultra)||
        (enabled&&(!root||!reader)))return Fail(ErrorCode::InvalidArgument,"Invalid RTAO preparation inputs.");
    auto s=std::make_shared<RayAmbientOcclusionStorage>();s->device=device;s->allocator=allocator;s->capacity=capacity;
    s->ao_capacity=AoExtent(capacity,profile);s->quality=quality;s->profile=profile;s->enabled=enabled;s->slots.resize(frames);
    s->denoiser_request=denoiser;
    if(!enabled){candidate.storage_=std::move(s);return Result::Success();}
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
    if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&options,sizeof(options)))||
       options.RaytracingTier<D3D12_RAYTRACING_TIER_1_1||
       FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&model,sizeof(model)))||model.HighestShaderModel<D3D_SHADER_MODEL_6_5)
        return Fail(ErrorCode::TaskFailed,"Inline RTAO requires native DXR1.1 and Shader Model6.5.");
    s->root=root;
    for(auto f:{DXGI_FORMAT_R16_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32G32_UINT})
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{f};auto hr=device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support));
        if(FAILED(hr))return Native("Check RTAO UAV format",hr);
        if(!(support.Support1&D3D12_FORMAT_SUPPORT1_TEXTURE2D)||!(support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_LOAD)||
           !(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))return Fail(ErrorCode::TaskFailed,"RTAO texture format lacks required load/store support.");
    }
    if(auto r=Texture(allocator,s->ao_capacity,DXGI_FORMAT_R32G32B32A32_FLOAT,s->raw);!r)return r;
    if(auto r=Texture(allocator,capacity,DXGI_FORMAT_R16_FLOAT,s->resolved);!r)return r;
    if(ResolveRayDenoiser(denoiser)!=NrdRadianceMethod::Off)
    {
        s->nrd=std::make_unique<NrdSystem>();NrdCandidate prepared;
        if(auto r=s->nrd->Prepare(device,allocator,s->ao_capacity,frames,reader,prepared,{NrdSignal::AmbientOcclusion,NrdRadianceMethod::Reblur});!r)return r;
        if(auto r=s->nrd->Commit(std::move(prepared),{});!r)return r;
        s->descriptor_count=kRayTransportSuffixCount;s->native_owner=std::make_shared<RayAmbientOcclusionStorage::ProducerOwner>();
        const std::pair<AllocationResource *,DXGI_FORMAT> outputs[]{{&s->native_motion,DXGI_FORMAT_R16G16_FLOAT},
            {&s->native_normal,DXGI_FORMAT_R10G10B10A2_UNORM},{&s->native_view_z,DXGI_FORMAT_R32_FLOAT},{&s->native_hit_distance,DXGI_FORMAT_R16_FLOAT}};
        for(const auto &[resource,format]:outputs)
        {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
            if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)))||
               !(support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_LOAD)||!(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
                return Fail(ErrorCode::TaskFailed,"NRD AO producer formats lack shader-load/UAV-store support.");
            if(auto r=Texture(allocator,s->ao_capacity,format,*resource);!r)return r;
            s->native_owner->allocations.push_back(resource->allocation);s->native_owner->resources.push_back(resource->resource);
        }
    }
    else for(UINT i=0;i<2;++i){if(auto r=Texture(allocator,s->ao_capacity,DXGI_FORMAT_R32G32B32A32_FLOAT,s->history[i]);!r)return r;
        if(auto r=Texture(allocator,s->ao_capacity,DXGI_FORMAT_R32G32_UINT,s->metadata[i]);!r)return r;}
    constexpr std::string_view products[]{"render/shader/rtao_trace_cs.dxil","render/shader/rtao_temporal_cs.dxil","render/shader/rtao_resolve_cs.dxil"};
    for(UINT i=0;i<3;++i){if(s->nrd&&i==1)continue;std::vector<std::byte> code;if(auto r=reader(products[i],code);!r)return r;
        if(code.empty())return Fail(ErrorCode::TaskFailed,"RTAO shader product is empty: "+std::string(products[i]));
        D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=root;p.CS={code.data(),code.size()};
        auto hr=device->CreateComputePipelineState(&p,IID_PPV_ARGS(&s->pipelines[i]));if(FAILED(hr))return Native("Create RTAO PSO",hr);}
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=frames*s->descriptor_count;
    auto hr=device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s->descriptors));if(FAILED(hr))return Native("Create RTAO CPU descriptors",hr);
    s->stride=device->GetDescriptorHandleIncrementSize(hd.Type);hd.NumDescriptors=1;
    hr=device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s->export_descriptor));if(FAILED(hr))return Native("Create RTAO output descriptor",hr);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R16_FLOAT;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
    device->CreateShaderResourceView(s->resolved.resource.Get(),&srv,s->export_descriptor->GetCPUDescriptorHandleForHeapStart());
    D3D12MA::ALLOCATION_DESC ad{};ad.HeapType=D3D12_HEAP_TYPE_UPLOAD;D3D12_RESOURCE_DESC d{};
    d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=UINT64(frames)*512;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;
    d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr=allocator->CreateResource(&ad,&d,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,&s->constants.allocation,IID_PPV_ARGS(s->constants.resource.ReleaseAndGetAddressOf()));
    if(FAILED(hr))return Native("Create RTAO constant upload",hr);s->constants.graph_access=Access::ComputeRead;
    void *mapped{};D3D12_RANGE empty{0,0};hr=s->constants.resource->Map(0,&empty,&mapped);
    if(FAILED(hr))return Native("Map RTAO constants",hr);s->mapped=static_cast<std::byte *>(mapped);
    candidate.storage_=std::move(s);return Result::Success();
}
Result RayAmbientOcclusionSystem::Commit(RayAmbientOcclusionCandidate &&candidate,const CompletionSet &completed)
{
    if(!candidate.storage_||impl_->pending.graph)return Fail(ErrorCode::InvalidState,"RTAO candidate missing or submission pending.");
    if(impl_->storage&&(!impl_->storage->usable||!impl_->storage->completion.IsSatisfiedBy(completed)))
        return Fail(ErrorCode::InvalidState,"RTAO replacement requires completed, usable old storage.");
    if(auto old=impl_->storage)if(old->nrd)if(auto r=old->nrd->ResetAfterCompletion(completed);!r)return r;
    impl_->storage=std::move(candidate.storage_);return Result::Success();
}
Result RayAmbientOcclusionSystem::Register(RenderGraphBuilder &graph,const RayAmbientOcclusionInputs &in,
    RayAmbientOcclusionConstants constants,RayAmbientOcclusionOutputs &out)
{
    out={};auto s=impl_->storage;if(!s)return Fail(ErrorCode::InvalidState,"RTAO service has not been committed.");
    if(!s->enabled)return Result::Success();if(!s->usable||impl_->pending.graph)return Fail(ErrorCode::InvalidState,"RTAO storage faulted or pending.");
    auto active=in.render_active;
    if(!Extent(active)||active.width>s->capacity.width||active.height>s->capacity.height||in.frame_slot>=s->slots.size()||
       !s->slots[in.frame_slot].completion.IsSatisfiedBy(in.completed)||!in.make_bindings||
       !(Guide(graph,in.depth,active,DXGI_FORMAT_R32_TYPELESS)||Guide(graph,in.depth,active,DXGI_FORMAT_R32_FLOAT)||Guide(graph,in.depth,active,DXGI_FORMAT_D32_FLOAT))||
       !Guide(graph,in.normal,active,DXGI_FORMAT_R16G16_SNORM)||!Guide(graph,in.metadata,active,DXGI_FORMAT_R32G32_UINT)||
       !Guide(graph,in.motion,active,DXGI_FORMAT_R16G16_FLOAT)||!Guide(graph,in.surface_history,active,DXGI_FORMAT_R32G32_FLOAT)||
       !BufferVersion(graph,in.scene.tlas,in.scene.tlas_resource)||!BufferVersion(graph,in.scene.metadata,in.scene.metadata_resource)||
       !in.scene.instance_count||!in.scene.tlas_address||in.scene.tlas_address!=in.scene.tlas_resource->GetGPUVirtualAddress()||
       !BufferVersion(graph,in.geometry.records,in.geometry.record_resource)||!BufferVersion(graph,in.geometry.instances,in.geometry.instance_resource)||
       !in.geometry.geometry_count||in.geometry.geometry_count!=in.geometry.geometries.size()||
       in.scene.metadata_resource->GetDesc().Width<std::uint64_t(in.scene.instance_count)*32||
       in.geometry.record_resource->GetDesc().Width<std::uint64_t(in.geometry.geometry_count)*sizeof(RayHitGeometry)||
       in.geometry.instance_resource->GetDesc().Width<std::uint64_t(in.geometry.geometry_count)*48)
        return Fail(ErrorCode::InvalidArgument,"RTAO guides, shared scene, exact ray geometry or frame slot invalid.");
    for(auto &r:in.reads)if(!r.resource||!r.version.resource||graph.Resolve(r.version.resource)!=r.resource||
        (r.access!=Access::ComputeRead&&r.access!=Access::ShaderRead&&r.access!=Access::AllShaderRead))
            return Fail(ErrorCode::InvalidArgument,"RTAO material/vertex read version does not match its native resource.");
    auto &ao=constants.ao;
    if(!std::isfinite(ao.gtao_parameters.x)||ao.gtao_parameters.x<=0||!std::isfinite(ao.gtao_parameters.y)||ao.gtao_parameters.y<0||ao.gtao_parameters.y>1||
       !std::isfinite(constants.ray_parameters.x)||constants.ray_parameters.x<=0||constants.ray_parameters.x>=ao.gtao_parameters.x||
       !std::isfinite(constants.ray_parameters.y)||constants.ray_parameters.y<0||
       !std::isfinite(constants.world_origin.x)||!std::isfinite(constants.world_origin.y)||!std::isfinite(constants.world_origin.z))
        return Fail(ErrorCode::InvalidArgument,"RTAO radius, strength, origin or ray cone invalid.");
    auto aoActive=AoExtent(active,s->profile);const UINT write=1-s->read_index;
    const bool valid=s->history_valid&&!in.reset_history;
    ao.render_size=Size(active);ao.ao_size=Size(aoActive);ao.history_size=Size(valid?s->previous_active:aoActive);
    if(constants.ray_parameters.y==0)
        constants.ray_parameters.y=2.0f*std::max(ao.render_size.z/std::max(std::abs(ao.projection._11),1e-6f),
            ao.render_size.w/std::max(std::abs(ao.projection._22),1e-6f));
    ao.sample_counts={s->profile.rtao_samples,0,0,s->profile.ao_spatial_radius};
    ao.frame_info={valid?1u:0u,UINT(in.frame_index),1,0};
    constants.nrd_parameters.w=s->nrd?1.0f:0.0f;
    std::memcpy(s->mapped+SIZE_T(in.frame_slot)*512,&constants,sizeof(constants));
    auto slot=in.frame_slot;
    D3D12_CONSTANT_BUFFER_VIEW_DESC cb{s->constants.resource->GetGPUVirtualAddress()+UINT64(slot)*512,512};s->device->CreateConstantBufferView(&cb,s->Cpu(slot,0));
    D3D12_SHADER_RESOURCE_VIEW_DESC as{};as.ViewDimension=D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;as.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    as.RaytracingAccelerationStructure.Location=in.scene.tlas_address;s->device->CreateShaderResourceView(nullptr,&as,s->Cpu(slot,1));
    s->Structured(slot,2,in.scene.metadata_resource,in.scene.instance_count,32);s->Structured(slot,3,in.geometry.record_resource,in.geometry.geometry_count,sizeof(RayHitGeometry));
    s->Structured(slot,4,in.geometry.record_resource,in.geometry.geometry_count,sizeof(RayHitGeometry));
    D3D12_SHADER_RESOURCE_VIEW_DESC raw{};raw.Format=DXGI_FORMAT_R32_TYPELESS;raw.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
    raw.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;raw.Buffer.NumElements=in.geometry.position_resource?UINT(in.geometry.position_resource->GetDesc().Width/4):1;
    raw.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;s->device->CreateShaderResourceView(in.geometry.position_resource,&raw,s->Cpu(slot,5));
    s->Srv(slot,6,in.depth.resource,DXGI_FORMAT_R32_FLOAT);s->Srv(slot,7,in.normal.resource,DXGI_FORMAT_R16G16_SNORM);
    s->Srv(slot,8,in.metadata.resource,DXGI_FORMAT_R32G32_UINT);s->Srv(slot,9,in.motion.resource,DXGI_FORMAT_R16G16_FLOAT);
    s->Srv(slot,10,in.surface_history.resource,DXGI_FORMAT_R32G32_FLOAT);s->Srv(slot,11,s->raw.resource.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT);
    s->Srv(slot,12,valid?s->history[s->read_index].resource.Get():nullptr,DXGI_FORMAT_R32G32B32A32_FLOAT);
    s->Srv(slot,13,valid?s->metadata[s->read_index].resource.Get():nullptr,DXGI_FORMAT_R32G32_UINT);
    s->Srv(slot,14,s->history[write].resource.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT);s->Structured(slot,15,in.geometry.instance_resource,in.geometry.geometry_count,48);
    D3D12_UNORDERED_ACCESS_VIEW_DESC unused{};unused.Format=DXGI_FORMAT_R32_TYPELESS;unused.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
    unused.Buffer.NumElements=1;unused.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;s->device->CreateUnorderedAccessView(nullptr,nullptr,&unused,s->Cpu(slot,16));
    s->Uav(slot,17,s->raw.resource.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT);s->Uav(slot,18,s->history[write].resource.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT);
    s->Uav(slot,19,s->metadata[write].resource.Get(),DXGI_FORMAT_R32G32_UINT);s->Uav(slot,20,s->resolved.resource.Get(),DXGI_FORMAT_R16_FLOAT);
    if(s->nrd)
    {
        // The NRD guide suffix occupies the complete current transport ABI:
        // space22 CBV/SRV26/UAV11, then space26 SRV2/UAV5. The extra metric
        // SRV/UAV slots precede the native AO guides, even though AO omits them.
        constexpr UINT transport=kRayDescriptorCount;
        s->device->CreateConstantBufferView(nullptr,s->Cpu(slot,transport));
        for(UINT i=transport+1;i<transport+27;++i)s->Srv(slot,i,nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT);
        for(UINT i=transport+27;i<transport+38;++i)s->Uav(slot,i,nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT);
        s->Srv(slot,transport+38,s->nrd->PreparedOutput(NrdSignal::AmbientOcclusion),DXGI_FORMAT_R16_FLOAT);
        s->Srv(slot,transport+39,nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT);
        s->Uav(slot,transport+40,s->native_motion.resource.Get(),DXGI_FORMAT_R16G16_FLOAT);
        s->Uav(slot,transport+41,s->native_normal.resource.Get(),DXGI_FORMAT_R10G10B10A2_UNORM);
        s->Uav(slot,transport+42,s->native_view_z.resource.Get(),DXGI_FORMAT_R32_FLOAT);
        s->Uav(slot,transport+43,s->native_hit_distance.resource.Get(),DXGI_FORMAT_R16_FLOAT);
        s->Uav(slot,transport+44,nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT);
    }
    RayShaderBinder binder;if(auto r=in.make_bindings(s->Cpu(slot,0),s->descriptor_count,binder);!r)return r;
    if(!binder)return Fail(ErrorCode::InvalidArgument,"RTAO frame table builder did not return a compute binder.");
    auto &pending=impl_->pending;pending={};pending.graph=&graph;pending.slot=slot;pending.write_index=write;pending.active=aoActive;
    auto import=[&](AllocationResource &r,std::string_view name){auto h=graph.ImportTexture({r.resource.Get(),r.graph_completion,r.graph_queue},r.graph_access,name);
        pending.imports.push_back({&r,h});return graph.Version(h);};
    auto rawVersion=import(s->raw,"RTAO.Raw"),resolved=import(s->resolved,"RTAO.Visibility");
    ResourceVersion previous,prevMetadata,history,metadata,nativeMotion,nativeNormal,nativeViewZ,nativeHitDistance,nativeClean;
    if(s->nrd)
    {
        nativeMotion=import(s->native_motion,"RTAO.NRD.Motion");nativeNormal=import(s->native_normal,"RTAO.NRD.NormalRoughness");
        nativeViewZ=import(s->native_view_z,"RTAO.NRD.LinearViewZ");nativeHitDistance=import(s->native_hit_distance,"RTAO.NRD.NormalizedHitDistance");
    }
    else
    {
        previous=import(s->history[s->read_index],"RTAO.Previous");prevMetadata=import(s->metadata[s->read_index],"RTAO.PreviousMetadata");
        history=import(s->history[write],"RTAO.Current");metadata=import(s->metadata[write],"RTAO.CurrentMetadata");
    }
    auto ch=graph.ImportBuffer({s->constants.resource.Get(),s->constants.graph_completion,s->constants.graph_queue},s->constants.graph_access,"RTAO.Constants");
    pending.imports.push_back({&s->constants,ch});auto cv=graph.Version(ch);
    auto execute=[s,binder](UINT pipeline,ViewExtent extent){return [s,binder,pipeline,extent](RenderPassContext &c)->Result{
        auto *cmd=static_cast<ID3D12GraphicsCommandList *>(c.NativeCommandList());cmd->SetComputeRootSignature(s->root.Get());cmd->SetPipelineState(s->pipelines[pipeline].Get());
        if(auto r=binder(cmd);!r)return r;std::uint32_t pass[]{0,0};cmd->SetComputeRoot32BitConstants(6,2,pass,0);
        cmd->Dispatch((extent.width+7)/8,(extent.height+7)/8,1);return Result::Success();};};
    auto guides=[&](PassBuilder &p){p.Read(in.depth.version,Access::ComputeRead);p.Read(in.normal.version,Access::ComputeRead);p.Read(in.metadata.version,Access::ComputeRead);p.Read(cv,Access::ComputeRead);};
    auto trace=graph.AddPass(144,"RTAOTrace",QueueHint::Direct);guides(trace);trace.Read(in.scene.tlas,Access::ASRead);trace.Read(in.scene.metadata,Access::ComputeRead);
    trace.Read(in.geometry.records,Access::ComputeRead);trace.Read(in.geometry.instances,Access::ComputeRead);
    for(auto &r:in.reads)trace.Read(r.version,r.access);rawVersion=trace.Write(rawVersion,Access::UnorderedWrite);trace.SetExecuteChecked(execute(0,aoActive));
    if(s->nrd)
    {
        trace.Read(in.motion.version,Access::ComputeRead);
        nativeMotion=trace.Write(nativeMotion,Access::UnorderedWrite);nativeNormal=trace.Write(nativeNormal,Access::UnorderedWrite);
        nativeViewZ=trace.Write(nativeViewZ,Access::UnorderedWrite);nativeHitDistance=trace.Write(nativeHitDistance,Access::UnorderedWrite);
        NrdNativeInputs native;native.motion={nativeMotion,s->native_motion.resource.Get()};native.normal_roughness={nativeNormal,s->native_normal.resource.Get()};
        native.view_z={nativeViewZ,s->native_view_z.resource.Get()};native.diffuse_normalized_hit_distance={nativeHitDistance,s->native_hit_distance.resource.Get()};
        native.native_geometry_packed=true;native.normalized_hit_distance_packed=true;native.camera=in.camera;native.identity=in.identity;
        native.camera.jitter_pixels[0]*=float(aoActive.width)/float(active.width);native.camera.jitter_pixels[1]*=float(aoActive.height)/float(active.height);
        native.camera.reset_history|=in.reset_history||!s->history_valid;native.active=aoActive;native.frame_slot=slot;
        native.first_pass_id=7000;native.completed=in.completed;native.owners.push_back(s->native_owner);
        NrdSettings settings;settings.ambient_occlusion=true;settings.hit_distance_A=constants.nrd_parameters.x;
        settings.hit_distance_B=constants.nrd_parameters.y;settings.hit_distance_C=constants.nrd_parameters.z;
        NrdNativeOutputs nativeOutputs;
        if(auto r=s->nrd->Register(graph,native,settings,nativeOutputs);!r)return r;
        pending.native_registered=true;
        nativeClean=nativeOutputs.ambient_occlusion.version;
    }
    else
    {
    auto temporal=graph.AddPass(145,"RTAOTemporal",QueueHint::Direct);guides(temporal);temporal.Read(rawVersion,Access::ComputeRead);
    temporal.Read(in.motion.version,Access::ComputeRead);temporal.Read(in.surface_history.version,Access::ComputeRead);
    if(valid){temporal.Read(previous,Access::ComputeRead);temporal.Read(prevMetadata,Access::ComputeRead);}
    history=temporal.Write(history,Access::UnorderedWrite);metadata=temporal.Write(metadata,Access::UnorderedWrite);temporal.SetExecuteChecked(execute(1,aoActive));
    }
    auto filter=graph.AddPass(146,"RTAOResolve",QueueHint::Direct);guides(filter);
    if(s->nrd){filter.Read(nativeClean,Access::ComputeRead);filter.Read(rawVersion,Access::ComputeRead);}
    else filter.Read(history,Access::ComputeRead);
    resolved=filter.Write(resolved,Access::UnorderedWrite);filter.SetExecuteChecked(execute(2,active));
    for(auto version:{history,metadata,nativeMotion,nativeNormal,nativeViewZ,nativeHitDistance,nativeClean})
        if(version.resource)graph.SetFinalAccess(version,Access::ComputeRead);
    auto &lease=s->slots[slot];lease.binder=std::move(binder);lease.reads.clear();
    for(auto &r:in.reads)lease.reads.emplace_back(r.resource);
    for(auto *r:{in.scene.tlas_resource,in.scene.metadata_resource,in.geometry.record_resource,in.geometry.instance_resource,
        in.depth.resource,in.normal.resource,in.metadata.resource,in.motion.resource,in.surface_history.resource})lease.reads.emplace_back(r);
    out.visibility=resolved;out.visibility_resource=s->resolved.resource.Get();out.visibility_srv=s->export_descriptor->GetCPUDescriptorHandleForHeapStart();
    out.descriptor_suffix=s->Cpu(slot,0);out.descriptor_count=s->descriptor_count;return Result::Success();
}
Result RayAmbientOcclusionSystem::CompleteSubmission(const RenderGraphBuilder &graph,const CompletionSet &completion,bool success)
{
    auto &p=impl_->pending;if(!p.graph)return Result::Success();if(p.graph!=&graph)return Fail(ErrorCode::InvalidArgument,"Wrong RTAO completion graph.");
    auto s=impl_->storage;s->completion.Merge(completion);s->slots[p.slot].completion.Merge(completion);
    if(p.native_registered)if(auto r=s->nrd->CompleteSubmission(graph,completion,success);!r){s->usable=false;return r;}
    for(auto &i:p.imports){auto lease=graph.ResourceCompletion(i.handle);s->completion.Merge(lease);s->slots[p.slot].completion.Merge(lease);
        i.resource->graph_completion.Merge(completion);i.resource->graph_completion.Merge(lease);
        if(success){i.resource->graph_access=graph.FinalAccess(i.handle);i.resource->graph_queue=graph.FinalQueue(i.handle);}}
    if(success){s->read_index=p.write_index;s->previous_active=p.active;s->history_valid=true;}
    else{s->history_valid=false;s->usable=false;}p={};return Result::Success();
}
void RayAmbientOcclusionSystem::ResetHistory()noexcept{if(impl_->storage)impl_->storage->history_valid=false;}
Result RayAmbientOcclusionSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if(impl_->pending.graph||(impl_->storage&&!impl_->storage->completion.IsSatisfiedBy(completed)))return Fail(ErrorCode::InvalidState,"RTAO reset requires all queue uses completed.");
    if(auto old=impl_->storage)if(old->nrd)if(auto r=old->nrd->ResetAfterCompletion(completed);!r)return r;
    impl_->storage.reset();return Result::Success();
}
} // namespace hs
