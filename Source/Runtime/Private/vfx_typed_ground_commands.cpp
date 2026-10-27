#include "vfx_typed_ground_commands.hpp"

#include <hs/render_data/render_snapshot.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <optional>
#include <string_view>

namespace hs::runtime_detail
{
namespace
{
constexpr std::uint32_t Hash32(std::string_view value) noexcept
{
    std::uint32_t hash = 2166136261u;
    for (const unsigned char character : value)
    {
        hash ^= character;
        hash *= 16777619u;
    }
    return hash;
}
constexpr std::uint64_t Hash64(std::string_view value) noexcept
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char character : value)
        hash = (hash ^ character) * 1099511628211ull;
    return hash;
}

constexpr auto kRingGapsPayload = Hash32("RingWithGapsPayload");
constexpr auto kCirclePayload = Hash32("CircleAreaPayload");
constexpr auto kPointPayload = Hash32("PointEventPayload");
constexpr auto kContextPayload = Hash32("PresentationContextPayload");
constexpr auto kProjectilePayload = Hash32("ProjectilePayload");
constexpr auto kEntityPayload = Hash32("EntityAttachmentPayload");
constexpr auto kPlayerInvulnerable = Hash64("particle.player.invulnerable_loop");
constexpr auto kBossPhase2Aura = Hash64("persistent.boss.phase2_aura");
constexpr auto kBossPhaseTransition = Hash64("persistent.boss.phase_transition_invulnerable");
constexpr auto kStatusSlow = Hash64("persistent.status.slow");
constexpr auto kStatusMark = Hash64("persistent.status.mark");
constexpr auto kChargedOverchargeLoop = Hash64("particle.upgrade.charged.overcharge_loop");
constexpr auto kPickupXpIdle = Hash64("particle.pickup.xp.idle");
constexpr auto kPickupHealIdle = Hash64("particle.pickup.heal.idle");
constexpr auto kPickupMagnetIdle = Hash64("particle.pickup.magnet.idle");
constexpr auto kPickupRelicIdle = Hash64("particle.pickup.relic_chest.idle");
constexpr auto kLinePayload = Hash32("LineAreaPayload");
constexpr auto kConePayload = Hash32("ConePayload");

