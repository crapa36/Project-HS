#pragma once

#include <hs/core/canonical_bytes.hpp>
#include <hs/game_rules/detail/simulation_rule_fields.hpp>

#include <array>
#include <type_traits>

namespace hs::content_identity
{
template <typename T> struct SimulationArray : std::false_type {};
template <typename T, std::size_t N>
struct SimulationArray<std::array<T, N>> : std::true_type {};

template <typename T> void SimulationValue(CanonicalBytes &bytes, const T &value)
{
    if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
        bytes.Scalar(value);
    else if constexpr (SimulationArray<T>::value)
        bytes.Sequence(value, [](CanonicalBytes &out, const auto &member) { SimulationValue(out, member); });
    else
        detail::VisitSimulationRuleFields(value, [&](const char *, const auto &member) {
            SimulationValue(bytes, member);
        });
}

inline std::vector<std::byte> Simulation(const SimulationRules &rules)
{
    CanonicalBytes bytes("project-hs-simulation-values", 1);
    bytes.Scalar(SimulationRulesSchemaHash());
    SimulationValue(bytes, rules);
    return std::move(bytes).Take();
}
}
