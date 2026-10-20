#include <hs/runtime/experiment_spec.hpp>
#include <hs/runtime/save_store.hpp>
#include <hs/runtime/audio_engine.hpp>
#include <hs/runtime/playtest_recording.hpp>
#include <hs/core/cooked_format.hpp>
#include "vfx_catalog.hpp"
#include "runtime_channels.hpp"
#include "session_music.hpp"
#include "camera_pose.hpp"
#include "recording_io.hpp"
#include "window.hpp"
#include "vfx_session_state.hpp"
#include "test_generation_paths.hpp"
#include "vfx_typed_ground_commands.hpp"
#include "vfx_typed_flash_commands.hpp"
#include "vfx_typed_ribbon_commands.hpp"
#include "vfx_typed_ballistic_commands.hpp"
#include "vfx_upgrade_dispatch.hpp"
#include "vfx_typed_frame_adapter.hpp"
#include "vfx_typed_light_commands.hpp"
#include "vfx_typed_distortion_commands.hpp"
#include "vfx_typed_decal_commands.hpp"

#include <Windows.h>
#include <hidusage.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#pragma comment(lib, "dwrite.lib")

#include <array>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <format>
#include <iostream>
#include <iterator>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

void Check(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

std::filesystem::path TestDirectory()
{
    return std::filesystem::temp_directory_path() /
           (L"ProjectHS_runtime_" + std::to_wstring(GetCurrentProcessId()));
}

void WriteCorrupt(const std::filesystem::path &path)
{
    std::ofstream(path, std::ios::trunc) << "{broken";
}

void TestSaveRecovery(const std::filesystem::path &root)
{
    hs::SaveStore store(root);
    hs::ProfileData first;
    first.best_level = 7;
    Check(store.SaveProfile(first).Succeeded(), "first profile save");

    auto second = first;
    second.best_level = 11;
    const auto second_save = store.SaveProfile(second);
    if (!second_save.Succeeded())
        throw std::runtime_error("second profile save: " +
                                 std::string(second_save.Message()));
    Check(std::filesystem::is_regular_file(root / "profile.json.bak"),
          "atomic replacement retains backup");

    WriteCorrupt(root / "profile.json");
    hs::ProfileData recovered;
    Check(store.LoadProfile(recovered).Succeeded() && recovered.best_level == 7,
          "corrupt primary restores valid backup");

    WriteCorrupt(root / "profile.json");
    WriteCorrupt(root / "profile.json.bak");
    hs::ProfileData reset;
    Check(store.LoadProfile(reset).Succeeded() && reset.best_level == 1,
          "two corrupt copies reset profile");
    std::size_t corrupt_count{};
    for (const auto &entry : std::filesystem::directory_iterator(root))
    {
        if (entry.path().extension() == ".corrupt") ++corrupt_count;
    }
    Check(corrupt_count >= 2, "corrupt profile copies are preserved");
}

void TestSettingsPersistence(const std::filesystem::path &root)
{
    hs::SaveStore store(root);
    hs::SettingsData first;
    first.vsync = false;
    first.frame_cap = 120;
    first.master_volume = 0.7f;
    first.skill_virtual_keys = {'W', 'Q', 'E', 'R'};
    Check(store.SaveSettings(first).Succeeded(), "settings save");

    hs::SettingsData loaded;
    Check(store.LoadSettings(loaded).Succeeded() && !loaded.vsync &&
              loaded.frame_cap == 120 && loaded.master_volume == 0.7f &&
              loaded.skill_virtual_keys == first.skill_virtual_keys,
          "screen, audio, and exchanged key settings round trip");

    auto invalid = first;
    invalid.skill_virtual_keys = {'Q', 'Q', 'E', 'R'};
    Check(!store.SaveSettings(invalid), "duplicate action keys are rejected");
}

void TestExperimentSpecRoundTrip(const std::filesystem::path &root)
{
    hs::ExperimentSpec source;
    source.scenario_id = "runtime-integration";
    source.seed = 42;
    source.mode = hs::ExperimentMode::SimulationOnly;
    source.build_hash = "0123456789abcdef";
    source.content_hash = "fedcba9876543210";
    source.timeline_actions.push_back({1, 1, "start_session", "{}"});
    source.probes = {"gameplay.checksum", "ecs.count"};
    source.assertions.push_back({"tick", "/tick", "eq", std::uint64_t{60}, 0.0});
    source.termination = {60, true, true, true};

    const auto path = root / "experiment.json";
    Check(hs::SaveExperimentSpec(path, source).Succeeded(), "save experiment spec");
    hs::ExperimentSpec loaded;
    Check(hs::LoadExperimentSpec(path, loaded).Succeeded(), "load experiment spec");
    Check(loaded.scenario_id == source.scenario_id && loaded.seed == 42 &&
              loaded.timeline_actions.size() == 1 && loaded.termination.maximum_tick == 60,
          "experiment spec round trip");

    source.timeline_actions = {{2, 1, "heal_player", "{\"amount\":5}"},
                               {1, 1, "damage_player", "{\"amount\":10}"}};
    Check(hs::SaveExperimentSpec(path, source).Succeeded() &&
              hs::LoadExperimentSpec(path, loaded).Succeeded() &&
              loaded.timeline_actions.size() == 2 &&
              loaded.timeline_actions[0].sequence == 1 &&
              loaded.timeline_actions[1].sequence == 2,
          "same-tick actions retain sequence order without transport");
    const auto rejected = [&](std::vector<hs::ExperimentTimelineAction> actions) {
        source.timeline_actions = std::move(actions);
        const auto saved = hs::SaveExperimentSpec(path, source);
        return !saved || !hs::LoadExperimentSpec(path, loaded);
    };
    Check(rejected({{1, 1, "damage_player", "{}"}, {1, 2, "heal_player", "{}"}}),
          "duplicate action sequence is rejected");
    Check(rejected({{1, 0, "damage_player", "{}"}}), "elapsed tick zero is rejected");
    Check(rejected({{1, 61, "damage_player", "{}"}}), "action after termination is rejected");
    Check(rejected({{1, 1, "damage_player", "[]"}}), "non-object payload is rejected");
}

void TestProductionVfxDecoding()
{
    hs::VfxCatalog catalog;
    Check(hs::VfxCatalog::Load(
              test_generation::ProductPath("vfx/particle_effects.hsbin"),
              test_generation::ProductPath("vfx/vfx_program.hsbin"), catalog).Succeeded(),
          "production VFX catalog");
    const auto &program = catalog.Program();
    const auto handle = [&](std::string_view id) {
        for (const auto &entry : program.effect_lookup)
            if (entry.effect_id == hs::MakeAssetId(id).value) return entry.handle;
        throw std::runtime_error("missing production VFX recipe");
    };
    hs::VfxEventInput warning;
    warning.effect_handle = handle("particle.boss.shockwave.telegraph");
    warning.geometry_owner_id = 17; warning.event_tick = warning.geometry_start_tick = 100;
    warning.geometry_end_tick = 220;
    hs::VfxRingGapsPayload ring;
    ring.inner_radius = 2; ring.outer_radius = 9; ring.gap_half_width_degrees = 12;
    ring.gap_angles_degrees = {23, 113, 203, 293}; warning.payload = ring;
    const auto ground = hs::runtime_detail::BuildVfxTypedEventGroundCommands(program, std::span(&warning,1),160);
    Check(ground.size() == 2, "both actual cooked shockwave preview outputs decode");
    Check(ground[1].geometry.shape == hs::VfxGroundShape::RingGapsTicks && ground[1].geometry.tick_count == 12,
          "actual cooked tick_count reaches geometry");
    hs::VfxEventInput explosion;
    explosion.effect_handle = handle("particle.common.explosion_large");
    hs::VfxCirclePayload circle; circle.center = {1,.15f,2}; circle.radius = 3; explosion.payload = circle;
    const auto sprites = hs::runtime_detail::BuildVfxTypedFlashCommands(program,std::span(&explosion,1));
    bool flame = false, smoke = false, core = false;
    for (const auto &sprite : sprites)
    {
        flame |= sprite.oit && sprite.shape == hs::VfxImpactShape::Flame;
        core |= !sprite.oit && sprite.shape == hs::VfxImpactShape::NoiseDisc && sprite.size == circle.radius;
        smoke |= sprite.oit && sprite.shape == hs::VfxImpactShape::Smoke6Way &&
            sprite.smoke_frame_count == 32 && sprite.smoke_fps > 0;
    }
    Check(flame && smoke && core, "actual cooked explosion decodes authoritative-size core, flame and six-way smoke");
    const auto shards=hs::runtime_detail::BuildVfxTypedBallisticCommands(program,std::span(&explosion,1));
    Check(shards.size()==8,"actual cooked explosion preserves ballistic burst count");
    for(const auto &shard:shards)
        Check(shard.authored_ballistic && shard.count==1 && shard.mesh_index==3 &&
              shard.authored_bounce==.2f && shard.gravity<0 && shard.authored_hdr>0,
              "actual cooked debris physics and material reach GPU transport");
    const auto hash32=[](std::string_view value){std::uint32_t h=2166136261u;for(unsigned char c:value){h^=c;h*=16777619u;}return h;};
    std::size_t ballistic_outputs=0;
    for(const auto &effect:program.effects)
    {
        if(effect.input_mode!=0||effect.timing_kind!=0)continue;
        hs::VfxEventInput event;event.effect_handle=effect.handle;
        if(effect.payload_kind==hash32("PointEventPayload"))event.payload=hs::VfxPointPayload{};
        else if(effect.payload_kind==hash32("PresentationContextPayload"))event.payload=hs::VfxContextPayload{};
        else if(effect.payload_kind==hash32("CircleAreaPayload")){hs::VfxCirclePayload geometry;geometry.radius=2;event.payload=geometry;}
        else continue;
        std::size_t expected=0;
        for(const auto &source:std::span(program.sources).subspan(effect.sources.first,effect.sources.count))
        {
            if(source.type!=hs::VfxSourceType::BallisticCollision)continue;
            std::uint32_t count=0;
            for(const auto &parameter:std::span(program.parameters).subspan(source.parameters.first,source.parameters.count))
                if(parameter.key==hash32("count")&&parameter.type==hs::VfxParameterType::Int)count=parameter.bits;
            for(const auto &output:std::span(program.outputs).subspan(source.outputs.first,source.outputs.count))
                if(output.profile==hs::VfxOutputProfile::MeshEmissiveOit){expected+=count;++ballistic_outputs;}
        }
        if(expected) Check(hs::runtime_detail::BuildVfxTypedBallisticCommands(program,std::span(&event,1)).size()==expected,
            "production ballistic recipe rejected: handle "+std::to_string(effect.handle));
    }
    Check(ballistic_outputs==56,"production ballistic coverage inventory changed");
    std::size_t light_outputs=0;
    for (const auto &effect:program.effects)
    {
        std::size_t expected=0;
        for(const auto &source:std::span(program.sources).subspan(effect.sources.first,effect.sources.count))
            for(const auto &output:std::span(program.outputs).subspan(source.outputs.first,source.outputs.count))
                expected+=output.profile==hs::VfxOutputProfile::Light;
        if(!expected)continue;
        hs::VfxEventInput input;input.effect_handle=effect.handle;input.sequence=71;
        if(effect.payload_kind==hash32("PointEventPayload"))input.payload=hs::VfxPointPayload{};
        else if(effect.payload_kind==hash32("PresentationContextPayload"))input.payload=hs::VfxContextPayload{};
        else if(effect.payload_kind==hash32("CircleAreaPayload")){hs::VfxCirclePayload p;p.radius=2;input.payload=p;}
        else if(effect.payload_kind==hash32("RingWithGapsPayload")){hs::VfxRingGapsPayload p;p.inner_radius=1;p.outer_radius=3;input.payload=p;}
        else if(effect.payload_kind==hash32("ConePayload")){hs::VfxConePayload p;p.direction={0,0,1};p.range=3;p.half_angle_degrees=40;input.payload=p;}
        std::size_t decoded=0;
        for(hs::Tick tick=0;tick<static_cast<hs::Tick>(std::ceil(effect.seconds*60));++tick)
            decoded=(std::max)(decoded,hs::runtime_detail::BuildVfxTypedLightCommands(program,std::span(&input,1),tick).size());
        Check(decoded==expected,"actual cooked VFX light recipe rejected: handle "+std::to_string(effect.handle));
        light_outputs+=decoded;
    }
    Check(light_outputs==17,"production light coverage inventory changed");
    std::size_t distortion_inventory=0;
    for(const auto &output:program.outputs) distortion_inventory+=output.profile==hs::VfxOutputProfile::Distortion;
    Check(distortion_inventory==38,"full authored distortion inventory changed");
    // RingGaps and Cone remain outside this radial renderer slice.
    std::size_t distortion_outputs=0;
    for(const auto &effect:program.effects)
    {
        if(effect.input_mode!=0 || effect.timing_kind!=0) continue;
        hs::VfxEventInput input;input.effect_handle=effect.handle;input.sequence=72;
        if(effect.payload_kind==hash32("PointEventPayload")) input.payload=hs::VfxPointPayload{};
        else if(effect.payload_kind==hash32("PresentationContextPayload")) input.payload=hs::VfxContextPayload{};
        else if(effect.payload_kind==hash32("CircleAreaPayload")){hs::VfxCirclePayload p;p.radius=2;input.payload=p;}
        else if(effect.payload_kind==hash32("ProjectilePayload")){hs::VfxProjectilePayload p;p.velocity={0,0,5};p.hitbox_radius=.25f;input.payload=p;}
        else continue;
        std::size_t expected=0;
        for(const auto &source:std::span(program.sources).subspan(effect.sources.first,effect.sources.count))
            for(const auto &output:std::span(program.outputs).subspan(source.outputs.first,source.outputs.count))
                expected+=output.profile==hs::VfxOutputProfile::Distortion;
        if(!expected)continue;
        std::size_t decoded=0;
        for(hs::Tick tick=0;tick<static_cast<hs::Tick>(std::ceil(effect.seconds*60));++tick)
            decoded=(std::max)(decoded,hs::runtime_detail::BuildVfxTypedDistortionCommands(program,std::span(&input,1),{},tick).size());
        Check(decoded==expected,"actual cooked distortion recipe rejected: handle "+std::to_string(effect.handle));
        distortion_outputs+=decoded;
    }
    Check(distortion_outputs==35,"supported fixed distortion coverage inventory changed");
    for(const auto &effect:program.effects)
    {
        if(effect.input_mode!=1 || effect.payload_kind!=hash32("ProjectilePayload"))continue;
        std::size_t expected=0;
        for(const auto &source:std::span(program.sources).subspan(effect.sources.first,effect.sources.count))
            for(const auto &output:std::span(program.outputs).subspan(source.outputs.first,source.outputs.count))
                expected+=output.profile==hs::VfxOutputProfile::Distortion;
        if(!expected)continue;
        hs::VfxPersistentInput input;input.stable_id=73;input.effect_handle=effect.handle;
        hs::VfxProjectilePayload payload;payload.velocity={0,0,5};payload.hitbox_radius=.25f;input.payload=payload;
        Check(hs::runtime_detail::BuildVfxTypedDistortionCommands(program,{},std::span(&input,1),10).size()==expected,
            "actual live projectile distortion recipe rejected");
        distortion_outputs+=expected;
    }
    Check(distortion_outputs==36,"supported distortion coverage inventory changed");
    std::size_t decal_outputs=0;
    for(const auto &effect:program.effects)
    {
        std::size_t expected=0;
        for(const auto &source:std::span(program.sources).subspan(effect.sources.first,effect.sources.count))
            for(const auto &output:std::span(program.outputs).subspan(source.outputs.first,source.outputs.count))
                expected+=output.profile==hs::VfxOutputProfile::Decal;
        if(!expected)continue;
        std::size_t decoded=0;
        if(effect.input_mode==0)
        {
            hs::VfxEventInput input;input.effect_handle=effect.handle;input.sequence=74;
            if(effect.payload_kind==hash32("PointEventPayload"))input.payload=hs::VfxPointPayload{};
            else if(effect.payload_kind==hash32("PresentationContextPayload"))input.payload=hs::VfxContextPayload{};
            else if(effect.payload_kind==hash32("CircleAreaPayload")){hs::VfxCirclePayload p;p.radius=2;input.payload=p;}
            else Check(false,"unexpected authored decal payload");
            for(hs::Tick tick=0;tick<static_cast<hs::Tick>(std::ceil(effect.seconds*60));++tick)
                decoded=(std::max)(decoded,hs::runtime_detail::BuildVfxTypedDecalCommands(program,std::span(&input,1),{},tick).size());
        }
        else
        {
            hs::VfxPersistentInput input;input.effect_handle=effect.handle;input.stable_id=75;input.normalized_age=.5f;
            hs::VfxCirclePayload p;p.radius=2;input.payload=p;
            decoded=hs::runtime_detail::BuildVfxTypedDecalCommands(program,{},std::span(&input,1),30).size();
        }
        Check(decoded==expected,"actual cooked decal recipe rejected: handle "+std::to_string(effect.handle));
        decal_outputs+=decoded;
    }
    Check(decal_outputs==15,"production decal coverage inventory changed");
    hs::PresentationEvent mini;
    mini.kind=hs::PresentationKind::Vfx; mini.sequence=902; mini.tick=100;
    mini.upgrade_skill=4; mini.upgrade_index=1; mini.upgrade_stage=2;
    mini.upgrade_cast_id=91; mini.upgrade_owner_id=901;
    mini.geometry.kind=hs::PresentationGeometryKind::Circle; mini.geometry.radius=1.6f;
    mini.geometry.source_id=901; mini.geometry.start_tick=100; mini.geometry.end_tick=160;
    mini.parameters=hs::EncodeVfxParameters({});
    std::vector<hs::PresentationEvent> dispatched;
    Check(hs::runtime_detail::DispatchVfxUpgradeEvents(program,std::span(&mini,1),dispatched).Succeeded(),
          "actual cooked mini-bomb stage dispatch");
    auto typed_mini=hs::runtime_detail::BuildVfxTypedFrameInputs(program,dispatched,{}, {},130);
    Check(typed_mini.events.size()==1 && typed_mini.unsupported_effect_ids.empty(),"mini-bomb Circle owner ingress");
    hs::PersistentVfxVisual mini_owner; mini_owner.kind=hs::PersistentVfxKind::MiniBombWarning;
    mini_owner.stable_id=901; mini_owner.active_tick=100; mini_owner.expires=160; mini_owner.radius=1.6f;
    hs::runtime_detail::RefreshVfxGroundEventOwners(typed_mini.events,std::span(&mini_owner,1),130);
    Check(typed_mini.events.size()==1,"mini-bomb warning survives owner refresh");
    const auto mini_ground=hs::runtime_detail::BuildVfxTypedEventGroundCommands(program,typed_mini.events,130);
    Check(mini_ground.size()==2 && mini_ground[0].stable_id==901 &&
          mini_ground[0].geometry.shape==hs::VfxGroundShape::CirclePreviewBorder &&
          mini_ground[0].geometry.outer_radius==1.6f && mini_ground[1].geometry.tick_count==12,
          "actual cooked mini-bomb boundary and ticks follow gameplay radius");
    hs::runtime_detail::RefreshVfxGroundEventOwners(typed_mini.events,{},131);
    Check(typed_mini.events.empty(),"cancelled mini-bomb warning disappears with owner");
    struct UpgradeFixture { std::uint8_t skill; std::uint8_t index; std::uint8_t stage; const char *effect; bool circle; };
    for (const auto fixture : std::array{
             UpgradeFixture{4, 2, 1, "particle.upgrade.explosive.shard_radial", false},
             UpgradeFixture{4, 4, 3, "particle.upgrade.explosive.blood_burst", false},
             UpgradeFixture{3, 0, 3, "particle.upgrade.charged.end_explosion", true},
             UpgradeFixture{3, 6, 3, "particle.upgrade.charged.burn_explosion", true}})
    {
        hs::PresentationEvent event;
        event.kind=hs::PresentationKind::Vfx; event.sequence=903; event.tick=101;
        event.session_id=11; event.position={2,1.05f,-3}; event.asset=hs::MakeAssetId("particle.common.explosion_large");
        event.upgrade_skill=fixture.skill; event.upgrade_index=fixture.index; event.upgrade_stage=fixture.stage;
        event.upgrade_cast_id=92; event.upgrade_owner_id=902;
        if (fixture.circle)
        {
            event.geometry.kind=hs::PresentationGeometryKind::Circle;
            event.geometry.radius=2.5f; event.geometry.source_id=902;
        }
        std::vector<hs::PresentationEvent> mapped;
        Check(hs::runtime_detail::DispatchVfxUpgradeEvents(program,std::span(&event,1),mapped).Succeeded() &&
              mapped.size()==1 && mapped.front().asset.value==hs::MakeAssetId(fixture.effect).value,
              std::string("actual cooked upgrade ingress mapping: ")+fixture.effect);
    }
    for (const auto id : {"particle.enemy.ranged.impact", "particle.boss.volley.projectile_impact", "particle.skill.charged_shot.impact"})
    {
        hs::VfxEventInput contact; contact.effect_handle=handle(id);
        contact.world_transform[12]=1; contact.world_transform[13]=.45f;
        hs::VfxProjectilePayload geometry; geometry.velocity={12,0,0}; geometry.hitbox_radius=.2f;
        geometry.hitbox_half_extents={.2f,.2f,.2f}; contact.payload=geometry;
        const auto flash=hs::runtime_detail::BuildVfxTypedFlashCommands(program,std::span(&contact,1));
        Check(flash.size()==1 && flash[0].size==.2f && flash[0].direction.x==1 && flash[0].position.x==1,
              "actual cooked projectile contact preserves hitbox and velocity");
        const auto debris=hs::runtime_detail::BuildVfxTypedBallisticCommands(program,std::span(&contact,1));
        const std::size_t expected=std::string_view(id)=="particle.enemy.ranged.impact" ? 5 :
            (std::string_view(id)=="particle.skill.charged_shot.impact" ? 7 : 0);
        Check(debris.size()==expected,"actual cooked projectile ballistic fan count");
    }
    for (const auto id : {"particle.line.ricochet", "particle.line.burn_transfer", "particle.line.relic_chain"})
    {
        hs::VfxPersistentInput owner;owner.stable_id=71;owner.effect_handle=handle(id);owner.normalized_age=.5f;
        hs::VfxLinkPayload link;link.source_position={0,.7f,0};link.target_position={3,.7f,4};link.width=.1f;link.lifetime01=.5f;owner.payload=link;
        const auto ribbons=hs::runtime_detail::BuildVfxTypedRibbonInputs(program,std::span(&owner,1));
        Check(ribbons.size()==1&&ribbons[0].analytic&&ribbons[0].outputs.size()==2,
              "actual cooked link yields one analytic source with both outputs");
    }
}

void TestVfxCatalog()
{
    hs::VfxCatalog catalog;
    const auto particle_path = test_generation::ProductPath("vfx/particle_effects.hsbin");
    const auto program_path = test_generation::ProductPath("vfx/vfx_program.hsbin");
    Check(hs::VfxCatalog::Load(particle_path, program_path, catalog).Succeeded(),
          "load selected VFX catalog");
    Check(catalog.SpriteCount() == 39, "load data-driven VFX sprite registry");
    const auto fixture = TestDirectory() / "vfx_required_program";
    std::filesystem::create_directories(fixture);
    std::filesystem::copy_file(particle_path, fixture / "particle_effects.hsbin",
                               std::filesystem::copy_options::overwrite_existing);
    hs::VfxCatalog rejected;
    Check(!hs::VfxCatalog::Load(fixture / "particle_effects.hsbin", rejected).Succeeded(),
          "missing compiled v4 program rejects runtime catalog");
    WriteCorrupt(fixture / "vfx_program.hsbin");
    Check(!hs::VfxCatalog::Load(fixture / "particle_effects.hsbin", rejected).Succeeded(),
          "corrupt compiled v4 program rejects runtime catalog");
    std::filesystem::remove(fixture / "vfx_program.hsbin");


    hs::PresentationEvent event;
    event.sequence = 42;
    event.tick = 10;
    event.kind = hs::PresentationKind::Vfx;
    event.asset = hs::MakeAssetId("particle.common.hit");
    event.position = {1.0f, 2.0f, 3.0f};
    hs::VfxEventParameters parameters;
    parameters.direction = {1.0f, 0.0f, 0.0f};
    parameters.scale = 2.0f;
    event.parameters = hs::EncodeVfxParameters(parameters);

    std::vector<hs::ParticleSpawnCommand> full;
    std::vector<hs::ParticleSpawnCommand> half;
    std::vector<hs::VfxLineSpawnCommand> lines;
    auto typed_only = event;
    typed_only.asset = hs::MakeAssetId("particle.player.arrow_release");
    Check(catalog.ExpandEvent(typed_only, 1.0f, full, lines).Succeeded() && full.empty() && lines.empty(),
          "typed-only event must not require a legacy emitter");
    typed_only.asset = hs::MakeAssetId("particle.invalid.not_in_either_catalog");
    Check(!catalog.ExpandEvent(typed_only, 1.0f, full, lines).Succeeded(),
          "unknown VFX event must still fail catalog dispatch");
    Check(catalog.ExpandEvent(event, 1.0f, full, lines).Succeeded() && !full.empty(),
          "expand full-quality VFX");
    Check(std::ranges::any_of(full, [](const auto &command) {
              return command.renderer == hs::VfxRenderer::Sprite &&
                     command.primitive == hs::VfxPrimitive::Soft;
          }) &&
              std::ranges::any_of(full, [](const auto &command) {
                  return command.renderer == hs::VfxRenderer::Mesh &&
                         command.primitive == hs::VfxPrimitive::Ember;
              }),
          "hit VFX samples its slash mask and keeps a small spark secondary");
    Check(std::ranges::any_of(full, [](const auto &command) {
              return command.renderer == hs::VfxRenderer::Mesh && command.mesh_index == 2;
          }), "real hit event selects compiled v4 authored shard mesh");
    event.asset = hs::MakeAssetId("particle.basic_attack");
    std::vector<hs::ParticleSpawnCommand> basic_mesh;
    std::vector<hs::VfxLineSpawnCommand> basic_lines;
    Check(catalog.ExpandEvent(event, 1.0f, basic_mesh, basic_lines).Succeeded(), "basic VFX event expands");
    Check(std::ranges::all_of(basic_mesh, [](const auto &command) {
        return command.renderer != hs::VfxRenderer::Mesh || command.primitive != hs::VfxPrimitive::Ember || command.mesh_index != 1;
    }), "basic ember must not become authored arrowhead");
    event.asset = hs::MakeAssetId("particle.common.hit");
    Check(std::ranges::all_of(full, [](const auto &command) {
              return command.renderer == hs::VfxRenderer::Mesh || command.mesh_index == 0;
          }), "authored mesh selection leaves sprite and ground outputs unchanged");
    Check(catalog.ExpandEvent(event, 0.5f, half, lines).Succeeded() && half.size() == full.size(),
          "expand half-quality VFX");
      for (std::size_t index = 0; index < full.size(); ++index)
      {
          Check(half[index].count == (full[index].count + 1) / 2,
                "half-quality emitter count");
          Check(full[index].seed == half[index].seed, "deterministic emitter seed");
      }

      constexpr std::array new_effects{
          "particle.common.player_hit",
          "particle.common.player_death",
          "particle.enemy.melee.windup",
          "particle.enemy.melee.hit",
          "particle.enemy.ranged.release",
          "particle.enemy.suicide.charge",
          "particle.enemy.suicide.explosion",
          "particle.boss.dash.start",
          "particle.boss.dash.impact",
          "particle.boss.volley.release",
          "particle.boss.area.activate",
          "particle.boss.shockwave.release",
          "particle.skill.retreat_shot.move",
          "particle.skill.damage_area.pulse",
          "particle.skill.fire_area.pulse",
          "particle.skill.arrow_rain.impact",
          "particle.skill.arrow_rain.area_pulse",
          "particle.skill.trap.idle",
          "particle.status.slow_apply",
          "particle.status.slow_area",
      };
      for (const auto *effect_id : new_effects)
      {
          event.asset = hs::MakeAssetId(effect_id);
          std::vector<hs::ParticleSpawnCommand> particles;
          std::vector<hs::VfxLineSpawnCommand> effect_lines;
          Check(catalog.FindEffect(event.asset) != nullptr,
                "new VFX exists in cooked catalog");
          Check(catalog.ExpandEvent(event, 1.0f, particles, effect_lines).Succeeded() &&
                    !particles.empty(),
                "new VFX expands into particles");
      }

    event.asset = hs::MakeAssetId("particle.boss.shockwave.release");
    event.geometry.kind = hs::PresentationGeometryKind::RingGaps;
    std::vector<hs::ParticleSpawnCommand> shockwave_accents;
    Check(catalog.ExpandEvent(event, 1.0f, shockwave_accents, lines).Succeeded() &&
        !shockwave_accents.empty() && std::ranges::none_of(shockwave_accents, [](const auto &command) {
            return command.renderer == hs::VfxRenderer::Ground && command.primitive == hs::VfxPrimitive::Ring;
        }), "authoritative wavefront replaces only legacy ring that crosses safe gaps");
    event.geometry = {};
      event.asset = hs::MakeAssetId("particle.line.ricochet");
    Check(!catalog.ExpandEvent(event, 1.0f, full, lines).Succeeded(), "line target required");
    parameters.flags = static_cast<std::uint32_t>(hs::VfxEventFlag::HasTarget);
    parameters.target = {4.0f, 0.0f, 5.0f};
    event.parameters = hs::EncodeVfxParameters(parameters);
    Check(catalog.ExpandEvent(event, 1.0f, full, lines).Succeeded() && lines.size() == 1 &&
              lines.front().sprite != 0 && lines.front().uv_repeat > 1.0f &&
              lines.front().primitive == hs::VfxPrimitive::DashedRicochet,
          "expand line VFX");

    const auto check_visual = [&](std::string_view id, hs::VfxRenderer renderer,
                                  hs::VfxPrimitive primitive) {
        event.asset = hs::MakeAssetId(id);
        std::vector<hs::ParticleSpawnCommand> particles;
        std::vector<hs::VfxLineSpawnCommand> ignored_lines;
        Check(catalog.ExpandEvent(event, 1.0f, particles, ignored_lines).Succeeded() &&
                  std::ranges::any_of(particles, [&](const auto &command) {
                      return command.renderer == renderer && command.primitive == primitive;
                  }),
              "VFX renderer and primitive survive content cook");
    };
    const auto check_composition = [&](std::string_view id, bool needs_ring,
                                       bool forbids_ring) {
        event.asset = hs::MakeAssetId(id);
        std::vector<hs::ParticleSpawnCommand> particles;
        std::vector<hs::VfxLineSpawnCommand> ignored_lines;
        Check(catalog.ExpandEvent(event, 1.0f, particles, ignored_lines).Succeeded(),
              "composition VFX expands");
        const bool has_ring = std::ranges::any_of(particles, [](const auto &command) {
            return command.renderer == hs::VfxRenderer::Ground &&
                   command.primitive == hs::VfxPrimitive::Ring;
        });
        Check((!needs_ring || has_ring) && (!forbids_ring || !has_ring),
              "VFX primary ring hierarchy");
    };
    check_composition("particle.common.explosion_small", true, false);
    check_composition("particle.common.explosion_large", true, false);
    check_composition("particle.common.heavy_hit", false, true);
    check_composition("particle.skill.explosive_arrow.main", true, false);
    check_composition("particle.skill.explosive_arrow.secondary", true, false);
    parameters.scale = 1.0f;
    event.parameters = hs::EncodeVfxParameters(parameters);
    const auto expanded = [&](std::string_view id) {
        event.asset = hs::MakeAssetId(id);
        std::vector<hs::ParticleSpawnCommand> particles;
        std::vector<hs::VfxLineSpawnCommand> ignored_lines;
        Check(catalog.ExpandEvent(event, 1.0f, particles, ignored_lines).Succeeded(),
              "hierarchy VFX expands");
        return particles;
    };
    const auto pull = expanded("particle.common.pull");
    Check(std::ranges::any_of(pull, [](const auto &command) {
              return command.primitive == hs::VfxPrimitive::Chevron &&
                     command.shape_extent.x >= 0.85f &&
                     command.shape_extent.x <= 1.0f &&
                     command.shape_extent.z >= 0.85f &&
                     command.shape_extent.z <= 1.0f &&
                     command.start_size_max <= 0.1f;
          }),
          "pull chevrons stay normalized at the physical boundary");
    const auto small_explosion = expanded("particle.common.explosion_small");
    Check(std::ranges::any_of(small_explosion, [](const auto &command) {
              return command.renderer == hs::VfxRenderer::Sprite &&
                     command.primitive == hs::VfxPrimitive::Flame;
          }) &&
              std::ranges::any_of(small_explosion, [](const auto &command) {
                  return command.primitive == hs::VfxPrimitive::Shard;
              }),
          "small explosion keeps flash and limited shards");
    const auto heavy_hit = expanded("particle.common.heavy_hit");
    Check(std::ranges::any_of(heavy_hit, [](const auto &command) {
              return command.renderer == hs::VfxRenderer::Sprite &&
                     command.primitive == hs::VfxPrimitive::Soft &&
                     command.count == 1 && command.speed_min == 0.0f;
          }) &&
              std::ranges::any_of(heavy_hit, [](const auto &command) {
                  return command.primitive == hs::VfxPrimitive::Shard &&
                         command.count >= 4 && command.count <= 7;
              }),
          "heavy hit keeps a central fracture and radial shards");
    const auto large_explosion = expanded("particle.common.explosion_large");
    Check(std::ranges::any_of(large_explosion, [](const auto &command) {
              return command.renderer == hs::VfxRenderer::Sprite &&
                     command.primitive == hs::VfxPrimitive::Flame;
          }) &&
              std::ranges::any_of(large_explosion, [](const auto &command) {
                  return command.primitive == hs::VfxPrimitive::Shard;
              }),
          "large explosion keeps flash and shards");
    const auto bleed_apply = expanded("particle.status.bleed_apply");
    const auto bleed_tick = expanded("particle.status.bleed_tick");
    Check(bleed_apply.size() > bleed_tick.size(), "bleed apply is stronger than tick");
    Check(expanded("particle.common.heal").size() >= 2,
          "heal keeps cross and upward motes");
    check_visual("particle.status.slow_area", hs::VfxRenderer::Ground,
                 hs::VfxPrimitive::Rune);
    check_visual("particle.skill.explosive_arrow.main", hs::VfxRenderer::Ground,
                 hs::VfxPrimitive::Ring);
    check_visual("particle.skill.retreat_shot.move", hs::VfxRenderer::Segment,
                 hs::VfxPrimitive::DashWake);
    check_visual("particle.status.burn_apply", hs::VfxRenderer::Sprite,
                 hs::VfxPrimitive::Flame);
}

void TestAudioCatalogValidation(const std::filesystem::path &root)
{
    hs::SettingsData settings;
    const auto write_catalog = [&](std::string_view name, std::string_view entry) {
        const auto directory = root / "audio" / name;
        std::filesystem::create_directories(directory);
        std::ofstream(directory / "audio_cues.json") << entry;
        return directory;
    };
    const auto unsafe_directory = write_catalog(
        "unsafe",
        R"({"entries":[{"id":"audio.test","asset_id":"audio.test","submix":"SFX","spatial":false,"preload":true,"streaming":false,"encoding":"PCM","loop":false,"priority":"Other","max_simultaneous":1,"minimum_retrigger_ms":0,"files":["../escape.wav"]}]})");
    hs::AudioEngine unsafe_engine;
    Check(!unsafe_engine.Initialize(settings, unsafe_directory),
          "audio catalog rejects unsafe paths before device setup");

    const auto streaming_directory = write_catalog(
        "streaming",
        R"({"entries":[{"id":"audio.test","asset_id":"audio.test","submix":"SFX","spatial":false,"preload":true,"streaming":true,"encoding":"PCM","loop":false,"priority":"Other","max_simultaneous":1,"minimum_retrigger_ms":0,"files":["test.wav"]}]})");
    hs::AudioEngine streaming_engine;
    Check(!streaming_engine.Initialize(settings, streaming_directory),
          "audio catalog rejects unsupported streaming before device setup");
}

void TestAudioPayloadLazyLoad(const std::filesystem::path &root)
{
    const auto directory = root / "audio" / "lazy";
    std::filesystem::create_directories(directory);
    std::ofstream wav(directory / "lazy.wav", std::ios::binary);
    const auto write_u16 = [&](std::uint16_t value) {
        const std::array<char, 2> bytes{static_cast<char>(value & 0xFFu),
                                        static_cast<char>((value >> 8) & 0xFFu)};
        wav.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    };
    const auto write_u32 = [&](std::uint32_t value) {
        const std::array<char, 4> bytes{static_cast<char>(value & 0xFFu),
                                        static_cast<char>((value >> 8) & 0xFFu),
                                        static_cast<char>((value >> 16) & 0xFFu),
                                        static_cast<char>((value >> 24) & 0xFFu)};
        wav.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    };
    wav.write("RIFF", 4);
    write_u32(40);
    wav.write("WAVEfmt ", 8);
    write_u32(16);
    write_u16(1);
    write_u16(1);
    write_u32(48000);
    write_u32(144000);
    write_u16(3);
    write_u16(24);
    wav.write("data", 4);
    write_u32(3);
    const std::array<char, 3> payload{};
    wav.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    wav.put('\0');
    wav.close();
    std::ofstream(directory / "audio_cues.json")
        << R"({"entries":[{"id":"audio.lazy","asset_id":"audio.lazy","submix":"SFX","spatial":false,"preload":false,"streaming":false,"encoding":"PCM","loop":false,"priority":"Other","max_simultaneous":1,"minimum_retrigger_ms":0,"files":["lazy.wav"]}]})";

    hs::AudioEngine engine;
    hs::SettingsData settings;
    const auto initialized = engine.Initialize(settings, directory);
    Check(initialized.Succeeded(), "lazy audio catalog validates WAV metadata at startup");
    auto status = engine.Status();
    Check(status.loaded_cues == 1 && status.loaded_files == 0,
          "preload false leaves PCM payload unloaded");

    settings.master_volume = 0.7f;
    engine.ApplySettings(settings);
    engine.SetBackgroundMuted(true);
    Check(engine.Status().background_muted && engine.Status().effective_master_volume == 0.0f,
          "background mute silences the master bus including UI and music");
    settings.master_volume = 0.4f;
    engine.ApplySettings(settings);
    engine.PauseCombat();
    engine.ResumeCombat();
    Check(engine.Status().effective_master_volume == 0.0f && settings.master_volume == 0.4f,
          "settings and combat pause changes cannot unmute background audio");
    engine.SetBackgroundMuted(false);
    Check(!engine.Status().background_muted && engine.Status().effective_master_volume == 0.4f,
          "foreground restores the latest configured master volume");
    engine.SetBackgroundMuted(true);
    settings.master_volume = 0.0f;
    engine.ApplySettings(settings);
    engine.SetBackgroundMuted(false);
    Check(engine.Status().effective_master_volume == 0.0f,
          "returning to foreground preserves explicit user mute");

    hs::PresentationEvent missing_event;
    missing_event.kind = hs::PresentationKind::Audio;
    missing_event.asset = hs::MakeAssetId("audio.missing");
    engine.Play(missing_event);
    Check(engine.Status().skipped_missing_cues == 1,
          "missing audio cue increments diagnostics");

    hs::PresentationEvent event;
    event.kind = hs::PresentationKind::Audio;
    event.asset = hs::MakeAssetId("audio.lazy");
    event.sequence = 1;
    engine.Play(event);
    status = engine.Status();
    Check(status.loaded_files == 1 && status.skipped_load_failures == 0,
          "first Play loads and caches a lazy PCM payload");
    engine.Play(event);
    Check(engine.Status().loaded_files == 1,
          "cached PCM payload is not loaded again");
}

void TestPlaytestRecordAndReplay(const std::filesystem::path &root)
{
    const auto directory = root / "playtest";
    hs::PlaytestRecorder recorder;
    Check(recorder.Start({directory, 42, 99, 101}).Succeeded(), "start playtest recorder");
    const std::array edges{
        hs::ActionEdge{1, hs::GameAction::SkillQ, hs::EdgeKind::Pressed},
        hs::ActionEdge{2, hs::GameAction::CharacterPage, hs::EdgeKind::Pressed},
        hs::ActionEdge{3, hs::GameAction::CharacterPage, hs::EdgeKind::Released}};
    const std::array ui_actions{
        hs::UiAction{hs::UiActionKind::SwapLoadoutSlots, 0, 1}};
    hs::InputFrame input{1, {}, edges, ui_actions};
    hs::SimulationObservation probe;
    probe.tick = 1;
    probe.phase = hs::SessionPhase::Playing;
    probe.balance.skill_uses[1] = 1;
    probe.balance.skill_casts_with_hit[1] = 1;
    probe.skill_levels[1] = 2;
    probe.upgrade_masks[1] = 1;
    probe.balance.upgrade_damage[1][0] = 17;
    probe.balance.upgrade_triggers[1][0] = 2;
    probe.balance.upgrade_effects[1][0][static_cast<std::size_t>(
        hs::UpgradeEffectMetric::ProjectilesCreated)] = 3;
    probe.balance.upgrade_effects[1][0][static_cast<std::size_t>(
        hs::UpgradeEffectMetric::CooldownTicksSaved)] = 12;
    probe.balance.upgrade_relic_synergy_count = 1;
    probe.balance.relic_kills[static_cast<std::size_t>(hs::RelicKind::BurnPropagation)] = 4;
    auto &synergy = probe.balance.upgrade_relic_synergies[0];
    synergy.skill = hs::SkillKind::PiercingShot;
    synergy.upgrade = 0;
    synergy.relic = hs::RelicKind::BurnPropagation;
    synergy.metrics[static_cast<std::size_t>(
        hs::UpgradeRelicSynergyMetric::Damage)] = 23;
    probe.damage_by_skill[1] = 30;
    probe.damage_dealt = 30;
    probe.balance.skill_boss_damage[1] = 17;
    probe.stat_points[static_cast<std::size_t>(hs::StatKind::AttackPower)] = 2;
    probe.balance.stat_utility[
        static_cast<std::size_t>(hs::StatKind::AttackPower)] = 6;
    Check(recorder.Record(input, 1234, probe, {}).Succeeded(), "record playtest tick");

    probe.tick = 2;
    probe.phase = hs::SessionPhase::Defeat;
    probe.health = 0;
    const hs::InputFrame terminal_input{2, {}, {}};
    Check(recorder.Record(terminal_input, 2234, probe, {}).Succeeded(),
          "record terminal playtest tick");
    probe.tick = 3;
    probe.phase = hs::SessionPhase::MainMenu;
    probe.health = 100;
    const hs::InputFrame menu_input{3, {}, {}};
    Check(recorder.Record(menu_input, 3234, probe, {}).Succeeded(),
          "record post-run menu tick");
    Check(recorder.Finish(true).Succeeded(), "finish playtest recorder");
    Check(recorder.Finish(true).Succeeded() && !recorder.Active(),
          "successful finalization remains complete on repetition");
    for (const auto name : {"metadata.json", "inputs.ndjson", "timeline.csv",
                            "events.ndjson", "analysis.md", "result.json"})
    {
        const auto file = CreateFileW((directory / name).c_str(), GENERIC_READ, 0,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "finalized recording releases every file handle");
        CloseHandle(file);
    }
    Check(std::filesystem::is_regular_file(directory / "result.json") &&
              std::filesystem::is_regular_file(directory / "analysis.md"),
          "playtest reports exist");
    std::ifstream result_stream(directory / "result.json");
    const std::string result_json((std::istreambuf_iterator<char>(result_stream)), {});
    Check(result_json.contains("\"build\"") && result_json.contains("\"skills\"") &&
              result_json.contains("\"upgrades\"") &&
              result_json.contains("\"relics\"") &&
              result_json.contains("\"enemies\"") &&
              result_json.contains("\"pickups\"") &&
              result_json.contains("\"stats\"") &&
              result_json.contains("\"damage\": 17") &&
              result_json.contains("\"damage_triggers\": 2") &&
              result_json.contains("\"projectiles_created\"") &&
               result_json.contains("\"cooldown_ticks_saved\"") &&
               result_json.contains("\"value\": 12") &&
               result_json.contains("\"upgrade_relic_synergies\"") &&
               result_json.contains("\"burn_spread_on_kill\"") &&
               result_json.contains("\"kills\": 4") &&
               result_json.contains("\"damage\": 23"),
          "playtest result contains balance metrics");
    Check(result_json.contains("\"outcome\": \"defeat\"") &&
              result_json.contains("\"tick\": 2"),
          "first terminal result survives a later menu transition");
    Check(result_json.contains("\"damage_to_normal\": 13") &&
              result_json.contains("\"damage_to_boss\": 17") &&
              result_json.contains("\"boss\": 17"),
          "skill damage is split by normal and boss targets");
    Check(result_json.contains("\"per_point_utility\": 0.1") &&
              result_json.contains("\"unit\": \"session_contribution_ratio\""),
          "stat utility uses a comparable per-point session contribution ratio");
    std::ifstream analysis_stream(directory / "analysis.md");
    const std::string analysis((std::istreambuf_iterator<char>(analysis_stream)), {});
    Check(analysis.contains("piercing_shot / 강화 1: 17 피해 (2회)"),
          "playtest analysis separates damage caused by each selected upgrade");
    Check(analysis.contains("추가 투사체 3") &&
              analysis.contains("쿨타임 단축 틱 12"),
          "playtest analysis explains each realized non-damage upgrade effect");
    Check(analysis.contains("attack_power: 10.00% / 포인트"),
          "UTF-8 analysis reports normalized stat utility in Korean");
    hs::PlaytestReplay replay;
    Check(hs::LoadPlaytestReplay(directory, replay).Succeeded() &&
              replay.header.format_version == 4 &&
              replay.header.simulation_version == hs::kSimulationVersion &&
              replay.header.gameplay_hash_version == hs::kGameplayHashVersion &&
              replay.header.determinism_profile == hs::kDeterminismProfile &&
              replay.header.seed == 42 && replay.header.simulation_rules_hash == 99 &&
              replay.frames.size() == 3 && replay.frames[0].expected_checksum == 1234 &&
              replay.frames[0].ui_actions.size() == 1 &&
              replay.frames[0].ui_actions[0] == ui_actions[0] &&
              replay.frames[2].expected_checksum == 3234,
          "playtest inputs replay exactly");
    Check(replay.frames[0].ordered_edges.size() == edges.size(),
          "replay retains character-page input edges");
    for (std::size_t i = 0; i < edges.size(); ++i)
    {
        const auto &actual = replay.frames[0].ordered_edges[i];
        Check(actual.sequence == edges[i].sequence && actual.action == edges[i].action &&
                  actual.kind == edges[i].kind,
              "replay preserves action order, kind, and sequence");
    }

    std::ifstream metadata_stream(directory / "metadata.json");
    const std::string metadata((std::istreambuf_iterator<char>(metadata_stream)), {});
    metadata_stream.close();
    const auto reject_version = [&](std::string_view field, std::uint32_t current,
                                    std::uint32_t previous) {
        auto invalid = metadata;
        const auto original = std::format("\"{}\": {}", field, current);
        const auto position = invalid.find(original);
        Check(position != std::string::npos, "find recorded replay version");
        invalid.replace(position, original.size(), std::format("\"{}\": {}", field, previous));
        {
            std::ofstream output(directory / "metadata.json", std::ios::trunc);
            output << invalid;
            Check(output.good(), "write obsolete replay version");
        }
        const auto result = hs::LoadPlaytestReplay(directory, replay);
        Check(!result && result.Message().find("incompatible") != std::string_view::npos,
              "reject obsolete replay version before loading its input frames");
        Check(replay.header.simulation_version == hs::kSimulationVersion &&
                  replay.header.gameplay_hash_version == hs::kGameplayHashVersion &&
                  replay.frames.size() == 3 && replay.frames[0].expected_checksum == 1234 &&
                  replay.frames[0].ordered_edges[1].action == hs::GameAction::CharacterPage,
              "obsolete replay version preserves the previous valid output");
    };
    reject_version("simulation_version", hs::kSimulationVersion, 4);
    reject_version("gameplay_hash_version", hs::kGameplayHashVersion, 6);
    {
        std::ofstream output(directory / "metadata.json", std::ios::trunc);
        output << metadata;
        Check(output.good(), "restore current replay metadata for malformed input tests");
    }

    std::ifstream input_stream(directory / "inputs.ndjson");
    const std::string trace((std::istreambuf_iterator<char>(input_stream)), {});
    input_stream.close();
    const auto reject_input = [&](std::string_view original, std::string_view replacement) {
        auto invalid = trace;
        const auto position = invalid.find(original);
        Check(position != std::string::npos, "find recorded field for corruption test");
        invalid.replace(position, original.size(), replacement);
        {
            std::ofstream output(directory / "inputs.ndjson", std::ios::trunc);
            output << invalid;
            Check(output.good(), "write malformed replay input");
        }
        Check(!hs::LoadPlaytestReplay(directory, replay), "reject invalid replay input");
        Check(replay.frames.size() == 3 && replay.frames[0].expected_checksum == 1234 &&
                  replay.frames[0].ordered_edges[1].action == hs::GameAction::CharacterPage,
              "failed replay load preserves the previous valid recording");
    };
    for (const auto value : {"7", "256", "-1", "1.5", "18446744073709551615"})
        reject_input("\"action\":6", std::string("\"action\":") + value);
    for (const auto value : {"2", "256", "-1", "0.5"})
        reject_input("\"kind\":0", std::string("\"kind\":") + value);
    for (const auto value : {"256", "-1", "0.5", "18446744073709551615"})
    {
        reject_input("\"value\":0", std::string("\"value\":") + value);
        reject_input("\"secondary\":1", std::string("\"secondary\":") + value);
    }
    for (const auto value : {"8", "256", "-1", "0.5"})
        reject_input("\"kind\":7", std::string("\"kind\":") + value);
}

void TestRecordingFileFinalization(const std::filesystem::path &root)
{
    const auto path = root / "recording-finalize.txt";
    std::filesystem::create_directories(root);
    std::ofstream normal(path);
    normal << "last partial batch";
    Check(hs::runtime_detail::FinishRecordingFile(normal) && !normal.is_open(),
          "normal finalization flushes and closes the real file");
    std::ifstream saved(path);
    const std::string text((std::istreambuf_iterator<char>(saved)), {});
    Check(text == "last partial batch", "partial buffered data survives finalization");
    saved.close();

    struct FailedFlush : std::streambuf { int sync() override { return -1; } } failed;
    for (bool exceptions : {false, true})
    {
        std::ofstream file(path);
        std::ostream &stream = file;
        auto *original = stream.rdbuf(&failed);
        if (exceptions) stream.exceptions(std::ios::badbit | std::ios::failbit);
        Check(!hs::runtime_detail::FinishRecordingFile(file) && !file.is_open(),
              "flush failure propagates while the owned file still closes");
        stream.rdbuf(original);

        std::ofstream closed(path);
        closed.close();
        if (exceptions) closed.exceptions(std::ios::badbit | std::ios::failbit);
        Check(!hs::runtime_detail::FinishRecordingFile(closed),
              "close failure cannot return success");
    }
}

void TestPlaytestFinalizationFailures(const std::filesystem::path &root)
{
    // An occupied report path is a real file-open failure, independent of ACLs.
    for (const auto blocked : {"analysis.md", "result.json"})
    {
        const auto directory = root / (std::string("playtest-failure-") + blocked);
        hs::PlaytestRecorder recorder;
        Check(recorder.Start({directory, 42, 99, 101}).Succeeded(),
              "start recorder for report failure");
        hs::SimulationObservation probe;
        probe.tick = 1;
        probe.phase = hs::SessionPhase::Playing;
        Check(recorder.Record({1, {}, {}}, 1234, probe, {}).Succeeded(),
              "buffer a partial batch before finalization");
        std::filesystem::create_directory(directory / blocked);
        Check(!recorder.Finish(true), "report file-open failure rejects finalization");
        if (std::string_view(blocked) == "analysis.md")
        {
            std::ifstream input(directory / "result.json");
            const std::string receipt((std::istreambuf_iterator<char>(input)), {});
            Check(receipt.contains("\"execution_valid\": false"),
                  "failed analysis cannot publish a valid recording receipt");
        }
        Check(!recorder.Finish(true), "failed finalization cannot later return success");
        Check(!recorder.Active(), "failed finalization ends recording admission");
    }
    const auto directory = root / "playtest-metadata-failure";
    std::filesystem::create_directories(directory / "metadata.json");
    hs::PlaytestRecorder recorder;
    Check(!recorder.Start({directory, 42, 99, 101}),
          "metadata file-open failure rejects recording admission");
    Check(!recorder.Active(), "missing metadata never admits recording");
}

void TestSessionMusicTransitions()
{
    const auto menu = hs::MakeAssetId("audio.bgm.main_menu");
    const auto session = hs::MakeAssetId("audio.bgm.session");
    const auto boss = hs::MakeAssetId("audio.bgm.final_boss");
    Check(hs::ResolveSessionMusic(hs::SessionPhase::Playing, menu).value == session.value &&
              hs::ResolveSessionMusic(hs::SessionPhase::Playing, {}).value == session.value,
          "new sessions start session music after menu or a terminal stinger");
    for (const auto track : {session, boss})
    {
        for (const auto phase : {hs::SessionPhase::Paused, hs::SessionPhase::CardSelection,
                                 hs::SessionPhase::StatAllocation, hs::SessionPhase::RelicSelection})
        {
            const auto temporary = hs::ResolveSessionMusic(phase, track);
            Check(temporary.value == track.value &&
                      hs::ResolveSessionMusic(hs::SessionPhase::Playing, temporary).value == track.value,
                  "temporary pages and resume preserve the current session or boss music");
        }
    }
    Check(hs::ResolveSessionMusic(hs::SessionPhase::MainMenu, boss).value == menu.value &&
              hs::ResolveSessionMusic(hs::SessionPhase::Victory, boss).value ==
                  hs::MakeAssetId("audio.stinger.victory").value &&
              hs::ResolveSessionMusic(hs::SessionPhase::Defeat, boss).value ==
                  hs::MakeAssetId("audio.stinger.defeat").value,
          "menu and terminal outcomes replace boss music with their own cue");
}

void TestCameraPoseContinuity()
{
    const auto at80 = hs::ComputeRuntimeCameraPose(28.0f, 55.0f, 80.0f);
    Check(std::abs(at80.distance_m - 22.4f) < 0.001f && at80.pitch_degrees == 55.0f && at80.target_height_m == 0.0f,
          "camera preserves authored 80 percent pose");
    auto previous = hs::ComputeRuntimeCameraPose(28.0f, 55.0f, 80.0f);
    for (float zoom = 75.0f; zoom >= 15.0f; zoom -= 1.0f)
    {
        const auto pose = hs::ComputeRuntimeCameraPose(28.0f, 55.0f, zoom);
        Check(pose.distance_m < previous.distance_m && pose.pitch_degrees < previous.pitch_degrees &&
                  pose.target_height_m > previous.target_height_m,
              "close camera pose changes monotonically");
        previous = pose;
    }
    const auto close = hs::ComputeRuntimeCameraPose(28.0f, 55.0f, 15.0f);
    Check(std::abs(close.distance_m - 4.2f) < 0.001f && close.pitch_degrees == 18.0f && close.target_height_m == 1.0f,
          "close camera reaches third person endpoint");
}

void TestVfxSessionReset()
{
    hs::runtime_detail::VfxSessionState state;
    hs::PresentationEvent old_event;
    old_event.session_id = 1;
    old_event.tick = 100;
    old_event.sequence = 9;
    auto next_event = old_event;
    next_event.session_id = 2;
    next_event.tick = 2;
    next_event.sequence = 10;
    state.pending_events = {old_event, next_event};
    Check(state.Synchronize(1), "first VFX session begins");
    Check(state.TakeReady(99).empty(), "future tick or session event played early");
    const auto first = state.TakeReady(100);
    Check(first.size() == 1 && first[0].sequence == 9, "current VFX event was lost");
    Check(state.MarkPersistentBurst(77, 3), "first persistent burst was lost");
    state.ClearDecoded(); // A successful catalog reload retains semantic episode history.
    Check(!state.MarkPersistentBurst(77, 3) && state.MarkPersistentBurst(77, 4),
          "catalog reload replayed an active burst or merged distinct visual kinds");
    state.decoded.flashes.emplace_back();
    state.decoded.ground_events.emplace_back();
    state.decoded.ribbon_events.emplace_back();
    state.decoded.light_events.emplace_back();
    state.decoded.distortion_events.emplace_back();
    state.decoded.decal_events.emplace_back();
    state.decoded.particle_spawns.emplace_back();
    state.decoded.effect_lines.emplace_back();
    Check(state.Synchronize(2) && state.decoded.flashes.empty() && state.decoded.ground_events.empty() &&
              state.decoded.particle_spawns.empty() && state.decoded.effect_lines.empty() && state.decoded.ribbon_events.empty() && state.decoded.light_events.empty() && state.decoded.distortion_events.empty() && state.decoded.decal_events.empty(),
          "session restart retained an old visual");
    Check(state.MarkPersistentBurst(77, 3),
          "new simulation session retained an old burst episode");
    state.pending_events.push_back(old_event); // Delayed cross-thread delivery.
    Check(state.TakeReady(1).empty(), "reset clock replayed an old/future event");
    const auto second = state.TakeReady(2);
    Check(second.size() == 1 && second[0].sequence == 10,
          "event arriving before its session snapshot was dropped");
    Check(!state.Synchronize(2) && state.TakeReady(100).empty(),
          "old VFX replayed when the next session caught up to its tick");
}

void TestSessionProbeUiBridge()
{
    auto channel_storage = std::make_unique<hs::RuntimeChannels>();
    auto &channels = *channel_storage;
    hs::SessionProbe published;
    published.phase = hs::SessionPhase::Paused;
    published.skill_levels[1] = 2;
    published.skill_loadout[0] = hs::SkillKind::BasicAttack;
    published.skill_loadout[1] = hs::SkillKind::Count;
    channels.PublishSessionProbe(published);
    const auto session = channels.ReadSessionProbe();
    Check(session.skill_levels[1] == 2 &&
              session.skill_loadout[0] == hs::SkillKind::BasicAttack &&
              session.skill_loadout[1] == hs::SkillKind::Count,
          "session probe bridge preserves skills and loadout");
}

void TestCharacterDetailsTextFit()
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IDWriteFactory> base;
    Check(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
              reinterpret_cast<IUnknown **>(base.GetAddressOf()))), "create text-fit factory");
    ComPtr<IDWriteFactory5> factory;
    Check(SUCCEEDED(base.As(&factory)), "query bundled-font factory");
    std::array<wchar_t, 32768> executable{};
    Check(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())) != 0,
          "locate bundled text-fit font");
    auto directory = std::filesystem::path(executable.data()).parent_path();
    if (!std::filesystem::exists(directory / L"Fonts" / L"NotoSansKR.ttf")) directory = directory.parent_path();
    ComPtr<IDWriteFontFile> file;
    ComPtr<IDWriteFontSetBuilder1> builder;
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFontCollection1> collection;
    Check(SUCCEEDED(factory->CreateFontFileReference((directory / L"Fonts" / L"NotoSansKR.ttf").c_str(), nullptr, &file)) &&
              SUCCEEDED(factory->CreateFontSetBuilder(&builder)) && SUCCEEDED(builder->AddFontFile(file.Get())) &&
              SUCCEEDED(builder->CreateFontSet(&set)) &&
              SUCCEEDED(factory->CreateFontCollectionFromFontSet(set.Get(), &collection)),
          "load the renderer's bundled Noto Sans KR font");
    ComPtr<IDWriteFontFamily> family;
    ComPtr<IDWriteLocalizedStrings> names;
    Check(collection->GetFontFamilyCount() && SUCCEEDED(collection->GetFontFamily(0, &family)) &&
              SUCCEEDED(family->GetFamilyNames(&names)), "query bundled font name");
    UINT32 index{}, length{};
    BOOL exists{};
    names->FindLocaleName(L"ko-kr", &index, &exists);
    if (!exists) index = 0;
    Check(SUCCEEDED(names->GetStringLength(index, &length)), "query font name length");
    std::wstring family_name(length + 1, L'\0');
    Check(SUCCEEDED(names->GetString(index, family_name.data(), length + 1)), "read bundled font name");
    std::string fit_failures;
    std::size_t trailing_only_wraps{};
    const auto check_fit = [&](const hs::UiModel &entry) {
        const std::string_view text(entry.utf8_text.data());
        const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                              static_cast<int>(text.size()), nullptr, 0);
        Check(count > 0, "details text is complete valid UTF-8");
        std::wstring wide(static_cast<std::size_t>(count), L'\0');
        Check(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                  static_cast<int>(text.size()), wide.data(), count) == count, "decode details text");
        ComPtr<IDWriteTextFormat> format;
        ComPtr<IDWriteTextLayout> layout;
        Check(SUCCEEDED(factory->CreateTextFormat(family_name.c_str(), collection.Get(), DWRITE_FONT_WEIGHT_NORMAL,
                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(entry.font_pixels),
                  L"ko-kr", &format)), "create authored details format");
        Check(SUCCEEDED(format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP)) &&
                  SUCCEEDED(factory->CreateTextLayout(wide.data(), count, format.Get(),
                      entry.size_pixels.x, 1080, &layout)), "measure unshrunk details text");
        DWRITE_TEXT_METRICS metrics{};
        Check(SUCCEEDED(layout->GetMetrics(&metrics)), "read details glyph metrics");
        // Wrapped trailing spaces are not visible overflow and must not trigger renderer shrink/trim.
        if (metrics.widthIncludingTrailingWhitespace > entry.size_pixels.x &&
            metrics.width <= entry.size_pixels.x && metrics.height <= entry.size_pixels.y)
            ++trailing_only_wraps;
        for (const float scale : {1.0f, 720.0f / 1080.0f})
            if (metrics.height * scale > entry.size_pixels.y * scale + 0.01f ||
                metrics.width * scale > entry.size_pixels.x * scale + 0.01f)
            {
                fit_failures += std::format("Details need shrinking/clipping at scale {}: {} (visible={} trailing={} height={} > {}x{})\n",
                    scale, text, metrics.width, metrics.widthIncludingTrailingWhitespace, metrics.height,
                    entry.size_pixels.x, entry.size_pixels.y);
            }
    };
    hs::PresentationCatalog catalog;
    Check(hs::LoadPresentationCatalog(test_generation::ProductPath("game-data/presentation-catalog"), catalog).Succeeded(),
          "load authored relic names and rules for text-fit");
    hs::GameReadModelStorage model;
    model.arena_half_extent = 10;
    model.player.health = model.player.max_health = 100;
    model.session.health = model.session.max_health = 100;
    model.session.skill_levels.fill(1);
    model.session.upgrade_masks.fill(0xFF);
    model.session.relic_mask = hs::kAllRelicsMask;
    for (auto &damage : model.summary.upgrade_damage) damage.fill(std::numeric_limits<std::uint64_t>::max());
    model.summary.relic_triggers.fill(std::numeric_limits<std::uint64_t>::max());
    model.summary.relic_damage.fill(std::numeric_limits<std::uint64_t>::max());
    model.summary.relic_kills.fill(std::numeric_limits<std::uint64_t>::max());
    for (auto &effects : model.summary.relic_effects) effects.fill(std::numeric_limits<std::uint64_t>::max());
    hs::RenderSnapshotStorage snapshot(16, 2, 2, 128);
    hs::PresentationUiState ui;
    const auto project = [&] {
        snapshot.Clear();
        Check(hs::ProjectRenderSnapshot(model.View(), catalog, ui, {}, snapshot), "project text-fit fixture");
    };
    for (std::size_t skill = 0; skill < hs::kCombatSkillCount; ++skill)
    {
        model.session.phase = hs::SessionPhase::MainMenu;
        ui.page = hs::UiPage::Collection;
        ui.selected_collection_skill = static_cast<std::uint8_t>(skill);
        project();
        std::vector<std::string> descriptions;
        for (const auto &entry : snapshot.View().ui)
            if (entry.kind == hs::UiModel::Kind::Panel && entry.color_rgba == 0xFF2D4058u)
            {
                const std::string_view text(entry.utf8_text.data());
                Check(text.find('\n') != std::string_view::npos, "collection retains the complete upgrade rule");
                descriptions.emplace_back(text.substr(text.find('\n') + 1));
            }
        Check(descriptions.size() == hs::kUpgradeCount, "all authored upgrade descriptions are present");
        model.session.phase = hs::SessionPhase::Paused;
        ui.page = hs::UiPage::CharacterSkills;
        ui.selected_character_skill = static_cast<std::uint8_t>(skill);
        std::size_t shown{};
        for (ui.character_details_page = 0; ui.character_details_page < hs::CharacterDetailsPageCount(model.session, ui);
             ++ui.character_details_page)
        {
            project();
            for (const auto &entry : snapshot.View().ui)
                if (entry.kind == hs::UiModel::Kind::Text && entry.anchor_pixels.x >= 780 &&
                    entry.anchor_pixels.y >= 530 && entry.anchor_pixels.y < 950)
                {
                    check_fit(entry);
                    if (entry.color_rgba == 0xFFD7E0ECu)
                        Check(shown < descriptions.size() && entry.utf8_text.data() == descriptions[shown++],
                              "paged upgrade rule matches the full collection rule without truncation");
                }
        }
        Check(shown == hs::kUpgradeCount, "paging exposes every authored upgrade rule exactly once");
    }
    ui.page = hs::UiPage::CharacterStats;
    std::size_t shown{};
    for (ui.character_details_page = 0; ui.character_details_page < hs::CharacterDetailsPageCount(model.session, ui);
         ++ui.character_details_page)
    {
        project();
        for (const auto &entry : snapshot.View().ui)
            if (entry.kind == hs::UiModel::Kind::Text && entry.anchor_pixels.x < 1620 &&
                entry.anchor_pixels.y >= 235 && entry.anchor_pixels.y < 950)
            {
                check_fit(entry);
                if (entry.color_rgba == 0xFFB8C2D0u)
                    Check(shown < hs::kRelicCount &&
                              std::string_view(entry.utf8_text.data()) == catalog.relic_rules[shown++].data(),
                          "paged relic rule matches the complete cooked description");
            }
    }
    Check(shown == hs::kRelicCount, "paging exposes every authored relic rule exactly once");
    Check(trailing_only_wraps != 0, "authored details exercise trailing-space wrap without visible overflow");
    Check(fit_failures.empty(), fit_failures);
    std::cout << "UI text-fit: 72 upgrade rules, 20 relic rules, maximum counters; no additional shrink/trim at 1080p/720p\n";
}

