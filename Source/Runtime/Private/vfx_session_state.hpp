#pragma once

#include <hs/core/fixed_step_clock.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/game_domain/game_types.hpp>
#include "vfx_typed_entity_surface_commands.hpp"
#include "vfx_typed_mote_commands.hpp"
#include "vfx_typed_screen_commands.hpp"
#include "vfx_event_view.hpp"
#include <hs/render_data/render_frame_input.hpp>
#include <hs/render_data/vfx_spawn_command.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hs::runtime_detail
{
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

    [[nodiscard]] Row Intern(const VfxEventInput &input)
    {
        const EventKey key = KeyFor(input);
        if (const auto found = event_rows_.find(key); found != event_rows_.end())
            return found->second;
        const auto row = static_cast<Row>(rows_.size());
        rows_.push_back(input);
        profile_masks_.push_back(0);
        binding_indices_.push_back(VfxEventView::kNoBinding);
        event_rows_.try_emplace(key, row);
        membership_dirty_ = true;
        return row;
    }

    [[nodiscard]] bool Add(Profile profile, Row row)
    {
        if (row >= rows_.size()) return false;
        const auto bit = ProfileBit(profile);
        if (profile_masks_[row] & bit) return false;
        profiles_[static_cast<std::size_t>(profile)].push_back(row);
        profile_masks_[row] |= bit;
        membership_dirty_ = true;
        return true;
    }

    void Append(Profile profile, const VfxEventInput &input)
    {
        (void)Add(profile, Intern(input));
    }

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
        std::erase_if(refs, [&](Row row) {
            if (rows_[row].geometry_owner_id == 0) return false;
            auto &binding = binding_indices_[row];
            if (binding == VfxEventView::kNoBinding)
            {
                if (bound_row_count_ == bound_rows_.size())
                    bound_rows_.push_back(rows_[row]);
                else
                    bound_rows_[bound_row_count_] = rows_[row];
                if (refresh(bound_rows_[bound_row_count_]))
                {
                    binding = static_cast<Row>(bound_row_count_++);
                }
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
            if (!rebind(rows_[i])) live[i] = 0;
        for (auto &profile : profiles_)
            std::erase_if(profile, [&](Row row) { return row >= live.size() || !live[row]; });
        for (std::size_t i = 0; i < rows_.size(); ++i)
            if (!live[i]) profile_masks_[i] = 0;
        std::erase_if(captures, [&](const auto &capture) {
            return capture.row >= live.size() || !live[capture.row];
        });
        membership_dirty_ = index_dirty_ = true;
        BeginFrameBindings();
        Compact(captures);
    }

    void RebindProfile(Profile profile, std::span<const VfxEventInput> rebound)
    {
        auto &refs = profiles_[static_cast<std::size_t>(profile)];
        std::vector<Row> surviving;
        surviving.reserve(rebound.size());
        const auto bit = ProfileBit(profile);
        for (const auto row : refs) profile_masks_[row] &= ~bit;
        for (const auto &input : rebound)
            for (const auto row : refs)
                if (SameEpisode(rows_[row], input))
                {
                    rows_[row] = input;
                    if (!(profile_masks_[row] & bit))
                    {
                        surviving.push_back(row);
                        profile_masks_[row] |= bit;
                    }
                    break;
                }
        refs.swap(surviving);
        membership_dirty_ = true;
        BeginFrameBindings();
        RebuildIndex();
    }

    void Compact(std::span<RetainedVfxEntitySurfaceCapture> captures = {})
    {
        if (!membership_dirty_ && !capture_only_rows_) return;
        live_rows_.resize(rows_.size());
        for (std::size_t i = 0; i < rows_.size(); ++i) live_rows_[i] = profile_masks_[i] != 0;
        capture_only_rows_ = false;
        for (const auto &capture : captures)
            if (capture.row < live_rows_.size())
            {
                live_rows_[capture.row] = 1;
                capture_only_rows_ |= profile_masks_[capture.row] == 0;
            }
        membership_dirty_ = false;
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
                    rows_[next] = std::move(rows_[i]);
                    profile_masks_[next] = profile_masks_[i];
                    if (!index_dirty_) event_rows_.at(key) = next;
                }
                ++next;
            }
            else if (!index_dirty_) event_rows_.erase(key);
        }
        for (auto &profile : profiles_) for (auto &row : profile) row = scratch_rows_[row];
        for (auto &capture : captures) capture.row = scratch_rows_[capture.row];
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
        bound_row_count_ = 0;
        scratch_rows_.clear(); live_rows_.clear();
        for (auto &profile : profiles_) profile.clear();
        membership_dirty_ = capture_only_rows_ = index_dirty_ = false;
    }
    void Swap(ActiveEffectStore &other) noexcept
    {
        rows_.swap(other.rows_); event_rows_.swap(other.event_rows_);
        profiles_.swap(other.profiles_);
        profile_masks_.swap(other.profile_masks_);
        binding_indices_.swap(other.binding_indices_);
        bound_rows_.swap(other.bound_rows_); bound_row_ids_.swap(other.bound_row_ids_);
        std::swap(bound_row_count_, other.bound_row_count_);
        scratch_rows_.swap(other.scratch_rows_); live_rows_.swap(other.live_rows_);
        std::swap(membership_dirty_, other.membership_dirty_);
        std::swap(capture_only_rows_, other.capture_only_rows_);
        std::swap(index_dirty_, other.index_dirty_);
    }
    [[nodiscard]] std::size_t Size() const noexcept { return rows_.size(); }

  private:
    struct EventKey
    {
        std::uint64_t sequence{}, episode{}, owner{}, geometry_effect{};
        std::uint64_t event_tick{};
        std::uint32_t effect_handle{};
        std::uint32_t geometry_sector_index{};
        bool operator==(const EventKey &) const = default;
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
        profile_masks_[row] &= ~ProfileBit(profile);
        membership_dirty_ = true;
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
    std::size_t bound_row_count_{};
    bool membership_dirty_{}, capture_only_rows_{}, index_dirty_{};
};

