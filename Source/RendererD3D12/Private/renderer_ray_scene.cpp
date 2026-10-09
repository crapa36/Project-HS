#include "renderer_ray_scene.hpp"
#include "d3d12_resources.hpp"

#include <D3D12MemAlloc.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
using OmmKey=render_data::OpacityMicromapDigest;
using StaticKey=std::tuple<std::uint64_t,std::uint64_t,OmmKey>;
using DynamicKey=std::tuple<std::uint32_t,std::uint32_t,std::uint64_t,std::uint64_t,std::uint32_t,OmmKey>;
constexpr auto kStaticFlags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
constexpr auto kDynamicFlags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD |
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;

Result Failure(ErrorCode code,std::string message)
{ return Result::Failure(code,"hs_renderer_ray_scene",std::move(message)); }
Result NativeFailure(std::string_view operation,HRESULT error)
{
    return Failure(ErrorCode::TaskFailed,std::string(operation)+" failed (HRESULT "+
        std::to_string(static_cast<std::int32_t>(error))+").");
}
std::uint64_t AlignedSize(std::uint64_t size)
{
    constexpr std::uint64_t alignment=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
    return size && size<=UINT64_MAX-(alignment-1) ? (size+alignment-1)&~(alignment-1) : 0;
}
Result Allocate(D3D12MA::Allocator *allocator,std::uint64_t size,D3D12_HEAP_TYPE heap,
    D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state,Access access,AllocationResource &target)
{
    if(!size) return Failure(ErrorCode::InvalidArgument,"DXR allocation has an invalid size.");
    D3D12_RESOURCE_DESC description{};
    description.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; description.Width=size;
    description.Height=1; description.DepthOrArraySize=description.MipLevels=1;
    description.SampleDesc.Count=1; description.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags=flags;
    D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType=heap;
    const auto native=allocator->CreateResource(&allocation,&description,state,nullptr,&target.allocation,
        IID_PPV_ARGS(target.resource.ReleaseAndGetAddressOf()));
    if(FAILED(native)) return NativeFailure("Allocate DXR buffer",native);
    target.graph_access=access;
    return Result::Success();
}
Result AllocateAS(D3D12MA::Allocator *allocator,std::uint64_t size,bool enhanced_barriers,AllocationResource &target)
{
    const auto aligned=AlignedSize(size);
    if(!aligned)return Failure(ErrorCode::InvalidArgument,"DXR allocation has an invalid size.");
    if(enhanced_barriers)
    {
        D3D12_RESOURCE_DESC1 description{};
        description.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;description.Width=aligned;
        description.Height=1;description.DepthOrArraySize=description.MipLevels=1;
        description.SampleDesc.Count=1;description.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|
            D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;
        D3D12MA::ALLOCATION_DESC allocation{};allocation.HeapType=D3D12_HEAP_TYPE_DEFAULT;
        const auto native=allocator->CreateResource3(&allocation,&description,D3D12_BARRIER_LAYOUT_UNDEFINED,
            nullptr,0,nullptr,&target.allocation,IID_PPV_ARGS(target.resource.ReleaseAndGetAddressOf()));
        if(FAILED(native))return NativeFailure("Allocate enhanced DXR acceleration structure",native);
        target.graph_access=Access::ASRead;return Result::Success();
    }
    return Allocate(allocator,aligned,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,Access::ASRead,target);
}
Result AllocateScratch(D3D12MA::Allocator *allocator,std::uint64_t size,AllocationResource &target)
{
    return Allocate(allocator,AlignedSize(size),D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        Access::ASBuildScratch,target);
}
std::uint64_t Bytes(const AllocationResource &allocation)
{ return allocation.resource ? allocation.resource->GetDesc().Width : 0; }

