#pragma once

#include <hs/core/cooked_format.hpp>
#include <hs/game_domain/game_types.hpp>

namespace hs
{

[[nodiscard]] inline AssetId ResolveSessionMusic(SessionPhase phase, AssetId current) noexcept
{
    switch (phase)
    {
    case SessionPhase::MainMenu: return MakeAssetId("audio.bgm.main_menu");
    case SessionPhase::Victory: return MakeAssetId("audio.stinger.victory");
    case SessionPhase::Defeat: return MakeAssetId("audio.stinger.defeat");
    case SessionPhase::Playing:
        if (current.value == 0 || current.value == MakeAssetId("audio.bgm.main_menu").value)
            return MakeAssetId("audio.bgm.session");
        [[fallthrough]];
    default:
        // Pause, character pages, and reward selection retain the session's track.
        return current;
    }
}

} // namespace hs
