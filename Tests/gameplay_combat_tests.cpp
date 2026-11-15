#include "gameplay_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gameplay_test
{

void TestArrowReleaseCarriesAuthoritativeDirection();
void TestBasicAttackStopsAtFirstEnemy();
void TestQwerInputBuffer();
void TestBasicAttackSkillCancellationAndBufferRules();
void TestSkillMovementPauseAndResume();
void TestChargedShotCancelsForLevelSelection();
void TestAuthoritativeCombatGeometryEmission();
void TestArrowRainFireAreasUseTheirAuthoredInterval();
void TestTrapFireAreasUseTheirAuthoredInterval();
void TestBossAreaUsesAuthoredTickInterval();
void TestRangedWarningAndExplosiveArea();
void TestHighFanoutChainsTerminate();
void TestRicochetCooldownBudget();
void TestIncomingDamageRelics();

void TestArrowReleaseCarriesAuthoritativeDirection()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({0x4152524F57u}, QuietGameData()).Succeeded(),
          "arrow release geometry initialize");
    hs::HeldInputState held;
    held.basic_attack_held = true;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    const hs::DomainSignal *release = nullptr;
    for (std::uint32_t tick = 0; tick < 30 && release == nullptr; ++tick)
    {
        (void)Tick(simulation, held);
        const auto signals = simulation.PendingDomainSignals();
        const auto found = std::ranges::find_if(
            signals, [](const hs::DomainSignal &signal) {
                return signal.kind == hs::DomainSignalKind::ArrowReleased;
            });
        if (found != signals.end()) release = &*found;
    }
    Check(release != nullptr && std::abs(release->direction.x - 1.0f) < 0.0001f &&
              std::abs(release->direction.z) < 0.0001f &&
              std::abs(release->position.y - 1.05f) < 0.0001f,
          "arrow release keeps normalized aim direction and release height");
    Check(simulation.Shutdown().Succeeded(), "arrow release geometry shutdown");
}

void TestBasicAttackStopsAtFirstEnemy()
{
    auto data = QuietGameData();
    data.enemies[0].health = 100;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({142}, data).Succeeded(), "basic pierce initialize");
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 0.0f});
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {7.0f, 0.0f});
    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    held.basic_attack_held = true;
    (void)Tick(simulation, held);
    held.basic_attack_held = false;
    for (std::uint32_t tick = 0; tick < 30; ++tick) (void)Tick(simulation, held);
    Check(simulation.GetObservation().damage_by_skill[0] == 10,
          "basic arrow damages only the first enemy without an upgrade");
    Check(simulation.Shutdown().Succeeded(), "basic pierce shutdown");
}

void TestEnemyLookupAcrossCleanupAndSpawn()
{
    auto data = QuietGameData();
    data.enemies[0].health = 1;
    data.enemies[0].move_speed = 0.0f;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({143}, data).Succeeded(), "enemy lookup initialize");
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 10.0f});
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 0.0f});
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, -10.0f});
    hs::GameReadModelStorage model;
    hs::CopyReadModelForDevelopment(simulation, model);
    const auto first = model.View().enemies.front().id;
    const auto last = model.View().enemies.back().id;
    const auto attack = [&] {
        hs::HeldInputState held;
        held.aim_world = {20.0f, 0.0f, 0.0f};
        held.basic_attack_held = true;
        (void)Tick(simulation, held);
        held.basic_attack_held = false;
        for (unsigned tick = 0; tick < 90; ++tick) (void)Tick(simulation, held);
        hs::CopyReadModelForDevelopment(simulation, model);
        Check(model.View().enemies.size() == 2 &&
                    model.View().enemies.front().id.value == first.value &&
                    model.View().enemies.back().id.value == last.value,
              "target lookup preserves surviving IDs across middle removal and respawn");
    };
    attack();
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 0.0f});
    attack();
    Debug(simulation, hs::DebugCommandKind::StartSession);
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 0.0f});
    hs::CopyReadModelForDevelopment(simulation, model);
    Check(model.View().enemies.size() == 1 &&
              model.View().enemies.front().id.value > last.value,
          "session reset clears actors without invalidating ID ordering");
    Check(simulation.Shutdown().Succeeded(), "enemy lookup shutdown");
}

