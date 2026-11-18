#include <hs/render_data/render_snapshot_storage.hpp>
#include <hs/core/shipped_content_owner.hpp>
#include <hs/runtime/audio_engine.hpp>
#include <hs/core/cooked_format.hpp>
#include <hs/render_data/vfx_payload_ids.hpp>
#include <hs/core/sha256.hpp>
#include <hs/renderer/renderer.hpp>
#include <hs/runtime/vfx_image_io.hpp>
#include "runtime_channels.hpp"
#include "native_input_frame.hpp"
#include "window.hpp"
#include "vfx_session_state.hpp"
#include "renderer_receipts.hpp"
#include "vfx_catalog.hpp"
#include "vfx_typed_frame_adapter.hpp"
#include "vfx_typed_mote_commands.hpp"

#include <Windows.h>

#include <array>
#include <algorithm>
#include <utility>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

void TestGraphicsSettings();

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

void TestMainWake()
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
    auto owner = std::make_shared<hs::ShippedContentOwner>();
    Check(hs::ShippedContentOwner::Open(path, *owner).Succeeded(), "typed audio backing open");
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
    engine.SetWakeCallback(hs::runtime_detail::MainWake::NotifyOpaque, &wake);
    std::shared_ptr<hs::audio_detail::PreparedAudio> prepared;
    Check(hs::PrepareAudio(image, *owner, prepared).Succeeded() &&
          engine.Initialize(settings, std::move(prepared), std::move(owner)).Succeeded(),
          "typed audio initializes with its pack owner");
    Check(engine.Status().loaded_files == 1, "typed audio preloads only declared samples");
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
        engine.UpdateListener({});
        if (engine.Status().loaded_files == 1) break;
        Sleep(1);
    } while (GetTickCount64() < completion_deadline);
    Check(engine.Status().loaded_files == 1, "stopped transition PCM releases only after native completion");
    const auto reload_deadline = GetTickCount64() + 1000;
    while (!engine.PrepareCue(transition) && GetTickCount64() < reload_deadline) Sleep(1);
    Check(engine.PrepareCue(transition) && engine.Status().loaded_files == 2,
          "evicted transition reloads and verifies through the retained content handle");
    engine.Stop(transition);
    engine.UpdateListener({});
    (void)engine.PrepareCue(transition);
    engine.Shutdown(); // Joins a queued/in-flight preparation before owner destruction.
    Check(engine.Status().loaded_files == 0, "shutdown releases typed PCM and pending preparation");
}







