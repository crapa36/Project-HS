#include "renderer_caster_storage.hpp"

#include <array>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <tuple>
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

using NodeKey = std::tuple<bool, std::uint64_t, std::uint32_t>;
struct NodeHash
{
    std::size_t *calls{};
    std::size_t operator()(const NodeKey &key) const noexcept
    {
        if (calls) ++*calls;
        auto hash = std::hash<std::uint64_t>{}(std::get<1>(key));
        hash ^= std::hash<std::uint32_t>{}(std::get<2>(key)) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        return hash ^ (std::get<0>(key) + 0x9e3779b9u + (hash << 6) + (hash >> 2));
    }
};
using NodeMap = std::unordered_map<NodeKey, Record, NodeHash>;
struct NodeScene { std::vector<std::uint64_t> stable_ids; };
using NodeLease = std::shared_ptr<const NodeScene>;
using Nodes = hs::renderer_detail::ShadowCasterNodeCache<NodeMap, NodeLease>;
struct NodeRef { std::uint32_t instance_index{}, instance_generation{}; };
struct NodeHistory
{
    std::size_t hashes{};
    NodeMap previous{0, NodeHash{&hashes}}, scratch{0, NodeHash{&hashes}};
    Nodes previous_nodes, scratch_nodes;
    std::uint64_t epoch{};
};
struct NodeOutput
{
    NodeKey key;
    Payload current{}, previous{};
    bool previous_present{};
    bool operator==(const NodeOutput &) const = default;
};
struct NodePrepared
{
    NodeMap current;
    Nodes current_nodes, prepared_previous_nodes;
    bool previous_warm{};
    std::size_t static_hashes{};
    std::vector<NodeOutput> order;
};

NodePrepared PrepareNodes(NodeHistory &history, const NodeLease &lease, std::uint32_t generation,
    const std::vector<NodeRef> &refs, std::initializer_list<Input> dynamic, int frame, bool cached,
    bool new_session = false, std::optional<std::size_t> fail_after = {})
{
    using namespace hs::renderer_detail;
    NodePrepared result;
    const auto epoch = BeginShadowCasterPreparation(history.scratch, history.epoch,
        history.scratch_nodes, result.current_nodes);
    result.current = std::move(history.scratch);
    if (cached)
    {
        result.previous_warm = history.previous_nodes.Matches(lease, generation, refs);
        if (!result.previous_warm) result.prepared_previous_nodes.Prepare(lease, generation, refs);
        if (!result.current_nodes.Matches(lease, generation, refs)) result.current_nodes.Prepare(lease, generation, refs);
    }
    else result.current_nodes.Invalidate();
    result.current.reserve(dynamic.size() + refs.size()); // Forces the production rehash boundary.
    const auto append = [&](const NodeKey &key, const Payload &payload, std::size_t ordinal, bool immutable) {
        auto *previous_cache = immutable && cached ? (result.previous_warm
            ? &history.previous_nodes : &result.prepared_previous_nodes) : nullptr;
        const auto *previous = FindPreviousShadowCasterNode(history.previous, key,
            previous_cache, ordinal, result.previous_warm);
        auto *current = FindCurrentShadowCasterNode(result.current, key,
            immutable && cached ? &result.current_nodes : nullptr, ordinal);
        if (!MarkShadowCasterPrepared(current->prepared_epoch, epoch)) return;
        NodeOutput output{key, payload};
        if (!new_session && previous) { output.previous = previous->caster; output.previous_present = true; }
        current->caster = payload;
        result.order.push_back(output);
    };
    std::size_t dynamic_ordinal{};
    for (const auto &[id, payload] : dynamic)
    {
        append({false, static_cast<std::uint64_t>(id), id == 0 ? static_cast<std::uint32_t>(dynamic_ordinal + 1) : 0u},
            payload, dynamic_ordinal, false);
        ++dynamic_ordinal;
    }
    const auto before_static = history.hashes;
    for (std::size_t ordinal = 0; ordinal < refs.size(); ++ordinal)
    {
        if (fail_after == ordinal) throw std::runtime_error("injected preparation failure");
        const auto source_index = refs[ordinal].instance_index;
        const auto stable_id = lease->stable_ids.at(source_index);
        append({true, stable_id, stable_id == 0 ? source_index + 1 : 0u},
            Values(static_cast<float>(frame * 10 + source_index * 100)), ordinal, true);
    }
    result.static_hashes = history.hashes - before_static;
    PruneShadowCasterStorage(result.current, epoch, result.order.size());
    if (cached)
    {
        for (const auto &entry : result.current_nodes.entries)
            Check(entry.node && entry.node->prepared_epoch == epoch, "prune must retain every cached static address");
        result.current_nodes.valid = true;
        result.prepared_previous_nodes.valid = !result.previous_warm;
    }
    return result;
}

void CommitNodes(NodeHistory &history, NodePrepared &prepared)
{
    hs::renderer_detail::CommitShadowCasterStorage(history.previous, history.scratch, prepared.current,
        history.previous_nodes, history.scratch_nodes, prepared.current_nodes,
        prepared.prepared_previous_nodes, prepared.previous_warm);
}

