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
    check_scale(hs::RenderMesh::MonsterMelee, 1.25f);
    check_scale(hs::RenderMesh::MonsterRanged, 1.40625f);
    check_scale(hs::RenderMesh::MonsterSuicide, 1.5625f);
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

void TestEnemyAnimationStateSignals()
{
    hs::GameReadModelStorage storage;
    hs::EnemyView enemy{};
    enemy.id = {7}; enemy.kind = hs::EnemyKind::Ranged;
    enemy.health = enemy.max_health = 100;
    storage.AddEnemy(enemy);
    hs::EnemyAnimationState state;
    state.Update(storage.View(), {});
    enemy.health = 90; storage.Clear(); storage.AddEnemy(enemy);
    storage.tick = 1; state.Update(storage.View(), {});
    Check(state.RecoilStart(7).has_value(), "nonattacking health drop starts recoil");
    enemy.attacking = true;
    storage.Clear(); storage.AddEnemy(enemy); storage.tick = 2;
    state.Update(storage.View(), {});
    Check(!state.RecoilStart(7).has_value(), "attack draw takes priority over hit recoil");
    hs::DomainSignal release{}; release.kind = hs::DomainSignalKind::RangedEnemyReleased;
    release.tick = 1; release.source_entity_id = 7; release.direction = {1, 0, 0};
    state.Update(storage.View(), std::span(&release, 1));
    Check(state.ReleaseTick(7) == 1, "ranged release keeps source entity id");
    hs::DomainSignal death{}; death.kind = hs::DomainSignalKind::EnemyDied;
    death.tick = 1; death.source_entity_id = 7; death.context = static_cast<std::uint8_t>(hs::EnemyKind::Ranged);
    state.Update(storage.View(), std::span(&death, 1));
    Check(state.DeathPoses().size() == 1, "normal death creates corpse pose");
    hs::EnemyAnimationState suicide_state;
    hs::GameReadModelStorage suicide_storage;
    suicide_storage.tick = 1;
    suicide_state.Update(suicide_storage.View(), {});
    Check(suicide_state.DeathPoses().empty(), "suicide explosion without death signal has no corpse");
    storage.tick = 49; state.Update(storage.View(), {});
    Check(state.DeathPoses().empty(), "corpse expires after 48 ticks");
}

void TestSlimeAttackPoseTransitions()
{
    hs::GameReadModelStorage model;
    hs::EnemyAnimationState state;
    hs::EnemyView enemy{};
    enemy.id = {91}; enemy.kind = hs::EnemyKind::Suicide;
    enemy.health = enemy.max_health = 100;
    enemy.attacking = true; enemy.attack_started = 10; enemy.attack_resolve = 70;
    enemy.locked_aim = {1, 0};
    float projected_yaw{};
    const auto project = [&](hs::Tick tick, std::span<const hs::DomainSignal> signals = {}) {
        model.Clear(); model.tick = tick; model.AddEnemy(enemy);
        state.Update(model.View(), signals);
        hs::RenderSnapshotStorage snapshot(30000, 2048, 8, 2048);
        Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
              test_ui, hs::SettingsData{}, snapshot, 0xFF, &state), "slime state snapshot");
        projected_yaw = snapshot.View().instances[snapshot.View().poses.back().instance_index].yaw;
        if (state.CancelProgress(enemy.id.value))
        {
            const auto &pose = snapshot.View().poses.back();
            const auto &instance = snapshot.View().instances[pose.instance_index];
            Check(std::abs(instance.yaw - std::numbers::pi_v<float> * 0.5f) < 0.0001f,
                  "cancelled charge keeps its original attack facing");
        }
        return snapshot.View().poses.back();
    };
    const auto charging = project(39);
    enemy.attacking = false; enemy.velocity = {0, 1};
    const auto cancelled = project(40);
    Check(cancelled.clip == hs::CharacterAnimationClip::Draw &&
          std::abs(cancelled.normalized_time - 0.5f) < 0.0001f &&
          cancelled.normalized_time >= charging.normalized_time,
          "suicide cancellation starts at exact current charge progress");
    const auto reversing = project(58);
    Check(reversing.clip == hs::CharacterAnimationClip::Draw &&
          std::abs(reversing.normalized_time - 0.25f) < 0.0001f,
          "suicide cancellation reverses the original draw slowly");
    Check(project(76).clip == hs::CharacterAnimationClip::Run,
          "suicide cancellation returns to locomotion after 36 ticks");
    enemy.attacking = true; enemy.attack_started = 80; enemy.attack_resolve = 140;
    (void)project(90); enemy.attacking = false; (void)project(91);
    enemy.attacking = true; enemy.attack_started = 92; enemy.attack_resolve = 152;
    Check(project(92).normalized_time == 0.0f && !state.CancelProgress(91),
          "new attack clears the old reversed charge");
    enemy.kind = hs::EnemyKind::Ranged; enemy.attacking = false; enemy.velocity = {};
    hs::DomainSignal release{}; release.kind = hs::DomainSignalKind::RangedEnemyReleased;
    release.tick = 100; release.source_entity_id = 91; release.direction = {1, 0, 0};
    const auto spit = project(100, std::span(&release, 1));
    Check(spit.clip == hs::CharacterAnimationClip::Draw && spit.normalized_time == 1.0f,
          "ranged release preserves fully inflated draw endpoint");
    Check(project(101).clip == hs::CharacterAnimationClip::Recoil,
          "ranged release proceeds into spit recovery");
    Check(std::abs(projected_yaw - std::numbers::pi_v<float> * 0.5f) < 0.0001f,
          "stationary ranged spit retains release facing on the next tick");
    Check(project(118).clip == hs::CharacterAnimationClip::Recoil &&
              std::abs(projected_yaw - std::numbers::pi_v<float> * 0.5f) < 0.0001f,
          "ranged spit retains release facing through its final recovery tick");
    Check(project(119).clip == hs::CharacterAnimationClip::Idle &&
              !state.IsAttackRecoil(91) && std::abs(projected_yaw) < 0.0001f,
          "ranged facing lock expires with attack recovery");
    enemy.kind = hs::EnemyKind::Melee; enemy.attacking = true; enemy.velocity = {-1, 0};
    enemy.attack_started = 120; enemy.attack_resolve = 130;
    (void)project(129); enemy.attacking = false;
    Check(project(130).normalized_time == 1.0f,
          "melee resolve preserves headbutt endpoint");
    Check(project(131).clip == hs::CharacterAnimationClip::Recoil,
          "melee strike enters recovery after contact");
    Check(std::abs(projected_yaw - std::numbers::pi_v<float> * 0.5f) < 0.0001f,
          "melee headbutt recovery retains attack facing against navigation velocity");
    Check(project(148).clip == hs::CharacterAnimationClip::Recoil &&
              std::abs(projected_yaw - std::numbers::pi_v<float> * 0.5f) < 0.0001f,
          "melee headbutt retains facing through its final recovery tick");
    Check(project(149).clip == hs::CharacterAnimationClip::Run,
          "melee recovery expires after 18 ticks");
    Check(!state.IsAttackRecoil(91) && std::abs(projected_yaw + std::numbers::pi_v<float> * 0.5f) < 0.0001f,
          "melee resumes navigation facing after recovery");
    enemy.attacking = true; enemy.attack_started = 150; enemy.attack_resolve = 153;
    (void)project(152); enemy.attacking = false; (void)project(153); (void)project(154);
    enemy.health = 90;
    Check(project(155).clip == hs::CharacterAnimationClip::Recoil && !state.IsAttackRecoil(91) &&
              std::abs(projected_yaw + std::numbers::pi_v<float> * 0.5f) < 0.0001f,
          "damage recoil supersedes the attack facing lock");
    enemy.attacking = true; enemy.attack_started = 160; enemy.attack_resolve = 190;
    (void)project(170); enemy.attacking = false;
    Check(project(171).clip == hs::CharacterAnimationClip::Run,
          "interrupted melee windup does not invent a strike recovery");
}

