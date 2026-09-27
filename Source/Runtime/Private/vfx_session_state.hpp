#pragma once

#include <hs/core/presentation_event.hpp>
#include <hs/renderer/vfx_frame_input.hpp>
#include <hs/renderer/vfx_spawn_command.hpp>

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace hs::runtime_detail
{
// Render-thread retention. Events can arrive before their matching snapshot;
// a session ID keeps a reset simulation clock from replaying the previous run.
struct VfxSessionState
{
    struct PersistentBurstKey
    {
        std::uint64_t stable_id{};
        std::uint8_t visual_kind{};
        bool operator==(const PersistentBurstKey &) const = default;
    };
    struct PersistentBurstHash
    {
        std::size_t operator()(const PersistentBurstKey &key) const noexcept
        {
            const auto mixed = key.stable_id ^
                (static_cast<std::uint64_t>(key.visual_kind) << 56);
            return static_cast<std::size_t>(mixed ^ (mixed >> 33));
        }
    };
    std::uint64_t session_id{};
    std::vector<PresentationEvent> pending_events;
    std::vector<ParticleSpawnCommand> particle_spawns;
    std::vector<VfxLineSpawnCommand> effect_lines;
    std::vector<VfxFlashSpawnInput> flashes;
    std::vector<VfxEventInput> ground_events;
    std::vector<VfxEventInput> ribbon_events;
    std::vector<VfxEventInput> light_events;
    std::vector<VfxEventInput> distortion_events;
    std::vector<VfxEventInput> decal_events;
    std::unordered_set<PersistentBurstKey, PersistentBurstHash> seen_persistent_bursts;

    bool MarkPersistentBurst(std::uint64_t stable_id, std::uint8_t visual_kind)
    {
        return stable_id != 0 &&
            seen_persistent_bursts.insert({stable_id, visual_kind}).second;
    }

    void ClearDecoded()
    {
        particle_spawns.clear();
        effect_lines.clear();
        flashes.clear();
        ground_events.clear();
        ribbon_events.clear();
        light_events.clear();
        distortion_events.clear();
        decal_events.clear();
    }

    bool Synchronize(std::uint64_t current_session)
    {
        if (session_id == current_session) return false;
        session_id = current_session;
        ClearDecoded();
        seen_persistent_bursts.clear();
        std::erase_if(pending_events, [&](const PresentationEvent &event) {
            return event.session_id < session_id;
        });
        return true;
    }

    std::vector<PresentationEvent> TakeReady(Tick tick)
    {
        std::vector<PresentationEvent> ready;
        std::erase_if(pending_events, [&](const PresentationEvent &event) {
            if (event.session_id < session_id) return true;
            if (event.session_id != session_id || event.tick > tick) return false;
            ready.push_back(event);
            return true;
        });
        return ready;
    }
};
} // namespace hs::runtime_detail
