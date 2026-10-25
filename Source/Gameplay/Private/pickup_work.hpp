#pragma once

#include <hs/game_domain/game_types.hpp>
#include "spatial_grid.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <vector>

namespace hs::gameplay_detail
{

struct PickupActor
{
    EntityId id{};
    PickupKind kind{PickupKind::Experience};
    Float2 position{};
    std::uint32_t value{};
    Tick spawned_tick{};
    bool guaranteed_boss_chest{};
    bool attracted_by_magnet_stat{};
    bool globally_attracted{};
    bool dead{};
    // Actual pickup motion clock, presentation-only and excluded from checksum.
    Tick visual_motion_started{}, visual_motion_updated{};
    float visual_motion_initial_distance{};
};

// Derived work membership only; PickupActor remains the sole payload owner.
// Logical IDs survive stable compaction. No actor pointer or row index is retained.
class PickupWorkOwner
{
  public:
    using Id = std::uint64_t;

    void Reserve(std::size_t count)
    {
        stationary_cells_.reserve(count);
        queued_ids_.reserve(count);
        visited_ids_.reserve(count);
    }

    void Reset() noexcept
    {
        ClearIndices();
        queued_ids_.clear();
        visited_ids_.clear();
        next_ = 0;
        indexed_ = true;
        has_extent_ = false;
        needs_compaction_ = false;
    }
    void Invalidate() noexcept { indexed_ = false; }
    [[nodiscard]] bool NeedsCompaction() const noexcept { return needs_compaction_; }
    void AfterCompaction() noexcept { needs_compaction_ = false; }

    void OnSpawn(const PickupActor &pickup, float arena_half_extent)
    {
        if (!indexed_) return;
        if (has_extent_ && extent_ != arena_half_extent) { Invalidate(); return; }
        indexed_ = false;
        extent_ = arena_half_extent;
        has_extent_ = true;
        Index(pickup, false);
        indexed_ = true;
    }

    void OnCollected(Id id) noexcept
    {
        needs_compaction_ = true;
        if (!indexed_) return;
        RemoveStationary(id);
        Erase(active_ids_, id);
        Erase(utility_ids_, id);
    }

    void Update(const PickupActor &pickup, bool moving)
    {
        if (!indexed_) return;
        if (pickup.dead)
        {
            OnCollected(pickup.id.value);
            return;
        }
        indexed_ = false;
        if (pickup.kind == PickupKind::Experience)
        {
            if (pickup.globally_attracted || moving)
            {
                RemoveStationary(pickup.id.value);
                active_ids_.insert(pickup.id.value);
            }
            else
            {
                Erase(active_ids_, pickup.id.value);
                UpsertStationary(pickup.id.value, pickup.position);
            }
        }
        indexed_ = true;
    }

    void Begin(std::span<const PickupActor> pickups, Float2 player,
               float attraction_radius, float collection_radius,
               bool auto_collect, float arena_half_extent)
    {
        if (!indexed_ || !has_extent_ || extent_ != arena_half_extent)
            Rebuild(pickups, arena_half_extent);
        queued_ids_.clear();
        visited_ids_.clear();
        next_ = 0;
        Append(active_ids_);
        Append(utility_ids_);
        const auto radius = std::max(std::abs(attraction_radius), std::abs(collection_radius));
        int low_x{}, low_y{}, high_x = kGridDimension - 1, high_y = kGridDimension - 1;
        if (std::isfinite(player.x) && std::isfinite(player.y) && std::isfinite(radius))
        {
            // One extra cell is conservative at float-rounded radius/cell boundaries.
            low_x = std::max(0, Coordinate(player.x - radius) - 1);
            low_y = std::max(0, Coordinate(player.y - radius) - 1);
            high_x = std::min(kGridDimension - 1, Coordinate(player.x + radius) + 1);
            high_y = std::min(kGridDimension - 1, Coordinate(player.y + radius) + 1);
        }
        for (auto y = low_y; y <= high_y; ++y)
            for (auto x = low_x; x <= high_x; ++x)
                Append(bins_[static_cast<std::size_t>(y * kGridDimension + x)]);
        if (auto_collect)
            for (const auto &pickup : pickups)
                if (!pickup.dead && (pickup.kind == PickupKind::Experience ||
                                     pickup.kind == PickupKind::RelicChest))
                    queued_ids_.push_back(pickup.id.value);
        Canonicalize();
    }

