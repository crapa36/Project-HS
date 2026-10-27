#include "vfx_typed_mote_commands.hpp"
#include "vfx_generation_rebind.hpp"
#include <hs/render_data/render_snapshot.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iterator>
#include <numbers>
#include <limits>
#include <optional>
#include <string_view>

namespace hs::runtime_detail
{
namespace
{
constexpr std::uint32_t Hash(std::string_view s)
{ std::uint32_t h = 2166136261u; for (const unsigned char c : s) { h ^= c; h *= 16777619u; } return h; }
constexpr std::uint64_t Hash64(std::string_view s)
{ std::uint64_t h = 14695981039346656037ull; for (const unsigned char c : s) { h ^= c; h *= 1099511628211ull; } return h; }
template<class T> bool Range(VfxRange r, std::span<const T> v)
{ return r.first <= v.size() && r.count <= v.size() - r.first; }
bool Finite(Float3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
std::uint32_t Mix(std::uint32_t v)
{ v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; return v ^ (v >> 16); }
float Random(std::uint32_t seed, std::uint32_t lane)
{ return static_cast<float>(Mix(seed ^ Mix(lane + 0x9e3779b9u)) >> 8) * (1.0f / 16777216.0f); }
struct Recipe { std::uint32_t shape; VfxMotionKind motion; VfxImpactShape kind; float size, radius, speed, rate; };
constexpr Recipe recipes[]{
    {Hash("soft_leaf_mote"), VfxMotionKind::SlowUpwardHelix, VfxImpactShape::LeafMote, .045f, .2f, .45f, 2},
    {Hash("small_motes"), VfxMotionKind::VeryShortLocalDrift, VfxImpactShape::SmallMote, .025f, .08f, .1f, 0},
    {Hash("diamond_motes"), VfxMotionKind::ShortUpwardSpiral, VfxImpactShape::DiamondMote, .035f, .15f, .6f, 5}
};
struct SourceParams { std::uint32_t count{}; float curl{.2f}, drag{.8f}; };
std::optional<SourceParams> DecodeParams(const VfxImage &image, std::size_t source_index)
{
    if (source_index >= image.mote_source_recipes.size()) return {};
    const auto &recipe = image.mote_source_recipes[source_index];
    if (!recipe.common_params_valid) return {};
    return SourceParams{recipe.count, recipe.curl, recipe.drag};
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
        if (out.mask_slice != 0xffffffffu || !Range(slices, program.slice_indices) || slices.count == 0)
            return false;
        const auto choices = program.slice_indices.subspan(slices.first, slices.count);
        out.mask_slice = choices[input.stable_seed % choices.size()];
        out.mask_strength = recipe.mask_strengths[quality];
    }
    return true;
}
constexpr std::array kStatusMoteEffects{
    Hash64("persistent.status.slow"), Hash64("persistent.status.mark")};
constexpr std::array kPickupIdleMoteEffects{
    Hash64("particle.pickup.xp.idle"), Hash64("particle.pickup.heal.idle"),
    Hash64("particle.pickup.magnet.idle"), Hash64("particle.pickup.relic_chest.idle")};
constexpr auto kChargedOverchargeLoop = Hash64("particle.upgrade.charged.overcharge_loop");
constexpr auto kEntityPayload = Hash("EntityAttachmentPayload");
constexpr auto kCirclePayload = Hash("CircleAreaPayload");
constexpr auto kTrapArmedEffect = Hash64("persistent.trap.armed");
constexpr auto kTrapPendingEffect = Hash64("persistent.trap.pending");
constexpr auto kAmbientMotes = Hash("ambient_motes");
constexpr auto kSoftMotes = Hash("soft_motes");
constexpr auto kSpriteDomain = Hash("sprite");
constexpr auto kComponentSize = Hash("component_size");
constexpr auto kParticle = Hash("particle");
constexpr auto kAnalytic = Hash("analytic");
constexpr auto kSoftDisc = Hash("soft_disc");
constexpr float kPersistentMoteLifetimeTicks = 39.0f;

struct PersistentMoteParameters
{
    float size{};
    std::uint32_t curve_row{0xffffffffu};
};

std::optional<PersistentMoteParameters> DecodePersistentMoteParameters(
    const VfxImage &image, std::size_t output_index, float footprint)
{
    const auto &program = image.program;
    if (output_index >= image.mote_output_recipes.size() || !std::isfinite(footprint) || footprint <= 0.0f) return {};
    const auto &recipe = image.mote_output_recipes[output_index];
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
constexpr std::array kSparkEffects{
    Hash64("particle.basic_attack"), Hash64("particle.skill.charged_shot"),
    Hash64("particle.skill.explosive_arrow"), Hash64("particle.skill.piercing_shot"),
    Hash64("particle.skill.ricochet_arrow"),
    Hash64("particle.upgrade.arrow_rain.incoming_arrow")};
struct SparkParams { std::uint32_t count{}; float cone_degrees{}, drag{}; };
std::optional<SparkParams> DecodeSparkParams(const VfxProgramView &p,
                                             const VfxSourceRecord &source)
{
    if (!Range(source.parameters, p.parameters) || source.parameters.count != 3) return {};
    bool count_seen = false, cone_seen = false, drag_seen = false;
    SparkParams result;
    for (const auto &parameter : std::span(p.parameters).subspan(
             source.parameters.first, source.parameters.count))
    {
        if (parameter.key == Hash("count") && parameter.type == VfxParameterType::Int &&
            !count_seen && parameter.bits > 0 && parameter.bits <= 64)
        { result.count = parameter.bits; count_seen = true; }
        else if (parameter.key == Hash("cone_deg") && parameter.type == VfxParameterType::Float &&
                 !cone_seen)
        {
            result.cone_degrees = std::bit_cast<float>(parameter.bits);
            cone_seen = true;
        }
        else if (parameter.key == Hash("drag") && parameter.type == VfxParameterType::Float &&
                 !drag_seen)
        {
            result.drag = std::bit_cast<float>(parameter.bits);
            drag_seen = true;
        }
        else return {};
    }
    if (!count_seen || !cone_seen || !drag_seen ||
        !std::isfinite(result.cone_degrees) || result.cone_degrees <= 0 ||
        result.cone_degrees > 90 || !std::isfinite(result.drag) ||
        result.drag < 0 || result.drag > 10)
        return {};
    return result;
}
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
                          VfxPersistentMoteBirthWindow *birth_window = nullptr,
                          double global_current_tick = 0,
                          std::uint64_t owner_id = 0,
                          std::uint64_t episode_generation = 0)
{
    const auto &program = image.program;
    constexpr double lifetime_ticks = kPersistentMoteLifetimeTicks;
    constexpr double lifetime_seconds = lifetime_ticks / 60.0;
    for (std::size_t source_index = effect.sources.first;
         source_index < static_cast<std::size_t>(effect.sources.first) + effect.sources.count;
         ++source_index)
    {
        const auto &source = program.sources[source_index];
        if (source.effect != effect.handle || source.stable_id != kAmbientMotes ||
            source.type != VfxSourceType::CurlMotes || source.knot_count != 2 ||
            source.knots[0] != 0.0f || source.knots[1] != 1.0f ||
            !Range(source.parameters, program.parameters) || source.parameters.count != 1 ||
            !Range(source.outputs, program.outputs) || source.outputs.count != 1)
            continue;
        if (source_index >= image.mote_source_recipes.size() ||
            !image.mote_source_recipes[source_index].rate_valid ||
            image.mote_source_recipes[source_index].rate != 3)
            continue;

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
            Mix(source.stable_id));

        if (source.outputs.first >= image.mote_output_recipes.size()) continue;
        const auto &output = program.outputs[source.outputs.first];
        const auto &output_recipe = image.mote_output_recipes[source.outputs.first];
        if (output.source != source_index || output.profile != VfxOutputProfile::SpriteAdd ||
            output.shape != kSoftMotes || output.shape_domain != kSpriteDomain ||
            output.shape_scale_rule != kComponentSize || output.shape_component_kind != kParticle ||
            output.coverage_type != kAnalytic || output.coverage_ref != kSoftDisc ||
            output.motion != VfxMotionKind::RareInwardDrift ||
            !output_recipe.output_color_valid ||
            output_recipe.min_quality != static_cast<std::uint32_t>(VfxQuality::Medium) ||
            output_recipe.min_quality > static_cast<std::uint32_t>(owner.quality) ||
            !Range(output.parameters, program.parameters) || output.parameters.count != 0)
            continue;
        const auto params = DecodePersistentMoteParameters(image, source.outputs.first, extent);
        if (!params)
            continue;

        for (std::uint64_t ordinal = first_ordinal; ordinal <= last_ordinal; ++ordinal)
        {
            const auto birth = PersistentBirthTick(start_ticks, 3.0f, ordinal);
            if (!birth || *birth > current_ticks || static_cast<double>(*birth) >= end_ticks)
                continue;
            const double age_ticks = static_cast<double>(current_ticks - *birth);
            if (age_ticks < 0.0 || age_ticks >= lifetime_ticks)
                continue;

            const auto mote_seed = Mix(source_seed ^
                Mix(static_cast<std::uint32_t>(ordinal + 1u)));
            Tick global_birth_tick{};
            const double local_birth_age = current_ticks - *birth;
            if (global_current_tick >= local_birth_age)
                global_birth_tick = static_cast<Tick>(std::llround(global_current_tick - local_birth_age));
            const auto unit = birth_window
                ? birth_window->GetOrCreate(owner_id, episode_generation, effect_id,
                    source.stable_id, static_cast<std::uint32_t>(ordinal), global_birth_tick, mote_seed)
                : VfxPersistentMoteBirthWindow::UnitBirth{Random(mote_seed, 0), Random(mote_seed, 1)};
            const float angle = 2.0f * std::numbers::pi_v<float> * unit.angle01;
            const float local_radius = extent * (0.45f + 0.4f * unit.radius01);
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
            mote.gradient_row = output_recipe.gradient_row;
            mote.curve_row = params->curve_row;
            mote.hdr = output_recipe.hdr;
            mote.color = output_recipe.color;
            if (Finite(mote.position) &&
                Finite({mote.color.x, mote.color.y, mote.color.z}) &&
                std::isfinite(mote.color.w) && mote.color.w >= 0.0f &&
                mote.color.w <= 1.0f && DecodeMask(image, source.outputs.first, owner, mote))
                result.push_back(mote);
            if (ordinal == std::numeric_limits<std::uint64_t>::max()) break;
        }
    }
}

void AppendPersistentTrapMotes(const VfxImage &image,
                               const VfxPersistentInput &owner,
                               const VfxCirclePayload &circle,
                               std::vector<VfxFlashSpawnInput> &result,
                               VfxPersistentMoteBirthWindow *birth_window = nullptr,
                               double global_current_tick = 0)
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
    if (effect.handle != owner.effect_handle || effect.input_mode != 1 ||
        effect.timing_kind != 1 || effect.payload_kind != kCirclePayload ||
        !Range(effect.sources, program.sources))
        return;
    const auto lookup = std::find_if(program.effect_lookup.begin(),
                                     program.effect_lookup.end(),
        [&](const auto &entry) { return entry.handle == effect.handle; });
    if (lookup == program.effect_lookup.end() ||
        (lookup->effect_id != kTrapArmedEffect && lookup->effect_id != kTrapPendingEffect &&
         lookup->effect_id != Hash64("particle.upgrade.retreat.start_trap")) ||
        std::count_if(program.effect_lookup.begin(), program.effect_lookup.end(),
            [&](const auto &entry) { return entry.handle == effect.handle; }) != 1)
        return;

