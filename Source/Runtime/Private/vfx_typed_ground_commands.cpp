#include "vfx_typed_ground_commands.hpp"
#include <hs/render_data/vfx_payload_ids.hpp>

#include <hs/render_data/render_snapshot_values.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace hs::runtime_detail
{
namespace
{
constexpr auto kRingGapsPayload = hs::vfx_payload::RingWithGaps;
constexpr auto kCirclePayload = hs::vfx_payload::CircleArea;
constexpr auto kPointPayload = hs::vfx_payload::PointEvent;
constexpr auto kContextPayload = hs::vfx_payload::PresentationContext;
constexpr auto kProjectilePayload = hs::vfx_payload::Projectile;
constexpr auto kLinePayload = hs::vfx_payload::LineArea;
constexpr auto kConePayload = hs::vfx_payload::Cone;

class GroundCommandOutput
{
  public:
    GroundCommandOutput(std::vector<VfxGroundSpawnInput> &output,
                        std::vector<float> &frame_gap_angles) noexcept
        : output_(output), frame_gap_angles_(frame_gap_angles) {}

    [[nodiscard]] std::size_t Size() const noexcept { return output_.size(); }
    void Push(VfxGroundSpawnInput &&command) { output_.push_back(std::move(command)); }
    void AppendPulseRows(std::span<const VfxGroundSpawnInput> commands)
    {
        output_.insert(output_.end(), commands.begin(), commands.end());
    }

    void BeginInput() noexcept
    {
        current_gap_count_ = 0;
    }

    void SetRingGapSlice(VfxGroundGeometry &geometry, const VfxRingGapsPayload &ring)
    {
        geometry.uniform_gaps = ring.uniform_gaps;
        if (ring.uniform_gaps.Active() || ring.gap_angles_degrees.empty()) return;
        if (current_gap_count_ == 0)
        {
            const auto count = ring.gap_angles_degrees.size();
            if (frame_gap_angles_.size() > UINT32_MAX || count > UINT32_MAX - frame_gap_angles_.size())
                throw std::length_error("Ground gap pool exceeds native index capacity.");
            current_gap_first_ = static_cast<std::uint32_t>(frame_gap_angles_.size());
            for (const float degrees : ring.gap_angles_degrees)
            {
                float wrapped = std::fmod(degrees, 360.0f);
                if (wrapped < 0.0f) wrapped += 360.0f;
                frame_gap_angles_.push_back(wrapped * (3.14159265358979323846f / 180.0f));
            }
            current_gap_count_ = static_cast<std::uint32_t>(count);
        }
        geometry.frame_gap_first = current_gap_first_;
        geometry.frame_gap_count = current_gap_count_;
    }

    void MarkWaveTimer(std::size_t first, std::uint8_t source_visual_kind)
    {
        for (std::size_t index = first; index < output_.size(); ++index)
        {
            auto &command = output_[index];
            command.source_visual_kind = source_visual_kind;
            command.command.lifetime_min = command.command.lifetime_max = 2.0f / 60.0f;
        }
    }

  private:
    std::vector<VfxGroundSpawnInput> &output_;
    std::vector<float> &frame_gap_angles_;
    std::uint32_t current_gap_first_{}, current_gap_count_{};
};

bool Finite(Float3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

std::span<const VfxGroundRecipe> TakeSourceRecipes(std::span<const VfxGroundRecipe> &remaining)
{
    const auto count = remaining.front().source_row_count;
    const auto group = remaining.first(count);
    remaining = remaining.subspan(count);
    return group;
}

std::array<std::span<const VfxGroundRecipe>, 2> TwoSourceRecipes(
    std::span<const VfxGroundRecipe> recipes, std::uint32_t first_source)
{
    const auto split = recipes.front().source_index == first_source ? recipes.front().source_row_count : 0u;
    return {recipes.first(split), recipes.subspan(split)};
}

bool SourceEnvelope(std::uint32_t knot_count, const std::array<float, 4> &knots,
                    float age, float &envelope) noexcept
{
    // Immutable knot validity and ordering are checked while preparing the
    // image. Only live age and the envelope value remain frame-dependent.
    if (age < knots[0] || age >= knots[knot_count - 1]) return false;
    envelope = 1.0f;
    if (knot_count == 4)
    {
        if (age < knots[1])
            envelope = (age - knots[0]) / (knots[1] - knots[0]);
        else if (age > knots[2])
            envelope = (knots[3] - age) / (knots[3] - knots[2]);
    }
    return std::isfinite(envelope) && envelope > 0.0f;
}

float IntegratedPulse(std::span<const VfxCurveKey> keys, float age, float duration)
{
    float integral=0;
    for (std::size_t i=1;i<keys.size();++i)
    {
        const auto &key=keys[i];
        const auto &previous=keys[i-1];
        if (age <= previous.time) break;
        const float end=std::min(age,key.time);
        if (end>previous.time)
        {
            const float elapsed=end-previous.time;
            const float value=previous.value+(key.value-previous.value)*(elapsed/(key.time-previous.time));
            integral += elapsed*(previous.value+value)*.5f;
        }
    }
    return duration * integral;
}

struct GroundOwner
{
    std::uint64_t stable_id{};
    VfxEffectHandle effect_handle{};
    std::uint8_t source_visual_kind{0xff};
    VfxCirclePayload circle{};
    VfxLinePayload line{};
    VfxConePayload cone{};
    const VfxRingGapsPayload *ring_gaps{};
    bool is_circle_preview{false};
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

void AppendSourceOutputs(const VfxImage &image, std::span<const VfxGroundRecipe> recipes,
                         const GroundOwner &owner,
                         GroundCommandOutput &result,
                         VfxGroundBirthSuppressionFacts *birth_facts = nullptr)
{
    for (const auto &recipe : recipes)
    {
        if (!recipe.direct_output || recipe.min_quality > static_cast<std::uint32_t>(owner.quality))
            continue;
        const bool ring = recipe.kind == VfxGroundRecipeKind::Ring;
        const bool border = recipe.kind == VfxGroundRecipeKind::Border;
        const bool preview = recipe.kind == VfxGroundRecipeKind::Preview;
        float phase = 0.0f;
        if (preview)
        {
            const auto keys = image.program.curve_keys.subspan(recipe.pulse_curve_keys.first,
                                                               recipe.pulse_curve_keys.count);
            phase = IntegratedPulse(keys, owner.normalized_age, owner.command_lifetime);
            if (!std::isfinite(phase)) continue;
        }
        const float primary = recipe.primary;
        const float secondary = recipe.secondary;
        const auto output_index=recipe.output_index;
        const float width = ring || border || preview ? primary : 0.0f;
        const float size = owner.is_line
            ? std::sqrt(owner.line_half_width * owner.line_half_width +
                        owner.line_half_length * owner.line_half_length)
            : (owner.is_cone ? owner.cone_range + width : (owner.ring_gaps ? owner.ring_gaps->outer_radius : owner.circle.radius) + width);
        if (!std::isfinite(size)) continue;
        const Float4 color{recipe.hdr_rgb.x, recipe.hdr_rgb.y,
                           recipe.hdr_rgb.z, recipe.alpha * owner.alpha};
        if ((owner.ring_gaps || owner.is_circle_preview) &&
            (!Finite({color.x, color.y, color.z}) || !std::isfinite(color.w))) continue;
        if (birth_facts)
        {
            if (owner.is_line && border)
                birth_facts->segment_solid_trail = true;
            else if (!owner.is_line && !owner.is_cone && !owner.ring_gaps)
            {
                if (preview || ring) birth_facts->ground_ring = true;
                else if (recipe.kind == VfxGroundRecipeKind::Fill) birth_facts->ground_disc = true;
            }
            continue;
        }
        VfxGroundSpawnInput item;
        item.stable_id = owner.stable_id;
        item.effect_handle = owner.effect_handle;
        item.source_visual_kind = owner.source_visual_kind;
        item.motion = {recipe.motion_rate_hz, recipe.motion_amplitude,
                       recipe.motion_inset_fraction};
        item.gradient_row = recipe.gradient_row;
        item.hdr = recipe.hdr;
        auto &command = item.command;
        command.sequence = owner.stable_id ^ owner.command_tick ^
            (static_cast<std::uint64_t>(owner.effect_handle) << 32) ^
            static_cast<std::uint64_t>(output_index);
        command.tick = owner.command_tick;
        command.position = owner.is_cone ? owner.cone.origin : (owner.ring_gaps ? owner.ring_gaps->center : owner.circle.center);
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
        command.stretch = ring ? (owner.ring_gaps ? owner.ring_gaps->outer_radius : owner.circle.radius) / size :
            (border ? 0.8f : primary);
        item.geometry.shape = recipe.shape;
        if (owner.ring_gaps)
        {
            item.geometry.progress = owner.normalized_age;
            item.geometry.animation_phase = phase;
            item.geometry.tick_count = recipe.tick_count;
            item.geometry.inner_radius = owner.ring_gaps->inner_radius;
            item.geometry.outer_radius = owner.ring_gaps->outer_radius;
            item.geometry.edge_width = width;
            item.geometry.gap_half_angle_radians = owner.ring_gaps->gap_half_width_degrees * (3.14159265358979323846f / 180.0f);
            result.SetRingGapSlice(item.geometry, *owner.ring_gaps);
        }
        if(owner.is_circle_preview && (preview || recipe.kind == VfxGroundRecipeKind::Ticks))
        {
            item.geometry.progress=owner.normalized_age;
            item.geometry.animation_phase=phase;
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
            item.geometry.scroll = border ? 0.8f : secondary;
        }
        if (owner.is_cone)
        {
            item.geometry.direction = owner.cone.direction;
            item.geometry.range = owner.cone_range;
            item.geometry.half_angle_radians = owner.cone_half_angle_radians;
            item.geometry.edge_width = border ? primary : 0.0f;
            item.geometry.spacing = border ? secondary : primary;
            item.geometry.scroll = border ? 0.8f : secondary;
        }
        command.start_color = command.end_color = color;
        command.seed = owner.stable_seed;
        result.Push(std::move(item));
    }
}
void AppendWaveGroundOutputs(const VfxImage &image, const VfxEventInput &input, float age,
                            std::span<const VfxGroundRecipe> recipes,
                            const std::array<std::span<const VfxGroundRecipe>, 2> &source_recipes,
                            GroundCommandOutput &result,
                            VfxGroundBirthSuppressionFacts *birth_facts = nullptr)
{
    if (recipes.empty()) return;
    const auto flags = recipes.front().admission_profiles;
    const bool warning = (flags & kVfxGroundEventWaveWarning) != 0;
    if (!(flags & (kVfxGroundEventWaveStart | kVfxGroundEventWaveWarning))) return;
    const auto *context = std::get_if<VfxContextPayload>(&input.payload);
    if (!context || !std::isfinite(context->scalar0) || context->scalar0 <= 0 ||
        !std::isfinite(age) || age < 0 || age >= 1)
        return;
    const Float3 center{input.world_transform[12], input.world_transform[13],
                        input.world_transform[14]};
    if (!Finite(center)) return;
    for (std::size_t offset = 0; offset < source_recipes.size(); ++offset)
    {
        const auto &source_rows = source_recipes[offset];
        if (source_rows.empty() || !source_rows.front().wave_source_supported) continue;
        const auto &source_recipe = source_rows.front();
        float envelope{};
        if (!SourceEnvelope(source_recipe.knot_count, source_recipe.knots, age, envelope)) continue;
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
        owner.command_lifetime = recipes.front().seconds;
        AppendSourceOutputs(image, source_rows, owner, result, birth_facts);
    }
}

void AppendExpandingRingOutputs(const VfxEventInput &input, float age, float envelope,
                                float duration, std::span<const VfxGroundRecipe> recipes,
                                GroundCommandOutput &result,
                                VfxGroundBirthSuppressionFacts *birth_facts = nullptr)
{
    const auto *point = std::get_if<VfxPointPayload>(&input.payload);
    const auto *circle = std::get_if<VfxCirclePayload>(&input.payload);
    if (!point && !circle) return;
    const Float3 center = point ? point->position : circle->center;
    const float scale = point ? point->authored_scale : circle->radius;
    if (!Finite(center) || !std::isfinite(scale) || scale <= 0.0f) return;
    for (const auto &recipe : recipes)
    {
        if(recipe.kind!=VfxGroundRecipeKind::ExpandingRing || recipe.min_quality>static_cast<std::uint32_t>(input.quality)) continue;
        const float progress = (age - recipe.knots[0]) / (recipe.knots[1] - recipe.knots[0]);
        if (!std::isfinite(progress) || progress < 0.0f || progress >= 1.0f) continue;
        const float radius = std::lerp(recipe.primary, recipe.secondary, progress) * scale;
        const float edge_width=recipe.tertiary;
        const float size = radius + edge_width;
        if (!std::isfinite(radius) || !std::isfinite(size) || radius <= 0.0f) continue;
        const Float4 color{recipe.hdr_rgb.x, recipe.hdr_rgb.y,
                           recipe.hdr_rgb.z, recipe.alpha * envelope};
        if (!Finite({color.x, color.y, color.z}) || !std::isfinite(color.w)) continue;
        if (birth_facts)
        {
            birth_facts->ground_ring = true;
            continue;
        }
        VfxGroundSpawnInput item;
        item.additive = true;
        item.stable_id = input.geometry_owner_id != 0 ? input.geometry_owner_id : static_cast<std::uint64_t>(input.sequence);
        item.effect_handle = input.effect_handle;
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
        command.start_color = command.end_color = color;
        command.seed = input.stable_seed;
        result.Push(std::move(item));
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
        slice=cooked.slice_indices[variant.slices.first+input.stable_seed%variant.slices.count];
    }
    return true;
}

void AppendHexConstellationOutputs(const VfxImage &image, const VfxEventInput &input, float age, float envelope,
                                   float duration, std::span<const VfxGroundRecipe> recipes,
                                   GroundCommandOutput &result)
{
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
    for (const auto &recipe : recipes)
    {
        if(recipe.kind!=VfxGroundRecipeKind::HexConstellation || recipe.min_quality>static_cast<std::uint32_t>(input.quality)) continue;
        const float progress=(age-recipe.knots[0])/(recipe.knots[1]-recipe.knots[0]);
        if(!std::isfinite(progress)||progress<0||progress>=1) continue;
        std::uint32_t mask_slice{};
        float mask_strength{};
        if (!HexMask(image,recipe,input,mask_slice,mask_strength)) continue;
        VfxGroundSpawnInput item;
        item.additive = true;
        item.stable_id = static_cast<std::uint64_t>(input.sequence);
        item.effect_handle = input.effect_handle;
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
            recipe.hdr_rgb.x, recipe.hdr_rgb.y,
            recipe.hdr_rgb.z, recipe.alpha * envelope};
        if (!Finite({command.start_color.x, command.start_color.y, command.start_color.z}) ||
            !std::isfinite(command.start_color.w)) continue;
        command.seed = input.stable_seed;
        result.Push(std::move(item));
    }
}
void AppendBossSignatureOutputs(const VfxEventInput &input, float age, float envelope,
                                float duration, std::span<const VfxGroundRecipe> recipes,
                                GroundCommandOutput &result)
{
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
    for (const auto &recipe : recipes)
    {
        if(recipe.kind!=VfxGroundRecipeKind::BossSignature || recipe.min_quality>static_cast<std::uint32_t>(input.quality)) continue;
        const float progress=(age-recipe.knots[0])/(recipe.knots[1]-recipe.knots[0]);
        if(!std::isfinite(progress)||progress<0||progress>=1) continue;
        VfxGroundSpawnInput item;
        item.additive = true;
        item.stable_id = input.geometry_owner_id != 0 ? input.geometry_owner_id : static_cast<std::uint64_t>(input.sequence);
        item.effect_handle = input.effect_handle;
        item.motion = {recipe.motion_rate_hz, recipe.motion_amplitude, recipe.motion_inset_fraction};
        item.gradient_row = recipe.gradient_row;
        item.hdr = recipe.hdr;
        auto &geometry = item.geometry;
        geometry.shape = recipe.shape;
        geometry.progress = progress;
        geometry.animation_phase = 1.0f;
        geometry.edge_width = kWorldEdgeWidth;
        geometry.outer_radius = radius;
        if (ring)
        {
            geometry.inner_radius = ring->inner_radius;
            geometry.gap_half_angle_radians = ring->gap_half_width_degrees *
                (3.14159265358979323846f / 180.0f);
            result.SetRingGapSlice(geometry, *ring);
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
            recipe.hdr_rgb.x, recipe.hdr_rgb.y,
            recipe.hdr_rgb.z, recipe.alpha * envelope};
        if (!Finite({command.start_color.x, command.start_color.y, command.start_color.z}) ||
            !std::isfinite(command.start_color.w)) continue;
        command.seed = input.stable_seed;
        result.Push(std::move(item));
    }
}
void AppendRemainingEventGroundOutputs(const VfxEventInput &input, float age, float envelope,
                                       float duration, std::span<const VfxGroundRecipe> recipes,
                                       GroundCommandOutput &result)
{
    const auto *point = std::get_if<VfxPointPayload>(&input.payload);
    const auto *circle = std::get_if<VfxCirclePayload>(&input.payload);
    const auto *projectile = std::get_if<VfxProjectilePayload>(&input.payload);
    if (!point && !circle && !projectile) return;
    const auto kind = point ? VfxGroundRecipeKind::CrossRing :
        circle ? VfxGroundRecipeKind::BrokenHex : VfxGroundRecipeKind::AxialFracture;
    const Float3 anchor = point ? point->position : (circle ? circle->center :
        Float3{input.world_transform[12], input.world_transform[13], input.world_transform[14]});
    if (!Finite(anchor)) return;
    for (const auto &recipe : recipes)
    {
        if (recipe.kind != kind || recipe.min_quality > static_cast<std::uint32_t>(input.quality)) continue;
        const float progress=(age-recipe.knots[0])/(recipe.knots[1]-recipe.knots[0]);
        if(!std::isfinite(progress)||progress<0||progress>=1) continue;
        float radius{};
        float edge{};
        float length{};
        float break_half_angle{};
        Float3 direction{0.0f, 0.0f, 1.0f};
        if (point)
        {
            radius = 0.65f * point->authored_scale;
            edge = 0.05f;
        }
        else if (circle)
        {
            edge = recipe.primary;
            const float sector_degrees = recipe.secondary;
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
        if (!std::isfinite(radius) || radius <= 0.0f) continue;
        const float size = projectile ? std::max(length * 0.5f + edge,
                                           std::max(radius, 3.0f * edge)) : radius + edge;
        if (!std::isfinite(size) || size <= 0.0f) continue;
        VfxGroundSpawnInput item;
        item.additive = true;
        item.stable_id = static_cast<std::uint64_t>(input.sequence);
        item.effect_handle = input.effect_handle;
        item.motion = {recipe.motion_rate_hz, recipe.motion_amplitude, recipe.motion_inset_fraction};
        item.gradient_row = recipe.gradient_row;
        item.hdr = recipe.hdr;
        auto &geometry = item.geometry;
        geometry.shape = recipe.shape;
        geometry.outer_radius = radius;
        geometry.edge_width = edge;
        geometry.progress = progress;
        geometry.direction = direction;
        if (circle)
        {
            geometry.gap_half_angle_radians = break_half_angle;
            geometry.animation_phase = static_cast<float>(input.stable_seed % 360u) *
                (3.14159265358979323846f / 180.0f);
        }
        if (projectile) geometry.range = length;
        auto &command = item.command;
        command.sequence = item.stable_id ^ input.event_tick ^
            (static_cast<std::uint64_t>(input.effect_handle) << 32) ^ recipe.output_index;
        command.tick = input.event_tick;
        command.position = {anchor.x, 0.025f, anchor.z};
        command.direction = projectile ? direction : Float3{0.0f, 1.0f, 0.0f};
        command.shape = ParticleShape::Point;
        command.velocity_mode = ParticleVelocity::Direction;
        command.facing = ParticleFacing::Ground;
        command.renderer = VfxRenderer::Ground;
        command.primitive = VfxPrimitive::ExactRing;
        command.lifetime_min = command.lifetime_max = duration;
        command.start_size_min = command.start_size_max = size;
        command.end_size_min = command.end_size_max = size;
        command.stretch = projectile ? 1.0f : radius / size;
        command.start_color = command.end_color = {
            recipe.hdr_rgb.x, recipe.hdr_rgb.y,
            recipe.hdr_rgb.z, recipe.alpha * envelope};
        if (!Finite({command.start_color.x, command.start_color.y, command.start_color.z}) ||
            !std::isfinite(command.start_color.w)) continue;
        command.seed = input.stable_seed;
        result.Push(std::move(item));
    }
}
struct ChargedPulseOutput
{
    const VfxGroundRecipe *recipe{};
    bool ring{};
    float envelope{}, extent{};
};

std::size_t EvaluateChargedPulseOutputs(const VfxEventInput &input, float age,
    const std::array<std::span<const VfxGroundRecipe>, 2> &source_recipes,
    std::array<ChargedPulseOutput, 2> &outputs)
{
    const auto *context = std::get_if<VfxContextPayload>(&input.payload);
    if (!context || context->owner_id != (1ull << 60) ||
        !Finite(context->direction) || !std::isfinite(context->ratio01) ||
        context->ratio01 < 0.0f || context->ratio01 > 1.0f ||
        !std::isfinite(context->scalar0) || !std::isfinite(context->scalar1) ||
        std::any_of(input.world_transform.begin(), input.world_transform.end(),
                    [](float value) { return !std::isfinite(value); }) ||
        input.geometry_owner_id != 0) return 0;
    constexpr float kCosmeticRadius = 0.65f;
    std::size_t output_count{};
    for (std::size_t offset = 0; offset < 2; ++offset)
    {
        const bool ring = offset == 0;
        const auto &source_rows = source_recipes[offset];
        const auto next = std::ranges::find_if(source_rows, [&](const auto &recipe) {
            return recipe.kind == (ring ? VfxGroundRecipeKind::ChargedBoundary : VfxGroundRecipeKind::ChargedInterior);
        });
        if (next == source_rows.end()) return 0;
        const auto &recipe = *next;
        const float value=recipe.primary;
        float envelope{};
        if (!SourceEnvelope(recipe.knot_count, recipe.knots, age, envelope)) continue;
        const float extent = kCosmeticRadius + (ring ? value : 0.0f);
        const Float4 color{recipe.hdr_rgb.x, recipe.hdr_rgb.y,
                           recipe.hdr_rgb.z, recipe.alpha * envelope};
        if (!Finite({color.x, color.y, color.z}) || !std::isfinite(color.w)) return 0;
        outputs[output_count++] = {&recipe, ring, envelope, extent};
    }
    return output_count;
}

void AppendChargedPulseGroundOutputs(const VfxEventInput &input, float age,
                                     const std::array<std::span<const VfxGroundRecipe>, 2> &source_recipes,
                                     GroundCommandOutput &result,
                                     VfxGroundBirthSuppressionFacts *birth_facts = nullptr)
{
    const auto *context = std::get_if<VfxContextPayload>(&input.payload);
    if (!context) return;
    std::array<ChargedPulseOutput, 2> outputs{};
    const auto output_count = EvaluateChargedPulseOutputs(
        input, age, source_recipes, outputs);
    if (birth_facts)
    {
        for (std::size_t i = 0; i < output_count; ++i)
        {
            if (outputs[i].ring) birth_facts->all_particles = true;
            else birth_facts->ground_disc = true;
        }
        return;
    }
    if (!output_count) return;
    const Float3 center{input.world_transform[12], input.world_transform[13],
                        input.world_transform[14]};
    constexpr float kCosmeticRadius = 0.65f;
    constexpr float kGroundHeight = 0.025f;
    std::array<VfxGroundSpawnInput, 2> commands{};
    for (std::size_t index = 0; index < output_count; ++index)
    {
        const auto &accepted = outputs[index];
        const auto &recipe = *accepted.recipe;
        const bool ring = accepted.ring;
        const float value = recipe.primary;
        const float extent = accepted.extent;
        auto &item = commands[index];
        item.stable_id = context->owner_id;
        item.effect_handle = input.effect_handle;
        item.motion = {recipe.motion_rate_hz, recipe.motion_amplitude,
                       recipe.motion_inset_fraction};
        item.gradient_row = recipe.gradient_row;
        item.hdr = recipe.hdr;
        item.geometry.shape = recipe.shape;
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
            recipe.hdr_rgb.x, recipe.hdr_rgb.y,
            recipe.hdr_rgb.z, recipe.alpha * accepted.envelope};
        command.seed = input.stable_seed;
    }
    result.AppendPulseRows(std::span<VfxGroundSpawnInput>(commands.data(), output_count));
}

void AppendPersistentEntityGroundOutputs(const VfxImage &image,
                                  const VfxPersistentInput &input, Tick current_tick,
                                  GroundCommandOutput &result)
{
    const auto &cooked = image.program;
    if (input.effect_handle == 0 || input.effect_handle > image.ground_effect_ranges.size() ||
        !std::holds_alternative<VfxEntityPayload>(input.payload) ||
        static_cast<std::uint32_t>(input.quality) > 2 || input.stable_id == 0)
        return;
    const auto recipe_range=image.ground_effect_ranges[input.effect_handle-1];
    if(!recipe_range.count) return;
    const auto recipes=image.ground_recipes.subspan(recipe_range.first,recipe_range.count);
    const auto routes = recipes.front().admission_profiles;
    if (!(routes & (kVfxGroundEntityPolar | kVfxGroundEntityState | kVfxGroundEntityCrown))) return;
    const bool pickup_idle = (routes & kVfxGroundEntityPickup) != 0;
    const bool charged_overcharge = (routes & kVfxGroundEntityCharged) != 0;
    const bool pickup_relic_idle = (routes & kVfxGroundEntityRelic) != 0;
    const auto &entity = std::get<VfxEntityPayload>(input.payload);
    const Float3 anchor{input.current_transform[12], input.current_transform[13],
                        input.current_transform[14]};
    if (!Finite(anchor) || !std::isfinite(entity.footprint_radius) ||
        entity.footprint_radius <= 0.0f || !std::isfinite(input.normalized_age) ||
        input.normalized_age < 0.0f || input.normalized_age > 1.0f ||
        !std::isfinite(entity.lifetime01) || entity.lifetime01 < 0.0f ||
        entity.lifetime01 > 1.0f || !std::isfinite(input.elapsed_seconds) ||
        input.elapsed_seconds < 0.0f) return;
    for (auto remaining_recipes = recipes; !remaining_recipes.empty();)
    {
        const auto source_recipes = TakeSourceRecipes(remaining_recipes);
        const auto &source_recipe = source_recipes.front();
        const float source_age = charged_overcharge && input.normalized_age >= 1.0f &&
                source_recipe.knots[1] >= 1.0f
            ? std::nextafter(1.0f, 0.0f)
            : input.normalized_age;
        float envelope{};
        if (!SourceEnvelope(source_recipe.knot_count, source_recipe.knots, source_age, envelope)) continue;
        const float progress = (source_age - source_recipe.knots[0]) /
            (source_recipe.knots[1] - source_recipe.knots[0]);
        if (!std::isfinite(progress) || progress < 0.0f || progress >= 1.0f) continue;
        for (const auto &recipe : source_recipes)
        {
            if (routes & kVfxGroundEntityPolar)
            {
                std::uint32_t mask_slice = 0xffffffffu;
                float mask_strength = 0.0f;
                // Non-relic texture contracts are admitted by PrepareVfxGroundRecipes.
                // Relic mask availability remains quality dependent.
                bool textures_ok=true;
                if(pickup_relic_idle)
                {
                    const auto quality=static_cast<std::uint32_t>(input.quality);
                    const auto &mask=recipe.mask_variants[quality];
                    textures_ok=mask.valid;
                    if(textures_ok && mask.present)
                    {
                        mask_slice=cooked.slice_indices[mask.slices.first+input.stable_seed%mask.slices.count];
                        mask_strength=mask.strength;
                    }
                }
                if (recipe.kind!=VfxGroundRecipeKind::PolarRune || recipe.min_quality>static_cast<std::uint32_t>(input.quality) ||
                    !textures_ok ||
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
                if (!std::isfinite(phase) || !std::isfinite(size)) continue;
                VfxGroundSpawnInput item;
                item.stable_id = input.stable_id;
                item.effect_handle = input.effect_handle;
                item.source_visual_kind = input.source_visual_kind;
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
                    recipe.output_index;
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
                    recipe.hdr_rgb.x, recipe.hdr_rgb.y,
                    recipe.hdr_rgb.z, recipe.alpha * envelope};
                if (!Finite({command.start_color.x, command.start_color.y,
                             command.start_color.z}) ||
                    !std::isfinite(command.start_color.w)) continue;
                command.seed = input.stable_seed;
                result.Push(std::move(item));
                continue;
            }
            if (routes & kVfxGroundEntityState)
            {
                if (entity.render_instance_id == 0 || recipe.kind!=VfxGroundRecipeKind::StateRing ||
                    recipe.min_quality>static_cast<std::uint32_t>(input.quality) ||
                    std::abs(entity.lifetime01 - input.normalized_age) > 0.00001f)
                    continue;
                constexpr float edge = 0.05f;
                const float radius = entity.footprint_radius;
                const float size = radius + edge;
                if (!std::isfinite(size)) continue;
                VfxGroundSpawnInput item;
                item.stable_id = input.stable_id;
                item.effect_handle = input.effect_handle;
                item.source_visual_kind = input.source_visual_kind;
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
                    (static_cast<std::uint64_t>(input.effect_handle) << 32) ^ recipe.output_index;
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
                    recipe.hdr_rgb.x, recipe.hdr_rgb.y,
                    recipe.hdr_rgb.z, recipe.alpha * envelope};
                command.seed = input.stable_seed;
                result.Push(std::move(item));
                continue;
            }
            const bool teeth=recipe.kind==VfxGroundRecipeKind::BrokenCrown;
            const bool locked=recipe.kind==VfxGroundRecipeKind::ClosedCrownRing;
            if((!teeth&&!locked)||recipe.min_quality>static_cast<std::uint32_t>(input.quality)) continue;
            const float pulse_hz=recipe.primary;
            const float edge=recipe.secondary;
            const float inner_radius=recipe.tertiary;
            const float phase = input.elapsed_seconds * pulse_hz;
            const float size = entity.footprint_radius + edge;
            if ((teeth && inner_radius >= entity.footprint_radius) ||
                !std::isfinite(phase) || !std::isfinite(size)) continue;
            VfxGroundSpawnInput item;
            item.additive = true;
            item.stable_id = input.stable_id;
            item.effect_handle = input.effect_handle;
            item.source_visual_kind = input.source_visual_kind;
            item.motion = {recipe.motion_rate_hz, recipe.motion_amplitude, recipe.motion_inset_fraction};
            item.gradient_row = recipe.gradient_row;
            item.hdr = recipe.hdr;
            item.geometry.shape = recipe.shape;
            item.geometry.outer_radius = entity.footprint_radius;
            item.geometry.inner_radius = inner_radius;
            item.geometry.edge_width = edge;
            item.geometry.progress = progress;
            item.geometry.animation_phase = phase;
            auto &command = item.command;
            command.sequence = input.stable_id ^ current_tick ^
                (static_cast<std::uint64_t>(input.effect_handle) << 32) ^ recipe.output_index;
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
                recipe.hdr_rgb.x, recipe.hdr_rgb.y,
                recipe.hdr_rgb.z, recipe.alpha * envelope};
            if (!Finite({command.start_color.x, command.start_color.y, command.start_color.z}) ||
                !std::isfinite(command.start_color.w)) continue;
            command.seed = input.stable_seed;
            result.Push(std::move(item));
        }
    }
}
void AppendPersistentChevronOutputs(std::span<const VfxGroundRecipe> recipes,
                                    const GroundOwner &owner, GroundCommandOutput &result)
{
    const auto &source = recipes.front();
    if (source.timing_kind != 1 || owner.stable_id == 0 ||
        std::abs(owner.normalized_age - owner.lifetime01) > 0.00001f) return;
    const float progress = (owner.normalized_age - source.knots[0]) /
        (source.knots[1] - source.knots[0]);
    if (!std::isfinite(progress) || progress < 0.0f || progress >= 1.0f) return;
    for (const auto &recipe : recipes)
    {
        if(recipe.kind!=VfxGroundRecipeKind::RepeatingChevron || recipe.min_quality>static_cast<std::uint32_t>(owner.quality)) continue;
        const float spacing=recipe.primary;
        constexpr float edge = 0.05f;
        const float size = std::max(owner.line_half_length, owner.line_half_width) + edge;
        VfxGroundSpawnInput item;
        item.additive = true;
        item.stable_id = owner.stable_id;
        item.effect_handle = owner.effect_handle;
        item.source_visual_kind = owner.source_visual_kind;
        item.motion = {recipe.motion_rate_hz, recipe.motion_amplitude,
                       recipe.motion_inset_fraction};
        item.gradient_row = recipe.gradient_row;
        item.hdr = recipe.hdr;
        auto &geometry = item.geometry;
        geometry.shape = VfxGroundShape::RepeatingChevron;
        geometry.direction = owner.line.direction;
        geometry.half_length = owner.line_half_length;
        geometry.half_width = owner.line_half_width;
        geometry.edge_width = edge;
        geometry.spacing = spacing;
        geometry.spokes = recipe.tick_count;
        geometry.progress = progress;
        auto &command = item.command;
        command.sequence = owner.stable_id ^ owner.command_tick ^
            (static_cast<std::uint64_t>(owner.effect_handle) << 32) ^ recipe.output_index;
        command.tick = owner.command_tick;
        command.position = {(owner.line.start.x + owner.line.end.x) * 0.5f, 0.025f,
                            (owner.line.start.z + owner.line.end.z) * 0.5f};
        command.direction = owner.line.direction;
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
            recipe.hdr_rgb.x, recipe.hdr_rgb.y,
            recipe.hdr_rgb.z, recipe.alpha * owner.alpha};
        if (!Finite({command.start_color.x, command.start_color.y, command.start_color.z}) ||
            !std::isfinite(command.start_color.w)) continue;
        command.seed = owner.stable_seed;
        result.Push(std::move(item));
    }
}
void AppendSafeSector(const VfxImage &image, const VfxEventInput &input,
                      double tick, GroundCommandOutput &result)
{
    const auto &cooked = image.program;
    if (input.effect_handle == 0 || input.effect_handle > cooked.effects.size()) return;
    const auto &effect = cooked.effects[input.effect_handle - 1];
    const auto &sector = std::get<VfxSafeSectorPayload>(input.payload);
    if (effect.handle != input.effect_handle || effect.input_mode != 0 || effect.timing_kind != 1 ||
        effect.payload_kind != hs::vfx_payload::SafeGapSector || input.geometry_owner_id == 0 ||
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
    if (input.effect_handle>image.ground_effect_ranges.size()) return;
    const auto recipe_range=image.ground_effect_ranges[input.effect_handle-1];
    const auto recipes=image.ground_recipes.subspan(recipe_range.first,recipe_range.count);
    for (auto remaining_recipes = recipes; !remaining_recipes.empty();)
    {
        const auto source_recipes = TakeSourceRecipes(remaining_recipes);
        const auto &source_recipe = source_recipes.front();
        float envelope{};
        if (!SourceEnvelope(source_recipe.knot_count, source_recipe.knots, age, envelope)) continue;
        for (const auto &recipe : source_recipes)
        {
            if(recipe.kind!=VfxGroundRecipeKind::SafeSector || recipe.min_quality>static_cast<std::uint32_t>(input.quality)) continue;
            const float phase = recipe.primary * elapsed;
            if (!std::isfinite(phase)) continue;
            VfxGroundSpawnInput item;
            item.stable_id = input.geometry_owner_id; item.effect_handle = input.effect_handle;
            item.gradient_row = recipe.gradient_row; item.hdr = recipe.hdr;
            auto &geometry = item.geometry;
            geometry.shape = VfxGroundShape::SafeSectorMarker;
            geometry.direction = {sector.direction.x / direction_length, 0, sector.direction.z / direction_length};
            geometry.inner_radius = sector.inner_radius; geometry.outer_radius = geometry.range = sector.outer_radius;
            geometry.half_angle_radians = sector.half_angle_degrees * (3.14159265358979323846f / 180);
            geometry.edge_width = .035f; geometry.progress = age; geometry.animation_phase = phase;
            geometry.spokes = recipe.tick_count; geometry.rings = recipe.pulse_curve_interpolation;
            auto &command = item.command;
            command.sequence = input.sequence ^ input.geometry_start_tick ^ (static_cast<std::uint64_t>(effect.handle) << 32) ^ recipe.output_index;
            command.tick = input.geometry_start_tick; command.position = sector.center; command.direction = geometry.direction;
            command.shape = ParticleShape::Point; command.velocity_mode = ParticleVelocity::Direction;
            command.facing = ParticleFacing::Ground; command.renderer = VfxRenderer::Ground; command.primitive = VfxPrimitive::ExactRing;
            command.lifetime_min = command.lifetime_max = duration;
            command.start_size_min = command.start_size_max = command.end_size_min = command.end_size_max = sector.outer_radius + .035f;
            command.start_color = command.end_color = {recipe.hdr_rgb.x, recipe.hdr_rgb.y,
                recipe.hdr_rgb.z, recipe.alpha*envelope};
            if (!Finite({command.start_color.x,command.start_color.y,command.start_color.z}) || !std::isfinite(command.start_size_max)) continue;
            command.seed = input.stable_seed;
            result.Push(std::move(item));
        }
    }
}
void AppendPersistentGroundCommands(
    const VfxImage &image,
    std::span<const VfxPersistentInput> inputs,
    Tick current_tick,
    GroundCommandOutput &result)
{
    const auto &cooked = image.program;
    for (const auto &input : inputs)
    {
        result.BeginInput();
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
            if (input.effect_handle > image.ground_effect_ranges.size()) continue;
            const auto range = image.ground_effect_ranges[input.effect_handle - 1];
            const auto recipes = image.ground_recipes.subspan(range.first, range.count);
            if (recipes.empty() || !(recipes.front().admission_profiles &
                (kVfxGroundEventWaveStart | kVfxGroundEventWaveWarning))) continue;
            const auto &effect = cooked.effects[input.effect_handle - 1];
            const auto source_recipes = TwoSourceRecipes(recipes, effect.sources.first);
            const auto first = result.Size();
            AppendWaveGroundOutputs(image, timer, input.normalized_age,
                                    recipes, source_recipes, result);
            result.MarkWaveTimer(first, input.source_visual_kind);
            continue;
        }
        if (std::holds_alternative<VfxEntityPayload>(input.payload))
        {
            AppendPersistentEntityGroundOutputs(image, input, current_tick, result);
            continue;
        }
        if (input.effect_handle == 0 || input.effect_handle > cooked.effects.size()) continue;
        const auto &effect = cooked.effects[input.effect_handle - 1];
        const auto *circle = std::get_if<VfxCirclePayload>(&input.payload);
        const auto *ring_gaps = std::get_if<VfxRingGapsPayload>(&input.payload);
        const auto *line = std::get_if<VfxLinePayload>(&input.payload);
        if ((!circle && !ring_gaps && !line) || effect.input_mode != 1 ||
            effect.payload_kind != (line ? kLinePayload : (ring_gaps ? kRingGapsPayload : kCirclePayload)))
            continue;
        if ((circle && (!Finite(circle->center) || !std::isfinite(circle->radius) || circle->radius <= 0.0f)) ||
            (line && (!Finite(line->start) || !Finite(line->end) || !std::isfinite(line->width) || line->width <= 0.0f)))
            continue;
        if (ring_gaps && (!ring_gaps->GapStorageConsistent() || !Finite(ring_gaps->center) || !std::isfinite(ring_gaps->inner_radius) ||
            !std::isfinite(ring_gaps->outer_radius) || ring_gaps->inner_radius < 0 || ring_gaps->outer_radius <= ring_gaps->inner_radius ||
            !std::isfinite(ring_gaps->gap_half_width_degrees) || ring_gaps->gap_half_width_degrees < 0 || ring_gaps->gap_half_width_degrees > 180 ||
            std::any_of(ring_gaps->gap_angles_degrees.begin(), ring_gaps->gap_angles_degrees.end(), [](float v) { return !std::isfinite(v); }))) continue;
        if (static_cast<std::uint32_t>(input.quality) > 2) continue;
        if (!std::isfinite(input.normalized_age) || input.normalized_age < 0.0f || input.normalized_age > 1.0f)
            continue;
        const float lifetime = line ? line->lifetime01 : (circle ? circle->lifetime01 : ring_gaps->lifetime01);
        if (!std::isfinite(lifetime) || lifetime < 0.0f || lifetime > 1.0f) continue;
        if (input.effect_handle > image.ground_effect_ranges.size()) continue;
        const auto range = image.ground_effect_ranges[input.effect_handle - 1];
        if (!range.count) continue;

        GroundOwner owner;
        owner.stable_id = input.stable_id;
        owner.effect_handle = effect.handle;
        owner.source_visual_kind = input.source_visual_kind;
        if (circle) owner.circle = *circle;
        if (line)
        {
            const float dx = line->end.x - line->start.x;
            const float dz = line->end.z - line->start.z;
            const float length = std::sqrt(dx * dx + dz * dz);
            if (!std::isfinite(length) || length <= 0.0001f) continue;
            owner.is_line = true;
            owner.line = *line;
            owner.line.direction = {dx / length, 0.0f, dz / length};
            owner.line_half_width = line->width * 0.5f;
            owner.line_half_length = length * 0.5f;
        }
        owner.ring_gaps = ring_gaps;
        owner.normalized_age = ring_gaps ? lifetime : input.normalized_age;
        owner.lifetime01 = lifetime;
        owner.quality = input.quality;
        owner.stable_seed = input.stable_seed;
        owner.command_tick = current_tick;
        owner.command_lifetime = 2.0f / 60.0f;
        auto remaining_recipes = image.ground_recipes.subspan(range.first, range.count);
        while (!remaining_recipes.empty())
        {
            const auto source_recipes = TakeSourceRecipes(remaining_recipes);
            const auto &source = source_recipes.front();
            // Persistent circles use owner timing without a source fade.
            if (!circle && !SourceEnvelope(source.knot_count, source.knots, owner.normalized_age, owner.alpha))
                continue;
            if (line) AppendPersistentChevronOutputs(source_recipes, owner, result);
            AppendSourceOutputs(image, source_recipes, owner, result);
        }
    }
}

