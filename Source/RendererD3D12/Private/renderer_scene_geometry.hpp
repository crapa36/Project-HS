#pragma once

#include "render_graph.hpp"
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
// CPU inputs for native software SDF geometry.
struct SoftwareSceneGeometry
{
    std::uint64_t stable_key{}, generation{}, current_revision{};
    ID3D12Resource *vertices{}, *indices{};
    ResourceVersion vertex_version{}, index_version{};
    std::uint64_t vertex_byte_offset{}, index_byte_offset{};
    std::uint32_t vertex_stride{}, vertex_count{}, index_count{};
    DXGI_FORMAT index_format{DXGI_FORMAT_R32_UINT};
    bool opaque{true}, dynamic{}, force_rebuild{};
};
struct SoftwareSceneInstance
{
    std::uint32_t stable_gpu_scene_slot{}, generation{}, geometry_index{}, material_id{};
    std::uint64_t mesh_key{}, mesh_generation{};
    DirectX::XMFLOAT4X4 world{};
};

// Append these ranges to the existing compute-visible frame table, space20:
// CBV b0 at0, SRV t3 at1, SRV t14 at2, UAV u0 at3. No new root parameter.
inline constexpr std::uint32_t kSceneGeometryDescriptorCount=4;
enum SceneGeometryFlags : std::uint32_t
{
    SceneGeometryDeformed=1, SceneGeometryCachedSkin=2, SceneGeometryTwoSided=4,
    SceneGeometryBuiltin=16,
    SceneGeometryTransmissive=32
};
// HLSL SceneGeometryRecord stride56. Offsets/counts are byte-address
// representable; indices are R32_UINT canonical indices, relative to the page.
// SceneGeometryTransmissive is for a homogeneous unsupported transport domain;
// mixed hero pages instead resolve each vertex's actual opaque binding ordinal.
struct SceneGeometryRecord
{
    std::uint32_t vertex_descriptor{},index_descriptor{},first_index{},index_count{};
    std::uint32_t vertex_byte_offset{},vertex_stride{76},vertex_count{},position_byte_offset{};
    std::uint32_t stable_gpu_scene_slot{},instance_generation{},skin_slot{},mesh_id{};
    std::uint32_t environment_ordinal{~0u},flags{};
};
static_assert(sizeof(SceneGeometryRecord)==56);
static_assert(offsetof(SceneGeometryRecord,vertex_byte_offset)==16);
static_assert(offsetof(SceneGeometryRecord,stable_gpu_scene_slot)==32);
static_assert(offsetof(SceneGeometryRecord,environment_ordinal)==48);
struct SceneDrawInstance
{
    DirectX::XMFLOAT4 position{},scale{};
    std::uint32_t color{},mesh{};
    float yaw{},padding{};
};
static_assert(sizeof(SceneDrawInstance)==48);
struct SceneShaderRead
{
    ResourceVersion version{};
    ID3D12Resource *resource{};
    Access access{Access::ComputeRead};
};
// The caller binds the main compute root signature and all referenced tables,
// FrameCB, native skin/material resources and sampler heaps. Its table32 must
// contain the service's descriptor_suffix at the appended space20 ranges.
// The callback runs after the service sets the PSO; it must not replace the PSO.
using SceneShaderBinder=std::function<Result(ID3D12GraphicsCommandList *)>;
using SceneFrameTableBuilder=std::function<Result(D3D12_CPU_DESCRIPTOR_HANDLE,std::uint32_t,SceneShaderBinder &)>;
using SceneShaderReader=std::function<Result(std::string_view,std::vector<std::byte> &)>;
struct SceneGeometryInputs
{
    std::span<const SceneGeometryRecord> geometries;
    std::span<const SceneDrawInstance> instances; // exact draw transform, not compact/camera refs
    std::span<const SceneShaderRead> reads; // canonical vertex/index, current skin, FrameCB, material set
    // Called once after all4 descriptors are valid. Copy them into a private
    // frame-table32 suffix and return a binder retaining that table/heap lease.
    SceneFrameTableBuilder make_bindings;
    std::uint32_t frame_slot{};
    CompletionSet completed{};
};
struct SceneGeometryOutputs
{
    ResourceVersion positions{};
    ID3D12Resource *position_resource{};
    // Each deformed job stores vertex_count local float3 positions (12B stride).
    // Returned offsets replace input records for software SDF geometry reads.
    std::vector<SceneGeometryRecord> geometries;
    std::shared_ptr<const void> owner; // actual service allocations, retained by native SDF registrations
};
struct SceneGeometryStorage;
class SceneGeometryCandidate
{
    std::shared_ptr<SceneGeometryStorage> storage_;
    friend class SceneGeometrySystem;
};
class SceneGeometrySystem
{
  public:
    SceneGeometrySystem();
    ~SceneGeometrySystem();
    [[nodiscard]] Result Prepare(ID3D12Device *,D3D12MA::Allocator *,ID3D12RootSignature *,
        bool enabled,std::uint32_t frame_slots,const SceneShaderReader &,SceneGeometryCandidate &) const;
    [[nodiscard]] Result Commit(SceneGeometryCandidate &&,const CompletionSet &);
    [[nodiscard]] Result Register(RenderGraphBuilder &,const SceneGeometryInputs &,SceneGeometryOutputs &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &,const CompletionSet &,bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    // Called only after all old submissions have reliably completed.
    void SwapState(SceneGeometrySystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
