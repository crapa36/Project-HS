#include "../Source/Runtime/Private/vfx_typed_ribbon_commands.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
std::uint32_t Hash(std::string_view value)
{
    std::uint32_t hash = 2166136261u;
    for (const auto character : value)
    {
        hash ^= static_cast<unsigned char>(character);
        hash *= 16777619u;
    }
    return hash;
}
std::uint64_t Hash64(std::string_view value)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto character : value)
    {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ull;
    }
    return hash;
}
void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
hs::VfxParameterRecord FloatParam(std::string_view key, float value)
{
    return {Hash(key), hs::VfxParameterType::Float, std::bit_cast<std::uint32_t>(value)};
}

void TestBossDashWakeRibbon()
{
    hs::VfxProgramData cooked;
    hs::VfxEffectRecord effect{};
    effect.handle = 1;
    effect.input_mode = 1;
    effect.timing_kind = 1;
    effect.payload_kind = Hash("LineAreaPayload");
    effect.importance = 4;
    effect.sources = {0, 1};
    cooked.effects.push_back(effect);
    cooked.effect_lookup.push_back({Hash64("persistent.boss.dash_wake"), 1});
    hs::VfxSourceRecord source{};
    source.effect = 1;
    source.stable_id = Hash("wake_rails");
    source.type = hs::VfxSourceType::HistoryRibbon;
    source.knot_count = 2;
    source.knots = {0.0f, 1.0f};
    source.parameters = {0, 2};
    source.outputs = {0, 1};
    cooked.sources.push_back(source);
    cooked.parameters = {FloatParam("history_seconds", .24f),
                         {Hash("ground_lock"), hs::VfxParameterType::Int, 1},
                         FloatParam("width_head", .16f),
                         FloatParam("width_tail", .03f)};
    hs::VfxOutputRecord output{};
    output.source = 0;
    output.profile = hs::VfxOutputProfile::RibbonAdd;
    output.shape = Hash("paired_ground_ribbons");
    output.shape_domain = Hash("ribbon");
    output.shape_scale_rule = Hash("component_width_and_history");
    output.shape_component_kind = Hash("ribbon");
    output.motion = hs::VfxMotionKind::HistoryRibbonTearsBackward;
    output.coverage_type = Hash("analytic");
    output.coverage_ref = Hash("ribbon_energy_core");
    output.rgba = {.7f, .3f, .1f, .62f};
    output.hdr = 2.8f;
    output.gradient_row = 7;
    output.parameters = {2, 2};
    output.textures = {0, 1};
    cooked.outputs.push_back(output);
    hs::VfxTextureBindingRecord detail{};
    detail.role = Hash("ribbon_detail_array");
    detail.catalog_slot = 4;
    detail.selection = Hash("stable_seed_mod_group_size");
    detail.strength = .3f;
    detail.slices = {0, 2};
    cooked.texture_bindings.push_back(detail);
    cooked.slice_indices = {1, 3};
    const std::string path = "Content/Textures/VFX/vfx_ribbon_detail_array.dds";
    cooked.texture_resources.push_back({4, {0, static_cast<std::uint32_t>(path.size())}});
    cooked.strings.assign(reinterpret_cast<const std::byte *>(path.data()),
                          reinterpret_cast<const std::byte *>(path.data() + path.size()));

    hs::VfxPersistentInput input{};
    input.stable_id = 74;
    input.effect_handle = 1;
    input.stable_seed = 5;
    input.normalized_age = .25f;
    input.payload = hs::VfxLinePayload{{2, 4, 3}, {8, 5, 3}, {1, 0, 0}, 1.8f, .25f, 0};
    const auto run = [&](const hs::VfxProgramData &program) {
        return hs::runtime_detail::BuildVfxTypedRibbonInputs(program, std::array{input});
    };
    const auto ribbons = run(cooked);
    Check(ribbons.size() == 1 && ribbons[0].outputs.size() == 1 &&
              ribbons[0].owner_id == 74 && ribbons[0].source_id == Hash("wake_rails") &&
              ribbons[0].history_seconds == .24f && ribbons[0].importance == 4 &&
              ribbons[0].previous_position.x == 2 && ribbons[0].position.x == 8 &&
              ribbons[0].previous_position.y == .025f && ribbons[0].position.y == .025f &&
              ribbons[0].control.x == 1.8f && !ribbons[0].analytic,
          "dash wake lost the authoritative line, ground lock, or history");
    const auto &rail = ribbons[0].outputs[0];
    Check(rail.profile == hs::VfxOutputProfile::RibbonAdd &&
              rail.motion == hs::VfxMotionKind::HistoryRibbonTearsBackward &&
              rail.width_head == .16f && rail.width_tail == .03f &&
              rail.hdr == 2.8f && rail.color.w == .62f &&
              rail.gradient_row == 7 && rail.detail_slice == 3 &&
              rail.detail_strength == .3f,
          "dash wake lost authored rail material or stable detail binding");
    auto invalid = cooked;
    invalid.effect_lookup[0].effect_id = Hash64("persistent.boss.other");
    Check(run(invalid).empty(), "unrelated line recipe decoded as dash wake");
    invalid = cooked;
    invalid.parameters[0] = FloatParam("history_seconds", .25f);
    Check(run(invalid).empty(), "dash wake accepted a different history window");
    invalid = cooked;
    invalid.parameters[1].bits = 0;
    Check(run(invalid).empty(), "dash wake accepted an unlocked ground source");
    invalid = cooked;
    invalid.outputs[0].shape = Hash("dual_camera_ribbon");
    Check(run(invalid).empty(), "dash wake accepted a different shape");
    invalid = cooked;
    invalid.outputs[0].motion = hs::VfxMotionKind::HistoryRibbonTaperedFlow;
    Check(run(invalid).empty(), "dash wake accepted a different motion");
    invalid = cooked;
    invalid.texture_bindings[0].selection = Hash("first_slice");
    Check(run(invalid).empty(), "dash wake accepted a different detail selection");
    invalid = cooked;
    invalid.slice_indices[1] = 8;
    Check(run(invalid).empty(), "dash wake accepted an invalid detail slice");
    auto malformed = input;
    std::get<hs::VfxLinePayload>(malformed.payload).end = {2, 5, 3};
    Check(hs::runtime_detail::BuildVfxTypedRibbonInputs(cooked, std::array{malformed}).empty(),
          "dash wake accepted a zero-length ground segment");
    malformed = input;
    std::get<hs::VfxLinePayload>(malformed.payload).lifetime01 = 1.0f;
    Check(hs::runtime_detail::BuildVfxTypedRibbonInputs(cooked, std::array{malformed}).empty(),
          "expired dash wake retained a ribbon");
}

