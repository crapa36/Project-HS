#include "gameplay_test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gameplay_test
{

namespace
{
} // namespace

void TestTenMinuteBossApproachesAttackRange()
{
    auto data = QuietGameData();
    data.stats.base_maximum_hp = 10'000.0f;
    data.stats.base_current_hp = 10'000.0f;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({143}, data).Succeeded(),
          "ten minute boss approach initialize");
    Debug(simulation, hs::DebugCommandKind::SpawnBoss,
          static_cast<std::uint64_t>(hs::BossKind::TenMinute));
    hs::RenderSnapshotStorage before_snapshot(32, 2, 2, 8);
    Check(WriteSnapshot(simulation, before_snapshot), "boss before snapshot");
    const auto before = std::ranges::find_if(
        before_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::BossTenMinute;
        });
    Check(before != before_snapshot.View().instances.end(), "ten minute boss visible");
    const auto before_distance = std::hypot(before->position.x, before->position.z);
    for (std::uint32_t tick = 0; tick < 151; ++tick) (void)Tick(simulation);
    hs::RenderSnapshotStorage after_snapshot(32, 2, 2, 8);
    Check(WriteSnapshot(simulation, after_snapshot), "boss after snapshot");
    const auto after = std::ranges::find_if(
        after_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::BossTenMinute;
        });
    Check(after != after_snapshot.View().instances.end() &&
              std::hypot(after->position.x, after->position.z) < before_distance - 4.0f,
          "ten minute boss uses doubled movement speed toward its 18 meter attack range");
    for (std::uint32_t tick = 151;
         tick < 1200 && simulation.GetObservation().boss_warning_count == 0; ++tick)
        (void)Tick(simulation);
    Check(simulation.GetObservation().boss_warning_count > 0,
          "ten minute boss starts a skill warning");
    hs::RenderSnapshotStorage warning_snapshot(32, 2, 2, 8);
    Check(WriteSnapshot(simulation, warning_snapshot), "boss warning snapshot");
    const auto warning = std::ranges::find_if(
        warning_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::BossTenMinute;
        });
    Check(warning != warning_snapshot.View().instances.end(),
          "ten minute boss visible during warning");
    const auto warning_position = warning->position;
    for (std::uint32_t tick = 0; tick < 20; ++tick) (void)Tick(simulation);
    hs::RenderSnapshotStorage casting_snapshot(32, 2, 2, 8);
    Check(WriteSnapshot(simulation, casting_snapshot), "boss casting snapshot");
    const auto casting = std::ranges::find_if(
        casting_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::BossTenMinute;
        });
    Check(casting != casting_snapshot.View().instances.end() &&
              std::hypot(casting->position.x - warning_position.x,
                         casting->position.z - warning_position.z) < 0.001f,
          "ten minute boss remains stationary while warning and casting");
    for (std::uint32_t tick = 0;
         tick < 1200 && simulation.GetObservation().balance.enemy_attack_attempts[4] == 0; ++tick)
        (void)Tick(simulation);
    Check(simulation.GetObservation().balance.enemy_attack_attempts[4] > 0,
          "ten minute boss begins ranged attacks from the expanded 18 meter range");
    hs::GameReadModelStorage boss_model;
    simulation.WriteReadModel(boss_model);
    const auto shockwave = std::ranges::find_if(
        boss_model.View().boss_actions, [](const hs::BossActionView &action) {
            return action.kind == hs::BossActionViewKind::Shockwave;
        });
    if (shockwave != boss_model.View().boss_actions.end())
        Check(shockwave->safe_gap_count == 4 &&
                  std::abs(shockwave->safe_gap_degrees - 25.0f) < 0.0001f &&
                  std::abs(shockwave->half_width - 0.75f) < 0.0001f,
              "boss shockwave read model preserves authored gap count, width, and ring thickness");

    auto volley_data = QuietGameData();
    volley_data.stats.base_maximum_hp = 1'000'000.0f;
    volley_data.stats.base_current_hp = 1'000'000.0f;
    volley_data.boss_common.projectile_range = 31.0f;
    hs::GameSimulation volley_simulation;
    Check(volley_simulation.Initialize({145}, volley_data).Succeeded(),
          "boss volley geometry initialize");
    Debug(volley_simulation, hs::DebugCommandKind::SpawnBoss,
          static_cast<std::uint64_t>(hs::BossKind::TenMinute));
    hs::GameReadModelStorage volley_model;
    bool checked_volley{};
    bool checked_area{};
    for (std::uint32_t tick = 0; tick < 2'400 && (!checked_volley || !checked_area); ++tick)
    {
        hs::HeldInputState approach;
        if (checked_volley)
        {
            const auto boss=std::ranges::find_if(volley_model.View().enemies,[](const auto &enemy){return enemy.boss.has_value();});
            if(boss!=volley_model.View().enemies.end())
            {
                approach.move_held=true;
                approach.move_target_world={boss->position.x,0,boss->position.y};
            }
        }
        (void)Tick(volley_simulation,approach);
        volley_simulation.WriteReadModel(volley_model);
        if (!checked_area)
        {
            const auto area=std::ranges::find_if(volley_model.View().boss_actions,[](const auto &action){return action.kind==hs::BossActionViewKind::Area;});
            if(area!=volley_model.View().boss_actions.end())
            {
                const auto signal=std::ranges::find_if(volley_simulation.PendingDomainSignals(),[&](const auto &event){return event.sequence==area->warning_sequence;});
                Check(signal!=volley_simulation.PendingDomainSignals().end()&&signal->kind==hs::DomainSignalKind::BossAreaTelegraphed&&
                    signal->geometry.kind==hs::DomainSignalGeometryKind::Circle&&signal->geometry.radius==area->radius&&
                    signal->geometry.start_tick==area->warning_started&&signal->geometry.end_tick==area->execute_tick&&
                    signal->geometry.source_id==area->warning_sequence&&signal->position.x==area->position.x&&signal->position.z==area->position.y,
                    "boss area warning preserves actual scheduled geometry and clocks");
                hs::RenderSnapshotStorage area_snapshot(512,4,4,128);
                Check(WriteSnapshot(volley_simulation,area_snapshot),"boss area warning snapshot");
                Check(std::ranges::any_of(area_snapshot.View().persistent_vfx,[&](const auto &owner){return owner.kind==hs::PersistentVfxKind::BossAreaWarning&&
                    owner.stable_id==area->warning_sequence&&owner.radius==area->radius&&owner.expires==area->execute_tick;}),
                    "boss area schedule publishes matching Circle owner");
                checked_area=true;
            }
        }
        const auto volley = std::ranges::find_if(
            volley_model.View().boss_actions, [](const hs::BossActionView &action) {
                return action.kind == hs::BossActionViewKind::Volley;
            });
        if (!checked_volley && volley != volley_model.View().boss_actions.end())
        {
            Check(std::abs(volley->volley_range - 31.0f) < 0.0001f,
                  "boss volley read model preserves the nondefault authored projectile range");
            Check(volley->warning_sequence != 0 &&
                      volley->warning_started <= volley->execute_tick,
                  "boss volley warning keeps its scheduled signal identity and start tick");
            const auto telegraph = std::ranges::find_if(
                volley_simulation.PendingDomainSignals(),
                [sequence = volley->warning_sequence](const hs::DomainSignal &signal) {
                    return signal.sequence == sequence &&
                           signal.kind == hs::DomainSignalKind::BossVolleyTelegraphed;
                });
            Check(telegraph != volley_simulation.PendingDomainSignals().end() &&
                      telegraph->geometry.kind == hs::DomainSignalGeometryKind::Cone &&
                      std::abs(telegraph->geometry.range - 31.0f) < 0.0001f &&
                      std::abs(telegraph->geometry.half_angle_degrees -
                               volley->arc_degrees * 0.5f) < 0.0001f &&
                      telegraph->geometry.source_id == volley->warning_sequence &&
                      telegraph->geometry.start_tick == volley->warning_started &&
                      telegraph->geometry.end_tick == volley->execute_tick,
                  "boss volley telegraph carries nondefault cone geometry and schedule identity");
            if (telegraph != volley_simulation.PendingDomainSignals().end() &&
                std::abs(volley->direction.x) + std::abs(volley->direction.y) > 0.0001f)
            {
                const auto length = std::hypot(volley->direction.x, volley->direction.y);
                const auto radians = volley->angle_offset *
                                     3.14159265358979323846f / 180.0f;
                const auto sine = std::sin(radians);
                const auto cosine = std::cos(radians);
                const auto expected_x = (volley->direction.x / length) * cosine +
                                         (volley->direction.y / length) * sine;
                const auto expected_y = -(volley->direction.x / length) * sine +
                                         (volley->direction.y / length) * cosine;
                Check(std::abs(telegraph->direction.x - expected_x) < 0.0001f &&
                          std::abs(telegraph->direction.z - expected_y) < 0.0001f,
                      "boss volley telegraph rotates its authored angle in degrees");
            }
            hs::RenderSnapshotStorage volley_snapshot(256, 2, 2, 8);
            Check(WriteSnapshot(volley_simulation, volley_snapshot),
                  "boss volley warning projection");
            const auto boss_position = volley_model.View().enemies.front().position;
            float maximum_warning_distance{};
            for (const auto &instance : volley_snapshot.View().instances)
                if (instance.mesh == hs::RenderMesh::Area &&
                    instance.color_rgba == 0xA03030FFu)
                    maximum_warning_distance = std::max(
                        maximum_warning_distance,
                        std::hypot(instance.position.x - boss_position.x,
                                   instance.position.z - boss_position.y));
            Check(std::abs(maximum_warning_distance - 31.0f) < 0.001f,
                  "boss volley warning reaches the authored projectile range");
            checked_volley = true;
        }
    }
    Check(checked_volley, "boss volley action remains observable during its telegraph");
    Check(checked_area, "boss area action remains observable during its telegraph");
    Check(volley_simulation.Shutdown().Succeeded(), "boss volley geometry shutdown");

    {
        hs::GameReadModelStorage directional;
        directional.tick = 50;
        hs::EnemyView owner{};
        owner.id = {700}; owner.boss = hs::BossKind::TenMinute;
        owner.position = {2,3}; owner.health = owner.max_health = 100;
        directional.AddEnemy(owner);
        hs::BossActionView action{};
        action.kind = hs::BossActionViewKind::Volley;
        action.boss_id = 700; action.direction = {0,1};
        action.angle_offset = 37; action.arc_degrees = 146; action.volley_range = 19;
        action.warning_sequence = 777; action.warning_started = 10; action.execute_tick = 90;
        directional.AddBossAction(action);
        action.warning_sequence = 778; action.angle_offset = -23;
        directional.AddBossAction(action);
        hs::RenderSnapshotStorage projected(256,2,2,8);
        Check(hs::ProjectRenderSnapshot(directional.View(), DefaultContent().presentation,
              test_ui, hs::SettingsData{}, projected), "offset warning snapshot");
        const auto first = std::ranges::find_if(projected.View().persistent_vfx,
            [](const auto &v) { return v.stable_id == 777; });
        Check(first != projected.View().persistent_vfx.end() &&
              first->kind == hs::PersistentVfxKind::BossVolleyWarning &&
              std::abs(first->yaw-37.0f*3.14159265358979323846f/180.0f) < 0.0001f &&
              first->radius == 19 && first->cone_half_angle_degrees == 73 &&
              first->active_tick == 10 && first->expires == 90,
              "offset warning owner must match gameplay rotation sign and degrees");
        Check(std::ranges::count_if(projected.View().persistent_vfx,
              [](const auto &v) { return v.kind == hs::PersistentVfxKind::BossVolleyWarning; }) == 2,
              "simultaneous warning owners were merged");
        Check(std::ranges::all_of(projected.View().instances, [](const auto &v) {
            return v.vfx_owner_id == 0 || v.stable_id == 0;
        }), "warning group identity must not alias marker interpolation identity");
    }

    {
        hs::GameReadModelStorage state_model;
        state_model.tick = 120;
        hs::EnemyView boss{};
        boss.id = {701}; boss.boss = hs::BossKind::Final;
        boss.position = {6, 0}; boss.health = 49; boss.max_health = 100;
        boss.footprint_radius = 1.2f;
        boss.final_phase = 2; boss.phase2_started = 110;
        boss.invulnerable_until = 170;
        boss.dash_started = 100; boss.dash_until = 130;
        boss.dash_origin = {0, 0};
        state_model.AddEnemy(boss);
        hs::RenderSnapshotStorage state_snapshot(256, 2, 2, 8);
        Check(hs::ProjectRenderSnapshot(state_model.View(), DefaultContent().presentation,
              test_ui, test_settings, state_snapshot), "boss state snapshot");
        const auto owners = state_snapshot.View().persistent_vfx;
        const auto wake = std::ranges::find_if(owners, [](const auto &v) {
            return v.kind == hs::PersistentVfxKind::BossDashWake;
        });
        const auto aura = std::ranges::find_if(owners, [](const auto &v) {
            return v.kind == hs::PersistentVfxKind::BossPhase2Aura;
        });
        const auto locked = std::ranges::find_if(owners, [](const auto &v) {
            return v.kind == hs::PersistentVfxKind::BossPhaseTransition;
        });
        Check(wake != owners.end() && wake->position.x == 3 && wake->length == 6 &&
              wake->active_tick == 100 && wake->expires == 130 &&
              aura != owners.end() && aura->position.x == 6 && aura->active_tick == 110 &&
              locked != owners.end() && locked->expires == 170 &&
              aura->entity_render_id == ((2ull << 60) | boss.id.value) &&
              aura->stable_id != locked->stable_id,
              "boss state owners lost actual path, bounds, clock or identity");
        state_model.tick = 170; state_model.Clear(); state_model.AddEnemy(boss);
        state_snapshot.Clear();
        Check(hs::ProjectRenderSnapshot(state_model.View(), DefaultContent().presentation,
              test_ui, test_settings, state_snapshot), "expired boss state snapshot");
        Check(std::ranges::count_if(state_snapshot.View().persistent_vfx, [](const auto &v) {
            return v.kind == hs::PersistentVfxKind::BossPhase2Aura;
        }) == 1 &&
              std::ranges::none_of(state_snapshot.View().persistent_vfx, [](const auto &v) {
                  return v.kind == hs::PersistentVfxKind::BossDashWake ||
                         v.kind == hs::PersistentVfxKind::BossPhaseTransition;
              }), "expired boss states outlived gameplay while phase2 aura was lost");
    }

    hs::GameReadModelStorage warning_model;
    hs::EnemyView warning_boss{};
    warning_boss.id = {0xB055u};
    warning_boss.boss = hs::BossKind::FiveMinute;
    warning_boss.health = warning_boss.max_health = 100;
    warning_model.AddEnemy(warning_boss);
    hs::BossActionView warning_action{};
    warning_action.kind = hs::BossActionViewKind::Shockwave;
    warning_action.boss_id = warning_boss.id.value;
    warning_action.radius = 5.0f;
    warning_action.safe_gap_count = 0;
    warning_model.AddBossAction(warning_action);
    hs::RenderSnapshotStorage gap_snapshot(128, 2, 2, 8);
    Check(hs::ProjectRenderSnapshot(warning_model.View(), DefaultContent().presentation,
                                    test_ui, test_settings, gap_snapshot),
          "zero-gap boss warning projection");
    const auto full_ring_count = std::ranges::count_if(
        gap_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.color_rgba == 0xA03030FFu;
        });
    Check(full_ring_count == 48,
          "zero-gap shockwave warning keeps its full circumference");
    warning_action.safe_gap_count = 3;
    warning_action.safe_gap_degrees = 40.0f;
    warning_action.cast_id = 17;
    warning_model.Clear();
    warning_model.AddEnemy(warning_boss);
    warning_model.AddBossAction(warning_action);
    gap_snapshot.Clear();
    Check(hs::ProjectRenderSnapshot(warning_model.View(), DefaultContent().presentation,
                                    test_ui, test_settings, gap_snapshot),
          "authored-gap boss warning projection");
    const auto authored_gap_count = std::ranges::count_if(
        gap_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.color_rgba == 0xA03030FFu;
        });
    Check(authored_gap_count == 33,
          "shockwave warning uses authored gap count and width instead of visual defaults");
    warning_boss.position = {20.0f, -17.0f};
    warning_action.position = {3.0f, 4.0f}; warning_action.distance = 1.7f;
    warning_action.warning_sequence = 831; warning_action.warning_started = 10;
    warning_action.execute_tick = 83; warning_model.tick = 40;
    warning_model.Clear(); warning_model.AddEnemy(warning_boss); warning_model.AddBossAction(warning_action);
    gap_snapshot.Clear();
    Check(hs::ProjectRenderSnapshot(warning_model.View(), DefaultContent().presentation,
          test_ui, test_settings, gap_snapshot), "fixed-center shockwave warning projection");
    const auto ring_warning = std::ranges::find_if(gap_snapshot.View().persistent_vfx,
        [](const auto &v) { return v.kind == hs::PersistentVfxKind::BossShockwaveWarning; });
    Check(ring_warning != gap_snapshot.View().persistent_vfx.end() &&
          ring_warning->position.x == 3.0f && ring_warning->position.z == 4.0f &&
          ring_warning->ring_inner_radius == 1.7f && ring_warning->ring_outer_radius == 5.0f &&
          ring_warning->stable_id == 831 && ring_warning->active_tick == 10 && ring_warning->expires == 83 &&
          ring_warning->gap_count == 3 && ring_warning->gap_half_angle_degrees == 20.0f &&
          ring_warning->gap_offset_degrees == 17.0f,
          "shockwave warning preserves scheduled center despite boss displacement");
    Check(std::ranges::all_of(gap_snapshot.View().instances, [](const auto &v) {
        return v.color_rgba != 0xA03030FFu || (v.stable_id == 0 && v.vfx_owner_id == 831 &&
            std::abs(std::hypot(v.position.x - 3.0f, v.position.z - 4.0f) - 5.0f) < 0.0001f);
    }), "shockwave fallback markers share fixed center and owner identity");
    hs::AreaView active_wave{};
    active_wave.id = {0xA11u};
    active_wave.kind = hs::AreaViewKind::EnemyDamage;
    active_wave.active_tick = 0;
    active_wave.expires = 100;
    active_wave.ring_inner_radius = 2.0f;
    active_wave.ring_outer_radius = 6.0f;
    active_wave.safe_gap_count = 0;
    warning_model.Clear();
    warning_model.tick = 50;
    warning_model.AddArea(active_wave);
    gap_snapshot.Clear();
    Check(hs::ProjectRenderSnapshot(warning_model.View(), DefaultContent().presentation,
                                    test_ui, test_settings, gap_snapshot),
          "zero-gap active wave projection");
    const auto active_ring_count = std::ranges::count_if(
        gap_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.color_rgba == 0xB04040FFu;
        });
    Check(active_ring_count == 64 &&
              !std::ranges::any_of(
                  gap_snapshot.View().instances, [](const hs::RenderInstance &instance) {
                      return instance.color_rgba == 0x604040FFu;
                  }),
          "zero-gap active wave interpolates a full ring without a filled disc");
    hs::GameReadModelStorage wave_model;
    hs::AreaView wave{};
    wave.id = {719}; wave.kind = hs::AreaViewKind::EnemyDamage; wave.skill = hs::SkillKind::Count;
    wave.position = {3.0f, -4.0f}; wave.active_tick = 100; wave.expires = 140;
    wave.ring_inner_radius = 0.3f; wave.ring_outer_radius = 12.3f;
    wave.ring_half_width = 0.7f; wave.safe_gap_count = 3; wave.safe_gap_degrees = 24.0f;
    wave.cast_id = 719;
    for (const auto sample_tick : {99ull, 100ull, 120ull, 139ull, 140ull})
    {
        wave_model.Clear(); wave_model.tick = sample_tick; wave_model.AddArea(wave);
        gap_snapshot.Clear();
        Check(hs::ProjectRenderSnapshot(wave_model.View(), DefaultContent().presentation,
              test_ui, test_settings, gap_snapshot), "wavefront snapshot");
        const auto found = std::ranges::find_if(gap_snapshot.View().persistent_vfx,
            [](const auto &v) { return v.kind == hs::PersistentVfxKind::BossShockwaveWavefront; });
        if (sample_tick < 100 || sample_tick >= 140)
        {
            Check(found == gap_snapshot.View().persistent_vfx.end(), "wavefront respects activation and expiry");
            continue;
        }
        const auto center_radius = std::lerp(0.3f, 12.3f, static_cast<float>(sample_tick - 100) / 40.0f);
        Check(found != gap_snapshot.View().persistent_vfx.end() &&
              std::abs(found->ring_inner_radius - std::max(0.0f, center_radius - 0.7f)) < 0.0001f &&
              std::abs(found->ring_outer_radius - center_radius - 0.7f) < 0.0001f &&
              found->radius == found->ring_outer_radius && found->active_tick == 100 && found->expires == 140 &&
              found->gap_count == 3 && found->gap_half_angle_degrees == 12.0f && found->gap_offset_degrees == 359.0f,
              "wavefront exposes moving collision annulus and rotated gaps at each age");
        Check(std::ranges::all_of(gap_snapshot.View().instances, [](const auto &v) {
                  return v.color_rgba != 0xB04040FFu ||
                      (v.stable_id == 0 && v.vfx_owner_id == ((4ull << 60) | 719));
              }), "wavefront legacy markers use owner identity without shared interpolation identity");
    }
    wave.safe_gap_count = 0; wave.safe_gap_degrees = 0.0f;
    wave_model.Clear(); wave_model.tick = 120; wave_model.AddArea(wave); gap_snapshot.Clear();
    Check(hs::ProjectRenderSnapshot(wave_model.View(), DefaultContent().presentation,
          test_ui, test_settings, gap_snapshot) &&
          std::ranges::any_of(gap_snapshot.View().persistent_vfx, [](const auto &v) {
              return v.kind == hs::PersistentVfxKind::BossShockwaveWavefront && v.gap_count == 0;
          }), "zero-gap wavefront remains a continuous annulus");
    warning_boss.dead = true;
    warning_model.Clear();
    warning_model.AddEnemy(warning_boss);
    warning_model.AddBossAction(warning_action);
    gap_snapshot.Clear();
    Check(hs::ProjectRenderSnapshot(warning_model.View(), DefaultContent().presentation,
                                    test_ui, test_settings, gap_snapshot),
          "dead boss warning projection");
    Check(!std::ranges::any_of(
              gap_snapshot.View().instances, [](const hs::RenderInstance &instance) {
                  return instance.color_rgba == 0xA03030FFu;
              }),
          "dead boss action does not leave a warning behind");
    Check(simulation.Shutdown().Succeeded(), "ten minute boss approach shutdown");
}

