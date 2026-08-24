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
    Debug(simulation, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::BasicAttack), 0);
    Debug(simulation, hs::DebugCommandKind::GrantRelic,
          static_cast<std::uint64_t>(hs::RelicKind::BleedKillHeal));
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
              contains_text(snapshot.View(), "출혈 중인 적을 처치하면"),
          "relic page shows acquired relic effects");

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
    for (const auto &card : probe.cards)
    {
        if (card.kind != hs::CardKind::BasicUpgrade &&
            card.kind != hs::CardKind::SkillUpgrade)
            continue;
        checked_upgrade = true;
        const auto heading = std::format("강화 {} · ",
                                         static_cast<unsigned>(card.upgrade) + 1);
        const auto has_description = std::ranges::any_of(
            snapshot.View().ui, [&](const hs::UiModel &model) {
                const auto end = std::ranges::find(model.utf8_text, '\0');
                const std::string_view text(model.utf8_text.data(),
                                            end - model.utf8_text.begin());
                const auto heading_position = text.find(heading);
                return heading_position != std::string_view::npos &&
                       text.find('#', heading_position) == std::string_view::npos &&
                       std::ranges::count(text, '\n') >= 1 &&
                       heading_position + heading.size() + 30 < text.size();
            });
        Check(has_description, "skill upgrade card includes readable effect description");
    }
    Check(checked_upgrade, "upgrade description test has an upgrade card");
    Check(simulation.Shutdown().Succeeded(), "upgrade description shutdown");
}

void RunGameplayPresentationTests()
{
    TestCursorMovement();
    TestUiHitRegionsMatchAnchors();
    TestCharacterInformationPage();
    TestPauseMenuActions();
    TestSkillUpgradeCardsHaveDescriptions();
}

} // namespace gameplay_test
