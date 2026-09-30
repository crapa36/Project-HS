#include "vfx_ribbon_state.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>

namespace hs::renderer_detail
{
namespace
{
bool Finite(const Float3 &v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool Finite(const Float4 &v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w); }
using Key = std::tuple<std::uint64_t, VfxEffectHandle, std::uint32_t>;
Key SourceKey(const VfxRibbonSourceInput &source) { return {source.owner_id, source.effect_handle, source.source_id}; }
std::uint32_t MotionCode(VfxMotionKind motion)
{
    switch (motion)
    {
    case VfxMotionKind::HistoryRibbonTaperedFlow: return 0;
    case VfxMotionKind::ShortTurbulentWake: return 1;
    case VfxMotionKind::CurvedRecoilArc: return 3; // emitter trajectory is evaluated from the event clock
    case VfxMotionKind::TwoPhaseNarrowWakes: return 2;
    case VfxMotionKind::CurvedLinkTravelingHead: return 4;
    case VfxMotionKind::CurvesTowardCollector: return 4;
    case VfxMotionKind::SoftWiderSheath: return 5;
    case VfxMotionKind::ShortLineIncomingVelocity: return 6;
    case VfxMotionKind::HistoryRibbonTearsBackward: return 7;
    default: return std::numeric_limits<std::uint32_t>::max();
    }
}
}

bool VfxRibbonState::Update(std::span<const VfxRibbonSourceInput> sources,
                            Tick tick, std::uint64_t session,
                            std::uint64_t catalog_generation,
                            std::uint32_t point_budget, float interpolation,
                            Float3 previous_eye, std::string &error)
{
    if (point_budget < kRibbonHistorySamples)
    {
        error = "Ribbon point budget must provide at least one 64-sample slot";
        return false;
    }
    if (!std::isfinite(interpolation) || interpolation < 0.0f || interpolation > 1.0f || !Finite(previous_eye))
    {
        error = "Ribbon interpolation and previous eye must be finite";
        return false;
    }

    const auto capacity = point_budget / kRibbonHistorySamples;
    if (!initialized_ || session != session_ || catalog_generation != catalog_generation_ || capacity != capacity_ || tick < last_tick_)
    {
        slots_.clear();
        updates_.clear();
        outputs_.clear();
        capacity_ = capacity;
        session_ = session;
        catalog_generation_ = catalog_generation;
        initialized_ = true;
    }

    std::vector<std::size_t> order(sources.size());
    for (std::size_t i = 0; i < sources.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (sources[a].importance != sources[b].importance) return sources[a].importance > sources[b].importance;
        return SourceKey(sources[a]) < SourceKey(sources[b]);
    });
    for (std::size_t i = 0; i < sources.size(); ++i)
    {
        const auto &source = sources[i];
        if (source.effect_handle == kInvalidVfxEffectHandle || !Finite(source.position) || !Finite(source.previous_position) ||
            !std::isfinite(source.history_seconds) || source.history_seconds < 0.0f ||
            (!source.analytic && (source.history_seconds <= 0.0f || source.history_seconds > 62.0f / 60.0f)) ||
            (source.analytic && (!Finite(source.control) || source.segments < 1 || source.segments >= kRibbonHistorySamples ||
                !std::isfinite(source.normalized_age) || source.normalized_age < 0 || source.normalized_age > 1)) ||
            !std::isfinite(source.start_alpha) || source.start_alpha < 0.0f || source.start_alpha > 1.0f || source.outputs.empty())
        {
            error = "Ribbon source has invalid identity, transform, history, alpha, or outputs";
            return false;
        }
        for (const auto &output : source.outputs)
        {
            if ((output.profile != VfxOutputProfile::RibbonAdd && output.profile != VfxOutputProfile::RibbonOit) ||
                MotionCode(output.motion) == std::numeric_limits<std::uint32_t>::max() ||
                (source.analytic != (MotionCode(output.motion) == 4 || MotionCode(output.motion) == 5)) ||
                (!source.analytic && output.travel_pulse) || !Finite(output.color) ||
                !std::isfinite(output.width_head) || output.width_head < 0.0f || !std::isfinite(output.width_tail) || output.width_tail < 0.0f || !std::isfinite(output.noise_warp) || output.noise_warp < 0.0f ||
                !std::isfinite(output.phase_offset) || !std::isfinite(output.hdr) || output.hdr < 0.0f ||
                !std::isfinite(output.detail_strength) || output.detail_strength < 0.0f || output.detail_strength > 1.0f ||
                output.gradient_row >= 68u ||
                (output.motion == VfxMotionKind::HistoryRibbonTearsBackward &&
                 (!std::isfinite(source.control.x) || source.control.x <= 0.0f)) ||
                (output.detail_slice != std::numeric_limits<std::uint32_t>::max() && output.detail_slice >= 8u))
            {
                error = "Ribbon output has an unsupported profile, motion, gradient, or non-finite material";
                return false;
            }
        }
    }
    std::set<Key> input_keys;
    for (const auto &source : sources)
        if (!input_keys.insert(SourceKey(source)).second)
        {
            error = "Ribbon frame contains a duplicate owner, effect, and source key";
            return false;
        }

