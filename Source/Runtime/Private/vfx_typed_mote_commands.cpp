#include "vfx_typed_mote_commands.hpp"
#include "vfx_workspace_size.hpp"
#include <hs/render_data/render_snapshot_values.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <iterator>
#include <numbers>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace hs::runtime_detail
{
namespace
{
template<class T> bool Range(VfxRange r, std::span<const T> v)
{ return r.first <= v.size() && r.count <= v.size() - r.first; }
bool Finite(Float3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
std::uint32_t Mix(std::uint32_t v)
{ v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; return v ^ (v >> 16); }
float Random(std::uint32_t seed, std::uint32_t lane)
{ return static_cast<float>(Mix(seed ^ Mix(lane + 0x9e3779b9u)) >> 8) * (1.0f / 16777216.0f); }
struct Recipe { VfxImpactShape kind; float size, radius, speed, rate; };
constexpr Recipe recipes[]{
    {VfxImpactShape::LeafMote, .045f, .2f, .45f, 2},
    {VfxImpactShape::SmallMote, .025f, .08f, .1f, 0},
    {VfxImpactShape::DiamondMote, .035f, .15f, .6f, 5}
};
const VfxMoteSourceRecipe *EffectRecipe(const VfxImage &image, const VfxEffectRecord &effect,
                                      VfxEffectHandle handle)
{
    if (effect.handle != handle || !Range(effect.sources, image.mote_source_recipes) ||
        effect.sources.count == 0) return nullptr;
    const auto &recipe = image.mote_source_recipes[effect.sources.first];
    return recipe.effect_handle == effect.handle && recipe.effect_routes != 0
        ? &recipe : nullptr;
}
VfxMoteRowQuota *QuotaForSource(const VfxImage &image, std::size_t source_index,
                                VfxMoteRowQuota *quota) noexcept
{
    if (!quota || source_index >= image.mote_source_recipes.size()) return nullptr;
    return image.mote_source_recipes[source_index].cosmetic ? quota : nullptr;
}
bool AdmitMoteRow(VfxMoteRowQuota *quota) noexcept
{
    if (!quota) return true;
    if (quota->requested_rows != std::numeric_limits<std::size_t>::max())
        ++quota->requested_rows;
    if (quota->remaining_rows != 0)
    {
        --quota->remaining_rows;
        if (quota->admitted_rows != std::numeric_limits<std::size_t>::max())
            ++quota->admitted_rows;
        return true;
    }
    return false;
}
using vfx_retained_size::ReserveAppend;
void ReserveAppendWithinQuota(std::vector<VfxFlashSpawnInput> &values, std::size_t candidate_rows,
                              const VfxMoteRowQuota *quota,
                              std::size_t already_admitted_rows = 0)
{
    if (!quota) return;
    const auto available = values.max_size() - values.size();
    const auto reserved_allowance = std::min(available, quota->remaining_rows);
    const auto admitted_allowance = std::min(available - reserved_allowance,
                                             already_admitted_rows);
    const auto allowance = reserved_allowance + admitted_allowance;
    const auto requested = std::min(candidate_rows, allowance);
    if (requested == 0 || requested <= values.capacity() - values.size()) return;
    const auto capacity = values.capacity();
    const auto grown = capacity + std::min(values.max_size() - capacity, capacity / 2 + 1);
    const auto ceiling = values.size() + allowance;
    values.reserve(std::min(ceiling, std::max(values.size() + requested, grown)));
}
template<class Input>
bool DecodeMask(const VfxImage &image, std::size_t output_index, const Input &input,
                VfxFlashSpawnInput &out)
{
    const auto &program = image.program;
    if (output_index >= image.mote_output_recipes.size()) return false;
    const auto &recipe = image.mote_output_recipes[output_index];
    const auto quality = static_cast<std::uint32_t>(input.quality);
    if (quality > 2 ||
        !(recipe.mask_quality_mask & (1u << quality))) return false;
    if (recipe.mask_present_mask & (1u << quality))
    {
        const auto slices = recipe.mask_slices[quality];
        if (out.mask_slice != 0xffffffffu)
            return false;
        const auto choices = program.slice_indices.subspan(slices.first, slices.count);
        out.mask_slice = choices[input.stable_seed % choices.size()];
        out.mask_strength = recipe.mask_strengths[quality];
    }
    return true;
}
constexpr float kPersistentMoteLifetimeTicks = 39.0f;

struct PersistentMoteParameters
{
    float size{};
    std::uint32_t curve_row{0xffffffffu};
};

std::optional<PersistentMoteParameters> EvaluatePersistentMoteParameters(
    const VfxMoteOutputRecipe &recipe, float footprint)
{
    if (!std::isfinite(footprint) || footprint <= 0.0f) return {};
    if (!recipe.persistent_params_valid) return {};
    const float size = recipe.persistent_size == 0.0f ? .05f * footprint : recipe.persistent_size;
    return std::isfinite(size) && size > 0.0f ?
        std::optional(PersistentMoteParameters{size, recipe.persistent_curve_row}) : std::nullopt;
}

bool FiniteTransform(const VfxTransform &transform)
{
    return std::all_of(transform.begin(), transform.end(),
                       [](float value) { return std::isfinite(value); });
}

Float3 TransformOffset(const VfxTransform &transform, Float3 local)
{
    return {transform[0] * local.x + transform[4] * local.y + transform[8] * local.z,
            transform[1] * local.x + transform[5] * local.y + transform[9] * local.z,
            transform[2] * local.x + transform[6] * local.y + transform[10] * local.z};
}

std::optional<std::int64_t> PersistentBirthTick(double start_ticks,
                                                float rate,
                                                std::uint64_t ordinal)
{
    const double value = std::ceil(static_cast<double>(start_ticks) +
                                   static_cast<double>(ordinal) * 60.0 /
                                       static_cast<double>(rate) - 1.0e-6);
    if (!std::isfinite(value) || value < 0.0 ||
        value > static_cast<double>(std::numeric_limits<std::int64_t>::max()))
        return {};
    return static_cast<std::int64_t>(value);
}
struct SparkParams { std::uint32_t count{}; float cone_degrees{}, drag{}; };
Float3 Cross(Float3 a, Float3 b)
{ return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
Float3 SparkDirection(Float3 velocity, float cone_degrees, std::uint32_t seed)
{
    const float speed = std::sqrt(velocity.x*velocity.x + velocity.y*velocity.y + velocity.z*velocity.z);
    const Float3 backward{-velocity.x/speed, -velocity.y/speed, -velocity.z/speed};
    const Float3 reference = std::abs(backward.y) < .9f ? Float3{0,1,0} : Float3{1,0,0};
    Float3 right = Cross(reference, backward);
    const float right_length = std::sqrt(right.x*right.x + right.y*right.y + right.z*right.z);
    right = {right.x/right_length, right.y/right_length, right.z/right_length};
    const Float3 up = Cross(backward, right);
    const float theta = cone_degrees * (std::numbers::pi_v<float>/180.0f) *
        std::sqrt(Random(seed, 0));
    const float azimuth = 2.0f * std::numbers::pi_v<float> * Random(seed, 1);
    const float axial = std::cos(theta), radial = std::sin(theta);
    return {backward.x*axial + radial*(right.x*std::cos(azimuth) + up.x*std::sin(azimuth)),
            backward.y*axial + radial*(right.y*std::cos(azimuth) + up.y*std::sin(azimuth)),
            backward.z*axial + radial*(right.z*std::cos(azimuth) + up.z*std::sin(azimuth))};
}

template<class Input>
void AppendRateMotes(const VfxImage &image, const Input &owner,
                          Float3 position, float extent, const VfxEffectRecord &effect,
                          std::uint64_t effect_id, double current_ticks,
                          double duration_ticks, std::vector<VfxFlashSpawnInput> &result,
                          VfxMoteRowQuota *quota)
{
    constexpr double lifetime_ticks = kPersistentMoteLifetimeTicks;
    constexpr double lifetime_seconds = lifetime_ticks / 60.0;
    for (std::size_t source_index = effect.sources.first;
         source_index < static_cast<std::size_t>(effect.sources.first) + effect.sources.count;
         ++source_index)
    {
        if (source_index >= image.mote_source_recipes.size()) continue;
        const auto &source_recipe = image.mote_source_recipes[source_index];
        if (!source_recipe.rate_route_valid || source_recipe.effect_handle != effect.handle) continue;
        auto *source_quota = QuotaForSource(image, source_index, quota);

        const double start_ticks = 0.0;
        const double end_ticks = duration_ticks;
        if (current_ticks < start_ticks || static_cast<double>(current_ticks) >= end_ticks)
            continue;
        const double first_value =
            (static_cast<double>(current_ticks) - lifetime_ticks - start_ticks) * 3.0 / 60.0;
        const double last_value =
            (std::min(static_cast<double>(current_ticks), end_ticks) - start_ticks) * 3.0 / 60.0;
        constexpr double kLastOrdinal =
            static_cast<double>(std::numeric_limits<std::uint32_t>::max() - 2u);
        if (!std::isfinite(first_value) || !std::isfinite(last_value) ||
            last_value < 0.0 || first_value > kLastOrdinal || last_value > kLastOrdinal)
            continue;
        const auto first_ordinal = first_value <= 0.0
            ? std::uint64_t{0}
            : static_cast<std::uint64_t>(std::floor(first_value));
        const auto last_ordinal = static_cast<std::uint64_t>(last_value + 2.0);
        const auto source_seed = Mix(owner.stable_seed ^
            Mix(static_cast<std::uint32_t>(effect_id)) ^
            Mix(static_cast<std::uint32_t>(effect_id >> 32)) ^
            Mix(source_recipe.stable_id));

        const auto output_index = source_recipe.output_range.first;
        if (output_index >= image.mote_output_recipes.size()) continue;
        const auto &output_recipe = image.mote_output_recipes[output_index];
        if (!output_recipe.rate_route_valid ||
            output_recipe.min_quality > static_cast<std::uint32_t>(owner.quality))
            continue;
        const auto params = EvaluatePersistentMoteParameters(output_recipe, extent);
        if (!params)
            continue;

        ReserveAppendWithinQuota(result,
            static_cast<std::size_t>(last_ordinal - first_ordinal + 1), source_quota);
        for (std::uint64_t ordinal = first_ordinal; ordinal <= last_ordinal; ++ordinal)
        {
            const auto birth = PersistentBirthTick(start_ticks, 3.0f, ordinal);
            if (!birth || *birth > current_ticks || static_cast<double>(*birth) >= end_ticks)
                continue;
            const double age_ticks = static_cast<double>(current_ticks - *birth);
            if (age_ticks < 0.0 || age_ticks >= lifetime_ticks)
                continue;
            if (!AdmitMoteRow(source_quota)) continue;

            const auto mote_seed = Mix(source_seed ^
                Mix(static_cast<std::uint32_t>(ordinal + 1u)));
            const float angle = 2.0f * std::numbers::pi_v<float> * Random(mote_seed, 0);
            const float local_radius = extent * (0.45f + 0.4f * Random(mote_seed, 1));
            const Float3 offset{std::cos(angle) * local_radius,
                                extent * 0.08f,
                                std::sin(angle) * local_radius};
            const float inward_scale = static_cast<float>(1.0 / lifetime_seconds);
            const Float3 velocity{-offset.x * inward_scale,
                                  -offset.y * inward_scale,
                                  -offset.z * inward_scale};
            const float horizontal = std::hypot(velocity.x, velocity.z);
            if (!Finite(offset) || !Finite(velocity) || !std::isfinite(horizontal) ||
                horizontal <= 0.0001f)
                continue;

            VfxFlashSpawnInput mote;
            mote.position = position;
            mote.shape = VfxImpactShape::SoftDisc;
            mote.size = params->size;
            mote.lifetime = static_cast<float>(lifetime_seconds);
            mote.normalized_age = static_cast<float>(age_ticks / lifetime_ticks);
            mote.stable_seed = mote_seed;
            mote.event_tick = {};
            mote.direction = {velocity.x / horizontal, 0.0f, velocity.z / horizontal};
            mote.initial_offset = offset;
            mote.initial_velocity = velocity;
            mote.appearance = std::cref(output_recipe.appearance);
            mote.curve_row = params->curve_row;
            if (Finite(mote.position) && DecodeMask(image, output_index, owner, mote))
                result.push_back(mote);
            if (ordinal == std::numeric_limits<std::uint64_t>::max()) break;
        }
    }
}

void AppendPersistentTrapMotes(const VfxImage &image,
                               const VfxPersistentInput &owner,
                               const VfxCirclePayload &circle,
                               std::vector<VfxFlashSpawnInput> &result,
                               VfxMoteRowQuota *quota)
{
    const auto &program = image.program;
    if (owner.stable_id == 0 || owner.effect_handle == 0 ||
        owner.effect_handle > program.effects.size() ||
        static_cast<std::uint32_t>(owner.quality) > 2 ||
        !std::isfinite(owner.elapsed_seconds) || owner.elapsed_seconds < 0.0f ||
        !std::isfinite(owner.normalized_age) || owner.normalized_age < 0.0f ||
        owner.normalized_age >= 1.0f ||
        !Finite(circle.center) || !Finite(circle.direction) ||
        !std::isfinite(circle.radius) || circle.radius <= 0.0f ||
        !std::isfinite(circle.inner_radius) || circle.inner_radius != 0.0f ||
        !std::isfinite(circle.lifetime01) || circle.lifetime01 < 0.0f ||
        circle.lifetime01 >= 1.0f ||
        std::abs(circle.lifetime01 - owner.normalized_age) > 0.00001f ||
        (owner.normalized_age == 0.0f && owner.elapsed_seconds > 0.0f))
        return;

    const auto &effect = program.effects[owner.effect_handle - 1];
    const auto *lookup = EffectRecipe(image, effect, owner.effect_handle);
    const auto phase = static_cast<PersistentVfxKind>(owner.source_visual_kind);
    const auto routes = phase == PersistentVfxKind::TrapPending
        ? kVfxMoteTrapPending | kVfxMoteTrapRetreat
        : phase == PersistentVfxKind::TrapArmed ? kVfxMoteTrapArmed | kVfxMoteTrapRetreat : 0u;
    if (!lookup || !(lookup->effect_routes & routes))
        return;

    const double elapsed_ticks = owner.elapsed_ticks >= 0.0 && std::isfinite(owner.elapsed_ticks)
        ? owner.elapsed_ticks : static_cast<double>(owner.elapsed_seconds) * 60.0;
    if (!std::isfinite(elapsed_ticks) ||
        elapsed_ticks > static_cast<double>(std::numeric_limits<std::int64_t>::max() / 2))
        return;
    const double current_ticks = elapsed_ticks;
    const double duration_ticks = owner.normalized_age > 0.0f
        ? elapsed_ticks / static_cast<double>(owner.normalized_age)
        : std::numeric_limits<double>::infinity();
    if (owner.normalized_age > 0.0f &&
        (!std::isfinite(duration_ticks) || duration_ticks <= 0.0))
        return;

    AppendRateMotes(image, owner, circle.center, circle.radius, effect, lookup->effect_id,
                          current_ticks, duration_ticks, result, quota);
}
}

void PrepareVfxTypedMoteBursts(const VfxImage &image, VfxEventView inputs,
    std::span<const Sequence> consumed_event_sequences,
    std::vector<VfxPreparedMoteBurst> &prepared)
{
    const auto &program = image.program;
    prepared.clear();
    for (const auto &input : inputs)
    {
        if (std::ranges::binary_search(consumed_event_sequences, input.sequence)) continue;
        if (input.effect_handle == 0 || input.effect_handle > program.effects.size() || static_cast<std::uint32_t>(input.quality) > 2) continue;
        const auto &effect = program.effects[input.effect_handle - 1];
        const auto *lookup = EffectRecipe(image, effect, input.effect_handle);
        if (!lookup) continue;
        if (const auto *entity = std::get_if<VfxEntityPayload>(&input.payload))
        {
            if (!(lookup->effect_routes & kVfxMoteResidualEntity) || input.sequence == 0 ||
                entity->render_instance_id == 0 || !std::isfinite(entity->footprint_radius) ||
                entity->footprint_radius <= 0) continue;
            const Float3 anchor{input.world_transform[12], input.world_transform[13],
                                input.world_transform[14]};
            if (!Finite(anchor)) continue;
            for (std::uint32_t si = effect.sources.first; si < effect.sources.first + effect.sources.count; ++si)
            {
                if (si >= image.mote_source_recipes.size()) continue;
                const auto &source_recipe = image.mote_source_recipes[si];
                if (!source_recipe.residual_route_valid || source_recipe.effect_handle != effect.handle) continue;
                const auto count = source_recipe.count;
                const auto speed = source_recipe.speed;
                for (std::size_t output_index = source_recipe.output_range.first;
                     output_index < static_cast<std::size_t>(source_recipe.output_range.first) + source_recipe.output_range.count;
                     ++output_index)
                {
                    if (output_index >= image.mote_output_recipes.size()) continue;
                    const auto &output_recipe = image.mote_output_recipes[output_index];
                    if (!output_recipe.residual_route_valid ||
                        !output_recipe.output_color_valid ||
                        output_recipe.min_quality > static_cast<std::uint32_t>(input.quality)) continue;
                    VfxFlashSpawnInput base;
                    base.position = anchor;
                    base.shape = VfxImpactShape::SmallMote;
                    base.size = .025f;
                    base.lifetime = effect.seconds;
                    base.event_tick = input.event_tick;
                    base.appearance = std::cref(output_recipe.appearance);
                    if (!DecodeMask(image, output_index, input, base)) continue;
                    prepared.push_back({base, count, count,
                        input.stable_seed ^ Mix(static_cast<std::uint32_t>(lookup->effect_id)) ^
                            Mix(static_cast<std::uint32_t>(lookup->effect_id >> 32)) ^
                            Mix(source_recipe.stable_id),
                        effect.importance, 1.0f, 0.0f, speed, speed * effect.seconds,
                        VfxPreparedMoteSampler::Residual,
                        source_recipe.cosmetic});
                }
            }
            continue;
        }
        Float3 position{}; float scale = 1;
        if (const auto *point = std::get_if<VfxPointPayload>(&input.payload))
        {
            if (!(lookup->effect_routes & kVfxMoteBurstPoint)) continue;
            position = point->position; scale = point->authored_scale;
        }
        else if (std::holds_alternative<VfxContextPayload>(input.payload))
        {
            if (!(lookup->effect_routes & kVfxMoteBurstContext)) continue;
            position = {input.world_transform[12], input.world_transform[13], input.world_transform[14]};
        }
        else continue;
        if (!Finite(position) || !std::isfinite(scale) || scale <= 0) continue;
        for (std::uint32_t si = effect.sources.first; si < effect.sources.first + effect.sources.count; ++si)
        {
            if (si >= image.mote_source_recipes.size()) continue;
            const auto &source_recipe = image.mote_source_recipes[si];
            if (!source_recipe.common_route_valid || source_recipe.effect_handle != effect.handle) continue;
            const auto source_seed = Mix(input.stable_seed ^ Mix(static_cast<std::uint32_t>(lookup->effect_id)) ^
                Mix(static_cast<std::uint32_t>(lookup->effect_id >> 32)) ^ Mix(source_recipe.stable_id));
            for (std::size_t output_index = source_recipe.output_range.first;
                 output_index < static_cast<std::size_t>(source_recipe.output_range.first) + source_recipe.output_range.count;
                 ++output_index)
            {
                if (output_index >= image.mote_output_recipes.size()) continue;
                const auto &output_recipe = image.mote_output_recipes[output_index];
                if (output_recipe.common_recipe_index >= std::size(recipes) ||
                    !output_recipe.output_color_valid ||
                    output_recipe.min_quality > static_cast<std::uint32_t>(input.quality)) continue;
                const auto &recipe = recipes[output_recipe.common_recipe_index];
                VfxFlashSpawnInput base;
                base.position = position; base.shape = recipe.kind; base.size = recipe.size * scale;
                base.delay = source_recipe.knots[0] * effect.seconds; base.lifetime = (source_recipe.knots[1] - source_recipe.knots[0]) * effect.seconds;
                base.event_tick = input.event_tick; base.appearance = std::cref(output_recipe.appearance);
                base.drag = source_recipe.drag; base.orbit_rate = recipe.rate;
                base.orbit_radius = (recipe.kind == VfxImpactShape::LeafMote ? source_recipe.curl : recipe.kind == VfxImpactShape::SmallMote ? 0.0f : .15f) * scale;
                if (!std::isfinite(base.size) || base.size <= 0 || !std::isfinite(base.delay) || !std::isfinite(base.lifetime) || base.lifetime <= 0 ||
                    !std::isfinite(base.orbit_radius) || !DecodeMask(image, output_index, input, base)) continue;
                prepared.push_back({base, source_recipe.count, source_recipe.count, source_seed,
                    effect.importance, scale, recipe.radius, recipe.speed, 0.0f,
                    VfxPreparedMoteSampler::Common,
                    source_recipe.cosmetic});
            }
        }
    }
}

VfxMoteBurstSelection SelectVfxPreparedMoteBursts(
    std::vector<VfxPreparedMoteBurst> &prepared, std::size_t max_decorative_rows,
    std::vector<std::size_t> &order_scratch)
{
    const auto checked_add = [](std::size_t value, std::size_t amount) {
        if (amount > std::numeric_limits<std::size_t>::max() - value)
            throw std::length_error("VFX mote fanout count overflow");
        return value + amount;
    };
    VfxMoteBurstSelection selection;
    order_scratch.clear();
    for (auto &burst : prepared)
    {
        burst.admitted_count = burst.count;
        if (burst.cosmetic)
            selection.requested_decorative_rows = checked_add(
                selection.requested_decorative_rows, burst.count);
        else selection.required_rows = checked_add(selection.required_rows, burst.count);
    }
    if (selection.requested_decorative_rows <= max_decorative_rows)
    {
        selection.admitted_decorative_rows = selection.requested_decorative_rows;
        return selection;
    }
    for (std::size_t index = 0; index < prepared.size(); ++index)
        if (prepared[index].cosmetic) order_scratch.push_back(index);
    std::sort(order_scratch.begin(), order_scratch.end(), [&](std::size_t left, std::size_t right) {
        if (prepared[left].importance != prepared[right].importance)
            return prepared[left].importance > prepared[right].importance;
        return left < right;
    });
    auto remaining = max_decorative_rows;
    for (const auto index : order_scratch)
    {
        auto &burst = prepared[index];
        const auto admitted = std::min(burst.count, remaining);
        burst.admitted_count = admitted;
        selection.admitted_decorative_rows = checked_add(selection.admitted_decorative_rows, admitted);
        remaining -= admitted;
    }
    return selection;
}

void AppendVfxPreparedMoteBursts(std::span<const VfxPreparedMoteBurst> prepared,
    std::vector<VfxFlashSpawnInput> &result,
    std::vector<Sequence> *emitted_status_sequences)
{
    const auto checked_add = [](std::size_t value, std::size_t amount) {
        if (amount > std::numeric_limits<std::size_t>::max() - value)
            throw std::length_error("VFX mote append count overflow");
        return value + amount;
    };
    std::size_t append_count{};
    std::size_t status_sequence_count{};
    for (const auto &burst : prepared)
    {
        if (burst.admitted_count > burst.count)
            throw std::length_error("VFX mote admission exceeds its prepared burst");
        append_count = checked_add(append_count, burst.admitted_count);
        if (burst.sampler == VfxPreparedMoteSampler::StatusTick && emitted_status_sequences)
            status_sequence_count = checked_add(status_sequence_count, burst.admitted_count);
    }
    if (append_count > result.max_size() - result.size() ||
        (emitted_status_sequences &&
         status_sequence_count > emitted_status_sequences->max_size() - emitted_status_sequences->size()))
        throw std::length_error("VFX mote append exceeds vector capacity");
    const auto required = result.size() + append_count;
    if (required > result.capacity()) result.reserve(required);
    static_assert(std::is_nothrow_copy_constructible_v<VfxFlashSpawnInput>);
    static_assert(std::is_nothrow_copy_constructible_v<Sequence>);
    if (emitted_status_sequences && status_sequence_count != 0)
        emitted_status_sequences->reserve(emitted_status_sequences->size() + status_sequence_count);
    for (const auto &burst : prepared)
    {
        for (std::size_t ordinal = 0; ordinal < burst.admitted_count; ++ordinal)
        {
            auto mote = burst.base;
            mote.stable_seed = Mix(burst.seed_base ^ Mix(static_cast<std::uint32_t>(ordinal + 1)));
            if (burst.sampler == VfxPreparedMoteSampler::Residual)
            {
                const auto phase = Random(mote.stable_seed, 0) * 2 * std::numbers::pi_v<float>;
                const Float3 direction{std::cos(phase), 0, std::sin(phase)};
                mote.initial_offset = {direction.x * burst.residual_reach, 0,
                                       direction.z * burst.residual_reach};
                mote.initial_velocity = {-direction.x * burst.speed, 0,
                                         -direction.z * burst.speed};
                if (Finite(mote.initial_offset) && Finite(mote.initial_velocity)) result.push_back(mote);
                continue;
            }
            // Preserve the source ordinal and sampling order used by the legacy emitter.
            const auto phase = Random(mote.stable_seed, 0) * 2 * std::numbers::pi_v<float>;
            const float radius = burst.radius * burst.scale * (.35f + .65f * Random(mote.stable_seed, 1));
            mote.initial_offset = {std::cos(phase) * radius, 0, std::sin(phase) * radius};
            const float velocity_phase = Random(mote.stable_seed, 2) * 2 * std::numbers::pi_v<float>;
            const float horizontal_speed = Random(mote.stable_seed, 3) * .08f * burst.scale;
            mote.initial_velocity = {std::cos(velocity_phase) * horizontal_speed, burst.speed * burst.scale,
                                     std::sin(velocity_phase) * horizontal_speed};
            if (burst.sampler == VfxPreparedMoteSampler::Common)
                mote.orbit_phase = phase;
            if (Finite(mote.initial_offset) && Finite(mote.initial_velocity))
            {
                result.push_back(mote);
                if (burst.sampler == VfxPreparedMoteSampler::StatusTick && emitted_status_sequences)
                    emitted_status_sequences->push_back(burst.event_sequence);
            }
        }
    }
}

void AppendVfxTypedMoteCommands(const VfxImage &image, VfxEventView inputs,
    std::span<const Sequence> consumed_event_sequences,
    std::vector<VfxFlashSpawnInput> &result)
{
    std::vector<VfxPreparedMoteBurst> prepared;
    std::vector<std::size_t> order_scratch;
    PrepareVfxTypedMoteBursts(image, inputs, consumed_event_sequences, prepared);
    (void)SelectVfxPreparedMoteBursts(prepared, std::numeric_limits<std::size_t>::max(), order_scratch);
    AppendVfxPreparedMoteBursts(prepared, result);
}
std::vector<VfxFlashSpawnInput> BuildVfxTypedMoteCommands(const VfxImage &image, VfxEventView inputs)
{
    std::vector<VfxFlashSpawnInput> result;
    AppendVfxTypedMoteCommands(image, inputs, {}, result);
    return result;
}
void PrepareVfxOwnedStatusTickMoteBursts(
    const VfxImage &image, VfxEventView events,
    std::span<const VfxPersistentInput> owners,
    std::vector<VfxPreparedMoteBurst> &prepared)
{
    const auto &program = image.program;
    prepared.clear();
    for (std::size_t event_index = 0; event_index < events.size(); ++event_index)
    {
        const auto &event = events[event_index];
        if (event.effect_handle == 0 || event.effect_handle > program.effects.size() ||
            event.sequence == 0 || event.geometry_owner_id == 0 || event.geometry_owner_id >= (1ull << 60) ||
            event.status_episode_generation == 0 || static_cast<std::uint32_t>(event.quality) > 2 ||
            !std::holds_alternative<VfxPointPayload>(event.payload)) continue;
        const auto &tick = program.effects[event.effect_handle - 1];
        const auto *tick_recipe = EffectRecipe(image, tick, event.effect_handle);
        if (!tick_recipe || !(tick_recipe->effect_routes & (kVfxMoteBleedTick | kVfxMoteBurnTick))) continue;
        const auto status_route = tick_recipe->effect_routes & kVfxMoteBleedTick
            ? kVfxMoteBleedOwner : kVfxMoteBurnOwner;
        const auto render_id = (2ull << 60) | event.geometry_owner_id;
        const auto event_plan_begin = prepared.size();
        for (const auto &owner : owners)
        {
            const auto *entity = std::get_if<VfxEntityPayload>(&owner.payload);
            if (!entity || owner.stable_id == 0 || owner.effect_handle == 0 ||
                owner.effect_handle > program.effects.size() ||
                owner.status_episode_generation != event.status_episode_generation ||
                entity->render_instance_id != render_id || !FiniteTransform(owner.current_transform) ||
                !std::isfinite(entity->footprint_radius) || entity->footprint_radius <= 0 ||
                !std::isfinite(owner.normalized_age) || owner.normalized_age < 0 || owner.normalized_age >= 1 ||
                entity->lifetime01 != owner.normalized_age || static_cast<std::uint32_t>(owner.quality) > 2)
                continue;
            const auto &status = program.effects[owner.effect_handle - 1];
            const auto *status_recipe = EffectRecipe(image, status, owner.effect_handle);
            if (!status_recipe || !(status_recipe->effect_routes & status_route)) continue;
            const Float3 anchor{owner.current_transform[12], owner.current_transform[13], owner.current_transform[14]};
            for (std::size_t si = status.sources.first; si < static_cast<std::size_t>(status.sources.first) + status.sources.count; ++si)
            {
                if (si >= image.mote_source_recipes.size()) continue;
                const auto &source_recipe = image.mote_source_recipes[si];
                if (!source_recipe.tick_route_valid || source_recipe.effect_handle != status.handle) continue;
                for (std::size_t output_index = source_recipe.output_range.first;
                     output_index < static_cast<std::size_t>(source_recipe.output_range.first) + source_recipe.output_range.count;
                     ++output_index)
                {
                    if (output_index >= image.mote_output_recipes.size()) continue;
                    const auto &output_recipe = image.mote_output_recipes[output_index];
                    if (!output_recipe.tick_route_valid ||
                        !output_recipe.output_color_valid ||
                        output_recipe.min_quality > static_cast<std::uint32_t>(event.quality) ||
                        output_recipe.min_quality > static_cast<std::uint32_t>(owner.quality)) continue;
                    VfxFlashSpawnInput base;
                    base.position = anchor; base.shape = VfxImpactShape::SmallMote;
                    base.size = recipes[1].size; base.lifetime = tick.seconds; base.event_tick = event.event_tick;
                    base.drag = source_recipe.drag; base.appearance = std::cref(output_recipe.appearance);
                    if (!DecodeMask(image, output_index, event, base)) continue;
                    const auto seed = Mix(event.stable_seed ^ Mix(static_cast<std::uint32_t>(status_recipe->effect_id)) ^
                        Mix(static_cast<std::uint32_t>(status_recipe->effect_id >> 32)) ^ Mix(source_recipe.stable_id));
                    prepared.push_back({base, source_recipe.count, source_recipe.count, seed,
                        status.importance, 1.0f, recipes[1].radius, recipes[1].speed,
                        0.0f, VfxPreparedMoteSampler::StatusTick, false, event.sequence});
                }
            }
            if (prepared.size() != event_plan_begin) break;
        }
    }
}

void AppendVfxOwnedStatusTickMoteFlashes(
    const VfxImage &image, VfxEventView events,
    std::span<const VfxPersistentInput> owners,
    std::vector<VfxFlashSpawnInput> &result,
    std::vector<Sequence> &consumed_event_sequences)
{
    std::vector<VfxPreparedMoteBurst> prepared;
    PrepareVfxOwnedStatusTickMoteBursts(image, events, owners, prepared);
    AppendVfxPreparedMoteBursts(prepared, result, &consumed_event_sequences);
}
std::vector<VfxOwnedStatusTickMoteCommand> BuildVfxOwnedStatusTickMoteCommands(
    const VfxImage &image, VfxEventView events,
    std::span<const VfxPersistentInput> owners)
{
    std::vector<VfxFlashSpawnInput> flashes;
    std::vector<Sequence> consumed;
    AppendVfxOwnedStatusTickMoteFlashes(image, events, owners, flashes, consumed);
    std::vector<VfxOwnedStatusTickMoteCommand> result;
    result.reserve(flashes.size());
    for (std::size_t i = 0; i < flashes.size(); ++i)
        result.push_back({consumed[i], flashes[i]});
    return result;
}

void AppendVfxOwnedMoteCommands(const VfxImage &image,
    VfxEventView inputs, double current_tick, std::vector<VfxFlashSpawnInput> &result,
    VfxMoteRowQuota *quota)
{
    const auto &program = image.program;
    constexpr double lifetime_ticks = 39.0;
    constexpr double lifetime = lifetime_ticks / 60.0;
    for (const auto &input : inputs)
    {
        if (const auto *point = std::get_if<VfxPointPayload>(&input.payload))
        {
            if (input.sequence == 0 || input.effect_handle == 0 || input.effect_handle > program.effects.size() ||
                current_tick < input.event_tick || current_tick - input.event_tick >= 39 ||
                !Finite(point->position) || !std::isfinite(point->authored_scale) || point->authored_scale <= 0 ||
                static_cast<std::uint32_t>(input.quality) > 2) continue;
            const auto &effect = program.effects[input.effect_handle - 1];
            const auto *recipe = EffectRecipe(image, effect, input.effect_handle);
            if (!recipe || !(recipe->effect_routes & kVfxMoteTrapPoint)) continue;
            AppendRateMotes(image, input, point->position, point->authored_scale, effect, recipe->effect_id,
                (current_tick - input.event_tick), 39.0, result, quota);
            continue;
        }
        if (const auto *circle = std::get_if<VfxCirclePayload>(&input.payload))
        {
            if (input.effect_handle == 0 || input.effect_handle > program.effects.size() ||
                input.geometry_owner_id == 0 ||
                current_tick < input.geometry_start_tick || current_tick >= input.geometry_end_tick ||
                input.geometry_end_tick <= input.geometry_start_tick ||
                static_cast<std::uint32_t>(input.quality) > 2 ||
                !Finite(circle->center) || !Finite(circle->direction) ||
                !std::isfinite(circle->radius) || circle->radius <= 0 ||
                !std::isfinite(circle->inner_radius) || circle->inner_radius != 0 ||
                !std::isfinite(circle->lifetime01) || circle->lifetime01 < 0 || circle->lifetime01 >= 1)
                continue;
            const auto &effect = program.effects[input.effect_handle - 1];
            const auto *recipe = EffectRecipe(image, effect, input.effect_handle);
            if (!recipe || !(recipe->effect_routes & kVfxMoteSuicideCircle) ||
                recipe->effect_id != input.geometry_effect_id)
                continue;
            // The fixed recipe retains its identity. Emission follows the real
            // attack-telegraph owner, which RefreshVfxGroundEventOwners retires.
            const auto elapsed = current_tick - input.geometry_start_tick;
            const auto duration = input.geometry_end_tick - input.geometry_start_tick;
            AppendRateMotes(image, input, circle->center, circle->radius, effect, input.geometry_effect_id,
                                  elapsed, static_cast<double>(duration), result, quota);
            continue;
        }
        const auto *sector = std::get_if<VfxSafeSectorPayload>(&input.payload);
        if (!sector || input.effect_handle == 0 || input.effect_handle > program.effects.size() ||
            input.geometry_owner_id == 0 || current_tick < input.geometry_start_tick ||
            current_tick >= input.geometry_end_tick || input.geometry_end_tick <= input.geometry_start_tick ||
            static_cast<std::uint32_t>(input.quality) > 2 || !Finite(sector->center) || !Finite(sector->direction) ||
            !std::isfinite(sector->inner_radius) || !std::isfinite(sector->outer_radius) ||
            sector->inner_radius < 0 || sector->outer_radius <= sector->inner_radius ||
            !std::isfinite(sector->half_angle_degrees) || sector->half_angle_degrees <= 0 || sector->half_angle_degrees > 180) continue;
        const float direction_length = std::hypot(sector->direction.x, sector->direction.z);
        if (!std::isfinite(direction_length) || direction_length < .0001f) continue;
        const auto &effect = program.effects[input.effect_handle-1];
        const auto *identity = EffectRecipe(image, effect, input.effect_handle);
        if (!identity || !(identity->effect_routes & kVfxMoteSafeSector)) continue;
        // Retain tick units for boundary decisions so exact 39-tick lifetimes do
        // not gain a frame from subtracting independently rounded seconds.
        const double elapsed = static_cast<double>(current_tick-input.geometry_start_tick);
        const double duration = static_cast<double>(input.geometry_end_tick-input.geometry_start_tick);
        for (std::uint32_t si=effect.sources.first;si<effect.sources.first+effect.sources.count;++si)
        {
            if (si >= image.mote_source_recipes.size()) continue;
            const auto &source_recipe = image.mote_source_recipes[si];
            if (!source_recipe.rate_route_valid || source_recipe.effect_handle != effect.handle) continue;
            auto *source_quota = QuotaForSource(image, si, quota);
            // The authored parameter catalog defines rate as an integer count/second.
            const double rate = source_recipe.rate;
            const double start=source_recipe.knots[0]*duration, end=source_recipe.knots[1]*duration;
            if(elapsed<start) continue;
            // Integer emission ordinal determines birth time; skipped frames never add duplicate particles.
            const double first_value=std::max(0.0,std::floor((elapsed-start-lifetime_ticks)*rate/60.0)+1);
            const double last_value=std::floor((std::min(elapsed,end)-start)*rate/60.0);
            if(first_value>last_value || last_value>std::numeric_limits<std::uint32_t>::max()) continue;
            const auto seed=Mix(input.stable_seed ^ Mix(static_cast<std::uint32_t>(identity->effect_id)) ^
                Mix(static_cast<std::uint32_t>(identity->effect_id>>32)) ^ Mix(source_recipe.stable_id));
            for (std::size_t output_index = source_recipe.output_range.first;
                 output_index < static_cast<std::size_t>(source_recipe.output_range.first) + source_recipe.output_range.count;
                 ++output_index)
            {
                if (output_index >= image.mote_output_recipes.size()) continue;
                const auto &output_recipe = image.mote_output_recipes[output_index];
                if(!output_recipe.persistent_route_valid||
                    output_recipe.min_quality>static_cast<std::uint32_t>(input.quality))continue;
                const auto first_ordinal = static_cast<std::uint32_t>(first_value);
                const auto last_ordinal = static_cast<std::uint32_t>(last_value);
                ReserveAppendWithinQuota(result,
                    static_cast<std::size_t>(last_ordinal - first_ordinal + 1u), source_quota);
                for(std::uint64_t ordinal=first_ordinal;ordinal<=last_ordinal;++ordinal)
                {
                    const double birth=start+static_cast<double>(ordinal)*60.0/rate;
                    if(birth>=end||elapsed<birth||elapsed-birth>=lifetime_ticks)continue;
                    if(!AdmitMoteRow(source_quota))continue;
                    VfxFlashSpawnInput mote;
                    mote.stable_seed=Mix(seed ^ Mix(static_cast<std::uint32_t>(ordinal)+1));
                    const float angle=std::atan2(sector->direction.z,sector->direction.x)+
                        (Random(mote.stable_seed,0)*2-1)*sector->half_angle_degrees*(std::numbers::pi_v<float>/180)*.65f;
                    const float radius=sector->inner_radius+(sector->outer_radius-sector->inner_radius)*(.45f+.35f*Random(mote.stable_seed,1));
                    mote.position={sector->center.x+std::cos(angle)*radius,sector->center.y+.08f,sector->center.z+std::sin(angle)*radius};
                    const float speed=std::min(.3f,(radius-sector->inner_radius)*.8f);
                    mote.initial_velocity={-std::cos(angle)*speed,.08f,-std::sin(angle)*speed};
                    mote.shape=VfxImpactShape::SoftDisc;mote.size=.035f;mote.lifetime=static_cast<float>(lifetime);
                    mote.normalized_age=static_cast<float>((elapsed-birth)/lifetime_ticks);mote.event_tick=input.geometry_start_tick;
                    mote.appearance = std::cref(output_recipe.appearance);
                    if(Finite(mote.position)&&DecodeMask(image,output_index,input,mote))result.push_back(mote);
                }
            }
        }
    }
}

std::vector<VfxFlashSpawnInput> BuildVfxOwnedMoteCommands(const VfxImage &image,
    VfxEventView inputs, double current_tick)
{
    std::vector<VfxFlashSpawnInput> result;
    AppendVfxOwnedMoteCommands(image, inputs, current_tick, result);
    return result;
}

namespace
{
void AppendVfxPersistentMoteCommandsImpl(
             const VfxImage &image, std::span<const VfxPersistentInput> owners,
             std::vector<VfxFlashSpawnInput> &result, VfxMoteRowQuota *quota)
{
    const auto &program = image.program;
    constexpr double lifetime_ticks = kPersistentMoteLifetimeTicks;
    constexpr double lifetime_seconds = lifetime_ticks / 60.0;
    for (const auto &owner : owners)
    {
        if (const auto *circle = std::get_if<VfxCirclePayload>(&owner.payload))
        {
            AppendPersistentTrapMotes(image, owner, *circle, result, quota);
            continue;
        }
        const auto *entity = std::get_if<VfxEntityPayload>(&owner.payload);
        if (!entity || owner.stable_id == 0 || owner.effect_handle == 0 ||
            owner.effect_handle > program.effects.size() ||
            static_cast<std::uint32_t>(owner.quality) > 2 ||
            !FiniteTransform(owner.current_transform) ||
            !std::isfinite(owner.elapsed_seconds) || owner.elapsed_seconds < 0.0f ||
            !std::isfinite(owner.normalized_age) || owner.normalized_age < 0.0f ||
            owner.normalized_age > 1.0f ||
            !std::isfinite(entity->footprint_radius) || entity->footprint_radius <= 0.0f ||
            !std::isfinite(entity->lifetime01) || entity->lifetime01 < 0.0f ||
            entity->lifetime01 > 1.0f ||
            entity->render_instance_id == 0 ||
            std::abs(entity->lifetime01 - owner.normalized_age) > 0.00001f)
            continue;

        const auto &effect = program.effects[owner.effect_handle - 1];
        const auto *lookup = EffectRecipe(image, effect, owner.effect_handle);
        if (!lookup || !(lookup->effect_routes & (kVfxMoteStatusPresence | kVfxMotePickupPresence |
                kVfxMoteChargedPresence | kVfxMoteSuicidePresence)))
            continue;
        const bool pickup_idle = (lookup->effect_routes & kVfxMotePickupPresence) != 0;
        const bool charged_overcharge = (lookup->effect_routes & kVfxMoteChargedPresence) != 0;
        const bool suicide_countdown = (lookup->effect_routes & kVfxMoteSuicidePresence) != 0;
        if (!charged_overcharge &&
            (owner.normalized_age >= 1.0f || entity->lifetime01 >= 1.0f))
            continue;

        const double elapsed_ticks = owner.elapsed_ticks >= 0.0 && std::isfinite(owner.elapsed_ticks)
            ? owner.elapsed_ticks : static_cast<double>(owner.elapsed_seconds) * 60.0;
        if (!std::isfinite(elapsed_ticks) ||
            elapsed_ticks > static_cast<double>(std::numeric_limits<std::int64_t>::max() / 2)) continue;
        const double current_ticks = elapsed_ticks;
        // Pickup presence has normalized_age == 0 for its whole live interval.
        // It has no expiry: use indexed births from owner age and the existing
        // 39-tick visible window. Never multiply a normalized knot by infinity.
        // At the first sample of a finite status, only a zero-start source runs.
        const double duration_ticks = charged_overcharge
            ? static_cast<double>(owner.source_duration_seconds) * 60.0
            : owner.normalized_age > 0.000001f
                ? elapsed_ticks / static_cast<double>(owner.normalized_age)
                : std::numeric_limits<double>::infinity();
        if (std::isnan(duration_ticks) || duration_ticks <= 0.0 ||
            (charged_overcharge && (!std::isfinite(duration_ticks) || duration_ticks <= elapsed_ticks)))
            continue;

        for (std::size_t source_index = effect.sources.first;
             source_index < static_cast<std::size_t>(effect.sources.first) + effect.sources.count;
             ++source_index)
        {
            if (source_index >= image.mote_source_recipes.size()) continue;
            const auto &source_recipe = image.mote_source_recipes[source_index];
            if (!source_recipe.rate_route_valid || source_recipe.effect_handle != effect.handle) continue;
            auto *source_quota = QuotaForSource(image, source_index, quota);
            const auto rate_value = source_recipe.rate;
            const float rate = static_cast<float>(rate_value);

            const double start_ticks = std::isfinite(duration_ticks)
                ? static_cast<double>(source_recipe.knots[0]) * duration_ticks
                : (source_recipe.knots[0] == 0.0f ? 0.0 :
                   std::numeric_limits<double>::infinity());
            const double end_ticks = std::isfinite(duration_ticks)
                ? static_cast<double>(source_recipe.knots[1]) * duration_ticks
                : std::numeric_limits<double>::infinity();
            if (!std::isfinite(start_ticks) || current_ticks < start_ticks ||
                static_cast<double>(current_ticks) >= end_ticks) continue;

            const double visible_end = std::min(static_cast<double>(current_ticks), end_ticks);
            const double first_value =
                (static_cast<double>(current_ticks) - lifetime_ticks - start_ticks) *
                static_cast<double>(rate) / 60.0;
            const double last_value =
                (visible_end - start_ticks) * static_cast<double>(rate) / 60.0;
            constexpr double kLastOrdinal =
                static_cast<double>(std::numeric_limits<std::uint32_t>::max() - 2u);
            if (!std::isfinite(first_value) || !std::isfinite(last_value) ||
                last_value < 0.0 || first_value > kLastOrdinal || last_value > kLastOrdinal)
                continue;
            const auto first_ordinal = first_value <= 0.0
                ? std::uint64_t{0}
                : static_cast<std::uint64_t>(std::max(0.0, std::floor(first_value)));
            const auto last_ordinal = static_cast<std::uint64_t>(last_value + 2.0);
            const auto source_seed = Mix(owner.stable_seed ^
                Mix(static_cast<std::uint32_t>(lookup->effect_id)) ^
                Mix(static_cast<std::uint32_t>(lookup->effect_id >> 32)) ^
                Mix(source_recipe.stable_id));
            const Float3 anchor{owner.current_transform[12], owner.current_transform[13],
                                owner.current_transform[14]};
            if (!Finite(anchor)) continue;

            const auto ordinal_count = static_cast<std::size_t>(last_ordinal - first_ordinal + 1);
            const auto output_count = static_cast<std::size_t>(source_recipe.output_range.count);
            const auto candidate_rows = output_count != 0 &&
                    ordinal_count > std::numeric_limits<std::size_t>::max() / output_count
                ? std::numeric_limits<std::size_t>::max()
                : ordinal_count * output_count;
            ReserveAppendWithinQuota(result, candidate_rows, source_quota);
            for (std::uint64_t ordinal = first_ordinal; ordinal <= last_ordinal; ++ordinal)
            {
                const auto birth = PersistentBirthTick(start_ticks, rate, ordinal);
                if (!birth || *birth > current_ticks ||
                    static_cast<double>(*birth) >= end_ticks) continue;
                const double age_ticks = static_cast<double>(current_ticks - *birth);
                if (age_ticks < 0.0 || age_ticks >= lifetime_ticks) continue;

                bool geometry_ready = false;
                bool geometry_valid = false;
                std::uint32_t mote_seed{};
                Float3 offset{}, velocity{};
                float horizontal{};
                for (std::size_t output_index = source_recipe.output_range.first;
                     output_index < static_cast<std::size_t>(source_recipe.output_range.first) + source_recipe.output_range.count;
                     ++output_index)
                {
                    if (output_index >= image.mote_output_recipes.size()) continue;
                    const auto &output_recipe = image.mote_output_recipes[output_index];
                    if (!output_recipe.persistent_route_valid ||
                        output_recipe.min_quality > static_cast<std::uint32_t>(owner.quality))
                        continue;
                    const auto params = EvaluatePersistentMoteParameters(
                        output_recipe, entity->footprint_radius);
                    if (!params) continue;
                    if (!AdmitMoteRow(source_quota)) continue;
                    if (!geometry_ready)
                    {
                        geometry_ready = true;
                        mote_seed = Mix(source_seed ^
                            Mix(static_cast<std::uint32_t>(ordinal + 1u)));
                        const float angle = 2.0f * std::numbers::pi_v<float> * Random(mote_seed, 0);
                        const float local_radius = entity->footprint_radius *
                            (0.45f + 0.4f * Random(mote_seed, 1));
                        const Float3 local_offset{std::cos(angle) * local_radius,
                                                  entity->footprint_radius * .08f,
                                                  std::sin(angle) * local_radius};
                        const Float3 world_offset = TransformOffset(owner.current_transform, local_offset);
                        offset = {world_offset.x, world_offset.y, world_offset.z};
                        const float inward_scale = static_cast<float>(1.0 / lifetime_seconds);
                        velocity = {-offset.x * inward_scale, -offset.y * inward_scale,
                                    -offset.z * inward_scale};
                        horizontal = std::hypot(velocity.x, velocity.z);
                        geometry_valid = Finite(offset) && Finite(velocity) &&
                            std::isfinite(horizontal) && horizontal > 0.0001f;
                    }
                    if (!geometry_valid) continue;

                    VfxFlashSpawnInput mote;
                    mote.position = anchor;
                    mote.shape = VfxImpactShape::SoftDisc;
                    mote.size = params->size;
                    mote.lifetime = static_cast<float>(lifetime_seconds);
                    mote.normalized_age = static_cast<float>(age_ticks / lifetime_ticks);
                    mote.stable_seed = mote_seed;
                    mote.event_tick = {};
                    mote.direction = {velocity.x / horizontal, 0.0f,
                                      velocity.z / horizontal};
                    mote.initial_offset = offset;
                    mote.initial_velocity = velocity;
                    mote.appearance = std::cref(output_recipe.appearance);
                    mote.curve_row = params->curve_row;
                    if (Finite(mote.position) && DecodeMask(image, output_index, owner, mote))
                        result.push_back(mote);
                }
                if (ordinal == std::numeric_limits<std::uint64_t>::max()) break;
            }
        }
    }
}
} // namespace

std::vector<VfxFlashSpawnInput> BuildVfxPersistentMoteCommands(
    const VfxImage &image, std::span<const VfxPersistentInput> owners)
{
    std::vector<VfxFlashSpawnInput> result;
    AppendVfxPersistentMoteCommandsImpl(image, owners, result, nullptr);
    return result;
}
void AppendVfxPersistentMoteCommands(const VfxImage &image,
    std::span<const VfxPersistentInput> owners, std::vector<VfxFlashSpawnInput> &result,
    VfxMoteRowQuota *quota)
{
    AppendVfxPersistentMoteCommandsImpl(image, owners, result, quota);
}

std::size_t VfxPersistentSparkState::RetainedCapacityBytes() const noexcept
{
    using namespace vfx_retained_size;
    return Add(Vector(cursors_), Vector(cursor_slots_));
}

void VfxPersistentSparkState::Append(
    const VfxImage &image, std::span<const VfxPersistentInput> owners,
    Tick current_tick, float interpolation, std::vector<VfxFlashSpawnInput> &result,
    VfxMoteRowQuota *quota)
{
    const auto &program = image.program;
    cursor_slots_.clear();
    std::erase_if(cursors_, [current_tick](const Cursor &cursor) {
        return current_tick >= cursor.expires_tick;
    });
    if (owners.empty()) return;
    // Runtime terminates on allocation failure, so retry copies are unnecessary.
    constexpr auto no_slot = std::numeric_limits<std::size_t>::max();
    const auto hash_key = [](std::uint64_t owner_id, VfxEffectHandle effect,
                             std::uint32_t source) noexcept {
        auto value = owner_id ^ (static_cast<std::uint64_t>(effect) << 32) ^
            (static_cast<std::uint64_t>(source) * 0x9e3779b97f4a7c15ull);
        value ^= value >> 30; value *= 0xbf58476d1ce4e5b9ull;
        value ^= value >> 27; value *= 0x94d049bb133111ebull;
        value ^= value >> 31;
        return static_cast<std::size_t>(value);
    };
    const auto key_matches = [&](std::size_t index, std::uint64_t owner_id,
                                 VfxEffectHandle effect, std::uint32_t source) {
        const auto &cursor = cursors_[index];
        return cursor.owner_id == owner_id && cursor.effect_handle == effect &&
            cursor.source_index == source;
    };
    const auto insert_index = [&](std::size_t index) {
        const auto &cursor = cursors_[index];
        auto slot = hash_key(cursor.owner_id, cursor.effect_handle, cursor.source_index) % cursor_slots_.size();
        while (cursor_slots_[slot] != no_slot)
        {
            if (key_matches(cursor_slots_[slot], cursor.owner_id, cursor.effect_handle, cursor.source_index)) return;
            slot = (slot + 1) % cursor_slots_.size();
        }
        cursor_slots_[slot] = index;
    };
    const auto rebuild_index = [&] {
        std::fill(cursor_slots_.begin(), cursor_slots_.end(), no_slot);
        for (std::size_t index = 0; index < cursors_.size(); ++index) insert_index(index);
    };
    const auto ensure_index_capacity = [&](std::size_t required_entries) {
        const auto maximum = cursor_slots_.max_size();
        if (required_entries > maximum / 2)
            throw std::length_error("persistent spark index exceeds vector capacity");
        const auto required_slots = std::max<std::size_t>(8, required_entries * 2);
        if (cursor_slots_.size() >= required_slots) return false;
        auto target = std::max<std::size_t>(8, cursor_slots_.size());
        while (target < required_slots)
            target = target > maximum / 2 ? required_slots : target * 2;
        cursor_slots_.reserve(target);
        cursor_slots_.resize(target, no_slot);
        rebuild_index();
        return true;
    };
    if (!ensure_index_capacity(cursors_.size())) rebuild_index();
    const auto find_slot = [&](std::uint64_t owner_id, VfxEffectHandle effect,
                               std::uint32_t source) {
        auto slot = hash_key(owner_id, effect, source) % cursor_slots_.size();
        while (cursor_slots_[slot] != no_slot &&
               !key_matches(cursor_slots_[slot], owner_id, effect, source))
            slot = (slot + 1) % cursor_slots_.size();
        return slot;
    };
    for (const auto &owner : owners)
    {
        const auto *projectile = std::get_if<VfxProjectilePayload>(&owner.payload);
        if (!projectile || owner.stable_id == 0 || owner.effect_handle == 0 ||
            owner.effect_handle > program.effects.size() ||
            static_cast<std::uint32_t>(owner.quality) > 2 ||
            !std::isfinite(owner.elapsed_seconds) || owner.elapsed_seconds < 0 ||
            !std::isfinite(projectile->hitbox_radius) || projectile->hitbox_radius <= 0 ||
            !Finite(projectile->velocity)) continue;
        const Float3 position{owner.current_transform[12], owner.current_transform[13],
                              owner.current_transform[14]};
        const float speed = std::sqrt(projectile->velocity.x*projectile->velocity.x +
            projectile->velocity.y*projectile->velocity.y +
            projectile->velocity.z*projectile->velocity.z);
        const double owner_elapsed_ticks = owner.elapsed_ticks >= 0.0 && std::isfinite(owner.elapsed_ticks)
            ? owner.elapsed_ticks : static_cast<double>(owner.elapsed_seconds) * 60.0;
        const double elapsed_ticks = owner_elapsed_ticks +
            (current_tick != 0 ? 1.0 - interpolation : 0.0);
        if (!Finite(position) || !std::isfinite(speed) || speed <= .0001f ||
            !std::isfinite(elapsed_ticks) ||
            elapsed_ticks > static_cast<double>(std::numeric_limits<std::int64_t>::max() / 2) ||
            std::abs(elapsed_ticks - std::round(elapsed_ticks)) > .01) continue;
        const auto owner_tick = static_cast<std::int64_t>(std::llround(elapsed_ticks));
        if (static_cast<std::uint64_t>(owner_tick) > current_tick) continue;
        const auto &effect = program.effects[owner.effect_handle - 1];
        const auto *lookup = EffectRecipe(image, effect, owner.effect_handle);
        if (!lookup || !(lookup->effect_routes & kVfxMoteProjectileSpark) ||
            owner.elapsed_seconds >= effect.seconds) continue;
        for (std::size_t source_index = effect.sources.first;
             source_index < static_cast<std::size_t>(effect.sources.first) + effect.sources.count;
             ++source_index)
        {
            if (source_index >= image.mote_source_recipes.size()) continue;
            const auto &source_recipe = image.mote_source_recipes[source_index];
            if (!source_recipe.spark_route_valid || source_recipe.effect_handle != effect.handle) continue;
            auto *source_quota = QuotaForSource(image, source_index, quota);
            const SparkParams params{source_recipe.count, source_recipe.cone_degrees, source_recipe.drag};
            for (std::size_t output_index = source_recipe.output_range.first;
                 output_index < static_cast<std::size_t>(source_recipe.output_range.first) + source_recipe.output_range.count;
                 ++output_index)
            {
                if (output_index >= image.mote_output_recipes.size() ||
                    !image.mote_output_recipes[output_index].spark_route_valid ||
                    !image.mote_output_recipes[output_index].spark_textures_valid ||
                    !image.mote_output_recipes[output_index].output_color_valid) continue;
                const auto &output_recipe = image.mote_output_recipes[output_index];
                const auto source = static_cast<std::uint32_t>(source_index);
                auto slot = find_slot(owner.stable_id, owner.effect_handle, source);
                auto index = cursor_slots_[slot];
                if (index == no_slot)
                {
                    if (cursors_.size() == std::numeric_limits<std::size_t>::max())
                        throw std::length_error("persistent spark index entry count overflow");
                    ensure_index_capacity(cursors_.size() + 1);
                    slot = find_slot(owner.stable_id, owner.effect_handle, source);
                    ReserveAppend(cursors_, 1);
                    index = cursors_.size();
                    const auto expiry_offset = static_cast<Tick>(
                        std::ceil(static_cast<double>(effect.seconds) * 60.0));
                    cursors_.push_back({owner.stable_id, owner.effect_handle, source, 0,
                         current_tick - static_cast<Tick>(owner_tick) + expiry_offset});
                    cursor_slots_[slot] = index;
                }
                auto &cursor = cursors_[index];
                const double start = static_cast<double>(source_recipe.knots[0]) * effect.seconds;
                const double window = static_cast<double>(source_recipe.knots[1] - source_recipe.knots[0]) *
                    effect.seconds;
                const double spacing = params.count > 1 ?
                    window / static_cast<double>(params.count - 1) : window;
                if (!std::isfinite(spacing) || spacing <= 0) continue;
                ReserveAppendWithinQuota(result,
                    static_cast<std::size_t>(params.count - cursor.next_ordinal), source_quota);
                while (cursor.next_ordinal < params.count)
                {
                    const auto ordinal = cursor.next_ordinal;
                    const auto birth_tick = static_cast<std::int64_t>(
                        std::ceil((start + spacing * ordinal) * 60.0 - 0.000001));
                    if (owner_tick < birth_tick) break;
                    ++cursor.next_ordinal;
                    if (owner_tick != birth_tick ||
                        image.mote_output_recipes[output_index].min_quality > static_cast<std::uint32_t>(owner.quality)) continue;
                    if (!AdmitMoteRow(source_quota)) continue;
                    const auto seed = Mix(owner.stable_seed ^
                        Mix(static_cast<std::uint32_t>(lookup->effect_id)) ^
                        Mix(static_cast<std::uint32_t>(lookup->effect_id >> 32)) ^
                        Mix(source_recipe.stable_id) ^ Mix(ordinal + 1));
                    const Float3 direction = SparkDirection(projectile->velocity,
                        params.cone_degrees, seed);
                    if (!Finite(direction)) continue;
                    VfxFlashSpawnInput spark;
                    spark.shape = VfxImpactShape::Spark;
                    spark.position = position;
                    spark.direction = direction;
                    spark.initial_velocity = {direction.x * speed, direction.y * speed,
                                              direction.z * speed};
                    spark.size = projectile->hitbox_radius;
                    spark.lifetime = static_cast<float>(spacing);
                    spark.event_tick = current_tick;
                    spark.stable_seed = seed;
                    spark.appearance = std::cref(output_recipe.appearance);
                    spark.drag = params.drag;
                    result.push_back(spark);
                }
            }
        }
    }

}

std::vector<VfxFlashSpawnInput> VfxPersistentSparkState::Emit(
    const VfxImage &image, std::span<const VfxPersistentInput> owners,
    Tick current_tick, float interpolation, VfxMoteRowQuota *quota)
{
    std::vector<VfxFlashSpawnInput> result;
    Append(image, owners, current_tick, interpolation, result, quota);
    return result;
}

}
