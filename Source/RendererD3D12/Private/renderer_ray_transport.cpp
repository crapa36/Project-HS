#include "renderer_ray_transport.hpp"
#include "renderer_pipeline_cache.hpp"
#include "renderer_feature_resolve.hpp"
#include "d3d12_resources.hpp"

#include <D3D12MemAlloc.h>
#include <wrl/client.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>
#include <initializer_list>
#include <string>
#include <utility>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr UINT kStages=3,kConstantStride=768,kExportCount=12;
constexpr DXGI_FORMAT kRadiance=DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr DXGI_FORMAT kValidation=DXGI_FORMAT_R32G32B32A32_UINT;
constexpr DXGI_FORMAT kHdr=DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kMetric=DXGI_FORMAT_R16G16_FLOAT;
constexpr std::string_view kProducts[]{
    "render/shader/rtgi_trace_cs.dxil","render/shader/rtgi_temporal_cs.dxil","render/shader/rtgi_spatial_cs.dxil",
    "render/shader/hybrid_depth_init_cs.dxil","render/shader/hybrid_depth_reduce_cs.dxil",
    "render/shader/hybrid_reflection_trace_cs.dxil","render/shader/hybrid_reflection_temporal_cs.dxil",
    "render/shader/hybrid_reflection_spatial_cs.dxil","render/shader/ray_transport_compose_cs.dxil"};
Result Fail(ErrorCode code,std::string message)
{return Result::Failure(code,"hs_renderer_ray_transport",std::move(message));}
Result Native(std::string_view operation,HRESULT hr)
{return Fail(ErrorCode::TaskFailed,std::string(operation)+" failed (HRESULT "+std::to_string(std::int32_t(hr))+").");}
bool Extent(ViewExtent e)
{return e.width&&e.height&&e.width<=D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION&&e.height<=D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;}
bool Quality(GraphicsQuality q)
{return q==GraphicsQuality::Low||q==GraphicsQuality::Medium||q==GraphicsQuality::High||q==GraphicsQuality::Ultra;}
ViewExtent SignalExtent(ViewExtent e,const render_data::QualityProfile &p)
{return {std::max(1u,UINT(std::ceil(e.width*p.rt_axis_ratio))),std::max(1u,UINT(std::ceil(e.height*p.rt_axis_ratio))) };}
DirectX::XMFLOAT4 Size(ViewExtent e)
{return {float(e.width),float(e.height),1.0f/float(e.width),1.0f/float(e.height)};}
DirectX::XMUINT4 Budget(const render_data::QualityProfile &p,bool specular)
{return {specular?p.rt_specular_samples:p.rt_diffuse_samples,specular?p.ssr_max_steps:0u,p.rt_spatial_radius,p.rt_diffuse_bounces};}
bool Finite(const DirectX::XMFLOAT4X4 &m)
{for(const auto &row:m.m)for(float f:row)if(!std::isfinite(f))return false;return true;}
bool Guide(const RenderGraphBuilder &graph,const AmbientOcclusionGuide &input,ViewExtent active,DXGI_FORMAT format)
{
    if(!input.resource||!input.version.resource||input.version.resource.kind!=ResourceKind::Texture||
       graph.Resolve(input.version.resource)!=input.resource)return false;
    const auto d=input.resource->GetDesc();
    return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.DepthOrArraySize==1&&d.SampleDesc.Count==1&&
        d.Width>=active.width&&d.Height>=active.height&&d.Format==format;
}
bool Depth(const RenderGraphBuilder &g,const AmbientOcclusionGuide &r,ViewExtent e)
{return Guide(g,r,e,DXGI_FORMAT_R32_TYPELESS)||Guide(g,r,e,DXGI_FORMAT_R32_FLOAT)||Guide(g,r,e,DXGI_FORMAT_D32_FLOAT);}
bool BufferVersion(const RenderGraphBuilder &g,ResourceVersion v,ID3D12Resource *r,std::uint64_t minimum)
{return r&&v.resource&&v.resource.kind==ResourceKind::Buffer&&g.Resolve(v.resource)==r&&
    r->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER&&r->GetDesc().Width>=minimum;}
bool SameVersion(ResourceVersion a,ResourceVersion b)
{return a.resource.value==b.resource.value&&a.resource.kind==b.resource.kind&&a.ordinal==b.ordinal;}
Result Texture(D3D12MA::Allocator *allocator,ViewExtent extent,DXGI_FORMAT format,AllocationResource &r)
{
    D3D12MA::ALLOCATION_DESC allocation{};allocation.HeapType=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=extent.width;d.Height=extent.height;
    d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Format=format;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto hr=allocator->CreateResource(&allocation,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,&r.allocation,
        IID_PPV_ARGS(r.resource.ReleaseAndGetAddressOf()));
    if(FAILED(hr))return Native("Create ray transport texture",hr);r.graph_access=Access::Common;return Result::Success();
}
Result Buffer(D3D12MA::Allocator *allocator,std::uint64_t bytes,bool upload,AllocationResource &r)
{
    D3D12MA::ALLOCATION_DESC allocation{};allocation.HeapType=upload?D3D12_HEAP_TYPE_UPLOAD:D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=1;
    d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags=upload?D3D12_RESOURCE_FLAG_NONE:D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto hr=allocator->CreateResource(&allocation,&d,upload?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COMMON,
        nullptr,&r.allocation,IID_PPV_ARGS(r.resource.ReleaseAndGetAddressOf()));
    if(FAILED(hr))return Native("Create ray transport buffer",hr);
    r.graph_access=upload?Access::ComputeRead:Access::Common;return Result::Success();
}
} // namespace

struct RayTransportStorage
{
    struct ProducerOwner
    {
        std::vector<ComPtr<D3D12MA::Allocation>> allocations;
        std::vector<ComPtr<ID3D12Resource>> resources;
    };
    struct Signal
    {
        AllocationResource raw,raw_stats,metric_count,resolved;
        std::array<AllocationResource,2> history,history_stats,validation;
        ViewExtent capacity,previous_active;
        UINT read_index{};
        bool history_valid{};
        std::unique_ptr<NrdSystem> nrd;
        NrdRadianceMethod method{NrdRadianceMethod::Off};
        AllocationResource native_motion,native_normal,native_view_z,native_packed,native_factors;
        std::shared_ptr<ProducerOwner> native_owner;
        std::array<ComPtr<ID3D12PipelineState>,2> native_pipelines;
    } gi,specular;
    struct Slot
    {
        CompletionSet completion;
        std::array<RayShaderBinder,kStages> binders;
        std::vector<ComPtr<ID3D12Resource>> reads;
        std::vector<std::shared_ptr<const void>> owners;
    };
    ComPtr<ID3D12Device> device;
    ComPtr<D3D12MA::Allocator> allocator;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>,9> pipelines;
    ComPtr<ID3D12DescriptorHeap> descriptors,exports;
    AllocationResource constants,hierarchy,delta,adjusted_subsurface,specular_stats,composed;
    std::array<DirectX::XMUINT4,16> hierarchy_levels{};
    std::vector<Slot> slots;
    ViewExtent capacity;
    GraphicsQuality gi_quality{},specular_quality{};
    render_data::QualityProfile gi_profile, specular_profile;
    DirectX::XMFLOAT4 previous_depth_parameters{};
    DirectX::XMFLOAT2 previous_jitter{};
    RayTransportRevisions previous_revisions;
    CompletionSet completion;
    std::byte *mapped{};
    std::uint64_t previous_frame{},reset_epoch{};
    UINT descriptor_stride{},hierarchy_count{},hierarchy_elements{},bank_count{};
    bool active_gi{},active_hybrid{},usable{true};
    RayDenoiserRequest denoiser_request;
    RaySignalConsumer consumer{RaySignalConsumer::Existing};
    RayNativeTraceSelection native;
    std::unique_ptr<RayShaderExecutionReorderingSystem> ser;
    ~RayTransportStorage(){if(mapped&&constants.resource)constants.resource->Unmap(0,nullptr);}
    std::vector<AllocationResource *> Resources()
    {
        std::vector<AllocationResource *> result{&constants,&hierarchy,&delta,&adjusted_subsurface,&specular_stats,&composed};
        for(auto *signal:{&gi,&specular})
        {
            result.insert(result.end(),{&signal->raw,&signal->raw_stats,&signal->metric_count,&signal->resolved});
            result.insert(result.end(),{&signal->native_motion,&signal->native_normal,&signal->native_view_z,&signal->native_packed,&signal->native_factors});
            for(UINT i=0;i<2;++i)result.insert(result.end(),{&signal->history[i],&signal->history_stats[i],&signal->validation[i]});
        }
        return result;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT slot,UINT stage,UINT offset)const
    {auto h=descriptors->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(Bank(slot,stage)*kRayTransportSuffixCount+offset)*descriptor_stride;return h;}
    UINT Bank(UINT slot,UINT stage)const
    {return slot*bank_count+(stage==2?bank_count-1u:stage==1&&active_gi?1u:0u);}
    D3D12_CPU_DESCRIPTOR_HANDLE Export(UINT offset)const
    {auto h=exports->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(offset)*descriptor_stride;return h;}
    void Srv(UINT slot,UINT stage,UINT offset,ID3D12Resource *resource,DXGI_FORMAT format)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=format;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(resource,&d,Cpu(slot,stage,offset));
    }
    void Uav(UINT slot,UINT stage,UINT offset,ID3D12Resource *resource,DXGI_FORMAT format)
    {D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=format;d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(resource,nullptr,&d,Cpu(slot,stage,offset));}
    void Structured(UINT slot,UINT stage,UINT offset,ID3D12Resource *resource,UINT count,UINT stride,bool uav=false)
    {
        if(uav){D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
            d.Buffer.NumElements=std::max(count,1u);d.Buffer.StructureByteStride=stride;
            device->CreateUnorderedAccessView(resource,nullptr,&d,Cpu(slot,stage,offset));}
        else{D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
            d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Buffer.NumElements=std::max(count,1u);d.Buffer.StructureByteStride=stride;
            device->CreateShaderResourceView(resource,&d,Cpu(slot,stage,offset));}
    }
};

