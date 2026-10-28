#include "vfx_typed_ribbon_commands.hpp"

#include <hs/render_data/render_snapshot.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <optional>
#include <numbers>
#include <string_view>
#include <utility>

namespace hs::runtime_detail
{
namespace
{
constexpr std::uint32_t Hash32(std::string_view value) noexcept
{
    std::uint32_t hash = 2166136261u;
    for (const auto character : value)
    {
        hash ^= static_cast<unsigned char>(character);
        hash *= 16777619u;
    }
    return hash;
}

constexpr std::uint64_t Hash64(std::string_view value) noexcept
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto character : value)
    {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ull;
    }
    return hash;
}

constexpr auto kHistoryRibbon = VfxSourceType::HistoryRibbon;
constexpr auto kRibbonAdd = VfxOutputProfile::RibbonAdd;
constexpr auto kRibbonOit = VfxOutputProfile::RibbonOit;
constexpr std::uint32_t kProjectilePayload = Hash32("ProjectilePayload");
constexpr std::uint32_t kProjectilePathPayload = Hash32("ProjectilePathPayload");
constexpr std::uint64_t kChargedImpactEffect = Hash64("particle.skill.charged_shot.impact");
constexpr std::uint32_t kLinkCoreSource = Hash32("link_core");
constexpr std::uint32_t kWakeRails = Hash32("wake_rails");