bool Finite(Float3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

template<class T>
bool ValidRange(VfxRange range, std::span<const T> values) noexcept
{
    return range.first <= values.size() && range.count <= values.size() - range.first;
}

bool SourceEnvelope(const VfxSourceRecord &source, float age, float &envelope) noexcept
{
    if (source.knot_count != 2 && source.knot_count != 4) return false;
    for (std::size_t index = 0; index < source.knot_count; ++index)
        if (!std::isfinite(source.knots[index]) || source.knots[index] < 0.0f ||
            source.knots[index] > 1.0f || (index > 0 && source.knots[index] < source.knots[index - 1]))
            return false;
    if (age < source.knots[0] || age >= source.knots[source.knot_count - 1]) return false;
    envelope = 1.0f;
    if (source.knot_count == 4)
    {
        if (age < source.knots[1] && source.knots[1] > source.knots[0])
            envelope = (age - source.knots[0]) / (source.knots[1] - source.knots[0]);
        else if (age > source.knots[2] && source.knots[3] > source.knots[2])
            envelope = (source.knots[3] - age) / (source.knots[3] - source.knots[2]);
    }
    return std::isfinite(envelope) && envelope > 0.0f;
}

std::optional<float> IntegratedPulse(const VfxProgramView &p, VfxRange curve_keys, float age, float duration)
{
    if (curve_keys.count < 2 || curve_keys.first > p.curve_keys.size() ||
        curve_keys.count > p.curve_keys.size()-curve_keys.first) return {};
    const auto keys=std::span(p.curve_keys).subspan(curve_keys.first,curve_keys.count);
    float integral=0;
    for (std::size_t i=0;i<keys.size();++i)
    {
        const auto &key=keys[i];
        if (!i) continue;
        const auto &previous=keys[i-1];
        const float end=std::min(age,key.time);
        if (end>previous.time)
        {
            const float elapsed=end-previous.time;
            const float value=previous.value+(key.value-previous.value)*(elapsed/(key.time-previous.time));
            integral += elapsed*(previous.value+value)*.5f;
        }
    }
    const float phase=duration*integral;
    return std::isfinite(phase)?std::optional(phase):std::nullopt;
}

struct GroundOwner
{
    std::uint64_t stable_id{};
    VfxEffectHandle effect_handle{};
    std::uint8_t source_visual_kind{0xff};
    VfxCirclePayload circle{};
    VfxLinePayload line{};
    VfxConePayload cone{};
    VfxRingGapsPayload ring_gaps{};
    bool is_ring_gaps{false};
    bool is_ring_preview{false};
    bool is_circle_preview{false};
    bool gameplay_warning{false};
    bool is_line{false};
    bool is_cone{false};
    float line_half_width{};
    float line_half_length{};
    float cone_range{};
    float cone_half_angle_radians{};
    float normalized_age{};
    float lifetime01{};
    float alpha{1.0f};
    VfxQuality quality{VfxQuality::High};
    std::uint32_t stable_seed{};
    Tick command_tick{};
    float command_lifetime{};
};

void AppendSourceOutputs(const VfxImage &image, std::size_t source_index,
                         const GroundOwner &owner,
                         std::vector<VfxTypedGroundCommand> &result)
{
    const auto cooked = image.program;
    if (source_index >= cooked.sources.size()) return;
    const auto &source = cooked.sources[source_index];
    if (source.type != VfxSourceType::Direct) return;
    if (!owner.effect_handle || owner.effect_handle > image.ground_effect_ranges.size()) return;
    const auto recipe_range = image.ground_effect_ranges[owner.effect_handle - 1];
    if (!ValidRange(recipe_range, image.ground_recipes)) return;
    const auto recipes = image.ground_recipes.subspan(recipe_range.first, recipe_range.count);
    auto next = std::ranges::lower_bound(recipes, source_index, {}, &VfxGroundRecipe::source_index);
    for (; next != recipes.end() && next->source_index == source_index; ++next)
    {
        const auto &recipe = *next;
        const bool ring = recipe.kind==VfxGroundRecipeKind::Ring && !owner.is_ring_preview && !owner.is_line && !owner.is_cone;
        const bool fill = recipe.kind==VfxGroundRecipeKind::Fill && !owner.is_ring_preview && !owner.is_line && !owner.is_cone;
        const bool border = (owner.is_line || owner.is_cone) &&
            recipe.kind == VfxGroundRecipeKind::Border;
        const bool hatch = (owner.is_line || owner.is_cone) &&
            recipe.kind == VfxGroundRecipeKind::Hatch;
        const bool preview = (owner.is_ring_preview || owner.is_circle_preview) && recipe.kind==VfxGroundRecipeKind::Preview;
        const bool ticks = (owner.is_ring_preview || owner.is_circle_preview) && recipe.kind==VfxGroundRecipeKind::Ticks;
        if ((!ring && !fill && !border && !hatch && !preview && !ticks) ||
            recipe.min_quality > static_cast<std::uint32_t>(owner.quality))
            continue;
        if(owner.gameplay_warning && !preview && !ticks)continue;
        if(owner.is_circle_preview && (preview || ticks) &&
            !recipe.source_parameters_empty)continue;
        const auto phase = preview ? IntegratedPulse(cooked,recipe.pulse_curve_keys,owner.normalized_age,owner.command_lifetime) : std::optional<float>{0};
        if (!phase) continue;
        const float primary = recipe.primary;
        const float secondary = recipe.secondary;
        const auto output_index=recipe.output_index;
        const float width = ring || border || preview ? primary : 0.0f;
        const float size = owner.is_line
            ? std::sqrt(owner.line_half_width * owner.line_half_width +
                        owner.line_half_length * owner.line_half_length)
            : (owner.is_cone ? owner.cone_range + width : (owner.is_ring_gaps ? owner.ring_gaps.outer_radius : owner.circle.radius) + width);
        if (!std::isfinite(size)) continue;
        VfxTypedGroundCommand item;
        item.stable_id = owner.stable_id;
        item.effect_handle = owner.effect_handle;
        item.source_visual_kind = owner.source_visual_kind;
        item.edge_width_world = width;
        item.normalized_age = owner.normalized_age;
        item.lifetime01 = owner.lifetime01;
        item.motion = {recipe.motion_rate_hz, recipe.motion_amplitude,
                       recipe.motion_inset_fraction};
        item.gradient_row = recipe.gradient_row;
        item.hdr = recipe.hdr;
        auto &command = item.command;
        command.sequence = owner.stable_id ^ owner.command_tick ^
            (static_cast<std::uint64_t>(owner.effect_handle) << 32) ^
            static_cast<std::uint64_t>(output_index);
        command.tick = owner.command_tick;
        command.position = owner.is_cone ? owner.cone.origin : (owner.is_ring_gaps ? owner.ring_gaps.center : owner.circle.center);
        if (owner.is_line)
            command.position = {(owner.line.start.x + owner.line.end.x) * 0.5f,
                                (owner.line.start.y + owner.line.end.y) * 0.5f,
                                (owner.line.start.z + owner.line.end.z) * 0.5f};
        command.shape = ParticleShape::Point;
        command.velocity_mode = ParticleVelocity::Direction;
        command.facing = ParticleFacing::Ground;
        command.renderer = VfxRenderer::Ground;
        command.primitive = (ring || border || preview) ? VfxPrimitive::ExactRing : VfxPrimitive::LowFrequencyFill;
        command.direction = owner.is_line ? owner.line.direction :
            (owner.is_cone ? owner.cone.direction : Float3{0.0f, 1.0f, 0.0f});
        command.lifetime_min = command.lifetime_max = owner.command_lifetime;
        command.start_size_min = command.start_size_max = size;
        command.end_size_min = command.end_size_max = size;
        command.stretch = ring ? (owner.is_ring_gaps ? owner.ring_gaps.outer_radius : owner.circle.radius) / size :
            (border ? 0.8f : primary);
        item.geometry.shape = owner.is_line
            ? (border ? VfxGroundShape::LineBorder : VfxGroundShape::LineHatch)
            : (owner.is_cone ? (border ? VfxGroundShape::ConeBorder : VfxGroundShape::ConeHatch)
                             : VfxGroundShape::Circle);
        if (owner.is_ring_gaps)
        {
            item.geometry.shape = owner.is_ring_preview
                ? (preview ? VfxGroundShape::RingGapsPreviewBorder : VfxGroundShape::RingGapsTicks)
                : (ring ? VfxGroundShape::RingGapsBorder : VfxGroundShape::RingGapsFill);
            item.geometry.progress = owner.normalized_age;
            item.geometry.animation_phase = *phase;
            item.geometry.tick_count = recipe.tick_count;
            item.geometry.inner_radius = owner.ring_gaps.inner_radius;
            item.geometry.outer_radius = owner.ring_gaps.outer_radius;
            item.geometry.edge_width = width;
            item.geometry.gap_half_angle_radians = owner.ring_gaps.gap_half_width_degrees * (3.14159265358979323846f / 180.0f);
            for (float degrees : owner.ring_gaps.gap_angles_degrees)
            {
                float wrapped = std::fmod(degrees, 360.0f);
                if (wrapped < 0) wrapped += 360.0f;
                item.geometry.gap_angles_radians.push_back(wrapped * (3.14159265358979323846f / 180.0f));
            }
        }
        if(owner.is_circle_preview && (preview || ticks))
        {
            item.geometry.shape=preview?VfxGroundShape::CirclePreviewBorder:VfxGroundShape::CirclePreviewTicks;
            item.geometry.progress=owner.normalized_age;
            item.geometry.animation_phase=*phase;
            item.geometry.tick_count=recipe.tick_count;
            item.geometry.inner_radius=0;
            item.geometry.outer_radius=owner.circle.radius;
            item.geometry.edge_width=width;
        }
        if (owner.is_line)
        {
            item.geometry.direction = owner.line.direction;
            item.geometry.half_width = owner.line_half_width;
            item.geometry.half_length = owner.line_half_length;
            item.geometry.edge_width = border ? primary : 0.0f;
            item.geometry.spacing = border ? secondary : primary;
            item.geometry.scroll = hatch ? secondary : 0.8f;
        }
        if (owner.is_cone)
        {
            item.geometry.direction = owner.cone.direction;
            item.geometry.range = owner.cone_range;
            item.geometry.half_angle_radians = owner.cone_half_angle_radians;
            item.geometry.edge_width = border ? primary : 0.0f;
            item.geometry.spacing = border ? secondary : primary;
            item.geometry.scroll = hatch ? secondary : 0.8f;
        }
        command.start_color = command.end_color = {
            recipe.rgb.x * recipe.hdr, recipe.rgb.y * recipe.hdr,
            recipe.rgb.z * recipe.hdr, recipe.alpha * owner.alpha};
        if ((owner.is_ring_gaps || owner.is_circle_preview) && (!Finite({command.start_color.x, command.start_color.y, command.start_color.z}) ||
            !std::isfinite(command.start_color.w))) continue;
        command.seed = owner.stable_seed;
        result.push_back(item);
    }
}
void AppendWaveGroundOutputs(const VfxImage &image, const VfxEffectRecord &effect,
                            const VfxEventInput &input, float age,
                            std::vector<VfxTypedGroundCommand> &result)
{
    const auto cooked=image.program;
    const auto *context = std::get_if<VfxContextPayload>(&input.payload);
    if (!context || effect.handle != input.effect_handle || effect.input_mode != 0 || effect.timing_kind != 0 ||
        effect.payload_kind != kContextPayload || !std::isfinite(effect.seconds) || effect.seconds <= 0 ||
        !std::isfinite(context->scalar0) || context->scalar0 <= 0 ||
        !std::isfinite(age) || age < 0 || age >= 1 ||
        effect.sources.count != 2 || effect.sources.first > cooked.sources.size() ||
        effect.sources.count > cooked.sources.size() - effect.sources.first)
        return;
    if(!input.effect_handle || input.effect_handle>image.ground_effect_ranges.size()) return;
    const auto range=image.ground_effect_ranges[input.effect_handle-1];
    if(!ValidRange(range,image.ground_recipes) || !range.count) return;
    const auto recipes=image.ground_recipes.subspan(range.first,range.count);
    std::uint64_t id{};
    for(const auto &lookup:cooked.effect_lookup)
        if(lookup.handle==effect.handle)
        {
            if(id) return;
            id=lookup.effect_id;
        }
    if(id!=recipes.front().effect_id) return;
    const bool warning = id == Hash64("particle.session.wave_warning");
    if (!warning && id != Hash64("particle.session.wave_start")) return;
    const Float3 center{input.world_transform[12], input.world_transform[13],
                        input.world_transform[14]};
    if (!Finite(center)) return;
    for (std::size_t index = effect.sources.first;
         index < effect.sources.first + effect.sources.count; ++index)
    {
        const auto &source = cooked.sources[index];
        auto source_recipe=std::ranges::lower_bound(recipes,index,{},&VfxGroundRecipe::source_index);
        if(source_recipe==recipes.end() || source_recipe->source_index!=index ||
           source_recipe->source_output_count!=1 || !source_recipe->source_parameters_empty) continue;
        if (source.effect != effect.handle || source.type != VfxSourceType::Direct ||
            source.stable_id != Hash32(warning
                ? (index == effect.sources.first ? "danger_boundary" : "radial_ticks")
                : (index == effect.sources.first ? "gameplay_boundary" : "interior_secondary")))
            continue;
        float envelope{};
        if (!SourceEnvelope(source, age, envelope)) continue;
        GroundOwner owner{};
        owner.stable_id = context->owner_id ? context->owner_id : input.sequence;
        owner.effect_handle = input.effect_handle;
        owner.circle.center = center;
        // Arena presentation context is a cosmetic arena-scale indicator;
        // it never represents an attack or changes an authoritative hitbox.
        owner.circle.radius = context->scalar0;
        owner.circle.lifetime01 = age;
        owner.is_circle_preview = warning;
        owner.normalized_age = owner.lifetime01 = age;
        owner.alpha = envelope;
        owner.quality = input.quality;
        owner.stable_seed = input.stable_seed;
        owner.command_tick = input.event_tick;
        owner.command_lifetime = effect.seconds;
        AppendSourceOutputs(image, index, owner, result);
    }
}

void AppendExpandingRingOutputs(const VfxImage &image, std::size_t source_index,
                                const VfxEventInput &input, float age, float envelope,
                                float duration, std::vector<VfxTypedGroundCommand> &result)
{
    if (!input.effect_handle || input.effect_handle > image.ground_effect_ranges.size()) return;
    const auto range=image.ground_effect_ranges[input.effect_handle-1];
    if(!ValidRange(range,image.ground_recipes)) return;
    const auto recipes=image.ground_recipes.subspan(range.first,range.count);
    auto next=std::ranges::lower_bound(recipes,source_index,{},&VfxGroundRecipe::source_index);
    const auto *point = std::get_if<VfxPointPayload>(&input.payload);
    const auto *circle = std::get_if<VfxCirclePayload>(&input.payload);
    if (!point && !circle) return;
    const Float3 center = point ? point->position : circle->center;
    const float scale = point ? point->authored_scale : circle->radius;
    if (!Finite(center) || !std::isfinite(scale) || scale <= 0.0f) return;
    for (;next!=recipes.end() && next->source_index==source_index;++next)
    {
        const auto &recipe=*next;
        if(recipe.kind!=VfxGroundRecipeKind::ExpandingRing || recipe.min_quality>static_cast<std::uint32_t>(input.quality)) continue;
        const float progress = (age - recipe.knots[0]) / (recipe.knots[1] - recipe.knots[0]);
        if (!std::isfinite(progress) || progress < 0.0f || progress >= 1.0f) continue;
        const float radius = std::lerp(recipe.primary, recipe.secondary, progress) * scale;
        const float edge_width=recipe.tertiary;
        const float size = radius + edge_width;
        if (!std::isfinite(radius) || !std::isfinite(size) || radius <= 0.0f) continue;
        VfxTypedGroundCommand item;
        item.additive = true;
        item.stable_id = input.geometry_owner_id != 0 ? input.geometry_owner_id : static_cast<std::uint64_t>(input.sequence);
        item.effect_handle = input.effect_handle;
        item.edge_width_world = edge_width;
        item.normalized_age = item.lifetime01 = age;
        item.motion = {recipe.motion_rate_hz, recipe.motion_amplitude, recipe.motion_inset_fraction};
        item.gradient_row = recipe.gradient_row;
        item.hdr = recipe.hdr;
        item.geometry.shape = VfxGroundShape::Circle;
        item.geometry.outer_radius = radius;
        item.geometry.edge_width = edge_width;
        item.geometry.progress = progress;
        auto &command = item.command;
        command.sequence = item.stable_id ^ input.event_tick ^
            (static_cast<std::uint64_t>(input.effect_handle) << 32) ^ recipe.output_index;
        command.tick = input.event_tick;
        command.position = center;
        command.shape = ParticleShape::Point;
        command.velocity_mode = ParticleVelocity::Direction;
        command.facing = ParticleFacing::Ground;
        command.renderer = VfxRenderer::Ground;
        command.primitive = VfxPrimitive::ExactRing;
        command.lifetime_min = command.lifetime_max = duration;
        command.start_size_min = command.start_size_max = size;
        command.end_size_min = command.end_size_max = size;
        command.stretch = radius / size;
        command.start_color = command.end_color = {
            recipe.rgb.x * recipe.hdr, recipe.rgb.y * recipe.hdr,
            recipe.rgb.z * recipe.hdr, recipe.alpha * envelope};
        if (!Finite({command.start_color.x, command.start_color.y, command.start_color.z}) ||
            !std::isfinite(command.start_color.w)) continue;
        command.seed = input.stable_seed;
        result.push_back(item);
    }
}
bool HexMask(const VfxImage &image,const VfxGroundRecipe &recipe,
             const VfxEventInput &input,std::uint32_t &slice,float &strength)
{
    const auto quality=static_cast<std::uint32_t>(input.quality);
    if(quality>=recipe.mask_variants.size()) return false;
    const auto &variant=recipe.mask_variants[quality];
    if(!variant.valid) return false;
    slice = 0xffffffffu;
    strength = variant.present?variant.strength:0.0f;
    if(variant.present)
    {
        const auto &cooked=image.program;
        if(!ValidRange(variant.slices,cooked.slice_indices) || !variant.slices.count) return false;
        slice=cooked.slice_indices[variant.slices.first+input.stable_seed%variant.slices.count];
    }
    return true;
}

void AppendHexConstellationOutputs(const VfxImage &image, std::size_t source_index,
                                   const VfxEventInput &input, float age, float envelope,
                                   float duration, std::vector<VfxTypedGroundCommand> &result)
{
    if (!input.effect_handle || input.effect_handle>image.ground_effect_ranges.size()) return;
    const auto range=image.ground_effect_ranges[input.effect_handle-1];
    if(!ValidRange(range,image.ground_recipes)) return;
    const auto recipes=image.ground_recipes.subspan(range.first,range.count);
    auto next=std::ranges::lower_bound(recipes,source_index,{},&VfxGroundRecipe::source_index);
    const auto *point = std::get_if<VfxPointPayload>(&input.payload);
    const auto *context = std::get_if<VfxContextPayload>(&input.payload);
    if (!point && !context) return;
    const Float3 anchor = point ? point->position :
        Float3{input.world_transform[12], input.world_transform[13], input.world_transform[14]};
    const float scale = point ? point->authored_scale : 1.0f;
    if (!Finite(anchor) || !std::isfinite(scale) || scale <= 0.0f) return;
    constexpr float kBaseRadius = 0.65f;
    constexpr float kWorldEdgeWidth = 0.05f;
    const float radius = kBaseRadius * scale;
    const float size = radius + kWorldEdgeWidth;
    if (!std::isfinite(radius) || !std::isfinite(size) || radius <= 0.0f) return;
    for (;next!=recipes.end() && next->source_index==source_index;++next)
    {
        const auto &recipe=*next;
        if(recipe.kind!=VfxGroundRecipeKind::HexConstellation || recipe.min_quality>static_cast<std::uint32_t>(input.quality)) continue;
        const float progress=(age-recipe.knots[0])/(recipe.knots[1]-recipe.knots[0]);
        if(!std::isfinite(progress)||progress<0||progress>=1) continue;
        std::uint32_t mask_slice{};
        float mask_strength{};
        if (!HexMask(image,recipe,input,mask_slice,mask_strength)) continue;
        VfxTypedGroundCommand item;
        item.additive = true;
        item.stable_id = static_cast<std::uint64_t>(input.sequence);
        item.effect_handle = input.effect_handle;
        item.edge_width_world = kWorldEdgeWidth;
        item.normalized_age = item.lifetime01 = age;
        item.motion = {recipe.motion_rate_hz, recipe.motion_amplitude, recipe.motion_inset_fraction};
        item.gradient_row = recipe.gradient_row;
        item.hdr = recipe.hdr;
        item.geometry.shape = VfxGroundShape::HexConstellation;
        item.geometry.spokes = recipe.tick_count;
        item.geometry.progress = progress;
        item.geometry.outer_radius = radius;
        item.geometry.edge_width = kWorldEdgeWidth;
        item.geometry.animation_phase = static_cast<float>(recipe.pulse_curve_interpolation);
        item.geometry.mask_slice = mask_slice;
        item.geometry.mask_strength = mask_strength;
        auto &command = item.command;
        command.sequence = item.stable_id ^ input.event_tick ^
            (static_cast<std::uint64_t>(input.effect_handle) << 32) ^ recipe.output_index;
        command.tick = input.event_tick;
        command.position = {anchor.x, 0.025f, anchor.z};
        command.shape = ParticleShape::Point;
        command.velocity_mode = ParticleVelocity::Direction;
        command.facing = ParticleFacing::Ground;
        command.renderer = VfxRenderer::Ground;
        command.primitive = VfxPrimitive::ExactRing;
        command.lifetime_min = command.lifetime_max = duration;
        command.start_size_min = command.start_size_max = size;
        command.end_size_min = command.end_size_max = size;
        command.stretch = radius / size;
        command.start_color = command.end_color = {
            recipe.rgb.x * recipe.hdr, recipe.rgb.y * recipe.hdr,
            recipe.rgb.z * recipe.hdr, recipe.alpha * envelope};
        if (!Finite({command.start_color.x, command.start_color.y, command.start_color.z}) ||
            !std::isfinite(command.start_color.w)) continue;
        command.seed = input.stable_seed;
        result.push_back(item);
    }
}
void AppendBossSignatureOutputs(const VfxImage &image, std::size_t source_index,
                                const VfxEventInput &input, float age, float envelope,
                                float duration, std::vector<VfxTypedGroundCommand> &result)
{
    if (!input.effect_handle || input.effect_handle>image.ground_effect_ranges.size()) return;
    const auto range=image.ground_effect_ranges[input.effect_handle-1];
    if(!ValidRange(range,image.ground_recipes)) return;
    const auto recipes=image.ground_recipes.subspan(range.first,range.count);
    auto next=std::ranges::lower_bound(recipes,source_index,{},&VfxGroundRecipe::source_index);
    const auto *point = std::get_if<VfxPointPayload>(&input.payload);
    const auto *circle = std::get_if<VfxCirclePayload>(&input.payload);
    const auto *ring = std::get_if<VfxRingGapsPayload>(&input.payload);
    const auto *cone = std::get_if<VfxConePayload>(&input.payload);
    if (!point && !circle && !ring && !cone) return;
    const Float3 anchor = point ? point->position :
        (circle ? circle->center : (ring ? ring->center : cone->origin));
    const float radius = point ? point->authored_scale :
        (circle ? circle->radius : (ring ? ring->outer_radius : cone->range));
    constexpr float kWorldEdgeWidth = 0.08f;
    const float size = radius + kWorldEdgeWidth;
    if (!Finite(anchor) || !std::isfinite(radius) || radius <= 0.0f || !std::isfinite(size)) return;
    const float cone_length = cone ? std::hypot(cone->direction.x, cone->direction.z) : 1.0f;
    if (cone && (!std::isfinite(cone_length) || cone_length <= 0.0001f)) return;
    for (;next!=recipes.end() && next->source_index==source_index;++next)
    {
        const auto &recipe=*next;
        if(recipe.kind!=VfxGroundRecipeKind::BossSignature || recipe.min_quality>static_cast<std::uint32_t>(input.quality)) continue;
        const float progress=(age-recipe.knots[0])/(recipe.knots[1]-recipe.knots[0]);
        if(!std::isfinite(progress)||progress<0||progress>=1) continue;
        VfxTypedGroundCommand item;
        item.additive = true;
        item.stable_id = input.geometry_owner_id != 0 ? input.geometry_owner_id : static_cast<std::uint64_t>(input.sequence);
        item.effect_handle = input.effect_handle;
        item.edge_width_world = kWorldEdgeWidth;
        item.normalized_age = item.lifetime01 = age;
        item.motion = {recipe.motion_rate_hz, recipe.motion_amplitude, recipe.motion_inset_fraction};
        item.gradient_row = recipe.gradient_row;
        item.hdr = recipe.hdr;
        auto &geometry = item.geometry;
        geometry.shape = cone ? VfxGroundShape::ConeBorder :
            (ring ? VfxGroundShape::RingGapsBorder : VfxGroundShape::Circle);
        geometry.progress = progress;
        geometry.animation_phase = 1.0f;
        geometry.edge_width = kWorldEdgeWidth;
        geometry.outer_radius = radius;
        if (ring)
        {
            geometry.inner_radius = ring->inner_radius;
            geometry.gap_half_angle_radians = ring->gap_half_width_degrees *
                (3.14159265358979323846f / 180.0f);
            for (float degrees : ring->gap_angles_degrees)
            {
                float wrapped = std::fmod(degrees, 360.0f);
                if (wrapped < 0.0f) wrapped += 360.0f;
                geometry.gap_angles_radians.push_back(wrapped * (3.14159265358979323846f / 180.0f));
            }
        }
        if (cone)
        {
            geometry.direction = {cone->direction.x / cone_length, 0.0f,
                                  cone->direction.z / cone_length};
            geometry.range = cone->range;
            geometry.half_angle_radians = cone->half_angle_degrees *
                (3.14159265358979323846f / 180.0f);
        }
        auto &command = item.command;
        command.sequence = item.stable_id ^ input.event_tick ^
            (static_cast<std::uint64_t>(input.effect_handle) << 32) ^ recipe.output_index;
        command.tick = input.event_tick;
        command.position = {anchor.x, 0.025f, anchor.z};
        command.direction = cone ? geometry.direction : Float3{0.0f, 1.0f, 0.0f};
        command.shape = ParticleShape::Point;
        command.velocity_mode = ParticleVelocity::Direction;
        command.facing = ParticleFacing::Ground;
        command.renderer = VfxRenderer::Ground;
        command.primitive = VfxPrimitive::ExactRing;
        command.lifetime_min = command.lifetime_max = duration;
        command.start_size_min = command.start_size_max = size;
        command.end_size_min = command.end_size_max = size;
        command.stretch = radius / size;
        command.start_color = command.end_color = {
            recipe.rgb.x * recipe.hdr, recipe.rgb.y * recipe.hdr,
            recipe.rgb.z * recipe.hdr, recipe.alpha * envelope};
        if (!Finite({command.start_color.x, command.start_color.y, command.start_color.z}) ||
            !std::isfinite(command.start_color.w)) continue;
        command.seed = input.stable_seed;
        result.push_back(item);
    }
}
void AppendRemainingEventGroundOutputs(const VfxImage &image, std::size_t source_index,
                                       const VfxEventInput &input, float age, float envelope,
                                       float duration, std::vector<VfxTypedGroundCommand> &result)
{
    if (!input.effect_handle || input.effect_handle>image.ground_effect_ranges.size()) return;
    const auto range=image.ground_effect_ranges[input.effect_handle-1];
    if(!ValidRange(range,image.ground_recipes)) return;
    const auto recipes=image.ground_recipes.subspan(range.first,range.count);
    auto next=std::ranges::lower_bound(recipes,source_index,{},&VfxGroundRecipe::source_index);
    const auto *point = std::get_if<VfxPointPayload>(&input.payload);
    const auto *circle = std::get_if<VfxCirclePayload>(&input.payload);
    const auto *projectile = std::get_if<VfxProjectilePayload>(&input.payload);
    if (!point && !circle && !projectile) return;
    const Float3 anchor = point ? point->position : (circle ? circle->center :
        Float3{input.world_transform[12], input.world_transform[13], input.world_transform[14]});
    if (!Finite(anchor)) return;
    for (;next!=recipes.end() && next->source_index==source_index;++next)
    {
        const auto &recipe=*next;
        const bool heal=point && recipe.kind==VfxGroundRecipeKind::CrossRing;
        const bool trap=circle && recipe.kind==VfxGroundRecipeKind::BrokenHex;
        const bool axial=projectile && recipe.kind==VfxGroundRecipeKind::AxialFracture;
        if((!heal&&!trap&&!axial)||recipe.min_quality>static_cast<std::uint32_t>(input.quality)) continue;
        const float progress=(age-recipe.knots[0])/(recipe.knots[1]-recipe.knots[0]);
        if(!std::isfinite(progress)||progress<0||progress>=1) continue;
        float radius{};
        float edge{};
        float length{};
        float break_half_angle{};
        Float3 direction{0.0f, 0.0f, 1.0f};
        if (heal)
        {
            radius = 0.65f * point->authored_scale;
            edge = 0.05f;
        }
        else if (trap)
        {
            edge = recipe.primary;
            const float sector_degrees = recipe.secondary;
            if (!std::isfinite(sector_degrees) || sector_degrees <= 0.0f || sector_degrees >= 360.0f)
                continue;
            break_half_angle = sector_degrees * (3.14159265358979323846f / 360.0f);
            radius = circle->radius;
        }
        else
        {
            if (!Finite(projectile->velocity) ||
                !std::isfinite(projectile->hitbox_radius) || projectile->hitbox_radius <= 0.0f)
                continue;
            length = recipe.primary;
            edge = recipe.secondary;
            const float horizontal_length = std::hypot(projectile->velocity.x, projectile->velocity.z);
            if (!std::isfinite(horizontal_length) || horizontal_length <= 0.0001f) continue;
            direction = {projectile->velocity.x / horizontal_length, 0.0f,
                         projectile->velocity.z / horizontal_length};
            radius = projectile->hitbox_radius;
        }
        if (!std::isfinite(radius) || radius <= 0.0f || !std::isfinite(edge) || edge <= 0.0f ||
            (axial && (!std::isfinite(length) || length <= 0.0f))) continue;
        const float size = axial ? std::max(length * 0.5f + edge,
                                           std::max(radius, 3.0f * edge)) : radius + edge;
        if (!std::isfinite(size) || size <= 0.0f) continue;
        VfxTypedGroundCommand item;
        item.additive = true;
        item.stable_id = static_cast<std::uint64_t>(input.sequence);
        item.effect_handle = input.effect_handle;
        item.edge_width_world = edge;
        item.normalized_age = item.lifetime01 = age;
        item.motion = {recipe.motion_rate_hz, recipe.motion_amplitude, recipe.motion_inset_fraction};
        item.gradient_row = recipe.gradient_row;
        item.hdr = recipe.hdr;
        auto &geometry = item.geometry;
        geometry.shape = heal ? VfxGroundShape::CrossRing :
            (trap ? VfxGroundShape::BrokenHex : VfxGroundShape::AxialFracture);
        geometry.outer_radius = radius;
        geometry.edge_width = edge;
        geometry.progress = progress;
        geometry.direction = direction;
        if (trap)
        {
            geometry.gap_half_angle_radians = break_half_angle;
            geometry.animation_phase = static_cast<float>(input.stable_seed % 360u) *
                (3.14159265358979323846f / 180.0f);
        }
        if (axial) geometry.range = length;
        auto &command = item.command;
        command.sequence = item.stable_id ^ input.event_tick ^
            (static_cast<std::uint64_t>(input.effect_handle) << 32) ^ recipe.output_index;
        command.tick = input.event_tick;
        command.position = {anchor.x, 0.025f, anchor.z};
        command.direction = axial ? direction : Float3{0.0f, 1.0f, 0.0f};
        command.shape = ParticleShape::Point;
        command.velocity_mode = ParticleVelocity::Direction;
        command.facing = ParticleFacing::Ground;
        command.renderer = VfxRenderer::Ground;
        command.primitive = VfxPrimitive::ExactRing;
        command.lifetime_min = command.lifetime_max = duration;
        command.start_size_min = command.start_size_max = size;
        command.end_size_min = command.end_size_max = size;
        command.stretch = axial ? 1.0f : radius / size;
        command.start_color = command.end_color = {
            recipe.rgb.x * recipe.hdr, recipe.rgb.y * recipe.hdr,
            recipe.rgb.z * recipe.hdr, recipe.alpha * envelope};
        if (!Finite({command.start_color.x, command.start_color.y, command.start_color.z}) ||
            !std::isfinite(command.start_color.w)) continue;
        command.seed = input.stable_seed;
        result.push_back(item);
    }
}
void AppendChargedPulseGroundOutputs(const VfxImage &image,
                                     const VfxEffectRecord &effect,
                                     const VfxEventInput &input, float age,
                                     std::vector<VfxTypedGroundCommand> &result)
{
    const auto cooked=image.program;
    const auto *context = std::get_if<VfxContextPayload>(&input.payload);
    if (!context || context->owner_id != (1ull << 60) ||
        !Finite(context->direction) || !std::isfinite(context->ratio01) ||
        context->ratio01 < 0.0f || context->ratio01 > 1.0f ||
        !std::isfinite(context->scalar0) || !std::isfinite(context->scalar1) ||
        std::any_of(input.world_transform.begin(), input.world_transform.end(),
                    [](float value) { return !std::isfinite(value); }) ||
        input.geometry_owner_id != 0 || effect.sources.count != 2 ||
        input.effect_handle>image.ground_effect_ranges.size()) return;
    const Float3 center{input.world_transform[12], input.world_transform[13],
                        input.world_transform[14]};
    // The authored event has no radius payload. This 0.65 m cosmetic ring
    // surrounds the 0.4 m player footprint without claiming a gameplay hitbox.
    constexpr float kCosmeticRadius = 0.65f;
    constexpr float kGroundHeight = 0.025f;
    std::vector<VfxTypedGroundCommand> commands;
    commands.reserve(2);
    const auto range=image.ground_effect_ranges[input.effect_handle-1];
    if(!ValidRange(range,image.ground_recipes)) return;
    const auto recipes=image.ground_recipes.subspan(range.first,range.count);
    for (std::size_t offset = 0; offset < 2; ++offset)
    {
        const auto source_index = static_cast<std::size_t>(effect.sources.first) + offset;
        if (source_index >= cooked.sources.size()) return;
        const auto &source = cooked.sources[source_index];
        const bool ring = offset == 0;
        if (source.effect != effect.handle || source.type != VfxSourceType::Direct ||
            (source.knot_count != 2 && source.knot_count != 4) ||
            source.knots[source.knot_count - 1] <= source.knots[0])
            return;
        auto next=std::ranges::lower_bound(recipes,source_index,{},&VfxGroundRecipe::source_index);
        while(next!=recipes.end() && next->source_index==source_index &&
              next->kind!=(ring?VfxGroundRecipeKind::ChargedBoundary:VfxGroundRecipeKind::ChargedInterior)) ++next;
        if(next==recipes.end()||next->source_index!=source_index) return;
        const auto &recipe=*next;
        if(!recipe.source_parameters_empty || recipe.source_output_count!=1) return;
        const float value=recipe.primary;
        if (!std::isfinite(value) || value <= 0.0f ||
            (ring && value >= kCosmeticRadius)) return;
        float envelope{};
        if (!SourceEnvelope(source, age, envelope))
        {
            // An inactive source is valid; a malformed lifecycle is not.
            if (source.knot_count != 2 && source.knot_count != 4) return;
            for (std::size_t index = 0; index < source.knot_count; ++index)
                if (!std::isfinite(source.knots[index]) || source.knots[index] < 0.0f ||
                    source.knots[index] > 1.0f ||
                    (index && source.knots[index] < source.knots[index - 1])) return;
            continue;
        }
        const float extent = kCosmeticRadius + (ring ? value : 0.0f);
        VfxTypedGroundCommand item;
        item.stable_id = context->owner_id;
        item.effect_handle = input.effect_handle;
        item.edge_width_world = ring ? value : 0.0f;
        item.normalized_age = item.lifetime01 = age;
        item.motion = {recipe.motion_rate_hz, recipe.motion_amplitude,
                       recipe.motion_inset_fraction};
        item.gradient_row = recipe.gradient_row;
        item.hdr = recipe.hdr;
        item.geometry.shape = ring ? VfxGroundShape::StateRing : VfxGroundShape::Circle;
        item.geometry.outer_radius = kCosmeticRadius;
        if (ring)
        {
            item.geometry.inner_radius = kCosmeticRadius - value;
            item.geometry.edge_width = value;
            item.geometry.spokes = 6;
            item.geometry.rings = 1; // The renderer's explicit charge-segment mode.
            item.geometry.progress = context->ratio01;
            item.geometry.animation_phase = age * recipe.seconds;
        }
        auto &command = item.command;
        command.sequence = context->owner_id ^ input.event_tick ^
            (static_cast<std::uint64_t>(input.effect_handle) << 32) ^ recipe.output_index;
        command.tick = input.event_tick;
        command.position = {center.x, kGroundHeight, center.z};
        command.shape = ParticleShape::Point;
        command.velocity_mode = ParticleVelocity::Direction;
        command.facing = ParticleFacing::Ground;
        command.renderer = VfxRenderer::Ground;
        command.primitive = ring ? VfxPrimitive::ExactRing : VfxPrimitive::LowFrequencyFill;
        command.direction = {0.0f, 1.0f, 0.0f};
        command.lifetime_min = command.lifetime_max = recipe.seconds;
        command.start_size_min = command.start_size_max = extent;
        command.end_size_min = command.end_size_max = extent;
        command.stretch = ring ? kCosmeticRadius / extent : value;
        command.start_color = command.end_color = {
            recipe.rgb.x * recipe.hdr, recipe.rgb.y * recipe.hdr,
            recipe.rgb.z * recipe.hdr, recipe.alpha * envelope};
        if (!Finite({command.start_color.x, command.start_color.y,
                     command.start_color.z}) || !std::isfinite(command.start_color.w)) return;
        command.seed = input.stable_seed;
        commands.push_back(item);
    }
    result.insert(result.end(), commands.begin(), commands.end());
}

void AppendPersistentEntityGroundOutputs(const VfxImage &image,
                                  const VfxPersistentInput &input, Tick current_tick,
                                  std::vector<VfxTypedGroundCommand> &result)
{
    const auto cooked=image.program;
    if (input.effect_handle == 0 || input.effect_handle > cooked.effects.size() ||
        !std::holds_alternative<VfxEntityPayload>(input.payload) ||
        static_cast<std::uint32_t>(input.quality) > 2 || input.stable_id == 0)
        return;
    const auto &effect = cooked.effects[input.effect_handle - 1];
    if (effect.handle != input.effect_handle || effect.input_mode != 1 ||
        effect.timing_kind != 1 || effect.payload_kind != kEntityPayload) return;
    if(input.effect_handle>image.ground_effect_ranges.size()) return;
    const auto recipe_range=image.ground_effect_ranges[input.effect_handle-1];
    if(!ValidRange(recipe_range,image.ground_recipes) || !recipe_range.count) return;
    const auto recipes=image.ground_recipes.subspan(recipe_range.first,recipe_range.count);
    const std::uint64_t effect_id=recipes.front().effect_id;
    if(!effect_id || std::any_of(recipes.begin(),recipes.end(),[&](const auto &recipe){return recipe.effect_id!=effect_id;})) return;
    const bool pickup_idle = effect_id == kPickupXpIdle ||
        effect_id == kPickupHealIdle || effect_id == kPickupMagnetIdle ||
        effect_id == kPickupRelicIdle;
    const bool charged_overcharge = effect_id == kChargedOverchargeLoop;
    const bool suicide_countdown = effect_id == Hash64("particle.enemy.suicide.countdown_loop");
    if (effect_id != kPlayerInvulnerable && effect_id != kBossPhase2Aura &&
        effect_id != kBossPhaseTransition && effect_id != kStatusSlow &&
        effect_id != kStatusMark && !pickup_idle && !charged_overcharge && !suicide_countdown) return;
    const bool pickup_relic_idle = effect_id == kPickupRelicIdle;
    const auto &entity = std::get<VfxEntityPayload>(input.payload);
    const Float3 anchor{input.current_transform[12], input.current_transform[13],
                        input.current_transform[14]};
    if (!Finite(anchor) || !std::isfinite(entity.footprint_radius) ||
        entity.footprint_radius <= 0.0f || !std::isfinite(input.normalized_age) ||
        input.normalized_age < 0.0f || input.normalized_age > 1.0f ||
        !std::isfinite(entity.lifetime01) || entity.lifetime01 < 0.0f ||
        entity.lifetime01 > 1.0f || !std::isfinite(input.elapsed_seconds) ||
        input.elapsed_seconds < 0.0f) return;
    const auto source_first = static_cast<std::size_t>(effect.sources.first);
    const auto source_count = static_cast<std::size_t>(effect.sources.count);
    if (source_first > cooked.sources.size() ||
        source_count > cooked.sources.size() - source_first) return;
    for (std::size_t source_index = source_first;
         source_index < source_first + source_count; ++source_index)
    {
        const auto &source = cooked.sources[source_index];
        const float source_age = charged_overcharge && input.normalized_age >= 1.0f &&
                source.knots[1] >= 1.0f
            ? std::nextafter(1.0f, 0.0f)
            : input.normalized_age;
        float envelope{};
        if (source.effect != effect.handle || source.type != VfxSourceType::Direct ||
            source.knot_count != 2 || source.knots[1] <= source.knots[0] ||
            !SourceEnvelope(source, source_age, envelope)) continue;
        const float progress = (source_age - source.knots[0]) /
            (source.knots[1] - source.knots[0]);
        if (!std::isfinite(progress) || progress < 0.0f || progress >= 1.0f) continue;
        auto next=std::ranges::lower_bound(recipes,source_index,{},&VfxGroundRecipe::source_index);
        for(;next!=recipes.end()&&next->source_index==source_index;++next)
        {
            const auto &recipe=*next;
            if (effect_id == kStatusSlow || effect_id == kStatusMark ||
                pickup_idle || charged_overcharge || suicide_countdown)
            {
                std::uint32_t mask_slice = 0xffffffffu;
                float mask_strength = 0.0f;
                bool textures_ok=recipe.texture_contract_valid;
                if(pickup_relic_idle)
                {
                    const auto quality=static_cast<std::uint32_t>(input.quality);
                    textures_ok=quality<recipe.mask_variants.size() && recipe.mask_variants[quality].valid;
                    if(textures_ok && recipe.mask_variants[quality].present)
                    {
                        const auto &mask=recipe.mask_variants[quality];
                        textures_ok=ValidRange(mask.slices,cooked.slice_indices) && mask.slices.count;
                        if(textures_ok)
                        {
                            mask_slice=cooked.slice_indices[mask.slices.first+input.stable_seed%mask.slices.count];
                            mask_strength=mask.strength;
                        }
                    }
                }
                if (recipe.kind!=VfxGroundRecipeKind::PolarRune || recipe.min_quality>static_cast<std::uint32_t>(input.quality) ||
                    !textures_ok || recipe.alpha < 0.0f || recipe.alpha > 1.0f ||
                    (charged_overcharge && entity.render_instance_id == 0) ||
                    std::abs(entity.lifetime01 - input.normalized_age) > 0.00001f ||
                    (pickup_idle && (input.normalized_age != 0.0f ||
                                     entity.lifetime01 != 0.0f ||
                                     entity.render_instance_id == 0)))
                    continue;
                const float rotation_speed=recipe.primary;
                const float phase = input.elapsed_seconds * rotation_speed;
                constexpr float edge = 0.035f;
                const float radius = entity.footprint_radius;
                const float size = radius + edge;
                if (!std::isfinite(rotation_speed) || rotation_speed <= 0.0f ||
                    !std::isfinite(phase) || !std::isfinite(size)) continue;
                VfxTypedGroundCommand item;
                item.stable_id = input.stable_id;
                item.effect_handle = input.effect_handle;
                item.source_visual_kind = input.source_visual_kind;
                item.edge_width_world = edge;
                item.normalized_age = input.normalized_age;
                item.lifetime01 = entity.lifetime01;
                item.motion = {recipe.motion_rate_hz, recipe.motion_amplitude,
                               recipe.motion_inset_fraction};
                item.gradient_row = recipe.gradient_row;
                item.hdr = recipe.hdr;
                item.geometry.shape = VfxGroundShape::PolarRune;
                item.geometry.outer_radius = radius;
                item.geometry.inner_radius = std::max(0.0f, radius - edge);
                item.geometry.edge_width = edge;
                item.geometry.spokes = recipe.tick_count;
                item.geometry.rings = recipe.pulse_curve_interpolation;
                item.geometry.progress = progress;
                item.geometry.animation_phase = phase;
                item.geometry.mask_slice = mask_slice;
                item.geometry.mask_strength = mask_strength;
                auto &command = item.command;
                command.sequence = input.stable_id ^ current_tick ^
                    (static_cast<std::uint64_t>(input.effect_handle) << 32) ^
                    output_index;
                command.tick = current_tick;
                command.position = {anchor.x, 0.025f, anchor.z};
                command.shape = ParticleShape::Point;
                command.velocity_mode = ParticleVelocity::Direction;
                command.facing = ParticleFacing::Ground;
                command.renderer = VfxRenderer::Ground;
                command.primitive = VfxPrimitive::ExactRing;
                command.lifetime_min = command.lifetime_max = 2.0f / 60.0f;
                command.start_size_min = command.start_size_max = size;
                command.end_size_min = command.end_size_max = size;
                command.stretch = radius / size;
                command.start_color = command.end_color = {
                    recipe.rgb.x * recipe.hdr, recipe.rgb.y * recipe.hdr,
                    recipe.rgb.z * recipe.hdr, recipe.alpha * envelope};
                if (!Finite({command.start_color.x, command.start_color.y,
                             command.start_color.z}) ||
                    !std::isfinite(command.start_color.w)) continue;
                command.seed = input.stable_seed;
                result.push_back(item);
                continue;
            }
            if (effect_id == kPlayerInvulnerable)
            {
                if (entity.render_instance_id == 0 || recipe.kind!=VfxGroundRecipeKind::StateRing ||
                    recipe.min_quality>static_cast<std::uint32_t>(input.quality) ||
                    !recipe.texture_contract_valid || recipe.alpha < 0.0f || recipe.alpha > 1.0f ||
                    std::abs(entity.lifetime01 - input.normalized_age) > 0.00001f)
                    continue;
                constexpr float edge = 0.05f;
                const float radius = entity.footprint_radius;
                const float size = radius + edge;
                if (!std::isfinite(size)) continue;
                VfxTypedGroundCommand item;
                item.stable_id = input.stable_id;
                item.effect_handle = input.effect_handle;
                item.source_visual_kind = input.source_visual_kind;
                item.edge_width_world = edge;
                item.normalized_age = input.normalized_age;
                item.lifetime01 = entity.lifetime01;
                item.motion = {recipe.motion_rate_hz, recipe.motion_amplitude,
                               recipe.motion_inset_fraction};
                item.gradient_row = recipe.gradient_row;
                item.hdr = recipe.hdr;
                item.geometry.shape = VfxGroundShape::StateRing;
                item.geometry.outer_radius = radius;
                item.geometry.inner_radius = std::max(0.0f, radius - edge);
                item.geometry.edge_width = edge;
                item.geometry.spokes = recipe.tick_count;
                item.geometry.progress = progress;
                item.geometry.animation_phase = input.elapsed_seconds;
                auto &command = item.command;
                command.sequence = input.stable_id ^ current_tick ^
                    (static_cast<std::uint64_t>(input.effect_handle) << 32) ^ output_index;
                command.tick = current_tick;
                command.position = {anchor.x, 0.025f, anchor.z};
                command.shape = ParticleShape::Point;
                command.velocity_mode = ParticleVelocity::Direction;
                command.facing = ParticleFacing::Ground;
                command.renderer = VfxRenderer::Ground;
                command.primitive = VfxPrimitive::ExactRing;
                command.lifetime_min = command.lifetime_max = 2.0f / 60.0f;
                command.start_size_min = command.start_size_max = size;
                command.end_size_min = command.end_size_max = size;
                command.stretch = radius / size;
                command.start_color = command.end_color = {
                    recipe.rgb.x * recipe.hdr, recipe.rgb.y * recipe.hdr,
                    recipe.rgb.z * recipe.hdr, recipe.alpha * envelope};
                command.seed = input.stable_seed;
                result.push_back(item);
                continue;
            }
            const bool teeth=recipe.kind==VfxGroundRecipeKind::BrokenCrown;
            const bool locked=recipe.kind==VfxGroundRecipeKind::ClosedCrownRing;
            if((!teeth&&!locked)||recipe.min_quality>static_cast<std::uint32_t>(input.quality)) continue;
            const float pulse_hz=recipe.primary;
            const float edge=locked?recipe.secondary:.05f;
            const float inner_radius=teeth?recipe.secondary:0.0f;
            const float phase = input.elapsed_seconds * pulse_hz;
            const float size = entity.footprint_radius + edge;
            if (!std::isfinite(pulse_hz) || pulse_hz <= 0.0f ||
                !std::isfinite(edge) || edge <= 0.0f ||
                (teeth && (!std::isfinite(inner_radius) || inner_radius <= 0.0f ||
                           inner_radius >= entity.footprint_radius)) ||
                !std::isfinite(phase) || !std::isfinite(size)) continue;
            VfxTypedGroundCommand item;
            item.additive = true;
            item.stable_id = input.stable_id;
            item.effect_handle = input.effect_handle;
            item.source_visual_kind = input.source_visual_kind;
            item.edge_width_world = edge;
            item.normalized_age = input.normalized_age;
            item.lifetime01 = entity.lifetime01;
            item.motion = {recipe.motion_rate_hz, recipe.motion_amplitude, recipe.motion_inset_fraction};
            item.gradient_row = recipe.gradient_row;
            item.hdr = recipe.hdr;
            item.geometry.shape = teeth ? VfxGroundShape::BrokenCrown : VfxGroundShape::ClosedCrownRing;
            item.geometry.outer_radius = entity.footprint_radius;
            item.geometry.inner_radius = inner_radius;
            item.geometry.edge_width = edge;
            item.geometry.progress = progress;
            item.geometry.animation_phase = phase;
            auto &command = item.command;
            command.sequence = input.stable_id ^ current_tick ^
                (static_cast<std::uint64_t>(input.effect_handle) << 32) ^ output_index;
            command.tick = current_tick;
            command.position = {anchor.x, 0.025f, anchor.z};
            command.shape = ParticleShape::Point;
            command.velocity_mode = ParticleVelocity::Direction;
            command.facing = ParticleFacing::Ground;
            command.renderer = VfxRenderer::Ground;
            command.primitive = VfxPrimitive::ExactRing;
            command.lifetime_min = command.lifetime_max = 2.0f / 60.0f;
            command.start_size_min = command.start_size_max = size;
            command.end_size_min = command.end_size_max = size;
            command.stretch = entity.footprint_radius / size;
            command.start_color = command.end_color = {
                recipe.rgb.x * recipe.hdr, recipe.rgb.y * recipe.hdr,
                recipe.rgb.z * recipe.hdr, recipe.alpha * envelope};
            if (!Finite({command.start_color.x, command.start_color.y, command.start_color.z}) ||
                !std::isfinite(command.start_color.w)) continue;
            command.seed = input.stable_seed;
            result.push_back(item);
        }
    }
}
void AppendPersistentChevronOutputs(const VfxImage &image,
                                    std::size_t source_index,
                                    const VfxPersistentInput &input,
                                    const VfxLinePayload &line, Tick current_tick,
                                    std::vector<VfxTypedGroundCommand> &result)
{
    const auto cooked=image.program;
    const auto &source = cooked.sources[source_index];
    if (cooked.effects[input.effect_handle - 1].timing_kind != 1 ||
        source.type != VfxSourceType::Direct || source.knot_count != 2 ||
        source.knots[1] <= source.knots[0] || input.stable_id == 0 ||
        !std::isfinite(input.normalized_age) ||
        !std::isfinite(line.lifetime01) ||
        std::abs(input.normalized_age - line.lifetime01) > 0.00001f ||
        !Finite(line.start) || !Finite(line.end) ||
        !std::isfinite(line.width) || line.width <= 0.0f) return;
    float envelope{};
    if (!SourceEnvelope(source, input.normalized_age, envelope)) return;
    const float dx = line.end.x - line.start.x;
    const float dz = line.end.z - line.start.z;
    const float length = std::hypot(dx, dz);
    if (!std::isfinite(length) || length <= 0.0001f) return;
    const Float3 direction{dx / length, 0.0f, dz / length};
    const float half_length = length * 0.5f;
    const float half_width = line.width * 0.5f;
    const float progress = (input.normalized_age - source.knots[0]) /
        (source.knots[1] - source.knots[0]);
    if (!std::isfinite(progress) || progress < 0.0f || progress >= 1.0f) return;
    if(!input.effect_handle || input.effect_handle>image.ground_effect_ranges.size()) return;
    const auto range=image.ground_effect_ranges[input.effect_handle-1];
    if(!ValidRange(range,image.ground_recipes)) return;
    const auto recipes=image.ground_recipes.subspan(range.first,range.count);
    auto next=std::ranges::lower_bound(recipes,source_index,{},&VfxGroundRecipe::source_index);
    for(;next!=recipes.end()&&next->source_index==source_index;++next)
    {
        const auto &recipe=*next;
        if(recipe.kind!=VfxGroundRecipeKind::RepeatingChevron || recipe.min_quality>static_cast<std::uint32_t>(input.quality)) continue;
        const float progress=(input.normalized_age-recipe.knots[0])/(recipe.knots[1]-recipe.knots[0]);
        if(!std::isfinite(progress)||progress<0||progress>=1) continue;
        const float spacing=recipe.primary;
        constexpr float edge = 0.05f;
        const float size = std::max(half_length, half_width) + edge;
        if (!std::isfinite(spacing) || spacing <= 0.0f ||
            !std::isfinite(size) || size <= 0.0f) continue;
        VfxTypedGroundCommand item;
        item.additive = true;
        item.stable_id = input.stable_id;
        item.effect_handle = input.effect_handle;
        item.source_visual_kind = input.source_visual_kind;
        item.edge_width_world = edge;
        item.normalized_age = item.lifetime01 = input.normalized_age;
        item.motion = {recipe.motion_rate_hz, recipe.motion_amplitude,
                       recipe.motion_inset_fraction};
        item.gradient_row = recipe.gradient_row;
        item.hdr = recipe.hdr;
        auto &geometry = item.geometry;
        geometry.shape = VfxGroundShape::RepeatingChevron;
        geometry.direction = direction;
        geometry.half_length = half_length;
        geometry.half_width = half_width;
        geometry.edge_width = edge;
        geometry.spacing = spacing;
        geometry.spokes = recipe.tick_count;
        geometry.progress = progress;
        auto &command = item.command;
        command.sequence = input.stable_id ^ current_tick ^
            (static_cast<std::uint64_t>(input.effect_handle) << 32) ^ recipe.output_index;
        command.tick = current_tick;
        command.position = {(line.start.x + line.end.x) * 0.5f, 0.025f,
                            (line.start.z + line.end.z) * 0.5f};
        command.direction = direction;
        command.shape = ParticleShape::Point;
        command.velocity_mode = ParticleVelocity::Direction;
        command.facing = ParticleFacing::Ground;
        command.renderer = VfxRenderer::Ground;
        command.primitive = VfxPrimitive::ExactRing;
        command.lifetime_min = command.lifetime_max = 2.0f / 60.0f;
        command.start_size_min = command.start_size_max = size;
        command.end_size_min = command.end_size_max = size;
        command.stretch = 1.0f;
        command.start_color = command.end_color = {
            recipe.rgb.x * recipe.hdr, recipe.rgb.y * recipe.hdr,
            recipe.rgb.z * recipe.hdr, recipe.alpha * envelope};
        if (!Finite({command.start_color.x, command.start_color.y, command.start_color.z}) ||
            !std::isfinite(command.start_color.w)) continue;
        command.seed = input.stable_seed;
        result.push_back(item);
    }
}
void AppendSafeSector(const VfxImage &image, const VfxEventInput &input,
                      double tick, std::vector<VfxTypedGroundCommand> &result)
{
    const auto cooked=image.program;
    if (input.effect_handle == 0 || input.effect_handle > cooked.effects.size()) return;
    const auto &effect = cooked.effects[input.effect_handle - 1];
    const auto &sector = std::get<VfxSafeSectorPayload>(input.payload);
    if (effect.handle != input.effect_handle || effect.input_mode != 0 || effect.timing_kind != 1 ||
        effect.payload_kind != Hash32("SafeGapSectorPayload") || input.geometry_owner_id == 0 ||
        input.geometry_end_tick <= input.geometry_start_tick || tick < input.event_tick ||
        tick < input.geometry_start_tick || tick >= input.geometry_end_tick ||
        static_cast<std::uint32_t>(input.quality) > 2 || !Finite(sector.center) || !Finite(sector.direction) ||
        !std::isfinite(sector.inner_radius) || !std::isfinite(sector.outer_radius) || sector.inner_radius < 0 ||
        sector.outer_radius <= sector.inner_radius || !std::isfinite(sector.half_angle_degrees) ||
        sector.half_angle_degrees <= 0 || sector.half_angle_degrees > 180) return;
    const float direction_length = std::hypot(sector.direction.x, sector.direction.z);
    if (!std::isfinite(direction_length) || direction_length <= .0001f) return;
    const float duration = static_cast<float>(input.geometry_end_tick - input.geometry_start_tick) / 60.0f;
    const float elapsed = static_cast<float>(tick - input.geometry_start_tick) / 60.0f;
    const float age = elapsed / duration;
    if (!std::isfinite(duration) || !std::isfinite(age)) return;
    if (effect.sources.first > cooked.sources.size() || effect.sources.count > cooked.sources.size() - effect.sources.first ||
        input.effect_handle>image.ground_effect_ranges.size()) return;
    const auto recipe_range=image.ground_effect_ranges[input.effect_handle-1];
    if(!ValidRange(recipe_range,image.ground_recipes)) return;
    const auto recipes=image.ground_recipes.subspan(recipe_range.first,recipe_range.count);
    for (std::size_t index = effect.sources.first; index < effect.sources.first + effect.sources.count; ++index)
    {
        const auto &source = cooked.sources[index];
        float envelope{};
        if (source.effect != effect.handle || source.type != VfxSourceType::Direct ||
            source.stable_id != Hash32("main_rune") || !SourceEnvelope(source, age, envelope)) continue;
        auto next=std::ranges::lower_bound(recipes,index,{},&VfxGroundRecipe::source_index);
        for(;next!=recipes.end()&&next->source_index==index;++next)
        {
            const auto &recipe=*next;
            if(recipe.kind!=VfxGroundRecipeKind::SafeSector || recipe.min_quality>static_cast<std::uint32_t>(input.quality)) continue;
            const float phase = recipe.primary * elapsed;
            if (!std::isfinite(phase)) continue;
            VfxTypedGroundCommand item;
            item.stable_id = input.geometry_owner_id; item.effect_handle = input.effect_handle;
            item.normalized_age = item.lifetime01 = age; item.gradient_row = recipe.gradient_row; item.hdr = recipe.hdr;
            item.edge_width_world = .035f;
            auto &geometry = item.geometry;
            geometry.shape = VfxGroundShape::SafeSectorMarker;
            geometry.direction = {sector.direction.x / direction_length, 0, sector.direction.z / direction_length};
            geometry.inner_radius = sector.inner_radius; geometry.outer_radius = geometry.range = sector.outer_radius;
            geometry.half_angle_radians = sector.half_angle_degrees * (3.14159265358979323846f / 180);
            geometry.edge_width = item.edge_width_world; geometry.progress = age; geometry.animation_phase = phase;
            geometry.spokes = recipe.tick_count; geometry.rings = recipe.pulse_curve_interpolation;
            auto &command = item.command;
            command.sequence = input.sequence ^ input.geometry_start_tick ^ (static_cast<std::uint64_t>(effect.handle) << 32) ^ recipe.output_index;
            command.tick = input.geometry_start_tick; command.position = sector.center; command.direction = geometry.direction;
            command.shape = ParticleShape::Point; command.velocity_mode = ParticleVelocity::Direction;
            command.facing = ParticleFacing::Ground; command.renderer = VfxRenderer::Ground; command.primitive = VfxPrimitive::ExactRing;
            command.lifetime_min = command.lifetime_max = duration;
            command.start_size_min = command.start_size_max = command.end_size_min = command.end_size_max = sector.outer_radius + .035f;
            command.start_color = command.end_color = {recipe.rgb.x*recipe.hdr, recipe.rgb.y*recipe.hdr,
                recipe.rgb.z*recipe.hdr, recipe.alpha*envelope};
            if (!Finite({command.start_color.x,command.start_color.y,command.start_color.z}) || !std::isfinite(command.start_size_max)) continue;
            command.seed = input.stable_seed;
            result.push_back(item);
        }
    }
}
} // namespace

