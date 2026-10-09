#include "renderer_ray_reconstruction.hpp"
#include "d3d12_resources.hpp"
#include "renderer_pipeline_cache.hpp"
#include <D3D12MemAlloc.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
enum TextureIndex : UINT { Radiance,Clean,Factors,ViewZ,Motion,Normal,DiffuseAlbedo,SpecularAlbedo,
    DiffuseHit,SpecularHit,Depth,MotionXY,Resolved,Delta,Subsurface,Responsive,TextureCount };
constexpr DXGI_FORMAT kHdr=DXGI_FORMAT_R16G16B16A16_FLOAT,kRaw=DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr UINT kBanks=2,kBankStride=1024,kRrOffset=768,kOwnOffset=kRayTransportSuffixCount;
constexpr UINT kDescriptors=kRayTransportSuffixCount+kRayReconstructionDescriptorCount;
struct alignas(16) RrConstants
{
    DirectX::XMFLOAT4X4 previous_projection,previous_view_projection;
    DirectX::XMFLOAT4 signal_size;
    DirectX::XMUINT4 mode;
};
static_assert(sizeof(RrConstants)==160);
constexpr std::array<TextureIndex,15> kUavs{Radiance,Factors,ViewZ,Motion,Normal,DiffuseAlbedo,SpecularAlbedo,
    DiffuseHit,SpecularHit,Depth,MotionXY,Resolved,Delta,Subsurface,Responsive};
constexpr std::string_view kProducts[]{"render/shader/rr_diffuse_pack_cs.dxil","render/shader/rr_diffuse_resolve_cs.dxil",
    "render/shader/rr_specular_pack_cs.dxil","render/shader/rr_specular_resolve_cs.dxil",
    "render/shader/dlss_rr_guides_cs.dxil","render/shader/rr_pre_exposure_cs.dxil"};
Result Fail(ErrorCode code,std::string message){return Result::Failure(code,"hs_renderer_ray_reconstruction",std::move(message));}
Result Native(const char *operation,HRESULT hr){return Fail(ErrorCode::TaskFailed,std::string(operation)+" failed (HRESULT "+std::to_string(std::int32_t(hr))+").");}
bool Extent(ViewExtent e){return e.width&&e.height&&e.width<=D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION&&e.height<=D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;}
bool Same(ViewExtent a,ViewExtent b){return a.width==b.width&&a.height==b.height;}
DirectX::XMFLOAT4 Size(ViewExtent e){return {float(e.width),float(e.height),1.0f/float(e.width),1.0f/float(e.height)};}
ViewExtent SignalExtent(ViewExtent e,float ratio){return {std::max(1u,UINT(std::ceil(e.width*ratio))),std::max(1u,UINT(std::ceil(e.height*ratio))) };}
DXGI_FORMAT Format(TextureIndex i)
{
    if(i==ViewZ||i==DiffuseHit||i==SpecularHit||i==Depth)return DXGI_FORMAT_R32_FLOAT;
    if(i==MotionXY)return DXGI_FORMAT_R16G16_FLOAT;
    if(i==Responsive)return DXGI_FORMAT_R8_UNORM;
    if(i==Resolved||i==Delta)return kRaw;
    return kHdr;
}
bool Texture(const RenderGraphBuilder &g,const RayTransportTexture &r,ViewExtent e,DXGI_FORMAT format)
{
    if(!r.resource||!r.version.resource||r.version.resource.kind!=ResourceKind::Texture||g.Resolve(r.version.resource)!=r.resource)return false;
    const auto d=r.resource->GetDesc();return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.DepthOrArraySize==1&&d.MipLevels==1&&
        d.SampleDesc.Count==1&&d.Width>=e.width&&d.Height>=e.height&&d.Format==format;
}
Result CreateTexture(D3D12MA::Allocator *allocator,ViewExtent e,DXGI_FORMAT format,AllocationResource &r)
{
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=e.width;d.Height=e.height;
    d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Format=format;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12MA::ALLOCATION_DESC a{};a.HeapType=D3D12_HEAP_TYPE_DEFAULT;
    const auto hr=allocator->CreateResource(&a,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,&r.allocation,IID_PPV_ARGS(r.resource.ReleaseAndGetAddressOf()));
    if(FAILED(hr))return Native("Create RR guide/output",hr);r.graph_access=Access::Common;return Result::Success();
}
} // namespace

struct RayReconstructionStorage
{
    struct Signal
    {
        std::array<AllocationResource,TextureCount> textures;
        std::unique_ptr<FsrRayRegenerationContext> native;
        ViewExtent capacity;
        std::uint64_t previous_successful_frame{};
        bool history{};
    };
    struct Slot
    {
        CompletionSet completion;
        std::array<RayShaderBinder,kBanks> binders;
        std::vector<ComPtr<ID3D12Resource>> resources;
        std::vector<std::shared_ptr<const void>> owners;
    };
    ComPtr<ID3D12Device> device;
    ComPtr<D3D12MA::Allocator> allocator;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>,6> pipelines;
    ComPtr<ID3D12DescriptorHeap> descriptors,exports;
    AllocationResource constants,raw_output,pre_exposed;
    std::array<Signal,2> signals;
    std::vector<Slot> slots;
    RayReconstructionProfile profile;
    StreamlineInputExtent input_range;
    StreamlineSession *streamline{};
    FsrSession *fsr{};
    CompletionSet completion;
    std::uint64_t native_bytes{};
    std::byte *mapped{};
    UINT descriptor_stride{};
    bool usable{true},registered{};
    std::string failure;
    ~RayReconstructionStorage(){if(mapped&&constants.resource)constants.resource->Unmap(0,nullptr);}
    std::vector<AllocationResource *> Resources()
    {
        std::vector<AllocationResource *> result{&constants,&raw_output,&pre_exposed};
        for(auto &s:signals)for(auto &r:s.textures)result.push_back(&r);return result;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT slot,UINT bank,UINT offset)const
    {auto h=descriptors->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T((slot*kBanks+bank)*kDescriptors+offset)*descriptor_stride;return h;}
    D3D12_CPU_DESCRIPTOR_HANDLE Export(UINT signal,UINT index)const
    {auto h=exports->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(signal*TextureCount+index)*descriptor_stride;return h;}
    void Srv(D3D12_CPU_DESCRIPTOR_HANDLE h,ID3D12Resource *r,DXGI_FORMAT format)const
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=format;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;device->CreateShaderResourceView(r,&d,h);
    }
    void Uav(D3D12_CPU_DESCRIPTOR_HANDLE h,ID3D12Resource *r,DXGI_FORMAT format)const
    {D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=format;d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(r,nullptr,&d,h);}
};

