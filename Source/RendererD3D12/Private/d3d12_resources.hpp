#pragma once

#include "render_graph.hpp"
#include "renderer_geometry.hpp"
#include <hs/renderer/renderer.hpp>

#include <D3D12MemAlloc.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace hs
{

constexpr std::uint32_t kFrameUploadSize = 576 * 1024;

struct AllocationResource
{
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12MA::Allocation *allocation{};
    Access graph_access{Access::Undefined};
    QueueHint graph_queue{QueueHint::Direct};
    CompletionSet graph_completion{};

    AllocationResource() = default;
    ~AllocationResource() { Reset(); }
    AllocationResource(const AllocationResource &) = delete;
    AllocationResource &operator=(const AllocationResource &) = delete;
    AllocationResource(AllocationResource &&other) noexcept
        : resource(std::move(other.resource)),
          allocation(std::exchange(other.allocation, nullptr)),
          graph_access(std::exchange(other.graph_access, Access::Undefined)),
          graph_queue(other.graph_queue), graph_completion(other.graph_completion)
    {
    }
    AllocationResource &operator=(AllocationResource &&other) noexcept
    {
        if (this != &other)
        {
            Reset();
            resource = std::move(other.resource);
            allocation = std::exchange(other.allocation, nullptr);
            graph_access = std::exchange(other.graph_access, Access::Undefined);
            graph_queue = other.graph_queue;
            graph_completion = other.graph_completion;
        }
        return *this;
    }

    void Reset() noexcept
    {
        graph_access = Access::Undefined;
        graph_queue = QueueHint::Direct;
        graph_completion = {};
        resource.Reset();
        if (allocation)
        {
            allocation->Release();
            allocation = nullptr;
        }
    }
};

struct FrameContext
{
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    AllocationResource upload;
    AllocationResource ui_upload;
    AllocationResource static_visible_refs;
    AllocationResource static_draw_args;
    AllocationResource static_rejected_refs;
    AllocationResource static_rejected_args;
    AllocationResource static_retest_refs;
    AllocationResource static_retest_args;
    AllocationResource dynamic_visible_refs;
    AllocationResource dynamic_draw_args;
    AllocationResource geometry_draw_records;
    AllocationResource geometry_draw_records_upload;
    std::vector<renderer_detail::GpuGeometryDrawRecord> geometry_draw_records_cpu;
    std::uint64_t static_input_ref_offset{}, dynamic_input_ref_offset{};
    std::uint32_t static_input_ref_count{}, dynamic_input_ref_count{};
    AllocationResource skin_cache_vertices;
    AllocationResource skin_cache_bounds;
    std::size_t static_visible_capacity{};
    std::size_t static_rejected_capacity{};
    std::size_t static_retest_capacity{};
    std::size_t dynamic_visible_capacity{};
    std::size_t skin_cache_vertex_capacity{};
    std::size_t skin_cache_bound_capacity{};
    std::size_t skin_cache_slot_stride{};
    bool skin_cache_vertex_read{};
    bool skin_cache_bounds_read{};
    bool skin_cache_valid{};
    std::byte *mapped{};
    std::size_t upload_size{kFrameUploadSize};
    std::byte *ui_mapped{};
    CompletionSet completion{};
    CompletionSet skin_cache_use_completion{};
    bool timestamps_recorded{};
    std::uint64_t submitted_frame_id{};
    std::uint32_t submitted_view_id{};
    std::uint64_t cpu_qpc_frequency{};
    mutable std::uint64_t timing_stream_published_frame_id{};
    float cpu_recording_ms{};
    bool cpu_recording_ms_valid{};
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> timestamp_heap;
    AllocationResource timestamp_readback;
    std::uint64_t *mapped_timestamps{};
    std::uint32_t timestamp_capacity{};
    std::vector<GpuPassTiming> timestamp_passes;
    std::vector<std::uint64_t> timestamp_frequencies;
    bool ui_initialized{};
};

} // namespace hs
