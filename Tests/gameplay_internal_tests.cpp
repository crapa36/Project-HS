#include "balance_observer.hpp"
#include "simulation_pipeline.hpp"
#include "spatial_grid.hpp"
#include "status_state.hpp"
#include "cast_runtime.hpp"
#include "simulation_world.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <map>
#include <ranges>
#include <stdexcept>
#include <vector>

namespace hs
{
struct GameplayTestAccess
{
    using World = GameSimulation::SimulationWorld;
    static World &State(GameSimulation &simulation) { return *simulation.impl_; }
};
}

namespace
{
void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void TestActorColumnStorage()
{
    using namespace hs::gameplay_detail;
    static_assert(std::ranges::input_range<EnemyStore>);
    static_assert(std::ranges::input_range<const EnemyStore>);
    static_assert(std::ranges::input_range<ProjectileStore>);
    static_assert(sizeof(EnemyRow) <= 8 * sizeof(void *));
    static_assert(sizeof(ProjectileRow) <= 6 * sizeof(void *));
    static_assert(std::is_const_v<std::remove_reference_t<
        decltype(std::declval<const EnemyStore &>()[0].motion)>>);
    EnemyStore enemies, pending;
    for (std::uint64_t id = 1; id <= 4; ++id)
    {
        PreparedEnemy value;
        value.id = {id};
        value.motion.position = {static_cast<float>(id), -static_cast<float>(id)};
        value.life.health = static_cast<std::int32_t>(id * 10);
        value.action.attack_cast_id = id * 100;
        value.cold.random_key = id * 1000;
        value.status.slows.push_back({.25f, id + 10});
        (id == 4 ? pending : enemies).push_back(std::move(value));
    }
    const auto *slow_storage = pending.status[0].slows.data();
    const auto pending_capacity = pending.capacity();
    enemies.AppendFrom(pending);
    Check(enemies.Consistent() && pending.Consistent() && pending.empty() &&
              pending.capacity() == pending_capacity && enemies.size() == 4,
          "pending enemy commit moves all columns and retains preparation capacity");
    Check(enemies.status[3].slows.data() == slow_storage,
          "pending status ownership moves without a deep-copy mirror");
    {
        auto row = enemies[1];
        Check(&row.motion == &enemies.motion[1] && &row.life == &enemies.life[1],
              "row proxies borrow actual grouped columns");
        row.life.dead = true;
        row.cold.last_damage_cast = 91;
        Check(enemies.life[1].dead && enemies.cold[1].last_damage_cast == 91,
              "kernel writes update the sole authoritative payload");
    }
    const auto generation = enemies.StructuralGeneration();
    Check(enemies.EraseIf([](ConstEnemyRow) { return false; }) == 0 &&
              enemies.StructuralGeneration() == generation,
          "no-op enemy erase advanced structural generation");
    bool predicate_threw{};
    try
    {
        static_cast<void>(enemies.EraseIf([](ConstEnemyRow) -> bool {
            throw std::runtime_error("predicate failure");
        }));
    }
    catch (const std::runtime_error &) { predicate_threw = true; }
    Check(predicate_threw && enemies.StructuralGeneration() == generation,
          "throwing no-op enemy predicate changed structural generation");
    bool rejected{};
    try { enemies.reserve(std::numeric_limits<std::size_t>::max()); }
    catch (const std::length_error &) { rejected = true; }
    Check(rejected && enemies.Consistent() && enemies.size() == 4 &&
              enemies.StructuralGeneration() == generation && enemies.ids[3].value == 4,
          "invalid reserve preserves every logical column and row");
    enemies.EraseIf([](ConstEnemyRow row) { return row.life.dead; });
    for (std::size_t row = 0; row < enemies.size(); ++row)
    {
        const auto id = row == 0 ? 1u : static_cast<unsigned>(row + 2);
        const auto value = std::as_const(enemies)[row];
        Check(value.id.value == id && value.motion.position.x == static_cast<float>(id) &&
                  value.life.health == static_cast<std::int32_t>(id * 10) &&
                  value.action.attack_cast_id == id * 100 &&
                  value.cold.random_key == id * 1000 &&
                  value.status.slows[0].expires == id + 10,
              "stable compaction preserves group association and survivor order");
    }
    Check(std::ranges::count_if(enemies, [](ConstEnemyRow row) {
        return row.life.health > 0;
    }) == 3, "const semantic readers iterate scoped group references");

    ProjectileStore projectiles, pending_projectiles;
    for (const auto id : {40u, 8u, 90u})
    {
        PreparedProjectile value;
        value.id = {id};
        value.motion.remaining_range = static_cast<float>(id);
        value.combat.cast_id = id + 100;
        value.combat.hit_ids.push_back(id + 200);
        value.episodes.bleed_extend_ticks = {id + 300, id + 301};
        pending_projectiles.push_back(std::move(value));
    }
    const auto *episode_storage = pending_projectiles.episodes[2].bleed_extend_ticks.data();
    projectiles.AppendFrom(pending_projectiles);
    Check(projectiles.ids[0].value == 40 && projectiles.ids[1].value == 8 &&
              projectiles.ids[2].value == 90,
          "projectile publication order does not assume sorted logical IDs");
    projectiles.EraseIf([](ConstProjectileRow row) { return row.id.value == 8; });
    Check(projectiles.Consistent() && projectiles.size() == 2 &&
              projectiles.ids[1].value == 90 && projectiles.combat[1].cast_id == 190 &&
              projectiles.combat[1].hit_ids[0] == 290 &&
              projectiles.episodes[1].bleed_extend_ticks[1] == 391 &&
              projectiles.episodes[1].bleed_extend_ticks.data() == episode_storage,
          "projectile stable compaction moves hit and episode owners with the same row");
    const auto capacity = projectiles.capacity();
    const auto projectile_generation = projectiles.StructuralGeneration();
    Check(projectiles.EraseIf([](ConstProjectileRow) { return false; }) == 0 &&
              projectiles.StructuralGeneration() == projectile_generation,
          "no-op projectile erase advanced structural generation");
    projectiles.clear();
    Check(projectiles.Consistent() && projectiles.empty() && projectiles.capacity() == capacity &&
              projectiles.StructuralGeneration() == projectile_generation + 1,
          "session clear retains all column capacities");
    const auto empty_generation = projectiles.StructuralGeneration();
    projectiles.clear();
    Check(projectiles.StructuralGeneration() == empty_generation,
          "empty projectile clear advanced structural generation");
    PreparedProjectile restarted;
    restarted.id = {105};
    projectiles.push_back(std::move(restarted));
    Check(projectiles.combat[0].source_upgrade == kNoTelemetrySource &&
              projectiles.combat[0].skill == hs::SkillKind::BasicAttack &&
              projectiles.episodes[0].bleed_extend_ticks.empty(),
          "reused columns retain original spawn defaults without stale owning payload");
}

void TestSmallFactsIgnoreNoOpMutation()
{
    hs::GameSimulation simulation;
    hs::SimulationConfig config;
    config.start_in_main_menu = true;
    config.capture_checksums = true;
    Check(simulation.Initialize(config).Succeeded(), "small-facts fixture initialization");
    auto &world = hs::GameplayTestAccess::State(simulation);
    {
        const auto read = simulation.BorrowTickReadView();
        Check(!world.observation.small_facts_dirty, "borrow did not refresh initial small facts");
    }
    {
        hs::GameplayTestAccess::World::MutationScope mutation(world);
        Check(!world.observation.small_facts_dirty && world.observation.checksum_dirty,
              "no-op ownership scope invalidated small facts or stopped invalidating checksum");
    }
    hs::InputFrame input;
    input.held.aim_world = {world.actors->player.position.x, 0.0f, world.actors->player.position.y};
    const auto no_op = simulation.TickFixed(input, std::chrono::nanoseconds{16'666'667});
    Check(!no_op.world_changed && !world.observation.small_facts_dirty,
          "frozen no-op tick invalidated collection-derived facts");
    const auto valid_aim = world.actors->player.aim;
    for (const auto component : {std::numeric_limits<float>::max(),
                                std::numeric_limits<float>::quiet_NaN()})
    {
        input.held.aim_world = {component, 0.0f, component};
        const auto invalid_aim = simulation.TickFixed(input, std::chrono::nanoseconds{16'666'667});
        Check(!invalid_aim.world_changed && world.actors->player.aim.x == valid_aim.x &&
              world.actors->player.aim.y == valid_aim.y,
              "unrepresentable aim changed the last valid firing direction");
    }
    simulation.ApplyUiAction({hs::UiActionKind::Resume});
    Check(!world.observation.small_facts_dirty,
          "rejected UI action invalidated small facts");
    simulation.ApplyUiAction({hs::UiActionKind::StartSession});
    Check(world.observation.small_facts_dirty,
          "accepted session action failed to invalidate small facts");
    {
        const auto read = simulation.BorrowTickReadView();
        Check(read.View().session.phase == hs::SessionPhase::Playing &&
                  !world.observation.small_facts_dirty,
              "accepted action was not reflected by fresh small facts");
    }
    Check(simulation.Shutdown().Succeeded(), "small-facts fixture shutdown");
}

void TestSimulationIdentityExhaustion()
{
    using namespace hs::gameplay_detail;
    auto next = std::numeric_limits<std::uint64_t>::max() - 1;
    Check(TakeSimulationIdentity(next) == std::numeric_limits<std::uint64_t>::max() - 1,
          "logical IDs preserve the original allocation order");
    bool exhausted{};
    try { (void)TakeSimulationIdentity(next); }
    catch (const std::overflow_error &) { exhausted = true; }
    Check(exhausted && next == std::numeric_limits<std::uint64_t>::max(),
          "entity and cast IDs fail before wrap or reuse");
    next = std::numeric_limits<std::uint64_t>::max() - 1;
    Check(PendingSimulationSequence(next) == std::numeric_limits<std::uint64_t>::max() &&
              NextSimulationSequence(next) == std::numeric_limits<std::uint64_t>::max(),
          "previewed signal sequence matches the actual next event");
    exhausted = false;
    try { (void)NextSimulationSequence(next); }
    catch (const std::overflow_error &) { exhausted = true; }
    Check(exhausted && next == std::numeric_limits<std::uint64_t>::max(),
          "session, tick and signal sequences fail before arithmetic wraps");
}

void TestPickupWorkMembership()
{
    using namespace hs;
    using namespace hs::gameplay_detail;
    PickupWorkOwner work;
    std::vector<PickupActor> pickups(4);
    for (std::size_t row = 0; row < pickups.size(); ++row) pickups[row].id = {row + 1};
    pickups[0].position = {-70.0f, -70.0f};
    pickups[1].kind = PickupKind::Heal;
    pickups[1].position = {500.0f, 500.0f};
    pickups[2].position = {2.0f, 0.0f};
    pickups[3].position = {400.0f, 400.0f};
    pickups[3].globally_attracted = true;
    for (const auto &pickup : pickups) work.OnSpawn(pickup, 60.0f);
    const auto expect = [&](std::initializer_list<std::uint64_t> ids) {
        Check(std::ranges::equal(work.Queued(), ids), "pickup work has canonical unique logical IDs");
    };
    work.Begin(pickups, {}, 3.0f, 0.5f, false, 60.0f);
    expect({2, 3, 4});
    pickups[2].position = {30.0f, 0.0f};
    work.Update(pickups[2], true);
    work.Begin(pickups, {}, 3.0f, 0.5f, false, 60.0f);
    expect({2, 3, 4});
    work.Update(pickups[2], false);
    work.Begin(pickups, {}, 3.0f, 0.5f, false, 60.0f);
    expect({2, 4});
    pickups[1].dead = true;
    work.OnCollected(2);
    Check(work.NeedsCompaction(), "collected pickup requests the original stable compaction");
    std::erase_if(pickups, [](const auto &pickup) { return pickup.dead; });
    work.AfterCompaction();
    work.Begin(pickups, {}, 3.0f, 0.5f, true, 60.0f);
    expect({1, 3, 4});
    work.Invalidate();
    work.Begin(pickups, {}, 3.0f, 0.5f, false, 80.0f);
    expect({4});
    work.Reset();
    work.Begin({}, {}, 3.0f, 0.5f, false, 60.0f);
    expect({});

    // The index is only a broad phase; every exact attraction/collection candidate must survive it.
    pickups.clear();
    for (std::size_t row = 0; row < 500; ++row)
    {
        PickupActor pickup;
        pickup.id = {row + 1};
        pickup.position = {static_cast<float>((row * 37) % 401) - 200.0f,
                           static_cast<float>((row * 83) % 401) - 200.0f};
        pickups.push_back(pickup);
        work.OnSpawn(pickup, 60.0f);
    }
    for (const auto player : {Float2{}, Float2{-180.0f, 180.0f}, Float2{200.0f, -200.0f}})
        for (const auto radius : {0.5f, 3.0f, 20.0f, 500.0f})
        {
            work.Begin(pickups, player, radius, 0.5f, false, 60.0f);
            Check(std::ranges::adjacent_find(work.Queued()) == work.Queued().end(),
                  "overlapping work sources cannot duplicate a pickup");
            for (const auto &pickup : pickups)
                if (DistanceSquared(player, pickup.position) <= radius * radius)
                    Check(std::ranges::binary_search(work.Queued(), pickup.id.value),
                          "pickup bins conservatively retain exact hits including off-grid positions");
        }
}

using PickupWorld = hs::GameplayTestAccess::World;

void InitializePickupFixture(PickupWorld &world, bool auto_collect = false)
{
    world.config.seed = 827;
    world.config.scenario.progression_enabled = false;
    world.config.scenario.automatic_spawning = false;
    world.config.scenario.auto_collect_progression = auto_collect;
    world.rules = hs::SimulationRules::Defaults();
    world.rules.stats.base_magnet_radius_m = 3.0f;
    world.rules.growth.experience_pickup_speed = 15.0f;
    world.rules.growth.experience_pickup_radius = 0.5f;
    world.InitializePlayerState();
}

void TestHomingProjectileUsesRetainedSpeed()
{
    using namespace hs;
    using namespace hs::gameplay_detail;
    PickupWorld world;
    InitializePickupFixture(world);
    world.StartSession();

    PreparedEnemy target;
    target.id = {77};
    target.motion.position = {4.0f, 0.0f};
    target.life.health = 100;
    target.life.max_health = 100;
    world.actors->enemies.push_back(std::move(target));

    PreparedProjectile homing;
    homing.id = {88};
    homing.motion.speed = 6.0f;
    homing.motion.velocity = {5.99f, 0.0f}; // Rounded instantaneous velocity differs from assigned speed.
    homing.motion.remaining_range = 100.0f;
    homing.combat.homing = true;
    homing.combat.homing_target = 77;
    world.actors->projectiles.push_back(std::move(homing));
    world.next_entity_id = 89;

    const auto assigned_speed_checksum = world.CalculateChecksum();
    world.actors->projectiles.motion[0].speed = 7.0f;
    const auto changed_speed_checksum = world.CalculateChecksum();
    Check(assigned_speed_checksum != changed_speed_checksum,
          "gameplay checksum includes retained projectile speed when all other state is unchanged");

    world.actors->projectiles.motion[0].speed = 6.0f;
    world.tick = 1;
    world.MovementPhase();
    {
        const auto projectile = std::as_const(world.actors->projectiles)[0];
        Check(projectile.motion.velocity.x == 6.0f && projectile.motion.velocity.y == 0.0f &&
              projectile.motion.position.x == 6.0f * kTickSeconds,
              "homing movement uses assigned speed instead of the rounded prior velocity magnitude");
    }

    world.rules.skills[static_cast<std::size_t>(SkillKind::BasicAttack)].projectile_speed = 8.0f;
    world.rules.skills[static_cast<std::size_t>(SkillKind::RicochetArrow)].projectile_speed = 0.0f;
    world.actors->player.upgrades[static_cast<std::size_t>(SkillKind::RicochetArrow)] = 1;
    {
        auto ricochet = world.FireProjectile(SkillKind::RicochetArrow, {1.0f, 0.0f}, {1.0f, 0.0f},
                                         1.0f, EffectOrigin::Original, 1, 1, true);
        Check(ricochet && ricochet->motion.speed == 8.0f,
              "Ricochet spawn retains the BasicAttack fallback when authored speed is nonpositive");
        ricochet->motion.remaining_range = 0.0f;
    }
    world.pipeline_phase = SimulationPhaseId::CollisionHit;
    world.CollisionHitPhase();
    const auto returned = std::as_const(world.actors->projectiles).back();
    Check(returned.combat.returning && returned.motion.speed == 8.0f &&
              returned.motion.velocity.x == -8.0f && returned.motion.velocity.y == 0.0f,
          "Ricochet return redirect keeps its effective spawn speed when authored speed is nonpositive");
}

// Pinned pre-N16 traversal is the independent semantic oracle, never a production fallback.
void ReferencePickupPass(PickupWorld &world)
{
    using namespace hs;
    using namespace hs::gameplay_detail;
    Check(!world.config.scenario.progression_enabled, "pickup oracle excludes separate level-card kernel");
    if (world.session_phase != SessionPhase::Playing) return;
    for (auto &pickup : world.actors->pickups)
    {
        if (pickup.dead) continue;
        if (world.config.scenario.auto_collect_progression &&
            (pickup.kind == PickupKind::Experience || pickup.kind == PickupKind::RelicChest))
            pickup.position = world.actors->player.position;
        const auto delta = Subtract(world.actors->player.position, pickup.position);
        const auto global = pickup.kind == PickupKind::Experience && pickup.globally_attracted;
        const auto radius = global ? world.rules.arena_half_extent * 3.0f : world.EffectiveMagnetRadius();
        if (LengthSquared(delta) <= radius * radius)
        {
            if (!global && LengthSquared(delta) > world.rules.stats.base_magnet_radius_m *
                                                  world.rules.stats.base_magnet_radius_m)
                pickup.attracted_by_magnet_stat = true;
            if (pickup.kind == PickupKind::Experience && LengthSquared(delta) > 0.0f &&
                world.rules.growth.experience_pickup_speed > 0.0f)
            {
                if (pickup.visual_motion_started == 0 || pickup.visual_motion_updated + 1 != world.tick)
                {
                    pickup.visual_motion_started = world.tick;
                    pickup.visual_motion_initial_distance = Length(delta);
                }
                pickup.visual_motion_updated = world.tick;
            }
            pickup.position = Add(pickup.position, Multiply(Normalize(delta),
                world.rules.growth.experience_pickup_speed * kTickSeconds));
        }
        if (DistanceSquared(pickup.position, world.actors->player.position) <=
            world.rules.growth.experience_pickup_radius * world.rules.growth.experience_pickup_radius)
        {
            world.CollectPickup(pickup);
            if (world.session_phase != SessionPhase::Playing) break;
        }
    }
}

void CheckPickupEquivalence(const PickupWorld &actual, const PickupWorld &reference)
{
    Check(actual.CalculateChecksum() == reference.CalculateChecksum(),
          "pickup work preserves exact authoritative state, counters, RNG and canonical hash");
    Check(actual.actors->pickups.size() == reference.actors->pickups.size(), "pickup storage size equivalence");
    for (std::size_t row = 0; row < actual.actors->pickups.size(); ++row)
    {
        const auto &left = actual.actors->pickups[row];
        const auto &right = reference.actors->pickups[row];
        Check(left.id.value == right.id.value && left.visual_motion_started == right.visual_motion_started &&
                  left.visual_motion_updated == right.visual_motion_updated &&
                  std::bit_cast<std::uint32_t>(left.visual_motion_initial_distance) ==
                      std::bit_cast<std::uint32_t>(right.visual_motion_initial_distance),
              "pickup motion episode clocks and exact initial distance stay unchanged");
    }
    const auto &left = actual.combat_state->domain_signals;
    const auto &right = reference.combat_state->domain_signals;
    Check(left.size() == right.size(), "pickup signal count equivalence");
    for (std::size_t row = 0; row < left.size(); ++row)
        Check(left[row].sequence == right[row].sequence && left[row].kind == right[row].kind &&
                  left[row].tick == right[row].tick && left[row].session_id == right[row].session_id &&
                  left[row].position.x == right[row].position.x && left[row].position.z == right[row].position.z &&
                  left[row].target.x == right[row].target.x && left[row].target.z == right[row].target.z &&
                  left[row].flags == right[row].flags,
              "pickup callbacks retain original event order and targets");
}

void TestPickupWorkAgainstOriginalTraversal()
{
    using namespace hs;
    using namespace hs::gameplay_detail;
    PickupWorld actual, reference;
    InitializePickupFixture(actual);
    InitializePickupFixture(reference);
    for (auto *world : {&actual, &reference})
    {
        world->SpawnPickup(PickupKind::Experience, {10.0f, 0.0f}, 1);
        world->SpawnPickup(PickupKind::Magnet, {}, 1);
        world->SpawnPickup(PickupKind::Experience, {-10.0f, 0.0f}, 1);
        world->SpawnPickup(PickupKind::Experience, {2.0f, 0.0f}, 1);
        world->SpawnPickup(PickupKind::Experience, {1000.0f, 0.0f}, 1);
        world->tick = 1;
    }
    actual.XpCardPhase();
    ReferencePickupPass(reference);
    CheckPickupEquivalence(actual, reference);
    Check(actual.actors->pickups[0].position.x == 10.0f &&
              actual.actors->pickups[2].position.x == -9.75f &&
              actual.actors->pickups[3].position.x == 1.75f,
          "XP_A waits until next tick while XP_B and already queued XP move exactly once after Magnet");
    actual.CommitCleanupBarrier();
    reference.CommitCleanupBarrier();
    CheckPickupEquivalence(actual, reference);
    actual.tick = reference.tick = 2;
    actual.XpCardPhase();
    ReferencePickupPass(reference);
    CheckPickupEquivalence(actual, reference);
    Check(actual.actors->pickups[0].position.x == 9.75f,
          "earlier global XP participates after stable compaction on the next tick");
    for (auto *world : {&actual, &reference})
    {
        world->StartSession();
        world->SpawnPickup(PickupKind::RelicChest, {}, 0);
        world->SpawnPickup(PickupKind::Experience, {2.0f, 0.0f}, 7);
        world->tick = 1;
    }
    actual.XpCardPhase();
    ReferencePickupPass(reference);
    CheckPickupEquivalence(actual, reference);
    Check(actual.session_phase == SessionPhase::RelicSelection &&
              actual.actors->pickups[1].position.x == 2.0f,
          "chest phase transition stops later XP and reset discards old work IDs");

    PickupWorld automatic, automatic_reference;
    InitializePickupFixture(automatic, true);
    InitializePickupFixture(automatic_reference, true);
    for (auto *world : {&automatic, &automatic_reference})
    {
        world->SpawnPickup(PickupKind::Experience, {5000.0f, -5000.0f}, 7);
        world->SpawnPickup(PickupKind::RelicChest, {-5000.0f, 5000.0f}, 0);
        world->tick = 1;
    }
    automatic.XpCardPhase();
    ReferencePickupPass(automatic_reference);
    CheckPickupEquivalence(automatic, automatic_reference);
    Check(automatic.actors->player.experience == 7 && automatic.session_phase == SessionPhase::RelicSelection,
          "development auto collect includes off-grid XP and chests in canonical order");
}

void TestPickupWorkStopRestartAndSparseTick()
{
    using namespace hs;
    using namespace hs::gameplay_detail;
    PickupWorld actual, reference;
    InitializePickupFixture(actual);
    InitializePickupFixture(reference);
    for (auto *world : {&actual, &reference})
        world->SpawnPickup(PickupKind::Experience, {2.5f, 0.0f}, 1);
    for (Tick tick = 1; tick <= 4; ++tick)
    {
        actual.tick = reference.tick = tick;
        actual.actors->player.position = reference.actors->player.position =
            tick == 2 || tick == 3 ? Float2{20.0f, 0.0f} : Float2{};
        actual.XpCardPhase();
        ReferencePickupPass(reference);
        CheckPickupEquivalence(actual, reference);
        if (tick == 3)
            Check(actual.progression->pickup_work.Visited().empty(), "stopped distant XP leaves the active work set");
    }
    Check(actual.actors->pickups[0].visual_motion_started == 4,
          "local attraction interruption restarts its original cosmetic motion episode");

    for (auto *world : {&actual, &reference})
    {
        world->StartSession();
        const auto stat = static_cast<std::size_t>(StatKind::MagnetRadius);
        world->rules.stats.allocations[stat].amount_per_point = 1.0f;
        world->actors->player.stats[stat] = 2;
        world->SpawnPickup(PickupKind::Experience, {4.0f, 0.0f}, 1);
        world->tick = 1;
    }
    actual.XpCardPhase();
    ReferencePickupPass(reference);
    CheckPickupEquivalence(actual, reference);
    Check(actual.actors->pickups[0].attracted_by_magnet_stat,
          "stat-only attraction retains its original attribution flag");

    for (auto *world : {&actual, &reference})
    {
        world->StartSession();
        world->rules.stats.base_magnet_radius_m = 0.25f;
        world->rules.growth.experience_pickup_radius = 2.0f;
        world->SpawnPickup(PickupKind::Experience, {1.5f, 0.0f}, 3);
        world->tick = 1;
    }
    actual.XpCardPhase();
    ReferencePickupPass(reference);
    CheckPickupEquivalence(actual, reference);
    Check(actual.actors->player.experience == 3, "collection radius may exceed attraction radius");
    for (auto *world : {&actual, &reference})
    {
        world->StartSession();
        for (std::size_t count = 0; count < 1024; ++count)
            world->SpawnPickup(PickupKind::Experience, {40.0f + static_cast<float>(count % 20), 30.0f}, 1);
        world->tick = 1;
    }
    actual.XpCardPhase();
    ReferencePickupPass(reference);
    CheckPickupEquivalence(actual, reference);
    Check(actual.progression->pickup_work.Visited().empty() &&
              !actual.progression->pickup_work.NeedsCompaction(),
          "ordinary distant stationary XP causes no per-pickup tick work or cleanup scan");
}

void TestPickupDistanceArithmeticEquivalence()
{
    using namespace hs;
    for (const auto speed : {0.0f, 15.0f, 120.0f})
    {
        PickupWorld actual, reference;
        InitializePickupFixture(actual);
        InitializePickupFixture(reference);
        for (auto *world : {&actual, &reference})
        {
            world->rules.growth.experience_pickup_speed = speed;
            for (const auto position : {Float2{}, Float2{0.0000001f, 0.0f}, Float2{0.2f, 0.2f},
                                        Float2{1.25f, -1.75f}, Float2{3.0f, 0.0f}, Float2{3.000001f, 0.0f}})
                world->SpawnPickup(PickupKind::Experience, position, 1);
            world->tick = 1;
        }
        actual.XpCardPhase();
        ReferencePickupPass(reference);
        CheckPickupEquivalence(actual, reference);
        if (speed == 120.0f)
            Check(!actual.actors->pickups[2].dead,
                  "post-movement collection distance preserves an overshoot outside collection radius");
    }
}

void TestBalanceHitDeduplication()
{
    hs::gameplay_detail::BalanceObserver observer;
    const std::vector<std::uint64_t> hits{50, 4, 50, 19, 4, 0, 19, 100, 0};
    std::vector<std::uint64_t> reference;
    for (const auto cast : hits)
    {
        const bool first = std::ranges::find(reference, cast) == reference.end();
        if (first) reference.push_back(cast);
        Check(observer.RecordHit(observer.player_hit_casts, cast) == first,
              "delayed and repeated hits preserve first-hit attribution");
        for (std::size_t index = 0; index < observer.enemy_hit_casts.size(); ++index)
            Check(observer.RecordEnemyHit(index, cast) == first,
                  "enemy archetypes independently deduplicate the same cast");
    }
    Check(observer.player_hit_casts.size() == reference.size(),
          "only unique cast ids occupy telemetry storage");
    observer.metrics.enemy_hits[0] = 7;
    observer.Reset();
    Check(observer.metrics.enemy_hits[0] == 0 &&
              observer.RecordHit(observer.player_hit_casts, 50),
          "run reset clears metrics and player hit history");
    for (auto &enemy : observer.enemy_hit_casts)
        Check(observer.RecordHit(enemy, 50), "run reset clears every enemy history");

    observer.player_hit_casts = {0, 4, 9};
    observer.enemy_hit_casts[0] = {4, 9};
    observer.enemy_seen_zero[0] = true;
    observer.active_player_casts = {9, 9};
    observer.active_enemy_casts[0] = {9, 9};
    observer.RetireInactiveCauses();
    Check(observer.player_hit_casts == std::vector<std::uint64_t>{9} &&
              observer.enemy_hit_casts[0] == std::vector<std::uint64_t>{9},
          "retirement preserves only active future causes and deduplicates membership");
    Check(!observer.RecordEnemyHit(0, 0),
          "enemy cast zero remains a fixed per-archetype seen key after retirement");
    observer.Reset(false);
    Check(!observer.metrics.detailed_balance_captured &&
              observer.metrics.hit_rates_captured &&
              observer.player_hit_casts.empty() && !observer.enemy_seen_zero[0],
          "session reset marks disabled detail and clears all retained causes");

    PickupWorld world;
    auto &retained = world.telemetry->balance_observer;
    retained.Reset(true);
    retained.player_hit_casts = {123};
    world.actors->player.retreat_followup_cast = 123;
    world.actors->player.retreat_landing_pending = true;
    world.RetireBalanceHitCauses();
    Check(retained.player_hit_casts == std::vector<std::uint64_t>{123},
          "landing still owns the original hit-cast membership");
    world.actors->player.retreat_landing_pending = false;
    world.actors->player.retreat_followup_tick = 20;
    world.RetireBalanceHitCauses();
    Check(retained.player_hit_casts == std::vector<std::uint64_t>{123},
          "delayed second retreat still owns the original hit-cast membership");
    world.actors->player.retreat_followup_tick = 0;
    world.RetireBalanceHitCauses();
    Check(retained.player_hit_casts.empty(),
          "retreat membership retires after both future causes resolve");
    InitializePickupFixture(world);
    world.config.capture_detailed_balance = true;
    world.config.capture_hit_rates = false;
    world.StartSession();
    Check(retained.metrics.detailed_balance_captured && !retained.metrics.hit_rates_captured &&
              retained.player_hit_casts.empty() &&
              std::ranges::all_of(retained.enemy_hit_casts, [](const auto &casts) { return casts.empty(); }),
          "session restart preserves independent hit-rate policy and clears retained membership");
}

void TestBleedActivePrefix()
{
    using namespace hs::gameplay_detail;
    for (std::size_t cap = 0; cap <= 5; ++cap)
    {
        StatusState status;
        for (std::size_t index = 0; index < cap; ++index)
            status.AddBleed({1.0f, 10, 1}, cap);
        status.AddBleed({2.0f, 20, 2}, cap);
        Check(status.bleed_count == cap, "bleed refresh preserves bounded active count");
        if (cap != 0)
            Check(status.bleeds[0].attack_snapshot == 2.0f,
                  "equal-expiry refresh replaces first active slot");
        for (std::size_t index = cap; index < status.bleeds.size(); ++index)
            Check(status.bleeds[index].expires == 0,
                  "refresh never writes inactive bleed slots");
        if (cap > 1)
        {
            status.AddBleed({3.0f, 30, 3}, cap);
            Check(status.bleeds[1].attack_snapshot == 3.0f,
                  "refresh next replaces earliest remaining active expiry");
        }
    }
}

void TestSpatialGridAgainstBruteForce()
{
    using namespace hs;
    using namespace hs::gameplay_detail;
    constexpr float arena_half_extent = 60.0f;
    constexpr float collision_radius = 1.35f;
    const auto segment_circle = [](Float2 from, Float2 to, Float2 center,
                                   float radius) {
        const Float2 segment{to.x - from.x, to.y - from.y};
        const Float2 offset{center.x - from.x, center.y - from.y};
        const auto length_squared = segment.x * segment.x + segment.y * segment.y;
        const auto t = length_squared > 0.0f
                           ? std::clamp((offset.x * segment.x + offset.y * segment.y) /
                                            length_squared,
                                        0.0f, 1.0f)
                           : 0.0f;
        const Float2 closest{from.x + segment.x * t, from.y + segment.y * t};
        const auto x = center.x - closest.x;
        const auto y = center.y - closest.y;
        return x * x + y * y <= radius * radius;
    };
    EnemySpatialGrid grid;
    std::vector<Float2> positions;
    std::uint64_t random = 0xC001D00Du;
    const auto next = [&]() {
        random = random * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<float>((random >> 40) & 0xFFFFFFu) /
               static_cast<float>(0xFFFFFFu);
    };
    for (std::size_t index = 0; index < 300; ++index)
    {
        const Float2 position{(next() * 2.0f - 1.0f) * arena_half_extent,
                              (next() * 2.0f - 1.0f) * arena_half_extent};
        positions.push_back(position);
        const auto x = SpatialGridCoordinate(position.x, arena_half_extent);
        const auto y = SpatialGridCoordinate(position.y, arena_half_extent);
        grid.AddRange(index, x, x, y, y);
    }
    grid.FinishBuild();

    EnemyQuery collision_query, range_query;
    for (std::uint32_t query = 0; query < 256; ++query)
    {
        const Float2 from{(next() * 2.0f - 1.0f) * arena_half_extent,
                          (next() * 2.0f - 1.0f) * arena_half_extent};
        const Float2 to{(next() * 2.0f - 1.0f) * arena_half_extent,
                        (next() * 2.0f - 1.0f) * arena_half_extent};
        collision_query.Collect(grid,
            {std::min(from.x, to.x) - collision_radius,
             std::min(from.y, to.y) - collision_radius},
            {std::max(from.x, to.x) + collision_radius,
             std::max(from.y, to.y) + collision_radius},
            arena_half_extent, positions.size());
        std::vector<std::size_t> grid_hits;
        for (const auto index : collision_query.rows)
            if (segment_circle(from, to, positions[index], collision_radius))
                grid_hits.push_back(index);
        std::vector<std::size_t> brute_hits;
        for (std::size_t index = 0; index < positions.size(); ++index)
            if (segment_circle(from, to, positions[index], collision_radius))
                brute_hits.push_back(index);
        std::ranges::sort(grid_hits);
        std::ranges::sort(brute_hits);
        Check(grid_hits == brute_hits,
              "spatial grid collision query matches brute force");
        const auto search_radius = collision_radius * (1 + query % 20);
        range_query.Collect(grid, {from.x - search_radius, from.y - search_radius},
                            {from.x + search_radius, from.y + search_radius},
                            arena_half_extent, positions.size());
        std::vector<std::size_t> expected, actual;
        const auto eligible = [&](std::size_t row) {
            const auto dx = positions[row].x - from.x, dy = positions[row].y - from.y;
            // Simulate live/dead and per-chain exclusions at consumption time.
            return row % 3 != query % 3 && row != query % positions.size() &&
                   dx * dx + dy * dy <= search_radius * search_radius;
        };
        for (std::size_t row = 0; row < positions.size(); ++row)
            if (eligible(row)) expected.push_back(row);
        for (const auto row : range_query.rows)
            if (eligible(row)) actual.push_back(row);
        Check(actual == expected, "radius query preserves brute-force eligible actor order");
    }
}

void TestSweptSpatialGridOrder()
{
    using namespace hs;
    using namespace hs::gameplay_detail;
    EnemySpatialGrid grid;
    std::array<std::vector<std::size_t>, kGridCellCount> reference;
    EnemyQuery unique, inner;
    for (std::size_t build = 0; build < 3; ++build)
    {
        grid.BeginBuild();
        for (auto &cell : reference) cell.clear();
        const auto count = build == 1 ? 0u : 200u;
        for (std::size_t actor = 0; actor < count; ++actor)
        {
            const auto x0 = static_cast<int>(actor % kGridDimension);
            const auto y0 = static_cast<int>((actor * 7) % kGridDimension);
            const auto x1 = std::min(x0 + 4, kGridDimension - 1);
            const auto y1 = std::min(y0 + 2, kGridDimension - 1);
            grid.AddRange(actor, x0, x1, y0, y1);
            for (auto y = y0; y <= y1; ++y)
                for (auto x = x0; x <= x1; ++x)
                    reference[y * kGridDimension + x].push_back(actor);
        }
        grid.FinishBuild();
        for (std::size_t cell = 0; cell < reference.size(); ++cell)
            Check(std::ranges::equal(grid[cell], reference[cell]),
                  "flat swept bins preserve each cell's insertion order and empty rebuilds");
        unique.Collect(grid, {-60, -60}, {60, 60}, 60, count);
        Check(unique.rows.size() == count && std::ranges::is_sorted(unique.rows),
              "swept-cell visits produce one candidate per actor in original order");
        const auto unique_before = unique.rows;
        inner.Collect(grid, {0, 0}, {8, 8}, 60, count);
        Check(unique.rows == unique_before, "nested unique queries retain outer candidates");
        unique.Collect(grid, {0, 0}, {8, 8}, 60, count);
        Check(unique.rows == inner.rows, "retained visits reset between queries");
    }

    grid.MarkBuilt(17, 23);
    Check(grid.IsCurrent(17, 23) && !grid.IsCurrent(18, 23) &&
              !grid.IsCurrent(17, 24),
          "swept grid freshness requires both matching tick and actor generation");

    EnemySpatialGrid unordered_grid;
    unordered_grid.BeginBuild();
    unordered_grid.AddRange(0, 20, 20, 20, 20);
    unordered_grid.AddRange(1, 2, 2, 2, 2);
    unordered_grid.FinishBuild();
    EnemyQuery fast, canonical;
    fast.Collect(unordered_grid, {-60, -60}, {60, 60}, 60, 2, false);
    canonical.Collect(unordered_grid, {-60, -60}, {60, 60}, 60, 2, true);
    Check(fast.rows == std::vector<std::size_t>{1, 0} &&
              canonical.rows == std::vector<std::size_t>{0, 1},
          "projectile broad phase can preserve visit order while canonical queries sort rows");
}

void TestSlowMultiplierIsReadOnly()
{
    using namespace hs;
    using namespace hs::gameplay_detail;
    PickupWorld world;
    InitializePickupFixture(world);
    Check(world.SpawnEnemy(EnemyKind::Melee, {2.0f, 0.0f}),
          "slow read fixture enemy spawn");
    world.actors->enemies.AppendFrom(world.actors->pending_enemy_spawns);
    world.tick = 100;
    auto enemy = world.actors->enemies[0];
    enemy.status.slows = {{0.5f, 101}, {0.75f, 99}, {0.25f, 102}};
    const auto before = enemy.status.slows;
    Check(world.SlowMultiplier(enemy) == 0.375f &&
              enemy.status.slows.size() == before.size() &&
              std::ranges::equal(enemy.status.slows, before, [](const auto &a, const auto &b) {
                  return a.reduction == b.reduction && a.expires == b.expires &&
                      a.source_skill == b.source_skill && a.source_upgrade == b.source_upgrade &&
                      a.source_relic == b.source_relic;
              }),
          "slow read ignores expired effects without erasing or reordering stored state");
}

void TestCastCleanupMembership()
{
    using namespace hs::gameplay_detail;
    std::vector<CastRuntime> runtimes;
    for (std::uint64_t id = 1; id < 600; ++id)
    {
        CastRuntime runtime;
        runtime.cast_id = id;
        runtime.cooldown_ticks_saved = id % 3;
        runtimes.push_back(runtime);
    }
    CastCleanupMembership membership;
    for (const std::uint64_t next : {256, 600, 900})
    {
        membership.Prepare(runtimes, next);
        std::vector<std::uint64_t> live_projectiles;
        for (std::uint64_t id = 1; id < 600; id += 7) live_projectiles.push_back(id);
        live_projectiles.push_back(2); // A late child joined at the cleanup barrier.
        live_projectiles.push_back(2);
        for (const auto id : live_projectiles) membership.Observe(id);
        for (const auto &runtime : runtimes)
        {
            const auto old_remove = runtime.cast_id + 256 < next &&
                (runtime.cooldown_ticks_saved == 0 ||
                 std::ranges::find(live_projectiles, runtime.cast_id) == live_projectiles.end());
            const auto new_remove = IsOldCast(runtime.cast_id, next) &&
                (runtime.cooldown_ticks_saved == 0 || !membership.Contains(runtime.cast_id));
            Check(old_remove == new_remove, "cast cleanup matches repeated projectile scan exactly");
        }
    }
    membership.Prepare({}, 900);
    Check(membership.entries.empty(), "cleanup workspace drops stale membership");
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    Check(!IsOldCast(0, 256) && IsOldCast(0, 257) && !IsOldCast(600, 600) && !IsOldCast(601, 600),
          "cast cleanup preserves exact age256/257, equal and future ID boundaries");
    Check(IsOldCast(maximum - 257, maximum) && !IsOldCast(maximum - 256, maximum) &&
          !IsOldCast(maximum - 1, 600) && !IsOldCast(maximum, 0),
          "cast cleanup must not age recent or future IDs through unsigned addition overflow");
    runtimes.clear();
    for (const auto id : {maximum - 257, maximum - 256, maximum - 1, maximum})
    {
        CastRuntime runtime;
        runtime.cast_id = id;
        runtime.cooldown_ticks_saved = 1;
        runtimes.push_back(runtime);
    }
    membership.Prepare(runtimes, maximum);
    for (const auto &runtime : runtimes) membership.Observe(runtime.cast_id);
    Check(membership.entries.size() == 1 && membership.Contains(maximum - 257) &&
          !membership.Contains(maximum - 256) && !membership.Contains(maximum - 1) && !membership.Contains(maximum),
          "cleanup membership excludes all recent IDs near the identity limit");
}

void TestHitIndexCleanup()
{
    using Key = std::pair<std::uint64_t, std::uint64_t>;
    hs::CombatState::HitIndex index;
    std::vector<std::pair<Key, std::uint8_t>> records;
    std::map<Key, std::uint8_t> reference;
    const auto hit = [&](Key key) {
        const auto [found, inserted] = index.try_emplace(key, records.size());
        if (inserted) records.push_back({key, 0});
        auto &count = records[found->second].second;
        ++count;
        Check(count == ++reference[key], "duplicate hit counts match ordered reference including wrap");
    };
    for (std::uint64_t id = 0; id < 1024; ++id)
    {
        hit({id, id});
        hit({id, id << 32});
        hit({id << 32, id});
        hit({~id, id});
    }
    for (unsigned repeat = 0; repeat < 320; ++repeat) hit({0, 0});
    const auto capacity = index.bucket_count();
    std::erase_if(records, [](const auto &record) { return record.first.first < 512; });
    std::erase_if(reference, [](const auto &record) { return record.first.first < 512; });
    index.clear();
    for (std::size_t row = 0; row < records.size(); ++row) index.emplace(records[row].first, row);
    Check(index.bucket_count() == capacity, "cleanup retains hash buckets");
    for (const auto &[key, count] : reference)
    {
        const auto found = index.find(key);
        Check(found != index.end() && records[found->second] == std::pair{key, count},
              "cleanup reindexes compacted authoritative rows");
    }
    hit({0, 0});
    Check(records.back().first == Key{0, 0} && records.back().second == 1,
          "expired hit key is appended with a fresh count");
    index.clear(); records.clear(); reference.clear();
    hit({~std::uint64_t{0}, ~std::uint64_t{0}});
    Check(records.size() == 1, "empty rebuild accepts full-width identifiers");
}

}