void TestCookedBossDashWakeRibbon()
{
    std::ifstream file("Cooked/vfx_program.hsbin", std::ios::binary);
    Check(file.good(), "production VFX program missing");
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file),
                                  std::istreambuf_iterator<char>()};
    hs::VfxProgramData cooked;
    std::string error;
    Check(hs::LoadVfxProgram(std::as_bytes(std::span(bytes)), cooked, error),
          "production VFX program could not be decoded");
    const auto identity = std::find_if(cooked.effect_lookup.begin(),
                                       cooked.effect_lookup.end(),
        [](const auto &entry) {
            return entry.effect_id == Hash64("persistent.boss.dash_wake");
        });
    Check(identity != cooked.effect_lookup.end(), "production dash wake lookup missing");
    hs::VfxPersistentInput input{};
    input.stable_id = 90;
    input.effect_handle = identity->handle;
    input.stable_seed = 1;
    input.normalized_age = .25f;
    input.payload = hs::VfxLinePayload{{2, 0, 3}, {8, 0, 3}, {1, 0, 0},
                                       1.8f, .25f, 0};
    const auto ribbons = hs::runtime_detail::BuildVfxTypedRibbonInputs(
        cooked, std::array{input});
    Check(ribbons.size() == 1 && ribbons[0].outputs.size() == 1 &&
              ribbons[0].source_id == Hash("wake_rails") &&
              ribbons[0].history_seconds == .24f &&
              ribbons[0].outputs[0].motion ==
                  hs::VfxMotionKind::HistoryRibbonTearsBackward &&
              ribbons[0].outputs[0].detail_slice < 8,
          "production dash wake did not decode its paired ground rails");
}

void TestAnalyticLinks()
{
    const auto hash64=[](std::string_view value){std::uint64_t h=14695981039346656037ull;for(unsigned char c:value){h^=c;h*=1099511628211ull;}return h;};
    hs::VfxProgramData p;hs::VfxEffectRecord e;e.handle=1;e.input_mode=1;e.timing_kind=1;
    e.payload_kind=Hash("SourceTargetPayload");e.sources={0,2};p.effects.push_back(e);
    p.effect_lookup.push_back({hash64("particle.line.burn_transfer"),1});
    hs::VfxSourceRecord src;src.effect=1;src.type=hs::VfxSourceType::HistoryRibbon;src.knot_count=2;src.knots={0,1,0,0};
    src.stable_id=Hash("link_core");src.outputs={0,1};p.sources.push_back(src);src.stable_id=Hash("link_outer");src.outputs={1,1};p.sources.push_back(src);
    p.parameters={{Hash("segments"),hs::VfxParameterType::Int,16},FloatParam("taper",.55f),
        {Hash("travel_pulse"),hs::VfxParameterType::Bool,1},FloatParam("width_multiplier",2.8f),FloatParam("noise_warp",.08f)};
    hs::VfxOutputRecord o;o.source=0;o.profile=hs::VfxOutputProfile::RibbonAdd;o.motion=hs::VfxMotionKind::CurvedLinkTravelingHead;
    o.shape=Hash("bezier_ribbon");o.parameters={0,3};o.rgba={1,.5f,.2f,.75f};o.hdr=3;o.gradient_row=7;p.outputs.push_back(o);
    o.source=1;o.profile=hs::VfxOutputProfile::RibbonOit;o.motion=hs::VfxMotionKind::SoftWiderSheath;o.parameters={3,2};o.min_quality=1;p.outputs.push_back(o);
    hs::VfxPersistentInput input;input.stable_id=55;input.effect_handle=1;input.elapsed_seconds=.1f;input.normalized_age=.9f;
    hs::VfxLinkPayload link;link.source_position={1,2,3};link.target_position={5,2,3};link.width=.06f;input.payload=link;
    const auto run=[&]{return hs::runtime_detail::BuildVfxTypedRibbonInputs(p,std::span(&input,1));};
    auto r=run();Check(r.size()==1&&r[0].analytic&&r[0].outputs.size()==2&&r[0].segments==16,
        "link did not share one analytic curve between its outputs");
    p.effect_lookup[0].effect_id=hash64("particle.upgrade.ricochet.return");
    Check(run().size()==1&&run()[0].outputs.size()==2,
          "ricochet return link did not accept both authored ribbon outputs");
    p.effect_lookup[0].effect_id=hash64("particle.line.ricochet");
    Check(r[0].previous_position.x==1&&r[0].position.x==5&&std::abs(r[0].control.x-3)<.0001f&&
        std::abs(r[0].control.y-2.35f)<.0001f&&std::abs(r[0].start_alpha-.5f)<.0001f,
        "link moved authoritative endpoints or ignored owner fade");
    Check(std::abs(r[0].outputs[0].width_head-.06f)<.0001f&&std::abs(r[0].outputs[0].width_tail-.033f)<.0001f&&
        r[0].outputs[0].travel_pulse&&!r[0].outputs[1].travel_pulse&&std::abs(r[0].outputs[1].width_head-.168f)<.0001f&&
        std::abs(r[0].outputs[1].noise_warp-.08f)<.0001f&&r[0].outputs[0].gradient_row==7,
        "link world width, taper, pulse, sheath or gradient lost");
    input.quality=hs::VfxQuality::Low;Check(run()[0].outputs.size()==1,"link output quality ignored");input.quality=hs::VfxQuality::High;
    const std::string path="Content/Textures/VFX/vfx_ribbon_detail_array.dds";
    for(char c:path)p.strings.push_back(static_cast<std::byte>(c));p.texture_resources.push_back({4,{0,static_cast<std::uint32_t>(path.size())}});
    hs::VfxTextureBindingRecord tex;tex.role=Hash("ribbon_detail_array");tex.catalog_slot=4;tex.selection=Hash("stable_seed_mod_group_size");tex.slices={0,2};tex.strength=.72f;
    p.texture_bindings.push_back(tex);p.slice_indices={2,3};p.outputs[0].textures={0,1};input.stable_seed=1;
    Check(run()[0].outputs[0].detail_slice==3&&run()[0].outputs[0].detail_strength==.72f,"link authored detail selection lost");
    p.parameters[0].type=hs::VfxParameterType::Float;Check(run().empty(),"link accepted malformed segments");p.parameters[0].type=hs::VfxParameterType::Int;
    input.normalized_age=1;Check(run().empty(),"expired link survived owner age");input.normalized_age=.5f;
    std::get<hs::VfxLinkPayload>(input.payload).target_position=link.source_position;Check(run().empty(),"degenerate link accepted");input.payload=link;
    std::get<hs::VfxLinkPayload>(input.payload).width=-1;Check(run().empty(),"negative link width accepted");input.payload=link;
    p.effect_lookup[0].effect_id=hash64("particle.line.unrelated");Check(run().empty(),"unrelated link recipe accepted");
}

