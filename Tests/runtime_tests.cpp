#include "vfx_light_reference.hpp"
#include <hs/runtime/experiment_spec.hpp>
#include <hs/runtime/shipping_content.hpp>
#include <hs/core/shipped_content_owner.hpp>
#include <hs/core/process_info.hpp>
#include <bit>
#include <hs/runtime/save_store.hpp>
#include <hs/runtime/audio_engine.hpp>
#include <hs/runtime/playtest_recording.hpp>
#include <hs/core/cooked_format.hpp>
#include <hs/core/sha256.hpp>
#include <hs/presentation/presentation_catalog_io.hpp>
#include <hs/game_rules/simulation_rules_io.hpp>
#include <hs/gameplay/game_simulation.hpp>
#include <hs/core/fixed_step_clock.hpp>
#include "vfx_catalog.hpp"
#include "runtime_channels.hpp"
#include "session_music.hpp"
#include "audio_session.hpp"
#include "audio_native.hpp"
#include "scene_generation_binding.hpp"
#include "native_input_frame.hpp"
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
#include <algorithm>
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

void TestProfileMigration(const std::filesystem::path &root)
{
    const std::string legacy = R"({"schema_version":1,"best_level":42,"total_wins":19,"total_kills":18446744073709551614,"unlocked_skills_mask":255,"unlocked_relics_mask":4095})";
    const auto read = [](const std::filesystem::path &path) {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };
    const auto folder = root / "profile-migration";
    std::filesystem::create_directories(folder);
    hs::SaveStore store(folder);
    const auto primary = folder / "profile.json";
    const auto backup = folder / "profile.json.bak";
    std::ofstream(primary) << legacy;
    hs::ProfileData profile;
    Check(store.LoadProfile(profile).Succeeded() && profile.schema_version == 2 &&
              profile.best_level == 42 && profile.total_wins == 19 &&
              profile.total_kills == std::numeric_limits<std::uint64_t>::max() - 1,
          "v1 profile migrates all used statistics without narrowing");
    Check(read(primary) == legacy && !std::filesystem::exists(backup),
          "reading legacy profile does not rewrite user bytes");
    Check(store.SaveProfile(profile).Succeeded(), "explicit save writes migrated profile");
    const auto current = read(primary);
    Check(current.find("unlocked_") == std::string::npos && read(backup) == legacy,
          "v2 removes unused masks while atomic backup preserves original v1");
    hs::ProfileData roundtrip;
    Check(store.LoadProfile(roundtrip).Succeeded() && roundtrip.schema_version == 2 &&
              roundtrip.total_kills == profile.total_kills && roundtrip.total_wins == 19,
          "v2 profile round trips statistics exactly");

    WriteCorrupt(primary);
    Check(store.LoadProfile(roundtrip).Succeeded() && roundtrip.best_level == 42 &&
              roundtrip.total_kills == profile.total_kills && read(backup) == legacy,
          "corrupt v2 primary recovers v1 backup without losing its original bytes");

    for (const auto &bad : {
             R"({"schema_version":1,"best_level":99,"total_wins":0,"total_kills":0,"unlocked_skills_mask":256,"unlocked_relics_mask":4095})",
             R"({"schema_version":1,"best_level":99,"total_wins":0,"total_kills":0,"unlocked_skills_mask":255,"unlocked_relics_mask":4096})",
             R"({"schema_version":2,"best_level":99,"best_level":100,"total_wins":0,"total_kills":0})",
             R"({"schema_version":2,"best_level":99,"total_wins":0,"total_kills":0,"unlocked_skills_mask":255})"})
    {
        std::ofstream(primary, std::ios::trunc) << bad;
        Check(store.LoadProfile(roundtrip).Succeeded() && roundtrip.best_level == 42,
              "migration keeps legacy mask bounds and strict duplicate/current field checks");
    }

    const std::string future = R"({"schema_version":3,"future_data":[1,2,3]})";
    std::ofstream(primary, std::ios::trunc) << future;
    Check(!store.LoadProfile(roundtrip) && !store.SaveProfile(profile) &&
              read(primary) == future && read(backup) == legacy,
          "future profile primary remains byte-for-byte read-only on load and save");
    // WriteDocument emits LF bytes directly; text-mode output would translate them to CRLF.
    std::ofstream(primary, std::ios::binary | std::ios::trunc) << current;
    std::ofstream(backup, std::ios::trunc) << future;
    Check(!store.SaveProfile(profile), "future backup blocks profile save");
    Check(read(primary) == current && read(backup) == future,
          "blocked save preserves primary and future backup bytes exactly");
    WriteCorrupt(primary);
    Check(!store.LoadProfile(roundtrip) && read(primary) == "{broken" && read(backup) == future,
          "future backup blocks recovery before corrupt primary is moved");
    const std::string duplicate_future = R"({"schema_version":3,"future_data":1,"future_data":2})";
    std::ofstream(primary, std::ios::trunc) << duplicate_future;
    Check(!store.LoadProfile(roundtrip) && !store.SaveProfile(profile) && read(primary) == duplicate_future,
          "future duplicate-key document is read-only even though strict parsing rejects it");
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
    source.timeline_actions.clear();
    source.config_overrides_json = "{\"diagnostic_enemy_count\":200}";
    hs::ApplicationConfig diagnostic;
    Check(hs::SaveExperimentSpec(path, source) && hs::LoadExperimentSpec(path, loaded) &&
              hs::ApplyExperimentOverrides(loaded, diagnostic) && diagnostic.diagnostic_enemy_count == 200 && diagnostic.capture_diagnostics,
          "diagnostic enemy population round trips and applies as a number");
    source.config_overrides_json = "{\"diagnostic_enemy_count\":10001}";
    Check(!hs::SaveExperimentSpec(path, source) || !hs::LoadExperimentSpec(path, loaded),
          "diagnostic enemy population above the fixture bound is rejected");
    Check(!hs::ApplicationConfig{}.graphics_preset,
          "graphics preset override is absent by default");
    for (const auto &[name, expected] : std::array{
             std::pair{"low", hs::GraphicsPreset::Low},
             std::pair{"medium", hs::GraphicsPreset::Medium},
             std::pair{"high", hs::GraphicsPreset::High},
             std::pair{"ultra", hs::GraphicsPreset::Ultra}})
    {
        source.config_overrides_json = std::string("{\"graphics_preset\":\"") + name + "\"}";
        hs::ApplicationConfig preset;
        Check(hs::SaveExperimentSpec(path, source) && hs::LoadExperimentSpec(path, loaded) &&
                  hs::ApplyExperimentOverrides(loaded, preset) && preset.graphics_preset == expected,
              "typed graphics preset override round trips");
    }
    for (const auto invalid : {"{\"graphics_preset\":\"custom\"}",
                               "{\"graphics_preset\":\"fast\"}",
                               "{\"graphics_preset\":true}",
                               "{\"graphics_preset\":1}"})
    {
        source.config_overrides_json = invalid;
        Check(!hs::SaveExperimentSpec(path, source) || !hs::LoadExperimentSpec(path, loaded),
              "graphics preset rejects custom, unknown, and nonstring values");
    }
    source.config_overrides_json = R"({"graphics_preset":"ultra","width":1920,"height":1080,"vsync":false,"frame_cap":30,"render_scale_percent":75,"shadow_resolution":1024,"particle_percentage":50,"borderless":true,"bloom":false,"outline":false})";
    hs::ApplicationConfig explicit_graphics;
    Check(hs::SaveExperimentSpec(path, source) && hs::LoadExperimentSpec(path, loaded) &&
              hs::ApplyExperimentOverrides(loaded, explicit_graphics) && explicit_graphics.graphics_preset == hs::GraphicsPreset::Ultra &&
              explicit_graphics.resolution_explicit && explicit_graphics.vsync_explicit &&
              explicit_graphics.frame_cap_explicit && explicit_graphics.render_scale_explicit &&
              explicit_graphics.shadow_resolution_explicit && explicit_graphics.particle_percentage_explicit &&
              explicit_graphics.borderless_explicit && explicit_graphics.bloom_explicit && explicit_graphics.outline_explicit,
          "experiment preset preserves explicit field precedence metadata");
    hs::ApplicationConfig hero;
    Check(!hero.capture_hero_work, "isolated Hero work capture defaults off");
    source.config_overrides_json = "{\"capture_hero_work\":true}";
    Check(hs::SaveExperimentSpec(path, source) && hs::LoadExperimentSpec(path, loaded) &&
              hs::ApplyExperimentOverrides(loaded, hero) && hero.capture_hero_work && hero.capture_diagnostics,
          "Hero work boolean override round trips and reaches Runtime configuration");
    source.config_overrides_json = "{\"capture_hero_work\":\"true\"}";
    Check(!hs::SaveExperimentSpec(path, source) || !hs::LoadExperimentSpec(path, loaded),
          "Hero work capture rejects a nonboolean override");
    Check(hs::ApplicationConfig{}.hero_work_mode == hs::HeroWorkDiagnosticMode::Full,
          "Hero work shading defaults to the unchanged full path");
    for (const auto &[name, expected] : std::array{
             std::pair{"full", hs::HeroWorkDiagnosticMode::Full},
             std::pair{"no-environment", hs::HeroWorkDiagnosticMode::NoEnvironment},
             std::pair{"coverage-only", hs::HeroWorkDiagnosticMode::CoverageOnly}})
    {
        source.config_overrides_json = std::string("{\"capture_hero_work\":true,\"capture_hero_work_mode\":\"") + name + "\"}";
        Check(hs::SaveExperimentSpec(path, source) && hs::LoadExperimentSpec(path, loaded) &&
                  hs::ApplyExperimentOverrides(loaded, hero) && hero.hero_work_mode == expected,
              "explicit Hero diagnostic mode round trips without changing its meaning");
    }
    for (const auto invalid : {"{\"capture_hero_work_mode\":true}", "{\"capture_hero_work_mode\":\"fast\"}"})
    {
        source.config_overrides_json = invalid;
        Check(!hs::SaveExperimentSpec(path, source) || !hs::LoadExperimentSpec(path, loaded),
              "Hero diagnostic mode rejects wrong types and unknown values");
    }
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
            decoded=(std::max)(decoded,hs::runtime_detail::BuildVfxTypedLightCommands(catalog.Image(),std::span(&input,1),tick).size());

        for(auto quality:{hs::VfxQuality::Low,hs::VfxQuality::Medium,hs::VfxQuality::High})
        for(bool owned:{false,true})
        {
            input.quality=quality; input.geometry_owner_id=owned?91:0;input.geometry_start_tick=3;input.geometry_end_tick=243;
            for(hs::Tick tick=0;tick<=250;++tick)
            {
                const auto reference=hs::runtime_detail::light_reference::BuildVfxTypedLightCommands(program,std::span(&input,1),tick);
                vfx_light_test::Compare(reference,hs::runtime_detail::BuildVfxTypedLightCommands(catalog.Image(),std::span(&input,1),tick));
                vfx_light_test::Compare(reference,hs::runtime_detail::BuildVfxTypedLightCommands(hs::shipping::Vfx(),std::span(&input,1),tick));
            }
        }
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

    // Exercise the prepared static policies after ownership transfer as well as Load.
    hs::VfxCatalog transferred;
    transferred.Swap(catalog);
    catalog = std::move(transferred);
    std::size_t compared_policies{};
    for (const auto &lookup : catalog.Program().effect_lookup)
    {
        const auto *definition = catalog.FindEffect({lookup.effect_id});
        if (!definition || definition->kind == hs::VfxDefinitionKind::Line) continue;
        const auto &policy = catalog.Program().effects[lookup.handle - 1];
        const bool decorative = !policy.must_match_logic && std::ranges::none_of(
            std::span(catalog.Program().sources).subspan(policy.sources.first, policy.sources.count),
            [](const auto &source) {
                return source.authoritative || source.type == hs::VfxSourceType::ProjectileFollow;
            });
        event.asset = {lookup.effect_id};
        std::vector<hs::ParticleSpawnCommand> reference;
        std::vector<hs::VfxLineSpawnCommand> ignored_lines;
        Check(catalog.ExpandEvent(event, 1.0f, reference, ignored_lines).Succeeded(),
              "prepared policy baseline expands after catalog swap and move");
        for (const auto fraction : {0.0f, 0.5f, 1.0f})
        {
            std::vector<hs::ParticleSpawnCommand> actual;
            Check(catalog.ExpandEvent(event, fraction, actual, ignored_lines).Succeeded() &&
                      actual.size() == reference.size(), "prepared policy retains emitter order and count");
            for (std::size_t i = 0; i < reference.size(); ++i)
            {
                const auto expected = decorative && reference[i].renderer != hs::VfxRenderer::Ground
                    ? static_cast<std::uint32_t>(std::ceil(double(reference[i].count) * fraction))
                    : reference[i].count;
                Check(actual[i].count == expected && actual[i].seed == reference[i].seed &&
                          actual[i].mesh_index == reference[i].mesh_index,
                      "prepared policy matches original source-based quality admission and seed");
            }
        }
        ++compared_policies;
    }
    Check(compared_policies != 0, "catalog policy regression visits authored legacy effects");

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

void TestMainWakeAndSceneLeases()
{
    hs::runtime_detail::MainWake wake;
    Check(wake.Handle() && wake.Error() == 0, "create owner-scoped Main event");
    wake.Notify();
    Check(wake.PrepareWait([] { return false; }) && WaitForSingleObject(wake.Handle(), 0) == WAIT_TIMEOUT,
          "drained coalesced notification resets before the final work check");
    bool injected = false;
    Check(!wake.PrepareWait([&] { injected = true; wake.Notify(); return false; }) && injected &&
              WaitForSingleObject(wake.Handle(), 0) == WAIT_OBJECT_0,
          "publication between reset and recheck cannot lose its wake");
    Check(wake.PrepareWait([] { return false; }), "prepare an empty wait");
    wake.Notify();
    Check(WaitForSingleObject(wake.Handle(), 0) == WAIT_OBJECT_0,
          "successful publication immediately before wait remains signaled");
    Check(!wake.PrepareWait([] { return true; }), "queued/stop work never enters a wait");
    const auto processed = wake.Sequence();
    wake.Notify(); // Reader/callback completion after the owner's readiness/reclaim work.
    Check(!wake.PrepareWait([] { return false; }, processed),
          "a callback arriving during Main work cannot be swallowed by a later reset even without a queued item");
    auto channels = std::make_unique<hs::RuntimeChannels>(&wake);
    auto simulation = channels->ForSimulation();
    Check(wake.PrepareWait([] { return false; }), "reset before an actual producer push");
    simulation.PublishAudioTick({1, 1, hs::SessionPhase::Playing, {}});
    Check(WaitForSingleObject(wake.Handle(), 0) == WAIT_OBJECT_0 &&
              !wake.PrepareWait([&] { return channels->sim_audio_events.Size() != 0; }),
          "successful sim audio publication notifies and the actual queue recheck prevents sleeping");
    hs::NativeInputFrame input;
    input.BindMainWake(hs::runtime_detail::MainWake::NotifyOpaque, &wake);
    Check(wake.PrepareWait([] { return false; }), "reset before native shutdown cancellation");
    input.Cancel();
    Check(WaitForSingleObject(wake.Handle(), 0) == WAIT_OBJECT_0 && !input.MainWorkPending(),
          "native cancellation wakes Main without inventing a sampling token");

    MSG message{};
    (void)PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE); // Create this test thread's message queue.
    const auto thread = GetCurrentThreadId();
    Check(PostThreadMessageW(thread, WM_KEYDOWN, VK_F24, 0x321) &&
              PostThreadMessageW(thread, WM_MOUSEMOVE, 0x321, 0) &&
              PostThreadMessageW(thread, WM_APP + 61, 0x321, 0), "publish sampled and system test messages");
    bool saw_system = false;
    while (hs::runtime_detail::PeekMainMessage(message, false))
    {
        Check(message.message != WM_KEYDOWN && message.message != WM_MOUSEMOVE,
              "parked native token cannot consume queued key/mouse input");
        if (message.message == WM_APP + 61) saw_system = true;
    }
    Check(saw_system && (hs::runtime_detail::MainMessageMask(true) & QS_INPUT) == 0 &&
              hs::runtime_detail::MainMessageFlags(true) == 0,
          "system peeking and wait flags exclude retained native input instead of repeatedly waking on it");
    unsigned sampled{};
    while (hs::runtime_detail::PeekMainMessage(message, true))
        if ((message.message == WM_KEYDOWN || message.message == WM_MOUSEMOVE) && message.wParam != 0) ++sampled;
    Check(sampled == 2 && hs::runtime_detail::MainMessageFlags(false) == MWMO_INPUTAVAILABLE,
          "the next normal sample drains already queued input under the ordinary message mask");

    hs::runtime_detail::SceneGenerationBinding binding;
    binding.source_generation_id = "source"; binding.resident_generation_id = "resident";
    const auto scene = [](std::uint64_t id) {
        auto storage = std::make_unique<hs::StaticRenderSceneStorage>();
        storage->content_generation_id = "source";
        hs::RenderInstance instance; instance.stable_id = id; instance.position.x = static_cast<float>(id);
        storage->instances.push_back(instance);
        storage->eligible_static_members.push_back({id, id + 1, 0});
        return hs::FreezeStaticRenderScene(std::move(storage));
    };
    auto original = scene(11);
    std::weak_ptr<const hs::StaticRenderScene> original_lifetime = original;
    auto rebound = binding.Resolve(original);
    original.reset();
    (void)binding.Resolve(scene(22)); (void)binding.Resolve(scene(33));
    Check(!original_lifetime.expired() && rebound->content_generation_id == "resident" &&
              rebound->instances[0].stable_id == 11 && rebound->instances[0].position.x == 11.0f &&
              rebound->eligible_static_members[0].object_id == 12 && rebound->static_keys[0].stable_id == 11,
          "evicted generation-relabel cache entries still retain the source owner of every exported span");
    rebound.reset();
    Check(original_lifetime.expired(), "source span backing retires after the final rebound packet lease");
}

