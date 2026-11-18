#include "gameplay_test_support.hpp"
#include <hs/presentation/process_scene.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <unordered_set>

namespace gameplay_test
{

void TestStaticEnvironmentSceneLease()
{
    hs::GameReadModelStorage model;
    model.seed = 19;
    model.session_id = 3;
    model.arena_half_extent = 16.0f;
    model.arena_boundary.count = 4;
    model.arena_boundary.points[0] = {-16, -16};
    model.arena_boundary.points[1] = {-16, 16};
    model.arena_boundary.points[2] = {16, 16};
    model.arena_boundary.points[3] = {16, -16};
    hs::RenderSnapshotStorage snapshot(4, 4, 2, 128);
    const auto model_view = model.View();
    const auto read = hs::development::BorrowOwnedReadModel(model_view);
    const auto first = hs::ProcessSceneView(
        hs::BuildProcessSceneAtLaunch(read, "test-generation"));
    Check(hs::ProjectRenderSnapshot(model_view, DefaultPresentation(),
                                    {}, hs::SettingsData{}, snapshot, 0xFF,
                                    nullptr, first), "static scene projection");
    const auto projected_first = snapshot.View().static_scene;
    Check(projected_first && projected_first == first && first->content_generation_id == "test-generation" &&
              first->instances.size() > 1 && snapshot.View().instances.size() == 1 &&
              snapshot.View().poses.size() == 1 &&
              snapshot.View().poses[0].instance_index == 0,
          "static scene lease preserves dynamic pose indices and content identity");
    std::unordered_set<std::uint64_t> identities;
    for (const auto &instance : first->instances)
        Check(instance.stable_id && identities.insert(instance.stable_id).second,
              "static scene instance identities must be unique");
    auto cook_model = model;
    cook_model.seed = 0;
    const auto cook_view = cook_model.View();
    const auto cook_scene = hs::ProcessSceneView(
        hs::BuildProcessSceneAtLaunch(hs::development::BorrowOwnedReadModel(cook_view), "test-generation"));
    Check(cook_scene->eligible_layout_digest == first->eligible_layout_digest &&
          cook_scene->eligible_static_members.size() == first->eligible_static_members.size(),
          "seed-zero scene cooking differs from runtime detail/HLOD placement");
    snapshot.Clear();
    Check(!snapshot.View().static_scene && first->instances.size() > 1,
          "clearing a slot releases only its own scene lease");
    const std::span<const hs::RenderInstance> retained_instances(first->instances);
    const std::span<const hs::StaticRenderScene::EligibleStaticMember> retained_members(first->eligible_static_members);
    const auto next_generation = hs::ProcessSceneView(
        hs::BuildProcessSceneAtLaunch(read, "next-generation"));
    Check(first->content_generation_id == "test-generation" &&
              next_generation->content_generation_id == "next-generation" &&
              retained_instances.data() == first->instances.data() &&
              retained_instances.size() == first->instances.size() &&
              retained_members.data() == first->eligible_static_members.data() &&
              retained_members.size() == first->eligible_static_members.size() &&
              retained_instances.front().stable_id == first->instances.front().stable_id,
          "a new content generation leaves the previous scene spans leased");
    Check(!next_generation->static_keys.empty() && next_generation->static_keys.size() == next_generation->instances.size() &&
              std::ranges::is_sorted(next_generation->static_keys, {}, &hs::RenderSceneInstanceKey::stable_id),
          "static keys are frozen in the process owner before publication");
    for (const auto &instance : next_generation->instances)
        Check(instance.stable_id && instance.revisions == hs::DeriveRenderInstanceRevisions(instance),
              "static revisions must reflect actual seeded rows");
}

void TestSessionIdentityAcrossPresentationReset()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({91, false, true}, QuietGameData()).Succeeded(),
          "session identity initialize");

    hs::GameReadModelStorage model;
    hs::RenderSnapshotStorage snapshot(8, 1, 1, 8);
    std::array<hs::PresentationEvent, 1> events{};

    Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::StartSession}).Succeeded(),
          "first session starts");
    hs::CopyReadModelForDevelopment(simulation, model);
    const auto first_id = model.View().session_id;
    Check(first_id != 0, "first session receives a nonzero identity");
    Check(hs::ProjectRenderSnapshot(model.View(), DefaultPresentation(),
                                    {}, hs::SettingsData{}, snapshot),
          "first session snapshot projection");
    Check(snapshot.View().header.session_id == first_id,
          "snapshot keeps first session identity");

    hs::DomainSignal signal{};
    signal.session_id = first_id;
    signal.kind = hs::DomainSignalKind::BasicAttackImpact;
    Check(hs::ProjectDomainSignal(signal, events) == 1 &&
              events[0].session_id == first_id,
          "presentation event keeps first session identity");

    Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::StartSession}).Succeeded(),
          "second session starts");
    hs::CopyReadModelForDevelopment(simulation, model);
    const auto second_id = model.View().session_id;
    Check(second_id > first_id, "session identity increases across reset");
    snapshot.Clear();
    Check(hs::ProjectRenderSnapshot(model.View(), DefaultPresentation(),
                                    {}, hs::SettingsData{}, snapshot),
          "second session snapshot projection");
    Check(snapshot.View().header.session_id == second_id,
          "snapshot keeps second session identity");
    signal.session_id = second_id;
    Check(hs::ProjectDomainSignal(signal, events) == 1 &&
              events[0].session_id == second_id,
          "presentation event keeps second session identity");
    Check(simulation.Shutdown().Succeeded(), "session identity shutdown");
}

void TestBorrowedProjectionOwnsPublication()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({319}, QuietGameData()).Succeeded(), "borrowed projection initialize");
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {6, 0});
    hs::HeldInputState held{}; held.basic_attack_held = true; held.aim_world = {20, 0, 0};
    for (int frame = 0; frame < 18; ++frame) (void)Tick(simulation, held);
    hs::RenderSnapshotStorage projected(64, 64, 8, 64);
    {
        auto read = simulation.BorrowTickReadView();
        Check(hs::ProjectTick(read.View(), DefaultPresentation(), {}, {}, projected,
                  hs::ProcessSceneView(hs::BuildProcessSceneAtLaunch(read.View(), {}))),
              "borrowed projection succeeds");
    }
    const auto a = projected.View();
    Check(!a.instances.empty(), "borrowed packet has live payload");
    const auto saved_tick = a.header.tick;
    const auto saved_id = a.instances.front().stable_id;
    const auto saved_revision = a.instances.front().revisions;
    Debug(simulation, hs::DebugCommandKind::StartSession);
    Check(projected.View().header.tick == saved_tick &&
              projected.View().instances.front().stable_id == saved_id &&
              projected.View().instances.front().revisions == saved_revision,
          "published payload remains owned after borrow ends and authoritative actors reset");
    Check(simulation.Shutdown().Succeeded(), "borrowed projection shutdown");
}

void RunGameplayPresentationTests()
{
    TestBorrowedProjectionOwnsPublication();
    TestStaticEnvironmentSceneLease();
    TestSessionIdentityAcrossPresentationReset();
}

} // namespace gameplay_test
