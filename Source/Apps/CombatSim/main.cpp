#include <hs/core/fixed_step_clock.hpp>
#include <hs/gameplay/game_simulation.hpp>
#include <hs/core/process_info.hpp>
#include <hs/presentation/projector.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
using Json = nlohmann::json;

constexpr std::array<std::string_view, hs::kCombatSkillCount> kCombatSuiteSkillIds{
    "basic_attack", "piercing_shot", "multishot", "charged_shot",
    "explosive_arrow", "ricochet_arrow", "arrow_rain", "trap", "retreat_shot"};

bool WriteSnapshot(hs::GameSimulation &simulation, hs::RenderSnapshotStorage &snapshot)
{
    static const hs::PresentationCatalog presentation;
    static const hs::SettingsData settings;
    hs::GameReadModelStorage model;
    simulation.WriteReadModel(model);
    return hs::ProjectRenderSnapshot(model.View(), presentation, {}, settings, snapshot);
}
constexpr std::array<std::string_view, hs::kRelicCount> kCombatSuiteRelicIds{
    "bleed_kill_heal", "burn_spread_on_kill", "kill_cooldown_surge",
    "bleed_burn_explosion", "sixth_basic_radial", "basic_kill_tracking_arrow",
    "movement_afterimage_arrow", "alternating_active_refund",
    "cross_active_tracking_arrow", "on_damage_push_slow", "once_revive",
    "combat_hit_chain"};
constexpr std::array<std::string_view, hs::kStatCount> kCombatSuiteStatIds{
    "maximum_hp", "movement_speed", "attack_power", "basic_attack_speed",
    "cooldown_reduction", "magnet_radius"};
struct CombatBuild
{
    std::string id;
    std::vector<std::uint8_t> skills;
    std::array<std::uint8_t, hs::kCombatSkillCount> upgrade_masks{};
    std::vector<std::uint8_t> relics;
    std::array<std::uint8_t, hs::kStatCount> stats{};
};

struct CombatSimulationSuite
{
    hs::Tick maximum_ticks{};
    std::vector<std::uint64_t> seeds;
    std::vector<CombatBuild> builds;
};

struct ProgressionSimulationSuite
{
    hs::Tick maximum_ticks{};
    std::vector<std::uint64_t> seeds;
};

struct ProgressionDraftProfile
{
    std::string id;
    std::uint8_t focus_skill{};
    std::uint8_t variant{};
};

template <std::size_t Size>
std::size_t RequireIdIndex(std::string_view value,
                    const std::array<std::string_view, Size> &ids,
                    std::string_view category)
{
    const auto found = std::ranges::find(ids, value);
    if (found == ids.end())
        throw std::runtime_error(std::string(category) + " id is invalid: " +
                                 std::string(value));
    return static_cast<std::size_t>(found - ids.begin());
}

