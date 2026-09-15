#include "renderer_impl.hpp"
#include <bit>

#include <numbers>
#include <ranges>
#include <unordered_map>

namespace hs
{

Result D3D12Renderer::Render(const RenderSnapshotExchange::ReadPair &snapshots,
                             std::span<const PresentationEvent> events,
                             std::span<const ParticleSpawnCommand> particle_spawns,
                             std::span<const VfxLineSpawnCommand> effect_lines,
                             const DevToolsFrameData &devtools,
                             RendererFrameResult &frame_result)
{
    static_cast<void>(effect_lines);
    if (!impl_->initialized)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Renderer not initialized.");
    }

#if defined(HS_DEVELOPMENT_TOOLS)
    if (std::chrono::steady_clock::now() >= impl_->next_shader_check)
    {
        impl_->next_shader_check = std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(250);
        const auto write = LatestShaderWrite();
        if (write != std::filesystem::file_time_type{} && write != impl_->shader_write)
        {
            impl_->shader_write = write;
            const auto reloaded = impl_->ReloadPipeline();
            std::ofstream(impl_->config.artifact_directory / "shader_hot_reload.log",
                          std::ios::app)
                << (reloaded ? "applied\n"
                             : std::format("rejected {}; previous PSO retained\n",
                                           reloaded.Message()));
        }
    }
#endif

    const auto back_buffer_index = impl_->swap_chain->GetCurrentBackBufferIndex();
    auto &frame = impl_->frames[back_buffer_index];
    if (auto result = impl_->WaitForFrame(frame); !result)
    {
        return result;
    }

