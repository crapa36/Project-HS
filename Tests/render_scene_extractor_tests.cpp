#include "render_scene_extractor.hpp"
#include "visibility_encoding.hpp"
#include <hs/presentation/projector.hpp>

#include <bit>
#include <limits>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace
{
void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

hs::RenderInstance Instance(std::uint64_t id, float x)
{
    hs::RenderInstance instance;
    instance.stable_id = id;
    instance.mesh = hs::RenderMesh::Archer;
    instance.position.x = x;
    hs::PopulateRenderInstanceRevisions(instance);
    return instance;
}

void CheckPopulatedRevisions()
{
    hs::RenderInstance instance;
    instance.mesh = hs::RenderMesh::Archer;
    instance.revisions = {11, 22, 33, 44};
    const auto explicit_revisions = instance.revisions;
    hs::PopulateRenderInstanceRevisions(instance);
    Check(instance.asset_id.value != 0 && instance.material_id.value != 0 &&
              instance.revisions == explicit_revisions,
          "explicit revisions must preserve identity population");
    instance.position.x = 5.0f;
    hs::PopulateRenderInstanceRevisions(instance);
    Check(instance.revisions == explicit_revisions,
          "repeated population must retain caller-owned revisions");

    instance.animation_clip_id.value = 123;
    hs::PopulateRenderInstanceRevisions(instance);
    const auto first_pose = instance.revisions.pose;
    instance.animation_time_seconds = 0.5;
    const auto expected_pose = hs::DeriveNamedAnimationPoseRevision(instance);
    hs::PopulateRenderInstanceRevisions(instance);
    Check(instance.revisions.pose == expected_pose && instance.revisions.pose != first_pose &&
              instance.revisions.transform == explicit_revisions.transform &&
              instance.revisions.material == explicit_revisions.material &&
              instance.revisions.content == explicit_revisions.content,
          "named animation time must refresh pose without replacing explicit revisions");
}

void CheckSharedSceneRules()
{
    hs::RenderSceneProjectionState producer;
    hs::RenderSceneExtractor consumer;
    hs::RenderSceneDelta published, submitted;
    std::vector<hs::RenderInstance> instances{Instance(0, 1), Instance(42, 2)};
    const auto compare = [&] {
        Check(published.created == submitted.created &&
                  published.updated == submitted.updated &&
                  published.unchanged == submitted.unchanged &&
                  published.destroyed == submitted.destroyed &&
                  published.instances.size() == submitted.instances.size(),
              "producer and consumer scene counts/retirements must agree");
        for (std::size_t i = 0; i < published.instances.size(); ++i)
        {
            const auto &a = published.instances[i], &b = submitted.instances[i];
            Check(a.key == b.key && a.source_index == b.source_index &&
                      a.revisions == b.revisions && a.kind == b.kind &&
                      a.revision_mask == b.revision_mask,
                  "producer and consumer deltas must agree");
        }
    };
    Check(producer.Build(instances, 7, published) && consumer.Build(instances, submitted),
          "initial shared scene rules");
    compare();
    Check(published.instances[0].key == hs::RenderSceneInstanceKey{0, 1} &&
              published.instances[1].key == hs::RenderSceneInstanceKey{42, 0} &&
              published.instances[0].revision_mask == 15,
          "legacy identity and create-mask encoding");
    producer.Commit(); consumer.Commit();
    for (unsigned mask = 0; mask < 16; ++mask)
    {
        auto &r = instances[1].revisions;
        if (mask & 1) ++r.transform;
        if (mask & 2) ++r.pose;
        if (mask & 4) ++r.material;
        if (mask & 8) ++r.content;
        Check(producer.Build(instances, 7, published) && consumer.Build(instances, submitted),
              "all combinations of revision changes");
        compare();
        Check(published.instances[1].revision_mask == mask,
              "revision bits retain their original ABI");
        producer.Commit(); consumer.Commit();
    }
    // Publication can advance while submission fails. Each owner retains its
    // own committed history and the next renderer fallback catches both edits.
    ++instances[1].revisions.transform;
    Check(producer.Build(instances, 7, published) && consumer.Build(instances, submitted),
          "prepare skipped submission");
    producer.Commit(); consumer.Abort();
    ++instances[1].revisions.material;
    Check(producer.Build(instances, 7, published) && consumer.Build(instances, submitted),
          "recover skipped submission");
    Check(published.instances[1].revision_mask == 4 &&
              submitted.instances[1].revision_mask == 5,
          "publication and successful-submission histories remain independent");
    producer.Commit(); consumer.Commit();
    Check(producer.Build(instances, 8, published) && consumer.Build(instances, submitted, true),
          "reset session");
    compare();
    Check(published.base_generation == 0 && published.generation == 1,
          "session restart resets producer generation");
    producer.Abort(); consumer.Abort();
    Check(producer.Build(instances, 7, published) && consumer.Build(instances, submitted),
          "abort reset retains both histories");
    compare();
    Check(published.unchanged == 2, "aborted reset must not become a create");
    Check(hs::MakeRenderSceneInstanceKey(0, std::numeric_limits<std::uint32_t>::max() - 1)
              .fallback_ordinal == std::numeric_limits<std::uint32_t>::max(),
          "largest valid anonymous source ordinal");

    for (float radius : {0.0f, 0.035f, 1.0f, 128.5f})
    {
        Check(std::bit_cast<std::uint32_t>(hs::EncodeGrassDeformationRadius(
                  radius, hs::OcclusionMode::Enabled)) == std::bit_cast<std::uint32_t>(radius) &&
                  std::bit_cast<std::uint32_t>(hs::EncodeGrassDeformationRadius(
                  radius, hs::OcclusionMode::Disabled)) == std::bit_cast<std::uint32_t>(-radius),
              "occlusion root constant preserves legacy bits including signed zero");
    }
}
} // namespace

