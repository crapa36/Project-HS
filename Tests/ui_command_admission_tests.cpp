#include "../Source/Runtime/Private/ui_command_admission.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
void Check(bool ok, const char *message = "UI admission contract") { if (!ok) throw std::runtime_error(message); }
int main()
{
    hs::BoundedSpscQueue<hs::UiAction,64> actions;
    hs::BoundedSpscQueue<hs::UiCommand,64> commands;
    std::uint64_t rejected=0;
    const hs::PresentationUiState previous{};
    for(int i=0;i<64;++i)
    {
        Check(actions.TryPush({hs::UiActionKind::Reroll}));
        Check(commands.TryPush({hs::UiCommandKind::CancelSkillRebind}));
    }
    // Production resolver's settings Back click mutates the page and emits
    // CancelSkillRebind. A full command queue must preserve the prior page.
    hs::PresentationUiState settings_previous;
    settings_previous.page=hs::UiPage::MainMenuSettings;
    auto settings_candidate=settings_previous;
    hs::SessionProbe session; session.phase=hs::SessionPhase::MainMenu;
    hs::SettingsData settings;
    auto back=hs::ResolveUiInteraction(session,settings_candidate,settings,
        {300.0f/960.0f-1.0f,1.0f-920.0f/540.0f});
    Check(back.runtime_command && settings_candidate.page==hs::UiPage::Root);
    Check(!hs::runtime_detail::AdmitUiInteraction(back,settings_candidate,settings_previous,
        actions,commands,rejected));
    Check(settings_candidate.page==hs::UiPage::MainMenuSettings && back.invalid);
    for(bool runtime : {false,true})
    {
        auto candidate=previous; candidate.page=hs::UiPage::Collection;
        hs::UiInteraction interaction;
        if(runtime) interaction.runtime_command=hs::UiCommand{hs::UiCommandKind::CancelSkillRebind};
        else interaction.gameplay_action=hs::UiAction{hs::UiActionKind::SelectCard};
        Check(!hs::runtime_detail::AdmitUiInteraction(interaction,candidate,previous,actions,commands,rejected));
        Check(candidate.page==previous.page && interaction.invalid);
        Check(!interaction.gameplay_action && !interaction.runtime_command); // no success cue
        Check(actions.Size()==64 && commands.Size()==64);
    }
    auto candidate=previous; candidate.page=hs::UiPage::Collection;
    hs::UiInteraction navigation;
    Check(hs::runtime_detail::AdmitUiInteraction(navigation,candidate,previous,actions,commands,rejected));
    Check(candidate.page==hs::UiPage::Collection && rejected==3);
    hs::UiAction action; hs::UiCommand command;
    for(int i=0;i<64;++i) { Check(actions.TryPop(action)); Check(commands.TryPop(command)); }
    hs::UiInteraction next{hs::UiAction{hs::UiActionKind::SelectCard},std::nullopt};
    Check(hs::runtime_detail::AdmitUiInteraction(next,candidate,previous,actions,commands,rejected));
    Check(next.gameplay_action && !next.invalid);
    Check(actions.TryPop(action) && action.kind==hs::UiActionKind::SelectCard);
    Check(!actions.TryPop(action));
    hs::UiInteraction setting{std::nullopt,hs::UiCommand{hs::UiCommandKind::CancelSkillRebind}};
    Check(hs::runtime_detail::AdmitUiInteraction(setting,candidate,previous,actions,commands,rejected));
    Check(commands.TryPop(command) && !commands.TryPop(command));

    hs::UiFrame displayed;
    displayed.generation=91; displayed.session_id=7;
    displayed.session.phase=hs::SessionPhase::CardSelection;
    displayed.session.card_count=2;
    displayed.session.cards[0]={hs::CardKind::LearnSkill,1,0};
    displayed.session.cards[1]={hs::CardKind::SkillUpgrade,2,3};
    displayed.selection_epoch=hs::runtime_detail::UiSelectionEpoch(displayed.session);
    std::array<hs::UiModel, 2> layout{};
    layout[0].kind=hs::UiModel::Kind::Button; layout[0].anchor_pixels={360,300}; layout[0].size_pixels={360,420};
    layout[1].kind=hs::UiModel::Kind::Text; layout[1].anchor_pixels={400,350}; layout[1].size_pixels={100,30};
    const auto card_cursor=hs::ReferenceUiCursor({500,400},{1920,1080});
    Check(hs::runtime_detail::PointHitsUiDrawButton(layout,card_cursor),
          "hit testing uses the accepted frame's button rectangle");
    Check(hs::runtime_detail::UiFrameExtentMatches({1920,1080},{1920,1080}) &&
          !hs::runtime_detail::UiFrameExtentMatches({1920,1080},{1280,720}),
          "resize rejects clicks until the new extent has an accepted UI frame");
    hs::runtime_detail::UiActionEnvelope envelope;
    envelope.action={hs::UiActionKind::SelectCard,0,0}; envelope.packet_generation=displayed.generation;
    envelope.session_id=displayed.session_id; envelope.selection_epoch=displayed.selection_epoch;
    envelope.displayed_session=displayed.session;
    Check(hs::runtime_detail::UiActionCurrent(envelope,7,displayed.session),
          "accepted card frame action retains its subject and session identity");
    auto rerolled=displayed.session; rerolled.cards[0].subject=4;
    Check(!hs::runtime_detail::UiActionCurrent(envelope,7,rerolled) &&
          !hs::runtime_detail::UiActionCurrent(envelope,8,displayed.session),
          "reroll and session changes reject the old displayed card ordinal");
    hs::BoundedSpscQueue<hs::runtime_detail::UiActionEnvelope,64> presented_actions;
    hs::BoundedSpscQueue<hs::UiCommand,64> presented_commands;
    for(int i=0;i<64;++i) Check(presented_actions.TryPush({}),"fill presented-action queue");
    auto presented_candidate=previous; presented_candidate.page=hs::UiPage::Collection;
    hs::UiInteraction presented_action{hs::UiAction{hs::UiActionKind::SelectCard},std::nullopt};
    Check(!hs::runtime_detail::AdmitPresentedUiInteraction(presented_action,presented_candidate,previous,
        displayed,presented_actions,presented_commands,rejected) &&
        presented_candidate.page==previous.page && !presented_action.gameplay_action,
        "full action queue preserves the current UI intent and emits no stale action");
    std::cout<<"PASS saturated gameplay/runtime queues, navigation and fresh input\n";
}