void TestAudioNativeOperations()
{
    using Op = hs::AudioNativeOperation;
    struct Voice
    {
        std::array<Op, 24> trace{};
        std::size_t count{};
        Op fail{};
        std::atomic<std::uint32_t> *pins{};
        bool destroyed{}, pin_held_at_destroy{};
        HRESULT Call(Op op) noexcept { trace[count++] = op; return op == fail ? E_FAIL : S_OK; }
        HRESULT Stop() noexcept { return Call(Op::Stop); }
        HRESULT FlushSourceBuffers() noexcept { return Call(Op::Flush); }
        HRESULT SetFrequencyRatio(float) noexcept { return Call(Op::Frequency); }
        HRESULT SetOutputVoices(const XAUDIO2_VOICE_SENDS *) noexcept { return Call(Op::Route); }
        HRESULT SetOutputMatrix(IXAudio2Voice *, UINT32, UINT32, const float *) noexcept { return Call(Op::Matrix); }
        HRESULT SubmitSourceBuffer(const XAUDIO2_BUFFER *) noexcept { return Call(Op::Submit); }
        HRESULT Start() noexcept { return Call(Op::Start); }
        void DestroyVoice() noexcept
        { destroyed = true; pin_held_at_destroy = pins && pins->load() == 1; }
    };
    constexpr std::array order{Op::Stop, Op::Flush, Op::Frequency, Op::Route, Op::Matrix, Op::Submit, Op::Start};
    XAUDIO2_VOICE_SENDS sends{}; XAUDIO2_BUFFER buffer{};
    const std::array matrix{0.25f, 0.75f};
    for (std::size_t failed = 0; failed <= order.size(); ++failed)
    {
        Voice voice; if (failed < order.size()) voice.fail = order[failed];
        const auto result = hs::audio_detail::SubmitPreparedVoice(voice, sends, nullptr, 2, matrix, buffer);
        Check(result.Succeeded() == (failed == order.size()), "each native failure is returned at its actual operation");
        if (failed < order.size()) Check(result.operation == order[failed] && result.error == E_FAIL,
            "Stop/Flush/frequency/route/matrix/Submit/Start failures retain distinct receipts");
        const auto expected = std::min(failed + 1, order.size());
        Check(voice.count == expected && std::equal(order.begin(), order.begin() + expected, voice.trace.begin()),
              "route and first matrix precede Submit/Start and no operation follows a failed boundary");
    }
    Voice defaults;
    Check(hs::audio_detail::SubmitPreparedVoice(defaults, sends, nullptr, 2, {}, buffer).Succeeded() &&
              defaults.count == 6 && defaults.trace[3] == Op::Route && defaults.trace[4] == Op::Submit,
          "native default mapping skips custom matrix without a late correction");
    Voice paused;
    Check(hs::audio_detail::SubmitPreparedVoice(paused, sends, nullptr, 2, matrix, buffer, nullptr, nullptr, false).Succeeded() &&
              paused.count == 6 && paused.trace[5] == Op::Submit,
          "a cue delivered after its pause boundary remains submitted and stopped until explicit Resume");

    std::atomic<std::uint32_t> first_pins{}, second_pins{};
    std::atomic<unsigned> callback_wakes{};
    hs::audio_detail::AudioBufferCompletion first, second;
    first.wake_main = +[](void *context) noexcept { static_cast<std::atomic<unsigned> *>(context)->fetch_add(1); };
    first.wake_context = &callback_wakes;
    first.Begin(first_pins, {5, 9, 100}); second.Begin(second_pins, {5, 10, 101});
    first.OnBufferEnd(&second);
    Check(first_pins.load() == 1 && second_pins.load() == 1, "a callback cannot release a different buffer record");
    second.OnBufferEnd(&second); second.OnBufferEnd(&second);
    Check(second_pins.load() == 0 && first_pins.load() == 1 && second.claimed,
          "out-of-order and repeated Flush completions release exactly one matching pin and cannot recycle a live voice record");
    first.OnVoiceError(&first, E_FAIL);
    Check(first.native_error.load() == E_FAIL && first_pins.load() == 1,
          "callback failure only publishes a fixed receipt and retains native-readable PCM");
    Voice native; native.pins = &first_pins;
    auto *native_pointer = &native; auto *record = &first;
    hs::audio_detail::DestroyOwnedVoice(native_pointer, record);
    Check(native.destroyed && native.pin_held_at_destroy && !native_pointer && !record &&
              first_pins.load() == 0 && !first.claimed && callback_wakes.load() == 2,
          "teardown destroys and joins the native voice before releasing its final transition pin");
    second.AfterVoiceQuiescence();
    first.Begin(first_pins, {6, 1, 102});
    Check(first.token != second.token && first_pins.load() == 1,
          "device reset/voice reuse receive distinct exact buffer identities");
    Voice *failed_create{}; record = &first;
    hs::audio_detail::DestroyOwnedVoice(failed_create, record);
    Check(first_pins.load() == 0 && !first.claimed, "failed dormant voice creation releases its reserved pin without touching old playback");
}

