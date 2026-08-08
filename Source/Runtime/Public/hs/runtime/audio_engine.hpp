#pragma once

#include <hs/core/presentation_event.hpp>
#include <hs/core/result.hpp>
#include <hs/core/types.hpp>
#include <hs/runtime/save_store.hpp>

#include <cstdint>
#include <memory>

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
    Ui = BossWarning,
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
    std::uint32_t active_source_voices{};
    std::uint64_t skipped_missing_cues{};
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
    void Shutdown() noexcept;
    void ApplySettings(const SettingsData &settings) noexcept;

    void Play(const PresentationEvent &event, AudioBus bus = AudioBus::Sfx,
              AudioPriority priority = AudioPriority::Other) noexcept;
    void UpdateListener(Float3 position, Float3 forward, Float3 up) noexcept;
    void PauseCombat() noexcept;
    void ResumeCombat() noexcept;

    [[nodiscard]] AudioStatus Status() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hs
