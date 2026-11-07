#pragma once

#if defined(HS_DEVELOPMENT_TOOLS)
#include <hs/render_data/material_schema.hpp>
#include <hs/render_data/render_snapshot_values.hpp>
#include <hs/presentation/projector.hpp>
#include <hs/renderer/renderer.hpp>
#include "vfx_typed_frame_adapter.hpp"
#include "vfx_ribbon_frame_bounds.hpp"
#include "vfx_typed_screen_commands.hpp"
#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <string>
#include <vector>

namespace hs::runtime_detail
{
// Room entries retain the actual renderer identities; the scene owns the spans
// only until the current render call returns.
struct AssetTestRoom
{
    enum class Group : std::uint8_t { Animation, Model, Vfx };
    struct Entry
    {
        RenderMesh mesh{};
        std::uint32_t variant{};
        CharacterAnimationClip clip{};
        AssetId named_clip{};
        std::string name;
        float duration{1};
        std::uint64_t effect_id{};
        std::uint32_t effect_handle{};
        std::uint32_t payload_kind{};
        std::optional<PersistentVfxKind> owner_kind;
        bool legacy{};
        Float3 scale{1, 1, 1};
        Float3 position{};
        float yaw{};
        std::uint32_t color{0xffffffffu};
        Group group{Group::Animation};
        float capture_distance{8};
        bool fixed_pose{};
        bool screen_effect{};
    };
    std::vector<Entry> entries;
    std::vector<RenderInstance> floors;
    std::vector<RenderInstance> instances;
    std::vector<AnimationPoseRef> poses;
    std::vector<RenderInstance> previous_instances;
    std::vector<AnimationPoseRef> previous_poses;
    std::vector<PersistentVfxVisual> persistent, previous_persistent;
    std::vector<CapturedRenderInstance> captures;
    std::vector<std::size_t> station_instances;
    std::vector<std::uint64_t> last_event_cycles;
    std::vector<std::uint64_t> last_legacy_cycles;
    std::vector<std::uint64_t> prepared_cycles;
    std::vector<Tick> station_starts;
    std::optional<std::size_t> active_screen_station;

    explicit AssetTestRoom(const std::vector<RendererCharacterAnimationClip> &clips,
        const std::vector<RendererLegacyCharacterAnimationClip> &legacy_clips, const VfxImage &vfx);

    static std::uint64_t Cycle(double time, float duration) noexcept
    { return static_cast<std::uint64_t>(std::max(0.0, time) / std::max(1.0, double(duration))); }
    static Sequence EventSequence(std::size_t index, std::uint64_t cycle) noexcept
    { return 0x7000000000000000ull | ((std::uint64_t(index + 1) & 0xfffffull) << 32) | (cycle & 0xffffffffull); }
    static std::uint64_t OwnerId(std::size_t index, std::uint64_t cycle) noexcept
    { return 0x6000000000000000ull | ((std::uint64_t(index + 1) & 0xfffffull) << 32) | ((cycle + 1) & 0xffffffffull); }

