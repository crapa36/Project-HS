#include "../Source/Runtime/Private/vfx_typed_ground_commands.hpp"
#include <hs/core/render_snapshot.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
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
        hash = (hash ^ character) * 1099511628211ull;
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
    effect.input_mode = 1;
    effect.payload_kind = Hash32("CircleAreaPayload");
    effect.sources = {0, 2};
    cooked.effects.push_back(effect);

    hs::VfxSourceRecord direct;
    direct.effect = 1;
    direct.type = hs::VfxSourceType::Direct;
    direct.knot_count = 2;
    direct.knots = {0.0f, 1.0f, 0.0f, 0.0f};
    direct.outputs = {0, 5};
    cooked.sources.push_back(direct);
    hs::VfxSourceRecord other = direct;
    other.type = hs::VfxSourceType::ProjectileFollow;
    other.outputs = {5, 1};
    cooked.sources.push_back(other);

    const auto width = [](float value) {
        return hs::VfxParameterRecord{Hash32("edge_width_world"),
                                      hs::VfxParameterType::Float,
                                      std::bit_cast<std::uint32_t>(value)};
    };
    cooked.parameters = {width(0.055f), width(0.075f), width(0.09f)};

    hs::VfxOutputRecord ring;
    ring.source = 0;
    ring.profile = hs::VfxOutputProfile::GroundSdfSoft;
    ring.shape = Hash32("exact_hitbox_ring");
    ring.motion = hs::VfxMotionKind::StableBoundaryInwardPulse;
    ring.motion_rate_hz = 0.8f;
    ring.motion_amplitude = 0.22f;
    ring.motion_inset_fraction = 0.12f;
    ring.rgba = {0.2f, 0.4f, 0.6f, 0.8f};
    ring.hdr = 2.5f;
    ring.gradient_row = 17;
    ring.parameters = {0, 1};
    cooked.outputs.push_back(ring);

    auto wrong_profile = ring;
    wrong_profile.profile = hs::VfxOutputProfile::GroundSdfAdd;
    cooked.outputs.push_back(wrong_profile);
    auto wrong_shape = ring;
    wrong_shape.shape = Hash32("exact_circle_ring");
    cooked.outputs.push_back(wrong_shape);
    auto high_only = ring;
    high_only.min_quality = 2;
    cooked.outputs.push_back(high_only);
    auto second_ring = ring;
    second_ring.parameters = {1, 1};
    cooked.outputs.push_back(second_ring);
    auto other_source = ring;
    other_source.source = 1;
    other_source.parameters = {2, 1};
    cooked.outputs.push_back(other_source);
    return cooked;
}

hs::VfxPersistentInput CircleInput()
{
    hs::VfxPersistentInput input;
    input.stable_id = 0x123456789abcdef0ull;
    input.effect_handle = 1;
    input.source_visual_kind = static_cast<std::uint8_t>(hs::PersistentVfxKind::FireArea);
    input.stable_seed = 0x1234abcd;
    input.normalized_age = 0.35f;
    input.quality = hs::VfxQuality::Medium;
    hs::VfxCirclePayload circle;
    circle.center = {2.0f, 0.25f, -4.0f};
    circle.radius = 2.75f;
    circle.lifetime01 = 0.4f;
    input.payload = circle;
    return input;
}

void ConvertsOnlyAuthoredDirectExactRings()
{
    const auto cooked = Program();
    const std::vector inputs{CircleInput()};
    const auto commands = hs::runtime_detail::BuildVfxTypedGroundCommands(cooked, inputs, 712);
    Require(commands.size() == 2, "direct exact-hitbox output filtering failed");
    const auto &first = commands[0];
    const auto &command = first.command;
    Require(first.stable_id == inputs[0].stable_id && first.effect_handle == 1 &&
            first.source_visual_kind == inputs[0].source_visual_kind,
            "gameplay owner/effect/visual identity was lost");
    Require(command.tick == 712 && command.seed == inputs[0].stable_seed,
            "tick or stable seed was lost");
    Require(command.position.x == 2.0f && command.position.y == 0.25f &&
            command.position.z == -4.0f, "gameplay center was changed");
    Require(command.primitive == hs::VfxPrimitive::ExactRing &&
            command.renderer == hs::VfxRenderer::Ground,
            "exact-ring rendering contract was not selected");
    Require(Near(first.edge_width_world, 0.055f) &&
            Near(command.start_size_min, 2.805f) &&
            Near(command.end_size_max, 2.805f) &&
            Near(command.stretch * command.start_size_min, 2.75f),
            "authored edge width or gameplay radius was changed");
    Require(Near(command.start_color.x, 0.5f) && Near(command.start_color.y, 1.0f) &&
            Near(command.start_color.z, 1.5f) && Near(command.start_color.w, 0.8f),
            "authored rgba or hdr was changed");
    Require(Near(first.normalized_age, 0.35f) && Near(first.lifetime01, 0.4f),
            "gameplay lifetime progress was lost");
    Require(first.gradient_row == 17 && Near(first.hdr, 2.5f),
            "authored LUT row or HDR intensity was lost");
    Require(Near(first.motion.rate_hz, 0.8f) && Near(first.motion.amplitude, 0.22f) &&
            Near(first.motion.inset_fraction, 0.12f), "cooked boundary motion was lost");
    Require(Near(commands[1].edge_width_world, 0.075f) &&
            commands[1].command.sequence != command.sequence,
            "multiple authored ring outputs were merged");
}

void RejectsUnsupportedOrMissingGeometry()
{
    auto cooked = Program();
    std::vector inputs{CircleInput()};
    inputs[0].payload = hs::VfxConePayload{};
    Require(hs::runtime_detail::BuildVfxTypedGroundCommands(cooked, inputs, 712).empty(),
            "non-circle input was rendered as a circle");
    inputs[0] = CircleInput();
    cooked.effects[0].payload_kind = Hash32("RingWithGapsPayload");
    Require(hs::runtime_detail::BuildVfxTypedGroundCommands(cooked, inputs, 712).empty(),
            "incompatible authored payload schema was rendered as a circle");
    cooked = Program();
    std::get<hs::VfxCirclePayload>(inputs[0].payload).radius = 0.0f;
    Require(hs::runtime_detail::BuildVfxTypedGroundCommands(cooked, inputs, 712).empty(),
            "zero gameplay radius created a fake ring");
    inputs[0] = CircleInput();
    cooked.outputs[0].parameters.count = 0;
    cooked.outputs[4].parameters.count = 0;
    Require(hs::runtime_detail::BuildVfxTypedGroundCommands(cooked, inputs, 712).empty(),
            "missing authored world-space edge width was invented");
    cooked = Program();
    cooked.sources[0].type = hs::VfxSourceType::ProjectileFollow;
    Require(hs::runtime_detail::BuildVfxTypedGroundCommands(cooked, inputs, 712).empty(),
            "non-direct source was executed as direct geometry");
}

void PreservesSharedSourceRingAndFill()
{
    auto cooked = Program();
    auto fill = cooked.outputs[0];
    fill.profile = hs::VfxOutputProfile::GroundSdfOit;
    fill.shape = Hash32("low_frequency_fill");
    fill.motion = hs::VfxMotionKind::SlowRadialFlow;
    fill.motion_rate_hz = fill.motion_amplitude = fill.motion_inset_fraction = 0.0f;
    fill.parameters = {static_cast<std::uint32_t>(cooked.parameters.size()), 1};
    fill.rgba[3] = 0.18f;
    fill.hdr = 0.9f;
    cooked.parameters.push_back({Hash32("flow_speed"), hs::VfxParameterType::Float,
                                std::bit_cast<std::uint32_t>(0.25f)});
    // Ring and interior consume the same Direct source and owner geometry.
    cooked.outputs[1] = fill;
    const std::vector inputs{CircleInput()};
    const auto commands = hs::runtime_detail::BuildVfxTypedGroundCommands(cooked, inputs, 712);
    Require(commands.size() == 3, "shared-source fill was dropped or replaced its ring");
    const auto &body = commands[1];
    Require(body.command.primitive == hs::VfxPrimitive::LowFrequencyFill &&
            body.stable_id == commands[0].stable_id &&
            body.command.sequence != commands[0].command.sequence,
            "shared source outputs lost independent draw identity");
    Require(Near(body.command.start_size_min, 2.75f) &&
            Near(body.command.stretch, 0.25f) && Near(body.edge_width_world, 0.0f),
            "fill changed gameplay extent or authored flow speed");
    Require(Near(body.command.start_color.x, 0.18f) &&
            Near(body.command.start_color.w, 0.18f), "fill color was not cooked RGBA/HDR");
    cooked.outputs[1].parameters.count = 0;
    Require(hs::runtime_detail::BuildVfxTypedGroundCommands(cooked, inputs, 712).size() == 2,
            "missing flow parameter was silently invented");
}

hs::VfxEventInput CircleEvent()
{
    hs::VfxEventInput input;
    input.effect_handle = 1;
    input.event_tick = 100;
    input.sequence = 0x55;
    input.stable_seed = 0x1234abcd;
    input.quality = hs::VfxQuality::Medium;
    hs::VfxCirclePayload circle;
    circle.center = {4.0f, 0.25f, -2.0f};
    circle.radius = 3.5f;
    circle.lifetime01 = 0.65f;
    input.payload = circle;
    return input;
}

void ConvertsFixedTimeCircleEvents()
{
    auto cooked = Program();
    cooked.effects[0].input_mode = 0;
    cooked.effects[0].timing_kind = 0;
    cooked.effects[0].seconds = 2.0f;
    auto event = CircleEvent();
    Require(hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked, std::vector{event}, 99).empty(),
            "event before start was rendered");
    auto commands = hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked, std::vector{event}, 160);
    Require(commands.size() == 2, "fixed-time circle event was not emitted");
    Require(commands[0].stable_id == event.sequence && commands[0].source_visual_kind == 0xff &&
            commands[0].command.tick == event.event_tick &&
            Near(commands[0].normalized_age, 0.5f), "event identity or age was changed");
    Require(Near(commands[0].command.start_size_min, 3.555f) &&
            Near(commands[0].command.stretch * commands[0].command.start_size_min, 3.5f),
            "event gameplay radius was replaced by cosmetic scale");
    Require(commands[0].gradient_row == 17 && Near(commands[0].hdr, 2.5f) &&
            Near(commands[0].motion.rate_hz, 0.8f), "event authored output fields were lost");
    auto second_event = event;
    second_event.sequence += (static_cast<hs::Sequence>(1) << 32);
    const auto second_commands = hs::runtime_detail::BuildVfxTypedEventGroundCommands(
        cooked, std::vector{second_event}, 160);
    Require(!second_commands.empty() && second_commands[0].command.sequence != commands[0].command.sequence,
            "event sequence high bits were discarded");
    Require(hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked, std::vector{event}, 220).empty(),
            "event after fixed lifetime was rendered");
}

void HonorsEventSourceEnvelopeAndSchema()
{
    auto cooked = Program();
    cooked.effects[0].input_mode = 0;
    cooked.effects[0].timing_kind = 0;
    cooked.effects[0].seconds = 2.0f;
    cooked.sources[0].knot_count = 4;
    cooked.sources[0].knots = {0.2f, 0.4f, 0.6f, 0.8f};
    cooked.sources[0].outputs = {0, 5};
    auto event = CircleEvent();
    auto commands = hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked, std::vector{event}, 124);
    Require(commands.empty(), "event before source envelope was rendered");
    commands = hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked, std::vector{event}, 136);
    Require(commands.size() == 2 && Near(commands[0].lifetime01, 0.65f) &&
            Near(commands[0].command.start_color.w, 0.4f),
            "four-knot source fade envelope was not applied");
    commands = hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked, std::vector{event}, 184);
    Require(commands.size() == 2 && Near(commands[0].command.start_color.w, 0.4f),
            "four-knot source fade out was not applied");
    Require(hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked, std::vector{event}, 196).empty(),
            "source remained visible at its exclusive endpoint");
    cooked.effects[0].payload_kind = Hash32("RingWithGapsPayload");
    Require(hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked, std::vector{event}, 136).empty(),
            "mismatched event payload schema was rendered");
    cooked = Program();
    cooked.effects[0].input_mode = 1;
    Require(hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked, std::vector{event}, 136).empty(),
            "persistent effect accepted event input");
}

hs::VfxPersistentInput LineInput()
{
    auto input = CircleInput();
    hs::VfxLinePayload line;
    line.start = {-3.0f, 0.2f, 4.0f};
    line.end = {5.0f, 0.2f, -2.0f};
    line.width = 2.0f;
    line.lifetime01 = 0.35f;
    input.payload = line;
    input.normalized_age = 0.5f;
    return input;
}

hs::VfxProgramData LineProgram(bool hatch)
{
    auto cooked = Program();
    cooked.effects[0].payload_kind = Hash32("LineAreaPayload");
    cooked.effects[0].sources = {0, 1};
    cooked.sources.resize(1);
    cooked.parameters.clear();
    cooked.sources[0].knot_count = 4;
    cooked.sources[0].knots = {0.2f, 0.4f, 0.6f, 0.8f};
    cooked.sources[0].outputs = {0, 1};
    cooked.outputs.resize(1);
    auto &output = cooked.outputs[0];
    output.source = 0;
    output.profile = hatch ? hs::VfxOutputProfile::GroundSdfOit : hs::VfxOutputProfile::GroundSdfSoft;
    output.shape = Hash32(hatch ? "diagonal_hatch" : "dual_parallel_lines");
    output.motion = hatch ? hs::VfxMotionKind::ScrollsAttackDirection :
        hs::VfxMotionKind::LockedChevronsTowardImpact;
    output.hdr = hatch ? 1.1f : 2.7f;
    output.rgba = {0.2f, 0.4f, 0.6f, 0.8f};
    output.parameters = {0, hatch ? 2u : 2u};
    cooked.parameters.push_back({Hash32(hatch ? "repeat" : "edge_width_world"),
        hs::VfxParameterType::Float, std::bit_cast<std::uint32_t>(hatch ? 5.0f : 0.06f)});
    cooked.parameters.push_back({Hash32(hatch ? "scroll" : "chevron_spacing"),
        hs::VfxParameterType::Float, std::bit_cast<std::uint32_t>(hatch ? 1.2f : 0.75f)});
    return cooked;
}

void ConvertsLineGeometryAndAuthoredParams()
{
    auto cooked = LineProgram(false);
    auto input = LineInput();
    auto commands = hs::runtime_detail::BuildVfxTypedGroundCommands(cooked, std::vector{input}, 712);
    Require(commands.size() == 1, "line border was not emitted");
    const auto &item = commands[0];
    const float length = std::sqrt(8.0f * 8.0f + 6.0f * 6.0f);
    Require(item.geometry.shape == hs::VfxGroundShape::LineBorder &&
            Near(item.geometry.half_width, 1.0f) && Near(item.geometry.half_length, length * 0.5f) &&
            Near(item.geometry.direction.x, 0.8f) && Near(item.geometry.direction.z, -0.6f) &&
            Near(item.geometry.edge_width, 0.06f) && Near(item.geometry.spacing, 0.75f) &&
            Near(item.geometry.scroll, 0.8f), "line geometry or authored border params changed");
    Require(Near(item.command.position.x, 1.0f) && Near(item.command.position.z, 1.0f) &&
            item.command.start_size_min > 0.0f, "line center or legacy bounding extent missing");
    Require(Near(item.command.start_color.w, 0.8f), "line source envelope was not applied");
    input.normalized_age = 0.3f;
    commands = hs::runtime_detail::BuildVfxTypedGroundCommands(cooked, std::vector{input}, 712);
    Require(commands.size() == 1 && Near(commands[0].command.start_color.w, 0.4f),
            "persistent line fade-in envelope was not applied");
    input.normalized_age = 0.8f;
    Require(hs::runtime_detail::BuildVfxTypedGroundCommands(cooked, std::vector{input}, 712).empty(),
            "persistent line remained visible at its exclusive source endpoint");

    cooked = LineProgram(true);
    input.normalized_age = 0.5f;
    commands = hs::runtime_detail::BuildVfxTypedGroundCommands(cooked, std::vector{input}, 712);
    Require(commands.size() == 1 && commands[0].geometry.shape == hs::VfxGroundShape::LineHatch &&
            Near(commands[0].geometry.spacing, 5.0f) && Near(commands[0].geometry.scroll, 1.2f) &&
            Near(commands[0].geometry.edge_width, 0.0f), "line hatch params were not preserved");
}

void RejectsMalformedLineGeometryAndCone()
{
    auto cooked = LineProgram(false);
    auto input = LineInput();
    input.payload = hs::VfxConePayload{};
    Require(hs::runtime_detail::BuildVfxTypedGroundCommands(cooked, std::vector{input}, 712).empty(),
            "cone payload was converted to a line");
    input = LineInput();
    std::get<hs::VfxLinePayload>(input.payload).end = std::get<hs::VfxLinePayload>(input.payload).start;
    Require(hs::runtime_detail::BuildVfxTypedGroundCommands(cooked, std::vector{input}, 712).empty(),
            "zero-length line was accepted");
    input = LineInput();
    cooked.parameters[0].bits = std::bit_cast<std::uint32_t>(0.0f);
    Require(hs::runtime_detail::BuildVfxTypedGroundCommands(cooked, std::vector{input}, 712).empty(),
            "zero edge width was invented");
    cooked = LineProgram(false);
    cooked.outputs[0].shape = Hash32("exact_hitbox_ring");
    cooked.outputs[0].motion = hs::VfxMotionKind::StableBoundaryInwardPulse;
    cooked.parameters[0].bits = std::bit_cast<std::uint32_t>(0.055f);
    Require(hs::runtime_detail::BuildVfxTypedGroundCommands(cooked, std::vector{input}, 712).empty(),
            "circle output was synthesized for a line payload");
}