void TestEventRecoilRibbon()
{
    hs::VfxProgramData program;
    hs::VfxEffectRecord effect{};
    effect.handle = 1;
    effect.input_mode = 0;
    effect.payload_kind = Hash("PointEventPayload");
    effect.importance = 2;
    effect.seconds = 0.6f;
    effect.sources = {0, 1};
    program.effects.push_back(effect);
    program.parameters = {FloatParam("history_seconds", 0.08f),
                          FloatParam("arc_deg", 35.0f)};
    hs::VfxSourceRecord source{};
    source.effect = 1;
    source.stable_id = Hash("bow_ribbon");
    source.type = hs::VfxSourceType::HistoryRibbon;
    source.knot_count = 2;
    source.knots = {0.0f, 1.0f};
    source.parameters = {0, 1};
    source.outputs = {0, 1};
    program.sources.push_back(source);
    hs::VfxOutputRecord output{};
    output.source = 0;
    output.profile = hs::VfxOutputProfile::RibbonAdd;
    output.shape = Hash("short_arc_ribbon");
    output.motion = hs::VfxMotionKind::CurvedRecoilArc;
    output.rgba = {1.0f, 0.5f, 0.2f, 0.8f};
    output.hdr = 2.0f;
    output.gradient_row = 4;
    output.parameters = {1, 1};
    program.outputs.push_back(output);

    hs::VfxEventInput event;
    event.effect_handle = 1;
    event.event_tick = 0;
    event.sequence = 77;
    event.stable_seed = 9;
    event.world_transform[12] = 1.0f;
    event.world_transform[13] = 2.0f;
    event.world_transform[14] = 3.0f;
    event.payload = hs::VfxPointPayload{{1.0f, 2.0f, 3.0f}, {}, {0.0f, 0.0f, 1.0f}, 1.0f};
    const std::array events{event};
    const auto start = hs::runtime_detail::BuildVfxEventRibbonInputs(program, events, 0);
    Check(start.size() == 1 && start[0].owner_id == 77 &&
              start[0].position.x == 1.0f && start[0].position.y == 2.0f &&
              start[0].position.z == 3.0f,
          "recoil ribbon did not start at event origin");
    const auto middle = hs::runtime_detail::BuildVfxEventRibbonInputs(program, events, 30);
    Check(middle.size() == 1 && middle[0].position.y > 2.21f && middle[0].position.y < 2.23f &&
              middle[0].previous_position.y < middle[0].position.y &&
              middle[0].position.x == 1.0f && middle[0].position.z < 3.0f,
          "recoil ribbon arc progression was not derived from event time");
    const auto ended = hs::runtime_detail::BuildVfxEventRibbonInputs(program, events, 36);
    Check(ended.empty(), "recoil ribbon survived its authored source window");
    auto delayed = program;
    delayed.effects[0].seconds = 1.0f;
    delayed.sources[0].knots = {0.25f, 0.75f};
    Check(hs::runtime_detail::BuildVfxEventRibbonInputs(delayed, events, 14).empty(),
          "delayed recoil source started before its knot interval");
    const auto onset = hs::runtime_detail::BuildVfxEventRibbonInputs(delayed, events, 15);
    Check(onset.size() == 1 && onset[0].position.z == 3.0f && onset[0].previous_position.z == 3.0f,
          "delayed recoil did not clamp its initial history to the event origin");
    Check(hs::runtime_detail::BuildVfxEventRibbonInputs(delayed, events, 45).empty(),
          "delayed recoil did not end at its knot boundary");

    auto invalid = program;
    invalid.outputs[0].motion = hs::VfxMotionKind::VelocityAligned;
    Check(hs::runtime_detail::BuildVfxEventRibbonInputs(invalid, events, 0).empty(),
          "unsupported recoil motion was accepted");
}
}

void TestEventImpactAxialRibbon()
{
    hs::VfxProgramData p;
    hs::VfxEffectRecord e{};
    e.handle = 1;
    e.input_mode = 0;
    e.payload_kind = Hash("ProjectilePayload");
    e.importance = 4;
    e.seconds = .48f;
    e.sources = {0, 1};
    p.effects.push_back(e);
    p.effect_lookup.push_back({Hash64("particle.skill.charged_shot.impact"), 1});
    p.parameters = {FloatParam("history_seconds", .09f),
                    FloatParam("width_head", .07f),
                    FloatParam("width_tail", .015f)};
    hs::VfxSourceRecord s{};
    s.effect = 1;
    s.stable_id = Hash("axial_afterline");
    s.type = hs::VfxSourceType::HistoryRibbon;
    s.knot_count = 2;
    s.knots = {.04f, .75f, 0.0f, 0.0f};
    s.parameters = {0, 1};
    s.outputs = {0, 1};
    p.sources.push_back(s);
    hs::VfxOutputRecord o{};
    o.source = 0;
    o.profile = hs::VfxOutputProfile::RibbonAdd;
    o.shape = Hash("impact_axis_ribbon");
    o.motion = hs::VfxMotionKind::ShortLineIncomingVelocity;
    o.rgba = {.1f, .2f, .3f, .56f};
    o.hdr = 2.8f;
    o.gradient_row = 23;
    o.parameters = {1, 2};
    o.textures = {0, 1};
    p.outputs.push_back(o);
    hs::VfxTextureBindingRecord detail{};
    detail.role = Hash("ribbon_detail_array");
    detail.catalog_slot = 7;
    detail.selection = Hash("stable_seed_mod_group_size");
    detail.strength = .35f;
    detail.slices = {0, 2};
    p.texture_bindings.push_back(detail);
    p.slice_indices = {2, 3};
    const std::string detail_asset = "Content/Textures/VFX/vfx_ribbon_detail_array.dds";
    p.texture_resources.push_back({7, {0, static_cast<std::uint32_t>(detail_asset.size())}});
    p.strings.assign(reinterpret_cast<const std::byte *>(detail_asset.data()),
                     reinterpret_cast<const std::byte *>(detail_asset.data() + detail_asset.size()));

    hs::VfxEventInput ev{};
    ev.effect_handle = 1;
    ev.event_tick = 0;
    ev.sequence = 44;
    ev.stable_seed = 1;
    ev.world_transform[12] = 10.0f;
    ev.world_transform[13] = 2.0f;
    ev.world_transform[14] = -4.0f;
    hs::VfxProjectilePayload projectile;
    projectile.velocity = {3.0f, 4.0f, 12.0f};
    projectile.hitbox_radius = .5f;
    ev.payload = projectile;
    const std::array events{ev};
    const auto out = hs::runtime_detail::BuildVfxEventRibbonInputs(p, events, 6);
    Check(out.size() == 1 && out[0].owner_id == 44 && out[0].source_id == s.stable_id,
          "charged impact axial ribbon was not routed through the exact recipe");
    Check(std::abs(out[0].position.x - 10.0f) < .0001f &&
              std::abs(out[0].position.y - 2.0f) < .0001f &&
              std::abs(out[0].position.z + 4.0f) < .0001f,
          "charged impact ribbon did not use the contact world transform");
    Check(std::abs(out[0].previous_position.x - 9.73f) < .0001f &&
              std::abs(out[0].previous_position.y - 1.64f) < .0001f &&
              std::abs(out[0].previous_position.z + 5.08f) < .0001f,
          "charged impact ribbon length was not velocity times authored history");
    Check(std::abs(out[0].history_seconds - .09f) < .0001f &&
              std::abs(out[0].start_alpha - .237089f) < .001f,
          "charged impact ribbon history or source knots were not preserved");
    Check(out[0].outputs.size() == 1 &&
              out[0].outputs[0].profile == hs::VfxOutputProfile::RibbonAdd &&
              out[0].outputs[0].motion == hs::VfxMotionKind::ShortLineIncomingVelocity &&
              std::abs(out[0].outputs[0].width_head - .07f) < .0001f &&
              std::abs(out[0].outputs[0].width_tail - .015f) < .0001f &&
              out[0].outputs[0].gradient_row == 23 &&
              std::abs(out[0].outputs[0].hdr - 2.8f) < .0001f &&
              out[0].outputs[0].detail_slice == 3 &&
              std::abs(out[0].outputs[0].detail_strength - .35f) < .0001f,
          "charged impact authored material was not preserved");

    auto invalid = p;
    invalid.effect_lookup[0].effect_id = Hash64("particle.enemy.ranged.impact");
    Check(hs::runtime_detail::BuildVfxEventRibbonInputs(invalid, events, 6).empty(),
          "unrelated projectile impact identity accepted the axial ribbon");
    invalid = p;
    invalid.sources[0].stable_id = Hash("other_afterline");
    Check(hs::runtime_detail::BuildVfxEventRibbonInputs(invalid, events, 6).empty(),
          "unrelated history source accepted the charged axial ribbon");
    invalid = p;
    invalid.outputs[0].profile = hs::VfxOutputProfile::RibbonOit;
    Check(hs::runtime_detail::BuildVfxEventRibbonInputs(invalid, events, 6).empty(),
          "non-additive axial output accepted");
    invalid = p;
    invalid.outputs[0].motion = hs::VfxMotionKind::CurvedRecoilArc;
    Check(hs::runtime_detail::BuildVfxEventRibbonInputs(invalid, events, 6).empty(),
          "non-axial motion accepted for the charged impact");
    auto malformed = ev;
    auto malformed_projectile = projectile;
    malformed_projectile.velocity = {0.0f, 0.0f, 0.0f};
    malformed.payload = malformed_projectile;
    Check(hs::runtime_detail::BuildVfxEventRibbonInputs(p, std::array{malformed}, 6).empty(),
          "zero projectile velocity accepted");
    malformed_projectile.velocity = {std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f};
    malformed.payload = malformed_projectile;
    Check(hs::runtime_detail::BuildVfxEventRibbonInputs(p, std::array{malformed}, 6).empty(),
          "nonfinite projectile velocity accepted");
    malformed_projectile.velocity = projectile.velocity;
    malformed_projectile.hitbox_radius = 0.0f;
    malformed.payload = malformed_projectile;
    Check(hs::runtime_detail::BuildVfxEventRibbonInputs(p, std::array{malformed}, 6).empty(),
          "nonpositive projectile radius accepted");
}