void TestDpiWindowCoordinates()
{
    auto manifest_path = test_generation::ExecutableDirectory() / "ProjectHS.manifest";
    if (!std::filesystem::is_regular_file(manifest_path))
        manifest_path = manifest_path.parent_path().parent_path() / "ProjectHS.manifest";
    std::ifstream manifest(manifest_path);
    const std::string declaration{std::istreambuf_iterator<char>(manifest), std::istreambuf_iterator<char>()};
    Check(declaration.find(
              "<dpiAwareness xmlns=\"http://schemas.microsoft.com/SMI/2016/WindowsSettings\">PerMonitorV2</dpiAwareness>") != std::string::npos,
          "ProjectHS generated manifest declares PerMonitorV2");

    const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Check(previous != nullptr, "select PMv2 for the hidden window test");
    struct RestoreDpiContext
    {
        DPI_AWARENESS_CONTEXT previous;
        ~RestoreDpiContext() { SetThreadDpiAwarenessContext(previous); }
    } restore{previous};
    auto channels = std::make_unique<hs::RuntimeChannels>();
    {
        hs::PresentationCamera camera{0.0f, 45.0f, 60.0f, 10.0f, 50, 200};
        hs::Window window(*channels, {}, camera);
        Check(window.Create(640, 360, false, false).Succeeded(), "create a hidden DPI-aware window");
        const auto handle = window.Handle();
        UINT registered_count{};
        Check(GetRegisteredRawInputDevices(nullptr, &registered_count, sizeof(RAWINPUTDEVICE)) == 0,
              "query registered raw input devices");
        std::vector<RAWINPUTDEVICE> registered(registered_count);
        Check(GetRegisteredRawInputDevices(registered.data(), &registered_count,
                                           sizeof(RAWINPUTDEVICE)) != static_cast<UINT>(-1),
              "read registered raw input devices");
        std::size_t foreground_devices{};
        for (const auto &device : registered)
        {
            if (device.hwndTarget != handle || device.usUsagePage != HID_USAGE_PAGE_GENERIC ||
                (device.usUsage != HID_USAGE_GENERIC_KEYBOARD && device.usUsage != HID_USAGE_GENERIC_MOUSE))
                continue;
            Check((device.dwFlags & (RIDEV_INPUTSINK | RIDEV_EXINPUTSINK)) == 0,
                  "game keyboard and mouse receive raw input only while foreground");
            ++foreground_devices;
        }
        Check(foreground_devices == 2, "both game raw input devices use foreground delivery");
        hs::HeldInputState held;
        held.move_held = true;
        held.basic_attack_held = true;
        channels->held_input.store(held);
        const auto focus_epoch = channels->focus_epoch.load();
        SendMessageW(handle, WM_KILLFOCUS, 0, 0);
        held = channels->held_input.load();
        Check(!held.move_held && !held.basic_attack_held &&
                  channels->focus_epoch.load() == focus_epoch + 1,
              "focus loss clears held input and invalidates queued action edges");
        Check(!IsWindowVisible(handle) && AreDpiAwarenessContextsEqual(
                  GetWindowDpiAwarenessContext(handle), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
              "hidden HWND retains the PMv2 context");
        Check(window.ApplyDisplayPlacement({"auto"}, false, 640, 360).Succeeded(),
              "place the requested physical client extent");
        RECT client{}, native{};
        Check(GetClientRect(handle, &client) && client.right == 640 && client.bottom == 360,
              "client extent remains physical pixels at the actual monitor DPI");
        POINT cursor{320, 180};
        Check(ClientToScreen(handle, &cursor) && ScreenToClient(handle, &cursor) &&
                  cursor.x == 320 && cursor.y == 180,
              "screen/client mouse coordinates round-trip without manual scaling");
        hs::Float2 hover{};
        window.SetUiHoverHandler([&](hs::Float2 value) { hover = value; });
        SendMessageW(handle, WM_MOUSEMOVE, 0, MAKELPARAM(160, 90));
        Check(std::abs(hover.x + 0.5f) < 0.001f && std::abs(hover.y - 0.5f) < 0.001f,
              "mouse lParam uses the physical client extent");

        Check(GetWindowRect(handle, &native), "query hidden window placement");
        RECT suggested{native.left + 7, native.top + 11, native.right + 20, native.bottom + 28};
        const auto dpi = GetDpiForWindow(handle);
        Check(dpi != 0, "query actual HWND DPI");
        // Synthetic message tests placement/resize handling without requiring two DPI monitors.
        SendMessageW(handle, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
        Check(GetWindowRect(handle, &native) && EqualRect(&native, &suggested),
              "DPI notification applies the supplied physical position and frame");
        Check(GetClientRect(handle, &client) && client.right == 653 && client.bottom == 377,
              "DPI notification preserves the suggested client resize");
        hs::ResizeCommand resize{}, last_resize{};
        bool resized{};
        while (channels->resize_commands.TryPop(resize)) { last_resize = resize; resized = true; }
        Check(resized && last_resize.width == 653 && last_resize.height == 377,
              "WM_SIZE publishes the physical renderer resize");

        Check(window.ApplyDisplayPlacement({"auto"}, true, 640, 360).Succeeded(), "enter hidden borderless mode");
        MONITORINFO monitor{sizeof(monitor)};
        Check(GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor),
              "query borderless monitor rectangle");
        suggested = {monitor.rcMonitor.left + 10, monitor.rcMonitor.top + 10,
                     monitor.rcMonitor.left + 110, monitor.rcMonitor.top + 110};
        SendMessageW(handle, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
        Check(GetWindowRect(handle, &native) && EqualRect(&native, &monitor.rcMonitor),
              "borderless DPI notification fills its target monitor");
        Check(window.ApplyDisplayPlacement({"auto"}, false, 640, 360).Succeeded() &&
                  GetClientRect(handle, &client) && client.right == 640 && client.bottom == 360,
              "return from borderless to the requested physical client extent");

        suggested = {};
        SendMessageW(handle, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
        PostQuitMessage(0);
        auto failure = hs::Result::Success();
        Check(!window.PumpMessages(&failure) && !failure && failure.Code() == hs::ErrorCode::InvalidState &&
                  failure.Message().find("DPI window placement failed") != std::string_view::npos,
              "synchronous DPI failure is returned even with WM_QUIT queued");
        Check(!channels->stop_requested.load(), "DPI failure reaches the caller before Runtime stops");
    }
    MSG quit{};
    while (PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
}

} // namespace

int main(int argc, char **argv)
{
    const auto root = TestDirectory();
    std::error_code error;
    std::filesystem::remove_all(root, error);
    try
    {
        if (argc > 1)
        {
            Check(argc == 2 && std::string_view(argv[1]) == "ui-text-fit", "unknown runtime test case");
            TestCharacterDetailsTextFit();
            return 0;
        }
        TestVfxCatalog();
        TestProductionVfxDecoding();
        TestSaveRecovery(root);
        TestSettingsPersistence(root);
        TestAudioCatalogValidation(root);
        TestAudioPayloadLazyLoad(root);
        TestExperimentSpecRoundTrip(root);
        TestPlaytestRecordAndReplay(root);
        TestRecordingFileFinalization(root);
        TestPlaytestFinalizationFailures(root);
        TestSessionMusicTransitions();
        TestSessionProbeUiBridge();
        TestVfxSessionReset();
        TestCameraPoseContinuity();
        TestDpiWindowCoordinates();
        std::filesystem::remove_all(root, error);
        std::cout << "runtime_tests passed\n";
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << "runtime_tests failed: " << exception.what() << '\n';
        return 1;
    }
}