    const auto phase = static_cast<PersistentVfxKind>(owner.source_visual_kind);
    if ((lookup->effect_id == kTrapPendingEffect && phase != PersistentVfxKind::TrapPending) ||
        (lookup->effect_id == kTrapArmedEffect && phase != PersistentVfxKind::TrapArmed) ||
        (lookup->effect_id == Hash64("particle.upgrade.retreat.start_trap") &&
         phase != PersistentVfxKind::TrapPending && phase != PersistentVfxKind::TrapArmed))
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
                          current_ticks, duration_ticks, result, birth_window, global_current_tick,
                          owner.stable_id, owner.status_episode_generation);
}
}

void VfxPersistentMoteBirthWindow::Prune(Tick current_tick)
{
    std::erase_if(births_, [current_tick](const auto &entry) {
        return current_tick >= entry.second.birth_tick &&
            current_tick - entry.second.birth_tick >= 39;
    });
}

VfxPersistentMoteBirthWindow::UnitBirth VfxPersistentMoteBirthWindow::GetOrCreate(
    std::uint64_t owner_id, std::uint64_t episode_generation, std::uint64_t effect_id,
    std::uint32_t source_id, std::uint32_t ordinal, Tick birth_tick, std::uint32_t seed)
{
    const Key key{owner_id, episode_generation, effect_id, source_id, ordinal, seed};
    if (const auto found = births_.find(key); found != births_.end()) return found->second.unit;
    const UnitBirth unit{Random(seed, 0), Random(seed, 1)};
    constexpr std::size_t kMaxBirths = 65536;
    // shortcut: full cache recomputes deterministic coordinates, increase only if profiling shows sustained saturation.
    if (births_.size() < kMaxBirths) births_.emplace(key, Entry{birth_tick, unit});
    return unit;
}

