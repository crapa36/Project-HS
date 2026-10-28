#pragma once
#include "navigation_grid.hpp"
#include "status_state.hpp"
#include <hs/game_domain/game_types.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace hs::gameplay_detail
{
inline constexpr std::uint8_t kNoTelemetrySource = 0xFF;
struct StatusVisualEpisode { Tick started{}; std::uint64_t generation{}; };

// Each column owns an access group, never an AoS mirror.
struct EnemyMotion
{
    Float2 position{};
    Float2 previous_position{};
    DiscMotion2D tick_motion;
    Float2 velocity{};
    Float2 displacement_per_tick{};
    Tick displacement_ticks{};
};

struct EnemyLife
{
    EnemyKind kind{EnemyKind::Melee};
    std::optional<BossKind> boss;
    Tick spawned_tick{};
    std::int32_t health{};
    std::int32_t max_health{};
    bool dead{};
};

struct EnemyAction
{
    float warning_extent{};
    Tick warning_ticks{};
    Tick next_attack{};
    Tick attack_resolve{};
    Tick boss_action_started{};
    Tick boss_action_until{};
    bool boss_action_recoil{};
    std::uint64_t attack_cast_id{};
    Float2 locked_aim{};
    Sequence warning_sequence{};
    Tick pattern_ready{};
    std::uint8_t last_pattern{255};
    std::uint8_t repeat_count{};
    std::uint8_t final_phase{1};
    Tick invulnerable_until{};
    Tick phase2_started{};
    Tick dash_started{};
    Float2 dash_origin{};
    Tick dash_until{};
    std::int32_t dash_damage{};
    bool dash_hit{};
    std::uint8_t phase_pattern_count{};
    bool attacking{};
};

struct EnemyColdState
{
    std::uint64_t random_key{};
    std::int32_t damage{};
    float move_speed{};
    float attack_range{};
    Tick attack_cooldown_ticks{};
    std::array<StatusVisualEpisode, 4> status_visual_episodes{};
    SkillKind last_damage_skill{SkillKind::Count};
    EffectOrigin last_damage_origin{EffectOrigin::Original};
    std::uint64_t last_damage_cast{};
    std::uint8_t last_damage_upgrade{kNoTelemetrySource};
    std::uint8_t last_damage_relic{kNoTelemetrySource};
    SkillKind marked_by_skill{SkillKind::Count};
    float marked_damage_coefficient{};
    Tick mark_expires{};
    Tick bleed_burn_ready{};
    Tick different_skill_ready{};
    Tick slow_synergy_ready{};
    Tick boss_pressure_ready{};
    SkillKind last_active_hit{SkillKind::Count};
    Tick last_active_hit_tick{};
};

struct ProjectileMotion
{
    Float2 position{};
    Float2 previous_position{};
    Float2 velocity{};
    float remaining_range{};
    float radius{};
};

struct ProjectileCombat
{
    bool player_owned{};
    Tick spawned_tick{};
    std::int32_t damage{};
    SkillKind skill{SkillKind::BasicAttack};
    std::uint8_t upgrade_mask{};
    EffectOrigin origin{EffectOrigin::Original};
    std::uint64_t cast_id{};
    std::uint8_t source_upgrade{kNoTelemetrySource};
    std::uint8_t source_relic{kNoTelemetrySource};
    std::uint8_t source_enemy{kNoTelemetrySource};
    std::uint16_t pierce_remaining{};
    std::uint8_t bounce_remaining{};
    std::uint32_t hit_count{};
    std::vector<std::uint64_t> hit_ids;
    float explosion_radius{};
    std::int32_t explosion_damage{};
    std::uint8_t explosion_source_upgrade{kNoTelemetrySource};
    std::uint8_t bleed_stacks{};
    bool burn{};
    bool slow{};
    bool returning{};
    bool homing{};
    std::uint64_t homing_target{};
    bool full_charge{};
    float charge_ratio{};
    float carried_burn_attack{};
    Tick carried_burn_expires{};
    bool dead{};
};

struct ProjectileEpisodes
{
    std::vector<Tick> bleed_extend_ticks;
    Tick return_started_tick{};
    Float2 return_start_position{};
};

struct ActorColumnMutation
{
    std::uint64_t generation{};
#ifndef NDEBUG
    mutable std::size_t active_rows{};
#endif
    void RequireUnborrowed() const noexcept
    {
#ifndef NDEBUG
        assert(active_rows == 0);
#endif
    }
    void Advance()
    {
        RequireUnborrowed();
        if (generation == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Actor structural generation exhausted.");
        ++generation;
    }
};

class ActorRowLease
{
  public:
    explicit ActorRowLease(const ActorColumnMutation &state) noexcept
#ifndef NDEBUG
        : state_(&state)
#endif
    {
#ifndef NDEBUG
        ++state_->active_rows;
#else
        (void)state;
#endif
    }
    ActorRowLease(const ActorRowLease &other) noexcept
#ifndef NDEBUG
        : state_(other.state_)
#endif
    {
#ifndef NDEBUG
        ++state_->active_rows;
#else
        (void)other;
#endif
    }
    ~ActorRowLease()
    {
#ifndef NDEBUG
        assert(state_->active_rows != 0);
        --state_->active_rows;
#endif
    }
  private:
#ifndef NDEBUG
    const ActorColumnMutation *state_;
#endif
};

template <bool Constant, typename T>
using ActorGroupReference = std::conditional_t<Constant, const T, T> &;

template <bool Constant> struct BasicEnemyRow
{
    ActorGroupReference<Constant, EntityId> id;
    ActorGroupReference<Constant, EnemyMotion> motion;
    ActorGroupReference<Constant, EnemyLife> life;
    ActorGroupReference<Constant, EnemyAction> action;
    ActorGroupReference<Constant, EnemyColdState> cold;
    ActorGroupReference<Constant, StatusState> status;
    ActorRowLease scope;
    BasicEnemyRow(ActorGroupReference<Constant, EntityId> id,
        ActorGroupReference<Constant, EnemyMotion> motion,
        ActorGroupReference<Constant, EnemyLife> life,
        ActorGroupReference<Constant, EnemyAction> action,
        ActorGroupReference<Constant, EnemyColdState> cold,
        ActorGroupReference<Constant, StatusState> status,
        const ActorColumnMutation &state) noexcept
        : id(id), motion(motion), life(life), action(action), cold(cold), status(status), scope(state) {}
    template <bool Other> requires (Constant && !Other)
    BasicEnemyRow(const BasicEnemyRow<Other> &other) noexcept
        : id(other.id), motion(other.motion), life(other.life), action(other.action), cold(other.cold), status(other.status), scope(other.scope) {}
};
using EnemyRow = BasicEnemyRow<false>;
using ConstEnemyRow = BasicEnemyRow<true>;

// Owning unpublished spawn preparation only, never retained for execution.
struct PreparedEnemy
{
    EntityId id{};
    EnemyMotion motion{};
    EnemyLife life{};
    EnemyAction action{};
    EnemyColdState cold{};
    StatusState status{};
};

template <bool Constant> struct BasicProjectileRow
{
    ActorGroupReference<Constant, EntityId> id;
    ActorGroupReference<Constant, ProjectileMotion> motion;
    ActorGroupReference<Constant, ProjectileCombat> combat;
    ActorGroupReference<Constant, ProjectileEpisodes> episodes;
    ActorRowLease scope;
    BasicProjectileRow(ActorGroupReference<Constant, EntityId> id,
        ActorGroupReference<Constant, ProjectileMotion> motion,
        ActorGroupReference<Constant, ProjectileCombat> combat,
        ActorGroupReference<Constant, ProjectileEpisodes> episodes,
        const ActorColumnMutation &state) noexcept
        : id(id), motion(motion), combat(combat), episodes(episodes), scope(state) {}
    template <bool Other> requires (Constant && !Other)
    BasicProjectileRow(const BasicProjectileRow<Other> &other) noexcept
        : id(other.id), motion(other.motion), combat(other.combat), episodes(other.episodes), scope(other.scope) {}
};
using ProjectileRow = BasicProjectileRow<false>;
using ConstProjectileRow = BasicProjectileRow<true>;

// Owning unpublished spawn preparation only, never retained for execution.
struct PreparedProjectile
{
    EntityId id{};
    ProjectileMotion motion{};
    ProjectileCombat combat{};
    ProjectileEpisodes episodes{};
};

template <typename Store> class ActorRowIterator
{
  public:
    using difference_type = std::ptrdiff_t;
    using value_type = decltype(std::declval<Store &>()[std::size_t{}]);
    using iterator_concept = std::input_iterator_tag;
    using iterator_category = std::input_iterator_tag;
    ActorRowIterator() = default;
    ActorRowIterator(Store &store, std::size_t row) : store_(&store), row_(row) {}
    value_type operator*() const { return (*store_)[row_]; }
    ActorRowIterator &operator++() { ++row_; return *this; }
    ActorRowIterator operator++(int) { auto previous = *this; ++*this; return previous; }
    friend bool operator==(const ActorRowIterator &, const ActorRowIterator &) = default;
  private:
    Store *store_{};
    std::size_t row_{};
};

class EnemyStore
{
  public:
    EnemyStore() = default;
    EnemyStore(const EnemyStore &) = delete;
    EnemyStore &operator=(const EnemyStore &) = delete;
    EnemyStore(EnemyStore &&) = delete;
    EnemyStore &operator=(EnemyStore &&) = delete;
    std::vector<EntityId> ids;
    std::vector<EnemyMotion> motion;
    std::vector<EnemyLife> life;
    std::vector<EnemyAction> action;
    std::vector<EnemyColdState> cold;
    std::vector<StatusState> status;
    std::size_t size() const noexcept { return ids.size(); }
    bool empty() const noexcept { return ids.empty(); }
    bool Consistent() const noexcept { return motion.size() == ids.size() && life.size() == ids.size() && action.size() == ids.size() && cold.size() == ids.size() && status.size() == ids.size(); }
    std::size_t capacity() const noexcept { return std::min({ids.capacity(), motion.capacity(), life.capacity(), action.capacity(), cold.capacity(), status.capacity()}); }
    std::uint64_t StructuralGeneration() const noexcept { return mutation_.generation; }
    EnemyRow operator[](std::size_t row) noexcept
    {
        assert(Consistent() && row < size());
        return {ids[row], motion[row], life[row], action[row], cold[row], status[row], mutation_};
    }
    EnemyRow back() noexcept { return (*this)[size() - 1]; }
    auto begin() noexcept { return ActorRowIterator<EnemyStore>(*this, 0); }
    auto end() noexcept { return ActorRowIterator<EnemyStore>(*this, size()); }
    ConstEnemyRow operator[](std::size_t row) const noexcept
    {
        assert(Consistent() && row < size());
        return {ids[row], motion[row], life[row], action[row], cold[row], status[row], mutation_};
    }
    ConstEnemyRow back() const noexcept { return (*this)[size() - 1]; }
    auto begin() const noexcept { return ActorRowIterator<const EnemyStore>(*this, 0); }
    auto end() const noexcept { return ActorRowIterator<const EnemyStore>(*this, size()); }
    void reserve(std::size_t count)
    {
        mutation_.RequireUnborrowed();
        if (count > MaxSize()) throw std::length_error("Actor column capacity overflow.");
        ids.reserve(count);
        motion.reserve(count);
        life.reserve(count);
        action.reserve(count);
        cold.reserve(count);
        status.reserve(count);
    }
    void push_back(PreparedEnemy value)
    {
        mutation_.RequireUnborrowed();
        if (size() == MaxSize()) throw std::length_error("Actor count overflow.");
        EnsureCapacity(size() + 1);
        assert(value.id.value != 0 && (empty() || ids.back().value < value.id.value));
        mutation_.Advance();
        static_assert(std::is_nothrow_move_constructible_v<EntityId>);
        ids.push_back(std::move(value.id));
        static_assert(std::is_nothrow_move_constructible_v<EnemyMotion>);
        motion.push_back(std::move(value.motion));
        static_assert(std::is_nothrow_move_constructible_v<EnemyLife>);
        life.push_back(std::move(value.life));
        static_assert(std::is_nothrow_move_constructible_v<EnemyAction>);
        action.push_back(std::move(value.action));
        static_assert(std::is_nothrow_move_constructible_v<EnemyColdState>);
        cold.push_back(std::move(value.cold));
        static_assert(std::is_nothrow_move_constructible_v<StatusState>);
        status.push_back(std::move(value.status));
        assert(Consistent());
    }
    void AppendFrom(EnemyStore &source)
    {
        assert(this != &source);
        mutation_.RequireUnborrowed(); source.mutation_.RequireUnborrowed();
        if (source.empty()) return;
        if (source.size() > MaxSize() - size()) throw std::length_error("Actor append overflow.");
        EnsureCapacity(size() + source.size());
        mutation_.Advance(); source.mutation_.Advance();
        assert(empty() || ids.back().value < source.ids.front().value);
        for (std::size_t row = 0; row < source.size(); ++row)
        {
            ids.push_back(std::move(source.ids[row]));
            motion.push_back(std::move(source.motion[row]));
            life.push_back(std::move(source.life[row]));
            action.push_back(std::move(source.action[row]));
            cold.push_back(std::move(source.cold[row]));
            status.push_back(std::move(source.status[row]));
        }
        source.ids.clear();
        source.motion.clear();
        source.life.clear();
        source.action.clear();
        source.cold.clear();
        source.status.clear();
        assert(Consistent() && source.Consistent());
    }
    template <typename Predicate> std::size_t EraseIf(Predicate predicate)
    {
        mutation_.RequireUnborrowed();
        const auto original = size();
        std::size_t write{};
        bool generation_advanced{};
        for (std::size_t read = 0; read < original; ++read)
        {
            bool remove;
            { auto row = (*this)[read]; remove = predicate(row); }
            if (remove) continue;
            if (write != read)
            {
                if (!generation_advanced)
                {
                    mutation_.Advance();
                    generation_advanced = true;
                }
                static_assert(std::is_nothrow_move_assignable_v<EntityId>);
                ids[write] = std::move(ids[read]);
                static_assert(std::is_nothrow_move_assignable_v<EnemyMotion>);
                motion[write] = std::move(motion[read]);
                static_assert(std::is_nothrow_move_assignable_v<EnemyLife>);
                life[write] = std::move(life[read]);
                static_assert(std::is_nothrow_move_assignable_v<EnemyAction>);
                action[write] = std::move(action[read]);
                static_assert(std::is_nothrow_move_assignable_v<EnemyColdState>);
                cold[write] = std::move(cold[read]);
                static_assert(std::is_nothrow_move_assignable_v<StatusState>);
                status[write] = std::move(status[read]);
            }
            ++write;
        }
        if (write != original && !generation_advanced) mutation_.Advance();
        ids.resize(write);
        motion.resize(write);
        life.resize(write);
        action.resize(write);
        cold.resize(write);
        status.resize(write);
        assert(Consistent());
        return original - write;
    }
    void clear()
    {
        mutation_.RequireUnborrowed();
        if (empty()) return;
        mutation_.Advance();
        ids.clear();
        motion.clear();
        life.clear();
        action.clear();
        cold.clear();
        status.clear();
    }
  private:
    std::size_t MaxSize() const noexcept { return std::min({ids.max_size(), motion.max_size(), life.max_size(), action.max_size(), cold.max_size(), status.max_size()}); }
    void EnsureCapacity(std::size_t count)
    {
        if (count <= capacity()) return;
        const auto previous = capacity();
        const auto grown = previous <= MaxSize() / 2 ? previous * 2 : MaxSize();
        reserve(std::max(count, grown));
    }
    mutable ActorColumnMutation mutation_;
};

class ProjectileStore
{
  public:
    ProjectileStore() = default;
    ProjectileStore(const ProjectileStore &) = delete;
    ProjectileStore &operator=(const ProjectileStore &) = delete;
    ProjectileStore(ProjectileStore &&) = delete;
    ProjectileStore &operator=(ProjectileStore &&) = delete;
    std::vector<EntityId> ids;
    std::vector<ProjectileMotion> motion;
    std::vector<ProjectileCombat> combat;
    std::vector<ProjectileEpisodes> episodes;
    std::size_t size() const noexcept { return ids.size(); }
    bool empty() const noexcept { return ids.empty(); }
    bool Consistent() const noexcept { return motion.size() == ids.size() && combat.size() == ids.size() && episodes.size() == ids.size(); }
    std::size_t capacity() const noexcept { return std::min({ids.capacity(), motion.capacity(), combat.capacity(), episodes.capacity()}); }
    std::uint64_t StructuralGeneration() const noexcept { return mutation_.generation; }
    ProjectileRow operator[](std::size_t row) noexcept
    {
        assert(Consistent() && row < size());
        return {ids[row], motion[row], combat[row], episodes[row], mutation_};
    }
    ProjectileRow back() noexcept { return (*this)[size() - 1]; }
    auto begin() noexcept { return ActorRowIterator<ProjectileStore>(*this, 0); }
    auto end() noexcept { return ActorRowIterator<ProjectileStore>(*this, size()); }
    ConstProjectileRow operator[](std::size_t row) const noexcept
    {
        assert(Consistent() && row < size());
        return {ids[row], motion[row], combat[row], episodes[row], mutation_};
    }
    ConstProjectileRow back() const noexcept { return (*this)[size() - 1]; }
    auto begin() const noexcept { return ActorRowIterator<const ProjectileStore>(*this, 0); }
    auto end() const noexcept { return ActorRowIterator<const ProjectileStore>(*this, size()); }
    void reserve(std::size_t count)
    {
        mutation_.RequireUnborrowed();
        if (count > MaxSize()) throw std::length_error("Actor column capacity overflow.");
        ids.reserve(count);
        motion.reserve(count);
        combat.reserve(count);
        episodes.reserve(count);
    }
    void push_back(PreparedProjectile value)
    {
        mutation_.RequireUnborrowed();
        if (size() == MaxSize()) throw std::length_error("Actor count overflow.");
        EnsureCapacity(size() + 1);
        assert(value.id.value != 0);
        mutation_.Advance();
        static_assert(std::is_nothrow_move_constructible_v<EntityId>);
        ids.push_back(std::move(value.id));
        static_assert(std::is_nothrow_move_constructible_v<ProjectileMotion>);
        motion.push_back(std::move(value.motion));
        static_assert(std::is_nothrow_move_constructible_v<ProjectileCombat>);
        combat.push_back(std::move(value.combat));
        static_assert(std::is_nothrow_move_constructible_v<ProjectileEpisodes>);
        episodes.push_back(std::move(value.episodes));
        assert(Consistent());
    }
    void AppendFrom(ProjectileStore &source)
    {
        assert(this != &source);
        mutation_.RequireUnborrowed(); source.mutation_.RequireUnborrowed();
        if (source.empty()) return;
        if (source.size() > MaxSize() - size()) throw std::length_error("Actor append overflow.");
        EnsureCapacity(size() + source.size());
        mutation_.Advance(); source.mutation_.Advance();
        for (std::size_t row = 0; row < source.size(); ++row)
        {
            ids.push_back(std::move(source.ids[row]));
            motion.push_back(std::move(source.motion[row]));
            combat.push_back(std::move(source.combat[row]));
            episodes.push_back(std::move(source.episodes[row]));
        }
        source.ids.clear();
        source.motion.clear();
        source.combat.clear();
        source.episodes.clear();
        assert(Consistent() && source.Consistent());
    }
    template <typename Predicate> std::size_t EraseIf(Predicate predicate)
    {
        mutation_.RequireUnborrowed();
        const auto original = size();
        std::size_t write{};
        bool generation_advanced{};
        for (std::size_t read = 0; read < original; ++read)
        {
            bool remove;
            { auto row = (*this)[read]; remove = predicate(row); }
            if (remove) continue;
            if (write != read)
            {
                if (!generation_advanced)
                {
                    mutation_.Advance();
                    generation_advanced = true;
                }
                static_assert(std::is_nothrow_move_assignable_v<EntityId>);
                ids[write] = std::move(ids[read]);
                static_assert(std::is_nothrow_move_assignable_v<ProjectileMotion>);
                motion[write] = std::move(motion[read]);
                static_assert(std::is_nothrow_move_assignable_v<ProjectileCombat>);
                combat[write] = std::move(combat[read]);
                static_assert(std::is_nothrow_move_assignable_v<ProjectileEpisodes>);
                episodes[write] = std::move(episodes[read]);
            }
            ++write;
        }
        if (write != original && !generation_advanced) mutation_.Advance();
        ids.resize(write);
        motion.resize(write);
        combat.resize(write);
        episodes.resize(write);
        assert(Consistent());
        return original - write;
    }
    void clear()
    {
        mutation_.RequireUnborrowed();
        if (empty()) return;
        mutation_.Advance();
        ids.clear();
        motion.clear();
        combat.clear();
        episodes.clear();
    }
  private:
    std::size_t MaxSize() const noexcept { return std::min({ids.max_size(), motion.max_size(), combat.max_size(), episodes.max_size()}); }
    void EnsureCapacity(std::size_t count)
    {
        if (count <= capacity()) return;
        const auto previous = capacity();
        const auto grown = previous <= MaxSize() / 2 ? previous * 2 : MaxSize();
        reserve(std::max(count, grown));
    }
    mutable ActorColumnMutation mutation_;
};

} // namespace hs::gameplay_detail
