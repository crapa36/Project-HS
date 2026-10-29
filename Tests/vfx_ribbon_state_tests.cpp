#include "vfx_ribbon_state.hpp"
#include "vfx_ribbon_resources.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
using hs::VfxMotionKind;
using hs::VfxOutputProfile;
using hs::VfxRibbonOutputInput;
using hs::VfxRibbonSourceInput;
using hs::renderer_detail::VfxRibbonState;
static_assert(std::is_nothrow_move_assignable_v<VfxRibbonState>);

void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class T>
bool SameBytes(std::span<const T> lhs, std::span<const T> rhs)
{
    static_assert(std::is_trivially_copyable_v<T>);
    return lhs.size() == rhs.size() &&
        (lhs.empty() || std::memcmp(lhs.data(), rhs.data(), lhs.size_bytes()) == 0);
}

VfxRibbonSourceInput Source(std::uint64_t owner, std::uint32_t importance = 1)
{
    VfxRibbonSourceInput source{};
    source.owner_id = owner;
    source.effect_handle = 1;
    source.source_id = 2;
    source.importance = importance;
    source.position = {2.0f, 0.0f, 0.0f};
    source.previous_position = {0.0f, 0.0f, 0.0f};
    source.history_seconds = 0.2f;
    source.outputs.push_back({VfxOutputProfile::RibbonAdd, VfxMotionKind::HistoryRibbonTaperedFlow,
                              {1.0f, 0.5f, 0.2f, 1.0f}, 0.2f, 0.05f, 0.01f, 0.0f, 2.0f, 0.5f, 3, 0, false});
    return source;
}

struct TestRibbonBuffer
{
    std::uint64_t width{};
    std::shared_ptr<int> allocation;
};

hs::Result TryRibbonBufferUpdate(VfxRibbonState &state, std::array<TestRibbonBuffer, 3> &buffers,
    std::span<const VfxRibbonSourceInput> sources, std::uint32_t points, std::uint64_t generation,
    unsigned fail_allocation, bool &checkpoint_copied)
{
    using namespace hs::renderer_detail;
    const std::array<std::uint64_t, 3> widths{buffers[0].width, buffers[1].width, buffers[2].width};
    const auto expected_history = std::uint64_t(points / kRibbonHistorySamples) * kRibbonHistoryStride;
    checkpoint_copied = widths[0] != expected_history || widths[1] != expected_history || widths[2] == 0 ||
        state.OutputsMayExceed(sources, widths[2] / kRibbonArgumentStride);
    std::optional<VfxRibbonState> checkpoint;
    if (checkpoint_copied) checkpoint = state;
    const auto reject = [&](hs::Result failure) {
        if (checkpoint) state = std::move(*checkpoint);
        return failure;
    };
    std::string error;
    if (!state.Update(sources, 11, 1, generation, points, .5f, {}, error))
        return reject(hs::Result::Failure(hs::ErrorCode::InvalidArgument, "test", error));
    const auto history_bytes = std::uint64_t(state.Capacity()) * kRibbonHistoryStride;
    const auto args_bytes = RibbonArgumentBufferBytes(widths[2],
        std::max<std::size_t>(state.Outputs().size(), 1) * kRibbonArgumentStride);
    const auto replace = RibbonBufferReplacements(widths, history_bytes, args_bytes);
    unsigned allocations{};
    std::array<std::weak_ptr<int>, 3> candidates;
    const auto allocate = [&](TestRibbonBuffer &buffer, std::uint64_t bytes) {
        buffer = {bytes, std::make_shared<int>(int(++allocations) + 10)};
        candidates[allocations - 1] = buffer.allocation;
        if (allocations == fail_allocation)
            return hs::Result::Failure(hs::ErrorCode::TaskFailed, "test", "injected ribbon allocation failure");
        return hs::Result::Success();
    };
    auto result = ReplaceRibbonBuffers(std::array{&buffers[0], &buffers[1], &buffers[2]},
        replace, history_bytes, args_bytes, allocate);
    if (!result)
    {
        for (const auto &candidate : candidates)
            Check(candidate.expired(), "failed ribbon candidates retained an allocation");
        return reject(result);
    }
    return result;
}