std::vector<VfxTypedGroundCommand> BuildVfxTypedGroundCommands(
    const VfxImage &image,
    std::span<const VfxPersistentInput> inputs,
    Tick current_tick)
{
    const auto cooked=image.program;
    std::vector<VfxTypedGroundCommand> result;
    for (const auto &input : inputs)
    {
        if (input.source_visual_kind == static_cast<std::uint8_t>(PersistentVfxKind::WaveTimer))
        {
            if (input.effect_handle == 0 || input.effect_handle > cooked.effects.size() ||
                input.stable_id == 0 || !std::holds_alternative<VfxContextPayload>(input.payload) ||
                !std::isfinite(input.elapsed_seconds) || input.elapsed_seconds < 0 ||
                static_cast<std::uint32_t>(input.quality) > 2)
                continue;
            VfxEventInput timer{};
            timer.effect_handle = input.effect_handle;
            timer.sequence = input.stable_id;
            timer.event_tick = current_tick;
            timer.world_transform = input.current_transform;
            timer.stable_seed = input.stable_seed;
            timer.quality = input.quality;
            timer.payload = input.payload;
            const auto first = result.size();
            AppendWaveGroundOutputs(image, cooked.effects[input.effect_handle - 1],
                                    timer, input.normalized_age, result);
            for (std::size_t index = first; index < result.size(); ++index)
            {
                result[index].source_visual_kind = input.source_visual_kind;
                result[index].command.lifetime_min = result[index].command.lifetime_max = 2.0f / 60.0f;
            }
            continue;
        }
        if (std::holds_alternative<VfxEntityPayload>(input.payload))
        {
            AppendPersistentEntityGroundOutputs(image, input, current_tick, result);
            continue;
        }
        if (input.effect_handle == 0 || input.effect_handle > cooked.effects.size() ||
            (!std::holds_alternative<VfxCirclePayload>(input.payload) &&
             !std::holds_alternative<VfxLinePayload>(input.payload) &&
             !std::holds_alternative<VfxRingGapsPayload>(input.payload)))
            continue;
        const auto &effect = cooked.effects[input.effect_handle - 1];
        const bool ring_payload = std::holds_alternative<VfxRingGapsPayload>(input.payload);
        const bool line_payload = std::holds_alternative<VfxLinePayload>(input.payload);
        if (effect.handle != input.effect_handle || effect.input_mode != 1 ||
            effect.payload_kind != (line_payload ? kLinePayload : (ring_payload ? kRingGapsPayload : kCirclePayload)))
            continue;
        const auto &circle = std::get_if<VfxCirclePayload>(&input.payload);
        const auto *ring_gaps = std::get_if<VfxRingGapsPayload>(&input.payload);
        const auto *line = std::get_if<VfxLinePayload>(&input.payload);
        if ((circle && (!Finite(circle->center) || !std::isfinite(circle->radius) || circle->radius <= 0.0f)) ||
            (line && (!Finite(line->start) || !Finite(line->end) || !std::isfinite(line->width) || line->width <= 0.0f)))
            continue;
        if (ring_gaps && (!Finite(ring_gaps->center) || !std::isfinite(ring_gaps->inner_radius) ||
            !std::isfinite(ring_gaps->outer_radius) || ring_gaps->inner_radius < 0 || ring_gaps->outer_radius <= ring_gaps->inner_radius ||
            !std::isfinite(ring_gaps->gap_half_width_degrees) || ring_gaps->gap_half_width_degrees < 0 || ring_gaps->gap_half_width_degrees > 180 ||
            !std::isfinite(ring_gaps->lifetime01) || ring_gaps->lifetime01 < 0 || ring_gaps->lifetime01 > 1 ||
            std::any_of(ring_gaps->gap_angles_degrees.begin(), ring_gaps->gap_angles_degrees.end(), [](float v) { return !std::isfinite(v); }))) continue;
        if (static_cast<std::uint32_t>(input.quality) > 2) continue;
        const float dx = line ? line->end.x - line->start.x : 0.0f;
        const float dz = line ? line->end.z - line->start.z : 0.0f;
        const float line_length = std::sqrt(dx * dx + dz * dz);
        if (line && (!std::isfinite(line_length) || line_length <= 0.0001f)) continue;
        if (!std::isfinite(input.normalized_age) || input.normalized_age < 0.0f || input.normalized_age > 1.0f)
            continue;
        if (circle && (!std::isfinite(circle->lifetime01) || circle->lifetime01 < 0.0f || circle->lifetime01 > 1.0f))
            continue;
        if (line && (!std::isfinite(line->lifetime01) || line->lifetime01 < 0.0f || line->lifetime01 > 1.0f))
            continue;
        /*
            A finite line has an authored source envelope. Circle compatibility
            intentionally retains its historic owner-level timing behavior.
        */

        const auto source_first = static_cast<std::size_t>(effect.sources.first);
        const auto source_count = static_cast<std::size_t>(effect.sources.count);
        if (source_first > cooked.sources.size() || source_count > cooked.sources.size() - source_first)
            continue;
        for (std::size_t source_index = source_first;
             source_index < source_first + source_count; ++source_index)
        {
            const auto &source = cooked.sources[source_index];
            if (source.effect != effect.handle)
                continue;
            GroundOwner owner;
            owner.stable_id = input.stable_id;
            owner.effect_handle = effect.handle;
            owner.source_visual_kind = input.source_visual_kind;
            if (circle) owner.circle = *circle;
            if (line)
            {
                owner.is_line = true;
                owner.line = *line;
                owner.line.direction = {dx / line_length, 0.0f, dz / line_length};
                owner.line_half_width = line->width * 0.5f;
                owner.line_half_length = line_length * 0.5f;
                owner.normalized_age = input.normalized_age;
                float envelope = 1.0f;
                if (!SourceEnvelope(source, input.normalized_age, envelope)) continue;
                owner.alpha = envelope;
            }
            if (ring_gaps)
            {
                owner.is_ring_gaps = true;
                owner.ring_gaps = *ring_gaps;
                float envelope = 1;
                if (!SourceEnvelope(source, ring_gaps->lifetime01, envelope)) continue;
                owner.alpha = envelope;
            }
            owner.normalized_age = ring_gaps ? ring_gaps->lifetime01 : input.normalized_age;
            owner.lifetime01 = line ? line->lifetime01 : (ring_gaps ? ring_gaps->lifetime01 : circle->lifetime01);
            owner.quality = input.quality;
            owner.stable_seed = input.stable_seed;
            owner.command_tick = current_tick;
            owner.command_lifetime = 2.0f / 60.0f;
            if (line) AppendPersistentChevronOutputs(image, source_index, input,
                                                      *line, current_tick, result);
            AppendSourceOutputs(image, source_index, owner, result);
        }
    }
    return result;
}

