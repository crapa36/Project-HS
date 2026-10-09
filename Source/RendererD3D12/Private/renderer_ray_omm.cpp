#include "renderer_ray_omm.hpp"
#include "renderer_feature_resolve.hpp"
#include "d3d12_resources.hpp"
#include <D3D12MemAlloc.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <exception>
#include <map>
#include <set>
#include <tuple>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
using Digest=render_data::OpacityMicromapDigest;
using Key=std::pair<std::uint64_t,Digest>;
Result Fail(ErrorCode code,std::string message){return Result::Failure(code,"hs_ray_omm",std::move(message));}
std::uint64_t Bytes(const AllocationResource &r){return r.allocation?r.allocation->GetSize():0;}
std::uint64_t Align(std::uint64_t n,std::uint64_t a){return n&&n<=UINT64_MAX-(a-1)?(n+a-1)&~(a-1):0;}
bool Same(const render_data::OpacityMicromapIdentity &a,const render_data::OpacityMicromapIdentity &b)
{return a.geometry_asset.value==b.geometry_asset.value&&a.triangle_count==b.triangle_count&&
    a.geometry_digest==b.geometry_digest&&a.topology_digest==b.topology_digest&&
    a.material_digest==b.material_digest&&a.coverage_key==b.coverage_key;}
bool MatchWords(const render_data::OpacityMicromapPackageView &view,std::span<const std::uint32_t> words)
{
    if(words.size()!=view.opacity_word_bytes.size()/4)return false;
    for(std::size_t i=0;i<words.size();++i)
    {std::uint32_t actual{};for(unsigned j=0;j<4;++j)actual|=std::uint32_t(std::to_integer<unsigned>(view.opacity_word_bytes[i*4+j]))<<(j*8);
        if(actual!=words[i])return false;}return true;
}
Result Buffer(D3D12MA::Allocator *allocator,std::uint64_t size,D3D12_HEAP_TYPE heap,
    D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state,bool enhanced_barriers,Access access,AllocationResource &r)
{
    if(!size)return Fail(ErrorCode::InvalidArgument,"Invalid native OMM allocation size.");
    D3D12MA::ALLOCATION_DESC a{};a.HeapType=heap;
    if(enhanced_barriers)
    {
        D3D12_RESOURCE_DESC1 d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=size;d.Height=1;
        d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=flags;
        const auto hr=allocator->CreateResource3(&a,&d,D3D12_BARRIER_LAYOUT_UNDEFINED,nullptr,0,nullptr,
            &r.allocation,IID_PPV_ARGS(r.resource.ReleaseAndGetAddressOf()));
        if(FAILED(hr))return Fail(ErrorCode::TaskFailed,"Cannot allocate enhanced native OMM backing buffer.");
    }
    else
    {
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=size;d.Height=1;
        d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=flags;
        const auto hr=allocator->CreateResource(&a,&d,state,nullptr,&r.allocation,IID_PPV_ARGS(r.resource.ReleaseAndGetAddressOf()));
        if(FAILED(hr))return Fail(ErrorCode::TaskFailed,"Cannot allocate native OMM backing buffer.");
    }
    r.graph_access=access;return Result::Success();
}
struct Entry
{
    AllocationResource upload,input,array,scratch;
    std::vector<std::byte> package;
    render_data::OpacityMicromapPackageView view;
    std::vector<D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY> histogram;
    std::uint64_t descriptor_offset{},index_offset{},input_bytes{},generation{};
    std::shared_ptr<const void> generation_owner;
    CompletionSet completion;
    bool built{};
    std::uint64_t AllocatedBytes()const{return Bytes(upload)+Bytes(input)+Bytes(array)+Bytes(scratch);}
    D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC ArrayDesc()const
    {return {UINT(histogram.size()),histogram.data(),input.resource->GetGPUVirtualAddress(),
        {input.resource->GetGPUVirtualAddress()+descriptor_offset,8}};}
};
struct Import {AllocationResource *allocation{};ResourceHandle handle;};
struct Job {std::shared_ptr<Entry> entry;ResourceVersion upload,input,array,scratch;bool build{};};
}
struct RayOmmStorage
{
    ComPtr<ID3D12Device5> device;ComPtr<D3D12MA::Allocator> allocator;
    RayOmmProductReader reader;RayOmmProfile profile;RayOmmStatus status;
    std::map<Key,std::shared_ptr<Entry>> entries;
    std::vector<std::shared_ptr<Entry>> retired;
    std::vector<CompletionSet> slots;CompletionSet completion;
    std::uint64_t AllocatedBytes()const
    {std::uint64_t bytes{};std::set<const Entry*> seen;
        for(const auto &[key,e]:entries)if(seen.insert(e.get()).second)bytes+=e->AllocatedBytes();
        for(const auto &e:retired)if(seen.insert(e.get()).second)bytes+=e->AllocatedBytes();return bytes;}
};
struct RayOpacityMicromapSystem::Impl
{
    std::shared_ptr<RayOmmStorage> storage;
    struct Pending
    {
        const RenderGraphBuilder *graph{};std::uint32_t slot{};
        std::map<Key,std::shared_ptr<Entry>> entries;std::vector<Job> jobs;std::vector<Import> imports;
        RayOmmStatus status;std::uint64_t additional_bytes{};
    } pending;
};
RayOpacityMicromapSystem::RayOpacityMicromapSystem():impl_(std::make_unique<Impl>()){}
RayOpacityMicromapSystem::~RayOpacityMicromapSystem()=default;
std::uint64_t RayOmmCandidate::AllocatedBytes()const noexcept{return storage_?storage_->AllocatedBytes():0;}
std::uint64_t RayOpacityMicromapSystem::AllocatedBytes()const noexcept
{return (impl_->storage?impl_->storage->AllocatedBytes():0)+impl_->pending.additional_bytes;}
RayOmmStatus RayOpacityMicromapSystem::Status()const
{auto status=impl_->storage?impl_->storage->status:RayOmmStatus{};status.allocated_bytes=AllocatedBytes();return status;}
Result RayOpacityMicromapSystem::SetNativeByteBudget(std::uint64_t budget)
{if(impl_->pending.graph)return Fail(ErrorCode::InvalidState,"Native OMM budget cannot change during a pending graph.");
    if(auto s=impl_->storage){if(budget<s->AllocatedBytes())return Fail(ErrorCode::InvalidState,"Native OMM budget excludes live backing.");
        s->profile.native_byte_budget=budget;s->profile.old_service_bytes=0;}return Result::Success();}
