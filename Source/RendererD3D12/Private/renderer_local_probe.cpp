#include "renderer_local_probe.hpp"
#include "d3d12_resources.hpp"

#include <hs/core/cooked_format.hpp>
#include <D3D12MemAlloc.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <exception>
#include <numbers>
#include <string>
#include <utility>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr std::string_view kProduct="render/lighting/local_probes.spkg";
std::atomic<std::uint64_t> g_local_probe_generation{};
constexpr std::array<std::string_view,2> kShaders{"render/shader/local_probe_sh_cs.dxil","render/shader/local_probe_ggx_cs.dxil"};
constexpr std::uint32_t kBakeSrvCount=6;
Result Failure(ErrorCode code,std::string message)
{ return Result::Failure(code,"hs_renderer_local_probe",std::move(message)); }
Result NativeFailure(std::string_view operation,HRESULT error)
{ return Failure(ErrorCode::TaskFailed,std::string(operation)+" failed (HRESULT "+std::to_string(static_cast<std::int32_t>(error))+")."); }
std::uint64_t Align(std::uint64_t value,std::uint64_t alignment) { return (value+alignment-1)&~(alignment-1); }
D3D12_RESOURCE_DESC BufferDescription(std::uint64_t size,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; d.Width=size; d.Height=1;
    d.DepthOrArraySize=1; d.MipLevels=1; d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags=flags; return d;
}
D3D12_RESOURCE_DESC TextureDescription(std::uint32_t edge,std::uint32_t layers,std::uint32_t mips,
    DXGI_FORMAT format,D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=edge; d.Height=edge;
    d.DepthOrArraySize=static_cast<UINT16>(layers); d.MipLevels=static_cast<UINT16>(mips); d.Format=format;
    d.SampleDesc.Count=1; d.Flags=flags; return d;
}
Result Allocate(D3D12MA::Allocator *allocator,D3D12_HEAP_TYPE heap,const D3D12_RESOURCE_DESC &d,
    AllocationResource &target)
{
    D3D12MA::ALLOCATION_DESC a{}; a.HeapType=heap;
    const auto initial=heap==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:
        heap==D3D12_HEAP_TYPE_READBACK?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_COMMON;
    const auto hr=allocator->CreateResource(&a,&d,initial,
        nullptr,&target.allocation,IID_PPV_ARGS(target.resource.ReleaseAndGetAddressOf()));
    target.graph_access=heap==D3D12_HEAP_TYPE_UPLOAD?Access::CopySource:
        heap==D3D12_HEAP_TYPE_READBACK?Access::CopyDest:Access::Common;
    return FAILED(hr)?NativeFailure("Allocate local probe resource",hr):Result::Success();
}
struct CubeUpload
{
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    std::vector<UINT> rows;
};
std::uint64_t Footprints(ID3D12Device *device,const D3D12_RESOURCE_DESC &d,std::uint64_t base,CubeUpload &u)
{
    const UINT count=UINT(d.MipLevels)*d.DepthOrArraySize; u.footprints.resize(count); u.rows.resize(count);
    std::vector<UINT64> rowBytes(count); UINT64 ignored{};
    device->GetCopyableFootprints(&d,0,count,base,u.footprints.data(),u.rows.data(),rowBytes.data(),&ignored);
    auto end=base;
    for(UINT i=0;i<count;++i) end=std::max(end,u.footprints[i].Offset+UINT64(u.footprints[i].Footprint.RowPitch)*u.rows[i]);
    return end;
}
struct alignas(16) GpuProbeRecord
{
    Float4 position_weight,influence_min_fade,influence_max_bias,projection_min_visibility_fade,
        projection_max_capture_distance,parameters,reserved0,reserved1;
};
static_assert(sizeof(GpuProbeRecord)==kLocalProbeGpuRecordStride);
struct alignas(16) BakeConstants
{
    std::uint32_t edge{},mip_count{},probe_count{},sample_count{};
    std::uint32_t write_mip{},write_edge{},write_probe{},reserved1{};
};
bool ValidRevisions(render_data::SceneProbeRevisions r)
{ return r.environment && r.material && r.light && r.static_layout; }
LocalProbeCaptureView CaptureView(const std::shared_ptr<const StaticRenderScene> &scene,
    render_data::SceneProbeRevisions revisions,const render_data::SceneProbeRecord &probe,
    std::uint32_t index,std::uint32_t face,std::uint32_t edge)
{
    using namespace DirectX;
    constexpr XMFLOAT3 directions[6]{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    constexpr XMFLOAT3 ups[6]{{0,1,0},{0,1,0},{0,0,-1},{0,0,1},{0,1,0},{0,1,0}};
    LocalProbeCaptureView v; v.scene=scene; v.revisions=revisions; v.probe_index=index; v.face=face; v.edge=edge;
    v.position=probe.position; v.far_distance=probe.capture_distance; v.array_slice=index*6+face;
    const auto position=XMVectorSet(probe.position.x,probe.position.y,probe.position.z,1);
    const auto view=XMMatrixLookToLH(position,XMLoadFloat3(&directions[face]),XMLoadFloat3(&ups[face]));
    const auto projection=XMMatrixPerspectiveFovLH(std::numbers::pi_v<float>/2,1,v.far_distance,v.near_distance);
    XMStoreFloat4x4(&v.world_to_view,view); XMStoreFloat4x4(&v.projection,projection);
    XMStoreFloat4x4(&v.view_projection,view*projection); return v;
}
} // namespace

struct LocalProbeStorage
{
    ComPtr<ID3D12Device> device; ComPtr<D3D12MA::Allocator> allocator;
    render_data::SceneProbeDescription description; std::vector<render_data::SceneProbeRecord> probes;
    std::vector<std::string> dependencies;
    AllocationResource raw,ggx,sh,distance,records,disabled_records,depth,upload,constants,export_readback;
    ComPtr<ID3D12DescriptorHeap> exports,bake_descriptors,rtvs,dsv;
    ComPtr<ID3D12RootSignature> root; std::array<ComPtr<ID3D12PipelineState>,2> pipelines;
    CubeUpload raw_upload,ggx_upload,distance_upload;
    std::uint64_t sh_offset{},record_offset{},disabled_offset{},bytes{};
    std::uint64_t generation{};
    std::uint32_t mip_count{},srv_stride{},rtv_stride{},captured_views{};
    std::shared_ptr<const StaticRenderScene> scene;
    render_data::SceneProbeRevisions revisions;
    std::array<std::byte,32> capture_identity{};
    std::array<std::byte,32> finished_capture_identity{};
    CompletionSet completion,capture_completion,bake_completion;
    bool capture_sources_ready{};
    bool capture_ready{},uploaded{},package_baked{},bake_submitted{},published{},current_request{},usable{true};
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap *heap,std::uint32_t index,std::uint32_t stride) const
    { auto h=heap->GetCPUDescriptorHandleForHeapStart(); h.ptr+=SIZE_T(index)*stride; return h; }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(std::uint32_t index) const
    { auto h=bake_descriptors->GetGPUDescriptorHandleForHeapStart(); h.ptr+=UINT64(index)*srv_stride; return h; }
    D3D12_CPU_DESCRIPTOR_HANDLE Export(std::uint32_t index) const { return Cpu(exports.Get(),index,srv_stride); }
};
struct LocalProbeSystem::Impl
{
    std::shared_ptr<LocalProbeStorage> storage;
    struct Import { AllocationResource *allocation{}; ResourceHandle handle; };
    struct Pending
    {
        const RenderGraphBuilder *graph{}; std::shared_ptr<LocalProbeStorage> storage;
        std::vector<Import> imports; std::shared_ptr<const StaticRenderScene> scene;
        render_data::SceneProbeRevisions revisions;
        std::array<std::byte,32> capture_identity{};
        std::uint32_t captured_views{}; bool upload{},finished_bake{};
    } pending;
};
LocalProbeSystem::LocalProbeSystem():impl_(std::make_unique<Impl>()) {}
LocalProbeSystem::~LocalProbeSystem()=default;
std::uint64_t LocalProbeCandidate::AllocatedBytes() const noexcept { return storage_?storage_->bytes:0; }
std::uint64_t LocalProbeSystem::AllocatedBytes() const noexcept { return impl_->storage?impl_->storage->bytes:0; }

