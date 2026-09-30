#pragma once
#include <hs/renderer/vfx_frame_input.hpp>
#include <hs/renderer/vfx_program.hpp>
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
};
struct VfxRibbonFrameInput
{
    std::span<const VfxRibbonSourceInput> sources;
    std::uint64_t catalog_generation{};
};
}
