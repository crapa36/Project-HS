#include "vfx_typed_frame_adapter.hpp"

#include <hs/game_domain/domain_signal.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace
{
void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

std::uint32_t Hash32(std::string_view value)
{
    std::uint32_t hash = 2166136261u;
    for (const unsigned char character : value)
    {
        hash ^= character;
        hash *= 16777619u;
    }
    return hash;
}

std::uint64_t Hash64(std::string_view value)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char character : value)
    {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    return hash;
}

hs::VfxProgramData Program(std::initializer_list<std::tuple<std::uint64_t, std::uint32_t,
                                                            std::uint32_t, std::uint32_t>> entries)
{
    hs::VfxProgramData program;
    for (const auto &[effect_id, handle, input_mode, payload_kind] : entries)
    {
        hs::VfxEffectRecord effect;
        effect.handle = handle;
        effect.input_mode = input_mode;
        effect.payload_kind = payload_kind;
        program.effects.push_back(effect);
        program.effect_lookup.push_back({effect_id, handle});
    }
    return program;
}

void SetLifetime(hs::PersistentVfxVisual &visual, hs::Tick active, hs::Tick expires)
{
    visual.active_tick = active;
    visual.expires = expires;
}
void TestPointEvent()
{
    const auto effect_id = Hash64("particle.common.hit");
    auto program = Program({{effect_id, 1, 0, Hash32("PointEventPayload")} });
    hs::PresentationEvent event;
    event.kind = hs::PresentationKind::Vfx;
    event.asset = {effect_id};
    event.sequence = 0x100000002ull;
    event.position = {1.0f, 2.0f, 3.0f};
    event.parameters = hs::EncodeVfxParameters({{0.0f, 0.0f, 1.0f}, 1.75f, {}, {}});
    const std::array events{event};
    const auto result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, events, {}, {}, 4);
    Check(result.events.size() == 1, "point event was not adapted");
    Check(result.unsupported_effect_ids.empty(), "valid point event was diagnosed");
    const auto &point = std::get<hs::VfxPointPayload>(result.events.front().payload);
    Check(point.position.x == 1.0f && point.position.y == 2.0f && point.position.z == 3.0f,
          "point position changed");
    Check(point.authored_scale == 1.75f, "point scale changed");

    event.parameters = hs::EncodeVfxParameters({{0.0f, 0.0f, 1.0f}, 0.0f, {}, {}});
    const std::array invalid_events{event};
    const auto rejected = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, invalid_events, {}, {}, 4);
    Check(rejected.events.empty() && rejected.unsupported_effect_ids.size() == 1,
          "point event accepted nonpositive authored scale");
}

void TestProjectileImpactEventUsesAuthoritativeGeometry()
{
    const auto effect_id = Hash64("particle.enemy.ranged.impact");
    auto program = Program({{effect_id, 1, 0, Hash32("ProjectilePayload")} });
    hs::PresentationEvent event;
    event.kind = hs::PresentationKind::Vfx;
    event.asset = {effect_id};
    event.position = {4.0f, 0.45f, -2.0f};
    event.geometry.kind = hs::PresentationGeometryKind::Projectile;
    event.geometry.velocity = {0.0f, 0.0f, 24.0f};
    event.geometry.radius = 0.18f;
    event.geometry.source_id = 712;
    const auto valid = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, std::array{event}, {}, {}, 30);
    Check(valid.events.size() == 1 && valid.unsupported_effect_ids.empty(),
          "projectile impact geometry was not adapted");
    const auto &payload = std::get<hs::VfxProjectilePayload>(valid.events.front().payload);
    Check(payload.velocity.z == 24.0f && payload.hitbox_radius == 0.18f &&
              payload.hitbox_half_extents.x == 0.18f,
          "projectile impact changed authoritative velocity or radius");

    event.geometry.velocity = {};
    const auto invalid = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, std::array{event}, {}, {}, 30);
    Check(invalid.events.empty() && invalid.unsupported_effect_ids.size() == 1,
          "zero-velocity projectile impact was accepted");
}

void TestEventSchemaAndModeRejection()
{
    const auto circle_id = Hash64("particle.skill.damage_area.pulse");
    const auto persistent_id = Hash64("particle.basic_attack");
    auto program = Program({
        {circle_id, 1, 0, Hash32("CircleAreaPayload")},
        {persistent_id, 2, 1, Hash32("PointEventPayload")},
    });
    hs::PresentationEvent first;
    first.kind = hs::PresentationKind::Vfx;
    first.asset = {circle_id};
    first.geometry.kind = hs::PresentationGeometryKind::Circle;
    first.geometry.radius = 3.0f;
    first.parameters = hs::EncodeVfxParameters({{0.0f, 0.0f, 1.0f}, 0.0f, {}, {}});
    hs::PresentationEvent second = first;
    second.asset = {persistent_id};
    const std::array events{first, second};
    const auto result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, events, {}, {}, 4);
    Check(result.events.size() == 1, "authored circle event was not adapted");
    Check(result.unsupported_effect_ids.size() == 1, "event rejection diagnostics missing");
    const auto &circle = std::get<hs::VfxCirclePayload>(result.events.front().payload);
    Check(circle.radius == 3.0f, "circle geometry was changed");
}

void TestEventGeometryTagsAndGapExpansion()
{
    const auto multishot_id = Hash64("particle.skill.multishot");
    const auto cone_id = Hash64("particle.boss.volley.release");
    const auto ring_id = Hash64("particle.boss.shockwave.release");
    auto program = Program({
        {cone_id, 1, 0, Hash32("ConePayload")},
        {ring_id, 2, 0, Hash32("RingWithGapsPayload")},
        {multishot_id, 3, 0, Hash32("ConePayload")},
    });
    hs::PresentationEvent cone;
    cone.kind = hs::PresentationKind::Vfx;
    cone.asset = {cone_id};
    cone.sequence = 42;
    cone.parameters = hs::EncodeVfxParameters({{0.0f, 0.0f, 1.0f}, 1.0f, {}, {}});
    cone.geometry.kind = hs::PresentationGeometryKind::Cone;
    cone.geometry.range = 18.0f;
    cone.geometry.half_angle_degrees = 22.0f;
    hs::PresentationEvent ring = cone;
    ring.asset = {ring_id};
    ring.geometry = {};
    ring.geometry.kind = hs::PresentationGeometryKind::RingGaps;
    ring.geometry.inner_radius = 2.0f;
    ring.geometry.outer_radius = 9.0f;
    ring.geometry.gap_count = 3;
    ring.geometry.gap_offset_degrees = 7.0f;
    ring.geometry.gap_half_width_degrees = 4.0f;
    const std::array events{cone, ring};
    const auto result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, events, {}, {}, 4);
    Check(result.unsupported_effect_ids.empty() && result.events.size() == 2,
          "tagged event geometry was rejected");
    Check(result.events[0].sequence == 42, "event sequence was not retained");
    const auto &cone_payload = std::get<hs::VfxConePayload>(result.events[0].payload);
    Check(cone_payload.range == 18.0f && cone_payload.half_angle_degrees == 22.0f,
          "cone geometry was changed");
    const auto &ring_payload = std::get<hs::VfxRingGapsPayload>(result.events[1].payload);
    Check(ring_payload.gap_angles_degrees.size() == 3 &&
              ring_payload.gap_angles_degrees[0] == 7.0f &&
              ring_payload.gap_angles_degrees[1] == 127.0f &&
              ring_payload.gap_angles_degrees[2] == 247.0f,
          "ring gaps were not expanded from authored spacing");

    hs::PresentationEvent wrong = cone;
    wrong.geometry.kind = hs::PresentationGeometryKind::RingGaps;
    wrong.geometry.inner_radius = 1.0f;
    wrong.geometry.outer_radius = 2.0f;
    wrong.geometry.gap_count = 1;
    const std::array wrong_events{wrong};
    const auto rejected = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, wrong_events, {}, {}, 4);
    Check(rejected.events.empty() && rejected.unsupported_effect_ids.size() == 1,
          "mismatched geometry tag was accepted");

    hs::PresentationEvent multishot = cone;
    multishot.asset = {multishot_id};
    multishot.geometry = {};
    const std::array untagged_multishot{multishot};
    const auto untagged_result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, untagged_multishot, {}, {}, 4);
    Check(untagged_result.events.empty() && untagged_result.unsupported_effect_ids.size() == 1,
          "untagged multishot cone was accepted");
    multishot.geometry.kind = hs::PresentationGeometryKind::Cone;
    multishot.geometry.range = 23.0f;
    multishot.geometry.half_angle_degrees = 31.0f;
    const std::array tagged_multishot{multishot};
    const auto tagged_result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, tagged_multishot, {}, {}, 4);
    Check(tagged_result.events.size() == 1 && tagged_result.unsupported_effect_ids.empty(),
          "tagged multishot cone was rejected");
    const auto &multishot_payload = std::get<hs::VfxConePayload>(
        tagged_result.events.front().payload);
    Check(std::abs(multishot_payload.range - 23.0f) < 0.0001f &&
              std::abs(multishot_payload.half_angle_degrees - 31.0f) < 0.0001f,
          "multishot cone geometry was changed by the adapter");
}

void TestArrowRainPullLinkGeometry()
{
    const auto effect_id = Hash64("particle.upgrade.arrow_rain.pull");
    auto program = Program({{effect_id, 1, 0, Hash32("SourceTargetPayload")} });
    hs::PresentationEvent event{};
    event.kind = hs::PresentationKind::Vfx;
    event.asset = {effect_id};
    event.sequence = 77;
    event.tick = 100;
    event.position = {4.0f, 1.05f, -2.0f};
    event.parameters = hs::EncodeVfxParameters({{1.0f, 0.0f, 0.0f}, 1.0f, {}, {}});
    event.geometry.kind = hs::PresentationGeometryKind::Line;
    event.geometry.width = 0.12f;
    event.geometry.source_id = 712;
    event.geometry.end_position = {3.25f, 1.05f, -2.0f};
    event.upgrade_cast_id = 19;
    event.upgrade_owner_id = 712;
    const auto valid = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, std::array{event}, {}, {}, 100);
    Check(valid.events.size() == 1 && valid.unsupported_effect_ids.empty(),
          "ArrowRain pull source-target geometry was not adapted");
    const auto &link = std::get<hs::VfxLinkPayload>(valid.events.front().payload);
    Check(link.source_position.x == event.position.x &&
              link.target_position.x == event.geometry.end_position.x &&
              link.width == event.geometry.width && link.source_id == 712 &&
              link.target_id == 712,
          "ArrowRain pull link geometry or identity changed");

    auto malformed = event;
    malformed.geometry.source_id = 0;
    Check(hs::runtime_detail::BuildVfxTypedFrameInputs(
              program, std::array{malformed}, {}, {}, 100).events.empty(),
          "source-target link accepted missing source identity");
    malformed = event;
    malformed.upgrade_owner_id = 713;
    Check(hs::runtime_detail::BuildVfxTypedFrameInputs(
              program, std::array{malformed}, {}, {}, 100).events.empty(),
          "source-target link accepted mismatched owner identity");
    malformed = event;
    malformed.upgrade_cast_id = 0;
    Check(hs::runtime_detail::BuildVfxTypedFrameInputs(
              program, std::array{malformed}, {}, {}, 100).events.empty(),
          "source-target link accepted missing cast identity");
    malformed = event;
    malformed.geometry.end_position = malformed.position;
    Check(hs::runtime_detail::BuildVfxTypedFrameInputs(
              program, std::array{malformed}, {}, {}, 100).events.empty(),
          "source-target link accepted coincident endpoints");
    malformed = event;
    malformed.geometry.width = std::numeric_limits<float>::quiet_NaN();
    Check(hs::runtime_detail::BuildVfxTypedFrameInputs(
              program, std::array{malformed}, {}, {}, 100).events.empty(),
          "source-target link accepted nonfinite width");

    auto wrong_schema = Program({{effect_id, 1, 0, Hash32("PointEventPayload")} });
    Check(hs::runtime_detail::BuildVfxTypedFrameInputs(
              wrong_schema, std::array{event}, {}, {}, 100).events.empty(),
          "line geometry was accepted for a non-link payload schema");
}

