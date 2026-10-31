#pragma once

#include <hs/core/result.hpp>
#include <hs/core/types.hpp>
#include "render_graph_types.hpp"

#include <d3d12.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace D3D12MA { class Allocator; }
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Fence;

namespace hs
{


enum class QueueHint : std::uint8_t
{
    Direct,
    Compute,
    Copy,
};

// Fence requirements carried by resources, frame slots, and external work.
// Values are monotonic per queue; merging keeps the latest requirement on each
// queue. Native provider fences are bridged onto these owned timelines; provider
// fence values must never be placed directly in this set.
struct CompletionSet
{
    std::uint64_t direct{};
    std::uint64_t compute{};
    std::uint64_t copy{};

    void Include(QueueHint queue, std::uint64_t value) noexcept
    {
        auto *target = queue == QueueHint::Direct ? &direct
            : queue == QueueHint::Compute ? &compute
            : queue == QueueHint::Copy ? &copy : nullptr;
        if (target && value > *target) *target = value;
    }

    void Merge(const CompletionSet &other) noexcept
    {
        Include(QueueHint::Direct, other.direct);
        Include(QueueHint::Compute, other.compute);
        Include(QueueHint::Copy, other.copy);
    }

    [[nodiscard]] std::uint64_t Required(QueueHint queue) const noexcept
    {
        return queue == QueueHint::Direct ? direct
            : queue == QueueHint::Compute ? compute
            : queue == QueueHint::Copy ? copy : 0;
    }

    [[nodiscard]] bool IsSatisfiedBy(const CompletionSet &completed) const noexcept
    {
        return completed.direct >= direct && completed.compute >= compute &&
               completed.copy >= copy;
    }
};

struct QueueScheduleDependency
{
    std::uint32_t producer_pass_id{};
    std::uint32_t consumer_pass_id{};
    QueueHint producer_queue{};
    QueueHint consumer_queue{};
};

[[nodiscard]] constexpr bool QueueAccessCompatible(QueueHint queue, Access access) noexcept
{
    switch (queue)
    {
    case QueueHint::Direct:
        return true;
    case QueueHint::Compute:
        return access == Access::Common || access == Access::ComputeRead ||
               access == Access::UnorderedRead || access == Access::UnorderedWrite ||
               access == Access::CopySource || access == Access::CopyDest ||
               access == Access::IndirectArgs;
    case QueueHint::Copy:
        return access == Access::Common || access == Access::CopySource ||
               access == Access::CopyDest;
    }
    return false;
}

// Half-open byte interval. Zero length and out-of-bounds intervals are invalid.
struct BufferRange
{
    std::uint64_t offset{};
    std::uint64_t size{};
};

// Mip, array slice, and format plane counts are all nonzero. Transient textures
// currently have one mip and one array slice; imports use their native extent.
struct TextureSubresourceRange
{
    std::uint32_t first_mip{};
    std::uint32_t mip_count{};
    std::uint32_t first_array_slice{};
    std::uint32_t array_slice_count{};
    std::uint32_t first_plane{};
    std::uint32_t plane_count{};
};

struct TextureDesc
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t format{};
    std::uint32_t flags{};
};

struct BufferDesc
{
    std::uint64_t size{};
    std::uint32_t flags{};
};

// Opaque owners point to retained ID3D12CommandQueue/ID3D12Fence interfaces.
// The optional lease owns provider command allocators/resources used in flight.
// Each operation submits only on its declared queue; arbitrary SDK release tokens
// and ranged external state handoffs are not supported.
struct ExternalQueueCompletion
{
    std::shared_ptr<void> native_fence_owner;
    std::uint64_t value{};
    std::shared_ptr<void> resource_lease;
};

struct ExternalQueueBinding
{
    std::shared_ptr<void> native_queue_owner;
    QueueHint queue_type{QueueHint::Direct};
    std::shared_ptr<void> resource_lease;
};

struct ExternalResourceAccess
{
    ResourceHandle resource;
    Access incoming{Access::Common};
    Access outgoing{Access::Common};
};

enum class NativeBackingKind : std::uint8_t
{
    Unknown,
    Allocation,
    DxgiSwapchain,
    OwnedReservedTiles,
};

// Issued by the current resource owner after validating native identity and
// backing ownership. OwnedReservedTiles attests that the streaming owner gives
// this reserved resource exclusive tile ranges for its mapping lifetime.
// This is frame binding data, never part of a cached graph plan.
struct NativeResourceContract
{
    void *native_resource{};
    void *resource_identity{};
    void *device_identity{};
    void *heap_identity{};
    D3D12_RESOURCE_DESC description{};
    D3D12_HEAP_TYPE buffer_heap{D3D12_HEAP_TYPE_DEFAULT};
    std::uint64_t heap_offset{}, heap_size{};
    std::uint64_t generation{};
    NativeBackingKind backing{NativeBackingKind::Unknown};
};

