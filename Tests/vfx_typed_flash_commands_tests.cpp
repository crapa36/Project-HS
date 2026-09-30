#include "../Source/Runtime/Private/vfx_typed_flash_commands.hpp"
#include <hs/core/render_snapshot.hpp>

#include <array>
#include <algorithm>
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
constexpr std::uint32_t Hash32(std::string_view value)
{
    std::uint32_t hash = 2166136261u;
    for (const unsigned char character : value)
    {
        hash ^= character;
        hash *= 16777619u;
    }
    return hash;
}
constexpr std::uint64_t Hash64(std::string_view value)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char character : value)
    {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    return hash;
}

void Require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

bool Near(float actual, float expected)
{
    return std::abs(actual - expected) < 0.00001f;
}

hs::VfxProgramData Program()
{
    hs::VfxProgramData cooked;
    hs::VfxEffectRecord effect;
    effect.handle = 1;
    effect.input_mode = 0;
    effect.payload_kind = Hash32("PointEventPayload");
    effect.seconds = 0.28f;
    effect.sources = {0, 1};
    cooked.effects.push_back(effect);

    hs::VfxEffectRecord second = effect;
    second.handle = 2;
    second.seconds = 0.4f;
    second.sources = {1, 1};
    cooked.effects.push_back(second);

    hs::VfxEffectRecord unrelated = effect;
    unrelated.handle = 3;
    unrelated.payload_kind = Hash32("ContextPayload");
    unrelated.sources = {2, 0};
    cooked.effects.push_back(unrelated);

    hs::VfxSourceRecord source;
    source.effect = 1;
    source.stable_id = Hash32("contact_flash");
    source.type = hs::VfxSourceType::ImpactSprite;
    source.knot_count = 2;
    source.knots = {0.0f, 0.18f, 0.0f, 0.0f};
    source.outputs = {0, 1};
    cooked.sources.push_back(source);
    source.effect = 2;
    source.knots = {0.0f, 0.25f, 0.0f, 0.0f};
    source.outputs = {1, 1};
    cooked.sources.push_back(source);

    const hs::VfxParameterRecord size{Hash32("size"),
                                      hs::VfxParameterType::Float,
                                      std::bit_cast<std::uint32_t>(0.35f)};
    cooked.parameters.push_back(size);

    hs::VfxOutputRecord output;
    output.source = 0;
    output.profile = hs::VfxOutputProfile::SpriteAdd;
    output.shape = Hash32("soft_disc");
    output.motion = hs::VfxMotionKind::InstantPointFlash;
    output.rgba = {1.0f, 1.0f, 1.0f, 0.35f};
    output.hdr = 3.0f;
    output.gradient_row = 12;
    output.parameters = {0, 1};
    cooked.outputs.push_back(output);
    output.source = 1;
    output.rgba = {0.5f, 0.25f, 0.1f, 0.4f};
    output.hdr = 2.0f;
    cooked.outputs.push_back(output);
    return cooked;
}

hs::VfxEventInput HitInput()
{
    hs::VfxEventInput input;
    input.effect_handle = 1;
    input.event_tick = 19;
    input.stable_seed = 0x1234abcd;
    hs::VfxPointPayload point;
    point.position = {2.0f, 0.25f, -4.0f};
    point.authored_scale = 1.0f;
    input.payload = point;
    return input;
}

hs::VfxProgramData SecondaryFanProgram()
{
    auto cooked = Program();
    hs::VfxEffectRecord effect = cooked.effects.front();
    effect.handle = 4;
    effect.payload_kind = Hash32("ConePayload");
    effect.seconds = 0.5f;
    effect.sources = {2, 2};
    cooked.effects.push_back(effect);

    hs::VfxSourceRecord muzzle;
    muzzle.effect = 4;
    muzzle.stable_id = Hash32("muzzle_primary");
    muzzle.type = hs::VfxSourceType::ImpactSprite;
    muzzle.knot_count = 2;
    muzzle.knots = {0.0f, 0.35f, 0.0f, 0.0f};
    muzzle.outputs = {2, 1};
    cooked.sources.push_back(muzzle);

    hs::VfxSourceRecord ribbon = muzzle;
    ribbon.stable_id = Hash32("bow_ribbon");
    ribbon.type = hs::VfxSourceType::HistoryRibbon;
    ribbon.outputs = {3, 1};
    cooked.sources.push_back(ribbon);

    cooked.parameters.push_back({Hash32("sdf"), hs::VfxParameterType::Enum,
                                 Hash32("asymmetric_star_slash")});
    cooked.parameters.push_back({Hash32("scale_curve_ref"), hs::VfxParameterType::CurveRow, 2});

    hs::VfxOutputRecord muzzle_output;
    muzzle_output.source = 2;
    muzzle_output.profile = hs::VfxOutputProfile::SpriteSdfAdd;
    muzzle_output.shape = Hash32("radial_slash_sdf");
    muzzle_output.motion = hs::VfxMotionKind::SnapsOutwardAim;
    muzzle_output.rgba = {0.25f, 0.5f, 0.75f, 0.9f};
    muzzle_output.hdr = 4.0f;
    muzzle_output.gradient_row = 9;
    muzzle_output.parameters = {1, 2};
    cooked.outputs.push_back(muzzle_output);

    hs::VfxOutputRecord ribbon_output;
    ribbon_output.source = 3;
    ribbon_output.profile = hs::VfxOutputProfile::RibbonAdd;
    ribbon_output.shape = Hash32("short_arc_ribbon");
    ribbon_output.motion = hs::VfxMotionKind::CurvedRecoilArc;
    ribbon_output.rgba = {1.0f, 0.75f, 0.25f, 0.55f};
    ribbon_output.hdr = 2.0f;
    cooked.outputs.push_back(ribbon_output);

    const std::string path = "Content/Textures/VFX/vfx_authored_mask_array.dds";
    const auto path_offset = static_cast<std::uint32_t>(cooked.strings.size());
    for (const char character : path)
        cooked.strings.push_back(static_cast<std::byte>(character));
    cooked.texture_resources.push_back({7, {path_offset, static_cast<std::uint32_t>(path.size())}});
    cooked.slice_indices = {1, 3, 5};
    hs::VfxTextureBindingRecord mask;
    mask.role = Hash32("authored_mask_array");
    mask.catalog_slot = 7;
    mask.selection = Hash32("stable_seed_mod_group_size");
    mask.strength = 0.48f;
    mask.slices = {0, 3};
    cooked.texture_bindings.push_back(mask);
    cooked.outputs[2].textures = {0, 1};
    cooked.effect_lookup.push_back({Hash64("particle.upgrade.multishot.secondary_fan"), 4});
    return cooked;
}

hs::VfxEventInput SecondaryFanInput()
{
    hs::VfxEventInput input;
    input.effect_handle = 4;
    input.event_tick = 37;
    input.stable_seed = 5;
    hs::VfxConePayload cone;
    cone.origin = {2.0f, 0.25f, -4.0f};
    cone.direction = {3.0f, 0.0f, 4.0f};
    cone.range = 16.0f;
    cone.half_angle_degrees = 25.0f;
    input.payload = cone;
    return input;
}

hs::VfxProgramData BossEnergyProgram(std::uint64_t effect_id, std::uint32_t payload_kind, float seconds)
{
    auto cooked = SecondaryFanProgram();
    cooked.effects.back().payload_kind = payload_kind;
    cooked.effects.back().seconds = seconds;
    cooked.effect_lookup[0].effect_id = effect_id;
    cooked.sources[2].stable_id = Hash32("boss_energy");
    cooked.sources[2].knots = {0.0f, 0.8f, 0.0f, 0.0f};
    cooked.parameters = {{Hash32("fbm_octaves"), hs::VfxParameterType::Int, 4}};
    cooked.outputs[2].profile = hs::VfxOutputProfile::SpriteNoiseOit;
    cooked.outputs[2].shape = Hash32("radial_energy");
    cooked.outputs[2].motion = hs::VfxMotionKind::DirectionalTurbulence;
    cooked.outputs[2].rgba = {0.2f, 0.4f, 0.6f, 0.65f};
    cooked.outputs[2].hdr = 3.0f;
    cooked.outputs[2].parameters = {0, 1};
    cooked.texture_bindings[0].strength = 0.28f;
    return cooked;
}

hs::VfxEventInput BossEnergyInput(bool ring_payload)
{
    auto input = SecondaryFanInput();
    if (!ring_payload)
        return input;
    hs::VfxRingGapsPayload ring;
    ring.center = {-5.0f, 0.4f, 7.0f};
    ring.inner_radius = 2.5f;
    ring.outer_radius = 11.0f;
    ring.gap_half_width_degrees = 9.0f;
    ring.gap_angles_degrees = {17.0f, 205.0f};
    input.payload = ring;
    return input;
}

