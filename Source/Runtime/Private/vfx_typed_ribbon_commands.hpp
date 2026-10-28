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
    VfxRibbonInputScratch(VfxRibbonInputScratch &&other) noexcept;
    VfxRibbonInputScratch &operator=(VfxRibbonInputScratch &&other) noexcept;

    void Reset() noexcept { active_count_ = 0; }
    [[nodiscard]] std::span<const VfxRibbonSourceInput> Sources() const noexcept
    { return {rows_.data(), active_count_}; }
    [[nodiscard]] std::span<const VfxRibbonSourceInput> Storage() const noexcept
    { return rows_; }
    [[nodiscard]] std::size_t StorageCapacity() const noexcept { return rows_.capacity(); }
    [[nodiscard]] std::vector<VfxRibbonSourceInput> TakeSources() &&;

    [[nodiscard]] VfxRibbonSourceInput &PrepareAppendRow();
    void CommitAppendRow() noexcept { ++active_count_; }
    void RejectAppendRow() noexcept;
    void Rollback(std::size_t active_count) noexcept;

private:
    std::vector<VfxRibbonSourceInput> rows_;
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