    [[nodiscard]] std::optional<Id> Next() noexcept
    {
        if (next_ == queued_ids_.size()) return std::nullopt;
        return queued_ids_[next_++];
    }
    void Visit(Id id)
    {
        assert(visited_ids_.empty() || visited_ids_.back() < id);
        visited_ids_.push_back(id);
    }
    void AfterMagnet(std::span<const PickupActor> pickups, Id cursor, float arena_half_extent)
    {
        // Magnet is the exceptional all-XP operation. Earlier IDs become active
        // for the next tick; only later IDs may enter this tick's remaining work.
        if (!indexed_ || !has_extent_ || extent_ != arena_half_extent)
            Rebuild(pickups, arena_half_extent);
        indexed_ = false;
        for (const auto &pickup : pickups)
        {
            if (pickup.dead || pickup.kind != PickupKind::Experience) continue;
            if (pickup.globally_attracted)
            {
                RemoveStationary(pickup.id.value);
                active_ids_.insert(pickup.id.value);
            }
            if (pickup.id.value > cursor) queued_ids_.push_back(pickup.id.value);
        }
        indexed_ = true;
        Canonicalize();
    }
    [[nodiscard]] std::span<const Id> Queued() const noexcept { return queued_ids_; }
    [[nodiscard]] std::span<const Id> Visited() const noexcept { return visited_ids_; }

  private:
    std::array<std::vector<Id>, kGridCellCount> bins_;
    struct StationaryLocation { std::size_t cell{}, offset{}; };
    std::unordered_map<Id, StationaryLocation> stationary_cells_;
    std::set<Id> active_ids_, utility_ids_;
    std::vector<Id> queued_ids_, visited_ids_;
    std::size_t next_{};
    float extent_{};
    bool indexed_{true}, has_extent_{}, needs_compaction_{};

    static void Erase(std::set<Id> &ids, Id id) noexcept { ids.erase(id); }
    int Coordinate(float value) const noexcept
    {
        const auto low = -extent_;
        if (!(value > low)) return 0;
        if (value >= low + kGridDimension * kGridCellSize) return kGridDimension - 1;
        return std::clamp(static_cast<int>(std::floor((value + extent_) / kGridCellSize)),
                          0, kGridDimension - 1);
    }
    void RemoveStationary(Id id) noexcept
    {
        const auto found = stationary_cells_.find(id);
        if (found == stationary_cells_.end()) return;
        const auto location = found->second;
        auto &bin = bins_[location.cell];
        const auto moved = bin.back();
        bin[location.offset] = moved;
        // Cell order is private broad-phase scratch; only the merged work is canonical.
        if (moved != id) stationary_cells_.find(moved)->second.offset = location.offset;
        bin.pop_back();
        stationary_cells_.erase(found);
    }
    void UpsertStationary(Id id, Float2 position)
    {
        const auto cell = static_cast<std::size_t>(Coordinate(position.y) * kGridDimension + Coordinate(position.x));
        const auto found = stationary_cells_.find(id);
        if (found != stationary_cells_.end() && found->second.cell == cell) return;
        RemoveStationary(id);
        const auto offset = bins_[cell].size();
        bins_[cell].push_back(id);
        stationary_cells_.emplace(id, StationaryLocation{cell, offset});
    }
    void Index(const PickupActor &pickup, bool moving)
    {
        if (pickup.dead) { needs_compaction_ = true; return; }
        if (pickup.kind != PickupKind::Experience) utility_ids_.insert(pickup.id.value);
        else if (pickup.globally_attracted || moving) active_ids_.insert(pickup.id.value);
        else UpsertStationary(pickup.id.value, pickup.position);
    }
    void ClearIndices() noexcept
    {
        for (auto &bin : bins_) bin.clear();
        stationary_cells_.clear();
        active_ids_.clear();
        utility_ids_.clear();
    }
    void Rebuild(std::span<const PickupActor> pickups, float arena_half_extent)
    {
        indexed_ = false;
        ClearIndices();
        extent_ = arena_half_extent;
        has_extent_ = true;
        for (const auto &pickup : pickups) Index(pickup, false);
        indexed_ = true;
    }
    void Append(const std::vector<Id> &ids) { queued_ids_.insert(queued_ids_.end(), ids.begin(), ids.end()); }
    void Append(const std::set<Id> &ids) { queued_ids_.insert(queued_ids_.end(), ids.begin(), ids.end()); }
    void Canonicalize()
    {
        std::ranges::sort(queued_ids_);
        queued_ids_.erase(std::unique(queued_ids_.begin(), queued_ids_.end()), queued_ids_.end());
        visited_ids_.reserve(queued_ids_.size());
    }
};

} // namespace hs::gameplay_detail