void TestAuthoredEnvironmentProjection()
{
    auto rules = QuietGameData();
    hs::GameSimulation simulation;
    constexpr std::uint64_t seed = 0x1234567887654321ull;
    Check(simulation.Initialize({seed}, rules).Succeeded(), "environment initialize");
    hs::RenderSnapshotStorage snapshot(30000, 2048, 8, 2048);
    Check(WriteSnapshot(simulation, snapshot), "environment snapshot capacity");
    const auto instances = snapshot.View().instances;
    std::size_t trees{}, grass{}, ground{};
    for (std::size_t i = 0; i < instances.size(); ++i)
    {
        const auto &item = instances[i];
        Check(item.mesh != hs::RenderMesh::DirtPatch, "dirt path blends in terrain without coplanar rectangle overlays");
        if (item.mesh == hs::RenderMesh::Ground)
        {
            ++ground;
            Check(item.environment_seed == static_cast<std::uint32_t>(seed ^ (seed >> 32)),
                  "all continuous ground tiles share the map seed");
        }
        if (item.mesh == hs::RenderMesh::TreeTrunk)
        {
            ++trees;
            Check(i + 1 < instances.size(), "tree has a canopy");
            const auto &canopy = instances[i + 1];
            Check(canopy.mesh == hs::RenderMesh::TreeCanopy && canopy.environment_variant == item.environment_variant &&
                      item.environment_variant < 3, "trunk and canopy share one authored tree species");
            Check(canopy.position.x == item.position.x && canopy.position.y == item.position.y &&
                      canopy.position.z == item.position.z && canopy.yaw == item.yaw &&
                      canopy.scale.x == item.scale.x && canopy.scale.y == item.scale.y &&
                      canopy.scale.z == item.scale.z && item.scale.x == item.scale.y && item.scale.y == item.scale.z,
                  "authored tree parts share ground origin and uniform metre scale");
        }
        if (item.mesh == hs::RenderMesh::Grass)
        {
            ++grass;
            Check(item.environment_variant < 4 && item.scale.x == item.scale.z &&
                      item.scale.x >= 0.55f && item.scale.x <= 1.2f &&
                      item.scale.y >= 0.18f && item.scale.y <= 0.4f,
                  "grass retains dense horizontal coverage at the reduced meter-scale height");
        }
    }
    Check(trees > 0 && grass > 0 && ground > 0, "authored environment is present in normal gameplay");
    Check(ground == 1, "exterior environment uses one continuous floor");
    const auto &floor = std::ranges::find_if(instances, [](const auto &item) { return item.mesh == hs::RenderMesh::Ground; });
    Check(floor != instances.end() && floor->scale.x >= 52.0f && floor->scale.z >= 52.0f,
          "continuous floor covers exterior tree rows");
}

void TestDenseGrassProjection()
{
    hs::GameReadModelStorage model;
    model.seed = 0x1234567887654321ull;
    model.arena_half_extent = 16.0f;
    model.arena_boundary.count = 4;
    model.arena_boundary.points[0] = {-16, -16};
    model.arena_boundary.points[1] = {-16, 16};
    model.arena_boundary.points[2] = {16, 16};
    model.arena_boundary.points[3] = {16, -16};
    model.arena_obstacle_count = 1;
    model.arena_obstacles[0].center = {9, 0};
    model.arena_obstacles[0].radius = 2.0f;
    const auto project_grass = [&] {
        hs::RenderSnapshotStorage snapshot(30000, 16, 8, 2048);
        Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
                                       test_ui, hs::SettingsData{}, snapshot), "dense grass snapshot");
        std::vector<hs::RenderInstance> result;
        for (const auto &instance : snapshot.View().instances)
            if (instance.mesh == hs::RenderMesh::Grass) result.push_back(instance);
        return result;
    };
    const auto grass = project_grass();
    const auto repeated = project_grass();
    Check(grass.size() > 600 && grass.size() == repeated.size(), "grass forms dense patches within allowed terrain");
    std::array<bool, 4> variants{};
    std::size_t grouped{};
    for (std::size_t i = 0; i < grass.size(); ++i)
    {
        const auto &item = grass[i];
        Check(item.position.x == repeated[i].position.x && item.position.z == repeated[i].position.z &&
                  item.yaw == repeated[i].yaw && item.scale.x == repeated[i].scale.x &&
                  item.environment_variant == repeated[i].environment_variant,
              "grass placement is deterministic for the map seed");
        Check(std::abs(item.position.x) <= 14 && std::abs(item.position.z) <= 14 &&
                  std::abs(item.position.x - std::sin(item.position.z * 0.085f) * 6.5f) >= 4.8f,
              "dense grass preserves arena inset and open path");
        const auto dx = item.position.x - 9.0f;
        Check(dx * dx + item.position.z * item.position.z >= 2.45f * 2.45f,
              "dense grass preserves obstacle clearance");
        variants[item.environment_variant] = true;
        std::size_t neighbours{};
        for (std::size_t j = 0; j < grass.size(); ++j)
        {
            if (i == j) continue;
            const auto x = item.position.x - grass[j].position.x;
            const auto z = item.position.z - grass[j].position.z;
            if (x * x + z * z < 0.65f * 0.65f) ++neighbours;
        }
        if (neighbours >= 3) ++grouped;
    }
    Check(grouped * 4 > grass.size() * 3, "most grass has several nearby neighbours instead of isolated tufts");
    Check(std::ranges::all_of(variants, [](bool present) { return present; }), "all four grass variants populate patches");
    model.seed ^= 1ull << 40;
    const auto changed = project_grass();
    Check(!changed.empty() && (changed.front().position.x != grass.front().position.x ||
                              changed.front().position.z != grass.front().position.z),
          "high seed bits change grass placement");
}