std::vector<VfxFlashSpawnInput> BuildVfxTypedMoteCommands(const VfxImage &image, VfxEventView inputs)
{
    const auto &program = image.program;
    std::vector<VfxFlashSpawnInput> result;
    for (const auto &input : inputs)
    {
        if (input.effect_handle == 0 || input.effect_handle > program.effects.size() || static_cast<std::uint32_t>(input.quality) > 2) continue;
        const auto &effect = program.effects[input.effect_handle - 1];
        if (effect.handle != input.effect_handle || effect.input_mode != 0 || effect.timing_kind != 0 ||
            !std::isfinite(effect.seconds) || effect.seconds <= 0 || !Range(effect.sources, program.sources)) continue;
        const auto lookup = std::find_if(program.effect_lookup.begin(), program.effect_lookup.end(), [&](const auto &v) { return v.handle == effect.handle; });
        if (lookup == program.effect_lookup.end() || lookup->effect_id == 0) continue;
        if (const auto *entity = std::get_if<VfxEntityPayload>(&input.payload))
        {
            if (lookup->effect_id != Hash64("particle.skill.charged_shot.cancel") ||
                effect.payload_kind != kEntityPayload || input.sequence == 0 ||
                entity->render_instance_id == 0 || !std::isfinite(entity->footprint_radius) ||
                entity->footprint_radius <= 0) continue;
            const Float3 anchor{input.world_transform[12], input.world_transform[13],
                                input.world_transform[14]};
            if (!Finite(anchor)) continue;
            for (std::uint32_t si = effect.sources.first; si < effect.sources.first + effect.sources.count; ++si)
            {
                const auto &source = program.sources[si];
                if (source.effect != effect.handle || source.type != VfxSourceType::CurlMotes ||
                    source.stable_id != Hash("residual_motes") || source.knot_count != 2 ||
                    source.knots[0] != 0 || source.knots[1] != 1 ||
                    !Range(source.parameters, program.parameters) || source.parameters.count != 2 ||
                    !Range(source.outputs, program.outputs)) continue;
                if (si >= image.mote_source_recipes.size() ||
                    !image.mote_source_recipes[si].residual_valid) continue;
                const auto count = image.mote_source_recipes[si].count;
                const auto speed = image.mote_source_recipes[si].speed;
            for (std::size_t output_index = source.outputs.first;
                 output_index < static_cast<std::size_t>(source.outputs.first) + source.outputs.count;
                 ++output_index)
                {
                const auto &output = program.outputs[output_index];
                if (output_index >= image.mote_output_recipes.size()) continue;
                const auto &output_recipe = image.mote_output_recipes[output_index];
                    if (output.source != si || output.profile != VfxOutputProfile::SpriteAdd ||
                        output.motion != VfxMotionKind::ParticlesPulledInward || output.shape != Hash("camera_billboard") ||
                        !output_recipe.output_color_valid ||
                        output_recipe.min_quality > static_cast<std::uint32_t>(input.quality) ||
                        !Range(output.parameters, program.parameters) || output.parameters.count != 0 ||
                        std::any_of(output.rgba.begin(), output.rgba.end(),
                            [](float value) { return !std::isfinite(value) || value < 0; })) continue;
                    for (std::uint32_t ordinal = 0; ordinal < count; ++ordinal)
                    {
                        VfxFlashSpawnInput mote;
                        mote.stable_seed = Mix(input.stable_seed ^
                            Mix(static_cast<std::uint32_t>(lookup->effect_id)) ^
                            Mix(static_cast<std::uint32_t>(lookup->effect_id >> 32)) ^
                            Mix(source.stable_id) ^ Mix(ordinal + 1));
                        const auto phase = Random(mote.stable_seed, 0) * 2 * std::numbers::pi_v<float>;
                        const Float3 direction{std::cos(phase), 0, std::sin(phase)};
                        const auto reach = speed * effect.seconds;
                        mote.position = anchor;
                        mote.initial_offset = {direction.x * reach, 0, direction.z * reach};
                        mote.initial_velocity = {-direction.x * speed, 0, -direction.z * speed};
                        mote.shape = VfxImpactShape::SmallMote;
                        mote.size = .025f;
                        mote.lifetime = effect.seconds;
                        mote.event_tick = input.event_tick;
                        mote.gradient_row = output_recipe.gradient_row;
                        mote.hdr = output_recipe.hdr;
                        mote.color = output_recipe.color;
                        if (Finite(mote.initial_offset) && Finite(mote.initial_velocity) &&
                            Finite({mote.color.x, mote.color.y, mote.color.z}) &&
                            DecodeMask(image, output_index, input, mote)) result.push_back(mote);
                    }
                }
            }
            continue;
        }
        Float3 position{}; float scale = 1;
        if (const auto *point = std::get_if<VfxPointPayload>(&input.payload))
        {
            if (effect.payload_kind != Hash("PointEventPayload")) continue;
            position = point->position; scale = point->authored_scale;
        }
        else if (std::holds_alternative<VfxContextPayload>(input.payload))
        {
            if (effect.payload_kind != Hash("PresentationContextPayload")) continue;
            position = {input.world_transform[12], input.world_transform[13], input.world_transform[14]};
        }
        else continue;
        if (!Finite(position) || !std::isfinite(scale) || scale <= 0) continue;
        for (std::uint32_t si = effect.sources.first; si < effect.sources.first + effect.sources.count; ++si)
        {
            const auto &source = program.sources[si];
            if (source.effect != effect.handle || source.type != VfxSourceType::CurlMotes || source.knot_count != 2 ||
                !std::isfinite(source.knots[0]) || !std::isfinite(source.knots[1]) || source.knots[0] < 0 ||
                source.knots[1] <= source.knots[0] || source.knots[1] > 1 || !Range(source.outputs, program.outputs)) continue;
            const auto params = DecodeParams(image, si);
            if (!params) continue;
            const auto source_seed = Mix(input.stable_seed ^ Mix(static_cast<std::uint32_t>(lookup->effect_id)) ^
                Mix(static_cast<std::uint32_t>(lookup->effect_id >> 32)) ^ Mix(source.stable_id));
            for (std::size_t output_index = source.outputs.first;
                 output_index < static_cast<std::size_t>(source.outputs.first) + source.outputs.count;
                 ++output_index)
            {
                const auto &output = program.outputs[output_index];
                if (output_index >= image.mote_output_recipes.size()) continue;
                const auto &output_recipe = image.mote_output_recipes[output_index];
                const auto recipe = std::find_if(std::begin(recipes), std::end(recipes), [&](const auto &v) { return v.shape == output.shape && v.motion == output.motion; });
                if (recipe == std::end(recipes) || output.source != si || output.profile != VfxOutputProfile::SpriteAdd ||
                    !output_recipe.output_color_valid ||
                    output_recipe.min_quality > static_cast<std::uint32_t>(input.quality) ||
                    !Range(output.parameters, program.parameters) || output.parameters.count != 0) continue;
                VfxFlashSpawnInput base;
                base.position = position; base.shape = recipe->kind; base.size = recipe->size * scale;
                base.delay = source.knots[0] * effect.seconds; base.lifetime = (source.knots[1] - source.knots[0]) * effect.seconds;
                base.event_tick = input.event_tick; base.gradient_row = output_recipe.gradient_row; base.hdr = output_recipe.hdr;
                base.color = output_recipe.color;
                base.drag = params->drag; base.orbit_rate = recipe->rate;
                base.orbit_radius = (recipe->kind == VfxImpactShape::LeafMote ? params->curl : recipe->kind == VfxImpactShape::SmallMote ? 0.0f : .15f) * scale;
                if (!std::isfinite(base.size) || base.size <= 0 || !std::isfinite(base.delay) || !std::isfinite(base.lifetime) || base.lifetime <= 0 ||
                    !std::isfinite(base.orbit_radius) || !std::isfinite(base.hdr) || base.hdr <= 0 || !Finite({base.color.x, base.color.y, base.color.z}) ||
                    !std::isfinite(base.color.w) || base.color.w < 0 || base.color.w > 1 || !DecodeMask(image, output_index, input, base)) continue;
                // Burst identity depends on semantic IDs and emission index, never frame count or catalog ordering.
                for (std::uint32_t i = 0; i < params->count; ++i)
                {
                    auto mote = base; mote.stable_seed = Mix(source_seed ^ Mix(i + 1));
                    const float phase = Random(mote.stable_seed, 0) * 2 * std::numbers::pi_v<float>;
                    const float radius = recipe->radius * scale * (.35f + .65f * Random(mote.stable_seed, 1));
                    mote.initial_offset = {std::cos(phase) * radius, 0, std::sin(phase) * radius};
                    const float velocity_phase = Random(mote.stable_seed, 2) * 2 * std::numbers::pi_v<float>;
                    const float horizontal_speed = Random(mote.stable_seed, 3) * .08f * scale;
                    mote.initial_velocity = {std::cos(velocity_phase) * horizontal_speed, recipe->speed * scale,
                                             std::sin(velocity_phase) * horizontal_speed};
                    mote.orbit_phase = phase;
                    if (Finite(mote.initial_offset) && Finite(mote.initial_velocity)) result.push_back(mote);
                }
            }
        }
    }
    return result;
}
std::vector<VfxOwnedStatusTickMoteCommand> BuildVfxOwnedStatusTickMoteCommands(
    const VfxImage &image, VfxEventView events,
    std::span<const VfxPersistentInput> owners)
{
    const auto &program = image.program;
    std::vector<VfxOwnedStatusTickMoteCommand> result;
    const auto unique_identity = [&](VfxEffectHandle handle, std::uint64_t id) {
        return std::count_if(program.effect_lookup.begin(), program.effect_lookup.end(),
            [&](const auto &v) { return v.handle == handle; }) == 1 &&
            std::any_of(program.effect_lookup.begin(), program.effect_lookup.end(),
                [&](const auto &v) { return v.handle == handle && v.effect_id == id; });
    };
    for (const auto &event : events)
    {
        if (event.effect_handle == 0 || event.effect_handle > program.effects.size() ||
            event.sequence == 0 || event.geometry_owner_id == 0 || event.geometry_owner_id >= (1ull << 60) ||
            event.status_episode_generation == 0 || static_cast<std::uint32_t>(event.quality) > 2 ||
            !std::holds_alternative<VfxPointPayload>(event.payload)) continue;
        const bool bleed = unique_identity(event.effect_handle, Hash64("particle.status.bleed_tick"));
        const bool burn = unique_identity(event.effect_handle, Hash64("particle.status.burn_tick"));
        const auto &tick = program.effects[event.effect_handle - 1];
        if (bleed == burn || tick.handle != event.effect_handle || tick.input_mode != 0 ||
            tick.timing_kind != 0 || tick.payload_kind != Hash("PointEventPayload") ||
            !std::isfinite(tick.seconds) || tick.seconds <= 0) continue;
        const auto status_id = bleed ? Hash64("persistent.status.bleed") : Hash64("persistent.status.burn");
        const auto render_id = (2ull << 60) | event.geometry_owner_id;
        bool emitted = false;
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
            if (!unique_identity(owner.effect_handle, status_id) || status.handle != owner.effect_handle ||
                status.input_mode != 1 || status.timing_kind != 1 || status.payload_kind != kEntityPayload ||
                !Range(status.sources, program.sources)) continue;
            const Float3 anchor{owner.current_transform[12], owner.current_transform[13], owner.current_transform[14]};
            for (std::size_t si = status.sources.first; si < static_cast<std::size_t>(status.sources.first) + status.sources.count; ++si)
            {
                const auto &source = program.sources[si];
                const auto params = DecodeParams(image, si);
                if (source.effect != status.handle || source.stable_id != Hash("tick_motes") ||
                    source.type != VfxSourceType::CurlMotes || source.knot_count != 2 ||
                    source.knots[0] != 0 || source.knots[1] != 1 || !params || params->count != 3 ||
                    source.parameters.count != 1 || !Range(source.outputs, program.outputs)) continue;
                for (std::size_t output_index = source.outputs.first;
                     output_index < static_cast<std::size_t>(source.outputs.first) + source.outputs.count;
                     ++output_index)
                {
                    const auto &output = program.outputs[output_index];
                    if (output_index >= image.mote_output_recipes.size()) continue;
                    const auto &output_recipe = image.mote_output_recipes[output_index];
                    if (output.source != si || output.profile != VfxOutputProfile::SpriteAdd ||
                        output.shape != Hash("small_motes") || output.motion != VfxMotionKind::VeryShortLocalDrift ||
                        output.shape_domain != kSpriteDomain || output.shape_scale_rule != kComponentSize ||
                        output.shape_component_kind != kParticle || output.coverage_type != kAnalytic ||
                        output.coverage_ref != Hash("diamond_spark") ||
                        !output_recipe.output_color_valid || output_recipe.min_quality > 2 ||
                        output_recipe.min_quality > static_cast<std::uint32_t>(event.quality) ||
                        output_recipe.min_quality > static_cast<std::uint32_t>(owner.quality) ||
                        !Range(output.parameters, program.parameters) || output.parameters.count != 0) continue;
                    VfxFlashSpawnInput base;
                    base.position = anchor; base.shape = VfxImpactShape::SmallMote;
                    base.size = recipes[1].size; base.lifetime = tick.seconds; base.event_tick = event.event_tick;
                    base.drag = params->drag; base.gradient_row = output_recipe.gradient_row; base.hdr = output_recipe.hdr;
                    base.color = output_recipe.color;
                    if (!Finite({base.color.x, base.color.y, base.color.z}) || !DecodeMask(image, output_index, event, base)) continue;
                    const auto seed = Mix(event.stable_seed ^ Mix(static_cast<std::uint32_t>(status_id)) ^
                        Mix(static_cast<std::uint32_t>(status_id >> 32)) ^ Mix(source.stable_id));
                    for (std::uint32_t ordinal = 0; ordinal < params->count; ++ordinal)
                    {
                        auto mote = base; mote.stable_seed = Mix(seed ^ Mix(ordinal + 1));
                        const float angle = Random(mote.stable_seed, 0) * 2 * std::numbers::pi_v<float>;
                        const float radius = recipes[1].radius * (.35f + .65f * Random(mote.stable_seed, 1));
                        mote.initial_offset = {std::cos(angle) * radius, 0, std::sin(angle) * radius};
                        const float velocity_angle = Random(mote.stable_seed, 2) * 2 * std::numbers::pi_v<float>;
                        const float speed = Random(mote.stable_seed, 3) * .08f;
                        mote.initial_velocity = {std::cos(velocity_angle) * speed, recipes[1].speed, std::sin(velocity_angle) * speed};
                        result.push_back({event.sequence, mote});
                    }
                    emitted = true;
                }
            }
            if (emitted) break;
        }
    }
    return result;
}

