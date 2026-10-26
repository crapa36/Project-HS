#include "renderer_adaptive_shading.hpp"
#include "d3d12_resources.hpp"
#include <D3D12MemAlloc.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr UINT kSrvCount=8,kDescriptorCount=10;
Result Failure(ErrorCode code,std::string text){return Result::Failure(code,"hs_renderer_adaptive_shading",std::move(text));}
Result Native(std::string_view operation,HRESULT hr)
{return Failure(ErrorCode::TaskFailed,std::string(operation)+" failed (HRESULT "+std::to_string(std::int32_t(hr))+").");}
bool Extent(ViewExtent e){return e.width&&e.height&&e.width<=D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION&&e.height<=D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;}
ViewExtent Tiles(ViewExtent e,UINT tile){return {e.width/tile+UINT(e.width%tile!=0),e.height/tile+UINT(e.height%tile!=0)};}
D3D12_RESOURCE_DESC Buffer(UINT64 bytes)
{D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;
    d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;return d;}
bool Guide(const RenderGraphBuilder &graph,AmbientOcclusionGuide g,ViewExtent e,DXGI_FORMAT format)
{
    if(!g.resource||!g.version.resource||g.version.resource.kind!=ResourceKind::Texture||graph.Resolve(g.version.resource)!=g.resource)return false;
    const auto d=g.resource->GetDesc();return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.DepthOrArraySize==1&&d.MipLevels==1&&
        d.SampleDesc.Count==1&&d.Width>=e.width&&d.Height>=e.height&&d.Format==format;
}
}
struct AdaptiveShadingStorage
{
    struct Frame
    {
        AllocationResource upload,density,rate;std::byte *mapped{};CompletionSet completion;
        std::vector<std::shared_ptr<const void>> owners;
        Frame()=default;Frame(const Frame &)=delete;Frame &operator=(const Frame &)=delete;
        Frame(Frame &&o)noexcept:upload(std::move(o.upload)),density(std::move(o.density)),rate(std::move(o.rate)),
            mapped(std::exchange(o.mapped,nullptr)),completion(o.completion),owners(std::move(o.owners)){}
        ~Frame(){if(mapped&&upload.resource)upload.resource->Unmap(0,nullptr);}
    };
    ComPtr<ID3D12Device> device;ComPtr<D3D12MA::Allocator> allocator;ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>,2> pipelines;ComPtr<ID3D12DescriptorHeap> descriptors;
    AdaptiveShadingProfile profile;RendererFeatureHardware hardware;UINT stride{};UINT64 bytes{};
    std::vector<Frame> frames;CompletionSet completion;bool usable{true};
    std::uint32_t last_successful_slot{};bool has_successful_slot{};
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT slot,UINT index)const
    {auto h=descriptors->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(slot*kDescriptorCount+index)*stride;return h;}
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT slot,UINT index)const
    {auto h=descriptors->GetGPUDescriptorHandleForHeapStart();h.ptr+=UINT64(slot*kDescriptorCount+index)*stride;return h;}
    void Srv(UINT slot,UINT index,ID3D12Resource *resource,DXGI_FORMAT format)
    {D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=format;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;device->CreateShaderResourceView(resource,&d,Cpu(slot,index));}
    void Uav(UINT slot,UINT index,ID3D12Resource *resource)
    {D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=DXGI_FORMAT_R8_UINT;d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(resource,nullptr,&d,Cpu(slot,kSrvCount+index));}
};
namespace
{
Result Allocate(AdaptiveShadingStorage &s,AllocationResource &out,D3D12_RESOURCE_DESC d,bool upload=false)
{
    D3D12MA::ALLOCATION_DESC a{};a.HeapType=upload?D3D12_HEAP_TYPE_UPLOAD:D3D12_HEAP_TYPE_DEFAULT;
    const auto hr=s.allocator->CreateResource(&a,&d,upload?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COMMON,
        nullptr,&out.allocation,IID_PPV_ARGS(out.resource.ReleaseAndGetAddressOf()));
    if(FAILED(hr))return Native("Create adaptive shading resource",hr);
    s.bytes+=out.allocation->GetSize();out.graph_access=upload?Access::AllShaderRead:Access::Common;
    if(s.bytes>s.profile.byte_budget)return Failure(ErrorCode::InvalidArgument,"Adaptive shading native allocation budget exceeded.");
    return Result::Success();
}
Result Image(AdaptiveShadingStorage &s,AllocationResource &out,ViewExtent extent)
{D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=extent.width;d.Height=extent.height;
    d.DepthOrArraySize=1;d.MipLevels=1;d.Format=DXGI_FORMAT_R8_UINT;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return Allocate(s,out,d);}
}
struct AdaptiveShadingSystem::Impl
{
    struct Import{AllocationResource *allocation;ResourceHandle handle;};
    std::shared_ptr<AdaptiveShadingStorage> storage;AdaptiveShadingStatus status;
    struct Pending{const RenderGraphBuilder *graph{};UINT slot{};std::vector<Import> imports;bool raster_recorded{};}pending;
};
AdaptiveShadingRasterScope::AdaptiveShadingRasterScope()=default;
AdaptiveShadingRasterScope::~AdaptiveShadingRasterScope()
{
    if(!commands_)return;
    const D3D12_SHADING_RATE_COMBINER reset[2]{D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
    commands_->RSSetShadingRateImage(nullptr);commands_->RSSetShadingRate(D3D12_SHADING_RATE_1X1,reset);commands_->Release();
}
AdaptiveShadingSystem::AdaptiveShadingSystem():impl_(std::make_unique<Impl>()){}
AdaptiveShadingSystem::~AdaptiveShadingSystem()=default;
std::uint64_t AdaptiveShadingCandidate::AllocatedBytes()const noexcept{return storage_?storage_->bytes:0;}
std::uint64_t AdaptiveShadingSystem::AllocatedBytes()const noexcept{return impl_->storage?impl_->storage->bytes:0;}
AdaptiveShadingStatus AdaptiveShadingSystem::Status()const{return impl_->status;}
std::array<const AllocationResource *, 2> AdaptiveShadingSystem::DiagnosticResources() const noexcept
{
    if (!impl_->storage || !impl_->storage->has_successful_slot ||
        (!impl_->storage->profile.raster_enabled && !impl_->storage->profile.compute_enabled)) return {};
    const auto &frame = impl_->storage->frames[impl_->storage->last_successful_slot];
    return {&frame.density, &frame.rate};
}
Result AdaptiveShadingSystem::Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,const AdaptiveShadingProfile &profile,
    const ShaderReader &reader,AdaptiveShadingCandidate &candidate)const
{
    candidate={};
    if(!profile.raster_enabled&&!profile.compute_enabled){candidate.prepared_=true;return Result::Success();}
    if(!device||!allocator||!Extent(profile.render_capacity)||!profile.frame_slots||profile.frame_slots>16||
        !profile.maximum_materials||profile.maximum_materials>4096||!profile.byte_budget||!reader)
    {candidate.prepared_=false;return Failure(ErrorCode::InvalidArgument,"Invalid adaptive shading native profile.");}
    const auto hardware=QueryRendererFeatureHardware(device);auto &status=candidate.status_;
    status.vrs_tier=hardware.vrs_tier;status.vrs_tile_size=hardware.vrs_tile_size;
    bool raster=profile.raster_enabled&&hardware.vrs_tier>=2&&hardware.vrs_tile_size&&hardware.compute_shader60;
    const bool compute=profile.compute_enabled&&hardware.compute_shader60;
    if(!raster&&profile.raster_enabled)status.reason=hardware.vrs_tier==1?
        "Tier1 mixed-material fullscreen DeferredLight stays1x1.":"Native Tier2 VRS image/compute prerequisites unavailable.";
    if(raster)
    {
        // Admit the actual command-list ABI before creating an optional pool.
        // This probe records/submits no GPU work and is absent when requestedOff.
        ComPtr<ID3D12CommandAllocator> commands;ComPtr<ID3D12GraphicsCommandList5> list;
        const auto allocator_result=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&commands));
        const auto list_result=SUCCEEDED(allocator_result)?device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,
            commands.Get(),nullptr,IID_PPV_ARGS(&list)):allocator_result;
        if(FAILED(list_result)){raster=false;status.reason="Actual CommandList5 VRS interface unavailable; deferred shading stays1x1.";}
        else if(const auto closed=list->Close();FAILED(closed))return Native("Close VRS interface probe",closed);
    }
    if(!compute&&profile.compute_enabled&&status.reason.empty())status.reason="Adaptive compute requires actual shader model6.0 support.";
    if(!raster&&!compute){candidate.prepared_=true;return Result::Success();}
    D3D12_FEATURE_DATA_FORMAT_SUPPORT format{DXGI_FORMAT_R8_UINT};
    const auto checked=device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&format,sizeof(format));
    if(FAILED(checked))return Native("Check adaptive R8_UINT support",checked);
    if(!(format.Support1&D3D12_FORMAT_SUPPORT1_TEXTURE2D)||!(format.Support1&D3D12_FORMAT_SUPPORT1_SHADER_LOAD)||
        !(format.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
    {status.reason="R8_UINT typed rate/density writes unavailable.";candidate.prepared_=true;return Result::Success();}
    auto s=std::make_shared<AdaptiveShadingStorage>();s->device=device;s->allocator=allocator;s->profile=profile;s->hardware=hardware;
    s->profile.raster_enabled=raster;s->profile.compute_enabled=compute;s->frames.resize(profile.frame_slots);
    for(auto &frame:s->frames)
    {
        if(auto r=Allocate(*s,frame.upload,Buffer(256+UINT64(profile.maximum_materials)*8),true);!r)return r;
        void *mapped{};D3D12_RANGE empty{};auto hr=frame.upload.resource->Map(0,&empty,&mapped);
        if(FAILED(hr))return Native("Map adaptive material policy",hr);frame.mapped=static_cast<std::byte *>(mapped);
        if(auto r=Image(*s,frame.density,Tiles(profile.render_capacity,kAdaptiveComputeTileSize));!r)return r;
        if(raster)if(auto r=Image(*s,frame.rate,Tiles(profile.render_capacity,hardware.vrs_tile_size));!r)return r;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,kSrvCount,0,33,0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,33,0}};
    D3D12_ROOT_PARAMETER parameters[3]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[0].Descriptor={0,33};
    parameters[1].ParameterType=parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable={1,&ranges[0]};parameters[2].DescriptorTable={1,&ranges[1]};
    D3D12_ROOT_SIGNATURE_DESC root{};root.NumParameters=3;root.pParameters=parameters;
    ComPtr<ID3DBlob> serialized,diagnostics;auto hr=D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&diagnostics);
    if(FAILED(hr))return Native("Serialize adaptive root",hr);
    hr=device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&s->root));
    if(FAILED(hr))return Native("Create adaptive root",hr);
    const char *products[]{"render/shader/adaptive_shading_classify_cs.dxil","render/shader/adaptive_shading_rate_cs.dxil"};
    for(UINT i=0;i<(raster?2u:1u);++i)
    {
        std::vector<std::byte> bytes;if(auto r=reader(products[i],bytes);!r)return r;
        if(bytes.empty())return Failure(ErrorCode::InvalidArgument,"Selected adaptive shader is empty.");
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};d.pRootSignature=s->root.Get();d.CS={bytes.data(),bytes.size()};
        hr=device->CreateComputePipelineState(&d,IID_PPV_ARGS(&s->pipelines[i]));if(FAILED(hr))return Native(products[i],hr);
    }
    s->stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    s->bytes+=UINT64(profile.frame_slots)*kDescriptorCount*s->stride;
    if(s->bytes>profile.byte_budget)return Failure(ErrorCode::InvalidArgument,"Adaptive descriptors exceed native byte budget.");
    D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors=profile.frame_slots*kDescriptorCount;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&s->descriptors));if(FAILED(hr))return Native("Create adaptive descriptors",hr);
    for(UINT slot=0;slot<profile.frame_slots;++slot)
    {
        auto &f=s->frames[slot];s->Srv(slot,6,f.density.resource.Get(),DXGI_FORMAT_R8_UINT);
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.FirstElement=256/8;d.Buffer.NumElements=profile.maximum_materials;d.Buffer.StructureByteStride=8;
        device->CreateShaderResourceView(f.upload.resource.Get(),&d,s->Cpu(slot,7));
        s->Uav(slot,0,f.density.resource.Get());s->Uav(slot,1,f.rate.resource.Get());
    }
    status.raster_ready=raster;status.compute_ready=compute;status.allocated_bytes=s->bytes;candidate.storage_=std::move(s);candidate.prepared_=true;return Result::Success();
}
Result AdaptiveShadingSystem::Commit(AdaptiveShadingCandidate &&candidate,const CompletionSet &completed)
{
    if(!candidate.prepared_||impl_->pending.graph||(impl_->storage&&!impl_->storage->completion.IsSatisfiedBy(completed)))
        return Failure(ErrorCode::InvalidState,"Adaptive replacement needs prepared storage and completed old readers.");
    impl_->storage=std::move(candidate.storage_);impl_->status=std::move(candidate.status_);candidate.prepared_=false;return Result::Success();
}
Result AdaptiveShadingSystem::Register(RenderGraphBuilder &graph,const AdaptiveShadingInputs &in,
    AdaptiveShadingOutputs &out,AdaptiveShadingLayout *layout)
{
    return RegisterFrame(graph,in,out,layout,nullptr);
}
bool AdaptiveShadingSystem::MatchesLayout(const AdaptiveShadingInputs &in,const AdaptiveShadingLayout &layout)const noexcept
{
    const auto &s=impl_->storage;
    return layout.valid&&layout.active==bool(s)&&(!s||
        (layout.raster==s->profile.raster_enabled&&layout.compute==s->profile.compute_enabled&&
         layout.capacity.width==s->profile.render_capacity.width&&layout.capacity.height==s->profile.render_capacity.height&&
         layout.native_tile_size==s->hardware.vrs_tile_size&&
         layout.guides==std::array{in.depth.version,in.normal.version,in.base.version,
             in.material.version,in.metadata.version,in.motion.version}));
}
Result AdaptiveShadingSystem::BindRetained(RenderGraphBuilder &graph,const AdaptiveShadingInputs &in,
    const AdaptiveShadingLayout &layout,AdaptiveShadingOutputs &out)
{
    return RegisterFrame(graph,in,out,nullptr,&layout);
}
Result AdaptiveShadingSystem::RegisterFrame(RenderGraphBuilder &graph,const AdaptiveShadingInputs &in,
    AdaptiveShadingOutputs &out,AdaptiveShadingLayout *capture,const AdaptiveShadingLayout *retained)
{
    out={};if(capture)*capture={};auto s=impl_->storage;
    if(retained&&!MatchesLayout(in,*retained))
        return Failure(ErrorCode::InvalidState,"Adaptive retained topology has changed.");
    if(capture){capture->active=bool(s);if(s){capture->raster=s->profile.raster_enabled;capture->compute=s->profile.compute_enabled;
        capture->capacity=s->profile.render_capacity;capture->native_tile_size=s->hardware.vrs_tile_size;}}
    if(!s){if(capture)capture->valid=true;return Result::Success();}
    if(!s->usable||impl_->pending.graph)return Failure(ErrorCode::InvalidState,"Adaptive graph failed or remains pending.");
    if(!Extent(in.render_active)||in.render_active.width>s->profile.render_capacity.width||in.render_active.height>s->profile.render_capacity.height||
        in.frame_slot>=s->frames.size()||!s->frames[in.frame_slot].completion.IsSatisfiedBy(in.completed)||in.material_policy.size()>s->profile.maximum_materials)
        return Failure(ErrorCode::InvalidArgument,"Invalid adaptive active extent, frame fence or material capacity.");
    if(!(Guide(graph,in.depth,in.render_active,DXGI_FORMAT_R32_TYPELESS)||Guide(graph,in.depth,in.render_active,DXGI_FORMAT_R32_FLOAT)||
        Guide(graph,in.depth,in.render_active,DXGI_FORMAT_D32_FLOAT))||!Guide(graph,in.normal,in.render_active,DXGI_FORMAT_R16G16_SNORM)||
        !Guide(graph,in.base,in.render_active,DXGI_FORMAT_R8G8B8A8_UNORM)||!Guide(graph,in.material,in.render_active,DXGI_FORMAT_R8G8B8A8_UNORM)||
        !Guide(graph,in.metadata,in.render_active,DXGI_FORMAT_R32G32_UINT)||!Guide(graph,in.motion,in.render_active,DXGI_FORMAT_R16G16_FLOAT)||
        !std::isfinite(in.depth_parameters.x)||!std::isfinite(in.depth_parameters.y)||!std::isfinite(in.depth_parameters.z)||!std::isfinite(in.depth_parameters.w))
        return Failure(ErrorCode::InvalidArgument,"Adaptive actual depth/normal/material/motion guides are invalid.");
    UINT previous{};for(const auto &policy:in.material_policy)
    {if(!policy.material_id||policy.material_id<=previous||policy.eligible>1)return Failure(ErrorCode::InvalidArgument,"Adaptive material policy must contain sorted unique actual IDs.");previous=policy.material_id;}
    auto &frame=s->frames[in.frame_slot];std::memset(frame.mapped,0,std::size_t(frame.upload.resource->GetDesc().Width));
    const AdaptiveShadingConstants constants{{in.render_active.width,in.render_active.height,kAdaptiveComputeTileSize,s->hardware.vrs_tile_size},
        in.depth_parameters,{UINT(in.material_policy.size()),s->profile.raster_enabled?1u:0u,s->profile.compute_enabled?1u:0u,in.force_full_rate?1u:0u}};
    std::memcpy(frame.mapped,&constants,sizeof(constants));if(!in.material_policy.empty())
        std::memcpy(frame.mapped+256,in.material_policy.data(),in.material_policy.size_bytes());
    const AmbientOcclusionGuide guides[]{in.depth,in.normal,in.base,in.material,in.metadata,in.motion};
    for(std::size_t i=0;i<6;++i)
    {
        if(capture)capture->guides[i]=guides[i].version;
    }
    const DXGI_FORMAT formats[]{DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16_SNORM,DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R32G32_UINT,DXGI_FORMAT_R16G16_FLOAT};
    for(UINT i=0;i<6;++i)s->Srv(in.frame_slot,i,guides[i].resource,formats[i]);
    Impl::Pending pending;pending.graph=&graph;pending.slot=in.frame_slot;
    std::size_t import_index{};bool bound=true;Result binding_result=Result::Success();
    auto import=[&](AllocationResource &r,std::string_view name)
    {
        if(auto checked=r.EnsureNativeContract(s->device.Get());!checked){binding_result=checked;return ResourceVersion{};}
        const auto h=retained?retained->textures[import_index]:
            graph.ImportTexture({r.resource.Get(),r.graph_completion,r.graph_queue,{},r.NativeContract()},r.graph_access,name);
        if(retained)bound&=graph.BindTexture(h,{r.resource.Get(),r.graph_completion,r.graph_queue,{},r.NativeContract()},r.graph_access);
        if(capture)capture->textures[import_index]=h;
        ++import_index;pending.imports.push_back({&r,h});return graph.Version(h);
    };
    auto density=import(frame.density,"Adaptive.ComputeDensity");
    if(!binding_result)return binding_result;
    if(!bound)return Failure(ErrorCode::InvalidState,"Adaptive retained density binding failed.");
    if(capture)capture->passes[0]=graph.PassCount();
    auto classifyBinding=retained?graph.BindPass(retained->passes[0],280):
        std::optional<PassBuilder>{graph.AddPass(280,"Adaptive.ClassifyActualGuides",QueueHint::Direct)};
    if(!classifyBinding)return Failure(ErrorCode::InvalidState,"Adaptive retained classify pass is missing.");
    auto &classify=*classifyBinding;
    if(retained)density=retained->written[0];
    else{for(const auto guide:guides)classify.Read(guide.version,Access::ComputeRead);density=classify.Write(density,Access::UnorderedWrite);}
    if(capture)capture->written[0]=density;
    auto execute=[s,slot=in.frame_slot](UINT pipeline,ViewExtent groups)
    {return [s,slot,pipeline,groups](RenderPassContext &context)
        {auto *cmd=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());ID3D12DescriptorHeap *heaps[]{s->descriptors.Get()};
            cmd->SetDescriptorHeaps(1,heaps);cmd->SetComputeRootSignature(s->root.Get());cmd->SetPipelineState(s->pipelines[pipeline].Get());
            cmd->SetComputeRootConstantBufferView(0,s->frames[slot].upload.resource->GetGPUVirtualAddress());
            cmd->SetComputeRootDescriptorTable(1,s->Gpu(slot,0));cmd->SetComputeRootDescriptorTable(2,s->Gpu(slot,kSrvCount));cmd->Dispatch(groups.width,groups.height,1);};};
    classify.SetExecute(execute(0,Tiles(s->profile.render_capacity,kAdaptiveComputeTileSize)));
    out.density={density,frame.density.resource.Get()};out.compute_ready=s->profile.compute_enabled;
    if(s->profile.raster_enabled)
    {
        auto rate=import(frame.rate,"Adaptive.NativeRateImage");
        if(!binding_result)return binding_result;
        if(!bound)return Failure(ErrorCode::InvalidState,"Adaptive retained VRS binding failed.");
        if(capture)capture->passes[1]=graph.PassCount();
        auto reduceBinding=retained?graph.BindPass(retained->passes[1],281):
            std::optional<PassBuilder>{graph.AddPass(281,"Adaptive.ReduceNativeVrsTiles",QueueHint::Direct)};
        if(!reduceBinding)return Failure(ErrorCode::InvalidState,"Adaptive retained reduce pass is missing.");
        auto &reduce=*reduceBinding;
        if(retained)rate=retained->written[1];
        else{reduce.Read(density,Access::ComputeRead);rate=reduce.Write(rate,Access::UnorderedWrite);}
        if(capture)capture->written[1]=rate;
        reduce.SetExecute(execute(1,Tiles(Tiles(s->profile.render_capacity,s->hardware.vrs_tile_size),8)));
        out.rate={rate,frame.rate.resource.Get()};out.raster_ready=true;
    }
    frame.owners=in.owners;impl_->pending=std::move(pending);
    if(capture)capture->valid=true;return Result::Success();
}
Result AdaptiveShadingSystem::BeginRaster(RenderPassContext &context,const AdaptiveShadingOutputs &out,AdaptiveShadingRasterScope &scope)
{
    if(!out.raster_ready)return Result::Success();
    const auto s=impl_->storage;const auto &pending=impl_->pending;
    if(!s||!pending.graph||scope.commands_||out.rate.resource!=s->frames[pending.slot].rate.resource.Get())
        return Failure(ErrorCode::InvalidState,"Adaptive raster scope does not match its registered native rate image.");
    auto *cmd=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());const auto hr=cmd->QueryInterface(IID_PPV_ARGS(&scope.commands_));
    if(FAILED(hr))return Native("Query actual CommandList5 for VRS",hr);
    const D3D12_SHADING_RATE_COMBINER combiners[2]{D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,D3D12_SHADING_RATE_COMBINER_OVERRIDE};
    scope.commands_->RSSetShadingRate(D3D12_SHADING_RATE_1X1,combiners);scope.commands_->RSSetShadingRateImage(out.rate.resource);
    impl_->pending.raster_recorded=true;return Result::Success();
}
Result AdaptiveShadingSystem::CompleteSubmission(const RenderGraphBuilder &graph,const CompletionSet &completion,bool success)
{
    auto &p=impl_->pending;if(!p.graph)return Result::Success();
    if(p.graph!=&graph)return Failure(ErrorCode::InvalidArgument,"Wrong adaptive completion graph.");
    auto s=impl_->storage;auto &frame=s->frames[p.slot];s->completion.Merge(completion);frame.completion.Merge(completion);
    for(auto &i:p.imports)
    {const auto lease=graph.ResourceCompletion(i.handle);s->completion.Merge(lease);frame.completion.Merge(lease);
        i.allocation->graph_completion.Merge(completion);i.allocation->graph_completion.Merge(lease);
        if(success){i.allocation->graph_access=graph.FinalAccess(i.handle);i.allocation->graph_queue=graph.FinalQueue(i.handle);}}
    impl_->status.raster_applied=success&&p.raster_recorded;
    if (success)
    {
        s->last_successful_slot = p.slot;
        s->has_successful_slot = true;
    }
    if(!success){s->usable=false;impl_->status.usable=false;impl_->status.reason="Adaptive partial submission requires completed retirement and fresh preparation.";}
    p={};return Result::Success();
}
Result AdaptiveShadingSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if(impl_->pending.graph||(impl_->storage&&!impl_->storage->completion.IsSatisfiedBy(completed)))
        return Failure(ErrorCode::InvalidState,"Adaptive reset requires all native image readers completed.");
    impl_->storage.reset();impl_->status={};return Result::Success();
}
} // namespace hs