void TestBasicAttackSkillCancellationAndBufferRules()
{
    {
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0xCA11u}, QuietGameData()).Succeeded(), "cast cancellation initialize");
        Debug(simulation, hs::DebugCommandKind::GrantSkill, static_cast<std::uint64_t>(hs::SkillKind::PiercingShot));
        hs::HeldInputState held; held.aim_world = {20.0f, 0.0f, 0.0f}; held.basic_attack_held = true;
        (void)Tick(simulation, held);
        hs::Sequence sequence{};
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
        held.basic_attack_held = false;
        bool basic_emitted{}, skill_emitted{};
        for (std::uint32_t i = 0; i < 30; ++i)
        {
            (void)Tick(simulation, held);
            hs::GameReadModelStorage model; hs::CopyReadModelForDevelopment(simulation, model);
            for (const auto &projectile : model.View().projectiles)
            {
                basic_emitted |= projectile.skill == hs::SkillKind::BasicAttack;
                skill_emitted |= projectile.skill == hs::SkillKind::PiercingShot;
            }
        }
        Check(!basic_emitted && skill_emitted,
              "successful skill cancels only the pending basic release");
        Check(simulation.Shutdown().Succeeded(), "cast cancellation shutdown");
    }
    {
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0xCA12u}, QuietGameData()).Succeeded(), "failed skill preservation initialize");
        hs::HeldInputState held; held.aim_world = {20.0f, 0.0f, 0.0f}; held.basic_attack_held = true;
        (void)Tick(simulation, held);
        hs::Sequence sequence{};
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
        held.basic_attack_held = false;
        bool basic_emitted{};
        for (std::uint32_t i = 0; i < 30; ++i)
        {
            (void)Tick(simulation, held);
            hs::GameReadModelStorage model; hs::CopyReadModelForDevelopment(simulation, model);
            for (const auto &projectile : model.View().projectiles)
                basic_emitted |= projectile.skill == hs::SkillKind::BasicAttack;
        }
        Check(basic_emitted, "failed skill leaves pending basic release intact");
        Check(simulation.Shutdown().Succeeded(), "failed skill preservation shutdown");
    }
    {
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0xCA13u}, QuietGameData()).Succeeded(), "tap buffer initialize");
        Debug(simulation, hs::DebugCommandKind::GrantSkill, static_cast<std::uint64_t>(hs::SkillKind::PiercingShot));
        hs::HeldInputState held; held.aim_world = {20.0f, 0.0f, 0.0f};
        hs::Sequence sequence{};
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Released, sequence, held);
        while (simulation.GetObservation().cooldown_ticks[0] > 9) (void)Tick(simulation, held);
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Released, sequence, held);
        bool recast{};
        for (std::uint32_t i = 0; i < 12; ++i) { (void)Tick(simulation, held); recast |= simulation.GetObservation().cooldown_ticks[0] > 9; }
        Check(recast, "tap release preserves buffered skill intent until recovery");
        Check(simulation.Shutdown().Succeeded(), "tap buffer shutdown");
    }
}

void TestQwerInputBuffer()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({15}, QuietGameData()).Succeeded(),
          "input buffer initialize");
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::PiercingShot));

    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    while (simulation.GetObservation().cooldown_ticks[0] > 9)
    {
        (void)Tick(simulation, held);
    }
    Check(simulation.GetObservation().cooldown_ticks[0] == 9,
          "skill reaches input buffer window");

    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().cooldown_ticks[0] == 8,
          "early Q is buffered instead of cast immediately");
    bool buffered_cast{};
    for (std::uint32_t attempt = 0; attempt < 9; ++attempt)
    {
        (void)Tick(simulation, held);
        if (simulation.GetObservation().cooldown_ticks[0] > 9)
        {
            buffered_cast = true;
            break;
        }
    }
    Check(buffered_cast, "buffered Q casts when cooldown becomes ready");
    Check(simulation.Shutdown().Succeeded(), "input buffer shutdown");
}