std::uint64_t BackingBytes(const AllocationResource &r){return r.allocation?r.allocation->GetSize():0;}
OmmKey CoverageKey(const RaySceneGeometry &g){return g.omm?g.omm.identity.coverage_key:OmmKey{};}
bool SameCoverage(const RayOmmBinding &a,const RayOmmBinding &b)
{
    return bool(a)==bool(b)&&a.identity.geometry_asset.value==b.identity.geometry_asset.value&&
        a.identity.triangle_count==b.identity.triangle_count&&a.identity.geometry_digest==b.identity.geometry_digest&&
        a.identity.topology_digest==b.identity.topology_digest&&a.identity.material_digest==b.identity.material_digest&&
        a.identity.coverage_key==b.identity.coverage_key;
}
struct NativeGeometry
{
    D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC triangles{};
    D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC linkage{};
    D3D12_RAYTRACING_GEOMETRY_DESC description{};
    explicit NativeGeometry(const RaySceneGeometry &geometry)
    {
    description.Flags=geometry.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;
    triangles.VertexBuffer={geometry.vertices->GetGPUVirtualAddress()+geometry.vertex_byte_offset,geometry.vertex_stride};
    triangles.VertexCount=geometry.vertex_count;
    triangles.IndexFormat=geometry.index_format; triangles.IndexCount=geometry.index_count;
    triangles.IndexBuffer=geometry.indices->GetGPUVirtualAddress()+geometry.index_byte_offset;
    if(geometry.omm)
    {description.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES;
        linkage.OpacityMicromapIndexBuffer={geometry.omm.index_resource->GetGPUVirtualAddress()+geometry.omm.index_byte_offset,4};
        linkage.OpacityMicromapIndexFormat=DXGI_FORMAT_R32_UINT;linkage.OpacityMicromapBaseLocation=geometry.omm.base_location;
        linkage.OpacityMicromapArray=geometry.omm.array_resource->GetGPUVirtualAddress();description.OmmTriangles={&triangles,&linkage};}
    else{description.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;description.Triangles=triangles;}
    }
    NativeGeometry(const NativeGeometry &)=delete;
    NativeGeometry &operator=(const NativeGeometry &)=delete;
};
D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS BlasInputs(
    const D3D12_RAYTRACING_GEOMETRY_DESC &geometry,bool dynamic)
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS result{};
    result.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    result.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY; result.NumDescs=1; result.pGeometryDescs=&geometry;
    result.Flags=dynamic ? kDynamicFlags : kStaticFlags;
    return result;
}
bool SameTopology(const RaySceneGeometry &a,const RaySceneGeometry &b)
{
    return a.stable_key==b.stable_key && a.generation==b.generation &&
        a.index_format==b.index_format && a.index_count==b.index_count &&
        a.index_byte_offset==b.index_byte_offset && a.indices==b.indices &&
        a.vertex_count==b.vertex_count && a.vertex_stride==b.vertex_stride && a.opaque==b.opaque &&
        a.dynamic==b.dynamic&&SameCoverage(a.omm,b.omm)&&a.omm.generation==b.omm.generation&&
        a.omm.array_resource==b.omm.array_resource&&a.omm.index_resource==b.omm.index_resource&&
        a.omm.index_byte_offset==b.omm.index_byte_offset&&a.omm.index_count==b.omm.index_count&&a.omm.base_location==b.omm.base_location;
}
bool SameStaticGeometry(const RaySceneGeometry &a,const RaySceneGeometry &b)
{
    return SameTopology(a,b) && a.vertices==b.vertices && a.vertex_byte_offset==b.vertex_byte_offset &&
        a.current_revision==b.current_revision;
}
bool ValidRange(ID3D12Resource *resource,std::uint64_t offset,std::uint64_t size)
{
    if(!resource || !size) return false;
    const auto description=resource->GetDesc();
    return description.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER &&
        offset<=description.Width && size<=description.Width-offset;
}
Result ValidateGeometry(const RenderGraphBuilder &graph,ID3D12Device5 *device,const RaySceneGeometry &geometry)
{
    const auto index_stride=geometry.index_format==DXGI_FORMAT_R32_UINT ? 4u :
        geometry.index_format==DXGI_FORMAT_R16_UINT ? 2u : 0u;
    if(!geometry.stable_key || !geometry.generation || geometry.vertex_count<3 || geometry.index_count<3 ||
        geometry.index_count%3 || geometry.vertex_stride<12 || geometry.vertex_stride%4 ||
        geometry.vertex_byte_offset%4 || !index_stride || geometry.index_byte_offset%index_stride ||
        !ValidRange(geometry.vertices,geometry.vertex_byte_offset,
            std::uint64_t(geometry.vertex_count-1)*geometry.vertex_stride+12) ||
        !ValidRange(geometry.indices,geometry.index_byte_offset,std::uint64_t(geometry.index_count)*index_stride))
        return Failure(ErrorCode::InvalidArgument,"Ray scene geometry has invalid identity, triangle layout or native ranges.");
    if(!geometry.vertex_version.resource || !geometry.index_version.resource ||
        geometry.vertex_version.resource.kind!=ResourceKind::Buffer || geometry.index_version.resource.kind!=ResourceKind::Buffer ||
        graph.Resolve(geometry.vertex_version.resource)!=geometry.vertices || graph.Resolve(geometry.index_version.resource)!=geometry.indices)
        return Failure(ErrorCode::InvalidArgument,"Ray scene geometry versions must identify the supplied native buffers.");
    for(auto *resource:{geometry.vertices,geometry.indices})
    {
        ComPtr<ID3D12Device5> owner;
        if(FAILED(resource->GetDevice(IID_PPV_ARGS(owner.ReleaseAndGetAddressOf()))) || owner.Get()!=device ||
            !resource->GetGPUVirtualAddress())
            return Failure(ErrorCode::InvalidArgument,"Ray scene geometry buffers must belong to the prepared DXR device.");
    }
    if(geometry.omm.array_resource||geometry.omm.index_resource||geometry.omm.owner)
    {
        const auto &o=geometry.omm;
        if(!o||!o.generation||o.index_count!=geometry.index_count/3||o.identity.triangle_count!=o.index_count||o.index_byte_offset%4||
           !ValidRange(o.index_resource,o.index_byte_offset,std::uint64_t(o.index_count)*4)||
           o.array_resource->GetDesc().Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||
           o.array_resource->GetGPUVirtualAddress()%D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_BYTE_ALIGNMENT||
           !o.array_version.resource||!o.index_version.resource||graph.Resolve(o.array_version.resource)!=o.array_resource||
           graph.Resolve(o.index_version.resource)!=o.index_resource)
            return Failure(ErrorCode::InvalidArgument,"OMM linkage must match the full canonical triangle range and actual native array/index graph resources.");
        for(auto *resource:{o.array_resource,o.index_resource})
        {ComPtr<ID3D12Device5> owner;if(FAILED(resource->GetDevice(IID_PPV_ARGS(owner.ReleaseAndGetAddressOf())))||owner.Get()!=device)
            return Failure(ErrorCode::InvalidArgument,"OMM linkage belongs to a different native device.");}
    }
    return Result::Success();
}
Result ValidateInstance(const RaySceneInstance &instance,std::size_t geometry_count)
{
    constexpr auto allowed=D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE |
        D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE |
        D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE | D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;
    const auto &m=instance.world;
    for(const auto &row:m.m) for(float value:row)
        if(!std::isfinite(value)) return Failure(ErrorCode::InvalidArgument,"Ray scene transform is not finite.");
    const float determinant=m._11*(m._22*m._33-m._23*m._32)-m._12*(m._21*m._33-m._23*m._31)+
        m._13*(m._21*m._32-m._22*m._31);
    if(instance.geometry_index>=geometry_count || !instance.generation || !instance.mesh_key || !instance.mesh_generation ||
        (instance.flags&~allowed) ||
        ((instance.flags&D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE) &&
         (instance.flags&D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE)) ||
        m._14!=0 || m._24!=0 || m._34!=0 || m._44!=1 || !std::isfinite(determinant) || determinant==0)
        return Failure(ErrorCode::InvalidArgument,"Ray scene instance has invalid identity, geometry, flags or affine transform.");
    return Result::Success();
}
Result Upload(AllocationResource &allocation,const void *data,std::size_t bytes)
{
    void *mapped{}; const D3D12_RANGE unread{0,0};
    const auto native=allocation.resource->Map(0,&unread,&mapped);
    if(FAILED(native)) return NativeFailure("Map DXR instance upload",native);
    if(bytes) std::memcpy(mapped,data,bytes);
    const D3D12_RANGE written{0,bytes}; allocation.resource->Unmap(0,&written);
    return Result::Success();
}

