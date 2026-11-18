#include <hs/runtime/application.hpp>
#include <hs/render_data/render_snapshot_storage.hpp>
#include <hs/runtime/shipping_content.hpp>
#include <hs/runtime/prepared_content.hpp>
#include <hs/runtime/vfx_image_io.hpp>
#include <hs/core/shipped_content_owner.hpp>
#ifdef HS_DEVELOPMENT_TOOLS
#include <hs/game_rules/simulation_rules_io.hpp>
#include <hs/asset_manifest/selected_generation.hpp>
#endif
#include <hs/presentation/projector.hpp>
#include <hs/presentation/meadow_surface.hpp>
#include <hs/presentation/process_scene.hpp>

#include "runtime_channels.hpp"
#if defined(HS_DEVELOPMENT_TOOLS)
#include "preview_scene.hpp"
#include "asset_test_room.hpp"
#endif
#include "native_input_frame.hpp"
#include "ui_command_admission.hpp"
#include "runtime_diagnostics.hpp"
#include "translucency_diagnostics_log.hpp"
#include "audio_session.hpp"
#include "vfx_catalog.hpp"
#include "vfx_typed_frame_adapter.hpp"
#include "vfx_typed_ground_commands.hpp"
#include "vfx_upgrade_dispatch.hpp"
#include "vfx_typed_light_commands.hpp"
#include "vfx_typed_distortion_commands.hpp"
#include "vfx_typed_decal_commands.hpp"
#include "vfx_typed_volume_commands.hpp"
#include "vfx_typed_screen_commands.hpp"
#include "vfx_typed_mesh_commands.hpp"
#include "vfx_typed_fresnel_commands.hpp"
#include "vfx_typed_ribbon_commands.hpp"
#include "vfx_typed_flash_commands.hpp"
#include "vfx_typed_mote_commands.hpp"
#include "vfx_typed_ballistic_commands.hpp"
#include "vfx_birth_admission.hpp"
#include "vfx_profile_admission.hpp"
#include "vfx_birth_retirement.hpp"
#include "vfx_visible_outputs.hpp"
#include "frame_performance_schema.hpp"
#include "vfx_session_state.hpp"
#include "renderer_receipts.hpp"
#include "renderer_settings_projection.hpp"
#include "startup_graphics_settings.hpp"
#include "window.hpp"
#include "camera_pose.hpp"

#include <hs/core/fixed_step_clock.hpp>
#include <hs/core/process_info.hpp>
#include <hs/gameplay/game_simulation.hpp>
#include <hs/jobs/task_system.hpp>
#include <hs/renderer/renderer.hpp>
#include <hs/render_data/render_package.hpp>
#include <hs/runtime/audio_engine.hpp>
#ifdef HS_DEVELOPMENT_TOOLS
#include <hs/runtime/playtest_recording.hpp>
#endif
#include <hs/runtime/save_store.hpp>
#include <hs/runtime/settings_projection.hpp>

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>


namespace hs
{
namespace
{

enum class GraphicsUiPhase : std::uint8_t
{
    Applied,
    Requested,
    Pending,
    AwaitingDisplayConfirmation,
    RestoringDisplay,
    ApplyFailed,
};

// Receipts contain only public renderer support queries and applied state.
Result ResolveGraphicsDisplayOutput(HWND window, const GraphicsDisplayId &requested,
                                    GraphicsDisplayId &effective)
{
    std::vector<RendererDisplayOutput> displays;
    if (auto result = EnumerateWindowDisplayPlacements(displays); !result) return result;
    if (requested.value != "auto")
    {
        const auto selected = std::ranges::find(displays, requested.value,
                                                &RendererDisplayOutput::id);
        if (selected != displays.end())
        {
            effective.value = selected->id;
            return Result::Success();
        }
    }
    if (!window)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_runtime",
                               "Cannot resolve a display without a window.");
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
                         reinterpret_cast<MONITORINFO *>(&monitor)))
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               "Cannot query the current display.");
    std::array<char, CCHDEVICENAME * 4> device_name{};
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, monitor.szDevice, -1,
        device_name.data(), static_cast<int>(device_name.size()), nullptr, nullptr);
    if (count <= 1)
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               "Cannot encode the current display device name.");
    const std::string_view device(device_name.data(), static_cast<std::size_t>(count - 1));
    const auto selected = std::ranges::find_if(displays, [&](const auto &display) {
        // Public native ID: dxgi:<16 hex LUID digits>:<UTF-8 GDI DeviceName>.
        // CCD rectangles are physical; GetMonitorInfo rectangles can be DPI-virtualized.
        constexpr std::size_t name_offset = 5 + 16 + 1;
        const std::string_view id(display.id);
        return id.starts_with("dxgi:") && id.size() == name_offset + device.size() &&
            id[name_offset - 1] == ':' && id.substr(name_offset) == device;
    });
    if (selected == displays.end())
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               "The current window display is not exposed by Win32 display enumeration.");
    effective.value = selected->id;
    return Result::Success();
}

UiModel SettingsStatusText(std::string_view label, Float2 anchor,
                           std::uint32_t color = 0xFFFFFFFFu)
{
    UiModel model;
    model.kind = UiModel::Kind::Text;
    model.anchor_pixels = anchor;
    model.size_pixels = {920, 27};
    model.color_rgba = color;
    model.font_pixels = 18;
    std::copy_n(label.data(), std::min(label.size(), model.utf8_text.size() - 1),
                model.utf8_text.data());
    return model;
}

UiModel SettingsConfirmButton(std::string_view label, Float2 anchor)
{
    auto model = SettingsStatusText(label, anchor, 0xFFFFFFFFu);
    model.kind = UiModel::Kind::Button;
    model.size_pixels = {260, 64};
    model.color_rgba = 0xFF34495Eu;
    model.font_pixels = 25;
    return model;
}

void SetThreadName(const wchar_t *name) noexcept
{
    SetThreadDescription(GetCurrentThread(), name);
}

void WriteText(const std::filesystem::path &path, std::string_view text)
{
#ifdef HS_DEVELOPMENT_TOOLS
    std::ofstream stream(path, std::ios::trunc);
    stream << text;
#else
    (void)path;
    (void)text;
#endif
}


#ifdef HS_DEVELOPMENT_TOOLS
Result WriteRendererStartup(const std::filesystem::path &path, double initialize_us, double audio_startup_us,
    bool succeeded, const RendererStartupTimings &timings,
    const RendererPsoCacheStats &cache)
{
    std::uint64_t step_ns{};
    for (const auto &sample : timings.steps) step_ns += sample.nanoseconds;
    std::string text = std::format(
        "{{\"initialize_us\":{},\"audio_startup_us\":{},\"succeeded\":{},\"detail_available\":{},"
        "\"completed\":{},\"begin_qpc\":{},\"end_qpc\":{},\"qpc_frequency\":{},"
        "\"initialize_ns\":{},\"step_wall_ns\":{},\"unclassified_wall_ns\":{},\"steps\":{{",
        initialize_us, audio_startup_us, succeeded, timings.captured, timings.completed,
        timings.begin_qpc, timings.end_qpc, timings.qpc_frequency, timings.total_ns,
        step_ns, timings.total_ns >= step_ns ? timings.total_ns - step_ns : 0);
    const auto append_samples = [&](const auto &names, const auto &samples) {
        for (std::size_t index = 0; index < names.size(); ++index)
        {
            if (index) text += ',';
            text += std::format("\"{}\":{{\"ns\":{},\"calls\":{}}}", names[index],
                samples[index].nanoseconds, samples[index].calls);
        }
    };
    append_samples(kRendererStartupStepNames, timings.steps);
    text += "},\"nested_operations\":{";
    append_samples(kRendererStartupOperationNames, timings.operations);
    text += std::format(
        "}},\"pso_at_initialize_return\":{{\"capture_timings\":{},\"hits\":{},\"misses\":{},"
        "\"native_creates\":{},\"loaded_bytes\":{},\"key_build_ns\":{},\"key_build_max_ns\":{},"
        "\"library_load_ns\":{},\"library_load_max_ns\":{},\"native_create_ns\":{},\"native_create_max_ns\":{},"
        "\"library_store_ns\":{},\"library_store_max_ns\":{},\"serialize_ns\":{},\"cache_write_ns\":{},"
        "\"mutex_wait_ns\":{},\"mutex_wait_max_ns\":{}}},",
        cache.capture_timings, cache.hits, cache.misses, cache.native_creates, cache.loaded_bytes,
        cache.key_build_ns, cache.key_build_max_ns, cache.library_load_ns, cache.library_load_max_ns,
        cache.native_create_ns, cache.native_create_max_ns, cache.library_store_ns, cache.library_store_max_ns,
        cache.serialize_ns, cache.cache_write_ns, cache.mutex_wait_ns, cache.mutex_wait_max_ns);
    text += "\"semantics\":\"wall ns; steps are sequential synchronous call intervals; nested operations and PSO intervals overlap steps and each other; do not sum with steps\","
        "\"read_coverage\":\"ReadBinary checked whole-file read plus local-probe and atlas reads; includes bounds/open/host allocation\","
        "\"hash_coverage\":\"explicit local-probe SHA256 only; package-internal validation/hash remains in decode; Runtime Generation verification precedes this Initialize\","
        "\"leaf_coverage\":\"character/environment decoders and geometry preparation; monster palette adoption and named animation decode; Impl allocation plus probe allocation; canonical/asset descriptors; host packing; utility submit and completion admission\","
        "\"leaf_limits\":\"other native service allocations/decoders remain inside their named step; async-worker internal work is not propagated into thread-local leaf samples; GPU/driver busy time unavailable\","
        "\"gpu_wait_semantics\":\"completion_admission includes fence setup/queue drain; gpu_fence_wait is only actual WaitForSingleObject calls; neither is GPU elapsed\","
        "\"native_device_internal_ns\":null,\"process_cold\":null,\"disk_cache_state\":\"unobserved\","
        "\"native_pipeline_cache_state\":\"observe loaded_bytes/hits/misses; cold/warm not forced or inferred\"}\n";
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.close();
    return stream ? Result::Success() : Result::Failure(ErrorCode::InvalidState, "hs_runtime",
        "Could not write renderer initialization diagnostics.");
}
#endif


PresentationEvent MakeAudioEvent(std::string_view cue, Sequence sequence) noexcept
{
    PresentationEvent event;
    event.sequence = sequence;
    event.kind = PresentationKind::Audio;
    event.asset = MakeAssetId(cue);
    event.parameters = EncodeAudioAction(AudioEventAction::Play);
    return event;
}

#ifdef HS_DEVELOPMENT_TOOLS
std::uint64_t ReadDiagnosticQpc() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

bool ReadRenderThreadCpuTicks(std::uint64_t &ticks) noexcept
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return false;
    ticks = (static_cast<std::uint64_t>(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
        (static_cast<std::uint64_t>(user.dwHighDateTime) << 32 | user.dwLowDateTime);
    return true;
}

#endif

#ifdef HS_DEVELOPMENT_TOOLS
Result RefillDiagnosticEnemies(GameSimulation &simulation, std::uint32_t target,
                               std::uint64_t &spawn_index, std::uint32_t &added)
{
    if (!target) return Result::Success();
    const auto probe = simulation.GetSessionProbe();
    if (probe.normal_enemy_count > target || probe.boss_count != 0)
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Diagnostic enemy fixture exceeded its population.");
    const auto deficit = target - probe.normal_enemy_count;
    for (std::uint32_t i = 0; i < deficit; ++i, ++spawn_index)
    {
        const auto angle = static_cast<double>(spawn_index) * 2.39996322972865332;
        const auto radius = 6.0 + 4.0 * static_cast<double>(spawn_index % 101) / 100.0;
        const Float2 position{probe.player_position.x + static_cast<float>(std::cos(angle) * radius),
                              probe.player_position.y + static_cast<float>(std::sin(angle) * radius)};
        if (auto result = simulation.ApplyDebugCommand({DebugCommandKind::SpawnEnemy,
                spawn_index % 4 == 3 ? static_cast<std::uint64_t>(EnemyKind::Ranged) :
                                      static_cast<std::uint64_t>(EnemyKind::Melee), 0, position}); !result)
            return result;
    }
    const auto after = simulation.GetSessionProbe();
    if (after.normal_enemy_count != target || after.boss_count != 0)
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Diagnostic enemy fixture could not fill its population.");
    added += deficit;
    return Result::Success();
}
#endif
} // namespace

