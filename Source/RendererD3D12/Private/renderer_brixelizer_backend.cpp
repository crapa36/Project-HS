// Deliberately the ONE compilation of the verified 1.1.4 DX12 backend source.
// Its private layout is used in this same TU, rather than mirrored across an
// invented ABI. CMake supplies only the verified pinned backend source path.
#include <ffx_dx12.cpp>
#define FFX_CPU
#include <FidelityFX/gpu/ffx_core.h>
#include <FidelityFX/host/ffx_brixelizer_raw.h>
#include <ffx_brixelizer_raw_private.h>
#include "renderer_brixelizer_backend.hpp"
#include "renderer_brixelizer_shaders.hpp"
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <format>
#include <string>

namespace hs::brixelizer_native
{
namespace
{
using Microsoft::WRL::ComPtr;
thread_local const ShaderReader *shader_reader{};
thread_local std::vector<std::byte> selected_blob;
thread_local BackendSlots *active_slots{};
Result Fail(std::string message)
{return Result::Failure(ErrorCode::TaskFailed,"hs_brixelizer_backend",std::move(message));}
Result NativeFailure(std::string_view operation,HRESULT result)
{return Fail(std::format("{} failed (HRESULT 0x{:08X}).",operation,static_cast<std::uint32_t>(result)));}
FfxErrorCode SelectedBlob(FfxEffect effect,FfxPass pass,FfxBindStage stage,std::uint32_t permutation,FfxShaderBlob *out)
{
    if(!out) return FFX_ERROR_INVALID_POINTER;
    if(!shader_reader) {out->data=nullptr;return FFX_ERROR_INVALID_POINTER;}
    auto error=ffxGetPermutationBlobByIndex(effect,pass,stage,permutation,out);
    if(error!=FFX_OK) return error;
    selected_blob.clear();
    if(!(*shader_reader)(ShaderProduct(effect,pass,permutation),selected_blob) || selected_blob.size()!=out->size ||
        std::memcmp(selected_blob.data(),out->data,out->size)!=0) {out->data=nullptr;return FFX_ERROR_BACKEND_API_ERROR;}
    out->data=reinterpret_cast<const std::uint8_t *>(selected_blob.data());
    return FFX_OK;
}
}
struct BackendSlots
{
    struct Upload
    {
        std::uint32_t resource_index{},original_size{};
        ComPtr<ID3D12Resource> canonical;
        std::uint8_t *canonical_map{};
        std::vector<ComPtr<ID3D12Resource>> buffers;
        std::vector<std::uint8_t *> maps;
    };
    FfxInterface *backend_interface{};
    FfxBrixelizerRawContext_Private *raw{};
    std::vector<Upload> uploads;
    std::vector<ComPtr<ID3D12DescriptorHeap>> snapshots;
    ComPtr<ID3D12DescriptorHeap> canonical,original_heap;
    std::uint32_t current_slot{},constant_stride{},constant_begin{},constant_end{},bindless_begin{},bindless_count{};
    bool begun{},batch_active{},snapshot_copied{},batch_fault{};
    ~BackendSlots()
    {
        auto *backend=backend_interface?reinterpret_cast<BackendContext_DX12 *>(backend_interface->scratchBuffer):nullptr;
        // The SDK owns its original heap reference and releases that pointer
        // when the final native context is destroyed. Restore it before our
        // CPU canonical/snapshots are released, including partial Prepare.
        if(backend&&original_heap)backend->descRingBuffer=original_heap.Get();
        if(active_slots==this)active_slots=nullptr;
        for(std::uint32_t i=0;i<uploads.size();++i)
        {
            auto &u=uploads[i];
            if(!u.canonical)continue;
            if(backend&&raw){backend->pResources[u.resource_index].resourcePtr=u.canonical.Get();
                raw->uploadBufferMappedPointers[i]=u.canonical_map;raw->uploadBufferOffsets[i]=0;raw->uploadBufferSizes[i]=u.original_size;}
            for(std::size_t j=0;j<u.buffers.size();++j)if(u.maps[j])u.buffers[j]->Unmap(0,nullptr);
        }
    }
};
namespace
{
// The official backend_interface has no user context field; the owner executes all
// SDK callbacks synchronously on its Prepare owner. This scope is per-thread.
FfxErrorCode StageOwned(FfxInterface *backend_interface,void *data,FfxUInt32 size,FfxConstantBuffer *constant)
{
    auto *slots=active_slots;
    if(!slots||slots->backend_interface->scratchBuffer!=backend_interface->scratchBuffer||!slots->batch_active)return FFX_ERROR_BACKEND_API_ERROR;
    auto *backend=reinterpret_cast<BackendContext_DX12 *>(backend_interface->scratchBuffer);
    if(std::uint64_t(backend->stagingRingBufferBase)+FFX_ALIGN_UP(std::uint64_t(size),256)>=FFX_CONSTANT_BUFFER_RING_BUFFER_SIZE)
    {slots->batch_fault=true;if(constant)*constant={};return FFX_ERROR_INSUFFICIENT_MEMORY;}
    return StageConstantBufferDataDX12(backend_interface,data,size,constant);
}
FfxErrorCode ScheduleOwned(FfxInterface *backend_interface,const FfxGpuJobDescription *job)
{
    auto *slots=active_slots;
    if(!slots||slots->backend_interface->scratchBuffer!=backend_interface->scratchBuffer||!slots->batch_active)return FFX_ERROR_BACKEND_API_ERROR;
    auto *backend=reinterpret_cast<BackendContext_DX12 *>(backend_interface->scratchBuffer);
    // Pinned ScheduleGpuJob asserts against FFX_MAX_GPU_JOBS even though the
    // backing array also scales by contexts. Preserve its stricter bound.
    if(backend->gpuJobCount>=FFX_MAX_GPU_JOBS){slots->batch_fault=true;return FFX_ERROR_INSUFFICIENT_MEMORY;}
    return ScheduleGpuJobDX12(backend_interface,job);
}
std::uint32_t DescriptorSpan(const FfxPipelineState &p,bool uav)
{
    std::uint32_t maximum=uav?p.uavTextureCount+p.uavBufferCount:p.srvTextureCount+p.srvBufferCount;
    if(uav)
    {
        for(std::uint32_t i=0;i<p.uavTextureCount;++i)
            maximum=std::max(maximum,p.uavTextureBindings[i].slotIndex+p.uavTextureBindings[i].arrayIndex);
        // Match the pinned backend expression exactly, including its texture
        // array-index lookup in the buffer loop.
        for(std::uint32_t i=0;i<p.uavBufferCount;++i)
            maximum=std::max(maximum,p.uavBufferBindings[i].slotIndex+p.uavTextureBindings[i].arrayIndex);
    }
    else
    {
        for(std::uint32_t i=0;i<p.srvTextureCount;++i)
            maximum=std::max(maximum,p.srvTextureBindings[i].slotIndex+p.srvTextureBindings[i].arrayIndex);
        for(std::uint32_t i=0;i<p.srvBufferCount;++i)
            maximum=std::max(maximum,p.srvBufferBindings[i].slotIndex+p.srvTextureBindings[i].arrayIndex);
    }
    return maximum?maximum+1u:0u;
}
FfxErrorCode ExecuteOwned(FfxInterface *backend_interface,FfxCommandList list,FfxUInt32 effect)
{
    auto *slots=active_slots;
    if(!slots || slots->backend_interface->scratchBuffer!=backend_interface->scratchBuffer || !slots->batch_active) return FFX_ERROR_BACKEND_API_ERROR;
    if(slots->batch_fault)return FFX_ERROR_INSUFFICIENT_MEMORY;
    auto *backend=reinterpret_cast<BackendContext_DX12 *>(backend_interface->scratchBuffer);
    std::uint64_t descriptors=backend->descRingBufferBase,constants=backend->constantBufferOffset;
    for(std::uint32_t i=0;i<backend->gpuJobCount;++i)
    {
        const auto &job=backend->pGpuJobs[i];
        if(job.jobType!=FFX_GPU_JOB_COMPUTE) continue;
        const auto &compute=job.computeJobDescriptor;
        descriptors+=DescriptorSpan(compute.pipeline,true)+DescriptorSpan(compute.pipeline,false);
        for(std::uint32_t c=0;c<compute.pipeline.constCount;++c)
            constants+=FFX_ALIGN_UP(std::uint64_t(compute.cbs[c].num32BitEntries)*sizeof(std::uint32_t),256);
    }
    // No GPU native job has been recorded at this point. Reject the entire
    // batch before the pinned backend can wrap either submitted allocation.
    if(descriptors>FFX_RING_BUFFER_DESCRIPTOR_COUNT*backend->maxEffectContexts || constants>=slots->constant_end)
    {slots->batch_fault=true;return FFX_ERROR_INSUFFICIENT_MEMORY;}
    const auto error=ExecuteGpuJobsDX12(backend_interface,list,effect);if(error!=FFX_OK)slots->batch_fault=true;return error;
}
}
void BackendSlotsDelete::operator()(BackendSlots *slots) const noexcept {delete slots;}
Result CreateSlots(FfxInterface &backend_interface,FfxBrixelizerRawContext *raw_context,std::uint32_t count,BackendSlotsOwner &output)
{
    output.reset();
    if(!raw_context||!count || count>FFX_BRIXELIZER_NUM_IN_FLIGHT_FRAMES||count>=FFX_MAX_QUEUED_FRAMES)
        return Fail("Native frame slots exceed the pinned raw and backend ownership capacities.");
    auto slots=BackendSlotsOwner(new BackendSlots);
    auto *backend=reinterpret_cast<BackendContext_DX12 *>(backend_interface.scratchBuffer);
    if(!backend || !backend->descRingBuffer || !backend->device) return Fail("Create native contexts before allocating frame heaps.");
    slots->backend_interface=&backend_interface; slots->original_heap=backend->descRingBuffer;
    slots->raw=reinterpret_cast<FfxBrixelizerRawContext_Private *>(raw_context);
    // In pinned raw.cpp only ContextRegisterBuffers registers static views;
    // creation, Update and GI Dispatch never register one. This service creates
    // slots immediately after both contexts and before RegisterScene buffers.
    if(slots->raw->effectContextId>=backend->maxEffectContexts||
        slots->raw->bufferIndexFreeListSize!=FFX_BRIXELIZER_MAX_INSTANCES)
        return Fail("Create native descriptor ownership before registering any geometry buffers.");
    const auto &raw_effect=backend->pEffectContexts[slots->raw->effectContextId];
    slots->bindless_begin=raw_effect.bindlessBufferSrvHeapStart;
    slots->bindless_count=raw_effect.bindlessBufferSrvHeapSize;
    for(std::uint32_t i=0;i<backend->maxEffectContexts;++i)
    {
        const auto &effect=backend->pEffectContexts[i];
        if(effect.bindlessTextureSrvHeapSize||effect.bindlessTextureUavHeapSize||effect.bindlessBufferUavHeapSize||
            (i!=slots->raw->effectContextId&&effect.bindlessBufferSrvHeapSize))
            return Fail("Pinned Brixelizer contexts must use only their raw geometry BUFFER_SRV bindless range.");
    }
    slots->snapshots.resize(count);
    const auto desc=slots->original_heap->GetDesc();
    if(desc.Type!=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV||
        desc.Flags!=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE||desc.NumDescriptors!=backend->descRingBufferSize||
        !slots->bindless_count||slots->bindless_begin<backend->descBindlessBase||
        slots->bindless_begin>desc.NumDescriptors||slots->bindless_count>desc.NumDescriptors-slots->bindless_begin)
        return Fail("Pinned native descriptor heap flags or geometry bindless range are invalid.");
    auto cpu_desc=desc;cpu_desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if(FAILED(backend->device->CreateDescriptorHeap(&cpu_desc,IID_PPV_ARGS(slots->canonical.ReleaseAndGetAddressOf()))))
        return Fail("Cannot allocate native CPU canonical descriptors.");
    // Raw registration uses structured uint32 BUFFER_SRVs (UNKNOWN/stride4),
    // not raw views. Initialize exactly that proven range so snapshot copies
    // never read uninitialized or unrelated tail descriptors.
    D3D12_SHADER_RESOURCE_VIEW_DESC null_buffer{};
    null_buffer.Format=DXGI_FORMAT_UNKNOWN;null_buffer.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
    null_buffer.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    null_buffer.Buffer.NumElements=1;null_buffer.Buffer.StructureByteStride=sizeof(std::uint32_t);
    const auto increment=backend->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto null_destination=slots->canonical->GetCPUDescriptorHandleForHeapStart();
    null_destination.ptr+=SIZE_T(slots->bindless_begin)*increment;
    for(std::uint32_t i=0;i<slots->bindless_count;++i,null_destination.ptr+=increment)
        backend->device->CreateShaderResourceView(nullptr,&null_buffer,null_destination);
    for(auto &snapshot:slots->snapshots)
        if(FAILED(backend->device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(snapshot.ReleaseAndGetAddressOf()))))
            return Fail("Cannot allocate native fence-owned descriptor snapshot.");
    // Force native upload allocation before assigning a nonzero slot offset.
    std::uint32_t seed{};
    (void)backend->FallbackConstantAllocator(&seed,sizeof(seed));
    if(!backend->constantBufferMem) return Fail("Native constant upload allocation failed.");
    slots->constant_stride=(backend->constantBufferSize/count)&~255u;
    // Raw mapped upload rings wrap by bytes, not by frame. A full real native
    // upload buffer per admitted slot eliminates cross-frame range aliasing.
    // Preserve official resource IDs, descriptors, formats and all public ABI.
    slots->uploads.resize(FFX_BRIXELIZER_NUM_UPLOAD_BUFFERS);
    for(std::uint32_t i=0;i<slots->uploads.size();++i)
    {
        const auto &metadata=uploadBufferMetaData[i];
        if(!(slots->raw->contextDescription.flags&FFX_BRIXELIZER_CONTEXT_FLAG_DEBUG_AABBS)&&
            metadata.id==FFX_BRIXELIZER_RESOURCE_IDENTIFIER_UPLOAD_DEBUG_INSTANCE_ID_BUFFER)continue;
        auto &u=slots->uploads[i];u.resource_index=slots->raw->resources[metadata.id].internalIndex;
        u.canonical=backend->pResources[u.resource_index].resourcePtr;u.canonical_map=slots->raw->uploadBufferMappedPointers[i];
        u.original_size=slots->raw->uploadBufferSizes[i];u.buffers.resize(count);u.maps.resize(count);
        D3D12_HEAP_PROPERTIES properties{};D3D12_HEAP_FLAGS flags{};
        if(!u.canonical)return Fail(std::format("Pinned raw staging resource {} is absent.",i));
        const HRESULT heap_result=u.canonical->GetHeapProperties(&properties,&flags);
        if(FAILED(heap_result))return NativeFailure(std::format("GetHeapProperties for pinned raw staging resource {}",i),heap_result);
        if(properties.Type!=D3D12_HEAP_TYPE_UPLOAD)
            return Fail(std::format("Pinned raw staging resource {} has heap type {} and flags 0x{:X}; expected UPLOAD.",
                i,static_cast<std::uint32_t>(properties.Type),static_cast<std::uint32_t>(flags)));
        const auto canonical_resource=u.canonical->GetDesc();
        if(canonical_resource.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||
            canonical_resource.Width<u.original_size||canonical_resource.Height!=1||
            canonical_resource.DepthOrArraySize!=1||canonical_resource.MipLevels!=1||
            canonical_resource.Format!=DXGI_FORMAT_UNKNOWN||canonical_resource.SampleDesc.Count!=1||
            canonical_resource.SampleDesc.Quality!=0||canonical_resource.Layout!=D3D12_TEXTURE_LAYOUT_ROW_MAJOR||
            canonical_resource.Flags!=D3D12_RESOURCE_FLAG_NONE)
            return Fail(std::format("Pinned raw staging resource {} has an incompatible upload descriptor (width {}, flags 0x{:X}).",
                i,canonical_resource.Width,static_cast<std::uint32_t>(canonical_resource.Flags)));
        D3D12_HEAP_PROPERTIES upload_properties{};upload_properties.Type=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC resource{};resource.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        resource.Width=canonical_resource.Width;resource.Height=1;resource.DepthOrArraySize=1;
        resource.MipLevels=1;resource.Format=DXGI_FORMAT_UNKNOWN;resource.SampleDesc.Count=1;
        resource.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;resource.Flags=D3D12_RESOURCE_FLAG_NONE;
        for(std::uint32_t slot=0;slot<count;++slot)
        {
            const HRESULT create_result=backend->device->CreateCommittedResource(&upload_properties,D3D12_HEAP_FLAG_NONE,
                &resource,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(u.buffers[slot].ReleaseAndGetAddressOf()));
            if(FAILED(create_result))
                return Fail(std::format("CreateCommittedResource for fence-owned raw upload snapshot {} slot {} failed "
                    "(HRESULT 0x{:08X}; width {}; state GENERIC_READ; flags NONE).",i,slot,
                    static_cast<std::uint32_t>(create_result),resource.Width));
            D3D12_RANGE empty_range{};
            const HRESULT map_result=u.buffers[slot]->Map(0,&empty_range,reinterpret_cast<void **>(&u.maps[slot]));
            if(FAILED(map_result))
                return NativeFailure(std::format("Map for fence-owned raw upload snapshot {} slot {}",i,slot),map_result);
        }
    }
    backend_interface.fpExecuteGpuJobs=ExecuteOwned;
    // Exact official RegisterStaticResourceDX12 now creates original views on
    // this non-visible heap between batches. ExecuteOwned admits jobs only
    // inside BeginBatch, where the backend points at the visible frame snapshot.
    backend->descRingBuffer=slots->canonical.Get();
    output=std::move(slots);return Result::Success();
}
Result BeginFrame(BackendSlots &slots,std::uint32_t slot)
{
    if(slot>=slots.snapshots.size() || slots.batch_active) return Fail("Invalid native frame-slot admission.");
    auto *backend=reinterpret_cast<BackendContext_DX12 *>(slots.backend_interface->scratchBuffer);
    slots.current_slot=slot;slots.constant_begin=slot*slots.constant_stride;
    slots.constant_end=slots.constant_begin+slots.constant_stride;
    backend->constantBufferOffset=slots.constant_begin;
    backend->descRingBufferBase=0;
    for(std::uint32_t i=0;i<slots.uploads.size();++i)
    {
        auto &u=slots.uploads[i];
        if(!u.canonical)continue;
        backend->pResources[u.resource_index].resourcePtr=u.buffers[slot].Get();
        slots.raw->uploadBufferMappedPointers[i]=u.maps[slot];slots.raw->uploadBufferOffsets[i]=0;
        slots.raw->uploadBufferSizes[i]=u.original_size;
    }
    slots.begun=true;slots.snapshot_copied=false;return Result::Success();
}
Result BeginBatch(BackendSlots &slots)
{
    if(!slots.begun || slots.batch_active || active_slots) return Fail("Native batch has no admitted frame slot.");
    auto *backend=reinterpret_cast<BackendContext_DX12 *>(slots.backend_interface->scratchBuffer);
    const auto increment=backend->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // The proven raw BUFFER_SRV range has CPU originals. Dynamic prefix views
    // already recorded by a preceding SDF batch retain their visible snapshot.
    const std::uint32_t start=slots.bindless_begin;
    auto source=slots.canonical->GetCPUDescriptorHandleForHeapStart();source.ptr+=SIZE_T(start)*increment;
    auto destination=slots.snapshots[slots.current_slot]->GetCPUDescriptorHandleForHeapStart();destination.ptr+=SIZE_T(start)*increment;
    if(!slots.snapshot_copied)
    {backend->device->CopyDescriptorsSimple(slots.bindless_count,destination,source,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);slots.snapshot_copied=true;}
    backend->descRingBuffer=slots.snapshots[slots.current_slot].Get();
    backend->stagingRingBufferBase=0;
    active_slots=&slots;slots.batch_active=true;slots.batch_fault=false;return Result::Success();
}
void EndBatch(BackendSlots &slots) noexcept
{
    auto *backend=reinterpret_cast<BackendContext_DX12 *>(slots.backend_interface->scratchBuffer);
    backend->descRingBuffer=slots.canonical.Get();slots.batch_active=false;active_slots=nullptr;
}
Result BatchResult(const BackendSlots &slots)
{return slots.batch_fault?Fail("Pinned native batch exceeded a preflight capacity or recording failed; reset is required."):Result::Success();}
std::uint64_t BackendGpuBytes(const BackendSlots &slots) noexcept
{
    const auto *backend=reinterpret_cast<const BackendContext_DX12 *>(slots.backend_interface->scratchBuffer);
    // Heap byte size is opaque. Descriptor storage is reported separately by
    // the D3D12 debug layer; constant resource bytes are concrete and budgeted.
    std::uint64_t bytes=backend->constantBufferResource?backend->constantBufferResource->GetDesc().Width:0;
    for(const auto &u:slots.uploads)for(const auto &buffer:u.buffers)if(buffer)bytes+=buffer->GetDesc().Width;
    return bytes;
}
std::uint64_t NativeResourceBytes(const FfxInterface &backend_interface) noexcept
{
    const auto *backend=reinterpret_cast<const BackendContext_DX12 *>(backend_interface.scratchBuffer);
    std::uint64_t result{};
    if(backend && backend->pEffectContexts) for(std::uint32_t i=0;i<backend->maxEffectContexts;++i)
        if(backend->pEffectContexts[i].active) result+=backend->pEffectContexts[i].vramUsage.totalUsageInBytes;
    return result;
}
ShaderReadScope::ShaderReadScope(const ShaderReader &reader):previous_(shader_reader) {shader_reader=&reader;}
ShaderReadScope::~ShaderReadScope() {shader_reader=previous_;}
void RequireSelectedShaderProducts(FfxInterface &backend_interface) noexcept
{backend_interface.fpGetPermutationBlobByIndex=SelectedBlob;backend_interface.fpExecuteGpuJobs=ExecuteOwned;
    backend_interface.fpStageConstantBufferDataFunc=StageOwned;backend_interface.fpScheduleGpuJob=ScheduleOwned;}
}
