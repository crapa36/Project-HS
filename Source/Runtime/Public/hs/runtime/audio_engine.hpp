#pragma once

#include <hs/core/presentation_event.hpp>
#include <hs/core/result.hpp>
#include <hs/core/types.hpp>
#include <hs/runtime/save_store.hpp>
#include <hs/runtime/audio_image.hpp>

#include <cstdint>
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hs
{

class ShippedContentOwner;
namespace audio_detail { struct DeveloperAudioCatalog; }

enum class AudioDeviceState : std::uint8_t
{
    Uninitialized,
    Ready,
    SilentUnavailable,
};

enum class AudioNativeOperation : std::uint8_t
{
    None, CreateVoice, Stop, Flush, Frequency, Route, Matrix, Submit, Start, Volume, Callback
};
enum class AudioCueReadiness : std::uint8_t { Missing, Pending, Ready, Failed };
enum class AudioPlayResult : std::uint8_t
{
    Ignored, MissingCue, Retriggered, Limited, NotReady, Silent, NativeFailure, Started, QueuedPaused
};

struct AudioStatus
{
    AudioDeviceState device_state{AudioDeviceState::Uninitialized};
    bool combat_paused{};
    bool background_muted{};
    float effective_master_volume{1.0f};
    std::uint32_t active_source_voices{};
    std::uint64_t skipped_missing_cues{};
    std::uint64_t skipped_load_failures{};
    std::uint64_t skipped_voice_limits{};
    std::uint32_t loaded_cues{};
    std::uint32_t loaded_files{};
    std::uint64_t played_count{};
    std::uint32_t native_error{};
    AudioNativeOperation native_operation{AudioNativeOperation::None};
    std::uint32_t native_cleanup_error{};
    AudioNativeOperation native_cleanup_operation{AudioNativeOperation::None};
};

enum class AudioWorkId : std::uint8_t
{
    // Spatial measures actual X3DAudio calculations; NativeVoice includes matrix application.
    PrepareCue, Play, Stop, Listener, Reclaim, Evict, Admission, Spatial, NativeVoice, InitializeDevice, Count
};
inline constexpr std::array<std::string_view, static_cast<std::size_t>(AudioWorkId::Count)> kAudioWorkNames{
    "prepare", "play", "stop", "listener", "reclaim", "evict", "admission", "spatial", "native_voice", "initialize_device"};
struct AudioWorkSample { std::uint64_t ticks{}, calls{}; };
// Cumulative QPC wall time; nested scopes must not be added to their parent.
// Native latency is XAudio2's estimate, not a measured physical output timestamp.
struct AudioWorkTimings
{
    bool captured{}, native_available{};
    std::uint64_t qpc_frequency{};
    std::array<AudioWorkSample, kAudioWorkNames.size()> scopes{};
    std::uint64_t reader_jobs{}, reader_failures{}, reader_bytes{};
    std::uint64_t reader_read_ticks{}, reader_queue_ticks{}, reader_request_ready_ticks{};
    std::uint64_t drain_submit_ticks{}, drain_submit_max_ticks{}, drain_submit_calls{};
    std::uint64_t native_sample_qpc{}, native_samples{}, audio_cycles{}, total_cycles{};
    std::uint32_t minimum_cycles_per_quantum{}, maximum_cycles_per_quantum{};
    std::uint32_t native_active_voices{}, native_total_voices{}, glitches{}, latency_samples{}, output_sample_rate{};
};

class AudioEngine
{
  public:
    static constexpr std::uint32_t kMaximumSourceVoices = 64;

    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine &) = delete;
    AudioEngine &operator=(const AudioEngine &) = delete;

    [[nodiscard]] Result Initialize(const SettingsData &settings);
    // Image and backing owner outlive this engine; Shutdown joins pending reads.
    [[nodiscard]] Result Initialize(const SettingsData &settings, const AudioImage &image,
                                    const ShippedContentOwner &owner);
    // Main-thread request; no waiting or disk I/O. Ready means every cue variant is prepared.
    [[nodiscard]] bool PrepareCue(AssetId cue) noexcept;
    [[nodiscard]] AudioCueReadiness RequestCue(AssetId cue) noexcept;
    [[nodiscard]] AudioCueReadiness CueReadiness(AssetId cue) const noexcept;
    [[nodiscard]] bool IsTransitionTrack(AssetId cue) const noexcept;
    [[nodiscard]] Result Initialize(const SettingsData &settings,
                                    const std::filesystem::path &audio_directory);
    [[nodiscard]] Result Initialize(
        const SettingsData &settings,
        const std::unordered_map<std::string, std::filesystem::path> &product_paths);
    void Shutdown() noexcept;
    void ApplySettings(const SettingsData &settings) noexcept;
    void SetBackgroundMuted(bool muted) noexcept;

    AudioPlayResult Play(const PresentationEvent &event, std::uint64_t dequeued_qpc = 0) noexcept;
    void Stop(AssetId cue) noexcept;
    void StopBus(AudioBus bus) noexcept;
    void UpdateListener(Float3 position, Float3 forward, Float3 up) noexcept;
    void PauseCombat() noexcept;
    void ResumeCombat() noexcept;
    // Main closes old scoped event voices; its music owner retains the old track until ready.
    void BeginSession(std::uint64_t session_id) noexcept;

    [[nodiscard]] AudioStatus Status() const noexcept;
    [[nodiscard]] AudioNativeOperation FailedNativeOperation() const noexcept;
    // Main-thread opt-in. Worker counters are published atomically; no per-frame native query.
    void SetCaptureTimings(bool enabled) noexcept;
    void SetWakeCallback(void (*notify)(void *) noexcept, void *context) noexcept;
    [[nodiscard]] AudioWorkTimings CaptureTimings() noexcept;

  private:
    using ResolveAudioPath =
        std::function<Result(std::string_view, std::filesystem::path &)>;
    [[nodiscard]] Result InitializeResolved(const SettingsData &settings,
                                             const ResolveAudioPath &resolve_path);
    [[nodiscard]] Result InitializeDevice(const SettingsData &settings);
    [[nodiscard]] Result InitializePlayback(const SettingsData &settings);
    [[nodiscard]] Result InitializePrepared(const SettingsData &settings, audio_detail::DeveloperAudioCatalog catalog);
    void RetainGeneration(std::shared_ptr<const void> generation);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hs