struct RayTransportSystem::Impl
{
    std::shared_ptr<RayTransportStorage> storage;
    struct Import{AllocationResource *allocation{};ResourceHandle handle;};
    struct Versions
    {
        ResourceVersion raw,raw_stats,metric_count,previous,previous_stats,previous_validation,current,current_stats,validation,resolved;
        ResourceVersion native_motion,native_normal,native_view_z,native_packed,native_factors,native_clean;
        bool native_registered{};
    };
    struct Pending
    {
        const RenderGraphBuilder *graph{};
        std::vector<Import> imports;
        Versions gi,specular;
        ResourceVersion constants,hierarchy,delta,adjusted_subsurface,specular_stats,composed;
        ViewExtent active,gi_active,specular_active;
        DirectX::XMFLOAT4 depth_parameters{};
        DirectX::XMFLOAT2 jitter{};
        RayTransportRevisions revisions;
        ID3D12Resource *depth{},*metadata{};
        std::uint64_t frame{},reset_epoch{};
        UINT slot{},gi_write{},specular_write{};
        bool gi_registered{},specular_registered{},composite_registered{},gi_history{},specular_history{},ser_registered{};
    } pending;

    Result Begin(RenderGraphBuilder &graph,const RayTransportInputs &in,const RayTransportConstants &c)
    {
        auto s=storage;
        if(!s->usable)return Fail(ErrorCode::InvalidState,"Ray transport storage faulted; drain and reset before replacement.");
        auto active=in.render_active;
        if(!Extent(active)||active.width>s->capacity.width||active.height>s->capacity.height||in.frame_slot>=s->slots.size()||
           !s->slots[in.frame_slot].completion.IsSatisfiedBy(in.completed)||!in.make_bindings||
           !Depth(graph,in.depth,active)||!Guide(graph,in.normal,active,DXGI_FORMAT_R16G16_SNORM)||
           !Guide(graph,in.metadata,active,DXGI_FORMAT_R32G32_UINT)||!Guide(graph,in.motion,active,DXGI_FORMAT_R16G16_FLOAT)||
           !Guide(graph,in.surface_history,active,DXGI_FORMAT_R32G32_FLOAT)||!Guide(graph,in.base,active,DXGI_FORMAT_R8G8B8A8_UNORM)||
           !Guide(graph,in.material,active,DXGI_FORMAT_R8G8B8A8_UNORM)||!Guide(graph,in.hdr,active,kHdr)||
           !Guide(graph,in.fallback_diffuse,active,kHdr)||!Guide(graph,in.environment_specular,active,kHdr)||
           (s->active_gi&&!Guide(graph,in.subsurface_diffuse,active,kHdr))||in.sidecar_plane_count>in.sidecars.size())
            return Fail(ErrorCode::InvalidArgument,"Invalid ray transport guides, radiance inputs, active extent or completed frame slot.");
        for(UINT i=0;i<in.sidecar_plane_count;++i)
            if(!Guide(graph,in.sidecars[i],active,DXGI_FORMAT_R32_UINT)&&!Guide(graph,in.sidecars[i],active,DXGI_FORMAT_R32G32_UINT)&&
               !Guide(graph,in.sidecars[i],active,kValidation))return Fail(ErrorCode::InvalidArgument,"Invalid ray transport sidecar.");
        if(in.scene.instance_count)
        {
            if(!BufferVersion(graph,in.scene.tlas,in.scene.tlas_resource,1)||!in.scene.tlas_address||
               in.scene.tlas_address!=in.scene.tlas_resource->GetGPUVirtualAddress()||
               !BufferVersion(graph,in.scene.metadata,in.scene.metadata_resource,std::uint64_t(in.scene.instance_count)*32)||
               !in.geometry.geometry_count||in.geometry.geometry_count!=in.geometry.geometries.size()||
               !BufferVersion(graph,in.geometry.records,in.geometry.record_resource,std::uint64_t(in.geometry.geometry_count)*sizeof(RayHitGeometry))||
               !BufferVersion(graph,in.geometry.instances,in.geometry.instance_resource,std::uint64_t(in.geometry.geometry_count)*sizeof(RayDrawInstance)))
                return Fail(ErrorCode::InvalidArgument,"Ray transport TLAS and exact hit geometry metadata do not match their graph resources.");
            if(s->native.use_omms&&(!BufferVersion(graph,in.scene.omm_view.tlas,in.scene.omm_view.resource,1)||
               in.scene.omm_view.gpu_address!=in.scene.omm_view.resource->GetGPUVirtualAddress()))
                return Fail(ErrorCode::InvalidArgument,"Native OMM transport requires the actual selected twin acceleration view.");
            if(s->native.use_omms)for(const auto &binding:in.scene.omm_view.backing)
                if(!binding||!BufferVersion(graph,binding.array_version,binding.array_resource,1)||
                   !BufferVersion(graph,binding.index_version,binding.index_resource,1))
                    return Fail(ErrorCode::InvalidArgument,"Native OMM transport backing does not match the graph.");
        }
        for(const auto &r:in.reads)
            if(!r.resource||!r.version.resource||graph.Resolve(r.version.resource)!=r.resource||
               (r.access!=Access::ComputeRead&&r.access!=Access::AllShaderRead&&r.access!=Access::ShaderRead))
                return Fail(ErrorCode::InvalidArgument,"Ray transport material, light, probe or canonical geometry read is invalid.");
        if(!Finite(c.inverse_view_projection)||!Finite(c.world_to_view)||!Finite(c.projection)||
           !std::isfinite(c.jitter_uv.x)||!std::isfinite(c.jitter_uv.y)||
           std::abs(c.jitter_uv.x*float(active.width))>0.5f||std::abs(c.jitter_uv.y*float(active.height))>0.5f||
           !std::isfinite(c.world_origin.x)||!std::isfinite(c.world_origin.y)||!std::isfinite(c.world_origin.z)||
           !std::isfinite(c.gi_parameters.x)||c.gi_parameters.x<=0||!std::isfinite(c.gi_parameters.y)||c.gi_parameters.y<=0||c.gi_parameters.y>=c.gi_parameters.x||
           !std::isfinite(c.gi_parameters.z)||c.gi_parameters.z<0||
           !std::isfinite(c.specular_parameters.x)||c.specular_parameters.x<=0||!std::isfinite(c.specular_parameters.y)||c.specular_parameters.y<=0||c.specular_parameters.y>=c.specular_parameters.x||
           !std::isfinite(c.specular_parameters.z)||c.specular_parameters.z<=0||!std::isfinite(c.specular_parameters.w)||c.specular_parameters.w<=0||c.specular_parameters.w>1||
           !std::isfinite(c.filter_parameters.x)||c.filter_parameters.x<=0||!std::isfinite(c.filter_parameters.y)||c.filter_parameters.y< -1||c.filter_parameters.y>1||
           !std::isfinite(c.filter_parameters.z)||c.filter_parameters.z<0||c.filter_parameters.z>1||!std::isfinite(c.filter_parameters.w)||c.filter_parameters.w<0||c.filter_parameters.w>1)
            return Fail(ErrorCode::InvalidArgument,"Ray transport transforms, jitter, ray distances, bias or filter parameters are invalid.");
        if(pending.graph)
        {
            if(pending.graph!=&graph||pending.slot!=in.frame_slot||pending.frame!=in.frame_index||pending.active.width!=active.width||
               pending.active.height!=active.height||pending.revisions!=in.revisions||pending.depth!=in.depth.resource||pending.metadata!=in.metadata.resource)
                return Fail(ErrorCode::InvalidState,"All ray transport stages must belong to the same render frame and surface guides.");
            return Result::Success();
        }
        Pending next;next.graph=&graph;next.slot=in.frame_slot;next.frame=in.frame_index;next.active=active;
        next.revisions=in.revisions;next.reset_epoch=s->reset_epoch;next.depth=in.depth.resource;next.metadata=in.metadata.resource;
        next.gi_active=s->consumer==RaySignalConsumer::DlssFused?active:SignalExtent(active,s->gi_profile);
        next.specular_active=s->consumer==RaySignalConsumer::DlssFused?active:SignalExtent(active,s->specular_profile);
        next.depth_parameters={c.projection._33,c.projection._34,c.projection._43,c.projection._44};next.jitter={c.jitter_uv.x,c.jitter_uv.y};
        next.gi_write=1-s->gi.read_index;next.specular_write=1-s->specular.read_index;
        const bool history=!in.reset_history&&in.revisions==s->previous_revisions&&in.frame_index==s->previous_frame+1;
        next.gi_history=history&&s->gi.history_valid;next.specular_history=history&&s->specular.history_valid;
        s->slots[in.frame_slot].reads.clear();s->slots[in.frame_slot].binders={};s->slots[in.frame_slot].owners.clear();
        pending=std::move(next);pending.constants=ImportResource(graph,s->constants,"RayTransport.Constants");
        return Result::Success();
    }
    ResourceVersion ImportResource(RenderGraphBuilder &graph,AllocationResource &r,std::string_view name)
    {
        for(const auto &i:pending.imports)if(i.allocation==&r)return graph.Version(i.handle);
        ResourceHandle h=r.resource->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER?
            ResourceHandle(graph.ImportBuffer({r.resource.Get(),r.graph_completion,r.graph_queue},r.graph_access,name)):
            ResourceHandle(graph.ImportTexture({r.resource.Get(),r.graph_completion,r.graph_queue},r.graph_access,name));
        pending.imports.push_back({&r,h});return graph.Version(h);
    }
    void ImportSignal(RenderGraphBuilder &graph,RayTransportStorage::Signal &signal,Versions &v,UINT write,bool history,std::string_view name)
    {
        const auto label=std::string(name);
        v.raw=ImportResource(graph,signal.raw,label+".NoisyRadiance");v.raw_stats=ImportResource(graph,signal.raw_stats,label+".NoisyStatistics");
        if(storage->consumer!=RaySignalConsumer::Existing)
        {
            v.metric_count=ImportResource(graph,signal.metric_count,label+".RawMetricHitDistanceAcceptedPaths");
            return;
        }
        if(signal.nrd)
        {
            v.native_motion=ImportResource(graph,signal.native_motion,label+".NRD.Motion");
            v.native_normal=ImportResource(graph,signal.native_normal,label+".NRD.NormalRoughness");
            v.native_view_z=ImportResource(graph,signal.native_view_z,label+".NRD.LinearViewZ");
            v.native_packed=ImportResource(graph,signal.native_packed,label+".NRD.RadianceHitDistance");
            v.native_factors=ImportResource(graph,signal.native_factors,label+".NRD.MaterialFactors");
            v.resolved=ImportResource(graph,signal.resolved,label+".CleanRadiance");
            return;
        }
        v.current=ImportResource(graph,signal.history[write],label+".CurrentRadiance");
        v.current_stats=ImportResource(graph,signal.history_stats[write],label+".CurrentStatistics");
        v.validation=ImportResource(graph,signal.validation[write],label+".CurrentValidation");
        v.resolved=ImportResource(graph,signal.resolved,label+".CleanRadiance");
        if(history)
        {
            v.previous=ImportResource(graph,signal.history[signal.read_index],label+".PreviousRadiance");
            v.previous_stats=ImportResource(graph,signal.history_stats[signal.read_index],label+".PreviousStatistics");
            v.previous_validation=ImportResource(graph,signal.validation[signal.read_index],label+".PreviousValidation");
        }
    }
    Access TraceRead(Access access=Access::ComputeRead)const
    {return storage->native.use_ser&&access==Access::ComputeRead?Access::RayTracingRead:access;}
    Access TraceWrite()const{return storage->native.use_ser?Access::RayTracingWrite:Access::UnorderedWrite;}
    void TraceGuides(PassBuilder &pass,const RayTransportInputs &in)
    {for(auto r:{in.depth,in.normal,in.metadata,in.material})pass.Read(r.version,TraceRead());pass.Read(pending.constants,TraceRead());}
    void TraceScene(PassBuilder &pass,const RayTransportInputs &in)
    {
        if(!in.scene.instance_count)return;
        pass.Read(storage->native.use_omms?in.scene.omm_view.tlas:in.scene.tlas,Access::ASRead);
        pass.Read(in.scene.metadata,TraceRead());pass.Read(in.geometry.records,TraceRead());pass.Read(in.geometry.instances,TraceRead());
        if(storage->native.use_omms)for(const auto &binding:in.scene.omm_view.backing)
        {pass.Read(binding.array_version,Access::ASRead);pass.Read(binding.index_version,TraceRead());}
    }
    Result TraceDispatch(RenderGraphBuilder &graph,PassBuilder &pass,const RayTransportInputs &in,RayNativeSignal signal,
        RayShaderBinder binder,ViewExtent extent,std::initializer_list<ResourceVersion> outputs)
    {
        auto s=storage;std::vector<std::pair<ResourceVersion,ID3D12Resource*>> exact;
        for(const auto output:outputs)if(output.resource)exact.emplace_back(output,static_cast<ID3D12Resource*>(graph.Resolve(output.resource)));
        std::function<Result(RenderPassContext &)> dispatch;
        if(s->ser)
        {
            if(auto result=s->ser->RegisterDispatch(graph,pass,signal,extent,pending.slot,in.completed,std::move(binder),dispatch);!result)return result;
            pending.ser_registered=true;
        }
        else dispatch=[s,binder=std::move(binder),signal,extent](RenderPassContext &ctx)->Result
        {
            auto *cmd=static_cast<ID3D12GraphicsCommandList*>(ctx.NativeCommandList());const UINT pipeline=signal==RayNativeSignal::Diffuse?0u:5u;
            if(!cmd||!s->pipelines[pipeline])return Fail(ErrorCode::InvalidState,"Selected ray transport trace pipeline is unavailable.");
            cmd->SetComputeRootSignature(s->root.Get());cmd->SetPipelineState(s->pipelines[pipeline].Get());
            if(auto r=binder(cmd);!r)return r;const UINT values[]{0,0};cmd->SetComputeRoot32BitConstants(6,2,values,0);
            cmd->Dispatch((extent.width+7)/8,(extent.height+7)/8,1);return Result::Success();
        };
        pass.SetExecuteChecked([dispatch=std::move(dispatch),exact=std::move(exact)](RenderPassContext &ctx)->Result
        {for(const auto &[version,resource]:exact)if(!resource||ctx.Resolve(version.resource)!=resource)
            return Fail(ErrorCode::InvalidState,"Native trace output versions differ from registered resources.");return dispatch(ctx);});
        return Result::Success();
    }
    Result Bind(const RayTransportInputs &in,RayTransportConstants c,UINT stage,RayShaderBinder &binder,
        const RayDiffuseOutputs *external_diffuse=nullptr,const RaySpecularOutputs *external_specular=nullptr)
    {
        auto s=storage;auto &p=pending;const UINT slot=p.slot;
        c.render_size=Size(p.active);c.gi_size=Size(p.gi_active);c.specular_size=Size(p.specular_active);
        c.previous_gi_size=Size(p.gi_history?s->gi.previous_active:p.gi_active);
        c.previous_specular_size=Size(p.specular_history?s->specular.previous_active:p.specular_active);
        c.previous_depth_parameters=(p.gi_history||p.specular_history)?s->previous_depth_parameters:DirectX::XMFLOAT4{};
        c.jitter_uv.z=s->previous_jitter.x;c.jitter_uv.w=s->previous_jitter.y;
        if(c.gi_parameters.z==0)c.gi_parameters.z=2.0f*std::max(c.render_size.z/std::max(std::abs(c.projection._11),1e-6f),
            c.render_size.w/std::max(std::abs(c.projection._22),1e-6f));
        c.gi_budget=Budget(s->gi_profile,false);c.specular_budget=Budget(s->specular_profile,true);
        c.frame_info={p.gi_history?1u:0u,p.specular_history?1u:0u,UINT(p.frame),(s->active_gi?1u:0u)|(s->active_hybrid?2u:0u)};
        c.frame_info.w|=(s->gi.nrd?4u:0u)|(s->specular.nrd?8u:0u);
        if(s->consumer!=RaySignalConsumer::Existing)c.frame_info.w|=(s->active_gi?4u:0u)|(s->active_hybrid?8u:0u)|16u;
        if(s->consumer==RaySignalConsumer::DlssFused)c.frame_info.w|=32u;
        c.nrd_parameters.w=float(stage==0?s->gi.method:s->specular.method);
        c.counts={in.scene.instance_count,in.geometry.geometry_count,s->hierarchy_count,in.sidecar_plane_count};
        c.scene_material_revisions={UINT(in.revisions.scene),UINT(in.revisions.scene>>32),UINT(in.revisions.material),UINT(in.revisions.material>>32)};
        c.light_probe_revisions={UINT(in.revisions.light),UINT(in.revisions.light>>32),UINT(in.revisions.probe),UINT(in.revisions.probe>>32)};
        c.hierarchy_levels=s->hierarchy_levels;
        const UINT64 address=s->constants.resource->GetGPUVirtualAddress()+UINT64(s->Bank(slot,stage))*kConstantStride;
        std::memcpy(s->mapped+SIZE_T(s->Bank(slot,stage))*kConstantStride,&c,sizeof(c));
        D3D12_CONSTANT_BUFFER_VIEW_DESC cb{address,kConstantStride};
        s->device->CreateConstantBufferView(&cb,s->Cpu(slot,stage,0));s->device->CreateConstantBufferView(&cb,s->Cpu(slot,stage,21));
        D3D12_SHADER_RESOURCE_VIEW_DESC as{};as.ViewDimension=D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        as.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        as.RaytracingAccelerationStructure.Location=s->native.use_omms?in.scene.omm_view.gpu_address:in.scene.tlas_address;
        s->device->CreateShaderResourceView(nullptr,&as,s->Cpu(slot,stage,1));
        s->Structured(slot,stage,2,in.scene.metadata_resource,in.scene.instance_count,32);
        s->Structured(slot,stage,3,in.geometry.record_resource,in.geometry.geometry_count,sizeof(RayHitGeometry));
        s->Structured(slot,stage,4,in.geometry.record_resource,in.geometry.geometry_count,sizeof(RayHitGeometry));
        D3D12_SHADER_RESOURCE_VIEW_DESC raw{};raw.Format=DXGI_FORMAT_R32_TYPELESS;raw.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
        raw.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;raw.Buffer.NumElements=in.geometry.position_resource?UINT(in.geometry.position_resource->GetDesc().Width/4):1;
        raw.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;s->device->CreateShaderResourceView(in.geometry.position_resource,&raw,s->Cpu(slot,stage,5));
        s->Srv(slot,stage,6,in.depth.resource,DXGI_FORMAT_R32_FLOAT);s->Srv(slot,stage,7,in.normal.resource,DXGI_FORMAT_R16G16_SNORM);
        s->Srv(slot,stage,8,in.metadata.resource,DXGI_FORMAT_R32G32_UINT);s->Srv(slot,stage,9,in.motion.resource,DXGI_FORMAT_R16G16_FLOAT);
        s->Srv(slot,stage,10,in.surface_history.resource,DXGI_FORMAT_R32G32_FLOAT);
        s->Srv(slot,stage,11,nullptr,kRadiance);s->Srv(slot,stage,12,nullptr,kRadiance);s->Srv(slot,stage,13,nullptr,DXGI_FORMAT_R32G32_UINT);
        s->Srv(slot,stage,14,nullptr,kRadiance);s->Structured(slot,stage,15,in.geometry.instance_resource,in.geometry.geometry_count,sizeof(RayDrawInstance));
        D3D12_UNORDERED_ACCESS_VIEW_DESC empty{};empty.Format=DXGI_FORMAT_R32_TYPELESS;empty.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
        empty.Buffer.NumElements=1;empty.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;s->device->CreateUnorderedAccessView(nullptr,nullptr,&empty,s->Cpu(slot,stage,16));
        s->Uav(slot,stage,17,nullptr,kRadiance);s->Uav(slot,stage,18,nullptr,kRadiance);s->Uav(slot,stage,19,nullptr,DXGI_FORMAT_R32G32_UINT);s->Uav(slot,stage,20,nullptr,DXGI_FORMAT_R16_FLOAT);
        for(UINT i=0;i<6;++i)
        {
            const AmbientOcclusionGuide inputs[]{in.base,in.material,in.hdr,in.fallback_diffuse,in.environment_specular,in.subsurface_diffuse};
            s->Srv(slot,stage,22+i,inputs[i].resource,i<2?DXGI_FORMAT_R8G8B8A8_UNORM:kHdr);
        }
        for(UINT i=0;i<2;++i)s->Srv(slot,stage,28+i,i<in.sidecar_plane_count?in.sidecars[i].resource:nullptr,
            i<in.sidecar_plane_count?in.sidecars[i].resource->GetDesc().Format:kValidation);
        s->Structured(slot,stage,30,s->hierarchy.resource.Get(),s->hierarchy_elements,sizeof(float));
        auto descriptors=[&](RayTransportStorage::Signal &signal,UINT start,UINT write,bool valid)
        {
            s->Srv(slot,stage,start,signal.raw.resource.Get(),kRadiance);s->Srv(slot,stage,start+1,signal.raw_stats.resource.Get(),kRadiance);
            s->Srv(slot,stage,start+2,valid?signal.history[signal.read_index].resource.Get():nullptr,kRadiance);
            s->Srv(slot,stage,start+3,valid?signal.history_stats[signal.read_index].resource.Get():nullptr,kRadiance);
            s->Srv(slot,stage,start+4,valid?signal.validation[signal.read_index].resource.Get():nullptr,kValidation);
            s->Srv(slot,stage,start+5,signal.history[write].resource.Get(),kRadiance);s->Srv(slot,stage,start+6,signal.history_stats[write].resource.Get(),kRadiance);
            s->Srv(slot,stage,start+7,signal.resolved.resource.Get(),kRadiance);
        };
        descriptors(s->gi,31,p.gi_write,p.gi_history);descriptors(s->specular,39,p.specular_write,p.specular_history);
        s->Srv(slot,stage,47,stage<2?(stage==0?s->gi.metric_count:s->specular.metric_count).resource.Get():nullptr,kMetric);
        s->Structured(slot,stage,48,s->hierarchy.resource.Get(),s->hierarchy_elements,sizeof(float),true);
        auto &signal=stage==0?s->gi:s->specular;const UINT write=stage==0?p.gi_write:p.specular_write;
        s->Uav(slot,stage,49,stage<2?signal.raw.resource.Get():nullptr,kRadiance);
        s->Uav(slot,stage,50,stage<2?signal.raw_stats.resource.Get():nullptr,kRadiance);
        s->Uav(slot,stage,51,stage<2?signal.history[write].resource.Get():nullptr,kRadiance);
        s->Uav(slot,stage,52,stage<2?signal.history_stats[write].resource.Get():nullptr,kRadiance);
        s->Uav(slot,stage,53,stage<2?signal.validation[write].resource.Get():nullptr,kValidation);
        s->Uav(slot,stage,54,stage<2?signal.resolved.resource.Get():nullptr,kRadiance);
        s->Uav(slot,stage,55,stage==0?s->delta.resource.Get():stage==1?s->specular_stats.resource.Get():nullptr,kRadiance);
        s->Uav(slot,stage,56,stage==0?s->adjusted_subsurface.resource.Get():nullptr,kHdr);
        s->Uav(slot,stage,57,stage==2?s->composed.resource.Get():nullptr,kHdr);
        s->Uav(slot,stage,58,stage<2?signal.metric_count.resource.Get():nullptr,kMetric);
        s->Srv(slot,stage,59,stage<2&&signal.nrd?signal.nrd->PreparedOutput(stage==0?NrdSignal::Diffuse:NrdSignal::Specular):nullptr,kHdr);
        s->Srv(slot,stage,60,stage<2?signal.native_factors.resource.Get():nullptr,kHdr);
        s->Uav(slot,stage,61,stage<2?signal.native_motion.resource.Get():nullptr,DXGI_FORMAT_R16G16_FLOAT);
        s->Uav(slot,stage,62,stage<2?signal.native_normal.resource.Get():nullptr,DXGI_FORMAT_R10G10B10A2_UNORM);
        s->Uav(slot,stage,63,stage<2?signal.native_view_z.resource.Get():nullptr,DXGI_FORMAT_R32_FLOAT);
        s->Uav(slot,stage,64,stage<2?signal.native_packed.resource.Get():nullptr,kHdr);
        s->Uav(slot,stage,65,stage<2?signal.native_factors.resource.Get():nullptr,kHdr);
        if(external_diffuse&&stage>0)
            s->Srv(slot,stage,38,external_diffuse->diffuse.resource,kRadiance);
        if(external_specular&&stage==2)
            s->Srv(slot,stage,46,external_specular->specular.resource,kRadiance);
        if(auto r=in.make_bindings(s->Cpu(slot,stage,0),kRayTransportSuffixCount,binder);!r)return r;
        if(!binder)return Fail(ErrorCode::InvalidArgument,"Ray transport frame table builder returned no compute binder.");
        auto &lease=s->slots[slot];lease.binders[stage]=binder;
        auto retain=[&](ID3D12Resource *r){if(r&&std::none_of(lease.reads.begin(),lease.reads.end(),[&](const auto &v){return v.Get()==r;}))lease.reads.emplace_back(r);};
        for(const auto &r:in.reads)retain(r.resource);
        for(const auto &r:{in.depth,in.normal,in.metadata,in.motion,in.surface_history,in.base,in.material,in.hdr,in.fallback_diffuse,in.environment_specular,in.subsurface_diffuse})retain(r.resource);
        for(UINT i=0;i<in.sidecar_plane_count;++i)retain(in.sidecars[i].resource);
        retain(in.scene.tlas_resource);retain(in.scene.metadata_resource);retain(in.geometry.record_resource);retain(in.geometry.instance_resource);retain(in.geometry.position_resource);
        if(s->native.use_omms){retain(in.scene.omm_view.resource);for(const auto &binding:in.scene.omm_view.backing)
            {retain(binding.array_resource);retain(binding.index_resource);lease.owners.push_back(binding.owner);}}
        if(external_diffuse)retain(external_diffuse->diffuse.resource);
        if(external_specular)retain(external_specular->specular.resource);
        return Result::Success();
    }
    Result RegisterNative(RenderGraphBuilder &graph,const RayTransportInputs &in,RayTransportConstants constants,
        RayShaderBinder binder,bool specular)
    {
        auto s=storage;auto &p=pending;auto &signal=specular?s->specular:s->gi;auto &v=specular?p.specular:p.gi;
        const auto active=specular?p.specular_active:p.gi_active;
        const auto execute=[s,binder,specular](UINT pipeline,ViewExtent extent)
        {return [s,binder,specular,pipeline,extent](RenderPassContext &ctx)->Result
        {
            auto *cmd=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());
            cmd->SetComputeRootSignature(s->root.Get());cmd->SetPipelineState((specular?s->specular:s->gi).native_pipelines[pipeline].Get());
            if(auto r=binder(cmd);!r)return r;const UINT pass[]{0,0};cmd->SetComputeRoot32BitConstants(6,2,pass,0);
            cmd->Dispatch((extent.width+7)/8,(extent.height+7)/8,1);return Result::Success();
        };};
        const auto guides=[&](PassBuilder &pass)
        {
            for(const auto &r:{in.depth,in.normal,in.metadata,in.material,in.base})pass.Read(r.version,Access::ComputeRead);
            for(UINT i=0;i<in.sidecar_plane_count;++i)pass.Read(in.sidecars[i].version,Access::ComputeRead);
            // FrameCB/material closure decoding uses the same canonical table.
            for(const auto &r:in.reads)pass.Read(r.version,r.access);
            pass.Read(p.constants,Access::ComputeRead);
        };
        auto pack=graph.AddPass(specular?164u:163u,specular?"ReflectionNRDPack":"GINRDPack",QueueHint::Direct);
        guides(pack);pack.Read(in.motion.version,Access::ComputeRead);pack.Read(v.raw,Access::ComputeRead);pack.Read(v.raw_stats,Access::ComputeRead);
        v.native_motion=pack.Write(v.native_motion,Access::UnorderedWrite);v.native_normal=pack.Write(v.native_normal,Access::UnorderedWrite);
        v.native_view_z=pack.Write(v.native_view_z,Access::UnorderedWrite);v.native_packed=pack.Write(v.native_packed,Access::UnorderedWrite);
        v.native_factors=pack.Write(v.native_factors,Access::UnorderedWrite);pack.SetExecuteChecked(execute(0,active));
        NrdNativeInputs native;
        native.motion={v.native_motion,signal.native_motion.resource.Get()};native.normal_roughness={v.native_normal,signal.native_normal.resource.Get()};
        native.view_z={v.native_view_z,signal.native_view_z.resource.Get()};
        auto &radiance=specular?native.specular_radiance_hit_distance:native.diffuse_radiance_hit_distance;
        radiance={v.native_packed,signal.native_packed.resource.Get()};
        native.radiance_packing=signal.method==NrdRadianceMethod::Reblur?NrdRadiancePacking::ReblurRadianceAndNormHitDistance:NrdRadiancePacking::RelaxRadianceAndHitDistance;
        native.native_geometry_packed=true;native.camera=in.camera;native.identity=in.identity;native.active=active;native.frame_slot=p.slot;
        native.camera.jitter_pixels[0]*=float(active.width)/float(p.active.width);
        native.camera.jitter_pixels[1]*=float(active.height)/float(p.active.height);
        native.camera.reset_history|=in.reset_history||!(specular?p.specular_history:p.gi_history);
        native.first_pass_id=specular?5000u:3000u;native.completed=in.completed;native.owners.push_back(signal.native_owner);
        NrdSettings settings;settings.radiance=signal.method;settings.hit_distance_A=constants.nrd_parameters.x;
        settings.hit_distance_B=constants.nrd_parameters.y;settings.hit_distance_C=constants.nrd_parameters.z;
        NrdNativeOutputs outputs;
        if(auto r=signal.nrd->Register(graph,native,settings,outputs);!r)return r;
        v.native_registered=true;
        v.native_motion=outputs.final_motion;
        const auto &clean=specular?outputs.specular:outputs.diffuse;v.native_clean=clean.version;
        auto resolve=graph.AddPass(specular?166u:165u,specular?"ReflectionNRDResolve":"GINRDResolve",QueueHint::Direct);
        guides(resolve);resolve.Read(v.native_clean,Access::ComputeRead);resolve.Read(v.native_factors,Access::ComputeRead);
        resolve.Read(v.raw,Access::ComputeRead);resolve.Read(v.raw_stats,Access::ComputeRead);
        if(!specular){resolve.Read(in.fallback_diffuse.version,Access::ComputeRead);resolve.Read(in.subsurface_diffuse.version,Access::ComputeRead);
            p.delta=resolve.Write(p.delta,Access::UnorderedWrite);p.adjusted_subsurface=resolve.Write(p.adjusted_subsurface,Access::UnorderedWrite);}
        else p.specular_stats=resolve.Write(p.specular_stats,Access::UnorderedWrite);
        v.resolved=resolve.Write(v.resolved,Access::UnorderedWrite);resolve.SetExecuteChecked(execute(1,p.active));
        for(auto version:{v.native_motion,v.native_normal,v.native_view_z,v.native_packed,v.native_factors,v.native_clean})
            graph.SetFinalAccess(version,Access::ComputeRead);
        return Result::Success();
    }
};