int main()
{
#ifdef _MSC_VER
    _set_error_mode(_OUT_TO_STDERR);
#endif
    using namespace hs;
    using namespace hs::gameplay_detail;
    try
    {
        TestActorColumnStorage();
        TestSmallFactsIgnoreNoOpMutation();
        TestSimulationIdentityExhaustion();
        TestHomingProjectileUsesRetainedSpeed();
        TestPickupWorkMembership();
        TestPickupWorkAgainstOriginalTraversal();
        TestPickupWorkStopRestartAndSparseTick();
        TestPickupDistanceArithmeticEquivalence();
        TestBleedActivePrefix();
        constexpr auto &pipeline = kSimulationPipeline;
        Check(pipeline[2] == SimulationPhaseId::SpawnBarrier &&
                  pipeline[5] == SimulationPhaseId::AbilitySpawnBarrier &&
                  pipeline[pipeline.size() - 2] == SimulationPhaseId::CleanupBarrier &&
                  pipeline.back() == SimulationPhaseId::GameplayHash,
              "phase-specific visibility barriers are explicit and ordered");
        TestSpatialGridAgainstBruteForce();
        TestSweptSpatialGridOrder();
        TestSlowMultiplierIsReadOnly();
        TestCastCleanupMembership();
        TestHitIndexCleanup();
        TestBalanceHitDeduplication();
        std::cout << "gameplay_internal_tests passed\n";
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << "gameplay_internal_tests failed: " << exception.what() << '\n';
        return 1;
    }
}
