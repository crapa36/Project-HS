#include "renderer_probe.hpp"
#include "d3d12_resources.hpp"
#include "renderer_startup.hpp"

#include <hs/core/cooked_format.hpp>
#include <hs/render_data/lighting_probe.hpp>

#include <D3D12MemAlloc.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr std::string_view kProduct="render/lighting/default_probe.rpkg";
std::atomic<std::uint64_t> g_lighting_probe_generation{};
Result Failure(ErrorCode code,std::string message)
{
    return Result::Failure(code,"hs_renderer_probe",std::move(message));
}
Result NativeFailure(std::string_view operation,HRESULT error)
{
    return Failure(ErrorCode::TaskFailed,std::string(operation)+" failed (HRESULT "+
        std::to_string(static_cast<std::int32_t>(error))+").");
}
std::uint64_t Align(std::uint64_t value,std::uint64_t alignment)
{
    return (value+alignment-1)&~(alignment-1);
}
Result Allocate(D3D12MA::Allocator *allocator,D3D12_HEAP_TYPE heap,
    D3D12_RESOURCE_DESC description,D3D12_RESOURCE_STATES initial,AllocationResource &target)
{
    D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType=heap;
    const auto native=renderer_detail::MeasureStartupOperation(RendererStartupOperation::NativeResourceAllocation, [&] { return allocator->CreateResource(&allocation,&description,initial,nullptr,&target.allocation,
        IID_PPV_ARGS(target.resource.ReleaseAndGetAddressOf())); });
    return FAILED(native)?NativeFailure("Allocate lighting probe resource",native):Result::Success();
}
D3D12_RESOURCE_DESC CubeDescription(const render_data::TextureDescription &cube)
{
    D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width=cube.width; d.Height=cube.height; d.DepthOrArraySize=6;
    d.MipLevels=static_cast<UINT16>(cube.mip_count); d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    d.SampleDesc.Count=1; return d;
}
D3D12_RESOURCE_DESC BufferDescription(std::uint64_t size)
{
    D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width=size; d.Height=1; d.DepthOrArraySize=1; d.MipLevels=1;
    d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; return d;
}
struct UploadCube
{
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    std::vector<UINT> rows;
    std::vector<UINT64> row_bytes;
};
std::uint64_t Footprints(ID3D12Device *device,const D3D12_RESOURCE_DESC &description,
    std::uint64_t base,UploadCube &cube)
{
    const UINT count=UINT(description.MipLevels)*6;
    cube.footprints.resize(count); cube.rows.resize(count); cube.row_bytes.resize(count);
    UINT64 ignored=0;
    device->GetCopyableFootprints(&description,0,count,base,cube.footprints.data(),cube.rows.data(),cube.row_bytes.data(),&ignored);
    auto end=base;
    for(UINT i=0;i<count;++i)
        end=std::max(end,cube.footprints[i].Offset+UINT64(cube.footprints[i].Footprint.RowPitch)*cube.rows[i]);
    return end;
}
} // namespace

struct LightingProbeStorage
{
    ComPtr<ID3D12Device> device;
    ComPtr<D3D12MA::Allocator> allocator;
    AllocationResource radiance,prefiltered,irradiance,upload;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    UploadCube raw_upload,ggx_upload;
    std::uint64_t sh_offset{},bytes{};
    std::uint64_t generation{};
    std::uint32_t descriptor_stride{};
    DirectX::XMFLOAT4 options{};
    CompletionSet completion,upload_completion;
    bool uploaded{},usable{true};
    D3D12_CPU_DESCRIPTOR_HANDLE Srv(std::uint32_t index) const
    {
        auto result=descriptors->GetCPUDescriptorHandleForHeapStart();
        result.ptr+=SIZE_T(index)*descriptor_stride; return result;
    }
};
struct LightingProbeSystem::Impl
{
    std::shared_ptr<LightingProbeStorage> storage;
    struct Import { AllocationResource *allocation{}; ResourceHandle handle; };
    struct Pending
    {
        const RenderGraphBuilder *graph{};
        std::shared_ptr<LightingProbeStorage> storage;
        std::vector<Import> imports;
        bool upload{};
    } pending;
};
LightingProbeSystem::LightingProbeSystem():impl_(std::make_unique<Impl>()) {}
LightingProbeSystem::~LightingProbeSystem()=default;
std::uint64_t LightingProbeCandidate::AllocatedBytes() const noexcept { return storage_?storage_->bytes:0; }
std::uint64_t LightingProbeSystem::AllocatedBytes() const noexcept { return impl_->storage?impl_->storage->bytes:0; }
DirectX::XMFLOAT4 LightingProbeSystem::FrameOptions() const noexcept
{
    return impl_->storage && impl_->storage->usable?impl_->storage->options:DirectX::XMFLOAT4{};
}