ApplicationResult RunApplication(const ApplicationConfig &config)
{
#ifdef HS_DEVELOPMENT_TOOLS
    // Visible performance experiments use the same clock and interpolation as interactive play.
    const bool realtime = !config.smoke || config.visible;
    // Smoke, experiment, recording and replay consumers require complete diagnostic artifacts.
    const bool capture_diagnostics = config.capture_diagnostics || (config.smoke && !config.test_room_capture) ||
        config.record_playtest || config.replay_compare || !config.replay_directory.empty();
    const bool validation_enabled = config.validation || config.gpu_validation;
#else
    constexpr bool realtime = true;
    constexpr bool capture_diagnostics = false;
    constexpr bool validation_enabled = false;
    if (config.test_room || config.test_room_capture || config.smoke || config.smoke_render_lockstep || config.capture_diagnostics ||
        config.capture_translucency_diagnostics || config.diagnostic_enemy_count || config.record_playtest ||
        config.replay_compare || !config.replay_directory.empty() || config.resize_test ||
        config.monster_preview_asset != 0xFFFFFFFFu || config.boss_vfx_capture != 0xFFFFFFFFu ||
        config.boss_phase2_capture || config.slime_family_preview_count || config.environment_preview ||
        config.character_preview || config.vfx_showcase || config.skill_vfx_capture != 0xFFFFFFFFu ||
        config.preview_camera_override ||
        !config.timeline_actions.empty())
        return {Result::Failure(ErrorCode::InvalidState, "hs_runtime",
            "Smoke, preview, replay, recording, and capture modes require a development executable.")};
#endif
#ifdef HS_DEVELOPMENT_TOOLS
    if (config.test_room && ((config.smoke && !config.test_room_capture) || config.record_playtest || config.replay_compare ||
        !config.replay_directory.empty() || config.skill_vfx_capture < kCombatSkillCount ||
        config.monster_preview_asset < 6 || config.environment_preview || config.character_preview ||
        config.vfx_showcase || config.slime_family_preview_count || config.boss_vfx_capture < 3 ||
        config.boss_phase2_capture))
        return {Result::Failure(ErrorCode::InvalidArgument, "hs_runtime",
            "The asset test room cannot be combined with replay or another preview mode; smoke requires a capture target.")};
    if (config.smoke_render_lockstep && !config.smoke)
        return {Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                "Smoke render lockstep requires --smoke.")};
    if (config.diagnostic_enemy_count &&
        (!config.smoke || !config.capture_diagnostics || config.diagnostic_enemy_count > 10'000 ||
         config.record_playtest || config.replay_compare || !config.replay_directory.empty() ||
         config.skill_vfx_capture < kCombatSkillCount || config.monster_preview_asset < 6 ||
         config.boss_vfx_capture < 3 || config.boss_phase2_capture))
        return {Result::Failure(ErrorCode::InvalidState, "hs_runtime",
            "Diagnostic enemy fixture requires smoke + diagnostics, at most 10000 enemies, and no replay, recording or preview.")};
    if (config.translucency_diagnostics_mode > TranslucencyDiagnosticsMode::CoverageOnly ||
        (!config.capture_translucency_diagnostics && config.translucency_diagnostics_mode != TranslucencyDiagnosticsMode::Full))
        return {Result::Failure(ErrorCode::InvalidArgument, "hs_runtime", "Hero work diagnostic mode requires its explicit capture flag.")};
    if (config.capture_translucency_diagnostics && !config.capture_diagnostics)
        return {Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Hero work capture requires explicit diagnostics.")};
#endif
#ifdef HS_DEVELOPMENT_TOOLS
    const runtime_detail::DiagnosticClock capture_clock = config.capture_diagnostics ? ReadDiagnosticQpc : nullptr;
    LARGE_INTEGER capture_frequency{};
    if (capture_clock && (!QueryPerformanceFrequency(&capture_frequency) || capture_frequency.QuadPart <= 0))
        return {Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Diagnostic clock calibration failed.")};
    const auto capture_microseconds = capture_clock ? 1'000'000.0 / static_cast<double>(capture_frequency.QuadPart) : 0.0;
#else
    constexpr runtime_detail::DiagnosticClock capture_clock = nullptr;
#endif
    const auto executable_directory = CurrentExecutableDirectory();
    if (executable_directory.empty())
    {
        return {Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                "Cannot resolve the executable directory.")};
    }
    const auto artifact_directory = config.artifact_directory.empty()
        ? executable_directory / "Artifacts" / "runtime" : config.artifact_directory;
    auto content_owner = std::make_shared<shipping::PreparedContent>();
    auto &prepared_content = *content_owner;
#ifdef HS_DEVELOPMENT_TOOLS
    if (!config.generation_pointer.empty())
    {
        SelectedGeneration selected_generation;
        if (auto loaded = LoadSelectedGeneration(config.generation_pointer, selected_generation); !loaded)
            return {loaded};
        if (auto loaded = shipping::LoadPreparedContent(std::move(selected_generation), prepared_content); !loaded)
            return {loaded};
    }
    else
#endif
    {
        if (auto loaded = shipping::LoadPreparedContent(executable_directory, shipping::ImageInfo(), prepared_content); !loaded)
            return {loaded};
    }
    const auto &shipped_content = prepared_content.owner;
    std::string deployment_id;
#ifdef HS_DEVELOPMENT_TOOLS
    if (prepared_content.generation) deployment_id = prepared_content.generation->id;
    else
#endif
    {
        deployment_id.reserve(64);
        constexpr char hex[] = "0123456789abcdef";
        for (std::size_t i = 16; i != 48; ++i)
        {
            const auto byte = std::to_integer<unsigned>(shipping::ImageInfo().header[i]);
            deployment_id += hex[byte >> 4]; deployment_id += hex[byte & 15];
        }
    }
    const auto &presentation_catalog = prepared_content.presentation;
    auto runtime_camera = presentation_catalog.camera;
#if defined(HS_DEVELOPMENT_TOOLS)
    if (config.test_room) runtime_camera.yaw_degrees = 0.0f;
    std::unique_ptr<SimulationRules> room_rules;
    if (config.test_room)
    {
        room_rules = std::make_unique<SimulationRules>(*prepared_content.simulation);
        room_rules->arena_half_extent = 512.0f;
        room_rules->arena_boundary.count = 4;
        room_rules->arena_boundary.points[0] = {-512, -512};
        room_rules->arena_boundary.points[1] = {-512, 512};
        room_rules->arena_boundary.points[2] = {512, 512};
        room_rules->arena_boundary.points[3] = {512, -512};
        room_rules->arena_obstacle_count = 0;
    }
    const auto &simulation_rules = room_rules ? *room_rules : *prepared_content.simulation;
#else
    const auto &simulation_rules = *prepared_content.simulation;
#endif
#ifdef HS_DEVELOPMENT_TOOLS
    const auto content_hash = prepared_content.content_hash;
    const auto simulation_rules_hash = SimulationRulesHash(simulation_rules);
#endif
    VfxCatalog vfx_catalog;
    if (auto bound = vfx_catalog.Bind(prepared_content.vfx.View()); !bound)
        return {bound};
#ifdef HS_DEVELOPMENT_TOOLS
    PlaytestReplay replay;
    const auto replaying = !config.replay_directory.empty();
    if (replaying)
    {
        if (auto loaded = LoadPlaytestReplay(config.replay_directory, replay); !loaded)
            return {loaded};
        if (!config.replay_compare &&
            replay.header.simulation_rules_hash != simulation_rules_hash)
            return {Result::Failure(ErrorCode::InvalidState, "hs_playtest",
                                    "Replay content hash differs from the current content.")};
    }
    const auto effective_seed = replaying ? replay.header.seed : config.seed;
    PlaytestRecorder playtest_recorder;
    if (config.record_playtest)
    {
        if (auto started = playtest_recorder.Start(
                {config.playtest_output_directory, effective_seed,
                 simulation_rules_hash, content_hash}); !started)
            return {started};
    }
#else
    constexpr bool replaying = false;
#endif
    const auto settings_root = (config.smoke || config.test_room)
        ? std::optional<std::filesystem::path>(artifact_directory / "Profile")
        : std::nullopt;
    SaveStore save_store(settings_root);
    SettingsDocument requested_settings;
    bool loaded_from_file{};
    auto settings_loaded = Result::Success();
#if defined(HS_DEVELOPMENT_TOOLS)
    if (config.test_room)
    {
        SaveStore game_settings_reader;
        settings_loaded = game_settings_reader.LoadSettingsV2(requested_settings, &loaded_from_file);
    }
    else
#endif
        settings_loaded = save_store.LoadSettingsV2(requested_settings, &loaded_from_file);
    if (!settings_loaded)
    {
        return {settings_loaded};
    }
    if (!loaded_from_file)
        requested_settings = LegacyDefaultSettingsDocument();
#if defined(HS_DEVELOPMENT_TOOLS)
    std::optional<ApplicationConfig> room_startup;
    if (config.test_room_capture)
    {
        room_startup.emplace(config);
        room_startup->smoke = false;
        room_startup->borderless_explicit = true;
        room_startup->borderless = false;
        room_startup->vsync_explicit = true;
        room_startup->vsync = false;
        room_startup->frame_cap_explicit = true;
        room_startup->frame_cap = 0;
    }
    const auto initial_settings = PrepareStartupGraphicsSettings(requested_settings, room_startup ? *room_startup : config);
#else
    const auto initial_settings = PrepareStartupGraphicsSettings(requested_settings, config);
#endif
    const bool mute_audio = config.mute_audio || capture_diagnostics || config.environment_preview ||
        config.character_preview || config.vfx_showcase || config.preview_camera_override
#if defined(HS_DEVELOPMENT_TOOLS)
        || config.test_room
#endif
        ;
    GraphicsSettingsCommit settings_commit(std::move(requested_settings),
                                           initial_settings, mute_audio);
    SettingsData &settings = settings_commit.Effective();
    ProfileData profile;
    if (auto loaded = save_store.LoadProfile(profile); !loaded)
    {
        return {loaded};
    }
    runtime_detail::MainWake main_wake;
    if (!main_wake.Handle())
        return {Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Cannot create Main wake event.")};
#ifdef HS_DEVELOPMENT_TOOLS
    const auto audio_startup_begin = capture_clock ? capture_clock() : 0;
#endif
    AudioEngine audio;
    audio.SetWakeCallback(runtime_detail::MainWake::NotifyOpaque, &main_wake);
    if (auto initialized = audio.Initialize(settings, prepared_content.prepared_audio, content_owner); !initialized)
    {
        return {initialized};
    }

#ifdef HS_DEVELOPMENT_TOOLS
    const auto audio_startup_ticks = capture_clock
        ? runtime_detail::PerformanceLogState::ElapsedTicks(audio_startup_begin, capture_clock()) : 0;
#endif
    // No window owns the foreground yet; suppress even the initial menu cue.
    audio.SetBackgroundMuted(true);

    // Bounded event queues carry full gameplay geometry and exceed the default
    // Windows stack reserve; their lifetime still spans all joined workers.
    auto channel_storage = std::make_unique<RuntimeChannels>(&main_wake);
    auto &channels = *channel_storage;
    channels.camera_zoom_percent.store(std::clamp(config.camera_zoom_percent, 15u, 120u));
    channels.best_level = profile.best_level;
    TaskSystem task_system;
    Window window(channels, settings.skill_virtual_keys, runtime_camera);
#if defined(HS_DEVELOPMENT_TOOLS)
    if (config.test_room) window.EnableTestRoom();
#endif
    Sequence ui_audio_sequence{};
    auto main_audio = std::make_unique<runtime_detail::MainAudioSession>(audio, ui_audio_sequence);
    SettingsData ui_click_settings = settings_commit.UiInput();
    SettingsDocument ui_click_document = settings_commit.Requested();
    GraphicsSettingsUiModel ui_click_model;
    ui_click_model.requested = ui_click_document.graphics;
    ui_click_model.transaction_state = "렌더러를 준비하는 중입니다";
    channels.presentation_graphics_ui.store(std::make_shared<const GraphicsSettingsUiModel>(ui_click_model), std::memory_order_release);
    bool ui_command_queue_full{};
    bool ui_callback_out_of_memory{};
    std::uint64_t consumed_selection_packet_generation{};
    std::atomic graphics_ui_phase{GraphicsUiPhase::Applied};
    std::atomic settings_save_failed{false};
    std::atomic display_modal_phase{GraphicsUiPhase::Applied};
    bool display_confirm_clicked{};
    bool display_cancel_clicked{};
    window.SetUiClickHandler([&](Float2 client_pixels, ViewExtent client_extent) {
        try
        {
            const auto receipt = channels.ui_packets.presented_ui_packet.load(std::memory_order_acquire);
            if (!receipt) return;
            const auto &displayed = receipt->frame;
            if (!runtime_detail::UiFrameExtentMatches(receipt->presented_extent, client_extent))
            {
                channels.ui_input_rejected.store(true, std::memory_order_release);
                return; // Wait until the resized target has an accepted UI frame.
            }
            const auto cursor = ReferenceUiCursor(client_pixels,
                {receipt->presented_extent.width, receipt->presented_extent.height});
            if (!displayed->draw_content ||
                !runtime_detail::PointHitsUiDrawButton(displayed->draw_content->models, cursor)) return;
            const auto modal = static_cast<GraphicsUiPhase>(displayed->modal_phase);
            if (modal != GraphicsUiPhase::Applied)
            {
                if (modal == GraphicsUiPhase::AwaitingDisplayConfirmation)
                {
                    const auto x = (cursor.x + 1.0f) * 960.0f;
                    const auto y = (1.0f - cursor.y) * 540.0f;
                    if (x >= 650 && x <= 910 && y >= 600 && y <= 664)
                        display_confirm_clicked = true;
                    else if (x >= 1010 && x <= 1270 && y >= 600 && y <= 664)
                        display_cancel_clicked = true;
                }
                return;
            }
            const auto live_session = channels.ReadSessionProbe();
            const auto live_page = static_cast<UiPage>(channels.ui_page.load(std::memory_order_acquire));
            const PresentationUiState live_ui{
                live_page,
                channels.collection_skill.load(std::memory_order_acquire),
                channels.character_skill.load(std::memory_order_acquire),
                channels.loadout_source.load(std::memory_order_acquire),
                channels.settings_group.load(std::memory_order_acquire),
                channels.settings_page.load(std::memory_order_acquire),
                channels.character_details_page.load(std::memory_order_acquire)};
            const bool selection_phase = displayed->session.phase == SessionPhase::CardSelection ||
                displayed->session.phase == SessionPhase::RelicSelection ||
                displayed->session.phase == SessionPhase::StatAllocation;
            if (channels.current_session_id.load(std::memory_order_acquire) != displayed->session_id ||
                live_session.phase != displayed->session.phase || !runtime_detail::SameUiState(live_ui, displayed->ui) ||
                (selection_phase && (!runtime_detail::SameUiSelection(live_session, displayed->session) ||
                    consumed_selection_packet_generation == displayed->generation)))
            {
                channels.ui_input_rejected.store(true, std::memory_order_release);
                return;
            }
            auto ui = displayed->ui;
            const auto previous_ui = ui;
            const auto current_graphics_ui = channels.presentation_graphics_ui.load(std::memory_order_acquire);
            if ((displayed->graphics_model && current_graphics_ui &&
                 displayed->graphics_model->generation != current_graphics_ui->generation) ||
                ((displayed->ui.page == UiPage::MainMenuSettings ||
                  displayed->ui.page == UiPage::PauseSettings) &&
                 !runtime_detail::SameUiSettings(displayed->settings, ui_click_settings)))
            {
                channels.ui_input_rejected.store(true, std::memory_order_release);
                return;
            }
            auto interaction = ResolveUiInteraction(displayed->session, ui, displayed->settings,
                cursor, displayed->graphics_model.get());
            const bool admitted = runtime_detail::AdmitPresentedUiInteraction(interaction, ui,
                previous_ui, *displayed, channels.ui_actions, channels.ui_commands,
                channels.rejected_ui_commands);
            if (admitted && selection_phase && interaction.gameplay_action)
                consumed_selection_packet_generation = displayed->generation;
            channels.ui_input_rejected.store(!admitted, std::memory_order_release);
            if (!admitted) ui_command_queue_full = true;
            channels.ui_page.store(static_cast<std::uint8_t>(ui.page),
                                   std::memory_order_release);
            channels.collection_skill.store(ui.selected_collection_skill, std::memory_order_release);
            channels.character_skill.store(ui.selected_character_skill, std::memory_order_release);
            channels.loadout_source.store(ui.loadout_source_slot, std::memory_order_release);
            channels.settings_group.store(ui.settings_group, std::memory_order_release);
            channels.settings_page.store(ui.settings_page, std::memory_order_release);
            channels.character_details_page.store(ui.character_details_page, std::memory_order_release);
            if (interaction.runtime_command &&
                ApplySettingsEdit(ui_click_document, *interaction.runtime_command))
            {
                ui_click_settings = ProjectSettingsDocument(ui_click_document);
                if (mute_audio) ui_click_settings.master_volume = 0;
                ui_click_model.requested = ui_click_document.graphics;
            }
            std::string_view cue;
            if (interaction.invalid)
                cue = "audio.ui.invalid";
            if (interaction.gameplay_action)
            {
                switch (interaction.gameplay_action->kind)
                {
                case UiActionKind::StartSession: cue = "audio.ui.confirm"; break;
                case UiActionKind::Quit:
                case UiActionKind::ReturnToMainMenu: cue = "audio.ui.cancel"; break;
                case UiActionKind::Reroll: cue = "audio.ui.reroll"; break;
                case UiActionKind::SelectCard: cue = "audio.ui.card_select"; break;
                case UiActionKind::AssignStat: cue = "audio.ui.stat_allocate"; break;
                case UiActionKind::Resume: cue = "audio.ui.pause_close"; break;
                case UiActionKind::SwapLoadoutSlots: cue = "audio.ui.tab"; break;
                }
            }
            if (interaction.runtime_command)
            {
                switch (interaction.runtime_command->kind)
                {
                case UiCommandKind::BeginSkillRebind: cue = "audio.ui.rebind_start"; break;
                case UiCommandKind::CancelSkillRebind: cue = "audio.ui.cancel"; break;
                case UiCommandKind::SetMasterVolumePercent:
                case UiCommandKind::SetBgmVolumePercent:
                case UiCommandKind::SetSfxVolumePercent:
                case UiCommandKind::SetUiVolumePercent: cue = "audio.ui.slider_tick"; break;
                default: if (cue.empty()) cue = "audio.ui.confirm"; break;
                }
            }
            if (!cue.empty())
                audio.Play(MakeAudioEvent(cue, ++ui_audio_sequence));
        }
        catch (const std::bad_alloc &)
        {
            ui_callback_out_of_memory = true;
            // A Win32 callback cannot unwind into the owning Main boundary.
            window.StopInput();
            channels.stop_requested.store(true, std::memory_order_release);
            channels.NotifyMain();
        }
    });
    std::uint32_t hover_target = 0;
    std::uint32_t previous_hover_target = 0;
    window.SetUiHoverHandler([&](Float2 cursor) {
        const auto phase = static_cast<SessionPhase>(
            channels.session_phase.load(std::memory_order_acquire));
        const auto page = static_cast<UiPage>(
            channels.ui_page.load(std::memory_order_acquire));
        const auto x = (cursor.x + 1.0f) * 960.0f;
        const auto y = (1.0f - cursor.y) * 540.0f;
        const auto inside = [&](float left, float top, float width, float height) {
            return x >= left && x <= left + width && y >= top && y <= top + height;
        };
        if (phase == SessionPhase::MainMenu && page == UiPage::Root)
        {
            for (std::uint32_t i = 0; i < 4; ++i)
                if (inside(760.0f, 270.0f + i * 150.0f, 400.0f, 92.0f)) { hover_target = i + 1; goto hover_done; }
        }
        if ((phase == SessionPhase::CardSelection || phase == SessionPhase::RelicSelection) &&
            inside(760.0f, 790.0f, 400.0f, 72.0f)) { hover_target = 20; goto hover_done; }
        if (phase == SessionPhase::Paused && page == UiPage::Root)
        {
            for (std::uint32_t i = 0; i < 3; ++i)
                if (inside(760.0f, 420.0f + i * 100.0f, 400.0f, 72.0f)) { hover_target = 30 + i; goto hover_done; }
        }
        hover_target = 0;
    hover_done:
        if (hover_target != previous_hover_target)
        {
            previous_hover_target = hover_target;
            if (hover_target != 0)
                audio.Play(MakeAudioEvent("audio.ui.hover", ++ui_audio_sequence));
        }
    });
    auto result =
        window.Create(settings.width, settings.height, config.visible, settings.borderless);
    if (!result)
    {
        return {result};
    }
    GraphicsSettings window_display_graphics = settings_commit.Requested().graphics;
    GraphicsExtent windowed_client_extent = window_display_graphics.output_extent.value_or(
        GraphicsExtent{settings.width, settings.height});
    bool window_borderless = settings.borderless;
    GraphicsDisplayId initial_native_output;
    if (auto resolved = ResolveGraphicsDisplayOutput(window.Handle(),
                                                     window_display_graphics.display_output,
                                                     initial_native_output); !resolved)
        return {resolved};
    if (auto placed = window.ApplyDisplayPlacement(initial_native_output,
            window_borderless, windowed_client_extent.width, windowed_client_extent.height); !placed)
        return {placed};
    GraphicsDisplayId window_native_output = initial_native_output;
    RECT initial_client{};
    if (!GetClientRect(window.Handle(), &initial_client) ||
        initial_client.right <= initial_client.left || initial_client.bottom <= initial_client.top)
        return {Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                "Cannot query the placed window client extent.")};
    settings.width = static_cast<std::uint32_t>(initial_client.right - initial_client.left);
    settings.height = static_cast<std::uint32_t>(initial_client.bottom - initial_client.top);
    settings_commit.SetDesiredOutputExtent(settings.width, settings.height);
    if (config.smoke)
    {
        window.StopInput();
    }

#ifdef HS_DEVELOPMENT_TOOLS
    if (capture_diagnostics || validation_enabled || config.test_room)
    {
        std::error_code error;
        std::filesystem::create_directories(artifact_directory, error);
        if (error) return {Result::Failure(ErrorCode::InvalidState, "hs_runtime", error.message())};
    }
#endif

    std::mutex result_mutex;
    Result thread_result = Result::Success();
    std::atomic<Tick> rendered_tick{};
    NativeInputFrame native_input_frame;
    native_input_frame.BindMainWake(runtime_detail::MainWake::NotifyOpaque, &main_wake);
#ifdef HS_DEVELOPMENT_TOOLS
    std::mutex smoke_mutex;
    std::condition_variable smoke_cv;
    const auto notify_smoke = [&] {
        if (!config.smoke_render_lockstep) return;
        std::lock_guard lock(smoke_mutex);
        smoke_cv.notify_all();
    };
#endif
    const auto request_stop = [&] {
        channels.stop_requested.store(true, std::memory_order_release);
#ifdef HS_DEVELOPMENT_TOOLS
        notify_smoke();
#endif
        native_input_frame.Cancel();
    };
    const auto finish_simulation = [&] {
        channels.simulation_done.store(true, std::memory_order_release);
#ifdef HS_DEVELOPMENT_TOOLS
        notify_smoke();
#endif
    };
    auto record_thread_failure = [&](Result failure) {
        std::lock_guard lock(result_mutex);
        if (thread_result)
        {
            thread_result = std::move(failure);
        }
        request_stop();
    };
#ifdef HS_DEVELOPMENT_TOOLS
    const auto log_error = [&](const char *file, const std::string &message) {
        OutputDebugStringA(message.c_str());
        std::error_code error;
        std::filesystem::create_directories(artifact_directory, error);
        std::ofstream(artifact_directory / file, std::ios::app) << message;
    };
#endif
    const auto log_settings_save_failure = [&](const Result &failure) {
        settings_save_failed.store(true, std::memory_order_release);
#ifdef HS_DEVELOPMENT_TOOLS
        const auto message = std::format("Settings save failed (code {}): {}\n",
                                         static_cast<unsigned>(failure.Code()),
                                         failure.Message());
        log_error("settings_save_errors.log", message);
#else
        (void)failure;
#endif
    };
    const auto save_committed_settings = [&] {
        auto confirmation = settings_commit.Committed();
        if (auto saved = save_store.SaveSettingsV2(confirmation); !saved)
        {
            log_settings_save_failure(saved);
        }
        else
        {
            settings_commit.ConfirmPersisted(std::move(confirmation));
            settings_save_failed.store(false, std::memory_order_release);
        }
    };


    auto render_ports = channels.ForRender();
    const SettingsData initial_renderer_settings = settings;
    const GraphicsSettings startup_graphics = settings_commit.Requested().graphics;
    std::jthread render_thread([&, render_ports, initial_renderer_settings, startup_graphics](std::stop_token) mutable {
        auto &channels = render_ports;
        SetThreadName(L"HS Render");
        try
        {
        const std::shared_ptr<const void> render_content_owner = content_owner;
        D3D12Renderer renderer;
        RendererConfig renderer_config = ProjectRendererConfig(initial_renderer_settings, startup_graphics);
        const auto user_data_root = SaveStore::DefaultRoot();
        if (!user_data_root.empty())
            renderer_config.pipeline_cache_directory = user_data_root / L"Cache" / L"Pipeline";
        std::vector<std::byte> startup_font;
        const auto font_result = [&]() -> Result {
#if defined(HS_DEVELOPMENT_TOOLS)
            if (!shipped_content)
            {
                ShippedContentOwner font_file;
                if (auto opened = ShippedContentOwner::Open(executable_directory / L"Fonts" / L"NotoSansKR.ttf", font_file); !opened) return opened;
                if (!font_file.Size() || font_file.Size() > UINT32_MAX)
                    return Result::Failure(ErrorCode::InvalidArgument, "hs_runtime", "Developer font extent is invalid.");
                startup_font.resize(static_cast<std::size_t>(font_file.Size()));
                return font_file.ReadExact(0, startup_font);
            }
#endif
            return shipping::ReadFont(*shipped_content, shipping::ImageInfo(), startup_font);
        }();
        if (!font_result)
        {
            record_thread_failure(font_result);
            channels.render_ready.store(true, std::memory_order_release);
            channels.render_ready.notify_all();
            channels.NotifyMain();
            return;
        }
        renderer_config.font_bytes = startup_font;
        renderer_config.display_output = initial_native_output;
        renderer_config.task_system = &task_system;
        renderer_config.window = window.Handle();
#ifdef HS_DEVELOPMENT_TOOLS
        renderer_config.warp = config.warp;
        renderer_config.validation = validation_enabled;
        renderer_config.gpu_validation = config.gpu_validation;
#else
        renderer_config.warp = false;
        renderer_config.validation = false;
        renderer_config.gpu_validation = false;
#endif
        renderer_config.surface_frontend = config.surface_frontend.value_or(SurfaceFrontend::VisibilityBuffer);
        if (config.geometry_frontend) renderer_config.geometry_frontend = *config.geometry_frontend;
#ifdef HS_DEVELOPMENT_TOOLS
        renderer_config.capture_timing_stream = config.capture_diagnostics;
        renderer_config.capture_gpu_timings = config.capture_gpu_timings;
        renderer_config.capture_translucency_diagnostics = config.capture_translucency_diagnostics;
        renderer_config.translucency_diagnostics_mode = config.translucency_diagnostics_mode;
#else
        renderer_config.capture_timing_stream = false;
        renderer_config.capture_gpu_timings = false;
        renderer_config.capture_translucency_diagnostics = false;
#endif
        renderer_config.frame_generation_allowed = false; // Startup/loading/menu are not gameplay frames.
        renderer_config.interpolate = realtime;
        // Scripted runs must not accept developer-UI input or vary their captures with its state.
        renderer_config.devtools_visible = !config.smoke && config.skill_vfx_capture >= kCombatSkillCount;
        const auto &initial_sprite_bindings = vfx_catalog.Image().sprite_bindings;
        renderer_config.bleed_status_sprite = initial_sprite_bindings.bleed_status_sprite;
        renderer_config.burn_status_sprite = initial_sprite_bindings.burn_status_sprite;
        renderer_config.slow_status_sprite = initial_sprite_bindings.slow_status_sprite;
        renderer_config.mark_status_sprite = initial_sprite_bindings.mark_status_sprite;
        renderer_config.trap_sprite = initial_sprite_bindings.trap_sprite;
        renderer_config.fire_area_sprite = initial_sprite_bindings.fire_area_sprite;
        renderer_config.vfx_mesh_recipes = vfx_catalog.MeshRecipeView();
        renderer_config.vfx_fresnel_recipes = vfx_catalog.FresnelRecipeView();
#ifdef HS_DEVELOPMENT_TOOLS
        renderer_config.barrier_mode = config.barrier_mode;
#else
        renderer_config.barrier_mode = BarrierMode::Automatic;
#endif
#ifdef HS_DEVELOPMENT_TOOLS
        renderer_config.artifact_directory = artifact_directory;
#endif
#ifdef HS_DEVELOPMENT_TOOLS
        if (prepared_content.generation)
        {
            renderer_config.product_paths = prepared_content.generation->product_paths;
            renderer_config.product_digests = prepared_content.generation->product_digests;
        }
#endif
        renderer_config.content_generation_id = deployment_id;
        renderer_config.content_generation_lease = content_owner;
        renderer_config.packed_content = shipped_content;
        if (shipped_content) renderer_config.UseRendererCatalog(prepared_content.renderer);

#ifdef HS_DEVELOPMENT_TOOLS
        const auto renderer_startup_begin = capture_clock ? capture_clock() : 0;
#endif
        auto initialized = renderer.Initialize(std::move(renderer_config));
#ifdef HS_DEVELOPMENT_TOOLS
        const auto renderer_startup_ticks = capture_clock
            ? runtime_detail::PerformanceLogState::ElapsedTicks(renderer_startup_begin, capture_clock()) : 0;
        if (capture_clock)
            if (auto written = WriteRendererStartup(artifact_directory / "renderer_startup.json",
                    renderer_startup_ticks * capture_microseconds, audio_startup_ticks * capture_microseconds, initialized.Succeeded(),
                    renderer.GetStartupTimings(), renderer.GetPipelineCacheStats()); !written)
                record_thread_failure(written);
#endif
        std::vector<std::byte>().swap(startup_font);
        if (!initialized)
        {
            record_thread_failure(initialized);
            channels.render_ready.store(true, std::memory_order_release);
            channels.render_ready.notify_all();
            channels.NotifyMain();
            return;
        }
        // Render-thread-owned copies: record resolved execution state only at
        // successful commit boundaries, without changing persisted user intent.
        GraphicsSettings active_graphics = startup_graphics;
        SettingsData active_renderer_settings = initial_renderer_settings;
        GraphicsDisplayId active_native_output = initial_native_output;
        std::uint64_t active_settings_generation{};
        const auto restore_renderer_settings = [&] {
            GraphicsDisplayId restored_output;
            if (auto resolved = ResolveGraphicsDisplayOutput(window.Handle(),
                                                             active_native_output,
                                                             restored_output); !resolved)
                return resolved;
            auto restored = renderer.Resize(active_renderer_settings.width, active_renderer_settings.height);
            if (restored) restored = renderer.RequestGraphicsSettings(
                GraphicsSettingsRequest{active_settings_generation,
                    GraphicsSettingsRequestCause::Recovery, active_graphics, restored_output});
            if (restored) active_native_output = std::move(restored_output);
            return restored;
        };
#ifdef HS_DEVELOPMENT_TOOLS
        std::ofstream graphics_settings_file;
        std::optional<runtime_detail::DiagnosticLog> graphics_settings_lines;
        std::uint64_t graphics_config_revision{};
        std::string graphics_config_signature;
        if (config.capture_diagnostics)
        {
            graphics_settings_file.open(artifact_directory / "graphics_settings.csv", std::ios::binary);
            if (!graphics_settings_file)
                record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                    "Could not initialize graphics settings capture."));
            graphics_settings_lines.emplace(graphics_settings_file, true,
                "config_revision,settings_generation,setting_id,key,requested,effective,requested_available,native_effective,native_reason\n");
        }
#endif
        bool execution_receipt_pending{true};
        const auto publish_graphics_receipt = [&]() {
            execution_receipt_pending = true;
            const auto receipt = RendererGraphicsReceipt(renderer, active_graphics, active_settings_generation,
                static_cast<HWND>(window.Handle()), validation_enabled);
            channels.renderer_graphics_ui.store(receipt, std::memory_order_release);
            channels.NotifyMain();
#ifdef HS_DEVELOPMENT_TOOLS
            if (graphics_settings_lines)
            {
                const auto canonical = renderer.GetGraphicsSettingsStatus();
                std::vector<std::string> rows;
                std::string signature = std::format("{}\n", active_settings_generation);
                for (const auto &descriptor : GraphicsSettingDescriptors())
                {
                    const auto requested = GraphicsSettingToken(active_graphics, descriptor.id);
                    const auto effective = canonical.actual_applied_available
                        ? GraphicsSettingToken(canonical.actual_applied, descriptor.id) : std::string{};
                    const auto choice = std::ranges::find_if(receipt->choices, [&](const auto &entry) {
                        return entry.setting == descriptor.id && entry.token == requested;
                    });
                    // UI display/extent choices are suggestions, not native capability admissions.
                    constexpr std::array capability_settings{SettingId::ReconstructionProvider,
                        SettingId::ReconstructionQuality, SettingId::FrameGenerationProvider,
                        SettingId::FrameGenerationMode, SettingId::FrameGenerationMultiplier,
                        SettingId::HdrMode, SettingId::LowLatency,
                        SettingId::AoMode, SettingId::GiMode, SettingId::ReflectionMode, SettingId::ShadowMode,
                        SettingId::AdaptiveShading};
                    const bool capability_reported = std::ranges::find(capability_settings, descriptor.id) != capability_settings.end();
                    const auto native = std::ranges::find(receipt->resolved, descriptor.id, &GraphicsUiStatus::setting);
                    rows.push_back(std::format("{},{},{},{},{},{},{}\n",
                        static_cast<unsigned>(descriptor.id), runtime_detail::CsvField(descriptor.key),
                        runtime_detail::CsvField(requested), runtime_detail::CsvField(effective),
                        !capability_reported || choice == receipt->choices.end() ? -1 : choice->available ? 1 : 0,
                        runtime_detail::CsvField(native == receipt->resolved.end() ? std::string_view{} : native->effective),
                        runtime_detail::CsvField(native == receipt->resolved.end() ? std::string_view{} : native->reason)));
                    signature += rows.back();
                }
                if (signature != graphics_config_signature)
                {
                    ++graphics_config_revision;
                    graphics_config_signature = std::move(signature);
                    for (const auto &row : rows)
                        graphics_settings_lines->Append(std::format("{},{},{}", graphics_config_revision,
                            active_settings_generation, row));
                }
            }
#endif
        };
        publish_graphics_receipt();
        auto previous_ui_presentation = renderer.GetPresentationStatus();
        auto previous_receipt_phase = channels.session_phase.load(std::memory_order_acquire);
        bool settings_was_open{};
#ifdef HS_DEVELOPMENT_TOOLS
        std::uint64_t telemetry_completed_frame{};
#endif
        channels.render_ready.store(true, std::memory_order_release);
        channels.render_ready.notify_all();
        channels.NotifyMain();

        auto active_frame_cap = initial_renderer_settings.frame_cap;
        auto active_decorative_particle_fraction = renderer.DecorativeParticleFraction();
        auto active_borderless = initial_renderer_settings.borderless;
        auto active_vfx_quality = active_graphics.particle_quality == GraphicsQuality::Low
            ? VfxQuality::Low
            : active_graphics.particle_quality == GraphicsQuality::Medium
                ? VfxQuality::Medium : VfxQuality::High;
        bool renderer_settings_suspended{};

#if defined(HS_DEVELOPMENT_TOOLS)
        PreviewEditValues preview_edit;
        preview_edit.enabled = config.character_preview;
        std::optional<runtime_detail::AssetTestRoom> asset_test_room;
        if (config.test_room) asset_test_room.emplace(renderer.GetCharacterAnimationClips(),
            renderer.GetLegacyCharacterAnimationClips(), vfx_catalog.Image());
        double room_time{};
        Tick room_previous_display_tick{};
        std::chrono::nanoseconds room_previous_simulation_time{};
        bool room_paused{};
        auto room_previous_clock = std::chrono::steady_clock::now();
        bool room_capture{};
        std::uint64_t room_capture_index{};
        const auto room_capture_session = std::chrono::system_clock::now().time_since_epoch().count();
        std::uint64_t room_session = 0x7100000000000001ull;
        Sequence room_revision{};
        std::vector<PreviewStationLabel> room_labels;
        const auto prepare_room_catalog = [&]() -> Result {
            if (!asset_test_room) return Result::Success();
            if (config.test_room_capture && (*config.test_room_capture == 0 ||
                *config.test_room_capture > asset_test_room->entries.size()))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_runtime",
                    std::format("Test room station must be between 1 and {}.", asset_test_room->entries.size()));
            auto ground = std::make_shared<MeadowGroundQuery>();
            if (auto prepared = asset_test_room->PrepareGround(*ground); !prepared) return prepared;
            channels.room_ground.store(std::move(ground), std::memory_order_release);
            room_labels.clear();
            room_labels.reserve(asset_test_room->entries.size());
            std::string catalog = "station,group,label,x,y,z,capture_distance\n";
            std::string catalog_row;
            constexpr std::array groups{"animation", "model", "vfx"};
            std::size_t index{};
            for (const auto &entry : asset_test_room->entries)
            {
                auto anchor = entry.position;
                anchor.y += .15f;
                room_labels.push_back({anchor, entry.name});
                catalog_row.clear();
                runtime_detail::AppendCsvValues(catalog_row, ++index);
                runtime_detail::AppendCsvText(catalog_row, groups[static_cast<std::size_t>(entry.group)]);
                runtime_detail::AppendCsvText(catalog_row, entry.name);
                runtime_detail::AppendCsvValues(catalog_row, entry.position.x, entry.position.y,
                    entry.position.z, entry.capture_distance);
                catalog += catalog_row;
                catalog += '\n';
            }
            std::error_code directory_error;
            std::filesystem::create_directories(artifact_directory, directory_error);
            if (directory_error)
                return Result::Failure(ErrorCode::TaskFailed, "hs_runtime", "Cannot create test room capture directory.");
            std::ofstream output(artifact_directory / "test-room-catalog.csv", std::ios::binary | std::ios::trunc);
            output << catalog;
            output.close();
            if (!output)
                return Result::Failure(ErrorCode::TaskFailed, "hs_runtime", "Cannot write test room station catalog.");
            const auto title = std::format("Project HS gallery - {} simultaneous exhibits [right mouse: walk; Space: freeze; R: view; F12: capture]",
                asset_test_room->entries.size());
            const std::wstring wide(title.begin(), title.end());
            SetWindowTextW(window.Handle(), wide.c_str());
            return Result::Success();
        };
        if (auto catalog_result = prepare_room_catalog(); !catalog_result)
        { record_thread_failure(catalog_result); return; }
#endif
        RenderSnapshotExchange::Consumer snapshot_consumer(channels.snapshots);
        runtime_detail::VfxSessionState vfx_session;
        runtime_detail::PreparedPairFacts vfx_pair_facts;
        runtime_detail::SnapshotParticleScratch snapshot_particle_scratch;
        runtime_detail::VfxTypedFrameInputs typed_vfx;
        std::vector<PresentationEvent> frame_events, dispatched_event_scratch;
        std::vector<VfxScreenInput> typed_screen;
        std::vector<VfxVolumeInput> typed_volumes;
        std::vector<VfxLightInput> typed_lights;
        std::vector<VfxDistortionInput> typed_distortions;
        std::vector<float> distortion_gap_angles;
        std::vector<VfxDecalInput> typed_decals;
        std::vector<VfxEntitySurfaceCommand> typed_surfaces;
        std::vector<VfxOwnedMeshInput> typed_mesh_spawns;
        std::vector<VfxFresnelInput> typed_fresnels;
        std::vector<VfxGroundSpawnInput> typed_ground_spawns;
        std::vector<float> ground_gap_angles;
        std::vector<VfxFlashSpawnInput> visible_flashes;
        std::vector<VfxFlashSpawnInput> status_stamp_flashes;
        std::vector<runtime_detail::VfxPreparedFireSource> prepared_fire_sources;
        std::vector<runtime_detail::VfxPreparedMoteBurst> prepared_mote_bursts;
        std::vector<runtime_detail::VfxPreparedMoteBurst> prepared_status_mote_bursts;
        std::vector<runtime_detail::VfxPreparedFlashOutput> prepared_flash_outputs;
        std::vector<runtime_detail::VfxPreparedBallisticBurst> prepared_ballistic_bursts;
        std::vector<VfxPreparedLegacyEvent> prepared_legacy_events;
        std::vector<std::uint32_t> prepared_legacy_emitters;
        std::vector<runtime_detail::vfx_retained_size::BirthRequest> birth_requests;
        std::vector<std::size_t> birth_order;
        runtime_detail::VfxRibbonInputScratch ribbon_inputs;
        runtime_detail::DecodedVfxState prepared_births;
        std::vector<ParticleSpawnCommand> snapshot_particle_births;
        std::vector<runtime_detail::VfxEntitySurfaceEventCapture> surface_capture_inputs;
        std::vector<runtime_detail::VfxEntitySurfacePersistentCapture> surface_persistent;
        std::vector<runtime_detail::VfxGroundBirthSuppressionFacts> ground_birth_suppressions;
        std::vector<Sequence> replaced_particle_sequences, superseded_pickup_sequences, owned_status_sequences;
        std::vector<Sequence> owned_status_mote_sequences;
        std::vector<Sequence> replaced_legacy_mesh_sequences, replaced_burst_shell_sequences;
        std::vector<std::size_t> vfx_row_indices;
        std::vector<runtime_detail::VfxSessionState::PersistentBurstKey> status_burst_keys;
        std::vector<ParticleSpawnCommand> status_shard_commands;
        const auto explosive_arrow_main = MakeAssetId("particle.skill.explosive_arrow.main").value;
        auto &active_effects = vfx_session.active_effects;
        using VfxEventStore = runtime_detail::ActiveEffectStore;
        using VfxEventProfile = VfxEventStore::Profile;
        std::vector<VfxEventStore::AppendRequest> profile_additions;
        const auto &profile_image = vfx_catalog.Image();
        std::vector<runtime_detail::VfxEventProfileAdmission> profile_admission_plans(
            profile_image.program.effects.size());
        const auto profile_bit = [](VfxEventProfile profile) -> std::uint16_t {
            return static_cast<std::uint16_t>(1u << static_cast<unsigned>(profile));
        };
        for (std::size_t i = 0; i < profile_admission_plans.size(); ++i)
        {
            const auto handle = static_cast<std::uint32_t>(i + 1);
            auto &plan = profile_admission_plans[i];
            if (profile_image.program.effects[i].handle == handle)
                plan = runtime_detail::PrepareVfxEventProfileAdmission(profile_image, handle);
            if (i < profile_image.light_effect_ranges.size() && profile_image.light_effect_ranges[i].count)
                plan.event_mask |= profile_bit(VfxEventProfile::Light);
            if (profile_image.HasDistortionRecipes(handle))
                plan.event_mask |= profile_bit(VfxEventProfile::Distortion);
            if (profile_image.HasDecalRecipes(handle))
                plan.event_mask |= profile_bit(VfxEventProfile::Decal);
            if (runtime_detail::IsVfxTypedVolumeEffect(profile_image, handle))
                plan.event_mask |= profile_bit(VfxEventProfile::Volume);
            if (runtime_detail::IsVfxTypedScreenEffect(profile_image, handle))
                plan.event_mask |= profile_bit(VfxEventProfile::Screen);
            if (plan.frame_rows[static_cast<std::size_t>(VfxEventProfile::Ribbon)] != 0)
                plan.event_mask |= profile_bit(VfxEventProfile::Ribbon);
        }
        VfxEventStore::PreparedAppendBatch prepared_profiles;
        runtime_detail::VfxPersistentSparkState persistent_sparks;
        runtime_detail::VfxUpgradeMovementState vfx_upgrade_movements;
        auto &pending_particle_spawns = vfx_session.decoded.particle_spawns;
        auto &pending_effect_lines = vfx_session.decoded.effect_lines;
        auto &active_vfx_flashes = vfx_session.decoded.flashes;
        const auto vfx_visible_held_bytes = [&]() noexcept {
            using namespace runtime_detail::vfx_retained_size;
            auto held = vfx_session.LogicalBytes();
            const auto charge = [&](const auto &rows) {
                using Row = typename std::remove_cvref_t<decltype(rows)>::value_type;
                held = Add(held, Multiply(rows.size(), sizeof(Row)));
            };
            charge(typed_screen); charge(typed_volumes); charge(typed_lights);
            charge(typed_distortions); charge(distortion_gap_angles); charge(typed_decals);
            charge(typed_surfaces); charge(typed_mesh_spawns); charge(typed_fresnels);
            charge(typed_ground_spawns); charge(ground_gap_angles);
            charge(ribbon_inputs.Sources()); charge(ribbon_inputs.Outputs());
            charge(vfx_session.decoded.entity_surface_captures); charge(surface_capture_inputs);
            return held;
        };
#ifdef HS_DEVELOPMENT_TOOLS
        const auto write_diagnostics_status = [&](std::string_view text) {
            std::ofstream status(artifact_directory / "diagnostics.json", std::ios::binary);
            status.write(text.data(), static_cast<std::streamsize>(text.size()));
            status.close();
            return !status.fail();
        };
        const bool status_started = !capture_diagnostics || write_diagnostics_status(
            "{\"complete\":false,\"reason\":\"running\"}\n");
        if (!status_started)
            record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                "Could not initialize diagnostic status."));
        std::ofstream event_file, timeline_file;
        if (capture_diagnostics)
        {
            event_file.open(artifact_directory / "events.ndjson", std::ios::binary);
            timeline_file.open(artifact_directory / "timeline.csv", std::ios::binary);
        }
        runtime_detail::DiagnosticLog event_lines(event_file, capture_diagnostics);
        runtime_detail::DiagnosticLog timeline_lines(timeline_file, capture_diagnostics,
            "frame,tick,cpu_us,memory_bytes,ecs_count,queue_depth\n");
        std::ofstream performance_file, gpu_live_file;
        std::optional<runtime_detail::DiagnosticLog> performance_lines, gpu_live_lines;
        runtime_detail::PerformanceLogState performance_state;
        bool performance_ready_published{};
        std::uint64_t performance_frequency{};
        if (config.capture_diagnostics)
        {
            LARGE_INTEGER frequency{};
            if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
                record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                    "Could not calibrate performance capture clock."));
            else
                performance_frequency = static_cast<std::uint64_t>(frequency.QuadPart);
            performance_file.open(artifact_directory / "performance.csv", std::ios::binary);
            if (!performance_file)
                record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                    "Could not initialize performance capture files."));
            performance_lines.emplace(performance_file, true,
                runtime_detail::FramePerformanceCsvHeader() + "\n");
            if (config.capture_gpu_timings)
            {
                gpu_live_file.open(artifact_directory / "gpu_live.csv", std::ios::binary);
                if (!gpu_live_file)
                    record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                        "Could not initialize GPU timing capture."));
                gpu_live_lines.emplace(gpu_live_file, true,
                    "completed_frame,view_id,critical_path_ms,critical_path_valid,fence_completed,pass_id,queue,nanoseconds,measured,timestamp_frequency,begin_qpc,end_qpc,qpc_frequency,clock_calibrated,pass_name,gpu_timings_valid,cpu_qpc_frequency,cpu_record_begin_qpc,cpu_record_end_qpc,cpu_record_valid,cpu_provider_submit_begin_qpc,cpu_provider_submit_end_qpc,cpu_provider_submit_valid,frame_metric_kind,timing_main_view,source_frame_sequence,is_generated_frame,source_ready_cadence_ms,calibration_valid,cadence_valid,gpu_backlog_proven,cadence_gap,cadence_exclusions_valid\n");
            }
        }
        bool gpu_drain_succeeded{true};
        std::string gpu_live_row, gpu_live_prefix, gpu_live_suffix;
        bool hero_drain_succeeded{true};
        std::unique_ptr<runtime_detail::TranslucencyDiagnosticsLog> hero_lines;
        if (config.capture_translucency_diagnostics)
        {
            hero_lines = std::make_unique<runtime_detail::TranslucencyDiagnosticsLog>(artifact_directory);
            if (hero_lines->Failed()) record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Could not initialize isolated Hero work capture."));
        }
        std::uint64_t invalid_gpu_frames{};
        const auto append_gpu_capture = [&](const GpuPassTimings &gpu) {
            if (!performance_state.AcceptGpuFrame(gpu.completed_frame_id, !gpu.passes.empty(), gpu.fence_completed)) return;
            if (!gpu.valid) ++invalid_gpu_frames;
            gpu_live_prefix.clear();
            runtime_detail::AppendCsvValues(gpu_live_prefix,
                gpu.completed_frame_id,
                gpu.view_id,
                gpu.critical_path_ms,
                static_cast<unsigned>(gpu.critical_path_valid),
                static_cast<unsigned>(gpu.fence_completed));
            gpu_live_suffix.clear();
            runtime_detail::AppendCsvText(gpu_live_suffix,
                gpu.metric_kind == GpuFrameTimingKind::ReferenceGraphSpan ? "reference_graph_span" :
                    gpu.metric_kind == GpuFrameTimingKind::OutputDependencyInterval ? "output_dependency_interval" :
                    gpu.metric_kind == GpuFrameTimingKind::SourceOutputReady ? "source_output_ready" : "unavailable");
            runtime_detail::AppendCsvValues(gpu_live_suffix,
                static_cast<unsigned>(gpu.main_view), gpu.source_frame_sequence,
                static_cast<unsigned>(gpu.generated_frame), gpu.source_ready_cadence_ms,
                static_cast<unsigned>(gpu.calibration_valid), static_cast<unsigned>(gpu.cadence_valid),
                static_cast<unsigned>(gpu.gpu_backlog_proven), static_cast<unsigned>(gpu.cadence_gap),
                static_cast<unsigned>(gpu.cadence_exclusions_valid));
            gpu_live_suffix += '\n';
            for (const auto &pass : gpu.passes)
            {
                gpu_live_row = gpu_live_prefix;
                runtime_detail::AppendCsvValues(gpu_live_row,
                    pass.pass_id,
                    static_cast<unsigned>(pass.queue),
                    pass.nanoseconds,
                    static_cast<unsigned>(pass.measured),
                    pass.timestamp_frequency,
                    pass.begin_qpc,
                    pass.end_qpc,
                    pass.qpc_frequency,
                    static_cast<unsigned>(pass.clock_calibrated));
                runtime_detail::AppendCsvText(gpu_live_row, pass.name);
                runtime_detail::AppendCsvValues(gpu_live_row,
                    static_cast<unsigned>(gpu.valid),
                    gpu.cpu_qpc_frequency,
                    pass.cpu_record_begin_qpc,
                    pass.cpu_record_end_qpc,
                    static_cast<unsigned>(pass.cpu_record_valid),
                    pass.cpu_provider_submit_begin_qpc,
                    pass.cpu_provider_submit_end_qpc,
                    static_cast<unsigned>(pass.cpu_provider_submit_valid));
                gpu_live_row += ',';
                gpu_live_row += gpu_live_suffix;
                gpu_live_lines->Append(gpu_live_row);
                if (&pass == gpu.passes.data())
                {
                    // Frame facts are stored only on the first pass row and joined by completed_frame.
                    const auto comma_count = std::count(gpu_live_suffix.begin(), gpu_live_suffix.end(), ',');
                    gpu_live_suffix.assign(static_cast<std::size_t>(comma_count), ',');
                    gpu_live_suffix += '\n';
                }
            }
        };
        const auto collect_gpu_capture = [&](bool drain) -> Result {
            if (gpu_live_lines)
            {
                std::vector<GpuPassTimings> completed;
                const auto collected = drain ? renderer.DrainGpuPassTimings(completed)
                                             : renderer.ConsumeCompletedGpuPassTimings(completed);
                if (!collected)
                {
                    gpu_drain_succeeded = false;
                    return collected;
                }
                for (const auto &gpu : completed) append_gpu_capture(gpu);
            }
            if (hero_lines)
            {
                try
                {
                    std::vector<RendererTranslucencyDiagnosticsSample> samples;
                    const auto captured = drain ? renderer.DrainTranslucencyDiagnostics(samples) : renderer.ConsumeCompletedTranslucencyDiagnostics(samples);
                    if (!captured) { hero_drain_succeeded = false; return captured; }
                    for (const auto &sample : samples) hero_lines->Append(sample);
                    if (hero_lines->Failed()) return Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Hero work capture write failed.");
                }
                catch (const std::exception &error)
                {
                    hero_drain_succeeded = false;
                    return Result::Failure(ErrorCode::TaskFailed, "hs_runtime", "Hero work capture failed: " + std::string(error.what()));
                }
            }
            return Result::Success();
        };
