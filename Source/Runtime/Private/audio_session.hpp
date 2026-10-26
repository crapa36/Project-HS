#pragma once
#include "runtime_channels.hpp"
#include "session_music.hpp"
#include <hs/runtime/audio_engine.hpp>
#include <array>
#include <optional>

namespace hs::runtime_detail
{
struct AudioDeliveryStats
{
    std::uint64_t delivered{}, discarded_old_session{}, tick_boundaries{};
    std::uint64_t event_age_ticks{}, maximum_event_age_ticks{}, event_age_samples{};
};

// Main owns the one bounded batch. Look ahead only for session identity, then
// replay the surviving session's boundaries/events in original producer FIFO.
class AudioDelivery
{
  public:
    explicit AudioDelivery(DiagnosticClock clock = nullptr) : clock_(clock) {}
    template<class Consume, class Observe>
    void DrainSimulation(BoundedSpscQueue<QueuedAudioEvent, 1024> &queue, Consume &&consume, Observe &&observe)
    {
        std::size_t count{};
        auto newest = session_id_;
        bool saw_boundary = initialized_;
        while (count < batch_.size() && queue.TryPop(batch_[count]))
        {
            batch_[count].dequeued_qpc = clock_ ? clock_() : 0;
            const auto &packet = batch_[count++];
            observe(packet, 0);
            if (packet.kind == QueuedAudioEvent::Kind::Tick)
            { newest = packet.tick_info.session_id; saw_boundary = true; }
        }
        if (!saw_boundary)
        {
            for (std::size_t i = 0; i < count; ++i) ++stats_.discarded_old_session;
            return;
        }
        session_id_ = newest;
        initialized_ = true;
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto &packet = batch_[i];
            const auto session = packet.kind == QueuedAudioEvent::Kind::Tick ?
                packet.tick_info.session_id : packet.event.session_id;
            if (session != session_id_)
            {
                if (packet.kind == QueuedAudioEvent::Kind::Event) ++stats_.discarded_old_session;
                continue;
            }
            if (packet.kind == QueuedAudioEvent::Kind::Tick)
            { tick_ = packet.tick_info.tick; ++stats_.tick_boundaries; }
            else ObserveAge(packet.event);
            consume(packet);
        }
    }

