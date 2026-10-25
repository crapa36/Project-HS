#pragma once

#include "renderer_cluster_catalog.hpp"
#include <hs/render_data/material_schema.hpp>
#include "d3d12_resources.hpp"
#include "residency_manager.hpp"
#include "streaming_manager.hpp"

#include <array>
#include <future>
#include <functional>
#include <filesystem>
#include <memory>
#include <span>

namespace hs
{
namespace renderer_detail { struct GeometryPageGpu; }
inline constexpr std::uint32_t kClusterRuntimeFrames = 3;
inline constexpr std::uint32_t kClusterRuntimeCatalogs = static_cast<std::uint32_t>(render_data::kEnvironmentMaterialNames.size());
inline constexpr std::uint32_t kClusterCutSrvCount = 10;
inline constexpr std::uint32_t kClusterCutUavCount = 18;
inline constexpr std::uint32_t kClusterCutDescriptorCount = 29;
inline constexpr std::uint32_t kClusterCutClearOffset = kClusterCutDescriptorCount;
inline constexpr std::uint32_t kClusterCutBankStride = kClusterCutClearOffset + kClusterCutUavCount;
inline constexpr std::uint32_t kClusterOcclusionPhases = 2;
inline constexpr std::uint32_t kClusterConstantStride = 768; // 576 bytes, CBV aligned
inline constexpr std::uint32_t kClusterOcclusionConstantStride = 256;

// b1/space6. Both matrices retain FrameConstants' column-major upload layout
// and include the corresponding world-space camera origin and raster jitter.
struct alignas(16) GpuClusterOcclusionConstants
{
    std::array<float, 16> current_view_projection{}, previous_view_projection{};
    std::array<float, 4> extent_near_far{}; // HZB width/height, physical near/far
    std::uint32_t level_count{}, element_count{}, history_valid{}, phase{};
};
static_assert(sizeof(GpuClusterOcclusionConstants) == 160);

struct ClusterPageStream
{
    ClusterPageStream();
    ~ClusterPageStream();
    ClusterPageStream(ClusterPageStream &&) noexcept;
    ClusterPageStream &operator=(ClusterPageStream &&) noexcept;
    ClusterPageStream(const ClusterPageStream &) = delete;
    ClusterPageStream &operator=(const ClusterPageStream &) = delete;
    std::uint32_t catalog{}, local_page{}, logical_slot{}, priority{};
    // Both mesh hierarchy and scene HLOD pages share this directory, pool,
    // request state machine and fence publication. The catalog after all environment geometries is scene HLOD.
    std::string source_id, logical_id;
    std::filesystem::path path;
    std::function<Result(std::span<const std::byte>, render_data::GeometryData &)> decode;
    std::uint32_t physical_slot{UINT32_MAX}, version{};
    bool pinned{}, upload_pending{}, upload_submitted{}, retiring{};
    // Optional scene HLOD groups retain every admitted page until the next
    // completed GPU demand snapshot withdraws the complete group.
    bool hlod_admitted{};
    std::uint64_t last_demand_epoch{};
    std::uint64_t read_bytes{};
    StreamingRequestId request;
    std::future<std::vector<std::byte>> read_future;
    std::vector<std::byte> read_storage;
    render_data::GeometryData decoded;
    ResidencyTicket decoded_residency, read_residency, upload_residency;
    std::unique_ptr<renderer_detail::GeometryPageGpu> page;
    AllocationResource upload;
    CompletionSet upload_completion{}, last_use{}, retirement{};
};
struct ClusterCatalogRange
{
    std::uint32_t group_base{}, group_count{}, child_base{}, child_count{}, first_root{};
    std::uint32_t cluster_base{}, cluster_count{}, page_base{}, page_count{};
    std::uint32_t worst_cut_records{};
    bool static_error{}, roots_ready{};
};
struct ClusterCutBuffers
{
    AllocationResource candidates, constants, occlusion_constants;
    AllocationResource counts, offsets, block_sums, block_offsets, demand, status;
    AllocationResource draw_commands, mesh_commands, command_counts;
    AllocationResource history_bits, history_generations;
    AllocationResource selected_bounds, previous_rejected, retest_draw_commands, retest_mesh_commands;
    AllocationResource readback;
    ResidencyTicket residency, staging_residency;
    CompletionSet completion{}, readback_completion{};
    std::uint32_t candidate_capacity{}, output_capacity{}, candidate_count{};
    std::uint32_t allocated_output_capacity{}; // Physical scratch capacity; output_capacity is this frame's logical range.
    std::uint32_t geometry_draw_base{}, generation{}, required_capacity{};
    std::uint32_t geometry_frontend_flags{};
    std::uint32_t history_instance_capacity{}, history_words_per_instance{};
    std::uint32_t previous_history_slot{UINT32_MAX};
    std::uint64_t actual_frame_id{};
    bool readback_pending{};
    // Cut growth and view-bank replacement reset this together with the owned resources.
    bool descriptors_initialized{};
    GpuClusterCutStatus last_status{};
};
struct ClusterFrameRuntime
{
    AllocationResource pages, page_upload;
    AllocationResource refs;
    ResidencyTicket refs_residency;
    std::uint32_t ref_capacity{};
    ResidencyTicket pages_residency, page_upload_residency;
    std::vector<GpuClusterPage> snapshot;
    std::array<ClusterCutBuffers, kClusterRuntimeCatalogs> cuts;
    CompletionSet completion{};
    std::uint64_t epoch{};
    std::uint64_t history_frame_id{};
    std::uint64_t history_generation{};
    // Previous raster dimensions and physical depth range belong to this view,
    // not the shared FrameConstants upload scratch.
    std::array<float, 4> history_extent_depth{};
    bool page_upload_pending{};
};
// Root supplies persistent-slot candidates, including the actual instance
// generation. These are candidates, never a CPU visible-list or selected cut.
struct ClusterCutInput
{
    std::span<const GpuClusterCandidate> candidates;
    GpuClusterRefinementConstants constants;
    std::uint32_t max_instance_count{};
};
struct ClusterViewRuntime
{
    std::array<ClusterFrameRuntime, kClusterRuntimeFrames> frames;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors;
    // UAV clear CPU handles must come from a non shader visible heap. The
    // offsets match descriptors so each waited frame/catalog/phase bank agrees.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> clear_descriptors;
};
// The active view bank follows OwnedView exchange. Catalogs, physical page
// ownership and streaming remain common to all views.
struct ClusterRuntimeState : ClusterViewRuntime
{
    AllocationResource groups, children, clusters, history_zero;
    std::array<ResidencyTicket, 4> tables_residency;
    ClusterGpuTables cpu;
    std::array<ClusterCatalogRange, kClusterRuntimeCatalogs> catalogs;
    std::vector<ClusterPageStream> streams;
    // Geometry records are shared execution scratch, so their growth tickets
    // stay with the shared owner rather than exchanging with view histories.
    std::array<ResidencyTicket, kClusterRuntimeFrames> geometry_records_residency;
    std::array<std::uint32_t, render_data::kGeometryPageCapacity> physical_owner{}, physical_versions{};
    std::uint32_t generation{}, first_physical_slot{};
    std::uint64_t epoch{}, requests{}, uploads{}, publications{}, evictions{};
    std::string content_generation;
    std::vector<AllocationResource> initial_uploads;
    std::vector<ResidencyTicket> initial_upload_residency;
};

// The root-owned graph imports pages as CopyDest when page_upload_pending,
// copies page_upload -> pages, then reads pages/groups/children/clusters as
// ComputeRead. Candidates/constants are immutable GENERIC_READ uploads.
// Per-catalog scratch is cleared, Count -> Prefix0/1/2 -> Emit uses UAV barriers;
// records and refs transition to raster/resolve reads, arguments/counts to
// IndirectArgs. Readback copies demand then48B status at page_count*4 bytes.
// SubmitClusterCutReadback carries the graph's COPY completion, and
// MarkClusterFrameUse carries ALL queue uses of snapshots/records/cut refs.
// A frame may be rewritten/grown only after its previous completion finishes.
// Each of two phase shader tables contains 10 SRVs followed by 18 UAVs and one
// HLOD leaf export SRV for root4; each bank appends 18 raw clear UAVs outside
// the shader ABI. t9 is that phase's HZB. t5/t6/t7/t8 are previous
// membership/generations/pages/status; u12/u13 are current membership/generations.
// History is keyed by persistent instance slot and catalog-local cluster ID,
// qualified within its owning view by immediate successful frame and view,
// content/instance/page generations. Camera/session/scene invalidation applies
// to membership as well as previous HZB tests, independently of TAA enablement.
// Graph imports the previous resources with their completion, and commit extends
// that frame slot's lifetime through its new GPU readers before reuse/growth.
// Root restores srv_heap
// before drawing, binds frame.refs at t29 space3, and binds the shared records via
// existing GeometryBuffers[0]. View arguments are contiguous OutputCapacity
// records per view:24B DRAW and16B DISPATCH_MESH. Count buffer offsets=view*4.
// CompactDeferred sets RasterRecordBase=UINT32_MAX: u11 binds a valid32B raw
// status buffer, and no raster record is read or written by the cut kernel.
// u14 stores each selected cluster's conservative world sphere; u15 marks
// previous-HZB rejects. u16/u17 contain sparse camera-only retest commands.
// Main/shadow counts are at bytes0/4/8/12; the retest count is at byte16.
} // namespace hs
