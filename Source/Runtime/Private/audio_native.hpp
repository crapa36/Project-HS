#pragma once
#include <hs/runtime/audio_engine.hpp>
#include <Windows.h>
#include <xaudio2.h>
#include <atomic>
#include <span>

namespace hs::audio_detail
{
struct NativeAudioResult
{
    AudioNativeOperation operation{AudioNativeOperation::None};
    HRESULT error{S_OK};
    bool Succeeded() const noexcept { return SUCCEEDED(error); }
};

template<class Voice> NativeAudioResult StopFlushVoice(Voice &voice) noexcept
{
    const auto stopped = voice.Stop();
    if (FAILED(stopped)) return {AudioNativeOperation::Stop, stopped};
    const auto flushed = voice.FlushSourceBuffers();
    if (FAILED(flushed)) return {AudioNativeOperation::Flush, flushed};
    return {};
}

// Shared with the CPU failure fixture: each destructive boundary has its own receipt.
template<class Voice> NativeAudioResult SubmitPreparedVoice(Voice &voice,
    const XAUDIO2_VOICE_SENDS &sends, IXAudio2Voice *target, std::uint32_t channels,
    std::span<const float> matrix, const XAUDIO2_BUFFER &buffer,
    std::uint64_t (*clock)() noexcept = nullptr, std::uint64_t *submit_qpc = nullptr,
    bool start = true) noexcept
{
    if (const auto result = StopFlushVoice(voice); !result.Succeeded()) return result;
    const auto frequency = voice.SetFrequencyRatio(1.0f);
    if (FAILED(frequency)) return {AudioNativeOperation::Frequency, frequency};
    const auto route = voice.SetOutputVoices(&sends);
    if (FAILED(route)) return {AudioNativeOperation::Route, route};
    if (!matrix.empty())
    {
        const auto applied = voice.SetOutputMatrix(target, 1, channels, matrix.data());
        if (FAILED(applied)) return {AudioNativeOperation::Matrix, applied};
    }
    if (clock && submit_qpc) *submit_qpc = clock();
    const auto submitted = voice.SubmitSourceBuffer(&buffer);
    if (FAILED(submitted)) return {AudioNativeOperation::Submit, submitted};
    if (start)
    {
        const auto started = voice.Start();
        if (FAILED(started)) return {AudioNativeOperation::Start, started};
    }
    return {};
}

struct AudioBufferToken
{
    std::uint64_t device_epoch{}, voice_generation{}, buffer_sequence{};
    bool operator==(const AudioBufferToken &) const = default;
};

// Only evictable PCM uses a record. The record is never reused before its voice
// is destroyed, even if OnBufferEnd has already released its one buffer pin.
struct AudioBufferCompletion final : IXAudio2VoiceCallback
{
    AudioBufferToken token{};
    std::atomic<std::uint32_t> *pins{};
    std::atomic<bool> pending{};
    std::atomic<HRESULT> native_error{S_OK};
    bool claimed{}; // Main owner only.
    void (*wake_main)(void *) noexcept{};
    void *wake_context{};

    void Begin(std::atomic<std::uint32_t> &owner_pins, AudioBufferToken identity) noexcept
    {
        token = identity; pins = &owner_pins; claimed = true;
        native_error.store(S_OK, std::memory_order_relaxed);
        pins->fetch_add(1, std::memory_order_relaxed);
        pending.store(true, std::memory_order_release);
    }
    void Complete() noexcept
    {
        if (pending.exchange(false, std::memory_order_acq_rel))
        {
            pins->fetch_sub(1, std::memory_order_release);
            if (wake_main) wake_main(wake_context);
        }
    }
    void AfterVoiceQuiescence() noexcept
    {
        Complete(); claimed = false; pins = nullptr;
    }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnBufferStart(void *) noexcept override {}
    void STDMETHODCALLTYPE OnBufferEnd(void *context) noexcept override
    {
        if (context == this) Complete();
    }
    void STDMETHODCALLTYPE OnLoopEnd(void *) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceError(void *context, HRESULT error) noexcept override
    {
        if (!context || context == this)
        {
            native_error.store(error, std::memory_order_release);
            if (wake_main) wake_main(wake_context);
        }
    }
};

template<class Voice> void DestroyOwnedVoice(Voice *&voice, AudioBufferCompletion *&completion) noexcept
{
    // XAudio2 DestroyVoice guarantees no further native reads or callbacks on return.
    if (voice) { voice->DestroyVoice(); voice = nullptr; }
    if (completion) { completion->AfterVoiceQuiescence(); completion = nullptr; }
}
}
