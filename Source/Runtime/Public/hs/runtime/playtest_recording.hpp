#pragma once

#include <hs/core/input.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/core/result.hpp>
#include <hs/game_domain/game_types.hpp>

#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace hs
{

struct ReplayHeader
{
    std::uint32_t format_version{3};
    std::uint32_t simulation_version{kSimulationVersion};
    std::uint32_t gameplay_hash_version{kGameplayHashVersion};
    std::uint64_t simulation_rules_hash{};
    std::uint32_t tick_rate{60};
    std::uint32_t determinism_profile{kDeterminismProfile};
    std::uint64_t seed{};
};

struct PlaytestRecorderConfig
{
    std::filesystem::path output_directory;
    std::uint64_t seed{};
    std::uint64_t content_hash{};
};

struct PlaytestReplayFrame
{
    Tick target_tick{};
    HeldInputState held{};
    std::vector<ActionEdge> ordered_edges;
    std::vector<UiAction> ui_actions;
    GameplayChecksum expected_checksum{};

    [[nodiscard]] InputFrame View() const noexcept
    {
        return {target_tick, held, ordered_edges, ui_actions};
    }
};

struct PlaytestReplay
{
    ReplayHeader header{};
    std::vector<PlaytestReplayFrame> frames;
};

class PlaytestRecorder
{
  public:
    PlaytestRecorder();
    ~PlaytestRecorder();
    PlaytestRecorder(const PlaytestRecorder &) = delete;
    PlaytestRecorder &operator=(const PlaytestRecorder &) = delete;

    [[nodiscard]] Result Start(const PlaytestRecorderConfig &config);
    [[nodiscard]] Result Record(const InputFrame &input, GameplayChecksum checksum,
                                const SimulationObservation &probe,
                                std::span<const PresentationEvent> events);
    [[nodiscard]] Result Finish(bool execution_valid);
    [[nodiscard]] const std::filesystem::path &Directory() const noexcept;
    [[nodiscard]] bool Active() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] Result LoadPlaytestReplay(const std::filesystem::path &directory,
                                        PlaytestReplay &replay);

} // namespace hs
