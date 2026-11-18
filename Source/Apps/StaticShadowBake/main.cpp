#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/presentation/environment_layout.hpp>
#include <hs/renderer/renderer.hpp>
#include <hs/render_data/static_shadow_archive.hpp>
#include "../Common/world_transport_scene.hpp"
#include "../AssetPipeline/action.hpp"
#include "../Common/bake_product_inputs.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <hs/core/sha256.hpp>
#include <hs/render_data/material_descriptor.hpp>
#include <hs/render_data/material_schema.hpp>
#include <hs/render_data/static_shadow.hpp>

extern "C"
{
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}

namespace hs::static_shadow
{
namespace
{
namespace ap = asset_pipeline;
namespace rd = render_data;
namespace fs = std::filesystem;
constexpr std::string_view kAction = "render.lighting.arena-static-shadow";
constexpr std::string_view kNamespace = "render/lighting/arena-static-shadow";
constexpr std::string_view kIndex = "render/lighting/arena_static_shadow.index";
constexpr std::string_view kPages = "render/lighting/arena_static_shadow.pages";
constexpr std::string_view kIndexFilename = "arena_static_shadow.index";
constexpr std::string_view kPagesFilename = "arena_static_shadow.pages";

void Require(const hs::Result &result)
{
    if (!result) throw std::runtime_error(std::string(result.Subsystem()) + ": " + std::string(result.Message()));
}

void Require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error(message);
}

bool Current(const fs::path &build, const ap::ActionSpec &spec)
{
    const auto result = build / "Actions" / kNamespace / "result.json";
    if (!fs::exists(result) || !fs::exists(result.parent_path() / "result.d")) return false;
    try
    {
        const ap::ActionResultSnapshot snapshot(build, kNamespace);
        if (snapshot.ActionId() != kAction || snapshot.ActionKeyValue() != ap::ActionKey(spec) ||
            snapshot.ProductCount() != 2) return false;
        std::vector<std::string> dependencies;
        for (const auto &upstream : spec.upstream) dependencies.push_back(upstream.logical_id);
        if (snapshot.ProductMetadata(kPages).value("dependencies", std::vector<std::string>{}) != dependencies)
            return false;
        dependencies.emplace_back(kPages);
        std::ranges::sort(dependencies);
        if (snapshot.ProductMetadata(kIndex).value("dependencies", std::vector<std::string>{}) != dependencies)
            return false;
        ap::UpstreamProduct ignored;
        fs::path path;
        snapshot.ReadProduct(kIndex, ignored, path);
        snapshot.ReadProduct(kPages, ignored, path);
        return true;
    }
    catch (const std::exception &) { return false; }
}
}

