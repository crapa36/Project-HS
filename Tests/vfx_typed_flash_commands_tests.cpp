#include <hs/renderer/vfx_program_loader.hpp>
#include "../Source/Runtime/Private/vfx_typed_flash_commands.hpp"
#include "../Source/Runtime/Private/vfx_visible_outputs.hpp"
#include <hs/runtime/vfx_image_io.hpp>
#include "test_generation_paths.hpp"
#include <hs/render_data/render_snapshot.hpp>

#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
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

hs::VfxProgramData PersistentStampProgram(std::string_view name, bool fixed,
    std::uint32_t payload_kind = Hash32("EntityAttachmentPayload"), float fixed_seconds = .55f);
hs::VfxPersistentInput PersistentStampOwner(hs::PersistentVfxKind kind);

hs::VfxImageData PreparedImage(const hs::VfxProgramData &program)
{
    hs::VfxImageData image; image.program=program;
    hs::PrepareVfxFlashRecipes(image.program.View(),image.flash_effect_ranges,image.flash_recipes);
    hs::PrepareVfxPersistentFlashRecipes(image.program.View(),
        image.persistent_stamp_effect_ranges,image.persistent_stamp_recipes,
        image.persistent_fire_effect_ranges,image.persistent_fire_recipes,
        image.owned_status_tick_effect_ranges,image.owned_status_tick_recipes);
    return image;
}
std::vector<hs::VfxFlashSpawnInput> BuildFlash(
    const hs::VfxProgramData &program,hs::runtime_detail::VfxEventView inputs,
    std::vector<hs::Sequence> *superseded=nullptr)
{ const auto image=PreparedImage(program); return hs::runtime_detail::BuildVfxTypedFlashCommands(image.View(),inputs,superseded); }
std::vector<hs::VfxFlashSpawnInput> BuildPersistentStamps(
    const hs::VfxProgramData &program, std::span<const hs::VfxPersistentInput> inputs)
{ const auto image=PreparedImage(program); return hs::runtime_detail::BuildVfxPersistentStatusStampCommands(image.View(),inputs); }
std::vector<hs::VfxFlashSpawnInput> BuildPersistentFire(
    const hs::VfxProgramData &program, std::span<const hs::VfxPersistentInput> inputs, double tick)
{ const auto image=PreparedImage(program); return hs::runtime_detail::BuildVfxPersistentFireFlameCommands(image.View(),inputs,tick); }
std::vector<hs::VfxFlashSpawnInput> LegacyPersistentFire(
    const hs::VfxImage &image, std::span<const hs::VfxPersistentInput> inputs, double current_tick)
{
    const auto mix = [](std::uint32_t value) {
        value ^= value >> 16; value *= 0x7feb352du; value ^= value >> 15;
        value *= 0x846ca68bu; return value ^ (value >> 16);
    };
    const auto random = [&](std::uint32_t seed, std::uint32_t lane) {
        return static_cast<float>(mix(seed ^ mix(lane + 0x9e3779b9u)) >> 8) / 16777216.0f;
    };
    std::vector<hs::VfxFlashSpawnInput> output;
    for (const auto &owner : inputs)
    {
        const auto *circle = std::get_if<hs::VfxCirclePayload>(&owner.payload);
        const auto range = image.persistent_fire_effect_ranges[owner.effect_handle - 1];
        const auto elapsed = static_cast<double>(owner.elapsed_seconds) * 60;
        const auto duration = static_cast<std::uint64_t>(std::llround(
            static_cast<double>(owner.source_duration_seconds) * 60));
        const auto started = static_cast<hs::Tick>(std::llround(std::max(0.0, current_tick - elapsed)));
        for (const auto &recipe : image.persistent_fire_recipes.subspan(range.first, range.count))
        {
            if (recipe.effect_handle != owner.effect_handle) continue;
            const auto source_seed = mix(owner.stable_seed ^ mix(recipe.source_stable_id) ^
                mix(static_cast<std::uint32_t>(recipe.effect_id)) ^
                mix(static_cast<std::uint32_t>(recipe.effect_id >> 32)));
            auto base = recipe.spawn;
            base.position = circle->center;
            if (recipe.mask_slices.count && recipe.mask_slices.first <= image.program.slice_indices.size() &&
                recipe.mask_slices.count <= image.program.slice_indices.size() - recipe.mask_slices.first)
            {
                const auto slices = image.program.slice_indices.subspan(
                    recipe.mask_slices.first, recipe.mask_slices.count);
                base.mask_slice = slices[owner.stable_seed % slices.size()];
                base.mask_strength = recipe.mask_strength;
            }
            const auto last = elapsed * recipe.rate / 60;
            for (std::uint64_t ordinal = 0; ordinal <= last; ++ordinal)
            {
                const auto birth = (ordinal * 60 + recipe.rate - 1) / recipe.rate;
                if (birth > elapsed || birth >= duration) continue;
                auto tongue = base;
                tongue.stable_seed = mix(source_seed ^ mix(static_cast<std::uint32_t>(ordinal + 1)));
                const float angle = 2 * std::numbers::pi_v<float> * random(tongue.stable_seed, 0);
                const float radius = std::sqrt(random(tongue.stable_seed, 1)) *
                                     circle->radius * recipe.radius_scale;
                tongue.initial_offset = {std::cos(angle) * radius, 0, std::sin(angle) * radius};
                tongue.lifetime = static_cast<float>(duration - birth) / 60;
                tongue.normalized_age = static_cast<float>(elapsed - birth) /
                                        static_cast<float>(duration - birth);
                tongue.event_tick = started + birth;
                tongue.initial_velocity = {-tongue.initial_offset.x / tongue.lifetime,
                    tongue.size / tongue.lifetime, -tongue.initial_offset.z / tongue.lifetime};
                if (std::isfinite(tongue.initial_velocity.x) &&
                    std::isfinite(tongue.initial_velocity.y) &&
                    std::isfinite(tongue.initial_velocity.z)) output.push_back(tongue);
            }
        }
    }
    return output;
}
std::vector<hs::runtime_detail::VfxOwnedStatusTickFlashCommand> BuildOwnedStatusTicks(
    const hs::VfxProgramData &program, hs::runtime_detail::VfxEventView events,
    std::span<const hs::VfxPersistentInput> inputs)
{ const auto image=PreparedImage(program); return hs::runtime_detail::BuildVfxOwnedStatusTickFlashCommands(image.View(),events,inputs); }
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
    const auto spawns = BuildFlash(cooked, inputs);
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
    const auto spawns = BuildFlash(cooked, inputs);
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
    const auto spawns = BuildFlash(cooked, inputs);
    Require(spawns.size() == 2, "matching contact flashes were not executed once per event");
    Require(Near(spawns[1].lifetime, 0.1f) &&
            Near(spawns[1].color.x, 1.0f) &&
            Near(spawns[1].color.y, 0.5f) &&
            Near(spawns[1].color.z, 0.2f) &&
            Near(spawns[1].color.w, 0.4f) && spawns[1].event_tick == 27,
            "second effect did not preserve its cooked output");
    cooked.outputs[1].profile = hs::VfxOutputProfile::SpriteSdfAdd;
    Require(BuildFlash(cooked, inputs).size() == 1,
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
        auto run = [&] { return BuildFlash(c, std::span(&input, 1)); };
        auto decoded = run();
        Require(decoded.size() == 1 && decoded[0].shape == test.kind && Near(decoded[0].size, test.size * 2), "SDF shape or metre scale missing");
        Require(!decoded[0].interpolate_owner_position,
                "world impact unexpectedly followed an owner pose");
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
    auto run = [&] { return BuildFlash(c, std::span(&input, 1)); };
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
    const auto run=[&]{return BuildFlash(c,std::span(&input,1));};
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
    const auto run=[&]{return BuildFlash(c,std::span(&input,1));};
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
        return BuildFlash(cooked, std::span(&input, 1));
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
    Require(BuildFlash(
                cooked, std::span(&malformed, 1)).empty(),
            "zero Cone range was accepted by flash decoder");
    cone = std::get<hs::VfxConePayload>(input.payload);
    cone.half_angle_degrees = 90.0f;
    Require(BuildFlash(
                cooked, std::span(&malformed, 1)).empty(),
            "wide Cone half angle was accepted by flash decoder");
    cone = std::get<hs::VfxConePayload>(input.payload);
    cone.direction = {};
    Require(BuildFlash(
                cooked, std::span(&malformed, 1)).empty(),
            "zero Cone direction was accepted by flash decoder");
    cone = std::get<hs::VfxConePayload>(input.payload);
    cone.origin.x = std::numeric_limits<float>::infinity();
    Require(BuildFlash(
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
        return BuildFlash(
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
    Require(BuildFlash(
                cone_cooked, std::span(&malformed_cone, 1)).empty(),
            "boss volley Cone accepted nonpositive range");
    malformed_cone = cone_input;
    std::get<hs::VfxConePayload>(malformed_cone.payload).half_angle_degrees = 90.0f;
    Require(BuildFlash(
                cone_cooked, std::span(&malformed_cone, 1)).empty(),
            "boss volley Cone accepted an invalid half angle");
    cone_input.payload = BossEnergyInput(true).payload;
    Require(run_cone().empty(), "boss volley Cone coerced a RingWithGaps payload");

    auto ring_cooked = BossEnergyProgram(Hash64("particle.boss.shockwave.release"),
                                         Hash32("RingWithGapsPayload"), 0.7f);
    auto ring_input = BossEnergyInput(true);
    const auto run_ring = [&] {
        return BuildFlash(
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
    Require(BuildFlash(
                ring_cooked, std::span(&malformed_ring, 1)).empty(),
            "boss shockwave ring accepted nonpositive outer radius");
    malformed_ring = ring_input;
    std::get<hs::VfxRingGapsPayload>(malformed_ring.payload).gap_angles_degrees[0] =
        std::numeric_limits<float>::infinity();
    Require(BuildFlash(
                ring_cooked, std::span(&malformed_ring, 1)).empty(),
            "boss shockwave ring accepted a nonfinite gap angle");
    ring_input.payload = BossEnergyInput(false).payload;
    Require(run_ring().empty(), "boss shockwave ring coerced a Cone payload");
}

void AuthoredOitRecipes()
{
    auto c=Program();auto input=HitInput();
    auto scalar=[](std::string_view name,float v){return hs::VfxParameterRecord{Hash32(name),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(v)};};
    auto run=[&]{return BuildFlash(c,std::span(&input,1));};
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

hs::VfxProgramData PersistentFireProgram()
{
    hs::VfxProgramData program;
    hs::VfxEffectRecord effect;
    effect.handle = 1; effect.input_mode = 1; effect.timing_kind = 1;
    effect.payload_kind = Hash32("CircleAreaPayload"); effect.sources = {0,1};
    program.effects.push_back(effect);
    program.effect_lookup.push_back({Hash64("persistent.area.fire"),1});
    hs::VfxSourceRecord source;
    source.effect = 1; source.stable_id = Hash32("high_quality_flame_tongues");
    source.type = hs::VfxSourceType::CurlMotes; source.knot_count = 2; source.knots = {0,1,0,0};
    source.parameters = {0,3}; source.outputs = {0,1};
    program.sources.push_back(source);
    program.parameters = {
        {Hash32("count_per_second"),hs::VfxParameterType::Int,8},
        {Hash32("spawn_domain"),hs::VfxParameterType::Enum,Hash32("inside_binding_radius")},
        {Hash32("spawn_radius_scale"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(.5f)},
        {Hash32("fbm_octaves"),hs::VfxParameterType::Int,3},
        {Hash32("size"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(.4f)},
        {Hash32("domain_warp"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(.2f)},
        {Hash32("flow_speed"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(.7f)},
    };
    hs::VfxOutputRecord output;
    output.source=0; output.profile=hs::VfxOutputProfile::SpriteFlameOit;
    output.min_quality=2; output.motion=hs::VfxMotionKind::ProceduralFlameTongue;
    output.shape=Hash32("warped_flame_disc"); output.coverage_type=Hash32("analytic");
    output.coverage_ref=Hash32("warped_flame_disc"); output.parameters={3,4};
    output.rgba={1.0f,.5f,.25f,.8f}; output.hdr=2.0f; output.gradient_row=31;
    output.textures={0,1};
    program.outputs.push_back(output);
    const std::string path="Content/Textures/VFX/vfx_authored_mask_array.dds";
    program.strings.resize(path.size());
    std::transform(path.begin(),path.end(),program.strings.begin(),[](char c){return std::byte(c);});
    program.texture_resources.push_back({1,{0,static_cast<std::uint32_t>(path.size())}});
    hs::VfxTextureBindingRecord mask;
    mask.role=Hash32("authored_mask_array"); mask.catalog_slot=1; mask.min_quality=2;
    mask.selection=Hash32("stable_seed_mod_group_size"); mask.strength=.45f; mask.slices={0,2};
    program.texture_bindings.push_back(mask); program.slice_indices={2,4};
    return program;
}

void PersistentFireFlameUsesPreparedRecipeAndLiveAge()
{
    const auto program=PersistentFireProgram();
    hs::VfxPersistentInput owner;
    owner.effect_handle=1; owner.source_visual_kind=static_cast<std::uint8_t>(hs::PersistentVfxKind::FireArea);
    owner.stable_id=91; owner.stable_seed=314; owner.quality=hs::VfxQuality::High;
    owner.elapsed_seconds=.5f; owner.source_duration_seconds=1.0f;
    hs::VfxCirclePayload circle; circle.center={3,1,4}; circle.radius=2;
    owner.payload=circle;
    const std::array owners{owner};
    const auto first=BuildPersistentFire(program,owners,100.0);
    const auto second=BuildPersistentFire(program,owners,100.0);
    Require(first.size()==5 && second.size()==first.size(),
        "prepared persistent fire recipe changed the authored 8 Hz birth schedule");
    constexpr std::array<hs::Tick,5> ticks{70,78,85,93,100};
    const auto mix=[](std::uint32_t v){v^=v>>16;v*=0x7feb352du;v^=v>>15;v*=0x846ca68bu;return v^(v>>16);};
    const auto source_seed=mix(owner.stable_seed ^ mix(Hash32("high_quality_flame_tongues")) ^
        mix(static_cast<std::uint32_t>(Hash64("persistent.area.fire"))) ^
        mix(static_cast<std::uint32_t>(Hash64("persistent.area.fire")>>32)));
    Require(first[0].stable_seed==mix(source_seed ^ mix(1)),
        "prepared fire recipe changed the source and ordinal seed contract");
    for(std::size_t i=0;i<first.size();++i)
        Require(first[i].stable_seed==second[i].stable_seed &&
            Near(first[i].position.x,3) && Near(first[i].position.z,4) &&
            first[i].mask_slice==2 && Near(first[i].mask_strength,.45f) &&
            first[i].gradient_row==31 && first[i].event_tick==ticks[i],
            "prepared fire recipe changed owner seed, mask, live position, or birth timing");
    Require(Near(first.back().normalized_age,0) && Near(first.back().lifetime,.5f),
        "persistent fire did not evaluate its final tongue from live source age");
}

void PreparedPersistentFireCapsNewestBirthsFairly()
{
    const auto program = PersistentFireProgram();
    auto first = PersistentStampOwner(hs::PersistentVfxKind::FireArea);
    first.effect_handle = 1; first.source_visual_kind = static_cast<std::uint8_t>(hs::PersistentVfxKind::FireArea);
    first.stable_id = 91; first.stable_seed = 314; first.quality = hs::VfxQuality::High;
    first.elapsed_seconds = 20.0f; first.source_duration_seconds = 30.0f;
    hs::VfxCirclePayload circle; circle.center = {3, 1, 4}; circle.radius = 2;
    first.payload = circle;
    auto second = first;
    second.stable_id = 92; second.stable_seed = 777;
    std::array owners{first, second};
    auto image = PreparedImage(program);
    const auto image_view = image.View();
    const auto expected = LegacyPersistentFire(image_view, owners, 1300.0);
    const auto compatibility_wrapper = BuildPersistentFire(program, owners, 1300.0);
    Require(expected.size() == 322,
            "long-lived fire fixture did not produce the expected source-order oracle");
    Require(compatibility_wrapper.size() == expected.size() &&
                compatibility_wrapper.front().stable_seed == expected.front().stable_seed &&
                compatibility_wrapper.back().event_tick == expected.back().event_tick,
            "unlimited compatibility wrapper changed legacy fire scheduling");

    std::vector<hs::runtime_detail::VfxPreparedFireSource> prepared;
    hs::runtime_detail::PrepareVfxPersistentFireSources(image_view, owners, 1300.0, prepared);
    Require(prepared.size() == 2 && prepared[0].eligible_ordinals == 161 &&
                prepared[1].eligible_ordinals == 161,
            "prepared fire did not count eligible ordinals without expanding them");
    auto authoritative_image = PreparedImage(program);
    authoritative_image.persistent_fire_recipes[0].authoritative = true;
    std::vector<hs::runtime_detail::VfxPreparedFireSource> authoritative_prepared;
    hs::runtime_detail::PrepareVfxPersistentFireSources(
        authoritative_image.View(), owners, 1300.0, authoritative_prepared);
    Require(authoritative_prepared.size() == 2 && authoritative_prepared[0].authoritative &&
                authoritative_prepared[1].authoritative,
            "prepared fire dropped the recipe's authoritative ownership flag");

    auto mixed = prepared;
    mixed[0].authoritative = true;
    std::vector<hs::VfxFlashSpawnInput> mixed_output;
    hs::runtime_detail::AppendVfxPreparedFireFlameCommands(mixed, 1, mixed_output);
    Require(mixed_output.size() == 162 && mixed_output.front().stable_seed == expected[0].stable_seed &&
                mixed_output[160].stable_seed == expected[160].stable_seed &&
                mixed_output.back().stable_seed == expected[321].stable_seed,
            "decorative quota suppressed authoritative fire or reordered a mixed source batch");

    mixed = prepared;
    mixed[1].authoritative = true;
    mixed_output.clear();
    hs::runtime_detail::AppendVfxPreparedFireFlameCommands(mixed, 0, mixed_output);
    Require(mixed_output.size() == 161 && mixed_output.front().stable_seed == expected[161].stable_seed &&
                mixed_output.back().stable_seed == expected[321].stable_seed,
            "zero decorative quota did not preserve only the later authoritative source");

    hs::VfxFlashSpawnInput prefix; prefix.stable_seed = 1234;
    std::vector<hs::VfxFlashSpawnInput> capped{prefix};
    hs::runtime_detail::AppendVfxPreparedFireFlameCommands(prepared, 0, capped);
    Require(capped.size() == 1 && capped.front().stable_seed == prefix.stable_seed,
            "zero fire admission changed the caller prefix");

    hs::runtime_detail::AppendVfxPreparedFireFlameCommands(prepared, 2, capped);
    Require(capped.size() == 3 && capped[1].stable_seed == expected[160].stable_seed &&
                capped[1].event_tick == expected[160].event_tick &&
                capped[2].stable_seed == expected[321].stable_seed &&
                capped[2].event_tick == expected[321].event_tick,
            "tiny fire cap did not admit each owner's newest birth in source order");

    capped.resize(1);
    hs::runtime_detail::AppendVfxPreparedFireFlameCommands(prepared, 3, capped);
    Require(capped.size() == 4 && capped[1].stable_seed == expected[159].stable_seed &&
                capped[2].stable_seed == expected[160].stable_seed &&
                capped[3].stable_seed == expected[321].stable_seed,
            "fire quota remainder was not distributed fairly before preserving newest ordinals");

    capped.resize(1);
    hs::runtime_detail::AppendVfxPreparedFireFlameCommands(
        prepared, std::numeric_limits<std::size_t>::max(), capped);
    Require(capped.size() == expected.size() + 1 &&
                capped[1].stable_seed == expected[0].stable_seed &&
                capped.back().event_tick == expected.back().event_tick,
            "unlimited prepared fire changed the legacy output extent or order");
    for (std::size_t index = 0; index < expected.size(); ++index)
        Require(capped[index + 1].stable_seed == expected[index].stable_seed &&
                    capped[index + 1].event_tick == expected[index].event_tick &&
                    std::bit_cast<std::uint32_t>(capped[index + 1].initial_offset.x) ==
                        std::bit_cast<std::uint32_t>(expected[index].initial_offset.x) &&
                    std::bit_cast<std::uint32_t>(capped[index + 1].initial_offset.z) ==
                        std::bit_cast<std::uint32_t>(expected[index].initial_offset.z) &&
                    std::bit_cast<std::uint32_t>(capped[index + 1].normalized_age) ==
                        std::bit_cast<std::uint32_t>(expected[index].normalized_age) &&
                    std::bit_cast<std::uint32_t>(capped[index + 1].lifetime) ==
                        std::bit_cast<std::uint32_t>(expected[index].lifetime),
                "unlimited prepared fire changed an ordinal's seed, age, or float formula");
    for (std::size_t index = 0; index < expected.size(); ++index)
        Require(compatibility_wrapper[index].stable_seed == expected[index].stable_seed &&
                    compatibility_wrapper[index].event_tick == expected[index].event_tick &&
                    std::bit_cast<std::uint32_t>(compatibility_wrapper[index].initial_offset.x) ==
                        std::bit_cast<std::uint32_t>(expected[index].initial_offset.x) &&
                    std::bit_cast<std::uint32_t>(compatibility_wrapper[index].normalized_age) ==
                        std::bit_cast<std::uint32_t>(expected[index].normalized_age),
                "unlimited raw fire wrapper changed an ordinal's seed, tick or formula");

    first.elapsed_seconds = std::numeric_limits<float>::quiet_NaN();
    first.payload = circle;
    hs::runtime_detail::PrepareVfxPersistentFireSources(
        image_view, std::span(&first, 1), 1300.0, prepared);
    Require(prepared.empty(), "malformed live fire owner produced prepared fanout");
    auto malformed_image = PreparedImage(program);
    malformed_image.persistent_fire_recipes[0].rate = 0;
    hs::runtime_detail::PrepareVfxPersistentFireSources(
        malformed_image.View(), owners, 1300.0, prepared);
    Require(prepared.empty(), "zero-rate fire recipe entered prepared fanout");
}

void PersistentFireCountsLargeClockWithoutExpandingPastBirths()
{
    auto image = PreparedImage(PersistentFireProgram());
    Require(image.persistent_fire_recipes.size() == 1, "large fire count fixture needs one prepared source");
    image.persistent_fire_recipes[0].rate = std::numeric_limits<std::uint32_t>::max();
    auto owner = PersistentStampOwner(hs::PersistentVfxKind::FireArea);
    owner.effect_handle = 1; owner.stable_id = 91; owner.quality = hs::VfxQuality::High;
    owner.elapsed_seconds = 67'108'856.0f;
    owner.source_duration_seconds = 67'108'864.0f;
    hs::VfxCirclePayload circle; circle.center = {3, 1, 4}; circle.radius = 2;
    owner.payload = circle;
    const double elapsed = static_cast<double>(owner.elapsed_seconds) * 60;
    const auto whole = static_cast<std::uint64_t>(std::floor(elapsed));
    const auto rate = std::uint64_t{std::numeric_limits<std::uint32_t>::max()};
    const auto previous_schedule = (whole / 60) * rate + (whole % 60) * rate / 60;
    const auto expected = std::min(previous_schedule,
        static_cast<std::uint64_t>(std::floor(elapsed * rate / 60))) + 1;
    std::vector<hs::runtime_detail::VfxPreparedFireSource> prepared;
    hs::runtime_detail::PrepareVfxPersistentFireSources(image.View(), std::span(&owner, 1), elapsed + 100, prepared);
    Require(prepared.size() == 1 && prepared[0].eligible_ordinals == expected &&
        expected > std::numeric_limits<std::uint32_t>::max(),
        "bounded 64-bit fire count changed the previous integer/float schedule");
    std::vector<hs::VfxFlashSpawnInput> first, repeat;
    hs::runtime_detail::AppendVfxPreparedFireFlameCommands(prepared, 3, first);
    hs::runtime_detail::AppendVfxPreparedFireFlameCommands(prepared, 3, repeat);
    Require(first.size() == 3 && repeat.size() == 3,
        "large fire count expanded historical births instead of the admitted suffix");
    for (std::size_t i = 0; i < first.size(); ++i)
        Require(first[i].stable_seed == repeat[i].stable_seed && first[i].event_tick == repeat[i].event_tick &&
            std::isfinite(first[i].normalized_age) && first[i].lifetime > 0,
            "large fire ordinal lost deterministic seed, timing or live lifetime");
}

void VisibleFireAdmissionPreservesRequiredRowsAndStampSuffix()
{
    const auto program = PersistentFireProgram();
    auto first = PersistentStampOwner(hs::PersistentVfxKind::FireArea);
    first.effect_handle = 1;
    first.source_visual_kind = static_cast<std::uint8_t>(hs::PersistentVfxKind::FireArea);
    first.stable_id = 91; first.stable_seed = 314; first.quality = hs::VfxQuality::High;
    first.elapsed_seconds = 20.0f; first.source_duration_seconds = 30.0f;
    hs::VfxCirclePayload circle; circle.center = {3, 1, 4}; circle.radius = 2;
    first.payload = circle;
    auto second = first; second.stable_id = 92; second.stable_seed = 777;
    const std::array owners{first, second};
    auto image = PreparedImage(program);
    const auto expected = LegacyPersistentFire(image.View(), owners, 1300.0);
    std::vector<hs::runtime_detail::VfxPreparedFireSource> prepared;
    hs::runtime_detail::PrepareVfxPersistentFireSources(image.View(), owners, 1300.0, prepared);
    Require(expected.size() == 322 && prepared.size() == 2,
        "visible fire admission setup did not create two sources");
    prepared[0].authoritative = true;
    const auto required_fire = static_cast<std::size_t>(prepared[0].eligible_ordinals);
    const auto decorative_fire = static_cast<std::size_t>(prepared[1].eligible_ordinals);
    Require(required_fire == 161 && decorative_fire == 161,
        "visible fire admission setup changed its source row counts");
    hs::VfxFlashSpawnInput stamp; stamp.stable_seed = 0xA55Au;
    const std::array stamps{stamp};
    hs::VfxFlashSpawnInput prefix; prefix.stable_seed = 1234;
    const auto required_suffix = required_fire + stamps.size();
    const auto make_visible_prefix = [&](std::size_t spare_rows) {
        std::vector<hs::VfxFlashSpawnInput> rows{prefix};
        rows.reserve(rows.size() + spare_rows);
        rows.resize(rows.capacity() - spare_rows, prefix);
        return rows;
    };

    auto exhausted_prepared = prepared;
    auto exhausted = make_visible_prefix(required_suffix);
    const auto exhausted_prefix = exhausted.size();
    std::size_t held_measurements{};
    const auto exhausted_result = hs::runtime_detail::AppendVisibleFireAndStamps(
        exhausted_prepared, decorative_fire, required_suffix, stamps, exhausted,
        hs::runtime_detail::kVfxWorkspaceSoftBudgetBytes, [&]() noexcept {
            ++held_measurements;
            return hs::runtime_detail::kVfxWorkspaceSoftBudgetBytes;
        });
    Require(held_measurements == 1 && exhausted.size() == exhausted_prefix + required_fire + stamps.size() &&
            exhausted_result.emitted_rows == required_fire && exhausted_result.budget_limited &&
            exhausted[0].stable_seed == prefix.stable_seed &&
            exhausted[exhausted_prefix].stable_seed == expected[0].stable_seed &&
            exhausted[exhausted_prefix + required_fire - 1].stable_seed == expected[required_fire - 1].stable_seed &&
            exhausted.back().stable_seed == stamp.stable_seed,
        "exhausted visible-fire budget dropped required rows, prefix, or status stamp");

    const auto run_limited = [&] {
        auto sources = prepared;
        auto output = make_visible_prefix(required_suffix + 3);
        const auto output_prefix = output.size();
        const auto result = hs::runtime_detail::AppendVisibleFireAndStamps(
            sources, decorative_fire, required_suffix, stamps, output,
            hs::runtime_detail::kVfxWorkspaceSoftBudgetBytes,
            []() noexcept { return hs::runtime_detail::kVfxWorkspaceSoftBudgetBytes; });
        return std::tuple{std::move(output), result, output_prefix};
    };
    const auto [limited, limited_result, limited_prefix] = run_limited();
    const auto [repeat, repeat_result, repeat_prefix] = run_limited();
    Require(limited_result.emitted_rows == required_fire + 3 &&
            limited_result.budget_limited && repeat_result.emitted_rows == limited_result.emitted_rows &&
            limited.size() == limited_prefix + required_fire + 3 + stamps.size() &&
            repeat_prefix == limited_prefix &&
            limited[limited_prefix].stable_seed == expected[0].stable_seed &&
            limited[limited_prefix + required_fire - 1].stable_seed == expected[required_fire - 1].stable_seed &&
            limited[limited_prefix + required_fire].stable_seed == expected[expected.size() - 3].stable_seed &&
            limited[limited_prefix + required_fire + 1].stable_seed == expected[expected.size() - 2].stable_seed &&
            limited[limited_prefix + required_fire + 2].stable_seed == expected.back().stable_seed &&
            limited.back().stable_seed == stamp.stable_seed,
        "limited visible-fire admission changed required rows or deterministic newest suffix");
    for (std::size_t index = 0; index < limited.size(); ++index)
        Require(limited[index].stable_seed == repeat[index].stable_seed,
            "limited visible-fire admission changed output order between frames");

    std::vector<hs::runtime_detail::VfxPreparedFireSource> no_sources;
    std::vector<hs::VfxFlashSpawnInput> stamp_only{prefix};
    stamp_only.reserve(stamp_only.size() + stamps.size());
    std::size_t unexpected_measurement{};
    const auto stamp_only_result = hs::runtime_detail::AppendVisibleFireAndStamps(
        no_sources, 0, stamps.size(), stamps, stamp_only,
        hs::runtime_detail::kVfxWorkspaceSoftBudgetBytes, [&]() noexcept {
            ++unexpected_measurement; return std::size_t{0};
        });
    Require(unexpected_measurement == 0 && stamp_only_result.emitted_rows == 0 &&
            stamp_only_result.admission_bytes == 0 && stamp_only.size() == 2 &&
            stamp_only.front().stable_seed == prefix.stable_seed &&
            stamp_only.back().stable_seed == stamp.stable_seed,
        "empty fire preparation failed to append its required stamp suffix without admission work");
}

void PersistentAppendPathsPreservePrefixAndRejectWithoutMutation()
{
    hs::VfxFlashSpawnInput prefix; prefix.stable_seed=999;
    auto fire=PersistentFireProgram();
    hs::VfxPersistentInput fire_owner;
    fire_owner.effect_handle=1; fire_owner.source_visual_kind=static_cast<std::uint8_t>(hs::PersistentVfxKind::FireArea);
    fire_owner.stable_id=91; fire_owner.stable_seed=314; fire_owner.quality=hs::VfxQuality::High;
    fire_owner.elapsed_seconds=.5f; fire_owner.source_duration_seconds=1;
    hs::VfxCirclePayload circle; circle.center={3,1,4}; circle.radius=2; fire_owner.payload=circle;
    const std::array fire_owners{fire_owner};
    const auto expected_fire=BuildPersistentFire(fire,fire_owners,100.0);
    const auto fire_image=PreparedImage(fire);
    std::vector<hs::VfxFlashSpawnInput> appended{prefix};
    hs::runtime_detail::AppendVfxPersistentFireFlameCommands(
        fire_image.View(),fire_owners,100.0,appended);
    Require(appended.size()==expected_fire.size()+1&&appended.front().stable_seed==prefix.stable_seed&&
        appended[1].stable_seed==expected_fire[0].stable_seed&&
        appended.back().event_tick==expected_fire.back().event_tick,
        "persistent fire append changed prefix or recipe order");
    fire_owner.source_duration_seconds=0;
    hs::runtime_detail::AppendVfxPersistentFireFlameCommands(
        fire_image.View(),std::span(&fire_owner,1),100.0,appended);
    Require(appended.size()==expected_fire.size()+1,"rejected fire owner partially appended output");

    const auto stamp=PersistentStampProgram("particle.player.bow_draw",false);
    const auto owner=PersistentStampOwner(hs::PersistentVfxKind::PlayerBowDraw);
    const std::array owners{owner};
    const auto expected_stamp=BuildPersistentStamps(stamp,owners);
    Require(expected_stamp.size()==1,"persistent status append fixture did not prepare");
    const auto stamp_image=PreparedImage(stamp);
    hs::runtime_detail::AppendVfxPersistentStatusStampCommands(stamp_image.View(),owners,appended);
    Require(appended.size()==expected_fire.size()+2&&appended.back().stable_seed==owner.stable_seed&&
        appended.back().shape==expected_stamp[0].shape,
        "persistent status append changed caller ordering");
    auto invalid=owner; invalid.source_duration_seconds=0;
    hs::runtime_detail::AppendVfxPersistentStatusStampCommands(
        stamp_image.View(),std::span(&invalid,1),appended);
    Require(appended.size()==expected_fire.size()+2,"rejected status owner partially appended output");
}

void OwnedStatusTickPulseUsesLiveOwnerAndEventClock()
{
    const auto cooked = StatusTickProgram();
    const std::array events{StatusTickEvent(true), StatusTickEvent(false)};
    const std::array owners{StatusTickOwner(true), StatusTickOwner(false)};
    const auto commands = BuildOwnedStatusTicks(cooked, events, owners);
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

    const auto image = PreparedImage(cooked);
    auto duplicate = events[0];
    duplicate.event_tick = 74;
    auto rejected = events[1];
    rejected.geometry_owner_id = 0;
    std::vector<hs::VfxFlashSpawnInput> appended(1);
    appended.front().event_tick = 5;
    std::vector<hs::Sequence> emitted{399};
    const std::array append_events{events[0], duplicate, rejected};
    hs::runtime_detail::AppendVfxOwnedStatusTickFlashes(
        image.View(), append_events, owners, appended, emitted);
    Require(appended.size() == 3 && appended[0].event_tick == 5 &&
            appended[1].event_tick == 73 && appended[2].event_tick == 74 &&
            emitted == std::vector<hs::Sequence>{399, 401, 401},
            "owned status append lost prefix/order or reported rejected duplicate sequence");
}

void OwnedStatusTickRejectsMissingOrWrongOwner()
{
    const auto cooked = StatusTickProgram();
    auto event = StatusTickEvent(true);
    auto owner = StatusTickOwner(true);
    const auto run = [&] { return BuildOwnedStatusTicks(
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
        return BuildOwnedStatusTicks(
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
                                          std::uint32_t payload_kind, float fixed_seconds)
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
        const auto run = [&] { return BuildPersistentStamps(program, std::span(&owner, 1)); };
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
        const auto run = [&] { return BuildPersistentStamps(program, std::span(&owner, 1)); };
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
        Require(command.interpolate_owner_position &&
                Near(command.previous_position.x, test.path ? 7.5f : owner.previous_transform[12]) &&
                Near(command.previous_position.z, test.path ? 8.5f : owner.previous_transform[14]),
                "attached stamp did not carry its owner's interpolation endpoints");
        // Authoritative equal endpoints already encode a spawn/discontinuity;
        // never substitute a previously rendered stamp or extrapolate velocity.
        if (test.path)
            std::get<hs::VfxProjectilePathPayload>(owner.payload).previous_position = command.position;
        else owner.previous_transform = owner.current_transform;
        const auto discontinuous = run();
        Require(discontinuous.size() == 1 &&
                Near(discontinuous[0].previous_position.x, discontinuous[0].position.x) &&
                Near(discontinuous[0].previous_position.z, discontinuous[0].position.z),
                "equal owner endpoints invented a discontinuity interpolation");
        if (test.path)
            std::get<hs::VfxProjectilePathPayload>(owner.payload).previous_position.x =
                std::numeric_limits<float>::quiet_NaN();
        else owner.previous_transform[12] = std::numeric_limits<float>::quiet_NaN();
        Require(run().empty(), "stamp accepted an invalid previous owner position");
        owner = PersistentStampOwner(test.kind);
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
    std::ifstream file(test_generation::ProductPath("vfx/vfx_program.hsbin"), std::ios::binary);
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
        const auto commands = BuildPersistentStamps(cooked, std::span(&owner, 1));
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
        std::vector<hs::Sequence> superseded;
        const auto run = [&] {
            superseded.clear();
            return BuildFlash(
                cooked, std::span(&input, 1), &superseded);
        };
        auto commands = run();
        Require(commands.size() == 1 && commands[0].shape == hs::VfxImpactShape::StarRing &&
                Near(commands[0].position.x, 3) && Near(commands[0].position.z, 4) &&
                Near(commands[0].direction.x, 4.0f / std::sqrt(41.0f)) &&
                Near(commands[0].lifetime, .08f) && superseded == std::vector<hs::Sequence>{12},
                "pickup link impact did not start at authoritative pickup position");
        cooked.outputs[0].shape = Hash32("unsupported_pickup_shape");
        Require(run().empty() && superseded.empty(),
                "unsupported pickup burst suppressed its legacy fallback");
        cooked.outputs[0].shape = output.shape;
        cooked.outputs[0].hdr = std::numeric_limits<float>::quiet_NaN();
        Require(run().empty() && superseded.empty(),
                "malformed pickup burst suppressed its legacy fallback");
        cooked.outputs[0].hdr = output.hdr;
        auto rejected = input;
        rejected.sequence = 13;
        std::get<hs::VfxLinkPayload>(rejected.payload).target_position.x =
            std::numeric_limits<float>::quiet_NaN();
        const std::array mixed{input, rejected};
        superseded.clear();
        Require(BuildFlash(cooked, mixed, &superseded).size() == 1 &&
                    superseded == std::vector<hs::Sequence>{12},
                "partially rejected pickup batch suppressed an unconverted event");
        link.target_position = link.source_position;
        input.payload = link;
        Require(run().size() == 1 && superseded == std::vector<hs::Sequence>{12},
                "coincident collector dropped pickup burst");
        input.sequence = 0;
        Require(run().empty() && superseded.empty(), "unidentified pickup collector event accepted");
        input.sequence = 12;
        cooked.effect_lookup[0].effect_id = Hash64("particle.player.arrow_release");
        Require(run().empty() && superseded.empty(), "unrelated link effect accepted as pickup impact");
        cooked.effects[0].payload_kind = Hash32("PointEventPayload");
        input.payload = hs::VfxPointPayload{};
        Require(run().size() == 1 && superseded.empty(),
                "non-pickup typed flash suppressed compatibility layers");
    }
}

void TypedFlashAppendPreservesPrefixAndSequenceSuppression()
{
    auto cooked = Program();
    auto first = HitInput();
    first.sequence = 500;
    first.event_tick = 11;
    auto duplicate = first;
    duplicate.event_tick = 12;
    auto next = first;
    next.sequence = 501;
    next.event_tick = 13;
    auto rejected = first;
    rejected.sequence = 502;
    rejected.effect_handle = 99;
    const std::array inputs{first, duplicate, next, rejected};
    const std::array excluded{hs::Sequence{500}};
    const auto image = PreparedImage(cooked);
    std::vector<hs::VfxFlashSpawnInput> output(1);
    output.front().event_tick = 3;
    std::vector<hs::Sequence> superseded{499};
    hs::runtime_detail::AppendVfxTypedFlashCommands(
        image.View(), inputs, excluded, output, &superseded);
    Require(output.size() == 2 && output[0].event_tick == 3 && output[1].event_tick == 13 &&
            superseded == std::vector<hs::Sequence>{499},
            "typed flash append lost prefix/order, failed to suppress duplicate sequences, or accepted rejection");

    hs::VfxProgramData pickup;
    hs::VfxEffectRecord effect;
    effect.handle = 1;
    effect.payload_kind = Hash32("SourceTargetPayload");
    effect.seconds = .32f;
    effect.sources = {0, 1};
    pickup.effects.push_back(effect);
    pickup.effect_lookup.push_back({Hash64("particle.pickup.xp_collect"), 1});
    hs::VfxSourceRecord source;
    source.effect = 1;
    source.type = hs::VfxSourceType::ImpactSprite;
    source.knot_count = 2;
    source.knots = {0, .25f, 0, 0};
    source.outputs = {0, 1};
    pickup.sources.push_back(source);
    pickup.parameters.push_back({Hash32("sdf"), hs::VfxParameterType::Enum,
                                 Hash32("star_ring")});
    hs::VfxOutputRecord sprite;
    sprite.source = 0;
    sprite.profile = hs::VfxOutputProfile::SpriteSdfAdd;
    sprite.shape = Hash32("small_star_ring");
    sprite.motion = hs::VfxMotionKind::SnapsInwardUpward;
    sprite.rgba = {1, .6f, .2f, .8f};
    sprite.hdr = 2;
    sprite.parameters = {0, 1};
    pickup.outputs.push_back(sprite);
    hs::VfxEventInput collect;
    collect.effect_handle = 1;
    collect.sequence = 77;
    collect.event_tick = 8;
    hs::VfxLinkPayload link;
    link.source_position = {1, 0, 2};
    link.target_position = {3, 0, 4};
    collect.payload = link;
    const std::array duplicate_collect{collect, collect};
    const auto pickup_image = PreparedImage(pickup);
    std::vector<hs::VfxFlashSpawnInput> pickup_output(1);
    pickup_output.front().event_tick = 4;
    std::vector<hs::Sequence> pickup_sequences{70};
    const std::array exclude_collect{hs::Sequence{77}};
    hs::runtime_detail::AppendVfxTypedFlashCommands(
        pickup_image.View(), duplicate_collect, exclude_collect,
        pickup_output, &pickup_sequences);
    Require(pickup_output.size() == 1 && pickup_output.front().event_tick == 4 &&
            pickup_sequences == std::vector<hs::Sequence>{70},
            "paired duplicate pickup sequences were not excluded before flash emission");
    collect.sequence = 78;
    collect.event_tick = 9;
    auto malformed_collect = collect;
    malformed_collect.sequence = 79;
    malformed_collect.event_tick = 10;
    std::get<hs::VfxLinkPayload>(malformed_collect.payload).target_position.x =
        std::numeric_limits<float>::quiet_NaN();
    const std::array mixed_collect{collect, malformed_collect};
    hs::runtime_detail::AppendVfxTypedFlashCommands(
        pickup_image.View(), mixed_collect, {}, pickup_output, &pickup_sequences);
    Require(pickup_output.size() == 2 && pickup_output.back().event_tick == 9 &&
            pickup_sequences == std::vector<hs::Sequence>{70, 78},
            "pickup append suppressed malformed event or lost successful source sequence");
}

void PreparedFlashAdmissionPreservesOrderAndProvenance()
{
    auto program = Program();
    program.sources[1].authoritative = 1;
    auto first = HitInput();
    first.sequence = 91;
    first.event_tick = 31;
    auto required = first;
    required.effect_handle = 2;
    required.sequence = 92;
    required.event_tick = 32;
    const std::array inputs{first, required};
    const auto image = PreparedImage(program);
    std::vector<hs::runtime_detail::VfxPreparedFlashOutput> prepared;
    hs::runtime_detail::PrepareVfxTypedFlashOutputs(image.View(), inputs, {}, prepared);
    Require(prepared.size() == 2 && prepared[0].count == 1 &&
            prepared[0].admitted_count == 1 && prepared[0].cosmetic &&
            !prepared[1].cosmetic && prepared[1].importance == program.effects[1].importance,
            "prepared flash source ownership or one-row fanout metadata was lost");
    auto all = BuildFlash(program, inputs);
    std::vector<hs::VfxFlashSpawnInput> result(1);
    result.reserve(8);
    result.front().event_tick = 7;
    prepared[0].admitted_count = 0;
    hs::runtime_detail::AppendVfxPreparedFlashOutputs(prepared, result);
    Require(all.size() == 2 && result.size() == 2 && result[0].event_tick == 7 &&
            result[1].event_tick == required.event_tick &&
            result[1].stable_seed == required.stable_seed,
            "prepared flash admission changed caller prefix, surviving event order, or seed");
    const auto warm_capacity = result.capacity();
    prepared[0].admitted_count = 1;
    hs::runtime_detail::AppendVfxPreparedFlashOutputs(prepared, result);
    Require(result.size() == 4 && result[2].event_tick == first.event_tick &&
            result[3].event_tick == required.event_tick && result.capacity() == warm_capacity,
            "unlimited prepared flash append failed original order or warm capacity reuse");

    hs::VfxProgramData pickup;
    hs::VfxEffectRecord pickup_effect;
    pickup_effect.handle = 1;
    pickup_effect.payload_kind = Hash32("SourceTargetPayload");
    pickup_effect.seconds = .32f;
    pickup_effect.sources = {0, 1};
    pickup.effects.push_back(pickup_effect);
    pickup.effect_lookup.push_back({Hash64("particle.pickup.xp_collect"), 1});
    hs::VfxSourceRecord pickup_source;
    pickup_source.effect = 1;
    pickup_source.type = hs::VfxSourceType::ImpactSprite;
    pickup_source.knot_count = 2;
    pickup_source.knots = {0, .25f, 0, 0};
    pickup_source.outputs = {0, 1};
    pickup.sources.push_back(pickup_source);
    pickup.parameters.push_back({Hash32("sdf"), hs::VfxParameterType::Enum,
                                 Hash32("star_ring")});
    hs::VfxOutputRecord pickup_output_recipe;
    pickup_output_recipe.source = 0;
    pickup_output_recipe.profile = hs::VfxOutputProfile::SpriteSdfAdd;
    pickup_output_recipe.shape = Hash32("small_star_ring");
    pickup_output_recipe.motion = hs::VfxMotionKind::SnapsInwardUpward;
    pickup_output_recipe.rgba = {1, .6f, .2f, .8f};
    pickup_output_recipe.hdr = 2;
    pickup_output_recipe.parameters = {0, 1};
    pickup.outputs.push_back(pickup_output_recipe);
    const auto pickup_image = PreparedImage(pickup);
    auto collect = HitInput();
    hs::VfxLinkPayload link;
    link.source_position = {1, 0, 2};
    link.target_position = {3, 0, 4};
    collect.payload = link;
    collect.sequence = 93;
    std::vector<hs::runtime_detail::VfxPreparedFlashOutput> pickup_plan;
    hs::runtime_detail::PrepareVfxTypedFlashOutputs(
        pickup_image.View(), std::span(&collect, 1), {}, pickup_plan);
    Require(pickup_plan.size() == 1 && pickup_plan.front().pickup_collect,
            "prepared pickup flash lost its supersession identity");
    pickup_plan.front().admitted_count = 0;
    std::vector<hs::VfxFlashSpawnInput> pickup_rows;
    std::vector<hs::Sequence> superseded;
    hs::runtime_detail::AppendVfxPreparedFlashOutputs(pickup_plan, pickup_rows, &superseded);
    Require(pickup_rows.empty() && superseded.empty(),
            "unadmitted pickup flash suppressed compatibility visuals");
    pickup_plan.front().admitted_count = 1;
    hs::runtime_detail::AppendVfxPreparedFlashOutputs(pickup_plan, pickup_rows, &superseded);
    Require(pickup_rows.size() == 1 && superseded == std::vector<hs::Sequence>{collect.sequence},
            "admitted pickup flash did not preserve the original supersession fact");
}

void ProductionStatusAndPickupOutputsDecode()
{
    std::ifstream file(test_generation::ProductPath("vfx/vfx_program.hsbin"), std::ios::binary);
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
        const auto commands = BuildPersistentStamps(cooked, std::span(&owner, 1));
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
        std::vector<hs::Sequence> superseded;
        const auto commands = BuildFlash(
            cooked, std::span(&event, 1), &superseded);
        Require(commands.size() == 1 && superseded == std::vector<hs::Sequence>{event.sequence},
                "production pickup link did not decode its collection burst");
    }
}
} // namespace

int RunVfxFlashTests()
{
    try
    {
        AuthoredSdfShapesAndContext();
        AuthoredOitRecipes();
        PersistentFireFlameUsesPreparedRecipeAndLiveAge();
        PreparedPersistentFireCapsNewestBirthsFairly();
        PersistentFireCountsLargeClockWithoutExpandingPastBirths();
        VisibleFireAdmissionPreservesRequiredRowsAndStampSuffix();
        PersistentAppendPathsPreservePrefixAndRejectWithoutMutation();
        OwnedStatusTickPulseUsesLiveOwnerAndEventClock();
        OwnedStatusTickRejectsMissingOrWrongOwner();
        OwnedStatusTickRejectsMalformedRecipe();
        PersistentStatusStamps();
        UpgradeStatusStampsUseAuthoredGeometry();
        ProductionUpgradeStatusStampsDecode();
        PickupCollectorImpactUsesLinkSource();
        TypedFlashAppendPreservesPrefixAndSequenceSuppression();
        PreparedFlashAdmissionPreservesOrderAndProvenance();
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