#else
        const auto collect_gpu_capture = [](bool) { return Result::Success(); };
#endif
        const auto drain_gpu_capture = [&]() -> Result {
            return collect_gpu_capture(true);
        };
        bool frame_generation_allowed{};
        const auto set_frame_generation_allowed = [&](bool allowed) -> Result {
            if (frame_generation_allowed == allowed) return Result::Success();
            if (renderer.GetPresentationStatus().requested.mode != GraphicsFrameGenerationMode::Off)
                if (auto drained = drain_gpu_capture(); !drained) return drained;
            if (auto changed = renderer.SetFrameGenerationAllowed(allowed); !changed) return changed;
            frame_generation_allowed = allowed;
            publish_graphics_receipt();
            return Result::Success();
        };
#ifdef HS_DEVELOPMENT_TOOLS
        const auto read_performance_qpc = [] {
            LARGE_INTEGER qpc{};
            QueryPerformanceCounter(&qpc);
            return static_cast<std::uint64_t>(qpc.QuadPart);
        };
#endif
        pending_particle_spawns.reserve(256);
        pending_effect_lines.reserve(64);

        Tick last_tick{};
#ifdef HS_DEVELOPMENT_TOOLS
        std::uint32_t typed_vfx_ground_peak{};
        std::uint64_t typed_vfx_ground_submitted_total{};
        std::uint32_t typed_vfx_flash_peak{};
        std::uint32_t typed_vfx_mesh_peak{};
        std::uint32_t typed_vfx_fresnel_peak{};
        std::uint32_t typed_vfx_ribbon_peak{};
        std::uint32_t typed_vfx_ribbon_dropped{};
        std::uint32_t typed_vfx_light_peak{};
        std::uint32_t typed_vfx_distortion_peak{};
        std::uint32_t typed_vfx_decal_peak{};
        std::uint32_t typed_vfx_volume_peak{};
        std::uint64_t typed_vfx_flash_submitted_total{};
#endif
        std::uint64_t static_scene_upload_count{};
#ifdef HS_DEVELOPMENT_TOOLS
        std::uint64_t vfx_fire_budget_limited_frames{};
        std::uint64_t vfx_mote_budget_limited_frames{};
        std::uint64_t vfx_birth_budget_limited_frames{};
        std::size_t vfx_birth_requested_rows{}, vfx_birth_allowed_rows{};
        std::uint64_t vfx_profile_budget_limited_frames{};
        std::size_t vfx_profile_requested_memberships{}, vfx_profile_admitted_memberships{};
        std::size_t vfx_profile_ground_reserved_peak_rows{}, vfx_profile_light_reserved_peak_rows{}, vfx_profile_decal_reserved_peak_rows{};
        std::size_t vfx_profile_volume_reserved_peak_rows{}, vfx_profile_screen_reserved_peak_rows{};
        std::size_t vfx_profile_distortion_reserved_peak_rows{}, vfx_profile_distortion_reserved_peak_gaps{};
        std::size_t vfx_profile_ground_reserved_peak_gaps{}, vfx_profile_ribbon_reserved_peak_rows{}, vfx_profile_ribbon_reserved_peak_outputs{};
        std::size_t vfx_fire_admission_peak_bytes{};
        std::uint64_t typed_vfx_unsupported_inputs{};
#endif
#ifdef HS_DEVELOPMENT_TOOLS
        bool smoke_finished{};
#endif
        bool resized{};
        bool vfx_showcase_injected{};
        const auto log_resize_failure = [&](const Result &failure) {
#ifdef HS_DEVELOPMENT_TOOLS
            const auto message = std::format("Recoverable display resize failed: {}\n",
                                             failure.Message());
            log_error("resize_errors.log", message);
#else
            (void)failure;
#endif
        };
        auto next_frame = std::chrono::steady_clock::now();
#ifdef HS_DEVELOPMENT_TOOLS
        std::uint64_t previous_capture_log_ticks{};
        std::string performance_row;
#endif
        RendererFrameResult frame;
        for (;;)
        {
#ifdef HS_DEVELOPMENT_TOOLS
            const auto loop_start_qpc = performance_lines ? read_performance_qpc() : 0;
#endif
            GraphicsSettingsCommitRequest renderer_settings;
            bool apply_renderer_settings{};
            while (channels.renderer_settings.TryPop(renderer_settings))
                apply_renderer_settings = true;
            NativeWindowMessage window_message;
            bool display_receipt_changed{};
            while (channels.window_messages.TryPop(window_message))
            {
                renderer.HandleWindowMessage(window_message);
                display_receipt_changed |= window_message.message == WM_DISPLAYCHANGE;
            }
            if (display_receipt_changed) publish_graphics_receipt();
            ResizeCommand resize;
            const bool has_resize = channels.resize_commands.TryPop(resize);
            if (apply_renderer_settings || has_resize)
                if (auto drained = drain_gpu_capture(); !drained)
                {
                    record_thread_failure(drained);
                    break;
                }
            auto resize_error = ErrorCode::None;
            if (has_resize)
            {
                if (auto resize_result = renderer.Resize(resize.width, resize.height);
                    !resize_result)
                {
                    if (resize_result.Code() != ErrorCode::TaskFailed)
                    {
                        record_thread_failure(resize_result);
                        break;
                    }
                    log_resize_failure(resize_result);
                    resize_error = resize_result.Code();
                }
                else if (!apply_renderer_settings)
                {
                    active_renderer_settings.width = resize.width;
                    active_renderer_settings.height = resize.height;
                    publish_graphics_receipt();
                }
            }
            // WM_SIZE is enqueued synchronously by the window-thread mode change.
            // ApplyOptions may only acknowledge that change after Resize succeeds.
            if (apply_renderer_settings)
            {
                const auto &effective = renderer_settings.effective;
                bool display_target_changed{};
                if (resize_error == ErrorCode::None &&
                    effective.borderless != active_borderless)
                {
                    if (auto resize_result = renderer.Resize(effective.width,
                                                              effective.height);
                        !resize_result)
                    {
                        if (resize_result.Code() != ErrorCode::TaskFailed)
                        {
                            record_thread_failure(resize_result);
                            break;
                        }
                        log_resize_failure(resize_result);
                        resize_error = resize_result.Code();
                    }
                    else
                        active_borderless = effective.borderless;
                }
                auto settings_error = resize_error;
                if (settings_error == ErrorCode::None)
                {
                    auto next_graphics = renderer_settings.graphics ? *renderer_settings.graphics : active_graphics;
                    GraphicsDisplayId next_native_output;
                    display_target_changed = next_graphics.display_output.value != "auto" &&
                        next_graphics.display_output != active_native_output;
                    auto applied = ResolveGraphicsDisplayOutput(window.Handle(),
                                                                next_graphics.display_output,
                                                                next_native_output);
                    if (applied)
                    {
                        display_target_changed = next_native_output != active_native_output;
                        applied = renderer.RequestGraphicsSettings(
                            GraphicsSettingsRequest{renderer_settings.generation,
                                renderer_settings.cause, next_graphics, next_native_output});
                    }
                    settings_error = applied.Code();
                    if (applied)
                    {
                        active_graphics = std::move(next_graphics);
                        active_native_output = std::move(next_native_output);
                        active_settings_generation = renderer_settings.generation;
                        active_renderer_settings = effective;
                        if (renderer_settings.cause == GraphicsSettingsRequestCause::Recovery)
                            renderer_settings_suspended = false;
                        active_frame_cap = effective.frame_cap;
                        active_decorative_particle_fraction = renderer.DecorativeParticleFraction();
                        const auto next_vfx_quality = active_graphics.particle_quality == GraphicsQuality::Low
                            ? VfxQuality::Low
                            : active_graphics.particle_quality == GraphicsQuality::Medium
                                ? VfxQuality::Medium : VfxQuality::High;
                        if (next_vfx_quality != active_vfx_quality)
                        {
                            active_vfx_quality = next_vfx_quality;
                            active_effects.SetQuality(active_vfx_quality);
                        }
                        publish_graphics_receipt();
                    }
                    else
                    {
                        // Resize and options are separate native transactions.
                        // A cross-monitor failure is acknowledged while the
                        // window thread rolls placement back. Recovery is
                        // submitted only after that placement completes.
                        if (display_target_changed)
                            renderer_settings_suspended = true;
                        else
                        {
                            auto restored = restore_renderer_settings();
                            if (!restored) { record_thread_failure(restored); break; }
                            active_borderless = active_renderer_settings.borderless;
                            publish_graphics_receipt();
                        }
                    }
                }
                if (!channels.renderer_settings_acks.TryPush(
                        {renderer_settings.generation, settings_error}))
                {
                    record_thread_failure(Result::Failure(
                        ErrorCode::InvalidState, "hs_runtime",
                        "Renderer settings acknowledgment queue capacity exceeded."));
                    break;
                }
                channels.NotifyMain();
            }

            const bool active_gameplay = AllowGameplayFrameGeneration(static_cast<SessionPhase>(
                channels.session_phase.load(std::memory_order_acquire)));
            if (auto policy = set_frame_generation_allowed(active_gameplay); !policy)
            { record_thread_failure(policy); break; }
            const bool has_current_snapshot = !renderer_settings_suspended && snapshot_consumer.AcquireLatest().has_current;
            std::uint64_t input_token{};
#ifdef HS_DEVELOPMENT_TOOLS
            const auto latency_begin_qpc = performance_lines ? read_performance_qpc() : 0;
            std::uint64_t input_wait_ticks{}, simulation_wait_ticks{};
#endif
            // Evaluate Auto cadence even while its native latency/FG owner is Off,
            // so a recovered base cadence can re-enable the existing request.
            if (has_current_snapshot)
                if (auto begun = renderer.BeginInputFrame(input_token); !begun)
                { record_thread_failure(begun); break; }
#ifdef HS_DEVELOPMENT_TOOLS
            const auto latency_begin_ticks = performance_lines
                ? runtime_detail::PerformanceLogState::ElapsedTicks(latency_begin_qpc, read_performance_qpc()) : 0;
#endif
            auto current_presentation = renderer.GetPresentationStatus();
            const bool native_latency = current_presentation.low_latency_active;
            if (current_presentation.effective_provider != previous_ui_presentation.effective_provider ||
                current_presentation.active_mode != previous_ui_presentation.active_mode ||
                current_presentation.maximum_multiplier != previous_ui_presentation.maximum_multiplier ||
                current_presentation.low_latency_active != previous_ui_presentation.low_latency_active ||
                current_presentation.availability != previous_ui_presentation.availability ||
                current_presentation.fallback_reason != previous_ui_presentation.fallback_reason)
            {
                previous_ui_presentation = std::move(current_presentation);
                publish_graphics_receipt();
            }
            const bool has_native_snapshot = native_latency && has_current_snapshot;
            if (native_latency && has_native_snapshot && config.smoke)
            {
                record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                    "Native input latency requires actual window input; smoke playback has stopped that input source."));
                break;
            }
            native_input_frame.SetEnabled(native_latency && has_native_snapshot);
            if (native_latency && has_native_snapshot)
            {
                const auto token = input_token;
                if (!native_input_frame.RequestInput(token))
                {
                    if (!channels.stop_requested.load(std::memory_order_acquire))
                        record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Native input frame request violated its acknowledged token order."));
                    break;
                }
#ifdef HS_DEVELOPMENT_TOOLS
                const auto input_wait_begin = performance_lines ? read_performance_qpc() : 0;
#endif
                if (!native_input_frame.WaitInput(token)) break;
#ifdef HS_DEVELOPMENT_TOOLS
                if (performance_lines) input_wait_ticks = runtime_detail::PerformanceLogState::ElapsedTicks(input_wait_begin, read_performance_qpc());
#endif
                if (auto sampled = renderer.InputSampled(token); !sampled) { record_thread_failure(sampled); break; }
                if (!native_input_frame.RequestSimulation(token))
                {
                    if (!channels.stop_requested.load(std::memory_order_acquire))
                        record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Native simulation request did not match acknowledged input."));
                    break;
                }
#ifdef HS_DEVELOPMENT_TOOLS
                const auto simulation_wait_begin = performance_lines ? read_performance_qpc() : 0;
#endif
                if (!native_input_frame.WaitSimulation(token)) break;
#ifdef HS_DEVELOPMENT_TOOLS
                if (performance_lines) simulation_wait_ticks = runtime_detail::PerformanceLogState::ElapsedTicks(simulation_wait_begin, read_performance_qpc());
#endif
                if (auto simulated = renderer.SimulationCompleted(token); !simulated) { record_thread_failure(simulated); break; }
            }
            // Acquire the selected snapshot before sampling the event prefix:
            // its publication follows that tick's event pushes on the producer.
            auto snapshots = snapshot_consumer.AcquireLatest();
            vfx_session.raw_events.DrainIngress(channels.presentation_events, [&](const PresentationEvent &event) {
#ifdef HS_DEVELOPMENT_TOOLS
                if (capture_diagnostics) event_lines.Append(
                    std::format("{{\"sequence\":{},\"tick\":{},\"kind\":{},"
                                "\"asset\":{},\"position\":[{},{},{}]}}\n",
                                event.sequence, event.tick, static_cast<unsigned>(event.kind),
                                event.asset.value, event.position.x, event.position.y,
                                event.position.z));
#else
                (void)event;
#endif
            });
            if (renderer_settings_suspended)
            {
                if (channels.stop_requested.load(std::memory_order_acquire) &&
                    channels.simulation_done.load(std::memory_order_acquire))
                    break;
                std::this_thread::yield();
                continue;
            }
            if (snapshots.has_current &&
                (!config.smoke_render_lockstep ||
                 snapshots.current.header.tick > rendered_tick.load(std::memory_order_acquire)))
            {
#if defined(HS_DEVELOPMENT_TOOLS)
                const Tick source_snapshot_tick = snapshots.current.header.tick;
                const std::uint64_t source_snapshot_session = snapshots.current.header.session_id;
                if (asset_test_room)
                {
                    const auto now = std::chrono::steady_clock::now();
                    const auto actions = window.ConsumeTestRoomActions();
                    if (actions & 1u) room_paused = !room_paused;
                    room_capture = (actions & 4u) != 0;
                    if (config.test_room_capture)
                        room_time = static_cast<double>(snapshots.current.header.tick > 30 ? snapshots.current.header.tick - 30 : 0) / 60.0;
                    else if (!room_paused)
                        room_time += std::chrono::duration<double>(now - room_previous_clock).count();
                    room_previous_clock = now;
                    snapshots.current.header.tick = 1 + static_cast<Tick>(room_time * 60.0);
                    snapshots.current.header.simulation_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::duration<double>(room_time));
                    if (snapshots.has_previous)
                    {
                        snapshots.previous.header.tick = room_previous_display_tick ? room_previous_display_tick : snapshots.current.header.tick;
                        snapshots.previous.header.simulation_time = room_previous_display_tick
                            ? room_previous_simulation_time : snapshots.current.header.simulation_time;
                    }
                    room_previous_display_tick = snapshots.current.header.tick;
                    room_previous_simulation_time = snapshots.current.header.simulation_time;
                    snapshots.current.header.session_id = room_session;
                    if (snapshots.has_previous) snapshots.previous.header.session_id = room_session;
                    asset_test_room->Prepare(room_time, snapshots.current.header.tick, snapshots);
                    snapshots.ui_frame.reset();
                    if (room_paused) snapshots.current.header.clock.mode = RenderClockMode::Frozen;
                    snapshots.current.header.dynamic_pair_revision = ++room_revision;
                    if (snapshots.has_previous) snapshots.previous.header.dynamic_pair_revision = room_revision > 1 ? room_revision - 1 : room_revision;
                }
#endif
                const auto &current_snapshot = snapshots.current;
                const auto sample_time = std::chrono::steady_clock::now();
                const auto sample_alpha = RenderSnapshotInterpolation(snapshots, sample_time, realtime);
                const double display_tick = static_cast<double>(current_snapshot.header.tick) -
                    (snapshots.has_previous && sample_alpha < 1.0f ? 1.0 - sample_alpha : 0.0);
                if (vfx_session.Synchronize(current_snapshot.header.session_id))
                {
                    persistent_sparks.Clear();
                    vfx_upgrade_movements.Clear();
                    vfx_showcase_injected = false;
                }
                prepared_births.Clear();
                typed_screen.clear(); typed_volumes.clear(); typed_lights.clear();
                typed_distortions.clear(); distortion_gap_angles.clear(); typed_decals.clear();
                typed_surfaces.clear(); typed_mesh_spawns.clear(); typed_fresnels.clear();
                typed_ground_spawns.clear(); ground_gap_angles.clear(); ribbon_inputs.Reset();
                visible_flashes.clear();
                vfx_session.decoded.AppendVisibleFlashes(display_tick, visible_flashes);
                const auto retained_flash_count = active_vfx_flashes.size();
                auto retained_particle_count = pending_particle_spawns.size();
                const auto &ready_batch = vfx_session.raw_events.Prepare(
                    current_snapshot.header.tick, vfx_session.session_id);
                if (auto dispatched = runtime_detail::DispatchVfxUpgradeEvents(
                        vfx_catalog.Program(), ready_batch.events, frame_events, &dispatched_event_scratch); !dispatched)
                {
                    record_thread_failure(dispatched);
                    break;
                }
                const bool inject_pickup_showcase = config.vfx_showcase && !vfx_showcase_injected;
                if (inject_pickup_showcase)
                {
                    constexpr std::array pickup_effects{
                        "particle.pickup.xp_collect", "particle.pickup.heal_collect",
                        "particle.pickup.magnet_collect", "particle.pickup.relic_collect"};
                    for (std::size_t index = 0; index < pickup_effects.size(); ++index)
                    {
                        PresentationEvent event;
                        event.sequence = 0xF100u + index;
                        event.tick = current_snapshot.header.tick;
                        event.session_id = current_snapshot.header.session_id;
                        event.kind = PresentationKind::Vfx;
                        event.asset = MakeAssetId(pickup_effects[index]);
                        event.position = {(static_cast<float>(index % 2) - .5f) * 4.0f,
                                          .35f, (static_cast<float>(index / 2) - .5f) * 4.0f};
                        event.parameters = EncodeVfxParameters({{0, 0, 1}, 1,
                            {event.position.x + .9f, .6f, event.position.z + .6f},
                            static_cast<std::uint32_t>(VfxEventFlag::HasTarget)});
                        frame_events.push_back(event);
                    }
                }
#if defined(HS_DEVELOPMENT_TOOLS)
                if (asset_test_room)
                    asset_test_room->AppendLegacyEvents(vfx_catalog.Image(), room_time,
                        snapshots.current.header.tick, snapshots.current.header.session_id, frame_events);
#endif
                if (config.resize_test && !resized &&
                    snapshots.current.header.tick >= config.maximum_ticks / 2)
                {
                    if (auto drained = drain_gpu_capture(); !drained)
                    {
                        record_thread_failure(drained);
                        break;
                    }
                    if (auto resize_result =
                            renderer.Resize(config.width + 64, config.height + 36);
                        !resize_result)
                    {
                        record_thread_failure(resize_result);
                        break;
                    }
                    resized = true;
                }
                DevToolsFrameData devtools{
                    static_cast<std::uint32_t>(task_system.WorkerCount()),
                    static_cast<std::uint32_t>(channels.action_edges.Size()),
                    static_cast<std::uint32_t>(channels.presentation_events.Size()),
                    0,
                    static_cast<std::uint32_t>(channels.resize_commands.Size()),
                    static_cast<std::uint32_t>(channels.debug_commands.Size()),
                    channels.dropped_input_edges.load(std::memory_order_relaxed),
                    channels.dropped_presentation_events.load(std::memory_order_relaxed),
                    0};
#if defined(HS_DEVELOPMENT_TOOLS)
                devtools.preview = preview_edit;
                devtools.test_room = asset_test_room.has_value();
                devtools.station_labels = room_labels;
                if (config.smoke_render_lockstep)
                    devtools.delta_seconds =
                        std::chrono::duration<float>(FixedStepClock::kFixedStep).count();
                devtools.wait_for_streaming = config.smoke_render_lockstep;