hs::VfxEventInput ConeEvent()
{
    hs::VfxEventInput event;
    event.effect_handle = 1;
    event.event_tick = 100;
    event.sequence = 0xfeed;
    event.quality = hs::VfxQuality::High;
    hs::VfxConePayload cone;
    cone.origin = {-4.0f, 0.35f, 2.0f};
    cone.direction = {3.0f, 0.2f, 4.0f};
    cone.range = 19.0f;
    cone.half_angle_degrees = 73.0f;
    cone.lifetime01 = 0.25f;
    event.payload = cone;
    return event;
}

void ConvertsFixedConeGroundEvents()
{
    auto cooked = LineProgram(false);
    cooked.effects[0].input_mode = 0;
    cooked.effects[0].payload_kind = Hash32("ConePayload");
    cooked.effects[0].timing_kind = 0;
    cooked.effects[0].seconds = 1.0f;
    auto event = ConeEvent();
    auto commands = hs::runtime_detail::BuildVfxTypedEventGroundCommands(
        cooked, std::vector{event}, 115);
    Require(commands.size() == 1, "fixed cone ground event was not emitted");
    const auto &item = commands[0];
    Require(item.geometry.shape == hs::VfxGroundShape::ConeBorder &&
            Near(item.geometry.range, 19.0f) &&
            Near(item.geometry.half_angle_radians, 73.0f * 3.14159265358979323846f / 180.0f) &&
            Near(item.geometry.direction.x, 0.6f) && Near(item.geometry.direction.z, 0.8f) &&
            Near(item.geometry.edge_width, 0.06f) && Near(item.geometry.spacing, 0.75f) &&
            Near(item.command.position.x, -4.0f) && Near(item.command.position.z, 2.0f) &&
            Near(item.command.start_size_min, 19.06f),
            "cone geometry or authored border params changed");
    Require(hs::runtime_detail::BuildVfxTypedEventGroundCommands(
        cooked, std::vector{event}, 99).empty(), "cone event rendered before its start");
    Require(hs::runtime_detail::BuildVfxTypedEventGroundCommands(
        cooked, std::vector{event}, 161).empty(), "cone event rendered after its lifetime");
    cooked.outputs[0].min_quality = 2;
    event.quality = hs::VfxQuality::Low;
    Require(hs::runtime_detail::BuildVfxTypedEventGroundCommands(
        cooked, std::vector{event}, 115).empty(), "cone output ignored quality filtering");
    cooked.outputs[0].min_quality = 0;
    event.quality = hs::VfxQuality::High;
    auto &cone = std::get<hs::VfxConePayload>(event.payload);
    cone.half_angle_degrees = 0.0f;
    Require(hs::runtime_detail::BuildVfxTypedEventGroundCommands(
        cooked, std::vector{event}, 115).empty(), "zero cone angle was accepted");
    cone.half_angle_degrees = 181.0f;
    Require(hs::runtime_detail::BuildVfxTypedEventGroundCommands(
        cooked, std::vector{event}, 115).empty(), "overwide cone angle was accepted");
    cone.half_angle_degrees = 73.0f;
    cone.range = 0.0f;
    Require(hs::runtime_detail::BuildVfxTypedEventGroundCommands(
        cooked, std::vector{event}, 115).empty(), "zero cone range was accepted");
    cone.range = 19.0f;
    cone.direction = {0.0f, 1.0f, 0.0f};
    Require(hs::runtime_detail::BuildVfxTypedEventGroundCommands(
        cooked, std::vector{event}, 115).empty(), "cone without an XZ direction was accepted");
}

void ConvertsLineEventTiming()
{
    auto cooked = LineProgram(false);
    cooked.effects[0].input_mode = 0;
    cooked.effects[0].timing_kind = 0;
    cooked.effects[0].seconds = 2.0f;
    auto persistent = LineInput();
    hs::VfxEventInput event;
    event.effect_handle = 1;
    event.event_tick = 100;
    event.sequence = 0xabc;
    event.payload = std::get<hs::VfxLinePayload>(persistent.payload);
    event.quality = hs::VfxQuality::High;
    auto commands = hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked, std::vector{event}, 136);
    Require(commands.size() == 1 && Near(commands[0].normalized_age, 0.3f) &&
            Near(commands[0].command.start_color.w, 0.4f),
            "line event timing or four-knot fade was changed");
    Require(hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked, std::vector{event}, 124).empty(),
            "line event rendered before its source envelope");
}

void UsesAuthoritativeWarningWindow()
{
    auto cooked = LineProgram(false);
    cooked.effects[0].input_mode = 0;
    cooked.effects[0].payload_kind = Hash32("ConePayload");
    cooked.effects[0].timing_kind = 0;
    cooked.effects[0].seconds = 0.6f;
    cooked.sources[0].knot_count = 2;
    cooked.sources[0].knots = {0,1,0,0};
    auto event = ConeEvent();
    event.geometry_owner_id = 51;
    event.geometry_start_tick = 100;
    event.geometry_end_tick = 220;
    const auto commands = hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked, std::vector{event}, 190);
    Require(commands.size() == 1 && Near(commands[0].normalized_age,0.75f) &&
            Near(commands[0].command.lifetime_max,2.0f),
            "warning ended at cosmetic recipe time instead of scheduled execution");
    Require(hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked, std::vector{event}, 220).empty(),
            "warning survived scheduled execution");
}
void RingGapsPreservesGeometryAndOwnerAge()
{
    auto cooked = Program();
    cooked.effects[0].payload_kind = Hash32("RingWithGapsPayload");
    cooked.effects[0].sources = {0,1};
    cooked.sources[0].outputs = {0,2};
    cooked.outputs[1] = cooked.outputs[0];
    cooked.outputs[1].profile = hs::VfxOutputProfile::GroundSdfOit;
    cooked.outputs[1].shape = Hash32("low_frequency_fill");
    cooked.outputs[1].motion = hs::VfxMotionKind::SlowRadialFlow;
    cooked.outputs[1].parameters = {3,1};
    cooked.parameters.push_back({Hash32("flow_speed"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(.31f)});
    auto input = CircleInput();
    hs::VfxRingGapsPayload ring;
    ring.center = {17,.4f,-8}; ring.inner_radius=8.875f; ring.outer_radius=8.9f;
    ring.gap_half_width_degrees=7; ring.gap_angles_degrees={-17,43,407,720}; ring.lifetime01=.6f;
    input.payload=ring; input.normalized_age=.1f;
    auto run = [&] { return hs::runtime_detail::BuildVfxTypedGroundCommands(cooked,std::span(&input,1),700); };
    auto commands=run();
    Require(commands.size()==2,"ring-with-gaps outputs not decoded");
    const auto &border=commands[0]; const auto &fill=commands[1];
    Require(border.geometry.shape==hs::VfxGroundShape::RingGapsBorder && fill.geometry.shape==hs::VfxGroundShape::RingGapsFill,
            "ring-with-gaps converted to generic circle");
    Require(Near(border.geometry.inner_radius,8.875f)&&Near(border.geometry.outer_radius,8.9f)&&
            Near(border.geometry.edge_width,.055f)&&Near(border.command.start_size_max,8.955f)&&
            Near(border.command.position.x,17)&&Near(border.command.position.z,-8),"narrow annulus or world edge lost");
    const float radians=3.14159265358979323846f/180;
    Require(border.geometry.gap_angles_radians.size()==4&&Near(border.geometry.gap_angles_radians[0],343*radians)&&
            Near(border.geometry.gap_angles_radians[1],43*radians)&&Near(border.geometry.gap_angles_radians[2],47*radians)&&
            Near(border.geometry.gap_angles_radians[3],0)&&Near(border.geometry.gap_half_angle_radians,7*radians),
            "irregular gap angles or wrapping lost");
    Require(Near(border.normalized_age,.6f)&&Near(border.lifetime01,.6f)&&Near(fill.command.stretch,.31f),"authoritative age or flow lost");
    cooked.sources[0].knot_count=4; cooked.sources[0].knots={0,.2f,.4f,1};
    commands=run();Require(commands.size()==2&&Near(commands[0].command.start_color.w,.8f*(2.0f/3.0f)),"four-knot owner fade ignored");
    std::get<hs::VfxRingGapsPayload>(input.payload).lifetime01=1;
    Require(run().empty(),"expired owner emitted ground output");
    std::get<hs::VfxRingGapsPayload>(input.payload).lifetime01=.3f;
    std::get<hs::VfxRingGapsPayload>(input.payload).gap_angles_degrees.clear();
    std::get<hs::VfxRingGapsPayload>(input.payload).gap_half_width_degrees=180;
    commands=run();Require(commands.size()==2&&commands[0].geometry.gap_angles_radians.empty(),"empty gaps invented sectors");
    cooked.outputs[1].min_quality=2;commands=run();Require(commands.size()==1,"ring gap quality ignored");
    cooked.effects[0].payload_kind=Hash32("CircleAreaPayload");Require(run().empty(),"ring geometry coerced into Circle schema");
}

void RingGapsRejectsMalformedGeometry()
{
    auto cooked=Program();cooked.effects[0].payload_kind=Hash32("RingWithGapsPayload");
    auto input=CircleInput();hs::VfxRingGapsPayload ring;
    ring.inner_radius=2;ring.outer_radius=3;ring.gap_half_width_degrees=12;ring.lifetime01=.5f;ring.gap_angles_degrees={5,81};input.payload=ring;
    auto run=[&] {return hs::runtime_detail::BuildVfxTypedGroundCommands(cooked,std::span(&input,1),1);};
    Require(run().size()==2,"valid ring fixture rejected");
    auto &payload=std::get<hs::VfxRingGapsPayload>(input.payload);
    payload.inner_radius=-1;Require(run().empty(),"negative inner radius accepted");payload.inner_radius=3;Require(run().empty(),"empty annulus accepted");payload.inner_radius=2;
    payload.gap_half_width_degrees=181;Require(run().empty(),"invalid gap width accepted");payload.gap_half_width_degrees=12;
    payload.gap_angles_degrees[0]=std::numeric_limits<float>::infinity();Require(run().empty(),"nonfinite gap angle accepted");payload.gap_angles_degrees[0]=5;
    cooked.parameters[0].bits=std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());
    Require(run().size()==1,"invalid world edge parameter was executed");
    cooked.outputs[4].motion_rate_hz=std::numeric_limits<float>::infinity();Require(run().empty(),"nonfinite motion parameter accepted");
    input.payload=hs::VfxCirclePayload{};Require(run().empty(),"Circle payload accepted as ring-with-gaps");
}
void CircleWarningUsesOwnerClockAndExactRadius()
{
    auto cooked=Program();cooked.effects[0].input_mode=0;cooked.effects[0].payload_kind=Hash32("CircleAreaPayload");
    cooked.effects[0].timing_kind=1;cooked.effects[0].seconds=0;cooked.effects[0].sources={0,1};cooked.sources[0].outputs={0,2};
    cooked.outputs[0].shape=Hash32("exact_circle_ring");cooked.outputs[0].motion=hs::VfxMotionKind::BoundaryPulseAccelerates;cooked.outputs[0].parameters={0,2};
    cooked.parameters={{Hash32("edge_width_world"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(.065f)},
        {Hash32("pulse_curve_ref"),hs::VfxParameterType::CurveRow,0},{Hash32("tick_count"),hs::VfxParameterType::Int,12}};
    cooked.curves.push_back({0,0,{0,2}});cooked.curve_keys={{0,1.2f},{1,4}};
    cooked.outputs[1]=cooked.outputs[0];cooked.outputs[1].shape=Hash32("radial_ticks_sdf");
    cooked.outputs[1].profile=hs::VfxOutputProfile::GroundSdfOit;cooked.outputs[1].motion=hs::VfxMotionKind::TicksRotateLock;cooked.outputs[1].parameters={2,1};
    hs::VfxEventInput input;input.effect_handle=1;input.sequence=711;input.event_tick=100;
    input.geometry_owner_id=99;input.geometry_start_tick=100;input.geometry_end_tick=220;
    hs::VfxCirclePayload circle;circle.center={7,.2f,-4};circle.radius=3.7f;input.payload=circle;
    const auto run=[&](hs::Tick tick){return hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked,std::span(&input,1),tick);};
    auto c=run(160);Require(c.size()==2&&c[0].geometry.shape==hs::VfxGroundShape::CirclePreviewBorder&&
        c[1].geometry.shape==hs::VfxGroundShape::CirclePreviewTicks,"owned circle warning shape pair missing");
    Require(c[0].stable_id==99&&c[1].stable_id==99&&Near(c[0].geometry.outer_radius,3.7f)&&c[0].geometry.inner_radius==0&&
        c[0].geometry.gap_angles_radians.empty()&&Near(c[0].geometry.edge_width,.065f)&&Near(c[0].command.start_size_max,3.765f),
        "circle owner identity or exact boundary changed");
    Require(Near(c[0].geometry.animation_phase,1.9f)&&Near(c[0].geometry.progress,.5f)&&Near(c[0].lifetime01,.5f)&&
        Near(c[0].command.lifetime_max,2)&&c[1].geometry.tick_count==12,"circle pulse did not integrate actual owner duration");
    const auto before_lock=run(195),at_lock=run(196),after_lock=run(208);
    Require(before_lock.size()==2&&at_lock.size()==2&&after_lock.size()==2&&before_lock[1].geometry.progress<.8f&&
        Near(at_lock[1].geometry.progress,.8f)&&Near(after_lock[1].geometry.progress,.9f)&&
        at_lock[1].geometry.tick_count==after_lock[1].geometry.tick_count&&
        Near(at_lock[1].geometry.outer_radius,after_lock[1].geometry.outer_radius),
        "final-20-percent lock shader contract did not receive stable geometry and owner progress");
    Require(run(99).empty()&&run(220).empty(),"circle warning escaped owner lifetime");
    input.geometry_owner_id=0;Require(run(160).empty(),"gameplay circle invented missing owner lifetime");
    cooked.effects[0].timing_kind=0;cooked.effects[0].seconds=2;
    c=run(160);Require(c.size()==2&&c[0].stable_id==711&&Near(c[0].geometry.animation_phase,1.9f),"fixed circle warning lost authored duration or event identity");
    Require(run(220).empty(),"fixed circle survived authored duration");input.geometry_owner_id=99;
    std::get<hs::VfxCirclePayload>(input.payload).inner_radius=1;Require(run(160).empty(),"circle preview coerced annulus");input.payload=circle;
    cooked.sources[0].parameters={0,1};Require(run(160).empty(),"circle preview ignored unknown source parameters");cooked.sources[0].parameters={0,0};
    cooked.parameters[2].type=hs::VfxParameterType::Float;Require(run(160).size()==1,"circle ticks accepted wrong count type");
}

