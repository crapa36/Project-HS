#include "persistent_gpu_scene.hpp"
#include "renderer_skin_ranges.hpp"
#include "renderer_geometry.hpp"
#include "renderer_monster_lod.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace
{
void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

hs::GpuInstance Instance(float x, std::uint32_t mesh = 1)
{
    return {{x, 2.0f, 3.0f, 1.0f},
            {1.0f, 1.0f, 1.0f, 1.0f},
            0xffffffffu, mesh, 0.0f, 0.0f};
}

void ChannelUploadsPreserveTables()
{
    using Channel = hs::GpuSceneChannel;
    hs::PersistentGpuSceneSlots scene;
    std::array inputs{Instance(1), Instance(2), Instance(3)};
    std::array<hs::GpuSceneMetadata, 3> metadata{};
    metadata[0].skin_flags = 1;
    std::array<hs::GpuSceneSerializedSlot, 3> gpu{};
    const auto prepare = [&] {
        scene.BeginFrame();
        for (std::size_t index = 0; index < inputs.size(); ++index)
            Check(!scene.Upsert({101 + index, 0}, inputs[index], metadata[index]).conflict,
                  "channel fixture retains stable slots");
        scene.EndFrame();
    };
    const auto upload = [&] {
        std::size_t bytes{};
        const auto copy = [&](Channel channel, auto member) {
            for (const auto &range : scene.DirtyRanges(channel))
                for (std::size_t index = range.begin; index < range.begin + range.count; ++index)
                {
                    const auto expected = hs::SerializeGpuSceneSlot(static_cast<std::uint32_t>(index),
                        scene.Instances()[index], scene.Metadata()[index]);
                    gpu[index].*member = expected.*member;
                    bytes += sizeof(expected.*member);
                }
        };
        copy(Channel::Instance, &hs::GpuSceneSerializedSlot::instance);
        copy(Channel::Transform, &hs::GpuSceneSerializedSlot::transform);
        copy(Channel::Mesh, &hs::GpuSceneSerializedSlot::mesh);
        copy(Channel::Material, &hs::GpuSceneSerializedSlot::material);
        copy(Channel::Bounds, &hs::GpuSceneSerializedSlot::bounds);
        copy(Channel::Skin, &hs::GpuSceneSerializedSlot::skin);
        for (std::size_t index = 0; index < gpu.size(); ++index)
        {
            const auto expected = hs::SerializeGpuSceneSlot(static_cast<std::uint32_t>(index),
                scene.Instances()[index], scene.Metadata()[index]);
            Check(std::memcmp(&gpu[index], &expected, sizeof(expected)) == 0,
                  "channel patches must equal a complete current table rebuild");
        }
        scene.Commit();
        return bytes;
    };
    prepare();
    Check(upload() == 3 * hs::kGpuSceneSlotPayloadBytes, "new slots initialize every GPU channel");

    inputs[0].position.x = 10;
    inputs[1].color = 0x12345678;
    inputs[2].position.w = 4;
    prepare();
    Check(scene.DirtyRanges(Channel::Instance).empty() && scene.DirtyRanges(Channel::Mesh).empty() &&
          scene.DirtyRanges(Channel::Skin).empty(), "movement/color/bounds do not rewrite stable identity or skin");
    Check(upload() == sizeof(hs::GpuSceneTransformEntry) + sizeof(hs::GpuSceneMaterialEntry) +
          sizeof(hs::GpuSceneBoundsEntry), "independent changed channels replace six-array uploads");

    metadata[0].previous_skin_vertex_stride = 12;
    metadata[1].asset_id = 42;
    metadata[2].reserved = 7;
    prepare();
    Check(scene.DirtyRanges(Channel::Transform).empty() && scene.DirtyRanges(Channel::Material).empty(),
          "view history and entity surface edits cannot rewrite transform/material tables");
    Check(upload() == sizeof(hs::GpuSceneSkinEntry) + sizeof(hs::GpuSceneMeshEntry) +
          sizeof(hs::GpuSceneInstanceEntry), "history stride, geometry and entity payload map to their actual channels");

    inputs[0].scale.w = 9;
    prepare();
    Check(scene.DirtyRanges().empty() && upload() == 0, "unserialized padding does not create GPU work");
    inputs[0].position.x = 20;
    prepare();
    scene.Abort();
    inputs[0].position.x = 10;
    prepare();
    Check(upload() == 0, "an aborted channel edit does not publish bytes or retain a dirty revision");

    prepare();
    scene.MarkAllActiveDirty();
    Check(upload() == 3 * hs::kGpuSceneSlotPayloadBytes,
          "fresh native backing initializes all channels even without semantic changes");
}

void SkinChannelTransactionsPreserveScene()
{
    hs::PersistentGpuSceneSlots scene;
    const auto instance = Instance(4.0f);
    hs::GpuSceneMetadata initial;
    initial.asset_id = 101;
    initial.material_id = 202;
    initial.skin_flags = 1;
    initial.skin_vertex_offset = 64;
    initial.previous_skin_vertex_offset = 32;
    hs::GpuSceneCanonicalInputs canonical;
    canonical.supplied_transform_revision = 3;
    canonical.supplied_material_revision = 5;

    scene.BeginFrame();
    const auto slot = scene.Upsert({303, 0}, instance, initial, canonical);
    scene.EndFrame();
    scene.Commit();
    const auto revision = scene.ContentRevision();
    const auto payload_revision = scene.PayloadRevision();
    const auto transform_revision = scene.Revisions(slot.index).transform;
    auto next_skin = initial;
    next_skin.previous_skin_vertex_offset = 96;
    const auto submitted_instance = scene.Instances()[slot.index];
    const auto submitted_metadata = scene.Metadata()[slot.index];
    const auto submitted_material_revision = scene.Revisions(slot.index).material;

    scene.BeginFrame();
    Check(!scene.Upsert({303, 0}, submitted_instance, next_skin, canonical).conflict,
        "canonical skin history update retains its stable scene slot");
    scene.EndFrame();
    Check(scene.DirtyRanges(hs::GpuSceneChannel::Transform).empty() &&
          scene.DirtyRanges(hs::GpuSceneChannel::Mesh).empty() &&
          scene.DirtyRanges(hs::GpuSceneChannel::Skin).size() == 1,
        "canonical skin-history edit updates only the skin channel");
    Check(scene.PayloadRevision() == payload_revision + 1,
        "prepared canonical skin edit advances the prospective GPU payload revision");
    scene.Abort();
    Check(scene.Metadata()[slot.index].previous_skin_vertex_offset == initial.previous_skin_vertex_offset &&
          scene.Metadata()[slot.index].asset_id == submitted_metadata.asset_id &&
          scene.Metadata()[slot.index].material_id == submitted_metadata.material_id &&
          scene.Metadata()[slot.index].reserved == submitted_metadata.reserved &&
          scene.Instances()[slot.index].position.x == submitted_instance.position.x &&
          scene.Instances()[slot.index].color == submitted_instance.color &&
          scene.Instances()[slot.index].mesh == submitted_instance.mesh &&
          scene.Instances()[slot.index].yaw == submitted_instance.yaw &&
          scene.ContentRevision() == revision &&
          scene.Revisions(slot.index).transform == transform_revision &&
          scene.Revisions(slot.index).material == submitted_material_revision && scene.ActiveCount() == 1,
        "aborted canonical skin edit leaves submitted membership and payload intact");
    Check(scene.PayloadRevision() == payload_revision,
        "aborted canonical skin edit does not publish its payload revision");

    scene.BeginFrame();
    Check(!scene.Upsert({303, 0}, submitted_instance, next_skin, canonical).conflict,
        "canonical skin update can be retried after abort");
    scene.EndFrame();
    scene.Commit();
    Check(scene.Metadata()[slot.index].previous_skin_vertex_offset == next_skin.previous_skin_vertex_offset &&
          scene.Metadata()[slot.index].asset_id == submitted_metadata.asset_id &&
          scene.Metadata()[slot.index].material_id == submitted_metadata.material_id &&
          scene.Metadata()[slot.index].reserved == submitted_metadata.reserved &&
          scene.Instances()[slot.index].position.x == submitted_instance.position.x &&
          scene.Instances()[slot.index].color == submitted_instance.color &&
          scene.Instances()[slot.index].mesh == submitted_instance.mesh &&
          scene.Instances()[slot.index].yaw == submitted_instance.yaw &&
          scene.ContentRevision() == revision &&
          scene.Revisions(slot.index).transform == transform_revision &&
          scene.Revisions(slot.index).material == submitted_material_revision && scene.ActiveCount() == 1,
        "committed skin history preserves current-scene revisions and active slot identity");
    Check(scene.PayloadRevision() == payload_revision + 1,
        "committed canonical skin edit publishes its GPU payload revision");
}

void PacketSkinConsumerCoverageIsPreparedForEveryView()
{
    std::array<hs::RenderViewInput, 2> views{};
    for (std::size_t i = 0; i < views.size(); ++i)
    {
        auto &view = views[i];
        view.id = {static_cast<std::uint32_t>(i + 1)};
        view.camera.position = {0.0, 0.0, -5.0};
        view.camera.target = {0.0, 0.0, 0.0};
        view.output_extent = {1280, 720};
        view.render_active = {1280, 720};
    }
    hs::LightView local_shadow;
    local_shadow.type = hs::LightType::Point;
    local_shadow.position = {0.0f, 0.0f, 0.0f};
    local_shadow.range = 20.0f;
    local_shadow.emitter_radius = 0.1f;
    local_shadow.shadow_flags = hs::kLightShadowRequested;
    const std::array lights{local_shadow};
    hs::MonsterLodProjectionInputs inputs;
    inputs.camera_views = views;
    inputs.local_lights = lights;
    inputs.csm_world_texels = {0.1, 0.5, 2.0};
    inputs.vsm_world_texels = {0.05, 0.2, 0.8};
    inputs.maximum_error_pixels = 1.0;
    inputs.local_shadow_face_resolution = 512;
    inputs.csm_active = inputs.vsm_active = inputs.local_shadows_active = true;
    inputs.camera_views_complete = inputs.local_lights_complete = true;
    const auto prepared = hs::PrepareMonsterLodConsumers(inputs);
    Check(prepared.baseline.valid && prepared.cameras.size() == views.size() &&
              prepared.local_lights.size() == lights.size(),
          "packet skin demand retains every camera and requested local-shadow consumer");
}

void PayloadRevisionRangesTrackRotatingUploads()
{
    using Channel = hs::GpuSceneChannel;
    hs::PersistentGpuSceneSlots scene;
    std::vector<hs::PersistentGpuSceneRange> ranges;
    std::array<std::vector<hs::PersistentGpuSceneRange>, hs::kGpuSceneChannelCount> channel_ranges;
    const auto collect = [&](std::uint64_t uploaded_revision) {
        scene.CollectDirtyRangesSince(uploaded_revision, ranges, channel_ranges);
    };

    scene.BeginFrame();
    const auto a = scene.Upsert({11, 0}, Instance(1));
    const auto b = scene.Upsert({22, 0}, Instance(2));
    const auto c = scene.Upsert({33, 0}, Instance(3));
    scene.EndFrame();
    const auto initial_revision = scene.PayloadRevision();
    collect(std::uint64_t{0});
    Check(ranges.size() == 1 && ranges[0].begin == 0 && ranges[0].count == 3,
          "revision zero uploads every active row in one union range");
    Check(std::ranges::all_of(channel_ranges, [](const auto &channel) {
              return channel.size() == 1 && channel[0].begin == 0 && channel[0].count == 3;
          }),
          "revision-zero batch marks every active channel range");
    scene.Commit();
    Check(scene.PayloadRevision() == initial_revision,
          "commit publishes the prepared payload revision");

    const auto baseline = scene.PayloadRevision();
    scene.BeginFrame();
    static_cast<void>(scene.Upsert({11, 0}, Instance(10)));
    static_cast<void>(scene.Upsert({22, 0}, Instance(2)));
    static_cast<void>(scene.Upsert({33, 0}, Instance(3)));
    scene.EndFrame();
    scene.Commit();
    const auto transform_revision = scene.PayloadRevision();

    scene.BeginFrame();
    auto material = scene.Metadata()[b.index];
    material.material_id = 0x1234;
    static_cast<void>(scene.Upsert({11, 0}, Instance(10)));
    static_cast<void>(scene.Upsert({22, 0}, Instance(2), material));
    static_cast<void>(scene.Upsert({33, 0}, Instance(3)));
    scene.EndFrame();
    scene.Commit();
    const auto material_revision = scene.PayloadRevision();
    collect(material_revision);
    Check(ranges.empty() && std::ranges::all_of(channel_ranges,
              [](const auto &channel) { return channel.empty(); }),
          "a backing at the accepted revision reports no pending upload");
    Check(transform_revision > baseline && material_revision > transform_revision,
          "payload revision advances independently for committed updates");
    collect(baseline);
    Check(channel_ranges[static_cast<std::size_t>(Channel::Transform)].size() == 1 &&
              channel_ranges[static_cast<std::size_t>(Channel::Transform)][0].begin == a.index &&
              channel_ranges[static_cast<std::size_t>(Channel::Transform)][0].count == 1,
          "channel collection reports transform changes after an older upload");
    Check(channel_ranges[static_cast<std::size_t>(Channel::Material)].size() == 1 &&
              channel_ranges[static_cast<std::size_t>(Channel::Material)][0].begin == b.index &&
              channel_ranges[static_cast<std::size_t>(Channel::Material)][0].count == 1,
          "channel collection retains material changes across skipped revisions");
    Check(ranges.size() == 1 && ranges[0].begin == a.index && ranges[0].count == 2,
          "union collection merges adjacent rows changed in different revisions");
    Check(channel_ranges[static_cast<std::size_t>(Channel::Bounds)].empty() &&
              channel_ranges[static_cast<std::size_t>(Channel::Skin)].empty() &&
              channel_ranges[static_cast<std::size_t>(Channel::Mesh)].empty() &&
              channel_ranges[static_cast<std::size_t>(Channel::Instance)].empty(),
          "unrelated channels do not inherit another channel change");

    const auto before_abort = scene.PayloadRevision();
    scene.BeginFrame();
    static_cast<void>(scene.Upsert({11, 0}, Instance(100)));
    scene.EndFrame();
    Check(scene.PayloadRevision() == before_abort + 1,
          "prepared update exposes its prospective payload revision");
    collect(before_abort);
    Check(ranges.size() == 1 && ranges[0].begin == a.index && ranges[0].count == 1 &&
              channel_ranges[static_cast<std::size_t>(Channel::Transform)].size() == 1 &&
              channel_ranges[static_cast<std::size_t>(Channel::Transform)][0].begin == a.index,
          "batch collection includes pending changed channels before commit");
    scene.Abort();
    Check(scene.PayloadRevision() == before_abort,
          "aborting prepared payload changes does not publish a revision");
    collect(before_abort);
    Check(ranges.empty() && std::ranges::all_of(channel_ranges,
              [](const auto &channel) { return channel.empty(); }),
          "aborted pending dirty channels are removed");

    scene.BeginFrame();
    static_cast<void>(scene.Upsert({11, 0}, Instance(1)));
    static_cast<void>(scene.Upsert({33, 0}, Instance(3)));
    scene.EndFrame();
    scene.Commit();
    collect(std::uint64_t{0});
    Check(ranges.size() == 2 && ranges[0].begin == a.index && ranges[0].count == 1 &&
              ranges[1].begin == c.index && ranges[1].count == 1,
          "inactive holes are excluded from all-active initialization ranges");

    const auto before_reuse = scene.PayloadRevision();
    scene.BeginFrame();
    const auto recycled = scene.Upsert({44, 0}, Instance(4));
    scene.EndFrame();
    Check(recycled.index == b.index, "new identity reuses the retired slot");
    scene.Commit();
    collect(before_reuse);
    Check(channel_ranges[static_cast<std::size_t>(Channel::Instance)].size() == 1 &&
              channel_ranges[static_cast<std::size_t>(Channel::Instance)][0].begin == recycled.index,
          "recycled identities request a fresh row upload");

    const auto before_reset = scene.PayloadRevision();
    scene.BeginFrame(true);
    static_cast<void>(scene.Upsert({66, 0}, Instance(6)));
    scene.EndFrame();
    scene.Commit();
    collect(before_reset);
    Check(scene.PayloadRevision() > before_reset && ranges.size() == 1 &&
              ranges[0].begin == 0 && ranges[0].count == 1,
          "session reset starts a fresh active table and stamps its rows");

    const auto before_clear = scene.PayloadRevision();
    scene.Clear();
    Check(scene.PayloadRevision() == before_clear,
          "clear preserves the monotonic payload revision epoch");
    scene.BeginFrame();
    static_cast<void>(scene.Upsert({55, 0}, Instance(5)));
    scene.EndFrame();
    scene.Commit();
    collect(before_clear);
    Check(ranges.size() == 1 && ranges[0].begin == 0,
          "post-clear rows cannot silently match an older uploaded revision");
}

void PreparedRemovalSetPreservesCommitAndAbortOrder()
{
    hs::PersistentGpuSceneSlots scene;
    scene.BeginFrame();
    for (std::uint64_t id = 1; id <= 5; ++id)
        static_cast<void>(scene.Upsert({id, 0}, Instance(static_cast<float>(id))));
    scene.EndFrame();
    scene.Commit();

    scene.BeginFrame();
    static_cast<void>(scene.Upsert({1, 0}, Instance(1.0f)));
    scene.EndFrame();
    Check(scene.Changes().destroyed == 4,
          "prepared removal set lost the ordered retirement count");
    scene.Abort();

    scene.BeginFrame();
    static_cast<void>(scene.Upsert({1, 0}, Instance(1.0f)));
    scene.EndFrame();
    scene.Commit();
    scene.BeginFrame();
    const auto first_reused = scene.Upsert({6, 0}, Instance(6.0f));
    const auto second_reused = scene.Upsert({7, 0}, Instance(7.0f));
    Check(first_reused.index == 4 && second_reused.index == 3,
          "prepared retirement changed ascending slot application or LIFO reuse");
    scene.Abort();
}
} // namespace