void TestTenMinuteBossGroundAreasStaySeparated()
{
    auto data = QuietGameData();
    data.stats.base_maximum_hp = 1'000'000.0f;
    data.stats.base_current_hp = 1'000'000.0f;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({144}, data).Succeeded(),
          "ten minute ground area initialize");
    Debug(simulation, hs::DebugCommandKind::SpawnBoss,
          static_cast<std::uint64_t>(hs::BossKind::TenMinute));
    for (std::uint32_t tick = 0;
         tick < 20'000 && simulation.GetObservation().enemy_area_count < 3; ++tick)
        (void)Tick(simulation);
    Check(simulation.GetObservation().enemy_area_count == 3,
          "ten minute boss creates three ground areas");

    hs::RenderSnapshotStorage snapshot(64, 2, 2, 8);
    Check(WriteSnapshot(simulation, snapshot), "ten minute ground area snapshot");
    std::vector<hs::Float3> centers;
    for (const auto &instance : snapshot.View().instances)
        if (instance.mesh == hs::RenderMesh::Area && instance.color_rgba == 0x604040FFu &&
            std::abs(instance.scale.x - 2.2f) < 0.0001f)
            centers.push_back(instance.position);
    Check(centers.size() == 3, "three ten minute ground areas are visible");
    for (std::size_t left = 0; left < centers.size(); ++left)
        for (std::size_t right = left + 1; right < centers.size(); ++right)
            Check(std::hypot(centers[left].x - centers[right].x,
                             centers[left].z - centers[right].z) >= 2.2f,
                  "ten minute ground area overlap stays below fifty percent");
    Check(simulation.Shutdown().Succeeded(), "ten minute ground area shutdown");

    auto ring_data = QuietGameData();
    ring_data.arena_obstacle_count = 0;
    ring_data.stats.base_maximum_hp = 10'000.0f;
    ring_data.stats.base_current_hp = 10'000.0f;
    auto &ring_boss = ring_data.bosses[static_cast<std::size_t>(hs::BossKind::FiveMinute)];
    ring_boss.movement_speed = 0.0f;
    ring_boss.preferred_distance_near = 100.0f;
    ring_boss.preferred_distance_far = 100.0f;
    ring_boss.pattern_count = 1;
    ring_boss.patterns[0] = {};
    ring_boss.patterns[0].logic = hs::BossPatternLogic::ExpandingShockwaveWithSafeGaps;
    ring_boss.patterns[0].phase = 1;
    ring_boss.patterns[0].telegraph_duration_ticks = 1;
    ring_boss.patterns[0].start_radius = 0.0f;
    ring_boss.patterns[0].end_radius = 70.0f;
    ring_boss.patterns[0].damage = 20;
    ring_boss.patterns[0].safe_gap_count = 0;
    ring_boss.patterns[0].safe_gap_angle_degrees = 0.0f;
    ring_boss.patterns[0].shockwave_duration_ticks = 140;
    ring_boss.patterns[0].shockwave_half_width = 0.5f;
    hs::SimulationConfig ring_config{146};
    ring_config.scenario = {.player_stationary = true,
                            .player_invulnerable = false,
                            .progression_enabled = false};
    hs::GameSimulation ring_simulation;
    Check(ring_simulation.Initialize(ring_config, ring_data).Succeeded(),
          "zero-gap shockwave collision initialize");
    Debug(ring_simulation, hs::DebugCommandKind::SpawnBoss,
          static_cast<std::uint64_t>(hs::BossKind::FiveMinute));
    const auto initial_health = ring_simulation.GetObservation().health;
    hs::GameReadModelStorage ring_model;
    bool found_ring{};
    bool saw_pre_intersection{};
    bool damaged_on_intersection{};
    for (std::uint32_t tick = 0; tick < 600 && !damaged_on_intersection; ++tick)
    {
        (void)Tick(ring_simulation);
        ring_simulation.WriteReadModel(ring_model);
        const auto area = std::ranges::find_if(
            ring_model.View().areas, [](const hs::AreaView &value) {
                return value.kind == hs::AreaViewKind::EnemyDamage &&
                       value.ring_outer_radius > 0.0f;
            });
        if (area == ring_model.View().areas.end()) continue;
        found_ring = true;
        const auto duration = std::max<hs::Tick>(area->expires - area->active_tick, 1);
        const auto progress = std::clamp(
            static_cast<float>(ring_model.View().tick - area->active_tick) /
                static_cast<float>(duration),
            0.0f, 1.0f);
        const auto radius = std::lerp(area->ring_inner_radius,
                                      area->ring_outer_radius, progress);
        const auto distance = std::hypot(area->position.x - ring_model.View().player.position.x,
                                         area->position.y - ring_model.View().player.position.y);
        if (radius + area->ring_half_width < distance)
        {
            saw_pre_intersection = true;
            Check(ring_simulation.GetObservation().health == initial_health,
                  "zero-gap shockwave does not damage before its expanding band reaches the player");
        }
        else if (ring_simulation.GetObservation().health < initial_health)
        {
            damaged_on_intersection = true;
        }
    }
    Check(found_ring && saw_pre_intersection && damaged_on_intersection,
          "zero-gap shockwave damages when its expanding ring intersects the player");
    Check(ring_simulation.Shutdown().Succeeded(), "zero-gap shockwave collision shutdown");
}

