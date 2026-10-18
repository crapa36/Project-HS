#include "renderer_instance_order.hpp"
#include "render_instance_lookup.hpp"
#include "renderer_grass_benders.hpp"

#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

namespace
{
using namespace hs;
using Order = std::vector<std::uint32_t>;
using Ranges = std::array<std::pair<std::size_t, std::size_t>, kInstanceBucketCount>;

// Original per-asset scans, retained as an independent ordering reference.
std::pair<Order, Ranges> LegacyOrder(std::span<const RenderInstance> sources, float density)
{
    const auto monster = [](RenderMesh mesh) {
        return mesh >= RenderMesh::MonsterMelee && mesh <= RenderMesh::BossFinal;
    };
    const auto environment = [](RenderMesh mesh) {
        return (mesh >= RenderMesh::TreeTrunk && mesh <= RenderMesh::Grass) || mesh == RenderMesh::GroundDetail;
    };
    const auto environment_index = [](const RenderInstance &source) {
        if (source.mesh == RenderMesh::GroundDetail)
            return std::size_t{14} + source.environment_variant % kGroundDetailVariantCount;
        constexpr std::array<std::size_t, 4> offsets{0, 3, 6, 10}, counts{3, 3, 4, 4};
        const auto type = static_cast<std::size_t>(source.mesh) - static_cast<std::size_t>(RenderMesh::TreeTrunk);
        return offsets[type] + source.environment_variant % counts[type];
    };
    const auto terrain = [](RenderMesh mesh) { return mesh == RenderMesh::Ground || mesh == RenderMesh::DirtPatch; };
    Order order;
    Ranges ranges{};
    std::size_t bucket{};
    const auto scan = [&](auto predicate) {
        const auto begin = order.size();
        for (std::size_t index = 0; index < sources.size(); ++index)
            if (predicate(sources[index]) && RetainDecorativeFoliage(sources[index], index, density))
                order.push_back(static_cast<std::uint32_t>(index));
        ranges[bucket++] = {begin, order.size() - begin};
    };
    scan([](const auto &source) { return source.mesh == RenderMesh::Archer; });
    for (std::size_t asset = 0; asset < 6; ++asset)
        scan([&](const auto &source) {
            return monster(source.mesh) && static_cast<std::size_t>(source.mesh) -
                   static_cast<std::size_t>(RenderMesh::MonsterMelee) == asset;
        });
    scan([](const auto &source) { return source.mesh == RenderMesh::EnemyProjectile; });
    for (std::size_t asset = 0; asset < 14 + kGroundDetailVariantCount; ++asset)
        scan([&](const auto &source) { return environment(source.mesh) && environment_index(source) == asset; });
    scan([&](const auto &source) { return terrain(source.mesh); });
    scan([&](const auto &source) {
        return source.mesh != RenderMesh::Archer && !monster(source.mesh) && !environment(source.mesh) &&
               source.mesh != RenderMesh::EnemyProjectile && !terrain(source.mesh);
    });
    return {std::move(order), ranges};
}

void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void CheckOrder(std::span<const RenderInstance> sources, float density, Order &output)
{
    const auto expected = LegacyOrder(sources, density);
    const auto ranges = BuildRenderInstanceOrder(sources, density, output);
    Check(output == expected.first, "draw order or canonical source/foliage identity changed");
    Check(ranges == expected.second, "actor/environment/terrain prefix ranges changed");
    for (const auto [begin, count] : ranges)
        for (std::size_t index = begin + 1; count != 0 && index < begin + count; ++index)
            Check(output[index - 1] < output[index], "bucket is not stable in canonical input order");
}

void TestExplicitOrderAndReuse()
{
    std::array<RenderInstance, 10> scene{};
    constexpr std::array meshes{RenderMesh::Grass, RenderMesh::Area, RenderMesh::BossFinal,
        RenderMesh::Archer, RenderMesh::DirtPatch, RenderMesh::TreeCanopy,
        RenderMesh::EnemyProjectile, RenderMesh::MonsterMelee, RenderMesh::Archer, RenderMesh::Grass};
    for (std::size_t index = 0; index < scene.size(); ++index)
    {
        scene[index].mesh = meshes[index];
        scene[index].environment_variant = std::numeric_limits<std::uint32_t>::max();
        scene[index].stable_id = index % 2 ? index + 1 : 0;
    }
    scene[9].vfx_owner_id = 123; // Authored owner retains its grass at zero density.
    Order output{99, 98};
    CheckOrder(scene, 1.0f, output);
    Check(output == Order{3, 8, 7, 2, 6, 5, 0, 9, 4, 1}, "explicit asset bucket order changed");
    CheckOrder(scene, 0.0f, output);
    Check(output == Order{3, 8, 7, 2, 6, 5, 9, 4, 1}, "anonymous grass filtering or VFX-owner retention changed");
    CheckOrder({}, 0.35f, output); // Reusing storage must not retain the old view's references.
    Check(output.empty(), "empty scene retained stale references");
    CheckOrder(scene, 1.0f, output); // A -> empty B -> A, without cached identities.
    scene[0].mesh = static_cast<RenderMesh>(255);
    CheckOrder(scene, 0.35f, output); // Preserve the original other bucket behavior.
}

void TestGroundDetailOrder()
{
    std::vector<RenderInstance> scene;
    for (std::uint32_t variant = kGroundDetailVariantCount; variant-- > 0;)
        for (std::uint32_t duplicate = 0; duplicate < 2; ++duplicate)
        {
            RenderInstance source{};
            source.mesh = RenderMesh::GroundDetail; source.environment_variant = variant;
            source.stable_id = scene.size() + 1; scene.push_back(source);
        }
    Order order;
    CheckOrder(scene, 0.0f, order);
    Check(order.size() == scene.size(), "ground details were lost to grass-density filtering");
    for (std::size_t pair = 0; pair < kGroundDetailVariantCount; ++pair)
        Check(order[pair * 2] == scene.size() - 2 - pair * 2 &&
              order[pair * 2 + 1] == order[pair * 2] + 1,
              "new geometry families changed stable per-variant source order");
}

void TestRandomizedEquivalence()
{
    std::mt19937 random(0x48534102);
    Order output;
    for (std::size_t test = 0; test < 300; ++test)
    {
        std::vector<RenderInstance> scene(test == 0 ? 0 : random() % 1024);
        for (std::size_t index = 0; index < scene.size(); ++index)
        {
            auto &source = scene[index];
            source.mesh = static_cast<RenderMesh>(random() % (static_cast<std::uint32_t>(RenderMesh::GroundDetail) + 1));
            source.environment_variant = static_cast<std::uint32_t>(random());
            source.stable_id = index % 3 ? index + 1 : 0;
            source.vfx_owner_id = index % 7 ? 0 : index + 1;
        }
        for (const auto density : {0.0f, 0.35f, 0.75f, 1.0f}) CheckOrder(scene, density, output);
    }
}
void TestInstanceLookup()
{
    RenderInstanceLookup lookup;
    std::vector<RenderInstance> scene(4);
    scene[0].stable_id = scene[3].stable_id = 1;
    scene[1].stable_id = 2;
    lookup.Update(scene);
    Check(lookup.Find(1) == &scene[0] && lookup.Find(2) == &scene[1],
          "lookup must preserve first duplicate IDs");
    Check(!lookup.Find(0) && !lookup.Find(99), "lookup must skip anonymous and missing IDs");
    auto other_view = scene;
    other_view[0].position.x = 42;
    lookup.Update(other_view);
    Check(lookup.Find(1) == &other_view[0] && lookup.Find(1)->position.x == 42,
          "same identities in another view must refresh source storage");
    other_view[0].position.x = 17;
    lookup.Update(other_view);
    Check(lookup.Find(1)->position.x == 17, "unchanged IDs must read current transforms");
    std::ranges::reverse(other_view);
    lookup.Update(other_view);
    Check(lookup.Find(1) == &other_view[0] && lookup.Find(2) == &other_view[2],
          "reordered identities must refresh lookup indices");
    lookup.Update({});
    Check(!lookup.Find(1) && !lookup.Find(2), "session reset must remove prior pointers");
    lookup.Update(scene);
    Check(lookup.Find(1) == &scene[0], "new session must rebuild source mapping");
    std::mt19937 random(0x48534350);
    for (std::size_t frame = 0; frame < 200; ++frame)
    {
        scene.resize(random() % 100);
        std::unordered_map<std::uint64_t, const RenderInstance *> expected;
        for (auto &instance : scene)
        {
            instance.stable_id = random() % 50;
            if (instance.stable_id) expected.emplace(instance.stable_id, &instance);
        }
        lookup.Update(scene);
        for (std::uint64_t id = 0; id <= 50; ++id)
        {
            const auto found = expected.find(id);
            Check(lookup.Find(id) == (found == expected.end() ? nullptr : found->second),
                  "lookup churn differs from first-ID reference");
        }
    }
}

void TestGrassBenders()
{
    const auto instance = [](RenderMesh mesh, float x, std::uint64_t id = 0) {
        RenderInstance source{};
        source.mesh = mesh; source.position = {x, 2, 0}; source.stable_id = id;
        return source;
    };
    std::vector<RenderInstance> scene{instance(RenderMesh::Archer, 0),
        instance(RenderMesh::MonsterMelee, 3), instance(RenderMesh::MonsterRanged, 1),
        instance(RenderMesh::Grass, .25f)};
    auto benders = SelectGrassBenders(scene);
    Check(benders.count == 3 && benders.instances[0] == &scene[0] &&
          benders.instances[1] == &scene[2] && benders.instances[2] == &scene[1],
          "anonymous monsters must bend at their own positions");
    scene = {instance(RenderMesh::Archer, 0, 1), instance(RenderMesh::MonsterMelee, 2, 20),
        instance(RenderMesh::MonsterRanged, -2, 10), instance(RenderMesh::BossFinal, 1, 20),
        instance(RenderMesh::MonsterMelee, -2, 10)};
    benders = SelectGrassBenders(scene);
    Check(benders.count == 5 && benders.instances[1] == &scene[3] &&
          benders.instances[2] == &scene[2] && benders.instances[3] == &scene[4] &&
          benders.instances[4] == &scene[1], "distance/ID/source order must retain duplicate actors");
    scene.resize(1);
    for (int x = 40; x > 0; --x) scene.push_back(instance(RenderMesh::MonsterMelee, float(x)));
    benders = SelectGrassBenders(scene);
    Check(benders.count == 32 && benders.instances[1]->position.x == 1 &&
          benders.instances[31]->position.x == 31, "grass must retain nearest 31 monsters");
    scene.erase(scene.begin());
    Check(SelectGrassBenders(scene).count == 0 && SelectGrassBenders({}).count == 0,
          "missing player must disable grass benders");
    scene = {instance(RenderMesh::Archer, 5)};
    benders = SelectGrassBenders(scene);
    Check(benders.count == 1 && benders.instances[0] == &scene[0], "player-only grass bender");
}
} // namespace

int main()
{
    try
    {
        TestExplicitOrderAndReuse();
        TestGroundDetailOrder();
        TestRandomizedEquivalence();
        TestInstanceLookup();
        TestGrassBenders();
        std::cout << "instance preparation: ordering, retained lookup/churn and nearest grass benders passed\n";
        return 0;
    }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
