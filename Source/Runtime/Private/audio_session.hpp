#pragma once
#include "runtime_channels.hpp"
#include "session_music.hpp"
#include <hs/runtime/audio_engine.hpp>
#include <array>
#include <optional>

namespace hs::runtime_detail
{
enum class MainAudioFailure : std::uint8_t { None, MissingCue, Preparation, Native };
struct MainAudioFailureDetails
{
    AssetId cue{};
    AudioCueReadiness readiness{AudioCueReadiness::Missing};
    AudioPlayResult play_result{AudioPlayResult::Ignored};
    std::uint64_t session_id{};
    Tick tick{};
    SessionPhase phase{SessionPhase::MainMenu};
    const char *stage{"none"};
};

class MainAudioSession
{
  public:
    MainAudioSession(AudioEngine &audio, Sequence &sequence)
        : audio_(audio), sequence_(sequence)
    { RequestMusic(Event("audio.bgm.main_menu")); }

    void Drain(BoundedSpscQueue<QueuedAudioEvent, 1024> &simulation)
    {
        // Select the latest session boundary, then consume its packets in producer order.
        std::size_t count{};
        auto newest = session_id_;
        bool saw_boundary = initialized_;
        while (count < batch_.size() && simulation.TryPop(batch_[count]))
        {
            const auto &packet = batch_[count++];
            if (packet.kind == QueuedAudioEvent::Kind::Tick)
            {
                newest = packet.tick_info.session_id;
                saw_boundary = true;
            }
        }
        if (!saw_boundary) return;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (failure_ != MainAudioFailure::None) break;
            const auto &packet = batch_[i];
            if (packet.kind == QueuedAudioEvent::Kind::Tick)
            {
                if (packet.tick_info.session_id == newest) ApplyTick(packet.tick_info);
            }
            else if (packet.event.session_id == newest) Consume(packet.event);
        }
    }
    void Update()
    {
        if (failure_ != MainAudioFailure::None) return;
        if (audio_.FailedNativeOperation() != AudioNativeOperation::None)
        { Fail(MainAudioFailure::Native, {}, "native_update"); return; }
        // Both independent owner intents must retain their PCM before reclaim/eviction.
        const auto music_ready = pending_music_ ? audio_.RequestCue(pending_music_->asset) : AudioCueReadiness::Ready;
        const auto ambience_ready = pending_ambience_ ? audio_.RequestCue(pending_ambience_->asset) : AudioCueReadiness::Ready;
        audio_.UpdateListener(listener_);
        if (audio_.FailedNativeOperation() != AudioNativeOperation::None)
        { Fail(MainAudioFailure::Native, {}, "listener_update"); return; }
        if (pending_music_ && Prepared(music_ready, pending_music_->asset, "music_prepare"))
        {
            const auto previous_music = active_music_;
            const bool must_stop_before_play = pending_music_restart_ ||
                (previous_music.value && previous_music.value == pending_music_->asset.value);
            // For a different cue, admission is the commit point: a Limited result
            // leaves the current track and its owner intent intact for the next Update.
            if (must_stop_before_play && previous_music.value)
            { audio_.Stop(previous_music); active_music_ = {}; }
            const auto result = audio_.Play(*pending_music_);
            if (result != AudioPlayResult::Limited)
            {
                CheckPlay(result, pending_music_->asset, "music_play");
                if (failure_ == MainAudioFailure::None)
                {
                    active_music_ = pending_music_->asset;
                    if (!must_stop_before_play && previous_music.value &&
                        previous_music.value != active_music_.value)
                        audio_.Stop(previous_music);
                }
                pending_music_.reset();
                pending_music_restart_ = false;
            }
        }
        if (failure_ != MainAudioFailure::None) return;
        StartPreparedAmbience(ambience_ready);
    }
    MainAudioFailure Failure() const noexcept { return failure_; }
    const MainAudioFailureDetails &FailureDetails() const noexcept { return failure_details_; }
  private:
    void RequestMusic(PresentationEvent event, bool restart = false)
    {
        if (pending_music_ && pending_music_->asset.value == event.asset.value)
        {
            pending_music_restart_ = pending_music_restart_ || restart;
            return;
        }
        if (pending_music_)
        {
            audio_.Stop(pending_music_->asset); pending_music_.reset();
            pending_music_restart_ = false;
        }
        if (!restart && active_music_.value == event.asset.value) return;
        pending_music_ = event;
        pending_music_restart_ = restart;
        (void)audio_.RequestCue(event.asset);
    }
    PresentationEvent Event(std::string_view cue)
    {
        PresentationEvent event;
        event.sequence = ++sequence_; event.session_id = session_id_; event.tick = tick_;
        event.kind = PresentationKind::Audio; event.asset = MakeAssetId(cue);
        event.parameters = EncodeAudioAction(AudioEventAction::Play);
        return event;
    }
    void ApplyTick(const QueuedAudioEvent::TickInfo &facts)
    {
        const bool new_session = !initialized_ || session_id_ != facts.session_id;
        if (new_session)
        {
            if (pending_music_ && (initialized_ || facts.phase != SessionPhase::MainMenu))
            {
                audio_.Stop(pending_music_->asset); pending_music_.reset();
                pending_music_restart_ = false;
            }
            CancelAmbience(); // Close old-session active/pending SFX ownership before its first new sound.
            audio_.BeginSession(facts.session_id);
            session_id_ = facts.session_id; initialized_ = true;
            // The new boundary precedes the first sound, never resets its clock afterwards.
            phase_ = SessionPhase::MainMenu;
        }
        tick_ = facts.tick; listener_ = facts.listener_position;
        if (!new_session && phase_ == facts.phase) return;
        const auto old = phase_;
        phase_ = facts.phase;
        const bool paused = phase_ == SessionPhase::Paused || phase_ == SessionPhase::CardSelection ||
            phase_ == SessionPhase::StatAllocation || phase_ == SessionPhase::RelicSelection;
        if (paused) audio_.PauseCombat(); else audio_.ResumeCombat();
        const auto music = ResolveSessionMusic(phase_, new_session && phase_ == SessionPhase::Playing ? AssetId{} : active_music_);
        const bool restart_music = new_session && phase_ == SessionPhase::Playing;
        const bool change_music = restart_music || (pending_music_ ?
            pending_music_->asset.value != music.value : active_music_.value != music.value);
        if (music.value && change_music && (phase_ == SessionPhase::MainMenu || phase_ == SessionPhase::Playing ||
                            phase_ == SessionPhase::Victory || phase_ == SessionPhase::Defeat))
        {
            auto event = Event("audio.bgm.session"); event.asset = music;
            RequestMusic(event, restart_music);
        }
        if (phase_ == SessionPhase::Playing && !active_ambience_.value && !pending_ambience_)
        {
            // arena is deliberately nonpreloaded SFX with two complete variants.
            // Retain this one original event/sequence while its bytes prepare.
            pending_ambience_ = Event("audio.ambience.arena");
            // Ready resident reuse preserves its original position before phase UI sounds.
            StartPreparedAmbience(audio_.RequestCue(pending_ambience_->asset));
        }
        else if (phase_ != SessionPhase::Playing) CancelAmbience();
        if (phase_ == SessionPhase::Paused) PlayChecked(Event("audio.ui.pause_open"));
        else if (old == SessionPhase::Paused) PlayChecked(Event("audio.ui.pause_close"));
        else if (phase_ == SessionPhase::CardSelection || phase_ == SessionPhase::StatAllocation)
            PlayChecked(Event("audio.ui.level_up"));
        else if (phase_ == SessionPhase::RelicSelection) PlayChecked(Event("audio.ui.relic_select"));
    }
    void StartPreparedAmbience(AudioCueReadiness readiness)
    {
        if (failure_ != MainAudioFailure::None || !pending_ambience_ ||
            !Prepared(readiness, pending_ambience_->asset, "ambience_prepare")) return;
        const auto result = audio_.Play(*pending_ambience_);
        if (result == AudioPlayResult::Limited) return;
        CheckPlay(result, pending_ambience_->asset, "ambience_play");
        if (failure_ == MainAudioFailure::None) active_ambience_ = pending_ambience_->asset;
        pending_ambience_.reset();
    }
    void CancelAmbience()
    {
        if (pending_ambience_) { audio_.Stop(pending_ambience_->asset); pending_ambience_.reset(); }
        if (active_ambience_.value) { audio_.Stop(active_ambience_); active_ambience_ = {}; }
    }
    void Fail(MainAudioFailure failure, AssetId cue, const char *stage, AudioPlayResult result = AudioPlayResult::Ignored)
    {
        if (failure_ != MainAudioFailure::None) return;
        failure_ = failure;
        failure_details_ = {cue, audio_.CueReadiness(cue), result, session_id_, tick_, phase_, stage};
    }
    bool Prepared(AudioCueReadiness readiness, AssetId cue, const char *stage)
    {
        if (readiness == AudioCueReadiness::Failed) Fail(MainAudioFailure::Preparation, cue, stage);
        else if (readiness == AudioCueReadiness::Missing) Fail(MainAudioFailure::MissingCue, cue, stage);
        return readiness == AudioCueReadiness::Ready;
    }
    void CheckPlay(AudioPlayResult result, AssetId cue, const char *stage)
    {
        if (result == AudioPlayResult::NativeFailure) Fail(MainAudioFailure::Native, cue, stage, result);
        else if (result == AudioPlayResult::NotReady) Fail(MainAudioFailure::Preparation, cue, stage, result);
        else if (result == AudioPlayResult::MissingCue) Fail(MainAudioFailure::MissingCue, cue, stage, result);
    }
    void PlayChecked(const PresentationEvent &event)
    { CheckPlay(audio_.Play(event), event.asset, "event_play"); }
    void Consume(const PresentationEvent &event)
    {
        if (DecodeAudioAction(event.parameters) == AudioEventAction::Stop)
        {
            if (pending_music_ && pending_music_->asset.value == event.asset.value)
            { pending_music_.reset(); pending_music_restart_ = false; }
            if (pending_ambience_ && pending_ambience_->asset.value == event.asset.value) pending_ambience_.reset();
            if (active_music_.value == event.asset.value) active_music_ = {};
            if (active_ambience_.value == event.asset.value) active_ambience_ = {};
            audio_.Stop(event.asset);
        }
        else if (audio_.IsTransitionTrack(event.asset)) RequestMusic(event, false);
        else PlayChecked(event);
    }
    AudioEngine &audio_;
    Sequence &sequence_;
    std::array<QueuedAudioEvent, 1024> batch_{};
    std::optional<PresentationEvent> pending_music_, pending_ambience_;
    AssetId active_music_{}, active_ambience_{};
    bool pending_music_restart_{};
    Float3 listener_{};
    std::uint64_t session_id_{};
    Tick tick_{};
    SessionPhase phase_{SessionPhase::MainMenu};
    bool initialized_{};
    MainAudioFailure failure_{};
    MainAudioFailureDetails failure_details_{};
};
}