Result LightingProbeSystem::Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,
    std::span<const std::byte> bytes,LightingProbeCandidate &candidate) const
{
    candidate={};
    if(!device || !allocator) return Failure(ErrorCode::InvalidArgument,"Probe preparation requires a device and allocator.");
    try
    {
        const auto package=renderer_detail::MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return render_data::ReadLightingProbePackage(bytes,MakeAssetId(kProduct)); });
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_R32G32B32A32_FLOAT};
        const auto queried=device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support));
        if(FAILED(queried)) return NativeFailure("Query HDR probe format",queried);
        if(!(support.Support1&D3D12_FORMAT_SUPPORT1_TEXTURECUBE) || !(support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE))
            return Failure(ErrorCode::TaskFailed,"RGBA32F cubemap sampling is unavailable.");
        auto storage=std::make_shared<LightingProbeStorage>(); storage->device=device; storage->allocator=allocator;
        const auto rawDescription=CubeDescription(package.radiance.description);
        const auto ggxDescription=CubeDescription(package.prefiltered.description);
        if(auto result=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,rawDescription,D3D12_RESOURCE_STATE_COMMON,storage->radiance);!result) return result;
        if(auto result=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,ggxDescription,D3D12_RESOURCE_STATE_COMMON,storage->prefiltered);!result) return result;
        if(auto result=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,BufferDescription(144),D3D12_RESOURCE_STATE_COMMON,storage->irradiance);!result) return result;
        for(auto *resource:{&storage->radiance,&storage->prefiltered,&storage->irradiance}) resource->graph_access=Access::Common;
        const auto rawEnd=Footprints(device,rawDescription,0,storage->raw_upload);
        const auto ggxEnd=Footprints(device,ggxDescription,Align(rawEnd,D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT),storage->ggx_upload);
        storage->sh_offset=Align(ggxEnd,16);
        if(auto result=Allocate(allocator,D3D12_HEAP_TYPE_UPLOAD,BufferDescription(storage->sh_offset+144),
            D3D12_RESOURCE_STATE_GENERIC_READ,storage->upload);!result) return result;
        storage->upload.graph_access=Access::CopySource;
        std::byte *mapped=nullptr; D3D12_RANGE empty{0,0};
        const auto mappedResult=storage->upload.resource->Map(0,&empty,reinterpret_cast<void **>(&mapped));
        if(FAILED(mappedResult)) return NativeFailure("Map lighting probe staging",mappedResult);
        renderer_detail::StartupOperationScope pack_timing(RendererStartupOperation::HostUploadPack);
        auto writeCube=[&](const render_data::TexturePackageView &cube,const UploadCube &upload) {
            for(std::size_t i=0;i<cube.subresources.size();++i)
            {
                const auto &source=cube.subresources[i]; const auto &footprint=upload.footprints[i];
                for(std::uint32_t y=0;y<source.height;++y)
                    std::memcpy(mapped+footprint.Offset+std::size_t(y)*footprint.Footprint.RowPitch,
                        source.bytes.data()+std::size_t(y)*source.row_pitch,source.row_pitch);
            }
        };
        writeCube(package.radiance,storage->raw_upload); writeCube(package.prefiltered,storage->ggx_upload);
        static_assert(sizeof(Float4)==16);
        std::memcpy(mapped+storage->sh_offset,package.irradiance.data(),144);
        D3D12_RANGE written{0,static_cast<SIZE_T>(storage->sh_offset+144)};
        storage->upload.resource->Unmap(0,&written);
        pack_timing.Stop();
        renderer_detail::StartupOperationScope descriptors_timing(RendererStartupOperation::DescriptorWrite);
        D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors=3;
        const auto created=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(storage->descriptors.ReleaseAndGetAddressOf()));
        if(FAILED(created)) return NativeFailure("Create lighting probe SRVs",created);
        storage->descriptor_stride=device->GetDescriptorHandleIncrementSize(heap.Type);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBE; view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.TextureCube.MipLevels=1; device->CreateShaderResourceView(storage->radiance.resource.Get(),&view,storage->Srv(0));
        view.TextureCube.MipLevels=package.prefiltered.description.mip_count;
        device->CreateShaderResourceView(storage->prefiltered.resource.Get(),&view,storage->Srv(1));
        view={}; view.Format=DXGI_FORMAT_UNKNOWN; view.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Buffer.NumElements=9; view.Buffer.StructureByteStride=16;
        device->CreateShaderResourceView(storage->irradiance.resource.Get(),&view,storage->Srv(2));
        storage->options={std::cos(package.description.orientation_y_radians),std::sin(package.description.orientation_y_radians),
            float(package.prefiltered.description.mip_count-1),1.0f};
        HS_RENDERER_SET_DEBUG_NAME(storage->radiance.resource.Get(), L"LightingProbeRawRadiance");
        HS_RENDERER_SET_DEBUG_NAME(storage->prefiltered.resource.Get(), L"LightingProbeGgxPrefilter");
        HS_RENDERER_SET_DEBUG_NAME(storage->irradiance.resource.Get(), L"LightingProbeCosineSh9");
        for(const auto *resource:{&storage->radiance,&storage->prefiltered,&storage->irradiance,&storage->upload})
            storage->bytes+=resource->allocation->GetSize();
        candidate.storage_=std::move(storage); return Result::Success();
    }
    catch(const std::exception &error) { return Failure(ErrorCode::InvalidArgument,error.what()); }
}
Result LightingProbeSystem::Commit(LightingProbeCandidate &&candidate,const CompletionSet &completed)
{
    if(!candidate.storage_) return Failure(ErrorCode::InvalidArgument,"Probe candidate is empty.");
    if(impl_->pending.graph) return Failure(ErrorCode::InvalidState,"Complete the registered probe graph before replacement.");
    if(impl_->storage && !impl_->storage->completion.IsSatisfiedBy(completed))
        return Failure(ErrorCode::InvalidState,"Probe replacement requires completed GPU leases.");
    candidate.storage_->generation=g_lighting_probe_generation.fetch_add(1,std::memory_order_relaxed)+1;
    impl_->storage=std::move(candidate.storage_); return Result::Success();
}
bool LightingProbeSystem::MatchesLayout(const LightingProbeLayout &layout) const noexcept
{
    const auto storage=impl_->storage;
    return storage&&storage->usable&&layout.storage_generation==storage->generation&&
        layout.upload==!storage->uploaded&&(!layout.upload||bool(storage->upload.resource));
}
Result LightingProbeSystem::Register(RenderGraphBuilder &graph,const CompletionSet &completed,
    LightingProbeOutputs &outputs,LightingProbeLayout *layout)
{return RegisterImpl(graph,completed,outputs,layout,nullptr);}
Result LightingProbeSystem::BindRetained(RenderGraphBuilder &graph,const CompletionSet &completed,
    const LightingProbeLayout &layout,LightingProbeOutputs &outputs)
{return RegisterImpl(graph,completed,outputs,nullptr,&layout);}
Result LightingProbeSystem::RegisterImpl(RenderGraphBuilder &graph,const CompletionSet &completed,
    LightingProbeOutputs &outputs,LightingProbeLayout *layout,const LightingProbeLayout *retained)
{
    outputs={}; auto storage=impl_->storage;
    if(!storage) return Failure(ErrorCode::InvalidState,"No lighting probe is prepared.");
    if(impl_->pending.graph || !storage->usable)
        return Failure(ErrorCode::InvalidState,"Probe submission is pending or its previous graph failed.");
    if(retained)
    {
        if(!MatchesLayout(*retained))return Failure(ErrorCode::InvalidState,"Lighting probe retained topology changed.");
        for(UINT i=0;i<4;++i)
            if((i<3||retained->upload)&&
                (!retained->imports[i]||retained->imports[i].value>graph.ResourceCount()||
                    retained->imports[i].kind!=(i<2?ResourceKind::Texture:ResourceKind::Buffer)))
                return Failure(ErrorCode::InvalidState,"Lighting probe retained import slot changed.");
        if(retained->upload&&!graph.BindPass(retained->upload_pass_index,80))
            return Failure(ErrorCode::InvalidState,"Lighting probe retained upload pass changed.");
    }
    if(layout){*layout={};layout->storage_generation=storage->generation;
        layout->upload=!storage->uploaded;}
    if(storage->uploaded && storage->upload.resource && storage->upload_completion.IsSatisfiedBy(completed))
    {
        storage->bytes-=storage->upload.allocation->GetSize(); storage->upload.Reset();
        storage->raw_upload={}; storage->ggx_upload={};
    }
    Impl::Pending pending; pending.graph=&graph; pending.storage=storage; pending.upload=!storage->uploaded;
    for(auto *resource:{&storage->radiance,&storage->prefiltered,&storage->irradiance,&storage->upload})
        if(resource->resource)
            if(auto certified=resource->EnsureNativeContract(storage->device.Get());!certified)return certified;
    UINT import_index{};bool imports_valid=true;
    auto texture=[&](AllocationResource &resource,std::string_view name) {
        const UINT index=import_index++;
        const auto current=ExternalTexture{resource.resource.Get(),resource.graph_completion,
            resource.graph_queue,{},resource.NativeContract()};
        const auto handle=retained?retained->imports[index]:graph.ImportTexture(current,resource.graph_access,name);
        if(retained)imports_valid&=graph.BindTexture(handle,current,resource.graph_access);
        if(layout)layout->imports[index]=handle;
        pending.imports.push_back({&resource,handle}); return graph.Version(handle);
    };
    auto buffer=[&](AllocationResource &resource,std::string_view name) {
        const UINT index=import_index++;
        const auto current=ExternalBuffer{resource.resource.Get(),resource.graph_completion,
            resource.graph_queue,{},resource.NativeContract()};
        const auto handle=retained?retained->imports[index]:graph.ImportBuffer(current,resource.graph_access,name);
        if(retained)imports_valid&=graph.BindBuffer(handle,current,resource.graph_access);
        if(layout)layout->imports[index]=handle;
        pending.imports.push_back({&resource,handle}); return graph.Version(handle);
    };
    auto raw=texture(storage->radiance,"ProbeRawCube"),ggx=texture(storage->prefiltered,"ProbeGgxCube");
    auto sh=buffer(storage->irradiance,"ProbeCosineSh9");
    if(!storage->uploaded)
    {
        if(!storage->upload.resource) return Failure(ErrorCode::InvalidState,"Probe upload staging is missing.");
        const auto upload=buffer(storage->upload,"ProbeUploadStaging");
        if(layout)layout->upload_pass_index=graph.PassCount();
        auto pass=retained?*graph.BindPass(retained->upload_pass_index,80):
            graph.AddPass(80,"LightingProbeUpload",QueueHint::Direct);
        if(!retained)pass.Read(upload,Access::CopySource);
        raw=retained?retained->outputs[0]:pass.Write(raw,Access::CopyDest);
        ggx=retained?retained->outputs[1]:pass.Write(ggx,Access::CopyDest);
        sh=retained?retained->outputs[2]:pass.Write(sh,Access::CopyDest);
        pass.SetExecuteChecked([storage,raw,ggx,sh,upload](RenderPassContext &context) {
            if(context.Resolve(raw.resource)!=storage->radiance.resource.Get() ||
               context.Resolve(ggx.resource)!=storage->prefiltered.resource.Get() ||
               context.Resolve(sh.resource)!=storage->irradiance.resource.Get() ||
               context.Resolve(upload.resource)!=storage->upload.resource.Get())
                return Failure(ErrorCode::InvalidState,"Probe graph resources do not match the prepared upload.");
            auto *commands=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            if(!commands) return Failure(ErrorCode::InvalidState,"Probe upload requires a native command list.");
            auto copy=[&](ID3D12Resource *target,const UploadCube &cube) {
                for(UINT i=0;i<cube.footprints.size();++i)
                {
                    D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource=storage->upload.resource.Get();
                    source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; source.PlacedFootprint=cube.footprints[i];
                    D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource=target;
                    destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; destination.SubresourceIndex=i;
                    commands->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
                }
            };
            copy(storage->radiance.resource.Get(),storage->raw_upload); copy(storage->prefiltered.resource.Get(),storage->ggx_upload);
            commands->CopyBufferRegion(storage->irradiance.resource.Get(),0,storage->upload.resource.Get(),storage->sh_offset,144);
            return Result::Success();
        });
    }
    if(!imports_valid)return Failure(ErrorCode::InvalidState,"Lighting probe retained binding changed.");
    if(!retained){graph.SetFinalAccess(raw,Access::AllShaderRead); graph.SetFinalAccess(ggx,Access::AllShaderRead); graph.SetFinalAccess(sh,Access::AllShaderRead);}
    if(layout)layout->outputs={raw,ggx,sh};
    outputs={raw,ggx,sh,storage->radiance.resource.Get(),storage->prefiltered.resource.Get(),storage->irradiance.resource.Get(),
        {storage->Srv(0),storage->Srv(1),storage->Srv(2)},storage->options};
    impl_->pending=std::move(pending); return Result::Success();
}
Result LightingProbeSystem::CompleteSubmission(const RenderGraphBuilder &graph,const CompletionSet &completion,bool success)
{
    auto &pending=impl_->pending;
    if(!pending.graph || pending.graph!=&graph)
        return Failure(ErrorCode::InvalidArgument,"Probe completion has no matching registered graph.");
    auto storage=pending.storage; storage->completion.Merge(completion);
    for(const auto &imported:pending.imports)
    {
        const auto required=graph.ResourceCompletion(imported.handle);
        storage->completion.Merge(required); imported.allocation->graph_completion.Merge(required);
        imported.allocation->graph_completion.Merge(completion);
        if(imported.allocation==&storage->upload)
        { storage->upload_completion.Merge(required); storage->upload_completion.Merge(completion); }
        if(success)
        {
            imported.allocation->graph_access=graph.FinalAccess(imported.handle);
            imported.allocation->graph_queue=graph.FinalQueue(imported.handle);
        }
    }
    if(success && pending.upload) storage->uploaded=true;
    if(!success) storage->usable=false;
    pending={}; return Result::Success();
}
Result LightingProbeSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if(impl_->pending.graph) return Failure(ErrorCode::InvalidState,"Complete the registered probe graph before reset.");
    if(impl_->storage && !impl_->storage->completion.IsSatisfiedBy(completed))
        return Failure(ErrorCode::InvalidState,"Probe reset requires completed GPU leases.");
    impl_->storage.reset(); return Result::Success();
}
} // namespace hs