void TestEventArrowRainPullRibbon()
{
    const auto effect_id = Hash64("particle.upgrade.arrow_rain.pull");
    hs::VfxProgramData program;
    hs::VfxEffectRecord effect{};
    effect.handle = 1;
    effect.input_mode = 0;
    effect.importance = 3;
    effect.seconds = 0.45f;
    effect.payload_kind = Hash("SourceTargetPayload");
    effect.sources = {0, 2};
    program.effects.push_back(effect);
    program.effect_lookup.push_back({effect_id, 1});
    program.parameters = {
        {Hash("segments"), hs::VfxParameterType::Int, 16},
        FloatParam("taper", 0.55f),
        {Hash("travel_pulse"), hs::VfxParameterType::Bool, 1},
        FloatParam("width_multiplier", 2.8f),
        FloatParam("noise_warp", 0.08f)};
    hs::VfxSourceRecord core{};
    core.effect = 1;
    core.stable_id = Hash("link_core");
    core.type = hs::VfxSourceType::HistoryRibbon;
    core.knot_count = 2;
    core.knots = {0.0f, 1.0f};
    core.outputs = {0, 1};
    program.sources.push_back(core);
    core.stable_id = Hash("link_outer");
    core.outputs = {1, 1};
    program.sources.push_back(core);
    hs::VfxOutputRecord output{};
    output.source = 0;
    output.profile = hs::VfxOutputProfile::RibbonAdd;
    output.shape = Hash("bezier_ribbon");
    output.motion = hs::VfxMotionKind::CurvedLinkTravelingHead;
    output.parameters = {0, 3};
    output.rgba = {1.0f, 0.5f, 0.2f, 0.75f};
    output.hdr = 3.0f;
    program.outputs.push_back(output);
    output.source = 1;
    output.profile = hs::VfxOutputProfile::RibbonOit;
    output.motion = hs::VfxMotionKind::SoftWiderSheath;
    output.parameters = {3, 2};
    output.rgba = {0.2f, 0.5f, 1.0f, 0.28f};
    output.hdr = 1.3f;
    program.outputs.push_back(output);

    hs::VfxEventInput event{};
    event.effect_handle = 1;
    event.event_tick = 100;
    event.sequence = 77;
    event.stable_seed = 0x1234;
    event.payload = hs::VfxLinkPayload{
        {4.0f, 1.05f, -2.0f}, {3.25f, 1.05f, -2.0f}, 712, 712, 0.0f, 0.12f, 0};
    const std::array events{event};
    const auto start = hs::runtime_detail::BuildVfxEventRibbonInputs(program, events, 100);
    Check(start.size() == 1 && start[0].owner_id == event.sequence &&
              start[0].source_id == Hash("link_core") && start[0].outputs.size() == 2 &&
              start[0].previous_position.x == 4.0f &&
              start[0].position.x == 3.25f && start[0].normalized_age == 0.0f,
          "ArrowRain pull event did not use its stable owner or real endpoints");
    Check(start[0].analytic && start[0].segments == 16 &&
              start[0].outputs[0].travel_pulse &&
              start[0].outputs[1].profile == hs::VfxOutputProfile::RibbonOit,
          "ArrowRain pull event did not preserve the exact link recipe");

    const auto active = hs::runtime_detail::BuildVfxEventRibbonInputs(program, events, 126);
    Check(active.size() == 1 && active[0].normalized_age > 0.95f &&
              active[0].stable_seed == event.stable_seed,
          "ArrowRain pull event age or seed was not preserved");
    Check(hs::runtime_detail::BuildVfxEventRibbonInputs(program, events, 127).empty(),
          "ArrowRain pull ribbon survived its fixed 0.45 second window");

    auto unrelated = program;
    unrelated.effect_lookup[0].effect_id = Hash64("particle.line.ricochet");
    Check(hs::runtime_detail::BuildVfxEventRibbonInputs(unrelated, events, 100).empty(),
          "unrelated link event identity was accepted");
    auto malformed = program;
    malformed.effects[0].seconds = 0.0f;
    Check(hs::runtime_detail::BuildVfxEventRibbonInputs(malformed, events, 100).empty(),
          "ArrowRain pull recipe with nonpositive duration was accepted");
    malformed = program;
    malformed.outputs[0].shape = Hash("camera_facing_ribbon");
    Check(hs::runtime_detail::BuildVfxEventRibbonInputs(malformed, events, 100).empty(),
          "ArrowRain pull recipe with a non-bezier core was accepted");
    malformed = program;
    auto invalid_event = event;
    const auto source = std::get<hs::VfxLinkPayload>(event.payload).source_position;
    invalid_event.payload = hs::VfxLinkPayload{
        source, source, 712, 712, 0.0f, 0.12f, 0};
    Check(hs::runtime_detail::BuildVfxEventRibbonInputs(
              malformed, std::array{invalid_event}, 100).empty(),
          "ArrowRain pull event accepted coincident endpoints");
}

