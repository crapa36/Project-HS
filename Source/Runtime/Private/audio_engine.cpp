#include <hs/runtime/audio_engine.hpp>
#include <hs/core/cooked_format.hpp>

#include <Windows.h>
#include <wrl/client.h>
#include <xaudio2.h>
#include <x3daudio.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

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

[[nodiscard]] bool IsArrowImpact(std::string_view id) noexcept
{
    return id.starts_with("audio.common.arrow_impact") ||
           id == "audio.skill.basic.impact" ||
           id == "audio.skill.piercing.pierce" ||
           id == "audio.skill.charged.hit" ||
           id == "audio.skill.ricochet.hit" ||
           id == "audio.skill.arrow_rain.impact";
}

using Json = nlohmann::json;

struct AudioSample
{
    std::filesystem::path path;
    std::vector<std::byte> bytes;
    WAVEFORMATEX format{};
    std::uint64_t data_offset{};
    std::uint64_t data_size{};
    bool payload_loaded{};
};

struct AudioCue
{
    AssetId id{};
    std::vector<std::shared_ptr<AudioSample>> files;
    AudioBus bus{AudioBus::Sfx};
    AudioPriority priority{AudioPriority::Other};
    bool spatial{};
    bool preload{};
    bool arrow_impact_family{};
    bool loop{};
    std::uint32_t max_simultaneous{1};
    std::uint32_t retrigger_ms{};
};

[[nodiscard]] std::uint16_t U16(const std::vector<std::byte> &b, std::size_t p) noexcept
{
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(b[p]) |
                                      (std::to_integer<unsigned>(b[p + 1]) << 8));
}
[[nodiscard]] std::uint32_t U32(const std::vector<std::byte> &b, std::size_t p) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned>(b[p]) |
                                      (std::to_integer<unsigned>(b[p + 1]) << 8) |
                                      (std::to_integer<unsigned>(b[p + 2]) << 16) |
                                      (std::to_integer<unsigned>(b[p + 3]) << 24));
}

[[nodiscard]] std::uint16_t ReadU16(const std::byte *bytes) noexcept
{
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[0]) |
                                      (std::to_integer<unsigned>(bytes[1]) << 8));
}

[[nodiscard]] std::uint32_t ReadU32(const std::byte *bytes) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[0]) |
                                      (std::to_integer<unsigned>(bytes[1]) << 8) |
                                      (std::to_integer<unsigned>(bytes[2]) << 16) |
                                      (std::to_integer<unsigned>(bytes[3]) << 24));
}

