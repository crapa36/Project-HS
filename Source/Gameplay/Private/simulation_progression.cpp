#include "simulation_world.hpp"

namespace hs
{

using namespace gameplay_detail;

void GameSimulation::SimulationWorld::CollectPickup(PickupActor &pickup)
{
    pickup.dead = true;
    ++balance.pickup_collected[static_cast<std::size_t>(pickup.kind)];
    if (pickup.attracted_by_magnet_stat)
        ++balance.stat_utility[static_cast<std::size_t>(StatKind::MagnetRadius)];
    if (pickup.kind != PickupKind::Experience)
    {
        constexpr std::array<DomainSignalKind, kPickupKindCount> effects{
            DomainSignalKind::ExperienceSpawned, DomainSignalKind::HealCollected,
            DomainSignalKind::MagnetCollected,
            DomainSignalKind::RelicCollected};
        EmitVfx(effects[static_cast<std::size_t>(pickup.kind)], pickup.position);
    }
    if (pickup.kind == PickupKind::Experience)
    {
        player.experience += pickup.value;
    }
    else if (pickup.kind == PickupKind::Heal)
    {
        Heal(RoundDamage(player.max_health * 0.08f));
    }
    else if (pickup.kind == PickupKind::Magnet)
    {
        for (auto &experience : pickups)
            if (!experience.dead && experience.kind == PickupKind::Experience)
                experience.globally_attracted = true;
    }
    else
    {
        GenerateRelicCards();
        if (config.automatic_choices)
        {
            SelectCard(0);
        }
        else
        {
            phase = SessionPhase::RelicSelection;
            GuardSelectionInput();
        }
    }
}

SkillTagMask GameSimulation::SimulationWorld::BuildTags() const
{
    SkillTagMask tags{};
    for (std::size_t skill = 0; skill < kCombatSkillCount; ++skill)
    {
        if (player.skill_levels[skill] == 0) continue;
        tags |= SkillTags(static_cast<SkillKind>(skill));
        for (std::uint8_t upgrade = 0; upgrade < kUpgradeCount; ++upgrade)
            if ((player.upgrades[skill] & (1u << upgrade)) != 0)
                tags |= UpgradeTags(static_cast<SkillKind>(skill), upgrade);
    }
    return tags;
}

void GameSimulation::SimulationWorld::WeightedShuffle(std::vector<CardView> &candidates,
                     std::uint64_t sequence, std::uint64_t purpose)
{
    const auto build_tags = BuildTags();
    const auto weight = [&](const CardView &card) {
        const auto tags = card.kind == CardKind::LearnSkill
                              ? SkillTags(static_cast<SkillKind>(card.subject))
                              : UpgradeTags(static_cast<SkillKind>(card.subject),
                                            card.upgrade);
        return 1u + std::popcount(static_cast<unsigned>(tags & build_tags));
    };
    for (std::size_t first = 0; first + 1 < candidates.size(); ++first)
    {
        std::uint32_t total{};
        for (std::size_t index = first; index < candidates.size(); ++index)
            total += weight(candidates[index]);
        auto roll = static_cast<std::uint32_t>(
            Random(player.level ^ Mix(sequence), purpose + first) % total);
        auto selected = first;
        while (roll >= weight(candidates[selected]))
            roll -= weight(candidates[selected++]);
        std::swap(candidates[first], candidates[selected]);
    }
}

void GameSimulation::SimulationWorld::GenerateLevelCards(bool exclude_current)
{
    const auto previous_cards = cards;
    const auto previous_count = card_count;
    std::vector<CardView> candidates;
    for (std::size_t index = 1; index < kCombatSkillCount; ++index)
    {
        if (player.skill_levels[index] == 0 && player.loadout.back() == SkillKind::Count)
        {
            candidates.push_back({CardKind::LearnSkill,
                                  static_cast<std::uint8_t>(index), 0});
        }
    }
    for (std::size_t skill = 0; skill < kCombatSkillCount; ++skill)
    {
        if (player.skill_levels[skill] == 0 || std::popcount(player.upgrades[skill]) >= 4)
        {
            continue;
        }
        for (std::uint8_t upgrade = 0; upgrade < 8; ++upgrade)
        {
            if ((player.upgrades[skill] & (1u << upgrade)) == 0)
            {
                candidates.push_back({skill == 0 ? CardKind::BasicUpgrade
                                                 : CardKind::SkillUpgrade,
                                      static_cast<std::uint8_t>(skill), upgrade});
            }
        }
    }
    if (exclude_current && candidates.size() > cards.size())
    {
        std::erase_if(candidates, [&](const CardView &candidate) {
            return std::ranges::any_of(
                std::span(previous_cards.data(), previous_count),
                [&](const CardView &previous) {
                    return std::tie(candidate.kind, candidate.subject, candidate.upgrade) ==
                           std::tie(previous.kind, previous.subject, previous.upgrade);
                });
        });
    }
    std::ranges::sort(candidates, [](const CardView &left, const CardView &right) {
        return std::tie(left.kind, left.subject, left.upgrade) <
               std::tie(right.kind, right.subject, right.upgrade);
    });
    WeightedShuffle(candidates, level_reroll_sequence, 0x43415244ull);
    card_count = 0;
    if (player.loadout.back() == SkillKind::Count)
    {
        const auto guaranteed = std::ranges::find(candidates, CardKind::LearnSkill,
                                                   &CardView::kind);
        if (guaranteed != candidates.end())
        {
            cards[card_count++] = *guaranteed;
            candidates.erase(guaranteed);
        }
    }
    for (const auto candidate : candidates)
    {
        if (card_count == cards.size()) break;
        cards[card_count++] = candidate;
    }
    while (card_count < cards.size())
    {
        cards[card_count++] = {CardKind::BonusStatPoint, 0, 0};
    }
}

void GameSimulation::SimulationWorld::GuardSelectionInput()
{
    selection_input_guard_frames = 12;
    selection_waiting_for_release = input.held.basic_attack_held;
}

void GameSimulation::SimulationWorld::GenerateRelicCards(bool exclude_current)
{
    const auto previous_cards = cards;
    const auto previous_count = card_count;
    std::vector<CardView> candidates;
    const auto build_tags = BuildTags();
    for (std::uint8_t relic = 0; relic < kRelicCount; ++relic)
    {
        const auto prerequisite = RelicPrerequisiteTags(
            static_cast<RelicKind>(relic));
        if ((player.relic_mask & (1u << relic)) == 0 &&
            (build_tags & prerequisite) == prerequisite)
        {
            candidates.push_back({CardKind::Relic, relic, 0});
        }
    }
    if (candidates.empty())
    {
        for (std::uint8_t relic = 0; relic < kRelicCount; ++relic)
            if ((player.relic_mask & (1u << relic)) == 0)
                candidates.push_back({CardKind::Relic, relic, 0});
    }
    if (exclude_current && candidates.size() > cards.size())
    {
        std::erase_if(candidates, [&](const CardView &candidate) {
            return std::ranges::any_of(
                std::span(previous_cards.data(), previous_count),
                [&](const CardView &previous) {
                    return candidate.subject == previous.subject;
                });
        });
    }
    std::ranges::sort(candidates, {}, &CardView::subject);
    for (std::size_t index = candidates.size(); index > 1; --index)
    {
        const auto swap = Random(player.level ^ Mix(relic_reroll_sequence),
                                 0x52454C4943ull + index) % index;
        std::swap(candidates[index - 1], candidates[swap]);
    }
    card_count = static_cast<std::uint8_t>(std::min<std::size_t>(3, candidates.size()));
    for (std::size_t index = 0; index < card_count; ++index)
    {
        cards[index] = candidates[index];
    }
}

bool GameSimulation::SimulationWorld::SelectCard(std::size_t index)
{
    if (index >= card_count)
    {
        return false;
    }
    const auto card = cards[index];
    if (card.kind == CardKind::LearnSkill)
    {
        const auto skill = static_cast<SkillKind>(card.subject);
        player.skill_levels[card.subject] = 1;
        const auto slot = std::ranges::find(player.loadout, SkillKind::Count);
        if (slot != player.loadout.end()) *slot = skill;
    }
    else if (card.kind == CardKind::SkillUpgrade ||
             card.kind == CardKind::BasicUpgrade)
    {
        player.upgrades[card.subject] |= 1u << card.upgrade;
        player.skill_levels[card.subject] = static_cast<std::uint8_t>(
            1 + std::popcount(player.upgrades[card.subject]));
    }
    else if (card.kind == CardKind::BonusStatPoint)
    {
        ++player.pending_stat_points;
    }
    else
    {
        player.relic_mask |= 1u << card.subject;
        active_rules.Rebuild(player.relic_mask);
        card_count = 0;
        phase = SessionPhase::Playing;
        return true;
    }
    ++player.pending_stat_points;
    card_count = 0;
    phase = SessionPhase::StatAllocation;
    GuardSelectionInput();
    if (config.automatic_choices)
    {
        AssignAutomaticStats();
    }
    return true;
}

void GameSimulation::SimulationWorld::AssignStat(StatKind stat)
{
    const auto index = static_cast<std::size_t>(stat);
    if (player.pending_stat_points == 0 || player.stats[index] >= 10)
    {
        return;
    }
    ++player.stats[index];
    --player.pending_stat_points;
    if (stat == StatKind::MaxHealth)
    {
        player.max_health = RoundDamage(100.0f * (1.0f + 0.08f * player.stats[index]));
        balance.stat_utility[index] += 8;
        Heal(8);
    }
    if (player.pending_stat_points == 0)
    {
        if (player.pending_levels > 0)
        {
            --player.pending_levels;
            GenerateLevelCards();
            phase = SessionPhase::CardSelection;
            GuardSelectionInput();
            if (config.automatic_choices) SelectCard(0);
        }
        else
        {
            phase = SessionPhase::Playing;
        }
    }
}

void GameSimulation::SimulationWorld::AssignAutomaticStats()
{
    while (player.pending_stat_points > 0)
    {
        bool assigned{};
        for (std::size_t offset = 0; offset < kStatCount; ++offset)
        {
            const auto index = (player.level + offset) % kStatCount;
            if (player.stats[index] < 10)
            {
                AssignStat(static_cast<StatKind>(index));
                assigned = true;
                break;
            }
        }
        if (!assigned)
        {
            player.pending_stat_points = 0;
            phase = SessionPhase::Playing;
        }
    }
}

void GameSimulation::SimulationWorld::XpCardPhase()
{
    constexpr float kPickupAttractSpeed = 15.0f;
    for (auto &pickup : pickups)
    {
        if (pickup.dead) continue;
        if (config.scenario.auto_collect_progression &&
            (pickup.kind == PickupKind::Experience ||
             pickup.kind == PickupKind::RelicChest))
        {
            pickup.position = player.position;
        }
        const auto delta = Subtract(player.position, pickup.position);
        const auto global_experience_magnet =
            pickup.kind == PickupKind::Experience && pickup.globally_attracted;
        const auto radius = global_experience_magnet
                                ? data.arena_half_extent * 3.0f
                                : EffectiveMagnetRadius();
        if (LengthSquared(delta) <= radius * radius)
        {
            if (!global_experience_magnet &&
                LengthSquared(delta) > data.player_magnet_radius *
                                               data.player_magnet_radius)
                pickup.attracted_by_magnet_stat = true;
            pickup.position = Add(pickup.position,
                                  Multiply(Normalize(delta),
                                           kPickupAttractSpeed * kTickSeconds));
        }
        if (DistanceSquared(pickup.position, player.position) <= 0.36f)
        {
            CollectPickup(pickup);
        }
    }
    while (config.scenario.progression_enabled &&
           player.experience >= ExperienceForLevel(player.level))
    {
        player.experience -= ExperienceForLevel(player.level);
        ++player.level;
        ++player.pending_levels;
    }
    if (player.pending_levels > 0 && phase == SessionPhase::Playing)
    {
        CancelChargedShot();
        --player.pending_levels;
        GenerateLevelCards();
        phase = SessionPhase::CardSelection;
        GuardSelectionInput();
        if (config.automatic_choices) SelectCard(0);
    }
}

} // namespace hs
