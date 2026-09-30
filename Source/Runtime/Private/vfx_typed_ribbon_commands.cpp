#include "vfx_typed_ribbon_commands.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <numbers>
#include <string_view>

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
constexpr std::uint64_t kArrowRainPullEffect = Hash64("particle.upgrade.arrow_rain.pull");
constexpr std::uint64_t kCommonPullEffect = Hash64("particle.common.pull");
constexpr std::uint64_t kRetreatMoveEffect = Hash64("particle.skill.retreat_shot.move");
constexpr std::uint64_t kExplosivePrePullEffect = Hash64("particle.upgrade.explosive.pre_pull");
constexpr std::uint64_t kPickupXpCollectEffect = Hash64("particle.pickup.xp_collect");
constexpr std::uint64_t kPickupHealCollectEffect = Hash64("particle.pickup.heal_collect");
constexpr std::uint64_t kPickupMagnetCollectEffect = Hash64("particle.pickup.magnet_collect");
constexpr std::uint64_t kPickupRelicCollectEffect = Hash64("particle.pickup.relic_collect");
constexpr std::uint64_t kBossDashWakeEffect = Hash64("persistent.boss.dash_wake");
constexpr std::uint32_t kAbsorbRibbonSource = Hash32("absorb_ribbon");
constexpr std::uint32_t kLinkCoreSource = Hash32("link_core");
constexpr std::uint32_t kLinkOuterSource = Hash32("link_outer");
constexpr std::uint32_t kAxialAfterlineSource = Hash32("axial_afterline");
constexpr std::uint32_t kImpactAxisRibbon = Hash32("impact_axis_ribbon");
constexpr std::uint32_t kPointPayload = Hash32("PointEventPayload");
constexpr std::uint32_t kCirclePayload = Hash32("CircleAreaPayload");
constexpr std::uint32_t kBezierRibbon = Hash32("bezier_ribbon");
constexpr std::uint32_t kAnalyticCoverage = Hash32("analytic");
constexpr std::uint32_t kRibbonEnergyCore = Hash32("ribbon_energy_core");
constexpr std::uint32_t kLinePayload = Hash32("LineAreaPayload");
constexpr std::uint32_t kWakeRails = Hash32("wake_rails");
constexpr std::uint32_t kPairedGroundRibbons = Hash32("paired_ground_ribbons");