int main()
{
    try
    {
        ChannelUploadsPreserveTables();
        SkinChannelTransactionsPreserveScene();
        PacketSkinConsumerCoverageIsPreparedForEveryView();
        PayloadRevisionRangesTrackRotatingUploads();
        PreparedRemovalSetPreservesCommitAndAbortOrder();
        hs::PersistentGpuSceneSlots scene;

        scene.BeginFrame();
        const auto a0 = scene.Upsert({101, 0}, Instance(1.0f));
        const auto b0 = scene.Upsert({202, 0}, Instance(2.0f));
        const auto a0_duplicate = scene.Upsert({101, 0}, Instance(1.0f));
        Check(!a0.conflict && !b0.conflict && !a0_duplicate.conflict,
              "initial stable slots");
        Check(a0.index != b0.index && a0.index == a0_duplicate.index,
              "stable IDs must resolve to stable slots");
        Check(a0.generation == a0_duplicate.generation,
              "duplicate reference generation");
        scene.EndFrame();
        scene.Commit();
        Check(scene.Changes().created == 2 && scene.Changes().updated == 0 &&
                  scene.ContentRevision() != 0,
              "complete initial sample creates each unique source once");
        Check(scene.ActiveCount() == 2, "two active stable instances");
        Check(scene.DirtyRanges().size() == 1 &&
                  scene.DirtyRanges()[0].count == 2,
              "initial adjacent writes merge");

        scene.BeginFrame();
        const auto b1 = scene.Upsert({202, 0}, Instance(2.0f));
        const auto a1 = scene.Upsert({101, 0}, Instance(1.0f));
        scene.EndFrame();
        scene.Commit();
        Check(scene.Changes().unchanged == 2 && scene.Changes().created == 0 &&
                  scene.Changes().updated == 0,
              "draw reorder preserves the committed canonical identities");
        Check(a1.index == a0.index && b1.index == b0.index,
              "input order cannot renumber stable slots");
        Check(scene.DirtyRanges().empty(),
              "unchanged stable scene has no upload");

        hs::RenderInstance pose_source;
        pose_source.mesh = hs::RenderMesh::Archer;
        pose_source.revisions.pose = 77;
        hs::AnimationPoseRef pose;
        pose.revision = 77;
        hs::GpuSceneCanonicalInputs pose_inputs;
        pose_inputs.pose = hs::MakeRenderPoseKey(pose_source, &pose);
        hs::PersistentGpuSceneSlots pose_scene;
        pose_scene.BeginFrame();
        static_cast<void>(pose_scene.Upsert({404, 0}, Instance(6.0f), {}, pose_inputs));
        pose_scene.EndFrame();
        pose_scene.Commit();
        const auto original_content_revision = pose_scene.ContentRevision();
        const auto original_pose_revisions = pose_scene.Revisions(0);
        pose.normalized_time = 0.5f; // A supplied nonzero revision cannot hide this edit.
        pose_inputs.pose = hs::MakeRenderPoseKey(pose_source, &pose);
        pose_scene.BeginFrame();
        static_cast<void>(pose_scene.Upsert({404, 0}, Instance(6.0f), {}, pose_inputs));
        pose_scene.EndFrame();
        Check(pose_scene.Changes().updated == 1 && pose_scene.DirtyRanges().empty(),
              "actual pose-only change updates the canonical row without uploading instance bytes");
        Check(pose_scene.Revisions(0).pose != original_pose_revisions.pose &&
                  pose_scene.Revisions(0).content == original_pose_revisions.content &&
                  pose_scene.ContentRevision() == original_content_revision,
              "pose changes preserve the material/content history consumed by volumetrics");
        pose_scene.Abort();
        Check(pose_scene.Revisions(0) == original_pose_revisions,
              "abort retains the last successful category revisions");
        pose_scene.BeginFrame();
        static_cast<void>(pose_scene.Upsert({404, 0}, Instance(6.0f), {}, pose_inputs));
        pose_scene.EndFrame();
        Check(pose_scene.Changes().updated == 1, "failed submission cannot publish its pose key");
        pose_scene.Commit();
        pose_scene.BeginFrame();
        static_cast<void>(pose_scene.Upsert({404, 0}, Instance(6.0f), {}, pose_inputs));
        pose_scene.EndFrame();
        Check(pose_scene.Changes().unchanged == 1, "reused packet compares with the last successful sample");
        auto different_pose = pose_inputs;
        different_pose.pose.values[8] ^= 1;
        Check(pose_scene.Upsert({404, 0}, Instance(6.0f), {}, different_pose).conflict,
              "duplicate draw references cannot disagree on exact pose operands");
        pose_scene.Abort();

        hs::PersistentGpuSceneSlots interpolation_scene;
        hs::GpuSceneCanonicalInputs source_inputs;
        source_inputs.supplied_transform_revision = 101;
        source_inputs.supplied_material_revision = 202;
        source_inputs.content[5] = 303;
        interpolation_scene.BeginFrame();
        const auto interpolation_first = interpolation_scene.Upsert(
            {505, 0}, Instance(10.0f), {}, source_inputs);
        interpolation_scene.EndFrame();
        interpolation_scene.Commit();
        const auto original_interpolation_revisions = interpolation_scene.Revisions(interpolation_first.index);
        const auto interpolation_content_revision = interpolation_scene.ContentRevision();
        interpolation_scene.BeginFrame();
        const auto interpolation_second = interpolation_scene.Upsert(
            {505, 0}, Instance(10.5f), {}, source_inputs);
        interpolation_scene.EndFrame();
        Check(interpolation_scene.Changes().updated == 1 && !interpolation_second.conflict &&
                  interpolation_scene.DirtyRanges().size() == 1 &&
                  interpolation_scene.DirtyRanges()[0].begin == interpolation_first.index,
              "final interpolation changes upload one slot despite unchanged supplied revisions");
        Check(interpolation_scene.Revisions(interpolation_second.index).transform != original_interpolation_revisions.transform &&
                  interpolation_scene.Revisions(interpolation_second.index).material == original_interpolation_revisions.material &&
                  interpolation_scene.Revisions(interpolation_second.index).content == original_interpolation_revisions.content &&
                  interpolation_scene.ContentRevision() == interpolation_content_revision,
              "actual motion advances only its owner transform revision");
        interpolation_scene.Commit();
        interpolation_scene.BeginFrame();
        ++source_inputs.supplied_transform_revision;
        const auto revision_only = interpolation_scene.Upsert(
            {505, 0}, Instance(10.5f), {}, source_inputs);
        interpolation_scene.EndFrame();
        Check(!revision_only.conflict && interpolation_scene.Changes().updated == 1 &&
                  interpolation_scene.DirtyRanges().empty(),
              "development revision changes cannot upload identical physical bytes");
        interpolation_scene.Commit();
        hs::PersistentGpuSceneSlots identity_scene;
        hs::GpuSceneMetadata identity{};
        identity.asset_id = 0x1122334455667788ull;
        identity.material_id = 0x8877665544332211ull;
        identity.skin_flags = 0x40000u;
        identity_scene.BeginFrame();
        const auto identity_first = identity_scene.Upsert(
            {606, 0}, Instance(12.0f), identity);
        identity_scene.EndFrame();
        identity_scene.Commit();
        Check(identity_scene.Metadata().size() == 1 &&
                  identity_scene.Metadata()[identity_first.index].generation ==
                      identity_first.generation &&
                  identity_scene.Metadata()[identity_first.index].skin_index ==
                      identity_first.index,
              "persistent scene metadata carries generation and stable skin slot");
        static_cast<void>(identity_scene.DirtyRanges());
        identity_scene.BeginFrame();
        identity.asset_id ^= 1u;
        const auto identity_changed = identity_scene.Upsert(
            {606, 0}, Instance(12.0f), identity);
        identity_scene.EndFrame();
        identity_scene.Commit();
        Check(!identity_changed.conflict &&
                  identity_scene.DirtyRanges().size() == 1 &&
                  identity_scene.DirtyRanges()[0].count == 1,
              "asset identity changes dirty exactly one split-table slot");
        Check(identity_scene.Revisions(identity_changed.index).content == 2 &&
                  identity_scene.Revisions(identity_changed.index).material == 1 &&
                  identity_scene.ContentRevision() == 2,
              "actual content rebinding invalidates shadow/GI/volume consumers without a producer hash");

        hs::GpuSceneTableLayout table_layout;
        Check(hs::BuildGpuSceneTableLayout(4, 2, table_layout) &&
                  table_layout.header.magic == hs::kGpuSceneTableMagic &&
                  table_layout.header.version == hs::kGpuSceneTableVersion &&
                  table_layout.header.version == 3 &&
                  table_layout.header.skin_stride == 32 &&
                  table_layout.header.capacity == 4 &&
                  table_layout.header.slot_count == 2 &&
                  table_layout.header.instance_offset == sizeof(hs::GpuSceneTableHeader) &&
                  table_layout.total_bytes ==
                      sizeof(hs::GpuSceneTableHeader) +
                          hs::kGpuSceneSlotPayloadBytes * 4,
              "versioned split GPU scene table layout is deterministic");
        const auto serialized = hs::SerializeGpuSceneSlot(
            3, Instance(13.0f, 17),
             {0x1122334455667788ull, 0x8877665544332211ull, 3, 0x40000u, 9, 0, 62719, 12565, 36, 12});
        Check(serialized.instance.transform_index == 3 &&
                  serialized.instance.mesh_index == 3 &&
                  serialized.instance.generation == 9 &&
                  serialized.mesh.mesh == 17 &&
                  serialized.mesh.asset_id_low == 0x55667788u &&
                  serialized.mesh.asset_id_high == 0x11223344u &&
                  serialized.material.material_id_low == 0x44332211u &&
                  serialized.material.material_id_high == 0x88776655u &&
                  serialized.bounds.center_radius.w == 1.0f &&
                  serialized.skin.skin_index == 3 &&
                  serialized.skin.vertex_offset == 62719 &&
                  serialized.skin.previous_vertex_offset == 12565 &&
                  serialized.skin.vertex_stride == 36 && serialized.skin.previous_vertex_stride == 12 &&
                  serialized.skin.reserved0 == 0 && serialized.skin.reserved1 == 0,
              "split GPU scene slot serialization preserves identities and bindings");

        scene.BeginFrame();
        const auto a2 = scene.Upsert({101, 0}, Instance(7.0f));
        scene.EndFrame();
        scene.Commit();
        Check(a2.index == a0.index, "updated stable instance keeps slot");
        Check(scene.ActiveCount() == 1, "missing instance is destroyed");
        Check(scene.Changes().destroyed == 1,
              "a complete sample retires absent sources without a delta generation chain");
        Check(scene.DirtyRanges().size() == 1 &&
                  scene.DirtyRanges()[0].begin == a0.index &&
                  scene.DirtyRanges()[0].count == 1,
              "one changed instance uploads one slot");

        scene.BeginFrame();
        const auto c0 = scene.Upsert({303, 0}, Instance(3.0f));
        scene.Upsert({101, 0}, Instance(7.0f));
        scene.EndFrame();
        scene.Commit();
        Check(c0.index == b0.index,
              "destroyed slot is reused instead of growing forever");
        Check(c0.generation != b0.generation,
              "reused slot must advance generation");

        scene.BeginFrame();
        const auto anonymous0 = scene.Upsert({0, 1}, Instance(4.0f));
        const auto anonymous1 = scene.Upsert({0, 2}, Instance(5.0f));
        const auto c1 = scene.Upsert({303, 0}, Instance(3.0f));
        const auto conflict = scene.Upsert({303, 0}, Instance(9.0f));
        scene.EndFrame();
        scene.Commit();
        Check(!anonymous0.conflict && !anonymous1.conflict &&
                  anonymous0.index != anonymous1.index,
              "anonymous fallback ordinals stay distinct");
        Check(!c1.conflict, "first stable publication in a frame is valid");
        Check(conflict.conflict,
              "same stable ID cannot publish conflicting base payloads in one frame");

        scene.BeginFrame();
        static_cast<void>(scene.Upsert({101, 0}, Instance(7.0f)));
        static_cast<void>(scene.Upsert({303, 0}, Instance(3.0f)));
        scene.EndFrame();
        scene.Commit();
        scene.MarkAllActiveDirty();
        Check(!scene.DirtyRanges().empty(), "buffer replacement can force live-slot upload");

        hs::PersistentGpuSceneSlots aborted_scene;
        aborted_scene.BeginFrame();
        const auto retained = aborted_scene.Upsert({600, 0}, Instance(6.0f));
        const auto removed = aborted_scene.Upsert({700, 0}, Instance(7.0f));
        aborted_scene.EndFrame();
        aborted_scene.Commit();
        aborted_scene.BeginFrame();
        static_cast<void>(aborted_scene.Upsert({600, 0}, Instance(60.0f)));
        const auto unpublished = aborted_scene.Upsert({800, 0}, Instance(8.0f));
        aborted_scene.EndFrame();
        Check(unpublished.index >= 2, "prepared create appends a new slot");
        aborted_scene.Abort();
        Check(aborted_scene.Instances().size() == 2 &&
                  aborted_scene.Instances()[retained.index].position.x == 6.0f,
              "abort restores submitted payload and removes unsubmitted slot growth");
        aborted_scene.BeginFrame();
        const auto retained_again = aborted_scene.Upsert({600, 0}, Instance(6.0f));
        const auto removed_again = aborted_scene.Upsert({700, 0}, Instance(7.0f));
        aborted_scene.EndFrame();
        Check(retained_again.index == retained.index && removed_again.index == removed.index &&
                  removed_again.generation == removed.generation &&
                  aborted_scene.DirtyRanges().empty(),
              "aborted destruction cannot retire or renumber submitted slots");
        aborted_scene.Commit();
        aborted_scene.BeginFrame();
        static_cast<void>(aborted_scene.Upsert({600, 0}, Instance(6.0f)));
        aborted_scene.EndFrame();
        aborted_scene.Commit();
        aborted_scene.BeginFrame();
        const auto aborted_reuse = aborted_scene.Upsert({900, 0}, Instance(9.0f));
        aborted_scene.EndFrame();
        aborted_scene.BeginFrame();
        const auto submitted_reuse = aborted_scene.Upsert({1000, 0}, Instance(10.0f));
        Check(submitted_reuse.index == removed.index &&
                  submitted_reuse.generation == aborted_reuse.generation,
              "aborted slot reuse cannot consume retained generation or free slot");
        aborted_scene.EndFrame();
        aborted_scene.Commit();

        aborted_scene.BeginFrame(true);
        const auto reset_slot = aborted_scene.Upsert({2000, 0}, Instance(20.0f));
        Check(reset_slot.index == 0 && aborted_scene.SlotCount() == 1,
              "new session prepares a fresh slot table");
        aborted_scene.EndFrame();
        aborted_scene.Abort();
        Check(aborted_scene.SlotCount() == 2 &&
                  aborted_scene.Instances()[submitted_reuse.index].position.x == 10.0f,
              "aborted session reset restores submitted slots and payloads");
        aborted_scene.BeginFrame();
        const auto after_reset_abort = aborted_scene.Upsert({1000, 0}, Instance(10.0f));
        Check(after_reset_abort.index == submitted_reuse.index &&
                  after_reset_abort.generation == submitted_reuse.generation,
              "aborted reset preserves generation of the submitted reused slot");
        aborted_scene.EndFrame();
        aborted_scene.Commit();

        hs::PersistentGpuSceneSlots staged_scene;
        staged_scene.BeginFrame();
        for (std::uint64_t id = 1; id <= 96; ++id)
            static_cast<void>(staged_scene.Upsert({id, 0}, Instance(float(id))));
        staged_scene.EndFrame();
        staged_scene.Commit();
        staged_scene.BeginFrame(true);
        static_cast<void>(staged_scene.Upsert({5000, 0}, Instance(5000.0f)));
        staged_scene.MarkAllActiveDirty();
        Check(staged_scene.DirtyRanges().size() == 1 &&
                  staged_scene.DirtyRanges()[0].begin == 0 &&
                  staged_scene.DirtyRanges()[0].count == 1,
              "reset staging must not publish untouched high-water slots");
        staged_scene.Abort();
        staged_scene.BeginFrame();
        const auto staged_retained = staged_scene.Upsert({96, 0}, Instance(96.0f));
        const auto staged_duplicate = staged_scene.Upsert({96, 0}, Instance(96.0f));
        const auto staged_conflict = staged_scene.Upsert({96, 0}, Instance(960.0f));
        Check(staged_retained.index == 95 && !staged_duplicate.conflict && staged_conflict.conflict &&
                  staged_scene.DirtyRanges().empty(),
              "aborted reset cannot expose stale staged payloads or duplicate revisions");
        staged_scene.EndFrame();
        staged_scene.Commit();
        staged_scene.BeginFrame();
        const auto staged_reuse = staged_scene.Upsert({6000, 0}, Instance(6000.0f));
        Check(staged_reuse.index == 94 && staged_reuse.generation == 2,
              "slot staging preserves committed free-slot reuse and generation");
        staged_scene.EndFrame();
        staged_scene.Commit();

        hs::SkinVertexRanges previous_ranges;
        hs::SkinVertexRange archer_range, melee_range, duplicate_range;
        Check(previous_ranges.Resolve({10, 0}, 62719, archer_range) && archer_range.offset == 0 &&
                  previous_ranges.Resolve({20, 0}, 12565, melee_range) && melee_range.offset == 62719,
              "mixed meshes reserve their actual vertex counts, not largest-mesh slots");
        Check(previous_ranges.Resolve({20, 0}, 12565, duplicate_range) &&
                  duplicate_range.offset == melee_range.offset && previous_ranges.VertexCount() == 75284,
              "duplicate draw keys share their exact deformation range");
        Check(!previous_ranges.Resolve({20, 0}, 17803, duplicate_range) &&
                  previous_ranges.VertexCount() == 75284,
              "conflicting duplicate geometry cannot overlap another range");
        const auto retained_ranges = previous_ranges;
        hs::SkinVertexRanges reordered_ranges;
        Check(reordered_ranges.Resolve({20, 0}, 12565, melee_range) && melee_range.offset == 0 &&
                  reordered_ranges.Resolve({10, 0}, 62719, archer_range) && archer_range.offset == 12565 &&
                  retained_ranges.Find({20, 0})->offset == 62719 &&
                  retained_ranges.Find({10, 0})->offset == 0,
              "reordered current layout cannot overwrite retained previous-frame offsets");
        const auto *packet_melee = retained_ranges.Find({20, 0});
        const auto *packet_archer = retained_ranges.Find({10, 0});
        Check(packet_melee && packet_archer && packet_melee->offset == 62719 &&
                  packet_archer->offset == 0 && reordered_ranges.Find({20, 0})->offset != packet_melee->offset &&
                  reordered_ranges.Find({10, 0})->offset != packet_archer->offset,
              "later views retain packet-owned current ranges while their previous-frame layout stays independent");
        hs::SkinVertexRanges despawned_ranges;
        Check(despawned_ranges.Resolve({30, 0}, 17803, duplicate_range) &&
                  duplicate_range.offset == 0 && !despawned_ranges.Find({20, 0}) &&
                  !retained_ranges.Find({30, 0}),
              "new identities cannot inherit deformation history from reused storage");
        Check(!despawned_ranges.Find({99, 0}) && despawned_ranges.VertexCount() == 17803,
              "non-skinned slots consume no deformation vertices");
        hs::SkinVertexRanges maximum_ranges;
        Check(maximum_ranges.Resolve({1, 0}, hs::SkinVertexRanges::kMaxVertices, duplicate_range) &&
                  !maximum_ranges.Resolve({2, 0}, 1, duplicate_range) &&
                  maximum_ranges.VertexCount() == hs::SkinVertexRanges::kMaxVertices,
              "packed vertex bytes cannot wrap the shader's 32-bit address space");
        hs::SkinVertexRanges invalid_ranges;
        Check(!invalid_ranges.Resolve({1, 0}, 0, duplicate_range) &&
                  !invalid_ranges.Resolve({1, 0}, UINT32_MAX, duplicate_range) &&
                  invalid_ranges.VertexCount() == 0,
              "invalid geometry ranges do not mutate the layout");
        hs::SkinVertexRanges geometry_ranges;
        Check(geometry_ranges.Resolve({1, 0}, 3, duplicate_range, 7) &&
                  !geometry_ranges.Resolve({1, 0}, 3, duplicate_range, 8) &&
                  geometry_ranges.Find({1, 0})->geometry_identity == 7,
              "same vertex count cannot conceal a different submitted geometry identity");
        Check(hs::SkinVertexRanges::kVertexBytes == 36 &&
                  hs::SkinVertexRanges::kPreviousPositionBytes == 12 &&
                  hs::SkinVertexRangeFits(2, 3, 36, 180) &&
                  !hs::SkinVertexRangeFits(2, 3, 36, 179) &&
                  hs::SkinVertexRangeFits(2, 3, 12, 60) &&
                  !hs::SkinVertexRangeFits(2, 3, 36, 60) &&
                  !hs::SkinVertexRangeFits(2, 3, 12, 59) &&
                  !hs::SkinVertexRangeFits(0, 0, 36, 36) &&
                  !hs::SkinVertexRangeFits(0, 1, 76, 76) &&
                  !hs::SkinVertexRangeFits(UINT32_MAX, 1, 36, UINT64_MAX),
              "regular deformation and supplemental previous positions use their own bounded byte strides");
        // Offset changes must upload even if the pose and transform did not change.
        identity_scene.BeginFrame();
        identity.skin_vertex_offset = 123;
        identity.previous_skin_vertex_offset = 456;
        identity.previous_skin_vertex_stride = 12;
        const auto moved_skin = identity_scene.Upsert({606, 0}, Instance(12.0f), identity);
        Check(!moved_skin.conflict && identity_scene.DirtyRanges().size() == 1,
              "compact skin relocation dirties persistent GPU metadata");
        identity_scene.Abort();
        Check(identity_scene.Metadata()[moved_skin.index].skin_vertex_offset == 0 &&
                  identity_scene.Metadata()[moved_skin.index].previous_skin_vertex_offset == 0 &&
                  identity_scene.Metadata()[moved_skin.index].previous_skin_vertex_stride == 36,
              "aborted preparation preserves the last submitted skin addressing");
        identity_scene.BeginFrame();
        auto stride_only = identity_scene.Metadata()[moved_skin.index];
        stride_only.previous_skin_vertex_stride = 12;
        const auto changed_stride = identity_scene.Upsert({606, 0}, Instance(12.0f), stride_only);
        Check(!changed_stride.conflict && identity_scene.DirtyRanges().size() == 1,
              "history stride changes upload even when skin offsets do not change");
        identity_scene.Abort();

        std::array<std::uint64_t,4> page_offsets{};
        std::uint64_t page_bytes{};
        const std::array<std::uint64_t,4> page_sizes{76,12,64,28};
        Check(hs::GeometryPageLayout(page_sizes,page_offsets,page_bytes),"mixed geometry payloads pack successfully");
        Check(page_offsets==std::array<std::uint64_t,4>{0,80,96,160} && page_bytes==188,
            "packed raw slices preserve vertex origin and 16-byte descriptor alignment");
        std::array<std::byte,188> packed{};
        for(std::size_t slice=0;slice<page_sizes.size();++slice)
            for(std::size_t byte=0;byte<page_sizes[slice];++byte)
                packed[page_offsets[slice]+byte]=std::byte((slice*31+byte)%256);
        for(std::size_t slice=0;slice<page_sizes.size();++slice)
            for(std::size_t byte=0;byte<page_sizes[slice];++byte)
                Check(packed[page_offsets[slice]+byte]==std::byte((slice*31+byte)%256),
                    "raw descriptor slice reads preserve canonical payload bytes without overlap");
        Check(!hs::GeometryPageLayout({76,0,64,28},page_offsets,page_bytes),"empty geometry slice is rejected");
        Check(!hs::GeometryPageLayout({76,13,64,28},page_offsets,page_bytes),"unaligned raw payload is rejected");
        Check(!hs::GeometryPageLayout({UINT32_MAX-3ull,4,4,4},page_offsets,page_bytes),
            "packed offset overflow is rejected before allocation");
        std::puts("Persistent GPU scene slot tests passed.");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