void TestPickupCollectLinkPayload()
{
    const auto payload = Hash32("SourceTargetPayload");
    const auto xp_id = Hash64("particle.pickup.xp_collect");
    const auto heal_id = Hash64("particle.pickup.heal_collect");
    const auto magnet_id = Hash64("particle.pickup.magnet_collect");
    const auto relic_id = Hash64("particle.pickup.relic_collect");
    const auto wrong_id = Hash64("particle.pickup.xp_spawn");
    auto program = Program({
        {xp_id, 1, 0, payload},
        {heal_id, 2, 0, payload},
        {magnet_id, 3, 0, payload},
        {relic_id, 4, 0, payload},
        {wrong_id, 5, 0, payload},
    });

    const auto make_event = [](std::uint64_t effect_id, hs::Sequence sequence,
                               hs::Float3 position, hs::Float3 target) {
        hs::PresentationEvent event{};
        event.kind = hs::PresentationKind::Vfx;
        event.asset = {effect_id};
        event.sequence = sequence;
        event.tick = 320;
        event.position = position;
        event.parameters = hs::EncodeVfxParameters({
            {0.0f, 0.0f, 1.0f}, 1.0f, target,
            static_cast<std::uint32_t>(hs::VfxEventFlag::HasTarget)});
        return event;
    };

    const hs::Float3 source{2.0f, 0.4f, -3.0f};
    const hs::Float3 target{7.0f, 1.0f, 5.0f};
    const auto valid = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program,
        std::array{
            make_event(xp_id, 101, source, target),
            make_event(heal_id, 102, source, target),
            make_event(magnet_id, 103, source, target),
            make_event(relic_id, 104, source, target),
        },
        {}, {}, 320);
    Check(valid.events.size() == 4 && valid.unsupported_effect_ids.empty(),
          "pickup collect SourceTarget events were not adapted");
    for (const auto &input : valid.events)
    {
        const auto &link = std::get<hs::VfxLinkPayload>(input.payload);
        Check(link.source_position.x == source.x && link.source_position.y == source.y &&
                  link.source_position.z == source.z && link.target_position.x == target.x &&
                  link.target_position.y == target.y && link.target_position.z == target.z &&
                  link.source_id == 0 && link.target_id == 0 && link.width == 0.0f,
              "pickup collect link changed source, target, identity, or authored width");
    }

    auto missing_target = make_event(xp_id, 105, source, target);
    missing_target.parameters = hs::EncodeVfxParameters({
        {0.0f, 0.0f, 1.0f}, 1.0f, target, 0});
    const auto missing = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, std::array{missing_target}, {}, {}, 320);
    Check(missing.events.empty() && missing.unsupported_effect_ids.size() == 1,
          "pickup collect event without HasTarget was accepted");

    const auto coincident = make_event(heal_id, 106, source, source);
    const auto coincident_result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, std::array{coincident}, {}, {}, 320);
    Check(coincident_result.events.size() == 1 &&
              coincident_result.unsupported_effect_ids.empty(),
          "coincident pickup collect source and target were rejected");
    const auto &coincident_link = std::get<hs::VfxLinkPayload>(
        coincident_result.events.front().payload);
    Check(coincident_link.source_position.x == coincident_link.target_position.x &&
              coincident_link.source_position.y == coincident_link.target_position.y &&
              coincident_link.source_position.z == coincident_link.target_position.z,
          "coincident pickup collect source and target changed");

    const auto wrong_effect = make_event(wrong_id, 107, source, target);
    const auto wrong = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, std::array{wrong_effect}, {}, {}, 320);
    Check(wrong.events.empty() && wrong.unsupported_effect_ids.size() == 1,
          "unlisted pickup effect accepted SourceTarget adaptation");
}

void TestContextAllowsZeroRatio()
{
    const auto effect_id = Hash64("particle.context.zero_ratio");
    auto program = Program({{effect_id, 1, 0, Hash32("PresentationContextPayload")} });
    hs::PresentationEvent event;
    event.kind = hs::PresentationKind::Vfx;
    event.asset = {effect_id};
    event.parameters = hs::EncodeVfxParameters({{0.0f, 0.0f, 1.0f}, 0.0f, {}, {}});
    const std::array events{event};
    const auto result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, events, {}, {}, 4);
    Check(result.unsupported_effect_ids.empty() && result.events.size() == 1,
          "context event rejected zero ratio");
    const auto &context = std::get<hs::VfxContextPayload>(result.events.front().payload);
    Check(context.owner_id == 0 && context.ratio01 == 0.0f,
          "unrelated context defaults were changed");
}

void TestChargedPulseContextMetadata()
{
    const auto effect_id = Hash64("particle.skill.charged_shot.pulse");
    auto program = Program({{effect_id, 1, 0, Hash32("PresentationContextPayload")} });
    hs::PresentationEvent event;
    event.kind = hs::PresentationKind::Vfx;
    event.asset = {effect_id};
    event.sequence = 77;
    event.position = {2.0f, 1.05f, -3.0f};
    event.parameters = hs::EncodeVfxParameters({{1.0f, 0.0f, 0.0f}, 1.0f, {}, {}});
    event.upgrade_stage = static_cast<std::uint8_t>(hs::UpgradeVisualStage::Spawn);
    event.upgrade_cast_id = 19;
    event.upgrade_owner_id = 1ull << 60;
    event.vfx_ratio01 = 0.375f;
    const std::array events{event};
    const auto result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, events, {}, {}, 12);
    Check(result.events.size() == 1 && result.unsupported_effect_ids.empty(),
          "upgraded charged pulse was not adapted");
    const auto &context = std::get<hs::VfxContextPayload>(result.events.front().payload);
    Check(context.owner_id == event.upgrade_owner_id &&
              context.ratio01 == event.vfx_ratio01 &&
              hs::DecodeVfxParameters(event.parameters).scale == 1.0f,
          "charged pulse context changed owner, ratio, or legacy scale");

    for (const auto ratio : {-0.01f, 1.01f,
                             std::numeric_limits<float>::quiet_NaN()})
    {
        auto malformed = event;
        malformed.vfx_ratio01 = ratio;
        const auto rejected = hs::runtime_detail::BuildVfxTypedFrameInputs(
            program, std::array{malformed}, {}, {}, 12);
        Check(rejected.events.empty() && rejected.unsupported_effect_ids.size() == 1,
              "malformed upgraded charged pulse ratio was accepted");
    }
}

void TestSafeSectorFanout()
{
    const auto warning_id = Hash64("particle.boss.shockwave.telegraph");
    const auto marker_id = Hash64("particle.boss.shockwave.safe_gap_marker");
    auto program = Program({{warning_id, 1, 0, Hash32("RingWithGapsPayload")},
                            {marker_id, 2, 0, Hash32("SafeGapSectorPayload")}});
    program.effects[1].timing_kind = 1;
    hs::PresentationEvent event{};
    event.kind = hs::PresentationKind::Vfx; event.asset = {warning_id}; event.sequence = 831; event.tick = 10;
    event.position = {3.0f, 0.025f, 4.0f};
    event.geometry.kind = hs::PresentationGeometryKind::RingGaps;
    event.geometry.inner_radius = 1.7f; event.geometry.outer_radius = 5.0f;
    event.geometry.source_id = 831; event.geometry.start_tick = 10; event.geometry.end_tick = 83;
    event.geometry.gap_count = 3; event.geometry.gap_half_width_degrees = 20; event.geometry.gap_offset_degrees = 359;
    auto inputs = hs::runtime_detail::BuildVfxTypedFrameInputs(program, std::array{event}, {}, {}, 40);
    Check(inputs.events.size() == 4 && inputs.unsupported_effect_ids.empty(), "safe marker did not fan out by actual gap count");
    std::vector<hs::VfxEventInput> markers;
    for (const auto &input : inputs.events)
    {
        if (input.geometry_effect_id != marker_id) continue;
        const auto &sector = std::get<hs::VfxSafeSectorPayload>(input.payload);
        const auto angle = (359.0f + 120.0f * input.geometry_sector_index) * 0.017453292519943295f;
        Check(std::abs(sector.direction.x - std::cos(angle)) < 0.0001f &&
              std::abs(sector.direction.z - std::sin(angle)) < 0.0001f &&
              sector.center.x == 3 && sector.inner_radius == 1.7f && sector.outer_radius == 5 &&
              sector.half_angle_degrees == 20 && input.geometry_owner_id == 831 &&
              input.geometry_start_tick == 10 && input.geometry_end_tick == 83,
              "safe sector changed authoritative angle, radii or timing");
        markers.push_back(input);
    }
    Check(markers.size() == 3 && markers[0].stable_seed != markers[1].stable_seed, "safe sector seed identity collapsed");
    hs::PersistentVfxVisual owner{};
    owner.kind = hs::PersistentVfxKind::BossShockwaveWarning; owner.stable_id = 831;
    owner.position = event.position; owner.radius = 5; owner.ring_inner_radius = 1.7f; owner.ring_outer_radius = 5;
    owner.active_tick = 10; owner.expires = 83; owner.gap_count = 2;
    owner.gap_half_angle_degrees = 20; owner.gap_offset_degrees = 0;
    hs::runtime_detail::RefreshVfxGroundEventOwners(markers, std::array{owner}, 40);
    Check(markers.size() == 2 &&
          std::get<hs::VfxSafeSectorPayload>(markers[1].payload).direction.x < -0.999f,
          "safe sector refresh did not update angle or remove invalid sector index");
    auto reloaded = Program({{Hash64("unrelated.effect"), 1, 0, Hash32("PointEventPayload")},
                             {warning_id, 2, 0, Hash32("RingWithGapsPayload")},
                             {marker_id, 3, 0, Hash32("SafeGapSectorPayload")}});
    reloaded.effects[2].timing_kind = 1;
    hs::runtime_detail::RebindVfxGroundEventOwners(reloaded, markers);
    Check(markers.size() == 2 && markers[1].effect_handle == 3 && markers[1].geometry_sector_index == 1,
          "safe sector reload lost index or exact recipe identity");
    auto cancelled = markers;
    hs::runtime_detail::RefreshVfxGroundEventOwners(cancelled, {}, 41);
    Check(cancelled.empty(), "safe sector survived cancelled owner");
    reloaded.effects[2].timing_kind = 0;
    hs::runtime_detail::RebindVfxGroundEventOwners(reloaded, markers);
    Check(markers.empty(), "safe sector reload accepted fixed timing instead of gameplay timing");
    event.geometry.gap_count = 0;
    Check(hs::runtime_detail::BuildVfxTypedFrameInputs(program, std::array{event}, {}, {}, 40).events.size() == 1,
          "zero-gap warning fabricated safe markers");
}

