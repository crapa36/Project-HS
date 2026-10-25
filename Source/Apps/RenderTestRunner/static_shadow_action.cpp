#include "static_shadow_action.hpp"
#include "../AssetPipeline/action.hpp"

#include <hs/core/sha256.hpp>
#include <hs/render_data/material_schema.hpp>
#include <hs/render_data/static_shadow.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace hs::render_test
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

int ProduceStaticShadow(int argc, char **argv,
    std::shared_ptr<const StaticRenderScene> (*scene_builder)(const SelectedGeneration &),
    void (*native_bake)(const SelectedGeneration &, const std::shared_ptr<const StaticRenderScene> &,
        const std::filesystem::path &,
        const BeginStaticShadowArchive &, const WriteStaticShadowPage &))
{
    fs::path source, build, pointer, result_list;
    try
    {
        std::set<std::string> arguments;
        for (int i = 1; i < argc; ++i)
        {
            std::string_view argument(argv[i]);
            if (argument == "--produce-static-shadow") continue;
            const auto separator = argument.find('=');
            const auto name = argument.substr(0, separator);
            Require(name == "--source-root" || name == "--build-root" || name == "--generation" ||
                name == "--result-list", "Unknown static shadow producer argument: " + std::string(name));
            Require(arguments.emplace(name).second, "Duplicate static shadow producer argument: " + std::string(name));
            std::string_view value;
            if (separator != std::string_view::npos) value = argument.substr(separator + 1);
            else { Require(i + 1 < argc, "Missing argument value: " + std::string(name)); value = argv[++i]; }
            Require(!value.empty(), "Empty static shadow producer argument");
            if (name == "--source-root") source = value;
            else if (name == "--build-root") build = value;
            else if (name == "--generation") pointer = value;
            else if (name == "--result-list") result_list = value;
        }
        Require(source.is_absolute() && build.is_absolute() && pointer.is_absolute() && result_list.is_absolute(),
            "Static shadow producer requires absolute --source-root, --build-root, --generation and --result-list");
        ap::RequireDirectory(source); ap::RequireDirectory(build);
        source = fs::canonical(source); build = fs::canonical(build);

        SelectedGeneration generation;
        const auto loaded = LoadSelectedGeneration(pointer, generation);
        Require(static_cast<bool>(loaded), std::string(loaded.Message()));
        Require(!generation.product_paths.contains(std::string(kIndex)) &&
            !generation.product_paths.contains(std::string(kPages)), "Bake input Generation must not contain its own shadow products");

        ap::ActionSpec spec;
        spec.action_id = kAction; spec.namespace_path = kNamespace;
        spec.recipe_id = "project-hs.render.static-shadow/coarse-fallback-lod2";
        spec.recipe_version = 1;
        for (const auto &[id, path] : std::initializer_list<std::pair<std::string_view, fs::path>>{
                 {"layout/implementation", source / "Source/Presentation/Private/environment_layout.cpp"},
                 {"lighting/environment-contract", source / "Source/Presentation/Public/hs/presentation/environment_layout.hpp"}})
            spec.inputs.push_back(ap::HashInput(std::string(id), path));

        // The compose input list is the complete namespace index. Never scan unrelated Actions.
        std::map<std::string, ap::ActionResultSnapshot> snapshots;
        std::map<std::string, std::string> owners;
        std::istringstream lines(ap::ReadBytes(result_list));
        std::string line;
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            Require(!line.empty(), "Empty entry in bake input result list");
            const fs::path path(line);
            Require(path.is_absolute() && path.filename() == "result.json", "Invalid Action result path in bake input list");
            const auto relative = fs::canonical(path).lexically_relative(build / "Actions");
            Require(!relative.empty() && !relative.is_absolute() && *relative.begin() != "..",
                "Bake input result lies outside build Actions");
            const auto name = relative.parent_path().generic_string();
            Require(name != kNamespace, "Bake input result list contains the output Action");
            auto [snapshot, inserted] = snapshots.try_emplace(name, build, name);
            Require(inserted, "Duplicate Action namespace in bake input result list");
            const auto document = ap::Json::parse(ap::ReadBytes(path));
            for (const auto &product : document.at("products"))
            {
                const auto id = product.at("logical_id").get<std::string>();
                (void)snapshot->second.ProductMetadata(id);
                Require(owners.emplace(id, name).second, "Duplicate product in bake input result list: " + id);
            }
        }
        Require(!owners.empty(), "Empty bake input result list");
        std::set<std::string> dependencies;
        const auto add_closure = [&](std::string root) {
            std::vector<std::string> pending{std::move(root)};
            while (!pending.empty())
            {
                auto id = std::move(pending.back()); pending.pop_back();
                if (!dependencies.insert(id).second) continue;
                fs::path selected;
                const auto resolved = generation.Require(id, selected);
                Require(static_cast<bool>(resolved), std::string(resolved.Message()));
                const auto owner = owners.find(id);
                Require(owner != owners.end(), "Selected bake input has no listed Action owner: " + id);
                const auto &metadata = snapshots.at(owner->second).ProductMetadata(id);
                const auto digest = metadata.at("digest").get<std::string>();
                const auto size = metadata.at("size").get<std::uint64_t>();
                Require(generation.product_digests.at(id) == digest && fs::file_size(selected) == size &&
                    ap::Sha256File(selected) == digest, "Selected bake input differs from its Action product: " + id);
                const auto product_dependencies = metadata.value("dependencies", std::vector<std::string>{});
                const auto selected_dependencies = generation.product_dependencies.find(id);
                Require(selected_dependencies != generation.product_dependencies.end() &&
                    selected_dependencies->second == product_dependencies, "Selected bake dependency closure differs: " + id);
                spec.upstream.push_back({id, digest, size, build / "Actions" / owner->second / "result.json"});
                pending.insert(pending.end(), product_dependencies.begin(), product_dependencies.end());
            }
        };
        // Verify the rules before the shared scene builder decodes them.
        add_closure("game-data/simulation-rules");
        const auto scene = scene_builder(generation);
        Require(scene && !scene->instances.empty(), "Static shadow scene is empty");
        add_closure("render/shader/local_probe_capture_source_base_vs.dxil");
        std::set<std::uint32_t> materials;
        for (const auto &instance : scene->instances)
        {
            if (!rd::IsStaticShadowCaster(instance)) continue;
            auto material = rd::kTerrainMaterialOrdinal;
            if (rd::IsEnvironmentMesh(instance.mesh))
            {
                const auto geometry = rd::EnvironmentGeometryOrdinal(instance.mesh, instance.environment_variant);
                Require(geometry < rd::kEnvironmentMaterialNames.size(), "Invalid static shadow geometry ordinal");
                add_closure("render/" + std::string(rd::kEnvironmentMaterialNames[geometry]) + ".rpkg");
                material = rd::EnvironmentGeometryMaterialOrdinal(geometry);
            }
            materials.insert(material);
        }
        Require(!materials.empty(), "Static shadow scene has no eligible material");
        for (const auto ordinal : materials)
        {
            const std::string name(rd::kAuthoredMaterialNames.at(ordinal));
            add_closure(ordinal == rd::kTerrainMaterialOrdinal || ordinal == rd::kGroundDetailMaterialOrdinal
                ? "render/material/" + name + ".rpkg" : "render/" + name + ".rpkg");
            add_closure("render/material/" + name + "/parameters.rpkg");
            add_closure("render/shader/" + name + "_probe_shadow.dxil");
        }
        std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);
        std::ranges::sort(spec.upstream, {}, &ap::UpstreamProduct::logical_id);
        if (Current(build, spec))
        {
            ap::TouchActionResult(build / "Actions" / kNamespace / "result.json");
            std::cout << kAction << " current\n";
            return 0;
        }
        const auto layout = rd::StaticShadowLayoutDigest(*scene);
        const auto stage = ap::CreateStage(build / "Actions" / kNamespace);
        try
        {
            rd::StaticShadowArchiveIndex index;
            std::vector<std::uint8_t> written;
            std::ofstream payload(stage / kPagesFilename, std::ios::binary | std::ios::trunc);
            Require(static_cast<bool>(payload), "Cannot create static shadow page payload");
            bool begun{}, covered{};
            const auto artifacts = build / "Artifacts" / "static-shadow-cook";
            ap::MakeDirectory(artifacts);
            native_bake(generation, scene, artifacts,
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
                });
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
        catch (...) { ap::RemoveStage(stage); throw; }
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
