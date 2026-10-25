#include "renderer_caster_storage.hpp"

#include <array>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace
{
using Payload = std::array<float, 14>;
struct Record
{
    Payload caster{};
    std::uint64_t prepared_epoch{};
};
using Map = std::unordered_map<int, Record>;
using Input = std::pair<int, Payload>;
struct History
{
    Map previous, scratch;
    std::uint64_t epoch{};
};
struct Prepared
{
    Map current;
    std::size_t allocations{};
};

void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

Payload Values(float first)
{
    Payload result{};
    for (std::size_t index = 0; index < result.size(); ++index)
        result[index] = first + static_cast<float>(index);
    return result;
}

Prepared Prepare(History &history, std::initializer_list<Input> inputs)
{
    using namespace hs::renderer_detail;
    const auto epoch = BeginShadowCasterPreparation(history.scratch, history.epoch);
    Prepared result{std::move(history.scratch)};
    std::unordered_map<int, Payload> fresh_reference;
    std::vector<Input> expected_order, actual_order;
    for (const auto &[key, payload] : inputs)
    {
        const bool expected = fresh_reference.try_emplace(key, payload).second;
        if (expected) expected_order.emplace_back(key, payload);
        const auto [current, allocated] = result.current.try_emplace(key);
        result.allocations += allocated;
        const bool inserted = MarkShadowCasterPrepared(current->second.prepared_epoch, epoch);
        Check(inserted == expected, "reused nodes must preserve first-key-wins membership");
        if (!inserted) continue;
        current->second.caster = payload;
        actual_order.emplace_back(key, payload);
    }
    PruneShadowCasterStorage(result.current, epoch, actual_order.size());
    Check(actual_order == expected_order, "reuse must preserve caster values and submission order");
    Check(result.current.size() == fresh_reference.size(), "stale scratch keys must be removed");
    for (const auto &[key, payload] : fresh_reference)
    {
        const auto &record = result.current.at(key);
        Check(record.caster == payload && record.prepared_epoch == epoch,
            "every retained record must contain this attempt's complete payload");
    }
    return result;
}

void Commit(History &history, Prepared &prepared)
{
    hs::renderer_detail::CommitShadowCasterStorage(history.previous, history.scratch, prepared.current);
}

void TestReuseRemovalAndReappearance()
{
    History history;
    auto first = Prepare(history, {{1, Values(10)}, {2, Values(20)}, {1, Values(999)}});
    Check(first.allocations == 2 && history.epoch == 1, "first preparation starts with fresh nodes");
    Commit(history, first);
    auto second = Prepare(history, {{1, Values(30)}, {2, Values(40)}});
    Commit(history, second);
    const auto *retained_node = &history.scratch.at(2);
    auto warm = Prepare(history, {{1, Values(50)}, {2, Values(60)}, {2, Values(999)}});
    Check(warm.allocations == 0 && &warm.current.at(2) == retained_node,
        "warm preparation must reuse nodes without allocating or retaining old values");
    Commit(history, warm);
    auto replacement = Prepare(history, {{1, Values(70)}, {3, Values(80)}});
    Check(!replacement.current.contains(2) && history.previous.at(2).caster == Values(60),
        "same-cardinality replacement must prune scratch while preserving submitted history");
    Commit(history, replacement);
    auto reappeared = Prepare(history, {{2, Values(90)}, {3, Values(100)}});
    Check(reappeared.current.at(2).caster == Values(90), "reappearing keys must receive fresh values");
    Commit(history, reappeared);
    auto empty = Prepare(history, {});
    Check(empty.current.empty() && history.previous.size() == 2,
        "empty preparation must prune every node without prematurely deleting history");
    Commit(history, empty);
    Check(history.previous.empty(), "successful empty submission must retire every previous key");
}

void TestFailedPreparationAndEpochWrap()
{
    History history;
    auto first = Prepare(history, {{1, Values(10)}});
    Commit(history, first);
    auto second = Prepare(history, {{1, Values(20)}});
    Commit(history, second);
    {
        auto failed = Prepare(history, {{1, Values(30)}, {2, Values(40)}});
        Check(failed.current.size() == 2 && history.previous.at(1).caster == Values(20) &&
            !history.previous.contains(2),
            "preparation failure must leave the last submitted values and key set unchanged");
    }
    const auto failed_epoch = history.epoch;
    auto retry = Prepare(history, {{1, Values(30)}, {2, Values(40)}});
    Check(history.epoch != failed_epoch && retry.current.size() == 2,
        "retry must not mistake failed-attempt stamps for duplicates");
    Commit(history, retry);
    history.epoch = std::numeric_limits<std::uint64_t>::max();
    history.scratch.at(1).prepared_epoch = 1;
    auto wrapped = Prepare(history, {{1, Values(50)}});
    Check(history.epoch == 1 && wrapped.allocations == 1,
        "epoch wrap must clear scratch nodes before reusing the first epoch");
    Check(history.previous.at(1).caster == Values(30) && history.previous.contains(2),
        "epoch wrap must preserve submitted history until commit");
    Commit(history, wrapped);
}

void TestRetainedIndependentKeyOracle()
{
    using namespace hs::renderer_detail;
    std::unordered_map<int, std::uint64_t> oracle, other_view;
    bool prepared{}, other_prepared{};
    const auto prepare = [&](std::uint64_t epoch, std::initializer_list<int> keys, bool finish) {
        BeginShadowCasterKeyOracle(oracle, epoch, prepared);
        std::unordered_set<int> fresh;
        for (const auto key : keys)
            Check(MarkShadowCasterKeyOracle(oracle, key, epoch) == fresh.insert(key).second,
                "retained oracle must match independent fresh first-key membership");
        if (finish)
        {
            PruneShadowCasterKeyOracle(oracle, epoch, fresh.size(), prepared);
            Check(oracle.size() == fresh.size(), "oracle must release stale keys after preparation");
        }
    };
    prepare(1, {1, 2, 1}, true);
    prepare(2, {2, 3, 2}, false); // A rejected preparation leaves partial marks.
    prepare(3, {3, 4, 3}, true);
    Check(!oracle.contains(1) && !oracle.contains(2), "failed-attempt stale keys must not accumulate");
    const auto *retained = &oracle.at(3);
    prepare(4, {3, 4, 3}, true);
    Check(&oracle.at(3) == retained, "steady oracle membership must reuse its allocated nodes");
    BeginShadowCasterKeyOracle(other_view, 1, other_prepared);
    Check(MarkShadowCasterKeyOracle(other_view, 3, 1) && oracle.at(3) == 4,
        "oracle epochs must remain isolated between views");
    prepare(std::numeric_limits<std::uint64_t>::max(), {3, 3}, true);
    oracle.at(3) = 1; // Simulate an ancient first-epoch mark before wrap.
    prepare(1, {3, 3}, true);
    Check(oracle.size() == 1 && oracle.at(3) == 1,
        "wrapped oracle must clear ancient epoch stamps before first-key checks");
    prepare(2, {}, true);
    Check(oracle.empty(), "empty or reset scene must release retained oracle keys");
}

void TestFrameLocalGroups()
{
    struct CountedKey
    {
        std::uint64_t value{};
        std::size_t *comparisons{};
        bool operator==(std::uint64_t other) const
        {
            ++*comparisons;
            return value == other;
        }
    };
    struct Group { CountedKey binding_key; int frontend{}; };
    std::vector<Group> groups;
    std::array<std::uint32_t, 5> indices;
    indices.fill(std::numeric_limits<std::uint32_t>::max());
    std::size_t comparisons{};
    const auto resolve = [&](std::size_t selector, std::uint64_t key, int frontend) {
        return hs::renderer_detail::ResolveShadowCasterGroup(indices[selector], groups, key,
            [&] { groups.push_back({{key, &comparisons}, frontend}); });
    };
    Check(resolve(0, 11, 7) == 0 && resolve(1, 22, 8) == 1,
        "first visits must append bindings in original encounter order");
    Check(resolve(2, 11, 99) == 0 && groups[0].frontend == 7 && groups.size() == 2,
        "different selectors sharing a key must retain the first binding");
    const auto warm_comparisons = comparisons;
    for (int index = 0; index < 1000; ++index)
        Check(resolve(0, 11, 7) == 0 && resolve(1, 22, 8) == 1 && resolve(2, 11, 99) == 0,
            "cached selectors must retain their original group indices");
    Check(comparisons == warm_comparisons,
        "repeat selectors must not repeat the binding-key linear search");
    Check(resolve(3, 33, 9) == 2 && resolve(4, 44, 10) == 3 && resolve(0, 11, 7) == 0,
        "vector growth must not invalidate cached indices");

    // An attempt owns all indices; a new frame must resolve its new bindings.
    groups.clear();
    indices.fill(std::numeric_limits<std::uint32_t>::max());
    Check(resolve(0, 22, 12) == 0 && groups[0].frontend == 12 && resolve(1, 11, 13) == 1,
        "next-frame bindings must not inherit the previous frame's indices or payload");
    auto failed_index = std::numeric_limits<std::uint32_t>::max();
    bool failed{};
    try
    {
        (void)hs::renderer_detail::ResolveShadowCasterGroup(failed_index, groups, 55,
            [] { throw std::runtime_error("append failure"); });
    }
    catch (const std::runtime_error &) { failed = true; }
    Check(failed && failed_index == std::numeric_limits<std::uint32_t>::max() && groups.size() == 2,
        "failed append must leave the selector cache unpublished");
    Check(hs::renderer_detail::ResolveShadowCasterGroup(failed_index, groups, 55,
            [&] { groups.push_back({{55, &comparisons}, 14}); }) == 2,
        "retry after failed append must resolve the complete binding");
}

void TestIndependentViews()
{
    History main, preview;
    for (int frame = 0; frame < 4; ++frame)
    {
        auto a = Prepare(main, {{1, Values(static_cast<float>(frame))}});
        Commit(main, a);
        auto b = Prepare(preview, {{1, Values(100.0f + static_cast<float>(frame))}});
        Commit(preview, b);
    }
    Check(main.previous.at(1).caster == Values(3) && preview.previous.at(1).caster == Values(103),
        "each view must retain its own submitted values, scratch nodes and preparation epoch");
}

void TestDenseSubmittedHistory()
{
    struct Caster
    {
        Payload current_bounds{},previous_bounds{},current_revisions{},previous_revisions{};
        std::uint64_t current_cut_revision{},previous_cut_revision{};
        std::uint32_t instance_generation{},previous_generation{};
        bool present{true},previous_present{},immutable_source{true},previous_immutable_source{};
        bool skin{},wind{true},animated_alpha{true},previous_skin{},previous_wind{},previous_animated_alpha{};
    };
    Caster submitted;
    submitted.current_bounds=Values(1); submitted.current_revisions=Values(2);
    submitted.current_cut_revision=5; submitted.instance_generation=7;
    const auto failed_candidate=submitted;
    auto candidate=submitted;
    hs::renderer_detail::AdvanceShadowCasterHistory(candidate);
    Check(!submitted.previous_present && !failed_candidate.previous_present,
        "settling a candidate must not publish history into the previous source batch");
    submitted=candidate;
    Check(submitted.previous_present && submitted.previous_immutable_source && submitted.previous_wind &&
        submitted.previous_animated_alpha && !submitted.previous_skin && submitted.wind && submitted.animated_alpha,
        "settled immutable source must retain wind/alpha dynamic-raster classification");
    Check(submitted.previous_bounds==submitted.current_bounds && submitted.previous_revisions==submitted.current_revisions &&
        submitted.previous_cut_revision==5 && submitted.previous_generation==7,
        "successful dense source history must settle all invalidation inputs");
    auto removed=submitted;
    hs::renderer_detail::AdvanceShadowCasterHistory(removed); removed.present=false;
    Check(!removed.present && removed.previous_present && removed.previous_bounds==Values(1),
        "deleted source retains last submitted conservative bounds");
    auto changed=submitted; changed.current_bounds=Values(8); changed.current_cut_revision=9;
    Check(changed.previous_bounds==Values(1) && changed.previous_cut_revision==5,
        "a changed source retains old bounds until successful submission");
    hs::renderer_detail::AdvanceShadowCasterHistory(changed);
    Check(changed.previous_bounds==Values(8) && changed.previous_cut_revision==9,
        "retry success settles the proposed bounds once");
}
} // namespace

int main()
{
    try
    {
        TestReuseRemovalAndReappearance();
        TestFailedPreparationAndEpochWrap();
        TestIndependentViews();
        TestFrameLocalGroups();
        TestRetainedIndependentKeyOracle();
        TestDenseSubmittedHistory();
        std::cout << "caster storage: reuse, duplicates, replacement, removal, retry, wrap, views and dense submitted history passed\n";
        return 0;
    }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