bool Finite(const Float3 &value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

const VfxRibbonEffectRecipe *FindEffect(const VfxImage &image,
                                  VfxEffectHandle handle) noexcept
{
    if (handle == 0 || handle > image.ribbon_effect_recipes.size()) return nullptr;
    const auto &effect = image.ribbon_effect_recipes[handle - 1];
    return effect.valid_handle && effect.handle == handle ? &effect : nullptr;
}

const VfxRibbonLinkEffectRecipe *FindLinkEffect(const VfxImage &image,
                                                 VfxEffectHandle handle) noexcept
{
    if (handle == 0 || handle > image.ribbon_link_effect_recipes.size()) return nullptr;
    const auto &recipe = image.ribbon_link_effect_recipes[handle - 1];
    return recipe.effect_handle == handle && recipe.kind != VfxRibbonLinkKind::Unsupported
        ? &recipe : nullptr;
}

float KnotAlpha(const std::array<float,4> &knots, std::uint32_t knot_count,
                float normalized_age) noexcept
{
    if (knot_count == 0) return 1.0f;
    const auto age = std::clamp(normalized_age, 0.0f, 1.0f);
    if (knot_count == 2)
    {
        if (knots[0] <= 0.0f) return 1.0f;
        if (age <= knots[0]) return 0.0f;
        return std::clamp((age - knots[0]) /
                              std::max(0.0001f, knots[1] - knots[0]),
                          0.0f, 1.0f);
    }
    if (age <= knots[0]) return 0.0f;
    if (age < knots[1])
        return (age - knots[0]) /
               std::max(0.0001f, knots[1] - knots[0]);
    if (age < knots[2]) return 1.0f;
    if (age < knots[3])
        return 1.0f - (age - knots[2]) /
            std::max(0.0001f, knots[3] - knots[2]);
    return 0.0f;
}

float KnotAlpha(const VfxRibbonSourceRecipe &source, float normalized_age) noexcept
{
    return KnotAlpha(source.knots, source.knot_count, normalized_age);
}

bool IsRibbonDetailBinding(const VfxRibbonTextureRecipe &binding) noexcept
{
    return binding.valid_path && binding.asset == VfxRibbonTextureAsset::DetailArray;
}

Float3 TransformOrigin(const VfxTransform &transform) noexcept
{
    return {transform[12], transform[13], transform[14]};
}

Float3 NormalizeHorizontal(Float3 value) noexcept
{
    value.y = 0.0f;
    const auto length = std::sqrt(value.x * value.x + value.z * value.z);
    return length > 0.0001f ? Float3{value.x / length, 0.0f, value.z / length}
                            : Float3{0.0f, 0.0f, 1.0f};
}

Float3 Add3(Float3 a, Float3 b) noexcept
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Float3 Scale3(Float3 value, float scale) noexcept
{
    return {value.x * scale, value.y * scale, value.z * scale};
}

struct EventEmitter
{
    Float3 origin{};
    Float3 direction{};
    float scale{1.0f};
    float speed{};
    bool projectile{};
};

std::optional<EventEmitter> Emitter(const VfxEventInput &input) noexcept
{
    if (const auto *point = std::get_if<VfxPointPayload>(&input.payload))
        return EventEmitter{point->position, point->direction, point->authored_scale};
    if (const auto *context = std::get_if<VfxContextPayload>(&input.payload))
        return EventEmitter{TransformOrigin(input.world_transform), context->direction,
                            1.0f};
    if (const auto *cone = std::get_if<VfxConePayload>(&input.payload))
        return EventEmitter{cone->origin, cone->direction, 1.0f};
    if (const auto *projectile = std::get_if<VfxProjectilePayload>(&input.payload))
    {
        const auto origin = TransformOrigin(input.world_transform);
        const auto speed = std::hypot(projectile->velocity.x,
                                      projectile->velocity.y,
                                      projectile->velocity.z);
        if (!Finite(origin) || !Finite(projectile->velocity) ||
            !std::isfinite(speed) || speed <= 0.0001f ||
            !std::isfinite(projectile->hitbox_radius) ||
            projectile->hitbox_radius <= 0.0f)
            return std::nullopt;
        return EventEmitter{origin,
                            Scale3(projectile->velocity, 1.0f / speed),
                            projectile->hitbox_radius, speed, true};
    }
    return std::nullopt;
}

std::uint32_t PayloadKind(const VfxEventInput &input) noexcept
{
    if (std::holds_alternative<VfxPointPayload>(input.payload))
        return Hash32("PointEventPayload");
    if (std::holds_alternative<VfxContextPayload>(input.payload))
        return Hash32("PresentationContextPayload");
    if (std::holds_alternative<VfxConePayload>(input.payload))
        return Hash32("ConePayload");
    if (std::holds_alternative<VfxProjectilePayload>(input.payload))
        return kProjectilePayload;
    return 0;
}

template<class T> bool InRange(VfxRange r,std::span<const T> v)
{return r.first<=v.size()&&r.count<=v.size()-r.first;}

void ResetRibbonSource(VfxRibbonSourceInput &ribbon)
{
    ribbon.owner_id = 0;
    ribbon.effect_handle = {};
    ribbon.source_id = 0;
    ribbon.stable_seed = 0;
    ribbon.importance = 0;
    ribbon.position = {};
    ribbon.previous_position = {};
    ribbon.history_seconds = 0.0f;
    ribbon.start_alpha = 1.0f;
    // Keep the vector object alive so checked-iterator builds reuse its proxy and capacity.
    ribbon.outputs.clear();
    ribbon.analytic = false;
    ribbon.control = {};
    ribbon.segments = 16;
    ribbon.normalized_age = 0.0f;
}

bool BossDashWakeRibbon(
    const VfxImage &cooked, const VfxPersistentInput &input,
    std::span<const VfxRibbonWakeRecipe> recipes, VfxRibbonSourceInput &ribbon)
{
    const auto *line = std::get_if<VfxLinePayload>(&input.payload);
    if (!line || input.stable_id == 0 ||
        static_cast<std::uint32_t>(input.quality) > 2 ||
        !Finite(line->start) || !Finite(line->end) || !Finite(line->direction) ||
        !std::isfinite(line->width) || line->width <= 0.0f ||
        !std::isfinite(line->lifetime01) || line->lifetime01 < 0.0f ||
        line->lifetime01 >= 1.0f || !std::isfinite(input.normalized_age) ||
        input.normalized_age != line->lifetime01 || recipes.empty())
        return false;
    const auto length = std::hypot(line->end.x - line->start.x,
                                   line->end.z - line->start.z);
    if (!std::isfinite(length) || length <= 0.0001f) return false;

    for (const auto &recipe : recipes)
    {
        if (recipe.effect_handle != input.effect_handle ||
            static_cast<std::uint32_t>(input.quality) < recipe.min_quality ||
            static_cast<std::uint32_t>(input.quality) < recipe.detail_min_quality ||
            !InRange(recipe.detail_slices, cooked.ribbon_slice_indices) ||
            recipe.detail_slices.count != 2)
            continue;
        const auto slices = cooked.ribbon_slice_indices.subspan(
            recipe.detail_slices.first, recipe.detail_slices.count);
        if (std::ranges::any_of(slices, [](std::uint32_t slice) { return slice >= 8u; })) continue;

        if (!ribbon.outputs.empty()) return false;
        ribbon.owner_id = input.stable_id;
        ribbon.effect_handle = recipe.effect_handle;
        ribbon.source_id = kWakeRails;
        ribbon.stable_seed = input.stable_seed;
        ribbon.importance = recipe.importance;
        ribbon.previous_position = {line->start.x, 0.025f, line->start.z};
        ribbon.position = {line->end.x, 0.025f, line->end.z};
        ribbon.control.x = line->width; // lane width for the paired ground rails
        ribbon.history_seconds = .24f;
        ribbon.start_alpha = 1.0f;
        VfxRibbonOutputInput translated;
        translated.profile = kRibbonAdd;
        translated.motion = VfxMotionKind::HistoryRibbonTearsBackward;
        translated.color = {recipe.color.x, recipe.color.y, recipe.color.z,
                            recipe.color.w};
        translated.width_head = recipe.width_head;
        translated.width_tail = recipe.width_tail;
        translated.hdr = recipe.hdr;
        translated.gradient_row = recipe.gradient_row;
        translated.detail_slice = slices[input.stable_seed % slices.size()];
        translated.detail_strength = recipe.detail_strength;
        ribbon.outputs.push_back(translated);
        // A stable owner can publish only one wake source.
    }
    return !ribbon.outputs.empty();
}

bool PickupCollectRibbon(
    const VfxImage &cooked, const VfxEventInput &event,
    const VfxRibbonLinkEffectRecipe &recipe, const VfxLinkPayload &link, double current_tick,
    VfxRibbonSourceInput &ribbon)
{
    if (recipe.kind != VfxRibbonLinkKind::PickupCollect || event.sequence == 0 ||
        current_tick < event.event_tick ||
        static_cast<std::uint32_t>(event.quality) > 2 ||
        !Finite(link.source_position) || !Finite(link.target_position) ||
        !InRange(recipe.outputs, cooked.ribbon_link_output_recipes))
        return false;
    const float distance = std::hypot(
        link.target_position.x - link.source_position.x,
        link.target_position.y - link.source_position.y,
        link.target_position.z - link.source_position.z);
    if (!std::isfinite(distance) || distance <= 0.0001f) return false;
    const float elapsed = static_cast<float>(current_tick - event.event_tick) / 60.0f;
    const float start = recipe.window_start * recipe.seconds;
    const float end = recipe.window_end * recipe.seconds;
    if (!std::isfinite(elapsed) || !std::isfinite(start) || !std::isfinite(end) ||
        end <= start || elapsed < start || elapsed >= end || recipe.outputs.count != 1)
        return false;
    const auto &prepared = cooked.ribbon_link_output_recipes[recipe.outputs.first];
    if (prepared.min_quality > static_cast<std::uint32_t>(event.quality)) return false;
    const auto &detail = prepared.details[static_cast<std::uint32_t>(event.quality)];
    if (!detail.valid) return false;
    auto translated = prepared.output;
    if (detail.present)
    {
        if (!InRange(detail.slices, cooked.ribbon_slice_indices) || detail.slices.count == 0) return false;
        translated.detail_slice = cooked.ribbon_slice_indices[
            detail.slices.first + event.stable_seed % detail.slices.count];
        if (translated.detail_slice >= 8) return false;
        translated.detail_strength = detail.strength;
    }
    ribbon.owner_id = event.sequence;
    ribbon.effect_handle = event.effect_handle;
    ribbon.source_id = prepared.source_id;
    ribbon.stable_seed = event.stable_seed;
    ribbon.importance = recipe.importance;
    ribbon.analytic = true;
    ribbon.previous_position = link.source_position;
    ribbon.position = link.target_position;
    ribbon.control = Add3(Scale3(link.source_position, 0.5f),
                          Scale3(link.target_position, 0.5f));
    ribbon.control.y += std::min(0.35f, distance * 0.12f);
    ribbon.history_seconds = end - start;
    ribbon.normalized_age = (elapsed - start) / (end - start);
    ribbon.segments = recipe.segments;
    ribbon.outputs.push_back(translated);
    return true;
}

bool LinkRibbon(const VfxImage &c,
                                               const VfxPersistentInput &input,
                                               const VfxRibbonLinkEffectRecipe &recipe,
                                               VfxRibbonSourceInput &ribbon,
                                               bool allow_arrow_rain = false)
{
    const bool basic_return = recipe.kind == VfxRibbonLinkKind::CapturedPath;
    const bool retreat_dash = recipe.kind == VfxRibbonLinkKind::CapturedLine;
    const bool retreat_afterimage = recipe.kind == VfxRibbonLinkKind::CapturedContext;
    const bool pickup_motion = recipe.kind == VfxRibbonLinkKind::PickupMotion &&
        input.source_visual_kind == static_cast<std::uint8_t>(PersistentVfxKind::PickupXpMagnetTrail);
    VfxLinkPayload captured{};
    const auto *link=std::get_if<VfxLinkPayload>(&input.payload);
    if (basic_return)
    {
        const auto *path = std::get_if<VfxProjectilePathPayload>(&input.payload);
        if (!path ||
            !Finite(path->path_origin_position) || !Finite(path->current_position) ||
            !std::isfinite(path->projectile_radius) || path->projectile_radius <= 0)
            return false;
        captured.source_position = path->path_origin_position;
        captured.target_position = path->current_position;
        captured.width = path->projectile_radius * 2.0f;
        link = &captured;
    }
    else if (retreat_dash)
    {
        const auto *line = std::get_if<VfxLinePayload>(&input.payload);
        if (!line) return false;
        captured.source_position = line->start;
        captured.target_position = line->end;
        captured.width = line->width;
        link = &captured;
    }
    else if (retreat_afterimage)
    {
        const auto *context = std::get_if<VfxContextPayload>(&input.payload);
        if (!context ||
            context->owner_id == 0 || !Finite(context->direction) ||
            !std::isfinite(context->scalar0) || context->scalar0 <= 0 ||
            !std::isfinite(context->scalar1) || context->scalar1 <= 0)
            return false;
        captured.source_position = TransformOrigin(input.current_transform);
        captured.target_position = Add3(captured.source_position,
                                        Scale3(context->direction, context->scalar0));
        captured.width = context->scalar1;
        link = &captured;
    }
    if(!link || recipe.kind == VfxRibbonLinkKind::Unsupported ||
        recipe.kind == VfxRibbonLinkKind::PickupCollect ||
        (recipe.kind == VfxRibbonLinkKind::ArrowRain && !allow_arrow_rain) ||
        (recipe.kind == VfxRibbonLinkKind::PickupMotion && !pickup_motion) ||
        input.stable_id==0||static_cast<std::uint32_t>(input.quality)>2||!Finite(link->source_position)||!Finite(link->target_position)||
        !std::isfinite(link->width)||link->width<=0||!std::isfinite(input.normalized_age)||input.normalized_age<0||input.normalized_age>=1||
        !std::isfinite(input.elapsed_seconds)||input.elapsed_seconds<0||
        !InRange(recipe.outputs,c.ribbon_link_output_recipes))return false;
    const float length=std::hypot(link->target_position.x-link->source_position.x,
        link->target_position.y-link->source_position.y,link->target_position.z-link->source_position.z);
    if(!std::isfinite(length)||length<=.0001f)return false;
    ribbon.owner_id=input.stable_id;ribbon.effect_handle=recipe.effect_handle;ribbon.source_id=kLinkCoreSource;
    ribbon.stable_seed=input.stable_seed;ribbon.importance=recipe.importance;ribbon.analytic=true;
    ribbon.position=link->target_position;ribbon.previous_position=link->source_position;
    ribbon.control=Add3(Scale3(link->source_position,.5f),Scale3(link->target_position,.5f));
    ribbon.control.y+=std::min(.35f,length*.12f);ribbon.history_seconds=1.0f/60;
    ribbon.normalized_age=input.normalized_age;
    ribbon.start_alpha=pickup_motion ? 1.0f : std::clamp((1-input.normalized_age)/.2f,0.0f,1.0f);
    ribbon.segments=recipe.segments;
    for (const auto &prepared : std::span(c.ribbon_link_output_recipes).subspan(
             recipe.outputs.first, recipe.outputs.count))
    {
        if (prepared.min_quality > static_cast<std::uint32_t>(input.quality)) continue;
        const auto &detail = prepared.details[static_cast<std::uint32_t>(input.quality)];
        if (!detail.valid) continue;
        auto out = prepared.output;
        float width = link->width;
        if (!prepared.core) width *= prepared.width_multiplier;
        out.width_head = width;
        out.width_tail = width * recipe.taper;
        if (!std::isfinite(width) || width <= 0.0f ||
            !std::isfinite(out.width_tail) || out.width_tail < 0.0f) continue;
        if (detail.present)
        {
            if (!InRange(detail.slices,c.ribbon_slice_indices) || detail.slices.count == 0) continue;
            out.detail_slice=c.ribbon_slice_indices[detail.slices.first+
                input.stable_seed%detail.slices.count];
            if(out.detail_slice>=8)continue;
            out.detail_strength=detail.strength;
        }
        ribbon.outputs.push_back(out);
    }
    return !ribbon.outputs.empty();
}

struct ProjectileGeometry
{
    Float3 current{};
    Float3 previous{};
    Float3 velocity{};
    float radius{};
};

std::optional<ProjectileGeometry> Geometry(const VfxPersistentInput &input) noexcept
{
    if (const auto *path = std::get_if<VfxProjectilePathPayload>(&input.payload))
    {
        if (!Finite(path->current_position) || !Finite(path->previous_position) ||
            !Finite(path->velocity) || !std::isfinite(path->projectile_radius) ||
            path->projectile_radius <= 0.0f)
            return std::nullopt;
        return ProjectileGeometry{path->current_position, path->previous_position,
                                  path->velocity, path->projectile_radius};
    }
    if (const auto *head = std::get_if<VfxProjectilePayload>(&input.payload))
    {
        const auto &transform = input.current_transform;
        const auto &previous = input.previous_transform;
        const Float3 current{transform[12], transform[13], transform[14]};
        const Float3 prior{previous[12], previous[13], previous[14]};
        if (!Finite(current) || !Finite(prior) || !Finite(head->velocity) ||
            !std::isfinite(head->hitbox_radius) || head->hitbox_radius <= 0.0f)
            return std::nullopt;
        return ProjectileGeometry{current, prior, head->velocity,
                                  head->hitbox_radius};
    }
    return std::nullopt;
}

struct FixedRibbonGeometry
{
    Float3 center{};
    Float3 direction{};
    float reach{};
    float direction_sign{1.0f};
};

std::optional<FixedRibbonGeometry> FixedEventGeometry(
    const VfxEventInput &event, VfxRibbonFixedKind kind) noexcept
{
    if (kind == VfxRibbonFixedKind::ExplosivePrePull)
    {
        const auto *circle = std::get_if<VfxCirclePayload>(&event.payload);
        if (!circle || !Finite(circle->center) || !Finite(circle->direction) ||
            !std::isfinite(circle->radius) || circle->radius <= 0.0f ||
            !std::isfinite(circle->inner_radius) || circle->inner_radius != 0.0f ||
            !std::isfinite(circle->lifetime01) || circle->lifetime01 < 0.0f ||
            circle->lifetime01 > 1.0f)
            return std::nullopt;
        return FixedRibbonGeometry{circle->center, NormalizeHorizontal(circle->direction),
                                   circle->radius, 1.0f};
    }

    if (kind != VfxRibbonFixedKind::CommonPull && kind != VfxRibbonFixedKind::RetreatMove)
        return std::nullopt;
    const auto *point = std::get_if<VfxPointPayload>(&event.payload);
    if (!point || !Finite(point->position) || !Finite(point->normal) ||
        !Finite(point->direction) || !std::isfinite(point->authored_scale) ||
        point->authored_scale <= 0.0f || !std::isfinite(point->scalar0) ||
        !std::isfinite(point->scalar1))
        return std::nullopt;

    // These event-stream payloads do not contain a gameplay target. The link
    // is deliberately a cosmetic span into the event center. Pull uses the
    // supplied direction as its outside point; retreat leaves a wake behind
    // the supplied movement direction.
    const auto sign = kind == VfxRibbonFixedKind::CommonPull ? 1.0f : -1.0f;
    return FixedRibbonGeometry{point->position, NormalizeHorizontal(point->direction),
                               point->authored_scale, sign};
}

bool FixedEventRibbon(
    const VfxEventInput &event,
    const VfxRibbonFixedRecipe &recipe,
    double current_tick,
    VfxRibbonSourceInput &ribbon)
{
    if (recipe.effect_handle != event.effect_handle || event.sequence == 0 ||
        static_cast<std::uint32_t>(event.quality) > 2 || current_tick < event.event_tick)
        return false;
    const auto geometry = FixedEventGeometry(event, recipe.kind);
    if (!geometry) return false;
    const auto elapsed = static_cast<float>(current_tick - event.event_tick) / 60.0f;
    if (!std::isfinite(elapsed) || elapsed < 0.0f || elapsed >= recipe.seconds)
        return false;
    const float base_width = 0.035f * geometry->reach;
    auto core_output = recipe.core_output;
    auto outer_output = recipe.outer_output;
    core_output.width_head = base_width;
    core_output.width_tail = base_width * recipe.taper;
    outer_output.width_head = base_width * recipe.outer_width_multiplier;
    outer_output.width_tail = outer_output.width_head * recipe.taper;
    if (!std::isfinite(base_width) || base_width <= 0.0f ||
        !std::isfinite(core_output.width_tail) || core_output.width_tail < 0.0f ||
        !std::isfinite(outer_output.width_head) || outer_output.width_head <= 0.0f ||
        !std::isfinite(outer_output.width_tail) || outer_output.width_tail < 0.0f)
        return false;

    ribbon.owner_id = event.sequence;
    ribbon.effect_handle = event.effect_handle;
    ribbon.source_id = kLinkCoreSource;
    ribbon.stable_seed = event.stable_seed;
    ribbon.importance = recipe.importance;
    ribbon.analytic = true;
    const auto source_offset = Scale3(geometry->direction,
                                      geometry->reach * geometry->direction_sign);
    ribbon.previous_position = Add3(geometry->center, source_offset);
    ribbon.position = geometry->center;
    const auto distance = geometry->reach;
    ribbon.control = Add3(Scale3(ribbon.previous_position, 0.5f),
                          Scale3(ribbon.position, 0.5f));
    ribbon.control.y += std::min(0.35f, distance * 0.12f);
    ribbon.history_seconds = 1.0f / 60.0f;
    ribbon.normalized_age = elapsed / recipe.seconds;
    ribbon.start_alpha = std::clamp((1.0f - ribbon.normalized_age) / 0.2f, 0.0f, 1.0f);
    ribbon.segments = recipe.segments;
    if (recipe.core_min_quality <= static_cast<std::uint32_t>(event.quality))
        ribbon.outputs.push_back(core_output);
    if (recipe.outer_min_quality <= static_cast<std::uint32_t>(event.quality))
        ribbon.outputs.push_back(outer_output);
    return !ribbon.outputs.empty();
}
}

