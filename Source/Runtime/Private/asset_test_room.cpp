#include "asset_test_room.hpp"

#if defined(HS_DEVELOPMENT_TOOLS)
#include <hs/presentation/meadow_surface.hpp>
namespace hs::runtime_detail
{
AssetTestRoom::AssetTestRoom(const std::vector<RendererCharacterAnimationClip> &clips,
        const std::vector<RendererLegacyCharacterAnimationClip> &legacy_clips, const VfxImage &vfx)
    {
        for (const auto &clip : clips)
            entries.push_back({RenderMesh::Archer, 0, {}, clip.id, "Archer / " + clip.name,
                               std::max(clip.duration_seconds, .001f)});
        constexpr const char *legacy[] = {"Idle", "Run", "Draw", "Recoil", "Death"};
        constexpr const char *characters[] = {"Slime melee", "Slime ranged", "Slime suicide",
                                               "Boss five minute", "Boss ten minute", "Boss final"};
        for (const auto &clip : legacy_clips)
            if ((clip.character != 0 || clips.empty()) && clip.character <= std::size(characters) && clip.clip < std::size(legacy))
                entries.push_back({clip.character == 0 ? RenderMesh::Archer : static_cast<RenderMesh>(
                    static_cast<std::uint32_t>(RenderMesh::MonsterMelee) + clip.character - 1),
                    0, static_cast<CharacterAnimationClip>(clip.clip), {},
                    std::string(clip.character == 0 ? "Archer" : characters[clip.character - 1]) + " / " + legacy[clip.clip],
                    std::max(clip.duration_seconds, .001f)});
        // Model stations reuse a real admitted clip for each character rather
        // than assuming that every product exposes an Idle slot.
        const auto animation_count = entries.size();
        for (const auto mesh : {RenderMesh::Archer, RenderMesh::MonsterMelee, RenderMesh::MonsterRanged,
                                RenderMesh::MonsterSuicide, RenderMesh::BossFiveMinute,
                                RenderMesh::BossTenMinute, RenderMesh::BossFinal})
        {
            const auto animations = std::span(entries).first(animation_count);
            auto found = std::ranges::find_if(animations, [mesh](const Entry &entry) {
                return entry.mesh == mesh && (entry.name.find("Idle") != std::string::npos ||
                                               entry.name.find("idle") != std::string::npos);
            });
            if (found == animations.end())
                found = std::ranges::find(animations, mesh, &Entry::mesh);
            if (found == animations.end()) continue;
            auto model = *found; model.group = Group::Model; model.fixed_pose = true;
            model.name = "Model / " + found->name; entries.push_back(std::move(model));
        }
        const auto add_builtin = [&](RenderMesh mesh, const char *name, Float3 position, Float3 scale,
                                     float yaw, std::uint32_t color) {
            Entry entry{}; entry.mesh = mesh; entry.name = name; entry.position = position;
            entry.group = Group::Model;
            entry.scale = scale; entry.yaw = yaw; entry.color = color; entries.push_back(std::move(entry));
        };
        add_builtin(RenderMesh::Pickup, "Pickup / experience", {0,.32f,0}, {.34f,.34f,.34f}, .785398f, 0xffffd040u);
        add_builtin(RenderMesh::Pickup, "Pickup / heal", {0,.32f,0}, {.26f,.55f,.26f}, 0, 0xff40e060u);
        add_builtin(RenderMesh::Pickup, "Pickup / magnet", {0,.32f,0}, {.48f,.48f,.48f}, 0, 0xffff3030u);
        add_builtin(RenderMesh::Pickup, "Pickup / relic", {0,.32f,0}, {.4f,.4f,.4f}, 0, 0xffe080ffu);
        add_builtin(RenderMesh::PlayerProjectile, "Projectile / basic", {0,1.05f,0}, {.24f,.24f,.825f}, 0, 0xff40e8ffu);
        add_builtin(RenderMesh::PlayerProjectile, "Projectile / piercing", {0,1.05f,0}, {.28f,.28f,1.25f}, 0, 0xffffe8a0u);
        add_builtin(RenderMesh::PlayerProjectile, "Projectile / multishot", {0,1.05f,0}, {.18f,.18f,.7f}, 0, 0xffffd878u);
        add_builtin(RenderMesh::PlayerProjectile, "Projectile / charged start", {0,1.05f,0}, {.228f,.228f,.81f}, 0, 0xfffff0c0u);
        add_builtin(RenderMesh::PlayerProjectile, "Projectile / charged", {0,1.05f,0}, {.42f,.42f,1.26f}, 0, 0xfffff0c0u);
        add_builtin(RenderMesh::PlayerProjectile, "Projectile / explosive", {0,1.05f,0}, {.34f,.34f,1}, 0, 0xff188cffu);
        add_builtin(RenderMesh::PlayerProjectile, "Projectile / ricochet", {0,1.05f,0}, {.25f,.25f,.85f}, 0, 0xffffa840u);
        add_builtin(RenderMesh::EnemyProjectile, "Projectile / enemy", {0,.45f,0}, {.24f,.24f,.8f}, 0, 0xff4040ffu);
        add_builtin(RenderMesh::Area, "Area / enemy damage", {0,.01f,0}, {2,.04f,2}, 0, 0x604040ffu);
        add_builtin(RenderMesh::Area, "Area / slow", {0,.01f,0}, {2,.04f,2}, 0, 0x6040a0ffu);
        add_builtin(RenderMesh::Area, "Area / friendly", {0,.01f,0}, {2,.04f,2}, 0, 0x6080d040u);
        add_builtin(RenderMesh::Area, "Area / trail", {0,.01f,0}, {1,.04f,4}, 0, 0x6080d040u);
        for (std::uint32_t ordinal = 0; ordinal < render_data::kEnvironmentMaterialNames.size(); ++ordinal)
        {
            const auto mesh = render_data::EnvironmentMeshForOrdinal(ordinal);
            const std::uint32_t first = ordinal < 3 ? 0 : ordinal < 6 ? 3 : ordinal < 10 ? 6 :
                ordinal < 14 ? 10 : ordinal < render_data::kMeadowGeometryBegin ? 14 : render_data::kMeadowGeometryBegin;
            entries.push_back({mesh, ordinal - first, {}, {}, std::string(render_data::kEnvironmentMaterialNames[ordinal]), 1});
            entries.back().group = Group::Model;
        }
        for (const auto &lookup : vfx.program.effect_lookup)
        {
            if (!lookup.handle || lookup.handle > vfx.program.effects.size())
            {
                Entry invalid{}; invalid.mesh = RenderMesh::Archer; invalid.effect_id = lookup.effect_id;
                invalid.group = Group::Vfx;
                invalid.name = std::format("VFX {:016x} / invalid handle", lookup.effect_id);
                entries.push_back(std::move(invalid));
                continue;
            }
            const auto &effect = vfx.program.effects[lookup.handle - 1];
            Entry entry{}; entry.mesh = RenderMesh::Archer;
            entry.group = Group::Vfx;
            entry.fixed_pose = true;
            if (lookup.handle == vfx.entity_surface_routes.phase_effect || lookup.handle == vfx.entity_surface_routes.death_effect ||
                TestRoomPersistentKind(lookup.effect_id, effect) == PersistentVfxKind::BossPhaseTransition)
                entry.mesh = RenderMesh::BossFinal;
            entry.name = std::format("VFX {:016x} / {}", lookup.effect_id,
                effect.input_mode == 0 ? "event" : TestRoomPersistentKind(lookup.effect_id, effect) ? "owner" : "unmapped owner");
            entry.duration = std::isfinite(effect.seconds) && effect.seconds > 0 ? effect.seconds : 2;
            entry.effect_id = lookup.effect_id; entry.effect_handle = lookup.handle; entry.payload_kind = effect.payload_kind;
            entry.screen_effect = IsVfxTypedScreenEffect(vfx, lookup.handle);
            entry.owner_kind = TestRoomPersistentKind(lookup.effect_id, effect);
            if (entry.owner_kind == PersistentVfxKind::EnemyBleedStatus || entry.owner_kind == PersistentVfxKind::EnemyBurnStatus ||
                entry.owner_kind == PersistentVfxKind::EnemySlowStatus || entry.owner_kind == PersistentVfxKind::EnemyMarkStatus)
                entry.mesh = RenderMesh::MonsterMelee;
            entries.push_back(std::move(entry));
        }
        for (const auto &definition : vfx.definitions)
        {
            if (std::ranges::any_of(vfx.program.effect_lookup,
                    [&](const auto &lookup) { return lookup.effect_id == definition.effect_id; })) continue;
            Entry entry{}; entry.mesh = RenderMesh::Archer; entry.effect_id = definition.effect_id; entry.legacy = true;
            entry.group = Group::Vfx;
            entry.fixed_pose = true;
            entry.name = std::format("VFX {:016x} / legacy", definition.effect_id);
            float lifetime = definition.kind == VfxDefinitionKind::Line ? definition.line_lifetime : 0;
            for (std::size_t index = 0; index < definition.emitter_count; ++index)
            {
                const auto emitter = std::size_t(definition.first_emitter) + index;
                if (emitter < vfx.emitters.size()) lifetime = std::max(lifetime, vfx.emitters[emitter].lifetime_max);
            }
            entry.duration = std::isfinite(lifetime) && lifetime > 0 ? lifetime : 2;
            entries.push_back(std::move(entry));
        }
        // Sort only layout indices: station IDs and all catalog ordinals remain stable.
        float row_front = 3.0f;
        float min_x = 0.0f, max_x = 0.0f, max_z = 0.0f;
        for (unsigned zone = 0; zone < 3; ++zone)
        {
            if (zone == 2) row_front = 3.0f;
            const float zone_center = zone == 2 ? 50.0f : 0.0f;
            const float zone_width = zone == 2 ? 64.0f : 32.0f;
            std::vector<std::size_t> members;
            for (std::size_t i = 0; i < entries.size(); ++i)
            {
                const auto &entry = entries[i];
                // The matching canopy shares its trunk's station footprint.
                if (zone == 1 && entry.mesh == RenderMesh::TreeCanopy) continue;
                const bool character = entry.mesh == RenderMesh::Archer ||
                    (entry.mesh >= RenderMesh::MonsterMelee && entry.mesh <= RenderMesh::BossFinal);
                if ((zone == 0 && entry.group != Group::Vfx && character) ||
                    (zone == 1 && entry.group != Group::Vfx && !character) ||
                    (zone == 2 && entry.group == Group::Vfx)) members.push_back(i);
            }
            const auto profile = [&](const Entry &entry) {
                if (!entry.effect_handle || entry.effect_handle > vfx.program.effects.size()) return 0xffffffffu;
                const auto &effect = vfx.program.effects[entry.effect_handle - 1];
                if (!effect.sources.count || effect.sources.first >= vfx.program.sources.size()) return 0xffffffffu;
                const auto &source = vfx.program.sources[effect.sources.first];
                if (!source.outputs.count || source.outputs.first >= vfx.program.outputs.size()) return 0xffffffffu;
                return static_cast<std::uint32_t>(vfx.program.outputs[source.outputs.first].profile);
            };
            std::stable_sort(members.begin(), members.end(), [&](std::size_t a, std::size_t b) {
                return zone == 2 ? profile(entries[a]) < profile(entries[b]) :
                    static_cast<std::uint32_t>(entries[a].mesh) < static_cast<std::uint32_t>(entries[b].mesh);
            });
            const auto cell_size = [&](const Entry &entry) {
                if (entry.mesh == RenderMesh::MeadowSurface || entry.mesh == RenderMesh::Ground) return 28.0f;
                if (entry.mesh == RenderMesh::BossFiveMinute || entry.mesh == RenderMesh::BossTenMinute ||
                    entry.mesh == RenderMesh::BossFinal) return 9.0f;
                if (entry.mesh == RenderMesh::TreeTrunk || entry.mesh == RenderMesh::TreeCanopy) return 6.0f;
                if (zone == 0) return 3.0f;
                if (zone == 1)
                {
                    if (entry.mesh == RenderMesh::GroundDetail &&
                        (entry.variant == 6 || entry.variant == 13 || entry.variant == 14 ||
                         entry.variant == 15 || entry.variant == 17)) return 4.0f;
                    return 2.5f;
                }
                if (entry.screen_effect) return 9.0f;
                const auto kind = entry.payload_kind;
                const auto hash = VfxRibbonBoundsHash32;
                if (kind == hash("SourceTargetPayload") || kind == hash("ProjectilePathPayload") ||
                    kind == hash("LinePayload") || kind == hash("RingPayload")) return 7.0f;
                const auto output = profile(entry);
                if (output == static_cast<std::uint32_t>(VfxOutputProfile::RibbonAdd) ||
                    output == static_cast<std::uint32_t>(VfxOutputProfile::RibbonOit) ||
                    output == static_cast<std::uint32_t>(VfxOutputProfile::GroundSdfSoft) ||
                    output == static_cast<std::uint32_t>(VfxOutputProfile::GroundSdfAdd) ||
                    output == static_cast<std::uint32_t>(VfxOutputProfile::GroundSdfOit)) return 6.0f;
                return 3.0f;
            };
            for (std::size_t first = 0; first < members.size(); )
            {
                auto last = first;
                float width = 0, depth = 0;
                while (last < members.size())
                {
                    const auto size = cell_size(entries[members[last]]);
                    if (last != first && width + size > zone_width) break;
                    width += size; depth = std::max(depth, size);
                    ++last;
                }
                float x = zone_center - width * .5f;
                min_x = std::min(min_x, x);
                max_x = std::max(max_x, x + width);
                for (auto i = first; i < last; ++i)
                {
                    auto &entry = entries[members[i]];
                    const auto size = cell_size(entry);
                    entry.position.x += x + size * .5f;
                    entry.position.z += row_front + depth * .5f;
                    x += size;
                }
                row_front += depth;
                max_z = std::max(max_z, row_front);
                first = last;
            }
            row_front += 2.0f;
        }
        for (auto &entry : entries)
        {
            if (entry.mesh != RenderMesh::TreeCanopy) continue;
            const auto trunk = std::ranges::find_if(entries, [&](const Entry &candidate) {
                return candidate.mesh == RenderMesh::TreeTrunk && candidate.variant == entry.variant;
            });
            if (trunk != entries.end())
            {
                entry.position = trunk->position;
                entry.scale = trunk->scale;
                entry.yaw = trunk->yaw;
            }
        }
        for (auto &entry : entries)
        {
            if (entry.mesh == RenderMesh::Archer ||
                (entry.mesh >= RenderMesh::MonsterMelee && entry.mesh <= RenderMesh::BossFinal))
            {
                const auto appearance = DefaultCharacterRenderAppearance(entry.mesh);
                entry.scale = appearance.scale; entry.color = appearance.color_rgba;
            }
            if (entry.group != Group::Vfx && (entry.mesh == RenderMesh::Archer ||
                entry.mesh == RenderMesh::MonsterMelee || entry.mesh == RenderMesh::MonsterRanged ||
                entry.mesh == RenderMesh::MonsterSuicide)) entry.capture_distance = 4;
            if (entry.mesh == RenderMesh::Pickup || entry.mesh == RenderMesh::PlayerProjectile ||
                entry.mesh == RenderMesh::EnemyProjectile) entry.capture_distance = 2;
            if (entry.mesh == RenderMesh::MeadowSurface || entry.mesh == RenderMesh::Ground)
                entry.capture_distance = 32;
            if (entry.mesh == RenderMesh::BossFiveMinute || entry.mesh == RenderMesh::BossTenMinute ||
                entry.mesh == RenderMesh::BossFinal || entry.mesh == RenderMesh::TreeTrunk || entry.mesh == RenderMesh::TreeCanopy)
                entry.capture_distance = 18;
        }
        // Meadow ground and water replace the flat floor inside their authored
        // 24 m footprints, just as they do in the normal environment layout.
        // Leave enough ground around edge exhibits for the normal viewing camera.
        constexpr float floor_margin = 16.0f;
        const float floor_left = min_x - floor_margin;
        const float floor_right = max_x + floor_margin;
        const float floor_end = max_z + floor_margin;
        float floor_front = -floor_margin;
        const auto add_floor = [&](float left, float right, float front, float back) {
            if (right <= left || back <= front) return;
            RenderInstance floor{}; floor.mesh = RenderMesh::Ground;
            floor.scale = {right - left, .1f, back - front};
            floor.position = {(left + right) * .5f, -.055f, (front + back) * .5f};
            floor.environment_seed = kMeadowSurfaceSeed;
            floor.stable_id = 0x545354524f4f4d01ull + floors.size();
            PopulateRenderInstanceRevisions(floor);
            floors.push_back(floor);
        };
        std::vector<const Entry *> meadow;
        for (const auto &entry : entries)
            if (entry.mesh == RenderMesh::MeadowSurface) meadow.push_back(&entry);
        std::ranges::sort(meadow, {}, [](const Entry *entry) { return entry->position.z; });
        for (const auto *entry : meadow)
        {
            const float front = entry->position.z - 12.0f;
            const float back = entry->position.z + 12.0f;
            add_floor(floor_left, floor_right, floor_front, front);
            add_floor(floor_left, entry->position.x - 12.0f, front, back);
            add_floor(entry->position.x + 12.0f, floor_right, front, back);
            floor_front = back;
        }
        add_floor(floor_left, floor_right, floor_front, floor_end);
        for (std::size_t index = 0; index < entries.size(); ++index)
        {
            const auto &entry = entries[index];
            if (entry.mesh != RenderMesh::MeadowSurface || entry.variant != 1) continue;
            RenderInstance bed{}; bed.mesh = RenderMesh::MeadowSurface;
            bed.position = entry.position; bed.scale = entry.scale; bed.yaw = entry.yaw;
            bed.color_rgba = entry.color; bed.environment_seed = 41703;
            bed.stable_id = 0x5600000000000000ull | (index + 1);
            PopulateRenderInstanceRevisions(bed);
            floors.push_back(bed);
        }
        station_instances.resize(entries.size());
        last_event_cycles.assign(entries.size(), std::numeric_limits<std::uint64_t>::max());
        last_legacy_cycles = last_event_cycles;
        prepared_cycles = last_event_cycles;
        station_starts.resize(entries.size());
    }

} // namespace hs::runtime_detail
#endif