std::vector<VfxFlashSpawnInput> BuildVfxOwnedMoteCommands(const VfxImage &image,
    VfxEventView inputs, double current_tick)
{
    const auto &program = image.program;
    std::vector<VfxFlashSpawnInput> result;
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
            constexpr auto id = Hash64("particle.skill.trap");
            if (effect.handle != input.effect_handle || effect.input_mode != 0 || effect.timing_kind != 0 ||
                effect.seconds != .65f || effect.payload_kind != Hash("PointEventPayload") ||
                !Range(effect.sources, program.sources) || std::count_if(program.effect_lookup.begin(), program.effect_lookup.end(),
                    [&](const auto &v) { return v.handle == effect.handle && v.effect_id == id; }) != 1) continue;
            AppendRateMotes(image, input, point->position, point->authored_scale, effect, id,
                (current_tick - input.event_tick), 39.0, result);
            continue;
        }
        if (const auto *circle = std::get_if<VfxCirclePayload>(&input.payload))
        {
            if (input.effect_handle == 0 || input.effect_handle > program.effects.size() ||
                input.geometry_owner_id == 0 || input.geometry_effect_id != Hash64("particle.enemy.suicide.charge") ||
                current_tick < input.geometry_start_tick || current_tick >= input.geometry_end_tick ||
                input.geometry_end_tick <= input.geometry_start_tick ||
                static_cast<std::uint32_t>(input.quality) > 2 ||
                !Finite(circle->center) || !Finite(circle->direction) ||
                !std::isfinite(circle->radius) || circle->radius <= 0 ||
                !std::isfinite(circle->inner_radius) || circle->inner_radius != 0 ||
                !std::isfinite(circle->lifetime01) || circle->lifetime01 < 0 || circle->lifetime01 >= 1)
                continue;
            const auto &effect = program.effects[input.effect_handle - 1];
            if (effect.handle != input.effect_handle || effect.input_mode != 0 || effect.timing_kind != 0 ||
                effect.payload_kind != kCirclePayload || !Range(effect.sources, program.sources) ||
                std::count_if(program.effect_lookup.begin(), program.effect_lookup.end(),
                    [&](const auto &v) { return v.handle == effect.handle &&
                        v.effect_id == input.geometry_effect_id; }) != 1)
                continue;
            // The fixed recipe retains its identity. Emission follows the real
            // attack-telegraph owner, which RefreshVfxGroundEventOwners retires.
            const auto elapsed = current_tick - input.geometry_start_tick;
            const auto duration = input.geometry_end_tick - input.geometry_start_tick;
            AppendRateMotes(image, input, circle->center, circle->radius, effect, input.geometry_effect_id,
                                  elapsed, static_cast<double>(duration), result);
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
        if (effect.handle != input.effect_handle || effect.input_mode != 0 || effect.timing_kind != 1 ||
            effect.payload_kind != Hash("SafeGapSectorPayload") || !Range(effect.sources,program.sources)) continue;
        const auto identity = std::find_if(program.effect_lookup.begin(),program.effect_lookup.end(),
            [&](const auto &v){return v.handle == effect.handle;});
        if (identity == program.effect_lookup.end() || identity->effect_id == 0) continue;
        // Retain tick units for boundary decisions so exact 39-tick lifetimes do
        // not gain a frame from subtracting independently rounded seconds.
        const double elapsed = static_cast<double>(current_tick-input.geometry_start_tick);
        const double duration = static_cast<double>(input.geometry_end_tick-input.geometry_start_tick);
        for (std::uint32_t si=effect.sources.first;si<effect.sources.first+effect.sources.count;++si)
        {
            const auto &source=program.sources[si];
            if(source.effect!=effect.handle || source.type!=VfxSourceType::CurlMotes || source.knot_count!=2 ||
                !std::isfinite(source.knots[0]) || !std::isfinite(source.knots[1]) || source.knots[0]<0 ||
                source.knots[1]<=source.knots[0] || source.knots[1]>1 || !Range(source.parameters,program.parameters) ||
                source.parameters.count!=1 || !Range(source.outputs,program.outputs)) continue;
            if (si >= image.mote_source_recipes.size() ||
                !image.mote_source_recipes[si].rate_valid) continue;
            // The authored parameter catalog defines rate as an integer count/second.
            const double rate = image.mote_source_recipes[si].rate;
            const double start=source.knots[0]*duration, end=source.knots[1]*duration;
            if(elapsed<start) continue;
            // Integer emission ordinal determines birth time; skipped frames never add duplicate particles.
            const double first_value=std::max(0.0,std::floor((elapsed-start-lifetime_ticks)*rate/60.0)+1);
            const double last_value=std::floor((std::min(elapsed,end)-start)*rate/60.0);
            if(first_value>last_value || last_value>std::numeric_limits<std::uint32_t>::max()) continue;
            const auto seed=Mix(input.stable_seed ^ Mix(static_cast<std::uint32_t>(identity->effect_id)) ^
                Mix(static_cast<std::uint32_t>(identity->effect_id>>32)) ^ Mix(source.stable_id));
            for (std::size_t output_index = source.outputs.first;
                 output_index < static_cast<std::size_t>(source.outputs.first) + source.outputs.count;
                 ++output_index)
            {
                const auto &output = program.outputs[output_index];
                if (output_index >= image.mote_output_recipes.size()) continue;
                const auto &output_recipe = image.mote_output_recipes[output_index];
                if(output.source!=si||output.profile!=VfxOutputProfile::SpriteAdd||output.shape!=Hash("soft_motes")||
                    output.motion!=VfxMotionKind::RareInwardDrift||
                    !output_recipe.output_color_valid||output_recipe.min_quality>static_cast<std::uint32_t>(input.quality)||
                    !Range(output.parameters,program.parameters)||output.parameters.count!=0)continue;
                for(std::uint64_t ordinal=static_cast<std::uint32_t>(first_value);ordinal<=static_cast<std::uint32_t>(last_value);++ordinal)
                {
                    const double birth=start+static_cast<double>(ordinal)*60.0/rate;
                    if(birth>=end||elapsed<birth||elapsed-birth>=lifetime_ticks)continue;
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
                    mote.gradient_row=output_recipe.gradient_row;mote.hdr=output_recipe.hdr;
                    mote.color=output_recipe.color;
                    if(Finite(mote.position)&&Finite({mote.color.x,mote.color.y,mote.color.z})&&DecodeMask(image,output_index,input,mote))result.push_back(mote);
                }
            }
        }
    }
    return result;
}

