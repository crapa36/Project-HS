#include "geometry_cooker.hpp"
#include "scene_hlod_cooker.hpp"
#include <hs/presentation/environment_layout.hpp>
#include <hs/render_data/material_schema.hpp>
#include <hs/render_data/material_descriptor.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cfenv>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>

namespace gc = hs::geometry_content;
namespace rd = hs::render_data;
void TestAssetActionSnapshot();
namespace
{
void Check(bool condition, const char *reason)
{
    if (!condition) throw std::runtime_error(reason);
}
rd::GeometryData Grid(bool nonplanar = false)
{
    std::vector<rd::ExpandedVertex> vertices;
    const auto vertex = [nonplanar](unsigned x, unsigned z) {
        rd::ExpandedVertex v{};
        v.position = {float(x), nonplanar ? 0.03f * float(x * z) : 0.0f, float(z)};
        v.normal = {0, 1, 0}; v.tangent = {1, 0, 0, 1};
        v.uv = {float(x) / 8, float(z) / 8};
        return v;
    };
    for (unsigned z = 0; z < 8; ++z)
        for (unsigned x = 0; x < 8; ++x)
            for (auto v : {vertex(x, z), vertex(x, z + 1), vertex(x + 1, z),
                           vertex(x + 1, z), vertex(x, z + 1), vertex(x + 1, z + 1)})
                vertices.push_back(v);
    return gc::CookGeometry(vertices, 0);
}
void TestCertifiedTriangleDistanceBound()
{
    // The same square with opposite diagonals has zero geometric error even
    // though none of its two source faces is byte-identical to a target face.
    // A centroid-plus-radius bound previously subdivided this repeatedly.
    const auto vertex = [](float x, float y, float z) {
        rd::ExpandedVertex v{};
        v.position = {x, y, z};
        v.normal = {0, 1, 0};
        v.tangent = {1, 0, 0, 1};
        return v;
    };
    const auto a = vertex(0, 0, 0);
    const auto b = vertex(2, 0, 0);
    const auto c = vertex(0, 0, 2);
    const auto d = vertex(2, 0, 2);
    const std::array fine{a, b, c, b, d, c};
    auto coarse = std::array{a, b, d, a, d, c};
    const auto coplanar = gc::MeasureGeometryError(fine, coarse);
    Check(std::isfinite(coplanar.lower) && std::isfinite(coplanar.upper) &&
              coplanar.lower >= 0 && coplanar.lower <= 1e-5 &&
              coplanar.upper >= coplanar.lower && coplanar.upper <= 0.015,
          "opposite-diagonal planar triangles require a tight certified zero-error bound");

    // Parallel, congruent squares have exactly 0.25m symmetric Hausdorff
    // distance. Both lower and upper must enclose that analytic answer.
    for (auto &v : coarse) v.position[1] += 0.25f;
    const auto displaced = gc::MeasureGeometryError(fine, coarse);
    Check(displaced.lower <= 0.25 + 1e-5 &&
              displaced.upper >= 0.25 - 1e-5 &&
              displaced.upper - displaced.lower < 0.015,
          "convex-triangle bound must enclose a real displaced-surface error");
}
void TestAnalyticErrorSurfaces()
{
    const auto triangle = [](std::vector<rd::ExpandedVertex> &surface, float x, float y,
                             std::uint16_t material) {
        for (const auto position : {std::array{x, y, 0.0f}, std::array{x + 1, y, 0.0f},
                                    std::array{x, y, 1.0f}})
        {
            rd::ExpandedVertex vertex{};
            vertex.position = position; vertex.material_index = material;
            surface.push_back(vertex);
        }
    };
    const auto encloses = [](const auto &fine, const auto &coarse, double exact) {
        const auto bounds = gc::MeasureGeometryError(fine, coarse);
        Check(std::isfinite(bounds.lower) && std::isfinite(bounds.upper) &&
                  bounds.lower >= 0 && bounds.lower <= exact && exact <= bounds.upper &&
                  bounds.upper - bounds.lower < 0.01,
              "analytic surface distance must be enclosed by a tight finite bound");
    };
    std::vector<rd::ExpandedVertex> fine, coarse;
    // Twelve separated patches per material force internal BVH nodes. Each
    // projected triangle remains congruent, so the largest vertical offset is
    // the exact symmetric distance, independent of traversal/triangle order.
    for (unsigned i = 0; i < 12; ++i)
    {
        triangle(fine, float(i * 4), 0, 0);
        triangle(coarse, float(i * 4), (i & 1) ? 0.25f : 0.5f, 0);
    }
    encloses(fine, coarse, 0.5);
    std::reverse(coarse.begin(), coarse.end());
    encloses(fine, coarse, 0.5);
    encloses(coarse, fine, 0.5);

    fine.clear(); coarse.clear();
    // The unlabelled surfaces are identical, but each material moves two
    // metres. A global nearest-surface query would incorrectly return zero.
    for (unsigned i = 0; i < 12; ++i)
        for (std::uint16_t material = 0; material < 2; ++material)
        {
            triangle(fine, float(i * 4), float(material * 2), material);
            triangle(coarse, float(i * 4), float((1 - material) * 2), material);
        }
    encloses(fine, coarse, 2);
    std::reverse(coarse.begin(), coarse.end());
    encloses(fine, coarse, 2);

    // Degenerate closed triangles represent a segment and an isolated point.
    // Both translate perpendicular to their support by exactly 0.75 metres.
    fine.clear(); coarse.clear();
    for (const float x : {0.0f, 1.0f, 2.0f, 8.0f, 8.0f, 8.0f})
    {
        rd::ExpandedVertex vertex{}; vertex.position = {x, 0, 0};
        fine.push_back(vertex);
        vertex.position[1] = 0.75f; coarse.push_back(vertex);
    }
    encloses(fine, coarse, 0.75);
    std::reverse(coarse.begin(), coarse.end());
    encloses(fine, coarse, 0.75);
    encloses(coarse, fine, 0.75);
}
void TestGeometryErrorQualificationFallback()
{
    std::vector<rd::ExpandedVertex> fine, coarse;
    for (unsigned i = 0; i < 25; ++i)
        for (const auto position : {std::array{float(i * 4), 0.0f, 0.0f},
                                   std::array{float(i * 4 + 1), 0.0f, 0.0f},
                                   std::array{float(i * 4), 0.0f, 1.0f}})
        {
            rd::ExpandedVertex vertex{}; vertex.position = position; vertex.material_index = static_cast<std::uint16_t>(i % 2);
            fine.push_back(vertex); vertex.position[1] = .5f; coarse.push_back(vertex);
        }
    const auto normal = gc::MeasureGeometryError(fine, coarse);
    Check(normal.oracle_queries == 24 && !normal.qualification_used &&
              normal.lower <= .5 && normal.upper >= .5,
          "ordinary cook without GT09 evidence must retain twelve oracle queries in each direction");
    // This synthetic record tests dispatch policy; it does not qualify the
    // implementation's continuous input domain for production use.
    gc::GeometryErrorQualification record{gc::GeometryErrorImplementationKey(), 200, .5, .5, 50, false};
    const bool eligible_build = record.key.compiler && record.key.target &&
        record.key.floating_point_contract == 1 && record.key.rounding_mode == FE_TONEAREST;
    const auto measured = gc::MeasureGeometryError(fine, coarse, {}, 4096,
        {gc::GeometryErrorCheckMode::Normal, &record});
    Check(measured.qualification_used == eligible_build &&
              measured.oracle_queries == (eligible_build ? 0 : 24) &&
              measured.lower == normal.lower && measured.upper == normal.upper,
          "qualification may suppress only the duplicate oracle within the exact implementation/domain");
    const auto fallback = [&](gc::GeometryErrorQualification mismatch) {
        const auto result = gc::MeasureGeometryError(fine, coarse, {}, 4096,
            {gc::GeometryErrorCheckMode::Normal, &mismatch});
        Check(result.oracle_queries == 24 && !result.qualification_used &&
                  result.lower == normal.lower && result.upper == normal.upper,
              "stale qualification or uncovered numeric input must use the unchanged sampled oracle");
    };
    auto mismatch = record; ++mismatch.key.bvh_abi; fallback(mismatch);
    mismatch = record; ++mismatch.key.distance_abi; fallback(mismatch);
    mismatch = record; ++mismatch.key.geometry_format_abi; fallback(mismatch);
    mismatch = record; ++mismatch.key.material_policy; fallback(mismatch);
    mismatch = record; ++mismatch.key.compiler; fallback(mismatch);
    mismatch = record; ++mismatch.key.target; fallback(mismatch);
    mismatch = record; ++mismatch.key.compiler_version; fallback(mismatch);
    mismatch = record; ++mismatch.key.floating_point_contract; fallback(mismatch);
    mismatch = record; ++mismatch.key.rounding_mode; fallback(mismatch);
    mismatch = record; mismatch.maximum_absolute_coordinate = 1; fallback(mismatch);
    mismatch = record; mismatch.maximum_triangles_per_surface = 24; fallback(mismatch);
    mismatch = record; mismatch.minimum_positive_edge_squared = 2; fallback(mismatch);
    mismatch = record; mismatch.minimum_positive_area_squared = 2; fallback(mismatch);
    const auto exhaustive = gc::MeasureGeometryError(fine, coarse, {}, 4096,
        {gc::GeometryErrorCheckMode::Exhaustive, &record});
    Check(!exhaustive.qualification_used && exhaustive.oracle_queries == 7 * 50 &&
              exhaustive.lower == normal.lower && exhaustive.upper == normal.upper,
          "explicit exhaustive mode must check every centroid plus vertex/edge corpus despite qualification");
    std::vector<rd::ExpandedVertex> line(fine.begin(), fine.begin() + 3), shifted;
    line[2].position = {2, 0, 0}; shifted = line;
    for (auto &vertex : shifted) vertex.position[1] = .5f;
    const auto degenerate = gc::MeasureGeometryError(line, shifted, {}, 4096,
        {gc::GeometryErrorCheckMode::Normal, &record});
    Check(degenerate.oracle_queries == 2 && !degenerate.qualification_used &&
              degenerate.lower <= .5 && degenerate.upper >= .5,
          "zero-area geometry outside a qualification must retain the segment/point oracle fallback");
    record.includes_degenerate_triangles = true;
    const auto supported_degenerate = gc::MeasureGeometryError(line, shifted, {}, 4096,
        {gc::GeometryErrorCheckMode::Normal, &record});
    Check(supported_degenerate.qualification_used == eligible_build &&
              supported_degenerate.oracle_queries == (eligible_build ? 0 : 2) &&
              supported_degenerate.lower == degenerate.lower && supported_degenerate.upper == degenerate.upper,
          "degenerate oracle skipping requires the qualification's explicit degenerate domain");
    for (auto &vertex : shifted) vertex.material_index = 7;
    bool missing_material = false;
    try { (void)gc::MeasureGeometryError(line, shifted); }
    catch (const std::runtime_error &error)
    { missing_material = std::string_view(error.what()) == "authored LOD removes a complete material surface"; }
    Check(missing_material, "fallback cannot waive complete material surface validation");
}
void TestDuplicatePrimitiveOrder()
{
    const std::array positions{std::array{0.0f, 0.0f, 0.0f}, std::array{1.0f, 0.0f, 0.0f},
                               std::array{0.0f, 0.0f, 1.0f}, std::array{1.0f, 0.0f, 1.0f}};
    const std::array triangles{std::array{0u, 1u, 2u}, std::array{0u, 1u, 3u},
                               std::array{0u, 1u, 2u}, std::array{0u, 2u, 1u},
                               std::array{0u, 1u, 2u}};
    std::vector<rd::ExpandedVertex> authored;
    for (std::uint16_t material = 0; material < 2; ++material)
        for (const auto &triangle : triangles)
            for (const auto index : triangle)
            {
                rd::ExpandedVertex vertex{}; vertex.position = positions[index];
                vertex.normal = {0, 1, 0}; vertex.tangent = {1, 0, 0, 1};
                vertex.material_index = material; authored.push_back(vertex);
            }
    const auto geometry = gc::CookGeometry(authored, 0);
    Check(geometry.submeshes.size() == 2, "duplicate fixture must retain both material runs");
    std::array<bool, 10> seen{};
    std::map<std::array<std::uint32_t, 3>, std::uint32_t> previous;
    for (const auto &meshlet : geometry.meshlets)
        for (std::uint32_t i = 0; i < meshlet.triangle_count; ++i)
        {
            const auto triangle_index = meshlet.first_triangle + i;
            const auto packed = geometry.meshlet_triangles[triangle_index];
            const auto primitive = geometry.canonical_primitive_remap[triangle_index];
            Check(primitive < seen.size() && !seen[primitive], "primitive remap must be a permutation");
            seen[primitive] = true;
            std::array<std::uint32_t, 3> key{};
            for (unsigned lane = 0; lane < 3; ++lane)
            {
                key[lane] = geometry.meshlet_vertex_refs[meshlet.first_vertex_ref + ((packed >> (lane * 8)) & 255u)];
                Check(key[lane] == geometry.indices[primitive * 3 + lane],
                      "primitive remap must preserve oriented triangle identity");
            }
            const auto [entry, inserted] = previous.emplace(key, primitive);
            if (!inserted)
            {
                Check(entry->second < primitive, "duplicate oriented primitives must use authored FIFO order");
                entry->second = primitive;
            }
        }
    Check(std::ranges::all_of(seen, [](bool value) { return value; }), "every authored occurrence must survive cooking");
}
void TestIndexedVertexFieldIdentity()
{
    std::vector<rd::ExpandedVertex> source;
    const auto append = [&](rd::ExpandedVertex vertex) {
        source.insert(source.end(), 3, vertex);
    };
    append({});
    for (std::size_t lane = 0; lane < 4; ++lane)
    {
        rd::ExpandedVertex vertex{}; vertex.bone_indices[lane] = 7; append(vertex);
        vertex = {}; vertex.bone_weights[lane] = 1; append(vertex);
        vertex = {}; vertex.tangent[lane] = 1; append(vertex);
        vertex = {}; vertex.tangent[lane] = -0.0f; append(vertex);
        if (lane < 3)
        {
            vertex = {}; vertex.position[lane] = 1; append(vertex);
            vertex = {}; vertex.normal[lane] = 1; append(vertex);
            vertex = {}; vertex.position[lane] = -0.0f; append(vertex);
        }
        if (lane < 2)
        {
            vertex = {}; vertex.uv[lane] = 1; append(vertex);
        }
    }
    rd::ExpandedVertex material{}; material.material_index = 1; append(material);
    append({}); // Also repeat an earlier vertex after all distinct fields.
    const auto encoded = rd::EncodeExpandedVertices(source);
    const auto record = [&](std::size_t index) {
        return std::span(encoded).subspan(index * rd::kExpandedVertexStride, rd::kExpandedVertexStride);
    };
    std::vector<rd::ExpandedVertex> expected_vertices;
    std::vector<unsigned> expected_indices;
    // Independent portable-byte reference: first identical record owns the
    // index, including distinctions such as positive versus negative zero.
    for (std::size_t i = 0; i < source.size(); ++i)
    {
        std::size_t previous = 0;
        while (previous < i && !std::ranges::equal(record(previous), record(i))) ++previous;
        if (previous == i)
        {
            expected_indices.push_back(static_cast<unsigned>(expected_vertices.size()));
            expected_vertices.push_back(source[i]);
        }
        else expected_indices.push_back(expected_indices[previous]);
    }
    std::vector<rd::ExpandedVertex> vertices;
    std::vector<unsigned> indices;
    gc::IndexVertices(source, vertices, indices);
    Check(indices == expected_indices, "native indexing must preserve portable field identity and first occurrence order");
    Check(rd::EncodeExpandedVertices(vertices) == rd::EncodeExpandedVertices(expected_vertices),
          "native indexed vertices must preserve every portable field bit");
    source.front().reserved = 1;
    bool rejected = false;
    try { gc::IndexVertices(source, vertices, indices); }
    catch (const std::runtime_error &) { rejected = true; }
    Check(rejected, "native indexing must retain reserved-field validation");
}
void Validate(const gc::CookedSceneHlod &cooked, std::uint32_t page_limit)
{
    const auto decoded = rd::DecodeSceneHlod(rd::EncodeSceneHlod(cooked.scene));
    rd::ValidateSceneHlodPages(decoded, cooked.pages);
    for (const auto &group : decoded.groups)
    {
        std::set<std::uint32_t> pages;
        for (std::uint32_t p = group.first_proxy; p < group.first_proxy + group.proxy_count; ++p)
            pages.insert(decoded.proxies[p].page_index);
        Check(pages.size() <= page_limit, "replacement exceeds complete-group page budget");
    }
    for (std::size_t id = 0; id < cooked.pages.size(); ++id)
    {
        Check(decoded.pages[id].flags == 0, "optional aggregate was pinned");
        const auto asset = hs::MakeAssetId(decoded.pages[id].logical_id);
        const auto bytes = rd::BuildGeometryPage(asset, {}, cooked.pages[id]);
        const auto payload = rd::DecodeGeometryPagePayload(bytes, asset, cooked.pages[id].flags);
        rd::ValidateSceneHlodPage(decoded, static_cast<std::uint32_t>(id), payload);
    }
}
void TestGroundDetailCanonicalLayoutDigest()
{
    hs::ArenaBoundary boundary{};
    boundary.count = 4;
    boundary.points[0] = {-16,-16}; boundary.points[1] = {-16,16};
    boundary.points[2] = {16,16}; boundary.points[3] = {16,-16};
    const auto layout = hs::BuildEnvironmentLayout(16, boundary, {}, 0);
    std::vector<rd::SceneHlodMember> canonical;
    std::vector<gc::SceneHlodCookMember> members;
    std::vector<rd::SceneHlodMember> subset;
    std::uint32_t details{}, pebbles{};
    for (const auto &eligible : layout.eligible_static_members)
    {
        const auto &instance = layout.instances.at(eligible.instance_index);
        rd::SceneHlodMember record{};
        record.stable_id = eligible.stable_id; record.object_id = eligible.object_id;
        record.asset_id = instance.asset_id; record.mesh_semantic = static_cast<std::uint32_t>(instance.mesh);
        record.variant = instance.environment_variant; record.tint_rgba = instance.color_rgba;
        record.position = instance.position; record.scale = instance.scale; record.yaw = instance.yaw;
        canonical.push_back(record);
        if ((instance.mesh == hs::RenderMesh::GroundDetail && details < 2) ||
            (instance.mesh == hs::RenderMesh::Rock && instance.scale.x <= .1f && pebbles == 0))
        {
            if (instance.mesh == hs::RenderMesh::GroundDetail) ++details; else ++pebbles;
            members.push_back({instance, eligible.object_id}); subset.push_back(record);
        }
    }
    Check(details == 2 && pebbles == 1, "small fixture must mix appended detail and pebble provenance categories");
    Check(layout.eligible_digest == rd::ComputeSceneHlodLayoutDigest(layout.abi, canonical),
          "shared environment digest must follow canonical stable-ID order");
    // The complete layout digest is checked above without cooking the forest.
    // Only three instances of a two-triangle source exercise actual HLOD validation.
    std::vector<rd::ExpandedVertex> vertices;
    for (const auto position : std::array<std::array<float,3>,6>{{
            {0,0,0},{0,0,.2f},{.2f,0,0},{.2f,0,0},{0,0,.2f},{.2f,0,.2f}}})
    {
        rd::ExpandedVertex vertex{}; vertex.position = position;
        vertex.normal = {0,1,0}; vertex.tangent = {1,0,0,1}; vertices.push_back(vertex);
    }
    const auto geometry = gc::CookGeometry(vertices,0);
    std::vector<std::array<std::string,1>> materials; materials.reserve(members.size());
    std::vector<gc::SceneHlodCookSource> sources;
    std::set<std::uint64_t> assets;
    for (const auto &member : members)
    {
        const auto &instance = member.instance;
        if (!assets.insert(instance.asset_id.value).second) continue;
        const auto ordinal = rd::EnvironmentGeometryOrdinal(instance.mesh,instance.environment_variant);
        const auto id = "render/" + std::string(rd::kEnvironmentMaterialNames[ordinal]) + ".rpkg";
        materials.push_back({rd::MaterialProduct(
            rd::kAuthoredMaterialNames[rd::EnvironmentGeometryMaterialOrdinal(ordinal)])});
        sources.push_back({instance.asset_id,id,&geometry,materials.back()});
    }
    gc::SceneHlodCookOptions options; options.error_worker_count = 1;
    const auto digest = rd::ComputeSceneHlodLayoutDigest(layout.abi,subset);
    const auto cooked = gc::CookSceneHlod(hs::MakeAssetId("render/scene/detail-digest.shlod"),
        layout.abi,digest,members,sources,"render/hlod/detail-digest",options);
    Validate(cooked,options.max_group_pages);
    Check(cooked.scene.members.size() == members.size(),"small detail cook lost source coverage");
}
void TestSharedMaterialAtlasSelectors()
{
    // Identical material IDs must not collapse atlas selectors, even when all
    // remaining shading keys (including the instance variant) are identical.
    const auto grid = Grid();
    std::vector<rd::ExpandedVertex> vertices;
    for (std::uint16_t selector = 0; selector < 2; ++selector)
        for (const auto index : grid.indices)
        {
            auto vertex = grid.vertices[index];
            vertex.material_index = selector; vertex.position[0] += 10.0f * selector;
            vertices.push_back(vertex);
        }
    const auto geometry = gc::CookGeometry(vertices, 0);
    const std::string source_id = "render/fence_broken.rpkg";
    const std::string material_id = "render/material/ground_detail.rpkg";
    const std::array materials{material_id, material_id};
    const std::array sources{gc::SceneHlodCookSource{
        hs::MakeAssetId(source_id), source_id, &geometry, materials}};
    std::vector<gc::SceneHlodCookMember> members;
    std::vector<rd::SceneHlodMember> layout;
    for (std::uint32_t i = 0; i < 2; ++i)
    {
        hs::RenderInstance instance{};
        instance.mesh = hs::RenderMesh::GroundDetail; instance.environment_variant = 14;
        instance.stable_id = i + 1; instance.asset_id = sources[0].asset_id;
        instance.material_id = hs::MakeAssetId(material_id);
        instance.position = {0, 0, 24.0f * i}; instance.scale = {1, 1, 1};
        instance.color_rgba = 0xffffffffu;
        members.push_back({instance, i + 1});
        rd::SceneHlodMember record{};
        record.stable_id = instance.stable_id; record.object_id = i + 1;
        record.asset_id = instance.asset_id; record.mesh_semantic = static_cast<std::uint32_t>(instance.mesh);
        record.variant = instance.environment_variant; record.tint_rgba = instance.color_rgba;
        record.position = instance.position; record.scale = instance.scale; record.yaw = instance.yaw;
        layout.push_back(record);
    }
    gc::SceneHlodCookOptions options;
    options.objects_per_leaf = 1; options.children_per_group = 2; options.error_worker_count = 1;
    const auto cooked = gc::CookSceneHlod(hs::MakeAssetId("render/scene/atlas-selectors.shlod"),
        1, rd::ComputeSceneHlodLayoutDigest(1, layout), members, sources,
        "render/hlod/atlas-selectors", options);
    Validate(cooked, options.max_group_pages);
    const auto decoded = rd::DecodeSceneHlod(rd::EncodeSceneHlod(cooked.scene));
    Check(std::ranges::count(decoded.dependencies, material_id) == 1,
          "shared material product was not canonically deduplicated");
    bool mixed_parent{};
    for (const auto &group : decoded.groups)
    {
        if (group.child_count == 0 || group.proxy_count == 0) continue;
        std::array<bool, 2> selectors{};
        for (std::uint32_t i = group.first_proxy; i < group.first_proxy + group.proxy_count; ++i)
        {
            const auto &proxy = decoded.proxies[i];
            Check(decoded.dependencies[proxy.material_dependency] == material_id && proxy.atlas_selector < 2,
                  "aggregate lost shared material or atlas identity");
            selectors[proxy.atlas_selector] = true;
            const auto asset = hs::MakeAssetId(decoded.pages[proxy.page_index].logical_id);
            const auto payload = rd::DecodeGeometryPagePayload(
                rd::BuildGeometryPage(asset, {}, cooked.pages[proxy.page_index]), asset,
                cooked.pages[proxy.page_index].flags);
            for (std::uint32_t j = proxy.first_index; j < proxy.first_index + proxy.index_count; ++j)
                Check(payload.vertices[payload.indices[j]].material_index == proxy.atlas_selector,
                      "encoded parent vertex remapped a shared-material atlas selector");
        }
        mixed_parent |= selectors[0] && selectors[1];
    }
    Check(mixed_parent, "fixture failed to produce a parent with distinct slice0/slice1 proxies");
}
} // namespace
int main()
{
    try
    {
        TestAssetActionSnapshot();
        TestCertifiedTriangleDistanceBound();
        TestAnalyticErrorSurfaces();
        TestGeometryErrorQualificationFallback();
        TestDuplicatePrimitiveOrder();
        TestIndexedVertexFieldIdentity();
        TestGroundDetailCanonicalLayoutDigest();
        TestSharedMaterialAtlasSelectors();
        const auto geometry = Grid();
        const auto nonplanar_geometry = Grid(true);
        const std::string source_id = "render/rock_0.rpkg";
        const std::array materials{std::string("render/material/rock.rmat")};
        const std::array sources{gc::SceneHlodCookSource{hs::MakeAssetId(source_id), source_id, &geometry, materials}};
        const std::array nonplanar_sources{gc::SceneHlodCookSource{
            hs::MakeAssetId(source_id), source_id, &nonplanar_geometry, materials}};
        std::vector<gc::SceneHlodCookMember> members;
        std::vector<rd::SceneHlodMember> layout;
        for (std::uint32_t i = 0; i < 12; ++i)
        {
            gc::SceneHlodCookMember member{};
            auto &instance = member.instance;
            member.object_id = i + 1; instance.stable_id = i + 1;
            instance.asset_id = sources[0].asset_id; instance.mesh = hs::RenderMesh::Rock;
            instance.position = {float(i) * 12, 0, 0}; instance.scale = {1, 1, 1};
            instance.color_rgba = 0xff000000u | (i + 1);
            members.push_back(member);
            rd::SceneHlodMember record{};
            record.stable_id = instance.stable_id; record.object_id = member.object_id;
            record.mesh_semantic = static_cast<std::uint32_t>(instance.mesh);
            record.variant = instance.environment_variant; record.tint_rgba = instance.color_rgba;
            record.position = instance.position; record.scale = instance.scale; record.yaw = instance.yaw;
            layout.push_back(record);
        }
        const auto digest = rd::ComputeSceneHlodLayoutDigest(1, layout);
        const auto cook = [&](gc::SceneHlodCookOptions options) {
            return gc::CookSceneHlod(hs::MakeAssetId("render/scene/fixture.shlod"), 1, digest,
                                    members, sources, "render/hlod/fixture", options);
        };
        gc::SceneHlodCookOptions packed_options;
        packed_options.max_page_triangles = 512;
        packed_options.error_worker_count = 1;
        const auto packed = cook(packed_options);
        auto parallel_options = packed_options;
        parallel_options.error_worker_count = 8;
        const auto parallel = cook(parallel_options);
        Validate(packed, packed_options.max_group_pages);
        Validate(parallel, parallel_options.max_group_pages);
        Check(rd::EncodeSceneHlod(packed.scene) == rd::EncodeSceneHlod(parallel.scene),
              "serial and parallel scene HLOD encodings differ");
        Check(packed.pages.size() == parallel.pages.size(),
              "serial and parallel scene HLOD page counts differ");
        for (std::size_t index = 0; index < packed.pages.size(); ++index)
        {
            Check(packed.scene.pages[index].logical_id == parallel.scene.pages[index].logical_id,
                  "serial and parallel scene HLOD page identities differ");
            const auto asset = hs::MakeAssetId(packed.scene.pages[index].logical_id);
            Check(rd::BuildGeometryPage(asset, {}, packed.pages[index]) ==
                      rd::BuildGeometryPage(asset, {}, parallel.pages[index]),
                  "serial and parallel geometry page payloads differ");
        }
        Check(!packed.scene.proxies.empty(), "fixture did not simplify to aggregate geometry");
        std::vector<unsigned> proxy_counts(packed.pages.size());
        for (const auto &proxy : packed.scene.proxies)
        {
            ++proxy_counts[proxy.page_index];
            Check(proxy.atlas_selector == 0 && proxy.variant == 0 &&
                  proxy.tint_rgba >= 0xff000001u && proxy.tint_rgba <= 0xff00000cu,
                  "packed fragment lost instance shading identity");
        }
        Check(std::ranges::any_of(proxy_counts, [](unsigned count) { return count > 1; }),
              "material/instance fragments were not packed into shared pages");

        const auto nonplanar_cook = [&](gc::SceneHlodCookOptions options) {
            return gc::CookSceneHlod(hs::MakeAssetId("render/scene/fixture.shlod"), 1, digest,
                                    members, nonplanar_sources, "render/hlod/fixture", options);
        };
        const auto nonplanar_serial = nonplanar_cook(packed_options);
        const auto nonplanar_parallel = nonplanar_cook(parallel_options);
        Validate(nonplanar_serial, packed_options.max_group_pages);
        Validate(nonplanar_parallel, parallel_options.max_group_pages);
        Check(std::ranges::any_of(nonplanar_serial.scene.groups,
                                  [](const auto &group) { return group.world_error > 0; }),
              "nonplanar fixture did not exercise geometric error measurement");
        Check(rd::EncodeSceneHlod(nonplanar_serial.scene) ==
                  rd::EncodeSceneHlod(nonplanar_parallel.scene),
              "nonplanar serial and parallel scene HLOD encodings differ");
        Check(nonplanar_serial.pages.size() == nonplanar_parallel.pages.size(),
              "nonplanar serial and parallel scene HLOD page counts differ");
        for (std::size_t index = 0; index < nonplanar_serial.pages.size(); ++index)
        {
            Check(nonplanar_serial.scene.pages[index].logical_id ==
                      nonplanar_parallel.scene.pages[index].logical_id,
                  "nonplanar serial and parallel scene HLOD page identities differ");
            const auto asset = hs::MakeAssetId(nonplanar_serial.scene.pages[index].logical_id);
            Check(rd::BuildGeometryPage(asset, {}, nonplanar_serial.pages[index]) ==
                      rd::BuildGeometryPage(asset, {}, nonplanar_parallel.pages[index]),
                  "nonplanar serial and parallel geometry page payloads differ");
        }

        gc::SceneHlodCookOptions bounded_options;
        bounded_options.objects_per_leaf = 1;
        bounded_options.max_page_triangles = 128;
        bounded_options.max_group_pages = 2;
        const auto bounded = cook(bounded_options);
        Validate(bounded, bounded_options.max_group_pages);
        Check(bounded.scene.roots.size() > 1 && !bounded.scene.proxies.empty(),
              "oversize aggregate did not retain independently loadable child forest");
        Check(bounded.scene.members.size() == members.size(), "forest lost original coverage");
        std::cout << "Bounded scene HLOD packing and forest contracts passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n'; return 1;
    }
}