struct RayReconstructionSystem::Impl
{
    std::shared_ptr<RayReconstructionStorage> storage;
    struct Import {AllocationResource *allocation{};ResourceHandle handle;};
    struct Pending
    {
        const RenderGraphBuilder *graph{};
        std::vector<Import> imports;
        std::array<std::array<ResourceVersion,TextureCount>,2> signals;
        ResourceVersion constants,raw_output,pre_exposed;
        UINT slot{};
        std::uint64_t frame{};
        ViewExtent active;
        bool diffuse{},specular{},guides{},fused{};
    } pending;
    ResourceVersion ImportResource(RenderGraphBuilder &g,AllocationResource &r,std::string_view label)
    {
        for(const auto &i:pending.imports)if(i.allocation==&r)return g.Version(i.handle);
        ResourceHandle h=r.resource->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER?
            ResourceHandle(g.ImportBuffer({r.resource.Get(),r.graph_completion,r.graph_queue},r.graph_access,label)):
            ResourceHandle(g.ImportTexture({r.resource.Get(),r.graph_completion,r.graph_queue},r.graph_access,label));
        pending.imports.push_back({&r,h});return g.Version(h);
    }
    Result Begin(RenderGraphBuilder &g,const RayReconstructionInputs &in)
    {
        auto s=storage;const auto &t=in.transport;
        if(!s||!s->usable||s->profile.mode==RayReconstructionMode::Off)return Fail(ErrorCode::InvalidState,"RR is not selected or has faulted.");
        if(!Extent(t.render_active)||t.render_active.width>s->profile.render_capacity.width||t.render_active.height>s->profile.render_capacity.height||
            t.frame_slot>=s->slots.size()||!s->slots[t.frame_slot].completion.IsSatisfiedBy(t.completed)||!in.make_bindings)
            return Fail(ErrorCode::InvalidArgument,"RR active extent, completed descriptor slot or frame-table builder is invalid.");
        const auto guide=[&](const AmbientOcclusionGuide &r,DXGI_FORMAT f){return Texture(g,{r.version,r.resource,{}},t.render_active,f);};
        if((!guide(t.depth,DXGI_FORMAT_R32_TYPELESS)&&!guide(t.depth,DXGI_FORMAT_D32_FLOAT)&&!guide(t.depth,DXGI_FORMAT_R32_FLOAT))||
            !guide(t.normal,DXGI_FORMAT_R16G16_SNORM)||!guide(t.metadata,DXGI_FORMAT_R32G32_UINT)||
            !guide(t.motion,DXGI_FORMAT_R16G16_FLOAT)||!guide(t.surface_history,DXGI_FORMAT_R32G32_FLOAT)||
            !guide(t.base,DXGI_FORMAT_R8G8B8A8_UNORM)||!guide(t.material,DXGI_FORMAT_R8G8B8A8_UNORM)||
            t.sidecar_plane_count>t.sidecars.size())
            return Fail(ErrorCode::InvalidArgument,"RR requires exact matching raster closure/depth/motion/history graph inputs.");
        for(UINT i=0;i<t.sidecar_plane_count;++i)if(!guide(t.sidecars[i],DXGI_FORMAT_R32G32B32A32_UINT))
            return Fail(ErrorCode::InvalidArgument,"RR material sidecar format or extent is invalid.");
        for(const auto &r:t.reads)if(!r.resource||!r.version.resource||g.Resolve(r.version.resource)!=r.resource)
            return Fail(ErrorCode::InvalidArgument,"RR material/frame/probe resource must be its actual graph producer.");
        if(s->profile.mode==RayReconstructionMode::DlssFused&&!Same(t.render_active,s->input_range.optimal))
            return Fail(ErrorCode::InvalidArgument,"Pinned DLSS RR requires its fixed optimal input extent; dynamic resolution requires drained reinitialization.");
        if(pending.graph)
        {
            if(pending.graph!=&g||pending.slot!=t.frame_slot||pending.frame!=t.identity.render_id||!Same(pending.active,t.render_active))
                return Fail(ErrorCode::InvalidState,"All RR stages must use the same graph, identity and current guide extent.");
            return Result::Success();
        }
        pending={};pending.graph=&g;pending.slot=t.frame_slot;pending.frame=t.identity.render_id;pending.active=t.render_active;
        pending.constants=ImportResource(g,s->constants,"RR.Constants");
        auto &slot=s->slots[t.frame_slot];slot.binders={};slot.resources.clear();slot.owners=in.owners;
        auto retain=[&](ID3D12Resource *r){if(r)slot.resources.emplace_back(r);};
        for(const auto &r:t.reads)retain(r.resource);
        for(const auto &r:{t.depth,t.normal,t.metadata,t.motion,t.surface_history,t.base,t.material,t.fallback_diffuse,t.subsurface_diffuse})retain(r.resource);
        for(UINT i=0;i<t.sidecar_plane_count;++i)retain(t.sidecars[i].resource);
        s->registered=true;return Result::Success();
    }
    void ImportSignal(RenderGraphBuilder &g,UINT signal)
    {
        auto &v=pending.signals[signal];auto &textures=storage->signals[signal].textures;
        for(UINT i=0;i<TextureCount;++i)if(textures[i].resource)v[i]=ImportResource(g,textures[i],"RR.Signal"+std::to_string(signal)+"."+std::to_string(i));
    }
    RayTransportTexture Output(UINT signal,TextureIndex i)const
    {const auto &r=storage->signals[signal].textures[i];return {pending.signals[signal][i],r.resource.Get(),storage->Export(signal,i)};}
    void GuideReads(PassBuilder &p,const RayReconstructionInputs &in,bool history)
    {
        const auto &t=in.transport;
        for(const auto &r:{t.depth,t.normal,t.metadata,t.base,t.material})p.Read(r.version,Access::ComputeRead);
        if(history){p.Read(t.motion.version,Access::ComputeRead);p.Read(t.surface_history.version,Access::ComputeRead);}
        for(UINT i=0;i<t.sidecar_plane_count;++i)p.Read(t.sidecars[i].version,Access::ComputeRead);
        for(const auto &r:t.reads)p.Read(r.version,r.access);
        p.Read(pending.constants,Access::ComputeRead);
    }
    Result Bind(const RayReconstructionInputs &in,RayTransportConstants rt,UINT signal,ViewExtent active,
        const RayNoisySignal *raw,const RayNoisySignal *specular,const RayDlssCompositeInputs *composite,RayShaderBinder &binder)
    {
        auto s=storage;const auto &t=in.transport;const UINT slot=pending.slot;
        auto cpu=[&](UINT i){return s->Cpu(slot,signal,i);};
        // Prefix declarations unused by these entry points still receive typed
        // null descriptors. Only actual closure/guide inputs are populated.
        for(UINT i=0;i<kRayTransportSuffixCount;++i)s->Srv(cpu(i),nullptr,kHdr);
        for(UINT i=16;i<=20;++i)s->Uav(cpu(i),nullptr,kHdr);
        for(UINT i=48;i<=58;++i)s->Uav(cpu(i),nullptr,kHdr);
        for(UINT i=61;i<=65;++i)s->Uav(cpu(i),nullptr,kHdr);
        D3D12_SHADER_RESOURCE_VIEW_DESC as{};as.ViewDimension=D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        as.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;s->device->CreateShaderResourceView(nullptr,&as,cpu(1));
        rt.render_size=Size(t.render_active);rt.gi_size=Size(active);rt.specular_size=Size(active);
        const auto offset=SIZE_T(slot*kBanks+signal)*kBankStride;
        std::memcpy(s->mapped+offset,&rt,sizeof(rt));
        RrConstants constants{t.camera.previous_projection,t.camera.previous_unjittered_view_projection,Size(active),
            {s->profile.mode==RayReconstructionMode::DlssFused?1u:0u,signal,t.reset_history||t.camera.reset_history?1u:0u,in.screen_overlay_alpha.resource?1u:0u}};
        std::memcpy(s->mapped+offset+kRrOffset,&constants,sizeof(constants));
        const auto address=s->constants.resource->GetGPUVirtualAddress()+offset;
        D3D12_CONSTANT_BUFFER_VIEW_DESC cb{address,kRrOffset};s->device->CreateConstantBufferView(&cb,cpu(0));s->device->CreateConstantBufferView(&cb,cpu(21));
        cb={address+kRrOffset,256};s->device->CreateConstantBufferView(&cb,cpu(kOwnOffset));
        s->Srv(cpu(6),t.depth.resource,DXGI_FORMAT_R32_FLOAT);s->Srv(cpu(7),t.normal.resource,DXGI_FORMAT_R16G16_SNORM);
        s->Srv(cpu(8),t.metadata.resource,DXGI_FORMAT_R32G32_UINT);s->Srv(cpu(9),t.motion.resource,DXGI_FORMAT_R16G16_FLOAT);
        s->Srv(cpu(10),t.surface_history.resource,DXGI_FORMAT_R32G32_FLOAT);
        s->Srv(cpu(22),t.base.resource,DXGI_FORMAT_R8G8B8A8_UNORM);s->Srv(cpu(23),t.material.resource,DXGI_FORMAT_R8G8B8A8_UNORM);
        s->Srv(cpu(25),t.fallback_diffuse.resource,kHdr);s->Srv(cpu(27),t.subsurface_diffuse.resource,kHdr);
        for(UINT i=0;i<2;++i)s->Srv(cpu(28+i),i<t.sidecar_plane_count?t.sidecars[i].resource:nullptr,DXGI_FORMAT_R32G32B32A32_UINT);
        auto &textures=s->signals[signal].textures;
        s->Srv(cpu(kOwnOffset+1),raw?raw->color.resource:nullptr,kRaw);
        s->Srv(cpu(kOwnOffset+2),raw?raw->metric_count.resource:nullptr,DXGI_FORMAT_R16G16_FLOAT);
        s->Srv(cpu(kOwnOffset+3),composite?s->raw_output.resource.Get():textures[Clean].resource.Get(),kHdr);
        s->Srv(cpu(kOwnOffset+4),textures[Factors].resource.Get(),kHdr);
        s->Srv(cpu(kOwnOffset+5),specular?specular->metric_count.resource:nullptr,DXGI_FORMAT_R16G16_FLOAT);
        s->Srv(cpu(kOwnOffset+6),composite?composite->exposure.resource:nullptr,composite?composite->exposure.resource->GetDesc().Format:DXGI_FORMAT_R32_FLOAT);
        s->Srv(cpu(kOwnOffset+7),in.screen_overlay_alpha.resource,DXGI_FORMAT_R8_UNORM);
        for(UINT i=0;i<kUavs.size();++i)
        {
            const auto index=kUavs[i];auto *resource=composite&&index==Resolved?s->pre_exposed.resource.Get():textures[index].resource.Get();
            s->Uav(cpu(kOwnOffset+8+i),resource,composite&&index==Resolved?kHdr:Format(index));
        }
        if(auto r=in.make_bindings(cpu(0),cpu(kOwnOffset),binder);!r)return r;
        if(!binder)return Fail(ErrorCode::InvalidState,"RR frame-table builder returned no binder.");
        s->slots[slot].binders[signal]=binder;return Result::Success();
    }
    auto Execute(UINT pipeline,ViewExtent active,RayShaderBinder binder)
    {
        return [s=storage,pipeline,active,binder=std::move(binder)](RenderPassContext &ctx)->Result{
            auto *cmd=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());cmd->SetComputeRootSignature(s->root.Get());cmd->SetPipelineState(s->pipelines[pipeline].Get());
            if(auto r=binder(cmd);!r)return r;const UINT values[]{0,0};cmd->SetComputeRoot32BitConstants(6,2,values,0);
            cmd->Dispatch((active.width+7)/8,(active.height+7)/8,1);return Result::Success();};
    }
    Result RegisterFsr(RenderGraphBuilder &,const RayReconstructionInputs &,const RayNoisySignal &,RayTransportConstants,UINT);
};

