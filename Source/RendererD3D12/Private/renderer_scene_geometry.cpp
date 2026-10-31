#include "renderer_scene_geometry.hpp"
#include <hs/render_data/material_schema.hpp>
#include <hs/render_data/geometry_package.hpp>
#include "d3d12_resources.hpp"
#include <D3D12MemAlloc.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
Result Fail(ErrorCode c,std::string m) { return Result::Failure(c,"hs_renderer_scene_geometry",std::move(m)); }
Result Native(std::string_view op,HRESULT hr)
{ return Fail(ErrorCode::TaskFailed,std::string(op)+" failed (HRESULT "+std::to_string(std::int32_t(hr))+")."); }
std::uint64_t BufferWidth(std::uint64_t bytes)
{
    // Use the buffer's allocation granularity so changing job counts do not churn graph shapes.
    constexpr auto alignment=std::uint64_t{D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT};
    return (std::max<std::uint64_t>(bytes,4)+alignment-1)&~(alignment-1);
}
Result Buffer(D3D12MA::Allocator *a,std::uint64_t bytes,bool upload,bool uav,AllocationResource &r)
{
    D3D12MA::ALLOCATION_DESC ad{}; ad.HeapType=upload?D3D12_HEAP_TYPE_UPLOAD:D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=BufferWidth(bytes);
    d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags=uav?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
    auto hr=a->CreateResource(&ad,&d,upload?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COMMON,
        nullptr,&r.allocation,IID_PPV_ARGS(r.resource.ReleaseAndGetAddressOf()));
    if(FAILED(hr))return Native("Create scene geometry buffer",hr);
    r.graph_access=upload?Access::CopySource:Access::Common;return Result::Success();
}
Result Upload(AllocationResource &r,const void *data,std::size_t bytes)
{
    void *p{};D3D12_RANGE empty{0,0};auto hr=r.resource->Map(0,&empty,&p);
    if(FAILED(hr))return Native("Map scene geometry upload",hr);
    std::memcpy(p,data,bytes);D3D12_RANGE written{0,bytes};r.resource->Unmap(0,&written);return Result::Success();
}
bool ValidRead(const RenderGraphBuilder &g,const SceneShaderRead &r)
{return r.resource&&r.version.resource&&g.Resolve(r.version.resource)==r.resource&&
    (r.access==Access::ComputeRead||r.access==Access::AllShaderRead||r.access==Access::ShaderRead);}
} // namespace

Result ValidateSceneGeometryRecord(const SceneGeometryRecord &g, const SceneDrawInstance &i)
{
    if(!g.vertex_descriptor||!g.index_descriptor||g.vertex_descriptor>=render_data::kGeometryDescriptorCount||g.index_descriptor>=render_data::kGeometryDescriptorCount||g.vertex_stride!=76||g.vertex_byte_offset%4||
        g.vertex_count<3||g.index_count<3||g.index_count%3||!g.instance_generation||
        (g.environment_ordinal!=~0u&&g.environment_ordinal>=render_data::kAuthoredMaterialNames.size())||(g.flags&~55u)||
        (g.flags&SceneGeometryCachedSkin&&g.flags&SceneGeometryDeformed)||g.mesh_id!=(i.mesh&255u)||
        !std::isfinite(i.yaw)||!std::isfinite(i.padding)||
        !std::isfinite(i.position.x)||!std::isfinite(i.position.y)||!std::isfinite(i.position.z)||
        !std::isfinite(i.scale.x)||!std::isfinite(i.scale.y)||!std::isfinite(i.scale.z)||
        i.scale.x<=0||i.scale.y<=0||i.scale.z<=0)
        return Fail(ErrorCode::InvalidArgument,"Scene geometry canonical layout, flags or exact instance transform is invalid.");
    if(std::uint64_t(g.vertex_byte_offset)+std::uint64_t(g.vertex_count)*76>UINT_MAX||
        (std::uint64_t(g.first_index)+g.index_count)*4>UINT_MAX)
        return Fail(ErrorCode::InvalidArgument,"Scene geometry source range exceeds ByteAddressBuffer addressing.");
    return Result::Success();
}
Result PrepareSceneGeometryRows(std::span<SceneGeometryRecord> records,
    std::span<const SceneDrawInstance> draws,std::vector<std::uint32_t> &deformation_rows,
    std::uint64_t &deformed_position_bytes)
{
    deformation_rows.clear();deformed_position_bytes=0;
    if(records.size()!=draws.size())
        return Fail(ErrorCode::InvalidArgument,"Scene geometry logical rows and draw instances differ in count.");
    for(std::size_t i=0;i<records.size();++i)
    {
        if(auto result=ValidateSceneGeometryRecord(records[i],draws[i]);!result)
            return result;
        if(records[i].flags&SceneGeometryDeformed)
        {
            if(deformed_position_bytes>UINT_MAX)
                return Fail(ErrorCode::InvalidArgument,"Scene position offsets exceed shader address range.");
            records[i].position_byte_offset=static_cast<std::uint32_t>(deformed_position_bytes);
            deformed_position_bytes+=std::uint64_t(records[i].vertex_count)*12;
            deformation_rows.push_back(static_cast<std::uint32_t>(i));
        }
    }
    if(deformed_position_bytes>UINT_MAX)
        return Fail(ErrorCode::InvalidArgument,"Scene deformed positions exceed ByteAddressBuffer addressing.");
    return Result::Success();
}

