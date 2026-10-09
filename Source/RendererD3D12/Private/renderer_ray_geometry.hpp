#pragma once

#include "renderer_ray_scene.hpp"
#include <d3d12.h>
#include <DirectXMath.h>
#include <functional>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace hs
{
// Append these ranges to the existing compute-visible frame table, space20:
// CBV b0 at0, SRV t0..14 at1, UAV u0..4 at16. No new root parameter.
inline constexpr std::uint32_t kRayDescriptorCount=21;
enum RayGeometryFlags : std::uint32_t
{
    RayGeometryDeformed=1, RayGeometryCachedSkin=2, RayGeometryTwoSided=4,
    RayGeometryAuthoredEnvironment=8, RayGeometryBuiltin=16,
    RayGeometryTransmissive=32
};
// HLSL RayHitGeometry/RayDeformJob stride80. Offsets/counts are byte-address
// representable; indices are R32_UINT canonical indices, relative to the page.
// RayGeometryTransmissive is for a homogeneous unsupported transport domain;
// mixed hero pages instead resolve each vertex's actual opaque binding ordinal.
struct RayHitGeometry
{
    std::uint32_t vertex_descriptor{},index_descriptor{},first_index{},index_count{};
    std::uint32_t vertex_byte_offset{},vertex_stride{76},vertex_count{},position_byte_offset{};
    std::uint32_t stable_gpu_scene_slot{},instance_generation{},skin_slot{},mesh_id{};
    std::uint32_t environment_ordinal{~0u},material_word{},flags{},geometry_generation{};
    std::uint32_t mesh_key_low{},mesh_key_high{},mesh_generation_low{},mesh_generation_high{};
};
static_assert(sizeof(RayHitGeometry)==80);
static_assert(offsetof(RayHitGeometry,vertex_byte_offset)==16);
static_assert(offsetof(RayHitGeometry,stable_gpu_scene_slot)==32);
static_assert(offsetof(RayHitGeometry,environment_ordinal)==48);
static_assert(offsetof(RayHitGeometry,mesh_key_low)==64);
struct RayDrawInstance
{
    DirectX::XMFLOAT4 position{},scale{};
    std::uint32_t color{},mesh{};
    float yaw{},padding{};
};
static_assert(sizeof(RayDrawInstance)==48);
struct RayShaderRead
{
    ResourceVersion version{};
    ID3D12Resource *resource{};
    Access access{Access::ComputeRead};
};
// The caller binds the main compute root signature and all referenced tables,
// FrameCB, native skin/material resources and sampler heaps. Its table32 must
// contain the service's descriptor_suffix at the appended space20 ranges.
// The callback runs after the service sets the PSO; it must not replace the PSO.
using RayShaderBinder=std::function<Result(ID3D12GraphicsCommandList *)>;
using RayFrameTableBuilder=std::function<Result(D3D12_CPU_DESCRIPTOR_HANDLE,std::uint32_t,RayShaderBinder &)>;
using RayShaderReader=std::function<Result(std::string_view,std::vector<std::byte> &)>;
struct RayGeometryInputs
{
    std::span<const RayHitGeometry> geometries;
    std::span<const RayDrawInstance> instances; // exact draw transform, not compact/camera refs
    std::span<const RayShaderRead> reads; // canonical vertex/index, current skin, FrameCB, material set
    // Called once after all21 descriptors are valid. Copy them into a private
    // frame-table32 suffix and return a binder retaining that table/heap lease.
    RayFrameTableBuilder make_bindings;
    std::uint32_t frame_slot{};
    CompletionSet completed{};
};
struct RayGeometryOutputs
{
    ResourceVersion positions{},records{},instances{};
    ID3D12Resource *position_resource{},*record_resource{},*instance_resource{};
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor_suffix{};
    std::uint32_t descriptor_count{kRayDescriptorCount},geometry_count{};
    // Register assigns compact 12B offsets to deformed jobs. The returned
    // records are the exact AS candidate layout and must replace input records.
    std::vector<RayHitGeometry> geometries;
    std::shared_ptr<const void> owner; // actual service allocations, retained by native SDF registrations
};
struct RayGeometryStorage;
class RayGeometryCandidate
{
    std::shared_ptr<RayGeometryStorage> storage_;
    friend class RayGeometrySystem;
};
class RayGeometrySystem
{
  public:
    RayGeometrySystem();
    ~RayGeometrySystem();
    [[nodiscard]] Result Prepare(ID3D12Device *,D3D12MA::Allocator *,ID3D12RootSignature *,
        bool enabled,std::uint32_t frame_slots,const RayShaderReader &,RayGeometryCandidate &) const;
    [[nodiscard]] Result Commit(RayGeometryCandidate &&,const CompletionSet &);
    [[nodiscard]] Result Register(RenderGraphBuilder &,const RayGeometryInputs &,RayGeometryOutputs &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &,const CompletionSet &,bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    // Called only after all old submissions have reliably completed.
    void SwapState(RayGeometrySystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