    template<class Consume, class Observe>
    void Drain(BoundedSpscQueue<QueuedAudioEvent, 1024> &simulation,
               BoundedSpscQueue<QueuedAudioEvent, 1024> &render, Consume &&consume, Observe &&observe)
    {
        DrainSimulation(simulation, consume, observe);
        QueuedAudioEvent packet;
        for (std::size_t i = 0; i < 1024 && render.TryPop(packet); ++i)
        {
            packet.dequeued_qpc = clock_ ? clock_() : 0;
            observe(packet, 1);
            // A rendered snapshot is published after its sim audio boundary. If
            // that boundary arrived after our first empty pop, consume it now.
            if (!initialized_ || packet.event.session_id != session_id_)
                DrainSimulation(simulation, consume, observe);
            if (!initialized_ || packet.event.session_id != session_id_)
            { ++stats_.discarded_old_session; continue; }
            ObserveAge(packet.event);
            consume(packet);
        }
    }
    const AudioDeliveryStats &Stats() const noexcept { return stats_; }
  private:
    void ObserveAge(const PresentationEvent &event) noexcept
    {
        const auto age = tick_ >= event.tick ? tick_ - event.tick : 0;
        stats_.event_age_ticks += age;
        stats_.maximum_event_age_ticks = std::max(stats_.maximum_event_age_ticks, age);
        ++stats_.event_age_samples; ++stats_.delivered;
    }
    std::array<QueuedAudioEvent, 1024> batch_{};
    std::uint64_t session_id_{};
    Tick tick_{};
    bool initialized_{};
    AudioDeliveryStats stats_{};
    DiagnosticClock clock_{};
};

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
    MainAudioSession(AudioEngine &audio, Sequence &sequence, DiagnosticClock clock = nullptr)
        : audio_(audio), sequence_(sequence), delivery_(clock)
    { RequestMusic(Event("audio.bgm.main_menu")); }

    template<class Observe>
    void Drain(BoundedSpscQueue<QueuedAudioEvent, 1024> &simulation,
               BoundedSpscQueue<QueuedAudioEvent, 1024> &render, Observe &&observe)
    {
        delivery_.Drain(simulation, render, [&](const QueuedAudioEvent &packet) {
            if (failure_ != MainAudioFailure::None) return;
            if (packet.kind == QueuedAudioEvent::Kind::Tick) ApplyTick(packet.tick_info);
            else Consume(packet.event, packet.dequeued_qpc);
        }, observe);
    }
    void Update()
    {
        if (failure_ != MainAudioFailure::None) return;
        if (audio_.FailedNativeOperation() != AudioNativeOperation::None)
        { Fail(MainAudioFailure::Native, {}, "native_update"); return; }
        // Both independent owner intents must retain their PCM before reclaim/eviction.
        const auto music_ready = pending_music_ ? audio_.RequestCue(pending_music_->asset) : AudioCueReadiness::Ready;
        const auto ambience_ready = pending_ambience_ ? audio_.RequestCue(pending_ambience_->asset) : AudioCueReadiness::Ready;
        audio_.UpdateListener(listener_, {0, 0, 1}, {0, 1, 0});
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
            const auto result = audio_.Play(*pending_music_, pending_music_dequeued_qpc_);
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
                pending_music_dequeued_qpc_ = 0;
            }
        }
        if (failure_ != MainAudioFailure::None) return;
        StartPreparedAmbience(ambience_ready);
    }
    MainAudioFailure Failure() const noexcept { return failure_; }
    const MainAudioFailureDetails &FailureDetails() const noexcept { return failure_details_; }
    const AudioDeliveryStats &Stats() const noexcept { return delivery_.Stats(); }
    bool HasPendingMusic() const noexcept { return pending_music_.has_value(); }
    bool HasPendingAmbience() const noexcept { return pending_ambience_.has_value(); }
    AssetId ActiveAmbience() const noexcept { return active_ambience_; }
    AssetId ActiveMusic() const noexcept { return active_music_; }
    SessionPhase Phase() const noexcept { return phase_; }
    void RequestMusic(PresentationEvent event, bool restart = false, std::uint64_t dequeued_qpc = 0)
    {
        if (pending_music_ && pending_music_->asset.value == event.asset.value)
        {
            if (!pending_music_dequeued_qpc_) pending_music_dequeued_qpc_ = dequeued_qpc;
            pending_music_restart_ = pending_music_restart_ || restart;
            return;
        }
        if (pending_music_)
        {
            audio_.Stop(pending_music_->asset); pending_music_.reset();
            pending_music_restart_ = false; pending_music_dequeued_qpc_ = 0;
        }
        if (!restart && active_music_.value == event.asset.value) return;
        pending_music_ = event;
        pending_music_restart_ = restart;
        pending_music_dequeued_qpc_ = dequeued_qpc;
        (void)audio_.RequestCue(event.asset);
    }
  private:
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
                pending_music_restart_ = false; pending_music_dequeued_qpc_ = 0;
            }
            CancelAmbience(); // Close old-session active/pending SFX ownership before its first new sound.
            audio_.BeginSession(facts.session_id);
            session_id_ = facts.session_id; initialized_ = true;
            // The new boundary precedes the first sound, never resets its clock afterwards.
            phase_known_ = false; phase_ = SessionPhase::MainMenu; active_ambience_ = {};
        }
        tick_ = facts.tick; listener_ = facts.listener_position;
        if (phase_known_ && phase_ == facts.phase) return;
        const auto old = phase_;
        phase_ = facts.phase; phase_known_ = true;
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
    void PlayChecked(const PresentationEvent &event, std::uint64_t dequeued_qpc = 0)
    { CheckPlay(audio_.Play(event, dequeued_qpc), event.asset, "event_play"); }
    void Consume(const PresentationEvent &event, std::uint64_t dequeued_qpc)
    {
        if (DecodeAudioAction(event.parameters) == AudioEventAction::Stop)
        {
            if (pending_music_ && pending_music_->asset.value == event.asset.value)
            { pending_music_.reset(); pending_music_restart_ = false; pending_music_dequeued_qpc_ = 0; }
            if (pending_ambience_ && pending_ambience_->asset.value == event.asset.value) pending_ambience_.reset();
            if (active_music_.value == event.asset.value) active_music_ = {};
            if (active_ambience_.value == event.asset.value) active_ambience_ = {};
            audio_.Stop(event.asset);
        }
        else if (audio_.IsTransitionTrack(event.asset)) RequestMusic(event, false, dequeued_qpc);
        else PlayChecked(event, dequeued_qpc);
    }
    AudioEngine &audio_;
    Sequence &sequence_;
    AudioDelivery delivery_;
    std::optional<PresentationEvent> pending_music_, pending_ambience_;
    std::uint64_t pending_music_dequeued_qpc_{};
    AssetId active_music_{}, active_ambience_{};
    bool pending_music_restart_{};
    Float3 listener_{};
    std::uint64_t session_id_{};
    Tick tick_{};
    SessionPhase phase_{SessionPhase::MainMenu};
    bool initialized_{}, phase_known_{};
    MainAudioFailure failure_{};
    MainAudioFailureDetails failure_details_{};
};
}
