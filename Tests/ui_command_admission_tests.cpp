#include "../Source/Runtime/Private/ui_command_admission.hpp"
#include <iostream>
#include <stdexcept>
void Check(bool ok, const char *message = "UI admission contract") { if (!ok) throw std::runtime_error(message); }
int main()
{
    hs::UiFrame displayed;
    displayed.generation=91; displayed.session_id=7;
    displayed.session.phase=hs::SessionPhase::CardSelection;
    displayed.session.card_count=2;
    displayed.session.cards[0]={hs::CardKind::LearnSkill,1,0};
    displayed.session.cards[1]={hs::CardKind::SkillUpgrade,2,3};

    hs::BoundedSpscQueue<hs::runtime_detail::UiActionEnvelope,64> actions;
    hs::BoundedSpscQueue<hs::UiCommand,64> commands;
    std::uint64_t rejected=0;
    const hs::PresentationUiState previous{};
    hs::runtime_detail::UiActionEnvelope envelope;
    hs::UiCommand command;
    auto candidate=previous;
    while(commands.TryPush({hs::UiCommandKind::CancelSkillRebind})) {}
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
    Check(!hs::runtime_detail::AdmitPresentedUiInteraction(back,settings_candidate,settings_previous,
        displayed,actions,commands,rejected));
    Check(settings_candidate.page==hs::UiPage::MainMenuSettings && back.invalid && rejected==1);

    while(commands.TryPop(command)) {}

    candidate=previous;
    hs::UiInteraction next{hs::UiAction{hs::UiActionKind::SelectCard},std::nullopt};
    Check(hs::runtime_detail::AdmitPresentedUiInteraction(next,candidate,previous,
        displayed,actions,commands,rejected));
    Check(next.gameplay_action && !next.invalid);
    Check(actions.TryPop(envelope) && envelope.action.kind==hs::UiActionKind::SelectCard &&
          envelope.packet_generation==91 && envelope.session_id==7 &&
          envelope.displayed_session.phase==hs::SessionPhase::CardSelection &&
          envelope.displayed_session.card_count==2 &&
          envelope.displayed_session.cards[0].kind==hs::CardKind::LearnSkill &&
          envelope.displayed_session.cards[0].subject==1 &&
          envelope.displayed_session.cards[1].kind==hs::CardKind::SkillUpgrade &&
          envelope.displayed_session.cards[1].subject==2 && envelope.displayed_session.cards[1].upgrade==3,
          "gameplay action carries the exact displayed frame identity and subject");
    Check(actions.Size()==0);
    Check(hs::runtime_detail::UiActionCurrent(envelope,7,displayed.session),
          "accepted card frame action retains its subject and session identity");
    auto rerolled=displayed.session; rerolled.cards[0].subject=4;
    Check(!hs::runtime_detail::UiActionCurrent(envelope,7,rerolled) &&
          !hs::runtime_detail::UiActionCurrent(envelope,8,displayed.session),
          "reroll and session changes reject the old displayed card ordinal");

    hs::UiInteraction setting{std::nullopt,hs::UiCommand{hs::UiCommandKind::CancelSkillRebind}};
    Check(hs::runtime_detail::AdmitPresentedUiInteraction(setting,candidate,previous,
        displayed,actions,commands,rejected));
    Check(commands.TryPop(command) && command.kind==hs::UiCommandKind::CancelSkillRebind && !commands.TryPop(command));

    std::cout<<"PASS UI admission rollback and displayed-action identity\n";
}