Result RayOpacityMicromapSystem::Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,const RayOmmProfile &profile,
    const RayOmmProductReader &reader,RayOmmCandidate &candidate)const
{
    candidate.storage_.reset();if(!profile.enabled)return Result::Success();
    if(!device||!allocator||!reader||!profile.frame_slots||profile.frame_slots>16||
       profile.maximum_subdivision<1||profile.maximum_subdivision>4||!profile.native_byte_budget||
       profile.old_service_bytes>profile.native_byte_budget)
        return Fail(ErrorCode::InvalidArgument,"OMM requires actual products, bounded native budget and frame slots.");
    const auto hardware=QueryRendererFeatureHardware(device);
    if(!hardware.opacity_micromap_arrays||!hardware.shader_model69)
        return Fail(ErrorCode::InvalidState,"OMM transport requires native DXR1.2 arrays and Shader Model6.9.");
    auto s=std::make_shared<RayOmmStorage>();s->allocator=allocator;s->profile=profile;s->reader=reader;s->slots.resize(profile.frame_slots);
    if(FAILED(device->QueryInterface(IID_PPV_ARGS(s->device.ReleaseAndGetAddressOf()))))return Fail(ErrorCode::TaskFailed,"OMM Device5 is unavailable.");
    s->status.enabled=true;s->status.reason="Awaiting selected immutable coverage packages and native array submission.";
    candidate.storage_=std::move(s);return Result::Success();
}
Result RayOpacityMicromapSystem::Commit(RayOmmCandidate &&candidate,const CompletionSet &completed)
{
    if(impl_->pending.graph|| (impl_->storage&&(!impl_->storage->status.usable||!impl_->storage->completion.IsSatisfiedBy(completed))))
        return Fail(ErrorCode::InvalidState,"OMM replacement requires completed usable storage and no pending graph.");
    impl_->storage=std::move(candidate.storage_);return Result::Success();
}
namespace
{
Result CreateEntry(RayOmmStorage &s,const RayOmmGeometryRequest &request,std::shared_ptr<Entry> &out)
{
    auto e=std::make_shared<Entry>();
    if(auto r=s.reader(request.product,e->package);!r)return r;
    try{e->view=render_data::ReadOpacityMicromapPackage(e->package,request.product_asset);}
    catch(const std::exception &error){return Fail(ErrorCode::InvalidArgument,error.what());}
    if(!Same(e->view.identity,request.identity)||!MatchWords(e->view,request.actual_opacity_words)||
        e->view.maximum_subdivision>s.profile.maximum_subdivision)return Result::Success();
    e->generation=request.generation;e->generation_owner=request.generation_owner;
    e->descriptor_offset=Align(e->view.states.size(),8);
    e->index_offset=Align(e->descriptor_offset+e->view.descriptor_bytes.size(),4);
    e->input_bytes=e->index_offset+e->view.index_bytes.size();
    if(!e->descriptor_offset||!e->index_offset||e->input_bytes<e->index_offset)
        return Fail(ErrorCode::InvalidArgument,"Native OMM input offsets overflow.");
    for(const auto &h:e->view.histogram)e->histogram.push_back({h.count,h.subdivision_level,D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE});
    // Prebuild does not read GPU contents, but validates non-null input addresses
    // for a nonempty array. Use the actual bounded input allocation.
    if(auto r=Buffer(s.allocator.Get(),e->input_bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COMMON,false,Access::Common,e->input);!r)return r;
    const auto array=e->ArrayDesc();
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY;inputs.NumDescs=1;
    inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;inputs.pOpacityMicromapArrayDesc=&array;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};s.device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs,&info);
    const auto result_bytes=Align(info.ResultDataMaxSizeInBytes,D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_BYTE_ALIGNMENT);
    const auto scratch_bytes=Align(info.ScratchDataSizeInBytes,D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    if(!result_bytes||!scratch_bytes)return Fail(ErrorCode::InvalidState,"Native OMM prebuild requirements are invalid.");
    if(auto r=Buffer(s.allocator.Get(),e->input_bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_GENERIC_READ,false,Access::CopySource,e->upload);!r)return r;
    if(auto r=Buffer(s.allocator.Get(),result_bytes,D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|
            (s.profile.enhanced_barriers?D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE:D3D12_RESOURCE_FLAG_NONE),
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,s.profile.enhanced_barriers,Access::ASRead,e->array);!r)return r;
    if(auto r=Buffer(s.allocator.Get(),scratch_bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,false,Access::ASBuildScratch,e->scratch);!r)return r;
    if(e->array.resource->GetGPUVirtualAddress()%D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_BYTE_ALIGNMENT)
        return Fail(ErrorCode::InvalidState,"Native OMM array address is not128-byte aligned.");
    void *mapped{};D3D12_RANGE empty{0,0};if(FAILED(e->upload.resource->Map(0,&empty,&mapped)))return Fail(ErrorCode::TaskFailed,"Cannot map OMM input upload.");
    std::memset(mapped,0,std::size_t(e->input_bytes));auto *bytes=static_cast<std::byte*>(mapped);
    std::memcpy(bytes,e->view.states.data(),e->view.states.size());
    std::memcpy(bytes+e->descriptor_offset,e->view.descriptor_bytes.data(),e->view.descriptor_bytes.size());
    std::memcpy(bytes+e->index_offset,e->view.index_bytes.data(),e->view.index_bytes.size());
    e->upload.resource->Unmap(0,nullptr);out=std::move(e);return Result::Success();
}
}
Result RayOpacityMicromapSystem::Register(RenderGraphBuilder &graph,std::span<const RayOmmGeometryRequest> requests,
    std::uint32_t slot,const CompletionSet &completed,std::vector<RayOmmBinding> &bindings)
{
    bindings.assign(requests.size(),{});auto s=impl_->storage;if(!s)return Result::Success();
    if(!s->status.usable||impl_->pending.graph||slot>=s->slots.size()||!s->slots[slot].IsSatisfiedBy(completed))
        return Fail(ErrorCode::InvalidState,"OMM registration needs a completed frame slot and usable idle storage.");
    std::erase_if(s->retired,[&](const auto &e){return e->completion.IsSatisfiedBy(completed)&&e.use_count()==1;});
    for(const auto &[key,e]:s->entries)if(e->built&&e->completion.IsSatisfiedBy(completed))
    {e->upload.Reset();e->scratch.Reset();}
    Impl::Pending p;p.graph=&graph;p.slot=slot;p.status=s->status;
    p.status.ready=p.status.applied=false;p.status.linked_triangles=p.status.known_cells=p.status.unknown_cells=0;p.status.unavailable_geometries=0;
    std::vector<std::shared_ptr<Entry>> selected(requests.size());
    for(std::size_t i=0;i<requests.size();++i)
    {
        const auto &r=requests[i];
        if(r.product.empty()||!r.product_asset.value||!r.generation||!r.generation_owner||r.actual_opacity_words.empty())
        {++p.status.unavailable_geometries;continue;}
        const Key key{r.generation,r.identity.coverage_key};std::shared_ptr<Entry> e;
        auto pending=p.entries.find(key),old=s->entries.find(key);
        if(pending!=p.entries.end())e=pending->second;else if(old!=s->entries.end())e=old->second;
        else
        {
            if(auto result=CreateEntry(*s,r,e);!result)
            {return result;} // selected missing/invalid product is an explicit preparation failure
            if(!e){++p.status.unavailable_geometries;continue;}
            p.additional_bytes+=e->AllocatedBytes();
        }
        if(!Same(e->view.identity,r.identity)||!MatchWords(e->view,r.actual_opacity_words))
        {++p.status.unavailable_geometries;continue;}
        if(s->AllocatedBytes()>s->profile.native_byte_budget-s->profile.old_service_bytes||
           p.additional_bytes>s->profile.native_byte_budget-s->profile.old_service_bytes-s->AllocatedBytes())
            return Fail(ErrorCode::InvalidState,"Actual native OMM old/candidate/in-flight backing exceeds the admitted byte budget.");
        p.entries[key]=e;selected[i]=e;
        p.status.linked_triangles+=e->view.identity.triangle_count;p.status.known_cells+=e->view.known_cells;p.status.unknown_cells+=e->view.unknown_cells;
    }
    const auto import=[&](AllocationResource &r,const char *name)
    {for(const auto &v:p.imports)if(v.allocation==&r)return graph.Version(v.handle);
        auto handle=graph.ImportBuffer({r.resource.Get(),r.graph_completion,r.graph_queue},r.graph_access,name);
        p.imports.push_back({&r,handle});return graph.Version(handle);};
    for(const auto &[key,e]:p.entries)
    {
        Job j;j.entry=e;j.input=import(e->input,"OMMImmutableInput");j.array=import(e->array,"OMMNativeArray");j.build=!e->built;
        if(j.build){j.upload=import(e->upload,"OMMInputUpload");j.scratch=import(e->scratch,"OMMBuildScratch");}p.jobs.push_back(j);
    }
    const bool building=std::any_of(p.jobs.begin(),p.jobs.end(),[](const auto &j){return j.build;});
    if(building)
    {
        auto copy=graph.AddPass(300,"OpacityMicromapInputUpload",QueueHint::Direct);
        for(auto &j:p.jobs)if(j.build){copy.Read(j.upload,Access::CopySource);j.input=copy.Write(j.input,Access::CopyDest);}
        copy.SetExecuteChecked([s,jobs=p.jobs](RenderPassContext &ctx)->Result
        {auto *cmd=static_cast<ID3D12GraphicsCommandList*>(ctx.NativeCommandList());if(!cmd)return Fail(ErrorCode::InvalidState,"OMM upload has no native Direct command list.");
            for(const auto &j:jobs)if(j.build)
            {if(ctx.Resolve(j.upload.resource)!=j.entry->upload.resource.Get()||ctx.Resolve(j.input.resource)!=j.entry->input.resource.Get())return Fail(ErrorCode::InvalidState,"OMM upload graph/native resources differ.");
                cmd->CopyBufferRegion(j.entry->input.resource.Get(),0,j.entry->upload.resource.Get(),0,j.entry->input_bytes);}return Result::Success();});
        auto build=graph.AddPass(301,"OpacityMicromapArrayBuild",QueueHint::Direct);
        for(auto &j:p.jobs)if(j.build){build.Read(j.input,Access::ASBuildInput);j.array=build.Write(j.array,Access::ASBuildWrite);j.scratch=build.Write(j.scratch,Access::ASBuildScratch);}
        build.SetExecuteChecked([s,jobs=p.jobs](RenderPassContext &ctx)->Result
        {
            auto *native=static_cast<ID3D12GraphicsCommandList*>(ctx.NativeCommandList());ComPtr<ID3D12GraphicsCommandList4> cmd;
            if(!native||native->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT||FAILED(native->QueryInterface(IID_PPV_ARGS(cmd.ReleaseAndGetAddressOf()))))return Fail(ErrorCode::TaskFailed,"OMM requires actual Direct DXR command list4.");
            for(const auto &j:jobs)if(j.build)
            {
                const auto &e=*j.entry;
                if(ctx.Resolve(j.input.resource)!=e.input.resource.Get()||ctx.Resolve(j.array.resource)!=e.array.resource.Get()||ctx.Resolve(j.scratch.resource)!=e.scratch.resource.Get())
                    return Fail(ErrorCode::InvalidState,"OMM array graph/native resources differ.");
                const auto array=e.ArrayDesc();D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
                desc.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY;desc.Inputs.NumDescs=1;
                desc.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;desc.Inputs.pOpacityMicromapArrayDesc=&array;
                desc.DestAccelerationStructureData=e.array.resource->GetGPUVirtualAddress();desc.ScratchAccelerationStructureData=e.scratch.resource->GetGPUVirtualAddress();
                cmd->BuildRaytracingAccelerationStructure(&desc,0,nullptr);
            }return Result::Success();
        });
    }
    for(const auto &j:p.jobs){graph.SetFinalAccess(j.array,Access::ASRead);graph.SetFinalAccess(j.input,Access::ASBuildInput);}
    for(std::size_t i=0;i<selected.size();++i)if(const auto &e=selected[i])
    {const auto job=std::find_if(p.jobs.begin(),p.jobs.end(),[&](const auto &j){return j.entry==e;});
        bindings[i]={job->array,job->input,e->array.resource.Get(),e->input.resource.Get(),e->view.identity,e->generation,
            e->view.known_cells,e->view.unknown_cells,e->view.identity.triangle_count,0,e->index_offset,e};}
    p.status.ready=!p.entries.empty();p.status.reason=p.status.ready?
        (p.status.known_cells?"Native four-state OMM arrays registered; awaiting actual submission.":"Native OMM arrays registered with zero proved cells; exact coverage remains required."):
        "Selected Generation has no matching immutable coverage product.";
    impl_->pending=std::move(p);return Result::Success();
}
Result RayOpacityMicromapSystem::CompleteSubmission(const RenderGraphBuilder &graph,const CompletionSet &completion,bool success)
{
    auto s=impl_->storage;auto &p=impl_->pending;if(!p.graph)return Result::Success();
    if(!s||p.graph!=&graph)return Fail(ErrorCode::InvalidArgument,"OMM completion has no matching registered graph.");
    s->completion.Merge(completion);s->slots[p.slot].Merge(completion);
    for(const auto &i:p.imports){auto required=graph.ResourceCompletion(i.handle);i.allocation->graph_completion.Merge(required);i.allocation->graph_completion.Merge(completion);
        s->completion.Merge(required);s->slots[p.slot].Merge(required);
        if(success){i.allocation->graph_access=graph.FinalAccess(i.handle);i.allocation->graph_queue=graph.FinalQueue(i.handle);}}
    for(const auto &[key,e]:p.entries)e->completion.Merge(s->completion);
    for(const auto &[key,e]:s->entries)if(!p.entries.contains(key)||p.entries.at(key)!=e)s->retired.push_back(e);
    if(success)
    {for(const auto &j:p.jobs)if(j.build)j.entry->built=true;s->entries=std::move(p.entries);s->status=std::move(p.status);s->status.applied=s->status.ready;}
    else
    {for(const auto &[key,e]:p.entries)s->retired.push_back(e);s->status.usable=s->status.ready=s->status.applied=false;
        s->status.reason="Native OMM submission failed; all queues must retire before reset.";}
    p={};return Result::Success();
}
Result RayOpacityMicromapSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if(impl_->pending.graph||(impl_->storage&&!impl_->storage->completion.IsSatisfiedBy(completed)))return Fail(ErrorCode::InvalidState,"OMM reset requires every submitted queue lease completed.");
    impl_->storage.reset();return Result::Success();
}
}
