#include "environment_mesh_cooker.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace fs = std::filesystem;
using Json = nlohmann::json;
void Check(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}
struct Scratch
{
    fs::path path = fs::temp_directory_path() /
        ("hs-environment-indexed-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { Check(fs::create_directory(path), "create exclusive mesh source test directory"); }
    ~Scratch() { std::error_code ignored; fs::remove_all(path, ignored); }
};
Json Vertex(double x, double z)
{
    return Json::array({x, 0.0, z, 0.0, 1.0, -0.0, 0.0, 0.0, 1.0, 0.0, -0.0, 1.0, 0.0});
}
std::array<Json, 2> Sources(bool indexed)
{
    constexpr std::array names{"trunk_0", "trunk_1", "trunk_2", "leaf_0", "leaf_1",
        "leaf_2", "rock_0", "rock_1", "rock_2", "rock_3", "grass_0", "grass_1",
        "grass_2", "grass_3", "flower_daisy", "flower_buttercup", "flower_violet", "clover_patch",
        "ground_leaves", "twig_cluster", "fence_short", "shrub_low", "stump_roots", "crate_intact",
        "crate_broken", "barrel_intact", "barrel_broken", "cart_broken", "fence_broken", "fence_splitrail", "sack_tied", "fallen_log", "rock_mossy", "meadow_ground", "meadow_water"};
    std::array<Json, 2> sources{Json{{"version", 1}, {"meshes", Json::array()}},
                                Json{{"version", 1}, {"meshes", Json::array()}}};
    const auto vertices = Json::array({Vertex(-0.0, 0.0), Vertex(0.0, 1.0), Vertex(1.0, 1.0), Vertex(1.0, 0.0)});
    const std::array<std::uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
    for (std::size_t name = 0; name < names.size(); ++name)
        for (int lod = 0; lod < 3; ++lod)
        {
            Json mesh{{"name", std::string(names[name]) + (lod ? "_lod" + std::to_string(lod) : "")},
                      {"vertices", Json::array()}};
            auto selected_vertices = vertices;
            if (name >= 28 && name < 33)
                for (auto &vertex : selected_vertices) vertex[12] = 1;
            if (indexed)
            {
                mesh["vertices"] = selected_vertices; // Four unique records need not be a multiple of three.
                mesh["indices"] = indices;
            }
            else
                for (auto index : indices) mesh["vertices"].push_back(selected_vertices[index]);
            sources[name < 33 ? 0 : 1]["meshes"].push_back(std::move(mesh));
        }
    return sources;
}
void Save(const fs::path &directory, const std::array<Json, 2> &sources)
{
    fs::create_directories(directory);
    std::ofstream(directory / "environment_meshes.json") << sources[0].dump();
    std::ofstream(directory / "meadow_surface_meshes.json") << sources[1].dump();
}
std::vector<char> Read(const fs::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    Check(bool(stream), "open cooked mesh oracle output");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void Run()
{
    Scratch scratch;
    const auto legacy = Sources(false), indexed = Sources(true);
    const auto legacy_source = scratch.path / "legacy", indexed_source = scratch.path / "indexed";
    const auto legacy_output = scratch.path / "legacy-output", indexed_output = scratch.path / "indexed-output";
    Save(legacy_source, legacy); Save(indexed_source, indexed);
    hs::content::CookEnvironmentMeshes(legacy_source / "environment_meshes.json", legacy_output);
    hs::content::CookEnvironmentMeshes(indexed_source / "environment_meshes.json", indexed_output);
    std::size_t compared{};
    for (const auto &entry : fs::directory_iterator(legacy_output))
    {
        const auto expected = Read(entry.path());
        Check(expected == Read(indexed_output / entry.path().filename()), "legacy/indexed cooked bytes differ");
        Check(expected.size() == 16 + 6 * 76, "expanded meshbin layout changed");
        // The first position is negative zero; index expansion must copy float32 bits verbatim.
        Check(static_cast<unsigned char>(expected[19]) == 0x80, "negative-zero position was lost");
        const auto filename = entry.path().filename().string();
        constexpr std::array atlas1_names{"fence_broken", "fence_splitrail", "sack_tied", "fallen_log", "rock_mossy"};
        const bool atlas1 = std::ranges::any_of(atlas1_names, [&](const auto name) {
            return filename.starts_with("environment_" + std::string(name));
        });
        Check(static_cast<unsigned char>(expected[16 + 72]) == (atlas1 ? 1u : 0u),
              "shared ground-detail atlas selector was not preserved");
        ++compared;
    }
    Check(compared == 105, "all environment and meadow LOD products compared");
    auto mixed = indexed;
    mixed[0]["meshes"][0] = legacy[0]["meshes"][0];
    mixed[1]["meshes"][0] = legacy[1]["meshes"][0];
    Save(indexed_source, mixed);
    hs::content::CookEnvironmentMeshes(indexed_source / "environment_meshes.json", indexed_output);
    for (const auto &entry : fs::directory_iterator(legacy_output))
        Check(Read(entry.path()) == Read(indexed_output / entry.path().filename()), "mixed v1 source cooked bytes differ");

    const auto reject = [&](std::string_view expected, auto mutate, std::size_t mesh_index = 0) {
        auto invalid = indexed;
        mutate(invalid[0]["meshes"][mesh_index]);
        Save(indexed_source, invalid);
        bool rejected{};
        try { hs::content::ValidateEnvironmentMeshes(indexed_source / "environment_meshes.json"); }
        catch (const std::exception &error)
        {
            rejected = true;
            Check(std::string_view(error.what()).find(expected) != std::string_view::npos,
                "invalid source failed for an unexpected reason");
        }
        Check(rejected, "malformed indexed source was accepted");
    };
    reject("index count", [](Json &mesh) { mesh["indices"] = nullptr; });
    reject("index count", [](Json &mesh) { mesh["indices"] = Json::object(); });
    reject("index count", [](Json &mesh) { mesh["indices"] = Json::array(); });
    reject("index count", [](Json &mesh) { mesh["indices"].erase(mesh["indices"].end() - 1); });
    reject("Invalid environment index:", [](Json &mesh) { mesh["indices"][0] = -1; });
    reject("Invalid environment index:", [](Json &mesh) { mesh["indices"][0] = 0.0; });
    reject("Invalid environment index:", [](Json &mesh) { mesh["indices"][0] = true; });
    reject("Invalid environment index range", [](Json &mesh) { mesh["indices"][0] = 4; });
    reject("Invalid environment index range", [](Json &mesh) { mesh["indices"][0] = std::numeric_limits<std::uint64_t>::max(); });
    reject("index count", [](Json &mesh) {
        mesh["indices"] = Json::array_t(3'000'003, Json(0));
    });
    reject("13 values", [](Json &mesh) { mesh["vertices"][0].erase(mesh["vertices"][0].end() - 1); });
    reject("Nonfinite environment vertex", [](Json &mesh) { mesh["vertices"][0][0] = 1e40; });
    reject("indexed environment material", [](Json &mesh) { mesh["vertices"][0][12] = true; });
    reject("indexed environment material", [](Json &mesh) { mesh["vertices"][0][12] = 1.0000000001; });
    reject("indexed environment material", [](Json &mesh) { mesh["vertices"][0][12] = -1; });
    reject("indexed environment material", [](Json &mesh) { mesh["vertices"][0][12] = 3; });
    reject("indexed environment material", [](Json &mesh) { mesh["vertices"][0][12] = 2; }, 28 * 3);
    reject("indexed environment material", [](Json &mesh) { mesh["vertices"][0][12] = 2; }, 32 * 3);
    reject("indexed environment material", [](Json &mesh) { mesh["vertices"][0][12] = 3; }, 32 * 3);
    reject("indexed environment material", [](Json &mesh) { mesh["vertices"][0][12] = 1; }, 14 * 3);
    reject("normal, tangent", [](Json &mesh) { mesh["vertices"][0][4] = 0; });
    reject("normal, tangent", [](Json &mesh) { mesh["vertices"][0][11] = 0; });
    reject("Foliage UV", [](Json &mesh) { mesh["vertices"][0][6] = 1.1; }, 9);
    reject("Degenerate triangle", [](Json &mesh) { mesh["indices"] = Json::array({0, 0, 2}); });
    reject("mixed triangle materials", [](Json &mesh) { mesh["vertices"][1][12] = 1; });
    reject("Nonfinite environment vertex", [](Json &mesh) {
        auto unused = mesh["vertices"][0]; unused[0] = 1e40; mesh["vertices"].push_back(std::move(unused));
    });
    reject("triangle count", [](Json &mesh) { mesh.erase("indices"); });
    std::cout << "Environment indexed source: 105 exact cooked products, mixed legacy/indexed, and malformed input checks passed.\n";
}
}
int main()
{
    try { Run(); return 0; }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}