void TestRibbonLookupLifecycle()
{
    VfxRibbonState state;
    std::string error;
    auto a = Source(20);
    auto b = Source(10); b.effect_handle = 2;
    auto c = Source(10); c.source_id = 3;
    const auto update = [&](std::vector<VfxRibbonSourceInput> sources, hs::Tick tick) {
        Check(state.Update(sources, tick, 1, 1, 256, .5f, {}, error), error.c_str());
    };
    update({a, b, c}, 1);
    const auto c_generation = state.Updates()[0].identity[2];
    const auto b_generation = state.Updates()[1].identity[2];
    const auto a_generation = state.Updates()[2].identity[2];
    Check(state.Updates()[0].identity[0] == 0 && state.Updates()[1].identity[0] == 1 &&
          state.Updates()[2].identity[0] == 2, "source-key ordering did not assign ascending slots");
    a.importance = 9; b.importance = 5;
    update({c, a, b}, 2);
    Check(state.Updates()[0].identity[0] == 2 && state.Updates()[0].identity[2] == a_generation &&
          state.Updates()[1].identity[0] == 1 && state.Updates()[1].identity[2] == b_generation &&
          state.Updates()[2].identity[0] == 0 && state.Updates()[2].identity[2] == c_generation,
        "reordered priorities changed a source's slot or generation");
    update({b, a}, 3);
    Check(state.Updates().size() == 3 && state.Updates()[2].identity[0] == 0 &&
          state.Updates()[2].identity[3] == 0 && state.Outputs()[2].metadata[0] == 0,
        "missing source tail lost slot order after priority reordering");
    update({}, 100);
    auto d = Source(40);
    update({d}, 101);
    Check(state.Updates()[0].identity[0] == 3 && (state.Updates()[0].identity[3] & 2u),
        "new source did not preserve append-before-reuse policy");
    auto e = Source(50, 10);
    update({c, e}, 102);
    Check(state.Updates().size() == 3 && state.Updates()[0].identity[0] == 0 &&
          state.Updates()[1].identity[0] == 1 && state.Updates()[1].identity[2] != c_generation &&
          state.Updates()[2].identity[0] == 3 && state.Updates()[2].identity[3] == 0,
        "expired keys or tail protection corrupted deterministic slot reuse");
    c.analytic = true; c.history_seconds = 0; c.outputs[0].motion = VfxMotionKind::CurvedLinkTravelingHead;
    update({d, c, e}, 103);
    const auto first_analytic_generation = state.Updates()[1].identity[2];
    auto history = c; history.analytic = false; history.history_seconds = .2f;
    history.outputs[0].motion = VfxMotionKind::HistoryRibbonTaperedFlow;
    update({d, history, e}, 103);
    Check(state.Updates()[1].identity[0] == 1 && state.Updates()[1].identity[3] == 3u &&
          state.Updates()[1].identity[2] != first_analytic_generation,
        "same-tick analytic-to-history change did not reset the same slot");
    update({d, c, e}, 103);
    const auto analytic_generation = state.Updates()[1].identity[2];
    Check(state.Updates()[1].identity[0] == 1 && (state.Updates()[1].identity[3] & 7u) == 7u,
        "history-to-analytic change did not reset the same slot");
    update({d, e}, 104);
    Check(state.Updates().size() == 3 && state.Updates()[2].identity[0] == 1 &&
          state.Updates()[2].identity[3] == 4u, "missing analytic key did not clear exactly once");
    c.analytic = false; c.history_seconds = .2f; c.outputs[0].motion = VfxMotionKind::HistoryRibbonTaperedFlow;
    update({c, e, d}, 105);
    Check(state.Updates()[1].identity[0] == 1 && state.Updates()[1].identity[2] != analytic_generation &&
          state.Updates()[1].identity[3] == 3u, "cancelled analytic key survived slot reuse");
    auto f = Source(60, 12);
    auto g = Source(70, 11);
    e.importance = 10; c.importance = 9; d.importance = 0;
    update({d, c, g, e, f}, 106);
    Check(state.DroppedSources() == 1 && state.Updates().size() == 4 &&
          state.Updates()[0].identity[0] == 2 && state.Updates()[1].identity[0] == 3 &&
          state.Updates()[2].identity[0] == 0 && state.Updates()[3].identity[0] == 1,
        "overflow replacement stole a later winner or changed first reusable slot order");
    const auto before_duplicate = state.Updates()[0].identity;
    auto duplicate = d; duplicate.importance = 99;
    Check(!state.Update(std::vector{f, g, e, c, d, duplicate}, 107, 1, 1, 256, .5f, {}, error) &&
          state.Updates()[0].identity == before_duplicate,
        "overflow loser duplicate was accepted or mutated the successful frame");

    for (unsigned reset = 0; reset != 4; ++reset)
    {
        auto reset_state = state;
        Check(reset_state.Update(std::vector{c, e}, reset == 2 ? 105 : 107,
              reset == 0 ? 2 : 1, reset == 1 ? 2 : 1, reset == 3 ? 128 : 256, .5f, {}, error), error.c_str());
        Check(reset_state.Updates().size() == 2 && reset_state.Updates()[0].identity[0] == 0 &&
              reset_state.Updates()[1].identity[0] == 1 && reset_state.Updates()[0].identity[3] == 3u &&
              reset_state.Updates()[1].identity[3] == 3u,
            "session/catalog/rewind/capacity reset retained a stale key index");
    }
}
void TestRejectedResetKeepsState()
{
    const std::vector original{Source(10)};
    for (unsigned reset = 0; reset != 4; ++reset)
    {
        VfxRibbonState state;
        std::string error;
        Check(VfxRibbonState::ValidateSources(original, error), error.c_str());
        Check(state.Update(original, 10, 1, 1, 64, .5f, {}, error), error.c_str());
        const auto identity = state.Updates()[0].identity;
        auto rejected = original;
        if (reset & 1) rejected.push_back(rejected.front());
        else rejected.front().outputs[0].hdr = -1;
        Check(!VfxRibbonState::ValidateSources(rejected, error), "ribbon admission accepted invalid input");
        Check(!state.Update(rejected, reset == 2 ? 9 : 11, reset == 0 ? 2 : 1,
            reset == 1 ? 2 : 1, reset == 3 ? 128 : 64, .5f, {}, error), "reset accepted invalid ribbon input");
        Check(state.Capacity() == 1 && state.Updates().size() == 1 && state.Outputs().size() == 1 &&
            state.Updates()[0].identity == identity, "invalid input consumed a ribbon state reset");
        Check(state.Update(original, 11, 1, 1, 64, .5f, {}, error), error.c_str());
        Check(state.Updates()[0].identity[2] == identity[2] && state.Updates()[0].identity[3] == 1,
            "invalid input discarded the retained ribbon slot or generation");
    }
}

