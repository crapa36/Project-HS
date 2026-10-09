#include "renderer_local_probe.hpp"
#include "d3d12_resources.hpp"

#include <hs/core/cooked_format.hpp>
#include <D3D12MemAlloc.h>
#include <wrl/client.h>

#include <algorithm>
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
    const auto hr=allocator->CreateResource(&a,&d,heap==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COMMON,
        nullptr,&target.allocation,IID_PPV_ARGS(target.resource.ReleaseAndGetAddressOf()));
    target.graph_access=heap==D3D12_HEAP_TYPE_UPLOAD?Access::CopySource:Access::Common;
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
    AllocationResource raw,ggx,sh,distance,records,disabled_records,depth,upload,constants;
    ComPtr<ID3D12DescriptorHeap> exports,bake_descriptors,rtvs,dsv;
    ComPtr<ID3D12RootSignature> root; std::array<ComPtr<ID3D12PipelineState>,2> pipelines;
    CubeUpload raw_upload,ggx_upload,distance_upload;
    std::uint64_t sh_offset{},record_offset{},disabled_offset{},bytes{};
    std::uint32_t mip_count{},srv_stride{},rtv_stride{},captured_views{};
    std::shared_ptr<const StaticRenderScene> scene;
    render_data::SceneProbeRevisions revisions;
    CompletionSet completion,bake_completion;
    bool uploaded{},package_baked{},bake_submitted{},published{},usable{true};
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
        std::uint32_t captured_views{}; bool upload{},finished_bake{};
    } pending;
};
LocalProbeSystem::LocalProbeSystem():impl_(std::make_unique<Impl>()) {}
LocalProbeSystem::~LocalProbeSystem()=default;
std::uint64_t LocalProbeCandidate::AllocatedBytes() const noexcept { return storage_?storage_->bytes:0; }
std::uint64_t LocalProbeSystem::AllocatedBytes() const noexcept { return impl_->storage?impl_->storage->bytes:0; }

Result LocalProbeSystem::Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,
    std::span<const std::byte> bytes,const ShaderReader &reader,LocalProbeCandidate &candidate) const
{
    candidate={}; if(!device || !allocator || !reader) return Failure(ErrorCode::InvalidArgument,"Local probe preparation needs device, allocator and shader reader.");
    try
    {
        const auto p=render_data::ReadSceneProbePackage(bytes,MakeAssetId(kProduct));
        for(auto format:{DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32_FLOAT})
        {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format}; auto hr=device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support));
            if(FAILED(hr)) return NativeFailure("Query local probe format",hr);
            if(!(support.Support1&D3D12_FORMAT_SUPPORT1_TEXTURECUBE) || !(support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) ||
                !(support.Support1&D3D12_FORMAT_SUPPORT1_RENDER_TARGET) ||
                (format==DXGI_FORMAT_R32G32B32A32_FLOAT && !(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)))
                return Failure(ErrorCode::TaskFailed,"Required local probe HDR/distance format support is unavailable.");
        }
        auto s=std::make_shared<LocalProbeStorage>(); s->device=device; s->allocator=allocator; s->description=p.description; s->probes=p.probes;
        s->mip_count=std::bit_width(p.description.edge); s->package_baked=p.payload==render_data::SceneProbePayload::Baked;
        const auto layers=static_cast<std::uint32_t>(s->probes.size()*6),edge=s->description.edge;
        const auto rawDesc=TextureDescription(edge,layers,1,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        const auto ggxDesc=TextureDescription(edge,layers,s->mip_count,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const auto distanceDesc=TextureDescription(edge,layers,1,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        if(auto r=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,rawDesc,s->raw);!r) return r;
        if(auto r=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,ggxDesc,s->ggx);!r) return r;
        if(auto r=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,distanceDesc,s->distance);!r) return r;
        if(auto r=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,BufferDescription(s->probes.size()*9*16,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),s->sh);!r) return r;
        if(auto r=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,BufferDescription(s->probes.size()*kLocalProbeGpuRecordStride),s->records);!r) return r;
        if(auto r=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,BufferDescription(kLocalProbeGpuRecordStride),s->disabled_records);!r) return r;
        if(auto r=Allocate(allocator,D3D12_HEAP_TYPE_DEFAULT,TextureDescription(edge,1,1,DXGI_FORMAT_D32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),s->depth);!r) return r;
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
        if(auto r=Allocate(allocator,D3D12_HEAP_TYPE_UPLOAD,BufferDescription(UINT64(s->mip_count)*s->probes.size()*256),s->constants);!r) return r;
        hr=s->constants.resource->Map(0,&empty,reinterpret_cast<void **>(&mapped)); if(FAILED(hr)) return NativeFailure("Map local probe constants",hr);
        std::memset(mapped,0,SIZE_T(s->mip_count)*s->probes.size()*256);
        for(std::uint32_t probe=0;probe<s->probes.size();++probe) for(std::uint32_t mip=0;mip<s->mip_count;++mip)
        { const BakeConstants c{edge,s->mip_count,static_cast<std::uint32_t>(s->probes.size()),s->description.ggx_samples,mip,std::max(1u,edge>>mip),probe,0}; std::memcpy(mapped+(SIZE_T(probe)*s->mip_count+mip)*256,&c,sizeof(c)); }
        s->constants.resource->Unmap(0,nullptr);
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
        s->raw.resource->SetName(L"LocalProbe.SceneLinearRawCubeArray"); s->ggx.resource->SetName(L"LocalProbe.GgxCubeArray");
        s->distance.resource->SetName(L"LocalProbe.FirstHitWorldDistance"); s->sh.resource->SetName(L"LocalProbe.CosineSh9");
        for(auto *r:{&s->raw,&s->ggx,&s->sh,&s->distance,&s->records,&s->disabled_records,&s->depth,&s->upload,&s->constants}) s->bytes+=r->allocation->GetSize();
        candidate.storage_=std::move(s); return Result::Success();
    }
    catch(const std::exception &e) { return Failure(ErrorCode::InvalidArgument,e.what()); }
}