void TestShockwaveWarningOwner()
{
    const auto id = Hash64("particle.boss.shockwave.telegraph");
    auto program = Program({{id, 1, 0, Hash32("RingWithGapsPayload")}});
    hs::PresentationEvent event{};
    event.kind = hs::PresentationKind::Vfx; event.asset = {id}; event.sequence = 831; event.tick = 10;
    event.position = {3.0f, 0.025f, 4.0f};
    event.geometry.kind = hs::PresentationGeometryKind::RingGaps;
    event.geometry.inner_radius = 1.7f; event.geometry.outer_radius = 5.0f;
    event.geometry.source_id = 831; event.geometry.start_tick = 10; event.geometry.end_tick = 83;
    event.geometry.gap_count = 3; event.geometry.gap_half_width_degrees = 20; event.geometry.gap_offset_degrees = 359;
    auto inputs = hs::runtime_detail::BuildVfxTypedFrameInputs(program, std::array{event}, {}, {}, 40);
    Check(inputs.events.size() == 1 && inputs.events[0].geometry_effect_id == id,
          "shockwave warning event lacks exact owned identity");
    hs::PersistentVfxVisual owner{};
    owner.kind = hs::PersistentVfxKind::BossShockwaveWarning; owner.stable_id = 831;
    owner.position = event.position; owner.radius = 5; owner.ring_inner_radius = 1.7f; owner.ring_outer_radius = 5;
    owner.active_tick = 10; owner.expires = 83; owner.gap_count = 3;
    owner.gap_half_angle_degrees = 20; owner.gap_offset_degrees = 359;
    hs::runtime_detail::RefreshVfxGroundEventOwners(inputs.events, std::array{owner}, 40);
    Check(inputs.events.size() == 1, "shockwave warning owner missing");
    const auto &ring = std::get<hs::VfxRingGapsPayload>(inputs.events[0].payload);
    Check(ring.center.x == 3 && ring.center.z == 4 && ring.inner_radius == 1.7f && ring.outer_radius == 5 &&
          ring.gap_angles_degrees == std::vector<float>{359,119,239} &&
          std::abs(ring.lifetime01 - 30.0f / 73.0f) < 0.0001f,
          "shockwave owner refresh changed fixed geometry or clock");
    auto rebound = Program({{Hash64("unrelated.effect"), 1, 0, Hash32("PointEventPayload")},
                            {id, 2, 0, Hash32("RingWithGapsPayload")}});
    hs::runtime_detail::RebindVfxGroundEventOwners(rebound, inputs.events);
    Check(inputs.events.size() == 1 && inputs.events[0].effect_handle == 2 &&
          inputs.events[0].geometry_owner_id == 831 && inputs.events[0].event_tick == 10,
          "shockwave reload lost exact identity or restarted lifetime");
    hs::runtime_detail::RefreshVfxGroundEventOwners(inputs.events, {}, 41);
    Check(inputs.events.empty(), "cancelled shockwave warning survived missing owner");
    Check(hs::runtime_detail::BuildVfxTypedFrameInputs(program, {}, std::array{owner}, {}, 40).persistent.empty(),
          "shockwave warning incorrectly used persistent recipe mode");
}

void TestShockwaveWavefrontBinding()
{
    const auto effect_id = Hash64("particle.boss.shockwave.wavefront");
    auto program = Program({{effect_id, 1, 1, Hash32("RingWithGapsPayload")}});
    hs::PersistentVfxVisual visual{};
    visual.kind = hs::PersistentVfxKind::BossShockwaveWavefront;
    visual.stable_id = (4ull << 60) | 719;
    visual.position = {3.0f, 0.03f, -4.0f};
    visual.ring_inner_radius = 5.6f; visual.ring_outer_radius = 7.0f;
    visual.radius = 7.0f; visual.gap_count = 3;
    visual.gap_offset_degrees = 359.0f; visual.gap_half_angle_degrees = 12.0f;
    SetLifetime(visual, 100, 140);
    const auto result = hs::runtime_detail::BuildVfxTypedFrameInputs(program, {}, std::array{visual}, {}, 120);
    Check(result.persistent.size() == 1 && result.unsupported_effect_ids.empty(), "wavefront binding missing");
    const auto &input = result.persistent.front();
    const auto &ring = std::get<hs::VfxRingGapsPayload>(input.payload);
    Check(input.stable_id == visual.stable_id && input.effect_handle == 1 &&
          input.normalized_age == 0.5f && std::abs(input.elapsed_seconds - 1.0f / 3.0f) < 0.0001f &&
          ring.center.x == 3.0f && ring.inner_radius == 5.6f && ring.outer_radius == 7.0f &&
          ring.lifetime01 == 0.5f && ring.gap_half_width_degrees == 12.0f &&
          ring.gap_angles_degrees == std::vector<float>{359.0f, 119.0f, 239.0f},
          "wavefront payload changed actual annulus, gap rotation or owner age");
    visual.gap_count = 0; visual.gap_half_angle_degrees = 0;
    const auto continuous = hs::runtime_detail::BuildVfxTypedFrameInputs(program, {}, std::array{visual}, {}, 100);
    Check(continuous.persistent.size() == 1 &&
          std::get<hs::VfxRingGapsPayload>(continuous.persistent.front().payload).gap_angles_degrees.empty(),
          "wavefront fabricated gaps in continuous annulus");
    Check(hs::runtime_detail::BuildVfxTypedFrameInputs(program, {}, std::array{visual}, {}, 99).persistent.empty() &&
          hs::runtime_detail::BuildVfxTypedFrameInputs(program, {}, std::array{visual}, {}, 140).persistent.empty(),
          "wavefront escaped scheduled active lifetime");
    program.effects.front().payload_kind = Hash32("CircleAreaPayload");
    Check(hs::runtime_detail::BuildVfxTypedFrameInputs(program, {}, std::array{visual}, {}, 120).persistent.empty(),
          "wavefront was downgraded to a circle payload");
}

void TestCircleLifetimeAndCompoundIdentity()
{
    const auto slow_id = Hash64("particle.status.slow_area");
    const auto upgrade_slow_id = Hash64("persistent.area.slow");
    const auto fire_id = Hash64("persistent.area.fire");
    auto program = Program({
        {slow_id, 1, 1, Hash32("CircleAreaPayload")},
        {fire_id, 2, 1, Hash32("CircleAreaPayload")},
        {upgrade_slow_id, 3, 1, Hash32("CircleAreaPayload")},
    });
    hs::PersistentVfxVisual slow;
    slow.position = {1.0f, 0.0f, 2.0f};
    slow.radius = 2.5f;
    slow.kind = hs::PersistentVfxKind::SlowArea;
    slow.stable_id = 77;
    SetLifetime(slow, 10, 30);
    hs::PersistentVfxVisual fire = slow;
    fire.kind = hs::PersistentVfxKind::FireArea;
    fire.radius = 1.0f;
    fire.yaw = 0.4f;
    hs::PersistentVfxVisual upgrade_slow = slow;
    upgrade_slow.kind = hs::PersistentVfxKind::UpgradeSlowArea;
    const std::array current{slow, fire, upgrade_slow};
    const auto result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, current, {}, 20);
    Check(result.persistent.size() == 3, "same owner distinct effects were collapsed");
    Check(result.persistent[0].stable_id == 77 && result.persistent[1].stable_id == 77 &&
          result.persistent[2].stable_id == 77,
          "persistent stable identity changed");
    Check(result.persistent[0].effect_handle != result.persistent[1].effect_handle &&
          result.persistent[0].effect_handle != result.persistent[2].effect_handle,
          "distinct persistent handles were collapsed");
    Check(result.persistent[0].source_visual_kind ==
              static_cast<std::uint8_t>(hs::PersistentVfxKind::SlowArea) &&
          result.persistent[1].source_visual_kind ==
          static_cast<std::uint8_t>(hs::PersistentVfxKind::FireArea) &&
          result.persistent[2].source_visual_kind ==
              static_cast<std::uint8_t>(hs::PersistentVfxKind::UpgradeSlowArea),
          "source visual kinds were collapsed across a compound owner");
    for (const auto &input : result.persistent)
    {
        Check(std::abs(input.normalized_age - 0.5f) < 0.0001f,
              "persistent lifetime was not normalized from bounds");
        const auto &circle = std::get<hs::VfxCirclePayload>(input.payload);
        Check(circle.radius > 0.0f && std::abs(circle.lifetime01 - 0.5f) < 0.0001f,
              "circle geometry or age was fabricated");
    }
}

hs::PersistentVfxVisual UpgradeSlowAreaVisual(std::uint8_t skill,
                                              std::uint8_t source_upgrade,
                                              std::uint64_t cast_id)
{
    hs::PersistentVfxVisual visual{};
    visual.kind = hs::PersistentVfxKind::UpgradeSlowArea;
    visual.stable_id = 0x4000000000000042ull;
    visual.status_episode_generation = 17;
    visual.position = {4.0f, 0.018f, -2.0f};
    visual.radius = 2.75f;
    visual.active_tick = 100;
    visual.expires = 160;
    visual.cast_id = cast_id;
    visual.skill = skill;
    visual.source_upgrade = source_upgrade;
    return visual;
}

void TestUpgradeSlowAreaAliases()
{
    // SkillKind is intentionally opaque in the Core snapshot contract. These
    // values are the stable gameplay ordinals for ArrowRain and Trap.
    constexpr std::uint8_t arrow_rain_skill = 6;
    constexpr std::uint8_t trap_skill = 7;
    const auto base_id = Hash64("persistent.area.slow");
    const auto trap_id = Hash64("particle.upgrade.trap.land_slow");
    const auto arrow_rain_id = Hash64("particle.upgrade.arrow_rain.finish_slow");
    auto program = Program({
        {base_id, 1, 1, Hash32("CircleAreaPayload")},
        {trap_id, 2, 1, Hash32("CircleAreaPayload")},
        {arrow_rain_id, 3, 1, Hash32("CircleAreaPayload")},
    });
    program.upgrade_bindings = {
        {Hash64("skill.trap"), 2, {0, 1}},
        {Hash64("skill.arrow_rain"), 8, {1, 1}},
    };
    program.upgrade_sequences = {2, 3};

    const auto trap = UpgradeSlowAreaVisual(trap_skill, 1, 0xA1);
    const auto trap_result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, std::array{trap}, {}, 130);
    Check(trap_result.unsupported_effect_ids.empty() &&
              trap_result.persistent.size() == 1 &&
              trap_result.persistent.front().effect_handle == 2,
          "trap landing slow did not select its authored alias");
    const auto &trap_input = trap_result.persistent.front();
    const auto &trap_circle = std::get<hs::VfxCirclePayload>(trap_input.payload);
    Check(trap_input.stable_id == trap.stable_id &&
              trap_input.status_episode_generation == trap.status_episode_generation &&
              trap_input.source_visual_kind ==
                  static_cast<std::uint8_t>(hs::PersistentVfxKind::UpgradeSlowArea) &&
              trap_input.current_transform[12] == trap.position.x &&
              trap_input.current_transform[14] == trap.position.z &&
              trap_input.normalized_age == 0.5f &&
              trap_circle.center.x == trap.position.x &&
              trap_circle.center.z == trap.position.z &&
              trap_circle.radius == trap.radius &&
              trap_circle.lifetime01 == 0.5f,
          "trap alias changed the area payload, age, owner, or legacy key");

    const auto arrow_rain = UpgradeSlowAreaVisual(arrow_rain_skill, 7, 0xA2);
    const auto arrow_rain_result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, std::array{arrow_rain}, {}, 130);
    Check(arrow_rain_result.unsupported_effect_ids.empty() &&
              arrow_rain_result.persistent.size() == 1 &&
              arrow_rain_result.persistent.front().effect_handle == 3,
          "arrow rain finish slow did not select its authored alias");

    // ArrowRain upgrade 6 uses the same UpgradeSlowArea visual kind but keeps
    // the generic persistent area recipe because its source is not ordinal 7.
    const auto arrow_rain_upgrade_six =
        UpgradeSlowAreaVisual(arrow_rain_skill, 5, 0xA3);
    const auto baseline_result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, std::array{arrow_rain_upgrade_six}, {}, 130);
    Check(baseline_result.unsupported_effect_ids.empty() &&
              baseline_result.persistent.size() == 1 &&
              baseline_result.persistent.front().effect_handle == 1,
          "non-finish arrow rain slow area did not keep its baseline recipe");

    const auto no_cast_alias_metadata =
        UpgradeSlowAreaVisual(arrow_rain_skill, 7, 0);
    const auto no_cast_result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, std::array{no_cast_alias_metadata}, {}, 130);
    Check(no_cast_result.unsupported_effect_ids.empty() &&
              no_cast_result.persistent.size() == 1 &&
              no_cast_result.persistent.front().effect_handle == 1,
          "slow alias was selected without an authoritative cast ID");
}