void OwnedRingPreviewUsesIntegratedPulse()
{
    auto cooked=Program(); cooked.effects[0].input_mode=0; cooked.effects[0].payload_kind=Hash32("RingWithGapsPayload");
    cooked.effects[0].seconds=.1f; cooked.effects[0].sources={0,1}; cooked.sources[0].outputs={0,2};
    cooked.outputs[0].shape=Hash32("exact_circle_ring"); cooked.outputs[0].motion=hs::VfxMotionKind::BoundaryPulseAccelerates;
    cooked.outputs[0].parameters={0,2};
    cooked.parameters={{Hash32("edge_width_world"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(.065f)},
        {Hash32("pulse_curve_ref"),hs::VfxParameterType::CurveRow,0}, {Hash32("tick_count"),hs::VfxParameterType::Int,12}};
    cooked.curves.push_back({0,0,{0,2}}); cooked.curve_keys={{0,1.2f},{1,4}};
    cooked.outputs[1]=cooked.outputs[0]; cooked.outputs[1].shape=Hash32("radial_ticks_sdf");
    cooked.outputs[1].profile=hs::VfxOutputProfile::GroundSdfOit; cooked.outputs[1].motion=hs::VfxMotionKind::TicksRotateLock;
    cooked.outputs[1].parameters={2,1};
    hs::VfxEventInput input; input.effect_handle=1; input.event_tick=100; input.geometry_owner_id=99;
    input.geometry_start_tick=100; input.geometry_end_tick=220;
    hs::VfxRingGapsPayload ring; ring.center={7,.2f,-4};ring.inner_radius=3;ring.outer_radius=17;ring.gap_half_width_degrees=11;ring.gap_angles_degrees={-13,51,244}; input.payload=ring;
    auto run=[&](hs::Tick tick){return hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked,std::span(&input,1),tick);};
    auto commands=run(160);
    Require(commands.size()==2&&commands[0].geometry.shape==hs::VfxGroundShape::RingGapsPreviewBorder&&
            commands[1].geometry.shape==hs::VfxGroundShape::RingGapsTicks,"preview shape pair missing");
    Require(Near(commands[0].geometry.animation_phase,1.9f)&&Near(commands[0].geometry.progress,.5f)&&
            Near(commands[0].command.lifetime_max,2),"pulse used nominal duration or instantaneous frequency times age");
    Require(commands[1].geometry.tick_count==12&&Near(commands[0].geometry.inner_radius,3)&&Near(commands[0].geometry.outer_radius,17)&&
            commands[0].geometry.gap_angles_radians.size()==3&&Near(commands[0].geometry.edge_width,.065f),"preview boundaries, gaps or count lost");
    Require(run(220).empty(),"preview survived authoritative end");
    // Piecewise integral across a bend: area [0,.25]=.5; [.25,.5]=.625, times 2 seconds.
    cooked.curves[0].keys={0,3}; cooked.curve_keys={{0,1},{.25f,3},{1,0}};
    commands=run(160);Require(commands.size()==2&&Near(commands[0].geometry.animation_phase,2.25f),"piecewise linear curve integral incorrect");
    cooked.curves[0].interpolation=2;Require(run(160).size()==1,"unsupported cubic curve approximated");cooked.curves[0].interpolation=0;
    cooked.curve_keys[1].time=0;Require(run(160).size()==1,"duplicate curve knots accepted");cooked.curve_keys[1].time=.25f;
    cooked.parameters[2].bits=0;Require(run(160).size()==1,"zero tick count accepted");cooked.parameters[2].bits=12;
    cooked.parameters[0].bits=std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());
    Require(run(160).size()==1,"nonfinite preview edge accepted");cooked.parameters[0].bits=std::bit_cast<std::uint32_t>(.065f);
    cooked.parameters[1].type=hs::VfxParameterType::Int;Require(run(160).size()==1,"wrong curve parameter type accepted");cooked.parameters[1].type=hs::VfxParameterType::CurveRow;
    cooked.outputs[0].profile=hs::VfxOutputProfile::GroundSdfAdd;Require(run(160).size()==1,"wrong preview profile accepted");cooked.outputs[0].profile=hs::VfxOutputProfile::GroundSdfSoft;
    cooked.outputs[0].shape=Hash32("exact_hitbox_ring");cooked.outputs[0].motion=hs::VfxMotionKind::StableBoundaryInwardPulse;
    Require(run(160).size()==1,"event warning accepted active wavefront shape");
    cooked.effects[0].input_mode=1;Require(run(160).empty(),"persistent recipe executed as warning event");cooked.effects[0].input_mode=0;
    input.geometry_owner_id=0;Require(run(160).empty(),"unowned preview accepted");input.geometry_owner_id=99;
    input.payload=hs::VfxCirclePayload{};Require(run(160).empty(),"preview ring gaps coerced to Circle");
}
void SafeSectorPreservesScheduledFootprint()
{
    auto cooked=Program(); auto &effect=cooked.effects[0];
    effect.input_mode=0; effect.timing_kind=1; effect.seconds=0;
    cooked.effect_lookup.push_back({9075101668763271276ull,1});
    effect.payload_kind=Hash32("SafeGapSectorPayload"); effect.sources={0,1};
    cooked.sources[0].stable_id=Hash32("main_rune"); cooked.sources[0].outputs={0,1};
    auto &output=cooked.outputs[0]; output.shape=Hash32("polar_rune_sdf");
    output.motion=hs::VfxMotionKind::SlowCounterRotationStableFootprint; output.parameters={0,3};
    cooked.parameters={{Hash32("spokes"),hs::VfxParameterType::Int,6},
        {Hash32("ring_count"),hs::VfxParameterType::Int,2},
        {Hash32("rotation_speed"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(.3f)}};
    hs::VfxEventInput input; input.effect_handle=1;input.sequence=82;input.event_tick=100;
    input.geometry_owner_id=7;input.geometry_start_tick=100;input.geometry_end_tick=280;
    input.payload=hs::VfxSafeSectorPayload{{4,.3f,-9},{3,5,4},7,19,17,.99f};
    auto run=[&](hs::Tick tick){return hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked,std::span(&input,1),tick);};
    auto commands=run(190);Require(commands.size()==1,"safe-sector output missing");
    const auto &item=commands[0];const auto &g=item.geometry;
    Require(g.shape==hs::VfxGroundShape::SafeSectorMarker && Near(g.inner_radius,7)&&Near(g.outer_radius,19)&&Near(g.range,19)&&
        Near(g.direction.x,.6f)&&Near(g.direction.y,0)&&Near(g.direction.z,.8f)&&Near(g.half_angle_radians,17*3.14159265358979323846f/180),
        "safe-sector bounds or direction changed");
    Require(g.spokes==6&&g.rings==2&&Near(g.animation_phase,.45f)&&Near(g.progress,.5f)&&Near(g.edge_width,.035f)&&
        Near(item.command.lifetime_max,3)&&Near(item.normalized_age,.5f)&&Near(item.command.position.z,-9),"safe-sector timing or rune parameters lost");
    Require(item.command.primitive==hs::VfxPrimitive::ExactRing&&item.gradient_row==17&&Near(item.hdr,2.5f)&&Near(item.command.start_color.x,.5f),"safe-sector material lost");
    Require(run(99).empty()&&run(280).empty(),"safe-sector outside scheduled interval");
    cooked.sources[0].knot_count=4;cooked.sources[0].knots={0,.2f,.4f,1};
    commands=run(190);Require(commands.size()==1&&Near(commands[0].command.start_color.w,.8f*5/6),"safe-sector source envelope ignored");
    output.min_quality=2;input.quality=hs::VfxQuality::Low;Require(run(190).empty(),"safe-sector quality ignored");input.quality=hs::VfxQuality::High;
    auto &sector=std::get<hs::VfxSafeSectorPayload>(input.payload);
    sector.direction={0,1,0};Require(run(190).empty(),"zero horizontal direction accepted");sector.direction={3,0,4};
    sector.inner_radius=19;Require(run(190).empty(),"empty sector accepted");sector.inner_radius=7;
    sector.half_angle_degrees=181;Require(run(190).empty(),"oversized safe-sector angle accepted");sector.half_angle_degrees=17;
    cooked.parameters[0].bits=0;Require(run(190).empty(),"zero spokes accepted");cooked.parameters[0].bits=6;
    cooked.parameters[1].type=hs::VfxParameterType::Float;Require(run(190).empty(),"wrong ring-count type accepted");cooked.parameters[1].type=hs::VfxParameterType::Int;
    cooked.parameters[2].bits=std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity());Require(run(190).empty(),"nonfinite rotation accepted");
    cooked.parameters[2].bits=std::bit_cast<std::uint32_t>(.3f);
    output.shape=Hash32("exact_hitbox_ring");Require(run(190).empty(),"safe-sector coerced to full ring");output.shape=Hash32("polar_rune_sdf");
    effect.timing_kind=0;Require(run(190).empty(),"fixed lifetime accepted as gameplay sector");effect.timing_kind=1;
    input.geometry_owner_id=0;Require(run(190).empty(),"unowned safe-sector accepted");input.geometry_owner_id=7;
    input.geometry_end_tick=100;Require(run(190).empty(),"invalid owner interval accepted");input.geometry_end_tick=280;
    input.payload=hs::VfxCirclePayload{};Require(run(190).empty(),"safe-sector schema accepted Circle");
}

hs::VfxProgramData ExpandingRingProgram(bool circle_payload = false)
{
    hs::VfxProgramData cooked;
    hs::VfxEffectRecord effect;
    effect.handle = 1;
    effect.input_mode = 0;
    effect.payload_kind = Hash32(circle_payload ? "CircleAreaPayload" : "PointEventPayload");
    effect.timing_kind = 0;
    effect.seconds = 1.0f;
    effect.sources = {0, 1};
    cooked.effects.push_back(effect);
    hs::VfxSourceRecord source;
    source.effect = 1;
    source.type = hs::VfxSourceType::Direct;
    source.knot_count = 2;
    source.knots = {0.05f, 0.55f, 0, 0};
    source.outputs = {0, 1};
    cooked.sources.push_back(source);
    hs::VfxOutputRecord output;
    output.source = 0;
    output.profile = hs::VfxOutputProfile::GroundSdfAdd;
    output.shape = Hash32("expanding_ring_sdf");
    output.motion = hs::VfxMotionKind::FastExpansionSurface;
    output.rgba = {0.2f, 0.4f, 0.6f, 0.55f};
    output.hdr = 2.5f;
    output.gradient_row = 17;
    output.parameters = {0, 3};
    cooked.outputs.push_back(output);
    cooked.parameters = {
        {Hash32("start_radius"), hs::VfxParameterType::Float, std::bit_cast<std::uint32_t>(0.15f)},
        {Hash32("end_radius"), hs::VfxParameterType::Float, std::bit_cast<std::uint32_t>(1.25f)},
        {Hash32("edge_width"), hs::VfxParameterType::Float, std::bit_cast<std::uint32_t>(0.07f)}};
    return cooked;
}

hs::VfxEventInput ExpandingRingEvent(bool circle_payload = false)
{
    hs::VfxEventInput input;
    input.effect_handle = 1;
    input.event_tick = 100;
    input.sequence = 0x1234;
    input.stable_seed = 0xdeadbeef;
    if (circle_payload)
    {
        hs::VfxCirclePayload circle;
        circle.center = {4, 0.25f, -3};
        circle.radius = 3.0f;
        input.payload = circle;
    }
    else
    {
        hs::VfxPointPayload point;
        point.position = {4, 0.25f, -3};
        point.authored_scale = 2.0f;
        input.payload = point;
    }
    return input;
}

void ExpandingRingUsesAuthoredSourceIntervalAndPayloadScale()
{
    auto cooked = ExpandingRingProgram();
    auto event = ExpandingRingEvent();
    const auto run = [&](hs::Tick tick) {
        return hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked, std::span(&event, 1), tick);
    };
    Require(run(102).empty(), "expanding ring appeared before source start");
    auto commands = run(103);
    Require(commands.size() == 1, "direct additive ground output missing");
    const auto &first = commands[0];
    Require(first.additive && first.command.primitive == hs::VfxPrimitive::ExactRing &&
            first.geometry.shape == hs::VfxGroundShape::Circle &&
            first.command.renderer == hs::VfxRenderer::Ground,
            "additive exact ring rendering contract lost");
    Require(first.command.position.x == 4 && first.command.position.y == .25f &&
            first.command.position.z == -3 && first.stable_id == event.sequence &&
            first.command.seed == event.stable_seed,
            "event center or identity lost");
    Require(Near(first.geometry.progress, 0) && Near(first.geometry.outer_radius, .30f) &&
            Near(first.edge_width_world, .07f) && Near(first.command.start_size_min, .37f) &&
            Near(first.command.stretch * first.command.start_size_min, .30f),
            "authored start radius or world edge width changed");
    Require(Near(first.command.start_color.x, .5f) && Near(first.command.start_color.y, 1.0f) &&
            Near(first.command.start_color.z, 1.5f) && Near(first.command.start_color.w, .55f) &&
            first.gradient_row == 17 && Near(first.hdr, 2.5f),
            "authored HDR, alpha, or gradient lost");
    commands = run(18 + 100);
    Require(commands.size() == 1 && Near(commands[0].geometry.progress, .5f) &&
            Near(commands[0].geometry.outer_radius, 1.4f) &&
            Near(commands[0].command.start_size_min, 1.47f) &&
            Near(commands[0].normalized_age, .3f),
            "expansion used effect age instead of normalized source interval");
    Require(run(133).empty(), "expanding ring survived its exclusive source endpoint");
    cooked = ExpandingRingProgram(true);
    event = ExpandingRingEvent(true);
    commands = run(118);
    Require(commands.size() == 1 && Near(commands[0].geometry.outer_radius, 2.1f) &&
            Near(commands[0].command.stretch * commands[0].command.start_size_min, 2.1f),
            "CircleAreaPayload gameplay radius was not used as the authored scale");
}

void ExpandingRingRejectsWrongContractAndMalformedParams()
{
    auto cooked = ExpandingRingProgram();
    auto event = ExpandingRingEvent();
    const auto run = [&] {
        return hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked, std::span(&event, 1), 118);
    };
    cooked.outputs[0].profile = hs::VfxOutputProfile::GroundSdfSoft;
    Require(run().empty(), "wrong expanding-ring profile accepted");
    cooked.outputs[0].profile = hs::VfxOutputProfile::GroundSdfAdd;
    cooked.outputs[0].shape = Hash32("exact_hitbox_ring");
    Require(run().empty(), "wrong expanding-ring shape accepted");
    cooked.outputs[0].shape = Hash32("expanding_ring_sdf");
    cooked.outputs[0].motion = hs::VfxMotionKind::SlowRadialFlow;
    Require(run().empty(), "wrong expanding-ring motion accepted");
    cooked.outputs[0].motion = hs::VfxMotionKind::FastExpansionSurface;
    for (std::size_t index = 0; index < 3; ++index)
    {
        const auto saved = cooked.parameters[index].bits;
        cooked.parameters[index].bits = std::bit_cast<std::uint32_t>(0.0f);
        Require(run().empty(), "zero authored ring parameter accepted");
        cooked.parameters[index].bits = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());
        Require(run().empty(), "nonfinite authored ring parameter accepted");
        cooked.parameters[index].bits = saved;
    }
    cooked.parameters[1].bits = std::bit_cast<std::uint32_t>(.14f);
    Require(run().empty(), "reversed expanding radius range accepted");
    cooked.parameters[1].bits = std::bit_cast<std::uint32_t>(1.25f);
    cooked.parameters[0].type = hs::VfxParameterType::Int;
    Require(run().empty(), "wrong authored parameter type accepted");
    cooked.parameters[0].type = hs::VfxParameterType::Float;
    event.payload = hs::VfxCirclePayload{};
    Require(run().empty(), "point recipe accepted a CircleAreaPayload");
    event = ExpandingRingEvent();
    std::get<hs::VfxPointPayload>(event.payload).authored_scale = 0;
    Require(run().empty(), "zero event scale accepted");
    event = ExpandingRingEvent();
    cooked.sources[0].knots[1] = cooked.sources[0].knots[0];
    Require(run().empty(), "empty source interval accepted");
}

