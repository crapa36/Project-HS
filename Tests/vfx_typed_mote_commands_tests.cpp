#include <hs/renderer/vfx_program_loader.hpp>
#include "../Source/Runtime/Private/vfx_typed_mote_commands.hpp"
#include "test_generation_paths.hpp"
#include <hs/render_data/render_snapshot.hpp>
#include <bit>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <vector>
namespace
{
constexpr std::uint32_t Hash(std::string_view s)
{ std::uint32_t h=2166136261u; for (const unsigned char c:s) {h^=c;h*=16777619u;}return h; }
constexpr std::uint64_t Hash64(std::string_view s)
{ std::uint64_t h=14695981039346656037ull;for(const unsigned char c:s){h^=c;h*=1099511628211ull;}return h; }
void Require(bool v,const char *message) {if(!v)throw std::runtime_error(message);}
bool Near(float a,float b) {return std::abs(a-b)<.00001f;}
hs::VfxImageData Prepared(const hs::VfxProgramData &program)
{
    hs::VfxImageData image; image.program=program;
    hs::PrepareVfxMoteRecipes(image.program.View(),image.mote_source_recipes,image.mote_output_recipes);
    return image;
}
hs::VfxProgramData Program()
{
    hs::VfxProgramData p;
    hs::VfxEffectRecord e; e.handle=1;e.payload_kind=Hash("PointEventPayload");e.seconds=.9f;e.sources={0,1};p.effects.push_back(e);
    p.effect_lookup.push_back({0xabc987123ull,1});
    hs::VfxSourceRecord s;s.effect=1;s.stable_id=Hash("ascending_motes");s.type=hs::VfxSourceType::CurlMotes;s.knot_count=2;
    s.knots={.05f,1,0,0};s.parameters={0,2};s.outputs={0,1};p.sources.push_back(s);
    p.parameters.push_back({Hash("count"),hs::VfxParameterType::Int,10});
    p.parameters.push_back({Hash("curl"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(.25f)});
    hs::VfxOutputRecord o;o.source=0;o.profile=hs::VfxOutputProfile::SpriteAdd;o.shape=Hash("soft_leaf_mote");o.motion=hs::VfxMotionKind::SlowUpwardHelix;
    o.rgba={.2f,.8f,.3f,.45f};o.hdr=1.8f;o.gradient_row=9;p.outputs.push_back(o);return p;
}
hs::VfxEventInput Input()
{
    hs::VfxEventInput in;in.effect_handle=1;in.event_tick=33;in.stable_seed=0xabcdef;
    hs::VfxPointPayload point;point.position={4,.25f,-7};point.authored_scale=2;in.payload=point;return in;
}
auto Decode(const hs::VfxProgramData &p,const hs::VfxEventInput &input)
{const auto image=Prepared(p);return hs::runtime_detail::BuildVfxTypedMoteCommands(image.View(),std::span(&input,1));}
void PreparedRoutesCloseAuthoredWindows()
{
    auto p=Program();
    auto image=Prepared(p);
    const auto &source=image.mote_source_recipes[0];
    const auto &output=image.mote_output_recipes[0];
    Require(source.effect_id==0xabc987123ull&&source.effect_lookup_count==1&&
        source.stable_id==Hash("ascending_motes")&&source.output_range.first==0&&
        source.output_range.count==1&&source.knots[0]==.05f&&source.knots[1]==1&&
        source.common_route_valid&&output.empty_parameters_valid&&output.common_recipe_index==0,
        "prepared Mote route identity/window missing");
    p=Program();p.sources[0].outputs={9,1};image=Prepared(p);
    Require(!image.mote_source_recipes[0].output_range_valid&&
        image.mote_output_recipes[0].common_recipe_index==0xffffffffu,
        "invalid authored output window entered a prepared Mote route");
    p=Program();p.outputs[0].parameters={9,1};image=Prepared(p);
    Require(!image.mote_output_recipes[0].empty_parameters_valid&&
        image.mote_output_recipes[0].common_recipe_index==0xffffffffu,
        "invalid authored parameter window entered a prepared Mote route");
}
void PreparedResidualAndTickRoutesRequireTwoKnots()
{
    hs::VfxProgramData residual;
    hs::VfxEffectRecord effect;effect.handle=1;residual.effects.push_back(effect);
    hs::VfxSourceRecord source;source.effect=1;source.stable_id=Hash("residual_motes");
    source.type=hs::VfxSourceType::CurlMotes;source.knot_count=2;source.knots={0,1,0,0};
    source.parameters={0,2};source.outputs={0,0};residual.sources.push_back(source);
    residual.parameters={{Hash("count"),hs::VfxParameterType::Int,4},
        {Hash("inward_speed"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(.5f)}};
    auto image=Prepared(residual);
    Require(image.mote_source_recipes[0].residual_route_valid,"valid residual source route was rejected");
    residual.sources[0].knot_count=4;image=Prepared(residual);
    Require(!image.mote_source_recipes[0].residual_route_valid,"residual route accepted a non-two-knot source");

    hs::VfxProgramData tick;
    effect={};effect.handle=1;tick.effects.push_back(effect);
    source={};source.effect=1;source.stable_id=Hash("tick_motes");source.type=hs::VfxSourceType::CurlMotes;
    source.knot_count=2;source.knots={0,1,0,0};source.parameters={0,1};source.outputs={0,0};tick.sources.push_back(source);
    tick.parameters.push_back({Hash("count"),hs::VfxParameterType::Int,3});
    image=Prepared(tick);
    Require(image.mote_source_recipes[0].tick_route_valid,"valid status tick source route was rejected");
    tick.sources[0].knot_count=0;image=Prepared(tick);
    Require(!image.mote_source_recipes[0].tick_route_valid,"status tick route accepted a non-two-knot source");
}
void BurstAndScale()
{
    auto p=Program();auto in=Input();const auto a=Decode(p,in);
    Require(a.size()==10,"authored burst count lost");
    Require(a[0].shape==hs::VfxImpactShape::LeafMote&&Near(a[0].size,.09f)&&Near(a[0].orbit_radius,.5f),"leaf scale or authored curl lost");
    Require(Near(a[0].delay,.045f)&&Near(a[0].lifetime,.855f)&&Near(a[0].initial_velocity.y,.9f),"source window or metre velocity changed");
    Require(a[0].gradient_row==9&&Near(a[0].color.w,.45f)&&a[0].event_tick==33&&Near(a[0].drag,.8f)&&Near(a[0].orbit_rate,2),"authored presentation lost");
    Require(a[0].stable_seed!=a[1].stable_seed&&!Near(a[0].initial_offset.x,a[1].initial_offset.x),"emission indices repeat");
    const auto repeat=Decode(p,in);Require(repeat[0].stable_seed==a[0].stable_seed&&repeat[0].initial_offset.x==a[0].initial_offset.x,"same event is not deterministic");
    ++in.stable_seed;Require(Decode(p,in)[0].stable_seed!=a[0].stable_seed,"event seed ignored");
    in=Input();p.effects[0].payload_kind=Hash("PresentationContextPayload");hs::VfxContextPayload ctx;ctx.ratio01=.01f;in.payload=ctx;in.world_transform[12]=12;
    const auto context=Decode(p,in);Require(context.size()==10&&Near(context[0].size,.045f)&&Near(context[0].orbit_radius,.25f)&&Near(context[0].position.x,12),"Context ratio used as scale");
    p.sources[0].parameters.count=1;Require(Near(Decode(p,in)[0].orbit_radius,.2f),"missing curl default changed");
}
hs::VfxProgramData StatusTickMoteProgram()
{
    hs::VfxProgramData p;
    hs::VfxEffectRecord tick; tick.handle=1; tick.payload_kind=Hash("PointEventPayload");
    tick.seconds=.16f; tick.sources={0,1}; p.effects.push_back(tick);
    hs::VfxEffectRecord status; status.handle=2; status.input_mode=1; status.timing_kind=1;
    status.payload_kind=Hash("EntityAttachmentPayload"); status.sources={1,1}; p.effects.push_back(status);
    p.effect_lookup={{Hash64("particle.status.bleed_tick"),1},
        {Hash64("persistent.status.bleed"),2}};
    hs::VfxSourceRecord event_source; event_source.effect=1; event_source.stable_id=Hash("event_motes");
    event_source.type=hs::VfxSourceType::CurlMotes; event_source.knot_count=2;
    event_source.knots={0,1,0,0}; event_source.parameters={0,3}; event_source.outputs={0,1};
    p.sources.push_back(event_source);
    hs::VfxSourceRecord owner_source; owner_source.effect=2; owner_source.stable_id=Hash("tick_motes");
    owner_source.type=hs::VfxSourceType::CurlMotes; owner_source.knot_count=2;
    owner_source.knots={0,1,0,0}; owner_source.parameters={3,1}; owner_source.outputs={1,1};
    p.sources.push_back(owner_source);
    p.parameters={{Hash("count"),hs::VfxParameterType::Int,3},
        {Hash("curl"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(.2f)},
        {Hash("drag"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(.8f)},
        {Hash("count"),hs::VfxParameterType::Int,3}};
    hs::VfxOutputRecord burst; burst.source=0; burst.profile=hs::VfxOutputProfile::SpriteAdd;
    burst.shape=Hash("soft_leaf_mote"); burst.motion=hs::VfxMotionKind::SlowUpwardHelix;
    burst.rgba={.2f,.5f,.8f,.7f}; burst.hdr=1; p.outputs.push_back(burst);
    hs::VfxOutputRecord tick_output; tick_output.source=1; tick_output.profile=hs::VfxOutputProfile::SpriteAdd;
    tick_output.shape=Hash("small_motes"); tick_output.motion=hs::VfxMotionKind::VeryShortLocalDrift;
    tick_output.shape_domain=Hash("sprite"); tick_output.shape_scale_rule=Hash("component_size");
    tick_output.shape_component_kind=Hash("particle"); tick_output.coverage_type=Hash("analytic");
    tick_output.coverage_ref=Hash("diamond_spark"); tick_output.rgba={.8f,.4f,.2f,.9f};
    tick_output.hdr=1.5f; tick_output.gradient_row=6; p.outputs.push_back(tick_output);
    return p;
}
void AppendAndConsumedSequenceSemantics()
{
    auto p=Program(); auto in=Input(); in.sequence=811;
    const auto expected=Decode(p,in);
    auto image=Prepared(p);
    hs::VfxFlashSpawnInput prefix; prefix.stable_seed=123;
    std::vector<hs::VfxFlashSpawnInput> appended{prefix};
    hs::runtime_detail::AppendVfxTypedMoteCommands(image.View(),std::span(&in,1),{},appended);
    Require(appended.size()==expected.size()+1&&appended.front().stable_seed==prefix.stable_seed&&
        appended[1].stable_seed==expected[0].stable_seed,"typed Mote append changed prefix or row order");
    const std::array consumed{in.sequence};
    hs::runtime_detail::AppendVfxTypedMoteCommands(image.View(),std::span(&in,1),consumed,appended);
    Require(appended.size()==expected.size()+1,"consumed event sequence was emitted again");

    auto tick_program=StatusTickMoteProgram(); auto tick_image=Prepared(tick_program);
    hs::VfxEventInput event; event.effect_handle=1; event.sequence=902; event.event_tick=51;
    event.stable_seed=55; event.geometry_owner_id=17; event.status_episode_generation=4;
    hs::VfxPointPayload point; point.position={1,2,3}; event.payload=point;
    auto duplicate=event; duplicate.geometry_owner_id=18;
    hs::VfxPersistentInput owner; owner.effect_handle=2; owner.stable_id=301;
    owner.status_episode_generation=4; owner.normalized_age=.5f; owner.current_transform[12]=4;
    hs::VfxEntityPayload entity; entity.render_instance_id=(2ull<<60)|17;
    entity.footprint_radius=.6f; entity.lifetime01=.5f; owner.payload=entity;
    const std::array events{event,duplicate}; const std::array owners{owner};
    std::vector<hs::VfxFlashSpawnInput> converted;
    std::vector<hs::Sequence> sequences;
    hs::runtime_detail::AppendVfxOwnedStatusTickMoteFlashes(
        tick_image.View(),events,owners,converted,sequences);
    Require(converted.size()==3&&sequences==std::vector<hs::Sequence>(3,event.sequence),
        "owned status Mote conversion did not report each consumed output row");
    std::ranges::sort(sequences); sequences.erase(std::unique(sequences.begin(),sequences.end()),sequences.end());
    std::vector<hs::VfxFlashSpawnInput> unpaired;
    hs::runtime_detail::AppendVfxTypedMoteCommands(tick_image.View(),events,sequences,unpaired);
    Require(unpaired.empty(),"duplicate event sequence escaped owned status pairing");
    const auto returning=hs::runtime_detail::BuildVfxOwnedStatusTickMoteCommands(
        tick_image.View(),events,owners);
    Require(returning.size()==converted.size()&&returning[0].event_sequence==event.sequence&&
        returning.back().flash.stable_seed==converted.back().stable_seed,
        "returning owned Mote helper diverged from append output");
}
void CatalogOrderAndMotions()
{
    auto p=Program();auto in=Input();auto before=Decode(p,in);
    const auto old_effect=p.effects[0];p.effects.push_back(old_effect);p.effects[1].handle=2;p.effects[1].sources={1,1};
    p.sources.push_back(p.sources[0]);p.sources[1].effect=2;p.sources[1].outputs={1,1};p.outputs.push_back(p.outputs[0]);p.outputs[1].source=1;
    p.effect_lookup[0].handle=2;in.effect_handle=2;
    auto after=Decode(p,in);Require(after.size()==10&&before[0].stable_seed==after[0].stable_seed&&before[0].initial_offset.x==after[0].initial_offset.x,"catalog reorder changed random particles");
    p=Program();in=Input();p.sources[0].parameters.count=1;p.parameters[0].bits=3;p.sources[0].knots={0,1,0,0};p.effects[0].seconds=.32f;
    p.outputs[0].shape=Hash("small_motes");p.outputs[0].motion=hs::VfxMotionKind::VeryShortLocalDrift;
    auto small=Decode(p,in);Require(small.size()==3&&Near(small[0].delay,0)&&Near(small[0].lifetime,.32f)&&Near(small[0].orbit_radius,0)&&Near(small[0].size,.05f),"small mote defaults wrong");
    p.parameters[0].bits=7;p.outputs[0].shape=Hash("diamond_motes");p.outputs[0].motion=hs::VfxMotionKind::ShortUpwardSpiral;p.outputs[0].min_quality=1;
    auto diamond=Decode(p,in);Require(diamond.size()==7&&Near(diamond[0].size,.07f)&&Near(diamond[0].orbit_radius,.3f)&&Near(diamond[0].orbit_rate,5),"diamond defaults wrong");
    in.quality=hs::VfxQuality::Low;Require(Decode(p,in).empty(),"quality filter ignored");
}
void Malformed()
{
    auto p=Program();auto in=Input();p.effect_lookup.clear();Require(Decode(p,in).empty(),"missing semantic identity accepted");
    p=Program();p.parameters[0].bits=0;Require(Decode(p,in).empty(),"zero count accepted");p.parameters[0].bits=0xffffffff;Require(Decode(p,in).empty(),"negative count accepted");
    p=Program();p.parameters[1].bits=std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity());Require(Decode(p,in).empty(),"infinite curl accepted");
    p=Program();p.sources[0].knot_count=4;Require(Decode(p,in).empty(),"unsupported four knot envelope accepted");
    p=Program();p.effects[0].timing_kind=1;Require(Decode(p,in).empty(),"nonfixed source accepted");
    p=Program();p.outputs[0].profile=hs::VfxOutputProfile::SpriteSdfAdd;Require(Decode(p,in).empty(),"wrong profile accepted");
    p=Program();p.outputs[0].motion=hs::VfxMotionKind::RareInwardDrift;Require(Decode(p,in).empty(),"wrong motion accepted");
    p=Program();p.sources[0].parameters.count=100;Require(Decode(p,in).empty(),"bad parameter span accepted");
}
hs::VfxProgramData OwnedProgram()
{
    auto p=Program(); p.effects[0].payload_kind=Hash("SafeGapSectorPayload");p.effects[0].timing_kind=1;
    p.sources[0].knots={0,1,0,0};p.sources[0].parameters={0,1};
    p.parameters[0]={Hash("count_per_second"),hs::VfxParameterType::Int,3};
    p.outputs[0].shape=Hash("soft_motes");p.outputs[0].motion=hs::VfxMotionKind::RareInwardDrift;
    p.outputs[0].min_quality=1;return p;
}
hs::VfxEventInput OwnedInput()
{
    auto in=Input();in.geometry_owner_id=77;in.geometry_start_tick=100;in.geometry_end_tick=220;
    hs::VfxSafeSectorPayload sector;sector.center={4,.25f,-7};sector.direction={1,0,0};
    sector.inner_radius=2;sector.outer_radius=8;sector.half_angle_degrees=15;in.payload=sector;return in;
}
auto Owned(const hs::VfxProgramData &p,const hs::VfxEventInput &in,double tick)
{const auto image=Prepared(p);return hs::runtime_detail::BuildVfxOwnedMoteCommands(image.View(),std::span(&in,1),tick);}
auto Owned(const hs::VfxProgramData &p,std::span<const hs::VfxEventInput> inputs,double tick)
{const auto image=Prepared(p);return hs::runtime_detail::BuildVfxOwnedMoteCommands(image.View(),inputs,tick);}
auto Persistent(const hs::VfxProgramData &p,std::span<const hs::VfxPersistentInput> owners)
{const auto image=Prepared(p);return hs::runtime_detail::BuildVfxPersistentMoteCommands(image.View(),owners);}
auto EmitSparks(hs::runtime_detail::VfxPersistentSparkState &state,const hs::VfxProgramData &p,
    std::span<const hs::VfxPersistentInput> owners,hs::Tick tick,float interpolation=1)
{const auto image=Prepared(p);return state.Emit(image.View(),owners,tick,interpolation);}
void OwnedRateAndClock()
{
    auto p=OwnedProgram();auto in=OwnedInput();
    Require(Owned(p,in,99).empty()&&Owned(p,in,220).empty(),"owner boundaries ignored");
    const auto birth=Owned(p,in,100);Require(birth.size()==1&&Near(birth[0].normalized_age,0),"integer rate first birth missing");
    const auto at20=Owned(p,in,120);Require(at20.size()==2,"integer rate cadence wrong");
    auto image=Prepared(p); hs::VfxFlashSpawnInput prefix; prefix.stable_seed=333;
    std::vector<hs::VfxFlashSpawnInput> appended{prefix};
    hs::runtime_detail::AppendVfxOwnedMoteCommands(image.View(),std::span(&in,1),120,appended);
    Require(appended.size()==at20.size()+1&&appended.front().stable_seed==prefix.stable_seed&&
        appended[1].stable_seed==at20[0].stable_seed,"owned Mote append changed caller order");
    const auto between=Owned(p,in,119.5);
    Require(between.size()==1&&between[0].stable_seed==birth[0].stable_seed&&
            between[0].normalized_age>birth[0].normalized_age,
            "fractional owner age admitted a future birth or changed its ordinal");
    const auto at38=Owned(p,in,138),at39=Owned(p,in,139),at40=Owned(p,in,140);
    Require(at38.size()==2&&at39.size()==1&&at40.size()==2,"particle lifetime boundary or next birth incorrect");
    Require(at39[0].stable_seed==at20[1].stable_seed&&at39[0].position.x==at20[1].position.x&&
            at39[0].normalized_age>at20[1].normalized_age,"live ordinal changed random identity between frames");
    const auto repeat=Owned(p,in,140);Require(repeat.size()==at40.size()&&repeat[0].stable_seed==at40[0].stable_seed&&
        repeat[0].normalized_age==at40[0].normalized_age,"same frame evaluation accumulated emissions");
    for(const auto &mote:at40)
    {
        const float x=mote.position.x-4,z=mote.position.z+7;
        Require(std::hypot(x,z)>2&&std::hypot(x,z)<8&&std::abs(std::atan2(z,x))<15*.0174533f&&
            x*mote.initial_velocity.x+z*mote.initial_velocity.z<0,"rate mote escaped safe sector or drifted outward");
    }
    auto reordered=p;reordered.effects.push_back(p.effects[0]);reordered.effects[1].handle=2;
    reordered.sources[0].effect=2;reordered.effect_lookup[0].handle=2;in.effect_handle=2;
    const auto moved=Owned(reordered,in,140);Require(moved.size()==at40.size()&&moved[0].stable_seed==at40[0].stable_seed&&
        moved[0].position.x==at40[0].position.x,"catalog handle reorder changed emission identity");
    in=OwnedInput();p.sources[0].knots={.25f,.5f,0,0};
    Require(Owned(p,in,129).empty()&&Owned(p,in,130).size()==1&&Owned(p,in,150).size()==2,
        "authored source interval start ignored");
    Require(Owned(p,in,160).size()==2&&Owned(p,in,169).size()==1&&Owned(p,in,189).empty(),
        "source interval emitted after end or discarded valid tails");
    Require(Owned(p,std::span<const hs::VfxEventInput>{},150).empty(),"cancelled owner left rate particles");
}
void SafeGapSectorAcceptsNonAmbientSourceId()
{
    auto p=OwnedProgram();
    p.sources[0].stable_id=Hash("safe_gap_rate_source");
    const auto in=OwnedInput();
    const auto first=Owned(p,in,120),repeat=Owned(p,in,120);
    Require(first.size()==2,"SafeGapSector rejected a valid non-ambient rate source");
    Require(first[0].stable_seed==repeat[0].stable_seed&&first[1].stable_seed==repeat[1].stable_seed&&
        first[0].stable_seed!=first[1].stable_seed,"non-ambient source birth seeds changed or repeated");
}
void OwnedMalformedAndQuality()
{
    auto p=OwnedProgram();auto in=OwnedInput();in.quality=hs::VfxQuality::Low;
    Require(Owned(p,in,120).empty(),"owned mote quality filter ignored");in=OwnedInput();
    p.parameters[0].type=hs::VfxParameterType::Float;p.parameters[0].bits=std::bit_cast<std::uint32_t>(3.0f);
    Require(Owned(p,in,120).empty(),"float substituted for authored integer rate");
    p=OwnedProgram();p.parameters[0].bits=0xffffffffu;Require(Owned(p,in,120).empty(),"negative integer rate accepted");
    p=OwnedProgram();p.parameters[0].bits=0;Require(Owned(p,in,120).empty(),"zero rate accepted");
    p=OwnedProgram();std::get<hs::VfxSafeSectorPayload>(in.payload).direction={};
    Require(Owned(p,in,120).empty(),"zero direction accepted");in=OwnedInput();
    std::get<hs::VfxSafeSectorPayload>(in.payload).outer_radius=std::numeric_limits<float>::infinity();
    Require(Owned(p,in,120).empty(),"infinite geometry accepted");in=OwnedInput();
    in.geometry_owner_id=0;Require(Owned(p,in,120).empty(),"unowned rate source accepted");in=OwnedInput();
    p.sources[0].knots[1]=p.sources[0].knots[0];Require(Owned(p,in,120).empty(),"empty source interval accepted");
    p=OwnedProgram();p.effects[0].timing_kind=0;Require(Owned(p,in,120).empty(),"fixed timing substituted for owner clock");
}

hs::VfxProgramData PersistentMoteProgram()
{
    hs::VfxProgramData program;
    hs::VfxEffectRecord effect;
    effect.handle = 1;
    effect.input_mode = 1;
    effect.timing_kind = 1;
    effect.payload_kind = Hash("EntityAttachmentPayload");
    effect.sources = {0, 1};
    program.effects.push_back(effect);
    program.effect_lookup.push_back({Hash64("persistent.status.slow"), 1});
    hs::VfxSourceRecord source;
    source.effect = 1;
    source.stable_id = Hash("ambient_motes");
    source.type = hs::VfxSourceType::CurlMotes;
    source.knot_count = 2;
    source.knots = {0, 1, 0, 0};
    source.parameters = {0, 1};
    source.outputs = {0, 1};
    program.sources.push_back(source);
    program.parameters.push_back({Hash("count_per_second"), hs::VfxParameterType::Int, 3});
    hs::VfxOutputRecord output;
    output.source = 0;
    output.profile = hs::VfxOutputProfile::SpriteAdd;
    output.shape = Hash("soft_motes");
    output.shape_domain = Hash("sprite");
    output.shape_scale_rule = Hash("component_size");
    output.shape_component_kind = Hash("particle");
    output.coverage_type = Hash("analytic");
    output.coverage_ref = Hash("soft_disc");
    output.motion = hs::VfxMotionKind::RareInwardDrift;
    output.min_quality = static_cast<std::uint32_t>(hs::VfxQuality::Medium);
    output.rgba = {.1f, .25f, 1.0f, .25f};
    output.hdr = 1.2f;
    output.gradient_row = 8;
    program.outputs.push_back(output);
    return program;
}

hs::VfxPersistentInput PersistentMoteOwner(std::uint32_t elapsed_tick)
{
    hs::VfxPersistentInput owner;
    owner.stable_id = 0x12345678u;
    owner.effect_handle = 1;
    owner.stable_seed = 0xabcdefu;
    owner.quality = hs::VfxQuality::Medium;
    owner.elapsed_seconds = static_cast<float>(elapsed_tick) / 60.0f;
    owner.elapsed_ticks = elapsed_tick;
    owner.current_transform[12] = 4.0f;
    owner.current_transform[13] = .25f;
    owner.current_transform[14] = -2.0f;
    owner.normalized_age = static_cast<float>(elapsed_tick) / 120.0f;
    owner.payload = hs::VfxEntityPayload{.8f, owner.normalized_age, 1.0f, 1u, 77u};
    return owner;
}

auto PersistentMotes(const hs::VfxProgramData &program,
                     const hs::VfxPersistentInput &owner)
{
    return Persistent(program, std::span(&owner, 1));
}

void PersistentMotesRecomputeDeterministicBirthsAndOwnerBoundGeometry()
{
    auto program = PersistentMoteProgram();
    auto owner = PersistentMoteOwner(20);
    const auto at20 = Persistent(program, std::span(&owner, 1));
    Require(at20.size() == 2 && at20[0].stable_seed != at20[1].stable_seed,
            "persistent mote birth cadence or indexed identity missing");
    const auto mix = [](std::uint32_t value) {
        value ^= value >> 16; value *= 0x7feb352du; value ^= value >> 15;
        value *= 0x846ca68bu; return value ^ (value >> 16);
    };
    const auto random = [&](std::uint32_t seed, std::uint32_t lane) {
        return static_cast<float>(mix(seed ^ mix(lane + 0x9e3779b9u)) >> 8) *
            (1.0f / 16777216.0f);
    };
    const auto effect_id = Hash64("persistent.status.slow");
    const auto source_id = Hash("ambient_motes");
    const auto source_seed = mix(owner.stable_seed ^ mix(static_cast<std::uint32_t>(effect_id)) ^
        mix(static_cast<std::uint32_t>(effect_id >> 32)) ^ mix(source_id));
    for (const auto &mote : at20)
    {
        const auto ordinal = mote.normalized_age == 0.0f ? 1u : 0u;
        const auto seed = mix(source_seed ^ mix(ordinal + 1u));
        const float angle = 2.0f * std::numbers::pi_v<float> * random(seed, 0);
        const float radius = .8f * (.45f + .4f * random(seed, 1));
        Require(mote.stable_seed == seed &&
                Near(mote.position.x + mote.initial_offset.x, 4.0f + std::cos(angle) * radius) &&
                Near(mote.position.z + mote.initial_offset.z, -2.0f + std::sin(angle) * radius),
                "persistent mote random position differs from direct seed calculation");
    }
    auto append_image=Prepared(program);
    hs::VfxFlashSpawnInput prefix; prefix.stable_seed=777;
    std::vector<hs::VfxFlashSpawnInput> appended{prefix};
    hs::runtime_detail::AppendVfxPersistentMoteCommands(
        append_image.View(),std::span(&owner,1),appended);
    Require(appended.size()==at20.size()+1&&appended.front().stable_seed==prefix.stable_seed&&
        appended[1].stable_seed==at20[0].stable_seed,
        "persistent Mote append changed caller prefix or row order");
    owner.normalized_age=1.0f;
    std::get<hs::VfxEntityPayload>(owner.payload).lifetime01=1.0f;
    hs::runtime_detail::AppendVfxPersistentMoteCommands(
        append_image.View(),std::span(&owner,1),appended);
    Require(appended.size()==at20.size()+1,"rejected persistent owner partially appended motes");
    owner=PersistentMoteOwner(20);
    const auto repeat = Persistent(program, std::span(&owner, 1));
    Require(repeat.size() == at20.size() && repeat[0].stable_seed == at20[0].stable_seed &&
            repeat[0].normalized_age == at20[0].normalized_age,
            "stateless persistent evaluation accumulated or changed births");
    auto fractional_owner = owner;
    fractional_owner.elapsed_seconds = 20.5f / 60.0f;
    fractional_owner.elapsed_ticks = 20.5;
    fractional_owner.normalized_age = 20.5f / 120.0f;
    std::get<hs::VfxEntityPayload>(fractional_owner.payload).lifetime01 = fractional_owner.normalized_age;
    const auto fractional = Persistent(program, std::span(&fractional_owner, 1));
    Require(fractional.size() == at20.size() && fractional[0].stable_seed == at20[0].stable_seed &&
            fractional[0].normalized_age > at20[0].normalized_age,
            "fractional display time dropped or regenerated persistent motes");
    owner = PersistentMoteOwner(40);
    const auto at40 = Persistent(program, std::span(&owner, 1));
    Require(at40.size() == 2 && at40[0].stable_seed == at20[1].stable_seed,
            "skipped persistent frame changed the live ordinal");
    owner.current_transform[12] = 12.0f;
    const auto moved = Persistent(program, std::span(&owner, 1));
    Require(moved.size() == at40.size() && moved[0].stable_seed == at40[0].stable_seed &&
            Near(moved[0].position.x, 12.0f),
            "persistent motes did not use the current owner transform");
    owner = PersistentMoteOwner(20);
    const auto owner_clock_rewind = Persistent(program, std::span(&owner, 1));
    Require(owner_clock_rewind.size() == at20.size() &&
            owner_clock_rewind[0].stable_seed == at20[0].stable_seed &&
            owner_clock_rewind[0].normalized_age == at20[0].normalized_age &&
            owner_clock_rewind[0].initial_offset.x == at20[0].initial_offset.x,
            "nonmonotonic owner age changed deterministic persistent output");
    auto reordered = program;
    auto extra_effect = reordered.effects.front();
    extra_effect.handle = 2;
    extra_effect.sources = {1, 1};
    reordered.effects.push_back(extra_effect);
    auto extra_source = reordered.sources.front();
    extra_source.effect = 2;
    extra_source.outputs = {1, 1};
    reordered.sources.push_back(extra_source);
    auto extra_output = reordered.outputs.front();
    extra_output.source = 1;
    reordered.outputs.push_back(extra_output);
    reordered.effect_lookup[0].handle = 2;
    owner.effect_handle = 2;
    const auto catalog_moved = PersistentMotes(reordered, owner);
    Require(catalog_moved.size() == moved.size() &&
            catalog_moved[0].stable_seed == moved[0].stable_seed,
            "catalog reorder changed persistent mote identity");

    owner.effect_handle = 1;
    owner.normalized_age = 1.0f;
    std::get<hs::VfxEntityPayload>(owner.payload).lifetime01 = 1.0f;
    Require(PersistentMotes(program, owner).empty(),
            "expired persistent owner still emitted motes");
}

void PersistentMotesRejectMalformedOwners()
{
    auto program = PersistentMoteProgram();
    auto owner = PersistentMoteOwner(20);
    owner.payload = hs::VfxPointPayload{};
    Require(PersistentMotes(program, owner).empty(), "wrong owner payload accepted");
    owner = PersistentMoteOwner(20);
    program.effect_lookup[0].effect_id = Hash64("persistent.status.bleed");
    Require(PersistentMotes(program, owner).empty(), "wrong persistent identity accepted");
    program = PersistentMoteProgram();
    owner = PersistentMoteOwner(20);
    program.parameters[0].type = hs::VfxParameterType::Float;
    program.parameters[0].bits = std::bit_cast<std::uint32_t>(3.0f);
    Require(PersistentMotes(program, owner).empty(), "noninteger mote rate accepted");
    program = PersistentMoteProgram();
    owner = PersistentMoteOwner(20);
    program.outputs[0].motion = hs::VfxMotionKind::RareInwardDrift;
    program.outputs[0].coverage_ref = Hash("wrong_coverage");
    Require(PersistentMotes(program, owner).empty(), "wrong mote coverage accepted");
    program = PersistentMoteProgram();
    owner = PersistentMoteOwner(20);
    owner.current_transform[12] = std::numeric_limits<float>::quiet_NaN();
    Require(PersistentMotes(program, owner).empty(), "nonfinite owner transform accepted");
    owner = PersistentMoteOwner(20);
    program.effect_lookup[0].effect_id = Hash64("particle.pickup.xp.idle");
    owner.normalized_age = 0.0f;
    std::get<hs::VfxEntityPayload>(owner.payload).lifetime01 = 0.0f;
    owner.elapsed_seconds = 2.0e9f;
    owner.elapsed_ticks = static_cast<double>(owner.elapsed_seconds) * 60.0;
    Require(PersistentMotes(program, owner).empty(),
            "persistent mote ordinal overflow was narrowed into a repeated seed");
    program = PersistentMoteProgram();
    owner = PersistentMoteOwner(20);
    std::get<hs::VfxEntityPayload>(owner.payload).render_instance_id = 0;
    Require(PersistentMotes(program, owner).empty(), "unbound entity owner accepted");
}

void ChargedOverchargeMotesUseAuthoredMediumRecipe()
{
    auto program = PersistentMoteProgram();
    program.effect_lookup[0].effect_id =
        Hash64("particle.upgrade.charged.overcharge_loop");
    auto owner = PersistentMoteOwner(30);
    owner.normalized_age = .75f;
    owner.source_duration_seconds = 1.0f;
    std::get<hs::VfxEntityPayload>(owner.payload).lifetime01 = .75f;
    const auto motes = PersistentMotes(program, owner);
    Require(motes.size() == 2 && motes[0].shape == hs::VfxImpactShape::SoftDisc &&
                Near(motes[0].size, .04f) && Near(motes[0].normalized_age, 30.0f / 39.0f),
            "charged overcharge ambient motes did not follow the owner clock");

    owner = PersistentMoteOwner(50);
    owner.normalized_age = 1.0f;
    owner.source_duration_seconds = 1.0f;
    std::get<hs::VfxEntityPayload>(owner.payload).lifetime01 = 1.0f;
    Require(!PersistentMotes(program, owner).empty(),
            "charged overcharge ambient motes stopped at a live ratio of one");

    owner.quality = hs::VfxQuality::Low;
    Require(PersistentMotes(program, owner).empty(),
            "charged overcharge ambient motes appeared below authored medium quality");
    program = PersistentMoteProgram();
    program.effect_lookup[0].effect_id =
        Hash64("particle.upgrade.charged.overcharge_loop");
    program.parameters[0].bits = 4;
    owner = PersistentMoteOwner(30);
    owner.normalized_age = .75f;
    owner.source_duration_seconds = 1.0f;
    std::get<hs::VfxEntityPayload>(owner.payload).lifetime01 = .75f;
    Require(PersistentMotes(program, owner).empty(),
            "charged overcharge ambient motes accepted a non-authored cadence");
}

constexpr std::array<std::string_view, 4> kPickupIdleEffects{
    "particle.pickup.xp.idle", "particle.pickup.heal.idle",
    "particle.pickup.magnet.idle", "particle.pickup.relic_chest.idle"};

void PickupIdleMotesFollowPresentEntity()
{
    for (const auto effect_id : kPickupIdleEffects)
    {
        auto program = PersistentMoteProgram();
        program.effect_lookup[0].effect_id = Hash64(effect_id);
        auto owner = PersistentMoteOwner(20);
        const auto at20 = PersistentMotes(program, owner);
        Require(at20.size() == 2 && at20[0].shape == hs::VfxImpactShape::SoftDisc &&
                Near(at20[0].size, .04f) && Near(at20[0].position.x, 4.0f) &&
                Near(at20[0].normalized_age, 20.0f / 39.0f) &&
                Near(at20[1].normalized_age, 0.0f),
                "pickup idle motes did not use the authored entity cadence");
        auto presence = owner;
        presence.normalized_age = 0.0f;
        std::get<hs::VfxEntityPayload>(presence.payload).lifetime01 = 0.0f;
        const auto live = PersistentMotes(program, presence);
        Require(live.size() == at20.size() && live[0].stable_seed == at20[0].stable_seed &&
                std::isfinite(live[0].normalized_age),
                "live pickup presence produced no births or a nonfinite source age");
        const auto repeat = PersistentMotes(program, owner);
        Require(repeat.size() == at20.size() &&
                repeat[0].stable_seed == at20[0].stable_seed,
                "pickup idle motes accumulated between identical frames");

        owner = PersistentMoteOwner(40);
        const auto at40 = PersistentMotes(program, owner);
        Require(at40.size() == 2 && at40[0].stable_seed == at20[1].stable_seed,
                "pickup idle motes did not retain live birth identity");
        owner.current_transform[12] = 12.0f;
        std::get<hs::VfxEntityPayload>(owner.payload).footprint_radius = 1.6f;
        const auto moved = PersistentMotes(program, owner);
        Require(moved.size() == at40.size() &&
                moved[0].stable_seed == at40[0].stable_seed &&
                Near(moved[0].position.x, 12.0f) &&
                Near(moved[0].size, .08f) &&
                Near(std::hypot(moved[0].initial_offset.x, moved[0].initial_offset.z),
                     2.0f * std::hypot(at40[0].initial_offset.x, at40[0].initial_offset.z)),
                "pickup idle motes did not use live position and footprint");
        owner.quality = hs::VfxQuality::Low;
        Require(PersistentMotes(program, owner).empty(),
                "pickup idle motes appeared at low quality");
    }
    auto pickup_program = PersistentMoteProgram();
    pickup_program.effect_lookup[0].effect_id = Hash64(kPickupIdleEffects[0]);
    Require(Persistent(pickup_program, {}).empty(),
            "absent pickup owner retained motes");
}

void PickupIdleMotesRejectMalformedRecipes()
{
    auto program = PersistentMoteProgram();
    program.effect_lookup[0].effect_id = Hash64(kPickupIdleEffects[0]);
    const auto owner = PersistentMoteOwner(20);
    program.parameters[0].bits = 4;
    Require(PersistentMotes(program, owner).empty(),
            "pickup idle accepted a different authored mote rate");
    program = PersistentMoteProgram();
    program.effect_lookup[0].effect_id = Hash64(kPickupIdleEffects[0]);
    program.outputs[0].min_quality = static_cast<std::uint32_t>(hs::VfxQuality::Low);
    Require(PersistentMotes(program, owner).empty(),
            "pickup idle accepted a low-quality output recipe");
    program = PersistentMoteProgram();
    program.effect_lookup[0].effect_id = Hash64(kPickupIdleEffects[0]);
    program.sources[0].knots[1] = .8f;
    Require(PersistentMotes(program, owner).empty(),
            "pickup idle accepted an expiring source interval");
}

hs::VfxProgramData PersistentTrapMoteProgram()
{
    auto program = PersistentMoteProgram();
    program.effects[0].payload_kind = Hash("CircleAreaPayload");
    program.effect_lookup[0].effect_id = Hash64("persistent.trap.armed");
    return program;
}

hs::VfxPersistentInput PersistentTrapMoteOwner(std::uint32_t elapsed_tick)
{
    auto owner = PersistentMoteOwner(elapsed_tick);
    owner.source_visual_kind = static_cast<std::uint8_t>(hs::PersistentVfxKind::TrapArmed);
    constexpr float duration_ticks = 120.0f;
    owner.normalized_age = static_cast<float>(elapsed_tick) / duration_ticks;
    hs::VfxCirclePayload circle;
    circle.center = {4.0f, 0.25f, -2.0f};
    circle.direction = {0.0f, 0.0f, 1.0f};
    circle.radius = 2.0f;
    circle.lifetime01 = owner.normalized_age;
    owner.payload = circle;
    return owner;
}

void EventTrapMotesUsePointClock()
{
    auto program = PersistentTrapMoteProgram();
    program.effects[0].input_mode = program.effects[0].timing_kind = 0;
    program.effects[0].seconds = .65f;
    program.effects[0].payload_kind = Hash("PointEventPayload");
    program.effect_lookup[0].effect_id = Hash64("particle.skill.trap");
    hs::VfxEventInput event;
    event.effect_handle = 1; event.sequence = 7; event.event_tick = 100; event.stable_seed = 99;
    hs::VfxPointPayload point; point.position = {4, .25f, -2}; point.authored_scale = 2;
    event.payload = point;
    const auto at20 = Owned(program, event, 120);
    Require(at20.size() == 2 && Near(at20[0].position.x, 4) && Near(at20[0].size, .1f) &&
            Near(at20[0].normalized_age, 20.0f / 39.0f) && Near(at20[1].normalized_age, 0),
            "event trap lost actual point scale, rate births or event clock");
    Require(Owned(program, event, 99).empty() && Owned(program, event, 139).empty(),
            "event trap emitted outside its fixed 39-tick interval");
    event.payload = hs::VfxCirclePayload{};
    Require(Owned(program, event, 120).empty(), "event trap accepted circle substitution");
}

void PersistentTrapMotesUseCircleClockGeometry()
{
    const auto program = PersistentTrapMoteProgram();
    auto owner = PersistentTrapMoteOwner(20);
    const auto at20 = PersistentMotes(program, owner);
    Require(at20.size() == 2 && at20[0].shape == hs::VfxImpactShape::SoftDisc &&
            Near(at20[0].position.x, 4.0f) && Near(at20[0].position.y, 0.25f) &&
            Near(at20[0].position.z, -2.0f) && Near(at20[0].lifetime, 39.0f / 60.0f),
            "trap ambient motes did not decode from the live circle owner");
    Require(at20[0].stable_seed != at20[1].stable_seed &&
            Near(at20[0].normalized_age, 20.0f / 39.0f) && Near(at20[1].normalized_age, 0.0f),
            "trap ambient mote cadence or lifetime is not tick based");
    const auto at39 = PersistentMotes(program, PersistentTrapMoteOwner(39));
    const auto at40 = PersistentMotes(program, PersistentTrapMoteOwner(40));
    Require(at39.size() == 1 && at40.size() == 2 &&
            at39[0].stable_seed == at20[1].stable_seed &&
            at40[0].stable_seed == at20[1].stable_seed,
            "trap ambient mote lifetime boundary changed the live ordinal set");

    owner = PersistentTrapMoteOwner(40);
    auto moved_owner = owner;
    std::get<hs::VfxCirclePayload>(moved_owner.payload).center.x = 12.0f;
    moved_owner.current_transform[12] = std::numeric_limits<float>::quiet_NaN();
    const auto moved = PersistentMotes(program, moved_owner);
    Require(moved.size() == at40.size() && moved[0].stable_seed == at40[0].stable_seed &&
            Near(moved[0].position.x, 12.0f) && Near(moved[0].position.z, -2.0f),
            "trap ambient motes did not follow the live circle center");
}

void PersistentTrapMotesRejectMalformedRecipesAndQuality()
{
    auto program = PersistentTrapMoteProgram();
    auto owner = PersistentTrapMoteOwner(20);
    owner.quality = hs::VfxQuality::Low;
    Require(PersistentMotes(program, owner).empty(), "low quality trap motes were not filtered");

    program = PersistentTrapMoteProgram();
    owner = PersistentTrapMoteOwner(20);
    program.effect_lookup[0].effect_id = Hash64("persistent.trap.pending");
    Require(PersistentMotes(program, owner).empty(), "pending trap recipe used the armed clock");
    owner.source_visual_kind = static_cast<std::uint8_t>(hs::PersistentVfxKind::TrapPending);
    Require(!PersistentMotes(program, owner).empty(), "actual pending trap clock was not admitted");
    program = PersistentTrapMoteProgram();
    owner = PersistentTrapMoteOwner(20);
    std::get<hs::VfxCirclePayload>(owner.payload).radius =
        std::numeric_limits<float>::quiet_NaN();
    Require(PersistentMotes(program, owner).empty(), "nonfinite trap circle accepted");
    program = PersistentTrapMoteProgram();
    owner = PersistentTrapMoteOwner(20);
    program.parameters[0].bits = 4;
    Require(PersistentMotes(program, owner).empty(), "wrong trap mote rate accepted");
    program = PersistentTrapMoteProgram();
    owner = PersistentTrapMoteOwner(20);
    program.outputs[0].motion = hs::VfxMotionKind::SlowUpwardHelix;
    Require(PersistentMotes(program, owner).empty(), "wrong trap mote motion accepted");
    program = PersistentTrapMoteProgram();
    owner = PersistentTrapMoteOwner(20);
    program.outputs[0].profile = hs::VfxOutputProfile::SpriteSdfAdd;
    Require(PersistentMotes(program, owner).empty(), "wrong trap mote output profile accepted");
    program = PersistentTrapMoteProgram();
    owner = PersistentTrapMoteOwner(20);
    std::get<hs::VfxCirclePayload>(owner.payload).lifetime01 = 0.5f;
    Require(PersistentMotes(program, owner).empty(), "circle age mismatch accepted");
}

void CookedPersistentTrapMotesConvert()
{
    const auto path = test_generation::ProductPath("vfx/vfx_program.hsbin");
    Require(std::filesystem::exists(path), "cooked VFX program missing for trap motes");
    std::ifstream stream(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(stream),
                                  std::istreambuf_iterator<char>()};
    hs::VfxProgramData program;
    std::string error;
    Require(hs::LoadVfxProgram(std::as_bytes(std::span(bytes)), program, error),
            "cooked VFX program failed to load for trap motes");

    const auto identity = std::find_if(
        program.effect_lookup.begin(), program.effect_lookup.end(),
        [](const auto &entry) { return entry.effect_id == Hash64("persistent.trap.armed"); });
    Require(identity != program.effect_lookup.end(),
            "cooked trap armed effect lookup missing");
    Require(identity->handle > 0 && identity->handle <= program.effects.size(),
            "cooked trap armed effect handle is invalid");
    Require(std::count_if(
                program.effect_lookup.begin(), program.effect_lookup.end(),
                [](const auto &entry) {
                    return entry.effect_id == Hash64("persistent.trap.armed");
                }) == 1,
            "cooked trap armed effect lookup was duplicated");

    hs::VfxPersistentInput owner;
    owner.stable_id = 0x13579bdu;
    owner.stable_seed = 0x2468aceu;
    owner.effect_handle = identity->handle;
    owner.source_visual_kind = static_cast<std::uint8_t>(hs::PersistentVfxKind::TrapArmed);
    owner.elapsed_seconds = 20.0f / 60.0f;
    owner.elapsed_ticks = 20.0;
    owner.normalized_age = 0.25f;
    owner.quality = hs::VfxQuality::Medium;
    owner.payload = hs::VfxCirclePayload{
        {4.0f, 0.25f, -2.0f}, {0.0f, 0.0f, 1.0f}, 2.0f, 0.0f, .25f, 0};

    const auto medium = Persistent(program, std::span(&owner, 1));
    Require(!medium.empty(), "cooked trap armed medium-quality mote was not authored");

    owner.quality = hs::VfxQuality::Low;
    Require(Persistent(program, std::span(&owner, 1)).empty(),
            "cooked trap armed low-quality mote was not filtered");
}

void CookedPickupIdleMotesConvert()
{
    const auto path = test_generation::ProductPath("vfx/vfx_program.hsbin");
    Require(std::filesystem::exists(path), "cooked VFX program missing for pickup idle motes");
    std::ifstream stream(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(stream),
                                  std::istreambuf_iterator<char>()};
    hs::VfxProgramData program;
    std::string error;
    Require(hs::LoadVfxProgram(std::as_bytes(std::span(bytes)), program, error),
            "cooked VFX program failed to load for pickup idle motes");

    for (const auto effect_id : kPickupIdleEffects)
    {
        const auto identity = std::find_if(program.effect_lookup.begin(),
            program.effect_lookup.end(), [&](const auto &entry) {
                return entry.effect_id == Hash64(effect_id);
            });
        if (identity == program.effect_lookup.end() || identity->handle == 0 ||
            identity->handle > program.effects.size())
            throw std::runtime_error("cooked pickup idle effect missing: " +
                                     std::string(effect_id));
        auto owner = PersistentMoteOwner(20);
        owner.effect_handle = identity->handle;
        const auto medium = PersistentMotes(program, owner);
        if (medium.size() != 2)
            throw std::runtime_error("cooked pickup idle ambient motes did not convert: " +
                                     std::string(effect_id));
        owner.quality = hs::VfxQuality::Low;
        Require(PersistentMotes(program, owner).empty(),
                "cooked pickup idle low-quality mote was not filtered");
    }
}

void AuthoredMask()
{
    auto p=Program();auto in=Input();const std::string path="Content/Textures/VFX/vfx_authored_mask_array.dds";
    for(char c:path)p.strings.push_back(static_cast<std::byte>(c));p.texture_resources.push_back({4,{0,static_cast<std::uint32_t>(path.size())}});
    hs::VfxTextureBindingRecord b;b.role=Hash("authored_mask_array");b.catalog_slot=4;b.selection=Hash("stable_seed_mod_group_size");b.strength=.38f;b.slices={0,3};
    p.texture_bindings.push_back(b);p.slice_indices={3,4,5};p.outputs[0].textures={0,1};auto decoded=Decode(p,in);
    const auto image=Prepared(p);
    Require(image.mote_source_recipes.size()==1&&image.mote_source_recipes[0].common_params_valid&&
        image.mote_source_recipes[0].count==10&&image.mote_output_recipes.size()==1&&
        image.mote_output_recipes[0].mask_present_mask==7&&image.mote_output_recipes[0].mask_quality_mask==7,
        "Mote immutable source/mask preparation lost an authored row");
    Require(decoded.size()==10&&decoded[0].mask_slice==p.slice_indices[in.stable_seed%3]&&Near(decoded[0].mask_strength,.38f),"mask binding missing");
    Require(decoded[0].mask_slice==decoded[9].mask_slice,"authored event mask selection changed per particle");
    p.slice_indices[0]=12;Require(Decode(p,in).empty(),"out of range mask accepted");
}

hs::VfxProgramData SparkProgram()
{
    hs::VfxProgramData program;
    hs::VfxEffectRecord effect;
    effect.handle = 1;
    effect.input_mode = 1;
    effect.timing_kind = 0;
    effect.seconds = .45f;
    effect.payload_kind = Hash("ProjectilePayload");
    effect.sources = {0, 1};
    program.effects.push_back(effect);
    program.effect_lookup.push_back({Hash64("particle.basic_attack"), 1});
    hs::VfxSourceRecord source;
    source.effect = 1;
    source.stable_id = Hash("micro_sparks");
    source.type = hs::VfxSourceType::CurlMotes;
    source.knot_count = 2;
    source.knots = {.1f, .9f, 0, 0};
    source.parameters = {0, 3};
    source.outputs = {0, 1};
    program.sources.push_back(source);
    program.parameters = {
        {Hash("count"), hs::VfxParameterType::Int, 3},
        {Hash("cone_deg"), hs::VfxParameterType::Float,
         std::bit_cast<std::uint32_t>(18.0f)},
        {Hash("drag"), hs::VfxParameterType::Float,
         std::bit_cast<std::uint32_t>(.8f)}};
    hs::VfxOutputRecord output;
    output.source = 0;
    output.profile = hs::VfxOutputProfile::SpriteAdd;
    output.shape = Hash("velocity_billboard");
    output.shape_domain = Hash("sprite");
    output.shape_scale_rule = Hash("component_size");
    output.shape_component_kind = Hash("particle");
    output.coverage_type = Hash("analytic");
    output.coverage_ref = Hash("velocity_streak");
    output.motion = hs::VfxMotionKind::SparseRearwardSparks;
    output.min_quality = 1;
    output.rgba = {.2f, .4f, .6f, .35f};
    output.hdr = 1.6f;
    output.gradient_row = 12;
    output.textures = {0, 3};
    program.outputs.push_back(output);
    const auto texture = [&](std::uint32_t slot, std::string_view path,
                             std::string_view role = "profile.global") {
        program.texture_resources.push_back({slot,
            {static_cast<std::uint32_t>(program.strings.size()),
             static_cast<std::uint32_t>(path.size())}});
        for (const unsigned char character : path)
            program.strings.push_back(static_cast<std::byte>(character));
        hs::VfxTextureBindingRecord binding;
        binding.role = Hash(role);
        binding.catalog_slot = slot;
        program.texture_bindings.push_back(binding);
    };
    texture(1, "Content/Textures/VFX/vfx_gradient_lut.dds");
    texture(2, "Content/Textures/VFX/vfx_curve_lut.dds");
    texture(3, "Content/Textures/VFX/vfx_authored_mask_array.dds",
            "profile.optional_detail");
    return program;
}

hs::VfxPersistentInput SparkOwner(std::uint32_t elapsed_tick)
{
    hs::VfxPersistentInput owner;
    owner.stable_id = 0x12345678u;
    owner.effect_handle = 1;
    owner.quality = hs::VfxQuality::Medium;
    owner.stable_seed = 0xabcdefu;
    owner.elapsed_seconds = static_cast<float>(elapsed_tick) / 60.0f;
    owner.elapsed_ticks = elapsed_tick;
    owner.current_transform[12] = 4.0f + static_cast<float>(elapsed_tick);
    owner.current_transform[13] = .35f;
    owner.current_transform[14] = -2.0f;
    owner.payload = hs::VfxProjectilePayload{{12, 0, 0}, {.12f, .12f, .12f},
                                             .12f, 0, 0};
    return owner;
}

std::uint32_t SparkModelMix(std::uint32_t value)
{
    value ^= value >> 16; value *= 0x7feb352du; value ^= value >> 15;
    value *= 0x846ca68bu; return value ^ (value >> 16);
}

hs::VfxProgramData MultiSourceSparkProgram()
{
    auto program = SparkProgram();
    program.effects[0].sources = {0, 2};
    auto second_source = program.sources[0];
    second_source.outputs = {1, 1};
    program.sources.push_back(second_source);
    auto second_output = program.outputs[0];
    second_output.source = 1;
    program.outputs.push_back(second_output);
    return program;
}

class LinearSparkReference
{
  public:
    void Append(std::span<const hs::VfxPersistentInput> owners, hs::Tick current_tick,
                std::vector<std::uint32_t> &seeds)
    {
        struct Change { std::size_t index; Cursor cursor; bool is_new; };
        std::vector<Change> changes;
        constexpr auto effect_id = Hash64("particle.basic_attack");
        constexpr auto source_id = Hash("micro_sparks");
        for (const auto &owner : owners)
        {
            const auto *projectile = std::get_if<hs::VfxProjectilePayload>(&owner.payload);
            if (!projectile) continue;
            const auto age = static_cast<std::int64_t>(owner.elapsed_ticks);
            for (std::uint32_t source = 0; source < 2; ++source)
            {
                auto change = std::find_if(changes.begin(), changes.end(), [&](const Change &item) {
                    return item.cursor.owner_id == owner.stable_id && item.cursor.effect_handle == owner.effect_handle &&
                        item.cursor.source_index == source;
                });
                if (change == changes.end())
                {
                    const auto cursor = std::find_if(cursors_.begin(), cursors_.end(), [&](const Cursor &item) {
                        return current_tick < item.expires_tick && item.owner_id == owner.stable_id &&
                            item.effect_handle == owner.effect_handle && item.source_index == source;
                    });
                    if (cursor != cursors_.end())
                        changes.push_back({static_cast<std::size_t>(cursor - cursors_.begin()), *cursor, false});
                    else
                        changes.push_back({std::numeric_limits<std::size_t>::max(),
                            {owner.stable_id, owner.effect_handle, source, 0,
                             current_tick - static_cast<hs::Tick>(age) + 27}, true});
                    change = std::prev(changes.end());
                }
                auto &cursor = change->cursor;
                for (std::uint32_t ordinal = cursor.next_ordinal; ordinal < 3;)
                {
                    const auto birth_tick = static_cast<std::int64_t>(
                        std::ceil((.1 * .45 + ((.9 - .1) * .45 / 2.0) * ordinal) * 60.0 - .000001));
                    if (age < birth_tick) break;
                    ++cursor.next_ordinal;
                    if (age == birth_tick)
                    {
                        const auto seed = SparkModelMix(owner.stable_seed ^
                            SparkModelMix(static_cast<std::uint32_t>(effect_id)) ^
                            SparkModelMix(static_cast<std::uint32_t>(effect_id >> 32)) ^
                            SparkModelMix(source_id) ^ SparkModelMix(ordinal + 1));
                        seeds.push_back(seed);
                    }
                    ++ordinal;
                }
            }
        }
        for (const auto &change : changes)
            if (!change.is_new) cursors_[change.index] = change.cursor;
        std::erase_if(cursors_, [current_tick](const Cursor &cursor) {
            return current_tick >= cursor.expires_tick;
        });
        for (const auto &change : changes)
            if (change.is_new) cursors_.push_back(change.cursor);
    }

  private:
    struct Cursor
    {
        std::uint64_t owner_id{};
        hs::VfxEffectHandle effect_handle{};
        std::uint32_t source_index{}, next_ordinal{};
        hs::Tick expires_tick{};
    };
    std::vector<Cursor> cursors_;
};

void PersistentSparkIndexMatchesOriginalLinearModel()
{
    const auto program = MultiSourceSparkProgram();
    const auto image = Prepared(program);
    hs::runtime_detail::VfxPersistentSparkState indexed;
    LinearSparkReference linear;
    auto a = SparkOwner(3); a.stable_id = 1001; a.stable_seed = 0x102030u;
    auto b = SparkOwner(3); b.stable_id = 2002; b.stable_seed = 0x405060u;
    auto c = SparkOwner(3); c.stable_id = 3003; c.stable_seed = 0x708090u;
    const auto compare = [&](std::span<const hs::VfxPersistentInput> owners, hs::Tick tick) {
        std::vector<hs::VfxFlashSpawnInput> output;
        std::vector<std::uint32_t> expected;
        indexed.Append(image.View(), owners, tick, 1.0f, output);
        linear.Append(owners, tick, expected);
        Require(output.size() == expected.size(), "indexed spark output count differed from linear cursor model");
        for (std::size_t i = 0; i < output.size(); ++i)
            Require(output[i].stable_seed == expected[i] && output[i].event_tick == tick,
                "indexed spark order or ordinal seed differed from linear cursor model");
        return output.size();
    };
    const std::array duplicate_inputs{a, b, a, c};
    Require(compare(duplicate_inputs, 100) == 6,
        "duplicate input or index growth changed multi-source spark births");
    a = SparkOwner(4); a.stable_id = 1001; a.stable_seed = 0x102030u;
    b = SparkOwner(4); b.stable_id = 2002; b.stable_seed = 0x405060u;
    const std::array skipped_tick_inputs{b, a, b};
    Require(compare(skipped_tick_inputs, 101) == 0,
        "skipped birth tick was replayed by the multi-source cursor index");
    a = SparkOwner(14); a.stable_id = 1001; a.stable_seed = 0x102030u;
    b = SparkOwner(14); b.stable_id = 2002; b.stable_seed = 0x405060u;
    const std::array second_birth_inputs{a, b};
    Require(compare(second_birth_inputs, 111) == 4,
        "multi-source cursor failed to catch the next authored birth");
    a = SparkOwner(25); a.stable_id = 1001; a.stable_seed = 0x102030u;
    b = SparkOwner(25); b.stable_id = 2002; b.stable_seed = 0x405060u;
    const std::array reordered_inputs{b, a};
    Require(compare(reordered_inputs, 122) == 4,
        "changed owner input order changed multi-source birth count");
    Require(compare({}, 124) == 0,
        "expired cursors emitted during an empty owner frame");
    a = SparkOwner(3); a.stable_id = 1001; a.stable_seed = 0x102030u;
    Require(compare(std::span(&a, 1), 124) == 2,
        "expired cursor identity was not reusable for a new owner episode");

    hs::runtime_detail::VfxPersistentSparkState retry_state;
    auto rejected = SparkOwner(3);
    rejected.stable_id = 3003;
    std::get<hs::VfxProjectilePayload>(rejected.payload).velocity = {};
    hs::VfxFlashSpawnInput prefix; prefix.stable_seed = 0x778899u;
    std::vector<hs::VfxFlashSpawnInput> retry_output{prefix};
    retry_state.Append(image.View(), std::span(&rejected, 1), 140, 1.0f, retry_output);
    Require(retry_output.size() == 1 && retry_output.front().stable_seed == prefix.stable_seed,
        "rejected spark input changed caller output before a valid retry");
    auto retry_owner = SparkOwner(3);
    retry_owner.stable_id = 3003;
    retry_state.Append(image.View(), std::span(&retry_owner, 1), 140, 1.0f, retry_output);
    Require(retry_output.size() == 3 && retry_output[0].stable_seed == prefix.stable_seed,
        "rejected spark input consumed its ordinal before valid retry");
}

void PersistentSparksUseOnlyLiveBirthPositions()
{
    const auto program = SparkProgram();
    hs::runtime_detail::VfxPersistentSparkState state;
    auto owner = SparkOwner(3);
    const auto emit = [&](hs::Tick tick) {
        return EmitSparks(state, program, std::span(&owner, 1), tick);
    };
    const auto first = emit(103);
    Require(state.RetainedCapacityBytes() > 0,
            "spark state omitted retained cursor vector capacity");
    Require(first.size() == 1 && first[0].shape == hs::VfxImpactShape::Spark &&
            Near(first[0].position.x, 7) && Near(first[0].position.y, .35f) &&
            Near(first[0].position.z, -2) && Near(first[0].size, .12f) &&
            Near(first[0].drag, .8f) && first[0].event_tick == 103,
            "first spark did not use the live projectile birth record");
    hs::runtime_detail::VfxPersistentSparkState append_state;
    auto image=Prepared(program);
    hs::VfxFlashSpawnInput prefix; prefix.stable_seed=888;
    std::vector<hs::VfxFlashSpawnInput> appended{prefix};
    append_state.Append(image.View(),std::span(&owner,1),103,1.0f,appended);
    Require(appended.size()==2&&appended.front().stable_seed==prefix.stable_seed&&
        appended.back().stable_seed==first[0].stable_seed,
        "persistent spark append changed output order or consumed birth state");
    const auto warm_capacity = append_state.RetainedCapacityBytes();
    append_state.Append(image.View(),std::span(&owner,1),103,1.0f,appended);
    Require(appended.size()==2&&appended.front().stable_seed==prefix.stable_seed&&
        appended.back().stable_seed==first[0].stable_seed&&
        append_state.RetainedCapacityBytes()==warm_capacity,
        "warm spark append changed output or failed to reuse retained scratch capacity");
    hs::runtime_detail::VfxPersistentSparkState swapped_state;
    swapped_state.Swap(append_state);
    Require(append_state.RetainedCapacityBytes()==0&&
        swapped_state.RetainedCapacityBytes()==warm_capacity,
        "spark swap did not transfer retained cursor and scratch capacity");
    Require(Near(first[0].lifetime, .18f) && first[0].gradient_row == 12 &&
            Near(first[0].hdr, 1.6f) && Near(first[0].color.x, .32f) &&
            Near(first[0].color.w, .35f) &&
            std::abs(std::hypot(first[0].initial_velocity.x,
                                first[0].initial_velocity.z) - 12.0f) < 1.0f,
            "authored material, hitbox scale, or rearward speed lost");
    const float rearward_dot = -first[0].direction.x;
    Require(rearward_dot >= std::cos(18.0f * .01745329252f) - .00001f,
            "spark escaped authored rearward half-cone");
    Require(emit(103).empty(), "same projectile tick duplicated spark ordinal");
    owner = SparkOwner(14);
    const auto second = emit(114);
    Require(second.size() == 1 && Near(second[0].position.x, 18) &&
            second[0].stable_seed != first[0].stable_seed &&
            Near(first[0].position.x, 7),
            "second spark synthesized an old projectile position or changed first birth");
    owner = SparkOwner(25);
    const auto third = emit(125);
    Require(third.size() == 1 && Near(third[0].position.x, 29) &&
            third[0].stable_seed != second[0].stable_seed,
            "third authored spark ordinal missing");
    Require(emit(125).empty(), "last spark reemitted on repeated frame");
    owner = SparkOwner(27);
    Require(emit(127).empty(), "expired projectile effect emitted spark");
    owner = SparkOwner(3);
    Require(emit(133).size() == 1,
            "expired owner cursor was not released for a reused projectile identity");
}

void PersistentSparksSkipMissedTicksAndReset()
{
    const auto program = SparkProgram();
    hs::runtime_detail::VfxPersistentSparkState reference;
    auto owner = SparkOwner(14);
    const auto expected = EmitSparks(reference, program, std::span(&owner, 1), 114);
    Require(expected.size() == 1, "reference second ordinal missing");
    hs::runtime_detail::VfxPersistentSparkState state;
    owner = SparkOwner(4);
    Require(EmitSparks(state, program, std::span(&owner, 1), 104).empty(),
            "late owner snapshot fabricated missed first spark");
    owner = SparkOwner(14);
    const auto actual = EmitSparks(state, program, std::span(&owner, 1), 114);
    Require(actual.size() == 1 && actual[0].stable_seed == expected[0].stable_seed &&
            Near(actual[0].direction.x, expected[0].direction.x),
            "ordinal identity depended on skipped frames");
    Require(EmitSparks(state, program, {}, 114).empty(), "absent owner emitted a spark");
    Require(EmitSparks(state, program, std::span(&owner, 1), 114).empty(),
            "transient missing owner replayed the same birth");
    state.Clear();
    Require(state.RetainedCapacityBytes() > 0,
            "spark clear hid retained cursor backing capacity");
    Require(EmitSparks(state, program, std::span(&owner, 1), 114).size() == 1,
            "session/catalog reset failed to clear ordinal state");
    owner = SparkOwner(25);
    owner.quality = hs::VfxQuality::Low;
    Require(EmitSparks(state, program, std::span(&owner, 1), 125).empty(),
            "low-quality spark was not filtered");
    owner.quality = hs::VfxQuality::Medium;
    Require(EmitSparks(state, program, std::span(&owner, 1), 125).empty(),
            "quality change replayed a skipped ordinal");
}

void PersistentSparksRejectWrongContracts()
{
    auto program = SparkProgram();
    auto owner = SparkOwner(3);
    const auto run = [&] {
        hs::runtime_detail::VfxPersistentSparkState state;
        return EmitSparks(state, program, std::span(&owner, 1), 103);
    };
    program.effect_lookup[0].effect_id = Hash64("particle.enemy.ranged.release");
    Require(run().empty(), "unrelated projectile effect accepted");
    program = SparkProgram();
    program.outputs[0].profile = hs::VfxOutputProfile::SpriteSdfAdd;
    Require(run().empty(), "wrong spark profile accepted");
    program.outputs[0].profile = hs::VfxOutputProfile::SpriteAdd;
    program.outputs[0].shape = Hash("small_motes");
    Require(run().empty(), "wrong spark shape accepted");
    program.outputs[0].shape = Hash("velocity_billboard");
    program.outputs[0].motion = hs::VfxMotionKind::VeryShortLocalDrift;
    Require(run().empty(), "wrong spark motion accepted");
    program = SparkProgram();
    program.parameters[0].bits = 0;
    Require(run().empty(), "zero spark count accepted");
    program = SparkProgram();
    program.parameters[1].bits = std::bit_cast<std::uint32_t>(
        std::numeric_limits<float>::quiet_NaN());
    Require(run().empty(), "nonfinite spark cone accepted");
    program = SparkProgram();
    program.texture_bindings[0].role = Hash("profile.authored");
    Require(run().empty(), "wrong sprite texture binding accepted");
    program = SparkProgram();
    std::get<hs::VfxProjectilePayload>(owner.payload).velocity = {};
    Require(run().empty(), "spark accepted stationary projectile");
}

void PersistentSparksFollowCookedTuning()
{
    auto program = SparkProgram();
    program.effects[0].seconds = .6f;
    program.sources[0].knots = {.2f, .8f, 0, 0};
    program.parameters[0].bits = 4;
    program.parameters[1].bits = std::bit_cast<std::uint32_t>(25.0f);
    program.parameters[2].bits = std::bit_cast<std::uint32_t>(1.2f);
    program.outputs[0].min_quality = static_cast<std::uint32_t>(hs::VfxQuality::High);
    program.outputs[0].hdr = 2.0f;
    program.outputs[0].rgba[3] = .5f;
    hs::runtime_detail::VfxPersistentSparkState state;
    std::size_t births{};
    for (std::uint32_t age_tick = 0; age_tick < 36; ++age_tick)
    {
        auto owner = SparkOwner(age_tick);
        owner.quality = hs::VfxQuality::High;
        const auto born = EmitSparks(state, program, std::span(&owner, 1), 100 + age_tick);
        if (!born.empty())
        {
            Require(born.size() == 1 && Near(born[0].hdr, 2) &&
                    Near(born[0].color.w, .5f) && Near(born[0].drag, 1.2f),
                    "retuned cooked spark values were not consumed");
            ++births;
        }
    }
    Require(births == 4, "retuned count/source interval did not emit four ordinals");
}

void CookedPersistentSparksConvertWhenAvailable()
{
    const auto path = test_generation::ProductPath("vfx/vfx_program.hsbin");
    std::ifstream stream(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(stream),
                                  std::istreambuf_iterator<char>()};
    hs::VfxProgramData program;
    std::string error;
    Require(hs::LoadVfxProgram(std::as_bytes(std::span(bytes)), program, error),
            "cooked VFX program failed to load for projectile sparks");
    std::size_t authored{}, converted{};
    for (const auto &lookup : program.effect_lookup)
    {
        bool spark_effect = false;
        for (const auto id : {Hash64("particle.basic_attack"),
                              Hash64("particle.skill.charged_shot"),
                              Hash64("particle.skill.explosive_arrow"),
                              Hash64("particle.skill.piercing_shot"),
                              Hash64("particle.skill.ricochet_arrow"),
                              Hash64("particle.upgrade.arrow_rain.incoming_arrow")})
            spark_effect |= lookup.effect_id == id;
        if (!spark_effect) continue;
        const auto &effect = program.effects[lookup.handle - 1];
        for (std::size_t si = effect.sources.first;
             si < static_cast<std::size_t>(effect.sources.first) + effect.sources.count; ++si)
        {
            const auto &source = program.sources[si];
            for (std::size_t oi = source.outputs.first;
                 oi < static_cast<std::size_t>(source.outputs.first) + source.outputs.count; ++oi)
                authored += program.outputs[oi].profile == hs::VfxOutputProfile::SpriteAdd &&
                    program.outputs[oi].shape == Hash("velocity_billboard");
        }
        hs::runtime_detail::VfxPersistentSparkState state;
        for (std::uint32_t age_tick = 0; age_tick < 60; ++age_tick)
        {
            auto owner = SparkOwner(age_tick);
            owner.effect_handle = lookup.handle;
            converted += EmitSparks(state, program, std::span(&owner, 1), 100 + age_tick).size();
        }
    }
    if (authored != 6 || converted != authored * 3)
        throw std::runtime_error("cooked projectile sparks: authored=" +
            std::to_string(authored) + " converted=" + std::to_string(converted));
}
}
int RunVfxMoteTests()
{
    try {PreparedRoutesCloseAuthoredWindows();PreparedResidualAndTickRoutesRequireTwoKnots();BurstAndScale();AppendAndConsumedSequenceSemantics();CatalogOrderAndMotions();Malformed();AuthoredMask();OwnedRateAndClock();SafeGapSectorAcceptsNonAmbientSourceId();OwnedMalformedAndQuality();PersistentMotesRecomputeDeterministicBirthsAndOwnerBoundGeometry();PersistentMotesRejectMalformedOwners();ChargedOverchargeMotesUseAuthoredMediumRecipe();PickupIdleMotesFollowPresentEntity();PickupIdleMotesRejectMalformedRecipes();EventTrapMotesUsePointClock();PersistentTrapMotesUseCircleClockGeometry();PersistentTrapMotesRejectMalformedRecipesAndQuality();CookedPersistentTrapMotesConvert();CookedPickupIdleMotesConvert();PersistentSparkIndexMatchesOriginalLinearModel();PersistentSparksUseOnlyLiveBirthPositions();PersistentSparksSkipMissedTicksAndReset();PersistentSparksRejectWrongContracts();PersistentSparksFollowCookedTuning();CookedPersistentSparksConvertWhenAvailable();std::cout<<"VFX typed mote commands passed\n";return 0;}
    catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