void TestAttacksRespectTerrain()
{
    const auto player_damage = [](bool wall, float enemy_x) {
        auto data = QuietGameData();
        data.arena_obstacle_count = wall ? 1 : 0;
        data.arena_obstacles[0] = {hs::ArenaObstacleKind::Rock, {5.0f, 0.0f}, 1.0f};
        data.enemies[0].health = 1000;
        data.enemies[0].move_speed = 0.0f;
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0x0B51u}, data).Succeeded(), "player terrain initialize");
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {enemy_x, 0});
        hs::HeldInputState held; held.aim_world = {20, 0, 0}; held.basic_attack_held = true;
        (void)Tick(simulation, held); held.basic_attack_held = false;
        for (int i = 0; i < 80; ++i) (void)Tick(simulation, held);
        const auto damage = simulation.GetObservation().damage_by_skill[0];
        Check(simulation.Shutdown().Succeeded(), "player terrain shutdown");
        return damage;
    };
    Check(player_damage(true, 8.0f) == 0, "rock blocks player arrow behind it");
    Check(player_damage(false, 8.0f) > 0, "clear control arrow reaches same enemy");
    Check(player_damage(true, 2.0f) > 0, "enemy before rock receives arrow damage");
    const auto incoming_damage = [](hs::EnemyKind kind, bool wall) {
        auto data = QuietGameData();
        data.arena_obstacle_count = wall ? 1 : 0;
        data.arena_obstacles[0] = {hs::ArenaObstacleKind::Tree, {2.5f, 0.0f}, 0.5f};
        auto &enemy = data.enemies[static_cast<std::size_t>(kind)];
        enemy.move_speed = 0.0f;
        enemy.attack_range = 7.0f;
        enemy.projectile_range = 20.0f;
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0x0B52u}, data).Succeeded(), "enemy terrain initialize");
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy, static_cast<std::uint64_t>(kind), 0, {5, 0});
        const auto health = simulation.GetObservation().health;
        for (int i = 0; i < 180; ++i) (void)Tick(simulation);
        const auto damage = health - simulation.GetObservation().health;
        Check(simulation.Shutdown().Succeeded(), "enemy terrain shutdown");
        return damage;
    };
    for (const auto kind : {hs::EnemyKind::Melee, hs::EnemyKind::Ranged})
    {
        Check(incoming_damage(kind, true) == 0, "tree blocks enemy attack against player");
        Check(incoming_damage(kind, false) > 0, "clear control enemy attack damages player");
    }
}

