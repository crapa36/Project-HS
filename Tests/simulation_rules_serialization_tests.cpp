#include <hs/game_rules/simulation_rules.hpp>
#include <hs/game_rules/simulation_rules_io.hpp>

#include <cstddef>
#include <cstring>
#include <iostream>
#include <type_traits>

namespace
{

static_assert(std::is_trivially_copyable_v<hs::SimulationRules>);
static_assert(offsetof(hs::SimulationRules, arena_boundary) == 8);
static_assert(offsetof(hs::SimulationRules, arena_obstacle_count) == 716);

} // namespace

int main()
{
    auto first = hs::SimulationRules::Defaults();
    hs::SimulationRules second{};
    std::memcpy(&second, &first, sizeof(first));

    // Bytes 10 and 717 are alignment gaps inside the cooked ABI image.
    auto *first_bytes = reinterpret_cast<std::byte *>(&first);
    auto *second_bytes = reinterpret_cast<std::byte *>(&second);
    first_bytes[10] = std::byte{0xA5};
    second_bytes[10] = std::byte{0x5A};
    first_bytes[717] = std::byte{0x3C};
    second_bytes[717] = std::byte{0xC3};

    const auto first_image = hs::EncodeSimulationRules(first);
    const auto second_image = hs::EncodeSimulationRules(second);
    if (first_image == std::vector<std::byte>{} || first_image != second_image)
    {
        std::cerr << "SimulationRules padding changed the wire image.\n";
        return 1;
    }
    hs::SimulationRules decoded{};
    if (!hs::DecodeSimulationRules(first_image, decoded) ||
        hs::EncodeSimulationRules(decoded) != first_image ||
        hs::SimulationRulesHash(decoded) != hs::SimulationRulesHash(first))
    {
        std::cerr << "SimulationRules wire roundtrip changed semantic fields.\n";
        return 1;
    }
    const auto rejected = [&](std::vector<std::byte> malformed) {
        auto output = first;
        const auto before = hs::EncodeSimulationRules(output);
        return !hs::DecodeSimulationRules(malformed, output) &&
               hs::EncodeSimulationRules(output) == before;
    };
    auto malformed = first_image;
    malformed.pop_back();
    if (!rejected(malformed)) return 1;
    malformed = first_image;
    malformed.push_back(std::byte{0});
    if (!rejected(malformed)) return 1;
    malformed = first_image;
    malformed[8] ^= std::byte{1}; // domain
    if (!rejected(malformed)) return 1;
    malformed = first_image;
    constexpr auto header = 8 + sizeof("project_hs.simulation_rules") - 1;
    malformed[header] ^= std::byte{1}; // wire version
    if (!rejected(malformed)) return 1;
    malformed = first_image;
    malformed[header + 4 + 4 + 4 + 4] ^= std::byte{1}; // boundary points count
    if (!rejected(malformed)) return 1;

    auto invalid = first;
    invalid.progression.card_candidate_count = 0;
    if (!rejected(hs::EncodeSimulationRules(invalid))) return 1;
    invalid = first;
    invalid.progression.card_candidate_count = 4;
    if (!rejected(hs::EncodeSimulationRules(invalid))) return 1;
    invalid = first;
    invalid.relic_drop.maximum_choices_per_box = 4;
    if (!rejected(hs::EncodeSimulationRules(invalid))) return 1;
    for (const float base : {0.0f, 0.5f})
    {
        invalid = first;
        invalid.progression.required_xp_base = base;
        invalid.progression.required_xp_linear = invalid.progression.required_xp_quadratic = 0;
        if (!rejected(hs::EncodeSimulationRules(invalid))) return 1;
    }
    invalid = first;
    invalid.upgrades.charged_shot.boss_or_fifth_pierce_split.projectile_count = 9;
    if (!rejected(hs::EncodeSimulationRules(invalid))) return 1;

    second.arena_boundary.points.back().x += 1.0f;
    second.arena_obstacles.back().radius += 1.0f;
    second.player_attack += 1.0f; // Legacy alias retained in the cooked ABI.
    second.skills.back().projectile_range += 1.0f;
    if (hs::EncodeSimulationRules(second) == first_image)
    {
        std::cerr << "A semantic rule field was omitted from the ABI image.\n";
        return 1;
    }
    return 0;
}
