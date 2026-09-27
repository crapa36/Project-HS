#include "renderer_impl.hpp"
#include <bit>
#include <limits>

#include <numbers>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

namespace hs
{

Result D3D12Renderer::Render(const RenderSnapshotExchange::ReadPair &snapshots,
                             std::span<const PresentationEvent> events,
                             std::span<const ParticleSpawnCommand> particle_spawns,
                             std::span<const VfxLineSpawnCommand> effect_lines,
                             std::span<const VfxEventInput> typed_vfx_events,
                             std::span<const VfxPersistentInput> typed_vfx_persistent,
                             std::span<const VfxGroundSpawnInput> typed_ground_spawns,
                             std::span<const VfxFlashSpawnInput> typed_flash_spawns,
                             std::span<const VfxOwnedMeshInput> typed_mesh_spawns,
                             std::span<const VfxFresnelInput> typed_fresnels,
                             const VfxRibbonFrameInput &typed_ribbons,
                             std::span<const VfxLightInput> typed_lights,
                             std::span<const VfxDistortionInput> typed_distortions,
                             std::span<const VfxDecalInput> typed_decals,
                             const DevToolsFrameData &devtools,
                             RendererFrameResult &frame_result)
{
    static_cast<void>(effect_lines);
    if (!impl_->initialized)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Renderer not initialized.");
    }
    for (const auto &input : typed_vfx_events)
        if (input.effect_handle == kInvalidVfxEffectHandle)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                   "Typed VFX event has an invalid effect handle.");
    std::string vfx_error;
    if (!impl_->typed_vfx_persistent_state.Update(typed_vfx_persistent, vfx_error))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", vfx_error);
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
    auto &transient_pool = impl_->transient_pools[back_buffer_index];
    transient_pool.ResetClaims();

    auto snapshot = snapshots.has_current ? snapshots.current : RenderSnapshot{};
    const bool new_session = snapshot.header.session_id != impl_->render_session_id;
    if (new_session)
    {
        impl_->render_session_id = snapshot.header.session_id;
        impl_->particles_initialized = false;
        impl_->temporal_history_valid = false;
    }
    const bool same_previous_session = snapshots.has_previous &&
        snapshots.previous.header.session_id == snapshot.header.session_id;
    std::unordered_map<std::uint64_t, const RenderInstance *> previous_by_id;
    previous_by_id.reserve(snapshots.has_previous ? snapshots.previous.instances.size() : 0);
    if (same_previous_session)
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
    if (new_session || snapshot.header.tick != impl_->observed_snapshot_tick)
    {
        impl_->observed_snapshot_tick = snapshot.header.tick;
        impl_->snapshot_arrival = now;
    }
    const auto interpolation =
        impl_->config.interpolate && same_previous_session
            ? std::clamp(
                  std::chrono::duration<float>(now - impl_->snapshot_arrival).count() * 60.0f,
                  0.0f, 1.0f)
            : 1.0f;
    std::vector<RenderInstance> render_instances(snapshot.instances.begin(),
                                                  snapshot.instances.end());
    // Replace the compatibility area body only when its authored fill is
    // actually present. Other area shapes and their gameplay warnings remain.
    if (impl_->config.slime_family_preview_count == 0)
        std::erase_if(render_instances, [&](const RenderInstance &instance) {
            return instance.mesh == RenderMesh::Area && instance.stable_id != 0 &&
                std::ranges::any_of(typed_ground_spawns, [&](const VfxGroundSpawnInput &spawn) {
                    return spawn.source_visual_kind != 0xff &&
                        spawn.stable_id == instance.stable_id &&
                        spawn.geometry.shape == VfxGroundShape::Circle &&
                        spawn.command.primitive == VfxPrimitive::LowFrequencyFill;
                });
        });
    if (impl_->config.slime_family_preview_count == 0)
        std::erase_if(render_instances, [&](const RenderInstance &instance) {
            return instance.mesh == RenderMesh::Area && instance.vfx_owner_id != 0 &&
                std::ranges::any_of(typed_ground_spawns, [&](const VfxGroundSpawnInput &spawn) {
                    return spawn.stable_id == instance.vfx_owner_id &&
                        ((spawn.source_visual_kind == 0xff &&
                          (spawn.geometry.shape == VfxGroundShape::LineBorder || spawn.geometry.shape == VfxGroundShape::ConeBorder)) ||
                         spawn.geometry.shape == VfxGroundShape::RingGapsBorder ||
                         spawn.geometry.shape == VfxGroundShape::RingGapsPreviewBorder ||
                         spawn.geometry.shape == VfxGroundShape::CirclePreviewBorder);
                });
        });
    if (impl_->config.slime_family_preview_count == 0)
        std::erase_if(render_instances, [&](const RenderInstance &instance) {
            return (instance.mesh == RenderMesh::PlayerProjectile || instance.mesh == RenderMesh::EnemyProjectile) &&
                std::ranges::any_of(typed_mesh_spawns, [&](const VfxOwnedMeshInput &head) {
                    return head.owner_id == instance.stable_id;
                });
        });
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
            const auto scale = kind == 0 ? 1.25f : kind == 1 ? 1.40625f : 1.5625f;
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
    const Float3 ribbon_eye{lod_eye.x, lod_eye.y, lod_eye.z};
    if (new_session || impl_->frame_number == 0) impl_->previous_ribbon_eye = ribbon_eye;
    if (!impl_->ribbon_state.Update(
            impl_->config.slime_family_preview_count == 0 ? typed_ribbons.sources : std::span<const VfxRibbonSourceInput>{},
            snapshot.header.tick, snapshot.header.session_id, typed_ribbons.catalog_generation,
            impl_->config.ribbon_history_points, interpolation, impl_->previous_ribbon_eye, vfx_error))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", vfx_error);
    const auto ribbon_updates = impl_->ribbon_state.Updates();
    const auto ribbon_outputs = impl_->ribbon_state.Outputs();
    std::unordered_set<std::uint64_t> ribbon_head_ids;
    for (const auto &source : typed_ribbons.sources) ribbon_head_ids.insert(source.owner_id);
    std::unordered_set<std::uint64_t> ribbon_projectile_ids;
    for (const auto &visual : snapshot.persistent_vfx)
        if (visual.kind == PersistentVfxKind::ProjectileHead && ribbon_head_ids.contains(visual.stable_id))
            ribbon_projectile_ids.insert(visual.projectile_owner_id);
    const auto ribbon_history_bytes = static_cast<UINT64>(impl_->ribbon_state.Capacity()) * kRibbonHistoryStride;
    const auto ribbon_args_bytes = std::max<std::size_t>(ribbon_outputs.size(), 1) * kRibbonArgumentStride;
    if (!impl_->ribbon_history.resource || impl_->ribbon_history.resource->GetDesc().Width != ribbon_history_bytes ||
        !impl_->ribbon_arguments.resource || impl_->ribbon_arguments.resource->GetDesc().Width < ribbon_args_bytes)
    {
        if (auto waited = impl_->WaitForGpu(); !waited) return waited;
        D3D12MA::ALLOCATION_DESC allocation{};
        allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        const auto allocate = [&](AllocationResource &resource, UINT64 bytes) {
            resource.Reset();
            return impl_->CreateAllocation(resource, allocation,
                BufferDescription(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        };
        if (!impl_->ribbon_history.resource || impl_->ribbon_history.resource->GetDesc().Width != ribbon_history_bytes)
        {
            if (auto allocated = allocate(impl_->ribbon_history, ribbon_history_bytes); !allocated) return allocated;
            if (auto allocated = allocate(impl_->ribbon_previous, ribbon_history_bytes); !allocated) return allocated;
        }
        if (!impl_->ribbon_arguments.resource || impl_->ribbon_arguments.resource->GetDesc().Width < ribbon_args_bytes)
            if (auto allocated = allocate(impl_->ribbon_arguments, ribbon_args_bytes); !allocated) return allocated;
    }
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
    const bool new_persistent_tick =
        snapshot.header.tick != impl_->last_status_visual_tick;
    std::vector<ParticleSpawnCommand> frame_particle_spawns;
    frame_particle_spawns.reserve(particle_spawns.size());
    if (impl_->config.slime_family_preview_count == 0)
    {
        frame_particle_spawns.insert(frame_particle_spawns.end(),
                                     particle_spawns.begin(), particle_spawns.end());
    }
    if (new_persistent_tick)
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
                if (impl_->config.slime_family_preview_count == 0 &&
                    (status == StatusVisual::Slow || status == StatusVisual::Mark))
                {
                    const auto kind = status == StatusVisual::Slow
                        ? PersistentVfxKind::EnemySlowStatus
                        : PersistentVfxKind::EnemyMarkStatus;
                    const auto authored = std::ranges::any_of(snapshot.persistent_vfx,
                        [&](const PersistentVfxVisual &visual) {
                            return visual.kind == kind &&
                                   visual.entity_render_id == source.stable_id &&
                                   std::ranges::any_of(typed_ground_spawns,
                                       [&](const VfxGroundSpawnInput &ground) {
                                           return ground.geometry.shape == VfxGroundShape::PolarRune &&
                                                  ground.stable_id == visual.stable_id &&
                                                  ground.source_visual_kind ==
                                                      static_cast<std::uint8_t>(kind);
                                       });
                        });
                    if (authored) return;
                }
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
            // Boss areas use authored direct outputs rather than a legacy emitter.
            if (visual.kind == PersistentVfxKind::BossAreaActive ||
                visual.kind == PersistentVfxKind::ProjectileHead ||
                visual.kind == PersistentVfxKind::BossDashWarning ||
                visual.kind == PersistentVfxKind::BossVolleyWarning ||
                visual.kind == PersistentVfxKind::RangedEnemyWarning ||
                visual.kind == PersistentVfxKind::MiniBombWarning ||
                visual.kind == PersistentVfxKind::EnemySpawnWarning ||
                visual.kind == PersistentVfxKind::BossSpawnWarning ||
                visual.kind == PersistentVfxKind::BossAreaWarning ||
                visual.kind == PersistentVfxKind::SuicideEnemyWarning ||
                visual.kind == PersistentVfxKind::BossShockwaveWavefront ||
                visual.kind == PersistentVfxKind::BossShockwaveWarning ||
                visual.kind == PersistentVfxKind::RicochetLink ||
                visual.kind == PersistentVfxKind::BurnTransferLink ||
                visual.kind == PersistentVfxKind::RelicChainLink) continue;
            // The authored head recipe owns the shared history. Do not add the
            // compatibility straight segment on top of the same projectile.
            if ((visual.kind == PersistentVfxKind::ProjectileTrail ||
                 visual.kind == PersistentVfxKind::ProjectileTrailOuter ||
                 visual.kind == PersistentVfxKind::RicochetProjectileTrail) &&
                ribbon_projectile_ids.contains(visual.projectile_owner_id)) continue;
            if ((visual.kind == PersistentVfxKind::DamageTrail ||
                 visual.kind == PersistentVfxKind::ChargeGuide) &&
                std::ranges::any_of(typed_ground_spawns, [&](const VfxGroundSpawnInput &spawn) {
                    return spawn.geometry.shape == VfxGroundShape::LineBorder &&
                        spawn.stable_id == visual.stable_id &&
                        spawn.source_visual_kind == static_cast<std::uint8_t>(visual.kind);
                })) continue;
            const bool authored_exact_ring =
                impl_->config.slime_family_preview_count == 0 &&
                std::ranges::any_of(
                    typed_ground_spawns, [&visual](const VfxGroundSpawnInput &spawn) {
                        return spawn.geometry.shape == VfxGroundShape::Circle &&
                            spawn.command.primitive == VfxPrimitive::ExactRing &&
                            spawn.stable_id == visual.stable_id &&
                            spawn.source_visual_kind ==
                                static_cast<std::uint8_t>(visual.kind);
                    });
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
                if (!authored_exact_ring) frame_particle_spawns.push_back(command);
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
            if (!authored_exact_ring ||
                (visual.kind != PersistentVfxKind::FireArea &&
                 visual.kind != PersistentVfxKind::ArrowRainArea &&
                 visual.kind != PersistentVfxKind::RangeIndicator))
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
        if (std::ranges::any_of(typed_ribbons.sources, [&](const VfxRibbonSourceInput &source) {
            return source.analytic && source.owner_id == line.sequence;
        })) continue;
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
    const auto ground_ring_data_offset =
        (particle_owner_data_offset + sizeof(std::uint32_t) * kParticleCount + 255u) &
        ~std::size_t{255u};
    const auto flash_data_offset =
        (ground_ring_data_offset + sizeof(GpuParticleSpawnCommand) *
             std::max<std::size_t>(typed_ground_spawns.size(), 1) + 255u) &
        ~std::size_t{255u};
    if (typed_ground_spawns.size() > std::numeric_limits<UINT>::max() ||
        std::max<std::size_t>(typed_ground_spawns.size(), 1) >
            (std::numeric_limits<std::size_t>::max() - ground_ring_data_offset) /
                sizeof(GpuParticleSpawnCommand))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Too many authored ground rings for frame upload.");
    if (typed_flash_spawns.size() > std::numeric_limits<UINT>::max() ||
        std::max<std::size_t>(typed_flash_spawns.size(), 1) >
            (std::numeric_limits<std::size_t>::max() - flash_data_offset) /
                sizeof(GpuParticleSpawnCommand))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Too many authored flashes for frame upload.");
    const auto owner_mesh_data_offset = (flash_data_offset +
        sizeof(GpuParticleSpawnCommand) * std::max<std::size_t>(typed_flash_spawns.size(), 1) + 255u) & ~std::size_t{255u};
    if (typed_mesh_spawns.size() > std::numeric_limits<UINT>::max() ||
        std::max<std::size_t>(typed_mesh_spawns.size(), 1) >
            (std::numeric_limits<std::size_t>::max() - owner_mesh_data_offset) / sizeof(GpuParticleSpawnCommand))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Too many owner mesh commands.");
    const auto ribbon_update_offset = (owner_mesh_data_offset +
        sizeof(GpuParticleSpawnCommand) * std::max<std::size_t>(typed_mesh_spawns.size(), 1) + 255u) & ~std::size_t{255u};
    const auto ribbon_output_offset = (ribbon_update_offset +
        sizeof(GpuRibbonSourceUpdate) * std::max<std::size_t>(ribbon_updates.size(), 1) + 255u) & ~std::size_t{255u};
    const auto ground_gap_offset = (ribbon_output_offset +
        sizeof(GpuRibbonOutput) * std::max<std::size_t>(ribbon_outputs.size(), 1) + 255u) & ~std::size_t{255u};
    std::size_t ground_gap_count{};
    for (const auto &spawn : typed_ground_spawns)
    {
        if (spawn.geometry.gap_angles_radians.size() > std::numeric_limits<UINT>::max() - ground_gap_count)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Ground gap index overflow.");
        ground_gap_count += spawn.geometry.gap_angles_radians.size();
    }
    for (const auto &distortion : typed_distortions)
    {
        if (distortion.shape != VfxDistortionShape::GappedAnnulus) continue;
        if (distortion.gap_angles_degrees.size() > std::numeric_limits<UINT>::max() - ground_gap_count)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Distortion gap index overflow.");
        ground_gap_count += distortion.gap_angles_degrees.size();
    }
    if (std::max<std::size_t>(ground_gap_count, 1) > (std::numeric_limits<std::size_t>::max() - ground_gap_offset) / sizeof(float))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Ground gap upload overflow.");
    struct alignas(16) GpuDistortion
    {
        DirectX::XMFLOAT4 position_radius, direction_strength, phase_alpha_shape_seed, geometry;
        DirectX::XMUINT4 gap_range;
    };
    static_assert(sizeof(GpuDistortion) == 80);
    constexpr std::uint32_t kMaxDistortions = 32;
    const auto distortion_data_offset = (ground_gap_offset + sizeof(float) * std::max<std::size_t>(ground_gap_count, 1) + 255u) & ~std::size_t{255u};
    struct alignas(16) GpuDecal
    {
        DirectX::XMFLOAT4 position_radius, axis_alpha_age, color_hdr, fracture, slab;
        DirectX::XMUINT4 metadata;
    };
    static_assert(sizeof(GpuDecal) == 96);
    constexpr std::uint32_t kMaxDecals = 64;
    const auto decal_data_offset = (distortion_data_offset + sizeof(GpuDistortion) * kMaxDistortions + 255u) & ~std::size_t{255u};
    constexpr std::size_t kMaxFresnelShells = 32;
    if (typed_fresnels.size() > kMaxFresnelShells)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Too many Fresnel shell commands.");
    const auto fresnel_data_offset =
        (decal_data_offset + sizeof(GpuDecal) * kMaxDecals + 255u) & ~std::size_t{255u};
    const auto required_upload_size = fresnel_data_offset +
        sizeof(GpuFresnelShell) * kMaxFresnelShells;
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
    if (!ribbon_updates.empty()) std::memcpy(frame.mapped + ribbon_update_offset, ribbon_updates.data(), ribbon_updates.size_bytes());
    if (!ribbon_outputs.empty()) std::memcpy(frame.mapped + ribbon_output_offset, ribbon_outputs.data(), ribbon_outputs.size_bytes());
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
    auto *gpu_ground_rings = reinterpret_cast<GpuParticleSpawnCommand *>(
        frame.mapped + ground_ring_data_offset);
    auto *gpu_flashes = reinterpret_cast<GpuParticleSpawnCommand *>(
        frame.mapped + flash_data_offset);
    auto *gpu_owner_meshes = reinterpret_cast<GpuParticleSpawnCommand *>(frame.mapped + owner_mesh_data_offset);
    auto *gpu_fresnels = reinterpret_cast<GpuFresnelShell *>(frame.mapped + fresnel_data_offset);
    struct FresnelDraw
    {
        std::size_t instance_index{};
        std::size_t asset_index{};
        std::size_t command_index{};
        bool archer{};
    };
    std::vector<FresnelDraw> fresnel_draws;
    fresnel_draws.reserve(typed_fresnels.size());
    std::unordered_set<std::uint64_t> fresnel_ids;
    for (std::size_t index = 0; index < typed_fresnels.size(); ++index)
    {
        const auto &source = typed_fresnels[index];
        const auto finite_transform = [](const VfxTransform &transform) {
            return std::ranges::all_of(transform, [](float value) { return std::isfinite(value); });
        };
        const bool crown = source.kind == VfxFresnelShellKind::BossCrown;
        const bool transition = source.kind == VfxFresnelShellKind::BossTransition;
        const bool player = source.kind == VfxFresnelShellKind::PlayerInvulnerable;
        if (source.render_instance_id == 0 || source.stable_id == 0 ||
            source.effect_handle == kInvalidVfxEffectHandle ||
            !fresnel_ids.insert(source.stable_id).second || (!crown && !transition && !player) ||
            !finite_transform(source.current_transform) ||
            !finite_transform(source.previous_transform) ||
            !std::isfinite(source.footprint_radius) || source.footprint_radius <= 0.0f ||
            !std::isfinite(source.source_progress) || source.source_progress < 0.0f ||
            source.source_progress >= 1.0f ||
            !std::isfinite(source.elapsed_seconds) || source.elapsed_seconds < 0.0f ||
            !std::isfinite(source.color.x) || source.color.x < 0.0f ||
            !std::isfinite(source.color.y) || source.color.y < 0.0f ||
            !std::isfinite(source.color.z) || source.color.z < 0.0f ||
            !std::isfinite(source.color.w) || source.color.w < 0.0f || source.color.w > 1.0f ||
            !std::isfinite(source.hdr) || source.hdr <= 0.0f ||
            source.gradient_row >= impl_->vfx_gradient_rows ||
            !std::isfinite(source.motion_rate_hz) || source.motion_rate_hz < 0.0f ||
            !std::isfinite(source.motion_amplitude) || source.motion_amplitude < 0.0f ||
            source.motion_amplitude > 1.0f ||
            !std::isfinite(source.motion_inset_fraction) ||
            source.motion_inset_fraction < 0.0f || source.motion_inset_fraction > 1.0f ||
            !std::isfinite(source.noise_amount) || source.noise_amount < 0.0f ||
            source.noise_amount > 1.0f ||
            (crown && (source.sector_count != 6 || !std::isfinite(source.rotation_hz) ||
                       source.rotation_hz >= 0.0f || source.fresnel_power != 0.0f ||
                       source.end_crack != 0 || source.noise_amount != 0.0f)) ||
            (transition && (source.sector_count != 0 || source.rotation_hz != 0.0f ||
                            !std::isfinite(source.fresnel_power) || source.fresnel_power <= 0.0f ||
                            source.end_crack != 1 || source.noise_amount != 0.0f)) ||
            (player && (source.sector_count != 0 || source.rotation_hz != 0.0f ||
                        !std::isfinite(source.fresnel_power) || source.fresnel_power <= 0.0f ||
                        source.end_crack != 0)))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                   "Invalid Fresnel shell command.");
        const auto match = std::ranges::find_if(render_instances, [&](const RenderInstance &instance) {
            return instance.stable_id == source.render_instance_id &&
                   (player ? instance.mesh == RenderMesh::Archer :
                    instance.mesh >= RenderMesh::BossFiveMinute && instance.mesh <= RenderMesh::BossFinal);
        });
        if (match == render_instances.end()) continue;
        const auto instance_index = static_cast<std::size_t>(match - render_instances.begin());
        const auto asset_index = player ? 0 : static_cast<std::size_t>(match->mesh) -
            static_cast<std::size_t>(RenderMesh::MonsterMelee);
        auto &target = gpu_fresnels[index];
        target.current_transform = source.current_transform;
        target.previous_transform = source.previous_transform;
        target.color = {source.color.x, source.color.y, source.color.z, source.color.w};
        target.geometry = {source.footprint_radius, source.source_progress,
                           source.elapsed_seconds, source.hdr};
        target.motion = {source.motion_rate_hz, source.motion_amplitude,
                         source.motion_inset_fraction, source.rotation_hz};
        target.signature = {source.fresnel_power, source.noise_amount, 0.0f, 0.0f};
        target.metadata = {static_cast<std::uint32_t>(source.kind), source.sector_count,
                           source.end_crack, source.gradient_row};
        target.identity = {static_cast<std::uint32_t>(source.render_instance_id),
                           static_cast<std::uint32_t>(source.render_instance_id >> 32),
                           static_cast<std::uint32_t>(source.stable_id),
                           static_cast<std::uint32_t>(source.stable_id >> 32)};
        fresnel_draws.push_back({instance_index, asset_index, index, player});
    }
    std::uint32_t owner_mesh_count{};
    if (impl_->config.slime_family_preview_count == 0)
        for (const auto &source : typed_mesh_spawns)
        {
            const auto finite = [](const Float3 &v) {
                return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
            };
            if (source.mesh_index == 0 || source.mesh_index > 7 ||
                source.gradient_row >= impl_->vfx_gradient_rows || !finite(source.position) ||
                !finite(source.previous_position) || !finite(source.velocity) ||
                !finite({source.color.x,source.color.y,source.color.z}) || !std::isfinite(source.color.w) ||
                !std::isfinite(source.radius) || source.radius <= 0 ||
                !std::isfinite(source.hdr) || source.hdr <= 0 ||
                !std::isfinite(source.fresnel) || source.fresnel < 0 || source.fresnel > 1)
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid owner mesh command.");
            auto &target_mesh = gpu_owner_meshes[owner_mesh_count++];
            target_mesh = {};
            target_mesh.position_lifetime_min = {
                std::lerp(source.previous_position.x, source.position.x, interpolation),
                std::lerp(source.previous_position.y, source.position.y, interpolation),
                std::lerp(source.previous_position.z, source.position.z, interpolation),0};
            target_mesh.direction_lifetime_max = {source.velocity.x,source.velocity.y,source.velocity.z,0};
            const float transverse = source.radius / impl_->vfx_mesh_transverse_radii[source.mesh_index-1];
            target_mesh.shape_extent_speed_min = {transverse,transverse,2.0f*source.radius,0};
            target_mesh.start_color = {source.color.x,source.color.y,source.color.z,source.color.w};
            target_mesh.size_range.y = source.hdr;
            target_mesh.rotation_range.x = source.fresnel;
            target_mesh.metadata.x = source.mesh_index;
            target_mesh.metadata.y = source.gradient_row;
        }
    std::uint32_t flash_count{};
    std::uint32_t flash_add_count{};
    for (const bool oit_group : {false, true})
    for (const auto &source : typed_flash_spawns)
    {
        if (source.oit != oit_group) continue;
        const auto finite = std::isfinite(source.position.x) &&
            std::isfinite(source.position.y) && std::isfinite(source.position.z) &&
            std::isfinite(source.color.x) && std::isfinite(source.color.y) &&
            std::isfinite(source.color.z) && std::isfinite(source.color.w) &&
            std::isfinite(source.size) && source.size > 0.0f &&
            std::isfinite(source.delay) && source.delay >= 0.0f &&
            std::isfinite(source.lifetime) && source.lifetime > 0.0f &&
            std::isfinite(source.hdr) && source.hdr > 0.0f &&
            std::isfinite(source.normalized_age) && source.normalized_age >= 0.0f &&
            source.normalized_age <= 1.0f && source.gradient_row < impl_->vfx_gradient_rows &&
            static_cast<std::uint32_t>(source.shape) <= static_cast<std::uint32_t>(VfxImpactShape::Spark) &&
            std::isfinite(source.direction.x) && std::isfinite(source.direction.y) && std::isfinite(source.direction.z) &&
            std::isfinite(source.aspect) && source.aspect > 0 &&
            (source.curve_row == 0xffffffffu || source.curve_row < 7) &&
            (source.mask_slice == 0xffffffffu || source.mask_slice < 12) &&
            std::isfinite(source.mask_strength) && source.mask_strength >= 0 && source.mask_strength <= 1 &&
            std::isfinite(source.initial_offset.x) && std::isfinite(source.initial_offset.y) && std::isfinite(source.initial_offset.z) &&
            std::isfinite(source.initial_velocity.x) && std::isfinite(source.initial_velocity.y) && std::isfinite(source.initial_velocity.z) &&
            std::isfinite(source.orbit_radius) && source.orbit_radius >= 0 &&
            std::isfinite(source.orbit_phase) && std::isfinite(source.orbit_rate) &&
            std::isfinite(source.drag) && source.drag >= 0 &&
            source.oit == (static_cast<std::uint32_t>(source.shape) >= 10 && static_cast<std::uint32_t>(source.shape) <= 12) &&
            source.fbm_octaves > 0 && source.fbm_octaves <= 4 &&
            std::isfinite(source.domain_warp) && source.domain_warp >= 0 &&
            std::isfinite(source.opacity_scale) && source.opacity_scale > 0 &&
            std::isfinite(source.motion_strength) && source.motion_strength >= 0 && source.motion_strength <= 1 &&
            std::isfinite(source.smoke_fps) && source.smoke_fps >= 0 &&
            (source.shape != VfxImpactShape::Smoke6Way ||
             (source.smoke_frame_count > 0 && source.smoke_first_frame < 64 &&
              source.smoke_frame_count <= 64 - source.smoke_first_frame && source.smoke_fps > 0));
        const auto spark_speed = std::hypot(source.initial_velocity.x,
                                            source.initial_velocity.y,
                                            source.initial_velocity.z);
        const auto spark_direction = std::hypot(source.direction.x,
                                                source.direction.y,
                                                source.direction.z);
        const auto spark_alignment = source.direction.x * source.initial_velocity.x +
            source.direction.y * source.initial_velocity.y +
            source.direction.z * source.initial_velocity.z;
        if (!finite || (source.shape == VfxImpactShape::Spark &&
            (source.oit || source.ground_base_anchor || source.aspect != 1.0f ||
             !std::isfinite(spark_speed) || spark_speed <= 0.0f ||
             !std::isfinite(spark_direction) || std::abs(spark_direction - 1.0f) > 0.001f ||
             spark_alignment < 0.999f * spark_speed)))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                   "Invalid authored flash command.");
        if (!source.oit) ++flash_add_count;
        auto &flash = gpu_flashes[flash_count++];
        flash = {};
        flash.position_lifetime_min = {source.position.x, source.position.y,
                                       source.position.z, source.ground_base_anchor ? 1.0f : 0.0f};
        flash.start_color = {source.color.x, source.color.y,
                             source.color.z, source.color.w};
        flash.size_range.x = source.size;
        flash.size_range.y = source.hdr;
        flash.size_range.z = source.normalized_age;
        flash.metadata.y = source.gradient_row;
        flash.metadata.x = static_cast<std::uint32_t>(source.shape);
        flash.metadata.z = source.curve_row;
        flash.metadata.w = source.mask_slice;
        flash.direction_lifetime_max = {source.direction.x, source.direction.y, source.direction.z, source.lifetime};
        flash.shape_extent_speed_min = {source.aspect, source.mask_strength, static_cast<float>(source.stable_seed & 65535u), source.opacity_scale};
        flash.end_color = {source.initial_offset.x, source.initial_offset.y, source.initial_offset.z, source.drag};
        flash.speed_cone_gravity_stretch = {source.initial_velocity.x, source.initial_velocity.y, source.initial_velocity.z, source.orbit_radius};
        flash.rotation_range = {source.orbit_phase, source.orbit_rate, source.domain_warp, source.motion_strength};
        flash.modes = {source.fbm_octaves, source.smoke_first_frame, source.smoke_frame_count, std::bit_cast<std::uint32_t>(source.smoke_fps)};
    }
    std::uint32_t ground_ring_count{};
    std::uint32_t ground_add_count{};
    auto *gpu_ground_gaps = reinterpret_cast<float *>(frame.mapped + ground_gap_offset);
    gpu_ground_gaps[0] = 0.0f;
    std::uint32_t gap_write_index{};
    if (impl_->config.slime_family_preview_count == 0)
    {
        for (const auto &input : typed_ground_spawns)
        {
            const auto &source = input.command;
            const auto &geometry = input.geometry;
            const bool line = geometry.shape == VfxGroundShape::LineBorder ||
                              geometry.shape == VfxGroundShape::LineHatch;
            const bool cone = geometry.shape == VfxGroundShape::ConeBorder ||
                              geometry.shape == VfxGroundShape::ConeHatch;
            const bool safe_sector = geometry.shape == VfxGroundShape::SafeSectorMarker;
            const bool hex = geometry.shape == VfxGroundShape::HexConstellation;
            const bool cross_ring = geometry.shape == VfxGroundShape::CrossRing;
            const bool broken_hex = geometry.shape == VfxGroundShape::BrokenHex;
            const bool axial_fracture = geometry.shape == VfxGroundShape::AxialFracture;
            const bool repeating_chevron = geometry.shape == VfxGroundShape::RepeatingChevron;
            const bool broken_crown = geometry.shape == VfxGroundShape::BrokenCrown;
            const bool closed_crown = geometry.shape == VfxGroundShape::ClosedCrownRing;
            const bool state_ring = geometry.shape == VfxGroundShape::StateRing;
            const bool polar_rune = geometry.shape == VfxGroundShape::PolarRune;
            const bool authored_additive_shape = cross_ring || broken_hex || axial_fracture ||
                repeating_chevron || broken_crown || closed_crown;
            const bool circle_preview = geometry.shape == VfxGroundShape::CirclePreviewBorder || geometry.shape == VfxGroundShape::CirclePreviewTicks;
            const bool preview = circle_preview || geometry.shape == VfxGroundShape::RingGapsPreviewBorder || geometry.shape == VfxGroundShape::RingGapsTicks;
            const bool annulus = preview || geometry.shape == VfxGroundShape::RingGapsBorder || geometry.shape == VfxGroundShape::RingGapsFill;
            const bool boss_signature = input.additive && geometry.animation_phase == 1.0f &&
                (geometry.shape == VfxGroundShape::Circle ||
                 geometry.shape == VfxGroundShape::ConeBorder ||
                 geometry.shape == VfxGroundShape::RingGapsBorder);
            if (preview && (!std::isfinite(geometry.progress) || geometry.progress < 0 || geometry.progress > 1 ||
                !std::isfinite(geometry.animation_phase) || geometry.animation_phase < 0 ||
                ((geometry.shape == VfxGroundShape::RingGapsTicks || geometry.shape == VfxGroundShape::CirclePreviewTicks) && geometry.tick_count == 0)))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored annulus preview timing.");
            if (annulus && (!std::isfinite(geometry.inner_radius) || geometry.inner_radius < 0 ||
                !std::isfinite(geometry.outer_radius) || geometry.outer_radius <= geometry.inner_radius ||
                !std::isfinite(geometry.edge_width) || geometry.edge_width < 0 ||
                ((geometry.shape == VfxGroundShape::RingGapsBorder || geometry.shape == VfxGroundShape::RingGapsPreviewBorder || geometry.shape == VfxGroundShape::CirclePreviewBorder) && geometry.edge_width == 0) ||
                (circle_preview && (geometry.inner_radius != 0 || !geometry.gap_angles_radians.empty() || geometry.gap_half_angle_radians != 0)) ||
                !std::isfinite(geometry.gap_half_angle_radians) || geometry.gap_half_angle_radians < 0 ||
                geometry.gap_half_angle_radians > std::numbers::pi_v<float> ||
                std::ranges::any_of(geometry.gap_angles_radians, [](float angle) { return !std::isfinite(angle); })))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored annulus geometry.");
            const float direction_length = std::hypot(geometry.direction.x, geometry.direction.z);
            if (safe_sector && (!std::isfinite(geometry.inner_radius) || geometry.inner_radius < 0 ||
                !std::isfinite(geometry.outer_radius) || geometry.outer_radius <= geometry.inner_radius ||
                !std::isfinite(geometry.half_angle_radians) || geometry.half_angle_radians <= 0 || geometry.half_angle_radians > std::numbers::pi_v<float> ||
                !std::isfinite(geometry.edge_width) || geometry.edge_width <= 0 ||
                !std::isfinite(geometry.animation_phase) || geometry.spokes == 0 || geometry.rings == 0 ||
                !std::isfinite(direction_length) || direction_length < .999f || direction_length > 1.001f ||
                !std::isfinite(geometry.direction.y) || geometry.direction.y != 0))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored safe-sector marker.");
            if (hex && (!input.additive || source.primitive != VfxPrimitive::ExactRing ||
                geometry.spokes != 6 || !std::isfinite(geometry.progress) ||
                geometry.progress < 0.0f || geometry.progress >= 1.0f ||
                (geometry.animation_phase != 0.0f && geometry.animation_phase != 1.0f) ||
                !std::isfinite(geometry.outer_radius) || geometry.outer_radius <= 0.0f ||
                !std::isfinite(geometry.edge_width) || geometry.edge_width <= 0.0f ||
                !std::isfinite(source.start_size_min) ||
                source.start_size_min < geometry.outer_radius + geometry.edge_width ||
                (geometry.mask_slice != 0xffffffffu && geometry.mask_slice >= 12) ||
                !std::isfinite(geometry.mask_strength) || geometry.mask_strength < 0.0f ||
                geometry.mask_strength > 1.0f ||
                (geometry.mask_slice == 0xffffffffu && geometry.mask_strength != 0.0f)))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored hex constellation.");
            if (state_ring && (input.additive || source.primitive != VfxPrimitive::ExactRing ||
                geometry.spokes < 3 || geometry.spokes > 64 ||
                geometry.rings > 1 ||
                !std::isfinite(geometry.progress) ||
                geometry.progress < 0.0f || geometry.progress > 1.0f ||
                !std::isfinite(geometry.animation_phase) || geometry.animation_phase < 0.0f ||
                !std::isfinite(geometry.outer_radius) || geometry.outer_radius <= 0.0f ||
                !std::isfinite(geometry.inner_radius) || geometry.inner_radius < 0.0f ||
                geometry.inner_radius >= geometry.outer_radius ||
                !std::isfinite(geometry.edge_width) || geometry.edge_width <= 0.0f ||
                std::abs(geometry.inner_radius -
                         std::max(0.0f, geometry.outer_radius - geometry.edge_width)) > 0.0001f ||
                !std::isfinite(source.start_size_min) ||
                source.start_size_min <= 0.0f ||
                std::abs(source.start_size_min -
                         (geometry.outer_radius + geometry.edge_width)) > 0.0001f ||
                !std::isfinite(source.stretch) ||
                std::abs(source.stretch - geometry.outer_radius / source.start_size_min) > 0.001f ||
                geometry.mask_slice != 0xffffffffu || geometry.mask_strength != 0.0f ||
                !geometry.gap_angles_radians.empty()))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored state ring.");
            if (polar_rune && (input.additive || source.primitive != VfxPrimitive::ExactRing ||
                geometry.spokes != 6 || geometry.rings != 2 ||
                !std::isfinite(geometry.progress) || geometry.progress < 0.0f ||
                geometry.progress >= 1.0f || !std::isfinite(geometry.animation_phase) ||
                geometry.animation_phase < 0.0f ||
                !std::isfinite(geometry.outer_radius) || geometry.outer_radius <= 0.0f ||
                !std::isfinite(geometry.inner_radius) || geometry.inner_radius < 0.0f ||
                geometry.inner_radius >= geometry.outer_radius ||
                !std::isfinite(geometry.edge_width) || geometry.edge_width <= 0.0f ||
                std::abs(geometry.inner_radius -
                         std::max(0.0f, geometry.outer_radius - geometry.edge_width)) > 0.0001f ||
                !std::isfinite(source.start_size_min) ||
                std::abs(source.start_size_min -
                         (geometry.outer_radius + geometry.edge_width)) > 0.0001f ||
                !std::isfinite(source.stretch) ||
                std::abs(source.stretch - geometry.outer_radius / source.start_size_min) > 0.001f ||
                geometry.mask_slice != 0xffffffffu || geometry.mask_strength != 0.0f ||
                !geometry.gap_angles_radians.empty()))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored polar rune.");
            const float authored_radius = geometry.outer_radius;
            const float authored_edge = geometry.edge_width;
            const float authored_extent = axial_fracture
                ? std::max(geometry.range * 0.5f + authored_edge,
                           std::max(authored_radius, authored_edge * 3.0f))
                : repeating_chevron
                    ? std::max(geometry.half_length, geometry.half_width) + authored_edge
                    : authored_radius + authored_edge;
            const float authored_direction_length = std::hypot(geometry.direction.x,
                                                                geometry.direction.z);
            const bool invalid_direction = (axial_fracture || repeating_chevron) &&
                (!std::isfinite(authored_direction_length) ||
                 authored_direction_length < 0.999f ||
                 authored_direction_length > 1.001f ||
                 !std::isfinite(geometry.direction.y) || geometry.direction.y != 0.0f);
            const bool invalid_chevron = repeating_chevron &&
                (!std::isfinite(geometry.half_width) || geometry.half_width <= 0.0f ||
                 !std::isfinite(geometry.half_length) || geometry.half_length <= 0.0f ||
                 !std::isfinite(geometry.spacing) || geometry.spacing <= 0.0f ||
                 geometry.spokes != 7);
            if (authored_additive_shape && (!input.additive ||
                source.primitive != VfxPrimitive::ExactRing ||
                !std::isfinite(geometry.progress) || geometry.progress < 0.0f ||
                geometry.progress >= 1.0f ||
                !std::isfinite(geometry.animation_phase) ||
                (!repeating_chevron && (!std::isfinite(authored_radius) ||
                                        authored_radius <= 0.0f)) ||
                !std::isfinite(authored_edge) || authored_edge <= 0.0f ||
                !std::isfinite(authored_extent) ||
                !std::isfinite(source.start_size_min) ||
                source.start_size_min < authored_extent ||
                (broken_hex && (!std::isfinite(geometry.gap_half_angle_radians) ||
                    geometry.gap_half_angle_radians <= 0.0f ||
                    geometry.gap_half_angle_radians >= std::numbers::pi_v<float> / 3.0f)) ||
                (broken_crown && (!std::isfinite(geometry.inner_radius) ||
                    geometry.inner_radius <= 0.0f ||
                    geometry.inner_radius >= authored_radius)) ||
                invalid_chevron || invalid_direction ||
                (axial_fracture && (!std::isfinite(geometry.range) ||
                    geometry.range <= 0.0f))))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored additive ground geometry.");
            if (boss_signature && (source.primitive != VfxPrimitive::ExactRing ||
                !std::isfinite(geometry.progress) || geometry.progress < 0.0f ||
                geometry.progress >= 1.0f || !std::isfinite(geometry.edge_width) ||
                geometry.edge_width <= 0.0f ||
                (geometry.shape == VfxGroundShape::Circle &&
                 (!std::isfinite(geometry.outer_radius) || geometry.outer_radius <= 0.0f)) ||
                !std::isfinite(source.start_size_min) ||
                source.start_size_min < (geometry.shape == VfxGroundShape::ConeBorder
                    ? geometry.range : geometry.outer_radius) + geometry.edge_width))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored boss signature.");
            if ((geometry.shape != VfxGroundShape::Circle && !line && !cone && !annulus && !safe_sector && !hex && !authored_additive_shape && !state_ring && !polar_rune) ||
                ((line || cone) && (!std::isfinite(direction_length) || direction_length < 0.999f || direction_length > 1.001f ||
                          !std::isfinite(geometry.direction.y) || geometry.direction.y != 0.0f ||
                          (line && (!std::isfinite(geometry.half_width) || geometry.half_width <= 0.0f ||
                                    !std::isfinite(geometry.half_length) || geometry.half_length <= 0.0f)) ||
                          (cone && (!std::isfinite(geometry.range) || geometry.range <= 0.0f ||
                                    !std::isfinite(geometry.half_angle_radians) || geometry.half_angle_radians <= 0.0f ||
                                    geometry.half_angle_radians > std::numbers::pi_v<float>)) ||
                          !std::isfinite(geometry.edge_width) || geometry.edge_width < 0.0f ||
                          ((geometry.shape == VfxGroundShape::LineBorder || geometry.shape == VfxGroundShape::ConeBorder) && geometry.edge_width == 0.0f) ||
                          !std::isfinite(geometry.spacing) || geometry.spacing <= 0.0f ||
                          !std::isfinite(geometry.scroll))))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored line geometry.");
            const auto finite_color = std::isfinite(source.start_color.x) &&
                std::isfinite(source.start_color.y) &&
                std::isfinite(source.start_color.z) &&
                std::isfinite(source.start_color.w);
            if (input.effect_handle == kInvalidVfxEffectHandle ||
                source.renderer != VfxRenderer::Ground ||
                (source.primitive != VfxPrimitive::ExactRing &&
                 source.primitive != VfxPrimitive::LowFrequencyFill) || source.count != 1 ||
                (input.additive && ((geometry.shape != VfxGroundShape::Circle && !hex && !boss_signature && !authored_additive_shape) ||
                                    source.primitive != VfxPrimitive::ExactRing)) ||
                (input.additive && geometry.shape == VfxGroundShape::Circle &&
                 geometry.animation_phase != 0.0f && !boss_signature) ||
                !std::isfinite(source.position.x) ||
                !std::isfinite(source.position.y) ||
                !std::isfinite(source.position.z) ||
                !std::isfinite(source.start_size_min) ||
                source.start_size_min <= 0.0f ||
                !std::isfinite(source.stretch) ||
                (!line && !cone && !annulus && !safe_sector && !axial_fracture &&
                 !repeating_chevron &&
                 source.primitive == VfxPrimitive::ExactRing &&
                 (source.stretch <= 0.0f || source.stretch >= 1.0f)) ||
                !finite_color || !std::isfinite(input.motion.rate_hz) ||
                !std::isfinite(input.motion.amplitude) ||
                !std::isfinite(input.motion.inset_fraction) ||
                input.motion.rate_hz < 0.0f || input.motion.amplitude < 0.0f ||
                input.motion.amplitude > 1.0f || input.motion.inset_fraction < 0.0f ||
                input.motion.inset_fraction > 1.0f ||
                !std::isfinite(input.hdr) || input.hdr < 0.0f ||
                input.gradient_row >= impl_->vfx_gradient_rows)
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                       "Invalid authored ground command.");
            // OIT commands grow from the front; additive rings grow from the
            // back of the same upload allocation.
            auto &gpu_ring = input.additive
                ? gpu_ground_rings[typed_ground_spawns.size() - ++ground_add_count]
                : gpu_ground_rings[ground_ring_count++];
            gpu_ring = {};
            gpu_ring.position_lifetime_min = {source.position.x, source.position.y,
                                              source.position.z, 0.0f};
            gpu_ring.speed_cone_gravity_stretch.w = source.stretch;
            gpu_ring.start_color = {source.start_color.x, source.start_color.y,
                                    source.start_color.z, source.start_color.w};
            gpu_ring.size_range.x = source.start_size_min;
            gpu_ring.size_range.y = input.hdr;
            gpu_ring.metadata.x = static_cast<std::uint32_t>(source.primitive);
            gpu_ring.metadata.y = input.gradient_row;
            gpu_ring.rotation_range = {input.motion.rate_hz, input.motion.amplitude,
                                       input.motion.inset_fraction, 0.0f};
            if (line || cone)
            {
                gpu_ring.metadata.z = static_cast<std::uint32_t>(geometry.shape);
                gpu_ring.direction_lifetime_max = {geometry.direction.x, 0.0f, geometry.direction.z, 0.0f};
                gpu_ring.shape_extent_speed_min = {cone ? geometry.range : geometry.half_width,
                                                  cone ? geometry.half_angle_radians : geometry.half_length,
                                                  geometry.edge_width, 0.0f};
                gpu_ring.speed_cone_gravity_stretch = {geometry.spacing, geometry.scroll, 0.0f, 0.0f};
            }
            if (safe_sector)
            {
                gpu_ring.metadata.z = static_cast<std::uint32_t>(geometry.shape);
                gpu_ring.direction_lifetime_max = {geometry.direction.x, 0, geometry.direction.z, 0};
                gpu_ring.shape_extent_speed_min = {geometry.inner_radius, geometry.outer_radius, geometry.edge_width, geometry.half_angle_radians};
                gpu_ring.end_color = {0, geometry.animation_phase, 0, 0};
                gpu_ring.modes.x = geometry.spokes;
                gpu_ring.modes.y = geometry.rings;
            }
            if (hex)
            {
                gpu_ring.metadata.z = static_cast<std::uint32_t>(geometry.shape);
                gpu_ring.shape_extent_speed_min = {0.0f, geometry.outer_radius,
                                                   geometry.edge_width, geometry.mask_strength};
                gpu_ring.end_color = {geometry.progress, geometry.animation_phase, 0.0f, 0.0f};
                gpu_ring.modes.x = geometry.spokes;
                gpu_ring.modes.y = geometry.mask_slice;
            }
            if (state_ring)
            {
                gpu_ring.metadata.z = static_cast<std::uint32_t>(geometry.shape);
                gpu_ring.shape_extent_speed_min = {geometry.inner_radius,
                    geometry.outer_radius, geometry.edge_width, 0.0f};
                gpu_ring.end_color = {geometry.progress, geometry.animation_phase, 0.0f, 0.0f};
                gpu_ring.modes.x = geometry.spokes;
                gpu_ring.modes.y = geometry.rings;
            }
            if (polar_rune)
            {
                gpu_ring.metadata.z = static_cast<std::uint32_t>(geometry.shape);
                gpu_ring.shape_extent_speed_min = {geometry.inner_radius,
                    geometry.outer_radius, geometry.edge_width, 0.0f};
                gpu_ring.end_color = {geometry.progress, geometry.animation_phase, 0.0f, 0.0f};
                gpu_ring.modes.x = geometry.spokes;
                gpu_ring.modes.y = geometry.rings;
            }
            if (authored_additive_shape)
            {
                gpu_ring.metadata.z = static_cast<std::uint32_t>(geometry.shape);
                gpu_ring.shape_extent_speed_min = {
                    broken_crown ? geometry.inner_radius :
                        (axial_fracture ? authored_radius :
                         repeating_chevron ? geometry.half_width : 0.0f),
                    axial_fracture ? geometry.range :
                        (repeating_chevron ? geometry.half_length : authored_radius),
                    authored_edge,
                    broken_hex ? geometry.gap_half_angle_radians :
                        (repeating_chevron ? geometry.spacing : 0.0f)};
                gpu_ring.end_color = {geometry.progress, geometry.animation_phase, 0.0f, 0.0f};
                if (axial_fracture || repeating_chevron)
                    gpu_ring.direction_lifetime_max = {geometry.direction.x, 0.0f,
                                                         geometry.direction.z, 0.0f};
                if (repeating_chevron) gpu_ring.modes.x = geometry.spokes;
            }
            if (annulus)
            {
                gpu_ring.metadata.z = static_cast<std::uint32_t>(geometry.shape);
                gpu_ring.shape_extent_speed_min = {geometry.inner_radius, geometry.outer_radius,
                    geometry.edge_width, geometry.gap_half_angle_radians};
                if (preview) gpu_ring.end_color = {geometry.progress, geometry.animation_phase,
                    std::bit_cast<float>(geometry.tick_count), 0.0f};
                gpu_ring.modes.x = gap_write_index;
                gpu_ring.modes.y = static_cast<std::uint32_t>(geometry.gap_angles_radians.size());
                for (float angle : geometry.gap_angles_radians) gpu_ground_gaps[gap_write_index++] = angle;
            }
            if (boss_signature)
            {
                gpu_ring.end_color = {geometry.progress, 1.0f, 0.0f, 0.0f};
                if (geometry.shape == VfxGroundShape::Circle)
                    gpu_ring.speed_cone_gravity_stretch.w =
                        geometry.outer_radius / source.start_size_min;
            }
        }
    }
    constexpr auto particle_capacity = kParticleCount;
    std::uint32_t gpu_particle_spawn_count{};
    std::uint32_t total_particles_to_spawn{};
    for (const auto &source : frame_particle_spawns)
    {
        if (source.mesh_index > 7)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "VFX mesh selector exceeds atlas range");
        if (source.authored_ballistic && (source.renderer != VfxRenderer::Mesh || source.mesh_index == 0 || source.count != 1 ||
            !std::isfinite(source.position.x) || !std::isfinite(source.position.y) || !std::isfinite(source.position.z) ||
            !std::isfinite(source.lifetime_min) || !std::isfinite(source.lifetime_max) || source.lifetime_min <= 0 || source.lifetime_max < source.lifetime_min ||
            !std::isfinite(source.start_size_min) || !std::isfinite(source.start_size_max) || source.start_size_min <= 0 || source.start_size_max < source.start_size_min ||
            !std::isfinite(source.end_size_min) || !std::isfinite(source.end_size_max) || source.end_size_min < 0 || source.end_size_max < source.end_size_min ||
            !std::isfinite(source.start_color.x) || !std::isfinite(source.start_color.y) || !std::isfinite(source.start_color.z) || !std::isfinite(source.start_color.w) ||
            !std::isfinite(source.end_color.x) || !std::isfinite(source.end_color.y) || !std::isfinite(source.end_color.z) || !std::isfinite(source.end_color.w) ||
            !std::isfinite(source.rotation_min) || !std::isfinite(source.rotation_max) || !std::isfinite(source.angular_velocity_min) || !std::isfinite(source.angular_velocity_max) ||
            !std::isfinite(source.authored_ground_y + source.authored_collision_radius) ||
            !std::isfinite(source.authored_initial_velocity.x) || !std::isfinite(source.authored_initial_velocity.y) || !std::isfinite(source.authored_initial_velocity.z) ||
            !std::isfinite(source.authored_drag) || source.authored_drag < 0 ||
            !std::isfinite(source.authored_bounce) || source.authored_bounce < 0 || source.authored_bounce > 1 ||
            !std::isfinite(source.authored_ground_y) || !std::isfinite(source.authored_collision_radius) || source.authored_collision_radius < 0 ||
            !std::isfinite(source.authored_hdr) || source.authored_hdr <= 0 || source.authored_gradient_row >= impl_->vfx_gradient_rows ||
            !std::isfinite(source.authored_birth_fraction) || source.authored_birth_fraction < 0 || source.authored_birth_fraction >= 1 ||
            !std::isfinite(source.gravity)))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored ballistic command.");
        if (source.authored_ballistic && (snapshot.header.tick < source.authored_birth_tick ||
            (snapshot.header.tick == source.authored_birth_tick && source.authored_birth_fraction > 0))) continue;
        if (total_particles_to_spawn == particle_capacity)
        {
            break;
        }
        const auto age_ticks =
            snapshot.header.tick > source.tick ? snapshot.header.tick - source.tick : 0;
        const auto age = source.authored_ballistic
            ? (static_cast<float>(snapshot.header.tick - source.authored_birth_tick) - source.authored_birth_fraction) / 60.0f
            : static_cast<float>(age_ticks) / 60.0f;
        if (age >= source.lifetime_max || source.count == 0)
        {
            continue;
        }
        const auto count = std::min(source.count, particle_capacity - total_particles_to_spawn);
        auto &target_spawn = gpu_particle_spawns[gpu_particle_spawn_count++];
        target_spawn = {};
        if (source.authored_ballistic)
        {
            target_spawn.current_velocity_drag = {source.authored_initial_velocity.x, source.authored_initial_velocity.y, source.authored_initial_velocity.z, source.authored_drag};
            target_spawn.collision_material = {source.authored_bounce, source.authored_ground_y, source.authored_collision_radius, source.authored_hdr};
            target_spawn.authored = {1u, std::bit_cast<std::uint32_t>(source.authored_birth_fraction), static_cast<std::uint32_t>(source.authored_birth_tick), source.authored_gradient_row};
        }
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
                                     (static_cast<std::uint32_t>(source.primitive) << 16u) |
                                     (static_cast<std::uint32_t>(source.mesh_index) << 24u);
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
    const bool temporal_enabled = impl_->config.vfx_quality != VfxQuality::Low;
    const auto projection = DirectX::XMMatrixPerspectiveFovLH(
        DirectX::XMConvertToRadians(snapshot.camera.vertical_fov_degrees),
        static_cast<float>(impl_->render_width) / static_cast<float>(impl_->render_height), 500.0f,
        0.1f);
    const auto halton = [](std::uint64_t index, std::uint32_t base) noexcept {
        float value = 0.0f;
        float scale = 1.0f / static_cast<float>(base);
        while (index != 0)
        {
            value += static_cast<float>(index % base) * scale;
            index /= base;
            scale /= static_cast<float>(base);
        }
        return value;
    };
    const auto jitter_index = 1u + impl_->frame_number % 8u;
    const auto jitter_x = temporal_enabled
        ? (halton(jitter_index, 2) - 0.5f) * 2.0f /
              static_cast<float>(impl_->render_width)
        : 0.0f;
    const auto jitter_y = temporal_enabled
        ? (0.5f - halton(jitter_index, 3)) * 2.0f /
              static_cast<float>(impl_->render_height)
        : 0.0f;
    const auto clip_matrix = view * projection *
        DirectX::XMMatrixTranslation(jitter_x, jitter_y, 0.0f);
    DirectX::XMStoreFloat4x4(&constants->view_projection,
                             DirectX::XMMatrixTranspose(clip_matrix));
    DirectX::XMStoreFloat4x4(&constants->previous_view_projection,
                             impl_->temporal_history_valid
                                 ? DirectX::XMLoadFloat4x4(&impl_->previous_view_projection)
                                 : DirectX::XMMatrixTranspose(clip_matrix));
    DirectX::XMFLOAT3 current_temporal_eye{};
    DirectX::XMStoreFloat3(&current_temporal_eye, eye);
    const auto camera_delta = std::hypot(
        current_temporal_eye.x - impl_->temporal_previous_eye.x,
        current_temporal_eye.y - impl_->temporal_previous_eye.y,
        current_temporal_eye.z - impl_->temporal_previous_eye.z);
    if (impl_->temporal_history_valid &&
        (camera_delta > std::max(2.0f, camera_distance * 0.25f) ||
         std::abs(snapshot.camera.vertical_fov_degrees - impl_->temporal_previous_fov) > 1.0f))
        impl_->temporal_history_valid = false;
    constants->temporal_options = {temporal_enabled ? 1.0f : 0.0f,
                                   impl_->temporal_history_valid ? 1.0f : 0.0f,
                                   0.0f, 0.0f};
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
    struct LightCandidate { const VfxLightInput *light; float screen_area; };
    std::vector<LightCandidate> light_candidates;
    for (const auto &source : typed_lights)
    {
        if (!std::isfinite(source.position.x) || !std::isfinite(source.position.y) || !std::isfinite(source.position.z) ||
            !std::isfinite(source.radius) || source.radius <= 0 || !std::isfinite(source.intensity) || source.intensity < 0 ||
            !std::isfinite(source.linear_rgb.x) || !std::isfinite(source.linear_rgb.y) || !std::isfinite(source.linear_rgb.z) ||
            source.linear_rgb.x < 0 || source.linear_rgb.y < 0 || source.linear_rgb.z < 0 ||
            static_cast<std::uint32_t>(source.quality) > 2)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored VFX light.");
        if (source.quality == VfxQuality::Low || source.intensity == 0 || impl_->config.slime_family_preview_count != 0) continue;
        const auto world = DirectX::XMVectorSet(source.position.x, source.position.y, source.position.z, 1);
        DirectX::XMFLOAT3 view_position;
        DirectX::XMStoreFloat3(&view_position, DirectX::XMVector3TransformCoord(world, view));
        if (view_position.z + source.radius <= .1f || view_position.z - source.radius >= 500.0f) continue;
        const float distance = std::max(view_position.z, .1f);
        const float tangent_y = std::tan(DirectX::XMConvertToRadians(snapshot.camera.vertical_fov_degrees) * .5f);
        const float tangent_x = tangent_y * static_cast<float>(impl_->render_width) / impl_->render_height;
        // Sphere distance to each slanted frustum plane, not a box at center depth.
        if (std::abs(view_position.x) - view_position.z * tangent_x > source.radius * std::sqrt(1.0f+tangent_x*tangent_x) ||
            std::abs(view_position.y) - view_position.z * tangent_y > source.radius * std::sqrt(1.0f+tangent_y*tangent_y)) continue;
        light_candidates.push_back({&source, std::min(1.0f, source.radius * source.radius / (distance * distance))});
    }
    std::sort(light_candidates.begin(), light_candidates.end(), [](const auto &a, const auto &b) {
        if (a.light->importance != b.light->importance) return a.light->importance > b.light->importance;
        if (a.screen_area != b.screen_area) return a.screen_area > b.screen_area;
        return a.light->stable_id < b.light->stable_id;
    });
    constants->vfx_light_count = {};
    for (const auto &candidate : light_candidates)
    {
        const auto &source = *candidate.light;
        const auto limit = source.quality == VfxQuality::Medium ? impl_->config.vfx_light_count / 2 : impl_->config.vfx_light_count;
        const auto index = constants->vfx_light_count.x;
        if (index >= limit) continue;
        constants->vfx_light_position_radius[index] = {source.position.x,source.position.y,source.position.z,source.radius};
        constants->vfx_light_color_intensity[index] = {source.linear_rgb.x,source.linear_rgb.y,source.linear_rgb.z,source.intensity};
        ++constants->vfx_light_count.x;
    }
    struct DistortionCandidate { const VfxDistortionInput *source; float area; };
    std::vector<DistortionCandidate> distortion_candidates;
    for (const auto &source : typed_distortions)
    {
        const auto finite = [](Float3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
        if (!finite(source.position) || !finite(source.direction) || !std::isfinite(source.radius) || source.radius <= 0 ||
            !std::isfinite(source.strength) || source.strength < 0 || !std::isfinite(source.alpha) || source.alpha < 0 || source.alpha > 1 ||
            !std::isfinite(source.normalized_age) || source.normalized_age < 0 || source.normalized_age > 1 ||
            static_cast<std::uint32_t>(source.shape) > 6 || static_cast<std::uint32_t>(source.quality) > 2)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored distortion command.");
        const bool cone = source.shape == VfxDistortionShape::ConeSector;
        const bool annulus = source.shape == VfxDistortionShape::GappedAnnulus;
        const float horizontal_direction = std::hypot(source.direction.x,source.direction.z);
        if ((cone && (!std::isfinite(source.half_angle_degrees) || source.half_angle_degrees <= 0 ||
                      source.half_angle_degrees > 180 || !std::isfinite(horizontal_direction) ||
                      horizontal_direction <= .0001f || source.inner_radius != 0 ||
                      source.gap_half_width_degrees != 0 || !source.gap_angles_degrees.empty())) ||
            (annulus && (!std::isfinite(source.inner_radius) || source.inner_radius < 0 ||
                         source.inner_radius >= source.radius || !std::isfinite(source.gap_half_width_degrees) ||
                         source.gap_half_width_degrees < 0 || source.gap_half_width_degrees > 180 ||
                         source.half_angle_degrees != 0 ||
                         (!source.gap_angles_degrees.empty() && source.gap_half_width_degrees == 0) ||
                         std::any_of(source.gap_angles_degrees.begin(),source.gap_angles_degrees.end(),
                             [](float angle){return !std::isfinite(angle); }))) ||
            (!cone && !annulus && (source.inner_radius != 0 || source.half_angle_degrees != 0 ||
                                   source.gap_half_width_degrees != 0 || !source.gap_angles_degrees.empty())))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid exact distortion geometry.");
        if (source.quality == VfxQuality::Low || source.alpha == 0 || source.strength == 0 || impl_->config.slime_family_preview_count != 0) continue;
        DirectX::XMFLOAT3 center;
        DirectX::XMStoreFloat3(&center, DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(source.position.x,source.position.y,source.position.z,1),view));
        const float radius = source.radius;
        if (center.z + radius <= .1f || center.z - radius >= 500) continue;
        const float ty = std::tan(DirectX::XMConvertToRadians(snapshot.camera.vertical_fov_degrees) * .5f);
        const float tx = ty * static_cast<float>(impl_->render_width) / impl_->render_height;
        if (std::abs(center.x) - center.z * tx > radius * std::sqrt(1 + tx*tx) ||
            std::abs(center.y) - center.z * ty > radius * std::sqrt(1 + ty*ty)) continue;
        const float distance = std::max(center.z - radius,.1f);
        const float area = std::min(1.0f, 3.14159265f * radius * radius / (4 * distance * distance * tx * ty));
        distortion_candidates.push_back({&source,area});
    }
    std::stable_sort(distortion_candidates.begin(),distortion_candidates.end(),[](const auto &a,const auto &b) {
        if (a.source->importance != b.source->importance) return a.source->importance > b.source->importance;
        if (a.area != b.area) return a.area > b.area;
        if (a.source->stable_id != b.source->stable_id) return a.source->stable_id < b.source->stable_id;
        return a.source->stable_seed < b.source->stable_seed;
    });
    auto *gpu_distortions = reinterpret_cast<GpuDistortion *>(frame.mapped + distortion_data_offset);
    std::uint32_t distortion_count{};
    float distortion_area{};
    for (const auto &candidate : distortion_candidates)
    {
        const auto &source = *candidate.source;
        const bool medium = source.quality == VfxQuality::Medium;
        if (distortion_count >= (medium ? kMaxDistortions/2 : kMaxDistortions) ||
            distortion_area + candidate.area > impl_->config.distortion_coverage * (medium ? .5f : 1.0f)) continue;
        auto &gpu = gpu_distortions[distortion_count++];
        gpu.position_radius = {source.position.x,source.position.y,source.position.z,source.radius};
        gpu.direction_strength = {source.direction.x,source.direction.y,source.direction.z,source.strength};
        gpu.phase_alpha_shape_seed = {source.normalized_age,source.alpha,static_cast<float>(source.shape),static_cast<float>(source.stable_seed & 0xffffu)};
        gpu.geometry = {source.inner_radius,
                        DirectX::XMConvertToRadians(source.half_angle_degrees),
                        DirectX::XMConvertToRadians(source.gap_half_width_degrees),0.0f};
        gpu.gap_range = {gap_write_index,static_cast<UINT>(source.gap_angles_degrees.size()),0,0};
        for (const float degrees : source.gap_angles_degrees)
            gpu_ground_gaps[gap_write_index++] = DirectX::XMConvertToRadians(degrees);
        distortion_area += candidate.area;
    }
    struct DecalCandidate { const VfxDecalInput *source; float area; };
    std::vector<DecalCandidate> decal_candidates;
    for (const auto &source : typed_decals)
    {
        const auto finite = [](Float3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
        const bool fracture = source.kind == VfxDecalKind::Fracture;
        if (!finite(source.position) || !finite(source.direction) || !finite(source.linear_rgb) ||
            source.linear_rgb.x < 0 || source.linear_rgb.y < 0 || source.linear_rgb.z < 0 ||
            !std::isfinite(source.radius) || source.radius <= 0 || !std::isfinite(source.hdr) || source.hdr < 0 ||
            !std::isfinite(source.alpha) || source.alpha < 0 || source.alpha > 1 || !std::isfinite(source.normalized_age) ||
            source.normalized_age < 0 || source.normalized_age > 1 || !std::isfinite(source.edge_glow) || source.edge_glow < 0 || source.edge_glow > 1 ||
            source.gradient_row >= impl_->vfx_gradient_rows || source.texture_slice >= (fracture ? 4u : 8u) ||
            static_cast<std::uint32_t>(source.kind) > 1 || static_cast<std::uint32_t>(source.quality) > 2 ||
            (fracture && source.voronoi_cells == 0))
            return Result::Failure(ErrorCode::InvalidArgument,"hs_renderer_d3d12","Invalid authored projected decal.");
        if (source.alpha == 0 || impl_->config.slime_family_preview_count != 0) continue;
        // Ground-only projector slab follows its source anchor, including elevated death events.
        DirectX::XMFLOAT3 center;
        DirectX::XMStoreFloat3(&center,DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(source.position.x,source.position.y-.825f,source.position.z,1),view));
        const float radius = std::hypot(source.radius,1.175f);
        const float ty = std::tan(DirectX::XMConvertToRadians(snapshot.camera.vertical_fov_degrees)*.5f);
        const float tx = ty*static_cast<float>(impl_->render_width)/impl_->render_height;
        if (center.z+radius <= .1f || center.z-radius >= 500 ||
            std::abs(center.x)-center.z*tx > radius*std::sqrt(1+tx*tx) ||
            std::abs(center.y)-center.z*ty > radius*std::sqrt(1+ty*ty)) continue;
        const float distance = std::max(center.z,.1f);
        decal_candidates.push_back({&source,std::min(1.0f,source.radius*source.radius/(distance*distance))});
    }
    std::stable_sort(decal_candidates.begin(),decal_candidates.end(),[](const auto &a,const auto &b) {
        if(a.source->importance != b.source->importance) return a.source->importance > b.source->importance;
        if(a.area != b.area) return a.area > b.area;
        if(a.source->stable_id != b.source->stable_id) return a.source->stable_id < b.source->stable_id;
        return a.source->stable_seed < b.source->stable_seed;
    });
    auto *gpu_decals = reinterpret_cast<GpuDecal *>(frame.mapped+decal_data_offset);
    constants->vfx_decal_count = {};
    for(const auto &candidate:decal_candidates)
    {
        const auto &source=*candidate.source;
        const auto limit=source.quality==VfxQuality::Low?kMaxDecals/4:(source.quality==VfxQuality::Medium?kMaxDecals/2:kMaxDecals);
        if(constants->vfx_decal_count.x>=limit) continue;
        auto &gpu=gpu_decals[constants->vfx_decal_count.x++];
        std::uint32_t hash=source.stable_seed*747796405u+2891336453u; hash=(hash^(hash>>16))*2246822519u;
        const float angle=std::atan2(source.direction.z,source.direction.x)+static_cast<float>(hash&0x00ffffffu)*(6.283185307f/16777216.0f);
        gpu.position_radius={source.position.x,source.position.y,source.position.z,source.radius};
        gpu.axis_alpha_age={std::cos(angle),std::sin(angle),source.alpha,source.normalized_age};
        gpu.color_hdr={source.linear_rgb.x,source.linear_rgb.y,source.linear_rgb.z,source.hdr};
        gpu.fracture={source.edge_glow,static_cast<float>(source.voronoi_cells),0,0};
        gpu.slab={source.position.y-2.0f,source.position.y+.35f,0,0};
        gpu.metadata={source.gradient_row,source.texture_slice,static_cast<std::uint32_t>(source.kind),source.stable_seed};
    }
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
    // Local-TRS blending changes foot height even when both clips are grounded.
    // Correct the final skin pose before the instance ground offset. Death uses
    // the cooker's whole-body grounding instead of the standing support surface.
    if (pose.clip != CharacterAnimationClip::Death && !impl_->archer_support_vertices.empty())
    {
        float support_y = std::numeric_limits<float>::max();
        for (const auto &vertex : impl_->archer_support_vertices)
        {
            float y = 0.0f;
            for (std::size_t influence = 0; influence < vertex.bone_weights.size(); ++influence)
            {
                const auto &skin = constants->archer_bones[vertex.bone_indices[influence]];
                y += vertex.bone_weights[influence] *
                     (skin._21 * vertex.position[0] + skin._22 * vertex.position[1] +
                      skin._23 * vertex.position[2] + skin._24);
            }
            support_y = std::min(support_y, y);
        }
        const float correction = -impl_->archer_ground_offset - support_y;
        for (std::uint32_t bone = 0; bone < impl_->archer_bone_count; ++bone)
            constants->archer_bones[bone]._24 += correction;
    }
    constants->render_options = {
        static_cast<float>(particle_capacity), impl_->config.bloom ? 1.0f : 0.0f,
        impl_->config.outline ? 1.0f : 0.0f,
        static_cast<float>(particle_delta_ticks) / 60.0f};
    constants->authored_particle_clock = {static_cast<std::uint32_t>(snapshot.header.tick), 0, 0, 0};
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
                                         ? impl_->archer_ground_offset * source.scale.y
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
    const auto distortion_rtv = rtv_at(12 + back_buffer_index);
    const auto bloom_half_rtv = rtv_at(kBloomRtvOffset);
    const auto bloom_quarter_rtv = rtv_at(kBloomRtvOffset + 1);
    const auto bloom_half_combined_rtv = rtv_at(kBloomRtvOffset + 2);
    const auto temporal_read_index = impl_->temporal_read_index;
    const auto temporal_write_index = temporal_read_index ^ 1u;
    const auto temporal_write_rtv = rtv_at(kTemporalRtvOffset + temporal_write_index);
    const auto dsv = impl_->dsv_heap->GetCPUDescriptorHandleForHeapStart();
    ID3D12DescriptorHeap *descriptor_heaps[] = {impl_->srv_heap.Get()};
    impl_->command_list->SetDescriptorHeaps(1, descriptor_heaps);
    auto texture_table = impl_->srv_heap->GetGPUDescriptorHandleForHeapStart();
    texture_table.ptr += static_cast<UINT64>(back_buffer_index) *
                         kTextureDescriptorCount * impl_->srv_stride;
    auto distortion_table = texture_table;
    distortion_table.ptr += 47ull * impl_->srv_stride;
    impl_->command_list->SetGraphicsRootDescriptorTable(26, distortion_table);
    auto decal_table = texture_table;
    decal_table.ptr += 50ull*impl_->srv_stride;
    impl_->command_list->SetGraphicsRootDescriptorTable(29,decal_table);
    auto bloom_table = texture_table;
    bloom_table.ptr += static_cast<UINT64>(kBloomTextureDescriptorOffset) * impl_->srv_stride;
    impl_->command_list->SetGraphicsRootDescriptorTable(30, bloom_table);
    auto temporal_cpu = impl_->srv_heap->GetCPUDescriptorHandleForHeapStart();
    temporal_cpu.ptr += (static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount +
                         kTemporalTextureDescriptorOffset) * impl_->srv_stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC temporal_view{};
    temporal_view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    temporal_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    temporal_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    temporal_view.Texture2D.MipLevels = 1;
    impl_->device->CreateShaderResourceView(
        impl_->temporal_history[temporal_read_index].resource.Get(), &temporal_view,
        temporal_cpu);
    temporal_cpu.ptr += impl_->srv_stride;
    impl_->device->CreateShaderResourceView(
        impl_->temporal_history[temporal_write_index].resource.Get(), &temporal_view,
        temporal_cpu);
    auto temporal_table = texture_table;
    temporal_table.ptr += static_cast<UINT64>(kTemporalTextureDescriptorOffset) *
                          impl_->srv_stride;
    impl_->command_list->SetGraphicsRootDescriptorTable(32, temporal_table);
    temporal_table.ptr += impl_->srv_stride;
    impl_->command_list->SetGraphicsRootDescriptorTable(33, temporal_table);
    impl_->command_list->SetGraphicsRootShaderResourceView(28,frame.upload.resource->GetGPUVirtualAddress()+decal_data_offset);
    auto character_table = texture_table;
    character_table.ptr += static_cast<UINT64>(kPostTextureDescriptorCount) *
                           impl_->srv_stride;
    impl_->command_list->SetGraphicsRootDescriptorTable(13, character_table);
    auto curve_table = character_table;
    curve_table.ptr += static_cast<UINT64>(kCharacterDescriptorCount + kMonsterPbrDescriptorCount +
        kEnvironmentDescriptorCount + kVfxGradientDescriptorCount + 3) * impl_->srv_stride;
    impl_->command_list->SetGraphicsRootDescriptorTable(24, curve_table);
    impl_->command_list->SetGraphicsRootShaderResourceView(25, frame.upload.resource->GetGPUVirtualAddress() + ground_gap_offset);
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
    const auto distortion_vectors = impl_->graph.CreateTexture(
        {impl_->render_width,impl_->render_height,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET}, "VFX Distortion Vectors");
    const auto ribbon_history = impl_->graph.ImportBuffer({impl_->ribbon_history.resource.Get()}, Access::UnorderedWrite, "RibbonHistory");
    const auto ribbon_previous = impl_->graph.ImportBuffer({impl_->ribbon_previous.resource.Get()}, Access::UnorderedWrite, "RibbonPrevious");
    const auto ribbon_arguments = impl_->graph.ImportBuffer({impl_->ribbon_arguments.resource.Get()}, Access::UnorderedWrite, "RibbonArguments");
    const auto draw_ribbons = [&](bool oit) {
        impl_->command_list->SetPipelineState(oit ? impl_->ribbon_oit_pipeline.Get() : impl_->ribbon_add_pipeline.Get());
        impl_->command_list->SetGraphicsRootShaderResourceView(18, frame.upload.resource->GetGPUVirtualAddress() + ribbon_output_offset);
        impl_->command_list->SetGraphicsRootShaderResourceView(19, impl_->ribbon_history.resource->GetGPUVirtualAddress());
        impl_->command_list->SetGraphicsRootShaderResourceView(20, impl_->ribbon_previous.resource->GetGPUVirtualAddress());
        const auto split = std::ranges::find_if(ribbon_outputs, [](const GpuRibbonOutput &output) {
            return (output.metadata[3] & 1u) != 0;
        });
        const auto first = oit ? static_cast<std::size_t>(split - ribbon_outputs.begin()) : 0;
        const auto count = oit ? ribbon_outputs.size() - first : static_cast<std::size_t>(split - ribbon_outputs.begin());
        if (count != 0)
            impl_->command_list->ExecuteIndirect(impl_->ribbon_draw_signature.Get(), static_cast<UINT>(count),
                impl_->ribbon_arguments.resource.Get(), first * kRibbonArgumentStride, nullptr, 0);
        impl_->command_list->SetGraphicsRoot32BitConstant(6, 0, 0);
    };
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
    const auto temporal_read = impl_->graph.ImportTexture(
        {impl_->temporal_history[temporal_read_index].resource.Get()},
        initial_color_access, "TemporalHistoryRead");
    const auto temporal_write = impl_->graph.ImportTexture(
        {impl_->temporal_history[temporal_write_index].resource.Get()},
        initial_color_access, "TemporalHistoryWrite");
    const auto bloom_half = impl_->graph.ImportTexture(
        {impl_->bloom_half.resource.Get()}, initial_color_access, "BloomHalf");
    const auto bloom_quarter = impl_->graph.ImportTexture(
        {impl_->bloom_quarter.resource.Get()}, initial_color_access, "BloomQuarter");
    const auto bloom_half_combined = impl_->graph.ImportTexture(
        {impl_->bloom_half_combined.resource.Get()}, initial_color_access,
        "BloomHalfCombined");
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
    particle_pass.ReadWrite(ribbon_history, Access::UnorderedWrite);
    particle_pass.ReadWrite(ribbon_previous, Access::UnorderedWrite);
    particle_pass.ReadWrite(ribbon_arguments, Access::UnorderedWrite);
    const auto initialize_particles = !impl_->particles_initialized;
    particle_pass.SetExecute([&](RenderPassContext &context) {
        impl_->command_list->SetPipelineState(impl_->particle_compute_pipeline.Get());
        impl_->command_list->SetComputeRootShaderResourceView(16, impl_->vfx_mesh_atlas.resource->GetGPUVirtualAddress());
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
        dispatch_phase(4, particle_capacity);
        context.UavBarrier(particle_alive_output);
        context.UavBarrier(particle_counters);
        dispatch_phase(5, 1);
        context.UavBarrier(particle_counters);
        context.UavBarrier(indirect_arguments);
        impl_->command_list->SetComputeRootShaderResourceView(17, frame.upload.resource->GetGPUVirtualAddress() + ribbon_update_offset);
        impl_->command_list->SetComputeRootShaderResourceView(18, frame.upload.resource->GetGPUVirtualAddress() + ribbon_output_offset);
        impl_->command_list->SetComputeRootUnorderedAccessView(21, impl_->ribbon_history.resource->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootUnorderedAccessView(22, impl_->ribbon_previous.resource->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootUnorderedAccessView(23, impl_->ribbon_arguments.resource->GetGPUVirtualAddress());
        if (!ribbon_updates.empty())
        {
            impl_->command_list->SetPipelineState(impl_->ribbon_update_pipeline.Get());
            impl_->command_list->Dispatch(static_cast<UINT>(ribbon_updates.size()), 1, 1);
            context.UavBarrier(ribbon_history);
            context.UavBarrier(ribbon_previous);
        }
        if (!ribbon_outputs.empty())
        {
            impl_->command_list->SetPipelineState(impl_->ribbon_args_pipeline.Get());
            impl_->command_list->Dispatch(static_cast<UINT>(ribbon_outputs.size()), 1, 1);
            context.UavBarrier(ribbon_arguments);
        }
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

    auto distortion_pass = impl_->graph.AddPass(kRenderPassNames[4], QueueHint::Direct);
    distortion_pass.Read(gbuffer_position, Access::ShaderRead);
    distortion_pass.Read(gbuffer_normal, Access::ShaderRead);
    distortion_pass.Write(distortion_vectors, Access::RenderTarget);
    distortion_pass.SetExecute([&](RenderPassContext &) {
        constexpr float clear[] = {0,0,0,0};
        impl_->command_list->ClearRenderTargetView(distortion_rtv,clear,0,nullptr);
        if (distortion_count == 0) return;
        impl_->command_list->OMSetRenderTargets(1,&distortion_rtv,FALSE,nullptr);
        impl_->command_list->SetPipelineState(impl_->vfx_distortion_pipeline.Get());
        impl_->command_list->SetGraphicsRootDescriptorTable(5,texture_table);
        impl_->command_list->SetGraphicsRootShaderResourceView(27,frame.upload.resource->GetGPUVirtualAddress()+distortion_data_offset);
        impl_->command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        impl_->command_list->DrawInstanced(6,distortion_count,0,0);
    });

    auto transparent_pass = impl_->graph.AddPass(kRenderPassNames[5], QueueHint::Direct);
    transparent_pass.Read(depth, Access::DepthRead);
    transparent_pass.Read(shadow, Access::ShaderRead);
    transparent_pass.Read(particles, Access::ShaderRead);
    transparent_pass.Read(particle_alive_output, Access::ShaderRead);
    transparent_pass.Read(indirect_arguments, Access::IndirectArgs);
    transparent_pass.Read(ribbon_history, Access::ShaderRead);
    transparent_pass.Read(ribbon_previous, Access::ShaderRead);
    transparent_pass.Read(ribbon_arguments, Access::IndirectArgs);
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
        impl_->command_list->SetGraphicsRootShaderResourceView(16, impl_->vfx_mesh_atlas.resource->GetGPUVirtualAddress());
        impl_->command_list->SetGraphicsRootShaderResourceView(
            2, impl_->particles.resource->GetGPUVirtualAddress());
        impl_->command_list->SetGraphicsRootShaderResourceView(
            12,
            impl_->particle_alive[particle_output_index].resource->GetGPUVirtualAddress());
        impl_->command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (UINT bin = 0; bin < 2; ++bin)
        {
            impl_->command_list->SetGraphicsRoot32BitConstant(6, bin, 0);
            impl_->command_list->ExecuteIndirect(
                impl_->draw_signature.Get(), 1, impl_->indirect_arguments.resource.Get(),
                bin * sizeof(D3D12_DRAW_ARGUMENTS), nullptr, 0);
        }
        impl_->command_list->SetGraphicsRoot32BitConstant(6, 0, 0);
        if (ground_ring_count != 0)
        {
            impl_->command_list->SetPipelineState(impl_->ground_ring_pipeline.Get());
            impl_->command_list->SetGraphicsRootShaderResourceView(
                11, frame.upload.resource->GetGPUVirtualAddress() +
                        ground_ring_data_offset);
            impl_->command_list->DrawInstanced(6, ground_ring_count, 0, 0);
        }

        if (owner_mesh_count != 0)
        {
            impl_->command_list->SetPipelineState(impl_->owner_mesh_pipeline.Get());
            impl_->command_list->SetGraphicsRootShaderResourceView(
                11, frame.upload.resource->GetGPUVirtualAddress() + owner_mesh_data_offset);
            for (std::uint32_t index = 0; index < owner_mesh_count; ++index)
            {
                impl_->command_list->SetGraphicsRoot32BitConstant(6, index, 0);
                impl_->command_list->DrawInstanced(
                    impl_->vfx_mesh_vertex_counts[typed_mesh_spawns[index].mesh_index - 1], 1, 0, 0);
            }
            impl_->command_list->SetGraphicsRoot32BitConstant(6, 0, 0);
        }

        if (flash_count > flash_add_count)
        {
            impl_->command_list->SetPipelineState(impl_->vfx_sprite_oit_pipeline.Get());
            impl_->command_list->SetGraphicsRootShaderResourceView(11,
                frame.upload.resource->GetGPUVirtualAddress() + flash_data_offset + flash_add_count * sizeof(GpuParticleSpawnCommand));
            impl_->command_list->DrawInstanced(6, flash_count - flash_add_count, 0, 0);
        }
        draw_ribbons(true);
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
        if (!fresnel_draws.empty())
        {
            impl_->command_list->SetPipelineState(impl_->fresnel_shell_pipeline.Get());
            impl_->command_list->SetGraphicsRootDescriptorTable(5, texture_table);
            for (const auto &draw : fresnel_draws)
            {
                impl_->command_list->SetGraphicsRootShaderResourceView(
                    1, frame.upload.resource->GetGPUVirtualAddress() + kInstanceDataOffset +
                           draw.instance_index * sizeof(GpuInstance));
                impl_->command_list->SetGraphicsRootShaderResourceView(
                    31, frame.upload.resource->GetGPUVirtualAddress() + fresnel_data_offset +
                            draw.command_index * sizeof(GpuFresnelShell));
                if (draw.archer)
                {
                    impl_->command_list->IASetVertexBuffers(0, 1, &impl_->archer_vertex_view);
                    impl_->command_list->DrawInstanced(impl_->archer_vertex_count, 1, 0, 0);
                }
                else
                {
                    const auto &asset = impl_->monster_assets[draw.asset_index];
                    impl_->command_list->IASetVertexBuffers(0, 1, &asset.vertex_view);
                    impl_->command_list->DrawInstanced(asset.vertex_count, 1, 0, 0);
                }
            }
        }
    });

    auto composite_pass = impl_->graph.AddPass(kRenderPassNames[6], QueueHint::Direct);
    composite_pass.Read(hdr, Access::ShaderRead);
    composite_pass.Read(distortion_vectors, Access::ShaderRead);
    composite_pass.Read(oit_accumulation, Access::ShaderRead);
    composite_pass.Read(oit_revealage, Access::ShaderRead);
    composite_pass.Read(gbuffer_position, Access::ShaderRead);
    composite_pass.Read(gbuffer_normal, Access::ShaderRead);
    composite_pass.Read(ribbon_history, Access::ShaderRead);
    composite_pass.Read(ribbon_previous, Access::ShaderRead);
    composite_pass.Read(ribbon_arguments, Access::IndirectArgs);
    composite_pass.Read(depth, Access::DepthRead);
    composite_pass.Write(post_a, Access::RenderTarget);
    composite_pass.SetExecute([&](RenderPassContext &) {
        constexpr float clear[] = {0, 0, 0, 0};
        impl_->command_list->ClearRenderTargetView(post_a_rtv, clear, 0, nullptr);
        draw_fullscreen(impl_->composite_pipeline.Get(), post_a_rtv);
        if (flash_add_count != 0)
        {
            impl_->command_list->SetPipelineState(impl_->vfx_flash_pipeline.Get());
            impl_->command_list->SetGraphicsRootShaderResourceView(
                11, frame.upload.resource->GetGPUVirtualAddress() + flash_data_offset);
            impl_->command_list->DrawInstanced(6, flash_add_count, 0, 0);
        }
        if (ground_add_count != 0)
        {
            impl_->command_list->OMSetRenderTargets(1, &post_a_rtv, FALSE, &dsv);
            impl_->command_list->SetPipelineState(impl_->ground_add_pipeline.Get());
            impl_->command_list->SetGraphicsRootShaderResourceView(
                11, frame.upload.resource->GetGPUVirtualAddress() + ground_ring_data_offset +
                        (typed_ground_spawns.size() - ground_add_count) *
                            sizeof(GpuParticleSpawnCommand));
            impl_->command_list->DrawInstanced(6, ground_add_count, 0, 0);
        }
        impl_->command_list->OMSetRenderTargets(1, &post_a_rtv, FALSE, &dsv);
        draw_ribbons(false);
    });

    auto temporal_pass = impl_->graph.AddPass(kRenderPassNames[7], QueueHint::Direct);
    temporal_pass.Read(post_a, Access::ShaderRead);
    temporal_pass.Read(hdr, Access::ShaderRead);
    temporal_pass.Read(gbuffer_position, Access::ShaderRead);
    temporal_pass.Read(gbuffer_normal, Access::ShaderRead);
    temporal_pass.Read(temporal_read, Access::ShaderRead);
    temporal_pass.Write(temporal_write, Access::RenderTarget);
    temporal_pass.SetExecute([&](RenderPassContext &) {
        draw_fullscreen(impl_->temporal_pipeline.Get(), temporal_write_rtv);
    });

    const auto half_width = impl_->render_width / 2 + impl_->render_width % 2;
    const auto half_height = impl_->render_height / 2 + impl_->render_height % 2;
    const auto quarter_width = half_width / 2 + half_width % 2;
    const auto quarter_height = half_height / 2 + half_height % 2;
    const auto set_bloom_viewport = [&](std::uint32_t width, std::uint32_t height) {
        const D3D12_VIEWPORT target_viewport{0.0f, 0.0f, static_cast<float>(width),
                                            static_cast<float>(height), 0.0f, 1.0f};
        const D3D12_RECT target_scissor{0, 0, static_cast<LONG>(width),
                                       static_cast<LONG>(height)};
        impl_->command_list->RSSetViewports(1, &target_viewport);
        impl_->command_list->RSSetScissorRects(1, &target_scissor);
    };
    auto bloom_extract_pass = impl_->graph.AddPass(kRenderPassNames[8], QueueHint::Direct);
    bloom_extract_pass.Read(post_a, Access::ShaderRead);
    bloom_extract_pass.Read(temporal_write, Access::ShaderRead);
    bloom_extract_pass.Write(bloom_half, Access::RenderTarget);
    bloom_extract_pass.SetExecute([&](RenderPassContext &) {
        constexpr float clear[] = {0, 0, 0, 0};
        impl_->command_list->ClearRenderTargetView(bloom_half_rtv, clear, 0, nullptr);
        set_bloom_viewport(half_width, half_height);
        draw_fullscreen(impl_->bloom_extract_pipeline.Get(), bloom_half_rtv);
    });

    auto bloom_downsample_pass = impl_->graph.AddPass(kRenderPassNames[9], QueueHint::Direct);
    bloom_downsample_pass.Read(bloom_half, Access::ShaderRead);
    bloom_downsample_pass.Write(bloom_quarter, Access::RenderTarget);
    bloom_downsample_pass.SetExecute([&](RenderPassContext &) {
        constexpr float clear[] = {0, 0, 0, 0};
        impl_->command_list->ClearRenderTargetView(bloom_quarter_rtv, clear, 0, nullptr);
        set_bloom_viewport(quarter_width, quarter_height);
        draw_fullscreen(impl_->bloom_downsample_pipeline.Get(), bloom_quarter_rtv);
    });

    auto bloom_upsample_pass = impl_->graph.AddPass(kRenderPassNames[10], QueueHint::Direct);
    bloom_upsample_pass.Read(bloom_half, Access::ShaderRead);
    bloom_upsample_pass.Read(bloom_quarter, Access::ShaderRead);
    bloom_upsample_pass.Write(bloom_half_combined, Access::RenderTarget);
    bloom_upsample_pass.SetExecute([&](RenderPassContext &) {
        constexpr float clear[] = {0, 0, 0, 0};
        impl_->command_list->ClearRenderTargetView(bloom_half_combined_rtv, clear, 0, nullptr);
        set_bloom_viewport(half_width, half_height);
        draw_fullscreen(impl_->bloom_upsample_pipeline.Get(), bloom_half_combined_rtv);
    });

    auto bloom_pass = impl_->graph.AddPass(kRenderPassNames[11], QueueHint::Direct);
    bloom_pass.Read(post_a, Access::ShaderRead);
    bloom_pass.Read(temporal_write, Access::ShaderRead);
    bloom_pass.Read(bloom_half_combined, Access::ShaderRead);
    bloom_pass.Write(post_b, Access::RenderTarget);
    bloom_pass.SetExecute([&](RenderPassContext &) {
        constexpr float clear[] = {0, 0, 0, 0};
        impl_->command_list->ClearRenderTargetView(post_b_rtv, clear, 0, nullptr);
        impl_->command_list->RSSetViewports(1, &viewport);
        impl_->command_list->RSSetScissorRects(1, &scissor);
        draw_fullscreen(impl_->bloom_pipeline.Get(), post_b_rtv);
    });

    auto tone_map_pass = impl_->graph.AddPass(kRenderPassNames[12], QueueHint::Direct);
    tone_map_pass.Read(post_b, Access::ShaderRead);
    tone_map_pass.Write(post_a, Access::RenderTarget);
    tone_map_pass.SetExecute([&](RenderPassContext &) {
        draw_fullscreen(impl_->tone_map_pipeline.Get(), post_a_rtv);
    });

    auto outline_pass = impl_->graph.AddPass(kRenderPassNames[13], QueueHint::Direct);
    outline_pass.Read(post_a, Access::ShaderRead);
    outline_pass.Read(gbuffer_position, Access::ShaderRead);
    outline_pass.Write(post_b, Access::RenderTarget);
    outline_pass.SetExecute([&](RenderPassContext &) {
        draw_fullscreen(impl_->outline_pipeline.Get(), post_b_rtv);
    });

    auto fxaa_pass = impl_->graph.AddPass(kRenderPassNames[14], QueueHint::Direct);
    fxaa_pass.Read(post_b, Access::ShaderRead);
    fxaa_pass.Write(back_buffer, Access::RenderTarget);
    fxaa_pass.SetExecute([&](RenderPassContext &) {
        impl_->command_list->RSSetViewports(1, &output_viewport);
        impl_->command_list->RSSetScissorRects(1, &output_scissor);
        draw_fullscreen(impl_->fxaa_pipeline.Get(), rtv);
    });

    auto ui_pass = impl_->graph.AddPass(kRenderPassNames[15], QueueHint::Direct);
    ui_pass.Read(ui, Access::ShaderRead);
    ui_pass.ReadWrite(back_buffer, Access::RenderTarget);
    ui_pass.SetExecute([&](RenderPassContext &) {
        (void)events;
        draw_fullscreen(impl_->ui_pipeline.Get(), rtv);
    });

    impl_->graph.SetFinalAccess(particles, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(ribbon_history, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(ribbon_previous, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(ribbon_arguments, Access::UnorderedWrite);
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
    impl_->graph.SetFinalAccess(temporal_read, Access::ShaderRead);
    impl_->graph.SetFinalAccess(temporal_write, Access::ShaderRead);
    impl_->graph.SetFinalAccess(bloom_half, Access::ShaderRead);
    impl_->graph.SetFinalAccess(bloom_quarter, Access::ShaderRead);
    impl_->graph.SetFinalAccess(bloom_half_combined, Access::ShaderRead);
#if defined(HS_DEVELOPMENT_TOOLS)
    impl_->graph.SetFinalAccess(back_buffer, Access::RenderTarget);
#else
    impl_->graph.SetFinalAccess(back_buffer, Access::Present);
#endif

    if (auto prepared = impl_->graph.Prepare(transient_pool); !prepared) return prepared;
    auto *distortion_resource = static_cast<ID3D12Resource *>(impl_->graph.Resolve(distortion_vectors));
    if (!distortion_resource) return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Prepared distortion resource is missing.");
    D3D12_RENDER_TARGET_VIEW_DESC distortion_rtv_description{};
    distortion_rtv_description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    distortion_rtv_description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    impl_->device->CreateRenderTargetView(distortion_resource,&distortion_rtv_description,distortion_rtv);
    D3D12_SHADER_RESOURCE_VIEW_DESC distortion_srv_description{};
    distortion_srv_description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    distortion_srv_description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    distortion_srv_description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    distortion_srv_description.Texture2D.MipLevels = 1;
    auto distortion_cpu = impl_->srv_heap->GetCPUDescriptorHandleForHeapStart();
    distortion_cpu.ptr += (static_cast<SIZE_T>(back_buffer_index)*kTextureDescriptorCount+47)*impl_->srv_stride;
    impl_->device->CreateShaderResourceView(distortion_resource,&distortion_srv_description,distortion_cpu);
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

    impl_->temporal_read_index = temporal_write_index;
    impl_->temporal_history_valid = temporal_enabled;
    impl_->previous_view_projection = constants->view_projection;
    impl_->temporal_previous_eye = {current_temporal_eye.x,
                                    current_temporal_eye.y,
                                    current_temporal_eye.z};
    impl_->temporal_previous_fov = snapshot.camera.vertical_fov_degrees;
    ++impl_->frame_number;
    frame_result = {impl_->frame_number, snapshot.header.tick,
#if defined(HS_DEVELOPMENT_TOOLS)
                    impl_->config.devtools_visible && ImGui::GetIO().WantCaptureMouse,
                    impl_->config.devtools_visible && ImGui::GetIO().WantCaptureKeyboard,
#else
                    false, false,
#endif
                    debug_command, debug_value, debug_secondary,
                    static_cast<std::uint32_t>(typed_vfx_events.size()),
                    static_cast<std::uint32_t>(impl_->typed_vfx_persistent_state.Active().size()),
                    ground_ring_count + ground_add_count, flash_count, owner_mesh_count,
                    static_cast<std::uint32_t>(fresnel_draws.size()),
                    static_cast<std::uint32_t>(ribbon_outputs.size()), impl_->ribbon_state.DroppedSources(), constants->vfx_light_count.x, distortion_count, constants->vfx_decal_count.x};
    impl_->previous_ribbon_eye = ribbon_eye;
    impl_->CountValidationErrors();
    return Result::Success();
}


} // namespace hs