void TestUpgradeSlowAreaAliasRequiresCookedMembership()
{
    constexpr std::uint8_t trap_skill = 7;
    const auto base_id = Hash64("persistent.area.slow");
    const auto trap_id = Hash64("particle.upgrade.trap.land_slow");
    auto program = Program({
        {base_id, 1, 1, Hash32("CircleAreaPayload")},
        {trap_id, 2, 1, Hash32("CircleAreaPayload")},
    });
    program.upgrade_bindings = {{Hash64("skill.trap"), 2, {0, 1}}};
    // The binding exists but points at the baseline effect, so the selected
    // alias must diagnose instead of silently falling back to handle 1.
    program.upgrade_sequences = {1};
    const auto visual = UpgradeSlowAreaVisual(trap_skill, 1, 0xB1);
    const auto missing_member = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, std::array{visual}, {}, 130);
    Check(missing_member.persistent.empty() &&
              missing_member.unsupported_effect_ids.size() == 1 &&
              missing_member.unsupported_effect_ids.front() == trap_id,
          "trap slow alias fell back when cooked binding membership was missing");

    program.upgrade_sequences = {2};
    program.effects[1].input_mode = 0;
    const auto malformed_effect = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, std::array{visual}, {}, 130);
    Check(malformed_effect.persistent.empty() &&
              malformed_effect.unsupported_effect_ids.size() == 1 &&
              malformed_effect.unsupported_effect_ids.front() == trap_id,
          "malformed trap slow alias recipe was accepted or fell back");
}

void TestDamageTrailUpgradeSupplements()
{
    constexpr std::uint8_t piercing_skill = 1;
    constexpr std::uint8_t retreat_skill = 8;
    const auto generic_id = Hash64("persistent.trail.damage");
    const auto retreat_id = Hash64("particle.upgrade.retreat.slow_trail");
    const auto pulse_id = Hash64("particle.skill.piercing_shot.trail_pulse");
    const auto line = Hash32("LineAreaPayload");
    auto program = Program({
        {generic_id, 1, 1, line},
        {retreat_id, 2, 1, line},
        {pulse_id, 3, 1, line},
    });
    program.effects[1].timing_kind = 1;
    program.effects[2].timing_kind = 0;
    program.effects[2].seconds = 0.55f;
    program.upgrade_bindings = {
        {Hash64("skill.retreat_shot"), 3, {0, 2}},
        {Hash64("skill.piercing_shot"), 4, {2, 2}},
    };
    program.upgrade_sequences = {2, 1, 1, 3};

    const auto make_trail = [](std::uint8_t skill, std::uint8_t source_upgrade,
                               std::uint64_t cast_id, std::uint64_t stable_id) {
        hs::PersistentVfxVisual visual{};
        visual.kind = hs::PersistentVfxKind::DamageTrail;
        visual.stable_id = stable_id;
        visual.position = {4.0f, 0.014f, -2.0f};
        visual.yaw = 0.35f;
        visual.radius = 0.4f;
        visual.length = 12.0f;
        visual.active_tick = 100;
        visual.expires = 200;
        visual.cast_id = cast_id;
        visual.skill = skill;
        visual.source_upgrade = source_upgrade;
        return visual;
    };
    const auto retreat = make_trail(retreat_skill, 2, 0xA1, 41);
    const auto piercing = make_trail(piercing_skill, 3, 0xA2, 42);
    const auto other = make_trail(retreat_skill, 1, 0xA3, 43);
    const auto no_cast = make_trail(retreat_skill, 2, 0, 44);
    const auto active = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, std::array{retreat, piercing, other, no_cast}, {}, 116);
    Check(active.unsupported_effect_ids.empty() && active.persistent.size() == 6,
          "qualified damage trails did not keep their generic and authored inputs");

    const hs::VfxPersistentInput *retreat_generic = nullptr;
    const hs::VfxPersistentInput *retreat_slow = nullptr;
    const hs::VfxPersistentInput *piercing_generic = nullptr;
    const hs::VfxPersistentInput *piercing_pulse = nullptr;
    const hs::VfxPersistentInput *other_generic = nullptr;
    for (const auto &input : active.persistent)
    {
        if (input.stable_id == retreat.stable_id && input.effect_handle == 1)
            retreat_generic = &input;
        else if (input.effect_handle == 2)
            retreat_slow = &input;
        else if (input.stable_id == piercing.stable_id && input.effect_handle == 1)
            piercing_generic = &input;
        else if (input.effect_handle == 3)
            piercing_pulse = &input;
        else if (input.stable_id == other.stable_id && input.effect_handle == 1)
            other_generic = &input;
    }
    Check(retreat_generic && retreat_slow && piercing_generic && piercing_pulse &&
              other_generic,
          "damage trail owner mappings produced duplicate or missing inputs");
    const auto &retreat_line = std::get<hs::VfxLinePayload>(retreat_generic->payload);
    const auto &slow_line = std::get<hs::VfxLinePayload>(retreat_slow->payload);
    const auto &piercing_line = std::get<hs::VfxLinePayload>(piercing_generic->payload);
    const auto &pulse_line = std::get<hs::VfxLinePayload>(piercing_pulse->payload);
    Check(retreat_slow->stable_id != retreat.stable_id &&
              retreat_line.start.x == slow_line.start.x &&
              retreat_line.end.z == slow_line.end.z &&
              retreat_line.width == slow_line.width &&
              piercing_pulse->stable_id != piercing.stable_id &&
              piercing_line.start.x == pulse_line.start.x &&
              piercing_line.end.z == pulse_line.end.z &&
              piercing_line.width == pulse_line.width,
          "authored damage trail supplements changed owner geometry or identity");
    std::size_t no_cast_inputs = 0;
    for (const auto &input : active.persistent)
        if (input.stable_id == no_cast.stable_id) ++no_cast_inputs;
    Check(no_cast_inputs == 1,
          "damage trail supplement was duplicated without an authoritative cast ID");
    const auto pulse_elapsed = 16.0f / 60.0f;
    Check(std::abs(piercing_pulse->elapsed_seconds - pulse_elapsed) < 0.00001f &&
              std::abs(piercing_pulse->normalized_age - pulse_elapsed / 0.55f) < 0.00001f &&
              std::abs(pulse_line.lifetime01 - pulse_elapsed / 0.55f) < 0.00001f &&
              other_generic->effect_handle == 1 &&
              std::abs(other_generic->normalized_age - 0.16f) < 0.00001f,
          "damage trail elapsed or normalized age was not authored correctly");

    const auto expired_pulse = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, std::array{piercing}, {}, 133);
    Check(expired_pulse.unsupported_effect_ids.empty() &&
              expired_pulse.persistent.size() == 1 &&
              expired_pulse.persistent.front().effect_handle == 1,
          "piercing trail pulse survived its authored 0.55 second window");

    auto missing_member = program;
    missing_member.upgrade_bindings[1].sequence = {2, 1};
    const auto missing = hs::runtime_detail::BuildVfxTypedFrameInputs(
        missing_member, {}, std::array{piercing}, {}, 116);
    Check(missing.persistent.size() == 1 &&
              missing.persistent.front().effect_handle == 1 &&
              missing.unsupported_effect_ids.size() == 1 &&
              missing.unsupported_effect_ids.front() == pulse_id,
          "piercing pulse rendered without both ordinal-4 cooked members");

    auto invalid_mode = program;
    invalid_mode.effects[1].input_mode = 0;
    const auto malformed = hs::runtime_detail::BuildVfxTypedFrameInputs(
        invalid_mode, {}, std::array{retreat}, {}, 116);
    Check(malformed.persistent.size() == 1 &&
              malformed.persistent.front().effect_handle == 1 &&
              malformed.unsupported_effect_ids.size() == 1 &&
              malformed.unsupported_effect_ids.front() == retreat_id,
          "retreat slow trail accepted an invalid persistent recipe mode");
}

void TestBossAreaUsesAuthoredCircleBinding()
{
    const auto effect_id = Hash64("particle.boss.area.active_loop");
    auto program = Program({{effect_id, 1, 1, Hash32("CircleAreaPayload")}});
    hs::PersistentVfxVisual visual;
    visual.kind = hs::PersistentVfxKind::BossAreaActive;
    visual.stable_id = 901;
    visual.position = {2.0f, 0.025f, 3.0f};
    visual.radius = 2.75f;
    SetLifetime(visual, 20, 40);
    const std::array current{visual};
    const auto result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, current, {}, 25);
    Check(result.unsupported_effect_ids.empty() && result.persistent.size() == 1,
          "boss area was not bound to its persistent authored effect");
    const auto &input = result.persistent.front();
    const auto &circle = std::get<hs::VfxCirclePayload>(input.payload);
    Check(input.stable_id == 901 && input.effect_handle == 1 &&
          input.source_visual_kind ==
              static_cast<std::uint8_t>(hs::PersistentVfxKind::BossAreaActive) &&
          std::abs(circle.radius - 2.75f) < 0.0001f &&
          std::abs(circle.lifetime01 - 0.25f) < 0.0001f,
          "boss area radius or lifetime was changed before rendering");
}

void TestProjectileIngressUsesAuthoritativeOwnerState()
{
    const auto effect_id = Hash64("particle.basic_attack");
    auto program = Program({{effect_id, 1, 1, Hash32("ProjectilePayload")}});
    hs::PersistentVfxVisual visual;
    visual.kind = hs::PersistentVfxKind::ProjectileHead;
    visual.stable_id = 0x3000000000000042ull;
    visual.position = {8.0f, 1.05f, 2.0f};
    visual.effect_asset = {effect_id};
    visual.projectile_current_position = {8.0f, 1.05f, 2.0f};
    visual.projectile_previous_position = {7.0f, 1.05f, 2.0f};
    visual.projectile_velocity = {60.0f, 0.0f, 0.0f};
    visual.projectile_hitbox_radius = 0.15f;
    visual.projectile_spawned_tick = 100;
    visual.projectile_owner_id = 42;
    visual.projectile_state_flags = 3;
    const std::array current{visual};
    const auto result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, current, {}, 130);
    Check(result.unsupported_effect_ids.empty() && result.persistent.size() == 1,
          "authoritative projectile head was not adapted");
    const auto &input = result.persistent.front();
    const auto &payload = std::get<hs::VfxProjectilePayload>(input.payload);
    Check(input.elapsed_seconds > 0.49f && input.elapsed_seconds < 0.51f &&
          input.effect_handle == 1 &&
          input.normalized_age == 0.0f && payload.lifetime01 == 0.0f &&
          payload.velocity.x == 60.0f && payload.state_flags == 3,
          "projectile lifetime or owner state was forecast instead of copied");

    const auto absent = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, {}, current, 131);
    Check(absent.persistent.empty(), "despawned projectile remained active");
}

void TestProjectilePathUsesAuthoritativeMotion()
{
    const auto effect_id = Hash64("persistent.projectile_trail");
    auto program = Program({{effect_id, 1, 1, Hash32("ProjectilePathPayload")}});
    hs::PersistentVfxVisual visual;
    visual.kind = hs::PersistentVfxKind::ProjectileTrail;
    visual.stable_id = 0x3000000000000071ull;
    visual.position = {99.0f, 9.0f, 99.0f}; // legacy cosmetic anchor
    visual.projectile_current_position = {4.0f, 1.0f, 6.0f};
    visual.projectile_previous_position = {3.0f, 1.0f, 6.0f};
    visual.projectile_velocity = {60.0f, 0.0f, 2.0f};
    visual.projectile_hitbox_radius = 0.2f;
    visual.projectile_spawned_tick = 20;
    visual.projectile_owner_id = 71;
    const std::array current{visual};
    const auto result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, current, {}, 21);
    Check(result.unsupported_effect_ids.empty() && result.persistent.size() == 1,
          "authoritative projectile path was not adapted");
    const auto &path = std::get<hs::VfxProjectilePathPayload>(
        result.persistent.front().payload);
    Check(path.current_position.x == 4.0f && path.previous_position.x == 3.0f &&
          path.velocity.x == 60.0f && path.velocity.z == 2.0f &&
          path.projectile_radius == 0.2f && path.lifetime01 == 0.0f,
          "projectile path used legacy cosmetic or frame delta motion");
}

