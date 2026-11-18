#pragma once
#include "vfx_event_view.hpp"

#include <hs/runtime/vfx_image.hpp>
#include <hs/render_data/vfx_ribbon_input.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace hs::runtime_detail
{
class VfxRibbonInputScratch
{
public:
    VfxRibbonInputScratch() = default;
    VfxRibbonInputScratch(const VfxRibbonInputScratch &) = delete;
    VfxRibbonInputScratch &operator=(const VfxRibbonInputScratch &) = delete;

    void Reset() noexcept { active_count_ = 0; outputs_.clear(); }
    [[nodiscard]] std::span<const VfxRibbonSourceInput> Sources() const noexcept
    { return {rows_.data(), active_count_}; }
    [[nodiscard]] std::span<const VfxRibbonOutputInput> Outputs() const noexcept
    { return outputs_; }
    void Reserve(std::size_t sources, std::size_t outputs)
    { rows_.reserve(sources); outputs_.reserve(outputs); }

    [[nodiscard]] VfxRibbonSourceInput &PrepareAppendRow();
    void AppendOutput(const VfxRibbonOutputInput &output);
    void CommitAppendRow() noexcept { ++active_count_; }
    void RejectAppendRow() noexcept;

private:
    std::vector<VfxRibbonSourceInput> rows_;
    std::vector<VfxRibbonOutputInput> outputs_;
    std::size_t active_count_{};
};

// Builders require an immutable prepared and admitted image; live events and owners remain mutable.
void AppendVfxTypedRibbonInputs(
    const VfxImage &image,
    std::span<const VfxPersistentInput> persistent,
    VfxRibbonInputScratch &scratch);



void AppendVfxEventRibbonInputs(
    const VfxImage &image,
    VfxEventView events,
    double current_tick,
    VfxRibbonInputScratch &scratch);


}
