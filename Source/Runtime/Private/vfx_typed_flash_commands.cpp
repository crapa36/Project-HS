#include "vfx_typed_flash_commands.hpp"

#include <hs/core/render_snapshot.hpp>

#include <algorithm>
#include <array>
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
constexpr auto kBleedTick = Hash64("particle.status.bleed_tick");
constexpr auto kBurnTick = Hash64("particle.status.burn_tick");
constexpr auto kBleedStatus = Hash64("persistent.status.bleed");
constexpr auto kBurnStatus = Hash64("persistent.status.burn");
constexpr auto kMultishotRetargetEffect = Hash64("particle.upgrade.retarget");
constexpr auto kRicochetBleedExtendEffect = Hash64("particle.upgrade.ricochet.bleed_extend");
constexpr std::array kPickupCollectEffects{
    Hash64("particle.pickup.xp_collect"),
    Hash64("particle.pickup.heal_collect"),
    Hash64("particle.pickup.magnet_collect"),
    Hash64("particle.pickup.relic_collect"),
};
constexpr auto kEntityPayload = Hash32("EntityAttachmentPayload");
constexpr auto kProjectilePathPayload = Hash32("ProjectilePathPayload");
constexpr auto kTickCore = Hash32("tick_core");
constexpr auto kSmallPulse = Hash32("small_pulse");
constexpr auto kStatusStamp = Hash32("status_stamp");
constexpr auto kStatusSymbol = Hash32("status_symbol_sdf");
bool Finite(const Float3 &v) noexcept
{ return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
template<class T> bool ValidRange(VfxRange r, const std::vector<T> &v)
{ return r.first <= v.size() && r.count <= v.size() - r.first; }
struct Recipe { std::uint32_t shape; VfxMotionKind motion; VfxImpactShape kind; float size; };
constexpr Recipe kRecipes[]{
    {Hash32("soft_disc"), VfxMotionKind::InstantPointFlash, VfxImpactShape::SoftDisc, .35f},
    {Hash32("directional_slash_sdf"), VfxMotionKind::TwoFrameSnapPerpendicular, VfxImpactShape::CrossSlash, .22f},
    {Hash32("soft_disc_noise"), VfxMotionKind::InstantExpansionCollapse, VfxImpactShape::NoiseDisc, .32f},
    {Hash32("status_symbol_sdf"), VfxMotionKind::FastStampBreakout, VfxImpactShape::StatusStamp, .25f},
    {Hash32("radial_slash_sdf"), VfxMotionKind::SnapsOutwardAim, VfxImpactShape::RadialSlash, .30f},
    {Hash32("small_pulse"), VfxMotionKind::SingleShortPulse, VfxImpactShape::Pulse, .25f},
    {Hash32("small_star_ring"), VfxMotionKind::SnapsInwardUpward, VfxImpactShape::StarRing, .4f},
    {Hash32("warped_flame_disc"), VfxMotionKind::RadialLobesUpward, VfxImpactShape::Flame, .45f},
    {Hash32("smoke_billboard_6way"), VfxMotionKind::BakedSixWayPlume, VfxImpactShape::Smoke6Way, .65f},
    {Hash32("radial_energy"), VfxMotionKind::DirectionalTurbulence, VfxImpactShape::RadialEnergy, .4f},
    {Hash32("three_prong_bite_sdf"), VfxMotionKind::CompressSnapOutward, VfxImpactShape::ThreeProngBite, 1},
    {Hash32("boss_crest_contact_sdf"), VfxMotionKind::SingleFrameSnapShrink, VfxImpactShape::BossCrestContact, 1},
    {Hash32("needle_star_sdf"), VfxMotionKind::InstantCompressionAxialSplit, VfxImpactShape::NeedleStar, 1}
};
bool Parameters(const VfxProgramData &c, const VfxOutputRecord &o, VfxFlashSpawnInput &spawn)
{
    if (!ValidRange(o.parameters, c.parameters)) return false;
    const auto params = std::span(c.parameters).subspan(o.parameters.first, o.parameters.count);
    auto find = [&](std::string_view name) -> const VfxParameterRecord * {
        const auto key = Hash32(name);
        const auto it = std::find_if(params.begin(), params.end(), [=](const auto &p) { return p.key == key; });
        return it == params.end() ? nullptr : &*it;
    };
    // Reject malformed floating values even when a parameter is not used by this shape.
    for (const auto &p : params)
        if (p.type == VfxParameterType::Float && !std::isfinite(std::bit_cast<float>(p.bits))) return false;
    auto scalar = [&](std::string_view name, float &value, bool required) {
        const auto *p = find(name);
        if (!p) return !required;
        if (p->type != VfxParameterType::Float) return false;
        value = std::bit_cast<float>(p->bits);
        return std::isfinite(value) && value > 0;
    };
    auto enumeration = [&](std::string_view name, std::string_view value) {
        const auto *p = find(name);
        return p && p->type == VfxParameterType::Enum && p->bits == Hash32(value);
    };
    if (!scalar("size", spawn.size, spawn.shape == VfxImpactShape::SoftDisc) ||
        !scalar("aspect", spawn.aspect, spawn.shape == VfxImpactShape::CrossSlash)) return false;
    const bool needs_curve = spawn.shape == VfxImpactShape::NoiseDisc || spawn.shape == VfxImpactShape::StatusStamp ||
        spawn.shape == VfxImpactShape::RadialSlash || spawn.shape == VfxImpactShape::Pulse ||
        spawn.shape == VfxImpactShape::ThreeProngBite || spawn.shape == VfxImpactShape::BossCrestContact ||
        spawn.shape == VfxImpactShape::NeedleStar;
    if (const auto *p = find("scale_curve_ref"))
    {
        if (p->type != VfxParameterType::CurveRow || p->bits >= 7) return false;
        spawn.curve_row = p->bits;
    }
    else if (needs_curve) return false;
    if (spawn.shape == VfxImpactShape::CrossSlash && !enumeration("sdf", "cross_slash")) return false;
    if (spawn.shape == VfxImpactShape::NoiseDisc && !enumeration("noise", "fbm")) return false;
    if (spawn.shape == VfxImpactShape::RadialSlash && !enumeration("sdf", "asymmetric_star_slash")) return false;
    if (spawn.shape == VfxImpactShape::StarRing && !enumeration("sdf", "star_ring")) return false;
    return true;
}
bool OitParameters(const VfxProgramData &c, const VfxOutputRecord &o, const VfxEventInput &input,
                   VfxFlashSpawnInput &spawn)
{
    if (!ValidRange(o.parameters, c.parameters)) return false;
    const auto params=std::span(c.parameters).subspan(o.parameters.first,o.parameters.count);
    const auto find=[&](std::string_view key)->const VfxParameterRecord* {
        const auto hash=Hash32(key);const VfxParameterRecord *found=nullptr;
        for(const auto &p:params)if(p.key==hash){if(found)return nullptr;found=&p;}return found;
    };
    const auto scalar=[&](std::string_view key,float &value){const auto *p=find(key);
        if(!p||p->type!=VfxParameterType::Float)return false;value=std::bit_cast<float>(p->bits);
        return std::isfinite(value)&&value>=0;};
    spawn.oit=true;
    if(spawn.shape!=VfxImpactShape::Smoke6Way)
    {
        const bool flame=spawn.shape==VfxImpactShape::Flame;
        const auto *octaves=find("fbm_octaves");
        if(params.size()!=(flame?2u:1u)||!octaves||octaves->type!=VfxParameterType::Int||
            octaves->bits<1||octaves->bits>8)return false;
        spawn.fbm_octaves=octaves->bits;
        return !flame||scalar("domain_warp",spawn.domain_warp);
    }
    const auto *clip=find("clip_index"),*blend=find("motion_blend");float strength{};
    if(params.size()!=5||!clip||clip->type!=VfxParameterType::Int||clip->bits>1||
        !blend||blend->type!=VfxParameterType::Enum||blend->bits!=Hash32("high_only")||
        !scalar("frame_rate",spawn.smoke_fps)||spawn.smoke_fps<=0||
        !scalar("opacity_scale",spawn.opacity_scale)||spawn.opacity_scale<=0||
        !scalar("motion_blend_strength",strength)||strength>1||
        o.coverage_type!=Hash32("flipbook_6way")||!ValidRange(o.textures,c.texture_bindings))return false;
    const VfxTextureBindingRecord *positive=nullptr,*negative=nullptr,*motion=nullptr;
    for(const auto &binding:std::span(c.texture_bindings).subspan(o.textures.first,o.textures.count))
    {
        if(binding.role!=Hash32("coverage")&&binding.role!=Hash32("flipbook_motion_vectors"))continue;
        const auto resource=std::find_if(c.texture_resources.begin(),c.texture_resources.end(),
            [&](const auto &r){return r.slot==binding.catalog_slot;});
        if(resource==c.texture_resources.end()||!ValidRange(resource->asset_path_bytes,c.strings))return false;
        const std::string_view path(reinterpret_cast<const char*>(c.strings.data())+resource->asset_path_bytes.first,resource->asset_path_bytes.count);
        const VfxTextureBindingRecord **target=nullptr;
        if(binding.role==Hash32("coverage"))
        {
            if(binding.min_quality>static_cast<std::uint32_t>(input.quality))return false;
            if(path=="Content/Textures/VFX/vfx_smoke6way_pos.dds")target=&positive;
            else if(path=="Content/Textures/VFX/vfx_smoke6way_neg.dds")target=&negative;
            else return false;
        }
        else
        {
            if(path!="Content/Textures/VFX/vfx_smoke_motion_vectors.dds"||binding.min_quality!=2)return false;
            target=&motion;
        }
        if(*target)return false;*target=&binding;
    }
    if(!positive||!negative||!motion||positive->frame_count==0||positive->first_frame>=64||
        positive->frame_count>64-positive->first_frame||positive->first_frame!=negative->first_frame||
        positive->frame_count!=negative->frame_count||!std::isfinite(positive->fps)||
        positive->fps!=negative->fps||positive->fps!=spawn.smoke_fps||
        positive->first_frame!=clip->bits*32||positive->frame_count!=32)return false;
    spawn.smoke_first_frame=positive->first_frame;spawn.smoke_frame_count=positive->frame_count;
    spawn.motion_strength=input.quality==VfxQuality::High?strength:0;
    return true;
}

bool Mask(const VfxProgramData &c, const VfxOutputRecord &o, const VfxEventInput &input,
          VfxFlashSpawnInput &spawn)
{
    if (!ValidRange(o.textures, c.texture_bindings)) return false;
    for (const auto &binding : std::span(c.texture_bindings).subspan(o.textures.first, o.textures.count))
    {
        if (binding.role != Hash32("authored_mask_array")) continue;
        if (binding.min_quality > 2 || !std::isfinite(binding.strength) || binding.strength < 0 || binding.strength > 1)
            return false;
        if (binding.min_quality > static_cast<std::uint32_t>(input.quality)) continue;
        const auto resource = std::find_if(c.texture_resources.begin(), c.texture_resources.end(),
            [&](const auto &r) { return r.slot == binding.catalog_slot; });
        if (resource == c.texture_resources.end() || !ValidRange(resource->asset_path_bytes, c.strings)) return false;
        const std::string_view path(reinterpret_cast<const char *>(c.strings.data()) + resource->asset_path_bytes.first,
                                    resource->asset_path_bytes.count);
        if (path != "Content/Textures/VFX/vfx_authored_mask_array.dds" ||
            spawn.mask_slice != 0xffffffffu || !ValidRange(binding.slices, c.slice_indices) || binding.slices.count == 0 ||
            binding.selection != Hash32("stable_seed_mod_group_size")) return false;
        for (const auto slice : std::span(c.slice_indices).subspan(binding.slices.first, binding.slices.count))
            if (slice >= 12) return false;
        spawn.mask_slice = c.slice_indices[binding.slices.first + (input.stable_seed % binding.slices.count)];
        spawn.mask_strength = binding.strength;
    }
    return true;
}

bool HasUniqueIdentity(const VfxProgramData &cooked, VfxEffectHandle handle,
                       std::uint64_t effect_id)
{
    return std::count_if(cooked.effect_lookup.begin(), cooked.effect_lookup.end(),
        [=](const auto &entry) { return entry.handle == handle && entry.effect_id == effect_id; }) == 1 &&
        std::count_if(cooked.effect_lookup.begin(), cooked.effect_lookup.end(),
        [=](const auto &entry) { return entry.handle == handle; }) == 1 &&
        std::count_if(cooked.effect_lookup.begin(), cooked.effect_lookup.end(),
        [=](const auto &entry) { return entry.effect_id == effect_id; }) == 1;
}

std::uint64_t StatusEffectId(std::uint8_t kind)
{
    switch (static_cast<PersistentVfxKind>(kind))
    {
    case PersistentVfxKind::PlayerBowDraw: return Hash64("particle.player.bow_draw");
    case PersistentVfxKind::ChargedFullReady: return Hash64("particle.upgrade.charged.full_ready");
    case PersistentVfxKind::EmpoweredReady: return Hash64("particle.upgrade.empowered_ready");
    case PersistentVfxKind::MultishotRetarget: return kMultishotRetargetEffect;
    case PersistentVfxKind::RicochetBleedExtend: return kRicochetBleedExtendEffect;
    default: return 0;
    }
}

bool StatusStampTextures(const VfxProgramData &cooked, const VfxOutputRecord &output,
                         bool bleed_extend)
{
    const bool texture_count_ok = bleed_extend
        ? output.textures.count == 4
        : (output.textures.count == 2 || output.textures.count == 3);
    if (!ValidRange(output.textures, cooked.texture_bindings) || !texture_count_ok) return false;
    bool gradient = false, curve = false, optional_mask = false, authored_mask = false;
    for (const auto &binding : std::span(cooked.texture_bindings).subspan(
             output.textures.first, output.textures.count))
    {
        if (binding.role == Hash32("profile.optional_detail"))
        {
            if (optional_mask || binding.min_quality != 0 || binding.selection != 0 ||
                binding.slices.count != 0 || !std::isfinite(binding.strength) ||
                binding.strength != 1.0f) return false;
            const auto resource = std::find_if(cooked.texture_resources.begin(),
                cooked.texture_resources.end(), [&](const auto &entry) {
                    return entry.slot == binding.catalog_slot;
                });
            if (resource == cooked.texture_resources.end() ||
                !ValidRange(resource->asset_path_bytes, cooked.strings)) return false;
            const std::string_view path(
                reinterpret_cast<const char *>(cooked.strings.data()) +
                    resource->asset_path_bytes.first,
                resource->asset_path_bytes.count);
            if (path != "Content/Textures/VFX/vfx_authored_mask_array.dds") return false;
            optional_mask = true;
            continue;
        }
        if (bleed_extend && binding.role == Hash32("authored_mask_array"))
        {
            if (authored_mask || binding.min_quality != 0 ||
                !std::isfinite(binding.strength) || binding.strength < 0.0f ||
                binding.strength > 1.0f ||
                binding.selection != Hash32("stable_seed_mod_group_size") ||
                !ValidRange(binding.slices, cooked.slice_indices) ||
                binding.slices.count != 3)
                return false;
            const auto resource = std::find_if(cooked.texture_resources.begin(),
                cooked.texture_resources.end(), [&](const auto &entry) {
                    return entry.slot == binding.catalog_slot;
                });
            if (resource == cooked.texture_resources.end() ||
                !ValidRange(resource->asset_path_bytes, cooked.strings)) return false;
            const std::string_view path(
                reinterpret_cast<const char *>(cooked.strings.data()) +
                    resource->asset_path_bytes.first,
                resource->asset_path_bytes.count);
            if (path != "Content/Textures/VFX/vfx_authored_mask_array.dds") return false;
            const auto slices = std::span(cooked.slice_indices).subspan(
                binding.slices.first, binding.slices.count);
            if (slices[0] != 3 || slices[1] != 4 || slices[2] != 5) return false;
            authored_mask = true;
            continue;
        }
        if (binding.role != Hash32("profile.global") || binding.min_quality != 0 ||
            binding.selection != 0 || binding.slices.count != 0 ||
            !std::isfinite(binding.strength) || binding.strength != 1.0f) return false;
        const auto resource = std::find_if(cooked.texture_resources.begin(), cooked.texture_resources.end(),
            [&](const auto &entry) { return entry.slot == binding.catalog_slot; });
        if (resource == cooked.texture_resources.end() ||
            !ValidRange(resource->asset_path_bytes, cooked.strings)) return false;
        const std::string_view path(
            reinterpret_cast<const char *>(cooked.strings.data()) + resource->asset_path_bytes.first,
            resource->asset_path_bytes.count);
        if (path == "Content/Textures/VFX/vfx_gradient_lut.dds" && !gradient) gradient = true;
        else if (path == "Content/Textures/VFX/vfx_curve_lut.dds" && !curve) curve = true;
        else return false;
    }
    return gradient && curve && (!bleed_extend || (optional_mask && authored_mask));
}

bool StatusStampCurve(const VfxProgramData &cooked, const VfxOutputRecord &output)
{
    if (!ValidRange(output.parameters, cooked.parameters) || output.parameters.count != 1) return false;
    const auto &parameter = cooked.parameters[output.parameters.first];
    if (parameter.key != Hash32("scale_curve_ref") ||
        parameter.type != VfxParameterType::CurveRow || parameter.bits != 4 ||
        parameter.bits >= cooked.curves.size()) return false;
    const auto &curve = cooked.curves[parameter.bits];
    if (curve.domain != Hash32("normalized_age") || curve.interpolation != 2 ||
        !ValidRange(curve.keys, cooked.curve_keys) || curve.keys.count < 2) return false;
    float previous = -1.0f;
    for (const auto &key : std::span(cooked.curve_keys).subspan(curve.keys.first, curve.keys.count))
    {
        if (!std::isfinite(key.time) || !std::isfinite(key.value) ||
            key.time <= previous || key.time > 1.0f || key.value < 0.0f) return false;
        previous = key.time;
    }
    return cooked.curve_keys[curve.keys.first].time == 0.0f && previous == 1.0f;
}

bool StatusTickCurve(const VfxProgramData &cooked, const VfxOutputRecord &output)
{
    if (!ValidRange(output.parameters, cooked.parameters) || output.parameters.count != 1) return false;
    const auto &parameter = cooked.parameters[output.parameters.first];
    if (parameter.key != Hash32("scale_curve_ref") ||
        parameter.type != VfxParameterType::CurveRow || parameter.bits != 6 ||
        parameter.bits >= cooked.curves.size()) return false;
    const auto &curve = cooked.curves[parameter.bits];
    if (curve.domain != Hash32("normalized_age") || curve.interpolation != 2 ||
        !ValidRange(curve.keys, cooked.curve_keys) || curve.keys.count < 2) return false;
    float previous = -1.0f;
    for (const auto &key : std::span(cooked.curve_keys).subspan(curve.keys.first, curve.keys.count))
    {
        if (!std::isfinite(key.time) || !std::isfinite(key.value) ||
            key.time <= previous || key.time > 1.0f || key.value < 0.0f) return false;
        previous = key.time;
    }
    return cooked.curve_keys[curve.keys.first].time == 0.0f && previous == 1.0f;
}

bool StatusTickTextures(const VfxProgramData &cooked, const VfxOutputRecord &output,
                        bool bleed, const VfxEventInput &event, VfxFlashSpawnInput &flash)
{
    if (!ValidRange(output.textures, cooked.texture_bindings) ||
        output.textures.count != (bleed ? 3u : 2u)) return false;
    bool gradient = false, curve = false, mask = false;
    for (const auto &binding : std::span(cooked.texture_bindings).subspan(
             output.textures.first, output.textures.count))
    {
        const auto resource = std::find_if(cooked.texture_resources.begin(), cooked.texture_resources.end(),
            [&](const auto &entry) { return entry.slot == binding.catalog_slot; });
        if (resource == cooked.texture_resources.end() ||
            !ValidRange(resource->asset_path_bytes, cooked.strings)) return false;
        const std::string_view path(
            reinterpret_cast<const char *>(cooked.strings.data()) + resource->asset_path_bytes.first,
            resource->asset_path_bytes.count);
        if (binding.role == Hash32("profile.global") && binding.min_quality == 0 &&
            binding.selection == 0 && binding.slices.count == 0 &&
            std::isfinite(binding.strength) && binding.strength == 1.0f &&
            path == "Content/Textures/VFX/vfx_gradient_lut.dds" && !gradient)
            gradient = true;
        else if (binding.role == Hash32("profile.global") && binding.min_quality == 0 &&
                 binding.selection == 0 && binding.slices.count == 0 &&
                 std::isfinite(binding.strength) && binding.strength == 1.0f &&
                 path == "Content/Textures/VFX/vfx_curve_lut.dds" && !curve)
            curve = true;
        else if (bleed && binding.role == Hash32("authored_mask_array") &&
                 path == "Content/Textures/VFX/vfx_authored_mask_array.dds" && !mask &&
                 binding.selection == Hash32("stable_seed_mod_group_size") &&
                 binding.min_quality == 0 &&
                 std::isfinite(binding.strength) && binding.strength >= 0.0f &&
                 binding.strength <= 1.0f &&
                 ValidRange(binding.slices, cooked.slice_indices) && binding.slices.count == 3 &&
                 cooked.slice_indices[binding.slices.first] == 3 &&
                 cooked.slice_indices[binding.slices.first + 1] == 4 &&
                 cooked.slice_indices[binding.slices.first + 2] == 5)
        {
            mask = true;
            flash.mask_slice = cooked.slice_indices[binding.slices.first + event.stable_seed % 3];
            flash.mask_strength = binding.strength;
        }
        else return false;
    }
    return gradient && curve && mask == bleed;
}
}

