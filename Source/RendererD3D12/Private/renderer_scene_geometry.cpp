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
Result Buffer(D3D12MA::Allocator *a,std::uint64_t bytes,bool upload,bool uav,AllocationResource &r)
{
    D3D12MA::ALLOCATION_DESC ad{}; ad.HeapType=upload?D3D12_HEAP_TYPE_UPLOAD:D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=std::max<std::uint64_t>(bytes,4);
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

struct SceneGeometryStorage
{
    struct Slot
    {
        AllocationResource positions,records,instances,record_upload,instance_upload,constants;
        CompletionSet completion;
        SceneShaderBinder binder;
        std::vector<ComPtr<ID3D12Resource>> reads;
    };
    ComPtr<ID3D12Device> device;ComPtr<D3D12MA::Allocator> allocator;
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    std::vector<Slot> slots;CompletionSet completion;
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
struct SceneGeometrySystem::Impl
{
    std::shared_ptr<SceneGeometryStorage> storage;
    struct Import {AllocationResource *resource;ResourceHandle handle;};
    struct Pending{const RenderGraphBuilder *graph{};std::uint32_t slot{};std::vector<Import> imports;} pending;
};
SceneGeometrySystem::SceneGeometrySystem():impl_(std::make_unique<Impl>()){}
SceneGeometrySystem::~SceneGeometrySystem()=default;
std::uint64_t SceneGeometrySystem::AllocatedBytes() const noexcept
{
    std::uint64_t bytes{};if(auto s=impl_->storage)for(auto &slot:s->slots)
        for(auto *r:{&slot.positions,&slot.records,&slot.instances,&slot.record_upload,&slot.instance_upload,&slot.constants})
            if(r->allocation)bytes+=r->allocation->GetSize();return bytes;
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
    if(impl_->storage&&(!impl_->storage->usable||!impl_->storage->completion.IsSatisfiedBy(completed)))
        return Fail(ErrorCode::InvalidState,"Scene geometry replacement requires completed, usable previous storage.");
    impl_->storage=std::move(c.storage_);return Result::Success();
}
Result SceneGeometrySystem::Register(RenderGraphBuilder &graph,const SceneGeometryInputs &in,SceneGeometryOutputs &out)
{
    out={};auto s=impl_->storage;if(!s)return Fail(ErrorCode::InvalidState,"Scene geometry service has not been committed.");
    if(!s->enabled)return Result::Success();
    if(!s->usable||impl_->pending.graph)return Fail(ErrorCode::InvalidState,"Scene geometry storage is faulted or pending.");
    if(in.frame_slot>=s->slots.size()||!s->slots[in.frame_slot].completion.IsSatisfiedBy(in.completed)||
        in.geometries.size()!=in.instances.size()||in.geometries.size()>UINT_MAX/sizeof(SceneGeometryRecord)||!in.make_bindings)
        return Fail(ErrorCode::InvalidArgument,"Scene geometry frame slot, draw instances or table builder is invalid.");
    if(in.geometries.empty())return Result::Success();
    for(auto &r:in.reads)if(!ValidRead(graph,r))return Fail(ErrorCode::InvalidArgument,"Scene geometry read version does not match its native input.");
    auto records=std::vector<SceneGeometryRecord>(in.geometries.begin(),in.geometries.end());
    std::uint64_t positionBytes=0;
    for(std::size_t n=0;n<records.size();++n)
    {
        auto &g=records[n];const auto &i=in.instances[n];
        if(auto valid=ValidateSceneGeometryRecord(g,i);!valid)return valid;
        if(g.flags&SceneGeometryDeformed){g.position_byte_offset=UINT(positionBytes);positionBytes+=std::uint64_t(g.vertex_count)*12;}
    }
    if(positionBytes>UINT_MAX)return Fail(ErrorCode::InvalidArgument,"Scene deformed positions exceed ByteAddressBuffer addressing.");
    auto &slot=s->slots[in.frame_slot];
    auto ensure=[&](AllocationResource &r,std::uint64_t bytes,bool upload,bool uav)->Result
    {if(r.resource&&r.resource->GetDesc().Width>=bytes)return Result::Success();AllocationResource next;
     if(auto result=Buffer(s->allocator.Get(),bytes,upload,uav,next);!result)return result;r=std::move(next);return Result::Success();};
    const auto recordBytes=records.size()*sizeof(SceneGeometryRecord),instanceBytes=records.size()*sizeof(SceneDrawInstance);
    for(auto r:{ensure(slot.positions,std::max<std::uint64_t>(positionBytes,4),false,true),
        ensure(slot.records,recordBytes,false,false),ensure(slot.instances,instanceBytes,false,false),
        ensure(slot.record_upload,recordBytes,true,false),ensure(slot.instance_upload,instanceBytes,true,false),
        ensure(slot.constants,256,true,false)})if(!r)return r;
    if(auto r=Upload(slot.record_upload,records.data(),recordBytes);!r)return r;
    if(auto r=Upload(slot.instance_upload,in.instances.data(),instanceBytes);!r)return r;
    std::array<std::uint32_t,4> info{UINT(records.size()),0,0,0};if(auto r=Upload(slot.constants,info.data(),16);!r)return r;
    // Initialize all four suffix descriptors used by the deformation shader.
    D3D12_CONSTANT_BUFFER_VIEW_DESC cb{slot.constants.resource->GetGPUVirtualAddress(),256};s->device->CreateConstantBufferView(&cb,s->Cpu(in.frame_slot,0));
    s->Structured(in.frame_slot,1,slot.records.resource.Get(),UINT(records.size()),sizeof(SceneGeometryRecord));
    s->Structured(in.frame_slot,2,slot.instances.resource.Get(),UINT(records.size()),sizeof(SceneDrawInstance));
    s->Positions(in.frame_slot,slot.positions.resource.Get());
    SceneShaderBinder binder;if(auto r=in.make_bindings(s->Cpu(in.frame_slot,0),kSceneGeometryDescriptorCount,binder);!r)return r;
    if(!binder)return Fail(ErrorCode::InvalidArgument,"Scene frame table builder did not return a compute binder.");
    Impl::Pending pending;pending.graph=&graph;pending.slot=in.frame_slot;
    auto import=[&](AllocationResource &r,std::string_view name){auto h=graph.ImportBuffer({r.resource.Get(),r.graph_completion,r.graph_queue},r.graph_access,name);
        pending.imports.push_back({&r,h});return graph.Version(h);};
    auto recordUpload=import(slot.record_upload,"SceneGeometry.RecordUpload"),instanceUpload=import(slot.instance_upload,"SceneGeometry.InstanceUpload");
    auto record=import(slot.records,"SceneGeometry.Records"),instance=import(slot.instances,"SceneGeometry.DrawInstances"),position=import(slot.positions,"SceneGeometry.Positions");
    auto constant=import(slot.constants,"SceneGeometry.Constants");
    auto upload=graph.AddPass(143,"SceneGeometryUpload",QueueHint::Direct);upload.Read(recordUpload,Access::CopySource);upload.Read(instanceUpload,Access::CopySource);
    record=upload.Write(record,Access::CopyDest);instance=upload.Write(instance,Access::CopyDest);
    upload.SetExecute([s,slotIndex=in.frame_slot,recordBytes,instanceBytes](RenderPassContext &c){auto &v=s->slots[slotIndex];
        auto *cmd=static_cast<ID3D12GraphicsCommandList *>(c.NativeCommandList());cmd->CopyBufferRegion(v.records.resource.Get(),0,v.record_upload.resource.Get(),0,recordBytes);
        cmd->CopyBufferRegion(v.instances.resource.Get(),0,v.instance_upload.resource.Get(),0,instanceBytes);});
    if(positionBytes)
    {
        auto deform=graph.AddPass(142,"SceneGeometryDeformation",QueueHint::Direct);deform.Read(record,Access::ComputeRead);deform.Read(instance,Access::ComputeRead);
        deform.Read(constant,Access::ComputeRead);for(auto &r:in.reads)deform.Read(r.version,r.access);position=deform.Write(position,Access::UnorderedWrite);
        deform.SetExecuteChecked([s,binder,records](RenderPassContext &c)->Result{auto *cmd=static_cast<ID3D12GraphicsCommandList *>(c.NativeCommandList());
            cmd->SetComputeRootSignature(s->root.Get());cmd->SetPipelineState(s->pipeline.Get());if(auto r=binder(cmd);!r)return r;
            for(UINT n=0;n<records.size();)
            {
                if(!(records[n].flags&SceneGeometryDeformed)){++n;continue;}
                const UINT first=n++,groups=(records[first].vertex_count+63)/64;
                // Equal-width jobs share a dispatch row each; no extra vertices or job buffer.
                while(n<records.size() && n-first<65535u && (records[n].flags&SceneGeometryDeformed) &&
                    (records[n].vertex_count+63)/64==groups)++n;
                std::uint32_t pass[]{first,0};cmd->SetComputeRoot32BitConstants(6,2,pass,0);
                cmd->Dispatch(std::min(groups,65535u),n-first,(groups+65534)/65535);
            }
            return Result::Success();});
        graph.SetFinalAccess(position,Access::ComputeRead);
    }
    graph.SetFinalAccess(record,Access::ComputeRead);graph.SetFinalAccess(instance,Access::ComputeRead);
    slot.binder=std::move(binder);slot.reads.clear();for(auto &r:in.reads)slot.reads.emplace_back(r.resource);
    out.positions=position;out.position_resource=slot.positions.resource.Get();
    out.geometries=std::move(records);out.owner=s;impl_->pending=std::move(pending);return Result::Success();
}
Result SceneGeometrySystem::CompleteSubmission(const RenderGraphBuilder &graph,const CompletionSet &completion,bool success)
{
    auto &p=impl_->pending;if(!p.graph)return Result::Success();if(p.graph!=&graph)return Fail(ErrorCode::InvalidArgument,"Wrong scene geometry completion graph.");
    auto s=impl_->storage;s->completion.Merge(completion);auto &slot=s->slots[p.slot];slot.completion.Merge(completion);
    for(auto &i:p.imports){auto lease=graph.ResourceCompletion(i.handle);s->completion.Merge(lease);slot.completion.Merge(lease);
        i.resource->graph_completion.Merge(completion);i.resource->graph_completion.Merge(lease);
        if(success){i.resource->graph_access=graph.FinalAccess(i.handle);i.resource->graph_queue=graph.FinalQueue(i.handle);}}
    if(!success)s->usable=false;p={};return Result::Success();
}
Result SceneGeometrySystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if(impl_->pending.graph||(impl_->storage&&!impl_->storage->completion.IsSatisfiedBy(completed)))
        return Fail(ErrorCode::InvalidState,"Scene geometry reset requires completion of all submitted uses.");
    impl_->storage.reset();return Result::Success();
}
} // namespace hs
