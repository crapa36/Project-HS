#include "simulation_world.hpp"

namespace hs
{

using namespace gameplay_detail;

void GameSimulation::SimulationWorld::CollectPickup(PickupActor &pickup)
{
    const auto target_collected_signal = [this]() {
        auto &signal = combat_state->domain_signals.back();
        signal.target = {actors->player.position.x, signal.position.y,
                         actors->player.position.y};
        signal.flags |= static_cast<std::uint8_t>(DomainSignalFlag::HasTarget);
    };
    progression->pickup_work.OnCollected(pickup.id.value);
    pickup.dead = true;
    if (HasRelic(actors->player.relic_mask, RelicKind::PickupReward))
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
        target_collected_signal();
    }
    if (pickup.kind == PickupKind::Experience)
    {
        actors->player.experience += pickup.value;
        EmitSignal(DomainSignalKind::ExperienceCollected, pickup.position);
        target_collected_signal();
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
        DomainSignalGeometry geometry{};
        geometry.kind = DomainSignalGeometryKind::Circle;
        geometry.radius = rules.arena_half_extent * 3.0f;
        geometry.source_id = pickup.id.value;
        geometry.start_tick = geometry.end_tick = tick;
        EmitVfx(DomainSignalKind::MagnetActivated, actors->player.position, {},
                1.0f, 0.025f, 0, 0, geometry);
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
    constexpr auto maximum_candidates = kCombatSkillCount * kUpgradeCount + kCombatSkillCount - 1;
    std::array<std::uint32_t, maximum_candidates> weights{};
    assert(candidates.size() <= weights.size());
    std::uint32_t total{};
    for (std::size_t index = 0; index < candidates.size(); ++index)
        total += weights[index] = weight(candidates[index]);
    for (std::size_t first = 0; first + 1 < candidates.size(); ++first)
    {
        auto roll = static_cast<std::uint32_t>(
            Random(actors->player.level ^ Mix(sequence), purpose + first) % total);
        auto selected = first;
        while (roll >= weights[selected])
            roll -= weights[selected++];
        std::swap(candidates[first], candidates[selected]);
        std::swap(weights[first], weights[selected]);
        total -= weights[first];
    }
}

void GameSimulation::SimulationWorld::GenerateLevelCards(bool exclude_current)
{
    const auto previous_cards = progression->cards;
    const auto previous_count = progression->card_count;
    std::vector<CardView> candidates;
    const auto active_count = static_cast<std::size_t>(std::ranges::count_if(
        actors->player.loadout, [](SkillKind value) { return value != SkillKind::Count; }));
    for (std::size_t index = 1; index < kCombatSkillCount; ++index)
    {
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
    EmitSignal(DomainSignalKind::PlayerStatAllocated, actors->player.position,
               static_cast<std::uint8_t>(stat));
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
    if (session_phase != SessionPhase::Playing) return;
    auto &work = progression->pickup_work;
    work.Begin(actors->pickups, actors->player.position, EffectiveMagnetRadius(),
               rules.growth.experience_pickup_radius,
               config.scenario.auto_collect_progression, rules.arena_half_extent);
    while (const auto id = work.Next())
    {
        auto found = std::lower_bound(actors->pickups.begin(), actors->pickups.end(), *id,
            [](const PickupActor &pickup, std::uint64_t value) { return pickup.id.value < value; });
        if (found == actors->pickups.end() || found->id.value != *id || found->dead) continue;
        work.Visit(*id);
        auto &pickup = *found;
        if (config.scenario.auto_collect_progression &&
            (pickup.kind == PickupKind::Experience ||
             pickup.kind == PickupKind::RelicChest))
        {
            pickup.position = actors->player.position;
        }
        const auto delta = Subtract(actors->player.position, pickup.position);
        const auto distance_squared = LengthSquared(delta);
        const auto global_experience_magnet =
            pickup.kind == PickupKind::Experience && pickup.globally_attracted;
        const auto radius = global_experience_magnet
                                ? rules.arena_half_extent * 3.0f
                                : EffectiveMagnetRadius();
        const auto attracted = distance_squared <= radius * radius;
        if (attracted)
        {
            const auto distance = std::sqrt(distance_squared);
            if (!global_experience_magnet &&
                distance_squared > rules.stats.base_magnet_radius_m *
                                               rules.stats.base_magnet_radius_m)
                pickup.attracted_by_magnet_stat = true;
            if (pickup.kind == PickupKind::Experience && distance_squared > 0.0f &&
                rules.growth.experience_pickup_speed > 0.0f)
            {
                if (pickup.visual_motion_started == 0 ||
                    pickup.visual_motion_updated + 1 != tick)
                {
                    pickup.visual_motion_started = tick;
                    pickup.visual_motion_initial_distance = distance;
                }
                pickup.visual_motion_updated = tick;
            }
            const auto direction = distance <= 0.00001f ? Float2{0.0f, 1.0f}
                : Float2{delta.x / distance, delta.y / distance};
            pickup.position = Add(pickup.position,
                                  Multiply(direction,
                                           rules.growth.experience_pickup_speed *
                                               kTickSeconds));
        }
        if (DistanceSquared(pickup.position, actors->player.position) <=
            rules.growth.experience_pickup_radius *
                rules.growth.experience_pickup_radius)
        {
            const auto magnet = pickup.kind == PickupKind::Magnet;
            CollectPickup(pickup);
            if (magnet) work.AfterMagnet(actors->pickups, *id, rules.arena_half_extent);
        }
        // Collection callbacks may append actors; resolve the logical ID again.
        found = std::lower_bound(actors->pickups.begin(), actors->pickups.end(), *id,
            [](const PickupActor &value, std::uint64_t target) { return value.id.value < target; });
        if (found != actors->pickups.end() && found->id.value == *id)
            work.Update(*found, attracted && rules.growth.experience_pickup_speed > 0.0f);
        if (session_phase != SessionPhase::Playing) break;
    }
    while (config.scenario.progression_enabled)
    {
        const auto required_xp = ExperienceForLevel(rules, actors->player.level);
        if (actors->player.experience < required_xp) break;
        actors->player.experience -= required_xp;
        ++actors->player.level;
        ++actors->player.pending_levels;
        EmitSignal(DomainSignalKind::PlayerLevelUp, actors->player.position);
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
