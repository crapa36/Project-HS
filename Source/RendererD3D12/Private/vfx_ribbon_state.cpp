#include "vfx_ribbon_state.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <tuple>

namespace hs::renderer_detail
{
namespace
{
bool Finite(const Float3 &v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool Finite(const Float4 &v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w); }
using Key = std::tuple<std::uint64_t, VfxEffectHandle, std::uint32_t>;
Key SourceKey(const VfxRibbonSourceInput &source) { return {source.owner_id, source.effect_handle, source.source_id}; }
std::uint32_t MotionCode(VfxMotionKind motion);
bool ValidateRibbonSources(std::span<const VfxRibbonSourceInput> sources, std::string &error,
                           std::vector<std::size_t> &source_order)
{
    source_order.clear();
    const auto reject = [&](const char *message) {
        error = message;
        source_order.clear();
        return false;
    };
    for (const auto &source : sources)
    {
        if (source.effect_handle == kInvalidVfxEffectHandle || !Finite(source.position) || !Finite(source.previous_position) ||
            !std::isfinite(source.history_seconds) || source.history_seconds < 0.0f ||
            (!source.analytic && (source.history_seconds <= 0.0f || source.history_seconds > 62.0f / 60.0f)) ||
            (source.analytic && (!Finite(source.control) || source.segments < 1 || source.segments >= kRibbonHistorySamples ||
                !std::isfinite(source.normalized_age) || source.normalized_age < 0 || source.normalized_age > 1)) ||
            !std::isfinite(source.start_alpha) || source.start_alpha < 0.0f || source.start_alpha > 1.0f || source.outputs.empty())
            return reject("Ribbon source has invalid identity, transform, history, alpha, or outputs");
        for (const auto &output : source.outputs)
        {
            const auto motion = MotionCode(output.motion);
            if ((output.profile != VfxOutputProfile::RibbonAdd && output.profile != VfxOutputProfile::RibbonOit) ||
                motion == std::numeric_limits<std::uint32_t>::max() ||
                (source.analytic != (motion == 4 || motion == 5)) ||
                (!source.analytic && output.travel_pulse) || !Finite(output.color) ||
                !std::isfinite(output.width_head) || output.width_head < 0.0f || !std::isfinite(output.width_tail) || output.width_tail < 0.0f || !std::isfinite(output.noise_warp) || output.noise_warp < 0.0f ||
                !std::isfinite(output.phase_offset) || !std::isfinite(output.hdr) || output.hdr < 0.0f ||
                !std::isfinite(output.detail_strength) || output.detail_strength < 0.0f || output.detail_strength > 1.0f ||
                output.gradient_row >= 68u ||
                (output.motion == VfxMotionKind::HistoryRibbonTearsBackward &&
                 (!std::isfinite(source.control.x) || source.control.x <= 0.0f)) ||
                (output.detail_slice != std::numeric_limits<std::uint32_t>::max() && output.detail_slice >= 8u))
                return reject("Ribbon output has an unsupported profile, motion, gradient, or non-finite material");
        }
    }

    if (sources.size() > 1)
    {
        source_order.resize(sources.size());
        for (std::size_t index = 0; index < source_order.size(); ++index) source_order[index] = index;
        std::sort(source_order.begin(), source_order.end(), [&](std::size_t left, std::size_t right) {
            return SourceKey(sources[left]) < SourceKey(sources[right]);
        });
        for (std::size_t index = 1; index < source_order.size(); ++index)
            if (SourceKey(sources[source_order[index - 1]]) == SourceKey(sources[source_order[index]]))
                return reject("Ribbon frame contains a duplicate owner, effect, and source key");
    }

    source_order.clear();
    error.clear();
    return true;
}
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

std::size_t VfxRibbonState::KeyHash::operator()(const Key &key) const noexcept
{
    auto hash = std::hash<std::uint64_t>{}(std::get<0>(key));
    hash ^= std::hash<VfxEffectHandle>{}(std::get<1>(key)) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
    hash ^= std::hash<std::uint32_t>{}(std::get<2>(key)) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
    return hash;
}

bool VfxRibbonState::OutputsMayExceed(std::span<const VfxRibbonSourceInput> sources,
                                     std::size_t output_capacity)
{
    constexpr auto word_bits = std::numeric_limits<std::size_t>::digits;
    source_scratch_.assign(slots_.size() / word_bits + (slots_.size() % word_bits != 0), 0);
    const auto finish = [&](bool exceeds) {
        source_scratch_.clear();
        return exceeds;
    };
    for (const auto &source : sources)
    {
        if (source.outputs.size() > output_capacity) return finish(true);
        output_capacity -= source.outputs.size();
        if (const auto found = slot_by_key_.find(SourceKey(source)); found != slot_by_key_.end())
            source_scratch_[found->second / word_bits] |= std::size_t{1} << (found->second % word_bits);
    }
    for (std::size_t i = 0; i < slots_.size(); ++i)
        if (slots_[i].occupied && !(source_scratch_[i / word_bits] & (std::size_t{1} << (i % word_bits))))
        {
            if (slots_[i].outputs.size() > output_capacity) return finish(true);
            output_capacity -= slots_[i].outputs.size();
        }
    return finish(false);
}
bool VfxRibbonState::Update(std::span<const VfxRibbonSourceInput> sources,
                            Tick tick, std::uint64_t session,
                            std::uint64_t catalog_generation,
                            std::uint32_t point_budget, float interpolation,
                            Float3 previous_eye, std::string &error)
{
    if (!ValidateFrameSources(sources, error)) return false;
    return UpdateValidated(sources, tick, session, catalog_generation, point_budget, interpolation, previous_eye, error);
}

bool VfxRibbonState::ValidateSources(std::span<const VfxRibbonSourceInput> sources, std::string &error)
{
    std::vector<std::size_t> source_order;
    return ValidateRibbonSources(sources, error, source_order);
}

bool VfxRibbonState::ValidateFrameSources(std::span<const VfxRibbonSourceInput> sources,
                                         std::string &error)
{
    return ValidateRibbonSources(sources, error, source_scratch_);
}

bool VfxRibbonState::UpdateValidated(std::span<const VfxRibbonSourceInput> sources,
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
        slot_by_key_.clear();
        updates_.clear();
        outputs_.clear();
        capacity_ = capacity;
        session_ = session;
        catalog_generation_ = catalog_generation;
        initialized_ = true;
    }