#endif
                const auto current_vfx = snapshots.has_current
                    ? snapshots.current.persistent_vfx
                    : std::span<const PersistentVfxVisual>{};
                const auto previous_vfx = snapshots.has_previous &&
                    snapshots.previous.header.session_id == snapshots.current.header.session_id
                    ? snapshots.previous.persistent_vfx
                    : std::span<const PersistentVfxVisual>{};
                const bool new_vfx_pair = vfx_pair_facts.Prepare({
                    .session_id = snapshots.current.header.session_id,
                    .previous_publication = snapshots.has_previous
                        ? snapshots.previous.header.dynamic_pair_revision
                        : Sequence{},
                    .current_publication = snapshots.current.header.dynamic_pair_revision},
                    previous_vfx, current_vfx, snapshots.current.instances,
                    snapshots.current.captured_instances);
                if (snapshots.has_current && (new_vfx_pair ||
                    (vfx_session.has_status_burst_tick &&
                     snapshots.current.header.tick < vfx_session.last_status_burst_tick)))
                {
                    vfx_session.RetireUnownedPersistentBursts(true,
                        snapshots.current.header.tick, [&](const auto &key) noexcept {
                            const auto kind = static_cast<PersistentVfxKind>(key.visual_kind);
                            return vfx_pair_facts.Current(key.stable_id, kind, current_vfx) ||
                                vfx_pair_facts.Previous(key.stable_id, kind, previous_vfx);
                        });
                }
                active_effects.BeginFrameBindings();
                const auto refresh_owner_event = [&](VfxEventInput &input) {
                    return runtime_detail::RefreshVfxGroundEventOwner(input,
                        current_vfx, snapshots.current.header.tick,
                        vfx_pair_facts, previous_vfx, sample_alpha);
                };
                runtime_detail::BuildVfxTypedFrameInputs(
                    vfx_catalog, frame_events,
#if defined(HS_DEVELOPMENT_TOOLS)
                    asset_test_room ? std::span<const PersistentVfxVisual>{} : current_vfx,
                    asset_test_room ? std::span<const PersistentVfxVisual>{} : previous_vfx,
#else
                    current_vfx, previous_vfx,
#endif
                    snapshots.has_current ? snapshots.current.header.tick : Tick{},
                    typed_vfx, vfx_pair_facts, sample_alpha, active_vfx_quality);
                {
                    vfx_upgrade_movements.Append(vfx_catalog, frame_events,
                        snapshots.has_current ? snapshots.current.header.tick : Tick{},
                        typed_vfx.persistent, sample_alpha, active_vfx_quality);
                }
#if defined(HS_DEVELOPMENT_TOOLS)
                if (asset_test_room)
                    asset_test_room->AppendInputs(vfx_catalog.Image(), room_time,
                        snapshots.current.header.tick, active_vfx_quality, typed_vfx,
                        snapshots.current.instances.empty() ? Float3{} : snapshots.current.instances.front().position,
                        config.test_room_capture ? std::optional<std::size_t>{*config.test_room_capture - 1} : std::nullopt);
#endif
                // Reserve retained profile storage before generating the birth fanout.
                const auto phase_effect = vfx_catalog.Image().entity_surface_routes.phase_effect;
                const auto death_effect = vfx_catalog.Image().entity_surface_routes.death_effect;
                const auto trap_effect = vfx_catalog.Image().entity_surface_routes.trap_point_rate_effect;
                const auto is_effect = [](const VfxEventInput &input, std::uint32_t handle) {
                    return handle != 0 && input.effect_handle == handle;
                };
                ground_birth_suppressions.clear();
                // Birth-time eligibility and duplicate sequences differ from display-time ground outputs.
                runtime_detail::AppendVfxTypedEventGroundBirthSuppression(
                    vfx_catalog.Image(), typed_vfx.events, ground_birth_suppressions);
                std::ranges::sort(ground_birth_suppressions, {},
                    &runtime_detail::VfxGroundBirthSuppressionFacts::sequence);
                auto unique_ground = ground_birth_suppressions.begin();
                for (auto it = ground_birth_suppressions.begin(); it != ground_birth_suppressions.end();)
                {
                    auto merged = *it++;
                    while (it != ground_birth_suppressions.end() && it->sequence == merged.sequence)
                    {
                        merged.all_particles |= it->all_particles;
                        merged.ground_ring |= it->ground_ring;
                        merged.ground_disc |= it->ground_disc;
                        merged.segment_solid_trail |= it->segment_solid_trail;
                        ++it;
                    }
                    *unique_ground++ = merged;
                }
                ground_birth_suppressions.erase(unique_ground, ground_birth_suppressions.end());
                replaced_particle_sequences.clear();
                superseded_pickup_sequences.clear();
                owned_status_sequences.clear();
                {
                    owned_status_sequences.reserve(typed_vfx.events.size());
                    runtime_detail::PrepareVfxOwnedStatusTickOutputs(
                        vfx_catalog.Image(), typed_vfx.events, typed_vfx.persistent,
                        prepared_births.flashes, owned_status_sequences,
                        prepared_status_mote_bursts, vfx_row_indices);
                    std::ranges::sort(owned_status_sequences);
                    runtime_detail::PrepareVfxTypedFlashOutputs(
                        vfx_catalog.Image(), typed_vfx.events, owned_status_sequences,
                        prepared_flash_outputs);
                }
                const auto ballistic_first = prepared_births.particle_spawns.size();
                auto ballistic_end = ballistic_first;
                replaced_legacy_mesh_sequences.clear();
                replaced_burst_shell_sequences.clear();
                {
                    prepared_ballistic_bursts.clear();
                    runtime_detail::PrepareVfxTypedBallisticBursts(
                        vfx_catalog.Image(), typed_vfx.events, prepared_ballistic_bursts);
                }
                prepared_legacy_events.clear();
                prepared_legacy_emitters.clear();
                prepared_legacy_events.reserve(frame_events.size());
                for (const auto &ready_event : frame_events)
                {
                    const auto fact = std::ranges::lower_bound(ground_birth_suppressions,
                        ready_event.sequence, {},
                        &runtime_detail::VfxGroundBirthSuppressionFacts::sequence);
                    const VfxLegacyGroundSuppression ground_suppression =
                        fact != ground_birth_suppressions.end() && fact->sequence == ready_event.sequence
                            ? VfxLegacyGroundSuppression{fact->all_particles, fact->ground_ring,
                                  fact->ground_disc, fact->segment_solid_trail}
                            : VfxLegacyGroundSuppression{};
                    VfxPreparedLegacyEvent prepared;
                    if (auto expanded = vfx_catalog.PrepareEvent(
                            ready_event, active_decorative_particle_fraction, prepared,
                            prepared_legacy_emitters, false, false, ground_suppression); !expanded)
                    {
                        record_thread_failure(expanded);
                        break;
                    }
                    else if (prepared.definition) prepared_legacy_events.push_back(prepared);
                }
                if (channels.stop_requested.load(std::memory_order_acquire)) break;
                const bool append_vfx_showcase = config.vfx_showcase && !vfx_showcase_injected;
                if (append_vfx_showcase)
                {
                    constexpr std::array colors{
                        Float4{0.35f, 0.75f, 2.5f, 0.9f},
                        Float4{1.8f, 0.25f, 2.2f, 0.9f},
                        Float4{3.0f, 2.4f, 0.5f, 1.0f},
                        Float4{0.4f, 2.2f, 3.0f, 0.9f},
                        Float4{0.65f, 0.72f, 0.8f, 0.8f},
                        Float4{3.2f, 0.55f, 0.08f, 0.95f},
                        Float4{1.8f, 0.02f, 0.03f, 0.95f},
                        Float4{0.4f, 1.6f, 2.8f, 0.95f},
                        Float4{2.8f, 2.8f, 3.0f, 1.0f},
                        Float4{0.3f, 1.2f, 3.0f, 0.9f},
                        Float4{0.7f, 1.8f, 3.0f, 0.9f}};
                    constexpr std::array primitives{
                        VfxPrimitive::Disc, VfxPrimitive::Ring, VfxPrimitive::Sector,
                        VfxPrimitive::Chevron, VfxPrimitive::Rune, VfxPrimitive::Cracks,
                        VfxPrimitive::Arrow, VfxPrimitive::Shard, VfxPrimitive::Ember,
                        VfxPrimitive::Spike, VfxPrimitive::ShockShell,
                        VfxPrimitive::SolidTrail, VfxPrimitive::DashedRicochet,
                        VfxPrimitive::FireTransfer, VfxPrimitive::RelicChain,
                        VfxPrimitive::DashWake};
                    for (std::size_t index = 0; index < primitives.size(); ++index)
                    {
                        ParticleSpawnCommand command;
                        command.sequence = 0xF000u + index;
                        command.tick = 0;
                        command.position = {
                            (static_cast<float>(index % 6) - 2.5f) * 3.2f,
                            index < 6 || index >= 11 ? 0.04f : 1.0f,
                            (static_cast<float>(index / 6) - 1.0f) * 4.0f};
                        command.direction = index >= 6 && index < 11
                                                ? Float3{0.5f, 0.35f, 1.0f}
                                                : Float3{0.0f, 1.0f, 0.0f};
                        command.shape = ParticleShape::Point;
                        command.velocity_mode = ParticleVelocity::Direction;
                        command.speed_min = command.speed_max =
                            index >= 6 && index < 11 ? 0.001f : 0.0f;
                        command.facing = index < 6 || index >= 11
                                             ? ParticleFacing::Ground
                                             : ParticleFacing::Velocity;
                        command.renderer = index < 6 ? VfxRenderer::Ground
                                           : index < 11 ? VfxRenderer::Mesh
                                                        : VfxRenderer::Segment;
                        command.primitive = primitives[index];
                        command.count = 1;
                        command.lifetime_min = command.lifetime_max = 10.0f;
                        command.start_color = command.end_color = colors[index % colors.size()];
                        command.start_size_min = command.start_size_max =
                            command.renderer == VfxRenderer::Segment ? 0.18f : 1.15f;
                        command.end_size_min = command.end_size_max =
                            command.start_size_min;
                        command.stretch = command.renderer == VfxRenderer::Segment ? 7.0f : 1.0f;
                        command.seed = static_cast<std::uint32_t>(index + 1);
                        prepared_births.particle_spawns.push_back(command);
                    }
                }

                std::size_t fire_requested{}, fire_admitted{}, fire_admission_bytes{};
                std::size_t required_fire{}, decorative_fire{};
                bool fire_budget_limited{};
#ifdef HS_DEVELOPMENT_TOOLS
                bool birth_budget_limited{};
#endif
                bool mote_birth_budget_limited{};
                {
                    using namespace runtime_detail::vfx_retained_size;
                    status_stamp_flashes.clear();
                    runtime_detail::AppendVfxPersistentStatusStampCommands(
                        vfx_catalog.Image(), typed_vfx.persistent, status_stamp_flashes);
                    runtime_detail::PrepareVfxPersistentFireSources(vfx_catalog.Image(),
                        typed_vfx.persistent, display_tick, prepared_fire_sources);
                    for (const auto &source : prepared_fire_sources)
                    {
                        fire_requested = Add(fire_requested, source.eligible_ordinals);
                        auto &count = source.authoritative ? required_fire : decorative_fire;
                        count = Add(count, source.eligible_ordinals);
                    }
                    owned_status_mote_sequences.clear();
                    owned_status_mote_sequences.reserve(prepared_status_mote_bursts.size());
                    for (const auto &burst : prepared_status_mote_bursts)
                        owned_status_mote_sequences.push_back(burst.event_sequence);
                    std::ranges::sort(owned_status_mote_sequences);
                    owned_status_mote_sequences.erase(std::unique(owned_status_mote_sequences.begin(),
                        owned_status_mote_sequences.end()), owned_status_mote_sequences.end());
                    runtime_detail::PrepareVfxTypedMoteBursts(
                        vfx_catalog.Image(), typed_vfx.events, owned_status_mote_sequences,
                        prepared_mote_bursts);

                    // Reserve later essential shard births before spending on decorations.
                    status_burst_keys.clear();
                    status_shard_commands.clear();
                    vfx_session.SelectUnseenPersistentBurstOwners(typed_vfx.persistent, vfx_row_indices);
                    status_burst_keys.reserve(vfx_row_indices.size());
                    for (const auto index : vfx_row_indices)
                    {
                        const auto &owner = typed_vfx.persistent[index];
                        status_burst_keys.push_back({owner.stable_id, owner.source_visual_kind});
                    }
                    runtime_detail::AppendVfxSelectedPersistentStatusShardCommands(
                        vfx_catalog.Image(), typed_vfx.persistent, vfx_row_indices,
                        snapshots.current.header.tick, status_shard_commands);
                    vfx_session.ReservePersistentBursts(status_burst_keys.size());

                    // Required rows, including compatibility fallback, are outside the soft cap.
                    std::size_t profile_output_bytes{};
                    std::array required_rows{Add(prepared_births.particle_spawns.size(), status_shard_commands.size()),
                        prepared_births.effect_lines.size(), prepared_births.flashes.size()};
                    birth_requests.clear();
                    birth_requests.reserve(Add(Add(prepared_flash_outputs.size(), prepared_ballistic_bursts.size()),
                        std::max(prepared_mote_bursts.size(), prepared_legacy_events.size())));
                    const auto prepare_row = [&](auto &plan, std::size_t storage) {
                        plan.admitted_count = plan.count;
                        if (plan.cosmetic)
                            birth_requests.push_back({plan.count, &plan.admitted_count, plan.importance, storage});
                        else required_rows[storage] = Add(required_rows[storage], plan.count);
                    };
                    for (auto &plan : prepared_flash_outputs) prepare_row(plan, 2);
                    for (auto &plan : prepared_ballistic_bursts) prepare_row(plan, 0);
                    for (const auto &plan : prepared_status_mote_bursts)
                        required_rows[2] = Add(required_rows[2], plan.count);
                    for (auto &plan : prepared_mote_bursts) prepare_row(plan, 2);
                    for (const auto &plan : prepared_legacy_events)
                        required_rows[plan.definition->kind == VfxDefinitionKind::Line ? 1 : 0] =
                            Add(required_rows[plan.definition->kind == VfxDefinitionKind::Line ? 1 : 0],
                                plan.maximum_required_count);
                    const auto required_suffix = Add(status_stamp_flashes.size(), required_fire);
                    const auto select_birth_rows = [&](std::array<std::size_t, 3> &rows,
                        std::span<BirthRequest> requests) {
                        vfx_session.SelectBirthRows(rows, requests, birth_order,
                            [&]() noexcept { return Add(vfx_session.LogicalBytes(),
                                Add(Multiply(visible_flashes.size(), sizeof(VfxFlashSpawnInput)), profile_output_bytes)); },
                            prepared_births, visible_flashes, required_suffix,
                            runtime_detail::kVfxWorkspaceSoftBudgetBytes);
#ifdef HS_DEVELOPMENT_TOOLS
                        if (capture_diagnostics) for (const auto &request : requests)
                        {
                            vfx_birth_requested_rows = Add(vfx_birth_requested_rows, request.count);
                            vfx_birth_allowed_rows = Add(vfx_birth_allowed_rows, *request.admitted);
                            birth_budget_limited |= *request.admitted < request.count;
                        }
#endif
                    };
                    select_birth_rows(required_rows, {});
                    {
                        profile_additions.clear();
                        const auto &image = vfx_catalog.Image();
                        // These arrays stay unchanged through all later admission queries.
                        for (const auto &input : typed_vfx.events)
                        {
                            std::uint16_t mask{};
                            const auto ground_admission = runtime_detail::ClassifyVfxTypedEventGroundAdmission(image, input);
                            const auto *plan = input.effect_handle != 0 &&
                                input.effect_handle <= profile_admission_plans.size()
                                ? &profile_admission_plans[input.effect_handle - 1] : nullptr;
                            if (ground_admission.accepted) mask |= profile_bit(VfxEventProfile::Ground);
                            if (is_effect(input, phase_effect) || is_effect(input, death_effect))
                                mask |= profile_bit(VfxEventProfile::EntitySurface);
                            if (is_effect(input, trap_effect)) mask |= profile_bit(VfxEventProfile::PointRate);
                            if (plan) mask |= plan->event_mask;
                            if (mask)
                            {
                                const auto cosmetic = static_cast<std::uint16_t>(
                                    !ground_admission.replace_context_owner && plan
                                    ? mask & plan->cosmetic_mask : 0u);
                                const auto importance = input.effect_handle != 0 &&
                                    input.effect_handle <= image.program.effects.size()
                                    ? image.program.effects[input.effect_handle - 1].importance : 0u;
                                const auto *frame_rows = plan ? &plan->frame_rows : nullptr;
                                const auto ribbon_outputs = plan ? plan->ribbon_frame_outputs : 0u;
                                profile_additions.push_back({&input, mask,
                                    ground_admission.replace_context_owner, cosmetic, importance, frame_rows, ribbon_outputs});
                            }
                        }
                        using runtime_detail::vfx_retained_size::Add;
                        constexpr auto ground_profile = static_cast<std::size_t>(VfxEventProfile::Ground);
                        constexpr auto ribbon_profile = static_cast<std::size_t>(VfxEventProfile::Ribbon);
                        constexpr auto light_profile = static_cast<std::size_t>(VfxEventProfile::Light);
                        constexpr auto distortion_profile = static_cast<std::size_t>(VfxEventProfile::Distortion);
                        constexpr auto decal_profile = static_cast<std::size_t>(VfxEventProfile::Decal);
                        constexpr auto volume_profile = static_cast<std::size_t>(VfxEventProfile::Volume);
                        constexpr auto screen_profile = static_cast<std::size_t>(VfxEventProfile::Screen);
                        constexpr auto surface_profile = static_cast<std::size_t>(VfxEventProfile::EntitySurface);
                        std::array<std::size_t, static_cast<std::size_t>(VfxEventProfile::Count)> frame_rows{};
                        std::size_t frame_distortion_gaps{};
                        std::size_t frame_ground_gaps{}, frame_ribbon_outputs{};
                        std::size_t frame_mesh_rows{}, frame_fresnel_rows{}, frame_surface_persistent_rows{};
                        const auto frame_surface_captures = Add(vfx_session.decoded.entity_surface_captures.size(),
                            active_effects.ProfileEvents(VfxEventProfile::EntitySurface).size());
                        for (const auto profile : {VfxEventProfile::Ground, VfxEventProfile::Light,
                                                  VfxEventProfile::Distortion, VfxEventProfile::Decal,
                                                  VfxEventProfile::Volume, VfxEventProfile::Screen, VfxEventProfile::Ribbon,
                                                  VfxEventProfile::EntitySurface})
                            for (const auto &input : active_effects.ProfileEvents(profile))
                                if (input.effect_handle != 0 && input.effect_handle <= profile_admission_plans.size())
                                {
                                    const auto index = static_cast<std::size_t>(profile);
                                    const auto rows = profile_admission_plans[input.effect_handle - 1].frame_rows[index];
                                    frame_rows[index] = Add(frame_rows[index], rows);
                                    if ((profile == VfxEventProfile::Distortion || profile == VfxEventProfile::Ground) && rows != 0)
                                        if (const auto *ring = std::get_if<VfxRingGapsPayload>(&input.payload);
                                            ring && !ring->uniform_gaps.Active())
                                        {
                                            auto &gaps = profile == VfxEventProfile::Ground ? frame_ground_gaps : frame_distortion_gaps;
                                            gaps = Add(gaps, ring->gap_angles_degrees.size());
                                        }
                                    if (profile == VfxEventProfile::Ribbon)
                                        frame_ribbon_outputs = Add(frame_ribbon_outputs,
                                            profile_admission_plans[input.effect_handle - 1].ribbon_frame_outputs);
                                }
                        for (const auto &capture : vfx_session.decoded.entity_surface_captures)
                            if (const auto *input = active_effects.Get(capture.row);
                                input && input->effect_handle != 0 && input->effect_handle <= profile_admission_plans.size())
                                frame_rows[surface_profile] = Add(frame_rows[surface_profile],
                                    profile_admission_plans[input->effect_handle - 1].frame_rows[surface_profile]);
                        for (const auto &input : typed_vfx.persistent)
                        {
                            if (input.effect_handle != 0 && input.effect_handle <= profile_admission_plans.size())
                            {
                                const auto &persistent_rows = profile_admission_plans[input.effect_handle - 1].persistent_frame_rows;
                                const auto &plan = profile_admission_plans[input.effect_handle - 1];
                                if (std::holds_alternative<VfxProjectilePayload>(input.payload))
                                    frame_mesh_rows = Add(frame_mesh_rows, plan.persistent_mesh_rows);
                                if (std::holds_alternative<VfxEntityPayload>(input.payload))
                                {
                                    frame_fresnel_rows = Add(frame_fresnel_rows, plan.persistent_fresnel_rows);
                                    if (input.source_visual_kind == static_cast<std::uint8_t>(PersistentVfxKind::BossPhaseTransition))
                                    {
                                        frame_rows[surface_profile] = Add(frame_rows[surface_profile], persistent_rows[surface_profile]);
                                        frame_surface_persistent_rows = Add(frame_surface_persistent_rows, 1);
                                    }
                                }
                                if (std::holds_alternative<VfxCirclePayload>(input.payload) ||
                                    std::holds_alternative<VfxLinePayload>(input.payload) ||
                                    std::holds_alternative<VfxRingGapsPayload>(input.payload) ||
                                    std::holds_alternative<VfxEntityPayload>(input.payload) ||
                                    (input.source_visual_kind == static_cast<std::uint8_t>(PersistentVfxKind::WaveTimer) &&
                                     std::holds_alternative<VfxContextPayload>(input.payload)))
                                {
                                    frame_rows[ground_profile] = Add(frame_rows[ground_profile], persistent_rows[ground_profile]);
                                    if (persistent_rows[ground_profile] != 0)
                                        if (const auto *ring = std::get_if<VfxRingGapsPayload>(&input.payload);
                                            ring && !ring->uniform_gaps.Active())
                                            frame_ground_gaps = Add(frame_ground_gaps, ring->gap_angles_degrees.size());
                                }
                                frame_rows[ribbon_profile] = Add(frame_rows[ribbon_profile], persistent_rows[ribbon_profile]);
                                frame_ribbon_outputs = Add(frame_ribbon_outputs,
                                    profile_admission_plans[input.effect_handle - 1].persistent_ribbon_frame_outputs);
                                if (std::holds_alternative<VfxCirclePayload>(input.payload))
                                    frame_rows[decal_profile] = Add(frame_rows[decal_profile], persistent_rows[decal_profile]);
                                if (std::holds_alternative<VfxProjectilePayload>(input.payload))
                                    frame_rows[distortion_profile] = Add(frame_rows[distortion_profile], persistent_rows[distortion_profile]);
                                frame_rows[volume_profile] = Add(frame_rows[volume_profile], persistent_rows[volume_profile]);
                                frame_rows[screen_profile] = Add(frame_rows[screen_profile], persistent_rows[screen_profile]);
                            }
                        }
                        typed_ground_spawns.reserve(frame_rows[ground_profile]);
                        typed_mesh_spawns.reserve(frame_mesh_rows);
                        typed_fresnels.reserve(frame_fresnel_rows);
                        typed_surfaces.reserve(frame_rows[surface_profile]);
                        surface_persistent.reserve(frame_surface_persistent_rows);
                        vfx_session.decoded.entity_surface_captures.reserve(frame_surface_captures);
                        surface_capture_inputs.reserve(frame_surface_captures);
                        if (frame_ground_gaps > std::numeric_limits<std::uint32_t>::max() ||
                            frame_ribbon_outputs > std::numeric_limits<std::uint32_t>::max())
                            throw std::length_error("VFX frame pool exceeds uint32 offsets");
                        ground_gap_angles.reserve(frame_ground_gaps);
                        ribbon_inputs.Reserve(frame_rows[ribbon_profile], frame_ribbon_outputs);
                        typed_lights.reserve(frame_rows[light_profile]);
                        if (frame_distortion_gaps > std::numeric_limits<std::uint32_t>::max())
                            throw std::length_error("too many frame distortion gap angles");
                        typed_distortions.reserve(frame_rows[distortion_profile]);
                        distortion_gap_angles.reserve(frame_distortion_gaps);
                        typed_decals.reserve(frame_rows[decal_profile]);
                        typed_volumes.reserve(frame_rows[volume_profile]);
                        typed_screen.reserve(frame_rows[screen_profile]);
                        auto required_birth_bytes = Multiply(required_rows[0], sizeof(ParticleSpawnCommand));
                        required_birth_bytes = Add(required_birth_bytes,
                            Multiply(required_rows[1], sizeof(VfxLineSpawnCommand)));
                        required_birth_bytes = Add(required_birth_bytes,
                            Multiply(required_rows[2], sizeof(VfxFlashSpawnInput)));
                        const auto base_held = Add(vfx_session.LogicalBytes(),
                            Add(Multiply(Add(visible_flashes.size(), required_suffix), sizeof(VfxFlashSpawnInput)),
                                required_birth_bytes));
                        VfxEventStore::ProfileAdmission admission{
                            base_held, runtime_detail::kVfxWorkspaceSoftBudgetBytes, birth_order};
                        admission.frame_storage[ground_profile] = {
                            sizeof(VfxGroundSpawnInput), frame_rows[ground_profile]};
                        admission.ground_gap_storage = {sizeof(float), frame_ground_gaps};
                        admission.frame_storage[ribbon_profile] = {
                            sizeof(VfxRibbonSourceInput), frame_rows[ribbon_profile]};
                        admission.ribbon_output_storage = {
                            sizeof(VfxRibbonOutputInput), frame_ribbon_outputs};
                        admission.frame_storage[light_profile] = {
                            sizeof(VfxLightInput), frame_rows[light_profile]};
                        admission.frame_storage[distortion_profile] = {
                            sizeof(VfxDistortionInput), frame_rows[distortion_profile]};
                        admission.distortion_gap_storage = {
                            sizeof(float), frame_distortion_gaps};
                        admission.frame_storage[decal_profile] = {
                            sizeof(VfxDecalInput), frame_rows[decal_profile]};
                        admission.frame_storage[volume_profile] = {
                            sizeof(VfxVolumeInput), frame_rows[volume_profile]};
                        admission.frame_storage[screen_profile] = {
                            sizeof(VfxScreenInput), frame_rows[screen_profile]};
                        admission.frame_storage[surface_profile] = {
                            sizeof(VfxEntitySurfaceCommand), frame_rows[surface_profile]};
                        admission.surface_capture_storage = {sizeof(runtime_detail::RetainedVfxEntitySurfaceCapture),
                            frame_surface_captures};
                        admission.surface_capture_input_storage = {sizeof(runtime_detail::VfxEntitySurfaceEventCapture),
                            frame_surface_captures};
                        if (!profile_additions.empty() &&
                            !active_effects.PrepareAppendBatch(profile_additions, prepared_profiles, &admission))
                        {
                            record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                "VFX event profiles could not be prepared."));
                            break;
                        }
                        profile_output_bytes = Add(admission.FrameBaseBytes(), admission.admitted_bytes);
                        if (!profile_additions.empty())
                        {
                            typed_ground_spawns.reserve(admission.reserved_frame_rows[ground_profile]);
                            ground_gap_angles.reserve(admission.reserved_ground_gaps);
                            ribbon_inputs.Reserve(admission.reserved_frame_rows[ribbon_profile], admission.reserved_ribbon_outputs);
                            typed_lights.reserve(admission.reserved_frame_rows[light_profile]);
                            typed_distortions.reserve(admission.reserved_frame_rows[distortion_profile]);
                            distortion_gap_angles.reserve(admission.reserved_distortion_gaps);
                            typed_decals.reserve(admission.reserved_frame_rows[decal_profile]);
                            typed_volumes.reserve(admission.reserved_frame_rows[volume_profile]);
                            typed_screen.reserve(admission.reserved_frame_rows[screen_profile]);
                            typed_surfaces.reserve(admission.reserved_frame_rows[surface_profile]);
                            vfx_session.decoded.entity_surface_captures.reserve(admission.reserved_surface_captures);
                            surface_capture_inputs.reserve(admission.reserved_surface_captures);
                        }
