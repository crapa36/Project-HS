#pragma once
#include "vfx_event_view.hpp"
#include "vfx_ribbon_frame_bounds.hpp"

#include <hs/runtime/vfx_image.hpp>
#include <hs/render_data/vfx_ribbon_input.hpp>

#include <cstddef>
#include <limits>
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
    VfxRibbonInputScratch(VfxRibbonInputScratch &&other) noexcept;
    VfxRibbonInputScratch &operator=(VfxRibbonInputScratch &&other) noexcept;

    void Reset() noexcept { active_count_ = 0; outputs_.clear(); }
    [[nodiscard]] std::span<const VfxRibbonSourceInput> Sources() const noexcept
    { return {rows_.data(), active_count_}; }
    [[nodiscard]] std::span<const VfxRibbonOutputInput> Outputs() const noexcept
    { return outputs_; }
    [[nodiscard]] std::size_t OutputCapacity() const noexcept { return outputs_.capacity(); }
    [[nodiscard]] std::span<const VfxRibbonSourceInput> Storage() const noexcept
    { return rows_; }
    [[nodiscard]] std::size_t StorageCapacity() const noexcept { return rows_.capacity(); }
    [[nodiscard]] std::size_t LogicalCapacityBytes() const noexcept
    {
        constexpr auto max = std::numeric_limits<std::size_t>::max();
        const auto row_capacity = rows_.capacity();
        const auto output_capacity = outputs_.capacity();
        if (row_capacity > max / sizeof(VfxRibbonSourceInput) ||
            output_capacity > max / sizeof(VfxRibbonOutputInput)) return max;
        const auto row_bytes = row_capacity * sizeof(VfxRibbonSourceInput);
        const auto output_bytes = output_capacity * sizeof(VfxRibbonOutputInput);
        return output_bytes > max - row_bytes ? max : row_bytes + output_bytes;
    }
    void Reserve(std::size_t sources, std::size_t outputs)
    { rows_.reserve(sources); outputs_.reserve(outputs); }
    [[nodiscard]] std::vector<VfxRibbonSourceInput> TakeSources() &&;

    [[nodiscard]] VfxRibbonSourceInput &PrepareAppendRow();
    void AppendOutput(const VfxRibbonOutputInput &output);
    void CommitAppendRow() noexcept { ++active_count_; }
    void RejectAppendRow() noexcept;
    void Rollback(std::size_t active_count) noexcept;

private:
    std::vector<VfxRibbonSourceInput> rows_;
    std::vector<VfxRibbonOutputInput> outputs_;
    std::size_t active_count_{};
};

void AppendVfxTypedRibbonInputs(
    const VfxImage &image,
    std::span<const VfxPersistentInput> persistent,
    VfxRibbonInputScratch &scratch);

void AppendVfxTypedRibbonInputs(
    const VfxImage &image,
    std::span<const VfxPersistentInput> persistent,
    std::vector<VfxRibbonSourceInput> &result);

[[nodiscard]] std::vector<VfxRibbonSourceInput> BuildVfxTypedRibbonInputs(
    const VfxImage &image,
    std::span<const VfxPersistentInput> persistent);

void AppendVfxEventRibbonInputs(
    const VfxImage &image,
    VfxEventView events,
    double current_tick,
    VfxRibbonInputScratch &scratch);

void AppendVfxEventRibbonInputs(
    const VfxImage &image,
    VfxEventView events,
    double current_tick,
    std::vector<VfxRibbonSourceInput> &result);

[[nodiscard]] std::vector<VfxRibbonSourceInput> BuildVfxEventRibbonInputs(
    const VfxImage &image,
    VfxEventView events,
    double current_tick);
}