void TestProjectileProjectionCarriesAuthoritativeOwner()
{
    hs::GameReadModelStorage storage;
    hs::ProjectileView projectile{};
    projectile.id = {42};
    projectile.player_owned = true;
    projectile.position = {8.0f, 2.0f};
    projectile.previous_position = {7.0f, 2.0f};
    projectile.velocity = {60.0f, 0.0f};
    projectile.spawned_tick = 10;
    projectile.skill = hs::SkillKind::ChargedShot;
    projectile.radius = 0.15f;
    projectile.cast_id = 99;
    storage.AddProjectile(projectile);
    // One actor may project several persistent outputs. Reservation is not a cap.
    hs::RenderSnapshotStorage snapshot(1, 1, 1, 16);
    Check(hs::ProjectRenderSnapshot(storage.View(), DefaultContent().presentation,
                                    test_ui, hs::SettingsData{}, snapshot),
          "projectile owner projection");
    const auto head = std::ranges::find_if(
        snapshot.View().persistent_vfx, [](const auto &visual) {
            return visual.kind == hs::PersistentVfxKind::ProjectileHead;
        });
    Check(head != snapshot.View().persistent_vfx.end() &&
              head->projectile_owner_id == 42 &&
              head->projectile_previous_position.x == 7.0f &&
              head->projectile_velocity.x == 60.0f &&
              head->projectile_spawned_tick == 10 &&
              head->effect_asset.value == hs::MakeAssetId("particle.skill.charged_shot").value,
          "projectile head dropped authoritative owner fields");
}

void TestProjectileUpgradeAttachmentEpisodes()
{
    hs::GameReadModelStorage model;
    model.tick = 20;
    model.session_id = 3;
    hs::ProjectileView retarget{};
    retarget.id = {42};
    retarget.player_owned = true;
    retarget.skill = hs::SkillKind::MultiShot;
    retarget.origin = hs::EffectOrigin::Derived;
    retarget.source_upgrade = 7;
    retarget.homing = true;
    retarget.spawned_tick = 10;
    retarget.position = {3.0f, 4.0f};
    retarget.previous_position = {2.0f, 4.0f};
    retarget.velocity = {12.0f, 0.0f};
    retarget.radius = 0.18f;
    hs::ProjectileView ricochet{};
    ricochet.id = {43};
    ricochet.player_owned = true;
    ricochet.skill = hs::SkillKind::RicochetArrow;
    ricochet.spawned_tick = 5;
    ricochet.position = {-3.0f, 6.0f};
    ricochet.previous_position = {-4.0f, 6.0f};
    ricochet.velocity = {18.0f, 0.0f};
    ricochet.radius = 0.25f;
    ricochet.bleed_extend_ticks = {19, 20};
    const auto add_live = [&] {
        model.AddProjectile(retarget);
        model.AddProjectile(ricochet);
    };
    add_live();
    hs::RenderSnapshotStorage snapshot(16, 2, 2, 8);
    const auto project = [&] {
        snapshot.Clear();
        Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
            test_ui, hs::SettingsData{}, snapshot), "projectile upgrade owner snapshot");
    };
    const auto find = [&](hs::PersistentVfxKind kind, hs::Tick started)
        -> const hs::PersistentVfxVisual * {
        const auto owners = snapshot.View().persistent_vfx;
        const auto found = std::ranges::find_if(owners,
            [=](const auto &visual) {
                return visual.kind == kind && visual.active_tick == started;
            });
        return found == owners.end() ? nullptr : &*found;
    };
    project();
    const auto *lock = find(hs::PersistentVfxKind::MultishotRetarget, 10);
    const auto *first = find(hs::PersistentVfxKind::RicochetBleedExtend, 19);
    const auto *second = find(hs::PersistentVfxKind::RicochetBleedExtend, 20);
    Check(lock && lock->effect_asset.value == hs::MakeAssetId("particle.upgrade.retarget").value &&
        lock->skill == static_cast<std::uint8_t>(hs::SkillKind::MultiShot) &&
        lock->source_upgrade == 7 && lock->expires == 28 &&
        lock->projectile_owner_id == 42 && lock->projectile_spawned_tick == 10 &&
        lock->projectile_previous_position.x == 2.0f &&
        lock->projectile_current_position.x == 3.0f &&
        lock->projectile_hitbox_radius == retarget.radius,
        "retarget child lost its real path and fixed episode clock");
    Check(first && second && first->stable_id != second->stable_id &&
        first->effect_asset.value == hs::MakeAssetId("particle.upgrade.ricochet.bleed_extend").value &&
        first->skill == static_cast<std::uint8_t>(hs::SkillKind::RicochetArrow) &&
        first->source_upgrade == 2 && first->expires == 40 && second->expires == 41 &&
        first->projectile_owner_id == 43 && first->entity_render_id == ((3ull << 60) | 43) &&
        first->radius == ricochet.radius && first->entity_health_fraction == 1.0f,
        "overlapping ricochet procs lost separate projectile attachment episodes");
    const auto lock_id = lock->stable_id;
    const auto first_id = first->stable_id;
    const auto second_id = second->stable_id;
    model.Clear();
    model.tick = 21;
    retarget.position = {5.0f, 4.0f};
    ricochet.position = {-1.0f, 6.0f};
    add_live();
    project();
    lock = find(hs::PersistentVfxKind::MultishotRetarget, 10);
    first = find(hs::PersistentVfxKind::RicochetBleedExtend, 19);
    second = find(hs::PersistentVfxKind::RicochetBleedExtend, 20);
    Check(lock && first && second && lock->stable_id == lock_id &&
        first->stable_id == first_id && second->stable_id == second_id &&
        lock->position.x == 5.0f && first->position.x == -1.0f &&
        second->position.x == -1.0f,
        "moving projectiles restarted or detached upgrade owners");
    model.session_id = 4;
    project();
    Check(find(hs::PersistentVfxKind::MultishotRetarget, 10)->stable_id != lock_id &&
        find(hs::PersistentVfxKind::RicochetBleedExtend, 19)->stable_id != first_id,
        "new session reused projectile upgrade owner identity");
    model.tick = 28;
    project();
    Check(!find(hs::PersistentVfxKind::MultishotRetarget, 10) &&
        find(hs::PersistentVfxKind::RicochetBleedExtend, 19) &&
        find(hs::PersistentVfxKind::RicochetBleedExtend, 20),
        "retarget owner outlived its 18 ticks or truncated ricochet episodes");
    model.tick = 40;
    project();
    Check(!find(hs::PersistentVfxKind::RicochetBleedExtend, 19) &&
        find(hs::PersistentVfxKind::RicochetBleedExtend, 20),
        "ricochet proc owners do not expire independently after 21 ticks");
    model.Clear();
    model.tick = 21;
    project();
    Check(std::ranges::none_of(snapshot.View().persistent_vfx, [](const auto &visual) {
        return visual.kind == hs::PersistentVfxKind::MultishotRetarget ||
               visual.kind == hs::PersistentVfxKind::RicochetBleedExtend;
    }), "dead projectile retained an upgrade attachment");
    retarget.homing = false;
    model.AddProjectile(retarget);
    project();
    Check(!find(hs::PersistentVfxKind::MultishotRetarget, 10),
        "retarget owner appeared without the successful homing child");
}