void CookedExpandingRingsConvertWhenContentIsAvailable()
{
    const auto path = std::filesystem::path("Cooked") / "vfx_program.hsbin";
    if (!std::filesystem::exists(path)) return; // A direct test-target build need not cook content.
    std::ifstream stream(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    hs::VfxProgramData cooked;
    std::string error;
    Require(hs::LoadVfxProgram(std::as_bytes(std::span(bytes)), cooked, error),
            "the cooked VFX program could not be loaded");
    std::size_t authored_count{};
    std::size_t converted_count{};
    for (const auto &effect : cooked.effects)
    {
        std::size_t effect_count{};
        for (std::size_t source_index = effect.sources.first;
             source_index < static_cast<std::size_t>(effect.sources.first) + effect.sources.count; ++source_index)
        {
            const auto &source = cooked.sources[source_index];
            if (source.type != hs::VfxSourceType::Direct) continue;
            for (std::size_t output_index = source.outputs.first;
                 output_index < static_cast<std::size_t>(source.outputs.first) + source.outputs.count; ++output_index)
            {
                const auto &output = cooked.outputs[output_index];
                effect_count += output.profile == hs::VfxOutputProfile::GroundSdfAdd &&
                    output.shape == Hash32("expanding_ring_sdf") &&
                    output.motion == hs::VfxMotionKind::FastExpansionSurface;
            }
        }
        if (!effect_count) continue;
        authored_count += effect_count;
        hs::VfxEventInput event;
        event.effect_handle = effect.handle;
        event.event_tick = 100;
        event.sequence = effect.handle;
        if (effect.payload_kind == Hash32("PointEventPayload"))
            event.payload = hs::VfxPointPayload{{4, .25f, -3}, {}, {}, 2.0f};
        else if (effect.payload_kind == Hash32("CircleAreaPayload"))
            event.payload = hs::VfxCirclePayload{{4, .25f, -3}, {}, 3.0f};
        else
            throw std::runtime_error("cooked expanding ring has an unsupported payload schema");
        const auto tick = event.event_tick + static_cast<hs::Tick>(std::lround(.3f * effect.seconds * 60.0f));
        const auto commands = hs::runtime_detail::BuildVfxTypedEventGroundCommands(
            cooked, std::span(&event, 1), tick);
        converted_count += std::count_if(commands.begin(), commands.end(),
            [](const auto &item) { return item.additive; });
    }
    Require(authored_count == 11 && converted_count == authored_count,
            "the eleven cooked expanding-ring outputs did not all convert");
}

void CookedRetreatShockwaveCircleStaysWithinGameplayRadiusWhenContentIsAvailable()
{
    const auto path = std::filesystem::path("Cooked") / "vfx_program.hsbin";
    if (!std::filesystem::exists(path)) return; // A direct test-target build need not cook content.
    std::ifstream stream(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(stream),
                                  std::istreambuf_iterator<char>()};
    hs::VfxProgramData cooked;
    std::string error;
    Require(hs::LoadVfxProgram(std::as_bytes(std::span(bytes)), cooked, error),
            "the cooked VFX program could not be loaded for retreat shockwave");

    constexpr float gameplay_radius = 5.0f;
    std::size_t matching_effects{};
    for (const auto &lookup : cooked.effect_lookup)
    {
        if (lookup.effect_id != Hash64("particle.upgrade.retreat.land_shockwave")) continue;
        ++matching_effects;
        Require(lookup.handle > 0 && lookup.handle <= cooked.effects.size(),
                "cooked retreat shockwave lookup has invalid handle");
        const auto &effect = cooked.effects[lookup.handle - 1];
        Require(effect.input_mode == 0 && effect.payload_kind == Hash32("CircleAreaPayload"),
                "cooked retreat shockwave does not use a CircleAreaPayload event");
        Require(effect.seconds > 0.0f && std::isfinite(effect.seconds),
                "cooked retreat shockwave has an invalid duration");

        const auto source_first = static_cast<std::size_t>(effect.sources.first);
        const auto source_count = static_cast<std::size_t>(effect.sources.count);
        Require(source_first <= cooked.sources.size() &&
                    source_count <= cooked.sources.size() - source_first,
                "cooked retreat shockwave has an invalid source range");

        std::size_t shock_ring_outputs{};
        for (std::size_t source_index = source_first;
             source_index < source_first + source_count; ++source_index)
        {
            const auto &source = cooked.sources[source_index];
            if (source.type != hs::VfxSourceType::Direct) continue;
            const auto output_first = static_cast<std::size_t>(source.outputs.first);
            const auto output_count = static_cast<std::size_t>(source.outputs.count);
            Require(output_first <= cooked.outputs.size() &&
                        output_count <= cooked.outputs.size() - output_first,
                    "cooked retreat shockwave has an invalid output range");
            for (std::size_t output_index = output_first;
                 output_index < output_first + output_count; ++output_index)
            {
                const auto &output = cooked.outputs[output_index];
                if (output.source != source_index ||
                    output.profile != hs::VfxOutputProfile::GroundSdfAdd ||
                    output.shape != Hash32("expanding_ring_sdf") ||
                    output.motion != hs::VfxMotionKind::FastExpansionSurface)
                    continue;
                ++shock_ring_outputs;
                Require(source.knot_count == 2 && std::isfinite(source.knots[0]) &&
                            std::isfinite(source.knots[1]) && source.knots[1] > source.knots[0],
                        "cooked retreat shockwave has an invalid source interval");

                const auto parameter_first = static_cast<std::size_t>(output.parameters.first);
                const auto parameter_count = static_cast<std::size_t>(output.parameters.count);
                Require(parameter_first <= cooked.parameters.size() &&
                            parameter_count <= cooked.parameters.size() - parameter_first,
                        "cooked retreat shockwave has an invalid ring parameter range");
                const auto end_parameter = std::find_if(
                    cooked.parameters.begin() + parameter_first,
                    cooked.parameters.begin() + parameter_first + parameter_count,
                    [](const auto &parameter) {
                        return parameter.key == Hash32("end_radius") &&
                            parameter.type == hs::VfxParameterType::Float;
                    });
                Require(end_parameter != cooked.parameters.begin() + parameter_first + parameter_count,
                        "cooked retreat shockwave ring has no float end_radius");
                Require(std::bit_cast<float>(end_parameter->bits) == 1.0f,
                        "retreat shockwave ring end_radius must be exactly 1.0");

                hs::VfxEventInput event;
                event.effect_handle = lookup.handle;
                event.event_tick = 100;
                event.sequence = 0x1234;
                event.stable_seed = 0xdeadbeef;
                event.payload = hs::VfxCirclePayload{{4.0f, 0.25f, -3.0f}, {},
                                                     gameplay_radius};
                const auto near_end_tick = event.event_tick +
                    static_cast<hs::Tick>(std::ceil(effect.seconds * source.knots[1] * 60.0f)) - 1;
                const auto commands = hs::runtime_detail::BuildVfxTypedEventGroundCommands(
                    cooked, std::span(&event, 1), near_end_tick);
                const auto converted = std::find_if(commands.begin(), commands.end(),
                    [](const auto &item) {
                        return item.additive && item.geometry.shape == hs::VfxGroundShape::Circle;
                    });
                Require(converted != commands.end(),
                        "cooked retreat shockwave Circle output did not convert near ring end");
                Require(converted->geometry.progress > 0.9f &&
                            converted->geometry.outer_radius <= gameplay_radius + 0.00001f,
                        "retreat shockwave Circle radius exceeded gameplay radius near ring end");
            }
        }
        Require(shock_ring_outputs == 1,
                "cooked retreat shockwave must contain one expanding shock_ring output");
    }
    Require(matching_effects == 1,
            "cooked retreat shockwave effect lookup is missing or duplicated");
}

hs::VfxProgramData HexProgram(bool context_payload = false, bool with_mask = true)
{
    hs::VfxProgramData cooked;
    hs::VfxEffectRecord effect;
    effect.handle = 1;
    effect.input_mode = 0;
    effect.payload_kind = Hash32(context_payload ? "PresentationContextPayload" : "PointEventPayload");
    effect.timing_kind = 0;
    effect.seconds = 1.0f;
    effect.sources = {0, 1};
    cooked.effects.push_back(effect);
    hs::VfxSourceRecord source;
    source.effect = 1;
    source.type = hs::VfxSourceType::Direct;
    source.knot_count = 2;
    source.knots = {0, .65f, 0, 0};
    source.outputs = {0, 1};
    cooked.sources.push_back(source);
    hs::VfxOutputRecord output;
    output.source = 0;
    output.profile = hs::VfxOutputProfile::GroundSdfAdd;
    output.shape = Hash32("hex_constellation_sdf");
    output.motion = hs::VfxMotionKind::HexSegmentsCollapse;
    output.rgba = {.2f, .4f, .6f, .72f};
    output.hdr = 3.0f;
    output.gradient_row = 13;
    output.parameters = {0, 2};
    output.textures = {0, with_mask ? 1u : 0u};
    cooked.outputs.push_back(output);
    cooked.parameters = {
        {Hash32("segments"), hs::VfxParameterType::Int, 6},
        {Hash32("sequence"), hs::VfxParameterType::Bool, 1}};
    if (with_mask)
    {
        hs::VfxTextureBindingRecord binding;
        binding.role = Hash32("authored_mask_array");
        binding.catalog_slot = 9;
        binding.selection = Hash32("stable_seed_mod_group_size");
        binding.strength = .58f;
        binding.slices = {0, 2};
        cooked.texture_bindings.push_back(binding);
        cooked.slice_indices = {5, 7};
        constexpr std::string_view path = "Content/Textures/VFX/vfx_authored_mask_array.dds";
        cooked.texture_resources.push_back({9, {0, static_cast<std::uint32_t>(path.size())}});
        for (char character : path) cooked.strings.push_back(static_cast<std::byte>(character));
    }
    return cooked;
}

hs::VfxEventInput HexEvent(bool context_payload = false)
{
    hs::VfxEventInput input;
    input.effect_handle = 1;
    input.event_tick = 100;
    input.sequence = 0x1234;
    input.stable_seed = 5;
    if (context_payload)
    {
        input.payload = hs::VfxContextPayload{};
        input.world_transform[12] = 4;
        input.world_transform[13] = 6;
        input.world_transform[14] = -3;
    }
    else
    {
        hs::VfxPointPayload point;
        point.position = {4, 6, -3};
        point.authored_scale = 2;
        input.payload = point;
    }
    return input;
}

void HexConstellationPreservesGeometryMaterialAndMask()
{
    auto cooked = HexProgram();
    auto event = HexEvent();
    const auto run = [&](hs::Tick tick) {
        return hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked, std::span(&event, 1), tick);
    };
    auto commands = run(119);
    Require(commands.size() == 1, "authored hex output missing");
    const auto &item = commands[0];
    Require(item.additive && item.geometry.shape == hs::VfxGroundShape::HexConstellation &&
            item.command.primitive == hs::VfxPrimitive::ExactRing &&
            item.command.renderer == hs::VfxRenderer::Ground,
            "hex additive rendering contract lost");
    Require(item.command.position.x == 4 && Near(item.command.position.y, .025f) &&
            item.command.position.z == -3 && Near(item.geometry.outer_radius, 1.3f) &&
            Near(item.geometry.edge_width, .05f) && Near(item.command.start_size_min, 1.35f) &&
            Near(item.command.stretch * item.command.start_size_min, 1.3f),
            "hex footprint or ground anchor changed");
    Require(item.geometry.spokes == 6 && Near(item.geometry.progress, (19.0f / 60.0f) / .65f) &&
            Near(item.geometry.animation_phase, 1.0f) && item.geometry.mask_slice == 7 &&
            Near(item.geometry.mask_strength, .58f),
            "hex segments, source progress, sequence, or authored mask lost");
    Require(Near(item.command.start_color.x, .6f) && Near(item.command.start_color.y, 1.2f) &&
            Near(item.command.start_color.z, 1.8f) && Near(item.command.start_color.w, .72f) &&
            item.gradient_row == 13 && Near(item.hdr, 3.0f) &&
            item.stable_id == event.sequence && item.command.seed == event.stable_seed,
            "hex color, HDR, gradient, or identity lost");
    Require(run(139).empty(), "hex survived exclusive source endpoint");
    cooked = HexProgram(true, false);
    event = HexEvent(true);
    commands = run(119);
    Require(commands.size() == 1 && Near(commands[0].geometry.outer_radius, .65f) &&
            commands[0].geometry.mask_slice == 0xffffffffu &&
            Near(commands[0].geometry.mask_strength, 0) &&
            commands[0].command.position.x == 4 && commands[0].command.position.z == -3 &&
            Near(commands[0].command.position.y, .025f),
            "context transform or unmasked hex defaults changed");
}

void HexConstellationRejectsMalformedContract()
{
    auto cooked = HexProgram();
    auto event = HexEvent();
    const auto run = [&] {
        return hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked, std::span(&event, 1), 119);
    };
    cooked.outputs[0].profile = hs::VfxOutputProfile::GroundSdfSoft;
    Require(run().empty(), "wrong hex profile accepted");
    cooked.outputs[0].profile = hs::VfxOutputProfile::GroundSdfAdd;
    cooked.outputs[0].shape = Hash32("expanding_ring_sdf");
    Require(run().empty(), "wrong hex shape accepted");
    cooked.outputs[0].shape = Hash32("hex_constellation_sdf");
    cooked.outputs[0].motion = hs::VfxMotionKind::FastExpansionSurface;
    Require(run().empty(), "wrong hex motion accepted");
    cooked.outputs[0].motion = hs::VfxMotionKind::HexSegmentsCollapse;
    cooked.parameters[0].bits = 5;
    Require(run().empty(), "unsupported hex segment count accepted");
    cooked.parameters[0].bits = 6;
    cooked.parameters[1].bits = 2;
    Require(run().empty(), "nonboolean sequence accepted");
    cooked.parameters[1].bits = 1;
    cooked.texture_bindings[0].strength = std::numeric_limits<float>::quiet_NaN();
    Require(run().empty(), "nonfinite authored mask strength accepted");
    cooked.texture_bindings[0].strength = .58f;
    cooked.slice_indices[1] = 12;
    Require(run().empty(), "out of range authored mask slice accepted");
    cooked.slice_indices[1] = 7;
    std::get<hs::VfxPointPayload>(event.payload).authored_scale = 0;
    Require(run().empty(), "zero hex scale accepted");
    event = HexEvent();
    event.payload = hs::VfxContextPayload{};
    Require(run().empty(), "point hex recipe accepted context payload");
    event = HexEvent();
    cooked.sources[0].knots[1] = 0;
    Require(run().empty(), "empty hex source interval accepted");
}

void CookedHexConstellationsConvertWhenContentIsAvailable()
{
    const auto path = std::filesystem::path("Cooked") / "vfx_program.hsbin";
    if (!std::filesystem::exists(path)) return;
    std::ifstream stream(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    hs::VfxProgramData cooked;
    std::string error;
    Require(hs::LoadVfxProgram(std::as_bytes(std::span(bytes)), cooked, error),
            "the cooked VFX program could not be loaded for hex coverage");
    std::size_t authored_count{};
    std::size_t masked_count{};
    std::size_t converted_count{};
    for (const auto &effect : cooked.effects)
    {
        std::size_t effect_count{};
        for (std::size_t source_index = effect.sources.first;
             source_index < static_cast<std::size_t>(effect.sources.first) + effect.sources.count; ++source_index)
        {
            const auto &source = cooked.sources[source_index];
            if (source.type != hs::VfxSourceType::Direct) continue;
            for (std::size_t output_index = source.outputs.first;
                 output_index < static_cast<std::size_t>(source.outputs.first) + source.outputs.count; ++output_index)
            {
                const auto &output = cooked.outputs[output_index];
                effect_count += output.profile == hs::VfxOutputProfile::GroundSdfAdd &&
                    output.shape == Hash32("hex_constellation_sdf") &&
                    output.motion == hs::VfxMotionKind::HexSegmentsCollapse;
            }
        }
        if (!effect_count) continue;
        authored_count += effect_count;
        hs::VfxEventInput event;
        event.effect_handle = effect.handle;
        event.event_tick = 100;
        event.sequence = effect.handle;
        event.stable_seed = 5;
        if (effect.payload_kind == Hash32("PointEventPayload"))
            event.payload = hs::VfxPointPayload{{4, 6, -3}, {}, {}, 2};
        else if (effect.payload_kind == Hash32("PresentationContextPayload"))
        {
            event.payload = hs::VfxContextPayload{};
            event.world_transform[12] = 4;
            event.world_transform[13] = 6;
            event.world_transform[14] = -3;
        }
        else
            throw std::runtime_error("cooked hex has an unsupported payload schema");
        const auto tick = event.event_tick + static_cast<hs::Tick>(std::lround(.3f * effect.seconds * 60.0f));
        const auto commands = hs::runtime_detail::BuildVfxTypedEventGroundCommands(
            cooked, std::span(&event, 1), tick);
        converted_count += std::count_if(commands.begin(), commands.end(), [&](const auto &item) {
            if (item.geometry.shape != hs::VfxGroundShape::HexConstellation || !item.additive) return false;
            masked_count += item.geometry.mask_slice != 0xffffffffu;
            return true;
        });
    }
    Require(authored_count == 19 && converted_count == authored_count && masked_count == 17,
            "the nineteen cooked hex outputs or seventeen authored masks did not all convert");
}

hs::VfxProgramData BossSignatureProgram(std::string_view schema)
{
    hs::VfxProgramData cooked;
    hs::VfxEffectRecord effect;
    effect.handle = 1;
    effect.input_mode = 0;
    effect.payload_kind = Hash32(schema);
    effect.timing_kind = 0;
    effect.seconds = 1.0f;
    effect.sources = {0, 1};
    cooked.effects.push_back(effect);
    hs::VfxSourceRecord source;
    source.effect = 1;
    source.type = hs::VfxSourceType::Direct;
    source.knot_count = 2;
    source.knots = {0, .45f, 0, 0};
    source.outputs = {0, 1};
    cooked.sources.push_back(source);
    hs::VfxOutputRecord output;
    output.source = 0;
    output.profile = hs::VfxOutputProfile::GroundSdfAdd;
    output.shape = Hash32("large_signature_sdf");
    output.motion = hs::VfxMotionKind::StrongSnapGameplayResolve;
    output.rgba = {.2f, .4f, .6f, .9f};
    output.hdr = 5.0f;
    output.gradient_row = 13;
    output.parameters = {0, 1};
    cooked.outputs.push_back(output);
    cooked.parameters.push_back({Hash32("signature_scale"), hs::VfxParameterType::Float,
                                 std::bit_cast<std::uint32_t>(1.0f)});
    return cooked;
}

hs::VfxEventInput BossSignatureEvent(std::string_view schema)
{
    hs::VfxEventInput input;
    input.effect_handle = 1;
    input.event_tick = 100;
    input.sequence = 0x1234;
    input.stable_seed = 91;
    if (schema == "PointEventPayload")
    {
        hs::VfxPointPayload point;
        point.position = {4, 6, -3};
        point.authored_scale = 2;
        input.payload = point;
    }
    else if (schema == "CircleAreaPayload")
        input.payload = hs::VfxCirclePayload{{4, 6, -3}, {}, 12};
    else if (schema == "RingWithGapsPayload")
    {
        hs::VfxRingGapsPayload ring;
        ring.center = {4, 6, -3};
        ring.inner_radius = 3;
        ring.outer_radius = 14;
        ring.gap_half_width_degrees = 12;
        ring.gap_angles_degrees = {-30, 90};
        input.payload = ring;
    }
    else
        input.payload = hs::VfxConePayload{{4, 6, -3}, {3, 2, 4}, 18, 45};
    return input;
}

void BossSignatureKeepsGameplayFootprintsAndSafeGaps()
{
    for (const std::string_view schema : {"PointEventPayload", "CircleAreaPayload",
                                        "RingWithGapsPayload", "ConePayload"})
    {
        auto cooked = BossSignatureProgram(schema);
        auto event = BossSignatureEvent(schema);
        const auto run = [&](hs::Tick tick) {
            return hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked, std::span(&event, 1), tick);
        };
        const auto commands = run(110);
        Require(commands.size() == 1, "boss signature output missing");
        const auto &item = commands[0];
        const float radius = schema == "PointEventPayload" ? 2 :
            (schema == "CircleAreaPayload" ? 12 : (schema == "RingWithGapsPayload" ? 14 : 18));
        const auto shape = schema == "ConePayload" ? hs::VfxGroundShape::ConeBorder :
            (schema == "RingWithGapsPayload" ? hs::VfxGroundShape::RingGapsBorder : hs::VfxGroundShape::Circle);
        Require(item.additive && item.geometry.shape == shape &&
                item.command.primitive == hs::VfxPrimitive::ExactRing &&
                item.command.renderer == hs::VfxRenderer::Ground &&
                Near(item.geometry.animation_phase, 1.0f),
                "boss signature rendering contract lost");
        Require(Near(item.geometry.outer_radius, radius) && Near(item.geometry.edge_width, .08f) &&
                Near(item.command.start_size_min, radius + .08f) &&
                Near(item.command.stretch * item.command.start_size_min, radius) &&
                Near(item.command.position.x, 4) && Near(item.command.position.y, .025f) &&
                Near(item.command.position.z, -3),
                "boss signature changed gameplay footprint or ground anchor");
        Require(Near(item.geometry.progress, (10.0f / 60.0f) / .45f) &&
                Near(item.command.start_color.x, 1.0f) && Near(item.command.start_color.y, 2.0f) &&
                Near(item.command.start_color.z, 3.0f) && Near(item.command.start_color.w, .9f) &&
                item.gradient_row == 13 && Near(item.hdr, 5.0f) &&
                item.stable_id == event.sequence && item.command.seed == event.stable_seed,
                "boss signature source timing, material, or identity lost");
        if (schema == "RingWithGapsPayload")
            Require(Near(item.geometry.inner_radius, 3) &&
                    Near(item.geometry.gap_half_angle_radians, 12 * 3.14159265358979323846f / 180) &&
                    item.geometry.gap_angles_radians.size() == 2 &&
                    Near(item.geometry.gap_angles_radians[0], 330 * 3.14159265358979323846f / 180) &&
                    Near(item.geometry.gap_angles_radians[1], 90 * 3.14159265358979323846f / 180),
                    "boss signature discarded safe-gap geometry");
        if (schema == "ConePayload")
            Require(Near(item.geometry.range, 18) &&
                    Near(item.geometry.half_angle_radians, 45 * 3.14159265358979323846f / 180) &&
                    Near(item.geometry.direction.x, .6f) && Near(item.geometry.direction.z, .8f),
                    "boss signature changed cone footprint or direction");
        Require(run(127).empty(), "boss signature survived exclusive source endpoint");
    }
}