void TestRangedWarningAndExplosiveArea()
{
    auto ranged_data = QuietGameData();
    auto &ranged_rule = ranged_data.enemies[static_cast<std::size_t>(hs::EnemyKind::Ranged)];
    ranged_rule.warning_ticks = 47;
    ranged_rule.projectile_range = 31.0f;
    ranged_rule.ranged_projectile_radius = 0.37f;
    hs::GameSimulation warning_simulation;
    Check(warning_simulation.Initialize({20}, ranged_data).Succeeded(),
          "ranged warning initialize");
    Debug(warning_simulation, hs::DebugCommandKind::SpawnEnemy,
          static_cast<std::uint64_t>(hs::EnemyKind::Ranged), 0, {10.0f, 0.0f});
    (void)Tick(warning_simulation);
    const auto warning_signal = std::ranges::find_if(warning_simulation.PendingDomainSignals(),
        [](const auto &signal) { return signal.kind == hs::DomainSignalKind::RangedEnemyTelegraphed; });
    Check(warning_signal != warning_simulation.PendingDomainSignals().end() &&
          warning_signal->geometry.kind == hs::DomainSignalGeometryKind::Line &&
          warning_signal->geometry.source_id == warning_signal->sequence &&
          warning_signal->geometry.end_tick - warning_signal->geometry.start_tick == 47 &&
          warning_signal->geometry.range == 31.0f &&
          std::abs(warning_signal->geometry.width - 0.74f) < 0.0001f &&
          std::abs(warning_signal->geometry.end_position.x + 21.0f) < 0.0001f,
          "ranged telegraph carries its scheduled lifetime, projectile width and range");
    hs::RenderSnapshotStorage warning_snapshot(128, 4, 2, 64);
    Check(WriteSnapshot(warning_simulation, warning_snapshot),
          "ranged warning snapshot");
    Check(std::ranges::any_of(
              warning_snapshot.View().instances, [](const hs::RenderInstance &instance) {
                  return instance.mesh == hs::RenderMesh::Area &&
                         instance.color_rgba == 0xA03030FFu && instance.scale.z >= 18.0f;
              }), "ranged enemy renders a red pre-attack range line");
    hs::GameReadModelStorage ranged_model;
    warning_simulation.WriteReadModel(ranged_model);
    Check(ranged_model.View().enemies.size() == 1 &&
          ranged_model.View().enemies[0].warning_sequence != 0 &&
          ranged_model.View().enemies[0].warning_half_width == 0.37f,
          "ranged warning owner is copied to read model");
    for (std::uint32_t step = 0; step < 47; ++step)
        (void)Tick(warning_simulation);
    warning_snapshot.Clear();
    Check(WriteSnapshot(warning_simulation, warning_snapshot) &&
          std::ranges::none_of(warning_snapshot.View().persistent_vfx, [](const auto &visual) {
              return visual.kind == hs::PersistentVfxKind::RangedEnemyWarning;
          }), "ranged warning owner disappears on projectile release");
    for (std::uint32_t step = 47; step < 90; ++step)
        (void)Tick(warning_simulation);
    warning_snapshot.Clear();
    Check(WriteSnapshot(warning_simulation, warning_snapshot),
          "ranged hold position snapshot");
    const auto ranged = std::ranges::find_if(
        warning_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::MonsterRanged;
        });
    Check(ranged != warning_snapshot.View().instances.end() &&
              std::abs(ranged->position.x - 10.0f) < 0.0001f &&
              std::abs(ranged->position.z) < 0.0001f,
          "ranged enemy holds position inside attack range instead of retreating");
    Check(warning_simulation.Shutdown().Succeeded(), "ranged warning shutdown");

    hs::GameReadModelStorage displaced;
    displaced.tick = 30;
    hs::EnemyView warning_enemy{};
    warning_enemy.id = {71}; warning_enemy.kind = hs::EnemyKind::Ranged;
    warning_enemy.position = {8.0f, 3.0f}; warning_enemy.locked_aim = {-1.0f, 0.0f};
    warning_enemy.attacking = true; warning_enemy.attack_started = 20; warning_enemy.attack_resolve = 67;
    warning_enemy.warning_extent = 31.0f; warning_enemy.warning_half_width = 0.37f;
    warning_enemy.warning_sequence = 901;
    displaced.AddEnemy(warning_enemy);
    warning_enemy.id = {72}; warning_enemy.position = {2.0f, 9.0f}; warning_enemy.warning_sequence = 902;
    displaced.AddEnemy(warning_enemy);
    warning_snapshot.Clear();
    Check(hs::ProjectRenderSnapshot(displaced.View(), DefaultContent().presentation,
          hs::PresentationUiState{}, hs::SettingsData{}, warning_snapshot), "displaced ranged warning projection");
    const auto first_warning = std::ranges::find_if(warning_snapshot.View().persistent_vfx,
        [](const auto &v) { return v.stable_id == 901; });
    Check(first_warning != warning_snapshot.View().persistent_vfx.end() &&
          first_warning->kind == hs::PersistentVfxKind::RangedEnemyWarning &&
          std::abs(first_warning->position.x + 7.5f) < 0.0001f && first_warning->position.z == 3.0f &&
          first_warning->active_tick == 20 && first_warning->expires == 67 &&
          first_warning->radius == 0.37f && first_warning->length == 31.0f,
          "ranged warning follows displaced origin while preserving locked aim and schedule");
    Check(std::ranges::count_if(warning_snapshot.View().persistent_vfx, [](const auto &v) {
              return v.kind == hs::PersistentVfxKind::RangedEnemyWarning;
          }) == 2 && std::ranges::count_if(warning_snapshot.View().instances, [](const auto &v) {
              return v.mesh == hs::RenderMesh::Area && v.stable_id == 0 &&
                     (v.vfx_owner_id == 901 || v.vfx_owner_id == 902);
          }) == 2, "ranged warnings have distinct owners without shared interpolation identities");

    hs::GameSimulation suicide_warning;
    Check(suicide_warning.Initialize({201}, QuietGameData()).Succeeded(),
          "suicide warning initialize");
    Debug(suicide_warning, hs::DebugCommandKind::SpawnEnemy,
          static_cast<std::uint64_t>(hs::EnemyKind::Suicide), 0, {2.0f, 0.0f});
    (void)Tick(suicide_warning);
    hs::RenderSnapshotStorage suicide_snapshot(128, 4, 2, 64);
    Check(WriteSnapshot(suicide_warning, suicide_snapshot),
          "suicide warning snapshot");
    Check(std::ranges::any_of(
              suicide_snapshot.View().instances, [](const hs::RenderInstance &instance) {
                  return instance.mesh == hs::RenderMesh::Area &&
                         instance.color_rgba == 0x803030FFu &&
                         std::abs(instance.scale.x - 3.0f) < 0.0001f &&
                         std::abs(instance.scale.z - 3.0f) < 0.0001f;
              }), "suicide enemy renders its red explosion radius while arming");
    const auto charge = std::ranges::find_if(suicide_warning.PendingDomainSignals(), [](const auto &signal) {
        return signal.kind == hs::DomainSignalKind::SuicideEnemyCharging;
    });
    Check(charge != suicide_warning.PendingDomainSignals().end() &&
        charge->geometry.kind == hs::DomainSignalGeometryKind::Circle && charge->geometry.radius == 3.0f &&
        charge->geometry.source_id == charge->sequence && charge->geometry.end_tick > charge->geometry.start_tick,
        "suicide charge carries actual explosion radius and attack schedule");
    Check(std::ranges::any_of(suicide_snapshot.View().persistent_vfx, [&](const auto &visual) {
        return visual.kind == hs::PersistentVfxKind::SuicideEnemyWarning && charge != suicide_warning.PendingDomainSignals().end() &&
            visual.stable_id == charge->sequence && visual.radius == 3.0f && visual.expires == charge->geometry.end_tick;
    }), "suicide attack publishes matching warning owner");
    Check(suicide_warning.Shutdown().Succeeded(), "suicide warning shutdown");

    hs::GameSimulation explosion_simulation;
    Check(explosion_simulation.Initialize({21}, QuietGameData()).Succeeded(),
          "explosive area initialize");
    Debug(explosion_simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::ExplosiveArrow));
    Debug(explosion_simulation, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::ExplosiveArrow), 3);
    Debug(explosion_simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 0.0f});
    Debug(explosion_simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 1.5f});
    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    hs::Sequence sequence{};
    (void)TickEdge(explosion_simulation, hs::GameAction::SkillQ,
                   hs::EdgeKind::Pressed, sequence, held);
    for (std::uint32_t tick = 0; tick < 30 && explosion_simulation.GetObservation().kills < 2; ++tick)
    {
        (void)Tick(explosion_simulation, held);
    }
    Check(explosion_simulation.GetObservation().kills == 2,
          "explosive arrow impact damages every enemy in its radius");
    hs::RenderSnapshotStorage fire_snapshot(128, 4, 2, 64);
    Check(WriteSnapshot(explosion_simulation, fire_snapshot),
          "explosive fire area snapshot");
    Check(std::ranges::any_of(
              fire_snapshot.View().persistent_vfx, [](const auto &visual) {
                  return visual.kind == hs::PersistentVfxKind::FireArea;
              }),
          "fire area uses a persistent ground visual distinct from explosion VFX");
    const auto combat_stats = explosion_simulation.GetObservation();
    Check(combat_stats.damage_by_skill[
              static_cast<std::size_t>(hs::SkillKind::ExplosiveArrow)] ==
              combat_stats.damage_dealt && combat_stats.damage_dealt > 0,
          "damage statistics attribute applied damage to the source skill");
    for (std::uint32_t tick = 0; tick < 60; ++tick)
        (void)Tick(explosion_simulation);
    Check(explosion_simulation.GetObservation().pickup_count == 2,
          "experience pickups remain separate in the same spatial cell");
    Check(explosion_simulation.Shutdown().Succeeded(), "explosive area shutdown");
}

} // namespace gameplay_test
