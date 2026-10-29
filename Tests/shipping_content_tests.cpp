#include "test_generation_paths.hpp"
#include "vfx_catalog.hpp"
#include "vfx_light_reference.hpp"
#include <hs/core/fixed_step_clock.hpp>
#include <hs/core/process_info.hpp>
#include <hs/core/sha256.hpp>
#include <hs/core/shipped_content_owner.hpp>
#include <hs/game_rules/simulation_rules_io.hpp>
#include <hs/gameplay/game_simulation.hpp>
#include <hs/presentation/presentation_catalog_io.hpp>
#include <hs/presentation/projector.hpp>
#include <hs/render_data/render_snapshot_storage.hpp>
#include <hs/renderer/renderer.hpp>
#include <hs/runtime/audio_engine.hpp>
#include <hs/runtime/shipping_content.hpp>
#include <hs/runtime/vfx_image_io.hpp>

#include <Windows.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#pragma comment(lib, "dwrite.lib")

namespace
{
void Check(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

std::uint32_t Hash32(std::string_view value)
{
    std::uint32_t hash = 2166136261u;
    for (unsigned char c : value) { hash ^= c; hash *= 16777619u; }
    return hash;
}

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

void TestPreparedVfxOutputMembership()
{
    hs::VfxProgramData program;
    program.effects.resize(2);
    program.effects[0].sources = {0, 2};
    program.effects[1].sources = {2, 1};
    program.sources.resize(3);
    program.sources[0].outputs = {0, 2};
    program.sources[1].outputs = {2, 1};
    program.sources[2].outputs = {3, 1};
    program.outputs.resize(4);
    program.outputs[0].profile = hs::VfxOutputProfile::Distortion;
    program.outputs[1].profile = hs::VfxOutputProfile::Decal;
    program.outputs[2].profile = hs::VfxOutputProfile::Distortion;
    program.outputs[3].profile = hs::VfxOutputProfile::RibbonAdd;
    hs::VfxImageData prepared;
    hs::PrepareVfxOutputProfiles(program.View(), prepared.effect_output_profiles);
    const auto image = prepared.View();
    Check(image.HasOutput(1, hs::VfxOutputProfile::Distortion) &&
          image.HasOutput(1, hs::VfxOutputProfile::Decal) &&
          !image.HasOutput(1, hs::VfxOutputProfile::RibbonAdd) &&
          image.HasOutput(2, hs::VfxOutputProfile::RibbonAdd) &&
          !image.HasOutput(3, hs::VfxOutputProfile::RibbonAdd),
          "prepared output membership must preserve effect/source boundaries and merge duplicates");
    program.effects[0].sources = {100, 1};
    hs::PrepareVfxOutputProfiles(program.View(), prepared.effect_output_profiles);
    Check(!prepared.View().HasOutput(1, hs::VfxOutputProfile::Distortion),
          "reprepare must clear stale output membership");
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
    Check(std::ranges::equal(image.effect_output_profiles,ref.effect_output_profiles),
          "compiled output admission must match developer preparation");
    Check(image.program.effect_lookup.size()==ref.program.effect_lookup.size(),"compiled ID lookup extent");
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

void TestCompiledVfxLightCoverage()
{
    hs::VfxCatalog catalog;
    Check(hs::VfxCatalog::Load(test_generation::ProductPath("vfx/particle_effects.hsbin"),
        test_generation::ProductPath("vfx/vfx_program.hsbin"),catalog).Succeeded(),"load VFX light reference");
    const auto &program=catalog.Program();
    std::size_t light_outputs{};
    for(const auto &effect:program.effects)
    {
        std::size_t expected{};
        for(const auto &source:std::span(program.sources).subspan(effect.sources.first,effect.sources.count))
            for(const auto &output:std::span(program.outputs).subspan(source.outputs.first,source.outputs.count))
                expected+=output.profile==hs::VfxOutputProfile::Light;
        if(!expected) continue;
        hs::VfxEventInput input; input.effect_handle=effect.handle; input.sequence=71;
        if(effect.payload_kind==Hash32("PointEventPayload")) input.payload=hs::VfxPointPayload{};
        else if(effect.payload_kind==Hash32("PresentationContextPayload")) input.payload=hs::VfxContextPayload{};
        else if(effect.payload_kind==Hash32("CircleAreaPayload")){hs::VfxCirclePayload p;p.radius=2;input.payload=p;}
        else if(effect.payload_kind==Hash32("RingWithGapsPayload")){hs::VfxRingGapsPayload p;p.inner_radius=1;p.outer_radius=3;input.payload=p;}
        else if(effect.payload_kind==Hash32("ConePayload")){hs::VfxConePayload p;p.direction={0,0,1};p.range=3;p.half_angle_degrees=40;input.payload=p;}
        std::size_t decoded{};
        for(hs::Tick tick=0;tick<static_cast<hs::Tick>(std::ceil(effect.seconds*60));++tick)
            decoded=(std::max)(decoded,hs::runtime_detail::BuildVfxTypedLightCommands(
                catalog.Image(),std::span(&input,1),tick).size());
        for(auto quality:{hs::VfxQuality::Low,hs::VfxQuality::Medium,hs::VfxQuality::High})
        for(bool owned:{false,true})
        {
            input.quality=quality; input.geometry_owner_id=owned?91:0; input.geometry_start_tick=3; input.geometry_end_tick=243;
            for(hs::Tick tick=0;tick<=250;++tick)
            {
                const auto reference=hs::runtime_detail::light_reference::BuildVfxTypedLightCommands(
                    program,std::span(&input,1),tick);
                vfx_light_test::Compare(reference,hs::runtime_detail::BuildVfxTypedLightCommands(
                    hs::shipping::Vfx(),std::span(&input,1),tick));
            }
        }
        Check(decoded==expected,"compiled VFX light recipe coverage");
        light_outputs+=decoded;
    }
    Check(light_outputs==17,"compiled VFX light coverage inventory changed");
}

void TestCompiledShippingTables()
{
    TestPreparedVfxOutputMembership();
    TestCompiledVfxImage();
    TestCompiledVfxLightCoverage();
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
    hs::SimulationConfig config;
    config.seed = 0x5a674bc; config.automatic_choices = true; config.scenario.player_invulnerable = true;
    hs::GameSimulation loaded_world, compiled_world;
    Check(loaded_world.Initialize(config, reference_rules).Succeeded() &&
          compiled_world.Initialize(config, compiled_rules).Succeeded(), "shipping rules simulation initialization");
    const hs::DebugCommand grant{hs::DebugCommandKind::GrantSkill, static_cast<std::uint64_t>(hs::SkillKind::PiercingShot)};
    Check(loaded_world.ApplyDebugCommand(grant).Succeeded() && compiled_world.ApplyDebugCommand(grant).Succeeded(),
          "shipping rules fixture must equip its active skill");
    const auto initial_position = loaded_world.GetObservation().player_position;
    bool moved = false; hs::Sequence sequence{};
    for (hs::Tick tick = 1; tick <= 600; ++tick)
    {
        if (tick % 120 == 1)
        {
            const hs::DebugCommand spawn{hs::DebugCommandKind::SpawnEnemy, (tick / 120) % 3, 0, {6, -4}};
            Check(loaded_world.ApplyDebugCommand(spawn).Succeeded() && compiled_world.ApplyDebugCommand(spawn).Succeeded(),
                  "shipping rules fixture enemy spawn");
        }
        hs::InputFrame input{}; input.target_tick = tick; input.held.move_held = true;
        input.held.move_target_world = (tick / 150) % 2 == 0 ? hs::Float3{12, 0, 8} : hs::Float3{-10, 0, -6};
        input.held.aim_world = {6, 0, -4}; input.held.basic_attack_held = tick % 60 < 40;
        std::array<hs::ActionEdge, 1> edges{};
        if (tick % 90 == 1 || tick % 90 == 2)
        {
            edges[0] = {++sequence, hs::GameAction::SkillQ,
                tick % 90 == 1 ? hs::EdgeKind::Pressed : hs::EdgeKind::Released}; input.ordered_edges = edges;
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
        if (metrics.widthIncludingTrailingWhitespace > entry.size_pixels.x && metrics.width <= entry.size_pixels.x && metrics.height <= entry.size_pixels.y)
            ++trailing_only_wraps;
        for (const float scale : {1.0f, 720.0f / 1080.0f})
            if (metrics.height * scale > entry.size_pixels.y * scale + 0.01f || metrics.width * scale > entry.size_pixels.x * scale + 0.01f)
                fit_failures += std::format("Details need shrinking/clipping at scale {}: {} (visible={} trailing={} height={} > {}x{})\n",
                    scale, text, metrics.width, metrics.widthIncludingTrailingWhitespace, metrics.height,
                    entry.size_pixels.x, entry.size_pixels.y);
    };
    hs::PresentationCatalog catalog;
    Check(hs::LoadPresentationCatalog(test_generation::ProductPath("game-data/presentation-catalog"), catalog).Succeeded(),
          "load authored relic names and rules for text-fit");
    hs::GameReadModelStorage model;
    model.arena_half_extent = 10; model.player.health = model.player.max_health = 100;
    model.session.health = model.session.max_health = 100; model.session.skill_levels.fill(1);
    model.session.upgrade_masks.fill(0xFF); model.session.relic_mask = hs::kAllRelicsMask;
    for (auto &damage : model.summary.upgrade_damage) damage.fill(std::numeric_limits<std::uint64_t>::max());
    model.summary.relic_triggers.fill(std::numeric_limits<std::uint64_t>::max());
    model.summary.relic_damage.fill(std::numeric_limits<std::uint64_t>::max());
    model.summary.relic_kills.fill(std::numeric_limits<std::uint64_t>::max());
    for (auto &effects : model.summary.relic_effects) effects.fill(std::numeric_limits<std::uint64_t>::max());
    hs::RenderSnapshotStorage snapshot(16, 2, 2, 128);
    hs::PresentationUiState ui;
    const auto project = [&] { snapshot.Clear(); Check(hs::ProjectRenderSnapshot(model.View(), catalog, ui, {}, snapshot).Succeeded(), "project text-fit fixture"); };
    for (std::size_t skill = 0; skill < hs::kCombatSkillCount; ++skill)
    {
        model.session.phase = hs::SessionPhase::MainMenu; ui.page = hs::UiPage::Collection;
        ui.selected_collection_skill = static_cast<std::uint8_t>(skill); project();
        std::vector<std::string> descriptions;
        for (const auto &entry : snapshot.View().ui)
            if (entry.kind == hs::UiModel::Kind::Panel && entry.color_rgba == 0xFF2D4058u)
            {
                const std::string_view text(entry.utf8_text.data());
                Check(text.find('\n') != std::string_view::npos, "collection retains the complete upgrade rule");
                descriptions.emplace_back(text.substr(text.find('\n') + 1));
            }
        Check(descriptions.size() == hs::kUpgradeCount, "all authored upgrade descriptions are present");
        model.session.phase = hs::SessionPhase::Paused; ui.page = hs::UiPage::CharacterSkills;
        ui.selected_character_skill = static_cast<std::uint8_t>(skill); std::size_t shown{};
        for (ui.character_details_page = 0; ui.character_details_page < hs::CharacterDetailsPageCount(model.session, ui); ++ui.character_details_page)
        {
            project();
            for (const auto &entry : snapshot.View().ui)
                if (entry.kind == hs::UiModel::Kind::Text && entry.anchor_pixels.x >= 780 && entry.anchor_pixels.y >= 530 && entry.anchor_pixels.y < 950)
                {
                    check_fit(entry);
                    if (entry.color_rgba == 0xFFD7E0ECu)
                        Check(shown < descriptions.size() && entry.utf8_text.data() == descriptions[shown++],
                              "paged upgrade rule matches the full collection rule without truncation");
                }
        }
        Check(shown == hs::kUpgradeCount, "paging exposes every authored upgrade rule exactly once");
    }
    ui.page = hs::UiPage::CharacterStats; std::size_t shown{};
    for (ui.character_details_page = 0; ui.character_details_page < hs::CharacterDetailsPageCount(model.session, ui); ++ui.character_details_page)
    {
        project();
        for (const auto &entry : snapshot.View().ui)
            if (entry.kind == hs::UiModel::Kind::Text && entry.anchor_pixels.x < 1620 && entry.anchor_pixels.y >= 235 && entry.anchor_pixels.y < 950)
            {
                check_fit(entry);
                if (entry.color_rgba == 0xFFB8C2D0u)
                    Check(shown < hs::kRelicCount && std::string_view(entry.utf8_text.data()) == catalog.relic_rules[shown++].data(),
                          "paged relic rule matches the complete cooked description");
            }
    }
    Check(shown == hs::kRelicCount, "paging exposes every authored relic rule exactly once");
    Check(trailing_only_wraps != 0, "authored details exercise trailing-space wrap without visible overflow");
    Check(fit_failures.empty(), fit_failures);
    std::cout << "UI text-fit: 72 upgrade rules, 20 relic rules, maximum counters; no additional shrink/trim at 1080p/720p\n";
}
} // namespace

void TestShippingImage();

int main(int argc, char **argv)
{
    try
    {
        if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "shipping-content"))
        {
            TestShippingImage();
            TestCompiledShippingTables();
            return 0;
        }
        Check(argc == 2 && std::string_view(argv[1]) == "ui-text-fit", "unknown shipping test case");
        TestCharacterDetailsTextFit();
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