VfxRibbonInputScratch::VfxRibbonInputScratch(VfxRibbonInputScratch &&other) noexcept
    : rows_(std::move(other.rows_)), active_count_(std::exchange(other.active_count_, 0))
{
}

VfxRibbonInputScratch &VfxRibbonInputScratch::operator=(VfxRibbonInputScratch &&other) noexcept
{
    if (this != &other)
    {
        rows_ = std::move(other.rows_);
        active_count_ = std::exchange(other.active_count_, 0);
    }
    return *this;
}

VfxRibbonSourceInput &VfxRibbonInputScratch::PrepareAppendRow()
{
    if (active_count_ == rows_.size()) rows_.emplace_back();
    auto &row = rows_[active_count_];
    ResetRibbonSource(row);
    return row;
}

void VfxRibbonInputScratch::RejectAppendRow() noexcept
{
    if (active_count_ < rows_.size()) rows_[active_count_].outputs.clear();
}

void VfxRibbonInputScratch::Rollback(std::size_t active_count) noexcept
{
    for (auto index = active_count; index < rows_.size(); ++index)
        rows_[index].outputs.clear();
    active_count_ = active_count;
}

std::vector<VfxRibbonSourceInput> VfxRibbonInputScratch::TakeSources() &&
{
    rows_.resize(active_count_);
    active_count_ = 0;
    return std::move(rows_);
}