    auto snapshot = snapshots.has_current ? snapshots.current : RenderSnapshot{};
    std::unordered_map<std::uint64_t, const RenderInstance *> previous_by_id;
    previous_by_id.reserve(snapshots.has_previous ? snapshots.previous.instances.size() : 0);
    if (snapshots.has_previous)
        for (const auto &instance : snapshots.previous.instances)
            if (instance.stable_id != 0) previous_by_id.emplace(instance.stable_id, &instance);
    std::unordered_map<std::uint64_t, const AnimationPoseRef *> pose_by_id;
    pose_by_id.reserve(snapshot.poses.size());
    for (const auto &pose : snapshot.poses)
        if (pose.instance_index < snapshot.instances.size())
        {
            const auto stable_id = snapshot.instances[pose.instance_index].stable_id;
            if (stable_id != 0) pose_by_id.emplace(stable_id, &pose);
        }
    const auto now = std::chrono::steady_clock::now();
    if (snapshot.header.tick != impl_->observed_snapshot_tick)
    {
        impl_->observed_snapshot_tick = snapshot.header.tick;
        impl_->snapshot_arrival = now;
    }
    const auto interpolation =
        impl_->config.interpolate && snapshots.has_previous
            ? std::clamp(
                  std::chrono::duration<float>(now - impl_->snapshot_arrival).count() * 60.0f,
                  0.0f, 1.0f)
            : 1.0f;
    std::vector<RenderInstance> render_instances(snapshot.instances.begin(),
                                                  snapshot.instances.end());
    if (const auto count = impl_->config.slime_family_preview_count; count != 0)
    {
        render_instances.clear();
        const auto columns = count == 1 ? 3u : static_cast<std::uint32_t>(std::ceil(std::sqrt(count * 3.0f)));
        for (std::uint32_t i = 0; i < count * 3; ++i)
        {
            const auto row = i / columns, column = i % columns;
            const auto x = count == 1 ? (static_cast<float>(i) - 1.0f) * 2.5f
                                      : (static_cast<float>(column) - (columns - 1) * 0.5f) * 1.1f;
            const auto rows = (count * 3 + columns - 1) / columns;
            const auto z = count == 1 ? 0.0f : (static_cast<float>(row) - (rows - 1) * 0.5f) * 1.1f;
            const auto kind = i % 3;
            const auto scale = kind == 0 ? 1.5f : kind == 1 ? 2.0f : 2.4f;
            render_instances.push_back({{x, 0.0f, z}, 0.0f, {scale, scale, scale},
                                         0xffffffffu, static_cast<RenderMesh>(10 + kind), 0, 0});
            if (kind == 2)
                render_instances.push_back({{x - 1.25f, 0.45f, z - 1.5f}, 0.0f, {.24f, .24f, .8f},
                                             0xff60d060u, RenderMesh::EnemyProjectile, 0, 0});
        }
    }
    const auto is_monster = [](RenderMesh mesh) {
        return mesh >= RenderMesh::MonsterMelee && mesh <= RenderMesh::BossFinal;
    };
    if (impl_->config.monster_preview_asset < 6 && impl_->config.slime_family_preview_count == 0)
    {
        const auto preview_mesh = static_cast<RenderMesh>(
            static_cast<std::uint32_t>(RenderMesh::MonsterMelee) +
            impl_->config.monster_preview_asset);
        const auto found = std::ranges::find_if(
            render_instances, [&](const auto &source) { return source.mesh == preview_mesh; });
        if (found != render_instances.end())
        {
            auto preview = *found;
            preview.position = {};
            preview.yaw = 0.0f;
            render_instances.assign(1, preview);
        }
        else
        {
            render_instances.clear();
        }
    }
    if (impl_->config.environment_preview)
    {
        render_instances.clear();
        const auto add = [&](RenderMesh mesh, Float3 position, Float3 scale, std::uint32_t variant) {
            RenderInstance instance{};
            instance.mesh = mesh;
            instance.position = position;
            instance.scale = scale;
            instance.environment_variant = variant;
            instance.environment_seed = 0x4853454eu;
            render_instances.push_back(instance);
        };
        for (std::uint32_t variant = 0; variant < 3; ++variant)
        {
            const float x = static_cast<float>(variant)*5.0f-5.0f;
            add(RenderMesh::TreeTrunk, {x,0,0}, {1,1,1}, variant);
            add(RenderMesh::TreeCanopy, {x,0,0}, {1,1,1}, variant);
        }
        for (std::uint32_t variant = 0; variant < 4; ++variant)
        {
            const float x = static_cast<float>(variant)*3.0f-4.5f;
            add(RenderMesh::Rock, {x,0,-5}, {.8f,.8f,.8f}, variant);
            add(RenderMesh::Grass, {x,0,-8}, {1,1,1}, variant);
        }
        for (int z=-5; z<=2; ++z)
            for (int x=-5; x<=5; ++x)
                add(RenderMesh::Ground, {static_cast<float>(x)*2.5f,-.055f,static_cast<float>(z)*2.5f},
                    {2.5f,.1f,2.5f}, 0);
        snapshot.camera.target = {0,1,0};
        if (!impl_->config.preview_camera_override)
        {
            snapshot.camera.yaw_degrees = 0.0f;
            snapshot.camera.pitch_degrees = 22.0f;
            snapshot.camera.distance = 22.0f;
        }
    }
    const auto family_preview = impl_->config.slime_family_preview_count != 0;
    const auto monster_preview = impl_->config.monster_preview_asset < 6 || family_preview;
    const auto target_height = monster_preview
                                   ? (!family_preview && impl_->config.monster_preview_asset >= 3 ? 3.0f : 1.0f)
                               : impl_->config.character_preview ? 1.0f
                                                                : snapshot.camera.target.y;
    const auto target = DirectX::XMVectorSet(family_preview ? 0.0f : snapshot.camera.target.x, target_height,
                                             family_preview ? 0.0f : snapshot.camera.target.z, 1.0f);
#if defined(HS_DEVELOPMENT_TOOLS)
    const auto camera_yaw = impl_->config.preview_camera_override
                                ? impl_->config.preview_camera_yaw
                            : impl_->config.character_preview ? impl_->preview_yaw
                                                              : snapshot.camera.yaw_degrees;
    const auto camera_pitch = impl_->config.preview_camera_override
                                  ? impl_->config.preview_camera_pitch
                              : impl_->config.character_preview ? impl_->preview_pitch
                                                                : snapshot.camera.pitch_degrees;
    const auto camera_distance = impl_->config.preview_camera_override
                                     ? impl_->config.preview_camera_distance
                                 : family_preview ? std::max(10.0f, std::sqrt(impl_->config.slime_family_preview_count * 3.0f) * 1.9f)
                                 : monster_preview
                                     ? (impl_->config.monster_preview_asset >= 3 ? 18.0f : 8.0f)
                                 : impl_->config.character_preview ? impl_->preview_distance
                                     : snapshot.camera.distance;
#else
    // Command-line captures also run in the validation build without the Debug UI.
    const auto camera_yaw = impl_->config.preview_camera_override
                                ? impl_->config.preview_camera_yaw
                                : snapshot.camera.yaw_degrees;
    const auto camera_pitch = impl_->config.preview_camera_override
                                  ? impl_->config.preview_camera_pitch
                                  : snapshot.camera.pitch_degrees;
    const auto camera_distance = impl_->config.preview_camera_override
                                     ? impl_->config.preview_camera_distance
                                     : family_preview ? std::max(10.0f, std::sqrt(impl_->config.slime_family_preview_count * 3.0f) * 1.9f)
                                     : snapshot.camera.distance;
#endif
    const auto yaw = DirectX::XMConvertToRadians(camera_yaw);
    const auto pitch = DirectX::XMConvertToRadians(camera_pitch);
    const auto direction = DirectX::XMVector3Normalize(
        DirectX::XMVectorSet(std::cos(pitch) * std::sin(yaw), -std::sin(pitch),
                             std::cos(pitch) * std::cos(yaw), 0.0f));
    const auto eye = DirectX::XMVectorSubtract(
        target, DirectX::XMVectorScale(direction, camera_distance));
    DirectX::XMFLOAT3 lod_eye{};
    DirectX::XMStoreFloat3(&lod_eye, eye);
    const auto is_environment_mesh = [](RenderMesh mesh) {
        return mesh >= RenderMesh::TreeTrunk && mesh <= RenderMesh::Grass;
    };
    const auto environment_mesh_index = [](const RenderInstance &source) -> std::size_t {
        const auto type = static_cast<std::size_t>(source.mesh) - static_cast<std::size_t>(RenderMesh::TreeTrunk);
        constexpr std::array<std::size_t, 4> offsets{0, 3, 6, 10};
        constexpr std::array<std::size_t, 4> counts{3, 3, 4, 4};
        return offsets[type] + source.environment_variant % counts[type];
    };
    std::vector<RenderInstance> ordered_instances;
    ordered_instances.reserve(render_instances.size());
    for (const auto &source : render_instances)
        if (source.mesh == RenderMesh::Archer) ordered_instances.push_back(source);
    for (std::uint32_t asset = 0; asset < 6; ++asset)
        for (const auto &source : render_instances)
            if (is_monster(source.mesh) &&
                static_cast<std::uint32_t>(source.mesh) -
                 static_cast<std::uint32_t>(RenderMesh::MonsterMelee) == asset)
                 ordered_instances.push_back(source);
    for (const auto &source : render_instances)
        if (source.mesh == RenderMesh::EnemyProjectile) ordered_instances.push_back(source);
    struct EnvironmentLod { float threshold{1.0f}; bool complement{}; };
    std::vector<EnvironmentLod> ordered_lods(ordered_instances.size());
    std::array<std::pair<std::size_t, std::size_t>, 42> environment_ranges{};
    const auto select_lod = [&](const RenderInstance &source, std::size_t lod, EnvironmentLod &selection) {
        const float dx = source.position.x - lod_eye.x;
        const float dy = source.position.y - lod_eye.y;
        const float dz = source.position.z - lod_eye.z;
        const float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
        const std::size_t low = distance < 32.0f ? 0 : distance < 65.0f ? 1 : 2;
        const float start = low == 0 ? 25.0f : 55.0f;
        const float end = low == 0 ? 32.0f : 65.0f;
        const bool transition = low < 2 && distance > start;
        if (lod == low)
        {
            selection = {transition ? 1.0f-(distance-start)/(end-start) : 1.0f, false};
            return true;
        }
        if (transition && lod == low+1)
        {
            selection = {1.0f-(distance-start)/(end-start), true};
            return true;
        }
        return false;
    };
    for (std::size_t asset = 0; asset < environment_ranges.size(); ++asset)
    {
        const auto begin = ordered_instances.size();
        for (const auto &source : render_instances)
        {
            EnvironmentLod selection;
            if (is_environment_mesh(source.mesh) && environment_mesh_index(source) == asset/3 &&
                select_lod(source, asset%3, selection))
            {
                ordered_instances.push_back(source);
                ordered_lods.push_back(selection);
            }
        }
        environment_ranges[asset] = {begin, ordered_instances.size() - begin};
    }
    for (const auto &source : render_instances)
        if (source.mesh != RenderMesh::Archer && !is_monster(source.mesh) &&
            !is_environment_mesh(source.mesh) && source.mesh != RenderMesh::EnemyProjectile)
        {
            ordered_instances.push_back(source);
            ordered_lods.push_back({});
        }
    render_instances = std::move(ordered_instances);
    std::array<std::pair<std::size_t, std::size_t>, 6> monster_ranges{};
    std::pair<std::size_t, std::size_t> projectile_range{};
    for (std::size_t asset = 0; asset < monster_ranges.size(); ++asset)
    {
        const auto first = std::ranges::find_if(render_instances, [&](const auto &source) {
            return is_monster(source.mesh) &&
                   static_cast<std::size_t>(static_cast<std::uint32_t>(source.mesh) -
                                             static_cast<std::uint32_t>(RenderMesh::MonsterMelee)) == asset;
        });
        if (first == render_instances.end()) continue;
        const auto begin = static_cast<std::size_t>(first - render_instances.begin());
        auto last = first;
        while (last != render_instances.end() &&
               is_monster(last->mesh) &&
               static_cast<std::size_t>(static_cast<std::uint32_t>(last->mesh) -
                                         static_cast<std::uint32_t>(RenderMesh::MonsterMelee)) == asset)
            ++last;
        monster_ranges[asset] = {begin, static_cast<std::size_t>(last - first)};
    }
    if (const auto first = std::ranges::find_if(render_instances, [](const auto &source) {
            return source.mesh == RenderMesh::EnemyProjectile;
        }); first != render_instances.end())
    {
        auto last = first;
        while (last != render_instances.end() && last->mesh == RenderMesh::EnemyProjectile) ++last;
        projectile_range = {static_cast<std::size_t>(first - render_instances.begin()),
                            static_cast<std::size_t>(last - first)};
    }
    std::vector<ParticleSpawnCommand> frame_particle_spawns(particle_spawns.begin(),
                                                             particle_spawns.end());
    if (impl_->config.slime_family_preview_count != 0) frame_particle_spawns.clear();
    if (snapshot.header.tick != impl_->last_status_visual_tick)
    {
        impl_->last_status_visual_tick = snapshot.header.tick;
        for (const auto &source : snapshot.instances)
        {
            const auto add_status = [&](StatusVisual status,
                                        const ParticleSpriteBinding &binding,
                                        ParticleFacing facing, VfxRenderer renderer,
                                        VfxPrimitive primitive, float height,
                                        float size, Float4 color, std::uint32_t count) {
                if ((source.status_visual_mask & static_cast<std::uint32_t>(status)) == 0)
                    return;
                const auto cadence = status == StatusVisual::Bleed ? 6u
                                   : status == StatusVisual::Burn ? 3u
                                                                  : 1u;
                if ((snapshot.header.tick + source.stable_id) % cadence != 0) return;
                ParticleSpawnCommand command;
                command.sequence = source.stable_id ^ snapshot.header.tick ^
                                   static_cast<std::uint32_t>(status);
                command.tick = snapshot.header.tick;
                command.position = {source.position.x, source.position.y + height,
                                    source.position.z};
                command.shape = ParticleShape::Point;
                command.velocity_mode = ParticleVelocity::Direction;
                command.facing = facing;
                command.renderer = renderer;
                command.primitive = primitive;
                command.sprite = binding.sprite;
                command.frame_columns = binding.frame_columns;
                command.frame_rows = binding.frame_rows;
                command.direction = {0.0f, 1.0f, 0.0f};
                command.lifetime_min = command.lifetime_max = 2.0f / 60.0f;
                command.start_color = command.end_color = color;
                command.start_size_min = command.start_size_max = size;
                command.end_size_min = command.end_size_max = size;
                command.count = count;
                command.seed = static_cast<std::uint32_t>(command.sequence);
                if (status == StatusVisual::Bleed)
                {
                    command.position.y = source.position.y + source.scale.y * 0.85f;
                    command.shape = ParticleShape::Sphere;
                    command.shape_extent = {source.scale.x * 0.35f,
                                            source.scale.y * 0.35f,
                                            source.scale.z * 0.35f};
                    command.direction = {0.0f, -1.0f, 0.0f};
                    command.speed_min = 0.25f;
                    command.speed_max = 0.6f;
                    command.lifetime_min = 0.35f;
                    command.lifetime_max = 0.5f;
                    command.end_color.w = 0.0f;
                    command.end_size_min = command.end_size_max = size * 0.45f;
                    command.gravity = -3.0f;
                    command.stretch = 2.0f;
                }
                else if (status == StatusVisual::Burn)
                {
                    command.position.y = source.position.y + source.scale.y * 0.2f;
                    command.shape = ParticleShape::Sphere;
                    command.velocity_mode = ParticleVelocity::Upward;
                    command.shape_extent = {source.scale.x * 0.4f,
                                            source.scale.y * 0.35f,
                                            source.scale.z * 0.4f};
                    command.speed_min = 0.3f;
                    command.speed_max = 0.9f;
                    command.lifetime_min = 0.28f;
                    command.lifetime_max = 0.45f;
                    command.end_color.w = 0.0f;
                    command.end_size_min = size * 0.35f;
                    command.end_size_max = size * 0.6f;
                    command.gravity = -0.4f;
                }
                frame_particle_spawns.push_back(command);
            };
            add_status(StatusVisual::Bleed, impl_->config.bleed_status_sprite,
                       ParticleFacing::Velocity, VfxRenderer::Sprite,
                       VfxPrimitive::Shard, source.scale.y * 0.55f,
                       source.scale.y * 0.16f, {1.8f, 0.04f, 0.05f, 0.42f}, 1);
            add_status(StatusVisual::Burn, impl_->config.burn_status_sprite,
                       ParticleFacing::Camera, VfxRenderer::Sprite,
                       VfxPrimitive::Flame, source.scale.y * 0.5f,
                       source.scale.y * 0.18f, {2.2f, 0.7f, 0.08f, 0.38f}, 1);
            add_status(StatusVisual::Slow, impl_->config.slow_status_sprite,
                       ParticleFacing::Ground, VfxRenderer::Ground,
                       VfxPrimitive::Rune, 0.025f, source.scale.x * 0.72f,
                       {0.25f, 0.85f, 1.8f, 0.28f}, 1);
            add_status(StatusVisual::Mark, impl_->config.mark_status_sprite,
                       ParticleFacing::Ground, VfxRenderer::Ground,
                       VfxPrimitive::Rune, 0.025f,
                       source.scale.x * 0.42f, {2.1f, 1.0f, 0.15f, 0.26f}, 1);
        }
        for (const auto &visual : snapshot.persistent_vfx)
        {
            auto position = visual.position;
            auto visual_yaw = visual.yaw;
            if (interpolation < 1.0f)
            {
                const auto previous = std::ranges::find_if(
                    snapshots.previous.persistent_vfx,
                    [&visual](const PersistentVfxVisual &candidate) {
                        return candidate.stable_id == visual.stable_id &&
                               candidate.kind == visual.kind;
                    });
                if (previous != snapshots.previous.persistent_vfx.end())
                {
                    position.x = std::lerp(previous->position.x, position.x, interpolation);
                    position.y = std::lerp(previous->position.y, position.y, interpolation);
                    position.z = std::lerp(previous->position.z, position.z, interpolation);
                    const auto yaw_delta = std::remainder(visual_yaw - previous->yaw,
                                                          2.0f * std::numbers::pi_v<float>);
                    visual_yaw = previous->yaw + yaw_delta * interpolation;
                }
            }
            ParticleSpawnCommand command;
            command.sequence = visual.stable_id ^ snapshot.header.tick;
            command.tick = snapshot.header.tick;
            command.position = position;
            command.shape = ParticleShape::Point;
            command.velocity_mode = ParticleVelocity::Direction;
            command.facing = ParticleFacing::Ground;
            command.renderer = VfxRenderer::Ground;
            command.direction = {0.0f, 1.0f, 0.0f};
            command.lifetime_min = command.lifetime_max = 2.0f / 60.0f;
            command.start_size_min = command.start_size_max = visual.radius;
            command.end_size_min = command.end_size_max = visual.radius;
            command.rotation_min = command.rotation_max = visual_yaw;
            switch (visual.kind)
            {
            case PersistentVfxKind::TrapPending:
                command.primitive = VfxPrimitive::Cracks;
                command.start_color = command.end_color = {0.45f, 0.8f, 1.3f, 0.18f};
                break;
            case PersistentVfxKind::TrapArmed:
                command.primitive = VfxPrimitive::Rune;
                command.start_color = command.end_color = {1.55f, 0.8f, 0.16f, 0.24f};
                break;
            case PersistentVfxKind::FireArea:
                command.primitive = VfxPrimitive::Ring;
                command.start_color = command.end_color = {3.2f, 0.62f, 0.035f, 0.34f};
                break;
            case PersistentVfxKind::SlowArea:
                command.primitive = VfxPrimitive::Rune;
                command.start_color = command.end_color = {0.2f, 0.75f, 1.65f, 0.16f};
                break;
            case PersistentVfxKind::ArrowRainArea:
                command.primitive = VfxPrimitive::Ring;
                command.start_color = command.end_color = {1.55f, 1.05f, 0.28f, 0.18f};
                break;
            case PersistentVfxKind::RangeIndicator:
                command.primitive = VfxPrimitive::Ring;
                command.start_color = command.end_color = {0.2f, 1.45f, 0.45f, 0.3f};
                break;
            case PersistentVfxKind::DamageTrail:
            case PersistentVfxKind::ChargeGuide:
            case PersistentVfxKind::ProjectileTrail:
            case PersistentVfxKind::ProjectileTrailOuter:
            case PersistentVfxKind::RicochetProjectileTrail:
            {
                command.renderer = VfxRenderer::Segment;
                command.primitive = visual.kind == PersistentVfxKind::ProjectileTrail
                                        ? VfxPrimitive::DashWake
                                        : VfxPrimitive::SolidTrail;
                const auto direction = Float3{std::sin(visual_yaw), 0.0f,
                                              std::cos(visual_yaw)};
                command.direction = direction;
                // Segment orientation is carried through the particle's initial velocity.
                command.speed_min = command.speed_max = 0.001f;
                command.rotation_min = command.rotation_max = 0.0f;
                command.start_size_min = command.start_size_max = visual.radius;
                command.end_size_min = command.end_size_max = visual.radius;
                command.stretch = visual.length / std::max(visual.radius * 2.0f, 0.001f);
                if (visual.kind == PersistentVfxKind::ChargeGuide)
                    command.start_color = command.end_color =
                        {0.25f, 1.2f, 2.4f, 0.28f};
                else if (visual.kind == PersistentVfxKind::ProjectileTrail)
                {
                    command.start_color = {0.65f, 2.2f, 3.6f, 0.58f};
                    command.end_color = {0.1f, 0.45f, 1.2f, 0.0f};
                }
                else if (visual.kind == PersistentVfxKind::ProjectileTrailOuter)
                {
                    command.start_color = {0.3f, 1.1f, 2.1f, 0.18f};
                    command.end_color = {0.08f, 0.25f, 0.7f, 0.0f};
                }
                else if (visual.kind == PersistentVfxKind::RicochetProjectileTrail)
                {
                    command.start_color = {1.4f, 0.5f, 0.08f, 0.36f};
                    command.end_color = {0.35f, 0.1f, 0.02f, 0.0f};
                }
                else
                    command.start_color = command.end_color =
                        {0.35f, 1.0f, 1.8f, 0.16f};
                break;
            }
            }
            if (visual.kind == PersistentVfxKind::ChargeGuide)
            {
                constexpr auto border_radius = 0.035f;
                const auto direction = Float3{std::sin(visual_yaw), 0.0f,
                                              std::cos(visual_yaw)};
                const auto right = Float3{direction.z, 0.0f, -direction.x};
                const auto border_offset = std::max(visual.radius - border_radius, 0.0f);
                command.primitive = VfxPrimitive::SolidTrail;
                command.start_size_min = command.start_size_max = border_radius;
                command.end_size_min = command.end_size_max = border_radius;
                command.stretch = visual.length / (border_radius * 2.0f);
                command.start_color = command.end_color = {0.25f, 1.2f, 2.4f, 0.28f};
                for (const auto side : {-1.0f, 1.0f})
                {
                    auto border = command;
                    border.sequence ^= side < 0.0f ? 0xD1B54A32D192ED03ull
                                                   : 0x94D049BB133111EBull;
                    border.seed = static_cast<std::uint32_t>(border.sequence);
                    border.position.x += right.x * border_offset * side;
                    border.position.z += right.z * border_offset * side;
                    border.count = 1;
                    frame_particle_spawns.push_back(border);
                }
                const auto cap_length = visual.radius * 2.0f;
                for (const auto side : {-1.0f, 1.0f})
                {
                    auto cap = command;
                    cap.sequence ^= side < 0.0f ? 0x4CF5AD432745937Full
                                                : 0x8A5CD789635D2DFFull;
                    cap.seed = static_cast<std::uint32_t>(cap.sequence);
                    cap.position.x += direction.x * visual.length * 0.5f * side;
                    cap.position.z += direction.z * visual.length * 0.5f * side;
                    cap.direction = right;
                    cap.rotation_min = cap.rotation_max =
                        visual_yaw + std::numbers::pi_v<float> * 0.5f;
                    cap.stretch = cap_length / (border_radius * 2.0f);
                    cap.count = 1;
                    frame_particle_spawns.push_back(cap);
                }
                continue;
            }
            if (visual.kind == PersistentVfxKind::DamageTrail)
            {
                const auto direction = Float3{std::sin(visual_yaw), 0.0f,
                                              std::cos(visual_yaw)};
                const auto right = Float3{direction.z, 0.0f, -direction.x};
                constexpr auto border_radius = 0.035f;
                const auto border_offset = std::max(visual.radius - border_radius, 0.0f);
                command.start_size_min = command.start_size_max = border_radius;
                command.end_size_min = command.end_size_max = border_radius;
                command.stretch = visual.length / (border_radius * 2.0f);
                command.start_color = command.end_color = {0.35f, 1.0f, 1.8f, 0.24f};
                for (const auto side : {-1.0f, 1.0f})
                {
                    auto border = command;
                    border.sequence ^= side < 0.0f ? 0xD1B54A32D192ED03ull
                                                   : 0x94D049BB133111EBull;
                    border.seed = static_cast<std::uint32_t>(border.sequence);
                    border.position.x += right.x * border_offset * side;
                    border.position.z += right.z * border_offset * side;
                    border.count = 1;
                    frame_particle_spawns.push_back(border);
                }
                auto streak = command;
                streak.sequence ^= 0x9E3779B97F4A7C15ull;
                streak.seed = static_cast<std::uint32_t>(streak.sequence);
                streak.primitive = VfxPrimitive::DashedRicochet;
                streak.start_size_min = streak.start_size_max = 0.025f;
                streak.end_size_min = streak.end_size_max = 0.025f;
                streak.stretch = visual.length / 0.05f;
                streak.start_color = streak.end_color = {0.2f, 0.75f, 1.5f, 0.10f};
                streak.count = 1;
                frame_particle_spawns.push_back(streak);
                continue;
            }
            if (visual.kind == PersistentVfxKind::SlowArea)
            {
                command.count = 1;
                command.seed = static_cast<std::uint32_t>(command.sequence);
                frame_particle_spawns.push_back(command);
                constexpr auto notch_count = 4;
                for (auto index = 0; index < notch_count; ++index)
                {
                    const auto angle = static_cast<float>(index) *
                                       (2.0f * std::numbers::pi_v<float> / notch_count);
                    auto notch = command;
                    notch.sequence ^= 0x9E3779B97F4A7C15ull +
                                      static_cast<std::uint64_t>(index);
                    notch.seed = static_cast<std::uint32_t>(notch.sequence);
                    notch.primitive = VfxPrimitive::Chevron;
                    notch.position.x += std::sin(angle) * visual.radius * 0.52f;
                    notch.position.z += std::cos(angle) * visual.radius * 0.52f;
                    notch.rotation_min = notch.rotation_max = angle + std::numbers::pi_v<float>;
                    notch.start_size_min = notch.start_size_max = visual.radius * 0.12f;
                    notch.end_size_min = notch.end_size_max = visual.radius * 0.12f;
                    notch.count = 1;
                    frame_particle_spawns.push_back(notch);
                }
                continue;
            }
            command.count = 1;
            command.seed = static_cast<std::uint32_t>(command.sequence);
            frame_particle_spawns.push_back(command);
            if (visual.kind == PersistentVfxKind::ArrowRainArea)
            {
                constexpr std::array marks{
                    Float2{-0.34f, -0.18f}, Float2{0.22f, -0.30f},
                    Float2{0.0f, 0.0f}, Float2{-0.18f, 0.32f},
                    Float2{0.36f, 0.20f}};
                for (std::size_t index = 0; index < marks.size(); ++index)
                {
                    auto mark = command;
                    mark.sequence ^= 0x9E3779B97F4A7C15ull + index;
                    mark.seed = static_cast<std::uint32_t>(mark.sequence);
                    mark.position.x += marks[index].x * visual.radius;
                    mark.position.z += marks[index].y * visual.radius;
                    mark.primitive = VfxPrimitive::Arrow;
                    mark.start_size_min = mark.start_size_max = visual.radius * 0.11f;
                    mark.end_size_min = mark.end_size_max = visual.radius * 0.11f;
                    mark.start_color = mark.end_color = {1.3f, 0.85f, 0.2f, 0.09f};
                    frame_particle_spawns.push_back(mark);
                }
                continue;
            }
            if (visual.kind == PersistentVfxKind::RangeIndicator)
            {
                constexpr auto mark_count = 6;
                for (auto index = 0; index < mark_count; ++index)
                {
                    const auto angle = static_cast<float>(index) *
                                       (2.0f * std::numbers::pi_v<float> / mark_count);
                    auto mark = command;
                    mark.sequence ^= 0xD1B54A32D192ED03ull +
                                     static_cast<std::uint64_t>(index);
                    mark.seed = static_cast<std::uint32_t>(mark.sequence);
                    mark.position.x += std::sin(angle) * visual.radius * 0.58f;
                    mark.position.z += std::cos(angle) * visual.radius * 0.58f;
                    mark.rotation_min = mark.rotation_max = angle + std::numbers::pi_v<float>;
                    mark.primitive = VfxPrimitive::Chevron;
                    mark.start_size_min = mark.start_size_max = visual.radius * 0.10f;
                    mark.end_size_min = mark.end_size_max = visual.radius * 0.10f;
                    mark.start_color = mark.end_color = {0.18f, 0.8f, 0.35f, 0.10f};
                    frame_particle_spawns.push_back(mark);
                }
                continue;
            }
            if (visual.kind == PersistentVfxKind::FireArea)
            {
                auto interior = command;
                interior.sequence ^= 0x9E3779B97F4A7C15ull;
                interior.seed = static_cast<std::uint32_t>(interior.sequence);
                interior.start_size_min = interior.start_size_max = visual.radius * 0.78f;
                interior.end_size_min = interior.end_size_max = visual.radius * 0.78f;
                interior.primitive = VfxPrimitive::Flame;
                interior.start_color = interior.end_color =
                    {2.2f, 0.42f, 0.025f, 0.11f};
                frame_particle_spawns.push_back(interior);
            }
        }
    }
    for (const auto &line : effect_lines)
    {
        const auto dx = line.end.x - line.start.x;
        const auto dz = line.end.z - line.start.z;
        const auto length = std::hypot(dx, dz);
        if (length <= 0.0001f) continue;
        ParticleSpawnCommand command;
        command.sequence = line.sequence;
        command.tick = line.tick;
        command.position = {(line.start.x + line.end.x) * 0.5f, 0.035f,
                            (line.start.z + line.end.z) * 0.5f};
        command.shape = ParticleShape::Line;
        command.velocity_mode = ParticleVelocity::Direction;
        command.facing = ParticleFacing::Ground;
        command.renderer = VfxRenderer::Segment;
        command.primitive = line.primitive;
        command.sprite = line.sprite;
        command.frame_columns = line.frame_columns;
        command.frame_rows = line.frame_rows;
        command.shape_extent = {length * 0.5f, 0.0f, 0.0f};
        command.direction = {dx / length, 0.0f, dz / length};
        command.speed_min = command.speed_max = 0.001f;
        command.lifetime_min = command.lifetime_max = line.lifetime;
        command.start_color = command.end_color = line.color;
        command.start_size_min = command.start_size_max = line.width * 2.5f;
        command.end_size_min = command.end_size_max = line.width * 2.5f;
        command.stretch = length / std::max(line.width * 5.0f, 0.001f);
        command.rotation_min = command.rotation_max = 0.0f;
        command.uv_repeat = line.uv_repeat;
        command.scroll_speed = line.scroll_speed;
        command.count = 1;
        command.seed = static_cast<std::uint32_t>(line.sequence);
        frame_particle_spawns.push_back(command);
    }
    const auto particle_spawn_data_offset =
        (kInstanceDataOffset + sizeof(GpuInstance) * render_instances.size() + 255u) &
        ~std::size_t{255u};
    const auto particle_owner_data_offset =
        (particle_spawn_data_offset + sizeof(GpuParticleSpawnCommand) *
             std::max<std::size_t>(frame_particle_spawns.size(), 1) + 255u) & ~std::size_t{255u};
    const auto required_upload_size = particle_owner_data_offset +
        sizeof(std::uint32_t) * kParticleCount;
    if (required_upload_size > frame.upload_size)
    {
        std::size_t new_size = frame.upload_size;
        while (new_size < required_upload_size) new_size *= 2;
        frame.upload.resource->Unmap(0, nullptr);
        frame.mapped = nullptr;
        frame.upload.Reset();
        D3D12MA::ALLOCATION_DESC upload_allocation{};
        upload_allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        if (auto result = impl_->CreateAllocation(
                frame.upload, upload_allocation, BufferDescription(new_size),
                D3D12_RESOURCE_STATE_GENERIC_READ);
            !result)
        {
            return result;
        }
        D3D12_RANGE no_read{};
        const auto mapped = frame.upload.resource->Map(
            0, &no_read, reinterpret_cast<void **>(&frame.mapped));
        if (FAILED(mapped)) return HResultFailure("Map grown frame upload", mapped);
        frame.upload_size = new_size;
    }
    std::uint8_t debug_command{};
    std::uint64_t debug_value{};
    std::uint32_t debug_secondary{};
#if defined(HS_DEVELOPMENT_TOOLS)
    if (impl_->config.devtools_visible)
    {
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2(430.0f, 720.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Project HS DevTools");
    ImGui::Text("Tick: %llu", static_cast<unsigned long long>(snapshot.header.tick));
    ImGui::Text("Checksum: %llu",
                static_cast<unsigned long long>(snapshot.header.checksum));
    ImGui::Text("Instances: %zu", snapshot.instances.size());
    ImGui::Text("Render entities: %zu", snapshot.instances.size());
    ImGui::Text("UI models: %zu", snapshot.ui.size());
    ImGui::Text("GPU particles: %u / %u", impl_->last_particle_count,
                kParticleCount * std::clamp(impl_->config.particle_percentage, 50u, 100u) /
                    100u);
    D3D12MA::Budget local_budget{};
    impl_->allocator->GetBudget(&local_budget, nullptr);
    ImGui::Text("Video memory: %.1f / %.1f MiB",
                static_cast<double>(local_budget.UsageBytes) / (1024.0 * 1024.0),
                static_cast<double>(local_budget.BudgetBytes) / (1024.0 * 1024.0));
    ImGui::Text("Threads: Main / Simulation / Render + %u workers",
                devtools.worker_count);
    ImGui::Text("Queues I/P/V/G/D: %u/%u/%u/%u/%u",
                devtools.input_queue_depth, devtools.presentation_queue_depth,
                devtools.particle_queue_depth, devtools.resize_queue_depth,
                devtools.debug_queue_depth);
    ImGui::Text("Dropped I/P/V: %llu/%llu/%llu",
                static_cast<unsigned long long>(devtools.dropped_input),
                static_cast<unsigned long long>(devtools.dropped_presentation),
                static_cast<unsigned long long>(devtools.dropped_particles));
    ImGui::SeparatorText("Character animation inspection");
    ImGui::Checkbox("Close-up preview", &impl_->config.character_preview);
    if (impl_->config.character_preview)
    {
        ImGui::SliderFloat("Camera distance", &impl_->preview_distance, 2.5f, 8.0f);
        ImGui::SliderFloat("Camera yaw", &impl_->preview_yaw, 0.0f, 360.0f);
        ImGui::SliderFloat("Camera pitch", &impl_->preview_pitch, -20.0f, 60.0f);
        ImGui::Checkbox("Override pose", &impl_->preview_pose_override);
        if (impl_->preview_pose_override)
        {
            constexpr const char *clips = "Idle\0Run\0Draw\0Recoil\0Death\0";
            ImGui::Combo("Base clip", &impl_->preview_base_clip, clips);
            ImGui::SliderFloat("Base time", &impl_->preview_base_time, 0.0f, 1.0f);
            ImGui::Combo("Blend clip", &impl_->preview_secondary_clip, clips);
            ImGui::SliderFloat("Blend time", &impl_->preview_secondary_time, 0.0f, 1.0f);
            ImGui::SliderFloat("Blend weight", &impl_->preview_secondary_weight, 0.0f, 1.0f);
            ImGui::Combo("Upper clip", &impl_->preview_upper_clip, clips);
            ImGui::SliderFloat("Upper time", &impl_->preview_upper_time, 0.0f, 1.0f);
            ImGui::SliderFloat("Upper weight", &impl_->preview_upper_weight, 0.0f, 1.0f);
        }
    }
    ImGui::SeparatorText("Simulation");
    if (ImGui::Button("Start Session")) debug_command = 1;
    if (ImGui::Button("Pause / Resume")) debug_command = 15;
    if (ImGui::Button("Grant 100 XP")) { debug_command = 6; debug_value = 100; }
    if (ImGui::Button("Damage Player 10")) { debug_command = 3; debug_value = 10; }
    ImGui::SameLine();
    if (ImGui::Button("Heal Player 10")) { debug_command = 4; debug_value = 10; }
    if (ImGui::Button("Damage Final Boss 1000")) { debug_command = 5; debug_value = 1000; }
    if (ImGui::Button("Spawn Melee")) { debug_command = 10; debug_value = 0; }
    ImGui::SameLine();
    if (ImGui::Button("Spawn Ranged")) { debug_command = 10; debug_value = 1; }
    ImGui::SameLine();
    if (ImGui::Button("Spawn Suicide")) { debug_command = 10; debug_value = 2; }
    if (ImGui::Button("Growth 5m")) { debug_command = 2; debug_value = 18'000; }
    ImGui::SameLine();
    if (ImGui::Button("Growth 10m")) { debug_command = 2; debug_value = 36'000; }
    ImGui::SameLine();
    if (ImGui::Button("Growth 15m")) { debug_command = 2; debug_value = 54'000; }
    if (ImGui::Button("Spawn 5m Boss")) { debug_command = 11; debug_value = 0; }
    ImGui::SameLine();
    if (ImGui::Button("Spawn 10m Boss")) { debug_command = 11; debug_value = 1; }
    ImGui::SameLine();
    if (ImGui::Button("Spawn Final Boss")) { debug_command = 11; debug_value = 2; }

    static int skill{};
    static int upgrade{};
    static int relic{};
    static int stat{};
    ImGui::SeparatorText("Build controls");
    ImGui::Combo("Skill", &skill, kDebugSkillNames.data(),
                 static_cast<int>(kDebugSkillNames.size()));
    if (ImGui::Button("Grant Skill")) { debug_command = 7; debug_value = skill + 1; }
    ImGui::Combo("Upgrade", &upgrade, kDebugUpgradeNames[skill].data(),
                 static_cast<int>(kDebugUpgradeNames[skill].size()));
    if (ImGui::Button("Grant Upgrade")) {
        debug_command = 8;
        debug_value = skill + 1;
        debug_secondary = static_cast<std::uint32_t>(upgrade);
    }
    ImGui::Combo("Relic", &relic, kDebugRelicNames.data(),
                 static_cast<int>(kDebugRelicNames.size()));
    if (ImGui::Button("Grant Relic")) { debug_command = 9; debug_value = relic; }
    ImGui::Combo("Stat", &stat, kDebugStatNames.data(),
                 static_cast<int>(kDebugStatNames.size()));
    if (ImGui::Button("Assign Stat")) { debug_command = 13; debug_value = stat; }
    ImGui::SameLine();
    if (ImGui::Button("Reroll")) debug_command = 14;
    ImGui::SeparatorText("RenderGraph");
    for (const auto pass : kRenderPassNames)
        ImGui::BulletText("%.*s", static_cast<int>(pass.size()), pass.data());
    ImGui::End();
    ImGui::Render();
    }
#endif
    if (auto result = impl_->RasterizeUi(back_buffer_index, snapshot.ui); !result)
    {
        return result;
    }
    const auto instance_count = render_instances.size();

    auto *constants = reinterpret_cast<FrameConstants *>(frame.mapped);
    auto *instances = reinterpret_cast<GpuInstance *>(frame.mapped + kInstanceDataOffset);
    auto *gpu_particle_spawns =
        reinterpret_cast<GpuParticleSpawnCommand *>(frame.mapped + particle_spawn_data_offset);
    auto *gpu_particle_owners =
        reinterpret_cast<std::uint32_t *>(frame.mapped + particle_owner_data_offset);
    constexpr auto particle_capacity = kParticleCount;
    std::uint32_t gpu_particle_spawn_count{};
    std::uint32_t total_particles_to_spawn{};
    for (const auto &source : frame_particle_spawns)
    {
        if (total_particles_to_spawn == particle_capacity)
        {
            break;
        }
        const auto age_ticks =
            snapshot.header.tick > source.tick ? snapshot.header.tick - source.tick : 0;
        const auto age = static_cast<float>(age_ticks) / 60.0f;
        if (age >= source.lifetime_max || source.count == 0)
        {
            continue;
        }
        const auto count = std::min(source.count, particle_capacity - total_particles_to_spawn);
        auto &target_spawn = gpu_particle_spawns[gpu_particle_spawn_count++];
        target_spawn.position_lifetime_min = {source.position.x, source.position.y,
                                              source.position.z, source.lifetime_min};
        target_spawn.direction_lifetime_max = {source.direction.x, source.direction.y,
                                               source.direction.z, source.lifetime_max};
        target_spawn.shape_extent_speed_min = {source.shape_extent.x, source.shape_extent.y,
                                               source.shape_extent.z, source.speed_min};
        target_spawn.speed_cone_gravity_stretch = {source.speed_max, source.cone_radians,
                                                   source.gravity, source.stretch};
        target_spawn.start_color = {source.start_color.x, source.start_color.y,
                                    source.start_color.z, source.start_color.w};
        target_spawn.end_color = {source.end_color.x, source.end_color.y,
                                  source.end_color.z, source.end_color.w};
        target_spawn.size_range = {source.start_size_min, source.start_size_max,
                                   source.end_size_min, source.end_size_max};
        target_spawn.rotation_range = source.renderer == VfxRenderer::Segment
            ? DirectX::XMFLOAT4{source.uv_repeat, source.uv_repeat,
                                source.scroll_speed, source.scroll_speed}
            : DirectX::XMFLOAT4{source.rotation_min, source.rotation_max,
                                source.angular_velocity_min,
                                source.angular_velocity_max};
        const auto sprite_metadata = static_cast<std::uint32_t>(source.sprite) |
                                     (static_cast<std::uint32_t>(source.frame_columns) << 16u) |
                                     (static_cast<std::uint32_t>(source.frame_rows) << 24u);
        const auto visual_metadata = static_cast<std::uint32_t>(source.facing) |
                                     (static_cast<std::uint32_t>(source.renderer) << 8u) |
                                     (static_cast<std::uint32_t>(source.primitive) << 16u);
        target_spawn.modes = {static_cast<std::uint32_t>(source.shape),
                              static_cast<std::uint32_t>(source.velocity_mode),
                              visual_metadata,
                              sprite_metadata};
        target_spawn.metadata = {count, source.seed, total_particles_to_spawn,
                                 std::bit_cast<std::uint32_t>(age)};
        std::fill_n(gpu_particle_owners + total_particles_to_spawn, count,
                    gpu_particle_spawn_count - 1);
        total_particles_to_spawn += count;
    }
    const auto particle_delta_ticks =
        impl_->particles_initialized && snapshot.header.tick > impl_->last_particle_tick
            ? snapshot.header.tick - impl_->last_particle_tick
            : 0;
    const auto view =
        DirectX::XMMatrixLookAtLH(eye, target, DirectX::XMVectorSet(0, 1, 0, 0));
    const auto projection = DirectX::XMMatrixPerspectiveFovLH(
        DirectX::XMConvertToRadians(snapshot.camera.vertical_fov_degrees),
        static_cast<float>(impl_->render_width) / static_cast<float>(impl_->render_height), 500.0f,
        0.1f);
    DirectX::XMStoreFloat4x4(&constants->view_projection,
                             DirectX::XMMatrixTranspose(view * projection));
    if (impl_->frame_number == 0)
    {
        DirectX::XMFLOAT3 projected_origin{};
        DirectX::XMFLOAT3 projected_forward{};
        DirectX::XMStoreFloat3(
            &projected_origin,
            DirectX::XMVector3TransformCoord(DirectX::XMVectorZero(), view * projection));
        DirectX::XMStoreFloat3(
            &projected_forward,
            DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(0, 0, 20, 1),
                                             view * projection));
        std::ofstream camera_log(impl_->config.artifact_directory / "camera.json",
                                 std::ios::trunc);
        camera_log << std::format(
            "{{\"origin\":[{},{},{}],\"forward\":[{},{},{}],\"yaw\":{},\"pitch\":{}}}\n",
            projected_origin.x, projected_origin.y, projected_origin.z, projected_forward.x,
            projected_forward.y, projected_forward.z, camera_yaw, camera_pitch);
    }
    DirectX::XMStoreFloat4(&constants->camera_time, eye);
    constants->camera_time.w =
        static_cast<float>(snapshot.header.simulation_time.count()) / 1'000'000'000.0f;
    const auto light = snapshot.lights.empty() ? LightView{{-0.4f, -0.8f, 0.3f}, 3.0f, {1, 1, 1}}
                                                : snapshot.lights.front();
    constants->light_direction_intensity = {light.direction.x, light.direction.y,
                                             light.direction.z, light.intensity};
    constants->light_color = {light.color.x, light.color.y, light.color.z, 1.0f};
    constants->screen_size = {
        static_cast<float>(impl_->render_width), static_cast<float>(impl_->render_height),
        1.0f / static_cast<float>(impl_->render_width),
        1.0f / static_cast<float>(impl_->render_height)};
    DirectX::XMFLOAT3 camera_forward;
    DirectX::XMStoreFloat3(&camera_forward, direction);
    constants->camera_forward_softness = {camera_forward.x, camera_forward.y,
                                          camera_forward.z, 0.35f};
    constexpr float cascade_extent[] = {18.0f, 48.0f, 160.0f};
    const auto base_shadow = std::clamp(impl_->config.shadow_resolution, 1024u, 2048u);
    constants->shadow_atlas_texel_size = {1.0f / float(base_shadow * 5u), 1.0f / float(base_shadow * 4u), float(base_shadow * 5u), float(base_shadow * 4u)};
    const auto light_direction = DirectX::XMVector3Normalize(DirectX::XMVectorSet(light.direction.x, light.direction.y, light.direction.z, 0.0f));
    const auto light_up = std::abs(DirectX::XMVectorGetY(light_direction)) > 0.99f
        ? DirectX::XMVectorSet(0, 0, 1, 0) : DirectX::XMVectorSet(0, 1, 0, 0);
    const auto light_view = DirectX::XMMatrixLookAtLH(
        DirectX::XMVectorScale(light_direction, -160.0f), DirectX::XMVectorZero(), light_up);
    for (std::size_t cascade = 0; cascade < 3; ++cascade)
    {
        const float resolution = cascade == 0 ? float(base_shadow * 4u) : float(base_shadow);
        const float extent = cascade_extent[cascade];
        const auto center = DirectX::XMVector3TransformCoord(target, light_view);
        const float texel = extent / resolution;
        const float cx = std::round(DirectX::XMVectorGetX(center) / texel) * texel;
        const float cy = std::round(DirectX::XMVectorGetY(center) / texel) * texel;
        const float cz = DirectX::XMVectorGetZ(center);
        const float half = extent * 0.5f;
        const auto projection = DirectX::XMMatrixOrthographicOffCenterLH(cx-half, cx+half, cy-half, cy+half, cz+160.0f, cz-160.0f);
        DirectX::XMStoreFloat4x4(&constants->shadow_view_projection[cascade], DirectX::XMMatrixTranspose(light_view * projection));
        constants->shadow_atlas_scale_offset[cascade] = cascade == 0 ? DirectX::XMFLOAT4{0.8f, 1.0f, 0.0f, 0.0f} : (cascade == 1 ? DirectX::XMFLOAT4{0.2f, 0.25f, 0.8f, 0.0f} : DirectX::XMFLOAT4{0.2f, 0.25f, 0.8f, 0.25f});
    }
    for (auto &bone : constants->archer_bones)
    {
        DirectX::XMStoreFloat4x4(
            &bone, DirectX::XMMatrixTranspose(DirectX::XMMatrixIdentity()));
    }
    for (std::size_t asset = 0; asset < impl_->monster_assets.size(); ++asset)
    {
        const auto &monster = impl_->monster_assets[asset];
        constants->monster_asset_meta[asset] = {monster.skin_offset, monster.bone_count, 0, 0};
        for (std::size_t clip = 0; clip < monster.clip_meta.size(); ++clip)
            constants->monster_clip_meta[asset][clip] = monster.clip_meta[clip];
    }
    auto pose = snapshot.poses.empty() ? AnimationPoseRef{} : snapshot.poses.front();
#if defined(HS_DEVELOPMENT_TOOLS)
    if (impl_->config.character_preview && impl_->preview_pose_override)
    {
        pose.clip = static_cast<CharacterAnimationClip>(impl_->preview_base_clip);
        pose.normalized_time = impl_->preview_base_time;
        pose.secondary_clip =
            static_cast<CharacterAnimationClip>(impl_->preview_secondary_clip);
        pose.secondary_normalized_time = impl_->preview_secondary_time;
        pose.secondary_weight = impl_->preview_secondary_weight;
        pose.upper_body_clip =
            static_cast<CharacterAnimationClip>(impl_->preview_upper_clip);
        pose.upper_body_normalized_time = impl_->preview_upper_time;
        pose.upper_body_weight = impl_->preview_upper_weight;
    }
#endif
    const auto eased_weight = [](float weight) {
        const auto clamped = std::clamp(weight, 0.0f, 1.0f);
        return clamped * clamped * (3.0f - 2.0f * clamped);
    };
    const auto blend_transform = [](const CharacterLocalTransform &first,
                                    const CharacterLocalTransform &second,
                                    float weight) {
        CharacterLocalTransform output;
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            output.translation[axis] = std::lerp(first.translation[axis],
                                                 second.translation[axis], weight);
            output.scale[axis] = std::lerp(first.scale[axis], second.scale[axis], weight);
        }
        auto first_rotation = DirectX::XMQuaternionNormalize(DirectX::XMVectorSet(
            first.rotation[0], first.rotation[1], first.rotation[2], first.rotation[3]));
        auto second_rotation = DirectX::XMQuaternionNormalize(DirectX::XMVectorSet(
            second.rotation[0], second.rotation[1], second.rotation[2], second.rotation[3]));
        if (DirectX::XMVectorGetX(
                DirectX::XMQuaternionDot(first_rotation, second_rotation)) < 0.0f)
        {
            second_rotation = DirectX::XMVectorNegate(second_rotation);
        }
        DirectX::XMFLOAT4 rotation;
        DirectX::XMStoreFloat4(
            &rotation, DirectX::XMQuaternionNormalize(DirectX::XMQuaternionSlerp(
                           first_rotation, second_rotation, weight)));
        output.rotation = {rotation.x, rotation.y, rotation.z, rotation.w};
        return output;
    };
    const auto sample = [&](CharacterAnimationClip clip, float time,
                            std::uint32_t bone_index,
                            CharacterLocalTransform &output) {
        const auto found = std::ranges::find(impl_->archer_clips, clip,
                                              &CharacterClipHeader::clip);
        if (found == impl_->archer_clips.end()) return false;
        const auto normalized = found->looping ? time - std::floor(time)
                                               : std::clamp(time, 0.0f, 1.0f);
        const auto frame_position = normalized * static_cast<float>(found->frame_count - 1);
        const auto first_frame = static_cast<std::uint32_t>(frame_position);
        const auto second_frame = std::min(first_frame + 1, found->frame_count - 1);
        const auto &first = impl_->archer_transforms[
            found->first_transform + first_frame * impl_->archer_bone_count + bone_index];
        const auto &second = impl_->archer_transforms[
            found->first_transform + second_frame * impl_->archer_bone_count + bone_index];
        output = blend_transform(first, second,
                                 frame_position - static_cast<float>(first_frame));
        return true;
    };
    std::array<DirectX::XMFLOAT4X4, kMaxCharacterBones> global_transforms{};
    for (std::uint32_t bone_index = 0; bone_index < impl_->archer_bone_count; ++bone_index)
    {
        CharacterLocalTransform base, secondary, upper;
        if (!sample(pose.clip, pose.normalized_time * std::max(0.0f, pose.playback_rate),
                    bone_index, base) ||
            !sample(pose.secondary_clip,
                    pose.secondary_normalized_time *
                        std::max(0.0f, pose.secondary_playback_rate),
                    bone_index, secondary))
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Cooked animation transform cannot be blended.");
        }
        base = blend_transform(base, secondary, eased_weight(pose.secondary_weight));
        const auto upper_weight = eased_weight(pose.upper_body_weight) *
                                  impl_->archer_upper_body_weights[bone_index];
        if (upper_weight > 0.0f &&
            (!sample(pose.upper_body_clip,
                     pose.upper_body_normalized_time *
                         std::max(0.0f, pose.upper_body_playback_rate),
                     bone_index, upper)))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Cooked upper-body animation cannot be blended.");
        if (upper_weight > 0.0f)
        {
            base = blend_transform(base, upper, upper_weight);
        }
        const auto local =
            DirectX::XMMatrixScaling(base.scale[0], base.scale[1], base.scale[2]) *
            DirectX::XMMatrixRotationQuaternion(DirectX::XMVectorSet(
                base.rotation[0], base.rotation[1], base.rotation[2], base.rotation[3])) *
            DirectX::XMMatrixTranslation(base.translation[0], base.translation[1],
                                         base.translation[2]);
        const auto parent = impl_->archer_parents[bone_index];
        const auto global = parent == std::numeric_limits<std::uint16_t>::max()
                                ? local
                                : local * DirectX::XMLoadFloat4x4(
                                              &global_transforms[parent]);
        DirectX::XMStoreFloat4x4(&global_transforms[bone_index], global);
        DirectX::XMFLOAT4X4 inverse_bind;
        std::memcpy(&inverse_bind,
                    impl_->archer_inverse_bind_matrices[bone_index].data(),
                    sizeof(inverse_bind));
        const auto skin = DirectX::XMMatrixTranspose(
                              DirectX::XMLoadFloat4x4(&inverse_bind)) *
                          global;
        const auto determinant = DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(skin));
        if (!std::isfinite(determinant) || std::abs(determinant) <= 0.0001f ||
            DirectX::XMMatrixIsNaN(skin) || DirectX::XMMatrixIsInfinite(skin))
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Blended animation produced an invalid skin pose.");
        }
        DirectX::XMStoreFloat4x4(&constants->archer_bones[bone_index],
                                 DirectX::XMMatrixTranspose(skin));
    }
    constants->render_options = {
        static_cast<float>(particle_capacity), impl_->config.bloom ? 1.0f : 0.0f,
        impl_->config.outline ? 1.0f : 0.0f,
        static_cast<float>(particle_delta_ticks) / 60.0f};
    constants->particle_options = {particle_capacity, gpu_particle_spawn_count,
                                   total_particles_to_spawn,
                                   impl_->archer_material_count};
    std::vector<std::pair<float, std::uint64_t>> grass_candidates;
    DirectX::XMFLOAT3 player_position{};
    bool have_player = false;
    for (const auto &source : render_instances)
    {
        if (source.mesh == RenderMesh::Archer)
        {
            player_position = {source.position.x, source.position.y, source.position.z};
            have_player = true;
            break;
        }
    }
    if (have_player)
    {
        for (const auto &source : render_instances)
        {
            if (source.mesh < RenderMesh::MonsterMelee || source.mesh > RenderMesh::BossFinal)
                continue;
            const auto dx = source.position.x - player_position.x;
            const auto dz = source.position.z - player_position.z;
            grass_candidates.emplace_back(dx * dx + dz * dz, source.stable_id);
        }
        std::ranges::sort(grass_candidates);
        constants->grass_benders[0] = {player_position.x, player_position.y,
                                       player_position.z, 1.25f};
        const auto count = std::min<std::size_t>(grass_candidates.size() + 1, 32);
        std::size_t output = 1;
        for (const auto &candidate : grass_candidates)
        {
            if (output == count) break;
            const auto it = std::ranges::find_if(render_instances, [&](const auto &source) {
                return source.stable_id == candidate.second;
            });
            if (it == render_instances.end()) continue;
            constants->grass_benders[output++] = {it->position.x, it->position.y,
                                                  it->position.z, 1.0f};
        }
        constants->grass_bender_count = {static_cast<std::uint32_t>(output), 0, 0, 0};
    }
    else
    {
        constants->grass_bender_count = {};
    }
    for (std::size_t index = 0; index < instance_count; ++index)
    {
        const auto &source = render_instances[index];
        auto position = source.position;
        auto yaw_value = source.yaw;
        if (snapshots.has_previous && source.stable_id != 0)
        {
            const auto previous_it = previous_by_id.find(source.stable_id);
            if (previous_it != previous_by_id.end())
            {
                const auto &previous = *previous_it->second;
                position.x = std::lerp(previous.position.x, source.position.x, interpolation);
                position.y = std::lerp(previous.position.y, source.position.y, interpolation);
                position.z = std::lerp(previous.position.z, source.position.z, interpolation);
                const auto yaw_delta = std::remainder(source.yaw - previous.yaw,
                                                      2.0f * DirectX::XM_PI);
                yaw_value = previous.yaw + yaw_delta * interpolation;
            }
        }
        const auto vertical_offset = is_environment_mesh(source.mesh) ? 0.0f : source.mesh == RenderMesh::Archer
                                         ? impl_->archer_ground_offset
                                         : is_monster(source.mesh)
                                             ? impl_->monster_assets[static_cast<std::size_t>(
                                                   static_cast<std::uint32_t>(source.mesh) -
                                                   static_cast<std::uint32_t>(RenderMesh::MonsterMelee))]
                                                   .ground_offset * source.scale.y
                                             : source.scale.y * 0.5f;
        auto mesh = static_cast<std::uint32_t>(source.mesh);
        if (is_environment_mesh(source.mesh))
            mesh |= 0x10000u | (ordered_lods[index].complement ? 0x20000u : 0u);
        float animation_time = source.mesh == RenderMesh::Ground || source.mesh == RenderMesh::DirtPatch
            ? std::bit_cast<float>(source.environment_seed) : 0.0f;
        if (is_monster(source.mesh))
        {
            if (impl_->config.monster_preview_asset < 6 || family_preview)
            {
                mesh |= impl_->config.monster_preview_clip << 8u;
                animation_time = impl_->config.monster_preview_time;
            }
            else if (const auto pose_it = pose_by_id.find(source.stable_id);
                     pose_it != pose_by_id.end())
            {
                mesh |= static_cast<std::uint32_t>(pose_it->second->clip) << 8u;
                animation_time = pose_it->second->normalized_time;
            }
        }
        instances[index] = {{position.x, position.y + vertical_offset, position.z, 1.0f},
                            {source.scale.x, source.scale.y, source.scale.z, ordered_lods[index].threshold},
                            source.color_rgba,
                            mesh,
                            yaw_value,
                            animation_time};
    }

    auto result = frame.allocator->Reset();
    if (FAILED(result))
    {
        return HResultFailure("Reset command allocator", result);
    }
    result = impl_->command_list->Reset(frame.allocator.Get(), nullptr);
    if (FAILED(result))
    {
        return HResultFailure("Reset command list", result);
    }

    auto &ui_texture = impl_->ui_textures[back_buffer_index];
    impl_->TransitionTexture(
        ui_texture.resource.Get(),
        frame.ui_initialized ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                             : D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_COPY_DEST,
        frame.ui_initialized ? D3D12_BARRIER_LAYOUT_SHADER_RESOURCE
                             : D3D12_BARRIER_LAYOUT_COMMON,
        D3D12_BARRIER_LAYOUT_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION ui_destination{};
    ui_destination.pResource = ui_texture.resource.Get();
    ui_destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION ui_source{};
    ui_source.pResource = frame.ui_upload.resource.Get();
    ui_source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    ui_source.PlacedFootprint = impl_->ui_footprint;
    impl_->command_list->CopyTextureRegion(&ui_destination, 0, 0, 0, &ui_source, nullptr);
    impl_->TransitionTexture(ui_texture.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_BARRIER_LAYOUT_COPY_DEST,
                             D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
    frame.ui_initialized = true;

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(impl_->render_width),
                                  static_cast<float>(impl_->render_height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(impl_->render_width),
                             static_cast<LONG>(impl_->render_height)};
    const D3D12_VIEWPORT output_viewport{0.0f, 0.0f, static_cast<float>(impl_->width),
                                         static_cast<float>(impl_->height), 0.0f, 1.0f};
    const D3D12_RECT output_scissor{0, 0, static_cast<LONG>(impl_->width),
                                    static_cast<LONG>(impl_->height)};
    impl_->command_list->RSSetViewports(1, &viewport);
    impl_->command_list->RSSetScissorRects(1, &scissor);
    impl_->command_list->SetGraphicsRootSignature(impl_->root_signature.Get());
    impl_->command_list->SetComputeRootSignature(impl_->root_signature.Get());
    impl_->command_list->SetGraphicsRootConstantBufferView(
        0, frame.upload.resource->GetGPUVirtualAddress());
    impl_->command_list->SetComputeRootConstantBufferView(
        0, frame.upload.resource->GetGPUVirtualAddress());

    const auto rtv_start = impl_->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    const auto rtv_at = [&](std::uint32_t index) {
        return D3D12_CPU_DESCRIPTOR_HANDLE{
            rtv_start.ptr + static_cast<SIZE_T>(index) * impl_->rtv_stride};
    };
    const auto rtv = rtv_at(back_buffer_index);
    const auto gbuffer_base_rtv = rtv_at(kFrameCount);
    const auto gbuffer_normal_rtv = rtv_at(kFrameCount + 1);
    const auto gbuffer_position_rtv = rtv_at(kFrameCount + 2);
    const auto hdr_rtv = rtv_at(kFrameCount + 3);
    const auto oit_accumulation_rtv = rtv_at(kFrameCount + 4);
    const auto oit_revealage_rtv = rtv_at(kFrameCount + 5);
    const auto post_a_rtv = rtv_at(kFrameCount + 6);
    const auto post_b_rtv = rtv_at(kFrameCount + 7);
    const auto gbuffer_material_rtv = rtv_at(kFrameCount + 8);
    const auto dsv = impl_->dsv_heap->GetCPUDescriptorHandleForHeapStart();
    ID3D12DescriptorHeap *descriptor_heaps[] = {impl_->srv_heap.Get()};
    impl_->command_list->SetDescriptorHeaps(1, descriptor_heaps);
    auto texture_table = impl_->srv_heap->GetGPUDescriptorHandleForHeapStart();
    texture_table.ptr += static_cast<UINT64>(back_buffer_index) *
                         kTextureDescriptorCount * impl_->srv_stride;
    auto character_table = texture_table;
    character_table.ptr += static_cast<UINT64>(kPostTextureDescriptorCount) *
                           impl_->srv_stride;
    impl_->command_list->SetGraphicsRootDescriptorTable(13, character_table);
    impl_->command_list->SetGraphicsRootShaderResourceView(
        15, impl_->monster_skin_matrices.resource->GetGPUVirtualAddress());
    const auto draw_fullscreen =
        [&](ID3D12PipelineState *pipeline, D3D12_CPU_DESCRIPTOR_HANDLE target) {
            impl_->command_list->OMSetRenderTargets(1, &target, FALSE, nullptr);
            impl_->command_list->SetPipelineState(pipeline);
            impl_->command_list->SetGraphicsRootDescriptorTable(5, texture_table);
            impl_->command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            impl_->command_list->DrawInstanced(3, 1, 0, 0);
        };

    impl_->graph.Reset();
    const auto particles = impl_->graph.ImportBuffer(
        {impl_->particles.resource.Get()}, Access::UnorderedWrite, "Particles");
    const auto particle_input_index = impl_->particle_input_is_a ? 0u : 1u;
    const auto particle_output_index = particle_input_index ^ 1u;
    const auto particle_alive_input = impl_->graph.ImportBuffer(
        {impl_->particle_alive[particle_input_index].resource.Get()}, Access::UnorderedWrite,
        "ParticleAliveInput");
    const auto particle_alive_output = impl_->graph.ImportBuffer(
        {impl_->particle_alive[particle_output_index].resource.Get()}, Access::UnorderedWrite,
        "ParticleAliveOutput");
    const auto particle_dead = impl_->graph.ImportBuffer(
        {impl_->particle_dead.resource.Get()}, Access::UnorderedWrite, "ParticleDead");
    const auto particle_counters = impl_->graph.ImportBuffer(
        {impl_->particle_counters.resource.Get()}, Access::UnorderedWrite, "ParticleCounters");
    const auto indirect_arguments = impl_->graph.ImportBuffer(
        {impl_->indirect_arguments.resource.Get()}, Access::UnorderedWrite, "IndirectArguments");
    const auto initial_depth_access =
        impl_->transient_textures_common ? Access::Common : Access::DepthWrite;
    const auto initial_color_access =
        impl_->transient_textures_common ? Access::Common : Access::ShaderRead;
    const auto shadow =
        impl_->graph.ImportTexture({impl_->shadow.resource.Get()}, initial_depth_access, "Shadow");
    const auto depth =
        impl_->graph.ImportTexture({impl_->depth.resource.Get()}, initial_depth_access, "Depth");
    const auto gbuffer_base = impl_->graph.ImportTexture(
        {impl_->gbuffer_base.resource.Get()}, initial_color_access, "GBufferBase");
    const auto gbuffer_normal = impl_->graph.ImportTexture(
        {impl_->gbuffer_normal.resource.Get()}, initial_color_access, "GBufferNormal");
    const auto gbuffer_position = impl_->graph.ImportTexture(
        {impl_->gbuffer_position.resource.Get()}, initial_color_access, "GBufferPosition");
    const auto hdr = impl_->graph.ImportTexture(
        {impl_->hdr_color.resource.Get()}, initial_color_access, "HdrColor");
    const auto oit_accumulation = impl_->graph.ImportTexture(
        {impl_->oit_accumulation.resource.Get()}, initial_color_access, "OitAccumulation");
    const auto oit_revealage = impl_->graph.ImportTexture(
        {impl_->oit_revealage.resource.Get()}, initial_color_access, "OitRevealage");
    const auto post_a = impl_->graph.ImportTexture(
        {impl_->post_a.resource.Get()}, initial_color_access, "PostA");
    const auto gbuffer_material = impl_->graph.ImportTexture(
        {impl_->gbuffer_material.resource.Get()}, initial_color_access, "GBufferMaterial");
    const auto post_b = impl_->graph.ImportTexture(
        {impl_->post_b.resource.Get()}, initial_color_access, "PostB");
    const auto ui = impl_->graph.ImportTexture(
        {ui_texture.resource.Get()}, Access::ShaderRead, "UiTexture");
    const auto back_buffer = impl_->graph.ImportTexture(
        {impl_->back_buffers[back_buffer_index].Get()}, Access::Present, "BackBuffer");

    auto particle_pass = impl_->graph.AddPass(kRenderPassNames[0], QueueHint::Direct);
    particle_pass.ReadWrite(particles, Access::UnorderedWrite);
    particle_pass.ReadWrite(particle_alive_input, Access::UnorderedWrite);
    particle_pass.ReadWrite(particle_alive_output, Access::UnorderedWrite);
    particle_pass.ReadWrite(particle_dead, Access::UnorderedWrite);
    particle_pass.ReadWrite(particle_counters, Access::UnorderedWrite);
    particle_pass.ReadWrite(indirect_arguments, Access::UnorderedWrite);
    const auto initialize_particles = !impl_->particles_initialized;
    particle_pass.SetExecute([&](RenderPassContext &context) {
        impl_->command_list->SetPipelineState(impl_->particle_compute_pipeline.Get());
        impl_->command_list->SetComputeRootUnorderedAccessView(
            3, impl_->particles.resource->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootUnorderedAccessView(
            4, impl_->indirect_arguments.resource->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootUnorderedAccessView(
            7, impl_->particle_alive[particle_input_index].resource->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootUnorderedAccessView(
            8, impl_->particle_alive[particle_output_index].resource->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootUnorderedAccessView(
            9, impl_->particle_dead.resource->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootUnorderedAccessView(
            10, impl_->particle_counters.resource->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootShaderResourceView(
            11, frame.upload.resource->GetGPUVirtualAddress() + particle_spawn_data_offset);
        impl_->command_list->SetComputeRootShaderResourceView(
            14, frame.upload.resource->GetGPUVirtualAddress() + particle_owner_data_offset);

        const auto dispatch_phase = [&](std::uint32_t phase, std::uint32_t item_count) {
            impl_->command_list->SetComputeRoot32BitConstant(6, phase, 0);
            impl_->command_list->Dispatch((std::max(item_count, 1u) + 255) / 256, 1, 1);
        };
        const auto synchronize_particle_state = [&] {
            context.UavBarrier(particles);
            context.UavBarrier(particle_alive_output);
            context.UavBarrier(particle_dead);
            context.UavBarrier(particle_counters);
        };

        if (initialize_particles)
        {
            dispatch_phase(0, particle_capacity);
            context.UavBarrier(particle_dead);
            context.UavBarrier(particle_counters);
            context.UavBarrier(indirect_arguments);
        }
        dispatch_phase(1, 1);
        context.UavBarrier(particle_counters);
        context.UavBarrier(indirect_arguments);
        dispatch_phase(2, particle_capacity);
        synchronize_particle_state();
        if (total_particles_to_spawn != 0)
        {
            dispatch_phase(3, total_particles_to_spawn);
            synchronize_particle_state();
        }
        dispatch_phase(4, 1);
        context.UavBarrier(particle_counters);
        context.UavBarrier(indirect_arguments);
    });

    const auto draw_character_instances = [&] {
        const auto base = frame.upload.resource->GetGPUVirtualAddress() + kInstanceDataOffset;
        if (instance_count != 0 && render_instances.front().mesh == RenderMesh::Archer)
        {
            impl_->command_list->SetGraphicsRootShaderResourceView(1, base);
            impl_->command_list->IASetVertexBuffers(0, 1, &impl_->archer_vertex_view);
            impl_->command_list->DrawInstanced(impl_->archer_vertex_count, 1, 0, 0);
        }
        for (std::size_t asset = 0; asset < monster_ranges.size(); ++asset)
        {
            const auto [begin, count] = monster_ranges[asset];
            if (count == 0) continue;
            impl_->command_list->SetGraphicsRootShaderResourceView(
                1, base + begin * sizeof(GpuInstance));
            impl_->command_list->IASetVertexBuffers(
                0, 1, &impl_->monster_assets[asset].vertex_view);
            impl_->command_list->DrawInstanced(impl_->monster_assets[asset].vertex_count,
                                               static_cast<UINT>(count), 0, 0);
        }
        for (std::size_t asset = 0; asset < environment_ranges.size(); ++asset)
        {
            const auto [begin, count] = environment_ranges[asset];
            if (count == 0) continue;
            const auto &mesh = impl_->environment_meshes[asset];
            impl_->command_list->SetGraphicsRootShaderResourceView(1, base + begin * sizeof(GpuInstance));
            impl_->command_list->IASetVertexBuffers(0, 1, &mesh.vertex_view);
            impl_->command_list->DrawInstanced(mesh.vertex_count, static_cast<UINT>(count), 0, 0);
        }
        const auto procedural_begin = std::ranges::find_if(
            render_instances, [&](const auto &source) {
                return source.mesh != RenderMesh::Archer && !is_environment_mesh(source.mesh) &&
                       !(source.mesh >= RenderMesh::MonsterMelee &&
                          source.mesh <= RenderMesh::BossFinal) &&
                       source.mesh != RenderMesh::EnemyProjectile;
            });
        if (procedural_begin != render_instances.end())
        {
            const auto begin = static_cast<std::size_t>(procedural_begin - render_instances.begin());
            impl_->command_list->SetGraphicsRootShaderResourceView(
                1, base + begin * sizeof(GpuInstance));
            impl_->command_list->IASetVertexBuffers(0, 1, &impl_->vertex_view);
            impl_->command_list->DrawInstanced(
                static_cast<UINT>(kCubeVertices.size()),
                static_cast<UINT>(render_instances.size() - begin), 0, 0);
        }
    };
    auto shadow_pass = impl_->graph.AddPass(kRenderPassNames[1], QueueHint::Direct);
    shadow_pass.Write(shadow, Access::DepthWrite);
    shadow_pass.SetExecute([&](RenderPassContext &) {
        const auto shadow_size =
            static_cast<float>(std::clamp(impl_->config.shadow_resolution, 1024u, 2048u));
        const D3D12_VIEWPORT shadow_viewport{0, 0, shadow_size, shadow_size, 0, 1};
        const D3D12_RECT shadow_scissor{0, 0, static_cast<LONG>(shadow_size),
                                        static_cast<LONG>(shadow_size)};
        impl_->command_list->RSSetViewports(1, &shadow_viewport);
        impl_->command_list->RSSetScissorRects(1, &shadow_scissor);
        impl_->command_list->SetPipelineState(impl_->shadow_pipeline.Get());
        impl_->command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        auto handle = impl_->dsv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += impl_->dsv_stride;
        impl_->command_list->ClearDepthStencilView(handle, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        for (std::uint32_t cascade = 0; cascade < 3; ++cascade)
        {
            impl_->command_list->OMSetRenderTargets(0, nullptr, FALSE, &handle);
            const UINT tile = cascade == 0 ? static_cast<UINT>(shadow_size * 4.0f) : static_cast<UINT>(shadow_size);
            const UINT x = cascade == 0 ? 0u : tile * 4u;
            const UINT y = cascade == 2 ? tile : 0u;
            const D3D12_VIEWPORT tile_view{static_cast<float>(x), static_cast<float>(y), static_cast<float>(tile), static_cast<float>(tile), 0, 1};
            const D3D12_RECT tile_rect{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + tile), static_cast<LONG>(y + tile)};
            impl_->command_list->RSSetViewports(1, &tile_view );
            impl_->command_list->RSSetScissorRects(1, &tile_rect);
            impl_->command_list->SetGraphicsRoot32BitConstant(6, cascade, 0);
            draw_character_instances();
            handle.ptr += impl_->dsv_stride;
        }
        impl_->command_list->RSSetViewports(1, &viewport);
        impl_->command_list->RSSetScissorRects(1, &scissor);
    });

    auto gbuffer_pass = impl_->graph.AddPass(kRenderPassNames[2], QueueHint::Direct);
    gbuffer_pass.Write(gbuffer_base, Access::RenderTarget);
    gbuffer_pass.Write(gbuffer_normal, Access::RenderTarget);
    gbuffer_pass.Write(gbuffer_position, Access::RenderTarget);
    gbuffer_pass.Write(gbuffer_material, Access::RenderTarget);
    gbuffer_pass.Write(depth, Access::DepthWrite);
    gbuffer_pass.SetExecute([&](RenderPassContext &) {
        constexpr float clear_base[] = {0, 0, 0, 0};
        constexpr float clear_normal[] = {0.5f, 1.0f, 0.5f, 0};
        constexpr float clear_position[] = {0, 0, 0, 0};
        impl_->command_list->ClearRenderTargetView(gbuffer_base_rtv, clear_base, 0, nullptr);
        impl_->command_list->ClearRenderTargetView(gbuffer_material_rtv, clear_base, 0, nullptr);
        impl_->command_list->ClearRenderTargetView(gbuffer_normal_rtv, clear_normal, 0, nullptr);
        impl_->command_list->ClearRenderTargetView(gbuffer_position_rtv, clear_position, 0,
                                                   nullptr);
        impl_->command_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0,
                                                   nullptr);
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[] = {
            gbuffer_base_rtv, gbuffer_normal_rtv, gbuffer_position_rtv, gbuffer_material_rtv};
        impl_->command_list->OMSetRenderTargets(4, targets, FALSE, &dsv);
        impl_->command_list->SetPipelineState(impl_->scene_pipeline.Get());
        impl_->command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        draw_character_instances();
    });

    auto lighting_pass = impl_->graph.AddPass(kRenderPassNames[3], QueueHint::Direct);
    lighting_pass.Read(gbuffer_base, Access::ShaderRead);
    lighting_pass.Read(gbuffer_normal, Access::ShaderRead);
    lighting_pass.Read(gbuffer_position, Access::ShaderRead);
    lighting_pass.Read(gbuffer_material, Access::ShaderRead);
    lighting_pass.Read(shadow, Access::ShaderRead);
    lighting_pass.Write(hdr, Access::RenderTarget);
    lighting_pass.SetExecute([&](RenderPassContext &) {
        constexpr float clear[] = {0, 0, 0, 0};
        impl_->command_list->ClearRenderTargetView(hdr_rtv, clear, 0, nullptr);
        draw_fullscreen(impl_->deferred_pipeline.Get(), hdr_rtv);
    });

    auto transparent_pass = impl_->graph.AddPass(kRenderPassNames[4], QueueHint::Direct);
    transparent_pass.Read(depth, Access::DepthRead);
    transparent_pass.Read(shadow, Access::ShaderRead);
    transparent_pass.Read(particles, Access::ShaderRead);
    transparent_pass.Read(particle_alive_output, Access::ShaderRead);
    transparent_pass.Read(indirect_arguments, Access::IndirectArgs);
    transparent_pass.Read(gbuffer_position, Access::ShaderRead);
    transparent_pass.Read(gbuffer_normal, Access::ShaderRead);
    transparent_pass.Write(oit_accumulation, Access::RenderTarget);
    transparent_pass.Write(oit_revealage, Access::RenderTarget);
    transparent_pass.SetExecute([&](RenderPassContext &) {
        constexpr float clear_accumulation[] = {0, 0, 0, 0};
        constexpr float clear_revealage[] = {1, 1, 1, 1};
        impl_->command_list->ClearRenderTargetView(
            oit_accumulation_rtv, clear_accumulation, 0, nullptr);
        impl_->command_list->ClearRenderTargetView(
            oit_revealage_rtv, clear_revealage, 0, nullptr);
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[] = {
            oit_accumulation_rtv, oit_revealage_rtv};
        impl_->command_list->OMSetRenderTargets(2, targets, FALSE, &dsv);
        impl_->command_list->SetPipelineState(impl_->particle_pipeline.Get());
        impl_->command_list->SetGraphicsRootShaderResourceView(
            2, impl_->particles.resource->GetGPUVirtualAddress());
        impl_->command_list->SetGraphicsRootShaderResourceView(
            12,
            impl_->particle_alive[particle_output_index].resource->GetGPUVirtualAddress());
        impl_->command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        impl_->command_list->ExecuteIndirect(
            impl_->draw_signature.Get(), 1, impl_->indirect_arguments.resource.Get(), 0, nullptr,
            0);

        // Family bodies and gel projectiles use the weighted OIT mesh pipeline.
        impl_->command_list->SetPipelineState(impl_->slime_pipeline.Get());
        impl_->command_list->IASetVertexBuffers(0, 1, &impl_->monster_assets[0].vertex_view);
        const auto draw_slime_range = [&](std::pair<std::size_t, std::size_t> range,
                                          const D3D12_VERTEX_BUFFER_VIEW &view,
                                          UINT vertices) {
            if (range.second == 0) return;
            impl_->command_list->SetGraphicsRootShaderResourceView(
                1, frame.upload.resource->GetGPUVirtualAddress() + kInstanceDataOffset +
                       range.first * sizeof(GpuInstance));
            impl_->command_list->IASetVertexBuffers(0, 1, &view);
            impl_->command_list->DrawInstanced(vertices, static_cast<UINT>(range.second), 0, 0);
        };
        draw_slime_range(monster_ranges[0],
                         impl_->monster_assets[0].vertex_view, impl_->monster_assets[0].vertex_count);
        draw_slime_range(monster_ranges[1],
                         impl_->monster_assets[1].vertex_view, impl_->monster_assets[1].vertex_count);
        draw_slime_range(monster_ranges[2],
                         impl_->monster_assets[2].vertex_view, impl_->monster_assets[2].vertex_count);
        draw_slime_range(projectile_range,
                         impl_->gel_projectile_vertex_view, impl_->gel_projectile_vertex_count);
    });

    auto composite_pass = impl_->graph.AddPass(kRenderPassNames[5], QueueHint::Direct);
    composite_pass.Read(hdr, Access::ShaderRead);
    composite_pass.Read(oit_accumulation, Access::ShaderRead);
    composite_pass.Read(oit_revealage, Access::ShaderRead);
    composite_pass.Write(post_a, Access::RenderTarget);
    composite_pass.SetExecute([&](RenderPassContext &) {
        constexpr float clear[] = {0, 0, 0, 0};
        impl_->command_list->ClearRenderTargetView(post_a_rtv, clear, 0, nullptr);
        draw_fullscreen(impl_->composite_pipeline.Get(), post_a_rtv);
    });

    auto bloom_pass = impl_->graph.AddPass(kRenderPassNames[6], QueueHint::Direct);
    bloom_pass.Read(post_a, Access::ShaderRead);
    bloom_pass.Write(post_b, Access::RenderTarget);
    bloom_pass.SetExecute([&](RenderPassContext &) {
        constexpr float clear[] = {0, 0, 0, 0};
        impl_->command_list->ClearRenderTargetView(post_b_rtv, clear, 0, nullptr);
        draw_fullscreen(impl_->bloom_pipeline.Get(), post_b_rtv);
    });

    auto tone_map_pass = impl_->graph.AddPass(kRenderPassNames[7], QueueHint::Direct);
    tone_map_pass.Read(post_b, Access::ShaderRead);
    tone_map_pass.Write(post_a, Access::RenderTarget);
    tone_map_pass.SetExecute([&](RenderPassContext &) {
        draw_fullscreen(impl_->tone_map_pipeline.Get(), post_a_rtv);
    });

    auto outline_pass = impl_->graph.AddPass(kRenderPassNames[8], QueueHint::Direct);
    outline_pass.Read(post_a, Access::ShaderRead);
    outline_pass.Read(gbuffer_position, Access::ShaderRead);
    outline_pass.Write(post_b, Access::RenderTarget);
    outline_pass.SetExecute([&](RenderPassContext &) {
        draw_fullscreen(impl_->outline_pipeline.Get(), post_b_rtv);
    });

    auto fxaa_pass = impl_->graph.AddPass(kRenderPassNames[9], QueueHint::Direct);
    fxaa_pass.Read(post_b, Access::ShaderRead);
    fxaa_pass.Write(back_buffer, Access::RenderTarget);
    fxaa_pass.SetExecute([&](RenderPassContext &) {
        impl_->command_list->RSSetViewports(1, &output_viewport);
        impl_->command_list->RSSetScissorRects(1, &output_scissor);
        draw_fullscreen(impl_->fxaa_pipeline.Get(), rtv);
    });

    auto ui_pass = impl_->graph.AddPass(kRenderPassNames[10], QueueHint::Direct);
    ui_pass.Read(ui, Access::ShaderRead);
    ui_pass.ReadWrite(back_buffer, Access::RenderTarget);
    ui_pass.SetExecute([&](RenderPassContext &) {
        (void)events;
        draw_fullscreen(impl_->ui_pipeline.Get(), rtv);
    });

    impl_->graph.SetFinalAccess(particles, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(particle_alive_input, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(particle_alive_output, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(particle_dead, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(particle_counters, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(indirect_arguments, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(shadow, Access::DepthWrite);
    impl_->graph.SetFinalAccess(depth, Access::DepthWrite);
    impl_->graph.SetFinalAccess(gbuffer_base, Access::ShaderRead);
    impl_->graph.SetFinalAccess(gbuffer_normal, Access::ShaderRead);
    impl_->graph.SetFinalAccess(gbuffer_position, Access::ShaderRead);
    impl_->graph.SetFinalAccess(gbuffer_material, Access::ShaderRead);
    impl_->graph.SetFinalAccess(hdr, Access::ShaderRead);
    impl_->graph.SetFinalAccess(oit_accumulation, Access::ShaderRead);
    impl_->graph.SetFinalAccess(oit_revealage, Access::ShaderRead);
    impl_->graph.SetFinalAccess(post_a, Access::ShaderRead);
    impl_->graph.SetFinalAccess(post_b, Access::ShaderRead);
#if defined(HS_DEVELOPMENT_TOOLS)
    impl_->graph.SetFinalAccess(back_buffer, Access::RenderTarget);
#else
    impl_->graph.SetFinalAccess(back_buffer, Access::Present);
#endif

    if (auto graph_result = impl_->graph.Execute(
            impl_->command_list.Get(), impl_->enhanced_command_list.Get(),
            impl_->enhanced ? BarrierMode::Enhanced : BarrierMode::Legacy,
            impl_->timestamp_heap.Get(), impl_->timestamp_readback.resource.Get(),
            back_buffer_index * kTimestampCountPerFrame);
        !graph_result)
    {
        return graph_result;
    }
#if defined(HS_DEVELOPMENT_TOOLS)
    if (impl_->config.devtools_visible)
    {
        impl_->command_list->RSSetViewports(1, &output_viewport);
        impl_->command_list->RSSetScissorRects(1, &output_scissor);
        impl_->command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        ID3D12DescriptorHeap *imgui_heaps[] = {impl_->imgui_heap.Get()};
        impl_->command_list->SetDescriptorHeaps(1, imgui_heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), impl_->command_list.Get());
    }
    impl_->TransitionTexture(impl_->back_buffers[back_buffer_index].Get(),
                             D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_STATE_PRESENT,
                             D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                             D3D12_BARRIER_LAYOUT_PRESENT);
#endif
    impl_->transient_textures_common = false;
    impl_->particles_initialized = true;
    impl_->particle_input_is_a = !impl_->particle_input_is_a;
    impl_->last_particle_tick = snapshot.header.tick;
    frame.timestamps_recorded = true;

    result = impl_->command_list->Close();
    if (FAILED(result))
    {
        return HResultFailure("Close command list", result);
    }
    ID3D12CommandList *lists[] = {impl_->command_list.Get()};
    impl_->queue->ExecuteCommandLists(1, lists);
    impl_->last_presented_index = back_buffer_index;
    result = impl_->swap_chain->Present(impl_->config.vsync ? 1 : 0, 0);
    if (FAILED(result))
    {
        return impl_->CheckDevice(result, "Present");
    }

    frame.fence_value = impl_->next_fence++;
    result = impl_->queue->Signal(impl_->fence.Get(), frame.fence_value);
    if (FAILED(result))
    {
        return impl_->CheckDevice(result, "Signal frame fence");
    }

    ++impl_->frame_number;
    frame_result = {impl_->frame_number, snapshot.header.tick,
#if defined(HS_DEVELOPMENT_TOOLS)
                    impl_->config.devtools_visible && ImGui::GetIO().WantCaptureMouse,
                    impl_->config.devtools_visible && ImGui::GetIO().WantCaptureKeyboard,
#else
                    false, false,
#endif
                    debug_command, debug_value, debug_secondary};
    impl_->CountValidationErrors();
    return Result::Success();
}


} // namespace hs
