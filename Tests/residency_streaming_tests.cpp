#include "residency_manager.hpp"
#include "streaming_manager.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace
{
void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    try
    {
        hs::ResidencyManager residency;
        residency.UpdateBudget({true, 1'000, 600, 2'000, 100});
        Check(residency.CanAdmit(250), "safe local-budget admission rejected");
        Check(!residency.CanAdmit(350), "unsafe local-budget admission accepted");
        const auto fallback = residency.Register(hs::ResidencyCategory::Texture, 80, true);
        const auto streamed = residency.Register(hs::ResidencyCategory::Texture, 200, false);
        const auto staging = residency.Register(hs::ResidencyCategory::Staging, 40, false);
        Check(fallback && streamed && staging, "residency tickets were not allocated");
        Check(!residency.Retire(fallback, {1, 0, 0}), "pinned fallback was retired");
        Check(residency.Retire(staging, {0, 0, 3}), "staging retirement failed");
        Check(residency.Collect({0, 0, 2}) == 0, "staging reclaimed before copy completion");
        Check(residency.Collect({0, 0, 3}) == 1, "staging was not reclaimed at completion");
        auto accounting = residency.Accounting();
        Check(accounting.live_total == 280 && accounting.retiring_total == 0,
              "residency live accounting mismatch");
        Check(accounting.reclaimed_total == 40, "residency reclaimed accounting mismatch");
        Check(residency.ReleaseNow(streamed), "explicit resident release failed");
        accounting = residency.Accounting();
        Check(accounting.live_total == 80 && accounting.reclaimed_total == 240,
              "residency explicit release accounting mismatch");

        hs::StreamingManager streaming;
        const auto request = streaming.Request("render/texture/environment_terrain_gaussian.rpkg",
                                               "mips:0-3", "generation-a", 100);
        Check(request && streaming.BeginRead(request), "stream request did not enter Reading");
        Check(!streaming.CompleteRead(request, "generation-b"),
              "stale generation read completion was accepted");
        const auto stale = streaming.Find(request);
        Check(stale && stale->state == hs::StreamingPageState::Cancelled,
              "stale completion did not cancel request");
        Check(streaming.Collect({}) == 1, "cancelled request was not collected");

        const auto live = streaming.Request("render/texture/environment_terrain_gaussian.rpkg",
                                            "mips:0-3", "generation-c", 100);
        Check(streaming.BeginRead(live), "live request did not enter Reading");
        Check(streaming.CompleteRead(live, "generation-c"), "live read completion failed");
        Check(streaming.BeginUpload(live, "generation-c"), "live upload did not begin");
        Check(!streaming.TryPublish(live, "generation-c", {}), "unsubmitted upload was published");
        Check(streaming.SubmitUpload(live, "generation-c", {0, 0, 7}),
              "live upload completion was not recorded");
        Check(!streaming.TryPublish(live, "generation-c", {0, 0, 6}),
              "stream page published before copy completion");
        Check(streaming.TryPublish(live, "generation-c", {0, 0, 7}),
              "stream page did not publish after copy completion");
        Check(streaming.Retire(live, {4, 2, 0}), "resident page did not enter Retiring");
        Check(streaming.Collect({4, 2, 6}) == 0, "retirement discarded the original upload completion");
        Check(streaming.Collect({3, 2, 7}) == 0,
              "retiring page reclaimed before all queue consumers completed");
        Check(streaming.Collect({4, 2, 7}) == 1,
              "retiring page was not reclaimed after CompletionSet");

        const auto texture = streaming.Request("render/texture/environment_terrain_gaussian.rpkg",
                                               "mips:0-3", "generation-d", 100);
        const auto geometry = streaming.Request("render/trunk_0.rpkg",
                                                "vertices:lod0", "generation-d", 90);
        Check(texture && geometry && texture != geometry,
              "concurrent texture/geometry stream requests did not receive unique identities");
        Check(streaming.BeginRead(texture) && streaming.BeginRead(geometry),
              "concurrent stream requests did not enter Reading");
        Check(streaming.CompleteRead(texture, "generation-d") &&
                  streaming.CompleteRead(geometry, "generation-d"),
              "concurrent stream reads did not complete");
        Check(streaming.BeginUpload(texture, "generation-d") &&
                  streaming.BeginUpload(geometry, "generation-d"),
              "concurrent stream uploads did not begin");
        Check(streaming.SubmitUpload(texture, "generation-d", {0, 0, 8}) &&
                  streaming.SubmitUpload(geometry, "generation-d", {0, 0, 9}),
              "concurrent stream upload completions were not recorded");
        Check(streaming.TryPublish(texture, "generation-d", {0, 0, 8}),
              "texture page did not publish at its completion");
        Check(!streaming.TryPublish(geometry, "generation-d", {0, 0, 8}) &&
                  streaming.TryPublish(geometry, "generation-d", {0, 0, 9}),
              "geometry page publication ignored its independent completion");

        const auto cancelled = streaming.Request("render/geometry/trunk_0.rpage", "page", "generation-e", 100);
        Check(streaming.BeginRead(cancelled) && streaming.CompleteRead(cancelled, "generation-e") &&
            streaming.BeginUpload(cancelled, "generation-e") &&
            streaming.SubmitUpload(cancelled, "generation-e", {5, 3, 10}) && streaming.Cancel(cancelled),
            "submitted upload cancellation failed");
        Check(streaming.Collect({4, 3, 10}) == 0 && streaming.Find(cancelled),
            "cancelled upload lost its record before direct completion");
        Check(streaming.Collect({5, 2, 10}) == 0 && streaming.Collect({5, 3, 9}) == 0,
            "cancelled upload ignored compute or copy completion");
        Check(streaming.Collect({5, 3, 10}) == 1, "completed cancelled upload was not collected");
        streaming.Reset();
        const auto after_reset = streaming.Request("render/geometry/trunk_0.rpage", "page", "generation-f", 100);
        Check(after_reset.value > cancelled.value && !streaming.BeginRead(geometry),
            "reset aliased an old asynchronous request identity");
        std::cout << "residency budget, concurrent generation-aware streaming and CompletionSet retirement passed\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