void TestPresentationGeometryTransport()
{
    hs::DomainSignal signal{};
    signal.kind = hs::DomainSignalKind::FireAreaPulse;
    signal.sequence = 77;
    signal.position = {2.0f, 0.12f, -3.0f};
    signal.geometry.kind = hs::DomainSignalGeometryKind::Circle;
    signal.geometry.radius = 4.5f;
    signal.geometry.start_tick = 10;
    signal.geometry.end_tick = 70;
    signal.geometry.source_id = 9001;
    std::array<hs::PresentationEvent, 3> output{};
    output[0].geometry.kind = hs::PresentationGeometryKind::Cone;
    const auto count = hs::ProjectDomainSignal(signal, output);
    Check(count >= 1 && output[0].geometry.kind == hs::PresentationGeometryKind::Circle &&
              output[0].geometry.radius == 4.5f &&
              output[0].geometry.start_tick == 10 &&
              output[0].geometry.end_tick == 70 &&
              output[0].geometry.source_id == 9001,
          "presentation projection dropped gameplay geometry");
    signal.kind = hs::DomainSignalKind::TrapDamaged;
    signal.geometry.radius = 1.8f;
    const auto damaged_count = hs::ProjectDomainSignal(signal, output);
    Check(damaged_count == 2 &&
              output[0].asset.value == hs::MakeAssetId("particle.skill.trap.damaged").value &&
              output[0].geometry.kind == hs::PresentationGeometryKind::Circle &&
              output[0].geometry.radius == 1.8f &&
              output[1].kind == hs::PresentationKind::Audio,
          "trap damage visual lost its real area radius or audio");
}

void TestProjectileImpactGeometryTransport()
{
    hs::DomainSignal signal{};
    signal.kind = hs::DomainSignalKind::EnemyRangedProjectileImpact;
    signal.sequence = 712;
    signal.position = {4.0f, 0.45f, -2.0f};
    signal.geometry.kind = hs::DomainSignalGeometryKind::Projectile;
    signal.geometry.velocity = {0.0f, 0.0f, 24.0f};
    signal.geometry.radius = 0.18f;
    signal.geometry.source_id = 712;
    std::array<hs::PresentationEvent, 2> output{};
    const auto count = hs::ProjectDomainSignal(signal, output);
    Check(count == 1 && output[0].kind == hs::PresentationKind::Vfx &&
              output[0].asset.value == hs::MakeAssetId("particle.enemy.ranged.impact").value &&
              output[0].position.y == 0.45f &&
              output[0].geometry.kind == hs::PresentationGeometryKind::Projectile &&
              output[0].geometry.velocity.z == 24.0f &&
              output[0].geometry.radius == 0.18f &&
              output[0].geometry.source_id == 712,
          "projectile impact projection dropped authoritative contact geometry");
}

void TestBossTelegraphProjectsGeometryAndAudio()
{
    hs::DomainSignal ranged{};
    ranged.kind = hs::DomainSignalKind::RangedEnemyTelegraphed;
    ranged.sequence = 711;
    ranged.geometry.kind = hs::DomainSignalGeometryKind::Line;
    ranged.geometry.width = 0.74f; ranged.geometry.range = 31.0f;
    ranged.geometry.source_id = 711; ranged.geometry.start_tick = 20; ranged.geometry.end_tick = 67;
    ranged.geometry.end_position = {-21.0f, 0.025f, 0.0f};
    std::array<hs::PresentationEvent, 3> ranged_output{};
    const auto ranged_count = hs::ProjectDomainSignal(ranged, ranged_output);
    Check(ranged_count == 1 && ranged_output[0].kind == hs::PresentationKind::Vfx &&
          ranged_output[0].asset.value == hs::MakeAssetId("particle.enemy.ranged.telegraph_line").value &&
          ranged_output[0].geometry.kind == hs::PresentationGeometryKind::Line &&
          ranged_output[0].geometry.source_id == 711 && ranged_output[0].geometry.end_tick == 67 &&
          ranged_output[0].geometry.width == 0.74f && ranged_output[0].geometry.end_position.x == -21.0f,
          "ranged warning projects exact line geometry without an added audio cue");

    hs::DomainSignal shockwave{};
    shockwave.kind = hs::DomainSignalKind::BossShockwaveTelegraphed;
    shockwave.geometry.kind = hs::DomainSignalGeometryKind::RingGaps;
    shockwave.geometry.inner_radius = 1.7f; shockwave.geometry.outer_radius = 13.0f;
    shockwave.geometry.gap_count = 3; shockwave.geometry.gap_half_width_degrees = 20;
    shockwave.geometry.source_id = 831; shockwave.geometry.start_tick = 10; shockwave.geometry.end_tick = 83;
    std::array<hs::PresentationEvent, 3> shockwave_output{};
    const auto shockwave_count = hs::ProjectDomainSignal(shockwave, shockwave_output);
    Check(shockwave_count == 2 && std::ranges::any_of(shockwave_output, [](const auto &e) {
        return e.kind == hs::PresentationKind::Vfx && e.asset.value == hs::MakeAssetId("particle.boss.shockwave.telegraph").value &&
            e.geometry.kind == hs::PresentationGeometryKind::RingGaps && e.geometry.inner_radius == 1.7f &&
            e.geometry.outer_radius == 13.0f && e.geometry.source_id == 831;
    }), "shockwave telegraph preserves ring geometry alongside its existing audio cue");
    const auto verify = [](hs::DomainSignalKind kind, hs::DomainSignalGeometry geometry,
                           std::string_view vfx_asset, std::string_view audio_asset) {
        hs::DomainSignal signal{};
        signal.kind = kind;
        signal.sequence = 803;
        signal.position = {2.0f, 0.025f, -3.0f};
        signal.direction = {0.6f, 0.0f, 0.8f};
        signal.geometry = geometry;
        std::array<hs::PresentationEvent, 4> output{};
        const auto count = hs::ProjectDomainSignal(signal, output);
        const auto vfx = std::ranges::find_if(
            output.begin(), output.begin() + count,
            [vfx_asset](const hs::PresentationEvent &event) {
                return event.kind == hs::PresentationKind::Vfx &&
                       event.asset.value == hs::MakeAssetId(vfx_asset).value;
            });
        const auto audio = std::ranges::find_if(
            output.begin(), output.begin() + count,
            [audio_asset](const hs::PresentationEvent &event) {
                return event.kind == hs::PresentationKind::Audio &&
                       event.asset.value == hs::MakeAssetId(audio_asset).value;
            });
        Check(vfx != output.begin() + count && audio != output.begin() + count &&
                  vfx->geometry.kind ==
                      (geometry.kind == hs::DomainSignalGeometryKind::Line
                           ? hs::PresentationGeometryKind::Line
                           : hs::PresentationGeometryKind::Cone) &&
                  vfx->geometry.width == geometry.width &&
                  vfx->geometry.range == geometry.range &&
                  vfx->geometry.end_position.x == geometry.end_position.x &&
                  vfx->geometry.end_position.z == geometry.end_position.z,
              "boss telegraph keeps geometry on VFX while retaining its audio cue");
    };

    hs::DomainSignalGeometry dash{};
    dash.kind = hs::DomainSignalGeometryKind::Line;
    dash.width = 3.25f;
    dash.range = 9.5f;
    dash.start_tick = 20;
    dash.end_tick = 55;
    dash.source_id = 12;
    dash.end_position = {11.5f, 0.025f, -3.0f};
    verify(hs::DomainSignalKind::BossDashTelegraphed, dash,
           "particle.boss.dash.telegraph", "audio.boss.dash.telegraph");

    hs::DomainSignalGeometry volley{};
    volley.kind = hs::DomainSignalGeometryKind::Cone;
    volley.range = 31.0f;
    volley.half_angle_degrees = 17.5f;
    volley.start_tick = 30;
    volley.end_tick = 80;
    volley.source_id = 13;
    verify(hs::DomainSignalKind::BossVolleyTelegraphed, volley,
           "particle.boss.volley.telegraph", "audio.boss.volley.telegraph");
}

