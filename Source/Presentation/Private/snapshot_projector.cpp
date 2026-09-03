#include <hs/presentation/projector.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>
#include <ranges>
#include <string>
#include <string_view>
namespace hs { namespace {
constexpr Tick Seconds(float v) noexcept { return static_cast<Tick>(v*60.0f+0.5f); }
constexpr Tick kRecoilClipTicks=41, kAnimationBlendOutTicks=6;
constexpr float kPi=std::numbers::pi_v<float>;
constexpr std::uint64_t kPlayerRenderId=1ull<<60,kEnemyRenderId=2ull<<60,kProjectileRenderId=3ull<<60,kAreaRenderId=4ull<<60,kPickupRenderId=5ull<<60;
bool HasUpgrade(std::uint8_t m,std::uint8_t o) noexcept{return o&&(m&(1u<<(o-1)));}
bool HasRelic(RelicMask m,RelicKind r) noexcept{return m&(RelicMask{1}<<static_cast<unsigned>(r));}
Float2 Add(Float2 a,Float2 b) noexcept{return {a.x+b.x,a.y+b.y};} Float2 Subtract(Float2 a,Float2 b) noexcept{return {a.x-b.x,a.y-b.y};} Float2 Multiply(Float2 v,float s) noexcept{return {v.x*s,v.y*s};}
float LengthSquared(Float2 v) noexcept{return v.x*v.x+v.y*v.y;} Float2 Normalize(Float2 v) noexcept{auto l=std::sqrt(LengthSquared(v));return l>.0001f?Multiply(v,1/l):Float2{0,1};} Float2 Rotate(Float2 v,float r) noexcept{auto c=std::cos(r),s=std::sin(r);return {v.x*c-v.y*s,v.x*s+v.y*c};}
} // namespace
bool ProjectRenderSnapshot(const GameReadModel &model,
                           const PresentationCatalog &presentation,
                           const PresentationUiState &ui,
                           const SettingsData &settings,
                           RenderSnapshotStorage &snapshot,
                           std::uint8_t pending_rebind_slot)
{
    snapshot.header.tick = model.tick;
    snapshot.header.simulation_time = std::chrono::nanoseconds(16'666'667) * model.tick;
    snapshot.header.checksum = model.checksum;
    snapshot.camera.target = {model.player.position.x, 0.0f, model.player.position.y};

    bool complete = true;
    complete &= snapshot.AddInstance(
        {{model.player.position.x, 0.0f, model.player.position.y},
         std::atan2(model.player.facing.x, model.player.facing.y),
         {1.0f, 1.0f, 1.0f}, 0xFFFFFFFFu, RenderMesh::Archer,
         kPlayerRenderId});
    AnimationPoseRef pose;
    pose.instance_index = 0;
    pose.clip = CharacterAnimationClip::Idle;
    pose.normalized_time = static_cast<float>(model.tick % 120) / 120.0f;
    pose.secondary_clip = CharacterAnimationClip::Run;
    pose.secondary_normalized_time = static_cast<float>(model.tick % 45) / 45.0f;
    pose.secondary_weight = model.player.locomotion_blend;
    if (model.session.phase == SessionPhase::Defeat)
    {
        pose.clip = CharacterAnimationClip::Death;
        pose.normalized_time = 1.0f;
        pose.secondary_clip = CharacterAnimationClip::Death;
        pose.secondary_normalized_time = 1.0f;
        pose.secondary_weight = 0.0f;
    }
    else if (model.player.charging)
    {
        pose.upper_body_clip = CharacterAnimationClip::Draw;
        pose.upper_body_normalized_time = std::min(
            1.0f, static_cast<float>(model.tick - model.player.charge_start) /
                      static_cast<float>(HasUpgrade(
                          model.player.upgrades[static_cast<std::size_t>(
                              SkillKind::ChargedShot)], 1)
                                             ? Seconds(1.4f)
                                             : Seconds(1.0f)));
        pose.upper_body_weight = std::clamp(
            static_cast<float>(model.tick - model.player.charge_start + 1) /
                static_cast<float>(Seconds(0.10f)),
            0.0f, 1.0f);
    }
    else if (model.tick < model.player.basic_attack_animation_until)
    {
        const auto clip_until = model.player.basic_attack_animation_until -
                                kAnimationBlendOutTicks;
        const auto playback_ticks = std::max<Tick>(
            1, clip_until - model.player.basic_attack_animation_start);
        pose.upper_body_clip = CharacterAnimationClip::Recoil;
        pose.upper_body_normalized_time =
            static_cast<float>(model.tick - model.player.basic_attack_animation_start) /
            static_cast<float>(kRecoilClipTicks);
        pose.upper_body_playback_rate = static_cast<float>(kRecoilClipTicks) /
                                        static_cast<float>(playback_ticks);
        const auto fade_in = static_cast<float>(
            model.tick - model.player.basic_attack_animation_start + 1) /
            static_cast<float>(Seconds(0.10f));
        const auto fade_out = model.tick <= clip_until
                                  ? 1.0f
                                  : static_cast<float>(
                                        model.player.basic_attack_animation_until -
                                        model.tick) /
                                        static_cast<float>(kAnimationBlendOutTicks);
        pose.upper_body_weight = std::clamp(std::min(fade_in, fade_out), 0.0f, 1.0f);
    }
    else if (model.tick < model.player.active_animation_until ||
             model.tick < model.player.retreat_until)
    {
        const auto clip_until = model.player.active_cast_tick;
        const auto playback_ticks = std::max<Tick>(
            1, clip_until - model.player.active_animation_start);
        pose.upper_body_clip = CharacterAnimationClip::Recoil;
        pose.upper_body_normalized_time =
            static_cast<float>(model.tick - model.player.active_animation_start) /
            static_cast<float>(kRecoilClipTicks);
        pose.upper_body_playback_rate = static_cast<float>(kRecoilClipTicks) /
                                        static_cast<float>(playback_ticks);
        const auto fade_in = static_cast<float>(
            model.tick - model.player.active_animation_start + 1) /
            static_cast<float>(Seconds(0.10f));
        const auto fade_out = model.tick <= clip_until
                                  ? 1.0f
                                  : static_cast<float>(
                                        model.player.active_animation_until - model.tick) /
                                        static_cast<float>(kAnimationBlendOutTicks);
        pose.upper_body_weight = std::clamp(std::min(fade_in, fade_out), 0.0f, 1.0f);
    }
    complete &= snapshot.AddPose(pose);

    if (model.player.charging &&
        model.player.charging_skill == SkillKind::ChargedShot)
    {
        const auto range = model.charge_range;
        const auto guide_length = range;
        const auto center = Add(model.player.position,
                                Multiply(model.player.aim, guide_length * 0.5f));
        complete &= snapshot.AddPersistentVfx(
            {{center.x, 0.025f, center.y},
             std::atan2(model.player.aim.x, model.player.aim.y),
             model.charge_radius, guide_length,
             PersistentVfxKind::ChargeGuide,
             kPlayerRenderId | static_cast<std::uint64_t>(SkillKind::ChargedShot)});
    }

    const auto arena_size = model.arena_half_extent * 2.0f;
    complete &= snapshot.AddInstance(
        {{0.0f, -0.05f, 0.0f}, 0.0f, {arena_size, 0.1f, arena_size},
         0xFF181818u, RenderMesh::Ground});

    constexpr float kBoundaryThickness = 0.6f;
    constexpr float kBoundaryHeightScale = 12.0f;
    constexpr std::uint32_t kBoundaryColor = 0xFF20A0FFu;
    const auto boundary_center = model.arena_half_extent + kBoundaryThickness * 0.5f;
    const auto boundary_length = model.arena_half_extent * 2.0f + kBoundaryThickness * 2.0f;
    for (const auto &instance : std::array{
             RenderInstance{{-boundary_center, 0.6f, 0.0f}, 0.0f,
                            {kBoundaryThickness, kBoundaryHeightScale, boundary_length},
                            kBoundaryColor, RenderMesh::Area},
             RenderInstance{{boundary_center, 0.6f, 0.0f}, 0.0f,
                            {kBoundaryThickness, kBoundaryHeightScale, boundary_length},
                            kBoundaryColor, RenderMesh::Area},
             RenderInstance{{0.0f, 0.6f, -boundary_center}, 0.0f,
                            {boundary_length, kBoundaryHeightScale, kBoundaryThickness},
                            kBoundaryColor, RenderMesh::Area},
             RenderInstance{{0.0f, 0.6f, boundary_center}, 0.0f,
                            {boundary_length, kBoundaryHeightScale, kBoundaryThickness},
                            kBoundaryColor, RenderMesh::Area},
         })
    {
        complete &= snapshot.AddInstance(instance);
    }
    for (const auto &enemy : model.enemies)
    {
        if (enemy.dead) continue;
        const auto color = enemy.boss == BossKind::Final ? 0xFF4040E8u :
                           enemy.boss == BossKind::TenMinute ? 0xFFB040E8u :
                           enemy.boss == BossKind::FiveMinute ? 0xFFE86040u :
                           enemy.kind == EnemyKind::Ranged ? 0xFF40B060u :
                           enemy.kind == EnemyKind::Suicide ? 0xFF40D8E8u : 0xFF5D66E8u;
        const auto mesh = enemy.boss == BossKind::Final ? RenderMesh::BossFinal :
                          enemy.boss == BossKind::TenMinute ? RenderMesh::BossTenMinute :
                          enemy.boss == BossKind::FiveMinute ? RenderMesh::BossFiveMinute :
                          enemy.kind == EnemyKind::Ranged ? RenderMesh::MonsterRanged :
                          enemy.kind == EnemyKind::Suicide ? RenderMesh::MonsterSuicide
                                                           : RenderMesh::MonsterMelee;
        const auto uniform_scale = enemy.boss == BossKind::Final ? 3.51f :
                                   enemy.boss == BossKind::TenMinute ? 4.71f :
                                   enemy.boss == BossKind::FiveMinute ? 4.23f :
                                    enemy.kind == EnemyKind::Ranged ? 2.00f :
                                    enemy.kind == EnemyKind::Suicide ? 2.40f : 1.50f;
        const Float3 scale{uniform_scale, uniform_scale, uniform_scale};
        std::uint32_t status_visual_mask{};
        if (enemy.status_flags & static_cast<std::uint8_t>(StatusFlag::Bleed))
            status_visual_mask |= static_cast<std::uint32_t>(StatusVisual::Bleed);
        if (enemy.status_flags & static_cast<std::uint8_t>(StatusFlag::Burn))
            status_visual_mask |= static_cast<std::uint32_t>(StatusVisual::Burn);
        if (enemy.status_flags & static_cast<std::uint8_t>(StatusFlag::Slow))
            status_visual_mask |= static_cast<std::uint32_t>(StatusVisual::Slow);
        if (enemy.status_flags & static_cast<std::uint8_t>(StatusFlag::Mark))
            status_visual_mask |= static_cast<std::uint32_t>(StatusVisual::Mark);
        const auto instance_index = snapshot.InstanceCount();
        const auto facing = enemy.attacking && LengthSquared(enemy.locked_aim) > 0.0001f
                                ? enemy.locked_aim : enemy.velocity;
        complete &= snapshot.AddInstance(
            {{enemy.position.x, 0.0f, enemy.position.y},
             std::atan2(facing.x, facing.y), scale, color, mesh,
             kEnemyRenderId | enemy.id.value, status_visual_mask});
        AnimationPoseRef enemy_pose;
        enemy_pose.instance_index = static_cast<std::uint32_t>(instance_index);
        const auto boss_action = std::ranges::find_if(
            model.boss_actions, [&](const BossActionView &action) {
                return action.boss_id == enemy.id.value;
            });
        const auto boss_release_frame = enemy.boss_action_until == model.tick &&
                                        enemy.boss_action_until > enemy.boss_action_started;
        if (enemy.attacking || boss_action != model.boss_actions.end() || boss_release_frame)
        {
            const auto active_boss_action = boss_action != model.boss_actions.end();
            const auto recoil = boss_release_frame
                                    ? enemy.boss_action_recoil
                                : active_boss_action
                                    ? boss_action->kind == BossActionViewKind::Area ||
                                          boss_action->kind == BossActionViewKind::Shockwave
                                    : enemy.boss_action_recoil;
            enemy_pose.clip = recoil ? CharacterAnimationClip::Recoil
                                     : CharacterAnimationClip::Draw;
            const auto start = boss_release_frame ? enemy.boss_action_started
                               : active_boss_action ? boss_action->animation_started
                                                    : enemy.attack_started;
            const auto until = boss_release_frame ? enemy.boss_action_until
                               : active_boss_action ? boss_action->execute_tick
                                                    : enemy.attack_resolve;
            enemy_pose.normalized_time = until > start
                                             ? std::clamp(
                                                   static_cast<float>(model.tick - start) /
                                                       static_cast<float>(until - start),
                                                   0.0f, 1.0f)
                                             : 0.0f;
        }
        else if (LengthSquared(enemy.velocity) > 0.0001f)
        {
            enemy_pose.clip = CharacterAnimationClip::Run;
            enemy_pose.normalized_time =
                static_cast<float>((model.tick + enemy.id.value * 17) % 45) / 45.0f;
        }
        else
        {
            enemy_pose.clip = CharacterAnimationClip::Idle;
            enemy_pose.normalized_time =
                static_cast<float>((model.tick + enemy.id.value * 17) % 120) / 120.0f;
        }
        complete &= snapshot.AddPose(enemy_pose);
        if (!enemy.boss && enemy.kind == EnemyKind::Ranged && enemy.attacking)
        {
            const auto range = enemy.warning_extent;
            const auto center = Add(enemy.position,
                                    Multiply(enemy.locked_aim, range * 0.5f));
            complete &= snapshot.AddInstance(
                {{center.x, 0.025f, center.y},
                 std::atan2(enemy.locked_aim.x, enemy.locked_aim.y),
                 {0.16f, 0.03f, range}, 0xA03030FFu, RenderMesh::Area});
        }
        if (!enemy.boss && enemy.kind == EnemyKind::Suicide && enemy.attacking)
        {
            const auto radius = enemy.warning_extent;
            complete &= snapshot.AddInstance(
                {{enemy.position.x, 0.025f, enemy.position.y}, 0.0f,
                 {radius, 0.03f, radius}, 0x803030FFu, RenderMesh::Area});
        }
    }
    for (const auto &projectile : model.projectiles)
    {
        if (projectile.dead) continue;
        auto scale = projectile.player_owned ? Float3{0.24f, 0.24f, 0.825f}
                                             : Float3{0.24f, 0.24f, 0.8f};
        auto color = projectile.player_owned ? 0xFF40E8FFu : 0xFF4040FFu;
        if (projectile.player_owned)
        {
            switch (projectile.skill)
            {
            case SkillKind::PiercingShot: scale = {0.28f, 0.28f, 1.25f}; color = 0xFFFFE8A0u; break;
            case SkillKind::MultiShot: scale = {0.18f, 0.18f, 0.7f}; color = 0xFFFFD878u; break;
            case SkillKind::ChargedShot:
                scale = {std::lerp(0.228f, 0.42f, projectile.charge_ratio),
                         std::lerp(0.228f, 0.42f, projectile.charge_ratio),
                         std::lerp(0.81f, 1.26f, projectile.charge_ratio)};
                color = 0xFFFFF0C0u;
                break;
            case SkillKind::ExplosiveArrow: scale = {0.34f, 0.34f, 1.0f}; color = 0xFF188CFFu; break;
            case SkillKind::RicochetArrow: scale = {0.25f, 0.25f, 0.85f}; color = 0xFFFFA840u; break;
            default: break;
            }
        }
        const auto render_position = projectile.position;
        complete &= snapshot.AddInstance(
            {{render_position.x,
              projectile.player_owned && projectile.skill == SkillKind::BasicAttack
                  ? 1.05f
                  : 0.25f,
              render_position.y},
             std::atan2(projectile.velocity.x, projectile.velocity.y),
             scale, color,
             projectile.player_owned ? RenderMesh::PlayerProjectile
                                      : RenderMesh::EnemyProjectile,
             kProjectileRenderId | projectile.id.value});
        if (projectile.player_owned)
        {
            const auto speed = std::sqrt(LengthSquared(projectile.velocity));
            const auto short_trail = projectile.skill == SkillKind::BasicAttack ||
                                     projectile.skill == SkillKind::MultiShot;
            const auto trail_length = std::clamp(speed * (short_trail ? 0.025f : 0.05f),
                                                 0.2f, short_trail ? 0.45f : 1.0f);
            const auto trail_direction = Normalize(projectile.velocity);
            const auto body_half_length = scale.z * 0.5f;
            const auto trail_position = Subtract(
                projectile.position,
                Multiply(trail_direction, body_half_length + trail_length * 0.5f));
            const auto body_center_height =
                (projectile.skill == SkillKind::BasicAttack ? 1.05f : 0.25f) +
                scale.y * 0.5f;
            const auto trail_radius = projectile.skill == SkillKind::ChargedShot
                                          ? 0.035f
                                          : short_trail
                                                ? 0.05f
                                                : std::min(0.06f,
                                                           projectile.radius * 0.35f);
            complete &= snapshot.AddPersistentVfx(
                {{trail_position.x, body_center_height, trail_position.y},
                 std::atan2(projectile.velocity.x, projectile.velocity.y),
                 trail_radius,
                 trail_length,
                 projectile.skill == SkillKind::RicochetArrow
                     ? PersistentVfxKind::RicochetProjectileTrail
                     : PersistentVfxKind::ProjectileTrail,
                 kProjectileRenderId | projectile.id.value});
            if (projectile.skill == SkillKind::ChargedShot)
            {
                complete &= snapshot.AddPersistentVfx(
                    {{trail_position.x, body_center_height, trail_position.y},
                     std::atan2(projectile.velocity.x, projectile.velocity.y),
                     0.075f, trail_length,
                     PersistentVfxKind::ProjectileTrailOuter,
                     (kProjectileRenderId | projectile.id.value) ^ (1ull << 59)});
            }
        }
    }
    for (const auto &area : model.areas)
    {
        if (area.dead) continue;
        // AreaActor lifetime is authoritative for every persistent visual.  The
        // read model can briefly retain an actor on its expiry tick, so do not
        // let a stale AreaView render past its explicit end tick.
        if (area.expires != 0 && model.tick >= area.expires) continue;
        if (area.kind == AreaViewKind::Trap)
        {
            const auto pending_kind = model.tick < area.active_tick
                                        ? PersistentVfxKind::TrapPending
                                        : PersistentVfxKind::TrapArmed;
            complete &= snapshot.AddPersistentVfx(
                {{area.position.x, 0.025f, area.position.y}, 0.0f, area.radius,
                 0.0f,
                  pending_kind,
                  kAreaRenderId | area.id.value});
        }
        const auto fire_area = area.applies_burn ||
            (area.skill == SkillKind::ExplosiveArrow && area.source_upgrade == 3) ||
            (area.skill == SkillKind::Trap && area.source_upgrade == 4) ||
            (area.skill == SkillKind::ArrowRain && area.source_upgrade == 3);
        if (fire_area && model.tick >= area.active_tick)
        {
            complete &= snapshot.AddPersistentVfx(
                {{area.position.x, 0.02f, area.position.y}, 0.0f, area.radius,
                 0.0f, PersistentVfxKind::FireArea,
                 kAreaRenderId | area.id.value});
        }
        if ((area.kind == AreaViewKind::Slow || area.applies_slow) &&
            area.half_length <= 0.0f &&
            model.tick >= area.active_tick)
        {
            complete &= snapshot.AddPersistentVfx(
                {{area.position.x, 0.018f, area.position.y}, 0.0f, area.radius,
                 0.0f, PersistentVfxKind::SlowArea,
                 kAreaRenderId | area.id.value});
        }
        if (area.skill == SkillKind::ArrowRain)
        {
            complete &= snapshot.AddPersistentVfx(
                {{area.position.x, 0.016f, area.position.y}, 0.0f, area.radius,
                 0.0f, PersistentVfxKind::ArrowRainArea,
                 kAreaRenderId | area.id.value});
        }
        if (area.kind == AreaViewKind::Slow && area.half_length > 0.0f &&
            model.tick >= area.active_tick)
        {
            complete &= snapshot.AddPersistentVfx(
                {{area.position.x, 0.014f, area.position.y},
                 std::atan2(area.direction.x, area.direction.y), area.radius,
                 area.half_length * 2.0f, PersistentVfxKind::DamageTrail,
                 kAreaRenderId | area.id.value});
        }
        // Trap and Arrow Rain keep one persistent visual across activation.
        // Other AreaActor visuals begin at active_tick.
        if (model.tick < area.active_tick) continue;
        if (area.half_length > 0.0f && area.kind == AreaViewKind::Damage &&
            model.tick >= area.active_tick)
        {
            complete &= snapshot.AddPersistentVfx(
                {{area.position.x, 0.014f, area.position.y},
                 std::atan2(area.direction.x, area.direction.y), area.radius,
                 area.half_length * 2.0f, PersistentVfxKind::DamageTrail,
                 kAreaRenderId | area.id.value});
        }
        const auto particle_visual = area.kind == AreaViewKind::Slow ||
            area.kind == AreaViewKind::Trap ||
            area.kind == AreaViewKind::Damage;
        if (particle_visual) continue;
        if (area.ring_outer_radius > 0.0f && area.safe_gap_count > 0)
        {
            const auto duration = std::max<Tick>(area.expires - area.active_tick, 1);
            const auto progress = std::clamp(
                static_cast<float>(model.tick - area.active_tick) /
                    static_cast<float>(duration),
                0.0f, 1.0f);
            const auto radius = std::lerp(area.ring_inner_radius,
                                          area.ring_outer_radius, progress);
            constexpr std::uint32_t kSegments = 64;
            for (std::uint32_t index = 0; index < kSegments; ++index)
            {
                const auto degrees = 360.0f * static_cast<float>(index) / kSegments;
                const auto spacing = 360.0f / area.safe_gap_count;
                const auto offset = static_cast<float>(area.cast_id % 360);
                const auto nearest_gap =
                    std::fmod(degrees - offset + spacing * 0.5f + 360.0f, spacing) -
                    spacing * 0.5f;
                if (std::abs(nearest_gap) <= area.safe_gap_degrees * 0.5f)
                    continue;
                const auto direction = Rotate({1.0f, 0.0f}, degrees);
                const auto position = Add(area.position, Multiply(direction, radius));
                complete &= snapshot.AddInstance(
                    {{position.x, 0.03f, position.y}, -degrees * kPi / 180.0f,
                     {0.24f, 0.04f, std::max(0.35f, radius * 0.05f)},
                     0xB04040FFu, RenderMesh::Area});
            }
        }
        else
        {
            const auto trail = area.half_length > 0.0f;
            complete &= snapshot.AddInstance(
                {{area.position.x, 0.01f, area.position.y},
                 trail ? std::atan2(area.direction.x, area.direction.y) : 0.0f,
                 trail ? Float3{area.radius * 2.0f, 0.04f,
                                area.half_length * 2.0f}
                       : Float3{area.radius, 0.04f, area.radius},
                 area.kind == AreaViewKind::EnemyDamage ? 0x604040FFu :
                 area.kind == AreaViewKind::Slow ? 0x6040A0FFu : 0x6080D040u,
                 RenderMesh::Area, kAreaRenderId | area.id.value});
        }
    }
    for (const auto &action : model.boss_actions)
    {
        const auto boss = std::ranges::find_if(
            model.enemies, [&](const EnemyView &enemy) {
                return enemy.id.value == action.boss_id && !enemy.dead;
            });
        if (boss == model.enemies.end())
            continue;

        const auto warning_color = 0xA03030FFu;
        if (action.kind == BossActionViewKind::Dash)
        {
            auto direction = action.direction;
            if (LengthSquared(direction) <= 0.0001f)
                direction = Normalize(Subtract(model.player.position, boss->position));
            constexpr std::uint32_t kMarkers = 20;
            for (std::uint32_t index = 1; index <= kMarkers; ++index)
            {
                const auto position = Add(
                    boss->position,
                    Multiply(direction, action.distance * static_cast<float>(index) /
                                            static_cast<float>(kMarkers)));
                complete &= snapshot.AddInstance(
                    {{position.x, 0.025f, position.y}, 0.0f,
                     {0.28f, 0.03f, 0.28f}, warning_color, RenderMesh::Area});
            }
        }
        else if (action.kind == BossActionViewKind::Volley)
        {
            const auto direction = LengthSquared(action.direction) > 0.0001f
                                       ? action.direction
                                       : Normalize(Subtract(model.player.position,
                                                            boss->position));
            const std::array angles{-action.arc_degrees * 0.5f + action.angle_offset,
                                    action.arc_degrees * 0.5f + action.angle_offset};
            for (const auto angle : angles)
            {
                const auto edge = Rotate(direction, angle);
                for (std::uint32_t index = 1; index <= 12; ++index)
                {
                    const auto position = Add(boss->position,
                        Multiply(edge, 24.0f * static_cast<float>(index) / 12.0f));
                    complete &= snapshot.AddInstance(
                        {{position.x, 0.025f, position.y}, 0.0f,
                         {0.22f, 0.03f, 0.22f}, warning_color, RenderMesh::Area});
                }
            }
        }
        else
        {
            const auto center = action.kind == BossActionViewKind::Shockwave
                                    ? boss->position : action.position;
            const auto radius = action.radius;
            constexpr std::uint32_t kSegments = 48;
            for (std::uint32_t index = 0; index < kSegments; ++index)
            {
                const auto degrees = 360.0f * static_cast<float>(index) / kSegments;
                if (action.kind == BossActionViewKind::Shockwave)
                {
                    constexpr auto kSpacing = 90.0f;
                    const auto offset = static_cast<float>(action.cast_id % 360);
                    const auto nearest_gap =
                        std::fmod(degrees - offset + kSpacing * 0.5f + 360.0f,
                                  kSpacing) - kSpacing * 0.5f;
                    if (std::abs(nearest_gap) <= 12.5f)
                        continue;
                }
                const auto direction = Rotate({1.0f, 0.0f}, degrees);
                const auto position = Add(center, Multiply(direction, radius));
                complete &= snapshot.AddInstance(
                    {{position.x, 0.025f, position.y}, 0.0f,
                     {0.24f, 0.03f, 0.24f}, warning_color, RenderMesh::Area});
            }
        }
    }
    for (const auto &pickup : model.pickups)
    {
        if (pickup.dead) continue;
        complete &= snapshot.AddInstance(
            {{pickup.position.x, 0.32f, pickup.position.y},
             pickup.kind == PickupKind::Experience ? 0.785398f : 0.0f,
             pickup.kind == PickupKind::Experience ? Float3{0.34f, 0.34f, 0.34f} :
             pickup.kind == PickupKind::Heal ? Float3{0.26f, 0.55f, 0.26f} :
             pickup.kind == PickupKind::Magnet ? Float3{0.48f, 0.48f, 0.48f} :
                                                 Float3{0.40f, 0.40f, 0.40f},
             pickup.kind == PickupKind::Experience ? 0xFFFFD040u :
             pickup.kind == PickupKind::Heal ? 0xFF40E060u :
             pickup.kind == PickupKind::Magnet ? 0xFFFF3030u : 0xFFE080FFu,
             RenderMesh::Pickup, kPickupRenderId | pickup.id.value});
    }
    complete &= snapshot.AddLight(
        {{-0.45f, -0.82f, 0.35f}, 3.0f, {1.0f, 0.92f, 0.78f}});

    const auto &probe = model.session;
    const auto add_ui = [&](UiModel::Kind kind, Float2 anchor, Float2 size,
                            std::uint32_t color, std::string_view text,
                            float value = 1.0f, std::uint16_t font = 28) {
        UiModel model;
        model.kind = kind;
        model.anchor_pixels = anchor;
        model.size_pixels = size;
        model.color_rgba = color;
        model.value = value;
        model.font_pixels = font;
        auto byte_count = std::min(text.size(), model.utf8_text.size() - 1);
        while (byte_count < text.size() && byte_count > 0 &&
               (static_cast<unsigned char>(text[byte_count]) & 0xC0u) == 0x80u)
            --byte_count;
        std::memcpy(model.utf8_text.data(), text.data(), byte_count);
        complete &= snapshot.AddUi(model);
    };
    constexpr std::array<std::string_view, kCombatSkillCount> skill_names{
        "기본 공격", "관통 사격", "다중 사격", "충전 사격", "폭발 화살",
        "도탄 화살", "화살비", "덫", "후퇴 사격"};
    constexpr std::array<std::string_view, kCombatSkillCount> skill_descriptions{
        "기본 공격을 유지하면 이동을 멈추고 조준 방향으로 화살을 반복 발사합니다. 화살은 처음 맞은 적에게 피해를 줍니다.",
        "조준 방향으로 즉시 관통 화살을 발사합니다. 많은 일반 적을 뚫지만 관통할수록 피해가 감소해 무리 정리에 적합합니다.",
        "조준 방향의 넓은 부채꼴에 화살 9발을 동시에 발사합니다. 가까이 모인 적이나 넓게 퍼진 무리를 상대하기 좋습니다.",
        "이동하며 최대 1초 충전하고 떼면 고화력 화살을 발사합니다. 오래 충전할수록 피해·사거리·크기가 증가하며 최대 12명을 추가 관통합니다.",
        "조준 방향으로 폭발 화살을 발사합니다. 처음 맞은 적 또는 최대 사거리에서 폭발해 주변의 모든 적을 공격합니다.",
        "사거리 안의 적을 자동 추적하는 화살을 발사합니다. 적중 후 아직 맞지 않은 가까운 적에게 연속으로 도탄합니다.",
        "커서 위치에 일정 시간 화살비를 내립니다. 범위 안의 적을 반복 공격하므로 오래 머무는 적에게 효과적입니다.",
        "조준 방향으로 전방 구르기하며 출발 지점에 덫을 설치합니다. 덫은 적이 접근하면 폭발해 주변을 공격하고 둔화시킵니다.",
        "조준 반대 방향으로 빠르게 물러나며 조준 방향으로 화살을 발사합니다. 이동 중에도 피해를 받을 수 있습니다."};
    constexpr std::array<std::array<std::string_view, 8>, kCombatSkillCount>
        skill_upgrade_names{{
            {{"연속 추가 화살", "추가 관통", "적중 분열", "출혈 화살",
              "화상 화살", "둔화 쿨타임 회수", "귀환 화살", "액티브 연계 사격"}},
            {{"후속 화살", "사거리 끝 분열", "관통 출혈", "피해 궤적",
              "관통 연쇄 사격", "적 밀어 정렬", "화상 전달", "빠른 재사용"}},
            {{"2차 부채", "적중 분열", "추가 관통", "후방 사격",
              "출혈 부채", "화상 전달", "화살 추가", "빗나감 재추적"}},
            {{"과충전 폭발", "빠른 충전", "즉시 사격 강화", "추가 관통",
              "완전 충전 출혈", "관통 분열", "화상 폭발", "다중 처치 쿨타임 회수"}},
            {{"재폭발", "소형 폭탄", "파편 폭발", "화상 지대",
              "출혈 연쇄 폭발", "폭발 흡인", "액티브 연계 표식", "빠른 재사용"}},
            {{"귀환 도탄", "분기 도탄", "출혈 도탄 연장", "화상 전달",
              "처치 소형 화살", "처치 연쇄 갱신", "도탄 쿨타임 회수", "빠른 재사용"}},
            {{"2차 화살비", "첫 타격 흡인", "반복 적중 출혈", "화상 지대",
              "추적 화살", "둔화 지대", "처치 추적 화살", "추가 타격과 둔화"}},
            {{"연속 덫", "착지 둔화", "덫 재활성", "출혈 덫",
              "화상 덫", "흡인 덫", "액티브 연계 표식", "처치 덫"}},
            {{"세 갈래 사격", "출발점 덫", "둔화 궤적", "출혈 추적 화살",
              "착지 충격", "다음 스킬 쿨타임 회수", "다중 적중 회복", "추가 후퇴"}}
        }};
    constexpr std::array<std::array<std::string_view, 8>, kCombatSkillCount>
        skill_upgrade_descriptions{{
            {{"기본 공격 3회마다 잠시 후 70% 위력의 기본 공격을 한 번 더 발사합니다. 추가 공격도 공격 횟수와 다른 기본 공격 강화를 적용합니다.",
              "기본 화살이 첫 적에게 멈추지 않고 뒤의 적 한 명까지 추가로 관통합니다.",
              "기본 화살이 처음 적중하면 그 지점에서 좌우로 약한 화살 2발이 갈라져 나갑니다.",
              "세 번째 기본 화살마다 적중한 대상에게 출혈을 부여합니다.",
              "네 번째 기본 화살마다 적중한 대상에게 화상을 부여합니다. 출혈 화살과 함께 발동할 수 있습니다.",
              "기본 화살의 첫 대상이 잠시 둔화됩니다. 동시에 남은 쿨타임이 가장 긴 액티브 스킬이 조금 회복됩니다.",
              "아무 적도 맞히지 못한 기본 화살이 한 번 되돌아오며 돌아오는 경로의 적을 공격합니다.",
              "액티브 스킬 사용 후 3초 안에 기본 공격하면 다음 공격이 세 갈래 화살로 바뀝니다."}},
            {{"관통 사격을 발사한 직후 새 대상을 추적하는 강한 후속 화살을 한 발 더 발사합니다.",
              "관통 화살이 최대 사거리에 도달하면 주변 적을 추적하는 화살 2발이 갈라져 나갑니다.",
              "관통 사격에 맞은 모든 적에게 출혈을 2중첩 부여합니다.",
              "관통 화살이 지나간 경로에 잠시 피해와 둔화를 주는 궤적이 남습니다.",
              "일반 적 3명을 관통할 때마다 진행 지점에서 좌우로 추가 화살을 발사합니다.",
              "관통 사격에 맞은 일반 적을 화살 진행 방향으로 밀어 뒤의 적과 한 줄로 모읍니다.",
              "첫 대상에게 화상을 부여하고, 화살이 다음 대상을 관통할 때 남은 화상을 전달합니다.",
              "관통 사격의 기본 쿨타임이 감소해 더 자주 사용할 수 있습니다."}},
            {{"다중 사격 후 잠시 뒤 같은 방향으로 약한 두 번째 부채 사격을 발사합니다.",
              "각 화살이 처음 적중한 지점에서 좌우로 약한 화살이 갈라져 나갑니다.",
              "다중 사격의 각 화살이 첫 적을 뚫고 뒤의 적 한 명까지 추가로 관통합니다.",
              "다중 사격과 동시에 등 뒤 방향으로도 화살 3발을 발사합니다.",
              "한 번의 다중 사격이 각 대상에게 처음 적중할 때 출혈을 1중첩 부여합니다.",
              "원래 화살이 맞힌 적을 태우고, 근처의 화상 없는 적에게 추적 화살과 화상을 최대 3회 전달합니다.",
              "부채꼴 바깥쪽에 기본 위력의 화살 2발을 더해 한 번에 11발을 발사합니다.",
              "원본과 강화로 생성된 화살이 빗나가면 약한 화살로 근처의 적을 한 번 추적합니다."}},
            {{"최대 충전 시간이 1.4초로 늘어나지만 완전 충전 피해가 크게 강해지고 화살 끝에서 폭발합니다.",
              "충전 속도가 빨라져 같은 위력의 화살을 더 짧게 눌러 발사할 수 있습니다.",
              "충전하지 않고 바로 발사해도 더 강한 피해를 줍니다. 오래 충전할수록 피해는 계속 증가합니다.",
              "추가 관통 수가 4 늘어나 최대 16명의 적을 추가 관통합니다.",
              "충전 화살이 출혈을 3중첩 부여합니다. 완전 충전으로 출혈이 가득한 적을 맞히면 추가 피해를 줍니다.",
              "보스를 처음 맞히거나 일반 적 3명을 관통하면 적중 지점에서 여덟 방향으로 강한 화살이 갈라집니다.",
              "첫 적중 대상을 태우고 그 주변을 폭발시켜 모여 있는 적을 함께 공격합니다.",
              "완전 충전 한 발로 일반 적 3명 이상을 처치하면 충전 사격의 남은 쿨타임 일부를 돌려받습니다."}},
            {{"주 폭발이 끝난 뒤 같은 위치에서 더 약한 폭발이 한 번 추가로 일어납니다.",
              "주 폭발 주변에 소형 폭탄 3개가 생겨 흩어진 적을 추가로 공격합니다.",
              "주 폭발 지점에서 여덟 방향으로 파편 화살을 발사해 바깥의 적까지 공격합니다.",
              "주 폭발 위치에 잠시 불장판이 남아 안의 적을 반복 공격하고 화상을 부여합니다.",
              "주 폭발에 맞은 적마다 출혈을 부여하고 작은 혈폭을 일으킵니다. 한 번의 시전당 최대 8회 발생합니다.",
              "폭발 직전에 주변 일반 적을 중심으로 끌어당겨 폭발 범위 안에 모읍니다.",
              "직접 맞히면 즉시 추가 폭발을 일으키고 표식을 남깁니다. 살아남은 적을 다른 공격으로 맞히면 다시 폭발합니다.",
              "폭발 화살의 기본 쿨타임이 감소해 더 자주 사용할 수 있습니다."}},
            {{"마지막 도탄 뒤 화살이 플레이어에게 돌아오며, 오는 길에 이전 대상들을 한 번 더 공격합니다.",
              "첫 번째 도탄 지점에서 화살이 세 갈래 연쇄로 나뉘어 서로 다른 적을 추적합니다.",
              "맞은 적에게 출혈을 부여합니다. 출혈 적을 맞힐수록 이번 화살의 남은 도탄 횟수가 최대 3회 늘어납니다.",
              "맞은 적을 태우고, 다음 도탄 대상에게 현재 남은 화상을 그대로 복제합니다.",
              "도탄 화살로 적을 처치하면 소형 화살 3발이 생깁니다. 한 번의 시전당 최대 9발 생성됩니다.",
              "원본 도탄 화살로 적을 처치하면 새 화살이 생겨 주변의 적 3명에게 다시 도탄합니다.",
              "원본 화살이 도탄할 때마다 다른 액티브 중 남은 쿨타임이 가장 긴 스킬을 조금씩 회복합니다.",
              "도탄 화살의 기본 쿨타임이 감소해 더 자주 사용할 수 있습니다."}},
            {{"첫 화살비가 시작된 뒤 시전 방향 앞쪽에 더 작고 짧은 두 번째 화살비가 생깁니다.",
              "첫 피해가 발생할 때 범위 안의 일반 적을 중심으로 끌어당깁니다.",
              "같은 화살비가 한 적을 세 번 맞힐 때마다 강한 출혈을 부여합니다.",
              "맞은 적을 태우고 그 자리에 작은 불장판을 만듭니다. 한 번의 시전당 최대 4개 생성됩니다.",
              "화살비가 피해를 줄 때마다 범위 근처의 가장 가까운 적에게 추적 화살을 발사합니다.",
              "화살비 안에 머무는 적에게 강한 둔화를 피해 주기마다 새로 부여합니다.",
              "화살비 안에서 적이 죽으면 범위 밖의 가까운 적에게 추적 화살을 발사합니다. 최대 6회 발동합니다.",
              "화살비가 두 번 더 공격하고, 종료된 자리에 강한 둔화 지대를 남깁니다."}},
            {{"구르는 경로를 따라 덫 3개를 설치합니다. 각 덫은 기본 덫보다 약하지만 따로 발동합니다.",
              "구르기가 끝난 위치에 강한 둔화 지대를 만들어 추격해 오는 적을 크게 늦춥니다.",
              "한 번 폭발한 덫이 사라지지 않고 잠시 뒤 다시 활성화되어 한 번 더 발동할 수 있습니다.",
              "덫이 폭발할 때 맞은 모든 적에게 출혈을 3중첩 부여합니다.",
              "덫이 폭발할 때 화상을 부여하고 작은 불장판을 남깁니다.",
              "덫이 폭발하기 직전 주변 적을 끌어당기고, 폭발에 맞은 적을 강하게 둔화시킵니다.",
              "덫을 발동시킨 적에게 즉시 추가 피해를 주고 표식을 남깁니다. 살아남은 적을 다른 공격으로 맞히면 폭발합니다.",
              "덫으로 적을 처치하면 가까운 적 옆에 소형 덫을 만듭니다. 한 번의 시전당 최대 3개 생성됩니다."}},
            {{"후퇴 사격의 한 발이 세 갈래 화살로 바뀌어 더 넓은 범위를 공격합니다.",
              "후퇴를 시작한 위치에 소형 덫을 남겨 따라오는 적을 공격합니다.",
              "후퇴한 경로에 강한 둔화 지대를 남겨 쫓아오는 적의 이동을 크게 늦춥니다.",
              "맞은 적에게 출혈을 부여하고 주변 적에게 추적 화살을 발사합니다. 최대 3회 발동합니다.",
              "후퇴가 끝난 지점에서 충격파를 일으켜 주변 적을 공격하고 일반 적을 밀어냅니다.",
              "후퇴 사격 후 3초 안에 다른 액티브를 사용하면 그 스킬의 남은 쿨타임 일부를 돌려받습니다.",
              "보스를 맞히거나 일반 적 3명 이상을 맞히면 최대 체력의 일부를 회복합니다.",
              "첫 후퇴 직후 한 번 더 물러나며 조준 방향으로 약한 추가 화살을 발사합니다."}}
        }};
    static_assert([] {
        for (const auto description : skill_descriptions)
            if (description.empty() || description.size() > 430) return false;
        for (const auto &skill : skill_upgrade_descriptions)
            for (const auto description : skill)
                if (description.empty() || description.size() > 400) return false;
        return true;
    }(), "Card descriptions must fit the UTF-8 UI text buffer.");
    constexpr std::array<std::string_view, kStatCount> stat_names{
        "최대 체력", "이동속도", "공격력", "공격속도", "쿨타임 감소", "자석 반경"};
    const auto key_name = [](std::uint16_t key) {
        if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z'))
            return std::string(1, static_cast<char>(key));
        if (key >= 0x70 && key <= 0x87)
            return std::format("F{}", key - 0x6F);
        return std::format("VK {}", key);
    };

    if (probe.phase == SessionPhase::MainMenu ||
        (probe.phase == SessionPhase::Paused && ui.page == UiPage::PauseSettings))
    {
        if (probe.phase == SessionPhase::MainMenu)
            add_ui(UiModel::Kind::Text, {760, 120}, {400, 80}, 0xFFFFFFFFu,
                   "PROJECT HS", 1.0f, 54);
        if (ui.page == UiPage::Collection)
        {
            const auto selected = std::min<std::size_t>(ui.selected_collection_skill,
                                                         kCombatSkillCount - 1);
            add_ui(UiModel::Kind::Panel, {160, 70}, {1'600, 930}, 0xD0202430u,
                   "스킬 도감");
            add_ui(UiModel::Kind::Text, {210, 95}, {360, 42}, 0xFFFFFFFFu,
                   "스킬 도감", 1.0f, 34);
            for (std::size_t skill = 0; skill < kCombatSkillCount; ++skill)
            {
                add_ui(UiModel::Kind::Button,
                       {210, 150.0f + static_cast<float>(skill) * 70.0f},
                       {360, 56}, selected == skill ? 0xFF507098u : 0xFF34495Eu,
                       skill_names[skill], 1.0f, 22);
            }

            add_ui(UiModel::Kind::Text, {620, 105}, {1'090, 46}, 0xFFFFFFFFu,
                   skill_names[selected], 1.0f, 32);
            add_ui(UiModel::Kind::Text, {620, 160}, {1'090, 125}, 0xFFE2E8F0u,
                   skill_descriptions[selected], 1.0f, 22);
            add_ui(UiModel::Kind::Text, {620, 285}, {1'090, 35}, 0xFFB8C2D0u,
                   "선택 가능한 강화 8종", 1.0f, 20);
            for (std::size_t upgrade = 0; upgrade < kUpgradeCount; ++upgrade)
            {
                const auto column = static_cast<float>(upgrade % 2);
                const auto row = static_cast<float>(upgrade / 2);
                add_ui(UiModel::Kind::Panel,
                       {620.0f + column * 545.0f, 330.0f + row * 145.0f},
                       {520, 125}, 0xFF2D4058u,
                       std::format("강화 {} · {}\n{}", upgrade + 1,
                                   skill_upgrade_names[selected][upgrade],
                                   skill_upgrade_descriptions[selected][upgrade]),
                       1.0f, 19);
            }
            add_ui(UiModel::Kind::Button, {210, 900}, {360, 56}, 0xFF3A5068u,
                   "돌아가기");
        }
        else if (ui.page == UiPage::MainMenuSettings ||
                 ui.page == UiPage::PauseSettings)
        {
            const auto enabled = [](bool value) { return value ? "켜짐" : "꺼짐"; };
            add_ui(UiModel::Kind::Panel, {450, 180}, {1'020, 790}, 0xD0202430u,
                   "설정");
            constexpr std::array<float, 8> left_y{250, 320, 390, 460, 530, 600, 670, 740};
            const std::array left_text{
                std::format("화면: {}", settings.borderless ? "테두리 없음" : "창"),
                std::format("VSync: {}", enabled(settings.vsync)),
                std::format("프레임 제한: {}", settings.frame_cap == 0
                                                   ? std::string("무제한")
                                                   : std::to_string(settings.frame_cap)),
                std::format("렌더 스케일: {}%", settings.render_scale_percent),
                std::format("그림자: {}", settings.shadow_resolution),
                std::format("파티클: {}%", settings.particle_percentage),
                std::format("Bloom: {}", enabled(settings.bloom)),
                std::format("외곽선: {}", enabled(settings.outline))};
            for (std::size_t index = 0; index < left_text.size(); ++index)
                add_ui(UiModel::Kind::Button, {500, left_y[index]}, {420, 56},
                       0xFF34495Eu, left_text[index], 1.0f, 23);

            constexpr std::array<float, 4> volume_y{250, 320, 390, 460};
            const std::array volume_text{
                std::format("-  Master {:3}%  +", std::lround(settings.master_volume * 100)),
                std::format("-  BGM {:3}%  +", std::lround(settings.bgm_volume * 100)),
                std::format("-  SFX {:3}%  +", std::lround(settings.sfx_volume * 100)),
                std::format("-  UI {:3}%  +", std::lround(settings.ui_volume * 100))};
            for (std::size_t index = 0; index < volume_text.size(); ++index)
                add_ui(UiModel::Kind::Button, {1'000, volume_y[index]}, {420, 56},
                       0xFF34495Eu, volume_text[index], 1.0f, 23);

            constexpr std::array<std::string_view, 4> slots{"Q", "W", "E", "R"};
            for (std::size_t slot = 0; slot < slots.size(); ++slot)
            {
                const auto waiting = pending_rebind_slot == slot;
                add_ui(UiModel::Kind::Button,
                       {1'000, 550.0f + static_cast<float>(slot) * 70.0f}, {420, 56},
                       waiting ? 0xFF8A5A30u : 0xFF34495Eu,
                       waiting ? std::format("{}: 새 키 입력...", slots[slot])
                               : std::format("{}: {}", slots[slot],
                                             key_name(settings.skill_virtual_keys[slot])),
                       1.0f, 23);
            }
            add_ui(UiModel::Kind::Text, {1'000, 830}, {420, 32}, 0xFFFFFFFFu,
                   "사용 중인 키 선택 시 서로 교환", 1.0f, 20);
            add_ui(UiModel::Kind::Button, {760, 870}, {400, 64}, 0xFF3A5068u,
                   "돌아가기");
        }
        else
        {
            constexpr std::array<std::string_view, 4> labels{
                "시작", "컬렉션", "설정", "종료"};
            for (std::size_t index = 0; index < labels.size(); ++index)
            {
                add_ui(UiModel::Kind::Button,
                       {760.0f, 270.0f + static_cast<float>(index) * 150.0f},
                       {400, 92}, 0xFF34495Eu, labels[index], 1.0f, 34);
            }
        }
    }
    else if (!(probe.phase == SessionPhase::Paused &&
               (ui.page == UiPage::CharacterOverview ||
                ui.page == UiPage::CharacterSkills ||
                ui.page == UiPage::CharacterStats)))
    {
        const auto time = probe.final_boss_spawned ? probe.boss_fight_ticks
                                                   : probe.growth_ticks;
        add_ui(UiModel::Kind::Panel, {20, 20}, {452, 146}, 0xA8181D28u, "");
        add_ui(UiModel::Kind::Text, {32, 32}, {680, 50}, 0xFFFFFFFFu,
               std::format("LV {}  {:02}:{:02}  적 {}", probe.level,
                           (time / 60) / 60, (time / 60) % 60,
                           probe.normal_enemy_count));
        add_ui(UiModel::Kind::Bar, {32, 92}, {420, 30}, 0xFFE85050u,
               std::format("HP {}/{}", probe.health, probe.max_health),
               static_cast<float>(std::max(probe.health, 0)) /
                   static_cast<float>(probe.max_health));
        add_ui(UiModel::Kind::Bar, {32, 132}, {420, 24}, 0xFFFFC840u,
               std::format("XP {}/{}", probe.experience, probe.experience_to_next),
               static_cast<float>(probe.experience) /
                   static_cast<float>(probe.experience_to_next));
        add_ui(UiModel::Kind::Button, {530, 948}, {180, 92}, 0xFF31425Au,
               std::format("LMB\n{} Lv{}", skill_names[0], probe.skill_levels[0]),
               1.0f, 22);
        std::array<std::string, 4> keys;
        for (std::size_t slot = 0; slot < keys.size(); ++slot)
            keys[slot] = key_name(settings.skill_virtual_keys[slot]);
        for (std::size_t slot = 0; slot < keys.size(); ++slot)
        {
            const auto skill = model.player.loadout[slot];
            const auto text = skill == SkillKind::Count
                ? std::format("{}\n-", keys[slot])
                : std::format("{}\n{} Lv{}  {:.1f}s", keys[slot],
                              skill_names[static_cast<std::size_t>(skill)],
                              probe.skill_levels[static_cast<std::size_t>(skill)],
                              static_cast<float>(probe.cooldown_ticks[
                                  static_cast<std::size_t>(skill) - 1]) / 60.0f);
            add_ui(UiModel::Kind::Button,
                   {730.0f + static_cast<float>(slot) * 190.0f, 948},
                   {180, 92}, 0xFF31425Au, text, 1.0f, 20);
        }
        std::string relic_hud = std::format("유물 {}", std::popcount(probe.relic_mask));
        std::size_t shown_relics{};
        for (std::size_t relic = 0; relic < kRelicCount && shown_relics < 3; ++relic)
        {
            if (!HasRelic(probe.relic_mask, static_cast<RelicKind>(relic))) continue;
            relic_hud += std::format("\n{}", presentation.relic_names[relic].data());
            ++shown_relics;
        }
        const auto hidden_relics = std::popcount(probe.relic_mask) - shown_relics;
        if (hidden_relics > 0) relic_hud += std::format("  외 {}개", hidden_relics);
        add_ui(UiModel::Kind::Panel, {1'510, 24}, {380, 122}, 0xB8202B3Au, "");
        add_ui(UiModel::Kind::Text, {1'530, 34}, {340, 102}, 0xFFF0D890u,
               relic_hud, 1.0f, 18);

        for (const auto &wave : model.waves)
        {
            const auto start = wave.start;
            if (probe.growth_ticks < start &&
                start - probe.growth_ticks <= Seconds(5.0f))
            {
                const auto seconds = (start - probe.growth_ticks + 59) / 60;
                add_ui(UiModel::Kind::Text, {610, 180}, {700, 64}, 0xFFFFA030u,
                       std::format("대규모 웨이브까지 {}초", seconds), 1.0f, 36);
                break;
            }
            if (probe.growth_ticks >= start &&
                probe.growth_ticks < start + wave.duration)
            {
                add_ui(UiModel::Kind::Text, {610, 180}, {700, 64}, 0xFFFF4040u,
                       "대규모 웨이브 발생", 1.0f, 36);
                break;
            }
        }

        std::vector<const EnemyView *> bosses;
        for (const auto &enemy : model.enemies)
        {
            if (enemy.boss && !enemy.dead) bosses.push_back(&enemy);
        }
        std::ranges::sort(bosses,
                          [](const EnemyView *left, const EnemyView *right) {
            const auto left_final = left->boss == BossKind::Final;
            const auto right_final = right->boss == BossKind::Final;
            return left_final != right_final ? left_final :
                   left->spawned_tick < right->spawned_tick;
        });
        constexpr std::array<std::string_view, 3> boss_names{
            "5분 보스", "10분 보스", "최종 보스"};
        for (std::size_t index = 0; index < bosses.size(); ++index)
        {
            const auto &boss = *bosses[index];
            add_ui(UiModel::Kind::Bar,
                   {560, 32.0f + static_cast<float>(index) * 46.0f}, {800, 38},
                   0xFFE04070u,
                   std::format("{} {}/{}",
                               boss_names[static_cast<std::size_t>(*boss.boss)],
                               boss.health, boss.max_health),
                   static_cast<float>(std::max(boss.health, 0)) /
                       static_cast<float>(boss.max_health));
        }
    }

    if (probe.phase == SessionPhase::CardSelection ||
        probe.phase == SessionPhase::RelicSelection)
    {
        add_ui(UiModel::Kind::Panel, {300, 160}, {1'320, 760}, 0xE0181D28u,
               probe.phase == SessionPhase::RelicSelection ? "유물 선택" : "레벨업");
        for (std::size_t index = 0; index < probe.card_count; ++index)
        {
            const auto card = probe.cards[index];
            std::string title;
            std::string detail;
            if (card.kind == CardKind::LearnSkill)
            {
                title = std::format("새 스킬 · {}", skill_names[card.subject]);
                detail = std::string(skill_descriptions[card.subject]);
            }
            else if (card.kind == CardKind::Relic)
            {
                title = std::format("유물 · {}",
                                    presentation.relic_names[card.subject].data());
                detail = presentation.relic_rules[card.subject].data();
            }
            else if (card.kind == CardKind::BonusStatPoint)
            {
                title = "보너스 · 스탯 포인트 +1";
                detail = "원하는 능력치에 투자할 포인트를 하나 더 얻습니다.";
            }
            else
            {
                title = std::format("{} · 강화 {}", skill_names[card.subject],
                                    static_cast<unsigned>(card.upgrade) + 1);
                detail = std::format("{}\n\n{}",
                    skill_upgrade_names[card.subject][card.upgrade],
                    skill_upgrade_descriptions[card.subject][card.upgrade]);
            }
            const auto x = 360.0f + static_cast<float>(index) * 420.0f;
            const auto accent = card.kind == CardKind::Relic ? 0xFFFFD06Au :
                                card.kind == CardKind::BonusStatPoint ? 0xFF76E0A0u :
                                                                      0xFF78B8FFu;
            add_ui(UiModel::Kind::Button,
                   {x, 300}, {360, 420}, 0xF02A394Cu, "");
            add_ui(UiModel::Kind::Text, {x + 24, 326}, {312, 72}, accent,
                   title, 1.0f, 27);
            add_ui(UiModel::Kind::Text, {x + 24, 420}, {312, 240}, 0xFFD7E0ECu,
                   detail, 1.0f, 20);
            add_ui(UiModel::Kind::Text, {x + 24, 674}, {312, 24}, 0xFF8190A4u,
                   std::format("클릭하여 선택  ·  {}", index + 1), 1.0f, 15);
        }
        add_ui(UiModel::Kind::Button, {760, 790}, {400, 72}, 0xFF5A4050u,
               std::format("재추첨 {}",
                           probe.phase == SessionPhase::RelicSelection
                               ? probe.relic_rerolls_remaining
                               : probe.level_rerolls_remaining));
    }
    else if (probe.phase == SessionPhase::StatAllocation)
    {
        add_ui(UiModel::Kind::Panel, {300, 160}, {1'320, 760}, 0xE0181D28u,
               std::format("스탯 배분  남은 포인트 {}", probe.pending_stat_points));
        for (std::size_t index = 0; index < stat_names.size(); ++index)
        {
            add_ui(UiModel::Kind::Button,
                   {390.0f + static_cast<float>(index % 3) * 400.0f,
                    310.0f + static_cast<float>(index / 3) * 260.0f},
                   {340, 180}, 0xFF334A64u,
                   std::format("{}\n{}/10", stat_names[index], probe.stat_points[index]));
        }
    }
    else if (probe.phase == SessionPhase::Paused)
    {
        if (ui.page == UiPage::Root)
        {
            add_ui(UiModel::Kind::Panel, {660, 300}, {600, 480}, 0xE0181D28u,
                   "일시정지", 1.0f, 38);
            add_ui(UiModel::Kind::Button, {760, 420}, {400, 72}, 0xFF34495Eu,
                   "계속", 1.0f, 28);
            add_ui(UiModel::Kind::Button, {760, 520}, {400, 72}, 0xFF34495Eu,
                   "설정", 1.0f, 28);
            add_ui(UiModel::Kind::Button, {760, 620}, {400, 72}, 0xFF5A4050u,
                   "게임 종료", 1.0f, 28);
            add_ui(UiModel::Kind::Text, {760, 710}, {400, 32}, 0xFFB8C2D0u,
                   "Esc: 계속", 1.0f, 20);
        }
        else if (ui.page == UiPage::CharacterOverview ||
                 ui.page == UiPage::CharacterSkills ||
                 ui.page == UiPage::CharacterStats)
        {
            add_ui(UiModel::Kind::Panel, {260, 80}, {1'400, 920}, 0xF0181D28u,
                   "");
            constexpr std::array<std::string_view, 3> tabs{
                "능력치·피해", "스킬·강화", "유물"};
            constexpr std::array tab_pages{
                UiPage::CharacterOverview, UiPage::CharacterSkills,
                UiPage::CharacterStats};
            for (std::size_t index = 0; index < tabs.size(); ++index)
            {
                add_ui(UiModel::Kind::Button,
                       {350.0f + static_cast<float>(index) * 300.0f, 140},
                        {280, 58}, ui.page == tab_pages[index]
                                      ? 0xFF507098u
                                                               : 0xFF34495Eu,
                       tabs[index], 1.0f, 24);
            }
            add_ui(UiModel::Kind::Button, {1'520, 140}, {120, 58}, 0xFF5A4050u,
                   "닫기", 1.0f, 22);
            add_ui(UiModel::Kind::Text, {300, 95}, {1'300, 38}, 0xFFFFFFFFu,
                   "캐릭터 정보  ·  Tab 또는 Esc로 닫기", 1.0f, 26);

            if (ui.page == UiPage::CharacterOverview)
            {
                const auto attack = model.effective_attack;
                const auto attack_speed = model.effective_attack_speed;
                const auto move_speed = model.effective_move_speed;
                const auto magnet_radius = model.effective_magnet_radius;
                const auto cooldown_reduction =
                    0.03f * probe.stat_points[
                                static_cast<std::size_t>(StatKind::CooldownReduction)];
                add_ui(UiModel::Kind::Text, {350, 235}, {570, 470}, 0xFFFFFFFFu,
                       std::format(
                           "상세 능력치\n\n"
                           "레벨                 {}\n"
                           "현재 체력             {} / {}\n"
                           "공격력                {:.1f}\n"
                           "기본 공격 피해        {}\n"
                           "기본 공격속도         {:.3f}회/초\n"
                           "이동속도              {:.2f}m/초\n"
                           "쿨타임 감소           {:.0f}%\n"
                           "자석 반경             {:.1f}m",
                           probe.level, probe.health, probe.max_health, attack,
                           model.skills[0].displayed_damage,
                           attack_speed, move_speed, cooldown_reduction * 100.0f,
                           magnet_radius),
                       1.0f, 23);
                add_ui(UiModel::Kind::Text, {350, 725}, {570, 160}, 0xFFFFFFFFu,
                       std::format(
                           "투자 포인트\n"
                           "체력 {} · 이동 {} · 공격 {}\n"
                           "공속 {} · 쿨감 {} · 자석 {}",
                           probe.stat_points[0], probe.stat_points[1],
                           probe.stat_points[2], probe.stat_points[3],
                           probe.stat_points[4], probe.stat_points[5]),
                       1.0f, 23);

                add_ui(UiModel::Kind::Text, {990, 235}, {570, 330}, 0xFFFFFFFFu,
                       std::format(
                    "현재 전투 기록\n\n총 피해              {}\n직접 피해            {}\n"
                    "파생 효과 피해        {}\n지속 피해            {}\n받은 피해            {}\n"
                    "회복량                {}\n처치 수              {}",
                    probe.damage_dealt, model.summary.direct_damage,
                    model.summary.derived_damage,
                    model.summary.damage_over_time,
                    probe.damage_taken, probe.healing, probe.kills),
                       1.0f, 23);
                add_ui(UiModel::Kind::Text, {990, 585}, {570, 42}, 0xFFFFFFFFu,
                       "스킬별 누적 피해", 1.0f, 23);
                std::size_t damage_row{};
                for (std::size_t skill = 0; skill < kCombatSkillCount; ++skill)
                {
                    if (probe.skill_levels[skill] == 0 && probe.damage_by_skill[skill] == 0)
                        continue;
                    add_ui(UiModel::Kind::Text,
                           {990, 635.0f + static_cast<float>(damage_row) * 30.0f},
                           {570, 28}, 0xFFE2E8F0u,
                           std::format("{}  {}", skill_names[skill],
                                       probe.damage_by_skill[skill]),
                           1.0f, 20);
                    ++damage_row;
                }
            }
            else if (ui.page == UiPage::CharacterSkills)
            {
                constexpr std::array<std::string_view, 4> slot_names{"Q", "W", "E", "R"};
                auto selected = std::min<std::size_t>(ui.selected_character_skill,
                                                       kCombatSkillCount - 1);
                if (probe.skill_levels[selected] == 0) selected = 0;
                for (std::size_t skill = 0; skill < kCombatSkillCount; ++skill)
                {
                    if (probe.skill_levels[skill] == 0) continue;
                    add_ui(UiModel::Kind::Button,
                           {350, 240.0f + static_cast<float>(skill) * 78.0f},
                           {360, 64}, selected == skill ? 0xFF507098u : 0xFF34495Eu,
                           std::format("{}  Lv{}", skill_names[skill],
                                       probe.skill_levels[skill]),
                           1.0f, 21);
                }

                add_ui(UiModel::Kind::Text, {780, 200}, {780, 22}, 0xFFB8C2D0u,
                       "두 슬롯을 차례로 선택하면 스킬을 이동하거나 교환합니다.",
                       1.0f, 16);

                for (std::size_t slot = 0; slot < probe.skill_loadout.size(); ++slot)
                {
                    const auto skill = probe.skill_loadout[slot];
                    const auto label = skill < SkillKind::Count
                        ? std::format("{}  {}", slot_names[slot],
                                      skill_names[static_cast<std::size_t>(skill)])
                        : std::format("{}  비어 있음", slot_names[slot]);
                    add_ui(UiModel::Kind::Button,
                           {780.0f + static_cast<float>(slot) * 195.0f, 225.0f},
                           {180, 54}, ui.loadout_source_slot == slot
                                          ? 0xFFB87828u
                                          : 0xFF34495Eu,
                           label, 1.0f, 18);
                }

                const auto &definition = model.skills[selected];
                const auto damage = definition.displayed_damage;
                std::string parameters = selected == 0
                    ? std::format("1발 피해 {}  ·  공격속도 {:.3f}회/초  ·  사거리 {:.1f}m",
                                  damage, model.effective_attack_speed,
                                  definition.effective_range)
                    : std::format("표기 피해 {}  ·  쿨타임 {:.2f}초  ·  사거리 {:.1f}m",
                                  damage,
                                  static_cast<float>(definition.effective_cooldown) / 60.0f,
                                  definition.effective_range);
                if (definition.area_radius > 0.0f)
                    parameters += std::format("  ·  범위 {:.1f}m", definition.area_radius);
                if (definition.duration > 0)
                    parameters += std::format("  ·  지속 {:.1f}초",
                                              definition.duration / 60.0f);
                if (definition.projectile_count > 1)
                    parameters += std::format("  ·  발사 {}개", definition.projectile_count);
                if (definition.pierce_count > 0)
                    parameters += std::format("  ·  추가 관통 {}", definition.pierce_count);

                add_ui(UiModel::Kind::Text, {780, 285}, {780, 58}, 0xFFFFFFFFu,
                       std::format("{}  Lv{}  ·  누적 피해 {}", skill_names[selected],
                                   probe.skill_levels[selected],
                                   probe.damage_by_skill[selected]),
                       1.0f, 28);
                add_ui(UiModel::Kind::Text, {780, 345}, {780, 110}, 0xFFFFFFFFu,
                       skill_descriptions[selected], 1.0f, 22);
                add_ui(UiModel::Kind::Text, {780, 460}, {780, 60}, 0xFFFFFFFFu,
                       parameters, 1.0f, 20);

                std::size_t upgrade_row{};
                for (std::uint8_t upgrade = 0; upgrade < kUpgradeCount; ++upgrade)
                {
                    if (!HasUpgrade(probe.upgrade_masks[selected], upgrade + 1)) continue;
                    add_ui(UiModel::Kind::Button,
                           {780, 530.0f + static_cast<float>(upgrade_row) * 98.0f},
                           {780, 86}, 0xFF2D4058u,
                            std::format("강화 {} · {}  ·  기여 피해 {}\n{}",
                                        static_cast<unsigned>(upgrade) + 1,
                                        skill_upgrade_names[selected][upgrade],
                                        model.summary.upgrade_damage[selected][upgrade],
                                        skill_upgrade_descriptions[selected][upgrade]),
                           1.0f, 19);
                    ++upgrade_row;
                }
                if (upgrade_row == 0)
                    add_ui(UiModel::Kind::Text, {780, 540}, {780, 50}, 0xFFB8C2D0u,
                           "현재 선택한 강화가 없습니다.", 1.0f, 21);
            }
            else
            {
                std::size_t relic_count{};
                for (std::size_t relic = 0; relic < kRelicCount; ++relic)
                {
                    if (!HasRelic(probe.relic_mask, static_cast<RelicKind>(relic))) continue;
                    const auto column = relic_count / 5;
                    const auto row = relic_count % 5;
                    const auto &effects = model.summary.relic_effects[relic];
                    const auto cooldown = effects[static_cast<std::size_t>(
                        UpgradeEffectMetric::CooldownTicksSaved)];
                    const auto prevented = effects[static_cast<std::size_t>(
                        UpgradeEffectMetric::DamagePrevented)];
                    const auto healing = effects[static_cast<std::size_t>(
                        UpgradeEffectMetric::Healing)];
                    std::string utility;
                    if (cooldown > 0)
                        utility += std::format(" · 쿨감 {:.1f}초",
                                               static_cast<double>(cooldown) / 60.0);
                    if (prevented > 0) utility += std::format(" · 방어 {}", prevented);
                    if (healing > 0) utility += std::format(" · 회복 {}", healing);
                    add_ui(UiModel::Kind::Button,
                           {300.0f + static_cast<float>(column) * 405.0f,
                            235.0f + static_cast<float>(row) * 140.0f},
                           {385, 125}, 0xFF2D4058u,
                           std::format("{}\n발동 {} · 피해 {} · 킬 {}{}\n{}",
                                       presentation.relic_names[relic].data(),
                                       model.summary.relic_triggers[relic],
                                       model.summary.relic_damage[relic],
                                       model.summary.relic_kills[relic], utility,
                                       presentation.relic_rules[relic].data()),
                           1.0f, 14);
                    ++relic_count;
                }
                if (relic_count == 0)
                    add_ui(UiModel::Kind::Text, {350, 250}, {1'200, 80}, 0xFFB8C2D0u,
                           "현재 획득한 유물이 없습니다.", 1.0f, 24);
            }
        }
    }
    else if (probe.phase == SessionPhase::Victory || probe.phase == SessionPhase::Defeat)
    {
        add_ui(UiModel::Kind::Panel, {500, 100}, {920, 880}, 0xE0181D28u,
               "");
        add_ui(UiModel::Kind::Text, {550, 130}, {820, 70},
               probe.phase == SessionPhase::Victory ? 0xFFFFD06Au : 0xFFFF7070u,
               std::format("{}  레벨 {}  처치 {}  보스전 {:02}:{:02}",
                           probe.phase == SessionPhase::Victory ? "승리" : "패배",
                           probe.level, probe.kills,
                           (probe.boss_fight_ticks / 60) / 60,
                           (probe.boss_fight_ticks / 60) % 60), 1.0f, 32);
        add_ui(UiModel::Kind::Text, {550, 210}, {820, 60}, 0xFFFFFFFFu,
               std::format("피해 {}  피격 {}  회복 {}",
                           probe.damage_dealt, probe.damage_taken, probe.healing), 1.0f, 28);
        std::array<std::pair<std::uint64_t, std::size_t>, kCombatSkillCount> ranking{};
        for (std::size_t index = 0; index < ranking.size(); ++index)
            ranking[index] = {probe.damage_by_skill[index], index};
        std::ranges::sort(ranking, std::greater{},
                          [](const auto &entry) { return entry.first; });
        std::string damage_table = "전투 피해 통계\n";
        for (const auto [damage, index] : ranking)
        {
            if (damage == 0) continue;
            damage_table += std::format("{}  {}\n", skill_names[index], damage);
        }
        if (damage_table == "전투 피해 통계\n") damage_table += "기록 없음";
        add_ui(UiModel::Kind::Text, {550, 285}, {820, 260}, 0xFFFFFFFFu,
               damage_table, 1.0f, 23);
        std::string skill_build = std::format(
            "{} Lv{} · 강화 {}개", skill_names[0], probe.skill_levels[0],
            std::popcount(probe.upgrade_masks[0]));
        for (const auto skill : probe.skill_loadout)
        {
            if (skill == SkillKind::Count) continue;
            const auto index = static_cast<std::size_t>(skill);
            skill_build += std::format("\n{} Lv{} · 강화 {}개", skill_names[index],
                                       probe.skill_levels[index],
                                       std::popcount(probe.upgrade_masks[index]));
        }
        add_ui(UiModel::Kind::Text, {550, 555}, {820, 32}, 0xFFFFD06Au,
               "빌드 요약", 1.0f, 24);
        add_ui(UiModel::Kind::Text, {550, 590}, {820, 105}, 0xFFD7E0ECu,
               skill_build, 1.0f, 18);
        std::string result_relics = std::format("유물 {}개", std::popcount(probe.relic_mask));
        std::size_t result_relic_count{};
        for (std::size_t relic = 0; relic < kRelicCount; ++relic)
        {
            if (!HasRelic(probe.relic_mask, static_cast<RelicKind>(relic))) continue;
            if (result_relic_count == 4) break;
            result_relics += std::format("\n{} · 발동 {} · 피해 {} · 킬 {}",
                                         presentation.relic_names[relic].data(),
                                         model.summary.relic_triggers[relic],
                                         model.summary.relic_damage[relic],
                                         model.summary.relic_kills[relic]);
            ++result_relic_count;
        }
        const auto hidden_result_relics =
            std::popcount(probe.relic_mask) - result_relic_count;
        if (hidden_result_relics > 0)
            result_relics += std::format("\n외 {}개", hidden_result_relics);
        add_ui(UiModel::Kind::Text, {550, 700}, {820, 135}, 0xFFD7E0ECu,
               result_relics, 1.0f, 18);
        add_ui(UiModel::Kind::Text, {550, 840}, {820, 45}, 0xFF8190A4u,
               std::format("스탯 {}/{}/{}/{}/{}/{}  시드 {}",
                           probe.stat_points[0], probe.stat_points[1],
                           probe.stat_points[2], probe.stat_points[3],
                           probe.stat_points[4], probe.stat_points[5], model.seed),
               1.0f, 25);
        add_ui(UiModel::Kind::Text, {550, 900}, {820, 50}, 0xFFFFFFFFu,
               "클릭: 메인 메뉴", 1.0f, 26);
    }
    return complete;
}

} // namespace hs
