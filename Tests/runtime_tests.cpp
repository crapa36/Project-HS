#include <hs/runtime/experiment_spec.hpp>
#include <hs/runtime/experiment_pipe.hpp>
#include <hs/runtime/save_store.hpp>
#include <hs/runtime/audio_engine.hpp>
#include <hs/runtime/playtest_recording.hpp>
#include <hs/core/cooked_format.hpp>
#include "vfx_catalog.hpp"
#include "runtime_channels.hpp"
#include "camera_pose.hpp"
#include "vfx_session_state.hpp"
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

#include <array>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
    Check(store.SaveProfile(second).Succeeded(), "second profile save");
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
}

void TestNamedPipeCommandRoundTrip()
{
    const auto name = L"test-" + std::to_wstring(GetCurrentProcessId());
    hs::Result server_result = hs::Result::Success();
    std::jthread server([&] {
        server_result = hs::ServeExperimentPipeOnce(
            name, [](std::string_view request, std::string &reply) {
                hs::ExperimentPipeCommand command;
                if (auto parsed = hs::ParseExperimentPipeCommand(request, command); !parsed)
                    return parsed;
                reply = hs::BuildExperimentPipeReply(command.sequence,
                                                     command.target_tick, "accepted", "applied");
                return hs::Result::Success();
            });
    });
    const std::string request =
        R"({"sequence":7,"target_tick":30,"command":"damage_player","payload":{"amount":5}})";
    std::string reply;
    hs::Result transaction = hs::Result::Failure(hs::ErrorCode::InvalidState,
                                                 "test", "not attempted");
    for (unsigned attempt = 0; attempt < 20 && !transaction; ++attempt)
    {
        transaction = hs::TransactExperimentPipe(name, request, reply,
                                                 std::chrono::milliseconds(100));
        if (!transaction) Sleep(5);
    }
    server.join();
    Check(transaction.Succeeded() && server_result.Succeeded(), "named pipe transaction");
    Check(reply.find("\"sequence\":7") != std::string::npos &&
              reply.find("\"status\":\"accepted\"") != std::string::npos,
          "named pipe ack preserves sequence");

    hs::ExperimentPipeCommand invalid;
    Check(!hs::ParseExperimentPipeCommand(R"({"sequence":1})", invalid),
          "named pipe rejects incomplete JSON command");
}

void TestNamedPipeIdempotencyAndTargetTick()
{
    hs::ExperimentPipeCommandQueue queue;
    const std::string accepted =
        R"({"sequence":11,"target_tick":30,"command":"damage_player","payload":{"amount":5}})";
    std::string first;
    std::string duplicate;
    Check(queue.Handle(accepted, 10, first).Succeeded() &&
              queue.Handle(accepted, 20, duplicate).Succeeded() && first == duplicate &&
              queue.AcceptedCount() == 1,
          "duplicate sequence replays response without queueing twice");

    const std::string missed =
        R"({"sequence":12,"target_tick":20,"command":"heal_player","payload":{"amount":5}})";
    std::string rejected;
    Check(queue.Handle(missed, 20, rejected).Succeeded() &&
              rejected.find("missed_target_tick") != std::string::npos &&
              queue.AcceptedCount() == 1,
          "elapsed target tick is rejected");
    const auto commands = queue.TakeAccepted();
    Check(commands.size() == 1 && commands.front().sequence == 11,
          "accepted command executes once");
}