void TestUiPacketReceiptFollowsExactPresentedFrame()
{
    hs::runtime_detail::UiDrawPackets packets;
    hs::RenderSnapshotExchange exchange(0, 0, 0, 0);
    hs::RenderSnapshotExchange::Consumer consumer(exchange);
    auto older = std::make_shared<hs::UiFrame>();
    older->generation = 10; older->session_id = 7;
    older->session.phase = hs::SessionPhase::CardSelection;
    older->session.card_count = 1;
    older->session.cards[0] = {hs::CardKind::LearnSkill, 1, 0};
    auto draw_content = std::make_shared<hs::UiDrawContent>();
    draw_content->models.push_back({hs::UiModel::Kind::Button, {360, 300}, {360, 420}});
    older->draw_content = draw_content;
    auto newer = std::make_shared<hs::UiFrame>(*older);
    newer->generation = 11; newer->session.cards[0].subject = 2;
    auto first = exchange.TryBeginWrite();
    Check(first.has_value(), "first UI publication slot");
    std::move(*first).Publish( 10, nullptr, older);
    auto rendered = consumer.AcquireLatest();
    auto second = exchange.TryBeginWrite();
    Check(second.has_value(), "new UI can publish while old UI is rendered");
    std::move(*second).Publish( 11, nullptr, newer);
    Check(rendered.ui_frame == older && rendered.current.ui.data() == draw_content->models.data(),
          "draw span and interaction facts retain the same immutable UI owner");
    Check(packets.AcceptPresentedUiPacket(10, rendered.ui_frame, 7, {1920, 1080}),
          "older submitted UI retains its exact facts despite a newer pending publication");
    auto shown = packets.presented_ui_packet.load(std::memory_order_acquire);
    Check(shown && shown->frame == older && shown->frame->session.cards[0].subject == 1 &&
          shown->presented_extent == hs::ViewExtent{1920, 1080},
          "Present receipt retains UI without copying draw models or interaction facts");
    Check(packets.AcceptPresentedUiPacket(10, rendered.ui_frame, 7, {1920, 1080}) &&
          packets.presented_ui_packet.load(std::memory_order_acquire) == shown,
          "repeated successful Present retains the same receipt owner");
    Check(!packets.AcceptPresentedUiPacket(10, rendered.ui_frame, 8, {1920, 1080}) &&
          !packets.AcceptPresentedUiPacket(10, rendered.ui_frame, 7, {}) &&
          !packets.AcceptPresentedUiPacket(11, rendered.ui_frame, 7, {1920, 1080}) &&
          packets.presented_ui_packet.load(std::memory_order_acquire) == shown,
          "invalid session, extent or generation preserves accepted UI");
    Check(packets.AcceptPresentedUiPacket(10, rendered.ui_frame, 7, {1280, 720}) &&
          packets.presented_ui_packet.load(std::memory_order_acquire)->frame == older,
          "resize updates receipt extent while preserving the immutable UI owner");
    rendered = consumer.AcquireLatest();
    Check(packets.AcceptPresentedUiPacket(11, rendered.ui_frame, 7, {1280, 720}) &&
          !packets.AcceptPresentedUiPacket(10, older, 7, {1280, 720}) &&
          packets.presented_ui_packet.load(std::memory_order_acquire)->frame == newer,
          "new frame supersedes old receipts without accepting stale UI");
}

void TestRendererReceiptsSurviveLaterFailure()
{
    hs::runtime_detail::VfxSessionState state;
    (void)state.Synchronize(7);
    hs::runtime_detail::UiDrawPackets packets;
    auto ui = std::make_shared<hs::UiFrame>();
    ui->generation = 10;
    ui->session_id = 7;
    hs::RendererFrameResult frame;
    frame.rendered_view.output_extent = {1920, 1080};
    frame.presented_ui_packet_generation = 10;
    const auto set_native_receipt = [&](hs::Tick tick) {
        const auto identity = state.PrepareNativeBatch(tick, true)->identity;
        frame.vfx_world_session_id = identity.session;
        frame.vfx_world_tick = identity.stateful_target;
        frame.vfx_world_batch_sequence = identity.sequence;
        frame.vfx_world_step_submitted = true;
    };
    set_native_receipt(5);
    // Receipts describe accepted work even if Render later returns a failure.
    auto facts = hs::runtime_detail::AcceptRendererReceipts(frame, packets, ui, 7, state);
    Check(facts.native_batch_accepted && !facts.ui_receipt_rejected &&
          !state.native_batch_active &&
          packets.presented_ui_packet.load(std::memory_order_acquire)->frame == ui,
          "accepted UI and native receipts precede overall Render failure handling");
    const auto shown = packets.presented_ui_packet.load(std::memory_order_acquire);

    set_native_receipt(6);
    frame.presented_ui_packet_generation = 0;
    frame.vfx_world_step_submitted = false;
    facts = hs::runtime_detail::AcceptRendererReceipts(frame, packets, ui, 7, state);
    Check(!facts.native_batch_accepted && !facts.ui_receipt_rejected && state.native_batch_active &&
          packets.presented_ui_packet.load(std::memory_order_acquire) == shown,
          "unsubmitted work and absent Present leave both pending owners unchanged");
    frame.vfx_world_step_submitted = true;
    ++frame.vfx_world_batch_sequence;
    facts = hs::runtime_detail::AcceptRendererReceipts(frame, packets, ui, 7, state);
    Check(!facts.native_batch_accepted && state.native_batch_active,
          "mismatched native receipt cannot consume the frozen batch");
    --frame.vfx_world_batch_sequence;
    frame.presented_ui_packet_generation = 11;
    facts = hs::runtime_detail::AcceptRendererReceipts(frame, packets, ui, 7, state);
    Check(facts.native_batch_accepted && facts.ui_receipt_rejected && !state.native_batch_active &&
          packets.presented_ui_packet.load(std::memory_order_acquire) == shown,
          "rejected UI receipt cannot prevent acknowledgement of accepted GPU work");

    set_native_receipt(7);
    frame.presented_ui_packet_generation = 0;
    facts = hs::runtime_detail::AcceptRendererReceipts(frame, packets, ui, 7, state);
    Check(facts.native_batch_accepted && !state.native_batch_active &&
          packets.presented_ui_packet.load(std::memory_order_acquire) == shown,
          "Present failure leaves UI unchanged while accepted world mutation stays acknowledged");
}