RayReconstructionSystem::RayReconstructionSystem():impl_(std::make_unique<Impl>()){}
RayReconstructionSystem::~RayReconstructionSystem()
{
    if(impl_&&impl_->storage&&impl_->storage->registered)
    {
        std::fprintf(stderr,"hs.rr.shutdown error=ResetAfterCompletion was not completed; retaining native and backing owners.\n");
        static_cast<void>(impl_.release());
    }
}
Result RayReconstructionSystem::Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,ID3D12RootSignature *root,
    const RayReconstructionProfile &profile,StreamlineSession *sl,FsrSession *fsr,const RayShaderReader &reader,
    RayReconstructionCandidate &candidate,RendererPipelineCache *cache)const
{
    auto s=std::make_shared<RayReconstructionStorage>();s->profile=profile;s->streamline=sl;s->fsr=fsr;
    if(profile.mode==RayReconstructionMode::Off){candidate.storage_=std::move(s);return Result::Success();}
    if(!device||!allocator||!root||!reader||!Extent(profile.render_capacity)||!Extent(profile.output_extent)||
        !profile.frame_slots||profile.frame_slots>UINT_MAX/(kBanks*kDescriptors)||!profile.byte_budget||(!profile.diffuse&&!profile.specular))
        return Fail(ErrorCode::InvalidArgument,"Selected RR requires actual active signals, capacity, output, frame slots and explicit byte budget.");
    s->device=device;s->allocator=allocator;s->root=root;s->slots.resize(profile.frame_slots);
    if(profile.mode==RayReconstructionMode::DlssFused)
    {
        if(!sl||!profile.diffuse||!profile.specular)return Fail(ErrorCode::InvalidArgument,"Fused DLSS RR requires both actual primary signals and the pre-device Streamline session.");
        if(sl->Availability(StreamlineFeature::RayReconstruction)!=ReconstructionAvailability::Available)
            return Fail(ErrorCode::InvalidState,std::string(sl->FailureReason(StreamlineFeature::RayReconstruction)));
        if(auto r=sl->QueryInputExtent(profile.reconstruction_quality,profile.output_extent,true,s->input_range);!r)return r;
        if(s->input_range.optimal.width>profile.render_capacity.width||s->input_range.optimal.height>profile.render_capacity.height)
            return Fail(ErrorCode::InvalidArgument,"DLSS RR optimal extent exceeds prepared guide capacity.");
    }
    else if(!fsr)return Fail(ErrorCode::InvalidArgument,"FSR RR requires the shared initialized FSR device/DLL session.");
    auto create=[&](UINT signal,TextureIndex index,ViewExtent extent)->Result{
        const auto format=Format(index);D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
        if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)))||
            !(support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_LOAD)||!(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
            return Fail(ErrorCode::TaskFailed,"RR native conversion/output format is unsupported by the device.");
        return CreateTexture(allocator,extent,format,s->signals[signal].textures[index]);};
    if(profile.mode==RayReconstructionMode::FsrStandalone)
    {
        std::vector<FsrVariant> variants;if(auto r=fsr->QueryVariants(FsrEffect::RayRegeneration,variants);!r)return r;
        if(variants.empty())return Fail(ErrorCode::InvalidState,"Pinned FSR DLL reports no native RR provider for this device.");
        for(UINT signal=0;signal<2;++signal)
        {
            if(signal?!profile.specular:!profile.diffuse)continue;
            auto &v=s->signals[signal];v.capacity=SignalExtent(profile.render_capacity,signal?profile.specular_axis_ratio:profile.diffuse_axis_ratio);
            if(auto r=fsr->CreateRayRegenerationContext(v.capacity,signal?FsrRaySignal::Specular:FsrRaySignal::Diffuse,v.native,profile.fsr_variant);!r)return r;
            for(auto i:{Radiance,Clean,Factors,ViewZ,Motion,Normal,DiffuseAlbedo,SpecularAlbedo})if(auto r=create(signal,i,v.capacity);!r)return r;
            if(auto r=create(signal,Resolved,profile.render_capacity);!r)return r;
            if(signal==0)for(auto i:{Delta,Subsurface})if(auto r=create(signal,i,profile.render_capacity);!r)return r;
        }
    }
    else
    {
        s->signals[0].capacity=profile.render_capacity;
        for(auto i:{Normal,DiffuseAlbedo,SpecularAlbedo,DiffuseHit,SpecularHit,Depth,MotionXY,Responsive})if(auto r=create(0,i,profile.render_capacity);!r)return r;
        if(auto r=create(0,ViewZ,{1,1});!r)return r;
        if(auto r=CreateTexture(allocator,profile.output_extent,kHdr,s->raw_output);!r)return r;
        if(auto r=CreateTexture(allocator,profile.output_extent,kHdr,s->pre_exposed);!r)return r;
    }
    for(UINT i=0;i<std::size(kProducts);++i)
    {
        if(profile.mode==RayReconstructionMode::DlssFused?i<4:i>=4||((i<2)?!profile.diffuse:!profile.specular))continue;
        std::vector<std::byte> code;if(auto r=reader(kProducts[i],code);!r)return r;
        if(code.empty())return Fail(ErrorCode::TaskFailed,"RR shader product is empty: "+std::string(kProducts[i]));
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};d.pRootSignature=root;d.CS={code.data(),code.size()};
        const auto hr=cache?cache->CreateCompute(&d,IID_PPV_ARGS(&s->pipelines[i])):device->CreateComputePipelineState(&d,IID_PPV_ARGS(&s->pipelines[i]));
        if(FAILED(hr))return Native("Create selected RR PSO",hr);
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=profile.frame_slots*kBanks*kDescriptors;
    auto hr=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&s->descriptors));if(FAILED(hr))return Native("Create RR CPU descriptor leases",hr);
    s->descriptor_stride=device->GetDescriptorHandleIncrementSize(heap.Type);heap.NumDescriptors=2*TextureCount+2;
    hr=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&s->exports));if(FAILED(hr))return Native("Create RR export descriptors",hr);
    for(UINT signal=0;signal<2;++signal)for(UINT i=0;i<TextureCount;++i)s->Srv(s->Export(signal,i),s->signals[signal].textures[i].resource.Get(),Format(static_cast<TextureIndex>(i)));
    s->Srv(s->Export(2,0),s->raw_output.resource.Get(),kHdr);s->Srv(s->Export(2,1),s->pre_exposed.resource.Get(),kHdr);
    D3D12_RESOURCE_DESC upload{};upload.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;upload.Width=UINT64(profile.frame_slots)*kBanks*kBankStride;
    upload.Height=1;upload.DepthOrArraySize=1;upload.MipLevels=1;upload.SampleDesc.Count=1;upload.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12MA::ALLOCATION_DESC a{};a.HeapType=D3D12_HEAP_TYPE_UPLOAD;
    hr=allocator->CreateResource(&a,&upload,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,&s->constants.allocation,IID_PPV_ARGS(s->constants.resource.ReleaseAndGetAddressOf()));
    if(FAILED(hr))return Native("Create RR constant leases",hr);s->constants.graph_access=Access::ComputeRead;
    void *mapped{};D3D12_RANGE empty{0,0};hr=s->constants.resource->Map(0,&empty,&mapped);if(FAILED(hr))return Native("Map RR constants",hr);s->mapped=static_cast<std::byte *>(mapped);
    std::uint64_t bytes{};for(auto *r:s->Resources())if(r->allocation)bytes+=r->allocation->GetSize();
    for(auto &signal:s->signals)if(signal.native)bytes+=signal.native->AllocatedBytes();
    if(bytes>profile.byte_budget)return Fail(ErrorCode::TaskFailed,"Actual RR native pools and guide allocations exceed the selected byte budget.");
    candidate.storage_=std::move(s);return Result::Success();
}
Result RayReconstructionSystem::ValidateNativeBudgetAfterConfiguration(RayReconstructionCandidate &candidate,
    const std::function<Result(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *)> &submit_and_wait) const
{
    auto s=candidate.storage_;
    if(!s||s->profile.mode!=RayReconstructionMode::DlssFused)return Result::Success();
    if(!submit_and_wait)return Fail(ErrorCode::InvalidArgument,"Native RR allocation requires the renderer's submitted and waited Direct queue.");
    ComPtr<ID3D12CommandAllocator> allocator;
    auto hr=s->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator));
    if(FAILED(hr))return Native("Create DLSS RR allocation command allocator",hr);
    ComPtr<ID3D12GraphicsCommandList> commands;
    hr=s->device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands));
    if(FAILED(hr))return Native("Create DLSS RR allocation command list",hr);
    // The explicit SDK allocation reads only these real resource descriptions.
    // It does not evaluate pixels or create a rendered frame/history sample.
    SdkReconstructionFrame metadata;
    metadata.color=s->signals[0].textures[DiffuseAlbedo].resource.Get();
    metadata.depth=s->signals[0].textures[Depth].resource.Get();
    metadata.motion=s->signals[0].textures[MotionXY].resource.Get();
    metadata.output=s->raw_output.resource.Get();
    metadata.active={0,0,s->input_range.optimal.width,s->input_range.optimal.height};
    metadata.output_rect={0,0,s->profile.output_extent.width,s->profile.output_extent.height};
    metadata.color_state=metadata.depth_state=metadata.motion_state=metadata.output_state=D3D12_RESOURCE_STATE_COMMON;
    bool created{};
    const auto allocated=s->streamline->AllocateRayReconstructionResources(commands.Get(),metadata,created);
    hr=commands->Close();if(FAILED(hr))return Native("Close DLSS RR allocation command list",hr);
    // NGX creation may record initialization even when it reports a failure.
    // Drain that work before clearing tags, querying memory or rolling back.
    if(auto r=submit_and_wait(commands.Get(),allocator.Get());!r)return r;
    if(auto r=s->streamline->CompleteRayReconstructionAllocationAfterCompletion();!r)return r;
    if(!allocated)return allocated;
    std::uint64_t native{};
    if(auto r=s->streamline->QueryRayReconstructionMemory(native);!r)return r;
    // Every matching live viewport is already included in DXGI CurrentUsage.
    // Count native bytes only when this candidate actually creates the viewport.
    std::uint64_t bytes=created?native:0;
    for(auto *resource:s->Resources())if(resource->allocation)
    {
        const auto count=resource->allocation->GetSize();
        if(count>UINT64_MAX-bytes)return Fail(ErrorCode::TaskFailed,"RR native and guide memory accounting overflow.");
        bytes+=count;
    }
    if(bytes>s->profile.byte_budget)return Fail(ErrorCode::TaskFailed,"Native DLSS RR estimated resident memory plus actual guide pools exceed the observed candidate byte budget.");
    s->native_bytes=native;return Result::Success();
}
Result RayReconstructionSystem::ValidateNativeBudgetAfterConfiguration(
    const std::function<Result(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *)> &submit_and_wait) const
{
    RayReconstructionCandidate candidate;candidate.storage_=impl_->storage;
    return ValidateNativeBudgetAfterConfiguration(candidate,submit_and_wait);
}
Result RayReconstructionSystem::Commit(RayReconstructionCandidate &&candidate,const CompletionSet &completed)
{
    if(!candidate.storage_||impl_->pending.graph)return Fail(ErrorCode::InvalidState,"RR candidate is absent or a graph remains pending.");
    if(auto r=ResetAfterCompletion(completed);!r)return r;
    impl_->storage=std::move(candidate.storage_);return Result::Success();
}
std::uint64_t RayReconstructionSystem::AllocatedBytes()const noexcept
{
    std::uint64_t bytes{};if(auto s=impl_->storage){bytes=s->native_bytes;for(auto *r:s->Resources())if(r->allocation)bytes+=r->allocation->GetSize();
        for(const auto &signal:s->signals)if(signal.native)bytes+=signal.native->AllocatedBytes();}return bytes;
}
AllocationResource RayReconstructionSystem::RetainPreExposedOutput() const
{
    AllocationResource retained;
    const auto storage = impl_->storage;
    if (!storage || !storage->pre_exposed.resource || !storage->pre_exposed.allocation)
        return retained;
    retained.resource = storage->pre_exposed.resource;
    retained.allocation = storage->pre_exposed.allocation;
    retained.allocation->AddRef();
    retained.graph_access = storage->pre_exposed.graph_access;
    retained.graph_queue = storage->pre_exposed.graph_queue;
    retained.graph_completion = storage->pre_exposed.graph_completion;
    return retained;
}
RayReconstructionMode RayReconstructionSystem::Mode()const noexcept{return impl_->storage?impl_->storage->profile.mode:RayReconstructionMode::Off;}
ReconstructionAvailability RayReconstructionSystem::Availability()const noexcept
{
    auto s=impl_->storage;if(!s||s->profile.mode==RayReconstructionMode::Off)return ReconstructionAvailability::Unavailable;
    if(!s->usable)return ReconstructionAvailability::Unavailable;
    if(s->profile.mode==RayReconstructionMode::DlssFused)return s->streamline->Availability(StreamlineFeature::RayReconstruction);
    for(const auto &v:s->signals)if(v.native&&v.native->Availability()!=ReconstructionAvailability::Available)return v.native->Availability();
    return ReconstructionAvailability::Available;
}
std::string_view RayReconstructionSystem::FailureReason()const noexcept
{
    auto s=impl_->storage;if(!s||s->profile.mode==RayReconstructionMode::Off)return "Ray reconstruction is Off.";
    if(!s->failure.empty())return s->failure;
    if(s->profile.mode==RayReconstructionMode::DlssFused)return s->streamline->FailureReason(StreamlineFeature::RayReconstruction);
    for(const auto &v:s->signals)if(v.native&&v.native->Availability()!=ReconstructionAvailability::Available)return v.native->FailureReason();return {};
}
Result RayReconstructionSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if(impl_->pending.graph)return Fail(ErrorCode::InvalidState,"Complete RR's submitted or abandoned graph before reset.");
    auto s=impl_->storage;if(!s)return Result::Success();
    if(!s->completion.IsSatisfiedBy(completed))return Fail(ErrorCode::InvalidState,"RR reset requires every Direct/Compute/Copy consumer completed.");
    for(auto &signal:s->signals)if(signal.native)if(auto r=signal.native->ReleaseAfterCompletion(true);!r)return r;
    impl_->storage.reset();return Result::Success();
}
Result RayReconstructionSystem::Impl::RegisterFsr(RenderGraphBuilder &g,const RayReconstructionInputs &in,
    const RayNoisySignal &raw,RayTransportConstants constants,UINT signal)
{
    auto s=storage;
    if(!s||s->profile.mode!=RayReconstructionMode::FsrStandalone||!s->signals[signal].native)
        return Fail(ErrorCode::InvalidState,"FSR RR registration requires the selected actual signal context.");
    if(auto r=Begin(g,in);!r)return r;
    if(signal?pending.specular:pending.diffuse)return Fail(ErrorCode::InvalidState,"Register each actual RR signal only once per graph.");
    const auto active=SignalExtent(in.transport.render_active,signal?s->profile.specular_axis_ratio:s->profile.diffuse_axis_ratio);
    if(raw.specular!=(signal!=0)||!Same(raw.active,active)||!raw.owner||
        !Texture(g,raw.color,active,kRaw)||!Texture(g,raw.metric_count,active,DXGI_FORMAT_R16G16_FLOAT))
        return Fail(ErrorCode::InvalidArgument,"FSR RR requires the true unfiltered signal and raw metric/count at its matching trace extent.");
    auto &slot=s->slots[pending.slot];slot.owners.push_back(raw.owner);
    ImportSignal(g,signal);auto &v=pending.signals[signal];RayShaderBinder binder;
    if(auto r=Bind(in,constants,signal,active,&raw,nullptr,nullptr,binder);!r)return r;
    auto pack=g.AddPass(signal?253u:250u,signal?"FSR RR Specular Pack":"FSR RR Diffuse Pack",QueueHint::Direct);
    GuideReads(pack,in,true);pack.Read(raw.color.version,Access::ComputeRead);pack.Read(raw.metric_count.version,Access::ComputeRead);
    for(auto i:{Radiance,Factors,ViewZ,Motion,Normal,DiffuseAlbedo,SpecularAlbedo})v[i]=pack.Write(v[i],Access::UnorderedWrite);
    pack.SetExecuteChecked(Execute(signal?2u:0u,active,binder));
    SdkRayReconstructionFrame frame;frame.reconstruction.active={0,0,active.width,active.height};
    frame.reconstruction.camera=in.transport.camera;frame.reconstruction.identity=in.transport.identity;
    frame.reconstruction.camera.jitter_pixels[0]*=float(active.width)/float(pending.active.width);
    frame.reconstruction.camera.jitter_pixels[1]*=float(active.height)/float(pending.active.height);
    frame.reconstruction.camera.reset_history|=in.transport.reset_history||!s->signals[signal].history||
        pending.frame!=s->signals[signal].previous_successful_frame+1;
    auto &textures=s->signals[signal].textures;
    frame.signed_view_z=textures[ViewZ].resource.Get();frame.packed_motion=textures[Motion].resource.Get();
    frame.normal_roughness=textures[Normal].resource.Get();frame.diffuse_albedo=textures[DiffuseAlbedo].resource.Get();
    frame.specular_albedo=textures[SpecularAlbedo].resource.Get();
    if(signal){frame.specular_radiance=textures[Radiance].resource.Get();frame.specular_output=textures[Clean].resource.Get();}
    else{frame.diffuse_radiance=textures[Radiance].resource.Get();frame.diffuse_output=textures[Clean].resource.Get();}
    auto native=g.AddPass(signal?254u:251u,signal?"FSR RR Specular Native":"FSR RR Diffuse Native",QueueHint::Direct);
    for(auto i:{Radiance,ViewZ,Motion,Normal,DiffuseAlbedo,SpecularAlbedo})native.Read(v[i],Access::ComputeRead);
    v[Clean]=native.Write(v[Clean],Access::UnorderedWrite);
    std::vector<ExternalResourceAccess> native_accesses{
        {v[Radiance].resource,Access::ComputeRead,Access::ComputeRead},
        {v[ViewZ].resource,Access::ComputeRead,Access::ComputeRead},
        {v[Motion].resource,Access::ComputeRead,Access::ComputeRead},
        {v[Normal].resource,Access::ComputeRead,Access::ComputeRead},
        {v[DiffuseAlbedo].resource,Access::ComputeRead,Access::ComputeRead},
        {v[SpecularAlbedo].resource,Access::ComputeRead,Access::ComputeRead},
        {v[Clean].resource,Access::UnorderedWrite,Access::UnorderedWrite},
    };
    native.SetExecuteOnOwner([s,signal,frame,native_accesses=std::move(native_accesses)](RenderPassContext &ctx)->Result{
        return ctx.RecordLegacyResourceCommands(native_accesses,[s,signal,frame,
            commands=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList())]() mutable -> Result {
            auto r=s->fsr->ExecuteRayRegeneration(*s->signals[signal].native,commands,frame);
            if(!r){s->usable=false;s->failure=r.Message();}return r;
        });
    });
    auto resolve=g.AddPass(signal?255u:252u,signal?"FSR RR Specular Remodulate":"FSR RR Diffuse Remodulate",QueueHint::Direct);
    GuideReads(resolve,in,false);resolve.Read(v[Clean],Access::ComputeRead);resolve.Read(v[Factors],Access::ComputeRead);
    v[Resolved]=resolve.Write(v[Resolved],Access::UnorderedWrite);
    if(!signal)
    {
        resolve.Read(in.transport.fallback_diffuse.version,Access::ComputeRead);resolve.Read(in.transport.subsurface_diffuse.version,Access::ComputeRead);
        v[Delta]=resolve.Write(v[Delta],Access::UnorderedWrite);v[Subsurface]=resolve.Write(v[Subsurface],Access::UnorderedWrite);
    }
    resolve.SetExecuteChecked(Execute(signal?3u:1u,pending.active,binder));
    for(auto version:v)if(version.resource)g.SetFinalAccess(version,Access::ComputeRead);
    if(signal)pending.specular=true;else pending.diffuse=true;return Result::Success();
}
Result RayReconstructionSystem::RegisterDiffuse(RenderGraphBuilder &g,const RayReconstructionInputs &in,
    const RayNoisySignal &raw,RayTransportConstants constants,RayDiffuseOutputs &out)
{
    out={};if(auto r=impl_->RegisterFsr(g,in,raw,constants,0);!r)return r;
    out={impl_->Output(0,Resolved),impl_->Output(0,Delta),impl_->Output(0,Subsurface),raw.color,raw.statistics};return Result::Success();
}
Result RayReconstructionSystem::RegisterSpecular(RenderGraphBuilder &g,const RayReconstructionInputs &in,
    const RayNoisySignal &raw,RayTransportConstants constants,RaySpecularOutputs &out)
{
    out={};if(auto r=impl_->RegisterFsr(g,in,raw,constants,1);!r)return r;
    out={impl_->Output(1,Resolved),raw.color,raw.statistics};return Result::Success();
}
Result RayReconstructionSystem::RegisterDlssGuides(RenderGraphBuilder &g,const RayReconstructionInputs &in,
    const RayNoisySignal &diffuse,const RayNoisySignal &specular,RayTransportConstants constants,RayDlssGuides &out)
{
    out={};auto s=impl_->storage;
    if(!s||s->profile.mode!=RayReconstructionMode::DlssFused)return Fail(ErrorCode::InvalidState,"DLSS guide registration requires selected fused RR.");
    if(auto r=impl_->Begin(g,in);!r)return r;auto &p=impl_->pending;
    if(p.guides)return Fail(ErrorCode::InvalidState,"Register DLSS RR guides once per real render frame.");
    for(const auto *raw:{&diffuse,&specular})
        if(!Same(raw->active,p.active)||!raw->owner||!Texture(g,raw->color,p.active,kRaw)||!Texture(g,raw->metric_count,p.active,DXGI_FORMAT_R16G16_FLOAT))
            return Fail(ErrorCode::InvalidArgument,"DLSS RR requires both true noisy signals at the full fixed render extent.");
    if(diffuse.specular||!specular.specular)return Fail(ErrorCode::InvalidArgument,"DLSS RR diffuse and specular producer identities are reversed.");
    if(in.screen_overlay_alpha.resource&&!Texture(g,in.screen_overlay_alpha,p.active,DXGI_FORMAT_R8_UNORM))
        return Fail(ErrorCode::InvalidArgument,"DLSS RR responsive input requires its actual raster-domain R8 producer.");
    if(!in.exposure.resource||!in.exposure.version.resource||g.Resolve(in.exposure.version.resource)!=in.exposure.resource||
        in.exposure.resource->GetDesc().Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||in.exposure.resource->GetDesc().Width<sizeof(float))
        return Fail(ErrorCode::InvalidArgument,"DLSS RR guides require the actual current scalar exposure buffer.");
    auto &slot=s->slots[p.slot];slot.owners.push_back(diffuse.owner);slot.owners.push_back(specular.owner);
    impl_->ImportSignal(g,0);auto &v=p.signals[0];RayShaderBinder binder;
    if(auto r=impl_->Bind(in,constants,0,p.active,&diffuse,&specular,nullptr,binder);!r)return r;
    auto pack=g.AddPass(256,"DLSS RR Opaque Guides",QueueHint::Direct);impl_->GuideReads(pack,in,true);
    pack.Read(diffuse.metric_count.version,Access::ComputeRead);pack.Read(specular.metric_count.version,Access::ComputeRead);
    pack.Read(in.exposure.version,Access::ComputeRead);slot.resources.emplace_back(in.exposure.resource);
    if(in.screen_overlay_alpha.resource){pack.Read(in.screen_overlay_alpha.version,Access::ComputeRead);slot.resources.emplace_back(in.screen_overlay_alpha.resource);}
    for(auto i:{DiffuseAlbedo,SpecularAlbedo,Normal,DiffuseHit,SpecularHit,Depth,MotionXY,ViewZ,Responsive})v[i]=pack.Write(v[i],Access::UnorderedWrite);
    pack.SetExecuteChecked(impl_->Execute(4,p.active,binder));
    for(auto version:v)if(version.resource)g.SetFinalAccess(version,Access::ComputeRead);
    out={impl_->Output(0,DiffuseAlbedo),impl_->Output(0,SpecularAlbedo),impl_->Output(0,Normal),impl_->Output(0,DiffuseHit),
        impl_->Output(0,SpecularHit),impl_->Output(0,Depth),impl_->Output(0,MotionXY),impl_->Output(0,ViewZ),p.active,in.transport.identity,s};
    out.responsive=impl_->Output(0,Responsive);
    p.guides=true;return Result::Success();
}
Result RayReconstructionSystem::RegisterDlssFused(RenderGraphBuilder &g,const RayReconstructionInputs &in,
    const RayDlssGuides &guides,const RayDlssCompositeInputs &composite,RayTransportConstants constants,RayDlssOutputs &out)
{
    out={};auto s=impl_->storage;if(!s||s->profile.mode!=RayReconstructionMode::DlssFused)return Fail(ErrorCode::InvalidState,"Fused DLSS RR is not selected.");
    if(auto r=impl_->Begin(g,in);!r)return r;auto &p=impl_->pending;
    if(!p.guides||p.fused||guides.owner.get()!=s.get()||guides.identity.render_id!=p.frame||!Same(guides.active,p.active))
        return Fail(ErrorCode::InvalidState,"DLSS RR requires this graph's exact matched opaque guides once.");
    if(!Texture(g,composite.noisy_color,p.active,kHdr)||!Texture(g,composite.exposure,{1,1},DXGI_FORMAT_R32_FLOAT)||
        !Texture(g,guides.responsive,p.active,DXGI_FORMAT_R8_UNORM))
        return Fail(ErrorCode::InvalidArgument,"DLSS RR requires real integrated noisy scene-linear RGBA16 and actual R32 exposure.");
    auto &slot=s->slots[p.slot];slot.owners.insert(slot.owners.end(),composite.owners.begin(),composite.owners.end());
    for(const auto r:{composite.noisy_color,composite.exposure,composite.responsive,composite.transparency_overlay,composite.color_before_transparency,composite.sss_luminance})
        if(r.resource){if(!r.version.resource||g.Resolve(r.version.resource)!=r.resource)return Fail(ErrorCode::InvalidArgument,"DLSS optional tag must be its real graph producer.");slot.resources.emplace_back(r.resource);}
    p.raw_output=impl_->ImportResource(g,s->raw_output,"DLSS RR.RawDisplayHDR");p.pre_exposed=impl_->ImportResource(g,s->pre_exposed,"DLSS RR.PreExposedDisplayHDR");
    SdkRayReconstructionFrame frame;auto &base=frame.reconstruction;
    base.color=composite.noisy_color.resource;base.depth=guides.depth.resource;base.motion=guides.motion.resource;
    base.exposure=composite.exposure.resource;base.responsive=guides.responsive.resource;base.output=s->raw_output.resource.Get();
    base.active={0,0,p.active.width,p.active.height};base.output_rect={0,0,s->profile.output_extent.width,s->profile.output_extent.height};
    base.camera=in.transport.camera;base.identity=in.transport.identity;base.input_pre_exposure=1.0f;base.exposure_scale=1.0f;
    base.camera.reset_history|=in.transport.reset_history;
    base.transparency_overlay=composite.transparency_overlay.resource;base.color_before_transparency=composite.color_before_transparency.resource;
    base.sss_luminance=composite.sss_luminance.resource;
    frame.diffuse_albedo=guides.diffuse_albedo.resource;frame.specular_albedo=guides.specular_albedo.resource;frame.normal_roughness=guides.normal_roughness.resource;
    frame.diffuse_hit_distance=guides.diffuse_hit_distance.resource;frame.specular_hit_distance=guides.specular_hit_distance.resource;
    auto native=g.AddPass(257,"DLSS RR Fused Native",QueueHint::Direct);
    for(const auto r:{composite.noisy_color,composite.exposure,composite.responsive,composite.transparency_overlay,composite.color_before_transparency,composite.sss_luminance,
        guides.diffuse_albedo,guides.specular_albedo,guides.normal_roughness,guides.diffuse_hit_distance,guides.specular_hit_distance,guides.depth,guides.motion,guides.responsive})
        if(r.version.resource)native.Read(r.version,Access::ComputeRead);
    p.raw_output=native.Write(p.raw_output,Access::UnorderedWrite);
    std::vector<ExternalResourceAccess> native_accesses{
        {composite.noisy_color.version.resource,Access::ComputeRead,Access::ComputeRead},
        {guides.depth.version.resource,Access::ComputeRead,Access::ComputeRead},
        {guides.motion.version.resource,Access::ComputeRead,Access::ComputeRead},
        {composite.exposure.version.resource,Access::ComputeRead,Access::ComputeRead},
        {guides.responsive.version.resource,Access::ComputeRead,Access::ComputeRead},
        {guides.diffuse_albedo.version.resource,Access::ComputeRead,Access::ComputeRead},
        {guides.specular_albedo.version.resource,Access::ComputeRead,Access::ComputeRead},
        {guides.normal_roughness.version.resource,Access::ComputeRead,Access::ComputeRead},
        {guides.diffuse_hit_distance.version.resource,Access::ComputeRead,Access::ComputeRead},
        {guides.specular_hit_distance.version.resource,Access::ComputeRead,Access::ComputeRead},
    };
    for(const auto r:{composite.transparency_overlay,composite.color_before_transparency,composite.sss_luminance})
        if(r.version.resource)native_accesses.push_back({r.version.resource,Access::ComputeRead,Access::ComputeRead});
    native_accesses.push_back({p.raw_output.resource,Access::UnorderedWrite,Access::UnorderedWrite});
    native.SetExecuteOnOwner([s,frame,native_accesses=std::move(native_accesses)](RenderPassContext &ctx)->Result{
        return ctx.RecordLegacyResourceCommands(native_accesses,[s,frame,
            commands=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList())]() mutable -> Result {
            auto r=s->streamline->ExecuteRayReconstruction(commands,frame);
            if(!r){s->usable=false;s->failure=r.Message();}return r;
        });
    });
    RayShaderBinder binder;if(auto r=impl_->Bind(in,constants,1,s->profile.output_extent,nullptr,nullptr,&composite,binder);!r)return r;
    auto exposure=g.AddPass(51,"DLSS RR PreExposure",QueueHint::Direct);exposure.Read(p.constants,Access::ComputeRead);
    exposure.Read(p.raw_output,Access::ComputeRead);exposure.Read(composite.exposure.version,Access::ComputeRead);
    p.pre_exposed=exposure.Write(p.pre_exposed,Access::UnorderedWrite);exposure.SetExecuteChecked(impl_->Execute(5,s->profile.output_extent,binder));
    g.SetFinalAccess(p.raw_output,Access::ComputeRead);g.SetFinalAccess(p.pre_exposed,Access::ComputeRead);
    out={{p.raw_output,s->raw_output.resource.Get(),s->Export(2,0)},{p.pre_exposed,s->pre_exposed.resource.Get(),s->Export(2,1)},guides,s};
    p.fused=true;return Result::Success();
}
Result RayReconstructionSystem::CompleteSubmission(const RenderGraphBuilder &g,const CompletionSet &completion,bool success)
{
    auto s=impl_->storage;auto &p=impl_->pending;if(!p.graph)return Result::Success();
    if(p.graph!=&g)return Fail(ErrorCode::InvalidArgument,"RR completion refers to another graph.");
    s->completion.Merge(completion);auto &slot=s->slots[p.slot];slot.completion.Merge(completion);
    for(const auto &i:p.imports)
    {
        const auto use=g.ResourceCompletion(i.handle);s->completion.Merge(use);slot.completion.Merge(use);
        i.allocation->graph_completion.Merge(completion);i.allocation->graph_completion.Merge(use);
        if(success){i.allocation->graph_access=g.FinalAccess(i.handle);i.allocation->graph_queue=g.FinalQueue(i.handle);}
    }
    for(UINT signal=0;signal<2;++signal)if(signal?p.specular:p.diffuse)
    {s->signals[signal].history=success;s->signals[signal].previous_successful_frame=p.frame;}
    if(!success){s->usable=false;s->failure="RR graph failed or was partially submitted; drain and prepare fresh native history.";}
    p={};return Result::Success();
}
} // namespace hs