namespace
{
Result PrepareCaptureResources(const std::shared_ptr<LocalProbeStorage> &s,const LocalProbeSystem::ShaderReader &reader)
{
    auto *device=s->device.Get(); auto *allocator=s->allocator.Get();
    const auto layers=static_cast<std::uint32_t>(s->probes.size()*6),edge=s->description.edge;
    if(s->package_baked)
    {
        // A stale baked image has no live readers before it becomes writable capture storage.
        for(auto format:{DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32_FLOAT})
        {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
            const auto hr=device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support));
            if(FAILED(hr)) return NativeFailure("Query local probe capture format",hr);
            if(!(support.Support1&D3D12_FORMAT_SUPPORT1_RENDER_TARGET) ||
                (format==DXGI_FORMAT_R32G32B32A32_FLOAT && !(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)))
                return Failure(ErrorCode::TaskFailed,"Local probe capture format is unavailable.");
        }
        s->raw.Reset(); s->ggx.Reset(); s->distance.Reset(); s->sh.Reset();
        if(auto r=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,TextureDescription(edge,layers,1,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),s->raw);!r) return r;
        if(auto r=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,TextureDescription(edge,layers,s->mip_count,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),s->ggx);!r) return r;
        if(auto r=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,TextureDescription(edge,layers,1,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),s->distance);!r) return r;
        if(auto r=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,BufferDescription(s->probes.size()*9*16,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),s->sh);!r) return r;
        HS_RENDERER_SET_DEBUG_NAME(s->raw.resource.Get(), L"LocalProbe.SceneLinearRawCubeArray"); HS_RENDERER_SET_DEBUG_NAME(s->ggx.resource.Get(), L"LocalProbe.GgxCubeArray");
        HS_RENDERER_SET_DEBUG_NAME(s->distance.resource.Get(), L"LocalProbe.FirstHitWorldDistance"); HS_RENDERER_SET_DEBUG_NAME(s->sh.resource.Get(), L"LocalProbe.CosineSh9");
        D3D12_SHADER_RESOURCE_VIEW_DESC cube{}; cube.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        cube.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBEARRAY; cube.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        cube.TextureCubeArray.NumCubes=static_cast<UINT>(s->probes.size()); cube.TextureCubeArray.MipLevels=1;
        device->CreateShaderResourceView(s->raw.resource.Get(),&cube,s->Export(0));
        cube.TextureCubeArray.MipLevels=s->mip_count;
        device->CreateShaderResourceView(s->ggx.resource.Get(),&cube,s->Export(1));
        D3D12_SHADER_RESOURCE_VIEW_DESC buffer{}; buffer.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
        buffer.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        buffer.Buffer.NumElements=static_cast<UINT>(s->probes.size()*9); buffer.Buffer.StructureByteStride=16;
        device->CreateShaderResourceView(s->sh.resource.Get(),&buffer,s->Export(2));
        D3D12_SHADER_RESOURCE_VIEW_DESC distance{}; distance.Format=DXGI_FORMAT_R32_FLOAT;
        distance.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY; distance.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        distance.Texture2DArray.ArraySize=layers; distance.Texture2DArray.MipLevels=1;
        device->CreateShaderResourceView(s->distance.resource.Get(),&distance,s->Export(3));
        s->uploaded=false;
    }
    if(auto r=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,TextureDescription(edge,1,1,DXGI_FORMAT_D32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),s->depth);!r) return r;
    std::byte *mapped{}; D3D12_RANGE empty{0,0}; HRESULT hr{};
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    if(auto r=Allocate(allocator,D3D12_HEAP_TYPE_UPLOAD,BufferDescription(UINT64(s->mip_count)*s->probes.size()*256),s->constants);!r) return r;
    hr=s->constants.resource->Map(0,&empty,reinterpret_cast<void **>(&mapped)); if(FAILED(hr)) return NativeFailure("Map local probe constants",hr);
    std::memset(mapped,0,SIZE_T(s->mip_count)*s->probes.size()*256);
    for(std::uint32_t probe=0;probe<s->probes.size();++probe) for(std::uint32_t mip=0;mip<s->mip_count;++mip)
    { const BakeConstants c{edge,s->mip_count,static_cast<std::uint32_t>(s->probes.size()),s->description.ggx_samples,mip,std::max(1u,edge>>mip),probe,0}; std::memcpy(mapped+(SIZE_T(probe)*s->mip_count+mip)*256,&c,sizeof(c)); }
    s->constants.resource->Unmap(0,nullptr);
    heap.NumDescriptors=kBakeSrvCount+2*s->mip_count; heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&s->bake_descriptors)); if(FAILED(hr)) return NativeFailure("Create local probe bake descriptors",hr);
    device->CopyDescriptorsSimple(5,s->Cpu(s->bake_descriptors.Get(),0,s->srv_stride),s->Export(0),heap.Type);
    D3D12_SHADER_RESOURCE_VIEW_DESC arrayView{}; arrayView.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    arrayView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY; arrayView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    arrayView.Texture2DArray.MipLevels=1; arrayView.Texture2DArray.ArraySize=layers;
    device->CreateShaderResourceView(s->raw.resource.Get(),&arrayView,s->Cpu(s->bake_descriptors.Get(),5,s->srv_stride));
    for(std::uint32_t mip=0;mip<s->mip_count;++mip)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{}; u.Format=DXGI_FORMAT_R32G32B32A32_FLOAT; u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        u.Texture2DArray.MipSlice=mip; u.Texture2DArray.ArraySize=layers;
        device->CreateUnorderedAccessView(s->ggx.resource.Get(),nullptr,&u,s->Cpu(s->bake_descriptors.Get(),kBakeSrvCount+2*mip,s->srv_stride));
        u={}; u.ViewDimension=D3D12_UAV_DIMENSION_BUFFER; u.Buffer.NumElements=static_cast<UINT>(s->probes.size()*9); u.Buffer.StructureByteStride=16;
        device->CreateUnorderedAccessView(s->sh.resource.Get(),nullptr,&u,s->Cpu(s->bake_descriptors.Get(),kBakeSrvCount+2*mip+1,s->srv_stride));
    }
    heap={}; heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors=layers*2;
    hr=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&s->rtvs)); if(FAILED(hr)) return NativeFailure("Create local probe RTVs",hr);
    s->rtv_stride=device->GetDescriptorHandleIncrementSize(heap.Type);
    for(std::uint32_t layer=0;layer<layers;++layer)
    {
        D3D12_RENDER_TARGET_VIEW_DESC v{}; v.Format=DXGI_FORMAT_R32G32B32A32_FLOAT; v.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        v.Texture2DArray.FirstArraySlice=layer; v.Texture2DArray.ArraySize=1;
        device->CreateRenderTargetView(s->raw.resource.Get(),&v,s->Cpu(s->rtvs.Get(),2*layer,s->rtv_stride));
        v.Format=DXGI_FORMAT_R32_FLOAT; device->CreateRenderTargetView(s->distance.resource.Get(),&v,s->Cpu(s->rtvs.Get(),2*layer+1,s->rtv_stride));
    }
    heap={}; heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV; heap.NumDescriptors=1;
    hr=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&s->dsv)); if(FAILED(hr)) return NativeFailure("Create local probe DSV",hr);
    D3D12_DEPTH_STENCIL_VIEW_DESC depthView{}; depthView.Format=DXGI_FORMAT_D32_FLOAT; depthView.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(s->depth.resource.Get(),&depthView,s->dsv->GetCPUDescriptorHandleForHeapStart());
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,kBakeSrvCount,0,23,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,23,0}};
    D3D12_ROOT_PARAMETER roots[3]{}; roots[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV; roots[0].Descriptor={0,23};
    for(UINT i=0;i<2;++i) { roots[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; roots[i+1].DescriptorTable={1,&ranges[i]}; }
    D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS; sampler.MaxAnisotropy=1;
    sampler.ShaderRegister=0; sampler.RegisterSpace=23; sampler.MaxLOD=D3D12_FLOAT32_MAX; sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rootDesc{}; rootDesc.NumParameters=3; rootDesc.pParameters=roots; rootDesc.NumStaticSamplers=1; rootDesc.pStaticSamplers=&sampler;
    ComPtr<ID3DBlob> signature,diagnostics; hr=D3D12SerializeRootSignature(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&diagnostics);
    if(FAILED(hr)) return NativeFailure("Serialize local probe bake root",hr);
    hr=device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&s->root));
    if(FAILED(hr)) return NativeFailure("Create local probe bake root",hr);
    for(std::uint32_t i=0;i<2;++i)
    {
        std::vector<std::byte> code; if(auto r=reader(kShaders[i],code);!r) return r;
        if(code.empty()) return Failure(ErrorCode::InvalidArgument,"Local probe bake shader is empty: "+std::string(kShaders[i]));
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{}; desc.pRootSignature=s->root.Get(); desc.CS={code.data(),code.size()};
        hr=device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&s->pipelines[i])); if(FAILED(hr)) return NativeFailure("Create local probe bake pipeline",hr);
    }
    s->capture_ready=true;
    return Result::Success();
}
} // namespace

