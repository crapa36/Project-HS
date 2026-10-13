#include "../Source/Runtime/Private/ui_command_admission.hpp"
#include <iostream>
#include <stdexcept>
void Check(bool ok) { if (!ok) throw std::runtime_error("UI admission contract"); }
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
    std::cout<<"PASS saturated gameplay/runtime queues, navigation and fresh input\n";
}
