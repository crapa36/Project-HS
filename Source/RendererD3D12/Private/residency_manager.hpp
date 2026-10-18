#pragma once

#include "render_graph.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>

namespace hs
{

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

struct ResidencyAccounting
{
    ResidencyBudgetSnapshot budget;
    std::array<std::uint64_t, static_cast<std::size_t>(ResidencyCategory::Count)> live_bytes{};
    std::array<std::uint64_t, static_cast<std::size_t>(ResidencyCategory::Count)> retiring_bytes{};
    // These totals contain native GPU allocations. Host payloads and both
    // reservation domains are reported independently below.
    std::uint64_t live_total{};
    std::uint64_t retiring_total{};
    std::uint64_t reclaimed_total{};
    std::uint64_t host_staging_total{}, host_reserved_total{}, gpu_reserved_total{}, gpu_growth_since_snapshot{};
};

class ResidencyManager
{
  public:
    void Reset() noexcept;
    void UpdateBudget(ResidencyBudgetSnapshot snapshot) noexcept;

    // Leave caller-selected headroom for the OS, other processes and untracked
    // renderer allocations. If DXGI does not expose a useful budget (for
    // example on some software adapters), admission stays enabled and the
    // tracked accounting still remains exact.
    [[nodiscard]] bool CanAdmit(std::uint64_t bytes,
                                std::uint32_t reserve_percent = 10) const noexcept;
    [[nodiscard]] bool CanStage(std::uint64_t bytes) const noexcept;
    [[nodiscard]] ResidencyTicket ReserveGpu(std::uint64_t bytes);
    [[nodiscard]] ResidencyTicket ReserveHost(std::uint64_t bytes);
    [[nodiscard]] bool CommitHost(ResidencyTicket, std::uint64_t bytes) noexcept;

    [[nodiscard]] ResidencyTicket Register(ResidencyCategory category,
                                           std::uint64_t bytes, bool pinned,
                                           ResidencyStorage storage = ResidencyStorage::GpuAllocation);
    [[nodiscard]] bool Retire(ResidencyTicket ticket,
                              const CompletionSet &completion) noexcept;
    [[nodiscard]] bool ReleaseNow(ResidencyTicket ticket) noexcept;
    std::uint32_t Collect(const CompletionSet &completed) noexcept;

    [[nodiscard]] bool Contains(ResidencyTicket ticket) const noexcept;
    [[nodiscard]] bool IsPinned(ResidencyTicket ticket) const noexcept;
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
    };

    void UpdateAdmissionTotals(ResidencyStorage storage, std::uint64_t previous, std::uint64_t next) noexcept;

    std::uint64_t gpu_reserved_{}, host_held_{};
    bool gpu_reserved_overflow_{}, host_held_overflow_{};
    ResidencyBudgetSnapshot budget_{};
    std::unordered_map<std::uint64_t, Allocation> allocations_;
    std::uint64_t next_ticket_{1};
    std::uint64_t reclaimed_total_{};
    std::uint64_t gpu_growth_since_snapshot_{};
};

// A synchronous native-allocation transaction holds its peak admission until
// all resulting resources have been registered. Failure releases the reservation.
class ScopedGpuAdmission
{
  public:
    ScopedGpuAdmission(ResidencyManager &manager, std::uint64_t bytes)
        : manager_(manager), ticket_(manager.ReserveGpu(bytes)) {}
    ~ScopedGpuAdmission() { if (ticket_) static_cast<void>(manager_.ReleaseNow(ticket_)); }
    // The native resources are now accounted as allocations; remove their
    // provisional peak before another admission or budget observation.
    void Complete() noexcept
    {
        if (ticket_) static_cast<void>(manager_.ReleaseNow(ticket_));
        ticket_ = {};
    }
    ScopedGpuAdmission(const ScopedGpuAdmission &) = delete;
    ScopedGpuAdmission &operator=(const ScopedGpuAdmission &) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(ticket_); }
  private:
    ResidencyManager &manager_;
    ResidencyTicket ticket_;
};

} // namespace hs