void BossSignatureRejectsWrongContractAndInvalidGeometry()
{
    auto cooked = BossSignatureProgram("RingWithGapsPayload");
    auto event = BossSignatureEvent("RingWithGapsPayload");
    const auto run = [&] {
        return hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked, std::span(&event, 1), 110);
    };
    cooked.outputs[0].profile = hs::VfxOutputProfile::GroundSdfSoft;
    Require(run().empty(), "wrong boss signature profile accepted");
    cooked.outputs[0].profile = hs::VfxOutputProfile::GroundSdfAdd;
    cooked.outputs[0].shape = Hash32("expanding_ring_sdf");
    Require(run().empty(), "wrong boss signature shape accepted");
    cooked.outputs[0].shape = Hash32("large_signature_sdf");
    cooked.outputs[0].motion = hs::VfxMotionKind::FastExpansionSurface;
    Require(run().empty(), "wrong boss signature motion accepted");
    cooked.outputs[0].motion = hs::VfxMotionKind::StrongSnapGameplayResolve;
    cooked.parameters[0].bits = std::bit_cast<std::uint32_t>(2.0f);
    Require(run().empty(), "scaled authoritative boss footprint accepted");
    cooked.parameters[0].bits = std::bit_cast<std::uint32_t>(1.0f);
    cooked.parameters[0].type = hs::VfxParameterType::Int;
    Require(run().empty(), "wrong signature scale type accepted");
    cooked.parameters[0].type = hs::VfxParameterType::Float;
    auto &ring = std::get<hs::VfxRingGapsPayload>(event.payload);
    ring.gap_half_width_degrees = 0;
    Require(run().empty(), "nonempty safe gaps with zero angular width accepted");
    ring.gap_half_width_degrees = 12;
    ring.outer_radius = ring.inner_radius;
    Require(run().empty(), "invalid authoritative annulus accepted");
    event = BossSignatureEvent("RingWithGapsPayload");
    event.payload = hs::VfxCirclePayload{};
    Require(run().empty(), "boss ring recipe accepted wrong payload schema");
    event = BossSignatureEvent("RingWithGapsPayload");
    cooked.sources[0].knots[1] = 0;
    Require(run().empty(), "empty boss signature source interval accepted");
}

void CookedBossSignaturesConvertWhenContentIsAvailable()
{
    const auto path = std::filesystem::path("Cooked") / "vfx_program.hsbin";
    if (!std::filesystem::exists(path)) return;
    std::ifstream stream(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    hs::VfxProgramData cooked;
    std::string error;
    Require(hs::LoadVfxProgram(std::as_bytes(std::span(bytes)), cooked, error),
            "the cooked VFX program could not be loaded for boss coverage");
    std::size_t authored_count{};
    std::size_t converted_count{};
    for (const auto &effect : cooked.effects)
    {
        std::size_t effect_count{};
        for (std::size_t source_index = effect.sources.first;
             source_index < static_cast<std::size_t>(effect.sources.first) + effect.sources.count; ++source_index)
        {
            const auto &source = cooked.sources[source_index];
            if (source.type != hs::VfxSourceType::Direct) continue;
            for (std::size_t output_index = source.outputs.first;
                 output_index < static_cast<std::size_t>(source.outputs.first) + source.outputs.count; ++output_index)
            {
                const auto &output = cooked.outputs[output_index];
                effect_count += output.profile == hs::VfxOutputProfile::GroundSdfAdd &&
                    output.shape == Hash32("large_signature_sdf") &&
                    output.motion == hs::VfxMotionKind::StrongSnapGameplayResolve;
            }
        }
        if (!effect_count) continue;
        authored_count += effect_count;
        hs::VfxEventInput event;
        if (effect.payload_kind == Hash32("PointEventPayload")) event = BossSignatureEvent("PointEventPayload");
        else if (effect.payload_kind == Hash32("CircleAreaPayload")) event = BossSignatureEvent("CircleAreaPayload");
        else if (effect.payload_kind == Hash32("RingWithGapsPayload")) event = BossSignatureEvent("RingWithGapsPayload");
        else if (effect.payload_kind == Hash32("ConePayload")) event = BossSignatureEvent("ConePayload");
        else throw std::runtime_error("cooked boss signature has an unsupported payload schema");
        event.effect_handle = effect.handle;
        const auto tick = event.event_tick + static_cast<hs::Tick>(std::lround(.2f * effect.seconds * 60.0f));
        const auto commands = hs::runtime_detail::BuildVfxTypedEventGroundCommands(
            cooked, std::span(&event, 1), tick);
        converted_count += std::count_if(commands.begin(), commands.end(),
            [](const auto &item) { return item.additive && item.geometry.animation_phase == 1.0f; });
    }
    Require(authored_count == 4 && converted_count == authored_count,
            "the four cooked boss signature outputs did not all convert");
}

hs::VfxProgramData RemainingEventProgram(std::string_view schema, std::string_view shape,
                                          hs::VfxMotionKind motion, float start, float end)
{
    hs::VfxProgramData cooked;
    hs::VfxEffectRecord effect;
    effect.handle = 1;
    effect.input_mode = 0;
    effect.payload_kind = Hash32(schema);
    effect.timing_kind = 0;
    effect.seconds = 1.0f;
    effect.sources = {0, 1};
    cooked.effects.push_back(effect);
    hs::VfxSourceRecord source;
    source.effect = 1;
    source.type = hs::VfxSourceType::Direct;
    source.knot_count = 2;
    source.knots = {start, end, 0, 0};
    source.outputs = {0, 1};
    cooked.sources.push_back(source);
    hs::VfxOutputRecord output;
    output.source = 0;
    output.profile = hs::VfxOutputProfile::GroundSdfAdd;
    output.shape = Hash32(shape);
    output.motion = motion;
    output.rgba = {.2f, .4f, .6f, .6f};
    output.hdr = 3;
    output.gradient_row = 13;
    if (shape == "cross_ring_sdf")
    {
        output.parameters = {0, 1};
        cooked.parameters.push_back({Hash32("sdf"), hs::VfxParameterType::Enum,
                                     Hash32("cross_plus_ring")});
    }
    else if (shape == "broken_hex_sdf")
    {
        output.parameters = {0, 2};
        cooked.parameters = {
            {Hash32("edge_width"), hs::VfxParameterType::Float, std::bit_cast<std::uint32_t>(.045f)},
            {Hash32("break_sector_deg"), hs::VfxParameterType::Float, std::bit_cast<std::uint32_t>(52.0f)}};
    }
    else
    {
        output.parameters = {0, 2};
        cooked.parameters = {
            {Hash32("length"), hs::VfxParameterType::Float, std::bit_cast<std::uint32_t>(1.3f)},
            {Hash32("width"), hs::VfxParameterType::Float, std::bit_cast<std::uint32_t>(.05f)}};
    }
    cooked.outputs.push_back(output);
    return cooked;
}

hs::VfxEventInput RemainingEvent(std::string_view schema)
{
    hs::VfxEventInput input;
    input.effect_handle = 1;
    input.event_tick = 100;
    input.sequence = 0x1234;
    input.stable_seed = 91;
    if (schema == "PointEventPayload")
    {
        hs::VfxPointPayload point;
        point.position = {4, 6, -3};
        point.authored_scale = 2;
        input.payload = point;
    }
    else if (schema == "CircleAreaPayload")
        input.payload = hs::VfxCirclePayload{{4, 6, -3}, {}, 3.2f};
    else
    {
        input.payload = hs::VfxProjectilePayload{{3, 0, 4}, {}, .23f};
        input.world_transform[12] = 4;
        input.world_transform[13] = 6;
        input.world_transform[14] = -3;
    }
    return input;
}

void ConvertsHealTrapAndAxialWithTheirOwnGeometry()
{
    struct Case { std::string_view schema, shape; hs::VfxMotionKind motion; float start, end; };
    const Case cases[]{
        {"PointEventPayload", "cross_ring_sdf", hs::VfxMotionKind::RingContractsReleases, 0, .65f},
        {"CircleAreaPayload", "broken_hex_sdf", hs::VfxMotionKind::QuickInwardFracture, 0, .7f},
        {"ProjectilePayload", "axial_fracture_sdf", hs::VfxMotionKind::StretchesProjectileDirection, .04f, .85f}};
    for (const auto &test : cases)
    {
        const auto cooked = RemainingEventProgram(test.schema, test.shape, test.motion, test.start, test.end);
        auto event = RemainingEvent(test.schema);
        const auto run = [&](hs::Tick tick) {
            return hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked, std::span(&event, 1), tick);
        };
        const auto commands = run(110);
        Require(commands.size() == 1, "remaining event ground output missing");
        const auto &item = commands[0];
        const auto shape = test.shape == "cross_ring_sdf" ? hs::VfxGroundShape::CrossRing :
            (test.shape == "broken_hex_sdf" ? hs::VfxGroundShape::BrokenHex : hs::VfxGroundShape::AxialFracture);
        Require(item.additive && item.geometry.shape == shape &&
                item.command.primitive == hs::VfxPrimitive::ExactRing &&
                item.command.renderer == hs::VfxRenderer::Ground &&
                Near(item.command.position.x, 4) && Near(item.command.position.y, .025f) &&
                Near(item.command.position.z, -3) &&
                Near(item.geometry.progress, ((10.0f / 60.0f) - test.start) / (test.end - test.start)),
                "event shape, anchor, or source progress changed");
        Require(Near(item.command.start_color.x, .6f) && Near(item.command.start_color.y, 1.2f) &&
                Near(item.command.start_color.z, 1.8f) && Near(item.command.start_color.w, .6f) &&
                item.gradient_row == 13 && Near(item.hdr, 3) &&
                item.stable_id == event.sequence && item.command.seed == event.stable_seed,
                "event HDR, gradient, alpha, or identity changed");
        if (test.shape == "cross_ring_sdf")
            Require(Near(item.geometry.outer_radius, 1.3f) && Near(item.geometry.edge_width, .05f) &&
                    Near(item.command.start_size_min, 1.35f),
                    "healing cross/ring radius or edge changed");
        else if (test.shape == "broken_hex_sdf")
            Require(Near(item.geometry.outer_radius, 3.2f) && Near(item.geometry.edge_width, .045f) &&
                    Near(item.geometry.gap_half_angle_radians, 26 * 3.14159265358979323846f / 180) &&
                    Near(item.geometry.animation_phase, 91 * 3.14159265358979323846f / 180),
                    "trap fracture radius or break sector changed");
        else
            Require(Near(item.geometry.outer_radius, .23f) && Near(item.geometry.range, 1.3f) &&
                    Near(item.geometry.edge_width, .05f) && Near(item.geometry.direction.x, .6f) &&
                    Near(item.geometry.direction.z, .8f) && Near(item.command.start_size_min, .7f),
                    "boss axial impact direction or hitbox dimensions changed");
        Require(run(151).empty(), "event ground output survived its authored source interval");
    }
}

void RemainingEventsRejectWrongContractsAndGeometry()
{
    auto cooked = RemainingEventProgram("PointEventPayload", "cross_ring_sdf",
                                        hs::VfxMotionKind::RingContractsReleases, 0, .65f);
    auto event = RemainingEvent("PointEventPayload");
    const auto run = [&] {
        return hs::runtime_detail::BuildVfxTypedEventGroundCommands(cooked, std::span(&event, 1), 110);
    };
    cooked.outputs[0].profile = hs::VfxOutputProfile::GroundSdfSoft;
    Require(run().empty(), "wrong healing profile accepted");
    cooked.outputs[0].profile = hs::VfxOutputProfile::GroundSdfAdd;
    cooked.outputs[0].shape = Hash32("expanding_ring_sdf");
    Require(run().empty(), "wrong healing shape accepted");
    cooked.outputs[0].shape = Hash32("cross_ring_sdf");
    cooked.outputs[0].motion = hs::VfxMotionKind::SlowRadialFlow;
    Require(run().empty(), "wrong healing motion accepted");
    cooked.outputs[0].motion = hs::VfxMotionKind::RingContractsReleases;
    cooked.parameters[0].bits = Hash32("circle_only");
    Require(run().empty(), "wrong healing SDF enum accepted");
    cooked = RemainingEventProgram("CircleAreaPayload", "broken_hex_sdf",
                                   hs::VfxMotionKind::QuickInwardFracture, 0, .7f);
    event = RemainingEvent("CircleAreaPayload");
    cooked.parameters[1].bits = std::bit_cast<std::uint32_t>(0.0f);
    Require(run().empty(), "zero fracture sector accepted");
    cooked = RemainingEventProgram("ProjectilePayload", "axial_fracture_sdf",
                                   hs::VfxMotionKind::StretchesProjectileDirection, .04f, .85f);
    event = RemainingEvent("ProjectilePayload");
    std::get<hs::VfxProjectilePayload>(event.payload).velocity = {0, 2, 0};
    Require(run().empty(), "axial ground fracture accepted vertical-only velocity");
    event = RemainingEvent("ProjectilePayload");
    cooked.parameters[0].bits = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());
    Require(run().empty(), "nonfinite axial length accepted");
}

hs::VfxProgramData CrownProgram(bool locked)
{
    hs::VfxProgramData cooked;
    hs::VfxEffectRecord effect;
    effect.handle = 1;
    effect.input_mode = 1;
    effect.timing_kind = 1;
    effect.payload_kind = Hash32("EntityAttachmentPayload");
    effect.sources = {0, 1};
    cooked.effects.push_back(effect);
    cooked.effect_lookup.push_back({Hash64(locked
        ? "persistent.boss.phase_transition_invulnerable"
        : "persistent.boss.phase2_aura"), 1});
    hs::VfxSourceRecord source;
    source.effect = 1;
    source.type = hs::VfxSourceType::Direct;
    source.knot_count = 2;
    source.knots = {0, locked ? .88f : 1.0f, 0, 0};
    source.outputs = {0, 1};
    cooked.sources.push_back(source);
    hs::VfxOutputRecord output;
    output.source = 0;
    output.profile = hs::VfxOutputProfile::GroundSdfAdd;
    output.shape = Hash32(locked ? "closed_crown_ring_sdf" : "broken_crown_sdf");
    output.motion = locked ? hs::VfxMotionKind::LockedSteadyRingPulse : hs::VfxMotionKind::InwardTeethPulse;
    output.rgba = {.2f, .4f, .6f, .72f};
    output.hdr = 4.6f;
    output.gradient_row = 13;
    output.parameters = {0, 2};
    cooked.outputs.push_back(output);
    if (locked)
        cooked.parameters = {
            {Hash32("edge_width"), hs::VfxParameterType::Float, std::bit_cast<std::uint32_t>(.06f)},
            {Hash32("pulse_hz"), hs::VfxParameterType::Float, std::bit_cast<std::uint32_t>(1.1f)}};
    else
        cooked.parameters = {
            {Hash32("pulse_hz"), hs::VfxParameterType::Float, std::bit_cast<std::uint32_t>(.8f)},
            {Hash32("inner_radius"), hs::VfxParameterType::Float, std::bit_cast<std::uint32_t>(.72f)}};
    return cooked;
}

hs::VfxPersistentInput CrownInput()
{
    hs::VfxPersistentInput input;
    input.stable_id = 0x1234;
    input.effect_handle = 1;
    input.current_transform[12] = 4;
    input.current_transform[13] = 6;
    input.current_transform[14] = -3;
    input.normalized_age = .5f;
    input.elapsed_seconds = 2.0f;
    input.stable_seed = 91;
    input.payload = hs::VfxEntityPayload{2.0f, .5f, 1.0f, 0};
    return input;
}