int Run(int argc, char **argv)
{
    fs::path build, pointer;
    try
    {
        std::set<std::string> arguments;
        for (int i = 1; i < argc; ++i)
        {
            std::string_view argument(argv[i]);
            const auto separator = argument.find('=');
            const auto name = argument.substr(0, separator);
            Require(name == "--build-root" || name == "--generation",
                "Unknown static shadow producer argument: " + std::string(name));
            Require(arguments.emplace(name).second, "Duplicate static shadow producer argument: " + std::string(name));
            std::string_view value;
            if (separator != std::string_view::npos) value = argument.substr(separator + 1);
            else { Require(i + 1 < argc, "Missing argument value: " + std::string(name)); value = argv[++i]; }
            Require(!value.empty(), "Empty static shadow producer argument");
            if (name == "--build-root") build = value;
            else pointer = value;
        }
        Require(build.is_absolute() && pointer.is_absolute(),
            "Static shadow producer requires absolute --build-root and --generation");
        ap::RequireDirectory(build);
        build = fs::canonical(build);

        SelectedGeneration generation;
        const auto loaded = LoadSelectedGeneration(pointer, generation);
        Require(static_cast<bool>(loaded), std::string(loaded.Message()));
        Require(!generation.product_paths.contains(std::string(kIndex)) &&
            !generation.product_paths.contains(std::string(kPages)), "Bake input Generation must not contain its own shadow products");

        ap::ActionSpec spec;
        spec.action_id = kAction; spec.namespace_path = kNamespace;
        spec.recipe_id = "project-hs.render.static-shadow/coarse-fallback-lod2";
        spec.recipe_version = 1;
        hs::app::BakeProductInputs inputs(build, generation);
        std::set<std::string> dependencies;
        const auto add_closure = [&](std::string root) {
            std::vector<std::string> pending{std::move(root)};
            while (!pending.empty())
            {
                auto id = std::move(pending.back()); pending.pop_back();
                if (!dependencies.insert(id).second) continue;
                spec.upstream.push_back(inputs.Verify(id));
                // Depth capture uses its named probe-shadow program, not runtime raster programs.
                for (const auto &dependency : generation.product_dependencies.at(id))
                    if (!hs::app::IsRuntimeSurfaceShaderDependency(id, dependency)) pending.push_back(dependency);
            }
        };
        // Verify the rules before the shared scene builder decodes them.
        add_closure("game-data/simulation-rules");
        // Static shadow archives must use the same default scene seed as gameplay.
        constexpr std::uint64_t kDefaultGameplaySeed = 1;
        const auto scene = hs::app::WorldTransportScene(generation, kDefaultGameplaySeed);
        Require(scene && !scene->instances.empty(), "Static shadow scene is empty");
        add_closure("lighting/producer/local_probe_capture_source_base_vs.dxil");
        std::set<std::uint32_t> materials;
        std::array<bool, rd::kEnvironmentMaterialNames.size()> geometry_seen{};
        for (const auto &instance : scene->instances)
        {
            if (!rd::IsStaticShadowCaster(instance)) continue;
            auto material = rd::kTerrainMaterialOrdinal;
            if (rd::IsEnvironmentMesh(instance.mesh))
            {
                const auto geometry = rd::EnvironmentGeometryOrdinal(instance.mesh, instance.environment_variant);
                Require(geometry < rd::kEnvironmentMaterialNames.size(), "Invalid static shadow geometry ordinal");
                if (geometry_seen[geometry]) continue;
                geometry_seen[geometry] = true;
                const auto lod = rd::kEnvironmentGeometryLodCount - 1;
                // Native static shadow capture draws only the terminal fallback page.
                add_closure("render/" + std::string(rd::kEnvironmentMaterialNames[geometry]) +
                    "_lod" + std::to_string(lod) + ".rpkg");
                material = rd::EnvironmentGeometryMaterialOrdinal(geometry);
            }
            materials.insert(material);
        }
        Require(!materials.empty(), "Static shadow scene has no eligible material");
        for (const auto ordinal : materials)
        {
            const std::string name(rd::kAuthoredMaterialNames.at(ordinal));
            add_closure(rd::MaterialProduct(name));
            add_closure("render/material/" + name + "/parameters.rpkg");
            add_closure("lighting/producer/" + name + "_probe_shadow.dxil");
        }
        std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);
        std::ranges::sort(spec.upstream, {}, &ap::UpstreamProduct::logical_id);
        if (Current(build, spec))
        {
            return 0;
        }
        const auto layout = rd::StaticShadowLayoutDigest(*scene);
        const auto action_directory = build / "Actions" / kNamespace;
        const auto stage = ap::CreateStage(action_directory);
        try
        {
            rd::StaticShadowArchiveIndex index;
            std::vector<std::uint8_t> written;
            std::ofstream payload(stage / kPagesFilename, std::ios::binary | std::ios::trunc);
            Require(static_cast<bool>(payload), "Cannot create static shadow page payload");
            bool begun{}, covered{};
            hs::RendererConfig config;
            config.width = 320; config.height = 180;
            config.artifact_directory = action_directory;
            config.temporal_reconstruction = false;
            config.content_generation_id = std::move(generation.id);
            config.product_paths = std::move(generation.product_paths);
            config.product_digests = std::move(generation.product_digests);
            config.content_generation_lease = std::make_shared<const hs::SelectedGeneration>(std::move(generation));
            Require(hs::D3D12Renderer::BakeStaticShadowPages(std::move(config), scene, hs::EnvironmentSun(),
                [&](const rd::StaticShadowArchiveDescription &description) {
                    Require(!begun && description.layout_digest == layout, "Native static shadow archive layout differs");
                    index.description = description;
                    // Fine pages retain the existing runtime static raster/cache producer.
                    for (std::size_t level = 0; level < 2; ++level)
                        index.description.levels[level].width = index.description.levels[level].height = 0;
                    const auto &coarse = index.description.levels[2];
                    const auto count = std::uint64_t(coarse.width) * coarse.height;
                    Require(count > 0 && count <= 1'000'000, "Static shadow coarse extent is invalid");
                    index.pages.resize(static_cast<std::size_t>(count));
                    written.resize(index.pages.size());
                    begun = true;
                    return Result::Success();
                },
                [&](std::uint32_t level, std::int32_t x, std::int32_t y, std::span<const float> depth) {
                    Require(begun && level == 2, "Unexpected native static shadow page level");
                    const auto &coarse = index.description.levels[2];
                    const auto local_x = std::int64_t(x) - coarse.minimum_x;
                    const auto local_y = std::int64_t(y) - coarse.minimum_y;
                    Require(local_x >= 0 && local_y >= 0 && local_x < coarse.width && local_y < coarse.height,
                        "Native static shadow page exceeds its declared extent");
                    const auto slot = static_cast<std::size_t>(local_y * coarse.width + local_x);
                    Require(!written[slot], "Native static shadow emitted a duplicate page");
                    const auto encoded = rd::EncodeStaticShadowPage(depth);
                    auto &record = index.pages[slot];
                    if (!encoded.empty())
                    {
                        record.offset = index.payload_bytes;
                        record.bytes = static_cast<std::uint32_t>(encoded.size());
                        record.digest = hs::Sha256(encoded);
                        payload.write(reinterpret_cast<const char *>(encoded.data()),
                            static_cast<std::streamsize>(encoded.size()));
                        Require(static_cast<bool>(payload), "Cannot write static shadow page payload");
                        index.payload_bytes += encoded.size();
                        covered = true;
                    }
                    written[slot] = 1;
                    return Result::Success();
                }));
            Require(begun && covered && std::ranges::all_of(written, [](auto value) { return value != 0; }),
                "Native static shadow coarse coverage is incomplete or entirely clear");
            payload.close();
            Require(!payload.fail(), "Cannot finish static shadow page payload");
            ap::WriteBytes(stage / kIndexFilename, rd::BuildStaticShadowArchiveIndex(index));
            auto page_product = ap::HashProduct(std::string(kPages), std::string(kPagesFilename), stage / kPagesFilename);
            page_product.dependencies.assign(dependencies.begin(), dependencies.end());
            auto index_product = ap::HashProduct(std::string(kIndex), std::string(kIndexFilename), stage / kIndexFilename);
            index_product.dependencies = page_product.dependencies;
            index_product.dependencies.emplace_back(kPages);
            ap::Publish(build, spec, stage, {std::move(index_product), std::move(page_product)});
        }
        catch (...) { ap::RemoveStageAfterFailure(stage); throw; }
        ap::RemoveStage(stage);
        std::cout << kAction << " published\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure(kAction, error.what(), {{"build_root", build.string()}});
        return 1;
    }
}
}

int main(int argc, char **argv)
{
    return hs::static_shadow::Run(argc, argv);
}