void TestAudioDeliverySessions()
{
    auto channels = std::make_unique<hs::RuntimeChannels>();
    auto delivery = std::make_unique<hs::runtime_detail::AudioDelivery>();
    auto simulation = channels->ForSimulation();
    std::vector<std::pair<bool, hs::Sequence>> seen;
    const auto consume = [&](const hs::QueuedAudioEvent &packet) {
        seen.emplace_back(packet.kind == hs::QueuedAudioEvent::Kind::Tick,
            packet.kind == hs::QueuedAudioEvent::Kind::Tick ? packet.tick_info.tick : packet.event.sequence);
    };
    const auto observe = [](const auto &, std::size_t) {};
    const auto event = [](std::uint64_t session, hs::Sequence sequence, hs::Tick tick) {
        hs::PresentationEvent result;
        result.kind = hs::PresentationKind::Audio; result.session_id = session;
        result.sequence = sequence; result.tick = tick; result.asset = hs::MakeAssetId("audio.test");
        return hs::QueuedAudioEvent{result, 0};
    };
    simulation.PublishAudioTick({1, 90, hs::SessionPhase::Playing, {}});
    Check(channels->sim_audio_events.TryPush(event(1, 1, 90)), "queue old session audio");
    simulation.PublishAudioTick({2, 1, hs::SessionPhase::Playing, {}});
    Check(channels->sim_audio_events.TryPush(event(2, 2, 1)) && channels->sim_audio_events.TryPush(event(2, 3, 1)),
          "queue ordered first sounds of restarted session");
    Check(channels->render_audio_events.TryPush(event(1, 4, 90)) && channels->render_audio_events.TryPush(event(2, 5, 1)),
          "render origin keeps a separate producer FIFO");
    delivery->Drain(channels->sim_audio_events, channels->render_audio_events, consume, observe);
    const std::vector<std::pair<bool, hs::Sequence>> expected{{true, 1}, {false, 2}, {false, 3}, {false, 5}};
    Check(seen == expected && delivery->Stats().discarded_old_session == 2,
          "batch discards old sessions before execution, then keeps boundary/first-signal/same-tick FIFO and sim-before-render merge");

    seen.clear();
    for (std::size_t i = 0; i < 1023; ++i)
        Check(channels->sim_audio_events.TryPush(event(2, 100 + i, 1)), "fill final old-session batch");
    simulation.PublishAudioTick({3, 1, hs::SessionPhase::Playing, {}});
    delivery->Drain(channels->sim_audio_events, channels->render_audio_events, consume, observe);
    Check(seen == std::vector<std::pair<bool, hs::Sequence>>{{true, 1}},
          "boundary at the final capacity slot closes every older queued event");
    seen.clear();
    Check(channels->sim_audio_events.TryPush(event(3, 5000, 1)), "new first sound arrives in next batch");
    delivery->Drain(channels->sim_audio_events, channels->render_audio_events, consume, observe);
    Check(seen == std::vector<std::pair<bool, hs::Sequence>>{{false, 5000}},
          "first sound after a batch-final session boundary is never lost or reset again");

    seen.clear(); bool published_late = false;
    Check(channels->render_audio_events.TryPush(event(4, 6000, 1)), "future rendered session becomes visible");
    const auto racing_observe = [&](const hs::QueuedAudioEvent &, std::size_t source) {
        if (source == 1 && !published_late)
        {
            published_late = true;
            simulation.PublishAudioTick({4, 1, hs::SessionPhase::Playing, {}});
            Check(channels->sim_audio_events.TryPush(event(4, 5999, 1)), "publish preceding sim boundary after initial empty pop");
        }
    };
    delivery->Drain(channels->sim_audio_events, channels->render_audio_events, consume, racing_observe);
    Check(seen == std::vector<std::pair<bool, hs::Sequence>>{{true, 1}, {false, 5999}, {false, 6000}},
          "late observation refreshes the preceding simulation boundary before admitting new-session render sound");
    seen.clear();
    simulation.PublishAudioTick({4, 7, hs::SessionPhase::Paused, {}});
    Check(channels->sim_audio_events.TryPush(event(4, 6001, 5)), "queue same-session delayed audio");
    delivery->Drain(channels->sim_audio_events, channels->render_audio_events, consume, observe);
    Check(seen == std::vector<std::pair<bool, hs::Sequence>>{{true, 7}, {false, 6001}} &&
              delivery->Stats().maximum_event_age_ticks == 2,
          "same-session FIFO observes actual event age without inventing an arbitrary expiry cutoff");
    for (std::size_t i = 0; i < 1024; ++i) Check(channels->sim_audio_events.TryPush(event(4, i, 7)), "fill audio queue");
    simulation.PublishAudioTick({5, 0, hs::SessionPhase::Playing, {}});
    Check(channels->dropped_presentation_events.load() == 1,
          "lost session boundary retains the original explicit overflow invalidation");
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

void TestTypedAudioPcm(const std::filesystem::path &root)
{
    const auto path = root / "typed-audio.pcm";
    std::filesystem::create_directories(root);
    std::vector<std::byte> pcm(28800);
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char *>(pcm.data()), static_cast<std::streamsize>(pcm.size()));
        Check(bool(output), "typed audio PCM fixture write");
    }
    hs::ShippedContentOwner owner;
    Check(hs::ShippedContentOwner::Open(path, owner).Succeeded(), "typed audio backing open");
    const std::array samples{hs::AudioSampleRow{0, 14400, 48000, 1, 24},
                             hs::AudioSampleRow{14400, 14400, 48000, 1, 24}};
    const auto digest = hs::Sha256(std::span(pcm).first(14400));
    const std::array chunks{hs::AudioPcmChunk{0, 14400, 0, 1, true, digest},
                           hs::AudioPcmChunk{14400, 14400, 1, 1, false, digest}};
    const auto preload = hs::MakeAssetId("audio.typed.preload");
    const auto transition = hs::MakeAssetId("audio.typed.transition");
    std::array cues{
        hs::AudioCueRow{preload, 0, 1, 1, 0, hs::AudioBus::Sfx, hs::AudioPriority::Other, false, true, false, false},
        hs::AudioCueRow{transition, 1, 1, 1, 0, hs::AudioBus::Bgm, hs::AudioPriority::Other, false, false, false, true}};
    std::ranges::sort(cues, {}, [](const hs::AudioCueRow &cue) { return cue.id.value; });
    hs::runtime_detail::MainWake wake;
    hs::AudioEngine engine;
    hs::SettingsData settings; settings.master_volume = 0;
    const hs::AudioImage image{cues, samples, chunks};
    const auto corrupt_path = root / "typed-audio-corrupt.pcm";
    for (std::size_t damaged_sample = 0; damaged_sample < samples.size(); ++damaged_sample)
    {
        auto damaged = pcm;
        damaged[damaged_sample * 14400] ^= std::byte{1};
        {
            std::ofstream output(corrupt_path, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char *>(damaged.data()), static_cast<std::streamsize>(damaged.size()));
            Check(bool(output), "same-size corrupt PCM fixture write");
        }
        hs::ShippedContentOwner corrupt_owner;
        Check(hs::ShippedContentOwner::Open(corrupt_path, corrupt_owner).Succeeded(), "corrupt PCM backing open");
        hs::AudioEngine rejected;
        const auto initialized = rejected.Initialize(settings, image, corrupt_owner);
        if (damaged_sample == 0)
            Check(!initialized && rejected.Status().loaded_files == 0,
                  "preload digest failure must not expose resident PCM");
        else
        {
            Check(initialized.Succeeded() && rejected.Status().loaded_files == 1,
                  "unloaded transition corruption must not reject a verified preload chunk");
            for (unsigned attempt = 0; attempt < 2; ++attempt)
            {
                const auto failures = rejected.Status().skipped_load_failures;
                const auto failure_deadline = GetTickCount64() + 1000;
                do
                {
                    Check(!rejected.PrepareCue(transition), "corrupt transition became Ready");
                    if (rejected.Status().skipped_load_failures > failures) break;
                    Sleep(1);
                } while (GetTickCount64() < failure_deadline);
                Check(rejected.Status().skipped_load_failures == failures + 1 && rejected.Status().loaded_files == 1,
                      "transition digest failure must retain only verified resident PCM");
                rejected.Stop(transition);
                rejected.UpdateListener({}, {0, 0, 1}, {0, 1, 0});
            }
        }
        rejected.Shutdown();
    }
    std::filesystem::remove(corrupt_path);
    Check(!engine.CaptureTimings().captured, "audio timing capture defaults off");
    engine.SetCaptureTimings(true);
    engine.SetWakeCallback(hs::runtime_detail::MainWake::NotifyOpaque, &wake);
    Check(engine.Initialize(settings, image, owner).Succeeded(), "typed audio initializes without JSON or WAV");
    Check(engine.Status().loaded_cues == 2 && engine.Status().loaded_files == 1, "typed audio preloads only declared samples");
    Check(engine.PrepareCue(preload), "verified resident PCM remains ready without another load");
    std::filesystem::remove(path); // Subsequent reads must use the held content handle.
    const auto deadline = GetTickCount64() + 1000;
    while (!engine.PrepareCue(transition) && GetTickCount64() < deadline) Sleep(1);
    Check(engine.PrepareCue(transition) && engine.Status().loaded_files == 2,
          "typed transition preparation reads headerless PCM from the retained pack");
    hs::PresentationEvent event{};
    event.kind = hs::PresentationKind::Audio; event.asset = transition; event.sequence = 1;
    engine.Play(event);
    engine.Stop(transition);
    const auto completion_deadline = GetTickCount64() + 1000;
    do
    {
        engine.UpdateListener({}, {0, 0, 1}, {0, 1, 0});
        if (engine.Status().loaded_files == 1) break;
        Sleep(1);
    } while (GetTickCount64() < completion_deadline);
    Check(engine.Status().loaded_files == 1, "stopped transition PCM releases only after native completion");
    const auto reload_deadline = GetTickCount64() + 1000;
    while (!engine.PrepareCue(transition) && GetTickCount64() < reload_deadline) Sleep(1);
    Check(engine.PrepareCue(transition) && engine.Status().loaded_files == 2,
          "evicted transition reloads and verifies through the retained content handle");
    const auto work = engine.CaptureTimings();
    Check(work.captured && work.qpc_frequency != 0 && work.reader_jobs == 2 && work.reader_failures == 0 &&
              work.reader_bytes == 28800 && work.scopes[static_cast<std::size_t>(hs::AudioWorkId::PrepareCue)].calls != 0 &&
              work.scopes[static_cast<std::size_t>(hs::AudioWorkId::Play)].calls == 1,
          "opt-in audio work counts actual verified worker loads and API activity without counting resident reuse as IO");
    engine.Stop(transition);
    engine.UpdateListener({}, {0, 0, 1}, {0, 1, 0});
    (void)engine.PrepareCue(transition);
    engine.Shutdown(); // Joins a queued/in-flight preparation before owner destruction.
    Check(engine.Status().loaded_files == 0, "shutdown releases typed PCM and pending preparation");
}