void TestProductionVfxDecoding()
{
    hs::VfxCatalog catalog;
    Check(hs::VfxCatalog::Load("Cooked/particle_effects.hsbin", catalog).Succeeded(), "production VFX catalog");
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
    Check(hs::VfxCatalog::Load("Cooked/particle_effects.hsbin", catalog).Succeeded(),
          "load cooked VFX catalog");
    Check(catalog.SpriteCount() == 39, "load data-driven VFX sprite registry");
    const auto fixture = TestDirectory() / "vfx_required_program";
    std::filesystem::create_directories(fixture);
    std::filesystem::copy_file("Cooked/particle_effects.hsbin", fixture / "particle_effects.hsbin",
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
    Check(catalog.ExpandEvent(typed_only, 100, full, lines).Succeeded() && full.empty() && lines.empty(),
          "typed-only event must not require a legacy emitter");
    typed_only.asset = hs::MakeAssetId("particle.invalid.not_in_either_catalog");
    Check(!catalog.ExpandEvent(typed_only, 100, full, lines).Succeeded(),
          "unknown VFX event must still fail catalog dispatch");
    Check(catalog.ExpandEvent(event, 100, full, lines).Succeeded() && !full.empty(),
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
    Check(catalog.ExpandEvent(event, 100, basic_mesh, basic_lines).Succeeded(), "basic VFX event expands");
    Check(std::ranges::all_of(basic_mesh, [](const auto &command) {
        return command.renderer != hs::VfxRenderer::Mesh || command.primitive != hs::VfxPrimitive::Ember || command.mesh_index != 1;
    }), "basic ember must not become authored arrowhead");
    event.asset = hs::MakeAssetId("particle.common.hit");
    Check(std::ranges::all_of(full, [](const auto &command) {
              return command.renderer == hs::VfxRenderer::Mesh || command.mesh_index == 0;
          }), "authored mesh selection leaves sprite and ground outputs unchanged");
    Check(catalog.ExpandEvent(event, 50, half, lines).Succeeded() && half.size() == full.size(),
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
          Check(catalog.ExpandEvent(event, 100, particles, effect_lines).Succeeded() &&
                    !particles.empty(),
                "new VFX expands into particles");
      }

    event.asset = hs::MakeAssetId("particle.boss.shockwave.release");
    event.geometry.kind = hs::PresentationGeometryKind::RingGaps;
    std::vector<hs::ParticleSpawnCommand> shockwave_accents;
    Check(catalog.ExpandEvent(event, 100, shockwave_accents, lines).Succeeded() &&
        !shockwave_accents.empty() && std::ranges::none_of(shockwave_accents, [](const auto &command) {
            return command.renderer == hs::VfxRenderer::Ground && command.primitive == hs::VfxPrimitive::Ring;
        }), "authoritative wavefront replaces only legacy ring that crosses safe gaps");
    event.geometry = {};
      event.asset = hs::MakeAssetId("particle.line.ricochet");
    Check(!catalog.ExpandEvent(event, 100, full, lines).Succeeded(), "line target required");
    parameters.flags = static_cast<std::uint32_t>(hs::VfxEventFlag::HasTarget);
    parameters.target = {4.0f, 0.0f, 5.0f};
    event.parameters = hs::EncodeVfxParameters(parameters);
    Check(catalog.ExpandEvent(event, 100, full, lines).Succeeded() && lines.size() == 1 &&
              lines.front().sprite != 0 && lines.front().uv_repeat > 1.0f &&
              lines.front().primitive == hs::VfxPrimitive::DashedRicochet,
          "expand line VFX");

    const auto check_visual = [&](std::string_view id, hs::VfxRenderer renderer,
                                  hs::VfxPrimitive primitive) {
        event.asset = hs::MakeAssetId(id);
        std::vector<hs::ParticleSpawnCommand> particles;
        std::vector<hs::VfxLineSpawnCommand> ignored_lines;
        Check(catalog.ExpandEvent(event, 100, particles, ignored_lines).Succeeded() &&
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
        Check(catalog.ExpandEvent(event, 100, particles, ignored_lines).Succeeded(),
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
        Check(catalog.ExpandEvent(event, 100, particles, ignored_lines).Succeeded(),
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
    const std::array edges{hs::ActionEdge{1, hs::GameAction::SkillQ,
                                         hs::EdgeKind::Pressed}};
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
    state.flashes.emplace_back();
    state.ground_events.emplace_back();
    state.ribbon_events.emplace_back();
    state.light_events.emplace_back();
    state.distortion_events.emplace_back();
    state.decal_events.emplace_back();
    state.particle_spawns.emplace_back();
    state.effect_lines.emplace_back();
    Check(state.Synchronize(2) && state.flashes.empty() && state.ground_events.empty() &&
              state.particle_spawns.empty() && state.effect_lines.empty() && state.ribbon_events.empty() && state.light_events.empty() && state.distortion_events.empty() && state.decal_events.empty(),
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

} // namespace

int main()
{
    const auto root = TestDirectory();
    std::error_code error;
    std::filesystem::remove_all(root, error);
    try
    {
        TestVfxCatalog();
        TestProductionVfxDecoding();
        TestSaveRecovery(root);
        TestSettingsPersistence(root);
        TestAudioCatalogValidation(root);
        TestAudioPayloadLazyLoad(root);
        TestExperimentSpecRoundTrip(root);
        TestNamedPipeCommandRoundTrip();
        TestNamedPipeIdempotencyAndTargetTick();
        TestPlaytestRecordAndReplay(root);
        TestSessionProbeUiBridge();
        TestVfxSessionReset();
        TestCameraPoseContinuity();
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
