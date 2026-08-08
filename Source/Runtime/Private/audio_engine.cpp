#include <hs/runtime/audio_engine.hpp>

#include <Windows.h>
#include <wrl/client.h>
#include <xaudio2.h>
#include <x3daudio.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace hs
{
namespace
{

[[nodiscard]] float ClampVolume(float value) noexcept
{
    return std::clamp(value, 0.0f, 1.0f);
}

[[nodiscard]] X3DAUDIO_VECTOR Normalize(Float3 value, X3DAUDIO_VECTOR fallback) noexcept
{
    const auto length_squared = value.x * value.x + value.y * value.y + value.z * value.z;
    if (length_squared <= 0.000001f)
    {
        return fallback;
    }
    const auto inverse_length = 1.0f / std::sqrt(length_squared);
    return {value.x * inverse_length, value.y * inverse_length, value.z * inverse_length};
}

[[nodiscard]] bool IsCombatBus(AudioBus bus) noexcept
{
    return bus == AudioBus::Bgm || bus == AudioBus::Sfx;
}

} // namespace

struct AudioEngine::Impl
{
    struct ActiveVoice
    {
        IXAudio2SourceVoice *voice{};
        AudioBus bus{AudioBus::Sfx};
        AudioPriority priority{AudioPriority::Other};
    };

    Microsoft::WRL::ComPtr<IXAudio2> engine;
    IXAudio2MasteringVoice *device_voice{};
    IXAudio2SubmixVoice *master_bus{};
    IXAudio2SubmixVoice *bgm_bus{};
    IXAudio2SubmixVoice *sfx_bus{};
    IXAudio2SubmixVoice *ui_bus{};
    std::array<ActiveVoice, AudioEngine::kMaximumSourceVoices> active_voices{};
    X3DAUDIO_HANDLE x3d{};
    X3DAUDIO_LISTENER listener{};
    AudioStatus status{};
    float master_volume{1.0f};
    float bgm_volume{1.0f};
    float sfx_volume{1.0f};
    float ui_volume{1.0f};
    bool owns_com{};

    void ReleaseNative() noexcept
    {
        for (auto &active : active_voices)
        {
            if (active.voice)
            {
                active.voice->DestroyVoice();
                active = {};
            }
        }
        if (ui_bus)
        {
            ui_bus->DestroyVoice();
            ui_bus = nullptr;
        }
        if (sfx_bus)
        {
            sfx_bus->DestroyVoice();
            sfx_bus = nullptr;
        }
        if (bgm_bus)
        {
            bgm_bus->DestroyVoice();
            bgm_bus = nullptr;
        }
        if (master_bus)
        {
            master_bus->DestroyVoice();
            master_bus = nullptr;
        }
        if (device_voice)
        {
            device_voice->DestroyVoice();
            device_voice = nullptr;
        }
        engine.Reset();
        if (owns_com)
        {
            CoUninitialize();
            owns_com = false;
        }
        status.active_source_voices = 0;
    }

    void EnterSilent(HRESULT error) noexcept
    {
        ReleaseNative();
        status.device_state = AudioDeviceState::SilentUnavailable;
        status.native_error = static_cast<std::uint32_t>(error);
    }

    void ApplyVolumes() noexcept
    {
        if (master_bus)
        {
            (void)master_bus->SetVolume(master_volume);
        }
        if (bgm_bus)
        {
            (void)bgm_bus->SetVolume(status.combat_paused ? 0.0f : bgm_volume);
        }
        if (sfx_bus)
        {
            (void)sfx_bus->SetVolume(status.combat_paused ? 0.0f : sfx_volume);
        }
        if (ui_bus)
        {
            (void)ui_bus->SetVolume(ui_volume);
        }
    }
};

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>())
{
}

AudioEngine::~AudioEngine()
{
    Shutdown();
}