Result LocalProbeSystem::Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,
    std::span<const std::byte> bytes,const ShaderReader &reader,LocalProbeCandidate &candidate) const
{
    candidate={}; if(!device || !allocator || !reader) return Failure(ErrorCode::InvalidArgument,"Local probe preparation needs device, allocator and shader reader.");
    try
    {
        const auto p=render_data::ReadSceneProbePackage(bytes,MakeAssetId(kProduct));
        const bool baked=p.payload==render_data::SceneProbePayload::Baked;
        for(auto format:{DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32_FLOAT})
        {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format}; auto hr=device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support));
            if(FAILED(hr)) return NativeFailure("Query local probe format",hr);
            if(!(support.Support1&D3D12_FORMAT_SUPPORT1_TEXTURECUBE) || !(support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) ||
                (!baked && (!(support.Support1&D3D12_FORMAT_SUPPORT1_RENDER_TARGET) ||
                (format==DXGI_FORMAT_R32G32B32A32_FLOAT && !(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)))))
                return Failure(ErrorCode::TaskFailed,"Required local probe HDR/distance format support is unavailable.");
        }
        auto s=std::make_shared<LocalProbeStorage>(); s->device=device; s->allocator=allocator;
        s->description=p.description; s->probes=p.probes; s->dependencies=p.dependencies;
        s->mip_count=std::bit_width(p.description.edge); s->package_baked=p.payload==render_data::SceneProbePayload::Baked;
        const auto layers=static_cast<std::uint32_t>(s->probes.size()*6),edge=s->description.edge;
        const auto rawDesc=TextureDescription(edge,layers,1,DXGI_FORMAT_R32G32B32A32_FLOAT,(baked?D3D12_RESOURCE_FLAG_NONE:D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET));
        const auto ggxDesc=TextureDescription(edge,layers,s->mip_count,DXGI_FORMAT_R32G32B32A32_FLOAT,(baked?D3D12_RESOURCE_FLAG_NONE:D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
        const auto distanceDesc=TextureDescription(edge,layers,1,DXGI_FORMAT_R32_FLOAT,(baked?D3D12_RESOURCE_FLAG_NONE:D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET));
        if(auto r=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,rawDesc,s->raw);!r) return r;
        if(auto r=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,ggxDesc,s->ggx);!r) return r;
        if(auto r=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,distanceDesc,s->distance);!r) return r;
        if(auto r=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,BufferDescription(s->probes.size()*9*16,baked?D3D12_RESOURCE_FLAG_NONE:D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),s->sh);!r) return r;
        if(auto r=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,BufferDescription(s->probes.size()*kLocalProbeGpuRecordStride),s->records);!r) return r;
        if(auto r=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,BufferDescription(kLocalProbeGpuRecordStride),s->disabled_records);!r) return r;
        std::uint64_t end=0;
        if(s->package_baked)
        {
            end=Footprints(device,rawDesc,0,s->raw_upload);
            end=Footprints(device,ggxDesc,Align(end,D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT),s->ggx_upload);
            end=Footprints(device,distanceDesc,Align(end,D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT),s->distance_upload);
            s->sh_offset=Align(end,16); end=s->sh_offset+p.cosine_sh.size();
        }
        s->record_offset=Align(end,16); s->disabled_offset=s->record_offset+s->probes.size()*kLocalProbeGpuRecordStride;
        if(auto r=Allocate(allocator,D3D12_HEAP_TYPE_UPLOAD,BufferDescription(s->disabled_offset+kLocalProbeGpuRecordStride),s->upload);!r) return r;
        std::byte *mapped{}; D3D12_RANGE empty{0,0}; auto hr=s->upload.resource->Map(0,&empty,reinterpret_cast<void **>(&mapped));
        if(FAILED(hr)) return NativeFailure("Map local probe staging",hr);
        std::memset(mapped,0,static_cast<std::size_t>(s->disabled_offset+kLocalProbeGpuRecordStride));
        if(s->package_baked)
        {
            auto copy=[&](const render_data::TexturePackageView &cube,const CubeUpload &upload) {
                for(std::size_t i=0;i<cube.subresources.size();++i)
                    for(std::uint32_t y=0;y<cube.subresources[i].height;++y)
                        std::memcpy(mapped+upload.footprints[i].Offset+std::size_t(y)*upload.footprints[i].Footprint.RowPitch,
                            cube.subresources[i].bytes.data()+std::size_t(y)*cube.subresources[i].row_pitch,cube.subresources[i].row_pitch);
            };
            copy(p.radiance,s->raw_upload); copy(p.prefiltered,s->ggx_upload);
            for(std::uint32_t layer=0;layer<layers;++layer) for(std::uint32_t y=0;y<edge;++y)
                std::memcpy(mapped+s->distance_upload.footprints[layer].Offset+std::size_t(y)*s->distance_upload.footprints[layer].Footprint.RowPitch,
                    p.first_hit_distance.data()+(std::size_t(layer)*edge*edge+std::size_t(y)*edge)*4,std::size_t(edge)*4);
            std::memcpy(mapped+s->sh_offset,p.cosine_sh.data(),p.cosine_sh.size());
        }
        for(std::uint32_t i=0;i<s->probes.size();++i)
        {
            const auto &v=s->probes[i]; const GpuProbeRecord record{{v.position.x,v.position.y,v.position.z,v.weight},
                {v.influence_min.x,v.influence_min.y,v.influence_min.z,v.influence_fade},
                {v.influence_max.x,v.influence_max.y,v.influence_max.z,v.visibility_bias},
                {v.projection_min.x,v.projection_min.y,v.projection_min.z,v.visibility_fade},
                {v.projection_max.x,v.projection_max.y,v.projection_max.z,v.capture_distance},
                {float(s->mip_count-1),float(i),1.0f,float(s->probes.size())},{},{}};
            std::memcpy(mapped+s->record_offset+std::size_t(i)*kLocalProbeGpuRecordStride,&record,sizeof(record));
        }
        s->upload.resource->Unmap(0,nullptr);
        D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors=10;
        hr=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&s->exports)); if(FAILED(hr)) return NativeFailure("Create local probe exports",hr);
        s->srv_stride=device->GetDescriptorHandleIncrementSize(heap.Type);
        auto cubeView=[&](ID3D12Resource *resource,DXGI_FORMAT format,std::uint32_t mips,D3D12_CPU_DESCRIPTOR_HANDLE handle) {
            D3D12_SHADER_RESOURCE_VIEW_DESC v{}; v.Format=format; v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; v.TextureCubeArray.MipLevels=mips;
            v.TextureCubeArray.NumCubes=static_cast<UINT>(s->probes.size()); device->CreateShaderResourceView(resource,&v,handle);
        };
        auto bufferView=[&](ID3D12Resource *resource,UINT count,UINT stride,D3D12_CPU_DESCRIPTOR_HANDLE handle) {
            D3D12_SHADER_RESOURCE_VIEW_DESC v{}; v.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
            v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; v.Buffer.NumElements=count; v.Buffer.StructureByteStride=stride;
            device->CreateShaderResourceView(resource,&v,handle);
        };
        cubeView(s->raw.resource.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT,1,s->Export(0));
        cubeView(s->ggx.resource.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT,s->mip_count,s->Export(1));
        bufferView(s->sh.resource.Get(),static_cast<UINT>(s->probes.size()*9),16,s->Export(2));
        auto distanceView=[&](ID3D12Resource *resource,D3D12_CPU_DESCRIPTOR_HANDLE handle) {
            D3D12_SHADER_RESOURCE_VIEW_DESC v{}; v.Format=DXGI_FORMAT_R32_FLOAT; v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; v.Texture2DArray.MipLevels=1;
            v.Texture2DArray.ArraySize=layers; device->CreateShaderResourceView(resource,&v,handle);
        };
        distanceView(s->distance.resource.Get(),s->Export(3));
        bufferView(s->records.resource.Get(),static_cast<UINT>(s->probes.size()),kLocalProbeGpuRecordStride,s->Export(4));
        cubeView(nullptr,DXGI_FORMAT_R32G32B32A32_FLOAT,1,s->Export(5)); cubeView(nullptr,DXGI_FORMAT_R32G32B32A32_FLOAT,1,s->Export(6));
        bufferView(nullptr,1,16,s->Export(7)); distanceView(nullptr,s->Export(8));
        bufferView(s->disabled_records.resource.Get(),1,kLocalProbeGpuRecordStride,s->Export(9));
        if(!s->package_baked)
            if(auto ready=PrepareCaptureResources(s,reader);!ready) return ready;
        HS_RENDERER_SET_DEBUG_NAME(s->raw.resource.Get(), L"LocalProbe.SceneLinearRawCubeArray"); HS_RENDERER_SET_DEBUG_NAME(s->ggx.resource.Get(), L"LocalProbe.GgxCubeArray");
        HS_RENDERER_SET_DEBUG_NAME(s->distance.resource.Get(), L"LocalProbe.FirstHitWorldDistance"); HS_RENDERER_SET_DEBUG_NAME(s->sh.resource.Get(), L"LocalProbe.CosineSh9");
        for(auto *r:{&s->raw,&s->ggx,&s->sh,&s->distance,&s->records,&s->disabled_records,&s->depth,&s->upload,&s->constants}) if(r->allocation) s->bytes+=r->allocation->GetSize();
        candidate.storage_=std::move(s); return Result::Success();
    }
    catch(const std::exception &e) { return Failure(ErrorCode::InvalidArgument,e.what()); }
}