std::vector<VfxFlashSpawnInput> BuildVfxTypedFlashCommands(const VfxProgramData &cooked,
                                                         std::span<const VfxEventInput> inputs)
{
    std::vector<VfxFlashSpawnInput> result;
    for (const auto &input : inputs)
    {
        if (input.effect_handle == 0 || input.effect_handle > cooked.effects.size() ||
            static_cast<std::uint32_t>(input.quality) > 2) continue;
        const auto &effect = cooked.effects[input.effect_handle - 1];
        if (effect.handle != input.effect_handle || effect.input_mode != 0 || effect.timing_kind != 0 ||
            !std::isfinite(effect.seconds) || effect.seconds <= 0 || !ValidRange(effect.sources, cooked.sources)) continue;
        Float3 position{}, direction{};
        float scale = 1;
        bool circle_geometry=false, cone_geometry=false, projectile_geometry=false, boss_energy_geometry=false;
        std::uint64_t projectile_effect_id{};
        if (const auto *point = std::get_if<VfxPointPayload>(&input.payload))
        {
            if (effect.payload_kind != Hash32("PointEventPayload")) continue;
            position = point->position; direction = point->direction; scale = point->authored_scale;
        }
        else if (const auto *context = std::get_if<VfxContextPayload>(&input.payload))
        {
            if (effect.payload_kind != Hash32("PresentationContextPayload")) continue;
            position = {input.world_transform[12], input.world_transform[13], input.world_transform[14]};
            direction = context->direction;
        }
        else if(const auto *projectile=std::get_if<VfxProjectilePayload>(&input.payload))
        {
            if(effect.payload_kind!=Hash32("ProjectilePayload")||!Finite(projectile->velocity)||
                !std::isfinite(projectile->hitbox_radius)||projectile->hitbox_radius<=0||
                std::hypot(projectile->velocity.x,projectile->velocity.y,projectile->velocity.z)<=.0001f)continue;
            const auto identity=std::find_if(cooked.effect_lookup.begin(),cooked.effect_lookup.end(),
                [&](const auto &v){return v.handle==effect.handle;});
            if(identity==cooked.effect_lookup.end())continue;
            projectile_effect_id=identity->effect_id;
            position={input.world_transform[12],input.world_transform[13],input.world_transform[14]};
            direction=projectile->velocity;scale=projectile->hitbox_radius;projectile_geometry=true;
        }
        else if (const auto *cone = std::get_if<VfxConePayload>(&input.payload))
        {
            if (effect.payload_kind != kConePayload || !Finite(cone->origin) ||
                !Finite(cone->direction) || !std::isfinite(cone->range) || cone->range <= 0.0f ||
                !std::isfinite(cone->half_angle_degrees) || cone->half_angle_degrees <= 0.0f ||
                cone->half_angle_degrees >= 90.0f)
                continue;
            const auto identity = std::find_if(cooked.effect_lookup.begin(), cooked.effect_lookup.end(),
                [&](const auto &value) { return value.handle == effect.handle; });
            if (identity == cooked.effect_lookup.end() ||
                (identity->effect_id != kSecondaryFan && identity->effect_id != kMultiShot &&
                 identity->effect_id != kRearFan && identity->effect_id != kRetreatTripleRelease &&
                 identity->effect_id != kArrowTripleRelease &&
                 identity->effect_id != kBossVolleyRelease))
                continue;
            position = cone->origin;
            direction = cone->direction;
            scale = 1.0f;
            cone_geometry = true;
            boss_energy_geometry = identity->effect_id == kBossVolleyRelease;
        }
        else if (const auto *ring = std::get_if<VfxRingGapsPayload>(&input.payload))
        {
            if (effect.payload_kind != kRingGapsPayload || !Finite(ring->center) ||
                !std::isfinite(ring->inner_radius) || ring->inner_radius <= 0.0f ||
                !std::isfinite(ring->outer_radius) || ring->outer_radius <= ring->inner_radius ||
                !std::isfinite(ring->gap_half_width_degrees) || ring->gap_half_width_degrees < 0.0f ||
                ring->gap_half_width_degrees > 180.0f ||
                (!ring->gap_angles_degrees.empty() && ring->gap_half_width_degrees == 0.0f) ||
                std::any_of(ring->gap_angles_degrees.begin(), ring->gap_angles_degrees.end(),
                    [](float angle) { return !std::isfinite(angle); }))
                continue;
            const auto identity = std::find_if(cooked.effect_lookup.begin(), cooked.effect_lookup.end(),
                [&](const auto &value) { return value.handle == effect.handle; });
            if (identity == cooked.effect_lookup.end() || identity->effect_id != kBossShockwaveRelease)
                continue;
            position = ring->center;
            direction = {0.0f, 0.0f, 1.0f};
            scale = 1.0f;
            boss_energy_geometry = true;
        }
        else if(const auto *circle=std::get_if<VfxCirclePayload>(&input.payload))
        {
            if(effect.payload_kind!=Hash32("CircleAreaPayload")||circle->inner_radius!=0)continue;
            position=circle->center;direction=circle->direction;scale=circle->radius;circle_geometry=true;
        }
        else if (const auto *link = std::get_if<VfxLinkPayload>(&input.payload))
        {
            const auto identity = std::find_if(cooked.effect_lookup.begin(),
                cooked.effect_lookup.end(), [&](const auto &entry) {
                    return entry.handle == effect.handle;
                });
            if (effect.payload_kind != Hash32("SourceTargetPayload") ||
                identity == cooked.effect_lookup.end() ||
                std::find(kPickupCollectEffects.begin(), kPickupCollectEffects.end(),
                          identity->effect_id) == kPickupCollectEffects.end() ||
                !HasUniqueIdentity(cooked, effect.handle, identity->effect_id) ||
                !Finite(link->source_position) || !Finite(link->target_position) ||
                input.sequence == 0)
                continue;
            position = link->source_position;
            direction = {link->target_position.x - position.x,
                         link->target_position.y - position.y,
                         link->target_position.z - position.z};
        }
        else continue;
        if (!Finite(position) || !Finite(direction) || !std::isfinite(scale) || scale <= 0) continue;
        const float magnitude = std::hypot(direction.x, direction.y, direction.z);
        if (!std::isfinite(magnitude) || (cone_geometry && magnitude <= 0.0001f)) continue;
        direction = magnitude > 0.0001f ? Float3{direction.x / magnitude, direction.y / magnitude, direction.z / magnitude}
                                      : Float3{0, 0, 1};
        for (std::uint32_t source_index = effect.sources.first; source_index < effect.sources.first + effect.sources.count; ++source_index)
        {
            const auto &source = cooked.sources[source_index];
            // All supported impact recipes have two knots. Four-knot envelopes require an explicit alpha channel.
            if (source.effect != effect.handle || source.type != VfxSourceType::ImpactSprite || source.knot_count != 2 ||
                !std::isfinite(source.knots[0]) || !std::isfinite(source.knots[1]) || source.knots[0] < 0 ||
                source.knots[1] <= source.knots[0] || source.knots[1] > 1 || !ValidRange(source.outputs, cooked.outputs) ||
                !ValidRange(source.parameters,cooked.parameters) || source.parameters.count!=0) continue;
            for (const auto &output : std::span(cooked.outputs).subspan(source.outputs.first, source.outputs.count))
            {
                const auto recipe = std::find_if(std::begin(kRecipes), std::end(kRecipes),
                    [&](const auto &r) { return r.shape == output.shape && r.motion == output.motion; });
                if(recipe==std::end(kRecipes))continue;
                const bool contact=recipe->kind==VfxImpactShape::ThreeProngBite||recipe->kind==VfxImpactShape::BossCrestContact||recipe->kind==VfxImpactShape::NeedleStar;
                if(contact!=projectile_geometry)continue;
                if (boss_energy_geometry && (source.stable_id != kBossEnergy ||
                                             recipe->kind != VfxImpactShape::RadialEnergy ||
                                             recipe->motion != VfxMotionKind::DirectionalTurbulence))
                    continue;
                if (cone_geometry && !boss_energy_geometry && (source.stable_id != kMuzzlePrimary ||
                                                               recipe->kind != VfxImpactShape::RadialSlash ||
                                                               recipe->motion != VfxMotionKind::SnapsOutwardAim))
                    continue;
                if(contact)
                {
                    const auto expected=recipe->kind==VfxImpactShape::ThreeProngBite?Hash64("particle.enemy.ranged.impact"):
                        recipe->kind==VfxImpactShape::BossCrestContact?Hash64("particle.boss.volley.projectile_impact"):Hash64("particle.skill.charged_shot.impact");
                    if(projectile_effect_id!=expected||output.parameters.count!=1)continue;
                }
                const bool oit=recipe->kind==VfxImpactShape::Flame||recipe->kind==VfxImpactShape::Smoke6Way||recipe->kind==VfxImpactShape::RadialEnergy;
                const auto profile=recipe->kind==VfxImpactShape::Flame?VfxOutputProfile::SpriteFlameOit:
                    recipe->kind==VfxImpactShape::Smoke6Way?VfxOutputProfile::SpriteSmoke6WayOit:
                    recipe->kind==VfxImpactShape::RadialEnergy?VfxOutputProfile::SpriteNoiseOit:
                    recipe->kind==VfxImpactShape::SoftDisc?VfxOutputProfile::SpriteAdd:VfxOutputProfile::SpriteSdfAdd;
                if(output.source!=source_index||output.min_quality>static_cast<std::uint32_t>(input.quality)||
                    output.profile!=profile)continue;
                VfxFlashSpawnInput spawn;
                spawn.shape = recipe->kind;
                // Cosmetic half-size defaults are metres; the authored curve controls their time evolution.
                spawn.size = recipe->size;
                if (!(oit?OitParameters(cooked,output,input,spawn):Parameters(cooked, output, spawn)) ||
                    !Mask(cooked, output, input, spawn)) continue;
                // Circle effects use the gameplay radius as their cosmetic extent;
                // the authored size default remains specific to Point/Context inputs.
                spawn.size = circle_geometry ? scale : spawn.size * scale;
                spawn.ground_base_anchor = circle_geometry &&
                    (spawn.shape == VfxImpactShape::SoftDisc || spawn.shape == VfxImpactShape::NoiseDisc ||
                     spawn.shape == VfxImpactShape::Flame || spawn.shape == VfxImpactShape::Smoke6Way ||
                     spawn.shape == VfxImpactShape::RadialEnergy);
                spawn.position = position; spawn.direction = direction;
                spawn.delay = effect.seconds * source.knots[0];
                spawn.lifetime = effect.seconds * (source.knots[1] - source.knots[0]);
                spawn.color = {output.rgba[0] * output.hdr, output.rgba[1] * output.hdr, output.rgba[2] * output.hdr, output.rgba[3]};
                if (!std::isfinite(spawn.size) || spawn.size <= 0 || !std::isfinite(spawn.delay) ||
                    !std::isfinite(spawn.lifetime) || spawn.lifetime <= 0 || !std::isfinite(output.hdr) || output.hdr <= 0 ||
                    !Finite({spawn.color.x, spawn.color.y, spawn.color.z}) || !std::isfinite(spawn.color.w) || spawn.color.w < 0 || spawn.color.w > 1) continue;
                spawn.event_tick = input.event_tick; spawn.stable_seed = input.stable_seed;
                spawn.gradient_row = output.gradient_row; spawn.hdr = output.hdr;
                result.push_back(spawn);
            }
        }
    }
    return result;
}