void TestHostileProjectileBindings()
{
    const auto enemy_id = Hash64("persistent.enemy.projectile_visual");
    const auto boss_id = Hash64("persistent.boss.volley_projectile_visual");
    auto program = Program({
        {enemy_id, 1, 1, Hash32("ProjectilePayload")},
        {boss_id, 2, 1, Hash32("ProjectilePayload")} });
    hs::PersistentVfxVisual enemy;
    enemy.kind = hs::PersistentVfxKind::ProjectileHead;
    enemy.stable_id = 501;
    enemy.effect_asset = {enemy_id};
    enemy.projectile_current_position = {1.0f, 0.5f, 1.0f};
    enemy.projectile_previous_position = {0.0f, 0.5f, 1.0f};
    enemy.projectile_velocity = {1.0f, 0.0f, 0.0f};
    enemy.projectile_hitbox_radius = 0.1f;
    enemy.projectile_spawned_tick = 1;
    enemy.projectile_owner_id = 501;
    auto boss = enemy;
    boss.stable_id = 502;
    boss.effect_asset = {boss_id};
    boss.projectile_owner_id = 502;
    const std::array current{enemy, boss};
    const auto result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, current, {}, 2);
    Check(result.unsupported_effect_ids.empty() && result.persistent.size() == 2,
          "hostile projectile bindings were not resolved");
    Check(result.persistent[0].effect_handle == 1 &&
              result.persistent[1].effect_handle == 2,
          "enemy and boss projectile effects were conflated");
}
}

namespace
{
void TestChargeGuideUsesPresenceInsteadOfExpiry()
{
    const auto program = Program({{Hash64("persistent.charge_guide"), 1, 1, Hash32("LineAreaPayload")}});
    hs::PersistentVfxVisual guide;
    guide.kind = hs::PersistentVfxKind::ChargeGuide;
    guide.stable_id = 93;
    guide.position = {4.0f, 0.025f, 8.0f};
    guide.yaw = 0.7f;
    guide.radius = 0.8f;
    guide.length = 23.0f;
    guide.active_tick = 10;
    const std::array current{guide};
    const auto decoded = hs::runtime_detail::BuildVfxTypedFrameInputs(program, {}, current, {}, 6010);
    Check(decoded.persistent.size() == 1 && decoded.unsupported_effect_ids.empty(),
          "held charge guide requires a fabricated expiry");
    const auto &item = decoded.persistent.front();
    const auto &segment = std::get<hs::VfxLinePayload>(item.payload);
    Check(std::abs(item.elapsed_seconds - 100.0f) < 0.001f && item.normalized_age == 0.0f &&
          std::abs(segment.width - 1.6f) < 0.0001f &&
          std::abs(std::hypot(segment.end.x-segment.start.x, segment.end.z-segment.start.z)-23.0f) < 0.0001f,
          "charge age or exact guide dimensions changed");
    Check(hs::runtime_detail::BuildVfxTypedFrameInputs(program, {}, {}, current, 6011).persistent.empty(),
          "charge guide survived release");
    Check(hs::runtime_detail::BuildVfxTypedFrameInputs(program, {}, current, {}, 9).persistent.empty(),
          "charge guide rendered before authoritative start");
}

void TestWarningOwnerRefresh()
{
    const auto dash_id = Hash64("particle.boss.dash.telegraph");
    const auto volley_id = Hash64("particle.boss.volley.telegraph");
    auto program = Program({
        {dash_id, 1, 0, Hash32("LineAreaPayload")},
        {volley_id, 2, 0, Hash32("ConePayload")} });
    hs::PresentationEvent event;
    event.kind = hs::PresentationKind::Vfx;
    event.asset = {dash_id};
    event.sequence = 1001;
    event.position = {0.0f, 0.0f, 0.0f};
    event.parameters = hs::EncodeVfxParameters({{1.0f, 0.0f, 0.0f}, 1.0f, {}, {}});
    event.geometry.kind = hs::PresentationGeometryKind::Line;
    event.geometry.width = 1.0f;
    event.geometry.end_position = {10.0f, 0.0f, 0.0f};
    event.geometry.source_id = 77;
    event.geometry.start_tick = 100;
    event.geometry.end_tick = 160;
    const auto decoded = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, std::array{event}, {}, {}, 110);
    Check(decoded.events.size() == 1 && decoded.events[0].geometry_owner_id == 77 &&
              decoded.events[0].geometry_effect_id == dash_id &&
              decoded.events[0].geometry_start_tick == 100 &&
              decoded.events[0].geometry_end_tick == 160,
          "boss warning owner metadata was not retained");

    hs::VfxEventInput line = decoded.events[0];
    hs::VfxEventInput sibling;
    sibling.payload = hs::VfxPointPayload{};
    sibling.event_tick = 110;
    sibling.geometry_owner_id = 0;
    const auto original_sibling = sibling;
    hs::PersistentVfxVisual owner;
    owner.kind = hs::PersistentVfxKind::BossDashWarning;
    owner.stable_id = 77;
    owner.position = {5.0f, 0.4f, 7.0f};
    owner.yaw = 1.57079632679f;
    owner.radius = 0.6f;
    owner.length = 22.0f;
    SetLifetime(owner, 120, 240);
    hs::PersistentVfxVisual unrelated = owner;
    unrelated.stable_id = 999;
    std::vector events{line, sibling};
    hs::runtime_detail::RefreshVfxGroundEventOwners(events, std::array{owner, unrelated}, 130);
    Check(events.size() == 2 && events[1].geometry_owner_id == original_sibling.geometry_owner_id,
          "owner refresh removed or altered an unrelated event");
    const auto &updated_line = std::get<hs::VfxLinePayload>(events[0].payload);
    Check(std::abs(updated_line.start.x - (-6.0f)) < 0.0001f &&
              std::abs(updated_line.end.x - 16.0f) < 0.0001f &&
              std::abs(updated_line.start.z - 7.0f) < 0.0001f &&
              std::abs(updated_line.end.z - 7.0f) < 0.0001f &&
              std::abs(updated_line.width - 1.2f) < 0.0001f &&
              events[0].geometry_start_tick == 120 && events[0].geometry_end_tick == 240,
          "line warning was not retargeted from its live owner");

    hs::VfxEventInput future = line;
    future.event_tick = 500;
    std::vector future_events{future};
    hs::PersistentVfxVisual absent = owner;
    absent.stable_id = 700;
    hs::runtime_detail::RefreshVfxGroundEventOwners(future_events, std::array{absent}, 130);
    Check(future_events.size() == 1 && future_events[0].event_tick == 500,
          "future warning event was erased before its event tick");
    std::vector cancelled{line};
    hs::runtime_detail::RefreshVfxGroundEventOwners(
        cancelled, std::span<const hs::PersistentVfxVisual>{}, 130);
    Check(cancelled.empty(), "cancelled warning owner remained in the event stream");

    hs::VfxEventInput cone;
    cone.effect_handle = 2;
    cone.event_tick = 100;
    cone.geometry_owner_id = 88;
    cone.geometry_effect_id = volley_id;
    cone.payload = hs::VfxConePayload{};
    hs::PersistentVfxVisual cone_owner = owner;
    cone_owner.kind = hs::PersistentVfxKind::BossVolleyWarning;
    cone_owner.stable_id = 88;
    cone_owner.position = {-2.0f, 0.1f, 3.0f};
    cone_owner.yaw = 0.0f;
    cone_owner.radius = 19.0f;
    cone_owner.cone_half_angle_degrees = 73.0f;
    SetLifetime(cone_owner, 100, 180);
    std::vector cone_events{cone};
    hs::runtime_detail::RefreshVfxGroundEventOwners(cone_events, std::array{cone_owner}, 120);
    Check(cone_events.size() == 1, "cone warning owner was rejected");
    const auto &updated_cone = std::get<hs::VfxConePayload>(cone_events[0].payload);
    Check(std::abs(updated_cone.origin.x + 2.0f) < 0.0001f &&
              std::abs(updated_cone.range - 19.0f) < 0.0001f &&
              std::abs(updated_cone.half_angle_degrees - 73.0f) < 0.0001f,
          "cone warning was not retargeted from its live owner");
    cone_owner.kind = hs::PersistentVfxKind::BossDashWarning;
    hs::runtime_detail::RefreshVfxGroundEventOwners(cone_events, std::array{cone_owner}, 120);
    Check(cone_events.empty(), "wrong owner warning kind was accepted");

    hs::VfxEventInput retained_line = line;
    hs::VfxEventInput retained_cone = cone;
    retained_cone.geometry_owner_id = 88;
    std::vector retained{retained_line, retained_cone, sibling};
    auto reloaded = Program({
        {volley_id, 1, 0, Hash32("ConePayload")},
        {dash_id, 2, 0, Hash32("LineAreaPayload")} });
    hs::runtime_detail::RebindVfxGroundEventOwners(reloaded, retained);
    Check(retained.size() == 2 && retained[0].effect_handle == 2 &&
              retained[1].effect_handle == 1 && retained[0].geometry_owner_id == 77 &&
              retained[1].geometry_owner_id == 88,
          "owner-backed warning handles were not rebound after reload");
    auto missing = Program({{dash_id, 4, 1, Hash32("LineAreaPayload")} });
    hs::runtime_detail::RebindVfxGroundEventOwners(missing, retained);
    Check(retained.empty(), "warnings with missing or mismatched recipes survived reload");
}

