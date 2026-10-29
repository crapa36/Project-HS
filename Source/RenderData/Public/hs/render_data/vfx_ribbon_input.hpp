#pragma once
#include <hs/render_data/vfx_frame_input.hpp>
#include <hs/render_data/vfx_program_values.hpp>
#include <cstddef>
#include <span>

namespace hs
{
struct VfxRibbonOutputInput
{
    VfxOutputProfile profile{VfxOutputProfile::RibbonAdd};
    VfxMotionKind motion{VfxMotionKind::HistoryRibbonTaperedFlow};
    Float4 color{};
    float width_head{}, width_tail{}, noise_warp{}, phase_offset{};
    float hdr{}, detail_strength{};
    std::uint32_t gradient_row{}, detail_slice{0xffffffffu};
    bool dashed{};
    bool travel_pulse{};
};
struct VfxRibbonSourceInput
{
    std::uint64_t owner_id{};
    VfxEffectHandle effect_handle{};
    std::uint32_t source_id{}, stable_seed{}, importance{};
    Float3 position{}, previous_position{};
    float history_seconds{}, start_alpha{1.0f};
    std::vector<VfxRibbonOutputInput> outputs;
    // Analytic mode: previous_position is the start, position is the end; control is quadratic Bezier control.
    bool analytic{};
    Float3 control{};
    std::uint32_t segments{16};
    float normalized_age{};
    std::uint32_t frame_output_first{}, frame_output_count{};

    [[nodiscard]] std::size_t OutputCount() const noexcept
    { return frame_output_count != 0 ? frame_output_count : outputs.size(); }

    [[nodiscard]] std::span<const VfxRibbonOutputInput> OutputSpan(
        std::span<const VfxRibbonOutputInput> frame_outputs = {}) const noexcept
    {
        if (frame_output_count == 0) return outputs;
        if (frame_output_first > frame_outputs.size() ||
            frame_output_count > frame_outputs.size() - frame_output_first) return {};
        return frame_outputs.subspan(frame_output_first, frame_output_count);
    }

    [[nodiscard]] bool OutputStorageConsistent(std::size_t frame_output_size) const noexcept
    {
        if (frame_output_count == 0) return frame_output_first == 0;
        if (!outputs.empty()) return false;
        return frame_output_first <= frame_output_size &&
            frame_output_count <= frame_output_size - frame_output_first;
    }
};
struct VfxRibbonFrameInput
{
    std::span<const VfxRibbonSourceInput> sources;
    std::uint64_t catalog_generation{};
    std::span<const VfxRibbonOutputInput> outputs;
};
}