void DecodesOnlyAuthoredContactFlash()
{
    const auto cooked = Program();
    const std::vector inputs{HitInput(), hs::VfxEventInput{}};
    const auto spawns = hs::runtime_detail::BuildVfxTypedFlashCommands(cooked, inputs);
    Require(spawns.size() == 1, "contact flash decoder emitted unrelated input");

    const auto &spawn = spawns.front();
    Require(spawn.gradient_row == 12 && Near(spawn.hdr, 3.0f) &&
            Near(spawn.normalized_age, 0.0f), "flash LUT inputs were lost");
    Require(Near(spawn.lifetime, 0.0504f), "source knot lifetime was not scaled by effect seconds");
    Require(Near(spawn.delay, 0.0f), "contact flash start delay changed");
    Require(Near(spawn.size, 0.35f), "cooked contact flash size was not preserved");
    Require(Near(spawn.color.x, 3.0f) && Near(spawn.color.y, 3.0f) &&
            Near(spawn.color.z, 3.0f) && Near(spawn.color.w, 0.35f),
            "cooked HDR color or alpha was not preserved");
    Require(spawn.position.x == 2.0f && spawn.position.y == 0.25f &&
            spawn.position.z == -4.0f, "event position was changed");
    Require(spawn.event_tick == 19 && spawn.stable_seed == 0x1234abcd,
            "event identity was not retained");
}

void HonorsCookedTimingChanges()
{
    auto cooked = Program();
    cooked.sources[0].knots[0] = 0.1f;
    cooked.sources[0].knots[1] = 0.35f;
    const std::vector inputs{HitInput()};
    const auto spawns = hs::runtime_detail::BuildVfxTypedFlashCommands(cooked, inputs);
    Require(spawns.size() == 1, "valid cooked timing disabled the flash");
    Require(Near(spawns[0].delay, 0.028f) && Near(spawns[0].lifetime, 0.07f),
            "authored start and end knots were not applied");
}

void DecodesEveryMatchingEffectAndRejectsUnsupportedOutput()
{
    auto cooked = Program();
    auto second = HitInput();
    second.effect_handle = 2;
    second.event_tick = 27;
    auto unrelated = HitInput();
    unrelated.effect_handle = 3;
    const std::vector inputs{HitInput(), second, unrelated};
    const auto spawns = hs::runtime_detail::BuildVfxTypedFlashCommands(cooked, inputs);
    Require(spawns.size() == 2, "matching contact flashes were not executed once per event");
    Require(Near(spawns[1].lifetime, 0.1f) &&
            Near(spawns[1].color.x, 1.0f) &&
            Near(spawns[1].color.y, 0.5f) &&
            Near(spawns[1].color.z, 0.2f) &&
            Near(spawns[1].color.w, 0.4f) && spawns[1].event_tick == 27,
            "second effect did not preserve its cooked output");
    cooked.outputs[1].profile = hs::VfxOutputProfile::SpriteSdfAdd;
    Require(hs::runtime_detail::BuildVfxTypedFlashCommands(cooked, inputs).size() == 1,
            "unsupported output profile was executed");
}

void AuthoredSdfShapesAndContext()
{
    struct Case { const char *shape; hs::VfxMotionKind motion; hs::VfxImpactShape kind; float size; int curve; const char *sdf; };
    const Case cases[]{
        {"directional_slash_sdf", hs::VfxMotionKind::TwoFrameSnapPerpendicular, hs::VfxImpactShape::CrossSlash, .22f, -1, "cross_slash"},
        {"soft_disc_noise", hs::VfxMotionKind::InstantExpansionCollapse, hs::VfxImpactShape::NoiseDisc, .32f, 0, nullptr},
        {"status_symbol_sdf", hs::VfxMotionKind::FastStampBreakout, hs::VfxImpactShape::StatusStamp, .25f, 4, nullptr},
        {"radial_slash_sdf", hs::VfxMotionKind::SnapsOutwardAim, hs::VfxImpactShape::RadialSlash, .30f, 2, "asymmetric_star_slash"},
        {"small_pulse", hs::VfxMotionKind::SingleShortPulse, hs::VfxImpactShape::Pulse, .25f, 6, nullptr},
        {"small_star_ring", hs::VfxMotionKind::SnapsInwardUpward, hs::VfxImpactShape::StarRing, .4f, -1, "star_ring"}
    };
    for (const auto &test : cases)
    {
        auto c = Program();
        c.parameters.clear();
        auto &o = c.outputs[0];
        o.profile = hs::VfxOutputProfile::SpriteSdfAdd;
        o.shape = Hash32(test.shape); o.motion = test.motion;
        if (test.curve >= 0) c.parameters.push_back({Hash32("scale_curve_ref"), hs::VfxParameterType::CurveRow, static_cast<std::uint32_t>(test.curve)});
        if (test.sdf) c.parameters.push_back({Hash32("sdf"), hs::VfxParameterType::Enum, Hash32(test.sdf)});
        if (test.kind == hs::VfxImpactShape::CrossSlash)
            c.parameters.push_back({Hash32("aspect"), hs::VfxParameterType::Float, std::bit_cast<std::uint32_t>(2.4f)});
        if (test.kind == hs::VfxImpactShape::NoiseDisc)
            c.parameters.push_back({Hash32("noise"), hs::VfxParameterType::Enum, Hash32("fbm")});
        o.parameters = {0, static_cast<std::uint32_t>(c.parameters.size())};
        auto input = HitInput();
        auto &point = std::get<hs::VfxPointPayload>(input.payload);
        point.authored_scale = 2; point.direction = {3, 0, 4};
        auto run = [&] { return hs::runtime_detail::BuildVfxTypedFlashCommands(c, std::span(&input, 1)); };
        auto decoded = run();
        Require(decoded.size() == 1 && decoded[0].shape == test.kind && Near(decoded[0].size, test.size * 2), "SDF shape or metre scale missing");
        Require(Near(decoded[0].direction.x, .6f) && Near(decoded[0].direction.z, .8f), "incoming direction was lost");
        if (test.curve >= 0) Require(decoded[0].curve_row == static_cast<std::uint32_t>(test.curve), "curve row changed");
        c.effects[0].payload_kind = Hash32("PresentationContextPayload");
        hs::VfxContextPayload context; context.direction = {0, 0, -2}; context.ratio01 = .12f;
        input.payload = context; input.world_transform[12] = 9; input.world_transform[14] = -7;
        decoded = run();
        Require(decoded.size() == 1 && Near(decoded[0].size, test.size) && Near(decoded[0].position.x, 9) &&
                Near(decoded[0].position.z, -7) && Near(decoded[0].direction.z, -1), "Context ratio incorrectly scaled the effect or translation missing");
        const auto motion = o.motion;
        o.motion = hs::VfxMotionKind::BakedSixWayPlume;
        Require(run().empty(), "mismatched motion accepted"); o.motion = motion;
        o.profile = hs::VfxOutputProfile::SpriteAdd;
        Require(run().empty(), "mismatched additive profile accepted"); o.profile = hs::VfxOutputProfile::SpriteSdfAdd;
        c.effects[0].timing_kind = 1;
        Require(run().empty(), "gameplay lifetime accepted as fixed event"); c.effects[0].timing_kind = 0;
        if (test.curve >= 0)
        {
            c.parameters[0].bits = 7;
            Require(run().empty(), "out-of-range curve LUT row accepted");
            c.parameters[0].bits = static_cast<std::uint32_t>(test.curve);
        }
        c.sources[0].knot_count = 4;
        Require(run().empty(), "unsupported fade envelope silently discarded");
    }
}
void MasksQualityAndMalformedInputs()
{
    auto c = Program();
    const std::string path = "Content/Textures/VFX/vfx_authored_mask_array.dds";
    for (const char character : path) c.strings.push_back(static_cast<std::byte>(character));
    c.texture_resources.push_back({3, {0, static_cast<std::uint32_t>(path.size())}});
    hs::VfxTextureBindingRecord binding;
    binding.role = Hash32("authored_mask_array"); binding.catalog_slot = 3; binding.selection = Hash32("stable_seed_mod_group_size");
    binding.slices = {0, 3}; binding.strength = .48f; binding.min_quality = 1;
    c.slice_indices = {0, 1, 2}; c.texture_bindings.push_back(binding); c.outputs[0].textures = {0, 1};
    auto input = HitInput(); input.stable_seed = 5;
    auto run = [&] { return hs::runtime_detail::BuildVfxTypedFlashCommands(c, std::span(&input, 1)); };
    auto result = run();
    Require(result.size() == 1 && result[0].mask_slice == 2 && Near(result[0].mask_strength, .48f), "stable authored mask selection missing");
    c.effects[0].sources = {1, 1};
    c.sources[1] = c.sources[0]; c.sources[1].outputs = {1, 1};
    c.outputs[1] = c.outputs[0]; c.outputs[1].source = 1;
    result = run();
    Require(result.size() == 1 && result[0].mask_slice == 2, "catalog source reordering changed stable mask selection");
    c.effects[0].sources = {0, 1};
    input.quality = hs::VfxQuality::Low;
    result = run();
    Require(result.size() == 1 && result[0].mask_slice == 0xffffffffu, "mask quality threshold ignored");
    input.quality = hs::VfxQuality::High;
    c.slice_indices[0] = 12;
    Require(run().empty(), "malformed unselected array slice accepted"); c.slice_indices[0] = 0;
    c.texture_bindings[0].strength = std::numeric_limits<float>::quiet_NaN();
    Require(run().empty(), "NaN texture strength accepted"); c.texture_bindings[0].strength = .48f;
    c.texture_bindings[0].slices.count = 99;
    Require(run().empty(), "out-of-bounds slice span accepted"); c.texture_bindings[0].slices.count = 3;
    c.strings[0] = std::byte{'X'};
    Require(run().empty(), "wrong texture resource accepted"); c.strings[0] = std::byte{'C'};
    c.outputs[0].min_quality = 2; input.quality = hs::VfxQuality::Medium;
    Require(run().empty(), "output quality threshold ignored"); input.quality = hs::VfxQuality::High;
    std::get<hs::VfxPointPayload>(input.payload).direction.x = std::numeric_limits<float>::infinity();
    Require(run().empty(), "infinite incoming direction accepted");
    input = HitInput(); c.parameters[0].type = hs::VfxParameterType::Int;
    Require(run().empty(), "wrong size parameter type accepted");
}
void ProjectileContactRecipes()
{
    const auto id=[](std::string_view name){std::uint64_t h=14695981039346656037ull;for(unsigned char c:name){h^=c;h*=1099511628211ull;}return h;};
    auto c=Program();auto input=HitInput();c.effects[0].payload_kind=Hash32("ProjectilePayload");
    hs::VfxProjectilePayload projectile;projectile.velocity={3,0,4};projectile.hitbox_radius=.27f;input.payload=projectile;
    input.world_transform[12]=7;input.world_transform[13]=.4f;input.world_transform[14]=-2;
    c.parameters={{Hash32("scale_curve_ref"),hs::VfxParameterType::CurveRow,0}};c.outputs[0].parameters={0,1};c.outputs[0].profile=hs::VfxOutputProfile::SpriteSdfAdd;
    const auto run=[&]{return hs::runtime_detail::BuildVfxTypedFlashCommands(c,std::span(&input,1));};
    const std::string_view names[]{"particle.enemy.ranged.impact","particle.boss.volley.projectile_impact","particle.skill.charged_shot.impact"};
    const std::string_view shapes[]{"three_prong_bite_sdf","boss_crest_contact_sdf","needle_star_sdf"};
    const hs::VfxMotionKind motions[]{hs::VfxMotionKind::CompressSnapOutward,hs::VfxMotionKind::SingleFrameSnapShrink,hs::VfxMotionKind::InstantCompressionAxialSplit};
    for(std::uint32_t n=0;n<3;++n)
    {
        c.effect_lookup={{id(names[n]),1}};c.outputs[0].shape=Hash32(shapes[n]);c.outputs[0].motion=motions[n];
        auto out=run();Require(out.size()==1&&static_cast<std::uint32_t>(out[0].shape)==13+n&&Near(out[0].size,.27f)&&
            Near(out[0].direction.x,.6f)&&Near(out[0].direction.z,.8f)&&out[0].position.x==7&&out[0].position.z==-2&&out[0].curve_row==0,
            "projectile contact lost exact recipe, velocity, radius or world contact point");
    }
    c.outputs[0].parameters.count=0;Require(run().empty(),"contact accepted missing scale curve");c.outputs[0].parameters.count=1;
    c.effect_lookup[0].effect_id=id("particle.enemy.ranged.impact");Require(run().empty(),"contact accepted recipe identity mismatch");c.effect_lookup[0].effect_id=id(names[2]);
    std::get<hs::VfxProjectilePayload>(input.payload).velocity={};Require(run().empty(),"contact invented direction for stopped projectile");input.payload=projectile;
    std::get<hs::VfxProjectilePayload>(input.payload).hitbox_radius=-1;Require(run().empty(),"contact accepted invalid projectile radius");input.payload=projectile;
    input.world_transform[12]=std::numeric_limits<float>::infinity();Require(run().empty(),"contact accepted invalid world position");
    input=HitInput();c.effects[0].payload_kind=Hash32("PointEventPayload");Require(run().empty(),"projectile contact coerced Point input");
}

