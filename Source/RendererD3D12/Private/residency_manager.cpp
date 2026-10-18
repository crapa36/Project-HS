#include "residency_manager.hpp"

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
    gpu_reserved_ = host_held_ = 0;
    gpu_reserved_overflow_ = host_held_overflow_ = false;
    allocations_.clear();
    next_ticket_ = 1;
    reclaimed_total_ = 0;
    gpu_growth_since_snapshot_ = 0;
}

void ResidencyManager::UpdateBudget(ResidencyBudgetSnapshot snapshot) noexcept
{
    budget_ = snapshot;
    // A successful query observes all native allocations made before it.
    // Destruction never subtracts from an old DXGI observation; only a fresh
    // query can credit reclaimed GPU memory.
    gpu_growth_since_snapshot_ = 0;
}

bool ResidencyManager::CanAdmit(std::uint64_t bytes,
                                std::uint32_t reserve_percent) const noexcept
{
    if (!budget_.valid || budget_.local_budget == 0) return true;
    reserve_percent = std::min(reserve_percent, 95u);
    const auto reserve = budget_.local_budget / 100 * reserve_percent;
    const auto safe_budget = budget_.local_budget > reserve ? budget_.local_budget - reserve : 0;
    std::uint64_t usage = budget_.local_usage;
    if (gpu_growth_since_snapshot_ > UINT64_MAX - usage) return false;
    usage += gpu_growth_since_snapshot_;
    if (gpu_reserved_overflow_ || gpu_reserved_ > UINT64_MAX - usage) return false;
    usage += gpu_reserved_;
    return usage <= safe_budget && bytes <= safe_budget - usage;
}

bool ResidencyManager::CanStage(std::uint64_t bytes) const noexcept
{
    if (!budget_.host_budget_valid) return true;
    return !host_held_overflow_ && host_held_ <= budget_.host_staging_budget &&
        bytes <= budget_.host_staging_budget - host_held_;
}

void ResidencyManager::UpdateAdmissionTotals(ResidencyStorage storage,
    std::uint64_t previous, std::uint64_t next) noexcept
{
    const bool gpu = storage == ResidencyStorage::GpuReservation;
    if (!gpu && storage != ResidencyStorage::HostStaging && storage != ResidencyStorage::HostReservation) return;
    auto &total = gpu ? gpu_reserved_ : host_held_;
    auto &overflow = gpu ? gpu_reserved_overflow_ : host_held_overflow_;
    if (overflow)
    {
        // Mutations already reached the ledger; recompute only to recover an overflowed sum.
        total = 0;
        overflow = false;
        for (const auto &[ticket, allocation] : allocations_)
        {
            static_cast<void>(ticket);
            const bool included = gpu ? allocation.storage == ResidencyStorage::GpuReservation :
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

ResidencyTicket ResidencyManager::ReserveGpu(std::uint64_t bytes)
{
    if (!CanAdmit(bytes)) return {};
    return Register(ResidencyCategory::Staging, bytes, false, ResidencyStorage::GpuReservation);
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
    UpdateAdmissionTotals(ResidencyStorage::HostStaging, previous, bytes);
    return true;
}

ResidencyTicket ResidencyManager::Register(ResidencyCategory category,
                                           std::uint64_t bytes, bool pinned, ResidencyStorage storage)
{
    if (category == ResidencyCategory::Count || bytes == 0 ||
        next_ticket_ == std::numeric_limits<std::uint64_t>::max() ||
        (storage == ResidencyStorage::GpuAllocation && bytes > UINT64_MAX - gpu_growth_since_snapshot_))
        return {};
    const auto ticket = ResidencyTicket{next_ticket_++};
    allocations_.emplace(ticket.value, Allocation{category, bytes, pinned, false, {}, storage});
    if (storage == ResidencyStorage::GpuAllocation) gpu_growth_since_snapshot_ += bytes;
    UpdateAdmissionTotals(storage, 0, bytes);
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
    const auto bytes = found->second.bytes;
    allocations_.erase(found);
    UpdateAdmissionTotals(storage, bytes, 0);
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
        const auto bytes = it->second.bytes;
        it = allocations_.erase(it);
        UpdateAdmissionTotals(storage, bytes, 0);
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
    accounting.reclaimed_total = reclaimed_total_;
    accounting.gpu_growth_since_snapshot = gpu_growth_since_snapshot_;
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
        if (allocation.retiring)
            accounting.retiring_total += allocation.bytes;
        else
            accounting.live_total += allocation.bytes;
    }
    return accounting;
}

} // namespace hs
