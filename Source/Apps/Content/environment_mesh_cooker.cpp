#include "environment_mesh_cooker.hpp"
#include <hs/core/cooked_format.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <set>
#include <stdexcept>

namespace hs::content
{
namespace
{
// Little-endian meshbin: uint32 magic (HSEM), version, vertex count, stride;
// followed by triangle-expanded SkinnedVertex records (76 bytes each).
constexpr std::uint32_t kMagic = 0x4d455348;
constexpr std::array names{"trunk_0", "trunk_1", "trunk_2", "leaf_0", "leaf_1",
    "leaf_2", "rock_0", "rock_1", "rock_2", "rock_3", "grass_0", "grass_1",
    "grass_2", "grass_3"};
struct Mesh { std::string name; std::vector<SkinnedVertex> vertices; };
std::vector<Mesh> ReadMeshes(const std::filesystem::path &path)
{
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("Cannot open environment mesh source: " + path.string());
    const auto root = nlohmann::json::parse(stream);
    if (root.at("version") != 1 || !root.at("meshes").is_array() ||
        root.at("meshes").size() != names.size() * 3)
        throw std::runtime_error("Environment mesh source must contain version 1 and all 42 LOD meshes.");
    std::set<std::string> found;
    std::vector<Mesh> meshes;
    for (const auto &entry : root.at("meshes"))
    {
        Mesh mesh{entry.at("name").get<std::string>(), {}};
        auto base_name = mesh.name;
        if (base_name.ends_with("_lod1") || base_name.ends_with("_lod2")) base_name.resize(base_name.size()-5);
        if (std::ranges::find(names, base_name) == names.end() || !found.insert(mesh.name).second)
            throw std::runtime_error("Unknown or duplicate environment mesh: " + mesh.name);
        const auto &vertices = entry.at("vertices");
        if (!vertices.is_array() || vertices.empty() || vertices.size() % 3 != 0 || vertices.size() > 3'000'000)
            throw std::runtime_error("Invalid environment triangle count: " + mesh.name);
        const int material_count = mesh.name.starts_with("leaf_") ? 18 :
                                   mesh.name.starts_with("grass_") ? 30 :
                                   mesh.name.starts_with("trunk_") ? 3 : 4;
        for (const auto &record : vertices)
        {
            if (!record.is_array() || record.size() != 13)
                throw std::runtime_error("Environment vertex must contain 13 values: " + mesh.name);
            std::array<float, 13> values{};
            for (std::size_t i = 0; i < values.size(); ++i)
            {
                values[i] = record[i].get<float>();
                if (!std::isfinite(values[i])) throw std::runtime_error("Nonfinite environment vertex: " + mesh.name);
            }
            const auto norm = [&](int offset) {
                return values[offset]*values[offset] + values[offset+1]*values[offset+1] + values[offset+2]*values[offset+2];
            };
            const auto dot = values[3]*values[8]+values[4]*values[9]+values[5]*values[10];
            if (std::abs(norm(3)-1.0f) > 0.02f || std::abs(norm(8)-1.0f) > 0.02f ||
                std::abs(dot) > 0.02f || std::abs(std::abs(values[11])-1.0f) > 0.001f ||
                values[12] != std::floor(values[12]) || values[12] < 0 || values[12] >= material_count)
                throw std::runtime_error("Invalid environment normal, tangent, or material: " + mesh.name);
            if ((mesh.name.starts_with("leaf_") || mesh.name.starts_with("grass_")) && (values[6] < 0 || values[6] > 1 || values[7] < 0 || values[7] > 1))
                throw std::runtime_error("Foliage UV outside local 0..1 rectangle: " + mesh.name);
            SkinnedVertex vertex{};
            std::copy_n(values.begin(), 3, vertex.position.begin());
            std::copy_n(values.begin()+3, 3, vertex.normal.begin());
            std::copy_n(values.begin()+6, 2, vertex.uv.begin());
            std::copy_n(values.begin()+8, 4, vertex.tangent.begin());
            vertex.material_index = static_cast<std::uint16_t>(values[12]);
            mesh.vertices.push_back(vertex);
        }
        for (std::size_t i = 0; i < mesh.vertices.size(); i += 3)
        {
            const auto &a = mesh.vertices[i]; const auto &b = mesh.vertices[i+1]; const auto &c = mesh.vertices[i+2];
            std::array<float,3> u{}, v{}, cross{};
            for (int j=0; j<3; ++j) { u[j]=b.position[j]-a.position[j]; v[j]=c.position[j]-a.position[j]; }
            cross = {u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};
            if (cross[0]*cross[0]+cross[1]*cross[1]+cross[2]*cross[2] <= 1e-16f ||
                a.material_index != b.material_index || a.material_index != c.material_index)
                throw std::runtime_error("Degenerate triangle or mixed triangle materials: " + mesh.name);
        }
        meshes.push_back(std::move(mesh));
    }
    return meshes;
}
}
void ValidateEnvironmentMeshes(const std::filesystem::path &source_json) { (void)ReadMeshes(source_json); }
void CookEnvironmentMeshes(const std::filesystem::path &source_json, const std::filesystem::path &output)
{
    const auto meshes = ReadMeshes(source_json);
    std::filesystem::create_directories(output);
    for (const auto &mesh : meshes)
    {
        const std::array<std::uint32_t,4> header{kMagic,1,static_cast<std::uint32_t>(mesh.vertices.size()),sizeof(SkinnedVertex)};
        std::ofstream stream(output / ("environment_" + mesh.name + ".meshbin"), std::ios::binary);
        stream.write(reinterpret_cast<const char *>(header.data()), sizeof(header));
        stream.write(reinterpret_cast<const char *>(mesh.vertices.data()), static_cast<std::streamsize>(mesh.vertices.size()*sizeof(SkinnedVertex)));
        if (!stream) throw std::runtime_error("Cannot write environment mesh: " + mesh.name);
    }
}
}
