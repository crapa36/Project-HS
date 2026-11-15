#include "../Environment/producer_inputs.hpp"
#include "../Content/environment_mesh_cooker.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <iterator>
#include <string_view>

namespace
{
namespace fs = std::filesystem;
namespace ap = hs::asset_pipeline;
using namespace hs::environment_producer;

std::vector<std::string> MeshProductNames()
{
    constexpr std::array names{"trunk_0", "trunk_1", "trunk_2", "leaf_0", "leaf_1",
        "leaf_2", "rock_0", "rock_1", "rock_2", "rock_3", "grass_0", "grass_1",
        "grass_2", "grass_3", "flower_daisy", "flower_buttercup", "flower_violet", "clover_patch",
        "ground_leaves", "twig_cluster", "fence_short", "shrub_low", "stump_roots", "crate_intact",
        "crate_broken", "barrel_intact", "barrel_broken", "cart_broken", "fence_broken", "fence_splitrail",
        "sack_tied", "fallen_log", "rock_mossy", "meadow_ground", "meadow_water"};
    std::vector<std::string> products;
    for (const auto *name : names) for (const auto *lod : {"", "_lod1", "_lod2"})
        products.push_back("environment_" + std::string(name) + lod + ".meshbin");
    return products;
}

void Produce(const fs::path &source_root, const fs::path &build_root)
{
    Require(source_root.is_absolute() && build_root.is_absolute(), "source and build roots must be absolute");
    ap::RequireDirectory(source_root);
    ap::MakeDirectory(build_root);
    const auto mesh_source = source_root / "ContentSource/Models/Environment/environment_meshes.json";
    ap::ActionSpec spec;
    spec.action_id = "environment.meshes";
    spec.recipe_id = "project-hs.environment.meshes";
    spec.recipe_version = 1;
    spec.namespace_path = "environment/meshes";
    auto names = MeshProductNames();
    AddInputs(spec, source_root, {mesh_source, mesh_source.parent_path() / "meadow_surface_meshes.json"});
    std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);
    if (IsCurrent(build_root, spec, names)) {
        ap::TouchActionResult(build_root / "Actions" / spec.namespace_path / "result.json");
        return;
    }
    const auto input_stage = ap::CreateStage(build_root / "Actions" / spec.namespace_path);
    try {
        InputSnapshot snapshot{source_root, input_stage, {}};
        const auto frozen_mesh_source = input_stage / "ContentSource/Models/Environment/environment_meshes.json";
        snapshot.Capture(frozen_mesh_source);
        snapshot.Capture(frozen_mesh_source.parent_path() / "meadow_surface_meshes.json");
        snapshot.SetInputs(spec, {frozen_mesh_source, frozen_mesh_source.parent_path() / "meadow_surface_meshes.json"});
        if (IsCurrent(build_root, spec, names)) {
            ap::TouchActionResult(build_root / "Actions" / spec.namespace_path / "result.json");
            ap::RemoveStage(input_stage);
            return;
        }
        const auto stage = ap::CreateStage(build_root / "Actions" / spec.namespace_path);
        try {
            hs::content::CookEnvironmentMeshes(frozen_mesh_source, stage);
            std::vector<ap::Product> products;
            for (const auto &name : names)
                products.push_back(ap::HashProduct("environment/" + name, name, stage / name));
            const auto staged_count = std::distance(fs::directory_iterator(stage), fs::directory_iterator{});
            Require(staged_count == static_cast<std::ptrdiff_t>(names.size()), "producer wrote unexpected staged files");
            ap::Publish(build_root, spec, stage, std::move(products));
        } catch (...) {
            ap::RemoveStageAfterFailure(stage);
            throw;
        }
        ap::RemoveStage(stage);
        ap::RemoveStage(input_stage);
        std::cout << spec.action_id << " published\n";
    } catch (...) {
        ap::RemoveStageAfterFailure(input_stage);
        throw;
    }
}
}

int wmain(int argc, wchar_t **argv)
{
    fs::path source_root, build_root;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::wstring_view arg = argv[i];
            if (arg == L"--source-root" && source_root.empty() && ++i < argc)
                source_root = fs::path(argv[i]);
            else if (arg == L"--build-root" && build_root.empty() && ++i < argc)
                build_root = fs::path(argv[i]);
            else throw std::runtime_error("hs_environment_mesh_cook: invalid command line");
        }
        Require(!source_root.empty() && !build_root.empty(),
            "usage: hs_environment_mesh_cook --source-root <absolute> --build-root <absolute>");
        Produce(source_root.lexically_normal(), build_root.lexically_normal());
        return 0;
    } catch (const std::exception &error) {
        ap::ReportFailure("hs_environment_mesh_cook", error.what(),
            {{"action_id", "environment.meshes"}, {"source_root", ap::DiagnosticPath(source_root)},
             {"build_root", ap::DiagnosticPath(build_root)}});
        return 1;
    }
}