RayTransportSystem::RayTransportSystem():impl_(std::make_unique<Impl>()){}
RayTransportSystem::~RayTransportSystem()=default;
RayDenoiserStatus RayTransportSystem::GetDenoiserStatus()const
{
    auto s=impl_->storage;auto status=DescribeRayDenoiser(s?s->denoiser_request:RayDenoiserRequest{});
    if(!s)return status;
    if(s->consumer!=RaySignalConsumer::Existing)
    {
        status.requested_provider.value=s->consumer==RaySignalConsumer::FsrStandalone?"fsr_rr":"dlss_rr";
        status.active_method=NrdRadianceMethod::Off;status.availability=ReconstructionAvailability::Unavailable;
        status.reason="The selected ray reconstruction service owns native denoising; transport owns only the actual noisy producer.";
        return status;
    }
    for(auto *signal:{&s->gi,&s->specular})if(signal->nrd)
    {
        status.active_method=signal->method;status.availability=signal->nrd->Availability();status.reason=signal->nrd->FailureReason();
        if(status.availability!=ReconstructionAvailability::Available)return status;
    }
    return status;
}
std::uint64_t RayTransportSystem::AllocatedBytes()const noexcept
{std::uint64_t bytes{};if(auto s=impl_->storage){for(auto *r:s->Resources())if(r->allocation)bytes+=r->allocation->GetSize();
    for(auto *signal:{&s->gi,&s->specular})if(signal->nrd)bytes+=signal->nrd->AllocatedBytes();
    if(s->ser)bytes+=s->ser->AllocatedBytes();}return bytes;}
