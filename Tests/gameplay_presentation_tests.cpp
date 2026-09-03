#include "gameplay_test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <filesystem>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gameplay_test
{

void TestCursorMovement()
{
    auto data = QuietGameData();
    data.arena_half_extent = 10.0f;

    hs::GameSimulation simulation;
    Check(simulation.Initialize({11}, data).Succeeded(), "movement initialize");

    hs::HeldInputState held;
    held.move_held = true;
    held.move_target_world = {10.0f, 0.0f, 0.0f};
    held.aim_world = {10.0f, 0.0f, 0.0f};
    (void)Tick(simulation, held);
    const auto click_position = simulation.GetObservation().player_position;

    held.move_target_world = {0.0f, 0.0f, 10.0f};
    (void)Tick(simulation, held);
    const auto updated_target_position = simulation.GetObservation().player_position;
    Check(updated_target_position.y > click_position.y,
          "held RMB continuously updates the destination");

    held.move_held = false;
    for (std::uint32_t tick = 0; tick < 30; ++tick)
    {
        (void)Tick(simulation, held);
    }
    const auto released = simulation.GetObservation().player_position;
    Check(released.y > updated_target_position.y + 2.0f,
          "one RMB click keeps moving after button release");

    held.move_target_world = {-10.0f, 0.0f, 0.0f};
    held.move_held = true;
    (void)Tick(simulation, held);
    held.move_held = false;
    for (std::uint32_t tick = 0; tick < 240; ++tick)
    {
        (void)Tick(simulation, held);
    }
    const auto negative_edge = simulation.GetObservation().player_position;
    Check(std::abs(negative_edge.x + 10.0f) < 0.0001f &&
              std::abs(negative_edge.y) < 0.0001f,
          "new RMB click replaces destination and reaches it");
    Check(simulation.Shutdown().Succeeded(), "movement shutdown");
}

void TestUiHitRegionsMatchAnchors()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({13, false, true}, QuietGameData()).Succeeded(),
          "UI hit initialize");
    hs::HeldInputState held;
    hs::Sequence sequence{};

    SetCursor(held, 960.0f, 466.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().phase == hs::SessionPhase::MainMenu &&
              test_ui.page == hs::UiPage::Collection,
          "collection anchor opens collection instead of starting");

    const auto contains_text = [](const hs::RenderSnapshot &snapshot,
                                  std::string_view expected) {
        return std::ranges::any_of(snapshot.ui, [&](const hs::UiModel &model) {
            const auto end = std::ranges::find(model.utf8_text, '\0');
            return std::string_view(model.utf8_text.data(),
                                    end - model.utf8_text.begin()).contains(expected);
        });
    };
    hs::RenderSnapshotStorage snapshot(4, 2, 2, 64);
    Check(WriteSnapshot(simulation, snapshot), "collection snapshot");
    Check(contains_text(snapshot.View(), "스킬 도감") &&
              contains_text(snapshot.View(), "연속 추가 화살") &&
              contains_text(snapshot.View(), "액티브 연계 사격"),
          "collection shows all eight basic attack upgrades");

    SetCursor(held, 390.0f, 248.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    snapshot.Clear();
    Check(WriteSnapshot(simulation, snapshot), "selected collection snapshot");
    Check(contains_text(snapshot.View(), "관통 사격") &&
              contains_text(snapshot.View(), "후속 화살") &&
              contains_text(snapshot.View(), "빠른 재사용") &&
              contains_text(snapshot.View(), "관통 사격을 발사한 직후"),
          "collection selection shows the skill description and all upgrades");

    SetCursor(held, 390.0f, 928.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(test_ui.page == hs::UiPage::Root, "page back anchor returns to main menu");

    SetCursor(held, 960.0f, 616.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(test_ui.page == hs::UiPage::MainMenuSettings, "settings anchor opens settings");

    SetCursor(held, 700.0f, 348.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(last_ui_command && last_ui_command->kind == hs::UiCommandKind::SetVsync &&
              last_ui_command->value == 0,
          "settings VSync button emits exact target value");

    SetCursor(held, 1'050.0f, 278.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(last_ui_command && last_ui_command->kind ==
              hs::UiCommandKind::SetMasterVolumePercent &&
              last_ui_command->value == 90,
          "settings volume control emits clamped percent");

    SetCursor(held, 1'100.0f, 578.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(last_ui_command && last_ui_command->kind ==
              hs::UiCommandKind::BeginSkillRebind &&
              last_ui_command->value == 0,
          "settings key button begins the selected slot rebind");

    (void)TickEdge(simulation, hs::GameAction::Pause, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(test_ui.page == hs::UiPage::Root, "Esc returns from settings");

    SetCursor(held, 960.0f, 316.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().phase == hs::SessionPhase::Playing,
          "start anchor begins session");
    Check(simulation.Shutdown().Succeeded(), "UI hit shutdown");
}

void TestCharacterInformationPage()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({18}, QuietGameData()).Succeeded(),
          "character page initialize");
    for (std::uint32_t upgrade = 0; upgrade < hs::kUpgradeCount; ++upgrade)
        Debug(simulation, hs::DebugCommandKind::GrantUpgrade,
              static_cast<std::uint64_t>(hs::SkillKind::BasicAttack), upgrade);
    for (std::uint64_t relic = 0; relic < hs::kRelicCount; ++relic)
        Debug(simulation, hs::DebugCommandKind::GrantRelic, relic);
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::PiercingShot));
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::MultiShot));

    hs::HeldInputState held;
    hs::Sequence sequence{};
    const auto before = simulation.GetObservation().tick;
    const auto opened = TickEdge(simulation, hs::GameAction::CharacterPage,
                                 hs::EdgeKind::Pressed, sequence, held);
    Check(opened.phase == hs::SessionPhase::Paused && opened.tick == before &&
              test_ui.page == hs::UiPage::CharacterOverview,
          "Tab opens the character page and pauses simulation");

    const auto contains_text = [](const hs::RenderSnapshot &snapshot,
                                  std::string_view expected) {
        return std::ranges::any_of(snapshot.ui, [&](const hs::UiModel &model) {
            const auto end = std::ranges::find(model.utf8_text, '\0');
            return std::string_view(model.utf8_text.data(),
                                    end - model.utf8_text.begin()).contains(expected);
        });
    };
    hs::RenderSnapshotStorage snapshot(16, 2, 2, 128);
    Check(WriteSnapshot(simulation, snapshot), "character stats snapshot");
    Check(contains_text(snapshot.View(), "상세 능력치") &&
              contains_text(snapshot.View(), "기본 공격 피해") &&
              contains_text(snapshot.View(), "투자 포인트") &&
              contains_text(snapshot.View(), "현재 전투 기록"),
          "character overview shows effective stats and damage totals");

    SetCursor(held, 700, 165);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    snapshot.Clear();
    Check(WriteSnapshot(simulation, snapshot), "character skills snapshot");
    Check(test_ui.page == hs::UiPage::CharacterSkills &&
              contains_text(snapshot.View(), "연속 추가 화살") &&
              contains_text(snapshot.View(), "기본 공격을 유지하면"),
          "skill page shows current upgrades and detailed skill explanation");
    const auto upgrade_cards = std::ranges::count_if(
        snapshot.View().ui, [](const hs::UiModel &model) {
            return model.kind == hs::UiModel::Kind::Button &&
                   model.color_rgba == 0xFF2D4058u;
        });
    const auto upgrades_fit = std::ranges::all_of(
        snapshot.View().ui, [](const hs::UiModel &model) {
            if (model.kind != hs::UiModel::Kind::Button ||
                model.color_rgba != 0xFF2D4058u)
                return true;
            return model.anchor_pixels.x >= 780.0f &&
                   model.anchor_pixels.x + model.size_pixels.x <= 1'560.0f &&
                   model.anchor_pixels.y + model.size_pixels.y <= 920.0f;
        });
    Check(upgrade_cards == hs::kUpgradeCount && upgrades_fit,
          "all upgrade cards fit the character panel");

    SetCursor(held, 850, 240);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    SetCursor(held, 1'250, 240);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    auto reordered = simulation.GetObservation();
    Check(reordered.skill_loadout[0] == hs::SkillKind::Count &&
              reordered.skill_loadout[2] == hs::SkillKind::PiercingShot,
          "clicking an occupied then empty slot moves the skill");

    SetCursor(held, 1'050, 240);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    SetCursor(held, 1'250, 240);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    reordered = simulation.GetObservation();
    Check(reordered.skill_loadout[1] == hs::SkillKind::PiercingShot &&
              reordered.skill_loadout[2] == hs::SkillKind::MultiShot,
          "clicking two occupied slots swaps their QWER bindings");

    SetCursor(held, 1'000, 165);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    snapshot.Clear();
    Check(WriteSnapshot(simulation, snapshot), "character relic snapshot");
    Check(test_ui.page == hs::UiPage::CharacterStats &&
               contains_text(snapshot.View(), "피의 회복") &&
               contains_text(snapshot.View(), "출혈 중인 적을 처치하면") &&
              contains_text(snapshot.View(), "발동 0 · 피해 0 · 킬 0") &&
               !contains_text(snapshot.View(), "유물 00001"),
           "relic page shows acquired relic effects and contribution metrics");
    const auto has_relic_text = [&](std::uint32_t color, float y) {
        return std::ranges::any_of(snapshot.View().ui, [&](const hs::UiModel &model) {
            return model.kind == hs::UiModel::Kind::Text &&
                   model.color_rgba == color && model.anchor_pixels.x == 316.0f &&
                   model.anchor_pixels.y == y;
        });
    };
    Check(has_relic_text(0xFFFFD06Au, 247.0f) &&
              has_relic_text(0xFF9CC8FFu, 277.0f) &&
              has_relic_text(0xFFB8C2D0u, 305.0f),
          "relic cards separate name, contribution, and rule hierarchy");
    const auto relic_cards = std::ranges::count_if(
        snapshot.View().ui, [](const hs::UiModel &model) {
            return model.kind == hs::UiModel::Kind::Button &&
                   model.color_rgba == 0xFF2D4058u;
        });
    const auto relics_fit = std::ranges::all_of(
        snapshot.View().ui, [](const hs::UiModel &model) {
            if (model.kind != hs::UiModel::Kind::Button ||
                model.color_rgba != 0xFF2D4058u)
                return true;
            return model.anchor_pixels.x >= 300.0f &&
                   model.anchor_pixels.x + model.size_pixels.x <= 1'620.0f &&
                   model.anchor_pixels.y + model.size_pixels.y <= 935.0f;
        });
    Check(relic_cards == hs::kRelicCount && relics_fit,
          "all relic cards fit the character panel");

    const auto closed = TickEdge(simulation, hs::GameAction::CharacterPage,
                                 hs::EdgeKind::Pressed, sequence, held);
    Check(closed.phase == hs::SessionPhase::Playing && test_ui.page == hs::UiPage::Root,
          "Tab closes the character page");
    held.aim_world = {20.0f, 0.0f, 0.0f};
    (void)TickEdge(simulation, hs::GameAction::SkillW, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().cooldown_ticks[0] > 0,
          "the swapped W binding immediately casts piercing shot");
    Check(simulation.Shutdown().Succeeded(), "character page shutdown");
}

void TestPauseMenuActions()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({19}, QuietGameData()).Succeeded(),
          "pause menu initialize");
    hs::HeldInputState held;
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::Pause, hs::EdgeKind::Pressed,
                   sequence, held);

    hs::RenderSnapshotStorage snapshot(16, 2, 2, 32);
    Check(WriteSnapshot(simulation, snapshot), "pause menu snapshot");
    const auto contains_text = [&](std::string_view expected) {
        return std::ranges::any_of(snapshot.View().ui, [&](const hs::UiModel &model) {
            const auto end = std::ranges::find(model.utf8_text, '\0');
            return std::string_view(model.utf8_text.data(),
                                    end - model.utf8_text.begin()).contains(expected);
        });
    };
    Check(contains_text("계속") && contains_text("설정") &&
              contains_text("게임 종료"),
          "Esc menu shows continue, settings, and quit actions");

    SetCursor(held, 960, 556);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().phase == hs::SessionPhase::Paused &&
              test_ui.page == hs::UiPage::PauseSettings,
          "pause settings keeps the game paused");
    snapshot.Clear();
    Check(WriteSnapshot(simulation, snapshot), "in-game settings snapshot");
    Check(contains_text("화면:") && !contains_text("상세 능력치") &&
              !contains_text("캐릭터 정보") && !contains_text("스킬·강화"),
          "in-game settings does not render the character information page");

    (void)TickEdge(simulation, hs::GameAction::Pause, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().phase == hs::SessionPhase::Paused &&
              test_ui.page == hs::UiPage::Root,
          "Esc returns from settings to the pause menu");

    SetCursor(held, 960, 456);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().phase == hs::SessionPhase::Playing,
          "continue resumes gameplay");

    (void)TickEdge(simulation, hs::GameAction::Pause, hs::EdgeKind::Pressed,
                   sequence, held);
    SetCursor(held, 960, 656);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().phase == hs::SessionPhase::QuitRequested,
          "pause menu quit requests application exit");
    Check(simulation.Shutdown().Succeeded(), "pause menu shutdown");
}

