#pragma once

#include "render_graph.hpp"
#include "view_state.hpp"
#include "renderer_shadow_guarantee.hpp"
#include <hs/render_data/render_snapshot_values.hpp>
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
struct RendererConfig;
struct VirtualShadowBakeData;
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
inline constexpr std::uint32_t kVirtualShadowDescriptorCount = 7;

struct VirtualShadowCacheStatus
{
    std::uint64_t requested_bytes{}, admitted_bytes{};
    std::uint32_t physical_pages{};
    bool baked_active{};
    std::uint32_t baked_pages{};
    std::uint64_t baked_staging_bytes{};
    bool cpu_plan_prepared{};
    VirtualShadowGuarantee guarantee;
};
enum class VirtualShadowFrontend : std::uint32_t { Indexed, Mesh };
struct VirtualShadowProfile
{
    bool enabled{};
    std::shared_ptr<const VirtualShadowBakeData> baked;
    std::uint64_t cache_budget_bytes{}; // Authored capacity for both native guarded depth banks.
    std::uint64_t gpu_budget_bytes{}; // Entire service, including all frame buffers.
    std::uint32_t physical_pages{}, max_casters{}, max_receivers{}, max_draw_groups{};
    std::uint32_t filter_samples{};
    std::uint32_t command_capacity{}, frame_slots{3};
    std::array<std::uint32_t,3> virtual_sides{32,8,8}; // Pages per clipmap side.
    ViewExtent render_capacity;
    float fixed_root_depth_span{}; // Fixed world-light Z interval, derived from configured shadow distance.
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
    bool skin{}, wind{}, animated_alpha{}, baked_source{};
    bool previous_skin{}, previous_wind{}, previous_animated_alpha{};
};
// The immutable source prefix is retained between frames; only the dynamic suffix
// is rebuilt. Iteration preserves one logical caster sequence without copying it.
struct ShadowCasterRange
{
    std::span<const VirtualShadowCaster> stable, dynamic;
    ShadowCasterRange()=default;
    ShadowCasterRange(std::span<const VirtualShadowCaster> a,std::span<const VirtualShadowCaster> b):stable(a),dynamic(b) {}
    ShadowCasterRange &operator=(std::span<const VirtualShadowCaster> value) { stable={}; dynamic=value; return *this; }
    [[nodiscard]] std::size_t size() const noexcept { return stable.size()+dynamic.size(); }
    [[nodiscard]] const VirtualShadowCaster &operator[](std::size_t i) const noexcept
    { return i<stable.size() ? stable[i] : dynamic[i-stable.size()]; }
    struct Iterator
    {
        const ShadowCasterRange *range{}; std::size_t index{};
        const VirtualShadowCaster &operator*() const noexcept { return (*range)[index]; }
        Iterator &operator++() noexcept { ++index; return *this; }
        bool operator!=(const Iterator &other) const noexcept { return index!=other.index; }
    };
    [[nodiscard]] Iterator begin() const noexcept { return {this,0}; }
    [[nodiscard]] Iterator end() const noexcept { return {this,size()}; }
};
struct VirtualShadowReceiver
{
    // World AABB. minimum.w: 0 requests pages and extends depth; 1 extends depth only.
    // maximum.w: 0 world-query enclosure; 1 camera-only GPU feedback envelope
    // whose CPU coverage proof is the independent actual-draw surface range.
    // The GPU ignores this discriminator. Keep the 32-byte GPU payload.
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
// Compute b0/space21 and raw t0/space21 share the payload. Preserve the original
// 368-byte VSM prefix; fixed-root metadata is append-only within the 512-byte slot.
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
    DirectX::XMUINT4 options{}; // Reset cache, enabled, total fixed command capacity, filter samples.
    DirectX::XMFLOAT4 baked_depth{0,0,0,1}; // Fixed archive Z min/max, enabled, actual CSM available.
    DirectX::XMINT4 baked_directory{}; // Coarse absolute minimum XY and page dimensions.
    DirectX::XMFLOAT4 root_bounds{}; // Fixed light-space XY min/max of the flattened root.
    DirectX::XMFLOAT4 root_extent{}; // Root texel width/height, world texel, filter guard.
    DirectX::XMFLOAT4 root_depth{}; // Fixed root Z minimum, inverse span, span, reserved.
    DirectX::XMUINT4 root_options{}; // Root resource present, ready, compatible, current-root counter base (zero disables).
};
static_assert(sizeof(VirtualShadowConstants)==432);
static_assert(offsetof(VirtualShadowConstants,clipmaps)==160);
static_assert(offsetof(VirtualShadowConstants,windows)==208);
static_assert(offsetof(VirtualShadowConstants,counts)==256);
static_assert(offsetof(VirtualShadowConstants,baked_depth)==336);
static_assert(offsetof(VirtualShadowConstants,baked_directory)==352);
static_assert(offsetof(VirtualShadowConstants,root_bounds)==368);
static_assert(offsetof(VirtualShadowConstants,root_extent)==384);
static_assert(offsetof(VirtualShadowConstants,root_depth)==400);
static_assert(offsetof(VirtualShadowConstants,root_options)==416);

