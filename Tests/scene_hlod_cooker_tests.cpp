#include "geometry_cooker.hpp"
#include "scene_hlod_cooker.hpp"
#include <hs/presentation/environment_layout.hpp>
#include <hs/render_data/material_schema.hpp>

#include <algorithm>
#include <array>
#include <iostream>
#include <set>
#include <stdexcept>

namespace gc = hs::geometry_content;
namespace rd = hs::render_data;
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
        materials.push_back({instance.mesh == hs::RenderMesh::GroundDetail
            ? std::string("render/material/ground_detail.rpkg") : id});
        sources.push_back({instance.asset_id,id,&geometry,materials.back()});
    }
    gc::SceneHlodCookOptions options; options.error_worker_count = 1;
    const auto digest = rd::ComputeSceneHlodLayoutDigest(layout.abi,subset);
    const auto cooked = gc::CookSceneHlod(hs::MakeAssetId("render/scene/detail-digest.shlod"),
        layout.abi,digest,members,sources,"render/hlod/detail-digest",options);
    Validate(cooked,options.max_group_pages);
    Check(cooked.scene.members.size() == members.size(),"small detail cook lost source coverage");
}
} // namespace
int main()
{
    try
    {
        TestGroundDetailCanonicalLayoutDigest();
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