void TestPreparedNonLoopTransition(const std::filesystem::path &root)
{
    const auto path = root / "prepared-stinger.pcm";
    const std::vector<std::byte> pcm(14400);
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char *>(pcm.data()), static_cast<std::streamsize>(pcm.size()));
        Check(bool(file), "write nonloop transition fixture");
    }
    hs::ShippedContentOwner content;
    Check(hs::ShippedContentOwner::Open(path, content).Succeeded(), "open nonloop transition backing");
    const auto id = hs::MakeAssetId("audio.stinger.defeat");
    const std::array cues{hs::AudioCueRow{id, 0, 1, 1, 0, hs::AudioBus::Bgm,
        hs::AudioPriority::Other, false, false, false, false}};
    const std::array samples{hs::AudioSampleRow{0, pcm.size(), 48000, 1, 24}};
    const std::array chunks{hs::AudioPcmChunk{0, pcm.size(), 0, 1, false, hs::Sha256(pcm)}};
    hs::runtime_detail::MainWake wake;
    hs::AudioEngine engine;
    hs::SettingsData settings; settings.master_volume = 0;
    engine.SetCaptureTimings(true);
    engine.SetWakeCallback(hs::runtime_detail::MainWake::NotifyOpaque, &wake);
    Check(engine.Initialize(settings, {cues, samples, chunks}, content).Succeeded(), "initialize transition-only bank");
    hs::PresentationEvent event;
    event.kind = hs::PresentationKind::Audio; event.asset = id; event.sequence = 17;
    Check(engine.Play(event) == hs::AudioPlayResult::NotReady && engine.Status().loaded_files == 0 &&
              engine.CaptureTimings().reader_jobs == 0,
          "cold Play reports not ready and never schedules disk work or hides a pending replay");
    const auto prior_failures = engine.Status().skipped_load_failures;
    hs::Sequence sequence{};
    auto owner = std::make_unique<hs::runtime_detail::MainAudioSession>(engine, sequence);
    auto channels = std::make_unique<hs::RuntimeChannels>();
    auto simulation = channels->ForSimulation();
    simulation.PublishAudioTick({1, 1, hs::SessionPhase::Defeat, {}});
    event.session_id = 1; event.tick = 1;
    Check(channels->sim_audio_events.TryPush({event, 0}), "queue same-tick projected defeat stinger");
    owner->Drain(channels->sim_audio_events, channels->render_audio_events, [](const auto &, std::size_t) {});
    Check(owner->HasPendingMusic(), "Main retains a nonloop transition intent while complete PCM prepares");
    const auto deadline = GetTickCount64() + 1000;
    while (owner->HasPendingMusic() && GetTickCount64() < deadline)
    {
        owner->Update();
        if (owner->HasPendingMusic()) Sleep(1);
    }
    Check(!owner->HasPendingMusic() && owner->ActiveMusic().value == id.value &&
              owner->Failure() == hs::runtime_detail::MainAudioFailure::None &&
              engine.CueReadiness(id) == hs::AudioCueReadiness::Ready &&
              engine.Status().skipped_load_failures == prior_failures && engine.CaptureTimings().reader_jobs == 1,
          "cold nonloop stinger starts only after verified Ready instead of being silently dropped");
    Check(WaitForSingleObject(wake.Handle(), 0) == WAIT_OBJECT_0,
          "worker Ready notifies Main independently of rendering or another simulation sound");
    owner.reset();
    engine.Shutdown();
}