    source_scratch_.resize(sources.size());
    auto &order = source_scratch_;
    for (std::size_t i = 0; i < sources.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (sources[a].importance != sources[b].importance) return sources[a].importance > sources[b].importance;
        return SourceKey(sources[a]) < SourceKey(sources[b]);
    });
    const auto winner_count = std::min<std::size_t>(capacity_, order.size());
    dropped_sources_ = static_cast<std::uint32_t>(order.size() - winner_count);
    for (auto &slot : slots_)
    {
        slot.winner = slot.seen = false;
        if (slot.occupied && !slot.analytic && tick >= slot.last_seen + static_cast<Tick>(std::ceil(slot.history_seconds * 60.0f)))
        {
            slot_by_key_.erase(Key{slot.owner_id, slot.effect_handle, slot.source_id});
            slot.occupied = false;
        }
    }
    for (std::size_t i = 0; i < winner_count; ++i)
        if (const auto found = slot_by_key_.find(SourceKey(sources[order[i]])); found != slot_by_key_.end())
            slots_[found->second].winner = true;
    std::size_t next_reusable{};
    updates_.clear();
    outputs_.clear();
    for (std::size_t winner = 0; winner < winner_count; ++winner)
    {
        const auto index = order[winner];
        const auto &source = sources[index];
        const auto key = SourceKey(source);
        const auto found = slot_by_key_.find(key);
        auto slot_index = found != slot_by_key_.end() ? found->second : slots_.size();
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
                while (next_reusable < slots_.size() && slots_[next_reusable].occupied && slots_[next_reusable].winner)
                    ++next_reusable;
                if (next_reusable == slots_.size()) continue;
                slot_index = next_reusable++;
            }
            auto &slot = slots_[slot_index];
            if (slot.occupied) slot_by_key_.erase(Key{slot.owner_id, slot.effect_handle, slot.source_id});
            slot_by_key_.emplace(key, slot_index);
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
        slot.winner = slot.seen = true;
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
        if (!slot.occupied || slot.seen) continue;
        if (slot.analytic)
        {
            GpuRibbonSourceUpdate clear{};
            clear.identity = {static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(tick), slot.generation, 4u};
            updates_.push_back(clear);
            slot_by_key_.erase(Key{slot.owner_id, slot.effect_handle, slot.source_id});
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
    auto &grouped = grouped_outputs_;
    grouped.clear();
    grouped.reserve(outputs_.size());
    for (const auto profile : {0u, 1u})
        for (const auto &output : outputs_)
            if ((output.metadata[3] & 1u) == profile) grouped.push_back(output);
    outputs_.swap(grouped);
    // Empty scratch stays out of the conditional state checkpoint's deep copy.
    grouped.clear();
    order.clear();
    last_tick_ = tick;
    error.clear();
    return true;
}
}