struct Blas
{
    AllocationResource acceleration,scratch,query,readback;
    RaySceneGeometry geometry{};
    ComPtr<ID3D12Resource> vertices,indices;
    CompletionSet completion{},query_completion{};
    bool built{},compacted{},query_ready{},query_consumed{};
    std::uint64_t revision{};
    std::uint64_t AllocatedBytes() const
    { return BackingBytes(acceleration)+BackingBytes(scratch)+BackingBytes(query)+BackingBytes(readback); }
};
struct SceneFrame
{
    AllocationResource acceleration,scratch,instances_upload,metadata_upload,metadata;
    AllocationResource omm_acceleration,omm_scratch,omm_instances_upload;
    CompletionSet completion{};
    std::uint64_t AllocatedBytes() const
    { return BackingBytes(acceleration)+BackingBytes(scratch)+BackingBytes(instances_upload)+BackingBytes(metadata_upload)+BackingBytes(metadata)+
        BackingBytes(omm_acceleration)+BackingBytes(omm_scratch)+BackingBytes(omm_instances_upload); }
};
struct Imported
{
    AllocationResource *allocation{};
    ResourceHandle handle{};
};
struct BuildJob
{
    std::shared_ptr<Blas> blas;
    RaySceneGeometry geometry;
    ResourceVersion acceleration,scratch,query;
    bool update{},static_build{};
};
struct CompactJob
{
    std::shared_ptr<Blas> source,destination;
    ResourceVersion source_version,destination_version;
};
struct FlightLease
{
    CompletionSet completion;
    std::vector<ComPtr<ID3D12Resource>> inputs;
};
struct PendingScene
{
    const RenderGraphBuilder *graph{};
    std::shared_ptr<SceneFrame> frame;
    std::uint32_t frame_slot{};
    std::vector<Imported> imports;
    std::vector<std::shared_ptr<Blas>> used;
    std::vector<BuildJob> builds;
    std::vector<CompactJob> compactions;
    std::map<StaticKey,std::shared_ptr<Blas>> statics;
    std::map<DynamicKey,std::shared_ptr<Blas>> dynamics;
    FlightLease input_lease;
    std::uint64_t additional_bytes{};
};
} // namespace

struct RaySceneStorage
{
    ComPtr<ID3D12Device5> device;
    D3D12MA::Allocator *allocator{};
    D3D12_RAYTRACING_TIER tier{D3D12_RAYTRACING_TIER_NOT_SUPPORTED};
    std::vector<std::shared_ptr<SceneFrame>> frames;
    std::map<StaticKey,std::shared_ptr<Blas>> statics;
    std::map<DynamicKey,std::shared_ptr<Blas>> dynamics;
    std::vector<std::shared_ptr<Blas>> retired;
    std::vector<FlightLease> flights;
    CompletionSet completion{};
    bool usable{true};
    bool enhanced_barriers{};
    bool use_omms{};
    std::uint64_t native_byte_budget{UINT64_MAX};
    std::uint64_t AllocatedBytes() const
    {
        std::uint64_t result{};
        for(const auto &[key,blas]:statics) result+=blas->AllocatedBytes();
        for(const auto &[key,blas]:dynamics) result+=blas->AllocatedBytes();
        for(const auto &blas:retired)
        {
            // Successful retirement removes entries from the cache. A fault
            // additionally retains touched cached entries, which count once.
            const bool cached=!usable && (
                std::any_of(statics.begin(),statics.end(),[&](const auto &entry) { return entry.second==blas; }) ||
                std::any_of(dynamics.begin(),dynamics.end(),[&](const auto &entry) { return entry.second==blas; }));
            if(!cached) result+=blas->AllocatedBytes();
        }
        for(const auto &frame:frames) if(frame) result+=frame->AllocatedBytes();
        return result;
    }
};
struct RayTracingSceneSystem::Impl
{
    std::shared_ptr<RaySceneStorage> storage;
    PendingScene pending;
};
RayTracingSceneSystem::RayTracingSceneSystem():impl_(std::make_unique<Impl>()){}
RayTracingSceneSystem::~RayTracingSceneSystem()=default;
std::uint64_t RaySceneCandidate::AllocatedBytes() const noexcept
{ return storage_ ? storage_->AllocatedBytes() : 0; }
bool RayTracingSceneSystem::Enabled() const noexcept { return bool(impl_->storage); }
D3D12_RAYTRACING_TIER RayTracingSceneSystem::Tier() const noexcept
{ return impl_->storage ? impl_->storage->tier : D3D12_RAYTRACING_TIER_NOT_SUPPORTED; }
std::uint64_t RayTracingSceneSystem::AllocatedBytes() const noexcept
{
    return (impl_->storage ? impl_->storage->AllocatedBytes() : 0)+impl_->pending.additional_bytes;
}
Result RayTracingSceneSystem::SetNativeByteBudget(std::uint64_t budget)
{
    if(impl_->pending.graph)return Failure(ErrorCode::InvalidState,"Native ray scene budget cannot change during a pending graph.");
    if(auto s=impl_->storage){if(!budget||budget<s->AllocatedBytes())return Failure(ErrorCode::InvalidState,"Native ray scene budget excludes live backing.");s->native_byte_budget=budget;}
    return Result::Success();
}
Result RayTracingSceneSystem::Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,
    bool enabled,std::uint32_t frame_slots,RaySceneCandidate &candidate,bool enhanced_barriers,
    bool use_omms,std::uint64_t native_byte_budget) const
{
    if(!enabled) { candidate.storage_.reset(); return Result::Success(); }
    if(!device || !allocator || !frame_slots || frame_slots>16)
        return Failure(ErrorCode::InvalidArgument,"DXR preparation requires a device, allocator and valid frame slots.");
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
    auto native=device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&options,sizeof(options));
    if(FAILED(native)) return NativeFailure("Query DXR OPTIONS5",native);
    if(options.RaytracingTier==D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
        return Failure(ErrorCode::InvalidState,"The device does not support DXR acceleration structures.");
    if(!native_byte_budget||(use_omms&&options.RaytracingTier<D3D12_RAYTRACING_TIER_1_2))
        return Failure(ErrorCode::InvalidState,"Native OMM geometry requires DXR1.2 and an actual admitted scene byte budget.");
    auto storage=std::make_shared<RaySceneStorage>();
    native=device->QueryInterface(IID_PPV_ARGS(storage->device.ReleaseAndGetAddressOf()));
    if(FAILED(native)) return NativeFailure("Query DXR device interface",native);
    storage->allocator=allocator; storage->tier=options.RaytracingTier; storage->frames.resize(frame_slots);
    storage->enhanced_barriers=enhanced_barriers;storage->use_omms=use_omms;storage->native_byte_budget=native_byte_budget;
    candidate.storage_=std::move(storage); return Result::Success();
}
Result RayTracingSceneSystem::Commit(RaySceneCandidate &&candidate,const CompletionSet &completed)
{
    if(impl_->pending.graph) return Failure(ErrorCode::InvalidState,"Complete the registered ray scene graph before replacement.");
    if(impl_->storage && !impl_->storage->usable)
        return Failure(ErrorCode::InvalidState,"A failed ray scene submission requires ResetAfterCompletion before replacement.");
    if(impl_->storage && !impl_->storage->completion.IsSatisfiedBy(completed))
        return Failure(ErrorCode::InvalidState,"DXR replacement requires all queue leases to complete.");
    impl_->storage=std::move(candidate.storage_); return Result::Success();
}