void TestRibbonDuplicateValidationScratchRecovery()
{
    VfxRibbonState state;
    std::string error;
    const std::vector original{Source(10)};
    Check(state.Update(original, 10, 1, 1, 64, .5f, {}, error), error.c_str());
    const auto identity = state.Updates()[0].identity;
    const auto output_metadata = state.Outputs()[0].metadata;

    auto duplicate = Source(20, 99);
    const std::vector rejected{Source(20, 1), Source(30, 1), duplicate};
    Check(!VfxRibbonState::ValidateSources(rejected, error),
          "raw ribbon validation accepted a nonadjacent duplicate key with different importance");
    Check(!state.ValidateFrameSources(rejected, error),
          "frame ribbon validation accepted a nonadjacent duplicate key with different importance");
    Check(state.ValidateFrameSources(original, error),
          "valid frame validation failed after duplicate rejection");
    Check(!state.Update(rejected, 11, 1, 1, 64, .5f, {}, error),
          "ribbon update accepted a duplicate key after frame validation");
    Check(state.Updates().size() == 1 && state.Outputs().size() == 1 &&
              state.Updates()[0].identity == identity && state.Outputs()[0].metadata == output_metadata,
          "duplicate rejection changed existing ribbon outputs");
    Check(state.Update(original, 11, 1, 1, 64, .5f, {}, error), error.c_str());
    Check(state.Updates().size() == 1 && state.Outputs().size() == 1 &&
              state.Updates()[0].identity[0] == identity[0] &&
              state.Updates()[0].identity[1] == 11 &&
              state.Updates()[0].identity[2] == identity[2] &&
              state.Updates()[0].identity[3] == 1 &&
              state.Outputs()[0].metadata == output_metadata,
          "valid ribbon frame did not recover after duplicate rejection");
}

void TestPoolBackedRibbonState()
{
    auto owning = Source(71);
    const auto frame_outputs = owning.outputs;
    auto pooled = owning;
    pooled.outputs.clear();
    pooled.frame_output_first = 0;
    pooled.frame_output_count = static_cast<std::uint32_t>(frame_outputs.size());

    VfxRibbonState owning_state, pooled_state;
    std::string error;
    Check(owning_state.Update(std::span(&owning, 1), 8, 2, 3, 64, .25f, {1, 2, 3}, error), error.c_str());
    Check(pooled_state.Update(std::span(&pooled, 1), 8, 2, 3, 64, .25f, {1, 2, 3}, error,
              frame_outputs), error.c_str());
    Check(SameBytes(owning_state.Updates(), pooled_state.Updates()) &&
              SameBytes(owning_state.Outputs(), pooled_state.Outputs()) &&
              owning_state.DroppedSources() == pooled_state.DroppedSources(),
          "pool-backed ribbon state changed source updates, outputs, or drop count");

    const auto preserve_state = [&] {
        const auto updates = std::vector(pooled_state.Updates().begin(), pooled_state.Updates().end());
        const auto outputs = std::vector(pooled_state.Outputs().begin(), pooled_state.Outputs().end());
        const auto dropped = pooled_state.DroppedSources();
        return [&, updates = std::move(updates), outputs = std::move(outputs), dropped] {
            Check(SameBytes(pooled_state.Updates(), std::span(updates)) &&
                      SameBytes(pooled_state.Outputs(), std::span(outputs)) &&
                      pooled_state.DroppedSources() == dropped,
                  "rejected pooled ribbon input changed retained state");
        };
    };
    const auto reject_without_mutation = [&](VfxRibbonSourceInput rejected,
                                             std::span<const VfxRibbonOutputInput> outputs) {
        const auto unchanged = preserve_state();
        Check(!pooled_state.Update(std::span(&rejected, 1), 9, 4, 5, 128, .5f, {}, error, outputs),
              "invalid pool-backed ribbon storage was accepted");
        unchanged();
    };

    auto missing = pooled;
    reject_without_mutation(missing, {});
    auto out_of_range = pooled;
    out_of_range.frame_output_first = 1;
    reject_without_mutation(out_of_range, frame_outputs);
    auto mixed = owning;
    mixed.frame_output_count = 1;
    reject_without_mutation(mixed, frame_outputs);
    auto nonfinite_outputs = frame_outputs;
    nonfinite_outputs[0].hdr = std::numeric_limits<float>::quiet_NaN();
    reject_without_mutation(pooled, nonfinite_outputs);
}