struct SceneGeometryStorage
{
    struct Slot
    {
        std::shared_ptr<AllocationResource> positions{std::make_shared<AllocationResource>()};
        AllocationResource records,instances,record_upload,instance_upload,constants;
        std::vector<SceneGeometryRecord> prepared_records;
        std::vector<SceneDrawInstance> prepared_instances;
        std::vector<std::uint32_t> deformation_rows;
        SceneGeometryRecordData prepared_record_data;
        bool prepared_active{};
        CompletionSet completion;
        SceneShaderBinder binder;
        std::vector<ComPtr<ID3D12Resource>> reads;
    };
    ComPtr<ID3D12Device> device;ComPtr<D3D12MA::Allocator> allocator;
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    std::vector<Slot> slots;CompletionSet completion;
    std::vector<std::weak_ptr<AllocationResource>> retired_positions;
    std::uint32_t stride{};bool enabled{},usable{true};
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(std::uint32_t slot,std::uint32_t index) const
    {auto h=descriptors->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(slot*kSceneGeometryDescriptorCount+index)*stride;return h;}
    void Positions(std::uint32_t slot,ID3D12Resource *resource)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=DXGI_FORMAT_R32_TYPELESS;d.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
        d.Buffer.NumElements=UINT(resource->GetDesc().Width/4);d.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(resource,nullptr,&d,Cpu(slot,3));
    }
    void Structured(std::uint32_t s,std::uint32_t index,ID3D12Resource *r,std::uint32_t elements,std::uint32_t size)
    {D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
     d.Buffer.NumElements=std::max(elements,1u);d.Buffer.StructureByteStride=size;device->CreateShaderResourceView(r,&d,Cpu(s,index));}
};
struct SceneGeometryPositionLease
{
    std::shared_ptr<SceneGeometryStorage> storage;
    std::shared_ptr<AllocationResource> position;
};
struct SceneGeometrySystem::Impl
{
    std::shared_ptr<SceneGeometryStorage> storage;
    struct Import {AllocationResource *resource;ResourceHandle handle;};
    struct Pending{const RenderGraphBuilder *graph{};std::uint32_t slot{};std::vector<Import> imports;} pending;
};

SceneGeometryPreparedFrame::SceneGeometryPreparedFrame(std::shared_ptr<SceneGeometryStorage> storage,
    std::shared_ptr<const void> owner,std::uint32_t slot)
    :storage_(std::move(storage)),owner_(std::move(owner)),frame_slot_(slot),active_(true){}
SceneGeometryPreparedFrame::SceneGeometryPreparedFrame(SceneGeometryPreparedFrame &&other) noexcept
    :storage_(std::move(other.storage_)),owner_(std::move(other.owner_)),frame_slot_(other.frame_slot_),
     active_(std::exchange(other.active_,false)){}
