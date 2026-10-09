#include "renderer_ray_ser.hpp"
#include "renderer_feature_resolve.hpp"
#include "d3d12_resources.hpp"
#include <D3D12MemAlloc.h>
#include <wrl/client.h>
#include <cstring>
#include <iterator>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
Result Fail(ErrorCode code,std::string message){return Result::Failure(code,"hs_ray_ser",std::move(message));}
constexpr const wchar_t *kExports[]{L"GITraceRayGen",L"HybridReflectionTraceRayGen"};
constexpr const char *kProducts[]{"render/shader/rtgi_trace_ser_lib.dxil","render/shader/hybrid_reflection_trace_ser_lib.dxil"};
constexpr const char *kOmmProducts[]{"render/shader/rtgi_trace_omm_ser_lib.dxil","render/shader/hybrid_reflection_trace_omm_ser_lib.dxil"};
struct Program
{
    std::vector<std::byte> code;
    ComPtr<ID3D12StateObject> state;
    ComPtr<ID3D12StateObjectProperties> properties;
    AllocationResource table;
};
}
struct RaySerStorage
{
    ComPtr<ID3D12Device5> device;ComPtr<D3D12MA::Allocator> allocator;ComPtr<ID3D12RootSignature> root;
    struct Slot{CompletionSet completion;std::array<RayShaderBinder,2> binders;};
    std::array<Program,2> programs;std::vector<Slot> slots;CompletionSet completion;
    RaySerStatus status;
    RayShaderReader selected_reader; // retains the selected Generation reader lease
    std::uint64_t AllocatedBytes()const
    {std::uint64_t bytes{};for(const auto &p:programs)if(p.table.allocation)bytes+=p.table.allocation->GetSize();return bytes;}
};
struct RayShaderExecutionReorderingSystem::Impl
{
    std::shared_ptr<RaySerStorage> storage;
    struct Pending
    {
        const RenderGraphBuilder *graph{};std::uint32_t slot{};
        std::array<ResourceHandle,2> tables{};std::array<bool,2> signals{};
        std::array<RayShaderBinder,2> binders;
        std::array<std::shared_ptr<bool>,2> dispatched;
    } pending;
};
RayShaderExecutionReorderingSystem::RayShaderExecutionReorderingSystem():impl_(std::make_unique<Impl>()){}
RayShaderExecutionReorderingSystem::~RayShaderExecutionReorderingSystem()=default;
std::uint64_t RaySerCandidate::AllocatedBytes()const noexcept{return storage_?storage_->AllocatedBytes():0;}
std::uint64_t RayShaderExecutionReorderingSystem::AllocatedBytes()const noexcept{return impl_->storage?impl_->storage->AllocatedBytes():0;}
RaySerStatus RayShaderExecutionReorderingSystem::Status()const
{auto status=impl_->storage?impl_->storage->status:RaySerStatus{};status.allocated_bytes=AllocatedBytes();return status;}
Result RayShaderExecutionReorderingSystem::Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,ID3D12RootSignature *root,
    const RaySerProfile &profile,const RayShaderReader &reader,RaySerCandidate &candidate)const
{
    candidate.storage_.reset();if(!profile.enabled)return Result::Success();
    if(!device||!allocator||!root||!reader||!profile.frame_slots||profile.frame_slots>16||
       (!profile.active_gi&&!profile.active_hybrid)||!profile.native_byte_budget||profile.old_service_bytes>profile.native_byte_budget)
        return Fail(ErrorCode::InvalidArgument,"SER requires actual transport consumers, selected libraries, root, slots and native budget.");
    const auto hardware=QueryRendererFeatureHardware(device);
    if(!hardware.shader_model69||!hardware.ser_actually_reorders||!hardware.inline_ray_queries||
       (profile.use_omms&&!hardware.opacity_micromap_arrays))
        return Fail(ErrorCode::InvalidState,"SER requires actual OPTIONS22 reordering, SM6.9 and compatible native ray hardware.");
    auto s=std::make_shared<RaySerStorage>();s->allocator=allocator;s->root=root;s->slots.resize(profile.frame_slots);s->selected_reader=reader;
    if(FAILED(device->QueryInterface(IID_PPV_ARGS(s->device.ReleaseAndGetAddressOf()))))return Fail(ErrorCode::TaskFailed,"SER Device5 is unavailable.");
    for(UINT i=0;i<2;++i)
    {
        if(i==0?!profile.active_gi:!profile.active_hybrid)continue;
        auto &p=s->programs[i];const auto product=profile.use_omms?kOmmProducts[i]:kProducts[i];
        if(auto result=reader(product,p.code);!result)return result;
        if(p.code.empty())return Fail(ErrorCode::InvalidState,"Selected SER library is empty: "+std::string(product));
        D3D12_EXPORT_DESC export_desc{kExports[i],nullptr,D3D12_EXPORT_FLAG_NONE};
        D3D12_DXIL_LIBRARY_DESC library{{p.code.data(),p.code.size()},1,&export_desc};
        D3D12_GLOBAL_ROOT_SIGNATURE global{root};D3D12_RAYTRACING_SHADER_CONFIG shader{0,8};
        D3D12_RAYTRACING_PIPELINE_CONFIG1 pipeline{1,profile.use_omms?
            D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_OPACITY_MICROMAPS:D3D12_RAYTRACING_PIPELINE_FLAG_NONE};
        const D3D12_STATE_SUBOBJECT subobjects[]{
            {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&library},
            {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,&global},
            {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,&shader},
            {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1,&pipeline}};
        D3D12_STATE_OBJECT_DESC object{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,UINT(std::size(subobjects)),subobjects};
        if(FAILED(s->device->CreateStateObject(&object,IID_PPV_ARGS(p.state.ReleaseAndGetAddressOf())))||
           FAILED(p.state.As(&p.properties)))return Fail(ErrorCode::TaskFailed,"Cannot create selected native SER state object.");
        const void *identifier=p.properties->GetShaderIdentifier(kExports[i]);
        if(!identifier)return Fail(ErrorCode::InvalidState,"Selected native state object has no actual raygen shader identifier.");
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
        d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12MA::ALLOCATION_DESC allocation{};allocation.HeapType=D3D12_HEAP_TYPE_UPLOAD;
        if(FAILED(allocator->CreateResource(&allocation,&d,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,&p.table.allocation,
            IID_PPV_ARGS(p.table.resource.ReleaseAndGetAddressOf()))))return Fail(ErrorCode::TaskFailed,"Cannot allocate immutable SER raygen shader table.");
        if(s->AllocatedBytes()>profile.native_byte_budget-profile.old_service_bytes)return Fail(ErrorCode::InvalidState,"Actual SER old/candidate table backing exceeds native budget.");
        p.table.graph_access=Access::RayTracingRead;
        if(p.table.resource->GetGPUVirtualAddress()%D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT)
            return Fail(ErrorCode::InvalidState,"SER shader-table address is not64-byte aligned.");
        void *mapped{};D3D12_RANGE empty{0,0};if(FAILED(p.table.resource->Map(0,&empty,&mapped)))return Fail(ErrorCode::TaskFailed,"Cannot map native SER shader table.");
        std::memset(mapped,0,D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
        std::memcpy(mapped,identifier,D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);p.table.resource->Unmap(0,nullptr);
    }
    s->status.enabled=s->status.ready=true;s->status.use_omms=profile.use_omms;
    s->status.reason="Actual native state objects and immutable raygen tables are ready; awaiting DispatchRays submission.";
    candidate.storage_=std::move(s);return Result::Success();
}
Result RayShaderExecutionReorderingSystem::Commit(RaySerCandidate &&candidate,const CompletionSet &completed)
{
    if(impl_->pending.graph||(impl_->storage&&(!impl_->storage->status.usable||!impl_->storage->completion.IsSatisfiedBy(completed))))
        return Fail(ErrorCode::InvalidState,"SER replacement requires completed usable storage and no pending graph.");
    impl_->storage=std::move(candidate.storage_);return Result::Success();
}
Result RayShaderExecutionReorderingSystem::RegisterDispatch(RenderGraphBuilder &graph,PassBuilder &pass,RayNativeSignal signal,
    ViewExtent extent,std::uint32_t slot,const CompletionSet &completed,RayShaderBinder binder,
    std::function<Result(RenderPassContext &)> &execute)
{
    execute={};auto s=impl_->storage;const UINT index=UINT(signal);
    if(!s||!s->status.usable||!s->status.ready||index>=2||!s->programs[index].state||!binder||
       slot>=s->slots.size()||!s->slots[slot].completion.IsSatisfiedBy(completed)||!extent.width||!extent.height||
       std::uint64_t(extent.width)*extent.height>D3D12_RAYTRACING_MAX_RAY_GENERATION_SHADER_THREADS)
        return Fail(ErrorCode::InvalidArgument,"SER requires ready actual program, completed slot, binder and bounded DispatchRays extent.");
    auto &p=impl_->pending;
    if(p.graph&&(p.graph!=&graph||p.slot!=slot||p.signals[index]))return Fail(ErrorCode::InvalidState,"SER signals must share one pending graph and register once.");
    if(!p.graph)s->slots[slot].binders={};
    auto &program=s->programs[index];auto h=graph.ImportBuffer({program.table.resource.Get(),program.table.graph_completion,program.table.graph_queue},
        program.table.graph_access,index?"SERSpecularShaderTable":"SERDiffuseShaderTable");
    auto version=graph.Version(h);pass.Read(version,Access::RayTracingRead);
    auto dispatched=std::make_shared<bool>(false);
    p.graph=&graph;p.slot=slot;p.signals[index]=true;p.tables[index]=h;p.binders[index]=binder;p.dispatched[index]=dispatched;s->slots[slot].binders[index]=binder;
    execute=[s,binder=std::move(binder),index,extent,version,dispatched](RenderPassContext &ctx)->Result
    {
        auto *native=static_cast<ID3D12GraphicsCommandList*>(ctx.NativeCommandList());ComPtr<ID3D12GraphicsCommandList4> cmd;
        const auto &program=s->programs[index];
        if(!native||ctx.Resolve(version.resource)!=program.table.resource.Get()||native->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT||
           FAILED(native->QueryInterface(IID_PPV_ARGS(cmd.ReleaseAndGetAddressOf()))))return Fail(ErrorCode::InvalidState,"SER requires actual Direct DXR command list and matching shader table.");
        cmd->SetComputeRootSignature(s->root.Get());if(auto result=binder(native);!result)return result;
        const UINT constants[]{0,0};cmd->SetComputeRoot32BitConstants(6,2,constants,0);
        cmd->SetPipelineState1(program.state.Get());D3D12_DISPATCH_RAYS_DESC dispatch{};
        dispatch.RayGenerationShaderRecord={program.table.resource->GetGPUVirtualAddress(),D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES};
        dispatch.Width=extent.width;dispatch.Height=extent.height;dispatch.Depth=1;
        cmd->DispatchRays(&dispatch);*dispatched=true;return Result::Success();
    };return Result::Success();
}
Result RayShaderExecutionReorderingSystem::CompleteSubmission(const RenderGraphBuilder &graph,const CompletionSet &completion,bool success)
{
    auto s=impl_->storage;auto &p=impl_->pending;if(!p.graph)return Result::Success();
    if(!s||p.graph!=&graph)return Fail(ErrorCode::InvalidArgument,"SER completion has no matching graph.");
    s->completion.Merge(completion);s->slots[p.slot].completion.Merge(completion);
    for(UINT i=0;i<2;++i)if(p.signals[i])
    {auto &table=s->programs[i].table;const auto required=graph.ResourceCompletion(p.tables[i]);table.graph_completion.Merge(required);table.graph_completion.Merge(completion);
        s->completion.Merge(required);s->slots[p.slot].completion.Merge(required);
        if(success){table.graph_access=graph.FinalAccess(p.tables[i]);table.graph_queue=graph.FinalQueue(p.tables[i]);}}
    bool actually_dispatched=false;for(UINT i=0;i<2;++i)if(p.signals[i]&&p.dispatched[i]&&*p.dispatched[i])actually_dispatched=true;
    if(success){s->status.applied=actually_dispatched&&completion.direct!=0;
        if(s->status.applied)s->status.reason="Actual native DispatchRays submitted successfully with selected SER raygeneration libraries.";}
    else{s->status.usable=s->status.ready=s->status.applied=false;
        s->status.reason="Native SER submission failed; retire all queues before reset.";}
    // Binder/descriptor leases survive submission through waited slot retirement.
    // Storage and its own slot retain the concrete binder until actual retirement.
    p={};return Result::Success();
}
Result RayShaderExecutionReorderingSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if(impl_->pending.graph||(impl_->storage&&!impl_->storage->completion.IsSatisfiedBy(completed)))return Fail(ErrorCode::InvalidState,"SER reset requires every actual queue lease retired.");
    impl_->storage.reset();return Result::Success();
}
}
