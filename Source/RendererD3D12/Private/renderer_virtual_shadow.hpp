#pragma once

#include "render_graph.hpp"
#include "view_state.hpp"
#include <hs/core/render_snapshot.hpp>
#include <DirectXMath.h>
#include <d3d12.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace hs
{
class RendererPipelineCache;
struct AllocationResource;
inline constexpr std::uint32_t kVirtualShadowInner = 128, kVirtualShadowGuard = 2;
inline constexpr std::uint32_t kVirtualShadowTile = 132, kVirtualShadowLevels = 3;
inline constexpr std::uint32_t kVirtualShadowPassBit = 0x80000000u;
// Append these five SRVs to graphics table32, t0..t4/space21. No new graphics
// root parameter/CBV. Virtual draw signatures set only PassValue (root6 DWORD0).
inline constexpr std::uint32_t kVirtualShadowMetadataOffset = 0;
inline constexpr std::uint32_t kVirtualShadowPageTableOffset = 1;
inline constexpr std::uint32_t kVirtualShadowStaticBankOffset = 2;
inline constexpr std::uint32_t kVirtualShadowDynamicBankOffset = 3;
inline constexpr std::uint32_t kVirtualShadowDrawRecordsOffset = 4;
inline constexpr std::uint32_t kVirtualShadowDescriptorCount = 5;

struct VirtualShadowCacheStatus
{
    std::uint64_t requested_bytes{}, admitted_bytes{};
    std::uint32_t physical_pages{};
};
enum class VirtualShadowFrontend : std::uint32_t { Indexed, Mesh };
struct VirtualShadowProfile
{
    bool enabled{};
    std::uint64_t cache_budget_bytes{}; // Authored capacity for both native guarded depth banks.
    std::uint64_t gpu_budget_bytes{}; // Entire service, including all frame buffers.
    std::uint32_t physical_pages{}, max_casters{}, max_receivers{}, max_draw_groups{};
    std::uint32_t filter_samples{};
    std::uint32_t command_capacity{}, frame_slots{3};
    std::array<std::uint32_t,3> virtual_sides{32,8,8}; // Pages per clipmap side.
    ViewExtent render_capacity;
};
// Each group has exactly one native geometry/material/scene binding and frontend.
// Caller chooses its PSO and native IA/skin/material bindings using binding_key.
struct VirtualShadowDrawGroup
{
    std::uint64_t binding_key{};
    VirtualShadowFrontend frontend{VirtualShadowFrontend::Indexed};
};
struct VirtualShadowCaster
{
    DirectX::XMFLOAT4 current_bounds{}, previous_bounds{}; // Conservative world spheres.
    RenderInstance::Revisions current_revisions{}, previous_revisions{};
    std::uint64_t current_cut_revision{}, previous_cut_revision{};
    std::uint32_t instance_index{}, instance_generation{}, previous_generation{};
    std::uint32_t source_ref{}, geometry_record{}, draw_group{};
    std::uint32_t index_count{}, first_index{}, meshlet_count{};
    std::int32_t base_vertex{};
    bool present{true}, previous_present{};
    bool immutable_source{}, previous_immutable_source{};
    bool skin{}, wind{}, animated_alpha{};
    bool previous_skin{}, previous_wind{}, previous_animated_alpha{};
};
struct VirtualShadowReceiver
{
    // World AABB. minimum.w: 0 requests pages and extends depth; 1 extends depth only.
    // maximum.w remains finite reserved padding. Keep the 32-byte GPU payload.
    DirectX::XMFLOAT4 minimum{}, maximum{};
};
struct alignas(16) GpuShadowPageEntry
{
    std::int32_t x{}, y{};
    std::uint32_t level{}, physical{UINT32_MAX};
    std::uint32_t epoch{}, flags{}, reserved0{}, reserved1{};
};
struct alignas(16) GpuShadowDrawRecord
{
    std::uint32_t geometry_record{}, source_ref{}, instance_index{}, instance_generation{};
    std::uint32_t page_entry{}, layer{}, draw_group{}, reserved{};
};
static_assert(sizeof(GpuShadowPageEntry)==32);
static_assert(sizeof(GpuShadowDrawRecord)==32);
// Compute b0/space21 and raw t0/space21 metadata have the same 336-byte payload.
// Inverse VP is camera-relative, row-major (same as FrameConstants); view_origin
// is added after reconstruction. Absolute light-space XY keys never follow jitter.
struct alignas(16) VirtualShadowConstants
{
    DirectX::XMFLOAT4X4 inverse_view_projection{};
    DirectX::XMFLOAT4 light_right{}, light_up{}, light_forward{};
    DirectX::XMFLOAT4 bank{}; // Width, height, columns, 132.
    DirectX::XMFLOAT4 depth{}; // Stable light-space minimum Z, inverse span, receiver bias, span.
    DirectX::XMFLOAT4 view_origin{};
    std::array<DirectX::XMFLOAT4,3> clipmaps{}; // Page world side, world texel, extent, reserved.
    std::array<DirectX::XMINT4,3> windows{}; // Absolute minimum page XY, side, table base.
    DirectX::XMUINT4 counts{}; // Casters, receivers, virtual entries, physical pages.
    DirectX::XMUINT4 limits{}; // First changed static caster (UINT32_MAX if none), groups, active width, active height.
    DirectX::XMUINT4 identity{}; // Light stable ID lo/hi, generation, depth epoch.
    DirectX::XMUINT4 frame{}; // Rendered frame ID lo/hi, request words, active draw-group segment count.
    DirectX::XMUINT4 options{}; // Reset cache, enabled, segment capacity, filter samples.
};
static_assert(sizeof(VirtualShadowConstants)==336);
static_assert(offsetof(VirtualShadowConstants,clipmaps)==160);
static_assert(offsetof(VirtualShadowConstants,windows)==208);
static_assert(offsetof(VirtualShadowConstants,counts)==256);

struct VirtualShadowNativeRead { ResourceVersion version; Access access{Access::ShaderRead}; };
struct VirtualShadowDrawBatch
{
    VirtualShadowDrawGroup group;
    std::uint32_t group_index{}, layer{}, maximum_commands{};
    std::uint64_t argument_offset{}, count_offset{};
};
struct VirtualShadowOutputs
{
    ResourceVersion metadata, page_table, static_bank, dynamic_bank, draw_records;
    ResourceVersion indexed_commands, mesh_commands, command_counts;
    std::array<ID3D12Resource *,5> resources{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE,5> srvs{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE,2> depth_views{};
    ID3D12Resource *indexed_arguments{}, *mesh_arguments{}, *counts_resource{};
    ViewExtent bank_extent;
    std::uint32_t descriptor_count{kVirtualShadowDescriptorCount};
};
struct VirtualShadowInputs
{
    LightView light; // Currently selected requested Directional; lookup checks its stable identity.
    DirectX::XMFLOAT3 clipmap_center{}, view_origin{};
    DirectX::XMFLOAT4X4 inverse_view_projection{}; // Camera-relative, row-major.
    std::array<float,3> coverage_extents{};
    float depth_span{}; // Stable epoch span covering off-camera casters; not camera near/far.
    ViewExtent render_active;
    std::uint32_t frame_slot{};
    std::uint64_t rendered_frame_id{};
    CompletionSet completed;
    ResourceVersion depth, normal;
    ID3D12Resource *depth_resource{}, *normal_resource{};
    std::span<const VirtualShadowCaster> casters; // All world candidates; complete resident LOD/proxy.
    std::span<const VirtualShadowReceiver> receivers; // Visible transparent bounds + volume ranges.
    std::span<const VirtualShadowDrawGroup> draw_groups;
    std::span<const VirtualShadowNativeRead> native_reads; // Canonical geometry/instances/SkinCache/material.
    // Called once per bank after GPU indirect tile clear. Rebind the main root,
    // table32 suffix and native resources; preserve the supplied full-bank DSV,
    // viewport/scissor. Execute each batch using main-root signatures:
    // root6 DWORD0 + DRAW_INDEXED (24B) / DISPATCH_MESH (16B), count at count_offset.
    std::function<Result(RenderPassContext &, const VirtualShadowOutputs &,
        std::span<const VirtualShadowDrawBatch>)> draw;
    std::function<void(PassBuilder &)> declare_material_sampling;
};
struct VirtualShadowTargets;
class VirtualShadowCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept; // GPU allocations only.
    [[nodiscard]] std::uint64_t HostStagingBytes() const noexcept;
  private:
    std::shared_ptr<VirtualShadowTargets> targets_;
    friend class VirtualShadowSystem;
};
// Passes147..155,224..225. Off prepares no resources/PSOs. Missing/unpublished/overflow
// pages return false from HsVirtualShadowLookup; caller samples compact cascades.
// Caller declares all five output reads in every lighting/transparent/world-query
// consumer. Partial submission failure faults the cache and does not commit epoch.
class VirtualShadowSystem
{
  public:
    using ShaderReader=std::function<Result(std::string_view,std::vector<std::byte> &)>;
    VirtualShadowSystem();
    ~VirtualShadowSystem();
    VirtualShadowSystem(const VirtualShadowSystem &)=delete;
    VirtualShadowSystem &operator=(const VirtualShadowSystem &)=delete;
    [[nodiscard]] Result Prepare(ID3D12Device *,D3D12MA::Allocator *,const VirtualShadowProfile &,
        const ShaderReader &,VirtualShadowCandidate &,RendererPipelineCache *cache=nullptr) const;
    [[nodiscard]] Result Commit(VirtualShadowCandidate &&,const CompletionSet &completed);
    [[nodiscard]] Result Register(RenderGraphBuilder &,const VirtualShadowInputs &,VirtualShadowOutputs &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &,const CompletionSet &,bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &);
    [[nodiscard]] VirtualShadowCacheStatus CacheStatus() const noexcept;
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    [[nodiscard]] std::uint64_t HostStagingBytes() const noexcept;
    [[nodiscard]] std::array<const AllocationResource *, 5> DiagnosticResources() const noexcept;
    // Called only after all old submissions have reliably completed.
    void SwapState(VirtualShadowSystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