struct ExternalTexture
{
    void *native_resource{};
    CompletionSet completion{};
    QueueHint initial_queue{QueueHint::Direct};
    ExternalQueueCompletion external_completion{};
    const NativeResourceContract *contract{};
};

struct ExternalBuffer
{
    void *native_resource{};
    CompletionSet completion{};
    QueueHint initial_queue{QueueHint::Direct};
    ExternalQueueCompletion external_completion{};
    const NativeResourceContract *contract{};
};

class RenderGraphBuilder;
struct RenderTaskExecutor;
struct GpuPassTiming;

struct GraphExecutionOptions
{
    std::array<void *, 3> timestamp_query_heaps{};
    std::array<void *, 3> timestamp_readbacks{};
    std::uint32_t timestamp_base{};
    // Optional compiled execution-order to query-pair map; UINT32_MAX skips a pass.
    const std::uint32_t *timestamp_query_slots{};
    std::size_t timestamp_query_slot_count{};
    const RenderTaskExecutor *task_executor{};
    // Optional capture output in compiled execution order; each recording task owns one element.
    GpuPassTiming *cpu_pass_timings{};
    std::size_t cpu_pass_timing_count{};
};

struct GraphExecutionStats
{
    // Native ExecuteCommandLists calls, which may contain several Enhanced lists.
    std::uint32_t direct_batches{};
    std::uint32_t compute_batches{};
    std::uint32_t copy_batches{};
    std::uint32_t cross_queue_waits{};
    std::uint32_t record_jobs{};
    std::uint32_t transient_resource_count{};
    std::uint32_t transient_block_count{};
    std::uint32_t alias_activation_count{};
    std::uint64_t transient_bytes_without_aliasing{};
    std::uint64_t transient_backing_bytes{};
    // Sequential wall time: planning/recorder admission, recording plus join,
    // then retirement setup and native queue submission (including providers).
    std::uint64_t cpu_plan_us{}, cpu_record_us{}, cpu_submit_us{};
    // Optional Prepare output: native checks, exact inputs/key, compile/aliases,
    // then first/last use and transient claims. Published only on success.
    std::uint64_t cpu_prepare_validation_us{}, cpu_prepare_info_key_us{};
    std::uint64_t cpu_prepare_compile_us{}, cpu_prepare_transient_us{};
};

// One pool per frame slot. ResetClaims/Clear require that slot's GPU work to be complete.
class TransientResourcePool
{
  public:
    TransientResourcePool();
    ~TransientResourcePool();
    TransientResourcePool(const TransientResourcePool &) = delete;
    TransientResourcePool &operator=(const TransientResourcePool &) = delete;
    // Device must own the allocator and all resources imported into this pool.
    [[nodiscard]] Result Initialize(D3D12MA::Allocator *allocator, ID3D12Device *device);
    [[nodiscard]] Result ReserveTexture(const TextureDesc &description);
    void ResetClaims() noexcept;
    void SetClaimCompletion(const CompletionSet &completion) noexcept;
    void ResetCompletedClaims(const CompletionSet &completed) noexcept;
    void Clear() noexcept;
    void Swap(TransientResourcePool &other) noexcept;
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class RenderGraphBuilder;
};

// Owns the native Direct/Compute/Copy submission timelines used by queued
// RenderGraph execution. The existing renderer single-list path stays separate
// until individual production passes are explicitly migrated.
class RenderGraphQueueExecutor
{
  public:
    RenderGraphQueueExecutor();
    ~RenderGraphQueueExecutor();
    RenderGraphQueueExecutor(const RenderGraphQueueExecutor &) = delete;
    RenderGraphQueueExecutor &operator=(const RenderGraphQueueExecutor &) = delete;

