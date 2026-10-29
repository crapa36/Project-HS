#include <hs/runtime/audio_engine.hpp>
#include <hs/core/cooked_format.hpp>
#include <hs/core/shipped_content_owner.hpp>
#include "audio_catalog.hpp"
#include "audio_native.hpp"
#include "runtime_diagnostics.hpp"

#include <Windows.h>
#include <wrl/client.h>
#include <xaudio2.h>
#include <x3daudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string_view>
#include <vector>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>



namespace hs
{
using namespace audio_detail;
namespace
{

std::uint64_t AudioQpc() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

[[nodiscard]] float ClampVolume(float value) noexcept
{
    return std::clamp(value, 0.0f, 1.0f);
}

[[nodiscard]] bool SameWaveFormat(const WAVEFORMATEX &a, const WAVEFORMATEX &b) noexcept
{
    return a.wFormatTag == b.wFormatTag &&
           a.nChannels == b.nChannels &&
           a.nSamplesPerSec == b.nSamplesPerSec &&
           a.nAvgBytesPerSec == b.nAvgBytesPerSec &&
           a.nBlockAlign == b.nBlockAlign &&
           a.wBitsPerSample == b.wBitsPerSample &&
           a.cbSize == b.cbSize;
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
    std::shared_ptr<const void> generation_lease;
    struct CuePlayback
    {
        std::uint64_t last_play_ms{};
        bool has_played{}, prepared{}, read_failure_reported{};
    };
    struct ActiveVoice
    {
        IXAudio2SourceVoice *voice{};
        AssetId cue{};
        AudioBus bus{AudioBus::Sfx};
        AudioPriority priority{AudioPriority::Other};
        AudioSample *sample{};
        Float3 position{};
        bool spatial{};
        bool arrow_impact_family{};
        Sequence event_sequence{};
        std::uint32_t aggregation_count{1};
        std::uint64_t started_ms{};
        std::uint64_t serial{};
        bool queued{}, releasing{};
        std::uint64_t session_id{};
        AudioBufferCompletion *completion{};
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
    std::uint64_t voice_serial{}, device_epoch{}, buffer_sequence{};
    std::array<std::uint64_t, AudioEngine::kMaximumSourceVoices> voice_generations{};
    std::array<std::array<AudioBufferCompletion, 2>, AudioEngine::kMaximumSourceVoices> completions{};
    std::span<const AudioCueRow> cues;
    std::vector<AudioCueRow> developer_cues;
    std::vector<std::uint32_t> transition_cues;
    std::unique_ptr<CuePlayback[]> playback;
    std::unique_ptr<AudioSample[]> samples;
    std::vector<std::byte> resident_pcm;
    std::size_t sample_count{};
    const ShippedContentOwner *content{};
    std::mutex read_mutex;
    std::condition_variable read_ready;
    std::deque<AudioSample *> reads;
    std::thread reader;
    bool stopping{};
    std::array<float, XAUDIO2_MAX_AUDIO_CHANNELS> spatial_matrix{};
    std::array<std::uint32_t, 4> spatial_matrix_key{};
    bool spatial_matrix_valid{};
    bool capture_timings{}; // Configured before the reader is started.
    void (*wake_main)(void *) noexcept{};
    void *wake_context{};
    std::array<runtime_detail::DiagnosticSample, kAudioWorkNames.size()> work{};
    std::atomic<std::uint64_t> reader_jobs{}, reader_failures{}, reader_bytes{};
    std::atomic<std::uint64_t> reader_read_ticks{}, reader_queue_ticks{}, reader_request_ready_ticks{};
    bool reader_failure_reported{}; // Reader thread only; one diagnostic per engine.
    AudioWorkTimings native_timings{};
    runtime_detail::DiagnosticClock Clock() const noexcept { return capture_timings ? AudioQpc : nullptr; }
    auto &Work(AudioWorkId id) noexcept { return work[static_cast<std::size_t>(id)]; }

    std::span<const float> SpatialMatrix(Float3 position, AudioPriority priority) noexcept
    {
        const auto distance = DistanceScaler(priority);
        const std::array key{std::bit_cast<std::uint32_t>(position.x),
            std::bit_cast<std::uint32_t>(position.y), std::bit_cast<std::uint32_t>(position.z),
            std::bit_cast<std::uint32_t>(distance)};
        if (!spatial_matrix_valid || spatial_matrix_key != key)
        {
            runtime_detail::DiagnosticScope spatial(Work(AudioWorkId::Spatial), Clock());
            X3DAUDIO_EMITTER emitter{};
            emitter.Position = {position.x, position.y, position.z};
            emitter.OrientFront = {0.0f, 0.0f, 1.0f};
            emitter.OrientTop = {0.0f, 1.0f, 0.0f};
            emitter.ChannelCount = 1;
            emitter.CurveDistanceScaler = distance;
            X3DAUDIO_DSP_SETTINGS dsp{};
            dsp.SrcChannelCount = 1;
            dsp.DstChannelCount = device_channels;
            dsp.pMatrixCoefficients = spatial_matrix.data();
            X3DAudioCalculate(x3d, &listener, &emitter, X3DAUDIO_CALCULATE_MATRIX, &dsp);
            spatial_matrix_key = key;
            spatial_matrix_valid = true;
        }
        return {spatial_matrix.data(), device_channels};
    }

    void Failure(NativeAudioResult result, bool cleanup = false) noexcept
    {
        if (result.Succeeded()) return;
        if (cleanup)
        {
            if (status.native_cleanup_operation == AudioNativeOperation::None)
            {
                status.native_cleanup_error = static_cast<std::uint32_t>(result.error);
                status.native_cleanup_operation = result.operation;
            }
        }
        else if (status.native_operation == AudioNativeOperation::None)
        {
            status.native_error = static_cast<std::uint32_t>(result.error);
            status.native_operation = result.operation;
        }
    }
    NativeAudioResult Retire(ActiveVoice &active) noexcept
    {
        const auto *completion = active.completion;
        DestroyOwnedVoice(active.voice, active.completion);
        const NativeAudioResult result{AudioNativeOperation::Callback,
            completion ? completion->native_error.load(std::memory_order_acquire) : S_OK};
        Failure(result);
        active = {};
        return result;
    }
    bool Observe(ActiveVoice &active) noexcept
    {
        if (!active.voice) return false;
        if (active.completion)
        {
            const auto error = active.completion->native_error.load(std::memory_order_acquire);
            if (FAILED(error))
            {
                Failure({AudioNativeOperation::Callback, error}); Retire(active); return false;
            }
        }
        XAUDIO2_VOICE_STATE state{};
        active.voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        active.queued = state.BuffersQueued != 0;
        if (!active.queued) active.releasing = false;
        return active.queued;
    }

    const AudioCueRow *Find(AssetId id) const noexcept
    {
        const auto it = std::lower_bound(cues.begin(), cues.end(), id.value,
            [](const AudioCueRow &cue, std::uint64_t key) { return cue.id.value < key; });
        return it != cues.end() && it->id.value == id.value ? &*it : nullptr;
    }
    CuePlayback &State(const AudioCueRow &cue) noexcept
    {
        return playback[static_cast<std::size_t>(&cue - cues.data())];
    }
    Result Read(AudioSample &sample)
    {
        if (sample.read_developer) return sample.read_developer(sample);
        sample.pcm = {};
        sample.bytes.resize(static_cast<std::size_t>(sample.data_size));
        auto result = content->ReadVerified(sample.data_offset, sample.bytes, sample.chunk->digest);
        if (result) sample.pcm = sample.bytes;
        return result;
    }
    void ReportReaderFailure(std::size_t sample_index, ErrorCode code,
                             std::string_view subsystem, std::string_view message) noexcept
    {
        if (reader_failure_reported) return;
        reader_failure_reported = true;
        char diagnostic[768]{};
        const auto subsystem_size = static_cast<int>(std::min(subsystem.size(), std::size_t{64}));
        const auto message_size = static_cast<int>(std::min(message.size(), std::size_t{512}));
        const auto *subsystem_text = subsystem.empty() ? "" : subsystem.data();
        const auto *message_text = message.empty() ? "" : message.data();
        const int length = std::snprintf(diagnostic, sizeof(diagnostic),
            "Audio sample read failed (index %zu, error %u, subsystem '%.*s'): %.*s\n",
            sample_index, static_cast<unsigned>(code), subsystem_size, subsystem_text,
            message_size, message_text);
        if (length <= 0)
        {
            constexpr char fallback[] = "Audio sample read failed; diagnostic formatting failed.\n";
            std::fputs(fallback, stderr);
            std::fflush(stderr);
            OutputDebugStringA(fallback);
            return;
        }
        const auto bytes = std::min(static_cast<std::size_t>(length), sizeof(diagnostic) - 1);
        std::fwrite(diagnostic, 1, bytes, stderr);
        std::fflush(stderr);
        OutputDebugStringA(diagnostic);
    }
    void RunReader() noexcept
    {
        for (;;)
        {
            AudioSample *sample{};
            {
                std::unique_lock lock(read_mutex);
                read_ready.wait(lock, [&] { return stopping || !reads.empty(); });
                if (stopping) return;
                sample = reads.front(); reads.pop_front();
            }
            const auto begin = capture_timings ? AudioQpc() : 0;
            const auto sample_index = static_cast<std::size_t>(sample - samples.get());
            std::optional<Result> read_result;
            bool success = false;
            try
            {
                read_result.emplace(Read(*sample));
                success = read_result->Succeeded();
            }
            catch (const std::exception &error)
            {
                ReportReaderFailure(sample_index, ErrorCode::TaskFailed, "exception", error.what());
            }
            catch (...)
            {
                ReportReaderFailure(sample_index, ErrorCode::TaskFailed, "exception", "unknown exception");
            }
            if (!success) sample->bytes.clear();
            if (capture_timings)
            {
                const auto end = AudioQpc();
                reader_read_ticks.fetch_add(end - begin, std::memory_order_relaxed);
                reader_queue_ticks.fetch_add(begin - sample->requested_qpc, std::memory_order_relaxed);
                reader_request_ready_ticks.fetch_add(end - sample->requested_qpc, std::memory_order_relaxed);
                reader_bytes.fetch_add(success ? sample->data_size : 0, std::memory_order_relaxed);
                reader_failures.fetch_add(!success, std::memory_order_relaxed);
                reader_jobs.fetch_add(1, std::memory_order_release);
            }
            sample->state.store(success ? SampleState::Ready : SampleState::Failed, std::memory_order_release);
            if (wake_main) wake_main(wake_context);
            if (!success && read_result)
                ReportReaderFailure(sample_index, read_result->Code(),
                                    read_result->Subsystem(), read_result->Message());
        }
    }
    void EvictStopped() noexcept
    {
        runtime_detail::DiagnosticScope scope(Work(AudioWorkId::Evict), Clock());
        for (const auto index : transition_cues)
        {
            const auto &cue = cues[index];
            auto &playing = State(cue);
            if (playing.prepared) continue;
            bool owned = false, pinned = false;
            for (std::size_t i = 0; i < cue.sample_count; ++i)
            {
                const auto &sample = samples[cue.first_sample + i];
                const auto state = sample.state.load(std::memory_order_acquire);
                owned |= state == SampleState::Ready || state == SampleState::Failed;
                pinned |= sample.native_pins.load(std::memory_order_acquire) != 0;
            }
            if (!owned || pinned) continue;
            // Native completion was observed by Reclaim. Destroy idle voices
            // before releasing the bytes they previously borrowed.
            for (auto &voice : active_voices)
                if (voice.voice && voice.cue.value == cue.id.value)
                { Retire(voice); }
            for (std::size_t i = 0; i < cue.sample_count; ++i)
            {
                auto *sample = &samples[cue.first_sample + i];
                const auto state = sample->state.load(std::memory_order_acquire);
                if (state == SampleState::Ready || state == SampleState::Failed)
                {
                    sample->pcm = {};
                    std::vector<std::byte>().swap(sample->bytes);
                    sample->state.store(SampleState::Empty, std::memory_order_release);
                }
            }
            playing.read_failure_reported = false;
        }
    }

    void ReleaseNative() noexcept
    {
        spatial_matrix_valid = false;
        for (auto &active : active_voices)
        {
            if (active.voice)
            {
                Retire(active);
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
        runtime_detail::DiagnosticScope scope(Work(AudioWorkId::Reclaim), Clock());
        status.active_source_voices = 0;
        for (auto &active : active_voices)
        {
            status.active_source_voices += Observe(active);
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
        status.effective_master_volume = status.background_muted ? 0.0f : master_volume;
        const auto apply = [&](IXAudio2SubmixVoice *bus, float volume) {
            if (!bus) return true;
            const auto result = bus->SetVolume(volume);
            if (SUCCEEDED(result)) return true;
            Failure({AudioNativeOperation::Volume, result}); EnterSilent(result); return false;
        };
        if (!apply(master_bus, status.effective_master_volume)) return;
        if (!apply(bgm_bus, status.combat_paused ? 0.0f : bgm_volume)) return;
        if (!apply(sfx_bus, status.combat_paused ? 0.0f : sfx_volume)) return;
        (void)apply(ui_bus, ui_volume);
    }
};

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>())
{
}

AudioEngine::~AudioEngine()
{
    Shutdown();
}

void AudioEngine::RetainGeneration(std::shared_ptr<const void> generation)
{
    impl_->generation_lease = std::move(generation);
}

Result AudioEngine::InitializePlayback(const SettingsData &settings)
{
    impl_->status.loaded_cues = static_cast<std::uint32_t>(impl_->cues.size());
    for (const auto &cue : impl_->cues)
        for (std::size_t i = 0; i < cue.sample_count; ++i)
            impl_->samples[cue.first_sample + i].preload = cue.preload;
    try
    {
        impl_->playback = std::make_unique<Impl::CuePlayback[]>(impl_->cues.size());
        impl_->transition_cues.clear();
        for (std::uint32_t i = 0; i < impl_->cues.size(); ++i)
            if (!impl_->cues[i].preload) impl_->transition_cues.push_back(i);
        impl_->reader = std::thread([this] { impl_->RunReader(); });
    }
    catch (const std::exception &error)
    { Shutdown(); return Result::Failure(ErrorCode::TaskFailed, "audio", error.what()); }
    return InitializeDevice(settings);
}

Result AudioEngine::InitializePrepared(const SettingsData &settings, DeveloperAudioCatalog catalog)
{
    impl_->developer_cues = std::move(catalog.cues);
    std::ranges::sort(impl_->developer_cues, {}, [](const AudioCueRow &cue) { return cue.id.value; });
    impl_->cues = impl_->developer_cues;
    impl_->samples = std::move(catalog.samples);
    impl_->sample_count = catalog.sample_count;
    return InitializePlayback(settings);
}

Result AudioEngine::Initialize(const SettingsData &settings, const AudioImage &image, const ShippedContentOwner &owner)
{
    Shutdown();
    impl_->content = &owner;
    try
    {
        // Shipping rows remain in generated order and storage. Only PCM and
        // mutable playback state belong to the engine.
        impl_->cues = image.cues;
        impl_->sample_count = image.samples.size();
        impl_->samples = std::make_unique<AudioSample[]>(impl_->sample_count);
        for (std::size_t i = 0; i < image.samples.size(); ++i)
        {
            const auto &range = image.samples[i];
            auto &sample = impl_->samples[i];
            sample.data_offset = range.offset; sample.data_size = range.bytes;
            const auto align = static_cast<WORD>(range.channels * (range.bits_per_sample / 8));
            sample.format = {WAVE_FORMAT_PCM, range.channels, range.sample_rate,
                              range.sample_rate * align, align, range.bits_per_sample, 0};
        }
        std::size_t resident_bytes{};
        for (const auto &chunk : image.chunks)
            if (chunk.preload) resident_bytes += static_cast<std::size_t>(chunk.bytes);
        impl_->resident_pcm.resize(resident_bytes);
        // Generated chunks retain contiguous preload reads and separate transition owners.
        std::size_t resident_offset{};
        for (const auto &chunk : image.chunks)
        {
            for (std::size_t i = 0; i < chunk.sample_count; ++i)
                impl_->samples[chunk.first_sample + i].chunk = &chunk;
            if (!chunk.preload) continue;
            auto pcm = std::span(impl_->resident_pcm).subspan(resident_offset, static_cast<std::size_t>(chunk.bytes));
            if (auto loaded = owner.ReadVerified(chunk.offset, pcm, chunk.digest); !loaded)
            { Shutdown(); return loaded; }
            std::size_t sample_offset{};
            for (std::size_t i = 0; i < chunk.sample_count; ++i)
            {
                auto &sample = impl_->samples[chunk.first_sample + i];
                sample.pcm = pcm.subspan(sample_offset, static_cast<std::size_t>(sample.data_size));
                sample_offset += static_cast<std::size_t>(sample.data_size);
                sample.state.store(SampleState::Ready, std::memory_order_release);
            }
            resident_offset += static_cast<std::size_t>(chunk.bytes);
        }
        return InitializePlayback(settings);
    }
    catch (const std::exception &error)
    { Shutdown(); return Result::Failure(ErrorCode::TaskFailed, "audio", error.what()); }
}

bool AudioEngine::PrepareCue(AssetId id) noexcept
{
    return RequestCue(id) == AudioCueReadiness::Ready;
}

AudioCueReadiness AudioEngine::RequestCue(AssetId id) noexcept
{
    runtime_detail::DiagnosticScope scope(impl_->Work(AudioWorkId::PrepareCue), impl_->Clock());
    auto *cue = impl_->Find(id);
    if (!cue) return AudioCueReadiness::Missing;
    auto &playing = impl_->State(*cue);
    playing.prepared = true;
    bool ready = true, failed = false;
    try
    {
        for (std::size_t i = 0; i < cue->sample_count; ++i)
        {
            auto *sample = &impl_->samples[cue->first_sample + i];
            auto state = sample->state.load(std::memory_order_acquire);
            failed |= state == SampleState::Failed;
            if (state == SampleState::Failed && !playing.read_failure_reported)
            {
                ++impl_->status.skipped_load_failures;
                playing.read_failure_reported = true;
            }
            if (state == SampleState::Empty)
            {
                std::lock_guard lock(impl_->read_mutex);
                sample->requested_qpc = impl_->capture_timings ? AudioQpc() : 0;
                impl_->reads.push_back(sample);
                sample->state.store(SampleState::Queued, std::memory_order_release);
                impl_->read_ready.notify_one();
            }
            ready &= state == SampleState::Ready;
        }
    }
    catch (...) { ++impl_->status.skipped_load_failures; return AudioCueReadiness::Failed; }
    return failed ? AudioCueReadiness::Failed : ready ? AudioCueReadiness::Ready : AudioCueReadiness::Pending;
}

AudioCueReadiness AudioEngine::CueReadiness(AssetId id) const noexcept
{
    const auto *cue = impl_->Find(id);
    if (!cue) return AudioCueReadiness::Missing;
    bool pending = false;
    for (std::size_t i = 0; i < cue->sample_count; ++i)
    {
        const auto state = impl_->samples[cue->first_sample + i].state.load(std::memory_order_acquire);
        if (state == SampleState::Failed) return AudioCueReadiness::Failed;
        pending |= state != SampleState::Ready;
    }
    return pending ? AudioCueReadiness::Pending : AudioCueReadiness::Ready;
}

bool AudioEngine::IsTransitionTrack(AssetId id) const noexcept
{
    const auto *cue = impl_->Find(id);
    return cue && cue->bus == AudioBus::Bgm && !cue->preload;
}

Result AudioEngine::InitializeDevice(const SettingsData &settings)
{
    runtime_detail::DiagnosticScope scope(impl_->Work(AudioWorkId::InitializeDevice), impl_->Clock());
    ++impl_->device_epoch;
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
    if (device_details.InputChannels == 0 || device_details.InputChannels > impl_->spatial_matrix.size())
    {
        impl_->EnterSilent(E_INVALIDARG);
        ApplySettings(settings);
        return Result::Success();
    }
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

    impl_->spatial_matrix_valid = false;
    impl_->listener.OrientFront = {0.0f, 0.0f, 1.0f};
    impl_->listener.OrientTop = {0.0f, 1.0f, 0.0f};
    impl_->device_channels = device_details.InputChannels;
    impl_->native_timings.output_sample_rate = device_details.InputSampleRate;
    impl_->status.device_state = AudioDeviceState::Ready;
    impl_->status.native_error = 0;
    ApplySettings(settings);
    return Result::Success();
}

void AudioEngine::Shutdown() noexcept
{
    {
        std::lock_guard lock(impl_->read_mutex);
        impl_->stopping = true;
    }
    impl_->read_ready.notify_one();
    // DestroyVoice joins callbacks/native PCM reads while every sample owner is
    // still alive. An in-flight disk read joins next, before any backing clears.
    impl_->ReleaseNative();
    if (impl_->reader.joinable()) impl_->reader.join();
    impl_->reads.clear();
    impl_->cues = {};
    impl_->developer_cues.clear();
    impl_->transition_cues.clear();
    impl_->playback.reset();
    impl_->samples.reset();
    std::vector<std::byte>().swap(impl_->resident_pcm);
    impl_->sample_count = 0;
    impl_->content = nullptr;
    impl_->stopping = false;
    impl_->generation_lease.reset();
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

void AudioEngine::SetBackgroundMuted(bool muted) noexcept
{
    if (impl_->status.background_muted == muted)
        return;
    impl_->status.background_muted = muted;
    impl_->ApplyVolumes();
}

AudioPlayResult AudioEngine::Play(const PresentationEvent &event, std::uint64_t dequeued_qpc) noexcept
{
    runtime_detail::DiagnosticScope scope(impl_->Work(AudioWorkId::Play), impl_->Clock());
    runtime_detail::DiagnosticScope admission(impl_->Work(AudioWorkId::Admission), impl_->Clock());
    try
    {
    if (event.kind != PresentationKind::Audio || event.asset.value == 0) return AudioPlayResult::Ignored;
    const auto *cue_ptr = impl_->Find(event.asset);
    if (!cue_ptr)
    {
        impl_->Reclaim(); ++impl_->status.skipped_missing_cues; return AudioPlayResult::MissingCue;
    }
    const auto &cue = *cue_ptr;
    auto &playing = impl_->State(cue);
    Impl::ActiveVoice *free_slot{}, *victim{}, *first_same_cue{};
    std::uint32_t same_cue{}, same_priority{}, recent_arrow_impacts{};
    std::array<Sequence, 6> recent_arrow_sequences{};
    struct ArrowCandidate { std::uint64_t started_ms; Sequence sequence; };
    std::array<ArrowCandidate, kMaximumSourceVoices> arrows;
    std::size_t arrow_count{};
    impl_->status.active_source_voices = 0;
    // Observe native completion and decide admission in the same slot pass.
    // The shared retrigger/arrow clock is still read after native observations.
    for (auto &active : impl_->active_voices)
    {
        impl_->status.active_source_voices += impl_->Observe(active);
        if (active.releasing) continue;
        if (!active.voice || !active.queued)
        {
            if (!free_slot) free_slot = &active;
            continue;
        }
        if (!victim || active.priority < victim->priority ||
            (active.priority == victim->priority && active.serial < victim->serial))
            victim = &active;
        if (active.cue.value == event.asset.value)
        {
            ++same_cue;
            if (!first_same_cue) first_same_cue = &active;
        }
        if (active.priority == cue.priority) ++same_priority;
        if (cue.arrow_impact_family && active.arrow_impact_family)
            arrows[arrow_count++] = {active.started_ms, active.event_sequence};
    }
    const auto now = GetTickCount64();
    for (std::size_t i = 0; i < arrow_count; ++i)
    {
        const auto &arrow = arrows[i];
        if (now - arrow.started_ms < 30 &&
            recent_arrow_impacts < recent_arrow_sequences.size() &&
            std::find(recent_arrow_sequences.begin(),
                      recent_arrow_sequences.begin() + recent_arrow_impacts,
                      arrow.sequence) == recent_arrow_sequences.begin() + recent_arrow_impacts)
            recent_arrow_sequences[recent_arrow_impacts++] = arrow.sequence;
    }
    if (playing.has_played && now - playing.last_play_ms < cue.retrigger_ms)
    {
        if (event.asset.value == MakeAssetId("audio.pickup.xp_collect").value && first_same_cue)
        {
            ++first_same_cue->aggregation_count;
            const auto pitch = first_same_cue->voice->SetFrequencyRatio(
                std::min(1.0f + 0.03f * static_cast<float>(first_same_cue->aggregation_count - 1), 1.5f));
            if (FAILED(pitch))
            {
                impl_->Failure({AudioNativeOperation::Frequency, pitch});
                impl_->Retire(*first_same_cue); --impl_->status.active_source_voices;
                return AudioPlayResult::NativeFailure;
            }
        }
        return AudioPlayResult::Retriggered;
    }
    if (same_cue >= cue.max_simultaneous) return AudioPlayResult::Limited;
    if (same_priority >= PrioritySoftCap(cue.priority) || recent_arrow_impacts >= 6)
    {
        ++impl_->status.skipped_voice_limits;
        return AudioPlayResult::Limited;
    }
    const auto index = static_cast<std::size_t>(event.sequence % cue.sample_count);
    auto *sample = &impl_->samples[cue.first_sample + index];
    if (sample->state.load(std::memory_order_acquire) != SampleState::Ready)
    {
        ++impl_->status.skipped_load_failures;
        return AudioPlayResult::NotReady;
    }
    if (!impl_->engine || !impl_->device_voice) return AudioPlayResult::Silent;
    admission.Stop();
    auto *selected = free_slot ? free_slot : victim;
    if (!free_slot && (!selected || selected->priority >= cue.priority)) return AudioPlayResult::Limited;
    const auto spatial_matrix = cue.spatial && sample->format.nChannels == 1
        ? impl_->SpatialMatrix(event.position, cue.priority) : std::span<const float>{};
    auto *target_bus = cue.bus == AudioBus::Sfx ? impl_->sfx_bus : cue.bus == AudioBus::Bgm ? impl_->bgm_bus : impl_->ui_bus;
    auto *destination = target_bus ? target_bus : impl_->master_bus;
    XAUDIO2_SEND_DESCRIPTOR send{0, destination}; XAUDIO2_VOICE_SENDS sends{1, &send};
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = static_cast<UINT32>(sample->pcm.size());
    buffer.pAudioData = reinterpret_cast<const BYTE *>(sample->pcm.data());
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    if (cue.loop) buffer.LoopCount = XAUDIO2_LOOP_INFINITE;

    runtime_detail::DiagnosticScope native(impl_->Work(AudioWorkId::NativeVoice), impl_->Clock());
    const bool reuse = selected->voice && sample->preload && !selected->completion && selected->sample &&
                       SameWaveFormat(selected->sample->format, sample->format);
    struct Candidate
    {
        IXAudio2SourceVoice *voice{}; AudioBufferCompletion *completion{};
        ~Candidate() { DestroyOwnedVoice(voice, completion); }
    } candidate;
    if (!reuse)
    {
        if (!sample->preload)
        {
            const auto slot = static_cast<std::size_t>(selected - impl_->active_voices.data());
            for (auto &record : impl_->completions[slot])
                if (!record.claimed) { candidate.completion = &record; break; }
            if (!candidate.completion) { ++impl_->status.skipped_voice_limits; return AudioPlayResult::Limited; }
            candidate.completion->wake_main = impl_->wake_main;
            candidate.completion->wake_context = impl_->wake_context;
            candidate.completion->Begin(sample->native_pins,
                {impl_->device_epoch, ++impl_->voice_generations[slot], ++impl_->buffer_sequence});
            buffer.pContext = candidate.completion;
        }
        const auto created = impl_->engine->CreateSourceVoice(&candidate.voice, &sample->format, 0, 2.0f,
            candidate.completion, nullptr, nullptr);
        if (FAILED(created))
        {
            impl_->Failure({AudioNativeOperation::CreateVoice, created});
            return AudioPlayResult::NativeFailure; // Old playback is still untouched.
        }
    }
    const bool old_queued = selected->queued;
    if (old_queued) --impl_->status.active_source_voices;
    selected->queued = false;
    if (!reuse && selected->voice)
    {
        const auto stopped = StopFlushVoice(*selected->voice);
        impl_->Failure(stopped);
        const auto retired = impl_->Retire(*selected); // Joins callbacks before their record can be reused.
        if (!stopped.Succeeded() || !retired.Succeeded())
            return AudioPlayResult::NativeFailure;
    }
    auto *source = reuse ? selected->voice : candidate.voice;
    std::uint64_t submit_qpc{};
    const bool start = !impl_->status.combat_paused || !IsCombatBus(cue.bus);
    const auto submitted = SubmitPreparedVoice(*source, sends, destination, impl_->device_channels,
        spatial_matrix, buffer,
        dequeued_qpc ? impl_->Clock() : nullptr, &submit_qpc, start);
    if (!submitted.Succeeded())
    {
        impl_->Failure(submitted);
        const auto cleanup = StopFlushVoice(*source);
        impl_->Failure(cleanup, true);
        if (reuse && cleanup.Succeeded())
        {
            // Flush success alone is not completion. This resident owner is retained
            // and excluded from admission until the next native observation proves idle.
            selected->sample = sample;
            selected->queued = impl_->Observe(*selected);
            selected->releasing = selected->queued;
            impl_->status.active_source_voices += selected->queued;
        }
        else if (reuse) impl_->Retire(*selected);
        return AudioPlayResult::NativeFailure; // New candidate guard destroys before releasing its PCM pin.
    }
    selected->voice = source;
    if (!reuse)
    {
        selected->completion = candidate.completion;
        candidate.voice = nullptr; candidate.completion = nullptr;
    }
    selected->cue = cue.id; selected->bus = cue.bus; selected->priority = cue.priority;
    selected->sample = sample; selected->position = event.position; selected->spatial = cue.spatial;
    selected->arrow_impact_family = cue.arrow_impact_family; selected->event_sequence = event.sequence;
    selected->aggregation_count = 1; selected->started_ms = now; selected->serial = ++impl_->voice_serial;
    selected->queued = true; selected->releasing = false; selected->session_id = event.session_id;
    playing.last_play_ms = now; playing.has_played = true;
    ++impl_->status.played_count; ++impl_->status.active_source_voices;
    if (submit_qpc && submit_qpc >= dequeued_qpc)
    {
        const auto elapsed = submit_qpc - dequeued_qpc;
        impl_->native_timings.drain_submit_ticks += elapsed;
        impl_->native_timings.drain_submit_max_ticks = std::max(impl_->native_timings.drain_submit_max_ticks, elapsed);
        ++impl_->native_timings.drain_submit_calls;
    }
    return start ? AudioPlayResult::Started : AudioPlayResult::QueuedPaused;
    }
    catch (...) { ++impl_->status.skipped_load_failures; return AudioPlayResult::NativeFailure; }
}

void AudioEngine::Stop(AssetId cue) noexcept
{
    runtime_detail::DiagnosticScope scope(impl_->Work(AudioWorkId::Stop), impl_->Clock());
    if (const auto *row = impl_->Find(cue)) impl_->State(*row).prepared = false;
    impl_->status.active_source_voices = 0;
    for (auto &active : impl_->active_voices)
    {
        if (active.voice && active.cue.value == cue.value)
        {
            const auto result = StopFlushVoice(*active.voice);
            if (!result.Succeeded()) { impl_->Failure(result); impl_->Retire(active); }
            else active.releasing = true;
        }
        impl_->status.active_source_voices += impl_->Observe(active);
    }
}

void AudioEngine::StopBus(AudioBus bus) noexcept
{
    runtime_detail::DiagnosticScope scope(impl_->Work(AudioWorkId::Stop), impl_->Clock());
    for (const auto &cue : impl_->cues)
        if (cue.bus == bus) impl_->State(cue).prepared = false;
    impl_->status.active_source_voices = 0;
    for (auto &active : impl_->active_voices)
    {
        if (active.voice && active.bus == bus)
        {
            const auto result = StopFlushVoice(*active.voice);
            if (!result.Succeeded()) { impl_->Failure(result); impl_->Retire(active); }
            else active.releasing = true;
        }
        impl_->status.active_source_voices += impl_->Observe(active);
    }
}

void AudioEngine::BeginSession(std::uint64_t session_id) noexcept
{
    impl_->status.active_source_voices = 0;
    for (auto &active : impl_->active_voices)
    {
        if (active.voice && active.bus != AudioBus::Bgm && active.session_id != 0 && active.session_id != session_id)
        {
            impl_->Failure(StopFlushVoice(*active.voice));
            impl_->Retire(active); // Session close joins native reads before any transition eviction.
        }
        impl_->status.active_source_voices += impl_->Observe(active);
    }
    // Reset once at the queue boundary, before its first event. Within the
    // session all producers still share the original per-cue retrigger clock.
    for (const auto &cue : impl_->cues) impl_->State(cue).has_played = false;
}

void AudioEngine::UpdateListener(Float3 position, Float3 forward, Float3 up) noexcept
{
    runtime_detail::DiagnosticScope scope(impl_->Work(AudioWorkId::Listener), impl_->Clock());
    impl_->Reclaim();
    impl_->EvictStopped();
    const X3DAUDIO_VECTOR next_position{position.x, position.y, position.z};
    const auto next_front = Normalize(forward, {0.0f, 0.0f, 1.0f});
    const auto next_top = Normalize(up, {0.0f, 1.0f, 0.0f});
    const auto equal = [](const X3DAUDIO_VECTOR &a, const X3DAUDIO_VECTOR &b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    };
    if (equal(impl_->listener.Position, next_position) &&
        equal(impl_->listener.OrientFront, next_front) &&
        equal(impl_->listener.OrientTop, next_top)) return;
    impl_->spatial_matrix_valid = false;
    impl_->listener.Position = next_position;
    impl_->listener.OrientFront = next_front;
    impl_->listener.OrientTop = next_top;
    for (auto &active : impl_->active_voices)
    {
        if (!active.voice || !active.queued || !active.spatial || !active.sample ||
            active.sample->format.nChannels != 1)
            continue;
        const auto matrix = impl_->SpatialMatrix(active.position, active.priority);
        const auto target_bus = active.bus == AudioBus::Sfx ? impl_->sfx_bus :
                                active.bus == AudioBus::Bgm ? impl_->bgm_bus : impl_->ui_bus;
        runtime_detail::DiagnosticScope native(impl_->Work(AudioWorkId::NativeVoice), impl_->Clock());
        const auto result = active.voice->SetOutputMatrix(target_bus ? target_bus : impl_->master_bus,
                                                          1, impl_->device_channels, matrix.data());
        if (FAILED(result))
        {
            impl_->Failure({AudioNativeOperation::Matrix, result});
            impl_->Retire(active);
            --impl_->status.active_source_voices;
        }
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
        if (active.voice && active.queued && IsCombatBus(active.bus))
        {
            const auto result = active.voice->Stop();
            if (FAILED(result))
            {
                impl_->Failure({AudioNativeOperation::Stop, result});
                impl_->Retire(active); --impl_->status.active_source_voices;
            }
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
        if (active.voice && active.queued && IsCombatBus(active.bus))
        {
            const auto result = active.voice->Start();
            if (FAILED(result))
            {
                impl_->Failure({AudioNativeOperation::Start, result});
                impl_->Retire(active); --impl_->status.active_source_voices;
            }
        }
    }
    impl_->ApplyVolumes();
}

AudioStatus AudioEngine::Status() const noexcept
{
    auto status = impl_->status;
    status.loaded_files = 0;
    for (std::size_t i = 0; i < impl_->sample_count; ++i)
        status.loaded_files += impl_->samples[i].state.load(std::memory_order_acquire) == SampleState::Ready;
    return status;
}

AudioNativeOperation AudioEngine::FailedNativeOperation() const noexcept
{
    return impl_->status.native_operation;
}

void AudioEngine::SetCaptureTimings(bool enabled) noexcept
{
    // Changing capture while the reader runs would change its counter ownership.
    if (impl_->reader.joinable()) return;
    impl_->capture_timings = enabled;
    LARGE_INTEGER frequency{};
    if (enabled) QueryPerformanceFrequency(&frequency);
    impl_->native_timings.qpc_frequency = static_cast<std::uint64_t>(frequency.QuadPart);
}

void AudioEngine::SetWakeCallback(void (*notify)(void *) noexcept, void *context) noexcept
{
    if (impl_->reader.joinable()) return;
    impl_->wake_main = notify; impl_->wake_context = context;
}

AudioWorkTimings AudioEngine::CaptureTimings() noexcept
{
    if (!impl_->capture_timings) return {};
    const auto now = AudioQpc();
    auto &native = impl_->native_timings;
    native.native_available = static_cast<bool>(impl_->engine);
    if (impl_->engine && (native.native_samples == 0 || now - native.native_sample_qpc >= native.qpc_frequency))
    {
        XAUDIO2_PERFORMANCE_DATA data{};
        impl_->engine->GetPerformanceData(&data);
        native.native_available = true;
        native.native_sample_qpc = now;
        ++native.native_samples;
        native.audio_cycles = data.AudioCyclesSinceLastQuery;
        native.total_cycles = data.TotalCyclesSinceLastQuery;
        native.minimum_cycles_per_quantum = data.MinimumCyclesPerQuantum;
        native.maximum_cycles_per_quantum = data.MaximumCyclesPerQuantum;
        native.native_active_voices = data.ActiveSourceVoiceCount;
        native.native_total_voices = data.TotalSourceVoiceCount;
        native.glitches = data.GlitchesSinceEngineStarted;
        native.latency_samples = data.CurrentLatencyInSamples;
    }
    auto result = native;
    result.captured = true;
    for (std::size_t i = 0; i < impl_->work.size(); ++i)
        result.scopes[i] = {impl_->work[i].ticks, impl_->work[i].calls};
    result.reader_jobs = impl_->reader_jobs.load(std::memory_order_acquire);
    result.reader_failures = impl_->reader_failures.load(std::memory_order_relaxed);
    result.reader_bytes = impl_->reader_bytes.load(std::memory_order_relaxed);
    result.reader_read_ticks = impl_->reader_read_ticks.load(std::memory_order_relaxed);
    result.reader_queue_ticks = impl_->reader_queue_ticks.load(std::memory_order_relaxed);
    result.reader_request_ready_ticks = impl_->reader_request_ready_ticks.load(std::memory_order_relaxed);
    return result;
}

} // namespace hs