void TestRedirectedArrowUsesRemainingTick()
{
    std::array<hs::GameplayChecksum, 2> checksums{};
    for (const auto repeat : {false, true})
    for (const bool returning : {false, true})
    {
        auto data = QuietGameData();
        data.arena_obstacle_count = 1;
        data.arena_obstacles[0] = {hs::ArenaObstacleKind::Rock, {1.1f, -0.5f}, 0.1f};
        data.enemies[0].health = 1000;
        data.enemies[0].move_speed = 0;
        data.enemies[0].damage = 0;
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0xC05u}, data).Succeeded(), "redirected arrow initialize");
        Debug(simulation, hs::DebugCommandKind::GrantSkill,
              static_cast<std::uint64_t>(hs::SkillKind::RicochetArrow));
        Debug(simulation, hs::DebugCommandKind::GrantUpgrade,
              static_cast<std::uint64_t>(hs::SkillKind::RicochetArrow), returning ? 0 : 1);
        hs::HeldInputState held; held.aim_world = {20, 0, 0};
        hs::Sequence sequence{};
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
        hs::GameReadModelStorage model;
        hs::ProjectileView before;
        for (int i = 0; i < 30 && before.id.value == 0; ++i)
        {
            hs::CopyReadModelForDevelopment(simulation, model);
            if (!model.View().projectiles.empty()) before = model.View().projectiles.front();
            else (void)Tick(simulation, held);
        }
        Check(before.id.value != 0 && std::abs(before.position.x - 28.0f / 60.0f) < 0.0001f,
              "fixture launches one unhomed authored-speed arrow");
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {0.7f, 1.069f});
        if (!returning)
            Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {-1.3f, 2});
        const auto result = Tick(simulation, held);
        hs::CopyReadModelForDevelopment(simulation, model);
        const auto view = model.View();
        const auto arrow = std::ranges::find_if(view.projectiles, [&](const auto &p) {
            return p.id.value == before.id.value;
        });
        Check(arrow != view.projectiles.end(), "redirected arrow survives the obsolete wall intersection");
        Check(arrow->returning == returning && arrow->position.x < 0.66f,
              "redirect uses its contact point and travels only the remaining tick");
        Check(std::abs(arrow->previous_position.x - before.position.x) < 0.0001f,
              "render interpolation retains the real tick start");
        if (returning)
        {
            Check(std::abs(arrow->return_start_position.x - 0.6537507f) < 0.0001f &&
                      std::abs(arrow->position.y) < 0.0001f,
                  "return starts at contact and heads back toward the player");
        }
        else
        {
            Check(std::abs(arrow->position.x - 0.4583818f) < 0.0002f &&
                      std::abs(arrow->position.y - 0.1999937f) < 0.0002f,
                  "ricochet endpoint matches the contact-to-target remaining-time segment");
            const auto child = std::ranges::find_if(view.projectiles, [](const auto &p) {
                return p.origin == hs::EffectOrigin::Derived && p.source_upgrade == 1;
            });
            Check(child != view.projectiles.end() && child->spawned_tick == result.tick &&
                      std::abs(child->position.x - 0.6537507f) < 0.0001f &&
                      child->position.x == child->previous_position.x &&
                      child->position.y == child->previous_position.y,
                  "split child starts at contact and cannot travel during its spawn tick");
        }
        Check(simulation.GetObservation().damage_by_skill[5] > 0, "first enemy still receives damage");
        Check(std::ranges::count_if(simulation.PendingDomainSignals(), [&](const auto &signal) {
                  return signal.tick == result.tick && signal.kind == hs::DomainSignalKind::RicochetArrowHit;
              }) == 1, "redirect departure cannot immediately rehit the same contact");
        if (repeat) Check(checksums[returning] == result.checksum, "redirected paths replay deterministically");
        else checksums[returning] = result.checksum;
        Check(simulation.Shutdown().Succeeded(), "redirected arrow shutdown");
    }
}

void TestProjectilesHitMovingTargets()
{
    struct Case { float projectile_speed, enemy_speed; bool hit; };
    for (const auto fixture : {Case{0, 0, false}, Case{0, 720, true},
                                Case{600, 720, true}, Case{600, 0, true}})
    {
        hs::GameplayChecksum first{};
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            auto data = QuietGameData();
            data.arena_obstacle_count = 0;
            data.enemies[0].health = 1000;
            data.enemies[0].move_speed = fixture.enemy_speed;
            data.enemies[0].damage = 0;
            data.enemies[0].attack_range = 0;
            data.skills[0].projectile_speed = fixture.projectile_speed;
            data.skills[5].projectile_speed = fixture.projectile_speed;
            data.skills[5].range = 100;
            hs::GameSimulation simulation;
            Check(simulation.Initialize({0xC010u}, data).Succeeded(), "moving target initialize");
            Debug(simulation, hs::DebugCommandKind::GrantSkill,
                  static_cast<std::uint64_t>(hs::SkillKind::RicochetArrow));
            hs::HeldInputState held; held.aim_world = {40, 0, 0};
            hs::Sequence sequence{};
            (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
            hs::GameReadModelStorage model;
            hs::ProjectileView before;
            for (int i = 0; i < 30 && before.id.value == 0; ++i)
            {
                hs::CopyReadModelForDevelopment(simulation, model);
                if (!model.View().projectiles.empty()) before = model.View().projectiles.front();
                else (void)Tick(simulation, held);
            }
            Check(before.id.value != 0, "moving target fixture launches an original arrow");
            Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0,
                  {before.position.x + 6, fixture.projectile_speed > 0 ? .6f : 0});
            const auto result = Tick(simulation, held);
            hs::CopyReadModelForDevelopment(simulation, model);
            const auto enemies = model.View().enemies;
            Check(enemies.size() == 1 && (enemies[0].health < 1000) == fixture.hit,
                  "moving target sweep catches intermediate contacts and preserves static controls");
            if (fixture.enemy_speed > 0)
                Check(enemies[0].position.x < before.position.x - 4,
                      "crossing fixture ends outside the projectile's final-center broadphase cells");
            if (repeat) Check(first == result.checksum, "moving target collision replay is deterministic");
            else first = result.checksum;
            Check(simulation.Shutdown().Succeeded(), "moving target shutdown");
        }
    }
}