RayNativeTraceSelection RayTransportSystem::GetNativeTraceSelection()const noexcept
{return impl_->storage?impl_->storage->native:RayNativeTraceSelection{};}
RaySerStatus RayTransportSystem::GetSerStatus()const
{return impl_->storage&&impl_->storage->ser?impl_->storage->ser->Status():RaySerStatus{};}
Result RayTransportSystem::Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,ID3D12RootSignature *root,
    bool gi,bool hybrid,GraphicsQuality giQuality,GraphicsQuality reflectionQuality,const render_data::QualityProfiles &profiles,ViewExtent capacity,
    UINT frames,const RayShaderReader &reader,RayTransportCandidate &candidate,RendererPipelineCache *cache,RayDenoiserRequest denoiser,
    RaySignalConsumer consumer,RayNativeTraceSelection native,std::uint64_t native_byte_budget,std::uint64_t old_native_bytes)const
{
    if(!device||!allocator||!Extent(capacity)||!frames||frames>UINT_MAX/(kStages*kRayTransportSuffixCount)||
       !Quality(giQuality)||!Quality(reflectionQuality)||((gi||hybrid)&&(!root||!reader)))
        return Fail(ErrorCode::InvalidArgument,"Invalid ray transport device, quality, capacity, slots, root or shader reader.");
    auto s=std::make_shared<RayTransportStorage>();s->device=device;s->allocator=allocator;s->capacity=capacity;s->slots.resize(frames);
    s->active_gi=gi;s->active_hybrid=hybrid;s->gi_quality=giQuality;s->specular_quality=reflectionQuality;
    s->gi_profile=profiles.profiles.at(static_cast<std::size_t>(giQuality));
    s->specular_profile=profiles.profiles.at(static_cast<std::size_t>(reflectionQuality));
    s->denoiser_request=std::move(denoiser);
    s->consumer=consumer;
    s->bank_count=(gi?1u:0u)+(hybrid?1u:0u)+1u;
    s->gi.capacity=consumer==RaySignalConsumer::DlssFused?capacity:SignalExtent(capacity,s->gi_profile);
    s->specular.capacity=consumer==RaySignalConsumer::DlssFused?capacity:SignalExtent(capacity,s->specular_profile);
    if(!gi&&!hybrid){candidate.storage_=std::move(s);return Result::Success();}
    s->native=native;
    if(native.use_omms)
    {
        const auto hardware=QueryRendererFeatureHardware(device);
        if(!hardware.opacity_micromap_arrays||!hardware.shader_model69)
            return Fail(ErrorCode::InvalidState,"Selected OMM transport requires actual native opacity micromap hardware and Shader Model6.9.");
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
    if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&options,sizeof(options)))||options.RaytracingTier<D3D12_RAYTRACING_TIER_1_1||
       FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&model,sizeof(model)))||model.HighestShaderModel<D3D_SHADER_MODEL_6_5)
        return Fail(ErrorCode::TaskFailed,"Native ray transport requires DXR1.1 and Shader Model6.5.");
    s->root=root;
    if(native.use_ser)
    {
        s->ser=std::make_unique<RayShaderExecutionReorderingSystem>();RaySerCandidate prepared;
        RaySerProfile profile{true,native.use_omms,gi,hybrid,frames,native_byte_budget,old_native_bytes};
        if(auto r=s->ser->Prepare(device,allocator,root,profile,reader,prepared);!r)return r;
        if(auto r=s->ser->Commit(std::move(prepared),{});!r)return r;
    }
    for(auto format:{kRadiance,kValidation,kHdr})
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};const auto hr=device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support));
        if(FAILED(hr))return Native("Check ray transport UAV format",hr);
        if(!(support.Support1&D3D12_FORMAT_SUPPORT1_TEXTURE2D)||!(support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_LOAD)||!(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
            return Fail(ErrorCode::TaskFailed,"Ray transport texture format lacks required shader load/UAV store support.");
    }
    auto signal=[&](RayTransportStorage::Signal &v,NrdSignal kind)->Result
    {
        for(auto *r:{&v.raw,&v.raw_stats})if(auto result=Texture(allocator,v.capacity,kRadiance,*r);!result)return result;
        if(consumer!=RaySignalConsumer::Existing)return Texture(allocator,v.capacity,kMetric,v.metric_count);
        if(auto r=Texture(allocator,capacity,kRadiance,v.resolved);!r)return r;
        v.method=ResolveRayDenoiser(s->denoiser_request);
        if(v.method!=NrdRadianceMethod::Off)
        {
            auto native=std::make_unique<NrdSystem>();NrdCandidate prepared;
            if(auto r=native->Prepare(device,allocator,v.capacity,frames,reader,prepared,{kind,v.method});!r)return r;
            if(auto r=native->Commit(std::move(prepared),{});!r)return r;
            v.nrd=std::move(native);v.native_owner=std::make_shared<RayTransportStorage::ProducerOwner>();
            const std::pair<AllocationResource *,DXGI_FORMAT> outputs[]{
                {&v.native_motion,DXGI_FORMAT_R16G16_FLOAT},{&v.native_normal,DXGI_FORMAT_R10G10B10A2_UNORM},
                {&v.native_view_z,DXGI_FORMAT_R32_FLOAT},{&v.native_packed,kHdr},{&v.native_factors,kHdr}};
            for(const auto &[resource,format]:outputs)
            {
                D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
                if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)))||
                   !(support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_LOAD)||!(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
                    return Fail(ErrorCode::TaskFailed,"NRD ray conversion requires native shader-load/UAV-store formats.");
                if(auto r=Texture(allocator,v.capacity,format,*resource);!r)return r;
                v.native_owner->allocations.push_back(resource->allocation);v.native_owner->resources.push_back(resource->resource);
            }
            const std::string_view productsGi[]{"render/shader/rtgi_nrd_pack_cs.dxil","render/shader/rtgi_nrd_resolve_cs.dxil"};
            const std::string_view productsSpec[]{"render/shader/reflection_nrd_pack_cs.dxil","render/shader/reflection_nrd_resolve_cs.dxil"};
            for(UINT i=0;i<2;++i)
            {
                const auto product=kind==NrdSignal::Diffuse?productsGi[i]:productsSpec[i];std::vector<std::byte> code;
                if(auto r=reader(product,code);!r)return r;
                if(code.empty())return Fail(ErrorCode::TaskFailed,"NRD conversion shader product is empty: "+std::string(product));
                D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=root;p.CS={code.data(),code.size()};
                const auto hr=cache?cache->CreateCompute(&p,IID_PPV_ARGS(&v.native_pipelines[i])):device->CreateComputePipelineState(&p,IID_PPV_ARGS(&v.native_pipelines[i]));
                if(FAILED(hr))return Native("Create NRD ray conversion PSO",hr);
            }
            return Result::Success();
        }
        for(UINT i=0;i<2;++i)
        {
            if(auto r=Texture(allocator,v.capacity,kRadiance,v.history[i]);!r)return r;
            if(auto r=Texture(allocator,v.capacity,kRadiance,v.history_stats[i]);!r)return r;
            if(auto r=Texture(allocator,v.capacity,kValidation,v.validation[i]);!r)return r;
        }
        return Result::Success();
    };
    if(gi)
    {
        if(auto r=signal(s->gi,NrdSignal::Diffuse);!r)return r;
        if(consumer!=RaySignalConsumer::FsrStandalone)
        {
            if(auto r=Texture(allocator,capacity,kRadiance,s->delta);!r)return r;
            if(auto r=Texture(allocator,capacity,kHdr,s->adjusted_subsurface);!r)return r;
        }
    }
    if(hybrid)
    {
        if(auto r=signal(s->specular,NrdSignal::Specular);!r)return r;
        if(consumer==RaySignalConsumer::Existing)if(auto r=Texture(allocator,capacity,kRadiance,s->specular_stats);!r)return r;
        auto extent=capacity;
        for(;;)
        {
            if(s->hierarchy_count==s->hierarchy_levels.size())return Fail(ErrorCode::InvalidArgument,"Ray transport hierarchy exceeds its16-level ABI.");
            s->hierarchy_levels[s->hierarchy_count++]={extent.width,extent.height,s->hierarchy_elements,0};
            s->hierarchy_elements+=extent.width*extent.height;
            if(extent.width==1&&extent.height==1)break;extent={(extent.width+1)/2,(extent.height+1)/2};
        }
        if(auto r=Buffer(allocator,std::uint64_t(s->hierarchy_elements)*sizeof(float),false,s->hierarchy);!r)return r;
    }
    if(auto r=Texture(allocator,capacity,kHdr,s->composed);!r)return r;
    for(UINT i=0;i<std::size(kProducts);++i)
    {
        if((i<3&&!gi)||(i>=3&&i<8&&!hybrid))continue;
        if((s->gi.nrd&&(i==1||i==2))||(s->specular.nrd&&(i==6||i==7)))continue;
        if(consumer!=RaySignalConsumer::Existing&&(i==1||i==2||i==6||i==7))continue;
        if(native.use_ser&&(i==0||i==5))continue;
        const auto product=native.use_omms&&i==0?std::string_view("render/shader/rtgi_trace_omm_cs.dxil"):
            native.use_omms&&i==5?std::string_view("render/shader/hybrid_reflection_trace_omm_cs.dxil"):kProducts[i];
        std::vector<std::byte> code;if(auto r=reader(product,code);!r)return r;
        if(code.empty())return Fail(ErrorCode::TaskFailed,"Ray transport shader product is empty: "+std::string(kProducts[i]));
        D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=root;p.CS={code.data(),code.size()};
        const auto hr=cache?cache->CreateCompute(&p,IID_PPV_ARGS(&s->pipelines[i])):device->CreateComputePipelineState(&p,IID_PPV_ARGS(&s->pipelines[i]));
        if(FAILED(hr))return Native("Create ray transport PSO "+std::string(kProducts[i]),hr);
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=frames*s->bank_count*kRayTransportSuffixCount;
    auto hr=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&s->descriptors));if(FAILED(hr))return Native("Create ray transport CPU descriptors",hr);
    s->descriptor_stride=device->GetDescriptorHandleIncrementSize(heap.Type);heap.NumDescriptors=kExportCount;
    hr=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&s->exports));if(FAILED(hr))return Native("Create ray transport output descriptors",hr);
    ID3D12Resource *resources[]{s->gi.resolved.resource.Get(),s->delta.resource.Get(),s->adjusted_subsurface.resource.Get(),s->gi.raw.resource.Get(),s->gi.raw_stats.resource.Get(),
        s->specular.resolved.resource.Get(),s->specular.raw.resource.Get(),s->specular_stats.resource.Get(),s->composed.resource.Get(),
        s->gi.metric_count.resource.Get(),s->specular.metric_count.resource.Get(),s->specular.raw_stats.resource.Get()};
    for(UINT i=0;i<kExportCount;++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=i==9||i==10?kMetric:i==2||i==8?kHdr:kRadiance;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(resources[i],&d,s->Export(i));
    }
    if(auto r=Buffer(allocator,UINT64(frames)*s->bank_count*kConstantStride,true,s->constants);!r)return r;
    void *mapped{};D3D12_RANGE empty{0,0};hr=s->constants.resource->Map(0,&empty,&mapped);
    if(FAILED(hr))return Native("Map ray transport constants",hr);s->mapped=static_cast<std::byte *>(mapped);
    candidate.storage_=std::move(s);return Result::Success();
}
Result RayTransportSystem::Commit(RayTransportCandidate &&candidate,const CompletionSet &completed)
{
    if(!candidate.storage_||impl_->pending.graph)return Fail(ErrorCode::InvalidState,"Ray transport candidate absent or submission pending.");
    if(impl_->storage&&(!impl_->storage->usable||!impl_->storage->completion.IsSatisfiedBy(completed)))
        return Fail(ErrorCode::InvalidState,"Ray transport replacement requires completed usable storage.");
    if(auto old=impl_->storage)for(auto *signal:{&old->gi,&old->specular})
        if(signal->nrd)if(auto r=signal->nrd->ResetAfterCompletion(completed);!r)return r;
    if(auto old=impl_->storage;old&&old->ser)if(auto r=old->ser->ResetAfterCompletion(completed);!r)return r;
    impl_->storage=std::move(candidate.storage_);return Result::Success();
}

