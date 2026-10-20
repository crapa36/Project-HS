#pragma once

#include <hs/core/presentation_event.hpp>
#include <hs/core/result.hpp>
#include <hs/core/types.hpp>
#include <hs/runtime/save_store.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace hs
{

enum class AudioBus : std::uint8_t
{
    Master,
    Bgm,
    Sfx,
    Ui,
};

enum class AudioPriority : std::uint8_t
{
    Other,
    Enemy,
    Player,
    BossWarning,
    Ui,
};

enum class AudioDeviceState : std::uint8_t
{
    Uninitialized,
    Ready,
    SilentUnavailable,
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
    [[nodiscard]] Result Initialize(const SettingsData &settings,
                                    const std::filesystem::path &audio_directory);
    [[nodiscard]] Result Initialize(
        const SettingsData &settings,
        const std::unordered_map<std::string, std::filesystem::path> &product_paths);
    void Shutdown() noexcept;
    void ApplySettings(const SettingsData &settings) noexcept;
    void SetBackgroundMuted(bool muted) noexcept;

    void Play(const PresentationEvent &event) noexcept;
    void Stop(AssetId cue) noexcept;
    void StopBus(AudioBus bus) noexcept;
    void UpdateListener(Float3 position, Float3 forward, Float3 up) noexcept;
    void PauseCombat() noexcept;
    void ResumeCombat() noexcept;

    [[nodiscard]] AudioStatus Status() const noexcept;

  private:
    using ResolveAudioPath =
        std::function<Result(std::string_view, std::filesystem::path &)>;
    [[nodiscard]] Result InitializeResolved(const SettingsData &settings,
                                             const ResolveAudioPath &resolve_path);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hs