std::vector<VfxFlashSpawnInput> BuildVfxPersistentStatusStampCommands(
    const VfxProgramData &cooked,
    std::span<const VfxPersistentInput> inputs)
{
    std::vector<VfxFlashSpawnInput> result;
    for (const auto &input : inputs)
    {
        const auto effect_id = StatusEffectId(input.source_visual_kind);
        const auto visual_kind = static_cast<PersistentVfxKind>(input.source_visual_kind);
        const bool retarget = visual_kind == PersistentVfxKind::MultishotRetarget;
        const bool bleed_extend = visual_kind == PersistentVfxKind::RicochetBleedExtend;
        if (effect_id == 0 || input.stable_id == 0 || input.effect_handle == 0 ||
            input.effect_handle > cooked.effects.size() ||
            static_cast<std::uint32_t>(input.quality) > 2 ||
            !std::isfinite(input.source_duration_seconds) || input.source_duration_seconds <= 0.0f ||
            !std::isfinite(input.elapsed_seconds) || input.elapsed_seconds < 0.0f ||
            !std::isfinite(input.normalized_age) || input.normalized_age < 0.0f ||
            input.normalized_age >= 1.0f ||
            !HasUniqueIdentity(cooked, input.effect_handle, effect_id)) continue;
        const auto *entity = std::get_if<VfxEntityPayload>(&input.payload);
        const auto *path = std::get_if<VfxProjectilePathPayload>(&input.payload);
        if (retarget)
        {
            if (!path || !Finite(path->current_position) || !Finite(path->previous_position) ||
                !Finite(path->velocity) || !std::isfinite(path->projectile_radius) ||
                path->projectile_radius <= 0.0f || !std::isfinite(path->lifetime01) ||
                path->lifetime01 != input.normalized_age)
                continue;
        }
        else if (!entity || entity->render_instance_id == 0 ||
                 !std::isfinite(entity->footprint_radius) || entity->footprint_radius <= 0.0f ||
                 !std::isfinite(entity->lifetime01) || entity->lifetime01 != input.normalized_age ||
                 !std::isfinite(entity->health_fraction) || entity->health_fraction < 0.0f ||
                 entity->health_fraction > 1.0f) continue;
        const auto &effect = cooked.effects[input.effect_handle - 1];
        const bool fixed = static_cast<PersistentVfxKind>(input.source_visual_kind) ==
            PersistentVfxKind::ChargedFullReady || retarget || bleed_extend;
        const auto expected_payload = retarget ? kProjectilePathPayload : kEntityPayload;
        if (effect.handle != input.effect_handle || effect.input_mode != 1 ||
            effect.payload_kind != expected_payload || effect.timing_kind != (fixed ? 0u : 1u) ||
            (fixed && (!std::isfinite(effect.seconds) || effect.seconds != input.source_duration_seconds)) ||
            !ValidRange(effect.sources, cooked.sources)) continue;
        const Float3 position = retarget
            ? path->current_position
            : Float3{input.current_transform[12], input.current_transform[13],
                     input.current_transform[14]};
        const float footprint = retarget ? path->projectile_radius : entity->footprint_radius;
        if (!Finite(position)) continue;
        for (std::size_t source_index = effect.sources.first;
             source_index < static_cast<std::size_t>(effect.sources.first) + effect.sources.count;
             ++source_index)
        {
            const auto &source = cooked.sources[source_index];
            if (source.effect != effect.handle || source.stable_id != kStatusStamp ||
                source.authoritative != 1 || source.type != VfxSourceType::ImpactSprite ||
                source.knot_count != 2 || source.knots[0] != 0.0f ||
                !std::isfinite(source.knots[1]) || source.knots[1] <= 0.0f ||
                source.knots[1] > 1.0f || input.normalized_age >= source.knots[1] ||
                !ValidRange(source.parameters, cooked.parameters) || source.parameters.count != 0 ||
                !ValidRange(source.outputs, cooked.outputs) || source.outputs.count != 1) continue;
            const auto &output = cooked.outputs[source.outputs.first];
            if (output.source != source_index || output.profile != VfxOutputProfile::SpriteSdfAdd ||
                output.shape != kStatusSymbol || output.shape_domain != Hash32("sdf") ||
                output.shape_scale_rule != Hash32("gameplay_geometry_if_bound_else_component_radius") ||
                output.shape_component_kind != Hash32("particle") ||
                output.coverage_type != Hash32("analytic") || output.coverage_ref != kStatusSymbol ||
                output.motion != VfxMotionKind::FastStampBreakout ||
                output.motion_rate_hz != 0.0f || output.motion_amplitude != 0.0f ||
                output.motion_inset_fraction != 0.0f || output.min_quality != 0 ||
                !StatusStampCurve(cooked, output) || !StatusStampTextures(cooked, output, bleed_extend) ||
                !std::isfinite(output.hdr) || output.hdr <= 0.0f ||
                !std::isfinite(output.rgba[3]) || output.rgba[3] < 0.0f ||
                output.rgba[3] > 1.0f) continue;
            VfxFlashSpawnInput flash;
            flash.position = position;
            flash.shape = VfxImpactShape::StatusStamp;
            flash.size = footprint;
            flash.curve_row = 4;
            flash.normalized_age = input.normalized_age / source.knots[1];
            flash.lifetime = input.source_duration_seconds * source.knots[1];
            flash.color = {output.rgba[0] * output.hdr, output.rgba[1] * output.hdr,
                           output.rgba[2] * output.hdr, output.rgba[3]};
            VfxEventInput mask_input;
            mask_input.quality = input.quality;
            mask_input.stable_seed = input.stable_seed;
            if (!std::isfinite(flash.lifetime) || flash.lifetime <= 0.0f ||
                !Finite({flash.color.x, flash.color.y, flash.color.z}) ||
                !Mask(cooked, output, mask_input, flash)) continue;
            flash.stable_seed = input.stable_seed;
            flash.gradient_row = output.gradient_row;
            flash.hdr = output.hdr;
            result.push_back(flash);
        }
    }
    return result;
}

