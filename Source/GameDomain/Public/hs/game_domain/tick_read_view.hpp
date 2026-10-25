#pragma once

#include <hs/game_domain/game_read_model.hpp>
#include <hs/game_domain/domain_signal.hpp>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

namespace hs
{
// A small semantic value; its history points directly into the scoped actor owner.
struct ProjectileReadValue
{
    EntityId id{};
    bool player_owned{};
    Float2 position{};
    Float2 velocity{};
    Tick spawned_tick{};
    SkillKind skill{SkillKind::BasicAttack};
    bool dead{};
    float radius{};
    float charge_ratio{};
    EffectOrigin origin{EffectOrigin::Original};
    std::uint8_t source_upgrade{0xFF};
    Float2 previous_position{};
    float remaining_range{};
    std::uint64_t cast_id{};
    std::uint8_t source_relic{0xFF};
    std::uint8_t source_enemy{0xFF};
    bool returning{};
    bool homing{};
    Tick return_started_tick{};
    Float2 return_start_position{};
    std::uint8_t upgrade_mask{};
    std::span<const Tick> bleed_extend_ticks;
};

// Concrete semantic readers use fixed accessors into Gameplay's actual private groups.
// They own no actor array and expose no private status/attribution container.
template <typename Value> class TickRows
{
  public:
    using Read = bool (*)(const void *, std::size_t, Value &);
    TickRows() = default;
    TickRows(const void *owner, std::size_t stored, std::size_t visible, Read read) noexcept
        : owner_(owner), stored_(stored), visible_(visible), read_(read) {}
    class Iterator
    {
      public:
        using value_type = Value;
        using difference_type = std::ptrdiff_t;
        using iterator_concept = std::input_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using reference = const Value &;
        using pointer = const Value *;
        Iterator() = default;
        Iterator(const TickRows &rows, std::size_t index) : rows_(&rows), index_(index) { Seek(); }
        reference operator*() const { assert(value_); return *value_; }
        pointer operator->() const { assert(value_); return &*value_; }
        Iterator &operator++() { ++index_; Seek(); return *this; }
        Iterator operator++(int) { auto old = *this; ++*this; return old; }
        friend bool operator==(const Iterator &a, const Iterator &b) noexcept
        { return a.rows_ == b.rows_ && a.index_ == b.index_; }
      private:
        void Seek()
        {
            value_.reset();
            while (rows_ && index_ < rows_->stored_)
            {
                value_.emplace();
                if (rows_->read_(rows_->owner_, index_, *value_)) return;
                value_.reset(); ++index_;
            }
        }
        const TickRows *rows_{};
        std::size_t index_{};
        std::optional<Value> value_;
    };
    Iterator begin() const { return {*this, 0}; }
    Iterator end() const { return {*this, stored_}; }
    std::size_t size() const noexcept { return visible_; }
    bool empty() const noexcept { return visible_ == 0; }
    Value operator[](std::size_t ordinal) const
    {
        auto row = begin();
        while (ordinal-- && row != end()) ++row;
        if (row == end()) throw std::out_of_range("Tick read row is outside its scope range.");
        return *row;
    }
  private:
    const void *owner_{};
    std::size_t stored_{}, visible_{};
    Read read_{};
};

using EnemyReadColumns = TickRows<EnemyView>;
using ProjectileReadColumns = TickRows<ProjectileReadValue>;

struct SessionSummaryReadView
{
    const std::uint64_t &direct_damage, &derived_damage, &damage_over_time;
    const decltype(SessionSummaryView::upgrade_damage) &upgrade_damage;
    const decltype(SessionSummaryView::relic_damage) &relic_damage;
    const decltype(SessionSummaryView::relic_triggers) &relic_triggers;
    const decltype(SessionSummaryView::relic_kills) &relic_kills;
    const decltype(SessionSummaryView::relic_effects) &relic_effects;
};

// Every reference/range expires with GameSimulation's scoped read lease.
// Only immutable content may be leased across a thread boundary.
struct TickReadView
{
    Tick tick{};
    GameplayChecksum checksum{};
    std::uint64_t seed{};
    const SessionProbe &session;
    const PlayerView &player;
    EnemyReadColumns enemies;
    ProjectileReadColumns projectiles;
    TickRows<AreaView> areas;
    TickRows<PickupView> pickups;
    TickRows<BossActionView> boss_actions;
    float effective_attack{}, effective_attack_speed{}, effective_move_speed{}, effective_magnet_radius{};
    float arena_half_extent{};
    const ArenaBoundary &arena_boundary;
    std::span<const ArenaObstacle2D> arena_obstacles;
    float charge_range{}, charge_radius{};
    Tick charge_normal_ready_tick{};
    float charge_ratio{};
    std::span<const SkillRuntimeView> skills;
    std::span<const WaveView> waves;
    SessionSummaryReadView summary;
    std::uint64_t session_id{};
    TickRows<VisualLinkView> visual_links;
    TickRows<MiniBombView> mini_bombs;
    TickRows<SpawnWarningView> spawn_warnings;
    TickRows<ScheduledAreaDeliveryView> scheduled_area_deliveries;
    std::span<const BossDeathVisualView> boss_death_visuals;
    std::span<const DomainSignal> domain_signals;
};

namespace development
{
// An existing owned-format fixture may feed the same synchronous projection kernel.
// The fixture must remain alive and unchanged until that call finishes.
inline TickReadView BorrowOwnedReadModel(const GameReadModel &model)
{
    TickRows<EnemyView> enemies(&model, model.enemies.size(), model.enemies.size(),
        [](const void *source, std::size_t row, EnemyView &value) {
            const auto &model = *static_cast<const GameReadModel *>(source);
            value = model.enemies[row];
            return true;
        });
    TickRows<ProjectileReadValue> projectiles(&model, model.projectiles.size(), model.projectiles.size(),
        [](const void *source, std::size_t row, ProjectileReadValue &value) {
            const auto &model = *static_cast<const GameReadModel *>(source);
            const auto &p = model.projectiles[row];
            value = {p.id, p.player_owned, p.position, p.velocity, p.spawned_tick, p.skill, p.dead, p.radius, p.charge_ratio, p.origin, p.source_upgrade, p.previous_position, p.remaining_range, p.cast_id, p.source_relic, p.source_enemy, p.returning, p.homing, p.return_started_tick, p.return_start_position, p.upgrade_mask, p.bleed_extend_ticks};
            return true;
        });
    TickRows<AreaView> areas(&model, model.areas.size(), model.areas.size(),
        [](const void *source, std::size_t row, AreaView &value) {
            const auto &model = *static_cast<const GameReadModel *>(source);
            value = model.areas[row];
            return true;
        });
    TickRows<PickupView> pickups(&model, model.pickups.size(), model.pickups.size(),
        [](const void *source, std::size_t row, PickupView &value) {
            const auto &model = *static_cast<const GameReadModel *>(source);
            value = model.pickups[row];
            return true;
        });
    TickRows<BossActionView> boss_actions(&model, model.boss_actions.size(), model.boss_actions.size(),
        [](const void *source, std::size_t row, BossActionView &value) {
            const auto &model = *static_cast<const GameReadModel *>(source);
            value = model.boss_actions[row];
            return true;
        });
    TickRows<VisualLinkView> visual_links(&model, model.visual_links.size(), model.visual_links.size(),
        [](const void *source, std::size_t row, VisualLinkView &value) {
            const auto &model = *static_cast<const GameReadModel *>(source);
            value = model.visual_links[row];
            return true;
        });
    TickRows<MiniBombView> mini_bombs(&model, model.mini_bombs.size(), model.mini_bombs.size(),
        [](const void *source, std::size_t row, MiniBombView &value) {
            const auto &model = *static_cast<const GameReadModel *>(source);
            value = model.mini_bombs[row];
            return true;
        });
    TickRows<SpawnWarningView> spawn_warnings(&model, model.spawn_warnings.size(), model.spawn_warnings.size(),
        [](const void *source, std::size_t row, SpawnWarningView &value) {
            const auto &model = *static_cast<const GameReadModel *>(source);
            value = model.spawn_warnings[row];
            return true;
        });
    TickRows<ScheduledAreaDeliveryView> scheduled_area_deliveries(&model, model.scheduled_area_deliveries.size(), model.scheduled_area_deliveries.size(),
        [](const void *source, std::size_t row, ScheduledAreaDeliveryView &value) {
            const auto &model = *static_cast<const GameReadModel *>(source);
            value = model.scheduled_area_deliveries[row];
            return true;
        });
    return {model.tick, model.checksum, model.seed, model.session, model.player,
        enemies, projectiles, areas, pickups, boss_actions,
        model.effective_attack, model.effective_attack_speed, model.effective_move_speed, model.effective_magnet_radius,
        model.arena_half_extent, model.arena_boundary, model.arena_obstacles,
        model.charge_range, model.charge_radius, model.charge_normal_ready_tick, model.charge_ratio,
        model.skills, model.waves,
        {model.summary.direct_damage, model.summary.derived_damage, model.summary.damage_over_time,
         model.summary.upgrade_damage, model.summary.relic_damage, model.summary.relic_triggers,
         model.summary.relic_kills, model.summary.relic_effects},
        model.session_id, visual_links, mini_bombs, spawn_warnings, scheduled_area_deliveries,
        model.boss_death_visuals, {}};
}
inline TickReadView BorrowOwnedReadModel(GameReadModel &&) = delete;
} // namespace development
} // namespace hs
