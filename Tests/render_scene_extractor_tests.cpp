#include "renderer_scene_sample.hpp"
#include "visibility_encoding.hpp"
#include <hs/presentation/snapshot_exchange.hpp>
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
        slot->storage->header.tick = sequence;
        Check(slot->storage->AddInstance(
                  {{static_cast<float>(sequence), 0, 0}, 0, {}, 0xFFFFFFFFu,
                   hs::RenderMesh::Enemy}),
              "snapshot instance");
        Check(slot->storage->AddInstance(
                  {{static_cast<float>(sequence), 0, 1}, 0, {}, 0xFFFFFFFFu,
                   hs::RenderMesh::Pickup}),
              "snapshot grows beyond its initial reserve");
        const auto previous_view = previous.View();
        exchange.Publish(*slot, sequence, sequence > 1 ? &previous_view : nullptr);
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

void TestSnapshotExchangeQueuedPublications()
{
    hs::RenderSnapshotExchange exchange(1, 0, 0, 0);
    hs::RenderSnapshotExchange::Consumer consumer(exchange);
    const auto publish = [&](hs::Sequence sequence, hs::Tick tick) {
        auto slot = exchange.TryBeginWrite();
        Check(slot.has_value(), "queued snapshot write slot");
        slot->storage->header.tick = tick;
        hs::RenderSnapshot previous;
        previous.header.tick = tick > 0 ? tick - 1 : 0;
        exchange.Publish(*slot, sequence, tick > 0 ? &previous : nullptr);
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
    const auto abandoned = exchange.TryBeginWrite();
    Check(abandoned.has_value(), "abandoned snapshot slot");
    abandoned->storage->header.tick = 99;
    exchange.Abandon(*abandoned);
    publish(5, 1);
    pair = consumer.AcquireLatest();
    Check(pair.current.header.tick == 1 && pair.previous.header.tick == 0,
          "abandon and stale rejection release their slots");

    hs::RenderSnapshotExchange overwritten(0, 0, 0, 0);
    hs::RenderSnapshotExchange::Consumer overwrite_consumer(overwritten);
    const auto maximum = std::numeric_limits<hs::Sequence>::max();
    for (hs::Sequence sequence = maximum - 3;; ++sequence)
    {
        const auto slot = overwritten.TryBeginWrite();
        Check(slot.has_value(), "oldest ready snapshot can be overwritten");
        slot->storage->header.tick = sequence;
        overwritten.Publish(*slot, sequence);
        if (sequence == maximum) break;
    }
    for (int read = 0; read < 4; ++read)
        Check(overwrite_consumer.AcquireLatest().current.header.tick == maximum,
              "maximum publication sequence is retained without wrapping");

    hs::RenderSnapshotExchange maximum_ready(0, 0, 0, 0);
    hs::RenderSnapshotExchange::Consumer maximum_consumer(maximum_ready);
    for (hs::Sequence sequence = maximum - 2;; ++sequence)
    {
        const auto slot = maximum_ready.TryBeginWrite();
        Check(slot.has_value(), "maximum ready sequence fixture");
        maximum_ready.Publish(*slot, sequence);
        if (sequence == maximum) break;
        (void)maximum_consumer.AcquireLatest();
    }
    const auto replace_maximum = maximum_ready.TryBeginWrite();
    Check(replace_maximum.has_value(), "UINT64_MAX ready slot is still an overwrite candidate");
    maximum_ready.Abandon(*replace_maximum);
}

void TestSnapshotExchangeConcurrentOwnership()
{
    hs::RenderSnapshotExchange exchange(1, 0, 0, 0);
    hs::RenderSnapshotExchange::Consumer consumer(exchange);
    constexpr hs::Sequence publication_count = 20'000;
    std::atomic<bool> finished{}, producer_failed{};
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
            slot->storage->header.tick = sequence;
            if (!slot->storage->AddInstance({{static_cast<float>(sequence), 0, 0}, 0, {},
                                             0xFFFFFFFFu, hs::RenderMesh::Enemy}))
                producer_failed.store(true, std::memory_order_relaxed);
            const auto previous_view = previous.View();
            // Copy the new producer state before publishing can release its slot.
            next.CopyDynamicFrom(slot->storage->View());
            exchange.Publish(*slot, sequence, sequence > 1 ? &previous_view : nullptr);
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
    Check(!producer_failed.load() && current_tick == publication_count,
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

void CheckDevelopmentAdapter()
{
    hs::RenderSnapshotStorage packet(4, 4, 0, 0);
    hs::RenderInstance instance;
    instance.mesh = hs::RenderMesh::Archer;
    instance.revisions = {11, 22, 33, 44};
    Check(packet.AddInstance(instance), "development instance append");
    Check(packet.View().instances[0].asset_id.value != 0 &&
              packet.View().instances[0].revisions == instance.revisions,
          "development explicit revisions retain identity population");
    hs::AnimationPoseRef pose;
    pose.revision = 77;
    Check(packet.AddPose(pose), "development pose append");
    pose.revision = 88;
    Check(packet.AddPose(pose) && packet.View().poses.size() == 2 &&
              packet.View().instances[0].revisions.pose == 88 &&
              hs::FindLastRenderPose(packet.View().poses, 0) == &packet.View().poses.back(),
          "last development pose owns the supplied pose revision");
    pose.instance_index = 3;
    Check(packet.AddPose(pose) && packet.View().poses.size() == 3,
          "development unassociated pose append remains accepted");
    instance.animation_clip_id.value = 123;
    instance.animation_time_seconds = 0.5;
    packet.Clear();
    Check(packet.AddInstance(instance), "named development instance append");
    pose.instance_index = 0;
    Check(packet.AddPose(pose) && packet.View().instances[0].revisions.pose ==
              hs::DeriveNamedAnimationPoseRevision(instance),
          "named animation takes precedence over supplied legacy pose revision");
    hs::CapturedRenderInstance capture;
    capture.instance = instance;
    capture.animation_pose = pose;
    Check(packet.AddCapturedInstance(capture) && packet.View().captured_instances[0].instance.revisions.pose ==
              hs::DeriveAnimationPoseRevision(pose), "manual capture retains its legacy pose-revision rule");
    packet.Clear();
    instance.revisions = {};
    instance.animation_clip_id = {};
    pose.revision = 0;
    Check(packet.AppendInstance(instance) && packet.AppendPose(pose) &&
              packet.AppendCapturedInstance({}), "complete producer operands append");
    Check(packet.View().instances[0].revisions == hs::RenderInstance::Revisions{} &&
              packet.View().poses[0].revision == 0 &&
              packet.View().captured_instances[0].instance.revisions == hs::RenderInstance::Revisions{},
          "complete producer append cannot calculate dynamic revision hashes");
    packet.Clear();
    Check(packet.View().instances.empty() && packet.View().poses.empty() &&
              packet.View().captured_instances.empty(), "packet reuse clears all logical operands");
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
        TestSnapshotExchangeQueuedPublications();
        TestSnapshotExchangeConcurrentOwnership();
        TestStaticSceneViewLifetime();
        CheckDevelopmentAdapter();
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
