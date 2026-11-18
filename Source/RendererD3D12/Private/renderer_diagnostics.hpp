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

// Storage is fixed at construction; snapshots aggregate directly from the ring.
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
    void ObserveTransition(std::uint64_t local_usage_bytes) noexcept;
    [[nodiscard]] RendererDiagnosticsSnapshot Snapshot() const;
    [[nodiscard]] const RendererDiagnosticsFrame &LastRenderedFrame() const noexcept
    { return frames_[(next_frame_ + frames_.size() - 1) % frames_.size()]; }
  private:
    std::array<RendererDiagnosticsFrame, kRendererDiagnosticsFrameCapacity> frames_{};
    std::uint64_t rendered_main_sequence_{};
    std::size_t next_frame_{}, frame_count_{};
    std::uint64_t last_rendered_id_{};
    std::uint64_t rendered_{};
    std::optional<std::uint64_t> transition_peak_local_usage_bytes_;
};

} // namespace hs
