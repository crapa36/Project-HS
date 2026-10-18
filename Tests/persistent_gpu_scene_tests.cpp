#include "persistent_gpu_scene.hpp"

#include <cstdio>
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
} // namespace

int main()
{
    try
    {
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
        Check(scene.ActiveCount() == 2, "two active stable instances");
        Check(scene.DirtyRanges().size() == 1 &&
                  scene.DirtyRanges()[0].count == 2,
              "initial adjacent writes merge");

        scene.BeginFrame();
        const auto b1 = scene.Upsert({202, 0}, Instance(2.0f));
        const auto a1 = scene.Upsert({101, 0}, Instance(1.0f));
        scene.EndFrame();
        scene.Commit();
        Check(a1.index == a0.index && b1.index == b0.index,
              "input order cannot renumber stable slots");
        Check(scene.DirtyRanges().empty(),
              "unchanged stable scene has no upload");

        scene.BeginFrame();
        hs::RenderInstance::Revisions pose_only{};
        pose_only.transform = 11;
        pose_only.material = 22;
        pose_only.content = 33;
        pose_only.pose = 1;
        const auto pose_slot = scene.Upsert({101, 0}, Instance(1.0f), pose_only);
        pose_only.pose = 2;
        const auto same_frame_pose = scene.Upsert({101, 0}, Instance(1.0f), pose_only);
        static_cast<void>(scene.Upsert({202, 0}, Instance(2.0f)));
        scene.EndFrame();
        scene.Commit();
        Check(!pose_slot.conflict && !same_frame_pose.conflict,
              "pose revision is owned by SkinCache, not the instance table");

        hs::PersistentGpuSceneSlots pose_scene;
        pose_scene.BeginFrame();
        pose_only.pose = 10;
        static_cast<void>(pose_scene.Upsert({404, 0}, Instance(6.0f), pose_only));
        pose_scene.EndFrame();
        pose_scene.Commit();
        static_cast<void>(pose_scene.DirtyRanges());
        pose_scene.BeginFrame();
        pose_only.pose = 11;
        static_cast<void>(pose_scene.Upsert({404, 0}, Instance(6.0f), pose_only));
        pose_scene.EndFrame();
        pose_scene.Commit();
        Check(pose_scene.DirtyRanges().empty(),
              "pose-only changes cannot upload the persistent instance payload");

        hs::PersistentGpuSceneSlots interpolation_scene;
        hs::RenderInstance::Revisions interpolated_revisions{};
        interpolated_revisions.transform = 101;
        interpolated_revisions.material = 202;
        interpolated_revisions.content = 303;
        interpolation_scene.BeginFrame();
        const auto interpolation_first = interpolation_scene.Upsert(
            {505, 0}, Instance(10.0f), interpolated_revisions);
        interpolation_scene.EndFrame();
        interpolation_scene.Commit();
        static_cast<void>(interpolation_scene.DirtyRanges());
        interpolation_scene.BeginFrame();
        const auto interpolation_second = interpolation_scene.Upsert(
            {505, 0}, Instance(10.5f), interpolated_revisions);
        interpolation_scene.EndFrame();
        interpolation_scene.Commit();
        Check(!interpolation_first.conflict && !interpolation_second.conflict,
              "interpolated payload changes cannot conflict with stable source revisions");
        Check(interpolation_scene.DirtyRanges().size() == 1 &&
                  interpolation_scene.DirtyRanges()[0].begin == interpolation_first.index &&
                  interpolation_scene.DirtyRanges()[0].count == 1,
              "interpolated payload change uploads exactly one stable slot");

        interpolation_scene.BeginFrame();
        ++interpolated_revisions.transform;
        const auto revision_only = interpolation_scene.Upsert(
            {505, 0}, Instance(10.5f), interpolated_revisions);
        interpolation_scene.EndFrame();
        interpolation_scene.Commit();
        Check(!revision_only.conflict && interpolation_scene.DirtyRanges().empty(),
              "revision metadata changes without payload changes do not upload bytes");

        hs::PersistentGpuSceneSlots identity_scene;
        hs::GpuSceneMetadata identity{};
        identity.asset_id = 0x1122334455667788ull;
        identity.material_id = 0x8877665544332211ull;
        identity.skin_flags = 0x40000u;
        identity_scene.BeginFrame();
        const auto identity_first = identity_scene.Upsert(
            {606, 0}, Instance(12.0f), {}, identity);
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
            {606, 0}, Instance(12.0f), {}, identity);
        identity_scene.EndFrame();
        identity_scene.Commit();
        Check(!identity_changed.conflict &&
                  identity_scene.DirtyRanges().size() == 1 &&
                  identity_scene.DirtyRanges()[0].count == 1,
              "asset identity changes dirty exactly one split-table slot");

        hs::GpuSceneTableLayout table_layout;
        Check(hs::BuildGpuSceneTableLayout(4, 2, table_layout) &&
                  table_layout.header.magic == hs::kGpuSceneTableMagic &&
                  table_layout.header.version == hs::kGpuSceneTableVersion &&
                  table_layout.header.capacity == 4 &&
                  table_layout.header.slot_count == 2 &&
                  table_layout.header.instance_offset == sizeof(hs::GpuSceneTableHeader) &&
                  table_layout.total_bytes ==
                      sizeof(hs::GpuSceneTableHeader) +
                          hs::kGpuSceneSlotPayloadBytes * 4,
              "versioned split GPU scene table layout is deterministic");
        const auto serialized = hs::SerializeGpuSceneSlot(
            3, Instance(13.0f, 17),
            {0x1122334455667788ull, 0x8877665544332211ull, 3, 0x40000u, 9, 0});
        Check(serialized.instance.transform_index == 3 &&
                  serialized.instance.mesh_index == 3 &&
                  serialized.instance.generation == 9 &&
                  serialized.mesh.mesh == 17 &&
                  serialized.mesh.asset_id_low == 0x55667788u &&
                  serialized.mesh.asset_id_high == 0x11223344u &&
                  serialized.material.material_id_low == 0x44332211u &&
                  serialized.material.material_id_high == 0x88776655u &&
                  serialized.bounds.center_radius.w == 1.0f &&
                  serialized.skin.skin_index == 3,
              "split GPU scene slot serialization preserves identities and bindings");

        scene.BeginFrame();
        const auto a2 = scene.Upsert({101, 0}, Instance(7.0f));
        scene.EndFrame();
        scene.Commit();
        Check(a2.index == a0.index, "updated stable instance keeps slot");
        Check(scene.ActiveCount() == 1, "missing instance is destroyed");
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

        std::puts("Persistent GPU scene slot tests passed.");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