namespace
{
std::vector<VfxFlashSpawnInput> BuildVfxPersistentMoteCommandsImpl(
    const VfxImage &image, std::span<const VfxPersistentInput> owners,
    VfxPersistentMoteBirthWindow *birth_window, double current_tick)
{
    const auto &program = image.program;
    std::vector<VfxFlashSpawnInput> result;
    constexpr double lifetime_ticks = kPersistentMoteLifetimeTicks;
    constexpr double lifetime_seconds = lifetime_ticks / 60.0;
    for (const auto &owner : owners)
    {
        if (const auto *circle = std::get_if<VfxCirclePayload>(&owner.payload))
        {
            AppendPersistentTrapMotes(image, owner, *circle, result, birth_window, current_tick);
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
        if (effect.handle != owner.effect_handle || effect.input_mode != 1 ||
            effect.timing_kind != 1 || effect.payload_kind != kEntityPayload ||
            !Range(effect.sources, program.sources)) continue;
        const auto lookup = std::find_if(program.effect_lookup.begin(),
                                         program.effect_lookup.end(),
            [&](const auto &entry) { return entry.handle == effect.handle; });
        if (lookup == program.effect_lookup.end() ||
            std::count_if(program.effect_lookup.begin(), program.effect_lookup.end(),
                [&](const auto &entry) { return entry.handle == effect.handle; }) != 1)
            continue;
        const bool pickup_idle = std::find(kPickupIdleMoteEffects.begin(),
            kPickupIdleMoteEffects.end(), lookup->effect_id) != kPickupIdleMoteEffects.end();
        const bool charged_overcharge = lookup->effect_id == kChargedOverchargeLoop;
        const bool suicide_countdown = lookup->effect_id == Hash64("particle.enemy.suicide.countdown_loop");
        if (!pickup_idle && !charged_overcharge && !suicide_countdown && std::find(kStatusMoteEffects.begin(),
                kStatusMoteEffects.end(), lookup->effect_id) == kStatusMoteEffects.end())
            continue;
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
            const auto &source = program.sources[source_index];
            if (source.effect != effect.handle || source.type != VfxSourceType::CurlMotes ||
                source.stable_id != kAmbientMotes || source.knot_count != 2 ||
                !std::isfinite(source.knots[0]) || !std::isfinite(source.knots[1]) ||
                source.knots[0] < 0.0f || source.knots[1] <= source.knots[0] ||
                source.knots[1] > 1.0f || !Range(source.parameters, program.parameters) ||
                source.parameters.count != 1 || !Range(source.outputs, program.outputs))
                continue;
            if (source_index >= image.mote_source_recipes.size() ||
                !image.mote_source_recipes[source_index].rate_valid) continue;
            const auto rate_value = image.mote_source_recipes[source_index].rate;
            if (rate_value <= 0 || rate_value > 1024 ||
                (charged_overcharge && rate_value != 3) ||
                (pickup_idle && (rate_value != 3 || source.knots[0] != 0.0f ||
                                 source.knots[1] != 1.0f))) continue;
            const float rate = static_cast<float>(rate_value);

            const double start_ticks = std::isfinite(duration_ticks)
                ? static_cast<double>(source.knots[0]) * duration_ticks
                : (source.knots[0] == 0.0f ? 0.0 :
                   std::numeric_limits<double>::infinity());
            const double end_ticks = std::isfinite(duration_ticks)
                ? static_cast<double>(source.knots[1]) * duration_ticks
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
                Mix(source.stable_id));
            const Float3 anchor{owner.current_transform[12], owner.current_transform[13],
                                owner.current_transform[14]};
            if (!Finite(anchor)) continue;

            for (std::uint64_t ordinal = first_ordinal; ordinal <= last_ordinal; ++ordinal)
            {
                const auto birth = PersistentBirthTick(start_ticks, rate, ordinal);
                if (!birth || *birth > current_ticks ||
                    static_cast<double>(*birth) >= end_ticks) continue;
                const double age_ticks = static_cast<double>(current_ticks - *birth);
                if (age_ticks < 0.0 || age_ticks >= lifetime_ticks) continue;

                const auto mote_seed = Mix(source_seed ^
                    Mix(static_cast<std::uint32_t>(ordinal + 1u)));
                Tick global_birth_tick{};
                const double local_birth_age = current_ticks - *birth;
                if (current_tick >= local_birth_age) global_birth_tick = static_cast<Tick>(std::llround(current_tick - local_birth_age));
                const auto unit = birth_window
                    ? birth_window->GetOrCreate(owner.stable_id, owner.status_episode_generation,
                        lookup->effect_id, source.stable_id, static_cast<std::uint32_t>(ordinal),
                        global_birth_tick, mote_seed)
                    : VfxPersistentMoteBirthWindow::UnitBirth{Random(mote_seed, 0), Random(mote_seed, 1)};
                const float angle = 2.0f * std::numbers::pi_v<float> * unit.angle01;
                const float local_radius = entity->footprint_radius *
                    (0.45f + 0.4f * unit.radius01);
                const Float3 local_offset{std::cos(angle) * local_radius,
                                          entity->footprint_radius * .08f,
                                          std::sin(angle) * local_radius};
                const Float3 world_offset = TransformOffset(owner.current_transform, local_offset);
                const Float3 offset{world_offset.x, world_offset.y, world_offset.z};
                const Float3 position = anchor;
                const float inward_scale = static_cast<float>(1.0 / lifetime_seconds);
                const Float3 velocity{-offset.x * inward_scale, -offset.y * inward_scale,
                                      -offset.z * inward_scale};
                const float horizontal = std::hypot(velocity.x, velocity.z);
                if (!Finite(offset) || !Finite(velocity) || !std::isfinite(horizontal) ||
                    horizontal <= 0.0001f) continue;

                for (std::size_t output_index = source.outputs.first;
                     output_index < static_cast<std::size_t>(source.outputs.first) + source.outputs.count;
                     ++output_index)
                {
                    const auto &output = program.outputs[output_index];
                    if (output_index >= image.mote_output_recipes.size()) continue;
                    const auto &output_recipe = image.mote_output_recipes[output_index];
                    if (output.source != source_index || output.profile != VfxOutputProfile::SpriteAdd ||
                        output.shape != kSoftMotes || output.shape_domain != kSpriteDomain ||
                        output.shape_scale_rule != kComponentSize ||
                        output.shape_component_kind != kParticle ||
                        output.coverage_type != kAnalytic || output.coverage_ref != kSoftDisc ||
                        output.motion != VfxMotionKind::RareInwardDrift ||
                        !output_recipe.output_color_valid ||
                        (charged_overcharge && output_recipe.min_quality !=
                            static_cast<std::uint32_t>(VfxQuality::Medium)) ||
                        (pickup_idle && output_recipe.min_quality !=
                            static_cast<std::uint32_t>(VfxQuality::Medium)) ||
                        output_recipe.min_quality > static_cast<std::uint32_t>(owner.quality) ||
                        output_recipe.min_quality > 2)
                        continue;
                    const auto params = DecodePersistentMoteParameters(
                        image, output_index, entity->footprint_radius);
                    if (!params) continue;

                    VfxFlashSpawnInput mote;
                    mote.position = position;
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
                    mote.gradient_row = output_recipe.gradient_row;
                    mote.curve_row = params->curve_row;
                    mote.hdr = output_recipe.hdr;
                    mote.color = output_recipe.color;
                    if (Finite(mote.position) &&
                        Finite({mote.color.x, mote.color.y, mote.color.z}) &&
                        std::isfinite(mote.color.w) && mote.color.w >= 0.0f &&
                        mote.color.w <= 1.0f && DecodeMask(image, output_index, owner, mote))
                        result.push_back(mote);
                }
                if (ordinal == std::numeric_limits<std::uint64_t>::max()) break;
            }
        }
    }
    return result;
}
} // namespace