void AppendEventGroundCommands(
    const VfxImage &image,
    VfxEventView inputs,
    double current_tick,
    GroundCommandOutput &result,
    std::vector<VfxGroundBirthSuppressionFacts> *birth_facts)
{
    const auto &cooked = image.program;
    for (const auto &input : inputs)
    {
        result.BeginInput();
        VfxGroundBirthSuppressionFacts *event_birth_facts{};
        if (birth_facts)
        {
            birth_facts->push_back({input.sequence});
            event_birth_facts = &birth_facts->back();
        }
        if (std::holds_alternative<VfxSafeSectorPayload>(input.payload))
        {
            if (!event_birth_facts) AppendSafeSector(image, input, current_tick, result);
            continue;
        }
        if (input.effect_handle == 0 || input.effect_handle > cooked.effects.size() ||
            current_tick < input.event_tick || static_cast<std::uint32_t>(input.quality) > 2)
            continue;
        const auto &effect = cooked.effects[input.effect_handle - 1];
        const auto effect_recipe_range = image.ground_effect_ranges[input.effect_handle - 1];
        const auto effect_recipes = image.ground_recipes.subspan(
            effect_recipe_range.first, effect_recipe_range.count);
        if (effect_recipes.empty()) continue;
        const auto &prepared_effect = effect_recipes.front();
        const auto routes = prepared_effect.admission_profiles;
        const auto *point = std::get_if<VfxPointPayload>(&input.payload);
        const auto *ring_gaps = std::get_if<VfxRingGapsPayload>(&input.payload);
        const auto *circle = std::get_if<VfxCirclePayload>(&input.payload);
        const auto *line = std::get_if<VfxLinePayload>(&input.payload);
        const auto *cone = std::get_if<VfxConePayload>(&input.payload);
        const bool context_payload = std::holds_alternative<VfxContextPayload>(input.payload);
        const bool projectile_payload = std::holds_alternative<VfxProjectilePayload>(input.payload);
        const auto payload_kind = point ? kPointPayload : circle ? kCirclePayload :
            line ? kLinePayload : cone ? kConePayload : ring_gaps ? kRingGapsPayload :
            context_payload ? kContextPayload : projectile_payload ? kProjectilePayload : 0u;
        if (!payload_kind || payload_kind != prepared_effect.payload_kind ||
            (!(routes & kVfxGroundEventFixedDuration) &&
             !((routes & kVfxGroundEventOwnerDuration) && circle && input.geometry_owner_id != 0))) continue;
        if ((circle && (!Finite(circle->center) || !std::isfinite(circle->radius) || circle->radius <= 0.0f || circle->inner_radius != 0)) ||
            (point && (!Finite(point->position) || !std::isfinite(point->authored_scale) || point->authored_scale <= 0.0f)) ||
            (line && (!Finite(line->start) || !Finite(line->end) || !std::isfinite(line->width) || line->width <= 0.0f))) continue;
        if (ring_gaps && (!ring_gaps->GapStorageConsistent() || !Finite(ring_gaps->center) ||
            !std::isfinite(ring_gaps->inner_radius) || !std::isfinite(ring_gaps->outer_radius) ||
            ring_gaps->inner_radius < 0 || ring_gaps->outer_radius <= ring_gaps->inner_radius ||
            !std::isfinite(ring_gaps->gap_half_width_degrees) || ring_gaps->gap_half_width_degrees < 0 || ring_gaps->gap_half_width_degrees > 180 ||
            (ring_gaps->GapCount() != 0 && ring_gaps->gap_half_width_degrees == 0) ||
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
            ? static_cast<float>(input.geometry_end_tick - start_tick) / 60.0f : prepared_effect.seconds;
        const float age = static_cast<float>(current_tick - start_tick) / (60.0f * duration);
        if (!std::isfinite(age) || age < 0.0f || age >= 1.0f)
            continue;
        if (context_payload && effect.sources.count == 2 &&
            (routes & (kVfxGroundEventChargedPulse | kVfxGroundEventWaveStart | kVfxGroundEventWaveWarning)))
        {
            const auto context_sources = TwoSourceRecipes(effect_recipes, effect.sources.first);
            if (routes & kVfxGroundEventChargedPulse)
                AppendChargedPulseGroundOutputs(input, age, context_sources, result, event_birth_facts);
            else
                AppendWaveGroundOutputs(image, input, age, effect_recipes, context_sources, result, event_birth_facts);
        }
        auto enabled = GroundExpandingRing | GroundDirectShape;
        if (!event_birth_facts) enabled |= GroundHexConstellation | GroundBossSignature | GroundContactShape;
        if (ring_gaps && !input.geometry_owner_id) enabled &= ~GroundDirectShape;
        auto remaining_recipes = effect_recipes;
        while (!remaining_recipes.empty())
        {
            const auto source_recipes = TakeSourceRecipes(remaining_recipes);
            const auto &source_recipe = source_recipes.front();
            const auto outputs = source_recipe.source_event_outputs & enabled;
            if (!outputs) continue;
            float envelope = 1.0f;
            if (!SourceEnvelope(source_recipe.knot_count, source_recipe.knots, age, envelope)) continue;
            if (outputs & GroundExpandingRing)
                AppendExpandingRingOutputs(input, age, envelope, duration, source_recipes, result, event_birth_facts);
            if (outputs & GroundHexConstellation)
                AppendHexConstellationOutputs(image, input, age, envelope, duration, source_recipes, result);
            if (outputs & GroundBossSignature)
                AppendBossSignatureOutputs(input, age, envelope, duration, source_recipes, result);
            if (outputs & GroundContactShape)
                AppendRemainingEventGroundOutputs(input, age, envelope, duration, source_recipes, result);
            if (!(outputs & GroundDirectShape)) continue;
            GroundOwner owner;
            owner.stable_id = input.geometry_owner_id != 0 ? input.geometry_owner_id : static_cast<std::uint64_t>(input.sequence);
            owner.effect_handle = effect.handle;
            if (circle) { owner.circle = *circle; owner.is_circle_preview = true; }
            owner.ring_gaps = ring_gaps;
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
            AppendSourceOutputs(image, source_recipes, owner, result, event_birth_facts);
        }
    }
}

} // namespace

VfxGroundEventAdmission ClassifyVfxTypedEventGroundAdmission(
    const VfxImage &image, const VfxEventInput &input) noexcept
{
    const bool point=std::holds_alternative<VfxPointPayload>(input.payload);
    const bool context=std::holds_alternative<VfxContextPayload>(input.payload);
    const bool projectile=std::holds_alternative<VfxProjectilePayload>(input.payload);
    if(!point&&!context&&!projectile&&
       !std::holds_alternative<VfxCirclePayload>(input.payload)&&
       !std::holds_alternative<VfxLinePayload>(input.payload)&&
       !std::holds_alternative<VfxConePayload>(input.payload)&&
       !std::holds_alternative<VfxRingGapsPayload>(input.payload)&&
       !std::holds_alternative<VfxSafeSectorPayload>(input.payload))return {};
    if(!input.effect_handle||input.effect_handle>image.ground_effect_ranges.size())return {};
    const auto range=image.ground_effect_ranges[input.effect_handle-1];
    if(!range.count) return {};
    const auto &first=image.ground_recipes[range.first];
    const bool charged_pulse=context&&(first.admission_profiles&kVfxGroundEventChargedPulse);
    const bool wave_start=context&&(first.admission_profiles&kVfxGroundEventWaveStart);
    const bool owned=input.geometry_owner_id!=0&&input.geometry_end_tick>input.geometry_start_tick;
    if(!(first.admission_profiles&kVfxGroundEventFixedDuration)&&
       !((first.admission_profiles&kVfxGroundEventOwnerDuration)&&owned))return {};
    if((point||context||projectile)&&
       !(first.admission_profiles&kVfxGroundEventDirectAdd)&&
       !((charged_pulse||wave_start)&&(first.admission_profiles&kVfxGroundEventDirectSoft)))return {};
    return {true,charged_pulse};
}

void AppendVfxTypedGroundCommands(const VfxImage &image,
    std::span<const VfxPersistentInput> inputs, Tick current_tick,
    std::vector<VfxGroundSpawnInput> &result, std::vector<float> &frame_gap_angles)
{
    GroundCommandOutput output(result, frame_gap_angles);
    AppendPersistentGroundCommands(image, inputs, current_tick, output);
}

void AppendVfxTypedEventGroundCommands(const VfxImage &image,
    VfxEventView inputs, double current_tick,
    std::vector<VfxGroundSpawnInput> &result, std::vector<float> &frame_gap_angles)
{
    GroundCommandOutput output(result, frame_gap_angles);
    AppendEventGroundCommands(image, inputs, current_tick, output, nullptr);
}

void AppendVfxTypedEventGroundBirthSuppression(
    const VfxImage &image, VfxEventView inputs,
    std::vector<VfxGroundBirthSuppressionFacts> &result)
{
    std::vector<VfxGroundSpawnInput> discard;
    std::vector<float> gap_scratch;
    GroundCommandOutput output(discard, gap_scratch);
    const auto first = result.size();
    if (inputs.size() > result.max_size() - first)
        throw std::length_error("too many VFX ground birth facts");
    result.reserve(first + inputs.size());
    for (const auto &input : inputs)
        AppendEventGroundCommands(image, std::span(&input, 1),
                                  static_cast<double>(input.event_tick), output, &result);
}

} // namespace hs::runtime_detail
