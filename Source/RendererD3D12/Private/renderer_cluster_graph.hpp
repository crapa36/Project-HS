#pragma once

#include "renderer_cluster_runtime.hpp"

namespace hs
{
struct ClusterGraphBuffer
{
    AllocationResource *native{};
    BufferHandle handle{};
    ResourceVersion version{};
};
struct ClusterGraphPage
{
    std::uint32_t logical_slot{};
    ClusterGraphBuffer buffer{};
    bool resident{}, upload{};
};
struct ClusterGraphCut
{
    bool active{};
    ClusterGraphBuffer counts, offsets, block_sums, block_offsets;
    ClusterGraphBuffer demand, status, draw_commands, mesh_commands, command_counts, readback;
    ClusterGraphBuffer history_bits, history_generations;
    ClusterGraphBuffer selected_bounds, previous_rejected, retest_draw_commands, retest_mesh_commands;
    ClusterGraphBuffer previous_bits, previous_generations, previous_pages, previous_status;
    std::uint32_t previous_frame_slot{UINT32_MAX};
};
// Frame-local graph state. Retained AllocationResource objects belong to Impl's
// immutable catalogs/page streams/frame slot. Recording joins before this state
// leaves scope; GPU lifetime is carried by CommitClusterFrameGraph completions.
struct ClusterFrameGraph
{
    std::uint32_t frame_slot{};
    ClusterGraphBuffer groups, children, clusters, representation_pages, pages, refs;
    std::vector<ClusterGraphPage> geometry;
    std::array<ClusterGraphCut, kClusterRuntimeCatalogs> cuts;
    std::vector<ClusterGraphBuffer> imports;
    ResourceVersion geometry_records{}, raster_records{};
    bool prepared{}, draws{}, visibility{}, previous_occlusion{}, current_retest{};
};
struct ClusterGraphPageRecord
{
    std::uint32_t logical_slot{}, physical_slot{}, page_version{};
    bool resident{}, upload{};
    bool operator==(const ClusterGraphPageRecord &) const noexcept = default;
};
// Prepared once before graph restore. All fields affect the declaration
// topology or the native range/capacity contract of a cluster pass.
struct ClusterFrameRecordData
{
    static_assert(kClusterRuntimeCatalogs <= 64);
    std::uint32_t frame_slot{}, runtime_generation{};
    std::uint64_t active_mask{};
    std::array<std::uint32_t,kClusterRuntimeCatalogs> previous_slots{},
        output_capacities{}, frontend_flags{};
    std::vector<ClusterGraphPageRecord> pages;
    bool visibility{}, page_upload{}, hlod_leaf{};
    bool operator==(const ClusterFrameRecordData &) const noexcept = default;
};
struct ClusterGraphLayout
{
    ClusterFrameRecordData record;
    std::vector<BufferHandle> imports;
    std::vector<ResourceVersion> versions;
    std::uint32_t upload_pass{UINT32_MAX}, count_pass{UINT32_MAX},
        emit_pass{UINT32_MAX}, readback_pass{UINT32_MAX},
        previous_occlusion_pass{UINT32_MAX}, retest_pass{UINT32_MAX};
    std::array<std::uint32_t,3> prefix_pass{UINT32_MAX,UINT32_MAX,UINT32_MAX};
};
// IDs39..44: cluster upload, count, prefix0/1/2, emit. ID50: demand/status readback.
// Add after the existing authored visibility pass and before Shadow. Pass reads
// of geometry_records/raster_records use the returned latest versions. Draw
// callers first bind the normal graphics root signature, texture/sampler/frame
// descriptors and render targets. ReadClusterFrame(...,false) is for visibility
// resolve: refs and every resident page are read without consuming arguments.
// Commit after successful Execute, before another frame can pump/retire pages.
// ID52 suppresses only camera commands using previous HZB; ID53 retests those
// rejects with current HZB after the first depth pyramid and before retest raster.
// ReadClusterFrame(...,true,true) and DrawClusterGeometry(...,0,true) consume
// the independent retest arrays. Both phases leave the complete cut intact.
} // namespace hs