void TestArrowReleaseProjectsVfxAndAudio()
{
    hs::DomainSignal signal{};
    signal.kind = hs::DomainSignalKind::ArrowReleased;
    signal.sequence = 12;
    signal.position = {1.0f, 1.05f, 2.0f};
    signal.direction = {0.0f, 0.0f, 1.0f};
    signal.context = static_cast<std::uint8_t>(hs::SkillKind::BasicAttack);
    std::array<hs::PresentationEvent, 3> output{};
    const auto count = hs::ProjectDomainSignal(signal, output);
    const auto parameters = hs::DecodeVfxParameters(output[0].parameters);
    Check(count == 3 && output[0].kind == hs::PresentationKind::Vfx &&
              output[0].asset.value == hs::MakeAssetId("particle.player.arrow_release").value &&
              parameters.direction.z == 1.0f && output[0].position.y == 1.05f &&
              output[1].kind == hs::PresentationKind::Audio &&
              output[2].kind == hs::PresentationKind::Audio,
          "arrow release projects directional VFX without changing its two audio cues");
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
    simulation.WriteReadModel(model);
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
    simulation.WriteReadModel(model);
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

void TestVisualLinkSnapshotTransport()
{
    hs::GameReadModelStorage model;model.tick=105;
    const hs::VisualLinkView ricochet{91,hs::VisualLinkKind::Ricochet,{1,.4f,2},{7,.8f,-4},.09f,100,108};
    const hs::VisualLinkView burn{92,hs::VisualLinkKind::BurnTransfer,{-3,.4f,9},{-2,.4f,5},.12f,100,111};
    const hs::VisualLinkView relic{93,hs::VisualLinkKind::RelicChain,{4,.4f,2},{8,.4f,3},.10f,100,109};
    model.AddVisualLink(ricochet);model.AddVisualLink(burn);model.AddVisualLink(relic);
    model.AddVisualLink({94,hs::VisualLinkKind::Ricochet,{},{1,0,1},.09f,105,105});
    model.AddVisualLink({95,hs::VisualLinkKind::Ricochet,{},{1,0,1},.09f,106,114});
    hs::RenderSnapshotStorage snapshot(8, 1, 1, 16);
    Check(hs::ProjectRenderSnapshot(model.View(),DefaultContent().presentation,test_ui,hs::SettingsData{},snapshot),"link snapshot projection");
    for(const auto &link:std::array{ricochet,burn,relic})
    {
        const auto found=std::ranges::find_if(snapshot.View().persistent_vfx,[&](const auto &visual){return visual.stable_id==link.owner_id;});
        const auto expected=link.kind==hs::VisualLinkKind::Ricochet?hs::PersistentVfxKind::RicochetLink:
            link.kind==hs::VisualLinkKind::BurnTransfer?hs::PersistentVfxKind::BurnTransferLink:hs::PersistentVfxKind::RelicChainLink;
        Check(found!=snapshot.View().persistent_vfx.end() && found->kind==expected && found->position.x==link.source_position.x &&
                  found->position.y==link.source_position.y && found->position.z==link.source_position.z &&
                  found->link_target_position.x==link.target_position.x && found->link_target_position.y==link.target_position.y &&
                  found->link_target_position.z==link.target_position.z && found->link_width==link.width &&
                  found->active_tick==link.started && found->expires==link.expires,"snapshot preserves fixed link geometry and owner clocks");
    }
    Check(std::ranges::none_of(snapshot.View().persistent_vfx,[](const auto &visual){return visual.stable_id==94 || visual.stable_id==95;}),
          "expired and future links are not published");
    model.Clear();Check(model.View().visual_links.empty(),"read model Clear removes prior links");
}

void TestUpgradeActivationProjection()
{
    hs::DomainSignal signal;signal.kind=hs::DomainSignalKind::UpgradeVisual;signal.sequence=91;signal.tick=100;signal.session_id=3;
    signal.position={4,.15f,-7};signal.upgrade_skill=static_cast<std::uint8_t>(hs::SkillKind::ExplosiveArrow);
    signal.upgrade_index=1;signal.upgrade_stage=hs::UpgradeVisualStage::Telegraph;signal.upgrade_cast_id=19;signal.upgrade_owner_id=71;
    signal.geometry.kind=hs::DomainSignalGeometryKind::Circle;signal.geometry.radius=2.25f;
    signal.geometry.source_id=71;signal.geometry.start_tick=100;signal.geometry.end_tick=160;
    std::array<hs::PresentationEvent,4> events{};
    Check(hs::ProjectDomainSignal(signal,events)==1 && events[0].kind==hs::PresentationKind::Vfx &&
        events[0].upgrade_skill==static_cast<std::uint8_t>(hs::SkillKind::ExplosiveArrow)&&events[0].upgrade_index==1&&events[0].upgrade_stage==2&&
        events[0].upgrade_cast_id==19&&events[0].upgrade_owner_id==71&&events[0].geometry.end_tick==160&&events[0].geometry.radius==2.25f,
        "upgrade activation preserves stage and authoritative schedule geometry");
    signal.kind=hs::DomainSignalKind::SmallExplosion;signal.upgrade_stage=hs::UpgradeVisualStage::Resolve;
    for(auto &event:events){event.upgrade_stage=99;event.upgrade_skill=4;event.upgrade_index=1;event.upgrade_cast_id=19;event.upgrade_owner_id=71;}
    Check(hs::ProjectDomainSignal(signal,events)==2&&events[0].upgrade_stage==3&&
        events[1].kind==hs::PresentationKind::Audio&&events[1].upgrade_stage==0&&events[1].upgrade_skill==0xFF&&events[1].upgrade_owner_id==0,
        "resolve projects one annotated generic visual and unannotated existing audio");
    hs::GameReadModelStorage model;model.tick=120;model.AddMiniBomb({71,19,{4,-7},2.25f,100,160});
    model.AddMiniBomb({72,19,{1,1},2,100,120});
    hs::RenderSnapshotStorage snapshot(8,1,1,16);
    Check(hs::ProjectRenderSnapshot(model.View(),DefaultContent().presentation,test_ui,hs::SettingsData{},snapshot),"mini bomb snapshot projection");
    const auto warning=std::ranges::find_if(snapshot.View().persistent_vfx,[](const auto &v){return v.kind==hs::PersistentVfxKind::MiniBombWarning;});
    Check(warning!=snapshot.View().persistent_vfx.end()&&warning->stable_id==71&&warning->cast_id==19&&warning->position.x==4&&
        warning->position.z==-7&&warning->radius==2.25f&&warning->active_tick==100&&warning->expires==160&&warning->source_upgrade==1,
        "mini bomb owner clocks and actual footprint lost in projection");
    Check(std::ranges::count_if(snapshot.View().persistent_vfx,[](const auto &v){return v.kind==hs::PersistentVfxKind::MiniBombWarning;})==1,
        "expired mini bomb warning was published");
    model.Clear();Check(model.View().mini_bombs.empty(),"read model Clear retains mini bombs");
}

void TestCircleWarningProjection()
{
    const std::array kinds{hs::DomainSignalKind::EnemySpawnWarning, hs::DomainSignalKind::BossSpawnWarning,
        hs::DomainSignalKind::BossAreaTelegraphed, hs::DomainSignalKind::SuicideEnemyCharging};
    const std::array ids{"particle.enemy.spawn_warning", "particle.boss.spawn_warning",
        "particle.boss.area.telegraph", "particle.enemy.suicide.telegraph_radius"};
    for (std::size_t i=0; i<kinds.size(); ++i)
    {
        hs::DomainSignal signal{}; signal.kind=kinds[i]; signal.sequence=44; signal.tick=10;
        signal.geometry.kind=hs::DomainSignalGeometryKind::Circle; signal.geometry.radius=3.75f;
        signal.geometry.source_id=44; signal.geometry.start_tick=10; signal.geometry.end_tick=83;
        std::array<hs::PresentationEvent,8> events{};
        const auto count=hs::ProjectDomainSignal(signal,events);
        const auto span=std::span(events).first(count);
        const auto warning=std::ranges::find_if(span,[&](const auto &e){return e.asset.value==hs::MakeAssetId(ids[i]).value;});
        Check(warning!=span.end() && warning->geometry.radius==3.75f && warning->geometry.source_id==44 &&
            warning->geometry.start_tick==10 && warning->geometry.end_tick==83,"Circle warning projection preserves exact schedule");
        if(i==1) Check(std::ranges::none_of(span,[](const auto &e){return e.asset.value==hs::MakeAssetId("particle.boss.spawn").value;}),
            "boss warning must not emit actual spawn burst");
        if(i==3) Check(std::ranges::count_if(span,[](const auto &e){return e.asset.value==hs::MakeAssetId("particle.enemy.suicide.charge").value;})==1,
            "suicide telegraph preserves charge visual");
    }
    hs::GameReadModelStorage model; model.tick=20;
    model.AddSpawnWarning({44,{3,4},.79f,10,83,false});
    model.AddSpawnWarning({45,{-3,4},1.1f,10,83,true});
    model.AddSpawnWarning({46,{0,0},1,10,20,false});
    hs::EnemyView enemy{}; enemy.id={11}; enemy.kind=hs::EnemyKind::Suicide; enemy.attacking=true;
    enemy.attack_started=10; enemy.attack_resolve=83; enemy.warning_sequence=47; enemy.warning_extent=3.75f;
    model.AddEnemy(enemy);
    hs::EnemyView boss{}; boss.id={12}; boss.boss=hs::BossKind::FiveMinute; model.AddEnemy(boss);
    hs::BossActionView area{}; area.boss_id=12; area.kind=hs::BossActionViewKind::Area;
    area.position={6,7}; area.radius=4.25f; area.warning_sequence=48; area.warning_started=10; area.execute_tick=83;
    model.AddBossAction(area);
    hs::RenderSnapshotStorage snapshot(256,4,4,32);
    Check(hs::ProjectRenderSnapshot(model.View(),DefaultContent().presentation,test_ui,hs::SettingsData{},snapshot),"Circle warning owner snapshot");
    for(auto id:{44u,45u,47u,48u})
    {
        const auto owner=std::ranges::find_if(snapshot.View().persistent_vfx,[&](const auto &v){return v.stable_id==id;});
        Check(owner!=snapshot.View().persistent_vfx.end()&&owner->active_tick==10&&owner->expires==83,"Circle warning owner clocks lost");
    }
    Check(std::ranges::none_of(snapshot.View().persistent_vfx,[](const auto &v){return v.stable_id==46;}),"expired spawn warning published");
    Check(std::ranges::any_of(snapshot.View().instances,[](const auto &v){return v.mesh==hs::RenderMesh::Area&&v.vfx_owner_id==47;})&&
        std::ranges::any_of(snapshot.View().instances,[](const auto &v){return v.mesh==hs::RenderMesh::Area&&v.vfx_owner_id==48;}),"Circle legacy markers lack replacement owners");
    model.Clear(); snapshot.Clear();
    Check(model.View().spawn_warnings.empty(),"spawn warning storage clear");
    Check(hs::ProjectRenderSnapshot(model.View(),DefaultContent().presentation,test_ui,hs::SettingsData{},snapshot)&&snapshot.View().persistent_vfx.empty(),
        "removed warning owners linger in next snapshot");
}

void TestStatusEpisodeSnapshot()
{
 hs::GameReadModelStorage model;model.tick=20;model.session_id=7;hs::EnemyView enemy;enemy.id={12};enemy.position={3,4};enemy.health=30;enemy.max_health=100;enemy.footprint_radius=.81f;enemy.status_episodes[0]={10,40,1};enemy.status_episodes[3]={10,21,2};model.AddEnemy(enemy);
 hs::RenderSnapshotStorage snapshot(256,4,4,32);
 const auto project=[&](){snapshot.Clear();Check(hs::ProjectRenderSnapshot(model.View(),DefaultContent().presentation,test_ui,hs::SettingsData{},snapshot),"status snapshot projection");};
 project();auto owners=snapshot.View().persistent_vfx;Check(owners.size()==2&&owners[0].radius==.81f&&owners[0].entity_health_fraction==.3f,"status actual footprint/health lost");const auto identity=owners[0].stable_id;
 model.tick=21;project();Check(snapshot.View().persistent_vfx.size()==1&&snapshot.View().persistent_vfx[0].stable_id==identity,"mark inclusive expiry lost or bleed identity changed");
 model.session_id=8;project();Check(snapshot.View().persistent_vfx[0].stable_id!=identity,"new session reused status owner");model.tick=40;project();Check(snapshot.View().persistent_vfx.empty(),"expired status owner retained");
}

void TestPickupCollectionEndpointProjection()
{
    struct PickupCase
    {
        hs::DomainSignalKind kind;
        std::string_view asset;
        std::string_view audio;
    };
    constexpr std::array cases{
        PickupCase{hs::DomainSignalKind::ExperienceCollected,
                   "particle.pickup.xp_collect", "audio.pickup.xp_collect"},
        PickupCase{hs::DomainSignalKind::HealCollected,
                   "particle.pickup.heal_collect", "audio.pickup.heal"},
        PickupCase{hs::DomainSignalKind::MagnetCollected,
                   "particle.pickup.magnet_collect", "audio.pickup.magnet"},
        PickupCase{hs::DomainSignalKind::RelicCollected,
                   "particle.pickup.relic_collect", "audio.pickup.relic_chest"}};
    for (const auto &pickup : cases)
    {
        hs::DomainSignal signal{};
        signal.kind = pickup.kind;
        signal.sequence = 73;
        signal.position = {4.0f, 0.3f, -2.0f};
        signal.target = {-1.0f, 0.3f, 6.0f};
        signal.flags = static_cast<std::uint8_t>(hs::DomainSignalFlag::HasTarget);
        std::array<hs::PresentationEvent, 2> output{};
        const auto count = hs::ProjectDomainSignal(signal, output);
        const auto parameters = hs::DecodeVfxParameters(output[0].parameters);
        Check(count == 2 && output[0].kind == hs::PresentationKind::Vfx &&
                  output[0].asset.value == hs::MakeAssetId(pickup.asset).value &&
                  output[0].position.x == signal.position.x &&
                  output[0].position.y == signal.position.y &&
                  output[0].position.z == signal.position.z &&
                  parameters.flags == static_cast<std::uint32_t>(hs::VfxEventFlag::HasTarget) &&
                  parameters.target.x == signal.target.x &&
                  parameters.target.y == signal.target.y &&
                  parameters.target.z == signal.target.z &&
                  output[1].kind == hs::PresentationKind::Audio &&
                  output[1].asset.value == hs::MakeAssetId(pickup.audio).value,
              "pickup projection preserves source, player endpoint, and existing audio");
    }
}

void TestPickupIdleAttachmentOwners()
{
    struct PickupCase
    {
        hs::PickupKind kind;
        hs::PersistentVfxKind visual_kind;
        std::string_view asset;
    };
    constexpr std::array cases{
        PickupCase{hs::PickupKind::Experience, hs::PersistentVfxKind::PickupXpIdle,
                   "particle.pickup.xp.idle"},
        PickupCase{hs::PickupKind::Heal, hs::PersistentVfxKind::PickupHealIdle,
                   "particle.pickup.heal.idle"},
        PickupCase{hs::PickupKind::Magnet, hs::PersistentVfxKind::PickupMagnetIdle,
                   "particle.pickup.magnet.idle"},
        PickupCase{hs::PickupKind::RelicChest, hs::PersistentVfxKind::PickupRelicIdle,
                   "particle.pickup.relic_chest.idle"}};
    hs::GameReadModelStorage model;
    model.tick = 40;
    model.session_id = 7;
    hs::RenderSnapshotStorage snapshot(32, 2, 2, 8);
    const auto project = [&] {
        snapshot.Clear();
        Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
            test_ui, hs::SettingsData{}, snapshot), "pickup idle snapshot projection");
    };
    const auto find_owner = [&](hs::PersistentVfxKind kind) -> const hs::PersistentVfxVisual * {
        const auto owners = snapshot.View().persistent_vfx;
        const auto found = std::ranges::find_if(owners,
            [kind](const auto &visual) { return visual.kind == kind; });
        return found == owners.end() ? nullptr : &*found;
    };
    for (std::size_t i = 0; i < cases.size(); ++i)
        model.AddPickup({{100 + i}, cases[i].kind,
            {static_cast<float>(i), static_cast<float>(i + 2)}, false,
            static_cast<hs::Tick>(10 + i)});
    project();
    std::array<std::uint64_t, cases.size()> identities{};
    for (std::size_t i = 0; i < cases.size(); ++i)
    {
        const auto *owner = find_owner(cases[i].visual_kind);
        const auto instances = snapshot.View().instances;
        const auto render_id = (5ull << 60) | (100 + i);
        const auto instance = std::ranges::find_if(instances,
            [render_id](const auto &candidate) {
                return candidate.mesh == hs::RenderMesh::Pickup &&
                       candidate.stable_id == render_id;
            });
        Check(owner && instance != instances.end() &&
            owner->effect_asset.value == hs::MakeAssetId(cases[i].asset).value &&
            owner->entity_render_id == instance->stable_id &&
            owner->position.x == instance->position.x &&
            owner->position.y == instance->position.y &&
            owner->position.z == instance->position.z &&
            owner->radius == std::max(instance->scale.x, instance->scale.z) &&
            owner->active_tick == 10 + i && owner->expires == 0 &&
            owner->stable_id != 0, "pickup idle owner loses actor geometry, asset, or spawn clock");
        Check(std::ranges::count_if(snapshot.View().persistent_vfx,
            [kind = cases[i].visual_kind](const auto &visual) {
                return visual.kind == kind;
            }) == 1, "live pickup projects exactly one idle owner");
        identities[i] = owner->stable_id;
    }
    model.Clear();
    model.tick = 41;
    for (std::size_t i = 0; i < cases.size(); ++i)
        model.AddPickup({{100 + i}, cases[i].kind,
            {static_cast<float>(i + 6), static_cast<float>(i + 8)}, false,
            static_cast<hs::Tick>(10 + i)});
    project();
    for (std::size_t i = 0; i < cases.size(); ++i)
    {
        const auto *owner = find_owner(cases[i].visual_kind);
        Check(owner && owner->stable_id == identities[i] &&
            owner->active_tick == 10 + i &&
            owner->position.x == static_cast<float>(i + 6) &&
            owner->position.z == static_cast<float>(i + 8),
            "pickup idle owner must follow its actor without restarting");
    }
    model.Clear();
    for (std::size_t i = 1; i < cases.size(); ++i)
        model.AddPickup({{100 + i}, cases[i].kind,
            {static_cast<float>(i + 6), static_cast<float>(i + 8)}, false,
            static_cast<hs::Tick>(10 + i)});
    project();
    Check(!find_owner(hs::PersistentVfxKind::PickupXpIdle),
        "collected pickup retained its idle owner");
    model.session_id = 8;
    project();
    const auto *next_session_heal = find_owner(hs::PersistentVfxKind::PickupHealIdle);
    Check(next_session_heal && next_session_heal->stable_id != identities[1],
        "new session reused a pickup owner identity");
}

