#include "simulation_world.hpp"

namespace hs
{

using namespace gameplay_detail;

void GameSimulation::SimulationWorld::CollectPickup(PickupActor &pickup)
{
    pickup.dead = true;
    for (const auto &rule : combat_state->relic_rules.RulesFor(RelicRuleHook::OnPickup))
        if (rule.handler == RelicRuleHandlerId::PickupReward)
            HandlePickupReward(pickup.kind);
    ++Metrics().pickup_collected[static_cast<std::size_t>(pickup.kind)];
    if (pickup.attracted_by_magnet_stat)
        ++Metrics().stat_utility[static_cast<std::size_t>(StatKind::MagnetRadius)];
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
        actors->player.experience += pickup.value;
        EmitSignal(DomainSignalKind::ExperienceCollected, pickup.position);
    }
    else if (pickup.kind == PickupKind::Heal)
    {
        Heal(RoundDamage(actors->player.max_health *
                         rules.relic_drop.healing_pickup_maximum_hp_heal_fraction), false);
    }
    else if (pickup.kind == PickupKind::Magnet)
    {
        for (auto &experience : actors->pickups)
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
            session_phase = SessionPhase::RelicSelection;
            GuardSelectionInput();
        }
    }
}

SkillTagMask GameSimulation::SimulationWorld::BuildTags() const
{
    SkillTagMask tags{};
    for (std::size_t skill = 0; skill < kCombatSkillCount; ++skill)
    {
        if (actors->player.skill_levels[skill] == 0) continue;
        tags |= SkillTags(static_cast<SkillKind>(skill));
        for (std::uint8_t upgrade = 0; upgrade < kUpgradeCount; ++upgrade)
            if ((actors->player.upgrades[skill] & (1u << upgrade)) != 0)
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
            Random(actors->player.level ^ Mix(sequence), purpose + first) % total);
        auto selected = first;
        while (roll >= weight(candidates[selected]))
            roll -= weight(candidates[selected++]);
        std::swap(candidates[first], candidates[selected]);
    }
}

void GameSimulation::SimulationWorld::GenerateLevelCards(bool exclude_current)
{
    const auto previous_cards = progression->cards;
    const auto previous_count = progression->card_count;
    std::vector<CardView> candidates;
    for (std::size_t index = 1; index < kCombatSkillCount; ++index)
    {
        const auto active_count = static_cast<std::size_t>(std::ranges::count_if(
            actors->player.loadout, [](SkillKind value) { return value != SkillKind::Count; }));
        if (actors->player.skill_levels[index] == 0 &&
            active_count < rules.progression.active_slot_count)
        {
            candidates.push_back({CardKind::LearnSkill,
                                  static_cast<std::uint8_t>(index), 0});
        }
    }
    for (std::size_t skill = 0; skill < kCombatSkillCount; ++skill)
    {
        const auto &definition = rules.skills[skill];
        const auto selected_upgrades = std::popcount(actors->player.upgrades[skill]);
        if (actors->player.skill_levels[skill] == 0 ||
            selected_upgrades >= rules.progression.upgrades_selected_per_skill ||
            actors->player.skill_levels[skill] >= definition.maximum_level)
        {
            continue;
        }
        for (std::uint8_t upgrade = 0; upgrade < 8; ++upgrade)
        {
            if ((actors->player.upgrades[skill] & (1u << upgrade)) == 0)
            {
                candidates.push_back({skill == 0 ? CardKind::BasicUpgrade
                                                 : CardKind::SkillUpgrade,
                                      static_cast<std::uint8_t>(skill), upgrade});
            }
        }
    }
    if (exclude_current && candidates.size() > rules.progression.card_candidate_count)
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
    WeightedShuffle(candidates, progression->level_reroll_sequence, 0x43415244ull);
    progression->card_count = 0;
    const auto active_count = static_cast<std::size_t>(std::ranges::count_if(
        actors->player.loadout, [](SkillKind value) { return value != SkillKind::Count; }));
    if (active_count < rules.progression.active_slot_count)
    {
        const auto guaranteed = std::ranges::find(candidates, CardKind::LearnSkill,
                                                   &CardView::kind);
        if (guaranteed != candidates.end())
        {
            progression->cards[progression->card_count++] = *guaranteed;
            candidates.erase(guaranteed);
        }
    }
    for (const auto candidate : candidates)
    {
        if (progression->card_count == std::min<std::size_t>(
                rules.progression.card_candidate_count, progression->cards.size())) break;
        progression->cards[progression->card_count++] = candidate;
    }
    while (progression->card_count < std::min<std::size_t>(
               rules.progression.card_candidate_count, progression->cards.size()))
    {
        if (!rules.progression.fallback_stat_point_cards) break;
        progression->cards[progression->card_count++] = {CardKind::BonusStatPoint, 0, 0};
    }
}