std::vector<VfxTypedGroundCommand> BuildVfxTypedEventGroundCommands(
    const VfxImage &image,
    VfxEventView inputs,
    double current_tick)
{
    const auto cooked=image.program;
    std::vector<VfxTypedGroundCommand> result;
    for (const auto &input : inputs)
    {
        if (std::holds_alternative<VfxSafeSectorPayload>(input.payload))
        {
            AppendSafeSector(image, input, current_tick, result);
            continue;
        }
        if (input.effect_handle == 0 || input.effect_handle > cooked.effects.size() ||
            (!std::holds_alternative<VfxPointPayload>(input.payload) &&
             !std::holds_alternative<VfxContextPayload>(input.payload) &&
             !std::holds_alternative<VfxProjectilePayload>(input.payload) &&
             !std::holds_alternative<VfxCirclePayload>(input.payload) &&
             !std::holds_alternative<VfxLinePayload>(input.payload) &&
             !std::holds_alternative<VfxConePayload>(input.payload) &&
             !std::holds_alternative<VfxRingGapsPayload>(input.payload)) ||
            current_tick < input.event_tick)
            continue;
        const auto &effect = cooked.effects[input.effect_handle - 1];
        const bool ring_payload = std::holds_alternative<VfxRingGapsPayload>(input.payload);
        const bool point_payload = std::holds_alternative<VfxPointPayload>(input.payload);
        const bool context_payload = std::holds_alternative<VfxContextPayload>(input.payload);
        const bool projectile_payload = std::holds_alternative<VfxProjectilePayload>(input.payload);
        const bool line_payload = std::holds_alternative<VfxLinePayload>(input.payload);
        const bool cone_payload = std::holds_alternative<VfxConePayload>(input.payload);
        if (effect.handle != input.effect_handle || effect.input_mode != 0 ||
            effect.payload_kind != (point_payload ? kPointPayload : (context_payload ? kContextPayload :
                (projectile_payload ? kProjectilePayload : (line_payload ? kLinePayload :
                    (cone_payload ? kConePayload : (ring_payload ? kRingGapsPayload : kCirclePayload)))))) ||
            (effect.timing_kind != 0 && !(effect.timing_kind == 1 && !point_payload && !context_payload && !projectile_payload && !line_payload && !cone_payload && !ring_payload && input.geometry_owner_id != 0)) ||
            (effect.timing_kind == 0 && (!std::isfinite(effect.seconds) || effect.seconds <= 0.0f)) ||
            static_cast<std::uint32_t>(input.quality)>2)
            continue;
        const auto *point = std::get_if<VfxPointPayload>(&input.payload);
        const auto *ring_gaps = std::get_if<VfxRingGapsPayload>(&input.payload);
        const auto *circle = std::get_if<VfxCirclePayload>(&input.payload);
        const auto *line = std::get_if<VfxLinePayload>(&input.payload);
        const auto *cone = std::get_if<VfxConePayload>(&input.payload);
        if ((circle && (!Finite(circle->center) || !std::isfinite(circle->radius) || circle->radius <= 0.0f || circle->inner_radius != 0)) ||
            (point && (!Finite(point->position) || !std::isfinite(point->authored_scale) || point->authored_scale <= 0.0f)) ||
            (line && (!Finite(line->start) || !Finite(line->end) || !std::isfinite(line->width) || line->width <= 0.0f))) continue;
        if (ring_gaps && ((effect.timing_kind == 1 && input.geometry_owner_id == 0) ||
            static_cast<std::uint32_t>(input.quality) > 2 || !Finite(ring_gaps->center) ||
            !std::isfinite(ring_gaps->inner_radius) || !std::isfinite(ring_gaps->outer_radius) ||
            ring_gaps->inner_radius < 0 || ring_gaps->outer_radius <= ring_gaps->inner_radius ||
            !std::isfinite(ring_gaps->gap_half_width_degrees) || ring_gaps->gap_half_width_degrees < 0 || ring_gaps->gap_half_width_degrees > 180 ||
            (!ring_gaps->gap_angles_degrees.empty() && ring_gaps->gap_half_width_degrees == 0) ||
            std::any_of(ring_gaps->gap_angles_degrees.begin(),ring_gaps->gap_angles_degrees.end(),[](float v){return !std::isfinite(v);}))) continue;
        const float dx = line ? line->end.x - line->start.x : 0.0f;
        const float dz = line ? line->end.z - line->start.z : 0.0f;
        const float line_length = std::sqrt(dx * dx + dz * dz);
        if (line && (!std::isfinite(line_length) || line_length <= 0.0001f)) continue;
        const float cone_dx = cone ? cone->direction.x : 0.0f;
        const float cone_dz = cone ? cone->direction.z : 0.0f;
        const float cone_direction_length = std::sqrt(cone_dx * cone_dx + cone_dz * cone_dz);
        if (cone && (!Finite(cone->origin) || !Finite(cone->direction) ||
                     !std::isfinite(cone->range) || cone->range <= 0.0f ||
                     !std::isfinite(cone->half_angle_degrees) || cone->half_angle_degrees <= 0.0f ||
                     cone->half_angle_degrees > 180.0f ||
                     !std::isfinite(cone_direction_length) || cone_direction_length <= 0.0001f)) continue;
        const auto start_tick = input.geometry_owner_id != 0 ? input.geometry_start_tick : input.event_tick;
        if (current_tick < start_tick ||
            (input.geometry_owner_id != 0 && input.geometry_end_tick <= start_tick)) continue;
        const float duration = input.geometry_owner_id != 0
            ? static_cast<float>(input.geometry_end_tick - start_tick) / 60.0f : effect.seconds;
        const float age = static_cast<float>(current_tick - start_tick) / (60.0f * duration);
        if (!std::isfinite(age) || age < 0.0f || age >= 1.0f)
            continue;
        if (context_payload)
        {
            AppendWaveGroundOutputs(image, effect, input, age, result);
            AppendChargedPulseGroundOutputs(image, effect, input, age, result);
        }
        const auto source_first = static_cast<std::size_t>(effect.sources.first);
        const auto source_count = static_cast<std::size_t>(effect.sources.count);
        if (source_first > cooked.sources.size() ||
            source_count > cooked.sources.size() - source_first)
            continue;
        for (std::size_t source_index = source_first;
             source_index < source_first + source_count; ++source_index)
        {
            const auto &source = cooked.sources[source_index];
            if (source.effect != effect.handle || source.type != VfxSourceType::Direct ||
                (source.knot_count != 2 && source.knot_count != 4))
                continue;
            float envelope = 1.0f;
            if (!SourceEnvelope(source, age, envelope)) continue;
            AppendExpandingRingOutputs(image, source_index, input, age, envelope, duration, result);
            AppendHexConstellationOutputs(image, source_index, input, age, envelope, duration, result);
            AppendBossSignatureOutputs(image, source_index, input, age, envelope, duration, result);
            AppendRemainingEventGroundOutputs(image, source_index, input, age, envelope, duration, result);
            if (point || context_payload || projectile_payload || (ring_gaps && input.geometry_owner_id == 0)) continue;
            GroundOwner owner;
            owner.stable_id = input.geometry_owner_id != 0 ? input.geometry_owner_id : static_cast<std::uint64_t>(input.sequence);
            owner.effect_handle = effect.handle;
            if (circle) { owner.circle = *circle; owner.is_circle_preview = true; owner.gameplay_warning = effect.timing_kind == 1; }
            if (ring_gaps) { owner.is_ring_gaps = true; owner.is_ring_preview = true; owner.ring_gaps = *ring_gaps; }
            if (line)
            {
                owner.is_line = true;
                owner.line = *line;
                owner.line.direction = {dx / line_length, 0.0f, dz / line_length};
                owner.line_half_width = line->width * 0.5f;
                owner.line_half_length = line_length * 0.5f;
            }
            if (cone)
            {
                owner.is_cone = true;
                owner.cone = *cone;
                owner.cone.direction = {cone_dx / cone_direction_length, 0.0f,
                                        cone_dz / cone_direction_length};
                owner.cone_range = cone->range;
                owner.cone_half_angle_radians = cone->half_angle_degrees *
                    (3.14159265358979323846f / 180.0f);
            }
            owner.normalized_age = age;
            owner.lifetime01 = line ? line->lifetime01 :
                (cone ? cone->lifetime01 : (ring_gaps || (circle && input.geometry_owner_id != 0) ? age : circle->lifetime01));
            owner.alpha = envelope;
            owner.quality = input.quality;
            owner.stable_seed = input.stable_seed;
            owner.command_tick = start_tick;
            owner.command_lifetime = duration;
            AppendSourceOutputs(image, source_index, owner, result);
        }
    }
    return result;
}

} // namespace hs::runtime_detail