[[nodiscard]] Result LoadWaveHeader(const std::filesystem::path &path,
                                     std::shared_ptr<AudioSample> &out)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return Result::Failure(ErrorCode::TaskFailed, "audio", "WAV open failed");
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length < 12 || static_cast<std::uint64_t>(length) > std::numeric_limits<std::uint32_t>::max() + 8ull)
        return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV size invalid");
    const auto file_size = static_cast<std::uint64_t>(length);
    std::array<std::byte, 12> riff{};
    input.seekg(0);
    input.read(reinterpret_cast<char *>(riff.data()), static_cast<std::streamsize>(riff.size()));
    if (!input || std::memcmp(riff.data(), "RIFF", 4) != 0 ||
        std::memcmp(riff.data() + 8, "WAVE", 4) != 0)
        return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV RIFF header invalid");
    if (ReadU32(riff.data() + 4) != file_size - 8)
        return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV RIFF size invalid");

    std::uint64_t cursor = 12;
    std::vector<std::byte> fmt;
    std::uint64_t data_offset{};
    std::uint64_t data_size{};
    while (cursor + 8 <= file_size)
    {
        input.seekg(static_cast<std::streamoff>(cursor));
        std::array<std::byte, 8> chunk_header{};
        input.read(reinterpret_cast<char *>(chunk_header.data()),
                   static_cast<std::streamsize>(chunk_header.size()));
        if (!input)
            return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV chunk header truncated");
        const auto chunk_size = static_cast<std::uint64_t>(ReadU32(chunk_header.data() + 4));
        const auto begin = cursor + 8;
        if (chunk_size > file_size - begin)
            return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV chunk exceeds file");
        if (std::memcmp(chunk_header.data(), "fmt ", 4) == 0)
        {
            if (chunk_size > 4096)
                return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV fmt chunk too large");
            fmt.resize(static_cast<std::size_t>(chunk_size));
            input.read(reinterpret_cast<char *>(fmt.data()),
                       static_cast<std::streamsize>(fmt.size()));
        }
        else if (std::memcmp(chunk_header.data(), "data", 4) == 0 && data_size == 0)
        {
            data_offset = begin;
            data_size = chunk_size;
        }
        const auto padded_end = begin + chunk_size + (chunk_size & 1u);
        if (padded_end > file_size)
            return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV chunk padding exceeds file");
        cursor = padded_end;
    }
    if (cursor != file_size)
        return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV chunk table truncated");
    if (fmt.size() < 16 || data_size == 0)
        return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV fmt/data missing");
    const auto channels = U16(fmt, 2);
    const auto sample_rate = U32(fmt, 4);
    const auto block_align = U16(fmt, 12);
    const auto bits = U16(fmt, 14);
    if (U16(fmt, 0) != WAVE_FORMAT_PCM || (channels != 1 && channels != 2) ||
        sample_rate != 48000 || bits != 24 || block_align != channels * 3 ||
        U32(fmt, 8) != sample_rate * block_align || data_size % block_align != 0)
        return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV must be PCM 48kHz 24-bit mono/stereo");
    out = std::make_shared<AudioSample>();
    out->path = path;
    out->data_offset = data_offset;
    out->data_size = data_size;
    out->format = {WAVE_FORMAT_PCM, channels, sample_rate, sample_rate * block_align,
                   block_align, bits, 0};
    return Result::Success();
}

[[nodiscard]] Result LoadWavePayload(AudioSample &sample)
{
    if (sample.payload_loaded) return Result::Success();
    if (sample.data_size > std::numeric_limits<std::size_t>::max() ||
        sample.data_size > std::numeric_limits<UINT32>::max())
        return Result::Failure(ErrorCode::InvalidArgument, "audio", "WAV payload too large");
    std::ifstream input(sample.path, std::ios::binary);
    if (!input) return Result::Failure(ErrorCode::TaskFailed, "audio", "WAV open failed");
    input.seekg(static_cast<std::streamoff>(sample.data_offset));
    sample.bytes.resize(static_cast<std::size_t>(sample.data_size));
    input.read(reinterpret_cast<char *>(sample.bytes.data()),
               static_cast<std::streamsize>(sample.bytes.size()));
    if (!input)
    {
        sample.bytes.clear();
        return Result::Failure(ErrorCode::TaskFailed, "audio", "WAV payload read failed");
    }
    sample.payload_loaded = true;
    return Result::Success();
}

[[nodiscard]] AudioBus ParseBus(const std::string &value) noexcept
{
    if (value == "BGM") return AudioBus::Bgm;
    if (value == "UI") return AudioBus::Ui;
    return AudioBus::Sfx;
}
[[nodiscard]] AudioPriority ParsePriority(const std::string &value) noexcept
{
    if (value == "UI") return AudioPriority::Ui;
    if (value == "BossTelegraph") return AudioPriority::BossWarning;
    if (value == "Player") return AudioPriority::Player;
    if (value == "Enemy") return AudioPriority::Enemy;
    return AudioPriority::Other;
}

[[nodiscard]] bool IsKnownBus(const std::string &value) noexcept
{
    return value == "BGM" || value == "SFX" || value == "UI";
}
[[nodiscard]] bool IsKnownPriority(const std::string &value) noexcept
{
    return value == "Other" || value == "Enemy" || value == "Player" ||
           value == "BossTelegraph" || value == "UI";
}