void GameSimulation::SimulationWorld::GuardSelectionInput()
{
    selection_input_guard_frames = 12;
    selection_waiting_for_release = current_input.held.basic_attack_held;
}

void GameSimulation::SimulationWorld::GenerateRelicCards(bool exclude_current)
{
    const auto previous_cards = progression->cards;
    const auto previous_count = progression->card_count;
    std::vector<CardView> candidates;
    const auto build_tags = BuildTags();
    for (std::uint8_t relic = 0; relic < kRelicCount; ++relic)
    {
        const auto prerequisite = RelicPrerequisiteTags(
            static_cast<RelicKind>(relic));
        if ((actors->player.relic_mask & (RelicMask{1} << relic)) == 0 &&
            (build_tags & prerequisite) == prerequisite)
        {
            candidates.push_back({CardKind::Relic, relic, 0});
        }
    }
    if (candidates.empty())
    {
        for (std::uint8_t relic = 0; relic < kRelicCount; ++relic)
            if ((actors->player.relic_mask & (RelicMask{1} << relic)) == 0)
                candidates.push_back({CardKind::Relic, relic, 0});
    }
    if (exclude_current && candidates.size() > rules.progression.card_candidate_count)
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
        const auto swap = Random(actors->player.level ^ Mix(progression->relic_reroll_sequence),
                                 0x52454C4943ull + index) % index;
        std::swap(candidates[index - 1], candidates[swap]);
    }
    progression->card_count = static_cast<std::uint8_t>(std::min<std::size_t>(
        {rules.progression.card_candidate_count, rules.relic_drop.maximum_choices_per_box,
         candidates.size()}));
    for (std::size_t index = 0; index < progression->card_count; ++index)
    {
        progression->cards[index] = candidates[index];
    }
}

bool GameSimulation::SimulationWorld::SelectCard(std::size_t index)
{
    if (index >= progression->card_count)
    {
        return false;
    }
    const auto card = progression->cards[index];
    if (card.kind == CardKind::LearnSkill)
    {
        const auto skill = static_cast<SkillKind>(card.subject);
        actors->player.skill_levels[card.subject] = rules.skills[card.subject].starting_level;
        const auto slot = std::ranges::find(actors->player.loadout, SkillKind::Count);
        if (slot != actors->player.loadout.end()) *slot = skill;
        EmitSignal(DomainSignalKind::SkillUnlocked, actors->player.position,
                   static_cast<std::uint8_t>(skill));
    }
    else if (card.kind == CardKind::SkillUpgrade ||
             card.kind == CardKind::BasicUpgrade)
    {
        actors->player.upgrades[card.subject] |= 1u << card.upgrade;
        actors->player.skill_levels[card.subject] = static_cast<std::uint8_t>(
            rules.skills[card.subject].starting_level +
            std::popcount(actors->player.upgrades[card.subject]));
    }
    else if (card.kind == CardKind::BonusStatPoint)
    {
        actors->player.pending_stat_points += rules.progression.fallback_extra_stat_points;
    }
    else
    {
        actors->player.relic_mask |= RelicMask{1} << card.subject;
        combat_state->relic_rules.Rebuild(actors->player.relic_mask);
        progression->card_count = 0;
        session_phase = SessionPhase::Playing;
        return true;
    }
    actors->player.pending_stat_points += rules.progression.base_stat_points_per_level;
    progression->card_count = 0;
    session_phase = SessionPhase::StatAllocation;
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
    if (actors->player.pending_stat_points == 0 ||
        actors->player.stats[index] >= rules.stats.maximum_points_per_stat)
    {
        return;
    }
    ++actors->player.stats[index];
    --actors->player.pending_stat_points;
    if (stat == StatKind::MaxHealth)
    {
        const auto &allocation = rules.stats.allocations[index];
        actors->player.max_health = RoundDamage(
            rules.stats.base_maximum_hp *
            (1.0f + allocation.amount_per_point * actors->player.stats[index]));
        const auto restored = RoundDamage(allocation.immediate_current_hp_restore_per_point);
        Metrics().stat_utility[index] += static_cast<std::uint64_t>(restored);
        Heal(restored);
    }
    if (actors->player.pending_stat_points == 0)
    {
        if (actors->player.pending_levels > 0)
        {
            --actors->player.pending_levels;
            GenerateLevelCards();
            session_phase = SessionPhase::CardSelection;
            GuardSelectionInput();
            if (config.automatic_choices) SelectCard(0);
        }
        else
        {
            session_phase = SessionPhase::Playing;
        }
    }
}

