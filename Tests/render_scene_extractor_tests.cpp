#include "renderer_scene_sample.hpp"
#include "visibility_encoding.hpp"
#include <hs/presentation/snapshot_exchange.hpp>
#include <hs/render_data/render_snapshot.hpp>
#include <array>
#include <atomic>
#include <thread>
#include <memory>
#include <utility>
#include <bit>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace
{
void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
void TestSnapshotExchange()
{
    hs::RenderSnapshotExchange exchange(4, 1, 1, 1);
    hs::RenderSnapshotExchange::Consumer consumer(exchange);
    hs::RenderSnapshotStorage previous(4, 1, 1, 1);

    for (hs::Sequence sequence = 1; sequence <= 3; ++sequence)
    {
        auto slot = exchange.TryBeginWrite();
        Check(slot.has_value(), "snapshot write slot");
        slot->Storage().header.tick = sequence;
        slot->Storage().AddInstance(
            {{static_cast<float>(sequence), 0, 0}, 0, {}, 0xFFFFFFFFu, hs::RenderMesh::Enemy});
        slot->Storage().AddInstance(
            {{static_cast<float>(sequence), 0, 1}, 0, {}, 0xFFFFFFFFu, hs::RenderMesh::Pickup});
        Check(slot->Storage().View().instances.size() == 2,
              "snapshot grows beyond its initial reserve");
        const auto previous_view = previous.View();
        std::move(*slot).Publish( sequence, sequence > 1 ? &previous_view : nullptr);
        const auto pair = consumer.AcquireLatest();
        Check(pair.has_current && pair.current.header.tick == sequence &&
                  pair.current_publication_sequence == sequence,
              "snapshot newest publication identity");
        if (sequence > 1)
        {
            Check(pair.has_previous && pair.previous.header.tick == sequence - 1 &&
                      pair.previous_publication_sequence == 0,
                  "snapshot owns the preceding actual sample, not a publication");
        }
        previous.CopyDynamicFrom(pair.current);
    }
    Check(exchange.DroppedPublishes() == 0, "snapshot no drops");
}

void TestRetainedSnapshotExchange()
{
    hs::RenderSnapshotExchange exchange(4, 4, 4, 4);
    hs::RenderSnapshotExchange::Consumer consumer(exchange);
    hs::RenderSnapshotStorage current_source(2, 2, 2, 2);
    current_source.header.session_id = 9;
    current_source.header.tick = 12;
    current_source.header.dynamic_pair_revision = 40;
    hs::RenderInstance instance{};
    instance.stable_id = 90;
    instance.position.x = 10;
    current_source.AddInstance(instance);
    hs::AnimationPoseRef pose{};
    pose.revision = 41;
    current_source.AddPose(pose);
    current_source.AddCapturedInstance({});
    current_source.AddLight({});
    current_source.AddPersistentVfx({});

    hs::RenderSnapshotStorage previous_source(2, 2, 2, 2);
    previous_source.header.session_id = 9;
    previous_source.header.tick = 11;
    previous_source.header.dynamic_pair_revision = 39;
    instance.position.x = 9;
    previous_source.AddInstance(instance);
    previous_source.AddPose(pose);
    previous_source.AddCapturedInstance({});
    previous_source.AddLight({});
    previous_source.AddPersistentVfx({});
    const auto previous_sample = previous_source.View();
    std::array<const hs::RenderInstance *, 3> current_arrays{};
    std::array<const hs::RenderInstance *, 3> previous_arrays{};
    std::array<const hs::RenderSnapshotStorage *, 3> slot_storages{};
    std::array<bool, 3> slot_seen{};

    for (hs::Sequence publication = 1; publication <= 4; ++publication)
    {
        current_source.header.checksum = publication;
        current_source.camera.target.x = static_cast<float>(publication);
        auto scene = std::make_shared<hs::StaticRenderScene>();
        scene->content_generation_id = publication % 2 ? "first" : "second";
        current_source.SetStaticScene(scene);
        const auto current_sample = current_source.View();
        auto slot = exchange.TryBeginRetainedWrite();
        Check(slot.has_value(), "retained snapshot writer slot");
        const auto *storage = &slot->Storage();
        std::size_t slot_index = slot_storages.size();
        for (std::size_t candidate = 0; candidate < slot_storages.size(); ++candidate)
        {
            if (slot_storages[candidate] == storage)
            {
                slot_index = candidate;
                break;
            }
            if (!slot_storages[candidate] && slot_index == slot_storages.size())
                slot_index = candidate;
        }
        Check(slot_index < slot_storages.size(), "retained writer returned known storage");
        slot_storages[slot_index] = storage;
        const bool copied = slot->Storage().CopyRetainedDynamicFrom(current_sample);
        const bool first_use = !slot_seen[slot_index];
        Check(copied == first_use,
            "retained slot must copy once, then reuse the same immutable dynamic sample");
        slot->Storage().AddUi({});
        std::move(*slot).Publish( publication, &previous_sample);
        const auto pair = consumer.AcquireLatest();
        Check(pair.has_current && pair.has_previous && pair.current.header.tick == 12 &&
            pair.previous.header.tick == 11 && pair.current.header.checksum == publication &&
            pair.current.camera.target.x == static_cast<float>(publication) &&
            pair.current.static_scene == scene && pair.current.ui.size() == 1 &&
            pair.current.instances.size() == 1 && pair.current.instances[0].position.x == 10 &&
            pair.previous.instances.size() == 1 && pair.previous.instances[0].position.x == 9,
            "retained publication must refresh current envelope/UI and preserve the previous sample pair");
        if (first_use)
        {
            current_arrays[slot_index] = pair.current.instances.data();
            previous_arrays[slot_index] = pair.previous.instances.data();
            slot_seen[slot_index] = true;
        }
        else
            Check(pair.current.instances.data() == current_arrays[slot_index] &&
                pair.previous.instances.data() == previous_arrays[slot_index],
                "reused retained slot must keep both current and previous array backing");
    }
    Check(exchange.DroppedPublishes() == 0, "retained snapshot publication must not drop");

    hs::RenderSnapshotExchange raw_exchange(2, 0, 0, 0);
    for (hs::Sequence publication = 1; publication <= 3; ++publication)
    {
        auto raw_slot = raw_exchange.TryBeginWrite();
        Check(raw_slot.has_value(), "raw snapshot writer slot");
        raw_slot->Storage().AddInstance({});
        std::move(*raw_slot).Publish( publication);
    }
    auto raw_slot = raw_exchange.TryBeginWrite();
    Check(raw_slot.has_value() && raw_slot->Storage().View().instances.empty(),
        "ordinary TryBeginWrite must continue clearing reused storage");
    std::move(*raw_slot).Abandon();
}

void TestSnapshotExchangeQueuedPublications()
{
    hs::RenderSnapshotExchange exchange(1, 0, 0, 0);
    hs::RenderSnapshotExchange::Consumer consumer(exchange);
    const auto publish = [&](hs::Sequence sequence, hs::Tick tick) {
        auto slot = exchange.TryBeginWrite();
        Check(slot.has_value(), "queued snapshot write slot");
        slot->Storage().header.tick = tick;
        hs::RenderSnapshot previous;
        previous.header.tick = tick > 0 ? tick - 1 : 0;
        std::move(*slot).Publish( sequence, tick > 0 ? &previous : nullptr);
    };
    for (hs::Sequence sequence = 1; sequence <= 3; ++sequence) publish(sequence, sequence);
    for (int read = 0; read < 4; ++read)
    {
        const auto pair = consumer.AcquireLatest();
        Check(pair.has_current && pair.current.header.tick == 3 && pair.has_previous &&
                  pair.previous.header.tick == 2,
              "queued snapshots must not reverse or replace the retained pair");
    }
    publish(4, 0); // A new session can restart ticks without restarting publication order.
    auto pair = consumer.AcquireLatest();
    Check(pair.current.header.tick == 0 && !pair.has_previous,
          "tick restart does not interpolate from an earlier publication");
    Check(pair.current_publication_sequence == 4 && pair.previous_publication_sequence == 0,
          "only the current packet has a publication identity");
    for (const hs::Sequence stale : {hs::Sequence{4}, hs::Sequence{2}})
    {
        publish(stale, 99);
        pair = consumer.AcquireLatest();
        Check(pair.current.header.tick == 0 && !pair.has_previous,
              "equal and stale publications preserve the retained packet");
    }
    {
        auto abandoned = exchange.TryBeginWrite();
        Check(abandoned.has_value(), "abandoned snapshot slot");
        abandoned->Storage().header.tick = 99;
    }
    publish(5, 1);
    pair = consumer.AcquireLatest();
    Check(pair.current.header.tick == 1 && pair.previous.header.tick == 0,
          "abandon and stale rejection release their slots");

    hs::RenderSnapshotExchange overwritten(0, 0, 0, 0);
    hs::RenderSnapshotExchange::Consumer overwrite_consumer(overwritten);
    const auto maximum = std::numeric_limits<hs::Sequence>::max();
    for (hs::Sequence sequence = maximum - 3;; ++sequence)
    {
        auto slot = overwritten.TryBeginWrite();
        Check(slot.has_value(), "oldest ready snapshot can be overwritten");
        slot->Storage().header.tick = sequence;
        std::move(*slot).Publish( sequence);
        if (sequence == maximum) break;
    }
    for (int read = 0; read < 4; ++read)
        Check(overwrite_consumer.AcquireLatest().current.header.tick == maximum,
              "maximum publication sequence is retained without wrapping");

    hs::RenderSnapshotExchange maximum_ready(0, 0, 0, 0);
    hs::RenderSnapshotExchange::Consumer maximum_consumer(maximum_ready);
    for (hs::Sequence sequence = maximum - 2;; ++sequence)
    {
        auto slot = maximum_ready.TryBeginWrite();
        Check(slot.has_value(), "maximum ready sequence fixture");
        std::move(*slot).Publish( sequence);
        if (sequence == maximum) break;
        (void)maximum_consumer.AcquireLatest();
    }
    auto replace_maximum = maximum_ready.TryBeginWrite();
    Check(replace_maximum.has_value(), "UINT64_MAX ready slot is still an overwrite candidate");
    std::move(*replace_maximum).Abandon();
}

void TestSnapshotExchangeConcurrentOwnership()
{
    hs::RenderSnapshotExchange exchange(1, 0, 0, 0);
    hs::RenderSnapshotExchange::Consumer consumer(exchange);
    constexpr hs::Sequence publication_count = 20'000;
    std::atomic<bool> finished{};
    std::jthread producer([&](std::stop_token stop) {
        hs::RenderSnapshotStorage previous(1, 0, 0, 0);
        hs::RenderSnapshotStorage next(1, 0, 0, 0);
        for (hs::Sequence sequence = 1; sequence <= publication_count; ++sequence)
        {
            auto slot = exchange.TryBeginWrite();
            while (!slot && !stop.stop_requested())
            {
                std::this_thread::yield();
                slot = exchange.TryBeginWrite();
            }
            if (!slot) return;
            slot->Storage().header.tick = sequence;
            slot->Storage().AddInstance({{static_cast<float>(sequence), 0, 0}, 0, {},
                                        0xFFFFFFFFu, hs::RenderMesh::Enemy});
            const auto previous_view = previous.View();
            // Copy the new producer state before publishing can release its slot.
            next.CopyDynamicFrom(slot->Storage().View());
            std::move(*slot).Publish( sequence, sequence > 1 ? &previous_view : nullptr);
            previous.CopyDynamicFrom(next.View());
            if ((sequence & 7u) == 0) std::this_thread::yield();
        }
        finished.store(true, std::memory_order_release);
    });
    hs::Tick current_tick{};
    const auto inspect = [&] {
        const auto pair = consumer.AcquireLatest();
        if (!pair.has_current) return;
        Check(pair.current.header.tick >= current_tick, "concurrent snapshot order is monotonic");
        current_tick = pair.current.header.tick;
        const auto previous_tick = pair.has_previous ? pair.previous.header.tick : 0;
        std::this_thread::yield(); // The producer must not rewrite either returned span.
        Check(pair.current.header.tick == current_tick && pair.current.instances.size() == 1 &&
                  pair.current.instances[0].position.x == static_cast<float>(current_tick),
              "producer preserves current snapshot storage");
        if (pair.has_previous)
            Check(pair.previous.header.tick == previous_tick && previous_tick + 1 == current_tick &&
                      pair.previous.instances.size() == 1 &&
                      pair.previous.instances[0].position.x == static_cast<float>(previous_tick),
                  "producer preserves previous snapshot storage");
    };
    do { inspect(); } while (!finished.load(std::memory_order_acquire));
    producer.join();
    for (int read = 0; read < 4; ++read) inspect();
    Check(current_tick == publication_count,
          "concurrent snapshot consumer receives the final publication");
}

void TestStaticSceneViewLifetime()
{
    auto storage = std::make_unique<hs::StaticRenderSceneStorage>();
    storage->content_generation_id = "first";
    storage->instances.resize(2);
    storage->instances[0].stable_id = 9;
    storage->instances[1].stable_id = 3;
    storage->eligible_static_members.push_back({9, 7, 0});
    const auto *instances = storage->instances.data();
    const auto first = hs::FreezeStaticRenderScene(std::move(storage));
    Check(!storage && first->instances.data() == instances && first->instances.size() == 2 &&
          first->eligible_static_members.front().stable_id == 9 &&
          first->static_keys[0].stable_id == 3 && first->static_keys[1].stable_id == 9,
          "frozen static view must borrow its stable backing and final prepared keys");
    auto replacement = std::make_unique<hs::StaticRenderSceneStorage>(*first);
    replacement->content_generation_id = "second";
    replacement->instances[0].position.x = 42;
    replacement->instances[0].stable_id = 10;
    replacement->eligible_static_members[0].stable_id = 10;
    const auto second = hs::FreezeStaticRenderScene(std::move(replacement));
    Check(second->instances.data() != first->instances.data() && first->instances[0].position.x == 0 &&
          second->instances[0].position.x == 42 && second->static_keys.back().stable_id == 10 &&
          first->content_generation_id == "first" && second->content_generation_id == "second",
          "development scene replacement must own new storage without rebinding an old leased view");
}

void CheckSnapshotStorageInputs()
{
    hs::RenderSnapshotStorage packet(4, 4, 0, 0);
    hs::RenderInstance instance;
    instance.mesh = hs::RenderMesh::Archer;
    instance.revisions = {11, 22, 33, 44};
    packet.AddInstance(instance);
    Check(packet.View().instances[0].asset_id.value == 0 &&
              packet.View().instances[0].revisions == instance.revisions,
          "snapshot storage preserves instance inputs without deriving identity");
    hs::AnimationPoseRef pose;
    pose.revision = 77;
    packet.AddPose(pose);
    pose.revision = 88;
    packet.AddPose(pose);
    Check(packet.View().poses.size() == 2 &&
              packet.View().instances[0].revisions == instance.revisions &&
              packet.View().poses.back().revision == 88 &&
              hs::FindLastRenderPose(packet.View().poses, 0) == &packet.View().poses.back(),
          "snapshot storage preserves pose inputs without deriving instance revisions");
    pose.instance_index = 3;
    packet.AddPose(pose);
    Check(packet.View().poses.size() == 3,
          "development unassociated pose append remains accepted");
    instance.animation_clip_id.value = 123;
    instance.animation_time_seconds = 0.5;
    packet.Clear();
    packet.AddInstance(instance);
    pose.instance_index = 0;
    packet.AddPose(pose);
    Check(packet.View().instances[0].revisions.pose == instance.revisions.pose &&
              packet.View().poses.back().revision == 88,
          "snapshot storage leaves final animation revisions to its display owner");
    hs::CapturedRenderInstance capture;
    capture.instance = instance;
    capture.animation_pose = pose;
    packet.AddCapturedInstance(capture);
    Check(packet.View().captured_instances[0].instance.revisions == capture.instance.revisions &&
              packet.View().captured_instances[0].animation_pose.revision == capture.animation_pose.revision,
          "snapshot storage preserves captured inputs without deriving revisions");
    packet.Clear();
    instance.revisions = {};
    instance.animation_clip_id = {};
    pose.revision = 0;
    packet.AddInstance(instance);
    packet.AddPose(pose);
    packet.AddCapturedInstance({});
    Check(packet.View().instances[0].revisions == hs::RenderInstance::Revisions{} &&
              packet.View().poses[0].revision == 0 &&
              packet.View().captured_instances[0].instance.revisions == hs::RenderInstance::Revisions{},
          "complete producer append cannot calculate dynamic revision hashes");
    packet.Clear();
    Check(packet.View().instances.empty() && packet.View().poses.empty() &&
              packet.View().captured_instances.empty(), "packet reuse clears all logical operands");
}

void CheckRetainedSnapshotCopy()
{
    hs::RenderSnapshotStorage source(4, 4, 4, 4);
    source.header.session_id = 7;
    source.header.tick = 12;
    source.header.dynamic_pair_revision = 31;
    source.header.checksum = 99;
    source.camera.target = {1, 2, 3};
    hs::RenderInstance instance{};
    instance.stable_id = 101;
    instance.position.x = 4;
    source.AddInstance(instance);
    hs::AnimationPoseRef pose{};
    pose.revision = 22;
    source.AddPose(pose);
    hs::CapturedRenderInstance captured{};
    captured.sequence = 33;
    source.AddCapturedInstance(captured);
    source.AddLight({});
    source.AddPersistentVfx({});
    source.AddUi({});
    auto first_scene = std::make_shared<hs::StaticRenderScene>();
    first_scene->content_generation_id = "first";
    source.SetStaticScene(first_scene);

    auto sample = source.View();
    hs::RenderSnapshotStorage retained(0, 0, 0, 0);
    retained.AddUi({});
    Check(retained.CopyRetainedDynamicFrom(sample),
        "first identified retained copy must copy dynamic arrays");
    auto view = retained.View();
    Check(view.instances.size() == 1 && view.instances[0].stable_id == 101 &&
        view.poses.size() == 1 && view.poses[0].revision == 22 &&
        view.captured_instances.size() == 1 && view.captured_instances[0].sequence == 33 &&
        view.lights.size() == 1 && view.persistent_vfx.size() == 1 && view.ui.empty(),
        "retained copy must transfer all dynamic arrays and clear UI");
    const auto *instances = view.instances.data();
    const auto *poses = view.poses.data();
    const auto *captures = view.captured_instances.data();
    const auto *lights = view.lights.data();
    const auto *persistent = view.persistent_vfx.data();

    source.header.checksum = 123;
    source.camera.target = {8, 9, 10};
    auto second_scene = std::make_shared<hs::StaticRenderScene>();
    second_scene->content_generation_id = "second";
    source.SetStaticScene(second_scene);
    source.AddUi({});
    sample = source.View();
    retained.AddUi({});
    Check(!retained.CopyRetainedDynamicFrom(sample),
        "same identified immutable arrays must be reused");
    view = retained.View();
    Check(view.instances.data() == instances && view.poses.data() == poses &&
        view.captured_instances.data() == captures && view.lights.data() == lights &&
        view.persistent_vfx.data() == persistent && view.header.checksum == 123 &&
        view.camera.target.x == 8 && view.static_scene == second_scene && view.ui.empty(),
        "retained reuse must still refresh envelope/scene and clear UI");

    auto copied = retained;
    Check(!copied.CopyRetainedDynamicFrom(sample),
        "default snapshot-storage copy must transfer its retained key with the arrays");
    auto moved = std::move(copied);
    Check(!moved.CopyRetainedDynamicFrom(sample),
        "default snapshot-storage move must transfer its retained key with the arrays");
    Check(copied.CopyRetainedDynamicFrom(sample) && copied.View().instances.size() == 1 &&
        copied.View().instances[0].stable_id == 101 &&
        !copied.CopyRetainedDynamicFrom(sample),
        "moved-from storage must invalidate its retained key and restore moved arrays");

    hs::RenderSnapshotStorage move_assigned(0, 0, 0, 0);
    move_assigned = std::move(moved);
    Check(!move_assigned.CopyRetainedDynamicFrom(sample) && moved.CopyRetainedDynamicFrom(sample) &&
        moved.View().instances.size() == 1,
        "move assignment must transfer the key and invalidate the source");
    move_assigned = std::move(move_assigned);
    Check(move_assigned.CopyRetainedDynamicFrom(sample) &&
        !move_assigned.CopyRetainedDynamicFrom(sample),
        "self-move must invalidate the retained key without losing dynamic contents");

    hs::RenderSnapshotStorage copy_assigned(0, 0, 0, 0);
    hs::RenderSnapshotStorage other_source(0, 0, 0, 0);
    other_source.header.session_id = 77;
    other_source.header.tick = 2;
    other_source.header.dynamic_pair_revision = 5;
    instance.stable_id = 707;
    other_source.AddInstance(instance);
    auto other_sample = other_source.View();
    Check(copy_assigned.CopyRetainedDynamicFrom(other_sample),
        "copy-assignment target must begin warm with a different key");
    copy_assigned.AddUi({});
    retained.AddUi({});
    copy_assigned = retained;
    const auto assigned_view = copy_assigned.View();
    const auto *assigned_instances = assigned_view.instances.data();
    Check(assigned_view.instances.size() == 1 && assigned_view.instances[0].stable_id == 101 &&
        assigned_view.ui.size() == 1 && retained.View().ui.size() == 1 &&
        !copy_assigned.CopyRetainedDynamicFrom(sample) &&
        copy_assigned.View().instances.data() == assigned_instances &&
        copy_assigned.View().ui.empty() && retained.View().ui.size() == 1,
        "copy assignment must transfer content/key, preserve its source, and keep warm reuse");

    const auto expect_copy = [&](const char *message) {
        Check(retained.CopyRetainedDynamicFrom(sample), message);
    };
    ++source.header.dynamic_pair_revision;
    sample = source.View();
    expect_copy("same tick with a new dynamic revision must copy");
    sample = source.View();
    Check(!retained.CopyRetainedDynamicFrom(sample), "new revision must become reusable");
    ++source.header.tick;
    sample = source.View();
    expect_copy("same session/revision with a new tick must copy");
    ++source.header.session_id;
    sample = source.View();
    expect_copy("new session must copy");

    source.header.session_id = 0;
    sample = source.View();
    expect_copy("zero session identity must copy");
    expect_copy("unknown identities must never enter the reuse cache");
    source.header.session_id = 8;
    source.header.tick = 0;
    sample = source.View();
    expect_copy("identified tick0 must copy on first use");
    Check(!retained.CopyRetainedDynamicFrom(sample), "identified tick0 must be reusable");
    source.header.tick = 14;
    source.header.dynamic_pair_revision = 0;
    sample = source.View();
    expect_copy("zero dynamic revision must copy");

    source.header.dynamic_pair_revision = 40;
    sample = source.View();
    expect_copy("identified sample must recache after unknown identity");
    const auto expect_mutation_invalidates = [&](auto &&mutate, const char *message) {
        Check(!retained.CopyRetainedDynamicFrom(sample), "fixture must begin with a warm retained key");
        mutate(retained);
        Check(retained.CopyRetainedDynamicFrom(sample), message);
        Check(!retained.CopyRetainedDynamicFrom(sample), "successful repair must restore a reusable key");
    };
    expect_mutation_invalidates([](auto &storage) { storage.AddInstance({}); },
        "AddInstance must invalidate retained array identity");
    expect_mutation_invalidates([](auto &storage) { storage.AddPose({}); },
        "AddPose must invalidate retained array identity");
    expect_mutation_invalidates([](auto &storage) { storage.AddCapturedInstance({}); },
        "AddCapturedInstance must invalidate retained array identity");
    expect_mutation_invalidates([](auto &storage) { storage.AddLight({}); },
        "AddLight must invalidate retained array identity");
    expect_mutation_invalidates([](auto &storage) { storage.AddPersistentVfx({}); },
        "AddPersistentVfx must invalidate retained array identity");
    expect_mutation_invalidates([](auto &storage) { storage.Clear(); },
        "Clear must invalidate retained array identity");

    retained.AddUi({});
    (void)retained.TakeUiModels();
    retained.SetStaticScene(first_scene);
    Check(!retained.CopyRetainedDynamicFrom(sample),
        "UI and static-scene operations must preserve dynamic retained identity");

    source.AddInstance({});
    sample = source.View(); // Same key with mutated rows is valid only for the raw-copy test.
    retained.CopyDynamicFrom(sample);
    Check(retained.View().instances.size() == 2 && retained.View().instances.back().stable_id == 0,
        "raw CopyDynamicFrom must keep its unconditional copy semantics");
    Check(retained.CopyRetainedDynamicFrom(sample),
        "raw CopyDynamicFrom must invalidate retained identity");
}

void CheckExactFinalPoseInputs()
{
    hs::RenderInstance instance;
    instance.mesh = hs::RenderMesh::Archer;
    instance.revisions.pose = 99;
    hs::AnimationPoseRef pose;
    pose.revision = 99;
    const auto original = hs::MakeRenderPoseKey(instance, &pose);
    const auto changed = [&](auto edit) {
        auto candidate = pose;
        edit(candidate);
        Check(hs::MakeRenderPoseKey(instance, &candidate) != original,
              "supplied revision concealed an actual final pose operand");
    };
    changed([](auto &p) { p.clip = hs::CharacterAnimationClip::Run; });
    changed([](auto &p) { p.normalized_time = 0.5f; });
    changed([](auto &p) { p.playback_rate = 2; });
    changed([](auto &p) { p.secondary_clip = hs::CharacterAnimationClip::Death; });
    changed([](auto &p) { p.secondary_normalized_time = 0.25f; });
    changed([](auto &p) { p.secondary_playback_rate = 2; });
    changed([](auto &p) { p.secondary_weight = 0.25f; });
    changed([](auto &p) { p.upper_body_clip = hs::CharacterAnimationClip::Run; });
    changed([](auto &p) { p.upper_body_normalized_time = 0.25f; });
    changed([](auto &p) { p.upper_body_playback_rate = 2; });
    changed([](auto &p) { p.upper_body_weight = 0.25f; });
    changed([](auto &p) { p.normalized_time = -0.0f; });
    instance.animation_clip_id.value = 123;
    instance.animation_time_seconds = 1;
    const auto named = hs::MakeRenderPoseKey(instance, &pose);
    instance.animation_time_seconds = std::bit_cast<double>(std::bit_cast<std::uint64_t>(1.0) + 1);
    Check(hs::MakeRenderPoseKey(instance, &pose) != named, "named animation key retains all double time bits");
    Check(hs::MakeRenderSceneInstanceKey(0, UINT32_MAX - 1).fallback_ordinal == UINT32_MAX &&
              hs::MakeRenderSceneInstanceKey(42, 5) == hs::RenderSceneInstanceKey{42, 0},
          "anonymous ordinals and stable identities preserve their original contract");
    for (float radius : {0.0f, 0.035f, 1.0f, 128.5f})
        Check(std::bit_cast<std::uint32_t>(hs::EncodeGrassDeformationRadius(radius, hs::OcclusionMode::Disabled)) ==
                  std::bit_cast<std::uint32_t>(-radius), "occlusion constant retains signed-zero bits");
}
void CheckInstantMotionSample()
{
    hs::RenderInstance previous{};
    previous.mesh = hs::RenderMesh::Archer;
    previous.stable_id = 1;
    auto current = previous;
    current.position.x = 10;
    current.motion_epoch = 9;
    std::array old_instances{previous}, new_instances{current};
    hs::RenderSnapshotPair pair;
    pair.has_previous = pair.has_current = true;
    pair.previous.instances = old_instances;
    pair.current.instances = new_instances;
    hs::RenderSceneSample sample;
    sample.Prepare(pair, 0.5f, [](const auto &, auto) { return false; });
    Check(sample.View(pair.current).instances[0].position.x == 0,
          "instant movement retains the prior display sample before its endpoint");
    sample.Prepare(pair, 1.0f, [](const auto &, auto) { return false; });
    Check(sample.View(pair.current).instances[0].position.x == 10,
          "instant movement selects the current display sample at its endpoint");
    new_instances[0].motion_epoch = previous.motion_epoch;
    sample.Prepare(pair, 0.5f, [](const auto &, auto) { return false; });
    Check(sample.View(pair.current).instances[0].position.x == 5,
          "continuous movement still interpolates between samples");
    Check(hs::MakeRenderContentKey(previous) == hs::MakeRenderContentKey(current),
          "movement epoch cannot rebind character content");
}
}
int main()
{
    try
    {
        TestSnapshotExchange();
        TestRetainedSnapshotExchange();
        TestSnapshotExchangeQueuedPublications();
        TestSnapshotExchangeConcurrentOwnership();
        TestStaticSceneViewLifetime();
        CheckSnapshotStorageInputs();
        CheckRetainedSnapshotCopy();
        CheckExactFinalPoseInputs();
        CheckInstantMotionSample();
        hs::RenderSnapshotPair camera_pair;
        camera_pair.has_previous = camera_pair.has_current = true;
        camera_pair.previous.camera.target = {0, 2, 4};
        camera_pair.current.camera.target = {10, 5, 8};
        camera_pair.current.camera.target_height_offset = 1;
        camera_pair.current.camera.distance = 12;
        const auto camera = hs::InterpolateRenderCamera(camera_pair, 0.5f);
        Check(camera.target.x == 5 && camera.target.y == 4 && camera.target.z == 6 && camera.distance == 12,
              "camera follow must share display time while zoom settings remain current");
        camera_pair.current.camera.target_motion_epoch = 9;
        const auto jumped_camera = hs::InterpolateRenderCamera(camera_pair, 0.5f);
        Check(jumped_camera.target.x == 0 && jumped_camera.target.y == 3 &&
                  jumped_camera.target.z == 4 && jumped_camera.distance == 12,
              "camera follow waits for the instant movement endpoint while zoom stays current");
        std::puts("Render scene sample tests passed.");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
