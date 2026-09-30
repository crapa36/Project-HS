#pragma once

#include "vfx_ribbon_gpu.hpp"
#include <hs/renderer/vfx_ribbon_input.hpp>
#include <span>
#include <string>
#include <vector>

namespace hs::renderer_detail
{
class VfxRibbonState
{
public:
    [[nodiscard]] bool Update(std::span<const VfxRibbonSourceInput> sources,
                               Tick tick, std::uint64_t session,
                               std::uint64_t catalog_generation,
                               std::uint32_t point_budget, float interpolation,
                               Float3 previous_eye, std::string &error);

    [[nodiscard]] std::span<const GpuRibbonSourceUpdate> Updates() const noexcept { return updates_; }
    [[nodiscard]] std::span<const GpuRibbonOutput> Outputs() const noexcept { return outputs_; }
    [[nodiscard]] std::uint32_t DroppedSources() const noexcept { return dropped_sources_; }
    [[nodiscard]] std::uint32_t Capacity() const noexcept { return capacity_; }

private:
    struct Slot
    {
        std::uint64_t owner_id{};
        VfxEffectHandle effect_handle{};
        std::uint32_t source_id{};
        std::uint32_t generation{};
        Tick last_seen{};
        float history_seconds{};
        Float3 previous_head{}, current_head{};
        std::vector<GpuRibbonOutput> outputs;
        bool occupied{};
        bool analytic{};
    };

    std::vector<Slot> slots_;
    std::vector<GpuRibbonSourceUpdate> updates_;
    std::vector<GpuRibbonOutput> outputs_;
    std::uint64_t session_{};
    std::uint64_t catalog_generation_{};
    Tick last_tick_{};
    std::uint32_t capacity_{};
    std::uint32_t dropped_sources_{};
    std::uint32_t next_generation_{1};
    bool initialized_{};
};
}