    // An existing Direct queue may be supplied so swap-chain/render work and
    // graph work share one Direct timeline. Compute and Copy queues are owned by
    // this executor. Each queue has its own fence timeline.
    [[nodiscard]] Result Initialize(ID3D12Device *device,
                                    ID3D12CommandQueue *direct_queue = nullptr,
                                    std::uint32_t frame_slot_count = 3,
                                    bool copy_timestamp_queries_supported = false);
    [[nodiscard]] Result Execute(RenderGraphBuilder &graph, std::uint32_t frame_slot,
                                 BarrierMode barriers, CompletionSet &completion,
                                 const GraphExecutionOptions *options = nullptr);
    [[nodiscard]] CompletionSet Completed() const noexcept;
    [[nodiscard]] Result Wait(const CompletionSet &completion);
    [[nodiscard]] Result Join(const CompletionSet &completion, QueueHint destination);
    [[nodiscard]] Result Drain();
    [[nodiscard]] std::uint64_t TimestampFrequency(QueueHint queue) const noexcept;
    [[nodiscard]] GraphExecutionStats LastExecutionStats() const noexcept;
    [[nodiscard]] ID3D12CommandQueue *NativeQueue(QueueHint queue) const noexcept;
    [[nodiscard]] ID3D12Fence *NativeFence(QueueHint queue) const noexcept;
    [[nodiscard]] Result ReleaseRecordersAfterCompletion();
    void Clear() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class RenderPassContext
{
  public:
    [[nodiscard]] void *NativeCommandList() const noexcept;
    [[nodiscard]] void *Resolve(ResourceHandle handle) const noexcept;
    void UavBarrier(ResourceHandle handle) const noexcept;
    // Record a native SDK operation that tags whole resources using legacy
    // resource states. Enhanced-barrier graphs are bridged through COMMON on
    // both sides for default-heap buffers and textures; legacy graphs invoke
    // the callback without extra barriers.
    [[nodiscard]] Result RecordLegacyResourceCommands(
        std::span<const ExternalResourceAccess> accesses,
        const std::function<Result()> &execute) const;

  private:
    RenderPassContext(void *native_command_list, void *enhanced_command_list,
                      BarrierMode barrier_mode, const void *graph,
                      QueueHint queue = QueueHint::Direct) noexcept;

    void *native_command_list_{};
    void *enhanced_command_list_{};
    BarrierMode barrier_mode_{};
    const void *graph_{};
    QueueHint queue_{};

    friend class RenderGraphBuilder;
    friend class RenderGraphQueueExecutor;
};

class RenderExternalPassContext
{
  public:
    [[nodiscard]] void *NativeCommandQueue() const noexcept { return native_queue_; }
    [[nodiscard]] void *Resolve(ResourceHandle handle) const noexcept;

  private:
    RenderExternalPassContext(void *native_queue, const void *graph) noexcept
        : native_queue_(native_queue), graph_(graph) {}
    void *native_queue_{};
    const void *graph_{};
    friend class RenderGraphQueueExecutor;
};

class PassBuilder
{
  public:
    // Overloads without a range declare the entire resource.
    void Read(ResourceVersion version, Access access);
    void Read(ResourceVersion version, Access access, BufferRange range);
    void Read(ResourceVersion version, Access access, TextureSubresourceRange range);
    [[nodiscard]] ResourceVersion Write(ResourceVersion predecessor, Access access);
    [[nodiscard]] ResourceVersion Write(ResourceVersion predecessor, Access access, BufferRange range);
    [[nodiscard]] ResourceVersion Write(ResourceVersion predecessor, Access access, TextureSubresourceRange range);
    [[nodiscard]] ResourceVersion ReadWrite(ResourceVersion predecessor, Access access);
    [[nodiscard]] ResourceVersion ReadWrite(ResourceVersion predecessor, Access access, BufferRange range);
    [[nodiscard]] ResourceVersion ReadWrite(ResourceVersion predecessor, Access access, TextureSubresourceRange range);
    void SetExecute(std::move_only_function<void(RenderPassContext &)> execute);
    void SetExecuteChecked(std::move_only_function<Result(RenderPassContext &)> execute);
    // SDKs with initialization-thread affinity still record ordinary graph
    // commands. Their checked callback runs on the Execute caller, while other
    // passes retain worker recording; failure prevents submission of the graph.
    void SetExecuteOnOwner(std::move_only_function<Result(RenderPassContext &)> execute);
    // Read/Write still declare graph hazards. Every whole-resource use needs
    // exactly one physical boundary record. Buffers, Copy-queue resources and
    // simultaneous-access textures enter/leave COMMON in both barrier modes;
    // the provider owns transitions inside its submitted work. Outgoing access
    // must hold even when the callback reports failure after submission.
    // This callback runs on the submission owner after recording tasks join.
    // Return any provider fence even on failure after submitting work. The graph
    // also signals its own fence on the bound queue to establish safe retirement.
    void SetExternalExecute(ExternalQueueBinding binding,
        std::span<const ExternalResourceAccess> accesses,
        std::move_only_function<Result(RenderExternalPassContext &, ExternalQueueCompletion &)> execute);
    // A restored plan already owns its logical whole-resource boundaries.
    void BindExternalExecute(ExternalQueueBinding binding,
        std::move_only_function<Result(RenderExternalPassContext &, ExternalQueueCompletion &)> execute);

  private:
    PassBuilder(void *graph, std::uint32_t pass) noexcept;

    void *graph_{};
    std::uint32_t pass_{};

    friend class RenderGraphBuilder;
};

class RenderGraphBuilder
{
  public:
    using PlanHandle = std::uint64_t;
    RenderGraphBuilder();
    ~RenderGraphBuilder();