Result LocalProbeSystem::Commit(LocalProbeCandidate &&candidate,const CompletionSet &completed)
{
    if(!candidate.storage_) return Failure(ErrorCode::InvalidArgument,"Local probe candidate is empty.");
    if(impl_->pending.graph || (impl_->storage && !impl_->storage->completion.IsSatisfiedBy(completed)))
        return Failure(ErrorCode::InvalidState,"Local probe replacement requires completed graph/resource leases.");
    candidate.storage_->generation=g_local_probe_generation.fetch_add(1,std::memory_order_relaxed)+1;
    impl_->storage=std::move(candidate.storage_); return Result::Success();
}

Result LocalProbeSystem::PrepareCapture(const ShaderReader &reader,const CompletionSet &completed)
{
    auto s=impl_->storage;
    if(!s || !s->usable || impl_->pending.graph)
        return Failure(ErrorCode::InvalidState,"Local probe capture preparation requires usable idle storage.");
    if(s->capture_ready) return Result::Success();
    if(!s->completion.IsSatisfiedBy(completed))
        return Failure(ErrorCode::InvalidState,"Local probe capture preparation requires all previous readers to finish.");
    s->usable=false;
    if(auto result=PrepareCaptureResources(s,reader);!result) return result;
    s->bytes=0;
    for(auto *r:{&s->raw,&s->ggx,&s->sh,&s->distance,&s->records,&s->disabled_records,&s->depth,&s->upload,&s->constants})
        if(r->allocation) s->bytes+=r->allocation->GetSize();
    s->usable=true;
    s->generation=g_local_probe_generation.fetch_add(1,std::memory_order_relaxed)+1;
    return Result::Success();
}