void AppendVfxTypedRibbonInputs(
    const VfxImage &cooked,
    std::span<const VfxPersistentInput> persistent,
    VfxRibbonInputScratch &scratch)
{
    const auto original_size = scratch.Sources().size();
    try
    {
        for (const auto &input : persistent)
        {
            if (std::holds_alternative<VfxLinePayload>(input.payload))
            {
                if (input.effect_handle != 0 &&
                    input.effect_handle <= cooked.ribbon_wake_effect_ranges.size())
                {
                    const auto range = cooked.ribbon_wake_effect_ranges[input.effect_handle - 1];
                    if (InRange(range, cooked.ribbon_wake_recipes))
                    {
                        auto &row = scratch.PrepareAppendRow();
                        if (BossDashWakeRibbon(cooked, input,
                            cooked.ribbon_wake_recipes.subspan(range.first, range.count), row))
                            scratch.CommitAppendRow();
                        else scratch.RejectAppendRow();
                    }
                }
                const auto *link_recipe = FindLinkEffect(cooked, input.effect_handle);
                if (link_recipe && link_recipe->kind == VfxRibbonLinkKind::CapturedLine)
                {
                    auto &row = scratch.PrepareAppendRow();
                    if (LinkRibbon(cooked, input, *link_recipe, row)) scratch.CommitAppendRow();
                    else scratch.RejectAppendRow();
                }
                continue;
            }
            const auto *link_recipe = FindLinkEffect(cooked, input.effect_handle);
            if (std::holds_alternative<VfxContextPayload>(input.payload) ||
                (std::holds_alternative<VfxProjectilePathPayload>(input.payload) &&
                 input.source_visual_kind == static_cast<std::uint8_t>(PersistentVfxKind::BasicArrowReturn)))
            {
                if (link_recipe)
                {
                    auto &row = scratch.PrepareAppendRow();
                    if (LinkRibbon(cooked, input, *link_recipe, row)) scratch.CommitAppendRow();
                    else scratch.RejectAppendRow();
                }
                continue;
            }
            if(std::holds_alternative<VfxLinkPayload>(input.payload))
            {
                if(link_recipe)
                {
                    auto &row=scratch.PrepareAppendRow();
                    if(LinkRibbon(cooked,input,*link_recipe,row))scratch.CommitAppendRow();
                    else scratch.RejectAppendRow();
                }
                continue;
            }
            const auto *effect = FindEffect(cooked, input.effect_handle);
            if (!effect) continue;
            const auto geometry = Geometry(input);
            if (!geometry) continue;
            const auto payload_kind = std::holds_alternative<VfxProjectilePayload>(input.payload)
                ? kProjectilePayload : kProjectilePathPayload;
            if (effect->payload_kind != payload_kind) continue;
            if (input.effect_handle > cooked.ribbon_projectile_effect_ranges.size()) continue;
            const auto effect_range = cooked.ribbon_projectile_effect_ranges[input.effect_handle - 1];
            if (!InRange(effect_range, cooked.ribbon_projectile_sources)) continue;
            for (const auto &source : std::span(cooked.ribbon_projectile_sources).subspan(
                     effect_range.first, effect_range.count))
            {
                const auto history_seconds = source.history_seconds;
                if (!InRange(source.outputs, cooked.ribbon_projectile_outputs)) continue;

                auto &ribbon = scratch.PrepareAppendRow();
                ribbon.owner_id = input.stable_id;
                ribbon.effect_handle = input.effect_handle;
                ribbon.source_id = source.source_id;
                ribbon.stable_seed = input.stable_seed;
                ribbon.importance = effect->importance;
                ribbon.position = geometry->current;
                ribbon.previous_position = geometry->previous;
                ribbon.history_seconds = history_seconds;
                // Projectile presence has no predicted expiry. Use authored
                // seconds only to fade in a live head; if none exists, the
                // history window is the bounded fade-in interval and the ribbon
                // remains present while the owner record exists.
                const auto fade_seconds = effect->seconds > 0.0f
                    ? effect->seconds : history_seconds;
                const auto live_alpha = fade_seconds > 0.0f
                    ? std::clamp(input.elapsed_seconds / fade_seconds, 0.0f, 1.0f)
                    : 1.0f;
                ribbon.start_alpha = input.normalized_age > 0.0f
                    ? KnotAlpha(source.knots, source.knot_count, input.normalized_age)
                    : source.knot_count == 4
                        ? std::clamp((live_alpha - source.knots[0]) /
                            std::max(source.knots[1] - source.knots[0], 0.0001f), 0.0f, 1.0f)
                        : KnotAlpha(source.knots, source.knot_count, live_alpha);

                for (const auto &prepared_output : std::span(cooked.ribbon_projectile_outputs).subspan(
                         source.outputs.first, source.outputs.count))
                {
                    if (static_cast<std::uint32_t>(input.quality) < prepared_output.min_quality)
                        continue;
                    auto translated = prepared_output.output;
                    if (prepared_output.detail_present)
                    {
                        if (!InRange(prepared_output.detail_slices, cooked.ribbon_slice_indices)) continue;
                        translated.detail_strength = prepared_output.detail_strength;
                        translated.detail_slice = cooked.ribbon_slice_indices[
                            prepared_output.detail_slices.first +
                            (input.stable_seed % prepared_output.detail_slices.count)];
                        if (translated.detail_slice >= 8) continue;
                    }
                    ribbon.outputs.push_back(translated);
                }
                if (!ribbon.outputs.empty()) scratch.CommitAppendRow();
                else scratch.RejectAppendRow();
            }
        }
        }
    catch (...)
    {
        scratch.Rollback(original_size);
        throw;
    }
}