SceneGeometryPreparedFrame &SceneGeometryPreparedFrame::operator=(SceneGeometryPreparedFrame &&other) noexcept
{
    if(this!=&other){Release();storage_=std::move(other.storage_);owner_=std::move(other.owner_);
        frame_slot_=other.frame_slot_;active_=std::exchange(other.active_,false);}return *this;
}
SceneGeometryPreparedFrame::~SceneGeometryPreparedFrame(){Release();}
void SceneGeometryPreparedFrame::Release() noexcept
{
    if(active_&&storage_&&frame_slot_<storage_->slots.size())
    {
        auto &slot=storage_->slots[frame_slot_];
        if(slot.prepared_active)
        {slot.prepared_records.clear();slot.prepared_instances.clear();slot.deformation_rows.clear();
         slot.prepared_record_data={};slot.prepared_active=false;}
    }
    active_=false;owner_.reset();storage_.reset();
}
std::span<const SceneGeometryRecord> SceneGeometryPreparedFrame::Geometries() const noexcept
{return active_&&storage_&&frame_slot_<storage_->slots.size()&&storage_->slots[frame_slot_].prepared_active
    ?std::span<const SceneGeometryRecord>{storage_->slots[frame_slot_].prepared_records}:std::span<const SceneGeometryRecord>{};}
std::span<const SceneDrawInstance> SceneGeometryPreparedFrame::Instances() const noexcept
{return active_&&storage_&&frame_slot_<storage_->slots.size()&&storage_->slots[frame_slot_].prepared_active
    ?std::span<const SceneDrawInstance>{storage_->slots[frame_slot_].prepared_instances}:std::span<const SceneDrawInstance>{};}
ID3D12Resource *SceneGeometryPreparedFrame::PositionResource() const noexcept
{return active_&&storage_&&frame_slot_<storage_->slots.size()&&storage_->slots[frame_slot_].prepared_active&&
    storage_->slots[frame_slot_].prepared_record_data.deformed_position_bytes
        ?storage_->slots[frame_slot_].positions->resource.Get():nullptr;}
const SceneGeometryRecordData *SceneGeometryPreparedFrame::RecordData() const noexcept
{return active_&&storage_&&frame_slot_<storage_->slots.size()&&storage_->slots[frame_slot_].prepared_active
    ?&storage_->slots[frame_slot_].prepared_record_data:nullptr;}
