#include <hs/game_rules/simulation_rules.hpp>

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

    const auto first_image = hs::CanonicalSimulationRulesBytes(first);
    const auto second_image = hs::CanonicalSimulationRulesBytes(second);
    if (first_image.size() != sizeof(first) || first_image != second_image ||
        first_image[10] != std::byte{0} || first_image[717] != std::byte{0})
    {
        std::cerr << "SimulationRules padding changed the canonical ABI image.\n";
        return 1;
    }

    second.arena_boundary.points.back().x += 1.0f;
    second.arena_obstacles.back().radius += 1.0f;
    second.player_attack += 1.0f; // Legacy alias retained in the cooked ABI.
    second.skills.back().projectile_range += 1.0f;
    if (hs::CanonicalSimulationRulesBytes(second) == first_image)
    {
        std::cerr << "A semantic rule field was omitted from the ABI image.\n";
        return 1;
    }
    return 0;
}
