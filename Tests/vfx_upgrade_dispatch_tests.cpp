#include "vfx_upgrade_dispatch.hpp"
#include "vfx_typed_frame_adapter.hpp"
#include <hs/core/cooked_format.hpp>
#include <hs/game_domain/game_types.hpp>
#include <hs/gameplay/game_simulation.hpp>
#include <hs/presentation/projector.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace
{
void Check(bool value,const char *message) { if(!value) throw std::runtime_error(message); }
std::uint32_t Hash32(std::string_view text)
{
    std::uint32_t hash=2166136261u;
    for(unsigned char c:text){hash^=c;hash*=16777619u;}
    return hash;
}
constexpr std::array names{
    "particle.upgrade.explosive.reexplosion","particle.upgrade.explosive.mini_bomb.spawn",
    "particle.upgrade.explosive.mini_bomb.telegraph","particle.upgrade.explosive.mini_bomb.explosion",
    "particle.upgrade.explosive.shard_radial","particle.upgrade.explosive.blood_burst",
    "particle.upgrade.charged.end_explosion","particle.upgrade.charged.burn_explosion",
    "particle.upgrade.arrow.split","particle.upgrade.tracking_spawn",
    "particle.upgrade.ricochet.branch","particle.upgrade.ricochet.kill_small_arrows",
    "particle.upgrade.ricochet.kill_restart","particle.upgrade.arrow_rain.tracking_arrow",
    "particle.upgrade.trap.mini_spawn","particle.upgrade.retreat.tracking_arrow",
    "particle.upgrade.heal_proc","particle.upgrade.arrow.echo","particle.upgrade.arrow.pierce",
    "particle.upgrade.piercing.exit_split","particle.upgrade.piercing.chain_release",
    "particle.upgrade.arrow.split","particle.upgrade.arrow.pierce",
    "particle.upgrade.charged.radial_split","particle.upgrade.explosive.pre_pull",
    "particle.upgrade.piercing.push_alignment","particle.upgrade.explosive.mark_detonation",
    "particle.upgrade.multishot.secondary_fan","particle.upgrade.cooldown_proc",
    "particle.upgrade.multishot.rear_fan","particle.upgrade.retreat.triple_release",
    "particle.skill.multishot","particle.upgrade.retreat.land_shockwave",
    "particle.skill.charged_shot.pulse",
    "particle.upgrade.arrow.triple_release","particle.skill.fire_area.pulse",
    "particle.skill.trap","particle.upgrade.trap.mark_burst",
    "particle.upgrade.trap.rearm","particle.upgrade.trap.bleed_variant",
    "particle.upgrade.trap.pull_in"};
hs::VfxProgramData Program()
{
    hs::VfxProgramData p;
    for(std::uint32_t i=0;i<names.size();++i)
    {
        hs::VfxEffectRecord e;e.handle=i+1;e.input_mode=0;e.timing_kind=i==2?1:0;
        e.seconds = i == 38 ? 0.6f : i == 39 ? 0.5f : i == 40 ? 0.4f : 0.6f;
        const bool circle = i == 0 || i == 2 || i == 3 || i == 6 || i == 7 || i == 24 || i == 32 || i == 35 || i == 38 || i == 40;
        const bool cone = i == 27 || i == 29 || i == 30 || i == 31 || i == 34;
        e.payload_kind=Hash32(i==17 || i==28 || i==33 ? "PresentationContextPayload" :
                              (cone ? "ConePayload" :
                               (circle ? "CircleAreaPayload" : "PointEventPayload")));
        p.effects.push_back(e);
        p.effect_lookup.push_back({hs::MakeAssetId(names[i]).value,i+1});
    }
    const auto skill=hs::MakeAssetId("skill.explosive_arrow").value;
    const auto charged=hs::MakeAssetId("skill.charged_shot").value;
    const auto basic=hs::MakeAssetId("skill.basic_attack").value;
    const auto piercing=hs::MakeAssetId("skill.piercing_shot").value;
    const auto ricochet=hs::MakeAssetId("skill.ricochet_arrow").value;
    const auto rain=hs::MakeAssetId("skill.arrow_rain").value;
    const auto trap=hs::MakeAssetId("skill.trap").value;
    const auto retreat=hs::MakeAssetId("skill.retreat_shot").value;
    const auto multi=hs::MakeAssetId("skill.multishot").value;
    p.upgrade_bindings={{skill,1,{0,1}},{skill,2,{1,3}},{skill,3,{4,1}},{skill,5,{5,1}},
                         {charged,1,{6,1}},{charged,2,{33,1}},{charged,7,{7,1}},
                         {basic,3,{8,1}},{piercing,1,{9,1}},
                         {ricochet,2,{10,1}},{ricochet,5,{11,1}},
                         {ricochet,6,{12,1}},{rain,5,{13,1}},
                         {rain,7,{13,1}},{trap,8,{14,1}},
                           {retreat,4,{15,1}},{retreat,5,{32,1}},{retreat,7,{16,1}},
                          {basic,1,{17,1}},{basic,2,{18,1}},{piercing,2,{19,1}},
                           {piercing,5,{20,1}},{multi,2,{8,1}},{multi,3,{18,1}},
                           {charged,6,{23,1}},{skill,6,{24,1}},
                           {piercing,6,{25,1}},{multi,6,{9,1}},
                           {charged,4,{18,1}},{skill,7,{26,1}},
                           {multi,1,{27,1}},
                           {charged,8,{28,1}},{ricochet,7,{28,1}},{retreat,6,{28,1}},
                            {multi,4,{29,1}},{retreat,1,{30,1}},{multi,7,{31,1}},
                           {basic,8,{34,1}},{skill,4,{35,1}},
                           {trap,1,{36,1}},{trap,7,{37,1}},
                           {trap,3,{38,1}},{trap,4,{39,1}},{trap,6,{40,1}}};
    p.upgrade_sequences={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41};return p;
}
hs::SimulationRules QuietRules()
{
    auto rules=hs::SimulationRules::Defaults();
    for(auto &stage:rules.spawn_stages) stage.per_second=0;
    for(auto &wave:rules.waves) wave.count=0;
    rules.growth.utility_pickup_base_chance=0.0f;
    rules.growth.utility_pickup_miss_increment=0.0f;
    rules.relic_drop.normal_enemy_base_probability=0.0f;
    rules.relic_drop.normal_enemy_probability_increment_per_kill=0.0f;
    rules.relic_drop.healing_pickup_probability=0.0f;
    return rules;
}
hs::VfxProgramData UpgradeSlowProgram()
{
    const auto circle=Hash32("CircleAreaPayload");
    const auto baseline=hs::MakeAssetId("persistent.area.slow").value;
    const auto trap=hs::MakeAssetId("particle.upgrade.trap.land_slow").value;
    const auto arrow_rain=hs::MakeAssetId("particle.upgrade.arrow_rain.finish_slow").value;
    hs::VfxProgramData program;
    const auto add_effect=[&](std::uint64_t id) {
        hs::VfxEffectRecord effect;
        effect.handle=static_cast<std::uint32_t>(program.effects.size()+1);
        effect.input_mode=1;
        effect.payload_kind=circle;
        program.effects.push_back(effect);
        program.effect_lookup.push_back({id,effect.handle});
    };
    add_effect(baseline); add_effect(trap); add_effect(arrow_rain);
    const auto trap_skill=hs::MakeAssetId("skill.trap").value;
    const auto arrow_rain_skill=hs::MakeAssetId("skill.arrow_rain").value;
    program.upgrade_bindings={{trap_skill,2,{0,1}},{arrow_rain_skill,8,{1,1}}};
    program.upgrade_sequences={2,3};
    return program;
}
hs::VfxProgramData ArrowRainPullProgram()
{
    hs::VfxProgramData program;
    hs::VfxEffectRecord effect;
    effect.handle=1;
    effect.input_mode=0;
    effect.payload_kind=Hash32("SourceTargetPayload");
    effect.seconds=0.45f;
    program.effects.push_back(effect);
    program.effect_lookup.push_back({hs::MakeAssetId(
        "particle.upgrade.arrow_rain.pull").value,1});
    program.upgrade_bindings={{hs::MakeAssetId("skill.arrow_rain").value,2,{0,1}}};
    program.upgrade_sequences={1};
    return program;
}
hs::PresentationEvent Event(std::uint8_t index,std::uint8_t stage)
{
    hs::PresentationEvent e;e.kind=hs::PresentationKind::Vfx;e.sequence=91;e.tick=100;e.session_id=3;
    e.asset=hs::MakeAssetId("particle.common.explosion_small");e.position={4,.15f,-7};
    e.upgrade_skill=static_cast<std::uint8_t>(hs::SkillKind::ExplosiveArrow);e.upgrade_index=index;e.upgrade_stage=stage;
    e.upgrade_cast_id=19;e.upgrade_owner_id=71;e.geometry.kind=hs::PresentationGeometryKind::Circle;
    e.geometry.radius=2.25f;e.geometry.source_id=71;e.geometry.start_tick=100;e.geometry.end_tick=160;return e;
}
void SimulationProjectionDispatchUsesZeroBasedUpgradeIndex()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({0xBA51Cu}).Succeeded(),"simulation upgrade ingress initialize");
    Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::BasicAttack),0}).Succeeded(),
          "grant basic echo upgrade");

    hs::InputFrame input;
    input.held.basic_attack_held=true;
    input.held.aim_world={20.0f,0.0f,0.0f};
    constexpr auto fixed_step=std::chrono::nanoseconds{16'666'667};
    hs::DomainSignal upgrade_signal{};
    bool found=false;
    for(std::uint32_t tick=0;tick<300 && !found;++tick)
    {
        input.target_tick=simulation.GetObservation().tick+1;
        (void)simulation.TickFixed(input,fixed_step);
        for(const auto &signal:simulation.PendingDomainSignals())
        {
            if(signal.kind==hs::DomainSignalKind::UpgradeVisual &&
               signal.upgrade_skill==static_cast<std::uint8_t>(hs::SkillKind::BasicAttack) &&
               signal.upgrade_stage==hs::UpgradeVisualStage::Spawn)
            {
                upgrade_signal=signal;
                found=true;
                break;
            }
        }
    }
    Check(found,"simulation emitted no basic echo upgrade visual");

    std::array<hs::PresentationEvent,2> projected{};
    const auto projected_count=hs::ProjectDomainSignal(upgrade_signal,projected);
    Check(projected_count==1 && projected[0].kind==hs::PresentationKind::Vfx &&
          projected[0].upgrade_index==0 &&
          projected[0].upgrade_stage==static_cast<std::uint8_t>(hs::UpgradeVisualStage::Spawn),
          "simulation upgrade visual did not project its zero-based ordinal");

    std::vector<hs::PresentationEvent> dispatched;
    Check(hs::runtime_detail::DispatchVfxUpgradeEvents(
              Program(),std::span(projected).first(projected_count),dispatched).Succeeded() &&
          dispatched.size()==1 &&
          dispatched[0].asset.value==hs::MakeAssetId("particle.upgrade.arrow.echo").value &&
          dispatched[0].asset.value!=hs::MakeAssetId("particle.upgrade.arrow.pierce").value,
          "simulation upgrade visual dispatched the wrong adjacent basic upgrade effect");
    Check(simulation.Shutdown().Succeeded(),"simulation upgrade ingress shutdown");
}
void SimulationChargedShotPulseProjectsAndDispatches()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({0xC4A2EDu}).Succeeded(),
          "charged pulse initialize");
    Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::ChargedShot)}).Succeeded() &&
          simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::ChargedShot),1}).Succeeded(),
          "grant charged shot and faster charge upgrade");

    hs::InputFrame input;
    input.held.aim_world={20.0f,0.0f,0.0f};
    constexpr auto fixed_step=std::chrono::nanoseconds{16'666'667};
    const std::array press{hs::ActionEdge{1,hs::GameAction::SkillQ,hs::EdgeKind::Pressed}};
    hs::DomainSignal pulse{};
    bool found=false;
    const auto inspect_pulse=[&] {
        std::uint32_t pulse_count{};
        std::uint32_t decorated_count{};
        for(const auto &signal:simulation.PendingDomainSignals())
        {
            if(signal.kind!=hs::DomainSignalKind::ChargedShotPulse) continue;
            ++pulse_count;
            if(signal.upgrade_skill==static_cast<std::uint8_t>(hs::SkillKind::ChargedShot) &&
               signal.upgrade_index==1 &&
               signal.upgrade_stage==hs::UpgradeVisualStage::Spawn)
            {
                pulse=signal;
                ++decorated_count;
            }
        }
        found=pulse_count==1 && decorated_count==1;
    };

    input.target_tick=simulation.GetObservation().tick+1;
    input.ordered_edges=std::span<const hs::ActionEdge>(press);
    (void)simulation.TickFixed(input,fixed_step);
    inspect_pulse();
    if(!found) simulation.ClearDomainSignals();
    input.ordered_edges=std::span<const hs::ActionEdge>{};
    for(std::uint32_t tick=0;tick<24 && !found;++tick)
    {
        input.target_tick=simulation.GetObservation().tick+1;
        (void)simulation.TickFixed(input,fixed_step);
        inspect_pulse();
        if(!found) simulation.ClearDomainSignals();
    }
    Check(found,"faster charge emitted no single decorated six-tick pulse");
    constexpr std::uint64_t player_render_id=1ull<<60;
    hs::GameReadModelStorage charging_model;
    simulation.WriteReadModel(charging_model);
    Check(pulse.upgrade_cast_id!=0 && pulse.upgrade_owner_id==player_render_id &&
          pulse.tick>charging_model.View().player.charge_start &&
          (pulse.tick+player_render_id)%6==0 &&
          pulse.scale==1.0f && std::isfinite(pulse.vfx_ratio01) &&
          pulse.vfx_ratio01>0.0f && pulse.vfx_ratio01<1.0f,
          "charged pulse lost cast owner, valid ratio, or legacy scale");

    std::array<hs::PresentationEvent,2> projected{};
    const auto projected_count=hs::ProjectDomainSignal(pulse,projected);
    const auto legacy_parameters=hs::DecodeVfxParameters(projected[0].parameters);
    Check(projected_count==1 && projected[0].kind==hs::PresentationKind::Vfx &&
          projected[0].asset.value==hs::MakeAssetId(
              "particle.skill.charged_shot.pulse").value &&
          projected[0].upgrade_skill==static_cast<std::uint8_t>(hs::SkillKind::ChargedShot) &&
          projected[0].upgrade_index==1 &&
          projected[0].upgrade_stage==static_cast<std::uint8_t>(hs::UpgradeVisualStage::Spawn) &&
          projected[0].upgrade_cast_id==pulse.upgrade_cast_id &&
          projected[0].upgrade_owner_id==pulse.upgrade_owner_id &&
          projected[0].vfx_ratio01==pulse.vfx_ratio01 &&
          legacy_parameters.scale==1.0f,
          "charged pulse projection changed its cooked asset or legacy scale");

    std::vector<hs::PresentationEvent> dispatched;
    Check(hs::runtime_detail::DispatchVfxUpgradeEvents(
              Program(),std::span(projected).first(projected_count),dispatched).Succeeded() &&
          dispatched.size()==1 &&
          dispatched[0].asset.value==hs::MakeAssetId(
              "particle.skill.charged_shot.pulse").value &&
          dispatched[0].upgrade_cast_id==pulse.upgrade_cast_id &&
          dispatched[0].upgrade_owner_id==pulse.upgrade_owner_id &&
          dispatched[0].vfx_ratio01==pulse.vfx_ratio01,
          "charged pulse did not dispatch through its cooked ordinal2 binding");

    const std::array release{hs::ActionEdge{2,hs::GameAction::SkillQ,hs::EdgeKind::Released}};
    input.target_tick=simulation.GetObservation().tick+1;
    input.ordered_edges=std::span<const hs::ActionEdge>(release);
    (void)simulation.TickFixed(input,fixed_step);
    hs::GameReadModelStorage model;
    simulation.WriteReadModel(model);
    const auto projectile=std::find_if(model.View().projectiles.begin(),
                                       model.View().projectiles.end(),[](const auto &value) {
        return value.player_owned && value.skill==hs::SkillKind::ChargedShot && !value.dead;
    });
    Check(projectile!=model.View().projectiles.end() &&
          projectile->cast_id==pulse.upgrade_cast_id,
          "released charged projectile did not retain pulse cast identity");
    Check(simulation.Shutdown().Succeeded(),"charged pulse shutdown");
}
void SimulationEmitsSecondaryFanAfterSuccessfulVolley()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({0x5EC0D1u}).Succeeded(),
          "multishot secondary fan initialize");
    Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::MultiShot)}).Succeeded(),
          "grant multishot skill");
    Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::MultiShot),0}).Succeeded(),
          "grant multishot secondary fan upgrade");

    hs::InputFrame input;
    input.held.aim_world={20.0f,0.0f,0.0f};
    const std::array edges{hs::ActionEdge{1,hs::GameAction::SkillQ,hs::EdgeKind::Pressed}};
    constexpr auto fixed_step=std::chrono::nanoseconds{16'666'667};
    hs::DomainSignal secondary{};
    bool found=false;
    for(std::uint32_t tick=0;tick<90 && !found;++tick)
    {
        input.target_tick=simulation.GetObservation().tick+1;
        input.ordered_edges=tick==0 ? std::span<const hs::ActionEdge>(edges) :
                                      std::span<const hs::ActionEdge>{};
        (void)simulation.TickFixed(input,fixed_step);
        for(const auto &signal:simulation.PendingDomainSignals())
        {
            if(signal.kind==hs::DomainSignalKind::UpgradeVisual &&
               signal.upgrade_skill==static_cast<std::uint8_t>(hs::SkillKind::MultiShot) &&
               signal.upgrade_index==0 &&
               signal.upgrade_stage==hs::UpgradeVisualStage::Spawn)
            {
                secondary=signal;
                found=true;
                break;
            }
        }
    }
    Check(found,"successful delayed multishot volley emitted no secondary fan visual");
    const auto &second_fan=simulation.Rules().upgrades.multishot.delayed_second_fan;
    const auto &skill=simulation.Rules().skills[static_cast<std::size_t>(hs::SkillKind::MultiShot)];
    Check(secondary.upgrade_cast_id!=0 && secondary.upgrade_owner_id!=0 &&
          secondary.geometry.kind==hs::DomainSignalGeometryKind::Cone &&
          secondary.geometry.source_id==secondary.upgrade_owner_id &&
          secondary.geometry.start_tick==secondary.tick &&
          secondary.geometry.end_tick==secondary.tick &&
          std::abs(secondary.geometry.range-skill.range)<0.0001f &&
          std::abs(secondary.geometry.half_angle_degrees-
                   std::abs(second_fan.fan_angle_degrees)*0.5f)<0.0001f,
          "secondary fan visual lost actual volley identity or cone geometry");
    Check(simulation.Shutdown().Succeeded(),"multishot secondary fan shutdown");
}
void ScheduledFanIngressUsesFrozenCastDirection()
{
    const auto check_fan=[](hs::SkillKind skill,std::uint8_t upgrade_index,
                            std::string_view effect,std::uint64_t seed) {
        hs::GameSimulation simulation;
        Check(simulation.Initialize({seed}).Succeeded(),"scheduled fan initialize");
        Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantSkill,
              static_cast<std::uint64_t>(skill)}).Succeeded(),"grant fan skill");
        Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantUpgrade,
              static_cast<std::uint64_t>(skill),upgrade_index}).Succeeded(),
              "grant fan upgrade");
        hs::InputFrame input;
        input.held.aim_world={20.0f,0.0f,0.0f};
        const std::array edges{hs::ActionEdge{1,hs::GameAction::SkillQ,hs::EdgeKind::Pressed}};
        constexpr auto fixed_step=std::chrono::nanoseconds{16'666'667};
        hs::DomainSignal fan{};
        std::uint32_t fan_count{};
        std::array<std::array<float,2>,3> retreat_directions{};
        std::size_t retreat_arrow_count{};
        for(std::uint32_t tick=0;tick<40;++tick)
        {
            input.target_tick=simulation.GetObservation().tick+1;
            input.ordered_edges=tick==0 ? std::span<const hs::ActionEdge>(edges) :
                                          std::span<const hs::ActionEdge>{};
            (void)simulation.TickFixed(input,fixed_step);
            for(const auto &signal:simulation.PendingDomainSignals())
            {
                if(skill==hs::SkillKind::RetreatShot &&
                   signal.kind==hs::DomainSignalKind::ArrowReleased)
                {
                    if(retreat_arrow_count<retreat_directions.size())
                        retreat_directions[retreat_arrow_count]={signal.direction.x,signal.direction.z};
                    ++retreat_arrow_count;
                }
                if(signal.kind==hs::DomainSignalKind::UpgradeVisual &&
                   signal.upgrade_skill==static_cast<std::uint8_t>(skill) &&
                   signal.upgrade_index==upgrade_index &&
                   signal.upgrade_stage==hs::UpgradeVisualStage::Spawn)
                {
                    fan=signal;
                    ++fan_count;
                }
            }
            simulation.ClearDomainSignals();
            // The scheduled release must retain the direction captured on the cast tick.
            input.held.aim_world={0.0f,0.0f,20.0f};
        }
        if(fan_count!=1)
            throw std::runtime_error("scheduled fan emitted " + std::to_string(fan_count) +
                                     " spawns for skill " +
                                     std::to_string(static_cast<int>(skill)));
        const auto expected_x=skill==hs::SkillKind::MultiShot ? -1.0f : 1.0f;
        const auto &skill_rule=simulation.Rules().skills[static_cast<std::size_t>(skill)];
        const auto expected_half_angle=skill==hs::SkillKind::MultiShot
            ? std::abs(simulation.Rules().upgrades.multishot.rear_fan.fan_angle_degrees)*0.5f
            : std::abs(simulation.Rules().upgrades.retreat_shot
                           .replace_with_three_arrows.angles_degrees[0]);
        Check(fan.upgrade_cast_id!=0 && fan.upgrade_owner_id!=0 &&
              fan.geometry.kind==hs::DomainSignalGeometryKind::Cone &&
              fan.geometry.source_id==fan.upgrade_owner_id &&
              fan.geometry.start_tick==fan.tick && fan.geometry.end_tick==fan.tick &&
              std::abs(fan.geometry.range-skill_rule.range)<0.0001f &&
              std::abs(fan.geometry.half_angle_degrees-expected_half_angle)<0.0001f &&
              std::abs(fan.direction.x-expected_x)<0.0001f &&
              std::abs(fan.direction.z)<0.0001f,
              "scheduled fan lost its projectile owner or frozen authored cone");
        if(skill==hs::SkillKind::RetreatShot)
        {
            const auto &angles=simulation.Rules().upgrades.retreat_shot
                                   .replace_with_three_arrows.angles_degrees;
            Check(retreat_arrow_count==angles.size(),"retreat upgrade released wrong arrow count");
            std::array<bool,3> matched{};
            for(const auto &direction:retreat_directions)
            {
                bool found=false;
                for(std::size_t index=0;index<angles.size();++index)
                {
                    if(matched[index]) continue;
                    const auto radians=angles[index]*std::numbers::pi_v<float>/180.0f;
                    if(std::abs(direction[0]-std::cos(radians))<0.0001f &&
                       std::abs(direction[1]+std::sin(radians))<0.0001f)
                    {
                        matched[index]=true;
                        found=true;
                        break;
                    }
                }
                Check(found,"retreat arrow direction disagrees with authored fan angle");
            }
        }
        std::array<hs::PresentationEvent,2> projected{};
        const auto projected_count=hs::ProjectDomainSignal(fan,projected);
        auto program=Program();
        std::vector<hs::PresentationEvent> output;
        Check(projected_count==1 &&
              hs::runtime_detail::DispatchVfxUpgradeEvents(
                  program,std::span(projected).first(projected_count),output).Succeeded() &&
              output.size()==1 && output[0].asset.value==hs::MakeAssetId(effect).value &&
              output[0].geometry.kind==hs::PresentationGeometryKind::Cone &&
              output[0].upgrade_stage==static_cast<std::uint8_t>(hs::UpgradeVisualStage::Spawn),
              "scheduled fan did not dispatch its exact cooked cone effect");
        const auto skill_id=hs::MakeAssetId(skill==hs::SkillKind::MultiShot
                                            ? "skill.multishot" : "skill.retreat_shot").value;
        const auto binding=std::find_if(program.upgrade_bindings.begin(),
                                        program.upgrade_bindings.end(),[&](const auto &value) {
            return value.skill_id==skill_id &&
                   value.ordinal==static_cast<std::uint32_t>(upgrade_index)+1u;
        });
        Check(binding!=program.upgrade_bindings.end(),"fan cooked binding missing in fixture");
        program.upgrade_sequences[binding->sequence.first]=1;
        Check(!hs::runtime_detail::DispatchVfxUpgradeEvents(
                  program,std::span(projected).first(projected_count),output).Succeeded() &&
              output.size()==1 && output[0].asset.value==hs::MakeAssetId(effect).value,
              "fan dispatch accepted an effect absent from cooked binding");
        Check(simulation.Shutdown().Succeeded(),"scheduled fan shutdown");
    };
    check_fan(hs::SkillKind::MultiShot,3,
              "particle.upgrade.multishot.rear_fan",0xBEEFu);
    check_fan(hs::SkillKind::RetreatShot,0,
              "particle.upgrade.retreat.triple_release",0xC0DEu);
}
void SimulationEmitsOuterArrowConeAfterSuccessfulProjectile()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({0x0A7E5u}).Succeeded(),
          "multishot outer-arrow initialize");
    Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::MultiShot)}).Succeeded() &&
          simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::MultiShot),6}).Succeeded(),
          "grant multishot outer-arrow upgrade");

    hs::InputFrame input;
    input.held.aim_world={20.0f,0.0f,0.0f};
    const std::array edges{hs::ActionEdge{1,hs::GameAction::SkillQ,hs::EdgeKind::Pressed}};
    constexpr auto fixed_step=std::chrono::nanoseconds{16'666'667};
    hs::DomainSignal outer_signal{};
    std::uint32_t outer_count{};
    for(std::uint32_t tick=0;tick<40;++tick)
    {
        input.target_tick=simulation.GetObservation().tick+1;
        input.ordered_edges=tick==0 ? std::span<const hs::ActionEdge>(edges) :
                                      std::span<const hs::ActionEdge>{};
        (void)simulation.TickFixed(input,fixed_step);
        for(const auto &signal:simulation.PendingDomainSignals())
        {
            if(signal.kind==hs::DomainSignalKind::UpgradeVisual &&
               signal.upgrade_skill==static_cast<std::uint8_t>(hs::SkillKind::MultiShot) &&
               signal.upgrade_index==6 &&
               signal.upgrade_stage==hs::UpgradeVisualStage::Spawn)
            {
                outer_signal=signal;
                ++outer_count;
            }
        }
        simulation.ClearDomainSignals();
        input.held.aim_world={0.0f,0.0f,20.0f};
    }
    Check(outer_count==1,"successful multishot outer volley emitted the wrong visual count");

    const auto &outer=simulation.Rules().upgrades.multishot.two_additional_outer_arrows;
    float expected_half_angle{};
    for(std::uint32_t index=0;index<outer.additional_projectiles;++index)
        expected_half_angle=std::max(expected_half_angle,
                                     std::abs(outer.outer_angles_degrees[index]));
    const auto &skill=simulation.Rules().skills[static_cast<std::size_t>(hs::SkillKind::MultiShot)];
    Check(outer_signal.upgrade_cast_id!=0 && outer_signal.upgrade_owner_id!=0 &&
          outer_signal.geometry.kind==hs::DomainSignalGeometryKind::Cone &&
          outer_signal.geometry.source_id==outer_signal.upgrade_owner_id &&
          outer_signal.geometry.start_tick==outer_signal.tick &&
          outer_signal.geometry.end_tick==outer_signal.tick &&
          std::abs(outer_signal.geometry.range-skill.range)<0.0001f &&
          std::abs(outer_signal.geometry.half_angle_degrees-expected_half_angle)<0.0001f &&
          std::abs(outer_signal.direction.x-1.0f)<0.0001f &&
          std::abs(outer_signal.direction.z)<0.0001f,
          "outer-arrow visual lost its successful owner or frozen authored cone");

    std::array<hs::PresentationEvent,2> projected{};
    const auto projected_count=hs::ProjectDomainSignal(outer_signal,projected);
    auto program=Program();
    std::vector<hs::PresentationEvent> output;
    Check(projected_count==1 &&
          hs::runtime_detail::DispatchVfxUpgradeEvents(
              program,std::span(projected).first(projected_count),output).Succeeded() &&
          output.size()==1 &&
          output[0].asset.value==hs::MakeAssetId("particle.skill.multishot").value &&
          output[0].geometry.kind==hs::PresentationGeometryKind::Cone &&
          output[0].upgrade_index==6 &&
          output[0].upgrade_stage==static_cast<std::uint8_t>(hs::UpgradeVisualStage::Spawn),
          "outer-arrow visual did not dispatch its cooked multishot cone");
    const auto multi_id=hs::MakeAssetId("skill.multishot").value;
    const auto binding=std::find_if(program.upgrade_bindings.begin(),
                                    program.upgrade_bindings.end(),[&](const auto &value) {
        return value.skill_id==multi_id && value.ordinal==7;
    });
    Check(binding!=program.upgrade_bindings.end(),"outer-arrow cooked binding missing in fixture");
    program.upgrade_sequences[binding->sequence.first]=1;
    Check(!hs::runtime_detail::DispatchVfxUpgradeEvents(
              program,std::span(projected).first(projected_count),output).Succeeded() &&
          output.size()==1 &&
          output[0].asset.value==hs::MakeAssetId("particle.skill.multishot").value,
          "outer-arrow dispatch accepted an effect absent from cooked binding");
    Check(simulation.Shutdown().Succeeded(),"multishot outer-arrow shutdown");
}
void SimulationEmitsRetreatLandingShockwaveAfterSuccessfulLanding()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({0x51A0C7u}).Succeeded(),
          "retreat landing shockwave initialize");
    Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::RetreatShot)}).Succeeded() &&
          simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::RetreatShot),4}).Succeeded(),
          "grant retreat landing shockwave upgrade");

    hs::InputFrame input;
    input.held.aim_world={20.0f,0.0f,0.0f};
    constexpr auto fixed_step=std::chrono::nanoseconds{16'666'667};
    const std::array retreat_edge{hs::ActionEdge{1,hs::GameAction::SkillQ,hs::EdgeKind::Pressed}};
    input.target_tick=simulation.GetObservation().tick+1;
    input.ordered_edges=std::span<const hs::ActionEdge>(retreat_edge);
    (void)simulation.TickFixed(input,fixed_step);

    hs::DomainSignal landing_signal{};
    bool found=false;
    for(std::uint32_t step=0;step<90 && !found;++step)
    {
        input.target_tick=simulation.GetObservation().tick+1;
        input.ordered_edges=std::span<const hs::ActionEdge>{};
        (void)simulation.TickFixed(input,fixed_step);
        for(const auto &signal:simulation.PendingDomainSignals())
        {
            if(signal.kind==hs::DomainSignalKind::UpgradeVisual &&
               signal.upgrade_skill==static_cast<std::uint8_t>(hs::SkillKind::RetreatShot) &&
               signal.upgrade_index==4 &&
               signal.upgrade_stage==hs::UpgradeVisualStage::Resolve)
            {
                landing_signal=signal;
                found=true;
                break;
            }
        }
    }
    Check(found,"successful retreat landing emitted no shockwave visual");
    const auto player=simulation.GetObservation().player_position;
    const auto &landing=simulation.Rules().upgrades.retreat_shot.landing_damage_and_push;
    constexpr std::uint64_t player_render_id=1ull<<60;
    Check(landing_signal.upgrade_cast_id!=0 &&
          landing_signal.upgrade_owner_id==player_render_id &&
          landing_signal.position.x==player.x &&
          landing_signal.position.z==player.y &&
          landing_signal.direction.x==1.0f &&
          landing_signal.direction.z==0.0f &&
          landing_signal.geometry.kind==hs::DomainSignalGeometryKind::Circle &&
          landing_signal.geometry.radius==landing.radius &&
          landing_signal.geometry.source_id==player_render_id,
          "retreat landing shockwave lost its landing geometry or player owner");

    std::array<hs::PresentationEvent,2> projected{};
    const auto projected_count=hs::ProjectDomainSignal(landing_signal,projected);
    std::vector<hs::PresentationEvent> dispatched;
    Check(projected_count==1 &&
          hs::runtime_detail::DispatchVfxUpgradeEvents(
              Program(),std::span(projected).first(projected_count),dispatched).Succeeded() &&
          dispatched.size()==1 &&
          dispatched[0].asset.value==hs::MakeAssetId(
              "particle.upgrade.retreat.land_shockwave").value &&
          dispatched[0].geometry.kind==hs::PresentationGeometryKind::Circle &&
          dispatched[0].geometry.radius==landing.radius &&
          dispatched[0].geometry.source_id==player_render_id,
          "retreat landing shockwave did not dispatch its cooked circle effect");
    Check(simulation.Shutdown().Succeeded(),"retreat landing shockwave shutdown");
}
void SimulationEmitsCooldownProcForRealRefund()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({0xC001D00Du}).Succeeded(),
          "retreat cooldown proc initialize");
    Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::RetreatShot)}).Succeeded() &&
          simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::PiercingShot)}).Succeeded(),
          "grant active skills for retreat cooldown proc");
    Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::RetreatShot),5}).Succeeded(),
          "grant retreat cooldown refund upgrade");

    hs::InputFrame input;
    input.held.aim_world={20.0f,0.0f,0.0f};
    constexpr auto fixed_step=std::chrono::nanoseconds{16'666'667};
    const std::array retreat_edge{hs::ActionEdge{1,hs::GameAction::SkillQ,hs::EdgeKind::Pressed}};
    input.target_tick=simulation.GetObservation().tick+1;
    input.ordered_edges=std::span<const hs::ActionEdge>(retreat_edge);
    (void)simulation.TickFixed(input,fixed_step);

    input.ordered_edges=std::span<const hs::ActionEdge>{};
    for(std::uint32_t tick=0;tick<24;++tick)
    {
        input.target_tick=simulation.GetObservation().tick+1;
        (void)simulation.TickFixed(input,fixed_step);
    }

    const std::array followup_edge{hs::ActionEdge{2,hs::GameAction::SkillW,hs::EdgeKind::Pressed}};
    input.target_tick=simulation.GetObservation().tick+1;
    input.ordered_edges=std::span<const hs::ActionEdge>(followup_edge);
    (void)simulation.TickFixed(input,fixed_step);

    const auto &signals=simulation.PendingDomainSignals();
    const auto signal=std::find_if(signals.begin(),signals.end(),[](const auto &value) {
        return value.kind==hs::DomainSignalKind::UpgradeVisual &&
               value.upgrade_skill==static_cast<std::uint8_t>(hs::SkillKind::RetreatShot) &&
               value.upgrade_index==5 &&
               value.upgrade_stage==hs::UpgradeVisualStage::Resolve;
    });
    Check(signal!=signals.end(),"real retreat cooldown refund emitted no visual");
    const auto player=simulation.GetObservation().player_position;
    Check(signal->upgrade_cast_id!=0 && signal->upgrade_owner_id!=0 &&
          signal->position.x==player.x && signal->position.z==player.y,
          "real retreat cooldown visual lost cast, owner, or player position");
    std::array<hs::PresentationEvent,2> projected{};
    const auto projected_count=hs::ProjectDomainSignal(*signal,projected);
    std::vector<hs::PresentationEvent> dispatched;
    Check(projected_count==1 &&
          hs::runtime_detail::DispatchVfxUpgradeEvents(
              Program(),std::span(projected).first(projected_count),dispatched).Succeeded() &&
          dispatched.size()==1 &&
          dispatched[0].asset.value==hs::MakeAssetId("particle.upgrade.cooldown_proc").value,
          "real retreat cooldown visual did not dispatch its cooked proc");
    Check(simulation.Shutdown().Succeeded(),"retreat cooldown proc shutdown");
}
void StagesAreIndependent()
{
    auto p=Program();std::vector<hs::PresentationEvent> out;
    for(std::uint8_t stage=1;stage<=3;++stage)
    {
        auto e=Event(1,stage);auto audio=e;audio.kind=hs::PresentationKind::Audio;audio.asset=hs::MakeAssetId("audio.common.explosion_small");
        const std::array input{e,audio};
        Check(hs::runtime_detail::DispatchVfxUpgradeEvents(p,input,out).Succeeded(),"valid stage dispatch failed");
        Check(out.size()==2&&out[0].asset.value==hs::MakeAssetId(names[stage]).value&&out[1].asset.value==audio.asset.value,
              "dispatch fired entire sequence or duplicated generic event/audio");
        Check(out[0].sequence==91&&out[0].tick==100&&out[0].geometry.radius==2.25f&&out[0].geometry.end_tick==160&&
              out[0].upgrade_owner_id==71&&out[0].session_id==3,"dispatch lost authoritative event fields");
    }
    auto e=Event(0,3);Check(hs::runtime_detail::DispatchVfxUpgradeEvents(p,std::span(&e,1),out).Succeeded()&&
        out.size()==1&&out[0].asset.value==hs::MakeAssetId(names[0]).value,"zero-based upgrade index did not select ordinal1");
    e.upgrade_stage=0;Check(hs::runtime_detail::DispatchVfxUpgradeEvents(p,std::span(&e,1),out).Succeeded()&&
        out[0].asset.value==e.asset.value,"ordinary event was remapped");
}
void InvalidContractsFailWithoutPartialOutput()
{
    auto p=Program();auto e=Event(1,2);std::vector<hs::PresentationEvent> out{Event(0,3)};
    const auto rejects=[&]{return !hs::runtime_detail::DispatchVfxUpgradeEvents(p,std::span(&e,1),out)&&out.size()==1&&out[0].upgrade_index==0;};
    p.upgrade_sequences[2]=2;Check(rejects(),"missing stage membership accepted");p.upgrade_sequences[2]=3;
    p.upgrade_bindings[1].sequence={999,3};Check(rejects(),"out-of-bounds sequence accepted");p.upgrade_bindings[1].sequence={1,3};
    e.geometry.end_tick=100;Check(rejects(),"zero warning lifetime accepted");e.geometry.end_tick=160;
    e.geometry.radius=0;Check(rejects(),"missing damage radius accepted");e.geometry.radius=2.25f;
    e.geometry.source_id=99;Check(rejects(),"different warning owner accepted");e.geometry.source_id=71;
    p.effects[2].timing_kind=0;Check(rejects(),"fixed recipe accepted as scheduled warning");p.effects[2].timing_kind=1;
    e.upgrade_index=0;Check(rejects(),"ordinal1 fired ordinal2 warning stage");
}
void ExplicitAdditionalMappings()
{
    auto p=Program();std::vector<hs::PresentationEvent> out;
    auto check=[&](std::uint8_t skill,std::uint8_t index,std::string_view name,bool circle) {
        auto e=Event(index,3); e.upgrade_skill=skill;
        if (!circle) e.geometry={};
        Check(hs::runtime_detail::DispatchVfxUpgradeEvents(p,std::span(&e,1),out).Succeeded(),"additional mapping rejected");
        Check(out.size()==1&&out[0].asset.value==hs::MakeAssetId(name).value,"additional mapping chose wrong effect");
    };
    check(static_cast<std::uint8_t>(hs::SkillKind::ExplosiveArrow),4,
          "particle.upgrade.explosive.blood_burst",false);
    check(static_cast<std::uint8_t>(hs::SkillKind::ChargedShot),0,
          "particle.upgrade.charged.end_explosion",true);
    check(static_cast<std::uint8_t>(hs::SkillKind::ChargedShot),6,
          "particle.upgrade.charged.burn_explosion",true);
    auto shard=Event(2,1);shard.upgrade_skill=static_cast<std::uint8_t>(hs::SkillKind::ExplosiveArrow);shard.geometry={};
    Check(hs::runtime_detail::DispatchVfxUpgradeEvents(p,std::span(&shard,1),out).Succeeded()&&
          out[0].asset.value==hs::MakeAssetId("particle.upgrade.explosive.shard_radial").value,
          "shard mapping rejected");
    auto wrong=Event(2,3);wrong.upgrade_skill=static_cast<std::uint8_t>(hs::SkillKind::ExplosiveArrow);
    Check(!hs::runtime_detail::DispatchVfxUpgradeEvents(p,std::span(&wrong,1),out).Succeeded(),
          "unsupported shard stage accepted");
    p.upgrade_bindings[2].sequence={999,1};
    Check(!hs::runtime_detail::DispatchVfxUpgradeEvents(p,std::span(&shard,1),out).Succeeded(),
          "missing shard cooked membership accepted");
    const auto check_point=[&](std::uint8_t skill,std::uint8_t index,std::uint8_t stage,std::string_view name) {
        auto point=Event(index,stage); point.upgrade_skill=skill; point.geometry={};
        Check(hs::runtime_detail::DispatchVfxUpgradeEvents(p,std::span(&point,1),out).Succeeded() &&
              out[0].asset.value==hs::MakeAssetId(name).value,
              "missing v4 point mapping");
    };
    check_point(static_cast<std::uint8_t>(hs::SkillKind::BasicAttack),2,1,
                 "particle.upgrade.arrow.split");
    check_point(static_cast<std::uint8_t>(hs::SkillKind::BasicAttack),1,3,
                "particle.upgrade.arrow.pierce");
    check_point(static_cast<std::uint8_t>(hs::SkillKind::PiercingShot),0,1,
                "particle.upgrade.tracking_spawn");
    check_point(static_cast<std::uint8_t>(hs::SkillKind::RicochetArrow),1,1,
                "particle.upgrade.ricochet.branch");
    check_point(static_cast<std::uint8_t>(hs::SkillKind::ArrowRain),4,1,
                "particle.upgrade.arrow_rain.tracking_arrow");
    check_point(static_cast<std::uint8_t>(hs::SkillKind::Trap),7,1,
                 "particle.upgrade.trap.mini_spawn");
    check_point(static_cast<std::uint8_t>(hs::SkillKind::RetreatShot),6,3,
                 "particle.upgrade.heal_proc");
    check_point(static_cast<std::uint8_t>(hs::SkillKind::MultiShot),2,3,
                "particle.upgrade.arrow.pierce");
    check_point(static_cast<std::uint8_t>(hs::SkillKind::ChargedShot),3,3,
                "particle.upgrade.arrow.pierce");
    check_point(static_cast<std::uint8_t>(hs::SkillKind::PiercingShot),5,3,
                "particle.upgrade.piercing.push_alignment");
    check_point(static_cast<std::uint8_t>(hs::SkillKind::ExplosiveArrow),6,3,
                "particle.upgrade.explosive.mark_detonation");
    auto secondary=Event(0,1);
    secondary.upgrade_skill=static_cast<std::uint8_t>(hs::SkillKind::MultiShot);
    secondary.geometry.kind=hs::PresentationGeometryKind::Cone;
    secondary.geometry.range=16.0f;
    secondary.geometry.half_angle_degrees=25.0f;
    secondary.geometry.start_tick=secondary.tick;
    secondary.geometry.end_tick=secondary.tick;
    Check(hs::runtime_detail::DispatchVfxUpgradeEvents(
              p,std::span(&secondary,1),out).Succeeded() &&
          out[0].asset.value==hs::MakeAssetId(
              "particle.upgrade.multishot.secondary_fan").value,
          "multishot secondary fan mapping rejected");
    auto pre_pull=Event(5,3); pre_pull.geometry.kind=hs::PresentationGeometryKind::Circle;
    pre_pull.upgrade_skill=static_cast<std::uint8_t>(hs::SkillKind::ExplosiveArrow);
    Check(hs::runtime_detail::DispatchVfxUpgradeEvents(p,std::span(&pre_pull,1),out).Succeeded() &&
          out[0].asset.value==hs::MakeAssetId("particle.upgrade.explosive.pre_pull").value,
          "explosive pre-pull resolves on its activation stage");
}
void CooldownProcMappings()
{
    auto p=Program();std::vector<hs::PresentationEvent> out;
    const std::array mappings{
        std::array<std::uint8_t,2>{static_cast<std::uint8_t>(hs::SkillKind::ChargedShot),7},
        std::array<std::uint8_t,2>{static_cast<std::uint8_t>(hs::SkillKind::RicochetArrow),6},
        std::array<std::uint8_t,2>{static_cast<std::uint8_t>(hs::SkillKind::RetreatShot),5}};
    for(const auto mapping:mappings)
    {
        auto event=Event(mapping[1],3);event.upgrade_skill=mapping[0];event.geometry={};
        Check(hs::runtime_detail::DispatchVfxUpgradeEvents(p,std::span(&event,1),out).Succeeded() &&
              out.size()==1 && out[0].asset.value==hs::MakeAssetId(
                  "particle.upgrade.cooldown_proc").value &&
              out[0].upgrade_index==mapping[1],"cooldown proc binding membership rejected");
    }
}
void RealSlowAreaOwnersSelectAuthoredAliases()
{
    constexpr auto fixed_step=std::chrono::nanoseconds{16'666'667};
    const auto check_cast=[&](hs::SkillKind skill,std::uint8_t upgrade_index,
                              std::uint64_t seed,std::uint32_t expected_effect,
                              std::string_view expected_effect_name,
                              const char *label) {
        hs::GameSimulation simulation;
        Check(simulation.Initialize({seed},QuietRules()).Succeeded(),label);
        Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantSkill,
              static_cast<std::uint64_t>(skill)}).Succeeded(),
              "slow-area skill grant");
        Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantUpgrade,
              static_cast<std::uint64_t>(skill),upgrade_index}).Succeeded(),
              "slow-area upgrade grant");

        hs::InputFrame input;
        input.held.aim_world={20.0f,0.0f,0.0f};
        const std::array edge{hs::ActionEdge{1,hs::GameAction::SkillQ,
                                              hs::EdgeKind::Pressed}};
        input.target_tick=simulation.GetObservation().tick+1;
        input.ordered_edges=std::span<const hs::ActionEdge>(edge);
        (void)simulation.TickFixed(input,fixed_step);
        input.ordered_edges=std::span<const hs::ActionEdge>{};

        hs::GameReadModelStorage model;
        hs::RenderSnapshotStorage snapshot(128,8,2,64);
        const hs::PresentationCatalog presentation{};
        const hs::PresentationUiState ui{};
        const hs::SettingsData settings{};
        hs::PersistentVfxVisual owner{};
        hs::AreaView area{};
        bool found=false;
        for(std::uint32_t step=0;step<450 && !found;++step)
        {
            model.Clear();
            simulation.WriteReadModel(model);
            const auto view=model.View();
            snapshot.Clear();
            Check(hs::ProjectRenderSnapshot(view,presentation,ui,settings,snapshot),
                  "slow-area snapshot projection");
            const auto projected=snapshot.View();
            std::size_t owner_count{};
            for(const auto &visual:projected.persistent_vfx)
            {
                if(visual.kind==hs::PersistentVfxKind::UpgradeSlowArea)
                {
                    owner=visual;
                    ++owner_count;
                }
            }
            if(owner_count==1)
            {
                const auto baseline_owner_count=std::count_if(
                    projected.persistent_vfx.begin(),projected.persistent_vfx.end(),
                    [&](const auto &visual) {
                        return visual.kind==hs::PersistentVfxKind::SlowArea &&
                               visual.stable_id==owner.stable_id;
                    });
                Check(baseline_owner_count==0,
                      "slow-area snapshot duplicated a baseline owner");
                const auto area_it=std::find_if(view.areas.begin(),view.areas.end(),
                    [&](const hs::AreaView &candidate) {
                        return candidate.kind==hs::AreaViewKind::Slow &&
                               candidate.skill==skill &&
                               candidate.source_upgrade==upgrade_index &&
                               candidate.cast_id==owner.cast_id;
                    });
                Check(area_it!=view.areas.end(),"slow-area snapshot lost its source area");
                area=*area_it;
                found=true;
                break;
            }
            input.target_tick=simulation.GetObservation().tick+1;
            (void)simulation.TickFixed(input,fixed_step);
        }
        Check(found,label);
        Check(owner.stable_id!=0 && owner.cast_id!=0 && owner.skill==static_cast<std::uint8_t>(skill) &&
              owner.source_upgrade==upgrade_index && owner.active_tick==area.active_tick &&
              owner.expires==area.expires && owner.active_tick<owner.expires &&
              owner.active_tick<=snapshot.View().header.tick &&
              snapshot.View().header.tick<owner.expires &&
              std::abs(owner.position.x-area.position.x)<0.0001f &&
              std::abs(owner.position.z-area.position.y)<0.0001f &&
              std::abs(owner.position.y-0.018f)<0.0001f &&
              std::abs(owner.radius-area.radius)<0.0001f,
              "slow-area owner metadata or geometry changed in the snapshot");

        const auto program=UpgradeSlowProgram();
        const auto expected_effect_id=hs::MakeAssetId(expected_effect_name).value;
        const auto expected_lookup=std::find_if(program.effect_lookup.begin(),
                                                 program.effect_lookup.end(),
            [expected_effect](const auto &lookup) {
                return lookup.handle==expected_effect;
            });
        Check(expected_lookup!=program.effect_lookup.end() &&
              expected_lookup->effect_id==expected_effect_id,
              "slow-area cooked alias fixture changed its authored effect");
        const auto adapted=hs::runtime_detail::BuildVfxTypedFrameInputs(
            program,{},std::array{owner},{},snapshot.View().header.tick);
        Check(adapted.unsupported_effect_ids.empty() && adapted.persistent.size()==1,
              "slow-area owner did not adapt to one typed persistent input");
        Check(adapted.persistent.front().effect_handle==expected_effect &&
              adapted.persistent.front().stable_id==owner.stable_id &&
              std::count_if(adapted.persistent.begin(),adapted.persistent.end(),
                  [&](const auto &input_value) {
                      return input_value.stable_id==owner.stable_id;
                  })==1,
              "slow-area owner selected a duplicate or baseline typed effect");
        const auto &circle=std::get<hs::VfxCirclePayload>(adapted.persistent.front().payload);
        Check(std::abs(circle.center.x-owner.position.x)<0.0001f &&
              std::abs(circle.center.z-owner.position.z)<0.0001f &&
              std::abs(circle.radius-owner.radius)<0.0001f &&
              circle.lifetime01>=0.0f && circle.lifetime01<1.0f,
              "typed slow-area alias changed its authoritative circle geometry");
        Check(simulation.Shutdown().Succeeded(),"slow-area shutdown");
    };
    // GrantUpgrade uses zero-based indices; these exercise cooked ordinals 2 and 8.
    check_cast(hs::SkillKind::Trap,1,0x51A0C7u,2,
               "particle.upgrade.trap.land_slow",
               "real trap landing slow owner did not spawn");
    check_cast(hs::SkillKind::ArrowRain,7,0xA77001u,3,
               "particle.upgrade.arrow_rain.finish_slow",
               "real arrow-rain finish slow owner did not spawn");
}
void RealArrowRainPullProjectsAndDispatches()
{
    constexpr auto fixed_step=std::chrono::nanoseconds{16'666'667};
    const auto cast_and_find=[&](bool with_target,hs::DomainSignal &output) {
        hs::GameSimulation simulation;
        auto rules=QuietRules();
        auto &enemy=rules.enemies[static_cast<std::size_t>(hs::EnemyKind::Melee)];
        enemy.move_speed=0.0f;
        enemy.attack_range=0.0f;
        Check(simulation.Initialize({0xA221u},rules).Succeeded(),
              "arrow-rain pull initialize");
        Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantSkill,
              static_cast<std::uint64_t>(hs::SkillKind::ArrowRain)}).Succeeded() &&
              simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantUpgrade,
              static_cast<std::uint64_t>(hs::SkillKind::ArrowRain),1}).Succeeded(),
              "arrow-rain pull skill or upgrade grant");
        if(with_target)
            Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::SpawnEnemy,
                  static_cast<std::uint64_t>(hs::EnemyKind::Melee),0,{18.0f,1.0f}}).Succeeded(),
                  "arrow-rain pull target spawn");

        hs::InputFrame input;
        input.held.aim_world={20.0f,0.0f,0.0f};
        const std::array edge{hs::ActionEdge{1,hs::GameAction::SkillQ,
                                              hs::EdgeKind::Pressed}};
        input.target_tick=simulation.GetObservation().tick+1;
        input.ordered_edges=std::span<const hs::ActionEdge>(edge);
        (void)simulation.TickFixed(input,fixed_step);
        simulation.ClearDomainSignals();
        input.ordered_edges=std::span<const hs::ActionEdge>{};

        bool found=false;
        std::uint32_t pull_cue_count=0;
        for(std::uint32_t tick=0;tick<90 && !found;++tick)
        {
            input.target_tick=simulation.GetObservation().tick+1;
            (void)simulation.TickFixed(input,fixed_step);
            for(const auto &signal:simulation.PendingDomainSignals())
            {
                if(signal.kind==hs::DomainSignalKind::Pull)
                    ++pull_cue_count;
                if(signal.kind==hs::DomainSignalKind::UpgradeVisual &&
                   signal.upgrade_skill==static_cast<std::uint8_t>(hs::SkillKind::ArrowRain) &&
                   signal.upgrade_index==1 &&
                   signal.upgrade_stage==hs::UpgradeVisualStage::Resolve)
                {
                    output=signal;
                    found=true;
                    break;
                }
            }
            simulation.ClearDomainSignals();
        }
        Check(pull_cue_count==(with_target ? 1u : 0u),
              "arrow-rain emitted a pull cue without actual displacement");
        if(found)
        {
            const auto delta=hs::Float3{
                output.geometry.end_position.x-output.position.x,
                output.geometry.end_position.y-output.position.y,
                output.geometry.end_position.z-output.position.z};
            const auto endpoint_length=std::sqrt(delta.x*delta.x+delta.y*delta.y+
                                                 delta.z*delta.z);
            const auto horizontal_length=std::hypot(delta.x,delta.z);
            const auto direction_length=std::hypot(output.direction.x,output.direction.z);
            const auto &pull=simulation.Rules().upgrades.arrow_rain.first_damage_tick_pull;
            Check(output.upgrade_cast_id!=0 && output.upgrade_owner_id!=0 &&
                  output.geometry.kind==hs::DomainSignalGeometryKind::Line &&
                  output.geometry.source_id==output.upgrade_owner_id &&
                  output.geometry.width>0.0f && std::isfinite(endpoint_length) &&
                  endpoint_length>0.0f && endpoint_length<=pull.pull_radius+0.0001f &&
                  horizontal_length>0.0f &&
                  direction_length>0.9f && direction_length<1.1f,
                  "arrow-rain pull lost its live owner or bounded line geometry");
            Check(std::abs(output.direction.x-delta.x/horizontal_length)<0.0001f &&
                  std::abs(output.direction.z-delta.z/horizontal_length)<0.0001f,
                  "arrow-rain pull direction disagrees with its endpoint");
        }
        Check(simulation.Shutdown().Succeeded(),"arrow-rain pull shutdown");
        return found;
    };

    hs::DomainSignal pull_signal{};
    Check(cast_and_find(true,pull_signal),
          "eligible arrow-rain target emitted no ordinal2 pull visual");
    hs::DomainSignal no_target_signal{};
    Check(!cast_and_find(false,no_target_signal),
          "arrow-rain ordinal2 emitted a pull visual without an eligible target");

    std::array<hs::PresentationEvent,2> projected{};
    const auto projected_count=hs::ProjectDomainSignal(pull_signal,projected);
    const auto program=ArrowRainPullProgram();
    std::vector<hs::PresentationEvent> dispatched;
    Check(projected_count==1 &&
          projected[0].upgrade_skill==pull_signal.upgrade_skill &&
          projected[0].upgrade_index==pull_signal.upgrade_index &&
          projected[0].upgrade_stage==static_cast<std::uint8_t>(pull_signal.upgrade_stage) &&
          projected[0].upgrade_cast_id==pull_signal.upgrade_cast_id &&
          projected[0].upgrade_owner_id==pull_signal.upgrade_owner_id &&
          projected[0].geometry.kind==hs::PresentationGeometryKind::Line &&
          projected[0].geometry.source_id==pull_signal.geometry.source_id &&
          projected[0].geometry.width==pull_signal.geometry.width &&
          projected[0].geometry.end_position.x==pull_signal.geometry.end_position.x &&
          projected[0].geometry.end_position.y==pull_signal.geometry.end_position.y &&
          projected[0].geometry.end_position.z==pull_signal.geometry.end_position.z &&
          hs::runtime_detail::DispatchVfxUpgradeEvents(
              program,std::span(projected).first(projected_count),dispatched).Succeeded() &&
          dispatched.size()==1 &&
          dispatched.front().asset.value==hs::MakeAssetId(
              "particle.upgrade.arrow_rain.pull").value &&
          dispatched.front().upgrade_cast_id==pull_signal.upgrade_cast_id &&
          dispatched.front().upgrade_owner_id==pull_signal.upgrade_owner_id &&
          dispatched.front().geometry.kind==hs::PresentationGeometryKind::Line &&
          dispatched.front().geometry.end_position.x==pull_signal.geometry.end_position.x &&
          dispatched.front().geometry.end_position.y==pull_signal.geometry.end_position.y &&
          dispatched.front().geometry.end_position.z==pull_signal.geometry.end_position.z &&
          dispatched.front().geometry.source_id==dispatched.front().upgrade_owner_id &&
          dispatched.front().geometry.width==pull_signal.geometry.width,
          "arrow-rain pull did not dispatch its authored SourceTargetPayload");
}
void FourNewBindingsRequireExactStageAndCookedMembership()
{
    struct Case
    {
        hs::SkillKind skill;
        std::uint8_t index;
        std::uint8_t stage;
        std::string_view effect;
        std::uint32_t handle;
        float seconds;
    };
    const std::array cases{
        Case{hs::SkillKind::BasicAttack,7,1,"particle.upgrade.arrow.triple_release",35,0.6f},
        Case{hs::SkillKind::ExplosiveArrow,3,3,"particle.skill.fire_area.pulse",36,0.6f},
        Case{hs::SkillKind::Trap,0,1,"particle.skill.trap",37,0.6f},
        Case{hs::SkillKind::Trap,6,3,"particle.upgrade.trap.mark_burst",38,0.6f},
        Case{hs::SkillKind::Trap,2,3,"particle.upgrade.trap.rearm",39,0.6f},
        Case{hs::SkillKind::Trap,3,3,"particle.upgrade.trap.bleed_variant",40,0.5f},
        Case{hs::SkillKind::Trap,5,3,"particle.upgrade.trap.pull_in",41,0.4f}};
    for(const auto &entry:cases)
    {
        auto program=Program();
        auto event=Event(entry.index,entry.stage);
        event.upgrade_skill=static_cast<std::uint8_t>(entry.skill);
        event.geometry={};
        if(entry.skill==hs::SkillKind::BasicAttack)
        {
            event.geometry.kind=hs::PresentationGeometryKind::Cone;
            event.geometry.range=16.0f;
            event.geometry.half_angle_degrees=15.0f;
            event.geometry.source_id=event.upgrade_owner_id;
            event.geometry.start_tick=event.geometry.end_tick=event.tick;
        }
        else if(entry.skill==hs::SkillKind::ExplosiveArrow)
        {
            event.geometry.kind=hs::PresentationGeometryKind::Circle;
            event.geometry.radius=2.5f;
            event.geometry.source_id=event.upgrade_owner_id;
        }
        else if(entry.skill==hs::SkillKind::Trap && entry.index != 3)
        {
            event.geometry.kind=hs::PresentationGeometryKind::Circle;
            event.geometry.radius=2.5f;
            event.geometry.source_id=event.upgrade_owner_id;
        }
        Check(std::abs(program.effects[entry.handle-1].seconds-entry.seconds)<0.0001f,
              "new binding fixture lost its authored fixed timing");
        std::vector<hs::PresentationEvent> output;
        Check(hs::runtime_detail::DispatchVfxUpgradeEvents(
                  program,std::span(&event,1),output).Succeeded() &&
              output.size()==1 &&
              output[0].asset.value==hs::MakeAssetId(entry.effect).value,
              "new binding did not select its exact cooked effect");
        const auto valid=event;
        event.upgrade_stage=entry.stage==1 ? 3 : 1;
        Check(!hs::runtime_detail::DispatchVfxUpgradeEvents(
                  program,std::span(&event,1),output).Succeeded() &&
              output.size()==1 && output[0].asset.value==hs::MakeAssetId(entry.effect).value,
              "new binding accepted the wrong activation stage or changed prior output");
        event=valid;
        const auto skill_id=hs::MakeAssetId(entry.skill==hs::SkillKind::BasicAttack
                                               ? "skill.basic_attack"
                                               : entry.skill==hs::SkillKind::Trap
                                                     ? "skill.trap":"skill.explosive_arrow").value;
        const auto binding=std::find_if(program.upgrade_bindings.begin(),
                                        program.upgrade_bindings.end(),[&](const auto &value) {
            return value.skill_id==skill_id && value.ordinal==entry.index+1u;
        });
        Check(binding!=program.upgrade_bindings.end(),"new fixture binding missing");
        program.upgrade_sequences[binding->sequence.first]=1;
        Check(!hs::runtime_detail::DispatchVfxUpgradeEvents(
                  program,std::span(&event,1),output).Succeeded(),
              "new binding accepted an effect outside its cooked sequence");
        program.upgrade_sequences[binding->sequence.first]=entry.handle;
        program.effects[entry.handle-1].payload_kind=Hash32("PresentationContextPayload");
        Check(!hs::runtime_detail::DispatchVfxUpgradeEvents(
                  program,std::span(&event,1),output).Succeeded(),
              "new binding accepted an incompatible cooked payload");
        program=Program();
        if(entry.skill==hs::SkillKind::BasicAttack) event.geometry.range=0.0f;
        else if(entry.skill==hs::SkillKind::ExplosiveArrow) event.geometry.radius=0.0f;
        else if(entry.skill==hs::SkillKind::Trap && entry.index != 3) event.geometry.radius=0.0f;
        else continue;
        Check(!hs::runtime_detail::DispatchVfxUpgradeEvents(
                  program,std::span(&event,1),output).Succeeded(),
              "new geometric binding accepted missing authoritative geometry");
    }
}
void RealEmpoweredBasicAndRollTrapBindings()
{
    constexpr auto fixed_step=std::chrono::nanoseconds{16'666'667};
    {
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0xB45C1u},QuietRules()).Succeeded(),
              "empowered basic initialize");
        Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantSkill,
              static_cast<std::uint64_t>(hs::SkillKind::PiercingShot)}).Succeeded() &&
              simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantUpgrade,
              static_cast<std::uint64_t>(hs::SkillKind::BasicAttack),7}).Succeeded(),
              "empowered basic grants");
        hs::InputFrame input;
        input.held.aim_world={20.0f,0.0f,0.0f};
        const std::array skill_edge{hs::ActionEdge{1,hs::GameAction::SkillQ,hs::EdgeKind::Pressed}};
        hs::DomainSignal cone{};
        std::uint32_t cone_count{};
        std::uint32_t released_before_cone{};
        for(std::uint32_t step=0;step<140;++step)
        {
            input.target_tick=simulation.GetObservation().tick+1;
            input.ordered_edges=step==0 ? std::span<const hs::ActionEdge>(skill_edge)
                                        : std::span<const hs::ActionEdge>{};
            input.held.basic_attack_held=step>=30;
            (void)simulation.TickFixed(input,fixed_step);
            for(const auto &signal:simulation.PendingDomainSignals())
            {
                if(signal.kind==hs::DomainSignalKind::ArrowReleased &&
                   signal.context==static_cast<std::uint8_t>(hs::SkillKind::BasicAttack) &&
                   cone_count==0) ++released_before_cone;
                if(signal.kind==hs::DomainSignalKind::UpgradeVisual &&
                   signal.upgrade_skill==static_cast<std::uint8_t>(hs::SkillKind::BasicAttack) &&
                   signal.upgrade_index==7)
                {
                    cone=signal;
                    ++cone_count;
                }
            }
            simulation.ClearDomainSignals();
        }
        float half_angle{};
        for(const auto angle:simulation.Rules().upgrades.basic_attack.post_active_three_arrow.angles_degrees)
            half_angle=std::max(half_angle,std::abs(angle));
        Check(cone_count==1 && released_before_cone==1 &&
              cone.upgrade_stage==hs::UpgradeVisualStage::Spawn &&
              cone.upgrade_cast_id!=0 && cone.upgrade_owner_id!=0 &&
              cone.geometry.kind==hs::DomainSignalGeometryKind::Cone &&
              cone.geometry.source_id==cone.upgrade_owner_id &&
              cone.geometry.start_tick==cone.tick && cone.geometry.end_tick==cone.tick &&
              cone.geometry.range==simulation.Rules().skills[0].range &&
              cone.geometry.half_angle_degrees==half_angle,
              "empowered basic did not spawn one cone at its first real triple release");
        std::array<hs::PresentationEvent,2> projected{};
        const auto count=hs::ProjectDomainSignal(cone,projected);
        std::vector<hs::PresentationEvent> output;
        Check(count==1 && hs::runtime_detail::DispatchVfxUpgradeEvents(
                  Program(),std::span(projected).first(count),output).Succeeded() &&
              output.size()==1 && output[0].asset.value==hs::MakeAssetId(
                  "particle.upgrade.arrow.triple_release").value,
              "real empowered basic cone did not dispatch");
        Check(simulation.Shutdown().Succeeded(),"empowered basic shutdown");
    }
    {
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0x7A91u},QuietRules()).Succeeded(),
              "roll trap initialize");
        Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantSkill,
              static_cast<std::uint64_t>(hs::SkillKind::Trap)}).Succeeded() &&
              simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantUpgrade,
              static_cast<std::uint64_t>(hs::SkillKind::Trap),0}).Succeeded(),
              "roll trap grants");
        hs::InputFrame input;
        input.target_tick=simulation.GetObservation().tick+1;
        input.held.aim_world={20.0f,0.0f,0.0f};
        const std::array edge{hs::ActionEdge{1,hs::GameAction::SkillQ,hs::EdgeKind::Pressed}};
        input.ordered_edges=std::span<const hs::ActionEdge>(edge);
        (void)simulation.TickFixed(input,fixed_step);
        std::uint32_t spawn_count{};
        std::uint32_t generic_count{};
        std::uint32_t audio_count{};
        std::uint64_t cast_id{};
        std::vector<std::uint64_t> owners;
        hs::Float3 first_position{};
        const auto &roll=simulation.Rules().upgrades.trap.roll_path_traps;
        for(const auto &signal:simulation.PendingDomainSignals())
        {
            if(signal.kind==hs::DomainSignalKind::TrapCast && signal.upgrade_stage==hs::UpgradeVisualStage::None)
                ++generic_count;
            if(signal.upgrade_skill!=static_cast<std::uint8_t>(hs::SkillKind::Trap) ||
               signal.upgrade_index!=0 || signal.upgrade_stage!=hs::UpgradeVisualStage::Spawn)
                continue;
            Check(signal.kind==hs::DomainSignalKind::TrapCast ||
                  signal.kind==hs::DomainSignalKind::UpgradeVisual,
                  "roll trap used an unrelated visual ingress");
            if(spawn_count==0) first_position=signal.position;
            Check(signal.scale==0.8f && signal.upgrade_cast_id!=0 &&
                  signal.upgrade_owner_id!=0 &&
                  signal.position.x==first_position.x+roll.spacing*static_cast<float>(spawn_count) &&
                  signal.position.z==first_position.z,
                  "roll trap visual lost its actual trap placement or scale");
            if(cast_id==0) cast_id=signal.upgrade_cast_id;
            Check(signal.upgrade_cast_id==cast_id &&
                  std::find(owners.begin(),owners.end(),signal.upgrade_owner_id)==owners.end(),
                  "roll trap visual repeated an owner or changed cast");
            owners.push_back(signal.upgrade_owner_id);
            std::array<hs::PresentationEvent,3> projected{};
            const auto count=hs::ProjectDomainSignal(signal,projected);
            std::vector<hs::PresentationEvent> output;
            Check(hs::runtime_detail::DispatchVfxUpgradeEvents(
                      Program(),std::span(projected).first(count),output).Succeeded() &&
                  !output.empty() && output[0].asset.value==hs::MakeAssetId(
                      "particle.skill.trap").value &&
                  hs::DecodeVfxParameters(output[0].parameters).scale==0.8f,
                  "real roll trap failed point dispatch or changed its scale");
            audio_count+=static_cast<std::uint32_t>(std::ranges::count_if(output,[](const auto &value) {
                return value.kind==hs::PresentationKind::Audio;
            }));
            ++spawn_count;
        }
        Check(spawn_count==roll.trap_count && generic_count==0 && audio_count==1,
              "roll trap did not create one scaled cue per trap with one cast audio");
        Check(simulation.Shutdown().Succeeded(),"roll trap shutdown");
    }
}
void RealTrapRearmPullAndBleedBindings()
{
    constexpr auto fixed_step=std::chrono::nanoseconds{16'666'667};
    const auto run=[&](std::uint8_t upgrade_index, hs::Float2 enemy_position,
                       std::string_view effect, bool expect_visual) {
        auto rules=QuietRules();
        auto &enemy=rules.enemies[static_cast<std::size_t>(hs::EnemyKind::Melee)];
        enemy.health=100'000;
        enemy.move_speed=0.0f;
        enemy.damage=0;
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0x7A4B0u+upgrade_index},rules).Succeeded(),
              "trap live binding initialize");
        Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantSkill,
              static_cast<std::uint64_t>(hs::SkillKind::Trap)}).Succeeded() &&
              simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantUpgrade,
              static_cast<std::uint64_t>(hs::SkillKind::Trap),upgrade_index}).Succeeded() &&
              simulation.ApplyDebugCommand({hs::DebugCommandKind::SpawnEnemy,
              static_cast<std::uint64_t>(hs::EnemyKind::Melee),0,enemy_position}).Succeeded(),
              "trap live binding setup");
        hs::InputFrame input;
        input.held.aim_world={20.0f,0.0f,0.0f};
        const std::array edge{hs::ActionEdge{1,hs::GameAction::SkillQ,hs::EdgeKind::Pressed}};
        hs::DomainSignal visual{};
        std::uint32_t count{};
        for(std::uint32_t step=0;step<190;++step)
        {
            input.target_tick=simulation.GetObservation().tick+1;
            input.ordered_edges=step==0 ? std::span<const hs::ActionEdge>(edge)
                                        : std::span<const hs::ActionEdge>{};
            (void)simulation.TickFixed(input,fixed_step);
            for(const auto &signal:simulation.PendingDomainSignals())
            {
                if(signal.kind==hs::DomainSignalKind::UpgradeVisual &&
                   signal.upgrade_skill==static_cast<std::uint8_t>(hs::SkillKind::Trap) &&
                   signal.upgrade_index==upgrade_index &&
                   signal.upgrade_stage==hs::UpgradeVisualStage::Resolve)
                {
                    visual=signal;
                    ++count;
                }
            }
            simulation.ClearDomainSignals();
        }
        Check(count==static_cast<std::uint32_t>(expect_visual),
              "trap live binding emitted on the wrong success condition");
        if(expect_visual)
        {
            Check(visual.upgrade_cast_id!=0 && visual.upgrade_owner_id!=0 &&
                  visual.position.x==0.0f && visual.position.z==0.0f,
                  "trap live binding lost its actual area identity");
            if(upgrade_index==2 || upgrade_index==5)
            {
                Check(visual.geometry.kind==hs::DomainSignalGeometryKind::Circle &&
                      visual.geometry.radius==simulation.Rules().skills[
                          static_cast<std::size_t>(hs::SkillKind::Trap)].area_radius &&
                      visual.geometry.source_id==visual.upgrade_owner_id,
                      "trap live circle binding lost its actual radius or owner");
            }
            std::array<hs::PresentationEvent,2> projected{};
            std::vector<hs::PresentationEvent> output;
            const auto projected_count=hs::ProjectDomainSignal(visual,projected);
            Check(projected_count==1 && hs::runtime_detail::DispatchVfxUpgradeEvents(
                      Program(),std::span(projected).first(projected_count),output).Succeeded() &&
                  output.size()==1 && output[0].asset.value==hs::MakeAssetId(effect).value &&
                  output[0].upgrade_cast_id==visual.upgrade_cast_id &&
                  output[0].upgrade_owner_id==visual.upgrade_owner_id,
                  "trap live binding did not dispatch its exact cooked effect");
        }
        Check(simulation.Shutdown().Succeeded(),"trap live binding shutdown");
    };
    run(2,{0.0f,0.0f},"particle.upgrade.trap.rearm",true);
    run(5,{1.0f,0.0f},"particle.upgrade.trap.pull_in",true);
    run(5,{0.0f,0.0f},"particle.upgrade.trap.pull_in",false);
    run(3,{0.0f,0.0f},"particle.upgrade.trap.bleed_variant",true);
}
void RealFirePulseAndTrapMarkBindings()
{
    constexpr auto fixed_step=std::chrono::nanoseconds{16'666'667};
    auto rules=QuietRules();
    auto &enemy=rules.enemies[static_cast<std::size_t>(hs::EnemyKind::Melee)];
    enemy.health=100'000;
    enemy.move_speed=0.0f;
    enemy.damage=0;
    {
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0xF14Eu},rules).Succeeded(),
              "explosive fire-area initialize");
        Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantSkill,
              static_cast<std::uint64_t>(hs::SkillKind::ExplosiveArrow)}).Succeeded() &&
              simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantUpgrade,
              static_cast<std::uint64_t>(hs::SkillKind::ExplosiveArrow),3}).Succeeded() &&
              simulation.ApplyDebugCommand({hs::DebugCommandKind::SpawnEnemy,
              static_cast<std::uint64_t>(hs::EnemyKind::Melee),0,{5.0f,0.0f}}).Succeeded(),
              "explosive fire-area setup");
        hs::InputFrame input;
        input.held.aim_world={20.0f,0.0f,0.0f};
        const std::array edge{hs::ActionEdge{1,hs::GameAction::SkillQ,hs::EdgeKind::Pressed}};
        hs::DomainSignal pulse{};
        std::uint32_t attributed_pulses{};
        std::uint32_t duplicate_pulses{};
        for(std::uint32_t step=0;step<100;++step)
        {
            input.target_tick=simulation.GetObservation().tick+1;
            input.ordered_edges=step==0 ? std::span<const hs::ActionEdge>(edge)
                                        : std::span<const hs::ActionEdge>{};
            (void)simulation.TickFixed(input,fixed_step);
            for(const auto &signal:simulation.PendingDomainSignals())
            {
                if(signal.kind!=hs::DomainSignalKind::FireAreaPulse ||
                   signal.upgrade_skill!=static_cast<std::uint8_t>(hs::SkillKind::ExplosiveArrow) ||
                   signal.upgrade_index!=3) continue;
                if(attributed_pulses==0) pulse=signal;
                if(signal.geometry.source_id==pulse.geometry.source_id &&
                   signal.tick==pulse.tick) ++duplicate_pulses;
                ++attributed_pulses;
            }
            simulation.ClearDomainSignals();
        }
        const auto expected_radius=simulation.Rules().upgrades.explosive_arrow
                                       .explosion_leaves_burning_area.radius;
        Check(attributed_pulses>0 && duplicate_pulses==1 &&
              pulse.upgrade_stage==hs::UpgradeVisualStage::Resolve &&
              pulse.upgrade_cast_id!=0 && pulse.upgrade_owner_id!=0 &&
              pulse.geometry.kind==hs::DomainSignalGeometryKind::Circle &&
              pulse.geometry.radius==expected_radius &&
              pulse.geometry.source_id==pulse.upgrade_owner_id &&
              pulse.tick>=pulse.geometry.start_tick &&
              pulse.tick<=pulse.geometry.end_tick,
              "actual explosive fire-area pulse lost its area geometry, owner, or tick");
        std::array<hs::PresentationEvent,2> projected{};
        const auto count=hs::ProjectDomainSignal(pulse,projected);
        std::vector<hs::PresentationEvent> output;
        Check(count==1 && hs::runtime_detail::DispatchVfxUpgradeEvents(
                  Program(),std::span(projected).first(count),output).Succeeded() &&
              output.size()==1 && output[0].asset.value==hs::MakeAssetId(
                  "particle.skill.fire_area.pulse").value &&
              output[0].tick==pulse.tick && output[0].geometry.radius==pulse.geometry.radius,
              "real explosive fire pulse did not dispatch through its existing cue");
        Check(simulation.Shutdown().Succeeded(),"explosive fire-area shutdown");
    }
    {
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0x7A4Au},rules).Succeeded(),
              "trap mark initialize");
        Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantSkill,
              static_cast<std::uint64_t>(hs::SkillKind::Trap)}).Succeeded() &&
              simulation.ApplyDebugCommand({hs::DebugCommandKind::GrantUpgrade,
              static_cast<std::uint64_t>(hs::SkillKind::Trap),6}).Succeeded() &&
              simulation.ApplyDebugCommand({hs::DebugCommandKind::SpawnEnemy,
              static_cast<std::uint64_t>(hs::EnemyKind::Melee),0,{0.0f,0.0f}}).Succeeded(),
              "trap mark setup");
        hs::InputFrame input;
        input.held.aim_world={20.0f,0.0f,0.0f};
        const std::array edge{hs::ActionEdge{1,hs::GameAction::SkillQ,hs::EdgeKind::Pressed}};
        bool mark_applied=false;
        hs::DomainSignal burst{};
        std::uint32_t burst_count{};
        std::uint32_t generic_mark_triggers{};
        for(std::uint32_t step=0;step<150;++step)
        {
            input.target_tick=simulation.GetObservation().tick+1;
            input.ordered_edges=step==0 ? std::span<const hs::ActionEdge>(edge)
                                        : std::span<const hs::ActionEdge>{};
            if(mark_applied)
            {
                input.held.basic_attack_held=true;
                input.held.aim_world={0.0f,0.0f,0.0f};
            }
            (void)simulation.TickFixed(input,fixed_step);
            for(const auto &signal:simulation.PendingDomainSignals())
            {
                if(signal.kind==hs::DomainSignalKind::MarkApplied)
                    mark_applied=true;
                if(signal.kind==hs::DomainSignalKind::MarkTriggered)
                {
                    if(signal.upgrade_skill==static_cast<std::uint8_t>(hs::SkillKind::Trap) &&
                       signal.upgrade_index==6 &&
                       signal.upgrade_stage==hs::UpgradeVisualStage::Resolve)
                    {
                        burst=signal;
                        ++burst_count;
                    }
                    else ++generic_mark_triggers;
                }
            }
            simulation.ClearDomainSignals();
        }
        Check(mark_applied && burst_count==1 && generic_mark_triggers==0 &&
              burst.upgrade_cast_id!=0 && burst.upgrade_owner_id!=0 &&
              burst.position.x==0.0f && burst.position.z==0.0f,
              "trap mark burst did not follow one real different-skill detonation");
        std::array<hs::PresentationEvent,3> projected{};
        const auto count=hs::ProjectDomainSignal(burst,projected);
        std::vector<hs::PresentationEvent> output;
        Check(count==2 && hs::runtime_detail::DispatchVfxUpgradeEvents(
                  Program(),std::span(projected).first(count),output).Succeeded() &&
              output.size()==2 && output[0].asset.value==hs::MakeAssetId(
                  "particle.upgrade.trap.mark_burst").value &&
              output[1].kind==hs::PresentationKind::Audio,
              "real trap mark burst duplicated its generic visual or lost trigger audio");
        Check(simulation.Shutdown().Succeeded(),"trap mark shutdown");
    }
}
}
int main()
{
    try {SimulationProjectionDispatchUsesZeroBasedUpgradeIndex();SimulationChargedShotPulseProjectsAndDispatches();SimulationEmitsSecondaryFanAfterSuccessfulVolley();ScheduledFanIngressUsesFrozenCastDirection();SimulationEmitsOuterArrowConeAfterSuccessfulProjectile();SimulationEmitsRetreatLandingShockwaveAfterSuccessfulLanding();SimulationEmitsCooldownProcForRealRefund();StagesAreIndependent();InvalidContractsFailWithoutPartialOutput();ExplicitAdditionalMappings();CooldownProcMappings();RealSlowAreaOwnersSelectAuthoredAliases();RealArrowRainPullProjectsAndDispatches();FourNewBindingsRequireExactStageAndCookedMembership();RealEmpoweredBasicAndRollTrapBindings();RealTrapRearmPullAndBleedBindings();RealFirePulseAndTrapMarkBindings();std::cout<<"VFX upgrade dispatch passed\n";return 0;}
    catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
