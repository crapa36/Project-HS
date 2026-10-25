#include "release_simulation_codegen.hpp"

#include <hs/game_rules/detail/simulation_rule_fields.hpp>
#include <bit>
#include <type_traits>

namespace hs::content
{
namespace
{
template <typename T>
struct IsStdArray : std::false_type {};
template <typename T, std::size_t N>
struct IsStdArray<std::array<T, N>> : std::true_type {};

template <typename T>
void EmitMember(const T &value, const std::string &path, std::string &source)
{
    if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
    {
        static_assert(!std::is_floating_point_v<T> || std::is_same_v<T, float>,
                      "Emit new floating-point types by their exact bit representation");
        source += "    " + path + " = ";
        if constexpr (std::is_same_v<T, float>)
            source += "std::bit_cast<float>(std::uint32_t{" +
                std::to_string(std::bit_cast<std::uint32_t>(value)) + "u})";
        else if constexpr (std::is_same_v<T, bool>)
            source += value ? "true" : "false";
        else
        {
            // Array subscripting yields a reference; strip it before the typed cast.
            source += "static_cast<std::remove_cvref_t<decltype(" + path + ")>>(";
            if constexpr (std::is_enum_v<T>)
                source += std::to_string(static_cast<std::underlying_type_t<T>>(value));
            else if constexpr (std::is_signed_v<T>)
                source += std::to_string(value) + "ll";
            else
                source += std::to_string(value) + "ull";
            source += ")";
        }
        source += ";\n";
    }
    else if constexpr (IsStdArray<T>::value)
    {
        for (std::size_t index = 0; index < value.size(); ++index)
            EmitMember(value[index], path + "[" + std::to_string(index) + "]", source);
    }
    else
    {
        detail::VisitSimulationRuleFields(value, [&](const char *name, const auto &member) {
            EmitMember(member, path + "." + name, source);
        });
    }
}
}

std::string EmitReleaseSimulationCpp(const SimulationRules &rules)
{
    std::string source =
        "#include <hs/game_rules/simulation_rules.hpp>\n"
        "#include <bit>\n#include <cstdint>\n#include <type_traits>\n"
        "namespace hs::shipping\n{\nnamespace\n{\n"
        "constinit const SimulationRules kSimulation = []() constexpr {\n"
        "    SimulationRules rules{};\n";
    EmitMember(rules, "rules", source);
    source += "    return rules;\n}();\n}\n"
        "const SimulationRules &Simulation() noexcept { return kSimulation; }\n}\n";
    return source;
}
}