void TestRibbonBufferFailureRecovery()
{
    using namespace hs::renderer_detail;
    Check(RibbonArgumentBufferBytes(60, 80) == 120 && RibbonArgumentBufferBytes(120, 80) == 120 &&
          RibbonArgumentBufferBytes(0, 20) == 20 && RibbonArgumentBufferBytes(20, 1000) == 1000 &&
          RibbonArgumentBufferBytes(UINT64_MAX/2+1, UINT64_MAX) == UINT64_MAX,
        "ribbon argument growth shrank, overflowed, or lost amortized capacity");
    const std::vector original{Source(10)};
    auto growing = std::vector{Source(20)};
    growing[0].outputs.push_back(growing[0].outputs[0]);
    const auto make_buffers = [] {
        return std::array<TestRibbonBuffer, 3>{TestRibbonBuffer{kRibbonHistoryStride, std::make_shared<int>(1)},
            TestRibbonBuffer{kRibbonHistoryStride, std::make_shared<int>(2)},
            TestRibbonBuffer{kRibbonArgumentStride, std::make_shared<int>(3)}};
    };
    for (unsigned fail = 1; fail <= 3; ++fail)
    {
        VfxRibbonState state;
        std::string error;
        Check(state.Update(original, 10, 1, 1, 64, .5f, {}, error), error.c_str());
        auto buffers = make_buffers();
        const auto committed = buffers;
        bool checkpoint_copied{};
        Check(!TryRibbonBufferUpdate(state, buffers, growing, 128, 2, fail, checkpoint_copied),
            "injected ribbon allocation unexpectedly succeeded");
        Check(checkpoint_copied && state.Capacity() == 1 && state.Outputs().size() == 1 &&
              state.Updates()[0].identity[1] == 10 && state.Updates()[0].identity[3] == 3,
            "failed ribbon allocation consumed CPU slots or their reset state");
        for (std::size_t index = 0; index < buffers.size(); ++index)
            Check(buffers[index].width == committed[index].width &&
                  buffers[index].allocation == committed[index].allocation,
                "partial ribbon allocation changed a committed buffer");
        Check(bool(TryRibbonBufferUpdate(state, buffers, growing, 128, 2, 0, checkpoint_copied)),
            "ribbon allocation retry failed");
        Check(state.Capacity() == 2 && state.Outputs().size() == 2 &&
              (state.Updates()[0].identity[3] & 2u) != 0,
            "ribbon retry lost the new GPU history reset");
        Check(buffers[0].width == 2 * kRibbonHistoryStride && buffers[1].width == 2 * kRibbonHistoryStride &&
              buffers[2].width == 2 * kRibbonArgumentStride,
            "successful ribbon allocation did not commit the requested sizes");
        for (std::size_t index = 0; index < buffers.size(); ++index)
            Check(buffers[index].allocation != committed[index].allocation,
                "successful ribbon allocation retained a replaced buffer");
    }

    VfxRibbonState state;
    std::string error;
    Check(state.Update(original, 10, 1, 1, 128, .5f, {}, error), error.c_str());
    Check(!state.OutputsMayExceed(original, 1), "steady ribbon inputs requested a deep checkpoint");
    Check(state.OutputsMayExceed(std::vector{Source(20)}, 1), "retained tail was omitted from the output bound");
    Check(state.Update(std::vector{Source(20)}, 11, 1, 1, 128, .5f, {}, error), error.c_str());
    Check(state.Outputs().size() == 2, "tail bound fixture did not retain its previous source");

    state = {};
    Check(state.Update(original, 10, 1, 1, 64, .5f, {}, error), error.c_str());
    auto buffers = make_buffers();
    const auto committed = buffers;
    bool checkpoint_copied{};
    Check(bool(TryRibbonBufferUpdate(state, buffers, original, 64, 1, 0, checkpoint_copied)) && !checkpoint_copied,
        "steady ribbon frame copied state or failed");
    for (std::size_t index = 0; index < buffers.size(); ++index)
        Check(buffers[index].allocation == committed[index].allocation, "steady ribbon frame reallocated a buffer");

    const auto previous_frame_arguments = buffers[2];
    buffers[2] = {};
    Check(!TryRibbonBufferUpdate(state, buffers, original, 64, 1, 1, checkpoint_copied) && checkpoint_copied &&
          !buffers[2].allocation && previous_frame_arguments.allocation == committed[2].allocation,
        "failed frame-local argument allocation changed another frame's arguments");
    Check(bool(TryRibbonBufferUpdate(state, buffers, original, 64, 1, 0, checkpoint_copied)) &&
          buffers[2].allocation && buffers[2].allocation != previous_frame_arguments.allocation &&
          buffers[0].allocation == committed[0].allocation && buffers[1].allocation == committed[1].allocation,
        "frame-local arguments replaced shared history or another frame's allocation");
    const auto current_frame_arguments = buffers[2].allocation;

    buffers[1] = {};
    Check(!TryRibbonBufferUpdate(state, buffers, original, 64, 1, 1, checkpoint_copied) && checkpoint_copied,
        "missing previous history did not prepare a recoverable replacement");
    Check(!buffers[1].allocation && buffers[0].allocation == committed[0].allocation &&
          buffers[2].allocation == current_frame_arguments, "failed previous repair changed other buffers");
    Check(bool(TryRibbonBufferUpdate(state, buffers, original, 64, 1, 0, checkpoint_copied)) &&
          buffers[1].allocation && buffers[1].width == kRibbonHistoryStride &&
          buffers[0].allocation == committed[0].allocation && buffers[2].allocation == current_frame_arguments,
        "missing previous history did not recover independently");
    buffers[1].width = 16;
    Check(bool(TryRibbonBufferUpdate(state, buffers, original, 64, 1, 0, checkpoint_copied)) &&
          buffers[1].width == kRibbonHistoryStride, "mismatched previous history did not recover");

    for (auto rejected : {std::vector{Source(10), Source(10)}, growing})
    {
        if (rejected.size() == 1) rejected[0].outputs[0].hdr = -1;
        Check(state.OutputsMayExceed(rejected, 1), "rejected inputs underestimated their output bound");
        Check(!TryRibbonBufferUpdate(state, buffers, rejected, 128, 2, 0, checkpoint_copied) &&
              state.Capacity() == 1 && state.Outputs().size() == 1,
            "rejected ribbon source consumed a checkpointed generation or capacity reset");
    }
    Check(!TryRibbonBufferUpdate(state, buffers, growing, 64, 2, 1, checkpoint_copied) && checkpoint_copied,
        "generation reset argument growth was not checkpointed");
    Check(bool(TryRibbonBufferUpdate(state, buffers, growing, 64, 2, 0, checkpoint_copied)) &&
          (state.Updates()[0].identity[3] & 2u) != 0,
        "argument-only failure consumed a generation reset before retry");
}