void TestSkillUpgradeCardsHaveDescriptions()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({73}, QuietGameData()).Succeeded(),
          "upgrade description initialize");
    Debug(simulation, hs::DebugCommandKind::GrantExperience, 20);
    (void)Tick(simulation);
    const auto probe = simulation.GetObservation();
    Check(probe.phase == hs::SessionPhase::CardSelection,
          "upgrade description card selection");

    hs::RenderSnapshotStorage snapshot(16, 2, 2, 128);
    Check(WriteSnapshot(simulation, snapshot), "upgrade description snapshot");
    bool checked_upgrade{};
    for (std::size_t card_index = 0; card_index < probe.card_count; ++card_index)
    {
        const auto card = probe.cards[card_index];
        if (card.kind != hs::CardKind::BasicUpgrade &&
            card.kind != hs::CardKind::SkillUpgrade)
            continue;
        checked_upgrade = true;
        const auto heading = std::format("강화 {}",
                                         static_cast<unsigned>(card.upgrade) + 1);
        const auto expected_x = 384.0f + static_cast<float>(card_index) * 420.0f;
        const auto has_heading = std::ranges::any_of(
            snapshot.View().ui, [&](const hs::UiModel &model) {
                const auto end = std::ranges::find(model.utf8_text, '\0');
                const std::string_view text(model.utf8_text.data(),
                                            end - model.utf8_text.begin());
                return model.kind == hs::UiModel::Kind::Text &&
                       model.color_rgba == 0xFF78B8FFu &&
                       model.anchor_pixels.x == expected_x && text.contains(heading);
            });
        const auto has_description = std::ranges::any_of(
            snapshot.View().ui, [&](const hs::UiModel &model) {
                const auto end = std::ranges::find(model.utf8_text, '\0');
                const std::string_view text(model.utf8_text.data(),
                                            end - model.utf8_text.begin());
                return model.kind == hs::UiModel::Kind::Text &&
                       model.color_rgba == 0xFFD7E0ECu &&
                       model.anchor_pixels.x == expected_x &&
                       model.anchor_pixels.y == 420.0f && text.size() > 30;
            });
        Check(has_heading && has_description,
              "skill upgrade card separates heading and readable description");
    }
    Check(checked_upgrade, "upgrade description test has an upgrade card");
    Check(simulation.Shutdown().Succeeded(), "upgrade description shutdown");
}

