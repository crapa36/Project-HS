#include "vfx_typed_flash_commands.hpp"

#include <hs/render_data/render_snapshot.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <string_view>

namespace hs::runtime_detail
{
namespace
{
constexpr std::uint32_t Hash32(std::string_view value) noexcept
{
    std::uint32_t hash = 2166136261u;
    for (const unsigned char character : value) { hash ^= character; hash *= 16777619u; }
    return hash;
}
constexpr std::uint64_t Hash64(std::string_view s)
{std::uint64_t h=14695981039346656037ull;for(unsigned char c:s){h^=c;h*=1099511628211ull;}return h;}
constexpr auto kConePayload = Hash32("ConePayload");
constexpr auto kRingGapsPayload = Hash32("RingWithGapsPayload");
constexpr auto kSecondaryFan = Hash64("particle.upgrade.multishot.secondary_fan");
constexpr auto kMultiShot = Hash64("particle.skill.multishot");
constexpr auto kRearFan = Hash64("particle.upgrade.multishot.rear_fan");
constexpr auto kRetreatTripleRelease = Hash64("particle.upgrade.retreat.triple_release");
constexpr auto kArrowTripleRelease = Hash64("particle.upgrade.arrow.triple_release");
constexpr auto kBossVolleyRelease = Hash64("particle.boss.volley.release");
constexpr auto kBossShockwaveRelease = Hash64("particle.boss.shockwave.release");
constexpr auto kMuzzlePrimary = Hash32("muzzle_primary");
constexpr auto kBossEnergy = Hash32("boss_energy");
constexpr std::array kPickupCollectEffects{
    Hash64("particle.pickup.xp_collect"),
    Hash64("particle.pickup.heal_collect"),
    Hash64("particle.pickup.magnet_collect"),
    Hash64("particle.pickup.relic_collect"),
};
bool Finite(const Float3 &v) noexcept
{ return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
template<class T> bool ValidRange(VfxRange r, std::span<const T> v)
{ return r.first <= v.size() && r.count <= v.size() - r.first; }
}

std::vector<VfxFlashSpawnInput> BuildVfxTypedFlashCommands(const VfxImage &image,
                                                         VfxEventView inputs,
                                                         std::vector<Sequence> *superseded_pickup_sequences)
{
    std::vector<VfxFlashSpawnInput> result;
    const auto cooked=image.program;
    for (const auto &input : inputs)
    {
        if (input.effect_handle == 0 || input.effect_handle > image.flash_effect_ranges.size() ||
            static_cast<std::uint32_t>(input.quality) > 2) continue;
        const auto recipe_range=image.flash_effect_ranges[input.effect_handle-1];
        if(!recipe_range.count || !ValidRange(recipe_range,image.flash_recipes))continue;
        const auto recipes=image.flash_recipes.subspan(recipe_range.first,recipe_range.count);
        const auto &first_recipe=recipes.front();
        if(first_recipe.effect_handle!=input.effect_handle || !std::isfinite(first_recipe.seconds) || first_recipe.seconds<=0)continue;
        const auto effect_id=first_recipe.effect_id;
        Float3 position{}, direction{};
        float scale = 1;
        bool circle_geometry=false, cone_geometry=false, projectile_geometry=false, boss_energy_geometry=false;
        bool pickup_collect = false;
        if (const auto *point = std::get_if<VfxPointPayload>(&input.payload))
        {
            if (first_recipe.payload_kind != Hash32("PointEventPayload")) continue;
            position = point->position; direction = point->direction; scale = point->authored_scale;
        }
        else if (const auto *context = std::get_if<VfxContextPayload>(&input.payload))
        {
            if (first_recipe.payload_kind != Hash32("PresentationContextPayload")) continue;
            position = {input.world_transform[12], input.world_transform[13], input.world_transform[14]};
            direction = context->direction;
        }
        else if(const auto *projectile=std::get_if<VfxProjectilePayload>(&input.payload))
        {
            if(first_recipe.payload_kind!=Hash32("ProjectilePayload")||!Finite(projectile->velocity)||
                !std::isfinite(projectile->hitbox_radius)||projectile->hitbox_radius<=0||
                std::hypot(projectile->velocity.x,projectile->velocity.y,projectile->velocity.z)<=.0001f)continue;
            position={input.world_transform[12],input.world_transform[13],input.world_transform[14]};
            direction=projectile->velocity;scale=projectile->hitbox_radius;projectile_geometry=true;
        }
        else if (const auto *cone = std::get_if<VfxConePayload>(&input.payload))
        {
            if (first_recipe.payload_kind != kConePayload || !Finite(cone->origin) ||
                !Finite(cone->direction) || !std::isfinite(cone->range) || cone->range <= 0.0f ||
                !std::isfinite(cone->half_angle_degrees) || cone->half_angle_degrees <= 0.0f ||
                cone->half_angle_degrees >= 90.0f)
                continue;
            if (effect_id != kSecondaryFan && effect_id != kMultiShot && effect_id != kRearFan &&
                effect_id != kRetreatTripleRelease && effect_id != kArrowTripleRelease && effect_id != kBossVolleyRelease)
                continue;
            position = cone->origin;
            direction = cone->direction;
            scale = 1.0f;
            cone_geometry = true;
            boss_energy_geometry = effect_id == kBossVolleyRelease;
        }
        else if (const auto *ring = std::get_if<VfxRingGapsPayload>(&input.payload))
        {
            if (first_recipe.payload_kind != kRingGapsPayload || !Finite(ring->center) ||
                !std::isfinite(ring->inner_radius) || ring->inner_radius <= 0.0f ||
                !std::isfinite(ring->outer_radius) || ring->outer_radius <= ring->inner_radius ||
                !std::isfinite(ring->gap_half_width_degrees) || ring->gap_half_width_degrees < 0.0f ||
                ring->gap_half_width_degrees > 180.0f ||
                (!ring->gap_angles_degrees.empty() && ring->gap_half_width_degrees == 0.0f) ||
                std::any_of(ring->gap_angles_degrees.begin(), ring->gap_angles_degrees.end(),
                    [](float angle) { return !std::isfinite(angle); }))
                continue;
            if (effect_id != kBossShockwaveRelease) continue;
            position = ring->center;
            direction = {0.0f, 0.0f, 1.0f};
            scale = 1.0f;
            boss_energy_geometry = true;
        }
        else if(const auto *circle=std::get_if<VfxCirclePayload>(&input.payload))
        {
            if(first_recipe.payload_kind!=Hash32("CircleAreaPayload")||circle->inner_radius!=0)continue;
            position=circle->center;direction=circle->direction;scale=circle->radius;circle_geometry=true;
        }
        else if (const auto *link = std::get_if<VfxLinkPayload>(&input.payload))
        {
            if (first_recipe.payload_kind != Hash32("SourceTargetPayload") ||
                std::find(kPickupCollectEffects.begin(), kPickupCollectEffects.end(),
                          effect_id) == kPickupCollectEffects.end() ||
                !first_recipe.unique_effect_identity ||
                !Finite(link->source_position) || !Finite(link->target_position) ||
                input.sequence == 0)
                continue;
            position = link->source_position;
            direction = {link->target_position.x - position.x,
                         link->target_position.y - position.y,
                         link->target_position.z - position.z};
            pickup_collect = true;
        }
        else continue;
        if (!Finite(position) || !Finite(direction) || !std::isfinite(scale) || scale <= 0) continue;
        const float magnitude = std::hypot(direction.x, direction.y, direction.z);
        if (!std::isfinite(magnitude) || (cone_geometry && magnitude <= 0.0001f)) continue;
        direction = magnitude > 0.0001f ? Float3{direction.x / magnitude, direction.y / magnitude, direction.z / magnitude}
                                      : Float3{0, 0, 1};
        const auto first_flash = result.size();
        for (const auto &recipe : recipes)
        {
            if(recipe.effect_handle!=input.effect_handle || !(recipe.supported_quality_mask&(1u<<static_cast<unsigned>(input.quality))))continue;
            const bool contact=recipe.spawn.shape==VfxImpactShape::ThreeProngBite||recipe.spawn.shape==VfxImpactShape::BossCrestContact||recipe.spawn.shape==VfxImpactShape::NeedleStar;
            if(contact!=projectile_geometry)continue;
            if(boss_energy_geometry&&(recipe.source_stable_id!=kBossEnergy||recipe.spawn.shape!=VfxImpactShape::RadialEnergy))continue;
            if(cone_geometry&&!boss_energy_geometry&&(recipe.source_stable_id!=kMuzzlePrimary||recipe.spawn.shape!=VfxImpactShape::RadialSlash))continue;
            if(contact)
            {
                const auto expected=recipe.spawn.shape==VfxImpactShape::ThreeProngBite?Hash64("particle.enemy.ranged.impact"):
                    recipe.spawn.shape==VfxImpactShape::BossCrestContact?Hash64("particle.boss.volley.projectile_impact"):Hash64("particle.skill.charged_shot.impact");
                if(effect_id!=expected)continue;
            }
            auto spawn=recipe.spawn;
            spawn.size=circle_geometry?scale:spawn.size*scale;
            spawn.ground_base_anchor=circle_geometry&&(spawn.shape==VfxImpactShape::SoftDisc||spawn.shape==VfxImpactShape::NoiseDisc||
                spawn.shape==VfxImpactShape::Flame||spawn.shape==VfxImpactShape::Smoke6Way||spawn.shape==VfxImpactShape::RadialEnergy);
            spawn.position=position;spawn.direction=direction;
            spawn.event_tick=input.event_tick;spawn.stable_seed=input.stable_seed;
            if(spawn.oit&&spawn.shape==VfxImpactShape::Smoke6Way&&input.quality!=VfxQuality::High)spawn.motion_strength=0;
            if(recipe.mask_present_mask&(1u<<static_cast<unsigned>(input.quality)))
            {
                const auto slices=recipe.mask_slice_ranges[static_cast<unsigned>(input.quality)];
                if(!ValidRange(slices,cooked.slice_indices)||!slices.count)continue;
                spawn.mask_slice=cooked.slice_indices[slices.first+(input.stable_seed%slices.count)];
                spawn.mask_strength=recipe.mask_strengths[static_cast<unsigned>(input.quality)];
            }
            if (!std::isfinite(spawn.size) || spawn.size <= 0) continue;
            result.push_back(spawn);
        }
        if (pickup_collect && result.size() != first_flash && superseded_pickup_sequences)
            superseded_pickup_sequences->push_back(input.sequence);
    }
    return result;
}

std::vector<VfxFlashSpawnInput> BuildVfxPersistentFireFlameCommands(
    const VfxImage &image, std::span<const VfxPersistentInput> inputs, double current_tick)
{
    std::vector<VfxFlashSpawnInput> result;
    const auto mix = [](std::uint32_t v) {
        v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; return v ^ (v >> 16);
    };
    const auto random = [&](std::uint32_t seed, std::uint32_t lane) {
        return static_cast<float>(mix(seed ^ mix(lane + 0x9e3779b9u)) >> 8) / 16777216.0f;
    };
    for (const auto &owner : inputs)
    {
        if (owner.effect_handle == 0 || owner.effect_handle > image.persistent_fire_effect_ranges.size() ||
            owner.source_visual_kind != static_cast<std::uint8_t>(PersistentVfxKind::FireArea) ||
            owner.stable_id == 0 || owner.quality != VfxQuality::High || !std::isfinite(owner.elapsed_seconds) ||
            owner.elapsed_seconds < 0 || !std::isfinite(owner.source_duration_seconds) ||
            owner.source_duration_seconds <= 0 || owner.elapsed_seconds >= owner.source_duration_seconds) continue;
        const auto *circle = std::get_if<VfxCirclePayload>(&owner.payload);
        if (!circle || !Finite(circle->center) || !std::isfinite(circle->radius) || circle->radius <= 0 ||
            circle->inner_radius != 0) continue;
        const auto range = image.persistent_fire_effect_ranges[owner.effect_handle - 1];
        if (!range.count || !ValidRange(range, image.persistent_fire_recipes)) continue;
        const double elapsed_ticks = static_cast<double>(owner.elapsed_seconds) * 60;
        const double duration_ticks = static_cast<double>(owner.source_duration_seconds) * 60;
        if (elapsed_ticks > current_tick + .01 || duration_ticks > std::numeric_limits<std::uint32_t>::max() ||
            std::abs(duration_ticks - std::round(duration_ticks)) > .01) continue;
        const double elapsed = elapsed_ticks;
        const auto duration = static_cast<std::uint64_t>(std::llround(duration_ticks));
        const Tick started = static_cast<Tick>(std::llround(std::max(0.0, current_tick - elapsed)));
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
                const auto slices = image.program.slice_indices.subspan(recipe.mask_slices.first, recipe.mask_slices.count);
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
                const float radius = std::sqrt(random(tongue.stable_seed, 1)) * circle->radius * recipe.radius_scale;
                tongue.initial_offset = {std::cos(angle) * radius, 0, std::sin(angle) * radius};
                tongue.lifetime = static_cast<float>(duration - birth) / 60;
                tongue.normalized_age = static_cast<float>(elapsed - birth) / static_cast<float>(duration - birth);
                tongue.event_tick = started + birth;
                tongue.initial_velocity = {-tongue.initial_offset.x / tongue.lifetime,
                    tongue.size / tongue.lifetime, -tongue.initial_offset.z / tongue.lifetime};
                if (Finite(tongue.initial_velocity)) result.push_back(tongue);
            }
        }
    }
    return result;
}