#ifdef HS_DEVELOPMENT_TOOLS
                        if (capture_diagnostics)
                        {
                            vfx_profile_ground_reserved_peak_rows = std::max(vfx_profile_ground_reserved_peak_rows,
                                profile_additions.empty() ? frame_rows[ground_profile] : admission.reserved_frame_rows[ground_profile]);
                            vfx_profile_ground_reserved_peak_gaps = std::max(vfx_profile_ground_reserved_peak_gaps,
                                profile_additions.empty() ? frame_ground_gaps : admission.reserved_ground_gaps);
                            vfx_profile_ribbon_reserved_peak_rows = std::max(vfx_profile_ribbon_reserved_peak_rows,
                                profile_additions.empty() ? frame_rows[ribbon_profile] : admission.reserved_frame_rows[ribbon_profile]);
                            vfx_profile_ribbon_reserved_peak_outputs = std::max(vfx_profile_ribbon_reserved_peak_outputs,
                                profile_additions.empty() ? frame_ribbon_outputs : admission.reserved_ribbon_outputs);
                            vfx_profile_light_reserved_peak_rows = std::max(vfx_profile_light_reserved_peak_rows,
                                profile_additions.empty() ? frame_rows[light_profile] : admission.reserved_frame_rows[light_profile]);
                            vfx_profile_distortion_reserved_peak_rows = std::max(vfx_profile_distortion_reserved_peak_rows,
                                profile_additions.empty() ? frame_rows[distortion_profile] : admission.reserved_frame_rows[distortion_profile]);
                            vfx_profile_distortion_reserved_peak_gaps = std::max(vfx_profile_distortion_reserved_peak_gaps,
                                profile_additions.empty() ? frame_distortion_gaps : admission.reserved_distortion_gaps);
                            vfx_profile_decal_reserved_peak_rows = std::max(vfx_profile_decal_reserved_peak_rows,
                                profile_additions.empty() ? frame_rows[decal_profile] : admission.reserved_frame_rows[decal_profile]);
                            vfx_profile_volume_reserved_peak_rows = std::max(vfx_profile_volume_reserved_peak_rows,
                                profile_additions.empty() ? frame_rows[volume_profile] : admission.reserved_frame_rows[volume_profile]);
                            vfx_profile_screen_reserved_peak_rows = std::max(vfx_profile_screen_reserved_peak_rows,
                                profile_additions.empty() ? frame_rows[screen_profile] : admission.reserved_frame_rows[screen_profile]);
                            vfx_profile_requested_memberships = Add(vfx_profile_requested_memberships,
                                admission.requested_cosmetic_memberships);
                            vfx_profile_admitted_memberships = Add(vfx_profile_admitted_memberships,
                                admission.admitted_cosmetic_memberships);
                            vfx_profile_budget_limited_frames += admission.admitted_cosmetic_memberships <
                                admission.requested_cosmetic_memberships;
                        }
#endif
                        vfx_session.ReserveSurfaceCaptures(profile_additions.empty() ? 0 :
                            prepared_profiles.AdditionalProfileRows(VfxEventProfile::EntitySurface));
                    }
                    select_birth_rows(required_rows, birth_requests);

                    runtime_detail::AppendVfxPreparedFlashOutputs(prepared_flash_outputs,
                        prepared_births.flashes, &superseded_pickup_sequences);
                    replaced_particle_sequences.insert(replaced_particle_sequences.end(),
                        owned_status_sequences.begin(), owned_status_sequences.end());
                    // A deliberate cosmetic drop must not resurrect the same legacy decoration.
                    for (const auto &plan : prepared_flash_outputs)
                        if (plan.pickup_collect) replaced_particle_sequences.push_back(plan.pickup_sequence);
                    std::ranges::sort(replaced_particle_sequences);
                    vfx_row_indices.clear();
                    runtime_detail::AppendVfxPreparedBallisticBursts(prepared_ballistic_bursts,
                        prepared_births.particle_spawns, &vfx_row_indices);
                    const auto &lookup = vfx_catalog.Program().effect_lookup;
                    const auto replace_mesh = [&](std::size_t index) {
                        const auto &input = typed_vfx.events[index];
                        replaced_legacy_mesh_sequences.push_back(input.sequence);
                        if (input.effect_handle && input.effect_handle <= lookup.size() &&
                            lookup[input.effect_handle - 1].effect_id == explosive_arrow_main)
                            replaced_burst_shell_sequences.push_back(input.sequence);
                    };
                    for (const auto index : vfx_row_indices) replace_mesh(index);
                    for (std::size_t first = 0; first < prepared_ballistic_bursts.size();)
                    {
                        const auto index = prepared_ballistic_bursts[first].event_index;
                        bool eligible{}, admitted{};
                        do {
                            const auto &plan = prepared_ballistic_bursts[first++];
                            eligible |= plan.count != 0; admitted |= plan.admitted_count != 0;
                        } while (first < prepared_ballistic_bursts.size() &&
                            prepared_ballistic_bursts[first].event_index == index);
                        if (eligible && !admitted) replace_mesh(index);
                    }
                    std::ranges::sort(replaced_legacy_mesh_sequences);
                    std::ranges::sort(replaced_burst_shell_sequences);
                    ballistic_end = prepared_births.particle_spawns.size();
                    runtime_detail::AppendVfxPreparedMoteBursts(prepared_status_mote_bursts, prepared_births.flashes);
                    runtime_detail::AppendVfxPreparedMoteBursts(prepared_mote_bursts, prepared_births.flashes);
#ifdef HS_DEVELOPMENT_TOOLS
                    if (capture_diagnostics)
                        mote_birth_budget_limited = std::ranges::any_of(prepared_mote_bursts, [](const auto &burst) {
                            return burst.cosmetic && burst.admitted_count < burst.count;
                        });
#endif

                    // Compatibility output uses the remaining budget after actual typed results.
                    birth_requests.clear();
                    required_rows = {Add(prepared_births.particle_spawns.size(), status_shard_commands.size()),
                        prepared_births.effect_lines.size(), prepared_births.flashes.size()};
                    for (auto &plan : prepared_legacy_events)
                    {
                        if (auto replaced = vfx_catalog.SetPreparedReplacements(plan, prepared_legacy_emitters,
                            std::ranges::binary_search(replaced_particle_sequences, plan.event->sequence),
                            std::ranges::binary_search(replaced_legacy_mesh_sequences, plan.event->sequence)); !replaced)
                        {
                            record_thread_failure(replaced);
                            break;
                        }
                        const auto storage = plan.definition->kind == VfxDefinitionKind::Line ? 1 : 0;
                        required_rows[storage] = Add(required_rows[storage], plan.required_count);
                        if (plan.count) birth_requests.push_back({plan.count, &plan.admitted_count, plan.importance, 0});
                    }
                    if (channels.stop_requested.load(std::memory_order_acquire)) break;
                    select_birth_rows(required_rows, birth_requests);
                    for (const auto &plan : prepared_legacy_events)
                    {
                        if (auto expanded = vfx_catalog.AppendPreparedEvent(plan, prepared_legacy_emitters,
                                prepared_births.particle_spawns, prepared_births.effect_lines); !expanded)
                        {
                            record_thread_failure(expanded);
                            break;
                        }
                    }
                    birth_requests.clear();
                    prepared_flash_outputs.clear();
                    prepared_ballistic_bursts.clear();
                    prepared_status_mote_bursts.clear();
                    prepared_mote_bursts.clear();
                    prepared_legacy_events.clear();
                    prepared_legacy_emitters.clear();
                }
                if (channels.stop_requested.load(std::memory_order_acquire)) break;
                // Keep legacy and showcase births before typed ballistic births
                // while leaving the already-retained particle prefix untouched.
                if (ballistic_first != ballistic_end && ballistic_end != prepared_births.particle_spawns.size())
                    std::rotate(prepared_births.particle_spawns.begin() + ballistic_first,
                        prepared_births.particle_spawns.begin() + ballistic_end,
                        prepared_births.particle_spawns.end());
                if (!vfx_session.CommitPreparedActivation(
                        prepared_births, prepared_profiles, !profile_additions.empty()))
                {
                    record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                        "Prepared VFX activation changed before publication."));
                    break;
                }
                if (append_vfx_showcase) vfx_showcase_injected = true;
#ifdef HS_DEVELOPMENT_TOOLS
                if (capture_diagnostics) vfx_birth_budget_limited_frames += birth_budget_limited;
#endif

                const Tick surface_tick = snapshots.current.header.tick;
                const auto surface_instance = [&](std::uint64_t id) -> const RenderInstance * {
                    if (const auto *instance = vfx_pair_facts.Instance(id, snapshots.current.instances))
                        return instance;
                    if (sample_alpha < 1.0f && snapshots.has_previous)
                    {
                        const auto found = std::find_if(snapshots.previous.instances.begin(), snapshots.previous.instances.end(),
                            [id](const auto &instance) { return instance.stable_id == id; });
                        if (found != snapshots.previous.instances.end()) return &*found;
                    }
                    return nullptr;
                };
                const auto terminal_phase = static_cast<SessionPhase>(
                    channels.session_phase.load(std::memory_order_acquire));
                const auto terminal_tick = vfx_session.UpdateTerminalClock(
                    surface_tick, terminal_phase, sample_time);
                const double death_current_tick = terminal_phase == SessionPhase::Victory || terminal_phase == SessionPhase::Defeat
                    ? static_cast<double>(terminal_tick) : display_tick;
                std::erase_if(vfx_session.decoded.entity_surface_captures, [&](const auto &capture) {
                    const auto *input = active_effects.Get(capture.row);
                    if (!input || input->effect_handle == 0 || input->effect_handle > vfx_catalog.Image().entity_surface_effect_recipes.size())
                        return true;
                    const auto &effect = vfx_catalog.Image().entity_surface_effect_recipes[input->effect_handle - 1];
                    const double current_tick = is_effect(*input, death_effect)
                        ? death_current_tick : display_tick;
                    return current_tick >= input->event_tick &&
                        static_cast<double>(current_tick - input->event_tick) >= static_cast<double>(effect.seconds) * 60.0;
                });
                active_effects.EraseIf(VfxEventProfile::EntitySurface, [&](const auto &input) {
                    const auto &effect = vfx_catalog.Image().entity_surface_effect_recipes[input.effect_handle - 1];
                    const double current_tick = is_effect(input, death_effect)
                        ? death_current_tick : display_tick;
                    if (current_tick >= input.event_tick &&
                        static_cast<double>(current_tick - input.event_tick) >= static_cast<double>(effect.seconds) * 60.0) return true;
                    const auto *context = std::get_if<VfxContextPayload>(&input.payload);
                    if (!context || context->owner_id == 0) return true;
                    runtime_detail::RetainedVfxEntitySurfaceCapture capture;
                    capture.row = active_effects.Find(input);
                    if (capture.row == std::numeric_limits<runtime_detail::ActiveEffectStore::Row>::max())
                        return false;
                    if (is_effect(input, death_effect))
                    {
                        const auto *actual = vfx_pair_facts.Capture(
                            snapshots.current.header.session_id, input.sequence,
                            input.event_tick, context->owner_id,
                            snapshots.current.captured_instances);
                        if (!actual) return false;
                        capture.instance = actual->instance;
                        capture.animation_pose = actual->animation_pose;
                        capture.retained_instance = capture.has_animation_pose = true;
                    }
                    else
                    {
                        const auto *actual = surface_instance((2ull << 60) | context->owner_id);
                        if (!actual) return false;
                        capture.instance = *actual;
                    }
                    vfx_session.decoded.entity_surface_captures.push_back(std::move(capture));
                    return true;
                });
                surface_capture_inputs.clear();
                for (const auto &capture : vfx_session.decoded.entity_surface_captures)
                    if (const auto *input = active_effects.Get(capture.row))
                        surface_capture_inputs.push_back({*input, capture.instance,
                            capture.retained_instance, capture.has_animation_pose,
                            capture.animation_pose});
                surface_persistent.clear();
                for (const auto &input : typed_vfx.persistent)
                {
                    if (input.source_visual_kind != static_cast<std::uint8_t>(PersistentVfxKind::BossPhaseTransition)) continue;
                    const auto *entity = std::get_if<VfxEntityPayload>(&input.payload);
                    if (!entity) continue;
                    const auto *actual = surface_instance(entity->render_instance_id);
                    const auto *visual = vfx_pair_facts.Current(
                        input.stable_id, PersistentVfxKind::BossPhaseTransition, current_vfx);
                    if (!visual || visual->status_episode_generation != input.status_episode_generation)
                        visual = vfx_pair_facts.Previous(
                            input.stable_id, PersistentVfxKind::BossPhaseTransition, previous_vfx);
                    if (visual && visual->status_episode_generation != input.status_episode_generation)
                        visual = nullptr;
                    if (actual && visual)
                        surface_persistent.push_back({input, *actual, visual->active_tick, visual->expires});
                }
                runtime_detail::AppendVfxTypedEntitySurfaceCommands(vfx_catalog.Image(),
                    surface_capture_inputs, surface_persistent, display_tick,
                    death_current_tick, typed_surfaces);
#ifdef HS_DEVELOPMENT_TOOLS
                if (capture_diagnostics) typed_vfx_unsupported_inputs += typed_vfx.unsupported_input_count;
#endif
                // Compatibility cancellation is a post-commit mutation: the staged
                // replacement births are already in the retained queue at this point.
                retained_particle_count = runtime_detail::vfx_birth_retirement::RetireCompatibilityBirthRows(
                    pending_particle_spawns, retained_particle_count, replaced_particle_sequences,
                    owned_status_sequences, replaced_legacy_mesh_sequences, replaced_burst_shell_sequences);
                if (!vfx_session.CommitPersistentBursts(status_burst_keys,
                        status_shard_commands, pending_particle_spawns))
                {
                    record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                        "Prepared persistent VFX births changed before publication."));
                    break;
                }
                runtime_detail::AppendVfxTypedGroundCommands(
                    vfx_catalog.Image(), typed_vfx.persistent,
                    snapshots.has_current ? snapshots.current.header.tick : Tick{}, typed_ground_spawns, ground_gap_angles);
                const auto light_tick = display_tick;
                active_effects.EraseIf(VfxEventProfile::Screen, [&](const VfxEventInput &input) {
                    const auto &effect = vfx_catalog.Image().screen_effect_recipes[input.effect_handle - 1];
                    return light_tick >= input.event_tick &&
                        static_cast<double>(light_tick - input.event_tick) / 60.0 >= effect.seconds;
                });
                if (auto converted = runtime_detail::BuildVfxTypedScreenCommands(vfx_catalog.Image(),
                        active_effects.ProfileEvents(VfxEventProfile::Screen), typed_vfx.persistent, current_snapshot.header.tick, display_tick,
                        active_vfx_quality, vfx_session.low_health_pulse, typed_screen); !converted)
                {
                    record_thread_failure(converted);
                    break;
                }
#if defined(HS_DEVELOPMENT_TOOLS)
                if (asset_test_room)
                    std::erase_if(typed_screen, [&](const VfxScreenInput &input) {
                        const auto identity = input.event_sequence ? input.event_sequence : input.owner_id;
                        const auto domain = identity >> 60;
                        if (domain != 6 && domain != 7) return false;
                        const auto station = static_cast<std::size_t>((identity >> 32) & 0xfffff);
                        return station == 0 || asset_test_room->active_screen_station != station - 1;
                    });
#endif
                active_effects.EraseIf(VfxEventProfile::Volume, [&](const VfxEventInput &input) {
                    const auto &effect = vfx_catalog.Image().volume_effect_recipes[input.effect_handle - 1];
                    return light_tick >= input.event_tick &&
                        static_cast<double>(light_tick - input.event_tick) / 60.0 >= effect.seconds;
                });
                if (auto converted = runtime_detail::BuildVfxTypedVolumeCommands(vfx_catalog.Image(),
                        active_effects.ProfileEvents(VfxEventProfile::Volume), typed_vfx.persistent, light_tick,
                        active_vfx_quality, typed_volumes); !converted)
                {
                    record_thread_failure(converted);
                    break;
                }
                const auto owner_event_expired = [&](const VfxEventInput &input) {
                    if (input.geometry_owner_id != 0) return light_tick >= input.geometry_end_tick;
                    const auto &effect = vfx_catalog.Program().effects[input.effect_handle-1];
                    return effect.timing_kind != 0 || !std::isfinite(effect.seconds) || effect.seconds <= 0 ||
                        (light_tick >= input.event_tick && static_cast<double>(light_tick-input.event_tick)/60.0 >= effect.seconds);
                };
                active_effects.RefreshProfile(VfxEventProfile::Light, refresh_owner_event, owner_event_expired);
                runtime_detail::AppendVfxTypedLightCommands(vfx_catalog.Image(),
                    active_effects.ProfileEvents(VfxEventProfile::Light), light_tick, typed_lights);
                active_effects.RefreshProfile(VfxEventProfile::Distortion, refresh_owner_event, owner_event_expired);
                runtime_detail::RebuildVfxTypedDistortionCommands(vfx_catalog.Image(),
                    active_effects.ProfileEvents(VfxEventProfile::Distortion),
                    typed_vfx.persistent, light_tick, typed_distortions, distortion_gap_angles);
                active_effects.RefreshProfile(VfxEventProfile::Decal, refresh_owner_event, owner_event_expired);
                runtime_detail::AppendVfxTypedDecalCommands(vfx_catalog.Image(),
                    active_effects.ProfileEvents(VfxEventProfile::Decal),
                    typed_vfx.persistent, light_tick, typed_decals);
                // Keep this after CommitPersistentBursts: accepted all-particle ground
                // facts also retire newly committed status-shard rows for that sequence.
                retained_particle_count = runtime_detail::vfx_birth_retirement::RetireGroundSuppressedBirthRows(
                    pending_particle_spawns, retained_particle_count, ground_birth_suppressions);
                const Tick ground_tick = snapshots.current.header.tick;
                active_effects.RefreshProfile(VfxEventProfile::Ground, refresh_owner_event, [&](const VfxEventInput &input) {
                    if (input.geometry_owner_id != 0)
                        return display_tick >= input.geometry_end_tick;
                    const auto seconds = vfx_catalog.Program().effects[input.effect_handle - 1].seconds;
                    return display_tick >= input.event_tick &&
                        (display_tick - input.event_tick) / 60.0 >= seconds;
                });
                runtime_detail::AppendVfxTypedEventGroundCommands(
                    vfx_catalog.Image(), active_effects.ProfileEvents(VfxEventProfile::Ground),
                    display_tick, typed_ground_spawns, ground_gap_angles);
                if (inject_pickup_showcase && std::ranges::count_if(superseded_pickup_sequences,
                        [](Sequence sequence) { return sequence >= 0xF100u && sequence < 0xF104u; }) != 4)
                {
                    record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                        "Pickup showcase did not convert all four collection bursts."));
                    break;
                }
                const auto required_flash_suffix = runtime_detail::vfx_retained_size::Add(
                    status_stamp_flashes.size(), required_fire);
                [[maybe_unused]] const auto mote_budget_limited = runtime_detail::PrepareVisibleMoteOutputs(
                    vfx_catalog.Image(), vfx_session, persistent_sparks, typed_vfx.persistent,
                    display_tick, ground_tick, sample_alpha, retained_flash_count,
                    required_flash_suffix, visible_flashes, mote_birth_budget_limited,
                    vfx_visible_held_bytes);
#ifdef HS_DEVELOPMENT_TOOLS
                if (capture_diagnostics)
                    vfx_mote_budget_limited_frames += mote_budget_limited;
#endif
                runtime_detail::AppendVfxTypedMeshCommands(
                    vfx_catalog.Image(), vfx_catalog.MeshRecipeView(),
                    typed_vfx.persistent, typed_mesh_spawns);
                runtime_detail::AppendVfxTypedFresnelCommands(
                    vfx_catalog.Image(), vfx_catalog.FresnelRecipeView(),
                    typed_vfx.persistent, typed_fresnels);
                active_effects.EraseIf(VfxEventProfile::Ribbon, [&](const VfxEventInput &input) {
                    const auto seconds = vfx_catalog.Image().program.effects[input.effect_handle - 1].seconds;
                    return display_tick >= input.event_tick &&
                        (display_tick - input.event_tick) / 60.0 >= seconds;
                });
                runtime_detail::AppendVfxTypedRibbonInputs(
                    vfx_catalog.Image(), typed_vfx.persistent, ribbon_inputs);
                runtime_detail::AppendVfxEventRibbonInputs(
                    vfx_catalog.Image(), active_effects.ProfileEvents(VfxEventProfile::Ribbon),
                    display_tick, ribbon_inputs);
                const VfxRibbonFrameInput typed_ribbons{
                    ribbon_inputs.Sources(), ribbon_inputs.Outputs()};
                // Authored source delays can cross later event deliveries. Keep the ready prefix chronological.
                vfx_session.decoded.SortNewParticleSpawns(retained_particle_count);
                const Tick stateful_target = current_snapshot.header.tick -
                    (snapshots.has_previous && sample_alpha < 1.0f && current_snapshot.header.tick != 0 ? 1u : 0u);
                const bool rewound = (vfx_session.has_committed_stateful_target &&
                    !vfx_session.particle_reset_pending && stateful_target < vfx_session.last_committed_stateful_target) ||
                    (vfx_session.native_batch_active && stateful_target < vfx_session.native_batch.identity.stateful_target);
                const bool reset_particles = rewound || vfx_session.particle_reset_pending ||
                    !vfx_session.has_committed_stateful_target;
                const bool new_world_step = reset_particles ||
                    stateful_target != vfx_session.last_committed_stateful_target;
                snapshot_particle_births.clear();
                if ((!vfx_session.native_batch_active || rewound) && new_world_step)
                {
                    auto birth_alpha = sample_alpha;
                    if (snapshots.has_previous && birth_alpha < 1.0f)
                    {
                        const auto previous_time = snapshots.previous.header.simulation_time;
                        const auto duration = current_snapshot.header.simulation_time - previous_time;
                        const auto display_time = previous_time + std::chrono::nanoseconds(
                            static_cast<std::int64_t>(static_cast<double>(duration.count()) * birth_alpha));
                        birth_alpha = static_cast<float>(static_cast<double>((display_time - previous_time).count()) /
                            static_cast<double>(duration.count()));
                    }
                    runtime_detail::AppendPersistentSnapshotParticleBirths(snapshots,
                        RenderVfxFrameInput{.ground = typed_ground_spawns, .ribbons = typed_ribbons},
                        stateful_target, vfx_session.last_committed_stateful_target, reset_particles,
                        birth_alpha, vfx_catalog.Image().sprite_bindings,
                        config.slime_family_preview_count != 0, vfx_pair_facts,
                        snapshot_particle_scratch, snapshot_particle_births);
                }
                auto *native_batch = vfx_session.PrepareNativeBatch(stateful_target,
                    !vfx_session.has_committed_stateful_target ||
                    stateful_target != vfx_session.last_committed_stateful_target, &prepared_births,
                    snapshot_particle_births);
                const RenderVfxWorldBatch native_world = native_batch ? native_batch->identity :
                    RenderVfxWorldBatch{.session = current_snapshot.header.session_id,
                        .stateful_target = stateful_target};
                const auto native_particles = native_batch
                    ? std::span<const ParticleSpawnCommand>(native_batch->particle_spawns).subspan(
                        config.slime_family_preview_count ? native_batch->decoded_particle_count : 0)
                    : std::span<const ParticleSpawnCommand>{};
                const auto native_lines = native_batch
                    ? std::span<const VfxLineSpawnCommand>(native_batch->effect_lines)
                    : std::span<const VfxLineSpawnCommand>{};
                {
                    const auto fire_admission = runtime_detail::AppendVisibleFireAndStamps(
                        prepared_fire_sources, decorative_fire, required_flash_suffix,
                        status_stamp_flashes, visible_flashes,
                        runtime_detail::kVfxWorkspaceSoftBudgetBytes, vfx_visible_held_bytes);
                    fire_admitted = fire_admission.emitted_rows;
                    fire_admission_bytes = fire_admission.admission_bytes;
                    fire_budget_limited = fire_admission.budget_limited;
#ifdef HS_DEVELOPMENT_TOOLS
                    if (capture_diagnostics)
                    {
                        vfx_fire_admission_peak_bytes = std::max(vfx_fire_admission_peak_bytes, fire_admission_bytes);
                        vfx_fire_budget_limited_frames += fire_budget_limited;
                    }
#endif
                }
                active_effects.Compact(vfx_session.decoded.entity_surface_captures);
                const auto frame_start = std::chrono::steady_clock::now();
#ifdef HS_DEVELOPMENT_TOOLS
                const auto render_start_qpc = performance_lines ? read_performance_qpc() : 0;
#endif
#ifdef HS_DEVELOPMENT_TOOLS
                std::uint64_t render_cpu_begin{};
                const bool render_cpu_available = performance_lines && ReadRenderThreadCpuTicks(render_cpu_begin);
#endif
                auto package_snapshots = snapshots;
                auto camera = InterpolateRenderCamera(snapshots, sample_alpha);
#if defined(HS_DEVELOPMENT_TOOLS)
                runtime_detail::PreviewScene preview_scene;
                if (!asset_test_room) preview_scene.Prepare(config, preview_edit, package_snapshots);
                camera = runtime_detail::PreviewScene::Camera(config, preview_edit, camera);
                if (asset_test_room && config.test_room_capture)
                {
                    const auto &subject = asset_test_room->entries[*config.test_room_capture - 1];
                    camera.target = subject.position;
                    camera.target.y += runtime_detail::PreviewScene::TargetHeight(subject.mesh);
                    if (!config.preview_camera_override)
                    {
                        camera.distance = subject.capture_distance;
                        camera.yaw_degrees = 180.0f; camera.pitch_degrees = 25.0f;
                    }
                }
#endif
                const auto *displayed_ui =
#if defined(HS_DEVELOPMENT_TOOLS)
                    asset_test_room ? nullptr :
#endif
                    snapshots.ui_frame.get();
                RenderPackage render_input{
                    .camera = camera,
                    .snapshots = package_snapshots,
                    .vfx = {
                        .particle_spawns = native_particles,
                        .effect_lines = native_lines,
                        .ground = typed_ground_spawns,
                        .flashes = visible_flashes,
                        .meshes = typed_mesh_spawns,
                        .fresnels = typed_fresnels,
                        .ribbons = typed_ribbons,
                        .lights = typed_lights,
                        .distortions = typed_distortions,
                        .decals = typed_decals,
                        .volumes = typed_volumes,
                        .screen = typed_screen,
                        .entity_surfaces = typed_surfaces,
                        .world_batch = native_world,
                        .distortion_gap_angles = distortion_gap_angles,
                        .ground_gap_angles = ground_gap_angles,
                    },
                    .devtools = devtools,
                    .ui_draw_content = displayed_ui && displayed_ui->draw_content
                        ? displayed_ui->draw_content : EmptyUiDrawContent(),
                    .ui_packet_generation = displayed_ui ? displayed_ui->generation : 0,
                    .display_sample = RenderDisplaySample{sample_time, sample_alpha},
                };
#if defined(HS_DEVELOPMENT_TOOLS)
                if (config.slime_family_preview_count)
                {
                    render_input.vfx.ground = {};
                    render_input.vfx.meshes = {};
                    render_input.vfx.ribbons = {};
                    render_input.vfx.lights = {};
                    render_input.vfx.distortions = {};
                    render_input.vfx.decals = {};
                }
                std::vector<VfxEntitySurfaceCommand> preview_surfaces;
                if (config.slime_family_preview_count || config.monster_preview_asset < 6 || config.environment_preview)
                {
                    for (const auto &surface : typed_surfaces)
                        if (std::ranges::any_of(package_snapshots.current.instances, [&](const auto &instance) {
                            return instance.stable_id == surface.render_instance_id;
                        })) preview_surfaces.push_back(surface);
                    render_input.vfx.entity_surfaces = preview_surfaces;
                }
#endif
                // GPU completion is delayed; offline attribution joins this revision through its own frame ID.
                // Simulation/input can enter Pause or a menu after this loop's
                // input boundary. Disable before that first non-gameplay Present.
                if (!AllowGameplayFrameGeneration(static_cast<SessionPhase>(channels.session_phase.load(std::memory_order_acquire))))
                    if (auto policy = set_frame_generation_allowed(false); !policy)
                    { record_thread_failure(policy); break; }
#ifdef HS_DEVELOPMENT_TOOLS
                const auto frame_config_revision = graphics_config_revision;
                const auto frame_settings_generation = active_settings_generation;
#endif
                const auto rendered = renderer.Render(render_input, frame);
#if defined(HS_DEVELOPMENT_TOOLS)
                preview_edit = frame.preview;
#endif
                const auto receipts = runtime_detail::AcceptRendererReceipts(frame,
                    channels.ui_packets, snapshots.ui_frame,
                    snapshots.current.header.session_id, vfx_session);
#ifdef HS_DEVELOPMENT_TOOLS
                if (receipts.ui_receipt_rejected)
                    OutputDebugStringA("Presented UI receipt did not match the retained packet; prior accepted frame retained.\n");
#endif
                if (!rendered)
                {
                    record_thread_failure(rendered);
                    break;
                }
#if defined(HS_DEVELOPMENT_TOOLS)
                if (room_capture)
                {
                    room_capture = false;
                    if (auto captured_room = renderer.Capture({.path = artifact_directory /
                            std::format("test-room-{}-{:05}.png", room_capture_session, ++room_capture_index)}); !captured_room)
                    { record_thread_failure(captured_room); break; }
                }
#endif
                if (native_world.sequence != 0 && !receipts.native_batch_accepted)
                {
                    record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                        "Renderer completed VFX inputs without acknowledging the matching world step."));
                    break;
                }
#if defined(HS_DEVELOPMENT_TOOLS)
                // The gallery intentionally presents no gameplay UI and uses its
                // own display clock/session. Acknowledge the real simulation
                // snapshot after its successful Present so its session barrier
                // can advance without fabricating a UI packet receipt.
                if (asset_test_room)
                {
                    if (frame.identity.presentation_id != 0)
                        native_input_frame.PresentedSession(source_snapshot_session, source_snapshot_tick);
                }
                else