hs::VfxProgramData FixedAnalyticRibbonProgram()
{
    constexpr std::array ids{"particle.common.pull",
                             "particle.skill.retreat_shot.move",
                             "particle.upgrade.explosive.pre_pull"};
    hs::VfxProgramData program;
    program.parameters = {
        {Hash("segments"), hs::VfxParameterType::Int, 16},
        FloatParam("taper", 0.55f),
        {Hash("travel_pulse"), hs::VfxParameterType::Bool, 1},
        FloatParam("width_multiplier", 2.8f),
        FloatParam("noise_warp", 0.08f)};
    for (std::size_t effect_index = 0; effect_index < ids.size(); ++effect_index)
    {
        hs::VfxEffectRecord effect{};
        effect.handle = static_cast<std::uint32_t>(effect_index + 1);
        effect.input_mode = 0;
        effect.importance = effect_index == 2 ? 3 : 2;
        effect.payload_kind = effect_index == 2 ? Hash("CircleAreaPayload")
                                                : Hash("PointEventPayload");
        effect.timing_kind = 0;
        effect.seconds = 0.45f;
        effect.sources = {static_cast<std::uint32_t>(effect_index * 2), 2};
        program.effects.push_back(effect);
        program.effect_lookup.push_back({Hash64(ids[effect_index]), effect.handle});

        hs::VfxSourceRecord core{};
        core.effect = effect.handle;
        core.stable_id = Hash("link_core");
        core.type = hs::VfxSourceType::HistoryRibbon;
        core.knot_count = 2;
        core.knots = {0.0f, 1.0f};
        core.outputs = {static_cast<std::uint32_t>(effect_index * 2), 1};
        core.authoritative = effect_index == 2 ? 1u : 0u;
        program.sources.push_back(core);
        core.stable_id = Hash("link_outer");
        core.outputs = {static_cast<std::uint32_t>(effect_index * 2 + 1), 1};
        core.authoritative = 0;
        program.sources.push_back(core);

        hs::VfxOutputRecord add{};
        add.source = static_cast<std::uint32_t>(effect_index * 2);
        add.profile = hs::VfxOutputProfile::RibbonAdd;
        add.shape = Hash("bezier_ribbon");
        add.rgba = {1.0f, 0.7f, 0.3f, 0.75f};
        add.hdr = 3.0f;
        add.gradient_row = effect_index == 0 ? 24u : effect_index == 1 ? 37u : 32u;
        add.coverage_type = Hash("analytic");
        add.coverage_ref = Hash("ribbon_energy_core");
        add.parameters = {0, 3};
        add.motion = hs::VfxMotionKind::CurvedLinkTravelingHead;
        program.outputs.push_back(add);
        hs::VfxOutputRecord outer = add;
        outer.source = static_cast<std::uint32_t>(effect_index * 2 + 1);
        outer.profile = hs::VfxOutputProfile::RibbonOit;
        outer.rgba = {0.2f, 0.5f, 1.0f, 0.28f};
        outer.hdr = 1.3f;
        outer.parameters = {3, 2};
        outer.motion = hs::VfxMotionKind::SoftWiderSheath;
        program.outputs.push_back(outer);
    }
    return program;
}

hs::VfxEventInput FixedAnalyticRibbonEvent(std::size_t effect_index)
{
    hs::VfxEventInput event{};
    event.effect_handle = static_cast<std::uint32_t>(effect_index + 1);
    event.event_tick = 100;
    event.sequence = static_cast<hs::Sequence>(700 + effect_index);
    event.stable_seed = static_cast<std::uint32_t>(17 + effect_index);
    if (effect_index == 0)
        event.payload = hs::VfxPointPayload{{1.0f, 2.0f, 3.0f}, {}, {0, 0, 1}, 2.0f};
    else if (effect_index == 1)
        event.payload = hs::VfxPointPayload{{1.0f, 2.0f, 3.0f}, {}, {1, 0, 0}, 2.0f};
    else
        event.payload = hs::VfxCirclePayload{{1.0f, 2.0f, 3.0f}, {0, 0, 1},
                                              3.5f, 0.0f, 0.0f, 0};
    return event;
}

void TestFixedEventAnalyticRibbons()
{
    auto program = FixedAnalyticRibbonProgram();
    for (std::size_t effect_index = 0; effect_index < 3; ++effect_index)
    {
        const auto event = FixedAnalyticRibbonEvent(effect_index);
        const auto ribbons = hs::runtime_detail::BuildVfxEventRibbonInputs(
            program, std::array{event}, 100);
        Check(ribbons.size() == 1 && ribbons[0].analytic &&
                  ribbons[0].outputs.size() == 2 && ribbons[0].segments == 16 &&
                  ribbons[0].source_id == Hash("link_core") &&
                  ribbons[0].normalized_age == 0.0f,
              "fixed event did not decode its analytic link recipe");
        const auto &ribbon = ribbons[0];
        if (effect_index == 0)
            Check(ribbon.previous_position.z == 5.0f && ribbon.position.z == 3.0f,
                  "pull ribbon did not use cosmetic center-inward geometry");
        else if (effect_index == 1)
            Check(ribbon.previous_position.x == -1.0f && ribbon.position.x == 1.0f,
                  "retreat ribbon did not use cosmetic movement-direction geometry");
        else
            Check(ribbon.previous_position.z == 6.5f && ribbon.position.z == 3.0f,
                  "pre-pull ribbon did not use the actual circle radius");
        Check(ribbon.control.y > ribbon.position.y &&
                  ribbon.outputs[0].travel_pulse &&
                  ribbon.outputs[1].profile == hs::VfxOutputProfile::RibbonOit,
              "fixed event lost shared Bezier control or authored outputs");

        auto wrong_payload = event;
        if (effect_index == 2)
            wrong_payload.payload = hs::VfxPointPayload{{1, 2, 3}, {}, {0, 0, 1}, 1};
        else
            wrong_payload.payload = hs::VfxCirclePayload{{1, 2, 3}, {}, 2, 0, 0, 0};
        Check(hs::runtime_detail::BuildVfxEventRibbonInputs(
                  program, std::array{wrong_payload}, 100).empty(),
              "fixed ribbon accepted the wrong payload schema");

        auto low_quality = event;
        program.outputs[effect_index * 2 + 1].min_quality = 1;
        low_quality.quality = hs::VfxQuality::Low;
        const auto gated = hs::runtime_detail::BuildVfxEventRibbonInputs(
            program, std::array{low_quality}, 100);
        Check(gated.size() == 1 && gated[0].outputs.size() == 1,
              "fixed ribbon ignored cooked output quality");
        program.outputs[effect_index * 2 + 1].min_quality = 0;

        auto malformed = program;
        malformed.outputs[effect_index * 2].coverage_ref = Hash("ribbon_dash_repeat");
        Check(hs::runtime_detail::BuildVfxEventRibbonInputs(
                  malformed, std::array{event}, 100).empty(),
              "fixed ribbon accepted a different analytic coverage recipe");
        Check(hs::runtime_detail::BuildVfxEventRibbonInputs(
                  program, std::array{event}, 127).empty(),
              "fixed ribbon survived its fixed lifetime");
    }
}