// Gameplay positions use meters. The default X3DAudio curve has no attenuation
// through CurveDistanceScaler, then falls off as scaler / distance.
[[nodiscard]] float DistanceScaler(AudioPriority priority) noexcept
{
    constexpr float kNormalCombatDistanceMeters = 35.0f;
    constexpr float kBossTelegraphDistanceMeters = 90.0f;
    return priority == AudioPriority::BossWarning
               ? kBossTelegraphDistanceMeters
               : kNormalCombatDistanceMeters;
}

[[nodiscard]] std::uint32_t PrioritySoftCap(AudioPriority priority) noexcept
{
    switch (priority)
    {
    case AudioPriority::Ui: return 4;
    case AudioPriority::BossWarning: return 6;
    case AudioPriority::Player: return 16;
    case AudioPriority::Enemy: return 18;
    case AudioPriority::Other: return 12;
    }
    return 12;
}

} // namespace

struct AudioEngine::Impl
{
    struct ActiveVoice
    {
        IXAudio2SourceVoice *voice{};
        AssetId cue{};
        AudioBus bus{AudioBus::Sfx};
        AudioPriority priority{AudioPriority::Other};
        std::shared_ptr<AudioSample> sample;
        Float3 position{};
        bool spatial{};
        bool arrow_impact_family{};
        Sequence event_sequence{};
        std::uint32_t aggregation_count{1};
        std::uint64_t started_ms{};
        std::uint64_t serial{};
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
    std::uint32_t device_channels{2};
    std::uint64_t voice_serial{};
    std::unordered_map<std::uint64_t, AudioCue> cues;
    std::unordered_map<std::uint64_t, std::uint64_t> last_play_ms;

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

