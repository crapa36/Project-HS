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
    GameplayChecksum expected_checksum{};

    [[nodiscard]] InputFrame View() const noexcept
    {
        return {target_tick, held, ordered_edges};
    }
};

struct PlaytestReplay
{
    std::uint64_t seed{};
    std::uint64_t content_hash{};
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
                                const SessionProbe &probe,
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
