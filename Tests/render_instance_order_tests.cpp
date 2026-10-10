#include "renderer_instance_order.hpp"

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
        return mesh >= RenderMesh::TreeTrunk && mesh <= RenderMesh::Grass;
    };
    const auto environment_index = [](const RenderInstance &source) {
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
    for (std::size_t asset = 0; asset < 14; ++asset)
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
            source.mesh = static_cast<RenderMesh>(random() % (static_cast<std::uint32_t>(RenderMesh::DirtPatch) + 1));
            source.environment_variant = static_cast<std::uint32_t>(random());
            source.stable_id = index % 3 ? index + 1 : 0;
            source.vfx_owner_id = index % 7 ? 0 : index + 1;
        }
        for (const auto density : {0.0f, 0.35f, 0.75f, 1.0f}) CheckOrder(scene, density, output);
    }
}
} // namespace

int main()
{
    try
    {
        TestExplicitOrderAndReuse();
        TestRandomizedEquivalence();
        std::cout << "instance order: explicit/reuse and 1200 randomized order/range checks passed\n";
        return 0;
    }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