SceneGeometrySystem::SceneGeometrySystem():impl_(std::make_unique<Impl>()){}
SceneGeometrySystem::~SceneGeometrySystem()=default;
std::uint64_t SceneGeometrySystem::AllocatedBytes() const noexcept
{
    std::uint64_t bytes{};if(auto s=impl_->storage)for(auto &slot:s->slots)
    {if(slot.positions->allocation)bytes+=slot.positions->allocation->GetSize();
     for(auto *r:{&slot.records,&slot.instances,&slot.record_upload,&slot.instance_upload,&slot.constants})
        if(r->allocation)bytes+=r->allocation->GetSize();}
    if(auto s=impl_->storage)for(const auto &weak:s->retired_positions)
        if(auto retired=weak.lock();retired&&retired->allocation)bytes+=retired->allocation->GetSize();
    return bytes;
}
Result SceneGeometrySystem::Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,ID3D12RootSignature *root,
    bool enabled,std::uint32_t frames,const SceneShaderReader &reader,SceneGeometryCandidate &candidate) const
{
    if(!device||!allocator||!frames||(enabled&&(!root||!reader)))return Fail(ErrorCode::InvalidArgument,"Invalid scene geometry preparation inputs.");
    auto s=std::make_shared<SceneGeometryStorage>();s->enabled=enabled;s->device=device;s->allocator=allocator;s->slots.resize(frames);
    if(!enabled){candidate.storage_=std::move(s);return Result::Success();}
    // Deform canonical vertices for software SDF GI with the shared raster evaluator.
    D3D12_FEATURE_DATA_SHADER_MODEL shader_model{D3D_SHADER_MODEL_6_5};
    auto hr=device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&shader_model,sizeof(shader_model));
    if(FAILED(hr)||shader_model.HighestShaderModel<D3D_SHADER_MODEL_6_5)return Fail(ErrorCode::TaskFailed,"Canonical geometry compute requires shader model 6.5.");
    s->root=root;std::vector<std::byte> code;
    if(auto r=reader("render/shader/scene_geometry_cs.dxil",code);!r)return r;
    if(code.empty())return Fail(ErrorCode::TaskFailed,"Scene geometry shader product is empty.");
    D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=root;p.CS={code.data(),code.size()};
    hr=device->CreateComputePipelineState(&p,IID_PPV_ARGS(&s->pipeline));if(FAILED(hr))return Native("Create scene geometry PSO",hr);
    if(frames>UINT_MAX/kSceneGeometryDescriptorCount)return Fail(ErrorCode::InvalidArgument,"Scene geometry descriptor count overflow.");
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=frames*kSceneGeometryDescriptorCount;
    hr=device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s->descriptors));if(FAILED(hr))return Native("Create scene geometry CPU descriptor heap",hr);
    s->stride=device->GetDescriptorHandleIncrementSize(hd.Type);candidate.storage_=std::move(s);return Result::Success();
}
Result SceneGeometrySystem::Commit(SceneGeometryCandidate &&c,const CompletionSet &completed)
{
    if(impl_->pending.graph||!c.storage_)return Fail(ErrorCode::InvalidState,"Scene geometry candidate is absent or registration pending.");
    if(impl_->storage&&(!impl_->storage->usable||!impl_->storage->completion.IsSatisfiedBy(completed)||
        std::ranges::any_of(impl_->storage->slots,[](const auto &slot){return slot.prepared_active;})))
        return Fail(ErrorCode::InvalidState,"Scene geometry replacement requires completed, usable previous storage.");
    impl_->storage=std::move(c.storage_);return Result::Success();
}
bool MatchesSceneGeometryGraphLayout(const SceneGeometryRecordData &current,
    const SceneGeometryGraphLayout &layout) noexcept
{
    // Record count and deformation byte count drive this frame's uploads and
    // dispatches. The graph declares whole buffers, so only a capacity change,
    // a zero/nonzero deformation branch, or a changed read set alters topology.
    return layout.valid && current.frame_slot == layout.record.frame_slot &&
        current.import_widths == layout.record.import_widths &&
        (current.deformed_position_bytes != 0) ==
            (layout.record.deformed_position_bytes != 0) &&
        current.reads == layout.record.reads;
}
Result SceneGeometrySystem::PrepareFrame(std::vector<SceneGeometryRecord> &&geometries,
    std::vector<SceneDrawInstance> &&instances,std::uint32_t frame_slot,const CompletionSet &completed,
    SceneGeometryPreparedFrame &prepared)
{
    prepared=SceneGeometryPreparedFrame{};auto s=impl_->storage;
    if(!s)return Fail(ErrorCode::InvalidState,"Scene geometry service has not been committed.");
    if(!s->enabled)return geometries.empty()&&instances.empty()?Result::Success():
        Fail(ErrorCode::InvalidState,"Disabled scene geometry service cannot prepare frame records.");
    if(!s->usable||impl_->pending.graph||frame_slot>=s->slots.size()||
        !s->slots[frame_slot].completion.IsSatisfiedBy(completed)||
        geometries.size()!=instances.size()||geometries.size()>UINT_MAX/sizeof(SceneGeometryRecord))
        return Fail(ErrorCode::InvalidArgument,"Scene geometry preview frame slot or draw count is invalid.");
    auto &slot=s->slots[frame_slot];
    if(slot.prepared_active)return Fail(ErrorCode::InvalidState,"Scene geometry frame slot already has an active preparation.");
    slot.prepared_records=std::move(geometries);slot.prepared_instances=std::move(instances);
    slot.prepared_record_data={};
    slot.prepared_active=true;
    SceneGeometryPreparedFrame attempt{s,{},frame_slot};
    auto &records=slot.prepared_records;
    auto &draws=slot.prepared_instances;
    slot.prepared_record_data.frame_slot=frame_slot;
    slot.prepared_record_data.record_count=records.size();
    slot.deformation_rows.clear();
    std::uint64_t bytes{};
    if(auto result=PrepareSceneGeometryRows(records,draws,slot.deformation_rows,bytes);!result)
        return result;
    slot.prepared_record_data.deformed_position_bytes=bytes;
    auto &position=slot.positions;
    if(bytes&&(!position->resource||position->resource->GetDesc().Width<std::max<std::uint64_t>(bytes,4)))
    {auto next=std::make_shared<AllocationResource>();
     if(auto result=Buffer(s->allocator.Get(),bytes,false,true,*next);!result)return result;
     if(position->resource){std::erase_if(s->retired_positions,[](const auto &weak){return weak.expired();});
         s->retired_positions.push_back(position);}
     position=std::move(next);}
    if(!slot.deformation_rows.empty())
    {
        const std::array<const AllocationResource *,6> imports{&slot.record_upload,&slot.instance_upload,
            &slot.records,&slot.instances,position.get(),&slot.constants};
        const auto deformation_count=slot.deformation_rows.size();
        const std::array<std::uint64_t,6> required{deformation_count*sizeof(SceneGeometryRecord),
            deformation_count*sizeof(SceneDrawInstance),deformation_count*sizeof(SceneGeometryRecord),
            deformation_count*sizeof(SceneDrawInstance),std::max<std::uint64_t>(bytes,4),256};
        for(std::size_t i=0;i<imports.size();++i)
            slot.prepared_record_data.import_widths[i]=std::max(BufferWidth(required[i]),imports[i]->resource
                ? imports[i]->resource->GetDesc().Width : std::uint64_t{4});
    }
    attempt.owner_=std::make_shared<SceneGeometryPositionLease>(SceneGeometryPositionLease{s,position});
    prepared=std::move(attempt);
    return Result::Success();
}
Result SceneGeometrySystem::RegisterPrepared(RenderGraphBuilder &graph,const SceneGeometryInputs &in,
    SceneGeometryPreparedFrame &prepared,SceneGeometryOutputs &out,SceneGeometryGraphLayout *capture)
{return RegisterCore(graph,in,prepared,out,capture,nullptr);}
Result SceneGeometrySystem::BindRetainedPrepared(RenderGraphBuilder &graph,const SceneGeometryInputs &in,
    SceneGeometryPreparedFrame &prepared,const SceneGeometryGraphLayout &layout,SceneGeometryOutputs &out)
{return RegisterCore(graph,in,prepared,out,nullptr,&layout);}
Result SceneGeometrySystem::RegisterCore(RenderGraphBuilder &graph,const SceneGeometryInputs &in,
    SceneGeometryPreparedFrame &prepared,SceneGeometryOutputs &out,SceneGeometryGraphLayout *capture,
    const SceneGeometryGraphLayout *retained)
{
    out={};auto s=impl_->storage;if(!s)return Fail(ErrorCode::InvalidState,"Scene geometry service has not been committed.");
    if(capture&&retained)return Fail(ErrorCode::InvalidArgument,"Scene geometry graph cannot capture and restore together.");
    if(!s->enabled)return Result::Success();
    if(!s->usable||impl_->pending.graph||!prepared.active_||prepared.storage_.get()!=s.get()||
        prepared.frame_slot_!=in.frame_slot||in.frame_slot>=s->slots.size())
        return Fail(ErrorCode::InvalidState,"Scene geometry preparation does not belong to this service, frame slot or registration.");
    if(!s->slots[in.frame_slot].prepared_active||!s->slots[in.frame_slot].completion.IsSatisfiedBy(in.completed))
        return Fail(ErrorCode::InvalidArgument,"Scene geometry frame slot, draw instances or table builder is invalid.");
    auto current=s->slots[in.frame_slot].prepared_record_data;
    auto &slot=s->slots[in.frame_slot];
    const bool has_deformation=!slot.deformation_rows.empty();
    if(has_deformation)
    {
        if(!in.make_bindings)
            return Fail(ErrorCode::InvalidArgument,"Scene deformation requires a current frame-table builder.");
        current.reads.reserve(in.reads.size());
        for(const auto &read:in.reads)current.reads.push_back({read.version,read.access});
    }
    if(retained&&!MatchesSceneGeometryGraphLayout(current,*retained))
        return Fail(ErrorCode::InvalidState,"Scene geometry graph shape or native range changed before retained binding.");
    if(capture){*capture={};capture->record=current;}
    if(current.record_count==0)
    {
        if(capture)capture->valid=true;
        auto &empty_slot=s->slots[prepared.frame_slot_];
        empty_slot.prepared_records.clear();empty_slot.prepared_instances.clear();empty_slot.deformation_rows.clear();
        empty_slot.prepared_record_data={};
        empty_slot.prepared_active=false;prepared.active_=false;return Result::Success();
    }
    auto &records=slot.prepared_records;
    const auto &instances=slot.prepared_instances;
    const auto positionBytes=current.deformed_position_bytes;
    if(records.size()!=instances.size()||records.size()!=current.record_count||
        (positionBytes&&(!slot.positions->resource||slot.positions->resource->GetDesc().Width<std::max<std::uint64_t>(positionBytes,4))))
        return Fail(ErrorCode::InvalidState,"Scene geometry prepared frame storage no longer matches its canonical range.");
    if(!has_deformation)
    {
        // No shader work consumes immutable or SkinCache rows. Keep the logical
        // rows and owner through submission because native GI still references
        // their stable ordering and transforms.
        if(capture)capture->valid=true;
        out.geometries=std::span<const SceneGeometryRecord>{records};
        out.owner=prepared.owner_;
        impl_->pending.graph=&graph;impl_->pending.slot=in.frame_slot;
        prepared.active_=false;
        return Result::Success();
    }
    for(auto &r:in.reads)if(!ValidRead(graph,r))return Fail(ErrorCode::InvalidArgument,"Scene geometry read version does not match its native input.");
    auto ensure=[&](AllocationResource &r,std::uint64_t bytes,bool upload,bool uav)->Result
    {if(r.resource&&r.resource->GetDesc().Width>=bytes)return Result::Success();AllocationResource next;
     if(auto result=Buffer(s->allocator.Get(),bytes,upload,uav,next);!result)return result;r=std::move(next);return Result::Success();};
    const auto deformationCount=slot.deformation_rows.size();
    const auto recordBytes=deformationCount*sizeof(SceneGeometryRecord),instanceBytes=deformationCount*sizeof(SceneDrawInstance);
    if(!slot.positions->resource)
        if(auto result=Buffer(s->allocator.Get(),std::max<std::uint64_t>(positionBytes,4),false,true,*slot.positions);!result)return result;
    if(slot.positions->resource->GetDesc().Width<std::max<std::uint64_t>(positionBytes,4))
        return Fail(ErrorCode::InvalidState,"Prepared scene position backing no longer covers its canonical range.");
    for(auto r:{ensure(slot.records,recordBytes,false,false),ensure(slot.instances,instanceBytes,false,false),
        ensure(slot.record_upload,recordBytes,true,false),ensure(slot.instance_upload,instanceBytes,true,false),
        ensure(slot.constants,256,true,false)})if(!r)return r;
    const std::array<AllocationResource *,6> native_imports{&slot.record_upload,&slot.instance_upload,
        &slot.records,&slot.instances,slot.positions.get(),&slot.constants};
    for(std::size_t i=0;i<native_imports.size();++i)
    {
        if(native_imports[i]->resource->GetDesc().Width!=current.import_widths[i])
            return Fail(ErrorCode::InvalidState,"Scene geometry native buffer range changed after preflight.");
        if(auto result=native_imports[i]->EnsureNativeContract(s->device.Get());!result)return result;
    }
    void *recordData{},*instanceData{};D3D12_RANGE empty{0,0};
    auto hr=slot.record_upload.resource->Map(0,&empty,&recordData);
    if(FAILED(hr))return Native("Map scene geometry deformation records",hr);
    hr=slot.instance_upload.resource->Map(0,&empty,&instanceData);
    if(FAILED(hr)){slot.record_upload.resource->Unmap(0,nullptr);return Native("Map scene geometry deformation instances",hr);}
    auto *recordBytesOut=static_cast<std::byte *>(recordData);
    auto *instanceBytesOut=static_cast<std::byte *>(instanceData);
    for(std::size_t compact=0;compact<deformationCount;++compact)
    {
        const auto row=slot.deformation_rows[compact];
        std::memcpy(recordBytesOut+compact*sizeof(SceneGeometryRecord),&records[row],sizeof(SceneGeometryRecord));
        std::memcpy(instanceBytesOut+compact*sizeof(SceneDrawInstance),&instances[row],sizeof(SceneDrawInstance));
    }
    D3D12_RANGE recordWritten{0,recordBytes},instanceWritten{0,instanceBytes};
    slot.record_upload.resource->Unmap(0,&recordWritten);slot.instance_upload.resource->Unmap(0,&instanceWritten);
    std::array<std::uint32_t,4> info{static_cast<std::uint32_t>(deformationCount),0,0,0};
    if(auto r=Upload(slot.constants,info.data(),16);!r)return r;
    // Initialize all four suffix descriptors used by the deformation shader.
    D3D12_CONSTANT_BUFFER_VIEW_DESC cb{slot.constants.resource->GetGPUVirtualAddress(),256};s->device->CreateConstantBufferView(&cb,s->Cpu(in.frame_slot,0));
    s->Structured(in.frame_slot,1,slot.records.resource.Get(),static_cast<std::uint32_t>(deformationCount),sizeof(SceneGeometryRecord));
    s->Structured(in.frame_slot,2,slot.instances.resource.Get(),static_cast<std::uint32_t>(deformationCount),sizeof(SceneDrawInstance));
    s->Positions(in.frame_slot,slot.positions->resource.Get());
    SceneShaderBinder binder;if(auto r=in.make_bindings(s->Cpu(in.frame_slot,0),kSceneGeometryDescriptorCount,binder);!r)return r;
    if(!binder)return Fail(ErrorCode::InvalidArgument,"Scene frame table builder did not return a compute binder.");
    Impl::Pending pending;pending.graph=&graph;pending.slot=in.frame_slot;
    bool bound=true;
    auto import=[&](AllocationResource &r,std::string_view name){
        const auto ordinal=pending.imports.size();BufferHandle h{};
        const ExternalBuffer external{r.resource.Get(),r.graph_completion,r.graph_queue,{},r.NativeContract()};
        if(retained)
        {
            if(ordinal>=retained->imports.size())bound=false;
            else {h=retained->imports[ordinal];bound=graph.BindBuffer(h,external,r.graph_access)&&bound;}
        }
        else h=graph.ImportBuffer(external,r.graph_access,name);
        pending.imports.push_back({&r,h});if(capture)capture->imports[ordinal]=h;
        return ResourceVersion{h,0};};
    auto recordUpload=import(slot.record_upload,"SceneGeometry.RecordUpload"),instanceUpload=import(slot.instance_upload,"SceneGeometry.InstanceUpload");
    auto record=import(slot.records,"SceneGeometry.Records"),instance=import(slot.instances,"SceneGeometry.DrawInstances"),position=import(*slot.positions,"SceneGeometry.Positions");
    auto constant=import(slot.constants,"SceneGeometry.Constants");
    if(!bound)return Fail(ErrorCode::InvalidState,"Scene geometry retained native import did not bind.");
    if(capture)capture->upload_pass=graph.PassCount();
    auto upload=retained?graph.BindPass(retained->upload_pass,143):
        std::optional<PassBuilder>{graph.AddPass(143,"SceneGeometryUpload",QueueHint::Direct)};
    if(!upload)return Fail(ErrorCode::InvalidState,"Scene geometry upload pass does not match retained plan.");
    if(!retained)
    {
        upload->Read(recordUpload,Access::CopySource);upload->Read(instanceUpload,Access::CopySource);
        record=upload->Write(record,Access::CopyDest);instance=upload->Write(instance,Access::CopyDest);
        if(capture){capture->records=record;capture->instances=instance;}
    }
    else {record=retained->records;instance=retained->instances;}
    upload->SetExecute([s,slotIndex=in.frame_slot,recordBytes,instanceBytes](RenderPassContext &c){auto &v=s->slots[slotIndex];
        auto *cmd=static_cast<ID3D12GraphicsCommandList *>(c.NativeCommandList());cmd->CopyBufferRegion(v.records.resource.Get(),0,v.record_upload.resource.Get(),0,recordBytes);
        cmd->CopyBufferRegion(v.instances.resource.Get(),0,v.instance_upload.resource.Get(),0,instanceBytes);});
    if(positionBytes)
    {
        if(capture)capture->deform_pass=graph.PassCount();
        auto deform=retained?graph.BindPass(retained->deform_pass,142):
            std::optional<PassBuilder>{graph.AddPass(142,"SceneGeometryDeformation",QueueHint::Direct)};
        if(!deform)return Fail(ErrorCode::InvalidState,"Scene geometry deformation pass does not match retained plan.");
        if(!retained)
        {
            deform->Read(record,Access::ComputeRead);deform->Read(instance,Access::ComputeRead);
            deform->Read(constant,Access::ComputeRead);for(auto &r:in.reads)deform->Read(r.version,r.access);
            position=deform->Write(position,Access::UnorderedWrite);
        }
        else position=retained->positions;
        deform->SetExecuteChecked([s,binder,slotIndex=in.frame_slot](RenderPassContext &c)->Result{auto *cmd=static_cast<ID3D12GraphicsCommandList *>(c.NativeCommandList());
            cmd->SetComputeRootSignature(s->root.Get());cmd->SetPipelineState(s->pipeline.Get());if(auto r=binder(cmd);!r)return r;
            const auto &records=s->slots[slotIndex].prepared_records;
            const auto &rows=s->slots[slotIndex].deformation_rows;
            for(UINT n=0;n<rows.size();)
            {
                const UINT first=n++,groups=(records[rows[first]].vertex_count+63)/64;
                // Equal-width jobs share a dispatch row each; no extra vertices or job buffer.
                while(n<rows.size() && n-first<65535u &&
                    (records[rows[n]].vertex_count+63)/64==groups)++n;
                std::uint32_t pass[]{first,0};cmd->SetComputeRoot32BitConstants(6,2,pass,0);
                cmd->Dispatch(std::min(groups,65535u),n-first,(groups+65534)/65535);
            }
            return Result::Success();});
        if(!retained)graph.SetFinalAccess(position,Access::ComputeRead);
    }
    if(!retained){graph.SetFinalAccess(record,Access::ComputeRead);graph.SetFinalAccess(instance,Access::ComputeRead);}
    if(capture){capture->positions=position;capture->valid=true;}
    slot.binder=std::move(binder);slot.reads.clear();for(auto &r:in.reads)slot.reads.emplace_back(r.resource);
    out.positions=position;out.position_resource=slot.positions->resource.Get();
    out.geometries=std::span<const SceneGeometryRecord>{records};
    out.owner=prepared.owner_;
    impl_->pending=std::move(pending);prepared.active_=false;return Result::Success();
}
Result SceneGeometrySystem::CompleteSubmission(const RenderGraphBuilder &graph,const CompletionSet &completion,bool success)
{
    auto &p=impl_->pending;if(!p.graph)return Result::Success();if(p.graph!=&graph)return Fail(ErrorCode::InvalidArgument,"Wrong scene geometry completion graph.");
    auto s=impl_->storage;s->completion.Merge(completion);auto &slot=s->slots[p.slot];slot.completion.Merge(completion);
    for(auto &i:p.imports){auto lease=graph.ResourceCompletion(i.handle);s->completion.Merge(lease);slot.completion.Merge(lease);
        i.resource->graph_completion.Merge(completion);i.resource->graph_completion.Merge(lease);
        if(success){i.resource->graph_access=graph.FinalAccess(i.handle);i.resource->graph_queue=graph.FinalQueue(i.handle);}}
    slot.prepared_records.clear();slot.prepared_instances.clear();slot.deformation_rows.clear();
    slot.prepared_record_data={};slot.prepared_active=false;
    if(!success)s->usable=false;p={};return Result::Success();
}
Result SceneGeometrySystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if(impl_->pending.graph||(impl_->storage&&(!impl_->storage->completion.IsSatisfiedBy(completed)||
        std::ranges::any_of(impl_->storage->slots,[](const auto &slot){return slot.prepared_active;}))))
        return Fail(ErrorCode::InvalidState,"Scene geometry reset requires completion of all submitted uses.");
    impl_->storage.reset();return Result::Success();
}
} // namespace hs