void TestRangedWarningIdentity()
{
    const auto ranged_id = Hash64("particle.enemy.ranged.telegraph_line");
    const auto dash_id = Hash64("particle.boss.dash.telegraph");
    auto program = Program({
        {dash_id, 1, 0, Hash32("LineAreaPayload")},
        {ranged_id, 2, 0, Hash32("LineAreaPayload")} });
    hs::PresentationEvent event;
    event.kind = hs::PresentationKind::Vfx;
    event.asset = {ranged_id};
    event.tick = 100;
    event.sequence = 901;
    event.parameters = hs::EncodeVfxParameters({{0, 0, 1}, 1, {}, {}});
    event.geometry.kind = hs::PresentationGeometryKind::Line;
    event.geometry.width = .4f;
    event.geometry.end_position = {0, 0, 14};
    event.geometry.source_id = 55;
    event.geometry.start_tick = 100;
    event.geometry.end_tick = 140;
    hs::PersistentVfxVisual ranged;
    ranged.kind = hs::PersistentVfxKind::RangedEnemyWarning;
    ranged.stable_id = 55;
    ranged.position = {3, .1f, 7}; ranged.radius = .2f; ranged.length = 14;
    SetLifetime(ranged, 100, 140);
    auto dash = ranged;
    dash.kind = hs::PersistentVfxKind::BossDashWarning;
    dash.position = {99, .2f, 0}; dash.length = 30; dash.radius = 1;
    auto decoded = hs::runtime_detail::BuildVfxTypedFrameInputs(program, std::array{event}, std::array{ranged, dash}, {}, 110);
    Check(decoded.events.size() == 1 && decoded.persistent.empty() && decoded.unsupported_effect_ids.empty(),
          "ranged warning ingress failed or owner entered normal persistent path");
    Check(decoded.events[0].geometry_effect_id == ranged_id && decoded.events[0].geometry_owner_id == 55,
          "ranged stable recipe identity was lost");
    auto boss = decoded.events[0]; boss.geometry_effect_id = dash_id; boss.effect_handle = 1;
    std::vector events{decoded.events[0], boss};
    hs::runtime_detail::RefreshVfxGroundEventOwners(events, std::array{dash, ranged}, 120);
    Check(events.size() == 2 && std::abs(std::get<hs::VfxLinePayload>(events[0].payload).start.x - 3) < .0001f &&
          std::abs(std::get<hs::VfxLinePayload>(events[1].payload).start.x - 99) < .0001f,
          "same-ID ranged and boss line owners were confused");
    auto reload = Program({
        {ranged_id, 1, 0, Hash32("LineAreaPayload")},
        {dash_id, 2, 0, Hash32("LineAreaPayload")} });
    hs::runtime_detail::RebindVfxGroundEventOwners(reload, events);
    Check(events.size() == 2 && events[0].effect_handle == 1 && events[1].effect_handle == 2 &&
          events[0].geometry_effect_id == ranged_id && events[0].geometry_start_tick == 100 && events[0].geometry_end_tick == 140,
          "catalog reordering confused the two line recipes or reset owner clocks");
    hs::runtime_detail::RefreshVfxGroundEventOwners(events, std::array{dash}, 125);
    Check(events.size() == 1 && events[0].geometry_effect_id == dash_id,
          "cancelled ranged warning was preserved using boss owner");
    auto invalid = boss; invalid.geometry_effect_id = 0;
    std::vector unknown{invalid};
    hs::runtime_detail::RebindVfxGroundEventOwners(reload, unknown);
    Check(unknown.empty(), "unknown recipe identity inferred from line shape");
    auto wrong_shape = decoded.events[0]; wrong_shape.payload = hs::VfxConePayload{};
    std::vector malformed{wrong_shape};
    hs::runtime_detail::RefreshVfxGroundEventOwners(malformed, std::array{ranged}, 120);
    Check(malformed.empty(), "ranged recipe accepted a cone payload");

}
void TestOwnedVisualLinks()
{
    auto program=Program({{Hash64("particle.line.ricochet"),1,1,Hash32("SourceTargetPayload")}});
    program.effects[0].timing_kind=1;
    hs::PersistentVfxVisual visual;
    visual.stable_id=918;visual.kind=hs::PersistentVfxKind::RicochetLink;
    visual.position={1,.7f,2};visual.link_target_position={5,.7f,9};visual.link_width=.09f;
    visual.active_tick=100;visual.expires=108;
    auto decode=[&](hs::Tick tick){return hs::runtime_detail::BuildVfxTypedFrameInputs(program,{},std::span(&visual,1),{},tick);};
    const auto result=decode(104);
    Check(result.persistent.size()==1&&result.unsupported_effect_ids.empty(),"live owned link missing");
    const auto &input=result.persistent.front();const auto &link=std::get<hs::VfxLinkPayload>(input.payload);
    Check(link.source_position.x==1&&link.target_position.z==9&&link.width==.09f&&link.lifetime01==.5f,
          "link exact geometry or owner clock lost");
    Check(link.source_id==0&&link.target_id==0&&input.stable_id==918,"link fabricated entity IDs");
    Check(decode(99).persistent.empty()&&decode(108).persistent.empty(),"link outside owner interval");
    auto oldseed=input.stable_seed;program.effects.push_back(program.effects[0]);program.effects[1].handle=2;program.effect_lookup[0].handle=2;
    Check(decode(104).persistent[0].stable_seed==oldseed,"link reorder changed semantic seed");
    visual.link_target_position=visual.position;Check(decode(104).persistent.empty(),"degenerate link accepted");
}

void TestEnemyStatusOwners()
{
 auto program=Program({{Hash64("persistent.status.bleed"),1,1,Hash32("EntityAttachmentPayload")}});program.effects[0].timing_kind=1;
 hs::PersistentVfxVisual owner;owner.kind=hs::PersistentVfxKind::EnemyBleedStatus;owner.effect_asset={Hash64("persistent.status.bleed")};owner.stable_id=91;owner.position={3,0,4};owner.radius=.73f;owner.active_tick=10;owner.expires=110;owner.entity_health_fraction=.6f;owner.entity_state_flags=1;owner.entity_render_id=0x2000000000000003ull;
 auto previous=owner;previous.position.x=2;
 const auto run=[&](hs::Tick tick){return hs::runtime_detail::BuildVfxTypedFrameInputs(program,{},std::span(&owner,1),std::span(&previous,1),tick);};
 auto a=run(35);Check(a.persistent.size()==1,"status entity owner not adapted");const auto &payload=std::get<hs::VfxEntityPayload>(a.persistent[0].payload);
 Check(payload.footprint_radius==.73f&&payload.health_fraction==.6f&&payload.lifetime01==.25f&&payload.render_instance_id==owner.entity_render_id&&a.persistent[0].previous_transform[12]==2&&a.persistent[0].current_transform[12]==3,"status geometry, target mesh, health, age or history lost");
 owner.expires=210;auto b=run(35);Check(b.persistent[0].stable_id==a.persistent[0].stable_id&&b.persistent[0].stable_seed==a.persistent[0].stable_seed&&b.persistent[0].normalized_age==.125f,"refresh restarted owner instead of extending actual episode");
 Check(run(210).persistent.empty()&&run(9).persistent.empty(),"status accepted outside actual interval");
 owner.effect_asset={Hash64("persistent.status.burn")};Check(run(35).persistent.empty(),"status exact identity not checked");owner.effect_asset={Hash64("persistent.status.bleed")};program.effects[0].payload_kind=Hash32("CircleAreaPayload");Check(run(35).persistent.empty(),"status coerced to circle");
 Check(hs::runtime_detail::BuildVfxTypedFrameInputs(program,{},{},std::span(&previous,1),35).persistent.empty(),"absent status persisted");
}

void TestStatusTickIdentity()
{
    const auto bleed_id = Hash64("particle.status.bleed_tick");
    const auto burn_id = Hash64("particle.status.burn_tick");
    const auto generic_id = Hash64("particle.common.hit");
    auto program = Program({
        {bleed_id, 1, 0, Hash32("PointEventPayload")},
        {burn_id, 2, 0, Hash32("PointEventPayload")},
        {generic_id, 3, 0, Hash32("PointEventPayload")},
    });
    for (const auto [effect_id, owner_id] : std::array{
             std::pair{bleed_id, 0x1001ull}, std::pair{burn_id, 0x2002ull}})
    {
        hs::PresentationEvent event;
        event.kind = hs::PresentationKind::Vfx;
        event.asset = {effect_id};
        event.sequence = owner_id + 7;
        event.tick = 42;
        event.position = {1.0f, 2.0f, 3.0f};
        event.parameters = hs::EncodeVfxParameters({{0.0f, 0.0f, 1.0f}, 1.75f, {}, {}});
        event.geometry.source_id = owner_id;
        const auto result = hs::runtime_detail::BuildVfxTypedFrameInputs(
            program, std::array{event}, {}, {}, event.tick);
        Check(result.events.size() == 1 && result.unsupported_effect_ids.empty(),
              "status tick event was not adapted");
        Check(result.events.front().geometry_owner_id == owner_id,
              "status tick owner identity was not retained");
        const auto &point = std::get<hs::VfxPointPayload>(result.events.front().payload);
        Check(point.position.x == 1.0f && point.position.y == 2.0f && point.position.z == 3.0f &&
                  point.authored_scale == 1.75f,
              "status tick point rendering input changed");
    }

    hs::PresentationEvent unrelated;
    unrelated.kind = hs::PresentationKind::Vfx;
    unrelated.asset = {generic_id};
    unrelated.sequence = 10;
    unrelated.position = {1.0f, 2.0f, 3.0f};
    unrelated.parameters = hs::EncodeVfxParameters({{0.0f, 0.0f, 1.0f}, 1.0f, {}, {}});
    unrelated.geometry.source_id = 0x3003ull;
    const auto generic = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, std::array{unrelated}, {}, {}, unrelated.tick);
    Check(generic.events.size() == 1 && generic.events.front().geometry_owner_id == 0,
          "non-status point event acquired status owner identity");
}

void TestBossStateOwners()
{
    auto program = Program({
        {Hash64("persistent.boss.dash_wake"), 1, 1, Hash32("LineAreaPayload")},
        {Hash64("persistent.boss.phase2_aura"), 2, 1, Hash32("EntityAttachmentPayload")},
        {Hash64("persistent.boss.phase_transition_invulnerable"), 3, 1, Hash32("EntityAttachmentPayload")},
    });
    for (auto &effect : program.effects) effect.timing_kind = 1;
    hs::PersistentVfxVisual dash;
    dash.kind = hs::PersistentVfxKind::BossDashWake;
    dash.effect_asset = {Hash64("persistent.boss.dash_wake")};
    dash.stable_id = 201; dash.position = {4, .025f, 8}; dash.yaw = 0;
    dash.radius = .7f; dash.length = 6; SetLifetime(dash, 100, 130);
    hs::PersistentVfxVisual aura;
    aura.kind = hs::PersistentVfxKind::BossPhase2Aura;
    aura.effect_asset = {Hash64("persistent.boss.phase2_aura")};
    aura.stable_id = 202; aura.position = {5, .025f, 9}; aura.radius = 1.2f;
    aura.active_tick = 110; aura.entity_health_fraction = .49f; aura.entity_state_flags = 2;
    aura.entity_render_id = 0x20000000000002bdull;
    auto transition = aura;
    transition.kind = hs::PersistentVfxKind::BossPhaseTransition;
    transition.effect_asset = {Hash64("persistent.boss.phase_transition_invulnerable")};
    transition.stable_id = 203; transition.expires = 170; transition.entity_state_flags = 3;
    const std::array owners{dash, aura, transition};
    const auto active = hs::runtime_detail::BuildVfxTypedFrameInputs(program, {}, owners, {}, 120);
    Check(active.unsupported_effect_ids.empty() && active.persistent.size() == 3,
          "live boss state owners were not adapted");
    const auto &line = std::get<hs::VfxLinePayload>(active.persistent[0].payload);
    Check(line.start.z == 5 && line.end.z == 11 && line.width == 1.4f &&
              active.persistent[0].normalized_age > 0,
          "dash wake lost swept path or active clock");
    const auto &phase = std::get<hs::VfxEntityPayload>(active.persistent[1].payload);
    const auto &locked = std::get<hs::VfxEntityPayload>(active.persistent[2].payload);
    Check(phase.footprint_radius == 1.2f && phase.lifetime01 == .5f &&
              phase.render_instance_id == aura.entity_render_id &&
              active.persistent[1].elapsed_seconds > 0 &&
              std::abs(locked.lifetime01 - 1.0f / 6.0f) < .00001f &&
              active.persistent[2].state_flags == 3,
          "phase2 presence or transition lifetime was invented");
    const auto later = hs::runtime_detail::BuildVfxTypedFrameInputs(program, {}, owners, {}, 170);
    Check(later.persistent.size() == 1 && later.persistent[0].effect_handle == 2 &&
              later.persistent[0].stable_id == 202,
          "expired dash/transition remained or live aura disappeared");
    Check(hs::runtime_detail::BuildVfxTypedFrameInputs(program, {}, {}, owners, 120).persistent.empty(),
          "absent boss owners persisted");
}

void TestPlayerStateOwners()
{
    auto program = Program({
        {Hash64("particle.player.invulnerable_loop"), 1, 1, Hash32("EntityAttachmentPayload")},
        {Hash64("post.player.low_health_vignette"), 2, 1, Hash32("PresentationContextPayload")},
    });
    for (auto &effect : program.effects) effect.timing_kind = 1;
    hs::PersistentVfxVisual shell;
    shell.kind = hs::PersistentVfxKind::PlayerInvulnerableLoop;
    shell.effect_asset = {Hash64("particle.player.invulnerable_loop")};
    shell.stable_id = 1ull << 60;
    shell.entity_render_id = shell.stable_id;
    shell.radius = 1.0f;
    shell.active_tick = 100;
    shell.expires = 160;
    shell.position = {2, 0, 3};
    hs::PersistentVfxVisual low;
    low.kind = hs::PersistentVfxKind::PlayerLowHealthVignette;
    low.stable_id = shell.stable_id | 1ull;
    low.entity_render_id = shell.stable_id;
    low.entity_health_fraction = .25f;
    low.active_tick = 130;
    low.expires = 131;
    const std::array owners{shell, low};
    const auto active = hs::runtime_detail::BuildVfxTypedFrameInputs(program, {}, owners, {}, 130);
    Check(active.persistent.size() == 2 && active.unsupported_effect_ids.empty(),
          "player state owners missing");
    const auto &entity = std::get<hs::VfxEntityPayload>(active.persistent[0].payload);
    const auto &context = std::get<hs::VfxContextPayload>(active.persistent[1].payload);
    Check(entity.render_instance_id == shell.entity_render_id &&
              entity.lifetime01 == .5f && active.persistent[0].elapsed_seconds == .5f &&
              context.ratio01 == .25f && context.owner_id == shell.stable_id,
          "player shell clock/mesh or low-health ratio lost");
    const auto expired = hs::runtime_detail::BuildVfxTypedFrameInputs(program, {}, owners, {}, 160);
    Check(expired.persistent.empty(), "expired player state remained active");
    shell.effect_asset = {};
    Check(hs::runtime_detail::BuildVfxTypedFrameInputs(program, {}, std::span(&shell, 1), {}, 130)
              .persistent.empty(), "unidentified player shell accepted");
}