#endif
                if (frame.presented_ui_packet_generation)
                    native_input_frame.PresentedSession(snapshots.current.header.session_id, frame.rendered_tick);
#ifdef HS_DEVELOPMENT_TOOLS
                if (performance_lines)
                {
                    const auto render_end_qpc = read_performance_qpc();
                    std::uint64_t render_cpu_end{};
                    const bool render_thread_cpu_valid = render_cpu_available &&
                        ReadRenderThreadCpuTicks(render_cpu_end) && render_cpu_end >= render_cpu_begin;
                    const double render_thread_cpu_us = render_thread_cpu_valid
                        ? static_cast<double>(render_cpu_end - render_cpu_begin) * 0.1 : 0.0;
                    const auto qpc_microseconds = performance_frequency
                        ? 1'000'000.0 / static_cast<double>(performance_frequency) : 0.0;
                    const auto &cpu = frame.cpu_timings;
                    const auto &gpu = renderer.LastCompletedGpuPassTimings();
                    const auto &diagnostic = renderer.LastRenderedFrameDiagnostics();
                    const auto &memory = diagnostic.memory;
                    const auto &queues = diagnostic.queues;
                    const auto capture_log_begin = read_performance_qpc();
                    performance_row.clear();
#define HS_FRAME_PERFORMANCE_FIELD(name, expression) \
                    runtime_detail::AppendCsvValue(performance_row, expression);
#include "frame_performance_fields.inc"
#undef HS_FRAME_PERFORMANCE_FIELD
                    performance_row += '\n';
                    performance_lines->Append(performance_row);
                    // The sampler waits for nonempty performance.csv before opening GPU counters.
                    if (!performance_ready_published && (!config.capture_gpu_timings || (gpu.valid && gpu.fence_completed)))
                    {
                        performance_ready_published = true;
                        if (!performance_lines->Flush())
                        {
                            record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                "Could not publish GPU-ready performance capture."));
                            break;
                        }
                    }
                    if (auto collected = collect_gpu_capture(false); !collected)
                    {
                        record_thread_failure(collected);
                        break;
                    }
                    const auto capture_log_end = read_performance_qpc();
                    previous_capture_log_ticks = runtime_detail::PerformanceLogState::ElapsedTicks(
                        capture_log_begin, capture_log_end);
                }
#endif
                if (config.smoke_render_lockstep)
                {
                    rendered_tick.store(
#ifdef HS_DEVELOPMENT_TOOLS
                        config.test_room_capture ? source_snapshot_tick :
#endif
                        snapshots.current.header.tick, std::memory_order_release);
#ifdef HS_DEVELOPMENT_TOOLS
                    notify_smoke();
#endif
                }
                const auto receipt_phase = channels.session_phase.load(std::memory_order_acquire);
                if (execution_receipt_pending || receipt_phase != previous_receipt_phase)
                {
                    publish_graphics_receipt();
                    execution_receipt_pending = false;
                    previous_receipt_phase = receipt_phase;
                }
                const auto settings_page = static_cast<UiPage>(channels.ui_page.load(std::memory_order_acquire));
                const auto settings_phase = static_cast<SessionPhase>(receipt_phase);
                const bool settings_open = (settings_phase == SessionPhase::MainMenu && settings_page == UiPage::MainMenuSettings) ||
                                           (settings_phase == SessionPhase::Paused && settings_page == UiPage::PauseSettings);
                if (settings_open && !settings_was_open) publish_graphics_receipt();
                settings_was_open = settings_open;
#ifdef HS_DEVELOPMENT_TOOLS
                if (settings_open && config.capture_diagnostics)
                {
                    const auto diagnostics = renderer.GetDiagnostics();
                    const auto receipt = channels.renderer_graphics_ui.load(std::memory_order_acquire);
                    if (receipt && diagnostics.newest_presented_frame_id > telemetry_completed_frame)
                    {
                        auto updated = std::make_shared<GraphicsSettingsUiModel>(*receipt);
                        UpdateGraphicsTelemetry(*updated, diagnostics, validation_enabled);
                        channels.renderer_graphics_ui.store(std::move(updated), std::memory_order_release);
                        channels.NotifyMain();
                        telemetry_completed_frame = diagnostics.newest_presented_frame_id;
                    }
                }
#endif
#ifdef HS_DEVELOPMENT_TOOLS
                if (capture_diagnostics)
                {
                    typed_vfx_ground_peak = std::max(
                        typed_vfx_ground_peak, frame.typed_vfx_ground_count);
                    typed_vfx_ground_submitted_total += frame.typed_vfx_ground_count;
                    typed_vfx_flash_peak = std::max(
                        typed_vfx_flash_peak, frame.typed_vfx_flash_count);
                    typed_vfx_flash_submitted_total += frame.typed_vfx_flash_count;
                    typed_vfx_mesh_peak = std::max(typed_vfx_mesh_peak, frame.typed_vfx_mesh_count);
                    typed_vfx_fresnel_peak = std::max(typed_vfx_fresnel_peak, frame.typed_vfx_fresnel_count);
                    typed_vfx_ribbon_peak = std::max(typed_vfx_ribbon_peak, frame.typed_vfx_ribbon_count);
                    typed_vfx_ribbon_dropped += frame.typed_vfx_ribbon_dropped;
                    typed_vfx_light_peak = std::max(typed_vfx_light_peak,frame.typed_vfx_light_count);
                    typed_vfx_distortion_peak = std::max(typed_vfx_distortion_peak,frame.typed_vfx_distortion_count);
                    typed_vfx_decal_peak = std::max(typed_vfx_decal_peak,frame.typed_vfx_decal_count);
                    typed_vfx_volume_peak = std::max(typed_vfx_volume_peak,frame.typed_vfx_volume_count);
                }
#endif
                static_scene_upload_count = frame.static_scene_upload_count;
                channels.rendered_frames.store(frame.frame, std::memory_order_release);
                channels.devtools_capture_mouse.store(frame.capture_mouse,
                                                       std::memory_order_release);
                channels.devtools_capture_keyboard.store(frame.capture_keyboard,
                                                          std::memory_order_release);
#ifdef HS_DEVELOPMENT_TOOLS
                if (frame.debug_command != 0)
                {
                    (void)channels.debug_commands.TryPush(
                        {static_cast<DebugCommandKind>(frame.debug_command - 1),
                         frame.debug_value, frame.debug_secondary, {}});
                }
#endif
#if defined(HS_DEVELOPMENT_TOOLS)
                last_tick = config.test_room_capture ? source_snapshot_tick : frame.rendered_tick;
#else
                last_tick = frame.rendered_tick;
#endif
#ifdef HS_DEVELOPMENT_TOOLS
                if (capture_diagnostics)
                {
                    gpu_live_row.clear();
                    runtime_detail::AppendCsvValues(gpu_live_row, frame.frame, last_tick,
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - frame_start).count(),
                        CurrentProcessWorkingSetBytes(),
                        snapshots.current.instances.size() + (snapshots.current.static_scene
                            ? snapshots.current.static_scene->instances.size() : 0),
                        devtools.input_queue_depth + devtools.presentation_queue_depth + devtools.particle_queue_depth +
                            devtools.resize_queue_depth + devtools.debug_queue_depth);
                    gpu_live_row += '\n';
                    timeline_lines.Append(gpu_live_row);
                }
#endif
                if (active_frame_cap != 0 && realtime && !renderer.UsesNativeFrameLimiter())
                {
                    next_frame += std::chrono::nanoseconds(
                        1'000'000'000ull / std::clamp(active_frame_cap, 1u, 1000u));
                    std::this_thread::sleep_until(next_frame);
                    if (std::chrono::steady_clock::now() > next_frame +
                                                                   std::chrono::milliseconds(100))
                    {
                        next_frame = std::chrono::steady_clock::now();
                    }
                }

            }
            else
            {
#ifdef HS_DEVELOPMENT_TOOLS
                if (config.smoke_render_lockstep)
                {
                    std::unique_lock lock(smoke_mutex);
                    smoke_cv.wait(lock, [&] {
                        return channels.completed_tick.load(std::memory_order_acquire) >
                                   rendered_tick.load(std::memory_order_acquire) ||
                            channels.stop_requested.load(std::memory_order_acquire) ||
                            channels.simulation_done.load(std::memory_order_acquire);
                    });
                }
                else
#endif
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

#ifdef HS_DEVELOPMENT_TOOLS
            // Completion can arrive after the terminal Render call. Capture
            // that frame without rendering the same snapshot a second time.
            if (config.smoke && snapshots.has_current && !smoke_finished &&
                channels.simulation_done.load(std::memory_order_acquire) &&
                last_tick >= channels.completed_tick.load(std::memory_order_acquire))
            {
                if (config.capture_smoke_frame)
                {
                    auto capture_name = config.smoke_capture_name;
                    if (config.test_room_capture)
                        capture_name = std::format("test-room-station-{}-time-{:.3f}.png", *config.test_room_capture, config.test_room_time);
                    else if (capture_name.empty())
                        capture_name = std::format("capture_tick_{}.png", last_tick);
                    if (auto capture = renderer.Capture({.path = artifact_directory / capture_name});
                        !capture)
                    {
                        record_thread_failure(capture);
                        break;
                    }
                    if (config.test_room_capture)
                    {
                        std::ofstream receipt(artifact_directory / "test-room-capture.json", std::ios::binary | std::ios::trunc);
                        receipt << std::format(
                            "{{\"scene\":\"asset-test-room\",\"station\":{},\"requested_time_seconds\":{},\"effective_time_seconds\":{},\"generation\":\"{}\",\"image\":\"{}\"}}\n",
                            *config.test_room_capture, config.test_room_time, room_time, deployment_id, capture_name);
                        receipt.close();
                        if (!receipt)
                        { record_thread_failure(Result::Failure(ErrorCode::TaskFailed, "hs_runtime", "Cannot write test room capture receipt.")); break; }
                    }
                }
                channels.rendered_particles = renderer.LastParticleCount();
                smoke_finished = true;
            }

            if (config.smoke && smoke_finished)
            {
                request_stop();
            }
#endif
            if (channels.stop_requested.load(std::memory_order_acquire) &&
                channels.simulation_done.load(std::memory_order_acquire) &&
                channels.presentation_events.Size() == 0)
            {
                break;
            }
        }

        native_input_frame.Cancel();
#ifdef HS_DEVELOPMENT_TOOLS
        const auto enhanced = renderer.UsesEnhancedBarriers();
        // Shutdown releases query readbacks; retain the last in-flight frames first.
        if (auto drained = drain_gpu_capture(); !drained) record_thread_failure(drained);
#if defined(HS_DEVELOPMENT_TOOLS)
        RendererTranslucencyDiagnosticsStatus hero_status;
        if (config.capture_translucency_diagnostics) hero_status = renderer.TranslucencyDiagnosticsStatus();
#endif
        RendererLightingStatus::ShadowCacheStatus shadow_cache;
        if (capture_diagnostics) shadow_cache = renderer.GetLightingStatus().shadow_cache;
#endif
        if (auto shutdown = renderer.Shutdown(); !shutdown)
        {
            record_thread_failure(shutdown);
        }
#ifdef HS_DEVELOPMENT_TOOLS
        if (config.capture_diagnostics)
        {
            const auto cache_stats = renderer.GetPipelineCacheStats();
            WriteText(artifact_directory / "pipeline_cache.json", std::format(
                "{{\"hits\":{},\"misses\":{},\"native_creates\":{},\"stores\":{},\"store_failures\":{},"
                "\"load_failures\":{},\"io_failures\":{},\"discarded_files\":{},\"unsupported_libraries\":{},"
                "\"uncached_mesh_creates\":{},\"root_registrations\":{},\"loaded_bytes\":{},\"serialized_bytes\":{},"
                "\"last_cache_hresult\":{},\"last_io_error\":{},"
                "\"key_build_ns\":{},\"key_build_max_ns\":{},\"library_load_ns\":{},\"library_load_max_ns\":{},"
                "\"native_create_ns\":{},\"native_create_max_ns\":{},\"library_store_ns\":{},\"library_store_max_ns\":{},"
                "\"serialize_ns\":{},\"cache_write_ns\":{},\"mutex_wait_ns\":{},\"mutex_wait_max_ns\":{},"
                "\"capture_timings\":{}}}\n",
                cache_stats.hits, cache_stats.misses, cache_stats.native_creates, cache_stats.stores,
                cache_stats.store_failures, cache_stats.load_failures, cache_stats.io_failures, cache_stats.discarded_files,
                cache_stats.unsupported_libraries, cache_stats.uncached_mesh_creates, cache_stats.root_registrations,
                cache_stats.loaded_bytes, cache_stats.serialized_bytes, cache_stats.last_cache_hresult, cache_stats.last_io_error,
                cache_stats.key_build_ns, cache_stats.key_build_max_ns, cache_stats.library_load_ns, cache_stats.library_load_max_ns,
                cache_stats.native_create_ns, cache_stats.native_create_max_ns, cache_stats.library_store_ns, cache_stats.library_store_max_ns,
                cache_stats.serialize_ns, cache_stats.cache_write_ns, cache_stats.mutex_wait_ns, cache_stats.mutex_wait_max_ns, cache_stats.capture_timings));
        }
        const auto validation_errors = renderer.ValidationErrorCount();
        const auto validation_status = renderer.ValidationStatus();
        if (!validation_status.MessageCollectionValid())
        {
            record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "D3D12 validation message collection was incomplete."));
        }
        else if (validation_errors != 0)
        {
            record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                        std::format("{} validation errors.", validation_errors)));
        }

        if (capture_diagnostics)
        {
            const bool events_written = event_lines.Finish();
            const bool timeline_written = timeline_lines.Finish();
            const bool performance_written = !performance_lines || performance_lines->Finish();
            const bool gpu_live_written = !gpu_live_lines || gpu_live_lines->Finish();
            const bool graphics_settings_written = !graphics_settings_lines || graphics_settings_lines->Finish();
            bool hero_written = true;
#if defined(HS_DEVELOPMENT_TOOLS)
            if (hero_lines)
            {
                try { hero_written = hero_lines->Finish(hero_status, hero_drain_succeeded); }
                catch (const std::exception &) { hero_written = false; }
            }
#endif
            event_file.close(); timeline_file.close();
            if (performance_lines) performance_file.close();
            if (gpu_live_lines) gpu_live_file.close();
            if (graphics_settings_lines) graphics_settings_file.close();
            const bool io_ok = events_written && timeline_written &&
                performance_written && gpu_live_written && graphics_settings_written && hero_written &&
                !event_file.fail() && !timeline_file.fail() &&
                !performance_file.fail() && !gpu_live_file.fail() &&
                (!graphics_settings_lines || !graphics_settings_file.fail());
            const auto expected_gpu_frames = gpu_live_lines ? performance_lines->Records() : 0;
            const bool gpu_frame_coverage_complete = !gpu_live_lines ||
                performance_state.GpuFrameCoverageComplete(expected_gpu_frames);
            const bool complete = io_ok && gpu_drain_succeeded && gpu_frame_coverage_complete &&
                event_lines.Complete() &&
                timeline_lines.Complete() &&
                (!performance_lines || performance_lines->Complete()) &&
                (!gpu_live_lines || gpu_live_lines->Complete()) &&
                (!graphics_settings_lines || graphics_settings_lines->Complete());
            const bool status_written = write_diagnostics_status(std::format(
                "{{\"schema\":1,\"mode\":\"{}\",\"complete\":{},\"io_failed\":{},"
                "\"event_records\":{},\"event_dropped\":{},\"timeline_records\":{},"
                "\"timeline_dropped\":{},"
                "\"performance_records\":{},\"gpu_live_records\":{},\"graphics_settings_records\":{},"
                "\"gpu_frames_written\":{},\"gpu_expected_frames\":{},\"gpu_last_frame\":{},\"gpu_timing_requested\":{},"
                "\"gpu_frame_coverage_complete\":{},\"gpu_drain_succeeded\":{},\"gpu_invalid_frames\":{}}}\n",
                "capture", complete ? "true" : "false",
                io_ok ? "false" : "true", event_lines.Records(), event_lines.Dropped(),
                timeline_lines.Records(), timeline_lines.Dropped(),
                performance_lines ? performance_lines->Records() : 0,
                gpu_live_lines ? gpu_live_lines->Records() : 0,
                graphics_settings_lines ? graphics_settings_lines->Records() : 0,
                performance_state.GpuFrames(), expected_gpu_frames, performance_state.LastGpuFrame(),
                gpu_live_lines ? "true" : "false",
                gpu_frame_coverage_complete ? "true" : "false", gpu_drain_succeeded ? "true" : "false", invalid_gpu_frames));
            if (!status_started || !status_written || !complete)
                record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                    "Diagnostic output incomplete: GPU frame coverage, metadata, write, flush or close failed."));
            WriteText(artifact_directory / "render.json",
                      std::format("{{\"enhanced_barriers\":{},\"validation_errors\":{},"
                                  "\"validation_message_collection_attempted\":{},"
                                  "\"validation_message_collection_hresult\":{},"
                                  "\"last_tick\":{},"
                                  "\"typed_vfx_ground_peak\":{},"
                                  "\"typed_vfx_ground_submitted_total\":{},"
                                  "\"typed_vfx_flash_peak\":{},"
                                  "\"typed_vfx_flash_submitted_total\":{},"
                                  "\"vfx_workspace_soft_budget_bytes\":{},"
                                  "\"vfx_fire_admission_peak_bytes\":{},"
                                  "\"vfx_fire_budget_limited_frames\":{},"
                                  "\"vfx_mote_budget_limited_frames\":{},"
                                  "\"vfx_birth_requested_rows\":{},"
                                  "\"vfx_birth_allowed_rows\":{},"
                                  "\"vfx_birth_budget_limited_frames\":{},"
                                  "\"vfx_profile_requested_memberships\":{},"
                                  "\"vfx_profile_admitted_memberships\":{},"
                                  "\"vfx_profile_budget_limited_frames\":{},"
                                  "\"vfx_profile_light_reserved_peak_rows\":{},"
                                  "\"vfx_profile_ground_reserved_peak_rows\":{},"
                                  "\"vfx_profile_ground_reserved_peak_gaps\":{},"
                                  "\"vfx_profile_ribbon_reserved_peak_rows\":{},"
                                  "\"vfx_profile_ribbon_reserved_peak_outputs\":{},"
                                  "\"vfx_profile_distortion_reserved_peak_rows\":{},"
                                  "\"vfx_profile_distortion_reserved_peak_gaps\":{},"
                                  "\"vfx_profile_decal_reserved_peak_rows\":{},"
                                  "\"vfx_profile_volume_reserved_peak_rows\":{},"
                                  "\"vfx_profile_screen_reserved_peak_rows\":{},"
                                  "\"typed_vfx_mesh_peak\":{},"
                                  "\"typed_vfx_fresnel_peak\":{},"
                                  "\"typed_vfx_ribbon_peak\":{},"
                                  "\"typed_vfx_ribbon_dropped\":{},"
                                  "\"typed_vfx_light_peak\":{},"
                                  "\"typed_vfx_distortion_peak\":{},"
                                  "\"typed_vfx_decal_peak\":{},"
                                  "\"typed_vfx_volume_peak\":{},"
                                  "\"static_scene_upload_count\":{},"
                                  "\"baked_static_shadow_active\":{},"
                                  "\"baked_static_shadow_pages\":{},"
                                  "\"baked_static_shadow_staging_bytes\":{},"
                                  "\"baked_static_shadow_cpu_plan_prepared\":{},"
                                  "\"baked_static_shadow_csm_required\":{},"
                                  "\"baked_static_shadow_guarantee_reason_bits\":{},"
                                  "\"baked_static_shadow_required_coarse_pages\":{},"
                                  "\"baked_static_shadow_maximum_coarse_commands\":{},"
                                  "\"baked_static_shadow_coarse_command_total\":{},"
                                  "\"baked_static_shadow_command_capacity\":{},"
                                  "\"typed_vfx_unsupported_inputs\":{}}}\n",
                                  enhanced ? "true" : "false", validation_errors,
                                  validation_status.message_collection_attempted ? "true" : "false",
                                  validation_status.message_collection_hresult, last_tick,
                                  typed_vfx_ground_peak, typed_vfx_ground_submitted_total,
                                  typed_vfx_flash_peak, typed_vfx_flash_submitted_total,
                                  runtime_detail::kVfxWorkspaceSoftBudgetBytes,
                                  vfx_fire_admission_peak_bytes, vfx_fire_budget_limited_frames,
                                  vfx_mote_budget_limited_frames,
                                  vfx_birth_requested_rows, vfx_birth_allowed_rows, vfx_birth_budget_limited_frames,
                                  vfx_profile_requested_memberships, vfx_profile_admitted_memberships,
                                  vfx_profile_budget_limited_frames,
                                  vfx_profile_light_reserved_peak_rows,
                                  vfx_profile_ground_reserved_peak_rows,
                                  vfx_profile_ground_reserved_peak_gaps,
                                  vfx_profile_ribbon_reserved_peak_rows, vfx_profile_ribbon_reserved_peak_outputs,
                                  vfx_profile_distortion_reserved_peak_rows, vfx_profile_distortion_reserved_peak_gaps,
                                  vfx_profile_decal_reserved_peak_rows,
                                  vfx_profile_volume_reserved_peak_rows, vfx_profile_screen_reserved_peak_rows,
                                  typed_vfx_mesh_peak, typed_vfx_fresnel_peak,
                                  typed_vfx_ribbon_peak, typed_vfx_ribbon_dropped,
                                  typed_vfx_light_peak, typed_vfx_distortion_peak, typed_vfx_decal_peak,
                                  typed_vfx_volume_peak,
                                  static_scene_upload_count,
                                  shadow_cache.baked_active, shadow_cache.baked_pages, shadow_cache.baked_staging_bytes,
                                  shadow_cache.cpu_plan_prepared, shadow_cache.csm_required, shadow_cache.guarantee_reason_bits,
                                  shadow_cache.required_coarse_pages, shadow_cache.maximum_coarse_commands,
                                  shadow_cache.coarse_command_total, shadow_cache.command_capacity,
                                  typed_vfx_unsupported_inputs));
        }
