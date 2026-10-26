#include "residency_manager.hpp"

#include <D3D12MemAlloc.h>

#include <algorithm>
#include <limits>

namespace hs
{

namespace
{
constexpr auto CategoryIndex(ResidencyCategory category) noexcept
{
    return static_cast<std::size_t>(category);
}
} // namespace

void ResidencyManager::Reset() noexcept
{
    budget_ = {};
    unified_memory_ = false;
    gpu_reserved_ = {};
    host_held_ = 0;
    gpu_reserved_overflow_ = {};
    host_held_overflow_ = false;
    allocations_.clear();
    next_ticket_ = 1;
    reclaimed_total_ = 0;
    gpu_growth_since_snapshot_ = 0;
    segment_growth_since_snapshot_ = {};
    allocator_stats_ = {};
    has_allocator_stats_ = false;
}

void ResidencyManager::UpdateBudget(ResidencyBudgetSnapshot snapshot) noexcept
{
    unified_memory_ = snapshot.unified_memory;
    if (snapshot.valid)
    {
        budget_.valid = true;
        budget_.local_budget = snapshot.local_budget;
        budget_.local_usage = snapshot.local_usage;
        segment_growth_since_snapshot_[static_cast<std::size_t>(ResidencySegment::Local)] = 0;
    }
    budget_.local_fresh = snapshot.local_fresh || snapshot.valid;
    if (snapshot.nonlocal_valid)
    {
        budget_.nonlocal_valid = true;
        budget_.nonlocal_budget = snapshot.nonlocal_budget;
        budget_.nonlocal_usage = snapshot.nonlocal_usage;
        segment_growth_since_snapshot_[static_cast<std::size_t>(ResidencySegment::NonLocal)] = 0;
    }
    budget_.nonlocal_fresh = snapshot.nonlocal_fresh || snapshot.nonlocal_valid;
    if (snapshot.host_budget_valid)
    {
        budget_.host_budget_valid = true;
        budget_.host_staging_budget = snapshot.host_staging_budget;
    }
    budget_.host_fresh = snapshot.host_fresh || snapshot.host_budget_valid;
    gpu_growth_since_snapshot_ = segment_growth_since_snapshot_[0] > UINT64_MAX - segment_growth_since_snapshot_[1]
        ? UINT64_MAX : segment_growth_since_snapshot_[0] + segment_growth_since_snapshot_[1];
}

void ResidencyManager::RecordNativeGrowth(ResidencySegment segment, std::uint64_t bytes) noexcept
{
    auto &growth = segment_growth_since_snapshot_[static_cast<std::size_t>(segment)];
    growth = bytes > UINT64_MAX - growth ? UINT64_MAX : growth + bytes;
    gpu_growth_since_snapshot_ = growth > UINT64_MAX - segment_growth_since_snapshot_[1u - static_cast<std::size_t>(segment)]
        ? UINT64_MAX : growth + segment_growth_since_snapshot_[1u - static_cast<std::size_t>(segment)];
}

ResidencyAllocatorStatistics ResidencyManager::SampleAllocatorStats(D3D12MA::Allocator *allocator,
    bool local_absorbed, bool nonlocal_absorbed) noexcept
{
    if (!allocator) return allocator_stats_;
    D3D12MA::Budget local{}, nonlocal{};
    allocator->GetBudget(&local, &nonlocal);
    const ResidencyAllocatorStatistics current{local.Stats.BlockBytes, nonlocal.Stats.BlockBytes,
        local.UsageBytes, nonlocal.UsageBytes};
    if (has_allocator_stats_)
    {
        // Block growth covers new backing heaps; usage growth covers extra allocations within existing blocks.
        const auto growth = [](std::uint64_t before_blocks, std::uint64_t after_blocks,
                               std::uint64_t before_usage, std::uint64_t after_usage) {
            const auto blocks = after_blocks > before_blocks ? after_blocks - before_blocks : 0;
            const auto usage = after_usage > before_usage ? after_usage - before_usage : 0;
            return std::max(blocks, usage);
        };
        if (!local_absorbed) RecordNativeGrowth(ResidencySegment::Local, growth(
            allocator_stats_.local_block_bytes, current.local_block_bytes,
            allocator_stats_.local_usage_bytes, current.local_usage_bytes));
        if (!nonlocal_absorbed) RecordNativeGrowth(ResidencySegment::NonLocal, growth(
            allocator_stats_.nonlocal_block_bytes, current.nonlocal_block_bytes,
            allocator_stats_.nonlocal_usage_bytes, current.nonlocal_usage_bytes));
    }
    allocator_stats_ = current;
    has_allocator_stats_ = true;
    return current;
}

bool ResidencyManager::CanAdmit(std::uint64_t bytes, std::uint32_t reserve_percent,
                                ResidencySegment segment) const noexcept
{
    const auto index = static_cast<std::size_t>(segment);
    const bool valid = segment == ResidencySegment::Local ? budget_.valid : budget_.nonlocal_valid;
    const auto domain_budget = segment == ResidencySegment::Local ? budget_.local_budget : budget_.nonlocal_budget;
    const auto domain_usage = segment == ResidencySegment::Local ? budget_.local_usage : budget_.nonlocal_usage;
    const bool fresh = segment == ResidencySegment::Local ? budget_.local_fresh : budget_.nonlocal_fresh;
    if (!valid || !fresh || domain_budget == 0) return false;
    reserve_percent = std::min(reserve_percent, 95u);
    const auto reserve = domain_budget / 100 * reserve_percent;
    const auto safe_budget = domain_budget > reserve ? domain_budget - reserve : 0;
    std::uint64_t usage = domain_usage;
    if (segment_growth_since_snapshot_[index] > UINT64_MAX - usage) return false;
    usage += segment_growth_since_snapshot_[index];
    if (gpu_reserved_overflow_[index] || gpu_reserved_[index] > UINT64_MAX - usage) return false;
    usage += gpu_reserved_[index];
    return usage <= safe_budget && bytes <= safe_budget - usage;
}

bool ResidencyManager::CanStage(std::uint64_t bytes) const noexcept
{
    if (!budget_.host_budget_valid || !budget_.host_fresh) return false;
    return !host_held_overflow_ && host_held_ <= budget_.host_staging_budget &&
        bytes <= budget_.host_staging_budget - host_held_;
}

void ResidencyManager::UpdateAdmissionTotals(ResidencyStorage storage, ResidencySegment segment,
    std::uint64_t previous, std::uint64_t next) noexcept
{
    const bool gpu = storage == ResidencyStorage::GpuReservation;
    if (!gpu && storage != ResidencyStorage::HostStaging && storage != ResidencyStorage::HostReservation) return;
    auto &total = gpu ? gpu_reserved_[static_cast<std::size_t>(segment)] : host_held_;
    auto &overflow = gpu ? gpu_reserved_overflow_[static_cast<std::size_t>(segment)] : host_held_overflow_;
    if (overflow)
    {
        // Mutations already reached the ledger; recompute only to recover an overflowed sum.
        total = 0;
        overflow = false;
        for (const auto &[ticket, allocation] : allocations_)
        {
            static_cast<void>(ticket);
            const bool included = gpu ? allocation.storage == ResidencyStorage::GpuReservation && allocation.segment == segment :
                allocation.storage == ResidencyStorage::HostStaging || allocation.storage == ResidencyStorage::HostReservation;
            if (!included) continue;
            if (allocation.bytes > UINT64_MAX - total) { overflow = true; break; }
            total += allocation.bytes;
        }
        return;
    }
    total -= previous;
    if (next > UINT64_MAX - total) overflow = true;
    else total += next;
}

ResidencyTicket ResidencyManager::ReserveGpu(std::uint64_t bytes, ResidencySegment segment)
{
    if (unified_memory_) segment = ResidencySegment::Local;
    if (!CanAdmit(bytes, 10, segment)) return {};
    return Register(ResidencyCategory::Staging, bytes, false, ResidencyStorage::GpuReservation, segment);
}

ResidencyTicket ResidencyManager::ReserveHost(std::uint64_t bytes)
{
    if (!CanStage(bytes)) return {};
    return Register(ResidencyCategory::Staging, bytes, false, ResidencyStorage::HostReservation);
}

bool ResidencyManager::CommitHost(ResidencyTicket ticket, std::uint64_t bytes) noexcept
{
    const auto found = allocations_.find(ticket.value);
    if (found == allocations_.end() || found->second.storage != ResidencyStorage::HostReservation ||
        !bytes || bytes > found->second.bytes) return false;
    const auto previous = found->second.bytes;
    found->second.storage = ResidencyStorage::HostStaging;
    found->second.bytes = bytes;
    UpdateAdmissionTotals(ResidencyStorage::HostStaging, ResidencySegment::Local, previous, bytes);
    return true;
}

ResidencyTicket ResidencyManager::Register(ResidencyCategory category,
                                           std::uint64_t bytes, bool pinned, ResidencyStorage storage,
                                           ResidencySegment segment)
{
    if (category == ResidencyCategory::Count || bytes == 0 ||
        next_ticket_ == std::numeric_limits<std::uint64_t>::max()) return {};
    const auto ticket = ResidencyTicket{next_ticket_++};
    allocations_.emplace(ticket.value, Allocation{category, bytes, pinned, false, {}, storage, segment});
    UpdateAdmissionTotals(storage, segment, 0, bytes);
    return ticket;
}

bool ResidencyManager::Retire(ResidencyTicket ticket,
                              const CompletionSet &completion) noexcept
{
    const auto found = allocations_.find(ticket.value);
    if (found == allocations_.end() || found->second.pinned) return false;
    found->second.retiring = true;
    found->second.completion.Merge(completion);
    return true;
}

bool ResidencyManager::ReleaseNow(ResidencyTicket ticket) noexcept
{
    const auto found = allocations_.find(ticket.value);
    if (found == allocations_.end()) return false;
    if (found->second.storage == ResidencyStorage::GpuAllocation) reclaimed_total_ += found->second.bytes;
    const auto storage = found->second.storage;
    const auto segment = found->second.segment;
    const auto bytes = found->second.bytes;
    allocations_.erase(found);
    UpdateAdmissionTotals(storage, segment, bytes, 0);
    return true;
}

std::uint32_t ResidencyManager::Collect(const CompletionSet &completed) noexcept
{
    std::uint32_t collected = 0;
    for (auto it = allocations_.begin(); it != allocations_.end();)
    {
        if (!it->second.retiring || !it->second.completion.IsSatisfiedBy(completed))
        {
            ++it;
            continue;
        }
        if (it->second.storage == ResidencyStorage::GpuAllocation) reclaimed_total_ += it->second.bytes;
        const auto storage = it->second.storage;
        const auto segment = it->second.segment;
        const auto bytes = it->second.bytes;
        it = allocations_.erase(it);
        UpdateAdmissionTotals(storage, segment, bytes, 0);
        ++collected;
    }
    return collected;
}

bool ResidencyManager::Contains(ResidencyTicket ticket) const noexcept
{
    return allocations_.contains(ticket.value);
}

bool ResidencyManager::IsPinned(ResidencyTicket ticket) const noexcept
{
    const auto found = allocations_.find(ticket.value);
    return found != allocations_.end() && found->second.pinned;
}

ResidencyAccounting ResidencyManager::Accounting() const noexcept
{
    ResidencyAccounting accounting;
    accounting.budget = budget_;
    accounting.allocator_stats = allocator_stats_;
    accounting.allocator_stats_available = has_allocator_stats_;
    accounting.reclaimed_total = reclaimed_total_;
    accounting.gpu_growth_since_snapshot = gpu_growth_since_snapshot_;
    accounting.local_growth_since_snapshot = segment_growth_since_snapshot_[0];
    accounting.nonlocal_growth_since_snapshot = segment_growth_since_snapshot_[1];
    accounting.gpu_reserved_by_segment = gpu_reserved_;
    for (const auto &[ticket, allocation] : allocations_)
    {
        static_cast<void>(ticket);
        if (allocation.storage == ResidencyStorage::GpuReservation)
        {
            accounting.gpu_reserved_total += allocation.bytes;
            continue;
        }
        if (allocation.storage == ResidencyStorage::HostReservation)
        {
            accounting.host_reserved_total += allocation.bytes;
            continue;
        }
        if (allocation.storage == ResidencyStorage::HostStaging)
        {
            accounting.host_staging_total += allocation.bytes;
            continue;
        }
        auto &bucket = allocation.retiring ? accounting.retiring_bytes : accounting.live_bytes;
        bucket[CategoryIndex(allocation.category)] += allocation.bytes;
        auto &segment_bucket = allocation.segment == ResidencySegment::Local
            ? (allocation.retiring ? accounting.local_retiring_bytes : accounting.local_live_bytes)
            : (allocation.retiring ? accounting.nonlocal_retiring_bytes : accounting.nonlocal_live_bytes);
        segment_bucket[CategoryIndex(allocation.category)] += allocation.bytes;
        if (allocation.retiring)
            accounting.retiring_total += allocation.bytes;
        else
            accounting.live_total += allocation.bytes;
    }
    return accounting;
}

} // namespace hs