Result RayTransportSystem::RegisterNoisyDiffuse(RenderGraphBuilder &graph,const RayTransportInputs &in,RayTransportConstants c,RayNoisySignal &out,RayDiffuseOutputs *composition)
{
    out={};auto s=impl_->storage;
    if(!s||s->consumer==RaySignalConsumer::Existing)return Fail(ErrorCode::InvalidState,"Raw RR registration requires a selected RR transport candidate.");
    if(!s->active_gi)return Result::Success();
    if(auto r=impl_->Begin(graph,in,c);!r)return r;auto &p=impl_->pending;
    if(p.gi_registered||p.specular_registered)return Fail(ErrorCode::InvalidState,"Register noisy diffuse once before noisy specular.");
    RayShaderBinder binder;if(auto r=impl_->Bind(in,c,0,binder);!r)return r;
    impl_->ImportSignal(graph,s->gi,p.gi,p.gi_write,false,"RR.Diffuse");
    auto trace=graph.AddPass(156,"RRDiffuseTrace",QueueHint::Direct);
    for(auto r:{in.depth,in.normal,in.metadata,in.base,in.material})trace.Read(r.version,impl_->TraceRead());
    trace.Read(p.constants,impl_->TraceRead());
    impl_->TraceScene(trace,in);
    for(UINT i=0;i<in.sidecar_plane_count;++i)trace.Read(in.sidecars[i].version,impl_->TraceRead());
    for(const auto &r:in.reads)trace.Read(r.version,impl_->TraceRead(r.access));
    p.gi.raw=trace.Write(p.gi.raw,impl_->TraceWrite());p.gi.raw_stats=trace.Write(p.gi.raw_stats,impl_->TraceWrite());
    p.gi.metric_count=trace.Write(p.gi.metric_count,impl_->TraceWrite());
    if(s->consumer==RaySignalConsumer::DlssFused)
    {
        trace.Read(in.fallback_diffuse.version,impl_->TraceRead());trace.Read(in.subsurface_diffuse.version,impl_->TraceRead());
        p.delta=impl_->ImportResource(graph,s->delta,"DLSS RR.NoisyDiffuseDelta");
        p.adjusted_subsurface=impl_->ImportResource(graph,s->adjusted_subsurface,"DLSS RR.NoisySubsurface");
        p.delta=trace.Write(p.delta,impl_->TraceWrite());p.adjusted_subsurface=trace.Write(p.adjusted_subsurface,impl_->TraceWrite());
        graph.SetFinalAccess(p.delta,Access::ComputeRead);graph.SetFinalAccess(p.adjusted_subsurface,Access::ComputeRead);
    }
    if(auto r=impl_->TraceDispatch(graph,trace,in,RayNativeSignal::Diffuse,binder,p.gi_active,
        {p.gi.raw,p.gi.raw_stats,p.gi.metric_count,p.delta,p.adjusted_subsurface});!r)return r;
    for(auto v:{p.gi.raw,p.gi.raw_stats,p.gi.metric_count})graph.SetFinalAccess(v,Access::ComputeRead);
    out={{p.gi.raw,s->gi.raw.resource.Get(),s->Export(3)},{p.gi.raw_stats,s->gi.raw_stats.resource.Get(),s->Export(4)},
        {p.gi.metric_count,s->gi.metric_count.resource.Get(),s->Export(9)},p.gi_active,false,s};
    if(composition&&s->consumer==RaySignalConsumer::DlssFused)
        *composition={out.color,{p.delta,s->delta.resource.Get(),s->Export(1)},
            {p.adjusted_subsurface,s->adjusted_subsurface.resource.Get(),s->Export(2)},out.color,out.statistics};
    p.gi_registered=true;return Result::Success();
}
Result RayTransportSystem::RegisterNoisySpecular(RenderGraphBuilder &graph,const RayTransportInputs &in,
    const RayDiffuseOutputs &diffuse,RayTransportConstants c,RayNoisySignal &out)
{
    out={};auto s=impl_->storage;
    if(!s||s->consumer==RaySignalConsumer::Existing)return Fail(ErrorCode::InvalidState,"Raw RR registration requires a selected RR transport candidate.");
    if(!s->active_hybrid)return Result::Success();
    if(auto r=impl_->Begin(graph,in,c);!r)return r;auto &p=impl_->pending;
    if(p.specular_registered||(s->active_gi&&!p.gi_registered))return Fail(ErrorCode::InvalidState,"Register noisy specular once after the active diffuse trace.");
    const bool cleanDiffuse=s->active_gi&&s->consumer==RaySignalConsumer::FsrStandalone;
    if(cleanDiffuse&&!Guide(graph,{diffuse.diffuse.version,diffuse.diffuse.resource},p.active,kRadiance))
        return Fail(ErrorCode::InvalidArgument,"FSR standalone reflection requires the current full-resolution clean diffuse result before SSS.");
    RayShaderBinder binder;if(auto r=impl_->Bind(in,c,1,binder,cleanDiffuse?&diffuse:nullptr);!r)return r;
    impl_->ImportSignal(graph,s->specular,p.specular,p.specular_write,false,"RR.Specular");
    p.hierarchy=impl_->ImportResource(graph,s->hierarchy,"RR.ReflectionClosestDepth");
    auto hierarchy=graph.AddPass(159,"RRReflectionDepth",QueueHint::Direct);
    hierarchy.Read(in.depth.version,Access::ComputeRead);hierarchy.Read(p.constants,Access::ComputeRead);
    p.hierarchy=hierarchy.Write(p.hierarchy,Access::UnorderedWrite);
    hierarchy.SetExecuteChecked([s,binder,extent=p.active,handle=p.hierarchy.resource](RenderPassContext &ctx)->Result{
        auto *cmd=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());cmd->SetComputeRootSignature(s->root.Get());cmd->SetPipelineState(s->pipelines[3].Get());
        if(auto r=binder(cmd);!r)return r;const UINT values[]{0,0};cmd->SetComputeRoot32BitConstants(6,2,values,0);cmd->Dispatch((extent.width+7)/8,(extent.height+7)/8,1);ctx.UavBarrier(handle);
        auto active=extent;for(UINT level=1;level<s->hierarchy_count;++level){active={(active.width+1)/2,(active.height+1)/2};cmd->SetComputeRoot32BitConstant(6,level,0);cmd->SetPipelineState(s->pipelines[4].Get());cmd->Dispatch((active.width+7)/8,(active.height+7)/8,1);ctx.UavBarrier(handle);}return Result::Success();});
    auto trace=graph.AddPass(160,"RRSpecularTrace",QueueHint::Direct);
    for(auto r:{in.depth,in.normal,in.metadata,in.base,in.material,in.hdr,in.environment_specular})trace.Read(r.version,impl_->TraceRead());
    trace.Read(p.constants,impl_->TraceRead());trace.Read(p.hierarchy,impl_->TraceRead());
    if(cleanDiffuse){trace.Read(diffuse.diffuse.version,impl_->TraceRead());trace.Read(in.fallback_diffuse.version,impl_->TraceRead());}
    impl_->TraceScene(trace,in);
    for(UINT i=0;i<in.sidecar_plane_count;++i)trace.Read(in.sidecars[i].version,impl_->TraceRead());
    for(const auto &r:in.reads)trace.Read(r.version,impl_->TraceRead(r.access));
    p.specular.raw=trace.Write(p.specular.raw,impl_->TraceWrite());p.specular.raw_stats=trace.Write(p.specular.raw_stats,impl_->TraceWrite());
    p.specular.metric_count=trace.Write(p.specular.metric_count,impl_->TraceWrite());
    if(auto r=impl_->TraceDispatch(graph,trace,in,RayNativeSignal::Specular,binder,p.specular_active,
        {p.specular.raw,p.specular.raw_stats,p.specular.metric_count});!r)return r;
    for(auto v:{p.specular.raw,p.specular.raw_stats,p.specular.metric_count})graph.SetFinalAccess(v,Access::ComputeRead);
    out={{p.specular.raw,s->specular.raw.resource.Get(),s->Export(6)},{p.specular.raw_stats,s->specular.raw_stats.resource.Get(),s->Export(11)},
        {p.specular.metric_count,s->specular.metric_count.resource.Get(),s->Export(10)},p.specular_active,true,s};
    p.specular_registered=true;return Result::Success();
}