void TestPlayerAttachmentOwnerWindows()
{
    hs::GameReadModelStorage model;
    hs::RenderSnapshotStorage snapshot(30000, 2048, 8, 2048);
    const auto project = [&] {
        snapshot.Clear();
        Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
            test_ui, hs::SettingsData{}, snapshot), "player attachment snapshot");
    };
    const auto find = [&](hs::PersistentVfxKind kind) -> const hs::PersistentVfxVisual * {
        const auto owners = snapshot.View().persistent_vfx;
        const auto found = std::ranges::find_if(owners,
            [kind](const auto &visual) { return visual.kind == kind; });
        return found == owners.end() ? nullptr : &*found;
    };

    model.tick = 10;
    model.player.position = {2.0f, 3.0f};
    model.player.basic_attack_animation_start = 10;
    model.player.basic_attack_release_tick = 14;
    model.player.basic_attack_cast_id = 77;
    project();
    const auto *basic = find(hs::PersistentVfxKind::PlayerBowDraw);
    Check(basic && basic->effect_asset.value == hs::MakeAssetId("particle.player.bow_draw").value &&
        basic->active_tick == 10 && basic->expires == 14 &&
        basic->source_horizon_tick == 14 && basic->entity_render_id == (1ull << 60) &&
        basic->radius > 0 && basic->position.x == 2.0f && basic->position.z == 3.0f,
        "basic draw lost actual release or player attachment");
    const auto basic_id = basic->stable_id;
    model.tick = 13; model.player.position = {4.0f, 5.0f}; project();
    basic = find(hs::PersistentVfxKind::PlayerBowDraw);
    Check(basic && basic->stable_id == basic_id && basic->position.x == 4.0f,
        "basic draw owner changed while moving");
    model.tick = 14; project();
    Check(!find(hs::PersistentVfxKind::PlayerBowDraw),
        "basic draw persisted through release tick");

    model.player.charging = true;
    model.player.charging_skill = hs::SkillKind::ChargedShot;
    model.player.charge_start = 20;
    model.player.charge_full_ready_tick = 59;
    model.tick = 58; project();
    const auto *charge = find(hs::PersistentVfxKind::PlayerBowDraw);
    Check(charge && charge->active_tick == 20 && charge->expires == 0 &&
        charge->source_horizon_tick == 59 &&
        charge->stable_id != basic_id && !find(hs::PersistentVfxKind::ChargedFullReady),
        "live charged draw needs its own owner and no forecast deadline");
    model.tick = 59; project();
    const auto *ready = find(hs::PersistentVfxKind::ChargedFullReady);
    Check(ready && ready->effect_asset.value ==
        hs::MakeAssetId("particle.upgrade.charged.full_ready").value &&
        ready->active_tick == 59 && ready->expires == 92 &&
        ready->stable_id != find(hs::PersistentVfxKind::PlayerBowDraw)->stable_id &&
        ready->entity_render_id == (1ull << 60) && ready->radius > 0,
        "full charge ready lost actual threshold or fixed window");
    model.tick = 91; project();
    Check(find(hs::PersistentVfxKind::ChargedFullReady),
        "full charge ready ended before its last live tick");
    model.tick = 92; project();
    Check(!find(hs::PersistentVfxKind::ChargedFullReady),
        "full charge ready exceeded 0.55 seconds");
    model.player.charging = false; model.tick = 60; project();
    Check(!find(hs::PersistentVfxKind::PlayerBowDraw) &&
        !find(hs::PersistentVfxKind::ChargedFullReady),
        "released charge retained player effects");

    model.player.charging = true;
    model.player.charge_start = 20;
    model.player.upgrades[static_cast<std::size_t>(hs::SkillKind::ChargedShot)] = 1;
    model.player.charge_full_ready_tick = 104;
    model.charge_normal_ready_tick = 59;
    model.charge_ratio = .8f;
    model.charge_radius = .72f;
    model.tick = 60; project();
    const auto *overcharge = find(hs::PersistentVfxKind::ChargedOverchargeLoop);
    Check(overcharge && overcharge->active_tick == 20 &&
        overcharge->expires == 104 && overcharge->radius == .72f &&
        overcharge->charge_ratio == .8f && overcharge->source_upgrade == 0,
        "charged ordinal1 overcharge owner lost gameplay threshold, footprint, or ratio");
    model.tick = 104; project();
    Check(!find(hs::PersistentVfxKind::ChargedOverchargeLoop),
        "charged ordinal1 overcharge owner survived its gameplay end");
    model.player.charging = false;

    model.player.active_basic_empower_started = 100;
    model.player.active_basic_empower_until = 103;
    model.tick = 102; project();
    Check(!find(hs::PersistentVfxKind::EmpoweredReady),
        "empower effect appeared without basic upgrade eight");
    model.player.upgrades[static_cast<std::size_t>(hs::SkillKind::BasicAttack)] = 0x80;
    model.tick = 103; project();
    const auto *empowered = find(hs::PersistentVfxKind::EmpoweredReady);
    Check(empowered && empowered->effect_asset.value ==
        hs::MakeAssetId("particle.upgrade.empowered_ready").value &&
        empowered->active_tick == 100 && empowered->expires == 104 &&
        empowered->entity_render_id == (1ull << 60) && empowered->radius > 0,
        "empower activation should include its final gameplay tick");
    const auto empower_id = empowered->stable_id;
    model.tick = 104; project();
    Check(!find(hs::PersistentVfxKind::EmpoweredReady),
        "expired empower activation retained its owner");
    model.player.active_basic_empower_started = 120;
    model.player.active_basic_empower_until = 123;
    model.tick = 120; project();
    empowered = find(hs::PersistentVfxKind::EmpoweredReady);
    Check(empowered && empowered->stable_id != empower_id,
        "a new empower activation reused its previous owner");
    const auto next_activation_id = empowered->stable_id;
    model.session_id = 2; project();
    empowered = find(hs::PersistentVfxKind::EmpoweredReady);
    Check(empowered && empowered->stable_id != next_activation_id,
        "a new session reused the previous player owner");
}