void TestBossAnimationReachesReleaseFrame()
{
    hs::GameReadModelStorage model_storage;
    hs::EnemyView enemy{};
    enemy.id = {1};
    enemy.boss = hs::BossKind::FiveMinute;
    enemy.max_health = enemy.health = 100;
    enemy.boss_action_started = 0;
    enemy.boss_action_until = 10;
    model_storage.AddEnemy(enemy);
    hs::BossActionView pending_action{};
    pending_action.boss_id = enemy.id.value;
    pending_action.animation_started = 0;
    pending_action.execute_tick = 10;
    model_storage.AddBossAction(pending_action);
    hs::RenderSnapshotStorage snapshot(8, 1, 1, 16);
    hs::SettingsData settings{};
    const auto project = [&](hs::Tick tick) {
        model_storage.tick = tick;
        snapshot.Clear();
        return hs::ProjectRenderSnapshot(model_storage.View(), DefaultContent().presentation,
                                         test_ui, settings, snapshot);
    };
    Check(project(9), "boss animation pre-release projection");
    Check(snapshot.View().poses.size() == 2 && snapshot.View().poses.back().normalized_time > 0.89f,
          "boss animation progresses monotonically before release");
    Check(project(10), "boss animation release projection");
    Check(snapshot.View().poses.size() == 2 && snapshot.View().poses.back().normalized_time == 1.0f,
          "boss animation reaches release frame");

    enemy.boss_action_started = enemy.boss_action_until = 0;
    const auto project_pending = [&](hs::Tick start, hs::Tick execute, hs::Tick tick) {
        model_storage.Clear();
        model_storage.AddEnemy(enemy);
        hs::BossActionView action{};
        action.boss_id = enemy.id.value;
        action.animation_started = start;
        action.execute_tick = execute;
        model_storage.AddBossAction(action);
        return project(tick);
    };
    Check(project_pending(0, 10, 9) && snapshot.View().poses.back().normalized_time > 0.89f,
          "first boss action approaches its release frame");
    Check(project_pending(10, 20, 10) && snapshot.View().poses.back().normalized_time == 0.0f,
          "chained boss action restarts its animation");
}

