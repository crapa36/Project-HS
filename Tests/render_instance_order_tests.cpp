#include "renderer_instance_order.hpp"
#include <hs/render_data/render_instance_lookup.hpp>
#include "renderer_grass_benders.hpp"
#include "renderer_geometry_demand.hpp"
#include "renderer_geometry.hpp"
#include "renderer_vfx_compatibility.hpp"

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

void TestOwnedVfxReplacementEquivalence()
{
    std::mt19937 random(8712);
    constexpr std::array shapes{VfxGroundShape::Circle,VfxGroundShape::LineBorder,
        VfxGroundShape::ConeBorder,VfxGroundShape::RingGapsBorder,
        VfxGroundShape::RingGapsPreviewBorder,VfxGroundShape::CirclePreviewBorder};
    constexpr std::array meshes{RenderMesh::Area,RenderMesh::PlayerProjectile,
        RenderMesh::EnemyProjectile,RenderMesh::Archer,RenderMesh::Grass};
    std::vector<std::pair<std::uint64_t,std::uint8_t>> scratch;
    for(unsigned trial=0;trial<64;++trial)
    {
        std::vector<RenderInstance> sources(128);
        for(std::size_t index=0;index<sources.size();++index)
        {
            auto &instance=sources[index];
            instance.mesh=meshes[random()%meshes.size()];
            instance.stable_id=random()%32;instance.vfx_owner_id=random()%32;
            instance.position.x=static_cast<float>(index);
        }
        sources[0].mesh = RenderMesh::PlayerProjectile;
        sources[0].stable_id = 101;
        sources[1].mesh = RenderMesh::Grass;
        sources[1].stable_id = sources[1].vfx_owner_id = 0;
        std::vector<VfxGroundSpawnInput> ground(trial%17);
        for(auto &spawn:ground)
        {
            spawn.stable_id=random()%32;spawn.geometry.shape=shapes[random()%shapes.size()];
            spawn.source_visual_kind=(random()&1)?0xff:1;
            spawn.command.primitive=(random()&1)?VfxPrimitive::LowFrequencyFill:VfxPrimitive::Rune;
        }
        std::vector<VfxOwnedMeshInput> heads(trial%13);
        for(auto &head:heads)head.owner_id=random()%32;
        heads.emplace_back().owner_id = 101;
        auto expected=sources;
        std::erase_if(expected,[&](const RenderInstance &instance) {
            return instance.mesh==RenderMesh::Area&&instance.stable_id!=0&&
                std::ranges::any_of(ground,[&](const auto &spawn) {
                    return spawn.source_visual_kind!=0xff&&spawn.stable_id==instance.stable_id&&
                        spawn.geometry.shape==VfxGroundShape::Circle&&spawn.command.primitive==VfxPrimitive::LowFrequencyFill;
                });
        });
        std::erase_if(expected,[&](const RenderInstance &instance) {
            return instance.mesh==RenderMesh::Area&&instance.vfx_owner_id!=0&&
                std::ranges::any_of(ground,[&](const auto &spawn) {
                    return spawn.stable_id==instance.vfx_owner_id&&
                        ((spawn.source_visual_kind==0xff&&(spawn.geometry.shape==VfxGroundShape::LineBorder||
                            spawn.geometry.shape==VfxGroundShape::ConeBorder))||
                         spawn.geometry.shape==VfxGroundShape::RingGapsBorder||
                         spawn.geometry.shape==VfxGroundShape::RingGapsPreviewBorder||
                         spawn.geometry.shape==VfxGroundShape::CirclePreviewBorder);
                });
        });
        std::erase_if(expected,[&](const RenderInstance &instance) {
            return (instance.mesh==RenderMesh::PlayerProjectile||instance.mesh==RenderMesh::EnemyProjectile)&&
                std::ranges::any_of(heads,[&](const auto &head){return head.owner_id==instance.stable_id;});
        });
        std::vector<std::uint32_t> ordinals(sources.size());
        for (std::size_t index=0;index<ordinals.size();++index) ordinals[index]=static_cast<std::uint32_t>(index);
        const auto original=sources;
        FilterRenderInstancesForOwnedVfx(sources,ground,heads,scratch,&ordinals);
        Check(sources.size()==expected.size(),"indexed VFX filtering changed survivor count");
        for(std::size_t index=0;index<sources.size();++index)
        {
            Check(sources[index].position.x==expected[index].position.x,
                "indexed VFX filtering changed owner matching or stable survivor order");
            Check(original[ordinals[index]].position.x==sources[index].position.x,
                "VFX filtering changed the original source ordinal");
        }
        for (const auto density : {0.0f, 0.5f, 1.0f})
        {
            auto expected_order = LegacyOrder(original, density).first;
            std::erase_if(expected_order, [&](auto index) {
                return !std::ranges::binary_search(ordinals, index);
            });
            std::vector<std::uint32_t> order, lod_scratch;
            const std::vector<std::uint16_t> lods(sources.size());
            Check(BuildRenderInstanceOrderWithMonsterLods(
                      sources, lods, density, order, lod_scratch, ordinals).has_value(),
                  "filtered source LOD ordering failed");
            for (auto &index : order) index = ordinals[index];
            Check(order == expected_order,
                  "VFX replacement changed surviving foliage identity or draw order");
        }
    }
}

