#include <hs/runtime/playtest_recording.hpp>

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <numeric>
#include <optional>
#include <string_view>

namespace hs
{
namespace
{

using Json = nlohmann::json;

constexpr std::array<std::string_view, kCombatSkillCount> kSkillIds{
    "basic_attack", "piercing_shot", "multi_shot", "charged_shot",
    "explosive_arrow", "ricochet_arrow", "arrow_rain", "trap", "retreat_shot"};
constexpr std::array<std::string_view, kRelicCount> kRelicIds{
    "bleed_kill_heal", "burn_propagation", "kill_cooldown_surge", "bleed_burn_explosion",
    "radial_basic_attack", "basic_kill_tracker", "movement_echo",
    "alternating_skills", "different_skill_tracker", "damage_knockback",
    "once_revive", "combat_hit_chain"};
constexpr std::array<std::string_view, kStatCount> kStatIds{
    "max_health", "move_speed", "attack_power", "attack_speed",
    "cooldown_reduction", "magnet_radius"};
constexpr std::array<std::string_view, kStatCount> kStatUtilityUnits{
    "max_health", "extra_millimetres", "bonus_damage", "attack_ticks_saved",
    "cooldown_ticks_saved", "extra_pickups"};
constexpr std::array<std::string_view, kEnemyArchetypeCount> kEnemyIds{
    "melee", "ranged", "suicide", "boss_5m", "boss_10m", "boss_final"};
constexpr std::array<std::string_view, kPickupKindCount> kPickupIds{
    "experience", "heal", "magnet", "relic_chest"};
constexpr std::array<std::string_view, kUpgradeEffectMetricCount> kUpgradeEffectIds{
    "projectiles_created", "areas_created", "explosions_created",
    "bleed_stacks_applied",
    "burn_applications", "slow_applications", "slow_target_ticks",
    "bleed_active_ticks", "burn_active_ticks", "slow_active_ticks",
    "cooldown_ticks_saved", "healing", "displacement_millimetres",
    "extra_targets_hit", "extra_bounces", "charge_ticks_saved",
    "duration_ticks_added", "marks_applied", "kills", "damage_amplified",
    "activations"};
constexpr std::array<std::string_view, kUpgradeEffectMetricCount> kUpgradeEffectUnits{
    "count", "count", "count", "stacks", "count", "count", "target_ticks",
    "stack_ticks", "target_ticks", "stack_ticks", "ticks", "health",
    "millimetres", "count", "count", "ticks",
    "ticks", "count", "count", "damage", "count"};
constexpr std::array<std::string_view, kUpgradeEffectMetricCount> kUpgradeEffectLabels{
    "추가 투사체", "추가 영역", "추가 폭발", "출혈 중첩 부여", "화상 부여",
    "둔화 부여", "둔화 누적 대상 틱", "출혈 실제 중첩 틱",
    "화상 실제 대상 틱", "둔화 실제 중첩 틱", "쿨타임 단축 틱", "회복량",
    "강제 이동 거리(mm)", "추가 적중 대상", "추가 도탄", "충전 단축 틱",
    "지속시간 증가 틱", "표식 부여", "처치", "추가 피해", "발동"};
constexpr std::array<std::string_view, kUpgradeRelicSynergyMetricCount>
    kSynergyMetricIds{"damage", "damage_events", "activations", "healing",
                      "burn_applications", "slow_applications",
                      "slow_target_ticks"};

double Ratio(std::uint64_t numerator, std::uint64_t denominator)
{
    return denominator == 0 ? 0.0 : static_cast<double>(numerator) /
                                         static_cast<double>(denominator);
}

std::filesystem::path DefaultDirectory(std::uint64_t seed)
{
    std::array<wchar_t, 32'768> local{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(),
                                                 static_cast<DWORD>(local.size()));
    if (length == 0 || length >= local.size()) return {};
    const auto now = std::chrono::system_clock::now();
    const auto stamp = std::chrono::floor<std::chrono::seconds>(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now.time_since_epoch())
                                  .count() % 1000;
    return std::filesystem::path(local.data()) / L"ProjectHS" / L"Playtests" /
           std::format("{:%Y%m%d-%H%M%S}-{:03}-{}-{}", stamp, milliseconds, seed,
                       GetCurrentProcessId());
}

Result Invalid(std::string message)
{
    return Result::Failure(ErrorCode::InvalidArgument, "hs_playtest", std::move(message));
}

Json InputJson(const InputFrame &input, GameplayChecksum checksum)
{
    Json edges = Json::array();
    for (const auto &edge : input.ordered_edges)
    {
        edges.push_back({{"sequence", edge.sequence},
                         {"action", static_cast<std::uint8_t>(edge.action)},
                         {"kind", static_cast<std::uint8_t>(edge.kind)}});
    }
    Json ui_actions = Json::array();
    for (const auto &action : input.ui_actions)
        ui_actions.push_back({{"kind", static_cast<std::uint8_t>(action.kind)},
                              {"value", action.value},
                              {"secondary", action.secondary}});
    return {{"target_tick", input.target_tick},
            {"move_target", {input.held.move_target_world.x,
                              input.held.move_target_world.y,
                              input.held.move_target_world.z}},
            {"aim_world", {input.held.aim_world.x, input.held.aim_world.y,
                            input.held.aim_world.z}},
            {"cursor_normalized", {input.held.cursor_normalized.x,
                                    input.held.cursor_normalized.y}},
            {"move_held", input.held.move_held},
            {"basic_attack_held", input.held.basic_attack_held},
            {"edges", std::move(edges)},
            {"ui_actions", std::move(ui_actions)},
            {"checksum", checksum}};
}

template <std::size_t Size>
Json ByteArray(const std::array<std::uint8_t, Size> &values)
{
    Json result = Json::array();
    for (const auto value : values) result.push_back(value);
    return result;
}

Json SkillLoadout(const std::array<SkillKind, 4> &loadout)
{
    Json result = Json::array();
    for (const auto skill : loadout)
        result.push_back(skill < SkillKind::Count
                             ? Json(kSkillIds[static_cast<std::size_t>(skill)])
                             : Json(nullptr));
    return result;
}

} // namespace

