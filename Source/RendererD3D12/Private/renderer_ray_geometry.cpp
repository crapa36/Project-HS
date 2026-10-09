#include "renderer_ray_geometry.hpp"
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
Result Fail(ErrorCode c,std::string m) { return Result::Failure(c,"hs_renderer_ray_geometry",std::move(m)); }
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
    if(FAILED(hr))return Native("Create ray geometry buffer",hr);
    r.graph_access=upload?Access::CopySource:Access::Common;return Result::Success();
}
Result Upload(AllocationResource &r,const void *data,std::size_t bytes)
{
    void *p{};D3D12_RANGE empty{0,0};auto hr=r.resource->Map(0,&empty,&p);
    if(FAILED(hr))return Native("Map ray geometry upload",hr);
    std::memcpy(p,data,bytes);D3D12_RANGE written{0,bytes};r.resource->Unmap(0,&written);return Result::Success();
}
bool ValidRead(const RenderGraphBuilder &g,const RayShaderRead &r)
{return r.resource&&r.version.resource&&g.Resolve(r.version.resource)==r.resource&&
    (r.access==Access::ComputeRead||r.access==Access::AllShaderRead||r.access==Access::ShaderRead);}
} // namespace

struct RayGeometryStorage
{
    struct Slot
    {
        AllocationResource positions,records,instances,record_upload,instance_upload,constants;
        CompletionSet completion;
        RayShaderBinder binder;
        std::vector<ComPtr<ID3D12Resource>> reads;
    };
    ComPtr<ID3D12Device> device;ComPtr<D3D12MA::Allocator> allocator;
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    std::vector<Slot> slots;CompletionSet completion;
    std::uint32_t stride{};bool enabled{},usable{true};
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(std::uint32_t slot,std::uint32_t index) const
    {auto h=descriptors->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(slot*kRayDescriptorCount+index)*stride;return h;}
    void Raw(std::uint32_t s,std::uint32_t index,ID3D12Resource *r,bool uav)
    {
        if(uav){D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=DXGI_FORMAT_R32_TYPELESS;d.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
            d.Buffer.NumElements=r?UINT(r->GetDesc().Width/4):1;d.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;device->CreateUnorderedAccessView(r,nullptr,&d,Cpu(s,index));}
        else{D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=DXGI_FORMAT_R32_TYPELESS;d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
            d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Buffer.NumElements=r?UINT(r->GetDesc().Width/4):1;
            d.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;device->CreateShaderResourceView(r,&d,Cpu(s,index));}
    }
    void Structured(std::uint32_t s,std::uint32_t index,ID3D12Resource *r,std::uint32_t elements,std::uint32_t size)
    {D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
     d.Buffer.NumElements=std::max(elements,1u);d.Buffer.StructureByteStride=size;device->CreateShaderResourceView(r,&d,Cpu(s,index));}
};
struct RayGeometrySystem::Impl
{
    std::shared_ptr<RayGeometryStorage> storage;
    struct Import {AllocationResource *resource;ResourceHandle handle;};
    struct Pending{const RenderGraphBuilder *graph{};std::uint32_t slot{};std::vector<Import> imports;} pending;
};
RayGeometrySystem::RayGeometrySystem():impl_(std::make_unique<Impl>()){}
RayGeometrySystem::~RayGeometrySystem()=default;
std::uint64_t RayGeometrySystem::AllocatedBytes() const noexcept
{
    std::uint64_t bytes{};if(auto s=impl_->storage)for(auto &slot:s->slots)
        for(auto *r:{&slot.positions,&slot.records,&slot.instances,&slot.record_upload,&slot.instance_upload,&slot.constants})
            if(r->allocation)bytes+=r->allocation->GetSize();return bytes;
}
Result RayGeometrySystem::Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,ID3D12RootSignature *root,
    bool enabled,std::uint32_t frames,const RayShaderReader &reader,RayGeometryCandidate &candidate) const
{
    if(!device||!allocator||!frames||(enabled&&(!root||!reader)))return Fail(ErrorCode::InvalidArgument,"Invalid ray geometry preparation inputs.");
    auto s=std::make_shared<RayGeometryStorage>();s->enabled=enabled;s->device=device;s->allocator=allocator;s->slots.resize(frames);
    if(!enabled){candidate.storage_=std::move(s);return Result::Success();}
    // This pass deforms canonical vertices for either DXR or software SDF GI.
    // Acceleration structure support belongs to RayScene, not this compute pass.
    D3D12_FEATURE_DATA_SHADER_MODEL shader_model{D3D_SHADER_MODEL_6_5};
    auto hr=device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&shader_model,sizeof(shader_model));
    if(FAILED(hr)||shader_model.HighestShaderModel<D3D_SHADER_MODEL_6_5)return Fail(ErrorCode::TaskFailed,"Canonical geometry compute requires shader model 6.5.");
    s->root=root;std::vector<std::byte> code;
    if(auto r=reader("render/shader/ray_geometry_cs.dxil",code);!r)return r;
    if(code.empty())return Fail(ErrorCode::TaskFailed,"Ray geometry shader product is empty.");
    D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=root;p.CS={code.data(),code.size()};
    hr=device->CreateComputePipelineState(&p,IID_PPV_ARGS(&s->pipeline));if(FAILED(hr))return Native("Create ray geometry PSO",hr);
    if(frames>UINT_MAX/kRayDescriptorCount)return Fail(ErrorCode::InvalidArgument,"Ray geometry descriptor count overflow.");
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=frames*kRayDescriptorCount;
    hr=device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s->descriptors));if(FAILED(hr))return Native("Create ray geometry CPU descriptor heap",hr);
    s->stride=device->GetDescriptorHandleIncrementSize(hd.Type);candidate.storage_=std::move(s);return Result::Success();
}
Result RayGeometrySystem::Commit(RayGeometryCandidate &&c,const CompletionSet &completed)
{
    if(impl_->pending.graph||!c.storage_)return Fail(ErrorCode::InvalidState,"Ray geometry candidate is absent or registration pending.");
    if(impl_->storage&&(!impl_->storage->usable||!impl_->storage->completion.IsSatisfiedBy(completed)))
        return Fail(ErrorCode::InvalidState,"Ray geometry replacement requires completed, usable previous storage.");
    impl_->storage=std::move(c.storage_);return Result::Success();
}
Result RayGeometrySystem::Register(RenderGraphBuilder &graph,const RayGeometryInputs &in,RayGeometryOutputs &out)
{
    out={};auto s=impl_->storage;if(!s)return Fail(ErrorCode::InvalidState,"Ray geometry service has not been committed.");
    if(!s->enabled)return Result::Success();
    if(!s->usable||impl_->pending.graph)return Fail(ErrorCode::InvalidState,"Ray geometry storage is faulted or pending.");
    if(in.frame_slot>=s->slots.size()||!s->slots[in.frame_slot].completion.IsSatisfiedBy(in.completed)||
        in.geometries.size()!=in.instances.size()||in.geometries.size()>UINT_MAX/sizeof(RayHitGeometry)||!in.make_bindings)
        return Fail(ErrorCode::InvalidArgument,"Ray geometry frame slot, draw instances or table builder is invalid.");
    if(in.geometries.empty())return Result::Success();
    for(auto &r:in.reads)if(!ValidRead(graph,r))return Fail(ErrorCode::InvalidArgument,"Ray geometry read version does not match its native input.");
    auto records=std::vector<RayHitGeometry>(in.geometries.begin(),in.geometries.end());
    std::uint64_t positionBytes=0;
    for(std::size_t n=0;n<records.size();++n)
    {
        auto &g=records[n];const auto &i=in.instances[n];
        if(!g.vertex_descriptor||!g.index_descriptor||g.vertex_descriptor>=513||g.index_descriptor>=513||g.vertex_stride!=76||g.vertex_byte_offset%4||
            g.vertex_count<3||g.index_count<3||g.index_count%3||!g.instance_generation||
            (g.environment_ordinal!=~0u&&g.environment_ordinal>14)||(g.flags&~63u)||
            (g.flags&RayGeometryCachedSkin&&g.flags&RayGeometryDeformed)||g.mesh_id!=(i.mesh&255u)||
            !std::isfinite(i.yaw)||!std::isfinite(i.padding)||
            !std::isfinite(i.position.x)||!std::isfinite(i.position.y)||!std::isfinite(i.position.z)||
            !std::isfinite(i.scale.x)||!std::isfinite(i.scale.y)||!std::isfinite(i.scale.z)||
            i.scale.x<=0||i.scale.y<=0||i.scale.z<=0)
            return Fail(ErrorCode::InvalidArgument,"Ray geometry canonical layout, flags or exact instance transform is invalid.");
        if(std::uint64_t(g.vertex_byte_offset)+std::uint64_t(g.vertex_count)*76>UINT_MAX||
            (std::uint64_t(g.first_index)+g.index_count)*4>UINT_MAX)
            return Fail(ErrorCode::InvalidArgument,"Ray hit source range exceeds ByteAddressBuffer addressing.");
        if(g.flags&RayGeometryDeformed){g.position_byte_offset=UINT(positionBytes);positionBytes+=std::uint64_t(g.vertex_count)*12;}
    }
    if(positionBytes>UINT_MAX)return Fail(ErrorCode::InvalidArgument,"Ray deformed positions exceed ByteAddressBuffer addressing.");
    auto &slot=s->slots[in.frame_slot];
    auto ensure=[&](AllocationResource &r,std::uint64_t bytes,bool upload,bool uav)->Result
    {if(r.resource&&r.resource->GetDesc().Width>=bytes)return Result::Success();AllocationResource next;
     if(auto result=Buffer(s->allocator.Get(),bytes,upload,uav,next);!result)return result;r=std::move(next);return Result::Success();};
    const auto recordBytes=records.size()*sizeof(RayHitGeometry),instanceBytes=records.size()*sizeof(RayDrawInstance);
    for(auto r:{ensure(slot.positions,std::max<std::uint64_t>(positionBytes,4),false,true),
        ensure(slot.records,recordBytes,false,false),ensure(slot.instances,instanceBytes,false,false),
        ensure(slot.record_upload,recordBytes,true,false),ensure(slot.instance_upload,instanceBytes,true,false),
        ensure(slot.constants,256,true,false)})if(!r)return r;
    if(auto r=Upload(slot.record_upload,records.data(),recordBytes);!r)return r;
    if(auto r=Upload(slot.instance_upload,in.instances.data(),instanceBytes);!r)return r;
    std::array<std::uint32_t,4> info{UINT(records.size()),0,0,0};if(auto r=Upload(slot.constants,info.data(),16);!r)return r;
    // Initialize every suffix descriptor, including unused entries copied into
    // the shared table; a deformation shader references only b0,t3,t14,u0.
    D3D12_CONSTANT_BUFFER_VIEW_DESC cb{slot.constants.resource->GetGPUVirtualAddress(),256};s->device->CreateConstantBufferView(&cb,s->Cpu(in.frame_slot,0));
    D3D12_SHADER_RESOURCE_VIEW_DESC as{};as.ViewDimension=D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    as.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;s->device->CreateShaderResourceView(nullptr,&as,s->Cpu(in.frame_slot,1));
    s->Structured(in.frame_slot,2,nullptr,1,32);s->Structured(in.frame_slot,3,slot.records.resource.Get(),UINT(records.size()),sizeof(RayHitGeometry));
    s->Structured(in.frame_slot,4,slot.records.resource.Get(),UINT(records.size()),sizeof(RayHitGeometry));s->Raw(in.frame_slot,5,slot.positions.resource.Get(),false);
    constexpr DXGI_FORMAT formats[]{DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16_SNORM,DXGI_FORMAT_R32G32_UINT,DXGI_FORMAT_R16G16_FLOAT,
        DXGI_FORMAT_R32G32_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32G32_UINT,DXGI_FORMAT_R32G32B32A32_FLOAT};
    for(UINT j=0;j<9;++j){D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.Format=formats[j];v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.Texture2D.MipLevels=1;s->device->CreateShaderResourceView(nullptr,&v,s->Cpu(in.frame_slot,6+j));}
    s->Structured(in.frame_slot,15,slot.instances.resource.Get(),UINT(records.size()),48);s->Raw(in.frame_slot,16,slot.positions.resource.Get(),true);
    constexpr DXGI_FORMAT uf[]{DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32G32_UINT,DXGI_FORMAT_R16_FLOAT};
    for(UINT j=0;j<4;++j){D3D12_UNORDERED_ACCESS_VIEW_DESC v{};v.Format=uf[j];v.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        s->device->CreateUnorderedAccessView(nullptr,nullptr,&v,s->Cpu(in.frame_slot,17+j));}
    RayShaderBinder binder;if(auto r=in.make_bindings(s->Cpu(in.frame_slot,0),kRayDescriptorCount,binder);!r)return r;
    if(!binder)return Fail(ErrorCode::InvalidArgument,"Ray frame table builder did not return a compute binder.");
    Impl::Pending pending;pending.graph=&graph;pending.slot=in.frame_slot;
    auto import=[&](AllocationResource &r,std::string_view name){auto h=graph.ImportBuffer({r.resource.Get(),r.graph_completion,r.graph_queue},r.graph_access,name);
        pending.imports.push_back({&r,h});return graph.Version(h);};
    auto recordUpload=import(slot.record_upload,"RayGeometry.RecordUpload"),instanceUpload=import(slot.instance_upload,"RayGeometry.InstanceUpload");
    auto record=import(slot.records,"RayGeometry.Records"),instance=import(slot.instances,"RayGeometry.DrawInstances"),position=import(slot.positions,"RayGeometry.Positions");
    auto constant=import(slot.constants,"RayGeometry.Constants");
    auto upload=graph.AddPass(143,"RayGeometryUpload",QueueHint::Direct);upload.Read(recordUpload,Access::CopySource);upload.Read(instanceUpload,Access::CopySource);
    record=upload.Write(record,Access::CopyDest);instance=upload.Write(instance,Access::CopyDest);
    upload.SetExecute([s,slotIndex=in.frame_slot,recordBytes,instanceBytes](RenderPassContext &c){auto &v=s->slots[slotIndex];
        auto *cmd=static_cast<ID3D12GraphicsCommandList *>(c.NativeCommandList());cmd->CopyBufferRegion(v.records.resource.Get(),0,v.record_upload.resource.Get(),0,recordBytes);
        cmd->CopyBufferRegion(v.instances.resource.Get(),0,v.instance_upload.resource.Get(),0,instanceBytes);});
    if(positionBytes)
    {
        auto deform=graph.AddPass(142,"RayGeometryDeformation",QueueHint::Direct);deform.Read(record,Access::ComputeRead);deform.Read(instance,Access::ComputeRead);
        deform.Read(constant,Access::ComputeRead);for(auto &r:in.reads)deform.Read(r.version,r.access);position=deform.Write(position,Access::UnorderedWrite);
        deform.SetExecuteChecked([s,binder,records](RenderPassContext &c)->Result{auto *cmd=static_cast<ID3D12GraphicsCommandList *>(c.NativeCommandList());
            cmd->SetComputeRootSignature(s->root.Get());cmd->SetPipelineState(s->pipeline.Get());if(auto r=binder(cmd);!r)return r;
            for(UINT n=0;n<records.size();++n)if(records[n].flags&RayGeometryDeformed){std::uint32_t pass[]{n,0};cmd->SetComputeRoot32BitConstants(6,2,pass,0);
                const UINT groups=(records[n].vertex_count+63)/64;
                cmd->Dispatch(std::min(groups,65535u),(groups+65534)/65535,1);}return Result::Success();});
        graph.SetFinalAccess(position,Access::ComputeRead);
    }
    graph.SetFinalAccess(record,Access::ComputeRead);graph.SetFinalAccess(instance,Access::ComputeRead);
    slot.binder=std::move(binder);slot.reads.clear();for(auto &r:in.reads)slot.reads.emplace_back(r.resource);
    out.positions=position;out.records=record;out.instances=instance;out.position_resource=slot.positions.resource.Get();
    out.record_resource=slot.records.resource.Get();out.instance_resource=slot.instances.resource.Get();out.descriptor_suffix=s->Cpu(in.frame_slot,0);
    out.geometry_count=UINT(records.size());out.geometries=std::move(records);out.owner=s;impl_->pending=std::move(pending);return Result::Success();
}
Result RayGeometrySystem::CompleteSubmission(const RenderGraphBuilder &graph,const CompletionSet &completion,bool success)
{
    auto &p=impl_->pending;if(!p.graph)return Result::Success();if(p.graph!=&graph)return Fail(ErrorCode::InvalidArgument,"Wrong ray geometry completion graph.");
    auto s=impl_->storage;s->completion.Merge(completion);auto &slot=s->slots[p.slot];slot.completion.Merge(completion);
    for(auto &i:p.imports){auto lease=graph.ResourceCompletion(i.handle);s->completion.Merge(lease);slot.completion.Merge(lease);
        i.resource->graph_completion.Merge(completion);i.resource->graph_completion.Merge(lease);
        if(success){i.resource->graph_access=graph.FinalAccess(i.handle);i.resource->graph_queue=graph.FinalQueue(i.handle);}}
    if(!success)s->usable=false;p={};return Result::Success();
}
Result RayGeometrySystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if(impl_->pending.graph||(impl_->storage&&!impl_->storage->completion.IsSatisfiedBy(completed)))
        return Fail(ErrorCode::InvalidState,"Ray geometry reset requires completion of all submitted uses.");
    impl_->storage.reset();return Result::Success();
}
} // namespace hs