void TestMonsterVisualScalesAndAttackFacing()
{
    hs::GameReadModelStorage model_storage;
    const auto add_enemy = [&](hs::EntityId id, hs::EnemyKind kind) {
        hs::EnemyView enemy{};
        enemy.id = id;
        enemy.kind = kind;
        enemy.health = enemy.max_health = 100;
        model_storage.AddEnemy(enemy);
    };
    add_enemy({1}, hs::EnemyKind::Melee);
    add_enemy({2}, hs::EnemyKind::Ranged);
    add_enemy({3}, hs::EnemyKind::Suicide);

    hs::RenderSnapshotStorage snapshot(64, 4, 4, 16);
    hs::SettingsData settings{};
    Check(hs::ProjectRenderSnapshot(model_storage.View(), DefaultContent().presentation,
                                    test_ui, settings, snapshot),
          "monster scale projection");
    const auto instances = snapshot.View().instances;
    const auto check_scale = [&](hs::RenderMesh mesh, float expected) {
        const auto instance = std::ranges::find_if(instances, [mesh](const auto &candidate) {
            return candidate.mesh == mesh;
        });
        Check(instance != instances.end(), "monster mesh is projected");
        Check(instance->scale.x == expected && instance->scale.y == expected &&
                  instance->scale.z == expected,
              "monster visual scale is uniform and mesh-specific");
    };
    check_scale(hs::RenderMesh::MonsterMelee, 1.50f);
    check_scale(hs::RenderMesh::MonsterRanged, 2.00f);
    check_scale(hs::RenderMesh::MonsterSuicide, 2.40f);
    hs::GameReadModelStorage facing_model;
    hs::EnemyView ranged{};
    ranged.id = {4};
    ranged.kind = hs::EnemyKind::Ranged;
    ranged.health = ranged.max_health = 100;
    ranged.attacking = true;
    ranged.locked_aim = {1.0f, 0.0f};
    facing_model.AddEnemy(ranged);
    snapshot.Clear();
    Check(hs::ProjectRenderSnapshot(facing_model.View(), DefaultContent().presentation,
                                    test_ui, settings, snapshot),
          "ranged attack facing projection");
    const auto projected_ranged = std::ranges::find_if(
        snapshot.View().instances, [](const auto &instance) {
            return instance.mesh == hs::RenderMesh::MonsterRanged;
        });
    Check(projected_ranged != snapshot.View().instances.end() &&
              std::abs(projected_ranged->yaw - std::numbers::pi_v<float> * 0.5f) < 0.001f,
          "stationary ranged enemy faces its locked attack direction");
}

