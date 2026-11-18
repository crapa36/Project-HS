#pragma once

#include <hs/game_domain/game_types.hpp>
#include "spatial_grid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
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
        locations_.reserve(count);
        queued_ids_.reserve(count);
    }

    void Reset() noexcept
    {
        ClearIndices();
        queued_ids_.clear();
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
        Remove(id);
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
                // Each live XP pickup belongs to exactly one of stationary or active.
                Upsert(pickup.id.value, kActiveBucket);
            }
            else
            {
                Upsert(pickup.id.value, StationaryBucket(pickup.position));
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
        next_ = 0;
        Append(buckets_[kActiveBucket]);
        Append(buckets_[kUtilityBucket]);
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
                Append(buckets_[static_cast<std::size_t>(y * kGridDimension + x)]);
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
    void ApplyMagnet(std::span<PickupActor> pickups, Id cursor)
    {
        // Magnet is the exceptional all-XP operation. Earlier IDs become active
        // for the next tick; only later IDs may enter this tick's remaining work.
        const auto previous_size = queued_ids_.size();
        indexed_ = false;
        for (auto &pickup : pickups)
        {
            if (pickup.dead || pickup.kind != PickupKind::Experience || pickup.globally_attracted) continue;
            pickup.globally_attracted = true;
            Upsert(pickup.id.value, kActiveBucket);
            if (pickup.id.value > cursor) queued_ids_.push_back(pickup.id.value);
        }
        indexed_ = true;
        if (queued_ids_.size() != previous_size) Canonicalize();
    }
    [[nodiscard]] std::span<const Id> Queued() const noexcept { return queued_ids_; }

  private:
    static constexpr std::size_t kActiveBucket = kGridCellCount;
    static constexpr std::size_t kUtilityBucket = kGridCellCount + 1;
    std::array<std::vector<Id>, kGridCellCount + 2> buckets_;
    struct Location { std::size_t bucket{}, offset{}; };
    std::unordered_map<Id, Location> locations_;
    std::vector<Id> queued_ids_;
    std::size_t next_{};
    float extent_{};
    bool indexed_{true}, has_extent_{}, needs_compaction_{};

    int Coordinate(float value) const noexcept
    {
        const auto low = -extent_;
        if (!(value > low)) return 0;
        if (value >= low + kGridDimension * kGridCellSize) return kGridDimension - 1;
        return std::clamp(static_cast<int>(std::floor((value + extent_) / kGridCellSize)),
                          0, kGridDimension - 1);
    }
    void RemoveFromBucket(Id id, Location location) noexcept
    {
        auto &bucket = buckets_[location.bucket];
        const auto moved = bucket.back();
        bucket[location.offset] = moved;
        // Bucket order is private broad-phase scratch; only the merged work is canonical.
        if (moved != id) locations_.find(moved)->second.offset = location.offset;
        bucket.pop_back();
    }
    void Remove(Id id) noexcept
    {
        const auto found = locations_.find(id);
        if (found == locations_.end()) return;
        RemoveFromBucket(id, found->second);
        locations_.erase(found);
    }
    void Upsert(Id id, std::size_t bucket_index)
    {
        const auto found = locations_.find(id);
        if (found != locations_.end() && found->second.bucket == bucket_index) return;
        if (found != locations_.end()) RemoveFromBucket(id, found->second);
        auto &bucket = buckets_[bucket_index];
        const auto offset = bucket.size();
        bucket.push_back(id);
        if (found != locations_.end()) found->second = {bucket_index, offset};
        else locations_.emplace(id, Location{bucket_index, offset});
    }
    [[nodiscard]] std::size_t StationaryBucket(Float2 position) const noexcept
    {
        return static_cast<std::size_t>(Coordinate(position.y) * kGridDimension + Coordinate(position.x));
    }
    void Index(const PickupActor &pickup, bool moving)
    {
        if (pickup.dead) { needs_compaction_ = true; return; }
        if (pickup.kind != PickupKind::Experience) Upsert(pickup.id.value, kUtilityBucket);
        else if (pickup.globally_attracted || moving) Upsert(pickup.id.value, kActiveBucket);
        else Upsert(pickup.id.value, StationaryBucket(pickup.position));
    }
    void ClearIndices() noexcept
    {
        for (auto &bucket : buckets_) bucket.clear();
        locations_.clear();
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
    void Canonicalize()
    {
        std::ranges::sort(queued_ids_);
        queued_ids_.erase(std::unique(queued_ids_.begin(), queued_ids_.end()), queued_ids_.end());
    }
};

} // namespace hs::gameplay_detail
