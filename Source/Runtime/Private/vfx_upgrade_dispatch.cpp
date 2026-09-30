#include "vfx_upgrade_dispatch.hpp"
#include <hs/core/cooked_format.hpp>
#include <hs/game_domain/game_types.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>

namespace hs::runtime_detail
{
namespace
{
constexpr std::uint32_t Hash32(std::string_view text) noexcept
{
    std::uint32_t hash=2166136261u;
    for (unsigned char c:text) { hash^=c; hash*=16777619u; }
    return hash;
}
Result Invalid(std::string_view message)
{
    return Result::Failure(ErrorCode::InvalidArgument,"hs_vfx_upgrade_dispatch",std::string(message));
}
}
Result DispatchVfxUpgradeEvents(const VfxProgramData &program,
                                std::span<const PresentationEvent> events,
                                std::vector<PresentationEvent> &output)
{
    std::vector<PresentationEvent> dispatched;
    dispatched.reserve(events.size());
    struct Activation
    {
        std::uint64_t skill;
        std::uint8_t index;
        std::uint8_t stage;
        std::string_view effect;
        std::uint32_t payload;
        std::uint32_t timing;
        PresentationGeometryKind geometry;
        bool scheduled;
    };
    const auto explosive = MakeAssetId("skill.explosive_arrow").value;
    const auto charged = MakeAssetId("skill.charged_shot").value;
    const auto basic = MakeAssetId("skill.basic_attack").value;
    const auto multi = MakeAssetId("skill.multishot").value;
    const auto piercing = MakeAssetId("skill.piercing_shot").value;
    const auto ricochet = MakeAssetId("skill.ricochet_arrow").value;
    const auto arrow_rain = MakeAssetId("skill.arrow_rain").value;
    const auto trap = MakeAssetId("skill.trap").value;
    const auto retreat = MakeAssetId("skill.retreat_shot").value;
    const std::array activations{
        Activation{explosive, 0, 3, "particle.upgrade.explosive.reexplosion",
                    Hash32("CircleAreaPayload"), 0, PresentationGeometryKind::Circle, false},
        Activation{explosive, 1, 1, "particle.upgrade.explosive.mini_bomb.spawn",
                    Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{explosive, 1, 2, "particle.upgrade.explosive.mini_bomb.telegraph",
                    Hash32("CircleAreaPayload"), 1, PresentationGeometryKind::Circle, true},
        Activation{explosive, 1, 3, "particle.upgrade.explosive.mini_bomb.explosion",
                    Hash32("CircleAreaPayload"), 0, PresentationGeometryKind::Circle, false},
        Activation{explosive, 2, 1, "particle.upgrade.explosive.shard_radial",
                    Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{explosive, 3, 3, "particle.skill.fire_area.pulse",
                    Hash32("CircleAreaPayload"), 0, PresentationGeometryKind::Circle, false},
        Activation{explosive, 4, 3, "particle.upgrade.explosive.blood_burst",
                    Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{charged, 0, 3, "particle.upgrade.charged.end_explosion",
                     Hash32("CircleAreaPayload"), 0, PresentationGeometryKind::Circle, false},
        Activation{charged, 1, 1, "particle.skill.charged_shot.pulse",
                   Hash32("PresentationContextPayload"), 0, PresentationGeometryKind::None, false},
        Activation{charged, 6, 3, "particle.upgrade.charged.burn_explosion",
                     Hash32("CircleAreaPayload"), 0, PresentationGeometryKind::Circle, false},
        Activation{charged, 7, 3, "particle.upgrade.cooldown_proc",
                   Hash32("PresentationContextPayload"), 0, PresentationGeometryKind::None, false},
        Activation{basic, 2, 1, "particle.upgrade.arrow.split",
                    Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{basic, 7, 1, "particle.upgrade.arrow.triple_release",
                    Hash32("ConePayload"), 0, PresentationGeometryKind::Cone, false},
        Activation{piercing, 0, 1, "particle.upgrade.tracking_spawn",
                    Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{ricochet, 1, 1, "particle.upgrade.ricochet.branch",
                    Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{ricochet, 4, 1, "particle.upgrade.ricochet.kill_small_arrows",
                    Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{ricochet, 5, 1, "particle.upgrade.ricochet.kill_restart",
                    Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{ricochet, 6, 3, "particle.upgrade.cooldown_proc",
                    Hash32("PresentationContextPayload"), 0, PresentationGeometryKind::None, false},
        Activation{arrow_rain, 4, 1, "particle.upgrade.arrow_rain.tracking_arrow",
                    Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{arrow_rain, 6, 1, "particle.upgrade.arrow_rain.tracking_arrow",
                    Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{arrow_rain, 1, 3, "particle.upgrade.arrow_rain.pull",
                    Hash32("SourceTargetPayload"), 0, PresentationGeometryKind::Line, false},
        Activation{trap, 7, 1, "particle.upgrade.trap.mini_spawn",
                    Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{trap, 0, 1, "particle.skill.trap",
                    Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{trap, 2, 3, "particle.upgrade.trap.rearm",
                   Hash32("CircleAreaPayload"), 0, PresentationGeometryKind::Circle, false},
        Activation{trap, 3, 3, "particle.upgrade.trap.bleed_variant",
                   Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{trap, 5, 3, "particle.upgrade.trap.pull_in",
                   Hash32("CircleAreaPayload"), 0, PresentationGeometryKind::Circle, false},
        Activation{trap, 6, 3, "particle.upgrade.trap.mark_burst",
                   Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{retreat, 3, 1, "particle.upgrade.retreat.tracking_arrow",
                    Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{retreat, 4, 3, "particle.upgrade.retreat.land_shockwave",
                   Hash32("CircleAreaPayload"), 0, PresentationGeometryKind::Circle, false},
        Activation{retreat, 6, 3, "particle.upgrade.heal_proc",
                     Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{retreat, 5, 3, "particle.upgrade.cooldown_proc",
                   Hash32("PresentationContextPayload"), 0, PresentationGeometryKind::None, false},
        Activation{basic, 0, 1, "particle.upgrade.arrow.echo",
                    Hash32("PresentationContextPayload"), 0, PresentationGeometryKind::None, false},
        Activation{basic, 1, 3, "particle.upgrade.arrow.pierce",
                    Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{piercing, 1, 1, "particle.upgrade.piercing.exit_split",
                    Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{piercing, 4, 1, "particle.upgrade.piercing.chain_release",
                    Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{multi, 1, 1, "particle.upgrade.arrow.split",
                    Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{multi, 2, 3, "particle.upgrade.arrow.pierce",
                    Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{charged, 5, 1, "particle.upgrade.charged.radial_split",
                    Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{explosive, 5, 3, "particle.upgrade.explosive.pre_pull",
                    Hash32("CircleAreaPayload"), 0, PresentationGeometryKind::Circle, false},
        Activation{charged, 3, 3, "particle.upgrade.arrow.pierce",
                    Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{explosive, 6, 3, "particle.upgrade.explosive.mark_detonation",
                   Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{piercing, 5, 3, "particle.upgrade.piercing.push_alignment",
                   Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{multi, 5, 1, "particle.upgrade.tracking_spawn",
                   Hash32("PointEventPayload"), 0, PresentationGeometryKind::None, false},
        Activation{multi, 0, 1, "particle.upgrade.multishot.secondary_fan",
                   Hash32("ConePayload"), 0, PresentationGeometryKind::Cone, false},
        Activation{multi, 3, 1, "particle.upgrade.multishot.rear_fan",
                   Hash32("ConePayload"), 0, PresentationGeometryKind::Cone, false},
        Activation{multi, 6, 1, "particle.skill.multishot",
                   Hash32("ConePayload"), 0, PresentationGeometryKind::Cone, false},
        Activation{retreat, 0, 1, "particle.upgrade.retreat.triple_release",
                   Hash32("ConePayload"), 0, PresentationGeometryKind::Cone, false},
    };
    for (const auto &event:events)
    {
        if (event.kind!=PresentationKind::Vfx || event.upgrade_stage==0)
        {
            dispatched.push_back(event);
            continue;
        }
        const auto skill_id = event.upgrade_skill == static_cast<std::uint8_t>(SkillKind::BasicAttack)
                                  ? basic
                                  : event.upgrade_skill == static_cast<std::uint8_t>(SkillKind::PiercingShot)
                                        ? piercing
                                        : event.upgrade_skill == static_cast<std::uint8_t>(SkillKind::ExplosiveArrow)
                                              ? explosive
                                              : event.upgrade_skill == static_cast<std::uint8_t>(SkillKind::RicochetArrow)
                                                    ? ricochet
                                                    : event.upgrade_skill == static_cast<std::uint8_t>(SkillKind::ChargedShot)
                                                          ? charged
                                                          : event.upgrade_skill == static_cast<std::uint8_t>(SkillKind::ArrowRain)
                                                                ? arrow_rain
                                                                : event.upgrade_skill == static_cast<std::uint8_t>(SkillKind::Trap)
                                                                      ? trap
                                                                      : event.upgrade_skill == static_cast<std::uint8_t>(SkillKind::RetreatShot)
                                                                            ? retreat
                                                                            : event.upgrade_skill == static_cast<std::uint8_t>(SkillKind::MultiShot)
                                                                                  ? multi
                                                                             : 0;
        const auto activation = std::find_if(activations.begin(), activations.end(), [&](const auto &value) {
            return value.skill == skill_id && value.index == event.upgrade_index &&
                   value.stage == event.upgrade_stage;
        });
        if (activation == activations.end())
        {
            const auto supported_pair = std::find_if(activations.begin(), activations.end(), [&](const auto &value) {
                return value.skill == skill_id && value.index == event.upgrade_index;
            });
            if (supported_pair == activations.end())
            {
                // The cooked catalog contains many upgrade bindings whose
                // visual ingress is intentionally not implemented yet. Keep
                // those events available to their existing presentation path.
                dispatched.push_back(event);
                continue;
            }
            return Invalid("Unsupported stage for upgrade activation.");
        }
        const auto effect_id=MakeAssetId(activation->effect).value;
        const auto binding=std::find_if(program.upgrade_bindings.begin(),program.upgrade_bindings.end(),[&](const auto &value){
            return value.skill_id==activation->skill && value.ordinal==event.upgrade_index+1u;
        });
        if (binding==program.upgrade_bindings.end() || binding->sequence.first>program.upgrade_sequences.size() ||
            binding->sequence.count>program.upgrade_sequences.size()-binding->sequence.first)
            return Invalid("Upgrade binding is missing or invalid.");
        const auto lookup=std::find_if(program.effect_lookup.begin(),program.effect_lookup.end(),[&](const auto &value){return value.effect_id==effect_id;});
        if (lookup==program.effect_lookup.end() || lookup->handle==0 || lookup->handle>program.effects.size())
            return Invalid("Upgrade stage effect is missing.");
        const auto sequence=std::span(program.upgrade_sequences).subspan(binding->sequence.first,binding->sequence.count);
        if (std::find(sequence.begin(),sequence.end(),lookup->handle)==sequence.end())
            return Invalid("Upgrade stage effect is not a member of its cooked upgrade binding.");
        const auto &effect=program.effects[lookup->handle-1];
        if (effect.handle!=lookup->handle || effect.input_mode!=0 ||
            effect.payload_kind!=activation->payload || effect.timing_kind!=activation->timing ||
            (!activation->scheduled && (!std::isfinite(effect.seconds) || effect.seconds<=0)))
            return Invalid(std::string("Upgrade stage effect has incompatible payload or timing: ") +
                           std::string(activation->effect));
        if (event.upgrade_owner_id==0 || event.upgrade_cast_id==0 ||
            !std::isfinite(event.position.x) || !std::isfinite(event.position.y) || !std::isfinite(event.position.z))
            return Invalid("Explosive stage activation is missing its actual owner or position.");
        if (activation->geometry == PresentationGeometryKind::Circle &&
            (event.geometry.kind!=PresentationGeometryKind::Circle ||
            !std::isfinite(event.geometry.radius) || event.geometry.radius<=0 ||
            event.geometry.source_id!=event.upgrade_owner_id))
            return Invalid("Upgrade stage activation is missing its actual circle geometry.");
        if (activation->geometry == PresentationGeometryKind::Cone &&
            (event.geometry.kind!=PresentationGeometryKind::Cone ||
             !std::isfinite(event.geometry.range) || event.geometry.range<=0 ||
             !std::isfinite(event.geometry.half_angle_degrees) ||
             event.geometry.half_angle_degrees<=0 ||
             event.geometry.half_angle_degrees>180 ||
             event.geometry.source_id!=event.upgrade_owner_id ||
             event.geometry.start_tick!=event.tick ||
             event.geometry.end_tick!=event.tick))
            return Invalid("Upgrade stage activation is missing its actual cone geometry.");
        if (activation->geometry == PresentationGeometryKind::Line &&
            (event.geometry.kind != PresentationGeometryKind::Line ||
             !std::isfinite(event.geometry.width) || event.geometry.width <= 0.0f ||
             !std::isfinite(event.geometry.end_position.x) ||
             !std::isfinite(event.geometry.end_position.y) ||
             !std::isfinite(event.geometry.end_position.z) ||
             event.geometry.source_id != event.upgrade_owner_id ||
             (event.geometry.end_position.x == event.position.x &&
              event.geometry.end_position.y == event.position.y &&
              event.geometry.end_position.z == event.position.z)))
            return Invalid("Upgrade stage activation is missing its actual link geometry.");
        if (activation->scheduled && (event.geometry.start_tick!=event.tick || event.geometry.end_tick<=event.geometry.start_tick))
            return Invalid("Mini-bomb warning requires its actual schedule interval.");
        auto translated=event;
        translated.asset={effect_id};
        dispatched.push_back(translated);
    }
    output=std::move(dispatched);
    return Result::Success();
}
}