std::vector<VfxFlashSpawnInput> BuildVfxPersistentStatusStampCommands(
    const VfxImage &image, std::span<const VfxPersistentInput> inputs)
{
    std::vector<VfxFlashSpawnInput> result;
    for (const auto &input : inputs)
    {
        if (input.stable_id == 0 || input.effect_handle == 0 ||
            input.effect_handle > image.persistent_stamp_effect_ranges.size() ||
            static_cast<std::uint32_t>(input.quality) > 2 ||
            !std::isfinite(input.source_duration_seconds) || input.source_duration_seconds <= 0.0f ||
            !std::isfinite(input.elapsed_seconds) || input.elapsed_seconds < 0.0f ||
            !std::isfinite(input.normalized_age) || input.normalized_age < 0.0f || input.normalized_age >= 1.0f)
            continue;
        const auto range = image.persistent_stamp_effect_ranges[input.effect_handle - 1];
        if (!range.count || !ValidRange(range, image.persistent_stamp_recipes)) continue;
        const auto *entity = std::get_if<VfxEntityPayload>(&input.payload);
        const auto *path = std::get_if<VfxProjectilePathPayload>(&input.payload);
        for (const auto &recipe : image.persistent_stamp_recipes.subspan(range.first, range.count))
        {
            if (recipe.effect_handle != input.effect_handle || recipe.source_visual_kind != input.source_visual_kind ||
                recipe.payload_kind != (path ? Hash32("ProjectilePathPayload") : Hash32("EntityAttachmentPayload")) ||
                (recipe.timing_kind == 0 && recipe.fixed_seconds != input.source_duration_seconds) ||
                input.normalized_age >= recipe.source_end) continue;
            Float3 position{}, previous_position{};
            float footprint{};
            if (recipe.payload_kind == Hash32("ProjectilePathPayload"))
            {
                if (!path || !Finite(path->current_position) || !Finite(path->previous_position) ||
                    !Finite(path->velocity) || !std::isfinite(path->projectile_radius) ||
                    path->projectile_radius <= 0.0f || !std::isfinite(path->lifetime01) ||
                    path->lifetime01 != input.normalized_age) continue;
                position = path->current_position;
                previous_position = path->previous_position;
                footprint = path->projectile_radius;
            }
            else
            {
                if (!entity || entity->render_instance_id == 0 || !std::isfinite(entity->footprint_radius) ||
                    entity->footprint_radius <= 0.0f || !std::isfinite(entity->lifetime01) ||
                    entity->lifetime01 != input.normalized_age || !std::isfinite(entity->health_fraction) ||
                    entity->health_fraction < 0.0f || entity->health_fraction > 1.0f) continue;
                position = {input.current_transform[12], input.current_transform[13], input.current_transform[14]};
                previous_position = {input.previous_transform[12], input.previous_transform[13], input.previous_transform[14]};
                footprint = entity->footprint_radius;
            }
            if (!Finite(position) || !Finite(previous_position)) continue;
            auto flash = recipe.spawn;
            flash.position = position;
            flash.previous_position = previous_position;
            flash.interpolate_owner_position = true;
            flash.size = footprint;
            flash.normalized_age = input.normalized_age / recipe.source_end;
            flash.lifetime = input.source_duration_seconds * recipe.source_end;
            flash.stable_seed = input.stable_seed;
            const auto quality = static_cast<std::uint32_t>(input.quality);
            if (quality < 3 && (recipe.mask_present_mask & (1u << quality)))
            {
                const auto mask = recipe.mask_slices[quality];
                if (!mask.count || mask.first > image.program.slice_indices.size() ||
                    mask.count > image.program.slice_indices.size() - mask.first) continue;
                const auto slices = image.program.slice_indices.subspan(mask.first, mask.count);
                flash.mask_slice = slices[input.stable_seed % slices.size()];
                flash.mask_strength = recipe.mask_strengths[quality];
            }
            if (!std::isfinite(flash.lifetime) || flash.lifetime <= 0.0f ||
                !Finite({flash.color.x, flash.color.y, flash.color.z})) continue;
            result.push_back(flash);
        }
    }
    return result;
}