Result AudioEngine::Initialize(const SettingsData &settings)
{
    Shutdown();

    const auto com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(com_result))
    {
        impl_->owns_com = true;
    }
    else if (com_result != RPC_E_CHANGED_MODE)
    {
        impl_->EnterSilent(com_result);
        ApplySettings(settings);
        return Result::Success();
    }

    auto result = XAudio2Create(impl_->engine.GetAddressOf());
    if (FAILED(result))
    {
        impl_->EnterSilent(result);
        ApplySettings(settings);
        return Result::Success();
    }

    result = impl_->engine->CreateMasteringVoice(&impl_->device_voice);
    if (FAILED(result))
    {
        impl_->EnterSilent(result);
        ApplySettings(settings);
        return Result::Success();
    }

    XAUDIO2_VOICE_DETAILS device_details{};
    impl_->device_voice->GetVoiceDetails(&device_details);
    DWORD channel_mask{};
    result = impl_->device_voice->GetChannelMask(&channel_mask);
    if (FAILED(result))
    {
        impl_->EnterSilent(result);
        ApplySettings(settings);
        return Result::Success();
    }

    XAUDIO2_SEND_DESCRIPTOR device_send{0, impl_->device_voice};
    XAUDIO2_VOICE_SENDS device_sends{1, &device_send};
    result = impl_->engine->CreateSubmixVoice(
        &impl_->master_bus, device_details.InputChannels, device_details.InputSampleRate, 0, 2,
        &device_sends);
    if (FAILED(result))
    {
        impl_->EnterSilent(result);
        ApplySettings(settings);
        return Result::Success();
    }

    XAUDIO2_SEND_DESCRIPTOR master_send{0, impl_->master_bus};
    XAUDIO2_VOICE_SENDS master_sends{1, &master_send};
    const auto create_category = [&](IXAudio2SubmixVoice **voice) {
        return impl_->engine->CreateSubmixVoice(
            voice, device_details.InputChannels, device_details.InputSampleRate, 0, 1,
            &master_sends);
    };
    if (FAILED(result = create_category(&impl_->bgm_bus)) ||
        FAILED(result = create_category(&impl_->sfx_bus)) ||
        FAILED(result = create_category(&impl_->ui_bus)))
    {
        impl_->EnterSilent(result);
        ApplySettings(settings);
        return Result::Success();
    }

    result = X3DAudioInitialize(channel_mask, X3DAUDIO_SPEED_OF_SOUND, impl_->x3d);
    if (FAILED(result))
    {
        impl_->EnterSilent(result);
        ApplySettings(settings);
        return Result::Success();
    }

    impl_->listener.OrientFront = {0.0f, 0.0f, 1.0f};
    impl_->listener.OrientTop = {0.0f, 1.0f, 0.0f};
    impl_->status.device_state = AudioDeviceState::Ready;
    impl_->status.native_error = 0;
    ApplySettings(settings);
    return Result::Success();
}

void AudioEngine::Shutdown() noexcept
{
    impl_->ReleaseNative();
    impl_->status = {};
}

void AudioEngine::ApplySettings(const SettingsData &settings) noexcept
{
    impl_->master_volume = ClampVolume(settings.master_volume);
    impl_->bgm_volume = ClampVolume(settings.bgm_volume);
    impl_->sfx_volume = ClampVolume(settings.sfx_volume);
    impl_->ui_volume = ClampVolume(settings.ui_volume);
    impl_->ApplyVolumes();
}

void AudioEngine::Play(const PresentationEvent &event, AudioBus, AudioPriority) noexcept
{
    if (event.kind != PresentationKind::Audio || event.asset.value == 0)
    {
        return;
    }

    // Audio content is intentionally absent until authored cues are supplied.
    ++impl_->status.skipped_missing_cues;
}

void AudioEngine::UpdateListener(Float3 position, Float3 forward, Float3 up) noexcept
{
    impl_->listener.Position = {position.x, position.y, position.z};
    impl_->listener.OrientFront = Normalize(forward, {0.0f, 0.0f, 1.0f});
    impl_->listener.OrientTop = Normalize(up, {0.0f, 1.0f, 0.0f});
}

void AudioEngine::PauseCombat() noexcept
{
    if (impl_->status.combat_paused)
    {
        return;
    }
    impl_->status.combat_paused = true;
    for (auto &active : impl_->active_voices)
    {
        if (active.voice && IsCombatBus(active.bus))
        {
            (void)active.voice->Stop();
        }
    }
    impl_->ApplyVolumes();
}

void AudioEngine::ResumeCombat() noexcept
{
    if (!impl_->status.combat_paused)
    {
        return;
    }
    impl_->status.combat_paused = false;
    for (auto &active : impl_->active_voices)
    {
        if (active.voice && IsCombatBus(active.bus))
        {
            (void)active.voice->Start();
        }
    }
    impl_->ApplyVolumes();
}

AudioStatus AudioEngine::Status() const noexcept
{
    return impl_->status;
}

} // namespace hs
