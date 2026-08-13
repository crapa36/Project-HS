#pragma once

#include "active_rules.hpp"

#include <utility>

namespace hs::gameplay_detail
{

template <RuleHandlerId Handler, typename Callback>
void DispatchRule(const ActiveRuleTable &table, RuleHook hook, Callback &&callback)
{
    for (const auto &rule : table.For(hook))
        if (rule.handler == Handler)
            std::forward<Callback>(callback)(rule);
}

} // namespace hs::gameplay_detail