void PersistentCrownsUseActualOwnerFootprintAndPhase()
{
    for (bool locked : {false, true})
    {
        auto cooked = CrownProgram(locked);
        auto input = CrownInput();
        const auto run = [&] {
            return hs::runtime_detail::BuildVfxTypedGroundCommands(cooked, std::span(&input, 1), 712);
        };
        auto commands = run();
        Require(commands.size() == 1, "persistent crown output missing");
        const auto &item = commands[0];
        Require(item.additive && item.geometry.shape == (locked ? hs::VfxGroundShape::ClosedCrownRing : hs::VfxGroundShape::BrokenCrown) &&
                item.command.primitive == hs::VfxPrimitive::ExactRing &&
                Near(item.command.position.x, 4) && Near(item.command.position.y, .025f) &&
                Near(item.command.position.z, -3) && Near(item.geometry.outer_radius, 2),
                "persistent crown did not follow actual boss owner footprint");
        Require(Near(item.geometry.progress, locked ? .5f / .88f : .5f) &&
                Near(item.geometry.animation_phase, locked ? 2.2f : 1.6f) &&
                Near(item.geometry.edge_width, locked ? .06f : .05f) &&
                Near(item.geometry.inner_radius, locked ? 0 : .72f) &&
                item.stable_id == input.stable_id && item.command.seed == input.stable_seed &&
                Near(item.command.start_color.w, .72f) && item.gradient_row == 13 && Near(item.hdr, 4.6f),
                "persistent crown timing, pulse, material, or identity lost");
        std::get<hs::VfxEntityPayload>(input.payload).footprint_radius = 0;
        Require(run().empty(), "zero crown owner footprint accepted");
        input = CrownInput();
        cooked.parameters[locked ? 1 : 0].bits = std::bit_cast<std::uint32_t>(0.0f);
        Require(run().empty(), "zero crown pulse rate accepted");
        if (locked)
        {
            cooked = CrownProgram(true);
            input.normalized_age = .9f;
            Require(run().empty(), "transition crown survived exclusive source end");
        }
    }
}

hs::VfxProgramData StateRingProgram()
{
    auto cooked = CrownProgram(false);
    cooked.effect_lookup[0].effect_id = Hash64("particle.player.invulnerable_loop");
    auto &source = cooked.sources[0];
    source.stable_id = Hash32("ground_state");
    source.knots = {.2f, .8f, 0, 0};
    auto &output = cooked.outputs[0];
    output.profile = hs::VfxOutputProfile::GroundSdfSoft;
    output.shape = Hash32("state_ring");
    output.shape_domain = Hash32("sdf");
    output.shape_scale_rule = Hash32("gameplay_geometry_if_bound_else_component_radius");
    output.shape_component_kind = Hash32("ground_sdf");
    output.coverage_type = Hash32("analytic");
    output.coverage_ref = Hash32("state_ring");
    output.motion = hs::VfxMotionKind::SlowStableRotation;
    output.rgba = {.25f, .5f, .75f, .35f};
    output.hdr = 1.4f;
    output.gradient_row = 8;
    output.parameters = {0, 1};
    output.textures = {0, 3};
    cooked.parameters = {{Hash32("segments"), hs::VfxParameterType::Int, 6}};
    const auto texture = [&](std::uint32_t slot, std::string_view role,
                             std::string_view path) {
        cooked.texture_resources.push_back({slot,
            {static_cast<std::uint32_t>(cooked.strings.size()),
             static_cast<std::uint32_t>(path.size())}});
        for (const unsigned char character : path)
            cooked.strings.push_back(static_cast<std::byte>(character));
        hs::VfxTextureBindingRecord binding;
        binding.role = Hash32(role);
        binding.catalog_slot = slot;
        cooked.texture_bindings.push_back(binding);
    };
    texture(1, "profile.global", "Content/Textures/VFX/vfx_gradient_lut.dds");
    texture(2, "profile.global", "Content/Textures/VFX/vfx_curve_lut.dds");
    texture(3, "profile.optional_detail",
            "Content/Textures/VFX/vfx_authored_mask_array.dds");
    return cooked;
}

hs::VfxPersistentInput StateRingInput()
{
    auto input = CrownInput();
    input.elapsed_seconds = 1.25f;
    input.payload = hs::VfxEntityPayload{1.0f, .5f, 1.0f, 0, 0xabcdu};
    return input;
}

void PlayerStateRingKeepsOwnerAndCookedContract()
{
    auto cooked = StateRingProgram();
    auto input = StateRingInput();
    const auto run = [&] {
        return hs::runtime_detail::BuildVfxTypedGroundCommands(
            cooked, std::span(&input, 1), 712);
    };
    const auto commands = run();
    Require(commands.size() == 1, "player ground state ring missing");
    const auto &item = commands.front();
    Require(!item.additive && item.geometry.shape == hs::VfxGroundShape::StateRing &&
            item.command.primitive == hs::VfxPrimitive::ExactRing &&
            item.geometry.spokes == 6 &&
            Near(item.geometry.outer_radius, 1) && Near(item.geometry.inner_radius, .95f) &&
            Near(item.geometry.edge_width, .05f) &&
            Near(item.geometry.progress, .5f) &&
            Near(item.geometry.animation_phase, 1.25f) &&
            Near(item.command.start_size_min, 1.05f) &&
            Near(item.command.stretch, 1.0f / 1.05f) &&
            Near(item.command.position.x, 4) && Near(item.command.position.y, .025f) &&
            Near(item.command.position.z, -3),
            "player state ring lost owner footprint or source clock");
    Require(item.stable_id == input.stable_id && item.command.seed == input.stable_seed &&
            item.gradient_row == 8 && Near(item.hdr, 1.4f) &&
            Near(item.command.start_color.x, .35f) &&
            Near(item.command.start_color.w, .35f),
            "player state ring lost cooked color, alpha, HDR, gradient, or identity");
    cooked.parameters[0].bits = 12;
    Require(run().size() == 1 && run()[0].geometry.spokes == 12,
            "retuned typed segment count was ignored");
}

void PlayerStateRingRejectsWrongTupleAndOwner()
{
    auto cooked = StateRingProgram();
    auto input = StateRingInput();
    const auto run = [&] {
        return hs::runtime_detail::BuildVfxTypedGroundCommands(
            cooked, std::span(&input, 1), 712);
    };
    cooked.effect_lookup[0].effect_id = Hash64("persistent.boss.phase2_aura");
    Require(run().empty(), "boss effect accepted player state ring");
    cooked = StateRingProgram();
    cooked.sources[0].stable_id = Hash32("crown_teeth");
    Require(run().empty(), "wrong player source accepted");
    cooked = StateRingProgram();
    cooked.outputs[0].profile = hs::VfxOutputProfile::GroundSdfAdd;
    Require(run().empty(), "additive profile accepted player state ring");
    cooked = StateRingProgram();
    cooked.outputs[0].shape = Hash32("exact_hitbox_ring");
    Require(run().empty(), "wrong player ground shape accepted");
    cooked = StateRingProgram();
    cooked.outputs[0].motion = hs::VfxMotionKind::LockedSteadyRingPulse;
    Require(run().empty(), "boss motion accepted player state ring");
    cooked = StateRingProgram();
    cooked.parameters[0].type = hs::VfxParameterType::Float;
    Require(run().empty(), "untyped segment count accepted");
    cooked = StateRingProgram();
    cooked.parameters[0].bits = 0;
    Require(run().empty(), "zero state-ring segments accepted");
    cooked = StateRingProgram();
    cooked.texture_bindings[2].role = Hash32("profile.global");
    Require(run().empty(), "wrong state-ring texture binding accepted");
    cooked = StateRingProgram();
    std::get<hs::VfxEntityPayload>(input.payload).render_instance_id = 0;
    Require(run().empty(), "state ring without Archer owner accepted");
    input = StateRingInput();
    std::get<hs::VfxEntityPayload>(input.payload).footprint_radius = 0;
    Require(run().empty(), "state ring without gameplay footprint accepted");
    input = StateRingInput();
    input.normalized_age = .6f;
    Require(run().empty(), "state ring accepted mismatched owner clock");
}

hs::VfxProgramData ChargedPulseProgram()
{
    hs::VfxProgramData cooked;
    hs::VfxEffectRecord effect;
    effect.handle = 1;
    effect.input_mode = 0;
    effect.timing_kind = 0;
    effect.seconds = .42f;
    effect.payload_kind = Hash32("PresentationContextPayload");
    effect.anchor = Hash32("presentation_context");
    effect.orientation = Hash32("effect_defined");
    effect.scale = Hash32("authored");
    effect.binding_timing = Hash32("charge_ratio");
    effect.geometry = Hash32("cosmetic_only");
    effect.sources = {0, 2};
    cooked.effects.push_back(effect);
    cooked.effect_lookup.push_back({Hash64("particle.skill.charged_shot.pulse"), 1});
    for (std::uint32_t index = 0; index < 2; ++index)
    {
        hs::VfxSourceRecord source;
        source.effect = 1;
        source.type = hs::VfxSourceType::Direct;
        source.stable_id = Hash32(index == 0 ? "gameplay_boundary" : "interior_secondary");
        source.knot_count = 2;
        source.knots = {0.0f, 1.0f, 0.0f, 0.0f};
        source.outputs = {index, 1};
        cooked.sources.push_back(source);
    }
    cooked.parameters = {
        {Hash32("anti_alias"), hs::VfxParameterType::Enum, Hash32("fwidth")},
        {Hash32("edge_width_world"), hs::VfxParameterType::Float,
         std::bit_cast<std::uint32_t>(.055f)},
        {Hash32("flow_speed"), hs::VfxParameterType::Float,
         std::bit_cast<std::uint32_t>(.25f)}};
    hs::VfxOutputRecord ring;
    ring.source = 0;
    ring.profile = hs::VfxOutputProfile::GroundSdfSoft;
    ring.shape = Hash32("exact_hitbox_ring");
    ring.shape_domain = Hash32("sdf");
    ring.shape_scale_rule = Hash32("gameplay_geometry_if_bound_else_component_radius");
    ring.shape_component_kind = Hash32("ground_sdf");
    ring.coverage_type = Hash32("analytic");
    ring.coverage_ref = ring.shape;
    ring.motion = hs::VfxMotionKind::StableBoundaryInwardPulse;
    ring.motion_rate_hz = .8f;
    ring.motion_amplitude = .22f;
    ring.motion_inset_fraction = .12f;
    ring.rgba = {.254152f, .686685f, 1.0f, .78f};
    ring.hdr = 2.2f;
    ring.gradient_row = 8;
    ring.parameters = {0, 2};
    ring.textures = {0, 3};
    cooked.outputs.push_back(ring);
    auto fill = ring;
    fill.source = 1;
    fill.profile = hs::VfxOutputProfile::GroundSdfOit;
    fill.shape = Hash32("low_frequency_fill");
    fill.shape_domain = Hash32("sprite");
    fill.shape_scale_rule = Hash32("component_size");
    fill.coverage_ref = fill.shape;
    fill.motion = hs::VfxMotionKind::SlowRadialFlow;
    fill.motion_rate_hz = fill.motion_amplitude = fill.motion_inset_fraction = 0;
    fill.rgba = {.194618f, .168269f, 1.0f, .18f};
    fill.hdr = .9f;
    fill.gradient_row = 9;
    fill.parameters = {2, 1};
    fill.textures = {3, 3};
    cooked.outputs.push_back(fill);
    const auto texture = [&](std::uint32_t slot, std::string_view role,
                             std::string_view path) {
        cooked.texture_resources.push_back({slot,
            {static_cast<std::uint32_t>(cooked.strings.size()),
             static_cast<std::uint32_t>(path.size())}});
        for (const unsigned char character : path)
            cooked.strings.push_back(static_cast<std::byte>(character));
        for (int output = 0; output < 2; ++output)
        {
            hs::VfxTextureBindingRecord binding;
            binding.role = Hash32(role);
            binding.catalog_slot = slot;
            binding.selection = 0;
            cooked.texture_bindings.push_back(binding);
        }
    };
    texture(1, "profile.global", "Content/Textures/VFX/vfx_gradient_lut.dds");
    texture(2, "profile.global", "Content/Textures/VFX/vfx_curve_lut.dds");
    texture(3, "profile.optional_detail", "Content/Textures/VFX/vfx_authored_mask_array.dds");
    // Bindings are grouped by output in the cooked program.
    const auto bindings = cooked.texture_bindings;
    cooked.texture_bindings = {bindings[0], bindings[2], bindings[4],
                               bindings[1], bindings[3], bindings[5]};
    return cooked;
}

hs::VfxEventInput ChargedPulseInput()
{
    hs::VfxEventInput input;
    input.effect_handle = 1;
    input.event_tick = 100;
    input.sequence = 81;
    input.stable_seed = 47;
    input.world_transform[12] = 4.0f;
    input.world_transform[13] = 2.0f;
    input.world_transform[14] = -3.0f;
    input.payload = hs::VfxContextPayload{1ull << 60, {}, .5f, 0.0f, 0.0f};
    return input;
}

void ChargedPulseUsesContextRatioAndAuthoredGroundOutputs()
{
    auto cooked = ChargedPulseProgram();
    auto input = ChargedPulseInput();
    const auto run = [&](hs::Tick tick) {
        return hs::runtime_detail::BuildVfxTypedEventGroundCommands(
            cooked, std::span(&input, 1), tick);
    };
    const auto commands = run(112);
    Require(commands.size() == 2, "charged pulse lost its two direct ground outputs");
    const auto &ring = commands[0];
    const auto &fill = commands[1];
    Require(!ring.additive && ring.geometry.shape == hs::VfxGroundShape::StateRing &&
            ring.command.primitive == hs::VfxPrimitive::ExactRing &&
            ring.geometry.spokes == 6 && ring.geometry.rings == 1 &&
            Near(ring.geometry.progress, .5f) && Near(ring.geometry.animation_phase, .2f) &&
            Near(ring.geometry.outer_radius, .65f) && Near(ring.geometry.inner_radius, .595f) &&
            Near(ring.geometry.edge_width, .055f) && Near(ring.command.start_size_min, .705f) &&
            Near(ring.command.stretch, .65f / .705f),
            "charged pulse ring lost cosmetic scale, six segments, or charge ratio");
    Require(!fill.additive && fill.geometry.shape == hs::VfxGroundShape::Circle &&
            fill.command.primitive == hs::VfxPrimitive::LowFrequencyFill &&
            Near(fill.geometry.outer_radius, .65f) && Near(fill.command.start_size_min, .65f) &&
            Near(fill.command.stretch, .25f),
            "charged pulse fill lost its supported circle geometry or flow");
    Require(ring.stable_id == (1ull << 60) && fill.stable_id == ring.stable_id &&
            ring.command.tick == 100 && fill.command.tick == 100 &&
            ring.command.seed == 47 && fill.command.seed == 47 &&
            Near(ring.command.position.x, 4) && Near(ring.command.position.y, .025f) &&
            Near(ring.command.position.z, -3) &&
            Near(fill.command.position.x, 4) && Near(fill.command.position.z, -3) &&
            Near(ring.command.start_color.x, .254152f * 2.2f) &&
            Near(ring.command.start_color.w, .78f) &&
            Near(fill.command.start_color.x, .194618f * .9f) &&
            Near(fill.command.start_color.w, .18f) &&
            Near(ring.command.lifetime_min, .42f) && Near(fill.command.lifetime_min, .42f),
            "charged pulse lost owner identity, event clock, center, or cooked material");
    Require(run(99).empty() && run(126).empty(),
            "charged pulse escaped its fixed event lifetime");
    std::get<hs::VfxContextPayload>(input.payload).ratio01 = 0.0f;
    Require(run(112).size() == 2 && Near(run(112)[0].geometry.progress, 0.0f),
            "zero charge did not reach segment geometry");
    std::get<hs::VfxContextPayload>(input.payload).ratio01 = 1.0f;
    Require(run(112).size() == 2 && Near(run(112)[0].geometry.progress, 1.0f),
            "full charge did not reach segment geometry");
    input = ChargedPulseInput();
    cooked.sources[0].knot_count = 4;
    cooked.sources[0].knots = {.1f, .3f, .6f, .8f};
    const auto envelope = run(105);
    Require(envelope.size() == 2 && Near(envelope[0].command.start_color.w,
            .78f * (((5.0f / 60.0f) / .42f - .1f) / .2f)),
            "charged pulse ignored its authored source envelope");
}

