#include <hs/game_rules/simulation_rules_io.hpp>
#include <hs/core/cooked_format.hpp>
#include <cstring>
#include <format>
#include <type_traits>

namespace hs
{
Result LoadSimulationRules(const std::filesystem::path &path,
                           SimulationRules &rules,
                           std::uint64_t *content_hash)
{
    static_assert(std::is_trivially_copyable_v<SimulationRules>);
    static_assert(std::is_standard_layout_v<SimulationRules>);
    CookedHeader header;
    std::vector<std::byte> payload;
    if (auto result = ReadCookedPayload(path, SimulationRulesSchemaHash(), header, payload); !result)
    {
        return result;
    }
    if (payload.size() != sizeof(SimulationRules))
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay",
                               std::format("Cooked game data table size is {}, expected {}.",
                                           payload.size(), sizeof(SimulationRules)));
    }
    std::memcpy(&rules, payload.data(), sizeof(rules));
    if (rules.version != 7)
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay",
                               "Cooked game data version is unsupported.");
    }
    if (content_hash)
    {
        *content_hash = header.source_hash;
    }
    return Result::Success();
}

} // namespace hs