void TestRibbonOutputCapacityAcrossPackedWords()
{
    VfxRibbonState state;
    Check(!state.OutputsMayExceed({}, 0), "empty ribbon output capacity changed");
    std::vector<VfxRibbonSourceInput> sources;
    for (std::uint64_t owner = 1; owner <= 130; ++owner) sources.push_back(Source(owner));
    std::string error;
    constexpr auto points = 130u * hs::renderer_detail::kRibbonHistorySamples;
    Check(state.Update(sources, 1, 1, 1, points, 1.0f, {}, error), error.c_str());
    Check(!state.OutputsMayExceed(sources, 130) && state.OutputsMayExceed(sources, 129),
          "packed ribbon inclusion lost a source across word boundaries");
    std::vector<VfxRibbonSourceInput> alternating;
    for (std::size_t index = 0; index < sources.size(); index += 2)
        alternating.push_back(sources[index]);
    Check(!state.OutputsMayExceed(alternating, 130) && state.OutputsMayExceed(alternating, 129),
          "packed ribbon inclusion lost a retained tail or kept a prior query mark");
    Check(!state.OutputsMayExceed(sources, 130), "early capacity rejection left stale inclusion bits");
    Check(state.Update(alternating, 2, 1, 1, points, 1.0f, {}, error) && state.Outputs().size() == 130,
          "source-order preparation reused packed inclusion data as row indices");
}
}

