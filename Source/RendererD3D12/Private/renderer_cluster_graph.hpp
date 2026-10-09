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
    std::array<ClusterGraphBuffer, 4> buffers;
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
    ClusterGraphBuffer groups, children, clusters, pages, refs;
    std::vector<ClusterGraphPage> geometry;
    std::array<ClusterGraphCut, kClusterRuntimeCatalogs> cuts;
    std::vector<ClusterGraphBuffer> imports;
    ResourceVersion geometry_records{}, raster_records{};
    bool prepared{}, draws{}, visibility{}, previous_occlusion{}, current_retest{};
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
