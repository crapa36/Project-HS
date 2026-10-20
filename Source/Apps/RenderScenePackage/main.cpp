#include "../AssetPipeline/action.hpp"
#include "../Content/arena_layout_payload.hpp"
#include "../Content/scene_hlod_cooker.hpp"
#include <hs/presentation/environment_layout.hpp>
#include <hs/render_data/render_asset_format.hpp>
#include <hs/render_data/material_schema.hpp>

#include <algorithm>
#include <array>
#include <iostream>
#include <map>
#include <ranges>
#include <stdexcept>
#include <string_view>

namespace
{
namespace ap = hs::asset_pipeline;
namespace rd = hs::render_data;
namespace fs = std::filesystem;
constexpr std::string_view kSceneId = "render/hlod/arena.shl1";
void Require(bool valid, std::string_view reason)
{
    if (!valid) throw std::runtime_error("hs_render_scene_package: " + std::string(reason));
}
std::span<const std::byte> Bytes(const std::string &bytes)
{
    return std::as_bytes(std::span(bytes.data(), bytes.size()));
}
fs::path Upstream(const fs::path &build, ap::ActionSpec &spec,
                  std::map<std::string, ap::ActionResultSnapshot> &snapshots,
                  std::string_view action_namespace, std::string_view id)
{
    ap::UpstreamProduct dependency;
    fs::path path;
    auto snapshot = snapshots.try_emplace(std::string(action_namespace), build, action_namespace).first;
    snapshot->second.ReadProduct(id, dependency, path);
    spec.upstream.push_back(std::move(dependency));
    return path;
}
bool Current(const fs::path &build, const ap::ActionSpec &spec,
             const std::array<std::byte, 32> &layout_digest)
{
    const auto result_path = build / "Actions" / spec.namespace_path / "result.json";
    if (!fs::exists(result_path) || !fs::exists(result_path.parent_path() / "result.d")) return false;
    try
    {
        const ap::ActionResultSnapshot result(build, spec.namespace_path);
        if (result.ActionId() != spec.action_id || result.ActionKeyValue() != ap::ActionKey(spec)) return false;
        ap::UpstreamProduct ignored;
        fs::path scene_path;
        result.ReadProduct(kSceneId, ignored, scene_path);
        const auto scene_bytes = ap::ReadBytes(scene_path);
        const auto scene = rd::DecodeSceneHlod(Bytes(scene_bytes));
        if (scene.scene_asset_id.value != hs::MakeAssetId(kSceneId).value || scene.layout_digest != layout_digest ||
            result.ProductCount() != scene.pages.size() + 1) return false;
        for (std::uint32_t i = 0; i < scene.pages.size(); ++i)
        {
            fs::path page_path;
            result.ReadProduct(scene.pages[i].logical_id, ignored, page_path);
            const auto bytes = ap::ReadBytes(page_path);
            const auto page = rd::DecodeGeometryPagePayload(Bytes(bytes),
                hs::MakeAssetId(scene.pages[i].logical_id), scene.pages[i].geometry_flags);
            rd::ValidateSceneHlodPage(scene, i, page);
        }
        return true;
    }
    catch (const std::exception &) { return false; }
}
void Produce(const fs::path &source, const fs::path &build)
{
    ap::ActionSpec spec;
    spec.action_id = "render.hlod.arena";
    spec.recipe_id = "project-hs.render.scene-hlod";
    spec.recipe_version = 2;
    spec.namespace_path = "render/hlod/arena";
    spec.inputs.push_back(ap::HashInput("layout/implementation",
        source / "Source/Presentation/Private/environment_layout.cpp"));
    std::map<std::string, ap::ActionResultSnapshot> snapshots;
    const auto arena_path = Upstream(build, spec, snapshots,
        "game-data/simulation-payload", "game-data/arena-layout");
    const auto arena = hs::content::DecodeArenaLayoutPayload(ap::ReadBytes(arena_path));
    const auto layout = hs::BuildEnvironmentLayout(arena.half_extent, arena.boundary,
        std::span(arena.obstacles).first(arena.obstacle_count), 0);
    std::vector<hs::geometry_content::SceneHlodCookMember> members;
    for (const auto &member : layout.eligible_static_members)
        members.push_back({layout.instances.at(member.instance_index), member.object_id});
    struct Source { std::string id; rd::GeometryData geometry; std::vector<std::string> materials; };
    std::map<std::uint64_t, Source> assets;
    for (const auto name : rd::kEnvironmentMaterialNames)
    {
        const auto id = "render/" + std::string(name) + ".rpkg";
        const auto asset_id = hs::MakeAssetId(id);
        if (!std::ranges::any_of(members, [&](const auto &member) { return member.instance.asset_id.value == asset_id.value; })) continue;
        const auto action_namespace = "render/" + std::string(name);
        const auto package_bytes = ap::ReadBytes(Upstream(build, spec, snapshots, action_namespace, id));
        const auto package = rd::ReadPackage(Bytes(package_bytes), asset_id);
        const auto metadata_chunk = std::ranges::find(package.chunks, rd::ChunkType::GeometryMetadata, &rd::ChunkView::type);
        Require(metadata_chunk != package.chunks.end(), "source geometry metadata missing");
        const auto metadata = rd::DecodeGeometryMetadata(metadata_chunk->bytes, package.dependencies, id);
        const auto page_id = "render/geometry/" + std::string(name) + ".rpage";
        const auto page_bytes = ap::ReadBytes(Upstream(build, spec, snapshots, action_namespace, page_id));
        auto geometry = rd::DecodeGeometryPage(Bytes(page_bytes), hs::MakeAssetId(page_id), metadata);
        std::uint32_t selectors{};
        for (const auto &vertex : geometry.vertices) selectors = std::max(selectors, std::uint32_t(vertex.material_index) + 1);
        const auto ordinal = static_cast<std::uint32_t>(std::ranges::find(rd::kEnvironmentMaterialNames, name) - rd::kEnvironmentMaterialNames.begin());
        const auto material_id = rd::EnvironmentGeometryMaterialOrdinal(ordinal) == rd::kGroundDetailMaterialOrdinal
            ? std::string("render/material/ground_detail.rpkg") : id;
        assets.emplace(asset_id.value, Source{id, std::move(geometry), std::vector<std::string>(selectors, material_id)});
    }
    std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);
    std::ranges::sort(spec.upstream, {}, &ap::UpstreamProduct::logical_id);
    if (Current(build, spec, layout.eligible_digest))
    {
        ap::TouchActionResult(build / "Actions" / spec.namespace_path / "result.json");
        std::cout << spec.action_id << " current\n";
        return;
    }
    std::vector<hs::geometry_content::SceneHlodCookSource> sources;
    for (const auto &[asset, value] : assets)
        sources.push_back({hs::AssetId{asset}, value.id, &value.geometry, value.materials});
    const auto cooked = hs::geometry_content::CookSceneHlod(hs::MakeAssetId(kSceneId), layout.abi,
        layout.eligible_digest, members, sources, "render/hlod/arena/page");
    // CookSceneHlod validates the complete scene/page payload before returning.
    const auto stage = ap::CreateStage(build / "Actions" / spec.namespace_path);
    try
    {
        std::vector<ap::Product> products;
        for (std::uint32_t i = 0; i < cooked.pages.size(); ++i)
        {
            const auto &id = cooked.scene.pages[i].logical_id;
            const auto filename = fs::path(id).filename().string();
            const auto bytes = rd::BuildGeometryPage(hs::MakeAssetId(id), cooked.scene.dependencies, cooked.pages[i]);
            ap::WriteBytes(stage / filename, bytes);
            auto product = ap::HashProduct(id, filename, stage / filename);
            product.dependencies = cooked.scene.dependencies;
            products.push_back(std::move(product));
        }
        const auto bytes = rd::EncodeSceneHlod(cooked.scene);
        (void)rd::DecodeSceneHlod(bytes);
        ap::WriteBytes(stage / "arena.shl1", bytes);
        auto product = ap::HashProduct(std::string(kSceneId), "arena.shl1", stage / "arena.shl1");
        product.dependencies = cooked.scene.dependencies;
        for (const auto &page : cooked.scene.pages) product.dependencies.push_back(page.logical_id);
        product.dependencies.push_back("game-data/arena-layout");
        products.push_back(std::move(product));
        ap::Publish(build, spec, stage, std::move(products));
    }
    catch (...) { ap::RemoveStage(stage); throw; }
    ap::RemoveStage(stage);
    std::cout << spec.action_id << " published\n";
}
}
int wmain(int argc, wchar_t **argv)
{
    fs::path source, build;
    try
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring_view option = argv[i];
            if (option == L"--source-root" && source.empty() && ++i < argc) source = argv[i];
            else if (option == L"--build-root" && build.empty() && ++i < argc) build = argv[i];
            else throw std::runtime_error("hs_render_scene_package: invalid command line");
        }
        Require(!source.empty() && !build.empty(), "--source-root and --build-root required");
        Produce(source.lexically_normal(), build.lexically_normal());
        return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure("hs_render_scene_package", error.what(),
                          {{"action_id", "render.hlod.arena"},
                           {"source_root", ap::DiagnosticPath(source)}, {"build_root", ap::DiagnosticPath(build)},
                           {"dependency", "game-data/arena-layout"}});
        return 1;
    }
}