namespace
{
Result NewBlas(RaySceneStorage &storage,const RaySceneGeometry &geometry,std::shared_ptr<Blas> &output)
{
    const NativeGeometry native_geometry(geometry);
    const auto inputs=BlasInputs(native_geometry.description,geometry.dynamic);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO information{};
    storage.device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs,&information);
    const auto scratch=std::max(information.ScratchDataSizeInBytes,information.UpdateScratchDataSizeInBytes);
    if(!AlignedSize(information.ResultDataMaxSizeInBytes) || !AlignedSize(scratch))
        return Failure(ErrorCode::InvalidState,"The device returned invalid BLAS prebuild requirements.");
    auto blas=std::make_shared<Blas>();
    if(auto result=AllocateAS(storage.allocator,information.ResultDataMaxSizeInBytes,storage.enhanced_barriers,blas->acceleration); !result) return result;
    if(auto result=AllocateScratch(storage.allocator,scratch,blas->scratch); !result) return result;
    if(!geometry.dynamic)
    {
        if(auto result=Allocate(storage.allocator,8,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,Access::ASBuildScratch,blas->query); !result) return result;
        if(auto result=Allocate(storage.allocator,8,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST,Access::CopyDest,blas->readback); !result) return result;
    }
    blas->geometry=geometry; blas->vertices=geometry.vertices; blas->indices=geometry.indices;
    output=std::move(blas); return Result::Success();
}
Result CompactedBlas(RaySceneStorage &storage,const std::shared_ptr<Blas> &source,
    const CompletionSet &completed,std::shared_ptr<Blas> &output)
{
    output.reset();
    if(!source->query_ready || source->query_consumed || source->compacted ||
        !source->query_completion.IsSatisfiedBy(completed)) return Result::Success();
    void *mapped{}; const D3D12_RANGE read{0,8};
    const auto native=source->readback.resource->Map(0,&read,&mapped);
    if(FAILED(native)) return NativeFailure("Map completed BLAS compacted size",native);
    std::uint64_t compacted_size{}; std::memcpy(&compacted_size,mapped,8);
    const D3D12_RANGE unwritten{0,0}; source->readback.resource->Unmap(0,&unwritten);
    if(!AlignedSize(compacted_size) || compacted_size>Bytes(source->acceleration))
        return Failure(ErrorCode::InvalidState,"The GPU returned an invalid BLAS compacted size.");
    if(AlignedSize(compacted_size)>=Bytes(source->acceleration))
    { source->query_consumed=true; return Result::Success(); }
    auto destination=std::make_shared<Blas>();
    if(auto result=AllocateAS(storage.allocator,compacted_size,storage.enhanced_barriers,destination->acceleration); !result) return result;
    destination->geometry=source->geometry; destination->vertices=source->vertices; destination->indices=source->indices;
    destination->revision=source->revision; destination->compacted=true;
    output=std::move(destination); return Result::Success();
}
D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS TlasInputs(std::uint32_t count)
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS result{};
    result.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    result.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY; result.NumDescs=count;
    result.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    return result;
}
Result FrameResources(RaySceneStorage &storage,std::uint32_t count,const std::shared_ptr<SceneFrame> &existing,
    std::shared_ptr<SceneFrame> &output,bool omms)
{
    const auto inputs=TlasInputs(count);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO information{};
    storage.device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs,&information);
    const auto instance_bytes=std::uint64_t(std::max(count,1u))*sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    const auto metadata_bytes=std::uint64_t(std::max(count,1u))*sizeof(RaySceneInstanceMetadata);
    if(!AlignedSize(information.ResultDataMaxSizeInBytes) || !AlignedSize(information.ScratchDataSizeInBytes))
        return Failure(ErrorCode::InvalidState,"The device returned invalid TLAS prebuild requirements.");
    if(existing && Bytes(existing->acceleration)>=information.ResultDataMaxSizeInBytes &&
        Bytes(existing->scratch)>=information.ScratchDataSizeInBytes &&
        Bytes(existing->instances_upload)>=instance_bytes && Bytes(existing->metadata)>=metadata_bytes&&
        (!omms||(Bytes(existing->omm_acceleration)>=information.ResultDataMaxSizeInBytes&&
            Bytes(existing->omm_scratch)>=information.ScratchDataSizeInBytes&&Bytes(existing->omm_instances_upload)>=instance_bytes)))
    { output=existing; return Result::Success(); }
    auto frame=std::make_shared<SceneFrame>();
    if(auto result=AllocateAS(storage.allocator,information.ResultDataMaxSizeInBytes,storage.enhanced_barriers,frame->acceleration); !result) return result;
    if(auto result=AllocateScratch(storage.allocator,information.ScratchDataSizeInBytes,frame->scratch); !result) return result;
    if(auto result=Allocate(storage.allocator,instance_bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_GENERIC_READ,Access::ASBuildInput,frame->instances_upload); !result) return result;
    if(auto result=Allocate(storage.allocator,metadata_bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_GENERIC_READ,Access::CopySource,frame->metadata_upload); !result) return result;
    if(auto result=Allocate(storage.allocator,metadata_bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COMMON,Access::Common,frame->metadata); !result) return result;
    if(omms)
    {
        if(auto result=AllocateAS(storage.allocator,information.ResultDataMaxSizeInBytes,storage.enhanced_barriers,frame->omm_acceleration);!result)return result;
        if(auto result=AllocateScratch(storage.allocator,information.ScratchDataSizeInBytes,frame->omm_scratch);!result)return result;
        if(auto result=Allocate(storage.allocator,instance_bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_GENERIC_READ,Access::ASBuildInput,frame->omm_instances_upload);!result)return result;
    }
    output=std::move(frame); return Result::Success();
}
void ReleaseCompleted(RaySceneStorage &storage,const CompletionSet &completed)
{
    std::erase_if(storage.retired,[&](const auto &blas) { return blas->completion.IsSatisfiedBy(completed); });
    std::erase_if(storage.flights,[&](const auto &flight) { return flight.completion.IsSatisfiedBy(completed); });
    // Initial static build scratch is no longer needed after the size query's
    // full submission has finished; compact destinations never own scratch.
    for(auto &[key,blas]:storage.statics)
        if(blas->built && blas->query_completion.IsSatisfiedBy(completed)) blas->scratch.Reset();
}
Result Commands4(RenderPassContext &context,ComPtr<ID3D12GraphicsCommandList4> &commands)
{
    auto *native=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
    if(!native) return Failure(ErrorCode::InvalidState,"DXR pass has no native command list.");
    const auto result=native->QueryInterface(IID_PPV_ARGS(commands.ReleaseAndGetAddressOf()));
    return FAILED(result) ? NativeFailure("Query DXR command list interface",result) : Result::Success();
}
bool Match(RenderPassContext &context,ResourceVersion version,ID3D12Resource *resource)
{ return context.Resolve(version.resource)==resource; }
} // namespace