std::vector<VfxFlashSpawnInput> BuildVfxPersistentMoteCommands(
    const VfxImage &image, std::span<const VfxPersistentInput> owners)
{
    return BuildVfxPersistentMoteCommandsImpl(image, owners, nullptr, 0);
}

std::vector<VfxFlashSpawnInput> BuildVfxPersistentMoteCommands(
    const VfxImage &image, std::span<const VfxPersistentInput> owners,
    VfxPersistentMoteBirthWindow &births, Tick current_tick, float interpolation)
{
    births.Prune(current_tick);
    const double display_tick = static_cast<double>(current_tick) -
        (current_tick != 0 ? 1.0 - interpolation : 0.0);
    return BuildVfxPersistentMoteCommandsImpl(image, owners, &births, display_tick);
}

std::uint64_t VfxPersistentSparkState::Rebind(const VfxProgramView &previous,
                                          const VfxProgramView &next) noexcept
{
    const auto count = cursors_.size();
    std::erase_if(cursors_, [&](Cursor &cursor) {
        const auto old_handle = cursor.effect_handle;
        if (cursor.source_index >= previous.sources.size() ||
            previous.sources[cursor.source_index].effect != old_handle) return true;
        const auto &old_source = previous.sources[cursor.source_index];
        if (RebindVfxGenerationHandle(previous, next, cursor.effect_handle, 1u) !=
            VfxGenerationRebindOutcome::Retained) return true;
        const auto &old_effect = previous.effects[old_handle - 1u];
        const auto &effect = next.effects[cursor.effect_handle - 1u];
        if (effect.timing_kind != 0 || !std::isfinite(effect.seconds) || effect.seconds <= 0 ||
            effect.seconds > 60 || !std::isfinite(old_effect.seconds) || old_effect.seconds <= 0 ||
            old_effect.seconds > 60 || !Range(effect.sources, next.sources)) return true;
        const auto sources = std::span(next.sources).subspan(effect.sources.first, effect.sources.count);
        const auto found = std::find_if(sources.begin(), sources.end(),
            [&](const auto &source) { return source.stable_id == old_source.stable_id; });
        if (found == sources.end() || std::count_if(sources.begin(), sources.end(),
                [&](const auto &source) { return source.stable_id == old_source.stable_id; }) != 1 ||
            found->effect != effect.handle || found->type != old_source.type ||
            found->type != VfxSourceType::CurlMotes || found->stable_id != Hash("micro_sparks") ||
            found->knot_count != 2 || !std::isfinite(found->knots[0]) || !std::isfinite(found->knots[1]) ||
            found->knots[0] < 0 || found->knots[1] > 1 || found->knots[1] <= found->knots[0] ||
            !DecodeSparkParams(next, *found) || !Range(found->outputs, next.outputs) ||
            found->outputs.count != 1) return true;
        const auto &output = next.outputs[found->outputs.first];
        const auto source_index = effect.sources.first + static_cast<std::uint32_t>(found - sources.begin());
        if (output.source != source_index || output.profile != VfxOutputProfile::SpriteAdd ||
            output.shape != Hash("velocity_billboard") || output.motion != VfxMotionKind::SparseRearwardSparks)
            return true;
        const auto old_duration = static_cast<Tick>(std::ceil(static_cast<double>(old_effect.seconds) * 60.0));
        const auto duration = static_cast<Tick>(std::ceil(static_cast<double>(effect.seconds) * 60.0));
        if (cursor.expires_tick < old_duration) return true;
        const auto started = cursor.expires_tick - old_duration;
        if (duration > std::numeric_limits<Tick>::max() - started) return true;
        cursor.effect_handle = effect.handle;
        cursor.source_index = source_index;
        cursor.expires_tick = started + duration;
        // The actual owner clock and already consumed birth ordinals survive.
        return false;
    });
    return count - cursors_.size();
}