int main()
{
    try
    {
        CheckPopulatedRevisions();
        CheckSharedSceneRules();
        hs::RenderSceneExtractor extractor;
        hs::RenderSceneDelta delta;
        std::vector<hs::RenderInstance> instances{Instance(11, 1.0f), Instance(22, 2.0f)};

        Check(extractor.Build(instances, delta), "initial scene delta");
        extractor.Commit();
        Check(delta.created == 2 && delta.updated == 0 && delta.unchanged == 0 &&
                  delta.destroyed.empty(),
              "initial instances must be creates");

        Check(extractor.Build(instances, delta), "unchanged scene delta");
        extractor.Commit();
        Check(delta.created == 0 && delta.updated == 0 && delta.unchanged == 2 &&
                  delta.destroyed.empty(),
              "unchanged instances must not update");

        instances[0].position.x = 3.0f;
        instances[0].revisions = {};
        hs::PopulateRenderInstanceRevisions(instances[0]);
        Check(extractor.Build(instances, delta), "transform scene delta");
        extractor.Commit();
        Check(delta.updated == 1 && delta.instances[0].revision_mask ==
                  hs::RenderSceneTransformChanged,
              "transform revision must be isolated");

        instances[0].revisions.pose = 0xabcdu;
        Check(extractor.Build(instances, delta), "pose scene delta");
        extractor.Commit();
        Check(delta.updated == 1 && delta.instances[0].revision_mask ==
                  hs::RenderScenePoseChanged,
              "pose revision must be isolated");

        instances[0].material_id = hs::MakeAssetId("render/test/material-binding.rpkg");
        instances[0].revisions = {};
        instances[0].revisions.pose = 0xabcdu;
        hs::PopulateRenderInstanceRevisions(instances[0]);
        Check(extractor.Build(instances, delta), "material scene delta");
        extractor.Commit();
        Check(delta.updated == 1 && delta.instances[0].revision_mask ==
                  hs::RenderSceneMaterialChanged,
              "material identity must contribute only to material revision");

        instances[0].asset_id = hs::MakeAssetId("render/test/replacement.rpkg");
        instances[0].revisions.content = 0;
        hs::PopulateRenderInstanceRevisions(instances[0]);
        Check(extractor.Build(instances, delta), "content scene delta");
        extractor.Commit();
        Check(delta.updated == 1 && delta.instances[0].revision_mask ==
                  hs::RenderSceneContentChanged,
              "asset identity must contribute only to content revision");

        instances.resize(1);
        Check(extractor.Build(instances, delta), "destroy scene delta");
        extractor.Commit();
        Check(delta.destroyed.size() == 1 && delta.destroyed[0].stable_id == 22,
              "missing stable instance must be destroyed");

        auto replacement = Instance(33, 4.0f);
        instances.push_back(replacement);
        Check(extractor.Build(instances, delta), "replacement scene delta");
        extractor.Commit();
        Check(delta.created == 1 && delta.updated == 0 && delta.destroyed.empty(),
              "replacement stable ID must be a create");

        auto duplicate = instances;
        duplicate.push_back(instances[0]);
        Check(!extractor.Build(duplicate, delta), "duplicate stable IDs must be rejected");
        extractor.Commit(); // A failed preparation cannot be committed.

        auto aborted = instances;
        aborted[0] = Instance(44, 9.0f);
        Check(extractor.Build(aborted, delta) && delta.created == 1 &&
                  delta.destroyed.size() == 1,
              "prepare replacement delta before aborted submission");
        extractor.Abort();
        Check(extractor.Build(instances, delta) && delta.unchanged == 2 &&
                  delta.created == 0 && delta.destroyed.empty(),
              "aborted scene preparation cannot publish creates or destroys");
        extractor.Commit();

        Check(extractor.Build(aborted, delta), "prepare without explicit abort");
        Check(extractor.Build(instances, delta) && delta.unchanged == 2,
              "next preparation must discard unsubmitted scene revisions");
        extractor.Commit();

        Check(extractor.Build(instances, delta, true) && delta.created == 2,
              "new session prepares a fresh scene history");
        extractor.Abort();
        Check(extractor.Build(instances, delta) && delta.unchanged == 2,
              "aborted session reset must retain submitted scene history");
        extractor.Commit();

        Check(extractor.Adopt(aborted), "adopt published replacement");
        extractor.Abort();
        Check(extractor.Build(instances, delta) && delta.unchanged == 2 &&
                  delta.destroyed.empty(),
              "aborted adoption must retain submitted history");
        extractor.Commit();
        Check(extractor.Adopt(aborted), "adopt submitted replacement");
        extractor.Commit();
        Check(extractor.Build(aborted, delta) && delta.unchanged == 2 &&
                  delta.created == 0 && delta.destroyed.empty(),
              "fallback must resume from successfully adopted history");
        extractor.Commit();
        Check(!extractor.Adopt(duplicate), "adoption rejects duplicate identities");
        extractor.Commit();
        Check(extractor.Build(aborted, delta) && delta.unchanged == 2,
              "failed adoption cannot replace submitted history");
        extractor.Commit();

        extractor.Clear();
        instances = {Instance(0, 1.0f), Instance(0, 2.0f)};
        Check(extractor.Build(instances, delta) && delta.instances[0].key.fallback_ordinal == 1 &&
                  delta.instances[1].key.fallback_ordinal == 2,
              "anonymous identities use canonical source ordinals");
        extractor.Commit();

        std::puts("Render scene extractor tests passed.");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