void TestMovingPlayerHitsStationaryEnemyProjectile()
{
    auto data = QuietGameData();
    data.arena_obstacle_count = 0;
    data.stats.base_movement_speed_mps = 720;
    data.enemies[1].move_speed = 0;
    data.enemies[1].projectile_speed = 0;
    data.enemies[1].attack_range = 10;
    data.enemies[1].damage = 5;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({0xC011u}, data).Succeeded(), "moving player collision initialize");
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy,
          static_cast<std::uint64_t>(hs::EnemyKind::Ranged), 0, {6, 0});
    hs::GameReadModelStorage model;
    hs::ProjectileView incoming;
    for (int i = 0; i < 120 && incoming.id.value == 0; ++i)
    {
        (void)Tick(simulation);
        hs::CopyReadModelForDevelopment(simulation, model);
        const auto found = std::ranges::find_if(model.View().projectiles,
            [](const auto &projectile) { return !projectile.player_owned; });
        if (found != model.View().projectiles.end()) incoming = *found;
    }
    Check(incoming.id.value != 0 && std::abs(incoming.position.x - 6) < .001f,
          "moving player fixture creates a real stationary ranged projectile");
    const auto health = simulation.GetObservation().health;
    hs::HeldInputState held; held.move_held = true; held.move_target_world = {20, 0, 0};
    (void)Tick(simulation, held);
    Check(simulation.GetObservation().player_position.x > incoming.position.x + 2 &&
              simulation.GetObservation().health < health,
          "enemy projectile hits a player crossing during the tick rather than only its endpoint");
    Check(simulation.Shutdown().Succeeded(), "moving player collision shutdown");
}

void TestRicochetClipsMovingTargetTime()
{
    auto data = QuietGameData();
    data.arena_obstacle_count = 0;
    for (const auto kind : {hs::EnemyKind::Melee, hs::EnemyKind::Ranged})
    {
        auto &enemy = data.enemies[static_cast<std::size_t>(kind)];
        enemy.health = 1000;
        enemy.move_speed = 0;
        enemy.damage = 0;
        enemy.attack_range = 0;
        enemy.collision_radius = .1f;
    }
    data.enemies[1].move_speed = 120;
    auto &skill = data.skills[5];
    skill.projectile_speed = 600;
    skill.collision_radius = .1f;
    skill.range = 100;
    skill.pierce_count = 1;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({0xC012u}, data).Succeeded(), "moving ricochet initialize");
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::RicochetArrow));
    hs::HeldInputState held; held.aim_world = {40, 0, 0};
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
    hs::GameReadModelStorage model;
    hs::ProjectileView before;
    for (int i = 0; i < 30 && before.id.value == 0; ++i)
    {
        hs::CopyReadModelForDevelopment(simulation, model);
        if (!model.View().projectiles.empty()) before = model.View().projectiles.front();
        else (void)Tick(simulation, held);
    }
    Check(before.id.value != 0 && std::abs(before.position.x - 10) < .001f,
          "moving ricochet fixture launches one ten-metre step");
    // The first hit is at 0.38 ticks; replaying the second target's whole path misses it.
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {14, 0});
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 1, 0, {11, 1.5f});
    const auto result = Tick(simulation, held);
    hs::CopyReadModelForDevelopment(simulation, model);
    Check(model.View().enemies.size() == 2 &&
              std::ranges::all_of(model.View().enemies, [](const auto &enemy) { return enemy.health < 1000; }),
          "ricochet tests the moving target only during the remaining global tick interval");
    Check(std::ranges::count_if(simulation.PendingDomainSignals(), [&](const auto &signal) {
              return signal.tick == result.tick && signal.kind == hs::DomainSignalKind::RicochetArrowHit;
          }) == 2, "moving ricochet hits each target once in the same tick");
    Check(simulation.Shutdown().Succeeded(), "moving ricochet shutdown");
}