void TestCookedFixedEventAnalyticRibbons()
{
    std::ifstream file("Cooked/vfx_program.hsbin", std::ios::binary);
    Check(file.good(), "production VFX program missing");
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file),
                                  std::istreambuf_iterator<char>()};
    hs::VfxProgramData cooked;
    std::string error;
    Check(hs::LoadVfxProgram(std::as_bytes(std::span(bytes)), cooked, error),
          "production VFX program could not be decoded");
    constexpr std::array ids{"particle.common.pull",
                             "particle.skill.retreat_shot.move",
                             "particle.upgrade.explosive.pre_pull"};
    for (std::size_t effect_index = 0; effect_index < ids.size(); ++effect_index)
    {
        const auto lookup = std::find_if(
            cooked.effect_lookup.begin(), cooked.effect_lookup.end(),
            [&](const auto &entry) { return entry.effect_id == Hash64(ids[effect_index]); });
        Check(lookup != cooked.effect_lookup.end(), "fixed ribbon cooked effect missing");
        auto event = FixedAnalyticRibbonEvent(effect_index);
        event.effect_handle = lookup->handle;
        const auto ribbons = hs::runtime_detail::BuildVfxEventRibbonInputs(
            cooked, std::array{event}, 100);
        Check(ribbons.size() == 1 && ribbons[0].analytic &&
                  ribbons[0].outputs.size() == 2 && ribbons[0].segments == 16,
              "production fixed event did not decode both analytic outputs");
        if (effect_index == 2)
            Check(ribbons[0].previous_position.z == 6.5f,
                  "production pre-pull ignored its circle radius");

        auto wrong_payload = event;
        if (effect_index == 2)
            wrong_payload.payload = hs::VfxPointPayload{{1, 2, 3}, {}, {0, 0, 1}, 1};
        else
            wrong_payload.payload = hs::VfxCirclePayload{{1, 2, 3}, {}, 2, 0, 0, 0};
        Check(hs::runtime_detail::BuildVfxEventRibbonInputs(
                  cooked, std::array{wrong_payload}, 100).empty(),
              "production fixed ribbon accepted the wrong payload schema");
        auto invalid_quality = event;
        invalid_quality.quality = static_cast<hs::VfxQuality>(3);
        Check(hs::runtime_detail::BuildVfxEventRibbonInputs(
                  cooked, std::array{invalid_quality}, 100).empty(),
              "production fixed ribbon accepted an invalid quality");
        Check(hs::runtime_detail::BuildVfxEventRibbonInputs(
                  cooked, std::array{event}, 127).empty(),
              "production fixed ribbon survived its cooked lifetime");
    }
}

void TestPickupCollectRibbon()
{
    constexpr std::array effect_ids{
        "particle.pickup.xp_collect", "particle.pickup.heal_collect",
        "particle.pickup.magnet_collect", "particle.pickup.relic_collect"};
    constexpr std::array durations{0.4f, 0.55f, 0.6f, 0.9f};
    for (std::size_t kind = 0; kind < effect_ids.size(); ++kind)
    {
        hs::VfxProgramData program;
        hs::VfxEffectRecord effect{};
        effect.handle = 1;
        effect.input_mode = 0;
        effect.timing_kind = 0;
        effect.payload_kind = Hash("SourceTargetPayload");
        effect.importance = 2;
        effect.seconds = durations[kind];
        effect.sources = {0, 1};
        program.effects.push_back(effect);
        program.effect_lookup.push_back({Hash64(effect_ids[kind]), 1});

        hs::VfxSourceRecord source{};
        source.effect = 1;
        source.stable_id = Hash("absorb_ribbon");
        source.type = hs::VfxSourceType::HistoryRibbon;
        source.knot_count = 2;
        source.knots = {0.1f, 1.0f};
        source.outputs = {0, 1};
        program.sources.push_back(source);
        program.parameters = {{Hash("segments"), hs::VfxParameterType::Int, 10},
                              FloatParam("width", 0.04f)};
        hs::VfxOutputRecord output{};
        output.source = 0;
        output.profile = hs::VfxOutputProfile::RibbonAdd;
        output.motion = hs::VfxMotionKind::CurvesTowardCollector;
        output.shape = Hash("short_bezier_to_player");
        output.coverage_type = Hash("analytic");
        output.coverage_ref = Hash("ribbon_energy_core");
        output.parameters = {0, 2};
        output.rgba = {0.8f, 0.5f, 0.2f, 0.45f};
        output.hdr = 1.6f;
        output.gradient_row = 7;
        program.outputs.push_back(output);

        hs::VfxEventInput event{};
        event.effect_handle = 1;
        event.event_tick = 100;
        event.sequence = 1234 + kind;
        event.stable_seed = 3;
        event.payload = hs::VfxLinkPayload{
            {4.0f, 0.5f, 2.0f}, {1.0f, 1.0f, 2.0f}};
        const std::array events{event};
        const auto build = [&](hs::Tick tick) {
            return hs::runtime_detail::BuildVfxEventRibbonInputs(program, events, tick);
        };
        Check(build(101).empty(), "pickup ribbon began before its cooked knot window");
        const auto active = build(106);
        Check(active.size() == 1 && active[0].outputs.size() == 1,
              "pickup collect did not decode one analytic ribbon");
        const auto &ribbon = active[0];
        const auto &ribbon_output = ribbon.outputs[0];
        Check(ribbon.owner_id == event.sequence && ribbon.effect_handle == 1 &&
                  ribbon.source_id == Hash("absorb_ribbon") &&
                  ribbon.stable_seed == event.stable_seed && ribbon.analytic &&
                  ribbon.previous_position.x == 4.0f && ribbon.position.x == 1.0f &&
                  ribbon.control.y > 0.75f && ribbon.segments == 10 &&
                  ribbon.history_seconds > 0.0f,
              "pickup ribbon lost captured endpoints, owner, or cooked geometry");
        const float expected_age = (0.1f - 0.1f * durations[kind]) /
                                   (0.9f * durations[kind]);
        Check(std::abs(ribbon.normalized_age - expected_age) < 0.0001f &&
                  ribbon_output.width_head == 0.04f &&
                  ribbon_output.width_tail == 0.04f &&
                  ribbon_output.motion == hs::VfxMotionKind::CurvesTowardCollector &&
                  ribbon_output.profile == hs::VfxOutputProfile::RibbonAdd &&
                  ribbon_output.color.w == 0.45f && ribbon_output.hdr == 1.6f &&
                  ribbon_output.gradient_row == 7,
              "pickup ribbon did not preserve authored material or local travel age");
        if (kind == 0)
        {
            auto adjusted = program;
            adjusted.parameters[0].bits = 12;
            adjusted.parameters[1] = FloatParam("width", 0.05f);
            const auto changed = hs::runtime_detail::BuildVfxEventRibbonInputs(
                adjusted, events, 106);
            Check(changed.size() == 1 && changed[0].segments == 12 &&
                      changed[0].outputs[0].width_head == 0.05f,
                  "pickup ribbon ignored valid cooked segment or width values");

            auto textured = program;
            hs::VfxTextureBindingRecord detail{};
            detail.role = Hash("ribbon_detail_array");
            detail.selection = Hash("stable_seed_mod_group_size");
            detail.slices = {0, 2};
            detail.strength = 0.3f;
            textured.texture_bindings.push_back(detail);
            textured.slice_indices = {1, 3};
            textured.outputs[0].textures = {0, 1};
            const std::string asset = "Content/Textures/VFX/vfx_ribbon_detail_array.dds";
            textured.texture_resources.push_back({0, {0,
                static_cast<std::uint32_t>(asset.size())}});
            textured.strings.assign(
                reinterpret_cast<const std::byte *>(asset.data()),
                reinterpret_cast<const std::byte *>(asset.data() + asset.size()));
            const auto with_detail = hs::runtime_detail::BuildVfxEventRibbonInputs(
                textured, events, 106);
            Check(with_detail.size() == 1 &&
                      with_detail[0].outputs[0].detail_slice == 3 &&
                      with_detail[0].outputs[0].detail_strength == 0.3f,
                  "pickup ribbon lost its cooked texture binding");
            textured.slice_indices[1] = 8;
            Check(hs::runtime_detail::BuildVfxEventRibbonInputs(
                      textured, events, 106).empty(),
                  "pickup ribbon accepted an invalid detail texture slice");
        }
        const auto last_tick = static_cast<hs::Tick>(
            100 + std::ceil(durations[kind] * 60.0f));
        Check(build(last_tick).empty(), "pickup ribbon survived its cooked lifetime");

        auto invalid = program;
        invalid.effect_lookup[0].effect_id = Hash64("particle.line.ricochet");
        Check(hs::runtime_detail::BuildVfxEventRibbonInputs(invalid, events, 106).empty(),
              "unrelated link identity decoded as a pickup ribbon");
        invalid = program;
        invalid.effects[0].payload_kind = Hash("PointEventPayload");
        Check(hs::runtime_detail::BuildVfxEventRibbonInputs(invalid, events, 106).empty(),
              "pickup ribbon accepted the wrong payload contract");
        invalid = program;
        invalid.sources[0].knots[0] = 0.0f;
        Check(hs::runtime_detail::BuildVfxEventRibbonInputs(invalid, events, 106).empty(),
              "pickup ribbon accepted a different knot recipe");
        invalid = program;
        invalid.effects[0].seconds = 0.0f;
        Check(hs::runtime_detail::BuildVfxEventRibbonInputs(invalid, events, 106).empty(),
              "pickup ribbon accepted a missing lifetime");
        invalid = program;
        invalid.sources[0].outputs.count = 0;
        Check(hs::runtime_detail::BuildVfxEventRibbonInputs(invalid, events, 106).empty(),
              "pickup ribbon accepted a missing authored output");
        invalid = program;
        invalid.outputs[0].shape = Hash("bezier_ribbon");
        Check(hs::runtime_detail::BuildVfxEventRibbonInputs(invalid, events, 106).empty(),
              "pickup ribbon accepted a different output shape");
        invalid = program;
        invalid.outputs[0].coverage_ref = Hash("ribbon_dash_repeat");
        Check(hs::runtime_detail::BuildVfxEventRibbonInputs(invalid, events, 106).empty(),
              "pickup ribbon accepted a different coverage recipe");
        invalid = program;
        invalid.parameters[1].bits = std::bit_cast<std::uint32_t>(
            std::numeric_limits<float>::quiet_NaN());
        Check(hs::runtime_detail::BuildVfxEventRibbonInputs(invalid, events, 106).empty(),
              "pickup ribbon accepted a nonfinite cooked width");
        invalid = program;
        invalid.parameters[0].type = hs::VfxParameterType::Float;
        Check(hs::runtime_detail::BuildVfxEventRibbonInputs(invalid, events, 106).empty(),
              "pickup ribbon accepted malformed cooked segments");
        invalid = program;
        invalid.parameters[1].key = Hash("width_head");
        Check(hs::runtime_detail::BuildVfxEventRibbonInputs(invalid, events, 106).empty(),
              "pickup ribbon supplied a fallback width");
        auto coincident = event;
        auto coincident_link = std::get<hs::VfxLinkPayload>(coincident.payload);
        coincident_link.target_position = coincident_link.source_position;
        coincident.payload = coincident_link;
        Check(hs::runtime_detail::BuildVfxEventRibbonInputs(
                  program, std::array{coincident}, 106).empty(),
              "pickup ribbon accepted coincident endpoints");
        auto malformed = event;
        auto malformed_link = std::get<hs::VfxLinkPayload>(malformed.payload);
        malformed_link.target_position.x = std::numeric_limits<float>::infinity();
        malformed.payload = malformed_link;
        Check(hs::runtime_detail::BuildVfxEventRibbonInputs(
                  program, std::array{malformed}, 106).empty(),
              "pickup ribbon accepted a nonfinite endpoint");
        Check(build(99).empty(), "pickup ribbon decoded before its event tick");
    }
}

