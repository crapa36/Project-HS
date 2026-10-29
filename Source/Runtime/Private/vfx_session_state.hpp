#pragma once

#include <hs/core/bounded_spsc_queue.hpp>
#include <hs/core/fixed_step_clock.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/game_domain/game_types.hpp>
#include "vfx_typed_entity_surface_commands.hpp"
#include "vfx_typed_screen_commands.hpp"
#include "vfx_event_view.hpp"
#include "vfx_workspace_size.hpp"
#include "vfx_birth_admission.hpp"
#include <hs/render_data/render_frame_input.hpp>
#include <hs/render_data/vfx_spawn_command.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hs::runtime_detail
{
namespace vfx_retained_size
{
inline std::size_t RingGapPayload(const VfxPayload &payload) noexcept
{
    const auto *gaps = std::get_if<VfxRingGapsPayload>(&payload);
    return gaps ? Vector(gaps->gap_angles_degrees) : 0;
}

inline std::size_t RingGapPayload(const VfxEventInput &input) noexcept
{ return RingGapPayload(input.payload); }
} // namespace vfx_retained_size

// Soft growth allowance for selected cosmetic CPU rows, including retained/queued capacity.
// Authoritative warnings keep their existing path and can grow independently.
// shortcut: selected cosmetic flash/mote rows use this allowance; other activation/native profiles still need fanout admission.
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
    using FrameRows = std::array<std::uint32_t, static_cast<std::size_t>(Profile::Count)>;

    struct AppendRequest
    {
        const VfxEventInput *input{};
        std::uint16_t profile_mask{};
        bool replace_ground_context_owner{};
        std::uint16_t cosmetic_profile_mask{};
        std::uint32_t importance{};
        const FrameRows *frame_rows{}; // Borrowed for PrepareAppendBatch only.
        std::uint32_t ribbon_frame_outputs{};
    };
    struct FrameStorage
    {
        // row_bytes == 0 disables this frame-fanout estimate for the profile.
        std::size_t row_bytes{};
        // Caller-owned current capacity and already-reserved base demand.
        std::size_t capacity_rows{};
        std::size_t base_rows{};
    };
    struct ProfileAdmission
    {
        std::size_t held_bytes{};
        std::size_t budget_bytes{};
        std::vector<std::size_t> &order_scratch;
        std::size_t requested_cosmetic_memberships{};
        std::size_t admitted_cosmetic_memberships{};
        std::array<FrameStorage, static_cast<std::size_t>(Profile::Count)> frame_storage{};
        std::array<std::size_t, static_cast<std::size_t>(Profile::Count)> reserved_frame_rows{};
        FrameStorage distortion_gap_storage{};
        std::size_t reserved_distortion_gaps{};
        FrameStorage ground_gap_storage{};
        std::size_t reserved_ground_gaps{};
        FrameStorage ribbon_output_storage{};
        std::size_t reserved_ribbon_outputs{};
        FrameStorage surface_capture_storage{};
        FrameStorage surface_capture_input_storage{};
        std::size_t reserved_surface_captures{};
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
        PreparedAppendBatch &prepared, ProfileAdmission *admission = nullptr);
    [[nodiscard]] bool CanCommitAppendBatch(const PreparedAppendBatch &prepared) const noexcept;

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

    void SetQuality(VfxQuality quality) noexcept
    {
        for (auto &row : rows_) row.quality = quality;
        BeginFrameBindings();
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
        RefreshProfile(profile, std::forward<decltype(refresh)>(refresh), nullptr);
    }

    void RefreshProfile(Profile profile, auto &&refresh, auto &&expired)
    {
        assert(UsesOwnerBinding(profile));
        using Expiry = std::remove_cvref_t<decltype(expired)>;
        constexpr bool has_expiry = !std::is_same_v<Expiry, std::nullptr_t>;
        auto &refs = profiles_[static_cast<std::size_t>(profile)];
        // Recording a completed binding must not fail after its row/count is published.
        const auto additional_bindings = std::min(refs.size(), rows_.size() - bound_row_ids_.size());
        ReserveTo(bound_row_ids_, bound_row_ids_.size() + additional_bindings);
        std::erase_if(refs, [&](Row row) {
            if (rows_[row].geometry_owner_id == 0)
            {
                if constexpr (has_expiry)
                    if (expired(rows_[row]))
                    {
                        RemoveMembership(profile, row);
                        return true;
                    }
                return false;
            }
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
            if (binding != kDroppedBinding)
            {
                if constexpr (has_expiry)
                    if (expired(bound_rows_[binding]))
                    {
                        RemoveMembership(profile, row);
                        return true;
                    }
                return false;
            }
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
        auto bytes = vfx_retained_size::Add(Vector(rows_),
            vfx_retained_size::Add(Vector(profile_masks_), Vector(binding_indices_)));
        bytes = vfx_retained_size::Add(bytes, Vector(bound_rows_));
        bytes = vfx_retained_size::Add(bytes, rows_payload_capacity_bytes_);
        bytes = vfx_retained_size::Add(bytes, bound_payload_capacity_bytes_);
        bytes = vfx_retained_size::Add(bytes,
            vfx_retained_size::Add(Vector(bound_row_ids_),
                vfx_retained_size::Add(Vector(scratch_rows_), Vector(live_rows_))));
        for (const auto &profile : profiles_)
            bytes = vfx_retained_size::Add(bytes, Vector(profile));
        return vfx_retained_size::Add(bytes, HashTable(event_rows_));
    }
    [[nodiscard]] std::size_t OwnedPayloadCapacityBytes() const noexcept
    { return vfx_retained_size::Add(rows_payload_capacity_bytes_, bound_payload_capacity_bytes_); }

  private:
    friend struct VfxSessionState;
    [[nodiscard]] bool CommitPreparedAppendBatch(PreparedAppendBatch &prepared) noexcept;
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
    [[nodiscard]] std::size_t AdditionalProfileRows(Profile profile) const noexcept
    { return profile_additions_[static_cast<std::size_t>(profile)].size(); }

  private:
    friend class ActiveEffectStore;
    struct PendingRow
    {
        Row row{};
        std::uint16_t initial_mask{}, required_mask{}, added_mask{}, recorded_mask{};
        std::size_t first_request_index{};
        bool is_new{};
        FrameRows frame_rows{};
        std::size_t nonuniform_gap_floats{};
        std::uint32_t ribbon_frame_outputs{};
        std::array<std::size_t, static_cast<std::size_t>(Profile::Count)> cosmetic_candidates = [] {
            std::array<std::size_t, static_cast<std::size_t>(Profile::Count)> result{};
            result.fill(std::numeric_limits<std::size_t>::max());
            return result;
        }();
    };
    struct CosmeticMembership
    {
        std::size_t pending_index{};
        std::size_t first_request_index{};
        std::uint32_t importance{};
        std::uint8_t profile{};
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
        request_pending_indices_.clear();
        cosmetic_memberships_.clear();
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
    std::vector<std::size_t> request_pending_indices_;
    std::vector<CosmeticMembership> cosmetic_memberships_;
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
        auto bytes = vfx_retained_size::Add(Vector(new_rows_),
            vfx_retained_size::Add(Vector(new_masks_), Vector(pending_rows_)));
        bytes = vfx_retained_size::Add(bytes, new_rows_payload_capacity_bytes_);
        bytes = vfx_retained_size::Add(bytes, Vector(pending_slots_));
        bytes = vfx_retained_size::Add(bytes,
            vfx_retained_size::Add(Vector(request_pending_indices_), Vector(cosmetic_memberships_)));
        bytes = vfx_retained_size::Add(bytes, HashTable(new_index_));
        bytes = vfx_retained_size::Add(bytes,
            vfx_retained_size::Add(Vector(ground_replacements_), Vector(removed_ground_rows_)));
        for (const auto &profile : profile_additions_)
            bytes = vfx_retained_size::Add(bytes, Vector(profile));
        return bytes;
    }
    [[nodiscard]] std::size_t OwnedPayloadCapacityBytes() const noexcept
    { return new_rows_payload_capacity_bytes_; }
};

inline bool ActiveEffectStore::PrepareAppendBatch(std::span<const AppendRequest> requests,
    PreparedAppendBatch &prepared, ProfileAdmission *admission)
{
    const auto old_owner_bytes = LogicalCapacityBytes();
    const auto old_prepared_bytes = prepared.LogicalCapacityBytes();
    const auto old_order_bytes = admission
        ? vfx_retained_size::Vector(admission->order_scratch) : 0;
    if (admission)
    {
        admission->requested_cosmetic_memberships = 0;
        admission->admitted_cosmetic_memberships = 0;
        admission->reserved_frame_rows.fill(0);
        admission->reserved_distortion_gaps = 0;
        admission->reserved_ground_gaps = 0;
        admission->reserved_ribbon_outputs = 0;
        admission->reserved_surface_captures = 0;
        admission->order_scratch.clear();
    }
    prepared.Cancel();
    constexpr auto profile_count = static_cast<unsigned>(Profile::Count);
    constexpr auto valid_mask = static_cast<std::uint16_t>((1u << profile_count) - 1u);
    constexpr auto ground_bit = ProfileBit(Profile::Ground);
    constexpr auto protected_mask = static_cast<std::uint16_t>(
        ProfileBit(Profile::EntitySurface) | ProfileBit(Profile::PointRate));
    constexpr auto ground_profile = static_cast<unsigned>(Profile::Ground);
    constexpr auto ribbon_profile = static_cast<unsigned>(Profile::Ribbon);
    constexpr auto distortion_profile = static_cast<unsigned>(Profile::Distortion);
    constexpr auto no_pending = std::numeric_limits<std::size_t>::max();
    std::size_t replacement_count{};
    std::size_t cosmetic_candidate_limit{};
    for (const auto &request : requests)
    {
        if (!request.input || (request.profile_mask & static_cast<std::uint16_t>(~valid_mask)) ||
            (request.cosmetic_profile_mask & static_cast<std::uint16_t>(~request.profile_mask)))
            return false;
        if (admission)
            for (unsigned profile = 0; profile < profile_count; ++profile)
                if ((request.profile_mask & static_cast<std::uint16_t>(1u << profile)) &&
                    admission->frame_storage[profile].row_bytes && !request.frame_rows)
                    return false;
        if (admission && (request.profile_mask & ProfileBit(Profile::Distortion)) &&
            admission->distortion_gap_storage.row_bytes && !request.frame_rows)
            return false;
        if (admission && (request.profile_mask & ProfileBit(Profile::Ground)) &&
            admission->ground_gap_storage.row_bytes && !request.frame_rows)
            return false;
        if (admission && (request.profile_mask & ProfileBit(Profile::Ribbon)) &&
            admission->ribbon_output_storage.row_bytes && !request.frame_rows)
            return false;
        if (admission && admission->ribbon_output_storage.row_bytes &&
            (request.profile_mask & ProfileBit(Profile::Ribbon)))
        {
            const auto ribbon_sources = request.frame_rows
                ? (*request.frame_rows)[ribbon_profile] : 0;
            if ((ribbon_sources == 0) != (request.ribbon_frame_outputs == 0)) return false;
        }
        if (admission &&
            (((request.profile_mask & ProfileBit(Profile::Ground)) &&
              admission->ground_gap_storage.row_bytes) ||
             ((request.profile_mask & ProfileBit(Profile::Distortion)) &&
              admission->distortion_gap_storage.row_bytes)))
            if (const auto *gaps = std::get_if<VfxRingGapsPayload>(&request.input->payload);
                gaps && !gaps->GapStorageConsistent()) return false;
        if (request.replace_ground_context_owner)
        {
            if (!(request.profile_mask & ground_bit) ||
                !std::holds_alternative<VfxContextPayload>(request.input->payload)) return false;
            replacement_count = CheckedAdd(replacement_count, 1);
        }
        if (admission)
            for (unsigned profile = 0; profile < profile_count; ++profile)
                if (request.cosmetic_profile_mask & static_cast<std::uint16_t>(1u << profile))
                    cosmetic_candidate_limit = CheckedAdd(cosmetic_candidate_limit, 1);
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

    const auto slot_count = CheckedAdd(requests.size(), requests.size());
    ReserveTo(prepared.pending_slots_, slot_count);
    prepared.pending_slots_.assign(slot_count, no_pending);
    ReserveTo(prepared.request_pending_indices_, requests.size());
    prepared.request_pending_indices_.assign(requests.size(), no_pending);
    ReserveTo(prepared.cosmetic_memberships_, cosmetic_candidate_limit);

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
            const auto &pending_input = pending.is_new
                ? *requests[pending.first_request_index].input : rows_[pending.row];
            if (KeyFor(pending_input) == key) break;
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
                PreparedAppendBatch::PendingRow pending;
                pending.row = existing->second;
                pending.initial_mask = initial_mask;
                pending.first_request_index = request_index;
                if (const auto *gaps = std::get_if<VfxRingGapsPayload>(&rows_[existing->second].payload);
                    gaps && gaps->GapStorageConsistent() && !gaps->uniform_gaps.Active())
                    pending.nonuniform_gap_floats = gaps->gap_angles_degrees.size();
                ReserveOne(prepared.pending_rows_);
                prepared.pending_rows_.push_back(std::move(pending));
            }
            else
            {
                pending_index = prepared.pending_rows_.size();
                PreparedAppendBatch::PendingRow pending;
                pending.first_request_index = request_index;
                pending.is_new = true;
                if (const auto *gaps = std::get_if<VfxRingGapsPayload>(&request.input->payload);
                    gaps && gaps->GapStorageConsistent() && !gaps->uniform_gaps.Active())
                    pending.nonuniform_gap_floats = gaps->gap_angles_degrees.size();
                ReserveOne(prepared.pending_rows_);
                prepared.pending_rows_.push_back(std::move(pending));
            }
            prepared.pending_slots_[slot] = pending_index;
        }
        prepared.request_pending_indices_[request_index] = pending_index;

        auto &pending_row = prepared.pending_rows_[pending_index];
        if (admission && request.frame_rows)
            for (unsigned profile = 0; profile < profile_count; ++profile)
                if (mask & static_cast<std::uint16_t>(1u << profile))
                    pending_row.frame_rows[profile] = std::max(pending_row.frame_rows[profile],
                        (*request.frame_rows)[profile]);
        if (admission && (mask & ProfileBit(Profile::Ribbon)))
            pending_row.ribbon_frame_outputs = std::max(pending_row.ribbon_frame_outputs,
                request.ribbon_frame_outputs);
        auto cosmetic_mask = admission && !request.replace_ground_context_owner
            ? static_cast<std::uint16_t>(request.cosmetic_profile_mask & mask & ~protected_mask) : 0;
        if (request.input->geometry_owner_id != 0)
            cosmetic_mask &= static_cast<std::uint16_t>(~ground_bit);
        const auto required_mask = static_cast<std::uint16_t>(mask & ~cosmetic_mask);
        pending_row.required_mask |= required_mask;
        for (unsigned profile = 0; profile < profile_count; ++profile)
        {
            const auto bit = static_cast<std::uint16_t>(1u << profile);
            if (!(cosmetic_mask & bit) || (pending_row.initial_mask & bit))
                continue;
            auto &candidate_index = pending_row.cosmetic_candidates[profile];
            if (candidate_index == no_pending)
            {
                candidate_index = prepared.cosmetic_memberships_.size();
                prepared.cosmetic_memberships_.push_back({pending_index, request_index,
                    request.importance, static_cast<std::uint8_t>(profile)});
            }
            else
                prepared.cosmetic_memberships_[candidate_index].importance = std::max(
                    prepared.cosmetic_memberships_[candidate_index].importance, request.importance);
        }
    }

    std::size_t requested_cosmetics{};
    for (const auto &candidate : prepared.cosmetic_memberships_)
    {
        const auto &pending = prepared.pending_rows_[candidate.pending_index];
        const auto bit = static_cast<std::uint16_t>(1u << candidate.profile);
        if (!(pending.initial_mask & bit) && !(pending.required_mask & bit))
            requested_cosmetics = CheckedAdd(requested_cosmetics, 1);
    }
    if (admission)
    {
        admission->requested_cosmetic_memberships = requested_cosmetics;
        ReserveTo(admission->order_scratch, requested_cosmetics);
    }

    const auto available_row_ids = static_cast<std::size_t>(std::numeric_limits<Row>::max()) - rows_.size();
    // Required memberships are never subject to the cosmetic allowance.
    std::size_t required_new_rows{};
    std::array<std::size_t, static_cast<std::size_t>(Profile::Count)> required_profile_rows{};
    std::array<std::size_t, static_cast<std::size_t>(Profile::Count)> required_frame_rows{};
    std::size_t required_distortion_gaps{};
    std::size_t required_ground_gaps{};
    std::size_t required_ribbon_outputs{};
    std::size_t required_payload_bytes{};
    for (const auto &pending : prepared.pending_rows_)
    {
        const auto added_required = static_cast<std::uint16_t>(pending.required_mask & ~pending.initial_mask);
        if (pending.is_new && pending.required_mask)
        {
            required_new_rows = CheckedAdd(required_new_rows, 1);
            const auto &input = *requests[pending.first_request_index].input;
            if (const auto *gaps = std::get_if<VfxRingGapsPayload>(&input.payload))
                required_payload_bytes = CheckedAdd(required_payload_bytes,
                    vfx_retained_size::Multiply(gaps->gap_angles_degrees.size(), sizeof(float)));
        }
        for (unsigned profile = 0; profile < profile_count; ++profile)
            if (added_required & static_cast<std::uint16_t>(1u << profile))
            {
                required_profile_rows[profile] = CheckedAdd(required_profile_rows[profile], 1);
                if (admission && admission->frame_storage[profile].row_bytes)
                    required_frame_rows[profile] = CheckedAdd(required_frame_rows[profile],
                        pending.frame_rows[profile]);
                if (admission && profile == static_cast<unsigned>(Profile::Distortion) &&
                    admission->distortion_gap_storage.row_bytes && pending.frame_rows[profile] != 0)
                    required_distortion_gaps = CheckedAdd(required_distortion_gaps,
                        pending.nonuniform_gap_floats);
                if (admission && profile == ground_profile &&
                    admission->ground_gap_storage.row_bytes && pending.frame_rows[profile] != 0)
                    required_ground_gaps = CheckedAdd(required_ground_gaps,
                        pending.nonuniform_gap_floats);
                if (admission && profile == ribbon_profile &&
                    admission->ribbon_output_storage.row_bytes && pending.frame_rows[profile] != 0)
                    required_ribbon_outputs = CheckedAdd(required_ribbon_outputs,
                        pending.ribbon_frame_outputs);
            }
    }
    if (required_new_rows > available_row_ids)
        throw std::length_error("too many retained VFX event rows");
    constexpr auto max_distortion_gap_count =
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
    if (admission && admission->distortion_gap_storage.row_bytes &&
        (admission->distortion_gap_storage.base_rows > max_distortion_gap_count ||
         required_distortion_gaps > max_distortion_gap_count - admission->distortion_gap_storage.base_rows))
        throw std::length_error("required distortion gap pool exceeds uint32 offsets");
    if (admission && admission->ground_gap_storage.row_bytes &&
        (admission->ground_gap_storage.base_rows > max_distortion_gap_count ||
         required_ground_gaps > max_distortion_gap_count - admission->ground_gap_storage.base_rows))
        throw std::length_error("required ground gap pool exceeds uint32 offsets");
    if (admission && admission->ribbon_output_storage.row_bytes &&
        (admission->ribbon_output_storage.base_rows > max_distortion_gap_count ||
         required_ribbon_outputs > max_distortion_gap_count - admission->ribbon_output_storage.base_rows))
        throw std::length_error("required ribbon output pool exceeds uint32 offsets");

    using SupplementalRows = std::array<std::size_t, 3>; // Ground gaps, distortion gaps, ribbon outputs.
    using ProfileCounts = std::array<std::size_t, profile_count>;
    const std::array<const FrameStorage *, 3> supplemental_storage = admission
        ? std::array<const FrameStorage *, 3>{&admission->ground_gap_storage,
            &admission->distortion_gap_storage, &admission->ribbon_output_storage}
        : std::array<const FrameStorage *, 3>{};
    const auto try_add_supplemental = [&](SupplementalRows &rows,
        std::size_t pool, std::size_t count) {
        const auto &storage = *supplemental_storage[pool];
        if (!storage.row_bytes) return true;
        if (storage.base_rows > max_distortion_gap_count ||
            rows[pool] > max_distortion_gap_count - storage.base_rows ||
            count > max_distortion_gap_count - storage.base_rows - rows[pool])
            return false;
        rows[pool] += count;
        return true;
    };
    using CapacityProjection = std::pair<std::size_t, bool>;
    const auto add_projection = [](CapacityProjection &total, CapacityProjection part) {
        total.second |= part.second || part.first > std::numeric_limits<std::size_t>::max() - total.first;
        total.first = vfx_retained_size::Add(total.first, part.first);
    };
    const auto product_projection = [](std::size_t count, std::size_t item_bytes) -> CapacityProjection {
        return {vfx_retained_size::Multiply(count, item_bytes),
            count && item_bytes > std::numeric_limits<std::size_t>::max() / count};
    };
    const auto vector_growth = [&](std::size_t capacity, std::size_t current_size,
        std::size_t additional, std::size_t max_size, std::size_t item_bytes) {
        if (additional > max_size - current_size)
            throw std::length_error("retained VFX capacity overflow");
        const auto needed = current_size + additional;
        if (needed <= capacity) return std::size_t{};
        const auto growth = capacity == 0 ? std::size_t{8} :
            capacity + std::min(max_size - capacity, capacity / 2 + 1);
        const auto target = std::max(needed, growth);
        return vfx_retained_size::Multiply(target - capacity, item_bytes);
    };
    const auto project_row_growth = [&](std::size_t new_row_count, std::size_t payload_bytes) {
        CapacityProjection total{};
        const auto add = [&](std::size_t bytes) { add_projection(total, {bytes, false}); };
        add(vector_growth(prepared.new_rows_.capacity(), prepared.new_rows_.size(), new_row_count,
            prepared.new_rows_.max_size(), sizeof(VfxEventInput)));
        add(vector_growth(prepared.new_masks_.capacity(), prepared.new_masks_.size(), new_row_count,
            prepared.new_masks_.max_size(), sizeof(std::uint16_t)));
        add(vector_growth(rows_.capacity(), rows_.size(), new_row_count, rows_.max_size(), sizeof(VfxEventInput)));
        add(vector_growth(profile_masks_.capacity(), profile_masks_.size(), new_row_count,
            profile_masks_.max_size(), sizeof(std::uint16_t)));
        add(vector_growth(binding_indices_.capacity(), rows_.size(), new_row_count,
            binding_indices_.max_size(), sizeof(std::uint32_t)));
        const auto &map = prepared.new_index_;
        if (new_row_count > map.max_size() - map.size())
            throw std::length_error("retained VFX index overflow");
        add(vfx_retained_size::Multiply(new_row_count,
            sizeof(std::pair<const EventKey, Row>) + 3 * sizeof(void *)));
        add(payload_bytes);
        return total;
    };
    const auto project_profile_growth = [&](std::size_t profile, std::size_t profile_rows,
        std::size_t frame_rows) {
        CapacityProjection total{};
        const auto add = [&](std::size_t bytes) { add_projection(total, {bytes, false}); };
        const auto removed = profile == static_cast<std::size_t>(Profile::Ground)
            ? prepared.removed_ground_rows_.size() : 0;
        const auto retained = profiles_[profile].size() - removed;
        add(vector_growth(profiles_[profile].capacity(), retained, profile_rows,
            profiles_[profile].max_size(), sizeof(Row)));
        const auto &staged = prepared.profile_additions_[profile];
        add(vector_growth(staged.capacity(), staged.size(), profile_rows,
            staged.max_size(), sizeof(Row)));
        if (admission && admission->frame_storage[profile].row_bytes)
        {
            const auto &frame = admission->frame_storage[profile];
            const auto demand = CheckedAdd(frame.base_rows, frame_rows);
            const auto growth = demand > frame.capacity_rows ? demand - frame.capacity_rows : 0;
            add_projection(total, product_projection(growth, frame.row_bytes));
        }
        if (admission && profile == static_cast<std::size_t>(Profile::EntitySurface))
            for (const auto *frame : {&admission->surface_capture_storage,
                                      &admission->surface_capture_input_storage})
            {
                if (!frame->row_bytes) continue;
                const auto demand = CheckedAdd(frame->base_rows, profile_rows);
                const auto growth = demand > frame->capacity_rows ? demand - frame->capacity_rows : 0;
                add_projection(total, product_projection(growth, frame->row_bytes));
            }
        return total;
    };
    const auto project_supplemental_growth = [&](std::size_t pool, std::size_t rows) -> CapacityProjection {
        if (!admission) return {};
        const auto &frame = *supplemental_storage[pool];
        if (!frame.row_bytes) return {};
        if (frame.base_rows > max_distortion_gap_count ||
            rows > max_distortion_gap_count - frame.base_rows)
            return {std::numeric_limits<std::size_t>::max(), true};
        const auto demand = frame.base_rows + rows;
        const auto growth = demand > frame.capacity_rows ? demand - frame.capacity_rows : 0;
        return product_projection(growth, frame.row_bytes);
    };
    const auto project_optional_growth = [&](std::size_t new_row_count,
        const ProfileCounts &profile_rows, const ProfileCounts &frame_rows,
        const SupplementalRows &supplemental_rows, std::size_t payload_bytes) {
        auto total = project_row_growth(new_row_count, payload_bytes);
        for (std::size_t profile = 0; profile < profiles_.size(); ++profile)
            add_projection(total, project_profile_growth(profile, profile_rows[profile], frame_rows[profile]));
        for (std::size_t pool = 0; pool < supplemental_storage.size(); ++pool)
            add_projection(total, project_supplemental_growth(pool, supplemental_rows[pool]));
        return total;
    };

    auto admitted_new_rows = required_new_rows;
    auto admitted_profile_rows = required_profile_rows;
    auto admitted_frame_rows = required_frame_rows;
    SupplementalRows admitted_supplemental_rows{
        required_ground_gaps, required_distortion_gaps, required_ribbon_outputs};
    auto admitted_payload_bytes = required_payload_bytes;
    std::size_t old_components = vfx_retained_size::Add(old_owner_bytes,
        vfx_retained_size::Add(old_prepared_bytes, old_order_bytes));
    const auto now_components = vfx_retained_size::Add(LogicalCapacityBytes(),
        vfx_retained_size::Add(prepared.LogicalCapacityBytes(),
            admission ? vfx_retained_size::Vector(admission->order_scratch) : 0));
    const auto adjusted_held = !admission || admission->held_bytes == std::numeric_limits<std::size_t>::max() ||
        old_components == std::numeric_limits<std::size_t>::max() || admission->held_bytes < old_components
        ? std::numeric_limits<std::size_t>::max()
        : vfx_retained_size::Add(admission->held_bytes - old_components, now_components);
    auto admitted_projection = project_optional_growth(admitted_new_rows,
        admitted_profile_rows, admitted_frame_rows, admitted_supplemental_rows, admitted_payload_bytes);
    if (admission)
    {
        for (std::size_t i = 0; i < prepared.cosmetic_memberships_.size(); ++i)
        {
            const auto &candidate = prepared.cosmetic_memberships_[i];
            const auto &pending = prepared.pending_rows_[candidate.pending_index];
            const auto bit = static_cast<std::uint16_t>(1u << candidate.profile);
            if ((pending.initial_mask | pending.required_mask) & bit) continue;
            admission->order_scratch.push_back(i);
        }
        bool all_cosmetics_admitted{};
        auto all_new_rows = admitted_new_rows;
        auto all_profile_rows = admitted_profile_rows;
        auto all_frame_rows = admitted_frame_rows;
        auto all_supplemental_rows = admitted_supplemental_rows;
        auto all_payload_bytes = admitted_payload_bytes;
        try
        {
            bool can_admit_all = true;
            for (const auto candidate_index : admission->order_scratch)
            {
                const auto &candidate = prepared.cosmetic_memberships_[candidate_index];
                const auto &pending = prepared.pending_rows_[candidate.pending_index];
                const auto bit = static_cast<std::uint16_t>(1u << candidate.profile);
                if ((pending.initial_mask | pending.required_mask) & bit) continue;
                auto first_candidate = no_pending;
                for (const auto index : pending.cosmetic_candidates)
                    first_candidate = std::min(first_candidate, index);
                if (pending.is_new && pending.required_mask == 0 &&
                    candidate_index == first_candidate)
                {
                    all_new_rows = CheckedAdd(all_new_rows, 1);
                    if (all_new_rows > available_row_ids)
                    {
                        can_admit_all = false;
                        break;
                    }
                    const auto &input = *requests[pending.first_request_index].input;
                    if (const auto *gaps = std::get_if<VfxRingGapsPayload>(&input.payload))
                        all_payload_bytes = CheckedAdd(all_payload_bytes,
                            vfx_retained_size::Multiply(gaps->gap_angles_degrees.size(), sizeof(float)));
                }
                all_profile_rows[candidate.profile] = CheckedAdd(all_profile_rows[candidate.profile], 1);
                if (admission->frame_storage[candidate.profile].row_bytes)
                    all_frame_rows[candidate.profile] = CheckedAdd(all_frame_rows[candidate.profile],
                        pending.frame_rows[candidate.profile]);
                if (candidate.profile == ground_profile &&
                    admission->ground_gap_storage.row_bytes && pending.frame_rows[candidate.profile] != 0)
                    can_admit_all &= try_add_supplemental(all_supplemental_rows, 0,
                        pending.nonuniform_gap_floats);
                if (candidate.profile == distortion_profile &&
                    admission->distortion_gap_storage.row_bytes && pending.frame_rows[candidate.profile] != 0)
                    can_admit_all &= try_add_supplemental(all_supplemental_rows, 1,
                        pending.nonuniform_gap_floats);
                if (candidate.profile == ribbon_profile && admission->ribbon_output_storage.row_bytes &&
                    pending.frame_rows[candidate.profile] != 0)
                    can_admit_all &= try_add_supplemental(all_supplemental_rows, 2,
                        pending.ribbon_frame_outputs);
                if (!can_admit_all) break;
            }
            if (can_admit_all)
            {
                const auto after = project_optional_growth(all_new_rows, all_profile_rows,
                    all_frame_rows, all_supplemental_rows, all_payload_bytes);
                const auto after_total = vfx_retained_size::Add(adjusted_held, after.first);
                const bool no_marginal_growth = !admitted_projection.second && !after.second &&
                    admitted_projection.first == after.first;
                const bool total_overflow = after.first > std::numeric_limits<std::size_t>::max() - adjusted_held;
                if ((!after.second && !total_overflow && after_total <= admission->budget_bytes) ||
                    no_marginal_growth)
                {
                    for (const auto candidate_index : admission->order_scratch)
                    {
                        const auto &candidate = prepared.cosmetic_memberships_[candidate_index];
                        prepared.pending_rows_[candidate.pending_index].added_mask |=
                            static_cast<std::uint16_t>(1u << candidate.profile);
                    }
                    admitted_new_rows = all_new_rows;
                    admitted_profile_rows = all_profile_rows;
                    admitted_frame_rows = all_frame_rows;
                    admitted_supplemental_rows = all_supplemental_rows;
                    admitted_payload_bytes = all_payload_bytes;
                    admitted_projection = after;
                    admission->admitted_cosmetic_memberships = CheckedAdd(
                        admission->admitted_cosmetic_memberships, admission->order_scratch.size());
                    all_cosmetics_admitted = true;
                }
            }
        }
        catch (const std::length_error &)
        {
            // Fall back to ordered partial admission when the aggregate cannot fit
            // representable destination capacity.
        }
        if (!all_cosmetics_admitted)
        {
            auto row_projection = project_row_growth(admitted_new_rows, admitted_payload_bytes);
            std::array<CapacityProjection, profile_count> profile_projections{};
            for (std::size_t profile = 0; profile < profile_count; ++profile)
                profile_projections[profile] = project_profile_growth(profile,
                    admitted_profile_rows[profile], admitted_frame_rows[profile]);
            std::array<CapacityProjection, 3> supplemental_projections{};
            for (std::size_t pool = 0; pool < supplemental_projections.size(); ++pool)
                supplemental_projections[pool] = project_supplemental_growth(pool, admitted_supplemental_rows[pool]);
            std::sort(admission->order_scratch.begin(), admission->order_scratch.end(), [&](auto left, auto right) {
                const auto &a = prepared.cosmetic_memberships_[left];
                const auto &b = prepared.cosmetic_memberships_[right];
                if (a.importance != b.importance) return a.importance > b.importance;
                if (a.first_request_index != b.first_request_index)
                    return a.first_request_index < b.first_request_index;
                return a.profile < b.profile;
            });
            for (const auto candidate_index : admission->order_scratch)
            {
                auto &candidate = prepared.cosmetic_memberships_[candidate_index];
                auto &pending = prepared.pending_rows_[candidate.pending_index];
                const auto bit = static_cast<std::uint16_t>(1u << candidate.profile);
                auto trial_new_rows = admitted_new_rows;
                auto trial_supplemental_rows = admitted_supplemental_rows;
                auto trial_payload_bytes = admitted_payload_bytes;
                const bool activates_row = pending.is_new && pending.required_mask == 0 && pending.added_mask == 0;
                if (activates_row)
                {
                    trial_new_rows = CheckedAdd(trial_new_rows, 1);
                    if (trial_new_rows > available_row_ids) continue;
                    const auto &input = *requests[pending.first_request_index].input;
                    if (const auto *gaps = std::get_if<VfxRingGapsPayload>(&input.payload))
                        trial_payload_bytes = CheckedAdd(trial_payload_bytes,
                            vfx_retained_size::Multiply(gaps->gap_angles_degrees.size(), sizeof(float)));
                }
                const auto trial_profile_rows = CheckedAdd(admitted_profile_rows[candidate.profile], 1);
                auto trial_frame_rows = admitted_frame_rows[candidate.profile];
                if (admission->frame_storage[candidate.profile].row_bytes)
                    trial_frame_rows = CheckedAdd(trial_frame_rows,
                        pending.frame_rows[candidate.profile]);
                if (candidate.profile == ground_profile &&
                    admission->ground_gap_storage.row_bytes && pending.frame_rows[candidate.profile] != 0)
                {
                    if (!try_add_supplemental(trial_supplemental_rows, 0,
                            pending.nonuniform_gap_floats)) continue;
                }
                if (candidate.profile == distortion_profile &&
                    admission->distortion_gap_storage.row_bytes && pending.frame_rows[candidate.profile] != 0)
                    if (!try_add_supplemental(trial_supplemental_rows, 1,
                            pending.nonuniform_gap_floats)) continue;
                if (candidate.profile == ribbon_profile && admission->ribbon_output_storage.row_bytes &&
                    pending.frame_rows[candidate.profile] != 0)
                    if (!try_add_supplemental(trial_supplemental_rows, 2,
                            pending.ribbon_frame_outputs)) continue;
                const auto trial_row_projection = activates_row
                    ? project_row_growth(trial_new_rows, trial_payload_bytes) : row_projection;
                const auto trial_profile_projection = project_profile_growth(candidate.profile,
                    trial_profile_rows, trial_frame_rows);
                const auto pool = candidate.profile == ground_profile ? 0u
                    : candidate.profile == distortion_profile ? 1u : candidate.profile == ribbon_profile ? 2u : 3u;
                const auto old_supplemental = pool < 3 ? supplemental_projections[pool] : CapacityProjection{};
                const auto trial_supplemental = pool < 3
                    ? project_supplemental_growth(pool, trial_supplemental_rows[pool]) : CapacityProjection{};
                // Only this membership's row, profile and supplemental pool can change.
                auto after = admitted_projection;
                if (!after.second)
                {
                    after.first -= row_projection.first;
                    after.first -= profile_projections[candidate.profile].first;
                    after.first -= old_supplemental.first;
                    add_projection(after, trial_row_projection);
                    add_projection(after, trial_profile_projection);
                    add_projection(after, trial_supplemental);
                }
                const auto after_total = vfx_retained_size::Add(adjusted_held, after.first);
                const bool no_marginal_growth = !admitted_projection.second && !after.second &&
                    admitted_projection.first == after.first;
                const bool total_overflow = after.first > std::numeric_limits<std::size_t>::max() - adjusted_held;
                if ((!after.second && !total_overflow && after_total <= admission->budget_bytes) || no_marginal_growth)
                {
                    pending.added_mask |= bit;
                    admitted_new_rows = trial_new_rows;
                    admitted_profile_rows[candidate.profile] = trial_profile_rows;
                    admitted_frame_rows[candidate.profile] = trial_frame_rows;
                    admitted_supplemental_rows = trial_supplemental_rows;
                    admitted_payload_bytes = trial_payload_bytes;
                    row_projection = trial_row_projection;
                    profile_projections[candidate.profile] = trial_profile_projection;
                    if (pool < 3) supplemental_projections[pool] = trial_supplemental;
                    admitted_projection = after;
                    admission->admitted_cosmetic_memberships = CheckedAdd(
                        admission->admitted_cosmetic_memberships, 1);
                }
            }
        }
    }
    if (admission)
    {
        for (std::size_t profile = 0; profile < admission->frame_storage.size(); ++profile)
            if (admission->frame_storage[profile].row_bytes)
                admission->reserved_frame_rows[profile] = CheckedAdd(
                    admission->frame_storage[profile].base_rows, admitted_frame_rows[profile]);
        if (admission->distortion_gap_storage.row_bytes)
            admission->reserved_distortion_gaps = CheckedAdd(
                admission->distortion_gap_storage.base_rows, admitted_supplemental_rows[1]);
        if (admission->ground_gap_storage.row_bytes)
            admission->reserved_ground_gaps = CheckedAdd(
                admission->ground_gap_storage.base_rows, admitted_supplemental_rows[0]);
        if (admission->ribbon_output_storage.row_bytes)
            admission->reserved_ribbon_outputs = CheckedAdd(
                admission->ribbon_output_storage.base_rows, admitted_supplemental_rows[2]);
        for (const auto *frame : {&admission->surface_capture_storage,
                                  &admission->surface_capture_input_storage})
            if (frame->row_bytes)
                admission->reserved_surface_captures = std::max(admission->reserved_surface_captures,
                    CheckedAdd(frame->base_rows,
                        admitted_profile_rows[static_cast<std::size_t>(Profile::EntitySurface)]));
    }

    // Required bits and selected cosmetics become the final membership mask;
    // provisional IDs are compacted in the original first-key order.
    std::size_t new_rows_count{};
    for (auto &pending : prepared.pending_rows_)
    {
        pending.added_mask |= static_cast<std::uint16_t>(pending.required_mask & ~pending.initial_mask);
        if (pending.is_new)
        {
            if ((pending.required_mask | pending.added_mask) == 0)
                pending.row = std::numeric_limits<Row>::max();
            else
            {
                if (new_rows_count >= std::numeric_limits<Row>::max() - rows_.size())
                    throw std::length_error("too many retained VFX event rows");
                pending.row = static_cast<Row>(CheckedAdd(rows_.size(), new_rows_count));
                new_rows_count = CheckedAdd(new_rows_count, 1);
            }
        }
    }

    std::array<std::size_t, static_cast<std::size_t>(Profile::Count)> profile_addition_counts{};
    for (const auto &pending : prepared.pending_rows_)
        for (unsigned profile = 0; profile < profile_count; ++profile)
            if (pending.added_mask & static_cast<std::uint16_t>(1u << profile))
                profile_addition_counts[profile] = CheckedAdd(profile_addition_counts[profile], 1);
    for (std::size_t profile = 0; profile < profile_addition_counts.size(); ++profile)
        ReserveTo(prepared.profile_additions_[profile], profile_addition_counts[profile]);

    for (std::size_t request_index = 0; request_index < requests.size(); ++request_index)
    {
        const auto pending_index = prepared.request_pending_indices_[request_index];
        if (pending_index == no_pending) continue;
        auto &pending = prepared.pending_rows_[pending_index];
        auto mask = requests[request_index].profile_mask;
        if (mask & ground_bit)
            if (const auto *replacement = replacement_for(*requests[request_index].input);
                replacement && request_index < replacement->last_request)
                mask &= static_cast<std::uint16_t>(~ground_bit);
        mask &= pending.added_mask;
        for (unsigned profile = 0; profile < profile_count; ++profile)
        {
            const auto bit = static_cast<std::uint16_t>(1u << profile);
            if ((mask & bit) && !(pending.recorded_mask & bit))
            {
                pending.recorded_mask |= bit;
                prepared.profile_additions_[profile].push_back(pending.row);
            }
        }
    }

    // Only after final admission do we reserve/copy unique rows and create nodes.
    ReserveTo(prepared.new_rows_, new_rows_count);
    ReserveTo(prepared.new_masks_, new_rows_count);
    ReserveMapTo(prepared.new_index_, new_rows_count);
    for (auto &pending : prepared.pending_rows_)
    {
        if (pending.is_new && pending.row != std::numeric_limits<Row>::max())
        {
            const auto &input = *requests[pending.first_request_index].input;
            prepared.new_rows_.push_back(input);
            prepared.new_rows_payload_capacity_bytes_ = vfx_retained_size::Add(
                prepared.new_rows_payload_capacity_bytes_,
                vfx_retained_size::RingGapPayload(prepared.new_rows_.back()));
            prepared.new_masks_.push_back(pending.added_mask);
            prepared.new_index_.try_emplace(KeyFor(input), pending.row);
        }
    }

    const auto needed_rows = CheckedAdd(rows_.size(), new_rows_count);
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

inline bool ActiveEffectStore::CanCommitAppendBatch(const PreparedAppendBatch &prepared) const noexcept
{
    if (!prepared.ready_ || prepared.owner_ != this || rows_.size() != prepared.base_rows_ ||
        event_rows_.size() != prepared.base_index_size_)
        return false;
    for (std::size_t i = 0; i < profiles_.size(); ++i)
        if (profiles_[i].size() != prepared.base_profile_sizes_[i]) return false;
    return true;
}

inline bool ActiveEffectStore::CommitAppendBatch(PreparedAppendBatch &prepared) noexcept
{
    if (!CanCommitAppendBatch(prepared)) return false;
    return CommitPreparedAppendBatch(prepared);
}

inline bool ActiveEffectStore::CommitPreparedAppendBatch(PreparedAppendBatch &prepared) noexcept
{
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

    // Process each row once per display frame; later births append after the retained prefix.
    void AppendVisibleFlashes(double display_tick, std::vector<VfxFlashSpawnInput> &visible,
        std::size_t first = 0)
    {
        assert(std::addressof(visible) != std::addressof(flashes) && first <= flashes.size());
        visible.reserve(vfx_retained_size::Add(visible.size(), flashes.size() - first));
        const auto end = std::remove_if(flashes.begin() + first, flashes.end(),
            [&](const VfxFlashSpawnInput &flash) {
                if (display_tick < flash.event_tick) return false;
                const auto elapsed = static_cast<float>((display_tick - flash.event_tick) / 60.0);
                if (elapsed >= flash.delay + flash.lifetime) return true;
                if (elapsed < flash.delay) return false;
                auto row = flash;
                row.normalized_age = (elapsed - flash.delay) / flash.lifetime;
                visible.push_back(row);
                return false;
            });
        flashes.erase(end, flashes.end());
    }

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
            std::vector<PresentationEvent> events;
        };

        RawEventScheduler() = default;
        RawEventScheduler(const RawEventScheduler &) = default;
        RawEventScheduler &operator=(const RawEventScheduler &) = default;
        RawEventScheduler(RawEventScheduler &&other) noexcept { Swap(other); }
        RawEventScheduler &operator=(RawEventScheduler &&other) noexcept
        { Swap(other); return *this; }

        [[nodiscard]] bool HasCapacity() const noexcept
        {
            return pending_count_ + in_flight.events.size() < kCapacity;
        }

        [[nodiscard]] std::size_t PendingCount() const noexcept { return pending_count_; }
        // Borrow until the next scheduler mutation.
        [[nodiscard]] const Entry *PendingFront() const noexcept
        { return pending_count_ ? &pending_[pending_head_] : nullptr; }

        [[nodiscard]] bool Enqueue(PresentationEvent event)
        {
            if (!HasCapacity()) return false;
            if (pending_count_ == pending_.size()) GrowPending();
            pending_[(pending_head_ + pending_count_) % pending_.size()] = {std::move(event)};
            ++pending_count_;
            return true;
        }

        template <std::size_t Capacity>
        void DrainIngress(BoundedSpscQueue<PresentationEvent, Capacity> &ingress, auto &&observe)
        {
            // Bound this drain to its published prefix, not an arbitrary per-frame quota.
            PresentationEvent event;
            for (auto remaining = ingress.Size(); remaining != 0; --remaining)
            {
                if (!ingress.TryPopIf(event, [this](const PresentationEvent &front) {
                        return front.kind != PresentationKind::Vfx || Enqueue(front);
                    })) break;
                observe(event);
            }
        }

        [[nodiscard]] const ReadyBatch &Prepare(Tick tick, std::uint64_t session)
        {
            if (!in_flight.events.empty()) return in_flight;
            const auto ready = [&](const Entry &entry) {
                return entry.event.session_id < session ||
                    (entry.event.session_id == session && entry.event.tick <= tick);
            };
            if (!pending_count_ || !ready(pending_[pending_head_])) return in_flight;
            // Stage only the ready prefix; publish FIFO removal after all allocation succeeds.
            static_assert(std::is_trivially_copyable_v<Entry>);
            std::size_t consumed{};
            try
            {
                while (consumed < pending_count_)
                {
                    const auto &entry = pending_[(pending_head_ + consumed) % pending_.size()];
                    if (!ready(entry)) break;
                    ++consumed;
                    if (entry.event.session_id < session) continue;
                    if (in_flight.events.size() == in_flight.events.capacity())
                    {
                        const auto capacity = in_flight.events.capacity();
                        in_flight.events.reserve(std::min(kCapacity,
                            capacity == 0 ? std::size_t{8} : capacity + capacity / 2 + 1));
                    }
                    in_flight.events.push_back(entry.event);
                }
            }
            catch (...)
            {
                in_flight.events.clear();
                throw;
            }
            pending_head_ = (pending_head_ + consumed) % pending_.size();
            pending_count_ -= consumed;
            return in_flight;
        }

        // A failed CPU activation retains this batch; native submission owns its decoded queues.
        void AcknowledgeActivation() noexcept
        {
            in_flight.events.clear();
        }

        void DiscardBeforeSession(std::uint64_t session)
        {
            while (pending_count_ && pending_[pending_head_].event.session_id < session)
                PopPending();
            std::erase_if(in_flight.events, [session](const PresentationEvent &event) {
                return event.session_id < session;
            });
        }

        [[nodiscard]] std::size_t LogicalCapacityBytes() const noexcept
        {
            using namespace vfx_retained_size;
            return Add(Vector(in_flight.events), Vector(pending_));
        }

        ReadyBatch in_flight;

      private:
        void GrowPending()
        {
            const auto size = pending_.empty() ? std::size_t{8} :
                std::min(kCapacity, pending_.size() * 2);
            std::vector<Entry> grown(size);
            static_assert(std::is_nothrow_move_assignable_v<Entry>);
            for (std::size_t index = 0; index < pending_count_; ++index)
                grown[index] = std::move(pending_[(pending_head_ + index) % pending_.size()]);
            pending_.swap(grown);
            pending_head_ = 0;
        }
        void Swap(RawEventScheduler &other) noexcept
        {
            pending_.swap(other.pending_);
            in_flight.events.swap(other.in_flight.events);
            std::swap(pending_head_, other.pending_head_);
            std::swap(pending_count_, other.pending_count_);
        }
        void PopPending() noexcept
        {
            pending_head_ = (pending_head_ + 1) % pending_.size();
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
        bool operator<(const PersistentBurstKey &other) const noexcept
        {
            return stable_id != other.stable_id ? stable_id < other.stable_id : visual_kind < other.visual_kind;
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
        return stable_id != 0 && std::binary_search(seen_persistent_bursts_.begin(),
            seen_persistent_bursts_.end(), PersistentBurstKey{stable_id, visual_kind});
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
            std::erase_if(seen_persistent_bursts_, [](const auto &key) {
                return IsPersistentStatusBurstKind(key.visual_kind);
            });
            last_status_burst_tick = current_tick;
            return;
        }
        last_status_burst_tick = current_tick;
        has_status_burst_tick = true;
        std::erase_if(seen_persistent_bursts_, [&](const auto &key) {
            if (!IsPersistentStatusBurstKind(key.visual_kind)) return false;
            return !has_live_owner(key);
        });
    }

    void ReservePersistentBursts(std::size_t additional_keys)
    {
        if (!additional_keys) return;
        ReserveTo(new_persistent_burst_keys_, additional_keys);
        ReserveTo(merged_persistent_burst_keys_, CheckedAdd(seen_persistent_bursts_.size(), additional_keys));
    }

    // The staged command vector must be independent of destination. All allocating work
    // completes before tombstones or retained commands are published.
    [[nodiscard]] bool CommitPersistentBursts(std::span<const PersistentBurstKey> keys,
        const std::vector<ParticleSpawnCommand> &commands, std::vector<ParticleSpawnCommand> &destination)
    {
        if (std::addressof(commands) == std::addressof(destination)) return false;
        if (keys.empty()) return commands.empty();
        struct ClearScratch
        {
            std::vector<PersistentBurstKey> &new_keys;
            std::vector<PersistentBurstKey> &merged_keys;
            ~ClearScratch() { new_keys.clear(); merged_keys.clear(); }
        } clear_scratch{new_persistent_burst_keys_, merged_persistent_burst_keys_};

        ReserveTo(new_persistent_burst_keys_, keys.size());
        for (const auto &key : keys) new_persistent_burst_keys_.push_back(key);
        std::sort(new_persistent_burst_keys_.begin(), new_persistent_burst_keys_.end());
        for (std::size_t index = 0; index < new_persistent_burst_keys_.size(); ++index)
        {
            const auto &key = new_persistent_burst_keys_[index];
            if (!key.stable_id || (index && key == new_persistent_burst_keys_[index - 1]))
                return false;
        }
        const auto needed_keys = CheckedAdd(seen_persistent_bursts_.size(), new_persistent_burst_keys_.size());
        const auto needed_commands = CheckedAdd(destination.size(), commands.size());

        ReserveTo(merged_persistent_burst_keys_, needed_keys);
        std::set_union(seen_persistent_bursts_.begin(), seen_persistent_bursts_.end(),
            new_persistent_burst_keys_.begin(), new_persistent_burst_keys_.end(),
            std::back_inserter(merged_persistent_burst_keys_));
        // A smaller union means a new key was already published.
        if (merged_persistent_burst_keys_.size() != needed_keys) return false;

        ReserveTo(destination, needed_commands);
        static_assert(std::is_nothrow_copy_constructible_v<ParticleSpawnCommand>);
        seen_persistent_bursts_.swap(merged_persistent_burst_keys_);
        for (const auto &command : commands) destination.push_back(command);
        return true;
    }

    // Tracked owned capacity for this state slice, not a complete activation
    // budget: caller-owned preparation/frame scratch and external
    // RenderInstance/content allocations have separate or transient owners.
    [[nodiscard]] std::size_t TrackedLogicalCapacityBytes() const noexcept
    {
        using namespace vfx_retained_size;
        return Add(active_effects.LogicalCapacityBytes(),
            Add(decoded.LogicalCapacityBytes(),
            Add(native_batch.LogicalCapacityBytes(),
            Add(raw_events.LogicalCapacityBytes(), PersistentBurstCapacityBytes()))));
    }

    [[nodiscard]] std::size_t PersistentBurstCount() const noexcept
    { return seen_persistent_bursts_.size(); }

    [[nodiscard]] std::size_t PersistentBurstCapacityBytes() const noexcept
    {
        using namespace vfx_retained_size;
        return Add(Vector(seen_persistent_bursts_),
            Add(Vector(new_persistent_burst_keys_), Vector(merged_persistent_burst_keys_)));
    }

    std::uint64_t session_id{};
    VfxLowHealthPulseState low_health_pulse;
    std::optional<TerminalPresentationClock> terminal_clock;
    RawEventScheduler raw_events;
    ActiveEffectStore active_effects;
    DecodedVfxState decoded;
    NativeEmissionBatch native_batch;
    bool native_batch_active{};
    Tick last_committed_stateful_target{};
    std::uint64_t next_native_batch_sequence{1};
    bool has_committed_stateful_target{};
    bool particle_reset_pending{};
    Tick last_status_burst_tick{};
    bool has_status_burst_tick{};

    template<class HeldBytesFn>
    void SelectBirthRows(std::array<std::size_t, 3> &rows,
        std::span<vfx_retained_size::BirthRequest> requests, std::vector<std::size_t> &order,
        HeldBytesFn &&measure_held_bytes, DecodedVfxState &prepared_births,
        std::vector<VfxFlashSpawnInput> &visible_flashes,
        std::size_t required_suffix, std::size_t budget_bytes)
    {
        vfx_birth_admission::ReserveAndSelectBirthRows(rows, requests, order,
            measure_held_bytes, prepared_births.particle_spawns, decoded.particle_spawns,
            native_batch.particle_spawns, prepared_births.effect_lines, decoded.effect_lines,
            native_batch.effect_lines, prepared_births.flashes, decoded.flashes,
            visible_flashes, required_suffix, budget_bytes);
    }

  private:
    std::vector<PersistentBurstKey> seen_persistent_bursts_;
    std::vector<PersistentBurstKey> new_persistent_burst_keys_;
    std::vector<PersistentBurstKey> merged_persistent_burst_keys_;

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
    NativeEmissionBatch *PrepareNativeBatch(Tick target, bool stateful_step_needed,
        DecodedVfxState *reusable_storage = nullptr)
    {
        if (reusable_storage && (std::addressof(*reusable_storage) == std::addressof(decoded) ||
            !reusable_storage->particle_spawns.empty() || !reusable_storage->effect_lines.empty()))
            throw std::invalid_argument("native freeze scratch must be independent and empty");
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
        const auto prepare_capacity = [](auto &owned, const auto &pending, std::size_t count, auto *scratch) {
            const auto copied = std::min(count, pending.size() - count);
            if (owned.capacity() >= copied) return;
            // Admission credits the larger empty allocation before further growth.
            if (scratch && scratch->capacity() > owned.capacity()) owned.swap(*scratch);
            owned.reserve(copied);
        };
        prepare_capacity(native_batch.particle_spawns, decoded.particle_spawns, particle_count,
            reusable_storage ? &reusable_storage->particle_spawns : nullptr);
        prepare_capacity(native_batch.effect_lines, decoded.effect_lines, line_count,
            reusable_storage ? &reusable_storage->effect_lines : nullptr);
        native_batch.identity = {session_id, target, next_native_batch_sequence++, particle_reset_pending};
        const auto freeze = [](auto &owned, auto &pending, std::size_t count) {
            using Row = typename std::remove_reference_t<decltype(pending)>::value_type;
            static_assert(std::is_nothrow_copy_constructible_v<Row> && std::is_nothrow_copy_assignable_v<Row>);
            if (!count) return;
            if (count == pending.size())
            {
                owned.swap(pending);
                return;
            }
            if (count > pending.size() - count)
            {
                // Keep the large due prefix in place; copy only its smaller future tail.
                owned.swap(pending);
                pending.assign(owned.begin() + count, owned.end());
                owned.resize(count);
            }
            else
            {
                owned.assign(pending.begin(), pending.begin() + count);
                pending.erase(pending.begin(), pending.begin() + count);
            }
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
        if (!stable_id) return false;
        const PersistentBurstKey key{stable_id, visual_kind};
        const auto position = std::lower_bound(seen_persistent_bursts_.begin(),
            seen_persistent_bursts_.end(), key);
        if (position != seen_persistent_bursts_.end() && *position == key) return false;
        const auto index = static_cast<std::size_t>(position - seen_persistent_bursts_.begin());
        ReserveTo(seen_persistent_bursts_, CheckedAdd(seen_persistent_bursts_.size(), 1));
        seen_persistent_bursts_.insert(seen_persistent_bursts_.begin() + index, key);
        return true;
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
            if (source.empty())
            {
                if (destination.empty() && source.capacity() > destination.capacity())
                    destination.swap(source);
                return;
            }
            if (destination.empty()) destination.swap(source);
            else for (const auto &value : source) destination.push_back(value);
            source.clear();
        };
        publish(particles, prepared.particle_spawns);
        publish(lines, prepared.effect_lines);
        publish(flashes, prepared.flashes);
        return true;
    }

    void ReserveSurfaceCaptures(std::size_t additional_profile_rows)
    {
        const auto surface = ActiveEffectStore::Profile::EntitySurface;
        const auto capture_rows = CheckedAdd(active_effects.ProfileEvents(surface).size(),
            additional_profile_rows);
        // Every queued surface event can transfer to one retained capture after CPU acceptance.
        vfx_retained_size::ReserveAppend(decoded.entity_surface_captures, capture_rows);
        static_assert(std::is_nothrow_move_constructible_v<RetainedVfxEntitySurfaceCapture>);
    }

    [[nodiscard]] bool CommitPreparedActivation(DecodedVfxState &prepared_births,
        ActiveEffectStore::PreparedAppendBatch &prepared_profiles, bool commit_profiles)
    {
        // Render-thread ownership keeps profile preflight valid through non-allocating publication.
        if (std::addressof(prepared_births) == std::addressof(decoded) ||
            (commit_profiles && !active_effects.CanCommitAppendBatch(prepared_profiles)))
            return false;
        ReserveSurfaceCaptures(commit_profiles ?
            prepared_profiles.AdditionalProfileRows(ActiveEffectStore::Profile::EntitySurface) : 0);
        if (!AppendPreparedBirths(prepared_births)) return false;
        if (commit_profiles && !active_effects.CommitPreparedAppendBatch(prepared_profiles)) return false;
        raw_events.AcknowledgeActivation();
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
        seen_persistent_bursts_.clear();
        new_persistent_burst_keys_.clear();
        merged_persistent_burst_keys_.clear();
        last_status_burst_tick = 0;
        has_status_burst_tick = false;
        raw_events.DiscardBeforeSession(session_id);
        return true;
    }
};
} // namespace hs::runtime_detail
