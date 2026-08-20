#pragma once

#include "active_rules.hpp"

#include <utility>

namespace hs::gameplay_detail
{

template <typename Callback>
void DispatchRules(const ActiveRuleTable &table, RuleHook hook, Callback &&callback)
{
    for (const auto &rule : table.For(hook))
        std::forward<Callback>(callback)(rule);
}

} // namespace hs::gameplay_detail
