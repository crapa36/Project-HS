#pragma once

#include <hs/core/presentation_event.hpp>
#include <hs/core/result.hpp>
#include <hs/core/types.hpp>
#include <hs/runtime/save_store.hpp>
#include <hs/runtime/audio_image.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hs
{

class ShippedContentOwner;
namespace audio_detail { struct PreparedAudio; }
[[nodiscard]] Result PrepareAudio(const AudioImage &, const ShippedContentOwner &,
                                  std::shared_ptr<audio_detail::PreparedAudio> &);
[[nodiscard]] Result PrepareAudio(const std::unordered_map<std::string, std::filesystem::path> &,
                                  std::shared_ptr<audio_detail::PreparedAudio> &);

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
    std::uint64_t skipped_load_failures{};
    std::uint32_t loaded_files{};
    std::uint32_t native_error{};
    AudioNativeOperation native_operation{AudioNativeOperation::None};
    std::uint32_t native_cleanup_error{};
    AudioNativeOperation native_cleanup_operation{AudioNativeOperation::None};
};

class AudioEngine
{
  public:
    static constexpr std::uint32_t kMaximumSourceVoices = 64;

    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine &) = delete;
    AudioEngine &operator=(const AudioEngine &) = delete;

    [[nodiscard]] Result Initialize(const SettingsData &settings,
                                    std::shared_ptr<audio_detail::PreparedAudio> prepared,
                                    std::shared_ptr<const void> owner);
    // Main-thread request; no waiting or disk I/O. Ready means every cue variant is prepared.
    [[nodiscard]] bool PrepareCue(AssetId cue) noexcept;
    [[nodiscard]] AudioCueReadiness RequestCue(AssetId cue) noexcept;
    [[nodiscard]] AudioCueReadiness CueReadiness(AssetId cue) const noexcept;
    [[nodiscard]] bool IsTransitionTrack(AssetId cue) const noexcept;
    void Shutdown() noexcept;
    void ApplySettings(const SettingsData &settings) noexcept;
    void SetBackgroundMuted(bool muted) noexcept;

    AudioPlayResult Play(const PresentationEvent &event) noexcept;
    void Stop(AssetId cue) noexcept;
    void UpdateListener(Float3 position) noexcept;
    void PauseCombat() noexcept;
    void ResumeCombat() noexcept;
    // Main closes old scoped event voices; its music owner retains the old track until ready.
    void BeginSession(std::uint64_t session_id) noexcept;

    [[nodiscard]] AudioStatus Status() const noexcept;
    [[nodiscard]] AudioNativeOperation FailedNativeOperation() const noexcept;
    void SetWakeCallback(void (*notify)(void *) noexcept, void *context) noexcept;

  private:
    [[nodiscard]] Result InitializeDevice(const SettingsData &settings);
    [[nodiscard]] Result InitializePlayback(const SettingsData &settings);
    [[nodiscard]] Result InitializePrepared(const SettingsData &settings, audio_detail::PreparedAudio catalog);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hs