struct VirtualShadowNativeRead { ResourceVersion version; Access access{Access::ShaderRead}; };
struct VirtualShadowDrawBatch
{
    VirtualShadowDrawGroup group;
    std::uint32_t group_index{}, layer{}, maximum_commands{};
    std::uint64_t argument_offset{}, count_offset{};
};
struct VirtualShadowOutputs
{
    ResourceVersion metadata, page_table, static_bank, dynamic_bank, draw_records, root_static, root_dynamic;
    ResourceVersion indexed_commands, mesh_commands, command_counts;
    std::array<ID3D12Resource *,7> resources{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE,7> srvs{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE,3> depth_views{};
    ViewExtent root_extent;
    ID3D12Resource *indexed_arguments{}, *mesh_arguments{}, *counts_resource{};
    ViewExtent bank_extent;
    std::uint32_t descriptor_count{kVirtualShadowDescriptorCount};
};
struct VirtualShadowInputs
{
    bool baked_static{};
    // Camera is always required; each actual world-query feature adds its own
    // required bit. Closed bits mean a complete inventory, including verified
    // empty domains. Private captures retain their own CSM and depth-only bounds.
    std::uint32_t required_receiver_domains{},closed_receiver_domains{};
    DirectX::XMFLOAT4X4 receiver_view_projection{}; // Exact current scene-to-raster matrix, row-major.
    ShadowSurfaceReceiverRange surface_receivers; // CPU-only, independent of caster eligibility.
    ViewId view_id;
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
    ShadowCasterRange casters; // All world candidates; complete resident LOD/proxy.
    std::uint64_t stable_caster_revision{}; // Nonzero, monotonically changed after submission settles.
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
    // CPU-only index into receivers for the camera-frustum enclosing World AABB.
    // UINT32_MAX means no such receiver; this field is not uploaded to the GPU.
    std::uint32_t frustum_receiver_index=UINT32_MAX;
    // CPU-only copies of the main camera's three orthographic CSM matrices.
    // These are used only to prove where legacy CSM could affect a receiver.
    std::array<ShadowViewProjection,3> main_csm_view_projection{};
    bool main_csm_view_projection_valid{};
    // Borrowed for this CPU preparation only; the fixed-root proof never calls it.
    std::function<bool(std::array<ShadowViewProjection,3> &)> prepare_main_csm;
};
struct VirtualShadowTargets;
// Pure CPU proof. No native/GPU counter or last-frame dynamic data is consulted.
[[nodiscard]] VirtualShadowGuarantee CheckVirtualShadowGuarantee(
    const VirtualShadowInputs &, const VirtualShadowConstants &,
    std::uint32_t physical_pages, bool seed_available,std::span<ShadowCommandSegment> command_segments,
    std::uint32_t command_capacity,bool fixed_root_eligible,const ShadowCommandDemand &commands);
struct VirtualShadowFramePlan
{
    VirtualShadowConstants constants;
    VirtualShadowGuarantee guarantee;
  private:
    std::shared_ptr<VirtualShadowTargets> targets;
    ShadowCasterRange casters;
    std::span<const VirtualShadowReceiver> receivers;
    std::span<const VirtualShadowDrawGroup> groups;
    LightView light;
    ViewId view_id;
    DirectX::XMFLOAT3 clipmap_center{};
    std::array<float,3> coverage_extents{};
    float depth_span{};
    std::uint64_t rendered_frame_id{}, stable_caster_revision{};
    std::uint32_t frame_slot{};
    std::uint32_t required_receiver_domains{},closed_receiver_domains{};
    DirectX::XMFLOAT4X4 receiver_view_projection{};
    ShadowSurfaceReceiverRange surface_receivers;
    std::vector<ShadowCommandSegment> command_segments;
    bool baked_static{}, fixed_root{}, prepared{};
    friend class VirtualShadowSystem;
};
struct VirtualShadowLayout
{
    struct Pass { std::uint32_t declaration_index{}, id{}; };
    std::uint64_t target_generation{};
    std::array<ResourceHandle,17> imports{};
    std::array<ResourceVersion,10> outputs{};
    std::array<Pass,16> passes{};
    std::uint32_t pass_count{};
    ResourceVersion depth{}, normal{};
    std::vector<VirtualShadowNativeRead> native_reads;
    bool fixed_root{}, baked_present{}, seed_upload{}, root_static_present{};
    bool root_static_upload{}, root_dynamic_present{}, reset{}, material_sampling{};
};
class VirtualShadowCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept; // GPU allocations only.
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
    [[nodiscard]] static Result LoadBake(const RendererConfig &, VirtualShadowProfile &,
        std::shared_ptr<const VirtualShadowBakeData> &shared_source);
    [[nodiscard]] bool MatchesBake(const std::shared_ptr<const StaticRenderScene> &, const LightView &,
        const std::array<float,3> &coverage_extents);
    [[nodiscard]] Result Commit(VirtualShadowCandidate &&,const CompletionSet &completed);
    // Per-frame CPU diagnostics only; committed native cache state is untouched.
    void BeginFramePolicy() noexcept;
    // CPU views stay unchanged until Register; only graph/native resource versions
    // may be supplied later. Register consumes this exact view/frame/slot plan.
    [[nodiscard]] Result PrepareFrame(const VirtualShadowInputs &, VirtualShadowFramePlan &);
    [[nodiscard]] Result Register(RenderGraphBuilder &,const VirtualShadowInputs &,
        const VirtualShadowFramePlan &,VirtualShadowOutputs &,VirtualShadowLayout *layout=nullptr);
    [[nodiscard]] Result BindRetained(RenderGraphBuilder &,const VirtualShadowInputs &,
        const VirtualShadowFramePlan &,const VirtualShadowLayout &,VirtualShadowOutputs &);
    [[nodiscard]] bool MatchesLayout(const VirtualShadowInputs &,
        const VirtualShadowFramePlan &,const VirtualShadowLayout &) const noexcept;
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &,const CompletionSet &,bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &);
    [[nodiscard]] VirtualShadowCacheStatus CacheStatus() const noexcept;
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    [[nodiscard]] std::array<const AllocationResource *, 5> DiagnosticResources() const noexcept;
    // Called only after all old submissions have reliably completed.
    void SwapState(VirtualShadowSystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    [[nodiscard]] Result RegisterImpl(RenderGraphBuilder &,const VirtualShadowInputs &,
        const VirtualShadowFramePlan &,VirtualShadowOutputs &,VirtualShadowLayout *,
        const VirtualShadowLayout *);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
