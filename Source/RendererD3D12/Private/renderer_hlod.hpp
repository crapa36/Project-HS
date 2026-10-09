#pragma once

#include "render_graph.hpp"
#include "renderer_geometry.hpp"
#include <hs/render_data/scene_hlod.hpp>

#include <DirectXMath.h>
#include <d3d12.h>

#include <array>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hs
{
inline constexpr std::uint32_t kHlodProxyResident = 1u;
inline constexpr std::uint32_t kHlodRepresentationChanged = 1u;
inline constexpr std::uint32_t kHlodViewCount = 4;

// Parent supplies immutable, fence-published snapshots from the existing page
// stream and the exact prebuilt native GeometryDrawRecord/ref. All fields are
// uint32; this 48B layout is also the t3/space25 selection input ABI.
struct GpuHlodProxyBinding
{
    std::uint32_t logical_page{}, physical_page_slot{}, page_version{}, content_generation{};
    std::uint32_t geometry_record_id{}, command_ordinal{}, bucket_index{}, flags{};
    std::uint32_t instance_slot{}, instance_generation{}, visible_ref_index{}, reserved{};
};
static_assert(sizeof(GpuHlodProxyBinding) == 48);

struct HlodMemberBinding
{
    std::uint64_t stable_id{};
    std::uint32_t instance_slot{}, instance_generation{};
};

// Buckets are a complete permutation of proxy command slots, partitioned by
// the parent's material/native geometry binding. Commands stay in those fixed
// slots, with zero draw/dispatch dimensions for unselected ranges. No visible
// list or cut is read back. ExecuteIndirect uses command_count and the GPU
// count at (view * bucket_count + bucket_index) * 4.
struct GpuHlodBucket
{
    std::uint32_t first_command{}, command_count{}, material_word{}, reserved{};
};
static_assert(sizeof(GpuHlodBucket) == 16);

// Main root export: t0/space25, 16B StructuredBuffer. A consumer suppresses a
// leaf only when generation matches its own static ref. An absent/mismatched
// entry means complete leaf fallback. Bit0=camera; bits1..3=conventional
// cascades. World-query shadow/RT consumers retain their independent demand.
struct GpuHlodLeafMask
{
    std::uint32_t generation{}, view_mask{15}, history_flags{}, reserved{};
};
static_assert(sizeof(GpuHlodLeafMask) == 16);
// Optional t1/space25 export, indexed by logical proxy index. Change flags are
// view bits; they lower history confidence when proxy/page identity changes.
struct GpuHlodProxySelection
{
    std::uint32_t view_mask{}, change_mask{}, page_version{}, reserved{};
};
static_assert(sizeof(GpuHlodProxySelection) == 16);

struct alignas(16) GpuHlodView
{
    DirectX::XMFLOAT4 origin_near{};
    DirectX::XMFLOAT4 forward{};
    std::array<DirectX::XMFLOAT4, 6> frustum_planes{};
    // x=output-pixel projection scale; y=maximum output-pixel error;
    // z=hysteresis fraction [0,1); w=0 perspective or1 orthographic.
    DirectX::XMFLOAT4 error_policy{};
};
static_assert(sizeof(GpuHlodView) == 144);

struct HlodExternalBuffer
{
    ID3D12Resource *resource{};
    ResourceVersion version;
};

struct HlodFrameInputs
{
    std::uint32_t frame_slot{}, active_view_mask{1};
    std::uint64_t rendered_frame_id{}, static_scene_generation{}, history_generation{};
    bool history_valid{true};
    CompletionSet completed;
    std::string_view content_generation_id;
    std::uint32_t layout_abi{};
    std::array<std::byte, 32> layout_digest{};
    std::span<const HlodMemberBinding> members; // SceneHlod.members order.
    std::span<const GpuHlodProxyBinding> proxies; // SceneHlod.proxies order.
    std::span<const GpuHlodBucket> buckets;
    std::array<GpuHlodView, kHlodViewCount> views;
    // Existing root-owned tables, already imported and initialized in graph.
    HlodExternalBuffer static_scene, geometry_records, proxy_refs;
};

struct HlodFrameOutputs
{
    ResourceVersion leaf_masks, proxy_selection, indexed_commands, mesh_commands;
    ResourceVersion command_counts, page_demand, status;
    std::array<ID3D12Resource *, 7> resources{}; // Same order as versions above.
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> srvs{}; // Leaf/proxy exports.
    std::uint32_t static_slot_capacity{}, proxy_command_capacity{}, bucket_count{}, page_count{};
    // Per-view byte offsets: view*proxy_command_capacity*24 or *16. PassValue
    // is exactly (existing_geometry_record_id << 2) | view, root6 unchanged.
    // Demand is uint per logical SHL1 page (0=no demand, float bits=priority).
    // Status is64B, telemetry only: fault bits at0, selected proxies at4,
    // selected groups at8, history changes at12; identity/frame at32..47.
};

struct HlodStorage;
class HlodCandidate
{
public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
private:
    std::shared_ptr<HlodStorage> storage_;
    friend class HlodSystem;
};

// Owns only metadata, selection scratch/commands/history and their descriptors.
// Parent owns every geometry page/allocation, native record, material binding,
// residency ticket and asynchronous demand/status telemetry copy. Consumers
// declare graph reads and restore their normal descriptor heap before drawing.
class HlodSystem
{
public:
    using ShaderReader = std::function<Result(std::string_view, std::vector<std::byte> &)>;
    HlodSystem();
    ~HlodSystem();
    HlodSystem(const HlodSystem &) = delete;
    HlodSystem &operator=(const HlodSystem &) = delete;
    [[nodiscard]] Result Prepare(ID3D12Device *, D3D12MA::Allocator *,
        const render_data::SceneHlod &, std::string_view content_generation_id,
        std::uint32_t content_generation, std::uint32_t static_slot_capacity,
        std::uint32_t frame_slot_count, const ShaderReader &, HlodCandidate &) const;
    [[nodiscard]] Result Commit(HlodCandidate &&, const CompletionSet &completed);
    [[nodiscard]] Result Register(RenderGraphBuilder &, const HlodFrameInputs &, HlodFrameOutputs &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &, const CompletionSet &, bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &completed);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    // History readers extend a prior descriptor/snapshot bank's lease. Parent
    // merges this set into its existing frame-slot wait before Register.
    [[nodiscard]] CompletionSet FrameCompletion(std::uint32_t frame_slot) const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