#endif
        }
        catch (const std::bad_alloc &)
        {
            record_thread_failure(Result::Failure(ErrorCode::TaskFailed, "hs_runtime", "Out of memory"));
        }
    });

    // Renderer initialization may call DXGI with this HWND from the render
    // thread. Keep its owner thread dispatching window/system messages until
    // the renderer publishes readiness instead of blocking in atomic::wait.
    while (!channels.render_ready.load(std::memory_order_acquire))
    {
        const auto processed_wake_sequence = main_wake.Sequence();
        Result window_failure = Result::Success();
        if (!window.PumpMessages(&window_failure, false))
        {
            if (!window_failure) record_thread_failure(std::move(window_failure));
            request_stop();
        }
#ifdef HS_DEVELOPMENT_TOOLS
        if (config.smoke_render_lockstep && channels.stop_requested.load(std::memory_order_acquire))
            request_stop();
#endif
        if (channels.render_ready.load(std::memory_order_acquire)) break;

        const auto pending_startup = [&]() noexcept {
            MSG pending{};
            return channels.render_ready.load(std::memory_order_acquire) ||
                runtime_detail::PeekMainMessage(pending, false, PM_NOREMOVE);
        };
        if (main_wake.PrepareWait(pending_startup, processed_wake_sequence) &&
            !pending_startup())
        {
            const auto handle = main_wake.Handle();
            const auto wait_result = MsgWaitForMultipleObjectsEx(1, &handle, INFINITE,
                runtime_detail::MainMessageMask(false),
                runtime_detail::MainMessageFlags(false));
            if (wait_result == WAIT_FAILED)
                record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                    std::format("Main startup message/event wait failed: {}.", GetLastError())));
        }
        if (const auto error = main_wake.Error())
            record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                std::format("Main startup wake event failed: {}.", error)));
    }
    {
        std::lock_guard lock(result_mutex);
        result = thread_result;
    }
    if (!result)
    {
        // Simulation was never started; let Render finish its normal drain.
        finish_simulation();
        // Teardown can publish another failure through result_mutex.
        render_thread.join();
        return {result};
    }

    auto simulation_ports = channels.ForSimulation();
    const SettingsData initial_projection_settings = settings;
    std::jthread simulation_thread;
    try
    {
    simulation_thread = std::jthread([&, simulation_ports, initial_projection_settings](std::stop_token) mutable {
        auto &channels = simulation_ports;
        SetThreadName(L"HS Simulation");
        GameSimulation simulation;
        SettingsData projection_settings = initial_projection_settings;
#ifdef HS_DEVELOPMENT_TOOLS
        const bool capture_balance = config.record_playtest || config.capture_diagnostics ||
            (config.smoke && !config.test_room) || replaying;
#endif
#ifdef HS_DEVELOPMENT_TOOLS
        if (auto initialized = simulation.Initialize(
                {.seed = effective_seed, .automatic_choices = config.smoke,
                 .start_in_main_menu = !config.smoke && !config.test_room,
                 .scenario = {.player_stationary = config.test_room_capture.has_value(),
                              .player_invulnerable = config.diagnostic_enemy_count != 0 || config.test_room,
                              .progression_enabled = config.diagnostic_enemy_count == 0 && !config.test_room,
                              .automatic_spawning = config.diagnostic_enemy_count == 0 && !config.test_room},
                 .capture_checksums = config.diagnostic_enemy_count == 0 && !config.test_room &&
                                      (config.smoke || config.record_playtest || replaying),
                 .capture_detailed_balance = capture_balance,
                 .capture_hit_rates = capture_balance},
                simulation_rules);
            !initialized)
        {
            record_thread_failure(initialized);
            finish_simulation();
            return;
        }
#else
        if (auto initialized = simulation.Initialize(
                {.seed = config.seed, .automatic_choices = false, .start_in_main_menu = true,
                 .capture_checksums = false,
                 .capture_detailed_balance = false, .capture_hit_rates = false},
                simulation_rules);
            !initialized)
        {
            record_thread_failure(initialized);
            finish_simulation();
            return;
        }
#endif

#ifdef HS_DEVELOPMENT_TOOLS
        const auto captured_skill =
            config.skill_vfx_capture < kCombatSkillCount
                ? static_cast<SkillKind>(config.skill_vfx_capture)
                : SkillKind::Count;
        if (captured_skill != SkillKind::Count)
        {
            if (captured_skill != SkillKind::BasicAttack)
            {
                if (auto granted = simulation.ApplyDebugCommand(
                        {DebugCommandKind::GrantSkill,
                         static_cast<std::uint64_t>(captured_skill)});
                    !granted)
                {
                    record_thread_failure(granted);
                    finish_simulation();
                    return;
                }
            }
            for (std::uint32_t upgrade = 0; upgrade < kUpgradeCount; ++upgrade)
            {
                if ((config.skill_vfx_upgrade_mask & (1u << upgrade)) == 0) continue;
                if (auto granted = simulation.ApplyDebugCommand(
                        {DebugCommandKind::GrantUpgrade,
                         static_cast<std::uint64_t>(captured_skill), upgrade});
                    !granted)
                {
                    record_thread_failure(granted);
                    finish_simulation();
                    return;
                }
            }
            constexpr std::array enemy_positions{
                Float2{-2.0f, 5.0f}, Float2{0.0f, 5.0f}, Float2{2.0f, 5.0f},
                Float2{-3.0f, 7.0f}, Float2{-1.0f, 7.0f}, Float2{1.0f, 7.0f},
                Float2{3.0f, 7.0f},  Float2{-2.0f, 9.0f}, Float2{0.0f, 9.0f},
                Float2{2.0f, 9.0f},  Float2{-1.0f, 11.0f}, Float2{1.0f, 11.0f}};
            for (const auto position : enemy_positions)
            {
                if (auto spawned = simulation.ApplyDebugCommand(
                        {DebugCommandKind::SpawnEnemy, 0, 0, position});
                    !spawned)
                {
                    record_thread_failure(spawned);
                    finish_simulation();
                    return;
                }
            }
        }
        if (config.monster_preview_asset < 6)
        {
            const auto boss = config.monster_preview_asset >= 3;
            if (auto spawned = simulation.ApplyDebugCommand(
                    {boss ? DebugCommandKind::SpawnBoss : DebugCommandKind::SpawnEnemy,
                     boss ? config.monster_preview_asset - 3 : config.monster_preview_asset,
                     0, {0.0f, 0.0f}});
                !spawned)
            {
                record_thread_failure(spawned);
                finish_simulation();
                return;
            }
        }
        if (config.boss_phase2_capture || config.boss_vfx_capture < 3)
        {
            if (auto spawned = simulation.ApplyDebugCommand(
                    {DebugCommandKind::SpawnBoss,
                     config.boss_phase2_capture ? 2u : config.boss_vfx_capture});
                !spawned)
            {
                record_thread_failure(spawned);
                finish_simulation();
                return;
            }
        }
#endif

#ifdef HS_DEVELOPMENT_TOOLS
        std::uint64_t diagnostic_spawn_index{};
        std::uint32_t initial_added{};
        {
            if (auto filled = RefillDiagnosticEnemies(simulation, config.diagnostic_enemy_count, diagnostic_spawn_index, initial_added); !filled)
            {
                record_thread_failure(filled);
                finish_simulation();
                return;
            }
        }
#endif
        FixedStepClock clock;
        auto now = FixedStepClock::Clock::now();
        (void)clock.Advance(now);
        std::uint64_t observed_focus_epoch{};
        Sequence publish_sequence{};
        RenderSnapshotStorage dynamic_previous(1'024, 256, 4, 0);
        RenderSnapshotStorage dynamic_current(1'024, 256, 4, 0);
        bool has_dynamic_sample{};
        bool has_previous_dynamic_sample{};
        std::vector<DomainSignal> retained_line_signals;
        std::uint64_t retained_line_session{};
        Sequence retained_line_sequence_high_water{};
        Tick retained_line_last_tick{};
        bool retained_line_state_initialized{};
        std::uint64_t display_clock_epoch{};
        RenderClockMode display_clock_mode{RenderClockMode::Scripted};
        std::uint64_t dynamic_pair_revision{};
        std::optional<std::uint64_t> rendered_session_barrier;
        bool rendered_playing_barrier{};
        std::shared_ptr<const UiFrame> prepared_ui;
        std::vector<std::uint64_t> ui_projection_facts;
        std::array<ActionEdge, 256> action_storage{};
        std::array<UiAction, 64> ui_action_storage{};
        auto previous_phase = config.smoke ? SessionPhase::Playing : SessionPhase::MainMenu;
        EnemyAnimationState enemy_animations;
        PlayerFootstepTimeline footstep_timeline;
        std::shared_ptr<const StaticRenderScene> launch_environment;
        std::shared_ptr<const MeadowGroundQuery> launch_ground = std::make_shared<MeadowGroundQuery>();
        const auto configure_dynamic_camera = [&] {
            const auto zoom_percent = static_cast<float>(channels.camera_zoom_percent.load(std::memory_order_acquire));
            const auto pose = ComputeRuntimeCameraPose(runtime_camera.distance_m,
                runtime_camera.pitch_degrees, zoom_percent);
            dynamic_current.camera.yaw_degrees = runtime_camera.yaw_degrees;
            dynamic_current.camera.pitch_degrees = pose.pitch_degrees;
            dynamic_current.camera.vertical_fov_degrees = runtime_camera.vertical_fov_degrees;
            dynamic_current.camera.distance = pose.distance_m;
            dynamic_current.camera.target_height_offset = pose.target_height_m;
            dynamic_current.camera.target.y = pose.target_height_m +
                launch_ground->Height({dynamic_current.camera.target.x, dynamic_current.camera.target.z});
        };
        const auto retain_line_signals = [&](const TickReadView &model) {
            if (!retained_line_state_initialized || retained_line_session != model.session_id ||
                model.tick < retained_line_last_tick)
            {
                retained_line_signals.clear();
                retained_line_sequence_high_water = 0;
                retained_line_session = model.session_id;
                retained_line_state_initialized = true;
            }
            const auto is_line_signal = [](DomainSignalKind kind) {
                switch (kind)
                {
                case DomainSignalKind::RicochetLinked:
                case DomainSignalKind::BurnTransferred:
                case DomainSignalKind::RelicChainLinked: return true;
                default: return false;
                }
            };
            std::erase_if(retained_line_signals, [&](const DomainSignal &signal) {
                return signal.session_id != model.session_id || !is_line_signal(signal.kind) ||
                    signal.tick + 11 <= model.tick;
            });
            for (const auto &signal : model.domain_signals)
            {
                if (signal.sequence <= retained_line_sequence_high_water) continue;
                // Gameplay appends DomainSignals in sequence order. Advance over
                // every event so repeated read seeds cannot re-admit an old line.
                retained_line_sequence_high_water = signal.sequence;
                if (!is_line_signal(signal.kind) || signal.session_id != model.session_id ||
                    signal.tick > model.tick || signal.tick + 11 <= model.tick ||
                    (signal.flags & static_cast<std::uint8_t>(DomainSignalFlag::HasTarget)) == 0)
                    continue;
                retained_line_signals.push_back(signal);
            }
            retained_line_last_tick = model.tick;
        };
        {
            auto read_scope = simulation.BorrowTickReadView();
            const auto &model = read_scope.View();
            retain_line_signals(model);
            // Seed the preceding endpoint without consuming or replaying pending events.
            enemy_animations.Update(model, {});
            footstep_timeline.Reset(model, enemy_animations.PlayerMotionEpoch());
#ifdef HS_DEVELOPMENT_TOOLS
            if (config.test_room)
            {
                auto room_scene = std::make_shared<StaticRenderScene>();
                room_scene->content_generation_id = deployment_id;
                launch_environment = std::move(room_scene);
            }
            else launch_environment = ProcessSceneView(BuildProcessSceneAtLaunch(model, deployment_id));
#else
            launch_environment = ProcessSceneView(BuildProcessSceneAtLaunch(model, deployment_id));
#endif
            auto ground = std::make_shared<MeadowGroundQuery>();
            if (auto prepared = ground->Prepare(launch_environment->instances); !prepared)
            { record_thread_failure(prepared); finish_simulation(); return; }
            launch_ground = std::move(ground);
#if defined(HS_DEVELOPMENT_TOOLS)
            if (config.test_room)
                if (auto room_ground = channels.room_ground.load(std::memory_order_acquire)) launch_ground = std::move(room_ground);
#endif
            if (auto projected = ProjectDynamicTick(model, dynamic_current,
                    launch_environment, *launch_ground, &enemy_animations, retained_line_signals); !projected)
            {
                record_thread_failure(projected);
                finish_simulation();
                return;
            }
            configure_dynamic_camera();
            now = FixedStepClock::Clock::now();
            clock.Realign(now);
            display_clock_epoch = 1;
            display_clock_mode = !realtime ? RenderClockMode::Scripted
                : model.session.phase == SessionPhase::Playing ? RenderClockMode::RealtimeRunning
                                                               : RenderClockMode::Frozen;
            dynamic_current.header.clock = {display_clock_epoch, display_clock_mode,
                realtime ? now : FixedStepClock::Clock::time_point{}};
            dynamic_current.header.dynamic_pair_revision = ++dynamic_pair_revision;
            has_dynamic_sample = true;
        }
#ifdef HS_DEVELOPMENT_TOOLS
        std::vector<PresentationEvent> record_events;
#endif
#ifdef HS_DEVELOPMENT_TOOLS
        std::size_t next_timeline_action{};
        std::size_t replay_frame_index{};
#endif
#ifdef HS_DEVELOPMENT_TOOLS
        bool smoke_finished{};
#else
        constexpr bool smoke_finished = false;
#endif
        while (!channels.stop_requested.load(std::memory_order_acquire))
        {
            const auto native_token = native_input_frame.AwaitSimulation();
            if (channels.stop_requested.load(std::memory_order_acquire)) break;
            std::uint32_t tick_count = 1;
            FixedStepClock::Duration simulation_debt{};
            FixedStepClock::Clock::time_point first_tick_boundary{};
            if (realtime)
            {
                now = FixedStepClock::Clock::now();
                const auto advanced = clock.Advance(now);
                tick_count = advanced.tick_count;
                simulation_debt = advanced.debt;
                first_tick_boundary = advanced.first_tick_boundary;
                if (tick_count == 0)
                {
                    if (native_token) { native_input_frame.Simulated(native_token); continue; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
            }

            bool stop_remaining_admitted_steps{};
            for (std::uint32_t local_tick = 0;
                 local_tick < tick_count &&
                 !smoke_finished && !stop_remaining_admitted_steps;
                 ++local_tick)
            {
#ifdef HS_DEVELOPMENT_TOOLS
                auto harness_added = std::exchange(initial_added, 0u);
#else
                constexpr std::uint32_t harness_added = 0;
#endif
                bool snapshot_published{};
                bool forced_dynamic_refresh{};
                SettingsData updated_presentation_settings;
                while (channels.presentation_settings.TryPop(
                    updated_presentation_settings))
                    projection_settings = updated_presentation_settings;
                std::size_t ui_action_count{};
                runtime_detail::UiActionEnvelope queued_ui_action;
                if (channels.ui_actions.TryPop(queued_ui_action))
                {
                    const auto authoritative_session = simulation.GetSessionProbe();
                    const auto authoritative_session_id = channels.current_session_id.load(
                        std::memory_order_acquire);
                    if (runtime_detail::UiActionCurrent(queued_ui_action, authoritative_session_id,
                                                        authoritative_session))
                        ui_action_storage[ui_action_count++] = queued_ui_action.action;
                    else
                    {
                        channels.stale_ui_action_drops.fetch_add(1, std::memory_order_relaxed);
                        channels.ui_input_rejected.store(true, std::memory_order_release);
                    }
                }
                const auto focus_epoch = channels.focus_epoch.load(std::memory_order_acquire);
                if (focus_epoch != observed_focus_epoch)
                {
                    channels.action_edges.ClearConsumer();
                    observed_focus_epoch = focus_epoch;
                }

                std::size_t action_count{};
                ActionEdge action;
                while (action_count < action_storage.size() &&
                       channels.action_edges.TryPop(action))
                {
                    const auto current_phase = static_cast<SessionPhase>(
                        channels.session_phase.load(std::memory_order_acquire));
                    const auto ui_page = static_cast<UiPage>(
                        channels.ui_page.load(std::memory_order_acquire));
                    if (action.kind == EdgeKind::Pressed &&
                        action.action == GameAction::Pause &&
                        ((current_phase == SessionPhase::Paused &&
                          ui_page == UiPage::PauseSettings) ||
                         (current_phase == SessionPhase::MainMenu &&
                          ui_page != UiPage::Root)))
                    {
                        channels.ui_page.store(static_cast<std::uint8_t>(UiPage::Root),
                                               std::memory_order_release);
                        channels.character_details_page.store(0, std::memory_order_release);
                        continue;
                    }
                    if (action.kind == EdgeKind::Pressed && action.action == GameAction::Pause &&
                        current_phase == SessionPhase::Paused &&
                        (ui_page == UiPage::CharacterOverview || ui_page == UiPage::CharacterSkills ||
                         ui_page == UiPage::CharacterStats))
                    {
                        channels.ui_page.store(static_cast<std::uint8_t>(UiPage::Root),
                                               std::memory_order_release);
                        channels.character_details_page.store(0, std::memory_order_release);
                        channels.loadout_source.store(0xFF, std::memory_order_release);
                    }
                    if (action.kind == EdgeKind::Pressed &&
                        action.action == GameAction::CharacterPage)
                    {
                        channels.ui_page.store(
                            static_cast<std::uint8_t>(
                                current_phase == SessionPhase::Playing
                                    ? UiPage::CharacterOverview
                                    : UiPage::Root),
                            std::memory_order_release);
                        channels.loadout_source.store(0xFF, std::memory_order_release);
                        channels.character_details_page.store(0, std::memory_order_release);
                    }
                    action_storage[action_count++] = action;
                }

                InputFrame input;
#ifdef HS_DEVELOPMENT_TOOLS
                if (replaying)
                {
                    if (replay_frame_index >= replay.frames.size())
                    {
                        request_stop();
                        break;
                    }
                    input = replay.frames[replay_frame_index].View();
                }
                else
#endif
                {
                    input.target_tick =
                        channels.completed_tick.load(std::memory_order_relaxed) + 1;
                    input.held = channels.held_input.load(std::memory_order_acquire);
                    input.ordered_edges = std::span(action_storage.data(), action_count);
                    input.ui_actions =
                        std::span(ui_action_storage.data(), ui_action_count);
                }
#ifdef HS_DEVELOPMENT_TOOLS
                if (captured_skill != SkillKind::Count && !replaying)
                {
                    input.held.aim_world = {config.skill_vfx_aim.x, 0.0f,
                                            config.skill_vfx_aim.y};
                    input.held.basic_attack_held =
                        captured_skill == SkillKind::BasicAttack && input.target_tick >= 5;
                    const auto add_edge = [&](GameAction action, EdgeKind kind) {
                        action_storage[action_count++] =
                            {static_cast<Sequence>(0xF000u + input.target_tick), action, kind};
                        input.ordered_edges =
                            std::span(action_storage.data(), action_count);
                    };
                    if (captured_skill != SkillKind::BasicAttack && input.target_tick == 5)
                        add_edge(GameAction::SkillQ, EdgeKind::Pressed);
                    if (captured_skill == SkillKind::ChargedShot && input.target_tick == 65)
                        add_edge(GameAction::SkillQ, EdgeKind::Released);
                }
#endif
#ifdef HS_DEVELOPMENT_TOOLS
                if (config.diagnostic_enemy_count)
                {
                    const auto player = simulation.GetSessionProbe().player_position;
                    constexpr std::array corners{Float3{-4, 0, -4}, Float3{4, 0, -4},
                                                  Float3{4, 0, 4}, Float3{-4, 0, 4}};
                    input.held.move_held = true;
                    input.held.move_target_world = corners[((input.target_tick - 1) / 300) % corners.size()];
                    input.held.basic_attack_held = true;
                    input.held.aim_world = {player.x, 0, player.y + 8};
                    if (input.target_tick >= 180 && (input.target_tick - 180) % 90 == 0)
                    {
                        if (action_count == action_storage.size())
                        {
                            record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Diagnostic input edge capacity exceeded."));
                            break;
                        }
                        constexpr std::array skills{GameAction::SkillQ, GameAction::SkillW, GameAction::SkillE, GameAction::SkillR};
                        action_storage[action_count++] = {static_cast<Sequence>(0xF0000000ull + input.target_tick),
                            skills[((input.target_tick - 180) / 90) % skills.size()], EdgeKind::Pressed};
                        input.ordered_edges = std::span(action_storage.data(), action_count);
                    }
                }
#endif
#ifdef HS_DEVELOPMENT_TOOLS
                if (next_timeline_action < config.timeline_actions.size() &&
                    config.timeline_actions[next_timeline_action].target_tick < input.target_tick)
                {
                    record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                                "Timeline action missed its target tick."));
                    break;
                }
                while (next_timeline_action < config.timeline_actions.size() &&
                       config.timeline_actions[next_timeline_action].target_tick ==
                           input.target_tick)
                {
                    forced_dynamic_refresh = true;
                    const auto &scheduled = config.timeline_actions[next_timeline_action++];
                    if (auto applied = simulation.ApplyDebugCommand(
                            {static_cast<DebugCommandKind>(scheduled.kind), scheduled.value,
                             scheduled.secondary, scheduled.position});
                        !applied)
                    {
                        record_thread_failure(applied);
                        break;
                    }
                }
#endif
#ifdef HS_DEVELOPMENT_TOOLS
                if (config.boss_phase2_capture && config.smoke && !replaying &&
                    input.target_tick == 10)
                {
                    forced_dynamic_refresh = true;
                    if (auto damaged = simulation.ApplyDebugCommand(
                            {DebugCommandKind::DamageFinalBoss, 2250}); !damaged)
                    {
                        record_thread_failure(damaged);
                        break;
                    }
                }
#endif
                if (channels.stop_requested.load(std::memory_order_acquire))
                    break;
#ifdef HS_DEVELOPMENT_TOOLS
                DebugCommand debug_command;
                while (channels.debug_commands.TryPop(debug_command))
                {
                    forced_dynamic_refresh = true;
                    if (auto applied = simulation.ApplyDebugCommand(debug_command); !applied)
                    {
                        record_thread_failure(applied);
                        break;
                    }
                }
#endif
                const auto tick = simulation.TickFixed(input);
#ifdef HS_DEVELOPMENT_TOOLS
                if (config.diagnostic_enemy_count)
                {
                    if (auto filled = RefillDiagnosticEnemies(simulation, config.diagnostic_enemy_count, diagnostic_spawn_index, harness_added); !filled)
                    {
                        record_thread_failure(filled);
                        break;
                    }
                }
#endif
#ifdef HS_DEVELOPMENT_TOOLS
                const bool final_smoke_frame =
                    config.smoke &&
                    (tick.tick >= config.maximum_ticks ||
                     tick.phase == SessionPhase::Victory ||
                     tick.phase == SessionPhase::Defeat);
#else
                constexpr bool final_smoke_frame = false;
#endif
                {
                    auto read_scope = simulation.BorrowTickReadView();
                    const auto &model = read_scope.View();
                    const bool new_session = !has_dynamic_sample ||
                        dynamic_current.header.session_id != model.session_id;
                    const bool tick_changed = !has_dynamic_sample ||
                        dynamic_current.header.tick != model.tick;
                    bool ground_changed = false;
#if defined(HS_DEVELOPMENT_TOOLS)
                    if (config.test_room)
                        if (auto room_ground = channels.room_ground.load(std::memory_order_acquire);
                            room_ground && room_ground != launch_ground)
                        { launch_ground = std::move(room_ground); ground_changed = true; }
#endif
                    retain_line_signals(model);
                    const bool project_dynamic = new_session || tick_changed || tick.world_changed ||
                        forced_dynamic_refresh || ground_changed || harness_added != 0;
                    if (project_dynamic)
                    {
                        enemy_animations.Update(model, model.domain_signals);
                    }
                    const auto &probe = model.session;
#ifdef HS_DEVELOPMENT_TOOLS
                    channels.PublishSessionProbe(probe, model.session_id);
                    if (replaying && !config.replay_compare && tick.checksum !=
                                         replay.frames[replay_frame_index].expected_checksum)
                    {
                        record_thread_failure(Result::Failure(
                            ErrorCode::InvalidState, "hs_playtest",
                            std::format("Replay checksum mismatch at tick {}.", tick.tick)));
                        break;
                    }
                    if (replaying) ++replay_frame_index;
#else
                    channels.PublishSessionProbe(probe, model.session_id);
#endif
                    channels.camera_target_x.store(probe.player_position.x,
                                                   std::memory_order_release);
                    channels.camera_target_z.store(probe.player_position.y,
                                                   std::memory_order_release);
                    channels.session_phase.store(static_cast<std::uint8_t>(tick.phase),
                                                 std::memory_order_release);
                    if ((tick.phase == SessionPhase::Victory ||
                         tick.phase == SessionPhase::Defeat) &&
                        tick.phase != previous_phase)
                    {
                        channels.completed_run_kills += probe.kills;
                        if (tick.phase == SessionPhase::Victory)
                        {
                            ++channels.completed_run_wins;
                        }
                        channels.best_level = std::max(channels.best_level,
                                                       static_cast<std::uint32_t>(probe.level));
                    }
                    previous_phase = tick.phase;

                    try
                    {
#ifdef HS_DEVELOPMENT_TOOLS
                        const bool recording = playtest_recorder.Active();
#else
                        constexpr bool recording = false;
#endif
                        channels.PublishAudioTick({model.session_id, probe.tick, tick.phase,
                            {probe.player_position.x, 0.0f, probe.player_position.y}});
#ifdef HS_DEVELOPMENT_TOOLS
                        channels.ProjectDomainSignalsOnce(model.domain_signals, *launch_ground,
                            capture_diagnostics, recording ? &record_events : nullptr);
#else
                        channels.ProjectDomainSignalsOnce(model.domain_signals, *launch_ground,
                            false, nullptr);
#endif
                        if (const auto footstep = footstep_timeline.Advance(
                                model, enemy_animations.PlayerMotionEpoch(), *launch_ground))
                            channels.PublishAudioEvent(*footstep);
                        if (recording)
                        {
#ifdef HS_DEVELOPMENT_TOOLS
                            const auto diagnostics = simulation.GetDiagnostics();
                            SimulationObservation recorded_probe;
                            static_cast<SessionProbe &>(recorded_probe) = probe;
                            recorded_probe.balance = diagnostics.balance;
                            if (auto recorded = playtest_recorder.Record(
                                    input, tick.checksum, recorded_probe, record_events, model.session_id); !recorded)
                            {
                                record_thread_failure(recorded);
                                break;
                            }
#endif
                        }
                    }
                    catch (const std::exception &failure)
                    {
                        // Fanout may already be partial. Stop instead of replaying this tick.
                        record_thread_failure(Result::Failure(ErrorCode::TaskFailed, "hs_runtime", failure.what()));
                        break;
                    }
                    catch (...)
                    {
                        record_thread_failure(Result::Failure(ErrorCode::TaskFailed, "hs_runtime",
                            "Simulation tick presentation failed."));
                        break;
                    }

                    const auto next_clock_mode = !realtime ? RenderClockMode::Scripted
                        : tick.phase == SessionPhase::Playing ? RenderClockMode::RealtimeRunning
                                                            : RenderClockMode::Frozen;
                    auto current_boundary = first_tick_boundary + FixedStepClock::kFixedStep * local_tick;
                    if (realtime && has_dynamic_sample &&
                        (new_session || model.tick < dynamic_current.header.tick ||
                         (display_clock_mode != RenderClockMode::RealtimeRunning &&
                          next_clock_mode == RenderClockMode::RealtimeRunning)))
                    {
                        current_boundary = FixedStepClock::Clock::now();
                        clock.Realign(current_boundary);
                        simulation_debt = {};
                        stop_remaining_admitted_steps = true;
                    }
                    if (new_session || next_clock_mode != display_clock_mode ||
                        (has_dynamic_sample && model.tick < dynamic_current.header.tick))
                        ++display_clock_epoch;
                    display_clock_mode = next_clock_mode;
                    if (new_session || tick_changed)
                    {
                        has_previous_dynamic_sample = has_dynamic_sample && !new_session &&
                            model.tick > dynamic_current.header.tick &&
                            model.tick - dynamic_current.header.tick == 1;
                        std::swap(dynamic_previous, dynamic_current);
                    }
                    if (project_dynamic) dynamic_current.Clear();
                    if (project_dynamic)
                    {
                        if (auto projected = ProjectDynamicTick(model, dynamic_current,
                                launch_environment, *launch_ground, &enemy_animations, retained_line_signals); !projected)
                        {
                            record_thread_failure(projected);
                            break;
                        }
                    }
                    if (project_dynamic) ++dynamic_pair_revision;
                    dynamic_current.header.checksum = model.checksum;
                    dynamic_current.header.dynamic_pair_revision = dynamic_pair_revision;
                    dynamic_current.header.clock = {display_clock_epoch, display_clock_mode,
                        realtime ? current_boundary
                                 : FixedStepClock::Clock::time_point{}};
                    configure_dynamic_camera();
                    has_dynamic_sample = true;

                    // Retain every actual dynamic sample, but publish UI only once after catch-up.
                    if (local_tick + 1 == tick_count || stop_remaining_admitted_steps ||
                        final_smoke_frame || config.smoke_render_lockstep)
                    {
                        auto slot = channels.snapshots.TryBeginRetainedWrite();
                        // The terminal snapshot must reach the renderer.  Waiting
                        // here only applies to exchange backpressure for that final
                        // frame; it never advances the simulation another tick.
                        while (!slot && final_smoke_frame &&
                               !channels.stop_requested.load(std::memory_order_acquire))
                        {
                            std::this_thread::yield();
                            slot = channels.snapshots.TryBeginRetainedWrite();
                        }
                        if (slot)
                        {
                            const auto graphics_ui = channels.presentation_graphics_ui.load(std::memory_order_acquire);
                            const PresentationUiState ui_draw_state{
                                static_cast<UiPage>(channels.ui_page.load(std::memory_order_acquire)),
                                channels.collection_skill.load(std::memory_order_acquire),
                                channels.character_skill.load(std::memory_order_acquire),
                                channels.loadout_source.load(std::memory_order_acquire),
                                channels.settings_group.load(std::memory_order_acquire),
                                channels.settings_page.load(std::memory_order_acquire),
                                channels.character_details_page.load(std::memory_order_acquire)};
                            const auto modal_phase = display_modal_phase.load(std::memory_order_acquire);
                            const auto pending_rebind = channels.pending_rebind_slot.load(std::memory_order_acquire);
                            const auto input_rejected = channels.ui_input_rejected.load(std::memory_order_acquire);
                            try
                            {
                                (void)slot->Storage().CopyRetainedDynamicFrom(dynamic_current.View());
                                CaptureUiProjectionFacts(model, presentation_catalog, ui_draw_state,
                                    projection_settings, static_cast<std::uint8_t>(modal_phase),
                                    pending_rebind, input_rejected, ui_projection_facts);
                                const bool reuse_draw = prepared_ui &&
                                    runtime_detail::SameUiDrawInputs(*prepared_ui,
                                        ui_projection_facts, graphics_ui);
                                std::shared_ptr<const UiDrawContent> draw_content = reuse_draw
                                    ? prepared_ui->draw_content : nullptr;
                                std::shared_ptr<const std::vector<std::uint64_t>> draw_facts = reuse_draw
                                    ? prepared_ui->draw_facts : nullptr;
                                if (!reuse_draw)
                                {
                                    ProjectTickUi(model, presentation_catalog, ui_draw_state,
                                        projection_settings, slot->Storage(), pending_rebind, graphics_ui.get());
                                    const auto page = ui_draw_state.page;
                                    if ((page == UiPage::MainMenuSettings || page == UiPage::PauseSettings) &&
                                        modal_phase != GraphicsUiPhase::Applied)
                                    {
                                        UiModel backdrop;
                                        backdrop.kind = UiModel::Kind::Panel;
                                        backdrop.anchor_pixels = {550, 360};
                                        backdrop.size_pixels = {820, 360};
                                        backdrop.color_rgba = 0xF0202430u;
                                        const auto title = SettingsStatusText(
                                            modal_phase == GraphicsUiPhase::AwaitingDisplayConfirmation
                                                ? "화면 모드를 유지하시겠습니까? (15초 안에 확인)"
                                                : modal_phase == GraphicsUiPhase::RestoringDisplay
                                                    ? "이전 화면 모드로 복원 중..."
                                                    : "화면 모드 적용 중...", {620, 440});
                                        slot->Storage().AddUi(backdrop);
                                        slot->Storage().AddUi(title);
                                        if (modal_phase == GraphicsUiPhase::AwaitingDisplayConfirmation)
                                        {
                                            slot->Storage().AddUi(SettingsConfirmButton("유지", {650, 600}));
                                            slot->Storage().AddUi(SettingsConfirmButton("취소", {1010, 600}));
                                        }
                                    }
                                    if (input_rejected)
                                        slot->Storage().AddUi(SettingsStatusText(
                                            "입력을 처리하지 못했습니다. 잠시 후 다시 눌러 주세요.",
                                            {500, 1000}, 0xFF80C0FFu));
                                    auto content = std::make_shared<UiDrawContent>();
                                    content->models = slot->Storage().TakeUiModels();
                                    draw_content = std::move(content);
                                    draw_facts = std::make_shared<const std::vector<std::uint64_t>>(
                                        ui_projection_facts);
                                }
                                auto ui_frame = std::make_shared<UiFrame>();
                                ui_frame->generation = publish_sequence + 1;
                                ui_frame->session_id = model.session_id;
                                ui_frame->session = model.session;
                                ui_frame->ui = ui_draw_state;
                                ui_frame->settings = projection_settings;
                                ui_frame->graphics_model = graphics_ui;
                                ui_frame->modal_phase = static_cast<std::uint8_t>(modal_phase);
                                ui_frame->pending_rebind_slot = pending_rebind;
                                ui_frame->input_rejected = input_rejected;
                                ui_frame->draw_facts = std::move(draw_facts);
                                ui_frame->draw_content = std::move(draw_content);
                                const auto previous_sample = dynamic_previous.View();
                                std::move(*slot).Publish(publish_sequence + 1,
                                    has_previous_dynamic_sample ? &previous_sample : nullptr, ui_frame);
                                ++publish_sequence;
                                prepared_ui = std::move(ui_frame);
                                snapshot_published = true;
                            }
                            catch (const std::bad_alloc &)
                            {
                                std::move(*slot).Abandon();
                                record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                    "UI frame could not be prepared."));
                                break;
                            }
                            catch (...)
                            {
                                std::move(*slot).Abandon();
                                throw;
                            }
                        }
                        else if (final_smoke_frame || config.smoke_render_lockstep)
                        {
                            record_thread_failure(Result::Failure(
                                ErrorCode::InvalidState, "hs_gameplay",
                                "Smoke render snapshot could not be published."));
                            break;
                        }
                    }
                } // Read borrows end before clearing signals or the next mutation.
                simulation.ClearDomainSignals();
                channels.completed_tick.store(tick.tick, std::memory_order_release);
#ifdef HS_DEVELOPMENT_TOOLS
                notify_smoke();
#endif
                channels.gameplay_checksum.store(tick.checksum, std::memory_order_release);
                const bool new_rendered_session = !rendered_session_barrier ||
                    *rendered_session_barrier != dynamic_current.header.session_id;
                if (realtime && snapshot_published &&
                    (new_rendered_session || (tick.phase == SessionPhase::Playing && !rendered_playing_barrier)))
                {
                    if (native_token) native_input_frame.Simulated(native_token);
                    if (!native_input_frame.WaitPresentedSession(dynamic_current.header.session_id, tick.tick)) break;
                    rendered_session_barrier = dynamic_current.header.session_id;
                    if (new_rendered_session) rendered_playing_barrier = false;
                    if (tick.phase == SessionPhase::Playing) rendered_playing_barrier = true;
                    clock.Realign(FixedStepClock::Clock::now());
                    simulation_debt = {};
                    stop_remaining_admitted_steps = true;
                }
                if (config.smoke_render_lockstep && !final_smoke_frame)
                {
#ifdef HS_DEVELOPMENT_TOOLS
                    std::unique_lock lock(smoke_mutex);
                    smoke_cv.wait(lock, [&] {
                        return rendered_tick.load(std::memory_order_acquire) >= tick.tick ||
                            channels.stop_requested.load(std::memory_order_acquire);
                    });
#endif
                    if (channels.stop_requested.load(std::memory_order_acquire))
                        break;
                }
                if (tick.phase == SessionPhase::QuitRequested)
                {
                    request_stop();
                    break;
                }

                // TickFixed does not advance a terminal gameplay clock.
#ifdef HS_DEVELOPMENT_TOOLS
                if (final_smoke_frame)
                {
                    smoke_finished = true;
                    break;
                }
#endif
            }

            if (native_token) native_input_frame.Simulated(native_token, simulation_debt);
#ifdef HS_DEVELOPMENT_TOOLS
            if (smoke_finished)
            {
                break;
            }
#endif
        }
        if (auto shutdown = simulation.Shutdown(); !shutdown)
        {
            record_thread_failure(shutdown);
        }
        native_input_frame.Cancel();
        finish_simulation();
    });

    bool display_start_deferred{};
    bool display_previous_borderless{};
    GraphicsSettings display_previous_graphics = window_display_graphics;
    GraphicsExtent display_previous_windowed_extent = windowed_client_extent;
    GraphicsDisplayId display_previous_native_output;
    bool display_restoring{};
    std::uint64_t display_generation{};
    auto display_confirmation_deadline = std::chrono::steady_clock::time_point{};
    const auto capture_display_placement = [&] {
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (!GetMonitorInfoW(MonitorFromWindow(window.Handle(), MONITOR_DEFAULTTONEAREST), &monitor))
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                   "Cannot capture the current display for rollback.");
        std::vector<RendererDisplayOutput> displays;
        if (auto enumerated = EnumerateRendererDisplays(displays); !enumerated) return enumerated;
        const auto selected = std::ranges::find_if(displays, [&](const auto &output) {
            return output.left == monitor.rcMonitor.left && output.top == monitor.rcMonitor.top &&
                static_cast<std::int64_t>(output.left) + output.width == monitor.rcMonitor.right &&
                static_cast<std::int64_t>(output.top) + output.height == monitor.rcMonitor.bottom;
        });
        if (selected == displays.end())
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                   "The current window display is not exposed by DXGI.");
        display_previous_native_output.value = selected->id;
        display_previous_borderless = window_borderless;
        display_previous_graphics = window_display_graphics;
        display_previous_graphics.display_mode = window_borderless
            ? GraphicsDisplayMode::BorderlessFullscreen : GraphicsDisplayMode::Windowed;
        display_previous_windowed_extent = windowed_client_extent;
        return Result::Success();
    };
    const auto set_desired_client_extent = [&] {
        RECT client{};
        if (!GetClientRect(window.Handle(), &client))
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                   "Cannot query display confirmation client extent.");
        if (client.right <= client.left || client.bottom <= client.top)
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                   "Display confirmation client extent is empty.");
        settings_commit.SetDesiredOutputExtent(
            static_cast<std::uint32_t>(client.right - client.left),
            static_cast<std::uint32_t>(client.bottom - client.top));
        return Result::Success();
    };
    const auto rollback_window_placement = [&] {
        // Requested Auto remains Auto in the document, but rollback must restore
        // the monitor captured before the move, not the new current monitor.
        GraphicsDisplayId restored_output;
        if (auto resolved = ResolveGraphicsDisplayOutput(window.Handle(),
                                                         display_previous_native_output,
                                                         restored_output); !resolved)
        {
            record_thread_failure(resolved);
            return false;
        }
        auto restored_graphics = display_previous_graphics;
        auto restored_tracking = restored_output;
        if (auto restored = window.ApplyDisplayPlacement(restored_output,
                display_previous_borderless, display_previous_windowed_extent.width,
                display_previous_windowed_extent.height); !restored)
        {
            record_thread_failure(restored);
            return false;
        }
        window_display_graphics = std::move(restored_graphics);
        window_native_output = std::move(restored_tracking);
        windowed_client_extent = display_previous_windowed_extent;
        window_borderless = display_previous_borderless;
        if (auto extent = set_desired_client_extent(); !extent)
        {
            record_thread_failure(extent);
            return false;
        }
        return true;
    };
    const auto restore_display_mode = [&] {
        if (!rollback_window_placement()) return false;
        settings_commit.RestoreDisplay(display_previous_graphics);
        display_restoring = true;
        display_generation = 0;
        display_modal_phase.store(GraphicsUiPhase::RestoringDisplay,
                                  std::memory_order_release);
        graphics_ui_phase.store(GraphicsUiPhase::RestoringDisplay,
                                 std::memory_order_release);
        return true;
    };
    std::shared_ptr<const GraphicsSettingsUiModel> last_renderer_receipt;
    auto last_published_phase = GraphicsUiPhase::Applied;
    bool last_published_pending{}, last_published_save_failure{};
    auto settings_request_deadline = std::chrono::steady_clock::time_point{};
    while (!channels.stop_requested.load(std::memory_order_acquire))
    {
        const auto processed_wake_sequence = main_wake.Sequence();
        ui_click_settings = settings_commit.UiInput();
        std::uint64_t sampled_token{};
        Result window_failure = Result::Success();
        const bool sample_input = native_input_frame.ShouldPump(sampled_token);
        if (!window.PumpMessages(&window_failure, sample_input))
        {
            if (!window_failure) record_thread_failure(std::move(window_failure));
            request_stop();
            break;
        }
#ifdef HS_DEVELOPMENT_TOOLS
        if (config.smoke_render_lockstep && channels.stop_requested.load(std::memory_order_acquire))
        {
            request_stop();
            break;
        }
#endif
        if (sampled_token) native_input_frame.Sampled(sampled_token);
        if (ui_command_queue_full)
        {
#ifdef HS_DEVELOPMENT_TOOLS
            OutputDebugStringA("UI command rejected: queue full. Retry with a new input.\n");
#endif
            ui_command_queue_full = false;
        }
        audio.SetBackgroundMuted(GetForegroundWindow() != window.Handle());
        bool presentation_settings_changed = false;
        GraphicsSettingsAck settings_ack;
        while (channels.renderer_settings_acks.TryPop(settings_ack))
        {
            const auto display_ack = display_generation != 0 &&
                                     settings_ack.generation == display_generation;
            const auto recovery_ack = settings_commit.IsRecovering();
            const auto outcome = settings_commit.Acknowledge(settings_ack);
            if (outcome == GraphicsSettingsCommit::AckOutcome::Ignored)
                continue;
            presentation_settings_changed = true;
            if (outcome == GraphicsSettingsCommit::AckOutcome::Recovering)
            {
                if (display_ack) { display_generation = 0; (void)rollback_window_placement(); }
                graphics_ui_phase.store(GraphicsUiPhase::RestoringDisplay, std::memory_order_release);
                continue;
            }
            if (outcome == GraphicsSettingsCommit::AckOutcome::Restored)
            {
                display_generation = 0;
                display_restoring = false;
                display_modal_phase.store(GraphicsUiPhase::Applied, std::memory_order_release);
                graphics_ui_phase.store(GraphicsUiPhase::ApplyFailed, std::memory_order_release);
                continue;
            }
            if (outcome == GraphicsSettingsCommit::AckOutcome::Rejected && recovery_ack)
            {
                record_thread_failure(Result::Failure(settings_ack.error, "hs_runtime",
                    "Renderer could not restore the last persisted graphics profile."));
                break;
            }
            if (outcome == GraphicsSettingsCommit::AckOutcome::Applied)
            {
                if (display_ack)
                {
                    display_generation = 0;
                    if (display_restoring)
                    {
                        display_restoring = false;
                        display_modal_phase.store(GraphicsUiPhase::Applied,
                                                  std::memory_order_release);
                        graphics_ui_phase.store(GraphicsUiPhase::Applied,
                                                std::memory_order_release);
                        save_committed_settings();
                    }
                    else
                    {
                        display_confirmation_deadline =
                            std::chrono::steady_clock::now() + std::chrono::seconds(15);
                        display_modal_phase.store(
                            GraphicsUiPhase::AwaitingDisplayConfirmation,
                            std::memory_order_release);
                        graphics_ui_phase.store(
                            GraphicsUiPhase::AwaitingDisplayConfirmation,
                            std::memory_order_release);
                    }
                }
                else if (const auto modal = display_modal_phase.load(
                             std::memory_order_acquire);
                         modal == GraphicsUiPhase::Applied ||
                         (modal == GraphicsUiPhase::Pending && display_start_deferred))
                {
                    graphics_ui_phase.store(GraphicsUiPhase::Applied,
                                            std::memory_order_release);
                    // This ack belongs to the earlier graphics request. The
                    // deferred display switch has not touched the window yet.
                    save_committed_settings();
                }
            }
            else if (display_ack)
            {
                display_generation = 0;
                if (display_restoring)
                {
                    display_restoring = false;
                    display_modal_phase.store(GraphicsUiPhase::Applied,
                                              std::memory_order_release);
                    graphics_ui_phase.store(GraphicsUiPhase::ApplyFailed,
                                            std::memory_order_release);
                }
                else
                    (void)restore_display_mode();
            }
            else if (display_modal_phase.load(std::memory_order_acquire) ==
                     GraphicsUiPhase::Applied)
                graphics_ui_phase.store(GraphicsUiPhase::ApplyFailed,
                                        std::memory_order_release);
        }
        UiCommand ui_command;
        bool audio_settings_changed{};
        while (channels.ui_commands.TryPop(ui_command))
        {
            switch (ui_command.kind)
            {
                case UiCommandKind::SetBorderless:
                    if (display_modal_phase.load(std::memory_order_acquire) ==
                        GraphicsUiPhase::Applied)
                    {
                        if (auto captured = capture_display_placement(); !captured)
                        {
#ifdef HS_DEVELOPMENT_TOOLS
                            OutputDebugStringA(std::format("Display placement capture failed: {}\n",
                                                          captured.Message()).c_str());
#endif
                            graphics_ui_phase.store(GraphicsUiPhase::ApplyFailed, std::memory_order_release);
                            presentation_settings_changed = true;
                            break;
                        }
                        display_start_deferred = true;
                        display_modal_phase.store(GraphicsUiPhase::Pending,
                                                  std::memory_order_release);
                        settings_commit.EditGraphics(ui_command);
                        graphics_ui_phase.store(GraphicsUiPhase::Requested,
                                                std::memory_order_release);
                        presentation_settings_changed = true;
                    }
                    break;
                case UiCommandKind::SetVsync:
                case UiCommandKind::SetFrameCap:
                case UiCommandKind::SetRenderScale:
                case UiCommandKind::SetShadowResolution:
                case UiCommandKind::SetParticlePercentage:
                case UiCommandKind::SetBloom:
                case UiCommandKind::SetGraphicsSetting:
                case UiCommandKind::SetVfxScreenOverlayIntensityPercent:
                    graphics_ui_phase.store((ui_command.kind != UiCommandKind::SetGraphicsSetting ||
                        GraphicsSettingTokenAvailable(ui_command.graphics_setting,
                            std::string_view(ui_command.graphics_token.data(),
                                std::ranges::find(ui_command.graphics_token, '\0') - ui_command.graphics_token.begin()), &ui_click_model)) &&
                        settings_commit.EditGraphics(ui_command)
                        ? GraphicsUiPhase::Requested : GraphicsUiPhase::ApplyFailed,
                                            std::memory_order_release);
                    presentation_settings_changed = true;
                    break;
                case UiCommandKind::SetMasterVolumePercent:
                case UiCommandKind::SetBgmVolumePercent:
                case UiCommandKind::SetSfxVolumePercent:
                case UiCommandKind::SetUiVolumePercent:
                    if (settings_commit.EditImmediate(ui_command))
                    {
                        audio_settings_changed = true;
                        presentation_settings_changed = true;
                    }
                    break;
                case UiCommandKind::BeginSkillRebind:
                    window.BeginSkillRebind(ui_command.value);
                    channels.pending_rebind_slot.store(
                        static_cast<std::uint8_t>(ui_command.value),
                        std::memory_order_release);
                    break;
                case UiCommandKind::CancelSkillRebind:
                    window.CancelSkillRebind();
                    channels.pending_rebind_slot.store(0xFF, std::memory_order_release);
                    break;
            }
        }
        if (audio_settings_changed)
        {
            audio.ApplySettings(settings);
            if (display_modal_phase.load(std::memory_order_acquire) ==
                GraphicsUiPhase::Applied)
                save_committed_settings();
        }
        std::array<std::uint16_t, 4> rebound_keys;
        if (window.ConsumeReboundSkillKeys(rebound_keys))
        {
            audio.Play(MakeAudioEvent("audio.ui.rebind_success", ++ui_audio_sequence));
            channels.pending_rebind_slot.store(0xFF, std::memory_order_release);
            settings_commit.RebindKeys(rebound_keys);
            presentation_settings_changed = true;
            if (display_modal_phase.load(std::memory_order_acquire) ==
                GraphicsUiPhase::Applied)
                save_committed_settings();
        }
        if (display_modal_phase.load(std::memory_order_acquire) ==
            GraphicsUiPhase::AwaitingDisplayConfirmation)
        {
            const auto expired = std::chrono::steady_clock::now() >=
                                 display_confirmation_deadline;
            if (display_confirm_clicked && !expired)
            {
                display_modal_phase.store(GraphicsUiPhase::Applied,
                                          std::memory_order_release);
                graphics_ui_phase.store(GraphicsUiPhase::Applied,
                                        std::memory_order_release);
                save_committed_settings();
            }
            else if (display_cancel_clicked || expired)
            {
                (void)restore_display_mode();
                presentation_settings_changed = true;
            }
        }
        display_confirm_clicked = false;
        display_cancel_clicked = false;
        if (settings_commit.HasPending() && !settings_commit.TimedOut() &&
            std::chrono::steady_clock::now() >= settings_request_deadline)
        {
            settings_commit.MarkTimedOut();
            graphics_ui_phase.store(GraphicsUiPhase::ApplyFailed, std::memory_order_release);
            presentation_settings_changed = true;
        }
        if (display_start_deferred && !settings_commit.HasPending())
        {
            const auto target = settings_commit.UiInput().borderless;
            display_start_deferred = false;
            if (target == display_previous_borderless)
                display_modal_phase.store(GraphicsUiPhase::Applied,
                                          std::memory_order_release);
            presentation_settings_changed = true;
        }
        if (!settings_commit.HasPending() && settings_commit.HasUnsentEdits())
        {
                const auto &requested = settings_commit.ApplyTargetGraphics();
                const auto target_borderless = settings_commit.ApplyTargetEffective().borderless;
                GraphicsDisplayId native_output;
                const auto resolved_output = ResolveGraphicsDisplayOutput(window.Handle(),
                                                                          requested.display_output,
                                                                          native_output);
                if (!resolved_output)
                {
                    settings_commit.DiscardUnsentEdits();
                    graphics_ui_phase.store(GraphicsUiPhase::ApplyFailed, std::memory_order_release);
                    presentation_settings_changed = true;
                }
                if (requested.display_output != window_display_graphics.display_output ||
                    requested.output_extent != window_display_graphics.output_extent ||
                    target_borderless != window_borderless || (resolved_output && native_output != window_native_output))
                {
                    if (display_modal_phase.load(std::memory_order_acquire) == GraphicsUiPhase::Applied)
                    {
                        if (auto captured = capture_display_placement(); !captured)
                        {
#ifdef HS_DEVELOPMENT_TOOLS
                            OutputDebugStringA(std::format("Display placement capture failed: {}\n",
                                                          captured.Message()).c_str());
#endif
                            settings_commit.DiscardUnsentEdits();
                            graphics_ui_phase.store(GraphicsUiPhase::ApplyFailed, std::memory_order_release);
                            presentation_settings_changed = true;
                        }
                        else
                            display_modal_phase.store(GraphicsUiPhase::Pending, std::memory_order_release);
                    }
                    if (settings_commit.HasUnsentEdits())
                    {
                        auto next_window_graphics = requested;
                        auto next_native_output = native_output;
                        const auto target_extent = requested.output_extent.value_or(windowed_client_extent);
                        auto placed = resolved_output;
                        if (placed) placed = window.ApplyDisplayPlacement(native_output, target_borderless,
                                                                         target_extent.width, target_extent.height);
                        if (placed) placed = set_desired_client_extent();
                        if (!placed)
                        {
#ifdef HS_DEVELOPMENT_TOOLS
                            OutputDebugStringA(std::format("Display placement failed: {}\n", placed.Message()).c_str());
#endif
                            (void)rollback_window_placement();
                            settings_commit.DiscardUnsentEdits();
                            display_modal_phase.store(GraphicsUiPhase::Applied, std::memory_order_release);
                            graphics_ui_phase.store(GraphicsUiPhase::ApplyFailed, std::memory_order_release);
                        }
                        else
                        {
                            window_display_graphics = std::move(next_window_graphics);
                            window_native_output = std::move(next_native_output);
                            windowed_client_extent = target_extent;
                            window_borderless = target_borderless;
                        }
                    }
                    presentation_settings_changed = true;
                }
        }
        if (presentation_settings_changed &&
            !channels.presentation_settings.TryPush(settings_commit.UiInput()))
            record_thread_failure(Result::Failure(
                ErrorCode::InvalidState, "hs_runtime",
                "Presentation settings queue capacity exceeded."));
        const auto next_settings_request = settings_commit.NextRequest();
        if (const auto &request = next_settings_request)
        {
            if (!channels.renderer_settings.TryPush(*request))
            {
                settings_commit.RejectUnqueuedRequest();
                const auto modal = display_modal_phase.load(std::memory_order_acquire);
                if ((modal == GraphicsUiPhase::Pending && !display_start_deferred) ||
                    modal == GraphicsUiPhase::RestoringDisplay)
                    (void)rollback_window_placement();
                record_thread_failure(Result::Failure(
                    ErrorCode::InvalidState, "hs_runtime",
                    "Renderer settings request queue capacity exceeded."));
            }
            else
            {
                settings_request_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                const auto modal = display_modal_phase.load(std::memory_order_acquire);
                if ((modal == GraphicsUiPhase::Pending && !display_start_deferred) ||
                    modal == GraphicsUiPhase::RestoringDisplay)
                    display_generation = request->generation;
                graphics_ui_phase.store(
                    display_restoring ? GraphicsUiPhase::RestoringDisplay
                                      : GraphicsUiPhase::Pending,
                    std::memory_order_release);
            }
        }
        const auto receipt = channels.renderer_graphics_ui.load(std::memory_order_acquire);
        const auto phase = graphics_ui_phase.load(std::memory_order_acquire);
        const auto pending = settings_commit.HasPending();
        const auto save_failure = settings_save_failed.load(std::memory_order_acquire);
        if (presentation_settings_changed || receipt != last_renderer_receipt ||
            phase != last_published_phase || pending != last_published_pending ||
            save_failure != last_published_save_failure)
        {
                auto model = receipt ? std::make_shared<GraphicsSettingsUiModel>(*receipt)
                                     : std::make_shared<GraphicsSettingsUiModel>();
                model->requested = settings_commit.Requested().graphics;
                model->pending = pending || settings_commit.HasUnsentEdits();
                const auto state = settings_commit.TimedOut() ? "적용 시간이 초과되어 장치 작업 완료 후 복원합니다"
                    : settings_commit.IsRecovering() ? "마지막으로 저장한 설정을 복원하는 중입니다"
                    : phase == GraphicsUiPhase::Requested ? "설정 변경을 요청했습니다"
                    : phase == GraphicsUiPhase::Pending ? "설정을 적용하는 중입니다"
                    : phase == GraphicsUiPhase::AwaitingDisplayConfirmation ? "화면 변경을 15초 안에 확인해 주세요"
                    : phase == GraphicsUiPhase::RestoringDisplay ? "이전 화면 설정을 복원하는 중입니다"
                    : phase == GraphicsUiPhase::ApplyFailed ? "적용할 수 없어 이전 설정을 유지합니다"
                    : receipt ? "적용 및 저장 완료 · 값이나 − / + 버튼으로 변경하세요" : "렌더러 상태를 확인하는 중입니다";
                model->transaction_state = std::string(state) + (save_failure ? " · 저장 실패: 적용 상태는 유지됩니다" : "");
                if (receipt && receipt->generation != settings_commit.CommittedGeneration())
                    model->transaction_state += " · 새 적용 결과를 기다리는 중입니다";
                ui_click_document = settings_commit.Requested();
                ui_click_settings = settings_commit.UiInput();
                ui_click_model = *model;
                channels.presentation_graphics_ui.store(std::move(model), std::memory_order_release);
            last_renderer_receipt = receipt;
            last_published_phase = phase;
            last_published_pending = pending;
            last_published_save_failure = save_failure;
        }
        main_audio->Drain(channels.sim_audio_events);
        main_audio->Update();
        if (main_audio->Failure() != runtime_detail::MainAudioFailure::None)
        {
            const auto failure = audio.Status();
            const auto &detail = main_audio->FailureDetails();
            record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_audio",
                std::format("Audio delivery failed (boundary={}, cue_id={}, readiness={}, play_result={}, session={}, tick={}, phase={}, stage={}, loaded_files={}, load_failures={}, native_operation={}, HRESULT={}, cleanup_operation={}, cleanup_HRESULT={}).",
                    static_cast<unsigned>(main_audio->Failure()), detail.cue.value, static_cast<unsigned>(detail.readiness),
                    static_cast<unsigned>(detail.play_result), detail.session_id, detail.tick, static_cast<unsigned>(detail.phase), detail.stage,
                    failure.loaded_files, failure.skipped_load_failures, static_cast<unsigned>(failure.native_operation),
                    failure.native_error, static_cast<unsigned>(failure.native_cleanup_operation), failure.native_cleanup_error)));
        }
        DWORD timeout = INFINITE;
        const auto include_deadline = [&](std::chrono::steady_clock::time_point deadline) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
            timeout = std::min(timeout, milliseconds <= 0 ? 0u : static_cast<DWORD>(std::min<std::int64_t>(milliseconds, INFINITE - 1ull)));
        };
        if (display_modal_phase.load(std::memory_order_acquire) == GraphicsUiPhase::AwaitingDisplayConfirmation)
            include_deadline(display_confirmation_deadline);
        if (settings_commit.HasPending() && !settings_commit.TimedOut()) include_deadline(settings_request_deadline);
        const auto pending_main_work = [&]() noexcept {
            MSG pending{};
            const auto input_gated = native_input_frame.InputGated();
            const bool messages = runtime_detail::PeekMainMessage(pending, !input_gated, PM_NOREMOVE);
            return messages || channels.stop_requested.load(std::memory_order_acquire) ||
                channels.sim_audio_events.Size() != 0 ||
                channels.renderer_settings_acks.Size() != 0 ||
                channels.ui_commands.Size() != 0 || native_input_frame.MainWorkPending() ||
                channels.renderer_graphics_ui.load(std::memory_order_acquire) != last_renderer_receipt;
        };
        if (main_wake.PrepareWait(pending_main_work, processed_wake_sequence))
        {
            const auto generation = main_wake.Sequence();
            // Recheck immediately before entering the OS wait. A later successful
            // producer push sets the manual event and cannot be lost by this wait.
            if (!pending_main_work() && main_wake.Sequence() == generation)
            {
                const auto handle = main_wake.Handle();
                const auto input_gated = native_input_frame.InputGated();
                const auto wait_result = MsgWaitForMultipleObjectsEx(1, &handle, timeout,
                    runtime_detail::MainMessageMask(input_gated),
                    runtime_detail::MainMessageFlags(input_gated));
                const auto wait_error = wait_result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
                if (wait_result == WAIT_FAILED)
                    record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                        std::format("Main message/event wait failed: {}.", wait_error)));
            }
        }
        if (const auto error = main_wake.Error())
            record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                std::format("Main wake event failed: {}.", error)));
    }
    if (ui_callback_out_of_memory) throw std::bad_alloc{};
    }
    catch (const std::bad_alloc &)
    {
        record_thread_failure(Result::Failure(ErrorCode::TaskFailed, "hs_runtime", "Out of memory"));
    }
    window.StopInput();
    request_stop();
    if (simulation_thread.joinable()) simulation_thread.join();
    else finish_simulation();
    // GraphicsSettingsCommit owns any in-flight request snapshot. Join before
    // that owner can be destroyed, including shutdown with an unconsumed ack.
    render_thread.join();
    audio.Shutdown();

    if (!config.smoke)
    {
        profile.best_level = channels.best_level;
        profile.total_wins += channels.completed_run_wins;
        profile.total_kills += channels.completed_run_kills;
        if (auto saved = save_store.SaveProfile(profile); !saved)
        {
            record_thread_failure(saved);
        }
    }

    {
        std::lock_guard lock(result_mutex);
        result = thread_result;
    }
    const auto final_tick = channels.completed_tick.load(std::memory_order_acquire);
    const auto checksum = channels.gameplay_checksum.load(std::memory_order_acquire);
    const auto rendered_frames = channels.rendered_frames.load(std::memory_order_acquire);