void TestPreparedSessionAmbience(const std::filesystem::path &root)
{
    constexpr std::size_t bytes = 14400;
    const std::vector<std::byte> pcm(bytes * 6);
    const auto ambience = hs::MakeAssetId("audio.ambience.arena");
    const auto menu = hs::MakeAssetId("audio.bgm.main_menu"), music = hs::MakeAssetId("audio.bgm.session");
    std::array cues{
        hs::AudioCueRow{ambience, 0, 2, 1, 50, hs::AudioBus::Sfx, hs::AudioPriority::Other, false, false, false, true},
        hs::AudioCueRow{menu, 2, 1, 1, 50, hs::AudioBus::Bgm, hs::AudioPriority::Other, false, false, false, true},
        hs::AudioCueRow{music, 3, 1, 1, 50, hs::AudioBus::Bgm, hs::AudioPriority::Other, false, false, false, true},
        hs::AudioCueRow{hs::MakeAssetId("audio.ui.pause_open"), 4, 1, 1, 0, hs::AudioBus::Ui, hs::AudioPriority::Ui, false, true, false, false},
        hs::AudioCueRow{hs::MakeAssetId("audio.ui.pause_close"), 5, 1, 1, 0, hs::AudioBus::Ui, hs::AudioPriority::Ui, false, true, false, false}};
    std::ranges::sort(cues, {}, [](const auto &cue) { return cue.id.value; });
    std::array<hs::AudioSampleRow, 6> samples;
    for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = {i * bytes, bytes, 48000, 1, 24};
    const auto digest = hs::Sha256(std::span(pcm).first(bytes));
    const std::array chunks{
        hs::AudioPcmChunk{0, bytes, 0, 1, false, digest}, hs::AudioPcmChunk{bytes, bytes, 1, 1, false, digest},
        hs::AudioPcmChunk{bytes * 2, bytes, 2, 1, false, digest}, hs::AudioPcmChunk{bytes * 3, bytes, 3, 1, false, digest},
        hs::AudioPcmChunk{bytes * 4, bytes * 2, 4, 2, true, hs::Sha256(std::span(pcm).subspan(bytes * 4))}};
    for (unsigned scenario = 0; scenario < 3; ++scenario)
    {
        const bool corrupt = scenario == 2;
        const auto path = root / ("session-ambience-" + std::to_string(scenario) + ".pcm");
        auto stored = pcm;
        if (corrupt) stored[0] ^= std::byte{1};
        {
            std::ofstream file(path, std::ios::binary);
            file.write(reinterpret_cast<const char *>(stored.data()), static_cast<std::streamsize>(stored.size()));
            Check(bool(file), "write actual-shape ambience/session fixture");
        }
        hs::ShippedContentOwner content;
        Check(hs::ShippedContentOwner::Open(path, content).Succeeded(), "open ambience/session fixture");
        hs::runtime_detail::MainWake wake;
        hs::AudioEngine engine;
        hs::SettingsData settings; settings.master_volume = 0;
        engine.SetCaptureTimings(true);
        engine.SetWakeCallback(hs::runtime_detail::MainWake::NotifyOpaque, &wake);
        Check(engine.Initialize(settings, {cues, samples, chunks}, content).Succeeded() && engine.Status().loaded_files == 2,
            "session bank retains preloaded UI but deliberately leaves two SFX ambience variants cold");
        Check(!engine.IsTransitionTrack(ambience) && engine.CueReadiness(ambience) == hs::AudioCueReadiness::Pending,
            "actual arena ambience is nonpreloaded SFX, independently owned from BGM transitions");
        hs::Sequence sequence{};
        auto owner = std::make_unique<hs::runtime_detail::MainAudioSession>(engine, sequence);
        auto channels = std::make_unique<hs::RuntimeChannels>();
        auto simulation = channels->ForSimulation();
        const auto boundary = [&](std::uint64_t session, hs::Tick tick, hs::SessionPhase phase) {
            simulation.PublishAudioTick({session, tick, phase, {}});
            Check(channels->dropped_presentation_events.load() == 0, "publish ambience session boundary");
            owner->Drain(channels->sim_audio_events, channels->render_audio_events, [](const auto &, std::size_t) {});
        };
        const auto pump = [&](const auto &done) {
            const auto deadline = GetTickCount64() + 1000;
            do
            {
                owner->Update();
                if (done()) return;
                Sleep(1);
            } while (GetTickCount64() < deadline);
            Check(false, "session ambience worker/main completion deadline");
        };
        boundary(1, 1, hs::SessionPhase::Playing);
        Check(owner->Failure() == hs::runtime_detail::MainAudioFailure::None && owner->HasPendingAmbience() &&
            owner->ActiveAmbience().value == 0 && engine.CaptureTimings().scopes[static_cast<std::size_t>(hs::AudioWorkId::Play)].calls == 0,
            "first Playing boundary prepares the original ambience event without an illegal cold Play");
        if (corrupt)
        {
            pump([&] { return owner->Failure() != hs::runtime_detail::MainAudioFailure::None; });
            const auto &failure = owner->FailureDetails();
            Check(owner->Failure() == hs::runtime_detail::MainAudioFailure::Preparation && failure.cue.value == ambience.value &&
                failure.readiness == hs::AudioCueReadiness::Failed && failure.session_id == 1 && failure.tick == 1 &&
                failure.phase == hs::SessionPhase::Playing && std::string_view(failure.stage) == "ambience_prepare" &&
                owner->ActiveAmbience().value == 0 && engine.Status().skipped_load_failures != 0 &&
                engine.Status().native_operation == hs::AudioNativeOperation::None,
                "same-size corrupt ambience remains a real preparation failure with exact cue/session metadata");
        }
        else
        {
            if (scenario == 1)
            {
                boundary(1, 2, hs::SessionPhase::Paused); // Before Main ever attempts the pending ambience.
                Check(!owner->HasPendingAmbience() && owner->ActiveAmbience().value == 0 && engine.Status().combat_paused,
                    "pause cancels an unsubmitted ambience intent instead of replaying it after worker Ready");
                pump([&] { return wake.Sequence() >= 4 && engine.CaptureTimings().reader_jobs >= 4 && !owner->HasPendingMusic(); });
                owner->Update();
                Check(owner->Failure() == hs::runtime_detail::MainAudioFailure::None && owner->ActiveAmbience().value == 0 &&
                    engine.CueReadiness(ambience) != hs::AudioCueReadiness::Ready,
                    "late Ready from a cancelled ambience is reclaimed while paused");
                boundary(1, 3, hs::SessionPhase::Playing);
            }
            pump([&] { return owner->ActiveAmbience().value == ambience.value; });
            Check(!owner->HasPendingAmbience() && owner->Failure() == hs::runtime_detail::MainAudioFailure::None &&
                engine.CueReadiness(ambience) == hs::AudioCueReadiness::Ready && engine.Status().skipped_load_failures == 0,
                "both nonpreloaded ambience variants verify before the retained event starts with no false load failure");
            const auto original_sequence = sequence;
            boundary(1, 4, hs::SessionPhase::Playing);
            owner->Update();
            Check(sequence == original_sequence && owner->ActiveAmbience().value == ambience.value,
                "repeated Playing ticks do not regenerate the sequence or retrigger the loop");
            const auto reads = engine.CaptureTimings().reader_jobs;
            engine.SetBackgroundMuted(true);
            boundary(2, 1, hs::SessionPhase::Playing);
            owner->Update();
            Check(owner->Failure() == hs::runtime_detail::MainAudioFailure::None && owner->ActiveAmbience().value == ambience.value &&
                engine.CaptureTimings().reader_jobs == reads && engine.Status().background_muted,
                "new-session first ambience reuses verified resident bytes after old native retirement without changing background policy");
        }
        owner.reset();
        engine.Shutdown();
    }
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
    const auto preparation_deadline = GetTickCount64() + 1000;
    while (!engine.PrepareCue(event.asset) && GetTickCount64() < preparation_deadline) Sleep(1);
    Check(engine.PrepareCue(event.asset), "lazy PCM is prepared asynchronously before playback");
    engine.Play(event);
    status = engine.Status();
    Check(status.loaded_files == 1 && status.skipped_load_failures == 0,
          "Play consumes an already prepared PCM payload");
    engine.Play(event);
    Check(engine.Status().loaded_files == 1,
          "cached PCM payload is not loaded again");

    engine.Shutdown();
    wav.open(directory / "spatial.wav", std::ios::binary | std::ios::trunc);
    wav.write("RIFF", 4);
    write_u32(36 + 48000 * 3);
    wav.write("WAVEfmt ", 8);
    write_u32(16);
    write_u16(1);
    write_u16(1);
    write_u32(48000);
    write_u32(144000);
    write_u16(3);
    write_u16(24);
    wav.write("data", 4);
    write_u32(48000 * 3);
    const std::vector<char> silence(48000 * 3);
    wav.write(silence.data(), static_cast<std::streamsize>(silence.size()));
    wav.close();
    std::filesystem::copy_file(directory / "spatial.wav", directory / "xp.wav",
                               std::filesystem::copy_options::overwrite_existing);
    std::ofstream(directory / "audio_cues.json") << R"({"entries":[
      {"id":"audio.spatial","asset_id":"audio.spatial","submix":"SFX","spatial":true,"preload":true,"streaming":false,"encoding":"PCM","loop":true,"priority":"Other","max_simultaneous":2,"minimum_retrigger_ms":0,"files":["spatial.wav"]},
      {"id":"audio.pickup.xp_collect","asset_id":"audio.pickup.xp_collect","submix":"UI","spatial":false,"preload":true,"streaming":false,"encoding":"PCM","loop":true,"priority":"UI","max_simultaneous":4,"minimum_retrigger_ms":60000,"files":["xp.wav"]}
    ]})";
    Check(engine.Initialize(settings, directory).Succeeded(), "spatial admission catalog initializes");
    if (engine.Status().device_state == hs::AudioDeviceState::Ready)
    {
        event.asset = hs::MakeAssetId("audio.spatial");
        event.position = {3.0f, 0.0f, 1.0f};
        engine.Play(event);
        engine.Play(event);
        engine.Play(event);
        Check(engine.Status().played_count == 2 && engine.Status().active_source_voices == 2,
              "single admission scan retains the per-cue voice cap");
        engine.UpdateListener({}, {0, 0, 1}, {0, 1, 0});
        engine.UpdateListener({}, {0, 0, 1}, {0, 1, 0});
        engine.UpdateListener({1, 0, 0}, {1, 0, 0}, {0, 1, 0});
        Check(engine.Status().active_source_voices == 2 && engine.Status().native_error == 0,
              "unchanged and moved listeners retain spatial voice routing");
        const auto wait_for_native_drain = [&] {
            // FlushSourceBuffers completes asynchronously; observe BuffersQueued through Reclaim.
            const auto deadline = GetTickCount64() + 1000;
            while (engine.Status().active_source_voices != 0 && GetTickCount64() < deadline)
            {
                Sleep(1);
                engine.UpdateListener({1, 0, 0}, {1, 0, 0}, {0, 1, 0});
            }
            Check(engine.Status().active_source_voices == 0 && engine.Status().native_error == 0,
                  "stopped native buffers drain within one second without errors");
        };
        engine.Stop(event.asset);
        wait_for_native_drain();
        engine.Play(event);
        Check(engine.Status().played_count == 3 && engine.Status().active_source_voices == 1,
              "admission observes Stop completion and reuses the first free voice");
        event.asset = hs::MakeAssetId("audio.pickup.xp_collect");
        engine.Play(event);
        engine.Play(event);
        Check(engine.Status().played_count == 4 && engine.Status().active_source_voices == 2,
              "cue-owned retrigger timestamp preserves XP aggregation without another voice");
        engine.StopBus(hs::AudioBus::Sfx);
        engine.StopBus(hs::AudioBus::Ui);
        wait_for_native_drain();
        Check(engine.Status().active_source_voices == 0, "all native buffers release before shutdown");
        event.asset = hs::MakeAssetId("audio.spatial"); event.session_id = 77;
        engine.BeginSession(77); engine.Play(event);
        event.asset = hs::MakeAssetId("audio.pickup.xp_collect"); engine.Play(event);
        engine.BeginSession(78);
        Check(engine.Status().active_source_voices == 0,
              "session close quiesces old scoped voices before new event admission");
        const auto before_first = engine.Status().played_count;
        event.session_id = 78; ++event.sequence; engine.Play(event);
        ++event.sequence; engine.Play(event);
        event.session_id = 0; ++event.sequence; engine.Play(event);
        Check(engine.Status().played_count == before_first + 1,
              "new-session first cue survives reset, then same-tick and Main-origin calls share the original retrigger clock");
        engine.StopBus(hs::AudioBus::Ui); wait_for_native_drain();
    }

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