void GameSimulation::SimulationWorld::AssignAutomaticStats()
{
    while (actors->player.pending_stat_points > 0)
    {
        bool assigned{};
        for (std::size_t offset = 0; offset < kStatCount; ++offset)
        {
            const auto index = (actors->player.level + offset) % kStatCount;
            if (actors->player.stats[index] < rules.stats.maximum_points_per_stat)
            {
                AssignStat(static_cast<StatKind>(index));
                assigned = true;
                break;
            }
        }
        if (!assigned)
        {
            actors->player.pending_stat_points = 0;
            session_phase = SessionPhase::Playing;
        }
    }
}

void GameSimulation::SimulationWorld::XpCardPhase()
{
    for (auto &pickup : actors->pickups)
    {
        if (pickup.dead) continue;
        if (config.scenario.auto_collect_progression &&
            (pickup.kind == PickupKind::Experience ||
             pickup.kind == PickupKind::RelicChest))
        {
            pickup.position = actors->player.position;
        }
        const auto delta = Subtract(actors->player.position, pickup.position);
        const auto global_experience_magnet =
            pickup.kind == PickupKind::Experience && pickup.globally_attracted;
        const auto radius = global_experience_magnet
                                ? rules.arena_half_extent * 3.0f
                                : EffectiveMagnetRadius();
        if (LengthSquared(delta) <= radius * radius)
        {
            if (!global_experience_magnet &&
                LengthSquared(delta) > rules.stats.base_magnet_radius_m *
                                               rules.stats.base_magnet_radius_m)
                pickup.attracted_by_magnet_stat = true;
            pickup.position = Add(pickup.position,
                                  Multiply(Normalize(delta),
                                           rules.growth.experience_pickup_speed *
                                               kTickSeconds));
        }
        if (DistanceSquared(pickup.position, actors->player.position) <=
            rules.growth.experience_pickup_radius *
                rules.growth.experience_pickup_radius)
        {
            CollectPickup(pickup);
        }
    }
    while (config.scenario.progression_enabled &&
           actors->player.experience >= ExperienceForLevel(rules, actors->player.level))
    {
        actors->player.experience -= ExperienceForLevel(rules, actors->player.level);
        ++actors->player.level;
        ++actors->player.pending_levels;
    }
    if (actors->player.pending_levels > 0 && session_phase == SessionPhase::Playing)
    {
        CancelChargedShot();
        --actors->player.pending_levels;
        GenerateLevelCards();
        session_phase = SessionPhase::CardSelection;
        GuardSelectionInput();
        if (config.automatic_choices) SelectCard(0);
    }
}

} // namespace hs