Result LocalProbeSystem::ExportBakedPackage(RenderGraphQueueExecutor &executor,TransientResourcePool &pool,
    std::uint32_t frame_slot,BarrierMode barriers,std::vector<std::byte> &package_bytes)
{
    package_bytes.clear(); auto s=impl_->storage;
    if(impl_->pending.graph || !s || !s->usable || !s->current_request || !s->bake_submitted ||
        (!s->package_baked&&s->finished_capture_identity!=s->capture_identity) ||
        !s->scene || s->captured_views!=s->probes.size()*6)
        return Failure(ErrorCode::InvalidState,"Scene-probe export requires a complete current native capture (views=" +
            std::to_string(s ? s->captured_views : 0) + "/" + std::to_string(s ? s->probes.size()*6 : 0) +
            ", submitted=" + std::to_string(s && s->bake_submitted) + ", published=" +
            std::to_string(s && s->published) + ").");
    const auto completed=executor.Completed();
    if(!s->completion.IsSatisfiedBy(completed) || !s->capture_completion.IsSatisfiedBy(completed) ||
        !s->bake_completion.IsSatisfiedBy(completed))
        return Failure(ErrorCode::InvalidState,"Scene-probe export requires completion of every capture, filter and reader.");
    const auto &description=s->description;
    const auto layers=static_cast<std::uint32_t>(s->probes.size()*6);
    const auto raw_desc=s->raw.resource->GetDesc(),ggx_desc=s->ggx.resource->GetDesc();
    const auto sh_desc=s->sh.resource->GetDesc(),distance_desc=s->distance.resource->GetDesc();
    if(raw_desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || raw_desc.Width!=description.edge || raw_desc.Height!=description.edge ||
        raw_desc.DepthOrArraySize!=layers || raw_desc.MipLevels!=1 || raw_desc.Format!=DXGI_FORMAT_R32G32B32A32_FLOAT || raw_desc.SampleDesc.Count!=1 ||
        ggx_desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || ggx_desc.Width!=description.edge || ggx_desc.Height!=description.edge ||
        ggx_desc.DepthOrArraySize!=layers || ggx_desc.MipLevels!=s->mip_count || ggx_desc.Format!=DXGI_FORMAT_R32G32B32A32_FLOAT || ggx_desc.SampleDesc.Count!=1 ||
        sh_desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER || sh_desc.Width!=s->probes.size()*9*16 ||
        distance_desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || distance_desc.Width!=description.edge || distance_desc.Height!=description.edge ||
        distance_desc.DepthOrArraySize!=layers || distance_desc.MipLevels!=1 || distance_desc.Format!=DXGI_FORMAT_R32_FLOAT || distance_desc.SampleDesc.Count!=1)
        return Failure(ErrorCode::InvalidState,"Published scene-probe native resources have an unexpected payload layout.");

    CubeUpload raw_upload,ggx_upload,distance_upload;
    auto end=Footprints(s->device.Get(),raw_desc,0,raw_upload);
    end=Footprints(s->device.Get(),ggx_desc,Align(end,D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT),ggx_upload);
    end=Footprints(s->device.Get(),distance_desc,Align(end,D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT),distance_upload);
    const auto sh_offset=Align(end,16); end=sh_offset+sh_desc.Width;
    if(!end || end>256ull*1024*1024)
        return Failure(ErrorCode::InvalidState,"Scene-probe readback exceeds the bounded package size.");
    if(s->export_readback.allocation)
        return Failure(ErrorCode::InvalidState,"A prior scene-probe export still owns readback storage; reset after its completion before retrying.");
    if(auto allocated=Allocate(s->allocator.Get(),D3D12_HEAP_TYPE_READBACK,BufferDescription(end),s->export_readback);!allocated)
    {
        s->export_readback.Reset();
        return allocated;
    }

    RenderGraphBuilder graph;
    const auto raw=graph.ImportTexture({s->raw.resource.Get(),s->raw.graph_completion,s->raw.graph_queue},
        s->raw.graph_access,"SceneProbe.ExportRaw");
    const auto ggx=graph.ImportTexture({s->ggx.resource.Get(),s->ggx.graph_completion,s->ggx.graph_queue},
        s->ggx.graph_access,"SceneProbe.ExportPrefiltered");
    const auto sh=graph.ImportBuffer({s->sh.resource.Get(),s->sh.graph_completion,s->sh.graph_queue},
        s->sh.graph_access,"SceneProbe.ExportCosineSh");
    const auto distance=graph.ImportTexture({s->distance.resource.Get(),s->distance.graph_completion,s->distance.graph_queue},
        s->distance.graph_access,"SceneProbe.ExportDistance");
    const auto output=graph.ImportBuffer({s->export_readback.resource.Get(),s->export_readback.graph_completion,QueueHint::Direct},Access::CopyDest,
        "SceneProbe.ExportReadback");
    auto pass=graph.AddPass(189,"SceneProbePackageReadback",QueueHint::Direct);
    pass.Read(graph.Version(raw),Access::CopySource); pass.Read(graph.Version(ggx),Access::CopySource);
    pass.Read(graph.Version(sh),Access::CopySource); pass.Read(graph.Version(distance),Access::CopySource);
    (void)pass.Write(graph.Version(output),Access::CopyDest);
    graph.SetFinalAccess(graph.Version(raw),Access::AllShaderRead);
    graph.SetFinalAccess(graph.Version(ggx),Access::AllShaderRead);
    graph.SetFinalAccess(graph.Version(sh),Access::AllShaderRead);
    graph.SetFinalAccess(graph.Version(distance),Access::AllShaderRead);
    const auto raw_resource=s->raw.resource,ggx_resource=s->ggx.resource,sh_resource=s->sh.resource,distance_resource=s->distance.resource;
    pass.SetExecuteChecked([=](RenderPassContext &context) {
        auto *commands=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        auto *target=static_cast<ID3D12Resource *>(context.Resolve(output));
        if(!commands || !target) return Failure(ErrorCode::InvalidState,"Scene-probe package copy pass is missing native resources.");
        auto copy=[&](ID3D12Resource *source,const CubeUpload &upload) {
            for(UINT subresource=0;subresource<upload.footprints.size();++subresource)
            {
                D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource=source;
                from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex=subresource;
                D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource=target;
                to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint=upload.footprints[subresource];
                commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            }
        };
        copy(raw_resource.Get(),raw_upload); copy(ggx_resource.Get(),ggx_upload); copy(distance_resource.Get(),distance_upload);
        commands->CopyBufferRegion(target,sh_offset,sh_resource.Get(),0,sh_desc.Width);
        return Result::Success();
    });
    if(auto prepared=graph.Prepare(pool);!prepared)
    {
        s->export_readback.Reset();
        return prepared;
    }
    CompletionSet completion;
    const auto retain_submission = [&](bool restore_final_access) {
        s->completion.Merge(completion);
        const auto retain_input = [&](AllocationResource &resource,ResourceHandle handle) {
            const auto use=graph.ResourceCompletion(handle);
            resource.graph_completion.Merge(use);
            resource.graph_completion.Merge(completion);
            s->completion.Merge(use);
            if(restore_final_access)
            {
                resource.graph_access=graph.FinalAccess(handle);
                resource.graph_queue=graph.FinalQueue(handle);
            }
        };
        retain_input(s->raw,raw); retain_input(s->ggx,ggx);
        retain_input(s->sh,sh); retain_input(s->distance,distance);
        const auto output_use=graph.ResourceCompletion(output);
        s->export_readback.graph_completion.Merge(output_use);
        s->export_readback.graph_completion.Merge(completion);
        s->completion.Merge(output_use);
    };
    if(auto executed=executor.Execute(graph,frame_slot,barriers,completion);!executed)
    {
        retain_submission(false);
        s->published=false; s->usable=false; s->bake_submitted=false;
        return executed;
    }
    if(auto waited=executor.Wait(completion);!waited)
    {
        retain_submission(false);
        s->published=false; s->usable=false; s->bake_submitted=false;
        return waited;
    }
    retain_submission(true);

    std::vector<Float4> radiance,prefiltered,cosine_sh; std::vector<float> first_hit_distance;
    try
    {
        const auto raw_texels=std::size_t(description.edge)*description.edge*layers;
        radiance.resize(raw_texels); first_hit_distance.resize(raw_texels);
        std::size_t ggx_texels{};
        for(std::uint32_t mip=0;mip<s->mip_count;++mip)
            ggx_texels+=std::size_t(layers)*std::max(1u,description.edge>>mip)*std::max(1u,description.edge>>mip);
        prefiltered.resize(ggx_texels);
        cosine_sh.resize(s->probes.size()*9);
    }
    catch(...)
    {
        s->export_readback.Reset();
        throw;
    }
    D3D12_RANGE read_range{0,static_cast<SIZE_T>(end)}; std::byte *mapped{};
    const auto map_result=s->export_readback.resource->Map(0,&read_range,reinterpret_cast<void **>(&mapped));
    if(FAILED(map_result))
    {
        s->export_readback.Reset();
        return NativeFailure("Map scene-probe package readback",map_result);
    }
    auto copy_rgba=[&](const CubeUpload &upload,std::uint32_t subresource,std::uint32_t edge,std::vector<Float4> &values,std::size_t &offset) {
        const auto &footprint=upload.footprints[subresource];
        for(std::uint32_t y=0;y<edge;++y)
            for(std::uint32_t x=0;x<edge;++x)
                std::memcpy(&values[offset++],mapped+footprint.Offset+std::size_t(y)*footprint.Footprint.RowPitch+std::size_t(x)*16,16);
    };
    std::size_t raw_offset{},distance_offset{};
    for(std::uint32_t layer=0;layer<layers;++layer)
    {
        copy_rgba(raw_upload,layer,description.edge,radiance,raw_offset);
        const auto &footprint=distance_upload.footprints[layer];
        for(std::uint32_t y=0;y<description.edge;++y)
            std::memcpy(first_hit_distance.data()+distance_offset+y*description.edge,
                mapped+footprint.Offset+std::size_t(y)*footprint.Footprint.RowPitch,std::size_t(description.edge)*sizeof(float));
        distance_offset+=std::size_t(description.edge)*description.edge;
    }
    std::size_t ggx_offset{};
    for(std::uint32_t layer=0;layer<layers;++layer)
        for(std::uint32_t mip=0;mip<s->mip_count;++mip)
            copy_rgba(ggx_upload,layer*s->mip_count+mip,std::max(1u,description.edge>>mip),prefiltered,ggx_offset);
    std::memcpy(cosine_sh.data(),mapped+sh_offset,static_cast<std::size_t>(sh_desc.Width));
    D3D12_RANGE no_write{}; s->export_readback.resource->Unmap(0,&no_write);
    s->export_readback.Reset();
    auto baked_description=description; baked_description.revisions=s->revisions;
    try
    {
        package_bytes=render_data::BuildSceneProbePackage(MakeAssetId(kProduct),s->dependencies,baked_description,
            s->probes,{radiance,prefiltered,cosine_sh,first_hit_distance});
    }
    catch(const std::exception &error) { return Failure(ErrorCode::InvalidState,error.what()); }
    return Result::Success();
}

