#pragma once

#include "render_graph.hpp"
#include "renderer_ray_omm.hpp"

#include <DirectXMath.h>
#include <d3d12.h>

#include <cstdint>
#include <memory>
#include <span>

namespace hs
{
// One indexed triangle range. Positions are float3 at vertex_byte_offset and
// every vertex_stride bytes thereafter (the current SkinCache stride is 76).
// Static keys identify immutable mesh/LOD pages; generation changes invalidate
// their cache. Dynamic keys identify a stable instance's current geometry.
// Indices are relative to this vertex range; index contents/topology are immutable
// within a generation. current_revision changes when current positions change.
struct RaySceneGeometry
{
    std::uint64_t stable_key{}, generation{}, current_revision{};
    ID3D12Resource *vertices{}, *indices{};
    ResourceVersion vertex_version{}, index_version{};
    std::uint64_t vertex_byte_offset{}, index_byte_offset{};
    std::uint32_t vertex_stride{}, vertex_count{}, index_count{};
    DXGI_FORMAT index_format{DXGI_FORMAT_R32_UINT};
    bool opaque{true}, dynamic{};
    // A measured/producer decision to replace an update chain with a new build.
    bool force_rebuild{};
    RayOmmBinding omm; // optional immutable canonical UV/topology/material coverage
};

struct RaySceneInstance
{
    std::uint32_t stable_gpu_scene_slot{}, generation{}, geometry_index{}, material_id{};
    std::uint64_t mesh_key{}, mesh_generation{};
    // DirectX row-vector affine local-to-world matrix; the service transposes
    // its first three columns into DXR's 3x4 Transform, including translation.
    DirectX::XMFLOAT4X4 world{};
    std::uint32_t flags{D3D12_RAYTRACING_INSTANCE_FLAG_NONE};
    std::uint8_t mask{0xff};
};

// StructuredBuffer stride 32. DXR InstanceID is the dense metadata index; the
// complete stable scene identity and material/mesh generation survive there.
struct RaySceneInstanceMetadata
{
    std::uint32_t stable_gpu_scene_slot{}, generation{}, material_id{}, geometry_index{};
    std::uint32_t mesh_key_low{}, mesh_key_high{}, mesh_generation_low{}, mesh_generation_high{};
};
static_assert(sizeof(RaySceneInstanceMetadata)==32);

struct RaySceneInputs
{
    std::span<const RaySceneGeometry> geometries;
    std::span<const RaySceneInstance> instances;
    std::uint32_t frame_slot{};
    CompletionSet completed{};
};

struct RaySceneAccelerationView
{
    ResourceVersion tlas{};
    ID3D12Resource *resource{};
    D3D12_GPU_VIRTUAL_ADDRESS gpu_address{};
    bool contains_omms{};
    std::vector<RayOmmBinding> backing; // explicit native array/index graph reads + leases
};
struct RaySceneOutputs
{
    ResourceVersion tlas{}, metadata{};
    ID3D12Resource *tlas_resource{}, *metadata_resource{};
    D3D12_GPU_VIRTUAL_ADDRESS tlas_address{};
    std::uint32_t instance_count{};
    RaySceneAccelerationView omm_view;
    // Use an actual RAYTRACING_ACCELERATION_STRUCTURE SRV or root SRV for tlas;
    // its opaque GPU address is never a normal structured/byte-address buffer.
};

struct RaySceneStorage;
class RaySceneCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
  private:
    std::shared_ptr<RaySceneStorage> storage_;
    friend class RayTracingSceneSystem;
};

// The caller supplies all trace-eligible instances, including offscreen ones.
// Register declares SkinCache reads, BLAS build/query/compaction and TLAS work.
// Call CompleteSubmission on every registered graph, including early failures.
// Partial submission faults the service; drain all queues and ResetAfterCompletion
// before replacing it. No ray tracing consumer or denoiser is owned here.
class RayTracingSceneSystem
{
  public:
    RayTracingSceneSystem();
    ~RayTracingSceneSystem();
    RayTracingSceneSystem(const RayTracingSceneSystem&)=delete;
    RayTracingSceneSystem &operator=(const RayTracingSceneSystem&)=delete;
    [[nodiscard]] Result Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,
        bool enabled,std::uint32_t frame_slots,RaySceneCandidate &candidate,
        bool enhanced_barriers,bool use_omms,std::uint64_t native_byte_budget) const;
    [[nodiscard]] Result Commit(RaySceneCandidate &&candidate,const CompletionSet &completed);
    [[nodiscard]] Result Register(RenderGraphBuilder &graph,const RaySceneInputs &inputs,
        RaySceneOutputs &outputs);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &graph,
        const CompletionSet &completion,bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &completed);
    [[nodiscard]] bool Enabled() const noexcept;
    [[nodiscard]] D3D12_RAYTRACING_TIER Tier() const noexcept;
    [[nodiscard]] Result SetNativeByteBudget(std::uint64_t);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    // Called only after all old submissions have reliably completed.
    void SwapState(RayTracingSceneSystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