int main()
{
    try
    {
        TestRibbonLookupLifecycle();
        TestRejectedResetKeepsState();
        TestRibbonDuplicateValidationScratchRecovery();
        TestPoolBackedRibbonState();
        TestRibbonBufferFailureRecovery();
        TestRibbonOutputCapacityAcrossPackedWords();
        std::string error;
        auto source = Source(10);
        VfxRibbonState state;
        std::vector<VfxRibbonSourceInput> inputs{source};
        Check(state.Update(inputs, 10, 1, 1, 128, 0.5f, {4.0f, 5.0f, 6.0f}, error), error.c_str());
        Check(state.Capacity() == 2 && state.Updates().size() == 1 && state.Outputs().size() == 1, "initial ribbon state shape changed");
        Check(state.Updates()[0].identity[3] == 3 && std::abs(state.Updates()[0].rendered_head.x - 1.0f) < 0.001f, "ribbon reset or interpolation lost");
        Check(state.Outputs()[0].metadata[0] == 0 && state.Outputs()[0].metadata[1] == 3 && state.Outputs()[0].detail.x == 0.0f, "ribbon GPU packing changed");
        Check(std::bit_cast<std::uint32_t>(state.Outputs()[0].previous_eye.w) == 10u, "ribbon output lost exact current tick");

        Check(state.Update(inputs, 10, 1, 1, 128, 0.5f, {4.0f, 5.0f, 6.0f}, error), error.c_str());
        Check(state.Updates()[0].identity[3] == 1, "repeated render frame reset the ribbon slot");
        auto duplicate = inputs;
        duplicate.push_back(source);
        Check(!state.Update(duplicate, 11, 1, 1, 128, 0.0f, {}, error), "duplicate ribbon source accepted");

        auto two_outputs = Source(10);
        two_outputs.outputs.push_back({VfxOutputProfile::RibbonOit, VfxMotionKind::TwoPhaseNarrowWakes,
                                       {0.2f, 0.4f, 1.0f, 1.0f}, 0.1f, 0.02f, 0.0f, 0.2f, 1.0f, 0.25f, 7, 0xffffffffu, true});
        std::swap(two_outputs.outputs[0], two_outputs.outputs[1]);
        Check(state.Update(std::vector<VfxRibbonSourceInput>{two_outputs}, 12, 1, 1, 128, 1.0f, {}, error), error.c_str());
        Check(state.Updates().size() == 1 && state.Outputs().size() == 2 &&
              (state.Outputs()[0].metadata[3] & 1u) == 0u && (state.Outputs()[1].metadata[3] & 1u) == 1u &&
              state.Outputs()[1].metadata[2] == 2 && state.Outputs()[1].metadata[3] == 3 && state.Outputs()[1].material.w == 4.0f,
              "shared ribbon history outputs were duplicated or profile-sorted incorrectly");

        auto grouping_checkpoint = state;
        const auto first_group = state.Outputs()[0].metadata;
        const auto second_group = state.Outputs()[1].metadata;
        const std::array repeated_groups{two_outputs};
        for (unsigned repeat = 0; repeat < 3; ++repeat)
        {
            Check(state.Update(repeated_groups, 12, 1, 1, 128, 1.0f, {}, error), error.c_str());
            Check(state.Outputs().size() == 2 && state.Outputs()[0].metadata == first_group &&
                      state.Outputs()[1].metadata == second_group,
                  "warm ribbon grouping retained stale rows or changed profile order");
        }
        const std::array single_group{Source(10)};
        Check(state.Update(single_group, 12, 1, 1, 128, 1.0f, {}, error) && state.Outputs().size() == 1,
              "ribbon grouping scratch kept outputs after the source shrank");
        state = std::move(grouping_checkpoint);
        Check(state.Update(repeated_groups, 12, 1, 1, 128, 1.0f, {}, error) &&
                  state.Outputs().size() == 2 && state.Outputs()[0].metadata == first_group &&
                  state.Outputs()[1].metadata == second_group,
              "restored ribbon checkpoint lost output grouping");

        Check(state.Update(inputs, 13, 2, 1, 128, 0.0f, {}, error), error.c_str());
        Check(state.Updates()[0].identity[3] == 3, "session reset did not reset the reused ribbon slot");
        Check(state.Update(inputs, 14, 2, 2, 128, 0.0f, {}, error), error.c_str());
        Check(state.Updates()[0].identity[3] == 3, "catalog reset did not reset the reused ribbon slot");
        Check(state.Update(inputs, 13, 2, 2, 64, 0.0f, {}, error), error.c_str());
        Check(state.Capacity() == 1 && state.Updates()[0].identity[3] == 3, "backward tick or budget reset failed");

        auto lower = Source(30, 0);
        Check(state.Update(std::vector<VfxRibbonSourceInput>{inputs[0], lower}, 15, 3, 1, 64, 0.0f, {}, error), error.c_str());
        Check(state.DroppedSources() == 1, "ribbon dropped source count changed");

        VfxRibbonState tail;
        Check(tail.Update(inputs, 1, 1, 1, 64, 0.0f, {}, error), error.c_str());
        Check(tail.Update({}, 2, 1, 1, 64, 0.0f, {}, error), error.c_str());
        Check(tail.Updates().size() == 1 && tail.Updates()[0].identity[3] == 0 && tail.Outputs().size() == 1 &&
              tail.Outputs()[0].previous_eye.x == 0.0f && tail.Outputs()[0].previous_eye.y == 0.0f &&
              tail.Outputs()[0].previous_eye.z == 0.0f && std::bit_cast<std::uint32_t>(tail.Outputs()[0].previous_eye.w) == 2u,
              "retired ribbon tail was not emitted with output");
        auto replacement = Source(99);
        Check(tail.Update(std::vector<VfxRibbonSourceInput>{replacement}, 20, 1, 1, 64, 0.0f, {}, error), error.c_str());
        Check(tail.DroppedSources() == 0 && tail.Updates()[0].identity[3] == 3, "retired ribbon slot was not reclaimed");

        VfxRibbonState priority;
        auto low = Source(40, 1);
        Check(priority.Update(std::vector<VfxRibbonSourceInput>{low}, 1, 1, 1, 64, 0.0f, {}, error), error.c_str());
        auto high = Source(41, 9);
        Check(priority.Update(std::vector<VfxRibbonSourceInput>{low, high}, 2, 1, 1, 64, 0.0f, {}, error), error.c_str());
        Check(priority.DroppedSources() == 1 && priority.Updates().size() == 1 && priority.Updates()[0].identity[3] == 3 && priority.Outputs().size() == 1, "higher priority ribbon did not take over the full slot");
        auto duplicate_priority = std::vector<VfxRibbonSourceInput>{low, low};
        duplicate_priority[1].importance = 99;
        Check(!priority.Update(duplicate_priority, 3, 1, 1, 64, 0.0f, {}, error), "duplicate ribbon key with different importance accepted");

        auto invalid = Source(7);
        invalid.history_seconds = 1.1f;
        Check(!tail.Update(std::vector<VfxRibbonSourceInput>{invalid}, 21, 1, 1, 64, 0.0f, {}, error), "overlong ribbon history accepted");
        invalid = Source(7);
        invalid.outputs[0].gradient_row = 0xffffffffu;
        Check(!tail.Update(std::vector<VfxRibbonSourceInput>{invalid}, 21, 1, 1, 64, 0.0f, {}, error), "invalid gradient row accepted");
        invalid = Source(7);
        invalid.outputs[0].detail_slice = 8;
        Check(!tail.Update(std::vector<VfxRibbonSourceInput>{invalid}, 21, 1, 1, 64, 0.0f, {}, error), "invalid detail slice accepted");
        invalid = Source(7);
        invalid.outputs[0].motion = VfxMotionKind::HistoryRibbonTearsBackward;
        Check(!tail.Update(std::vector<VfxRibbonSourceInput>{invalid}, 21, 1, 1, 64, 0.0f, {}, error),
              "ground rails accepted a missing lane width");

        VfxRibbonState dash_state;
        auto dash = Source(76);
        dash.source_id = 123;
        dash.position = {8.0f, .025f, 3.0f};
        dash.previous_position = {2.0f, .025f, 3.0f};
        dash.control.x = 1.8f;
        dash.history_seconds = .24f;
        dash.outputs[0].motion = VfxMotionKind::HistoryRibbonTearsBackward;
        Check(dash_state.Update(std::vector{dash}, 1, 1, 1, 64, 1.0f, {}, error), error.c_str());
        Check(dash_state.Updates().size() == 1 && dash_state.Outputs().size() == 1 &&
              dash_state.Updates()[0].previous_interpolation.x == 2.0f &&
              dash_state.Updates()[0].position_duration.x == 8.0f &&
              dash_state.Outputs()[0].metadata[2] == 7 &&
              dash_state.Outputs()[0].analytic.w == 1.8f &&
              dash_state.Outputs()[0].material.y == .24f,
              "dash rail did not pack its actual segment, paired motion, or bounded history");
        dash.position.x = 10.0f;
        Check(dash_state.Update(std::vector{dash}, 2, 1, 1, 64, .5f, {}, error), error.c_str());
        Check(dash_state.Updates()[0].identity[3] == 1 &&
              dash_state.Updates()[0].position_duration.x == 10.0f &&
              dash_state.Updates()[0].rendered_head.x == 9.0f,
              "dash rail did not interpolate its growing tip from the prior tick");
        Check(dash_state.Update({}, 3, 1, 1, 64, 0.0f, {}, error), error.c_str());
        Check(dash_state.Outputs().size() == 1 && dash_state.Updates()[0].identity[3] == 0,
              "dash rail did not retain its short history after owner expiry");
        Check(dash_state.Update({}, 17, 1, 1, 64, 0.0f, {}, error), error.c_str());
        Check(dash_state.Outputs().empty(), "dash rail outlived its 0.24 second history");
        auto next_dash = dash;
        next_dash.owner_id = 77;
        Check(dash_state.Update(std::vector{next_dash}, 18, 1, 1, 64, 0.0f, {}, error), error.c_str());
        Check(dash_state.Updates().size() == 1 && (dash_state.Updates()[0].identity[3] & 2u),
              "expired dash rail was not reset for a new owner");

        VfxRibbonState axial_state;
        auto axial = Source(8);
        axial.outputs[0].motion = VfxMotionKind::ShortLineIncomingVelocity;
        Check(axial_state.Update(std::vector<VfxRibbonSourceInput>{axial}, 1, 1, 1, 64, 0.0f, {}, error), error.c_str());
        Check(axial_state.Outputs().size() == 1 && axial_state.Outputs()[0].metadata[2] == 6 &&
              axial_state.Outputs()[0].metadata[3] == 0,
              "short incoming velocity ribbon did not receive the distinct one-strand motion code");

        VfxRibbonState analytic_state;
        auto link = Source(101);
        link.analytic = true;
        link.position = {8, 2, 3}; link.previous_position = {-2, 1, 0}; link.control = {1, 5, -1};
        link.segments = 16; link.normalized_age = .4f; link.history_seconds = 0;
        link.outputs[0].motion = VfxMotionKind::CurvedLinkTravelingHead;
        link.outputs[0].travel_pulse = true;
        auto sheath = link.outputs[0]; sheath.motion = VfxMotionKind::SoftWiderSheath;
        sheath.profile = VfxOutputProfile::RibbonOit; sheath.travel_pulse = false;
        link.outputs.push_back(sheath);
        Check(analytic_state.Update(std::vector{link}, 100, 1, 1, 64, .25f, {}, error), error.c_str());
        const auto &update = analytic_state.Updates()[0];
        Check((update.identity[3]&7u)==7u && ((update.identity[3]>>8)&255u)==16u &&
              update.rendered_head.x==8 && update.previous_interpolation.x==-2 &&
              update.control_age.y==5 && update.control_age.w==.4f,
              "analytic segment/control/age or endpoint packing incorrect");
        Check(analytic_state.Outputs().size()==2 && analytic_state.Outputs()[0].metadata[2]==4 &&
              analytic_state.Outputs()[1].metadata[2]==5 && analytic_state.Outputs()[0].analytic.x==.4f &&
              analytic_state.Outputs()[0].analytic.y==1 && analytic_state.Outputs()[1].analytic.y==0 &&
              analytic_state.Outputs()[0].analytic.z==1,"analytic shared outputs or pulse missing");
        link.control.y=7;
        Check(analytic_state.Update(std::vector{link},100,1,1,64,.8f,{},error),error.c_str());
        Check((analytic_state.Updates()[0].identity[3]&2u)==0 && analytic_state.Updates()[0].control_age.y==7,
              "same-tick analytic refresh reset history or lost changed control");
        Check(analytic_state.Update({},100,1,1,64,0,{},error),error.c_str());
        Check(analytic_state.Outputs().empty() && analytic_state.Updates().size()==1 && analytic_state.Updates()[0].identity[3]==4,
              "cancelled analytic source retained a temporal tail");
        Check(analytic_state.Update({},101,1,1,64,0,{},error),error.c_str());
        Check(analytic_state.Updates().empty(),"analytic cancellation repeated indefinitely");
        Check(analytic_state.Update(std::vector{link},102,1,1,64,0,{},error),error.c_str());
        Check((analytic_state.Updates()[0].identity[3]&2u)!=0,"reclaimed analytic slot was not reset");
        auto invalid_link=link; invalid_link.segments=64;
        Check(!analytic_state.Update(std::vector{invalid_link},103,1,1,64,0,{},error),"analytic point count exceeded fixed slot");
        invalid_link=link; invalid_link.control.x=std::numeric_limits<float>::infinity();
        Check(!analytic_state.Update(std::vector{invalid_link},103,1,1,64,0,{},error),"nonfinite analytic control accepted");
        invalid_link=link; invalid_link.normalized_age=1.1f;
        Check(!analytic_state.Update(std::vector{invalid_link},103,1,1,64,0,{},error),"out-of-range analytic owner age accepted");
        invalid_link=link; invalid_link.analytic=false;
        Check(!analytic_state.Update(std::vector{invalid_link},103,1,1,64,0,{},error),"link motion accepted as history emitter");

        std::cout << "vfx ribbon state contracts passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