std::vector<VfxOwnedStatusTickFlashCommand> BuildVfxOwnedStatusTickFlashCommands(
    const VfxImage &image, VfxEventView events, std::span<const VfxPersistentInput> persistent)
{
    std::vector<VfxOwnedStatusTickFlashCommand> result;
    for (const auto &event : events)
    {
        if (event.effect_handle == 0 || event.effect_handle > image.owned_status_tick_effect_ranges.size() ||
            event.geometry_owner_id == 0 || event.geometry_owner_id >= (1ull << 60) ||
            event.status_episode_generation == 0 || static_cast<std::uint32_t>(event.quality) > 2 ||
            !std::holds_alternative<VfxPointPayload>(event.payload)) continue;
        const auto range = image.owned_status_tick_effect_ranges[event.effect_handle - 1];
        if (!range.count || !ValidRange(range, image.owned_status_tick_recipes)) continue;
        const auto render_id = (2ull << 60) | event.geometry_owner_id;
        bool emitted = false;
        for (const auto &owner : persistent)
        {
            if (owner.effect_handle == 0 || static_cast<std::uint32_t>(owner.quality) > 2 ||
                owner.stable_id == 0 || owner.status_episode_generation != event.status_episode_generation ||
                !std::isfinite(owner.elapsed_seconds) || owner.elapsed_seconds < 0.0f ||
                !std::isfinite(owner.normalized_age) || owner.normalized_age < 0.0f || owner.normalized_age >= 1.0f)
                continue;
            const auto *entity = std::get_if<VfxEntityPayload>(&owner.payload);
            if (!entity || entity->render_instance_id != render_id ||
                !std::isfinite(entity->footprint_radius) || entity->footprint_radius <= 0.0f ||
                !std::isfinite(entity->lifetime01) || entity->lifetime01 != owner.normalized_age) continue;
            const Float3 position{owner.current_transform[12], owner.current_transform[13], owner.current_transform[14]};
            if (!Finite(position)) continue;
            for (const auto &recipe : image.owned_status_tick_recipes.subspan(range.first, range.count))
            {
                if (recipe.tick_effect_handle != event.effect_handle ||
                    recipe.status_effect_handle != owner.effect_handle ||
                    recipe.min_quality > static_cast<std::uint32_t>(event.quality) ||
                    recipe.min_quality > static_cast<std::uint32_t>(owner.quality)) continue;
                auto flash = recipe.spawn;
                flash.size = entity->footprint_radius;
                flash.position = position;
                flash.delay = recipe.tick_seconds * recipe.delay_fraction;
                flash.lifetime = recipe.tick_seconds * recipe.lifetime_fraction;
                flash.event_tick = event.event_tick;
                flash.stable_seed = event.stable_seed;
                if (recipe.bleed)
                {
                    flash.mask_slice = recipe.mask_slices[event.stable_seed % recipe.mask_slices.size()];
                    flash.mask_strength = recipe.mask_strength;
                }
                result.push_back({event.sequence, flash});
                emitted = true;
                break;
            }
            if (emitted) break;
        }
    }
    return result;
}} // namespace hs::runtime_detail
