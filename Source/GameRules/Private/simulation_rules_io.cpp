#include <hs/game_rules/simulation_rules_io.hpp>
#include <hs/game_rules/detail/simulation_rule_fields.hpp>
#include <hs/core/canonical_bytes.hpp>
#include <hs/core/canonical_reader.hpp>
#include <hs/core/cooked_format.hpp>

#include <array>
#include <cmath>
#include <stdexcept>
#include <type_traits>

namespace hs
{
namespace
{
template <typename T> struct Array : std::false_type {};
template <typename T, std::size_t N> struct Array<std::array<T, N>> : std::true_type {};

template <typename T> void WriteField(CanonicalBytes &out, const T &value)
{
    if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) out.Scalar(value);
    else if constexpr (Array<T>::value)
    {
        out.Scalar(static_cast<std::uint64_t>(value.size()));
        for (const auto &element : value) WriteField(out, element);
    }
    else detail::VisitSimulationRuleFields(value, [&](const char *, const auto &member) {
        WriteField(out, member);
    });
}

template <typename T> void ReadField(CanonicalReader &in, T &value)
{
    if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
    {
        value = in.Scalar<T>();
        if constexpr (std::is_floating_point_v<T>)
            if (!std::isfinite(value)) throw std::runtime_error("Non-finite simulation field.");
        if constexpr (std::is_same_v<T, AbilityHandlerId>)
        { if (value > AbilityHandlerId::ForcedRetreatAndProjectile) throw std::runtime_error("Invalid ability handler."); }
        else if constexpr (std::is_same_v<T, EnemyScalingMachine>)
        { if (value != EnemyScalingMachine::CompletedMinutes) throw std::runtime_error("Invalid scaling machine."); }
        else if constexpr (std::is_same_v<T, BossPatternLogic>)
        { if (value > BossPatternLogic::DoubleOffsetFanProjectiles) throw std::runtime_error("Invalid boss pattern."); }
        else if constexpr (std::is_same_v<T, SpawnFallbackLocation>)
        { if (value != SpawnFallbackLocation::FarthestArenaEdge) throw std::runtime_error("Invalid spawn fallback."); }
        else if constexpr (std::is_same_v<T, ArenaObstacleKind>)
        { if (value > ArenaObstacleKind::Rock) throw std::runtime_error("Invalid arena obstacle."); }
        else if constexpr (std::is_same_v<T, SkillKind>)
        { if (value > SkillKind::Count) throw std::runtime_error("Invalid skill kind."); }
        else if constexpr (std::is_same_v<T, RelicKind>)
        { if (value > RelicKind::Count) throw std::runtime_error("Invalid relic kind."); }
    }
    else if constexpr (Array<T>::value)
    {
        if (in.Count() != value.size()) throw std::runtime_error("Simulation array length mismatch.");
        for (auto &element : value) ReadField(in, element);
    }
    else detail::VisitSimulationRuleFields(value, [&](const char *, auto &member) {
        ReadField(in, member);
    });
}
}

std::vector<std::byte> EncodeSimulationRules(const SimulationRules &rules)
{
    CanonicalBytes out("project_hs.simulation_rules", 1);
    WriteField(out, rules);
    return std::move(out).Take();
}

Result DecodeSimulationRules(std::span<const std::byte> bytes, SimulationRules &rules)
{
    try
    {
        CanonicalReader in(bytes, "project_hs.simulation_rules", 1);
        SimulationRules decoded{};
        ReadField(in, decoded);
        in.Finish();
        if (!SimulationRulesHaveSafeProgression(decoded))
            throw std::runtime_error("Invalid progression or projectile array bounds.");
        if (decoded.version != 7 || decoded.arena_boundary.count < 3 ||
            decoded.arena_boundary.count > decoded.arena_boundary.points.size() ||
            decoded.arena_obstacle_count > decoded.arena_obstacles.size() ||
            decoded.character_initial.starting_active_skill_count > decoded.character_initial.starting_active_skill_ids.size() ||
            decoded.character_initial.starting_relic_count > decoded.character_initial.starting_relic_ids.size())
            throw std::runtime_error("Invalid simulation version or arena counts.");
        for (const auto &boss : decoded.bosses)
            if (boss.pattern_count > boss.patterns.size())
                throw std::runtime_error("Invalid boss pattern count.");
        for (std::size_t i = 0; i < decoded.character_initial.starting_active_skill_count; ++i)
            if (decoded.character_initial.starting_active_skill_ids[i] == SkillKind::Count)
                throw std::runtime_error("Invalid active skill kind.");
        for (std::size_t i = 0; i < decoded.character_initial.starting_relic_count; ++i)
            if (decoded.character_initial.starting_relic_ids[i] == RelicKind::Count)
                throw std::runtime_error("Invalid active relic kind.");
        rules = decoded;
        return Result::Success();
    }
    catch (const std::runtime_error &error)
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay", error.what());
    }
}

Result LoadSimulationRules(const std::filesystem::path &path, SimulationRules &rules,
                           std::uint64_t *content_hash)
{
    CookedHeader header;
    std::vector<std::byte> payload;
    if (auto result = ReadCookedPayload(path, SimulationRulesSchemaHash(), header, payload); !result)
        return result;
    if (auto result = DecodeSimulationRules(payload, rules); !result) return result;
    if (content_hash) *content_hash = header.source_hash;
    return Result::Success();
}
} // namespace hs