void TestPickupIdleOwners()
{
    const auto xp_id = Hash64("particle.pickup.xp.idle");
    const auto heal_id = Hash64("particle.pickup.heal.idle");
    const auto magnet_id = Hash64("particle.pickup.magnet.idle");
    const auto relic_id = Hash64("particle.pickup.relic_chest.idle");
    const auto entity_payload = Hash32("EntityAttachmentPayload");
    auto program = Program({
        {xp_id, 1, 1, entity_payload},
        {heal_id, 2, 1, entity_payload},
        {magnet_id, 3, 1, entity_payload},
        {relic_id, 4, 1, entity_payload},
    });
    for (auto &effect : program.effects) effect.timing_kind = 1;

    const auto make_owner = [](hs::PersistentVfxKind kind, std::uint64_t effect_id,
                               std::uint64_t stable_id) {
        hs::PersistentVfxVisual owner{};
        owner.kind = kind;
        owner.effect_asset = {effect_id};
        owner.stable_id = stable_id;
        owner.position = {2.0f, 0.8f, 3.0f};
        owner.yaw = 0.25f;
        owner.radius = 0.55f;
        owner.active_tick = 100;
        owner.expires = 0;
        owner.entity_health_fraction = 0.75f;
        owner.entity_state_flags = 3;
        owner.entity_render_id = 0x4000000000000000ull | stable_id;
        return owner;
    };
    auto owners = std::array{
        make_owner(hs::PersistentVfxKind::PickupXpIdle, xp_id, 201),
        make_owner(hs::PersistentVfxKind::PickupHealIdle, heal_id, 202),
        make_owner(hs::PersistentVfxKind::PickupMagnetIdle, magnet_id, 203),
        make_owner(hs::PersistentVfxKind::PickupRelicIdle, relic_id, 204),
    };
    const auto run = [&](std::span<const hs::PersistentVfxVisual> current,
                         const hs::VfxProgramData &cooked, hs::Tick tick) {
        return hs::runtime_detail::BuildVfxTypedFrameInputs(
            cooked, {}, current, {}, tick);
    };
    const auto valid = run(owners, program, 160);
    Check(valid.persistent.size() == 4 && valid.unsupported_effect_ids.empty(),
          "pickup idle entity owners were not adapted");
    for (std::size_t index = 0; index < valid.persistent.size(); ++index)
    {
        const auto &input = valid.persistent[index];
        const auto &entity = std::get<hs::VfxEntityPayload>(input.payload);
        Check(input.normalized_age == 0.0f && input.elapsed_seconds == 1.0f &&
                  input.source_duration_seconds == 0.0f &&
                  input.stable_id == owners[index].stable_id &&
                  input.current_transform[12] == owners[index].position.x &&
                  entity.footprint_radius == owners[index].radius &&
                  entity.health_fraction == owners[index].entity_health_fraction &&
                  entity.state_flags == owners[index].entity_state_flags &&
                  entity.render_instance_id == owners[index].entity_render_id,
              "pickup idle owner payload or spawned clock changed");
    }

    auto finite_expiry = owners;
    finite_expiry[0].expires = 101;
    const auto owner_presence = run(finite_expiry, program, 160);
    Check(owner_presence.persistent.size() == 4 && owner_presence.unsupported_effect_ids.empty(),
          "pickup idle owner was culled by predicted expiry");
    Check(run(owners, program, 99).persistent.empty(),
          "pickup idle owner was emitted before its spawned tick");
    Check(run(std::span<const hs::PersistentVfxVisual>{}, program, 160).persistent.empty(),
          "absent pickup idle owner persisted");

    const auto expect_rejected = [&](hs::PersistentVfxVisual malformed,
                                     const hs::VfxProgramData &cooked) {
        const auto result = run(std::array{malformed}, cooked, 160);
        Check(result.persistent.empty() && result.unsupported_effect_ids.size() == 1 &&
                  result.unsupported_effect_ids.front() == xp_id,
              "malformed pickup idle owner was accepted");
    };
    auto wrong_asset = owners[0];
    wrong_asset.effect_asset = {heal_id};
    expect_rejected(wrong_asset, program);
    auto missing_identity = owners[0];
    missing_identity.entity_render_id = 0;
    expect_rejected(missing_identity, program);
    auto invalid_position = owners[0];
    invalid_position.position.x = std::numeric_limits<float>::quiet_NaN();
    expect_rejected(invalid_position, program);
    auto invalid_radius = owners[0];
    invalid_radius.radius = 0.0f;
    expect_rejected(invalid_radius, program);
    auto invalid_health = owners[0];
    invalid_health.entity_health_fraction = 1.01f;
    expect_rejected(invalid_health, program);
    auto wrong_payload = program;
    wrong_payload.effects[0].payload_kind = Hash32("PointEventPayload");
    expect_rejected(owners[0], wrong_payload);
    auto wrong_mode = program;
    wrong_mode.effects[0].input_mode = 0;
    expect_rejected(owners[0], wrong_mode);
    auto wrong_timing = program;
    wrong_timing.effects[0].timing_kind = 0;
    expect_rejected(owners[0], wrong_timing);
}

void TestPlayerStampedEntityBindings()
{
    const auto bow_id = Hash64("particle.player.bow_draw");
    const auto charged_id = Hash64("particle.upgrade.charged.full_ready");
    const auto empowered_id = Hash64("particle.upgrade.empowered_ready");
    const auto entity_payload = Hash32("EntityAttachmentPayload");
    auto program = Program({
        {bow_id, 1, 1, entity_payload},
        {charged_id, 2, 1, entity_payload},
        {empowered_id, 3, 1, entity_payload},
    });
    program.effects[0].timing_kind = 1;
    program.effects[1].timing_kind = 0;
    program.effects[1].seconds = 0.55f;
    program.effects[2].timing_kind = 1;

    const auto make_owner = [](hs::PersistentVfxKind kind, std::uint64_t effect_id,
                               std::uint64_t stable_id, hs::Tick expires) {
        hs::PersistentVfxVisual owner{};
        owner.kind = kind;
        owner.effect_asset = {effect_id};
        owner.stable_id = stable_id;
        owner.position = {2.0f, 0.8f, 3.0f};
        owner.yaw = 0.25f;
        owner.radius = 0.9f;
        owner.active_tick = 100;
        owner.expires = expires;
        owner.entity_health_fraction = 0.65f;
        owner.entity_state_flags = 7;
        owner.entity_render_id = 0x7000000000000000ull | stable_id;
        return owner;
    };
    auto bow = make_owner(hs::PersistentVfxKind::PlayerBowDraw,
                          bow_id, 101, 0);
    bow.source_horizon_tick = 130;
    const auto charged = make_owner(hs::PersistentVfxKind::ChargedFullReady,
                                    charged_id, 102, 0);
    const auto empowered = make_owner(hs::PersistentVfxKind::EmpoweredReady,
                                      empowered_id, 103, 160);
    const auto current_with_horizon = std::array{bow, charged, empowered};
    const auto valid = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, current_with_horizon, {}, 110);
    Check(valid.persistent.size() == 3 && valid.unsupported_effect_ids.empty(),
          "player stamped entity bindings were not adapted");
    for (const auto &input : valid.persistent)
    {
        const auto &entity = std::get<hs::VfxEntityPayload>(input.payload);
        Check(entity.render_instance_id != 0 && entity.footprint_radius == 0.9f &&
                  entity.health_fraction == 0.65f && entity.state_flags == 7,
              "player stamped entity payload changed attachment metadata");
    }
    Check(std::abs(valid.persistent[0].normalized_age - 1.0f / 3.0f) < 0.00001f &&
              std::abs(valid.persistent[1].normalized_age - (10.0f / 60.0f) / 0.55f) < 0.00001f &&
              std::abs(valid.persistent[2].normalized_age - 1.0f / 6.0f) < 0.00001f,
          "player stamped entity age did not follow its timing contract");
    Check(std::abs(valid.persistent[0].source_duration_seconds - 0.5f) < 0.00001f &&
              std::abs(valid.persistent[1].source_duration_seconds - 0.55f) < 0.00001f &&
              std::abs(valid.persistent[2].source_duration_seconds - 1.0f) < 0.00001f,
          "player stamped source durations were not copied from their horizons");

    const auto held_bow = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, std::array{bow}, {}, 140);
    Check(held_bow.persistent.size() == 1 && held_bow.unsupported_effect_ids.empty() &&
              held_bow.persistent.front().normalized_age < 1.0f &&
              held_bow.persistent.front().normalized_age > 0.9998f &&
              std::abs(held_bow.persistent.front().source_duration_seconds - 0.5f) < 0.00001f,
          "open bow draw did not progress to readiness while its owner remained live");

    auto wrong_timing = program;
    wrong_timing.effects[0].timing_kind = 0;
    wrong_timing.effects[0].seconds = 0.5f;
    const auto wrong_timing_result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        wrong_timing, {}, std::array{bow}, {}, 110);
    Check(wrong_timing_result.persistent.empty() &&
              wrong_timing_result.unsupported_effect_ids.size() == 1 &&
              wrong_timing_result.unsupported_effect_ids.front() == bow_id,
          "bow draw accepted a fixed cooked timing");

    auto charged_gameplay = program;
    charged_gameplay.effects[1].timing_kind = 1;
    const auto charged_wrong_timing = hs::runtime_detail::BuildVfxTypedFrameInputs(
        charged_gameplay, {}, std::array{charged}, {}, 110);
    Check(charged_wrong_timing.persistent.empty() &&
              charged_wrong_timing.unsupported_effect_ids.size() == 1 &&
              charged_wrong_timing.unsupported_effect_ids.front() == charged_id,
          "charged full ready accepted gameplay timing");

    auto wrong_payload = program;
    wrong_payload.effects[2].payload_kind = Hash32("CircleAreaPayload");
    const auto wrong_payload_result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        wrong_payload, {}, std::array{empowered}, {}, 110);
    Check(wrong_payload_result.persistent.empty() &&
              wrong_payload_result.unsupported_effect_ids.size() == 1 &&
              wrong_payload_result.unsupported_effect_ids.front() == empowered_id,
          "empowered ready accepted a non-entity cooked payload");

    auto wrong_asset_owner = empowered;
    wrong_asset_owner.effect_asset = {bow_id};
    const auto wrong_asset = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, std::array{wrong_asset_owner}, {}, 110);
    Check(wrong_asset.persistent.empty() &&
              wrong_asset.unsupported_effect_ids.size() == 1 &&
              wrong_asset.unsupported_effect_ids.front() == empowered_id,
          "empowered ready accepted a mismatched source asset");

    auto missing_render_id = empowered;
    missing_render_id.entity_render_id = 0;
    const auto missing_render = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, std::array{missing_render_id}, {}, 110);
    Check(missing_render.persistent.empty() &&
              missing_render.unsupported_effect_ids.size() == 1 &&
              missing_render.unsupported_effect_ids.front() == empowered_id,
          "entity attachment accepted a missing render instance identity");

    auto open_empowered = empowered;
    open_empowered.expires = 0;
    const auto open_empowered_result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, std::array{open_empowered}, {}, 110);
    Check(open_empowered_result.persistent.empty() &&
              open_empowered_result.unsupported_effect_ids.size() == 1,
          "open ended lifetime escaped the bow draw sentinel contract");

    auto invalid_bow_horizon = bow;
    invalid_bow_horizon.source_horizon_tick = invalid_bow_horizon.active_tick;
    const auto invalid_bow_horizon_result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, std::array{invalid_bow_horizon}, {}, 110);
    Check(invalid_bow_horizon_result.persistent.empty() &&
              invalid_bow_horizon_result.unsupported_effect_ids.size() == 1 &&
              invalid_bow_horizon_result.unsupported_effect_ids.front() == bow_id,
          "bow draw accepted a nonpositive source horizon");

    auto invalid_empowered_horizon = empowered;
    invalid_empowered_horizon.expires = invalid_empowered_horizon.active_tick;
    const auto invalid_empowered_horizon_result = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, std::array{invalid_empowered_horizon}, {}, 110);
    Check(invalid_empowered_horizon_result.persistent.empty() &&
              invalid_empowered_horizon_result.unsupported_effect_ids.size() == 1 &&
              invalid_empowered_horizon_result.unsupported_effect_ids.front() == empowered_id,
          "empowered ready accepted a nonpositive source horizon");

    auto early_bow = bow;
    early_bow.active_tick = 111;
    Check(hs::runtime_detail::BuildVfxTypedFrameInputs(
              program, {}, std::array{early_bow}, {}, 110).persistent.empty(),
          "bow draw was emitted before its active tick");

    const auto fixed_active = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, std::array{charged}, {}, 132);
    Check(fixed_active.persistent.size() == 1 && fixed_active.unsupported_effect_ids.empty() &&
              std::abs(fixed_active.persistent.front().normalized_age -
                       ((32.0f / 60.0f) / 0.55f)) < 0.00001f,
          "charged full ready did not use cooked fixed duration");
    const auto fixed_expired = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, std::array{charged}, {}, 133);
    Check(fixed_expired.persistent.empty() &&
              fixed_expired.unsupported_effect_ids.size() == 1,
          "charged full ready survived its cooked 0.55 second lifetime");

    auto invalid_fixed_duration = program;
    invalid_fixed_duration.effects[1].seconds = 0.0f;
    const auto invalid_fixed = hs::runtime_detail::BuildVfxTypedFrameInputs(
        invalid_fixed_duration, {}, std::array{charged}, {}, 110);
    Check(invalid_fixed.persistent.empty() &&
              invalid_fixed.unsupported_effect_ids.size() == 1 &&
              invalid_fixed.unsupported_effect_ids.front() == charged_id,
          "charged full ready accepted a nonpositive cooked source duration");
}