Result RayTracingSceneSystem::Register(RenderGraphBuilder &graph,const RaySceneInputs &inputs,RaySceneOutputs &outputs)
{
    outputs={};
    const auto storage=impl_->storage;
    if(!storage) return Result::Success();
    if(!storage->usable || impl_->pending.graph)
        return Failure(ErrorCode::InvalidState,"DXR scene is faulted or has an unfinished registered submission.");
    if(inputs.frame_slot>=storage->frames.size() || inputs.instances.size()>0x1000000u)
        return Failure(ErrorCode::InvalidArgument,"DXR frame slot or 24-bit instance count is invalid.");
    auto existing_frame=storage->frames[inputs.frame_slot];
    if(existing_frame && !existing_frame->completion.IsSatisfiedBy(inputs.completed))
        return Failure(ErrorCode::InvalidState,"DXR frame slot must finish all queue uses before rewriting its uploads or AS.");
    std::set<std::uint32_t> stable_slots;
    std::set<std::uint32_t> validated_geometries;
    for(const auto &instance:inputs.instances)
    {
        if(auto result=ValidateInstance(instance,inputs.geometries.size()); !result) return result;
        if(!stable_slots.insert(instance.stable_gpu_scene_slot).second)
            return Failure(ErrorCode::InvalidArgument,"DXR instance list repeats a stable GPU scene slot.");
        if(validated_geometries.insert(instance.geometry_index).second)
            if(auto result=ValidateGeometry(graph,storage->device.Get(),inputs.geometries[instance.geometry_index]); !result) return result;
    }
    ReleaseCompleted(*storage,inputs.completed);
    PendingScene pending; pending.graph=&graph; pending.frame_slot=inputs.frame_slot;
    const bool omms=storage->use_omms&&std::any_of(inputs.instances.begin(),inputs.instances.end(),[&](const auto &i){return bool(inputs.geometries[i.geometry_index].omm);});
    if(auto result=FrameResources(*storage,static_cast<std::uint32_t>(inputs.instances.size()),existing_frame,pending.frame,omms); !result) return result;
    if(pending.frame!=existing_frame) pending.additional_bytes+=pending.frame->AllocatedBytes();
    std::vector<std::shared_ptr<Blas>> instance_blas;
    std::vector<std::shared_ptr<Blas>> omm_instance_blas;
    std::vector<RayOmmBinding> current_omms;
    std::set<ID3D12Resource*> current_arrays;
    instance_blas.reserve(inputs.instances.size());
    std::set<const Blas*> used;
    // Allocate and validate every candidate before adding any graph passes.
    // Cached addresses are published only by CompleteSubmission(success).
    for(const auto &instance:inputs.instances)
    {
      std::shared_ptr<Blas> ordinary;
      const auto &source_geometry=inputs.geometries[instance.geometry_index];
      if(omms&&source_geometry.omm&&current_arrays.insert(source_geometry.omm.array_resource).second)
          current_omms.push_back(source_geometry.omm);
      const unsigned variants=omms&&source_geometry.omm?2u:1u;
      for(unsigned variant=0;variant<variants;++variant)
      {
        auto geometry=source_geometry;if(variant==0)geometry.omm={};
        std::shared_ptr<Blas> blas;
        bool build=false,update=false;
        if(geometry.dynamic)
        {
            const DynamicKey key{instance.stable_gpu_scene_slot,instance.generation,geometry.stable_key,geometry.generation,inputs.frame_slot,CoverageKey(geometry)};
            const auto found=storage->dynamics.find(key);
            if(found!=storage->dynamics.end() && SameTopology(found->second->geometry,geometry) && !geometry.force_rebuild)
            {
                blas=found->second;
                if(!blas->completion.IsSatisfiedBy(inputs.completed))
                    return Failure(ErrorCode::InvalidState,"A dynamic BLAS slot still has in-flight ray scene consumers.");
                build=!blas->built || blas->revision!=geometry.current_revision ||
                    blas->geometry.vertices!=geometry.vertices || blas->geometry.vertex_byte_offset!=geometry.vertex_byte_offset;
                update=blas->built;
            }
            else
            {
                if(auto result=NewBlas(*storage,geometry,blas); !result) return result;
                pending.additional_bytes+=blas->AllocatedBytes();
                build=true;
            }
            pending.dynamics[key]=blas;
        }
        else
        {
            const StaticKey key{geometry.stable_key,geometry.generation,CoverageKey(geometry)};
            const auto already=pending.statics.find(key);
            if(already!=pending.statics.end())
            {
                blas=already->second;
                if(!SameStaticGeometry(blas->geometry,geometry))
                    return Failure(ErrorCode::InvalidArgument,"An immutable static BLAS key identifies different geometry ranges.");
            }
            else
            {
                const auto found=storage->statics.find(key);
                if(found!=storage->statics.end() && SameStaticGeometry(found->second->geometry,geometry) && !geometry.force_rebuild)
                {
                    blas=found->second;
                    std::shared_ptr<Blas> compacted;
                    if(auto result=CompactedBlas(*storage,blas,inputs.completed,compacted); !result) return result;
                    if(compacted)
                    {
                        pending.compactions.push_back({blas,compacted,{},{}});
                        pending.additional_bytes+=compacted->AllocatedBytes();
                        if(used.insert(blas.get()).second) pending.used.push_back(blas);
                        blas=std::move(compacted);
                    }
                }
                else
                {
                    if(auto result=NewBlas(*storage,geometry,blas); !result) return result;
                    pending.additional_bytes+=blas->AllocatedBytes();
                    build=true;
                }
                pending.statics[key]=blas;
            }
        }
        if(used.insert(blas.get()).second) pending.used.push_back(blas);
        if(variant==0){instance_blas.push_back(blas);ordinary=blas;}else omm_instance_blas.push_back(blas);
        if(build) pending.builds.push_back({blas,geometry,{},{},{},update,!geometry.dynamic});
      }
      if(omms&&variants==1)omm_instance_blas.push_back(ordinary);
    }
    if(storage->AllocatedBytes()>storage->native_byte_budget||
       pending.additional_bytes>storage->native_byte_budget-storage->AllocatedBytes())
        return Failure(ErrorCode::InvalidState,"Actual ordinary/OMM BLAS, twin TLAS and old/in-flight backing exceed the admitted native scene budget before recording.");
    for(bool static_build:{true,false})
    {
        std::map<std::uint32_t,ResourceVersion> input_versions;
        for(const auto &job:pending.builds) if(job.static_build==static_build)
            for(auto version:{job.geometry.vertex_version,job.geometry.index_version})
            {
                const auto [found,inserted]=input_versions.emplace(version.resource.value,version);
                if(!inserted && found->second.ordinal!=version.ordinal)
                    return Failure(ErrorCode::InvalidArgument,"One BLAS build pass cannot read different versions of a physical geometry buffer.");
            }
    }
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> native_instances(std::max<std::size_t>(inputs.instances.size(),1));
    std::vector<RaySceneInstanceMetadata> metadata(std::max<std::size_t>(inputs.instances.size(),1));
    for(std::size_t i=0;i<inputs.instances.size();++i)
    {
        const auto &instance=inputs.instances[i]; auto &native=native_instances[i];
        for(unsigned row=0;row<3;++row) for(unsigned column=0;column<4;++column)
            native.Transform[row][column]=instance.world.m[column][row];
        native.InstanceID=static_cast<UINT>(i); native.InstanceMask=instance.mask;
        native.Flags=instance.flags; native.AccelerationStructure=instance_blas[i]->acceleration.resource->GetGPUVirtualAddress();
        metadata[i]={instance.stable_gpu_scene_slot,instance.generation,instance.material_id,instance.geometry_index,
            static_cast<std::uint32_t>(instance.mesh_key),static_cast<std::uint32_t>(instance.mesh_key>>32),
            static_cast<std::uint32_t>(instance.mesh_generation),static_cast<std::uint32_t>(instance.mesh_generation>>32)};
    }
    if(auto result=Upload(pending.frame->instances_upload,native_instances.data(),native_instances.size()*sizeof(native_instances[0])); !result) return result;
    if(auto result=Upload(pending.frame->metadata_upload,metadata.data(),metadata.size()*sizeof(metadata[0])); !result) return result;
    if(omms)
    {auto native_omms=native_instances;for(std::size_t i=0;i<inputs.instances.size();++i)
        native_omms[i].AccelerationStructure=omm_instance_blas[i]->acceleration.resource->GetGPUVirtualAddress();
        if(auto result=Upload(pending.frame->omm_instances_upload,native_omms.data(),native_omms.size()*sizeof(native_omms[0]));!result)return result;}

    std::map<ID3D12Resource*,ResourceVersion> imports;
    const auto import=[&](AllocationResource &allocation,std::string_view name) {
        const auto found=imports.find(allocation.resource.Get());
        if(found!=imports.end()) return found->second;
        ExternalBuffer external{}; external.native_resource=allocation.resource.Get();
        external.completion=allocation.graph_completion; external.initial_queue=allocation.graph_queue;
        const auto handle=graph.ImportBuffer(external,allocation.graph_access,name);
        const auto version=graph.Version(handle); imports.emplace(allocation.resource.Get(),version);
        pending.imports.push_back({&allocation,handle}); return version;
    };
    for(auto &blas:pending.used) (void)import(blas->acceleration,"RaySceneBLAS");
    const auto tlas_initial=import(pending.frame->acceleration,"RaySceneTLAS");
    auto tlas_scratch=import(pending.frame->scratch,"RaySceneTLASScratch");
    auto instance_upload=import(pending.frame->instances_upload,"RaySceneInstancesUpload");
    auto metadata_upload=import(pending.frame->metadata_upload,"RaySceneMetadataUpload");
    auto metadata_version=import(pending.frame->metadata,"RaySceneMetadata");
    ResourceVersion omm_tlas{},omm_scratch{},omm_upload{};
    if(omms)
    {omm_tlas=import(pending.frame->omm_acceleration,"RaySceneOmmTLAS");omm_scratch=import(pending.frame->omm_scratch,"RaySceneOmmTLASScratch");
        omm_upload=import(pending.frame->omm_instances_upload,"RaySceneOmmInstancesUpload");}

    for(auto &job:pending.builds)
    {
        job.acceleration=imports.at(job.blas->acceleration.resource.Get());
        job.scratch=import(job.blas->scratch,"RaySceneBLASScratch");
        if(job.static_build) job.query=import(job.blas->query,"RaySceneCompactedSize");
        pending.input_lease.inputs.emplace_back(job.geometry.vertices);
        pending.input_lease.inputs.emplace_back(job.geometry.indices);
    }
    const auto register_builds=[&](bool static_build,std::uint32_t id,std::string_view name) {
        std::vector<BuildJob> jobs;
        for(const auto &job:pending.builds) if(job.static_build==static_build) jobs.push_back(job);
        if(jobs.empty()) return;
        auto pass=graph.AddPass(id,name,QueueHint::Direct);
        std::map<std::uint32_t,ResourceVersion> inputs_by_resource;
        for(auto &job:jobs)
        {
            inputs_by_resource.emplace(job.geometry.vertex_version.resource.value,job.geometry.vertex_version);
            inputs_by_resource.emplace(job.geometry.index_version.resource.value,job.geometry.index_version);
            if(job.geometry.omm)
            {pass.Read(job.geometry.omm.array_version,Access::ASBuildRead);pass.Read(job.geometry.omm.index_version,Access::ASBuildInput);}
            job.acceleration=job.update ? pass.ReadWrite(job.acceleration,Access::ASBuildWrite) :
                pass.Write(job.acceleration,Access::ASBuildWrite);
            job.scratch=pass.Write(job.scratch,Access::ASBuildScratch);
            if(static_build) job.query=pass.Write(job.query,Access::ASBuildScratch);
            imports[job.blas->acceleration.resource.Get()]=job.acceleration;
            for(auto &original:pending.builds) if(original.blas==job.blas) original=job;
        }
        for(const auto &[key,version]:inputs_by_resource) pass.Read(version,Access::ASBuildInput);
        pass.SetExecuteChecked([storage,jobs=std::move(jobs)](RenderPassContext &context) {
            ComPtr<ID3D12GraphicsCommandList4> commands;
            if(auto result=Commands4(context,commands); !result) return result;
            for(const auto &job:jobs)
            {
                if(!Match(context,job.geometry.vertex_version,job.geometry.vertices) ||
                    !Match(context,job.geometry.index_version,job.geometry.indices) ||
                    !Match(context,job.acceleration,job.blas->acceleration.resource.Get()) ||
                    !Match(context,job.scratch,job.blas->scratch.resource.Get()) ||
                    (job.static_build && !Match(context,job.query,job.blas->query.resource.Get()))||
                    (job.geometry.omm&&(!Match(context,job.geometry.omm.array_version,job.geometry.omm.array_resource)||
                        !Match(context,job.geometry.omm.index_version,job.geometry.omm.index_resource))))
                    return Failure(ErrorCode::InvalidState,"BLAS graph versions disagree with their native build resources.");
                const NativeGeometry native_geometry(job.geometry);
                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC description{};
                description.Inputs=BlasInputs(native_geometry.description,job.geometry.dynamic);
                description.DestAccelerationStructureData=job.blas->acceleration.resource->GetGPUVirtualAddress();
                description.ScratchAccelerationStructureData=job.blas->scratch.resource->GetGPUVirtualAddress();
                if(job.update)
                {
                    description.Inputs.Flags|=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
                    description.SourceAccelerationStructureData=description.DestAccelerationStructureData;
                }
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC query{};
                query.InfoType=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
                if(job.static_build) query.DestBuffer=job.blas->query.resource->GetGPUVirtualAddress();
                commands->BuildRaytracingAccelerationStructure(&description,job.static_build ? 1u : 0u,
                    job.static_build ? &query : nullptr);
            }
            return Result::Success();
        });
    };
    register_builds(true,120,"RaySceneStaticBLASBuild");
    struct SizeCopy { std::shared_ptr<Blas> blas; ResourceVersion source,destination; };
    std::vector<SizeCopy> size_copies;
    for(const auto &job:pending.builds) if(job.static_build)
        size_copies.push_back({job.blas,job.query,import(job.blas->readback,"RaySceneCompactedSizeReadback")});
    if(!size_copies.empty())
    {
        auto pass=graph.AddPass(121,"RaySceneCompactedSizeReadback",QueueHint::Direct);
        for(auto &copy:size_copies)
        {
            pass.Read(copy.source,Access::CopySource);
            copy.destination=pass.Write(copy.destination,Access::CopyDest);
        }
        pass.SetExecuteChecked([storage,copies=std::move(size_copies)](RenderPassContext &context) {
            auto *commands=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            if(!commands) return Failure(ErrorCode::InvalidState,"BLAS size readback has no native command list.");
            for(const auto &copy:copies)
            {
                if(!Match(context,copy.source,copy.blas->query.resource.Get()) ||
                    !Match(context,copy.destination,copy.blas->readback.resource.Get()))
                    return Failure(ErrorCode::InvalidState,"BLAS compacted-size graph resources disagree with native buffers.");
                commands->CopyBufferRegion(copy.blas->readback.resource.Get(),0,copy.blas->query.resource.Get(),0,8);
            }
            return Result::Success();
        });
    }
    if(!pending.compactions.empty())
    {
        auto pass=graph.AddPass(122,"RaySceneStaticBLASCompact",QueueHint::Direct);
        for(auto &job:pending.compactions)
        {
            job.source_version=imports.at(job.source->acceleration.resource.Get());
            pass.Read(job.source_version,Access::ASCopyRead);
            job.destination_version=pass.Write(imports.at(job.destination->acceleration.resource.Get()),Access::ASCopyWrite);
            imports[job.destination->acceleration.resource.Get()]=job.destination_version;
        }
        pass.SetExecuteChecked([storage,jobs=pending.compactions](RenderPassContext &context) {
            ComPtr<ID3D12GraphicsCommandList4> commands;
            if(auto result=Commands4(context,commands); !result) return result;
            for(const auto &job:jobs)
            {
                if(!Match(context,job.source_version,job.source->acceleration.resource.Get()) ||
                    !Match(context,job.destination_version,job.destination->acceleration.resource.Get()))
                    return Failure(ErrorCode::InvalidState,"BLAS compaction graph versions disagree with native acceleration structures.");
                commands->CopyRaytracingAccelerationStructure(job.destination->acceleration.resource->GetGPUVirtualAddress(),
                    job.source->acceleration.resource->GetGPUVirtualAddress(),D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
            }
            return Result::Success();
        });
    }
    register_builds(false,123,"RaySceneDynamicBLASBuildUpdate");
    {
        auto pass=graph.AddPass(124,"RaySceneMetadataUpload",QueueHint::Direct);
        pass.Read(metadata_upload,Access::CopySource);
        metadata_version=pass.Write(metadata_version,Access::CopyDest);
        pass.SetExecuteChecked([storage,frame=pending.frame,metadata_upload,metadata_version,
            bytes=metadata.size()*sizeof(metadata[0])](RenderPassContext &context) {
            auto *commands=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            if(!commands || !Match(context,metadata_upload,frame->metadata_upload.resource.Get()) ||
                !Match(context,metadata_version,frame->metadata.resource.Get()))
                return Failure(ErrorCode::InvalidState,"Ray metadata upload graph resources disagree with native buffers.");
            commands->CopyBufferRegion(frame->metadata.resource.Get(),0,frame->metadata_upload.resource.Get(),0,bytes);
            return Result::Success();
        });
    }
    auto tlas=tlas_initial;
    {
        auto pass=graph.AddPass(125,"RaySceneTLASBuild",QueueHint::Direct);
        pass.Read(instance_upload,Access::ASBuildInput);
        std::set<const Blas*> read;
        for(const auto &blas:instance_blas)
            if(read.insert(blas.get()).second) pass.Read(imports.at(blas->acceleration.resource.Get()),Access::ASBuildRead);
        if(omms)
        {
            pass.Read(omm_upload,Access::ASBuildInput);
            for(const auto &blas:omm_instance_blas)
                if(read.insert(blas.get()).second)pass.Read(imports.at(blas->acceleration.resource.Get()),Access::ASBuildRead);
            for(const auto &binding:current_omms)pass.Read(binding.array_version,Access::ASBuildRead);
            omm_tlas=pass.Write(omm_tlas,Access::ASBuildWrite);omm_scratch=pass.Write(omm_scratch,Access::ASBuildScratch);
        }
        tlas=pass.Write(tlas,Access::ASBuildWrite);
        tlas_scratch=pass.Write(tlas_scratch,Access::ASBuildScratch);
        pass.SetExecuteChecked([storage,frame=pending.frame,tlas,tlas_scratch,instance_upload,omms,omm_tlas,omm_scratch,omm_upload,
            count=static_cast<std::uint32_t>(inputs.instances.size())](RenderPassContext &context) {
            if(!Match(context,tlas,frame->acceleration.resource.Get()) ||
                !Match(context,tlas_scratch,frame->scratch.resource.Get()) ||
                !Match(context,instance_upload,frame->instances_upload.resource.Get()))
                return Failure(ErrorCode::InvalidState,"TLAS graph versions disagree with native build resources.");
            ComPtr<ID3D12GraphicsCommandList4> commands;
            if(auto result=Commands4(context,commands); !result) return result;
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC description{};
            description.Inputs=TlasInputs(count);
            description.Inputs.InstanceDescs=frame->instances_upload.resource->GetGPUVirtualAddress();
            description.DestAccelerationStructureData=frame->acceleration.resource->GetGPUVirtualAddress();
            description.ScratchAccelerationStructureData=frame->scratch.resource->GetGPUVirtualAddress();
            commands->BuildRaytracingAccelerationStructure(&description,0,nullptr);
            if(omms)
            {
                if(!Match(context,omm_tlas,frame->omm_acceleration.resource.Get())||
                   !Match(context,omm_scratch,frame->omm_scratch.resource.Get())||!Match(context,omm_upload,frame->omm_instances_upload.resource.Get()))
                    return Failure(ErrorCode::InvalidState,"OMM twin TLAS graph/native resources differ.");
                description.Inputs.InstanceDescs=frame->omm_instances_upload.resource->GetGPUVirtualAddress();
                description.DestAccelerationStructureData=frame->omm_acceleration.resource->GetGPUVirtualAddress();
                description.ScratchAccelerationStructureData=frame->omm_scratch.resource->GetGPUVirtualAddress();
                commands->BuildRaytracingAccelerationStructure(&description,0,nullptr);
            }
            return Result::Success();
        });
    }
    graph.SetFinalAccess(tlas,Access::ASRead);
    graph.SetFinalAccess(metadata_version,Access::AllShaderRead);
    outputs={tlas,metadata_version,pending.frame->acceleration.resource.Get(),pending.frame->metadata.resource.Get(),
        pending.frame->acceleration.resource->GetGPUVirtualAddress(),static_cast<std::uint32_t>(inputs.instances.size())};
    if(storage->use_omms)
    {
        outputs.omm_view={omms?omm_tlas:tlas,omms?pending.frame->omm_acceleration.resource.Get():pending.frame->acceleration.resource.Get(),
            (omms?pending.frame->omm_acceleration:pending.frame->acceleration).resource->GetGPUVirtualAddress(),omms,{}};
        if(omms){graph.SetFinalAccess(omm_tlas,Access::ASRead);outputs.omm_view.backing=std::move(current_omms);}
    }
    impl_->pending=std::move(pending); return Result::Success();
}

Result RayTracingSceneSystem::CompleteSubmission(const RenderGraphBuilder &graph,const CompletionSet &completion,bool success)
{
    auto &pending=impl_->pending; const auto storage=impl_->storage;
    if(!pending.graph)return Result::Success();
    if(!storage || pending.graph!=&graph)
        return Failure(ErrorCode::InvalidArgument,"Ray scene completion has no matching registered graph.");
    storage->completion.Merge(completion); pending.frame->completion.Merge(completion);
    for(const auto &imported:pending.imports)
    {
        const auto required=graph.ResourceCompletion(imported.handle);
        imported.allocation->graph_completion.Merge(required);
        imported.allocation->graph_completion.Merge(completion);
        storage->completion.Merge(required); pending.frame->completion.Merge(required);
        if(success)
        {
            imported.allocation->graph_access=graph.FinalAccess(imported.handle);
            imported.allocation->graph_queue=graph.FinalQueue(imported.handle);
        }
    }
    // A TLAS traversal reads its BLAS transitively; graph consumers only name
    // the TLAS. Every referenced BLAS therefore retains the full frame's Direct,
    // Compute and Copy requirements, not merely its own build-pass fence.
    for(const auto &blas:pending.used)
    { blas->completion.Merge(completion); blas->completion.Merge(storage->completion); }
    pending.input_lease.completion=storage->completion;
    storage->flights.push_back(std::move(pending.input_lease));
    if(success)
    {
        for(const auto &job:pending.builds)
        {
            job.blas->built=true; job.blas->revision=job.geometry.current_revision;
            job.blas->geometry=job.geometry; job.blas->vertices=job.geometry.vertices; job.blas->indices=job.geometry.indices;
            if(job.static_build)
            {
                job.blas->query_ready=true;
                job.blas->query_completion.Merge(completion);
                job.blas->query_completion.Merge(job.blas->readback.graph_completion);
            }
        }
        for(const auto &job:pending.compactions)
        { job.destination->built=true; job.source->query_consumed=true; }
        const auto retire=[&](const auto &blas) {
            if(std::find(storage->retired.begin(),storage->retired.end(),blas)==storage->retired.end())
                storage->retired.push_back(blas);
        };
        for(const auto &[key,blas]:storage->statics)
        {
            const auto found=pending.statics.find(key);
            if(found==pending.statics.end() || found->second!=blas) retire(blas);
        }
        // Preserve other frame slots of live dynamic instances, but retire old
        // generations and removed instances only after their accumulated uses.
        std::set<std::tuple<std::uint32_t,std::uint32_t,std::uint64_t,std::uint64_t,OmmKey>> live_dynamic;
        for(const auto &[key,blas]:pending.dynamics)
            live_dynamic.emplace(std::get<0>(key),std::get<1>(key),std::get<2>(key),std::get<3>(key),std::get<5>(key));
        for(auto iterator=storage->dynamics.begin();iterator!=storage->dynamics.end();)
        {
            const auto &[key,blas]=*iterator;
            const auto stable_slot=std::get<0>(key),generation=std::get<1>(key);
            const bool live=live_dynamic.contains({stable_slot,generation,std::get<2>(key),std::get<3>(key),std::get<5>(key)});
            const auto replacement=pending.dynamics.find(key);
            if(!live || (replacement!=pending.dynamics.end() && replacement->second!=blas))
            { retire(blas); iterator=storage->dynamics.erase(iterator); }
            else ++iterator;
        }
        storage->statics=std::move(pending.statics);
        for(auto &[key,blas]:pending.dynamics) storage->dynamics[key]=std::move(blas);
        storage->frames[pending.frame_slot]=pending.frame;
    }
    else
    {
        storage->usable=false;
        // New candidates can have been touched by partially submitted batches.
        // Retain them together with unpublished TLAS/metadata until a full drain.
        for(const auto &blas:pending.used)
            if(std::find(storage->retired.begin(),storage->retired.end(),blas)==storage->retired.end()) storage->retired.push_back(blas);
        storage->frames[pending.frame_slot]=pending.frame;
    }
    pending={}; return Result::Success();
}
Result RayTracingSceneSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if(impl_->pending.graph) return Failure(ErrorCode::InvalidState,"Complete the registered ray scene graph before reset.");
    if(impl_->storage && !impl_->storage->completion.IsSatisfiedBy(completed))
        return Failure(ErrorCode::InvalidState,"DXR reset requires all Direct, Compute and Copy leases to complete.");
    impl_->storage.reset(); return Result::Success();
}
} // namespace hs