void ChargedPulseRejectsMalformedContextAndCookedContract()
{
    auto cooked = ChargedPulseProgram();
    auto input = ChargedPulseInput();
    const auto run = [&] {
        return hs::runtime_detail::BuildVfxTypedEventGroundCommands(
            cooked, std::span(&input, 1), 112);
    };
    const auto reject = [&](const char *message) { Require(run().empty(), message); };
    cooked.effect_lookup[0].effect_id = Hash64("particle.skill.charged_shot.ready");
    reject("unrelated context effect accepted charged pulse ground outputs");
    cooked = ChargedPulseProgram();
    cooked.effects[0].binding_timing = Hash32("fixed");
    reject("wrong charge binding accepted");
    cooked = ChargedPulseProgram();
    cooked.sources[0].stable_id = Hash32("other_boundary");
    reject("wrong direct source accepted");
    cooked = ChargedPulseProgram();
    cooked.sources[1].type = hs::VfxSourceType::ProjectileFollow;
    reject("non-direct fill source accepted");
    cooked = ChargedPulseProgram();
    cooked.sources[0].knots[1] = 0.0f;
    reject("empty source interval accepted");
    cooked = ChargedPulseProgram();
    cooked.outputs[0].profile = hs::VfxOutputProfile::GroundSdfAdd;
    reject("wrong ring profile accepted");
    cooked = ChargedPulseProgram();
    cooked.outputs[0].shape = Hash32("state_ring");
    reject("wrong ring shape accepted");
    cooked = ChargedPulseProgram();
    cooked.outputs[0].motion = hs::VfxMotionKind::SlowStableRotation;
    reject("wrong ring motion accepted");
    cooked = ChargedPulseProgram();
    cooked.outputs[0].coverage_ref = Hash32("state_ring");
    reject("wrong ring coverage accepted");
    cooked = ChargedPulseProgram();
    cooked.outputs[1].shape_domain = Hash32("sdf");
    reject("wrong fill shape domain accepted");
    cooked = ChargedPulseProgram();
    cooked.outputs[1].profile = hs::VfxOutputProfile::GroundSdfSoft;
    reject("wrong fill profile accepted");
    cooked = ChargedPulseProgram();
    cooked.outputs[1].min_quality = 1;
    reject("wrong fill quality accepted");
    cooked = ChargedPulseProgram();
    cooked.parameters[0].bits = Hash32("none");
    reject("unsupported ring anti-alias mode accepted");
    cooked = ChargedPulseProgram();
    cooked.parameters[1].bits = std::bit_cast<std::uint32_t>(0.0f);
    reject("zero ring edge width accepted");
    cooked = ChargedPulseProgram();
    cooked.parameters[2].bits = std::bit_cast<std::uint32_t>(
        std::numeric_limits<float>::quiet_NaN());
    reject("nonfinite fill flow accepted");
    cooked = ChargedPulseProgram();
    cooked.texture_bindings[0].role = Hash32("coverage");
    reject("wrong profile texture accepted");
    cooked = ChargedPulseProgram();
    cooked.texture_bindings[0].selection = Hash32("stable_seed_mod_group_size");
    reject("unsupported charged pulse texture selection accepted");
    cooked = ChargedPulseProgram();
    cooked.texture_resources[0].asset_path_bytes.count = 0;
    reject("missing texture resource accepted");
    cooked = ChargedPulseProgram();
    std::get<hs::VfxContextPayload>(input.payload).owner_id = 0;
    reject("context without player owner accepted");
    input = ChargedPulseInput();
    std::get<hs::VfxContextPayload>(input.payload).ratio01 = 1.1f;
    reject("charge ratio above one accepted");
    input = ChargedPulseInput();
    std::get<hs::VfxContextPayload>(input.payload).ratio01 =
        std::numeric_limits<float>::quiet_NaN();
    reject("nonfinite charge ratio accepted");
    input = ChargedPulseInput();
    input.world_transform[12] = std::numeric_limits<float>::infinity();
    reject("nonfinite player center accepted");
    input = ChargedPulseInput();
    input.payload = hs::VfxPointPayload{};
    reject("wrong payload accepted charged pulse ground outputs");
}

void CookedChargedPulseGroundOutputsConvertWhenContentIsAvailable()
{
    const auto path = std::filesystem::path("Cooked") / "vfx_program.hsbin";
    if (!std::filesystem::exists(path)) return;
    std::ifstream stream(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(stream),
                                  std::istreambuf_iterator<char>()};
    hs::VfxProgramData cooked;
    std::string error;
    Require(hs::LoadVfxProgram(std::as_bytes(std::span(bytes)), cooked, error),
            "cooked VFX program failed to load for charged pulse coverage");
    std::size_t matches{};
    for (const auto &lookup : cooked.effect_lookup)
    {
        if (lookup.effect_id != Hash64("particle.skill.charged_shot.pulse")) continue;
        ++matches;
        auto input = ChargedPulseInput();
        input.effect_handle = lookup.handle;
        const auto commands = hs::runtime_detail::BuildVfxTypedEventGroundCommands(
            cooked, std::span(&input, 1), 112);
        Require(commands.size() == 2 &&
                commands[0].geometry.shape == hs::VfxGroundShape::StateRing &&
                commands[0].geometry.spokes == 6 && commands[0].geometry.rings == 1 &&
                Near(commands[0].geometry.progress, .5f) &&
                commands[1].geometry.shape == hs::VfxGroundShape::Circle &&
                commands[1].command.primitive == hs::VfxPrimitive::LowFrequencyFill,
                "cooked charged pulse ground outputs did not decode exactly");
    }
    Require(matches == 1, "cooked charged pulse lookup is missing or ambiguous");
}

hs::VfxProgramData StatusPolarRuneProgram(std::string_view effect_name)
{
    auto cooked = StateRingProgram();
    cooked.effect_lookup[0].effect_id = Hash64(effect_name);
    auto &source = cooked.sources[0];
    source.stable_id = Hash32("main_rune");
    source.knots = {.2f, .8f, 0, 0};
    auto &output = cooked.outputs[0];
    output.profile = hs::VfxOutputProfile::GroundSdfSoft;
    output.shape = Hash32("polar_rune_sdf");
    output.shape_domain = Hash32("sdf");
    output.shape_scale_rule = Hash32("gameplay_geometry_if_bound_else_component_radius");
    output.shape_component_kind = Hash32("ground_sdf");
    output.coverage_type = Hash32("analytic");
    output.coverage_ref = Hash32("polar_rune_sdf");
    output.motion = hs::VfxMotionKind::SlowCounterRotationStableFootprint;
    output.rgba = {.1f, .3f, .8f, .55f};
    output.hdr = 1.8f;
    output.gradient_row = 14;
    output.parameters = {0, 3};
    cooked.parameters = {
        {Hash32("spokes"), hs::VfxParameterType::Int, 6},
        {Hash32("ring_count"), hs::VfxParameterType::Int, 2},
        {Hash32("rotation_speed"), hs::VfxParameterType::Float,
         std::bit_cast<std::uint32_t>(.3f)}};
    return cooked;
}

hs::VfxPersistentInput StatusPolarRuneInput()
{
    auto input = StateRingInput();
    input.source_visual_kind = 42;
    input.elapsed_seconds = 2.0f;
    input.normalized_age = .5f;
    input.payload = hs::VfxEntityPayload{2.0f, .5f, 1.0f, 0, 0xabcdu};
    return input;
}

hs::VfxProgramData PickupIdlePolarRuneProgram(std::string_view effect_name,
                                              bool relic_mask)
{
    auto cooked = StatusPolarRuneProgram(effect_name);
    cooked.sources[0].knots = {0.0f, 1.0f, 0.0f, 0.0f};
    if (relic_mask)
    {
        cooked.outputs[0].textures.count = 4;
        hs::VfxTextureBindingRecord binding;
        binding.role = Hash32("authored_mask_array");
        binding.catalog_slot = 3;
        binding.selection = Hash32("stable_seed_mod_group_size");
        binding.strength = .58f;
        binding.slices = {0, 2};
        cooked.texture_bindings.push_back(binding);
        cooked.slice_indices = {7, 9};
    }
    return cooked;
}

hs::VfxPersistentInput PickupIdlePolarRuneInput()
{
    auto input = StatusPolarRuneInput();
    input.normalized_age = 0.0f;
    std::get<hs::VfxEntityPayload>(input.payload).lifetime01 = 0.0f;
    return input;
}

void PickupIdlePolarRunesUsePresenceClockAndRelicMask()
{
    const std::array names{
        std::string_view("particle.pickup.xp.idle"),
        std::string_view("particle.pickup.heal.idle"),
        std::string_view("particle.pickup.magnet.idle"),
        std::string_view("particle.pickup.relic_chest.idle")};
    for (std::size_t index = 0; index < names.size(); ++index)
    {
        auto cooked = PickupIdlePolarRuneProgram(names[index], index == 3);
        auto input = PickupIdlePolarRuneInput();
        const auto commands = hs::runtime_detail::BuildVfxTypedGroundCommands(
            cooked, std::span(&input, 1), 712);
        Require(commands.size() == 1, "pickup idle polar rune missing");
        const auto &item = commands.front();
        Require(!item.additive && item.geometry.shape == hs::VfxGroundShape::PolarRune &&
                item.command.primitive == hs::VfxPrimitive::ExactRing &&
                Near(item.normalized_age, 0.0f) && Near(item.lifetime01, 0.0f) &&
                Near(item.geometry.outer_radius, 2.0f) &&
                Near(item.geometry.progress, 0.0f) &&
                Near(item.geometry.animation_phase, 0.6f) &&
                Near(item.command.position.x, 4.0f) &&
                Near(item.command.position.y, .025f) &&
                Near(item.command.position.z, -3.0f),
                "pickup idle polar rune lost owner footprint or spawned clock");
        if (index == 3)
            Require(item.geometry.mask_slice == 9 &&
                        Near(item.geometry.mask_strength, .58f),
                    "relic pickup idle did not apply its authored mask slice");
        else
            Require(item.geometry.mask_slice == 0xffffffffu &&
                        Near(item.geometry.mask_strength, 0.0f),
                    "non-relic pickup idle accepted an authored mask");
    }
}

void PickupIdlePolarRunesRejectUnauthorizedContracts()
{
    auto cooked = PickupIdlePolarRuneProgram("particle.pickup.xp.idle", false);
    auto input = PickupIdlePolarRuneInput();
    const auto run = [&] {
        return hs::runtime_detail::BuildVfxTypedGroundCommands(
            cooked, std::span(&input, 1), 712);
    };
    input.normalized_age = .1f;
    std::get<hs::VfxEntityPayload>(input.payload).lifetime01 = .1f;
    Require(run().empty(), "pickup idle polar rune accepted a predicted age");

    cooked = PickupIdlePolarRuneProgram("particle.pickup.xp.idle", false);
    input = PickupIdlePolarRuneInput();
    hs::VfxTextureBindingRecord unauthorized;
    unauthorized.role = Hash32("authored_mask_array");
    unauthorized.catalog_slot = 3;
    unauthorized.selection = Hash32("stable_seed_mod_group_size");
    unauthorized.strength = .58f;
    unauthorized.slices = {0, 2};
    cooked.texture_bindings.push_back(unauthorized);
    cooked.slice_indices = {7, 9};
    cooked.outputs[0].textures.count = 4;
    Require(run().empty(), "non-relic pickup idle accepted an authored mask");

    cooked = PickupIdlePolarRuneProgram("particle.pickup.relic_chest.idle", true);
    input = PickupIdlePolarRuneInput();
    cooked.outputs[0].textures.count = 3;
    Require(run().empty(), "relic pickup idle accepted a missing authored mask");

    cooked = PickupIdlePolarRuneProgram("particle.pickup.relic_chest.idle", true);
    input = PickupIdlePolarRuneInput();
    cooked.slice_indices[0] = 12;
    Require(run().empty(), "relic pickup idle accepted an out of range mask slice");

    cooked = StatusPolarRuneProgram("persistent.status.slow");
    input = StatusPolarRuneInput();
    const auto status_run = [&] {
        return hs::runtime_detail::BuildVfxTypedGroundCommands(
            cooked, std::span(&input, 1), 712);
    };
    hs::VfxTextureBindingRecord status_mask;
    status_mask.role = Hash32("authored_mask_array");
    status_mask.catalog_slot = 3;
    status_mask.selection = Hash32("stable_seed_mod_group_size");
    status_mask.strength = .58f;
    status_mask.slices = {0, 2};
    cooked.texture_bindings.push_back(status_mask);
    cooked.slice_indices = {7, 9};
    cooked.outputs[0].textures.count = 4;
    Require(status_run().empty(), "status polar rune accepted a pickup mask");
}

void PersistentStatusPolarRuneUsesOwnerFootprintAndClock()
{
    for (const auto effect_name : {std::string_view("persistent.status.slow"),
                                   std::string_view("persistent.status.mark")})
    {
        auto cooked = StatusPolarRuneProgram(effect_name);
        auto input = StatusPolarRuneInput();
        const auto commands = hs::runtime_detail::BuildVfxTypedGroundCommands(
            cooked, std::span(&input, 1), 712);
        Require(commands.size() == 1, "persistent status polar rune missing");
        const auto &item = commands.front();
        const auto &geometry = item.geometry;
        Require(!item.additive && geometry.shape == hs::VfxGroundShape::PolarRune &&
                item.command.primitive == hs::VfxPrimitive::ExactRing &&
                Near(geometry.outer_radius, 2.0f) &&
                Near(geometry.inner_radius, 1.965f) &&
                Near(geometry.edge_width, .035f) && geometry.spokes == 6 &&
                geometry.rings == 2 && Near(geometry.progress, .5f) &&
                Near(geometry.animation_phase, .6f),
                "persistent status polar rune geometry or clock changed");
        Require(Near(item.command.start_size_min, 2.035f) &&
                Near(item.command.stretch, 2.0f / 2.035f) &&
                Near(item.command.position.x, 4.0f) &&
                Near(item.command.position.y, .025f) &&
                Near(item.command.position.z, -3.0f) &&
                item.source_visual_kind == input.source_visual_kind &&
                item.stable_id == input.stable_id &&
                item.gradient_row == 14 && Near(item.hdr, 1.8f) &&
                Near(item.command.start_color.x, .18f) &&
                Near(item.command.start_color.w, .55f) &&
                geometry.gap_angles_radians.empty() &&
                geometry.mask_slice == 0xffffffffu &&
                Near(geometry.mask_strength, 0.0f),
                "persistent status polar rune material, identity, or coverage changed");
    }
}

void PersistentStatusPolarRuneRejectsWrongContract()
{
    auto cooked = StatusPolarRuneProgram("persistent.status.slow");
    auto input = StatusPolarRuneInput();
    const auto run = [&] {
        return hs::runtime_detail::BuildVfxTypedGroundCommands(
            cooked, std::span(&input, 1), 712);
    };
    cooked.effect_lookup[0].effect_id = Hash64("persistent.status.bleed");
    Require(run().empty(), "wrong status effect identity accepted polar rune");
    cooked = StatusPolarRuneProgram("persistent.status.slow");
    cooked.sources[0].stable_id = Hash32("ground_state");
    Require(run().empty(), "wrong status source accepted polar rune");
    cooked = StatusPolarRuneProgram("persistent.status.slow");
    cooked.sources[0].type = hs::VfxSourceType::ProjectileFollow;
    Require(run().empty(), "non-direct status source accepted polar rune");
    cooked = StatusPolarRuneProgram("persistent.status.slow");
    cooked.outputs[0].profile = hs::VfxOutputProfile::GroundSdfAdd;
    Require(run().empty(), "additive status profile accepted polar rune");
    cooked = StatusPolarRuneProgram("persistent.status.slow");
    cooked.outputs[0].shape = Hash32("exact_hitbox_ring");
    Require(run().empty(), "wrong status shape accepted polar rune");
    cooked = StatusPolarRuneProgram("persistent.status.slow");
    cooked.outputs[0].coverage_ref = Hash32("state_ring");
    Require(run().empty(), "wrong status coverage accepted polar rune");
    cooked = StatusPolarRuneProgram("persistent.status.slow");
    cooked.outputs[0].parameters.count = 2;
    Require(run().empty(), "missing polar rune parameter accepted");
    cooked = StatusPolarRuneProgram("persistent.status.slow");
    cooked.parameters[0].bits = 5;
    Require(run().empty(), "wrong polar rune spoke count accepted");
    cooked = StatusPolarRuneProgram("persistent.status.slow");
    cooked.parameters[1].type = hs::VfxParameterType::Float;
    Require(run().empty(), "wrong polar rune ring count type accepted");
    cooked = StatusPolarRuneProgram("persistent.status.slow");
    cooked.parameters[2].bits = std::bit_cast<std::uint32_t>(0.0f);
    Require(run().empty(), "zero polar rune rotation accepted");
    cooked = StatusPolarRuneProgram("persistent.status.slow");
    cooked.texture_bindings[2].role = Hash32("profile.global");
    Require(run().empty(), "wrong polar rune texture binding accepted");
    cooked = StatusPolarRuneProgram("persistent.status.slow");
    input = StatusPolarRuneInput();
    std::get<hs::VfxEntityPayload>(input.payload).footprint_radius = 0.0f;
    Require(run().empty(), "zero status owner footprint accepted");
    input = StatusPolarRuneInput();
    input.normalized_age = .6f;
    Require(run().empty(), "status owner clock mismatch accepted");
}

void PersistentChargedOverchargePolarRuneUsesOwnerChargeRatio()
{
    auto cooked = StatusPolarRuneProgram("particle.upgrade.charged.overcharge_loop");
    cooked.sources[0].knots = {0.0f, 1.0f, 0.0f, 0.0f};
    auto input = StatusPolarRuneInput();
    input.normalized_age = .75f;
    input.elapsed_seconds = 1.25f;
    std::get<hs::VfxEntityPayload>(input.payload).lifetime01 = .75f;
    const auto commands = hs::runtime_detail::BuildVfxTypedGroundCommands(
        cooked, std::span(&input, 1), 712);
    Require(commands.size() == 1 && !commands.front().additive &&
                commands.front().geometry.shape == hs::VfxGroundShape::PolarRune &&
                Near(commands.front().geometry.progress, .75f) &&
                Near(commands.front().geometry.outer_radius, 2.0f) &&
                Near(commands.front().geometry.animation_phase, .375f),
            "charged overcharge rune lost charge ratio or gameplay footprint");

    input.normalized_age = 1.0f;
    std::get<hs::VfxEntityPayload>(input.payload).lifetime01 = 1.0f;
    Require(hs::runtime_detail::BuildVfxTypedGroundCommands(
                cooked, std::span(&input, 1), 712).size() == 1,
            "charged overcharge rune stopped at a live ratio of one");

    cooked.effect_lookup[0].effect_id = Hash64("particle.upgrade.charged.full_ready");
    Require(hs::runtime_detail::BuildVfxTypedGroundCommands(
                cooked, std::span(&input, 1), 712).empty(),
            "charged overcharge rune accepted a different effect identity");
}

