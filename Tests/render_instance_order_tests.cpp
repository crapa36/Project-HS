#include "renderer_instance_order.hpp"
#include "render_instance_lookup.hpp"
#include "renderer_grass_benders.hpp"
#include "renderer_geometry_demand.hpp"
#include "renderer_geometry.hpp"

#include <iostream>
#include <bit>
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
        return (mesh >= RenderMesh::TreeTrunk && mesh <= RenderMesh::Grass) || mesh == RenderMesh::GroundDetail || mesh == RenderMesh::MeadowSurface;
    };
    const auto environment_index = [](const RenderInstance &source) {
        if (source.mesh == RenderMesh::MeadowSurface)
            return std::size_t{14} + kGroundDetailVariantCount + source.environment_variant % 2u;
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
    for (std::size_t asset = 0; asset < 16 + kGroundDetailVariantCount; ++asset)
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

void TestMeadowCatalogOrder()
{
    std::array<RenderInstance, 4> scene{};
    scene[0].mesh=RenderMesh::MeadowSurface; scene[0].environment_variant=1;
    scene[1].mesh=RenderMesh::GroundDetail; scene[1].environment_variant=18;
    scene[2].mesh=RenderMesh::MeadowSurface;
    scene[3].mesh=RenderMesh::Ground;
    Order order; CheckOrder(scene, 0.0f, order);
    Check(order==Order{1,2,0,3}, "new props, meadow ground/water and fallback terrain buckets overlap");
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
void TestRasterIndexSuffix()
{
    // Duplicate triangles remain distinct canonical primitives. Each material
    // owns two primitives; remapping only permutes within those subranges.
    const std::array<std::uint32_t, 12> canonical{0,1,2, 0,1,2, 3,4,5, 5,6,3};
    const std::array<std::uint32_t, 4> remap{1,0,3,2};
    std::array<std::uint32_t, 24> storage{};
    std::copy(canonical.begin(), canonical.end(), storage.begin());
    const auto suffix = std::as_writable_bytes(std::span(storage).subspan(12));
    Check(WriteGeometryRasterIndexSuffix(suffix, std::as_bytes(std::span(canonical)),
        std::as_bytes(std::span(remap))), "raster suffix construction");
    Check(std::equal(canonical.begin(), canonical.end(), storage.begin()), "canonical prefix changed");
    for (std::size_t primitive = 0; primitive < remap.size(); ++primitive)
    {
        Check(remap[primitive] / 2 == primitive / 2, "material subrange changed");
        for (std::size_t corner = 0; corner < 3; ++corner)
            Check(storage[12 + primitive * 3 + corner] == canonical[remap[primitive] * 3 + corner],
                "raster order must preserve canonical triangle corners");
    }
    Check(!WriteGeometryRasterIndexSuffix(suffix.first(4), std::as_bytes(std::span(canonical)),
        std::as_bytes(std::span(remap))), "short suffix accepted");
    Check(!WriteGeometryRasterIndexSuffix(suffix, std::as_bytes(std::span(canonical)), {}), "missing remap accepted");
    const std::array<std::uint32_t, 4> invalid{4,0,1,2};
    Check(!WriteGeometryRasterIndexSuffix(suffix, std::as_bytes(std::span(canonical)),
        std::as_bytes(std::span(invalid))), "out-of-range primitive accepted");
}

void TestGeometryDemandCells()
{
    std::vector<GeometryDemandPoint> points{{{-0.1f, 0, 30}, 2, 0, false},
        {{0, 0, 30}, 1, 1, true}, {{1, 2, 31}, 3, 2, false},
        {{0, 0, 30}, std::numeric_limits<float>::infinity(), 3, false}};
    auto cells = BuildGeometryDemandCells(points);
    Check(cells.size() == 3 && cells[0].instances == Order{0} &&
        cells[1].instances == Order({1, 2}) && cells[1].max_scale == 3 && cells[1].any_wind &&
        cells[2].instances == Order{3}, "demand cell membership/negative coordinate/fallback");
    struct Plane { float x, y, z, w; };
    const std::array planes{Plane{1,0,0,8}, Plane{-1,0,0,8}, Plane{0,0,1,-1}};
    float view[4][4]{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
    Check(ClassifyGeometryDemandCell(cells[2], 1, planes) == GeometryDemandCellVisibility::Partial &&
        std::isinf(GeometryDemandCellErrorUpperBound(cells[2], 1, 1, view, 1000, .1f)),
        "invalid inputs must remain unprunable");
    const GeometryDemandCell near_cell{{0, 0, .05f}, {1, 1, .05f}, 1, false, {0, 1}};
    Check(std::isinf(GeometryDemandCellErrorUpperBound(near_cell, 0, 0, view, 1000, .1f)),
        "zero error still requires the near-branch fallback");
    std::mt19937 random(931);
    std::uniform_real_distribution<float> coordinate(-1000, 1000), positive(0, 10);
    std::uniform_real_distribution<float> within_cell(.25f, 15.75f);
    for (unsigned trial = 0; trial < 300; ++trial)
    {
        points.clear();
        for (unsigned i = 0; i < 60; ++i)
        {
            const float base = 16.0f * (static_cast<int>(i % (2 + trial % 2)) - 1);
            const std::array<float, 3> position = i < 30
                ? std::array<float, 3>{base + within_cell(random), base + within_cell(random), 32 + base + within_cell(random)}
                : std::array<float, 3>{coordinate(random), coordinate(random), coordinate(random)};
            points.push_back({position, positive(random), i, (i & 1) != 0});
        }
        cells = BuildGeometryDemandCells(points);
        Check(std::ranges::any_of(cells, [](const auto &cell) { return cell.instances.size() >= 10; }),
            "generated scene must exercise multi-member AABBs");
        const float angle = coordinate(random), c = std::cos(angle), s = std::sin(angle);
        view[0][0] = c; view[0][2] = s; view[2][0] = -s; view[2][2] = c;
        view[3][0] = coordinate(random); view[3][1] = coordinate(random); view[3][2] = coordinate(random);
        // Reuse the same membership with changed current catalog/grass bounds.
        for (unsigned catalog = 0; catalog < 2; ++catalog)
        {
        const float catalog_radius = positive(random), catalog_error = trial % 3 ? positive(random) : 0;
        const float wind_margin = catalog ? 4.0f : .25f;
        std::array<std::array<float, 60>, 2> references{};
        std::vector<double> bounds;
        for (const auto &cell : cells)
        {
            const float radius = catalog_radius * cell.max_scale + (cell.any_wind ? wind_margin : 0);
            const float error = catalog_error * cell.max_scale;
            const auto visibility = ClassifyGeometryDemandCell(cell, radius, planes);
            const double bound = GeometryDemandCellErrorUpperBound(cell, radius, error, view, 1000, .1f);
            bounds.push_back(bound);
            for (const auto id : cell.instances)
            {
                const auto &p = points[id].position;
                const float point_radius = catalog_radius * points[id].scale + (points[id].wind ? wind_margin : 0);
                const float point_error = catalog_error * points[id].scale;
                bool outside = false, inside = true;
                for (const auto &plane : planes)
                {
                    const float dot = std::fma(p[0], plane.x, std::fma(p[1], plane.y, std::fma(p[2], plane.z, plane.w)));
                    outside |= dot < -point_radius; inside &= dot >= 0;
                }
                Check(visibility != GeometryDemandCellVisibility::Outside || outside, "false cell outside");
                Check(visibility != GeometryDemandCellVisibility::Inside || inside, "false cell inside");
                for (const bool fused : {false, true})
                {
                    float v[3]{};
                    for (unsigned axis = 0; axis < 3; ++axis)
                        v[axis] = fused ? std::fma(p[0], view[0][axis], std::fma(p[1], view[1][axis], std::fma(p[2], view[2][axis], view[3][axis]))) :
                            ((p[0] * view[0][axis] + p[1] * view[1][axis]) + p[2] * view[2][axis]) + view[3][axis];
                    const float distance = v[2] - point_radius - point_error;
                    if (distance <= .1f)
                    {
                        Check(std::isinf(bound), "near branch must not prune");
                        references[fused][id] = 2.0f * 1.25f;
                        continue;
                    }
                    const float rho = std::hypot(std::abs(v[0]) + point_radius + point_error, std::abs(v[1]) + point_radius + point_error) / distance;
                    const float reference = point_error * 1000 * std::sqrt(1.0f + rho * rho) / distance;
                    references[fused][id] = reference;
                    Check(reference <= bound, "cell error bound underestimated float source");
                }
            }
        }
        for (const auto &reference : references)
        {
            const float direct = *std::max_element(reference.begin(), reference.end());
            float pruned{};
            for (std::size_t cell = 0; cell < cells.size(); ++cell)
                if (bounds[cell] > pruned)
                    for (const auto id : cells[cell].instances) pruned = std::max(pruned, reference[id]);
            Check(std::bit_cast<std::uint32_t>(direct) == std::bit_cast<std::uint32_t>(pruned),
                "cell pruning changed the exact demand maximum");
        }
        }
    }
}
} // namespace

int main()
{
    try
    {
        TestExplicitOrderAndReuse();
        TestGroundDetailOrder();
        TestMeadowCatalogOrder();
        TestRandomizedEquivalence();
        TestInstanceLookup();
        TestGrassBenders();
        TestGeometryDemandCells();
        TestRasterIndexSuffix();
        std::cout << "instance preparation: ordering, retained lookup/churn and nearest grass benders passed\n";
        return 0;
    }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