    // The caller supplies the current/previous real gameplay snapshots.  Their
    // player instance and pose remain the camera/input owner in this gallery.
    void Prepare(double room_time, Tick current_tick, RenderSnapshotPair &pair)
    {
        previous_instances.swap(instances); previous_poses.swap(poses);
        previous_persistent.swap(persistent); instances.clear(); poses.clear(); persistent.clear(); captures.clear();
        for (std::size_t i = 0; i < pair.current.instances.size(); ++i)
            if (pair.current.instances[i].mesh == RenderMesh::Archer)
            {
                instances.push_back(pair.current.instances[i]);
                for (const auto &source : pair.current.poses)
                    if (source.instance_index == i) { auto pose = source; pose.instance_index = 0; poses.push_back(pose); }
                break;
            }
        instances.insert(instances.end(), floors.begin(), floors.end());
        for (std::size_t index = 0; index < entries.size(); ++index)
        {
            const auto &entry = entries[index];
            RenderInstance actor{}; actor.mesh = entry.mesh; actor.environment_variant = entry.variant;
            actor.position = entry.position; actor.scale = entry.scale; actor.yaw = entry.yaw; actor.color_rgba = entry.color;
            actor.environment_seed = 41703;
            actor.stable_id = entry.group == Group::Vfx
                ? (2ull << 60) | (0x100000000ull + index + 1)
                : 0x5400000000000000ull | (index + 1);
            actor.animation_clip_id = entry.named_clip;
            actor.animation_time_seconds = entry.fixed_pose ? 0.0 : std::fmod(std::max(0.0, room_time), double(entry.duration));
            PopulateRenderInstanceRevisions(actor);
            station_instances[index] = instances.size(); instances.push_back(actor);
            AnimationPoseRef pose{}; bool has_pose = false;
            if (!entry.named_clip.value && (actor.mesh == RenderMesh::Archer ||
                (actor.mesh >= RenderMesh::MonsterMelee && actor.mesh <= RenderMesh::BossFinal)))
            {
                pose.instance_index = static_cast<std::uint32_t>(station_instances[index]); pose.clip = entry.clip;
                pose.normalized_time = entry.fixed_pose ? 0.0f : static_cast<float>(std::fmod(std::max(0.0, room_time), double(entry.duration)) / entry.duration);
                pose.revision = DeriveAnimationPoseRevision(pose); poses.push_back(pose); has_pose = true;
            }
            if (!entry.effect_id || entry.legacy || !current_tick) continue;
            const auto cycle = Cycle(room_time, entry.duration);
            if (prepared_cycles[index] != cycle)
            { prepared_cycles[index] = cycle; station_starts[index] = current_tick; }
            const auto start = station_starts[index];
            const auto owner_id = OwnerId(index, cycle);
            if (entry.payload_kind == VfxRibbonBoundsHash32("SourceTargetPayload"))
            {
                RenderInstance target{}; target.mesh = RenderMesh::GroundDetail; target.environment_variant = 9;
                target.position = {entry.position.x + 3, entry.position.y, entry.position.z};
                target.stable_id = actor.stable_id ^ 0x10000000ull;
                PopulateRenderInstanceRevisions(target); instances.push_back(target);
            }
            if (entry.payload_kind == VfxRibbonBoundsHash32("ProjectilePayload") ||
                entry.payload_kind == VfxRibbonBoundsHash32("ProjectilePathPayload"))
            {
                RenderInstance projectile{}; projectile.mesh = RenderMesh::EnemyProjectile;
                projectile.position = {entry.position.x, entry.position.y + .5f,
                    entry.position.z + static_cast<float>(std::fmod(std::max(0.0, room_time), 2.0))};
                projectile.scale = {.2f, .2f, .2f}; projectile.stable_id = 0x5500000000000000ull | (index + 1);
                PopulateRenderInstanceRevisions(projectile); instances.push_back(projectile);
            }
            // Persistent inputs have a real room-owned row so the existing
            // burst retirement logic can retire the preceding station cycle.
            PersistentVfxVisual visual{};
            visual.kind = entry.owner_kind.value_or(static_cast<PersistentVfxKind>(0xff));
            visual.stable_id = owner_id; visual.entity_render_id = actor.stable_id;
            visual.status_episode_generation = start; visual.active_tick = start;
            visual.expires = start + std::max<Tick>(1, static_cast<Tick>(std::ceil(std::max(1.0f, entry.duration) * 60.0f)));
            visual.effect_asset = AssetId{entry.effect_id}; visual.position = actor.position; visual.radius = 2; visual.length = 3;
            visual.ring_inner_radius = 1; visual.ring_outer_radius = 2; visual.gap_count = 1;
            visual.gap_half_angle_degrees = 20; visual.cone_half_angle_degrees = 30;
            persistent.push_back(visual);
            if (entry.mesh == RenderMesh::BossFinal && has_pose)
                captures.push_back({pair.current.header.session_id, actor.stable_id & ((1ull << 60) - 1),
                    EventSequence(index, cycle), start, actor, pose});
        }
        pair.current.instances = instances; pair.current.poses = poses;
        pair.current.persistent_vfx = persistent; pair.current.captured_instances = captures;
        if (pair.has_previous)
        {
            pair.previous.instances = previous_instances.empty() ? std::span<const RenderInstance>(instances) : std::span<const RenderInstance>(previous_instances);
            pair.previous.poses = previous_instances.empty() ? std::span<const AnimationPoseRef>(poses) : std::span<const AnimationPoseRef>(previous_poses);
            pair.previous.persistent_vfx = previous_instances.empty() ? std::span<const PersistentVfxVisual>(persistent) : std::span<const PersistentVfxVisual>(previous_persistent);
        }
    }

