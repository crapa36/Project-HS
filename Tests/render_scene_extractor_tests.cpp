#include "render_scene_extractor.hpp"

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
} // namespace

int main()
{
    try
    {
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