void TestSimulationSignalFanout()
{
    auto storage = std::make_unique<hs::RuntimeChannels>();
    auto &channels = *storage;
    auto simulation = channels.ForSimulation();
    auto rendering = channels.ForRender();
    std::array<hs::DomainSignal, 2> signals{};
    for (std::size_t row = 0; row < signals.size(); ++row)
    {
        auto &signal = signals[row];
        signal.kind = hs::DomainSignalKind::ArrowReleased;
        signal.sequence = 101 + row;
        signal.tick = 42;
        signal.session_id = 7;
        signal.context = static_cast<std::uint8_t>(hs::SkillKind::BasicAttack);
        signal.position = {static_cast<float>(row + 1), 1.05f, -3.0f};
        signal.direction = {1, 0, 0};
    }
    std::vector<hs::PresentationEvent> expected;
    for (const auto &signal : signals)
    {
        std::array<hs::PresentationEvent, 3> projected{};
        const auto count = hs::ProjectDomainSignal(signal, projected, 12.0f);
        Check(count == 3 && projected[0].kind == hs::PresentationKind::Vfx &&
                  projected[1].kind == hs::PresentationKind::Audio &&
                  projected[2].kind == hs::PresentationKind::Audio,
              "arrow signal retains visual then two ordered audio values");
        expected.insert(expected.end(), projected.begin(), projected.begin() + count);
    }
    const auto same = [](const auto &a, const auto &b) {
        return a.sequence == b.sequence && a.tick == b.tick && a.kind == b.kind &&
            a.session_id == b.session_id && a.asset.value == b.asset.value && a.parameters == b.parameters &&
            a.position.x == b.position.x && a.position.y == b.position.y && a.position.z == b.position.z &&
            a.vfx_ratio01 == b.vfx_ratio01 && a.status_episode_generation == b.status_episode_generation &&
            a.upgrade_skill == b.upgrade_skill && a.upgrade_index == b.upgrade_index &&
            a.upgrade_stage == b.upgrade_stage && a.upgrade_cast_id == b.upgrade_cast_id &&
            a.upgrade_owner_id == b.upgrade_owner_id;
    };
    std::vector<hs::PresentationEvent> recorded(1);
    simulation.ProjectDomainSignalsOnce(signals, 12.0f, false, &recorded);
    Check(recorded.size() == expected.size(), "per-tick record buffer resets and keeps every ordered value");
    hs::PresentationEvent event;
    hs::QueuedAudioEvent queued;
    for (std::size_t row = 0; row < expected.size(); ++row)
    {
        Check(same(recorded[row], expected[row]), "recording observes exactly the projected owned values");
        if (expected[row].kind == hs::PresentationKind::Audio)
            Check(channels.sim_audio_events.TryPop(queued) && same(queued.event, expected[row]) && queued.enqueued_qpc == 0,
                  "simulation audio reaches Main in source order");
        else
            Check(channels.presentation_events.TryPop(event) && same(event, expected[row]),
                  "normal render destination contains each visual once");
    }
    Check(channels.presentation_events.Size() == 0 && channels.sim_audio_events.Size() == 0 &&
              channels.render_audio_events.Size() == 0 && channels.dropped_presentation_events.load() == 0,
          "normal fanout has no duplicate audio or borrowed queue payload");
    simulation.capture_clock = +[]() noexcept -> std::uint64_t { return 1234; };

    hs::PresentationEvent footstep{};
    footstep.kind = hs::PresentationKind::Audio;
    footstep.sequence = 900;
    footstep.asset = hs::MakeAssetId("audio.player.footstep_walk");
    Check(rendering.render_audio_events.TryPush({footstep, 17}), "render keeps its own audio producer queue");
    simulation.ProjectDomainSignalsOnce(signals, 12.0f, true, &recorded);
    for (const auto &wanted : expected)
        Check(channels.presentation_events.TryPop(event) && same(event, wanted),
              "explicit capture preserves the original mixed diagnostic trace order");
    for (const auto &wanted : expected)
        if (wanted.kind == hs::PresentationKind::Audio)
            Check(channels.sim_audio_events.TryPop(queued) && same(queued.event, wanted) && queued.enqueued_qpc == 1234,
                  "diagnostic trace copies do not delay or duplicate live simulation audio");
    Check(channels.render_audio_events.TryPop(queued) && same(queued.event, footstep) && queued.enqueued_qpc == 17 &&
              !channels.render_audio_events.TryPop(queued),
          "simulation fanout cannot write or reorder render footsteps");

    for (std::size_t row = 0; row < 1024; ++row)
        Check(channels.sim_audio_events.TryPush({footstep, 0}), "fill simulation audio destination");
    simulation.ProjectDomainSignalsOnce(signals, 12.0f, false, &recorded);
    Check(channels.dropped_presentation_events.load() == 4 && recorded.size() == expected.size() &&
              channels.presentation_events.Size() == 2,
          "live audio overflow preserves exact recording and the existing drop invalidation policy");
    channels.sim_audio_events.ClearConsumer();
    channels.presentation_events.ClearConsumer();
    channels.dropped_presentation_events.store(0);
    for (std::size_t row = 0; row < 8192; ++row)
        Check(channels.presentation_events.TryPush(expected[0]), "fill render and diagnostic destination");
    simulation.ProjectDomainSignalsOnce(signals, 12.0f, true, &recorded);
    Check(channels.dropped_presentation_events.load() == expected.size() &&
              channels.sim_audio_events.Size() == 4 && recorded.size() == expected.size(),
          "stalled rendering cannot block audio and lost trace rows still invalidate diagnostic completeness");
    for (std::size_t row = 0; row < expected.size(); ++row)
        Check(same(recorded[row], expected[row]), "queue overflow never changes exact recorded event order");
}

