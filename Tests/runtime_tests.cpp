#include <hs/render_data/render_snapshot_storage.hpp>
#include <hs/core/shipped_content_owner.hpp>
#include <hs/runtime/save_store.hpp>
#include <hs/runtime/audio_engine.hpp>
#include <hs/core/sha256.hpp>
#include <hs/renderer/renderer.hpp>
#include "runtime_channels.hpp"
#include "native_input_frame.hpp"
#include "window.hpp"
#include "vfx_session_state.hpp"
#include "renderer_receipts.hpp"

#include <Windows.h>

#include <array>
#include <algorithm>
#include <utility>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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

    const std::string duplicate_current =
        R"({"schema_version":2,"best_level":99,"best_level":100,"total_wins":0,"total_kills":0})";
    std::ofstream(primary, std::ios::trunc) << duplicate_current;
    Check(store.LoadProfile(roundtrip).Succeeded() && roundtrip.best_level == 100 &&
              read(primary) == duplicate_current,
          "duplicate current-profile key uses its last value without rewriting bytes");

    WriteCorrupt(primary);
    Check(store.LoadProfile(roundtrip).Succeeded() && roundtrip.best_level == 42 &&
              roundtrip.total_kills == profile.total_kills && read(backup) == legacy,
          "corrupt v2 primary recovers v1 backup without losing its original bytes");

    for (const auto &bad : {
             R"({"schema_version":1,"best_level":99,"total_wins":0,"total_kills":0,"unlocked_skills_mask":256,"unlocked_relics_mask":4095})",
             R"({"schema_version":1,"best_level":99,"total_wins":0,"total_kills":0,"unlocked_skills_mask":255,"unlocked_relics_mask":4096})",
             R"({"schema_version":2,"best_level":99,"total_wins":0,"total_kills":0,"unlocked_skills_mask":255})"})
    {
        std::ofstream(primary, std::ios::trunc) << bad;
        Check(store.LoadProfile(roundtrip).Succeeded() && roundtrip.best_level == 42,
              "migration keeps legacy mask bounds and current profile field checks");
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
          "future duplicate-key document remains read-only under last-value parsing");
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
    older->draw_facts = std::make_shared<const std::vector<std::uint64_t>>(
        std::vector<std::uint64_t>{1, 2, 3});
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
    const std::array<std::uint64_t, 3> unchanged_facts{1, 2, 3};
    const std::array<std::uint64_t, 3> changed_facts{1, 2, 4};
    Check(hs::runtime_detail::SameUiDrawInputs(*newer, unchanged_facts, newer->graphics_model) &&
          !hs::runtime_detail::SameUiDrawInputs(*newer, changed_facts, newer->graphics_model),
          "UI draw reuse follows concrete display facts while newer interaction receipts share the content owner");
    const auto changed_graphics = std::make_shared<const hs::GraphicsSettingsUiModel>();
    Check(hs::runtime_detail::SameUiDrawInputs(*newer, unchanged_facts, changed_graphics),
          "hidden graphics telemetry invalidated unchanged root draw content");
    auto settings_frame = *newer;
    settings_frame.ui.page = hs::UiPage::PauseSettings;
    Check(!hs::runtime_detail::SameUiDrawInputs(settings_frame, unchanged_facts, changed_graphics),
          "visible graphics settings ignored a new model owner");
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



} // namespace

int main()
{
    const auto root = TestDirectory();
    std::error_code error;
    std::filesystem::remove_all(root, error);
    try
    {
        TestSaveRecovery(root);
        TestProfileMigration(root);
        TestMainWake();
        TestTypedAudioPcm(root);
        TestUiPacketReceiptFollowsExactPresentedFrame();
        TestRendererReceiptsSurviveLaterFailure();
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