void TestChargedOverchargeOwner()
{
    const auto effect_id = Hash64("particle.upgrade.charged.overcharge_loop");
    auto program = Program({
        {effect_id, 1, 1, Hash32("EntityAttachmentPayload")},
    });
    program.effects[0].timing_kind = 1;
    program.effects[0].binding_timing = Hash32("charge_ratio");
    program.upgrade_bindings = {
        {Hash64("skill.charged_shot"), 1, {0, 1}},
    };
    program.upgrade_sequences = {1};

    hs::PersistentVfxVisual owner{};
    owner.kind = hs::PersistentVfxKind::ChargedOverchargeLoop;
    owner.effect_asset = {effect_id};
    owner.stable_id = 0x4201;
    owner.position = {2.0f, 0.8f, 3.0f};
    owner.yaw = 0.25f;
    owner.radius = .82f;
    owner.active_tick = 100;
    owner.expires = 184;
    owner.skill = 3;
    owner.source_upgrade = 0;
    owner.charge_ratio = .75f;
    owner.entity_render_id = 0x1000000000000001ull;

    const auto valid = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, std::array{owner}, {}, 140);
    Check(valid.persistent.size() == 1 && valid.unsupported_effect_ids.empty(),
          "charged overcharge owner did not bind through its ordinal1 recipe");
    const auto &input = valid.persistent.front();
    const auto &entity = std::get<hs::VfxEntityPayload>(input.payload);
    Check(std::abs(input.elapsed_seconds - 40.0f / 60.0f) < 0.00001f &&
              std::abs(input.source_duration_seconds - 84.0f / 60.0f) < 0.00001f &&
              std::abs(input.normalized_age - .75f) < 0.00001f &&
              std::abs(entity.footprint_radius - .82f) < 0.00001f &&
              std::abs(entity.lifetime01 - .75f) < 0.00001f &&
              entity.render_instance_id == owner.entity_render_id,
          "charged overcharge owner lost episode clock, ratio, or footprint");

    auto missing_membership = program;
    missing_membership.upgrade_sequences.clear();
    const auto rejected_membership = hs::runtime_detail::BuildVfxTypedFrameInputs(
        missing_membership, {}, std::array{owner}, {}, 140);
    Check(rejected_membership.persistent.empty() &&
              rejected_membership.unsupported_effect_ids.size() == 1 &&
              rejected_membership.unsupported_effect_ids.front() == effect_id,
          "charged overcharge owner accepted missing ordinal1 membership");

    auto wrong_upgrade = owner;
    wrong_upgrade.source_upgrade = 1;
    const auto rejected_upgrade = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, std::array{wrong_upgrade}, {}, 140);
    Check(rejected_upgrade.persistent.empty() &&
              rejected_upgrade.unsupported_effect_ids.size() == 1,
          "charged overcharge owner accepted a different upgrade ordinal");

    Check(hs::runtime_detail::BuildVfxTypedFrameInputs(
              program, {}, std::array{owner}, {}, 184).persistent.empty(),
          "charged overcharge owner survived its gameplay overcharge end");
}

void TestProcStampedProjectileBindings()
{
    const auto retarget_id = Hash64("particle.upgrade.retarget");
    const auto bleed_extend_id = Hash64("particle.upgrade.ricochet.bleed_extend");
    auto program = Program({
        {retarget_id, 1, 1, Hash32("ProjectilePathPayload")},
        {bleed_extend_id, 2, 1, Hash32("EntityAttachmentPayload")},
    });
    program.effects[0].timing_kind = 0;
    program.effects[0].seconds = 0.30f;
    program.effects[1].timing_kind = 0;
    program.effects[1].seconds = 0.35f;
    program.upgrade_bindings = {
        {Hash64("skill.multishot"), 8, {0, 1}},
        {Hash64("skill.ricochet_arrow"), 3, {1, 1}},
    };
    program.upgrade_sequences = {1, 2};

    hs::PersistentVfxVisual retarget{};
    retarget.kind = hs::PersistentVfxKind::MultishotRetarget;
    retarget.effect_asset = {retarget_id};
    retarget.stable_id = 311;
    retarget.skill = 2;
    retarget.source_upgrade = 7;
    retarget.active_tick = retarget.projectile_spawned_tick = 100;
    retarget.expires = 118;
    retarget.projectile_owner_id = 51;
    retarget.position = retarget.projectile_current_position = {3.0f, 1.0f, 4.0f};
    retarget.projectile_previous_position = {2.0f, 1.0f, 4.0f};
    retarget.projectile_velocity = {20.0f, 0.0f, 0.0f};
    retarget.projectile_hitbox_radius = 0.12f;
    hs::PersistentVfxVisual ricochet = retarget;
    ricochet.kind = hs::PersistentVfxKind::RicochetBleedExtend;
    ricochet.effect_asset = {bleed_extend_id};
    ricochet.stable_id = 312;
    ricochet.skill = 5;
    ricochet.source_upgrade = 2;
    ricochet.active_tick = 105;
    ricochet.expires = 126;
    ricochet.radius = 0.12f;
    ricochet.entity_render_id = 0x5000000000000033ull;
    const auto owners = std::array{retarget, ricochet};
    const auto decoded = hs::runtime_detail::BuildVfxTypedFrameInputs(
        program, {}, owners, {}, 110);
    Check(decoded.persistent.size() == 2 && decoded.unsupported_effect_ids.empty(),
          "successful retarget and bleed extension owners did not bind");
    const auto &path = std::get<hs::VfxProjectilePathPayload>(decoded.persistent[0].payload);
    const auto &entity = std::get<hs::VfxEntityPayload>(decoded.persistent[1].payload);
    Check(path.current_position.x == 3.0f && path.previous_position.x == 2.0f &&
              path.projectile_radius == 0.12f &&
              std::abs(path.lifetime01 - 10.0f / 18.0f) < 0.00001f &&
              std::abs(decoded.persistent[0].elapsed_seconds - 10.0f / 60.0f) < 0.00001f &&
              std::abs(decoded.persistent[0].source_duration_seconds - 0.30f) < 0.00001f &&
              entity.render_instance_id == ricochet.entity_render_id &&
              entity.footprint_radius == ricochet.radius &&
              std::abs(decoded.persistent[1].normalized_age - 5.0f / 21.0f) < 0.00001f &&
              std::abs(decoded.persistent[1].source_duration_seconds - 0.35f) < 0.00001f,
          "fixed proc owner lost projectile path, entity attachment, or episode clock");
    const auto run = [&](const hs::PersistentVfxVisual &owner, hs::Tick tick,
                         const hs::VfxProgramData &content) {
        return hs::runtime_detail::BuildVfxTypedFrameInputs(
            content, {}, std::span(&owner, 1), {}, tick);
    };
    Check(run(retarget, 99, program).persistent.empty() &&
              run(retarget, 118, program).persistent.empty() &&
              run(ricochet, 104, program).persistent.empty() &&
              run(ricochet, 126, program).persistent.empty(),
          "proc stamp escaped its actual fixed owner window");
    auto wrong_skill = retarget;
    wrong_skill.skill = 5;
    Check(run(wrong_skill, 110, program).persistent.empty(),
          "retarget accepted unrelated skill attribution");
    auto wrong_upgrade = ricochet;
    wrong_upgrade.source_upgrade = 7;
    Check(run(wrong_upgrade, 110, program).persistent.empty(),
          "bleed extension accepted unrelated upgrade attribution");
    auto missing_membership = program;
    missing_membership.upgrade_sequences[0] = 2;
    Check(run(retarget, 110, missing_membership).persistent.empty(),
          "retarget accepted missing cooked upgrade membership");
    auto wrong_asset = retarget;
    wrong_asset.effect_asset = {bleed_extend_id};
    Check(run(wrong_asset, 110, program).persistent.empty(),
          "retarget accepted a mismatched authored effect");
    auto missing_render = ricochet;
    missing_render.entity_render_id = 0;
    Check(run(missing_render, 110, program).persistent.empty(),
          "bleed extension accepted missing projectile render identity");
}

}

int main()
{
    try
    {
        TestEnemyStatusOwners();
        TestBossStateOwners();
        TestPlayerStateOwners();
        TestPickupIdleOwners();
        TestPlayerStampedEntityBindings();
        TestChargedOverchargeOwner();
        TestProcStampedProjectileBindings();
        TestStatusTickIdentity();
        TestOwnedVisualLinks();
        TestPointEvent();
        TestProjectileImpactEventUsesAuthoritativeGeometry();
        TestEventSchemaAndModeRejection();
        TestEventGeometryTagsAndGapExpansion();
        TestArrowRainPullLinkGeometry();
        TestPickupCollectLinkPayload();
        TestContextAllowsZeroRatio();
        TestChargedPulseContextMetadata();
        TestCircleLifetimeAndCompoundIdentity();
        TestUpgradeSlowAreaAliases();
        TestUpgradeSlowAreaAliasRequiresCookedMembership();
        TestDamageTrailUpgradeSupplements();
        TestShockwaveWavefrontBinding();
        TestShockwaveWarningOwner();
        TestSafeSectorFanout();
        TestBossAreaUsesAuthoredCircleBinding();
        TestProjectileIngressUsesAuthoritativeOwnerState();
        TestProjectilePathUsesAuthoritativeMotion();
        TestHostileProjectileBindings();
        TestChargeGuideUsesPresenceInsteadOfExpiry();
        TestWarningOwnerRefresh();
        TestRangedWarningIdentity();
        std::cout << "Typed VFX frame adapter tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