    RenderGraphBuilder(const RenderGraphBuilder &) = delete;
    RenderGraphBuilder &operator=(const RenderGraphBuilder &) = delete;

    void Reset();
    // Handles identify one of the eight retained value-only plans on this builder.
    // The caller must select by its exact pass/resource/use topology; a miss
    // leaves it free to Reset and declare a new topology. Current native imports,
    // state, completion and callbacks must all be rebound before Prepare.
    [[nodiscard]] bool RestorePlan(PlanHandle handle);
    [[nodiscard]] PlanHandle PreparedPlan() const noexcept;
    [[nodiscard]] bool AllImportsCertified() const noexcept;
    [[nodiscard]] bool BindTexture(TextureHandle slot, const ExternalTexture &texture, Access initial);
    [[nodiscard]] bool BindBuffer(BufferHandle slot, const ExternalBuffer &buffer, Access initial);
    [[nodiscard]] std::optional<PassBuilder> BindPass(std::uint32_t declaration_index,
                                                       std::uint32_t expected_pass_id);
    [[nodiscard]] std::uint32_t ResourceCount() const noexcept;
    [[nodiscard]] TextureHandle CreateTexture(const TextureDesc &description,
                                              std::string_view name);
    [[nodiscard]] BufferHandle CreateBuffer(const BufferDesc &description,
                                            std::string_view name);
    [[nodiscard]] TextureHandle ImportTexture(const ExternalTexture &texture, Access initial,
                                              std::string_view name);
    [[nodiscard]] BufferHandle ImportBuffer(const ExternalBuffer &buffer, Access initial,
                                            std::string_view name);
    [[nodiscard]] ResourceVersion Version(ResourceHandle resource) const noexcept { return {resource, 0}; }
    [[nodiscard]] PassBuilder AddPass(std::uint32_t pass_id, std::string_view name, QueueHint queue);
    // Explicit submission ordering for timing/provider boundaries that carry no
    // artificial resource access. IDs must exist; cycles are compiler errors.
    void AddOrderingDependency(std::uint32_t producer_id, std::uint32_t consumer_id);
    [[nodiscard]] std::uint32_t DeclaredPassId(std::uint32_t index) const noexcept;
    void SetFinalAccess(ResourceVersion version, Access access);

    // Null output performs no detailed timing clock reads. Non-null output is
    // cleared at entry, with completed stage durations published only on success.
    [[nodiscard]] Result Prepare(TransientResourcePool &pool, GraphExecutionStats *capture_cpu = nullptr);
    [[nodiscard]] void *Resolve(ResourceHandle handle) const noexcept;
    [[nodiscard]] Result Execute(void *native_command_list, void *enhanced_command_list,
                                 BarrierMode barriers, void *timestamp_query_heap = nullptr,
                                 void *timestamp_readback = nullptr,
                                 std::uint32_t timestamp_base = 0);
    [[nodiscard]] std::uint32_t PassCount() const noexcept;
    [[nodiscard]] std::uint32_t CompiledPassId(std::uint32_t execution_index) const noexcept;
    [[nodiscard]] std::string_view CompiledPassName(std::uint32_t execution_index) const noexcept;
    [[nodiscard]] QueueHint CompiledPassQueue(std::uint32_t execution_index) const noexcept;
    [[nodiscard]] std::uint32_t QueuePassCount(QueueHint queue) const noexcept;
    [[nodiscard]] std::uint32_t CompiledQueuePassId(QueueHint queue,
                                                    std::uint32_t queue_index) const noexcept;
    [[nodiscard]] std::uint32_t CrossQueueDependencyCount() const noexcept;
    [[nodiscard]] QueueScheduleDependency CrossQueueDependency(
        std::uint32_t dependency_index) const noexcept;
    // Execution slots on the physical queue/resource path to this output pass.
    [[nodiscard]] std::vector<std::uint32_t> OutputDependencyClosureSlots(std::uint32_t pass_id) const;
    [[nodiscard]] bool OutputClosureImportsReady(std::span<const std::uint32_t> slots,
                                                 const CompletionSet &completed) const noexcept;
    [[nodiscard]] Access FinalAccess(ResourceHandle resource) const noexcept;
    [[nodiscard]] CompletionSet ResourceCompletion(ResourceHandle resource) const noexcept;
    [[nodiscard]] QueueHint FinalQueue(ResourceHandle resource) const noexcept;

  private:
    [[nodiscard]] std::uint64_t PlanCompilationCount() const noexcept;
    [[nodiscard]] std::uint64_t PlanReuseCount() const noexcept;
    [[nodiscard]] std::uint64_t PlanInputBuildCount() const noexcept;
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend struct RenderGraphTestAccess;
    friend class PassBuilder;
    friend class RenderPassContext;
    friend class RenderExternalPassContext;
    friend class RenderGraphQueueExecutor;
};

} // namespace hs
