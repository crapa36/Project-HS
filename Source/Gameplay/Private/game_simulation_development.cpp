#include <hs/gameplay/game_simulation_development.hpp>
#include <algorithm>

namespace hs
{

void CopyReadModelForDevelopment(const GameSimulation &simulation, GameReadModelStorage &model)
{
    auto scope = simulation.BorrowTickReadView();
    const auto &view = scope.View();
    model.Clear(); model.tick = view.tick; model.checksum = view.checksum; model.seed = view.seed;
    model.session = view.session; model.session_id = view.session_id; model.player = view.player;
    model.effective_attack = view.effective_attack; model.effective_attack_speed = view.effective_attack_speed;
    model.effective_move_speed = view.effective_move_speed; model.effective_magnet_radius = view.effective_magnet_radius;
    model.arena_half_extent = view.arena_half_extent; model.arena_boundary = view.arena_boundary;
    model.arena_obstacle_count = static_cast<std::uint32_t>(view.arena_obstacles.size());
    std::ranges::copy(view.arena_obstacles, model.arena_obstacles.begin());
    model.charge_range = view.charge_range; model.charge_radius = view.charge_radius;
    model.charge_normal_ready_tick = view.charge_normal_ready_tick; model.charge_ratio = view.charge_ratio;
    std::ranges::copy(view.skills, model.skills.begin()); std::ranges::copy(view.waves, model.waves.begin());
    model.summary = {view.summary.direct_damage, view.summary.derived_damage, view.summary.damage_over_time,
        view.summary.upgrade_damage, view.summary.relic_damage, view.summary.relic_triggers, view.summary.relic_kills, view.summary.relic_effects};
    for (const auto &e : view.enemies) model.AddEnemy(e);
    for (const auto &p : view.projectiles) model.AddProjectile({p.id, p.player_owned, p.position, p.velocity, p.spawned_tick, p.skill, p.dead, p.radius, p.charge_ratio, p.origin, p.source_upgrade, p.previous_position, p.remaining_range, p.cast_id, p.source_relic, p.source_enemy, p.returning, p.homing, p.return_started_tick, p.return_start_position, p.upgrade_mask, {p.bleed_extend_ticks.begin(), p.bleed_extend_ticks.end()}});
    for (const auto &a : view.areas) model.AddArea(a);
    for (const auto &p : view.pickups) model.AddPickup(p);
    for (const auto &a : view.boss_actions) model.AddBossAction(a);
    for (const auto &v : view.visual_links) model.AddVisualLink(v);
    for (const auto &v : view.mini_bombs) model.AddMiniBomb(v);
    for (const auto &v : view.spawn_warnings) model.AddSpawnWarning(v);
    for (const auto &v : view.scheduled_area_deliveries) model.AddScheduledAreaDelivery(v);
    for (const auto &v : view.boss_death_visuals) model.AddBossDeathVisual(v);
}

} // namespace hs