void TestRelicTriggerProjection()
{
    constexpr std::array<std::string_view, 8> assets{
        "particle.relic.projectile_cadence_reward",
        "particle.relic.pre_damage_guard",
        "particle.relic.slow_synergy",
        "particle.relic.area_resonance",
        "particle.relic.boss_pressure",
        "particle.relic.hit_streak_reward",
        "particle.relic.pickup_reward",
        "particle.relic.low_health_survival"};
    for (std::size_t index = 0; index < assets.size(); ++index)
    {
        hs::DomainSignal signal;
        signal.kind = hs::DomainSignalKind::RelicTriggered;
        signal.context = static_cast<std::uint8_t>(
            static_cast<std::size_t>(hs::RelicKind::ProjectileCadenceReward) + index);
        std::array<hs::PresentationEvent, 1> output{};
        const auto count = hs::ProjectDomainSignal(signal, output);
        Check(count == 1 && output[0].kind == hs::PresentationKind::Vfx &&
                  output[0].asset.value == hs::MakeAssetId(assets[index]).value,
              "new relic activation projects its distinct VFX");
    }
}

void RunGameplayPresentationTests()
{
    TestCursorMovement();
    TestUiHitRegionsMatchAnchors();
    TestCharacterInformationPage();
    TestPauseMenuActions();
    TestSkillUpgradeCardsHaveDescriptions();
    TestBossAnimationReachesReleaseFrame();
    TestMonsterVisualScalesAndAttackFacing();
    TestRelicTriggerProjection();
}

} // namespace gameplay_test