#ifdef HS_DEVELOPMENT_TOOLS
    const auto rendered_particles = channels.rendered_particles;
    auto valid =
        result && final_tick >= (config.smoke ? config.maximum_ticks : 0) && rendered_frames > 0 &&
        rendered_particles <= 10'000u &&
        channels.dropped_input_edges.load() == 0 &&
        channels.dropped_presentation_events.load() == 0;
#endif
#ifdef HS_DEVELOPMENT_TOOLS
    if (playtest_recorder.Active())
    {
        if (auto finished = playtest_recorder.Finish(valid); !finished)
        {
            result = finished;
            valid = false;
        }
    }
#endif

#ifdef HS_DEVELOPMENT_TOOLS
    if (capture_diagnostics)
    {
        WriteText(artifact_directory / "result.json",
                  std::format("{{\"schema_version\":1,\"scenario_id\":\"runtime-smoke\","
                              "\"seed\":{},\"maximum_ticks\":{},"
                              "\"mode\":\"offscreen_render\",\"execution_valid\":{},"
                              "\"assertions_passed\":{},"
                              "\"tick\":{},\"checksum\":{},\"rendered_frames\":{},"
                              "\"rendered_particles\":{},"
                              "\"content_hash\":{},\"generation_id\":\"{}\","
                              "\"worker_count\":{},"
                              "\"dropped_input_edges\":{},\"dropped_presentation_events\":{},"
                              "\"particle_effects_hash\":{}}}\n",
                              config.seed, config.maximum_ticks,
                              valid ? "true" : "false", valid ? "true" : "false", final_tick,
                              checksum, rendered_frames, rendered_particles, content_hash, deployment_id,
                              task_system.WorkerCount(),
                              channels.dropped_input_edges.load(),
                              channels.dropped_presentation_events.load(),
                              vfx_catalog.PayloadHash()));
    }
#endif

#ifdef HS_DEVELOPMENT_TOOLS
    if (!valid && result)
    {
        result = Result::Failure(ErrorCode::InvalidState, "hs_runtime",
            std::format("Runtime execution assertions failed (tick={}, frames={}, particles={}, dropped input={}, dropped events={}).",
                final_tick, rendered_frames, rendered_particles,
                channels.dropped_input_edges.load(), channels.dropped_presentation_events.load()));
    }
#endif
#ifdef HS_DEVELOPMENT_TOOLS
    return {result, final_tick, checksum, rendered_frames, playtest_recorder.Directory()};
#else
    return {result, final_tick, checksum, rendered_frames, {}};
#endif
}

} // namespace hs