void TestPiercingAndWallContactOrder()
{
    const auto run = [](bool tie, bool wall) {
        auto data = QuietGameData();
        data.arena_obstacle_count = wall ? 1 : 0;
        data.arena_obstacles[0] = tie
            ? hs::ArenaObstacle2D{hs::ArenaObstacleKind::Rock, {5, 1}, 0.63f}
            : hs::ArenaObstacle2D{hs::ArenaObstacleKind::Rock, {6, 0}, 0.3f};
        data.enemies[0].health = 1000;
        data.enemies[0].move_speed = 0;
        data.enemies[0].damage = 0;
        data.skills[0].projectile_speed = 600;
        data.skills[0].collision_radius = 0.44f;
        data.skills[0].pierce_count = 3;
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0xC051u}, data).Succeeded(), "piercing order initialize");
        if (tie) Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5, -1});
        else
            for (const auto x : {2.0f, 4.0f, 8.0f})
                Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {x, 0});
        hs::HeldInputState held; held.aim_world = {20, 0, 0}; held.basic_attack_held = true;
        (void)Tick(simulation, held); held.basic_attack_held = false;
        for (int i = 0; i < 30; ++i) (void)Tick(simulation, held);
        hs::GameReadModelStorage model;
        hs::CopyReadModelForDevelopment(simulation, model);
        std::vector<std::int32_t> health;
        for (const auto &enemy : model.View().enemies) health.push_back(enemy.health);
        Check(simulation.Shutdown().Succeeded(), "piercing order shutdown");
        return health;
    };
    const auto pierce = run(false, true);
    Check(pierce.size() == 3 && pierce[0] < 1000 && pierce[1] < 1000 && pierce[2] == 1000,
          "one straight segment pierces both front enemies and stops at the wall");
    const auto tie = run(true, true), clear = run(true, false);
    Check(tie.size() == 1 && clear.size() == 1 && tie[0] == 1000 && clear[0] < 1000,
          "opaque terrain wins an equal actor TOI without dropping the clear control hit");
}

void TestZeroTimeRicochetsAreBounded()
{
    auto data = QuietGameData();
    data.arena_obstacle_count = 0;
    data.enemies[0].health = 10000;
    data.enemies[0].move_speed = 0;
    data.enemies[0].damage = 0;
    data.skills[5].collision_radius = 100;
    data.skills[5].pierce_count = 200;
    data.skills[5].ricochet_search_radius = 32;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({0xC052u}, data).Succeeded(), "zero-time ricochet initialize");
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::RicochetArrow));
    for (int i = 0; i < 80; ++i)
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0,
              {2.0f + static_cast<float>(i % 10), static_cast<float>(i / 10)});
    hs::HeldInputState held; held.aim_world = {20, 0, 0};
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
    bool progressed{};
    hs::GameReadModelStorage model;
    for (int i = 0; i < 30 && !progressed; ++i)
    {
        (void)Tick(simulation, held);
        hs::CopyReadModelForDevelopment(simulation, model);
        const auto enemies = model.View().enemies;
        const auto hits = std::ranges::count_if(enemies, [](const auto &enemy) { return enemy.health < 10000; });
        if (hits == 0) continue;
        Check(hits < 80, "zero-time chains leave remaining contacts for a later tick");
        progressed = true;
    }
    Check(progressed, "zero-time fixture reaches its first contacts");
    for (int i = 0; i < 4; ++i) (void)Tick(simulation, held);
    hs::CopyReadModelForDevelopment(simulation, model);
    Check(std::ranges::all_of(model.View().enemies, [](const auto &enemy) { return enemy.health < 10000; }),
          "a bounded zero-time chain resumes and eventually visits every target");
    Check(simulation.Shutdown().Succeeded(), "zero-time ricochet shutdown");
}