    const auto winner_count = std::min<std::size_t>(capacity_, order.size());
    dropped_sources_ = static_cast<std::uint32_t>(order.size() - winner_count);
    std::vector<bool> seen(slots_.size(), false);
    auto find_slot = [&](const auto &source) -> std::size_t {
        const auto key = SourceKey(source);
        for (std::size_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].occupied && Key{slots_[i].owner_id, slots_[i].effect_handle, slots_[i].source_id} == key) return i;
        return slots_.size();
    };
    const auto winner_owns_slot = [&](const Slot &slot) {
        const Key key{slot.owner_id, slot.effect_handle, slot.source_id};
        for (std::size_t i = 0; i < winner_count; ++i)
            if (SourceKey(sources[order[i]]) == key) return true;
        return false;
    };
    auto reclaim_retired = [&]() {
        for (auto &slot : slots_)
            if (slot.occupied && !slot.analytic && tick >= slot.last_seen + static_cast<Tick>(std::ceil(slot.history_seconds * 60.0f))) slot.occupied = false;
    };
    reclaim_retired();

    updates_.clear();
    outputs_.clear();
    for (std::size_t winner = 0; winner < winner_count; ++winner)
    {
        const auto index = order[winner];
        const auto &source = sources[index];
        auto slot_index = find_slot(source);
        bool reset = false;
        if (slot_index == slots_.size())
        {
            if (slots_.size() < capacity_)
            {
                slots_.push_back({});
                slot_index = slots_.size() - 1;
            }
            else
            {
                auto retired = std::find_if(slots_.begin(), slots_.end(), [&](const Slot &slot) {
                    return !slot.occupied || !winner_owns_slot(slot);
                });
                if (retired == slots_.end()) continue;
                slot_index = static_cast<std::size_t>(std::distance(slots_.begin(), retired));
            }
            auto &slot = slots_[slot_index];
            slot.owner_id = source.owner_id;
            slot.effect_handle = source.effect_handle;
            slot.source_id = source.source_id;
            slot.generation = next_generation_++;
            slot.outputs.clear();
            slot.occupied = true;
            reset = true;
        }
        auto &slot = slots_[slot_index];
        if (slot.analytic != source.analytic) { reset = true; slot.generation = next_generation_++; }
        slot.analytic = source.analytic;
        if (reset)
            slot.previous_head = slot.current_head = source.position;
        else if (tick != slot.last_seen)
        {
            slot.previous_head = slot.current_head;
            slot.current_head = source.position;
        }
        else
            slot.current_head = source.position;
        slot.last_seen = tick;
        slot.history_seconds = source.history_seconds;
        seen.resize(slots_.size(), false);
        seen[slot_index] = true;
        GpuRibbonSourceUpdate update{};
        update.position_duration = {source.position.x, source.position.y, source.position.z, source.history_seconds};
        update.previous_interpolation = {source.previous_position.x, source.previous_position.y, source.previous_position.z, interpolation};
        update.identity = {static_cast<std::uint32_t>(slot_index), static_cast<std::uint32_t>(tick), slot.generation, 1u | (reset ? 2u : 0u) | (source.analytic ? 4u | (source.segments << 8) : 0u)};
        update.rendered_head = {
            source.previous_position.x + (source.position.x - source.previous_position.x) * interpolation,
            source.previous_position.y + (source.position.y - source.previous_position.y) * interpolation,
            source.previous_position.z + (source.position.z - source.previous_position.z) * interpolation, 1.0f};
        if (!source.outputs.empty() &&
            source.outputs[0].motion == VfxMotionKind::HistoryRibbonTearsBackward)
            update.rendered_head = {
                slot.previous_head.x + (slot.current_head.x - slot.previous_head.x) * interpolation,
                slot.previous_head.y + (slot.current_head.y - slot.previous_head.y) * interpolation,
                slot.previous_head.z + (slot.current_head.z - slot.previous_head.z) * interpolation, 1.0f};
        update.control_age = {source.control.x, source.control.y, source.control.z, source.normalized_age};
        if (source.analytic) update.rendered_head = {source.position.x, source.position.y, source.position.z, 1};
        updates_.push_back(update);
        slot.outputs.clear();
        for (const auto &output : source.outputs)
        {
            GpuRibbonOutput gpu{};
            gpu.metadata = {static_cast<std::uint32_t>(slot_index), output.gradient_row, MotionCode(output.motion),
                            (output.profile == VfxOutputProfile::RibbonOit ? 1u : 0u) | (output.dashed ? 2u : 0u)};
            gpu.widths = {output.width_head, output.width_tail, output.noise_warp, output.phase_offset};
            gpu.color = output.color;
            gpu.material = {output.hdr, source.history_seconds, output.detail_strength, 4.0f};
            gpu.previous_eye = {previous_eye.x, previous_eye.y, previous_eye.z,
                                std::bit_cast<float>(static_cast<std::uint32_t>(tick))};
            gpu.detail = {output.detail_slice == std::numeric_limits<std::uint32_t>::max() ? -1.0f : static_cast<float>(output.detail_slice), 1.0f, 0.0f, source.start_alpha};
            gpu.analytic = {source.normalized_age, output.travel_pulse ? 1.0f : 0.0f,
                            source.analytic ? 1.0f : 0.0f,
                            output.motion == VfxMotionKind::HistoryRibbonTearsBackward
                                ? source.control.x : 0.0f};
            outputs_.push_back(gpu);
            slot.outputs.push_back(gpu);
        }
    }

    for (std::size_t i = 0; i < slots_.size(); ++i)
    {
        auto &slot = slots_[i];
        if (!slot.occupied || (i < seen.size() && seen[i])) continue;
        if (slot.analytic)
        {
            GpuRibbonSourceUpdate clear{};
            clear.identity = {static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(tick), slot.generation, 4u};
            updates_.push_back(clear);
            slot.occupied = false;
            slot.outputs.clear();
            continue;
        }
        const auto age = tick >= slot.last_seen ? tick - slot.last_seen : 0;
        if (age >= static_cast<Tick>(std::ceil(slot.history_seconds * 60.0f))) continue;
        GpuRibbonSourceUpdate tail{};
        tail.previous_interpolation = {previous_eye.x, previous_eye.y, previous_eye.z, interpolation};
        tail.position_duration.w = slot.history_seconds;
        tail.identity = {static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(tick), slot.generation, 0u};
        updates_.push_back(tail);
        for (auto output : slot.outputs)
        {
            output.metadata[0] = static_cast<std::uint32_t>(i);
            output.previous_eye = {previous_eye.x, previous_eye.y, previous_eye.z,
                                   std::bit_cast<float>(static_cast<std::uint32_t>(tick))};
            outputs_.push_back(output);
        }
    }
    std::vector<GpuRibbonOutput> grouped;
    grouped.reserve(outputs_.size());
    for (const auto profile : {0u, 1u})
        for (const auto &output : outputs_)
            if ((output.metadata[3] & 1u) == profile) grouped.push_back(output);
    outputs_.swap(grouped);
    last_tick_ = tick;
    error.clear();
    return true;
}
}
