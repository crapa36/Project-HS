#pragma once

#include <hs/core/fixed_step_clock.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/game_domain/game_types.hpp>
#include "vfx_typed_entity_surface_commands.hpp"
#include "vfx_typed_screen_commands.hpp"
#include "vfx_event_view.hpp"
#include <hs/render_data/render_frame_input.hpp>
#include <hs/render_data/vfx_spawn_command.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hs::runtime_detail
{
namespace vfx_retained_size
{
inline std::size_t Add(std::size_t a, std::size_t b) noexcept
{ return b > std::numeric_limits<std::size_t>::max() - a ? std::numeric_limits<std::size_t>::max() : a + b; }

inline std::size_t Multiply(std::size_t count, std::size_t size) noexcept
{ return count && size > std::numeric_limits<std::size_t>::max() / count
    ? std::numeric_limits<std::size_t>::max() : count * size; }

template<class T, class Allocator>
std::size_t Vector(const std::vector<T, Allocator> &items) noexcept
{ return Multiply(items.capacity(), sizeof(T)); }

template<class Map>
std::size_t HashTable(const Map &items) noexcept
{
    // Logical estimate: bucket pointers plus each live value and three node/link
    // pointer slots; this intentionally overcharges common one-link node layouts.
    return Add(Multiply(items.bucket_count(), sizeof(void *)),
        Multiply(items.size(), sizeof(typename Map::value_type) + 3 * sizeof(void *)));
}

inline std::size_t RingGapPayload(const VfxPayload &payload) noexcept
{
    const auto *gaps = std::get_if<VfxRingGapsPayload>(&payload);
    return gaps ? Vector(gaps->gap_angles_degrees) : 0;
}

inline std::size_t RingGapPayload(const VfxEventInput &input) noexcept
{ return RingGapPayload(input.payload); }
} // namespace vfx_retained_size

// A soft bound for optional CPU output, shared with the retained/queued workspace.
// Essential warnings keep their existing path; over-budget frames may reuse owned rows but cannot grow cosmetics.
// shortcut: only fire has bounded admission; extend to other cosmetics when their prepared fanout is available.
inline constexpr std::size_t kVfxWorkspaceSoftBudgetBytes = 64u * 1024 * 1024;

inline std::size_t AvailableVfxFlashAppendRows(std::size_t held_bytes,
    std::size_t budget_bytes, const std::vector<VfxFlashSpawnInput> &output) noexcept
{
    const auto reserved_rows = output.capacity() - output.size();
    if (held_bytes >= budget_bytes) return reserved_rows;
    return vfx_retained_size::Add(reserved_rows,
        (budget_bytes - held_bytes) / sizeof(VfxFlashSpawnInput));
}

struct RetainedVfxEntitySurfaceCapture
{
    std::uint32_t row{std::numeric_limits<std::uint32_t>::max()};
    RenderInstance instance;
    bool retained_instance{};
    bool has_animation_pose{};
    AnimationPoseRef animation_pose{};
    std::shared_ptr<const void> generation_lease;
};

class ActiveEffectStore
{
  public:
    enum class Profile : std::uint8_t
    {
        Ground, Ribbon, Light, Distortion, Decal, Volume, Screen,
        EntitySurface, PointRate, Count
    };
    using Row = std::uint32_t;

    struct AppendRequest
    {
        const VfxEventInput *input{};
        std::uint16_t profile_mask{};
        bool replace_ground_context_owner{};
    };
    struct PreparedAppendBatch;

    [[nodiscard]] Row Intern(const VfxEventInput &input)
    {
        const EventKey key = KeyFor(input);
        if (const auto found = event_rows_.find(key); found != event_rows_.end())
            return found->second;
        if (rows_.size() >= std::numeric_limits<Row>::max())
            throw std::length_error("too many retained VFX event rows");
        VfxEventInput staged(input);
        const auto row = static_cast<Row>(rows_.size());
        ReserveOne(rows_);
        ReserveOne(profile_masks_);
        ReserveOne(binding_indices_);
        static_assert(std::is_nothrow_move_constructible_v<VfxEventInput>);
        // Index insertion may allocate; all remaining publication steps cannot.
        event_rows_.try_emplace(key, row);
        rows_payload_capacity_bytes_ = vfx_retained_size::Add(
            rows_payload_capacity_bytes_, vfx_retained_size::RingGapPayload(staged));
        rows_.push_back(std::move(staged));
        profile_masks_.push_back(0);
        binding_indices_.push_back(VfxEventView::kNoBinding);
        ++unreferenced_rows_;
        return row;
    }

    [[nodiscard]] bool Add(Profile profile, Row row)
    {
        if (row >= rows_.size()) return false;
        const auto bit = ProfileBit(profile);
        if (profile_masks_[row] & bit) return false;
        auto &refs = profiles_[static_cast<std::size_t>(profile)];
        ReserveOne(refs);
        refs.push_back(row);
        if (!profile_masks_[row]) --unreferenced_rows_;
        profile_masks_[row] |= bit;
        return true;
    }

    void Append(Profile profile, const VfxEventInput &input)
    {
        const auto found = Find(input);
        if (found != std::numeric_limits<Row>::max())
        {
            (void)Add(profile, found);
            return;
        }
        auto &refs = profiles_[static_cast<std::size_t>(profile)];
        ReserveOne(refs);
        (void)Add(profile, Intern(input));
    }

    // Stages retained memberships and owned new rows, then reserves destination storage.
    // Existing membership remains visible exactly as before until CommitAppendBatch.
    // Capacity growth may invalidate borrowed rows/views; callers must not retain them.
    [[nodiscard]] bool PrepareAppendBatch(std::span<const AppendRequest> requests,
        PreparedAppendBatch &prepared);

    // No allocation follows the reserve phase. Prepared rows own their input snapshot;
    // only the store must remain unchanged until this call. Narrow row/index/
    // profile size checks catch append/erase changes without rescanning keys.
    // Same-size mutation is outside this private render-thread caller contract.
    [[nodiscard]] bool CommitAppendBatch(PreparedAppendBatch &prepared) noexcept;
    void CancelAppendBatch(PreparedAppendBatch &prepared) noexcept;

    [[nodiscard]] const VfxEventInput *Get(Row row) const noexcept
    { return row < rows_.size() ? &rows_[row] : nullptr; }
    [[nodiscard]] Row Find(const VfxEventInput &input) const noexcept
    {
        const auto found = event_rows_.find(KeyFor(input));
        return found == event_rows_.end() ? std::numeric_limits<Row>::max() : found->second;
    }

    [[nodiscard]] VfxEventView ProfileEvents(Profile profile) const noexcept
    {
        const auto &refs = profiles_[static_cast<std::size_t>(profile)];
        if (UsesOwnerBinding(profile))
            return {rows_, refs, std::span<const VfxEventInput>(bound_rows_).first(bound_row_count_), binding_indices_};
        return {rows_, refs};
    }

    // Only attached outputs share current owner geometry; birth/capture inputs stay immutable.
    void BeginFrameBindings() noexcept
    {
        for (const auto row : bound_row_ids_) binding_indices_[row] = VfxEventView::kNoBinding;
        bound_row_ids_.clear();
        bound_row_count_ = 0;
    }

    void RefreshProfile(Profile profile, auto &&refresh)
    {
        assert(UsesOwnerBinding(profile));
        auto &refs = profiles_[static_cast<std::size_t>(profile)];
        // Recording a completed binding must not fail after its row/count is published.
        ReserveTo(bound_row_ids_, rows_.size());
        std::erase_if(refs, [&](Row row) {
            if (rows_[row].geometry_owner_id == 0) return false;
            auto &binding = binding_indices_[row];
            if (binding == VfxEventView::kNoBinding)
            {
                const auto bound_index = bound_row_count_;
                const bool append_bound = bound_index == bound_rows_.size();
                const auto old_bytes = append_bound ? 0 :
                    vfx_retained_size::RingGapPayload(bound_rows_[bound_index]);
                bool keep{};
                try
                {
                    if (append_bound) bound_rows_.push_back(rows_[row]);
                    else bound_rows_[bound_index] = rows_[row];
                    keep = refresh(bound_rows_[bound_index]);
                }
                catch (...)
                {
                    if (append_bound ? bound_rows_.size() > bound_index : true)
                    {
                        bound_payload_capacity_bytes_ -= old_bytes;
                        bound_payload_capacity_bytes_ = vfx_retained_size::Add(
                            bound_payload_capacity_bytes_,
                            vfx_retained_size::RingGapPayload(bound_rows_[bound_index]));
                    }
                    throw;
                }
                bound_payload_capacity_bytes_ -= old_bytes;
                bound_payload_capacity_bytes_ = vfx_retained_size::Add(
                    bound_payload_capacity_bytes_,
                    vfx_retained_size::RingGapPayload(bound_rows_[bound_index]));
                if (keep) binding = static_cast<Row>(bound_row_count_++);
                else binding = kDroppedBinding;
                bound_row_ids_.push_back(row);
            }
            if (binding != kDroppedBinding) return false;
            RemoveMembership(profile, row);
            return true;
        });
    }

    void EraseIf(Profile profile, auto &&expired)
    {
        auto &refs = profiles_[static_cast<std::size_t>(profile)];
        std::erase_if(refs, [&](Row row) {
            const auto binding = binding_indices_[row];
            const auto &input = UsesOwnerBinding(profile) && binding < bound_row_count_
                ? bound_rows_[binding] : rows_[row];
            if (!expired(input)) return false;
            RemoveMembership(profile, row);
            return true;
        });
    }

    void CopyProfileForRebind(Profile profile, std::vector<VfxEventInput> &scratch) const
    {
        const auto &refs = profiles_[static_cast<std::size_t>(profile)];
        scratch.clear();
        scratch.reserve(refs.size());
        for (const auto row : refs) scratch.push_back(rows_[row]);
    }

    void RebindRows(auto &&rebind, std::vector<RetainedVfxEntitySurfaceCapture> &captures)
    {
        std::vector<std::uint8_t> live(rows_.size(), 1);
        for (std::size_t i = 0; i < rows_.size(); ++i)
        {
            const auto old_bytes = vfx_retained_size::RingGapPayload(rows_[i]);
            try
            {
                if (!rebind(rows_[i])) live[i] = 0;
            }
            catch (...)
            {
                rows_payload_capacity_bytes_ -= old_bytes;
                rows_payload_capacity_bytes_ = vfx_retained_size::Add(
                    rows_payload_capacity_bytes_, vfx_retained_size::RingGapPayload(rows_[i]));
                throw;
            }
            rows_payload_capacity_bytes_ -= old_bytes;
            rows_payload_capacity_bytes_ = vfx_retained_size::Add(
                rows_payload_capacity_bytes_, vfx_retained_size::RingGapPayload(rows_[i]));
        }
        for (auto &profile : profiles_)
            std::erase_if(profile, [&](Row row) { return row >= live.size() || !live[row]; });
        for (std::size_t i = 0; i < rows_.size(); ++i)
            if (!live[i] && profile_masks_[i])
            {
                profile_masks_[i] = 0;
                ++unreferenced_rows_;
            }
        std::erase_if(captures, [&](const auto &capture) {
            return capture.row >= live.size() || !live[capture.row];
        });
        index_dirty_ = true;
        BeginFrameBindings();
        Compact(captures);
    }

    void RebindProfile(Profile profile, std::span<const VfxEventInput> rebound)
    {
        auto &refs = profiles_[static_cast<std::size_t>(profile)];
        std::vector<Row> surviving;
        surviving.reserve(rebound.size());
        const auto bit = ProfileBit(profile);
        for (const auto row : refs) RemoveMembership(profile, row);
        for (const auto &input : rebound)
            for (const auto row : refs)
                if (SameEpisode(rows_[row], input))
                {
                    const auto old_bytes = vfx_retained_size::RingGapPayload(rows_[row]);
                    try { rows_[row] = input; }
                    catch (...)
                    {
                        rows_payload_capacity_bytes_ -= old_bytes;
                        rows_payload_capacity_bytes_ = vfx_retained_size::Add(
                            rows_payload_capacity_bytes_, vfx_retained_size::RingGapPayload(rows_[row]));
                        throw;
                    }
                    rows_payload_capacity_bytes_ -= old_bytes;
                    rows_payload_capacity_bytes_ = vfx_retained_size::Add(
                        rows_payload_capacity_bytes_, vfx_retained_size::RingGapPayload(rows_[row]));
                    if (!(profile_masks_[row] & bit))
                    {
                        surviving.push_back(row);
                        if (!profile_masks_[row]) --unreferenced_rows_;
                        profile_masks_[row] |= bit;
                    }
                    break;
                }
        refs.swap(surviving);
        BeginFrameBindings();
        RebuildIndex();
    }

    void Compact(std::span<RetainedVfxEntitySurfaceCapture> captures = {})
    {
        if (!unreferenced_rows_)
        {
            if (index_dirty_) RebuildIndex();
            return;
        }
        live_rows_.resize(rows_.size());
        for (std::size_t i = 0; i < rows_.size(); ++i) live_rows_[i] = profile_masks_[i] != 0;
        for (const auto &capture : captures)
            if (capture.row < live_rows_.size())
            {
                live_rows_[capture.row] = 1;
            }
        if (std::ranges::all_of(live_rows_, [](auto live) { return live != 0; }))
        {
            if (index_dirty_) RebuildIndex();
            return;
        }
        BeginFrameBindings();
        scratch_rows_.assign(rows_.size(), std::numeric_limits<Row>::max());
        Row next{};
        for (std::size_t i = 0; i < rows_.size(); ++i)
        {
            const auto key = KeyFor(rows_[i]);
            if (live_rows_[i])
            {
                scratch_rows_[i] = next;
                if (next != i)
                {
                    const auto old_destination = vfx_retained_size::RingGapPayload(rows_[next]);
                    const auto old_source = vfx_retained_size::RingGapPayload(rows_[i]);
                    rows_[next] = std::move(rows_[i]);
                    rows_payload_capacity_bytes_ -= old_destination;
                    rows_payload_capacity_bytes_ -= old_source;
                    rows_payload_capacity_bytes_ = vfx_retained_size::Add(
                        rows_payload_capacity_bytes_,
                        vfx_retained_size::Add(vfx_retained_size::RingGapPayload(rows_[next]),
                            vfx_retained_size::RingGapPayload(rows_[i])));
                    profile_masks_[next] = profile_masks_[i];
                    if (!index_dirty_) event_rows_.at(key) = next;
                }
                ++next;
            }
            else if (!index_dirty_) event_rows_.erase(key);
        }
        for (auto &profile : profiles_) for (auto &row : profile) row = scratch_rows_[row];
        for (auto &capture : captures) capture.row = scratch_rows_[capture.row];
        unreferenced_rows_ -= rows_.size() - next;
        std::size_t removed_payload_bytes{};
        for (std::size_t i = next; i < rows_.size(); ++i)
            removed_payload_bytes = vfx_retained_size::Add(
                removed_payload_bytes, vfx_retained_size::RingGapPayload(rows_[i]));
        rows_payload_capacity_bytes_ -= removed_payload_bytes;
        rows_.resize(next);
        profile_masks_.resize(next);
        binding_indices_.resize(next);
        if (index_dirty_) RebuildIndex();
    }

    void RebuildIndex()
    {
        event_rows_.clear();
        event_rows_.reserve(rows_.size());
        for (std::size_t i = 0; i < rows_.size(); ++i)
            event_rows_.try_emplace(KeyFor(rows_[i]), static_cast<Row>(i));
        index_dirty_ = false;
    }

    void Clear() noexcept
    {
        rows_.clear(); profile_masks_.clear(); event_rows_.clear();
        binding_indices_.clear(); bound_rows_.clear(); bound_row_ids_.clear();
        rows_payload_capacity_bytes_ = bound_payload_capacity_bytes_ = 0;
        bound_row_count_ = 0;
        unreferenced_rows_ = 0;
        scratch_rows_.clear(); live_rows_.clear();
        for (auto &profile : profiles_) profile.clear();
        index_dirty_ = false;
    }
    void Swap(ActiveEffectStore &other) noexcept
    {
        rows_.swap(other.rows_); event_rows_.swap(other.event_rows_);
        profiles_.swap(other.profiles_);
        profile_masks_.swap(other.profile_masks_);
        binding_indices_.swap(other.binding_indices_);
        bound_rows_.swap(other.bound_rows_); bound_row_ids_.swap(other.bound_row_ids_);
        std::swap(bound_row_count_, other.bound_row_count_);
        std::swap(unreferenced_rows_, other.unreferenced_rows_);
        std::swap(rows_payload_capacity_bytes_, other.rows_payload_capacity_bytes_);
        std::swap(bound_payload_capacity_bytes_, other.bound_payload_capacity_bytes_);
        scratch_rows_.swap(other.scratch_rows_); live_rows_.swap(other.live_rows_);
        std::swap(index_dirty_, other.index_dirty_);
    }
    [[nodiscard]] std::size_t Size() const noexcept { return rows_.size(); }
    [[nodiscard]] std::size_t LogicalCapacityBytes() const noexcept
    {
        using namespace vfx_retained_size;
        auto bytes = Add(Vector(rows_), Add(Vector(profile_masks_), Vector(binding_indices_)));
        bytes = Add(bytes, Vector(bound_rows_));
        bytes = Add(bytes, rows_payload_capacity_bytes_);
        bytes = Add(bytes, bound_payload_capacity_bytes_);
        bytes = Add(bytes, Add(Vector(bound_row_ids_), Add(Vector(scratch_rows_), Vector(live_rows_))));
        for (const auto &profile : profiles_) bytes = Add(bytes, Vector(profile));
        return Add(bytes, HashTable(event_rows_));
    }
    [[nodiscard]] std::size_t OwnedPayloadCapacityBytes() const noexcept
    { return vfx_retained_size::Add(rows_payload_capacity_bytes_, bound_payload_capacity_bytes_); }

  private:
    static std::size_t CheckedAdd(std::size_t value, std::size_t increment)
    {
        if (increment > std::numeric_limits<std::size_t>::max() - value)
            throw std::length_error("retained VFX capacity overflow");
        return value + increment;
    }
    template<class T, class Allocator>
    static void ReserveOne(std::vector<T, Allocator> &items)
    {
        if (items.size() < items.capacity()) return;
        const auto needed = CheckedAdd(items.size(), 1);
        const auto growth = items.capacity() == 0 ? std::size_t{8} :
            items.capacity() + std::min(items.capacity() / 2 + 1,
                items.max_size() - items.capacity());
        items.reserve(std::max(needed, std::min(growth, items.max_size())));
    }
    template<class T, class Allocator>
    static void ReserveTo(std::vector<T, Allocator> &items, std::size_t needed)
    {
        if (needed <= items.capacity()) return;
        if (needed > items.max_size()) throw std::length_error("retained VFX capacity overflow");
        const auto growth = items.capacity() == 0 ? std::size_t{8} :
            items.capacity() + std::min(items.capacity() / 2 + 1,
                items.max_size() - items.capacity());
        items.reserve(std::max(needed, std::min(growth, items.max_size())));
    }
    template<class Map>
    static void ReserveMapTo(Map &map, std::size_t needed)
    {
        const auto scaled_capacity = static_cast<long double>(map.bucket_count()) * map.max_load_factor();
        const auto capacity = scaled_capacity >= static_cast<long double>(map.max_size())
            ? map.max_size() : static_cast<std::size_t>(scaled_capacity);
        if (needed <= capacity) return;
        if (needed > map.max_size()) throw std::length_error("retained VFX index overflow");
        const auto growth = capacity + std::min(capacity / 2 + 1, map.max_size() - capacity);
        map.reserve(std::max(needed, growth));
    }
    struct EventKey
    {
        std::uint64_t sequence{}, episode{}, owner{}, geometry_effect{};
        std::uint64_t event_tick{};
        std::uint32_t effect_handle{};
        std::uint32_t geometry_sector_index{};
        bool operator==(const EventKey &) const noexcept = default;
    };
    struct EventKeyHash
    {
        std::size_t operator()(const EventKey &key) const noexcept
        {
            auto hash = std::hash<std::uint64_t>{}(key.sequence);
            const auto mix = [&](std::uint64_t value) {
                hash ^= std::hash<std::uint64_t>{}(value) + 0x9e3779b97f4a7c15ull +
                    (hash << 6) + (hash >> 2);
            };
            mix(key.episode); mix(key.owner); mix(key.geometry_effect);
            mix(key.event_tick); mix(key.effect_handle); mix(key.geometry_sector_index);
            return hash;
        }
    };
    static constexpr Row kDroppedBinding = VfxEventView::kNoBinding - 1;
    static constexpr std::uint16_t ProfileBit(Profile profile) noexcept
    { return static_cast<std::uint16_t>(1u << static_cast<unsigned>(profile)); }
    static constexpr bool UsesOwnerBinding(Profile profile) noexcept
    {
        return profile == Profile::Ground || profile == Profile::Light ||
            profile == Profile::Distortion || profile == Profile::Decal;
    }
    void RemoveMembership(Profile profile, Row row) noexcept
    {
        const auto prior = profile_masks_[row];
        profile_masks_[row] &= ~ProfileBit(profile);
        if (prior && !profile_masks_[row]) ++unreferenced_rows_;
    }
    static EventKey KeyFor(const VfxEventInput &input) noexcept
    {
        return {input.sequence, input.status_episode_generation,
            input.geometry_owner_id, input.geometry_effect_id, input.event_tick,
            input.effect_handle, input.geometry_sector_index};
    }
    static bool SameEpisode(const VfxEventInput &a, const VfxEventInput &b) noexcept
    {
        return a.sequence == b.sequence && a.status_episode_generation == b.status_episode_generation &&
            a.geometry_owner_id == b.geometry_owner_id && a.geometry_effect_id == b.geometry_effect_id &&
            a.event_tick == b.event_tick && a.geometry_sector_index == b.geometry_sector_index;
    }
    std::vector<VfxEventInput> rows_;
    std::vector<std::uint16_t> profile_masks_;
    std::unordered_map<EventKey, Row, EventKeyHash> event_rows_;
    std::array<std::vector<Row>, static_cast<std::size_t>(Profile::Count)> profiles_;
    std::vector<Row> scratch_rows_;
    std::vector<std::uint8_t> live_rows_;
    std::vector<Row> binding_indices_, bound_row_ids_;
    std::vector<VfxEventInput> bound_rows_;
    std::size_t bound_row_count_{}, unreferenced_rows_{};
    std::size_t rows_payload_capacity_bytes_{}, bound_payload_capacity_bytes_{};
    bool index_dirty_{};
};

struct ActiveEffectStore::PreparedAppendBatch
{
  public:
    PreparedAppendBatch() = default;

  private:
    friend class ActiveEffectStore;
    struct PendingRow
    {
        Row row{};
        std::uint16_t initial_mask{}, added_mask{};
        bool is_new{};
    };
    using GroundOwnerKey = std::pair<std::uint32_t, std::uint64_t>;
    struct GroundReplacement
    {
        GroundOwnerKey key;
        std::size_t last_request{};
    };

    void Cancel() noexcept
    {
        new_rows_.clear();
        new_masks_.clear();
        pending_rows_.clear();
        pending_slots_.clear();
        new_index_.clear();
        ground_replacements_.clear();
        removed_ground_rows_.clear();
        new_rows_payload_capacity_bytes_ = 0;
        for (auto &profile : profile_additions_) profile.clear();
        base_profile_sizes_.fill(0);
        base_rows_ = base_index_size_ = 0;
        owner_ = nullptr;
        ready_ = false;
    }

    std::vector<VfxEventInput> new_rows_;
    std::vector<std::uint16_t> new_masks_;
    std::vector<PendingRow> pending_rows_;
    std::vector<std::size_t> pending_slots_;
    // These nodes have the exact key/value/allocator type of event_rows_ and
    // can move into it after its buckets have been reserved.
    std::unordered_map<EventKey, Row, EventKeyHash> new_index_;
    std::vector<GroundReplacement> ground_replacements_;
    std::vector<Row> removed_ground_rows_;
    std::array<std::vector<Row>, static_cast<std::size_t>(Profile::Count)> profile_additions_;
    std::array<std::size_t, static_cast<std::size_t>(Profile::Count)> base_profile_sizes_{};
    std::size_t base_rows_{}, base_index_size_{};
    std::size_t new_rows_payload_capacity_bytes_{};
    const ActiveEffectStore *owner_{};
    bool ready_{};

  public:
    [[nodiscard]] std::size_t LogicalCapacityBytes() const noexcept
    {
        using namespace vfx_retained_size;
        auto bytes = Add(Vector(new_rows_), Add(Vector(new_masks_), Vector(pending_rows_)));
        bytes = Add(bytes, new_rows_payload_capacity_bytes_);
        bytes = Add(bytes, Vector(pending_slots_));
        bytes = Add(bytes, HashTable(new_index_));
        bytes = Add(bytes, Add(Vector(ground_replacements_), Vector(removed_ground_rows_)));
        for (const auto &profile : profile_additions_) bytes = Add(bytes, Vector(profile));
        return bytes;
    }
    [[nodiscard]] std::size_t OwnedPayloadCapacityBytes() const noexcept
    { return new_rows_payload_capacity_bytes_; }
};

inline bool ActiveEffectStore::PrepareAppendBatch(std::span<const AppendRequest> requests,
    PreparedAppendBatch &prepared)
{
    prepared.Cancel();
    constexpr auto profile_count = static_cast<unsigned>(Profile::Count);
    constexpr auto valid_mask = static_cast<std::uint16_t>((1u << profile_count) - 1u);
    constexpr auto ground_bit = ProfileBit(Profile::Ground);
    std::size_t replacement_count{};
    std::array<std::size_t, static_cast<std::size_t>(Profile::Count)> request_profile_counts{};
    for (const auto &request : requests)
    {
        if (!request.input || (request.profile_mask & static_cast<std::uint16_t>(~valid_mask)))
            return false;
        if (request.replace_ground_context_owner)
        {
            if (!(request.profile_mask & ground_bit) ||
                !std::holds_alternative<VfxContextPayload>(request.input->payload)) return false;
            ++replacement_count;
        }
        for (unsigned profile = 0; profile < profile_count; ++profile)
            if (request.profile_mask & static_cast<std::uint16_t>(1u << profile))
                request_profile_counts[profile] = CheckedAdd(request_profile_counts[profile], 1);
    }
    prepared.base_rows_ = rows_.size();
    prepared.base_index_size_ = event_rows_.size();
    prepared.owner_ = this;
    for (std::size_t i = 0; i < profiles_.size(); ++i)
        prepared.base_profile_sizes_[i] = profiles_[i].size();

    ReserveTo(prepared.ground_replacements_, replacement_count);
    for (std::size_t i = 0; i < requests.size(); ++i)
        if (requests[i].replace_ground_context_owner)
            prepared.ground_replacements_.push_back({{requests[i].input->effect_handle,
                std::get<VfxContextPayload>(requests[i].input->payload).owner_id}, i});
    std::ranges::sort(prepared.ground_replacements_, [](const auto &a, const auto &b) {
        return a.key != b.key ? a.key < b.key : a.last_request < b.last_request;
    });
    std::size_t unique_replacements{};
    for (const auto &replacement : prepared.ground_replacements_)
    {
        if (unique_replacements && prepared.ground_replacements_[unique_replacements - 1].key == replacement.key)
            prepared.ground_replacements_[unique_replacements - 1].last_request = replacement.last_request;
        else prepared.ground_replacements_[unique_replacements++] = replacement;
    }
    prepared.ground_replacements_.resize(unique_replacements);
    const auto replacement_for = [&](const VfxEventInput &input) -> const PreparedAppendBatch::GroundReplacement * {
        const auto *context = std::get_if<VfxContextPayload>(&input.payload);
        if (!context || prepared.ground_replacements_.empty()) return nullptr;
        const PreparedAppendBatch::GroundOwnerKey key{input.effect_handle, context->owner_id};
        const auto found = std::ranges::lower_bound(prepared.ground_replacements_, key,
            {}, &PreparedAppendBatch::GroundReplacement::key);
        return found != prepared.ground_replacements_.end() && found->key == key
            ? std::addressof(*found) : nullptr;
    };
    if (!prepared.ground_replacements_.empty())
    {
        const auto &ground = profiles_[static_cast<std::size_t>(Profile::Ground)];
        ReserveTo(prepared.removed_ground_rows_, ground.size());
        for (const auto row : ground)
            if (replacement_for(rows_[row])) prepared.removed_ground_rows_.push_back(row);
    }

    ReserveTo(prepared.new_rows_, requests.size());
    ReserveTo(prepared.new_masks_, requests.size());
    ReserveTo(prepared.pending_rows_, requests.size());
    constexpr auto no_pending = std::numeric_limits<std::size_t>::max();
    const auto slot_count = CheckedAdd(requests.size(), requests.size());
    ReserveTo(prepared.pending_slots_, slot_count);
    prepared.pending_slots_.assign(slot_count, no_pending);
    ReserveMapTo(prepared.new_index_, requests.size());
    for (std::size_t i = 0; i < prepared.profile_additions_.size(); ++i)
        ReserveTo(prepared.profile_additions_[i], request_profile_counts[i]);

    for (std::size_t request_index = 0; request_index < requests.size(); ++request_index)
    {
        const auto &request = requests[request_index];
        auto mask = request.profile_mask;
        if (mask & ground_bit)
            if (const auto *replacement = replacement_for(*request.input);
                replacement && request_index < replacement->last_request)
                mask &= static_cast<std::uint16_t>(~ground_bit);
        if (!mask) continue;

        const auto key = KeyFor(*request.input);
        auto slot = EventKeyHash{}(key) % slot_count;
        // At most one pending row per request: half-full slots always reach an empty entry.
        while (prepared.pending_slots_[slot] != no_pending)
        {
            const auto &pending = prepared.pending_rows_[prepared.pending_slots_[slot]];
            const auto &input = pending.is_new
                ? prepared.new_rows_[pending.row - static_cast<Row>(rows_.size())] : rows_[pending.row];
            if (KeyFor(input) == key) break;
            slot = slot + 1 == slot_count ? 0 : slot + 1;
        }
        auto pending_index = prepared.pending_slots_[slot];
        if (pending_index == no_pending)
        {
            const auto existing = event_rows_.find(key);
            if (existing != event_rows_.end())
            {
                pending_index = prepared.pending_rows_.size();
                auto initial_mask = profile_masks_[existing->second];
                if ((initial_mask & ground_bit) && replacement_for(rows_[existing->second]))
                    initial_mask &= static_cast<std::uint16_t>(~ground_bit);
                prepared.pending_rows_.push_back({existing->second, initial_mask, 0, false});
            }
            else
            {
                const auto row_index = CheckedAdd(rows_.size(), prepared.new_rows_.size());
                if (row_index >= std::numeric_limits<Row>::max())
                    throw std::length_error("too many retained VFX event rows");
                const auto row = static_cast<Row>(row_index);
                pending_index = prepared.pending_rows_.size();
                prepared.pending_rows_.push_back({row, 0, 0, true});
                prepared.new_rows_.push_back(*request.input);
                prepared.new_rows_payload_capacity_bytes_ = vfx_retained_size::Add(
                    prepared.new_rows_payload_capacity_bytes_,
                    vfx_retained_size::RingGapPayload(prepared.new_rows_.back()));
                prepared.new_masks_.push_back(0);
                prepared.new_index_.try_emplace(key, row);
            }
            prepared.pending_slots_[slot] = pending_index;
        }

        auto &pending_row = prepared.pending_rows_[pending_index];
        for (unsigned profile = 0; profile < profile_count; ++profile)
        {
            const auto bit = static_cast<std::uint16_t>(1u << profile);
            if (!(mask & bit) || ((pending_row.initial_mask | pending_row.added_mask) & bit))
                continue;
            pending_row.added_mask |= bit;
            prepared.profile_additions_[profile].push_back(pending_row.row);
        }
        if (pending_row.is_new)
            prepared.new_masks_[pending_row.row - static_cast<Row>(rows_.size())] = pending_row.added_mask;
    }

    const auto needed_rows = CheckedAdd(rows_.size(), prepared.new_rows_.size());
    if (needed_rows > std::numeric_limits<Row>::max())
        throw std::length_error("too many retained VFX event rows");
    ReserveTo(rows_, needed_rows);
    ReserveTo(profile_masks_, needed_rows);
    ReserveTo(binding_indices_, needed_rows);
    for (std::size_t i = 0; i < profiles_.size(); ++i)
    {
        const auto retained = profiles_[i].size() - (i == static_cast<std::size_t>(Profile::Ground)
            ? prepared.removed_ground_rows_.size() : 0);
        ReserveTo(profiles_[i], CheckedAdd(retained, prepared.profile_additions_[i].size()));
    }
    ReserveMapTo(event_rows_, needed_rows);
    prepared.ready_ = true;
    return true;
}

inline bool ActiveEffectStore::CommitAppendBatch(PreparedAppendBatch &prepared) noexcept
{
    if (!prepared.ready_ || prepared.owner_ != this || rows_.size() != prepared.base_rows_ ||
        event_rows_.size() != prepared.base_index_size_)
        return false;
    for (std::size_t i = 0; i < profiles_.size(); ++i)
        if (profiles_[i].size() != prepared.base_profile_sizes_[i]) return false;

    static_assert(std::is_nothrow_move_constructible_v<VfxEventInput>);
    static_assert(std::is_nothrow_invocable_v<EventKeyHash, const EventKey &>);
    static_assert(noexcept(std::declval<const EventKey &>() == std::declval<const EventKey &>()));
    if (!prepared.removed_ground_rows_.empty())
    {
        for (const auto row : prepared.removed_ground_rows_) RemoveMembership(Profile::Ground, row);
        std::erase_if(profiles_[static_cast<std::size_t>(Profile::Ground)], [&](Row row) {
            return !(profile_masks_[row] & ProfileBit(Profile::Ground));
        });
    }
    while (!prepared.new_index_.empty())
    {
        auto node = prepared.new_index_.extract(prepared.new_index_.begin());
        const auto inserted = event_rows_.insert(std::move(node));
        assert(inserted.inserted);
        (void)inserted;
    }
    for (std::size_t i = 0; i < prepared.new_rows_.size(); ++i)
    {
        rows_payload_capacity_bytes_ = vfx_retained_size::Add(
            rows_payload_capacity_bytes_,
            vfx_retained_size::RingGapPayload(prepared.new_rows_[i]));
        rows_.push_back(std::move(prepared.new_rows_[i]));
        profile_masks_.push_back(prepared.new_masks_[i]);
        binding_indices_.push_back(VfxEventView::kNoBinding);
    }
    for (const auto &row : prepared.pending_rows_)
    {
        if (row.is_new || !row.added_mask) continue;
        if (!row.initial_mask) --unreferenced_rows_;
        profile_masks_[row.row] = static_cast<std::uint16_t>(row.initial_mask | row.added_mask);
    }
    for (std::size_t i = 0; i < profiles_.size(); ++i)
        for (const auto row : prepared.profile_additions_[i]) profiles_[i].push_back(row);
    prepared.Cancel();
    return true;
}

inline void ActiveEffectStore::CancelAppendBatch(PreparedAppendBatch &prepared) noexcept
{
    prepared.Cancel();
}

struct DecodedVfxState
{
    struct BirthPrefix
    {
        std::size_t particles{}, lines{}, flashes{};
    };

    std::vector<ParticleSpawnCommand> particle_spawns;
    std::vector<VfxLineSpawnCommand> effect_lines;
    std::vector<VfxFlashSpawnInput> flashes;
    std::vector<RetainedVfxEntitySurfaceCapture> entity_surface_captures;

    [[nodiscard]] BirthPrefix MarkBirths() const noexcept
    { return {particle_spawns.size(), effect_lines.size(), flashes.size()}; }

    void RollbackBirths(BirthPrefix prefix) noexcept
    {
        assert(prefix.particles <= particle_spawns.size() &&
               prefix.lines <= effect_lines.size() && prefix.flashes <= flashes.size());
        particle_spawns.resize(prefix.particles);
        effect_lines.resize(prefix.lines);
        flashes.resize(prefix.flashes);
    }

    void SortNewParticleSpawns(std::size_t retained_count)
    {
        assert(retained_count <= particle_spawns.size());
        if (retained_count == particle_spawns.size()) return;
        const auto appended = particle_spawns.begin() + retained_count;
        const auto due = [](const auto &a, const auto &b) { return a.tick < b.tick; };
        std::stable_sort(appended, particle_spawns.end(), due);
        if (retained_count && due(*appended, *(appended - 1)))
            std::inplace_merge(particle_spawns.begin(), appended, particle_spawns.end(), due);
    }

    // Member swaps preserve the original allocation-free publication operation,
    // including implementations with debug iterator bookkeeping on moves.
    void Swap(DecodedVfxState &prepared) noexcept
    {
        particle_spawns.swap(prepared.particle_spawns);
        effect_lines.swap(prepared.effect_lines);
        flashes.swap(prepared.flashes);
        entity_surface_captures.swap(prepared.entity_surface_captures);
    }

    void Clear() noexcept
    {
        particle_spawns.clear();
        effect_lines.clear();
        flashes.clear();
        entity_surface_captures.clear();
    }

    [[nodiscard]] std::size_t LogicalCapacityBytes() const noexcept
    {
        using namespace vfx_retained_size;
        return Add(Vector(particle_spawns), Add(Vector(effect_lines),
            Add(Vector(flashes), Vector(entity_surface_captures))));
    }
};

struct TerminalPresentationClock
{
    Tick source_tick{};
    SessionPhase phase{};
    FixedStepClock::Clock::time_point started_at{};
};

// Render-thread retention. Events can arrive before their matching snapshot;
// a session ID keeps a reset simulation clock from replaying the previous run.
struct VfxSessionState
{
    struct NativeEmissionBatch
    {
        RenderVfxWorldBatch identity;
        std::vector<ParticleSpawnCommand> particle_spawns;
        std::vector<VfxLineSpawnCommand> effect_lines;

        [[nodiscard]] std::size_t LogicalCapacityBytes() const noexcept
        { return vfx_retained_size::Add(vfx_retained_size::Vector(particle_spawns),
            vfx_retained_size::Vector(effect_lines)); }
    };

    // The single simulation producer publishes monotone session/tick order.
    struct RawEventScheduler
    {
        static constexpr std::size_t kCapacity = 4096;
        struct Entry
        {
            PresentationEvent event;
        };
        struct ReadyBatch
        {
            std::vector<Entry> entries;
        };

        RawEventScheduler() = default;
        RawEventScheduler(const RawEventScheduler &) = default;
        RawEventScheduler &operator=(const RawEventScheduler &) = default;
        RawEventScheduler(RawEventScheduler &&other) noexcept { Swap(other); }
        RawEventScheduler &operator=(RawEventScheduler &&other) noexcept
        { Swap(other); return *this; }

        [[nodiscard]] bool HasCapacity() const noexcept
        {
            return pending_count_ + in_flight.entries.size() < kCapacity;
        }

        [[nodiscard]] std::size_t PendingCount() const noexcept { return pending_count_; }
        [[nodiscard]] const Entry *PendingFront() const noexcept
        { return pending_count_ ? &pending_[pending_head_] : nullptr; }

        [[nodiscard]] bool Enqueue(PresentationEvent event)
        {
            if (!HasCapacity()) return false;
            // One bounded backing makes retained storage independent of deque block policy.
            if (pending_.empty()) pending_.resize(kCapacity);
            pending_[(pending_head_ + pending_count_) % kCapacity] = {std::move(event)};
            ++pending_count_;
            return true;
        }

        [[nodiscard]] const ReadyBatch &Prepare(Tick tick, std::uint64_t session)
        {
            if (!in_flight.entries.empty()) return in_flight;
            const auto ready = [&](const Entry &entry) {
                return entry.event.session_id < session ||
                    (entry.event.session_id == session && entry.event.tick <= tick);
            };
            if (!pending_count_ || !ready(pending_[pending_head_])) return in_flight;
            // Reserve before removing an eligible event; allocation failure retains the FIFO.
            if (pending_count_ > in_flight.entries.capacity())
            {
                const auto grown = in_flight.entries.capacity() + in_flight.entries.capacity() / 2 + 1;
                in_flight.entries.reserve(std::min(kCapacity, std::max(pending_count_, grown)));
            }
            static_assert(std::is_nothrow_move_constructible_v<Entry> &&
                std::is_nothrow_move_assignable_v<Entry>);
            while (pending_count_)
            {
                if (!ready(pending_[pending_head_])) break;
                auto entry = std::move(pending_[pending_head_]);
                PopPending();
                if (entry.event.session_id < session) continue;
                in_flight.entries.push_back(std::move(entry));
            }
            return in_flight;
        }

        // A failed CPU activation retains this batch; native submission owns its decoded queues.
        void AcknowledgeActivation() noexcept
        {
            in_flight.entries.clear();
        }

        void DiscardBeforeSession(std::uint64_t session)
        {
            while (pending_count_ && pending_[pending_head_].event.session_id < session)
                PopPending();
            std::erase_if(in_flight.entries, [session](const Entry &entry) {
                return entry.event.session_id < session;
            });
        }

        [[nodiscard]] std::size_t LogicalCapacityBytes() const noexcept
        {
            using namespace vfx_retained_size;
            return Add(Vector(in_flight.entries), Vector(pending_));
        }

        ReadyBatch in_flight;

      private:
        void Swap(RawEventScheduler &other) noexcept
        {
            pending_.swap(other.pending_);
            in_flight.entries.swap(other.in_flight.entries);
            std::swap(pending_head_, other.pending_head_);
            std::swap(pending_count_, other.pending_count_);
        }
        void PopPending() noexcept
        {
            pending_head_ = (pending_head_ + 1) % kCapacity;
            --pending_count_;
        }
        std::vector<Entry> pending_;
        std::size_t pending_head_{}, pending_count_{};
    };
    struct PersistentBurstKey
    {
        std::uint64_t stable_id{};
        std::uint8_t visual_kind{};
        bool operator==(const PersistentBurstKey &) const = default;
    };
    struct PersistentBurstHash
    {
        std::size_t operator()(const PersistentBurstKey &key) const noexcept
        {
            const auto mixed = key.stable_id ^
                (static_cast<std::uint64_t>(key.visual_kind) << 56);
            return static_cast<std::size_t>(mixed ^ (mixed >> 33));
        }
    };

    [[nodiscard]] static bool IsPersistentStatusBurstKind(std::uint8_t kind) noexcept
    {
        return kind == static_cast<std::uint8_t>(PersistentVfxKind::PlayerBowDraw) ||
            kind == static_cast<std::uint8_t>(PersistentVfxKind::ChargedFullReady) ||
            kind == static_cast<std::uint8_t>(PersistentVfxKind::EmpoweredReady) ||
            kind == static_cast<std::uint8_t>(PersistentVfxKind::MultishotRetarget) ||
            kind == static_cast<std::uint8_t>(PersistentVfxKind::RicochetBleedExtend);
    }

    [[nodiscard]] bool HasPersistentBurst(std::uint64_t stable_id, std::uint8_t visual_kind) const noexcept
    {
        return stable_id != 0 && seen_persistent_bursts.contains({stable_id, visual_kind});
    }

    // First source row owns each new episode; return accepted rows in source order.
    void SelectUnseenPersistentBurstOwners(std::span<const VfxPersistentInput> owners,
        std::vector<std::size_t> &indices) const
    {
        indices.clear();
        for (std::size_t index = 0; index < owners.size(); ++index)
        {
            const auto &owner = owners[index];
            if (owner.stable_id && IsPersistentStatusBurstKind(owner.source_visual_kind) &&
                !HasPersistentBurst(owner.stable_id, owner.source_visual_kind))
                indices.push_back(index);
        }
        std::ranges::sort(indices, [&](std::size_t a, std::size_t b) {
            const auto &left = owners[a];
            const auto &right = owners[b];
            if (left.stable_id != right.stable_id) return left.stable_id < right.stable_id;
            if (left.source_visual_kind != right.source_visual_kind)
                return left.source_visual_kind < right.source_visual_kind;
            return a < b;
        });
        const auto duplicates = std::unique(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
            return owners[a].stable_id == owners[b].stable_id &&
                owners[a].source_visual_kind == owners[b].source_visual_kind;
        });
        indices.erase(duplicates, indices.end());
        std::ranges::sort(indices);
    }

    // Query the already prepared current/previous source owners, independently of typed quality.
    void RetireUnownedPersistentBursts(bool has_current_snapshot, Tick current_tick,
        auto &&has_live_owner) noexcept
    {
        if (!has_current_snapshot) return;
        if (has_status_burst_tick && current_tick < last_status_burst_tick)
        {
            std::erase_if(seen_persistent_bursts, [](const auto &key) {
                return IsPersistentStatusBurstKind(key.visual_kind);
            });
            last_status_burst_tick = current_tick;
            return;
        }
        last_status_burst_tick = current_tick;
        has_status_burst_tick = true;
        std::erase_if(seen_persistent_bursts, [&](const auto &key) {
            if (!IsPersistentStatusBurstKind(key.visual_kind)) return false;
            return !has_live_owner(key);
        });
    }

    // The staged command vector must be independent of destination. All allocating work
    // completes before tombstones or retained commands are published.
    [[nodiscard]] bool CommitPersistentBursts(std::span<const PersistentBurstKey> keys,
        const std::vector<ParticleSpawnCommand> &commands, std::vector<ParticleSpawnCommand> &destination)
    {
        if (std::addressof(commands) == std::addressof(destination)) return false;
        if (keys.empty()) return commands.empty();
        std::unordered_set<PersistentBurstKey, PersistentBurstHash> staged;
        staged.reserve(keys.size());
        for (const auto &key : keys)
        {
            if (!key.stable_id || seen_persistent_bursts.contains(key) ||
                !staged.insert(key).second)
                return false;
        }
        const auto needed_keys = CheckedAdd(seen_persistent_bursts.size(), staged.size());
        const auto needed_commands = CheckedAdd(destination.size(), commands.size());
        seen_persistent_bursts.reserve(needed_keys);
        ReserveTo(destination, needed_commands);
        static_assert(std::is_nothrow_copy_constructible_v<ParticleSpawnCommand>);
        while (!staged.empty())
            seen_persistent_bursts.insert(staged.extract(staged.begin()));
        for (const auto &command : commands) destination.push_back(command);
        return true;
    }

    // Tracked owned capacity for this state slice, not a complete activation
    // budget: call-local staging and external
    // RenderInstance/content allocations have separate or transient owners.
    [[nodiscard]] std::size_t TrackedLogicalCapacityBytes() const noexcept
    {
        using namespace vfx_retained_size;
        return Add(active_effects.LogicalCapacityBytes(),
            Add(decoded.LogicalCapacityBytes(),
            Add(native_batch.LogicalCapacityBytes(),
            Add(raw_events.LogicalCapacityBytes(), HashTable(seen_persistent_bursts)))));
    }

    std::uint64_t session_id{};
    VfxLowHealthPulseState low_health_pulse;
    std::optional<TerminalPresentationClock> terminal_clock;
    RawEventScheduler raw_events;
    ActiveEffectStore active_effects;
    DecodedVfxState decoded;
    std::unordered_set<PersistentBurstKey, PersistentBurstHash> seen_persistent_bursts;
    NativeEmissionBatch native_batch;
    bool native_batch_active{};
    Tick last_committed_stateful_target{};
    std::uint64_t next_native_batch_sequence{1};
    bool has_committed_stateful_target{};
    bool particle_reset_pending{};
    Tick last_status_burst_tick{};
    bool has_status_burst_tick{};

  private:
    static std::size_t CheckedAdd(std::size_t value, std::size_t increment)
    {
        if (increment > std::numeric_limits<std::size_t>::max() - value)
            throw std::length_error("retained VFX capacity overflow");
        return value + increment;
    }
    template<class T, class Allocator>
    static void ReserveTo(std::vector<T, Allocator> &items, std::size_t needed)
    {
        if (needed <= items.capacity()) return;
        const auto growth = items.capacity() == 0 ? std::size_t{8} :
            items.capacity() + std::min(items.capacity() / 2 + 1,
                items.max_size() - items.capacity());
        items.reserve(std::max(needed, std::min(growth, items.max_size())));
    }

  public:

    void DiscardNativeBatch() noexcept
    {
        native_batch.particle_spawns.clear();
        native_batch.effect_lines.clear();
        native_batch_active = false;
    }

    // Freeze due prefixes once. A retry returns the same owned vectors and identity;
    // later decoded output stays pending for the next batch.
    NativeEmissionBatch *PrepareNativeBatch(Tick target, bool stateful_step_needed)
    {
        const bool rewound = has_committed_stateful_target && !particle_reset_pending &&
            target < last_committed_stateful_target;
        if (rewound || (native_batch_active && target < native_batch.identity.stateful_target))
        {
            DiscardNativeBatch();
            particle_reset_pending = true;
        }
        if (native_batch_active) return &native_batch;

        std::size_t particle_count{};
        while (particle_count < decoded.particle_spawns.size() &&
               decoded.particle_spawns[particle_count].tick <= target)
            ++particle_count;
        std::size_t line_count{};
        while (line_count < decoded.effect_lines.size() && decoded.effect_lines[line_count].tick <= target)
            ++line_count;
        if (!particle_count && !line_count && !stateful_step_needed && !particle_reset_pending)
            return nullptr;

        native_batch.particle_spawns.clear();
        native_batch.effect_lines.clear();
        if (particle_count < decoded.particle_spawns.size()) native_batch.particle_spawns.reserve(particle_count);
        if (line_count < decoded.effect_lines.size()) native_batch.effect_lines.reserve(line_count);
        native_batch.identity = {session_id, target, next_native_batch_sequence++, particle_reset_pending};
        const auto freeze = [](auto &owned, auto &pending, std::size_t count) {
            if (!count) return;
            if (count == pending.size())
            {
                owned.swap(pending);
                return;
            }
            for (std::size_t i = 0; i < count; ++i) owned.push_back(std::move(pending[i]));
            pending.erase(pending.begin(), pending.begin() + count);
        };
        freeze(native_batch.particle_spawns, decoded.particle_spawns, particle_count);
        freeze(native_batch.effect_lines, decoded.effect_lines, line_count);
        native_batch_active = true;
        return &native_batch;
    }

    [[nodiscard]] bool AcknowledgeNativeBatch(RenderVfxWorldBatch receipt, bool submitted) noexcept
    {
        if (!submitted || !native_batch_active || receipt.session != native_batch.identity.session ||
            receipt.stateful_target != native_batch.identity.stateful_target ||
            receipt.sequence != native_batch.identity.sequence)
            return false;
        last_committed_stateful_target = receipt.stateful_target;
        has_committed_stateful_target = true;
        if (native_batch.identity.reset_particle_state) particle_reset_pending = false;
        DiscardNativeBatch();
        return true;
    }

    bool MarkPersistentBurst(std::uint64_t stable_id, std::uint8_t visual_kind)
    {
        return stable_id != 0 &&
            seen_persistent_bursts.insert({stable_id, visual_kind}).second;
    }

    void ClearDecoded() noexcept { decoded.Clear(); }

    // Publish staged birth commands only after every destination has capacity.
    // Retained command contents stay unchanged if any reserve throws; after reservation,
    // the payload copies cannot throw or allocate.
    [[nodiscard]] bool AppendPreparedBirths(DecodedVfxState &prepared)
    {
        if (std::addressof(prepared) == std::addressof(decoded)) return false;
        auto &particles = decoded.particle_spawns;
        auto &lines = decoded.effect_lines;
        auto &flashes = decoded.flashes;
        const auto reserve_append = [](auto &destination, const auto &source) {
            if (!destination.empty()) ReserveTo(destination, CheckedAdd(destination.size(), source.size()));
        };
        reserve_append(particles, prepared.particle_spawns);
        reserve_append(lines, prepared.effect_lines);
        reserve_append(flashes, prepared.flashes);
        static_assert(std::is_nothrow_copy_constructible_v<ParticleSpawnCommand>);
        static_assert(std::is_nothrow_copy_constructible_v<VfxLineSpawnCommand>);
        static_assert(std::is_nothrow_copy_constructible_v<VfxFlashSpawnInput>);
        const auto publish = [](auto &destination, auto &source) {
            if (source.empty()) return;
            if (destination.empty()) destination.swap(source);
            else for (const auto &value : source) destination.push_back(value);
            source.clear();
        };
        publish(particles, prepared.particle_spawns);
        publish(lines, prepared.effect_lines);
        publish(flashes, prepared.flashes);
        return true;
    }

    // Only decoded retention belongs to a content generation. Publication must
    // preserve the session clock, pending events and burst tombstones.
    void SwapDecoded(DecodedVfxState &prepared) noexcept
    {
        decoded.Swap(prepared);
    }

    // Terminal simulation stops advancing after the final boss transition.
    // Keep only this presentation clock on real time; all other VFX clocks
    // remain tied to the authoritative snapshot tick.
    double UpdateTerminalClock(Tick snapshot_tick, SessionPhase phase,
                             FixedStepClock::Clock::time_point now) noexcept
    {
        const bool terminal = phase == SessionPhase::Victory || phase == SessionPhase::Defeat;
        if (!terminal)
        {
            terminal_clock.reset();
            return snapshot_tick;
        }
        if (!terminal_clock ||
            terminal_clock->source_tick != snapshot_tick || terminal_clock->phase != phase)
        {
            terminal_clock = TerminalPresentationClock{snapshot_tick, phase, now};
            return snapshot_tick;
        }
        if (now <= terminal_clock->started_at) return terminal_clock->source_tick;
        const auto elapsed = now - terminal_clock->started_at;
        const auto elapsed_ticks = elapsed / FixedStepClock::kFixedStep;
        const auto advance = static_cast<Tick>(elapsed_ticks);
        const auto remaining = std::numeric_limits<Tick>::max() - terminal_clock->source_tick;
        if (advance >= remaining) return static_cast<double>(std::numeric_limits<Tick>::max());
        const double fraction = std::chrono::duration<double>(elapsed % FixedStepClock::kFixedStep).count() /
            std::chrono::duration<double>(FixedStepClock::kFixedStep).count();
        return static_cast<double>(terminal_clock->source_tick + advance) + fraction;
    }

    bool Synchronize(std::uint64_t current_session)
    {
        if (session_id == current_session) return false;
        session_id = current_session;
        low_health_pulse.Clear();
        terminal_clock.reset();
        active_effects.Clear();
        ClearDecoded();
        DiscardNativeBatch();
        last_committed_stateful_target = 0;
        next_native_batch_sequence = 1;
        has_committed_stateful_target = false;
        particle_reset_pending = false;
        seen_persistent_bursts.clear();
        last_status_burst_tick = 0;
        has_status_burst_tick = false;
        raw_events.DiscardBeforeSession(session_id);
        return true;
    }
};
} // namespace hs::runtime_detail