void TestPreparedVfxOwnerRoutes()
{
    using namespace hs;
    using namespace runtime_detail;

    const auto bleed_id = MakeAssetId("persistent.status.bleed");
    const auto tick_id = MakeAssetId("particle.status.bleed_tick");
    const auto ready_id = MakeAssetId("particle.upgrade.charged.full_ready");
    std::array<VfxEffectRecord, 3> effects{};
    effects[0].handle = 1;
    effects[0].input_mode = 1;
    effects[0].payload_kind = hs::vfx_payload::EntityAttachment;
    effects[0].timing_kind = 1;
    effects[0].sources = {0, 1};
    effects[1].handle = 2;
    effects[1].input_mode = 0;
    effects[1].payload_kind = hs::vfx_payload::PointEvent;
    effects[1].timing_kind = 0;
    effects[1].seconds = 0.5f;
    effects[1].sources = {1, 1};
    effects[2].handle = 3;
    effects[2].input_mode = 1;
    effects[2].payload_kind = hs::vfx_payload::EntityAttachment;
    effects[2].timing_kind = 0;
    effects[2].seconds = 1.0f;
    effects[2].sources = {2, 0};
    std::array<VfxEffectLookupRecord, 3> lookups{{
        {bleed_id.value, 1}, {tick_id.value, 2}, {ready_id.value, 3}}};
    std::sort(lookups.begin(), lookups.end(), [](const auto &left, const auto &right) {
        return left.effect_id < right.effect_id;
    });
    std::array<VfxUpgradeBindingRecord, 1> upgrades{{
        {MakeAssetId("skill.charged_shot").value, 1, {0, 1}}}};
    std::array<std::uint32_t, 1> sequences{3};
    std::array<VfxSourceRecord, 2> program_sources{};
    program_sources[0].effect = 1;
    program_sources[0].outputs = {0, 1};
    program_sources[1].effect = 2;
    std::array<VfxOutputRecord, 1> program_outputs{};
    program_outputs[0].source = 0;
    VfxImage image{};
    image.program.effects = effects;
    image.program.sources = program_sources;
    image.program.outputs = program_outputs;
    image.program.effect_lookup = lookups;
    image.program.upgrade_bindings = upgrades;
    image.program.upgrade_sequences = sequences;
    image.effect_lookup_by_id = lookups;

    std::array<VfxMoteSourceRecipe, 2> mote_sources{};
    mote_sources[0].effect_id = bleed_id.value;
    mote_sources[0].effect_routes = kVfxMoteBleedOwner;
    mote_sources[0].count = 2;
    mote_sources[0].stable_id = 11;
    mote_sources[0].tick_route_valid = true;
    mote_sources[0].output_range = {0, 1};
    mote_sources[1].effect_id = tick_id.value;
    mote_sources[1].effect_routes = kVfxMoteBleedTick;
    std::array<VfxMoteOutputRecipe, 1> mote_outputs{};
    mote_outputs[0].tick_route_valid = true;
    mote_outputs[0].output_color_valid = true;
    mote_outputs[0].min_quality = 2;
    image.mote_source_recipes = mote_sources;
    image.mote_output_recipes = mote_outputs;
    std::array<VfxRange, 3> flash_ranges{};
    flash_ranges[1] = {0, 1};
    std::array<VfxOwnedStatusTickRecipe, 1> flash_recipes{};
    flash_recipes[0].tick_effect_id = tick_id.value;
    flash_recipes[0].status_effect_id = bleed_id.value;
    flash_recipes[0].tick_effect_handle = 2;
    flash_recipes[0].status_effect_handle = 1;
    flash_recipes[0].min_quality = 1;
    flash_recipes[0].bleed = true;
    flash_recipes[0].mask_slices = {2, 3, 4};
    flash_recipes[0].mask_strength = 1.0f;
    flash_recipes[0].spawn.shape = VfxImpactShape::SmallMote;
    image.owned_status_tick_effect_ranges = flash_ranges;
    image.owned_status_tick_recipes = flash_recipes;

    VfxCatalog catalog;
    Check(catalog.Bind(image).Succeeded(), "bind prepared VFX owner routes");
    std::array<PersistentVfxVisual, 2> visuals{};
    visuals[0].kind = PersistentVfxKind::EnemyBleedStatus;
    visuals[0].stable_id = 99;
    visuals[0].status_episode_generation = 9;
    visuals[0].active_tick = 0;
    visuals[0].expires = 120;
    visuals[0].position = {1, 2, 3};
    visuals[0].radius = 1;
    visuals[0].entity_render_id = (2ull << 60) | 7;
    visuals[0].effect_asset = bleed_id;
    visuals[1] = visuals[0];
    visuals[1].kind = PersistentVfxKind::ChargedFullReady;
    visuals[1].stable_id = 100;
    visuals[1].effect_asset = ready_id;
    visuals[1].skill = 3;
    visuals[1].source_upgrade = 0;
    PreparedPairFacts facts;
    facts.Prepare({1, 0, 1, 0, 2, 0, 0}, {}, visuals, {}, {});
    VfxTypedFrameInputs inputs;
    BuildVfxTypedFrameInputs(catalog, {}, visuals, {}, 30, inputs, facts);
    Check(inputs.persistent.size() == 2 && inputs.persistent[0].effect_handle == 1 &&
              inputs.persistent[1].effect_handle == 3 && inputs.unsupported_input_count == 0,
          "prepared catalog routes supported persistent owner handles");
    const auto bleed_owner = inputs.persistent[0];
    visuals[1].source_upgrade = 4;
    BuildVfxTypedFrameInputs(catalog, {}, visuals, {}, 30, inputs, facts);
    Check(inputs.persistent.size() == 1 && inputs.persistent[0].effect_handle == 1,
          "unregistered charged upgrade does not produce a persistent effect");

    std::vector<VfxPersistentInput> owners;
    for (std::size_t i = 0; i < 64; ++i)
    {
        auto owner = bleed_owner;
        owner.stable_id = 1000 + i;
        owner.current_transform[12] = static_cast<float>(i);
        auto &entity = std::get<VfxEntityPayload>(owner.payload);
        entity.render_instance_id = (2ull << 60) | (4 + i % 13);
        owners.push_back(owner);
    }
    owners[3].quality = VfxQuality::Medium;
    std::array<VfxEventInput, 1> events{};
    events[0].effect_handle = 2;
    events[0].sequence = 17;
    events[0].geometry_owner_id = 7;
    events[0].status_episode_generation = 9;
    events[0].quality = VfxQuality::High;
    events[0].event_tick = 30;
    events[0].stable_seed = 57;
    events[0].payload = VfxPointPayload{};
    std::vector<VfxFlashSpawnInput> flashes;
    std::vector<Sequence> emitted_sequences;
    std::vector<VfxPreparedMoteBurst> prepared;
    std::vector<std::size_t> owner_indices{999};
    PrepareVfxOwnedStatusTickOutputs(image, events, owners, flashes,
                                     emitted_sequences, prepared, owner_indices);
    Check(flashes.size() == 1 && emitted_sequences.size() == 1 &&
              emitted_sequences[0] == 17 && prepared.size() == 1 &&
              prepared[0].count == 2 && prepared[0].event_sequence == 17 &&
              flashes[0].position.x == 3.0f && prepared[0].base.position.x == 16.0f,
          "status tick routes flash and mote to independent first eligible owners in source order");

    owners[3].status_episode_generation = 10;
    flashes.clear();
    emitted_sequences.clear();
    PrepareVfxOwnedStatusTickOutputs(image, events, owners, flashes,
                                     emitted_sequences, prepared, owner_indices);
    Check(flashes.size() == 1 && emitted_sequences.size() == 1 &&
              emitted_sequences[0] == 17 && prepared.size() == 1 &&
              flashes[0].position.x == 16.0f &&
              prepared[0].base.position.x == 16.0f,
          "new owner generation cannot take an event from an older status episode");

    const auto hash = [](std::string_view value) {
        std::uint32_t result = 2166136261u;
        for (const unsigned char c : value) { result ^= c; result *= 16777619u; }
        return result;
    };
    VfxImageData mote_data;
    auto &program = mote_data.program;
    program.effects.push_back({});
    program.effects[0].handle = 1;
    program.effects[0].input_mode = 0;
    program.effects[0].timing_kind = 0;
    program.effects[0].seconds = .65f;
    program.effects[0].payload_kind = hs::vfx_payload::PointEvent;
    program.effects[0].sources = {0, 2};
    program.effect_lookup.push_back({MakeAssetId("particle.skill.trap").value, 1});
    program.parameters.push_back({hash("count_per_second"), VfxParameterType::Int, 3});
    program.sources.resize(2);
    for (auto &source : program.sources)
    {
        source.effect = 1;
        source.type = VfxSourceType::CurlMotes;
        source.knot_count = 2;
        source.knots[0] = 0.0f;
        source.knots[1] = 1.0f;
        source.parameters = {0, 1};
    }
    program.sources[0].stable_id = hash("unused_rate");
    program.sources[0].outputs = {0, 0};
    program.sources[1].stable_id = hash("ambient_motes");
    program.sources[1].outputs = {0, 1};
    program.outputs.push_back({});
    auto &rate_output = program.outputs[0];
    rate_output.source = 1;
    rate_output.profile = VfxOutputProfile::SpriteAdd;
    rate_output.shape = hash("soft_motes");
    rate_output.shape_domain = hash("sprite");
    rate_output.shape_scale_rule = hash("component_size");
    rate_output.shape_component_kind = hash("particle");
    rate_output.rgba = {1, 1, 1, 1};
    rate_output.hdr = 1;
    rate_output.min_quality = static_cast<std::uint32_t>(VfxQuality::Medium);
    rate_output.coverage_type = hash("analytic");
    rate_output.coverage_ref = hash("soft_disc");
    rate_output.motion = VfxMotionKind::RareInwardDrift;

    PrepareVfxMoteRecipes(mote_data);
    std::array<VfxEventInput, 1> rate_events{};
    rate_events[0].sequence = 1;
    rate_events[0].effect_handle = 1;
    rate_events[0].event_tick = 0;
    rate_events[0].stable_seed = 57;
    rate_events[0].quality = VfxQuality::High;
    VfxPointPayload rate_payload{};
    rate_payload.position = {1, 2, 3};
    rate_payload.authored_scale = 1;
    rate_events[0].payload = rate_payload;
    flashes.clear();
    AppendVfxOwnedMoteCommands(mote_data.View(), std::span(rate_events), 20, flashes);
    Check(flashes.size() == 2, "empty mote source must not borrow the following source's output");
}



} // namespace

int main()
{
    const auto root = TestDirectory();
    std::error_code error;
    std::filesystem::remove_all(root, error);
    try
    {
        TestMainWake();
        TestGraphicsSettings();
        TestTypedAudioPcm(root);
        TestUiPacketReceiptFollowsExactPresentedFrame();
        TestRendererReceiptsSurviveLaterFailure();
        TestPreparedVfxOwnerRoutes();
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