void RunGameplayPresentationTests()
{
    TestPlayerAttachmentOwnerWindows();
    TestStatusEpisodeSnapshot();
    TestCircleWarningProjection();
    TestUpgradeActivationProjection();
    TestVisualLinkSnapshotTransport();
    TestCursorMovement();
    TestUiHitRegionsMatchAnchors();
    TestCharacterInformationPage();
    TestPauseMenuActions();
    TestSkillUpgradeCardsHaveDescriptions();
    TestBossAnimationReachesReleaseFrame();
    TestMonsterVisualScalesAndAttackFacing();
    TestRelicTriggerProjection();
    TestProjectileProjectionCarriesAuthoritativeOwner();
    TestProjectileUpgradeAttachmentEpisodes();
    TestEnemyAnimationStateSignals();
    TestSlimeAttackPoseTransitions();
    TestAuthoredEnvironmentProjection();
    TestArrowReleaseProjectsVfxAndAudio();
    TestBossTelegraphProjectsGeometryAndAudio();
    TestDenseGrassProjection();
    TestPresentationGeometryTransport();
    TestProjectileImpactGeometryTransport();
    TestPickupCollectionEndpointProjection();
    TestPickupIdleAttachmentOwners();
    TestSessionIdentityAcrossPresentationReset();
}

} // namespace gameplay_test