Result LocalProbeSystem::Commit(LocalProbeCandidate &&candidate,const CompletionSet &completed)
{
    if(!candidate.storage_) return Failure(ErrorCode::InvalidArgument,"Local probe candidate is empty.");
    if(impl_->pending.graph || (impl_->storage && !impl_->storage->completion.IsSatisfiedBy(completed)))
        return Failure(ErrorCode::InvalidState,"Local probe replacement requires completed graph/resource leases.");
    impl_->storage=std::move(candidate.storage_); return Result::Success();
}

Result LocalProbeSystem::Register(RenderGraphBuilder &graph,const LocalProbeInputs &in,LocalProbeOutputs &out)
{
    out={}; auto s=impl_->storage;
    if(!s || impl_->pending.graph || !s->usable) return Failure(ErrorCode::InvalidState,"Local probes are unprepared, pending or require reset after failed submission.");
    if((in.scene && !ValidRevisions(in.revisions)) || in.max_cube_views>6 ||
        (!in.scene && (in.max_cube_views || in.draw_static || in.revisions.environment || in.revisions.material ||
            in.revisions.light || in.revisions.static_layout)))
        return Failure(ErrorCode::InvalidArgument,"Local probe registration needs an immutable scene, revisions and at most six cube views.");
    if(in.max_cube_views && in.draw_static && in.scene_reads.empty())
        return Failure(ErrorCode::InvalidArgument,"Native local probe capture requires explicit actual scene graph reads.");
    for(const auto &read:in.scene_reads) if(!read.version.resource || read.access==Access::Undefined || read.access==Access::Common ||
        read.access==Access::RenderTarget || read.access==Access::DepthWrite || read.access==Access::UnorderedWrite || read.access==Access::CopyDest)
        return Failure(ErrorCode::InvalidArgument,"Local probe scene dependency is not a valid graph read.");
    if(!in.scene)
    {
        s->published=false;
        if(s->completion.IsSatisfiedBy(in.completed))
        { s->scene.reset(); s->revisions={}; s->captured_views=0; s->bake_submitted=false; }
    }
    const bool same=s->scene==in.scene && s->revisions==in.revisions;
    if(!same)
    {
        s->published=false;
        if(s->completion.IsSatisfiedBy(in.completed))
        {
            const bool initial=!s->scene;
            s->scene=in.scene; s->revisions=in.revisions; s->captured_views=0; s->bake_submitted=false;
            if(initial && s->package_baked && s->description.revisions==in.revisions)
                s->captured_views=static_cast<std::uint32_t>(s->probes.size()*6);
        }
    }
    if(in.scene && s->bake_submitted && s->scene==in.scene && s->revisions==in.revisions && s->bake_completion.IsSatisfiedBy(in.completed)) s->published=true;
    Impl::Pending pending; pending.graph=&graph; pending.storage=s; pending.scene=s->scene; pending.revisions=s->revisions;
    pending.captured_views=s->captured_views; pending.upload=!s->uploaded;
    auto import=[&](AllocationResource &r,std::string_view name) {
        const auto h=r.resource->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER ?
            graph.ImportBuffer({r.resource.Get(),r.graph_completion,r.graph_queue},r.graph_access,name) :
            graph.ImportTexture({r.resource.Get(),r.graph_completion,r.graph_queue},r.graph_access,name);
        pending.imports.push_back({&r,h}); return graph.Version(h);
    };
    auto disabled=import(s->disabled_records,"LocalProbe.ZeroCountMetadata");
    ResourceVersion raw{},ggx{},sh{},distance{},records{};
    auto importCubes=[&]() {
        if(raw.resource) return;
        raw=import(s->raw,"LocalProbe.RawRadiance"); ggx=import(s->ggx,"LocalProbe.GgxRadiance");
        sh=import(s->sh,"LocalProbe.CosineSh9"); distance=import(s->distance,"LocalProbe.FirstHitDistance");
        records=import(s->records,"LocalProbe.Records");
    };
    if(!s->uploaded)
    {
        importCubes(); const auto upload=import(s->upload,"LocalProbe.ImmutableStaging");
        auto pass=graph.AddPass(186,"LocalProbePackageUpload",QueueHint::Direct); pass.Read(upload,Access::CopySource);
        records=pass.Write(records,Access::CopyDest); disabled=pass.Write(disabled,Access::CopyDest);
        if(s->package_baked) { raw=pass.Write(raw,Access::CopyDest); ggx=pass.Write(ggx,Access::CopyDest); sh=pass.Write(sh,Access::CopyDest); distance=pass.Write(distance,Access::CopyDest); }
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
        if(s->captured_views==s->probes.size()*6) pending.finished_bake=true;
    }
    const bool current=s->scene==in.scene && s->revisions==in.revisions;
    if(current && !s->published && !s->bake_submitted && s->captured_views<s->probes.size()*6 &&
        in.max_cube_views && in.draw_static)
    {
        importCubes(); auto depth=import(s->depth,"LocalProbe.CaptureDepth");
        const auto count=std::min(in.max_cube_views,static_cast<std::uint32_t>(s->probes.size()*6)-s->captured_views);
        for(std::uint32_t i=0;i<count;++i)
        {
            const auto flat=s->captured_views+i,probe=flat/6,face=flat%6;
            auto view=CaptureView(s->scene,s->revisions,s->probes[probe],probe,face,s->description.edge);
            view.radiance_resource=s->raw.resource.Get(); view.distance_resource=s->distance.resource.Get(); view.depth_resource=s->depth.resource.Get();
            view.radiance_rtv=s->Cpu(s->rtvs.Get(),flat*2,s->rtv_stride); view.distance_rtv=s->Cpu(s->rtvs.Get(),flat*2+1,s->rtv_stride);
            view.depth_dsv=s->dsv->GetCPUDescriptorHandleForHeapStart();
            auto pass=graph.AddPass(180+i,"LocalProbeStaticCubeCapture",QueueHint::Direct);
            for(const auto &read:in.scene_reads) pass.Read(read.version,read.access);
            if(in.draw_static && in.declare_material_sampling) in.declare_material_sampling(pass);
            pass.Read(disabled,Access::AllShaderRead);
            const TextureSubresourceRange range{0,1,flat,1,0,1}; raw=pass.Write(raw,Access::RenderTarget,range);
            distance=pass.Write(distance,Access::RenderTarget,range); depth=pass.Write(depth,Access::DepthWrite);
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
        if(pending.captured_views/6>s->captured_views/6)
        {
            const auto finishedProbe=pending.captured_views/6-1;
            const TextureSubresourceRange rawRange{0,1,finishedProbe*6,6,0,1},ggxRange{0,s->mip_count,finishedProbe*6,6,0,1};
            const auto constantBase=UINT64(finishedProbe)*s->mip_count*256;
            auto shPass=graph.AddPass(187,"LocalProbeCosineShIntegration",QueueHint::Direct); shPass.Read(raw,Access::ComputeRead,rawRange);
            sh=shPass.Write(sh,Access::UnorderedWrite,BufferRange{UINT64(finishedProbe)*9*16,9*16});
            shPass.SetExecuteChecked([s,constantBase](RenderPassContext &context) {
                auto *c=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList()); if(!c) return Failure(ErrorCode::InvalidState,"Local probe SH integration requires native commands.");
                ID3D12DescriptorHeap *heaps[]{s->bake_descriptors.Get()}; c->SetDescriptorHeaps(1,heaps); c->SetComputeRootSignature(s->root.Get()); c->SetPipelineState(s->pipelines[0].Get());
                c->SetComputeRootConstantBufferView(0,s->constants.resource->GetGPUVirtualAddress()+constantBase); c->SetComputeRootDescriptorTable(1,s->Gpu(0)); c->SetComputeRootDescriptorTable(2,s->Gpu(kBakeSrvCount));
                c->Dispatch(9,1,1); return Result::Success();
            });
            auto ggxPass=graph.AddPass(188,"LocalProbeGgxPrefilter",QueueHint::Direct); ggxPass.Read(raw,Access::ComputeRead,rawRange);
            ggx=ggxPass.Write(ggx,Access::UnorderedWrite,ggxRange);
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
    graph.SetFinalAccess(disabled,Access::AllShaderRead);
    for(std::uint32_t i=0;i<kLocalProbeSrvCount;++i) out.srvs[i]=s->Export(5+i);
    out.records=disabled; out.record_resource=s->disabled_records.resource.Get(); out.revisions=in.revisions;
    out.edge=s->description.edge; out.mip_count=s->mip_count; out.total_cube_views=static_cast<std::uint32_t>(s->probes.size()*6);
    out.captured_cube_views=pending.captured_views; out.update_pending=!s->published;
    if(s->published && current)
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
        if(pending.finished_bake || s->published)
        {
            graph.SetFinalAccess(raw,Access::AllShaderRead); graph.SetFinalAccess(ggx,Access::AllShaderRead);
            graph.SetFinalAccess(sh,Access::AllShaderRead); graph.SetFinalAccess(distance,Access::AllShaderRead);
        }
        graph.SetFinalAccess(records,Access::AllShaderRead);
    }
    impl_->pending=std::move(pending); return Result::Success();
}

bool LocalProbeSystem::NativeCaptureRequired(const std::shared_ptr<const StaticRenderScene> &scene,
    render_data::SceneProbeRevisions revisions,const CompletionSet &completed) const noexcept
{
    const auto &s=impl_->storage;
    if(!s || !s->usable || !scene) return false;
    if(s->scene!=scene || s->revisions!=revisions)
    {
        if(!s->completion.IsSatisfiedBy(completed)) return false;
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
        if(success) { item.allocation->graph_access=graph.FinalAccess(item.handle); item.allocation->graph_queue=graph.FinalQueue(item.handle); }
    }
    if(success)
    {
        if(p.upload) s->uploaded=true;
        if(p.scene==current_scene && p.revisions==current_revisions)
        {
            s->captured_views=p.captured_views;
            if(p.finished_bake) { s->bake_submitted=true; s->bake_completion=s->completion; }
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