struct PlaytestRecorder::Impl
{
    std::filesystem::path directory;
    std::ofstream inputs;
    std::ofstream timeline;
    std::ofstream events;
    PlaytestRecorderConfig config;
    SimulationObservation last_probe;
    std::optional<SimulationObservation> terminal_probe;
    Float2 previous_position{};
    std::array<std::uint64_t, 4> skill_press_counts{};
    std::uint64_t frame_count{};
    std::uint64_t near_death_ticks{};
    std::uint64_t maximum_damage_taken_in_tick{};
    std::uint64_t previous_damage_taken{};
    std::uint32_t minimum_health{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t peak_enemies{};
    std::uint32_t peak_projectiles{};
    std::uint32_t peak_pickups{};
    std::uint64_t first_kill_tick{};
    std::array<std::uint64_t, 10> phase_ticks{};
    std::array<std::array<Tick, kUpgradeCount>, kCombatSkillCount>
        upgrade_acquired_ticks{};
    std::array<std::array<std::uint64_t, kUpgradeCount>, kCombatSkillCount>
        upgrade_uses_at_acquisition{};
    std::array<Tick, kRelicCount> relic_acquired_ticks{};
    double distance_moved{};
    bool active{};
    bool finished{};
};

PlaytestRecorder::PlaytestRecorder() : impl_(std::make_unique<Impl>()) {}
PlaytestRecorder::~PlaytestRecorder() = default;

Result PlaytestRecorder::Start(const PlaytestRecorderConfig &config)
{
    if (impl_->active) return Invalid("Recorder is already active.");
    impl_->config = config;
    impl_->directory = config.output_directory.empty()
                           ? DefaultDirectory(config.seed)
                           : config.output_directory;
    if (impl_->directory.empty())
        return Result::Failure(ErrorCode::InvalidState, "hs_playtest",
                               "LOCALAPPDATA is unavailable and no output directory was set.");
    std::error_code error;
    std::filesystem::create_directories(impl_->directory, error);
    if (error)
        return Result::Failure(ErrorCode::InvalidState, "hs_playtest", error.message());

    impl_->inputs.open(impl_->directory / "inputs.ndjson", std::ios::trunc);
    impl_->timeline.open(impl_->directory / "timeline.csv", std::ios::trunc);
    impl_->events.open(impl_->directory / "events.ndjson", std::ios::trunc);
    if (!impl_->inputs || !impl_->timeline || !impl_->events)
        return Result::Failure(ErrorCode::InvalidState, "hs_playtest",
                               "Cannot create playtest trace files.");

    std::ofstream(impl_->directory / "metadata.json", std::ios::trunc)
        << Json{{"format_version", 4},
                {"simulation_version", kSimulationVersion},
                {"gameplay_hash_version", kGameplayHashVersion},
                {"simulation_rules_hash", config.simulation_rules_hash},
                {"content_source_hash", config.content_source_hash},
                {"tick_rate", 60},
                {"determinism_profile", kDeterminismProfile},
                {"seed", config.seed}}
               .dump(2)
        << '\n';
    impl_->timeline
        << "tick,growth_tick,boss_tick,phase,level,xp,hp,max_hp,enemies,bosses,"
           "player_projectiles,enemy_projectiles,pickups,kills,damage_dealt,"
           "damage_taken,healing,x,z,checksum\n";
    impl_->active = true;
    impl_->terminal_probe.reset();
    for (auto &skill : impl_->upgrade_acquired_ticks)
        skill.fill(std::numeric_limits<Tick>::max());
    impl_->relic_acquired_ticks.fill(std::numeric_limits<Tick>::max());
    return Result::Success();
}

Result PlaytestRecorder::Record(const InputFrame &input, GameplayChecksum checksum,
                                const SimulationObservation &probe,
                                std::span<const PresentationEvent> presentation_events)
{
    if (!impl_->active || impl_->finished) return Result::Success();
    for (std::size_t skill = 0; skill < kCombatSkillCount; ++skill)
        for (std::size_t upgrade = 0; upgrade < kUpgradeCount; ++upgrade)
            if ((probe.upgrade_masks[skill] & (1u << upgrade)) != 0 &&
                impl_->upgrade_acquired_ticks[skill][upgrade] ==
                    std::numeric_limits<Tick>::max())
            {
                impl_->upgrade_acquired_ticks[skill][upgrade] = probe.tick;
                impl_->upgrade_uses_at_acquisition[skill][upgrade] =
                    probe.balance.skill_uses[skill];
            }
    for (std::size_t relic = 0; relic < kRelicCount; ++relic)
        if ((probe.relic_mask & (1u << relic)) != 0 &&
            impl_->relic_acquired_ticks[relic] == std::numeric_limits<Tick>::max())
            impl_->relic_acquired_ticks[relic] = probe.tick;
    impl_->inputs << InputJson(input, checksum).dump() << '\n';
    impl_->timeline << probe.tick << ',' << probe.growth_ticks << ','
                    << probe.boss_fight_ticks << ','
                    << static_cast<unsigned>(probe.phase) << ',' << probe.level << ','
                    << probe.experience << ',' << probe.health << ',' << probe.max_health << ','
                    << probe.normal_enemy_count << ',' << probe.boss_count << ','
                    << probe.player_projectile_count << ',' << probe.enemy_projectile_count << ','
                    << probe.pickup_count << ',' << probe.kills << ',' << probe.damage_dealt << ','
                    << probe.damage_taken << ',' << probe.healing << ','
                    << probe.player_position.x << ',' << probe.player_position.y << ','
                    << checksum << '\n';
    for (const auto &event : presentation_events)
    {
        impl_->events << Json{{"sequence", event.sequence}, {"tick", event.tick},
                              {"kind", static_cast<std::uint8_t>(event.kind)},
                              {"position", {event.position.x, event.position.y,
                                            event.position.z}},
                              {"asset_id", event.asset.value}}
                                 .dump()
                      << '\n';
    }

    for (const auto &edge : input.ordered_edges)
    {
        if (edge.kind != EdgeKind::Pressed) continue;
        if (edge.action >= GameAction::SkillQ && edge.action <= GameAction::SkillR)
            ++impl_->skill_press_counts[static_cast<std::size_t>(edge.action) -
                                        static_cast<std::size_t>(GameAction::SkillQ)];
    }
    if (impl_->frame_count != 0)
    {
        const auto dx = probe.player_position.x - impl_->previous_position.x;
        const auto dz = probe.player_position.y - impl_->previous_position.y;
        impl_->distance_moved += std::sqrt(dx * dx + dz * dz);
    }
    impl_->previous_position = probe.player_position;
    impl_->minimum_health = std::min(impl_->minimum_health,
        static_cast<std::uint32_t>(std::max(probe.health, 0)));
    impl_->peak_enemies = std::max(impl_->peak_enemies,
                                  probe.normal_enemy_count + probe.boss_count);
    impl_->peak_projectiles = std::max(
        impl_->peak_projectiles,
        probe.player_projectile_count + probe.enemy_projectile_count);
    impl_->peak_pickups = std::max(impl_->peak_pickups, probe.pickup_count);
    if (probe.max_health > 0 && probe.health * 4 <= probe.max_health)
        ++impl_->near_death_ticks;
    ++impl_->phase_ticks[static_cast<std::size_t>(probe.phase)];
    if (impl_->first_kill_tick == 0 && probe.kills != 0)
        impl_->first_kill_tick = probe.tick;
    const auto damage_delta = probe.damage_taken - impl_->previous_damage_taken;
    impl_->maximum_damage_taken_in_tick = std::max(
        impl_->maximum_damage_taken_in_tick, damage_delta);
    impl_->previous_damage_taken = probe.damage_taken;
    impl_->last_probe = probe;
    if (!impl_->terminal_probe &&
        (probe.phase == SessionPhase::Victory || probe.phase == SessionPhase::Defeat))
        impl_->terminal_probe = probe;
    ++impl_->frame_count;
    if (impl_->frame_count % 60 == 0)
    {
        impl_->inputs.flush();
        impl_->timeline.flush();
        impl_->events.flush();
    }
    if (!impl_->inputs || !impl_->timeline || !impl_->events)
        return Result::Failure(ErrorCode::InvalidState, "hs_playtest",
                               "Writing the playtest trace failed.");
    return Result::Success();
}

Result PlaytestRecorder::Finish(bool execution_valid)
{
    if (!impl_->active || impl_->finished) return Result::Success();
    impl_->finished = true;
    impl_->inputs.flush();
    impl_->timeline.flush();
    impl_->events.flush();

    const auto &probe = impl_->terminal_probe ? *impl_->terminal_probe
                                              : impl_->last_probe;
    Json skill_metrics = Json::array();
    Json upgrade_metrics = Json::array();
    for (std::size_t skill = 0; skill < kCombatSkillCount; ++skill)
    {
        const auto uses = probe.balance.skill_uses[skill];
        Json selected_upgrades = Json::array();
        for (std::size_t upgrade = 0; upgrade < kUpgradeCount; ++upgrade)
        {
            Json effects = Json::object();
            for (std::size_t effect = 0; effect < kUpgradeEffectMetricCount; ++effect)
                effects[kUpgradeEffectIds[effect]] =
                    {{"value", probe.balance.upgrade_effects[skill][upgrade][effect]},
                     {"unit", kUpgradeEffectUnits[effect]}};
            Json metric{{"skill", kSkillIds[skill]}, {"upgrade", upgrade + 1},
                        {"selected", (probe.upgrade_masks[skill] &
                                      (1u << upgrade)) != 0},
                        {"damage", probe.balance.upgrade_damage[skill][upgrade]},
                        {"damage_events",
                         probe.balance.upgrade_triggers[skill][upgrade]},
                        {"damage_triggers",
                          probe.balance.upgrade_triggers[skill][upgrade]},
                        {"effects", std::move(effects)}};
            const auto acquired_tick = impl_->upgrade_acquired_ticks[skill][upgrade];
            if (acquired_tick != std::numeric_limits<Tick>::max())
            {
                const auto active_seconds = std::max(
                    1.0 / 60.0,
                    static_cast<double>(probe.tick - acquired_tick) / 60.0);
                const auto owning_skill_uses = probe.balance.skill_uses[skill] -
                    impl_->upgrade_uses_at_acquisition[skill][upgrade];
                metric["acquired_tick"] = acquired_tick;
                metric["active_seconds"] = active_seconds;
                metric["owning_skill_uses"] = owning_skill_uses;
                metric["direct_dps"] =
                    probe.balance.upgrade_damage[skill][upgrade] / active_seconds;
                metric["direct_damage_per_use"] = owning_skill_uses == 0 ? 0.0 :
                    static_cast<double>(probe.balance.upgrade_damage[skill][upgrade]) /
                        owning_skill_uses;
            }
            upgrade_metrics.push_back(metric);
            if (metric["selected"].get<bool>()) selected_upgrades.push_back(metric);
        }
        const auto boss_damage = probe.balance.skill_boss_damage[skill];
        skill_metrics.push_back(
            {{"id", kSkillIds[skill]}, {"level", probe.skill_levels[skill]},
             {"damage", probe.damage_by_skill[skill]}, {"uses", uses},
             {"damage_to_normal", probe.damage_by_skill[skill] -
                                      std::min(probe.damage_by_skill[skill], boss_damage)},
             {"damage_to_boss", boss_damage},
             {"casts_with_hit", probe.balance.skill_casts_with_hit[skill]},
             {"hit_events", probe.balance.skill_hit_events[skill]},
             {"cast_hit_rate", Ratio(probe.balance.skill_casts_with_hit[skill], uses)},
             {"kills", probe.balance.skill_kills[skill]},
             {"upgrades", std::move(selected_upgrades)}});
    }
    Json relic_metrics = Json::array();
    for (std::size_t index = 0; index < kRelicCount; ++index)
    {
        Json effects = Json::object();
        for (std::size_t effect = 0; effect < kUpgradeEffectMetricCount; ++effect)
            effects[kUpgradeEffectIds[effect]] =
                {{"value", probe.balance.relic_effects[index][effect]},
                 {"unit", kUpgradeEffectUnits[effect]}};
        Json metric{{"id", kRelicIds[index]},
                    {"acquired", (probe.relic_mask & (1u << index)) != 0},
                    {"damage", probe.balance.relic_damage[index]},
                    {"triggers", probe.balance.relic_triggers[index]},
                    {"damage_events", probe.balance.relic_triggers[index]},
                    {"effects", std::move(effects)}};
        if (impl_->relic_acquired_ticks[index] != std::numeric_limits<Tick>::max())
        {
            const auto acquired_tick = impl_->relic_acquired_ticks[index];
            const auto active_seconds = std::max(
                1.0 / 60.0,
                static_cast<double>(probe.tick - acquired_tick) / 60.0);
            metric["acquired_tick"] = acquired_tick;
            metric["active_seconds"] = active_seconds;
            metric["damage_dps"] = probe.balance.relic_damage[index] / active_seconds;
        }
        relic_metrics.push_back(std::move(metric));
    }
    Json synergy_metrics = Json::array();
    for (std::size_t index = 0;
         index < probe.balance.upgrade_relic_synergy_count; ++index)
    {
        const auto &entry = probe.balance.upgrade_relic_synergies[index];
        Json effects = Json::object();
        for (std::size_t metric = 0;
             metric < kUpgradeRelicSynergyMetricCount; ++metric)
            if (entry.metrics[metric] != 0)
                effects[kSynergyMetricIds[metric]] = entry.metrics[metric];
        synergy_metrics.push_back(
            {{"skill", kSkillIds[static_cast<std::size_t>(entry.skill)]},
             {"upgrade", entry.upgrade + 1},
             {"relic", kRelicIds[static_cast<std::size_t>(entry.relic)]},
             {"effects", std::move(effects)}});
    }
    Json enemy_metrics = Json::array();
    for (std::size_t index = 0; index < kEnemyArchetypeCount; ++index)
    {
        const auto spawned = probe.balance.enemy_spawned[index];
        const auto killed = probe.balance.enemy_killed[index];
        const auto alive = spawned - std::min(spawned, killed);
        enemy_metrics.push_back(
            {{"id", kEnemyIds[index]}, {"spawned", spawned}, {"killed", killed},
             {"alive", alive}, {"survival_rate", Ratio(alive, spawned)},
             {"average_lifetime_seconds", killed == 0 ? 0.0 :
                  static_cast<double>(probe.balance.enemy_lifetime_ticks[index]) /
                      (60.0 * static_cast<double>(killed))},
             {"attack_attempts", probe.balance.enemy_attack_attempts[index]},
             {"hits", probe.balance.enemy_hits[index]},
             {"hit_rate", Ratio(probe.balance.enemy_hits[index],
                                 probe.balance.enemy_attack_attempts[index])},
             {"damage", probe.balance.enemy_damage[index]}});
    }
    Json pickup_metrics = Json::array();
    for (std::size_t index = 0; index < kPickupKindCount; ++index)
        pickup_metrics.push_back(
            {{"id", kPickupIds[index]},
             {"attempts", probe.balance.pickup_drop_attempts[index]},
             {"drops", probe.balance.pickup_drops[index]},
             {"drop_rate", Ratio(probe.balance.pickup_drops[index],
                                 probe.balance.pickup_drop_attempts[index])},
             {"collected", probe.balance.pickup_collected[index]}});
    const auto active_ticks = std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>(probe.growth_ticks) + probe.boss_fight_ticks);
    const auto collected_pickups = std::accumulate(
        probe.balance.pickup_collected.begin(), probe.balance.pickup_collected.end(),
        std::uint64_t{});
    const std::array<double, kStatCount> stat_denominators{
        static_cast<double>(std::max(probe.max_health, 1)),
        std::max(impl_->distance_moved * 1000.0, 1.0),
        static_cast<double>(std::max<std::uint64_t>(probe.damage_dealt, 1)),
        static_cast<double>(active_ticks), static_cast<double>(active_ticks),
        static_cast<double>(std::max<std::uint64_t>(collected_pickups, 1))};
    std::array<double, kStatCount> stat_contribution{};
    Json stat_metrics = Json::array();
    for (std::size_t index = 0; index < kStatCount; ++index)
    {
        stat_contribution[index] = static_cast<double>(
            probe.balance.stat_utility[index]) / stat_denominators[index];
        stat_metrics.push_back(
            {{"id", kStatIds[index]}, {"points", probe.stat_points[index]},
             {"raw_utility", probe.balance.stat_utility[index]},
             {"raw_unit", kStatUtilityUnits[index]},
             {"realized_utility", stat_contribution[index]},
             {"per_point_utility", probe.stat_points[index] == 0 ? 0.0 :
                  stat_contribution[index] / probe.stat_points[index]},
             {"unit", "session_contribution_ratio"}});
    }
    Json damage = Json::array();
    for (std::size_t index = 0; index < probe.damage_by_skill.size(); ++index)
        damage.push_back({{"skill", kSkillIds[index]},
                          {"damage", probe.damage_by_skill[index]}});
    Json findings = Json::array();
    if (probe.growth_ticks >= 54'000 && (probe.level < 20 || probe.level > 30))
        findings.push_back({{"severity", "warning"}, {"category", "growth"},
                            {"message", "15분 성장 레벨이 목표 범위 20~30을 벗어났습니다."},
                            {"actual", probe.level}, {"target", 25}});
    if (impl_->maximum_damage_taken_in_tick * 2 >
        static_cast<std::uint64_t>(std::max(probe.max_health, 1)))
        findings.push_back({{"severity", "review"}, {"category", "damage_spike"},
                            {"message", "한 틱 피해가 최대 체력의 절반을 넘었습니다."},
                            {"damage", impl_->maximum_damage_taken_in_tick}});
    for (std::size_t index = 1; index < kCombatSkillCount; ++index)
        if (probe.skill_levels[index] != 0 &&
            probe.balance.skill_uses[index] >= 20 && probe.damage_dealt != 0 &&
            probe.damage_by_skill[index] * 20 < probe.damage_dealt)
            findings.push_back({{"severity", "review"}, {"category", "skill_balance"},
                                {"skill", kSkillIds[index]},
                                {"uses", probe.balance.skill_uses[index]},
                                {"damage", probe.damage_by_skill[index]},
                                {"damage_per_use", Ratio(
                                     probe.damage_by_skill[index],
                                     probe.balance.skill_uses[index])},
                                {"message", "습득한 스킬의 피해 기여도가 5% 미만입니다."}});

    const auto outcome = probe.phase == SessionPhase::Victory ? "victory" :
                         probe.phase == SessionPhase::Defeat ? "defeat" : "incomplete";
    const auto boss_damage = std::accumulate(
        probe.balance.skill_boss_damage.begin(),
        probe.balance.skill_boss_damage.end(), std::uint64_t{});
    Json output{{"schema_version", 5}, {"seed", impl_->config.seed},
                {"content_hash", impl_->config.content_source_hash},
                {"execution_valid", execution_valid}, {"outcome", outcome},
                {"tick", probe.tick}, {"growth_tick", probe.growth_ticks},
                {"boss_fight_tick", probe.boss_fight_ticks}, {"level", probe.level},
                {"kills", probe.kills}, {"health", probe.health},
                {"max_health", probe.max_health}, {"minimum_health", impl_->minimum_health},
                {"damage_dealt", probe.damage_dealt}, {"damage_taken", probe.damage_taken},
                {"healing", probe.healing}, {"damage_by_skill", std::move(damage)},
                {"damage_targets", {{"normal", probe.damage_dealt -
                                                  std::min(probe.damage_dealt, boss_damage)},
                                    {"boss", boss_damage}}},
                {"damage_composition", {{"direct", probe.balance.direct_damage},
                                         {"derived", probe.balance.derived_damage},
                                         {"damage_over_time", probe.balance.damage_over_time}}},
                {"balance", {{"skills", std::move(skill_metrics)},
                               {"upgrades", std::move(upgrade_metrics)},
                               {"relics", std::move(relic_metrics)},
                               {"upgrade_relic_synergies", std::move(synergy_metrics)},
                              {"enemies", std::move(enemy_metrics)},
                              {"pickups", std::move(pickup_metrics)},
                              {"stats", std::move(stat_metrics)}}},
                {"skill_levels", ByteArray(probe.skill_levels)},
                {"upgrade_masks", ByteArray(probe.upgrade_masks)},
                {"stat_points", ByteArray(probe.stat_points)},
                {"relic_mask", probe.relic_mask},
                {"build", {{"skill_levels", ByteArray(probe.skill_levels)},
                            {"skill_loadout", SkillLoadout(probe.skill_loadout)},
                            {"upgrade_masks", ByteArray(probe.upgrade_masks)},
                            {"stat_points", ByteArray(probe.stat_points)},
                            {"relic_mask", probe.relic_mask}}},
                {"distance_moved_m", impl_->distance_moved},
                {"near_death_seconds", static_cast<double>(impl_->near_death_ticks) / 60.0},
                {"peak_enemies", impl_->peak_enemies},
                {"peak_projectiles", impl_->peak_projectiles},
                {"peak_pickups", impl_->peak_pickups},
                {"maximum_damage_taken_in_tick", impl_->maximum_damage_taken_in_tick},
                {"first_kill_seconds", static_cast<double>(impl_->first_kill_tick) / 60.0},
                {"phase_ticks", impl_->phase_ticks},
                {"skill_press_counts", impl_->skill_press_counts},
                {"findings", findings}, {"user_review", "awaiting"}};
    std::ofstream result(impl_->directory / "result.json", std::ios::trunc);
    result << output.dump(2) << '\n';

    std::ofstream report(impl_->directory / "analysis.md", std::ios::trunc);
    report << "# Project HS 플레이테스트 분석\n\n"
                 << "- 결과: " << outcome << "\n- 레벨: " << probe.level
                 << "\n- 처치: " << probe.kills << "\n- 총 피해: " << probe.damage_dealt
                 << "\n- 받은 피해: " << probe.damage_taken << "\n- 회복: " << probe.healing
                 << "\n- 이동 거리: " << std::format("{:.1f}", impl_->distance_moved)
                 << "m\n\n## 자동 검토\n\n";
    if (findings.empty()) report << "- 자동 경고 없음\n";
    for (const auto &finding : findings)
        report << "- [" << finding.value("category", "review") << "] "
               << finding.value("message", "") << '\n';
    report << "\n## 스킬 강화별 기여 피해\n\n";
    for (std::size_t skill = 0; skill < kCombatSkillCount; ++skill)
    {
        for (std::size_t upgrade = 0; upgrade < kUpgradeCount; ++upgrade)
        {
            if ((probe.upgrade_masks[skill] & (1u << upgrade)) == 0) continue;
            report << "- " << kSkillIds[skill] << " / 강화 " << upgrade + 1
                   << ": " << probe.balance.upgrade_damage[skill][upgrade]
                   << " 피해 (" << probe.balance.upgrade_triggers[skill][upgrade]
                   << "회)";
            for (std::size_t effect = 0; effect < kUpgradeEffectMetricCount; ++effect)
            {
                const auto value = probe.balance.upgrade_effects[skill][upgrade][effect];
                if (value != 0)
                    report << " · " << kUpgradeEffectLabels[effect] << ' ' << value;
            }
            report << '\n';
        }
    }
    report << "\n## 강화·유물 실제 연쇄 기여\n\n";
    if (probe.balance.upgrade_relic_synergy_count == 0)
        report << "- 발동한 강화·유물 연쇄 없음\n";
    for (std::size_t index = 0;
         index < probe.balance.upgrade_relic_synergy_count; ++index)
    {
        const auto &entry = probe.balance.upgrade_relic_synergies[index];
        report << "- " << kSkillIds[static_cast<std::size_t>(entry.skill)]
               << " / 강화 " << static_cast<unsigned>(entry.upgrade) + 1
               << " × " << kRelicIds[static_cast<std::size_t>(entry.relic)];
        for (std::size_t metric = 0;
             metric < kUpgradeRelicSynergyMetricCount; ++metric)
            if (entry.metrics[metric] != 0)
                report << " · " << kSynergyMetricIds[metric] << ' '
                       << entry.metrics[metric];
        report << '\n';
    }
    report << "\n## 스탯 포인트당 세션 기여율\n\n";
    for (std::size_t stat = 0; stat < kStatCount; ++stat)
    {
        if (probe.stat_points[stat] == 0) continue;
        report << "- " << kStatIds[stat] << ": "
               << std::format("{:.2f}%", stat_contribution[stat] * 100.0 /
                                           probe.stat_points[stat])
               << " / 포인트\n";
    }
    report << "\n## 사용자 경험 검토\n\n"
                 << "- [ ] 이동, 공격 중 정지, 공격 후 이동 재개가 자연스러웠다.\n"
                 << "- [ ] 평타와 스킬 발사 시점이 애니메이션과 일치했다.\n"
                 << "- [ ] 적 공격과 보스 경고를 즉시 구분할 수 있었다.\n"
                 << "- [ ] 성장 선택의 효과와 시너지를 체감할 수 있었다.\n"
                 << "- [ ] 일반 구간과 보스전의 시간 및 난이도가 적절했다.\n"
                 << "- 소감: \n";
    return result && report ? Result::Success()
                            : Result::Failure(ErrorCode::InvalidState, "hs_playtest",
                                              "Cannot finalize playtest reports.");
}

const std::filesystem::path &PlaytestRecorder::Directory() const noexcept
{
    return impl_->directory;
}

bool PlaytestRecorder::Active() const noexcept
{
    return impl_->active && !impl_->finished;
}

Result LoadPlaytestReplay(const std::filesystem::path &directory,
                          PlaytestReplay &replay)
{
    try
    {
        std::ifstream metadata_stream(directory / "metadata.json");
        Json metadata;
        metadata_stream >> metadata;
        if (!metadata_stream || metadata.value("format_version", 0) != 4)
            return Invalid("Playtest metadata is missing or unsupported.");
        PlaytestReplay parsed;
        parsed.header.format_version = metadata.at("format_version").get<std::uint32_t>();
        parsed.header.simulation_version =
            metadata.at("simulation_version").get<std::uint32_t>();
        parsed.header.gameplay_hash_version =
            metadata.at("gameplay_hash_version").get<std::uint32_t>();
        parsed.header.simulation_rules_hash =
            metadata.at("simulation_rules_hash").get<std::uint64_t>();
        parsed.header.tick_rate = metadata.at("tick_rate").get<std::uint32_t>();
        parsed.header.determinism_profile =
            metadata.at("determinism_profile").get<std::uint32_t>();
        parsed.header.seed = metadata.at("seed").get<std::uint64_t>();
        if (parsed.header.simulation_version != kSimulationVersion ||
            parsed.header.gameplay_hash_version != kGameplayHashVersion ||
            parsed.header.tick_rate != 60 ||
            parsed.header.determinism_profile != kDeterminismProfile)
            return Invalid("Playtest replay is incompatible with this simulation build.");

        std::ifstream input(directory / "inputs.ndjson");
        std::string line;
        while (std::getline(input, line))
        {
            if (line.empty()) continue;
            const auto json = Json::parse(line);
            PlaytestReplayFrame frame;
            frame.target_tick = json.at("target_tick").get<Tick>();
            frame.expected_checksum = json.at("checksum").get<GameplayChecksum>();
            const auto &move = json.at("move_target");
            const auto &aim = json.at("aim_world");
            const auto &cursor = json.at("cursor_normalized");
            frame.held.move_target_world = {move.at(0).get<float>(), move.at(1).get<float>(),
                                            move.at(2).get<float>()};
            frame.held.aim_world = {aim.at(0).get<float>(), aim.at(1).get<float>(),
                                    aim.at(2).get<float>()};
            frame.held.cursor_normalized = {cursor.at(0).get<float>(),
                                            cursor.at(1).get<float>()};
            frame.held.move_held = json.at("move_held").get<bool>();
            frame.held.basic_attack_held = json.at("basic_attack_held").get<bool>();
            for (const auto &edge : json.at("edges"))
            {
                const auto action = edge.at("action").get<std::uint8_t>();
                const auto kind = edge.at("kind").get<std::uint8_t>();
                if (action > static_cast<std::uint8_t>(GameAction::Pause) ||
                    kind > static_cast<std::uint8_t>(EdgeKind::Released))
                    return Invalid("Playtest input contains an invalid action edge.");
                frame.ordered_edges.push_back(
                    {edge.at("sequence").get<Sequence>(),
                     static_cast<GameAction>(action), static_cast<EdgeKind>(kind)});
            }
            for (const auto &action_json : json.at("ui_actions"))
            {
                const auto kind = action_json.at("kind").get<std::uint8_t>();
                if (kind > static_cast<std::uint8_t>(UiActionKind::SwapLoadoutSlots))
                    return Invalid("Playtest input contains an invalid UI action.");
                frame.ui_actions.push_back(
                    {static_cast<UiActionKind>(kind),
                     action_json.at("value").get<std::uint8_t>(),
                     action_json.value("secondary", std::uint8_t{})});
            }
            parsed.frames.push_back(std::move(frame));
        }
        if (!input.eof() || parsed.frames.empty())
            return Invalid("Playtest input trace is empty or unreadable.");
        replay = std::move(parsed);
        return Result::Success();
    }
    catch (const std::exception &exception)
    {
        return Invalid(exception.what());
    }
}

} // namespace hs