std::vector<VfxFlashSpawnInput> VfxPersistentSparkState::Emit(
    const VfxImage &image, std::span<const VfxPersistentInput> owners,
    Tick current_tick, float interpolation)
{
    const auto &program = image.program;
    std::vector<VfxFlashSpawnInput> result;
    // Keep a short tombstone through the authored expiry. A transiently absent
    // snapshot cannot replay the same ordinal if its owner returns this tick.
    std::erase_if(cursors_, [current_tick](const Cursor &cursor) {
        return current_tick >= cursor.expires_tick;
    });
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
        if (effect.handle != owner.effect_handle || effect.input_mode != 1 ||
            effect.timing_kind != 0 || effect.payload_kind != Hash("ProjectilePayload") ||
            !std::isfinite(effect.seconds) || effect.seconds <= 0 || effect.seconds > 60 ||
            owner.elapsed_seconds >= effect.seconds || !Range(effect.sources, program.sources))
            continue;
        const auto lookup = std::find_if(program.effect_lookup.begin(), program.effect_lookup.end(),
            [&](const auto &entry) { return entry.handle == effect.handle; });
        if (lookup == program.effect_lookup.end() ||
            std::find(kSparkEffects.begin(), kSparkEffects.end(), lookup->effect_id) ==
                kSparkEffects.end()) continue;
        for (std::size_t source_index = effect.sources.first;
             source_index < static_cast<std::size_t>(effect.sources.first) + effect.sources.count;
             ++source_index)
        {
            const auto &source = program.sources[source_index];
            if (source.effect != effect.handle || source.type != VfxSourceType::CurlMotes ||
                source.stable_id != Hash("micro_sparks") || source.knot_count != 2 ||
                !std::isfinite(source.knots[0]) || !std::isfinite(source.knots[1]) ||
                source.knots[0] < 0 || source.knots[1] > 1 ||
                source.knots[1] <= source.knots[0] ||
                !Range(source.outputs, program.outputs)) continue;
            if (source_index >= image.mote_source_recipes.size() ||
                !image.mote_source_recipes[source_index].spark_valid) continue;
            const auto &source_recipe = image.mote_source_recipes[source_index];
            const SparkParams params{source_recipe.count, source_recipe.cone_degrees, source_recipe.drag};
            for (std::size_t output_index = source.outputs.first;
                 output_index < static_cast<std::size_t>(source.outputs.first) + source.outputs.count;
                 ++output_index)
            {
                const auto &output = program.outputs[output_index];
                if (output_index >= image.mote_output_recipes.size() ||
                    !image.mote_output_recipes[output_index].spark_textures_valid) continue;
                if (output.source != source_index ||
                    output.profile != VfxOutputProfile::SpriteAdd ||
                    output.shape != Hash("velocity_billboard") ||
                    output.shape_domain != Hash("sprite") ||
                    output.shape_scale_rule != Hash("component_size") ||
                    output.shape_component_kind != Hash("particle") ||
                    output.coverage_type != Hash("analytic") ||
                    output.coverage_ref != Hash("velocity_streak") ||
                    output.motion != VfxMotionKind::SparseRearwardSparks ||
                    image.mote_output_recipes[output_index].min_quality > static_cast<std::uint32_t>(VfxQuality::High) ||
                    !Range(output.parameters, program.parameters) ||
                    output.parameters.count != 0 ||
                    !std::isfinite(output.motion_rate_hz) || output.motion_rate_hz < 0 ||
                    !std::isfinite(output.motion_amplitude) || output.motion_amplitude < 0 ||
                    output.motion_amplitude > 1 ||
                    !std::isfinite(output.motion_inset_fraction) ||
                    output.motion_inset_fraction < 0 || output.motion_inset_fraction > 1 ||
                    !image.mote_output_recipes[output_index].output_color_valid) continue;
                auto cursor = std::find_if(cursors_.begin(), cursors_.end(),
                    [&](const Cursor &entry) {
                        return entry.owner_id == owner.stable_id &&
                            entry.effect_handle == owner.effect_handle &&
                            entry.source_index == source_index;
                    });
                if (cursor == cursors_.end())
                {
                    const auto expiry_offset = static_cast<Tick>(
                        std::ceil(static_cast<double>(effect.seconds) * 60.0));
                    cursors_.push_back({owner.stable_id, owner.effect_handle,
                                        static_cast<std::uint32_t>(source_index), 0,
                                        current_tick - static_cast<Tick>(owner_tick) +
                                            expiry_offset});
                    cursor = std::prev(cursors_.end());
                }
                const double start = static_cast<double>(source.knots[0]) * effect.seconds;
                const double window = static_cast<double>(source.knots[1] - source.knots[0]) *
                    effect.seconds;
                const double spacing = params->count > 1 ?
                    window / static_cast<double>(params->count - 1) : window;
                if (!std::isfinite(spacing) || spacing <= 0) continue;
                while (cursor->next_ordinal < params->count)
                {
                    const auto ordinal = cursor->next_ordinal;
                    const auto birth_tick = static_cast<std::int64_t>(
                        std::ceil((start + spacing * ordinal) * 60.0 - 0.000001));
                    if (owner_tick < birth_tick) break;
                    ++cursor->next_ordinal;
                    if (owner_tick != birth_tick ||
                        image.mote_output_recipes[output_index].min_quality > static_cast<std::uint32_t>(owner.quality)) continue;
                    const auto seed = Mix(owner.stable_seed ^
                        Mix(static_cast<std::uint32_t>(lookup->effect_id)) ^
                        Mix(static_cast<std::uint32_t>(lookup->effect_id >> 32)) ^
                        Mix(source.stable_id) ^ Mix(ordinal + 1));
                    const Float3 direction = SparkDirection(projectile->velocity,
                        params->cone_degrees, seed);
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
                    spark.gradient_row = image.mote_output_recipes[output_index].gradient_row;
                    spark.hdr = image.mote_output_recipes[output_index].hdr;
                    spark.color = image.mote_output_recipes[output_index].color;
                    spark.drag = params->drag;
                    result.push_back(spark);
                }
            }
        }
    }
    return result;
}

}