std::vector<VfxOwnedStatusTickFlashCommand> BuildVfxOwnedStatusTickFlashCommands(
    const VfxProgramData &cooked,
    std::span<const VfxEventInput> events,
    std::span<const VfxPersistentInput> persistent)
{
    std::vector<VfxOwnedStatusTickFlashCommand> result;
    for (const auto &event : events)
    {
        if (event.effect_handle == 0 || event.effect_handle > cooked.effects.size() ||
            event.geometry_owner_id == 0 || event.geometry_owner_id >= (1ull << 60) ||
            event.status_episode_generation == 0 ||
            static_cast<std::uint32_t>(event.quality) > 2 ||
            !std::holds_alternative<VfxPointPayload>(event.payload)) continue;
        const auto &tick = cooked.effects[event.effect_handle - 1];
        const bool bleed = HasUniqueIdentity(cooked, event.effect_handle, kBleedTick);
        const bool burn = HasUniqueIdentity(cooked, event.effect_handle, kBurnTick);
        if (bleed == burn || tick.handle != event.effect_handle || tick.input_mode != 0 ||
            tick.timing_kind != 0 || tick.payload_kind != Hash32("PointEventPayload") ||
            !std::isfinite(tick.seconds) || tick.seconds <= 0.0f) continue;
        const auto status_id = bleed ? kBleedStatus : kBurnStatus;
        const auto render_id = (2ull << 60) | event.geometry_owner_id;
        bool emitted = false;
        for (const auto &owner : persistent)
        {
            if (owner.effect_handle == 0 || owner.effect_handle > cooked.effects.size() ||
                owner.stable_id == 0 || static_cast<std::uint32_t>(owner.quality) > 2 ||
                owner.status_episode_generation != event.status_episode_generation ||
                !std::isfinite(owner.elapsed_seconds) || owner.elapsed_seconds < 0.0f ||
                !std::isfinite(owner.normalized_age) || owner.normalized_age < 0.0f ||
                owner.normalized_age >= 1.0f) continue;
            const auto *entity = std::get_if<VfxEntityPayload>(&owner.payload);
            if (!entity || entity->render_instance_id != render_id ||
                !std::isfinite(entity->footprint_radius) || entity->footprint_radius <= 0.0f ||
                !std::isfinite(entity->lifetime01) || entity->lifetime01 != owner.normalized_age)
                continue;
            const auto &status = cooked.effects[owner.effect_handle - 1];
            if (!HasUniqueIdentity(cooked, owner.effect_handle, status_id) ||
                status.handle != owner.effect_handle || status.input_mode != 1 ||
                status.timing_kind != 1 || status.payload_kind != kEntityPayload ||
                !ValidRange(status.sources, cooked.sources)) continue;
            const Float3 position{owner.current_transform[12], owner.current_transform[13],
                                  owner.current_transform[14]};
            if (!Finite(position)) continue;
            for (std::size_t source_index = status.sources.first;
                 source_index < static_cast<std::size_t>(status.sources.first) + status.sources.count;
                 ++source_index)
            {
                const auto &source = cooked.sources[source_index];
                if (source.effect != status.handle || source.stable_id != kTickCore ||
                    source.authoritative != 1 || source.type != VfxSourceType::ImpactSprite ||
                    source.knot_count != 2 || source.knots[0] != 0.0f ||
                    source.knots[1] != 0.5f || !ValidRange(source.outputs, cooked.outputs) ||
                    source.outputs.count != 1 || !ValidRange(source.parameters, cooked.parameters) ||
                    source.parameters.count != 0) continue;
                const auto &output = cooked.outputs[source.outputs.first];
                if (output.source != source_index ||
                    output.profile != VfxOutputProfile::SpriteSdfAdd ||
                    output.shape != kSmallPulse ||
                    output.shape_domain != Hash32("procedural_sprite") ||
                    output.shape_scale_rule != Hash32("component_size_or_bound_radius") ||
                    output.shape_component_kind != Hash32("particle") ||
                    output.coverage_type != Hash32("analytic") ||
                    output.coverage_ref != kSmallPulse ||
                    output.motion != VfxMotionKind::SingleShortPulse ||
                    output.motion_rate_hz != 0.0f || output.motion_amplitude != 0.0f ||
                    output.motion_inset_fraction != 0.0f ||
                    output.min_quality > static_cast<std::uint32_t>(event.quality) ||
                    output.min_quality > static_cast<std::uint32_t>(owner.quality) ||
                    !StatusTickCurve(cooked, output)) continue;
                VfxFlashSpawnInput flash;
                flash.shape = VfxImpactShape::Pulse;
                flash.size = entity->footprint_radius;
                flash.position = position;
                flash.delay = tick.seconds * source.knots[0];
                flash.lifetime = tick.seconds * (source.knots[1] - source.knots[0]);
                flash.curve_row = 6;
                flash.color = {output.rgba[0] * output.hdr, output.rgba[1] * output.hdr,
                               output.rgba[2] * output.hdr, output.rgba[3]};
                if (!StatusTickTextures(cooked, output, bleed, event, flash) ||
                    !std::isfinite(output.hdr) || output.hdr <= 0.0f ||
                    !Finite({flash.color.x, flash.color.y, flash.color.z}) ||
                    !std::isfinite(flash.color.w) || flash.color.w < 0.0f ||
                    flash.color.w > 1.0f || !std::isfinite(flash.lifetime) ||
                    flash.lifetime <= 0.0f) continue;
                flash.event_tick = event.event_tick;
                flash.stable_seed = event.stable_seed;
                flash.gradient_row = output.gradient_row;
                flash.hdr = output.hdr;
                result.push_back({event.sequence, flash});
                emitted = true;
                break;
            }
            // A status event belongs to at most one live entity instance.
            if (emitted) break;
        }
    }
    return result;
}
} // namespace hs::runtime_detail