void CircleAdditiveImpactRecipes()
{
    auto c=Program();auto input=HitInput();c.effects[0].payload_kind=Hash32("CircleAreaPayload");
    hs::VfxCirclePayload circle;circle.center={3,.025f,-4};circle.direction={1,0,0};circle.radius=4.75f;input.payload=circle;
    const auto run=[&]{return hs::runtime_detail::BuildVfxTypedFlashCommands(c,std::span(&input,1));};
    auto result=run();Require(result.size()==1&&result[0].shape==hs::VfxImpactShape::SoftDisc&&
        Near(result[0].size,4.75f)&&!result[0].oit,"circle contact flash ignored gameplay radius");
    Require(result[0].position.x==3&&result[0].position.z==-4&&result[0].direction.x==1&&
        Near(result[0].lifetime,.0504f)&&result[0].stable_seed==input.stable_seed,
        "circle contact geometry, source window or identity changed");
    c.outputs[0].profile=hs::VfxOutputProfile::SpriteSdfAdd;
    c.outputs[0].shape=Hash32("soft_disc_noise");c.outputs[0].motion=hs::VfxMotionKind::InstantExpansionCollapse;
    c.parameters={{Hash32("noise"),hs::VfxParameterType::Enum,Hash32("fbm")},
        {Hash32("scale_curve_ref"),hs::VfxParameterType::CurveRow,0}};c.outputs[0].parameters={0,2};
    c.effects[0].seconds=1.05f;c.sources[0].knots={0,.3f,0,0};
    result=run();Require(result.size()==1&&Near(result[0].size,4.75f)&&Near(result[0].lifetime,.315f)&&
        result[0].curve_row==0&&result[0].ground_base_anchor,"circle explosion curve, radius, ground anchor or duration lost");
    c.parameters[0].bits=Hash32("3d_fbm");Require(run().empty(),"circle explosion accepted unsupported noise");c.parameters[0].bits=Hash32("fbm");
    c.outputs[0].shape=Hash32("status_symbol_sdf");c.outputs[0].motion=hs::VfxMotionKind::FastStampBreakout;
    c.parameters={{Hash32("scale_curve_ref"),hs::VfxParameterType::CurveRow,4}};c.outputs[0].parameters={0,1};
    result=run();Require(result.size()==1&&Near(result[0].size,4.75f)&&result[0].curve_row==4&&!result[0].ground_base_anchor,"circle stamp did not retain centered anchor, supplied radius and curve");
    c.parameters[0].type=hs::VfxParameterType::Int;Require(run().empty(),"circle stamp accepted invalid curve type");
    c.outputs[0].shape=Hash32("directional_slash_sdf");c.outputs[0].motion=hs::VfxMotionKind::TwoFrameSnapPerpendicular;
    c.parameters={{Hash32("sdf"),hs::VfxParameterType::Enum,Hash32("cross_slash")},
        {Hash32("aspect"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(2.4f)}};c.outputs[0].parameters={0,2};
    result=run();Require(result.size()==1&&Near(result[0].size,4.75f)&&Near(result[0].aspect,2.4f),"circle slash aspect or radius changed");
    c.outputs[0].min_quality=1;input.quality=hs::VfxQuality::Low;Require(run().empty(),"circle additive quality ignored");input.quality=hs::VfxQuality::High;
    c.outputs[0].motion=hs::VfxMotionKind::InstantExpansionCollapse;Require(run().empty(),"circle accepted mismatched shape/motion");c.outputs[0].motion=hs::VfxMotionKind::TwoFrameSnapPerpendicular;
    std::get<hs::VfxCirclePayload>(input.payload).inner_radius=1;Require(run().empty(),"circle additive coerced annular payload");
    input.payload=circle;std::get<hs::VfxCirclePayload>(input.payload).radius=std::numeric_limits<float>::infinity();Require(run().empty(),"circle infinite radius accepted");
    input.payload=hs::VfxRingGapsPayload{};Require(run().empty(),"ring gaps coerced to additive circle");
    c.effects[0].payload_kind=Hash32("PointEventPayload");input=HitInput();
    result=run();Require(result.size()==1&&Near(result[0].size,.22f),"circle support changed Point cosmetic sizing");
}

void MuzzleConeFlashRecipes()
{
    auto cooked = SecondaryFanProgram();
    auto input = SecondaryFanInput();
    const auto run = [&] {
        return hs::runtime_detail::BuildVfxTypedFlashCommands(cooked, std::span(&input, 1));
    };

    struct Case { const char *effect_id; float seconds; };
    constexpr Case cases[]{
        {"particle.skill.multishot", 0.50f},
        {"particle.upgrade.multishot.secondary_fan", 0.50f},
        {"particle.upgrade.multishot.rear_fan", 0.48f},
        {"particle.upgrade.retreat.triple_release", 0.45f},
        {"particle.upgrade.arrow.triple_release", 0.45f}
    };
    for (const auto &test : cases)
    {
        cooked.effect_lookup[0].effect_id = Hash64(test.effect_id);
        cooked.effects.back().seconds = test.seconds;
        const auto result = run();
        Require(result.size() == 1 && result[0].shape == hs::VfxImpactShape::RadialSlash &&
                Near(result[0].size, 0.30f) && Near(result[0].direction.x, 0.6f) &&
                Near(result[0].direction.z, 0.8f) && result[0].position.x == 2.0f &&
                result[0].position.y == 0.25f && result[0].position.z == -4.0f &&
                result[0].curve_row == 2 && result[0].mask_slice == 5 &&
                Near(result[0].mask_strength, 0.48f) && Near(result[0].color.x, 1.0f) &&
                Near(result[0].color.y, 2.0f) && Near(result[0].color.z, 3.0f) &&
                Near(result[0].color.w, 0.9f) && Near(result[0].delay, 0.0f) &&
                Near(result[0].lifetime, test.seconds * 0.35f),
                "muzzle Cone flash did not preserve authored output tuple");
    }

    cooked.effect_lookup[0].effect_id = Hash64("particle.skill.piercing_shot");
    Require(run().empty(), "unrelated Cone effect identity was accepted by flash decoder");
    cooked.effect_lookup[0].effect_id = Hash64("particle.upgrade.multishot.secondary_fan");

    auto malformed = input;
    auto &cone = std::get<hs::VfxConePayload>(malformed.payload);
    cone.range = 0.0f;
    Require(hs::runtime_detail::BuildVfxTypedFlashCommands(
                cooked, std::span(&malformed, 1)).empty(),
            "zero Cone range was accepted by flash decoder");
    cone = std::get<hs::VfxConePayload>(input.payload);
    cone.half_angle_degrees = 90.0f;
    Require(hs::runtime_detail::BuildVfxTypedFlashCommands(
                cooked, std::span(&malformed, 1)).empty(),
            "wide Cone half angle was accepted by flash decoder");
    cone = std::get<hs::VfxConePayload>(input.payload);
    cone.direction = {};
    Require(hs::runtime_detail::BuildVfxTypedFlashCommands(
                cooked, std::span(&malformed, 1)).empty(),
            "zero Cone direction was accepted by flash decoder");
    cone = std::get<hs::VfxConePayload>(input.payload);
    cone.origin.x = std::numeric_limits<float>::infinity();
    Require(hs::runtime_detail::BuildVfxTypedFlashCommands(
                cooked, std::span(&malformed, 1)).empty(),
            "nonfinite Cone origin was accepted by flash decoder");

    cooked.outputs[2].profile = hs::VfxOutputProfile::SpriteAdd;
    Require(run().empty(), "muzzle Cone accepted a non SpriteSdfAdd output");
    cooked.outputs[2].profile = hs::VfxOutputProfile::SpriteSdfAdd;
    cooked.outputs[2].motion = hs::VfxMotionKind::InstantPointFlash;
    Require(run().empty(), "muzzle Cone accepted a non outward motion");
    cooked.outputs[2].motion = hs::VfxMotionKind::SnapsOutwardAim;
    cooked.sources[2].stable_id = Hash32("wrong_source");
    Require(run().empty(), "muzzle Cone accepted a non muzzle_primary source");
    cooked.sources[2].stable_id = Hash32("muzzle_primary");

    cooked.outputs[2].source = 3;
    Require(run().empty(), "muzzle Cone accepted an output/source tuple mismatch");
    cooked.outputs[2].source = 2;

    cooked.effects.back().payload_kind = Hash32("PointEventPayload");
    Require(run().empty(), "muzzle Cone accepted a wrong effect payload schema");
    cooked.effects.back().payload_kind = Hash32("ConePayload");
    input.payload = hs::VfxPointPayload{};
    Require(run().empty(), "muzzle effect accepted a wrong input payload");
}

void BossEnergyConeAndRingFlashRecipes()
{
    auto cone_cooked = BossEnergyProgram(Hash64("particle.boss.volley.release"),
                                         Hash32("ConePayload"), 0.58f);
    auto cone_input = BossEnergyInput(false);
    const auto run_cone = [&] {
        return hs::runtime_detail::BuildVfxTypedFlashCommands(
            cone_cooked, std::span(&cone_input, 1));
    };
    auto result = run_cone();
    Require(result.size() == 1 && result[0].shape == hs::VfxImpactShape::RadialEnergy &&
            result[0].oit && !result[0].ground_base_anchor && Near(result[0].size, 0.4f) &&
            Near(result[0].direction.x, 0.6f) && Near(result[0].direction.z, 0.8f) &&
            result[0].position.x == 2.0f && result[0].position.z == -4.0f &&
            result[0].fbm_octaves == 4 && result[0].mask_slice == 5 &&
            Near(result[0].mask_strength, 0.28f) && Near(result[0].color.x, 0.6f) &&
            Near(result[0].color.y, 1.2f) && Near(result[0].color.z, 1.8f) &&
            Near(result[0].color.w, 0.65f) && Near(result[0].delay, 0.0f) &&
            Near(result[0].lifetime, 0.464f),
            "boss volley Cone energy did not preserve authored local flash");

    cone_cooked.effect_lookup[0].effect_id = Hash64("particle.boss.shockwave.release");
    Require(run_cone().empty(), "shockwave identity was accepted for the boss volley Cone");
    cone_cooked.effect_lookup[0].effect_id = Hash64("particle.boss.volley.release");
    cone_cooked.sources[2].stable_id = Hash32("wrong_source");
    Require(run_cone().empty(), "boss volley Cone accepted a non boss_energy source");
    cone_cooked.sources[2].stable_id = Hash32("boss_energy");
    cone_cooked.outputs[2].source = 3;
    Require(run_cone().empty(), "boss volley Cone accepted an output/source mismatch");
    cone_cooked.outputs[2].source = 2;
    cone_cooked.outputs[2].motion = hs::VfxMotionKind::RadialLobesUpward;
    Require(run_cone().empty(), "boss volley Cone accepted a non turbulence output");
    cone_cooked.outputs[2].motion = hs::VfxMotionKind::DirectionalTurbulence;
    auto malformed_cone = cone_input;
    std::get<hs::VfxConePayload>(malformed_cone.payload).range = 0.0f;
    Require(hs::runtime_detail::BuildVfxTypedFlashCommands(
                cone_cooked, std::span(&malformed_cone, 1)).empty(),
            "boss volley Cone accepted nonpositive range");
    malformed_cone = cone_input;
    std::get<hs::VfxConePayload>(malformed_cone.payload).half_angle_degrees = 90.0f;
    Require(hs::runtime_detail::BuildVfxTypedFlashCommands(
                cone_cooked, std::span(&malformed_cone, 1)).empty(),
            "boss volley Cone accepted an invalid half angle");
    cone_input.payload = BossEnergyInput(true).payload;
    Require(run_cone().empty(), "boss volley Cone coerced a RingWithGaps payload");

    auto ring_cooked = BossEnergyProgram(Hash64("particle.boss.shockwave.release"),
                                         Hash32("RingWithGapsPayload"), 0.7f);
    auto ring_input = BossEnergyInput(true);
    const auto run_ring = [&] {
        return hs::runtime_detail::BuildVfxTypedFlashCommands(
            ring_cooked, std::span(&ring_input, 1));
    };
    result = run_ring();
    Require(result.size() == 1 && result[0].shape == hs::VfxImpactShape::RadialEnergy &&
            result[0].oit && !result[0].ground_base_anchor && Near(result[0].size, 0.4f) &&
            result[0].position.x == -5.0f && result[0].position.z == 7.0f &&
            result[0].direction.x == 0.0f && result[0].direction.z == 1.0f &&
            result[0].fbm_octaves == 4 && result[0].mask_slice == 5 &&
            Near(result[0].mask_strength, 0.28f) && Near(result[0].color.x, 0.6f) &&
            Near(result[0].color.y, 1.2f) && Near(result[0].color.z, 1.8f) &&
            Near(result[0].color.w, 0.65f) && Near(result[0].delay, 0.0f) &&
            Near(result[0].lifetime, 0.56f),
            "boss shockwave RingWithGaps energy did not preserve authored local flash");

    ring_cooked.effect_lookup[0].effect_id = Hash64("particle.boss.volley.release");
    Require(run_ring().empty(), "volley identity was accepted for the boss shockwave ring");
    ring_cooked.effect_lookup[0].effect_id = Hash64("particle.boss.shockwave.release");
    ring_cooked.sources[2].stable_id = Hash32("wrong_source");
    Require(run_ring().empty(), "boss shockwave ring accepted a non boss_energy source");
    ring_cooked.sources[2].stable_id = Hash32("boss_energy");
    ring_cooked.outputs[2].profile = hs::VfxOutputProfile::SpriteAdd;
    Require(run_ring().empty(), "boss shockwave ring accepted a non noise OIT output");
    ring_cooked.outputs[2].profile = hs::VfxOutputProfile::SpriteNoiseOit;
    auto malformed_ring = ring_input;
    std::get<hs::VfxRingGapsPayload>(malformed_ring.payload).outer_radius = 0.0f;
    Require(hs::runtime_detail::BuildVfxTypedFlashCommands(
                ring_cooked, std::span(&malformed_ring, 1)).empty(),
            "boss shockwave ring accepted nonpositive outer radius");
    malformed_ring = ring_input;
    std::get<hs::VfxRingGapsPayload>(malformed_ring.payload).gap_angles_degrees[0] =
        std::numeric_limits<float>::infinity();
    Require(hs::runtime_detail::BuildVfxTypedFlashCommands(
                ring_cooked, std::span(&malformed_ring, 1)).empty(),
            "boss shockwave ring accepted a nonfinite gap angle");
    ring_input.payload = BossEnergyInput(false).payload;
    Require(run_ring().empty(), "boss shockwave ring coerced a Cone payload");
}

void AuthoredOitRecipes()
{
    auto c=Program();auto input=HitInput();
    auto scalar=[](std::string_view name,float v){return hs::VfxParameterRecord{Hash32(name),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(v)};};
    auto run=[&]{return hs::runtime_detail::BuildVfxTypedFlashCommands(c,std::span(&input,1));};
    c.outputs[0].shape=Hash32("warped_flame_disc");c.outputs[0].motion=hs::VfxMotionKind::RadialLobesUpward;
    c.outputs[0].profile=hs::VfxOutputProfile::SpriteFlameOit;
    c.parameters={{Hash32("fbm_octaves"),hs::VfxParameterType::Int,4},scalar("domain_warp",.22f)};
    c.outputs[0].parameters={0,2};c.sources[0].knots={.02f,.65f,0,0};
    auto flame=run();Require(flame.size()==1&&flame[0].oit&&flame[0].shape==hs::VfxImpactShape::Flame&&
        !flame[0].ground_base_anchor&&Near(flame[0].size,.45f)&&Near(flame[0].domain_warp,.22f)&&flame[0].fbm_octaves==4&&
        Near(flame[0].delay,.0056f)&&Near(flame[0].lifetime,.1764f),"flame recipe parameters or source window lost");
    c.effects[0].payload_kind=Hash32("CircleAreaPayload");hs::VfxCirclePayload circle;circle.center={1,0,2};circle.radius=4.2f;input.payload=circle;
    Require(Near(run()[0].size,4.2f)&&run()[0].ground_base_anchor,"circle flame did not preserve actual radius and ground support");
    input.payload=hs::VfxConePayload{};Require(run().empty(),"cone coerced to OIT point");input=HitInput();
    c.effects[0].payload_kind=Hash32("PointEventPayload");c.outputs[0].shape=Hash32("radial_energy");
    c.outputs[0].motion=hs::VfxMotionKind::DirectionalTurbulence;c.outputs[0].profile=hs::VfxOutputProfile::SpriteNoiseOit;c.outputs[0].parameters.count=1;
    Require(run().size()==1&&Near(run()[0].size,.4f),"radial energy exact recipe rejected");
    c.outputs[0].motion=hs::VfxMotionKind::RadialLobesUpward;Require(run().empty(),"mismatched energy motion accepted");
    c.outputs[0].shape=Hash32("smoke_billboard_6way");c.outputs[0].motion=hs::VfxMotionKind::BakedSixWayPlume;
    c.outputs[0].profile=hs::VfxOutputProfile::SpriteSmoke6WayOit;c.outputs[0].coverage_type=Hash32("flipbook_6way");c.outputs[0].min_quality=1;
    c.parameters={{Hash32("clip_index"),hs::VfxParameterType::Int,1},scalar("frame_rate",20),
        {Hash32("motion_blend"),hs::VfxParameterType::Enum,Hash32("high_only")},scalar("motion_blend_strength",.85f),scalar("opacity_scale",1.35f)};
    c.outputs[0].parameters={0,5};
    const auto texture=[&](std::string_view path,std::uint32_t slot,bool motion){
        const auto first=static_cast<std::uint32_t>(c.strings.size());for(char ch:path)c.strings.push_back(static_cast<std::byte>(ch));
        c.texture_resources.push_back({slot,{first,static_cast<std::uint32_t>(path.size())}});
        hs::VfxTextureBindingRecord t;t.catalog_slot=slot;t.role=Hash32(motion?"flipbook_motion_vectors":"coverage");
        t.min_quality=motion?2:0;if(!motion){t.first_frame=32;t.frame_count=32;t.fps=20;}c.texture_bindings.push_back(t);
    };
    texture("Content/Textures/VFX/vfx_smoke6way_pos.dds",1,false);texture("Content/Textures/VFX/vfx_smoke6way_neg.dds",2,false);
    texture("Content/Textures/VFX/vfx_smoke_motion_vectors.dds",3,true);c.outputs[0].textures={0,3};
    auto smoke=run();Require(smoke.size()==1&&smoke[0].oit&&smoke[0].smoke_first_frame==32&&smoke[0].smoke_frame_count==32&&
        Near(smoke[0].smoke_fps,20)&&Near(smoke[0].motion_strength,.85f)&&Near(smoke[0].opacity_scale,1.35f)&&Near(smoke[0].size,.65f),"authored smoke clip or MV binding lost");
    input.quality=hs::VfxQuality::Medium;Require(run().size()==1&&run()[0].motion_strength==0,"medium smoke used high-only MV");
    input.quality=hs::VfxQuality::Low;Require(run().empty(),"smoke min quality ignored");input.quality=hs::VfxQuality::High;
    c.texture_bindings[1].first_frame=0;Require(run().empty(),"inconsistent positive/negative smoke clips accepted");c.texture_bindings[1].first_frame=32;
    c.texture_bindings[0].frame_count=33;Require(run().empty(),"smoke clip exceeded texture slices");c.texture_bindings[0].frame_count=32;
    c.texture_bindings[2].min_quality=1;Require(run().empty(),"MV binding weakened high-only quality");c.texture_bindings[2].min_quality=2;
    c.parameters[0].bits=0;Require(run().empty(),"clip parameter mismatched coverage clip");c.parameters[0].bits=1;
    c.sources[0].parameters={0,1};Require(run().empty(),"unsupported ImpactSprite source parameters ignored");
}

hs::VfxProgramData StatusTickProgram()
{
    hs::VfxProgramData cooked;
    cooked.effects.resize(4);
    for (std::uint32_t index = 0; index < 4; ++index)
    {
        auto &effect = cooked.effects[index];
        effect.handle = index + 1;
        effect.input_mode = index < 2 ? 0 : 1;
        effect.timing_kind = index < 2 ? 0 : 1;
        effect.payload_kind = Hash32(index < 2 ? "PointEventPayload" : "EntityAttachmentPayload");
        effect.seconds = index == 0 ? .32f : index == 1 ? .34f : 0.0f;
        effect.sources = index < 2 ? hs::VfxRange{} : hs::VfxRange{index - 2, 1};
    }
    cooked.effect_lookup = {
        {Hash64("particle.status.bleed_tick"), 1},
        {Hash64("particle.status.burn_tick"), 2},
        {Hash64("persistent.status.bleed"), 3},
        {Hash64("persistent.status.burn"), 4},
    };
    cooked.curves.resize(7);
    cooked.curve_keys = {{0.0f, .5f}, {.25f, 1.0f}, {1.0f, .7f}};
    cooked.curves[6] = {2, Hash32("normalized_age"), {0, 3}};
    cooked.parameters = {{Hash32("scale_curve_ref"), hs::VfxParameterType::CurveRow, 6}};
    cooked.slice_indices = {3, 4, 5};
    const auto add_texture = [&](std::uint32_t slot, std::string_view path,
                                 std::string_view role) {
        const auto first = static_cast<std::uint32_t>(cooked.strings.size());
        for (const char ch : path) cooked.strings.push_back(static_cast<std::byte>(ch));
        cooked.texture_resources.push_back({slot, {first, static_cast<std::uint32_t>(path.size())}});
        hs::VfxTextureBindingRecord binding;
        binding.catalog_slot = slot;
        binding.role = Hash32(role);
        cooked.texture_bindings.push_back(binding);
    };
    add_texture(1, "Content/Textures/VFX/vfx_gradient_lut.dds", "profile.global");
    add_texture(2, "Content/Textures/VFX/vfx_curve_lut.dds", "profile.global");
    add_texture(3, "Content/Textures/VFX/vfx_authored_mask_array.dds", "authored_mask_array");
    auto &mask = cooked.texture_bindings.back();
    mask.selection = Hash32("stable_seed_mod_group_size");
    mask.strength = .38f;
    mask.slices = {0, 3};
    add_texture(4, "Content/Textures/VFX/vfx_gradient_lut.dds", "profile.global");
    add_texture(5, "Content/Textures/VFX/vfx_curve_lut.dds", "profile.global");
    for (std::uint32_t index = 0; index < 2; ++index)
    {
        hs::VfxSourceRecord source;
        source.effect = index + 3;
        source.stable_id = Hash32("tick_core");
        source.type = hs::VfxSourceType::ImpactSprite;
        source.authoritative = 1;
        source.knot_count = 2;
        source.knots = {0.0f, .5f, 0.0f, 0.0f};
        source.outputs = {index, 1};
        cooked.sources.push_back(source);
        hs::VfxOutputRecord output;
        output.source = index;
        output.profile = hs::VfxOutputProfile::SpriteSdfAdd;
        output.shape = Hash32("small_pulse");
        output.shape_domain = Hash32("procedural_sprite");
        output.shape_scale_rule = Hash32("component_size_or_bound_radius");
        output.shape_component_kind = Hash32("particle");
        output.coverage_type = Hash32("analytic");
        output.coverage_ref = Hash32("small_pulse");
        output.motion = hs::VfxMotionKind::SingleShortPulse;
        output.parameters = {0, 1};
        output.textures = {index == 0 ? 0u : 3u, index == 0 ? 3u : 2u};
        output.rgba = index == 0 ? std::array{.8f, .1f, .05f, .75f}
                                 : std::array{1.0f, .4f, .05f, .75f};
        output.hdr = 2.4f;
        output.gradient_row = index == 0 ? 10 : 11;
        cooked.outputs.push_back(output);
    }
    return cooked;
}

hs::VfxEventInput StatusTickEvent(bool bleed)
{
    hs::VfxEventInput event;
    event.effect_handle = bleed ? 1 : 2;
    event.geometry_owner_id = 17;
    event.status_episode_generation = bleed ? 1001 : 2002;
    event.event_tick = 73;
    event.sequence = bleed ? 401 : 402;
    event.stable_seed = 5;
    hs::VfxPointPayload point;
    point.position = {99.0f, 99.0f, 99.0f};
    event.payload = point;
    return event;
}

hs::VfxPersistentInput StatusTickOwner(bool bleed)
{
    hs::VfxPersistentInput owner;
    owner.effect_handle = bleed ? 3 : 4;
    owner.stable_id = 91;
    owner.status_episode_generation = bleed ? 1001 : 2002;
    owner.current_transform[12] = 3.0f;
    owner.current_transform[13] = 1.0f;
    owner.current_transform[14] = 4.0f;
    owner.elapsed_seconds = 2.0f;
    owner.normalized_age = .75f;
    hs::VfxEntityPayload entity;
    entity.render_instance_id = (2ull << 60) | 17;
    entity.footprint_radius = .73f;
    entity.lifetime01 = owner.normalized_age;
    owner.payload = entity;
    return owner;
}

void OwnedStatusTickPulseUsesLiveOwnerAndEventClock()
{
    const auto cooked = StatusTickProgram();
    const std::array events{StatusTickEvent(true), StatusTickEvent(false)};
    const std::array owners{StatusTickOwner(true), StatusTickOwner(false)};
    const auto commands = hs::runtime_detail::BuildVfxOwnedStatusTickFlashCommands(cooked, events, owners);
    Require(commands.size() == 2, "live bleed and burn ticks were not paired");
    const auto &bleed = commands[0].flash;
    const auto &burn = commands[1].flash;
    Require(commands[0].event_sequence == 401 && commands[1].event_sequence == 402 &&
            bleed.event_tick == 73 && burn.event_tick == 73 && bleed.stable_seed == 5 &&
            burn.stable_seed == 5, "converted tick lost event identity");
    Require(bleed.shape == hs::VfxImpactShape::Pulse && burn.shape == hs::VfxImpactShape::Pulse &&
            Near(bleed.position.x, 3.0f) && Near(bleed.position.y, 1.0f) &&
            Near(burn.position.z, 4.0f) && Near(bleed.size, .73f) && Near(burn.size, .73f),
            "status tick ignored current owner position or footprint");
    Require(Near(bleed.delay, 0.0f) && Near(bleed.lifetime, .16f) &&
            Near(burn.lifetime, .17f) && bleed.curve_row == 6 && burn.curve_row == 6 &&
            bleed.mask_slice == 5 && Near(bleed.mask_strength, .38f) &&
            burn.mask_slice == 0xffffffffu && Near(bleed.color.x, 1.92f),
            "status pulse lost fixed timing or authored curve/mask/color");
}

void OwnedStatusTickRejectsMissingOrWrongOwner()
{
    const auto cooked = StatusTickProgram();
    auto event = StatusTickEvent(true);
    auto owner = StatusTickOwner(true);
    const auto run = [&] { return hs::runtime_detail::BuildVfxOwnedStatusTickFlashCommands(
        cooked, std::span(&event, 1), std::span(&owner, 1)); };
    Require(event.status_episode_generation == owner.status_episode_generation &&
            run().size() == 1, "matching status episode rejected");
    owner.status_episode_generation = 1002;
    Require(run().empty(), "tick paired to another status episode");
    owner.status_episode_generation = 0;
    Require(run().empty(), "tick paired to owner with missing episode");
    owner.status_episode_generation = 1001;
    event.status_episode_generation = 0;
    Require(run().empty(), "tick with missing episode paired to owner");
    event.status_episode_generation = 1001;
    owner.effect_handle = 4;
    Require(run().empty(), "bleed tick paired to burn status");
    owner.effect_handle = 3;
    std::get<hs::VfxEntityPayload>(owner.payload).render_instance_id = (2ull << 60) | 18;
    Require(run().empty(), "tick paired to wrong entity");
    std::get<hs::VfxEntityPayload>(owner.payload).render_instance_id = (2ull << 60) | 17;
    owner.normalized_age = 1.0f;
    Require(run().empty(), "expired status emitted tick pulse");
    owner.normalized_age = .75f;
    event.geometry_owner_id = 0;
    Require(run().empty(), "unowned status tick emitted pulse");
    event.geometry_owner_id = 17;
    std::get<hs::VfxEntityPayload>(owner.payload).lifetime01 = .5f;
    Require(run().empty(), "inconsistent live status age accepted");
}

void OwnedStatusTickRejectsMalformedRecipe()
{
    const auto event = StatusTickEvent(true);
    const auto owner = StatusTickOwner(true);
    const auto run = [&](const hs::VfxProgramData &cooked) {
        return hs::runtime_detail::BuildVfxOwnedStatusTickFlashCommands(
            cooked, std::span(&event, 1), std::span(&owner, 1));
    };
    auto cooked = StatusTickProgram();
    cooked.sources[0].authoritative = 0;
    Require(run(cooked).empty(), "non-authoritative status source accepted");
    cooked = StatusTickProgram();
    cooked.sources[0].knots[1] = .6f;
    Require(run(cooked).empty(), "wrong persistent pulse window accepted");
    cooked = StatusTickProgram();
    cooked.outputs[0].profile = hs::VfxOutputProfile::SpriteAdd;
    Require(run(cooked).empty(), "wrong status output profile accepted");
    cooked = StatusTickProgram();
    cooked.outputs[0].shape = Hash32("soft_disc");
    Require(run(cooked).empty(), "wrong status shape accepted");
    cooked = StatusTickProgram();
    cooked.outputs[0].motion = hs::VfxMotionKind::InstantPointFlash;
    Require(run(cooked).empty(), "wrong status motion accepted");
    cooked = StatusTickProgram();
    cooked.parameters[0].bits = 5;
    Require(run(cooked).empty(), "wrong status curve accepted");
    cooked = StatusTickProgram();
    cooked.curves[6].domain = Hash32("seconds");
    Require(run(cooked).empty(), "malformed pulse curve accepted");
    cooked = StatusTickProgram();
    cooked.texture_bindings[2].selection = 0;
    Require(run(cooked).empty(), "malformed bleed mask accepted");
    cooked = StatusTickProgram();
    cooked.slice_indices[1] = 6;
    Require(run(cooked).empty(), "wrong bleed mask slice group accepted");
}
hs::VfxProgramData PersistentStampProgram(std::string_view name, bool fixed,
                                          std::uint32_t payload_kind = Hash32("EntityAttachmentPayload"),
                                          float fixed_seconds = .55f)
{
    hs::VfxProgramData p;
    hs::VfxEffectRecord effect;
    effect.handle = 1;
    effect.input_mode = 1;
    effect.timing_kind = fixed ? 0 : 1;
    effect.seconds = fixed ? fixed_seconds : 0.0f;
    effect.payload_kind = payload_kind;
    effect.sources = {0, 1};
    p.effects.push_back(effect);
    p.effect_lookup.push_back({Hash64(name), 1});
    hs::VfxSourceRecord source;
    source.effect = 1;
    source.stable_id = Hash32("status_stamp");
    source.type = hs::VfxSourceType::ImpactSprite;
    source.authoritative = 1;
    source.knot_count = 2;
    source.knots = {0, .55f, 0, 0};
    source.outputs = {0, 1};
    p.sources.push_back(source);
    p.curves.resize(5);
    p.curve_keys = {{0, 1.4f}, {.22f, .85f}, {.48f, 1}, {1, 1}};
    p.curves[4] = {2, Hash32("normalized_age"), {0, 4}};
    p.parameters = {{Hash32("scale_curve_ref"), hs::VfxParameterType::CurveRow, 4}};
    const auto texture = [&](std::string_view path, std::uint32_t slot,
                             std::uint32_t role) {
        const auto first = static_cast<std::uint32_t>(p.strings.size());
        for (char ch : path) p.strings.push_back(static_cast<std::byte>(ch));
        p.texture_resources.push_back({slot, {first, static_cast<std::uint32_t>(path.size())}});
        hs::VfxTextureBindingRecord binding;
        binding.role = role;
        binding.catalog_slot = slot;
        p.texture_bindings.push_back(binding);
    };
    texture("Content/Textures/VFX/vfx_gradient_lut.dds", 1, Hash32("profile.global"));
    texture("Content/Textures/VFX/vfx_curve_lut.dds", 2, Hash32("profile.global"));
    const bool bleed_extend = name == "particle.upgrade.ricochet.bleed_extend";
    if (bleed_extend)
    {
        texture("Content/Textures/VFX/vfx_authored_mask_array.dds", 3,
                Hash32("profile.optional_detail"));
        texture("Content/Textures/VFX/vfx_authored_mask_array.dds", 4,
                Hash32("authored_mask_array"));
        auto &mask = p.texture_bindings.back();
        mask.selection = Hash32("stable_seed_mod_group_size");
        mask.slices = {0, 3};
        mask.strength = .38f;
        p.slice_indices = {3, 4, 5};
    }
    hs::VfxOutputRecord output;
    output.source = 0;
    output.profile = hs::VfxOutputProfile::SpriteSdfAdd;
    output.shape = Hash32("status_symbol_sdf");
    output.shape_domain = Hash32("sdf");
    output.shape_scale_rule = Hash32("gameplay_geometry_if_bound_else_component_radius");
    output.shape_component_kind = Hash32("particle");
    output.coverage_type = Hash32("analytic");
    output.coverage_ref = output.shape;
    output.motion = hs::VfxMotionKind::FastStampBreakout;
    output.rgba = {.5f, .25f, 1, .9f};
    output.hdr = 3.2f;
    output.gradient_row = 6;
    output.parameters = {0, 1};
    output.textures = {0, bleed_extend ? 4u : 2u};
    p.outputs.push_back(output);
    return p;
}

hs::VfxPersistentInput PersistentStampOwner(hs::PersistentVfxKind kind)
{
    hs::VfxPersistentInput owner;
    owner.effect_handle = 1;
    owner.source_visual_kind = static_cast<std::uint8_t>(kind);
    owner.stable_id = 87;
    owner.stable_seed = 314;
    owner.source_duration_seconds = kind == hs::PersistentVfxKind::ChargedFullReady ? .55f :
        kind == hs::PersistentVfxKind::MultishotRetarget ? .30f :
        kind == hs::PersistentVfxKind::RicochetBleedExtend ? .35f : 2.0f;
    owner.elapsed_seconds = .2f;
    owner.normalized_age = .25f;
    owner.current_transform[12] = 3;
    owner.current_transform[13] = .5f;
    owner.current_transform[14] = 4;
    hs::VfxEntityPayload entity;
    entity.render_instance_id = 90;
    entity.footprint_radius = .7f;
    entity.lifetime01 = owner.normalized_age;
    owner.payload = entity;
    if (kind == hs::PersistentVfxKind::MultishotRetarget)
    {
        hs::VfxProjectilePathPayload path;
        path.current_position = {8, .75f, 9};
        path.previous_position = {7.5f, .75f, 8.5f};
        path.velocity = {3, 0, 3};
        path.projectile_radius = .18f;
        path.lifetime01 = owner.normalized_age;
        owner.current_transform[12] = -40;
        owner.current_transform[14] = -40;
        owner.payload = path;
    }
    return owner;
}

void PersistentStatusStamps()
{
    struct Case { const char *name; hs::PersistentVfxKind kind; bool fixed; };
    constexpr Case cases[]{
        {"particle.player.bow_draw", hs::PersistentVfxKind::PlayerBowDraw, false},
        {"particle.upgrade.charged.full_ready", hs::PersistentVfxKind::ChargedFullReady, true},
        {"particle.upgrade.empowered_ready", hs::PersistentVfxKind::EmpoweredReady, false},
    };
    for (const auto &test : cases)
    {
        auto program = PersistentStampProgram(test.name, test.fixed);
        auto owner = PersistentStampOwner(test.kind);
        const auto run = [&] { return hs::runtime_detail::BuildVfxPersistentStatusStampCommands(
            program, std::span(&owner, 1)); };
        auto commands = run();
        Require(commands.size() == 1 && commands[0].shape == hs::VfxImpactShape::StatusStamp &&
                Near(commands[0].position.x, 3) && Near(commands[0].size, .7f) &&
                Near(commands[0].normalized_age, .25f / .55f) &&
                Near(commands[0].lifetime, owner.source_duration_seconds * .55f) &&
                commands[0].curve_row == 4 && commands[0].stable_seed == 314 &&
                Near(commands[0].color.x, 1.6f) && commands[0].event_tick == 0,
                "persistent stamp did not use live owner and authored source interval");
        owner.quality = hs::VfxQuality::Low;
        Require(run().size() == 1, "low quality lost authored status stamp");
        owner.quality = hs::VfxQuality::High;
        owner.current_transform[12] = 8;
        owner.normalized_age = .5f;
        std::get<hs::VfxEntityPayload>(owner.payload).lifetime01 = .5f;
        commands = run();
        Require(commands.size() == 1 && Near(commands[0].position.x, 8) &&
                Near(commands[0].normalized_age, .5f / .55f),
                "persistent stamp did not follow current owner position and age");
        owner.normalized_age = .55f;
        std::get<hs::VfxEntityPayload>(owner.payload).lifetime01 = .55f;
        Require(run().empty(), "stamp outlived its source knots");
        owner = PersistentStampOwner(test.kind);
        owner.source_duration_seconds = 0;
        Require(run().empty(), "nonpositive source duration accepted");
        owner = PersistentStampOwner(test.kind);
        program.effects[0].input_mode = 0;
        Require(run().empty(), "event mode accepted as persistent status stamp");
        program = PersistentStampProgram(test.name, test.fixed);
        program.effects[0].timing_kind = test.fixed ? 1 : 0;
        Require(run().empty(), "wrong timing kind accepted");
        program = PersistentStampProgram(test.name, test.fixed);
        owner.source_visual_kind = static_cast<std::uint8_t>(hs::PersistentVfxKind::EnemyBurnStatus);
        Require(run().empty(), "unrelated visual kind accepted");
        owner = PersistentStampOwner(test.kind);
        program.effect_lookup[0].effect_id = Hash64("particle.player.arrow_release");
        Require(run().empty(), "wrong effect identity accepted");
        program = PersistentStampProgram(test.name, test.fixed);
        program.outputs[0].motion = hs::VfxMotionKind::InstantPointFlash;
        Require(run().empty(), "wrong authored motion accepted");
        program = PersistentStampProgram(test.name, test.fixed);
        program.texture_bindings[1].catalog_slot = 99;
        Require(run().empty(), "missing shared curve texture accepted");
        program = PersistentStampProgram(test.name, test.fixed);
        owner.payload = hs::VfxPointPayload{};
        Require(run().empty(), "wrong persistent payload accepted");
    }
}

void UpgradeStatusStampsUseAuthoredGeometry()
{
    struct Case { const char *name; hs::PersistentVfxKind kind; float seconds; bool path; };
    constexpr Case cases[]{
        {"particle.upgrade.retarget", hs::PersistentVfxKind::MultishotRetarget, .30f, true},
        {"particle.upgrade.ricochet.bleed_extend", hs::PersistentVfxKind::RicochetBleedExtend, .35f, false},
    };
    for (const auto &test : cases)
    {
        const auto payload = test.path ? Hash32("ProjectilePathPayload") :
            Hash32("EntityAttachmentPayload");
        auto program = PersistentStampProgram(test.name, true, payload, test.seconds);
        auto owner = PersistentStampOwner(test.kind);
        const auto run = [&] { return hs::runtime_detail::BuildVfxPersistentStatusStampCommands(
            program, std::span(&owner, 1)); };
        const auto commands = run();
        Require(commands.size() == 1, "upgrade status stamp was not decoded");
        const auto &command = commands.front();
        const float expected_size = test.path
            ? std::get<hs::VfxProjectilePathPayload>(owner.payload).projectile_radius
            : std::get<hs::VfxEntityPayload>(owner.payload).footprint_radius;
        Require(command.shape == hs::VfxImpactShape::StatusStamp &&
                Near(command.position.x, test.path ? 8.0f : 3.0f) &&
                Near(command.position.z, test.path ? 9.0f : 4.0f) &&
                Near(command.size, expected_size) &&
                Near(command.normalized_age, .25f / .55f) &&
                Near(command.lifetime, test.seconds * .55f) &&
                (test.path || (command.mask_slice == 5 && Near(command.mask_strength, .38f))),
                "upgrade status stamp did not use its authored geometry and fixed timing");
        owner.quality = hs::VfxQuality::Low;
        Require(run().size() == 1, "low quality lost upgrade status stamp");

        owner = PersistentStampOwner(test.kind);
        program = PersistentStampProgram(test.name, true, payload, test.seconds);
        program.outputs[0].shape = Hash32("soft_disc");
        Require(run().empty(), "upgrade status stamp accepted the wrong shape");
        owner = PersistentStampOwner(test.kind);
        program = PersistentStampProgram(test.name, true, payload, test.seconds);
        program.parameters[0].bits = 5;
        Require(run().empty(), "upgrade status stamp accepted the wrong curve");
        owner = PersistentStampOwner(test.kind);
        program = PersistentStampProgram(test.name, true, payload, test.seconds);
        program.texture_bindings[1].catalog_slot = 99;
        Require(run().empty(), "upgrade status stamp accepted a missing curve texture");
        owner = PersistentStampOwner(test.kind);
        owner.quality = hs::VfxQuality::Low;
        program = PersistentStampProgram(test.name, true, payload, test.seconds);
        program.outputs[0].min_quality = 1;
        Require(run().empty(), "upgrade status stamp ignored output quality");

        if (test.path)
        {
            owner = PersistentStampOwner(test.kind);
            owner.payload = hs::VfxEntityPayload{};
            Require(run().empty(), "retarget status stamp accepted an entity payload");
            owner = PersistentStampOwner(test.kind);
            auto &path = std::get<hs::VfxProjectilePathPayload>(owner.payload);
            path.projectile_radius = 0;
            Require(run().empty(), "retarget status stamp accepted an invalid footprint");
            owner = PersistentStampOwner(test.kind);
            program.effects[0].payload_kind = Hash32("EntityAttachmentPayload");
            Require(run().empty(), "retarget status stamp accepted the wrong effect payload");
        }
        else
        {
            owner = PersistentStampOwner(test.kind);
            owner.payload = hs::VfxProjectilePathPayload{};
            Require(run().empty(), "ricochet status stamp accepted a path payload");
            owner = PersistentStampOwner(test.kind);
            program.texture_bindings.back().selection = 0;
            Require(run().empty(), "ricochet status stamp accepted an unselected authored mask");
            owner = PersistentStampOwner(test.kind);
            program.texture_bindings.back().selection = Hash32("stable_seed_mod_group_size");
            program.slice_indices[1] = 6;
            Require(run().empty(), "ricochet status stamp accepted the wrong authored mask group");
        }
    }
}

void ProductionUpgradeStatusStampsDecode()
{
    std::ifstream file("Cooked/vfx_program.hsbin", std::ios::binary);
    Require(file.good(), "production VFX program missing");
    const std::string binary{std::istreambuf_iterator<char>(file),
                             std::istreambuf_iterator<char>()};
    hs::VfxProgramData cooked;
    std::string error;
    Require(hs::LoadVfxProgram(std::as_bytes(std::span(binary)), cooked, error),
            "production VFX program could not be decoded");
    struct Case { const char *name; hs::PersistentVfxKind kind; float seconds; bool path; };
    constexpr Case cases[]{
        {"particle.upgrade.retarget", hs::PersistentVfxKind::MultishotRetarget, .30f, true},
        {"particle.upgrade.ricochet.bleed_extend", hs::PersistentVfxKind::RicochetBleedExtend, .35f, false},
    };
    for (const auto &test : cases)
    {
        const auto lookup = std::find_if(cooked.effect_lookup.begin(), cooked.effect_lookup.end(),
            [&](const auto &entry) { return entry.effect_id == Hash64(test.name); });
        Require(lookup != cooked.effect_lookup.end(), "production upgrade status effect missing");
        auto owner = PersistentStampOwner(test.kind);
        owner.effect_handle = lookup->handle;
        owner.source_duration_seconds = test.seconds;
        owner.elapsed_seconds = .2f * test.seconds;
        owner.normalized_age = .2f;
        if (test.path)
            std::get<hs::VfxProjectilePathPayload>(owner.payload).lifetime01 = owner.normalized_age;
        else
            std::get<hs::VfxEntityPayload>(owner.payload).lifetime01 = owner.normalized_age;
        owner.quality = hs::VfxQuality::Medium;
        const auto commands = hs::runtime_detail::BuildVfxPersistentStatusStampCommands(
            cooked, std::span(&owner, 1));
        Require(commands.size() == 1 && commands[0].shape == hs::VfxImpactShape::StatusStamp &&
                Near(commands[0].size, test.path ? .18f : .7f) &&
                Near(commands[0].position.x, test.path ? 8.0f : 3.0f) &&
                (test.path || (commands[0].mask_slice != 0xffffffffu &&
                               Near(commands[0].mask_strength, .38f))),
                "production upgrade status stamp did not decode authored geometry");
    }
}

void PickupCollectorImpactUsesLinkSource()
{
    constexpr std::array effect_names{
        "particle.pickup.xp_collect", "particle.pickup.heal_collect",
        "particle.pickup.magnet_collect", "particle.pickup.relic_collect"};
    for (const auto name : effect_names)
    {
        hs::VfxProgramData cooked;
        hs::VfxEffectRecord effect;
        effect.handle = 1;
        effect.input_mode = 0;
        effect.payload_kind = Hash32("SourceTargetPayload");
        effect.seconds = .32f;
        effect.sources = {0, 1};
        cooked.effects.push_back(effect);
        cooked.effect_lookup.push_back({Hash64(name), 1});
        hs::VfxSourceRecord source;
        source.effect = 1;
        source.type = hs::VfxSourceType::ImpactSprite;
        source.knot_count = 2;
        source.knots = {0, .25f, 0, 0};
        source.outputs = {0, 1};
        cooked.sources.push_back(source);
        cooked.parameters.push_back({Hash32("sdf"), hs::VfxParameterType::Enum,
                                     Hash32("star_ring")});
        hs::VfxOutputRecord output;
        output.source = 0;
        output.profile = hs::VfxOutputProfile::SpriteSdfAdd;
        output.shape = Hash32("small_star_ring");
        output.motion = hs::VfxMotionKind::SnapsInwardUpward;
        output.rgba = {1, .6f, .2f, .8f};
        output.hdr = 2;
        output.parameters = {0, 1};
        cooked.outputs.push_back(output);
        hs::VfxEventInput input;
        input.effect_handle = 1;
        input.event_tick = 42;
        input.sequence = 12;
        hs::VfxLinkPayload link;
        link.source_position = {3, .4f, 4};
        link.target_position = {7, .4f, 9};
        input.payload = link;
        const auto run = [&] { return hs::runtime_detail::BuildVfxTypedFlashCommands(
            cooked, std::span(&input, 1)); };
        auto commands = run();
        Require(commands.size() == 1 && commands[0].shape == hs::VfxImpactShape::StarRing &&
                Near(commands[0].position.x, 3) && Near(commands[0].position.z, 4) &&
                Near(commands[0].direction.x, 4.0f / std::sqrt(41.0f)) &&
                Near(commands[0].lifetime, .08f),
                "pickup link impact did not start at authoritative pickup position");
        link.target_position = link.source_position;
        input.payload = link;
        Require(run().size() == 1, "coincident collector dropped pickup burst");
        input.sequence = 0;
        Require(run().empty(), "unidentified pickup collector event accepted");
        input.sequence = 12;
        cooked.effect_lookup[0].effect_id = Hash64("particle.player.arrow_release");
        Require(run().empty(), "unrelated link effect accepted as pickup impact");
    }
}

void ProductionStatusAndPickupOutputsDecode()
{
    std::ifstream file("Cooked/vfx_program.hsbin", std::ios::binary);
    Require(file.good(), "production VFX program missing");
    const std::string binary{std::istreambuf_iterator<char>(file),
                             std::istreambuf_iterator<char>()};
    hs::VfxProgramData cooked;
    std::string error;
    Require(hs::LoadVfxProgram(std::as_bytes(std::span(binary)), cooked, error),
            "production VFX program could not be decoded");
    struct Case { const char *name; hs::PersistentVfxKind kind; float duration; };
    constexpr Case cases[]{
        {"particle.player.bow_draw", hs::PersistentVfxKind::PlayerBowDraw, 14.0f / 60.0f},
        {"particle.upgrade.charged.full_ready", hs::PersistentVfxKind::ChargedFullReady, .55f},
        {"particle.upgrade.empowered_ready", hs::PersistentVfxKind::EmpoweredReady, 1.0f},
    };
    for (const auto &test : cases)
    {
        const auto lookup = std::find_if(cooked.effect_lookup.begin(), cooked.effect_lookup.end(),
            [&](const auto &entry) { return entry.effect_id == Hash64(test.name); });
        Require(lookup != cooked.effect_lookup.end(), "production player status effect missing");
        auto owner = PersistentStampOwner(test.kind);
        owner.effect_handle = lookup->handle;
        owner.source_duration_seconds = test.duration;
        owner.elapsed_seconds = .2f * test.duration;
        owner.normalized_age = .2f;
        std::get<hs::VfxEntityPayload>(owner.payload).lifetime01 = owner.normalized_age;
        const auto commands = hs::runtime_detail::BuildVfxPersistentStatusStampCommands(
            cooked, std::span(&owner, 1));
        Require(commands.size() == 1,
                "production player owner did not decode its status stamp");
    }
    constexpr std::array pickup_names{
        "particle.pickup.xp_collect", "particle.pickup.heal_collect",
        "particle.pickup.magnet_collect", "particle.pickup.relic_collect"};
    for (const auto name : pickup_names)
    {
        const auto lookup = std::find_if(cooked.effect_lookup.begin(), cooked.effect_lookup.end(),
            [&](const auto &entry) { return entry.effect_id == Hash64(name); });
        Require(lookup != cooked.effect_lookup.end(), "production pickup effect missing");
        hs::VfxEventInput event;
        event.effect_handle = lookup->handle;
        event.event_tick = 42;
        event.sequence = 12;
        hs::VfxLinkPayload link;
        link.source_position = {3, .3f, 4};
        link.target_position = {7, .3f, 9};
        event.payload = link;
        const auto commands = hs::runtime_detail::BuildVfxTypedFlashCommands(
            cooked, std::span(&event, 1));
        Require(commands.size() == 1,
                "production pickup link did not decode its collection burst");
    }
}
} // namespace

int main()
{
    try
    {
        AuthoredSdfShapesAndContext();
        AuthoredOitRecipes();
        OwnedStatusTickPulseUsesLiveOwnerAndEventClock();
        OwnedStatusTickRejectsMissingOrWrongOwner();
        OwnedStatusTickRejectsMalformedRecipe();
        PersistentStatusStamps();
        UpgradeStatusStampsUseAuthoredGeometry();
        ProductionUpgradeStatusStampsDecode();
        PickupCollectorImpactUsesLinkSource();
        ProductionStatusAndPickupOutputsDecode();
        CircleAdditiveImpactRecipes();
        MuzzleConeFlashRecipes();
        BossEnergyConeAndRingFlashRecipes();
        ProjectileContactRecipes();
        MasksQualityAndMalformedInputs();
        DecodesOnlyAuthoredContactFlash();
        HonorsCookedTimingChanges();
        DecodesEveryMatchingEffectAndRejectsUnsupportedOutput();
        std::cout << "VFX typed flash commands passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