bool Finite(const Float3 &value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

const VfxEffectRecord *FindEffect(const VfxProgramData &program,
                                  VfxEffectHandle handle) noexcept
{
    if (handle == 0 || handle > program.effects.size()) return nullptr;
    const auto &effect = program.effects[handle - 1];
    return effect.handle == handle ? &effect : nullptr;
}

struct ParameterValue
{
    bool present{};
    bool valid{true};
    float value{};
};

ParameterValue Parameter(std::span<const VfxParameterRecord> parameters,
                         std::uint32_t key) noexcept
{
    const auto found = std::find_if(parameters.begin(), parameters.end(),
        [key](const auto &parameter) { return parameter.key == key; });
    if (found == parameters.end()) return {};
    if (found->type != VfxParameterType::Float) return {true, false, 0.0f};
    const auto value = std::bit_cast<float>(found->bits);
    return {true, std::isfinite(value), value};
}

float KnotAlpha(const VfxSourceRecord &source, float normalized_age) noexcept
{
    if (source.knot_count == 0) return 1.0f;
    const auto age = std::clamp(normalized_age, 0.0f, 1.0f);
    if (source.knot_count == 2)
    {
        if (source.knots[0] <= 0.0f) return 1.0f;
        if (age <= source.knots[0]) return 0.0f;
        return std::clamp((age - source.knots[0]) /
                              std::max(0.0001f, source.knots[1] - source.knots[0]),
                          0.0f, 1.0f);
    }
    if (age <= source.knots[0]) return 0.0f;
    if (age < source.knots[1])
        return (age - source.knots[0]) /
               std::max(0.0001f, source.knots[1] - source.knots[0]);
    if (age < source.knots[2]) return 1.0f;
    if (age < source.knots[3])
        return 1.0f - (age - source.knots[2]) /
            std::max(0.0001f, source.knots[3] - source.knots[2]);
    return 0.0f;
}

bool AcceptedMotion(VfxMotionKind motion) noexcept
{
    return motion == VfxMotionKind::HistoryRibbonTaperedFlow ||
           motion == VfxMotionKind::ShortTurbulentWake ||
           motion == VfxMotionKind::TwoPhaseNarrowWakes;
}

bool IsRibbonDetailBinding(const VfxProgramData &cooked,
                           const VfxTextureBindingRecord &binding) noexcept
{
    const auto resource = std::find_if(cooked.texture_resources.begin(),
                                       cooked.texture_resources.end(),
        [&](const auto &candidate) { return candidate.slot == binding.catalog_slot; });
    if (resource == cooked.texture_resources.end() ||
        resource->asset_path_bytes.first > cooked.strings.size() ||
        resource->asset_path_bytes.count > cooked.strings.size() -
                                              resource->asset_path_bytes.first)
        return false;
    const auto *data = reinterpret_cast<const char *>(cooked.strings.data()) +
                       resource->asset_path_bytes.first;
    const std::string_view path(data, resource->asset_path_bytes.count);
    return path.find("ribbon_detail_array") != std::string_view::npos;
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

template<class T> bool InRange(VfxRange r,const std::vector<T>&v)
{return r.first<=v.size()&&r.count<=v.size()-r.first;}

std::optional<VfxRibbonSourceInput> BossDashWakeRibbon(
    const VfxProgramData &cooked, const VfxPersistentInput &input,
    const VfxEffectRecord &effect)
{
    const auto *line = std::get_if<VfxLinePayload>(&input.payload);
    if (!line || effect.input_mode != 1 || effect.timing_kind != 1 ||
        effect.payload_kind != kLinePayload || input.stable_id == 0 ||
        static_cast<std::uint32_t>(input.quality) > 2 ||
        !Finite(line->start) || !Finite(line->end) || !Finite(line->direction) ||
        !std::isfinite(line->width) || line->width <= 0.0f ||
        !std::isfinite(line->lifetime01) || line->lifetime01 < 0.0f ||
        line->lifetime01 >= 1.0f || !std::isfinite(input.normalized_age) ||
        input.normalized_age != line->lifetime01 || !InRange(effect.sources, cooked.sources))
        return {};
    const auto length = std::hypot(line->end.x - line->start.x,
                                   line->end.z - line->start.z);
    if (!std::isfinite(length) || length <= 0.0001f) return {};

    std::optional<VfxRibbonSourceInput> result;
    for (std::uint32_t source_index = effect.sources.first;
         source_index < effect.sources.first + effect.sources.count; ++source_index)
    {
        const auto &source = cooked.sources[source_index];
        if (source.effect != effect.handle || source.stable_id != kWakeRails ||
            source.type != kHistoryRibbon || source.knot_count != 2 ||
            source.knots[0] != 0.0f || source.knots[1] != 1.0f ||
            !InRange(source.parameters, cooked.parameters) ||
            source.parameters.count != 2 || !InRange(source.outputs, cooked.outputs) ||
            source.outputs.count != 1)
            continue;
        const auto params = std::span(cooked.parameters).subspan(
            source.parameters.first, source.parameters.count);
        const auto history = Parameter(params, Hash32("history_seconds"));
        const auto ground = std::find_if(params.begin(), params.end(),
            [](const auto &p) { return p.key == Hash32("ground_lock"); });
        if (!history.present || !history.valid || history.value != 0.24f ||
            ground == params.end() || ground->type != VfxParameterType::Int ||
            ground->bits != 1u || params[0].key == params[1].key)
            continue;
        const auto &output = cooked.outputs[source.outputs.first];
        if (output.source != source_index || output.profile != kRibbonAdd ||
            output.shape != kPairedGroundRibbons ||
            output.shape_domain != Hash32("ribbon") ||
            output.shape_scale_rule != Hash32("component_width_and_history") ||
            output.shape_component_kind != Hash32("ribbon") ||
            output.motion != VfxMotionKind::HistoryRibbonTearsBackward ||
            output.coverage_type != kAnalyticCoverage ||
            output.coverage_ref != kRibbonEnergyCore ||
            output.min_quality > static_cast<std::uint32_t>(input.quality) ||
            !InRange(output.parameters, cooked.parameters) ||
            output.parameters.count != 2 ||
            !InRange(output.textures, cooked.texture_bindings) ||
            !std::all_of(output.rgba.begin(), output.rgba.end(),
                         [](float value) { return std::isfinite(value); }) ||
            output.rgba[3] < 0.0f || output.rgba[3] > 1.0f ||
            !std::isfinite(output.hdr) || output.hdr <= 0.0f ||
            output.gradient_row >= 68u)
            continue;
        const auto widths = std::span(cooked.parameters).subspan(
            output.parameters.first, output.parameters.count);
        const auto head = Parameter(widths, Hash32("width_head"));
        const auto tail = Parameter(widths, Hash32("width_tail"));
        if (!head.present || !head.valid || head.value <= 0.0f ||
            !tail.present || !tail.valid || tail.value < 0.0f ||
            widths[0].key == widths[1].key)
            continue;
        const auto bindings = std::span(cooked.texture_bindings).subspan(
            output.textures.first, output.textures.count);
        const VfxTextureBindingRecord *detail_binding = nullptr;
        for (const auto &binding : bindings)
            if (binding.role == Hash32("ribbon_detail_array"))
            {
                if (detail_binding) { detail_binding = nullptr; break; }
                detail_binding = &binding;
            }
        if (!detail_binding) continue;
        const auto &detail = *detail_binding;
        if (detail.role != Hash32("ribbon_detail_array") ||
            !IsRibbonDetailBinding(cooked, detail) ||
            detail.selection != Hash32("stable_seed_mod_group_size") ||
            detail.min_quality > static_cast<std::uint32_t>(input.quality) ||
            !std::isfinite(detail.strength) || detail.strength < 0.0f ||
            detail.strength > 1.0f || !InRange(detail.slices, cooked.slice_indices) ||
            detail.slices.count != 2)
            continue;
        const auto slices = std::span(cooked.slice_indices).subspan(
            detail.slices.first, detail.slices.count);
        if (std::any_of(slices.begin(), slices.end(),
                        [](std::uint32_t slice) { return slice >= 8u; }))
            continue;

        VfxRibbonSourceInput ribbon;
        ribbon.owner_id = input.stable_id;
        ribbon.effect_handle = effect.handle;
        ribbon.source_id = source.stable_id;
        ribbon.stable_seed = input.stable_seed;
        ribbon.importance = effect.importance;
        ribbon.previous_position = {line->start.x, 0.025f, line->start.z};
        ribbon.position = {line->end.x, 0.025f, line->end.z};
        ribbon.control.x = line->width; // lane width for the paired ground rails
        ribbon.history_seconds = history.value;
        ribbon.start_alpha = KnotAlpha(source, input.normalized_age);
        VfxRibbonOutputInput translated;
        translated.profile = output.profile;
        translated.motion = output.motion;
        translated.color = {output.rgba[0], output.rgba[1], output.rgba[2],
                            output.rgba[3]};
        translated.width_head = head.value;
        translated.width_tail = tail.value;
        translated.hdr = output.hdr;
        translated.gradient_row = output.gradient_row;
        translated.detail_slice = slices[input.stable_seed % slices.size()];
        translated.detail_strength = detail.strength;
        ribbon.outputs.push_back(translated);
        if (result) return {}; // a stable owner can publish only one wake source
        result = std::move(ribbon);
    }
    return result;
}

bool PickupCollectEffect(std::uint64_t id) noexcept
{
    return id == kPickupXpCollectEffect || id == kPickupHealCollectEffect ||
           id == kPickupMagnetCollectEffect || id == kPickupRelicCollectEffect;
}

std::optional<VfxRibbonSourceInput> PickupCollectRibbon(
    const VfxProgramData &cooked, const VfxEventInput &event,
    const VfxEffectRecord &effect, const VfxLinkPayload &link, Tick current_tick)
{
    if (effect.input_mode != 0 || effect.timing_kind != 0 ||
        effect.payload_kind != Hash32("SourceTargetPayload") ||
        !std::isfinite(effect.seconds) || effect.seconds <= 0.0f ||
        event.sequence == 0 || current_tick < event.event_tick ||
        static_cast<std::uint32_t>(event.quality) > 2 ||
        !Finite(link.source_position) || !Finite(link.target_position) ||
        !InRange(effect.sources, cooked.sources))
        return {};
    const float distance = std::hypot(
        link.target_position.x - link.source_position.x,
        link.target_position.y - link.source_position.y,
        link.target_position.z - link.source_position.z);
    if (!std::isfinite(distance) || distance <= 0.0001f) return {};
    const float elapsed = static_cast<float>(current_tick - event.event_tick) / 60.0f;
    if (!std::isfinite(elapsed) || elapsed >= effect.seconds) return {};

    std::optional<VfxRibbonSourceInput> result;
    for (std::uint32_t source_index = effect.sources.first;
         source_index < effect.sources.first + effect.sources.count; ++source_index)
    {
        const auto &source = cooked.sources[source_index];
        if (source.effect != effect.handle || source.stable_id != kAbsorbRibbonSource ||
            source.type != kHistoryRibbon || source.knot_count != 2 ||
            !std::isfinite(source.knots[0]) || !std::isfinite(source.knots[1]) ||
            source.knots[0] != 0.1f || source.knots[1] != 1.0f ||
            !InRange(source.parameters, cooked.parameters) || source.parameters.count != 0 ||
            !InRange(source.outputs, cooked.outputs) || source.outputs.count != 1)
            continue;
        const float start = source.knots[0] * effect.seconds;
        const float end = source.knots[1] * effect.seconds;
        if (!std::isfinite(start) || !std::isfinite(end) ||
            end <= start || elapsed < start || elapsed >= end)
            continue;
        const auto &output = cooked.outputs[source.outputs.first];
        if (output.source != source_index || output.profile != kRibbonAdd ||
            output.shape != Hash32("short_bezier_to_player") ||
            output.motion != VfxMotionKind::CurvesTowardCollector ||
            output.coverage_type != Hash32("analytic") ||
            output.coverage_ref != Hash32("ribbon_energy_core") ||
            output.min_quality > static_cast<std::uint32_t>(event.quality) ||
            !InRange(output.parameters, cooked.parameters) || output.parameters.count != 2 ||
            !InRange(output.textures, cooked.texture_bindings) ||
            !std::isfinite(output.rgba[0]) || !std::isfinite(output.rgba[1]) ||
            !std::isfinite(output.rgba[2]) || !std::isfinite(output.rgba[3]) ||
            output.rgba[3] < 0.0f || output.rgba[3] > 1.0f ||
            !std::isfinite(output.hdr) || output.hdr <= 0.0f ||
            output.gradient_row >= 68u)
            continue;
        const auto params = std::span(cooked.parameters).subspan(
            output.parameters.first, output.parameters.count);
        const auto segments = std::find_if(params.begin(), params.end(),
            [](const auto &p) { return p.key == Hash32("segments"); });
        const auto width = Parameter(params, Hash32("width"));
        if (segments == params.end() || segments->type != VfxParameterType::Int ||
            segments->bits < 2 || segments->bits > 63 ||
            !width.present || !width.valid || width.value <= 0.0f)
            continue;

        VfxRibbonOutputInput translated;
        translated.profile = output.profile;
        translated.motion = output.motion;
        translated.color = {output.rgba[0], output.rgba[1],
                            output.rgba[2], output.rgba[3]};
        translated.hdr = output.hdr;
        translated.gradient_row = output.gradient_row;
        translated.width_head = width.value;
        translated.width_tail = width.value;
        bool valid_textures = true;
        for (const auto &binding : std::span(cooked.texture_bindings).subspan(
                 output.textures.first, output.textures.count))
        {
            const auto resource = std::find_if(cooked.texture_resources.begin(),
                cooked.texture_resources.end(), [&](const auto &candidate) {
                    return candidate.slot == binding.catalog_slot;
                });
            if (resource == cooked.texture_resources.end() ||
                !InRange(resource->asset_path_bytes, cooked.strings) ||
                binding.min_quality > 2 || !std::isfinite(binding.strength) ||
                binding.strength < 0.0f || binding.strength > 1.0f)
            {
                valid_textures = false;
                break;
            }
            const std::string_view path(
                reinterpret_cast<const char *>(cooked.strings.data()) +
                    resource->asset_path_bytes.first,
                resource->asset_path_bytes.count);
            if (binding.role == Hash32("profile.global") ||
                binding.role == Hash32("profile.authored") ||
                binding.role == Hash32("profile.optional_detail"))
            {
                const bool known =
                    (binding.role == Hash32("profile.global") &&
                     (path == "Content/Textures/VFX/vfx_gradient_lut.dds" ||
                      path == "Content/Textures/VFX/vfx_curve_lut.dds")) ||
                    (binding.role == Hash32("profile.authored") &&
                     path == "Content/Textures/VFX/vfx_flow_curl_2d.dds") ||
                    (binding.role == Hash32("profile.optional_detail") &&
                     path == "Content/Textures/VFX/vfx_ribbon_detail_array.dds");
                if (!known || binding.min_quality != 0 || binding.selection != 0 ||
                    binding.slices.count != 0 || binding.strength != 1.0f)
                    valid_textures = false;
                if (!valid_textures) break;
                continue;
            }
            if (binding.role != Hash32("ribbon_detail_array"))
            {
                valid_textures = false;
                break;
            }
            if (binding.min_quality > static_cast<std::uint32_t>(event.quality)) continue;
            if (path != "Content/Textures/VFX/vfx_ribbon_detail_array.dds" ||
                !InRange(binding.slices, cooked.slice_indices) ||
                binding.slices.count == 0 ||
                binding.selection != Hash32("stable_seed_mod_group_size") ||
                translated.detail_slice != 0xffffffffu)
            {
                valid_textures = false;
                break;
            }
            for (const auto slice : std::span(cooked.slice_indices).subspan(
                     binding.slices.first, binding.slices.count))
                if (slice >= 8) valid_textures = false;
            if (!valid_textures) break;
            translated.detail_slice = cooked.slice_indices[
                binding.slices.first + event.stable_seed % binding.slices.count];
            translated.detail_strength = binding.strength;
        }
        if (!valid_textures || result) return {};

        VfxRibbonSourceInput ribbon;
        ribbon.owner_id = event.sequence;
        ribbon.effect_handle = event.effect_handle;
        ribbon.source_id = source.stable_id;
        ribbon.stable_seed = event.stable_seed;
        ribbon.importance = effect.importance;
        ribbon.analytic = true;
        ribbon.previous_position = link.source_position;
        ribbon.position = link.target_position;
        ribbon.control = Add3(Scale3(link.source_position, 0.5f),
                              Scale3(link.target_position, 0.5f));
        ribbon.control.y += std::min(0.35f, distance * 0.12f);
        ribbon.history_seconds = end - start;
        ribbon.normalized_age = (elapsed - start) / (end - start);
        ribbon.segments = segments->bits;
        ribbon.outputs.push_back(translated);
        result = std::move(ribbon);
    }
    return result;
}

std::optional<VfxRibbonSourceInput> LinkRibbon(const VfxProgramData &c,
                                               const VfxPersistentInput &input,
                                               const VfxEffectRecord &effect,
                                               bool allow_arrow_rain = false)
{
    const auto *link=std::get_if<VfxLinkPayload>(&input.payload);
    if(!link||(allow_arrow_rain ? (effect.input_mode!=0||effect.timing_kind!=0)
                               : (effect.input_mode!=1||effect.timing_kind!=1))||
        effect.payload_kind!=Hash32("SourceTargetPayload")||
        input.stable_id==0||static_cast<std::uint32_t>(input.quality)>2||!Finite(link->source_position)||!Finite(link->target_position)||
        !std::isfinite(link->width)||link->width<=0||!std::isfinite(input.normalized_age)||input.normalized_age<0||input.normalized_age>=1||
        !std::isfinite(input.elapsed_seconds)||input.elapsed_seconds<0||!InRange(effect.sources,c.sources))return {};
    const auto identity=std::find_if(c.effect_lookup.begin(),c.effect_lookup.end(),[&](const auto &x){return x.handle==effect.handle;});
    if(identity==c.effect_lookup.end()||(identity->effect_id!=Hash64("particle.line.ricochet")&&
        identity->effect_id!=Hash64("particle.line.burn_transfer")&&identity->effect_id!=Hash64("particle.line.relic_chain")&&
        identity->effect_id!=Hash64("particle.upgrade.ricochet.return")&&
        (!allow_arrow_rain || identity->effect_id != kArrowRainPullEffect)))return {};
    const float length=std::hypot(link->target_position.x-link->source_position.x,
        link->target_position.y-link->source_position.y,link->target_position.z-link->source_position.z);
    if(!std::isfinite(length)||length<=.0001f)return {};
    VfxRibbonSourceInput ribbon;
    ribbon.owner_id=input.stable_id;ribbon.effect_handle=effect.handle;ribbon.source_id=Hash32("link_core");
    ribbon.stable_seed=input.stable_seed;ribbon.importance=effect.importance;ribbon.analytic=true;
    ribbon.position=link->target_position;ribbon.previous_position=link->source_position;
    ribbon.control=Add3(Scale3(link->source_position,.5f),Scale3(link->target_position,.5f));
    ribbon.control.y+=std::min(.35f,length*.12f);ribbon.history_seconds=1.0f/60;
    ribbon.normalized_age=input.normalized_age;ribbon.start_alpha=std::clamp((1-input.normalized_age)/.2f,0.0f,1.0f);
    float taper{};std::uint32_t segments{};bool have_core=false;
    // Both authored source records share one analytic curve; core defines tessellation and taper.
    for(std::uint32_t si=effect.sources.first;si<effect.sources.first+effect.sources.count;++si)
    {
        const auto &source=c.sources[si];
        if(source.effect!=effect.handle||source.type!=kHistoryRibbon||source.stable_id!=Hash32("link_core")||
            !InRange(source.parameters,c.parameters)||source.parameters.count!=0||!InRange(source.outputs,c.outputs))continue;
        for(const auto &o:std::span(c.outputs).subspan(source.outputs.first,source.outputs.count))
        {
            if(o.source!=si||o.profile!=kRibbonAdd||o.shape!=Hash32("bezier_ribbon")||
                o.motion!=VfxMotionKind::CurvedLinkTravelingHead||!InRange(o.parameters,c.parameters)||o.parameters.count!=3)continue;
            const auto params=std::span(c.parameters).subspan(o.parameters.first,o.parameters.count);
            const auto t=Parameter(params,Hash32("taper"));
            const auto sg=std::find_if(params.begin(),params.end(),[](const auto &v){return v.key==Hash32("segments");});
            const auto pulse=std::find_if(params.begin(),params.end(),[](const auto &v){return v.key==Hash32("travel_pulse");});
            if(!t.present||!t.valid||t.value<0||t.value>1||sg==params.end()||sg->type!=VfxParameterType::Int||sg->bits<2||sg->bits>63||
                pulse==params.end()||pulse->type!=VfxParameterType::Bool||pulse->bits!=1)continue;
            if(have_core)return {};have_core=true;taper=t.value;segments=sg->bits;
        }
    }
    if(!have_core)return {};ribbon.segments=segments;
    for(std::uint32_t si=effect.sources.first;si<effect.sources.first+effect.sources.count;++si)
    {
        const auto &source=c.sources[si];const bool core=source.stable_id==Hash32("link_core");
        if((!core&&source.stable_id!=Hash32("link_outer"))||source.effect!=effect.handle||source.type!=kHistoryRibbon||
            !InRange(source.parameters,c.parameters)||source.parameters.count!=0||!InRange(source.outputs,c.outputs)||
            source.knot_count!=2||source.knots[0]!=0||source.knots[1]!=1)continue;
        for(const auto &o:std::span(c.outputs).subspan(source.outputs.first,source.outputs.count))
        {
            if(o.source!=si||o.profile!=(core?kRibbonAdd:kRibbonOit)||o.shape!=Hash32("bezier_ribbon")||
                o.motion!=(core?VfxMotionKind::CurvedLinkTravelingHead:VfxMotionKind::SoftWiderSheath)||
                o.min_quality>static_cast<std::uint32_t>(input.quality)||!InRange(o.parameters,c.parameters)||
                o.parameters.count!=(core?3u:2u)||!InRange(o.textures,c.texture_bindings))continue;
            VfxRibbonOutputInput out;out.profile=o.profile;out.motion=o.motion;out.color={o.rgba[0],o.rgba[1],o.rgba[2],o.rgba[3]};
            out.hdr=o.hdr;out.gradient_row=o.gradient_row;out.travel_pulse=core;
            float width=link->width;
            if(!core){const auto params=std::span(c.parameters).subspan(o.parameters.first,o.parameters.count);
                const auto multiplier=Parameter(params,Hash32("width_multiplier")),noise=Parameter(params,Hash32("noise_warp"));
                if(!multiplier.present||!multiplier.valid||multiplier.value<=0||!noise.present||!noise.valid||noise.value<0)continue;
                width*=multiplier.value;out.noise_warp=noise.value;}
            out.width_head=width;out.width_tail=width*taper;out.dashed=o.coverage_ref==Hash32("ribbon_dash_repeat");
            bool valid=std::isfinite(width)&&width>0&&Finite({out.color.x,out.color.y,out.color.z})&&
                std::isfinite(out.color.w)&&out.color.w>=0&&out.color.w<=1&&std::isfinite(out.hdr)&&out.hdr>0;
            for(const auto &b:std::span(c.texture_bindings).subspan(o.textures.first,o.textures.count))
            {
                if(b.role!=Hash32("ribbon_detail_array"))continue;
                if(b.min_quality>2||!std::isfinite(b.strength)||b.strength<0||b.strength>1){valid=false;break;}
                if(b.min_quality>static_cast<std::uint32_t>(input.quality))continue;
                if(!IsRibbonDetailBinding(c,b)||!InRange(b.slices,c.slice_indices)||b.slices.count==0||
                    b.selection!=Hash32("stable_seed_mod_group_size")||out.detail_slice!=0xffffffffu){valid=false;break;}
                for(auto slice:std::span(c.slice_indices).subspan(b.slices.first,b.slices.count))if(slice>=8)valid=false;
                if(!valid)break;
                out.detail_slice=c.slice_indices[b.slices.first+input.stable_seed%b.slices.count];out.detail_strength=b.strength;
            }
            if(valid)ribbon.outputs.push_back(out);
        }
    }
    return ribbon.outputs.empty()?std::nullopt:std::optional{std::move(ribbon)};
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
    const VfxEventInput &event, std::uint64_t effect_id) noexcept
{
    if (effect_id == kExplosivePrePullEffect)
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

    if (effect_id != kCommonPullEffect && effect_id != kRetreatMoveEffect)
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
    const auto sign = effect_id == kCommonPullEffect ? 1.0f : -1.0f;
    return FixedRibbonGeometry{point->position, NormalizeHorizontal(point->direction),
                               point->authored_scale, sign};
}

bool TextureAssetPath(const VfxProgramData &cooked,
                      const VfxTextureBindingRecord &binding,
                      std::string_view &path) noexcept
{
    const auto resource = std::find_if(cooked.texture_resources.begin(),
                                       cooked.texture_resources.end(),
        [&](const auto &candidate) { return candidate.slot == binding.catalog_slot; });
    if (resource == cooked.texture_resources.end() ||
        !InRange(resource->asset_path_bytes, cooked.strings))
        return false;
    path = std::string_view(
        reinterpret_cast<const char *>(cooked.strings.data()) +
            resource->asset_path_bytes.first,
        resource->asset_path_bytes.count);
    return true;
}

bool ValidFixedRibbonTextures(const VfxProgramData &cooked,
                              const VfxOutputRecord &output) noexcept
{
    if (!InRange(output.textures, cooked.texture_bindings)) return false;
    for (const auto &binding : std::span(cooked.texture_bindings).subspan(
             output.textures.first, output.textures.count))
    {
        std::string_view path;
        if (!TextureAssetPath(cooked, binding, path) || binding.min_quality > 2 ||
            !std::isfinite(binding.strength) || binding.strength != 1.0f ||
            binding.selection != 0 || !InRange(binding.slices, cooked.slice_indices) ||
            binding.slices.count != 0 ||
            binding.first_frame != 0 || binding.frame_count != 0 ||
            binding.fps != 0.0f)
            return false;
        const auto role = binding.role;
        const bool global = role == Hash32("profile.global") &&
            (path == "Content/Textures/VFX/vfx_gradient_lut.dds" ||
             path == "Content/Textures/VFX/vfx_curve_lut.dds");
        const bool authored = role == Hash32("profile.authored") &&
            path == "Content/Textures/VFX/vfx_flow_curl_2d.dds";
        const bool optional_detail = role == Hash32("profile.optional_detail") &&
            path == "Content/Textures/VFX/vfx_ribbon_detail_array.dds";
        if (!global && !authored && !optional_detail) return false;
    }
    return true;
}

bool DecodeFixedRibbonOutput(const VfxProgramData &cooked,
                             const VfxOutputRecord &output,
                             bool core,
                             float base_width,
                             float &taper,
                             std::uint32_t &segments_value,
                             VfxRibbonOutputInput &translated) noexcept
{
    if (output.profile != (core ? kRibbonAdd : kRibbonOit) ||
        output.shape != kBezierRibbon ||
        output.motion != (core ? VfxMotionKind::CurvedLinkTravelingHead
                               : VfxMotionKind::SoftWiderSheath) ||
        output.coverage_type != kAnalyticCoverage ||
        output.coverage_ref != kRibbonEnergyCore ||
        !std::isfinite(output.rgba[0]) || !std::isfinite(output.rgba[1]) ||
        !std::isfinite(output.rgba[2]) || !std::isfinite(output.rgba[3]) ||
        output.rgba[3] < 0.0f || output.rgba[3] > 1.0f ||
        !std::isfinite(output.hdr) || output.hdr <= 0.0f ||
        output.gradient_row >= 68u || !InRange(output.parameters, cooked.parameters) ||
        !ValidFixedRibbonTextures(cooked, output))
        return false;

    const auto params = std::span(cooked.parameters).subspan(
        output.parameters.first, output.parameters.count);
    const auto segments = std::find_if(params.begin(), params.end(),
        [](const auto &parameter) { return parameter.key == Hash32("segments"); });
    const auto travel_pulse = std::find_if(params.begin(), params.end(),
        [](const auto &parameter) { return parameter.key == Hash32("travel_pulse"); });
    const auto authored_taper = Parameter(params, Hash32("taper"));
    const auto width_multiplier = Parameter(params, Hash32("width_multiplier"));
    const auto noise_warp = Parameter(params, Hash32("noise_warp"));
    if (core)
    {
        if (output.parameters.count != 3 || segments == params.end() ||
            segments->type != VfxParameterType::Int || segments->bits < 2 ||
            segments->bits > 63 || travel_pulse == params.end() ||
            travel_pulse->type != VfxParameterType::Bool || travel_pulse->bits != 1 ||
            !authored_taper.present || !authored_taper.valid ||
            authored_taper.value < 0.0f || authored_taper.value > 1.0f)
            return false;
        taper = authored_taper.value;
        segments_value = segments->bits;
    }
    else if (output.parameters.count != 2 || !width_multiplier.present ||
             !width_multiplier.valid || width_multiplier.value <= 0.0f ||
             !noise_warp.present || !noise_warp.valid || noise_warp.value < 0.0f)
        return false;

    if (!std::isfinite(base_width) || base_width <= 0.0f) return false;
    translated.profile = output.profile;
    translated.motion = output.motion;
    translated.color = {output.rgba[0], output.rgba[1], output.rgba[2], output.rgba[3]};
    translated.hdr = output.hdr;
    translated.gradient_row = output.gradient_row;
    translated.width_head = core ? base_width : base_width * width_multiplier.value;
    translated.width_tail = translated.width_head * taper;
    translated.noise_warp = core ? 0.0f : noise_warp.value;
    translated.travel_pulse = core;
    return std::isfinite(translated.width_head) && translated.width_head > 0.0f &&
           std::isfinite(translated.width_tail) && translated.width_tail >= 0.0f;
}

std::optional<VfxRibbonSourceInput> FixedEventRibbon(
    const VfxProgramData &cooked,
    const VfxEventInput &event,
    const VfxEffectRecord &effect,
    std::uint64_t effect_id,
    Tick current_tick)
{
    const auto expected_payload = effect_id == kExplosivePrePullEffect
        ? kCirclePayload : kPointPayload;
    if (effect.input_mode != 0 || effect.timing_kind != 0 ||
        effect.payload_kind != expected_payload || effect.sources.count != 2 ||
        !std::isfinite(effect.seconds) || effect.seconds != 0.45f ||
        event.sequence == 0 || static_cast<std::uint32_t>(event.quality) > 2 ||
        current_tick < event.event_tick || !InRange(effect.sources, cooked.sources))
        return std::nullopt;

    const auto identity = std::find_if(cooked.effect_lookup.begin(),
                                       cooked.effect_lookup.end(),
        [&](const auto &candidate) { return candidate.handle == effect.handle; });
    if (identity == cooked.effect_lookup.end() || identity->effect_id != effect_id ||
        std::count_if(cooked.effect_lookup.begin(), cooked.effect_lookup.end(),
                      [&](const auto &candidate) {
                          return candidate.handle == effect.handle ||
                                 candidate.effect_id == effect_id;
                      }) != 1)
        return std::nullopt;
    const auto geometry = FixedEventGeometry(event, effect_id);
    if (!geometry) return std::nullopt;
    const auto elapsed = static_cast<float>(current_tick - event.event_tick) / 60.0f;
    if (!std::isfinite(elapsed) || elapsed < 0.0f || elapsed >= effect.seconds)
        return std::nullopt;

    std::uint32_t core_index{};
    std::uint32_t outer_index{};
    bool have_core = false;
    bool have_outer = false;
    for (std::uint32_t index = effect.sources.first;
         index < effect.sources.first + effect.sources.count; ++index)
    {
        const auto &source = cooked.sources[index];
        const bool core = source.stable_id == kLinkCoreSource;
        const bool outer = source.stable_id == kLinkOuterSource;
        if (source.effect != effect.handle || (!core && !outer) ||
            (core && have_core) || (outer && have_outer) || source.type != kHistoryRibbon ||
            source.knot_count != 2 || source.knots[0] != 0.0f ||
            source.knots[1] != 1.0f || source.parameters.count != 0 ||
            source.outputs.count != 1 || !InRange(source.parameters, cooked.parameters) ||
            !InRange(source.outputs, cooked.outputs) ||
            source.authoritative != (effect_id == kExplosivePrePullEffect && core ? 1u : 0u))
            return std::nullopt;
        if (core) { core_index = index; have_core = true; }
        else { outer_index = index; have_outer = true; }
    }
    if (!have_core || !have_outer) return std::nullopt;

    float taper{};
    std::uint32_t segments{};
    VfxRibbonOutputInput core_output;
    const auto &core_record = cooked.outputs[cooked.sources[core_index].outputs.first];
    if (core_record.source != core_index || core_record.min_quality > 2 ||
        !DecodeFixedRibbonOutput(cooked, core_record, true,
                                 0.035f * geometry->reach, taper, segments, core_output))
        return std::nullopt;
    VfxRibbonOutputInput outer_output;
    const auto &outer_record = cooked.outputs[cooked.sources[outer_index].outputs.first];
    if (outer_record.source != outer_index || outer_record.min_quality > 2 ||
        !DecodeFixedRibbonOutput(cooked, outer_record, false,
                                 0.035f * geometry->reach, taper, segments, outer_output))
        return std::nullopt;

    VfxRibbonSourceInput ribbon;
    ribbon.owner_id = event.sequence;
    ribbon.effect_handle = event.effect_handle;
    ribbon.source_id = kLinkCoreSource;
    ribbon.stable_seed = event.stable_seed;
    ribbon.importance = effect.importance;
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
    ribbon.normalized_age = elapsed / effect.seconds;
    ribbon.start_alpha = std::clamp((1.0f - ribbon.normalized_age) / 0.2f, 0.0f, 1.0f);
    ribbon.segments = segments;
    if (core_record.min_quality <= static_cast<std::uint32_t>(event.quality))
        ribbon.outputs.push_back(core_output);
    if (outer_record.min_quality <= static_cast<std::uint32_t>(event.quality))
        ribbon.outputs.push_back(outer_output);
    if (ribbon.outputs.empty()) return std::nullopt;
    return ribbon;
}
}

std::vector<VfxRibbonSourceInput> BuildVfxTypedRibbonInputs(
    const VfxProgramData &cooked,
    std::span<const VfxPersistentInput> persistent)
{
    std::vector<VfxRibbonSourceInput> result;
    for (const auto &input : persistent)
    {
        const auto *effect = FindEffect(cooked, input.effect_handle);
        if (!effect) continue;
        if (std::holds_alternative<VfxLinePayload>(input.payload))
        {
            const auto identity = std::find_if(cooked.effect_lookup.begin(),
                                               cooked.effect_lookup.end(),
                [&](const auto &entry) { return entry.handle == effect->handle; });
            if (identity != cooked.effect_lookup.end() &&
                identity->effect_id == kBossDashWakeEffect)
                if (auto wake = BossDashWakeRibbon(cooked, input, *effect))
                    result.push_back(std::move(*wake));
            continue;
        }
        if(std::holds_alternative<VfxLinkPayload>(input.payload))
        {
            if(auto link=LinkRibbon(cooked,input,*effect))result.push_back(std::move(*link));
            continue;
        }
        const auto geometry = Geometry(input);
        if (!geometry) continue;
        const auto payload_kind = std::holds_alternative<VfxProjectilePayload>(input.payload)
            ? kProjectilePayload : kProjectilePathPayload;
        if (effect->payload_kind != payload_kind) continue;
        if (effect->sources.first > cooked.sources.size() ||
            effect->sources.count > cooked.sources.size() - effect->sources.first)
            continue;
        for (std::uint32_t source_index = effect->sources.first;
             source_index < effect->sources.first + effect->sources.count;
             ++source_index)
        {
            if (source_index >= cooked.sources.size()) break;
            const auto &source = cooked.sources[source_index];
            if (source.effect != effect->handle || source.type != kHistoryRibbon) continue;
            if (source.parameters.first > cooked.parameters.size() ||
                source.parameters.count > cooked.parameters.size() - source.parameters.first ||
                source.outputs.first > cooked.outputs.size() ||
                source.outputs.count > cooked.outputs.size() - source.outputs.first)
                continue;
            const auto source_params = std::span<const VfxParameterRecord>(
                cooked.parameters).subspan(source.parameters.first,
                                            source.parameters.count);
            const auto history_seconds = Parameter(source_params,
                                                   Hash32("history_seconds"));
            if (!history_seconds.present || !history_seconds.valid ||
                history_seconds.value <= 0.0f ||
                history_seconds.value > 62.0f / 60.0f)
                continue;

            VfxRibbonSourceInput ribbon;
            ribbon.owner_id = input.stable_id;
            ribbon.effect_handle = input.effect_handle;
            ribbon.source_id = source.stable_id;
            ribbon.stable_seed = input.stable_seed;
            ribbon.importance = effect->importance;
            ribbon.position = geometry->current;
            ribbon.previous_position = geometry->previous;
            ribbon.history_seconds = history_seconds.value;
            // Projectile presence has no predicted expiry. Use authored
            // seconds only to fade in a live head; if none exists, the
            // history window is the bounded fade-in interval and the ribbon
            // remains present while the owner record exists.
            const auto fade_seconds = effect->seconds > 0.0f
                ? effect->seconds : history_seconds.value;
            const auto live_alpha = fade_seconds > 0.0f
                ? std::clamp(input.elapsed_seconds / fade_seconds, 0.0f, 1.0f)
                : 1.0f;
            ribbon.start_alpha = input.normalized_age > 0.0f
                ? KnotAlpha(source, input.normalized_age)
                : source.knot_count == 4
                    ? std::clamp((live_alpha - source.knots[0]) /
                        std::max(source.knots[1] - source.knots[0], 0.0001f), 0.0f, 1.0f)
                    : KnotAlpha(source, live_alpha);

            const auto output_first = source.outputs.first;
            const auto output_last = output_first + source.outputs.count;
            for (std::uint32_t output_index = output_first;
                 output_index < output_last && output_index < cooked.outputs.size();
                 ++output_index)
            {
                const auto &output = cooked.outputs[output_index];
                if (output.source != source_index ||
                    (output.profile != kRibbonAdd && output.profile != kRibbonOit) ||
                    !AcceptedMotion(output.motion) ||
                    static_cast<std::uint32_t>(input.quality) < output.min_quality)
                    continue;
                const auto shape_camera = output.shape == Hash32("camera_facing_ribbon") ||
                                          output.shape == Hash32("dual_camera_ribbon");
                if (!shape_camera) continue;
                if (output.parameters.first > cooked.parameters.size() ||
                    output.parameters.count > cooked.parameters.size() -
                                                  output.parameters.first)
                    continue;
                const auto params = std::span<const VfxParameterRecord>(
                    cooked.parameters).subspan(output.parameters.first,
                                               output.parameters.count);
                VfxRibbonOutputInput translated;
                translated.profile = output.profile;
                translated.motion = output.motion;
                translated.color = {output.rgba[0], output.rgba[1], output.rgba[2],
                                    output.rgba[3]};
                translated.hdr = output.hdr;
                translated.gradient_row = output.gradient_row;
                const auto width_head = Parameter(params, Hash32("width_head"));
                const auto width_tail = Parameter(params, Hash32("width_tail"));
                const auto noise_warp = Parameter(params, Hash32("noise_warp"));
                const auto phase_offset = Parameter(params, Hash32("phase_offset"));
                if (!width_head.present || !width_head.valid ||
                    !width_tail.present || !width_tail.valid ||
                    (noise_warp.present && !noise_warp.valid) ||
                    (phase_offset.present && !phase_offset.valid))
                    continue;
                translated.width_head = width_head.value;
                translated.width_tail = width_tail.value;
                translated.noise_warp = noise_warp.present ? noise_warp.value : 0.0f;
                translated.phase_offset = phase_offset.present ? phase_offset.value : 0.0f;
                const auto coverage = Hash32("ribbon_dash_repeat");
                translated.dashed = output.coverage_ref == coverage;
                if (output.textures.first > cooked.texture_bindings.size() ||
                    output.textures.count > cooked.texture_bindings.size() -
                                                  output.textures.first)
                    continue;
                const auto texture_range = std::span<const VfxTextureBindingRecord>(
                    cooked.texture_bindings).subspan(output.textures.first,
                                                     output.textures.count);
                const auto detail = std::find_if(
                    texture_range.begin(), texture_range.end(),
                    [&](const auto &binding) { return IsRibbonDetailBinding(cooked, binding) &&
                                                       binding.slices.count != 0; });
                if (detail != texture_range.end())
                {
                    if (detail->slices.first > cooked.slice_indices.size() ||
                        detail->slices.count > cooked.slice_indices.size() -
                                                    detail->slices.first)
                        continue;
                    translated.detail_strength = detail->strength;
                    translated.detail_slice = cooked.slice_indices[
                        detail->slices.first +
                        (input.stable_seed % detail->slices.count)];
                    if (translated.detail_slice >= 8) continue;
                }
                if (!std::isfinite(translated.width_head) ||
                    !std::isfinite(translated.width_tail) ||
                    translated.width_head <= 0.0f || translated.width_tail < 0.0f)
                    continue;
                ribbon.outputs.push_back(translated);
            }
            if (!ribbon.outputs.empty()) result.push_back(std::move(ribbon));
        }
    }
    return result;
}

std::vector<VfxRibbonSourceInput> BuildVfxEventRibbonInputs(
    const VfxProgramData &cooked,
    std::span<const VfxEventInput> events,
    Tick current_tick)
{
    std::vector<VfxRibbonSourceInput> result;
    for (const auto &event : events)
    {
        const auto *effect = FindEffect(cooked, event.effect_handle);
        if (const auto *link = std::get_if<VfxLinkPayload>(&event.payload))
        {
            // SourceTargetPayload links have effect-specific authored recipes.
            const auto identity = effect == nullptr
                ? cooked.effect_lookup.end()
                : std::find_if(cooked.effect_lookup.begin(), cooked.effect_lookup.end(),
                    [&](const auto &candidate) { return candidate.handle == effect->handle; });
            if (effect && identity != cooked.effect_lookup.end() &&
                PickupCollectEffect(identity->effect_id))
            {
                if (std::count_if(cooked.effect_lookup.begin(),
                                  cooked.effect_lookup.end(),
                    [&](const auto &candidate) {
                        return candidate.handle == effect->handle ||
                               candidate.effect_id == identity->effect_id;
                    }) != 1)
                    continue;
                if (auto ribbon = PickupCollectRibbon(cooked, event, *effect,
                                                      *link, current_tick))
                    result.push_back(std::move(*ribbon));
                continue;
            }
            if (!effect || identity == cooked.effect_lookup.end() ||
                identity->effect_id != kArrowRainPullEffect ||
                effect->input_mode != 0 || effect->timing_kind != 0 ||
                effect->payload_kind != Hash32("SourceTargetPayload") ||
                !std::isfinite(effect->seconds) || effect->seconds <= 0.0f ||
                event.sequence == 0 ||
                current_tick < event.event_tick || link->source_id == 0 ||
                link->target_id == 0 || link->source_id != link->target_id)
                continue;
            const auto elapsed = static_cast<float>(current_tick - event.event_tick) /
                                 60.0f;
            if (!std::isfinite(elapsed) || elapsed < 0.0f ||
                elapsed >= effect->seconds)
                continue;
            VfxPersistentInput synthetic;
            synthetic.stable_id = event.sequence;
            synthetic.effect_handle = event.effect_handle;
            synthetic.payload = *link;
            synthetic.elapsed_seconds = elapsed;
            synthetic.normalized_age = elapsed / effect->seconds;
            synthetic.stable_seed = event.stable_seed;
            synthetic.quality = event.quality;
            if (auto ribbon = LinkRibbon(cooked, synthetic, *effect, true))
                result.push_back(std::move(*ribbon));
            continue;
        }
        const auto emitter = Emitter(event);
        const bool projectile_event =
            std::holds_alternative<VfxProjectilePayload>(event.payload);
        const auto identity = effect == nullptr
            ? cooked.effect_lookup.end()
            : std::find_if(cooked.effect_lookup.begin(), cooked.effect_lookup.end(),
                [&](const auto &candidate) { return candidate.handle == effect->handle; });
        if (effect && identity != cooked.effect_lookup.end() &&
            (identity->effect_id == kCommonPullEffect ||
             identity->effect_id == kRetreatMoveEffect ||
             identity->effect_id == kExplosivePrePullEffect))
        {
            if (auto ribbon = FixedEventRibbon(cooked, event, *effect,
                                               identity->effect_id, current_tick))
                result.push_back(std::move(*ribbon));
            continue;
        }
        if (!effect || effect->input_mode != 0 || !emitter ||
            effect->payload_kind != PayloadKind(event) ||
            !Finite(emitter->origin) || !Finite(emitter->direction) ||
            !std::isfinite(emitter->scale) || emitter->scale <= 0.0f ||
            effect->timing_kind != 0 || !std::isfinite(effect->seconds) ||
            effect->seconds <= 0.0f || current_tick < event.event_tick ||
            (projectile_event && (identity == cooked.effect_lookup.end() ||
                                  identity->effect_id != kChargedImpactEffect)))
            continue;
        if (effect->sources.first > cooked.sources.size() ||
            effect->sources.count > cooked.sources.size() - effect->sources.first)
            continue;
        const auto direction = NormalizeHorizontal(emitter->direction);
        const auto elapsed = current_tick >= event.event_tick
            ? static_cast<float>(current_tick - event.event_tick) / 60.0f : 0.0f;
        for (std::uint32_t source_index = effect->sources.first;
             source_index < effect->sources.first + effect->sources.count;
             ++source_index)
        {
            const auto &source = cooked.sources[source_index];
            if (source.effect != effect->handle || source.type != kHistoryRibbon ||
                (projectile_event && source.stable_id != kAxialAfterlineSource) ||
                source.parameters.first > cooked.parameters.size() ||
                source.parameters.count > cooked.parameters.size() - source.parameters.first ||
                source.outputs.first > cooked.outputs.size() ||
                source.outputs.count > cooked.outputs.size() - source.outputs.first)
                continue;
            const auto source_params = std::span<const VfxParameterRecord>(
                cooked.parameters).subspan(source.parameters.first,
                                            source.parameters.count);
            const auto history = Parameter(source_params, Hash32("history_seconds"));
            if (!history.present || !history.valid || history.value <= 0.0f ||
                history.value > 62.0f / 60.0f ||
                (source.knot_count != 2 && source.knot_count != 4))
                continue;
            bool valid_knots = true;
            for (std::uint32_t knot = 0; knot < source.knot_count; ++knot)
            {
                if (!std::isfinite(source.knots[knot]) || source.knots[knot] < 0.0f ||
                    source.knots[knot] > 1.0f ||
                    (knot != 0 && source.knots[knot] <= source.knots[knot - 1]))
                {
                    valid_knots = false;
                    break;
                }
            }
            if (!valid_knots) continue;
            const auto start_seconds = effect->seconds * source.knots[0];
            const auto end_seconds = effect->seconds *
                source.knots[source.knot_count - 1];
            if (!std::isfinite(start_seconds) || !std::isfinite(end_seconds) ||
                end_seconds <= start_seconds || elapsed < start_seconds || elapsed >= end_seconds)
                continue;
            const auto previous_elapsed = std::max(
                start_seconds, elapsed - 1.0f / 60.0f);
            const auto arc_degrees_key = Hash32("arc_deg");
            const auto width_head_key = Hash32("width_head");
            const auto width_tail_key = Hash32("width_tail");
            VfxRibbonSourceInput ribbon;
            ribbon.owner_id = event.sequence;
            ribbon.effect_handle = event.effect_handle;
            ribbon.source_id = source.stable_id;
            ribbon.stable_seed = event.stable_seed;
            ribbon.importance = effect->importance;
            ribbon.history_seconds = history.value;
            ribbon.start_alpha = KnotAlpha(
                source, std::clamp(elapsed / effect->seconds, 0.0f, 1.0f));
            float arc_degrees = 35.0f;
            for (std::uint32_t output_index = source.outputs.first;
                 output_index < source.outputs.first + source.outputs.count;
                 ++output_index)
            {
                const auto &output = cooked.outputs[output_index];
                const bool output_axial = output.motion == VfxMotionKind::ShortLineIncomingVelocity &&
                                          output.shape == kImpactAxisRibbon;
                if (projectile_event && (!output_axial ||
                                         output.profile != kRibbonAdd))
                    continue;
                if (!projectile_event && output_axial)
                    continue;
                if (output.source != source_index ||
                    (output.profile != kRibbonAdd && output.profile != kRibbonOit) ||
                    (!output_axial && (output.motion != VfxMotionKind::CurvedRecoilArc ||
                                output.shape != Hash32("short_arc_ribbon"))) ||
                    static_cast<std::uint32_t>(event.quality) < output.min_quality ||
                    output.parameters.first > cooked.parameters.size() ||
                    output.parameters.count > cooked.parameters.size() - output.parameters.first)
                    continue;
                const auto params = std::span<const VfxParameterRecord>(
                    cooked.parameters).subspan(output.parameters.first,
                                               output.parameters.count);
                const auto arc_param = Parameter(params, arc_degrees_key);
                const auto authored_head = Parameter(params, width_head_key);
                const auto authored_tail = Parameter(params, width_tail_key);
                if (output_axial && ((!authored_head.present || !authored_head.valid || authored_head.value <= 0.0f) ||
                              (!authored_tail.present || !authored_tail.valid || authored_tail.value < 0.0f)))
                    continue;
                if (output_axial &&
                    (!std::isfinite(output.rgba[0]) || !std::isfinite(output.rgba[1]) ||
                     !std::isfinite(output.rgba[2]) || !std::isfinite(output.rgba[3]) ||
                     output.rgba[3] < 0.0f || output.rgba[3] > 1.0f ||
                     !std::isfinite(output.hdr) || output.hdr <= 0.0f ||
                     output.gradient_row >= 68u))
                    continue;
                if (arc_param.present &&
                    (!arc_param.valid || arc_param.value <= 0.0f || arc_param.value > 360.0f))
                    continue;
                if (arc_param.present) arc_degrees = arc_param.value;
                VfxRibbonOutputInput translated;
                translated.profile = output.profile;
                translated.motion = output.motion;
                translated.color = {output.rgba[0], output.rgba[1], output.rgba[2], output.rgba[3]};
                translated.hdr = output.hdr;
                translated.gradient_row = output.gradient_row;
                translated.width_head = output_axial && authored_head.present ? authored_head.value : 0.035f * emitter->scale;
                translated.width_tail = output_axial && authored_tail.present ? authored_tail.value : 0.008f * emitter->scale;
                translated.dashed = output.coverage_ref == Hash32("ribbon_dash_repeat");
                if (output.textures.first > cooked.texture_bindings.size() ||
                    output.textures.count > cooked.texture_bindings.size() - output.textures.first)
                    continue;
                const auto texture_range = std::span<const VfxTextureBindingRecord>(
                    cooked.texture_bindings).subspan(output.textures.first,
                                                     output.textures.count);
                const auto detail = std::find_if(texture_range.begin(), texture_range.end(),
                    [&](const auto &binding) { return IsRibbonDetailBinding(cooked, binding) &&
                                                       binding.slices.count != 0; });
                if (detail != texture_range.end())
                {
                    if (detail->slices.first > cooked.slice_indices.size() ||
                        detail->slices.count > cooked.slice_indices.size() - detail->slices.first ||
                        !std::isfinite(detail->strength) || detail->strength < 0.0f ||
                        detail->strength > 1.0f)
                        continue;
                    translated.detail_strength = detail->strength;
                    translated.detail_slice = cooked.slice_indices[
                        detail->slices.first + (event.stable_seed % detail->slices.count)];
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
                    const auto line_length = emitter->speed * history.value;
                    if (!std::isfinite(line_length) || line_length <= 0.0f)
                        continue;
                    ribbon.position = emitter->origin;
                    ribbon.previous_position = Add3(
                        emitter->origin, Scale3(emitter->direction, -line_length));
                }
                else
                {
                    ribbon.position = make_position(elapsed);
                    ribbon.previous_position = make_position(previous_elapsed);
                }
                result.push_back(std::move(ribbon));
            }
        }
    }
    return result;
}
}