void TestCharacterDetailsTextFit()
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IDWriteFactory> base;
    Check(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
              reinterpret_cast<IUnknown **>(base.GetAddressOf()))), "create text-fit factory");
    ComPtr<IDWriteFactory5> factory;
    Check(SUCCEEDED(base.As(&factory)), "query bundled-font factory");
    ComPtr<IDWriteInMemoryFontFileLoader> memory_loader;
    struct UnregisterFont { IDWriteFactory5 *factory; IDWriteFontFileLoader *loader{}; ~UnregisterFont(){if(loader)factory->UnregisterFontFileLoader(loader);} } unregister{factory.Get()};
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

    hs::ShippedContentOwner packed_owner;
    Check(hs::shipping::OpenImage(hs::CurrentExecutableDirectory(),hs::shipping::ImageInfo(),packed_owner).Succeeded(),"open packed text-fit font");
    std::vector<std::byte> packed_font;
    Check(hs::shipping::ReadFont(packed_owner,hs::shipping::ImageInfo(),packed_font).Succeeded(),"read packed text-fit font");
    std::ifstream font_stream(directory / L"Fonts" / L"NotoSansKR.ttf",std::ios::binary);
    const std::vector<char> loose_font((std::istreambuf_iterator<char>(font_stream)),{});
    Check(packed_font.size()==loose_font.size() && std::equal(packed_font.begin(),packed_font.end(),loose_font.begin(),
        [](std::byte a,char b){return std::to_integer<unsigned char>(a)==static_cast<unsigned char>(b);}),"packed and file font bytes match");
    Check(SUCCEEDED(factory->CreateInMemoryFontFileLoader(&memory_loader)) && SUCCEEDED(factory->RegisterFontFileLoader(memory_loader.Get())),"register memory font loader");
    unregister.loader=memory_loader.Get();
    {
        ComPtr<IDWriteFontFile> truncated;
        Check(SUCCEEDED(memory_loader->CreateInMemoryFontFileReference(factory.Get(),packed_font.data(),16,nullptr,&truncated)),"create truncated memory reference");
        BOOL supported=FALSE;DWRITE_FONT_FILE_TYPE file_type{};DWRITE_FONT_FACE_TYPE face_type{};UINT32 faces{};
        const auto analyzed=truncated->Analyze(&supported,&file_type,&face_type,&faces);
        Check(FAILED(analyzed)||!supported||faces==0,"truncated font must not be accepted");
    }
    ComPtr<IDWriteFontFile> memory_file;
    Check(SUCCEEDED(memory_loader->CreateInMemoryFontFileReference(factory.Get(),packed_font.data(),static_cast<UINT32>(packed_font.size()),nullptr,&memory_file)),"create owned memory font");
    std::fill(packed_font.begin(),packed_font.end(),std::byte{0});packed_font.clear();packed_font.shrink_to_fit();
    packed_owner=hs::ShippedContentOwner{}; // Native copy must outlive both input buffer and file handle.
    ComPtr<IDWriteFontSetBuilder1> memory_builder;
    ComPtr<IDWriteFontSet> memory_set;
    ComPtr<IDWriteFontCollection1> memory_collection;
    Check(SUCCEEDED(factory->CreateFontSetBuilder(&memory_builder)) && SUCCEEDED(memory_builder->AddFontFile(memory_file.Get())) &&
        SUCCEEDED(memory_builder->CreateFontSet(&memory_set)) && SUCCEEDED(factory->CreateFontCollectionFromFontSet(memory_set.Get(),&memory_collection)),"memory font survives source retirement");
    auto file_collection=collection;
    collection=memory_collection;
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

        ComPtr<IDWriteTextFormat> file_format;
        ComPtr<IDWriteTextLayout> file_layout;
        Check(SUCCEEDED(factory->CreateTextFormat(family_name.c_str(),file_collection.Get(),DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,static_cast<float>(entry.font_pixels),L"ko-kr",&file_format)) &&
            SUCCEEDED(file_format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP)) &&
            SUCCEEDED(factory->CreateTextLayout(wide.data(),count,file_format.Get(),entry.size_pixels.x,1080,&file_layout)),"create file font comparison layout");
        DWRITE_TEXT_METRICS file_metrics{};
        Check(SUCCEEDED(file_layout->GetMetrics(&file_metrics)) && metrics.left==file_metrics.left && metrics.top==file_metrics.top &&
            metrics.width==file_metrics.width && metrics.widthIncludingTrailingWhitespace==file_metrics.widthIncludingTrailingWhitespace &&
            metrics.height==file_metrics.height && metrics.layoutWidth==file_metrics.layoutWidth && metrics.layoutHeight==file_metrics.layoutHeight &&
            metrics.maxBidiReorderingDepth==file_metrics.maxBidiReorderingDepth && metrics.lineCount==file_metrics.lineCount,"memory/file text layout equivalence");
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

void TestShippingImage();


// Explicit semantic fields, independent of generator inventory and object padding.
std::vector<std::uint64_t> SpawnFields(const hs::ParticleSpawnCommand &c)
{
    std::vector<std::uint64_t> f{c.sequence,c.tick,static_cast<std::uint64_t>(c.shape),static_cast<std::uint64_t>(c.velocity_mode),
        static_cast<std::uint64_t>(c.facing),static_cast<std::uint64_t>(c.renderer),static_cast<std::uint64_t>(c.primitive),
        c.mesh_index,c.sprite,c.frame_columns,c.frame_rows,c.count,c.seed,c.authored_ballistic,c.authored_birth_tick,c.authored_gradient_row};
    for(float v : {c.position.x,c.position.y,c.position.z,c.shape_extent.x,c.shape_extent.y,c.shape_extent.z,
        c.direction.x,c.direction.y,c.direction.z,c.speed_min,c.speed_max,c.cone_radians,c.lifetime_min,c.lifetime_max,
        c.start_color.x,c.start_color.y,c.start_color.z,c.start_color.w,c.end_color.x,c.end_color.y,c.end_color.z,c.end_color.w,
        c.start_size_min,c.start_size_max,c.end_size_min,c.end_size_max,c.gravity,c.rotation_min,c.rotation_max,
        c.angular_velocity_min,c.angular_velocity_max,c.stretch,c.uv_repeat,c.scroll_speed,
        c.authored_initial_velocity.x,c.authored_initial_velocity.y,c.authored_initial_velocity.z,c.authored_drag,
        c.authored_bounce,c.authored_ground_y,c.authored_collision_radius,c.authored_hdr,c.authored_birth_fraction})
        f.push_back(std::bit_cast<std::uint32_t>(v));
    return f;
}
std::vector<std::uint64_t> LineFields(const hs::VfxLineSpawnCommand &c)
{
    std::vector<std::uint64_t> f{c.sequence,c.tick,c.sprite,c.frame_columns,c.frame_rows,static_cast<std::uint64_t>(c.primitive)};
    for(float v : {c.start.x,c.start.y,c.start.z,c.end.x,c.end.y,c.end.z,c.color.x,c.color.y,c.color.z,c.color.w,
        c.width,c.lifetime,c.uv_repeat,c.scroll_speed}) f.push_back(std::bit_cast<std::uint32_t>(v));
    return f;
}
void TestCompiledVfxImage()
{
    hs::VfxCatalog loaded,compiled;
    Check(hs::VfxCatalog::Load(test_generation::ProductPath("vfx/particle_effects.hsbin"),
        test_generation::ProductPath("vfx/vfx_program.hsbin"),loaded).Succeeded(),"load independent VFX reference");
    const auto &image=hs::shipping::Vfx(); const auto &ref=loaded.Image();
    Check(&image==&hs::shipping::Vfx(),"compiled VFX stable identity"); compiled.Bind(image);
    Check(image.payload_hash==ref.payload_hash && image.sprite_count==ref.sprite_count &&
        image.has_typed_wavefront==ref.has_typed_wavefront && image.has_required_sprite_bindings==ref.has_required_sprite_bindings &&
        image.definitions.size()==ref.definitions.size() && image.emitters.size()==ref.emitters.size(),"compiled VFX identity and extents");
    Check(std::ranges::equal(image.authored_mesh_indices,ref.authored_mesh_indices) &&
        std::ranges::equal(image.decorative_definitions,ref.decorative_definitions),"compiled legacy static policies");
    Check(image.program.effect_lookup.size()==ref.program.effect_lookup.size(),"compiled VFX lookup extent");
    for(std::size_t i=0;i<ref.program.effect_lookup.size();++i)
        Check(image.program.effect_lookup[i].effect_id==ref.program.effect_lookup[i].effect_id &&
            image.program.effect_lookup[i].handle==ref.program.effect_lookup[i].handle,"compiled ID to handle relation");
    Check(image.sprites.size()==ref.sprites.size(),"compiled sprite extent");
    for(std::size_t i=0;i<ref.sprites.size();++i)
        Check(image.sprites[i].sprite_id==ref.sprites[i].sprite_id && image.sprites[i].index==ref.sprites[i].index &&
            image.sprites[i].frame_columns==ref.sprites[i].frame_columns && image.sprites[i].frame_rows==ref.sprites[i].frame_rows,
            "compiled sprite relations");
    const auto bindings=[](const hs::RendererVfxSpriteBindings &b){
        std::vector<std::uint32_t> f;
        for(auto v:{b.bleed_status_sprite,b.burn_status_sprite,b.slow_status_sprite,b.mark_status_sprite,b.trap_sprite,b.fire_area_sprite})
            f.insert(f.end(),{v.sprite,v.frame_columns,v.frame_rows});
        return f;
    };
    Check(bindings(image.sprite_bindings)==bindings(ref.sprite_bindings),"compiled sprite bindings");
    std::size_t particles{},lines{};
    for(std::size_t i=0;i<ref.definitions.size();++i){
        const auto &a=image.definitions[i], &b=ref.definitions[i];
        Check(a.effect_id==b.effect_id && a.first_emitter==b.first_emitter && a.emitter_count==b.emitter_count && a.kind==b.kind,
            "compiled definition order and ranges");
        for(float fraction:{0.0f,0.5f,1.0f})
        for(auto geometry:{hs::PresentationGeometryKind::None,hs::PresentationGeometryKind::RingGaps}){
            hs::PresentationEvent event; event.kind=hs::PresentationKind::Vfx; event.asset={b.effect_id};
            event.sequence=0x123456789ull+i; event.tick=137+i; event.position={-3.25f,2.5f,7.75f}; event.geometry.kind=geometry;
            hs::VfxEventParameters params; params.direction={0.6f,0.2f,-0.8f}; params.scale=1.375f;
            params.target={8.5f,1.25f,-4.75f}; params.flags=static_cast<std::uint32_t>(hs::VfxEventFlag::HasTarget);
            event.parameters=hs::EncodeVfxParameters(params);
            std::vector<hs::ParticleSpawnCommand> expected,actual; std::vector<hs::VfxLineSpawnCommand> el,al;
            Check(loaded.ExpandEvent(event,fraction,expected,el).Succeeded() && compiled.ExpandEvent(event,fraction,actual,al).Succeeded(),
                "all authored legacy events expand");
            Check(expected.size()==actual.size() && el.size()==al.size(),"compiled emission counts");
            for(std::size_t j=0;j<expected.size();++j) Check(SpawnFields(expected[j])==SpawnFields(actual[j]),"compiled ordered particle fields and seed");
            for(std::size_t j=0;j<el.size();++j) Check(LineFields(el[j])==LineFields(al[j]),"compiled ordered line fields");
            particles+=expected.size(); lines+=el.size();
        }
    }
    Check(particles>0 && lines>0,"comparison exercises both command families");
    const auto *backing=loaded.Image().definitions.data(); loaded.Bind(loaded.Image());
    Check(loaded.Image().definitions.data()==backing,"self-bind preserves owned backing");
    hs::VfxCatalog moved(std::move(loaded));
    Check(loaded.Image().definitions.empty() && moved.Image().definitions.data()==backing,"move clears source and transfers owner");
    moved.Swap(compiled);
    Check(compiled.Image().definitions.data()==backing && moved.Image().definitions.data()==image.definitions.data(),"swap pairs spans with owner");
}