CombatSimulationSuite LoadCombatSimulationSuite(const std::filesystem::path &path)
{
    std::ifstream stream(path);
    Json root;
    stream >> root;
    if (!stream || root.value("schema_version", 0) != 1)
        throw std::runtime_error("Combat suite must be valid schema_version 1 JSON.");

    const auto seconds = root.at("duration_seconds").get<std::uint32_t>();
    if (seconds == 0 || seconds > 1'800)
        throw std::runtime_error("duration_seconds must be in [1, 1800].");
    CombatSimulationSuite suite;
    suite.maximum_ticks = static_cast<hs::Tick>(seconds) * 60;
    suite.seeds = root.at("seeds").get<std::vector<std::uint64_t>>();
    if (suite.seeds.empty() || suite.seeds.size() > 32 ||
        std::set(suite.seeds.begin(), suite.seeds.end()).size() != suite.seeds.size())
        throw std::runtime_error("seeds must contain 1-32 unique values.");

    std::set<std::string> build_ids;
    for (const auto &source : root.at("builds"))
    {
        CombatBuild build;
        build.id = source.at("id").get<std::string>();
        if (build.id.empty() || !build_ids.insert(build.id).second)
            throw std::runtime_error("Build ids must be non-empty and unique.");
        for (const auto &skill : source.at("skills"))
        {
            const auto index = RequireIdIndex(skill.get<std::string>(), kCombatSuiteSkillIds, "skill");
            if (index == 0 || build.skills.size() == 4 ||
                std::ranges::find(build.skills, index) != build.skills.end())
                throw std::runtime_error("Each build requires 0-4 unique active skills.");
            build.skills.push_back(static_cast<std::uint8_t>(index));
        }
        for (const auto &[skill_id, upgrades] : source.at("upgrades").items())
        {
            const auto skill = RequireIdIndex(skill_id, kCombatSuiteSkillIds, "upgrade skill");
            if (skill != 0 && std::ranges::find(build.skills, skill) == build.skills.end())
                throw std::runtime_error("Upgrades require the owning skill in the build.");
            for (const auto &ordinal_json : upgrades)
            {
                const auto ordinal = ordinal_json.get<std::uint32_t>();
                if (ordinal == 0 || ordinal > hs::kUpgradeCount ||
                    (build.upgrade_masks[skill] & (1u << (ordinal - 1))) != 0)
                    throw std::runtime_error("Upgrade ordinals must be unique values in [1, 8].");
                build.upgrade_masks[skill] |= 1u << (ordinal - 1);
            }
            if (std::popcount(build.upgrade_masks[skill]) > 4)
                throw std::runtime_error("A skill can equip at most four upgrades.");
        }
        for (const auto &relic : source.at("relics"))
        {
            const auto index = RequireIdIndex(relic.get<std::string>(), kCombatSuiteRelicIds, "relic");
            if (std::ranges::find(build.relics, index) != build.relics.end())
                throw std::runtime_error("Relics must be unique.");
            build.relics.push_back(static_cast<std::uint8_t>(index));
        }
        for (const auto &[stat_id, points_json] : source.at("stats").items())
        {
            const auto stat = RequireIdIndex(stat_id, kCombatSuiteStatIds, "stat");
            const auto points = points_json.get<std::uint32_t>();
            if (points > 10) throw std::runtime_error("Stat points must be in [0, 10].");
            build.stats[stat] = static_cast<std::uint8_t>(points);
        }
        suite.builds.push_back(std::move(build));
    }
    if (suite.builds.empty()) throw std::runtime_error("At least one build is required.");
    return suite;
}

ProgressionSimulationSuite LoadProgressionSimulationSuite(const std::filesystem::path &path)
{
    std::ifstream stream(path);
    Json root;
    stream >> root;
    if (!stream || root.value("schema_version", 0) != 1)
        throw std::runtime_error(
            "Progression suite must be valid schema_version 1 JSON.");
    const auto seconds = root.at("duration_seconds").get<std::uint32_t>();
    if (seconds < 900 || seconds > 1'800)
        throw std::runtime_error("duration_seconds must be in [900, 1800].");
    ProgressionSimulationSuite suite;
    suite.maximum_ticks = static_cast<hs::Tick>(seconds) * 60;
    suite.seeds = root.at("seeds").get<std::vector<std::uint64_t>>();
    if (suite.seeds.empty() || suite.seeds.size() > 8 ||
        std::set(suite.seeds.begin(), suite.seeds.end()).size() != suite.seeds.size())
        throw std::runtime_error("seeds must contain 1-8 unique values.");
    return suite;
}

hs::Float3 SelectNearestEnemyAim(const hs::RenderSnapshot &snapshot,
                     hs::Float2 player_position)
{
    hs::Float2 best{player_position.x, player_position.y + 20.0f};
    auto best_distance = std::numeric_limits<float>::max();
    for (const auto &instance : snapshot.instances)
    {
        if (instance.mesh == hs::RenderMesh::Enemy ||
            instance.mesh == hs::RenderMesh::EnemyRanged ||
            instance.mesh == hs::RenderMesh::EnemySuicide ||
            instance.mesh == hs::RenderMesh::Boss)
        {
            const auto dx = instance.position.x - player_position.x;
            const auto dy = instance.position.z - player_position.y;
            const auto distance = dx * dx + dy * dy;
            if (distance < best_distance)
            {
                best = {instance.position.x, instance.position.z};
                best_distance = distance;
            }
        }
    }
    return {best.x, 0.0f, best.y};
}

hs::GameAction SkillSlotAction(std::size_t slot)
{
    constexpr std::array actions{hs::GameAction::SkillQ, hs::GameAction::SkillW,
                                 hs::GameAction::SkillE, hs::GameAction::SkillR};
    return actions[slot];
}

struct CombatControlState
{
    std::uint64_t sequence{};
    hs::Tick next_active_tick{1};
    hs::Tick charged_release_tick{};
    std::uint32_t charged_cast_count{};
    std::size_t next_slot{};
    hs::HeldInputState held{};
    std::array<hs::ActionEdge, 1> edges{};
};

hs::InputFrame MakeCombatInput(const hs::SessionProbe &probe,
                               const hs::RenderSnapshot &snapshot,
                               hs::Tick target_tick,
                               CombatControlState &state)
{
    state.held = {};
    state.held.basic_attack_held = true;
    state.held.aim_world = SelectNearestEnemyAim(snapshot, probe.player_position);
    std::span<const hs::ActionEdge> edge_span;
    if (state.charged_release_tick != 0 &&
        target_tick >= state.charged_release_tick)
    {
        const auto slot = static_cast<std::size_t>(std::ranges::find(
            probe.skill_loadout, hs::SkillKind::ChargedShot) -
            probe.skill_loadout.begin());
        if (slot < probe.skill_loadout.size())
        {
            state.edges[0] = {++state.sequence, SkillSlotAction(slot),
                              hs::EdgeKind::Released};
            edge_span = state.edges;
        }
        state.charged_release_tick = 0;
        state.next_active_tick = target_tick + 10;
    }
    else if (state.charged_release_tick == 0 &&
             target_tick >= state.next_active_tick &&
             probe.active_skill_count != 0 &&
             probe.normal_enemy_count + probe.boss_count != 0)
    {
        for (std::size_t offset = 0; offset < probe.skill_loadout.size(); ++offset)
        {
            const auto slot = (state.next_slot + offset) % probe.skill_loadout.size();
            const auto skill = probe.skill_loadout[slot];
            if (skill == hs::SkillKind::Count ||
                probe.cooldown_ticks[static_cast<std::size_t>(skill) - 1] != 0)
                continue;
            state.edges[0] = {++state.sequence, SkillSlotAction(slot),
                              hs::EdgeKind::Pressed};
            edge_span = state.edges;
            state.next_slot = (slot + 1) % probe.skill_loadout.size();
            if (skill == hs::SkillKind::ChargedShot)
            {
                const auto mask = probe.upgrade_masks[
                    static_cast<std::size_t>(hs::SkillKind::ChargedShot)];
                constexpr std::array<std::uint32_t, 3> kChargePercent{50, 75, 100};
                auto charge_ticks = ((mask & 1u) != 0 ? 84u : 60u) *
                    kChargePercent[state.charged_cast_count++ % kChargePercent.size()];
                charge_ticks = (charge_ticks + 99) / 100;
                if ((mask & 2u) != 0)
                    charge_ticks = (charge_ticks * 65 + 99) / 100;
                state.charged_release_tick = target_tick + charge_ticks;
            }
            else
            {
                state.next_active_tick = target_tick + 20;
            }
            break;
        }
    }
    return {target_tick, state.held, edge_span};
}

Json BuildDefinitionJson(const CombatBuild &build)
{
    Json skills = Json::array();
    for (const auto skill : build.skills) skills.push_back(kCombatSuiteSkillIds[skill]);
    Json upgrades = Json::object();
    for (std::size_t skill = 0; skill < build.upgrade_masks.size(); ++skill)
    {
        if (build.upgrade_masks[skill] == 0) continue;
        upgrades[std::string(kCombatSuiteSkillIds[skill])] = Json::array();
        for (std::size_t upgrade = 0; upgrade < hs::kUpgradeCount; ++upgrade)
            if ((build.upgrade_masks[skill] & (1u << upgrade)) != 0)
                upgrades[std::string(kCombatSuiteSkillIds[skill])].push_back(upgrade + 1);
    }
    Json relics = Json::array();
    for (const auto relic : build.relics) relics.push_back(kCombatSuiteRelicIds[relic]);
    Json stats = Json::object();
    for (std::size_t stat = 0; stat < build.stats.size(); ++stat)
        stats[std::string(kCombatSuiteStatIds[stat])] = build.stats[stat];
    return {{"id", build.id}, {"skills", std::move(skills)},
            {"upgrades", std::move(upgrades)}, {"relics", std::move(relics)},
            {"stats", std::move(stats)}};
}

Json RunCombatBuild(const CombatBuild &build, std::uint64_t seed, hs::Tick maximum_ticks,
              const hs::SimulationRules &rules)
{
    hs::GameSimulation simulation;
    hs::SimulationConfig config{seed};
    config.scenario = {.player_stationary = true, .player_invulnerable = true,
                       .progression_enabled = false};
    if (auto initialized = simulation.Initialize(config, rules); !initialized)
        throw std::runtime_error(std::string(initialized.Message()));

    const auto apply = [&](hs::DebugCommand command) {
        if (auto result = simulation.ApplyDebugCommand(command); !result)
            throw std::runtime_error(std::string(result.Message()));
    };
    for (const auto skill : build.skills)
        apply({hs::DebugCommandKind::GrantSkill, skill});
    for (std::size_t skill = 0; skill < build.upgrade_masks.size(); ++skill)
        for (std::uint32_t upgrade = 0; upgrade < hs::kUpgradeCount; ++upgrade)
            if ((build.upgrade_masks[skill] & (1u << upgrade)) != 0)
                apply({hs::DebugCommandKind::GrantUpgrade, skill, upgrade});
    for (const auto relic : build.relics)
        apply({hs::DebugCommandKind::GrantRelic, relic});
    for (std::size_t stat = 0; stat < build.stats.size(); ++stat)
        apply({hs::DebugCommandKind::SetStat, stat, build.stats[stat]});

    hs::RenderSnapshotStorage snapshot(20'000, 2, 2, 128);
    std::vector<std::uint64_t> tick_microseconds;
    tick_microseconds.reserve(static_cast<std::size_t>(maximum_ticks));
    CombatControlState control;
    std::uint32_t maximum_enemies{};
    std::uint32_t maximum_projectiles{};
    std::uint32_t maximum_pickups{};
    std::uint64_t maximum_memory{};

    for (hs::Tick target_tick = 1; target_tick <= maximum_ticks; ++target_tick)
    {
        const auto before = simulation.GetObservation();
        if (before.phase == hs::SessionPhase::Victory) break;
        snapshot.Clear();
        if (!WriteSnapshot(simulation, snapshot))
            throw std::runtime_error("Combat target snapshot capacity was exceeded.");

        const auto input = MakeCombatInput(before, snapshot.View(), target_tick,
                                           control);
        const auto started = std::chrono::steady_clock::now();
        const auto tick = simulation.TickFixed(input, hs::FixedStepClock::kFixedStep);
        tick_microseconds.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count()));
        simulation.ClearDomainSignals();
        const auto probe = simulation.GetObservation();
        maximum_enemies = std::max(maximum_enemies,
                                   probe.normal_enemy_count + probe.boss_count);
        maximum_projectiles = std::max(maximum_projectiles,
                                       probe.player_projectile_count +
                                           probe.enemy_projectile_count);
        maximum_pickups = std::max(maximum_pickups, probe.pickup_count);
        if (target_tick % 60 == 0)
            maximum_memory = std::max(maximum_memory,
                                      hs::CurrentProcessWorkingSetBytes());
        if (tick.phase == hs::SessionPhase::Victory) break;
    }

    const auto probe = simulation.GetObservation();
    if (probe.player_position.x != 0.0f || probe.player_position.y != 0.0f ||
        probe.health != probe.max_health)
        throw std::runtime_error("Stationary invulnerable combat contract was violated.");
    std::ranges::sort(tick_microseconds);
    const auto percentile = [&](double fraction) {
        const auto index = static_cast<std::size_t>(std::ceil(
            fraction * static_cast<double>(tick_microseconds.size()))) - 1;
        return tick_microseconds[std::min(index, tick_microseconds.size() - 1)];
    };
    const auto elapsed_seconds = static_cast<double>(probe.tick) / 60.0;
    const auto cpu_total = std::accumulate(tick_microseconds.begin(),
                                           tick_microseconds.end(), std::uint64_t{});
    Json skills = Json::array();
    for (std::size_t skill = 0; skill < kCombatSuiteSkillIds.size(); ++skill)
    {
        const auto uses = probe.balance.skill_uses[skill];
        skills.push_back({{"id", kCombatSuiteSkillIds[skill]},
                          {"damage", probe.damage_by_skill[skill]},
                          {"boss_damage", probe.balance.skill_boss_damage[skill]},
                          {"uses", uses},
                          {"casts_with_hit", probe.balance.skill_casts_with_hit[skill]},
                          {"hit_rate", uses == 0 ? 0.0 :
                              static_cast<double>(probe.balance.skill_casts_with_hit[skill]) /
                                  uses},
                          {"hit_events", probe.balance.skill_hit_events[skill]},
                          {"kills", probe.balance.skill_kills[skill]}});
    }
    Json upgrades = Json::array();
    for (std::size_t skill = 0; skill < kCombatSuiteSkillIds.size(); ++skill)
        for (std::size_t upgrade = 0; upgrade < hs::kUpgradeCount; ++upgrade)
            if ((build.upgrade_masks[skill] & (1u << upgrade)) != 0)
            {
                Json effects = Json::object();
                for (std::size_t metric = 0; metric < hs::kUpgradeEffectMetricIds.size();
                     ++metric)
                    effects[std::string(hs::kUpgradeEffectMetricIds[metric])] =
                        probe.balance.upgrade_effects[skill][upgrade][metric];
                upgrades.push_back({{"skill", kCombatSuiteSkillIds[skill]},
                                    {"ordinal", upgrade + 1},
                                    {"damage", probe.balance.upgrade_damage[skill][upgrade]},
                                    {"triggers", probe.balance.upgrade_triggers[skill][upgrade]},
                                    {"effects", std::move(effects)}});
            }
    Json relics = Json::array();
    for (const auto relic : build.relics)
    {
        Json effects = Json::object();
        for (std::size_t metric = 0; metric < hs::kUpgradeEffectMetricIds.size(); ++metric)
            effects[std::string(hs::kUpgradeEffectMetricIds[metric])] =
                probe.balance.relic_effects[relic][metric];
        relics.push_back({{"id", kCombatSuiteRelicIds[relic]},
                          {"damage", probe.balance.relic_damage[relic]},
                          {"triggers", probe.balance.relic_triggers[relic]},
                          {"effects", std::move(effects)}});
    }

    Json result{{"build_id", build.id},
                {"seed", seed},
                {"ticks", probe.tick},
                {"seconds", elapsed_seconds},
                {"checksum", simulation.ComputeChecksum()},
                {"damage", probe.damage_dealt},
                {"dps", elapsed_seconds > 0.0 ? probe.damage_dealt / elapsed_seconds : 0.0},
                {"kills", probe.kills},
                {"incoming_damage", probe.damage_taken},
                {"direct_damage", probe.balance.direct_damage},
                {"derived_damage", probe.balance.derived_damage},
                {"damage_over_time", probe.balance.damage_over_time},
                {"cpu_tick_median_us", percentile(0.5)},
                {"cpu_tick_p95_us", percentile(0.95)},
                {"cpu_total_us", cpu_total},
                {"damage_per_cpu_ms", cpu_total == 0 ? 0.0 :
                    static_cast<double>(probe.damage_dealt) * 1'000.0 / cpu_total},
                {"maximum_memory_bytes", maximum_memory},
                {"maximum_enemies", maximum_enemies},
                {"maximum_projectiles", maximum_projectiles},
                {"maximum_pickups", maximum_pickups},
                {"enemy_spawned", probe.balance.enemy_spawned},
                {"enemy_killed", probe.balance.enemy_killed},
                {"enemy_attack_attempts", probe.balance.enemy_attack_attempts},
                {"enemy_hits", probe.balance.enemy_hits},
                {"enemy_damage", probe.balance.enemy_damage},
                {"stat_utility", probe.balance.stat_utility},
                {"skills", std::move(skills)},
                {"upgrades", std::move(upgrades)},
                {"relics", std::move(relics)}};
    if (auto shutdown = simulation.Shutdown(); !shutdown)
        throw std::runtime_error(std::string(shutdown.Message()));
    return result;
}

std::uint64_t Mix(std::uint64_t value) noexcept
{
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

std::uint64_t CardScore(const hs::CardView &card, const ProgressionDraftProfile &profile,
                        std::uint64_t seed, const hs::SessionProbe &probe)
{
    const auto preferred_half = [&](std::uint8_t upgrade) {
        return upgrade / 4 == profile.variant;
    };
    if (card.kind == hs::CardKind::LearnSkill)
    {
        if (card.subject == profile.focus_skill) return 4'000'000;
        return 2'000'000 +
               Mix(seed ^ (static_cast<std::uint64_t>(profile.focus_skill) << 24) ^
                   card.subject) % 100'000;
    }
    if (card.kind == hs::CardKind::SkillUpgrade ||
        card.kind == hs::CardKind::BasicUpgrade)
    {
        if (profile.focus_skill ==
                static_cast<std::uint8_t>(hs::SkillKind::RicochetArrow) &&
            profile.variant == 1 &&
            card.subject == profile.focus_skill && card.upgrade == 0)
            return 3'600'000;
        if (card.subject == profile.focus_skill && preferred_half(card.upgrade))
            return 3'500'000 - card.upgrade;
        if (preferred_half(card.upgrade))
            return 1'500'000 +
                   Mix(seed ^ (static_cast<std::uint64_t>(card.subject) << 16) ^
                       card.upgrade) % 100'000;
        return 1'000'000 +
               Mix(seed ^ probe.level ^
                   (static_cast<std::uint64_t>(card.subject) << 16) ^ card.upgrade) %
                   100'000;
    }
    if (card.kind == hs::CardKind::Relic)
        return 1'000'000 +
               Mix(seed ^ (static_cast<std::uint64_t>(profile.focus_skill) << 20) ^
                   (static_cast<std::uint64_t>(profile.variant) << 16) ^ card.subject) %
                   100'000;
    return 0;
}

struct Acquisition
{
    hs::Tick tick{};
    hs::CardView card{};
    std::uint64_t owning_skill_uses{};
};

void ResolveProgressionChoices(hs::GameSimulation &simulation,
                               const ProgressionDraftProfile &profile, std::uint64_t seed,
                               std::vector<Acquisition> &acquisitions)
{
    for (std::size_t guard = 0; guard < 64; ++guard)
    {
        const auto probe = simulation.GetObservation();
        if (probe.phase == hs::SessionPhase::StatAllocation)
        {
            constexpr std::array allowed{hs::StatKind::AttackPower,
                                         hs::StatKind::AttackSpeed,
                                         hs::StatKind::CooldownReduction};
            bool assigned{};
            for (std::size_t offset = 0; offset < allowed.size(); ++offset)
            {
                const auto stat = allowed[(probe.level + profile.focus_skill +
                                           profile.variant + offset) % allowed.size()];
                if (probe.stat_points[static_cast<std::size_t>(stat)] >= 10) continue;
                if (auto result = simulation.ApplyDebugCommand(
                        {hs::DebugCommandKind::AssignStat,
                         static_cast<std::uint64_t>(stat)});
                    !result)
                    throw std::runtime_error(std::string(result.Message()));
                assigned = true;
                break;
            }
            if (!assigned)
                throw std::runtime_error("Allowed progression stats are all capped.");
            continue;
        }
        if (probe.phase != hs::SessionPhase::CardSelection &&
            probe.phase != hs::SessionPhase::RelicSelection)
            return;
        if (probe.card_count == 0)
            throw std::runtime_error("Progression selection has no cards.");

        const auto focus_owned = probe.skill_levels[profile.focus_skill] != 0;
        const auto focus_incomplete =
            std::popcount(probe.upgrade_masks[profile.focus_skill]) < 4;
        const auto has_preferred_focus = std::ranges::any_of(
            std::span(probe.cards.data(), probe.card_count), [&](const hs::CardView &card) {
                return (card.kind == hs::CardKind::LearnSkill &&
                        card.subject == profile.focus_skill) ||
                       ((card.kind == hs::CardKind::SkillUpgrade ||
                         card.kind == hs::CardKind::BasicUpgrade) &&
                        card.subject == profile.focus_skill &&
                        card.upgrade / 4 == profile.variant);
            });
        if (probe.phase == hs::SessionPhase::CardSelection &&
            focus_incomplete && (profile.focus_skill == 0 || focus_owned) &&
            !has_preferred_focus && probe.level_rerolls_remaining != 0)
        {
            if (auto result = simulation.ApplyDebugCommand(
                    {hs::DebugCommandKind::Reroll});
                !result)
                throw std::runtime_error(std::string(result.Message()));
            continue;
        }

        std::size_t selected{};
        auto best_score = std::uint64_t{};
        for (std::size_t index = 0; index < probe.card_count; ++index)
        {
            const auto score = CardScore(probe.cards[index], profile, seed, probe);
            if (score > best_score)
            {
                best_score = score;
                selected = index;
            }
        }
        const auto card = probe.cards[selected];
        acquisitions.push_back({probe.tick, card,
            card.subject < hs::kCombatSkillCount
                ? probe.balance.skill_uses[card.subject] : 0});
        if (auto result = simulation.ApplyDebugCommand(
                {hs::DebugCommandKind::SelectCard, selected});
            !result)
            throw std::runtime_error(std::string(result.Message()));
    }
    throw std::runtime_error("Progression choice resolution did not converge.");
}

CombatBuild FinalBuild(const ProgressionDraftProfile &profile, const hs::SessionProbe &probe)
{
    CombatBuild build;
    build.id = profile.id;
    for (const auto skill : probe.skill_loadout)
        if (skill != hs::SkillKind::Count)
            build.skills.push_back(static_cast<std::uint8_t>(skill));
    build.upgrade_masks = probe.upgrade_masks;
    for (std::size_t relic = 0; relic < hs::kRelicCount; ++relic)
        if ((probe.relic_mask & (1u << relic)) != 0)
            build.relics.push_back(static_cast<std::uint8_t>(relic));
    build.stats = probe.stat_points;
    return build;
}

Json CharacteristicMetrics(std::span<const std::uint64_t> effects)
{
    Json result = Json::object();
    const auto add = [&](std::string_view id, hs::UpgradeEffectMetric metric,
                         double divisor = 1.0) {
        const auto value = effects[static_cast<std::size_t>(metric)];
        if (value != 0) result[std::string(id)] = value / divisor;
    };
    add("projectiles_created", hs::UpgradeEffectMetric::ProjectilesCreated);
    add("areas_created", hs::UpgradeEffectMetric::AreasCreated);
    add("explosions_created", hs::UpgradeEffectMetric::ExplosionsCreated);
    add("bleed_stacks_applied", hs::UpgradeEffectMetric::BleedStacksApplied);
    add("burn_applications", hs::UpgradeEffectMetric::BurnApplications);
    add("slow_applications", hs::UpgradeEffectMetric::SlowApplications);
    add("slow_target_seconds", hs::UpgradeEffectMetric::SlowTargetTicks, 60.0);
    add("bleed_stack_seconds", hs::UpgradeEffectMetric::BleedActiveTicks, 60.0);
    add("burn_target_seconds", hs::UpgradeEffectMetric::BurnActiveTicks, 60.0);
    add("slow_stack_seconds", hs::UpgradeEffectMetric::SlowActiveTicks, 60.0);
    add("cooldown_seconds_saved", hs::UpgradeEffectMetric::CooldownTicksSaved, 60.0);
    add("healing", hs::UpgradeEffectMetric::Healing);
    add("displacement_metres", hs::UpgradeEffectMetric::DisplacementMillimetres,
        1000.0);
    add("extra_targets_hit", hs::UpgradeEffectMetric::ExtraTargetsHit);
    add("extra_bounces", hs::UpgradeEffectMetric::ExtraBounces);
    add("charge_seconds_saved", hs::UpgradeEffectMetric::ChargeTicksSaved, 60.0);
    add("duration_seconds_added", hs::UpgradeEffectMetric::DurationTicksAdded, 60.0);
    add("marks_applied", hs::UpgradeEffectMetric::MarksApplied);
    add("kills", hs::UpgradeEffectMetric::Kills);
    add("damage_amplified", hs::UpgradeEffectMetric::DamageAmplified);
    add("activations", hs::UpgradeEffectMetric::Activations);
    return result;
}

Json CharacteristicMetrics(const hs::BalanceTelemetry &balance,
                           std::size_t skill, std::size_t upgrade)
{
    return CharacteristicMetrics(balance.upgrade_effects[skill][upgrade]);
}

Json RunProgression(const ProgressionDraftProfile &profile, std::uint64_t seed,
                    hs::Tick maximum_ticks, const hs::SimulationRules &rules,
                    CombatBuild &final_build)
{
    hs::GameSimulation simulation;
    hs::SimulationConfig config{seed};
    config.scenario = {.player_stationary = true, .player_invulnerable = true,
                       .progression_enabled = true,
                       .auto_collect_progression = true};
    if (auto initialized = simulation.Initialize(config, rules); !initialized)
        throw std::runtime_error(std::string(initialized.Message()));

    hs::RenderSnapshotStorage snapshot(20'000, 2, 2, 128);
    CombatControlState control;
    std::vector<Acquisition> acquisitions;
    std::vector<hs::SimulationObservation> checkpoints{simulation.GetObservation()};
    for (hs::Tick target_tick = 1; target_tick <= maximum_ticks; ++target_tick)
    {
        ResolveProgressionChoices(simulation, profile, seed, acquisitions);
        const auto before = simulation.GetObservation();
        if (before.phase == hs::SessionPhase::Victory) break;
        snapshot.Clear();
        if (!WriteSnapshot(simulation, snapshot))
            throw std::runtime_error("Progression target snapshot capacity was exceeded.");
        const auto input = MakeCombatInput(before, snapshot.View(), target_tick, control);
        (void)simulation.TickFixed(input, hs::FixedStepClock::kFixedStep);
        simulation.ClearDomainSignals();
        const auto after = simulation.GetObservation();
        constexpr std::array phase_boundaries{hs::Tick{300 * 60},
                                               hs::Tick{600 * 60},
                                               hs::Tick{900 * 60}};
        if (std::ranges::any_of(phase_boundaries, [&](hs::Tick boundary) {
                return checkpoints.back().growth_ticks < boundary &&
                       after.growth_ticks >= boundary;
            }))
            checkpoints.push_back(after);
    }
    ResolveProgressionChoices(simulation, profile, seed, acquisitions);
    const auto probe = simulation.GetObservation();
    if (probe.player_position.x != 0.0f || probe.player_position.y != 0.0f ||
        probe.health != probe.max_health)
        throw std::runtime_error("Stationary progression contract was violated.");
    if (probe.stat_points[static_cast<std::size_t>(hs::StatKind::MaxHealth)] != 0 ||
        probe.stat_points[static_cast<std::size_t>(hs::StatKind::MoveSpeed)] != 0 ||
        probe.stat_points[static_cast<std::size_t>(hs::StatKind::MagnetRadius)] != 0)
        throw std::runtime_error("Progression selected an excluded utility stat.");
    final_build = FinalBuild(profile, probe);
    if (checkpoints.back().tick != probe.tick) checkpoints.push_back(probe);

    Json upgrades = Json::array();
    for (const auto &acquisition : acquisitions)
    {
        if (acquisition.card.kind != hs::CardKind::SkillUpgrade &&
            acquisition.card.kind != hs::CardKind::BasicUpgrade)
            continue;
        const auto skill = acquisition.card.subject;
        const auto upgrade = acquisition.card.upgrade;
        const auto active_seconds = std::max(
            1.0 / 60.0, static_cast<double>(probe.tick - acquisition.tick) / 60.0);
        const auto damage = probe.balance.upgrade_damage[skill][upgrade];
        const auto uses = probe.balance.skill_uses[skill] -
                          acquisition.owning_skill_uses;
        const auto triggers = probe.balance.upgrade_triggers[skill][upgrade];
        upgrades.push_back(
            {{"skill", kCombatSuiteSkillIds[skill]}, {"ordinal", upgrade + 1},
             {"acquired_tick", acquisition.tick}, {"active_seconds", active_seconds},
             {"owning_skill_uses", uses}, {"damage", damage},
             {"damage_dps", damage / active_seconds}, {"damage_triggers", triggers},
             {"damage_per_trigger", triggers == 0 ? 0.0 :
                  static_cast<double>(damage) / triggers},
             {"characteristic_metrics",
              CharacteristicMetrics(probe.balance, skill, upgrade)}});
    }
    Json relics = Json::array();
    for (std::size_t relic = 0; relic < hs::kRelicCount; ++relic)
        if ((probe.relic_mask & (1u << relic)) != 0)
        {
            const auto acquired = std::ranges::find_if(
                acquisitions, [&](const Acquisition &candidate) {
                    return candidate.card.kind == hs::CardKind::Relic &&
                           candidate.card.subject == relic;
                });
            const auto acquired_tick = acquired == acquisitions.end()
                                           ? hs::Tick{}
                                           : acquired->tick;
            const auto active_seconds = std::max(
                1.0 / 60.0,
                static_cast<double>(probe.tick - acquired_tick) / 60.0);
            relics.push_back(
                {{"id", kCombatSuiteRelicIds[relic]},
                 {"acquired_tick", acquired_tick},
                 {"active_seconds", active_seconds},
                 {"damage", probe.balance.relic_damage[relic]},
                 {"damage_dps", probe.balance.relic_damage[relic] / active_seconds},
                 {"damage_events", probe.balance.relic_triggers[relic]},
                 {"characteristic_metrics",
                  CharacteristicMetrics(probe.balance.relic_effects[relic])}});
        }
    Json synergies = Json::array();
    for (std::size_t index = 0;
         index < probe.balance.upgrade_relic_synergy_count; ++index)
    {
        const auto &entry = probe.balance.upgrade_relic_synergies[index];
        Json effects = Json::object();
        for (std::size_t metric = 0;
             metric < hs::kUpgradeRelicSynergyMetricCount; ++metric)
            if (entry.metrics[metric] != 0)
                effects[hs::kUpgradeRelicSynergyMetricIds[metric]] = entry.metrics[metric];
        synergies.push_back(
            {{"skill", kCombatSuiteSkillIds[static_cast<std::size_t>(entry.skill)]},
             {"ordinal", entry.upgrade + 1},
             {"relic", kCombatSuiteRelicIds[static_cast<std::size_t>(entry.relic)]},
             {"effects", std::move(effects)}});
    }
    Json situations = Json::array();
    for (std::size_t phase = 1; phase < checkpoints.size(); ++phase)
    {
        const auto &before = checkpoints[phase - 1];
        const auto &after = checkpoints[phase];
        const auto seconds = std::max(
            1.0 / 60.0, static_cast<double>(after.tick - before.tick) / 60.0);
        const auto label = before.growth_ticks < 300 * 60 ? "early_0_5m" :
                           before.growth_ticks < 600 * 60 ? "mid_5_10m" :
                           before.growth_ticks < 900 * 60 ? "late_10_15m" :
                                                            "boss_fight";
        Json phase_upgrades = Json::array();
        for (std::size_t skill = 0; skill < hs::kCombatSkillCount; ++skill)
            for (std::size_t upgrade = 0; upgrade < hs::kUpgradeCount; ++upgrade)
            {
                const auto damage = after.balance.upgrade_damage[skill][upgrade] -
                                    before.balance.upgrade_damage[skill][upgrade];
                Json effects = Json::object();
                for (std::size_t metric = 0;
                     metric < hs::kUpgradeEffectMetricCount; ++metric)
                {
                    const auto value = after.balance.upgrade_effects[skill][upgrade][metric] -
                                       before.balance.upgrade_effects[skill][upgrade][metric];
                    if (value != 0) effects[hs::kUpgradeEffectMetricIds[metric]] = value;
                }
                if (damage == 0 && effects.empty()) continue;
                phase_upgrades.push_back(
                    {{"skill", kCombatSuiteSkillIds[skill]}, {"ordinal", upgrade + 1},
                     {"direct_damage", damage}, {"direct_dps", damage / seconds},
                     {"effects", std::move(effects)}});
            }
        situations.push_back(
            {{"id", label}, {"start_tick", before.tick}, {"end_tick", after.tick},
             {"seconds", seconds}, {"upgrades", std::move(phase_upgrades)}});
    }
    const auto elapsed_seconds = std::max(1.0, static_cast<double>(probe.tick) / 60.0);
    Json result{{"profile_id", profile.id}, {"seed", seed},
                {"focus_skill", kCombatSuiteSkillIds[profile.focus_skill]},
                {"focus_upgrade_half", profile.variant == 0 ? "1-4" : "5-8"},
                {"outcome", probe.phase == hs::SessionPhase::Victory ? "victory" :
                            probe.phase == hs::SessionPhase::Defeat ? "defeat" : "incomplete"},
                {"ticks", probe.tick}, {"level", probe.level}, {"kills", probe.kills},
                {"seconds", elapsed_seconds},
                {"damage", probe.damage_dealt}, {"dps", probe.damage_dealt / elapsed_seconds},
                {"incoming_damage", probe.damage_taken},
                {"skill_uses", probe.balance.skill_uses},
                {"skill_damage", probe.damage_by_skill},
                {"final_build", BuildDefinitionJson(final_build)},
                {"upgrades", std::move(upgrades)}, {"relics", std::move(relics)},
                {"situations", std::move(situations)},
                {"upgrade_relic_synergies", std::move(synergies)},
                {"checksum", simulation.ComputeChecksum()}};
    if (auto shutdown = simulation.Shutdown(); !shutdown)
        throw std::runtime_error(std::string(shutdown.Message()));
    return result;
}

Json AggregateProgression(const Json &runs)
{
    Json skills = Json::array();
    for (std::size_t skill = 0; skill < hs::kCombatSkillCount; ++skill)
    {
        double damage{}, uses{}, dps{}, selected_runs{};
        for (const auto &run : runs)
        {
            damage += run.at("skill_damage").at(skill).get<double>();
            uses += run.at("skill_uses").at(skill).get<double>();
            dps += run.at("skill_damage").at(skill).get<double>() /
                   run.at("seconds").get<double>();
            const auto &selected_skills = run.at("final_build").at("skills");
            if (skill == 0 || std::ranges::any_of(selected_skills,
                    [&](const Json &selected) {
                        return selected.get<std::string>() == kCombatSuiteSkillIds[skill];
                    }))
                ++selected_runs;
        }
        skills.push_back({{"id", kCombatSuiteSkillIds[skill]}, {"selected_runs", selected_runs},
                          {"total_damage", damage}, {"total_uses", uses},
                          {"mean_dps", dps / runs.size()},
                          {"damage_per_use", uses == 0.0 ? 0.0 : damage / uses}});
    }

    Json upgrades = Json::array();
    bool coverage_complete = true;
    for (std::size_t skill = 0; skill < hs::kCombatSkillCount; ++skill)
    {
        for (std::size_t upgrade = 0; upgrade < hs::kUpgradeCount; ++upgrade)
        {
            double actual_damage{}, damage_dps_sum{}, active_seconds{}, uses{}, triggers{};
            std::size_t selected_runs{}, observed_runs{};
            Json characteristic_totals = Json::object();
            for (const auto &run : runs)
            {
                for (const auto &metric : run.at("upgrades"))
                {
                    if (metric.at("skill").get<std::string>() != kCombatSuiteSkillIds[skill] ||
                        metric.at("ordinal").get<std::size_t>() != upgrade + 1)
                        continue;
                    ++selected_runs;
                    actual_damage += metric.at("damage").get<double>();
                    damage_dps_sum += metric.at("damage_dps").get<double>();
                    active_seconds += metric.at("active_seconds").get<double>();
                    uses += metric.at("owning_skill_uses").get<double>();
                    triggers += metric.at("damage_triggers").get<double>();
                    for (const auto &[id, value] :
                         metric.at("characteristic_metrics").items())
                        characteristic_totals[id] =
                            characteristic_totals.value(id, 0.0) + value.get<double>();
                    if (metric.at("damage").get<double>() != 0.0 ||
                        !metric.at("characteristic_metrics").empty())
                        ++observed_runs;
                }
            }
            coverage_complete &= selected_runs != 0;
            upgrades.push_back(
                {{"skill", kCombatSuiteSkillIds[skill]}, {"ordinal", upgrade + 1},
                 {"selected_runs", selected_runs},
                 {"effect_observed_runs", observed_runs},
                 {"direct_damage", actual_damage},
                 {"mean_session_direct_dps", selected_runs == 0 ? 0.0 :
                      damage_dps_sum / selected_runs},
                 {"active_seconds", active_seconds},
                 {"owning_skill_uses", uses},
                 {"damage_triggers", triggers},
                 {"direct_damage_per_use", uses == 0.0 ? 0.0 : actual_damage / uses},
                 {"triggers_per_use", uses == 0.0 ? 0.0 : triggers / uses},
                 {"damage_per_trigger", triggers == 0.0 ? 0.0 :
                      actual_damage / triggers},
                 {"characteristic_totals", std::move(characteristic_totals)}});
        }
    }

    Json relics = Json::array();
    for (std::size_t relic = 0; relic < hs::kRelicCount; ++relic)
    {
        double damage{}, damage_dps{}, damage_events{}, active_seconds{};
        std::size_t acquired_runs{};
        Json characteristic_totals = Json::object();
        for (const auto &run : runs)
            for (const auto &metric : run.at("relics"))
                if (metric.at("id").get<std::string>() == kCombatSuiteRelicIds[relic])
                {
                    ++acquired_runs;
                    damage += metric.at("damage").get<double>();
                    damage_dps += metric.at("damage_dps").get<double>();
                    damage_events += metric.at("damage_events").get<double>();
                    active_seconds += metric.at("active_seconds").get<double>();
                    for (const auto &[id, value] :
                         metric.at("characteristic_metrics").items())
                        characteristic_totals[id] =
                            characteristic_totals.value(id, 0.0) + value.get<double>();
                }
            relics.push_back({{"id", kCombatSuiteRelicIds[relic]},
                              {"acquired_runs", acquired_runs},
                              {"direct_damage", damage},
                              {"mean_session_direct_dps",
                               acquired_runs == 0 ? 0.0 : damage_dps / acquired_runs},
                              {"active_seconds", active_seconds},
                              {"damage_events", damage_events},
                              {"characteristic_totals",
                               std::move(characteristic_totals)}});
    }
    std::map<std::string, Json> synergy_groups;
    for (const auto &run : runs)
        for (const auto &synergy : run.at("upgrade_relic_synergies"))
        {
            const auto key = synergy.at("skill").get<std::string>() + ':' +
                std::to_string(synergy.at("ordinal").get<unsigned>()) + '|' +
                synergy.at("relic").get<std::string>();
            auto &group = synergy_groups[key];
            if (group.is_null())
                group = {{"skill", synergy.at("skill")},
                         {"ordinal", synergy.at("ordinal")},
                         {"relic", synergy.at("relic")},
                         {"observed_runs", 0},
                         {"effects", Json::object()}};
            group["observed_runs"] = group.at("observed_runs").get<std::size_t>() + 1;
            for (const auto &[id, value] : synergy.at("effects").items())
                group["effects"][id] =
                    group["effects"].value(id, std::uint64_t{}) +
                    value.get<std::uint64_t>();
        }
    Json synergies = Json::array();
    for (auto &[key, group] : synergy_groups)
    {
        (void)key;
        synergies.push_back(std::move(group));
    }
    return {{"upgrade_coverage_complete", coverage_complete},
             {"skills", std::move(skills)}, {"upgrades", std::move(upgrades)},
            {"relics", std::move(relics)},
            {"upgrade_relic_synergies", std::move(synergies)}};
}

Json Aggregate(const CombatSimulationSuite &suite, const Json &runs)
{
    Json aggregates = Json::array();
    for (const auto &build : suite.builds)
    {
        double damage{}, dps{}, kills{}, incoming{}, cpu_p95{}, efficiency{};
        std::size_t count{};
        for (const auto &run : runs)
        {
            if (run.at("build_id") != build.id) continue;
            damage += run.at("damage").get<double>();
            dps += run.at("dps").get<double>();
            kills += run.at("kills").get<double>();
            incoming += run.at("incoming_damage").get<double>();
            cpu_p95 += run.at("cpu_tick_p95_us").get<double>();
            efficiency += run.at("damage_per_cpu_ms").get<double>();
            ++count;
        }
        aggregates.push_back({{"build_id", build.id},
                              {"runs", count},
                              {"mean_damage", damage / count},
                              {"mean_dps", dps / count},
                              {"mean_kills", kills / count},
                              {"mean_incoming_damage", incoming / count},
                              {"mean_cpu_tick_p95_us", cpu_p95 / count},
                              {"mean_damage_per_cpu_ms", efficiency / count}});
    }
    const auto baseline_dps = aggregates.front().at("mean_dps").get<double>();
    const auto baseline_kills = aggregates.front().at("mean_kills").get<double>();
    const auto baseline_cpu = aggregates.front().at("mean_cpu_tick_p95_us").get<double>();
    for (auto &aggregate : aggregates)
    {
        const auto delta = [](double value, double baseline) {
            return baseline == 0.0 ? 0.0 : (value / baseline - 1.0) * 100.0;
        };
        aggregate["dps_delta_percent"] =
            delta(aggregate.at("mean_dps").get<double>(), baseline_dps);
        aggregate["kills_delta_percent"] =
            delta(aggregate.at("mean_kills").get<double>(), baseline_kills);
        aggregate["cpu_p95_delta_percent"] =
            delta(aggregate.at("mean_cpu_tick_p95_us").get<double>(), baseline_cpu);
    }
    return aggregates;
}

void ValidatePairedWorkloads(const CombatSimulationSuite &suite, const Json &runs)
{
    for (const auto seed : suite.seeds)
    {
        const Json *expected{};
        for (const auto &run : runs)
        {
            if (run.at("seed").get<std::uint64_t>() != seed) continue;
            if (!expected) expected = &run.at("enemy_spawned");
            else if (*expected != run.at("enemy_spawned"))
                throw std::runtime_error(
                    "Paired build comparison received different enemy spawn streams.");
        }
    }
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        std::filesystem::path suite_path;
        std::filesystem::path progression_suite_path;
        std::filesystem::path output_directory;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument = argv[index];
            if (argument.starts_with("--suite=")) suite_path = argument.substr(8);
            else if (argument.starts_with("--progression-suite="))
                progression_suite_path = argument.substr(20);
            else if (argument.starts_with("--output=")) output_directory = argument.substr(9);
            else throw std::runtime_error(
                "Usage: hs_combat_sim (--suite=FILE | --progression-suite=FILE) --output=DIR");
        }
        if (output_directory.empty() || suite_path.empty() == progression_suite_path.empty())
            throw std::runtime_error(
                "Usage: hs_combat_sim (--suite=FILE | --progression-suite=FILE) --output=DIR");

        const auto executable = std::filesystem::absolute(argv[0]);
        hs::SimulationRules rules;
        if (auto loaded = hs::LoadSimulationRules(
                executable.parent_path() / "Cooked" / "simulation_rules.hsbin", rules);
            !loaded)
            throw std::runtime_error(std::string(loaded.Message()));
        std::filesystem::create_directories(output_directory);

        if (!progression_suite_path.empty())
        {
            const auto suite = LoadProgressionSimulationSuite(progression_suite_path);
            std::vector<ProgressionDraftProfile> profiles;
            for (std::size_t skill = 0; skill < hs::kCombatSkillCount; ++skill)
                for (std::uint8_t variant = 0; variant < 2; ++variant)
                    profiles.push_back(
                        {std::string(kCombatSuiteSkillIds[skill]) + '-' +
                             (variant == 0 ? "upgrades-1-4" : "upgrades-5-8"),
                         static_cast<std::uint8_t>(skill), variant});

            struct Execution
            {
                Json run;
            };
            struct Request
            {
                std::size_t seed_index{};
                std::size_t profile_index{};
            };
            std::vector<Request> requests;
            for (std::size_t seed_index = 0; seed_index < suite.seeds.size(); ++seed_index)
            {
                for (std::size_t offset = 0; offset < profiles.size(); ++offset)
                {
                    const auto profile_index = (seed_index + offset) % profiles.size();
                    requests.push_back({seed_index, profile_index});
                }
            }
            const auto parallel_workers = std::min<std::size_t>(
                8, std::max<std::size_t>(1, std::thread::hardware_concurrency()));
            Json runs = Json::array();
            for (std::size_t first = 0; first < requests.size();
                 first += parallel_workers)
            {
                std::vector<std::future<Execution>> futures;
                const auto end = std::min(requests.size(), first + parallel_workers);
                for (std::size_t index = first; index < end; ++index)
                {
                    const auto request = requests[index];
                    futures.push_back(std::async(std::launch::async, [&, request] {
                        CombatBuild final_build;
                        auto run = RunProgression(profiles[request.profile_index],
                                                  suite.seeds[request.seed_index],
                                                  suite.maximum_ticks, rules,
                                                  final_build);
                        return Execution{std::move(run)};
                    }));
                }
                for (auto &future : futures)
                {
                    auto execution = future.get();
                    runs.push_back(std::move(execution.run));
                }
            }
            auto aggregate = AggregateProgression(runs);
            const auto coverage_complete =
                aggregate.at("upgrade_coverage_complete").get<bool>();
            Json report{{"schema_version", 1},
                        {"execution_valid", coverage_complete},
                        {"method", "stationary_invulnerable_progression_matrix"},
                        {"simulation_only", true}, {"fixed_tick_hz", 60},
                        {"experience_policy", "instant_automatic_collection"},
                        {"skill_policy", "nearest_enemy_round_robin_varied_charge"},
                        {"draft_policy", "18_skill_and_upgrade_half_focus_profiles"},
                        {"stat_policy", "attack_attack_speed_cooldown_only"},
                        {"excluded_stats", {"maximum_hp", "movement_speed",
                                             "magnet_radius"}},
                        {"duration_ticks", suite.maximum_ticks},
                        {"parallel_workers", parallel_workers},
                        {"seeds", suite.seeds}, {"profile_count", profiles.size()},
                        {"runs", std::move(runs)},
                        {"aggregate", aggregate}};
            std::ofstream(output_directory / "progression_report.json", std::ios::trunc)
                << report.dump(2) << '\n';
            std::ofstream upgrade_csv(output_directory /
                                      "progression_upgrade_summary.csv",
                                      std::ios::trunc);
            upgrade_csv << "skill,ordinal,selected_runs,effect_observed_runs,"
                           "direct_damage,mean_session_direct_dps,uses,triggers,"
                           "direct_damage_per_use,triggers_per_use,damage_per_trigger\n";
            for (const auto &row : aggregate.at("upgrades"))
                upgrade_csv << row.at("skill").get<std::string>() << ','
                            << row.at("ordinal") << ',' << row.at("selected_runs") << ','
                            << row.at("effect_observed_runs") << ','
                            << row.at("direct_damage") << ','
                            << row.at("mean_session_direct_dps") << ','
                            << row.at("owning_skill_uses") << ','
                            << row.at("damage_triggers") << ','
                            << row.at("direct_damage_per_use") << ','
                            << row.at("triggers_per_use") << ','
                            << row.at("damage_per_trigger") << '\n';
            std::ofstream synergy_csv(output_directory /
                                      "progression_synergy_summary.csv",
                                      std::ios::trunc);
            synergy_csv << "skill,ordinal,relic,observed_runs,damage,damage_events,"
                           "activations,healing,burn_applications,slow_applications,"
                           "slow_target_ticks\n";
            for (const auto &row : aggregate.at("upgrade_relic_synergies"))
            {
                const auto &effects = row.at("effects");
                synergy_csv << row.at("skill").get<std::string>() << ','
                            << row.at("ordinal") << ','
                            << row.at("relic").get<std::string>() << ','
                            << row.at("observed_runs");
                for (const auto id : hs::kUpgradeRelicSynergyMetricIds)
                    synergy_csv << ',' << effects.value(std::string(id), 0ull);
                synergy_csv << '\n';
            }
            if (!upgrade_csv || !synergy_csv)
                throw std::runtime_error("Cannot write progression summaries.");
            std::cout << "combat_progression profiles=" << profiles.size()
                      << " seeds=" << suite.seeds.size()
                      << " report="
                      << (output_directory / "progression_report.json").string()
                      << '\n';
            return 0;
        }

        const auto suite = LoadCombatSimulationSuite(suite_path);

        Json runs = Json::array();
        for (std::size_t seed_index = 0; seed_index < suite.seeds.size(); ++seed_index)
        {
            for (std::size_t offset = 0; offset < suite.builds.size(); ++offset)
            {
                const auto build_index = (seed_index + offset) % suite.builds.size();
                runs.push_back(RunCombatBuild(suite.builds[build_index],
                                        suite.seeds[seed_index],
                                        suite.maximum_ticks, rules));
            }
        }
        ValidatePairedWorkloads(suite, runs);
        const auto aggregates = Aggregate(suite, runs);
        Json builds = Json::array();
        for (const auto &build : suite.builds)
            builds.push_back(BuildDefinitionJson(build));
        Json report{{"schema_version", 1},
                    {"execution_valid", true},
                    {"method", "stationary_invulnerable_fixed_tick"},
                    {"aim_policy", "nearest_enemy"},
                    {"active_skill_policy", "round_robin_when_ready"},
                    {"charged_skill_policy", "full_charge"},
                    {"paired_spawn_streams_valid", true},
                    {"fixed_tick_hz", 60},
                    {"duration_ticks", suite.maximum_ticks},
                    {"baseline_build_id", suite.builds.front().id},
                    {"seeds", suite.seeds},
                    {"builds", std::move(builds)},
                    {"runs", runs},
                    {"aggregates", aggregates}};
        std::ofstream(output_directory / "combat_report.json", std::ios::trunc)
            << report.dump(2) << '\n';
        std::ofstream csv(output_directory / "combat_summary.csv", std::ios::trunc);
        csv << "build_id,mean_damage,mean_dps,mean_kills,mean_incoming_damage,"
               "mean_cpu_tick_p95_us,mean_damage_per_cpu_ms,dps_delta_percent,"
               "kills_delta_percent,cpu_p95_delta_percent\n";
        for (const auto &row : aggregates)
            csv << row.at("build_id").get<std::string>() << ','
                << row.at("mean_damage") << ',' << row.at("mean_dps") << ','
                << row.at("mean_kills") << ',' << row.at("mean_incoming_damage") << ','
                << row.at("mean_cpu_tick_p95_us") << ','
                << row.at("mean_damage_per_cpu_ms") << ','
                << row.at("dps_delta_percent") << ','
                << row.at("kills_delta_percent") << ','
                << row.at("cpu_p95_delta_percent") << '\n';
        if (!csv) throw std::runtime_error("Cannot write combat summary.");
        std::cout << "combat_sim builds=" << suite.builds.size()
                  << " seeds=" << suite.seeds.size()
                  << " report=" << (output_directory / "combat_report.json").string()
                  << '\n';
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << "hs_combat_sim: " << exception.what() << '\n';
        return 1;
    }
}