Result LocalProbeSystem::PrepareFrameRecordData(const LocalProbeInputs &in,LocalProbeFramePlan &plan) const
{
    plan={};const auto s=impl_->storage;
    if(!s||impl_->pending.graph||!s->usable)
        return Failure(ErrorCode::InvalidState,"Local probes are unprepared, pending or require reset after failed submission.");
    if((in.scene&&!ValidRevisions(in.revisions))||in.max_cube_views>6||
        (!in.scene&&(in.max_cube_views||in.draw_static||in.revisions.environment||in.revisions.material||
            in.revisions.light||in.revisions.static_layout)))
        return Failure(ErrorCode::InvalidArgument,"Local probe record plan requires an immutable scene and at most six cube views.");
    if(in.max_cube_views&&in.draw_static&&(in.scene_reads.empty()||!s->capture_ready))
        return Failure(ErrorCode::InvalidState,"Local probe capture needs prepared resources and scene reads.");
    for(const auto &read:in.scene_reads)
        if(!read.version.resource||read.access==Access::Undefined||read.access==Access::Common||
            read.access==Access::RenderTarget||read.access==Access::DepthWrite||
            read.access==Access::UnorderedWrite||read.access==Access::CopyDest)
            return Failure(ErrorCode::InvalidArgument,"Local probe scene dependency is not a valid graph read.");
    plan.source=&in;plan.storage_generation=s->generation;
    plan.requested_scene=in.scene;plan.requested_revisions=in.revisions;
    plan.completed=in.completed;plan.max_cube_views=in.max_cube_views;
    plan.scene_reads=in.scene_reads.data();plan.scene_read_count=in.scene_reads.size();
    plan.draw_requested=bool(in.draw_static);plan.sampling_requested=bool(in.declare_material_sampling);
    plan.before_scene=s->scene;plan.before_revisions=s->revisions;
    plan.before_captured=s->captured_views;plan.before_uploaded=s->uploaded;
    plan.before_bake_submitted=s->bake_submitted;plan.before_published=s->published;
    plan.scene=s->scene;plan.revisions=s->revisions;plan.capture_identity=s->capture_identity;
    plan.capture_first=s->captured_views;plan.bake_submitted=s->bake_submitted;
    plan.published=s->published;
    if(!in.scene)
    {
        plan.published=false;
        if(s->capture_completion.IsSatisfiedBy(in.completed))
        {plan.scene.reset();plan.revisions={};plan.capture_first=0;plan.bake_submitted=false;}
    }
    if(plan.scene!=in.scene||plan.revisions!=in.revisions)
    {
        plan.published=false;
        if(s->capture_completion.IsSatisfiedBy(in.completed))
        {
            const bool initial=!plan.scene;
            plan.scene=in.scene;plan.revisions=in.revisions;
            plan.capture_first=0;plan.bake_submitted=false;
            if(initial&&s->package_baked&&s->description.revisions==in.revisions)
                plan.capture_first=static_cast<std::uint32_t>(s->probes.size()*6);
        }
    }
    const bool package_sample_ready=s->package_baked&&!s->capture_ready&&s->description.revisions==in.revisions&&
        plan.capture_first==s->probes.size()*6&&!plan.bake_submitted;
    if(in.capture_identity_valid&&in.scene&&!package_sample_ready&&plan.scene==in.scene&&plan.revisions==in.revisions&&
        plan.capture_identity!=in.capture_identity&&s->capture_completion.IsSatisfiedBy(in.completed))
    {
        plan.capture_identity=in.capture_identity;plan.capture_first=0;plan.bake_submitted=false;
        plan.published=false;
    }
    if(in.scene&&plan.bake_submitted&&plan.scene==in.scene&&plan.revisions==in.revisions&&
        s->bake_completion.IsSatisfiedBy(in.completed))plan.published=true;
    plan.current=plan.scene==in.scene&&plan.revisions==in.revisions;
    const auto total=static_cast<std::uint32_t>(s->probes.size()*6);
    if(plan.current&&!plan.published&&!plan.bake_submitted&&plan.capture_first<total&&
        in.max_cube_views&&in.draw_static&&in.capture_identity_valid&&in.capture_sources_ready&&plan.capture_identity==in.capture_identity)
        plan.capture_count=std::min(in.max_cube_views,total-plan.capture_first);
    plan.integrate=plan.capture_count&&
        (plan.capture_first+plan.capture_count)/6>plan.capture_first/6;
    plan.upload=!s->uploaded;
    plan.import_cubes=plan.upload||plan.capture_count||(plan.published&&plan.current);
    plan.finished_bake=(plan.upload&&plan.capture_first==total)||
        (plan.integrate&&plan.capture_first+plan.capture_count==total);
    plan.material_sampling=plan.capture_count&&bool(in.draw_static)&&bool(in.declare_material_sampling);
    plan.valid=true;return Result::Success();
}
LocalProbeBakeStatus LocalProbeSystem::BakeStatus(const CompletionSet &completed) const noexcept
{
    const auto &s=impl_->storage;
    if(!s||!s->usable)return {};
    const auto total=static_cast<std::uint32_t>(s->probes.size()*6);
    return {s->capture_sources_ready,s->bake_submitted&&s->captured_views==total&&
        (s->package_baked||s->finished_capture_identity==s->capture_identity)&&
        s->bake_completion.IsSatisfiedBy(completed)};
}
bool LocalProbeSystem::CaptureIdentityPending(const CompletionSet &completed) const noexcept
{
    const auto &s=impl_->storage;
    return s&&s->usable&&s->capture_ready&&s->current_request&&
        (!s->bake_submitted||!s->bake_completion.IsSatisfiedBy(completed));
}
Result LocalProbeSystem::Register(RenderGraphBuilder &graph,const LocalProbeInputs &in,LocalProbeOutputs &out)
{
    LocalProbeFramePlan plan;
    if(auto prepared=PrepareFrameRecordData(in,plan);!prepared)return prepared;
    return RegisterImpl(graph,in,plan,out,nullptr,nullptr);
}
Result LocalProbeSystem::Register(RenderGraphBuilder &graph,const LocalProbeInputs &in,
    const LocalProbeFramePlan &plan,LocalProbeOutputs &out,LocalProbeLayout *layout)
{return RegisterImpl(graph,in,plan,out,layout,nullptr);}
bool LocalProbeSystem::MatchesLayout(const LocalProbeInputs &in,const LocalProbeFramePlan &plan,
    const LocalProbeLayout &layout) const noexcept
{
    const auto s=impl_->storage;
    if(!s||!s->usable||!plan.valid||plan.source!=&in||plan.storage_generation!=s->generation||
        plan.requested_scene!=in.scene||plan.requested_revisions!=in.revisions||
        plan.max_cube_views!=in.max_cube_views||plan.scene_reads!=in.scene_reads.data()||
        plan.scene_read_count!=in.scene_reads.size()||
        plan.draw_requested!=bool(in.draw_static)||
        plan.sampling_requested!=bool(in.declare_material_sampling)||
        plan.completed.direct!=in.completed.direct||plan.completed.compute!=in.completed.compute||
        plan.completed.copy!=in.completed.copy||
        layout.storage_generation!=s->generation||layout.upload!=plan.upload||
        layout.package_baked!=s->package_baked||layout.import_cubes!=plan.import_cubes||
        layout.integrate!=plan.integrate||layout.material_sampling!=plan.material_sampling||
        layout.published!=(plan.published&&plan.current)||layout.finished_bake!=plan.finished_bake||
        layout.capture_first!=plan.capture_first||layout.capture_count!=plan.capture_count||
        layout.scene_reads.size()!=in.scene_reads.size())return false;
    for(std::size_t i=0;i<in.scene_reads.size();++i)
        if(layout.scene_reads[i].version!=in.scene_reads[i].version||
            layout.scene_reads[i].access!=in.scene_reads[i].access)return false;
    return true;
}
Result LocalProbeSystem::BindRetained(RenderGraphBuilder &graph,const LocalProbeInputs &in,
    const LocalProbeFramePlan &plan,const LocalProbeLayout &layout,LocalProbeOutputs &out)
{return RegisterImpl(graph,in,plan,out,nullptr,&layout);}
Result LocalProbeSystem::RegisterImpl(RenderGraphBuilder &graph,const LocalProbeInputs &in,
    const LocalProbeFramePlan &plan,LocalProbeOutputs &out,LocalProbeLayout *layout,
    const LocalProbeLayout *retained)
{
    out={}; auto s=impl_->storage;
    if(!s || impl_->pending.graph || !s->usable) return Failure(ErrorCode::InvalidState,"Local probes are unprepared, pending or require reset after failed submission.");
    if(!plan.valid||plan.source!=&in||plan.storage_generation!=s->generation||
        plan.requested_scene!=in.scene||plan.requested_revisions!=in.revisions||
        plan.max_cube_views!=in.max_cube_views||plan.scene_reads!=in.scene_reads.data()||
        plan.scene_read_count!=in.scene_reads.size()||
        plan.draw_requested!=bool(in.draw_static)||
        plan.sampling_requested!=bool(in.declare_material_sampling)||
        plan.completed.direct!=in.completed.direct||plan.completed.compute!=in.completed.compute||
        plan.completed.copy!=in.completed.copy||
        s->scene!=plan.before_scene||s->revisions!=plan.before_revisions||
        s->captured_views!=plan.before_captured||s->uploaded!=plan.before_uploaded||
        s->bake_submitted!=plan.before_bake_submitted||s->published!=plan.before_published)
        return Failure(ErrorCode::InvalidState,"Local probe prepared frame record changed before registration.");
    if(retained)
    {
        if(!MatchesLayout(in,plan,*retained))
            return Failure(ErrorCode::InvalidState,"Local probe retained topology changed.");
        std::array<UINT,9> ids{};UINT count{};
        if(plan.upload)ids[count++]=186;
        for(UINT i=0;i<plan.capture_count;++i)ids[count++]=180+i;
        if(plan.integrate){ids[count++]=187;ids[count++]=188;}
        if(count!=retained->pass_count)return Failure(ErrorCode::InvalidState,"Local probe retained pass count changed.");
        for(UINT i=0;i<count;++i)
            if(retained->passes[i].id!=ids[i]||!graph.BindPass(retained->passes[i].index,ids[i]))
                return Failure(ErrorCode::InvalidState,"Local probe retained pass sequence changed.");
        const UINT imports=1+(plan.import_cubes?5:0)+(plan.upload?1:0)+(plan.capture_count?1:0);
        for(UINT i=0;i<imports;++i)
            if(!retained->imports[i]||retained->imports[i].value>graph.ResourceCount())
                return Failure(ErrorCode::InvalidState,"Local probe retained import slot changed.");
        UINT import_cursor=1;
        if(retained->imports[0].kind!=ResourceKind::Buffer)
            return Failure(ErrorCode::InvalidState,"Local probe metadata import type changed.");
        if(plan.import_cubes)
        {
            for(auto kind:{ResourceKind::Texture,ResourceKind::Texture,ResourceKind::Buffer,
                    ResourceKind::Texture,ResourceKind::Buffer})
                if(retained->imports[import_cursor++].kind!=kind)
                    return Failure(ErrorCode::InvalidState,"Local probe cube import type changed.");
        }
        if(plan.upload&&retained->imports[import_cursor++].kind!=ResourceKind::Buffer)
            return Failure(ErrorCode::InvalidState,"Local probe staging import type changed.");
        if(plan.capture_count&&retained->imports[import_cursor].kind!=ResourceKind::Texture)
            return Failure(ErrorCode::InvalidState,"Local probe depth import type changed.");
    }
    if(layout){*layout={};layout->storage_generation=s->generation;
        layout->capture_first=plan.capture_first;layout->capture_count=plan.capture_count;
        layout->upload=plan.upload;layout->package_baked=s->package_baked;
        layout->import_cubes=plan.import_cubes;layout->integrate=plan.integrate;
        layout->material_sampling=plan.material_sampling;
        layout->published=plan.published&&plan.current;
        layout->finished_bake=plan.finished_bake;
        layout->scene_reads.assign(in.scene_reads.begin(),in.scene_reads.end());}
    Impl::Pending pending;pending.graph=&graph;pending.storage=s;
    pending.scene=plan.scene;pending.revisions=plan.revisions;
    pending.captured_views=plan.capture_first;pending.capture_identity=plan.capture_identity;pending.upload=plan.upload;
    for(auto *resource:{&s->disabled_records,&s->raw,&s->ggx,&s->sh,&s->distance,
            &s->records,&s->upload,&s->depth})
        if(resource->resource)
            if(auto certified=resource->EnsureNativeContract(s->device.Get());!certified)return certified;
    UINT import_index{},pass_index{};bool imports_valid=true;
    auto import=[&](AllocationResource &r,std::string_view name,bool buffer) {
        const UINT index=import_index++;
        const auto h=retained?retained->imports[index]:
            (buffer?graph.ImportBuffer({r.resource.Get(),r.graph_completion,r.graph_queue,{},r.NativeContract()},r.graph_access,name):
                graph.ImportTexture({r.resource.Get(),r.graph_completion,r.graph_queue,{},r.NativeContract()},r.graph_access,name));
        if(retained)imports_valid&=buffer?
            graph.BindBuffer(h,{r.resource.Get(),r.graph_completion,r.graph_queue,{},r.NativeContract()},r.graph_access):
            graph.BindTexture(h,{r.resource.Get(),r.graph_completion,r.graph_queue,{},r.NativeContract()},r.graph_access);
        if(layout)layout->imports[index]=h;
        pending.imports.push_back({&r,h}); return graph.Version(h);
    };
    const auto make_pass=[&](UINT id,std::string_view name)
    {
        const UINT index=pass_index++;
        if(layout)layout->passes[index]={graph.PassCount(),id};
        return retained?*graph.BindPass(retained->passes[index].index,id):
            graph.AddPass(id,name,QueueHint::Direct);
    };
    const auto write=[&](PassBuilder &pass,ResourceVersion v,Access access)
    {return retained?ResourceVersion{v.resource,v.ordinal+1}:pass.Write(v,access);};
    const auto writeTexture=[&](PassBuilder &pass,ResourceVersion v,Access access,TextureSubresourceRange range)
    {return retained?ResourceVersion{v.resource,v.ordinal+1}:pass.Write(v,access,range);};
    const auto writeBuffer=[&](PassBuilder &pass,ResourceVersion v,Access access,BufferRange range)
    {return retained?ResourceVersion{v.resource,v.ordinal+1}:pass.Write(v,access,range);};
    auto disabled=import(s->disabled_records,"LocalProbe.ZeroCountMetadata",true);
    ResourceVersion raw{},ggx{},sh{},distance{},records{},depth{};
    auto importCubes=[&]() {
        if(raw.resource) return;
        raw=import(s->raw,"LocalProbe.RawRadiance",false); ggx=import(s->ggx,"LocalProbe.GgxRadiance",false);
        sh=import(s->sh,"LocalProbe.CosineSh9",true); distance=import(s->distance,"LocalProbe.FirstHitDistance",false);
        records=import(s->records,"LocalProbe.Records",true);
    };
    if(plan.upload)
    {
        importCubes();const auto upload=import(s->upload,"LocalProbe.ImmutableStaging",true);
        auto pass=make_pass(186,"LocalProbePackageUpload");if(!retained)pass.Read(upload,Access::CopySource);
        records=write(pass,records,Access::CopyDest);disabled=write(pass,disabled,Access::CopyDest);
        if(s->package_baked){raw=write(pass,raw,Access::CopyDest);ggx=write(pass,ggx,Access::CopyDest);
            sh=write(pass,sh,Access::CopyDest);distance=write(pass,distance,Access::CopyDest);}
        pass.SetExecuteChecked([s](RenderPassContext &context) {
            auto *c=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList()); if(!c) return Failure(ErrorCode::InvalidState,"Local probe upload requires a native list.");
            c->CopyBufferRegion(s->records.resource.Get(),0,s->upload.resource.Get(),s->record_offset,s->probes.size()*kLocalProbeGpuRecordStride);
            c->CopyBufferRegion(s->disabled_records.resource.Get(),0,s->upload.resource.Get(),s->disabled_offset,kLocalProbeGpuRecordStride);
            if(s->package_baked)
            {
                auto copy=[&](ID3D12Resource *target,const CubeUpload &u) {
                    for(UINT i=0;i<u.footprints.size();++i)
                    {
                        D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource=s->upload.resource.Get(); from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint=u.footprints[i];
                        D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource=target; to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; to.SubresourceIndex=i;
                        c->CopyTextureRegion(&to,0,0,0,&from,nullptr);
                    }
                };
                copy(s->raw.resource.Get(),s->raw_upload); copy(s->ggx.resource.Get(),s->ggx_upload); copy(s->distance.resource.Get(),s->distance_upload);
                c->CopyBufferRegion(s->sh.resource.Get(),0,s->upload.resource.Get(),s->sh_offset,s->probes.size()*9*16);
            }
            return Result::Success();
        });
        if(plan.finished_bake)pending.finished_bake=true;
    }
    const bool current=plan.current;
    if(plan.capture_count)
    {
        importCubes();depth=import(s->depth,"LocalProbe.CaptureDepth",false);
        const auto count=plan.capture_count;
        for(std::uint32_t i=0;i<count;++i)
        {
            const auto flat=plan.capture_first+i,probe=flat/6,face=flat%6;
            auto view=CaptureView(plan.scene,plan.revisions,s->probes[probe],probe,face,s->description.edge);
            view.radiance_resource=s->raw.resource.Get(); view.distance_resource=s->distance.resource.Get(); view.depth_resource=s->depth.resource.Get();
            view.radiance_rtv=s->Cpu(s->rtvs.Get(),flat*2,s->rtv_stride); view.distance_rtv=s->Cpu(s->rtvs.Get(),flat*2+1,s->rtv_stride);
            view.depth_dsv=s->dsv->GetCPUDescriptorHandleForHeapStart();
            auto pass=make_pass(180+i,"LocalProbeStaticCubeCapture");
            if(!retained)
            {
                for(const auto &read:in.scene_reads)pass.Read(read.version,read.access);
                if(plan.material_sampling)in.declare_material_sampling(pass);
                pass.Read(disabled,Access::AllShaderRead);
            }
            const TextureSubresourceRange range{0,1,flat,1,0,1};
            raw=writeTexture(pass,raw,Access::RenderTarget,range);
            distance=writeTexture(pass,distance,Access::RenderTarget,range);
            depth=write(pass,depth,Access::DepthWrite);
            pass.SetExecuteChecked([s,view,draw=in.draw_static](RenderPassContext &context) {
                auto *c=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList()); if(!c) return Failure(ErrorCode::InvalidState,"Local probe capture requires native commands.");
                const float clearRadiance[4]{0,0,0,1},clearDistance[4]{view.far_distance,0,0,0};
                c->ClearRenderTargetView(view.radiance_rtv,clearRadiance,0,nullptr); c->ClearRenderTargetView(view.distance_rtv,clearDistance,0,nullptr);
                c->ClearDepthStencilView(view.depth_dsv,D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr);
                const D3D12_CPU_DESCRIPTOR_HANDLE targets[2]{view.radiance_rtv,view.distance_rtv}; c->OMSetRenderTargets(2,targets,FALSE,&view.depth_dsv);
                const D3D12_VIEWPORT viewport{0,0,float(view.edge),float(view.edge),0,1}; const D3D12_RECT scissor{0,0,LONG(view.edge),LONG(view.edge)};
                c->RSSetViewports(1,&viewport); c->RSSetScissorRects(1,&scissor); return draw(context,view);
            });
        }
        pending.captured_views+=count;
        // At most one probe finishes within a six-view budget. Convolve that
        // probe now rather than issuing an unbounded all-probe filter burst at
        // the end of the generation. Every cube/SH output stays unpublished.
        if(plan.integrate)
        {
            const auto finishedProbe=pending.captured_views/6-1;
            const TextureSubresourceRange rawRange{0,1,finishedProbe*6,6,0,1},ggxRange{0,s->mip_count,finishedProbe*6,6,0,1};
            const auto constantBase=UINT64(finishedProbe)*s->mip_count*256;
            auto shPass=make_pass(187,"LocalProbeCosineShIntegration");
            if(!retained)shPass.Read(raw,Access::ComputeRead,rawRange);
            sh=writeBuffer(shPass,sh,Access::UnorderedWrite,BufferRange{UINT64(finishedProbe)*9*16,9*16});
            shPass.SetExecuteChecked([s,constantBase](RenderPassContext &context) {
                auto *c=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList()); if(!c) return Failure(ErrorCode::InvalidState,"Local probe SH integration requires native commands.");
                ID3D12DescriptorHeap *heaps[]{s->bake_descriptors.Get()}; c->SetDescriptorHeaps(1,heaps); c->SetComputeRootSignature(s->root.Get()); c->SetPipelineState(s->pipelines[0].Get());
                c->SetComputeRootConstantBufferView(0,s->constants.resource->GetGPUVirtualAddress()+constantBase); c->SetComputeRootDescriptorTable(1,s->Gpu(0)); c->SetComputeRootDescriptorTable(2,s->Gpu(kBakeSrvCount));
                c->Dispatch(9,1,1); return Result::Success();
            });
            auto ggxPass=make_pass(188,"LocalProbeGgxPrefilter");
            if(!retained)ggxPass.Read(raw,Access::ComputeRead,rawRange);
            ggx=writeTexture(ggxPass,ggx,Access::UnorderedWrite,ggxRange);
            ggxPass.SetExecuteChecked([s,constantBase](RenderPassContext &context) {
                auto *c=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList()); if(!c) return Failure(ErrorCode::InvalidState,"Local probe GGX integration requires native commands.");
                ID3D12DescriptorHeap *heaps[]{s->bake_descriptors.Get()}; c->SetDescriptorHeaps(1,heaps); c->SetComputeRootSignature(s->root.Get()); c->SetPipelineState(s->pipelines[1].Get()); c->SetComputeRootDescriptorTable(1,s->Gpu(0));
                for(std::uint32_t mip=0;mip<s->mip_count;++mip)
                {
                    c->SetComputeRootConstantBufferView(0,s->constants.resource->GetGPUVirtualAddress()+constantBase+UINT64(mip)*256);
                    c->SetComputeRootDescriptorTable(2,s->Gpu(kBakeSrvCount+2*mip)); const auto e=std::max(1u,s->description.edge>>mip);
                    c->Dispatch((e+7)/8,(e+7)/8,6);
                }
                return Result::Success();
            }); pending.finished_bake=pending.captured_views==s->probes.size()*6;
        }
    }
    if(!retained)graph.SetFinalAccess(disabled,Access::AllShaderRead);
    for(std::uint32_t i=0;i<kLocalProbeSrvCount;++i) out.srvs[i]=s->Export(5+i);
    out.records=disabled; out.record_resource=s->disabled_records.resource.Get(); out.revisions=in.revisions;
    out.edge=s->description.edge; out.mip_count=s->mip_count; out.total_cube_views=static_cast<std::uint32_t>(s->probes.size()*6);
    out.captured_cube_views=pending.captured_views; out.update_pending=!plan.published;
    if(plan.published && current)
    {
        importCubes(); out.radiance=raw; out.prefiltered=ggx; out.irradiance=sh; out.first_hit_distance=distance; out.records=records;
        out.radiance_resource=s->raw.resource.Get(); out.prefiltered_resource=s->ggx.resource.Get(); out.irradiance_resource=s->sh.resource.Get();
        out.distance_resource=s->distance.resource.Get(); out.record_resource=s->records.resource.Get();
        for(std::uint32_t i=0;i<kLocalProbeSrvCount;++i) out.srvs[i]=s->Export(i);
        out.probe_count=static_cast<std::uint32_t>(s->probes.size()); out.enabled=true; out.update_pending=false;
    }
    if(raw.resource)
    {
        // Preserve exact final states of all capture/import resources across
        // bounded updates. They are never exported before full publication.
        if(pending.finished_bake || plan.published)
        {
            if(!retained){graph.SetFinalAccess(raw,Access::AllShaderRead);graph.SetFinalAccess(ggx,Access::AllShaderRead);
                graph.SetFinalAccess(sh,Access::AllShaderRead);graph.SetFinalAccess(distance,Access::AllShaderRead);}
        }
        if(!retained)graph.SetFinalAccess(records,Access::AllShaderRead);
    }
    if(!imports_valid)return Failure(ErrorCode::InvalidState,"Local probe retained binding changed.");
    const std::array<ResourceVersion,7> final_versions{raw,ggx,sh,distance,records,disabled,depth};
    if(retained&&retained->outputs!=final_versions)
        return Failure(ErrorCode::InvalidState,"Local probe retained output versions changed.");
    if(layout){layout->pass_count=pass_index;layout->outputs=final_versions;}
    s->scene=plan.scene;s->revisions=plan.revisions;s->capture_identity=plan.capture_identity;s->captured_views=plan.capture_first;
    if(in.capture_identity_valid)s->capture_sources_ready=in.capture_sources_ready;
    s->bake_submitted=plan.bake_submitted;s->published=plan.published;
    s->current_request=current;
    impl_->pending=std::move(pending); return Result::Success();
}