void TestStaticNodeCache()
{
    NodeHistory cached, reference;
    NodeLease scene = std::make_shared<NodeScene>(NodeScene{{17, 17, 0, 0, 29}});
    std::vector<NodeRef> refs{{0, 1}, {1, 1}, {2, 1}, {3, 1}};
    std::uint32_t generation = 7;
    int frame{};
    const auto compare = [&](std::initializer_list<Input> dynamic, bool new_session = false) {
        auto actual = PrepareNodes(cached, scene, generation, refs, dynamic, frame, true, new_session);
        auto expected = PrepareNodes(reference, scene, generation, refs, dynamic, frame++, false, new_session);
        Check(actual.order == expected.order && actual.current.size() == expected.current.size(),
            "node cache must preserve keys, first winner, all 14 current/previous fields and output order");
        for (const auto &[key, record] : expected.current)
            Check(actual.current.at(key).caster == record.caster, "node cache must reconstruct every payload field");
        const auto static_hashes = actual.static_hashes;
        CommitNodes(cached, actual); CommitNodes(reference, expected);
        return static_hashes;
    };
    (void)compare({}); (void)compare({});
    Check(compare({}) == 0, "both warm static maps must bypass hashing, including duplicate and anonymous keys");
    Check(cached.previous_nodes.entries[0].node == cached.previous_nodes.entries[1].node &&
        cached.previous_nodes.entries[2].node != cached.previous_nodes.entries[3].node,
        "named duplicates must alias; anonymous sources must retain canonical source identity");
    cached.previous.reserve(1024); cached.scratch.reserve(1024);
    Check(compare({}) == 0, "rehashing either map must preserve cached node addresses");
    (void)compare({{17, Values(500)}}); (void)compare({{17, Values(600)}});
    (void)compare({});
    Check(compare({}) == 0, "pruning dynamic keys must preserve the distinct static key domain");
    Check(compare({}, true) == 0, "new session must suppress previous payloads without changing node identity");

    std::swap(refs[0], refs[3]); // Equal count is insufficient: ordered source mapping changed.
    Check(!cached.previous_nodes.Matches(scene, generation, refs), "ordered retention changes must invalidate the cache");
    (void)compare({}); (void)compare({});
    refs[1].instance_generation = 2;
    Check(!cached.previous_nodes.Matches(scene, generation, refs), "reference generation changes must invalidate the cache");
    (void)compare({});
    refs = {{4, 1}, {2, 1}};
    (void)compare({}); (void)compare({});
    Check(compare({}) == 0, "static removal/replacement must rebuild both maps before warming");
    scene = std::make_shared<NodeScene>(NodeScene{{17, 17, 0, 0, 29}});
    Check(!cached.previous_nodes.Matches(scene, generation, refs) &&
        !cached.previous_nodes.Matches(cached.previous_nodes.lease, generation + 1, refs),
        "strong source lease and nonwrapping scene generation must both match");
    (void)compare({}); (void)compare({});
    ++generation;
    (void)compare({}); (void)compare({});
    const auto previous_snapshot = cached.previous;
    const auto *submitted_node = cached.previous_nodes.entries.front().node;
    bool failed{};
    try { (void)PrepareNodes(cached, scene, generation, refs, {}, frame, true, false, 1); }
    catch (const std::runtime_error &) { failed = true; }
    Check(failed && !cached.scratch_nodes.valid && cached.scratch_nodes.entries.empty() &&
        cached.previous_nodes.Matches(scene, generation, refs) &&
        cached.previous_nodes.entries.front().node == submitted_node,
        "failed attempt destruction must invalidate scratch addresses while retaining submitted history");
    for (const auto &[key, record] : previous_snapshot)
        Check(cached.previous.at(key).caster == record.caster, "failed preparation must not publish any payload field");
    (void)compare({}); (void)compare({});
    {
        auto unsubmitted = PrepareNodes(cached, scene, generation, refs, {}, frame, true);
        auto unsubmitted_reference = PrepareNodes(reference, scene, generation, refs, {}, frame, false);
        Check(unsubmitted.order == unsubmitted_reference.order && !cached.scratch_nodes.valid,
            "successful preparation without submission must still own its scratch addresses locally");
    }
    (void)compare({});
    cached.epoch = reference.epoch = std::numeric_limits<std::uint64_t>::max();
    Check(compare({}) > 0 && cached.epoch == 1, "epoch wrap must invalidate addresses before clearing scratch nodes");
    (void)compare({});

    // ExchangeView swaps previous map/cache together; scratch and epoch stay owned by their view.
    NodeHistory preview, preview_reference;
    auto p = PrepareNodes(preview, scene, generation, refs, {}, 900, true);
    auto pr = PrepareNodes(preview_reference, scene, generation, refs, {}, 900, false);
    CommitNodes(preview, p); CommitNodes(preview_reference, pr);
    std::swap(cached.previous, preview.previous);
    std::swap(cached.previous_nodes, preview.previous_nodes);
    std::swap(reference.previous, preview_reference.previous);
    (void)compare({});
    std::swap(cached.previous, preview.previous);
    std::swap(cached.previous_nodes, preview.previous_nodes);
    std::swap(reference.previous, preview_reference.previous);
    (void)compare({});
    refs.clear(); (void)compare({});
    cached.previous.clear(); cached.previous_nodes.Invalidate();
    reference.previous.clear();
    Check(!cached.previous_nodes.valid && !cached.previous_nodes.lease,
        "shutdown must release the strong lease and every cached address");
    refs = {{0, 1}, {2, 1}};
    (void)compare({});
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
        TestStaticNodeCache();
        std::cout << "caster storage: reuse, duplicates, replacement, removal, retry, wrap, views and node cache passed\n";
        return 0;
    }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
