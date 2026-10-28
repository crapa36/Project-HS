#pragma once

#include "render_graph.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>

namespace D3D12MA { class Allocator; }

namespace hs
{
inline constexpr std::uint64_t kMaximumHostStagingBudgetBytes = 512ull * 1024u * 1024u;

enum class ResidencyCategory : std::uint8_t
{
    Texture,
    Geometry,
    Skin,
    Shadow,
    Gi,
    History,
    Transient,
    Sdk,
    Staging,
    Count,
};

struct ResidencyBudgetSnapshot
{
    bool valid{};
    std::uint64_t local_budget{};
    std::uint64_t local_usage{};
    std::uint64_t nonlocal_budget{};
    std::uint64_t nonlocal_usage{};
    bool host_budget_valid{};
    std::uint64_t host_staging_budget{};
    bool nonlocal_valid{};
    bool local_fresh{}, nonlocal_fresh{}, host_fresh{};
    bool unified_memory{};
};

enum class ResidencySegment : std::uint8_t { Local, NonLocal };

// A finite, essential-only startup ceiling derived from adapter-reported
// maximum physical capacity. It is not DXGI current availability and must
// never be used for optional admission.
class EssentialBootstrapAdmission
{
  public:
    [[nodiscard]] bool Configure(std::uint64_t local_capacity, std::uint64_t nonlocal_capacity,
                                 bool unified_memory) noexcept;
    void Reset() noexcept;
    [[nodiscard]] bool HasCapacity(ResidencySegment segment) const noexcept;
    [[nodiscard]] bool CanAdmit(std::uint64_t bytes, ResidencySegment segment) const noexcept;
    [[nodiscard]] bool Commit(std::uint64_t actual_allocation_bytes,
                              ResidencySegment segment) noexcept;
    [[nodiscard]] std::uint64_t Used(ResidencySegment segment) const noexcept;

  private:
    std::array<std::uint64_t, 2> capacity_{};
    std::array<std::uint64_t, 2> used_{};
    bool unified_memory_{};
};

enum class ResidencyStorage : std::uint8_t
{
    GpuAllocation,
    HostStaging,
    GpuReservation,
    HostReservation,
};

struct ResidencyTicket
{
    std::uint64_t value{};
    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    friend bool operator==(ResidencyTicket, ResidencyTicket) = default;
};

struct ResidencyAllocatorStatistics
{
    std::uint64_t local_block_bytes{}, nonlocal_block_bytes{};
    std::uint64_t local_usage_bytes{}, nonlocal_usage_bytes{};
};

struct ResidencyAccounting
{
    ResidencyBudgetSnapshot budget;
    ResidencyAllocatorStatistics allocator_stats;
    bool allocator_stats_available{};
    std::array<std::uint64_t, static_cast<std::size_t>(ResidencyCategory::Count)> live_bytes{};
    std::array<std::uint64_t, static_cast<std::size_t>(ResidencyCategory::Count)> retiring_bytes{};
    // These totals contain native GPU allocations. Host payloads and both
    // reservation domains are reported independently below.
    std::uint64_t live_total{};
    std::uint64_t retiring_total{};
    std::uint64_t reclaimed_total{};
    std::uint64_t host_staging_total{}, host_reserved_total{}, gpu_reserved_total{}, gpu_growth_since_snapshot{};
    std::uint64_t local_growth_since_snapshot{}, nonlocal_growth_since_snapshot{};
    std::array<std::uint64_t, 2> gpu_reserved_by_segment{};
    std::array<std::uint64_t, static_cast<std::size_t>(ResidencyCategory::Count)> local_live_bytes{}, local_retiring_bytes{}, nonlocal_live_bytes{}, nonlocal_retiring_bytes{};
};

struct ResidencyHostAdmissionFacts
{
    ResidencyBudgetSnapshot budget;
    std::uint64_t held_bytes{};
    bool held_overflow{};
};

struct ResidencyGpuPressureFacts
{
    ResidencyBudgetSnapshot budget;
    std::uint64_t growth_since_snapshot{};
    std::uint64_t local_growth_since_snapshot{}, nonlocal_growth_since_snapshot{};
};

class ResidencyManager
{
  public:
    void Reset() noexcept;
    void UpdateBudget(ResidencyBudgetSnapshot snapshot) noexcept;
    void RecordNativeGrowth(ResidencySegment segment, std::uint64_t bytes) noexcept;
    [[nodiscard]] ResidencyAllocatorStatistics SampleAllocatorStats(D3D12MA::Allocator *,
        bool local_absorbed = false, bool nonlocal_absorbed = false) noexcept;

    // Leave caller-selected headroom. Unknown or stale segment capacity defers
    // optional native allocations until a successful observation arrives.
    [[nodiscard]] bool CanAdmit(std::uint64_t bytes, std::uint32_t reserve_percent = 10,
                                ResidencySegment segment = ResidencySegment::Local) const noexcept;
    [[nodiscard]] bool CanStage(std::uint64_t bytes) const noexcept;
    [[nodiscard]] ResidencyTicket ReserveGpu(std::uint64_t bytes,
        ResidencySegment segment = ResidencySegment::Local);
    [[nodiscard]] ResidencyTicket ReserveHost(std::uint64_t bytes);
    [[nodiscard]] bool CommitHost(ResidencyTicket, std::uint64_t bytes) noexcept;