bool LocalProbeSystem::NativeCaptureRequired(const std::shared_ptr<const StaticRenderScene> &scene,
    render_data::SceneProbeRevisions revisions,const CompletionSet &completed) const noexcept
{
    const auto &s=impl_->storage;
    if(!s || !s->usable || !scene) return false;
    if(!s->capture_ready && !s->completion.IsSatisfiedBy(completed)) return false;
    if(s->scene!=scene || s->revisions!=revisions)
    {
        if(!s->capture_completion.IsSatisfiedBy(completed)) return false;
        return !(!s->scene && s->package_baked && s->description.revisions==revisions);
    }
    return !s->published && !s->bake_submitted && s->captured_views<s->probes.size()*6;
}

Result LocalProbeSystem::CompleteSubmission(const RenderGraphBuilder &graph,const CompletionSet &completion,
    bool success,const std::shared_ptr<const StaticRenderScene> &current_scene,render_data::SceneProbeRevisions current_revisions)
{
    auto &p=impl_->pending;
    if(p.graph!=&graph) return Failure(ErrorCode::InvalidArgument,"Local probe completion has no matching graph.");
    auto s=p.storage; s->completion.Merge(completion);
    for(const auto &item:p.imports)
    {
        const auto c=graph.ResourceCompletion(item.handle); s->completion.Merge(c); item.allocation->graph_completion.Merge(c); item.allocation->graph_completion.Merge(completion);
        // Zero-count metadata stays readable while a stale cube capture waits for its own users to finish.
        if(item.allocation!=&s->disabled_records) { s->capture_completion.Merge(c); s->capture_completion.Merge(completion); }
        if(success) { item.allocation->graph_access=graph.FinalAccess(item.handle); item.allocation->graph_queue=graph.FinalQueue(item.handle); }
    }
    if(success)
    {
        if(p.upload) s->uploaded=true;
        if(p.scene==current_scene && p.revisions==current_revisions && p.capture_identity==s->capture_identity)
        {
            s->captured_views=p.captured_views;
            if(p.finished_bake) { s->bake_submitted=true; s->finished_capture_identity=p.capture_identity; s->bake_completion=s->completion; }
        }
        else { s->published=false; s->bake_submitted=false; }
    }
    else { s->published=false; s->usable=false; }
    p={}; return Result::Success();
}
Result LocalProbeSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if(impl_->pending.graph || (impl_->storage && !impl_->storage->completion.IsSatisfiedBy(completed)))
        return Failure(ErrorCode::InvalidState,"Local probe reset requires completed graph/resource/scene leases.");
    impl_->storage.reset(); return Result::Success();
}
} // namespace hs
