#include "renderer_ltc.hpp"
#include "d3d12_resources.hpp"
#include <hs/core/cooked_format.hpp>
#include <hs/render_data/ltc_lut.hpp>
#include <D3D12MemAlloc.h>
#include <wrl/client.h>
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
constexpr std::string_view kProduct="render/lighting/default_ltc.rpkg";
Result Failure(ErrorCode code,std::string message)
{
    return Result::Failure(code,"hs_renderer_ltc",std::move(message));
}
Result NativeFailure(std::string_view operation,HRESULT error)
{
    return Failure(ErrorCode::TaskFailed,std::string(operation)+" failed (HRESULT "+
        std::to_string(static_cast<std::int32_t>(error))+").");
}
Result Allocate(D3D12MA::Allocator *allocator,D3D12_HEAP_TYPE heap,
    D3D12_RESOURCE_DESC description,D3D12_RESOURCE_STATES initial,AllocationResource &target)
{
    D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType=heap;
    const auto native=allocator->CreateResource(&allocation,&description,initial,nullptr,&target.allocation,
        IID_PPV_ARGS(target.resource.ReleaseAndGetAddressOf()));
    return FAILED(native)?NativeFailure("Allocate LTC resource",native):Result::Success();
}
} // namespace
struct LtcLutStorage
{
    ComPtr<ID3D12Device> device;
    ComPtr<D3D12MA::Allocator> allocator;
    std::array<AllocationResource,2> textures;
    AllocationResource upload;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,2> footprints{};
    std::uint32_t descriptor_stride{};
    std::uint64_t bytes{};
    CompletionSet completion,upload_completion;
    bool uploaded{},usable{true};
    D3D12_CPU_DESCRIPTOR_HANDLE Srv(std::uint32_t index) const
    {
        auto handle=descriptors->GetCPUDescriptorHandleForHeapStart();
        handle.ptr+=SIZE_T(index)*descriptor_stride; return handle;
    }
};
struct LtcLutSystem::Impl
{
    std::shared_ptr<LtcLutStorage> storage;
    struct Import { AllocationResource *allocation{}; ResourceHandle handle; };
    struct Pending
    {
        const RenderGraphBuilder *graph{};
        std::shared_ptr<LtcLutStorage> storage;
        std::vector<Import> imports;
        bool upload{};
    } pending;
};
LtcLutSystem::LtcLutSystem():impl_(std::make_unique<Impl>()) {}
LtcLutSystem::~LtcLutSystem()=default;
std::uint64_t LtcLutCandidate::AllocatedBytes() const noexcept { return storage_?storage_->bytes:0; }
std::uint64_t LtcLutSystem::AllocatedBytes() const noexcept { return impl_->storage?impl_->storage->bytes:0; }
Result LtcLutSystem::Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,
    std::span<const std::byte> bytes,LtcLutCandidate &candidate) const
{
    candidate={};
    if(bytes.empty()) return Result::Success();
    if(!device || !allocator) return Failure(ErrorCode::InvalidArgument,"LTC preparation requires a device and allocator.");
    try
    {
        const auto package=render_data::ReadLtcLutPackage(bytes,MakeAssetId(kProduct));
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_R32G32B32A32_FLOAT};
        const auto queried=device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support));
        if(FAILED(queried)) return NativeFailure("Query LTC format",queried);
        if(!(support.Support1&D3D12_FORMAT_SUPPORT1_TEXTURE2D) || !(support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE))
            return Failure(ErrorCode::TaskFailed,"RGBA32F LTC texture sampling is unavailable.");
        auto storage=std::make_shared<LtcLutStorage>(); storage->device=device; storage->allocator=allocator;
        D3D12_RESOURCE_DESC texture{}; texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width=render_data::kLtcLutEdge; texture.Height=render_data::kLtcLutEdge;
        texture.DepthOrArraySize=1; texture.MipLevels=1; texture.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        texture.SampleDesc.Count=1;
        UINT64 end=0;
        for(std::uint32_t i=0;i<2;++i)
        {
            if(auto result=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,texture,D3D12_RESOURCE_STATE_COMMON,storage->textures[i]);!result) return result;
            storage->textures[i].graph_access=Access::Common;
            const auto base=(end+D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT-1)&~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT-1);
            UINT rows=0; UINT64 rowBytes=0,ignored=0;
            device->GetCopyableFootprints(&texture,0,1,base,&storage->footprints[i],&rows,&rowBytes,&ignored);
            end=base+UINT64(storage->footprints[i].Footprint.RowPitch)*rows;
        }
        D3D12_RESOURCE_DESC upload{}; upload.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        upload.Width=end; upload.Height=1; upload.DepthOrArraySize=1; upload.MipLevels=1;
        upload.SampleDesc.Count=1; upload.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(auto result=Allocate(allocator,D3D12_HEAP_TYPE_UPLOAD,upload,D3D12_RESOURCE_STATE_GENERIC_READ,storage->upload);!result) return result;
        storage->upload.graph_access=Access::CopySource;
        std::byte *mapped=nullptr; D3D12_RANGE empty{0,0};
        const auto mappedResult=storage->upload.resource->Map(0,&empty,reinterpret_cast<void **>(&mapped));
        if(FAILED(mappedResult)) return NativeFailure("Map LTC staging",mappedResult);
        const std::array sources{&package.matrix,&package.amplitude};
        for(std::uint32_t i=0;i<2;++i)
        {
            const auto &source=sources[i]->subresources[0]; const auto &footprint=storage->footprints[i];
            for(std::uint32_t y=0;y<source.height;++y)
                std::memcpy(mapped+footprint.Offset+SIZE_T(y)*footprint.Footprint.RowPitch,
                    source.bytes.data()+SIZE_T(y)*source.row_pitch,source.row_pitch);
        }
        D3D12_RANGE written{0,static_cast<SIZE_T>(end)}; storage->upload.resource->Unmap(0,&written);
        D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors=2;
        const auto created=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(storage->descriptors.ReleaseAndGetAddressOf()));
        if(FAILED(created)) return NativeFailure("Create LTC SRVs",created);
        storage->descriptor_stride=device->GetDescriptorHandleIncrementSize(heap.Type);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels=1;
        for(std::uint32_t i=0;i<2;++i) device->CreateShaderResourceView(storage->textures[i].resource.Get(),&view,storage->Srv(i));
        storage->textures[0].resource->SetName(L"LtcInverseMatrix"); storage->textures[1].resource->SetName(L"LtcAmplitude");
        storage->bytes=storage->upload.allocation->GetSize();
        for(const auto &resource:storage->textures) storage->bytes+=resource.allocation->GetSize();
        candidate.storage_=std::move(storage); return Result::Success();
    }
    catch(const std::exception &error) { return Failure(ErrorCode::InvalidArgument,error.what()); }
}
Result LtcLutSystem::Commit(LtcLutCandidate &&candidate,const CompletionSet &completed)
{
    if(impl_->pending.graph) return Failure(ErrorCode::InvalidState,"Complete the registered LTC graph before replacement.");
    if(impl_->storage && !impl_->storage->completion.IsSatisfiedBy(completed))
        return Failure(ErrorCode::InvalidState,"LTC replacement requires completed GPU leases.");
    impl_->storage=std::move(candidate.storage_); return Result::Success();
}
Result LtcLutSystem::Register(RenderGraphBuilder &graph,const CompletionSet &completed,LtcLutOutputs &outputs)
{
    outputs={}; auto storage=impl_->storage;
    if(impl_->pending.graph || (storage && !storage->usable))
        return Failure(ErrorCode::InvalidState,"LTC submission is pending or its previous graph failed.");
    if(!storage) return Result::Success();
    if(storage->uploaded && storage->upload.resource && storage->upload_completion.IsSatisfiedBy(completed))
    {
        storage->bytes-=storage->upload.allocation->GetSize(); storage->upload.Reset();
    }
    Impl::Pending pending; pending.graph=&graph; pending.storage=storage; pending.upload=!storage->uploaded;
    std::array<ResourceVersion,2> versions;
    for(std::uint32_t i=0;i<2;++i)
    {
        auto &resource=storage->textures[i];
        const auto handle=graph.ImportTexture({resource.resource.Get(),resource.graph_completion,resource.graph_queue},
            resource.graph_access,i==0?"LtcMatrix":"LtcAmplitude");
        pending.imports.push_back({&resource,handle}); versions[i]=graph.Version(handle);
    }
    if(!storage->uploaded)
    {
        if(!storage->upload.resource) return Failure(ErrorCode::InvalidState,"LTC staging is missing.");
        auto &resource=storage->upload;
        const auto handle=graph.ImportBuffer({resource.resource.Get(),resource.graph_completion,resource.graph_queue},resource.graph_access,"LtcUploadStaging");
        pending.imports.push_back({&resource,handle}); const auto upload=graph.Version(handle);
        auto pass=graph.AddPass(140,"LtcLutUpload",QueueHint::Direct); pass.Read(upload,Access::CopySource);
        for(auto &version:versions) version=pass.Write(version,Access::CopyDest);
        pass.SetExecuteChecked([storage,versions,upload](RenderPassContext &context) {
            if(context.Resolve(upload.resource)!=storage->upload.resource.Get())
                return Failure(ErrorCode::InvalidState,"LTC graph staging does not match prepared upload.");
            auto *commands=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            if(!commands) return Failure(ErrorCode::InvalidState,"LTC upload requires a native command list.");
            for(std::uint32_t i=0;i<2;++i)
            {
                if(context.Resolve(versions[i].resource)!=storage->textures[i].resource.Get())
                    return Failure(ErrorCode::InvalidState,"LTC graph texture does not match prepared upload.");
                D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource=storage->upload.resource.Get();
                source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; source.PlacedFootprint=storage->footprints[i];
                D3D12_TEXTURE_COPY_LOCATION target{}; target.pResource=storage->textures[i].resource.Get();
                target.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                commands->CopyTextureRegion(&target,0,0,0,&source,nullptr);
            }
            return Result::Success();
        });
    }
    for(auto version:versions) graph.SetFinalAccess(version,Access::AllShaderRead);
    outputs={versions[0],versions[1],storage->textures[0].resource.Get(),storage->textures[1].resource.Get(),
        {storage->Srv(0),storage->Srv(1)}};
    impl_->pending=std::move(pending); return Result::Success();
}
Result LtcLutSystem::CompleteSubmission(const RenderGraphBuilder &graph,const CompletionSet &completion,bool success)
{
    auto &pending=impl_->pending;
    if(!pending.graph && !impl_->storage) return Result::Success();
    if(pending.graph!=&graph) return Failure(ErrorCode::InvalidArgument,"LTC completion has no matching registered graph.");
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
Result LtcLutSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if(impl_->pending.graph) return Failure(ErrorCode::InvalidState,"Complete the registered LTC graph before reset.");
    if(impl_->storage && !impl_->storage->completion.IsSatisfiedBy(completed))
        return Failure(ErrorCode::InvalidState,"LTC reset requires completed GPU leases.");
    impl_->storage.reset(); return Result::Success();
}
} // namespace hs