void AppendVfxTypedRibbonInputs(
    const VfxImage &cooked,
    std::span<const VfxPersistentInput> persistent,
    std::vector<VfxRibbonSourceInput> &result)
{
    const auto original_size = result.size();
    try
    {
        VfxRibbonInputScratch scratch;
        AppendVfxTypedRibbonInputs(cooked, persistent, scratch);
        auto rows = std::move(scratch).TakeSources();
        result.insert(result.end(), std::make_move_iterator(rows.begin()),
                      std::make_move_iterator(rows.end()));
    }
    catch (...)
    {
        result.resize(original_size);
        throw;
    }
}

std::vector<VfxRibbonSourceInput> BuildVfxTypedRibbonInputs(
    const VfxImage &cooked,
    std::span<const VfxPersistentInput> persistent)
{
    VfxRibbonInputScratch scratch;
    AppendVfxTypedRibbonInputs(cooked, persistent, scratch);
    return std::move(scratch).TakeSources();
}

void AppendVfxEventRibbonInputs(
    const VfxImage &cooked,
    VfxEventView events,
    double current_tick,
    VfxRibbonInputScratch &scratch)
{
    const auto original_size = scratch.Sources().size();
    try
    {
        for (const auto &event : events)
        {
            if (const auto *entity = std::get_if<VfxEntityPayload>(&event.payload))
            {
                if (event.effect_handle != 0 &&
                    event.effect_handle <= cooked.ribbon_cancel_effect_ranges.size() &&
                    static_cast<std::uint32_t>(event.quality) <= 2 && event.sequence != 0 &&
                    current_tick >= event.event_tick && entity->render_instance_id != 0 &&
                    std::isfinite(entity->footprint_radius) && entity->footprint_radius > 0.0f)
                {
                    const auto range = cooked.ribbon_cancel_effect_ranges[event.effect_handle - 1];
                    if (InRange(range, cooked.ribbon_cancel_recipes))
                    {
                        const auto elapsed = static_cast<float>(current_tick - event.event_tick) / 60.0f;
                        const auto anchor = TransformOrigin(event.world_transform);
                        const Float3 forward{event.world_transform[8], event.world_transform[9],
                                             event.world_transform[10]};
                        if (Finite(anchor) && Finite(forward))
                            for (const auto &recipe : std::span(cooked.ribbon_cancel_recipes).subspan(
                                     range.first, range.count))
                            {
                                if (recipe.effect_handle != event.effect_handle ||
                                    elapsed >= recipe.seconds ||
                                    recipe.min_quality > static_cast<std::uint32_t>(event.quality))
                                    continue;
                                const auto age = elapsed / recipe.seconds;
                                auto &ribbon = scratch.PrepareAppendRow();
                                ribbon.owner_id = event.sequence;
                                ribbon.effect_handle = event.effect_handle;
                                ribbon.source_id = recipe.source_id;
                                ribbon.stable_seed = event.stable_seed;
                                ribbon.importance = recipe.importance;
                                ribbon.analytic = true;
                                ribbon.position = anchor;
                                ribbon.previous_position = Add3(anchor,
                                    Scale3(forward, entity->footprint_radius * (1 - age)));
                                ribbon.control = Scale3(Add3(ribbon.position, ribbon.previous_position), .5f);
                                ribbon.control.y += entity->footprint_radius * .2f * (1 - age);
                                ribbon.history_seconds = recipe.history_seconds;
                                ribbon.normalized_age = age;
                                ribbon.start_alpha = 1.0f;
                                ribbon.segments = 16;
                                VfxRibbonOutputInput translated;
                                translated.profile = recipe.profile;
                                translated.motion = recipe.motion;
                                translated.color = recipe.color;
                                translated.hdr = recipe.hdr;
                                translated.gradient_row = recipe.gradient_row;
                                translated.width_head = recipe.width_head * (1 - age);
                                translated.width_tail = recipe.width_tail * (1 - age);
                                ribbon.outputs.push_back(translated);
                                scratch.CommitAppendRow();
                            }
                    }
                }
                continue;
            }
            if (event.effect_handle != 0 &&
                event.effect_handle <= cooked.ribbon_fixed_effect_ranges.size())
            {
                const auto range = cooked.ribbon_fixed_effect_ranges[event.effect_handle - 1];
                if (range.count == 1 && InRange(range, cooked.ribbon_fixed_recipes))
                {
                    const auto &recipe = cooked.ribbon_fixed_recipes[range.first];
                    if (recipe.effect_handle == event.effect_handle &&
                        recipe.kind != VfxRibbonFixedKind::Unsupported)
                    {
                        auto &row = scratch.PrepareAppendRow();
                        if (FixedEventRibbon(event, recipe, current_tick, row))
                            scratch.CommitAppendRow();
                        else scratch.RejectAppendRow();
                        continue;
                    }
                }
            }
            if (const auto *link = std::get_if<VfxLinkPayload>(&event.payload))
            {
                const auto *recipe = FindLinkEffect(cooked, event.effect_handle);
                if (recipe && recipe->kind == VfxRibbonLinkKind::PickupCollect)
                {
                    auto &row = scratch.PrepareAppendRow();
                    if (PickupCollectRibbon(cooked, event, *recipe,
                                            *link, current_tick, row))
                        scratch.CommitAppendRow();
                    else scratch.RejectAppendRow();
                    continue;
                }
                if (!recipe || recipe->kind != VfxRibbonLinkKind::ArrowRain ||
                    !std::isfinite(recipe->seconds) || recipe->seconds <= 0.0f ||
                    event.sequence == 0 ||
                    current_tick < event.event_tick || link->source_id == 0 ||
                    link->target_id == 0 || link->source_id != link->target_id)
                    continue;
                const auto elapsed = static_cast<float>(current_tick - event.event_tick) /
                                     60.0f;
                if (!std::isfinite(elapsed) || elapsed < 0.0f ||
                    elapsed >= recipe->seconds)
                    continue;
                VfxPersistentInput synthetic;
                synthetic.stable_id = event.sequence;
                synthetic.effect_handle = event.effect_handle;
                synthetic.payload = *link;
                synthetic.elapsed_seconds = elapsed;
                synthetic.normalized_age = elapsed / recipe->seconds;
                synthetic.stable_seed = event.stable_seed;
                synthetic.quality = event.quality;
                auto &row = scratch.PrepareAppendRow();
                if (LinkRibbon(cooked, synthetic, *recipe, row, true))
                    scratch.CommitAppendRow();
                else scratch.RejectAppendRow();
                continue;
            }
            const auto *event_recipe = event.effect_handle != 0 &&
                    event.effect_handle <= cooked.ribbon_event_effect_recipes.size()
                ? &cooked.ribbon_event_effect_recipes[event.effect_handle - 1] : nullptr;
            const auto emitter = Emitter(event);
            const bool projectile_event =
                std::holds_alternative<VfxProjectilePayload>(event.payload);
            if (!event_recipe || event_recipe->effect_handle != event.effect_handle ||
                event_recipe->sources.count == 0 || !emitter ||
                event_recipe->payload_kind != PayloadKind(event) ||
                !Finite(emitter->origin) || !Finite(emitter->direction) ||
                !std::isfinite(emitter->scale) || emitter->scale <= 0.0f ||
                current_tick < event.event_tick ||
                (projectile_event && event_recipe->effect_id != kChargedImpactEffect))
                continue;
            const auto effect_range = event_recipe->sources;
            if (!InRange(effect_range, cooked.ribbon_event_sources)) continue;
            const auto direction = NormalizeHorizontal(emitter->direction);
            const auto elapsed = current_tick >= event.event_tick
                ? static_cast<float>(current_tick - event.event_tick) / 60.0f : 0.0f;
            for (const auto &source : std::span(cooked.ribbon_event_sources).subspan(
                     effect_range.first, effect_range.count))
            {
                if (projectile_event && !source.axial_source) continue;
                const auto history = source.history_seconds;
                const auto start_seconds = event_recipe->seconds * source.knots[0];
                const auto end_seconds = event_recipe->seconds *
                    source.knots[source.knot_count - 1];
                if (!std::isfinite(start_seconds) || !std::isfinite(end_seconds) ||
                    end_seconds <= start_seconds || elapsed < start_seconds || elapsed >= end_seconds)
                    continue;
                const auto previous_elapsed = std::max(
                    start_seconds, elapsed - 1.0f / 60.0f);
                auto &ribbon = scratch.PrepareAppendRow();
                ribbon.owner_id = event.sequence;
                ribbon.effect_handle = event.effect_handle;
                ribbon.source_id = source.source_id;
                ribbon.stable_seed = event.stable_seed;
                ribbon.importance = event_recipe->importance;
                ribbon.history_seconds = history;
                ribbon.start_alpha = KnotAlpha(
                    source.knots, source.knot_count,
                    std::clamp(elapsed / event_recipe->seconds, 0.0f, 1.0f));
                const auto quality = static_cast<std::uint32_t>(event.quality);
                float arc_degrees = 35.0f;
                if (quality < 3)
                    arc_degrees = projectile_event ? source.axial_arc_by_quality[quality] :
                                                     source.recoil_arc_by_quality[quality];
                for (const auto &prepared : std::span(cooked.ribbon_event_outputs).subspan(
                         source.outputs.first, source.outputs.count))
                {
                    if (projectile_event && (!prepared.axial || prepared.output.profile != kRibbonAdd))
                        continue;
                    if (!projectile_event && prepared.axial) continue;
                    if (quality < prepared.min_quality) continue;
                    if (quality >= 3 && prepared.arc_present) arc_degrees = prepared.arc_degrees;
                    auto translated = prepared.output;
                    if (!prepared.axial)
                    {
                        translated.width_head = 0.035f * emitter->scale;
                        translated.width_tail = 0.008f * emitter->scale;
                    }
                    if (prepared.detail_present)
                    {
                        if (!InRange(prepared.detail_slices, cooked.ribbon_slice_indices) ||
                            prepared.detail_slices.count == 0) continue;
                        translated.detail_strength = prepared.detail_strength;
                        translated.detail_slice = cooked.ribbon_slice_indices[
                            prepared.detail_slices.first + (event.stable_seed % prepared.detail_slices.count)];
                        if (translated.detail_slice >= 8) continue;
                    }
                    ribbon.outputs.push_back(translated);
                }
                const auto make_position = [&](float time) {
                    const auto arc_progress = std::clamp(
                        (time - start_seconds) / std::max(0.0001f, end_seconds - start_seconds),
                        0.0f, 1.0f);
                    const auto theta = arc_progress * arc_degrees *
                        std::numbers::pi_v<float> / 180.0f;
                    const auto radius = 0.45f * emitter->scale;
                    const auto forward = Scale3(direction, radius * (std::cos(theta) - 1.0f));
                    const Float3 up{0.0f, radius * std::sin(theta), 0.0f};
                    return Add3(emitter->origin, Add3(forward, up));
                };
                if (!ribbon.outputs.empty())
                {
                    if (projectile_event)
                    {
                        const auto line_length = emitter->speed * history;
                        if (!std::isfinite(line_length) || line_length <= 0.0f)
                        {
                            scratch.RejectAppendRow();
                            continue;
                        }
                        ribbon.position = emitter->origin;
                        ribbon.previous_position = Add3(
                            emitter->origin, Scale3(emitter->direction, -line_length));
                    }
                    else
                    {
                        ribbon.position = make_position(elapsed);
                        ribbon.previous_position = make_position(previous_elapsed);
                    }
                    scratch.CommitAppendRow();
                }
                else scratch.RejectAppendRow();
            }
        }
        }
    catch (...)
    {
        scratch.Rollback(original_size);
        throw;
    }
}

void AppendVfxEventRibbonInputs(
    const VfxImage &cooked,
    VfxEventView events,
    double current_tick,
    std::vector<VfxRibbonSourceInput> &result)
{
    const auto original_size = result.size();
    try
    {
        VfxRibbonInputScratch scratch;
        AppendVfxEventRibbonInputs(cooked, events, current_tick, scratch);
        auto rows = std::move(scratch).TakeSources();
        result.insert(result.end(), std::make_move_iterator(rows.begin()),
                      std::make_move_iterator(rows.end()));
    }
    catch (...)
    {
        result.resize(original_size);
        throw;
    }
}

std::vector<VfxRibbonSourceInput> BuildVfxEventRibbonInputs(
    const VfxImage &cooked,
    VfxEventView events,
    double current_tick)
{
    VfxRibbonInputScratch scratch;
    AppendVfxEventRibbonInputs(cooked, events, current_tick, scratch);
    return std::move(scratch).TakeSources();
}
}