    void Reclaim() noexcept
    {
        status.active_source_voices = 0;
        for (auto &active : active_voices)
        {
            if (!active.voice) continue;
            XAUDIO2_VOICE_STATE state{};
            active.voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (state.BuffersQueued == 0)
            {
                active.voice->DestroyVoice();
                active = {};
            }
            else ++status.active_source_voices;
        }
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
    return Initialize(settings, std::filesystem::path{"Cooked/Audio"});
}

Result AudioEngine::Initialize(const SettingsData &settings,
                               const std::filesystem::path &audio_directory)
{
    Shutdown();

    try
    {
        std::ifstream catalog_file(audio_directory / "audio_cues.json");
        if (!catalog_file)
            return Result::Failure(ErrorCode::TaskFailed, "audio", "audio catalog open failed");
        const auto catalog = Json::parse(catalog_file);
        if (!catalog.is_object() || !catalog.contains("entries") || !catalog.at("entries").is_array())
            return Result::Failure(ErrorCode::InvalidArgument, "audio", "audio catalog entries invalid");
        std::unordered_set<std::string> cue_names;
        std::unordered_set<std::uint64_t> cue_hashes;
        std::unordered_set<std::string> file_names;
        for (const auto &item : catalog.at("entries"))
        {
            const auto id = item.at("id").get<std::string>();
            const auto asset_name = item.at("asset_id").get<std::string>();
            if (!cue_names.insert(id).second ||
                (asset_name != id && !cue_names.insert(asset_name).second))
                return Result::Failure(ErrorCode::InvalidArgument, "audio", "duplicate audio cue name");
            const auto hash = MakeAssetId(asset_name).value;
            if (hash == 0 || !cue_hashes.insert(hash).second)
                return Result::Failure(ErrorCode::InvalidArgument, "audio", "duplicate or empty audio cue id");
            AudioCue cue;
            cue.id = {hash};
            const auto submix = item.at("submix").get<std::string>();
            const auto priority_name = item.at("priority").get<std::string>();
            if (!IsKnownBus(submix) || !IsKnownPriority(priority_name) ||
                item.at("encoding").get<std::string>() != "PCM" ||
                item.at("streaming").get<bool>())
                return Result::Failure(ErrorCode::InvalidArgument, "audio", "unsupported audio catalog value");
            cue.bus = ParseBus(submix);
            cue.priority = ParsePriority(priority_name);
            cue.spatial = item.at("spatial").get<bool>();
            cue.preload = item.at("preload").get<bool>();
            cue.arrow_impact_family = IsArrowImpact(asset_name);
            cue.loop = item.at("loop").get<bool>();
            cue.max_simultaneous = std::max(1u, item.at("max_simultaneous").get<std::uint32_t>());
            cue.retrigger_ms = item.at("minimum_retrigger_ms").get<std::uint32_t>();
            const auto &files = item.at("files");
            if (!files.is_array() || files.empty())
                return Result::Failure(ErrorCode::InvalidArgument, "audio", "audio cue has no files");
            for (const auto &file : files)
            {
                const auto name = file.get<std::string>();
                const std::filesystem::path relative{name};
                if (name.empty() || relative.is_absolute() || relative.has_parent_path() ||
                    relative.filename().string() != name || relative.extension() != ".wav")
                    return Result::Failure(ErrorCode::InvalidArgument, "audio", "unsafe audio filename");
                if (!file_names.insert(name).second)
                    return Result::Failure(ErrorCode::InvalidArgument, "audio", "duplicate audio filename");
                std::shared_ptr<AudioSample> sample;
                if (auto loaded = LoadWaveHeader(audio_directory / relative, sample); !loaded)
                    return loaded;
                if (cue.spatial && sample->format.nChannels != 1)
                    return Result::Failure(ErrorCode::InvalidArgument, "audio", "spatial audio must be mono");
                if (cue.preload)
                {
                    if (auto loaded = LoadWavePayload(*sample); !loaded)
                        return loaded;
                    ++impl_->status.loaded_files;
                }
                cue.files.push_back(std::move(sample));
            }
            impl_->cues.emplace(hash, std::move(cue));
            ++impl_->status.loaded_cues;
        }
    }
    catch (const std::exception &error)
    {
        impl_->cues.clear();
        impl_->status = {};
        return Result::Failure(ErrorCode::InvalidArgument, "audio", error.what());
    }

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
    impl_->device_channels = device_details.InputChannels;
    impl_->status.device_state = AudioDeviceState::Ready;
    impl_->status.native_error = 0;
    ApplySettings(settings);
    return Result::Success();
}

void AudioEngine::Shutdown() noexcept
{
    impl_->ReleaseNative();
    impl_->cues.clear();
    impl_->last_play_ms.clear();
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

void AudioEngine::Play(const PresentationEvent &event) noexcept
{
    try
    {
    if (event.kind != PresentationKind::Audio || event.asset.value == 0)
    {
        return;
    }
    impl_->Reclaim();
    const auto cue_it = impl_->cues.find(event.asset.value);
    if (cue_it == impl_->cues.end())
    {
        ++impl_->status.skipped_missing_cues;
        return;
    }
    const auto &cue = cue_it->second;
    const auto now = GetTickCount64();
    if (const auto previous = impl_->last_play_ms.find(event.asset.value);
        previous != impl_->last_play_ms.end() && now - previous->second < cue.retrigger_ms)
    {
        if (event.asset.value == MakeAssetId("audio.pickup.xp_collect").value)
            if (const auto active = std::find_if(impl_->active_voices.begin(), impl_->active_voices.end(),
                                                 [&](const auto &voice) {
                                                     return voice.voice && voice.cue.value == event.asset.value;
                                                 });
                active != impl_->active_voices.end())
            {
                ++active->aggregation_count;
                (void)active->voice->SetFrequencyRatio(
                    std::min(1.0f + 0.03f * static_cast<float>(active->aggregation_count - 1), 1.5f));
            }
        return;
    }
    std::uint32_t same_cue{};
    for (const auto &active : impl_->active_voices)
        if (active.voice && active.cue.value == event.asset.value) ++same_cue;
    if (same_cue >= cue.max_simultaneous) return;
    std::uint32_t same_priority{};
    std::array<Sequence, 6> recent_arrow_sequences{};
    std::uint32_t recent_arrow_impacts{};
    for (const auto &active : impl_->active_voices)
    {
        if (!active.voice) continue;
        if (active.priority == cue.priority) ++same_priority;
        if (cue.arrow_impact_family && active.arrow_impact_family &&
            now - active.started_ms < 30 &&
            recent_arrow_impacts < recent_arrow_sequences.size() &&
            std::find(recent_arrow_sequences.begin(),
                      recent_arrow_sequences.begin() + recent_arrow_impacts,
                      active.event_sequence) == recent_arrow_sequences.begin() + recent_arrow_impacts)
        {
            recent_arrow_sequences[recent_arrow_impacts] = active.event_sequence;
            ++recent_arrow_impacts;
        }
    }
    if (same_priority >= PrioritySoftCap(cue.priority) || recent_arrow_impacts >= 6)
    {
        ++impl_->status.skipped_voice_limits;
        return;
    }
    const auto index = cue.files.empty() ? 0u : static_cast<std::size_t>(event.sequence % cue.files.size());
    const auto &sample = cue.files[index];
    if (!sample->payload_loaded)
    {
        if (auto loaded = LoadWavePayload(*sample); !loaded)
        {
            ++impl_->status.skipped_load_failures;
            return;
        }
        ++impl_->status.loaded_files;
    }
    if (!impl_->engine || !impl_->device_voice) return;
    const auto slot = std::find_if(impl_->active_voices.begin(), impl_->active_voices.end(),
                                   [](const auto &voice) { return voice.voice == nullptr; });
    auto selected = slot;
    if (selected == impl_->active_voices.end())
    {
        selected = std::min_element(impl_->active_voices.begin(), impl_->active_voices.end(),
                                    [](const auto &a, const auto &b) {
                                        if (a.priority != b.priority)
                                            return static_cast<unsigned>(a.priority) < static_cast<unsigned>(b.priority);
                                        return a.serial < b.serial;
                                    });
        if (selected->voice && static_cast<unsigned>(selected->priority) >= static_cast<unsigned>(cue.priority)) return;
        if (selected->voice)
        {
            selected->voice->DestroyVoice();
            selected->voice = nullptr;
            if (impl_->status.active_source_voices > 0) --impl_->status.active_source_voices;
        }
    }
    IXAudio2SourceVoice *source{};
    if (FAILED(impl_->engine->CreateSourceVoice(&source, &sample->format, 0, 2.0f, nullptr, nullptr, nullptr))) return;
    const auto target_bus = cue.bus == AudioBus::Sfx ? impl_->sfx_bus : cue.bus == AudioBus::Bgm ? impl_->bgm_bus : impl_->ui_bus;
    XAUDIO2_SEND_DESCRIPTOR send{0, target_bus ? target_bus : impl_->master_bus};
    XAUDIO2_VOICE_SENDS sends{1, &send};
    (void)source->SetOutputVoices(&sends);
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = static_cast<UINT32>(sample->bytes.size());
    buffer.pAudioData = reinterpret_cast<const BYTE *>(sample->bytes.data());
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    if (cue.loop) buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
    if (FAILED(source->SubmitSourceBuffer(&buffer)) || FAILED(source->Start()))
    {
        source->DestroyVoice();
        return;
    }
    if (cue.spatial && sample->format.nChannels == 1)
    {
        X3DAUDIO_EMITTER emitter{};
        emitter.Position = {event.position.x, event.position.y, event.position.z};
        emitter.OrientFront = {0.0f, 0.0f, 1.0f};
        emitter.OrientTop = {0.0f, 1.0f, 0.0f};
        emitter.ChannelCount = 1;
        emitter.CurveDistanceScaler = DistanceScaler(cue.priority);
        std::vector<float> matrix(impl_->device_channels);
        X3DAUDIO_DSP_SETTINGS dsp{};
        dsp.SrcChannelCount = 1;
        dsp.DstChannelCount = impl_->device_channels;
        dsp.pMatrixCoefficients = matrix.data();
        X3DAudioCalculate(impl_->x3d, &impl_->listener, &emitter,
                          X3DAUDIO_CALCULATE_MATRIX,
                          &dsp);
        (void)source->SetOutputMatrix(target_bus ? target_bus : impl_->master_bus,
                                      1, dsp.DstChannelCount, matrix.data());
    }
    selected->voice = source;
    selected->cue = cue.id;
    selected->bus = cue.bus;
    selected->priority = cue.priority;
    selected->sample = sample;
    selected->position = event.position;
    selected->spatial = cue.spatial;
    selected->arrow_impact_family = cue.arrow_impact_family;
    selected->event_sequence = event.sequence;
    selected->aggregation_count = 1;
    selected->started_ms = now;
    selected->serial = ++impl_->voice_serial;
    impl_->last_play_ms[event.asset.value] = now;
    ++impl_->status.played_count;
    ++impl_->status.active_source_voices;
    }
    catch (...)
    {
        ++impl_->status.skipped_load_failures;
    }
}

void AudioEngine::Stop(AssetId cue) noexcept
{
    for (auto &active : impl_->active_voices)
        if (active.voice && active.cue.value == cue.value)
        {
            active.voice->Stop();
            active.voice->DestroyVoice();
            active = {};
        }
    impl_->Reclaim();
}

void AudioEngine::StopBus(AudioBus bus) noexcept
{
    for (auto &active : impl_->active_voices)
        if (active.voice && active.bus == bus)
        {
            active.voice->Stop();
            active.voice->DestroyVoice();
            active = {};
        }
    impl_->Reclaim();
}

void AudioEngine::UpdateListener(Float3 position, Float3 forward, Float3 up) noexcept
{
    impl_->Reclaim();
    impl_->listener.Position = {position.x, position.y, position.z};
    impl_->listener.OrientFront = Normalize(forward, {0.0f, 0.0f, 1.0f});
    impl_->listener.OrientTop = Normalize(up, {0.0f, 1.0f, 0.0f});
    for (auto &active : impl_->active_voices)
    {
        if (!active.voice || !active.spatial || !active.sample || active.sample->format.nChannels != 1)
            continue;
        X3DAUDIO_EMITTER emitter{};
        emitter.Position = {active.position.x, active.position.y, active.position.z};
        emitter.OrientFront = {0.0f, 0.0f, 1.0f};
        emitter.OrientTop = {0.0f, 1.0f, 0.0f};
        emitter.ChannelCount = 1;
        emitter.CurveDistanceScaler = DistanceScaler(active.priority);
        std::vector<float> matrix(impl_->device_channels);
        X3DAUDIO_DSP_SETTINGS dsp{};
        dsp.SrcChannelCount = 1;
        dsp.DstChannelCount = impl_->device_channels;
        dsp.pMatrixCoefficients = matrix.data();
        X3DAudioCalculate(impl_->x3d, &impl_->listener, &emitter,
                          X3DAUDIO_CALCULATE_MATRIX,
                          &dsp);
        const auto target_bus = active.bus == AudioBus::Sfx ? impl_->sfx_bus :
                                active.bus == AudioBus::Bgm ? impl_->bgm_bus : impl_->ui_bus;
        (void)active.voice->SetOutputMatrix(target_bus ? target_bus : impl_->master_bus,
                                            1, dsp.DstChannelCount, matrix.data());
    }
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
