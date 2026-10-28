#include "../AssetPipeline/action.hpp"
#include "../Content/arena_layout_payload.hpp"
#include "../Content/scene_hlod_cooker.hpp"
#include <hs/presentation/environment_layout.hpp>
#include <hs/render_data/render_asset_format.hpp>
#include <hs/render_data/material_schema.hpp>
#include <hs/render_data/material_descriptor.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
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
std::string Upstream(const fs::path &build, ap::ActionSpec &spec,
                  std::map<std::string, ap::ActionResultSnapshot> &snapshots,
                  std::string_view action_namespace, std::string_view id)
{
    ap::UpstreamProduct dependency;
    auto snapshot = snapshots.try_emplace(std::string(action_namespace), build, action_namespace).first;
    auto bytes = snapshot->second.ReadProductBytes(id, dependency);
    spec.upstream.push_back(std::move(dependency));
    return bytes;
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
        const auto scene_bytes = result.ReadProductBytes(kSceneId, ignored);
        const auto scene = rd::DecodeSceneHlod(Bytes(scene_bytes));
        if (scene.scene_asset_id.value != hs::MakeAssetId(kSceneId).value || scene.layout_digest != layout_digest ||
            result.ProductCount() != scene.pages.size() + 1) return false;
        for (std::uint32_t i = 0; i < scene.pages.size(); ++i)
        {
            // This exact producer already validated the published payload; verify its bytes, not its geometry again.
            fs::path path;
            result.ReadProduct(scene.pages[i].logical_id, ignored, path);
        }
        return true;
    }
    catch (const std::exception &) { return false; }
}
void Produce(const fs::path &source, const fs::path &build, bool exhaustive_geometry_error)
{
    const bool profile = std::getenv("HS_HLOD_GEOMETRY_PROFILE") != nullptr;
    auto phase_start = std::chrono::steady_clock::now();
    const auto phase = [&](std::string_view name) {
        const auto now = std::chrono::steady_clock::now();
        if (profile) std::cerr << "hs.hlod.phase name=" << name << " elapsed_ms="
            << std::chrono::duration<double, std::milli>(now - phase_start).count() << '\n';
        phase_start = now;
    };
    ap::ActionSpec spec;
    spec.action_id = "render.hlod.arena";
    spec.recipe_id = "project-hs.render.scene-hlod";
    spec.recipe_version = 3;
    spec.namespace_path = "render/hlod/arena";
    spec.inputs.push_back(ap::HashInput("layout/implementation",
        source / "Source/Presentation/Private/environment_layout.cpp"));
    std::map<std::string, ap::ActionResultSnapshot> snapshots;
    const auto arena_bytes = Upstream(build, spec, snapshots,
        "game-data/simulation-payload", "game-data/arena-layout");
    const auto arena = hs::content::DecodeArenaLayoutPayload(arena_bytes);
    const auto layout = hs::BuildEnvironmentLayout(arena.half_extent, arena.boundary,
        std::span(arena.obstacles).first(arena.obstacle_count), 0);
    std::vector<hs::geometry_content::SceneHlodCookMember> members;
    for (const auto &member : layout.eligible_static_members)
        members.push_back({layout.instances.at(member.instance_index), member.object_id});
    std::vector<std::string_view> selected_names;
    for (const auto name : rd::kEnvironmentMaterialNames)
    {
        const auto id = "render/" + std::string(name) + ".rpkg";
        const auto asset_id = hs::MakeAssetId(id);
        if (!std::ranges::any_of(members, [&](const auto &member) { return member.instance.asset_id.value == asset_id.value; })) continue;
        selected_names.push_back(name);
        const auto action_namespace = "render/" + std::string(name);
        auto &snapshot = snapshots.try_emplace(action_namespace, build, action_namespace).first->second;
        for (const auto &product_id : {id, "render/geometry/" + std::string(name) + ".rpage"})
        {
            ap::UpstreamProduct dependency;
            fs::path path;
            snapshot.ReadProduct(product_id, dependency, path);
            spec.upstream.push_back(std::move(dependency));
        }
    }
    std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);
    std::ranges::sort(spec.upstream, {}, &ap::UpstreamProduct::logical_id);
    phase("inputs");
    if (!exhaustive_geometry_error && Current(build, spec, layout.eligible_digest))
    {
        phase("current_hit");
        ap::TouchActionResult(build / "Actions" / spec.namespace_path / "result.json");
        std::cout << spec.action_id << " current\n";
        return;
    }
    phase("current_miss");

    // Geometry expansion is only needed for a miss; each decoder consumes verified bytes.
    struct Source { std::string id; rd::GeometryData geometry; std::vector<std::string> materials; };
    std::map<std::uint64_t, Source> assets;
    for (const auto name : selected_names)
    {
        const auto id = "render/" + std::string(name) + ".rpkg";
        const auto asset_id = hs::MakeAssetId(id);
        const auto action_namespace = "render/" + std::string(name);
        const auto &snapshot = snapshots.at(action_namespace);
        ap::UpstreamProduct dependency;
        const auto package_bytes = snapshot.ReadProductBytes(id, dependency);
        const auto package = rd::ReadPackage(Bytes(package_bytes), asset_id);
        const auto metadata_chunk = std::ranges::find(package.chunks, rd::ChunkType::GeometryMetadata, &rd::ChunkView::type);
        Require(metadata_chunk != package.chunks.end(), "source geometry metadata missing");
        const auto metadata = rd::DecodeGeometryMetadata(metadata_chunk->bytes, package.dependencies, id);
        const auto page_id = "render/geometry/" + std::string(name) + ".rpage";
        const auto page_bytes = snapshot.ReadProductBytes(page_id, dependency);
        auto geometry = rd::DecodeGeometryPage(Bytes(page_bytes), hs::MakeAssetId(page_id), metadata);
        std::uint32_t selectors{};
        for (const auto &vertex : geometry.vertices) selectors = std::max(selectors, std::uint32_t(vertex.material_index) + 1);
        const auto ordinal = static_cast<std::uint32_t>(std::ranges::find(rd::kEnvironmentMaterialNames, name) - rd::kEnvironmentMaterialNames.begin());
        const auto reference = std::ranges::find(package.chunks,
            rd::ChunkType::GeometryMaterialReference, &rd::ChunkView::type);
        Require(reference != package.chunks.end() && reference->required,
            "source canonical material reference missing");
        const auto &material_id = package.dependencies[rd::DecodeGeometryMaterialReference(
            reference->bytes, package.dependencies)];
        Require(material_id == rd::MaterialProduct(
            rd::kAuthoredMaterialNames[rd::EnvironmentGeometryMaterialOrdinal(ordinal)]),
            "source canonical material does not match its geometry");
        assets.emplace(asset_id.value, Source{id, std::move(geometry), std::vector<std::string>(selectors, material_id)});
    }
    std::vector<hs::geometry_content::SceneHlodCookSource> sources;
    for (const auto &[asset, value] : assets)
        sources.push_back({hs::AssetId{asset}, value.id, &value.geometry, value.materials});
    phase("source_decode");
    hs::geometry_content::SceneHlodCookOptions options;
    if (exhaustive_geometry_error)
        options.error_checks.mode = hs::geometry_content::GeometryErrorCheckMode::Exhaustive;
    const auto cooked = hs::geometry_content::CookSceneHlod(hs::MakeAssetId(kSceneId), layout.abi,
        layout.eligible_digest, members, sources, "render/hlod/arena/page", options);
    phase("cook");
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
        phase("serialize");
        ap::Publish(build, spec, stage, std::move(products));
        phase("publish");
    }
    catch (...) { ap::RemoveStageAfterFailure(stage); throw; }
    ap::RemoveStage(stage);
    std::cout << spec.action_id << " published\n";
}
}
int wmain(int argc, wchar_t **argv)
{
    fs::path source, build;
    bool exhaustive_geometry_error{};
    try
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring_view option = argv[i];
            if (option == L"--source-root" && source.empty() && ++i < argc) source = argv[i];
            else if (option == L"--build-root" && build.empty() && ++i < argc) build = argv[i];
            else if (option == L"--exhaustive-geometry-error" && !exhaustive_geometry_error)
                exhaustive_geometry_error = true;
            else throw std::runtime_error("hs_render_scene_package: invalid command line");
        }
        Require(!source.empty() && !build.empty(), "--source-root and --build-root required [--exhaustive-geometry-error]");
        Produce(source.lexically_normal(), build.lexically_normal(), exhaustive_geometry_error);
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