void TestCookedPickupCollectRibbon()
{
    std::ifstream file("Cooked/vfx_program.hsbin", std::ios::binary);
    Check(file.good(), "production VFX program missing");
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file),
                                  std::istreambuf_iterator<char>()};
    hs::VfxProgramData cooked;
    std::string error;
    Check(hs::LoadVfxProgram(std::as_bytes(std::span(bytes)), cooked, error),
          "production VFX program could not be decoded");

    constexpr std::array pickup_names{
        "particle.pickup.xp_collect", "particle.pickup.heal_collect",
        "particle.pickup.magnet_collect", "particle.pickup.relic_collect"};
    constexpr hs::Float3 source{4.0f, 0.5f, 2.0f};
    constexpr hs::Float3 target{1.0f, 1.0f, 2.0f};
    std::array<hs::VfxEventInput, pickup_names.size()> events{};
    std::array<hs::VfxEffectHandle, pickup_names.size()> handles{};
    for (std::size_t kind = 0; kind < pickup_names.size(); ++kind)
    {
        const auto identity = std::find_if(
            cooked.effect_lookup.begin(), cooked.effect_lookup.end(),
            [&](const auto &entry) { return entry.effect_id == Hash64(pickup_names[kind]); });
        Check(identity != cooked.effect_lookup.end(),
              "production pickup collect effect lookup missing");
        Check(std::count_if(cooked.effect_lookup.begin(), cooked.effect_lookup.end(),
                            [&](const auto &entry) {
                                return entry.effect_id == identity->effect_id;
                            }) == 1,
              "production pickup collect effect lookup was duplicated");
        Check(identity->handle > 0 && identity->handle <= cooked.effects.size(),
              "production pickup collect effect handle is invalid");
        for (std::size_t prior = 0; prior < kind; ++prior)
            Check(handles[prior] != identity->handle,
                  "production pickup collect effects reused a lookup handle");
        handles[kind] = identity->handle;

        auto &event = events[kind];
        event.effect_handle = identity->handle;
        event.event_tick = 100;
        event.sequence = 200 + static_cast<hs::Sequence>(kind);
        event.stable_seed = 17 + static_cast<std::uint32_t>(kind);
        event.payload = hs::VfxLinkPayload{source, target};
    }

    const auto ribbons = hs::runtime_detail::BuildVfxEventRibbonInputs(
        cooked, events, 112);
    Check(ribbons.size() == pickup_names.size(),
          "production pickup collect events did not decode one ribbon each");
    for (std::size_t kind = 0; kind < pickup_names.size(); ++kind)
    {
        const auto ribbon = std::find_if(
            ribbons.begin(), ribbons.end(),
            [&](const auto &candidate) {
                return candidate.effect_handle == handles[kind];
            });
        Check(ribbon != ribbons.end() && ribbon->owner_id == events[kind].sequence &&
                  ribbon->analytic && !ribbon->outputs.empty() &&
                  ribbon->previous_position.x == source.x &&
                  ribbon->previous_position.y == source.y &&
                  ribbon->previous_position.z == source.z &&
                  ribbon->position.x == target.x &&
                  ribbon->position.y == target.y &&
                  ribbon->position.z == target.z,
              "production pickup collect ribbon lost its live link or output");
    }
}