hs::VfxProgramData DashWakeProgram()
{
    hs::VfxProgramData cooked;
    hs::VfxEffectRecord effect;
    effect.handle = 1;
    effect.input_mode = 1;
    effect.timing_kind = 1;
    effect.payload_kind = Hash32("LineAreaPayload");
    effect.sources = {0, 1};
    cooked.effects.push_back(effect);
    hs::VfxSourceRecord source;
    source.effect = 1;
    source.type = hs::VfxSourceType::Direct;
    source.knot_count = 2;
    source.knots = {.05f, 1.0f, 0, 0};
    source.outputs = {0, 1};
    cooked.sources.push_back(source);
    hs::VfxOutputRecord output;
    output.source = 0;
    output.profile = hs::VfxOutputProfile::GroundSdfAdd;
    output.shape = Hash32("repeating_chevron_sdf");
    output.motion = hs::VfxMotionKind::ChevronsDissipate;
    output.rgba = {.3f, .4f, .5f, .38f};
    output.hdr = 1.5f;
    output.gradient_row = 9;
    output.parameters = {0, 2};
    cooked.outputs.push_back(output);
    cooked.parameters = {
        {Hash32("spacing"), hs::VfxParameterType::Float,
         std::bit_cast<std::uint32_t>(.65f)},
        {Hash32("count"), hs::VfxParameterType::Int, 7}};
    return cooked;
}

hs::VfxPersistentInput DashWakeInput()
{
    hs::VfxPersistentInput input;
    input.stable_id = 0xabcdu;
    input.effect_handle = 1;
    input.source_visual_kind = 34;
    input.normalized_age = .5f;
    input.stable_seed = 42;
    input.payload = hs::VfxLinePayload{{2, 3, 4}, {8, 7, 12}, {}, 1.6f, .5f, 0};
    return input;
}

void DashWakeChevronsFollowActualLinePath()
{
    auto cooked = DashWakeProgram();
    auto input = DashWakeInput();
    const auto run = [&] {
        return hs::runtime_detail::BuildVfxTypedGroundCommands(cooked,
            std::span(&input, 1), 600);
    };
    const auto commands = run();
    Require(commands.size() == 1, "dash wake chevrons missing");
    const auto &item = commands[0];
    Require(item.additive && item.geometry.shape == hs::VfxGroundShape::RepeatingChevron &&
            item.command.primitive == hs::VfxPrimitive::ExactRing &&
            Near(item.command.position.x, 5) && Near(item.command.position.y, .025f) &&
            Near(item.command.position.z, 8) && Near(item.geometry.direction.x, .6f) &&
            Near(item.geometry.direction.y, 0) && Near(item.geometry.direction.z, .8f),
            "dash wake lost its live ground path");
    Require(Near(item.geometry.half_length, 5) && Near(item.geometry.half_width, .8f) &&
            Near(item.geometry.spacing, .65f) && item.geometry.spokes == 7 &&
            Near(item.geometry.edge_width, .05f) &&
            Near(item.command.start_size_min, 5.05f) &&
            Near(item.geometry.progress, (.5f - .05f) / .95f) &&
            Near(item.command.start_color.x, .45f) &&
            Near(item.command.start_color.w, .38f) &&
            item.gradient_row == 9 && Near(item.hdr, 1.5f) &&
            item.stable_id == input.stable_id && item.command.seed == input.stable_seed,
            "dash wake geometry, source timing, material, or identity changed");
    input.normalized_age = .04f;
    std::get<hs::VfxLinePayload>(input.payload).lifetime01 = .04f;
    Require(run().empty(), "dash wake appeared before its authored source");
    input.normalized_age = 1.0f;
    std::get<hs::VfxLinePayload>(input.payload).lifetime01 = 1.0f;
    Require(run().empty(), "dash wake survived exclusive source end");
}

void DashWakeRejectsWrongContractAndInvalidGeometry()
{
    auto cooked = DashWakeProgram();
    auto input = DashWakeInput();
    const auto run = [&] {
        return hs::runtime_detail::BuildVfxTypedGroundCommands(cooked,
            std::span(&input, 1), 600);
    };
    cooked.outputs[0].profile = hs::VfxOutputProfile::GroundSdfSoft;
    Require(run().empty(), "wrong dash wake profile accepted");
    cooked.outputs[0].profile = hs::VfxOutputProfile::GroundSdfAdd;
    cooked.outputs[0].shape = Hash32("dual_parallel_lines");
    Require(run().empty(), "wrong dash wake shape accepted");
    cooked.outputs[0].shape = Hash32("repeating_chevron_sdf");
    cooked.outputs[0].motion = hs::VfxMotionKind::LockedChevronsTowardImpact;
    Require(run().empty(), "wrong dash wake motion accepted");
    cooked.outputs[0].motion = hs::VfxMotionKind::ChevronsDissipate;
    cooked.parameters[1].bits = 6;
    Require(run().empty(), "wrong chevron count accepted");
    cooked.parameters[1].bits = 7;
    cooked.parameters[0].bits = std::bit_cast<std::uint32_t>(0.0f);
    Require(run().empty(), "zero chevron spacing accepted");
    cooked = DashWakeProgram();
    std::get<hs::VfxLinePayload>(input.payload).end = {2, 7, 4};
    Require(run().empty(), "zero-length horizontal wake accepted");
    input = DashWakeInput();
    std::get<hs::VfxLinePayload>(input.payload).width = 0;
    Require(run().empty(), "zero-width dash path accepted");
    input = DashWakeInput();
    std::get<hs::VfxLinePayload>(input.payload).lifetime01 = .3f;
    Require(run().empty(), "inconsistent dash owner age accepted");
}

void CookedRemainingGroundOutputsConvertWhenContentIsAvailable()
{
    const auto path = std::filesystem::path("Cooked") / "vfx_program.hsbin";
    if (!std::filesystem::exists(path)) return;
    std::ifstream stream(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    hs::VfxProgramData cooked;
    std::string error;
    Require(hs::LoadVfxProgram(std::as_bytes(std::span(bytes)), cooked, error),
            "the cooked VFX program could not be loaded for remaining ground coverage");
    std::size_t authored_event{}, converted_event{}, authored_crown{}, converted_crown{};
    std::size_t authored_dash{}, converted_dash{};
    for (const auto &effect : cooked.effects)
    {
        std::size_t event_count{}, crown_count{}, dash_count{};
        for (std::size_t source_index = effect.sources.first;
             source_index < static_cast<std::size_t>(effect.sources.first) + effect.sources.count; ++source_index)
        {
            const auto &source = cooked.sources[source_index];
            if (source.type != hs::VfxSourceType::Direct) continue;
            for (std::size_t output_index = source.outputs.first;
                 output_index < static_cast<std::size_t>(source.outputs.first) + source.outputs.count; ++output_index)
            {
                const auto &output = cooked.outputs[output_index];
                if (output.profile != hs::VfxOutputProfile::GroundSdfAdd) continue;
                event_count += output.shape == Hash32("cross_ring_sdf") ||
                    output.shape == Hash32("broken_hex_sdf") || output.shape == Hash32("axial_fracture_sdf");
                crown_count += output.shape == Hash32("broken_crown_sdf") ||
                    output.shape == Hash32("closed_crown_ring_sdf");
                dash_count += output.shape == Hash32("repeating_chevron_sdf");
            }
        }
        if (event_count)
        {
            authored_event += event_count;
            hs::VfxEventInput input;
            input.effect_handle = effect.handle;
            input.event_tick = 100;
            input.sequence = effect.handle;
            input.stable_seed = 91;
            if (effect.payload_kind == Hash32("PointEventPayload"))
                input = RemainingEvent("PointEventPayload");
            else if (effect.payload_kind == Hash32("CircleAreaPayload"))
                input = RemainingEvent("CircleAreaPayload");
            else if (effect.payload_kind == Hash32("ProjectilePayload"))
                input = RemainingEvent("ProjectilePayload");
            else throw std::runtime_error("remaining cooked event output has unsupported payload schema");
            input.effect_handle = effect.handle;
            const auto tick = input.event_tick + static_cast<hs::Tick>(std::lround(.3f * effect.seconds * 60));
            const auto commands = hs::runtime_detail::BuildVfxTypedEventGroundCommands(
                cooked, std::span(&input, 1), tick);
            converted_event += std::count_if(commands.begin(), commands.end(), [](const auto &item) {
                return item.additive && (item.geometry.shape == hs::VfxGroundShape::CrossRing ||
                    item.geometry.shape == hs::VfxGroundShape::BrokenHex ||
                    item.geometry.shape == hs::VfxGroundShape::AxialFracture);
            });
        }
        if (crown_count)
        {
            authored_crown += crown_count;
            auto input = CrownInput();
            input.effect_handle = effect.handle;
            const auto commands = hs::runtime_detail::BuildVfxTypedGroundCommands(
                cooked, std::span(&input, 1), 712);
            converted_crown += std::count_if(commands.begin(), commands.end(), [](const auto &item) {
                return item.additive && (item.geometry.shape == hs::VfxGroundShape::BrokenCrown ||
                    item.geometry.shape == hs::VfxGroundShape::ClosedCrownRing);
            });
        }
        if (dash_count)
        {
            authored_dash += dash_count;
            auto input = DashWakeInput();
            input.effect_handle = effect.handle;
            const auto commands = hs::runtime_detail::BuildVfxTypedGroundCommands(
                cooked, std::span(&input, 1), 712);
            converted_dash += std::count_if(commands.begin(), commands.end(), [](const auto &item) {
                return item.additive && item.geometry.shape == hs::VfxGroundShape::RepeatingChevron;
            });
        }
    }
    Require(authored_event == 4 && converted_event == authored_event &&
            authored_crown == 2 && converted_crown == authored_crown &&
            authored_dash == 1 && converted_dash == authored_dash,
            "cooked heal/trap/axial/crown/dash output coverage changed");
}

void CookedPlayerStateRingConvertsWhenContentIsAvailable()
{
    const auto path = std::filesystem::path("Cooked") / "vfx_program.hsbin";
    if (!std::filesystem::exists(path)) return;
    std::ifstream stream(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(stream),
                                  std::istreambuf_iterator<char>()};
    hs::VfxProgramData cooked;
    std::string error;
    Require(hs::LoadVfxProgram(std::as_bytes(std::span(bytes)), cooked, error),
            "cooked VFX program failed to load for player state ring");
    std::size_t authored{}, converted{};
    for (const auto &lookup : cooked.effect_lookup)
    {
        if (lookup.effect_id != Hash64("particle.player.invulnerable_loop")) continue;
        Require(lookup.handle > 0 && lookup.handle <= cooked.effects.size(),
                "cooked player state lookup has invalid handle");
        const auto &effect = cooked.effects[lookup.handle - 1];
        for (std::size_t source_index = effect.sources.first;
             source_index < static_cast<std::size_t>(effect.sources.first) + effect.sources.count;
             ++source_index)
        {
            const auto &source = cooked.sources[source_index];
            for (std::size_t output_index = source.outputs.first;
                 output_index < static_cast<std::size_t>(source.outputs.first) + source.outputs.count;
                 ++output_index)
                authored += cooked.outputs[output_index].profile ==
                    hs::VfxOutputProfile::GroundSdfSoft &&
                    cooked.outputs[output_index].shape == Hash32("state_ring");
        }
        auto input = StateRingInput();
        input.effect_handle = lookup.handle;
        const auto commands = hs::runtime_detail::BuildVfxTypedGroundCommands(
            cooked, std::span(&input, 1), 712);
        converted += commands.size();
        if (!commands.empty())
            Require(!commands[0].additive &&
                    commands[0].geometry.shape == hs::VfxGroundShape::StateRing &&
                    commands[0].geometry.spokes == 6 &&
                    Near(commands[0].geometry.outer_radius, 1.0f),
                    "cooked player ground state lost typed segments or footprint");
    }
    Require(authored == 1 && converted == 1,
            "one cooked player state_ring output must convert");
}

void CookedPickupIdlePolarRunesConvertWhenContentIsAvailable()
{
    const auto path = std::filesystem::path("Cooked") / "vfx_program.hsbin";
    if (!std::filesystem::exists(path)) return;
    std::ifstream stream(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(stream),
                                  std::istreambuf_iterator<char>()};
    hs::VfxProgramData cooked;
    std::string error;
    Require(hs::LoadVfxProgram(std::as_bytes(std::span(bytes)), cooked, error),
            "cooked VFX program failed to load for pickup idle polar runes");
    const std::array names{
        std::string_view("particle.pickup.xp.idle"),
        std::string_view("particle.pickup.heal.idle"),
        std::string_view("particle.pickup.magnet.idle"),
        std::string_view("particle.pickup.relic_chest.idle")};
    std::size_t matched{};
    for (std::size_t index = 0; index < names.size(); ++index)
    {
        for (const auto &lookup : cooked.effect_lookup)
        {
            if (lookup.effect_id != Hash64(names[index])) continue;
            ++matched;
            Require(lookup.handle > 0 && lookup.handle <= cooked.effects.size(),
                    "cooked pickup idle lookup has invalid handle");
            auto input = PickupIdlePolarRuneInput();
            input.effect_handle = lookup.handle;
            const auto commands = hs::runtime_detail::BuildVfxTypedGroundCommands(
                cooked, std::span(&input, 1), 712);
            Require(commands.size() == 1 &&
                        commands.front().geometry.shape == hs::VfxGroundShape::PolarRune &&
                        Near(commands.front().command.position.y, .025f) &&
                        Near(commands.front().geometry.outer_radius, 2.0f),
                    "cooked pickup idle polar rune did not convert");
            if (index == 3)
                Require(commands.front().geometry.mask_slice != 0xffffffffu &&
                            Near(commands.front().geometry.mask_strength, .58f),
                        "cooked relic pickup idle lost authored mask binding");
            else
                Require(commands.front().geometry.mask_slice == 0xffffffffu &&
                            Near(commands.front().geometry.mask_strength, 0.0f),
                        "cooked non-relic pickup idle gained an authored mask");
        }
    }
    Require(matched == names.size(),
            "cooked pickup idle polar rune lookup is missing or duplicated");
}
} // namespace

int main()
{
    try
    {
        ExpandingRingUsesAuthoredSourceIntervalAndPayloadScale();
        ExpandingRingRejectsWrongContractAndMalformedParams();
        CookedExpandingRingsConvertWhenContentIsAvailable();
        CookedRetreatShockwaveCircleStaysWithinGameplayRadiusWhenContentIsAvailable();
        HexConstellationPreservesGeometryMaterialAndMask();
        HexConstellationRejectsMalformedContract();
        CookedHexConstellationsConvertWhenContentIsAvailable();
        BossSignatureKeepsGameplayFootprintsAndSafeGaps();
        BossSignatureRejectsWrongContractAndInvalidGeometry();
        CookedBossSignaturesConvertWhenContentIsAvailable();
        ConvertsHealTrapAndAxialWithTheirOwnGeometry();
        RemainingEventsRejectWrongContractsAndGeometry();
        PersistentCrownsUseActualOwnerFootprintAndPhase();
        PlayerStateRingKeepsOwnerAndCookedContract();
        PlayerStateRingRejectsWrongTupleAndOwner();
        PickupIdlePolarRunesUsePresenceClockAndRelicMask();
        PickupIdlePolarRunesRejectUnauthorizedContracts();
        ChargedPulseUsesContextRatioAndAuthoredGroundOutputs();
        ChargedPulseRejectsMalformedContextAndCookedContract();
        CookedChargedPulseGroundOutputsConvertWhenContentIsAvailable();
        PersistentStatusPolarRuneUsesOwnerFootprintAndClock();
        PersistentStatusPolarRuneRejectsWrongContract();
        PersistentChargedOverchargePolarRuneUsesOwnerChargeRatio();
        DashWakeChevronsFollowActualLinePath();
        DashWakeRejectsWrongContractAndInvalidGeometry();
        CookedRemainingGroundOutputsConvertWhenContentIsAvailable();
        CookedPlayerStateRingConvertsWhenContentIsAvailable();
        CookedPickupIdlePolarRunesConvertWhenContentIsAvailable();
        SafeSectorPreservesScheduledFootprint();
        OwnedRingPreviewUsesIntegratedPulse();
        CircleWarningUsesOwnerClockAndExactRadius();
        RingGapsPreservesGeometryAndOwnerAge();
        RingGapsRejectsMalformedGeometry();
        ConvertsOnlyAuthoredDirectExactRings();
        RejectsUnsupportedOrMissingGeometry();
        PreservesSharedSourceRingAndFill();
        ConvertsFixedTimeCircleEvents();
        HonorsEventSourceEnvelopeAndSchema();
        ConvertsLineGeometryAndAuthoredParams();
        RejectsMalformedLineGeometryAndCone();
        ConvertsLineEventTiming();
        ConvertsFixedConeGroundEvents();
        UsesAuthoritativeWarningWindow();
        std::cout << "VFX typed ground commands passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
