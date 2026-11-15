#include "gameplay_test_support.hpp"
#include <hs/presentation/process_scene.hpp>
#include <type_traits>
#include <hs/presentation/meadow_surface.hpp>
#include <hs/presentation/presentation_catalog_io.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string_view>
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
    Check(hs::ProjectRenderSnapshot(model_view, DefaultContent().presentation,
                                    test_ui, hs::SettingsData{}, snapshot, 0xFF,
                                    nullptr, first), "static scene projection");
    const auto projected_first = snapshot.View().static_scene;
    Check(projected_first && projected_first == first && first->content_generation_id == "test-generation" &&
              first->instances.size() > 1 && snapshot.View().instances.size() == 1 &&
              snapshot.View().poses.size() == 1 &&
              snapshot.View().poses[0].instance_index == 0,
          "static scene lease preserves dynamic pose indices and content identity");
    std::array<bool, hs::kGroundDetailVariantCount> detail_variants{};
    std::unordered_set<std::uint64_t> identities;
    std::size_t details{}, pebbles{}, short_tufts{}, clover_links{}, stacked_crates{}, roadside_details{}, packing_tufts{};
    for (const auto &instance : first->instances)
    {
        Check(instance.stable_id && identities.insert(instance.stable_id).second,
              "appended ground details collide with existing placement provenance");
        if (instance.mesh == hs::RenderMesh::GroundDetail)
        {
            ++details;
            if ((instance.stable_id & 0xFFF00000ull) == 0x400000ull) {
                ++clover_links;
                Check(instance.environment_variant == 3u, "colony links must retain low clover geometry");
            }
            const auto detail_key = instance.stable_id & 0xFFFFFFull;
            if ((detail_key & 0xFFF00000ull) == 0x500000ull || (detail_key & 0xFFF00000ull) == 0x600000ull) {
                ++roadside_details;
                if (detail_key == 0x500012ull) {
                    ++stacked_crates;
                    const auto base = std::ranges::find(first->instances,
                        (instance.stable_id & ~0xFFFFFFull) | 0x500010ull, &hs::RenderInstance::stable_id);
                    Check(base != first->instances.end() && base->environment_variant == 9u &&
                              instance.environment_variant == 9u && instance.position.x == base->position.x &&
                              instance.position.z == base->position.z && instance.yaw == base->yaw &&
                              std::abs(instance.position.y - base->position.y - .658665597f) < 1e-6f,
                          "stacked crate must rest on the actual authored base height with aligned supports");
                    const auto member = std::ranges::find(first->eligible_static_members,
                        instance.stable_id, &hs::StaticRenderScene::EligibleStaticMember::stable_id);
                    Check(member != first->eligible_static_members.end() && member->object_id == base->stable_id,
                          "a crate stack must remain one HLOD object");
                } else {
                    Check(std::abs(instance.position.y - hs::MeadowGroundHeight({instance.position.x, instance.position.z})) < 1e-6f,
                          "roadside debris and props must retain terrain contact");
                }
            }
            Check(instance.environment_variant < detail_variants.size() &&
                  instance.asset_id.value && instance.material_id.value == hs::MakeAssetId("render/material/ground_detail.rpkg").value,
                  "detail geometry is not bound to the actual shared material product");
            detail_variants[instance.environment_variant] = true;
            const auto variant = instance.environment_variant;
            constexpr std::array<float, hs::kGroundDetailVariantCount> minimum_scale{1.2f, 1.2f, 1.6f, .8f, .8f, 1.2f, 1.35f, 1,1,1,1,1,1,1,1,1,1,1,1};
            constexpr std::array<float, hs::kGroundDetailVariantCount> maximum_scale{1.725f, 1.725f, 2.3f, 1.15f, 1.1f, 1.65f, 1.575f, 1,1,1,1,1,1,1,1,1,1,1,1};
            Check(instance.scale.x >= minimum_scale[variant] - 1e-5f &&
                  instance.scale.x <= maximum_scale[variant] + 1e-5f,
                  "ground-detail species lost its player-relative placement scale");
            constexpr std::array<float, hs::kGroundDetailVariantCount> footprint{.58f,.58f,.58f,.58f,.58f,.58f,1.05f,1,1.3f,.75f,.75f,.65f,.65f,2,1.35f,1.4f,.5f,1.75f,1.05f};
            const auto radius = footprint[variant] * instance.scale.x;
            Check((variant == 13u || hs::MeadowRoadCoverage({instance.position.x, instance.position.z}, -radius) == 0.0f) &&
                  instance.scale.x == instance.scale.y && instance.scale.y == instance.scale.z,
                  "scaled static detail enters the clear path or loses uniform transforms");
        }
        if (instance.mesh == hs::RenderMesh::Grass && (instance.stable_id & 0xFFFFFCull) == 0x500200ull) {
            ++packing_tufts;
            const hs::Float2 point{instance.position.x, instance.position.z};
            Check(instance.scale.y == .07f && instance.scale.x == instance.scale.z && instance.scale.x == 1.05f &&
                      instance.color_rgba == 0xFF1865F0u && hs::MeadowWetness(point) <= .05f &&
                      !std::isfinite(hs::MeadowWaterLevel(point)) && hs::MeadowRoadCoverage(point, -.55f) == 0.0f,
                  "packing litter must remain low, dry and clear of the road");
        }
        if (instance.mesh == hs::RenderMesh::Rock && instance.scale.x <= .1f) ++pebbles;
        if (instance.mesh == hs::RenderMesh::Grass && instance.scale.y == .18f) ++short_tufts;
    }
    Check(details > 0 && details <= 320 && pebbles > 0 && pebbles <= 64 && short_tufts > 0 && short_tufts <= 64,
          "new detail, small-rock, or short-grass population escaped its explicit budget");
    Check(stacked_crates == 1 && packing_tufts == 3 && roadside_details >= 14 && roadside_details <= 18,
          "bounded roadside stories need their supply stack and broken-edge debris");
    Check(clover_links > 0 && clover_links <= 16,
          "large colonies need bounded clover links instead of a uniform flower border");
    for (std::size_t variant = 0; variant < detail_variants.size(); ++variant)
        Check(detail_variants[variant], std::format("normal play scene omits ground-detail variant {}", variant));
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
    static_assert(!std::is_move_constructible_v<hs::ProcessSceneLayout>);
    static_assert(!std::is_copy_constructible_v<hs::ProcessSceneLayout>);
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
    Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
                                    test_ui, hs::SettingsData{}, snapshot),
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
    Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
                                    test_ui, hs::SettingsData{}, snapshot),
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
    hs::GameReadModelStorage legacy;
    hs::CopyReadModelForDevelopment(simulation, legacy);
    hs::RenderSnapshotStorage projected(64, 64, 8, 64), fixture(64, 64, 8, 64);
    {
        auto read = simulation.BorrowTickReadView();
        Check(hs::ProjectTick(read.View(), DefaultContent().presentation, {}, {}, projected,
                  hs::ProcessSceneView(hs::BuildProcessSceneAtLaunch(read.View(), {}))) &&
                  hs::ProjectRenderSnapshot(legacy.View(), DefaultContent().presentation, {}, {}, fixture),
              "borrowed and explicit owned fixture projection succeed");
    }
    const auto a = projected.View(), b = fixture.View();
    Check(a.header.tick == b.header.tick && a.header.session_id == b.header.session_id &&
              a.instances.size() == b.instances.size() && a.poses.size() == b.poses.size() &&
              a.persistent_vfx.size() == b.persistent_vfx.size() && a.ui.size() == b.ui.size(),
          "borrowed projection preserves emitted row counts and publication identity");
    for (std::size_t row = 0; row < a.instances.size(); ++row)
        Check(a.instances[row].stable_id == b.instances[row].stable_id &&
                  a.instances[row].revisions == b.instances[row].revisions,
              "borrowed projection preserves exact transform, content, material and pose signatures");
    for (std::size_t row = 0; row < a.poses.size(); ++row)
        Check(a.poses[row].revision == b.poses[row].revision, "borrowed animation operands match owned format");
    for (std::size_t row = 0; row < a.persistent_vfx.size(); ++row)
        Check(a.persistent_vfx[row].stable_id == b.persistent_vfx[row].stable_id &&
                  a.persistent_vfx[row].kind == b.persistent_vfx[row].kind &&
                  a.persistent_vfx[row].active_tick == b.persistent_vfx[row].active_tick &&
                  a.persistent_vfx[row].expires == b.persistent_vfx[row].expires,
              "borrowed VFX retains emission order, activation and episode identities");
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