    void AppendLegacyEvents(const VfxImage &, double time, Tick tick, std::uint64_t session,
        std::vector<PresentationEvent> &events)
    {
        if (!tick) return;
        for (std::size_t index = 0; index < entries.size(); ++index)
        {
            const auto &entry = entries[index]; if (!entry.legacy) continue;
            const auto cycle = Cycle(time, entry.duration); if (last_legacy_cycles[index] == cycle) continue;
            last_legacy_cycles[index] = cycle;
            PresentationEvent event{}; event.kind = PresentationKind::Vfx; event.asset = AssetId{entry.effect_id};
            event.tick = tick; event.session_id = session; event.sequence = EventSequence(index, cycle);
            event.position = entry.position; event.parameters = EncodeVfxParameters({{0,0,1}, 1,
                {entry.position.x + 3, entry.position.y, entry.position.z}, static_cast<std::uint32_t>(VfxEventFlag::HasTarget)});
            events.push_back(event);
        }
    }

    void AppendInputs(const VfxImage &image, double time, Tick tick, VfxQuality quality, VfxTypedFrameInputs &inputs,
        Float3 observer, std::optional<std::size_t> capture_station)
    {
        active_screen_station.reset();
        if (!tick) return;
        const auto hash = VfxRibbonBoundsHash32;
        for (std::size_t index = 0; index < entries.size(); ++index)
        {
            const auto &entry = entries[index];
            if (!entry.effect_handle || entry.effect_handle > image.program.effects.size() || station_instances[index] >= instances.size()) continue;
            if (entry.screen_effect)
            {
                const double dx = double(observer.x) - entry.position.x;
                const double dy = double(observer.y) - entry.position.y;
                const double dz = double(observer.z) - entry.position.z;
                if (capture_station ? *capture_station != index : dx * dx + dy * dy + dz * dz > 16.0) continue;
                active_screen_station = index;
            }
            const auto &effect = image.program.effects[entry.effect_handle - 1];
            if (effect.handle != entry.effect_handle || effect.input_mode > 1) continue;
            const auto cycle = Cycle(time, entry.duration); const auto start = station_starts[index];
            const auto &actor = instances[station_instances[index]]; const auto point = actor.position;
            const Float3 target{point.x + 3, point.y, point.z};
            const auto kind = effect.payload_kind;
            const bool moving = kind == hash("ProjectilePayload") || kind == hash("ProjectilePathPayload");
            // Prepare placed each projectile immediately after its station actor.
            const auto projectile = moving ? instances[station_instances[index] + 1].position : point;
            const Float3 prior = moving
                ? Float3{projectile.x, projectile.y, projectile.z - std::min(projectile.z - point.z, 1.0f / 60.0f)}
                : point;
            const float duration = std::max(1.0f, entry.duration);
            const float elapsed = static_cast<float>(tick >= start ? tick - start : 0) / 60.0f;
            const float age = std::clamp(elapsed / duration, 0.0f, 1.0f);
            const bool status_tick = effect.input_mode == 0 &&
                (entry.owner_kind == PersistentVfxKind::EnemyBleedStatus || entry.owner_kind == PersistentVfxKind::EnemyBurnStatus);
            if (status_tick)
            {
                const auto status_id = MakeAssetId(entry.owner_kind == PersistentVfxKind::EnemyBleedStatus
                    ? "persistent.status.bleed" : "persistent.status.burn");
                const auto lookup = std::ranges::find(image.program.effect_lookup, status_id.value, &VfxEffectLookupRecord::effect_id);
                if (lookup != image.program.effect_lookup.end())
                {
                    VfxPersistentInput status{}; status.effect_handle = lookup->handle; status.stable_id = OwnerId(index, cycle);
                    status.status_episode_generation = start; status.source_visual_kind = static_cast<std::uint8_t>(*entry.owner_kind);
                    status.quality = quality; status.elapsed_seconds = elapsed; status.elapsed_ticks = tick >= start ? double(tick - start) : 0;
                    status.normalized_age = age; status.source_duration_seconds = duration;
                    status.current_transform[12] = status.previous_transform[12] = point.x;
                    status.current_transform[13] = status.previous_transform[13] = point.y;
                    status.current_transform[14] = status.previous_transform[14] = point.z;
                    status.payload = VfxEntityPayload{1, age, .2f, 0, actor.stable_id};
                    inputs.persistent.push_back(std::move(status));
                }
            }
            if (effect.input_mode == 0 && last_event_cycles[index] == cycle) continue;
            VfxPayload payload;
            if (kind == hash("PointEventPayload")) payload = VfxPointPayload{point, {0,1,0}, {0,0,1}, 1, 1, 0};
            else if (kind == hash("PresentationContextPayload")) payload = VfxContextPayload{actor.stable_id & ((1ull << 60) - 1), {0,0,1}, entry.screen_effect ? .2f : age, 1, 0};
            else if (kind == hash("ProjectilePayload")) payload = VfxProjectilePayload{{0,0,1}, {.2f,.2f,.2f}, .2f, age, 0};
            else if (kind == hash("ProjectilePathPayload")) payload = VfxProjectilePathPayload{projectile, prior, {0,0,1}, point, .2f, age, 0};
            else if (kind == hash("CircleAreaPayload")) payload = VfxCirclePayload{point, {0,0,1}, 2, 0, age, 0};
            else if (kind == hash("ConePayload")) payload = VfxConePayload{point, {0,0,1}, 4, 30, age, 0};
            else if (kind == hash("LineAreaPayload")) payload = VfxLinePayload{point, target, {1,0,0}, 1, age, 0};
            else if (kind == hash("RingWithGapsPayload"))
            { VfxRingGapsPayload ring{}; ring.center = point; ring.inner_radius = 1; ring.outer_radius = 2;
              ring.gap_half_width_degrees = 20; ring.lifetime01 = age; ring.uniform_gaps.count = 1; payload = std::move(ring); }
            else if (kind == hash("SafeGapSectorPayload")) payload = VfxSafeSectorPayload{point, {0,0,1}, 1, 2, 20, age};
            else if (kind == hash("EntityAttachmentPayload")) payload = VfxEntityPayload{1, age, .2f, 0, actor.stable_id};
            else if (kind == hash("SourceTargetPayload")) payload = VfxLinkPayload{point, target, actor.stable_id, actor.stable_id ^ 0x10000000ull, age, .5f, 0};
            else if (kind == hash("GenericGameplayGeometryPayload")) payload = VfxGeometryPayload{{0,0,1}, 2, 1, 4, 30, age, 0};
            else continue;
            const auto seed = static_cast<std::uint32_t>(entry.effect_id ^ (entry.effect_id >> 32) ^ index);
            if (effect.input_mode == 0)
            {
                last_event_cycles[index] = cycle;
                VfxEventInput event{}; event.effect_handle = entry.effect_handle; event.event_tick = start;
                event.sequence = EventSequence(index, cycle); event.status_episode_generation = start;
                if (status_tick) event.geometry_owner_id = actor.stable_id & ((1ull << 60) - 1);
                if (entry.owner_kind && !status_tick)
                {
                    switch (*entry.owner_kind)
                    {
                    case PersistentVfxKind::BossDashWarning: case PersistentVfxKind::BossVolleyWarning:
                    case PersistentVfxKind::RangedEnemyWarning: case PersistentVfxKind::BossShockwaveWarning:
                    case PersistentVfxKind::MiniBombWarning: case PersistentVfxKind::EnemySpawnWarning:
                    case PersistentVfxKind::BossSpawnWarning: case PersistentVfxKind::BossAreaWarning:
                    case PersistentVfxKind::SuicideEnemyWarning:
                        event.geometry_owner_id = OwnerId(index, cycle); event.geometry_effect_id = entry.effect_id;
                        event.geometry_start_tick = start;
                        event.geometry_end_tick = start + std::max<Tick>(1, static_cast<Tick>(std::ceil(duration * 60.0f)));
                        break;
                    default: break;
                    }
                }
                event.stable_seed = seed; event.quality = quality; event.payload = std::move(payload);
                event.world_transform[12] = point.x; event.world_transform[13] = point.y; event.world_transform[14] = point.z;
                inputs.events.push_back(std::move(event));
            }
            else
            {
                VfxPersistentInput owner{}; owner.effect_handle = entry.effect_handle; owner.stable_id = OwnerId(index, cycle);
                owner.status_episode_generation = start; owner.stable_seed = seed; owner.quality = quality;
                owner.source_visual_kind = entry.owner_kind ? static_cast<std::uint8_t>(*entry.owner_kind) : 0xff;
                owner.elapsed_seconds = elapsed; owner.elapsed_ticks = tick >= start ? double(tick - start) : 0;
                owner.normalized_age = age; owner.source_duration_seconds = duration; owner.payload = std::move(payload);
                const auto &current = moving ? projectile : point; const auto &previous = moving ? prior : point;
                owner.current_transform[12] = current.x; owner.current_transform[13] = current.y; owner.current_transform[14] = current.z;
                owner.previous_transform[12] = previous.x; owner.previous_transform[13] = previous.y; owner.previous_transform[14] = previous.z;
                inputs.persistent.push_back(std::move(owner));
            }
        }
    }
};
} // namespace hs::runtime_detail
#endif
