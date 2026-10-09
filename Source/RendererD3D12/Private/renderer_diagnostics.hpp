#pragma once

#include <hs/core/result.hpp>
#include <hs/renderer/renderer.hpp>

#include <Windows.h>

#include <string_view>

namespace hs
{

[[nodiscard]] Result HResultFailure(std::string_view operation, HRESULT result);

[[nodiscard]] std::uint64_t RendererDiagnosticsCounter() noexcept;

struct CompletedRenderCadence
{
    std::uint64_t newest_frame_id{};
    std::uint32_t intervals{};
    double fps{};
};

// Storage is fixed at construction. Only an explicit snapshot creates bounded
// output vectors; recording never retains strings/vectors or appends a log.
class RendererDiagnosticsRecorder
{
  public:
    void RecordRenderedFrame(std::uint64_t frame_id, std::uint64_t submit_qpc,
        std::uint64_t present_begin_qpc, std::uint64_t present_end_qpc,
        const RendererQueueStats &, const RendererDiagnosticsMemory &,
        bool validation, bool gpu_validation, bool presented = true,
        std::uint32_t view_id = 0) noexcept;
    void RecordCompleted(const GpuPassTimings &) noexcept;
    [[nodiscard]] bool HasCompleted(std::uint64_t frame_id, std::uint32_t view_id) const noexcept;
    [[nodiscard]] CompletedRenderCadence RecentCompletedMainCadence(
        std::uint32_t maximum_intervals) const noexcept;
    // Observe while old and candidate resources coexist. This reports actual
    // observed transaction boundaries, never an interpolated or reserved peak.
    void ObserveTransition(const RendererDiagnosticsMemory &) noexcept;
    [[nodiscard]] RendererDiagnosticsSnapshot Snapshot() const;
  private:
    struct Pass
    {
        std::uint32_t pass_id{};
        std::array<char, 96> name{};
        RenderQueue queue{};
        std::uint64_t nanoseconds{}, begin_ticks{}, end_ticks{}, timestamp_frequency{};
        std::uint64_t begin_qpc{}, end_qpc{}, qpc_frequency{};
        bool measured{}, clock_calibrated{}, name_truncated{};
    };
    std::array<RendererDiagnosticsFrame, kRendererDiagnosticsFrameCapacity> frames_{};
    struct MainCadenceSample
    {
        std::uint64_t sequence{}, frame_id{}, submit_qpc{}, qpc_frequency{};
        bool presented{}, validation{}, gpu_validation{};
    };
    std::array<MainCadenceSample, kRendererDiagnosticsFrameCapacity> main_cadence_{};
    std::uint64_t latest_main_cadence_sequence_{};
    std::uint64_t rendered_main_sequence_{};
    std::array<Pass, kRendererDiagnosticsPassCapacity> passes_{};
    std::size_t next_frame_{}, frame_count_{}, pass_count_{};
    std::uint64_t last_rendered_id_{}, timeline_frame_id_{};
    std::uint32_t timeline_view_id_{};
    std::uint64_t rendered_{}, completed_{}, duplicates_{}, rejected_{};
    bool timeline_truncated_{}, transition_observed_{};
    RendererDiagnosticsMemory sampled_peak_{}, transition_peak_{};
};

} // namespace hs
