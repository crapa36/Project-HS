#pragma once

#include "render_graph.hpp"
#include "renderer_geometry.hpp"
#include "persistent_gpu_scene.hpp"
#include <hs/renderer/renderer.hpp>

#include <D3D12MemAlloc.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
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
    NativeResourceContract native_contract{};
    ID3D12Resource *contract_resource{};
    D3D12MA::Allocation *contract_allocation{};
    ID3D12Device *contract_device_input{};

    AllocationResource() = default;
    ~AllocationResource() { Reset(); }
    AllocationResource(const AllocationResource &) = delete;
    AllocationResource &operator=(const AllocationResource &) = delete;
    AllocationResource(AllocationResource &&other) noexcept
        : resource(std::move(other.resource)),
          allocation(std::exchange(other.allocation, nullptr)),
          graph_access(std::exchange(other.graph_access, Access::Undefined)),
          graph_queue(other.graph_queue), graph_completion(other.graph_completion),
          native_contract(other.native_contract),
          contract_resource(std::exchange(other.contract_resource, nullptr)),
          contract_allocation(std::exchange(other.contract_allocation, nullptr)),
          contract_device_input(std::exchange(other.contract_device_input, nullptr))
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
            native_contract = other.native_contract;
            contract_resource = std::exchange(other.contract_resource, nullptr);
            contract_allocation = std::exchange(other.contract_allocation, nullptr);
            contract_device_input = std::exchange(other.contract_device_input, nullptr);
        }
        return *this;
    }

    void Reset() noexcept
    {
        graph_access = Access::Undefined;
        graph_queue = QueueHint::Direct;
        graph_completion = {};
        native_contract = {};
        contract_resource = nullptr;
        contract_allocation = nullptr;
        contract_device_input = nullptr;
        resource.Reset();
        if (allocation)
        {
            allocation->Release();
            allocation = nullptr;
        }
    }

    [[nodiscard]] Result EnsureNativeContract(ID3D12Device *device);
    [[nodiscard]] const NativeResourceContract *NativeContract() const noexcept
    {
        return resource && contract_resource == resource.Get() && contract_allocation == allocation
            ? &native_contract : nullptr;
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
    AllocationResource ribbon_arguments;
    AllocationResource geometry_draw_records;
    AllocationResource geometry_draw_records_upload;
    std::vector<renderer_detail::GpuGeometryDrawRecord> geometry_draw_records_cpu;
    std::uint64_t dynamic_input_ref_offset{};
    std::uint32_t static_input_ref_count{}, dynamic_input_ref_count{};
    std::shared_ptr<const std::vector<GpuStaticDrawRef>> static_input_refs;
    std::vector<GpuSceneSerializedSlot> scene_serialization_scratch;
    std::vector<PersistentGpuSceneRange> scene_dirty_scratch, scene_channel_dirty_scratch;
    // CPU-only per-view instance preparation. Reused only after this frame slot
    // has been waited; these vectors do not escape the synchronous frame build.
    std::vector<GpuInstance> next_draw_instances_scratch;
    std::vector<GpuSceneCanonicalInputs> dynamic_canonical_inputs_scratch;
    std::vector<GpuStaticDrawRef> dynamic_draw_refs_scratch;
    std::vector<std::uint32_t> dynamic_skin_offsets_scratch;
    std::vector<std::uint32_t> dynamic_previous_skin_offsets_scratch;
    std::vector<std::uint32_t> dynamic_skin_vertex_counts_scratch;
    std::vector<std::uint64_t> dynamic_skin_geometry_ids_scratch;
    std::vector<std::uint32_t> dynamic_pose_flags_scratch;
    std::vector<float> dynamic_pose_times_scratch;
    std::vector<std::uint8_t> dynamic_skin_history_scratch;
    std::vector<std::uint8_t> dynamic_cooked_skin_bounds_scratch;
    std::vector<std::uint8_t> dynamic_skin_needed_scratch;
    std::vector<std::uint32_t> dynamic_skin_slots_scratch;
    AllocationResource skin_cache_vertices;
    AllocationResource skin_cache_bounds;
    std::size_t static_visible_capacity{};
    std::size_t static_rejected_capacity{};
    std::size_t static_retest_capacity{};
    std::size_t dynamic_visible_capacity{};
    std::size_t skin_cache_vertex_capacity{};
    std::size_t skin_cache_bound_capacity{};
    bool skin_cache_vertex_read{};
    bool skin_cache_bounds_read{};
    bool skin_cache_valid{};
    std::byte *mapped{};
    std::size_t upload_size{kFrameUploadSize};
    std::byte *ui_mapped{};
    CompletionSet completion{};
    CompletionSet skin_cache_use_completion{};
    bool timestamps_recorded{};
    GpuTimingMode timing_mode{GpuTimingMode::Off};
    std::uint64_t timing_layout_generation{};
    std::uint32_t timing_layout_capacity{};
    std::uint64_t submitted_frame_id{};
    std::uint64_t constants_submission_id{};
    std::uint32_t submitted_view_id{};
    GpuFrameTimingKind timing_kind{GpuFrameTimingKind::Unavailable};
    std::uint64_t timing_generation{}, timing_resize_epoch{};
    std::uint64_t timing_source_sequence{}, timing_quality_generation{}, timing_provider_generation{};
    ViewExtent timing_render_active;
    bool timing_main_view{}, timing_frame_generation_enabled{};
    struct ClockAnchor
    {
        std::uint64_t gpu{}, cpu{}, gpu_frequency{}, cpu_frequency{};
        bool valid{};
    };
    std::array<ClockAnchor, 3> timing_clock_before{};
    std::vector<std::uint32_t> timing_closure_slots;
    bool timing_closure_imports_ready{};
    std::uint64_t timing_execute_begin_qpc{}, timing_execute_end_qpc{};
    CompletionSet timing_prior_output_completion;
    bool timing_backlog_at_submit{};
    std::uint64_t cpu_qpc_frequency{};
    mutable std::uint64_t timing_stream_published_frame_id{};
    float cpu_recording_ms{};
    bool cpu_recording_ms_valid{};
    std::array<Microsoft::WRL::ComPtr<ID3D12QueryHeap>, 3> timestamp_heaps;
    std::array<AllocationResource, 3> timestamp_readbacks;
    std::array<std::uint64_t *, 3> mapped_timestamps{};
    std::uint32_t timestamp_capacity{};
    std::uint64_t ui_upload_bytes{};
    std::vector<GpuPassTiming> timestamp_passes;
    std::vector<std::uint64_t> timestamp_frequencies;
    std::vector<std::uint32_t> timestamp_pass_query_slots;
    std::vector<std::uint32_t> timestamp_query_slots;
};

} // namespace hs