void TestChargeBufferAndRecovery()
{
    const auto player = [](hs::GameSimulation &simulation) {
        hs::GameReadModelStorage model; hs::CopyReadModelForDevelopment(simulation, model);
        return model.View().player;
    };
    // A charge started during windup replaces the pending basic projectile.
    {
        hs::GameSimulation simulation;
        auto data = QuietGameData(); data.arena_obstacle_count = 0;
        Check(simulation.Initialize({0xC401u}, data).Succeeded(), "charge cancel initialize");
        Debug(simulation, hs::DebugCommandKind::GrantSkill, static_cast<std::uint64_t>(hs::SkillKind::ChargedShot));
        hs::HeldInputState held; held.aim_world = {20, 0, 0}; held.basic_attack_held = true;
        (void)Tick(simulation, held); held.basic_attack_held = false;
        hs::Sequence sequence{};
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
        Check(player(simulation).charging, "charge starts during basic windup");
        bool basic_emitted{};
        for (int i = 0; i < 30; ++i)
        {
            (void)Tick(simulation, held);
            hs::GameReadModelStorage model; hs::CopyReadModelForDevelopment(simulation, model);
            for (const auto &shot : model.View().projectiles)
                basic_emitted |= shot.skill == hs::SkillKind::BasicAttack;
        }
        Check(!basic_emitted, "charge cancels pending basic emission");
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Released, sequence, held);
        const auto recovery_end = player(simulation).active_cast_tick;
        Check(!player(simulation).charging && recovery_end > simulation.GetObservation().tick,
              "charge release retains recovery");
        held.basic_attack_held = true;
        const auto uses = simulation.GetObservation().balance.skill_uses[0];
        while (simulation.GetObservation().tick + 1 < recovery_end)
        {
            (void)Tick(simulation, held);
            Check(simulation.GetObservation().balance.skill_uses[0] == uses,
                  "held basic cannot bypass charged shot recovery");
        }
        Check(simulation.Shutdown().Succeeded(), "charge cancel shutdown");
    }
    // Skills cancel basic recovery while preserving the already released arrow.
    for (const auto skill : {hs::SkillKind::PiercingShot, hs::SkillKind::ChargedShot})
    {
        hs::GameSimulation simulation;
        auto data = QuietGameData(); data.arena_obstacle_count = 0;
        Check(simulation.Initialize({0xC403u}, data).Succeeded(), "basic recovery initialize");
        Debug(simulation, hs::DebugCommandKind::GrantSkill, static_cast<std::uint64_t>(skill));
        hs::HeldInputState held; held.aim_world = {20, 0, 0}; held.basic_attack_held = true;
        (void)Tick(simulation, held); held.basic_attack_held = false;
        bool emitted{};
        for (int i = 0; i < 60 && !emitted; ++i)
        {
            (void)Tick(simulation, held);
            emitted = simulation.GetObservation().player_projectile_count != 0;
        }
        const auto recovery_end = player(simulation).basic_attack_animation_until;
        Check(emitted && simulation.GetObservation().tick + 1 < recovery_end,
              "basic emission precedes recovery boundary");
        hs::Sequence sequence{};
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
        if (skill == hs::SkillKind::ChargedShot)
            Check(player(simulation).charging, "charge cancels basic recovery immediately");
        else
            Check(simulation.GetObservation().balance.skill_uses[static_cast<std::size_t>(skill)] == 1,
                  "instant skill cancels basic recovery immediately");
        Check(player(simulation).basic_attack_animation_until <= simulation.GetObservation().tick,
              "successful skill ends the basic animation");
        hs::GameReadModelStorage model; hs::CopyReadModelForDevelopment(simulation, model);
        bool basic_survives{};
        for (const auto &shot : model.View().projectiles)
            basic_survives |= shot.skill == hs::SkillKind::BasicAttack;
        Check(basic_survives, "recovery cancel preserves the released basic arrow");
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Released, sequence, held);
        held.basic_attack_held = true;
        const auto uses = simulation.GetObservation().balance.skill_uses[0];
        while (simulation.GetObservation().tick + 1 < recovery_end)
        {
            (void)Tick(simulation, held);
            Check(simulation.GetObservation().balance.skill_uses[0] == uses,
                  "skill cancel does not reset the basic attack rate limiter");
        }
        Check(simulation.Shutdown().Succeeded(), "basic recovery shutdown");
    }
    // Queue near charge cooldown completion; release either cancels intent or ends started charge.
    for (const bool release_before_ready : {true, false})
    {
        hs::GameSimulation simulation;
        auto data = QuietGameData(); data.arena_obstacle_count = 0;
        Check(simulation.Initialize({0xC402u}, data).Succeeded(), "queued charge initialize");
        Debug(simulation, hs::DebugCommandKind::GrantSkill, static_cast<std::uint64_t>(hs::SkillKind::ChargedShot));
        hs::HeldInputState held; held.aim_world = {20, 0, 0}; hs::Sequence sequence{};
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Released, sequence, held);
        const auto cooldown_index = static_cast<std::size_t>(hs::SkillKind::ChargedShot) - 1;
        for (int i = 0; i < 2000 && simulation.GetObservation().cooldown_ticks[cooldown_index] > 5; ++i)
            (void)Tick(simulation, held);
        Check(simulation.GetObservation().cooldown_ticks[cooldown_index] == 5, "charge reaches bounded buffer window");
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
        Check(!player(simulation).charging, "queued charge does not bypass cooldown");
        if (release_before_ready)
            (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Released, sequence, held);
        for (int i = 0; i < 8; ++i) (void)Tick(simulation, held);
        Check(player(simulation).charging == !release_before_ready,
              "released queued charge cancels while held queued charge starts");
        if (!release_before_ready)
        {
            (void)TickEdge(simulation, hs::GameAction::SkillW, hs::EdgeKind::Released, sequence, held);
            Check(player(simulation).charging, "unrelated slot release cannot end queued charge");
            (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Released, sequence, held);
            Check(!player(simulation).charging && simulation.GetObservation().cooldown_ticks[cooldown_index] > 5,
                  "original slot release fires queued charge and applies cooldown");
        }
        Check(simulation.Shutdown().Succeeded(), "queued charge shutdown");
    }
}

void RunGameplayCombatTests()
{
    TestAttacksRespectTerrain();
    TestRedirectedArrowUsesRemainingTick();
    TestProjectilesHitMovingTargets();
    TestMovingPlayerHitsStationaryEnemyProjectile();
    TestRicochetClipsMovingTargetTime();
    TestPiercingAndWallContactOrder();
    TestZeroTimeRicochetsAreBounded();
    TestRicochetCooldownBudget();
    TestChargeBufferAndRecovery();
    TestArrowReleaseCarriesAuthoritativeDirection();
    TestSkillMovementPauseAndResume();
    TestBasicAttackStopsAtFirstEnemy();
    TestEnemyLookupAcrossCleanupAndSpawn();
    TestBossAreaUsesAuthoredTickInterval();
    TestQwerInputBuffer();
    TestBasicAttackSkillCancellationAndBufferRules();
    TestChargedShotCancelsForLevelSelection();
    TestAuthoritativeCombatGeometryEmission();
    TestArrowRainFireAreasUseTheirAuthoredInterval();
    TestTrapFireAreasUseTheirAuthoredInterval();
    TestRangedWarningAndExplosiveArea();
    TestIncomingDamageRelics();
    TestHighFanoutChainsTerminate();
}

} // namespace gameplay_test