Result RayTransportSystem::RegisterDiffuse(RenderGraphBuilder &graph,const RayTransportInputs &in,RayTransportConstants c,RayDiffuseOutputs &out)
{
    out={};auto s=impl_->storage;if(!s)return Fail(ErrorCode::InvalidState,"Ray transport service has not been committed.");
    if(!s->active_gi)return Result::Success();
    if(s->consumer!=RaySignalConsumer::Existing)return Fail(ErrorCode::InvalidState,"Selected RR transport uses RegisterNoisyDiffuse and its RR service.");
    if(auto r=impl_->Begin(graph,in,c);!r)return r;auto &p=impl_->pending;
    if(p.gi_registered||p.specular_registered||p.composite_registered)return Fail(ErrorCode::InvalidState,"Register diffuse once before SSS and specular.");
    RayShaderBinder binder;if(auto r=impl_->Bind(in,c,0,binder);!r)return r;
    impl_->ImportSignal(graph,s->gi,p.gi,p.gi_write,p.gi_history,"RTGI");
    p.delta=impl_->ImportResource(graph,s->delta,"RTGI.DiffuseDelta");p.adjusted_subsurface=impl_->ImportResource(graph,s->adjusted_subsurface,"RTGI.AdjustedSubsurface");
    auto execute=[s,binder](UINT pipeline,ViewExtent extent){return [s,binder,pipeline,extent](RenderPassContext &ctx)->Result{
        auto *cmd=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());cmd->SetComputeRootSignature(s->root.Get());cmd->SetPipelineState(s->pipelines[pipeline].Get());
        if(auto r=binder(cmd);!r)return r;const UINT pass[]{0,0};cmd->SetComputeRoot32BitConstants(6,2,pass,0);cmd->Dispatch((extent.width+7)/8,(extent.height+7)/8,1);return Result::Success();};};
    auto guides=[&](PassBuilder &pass){for(auto r:{in.depth,in.normal,in.metadata,in.material})pass.Read(r.version,Access::ComputeRead);pass.Read(p.constants,Access::ComputeRead);};
    auto trace=graph.AddPass(156,"RTGITrace",QueueHint::Direct);impl_->TraceGuides(trace,in);trace.Read(in.base.version,impl_->TraceRead());
    impl_->TraceScene(trace,in);
    for(UINT i=0;i<in.sidecar_plane_count;++i)trace.Read(in.sidecars[i].version,impl_->TraceRead());
    for(const auto &r:in.reads)trace.Read(r.version,impl_->TraceRead(r.access));
    p.gi.raw=trace.Write(p.gi.raw,impl_->TraceWrite());p.gi.raw_stats=trace.Write(p.gi.raw_stats,impl_->TraceWrite());if(auto r=impl_->TraceDispatch(graph,trace,in,RayNativeSignal::Diffuse,binder,p.gi_active,{p.gi.raw,p.gi.raw_stats});!r)return r;
    if(s->gi.nrd)
    {
        if(auto r=impl_->RegisterNative(graph,in,c,binder,false);!r)return r;
    }
    else
    {
    auto temporal=graph.AddPass(157,"RTGITemporal",QueueHint::Direct);guides(temporal);temporal.Read(in.motion.version,Access::ComputeRead);temporal.Read(in.surface_history.version,Access::ComputeRead);
    temporal.Read(p.gi.raw,Access::ComputeRead);temporal.Read(p.gi.raw_stats,Access::ComputeRead);
    if(p.gi_history){temporal.Read(p.gi.previous,Access::ComputeRead);temporal.Read(p.gi.previous_stats,Access::ComputeRead);temporal.Read(p.gi.previous_validation,Access::ComputeRead);}
    p.gi.current=temporal.Write(p.gi.current,Access::UnorderedWrite);p.gi.current_stats=temporal.Write(p.gi.current_stats,Access::UnorderedWrite);p.gi.validation=temporal.Write(p.gi.validation,Access::UnorderedWrite);temporal.SetExecuteChecked(execute(1,p.gi_active));
    auto spatial=graph.AddPass(158,"RTGISpatial",QueueHint::Direct);guides(spatial);spatial.Read(p.gi.current,Access::ComputeRead);spatial.Read(p.gi.current_stats,Access::ComputeRead);
    spatial.Read(in.fallback_diffuse.version,Access::ComputeRead);spatial.Read(in.subsurface_diffuse.version,Access::ComputeRead);
    p.gi.resolved=spatial.Write(p.gi.resolved,Access::UnorderedWrite);p.delta=spatial.Write(p.delta,Access::UnorderedWrite);p.adjusted_subsurface=spatial.Write(p.adjusted_subsurface,Access::UnorderedWrite);spatial.SetExecuteChecked(execute(2,p.active));
    }
    for(auto v:{p.gi.raw,p.gi.raw_stats,p.gi.current,p.gi.current_stats,p.gi.validation,p.gi.resolved,p.delta,p.adjusted_subsurface})
        if(v.resource)graph.SetFinalAccess(v,Access::ComputeRead);
    out={{p.gi.resolved,s->gi.resolved.resource.Get(),s->Export(0)},{p.delta,s->delta.resource.Get(),s->Export(1)},
        {p.adjusted_subsurface,s->adjusted_subsurface.resource.Get(),s->Export(2)},{p.gi.raw,s->gi.raw.resource.Get(),s->Export(3)},
        {p.gi.raw_stats,s->gi.raw_stats.resource.Get(),s->Export(4)}};
    p.gi_registered=true;return Result::Success();
}
Result RayTransportSystem::RegisterSpecular(RenderGraphBuilder &graph,const RayTransportInputs &in,const RayDiffuseOutputs &diffuse,RayTransportConstants c,RaySpecularOutputs &out)
{
    out={};auto s=impl_->storage;if(!s)return Fail(ErrorCode::InvalidState,"Ray transport service has not been committed.");
    if(!s->active_hybrid)return Result::Success();
    if(s->consumer!=RaySignalConsumer::Existing)return Fail(ErrorCode::InvalidState,"Selected RR transport uses RegisterNoisySpecular and its RR service.");
    if(auto r=impl_->Begin(graph,in,c);!r)return r;auto &p=impl_->pending;
    if(p.specular_registered||p.composite_registered||(s->active_gi&&(!p.gi_registered||diffuse.diffuse.resource!=s->gi.resolved.resource.Get()||!SameVersion(diffuse.diffuse.version,p.gi.resolved))))
        return Fail(ErrorCode::InvalidState,"Register specular once after the current diffuse stage and SSS.");
    RayShaderBinder binder;if(auto r=impl_->Bind(in,c,1,binder);!r)return r;
    impl_->ImportSignal(graph,s->specular,p.specular,p.specular_write,p.specular_history,"HybridReflection");
    p.hierarchy=impl_->ImportResource(graph,s->hierarchy,"HybridReflection.ClosestDepth");p.specular_stats=impl_->ImportResource(graph,s->specular_stats,"HybridReflection.CleanStatistics");
    auto execute=[s,binder](UINT pipeline,ViewExtent extent){return [s,binder,pipeline,extent](RenderPassContext &ctx)->Result{
        auto *cmd=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());cmd->SetComputeRootSignature(s->root.Get());cmd->SetPipelineState(s->pipelines[pipeline].Get());
        if(auto r=binder(cmd);!r)return r;const UINT pass[]{0,0};cmd->SetComputeRoot32BitConstants(6,2,pass,0);cmd->Dispatch((extent.width+7)/8,(extent.height+7)/8,1);return Result::Success();};};
    auto hierarchy=graph.AddPass(159,"HybridReflectionDepth",QueueHint::Direct);hierarchy.Read(in.depth.version,Access::ComputeRead);hierarchy.Read(p.constants,Access::ComputeRead);
    p.hierarchy=hierarchy.Write(p.hierarchy,Access::UnorderedWrite);
    hierarchy.SetExecuteChecked([s,binder,extent=p.active,handle=p.hierarchy.resource](RenderPassContext &ctx)->Result{
        auto *cmd=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());cmd->SetComputeRootSignature(s->root.Get());cmd->SetPipelineState(s->pipelines[3].Get());
        if(auto r=binder(cmd);!r)return r;const UINT pass[]{0,0};cmd->SetComputeRoot32BitConstants(6,2,pass,0);cmd->Dispatch((extent.width+7)/8,(extent.height+7)/8,1);ctx.UavBarrier(handle);
        auto active=extent;for(UINT level=1;level<s->hierarchy_count;++level){active={(active.width+1)/2,(active.height+1)/2};cmd->SetComputeRoot32BitConstant(6,level,0);cmd->SetPipelineState(s->pipelines[4].Get());cmd->Dispatch((active.width+7)/8,(active.height+7)/8,1);ctx.UavBarrier(handle);}
        return Result::Success();});
    auto guides=[&](PassBuilder &pass){for(auto r:{in.depth,in.normal,in.metadata,in.material})pass.Read(r.version,Access::ComputeRead);pass.Read(p.constants,Access::ComputeRead);};
    auto trace=graph.AddPass(160,"HybridReflectionTrace",QueueHint::Direct);impl_->TraceGuides(trace,in);trace.Read(in.base.version,impl_->TraceRead());
    trace.Read(p.hierarchy,impl_->TraceRead());trace.Read(in.hdr.version,impl_->TraceRead());trace.Read(in.environment_specular.version,impl_->TraceRead());
    if(s->active_gi){trace.Read(p.gi.resolved,impl_->TraceRead());trace.Read(in.fallback_diffuse.version,impl_->TraceRead());}
    impl_->TraceScene(trace,in);
    for(UINT i=0;i<in.sidecar_plane_count;++i)trace.Read(in.sidecars[i].version,impl_->TraceRead());for(const auto &r:in.reads)trace.Read(r.version,impl_->TraceRead(r.access));
    p.specular.raw=trace.Write(p.specular.raw,impl_->TraceWrite());p.specular.raw_stats=trace.Write(p.specular.raw_stats,impl_->TraceWrite());if(auto r=impl_->TraceDispatch(graph,trace,in,RayNativeSignal::Specular,binder,p.specular_active,{p.specular.raw,p.specular.raw_stats});!r)return r;
    if(s->specular.nrd)
    {
        if(auto r=impl_->RegisterNative(graph,in,c,binder,true);!r)return r;
    }
    else
    {
    auto temporal=graph.AddPass(161,"HybridReflectionTemporal",QueueHint::Direct);guides(temporal);temporal.Read(in.motion.version,Access::ComputeRead);temporal.Read(in.surface_history.version,Access::ComputeRead);
    temporal.Read(p.specular.raw,Access::ComputeRead);temporal.Read(p.specular.raw_stats,Access::ComputeRead);
    if(p.specular_history){temporal.Read(p.specular.previous,Access::ComputeRead);temporal.Read(p.specular.previous_stats,Access::ComputeRead);temporal.Read(p.specular.previous_validation,Access::ComputeRead);}
    p.specular.current=temporal.Write(p.specular.current,Access::UnorderedWrite);p.specular.current_stats=temporal.Write(p.specular.current_stats,Access::UnorderedWrite);p.specular.validation=temporal.Write(p.specular.validation,Access::UnorderedWrite);temporal.SetExecuteChecked(execute(6,p.specular_active));
    auto spatial=graph.AddPass(162,"HybridReflectionSpatial",QueueHint::Direct);guides(spatial);spatial.Read(p.specular.current,Access::ComputeRead);spatial.Read(p.specular.current_stats,Access::ComputeRead);
    p.specular.resolved=spatial.Write(p.specular.resolved,Access::UnorderedWrite);p.specular_stats=spatial.Write(p.specular_stats,Access::UnorderedWrite);spatial.SetExecuteChecked(execute(7,p.active));
    }
    for(auto v:{p.specular.raw,p.specular.raw_stats,p.specular.current,p.specular.current_stats,p.specular.validation,p.specular.resolved,p.specular_stats})
        if(v.resource)graph.SetFinalAccess(v,Access::ComputeRead);
    out={{p.specular.resolved,s->specular.resolved.resource.Get(),s->Export(5)},{p.specular.raw,s->specular.raw.resource.Get(),s->Export(6)},
        {p.specular_stats,s->specular_stats.resource.Get(),s->Export(7)}};
    p.specular_registered=true;return Result::Success();
}
Result RayTransportSystem::RegisterComposite(RenderGraphBuilder &graph,const RayTransportInputs &in,const RayDiffuseOutputs &diffuse,
    const RaySpecularOutputs &specular,RayTransportConstants c,RayTransportOutputs &out)
{
    out={};auto s=impl_->storage;if(!s)return Fail(ErrorCode::InvalidState,"Ray transport service has not been committed.");
    if(!s->active_gi&&!s->active_hybrid)return Result::Success();
    if(auto r=impl_->Begin(graph,in,c);!r)return r;auto &p=impl_->pending;
    const bool external=s->consumer!=RaySignalConsumer::Existing;
    const bool diffuseValid=external?Guide(graph,{diffuse.diffuse.version,diffuse.diffuse.resource},p.active,kRadiance):
        diffuse.diffuse.resource==s->gi.resolved.resource.Get()&&SameVersion(diffuse.diffuse.version,p.gi.resolved);
    const bool specularValid=external?Guide(graph,{specular.specular.version,specular.specular.resource},p.active,kRadiance):
        specular.specular.resource==s->specular.resolved.resource.Get()&&SameVersion(specular.specular.version,p.specular.resolved);
    if(p.composite_registered||(s->active_gi&&(!p.gi_registered||!diffuseValid))||(s->active_hybrid&&(!p.specular_registered||!specularValid)))
        return Fail(ErrorCode::InvalidState,"Ray transport composite requires each enabled stage's current-frame output exactly once.");
    RayShaderBinder binder;if(auto r=impl_->Bind(in,c,2,binder,external?&diffuse:nullptr,external?&specular:nullptr);!r)return r;
    p.composed=impl_->ImportResource(graph,s->composed,"RayTransport.ComposedHDR");
    auto pass=graph.AddPass(169,"RayTransportCompose",QueueHint::Direct);pass.Read(p.constants,Access::ComputeRead);pass.Read(in.hdr.version,Access::ComputeRead);
    if(s->active_gi){pass.Read(external?diffuse.diffuse.version:p.gi.resolved,Access::ComputeRead);pass.Read(in.fallback_diffuse.version,Access::ComputeRead);}
    if(s->active_hybrid){pass.Read(external?specular.specular.version:p.specular.resolved,Access::ComputeRead);pass.Read(in.environment_specular.version,Access::ComputeRead);}
    p.composed=pass.Write(p.composed,Access::UnorderedWrite);
    pass.SetExecuteChecked([s,binder,extent=p.active](RenderPassContext &ctx)->Result{
        auto *cmd=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());cmd->SetComputeRootSignature(s->root.Get());cmd->SetPipelineState(s->pipelines[8].Get());
        if(auto r=binder(cmd);!r)return r;const UINT values[]{0,0};cmd->SetComputeRoot32BitConstants(6,2,values,0);cmd->Dispatch((extent.width+7)/8,(extent.height+7)/8,1);return Result::Success();});
    graph.SetFinalAccess(p.composed,Access::ComputeRead);out.composed={p.composed,s->composed.resource.Get(),s->Export(8)};p.composite_registered=true;return Result::Success();
}
Result RayTransportSystem::CompleteSubmission(const RenderGraphBuilder &graph,const CompletionSet &completion,bool success)
{
    auto &p=impl_->pending;auto s=impl_->storage;
    if(!p.graph)return Result::Success();
    if(p.graph!=&graph)return Fail(ErrorCode::InvalidArgument,"Ray transport completion refers to a different graph.");
    Result completed_result=Result::Success();
    if(p.ser_registered)
        if(auto r=s->ser->CompleteSubmission(graph,completion,success);!r){s->usable=false;completed_result=std::move(r);success=false;}
    for(auto pair:{std::pair{&s->gi,&p.gi},std::pair{&s->specular,&p.specular}})
        if(pair.second->native_registered)
            if(auto r=pair.first->nrd->CompleteSubmission(graph,completion,success);!r){s->usable=false;completed_result=std::move(r);success=false;}
    s->completion.Merge(completion);auto &slot=s->slots[p.slot];slot.completion.Merge(completion);
    for(const auto &i:p.imports)
    {
        const auto use=graph.ResourceCompletion(i.handle);s->completion.Merge(use);slot.completion.Merge(use);
        i.allocation->graph_completion.Merge(completion);i.allocation->graph_completion.Merge(use);
        if(success){i.allocation->graph_access=graph.FinalAccess(i.handle);i.allocation->graph_queue=graph.FinalQueue(i.handle);}
    }
    if(success&&p.composite_registered)
    {
        if(p.gi_registered&&s->consumer==RaySignalConsumer::Existing){s->gi.read_index=p.gi_write;s->gi.previous_active=p.gi_active;s->gi.history_valid=s->reset_epoch==p.reset_epoch;}
        if(p.specular_registered&&s->consumer==RaySignalConsumer::Existing){s->specular.read_index=p.specular_write;s->specular.previous_active=p.specular_active;s->specular.history_valid=s->reset_epoch==p.reset_epoch;}
        s->previous_frame=p.frame;s->previous_revisions=p.revisions;s->previous_jitter=p.jitter;s->previous_depth_parameters=p.depth_parameters;
    }
    else
    {
        s->gi.history_valid=false;s->specular.history_valid=false;
        if(!success)s->usable=false;
    }
    p={};return completed_result;
}
void RayTransportSystem::ResetHistory()noexcept
{if(auto s=impl_->storage){s->gi.history_valid=false;s->specular.history_valid=false;++s->reset_epoch;}}
Result RayTransportSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if(impl_->pending.graph)return Fail(ErrorCode::InvalidState,"Complete ray transport's pending submission before reset.");
    if(impl_->storage&&!impl_->storage->completion.IsSatisfiedBy(completed))return Fail(ErrorCode::InvalidState,"Ray transport reset requires all GPU uses completed.");
    if(auto old=impl_->storage)for(auto *signal:{&old->gi,&old->specular})
        if(signal->nrd)if(auto r=signal->nrd->ResetAfterCompletion(completed);!r)return r;
    if(auto old=impl_->storage;old&&old->ser)if(auto r=old->ser->ResetAfterCompletion(completed);!r)return r;
    impl_->storage.reset();return Result::Success();
}
} // namespace hs