void TestCompiledShippingTables()
{
    TestCompiledVfxImage();
    hs::ShippedContentOwner owner;
    Check(hs::shipping::OpenImage(hs::CurrentExecutableDirectory(), hs::shipping::ImageInfo(), owner).Succeeded(),
          "compiled image must open its immutable bundle");
    hs::PresentationCatalog reference;
    std::uint64_t hash{};
    Check(hs::LoadPresentationCatalog(test_generation::ProductPath("game-data/presentation-catalog"), reference, &hash).Succeeded(),
          "reference presentation must load");
    const auto &compiled = hs::shipping::Presentation();
    Check(&compiled == &hs::shipping::Presentation() && hash == hs::shipping::ImageInfo().content_hash,
          "compiled presentation identity");
    Check(compiled.relic_names == reference.relic_names && compiled.relic_rules == reference.relic_rules,
          "compiled presentation text bytes");
    const auto same = [](float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); };
    Check(same(compiled.camera.yaw_degrees, reference.camera.yaw_degrees) &&
          same(compiled.camera.pitch_degrees, reference.camera.pitch_degrees) &&
          same(compiled.camera.vertical_fov_degrees, reference.camera.vertical_fov_degrees) &&
          same(compiled.camera.distance_m, reference.camera.distance_m) &&
          compiled.camera.minimum_distance_percent == reference.camera.minimum_distance_percent &&
          compiled.camera.maximum_distance_percent == reference.camera.maximum_distance_percent,
          "compiled presentation camera bits");

    hs::SimulationRules reference_rules;
    Check(hs::LoadSimulationRules(test_generation::ProductPath("game-data/simulation-rules"), reference_rules).Succeeded(),
          "reference simulation rules must load");
    const auto &compiled_rules = hs::shipping::Simulation();
    Check(&compiled_rules == &hs::shipping::Simulation(), "compiled simulation rules have a stable address");
    Check(hs::CanonicalSimulationRulesBytes(compiled_rules) == hs::CanonicalSimulationRulesBytes(reference_rules),
          "compiled simulation rules preserve all canonical field bytes");
    Check(hs::SimulationRulesHash(compiled_rules) == hs::SimulationRulesHash(reference_rules),
          "compiled simulation rules preserve the gameplay rules hash");

    // The independently loaded cooked rules remain the behavioral reference.
    // Both worlds use unchanged production rules and the same scripted input.
    hs::SimulationConfig config;
    config.seed = 0x5a674bc;
    config.automatic_choices = true;
    config.scenario.player_invulnerable = true;
    hs::GameSimulation loaded_world, compiled_world;
    Check(loaded_world.Initialize(config, reference_rules).Succeeded() &&
              compiled_world.Initialize(config, compiled_rules).Succeeded(), "shipping rules simulation initialization");
    const hs::DebugCommand grant{hs::DebugCommandKind::GrantSkill, static_cast<std::uint64_t>(hs::SkillKind::PiercingShot)};
    Check(loaded_world.ApplyDebugCommand(grant).Succeeded() && compiled_world.ApplyDebugCommand(grant).Succeeded(),
          "shipping rules fixture must equip its active skill");
    const auto initial_position = loaded_world.GetObservation().player_position;
    bool moved = false;
    hs::Sequence sequence{};
    for (hs::Tick tick = 1; tick <= 600; ++tick)
    {
        if (tick % 120 == 1)
        {
            const hs::DebugCommand spawn{hs::DebugCommandKind::SpawnEnemy, (tick / 120) % 3, 0, {6, -4}};
            Check(loaded_world.ApplyDebugCommand(spawn).Succeeded() && compiled_world.ApplyDebugCommand(spawn).Succeeded(),
                  "shipping rules fixture enemy spawn");
        }
        hs::InputFrame input{};
        input.target_tick = tick;
        input.held.move_held = true;
        input.held.move_target_world = (tick / 150) % 2 == 0 ? hs::Float3{12, 0, 8} : hs::Float3{-10, 0, -6};
        input.held.aim_world = {6, 0, -4};
        input.held.basic_attack_held = tick % 60 < 40;
        std::array<hs::ActionEdge, 1> edges{};
        if (tick % 90 == 1 || tick % 90 == 2)
        {
            edges[0] = {++sequence, hs::GameAction::SkillQ,
                        tick % 90 == 1 ? hs::EdgeKind::Pressed : hs::EdgeKind::Released};
            input.ordered_edges = edges;
        }
        const auto expected = loaded_world.TickFixed(input, hs::FixedStepClock::kFixedStep);
        const auto actual = compiled_world.TickFixed(input, hs::FixedStepClock::kFixedStep);
        Check(actual.tick == expected.tick && actual.phase == expected.phase && actual.checksum == expected.checksum,
              "compiled rules must preserve each scripted gameplay tick");
        const auto position = loaded_world.GetObservation().player_position;
        moved |= position.x != initial_position.x || position.y != initial_position.y;
        loaded_world.ClearDomainSignals(); compiled_world.ClearDomainSignals();
    }
    Check(moved && loaded_world.GetObservation().balance.skill_uses[static_cast<std::size_t>(hs::SkillKind::PiercingShot)] > 0,
          "shipping rules comparison must exercise movement and an active skill");
    Check(compiled_world.ComputeChecksum() == loaded_world.ComputeChecksum(), "compiled rules final gameplay state");
    Check(loaded_world.Shutdown().Succeeded() && compiled_world.Shutdown().Succeeded(), "shipping rules simulation shutdown");
    const auto &audio_image = hs::shipping::Audio();
    Check(&audio_image == &hs::shipping::Audio() && audio_image.cues.size() == 107 && audio_image.samples.size() == 379,
          "compiled audio tables retain stable identity and complete authored coverage");
    std::uint32_t preload_samples{};
    for (const auto &cue : audio_image.cues) if (cue.preload) preload_samples += cue.sample_count;
    Check(preload_samples == 374, "compiled audio keeps the qualified preload versus transition split");
    std::uint32_t preload_chunks{}, transition_chunks{}, chunk_samples{};
    for (const auto &chunk : audio_image.chunks)
    {
        Check(chunk.offset >= 64 && chunk.offset <= owner.Size() && chunk.bytes <= owner.Size() - chunk.offset,
              "compiled PCM chunk belongs to the matching immutable image");
        chunk_samples += chunk.sample_count;
        if (chunk.preload) ++preload_chunks;
        else { ++transition_chunks; Check(chunk.sample_count == 1, "transition PCM keeps an independent owner"); }
    }
    Check(preload_chunks == 1 && transition_chunks == 5 && chunk_samples == 379,
          "compiled PCM groups the contiguous preload run without merging transition samples");
    for (const auto &sample : audio_image.samples)
        Check(sample.offset >= 64 && sample.offset <= owner.Size() && sample.bytes <= owner.Size() - sample.offset,
              "compiled PCM ranges belong to the matching immutable image");
    hs::SettingsData silent_settings; silent_settings.master_volume = 0;
    hs::AudioEngine compiled_audio;
    Check(compiled_audio.Initialize(silent_settings, audio_image, owner).Succeeded(), "compiled audio consumes PCM ranges directly");
    Check(compiled_audio.Status().loaded_cues == 107 && compiled_audio.Status().loaded_files == 374,
          "compiled audio does not eagerly load the five transition samples");
    compiled_audio.Shutdown();
}

int main(int argc, char **argv)
{
    const auto root = TestDirectory();
    std::error_code error;
    std::filesystem::remove_all(root, error);
    try
    {
        if (argc > 1)
        {
            if (argc == 2 && std::string_view(argv[1]) == "shipping-content")
            {
                TestShippingImage();
                TestCompiledShippingTables();
                return 0;
            }
            Check(argc == 2 && std::string_view(argv[1]) == "ui-text-fit", "unknown runtime test case");
            TestCharacterDetailsTextFit();
            return 0;
        }
        TestVfxCatalog();
        TestShippingImage();
        TestCompiledShippingTables();
        TestProductionVfxDecoding();
        TestSaveRecovery(root);
        TestProfileMigration(root);
        TestSettingsPersistence(root);
        TestAudioCatalogValidation(root);
        TestAudioNativeOperations();
        TestMainWakeAndSceneLeases();
        TestAudioDeliverySessions();
        TestAudioPayloadLazyLoad(root);
        TestTypedAudioPcm(root);
        TestPreparedNonLoopTransition(root);
        TestPreparedSessionAmbience(root);
        TestExperimentSpecRoundTrip(root);
        TestPlaytestRecordAndReplay(root);
        TestRecordingFileFinalization(root);
        TestPlaytestFinalizationFailures(root);
        TestSessionMusicTransitions();
        TestSessionProbeUiBridge();
        TestSimulationSignalFanout();
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