struct DecodedVfxState
{
    std::vector<ParticleSpawnCommand> particle_spawns;
    std::vector<VfxLineSpawnCommand> effect_lines;
    std::vector<VfxFlashSpawnInput> flashes;
    std::vector<RetainedVfxEntitySurfaceCapture> entity_surface_captures;

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

        [[nodiscard]] bool HasCapacity() const noexcept
        {
            return pending.size() + in_flight.entries.size() < kCapacity;
        }

        [[nodiscard]] bool Enqueue(PresentationEvent event)
        {
            if (!HasCapacity()) return false;
            pending.push_back({std::move(event)});
            return true;
        }

        [[nodiscard]] const ReadyBatch &Prepare(Tick tick, std::uint64_t session)
        {
            if (!in_flight.entries.empty()) return in_flight;
            while (!pending.empty())
            {
                const auto &next = pending.front().event;
                if (next.session_id > session ||
                    (next.session_id == session && next.tick > tick)) break;
                auto entry = std::move(pending.front());
                pending.pop_front();
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
            while (!pending.empty() && pending.front().event.session_id < session)
                pending.pop_front();
            std::erase_if(in_flight.entries, [session](const Entry &entry) {
                return entry.event.session_id < session;
            });
        }

        std::deque<Entry> pending;
        ReadyBatch in_flight;
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
    std::uint64_t session_id{};
    VfxLowHealthPulseState low_health_pulse;
    std::optional<TerminalPresentationClock> terminal_clock;
    RawEventScheduler raw_events;
    VfxPersistentMoteBirthWindow persistent_mote_births;
    ActiveEffectStore active_effects;
    DecodedVfxState decoded;
    std::unordered_set<PersistentBurstKey, PersistentBurstHash> seen_persistent_bursts;
    NativeEmissionBatch native_batch;
    bool native_batch_active{};
    Tick last_committed_stateful_target{};
    std::uint64_t next_native_batch_sequence{1};
    bool has_committed_stateful_target{};
    bool particle_reset_pending{};

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

        native_batch.identity = {session_id, target, next_native_batch_sequence++, particle_reset_pending};
        native_batch.particle_spawns.clear();
        native_batch.effect_lines.clear();
        native_batch.particle_spawns.reserve(particle_count);
        native_batch.effect_lines.reserve(line_count);
        for (std::size_t i = 0; i < particle_count; ++i)
            native_batch.particle_spawns.push_back(std::move(decoded.particle_spawns[i]));
        for (std::size_t i = 0; i < line_count; ++i)
            native_batch.effect_lines.push_back(std::move(decoded.effect_lines[i]));
        decoded.particle_spawns.erase(decoded.particle_spawns.begin(), decoded.particle_spawns.begin() + particle_count);
        decoded.effect_lines.erase(decoded.effect_lines.begin(), decoded.effect_lines.begin() + line_count);
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
        persistent_mote_births.Clear();
        active_effects.Clear();
        ClearDecoded();
        DiscardNativeBatch();
        last_committed_stateful_target = 0;
        next_native_batch_sequence = 1;
        has_committed_stateful_target = false;
        particle_reset_pending = false;
        seen_persistent_bursts.clear();
        raw_events.DiscardBeforeSession(session_id);
        return true;
    }
};
} // namespace hs::runtime_detail