int main()
{
    try
    {
        TestBossDashWakeRibbon();
        TestCookedBossDashWakeRibbon();
        TestEventRecoilRibbon();
        TestAnalyticLinks();
        TestEventImpactAxialRibbon();
        TestEventArrowRainPullRibbon();
        TestFixedEventAnalyticRibbons();
        TestPickupCollectRibbon();
        TestCookedPickupCollectRibbon();
        TestCookedFixedEventAnalyticRibbons();
        hs::VfxProgramData program;
        hs::VfxEffectRecord effect{};
        effect.handle = 1;
        effect.input_mode = 1;
        effect.importance = 4;
        effect.seconds = 0.8f;
        effect.payload_kind = Hash("ProjectilePathPayload");
        effect.sources = {0, 1};
        program.effects.push_back(effect);

        program.parameters = {
            FloatParam("history_seconds", 0.12f),
            FloatParam("width_head", 0.085f),
            FloatParam("width_tail", 0.012f),
            FloatParam("noise_warp", 0.08f),
            FloatParam("phase_offset", 0.25f)};
        hs::VfxSourceRecord source{};
        source.effect = 1;
        source.stable_id = Hash("projectile_wake");
        source.type = hs::VfxSourceType::HistoryRibbon;
        source.knot_count = 4;
        source.knots = {0.0f, 0.12f, 0.65f, 1.0f};
        source.parameters = {0, 1};
        source.outputs = {0, 4};
        program.sources.push_back(source);

        hs::VfxOutputRecord add{};
        add.source = 0;
        add.profile = hs::VfxOutputProfile::RibbonAdd;
        add.shape = Hash("camera_facing_ribbon");
        add.rgba = {0.1f, 0.2f, 0.3f, 0.8f};
        add.hdr = 1.4f;
        add.gradient_row = 12;
        add.motion = hs::VfxMotionKind::ShortTurbulentWake;
        add.parameters = {1, 4};
        add.textures = {0, 1};
        add.coverage_ref = Hash("ribbon_energy_core");
        hs::VfxOutputRecord oit = add;
        oit.profile = hs::VfxOutputProfile::RibbonOit;
        oit.motion = hs::VfxMotionKind::TwoPhaseNarrowWakes;
        oit.shape = Hash("dual_camera_ribbon");
        oit.coverage_ref = Hash("ribbon_dash_repeat");
        hs::VfxOutputRecord wrong_motion = add;
        wrong_motion.motion = hs::VfxMotionKind::VelocityAligned;
        hs::VfxOutputRecord wrong_shape = add;
        wrong_shape.shape = Hash("bezier_ribbon");
        program.outputs = {add, oit, wrong_motion, wrong_shape};
        hs::VfxTextureBindingRecord detail{};
        detail.role = Hash("ribbon_detail_array");
        detail.slices = {0, 2};
        detail.strength = 0.3f;
        program.texture_bindings.push_back(detail);
        program.slice_indices = {1, 3};
        const std::string detail_asset =
            "Content/Textures/VFX/vfx_ribbon_detail_array.dds";
        program.texture_resources.push_back({0, {0,
            static_cast<std::uint32_t>(detail_asset.size())}});
        program.strings.assign(reinterpret_cast<const std::byte *>(detail_asset.data()),
                               reinterpret_cast<const std::byte *>(detail_asset.data() +
                                                                    detail_asset.size()));

        hs::VfxPersistentInput input;
        input.stable_id = 0x1234;
        input.effect_handle = 1;
        input.stable_seed = 3;
        input.elapsed_seconds = 0.05f;
        hs::VfxProjectilePathPayload path;
        path.current_position = {4.0f, 1.0f, 6.0f};
        path.previous_position = {3.0f, 1.0f, 6.0f};
        path.velocity = {60.0f, 0.0f, 2.0f};
        path.projectile_radius = 0.2f;
        input.payload = path;
        const std::array inputs{input};
        const auto result = hs::runtime_detail::BuildVfxTypedRibbonInputs(
            program, inputs);
        Check(result.size() == 1 && result[0].outputs.size() == 2,
              "ribbon outputs were not grouped by source");
        const auto &ribbon = result[0];
        Check(ribbon.owner_id == input.stable_id &&
                  ribbon.source_id == source.stable_id &&
                  ribbon.position.x == 4.0f && ribbon.previous_position.x == 3.0f &&
                  ribbon.history_seconds == 0.12f &&
                  ribbon.start_alpha > 0.51f && ribbon.start_alpha < 0.53f,
              "ribbon owner or lifecycle data changed");
        Check(ribbon.outputs[0].width_head == 0.085f &&
                  ribbon.outputs[0].width_tail == 0.012f &&
                  ribbon.outputs[0].noise_warp == 0.08f &&
                  ribbon.outputs[0].detail_slice == 3 &&
                  ribbon.outputs[0].detail_strength == 0.3f,
              "ribbon authored parameters or detail slice changed");
        Check(ribbon.outputs[1].profile == hs::VfxOutputProfile::RibbonOit &&
                  ribbon.outputs[1].dashed,
              "ribbon OIT or dash coverage was not preserved");

        // The authored projectile-follow head recipe carries ProjectilePayload;
        // ribbon extraction must consume its owner transforms without requiring
        // a legacy path payload.
        auto head_input = input;
        head_input.current_transform[12] = 4.0f;
        head_input.current_transform[13] = 1.0f;
        head_input.current_transform[14] = 6.0f;
        head_input.previous_transform[12] = 3.0f;
        head_input.previous_transform[13] = 1.0f;
        head_input.previous_transform[14] = 6.0f;
        hs::VfxProjectilePayload head_payload;
        head_payload.velocity = {60.0f, 0.0f, 2.0f};
        head_payload.hitbox_radius = 0.2f;
        head_input.payload = head_payload;
        const std::array head_inputs{head_input};
        auto head_program = program;
        head_program.effects[0].payload_kind = Hash("ProjectilePayload");
        const auto head_result = hs::runtime_detail::BuildVfxTypedRibbonInputs(
            head_program, head_inputs);
        Check(head_result.size() == 1 && head_result[0].position.x == 4.0f &&
                  head_result[0].previous_position.x == 3.0f,
              "projectile head payload was not routed to history ribbon");

        auto invalid_detail = program;
        invalid_detail.texture_bindings[0].slices = {1, 2};
        const auto rejected = hs::runtime_detail::BuildVfxTypedRibbonInputs(
            invalid_detail, inputs);
        Check(rejected.empty(), "out-of-range detail slice binding was accepted");
        invalid_detail = program;
        invalid_detail.slice_indices = {8, 9};
        Check(hs::runtime_detail::BuildVfxTypedRibbonInputs(invalid_detail, inputs).empty(),
              "DDS slice index outside the actual eight-slice array was accepted");
        auto old_owner = input;
        old_owner.elapsed_seconds = 20.0f;
        const std::array old_owners{old_owner};
        const auto old_ribbon = hs::runtime_detail::BuildVfxTypedRibbonInputs(program, old_owners);
        Check(old_ribbon.size() == 1 && old_ribbon[0].start_alpha == 1.0f,
              "live owner faded out at an invented projectile expiry");

        auto invalid_parameter = program;
        invalid_parameter.parameters[3].bits = std::bit_cast<std::uint32_t>(
            std::numeric_limits<float>::quiet_NaN());
        const auto rejected_parameter = hs::runtime_detail::BuildVfxTypedRibbonInputs(
            invalid_parameter, inputs);
        Check(rejected_parameter.empty(), "nonfinite ribbon parameter was accepted");
        std::cout << "Typed VFX ribbon command tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