void TestVfxOwnedMeshDrawRunsPreserveCommandOrder()
{
    using namespace hs;
    using namespace hs::renderer_detail;
    constexpr std::array<std::uint32_t, 4> vertex_counts{12, 12, 24, 48};
    std::array<VfxMeshRecipe, 4> recipe_rows{};
    for (std::size_t index = 0; index < recipe_rows.size(); ++index)
    {
        recipe_rows[index].effect_handle = 1;
        recipe_rows[index].mesh_index = static_cast<std::uint32_t>(index + 1);
        recipe_rows[index].hdr = 1.0f;
    }
    PreparedVfxMeshRecipeView recipes;
    Check(PreparedVfxMeshRecipeView::Create(recipe_rows, recipes).Succeeded(),
          "owned mesh recipe fixture admission failed");
    const auto command = [](std::uint32_t mesh_index) {
        VfxOwnedMeshInput input;
        input.recipe_index = mesh_index - 1;
        return input;
    };
    const auto verify = [&](std::span<const VfxOwnedMeshInput> commands,
                            std::span<const std::pair<std::size_t, std::uint32_t>> expected_runs) {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> original_draws;
        for (const auto &input : commands)
        {
            const auto mesh_index = recipes.Rows()[input.recipe_index].mesh_index;
            original_draws.emplace_back(mesh_index, vertex_counts[mesh_index - 1]);
        }

        std::vector<std::pair<std::uint32_t, std::uint32_t>> batched_draws;
        std::size_t first{};
        std::size_t run_index{};
        while (first < commands.size())
        {
            const auto run = NextVfxOwnedMeshDrawRun(commands, recipes, vertex_counts, first);
            Check(run_index < expected_runs.size() && run.first == first && run.count != 0 &&
                      run.vertex_count == expected_runs[run_index].second &&
                      run.count == expected_runs[run_index].first,
                  "owned mesh run boundaries or native vertex count changed");
            for (std::size_t index = run.first; index < run.first + run.count; ++index)
                batched_draws.emplace_back(recipes.Rows()[commands[index].recipe_index].mesh_index,
                                           run.vertex_count);
            first += run.count;
            ++run_index;
        }
        Check(run_index == expected_runs.size() && batched_draws == original_draws,
              "owned mesh batching reordered or changed per-row mesh draws");
        const auto end = NextVfxOwnedMeshDrawRun(commands, recipes, vertex_counts, commands.size());
        Check(end.first == commands.size() && end.count == 0 && end.vertex_count == 0,
              "owned mesh draw run at end boundary was not empty");
    };

    const std::vector<VfxOwnedMeshInput> empty;
    Check(NextVfxOwnedMeshDrawRun(empty, recipes, vertex_counts, 0).count == 0,
          "empty owned mesh input produced a draw run");
    const std::array<std::pair<std::size_t, std::uint32_t>, 0> no_runs{};
    verify(empty, no_runs);

    const std::array same_count{command(1), command(2), command(2), command(1)};
    const std::array<std::pair<std::size_t, std::uint32_t>, 1> same_run{{{4, 12}}};
    verify(same_count, same_run);

    const std::vector<VfxOwnedMeshInput> crowded(200, command(1));
    const std::array<std::pair<std::size_t, std::uint32_t>, 1> crowded_run{{{200, 12}}};
    verify(crowded, crowded_run);

    const std::array alternating{command(1), command(2), command(3), command(4), command(2)};
    const std::array<std::pair<std::size_t, std::uint32_t>, 4> alternating_runs{{
        {2, 12}, {1, 24}, {1, 48}, {1, 12}}};
    verify(alternating, alternating_runs);

    const auto last = NextVfxOwnedMeshDrawRun(alternating, recipes, vertex_counts, alternating.size() - 1);
    Check(last.first == alternating.size() - 1 && last.count == 1 && last.vertex_count == 12,
          "owned mesh final single-row run changed");
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
    const float isotropic=ConservativeGroundTextureFootprint(1,0,0,1);
    const float grazing=ConservativeGroundTextureFootprint(100,0,0,0.01);
    Check(isotropic>0.70f && isotropic<0.71f && grazing>0.009f && grazing<=0.01f &&
        ConservativeGroundTextureFootprint(1,1,2,2)==0 &&
        ConservativeGroundTextureFootprint(std::numeric_limits<double>::infinity(),0,0,1)==0,
        "terrain texture footprint conservatively bounds anisotropic and degenerate gradients");
    std::vector<GeometryDemandPoint> points{{{-0.1f, 0, 30}, 2, 0, false},
        {{0, 0, 30}, 1, 1, true}, {{1, 2, 31}, 3, 2, false},
        {{0, 0, 30}, std::numeric_limits<float>::infinity(), 3, false}};
    auto cells = BuildGeometryDemandCells(points);
    Check(cells.size() == 3 && cells[0].points.size() == 1 && cells[0].points[0].instance == 0 &&
        cells[1].points.size() == 2 && cells[1].points[0].instance == 1 && cells[1].points[1].instance == 2 &&
        cells[1].points[1].position == points[2].position && cells[1].points[1].scale == points[2].scale &&
        cells[1].points[1].wind == points[2].wind && cells[1].max_scale == 3 && cells[1].any_wind &&
        cells[2].points.size() == 1 && cells[2].points[0].instance == 3,
        "demand cells retain ordered static point payload/negative coordinate/fallback");
    struct Plane { float x, y, z, w; };
    const std::array planes{Plane{1,0,0,8}, Plane{-1,0,0,8}, Plane{0,0,1,-1}};
    float view[4][4]{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
    const auto initial_view = PrepareGeometryDemandView(planes, view, 1000, .1f);
    Check(ClassifyGeometryDemandCell(cells[2], 1, initial_view) == GeometryDemandCellVisibility::Partial &&
        std::isinf(GeometryDemandCellErrorUpperBound(cells[2], 1, 1, initial_view)),
        "invalid inputs must remain unprunable");
    const GeometryDemandCell near_cell{{0, 0, .05f}, {1, 1, .05f}, 1, false, {}};
    const auto built_near = BuildGeometryDemandCells(std::array{GeometryDemandPoint{{0, 0, .05f}, 1, 0, false}});
    Check(std::isinf(GeometryDemandCellErrorUpperBound(built_near[0], 0, 0, initial_view)),
        "zero error still requires the near-branch fallback");
    auto invalid_planes = planes;
    invalid_planes[0].x = std::numeric_limits<float>::quiet_NaN();
    const auto invalid_view = PrepareGeometryDemandView(invalid_planes, view, 1000, .1f);
    Check(ClassifyGeometryDemandCell(cells[0], 1, invalid_view) == GeometryDemandCellVisibility::Partial &&
        ClassifyGeometryDemandCell(near_cell, 1, initial_view) == GeometryDemandCellVisibility::Partial,
        "invalid prepared planes or unprepared bounds cannot prune");
    view[0][3] = 1;
    const auto non_affine = PrepareGeometryDemandView(planes, view, 1000, .1f);
    Check(std::isinf(GeometryDemandCellErrorUpperBound(cells[0], 1, 1, non_affine)),
        "non-affine view cannot prune the per-point path");
    view[0][3] = 0;
    view[2][0] = std::numeric_limits<float>::infinity();
    const auto nonfinite_view = PrepareGeometryDemandView(planes, view, 1000, .1f);
    Check(std::isinf(GeometryDemandCellErrorUpperBound(cells[0], 1, 1, nonfinite_view)),
        "nonfinite view coefficients cannot prune the per-point path");
    view[2][0] = 0;
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
        Check(std::ranges::any_of(cells, [](const auto &cell) { return cell.points.size() >= 10; }),
            "generated scene must exercise multi-member AABBs");
        const float angle = coordinate(random), c = std::cos(angle), s = std::sin(angle);
        view[0][0] = c; view[0][2] = s; view[2][0] = -s; view[2][2] = c;
        view[3][0] = coordinate(random); view[3][1] = coordinate(random); view[3][2] = coordinate(random);
        const auto prepared_view = PrepareGeometryDemandView(planes, view, 1000, .1f);
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
            const auto visibility = ClassifyGeometryDemandCell(cell, radius, prepared_view);
            const double bound = GeometryDemandCellErrorUpperBound(cell, radius, error, prepared_view);
            bounds.push_back(bound);
            for (const auto &point : cell.points)
            {
                const auto id = point.instance;
                const auto &source_point = points[id];
                Check(point.position == source_point.position && point.scale == source_point.scale &&
                    point.wind == source_point.wind, "demand cell point payload must match original input");
                const auto &p = source_point.position;
                const float point_radius = catalog_radius * source_point.scale + (source_point.wind ? wind_margin : 0);
                const float point_error = catalog_error * source_point.scale;
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
                    for (const auto &point : cells[cell].points) pruned = std::max(pruned, reference[point.instance]);
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
        TestOwnedVfxReplacementEquivalence();
        TestVfxOwnedMeshDrawRunsPreserveCommandOrder();
        TestGrassBenders();
        TestGeometryDemandCells();
        TestRasterIndexSuffix();
        std::cout << "instance preparation: ordering, retained lookup/churn and nearest grass benders passed\n";
        return 0;
    }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