    [[nodiscard]] ResidencyTicket Register(ResidencyCategory category,
                                           std::uint64_t bytes, bool pinned,
                                           ResidencyStorage storage = ResidencyStorage::GpuAllocation,
                                           ResidencySegment segment = ResidencySegment::Local);
    [[nodiscard]] bool Retire(ResidencyTicket ticket,
                              const CompletionSet &completion) noexcept;
    [[nodiscard]] bool ReleaseNow(ResidencyTicket ticket) noexcept;
    std::uint32_t Collect(const CompletionSet &completed) noexcept;

    [[nodiscard]] bool Contains(ResidencyTicket ticket) const noexcept;
    [[nodiscard]] bool IsPinned(ResidencyTicket ticket) const noexcept;
    [[nodiscard]] ResidencyHostAdmissionFacts HostAdmissionFacts() const noexcept
    {
        return {budget_, host_held_, host_held_overflow_};
    }
    [[nodiscard]] ResidencyGpuPressureFacts GpuPressureFacts() const noexcept
    {
        return {budget_, gpu_growth_since_snapshot_, segment_growth_since_snapshot_[0],
            segment_growth_since_snapshot_[1]};
    }
    [[nodiscard]] ResidencyAccounting Accounting() const noexcept;

  private:
    struct Allocation
    {
        ResidencyCategory category{};
        std::uint64_t bytes{};
        bool pinned{};
        bool retiring{};
        CompletionSet completion{};
        ResidencyStorage storage{};
        ResidencySegment segment{ResidencySegment::Local};
    };

    void UpdateAdmissionTotals(ResidencyStorage storage, ResidencySegment segment,
        std::uint64_t previous, std::uint64_t next) noexcept;

    std::array<std::uint64_t, 2> gpu_reserved_{};
    std::uint64_t host_held_{};
    std::array<bool, 2> gpu_reserved_overflow_{};
    bool host_held_overflow_{};
    ResidencyBudgetSnapshot budget_{};
    bool unified_memory_{};
    std::unordered_map<std::uint64_t, Allocation> allocations_;
    std::uint64_t next_ticket_{1};
    std::uint64_t retiring_count_{};
    std::uint64_t reclaimed_total_{};
    std::uint64_t gpu_growth_since_snapshot_{};
    std::array<std::uint64_t, 2> segment_growth_since_snapshot_{};
    ResidencyAllocatorStatistics allocator_stats_{};
    bool has_allocator_stats_{};
};

// A synchronous native-allocation transaction holds its peak admission until
// all resulting resources have been registered. Failure releases the reservation.
class ScopedGpuAdmission
{
  public:
    ScopedGpuAdmission(ResidencyManager &manager, std::uint64_t bytes,
        ResidencySegment segment = ResidencySegment::Local)
        : manager_(manager), ticket_(bytes ? manager.ReserveGpu(bytes, segment) : ResidencyTicket{}),
          valid_(!bytes || static_cast<bool>(ticket_)) {}
    ScopedGpuAdmission(ResidencyManager &manager, std::uint64_t local_bytes,
        std::uint64_t nonlocal_bytes)
        : manager_(manager), ticket_(local_bytes ? manager.ReserveGpu(local_bytes, ResidencySegment::Local) : ResidencyTicket{}),
          nonlocal_ticket_(nonlocal_bytes ? manager.ReserveGpu(nonlocal_bytes, ResidencySegment::NonLocal) : ResidencyTicket{}),
          valid_((!local_bytes || static_cast<bool>(ticket_)) && (!nonlocal_bytes || static_cast<bool>(nonlocal_ticket_)))
    {
        if (!valid_)
        {
            if (ticket_) static_cast<void>(manager_.ReleaseNow(ticket_));
            ticket_ = {};
            if (nonlocal_ticket_) static_cast<void>(manager_.ReleaseNow(nonlocal_ticket_));
            nonlocal_ticket_ = {};
        }
    }
    ~ScopedGpuAdmission() { if (ticket_) static_cast<void>(manager_.ReleaseNow(ticket_)); if (nonlocal_ticket_) static_cast<void>(manager_.ReleaseNow(nonlocal_ticket_)); }
    // The native resources are now accounted as allocations; remove their
    // provisional peak before another admission or budget observation.
    void Complete() noexcept
    {
        if (ticket_) static_cast<void>(manager_.ReleaseNow(ticket_));
        if (nonlocal_ticket_) static_cast<void>(manager_.ReleaseNow(nonlocal_ticket_));
        ticket_ = {};
        nonlocal_ticket_ = {};
    }
    ScopedGpuAdmission(const ScopedGpuAdmission &) = delete;
    ScopedGpuAdmission &operator=(const ScopedGpuAdmission &) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return valid_ && (!ticket_.value || static_cast<bool>(ticket_)) && (!nonlocal_ticket_.value || static_cast<bool>(nonlocal_ticket_)); }
  private:
    ResidencyManager &manager_;
    ResidencyTicket ticket_;
    ResidencyTicket nonlocal_ticket_;
    bool valid_{true};
};

} // namespace hs
