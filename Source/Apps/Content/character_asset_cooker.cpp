#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "content_cooker.hpp"
#include "character_texture_mips.hpp"
#include "character_bc7_coverage.hpp"
#include "character_bc7_normal.hpp"
#include "../CharacterContent/character_sources.hpp"

#include <hs/render_data/material_schema.hpp>
#include <hs/render_data/dds_texture.hpp>

#include <fbxsdk.h>
#include <d3d11.h>
#include <dxgi.h>
#include <DirectXTex.h>
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hs::content
{

namespace
{
bool AtomicWriteCharacterFile(const std::filesystem::path &path,
                              std::span<const std::byte> bytes,
                              std::string &error_message)
{
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream.write(reinterpret_cast<const char *>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size())))
        {
            error_message = "cannot write temporary file";
            return false;
        }
    }
    const auto handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        error_message = "cannot open temporary file for flush";
        DeleteFileW(temporary.c_str());
        return false;
    }
    const auto flushed = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!flushed || !MoveFileExW(temporary.c_str(), path.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        error_message = "atomic replacement failed with Win32 error " +
                        std::to_string(GetLastError());
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}
} // namespace

FbxScene *LoadFbx(FbxManager &manager, const std::filesystem::path &path,
                  std::string_view scene_name)
{
    auto *scene = FbxScene::Create(&manager, std::string(scene_name).c_str());
    auto *importer = FbxImporter::Create(&manager, "");
    const auto initialized = importer->Initialize(path.string().c_str(), -1,
                                                  manager.GetIOSettings());
    const auto imported = initialized && importer->Import(scene);
    const auto error = std::string(importer->GetStatus().GetErrorString());
    importer->Destroy();
    if (!imported)
    {
        scene->Destroy();
        throw std::runtime_error(path.string() + ": FBX import failed: " + error);
    }
    FbxAxisSystem::DirectX.ConvertScene(scene);
    FbxSystemUnit::m.ConvertScene(scene);
    return scene;
}

void VisitNodes(FbxNode *node, const auto &visitor)
{
    visitor(node);
    for (int index = 0; index < node->GetChildCount(); ++index)
    {
        VisitNodes(node->GetChild(index), visitor);
    }
}

FbxAMatrix GeometryTransform(FbxNode &node)
{
    FbxAMatrix matrix;
    matrix.SetT(node.GetGeometricTranslation(FbxNode::eSourcePivot));
    matrix.SetR(node.GetGeometricRotation(FbxNode::eSourcePivot));
    matrix.SetS(node.GetGeometricScaling(FbxNode::eSourcePivot));
    return matrix;
}

std::array<float, 16> RuntimeMatrix(const FbxAMatrix &matrix)
{
    std::array<float, 16> output{};
    for (std::size_t row = 0; row < 4; ++row)
    {
        for (std::size_t column = 0; column < 4; ++column)
        {
            output[row * 4 + column] =
                static_cast<float>(matrix.Get(static_cast<int>(column),
                                              static_cast<int>(row)));
        }
    }
    return output;
}

std::string BoneName(const FbxNode &node)
{
    return node.GetNameOnly().Buffer();
}

struct BoneSource
{
    std::string name;
    FbxNode *node{};
    FbxAMatrix global_bind;
    std::size_t parent{std::numeric_limits<std::size_t>::max()};
};

std::filesystem::path MaterialTexture(FbxSurfaceMaterial &material,
                                      const char *property_name,
                                      const std::filesystem::path &model_path,
                                      bool allow_missing)
{
    const auto property = material.FindProperty(property_name);
    if (!property.IsValid())
    {
        return {};
    }
    auto *texture = property.GetSrcObject<FbxFileTexture>(0);
    if (!texture)
    {
        return {};
    }
    const auto filename = std::filesystem::path(texture->GetFileName()).filename();
    const auto path = model_path.parent_path() /
                      (model_path.stem().string() + ".fbm") / filename;
    if (!std::filesystem::is_regular_file(path))
    {
        if (allow_missing)
            return {};
        throw std::runtime_error("Character material texture is missing: " + path.string());
    }
    return path;
}

std::uint16_t GatherMaterial(FbxSurfaceMaterial &material,
                             CharacterIR &output,
                             const std::filesystem::path &model_path,
                             bool allow_missing)
{
    auto diffuse = MaterialTexture(material, FbxSurfaceMaterial::sDiffuse,
                                   model_path, allow_missing);
    auto normal = MaterialTexture(material, FbxSurfaceMaterial::sNormalMap,
                                  model_path, allow_missing);
    if (normal.empty())
    {
        normal = MaterialTexture(material, FbxSurfaceMaterial::sBump,
                                 model_path, allow_missing);
    }
    const auto name = std::string(material.GetName());
    const auto existing = std::ranges::find_if(
        output.materials, [&](const CharacterIR::Material &candidate) {
            return candidate.diffuse == diffuse && candidate.normal == normal &&
                   candidate.name == name;
        });
    if (existing != output.materials.end())
    {
        return static_cast<std::uint16_t>(existing - output.materials.begin());
    }
    if (output.materials.size() == hs::kMaxCharacterMaterials)
    {
        throw std::runtime_error("Archer exceeds the 8 material limit");
    }
    output.materials.push_back({std::move(diffuse), std::move(normal), name});
    return static_cast<std::uint16_t>(output.materials.size() - 1);
}

std::uint16_t PolygonMaterial(FbxMesh &mesh, int polygon,
                              std::span<const std::uint16_t> node_materials)
{
    auto *element = mesh.GetElementMaterial();
    if (!element || node_materials.empty())
    {
        return 0;
    }
    const auto mapped = element->GetMappingMode() == FbxGeometryElement::eAllSame
                            ? 0
                            : polygon;
    const auto local = element->GetIndexArray().GetAt(mapped);
    if (local < 0 || static_cast<std::size_t>(local) >= node_materials.size())
    {
        throw std::runtime_error("Archer polygon has an invalid material index");
    }
    return node_materials[static_cast<std::size_t>(local)];
}

void GatherModel(FbxScene &scene, CharacterIR &output,
                 std::vector<BoneSource> &bones,
                 const std::filesystem::path &model_path,
                 bool allow_missing_material_textures, bool ground_archer)
{
    FbxGeometryConverter converter(scene.GetFbxManager());
    if (!converter.Triangulate(&scene, true, false))
    {
        throw std::runtime_error("Archer FBX triangulation failed");
    }

    std::vector<FbxNode *> mesh_nodes;
    std::unordered_set<FbxNode *> weighted_bones;
    std::unordered_map<FbxNode *, FbxAMatrix> bind_by_bone;
    VisitNodes(scene.GetRootNode(), [&](FbxNode *node) {
        auto *mesh = node->GetMesh();
        if (!mesh)
        {
            return;
        }
        // This FBX has permuted node names: Arrow_Mesh is the clothed body,
        // while Eyes_Mesh carries the independent arrow. Identify the arrow
        // by its authored material, before collecting its skin-only bones.
        bool arrow_only = ground_archer && node->GetMaterialCount() > 0;
        for (int index = 0; index < node->GetMaterialCount(); ++index)
        {
            const auto *material = node->GetMaterial(index);
            arrow_only = arrow_only && material &&
                         std::string_view(material->GetName()) == "Arrow_MAT";
        }
        if (arrow_only)
        {
            return;
        }
        mesh_nodes.push_back(node);
        for (int skin_index = 0;
             skin_index < mesh->GetDeformerCount(FbxDeformer::eSkin); ++skin_index)
        {
            auto *skin = static_cast<FbxSkin *>(
                mesh->GetDeformer(skin_index, FbxDeformer::eSkin));
            for (int cluster_index = 0; cluster_index < skin->GetClusterCount();
                 ++cluster_index)
            {
                auto *cluster = skin->GetCluster(cluster_index);
                auto *link = cluster->GetLink();
                if (!link || cluster->GetLinkMode() == FbxCluster::eAdditive)
                {
                    throw std::runtime_error(
                        "Archer FBX requires non-additive linked skin clusters");
                }
                weighted_bones.insert(link);
                FbxAMatrix link_bind;
                cluster->GetTransformLinkMatrix(link_bind);
                bind_by_bone.try_emplace(link, link_bind);
            }
        }
    });
    if (mesh_nodes.empty() || weighted_bones.empty())
    {
        throw std::runtime_error("Archer FBX has no skinned mesh");
    }

    VisitNodes(scene.GetRootNode(), [&](FbxNode *node) {
        if (weighted_bones.contains(node))
        {
            bones.push_back(
                {BoneName(*node), node, bind_by_bone.at(node)});
        }
    });
    if (bones.empty() || bones.size() > hs::kMaxCharacterBones)
    {
        throw std::runtime_error("Archer skeleton bone count must be 1..128");
    }
    std::unordered_map<FbxNode *, std::uint16_t> bone_indices;
    std::unordered_set<std::string> bone_names;
    for (std::size_t index = 0; index < bones.size(); ++index)
    {
        if (!bone_indices.emplace(bones[index].node,
                                  static_cast<std::uint16_t>(index)).second)
        {
            throw std::runtime_error("Archer skeleton contains a duplicate bone");
        }
        if (!bone_names.emplace(bones[index].name).second)
            throw std::runtime_error("Character skeleton contains a duplicate bone name");
    }
    for (auto &bone : bones)
    {
        for (auto *parent = bone.node->GetParent(); parent; parent = parent->GetParent())
        {
            if (const auto iterator = bone_indices.find(parent);
                iterator != bone_indices.end())
            {
                bone.parent = iterator->second;
                break;
            }
        }
    }
    output.parents.reserve(bones.size());
    output.inverse_bind_matrices.reserve(bones.size());
    for (const auto &bone : bones)
    {
        if (bone.parent != std::numeric_limits<std::size_t>::max() &&
            bone.parent >= output.parents.size())
        {
            throw std::runtime_error("Archer skeleton parent order is invalid");
        }
        output.parents.push_back(
            bone.parent == std::numeric_limits<std::size_t>::max()
                ? std::numeric_limits<std::uint16_t>::max()
                : static_cast<std::uint16_t>(bone.parent));
        output.inverse_bind_matrices.push_back(
            RuntimeMatrix(bone.global_bind.Inverse()));
    }
    for (auto *node : mesh_nodes)
    {
        auto *mesh = node->GetMesh();
        FbxStringList uv_sets;
        mesh->GetUVSetNames(uv_sets);
        if (uv_sets.GetCount() == 0)
        {
            throw std::runtime_error("Archer mesh has no UV set");
        }
        std::vector<std::uint16_t> node_materials;
        node_materials.reserve(static_cast<std::size_t>(node->GetMaterialCount()));
        for (int material_index = 0; material_index < node->GetMaterialCount();
             ++material_index)
        {
            auto *material = node->GetMaterial(material_index);
            if (!material)
            {
                throw std::runtime_error("Archer node has an empty material slot");
            }
            node_materials.push_back(GatherMaterial(
                *material, output, model_path, allow_missing_material_textures));
        }
        if (node_materials.empty())
        {
            if (output.materials.empty())
            {
                output.materials.push_back({{}, {}, "Default"});
            }
            node_materials.push_back(0);
        }
        std::vector<std::vector<std::pair<std::uint16_t, float>>> weights(
            static_cast<std::size_t>(mesh->GetControlPointsCount()));
        FbxAMatrix mesh_bind;
        bool has_mesh_bind{};
        for (int skin_index = 0;
             skin_index < mesh->GetDeformerCount(FbxDeformer::eSkin); ++skin_index)
        {
            auto *skin = static_cast<FbxSkin *>(
                mesh->GetDeformer(skin_index, FbxDeformer::eSkin));
            for (int cluster_index = 0; cluster_index < skin->GetClusterCount();
                 ++cluster_index)
            {
                auto *cluster = skin->GetCluster(cluster_index);
                if (!has_mesh_bind)
                {
                    cluster->GetTransformMatrix(mesh_bind);
                    has_mesh_bind = true;
                }
                const auto bone = bone_indices.at(cluster->GetLink());
                const auto *control_points = cluster->GetControlPointIndices();
                const auto *control_weights = cluster->GetControlPointWeights();
                for (int weight_index = 0;
                     weight_index < cluster->GetControlPointIndicesCount(); ++weight_index)
                {
                    const auto control_point = control_points[weight_index];
                    if (control_point < 0 || control_point >= mesh->GetControlPointsCount())
                    {
                        throw std::runtime_error(
                            model_path.string() + ": node " + node->GetNameOnly().Buffer() +
                            " has an invalid skin control point index");
                    }
                    const auto weight = control_weights[weight_index];
                    if (!std::isfinite(weight) || weight < 0.0)
                    {
                        throw std::runtime_error(
                            model_path.string() + ": node " + node->GetNameOnly().Buffer() +
                            " has a non-finite or negative skin weight");
                    }
                    if (weight == 0.0)
                    {
                        continue;
                    }
                    weights[static_cast<std::size_t>(control_point)].push_back(
                        {bone, static_cast<float>(weight)});
                }
            }
        }
        if (!has_mesh_bind)
        {
            throw std::runtime_error("Archer mesh has no skin bind transform");
        }
        mesh_bind *= GeometryTransform(*node);
        const auto normal_matrix = mesh_bind.Inverse().Transpose();
        for (int polygon = 0; polygon < mesh->GetPolygonCount(); ++polygon)
        {
            if (mesh->GetPolygonSize(polygon) != 3)
            {
                throw std::runtime_error("Archer FBX contains a non-triangle polygon");
            }
            std::array<hs::SkinnedVertex, 3> triangle{};
            for (int corner = 0; corner < 3; ++corner)
            {
                const auto control_point = mesh->GetPolygonVertex(polygon, corner);
                const auto position = mesh_bind.MultT(mesh->GetControlPointAt(control_point));
                FbxVector4 normal;
                if (!mesh->GetPolygonVertexNormal(polygon, corner, normal))
                {
                    throw std::runtime_error("Archer FBX contains a vertex without a normal");
                }
                normal[3] = 0.0;
                normal = normal_matrix.MultT(normal);
                normal.Normalize();

                auto &vertex = triangle[static_cast<std::size_t>(corner)];
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    vertex.position[axis] =
                        static_cast<float>(position[static_cast<int>(axis)]);
                    vertex.normal[axis] =
                        static_cast<float>(normal[static_cast<int>(axis)]);
                    output.bounds_min[axis] =
                        std::min(output.bounds_min[axis], vertex.position[axis]);
                    output.bounds_max[axis] =
                        std::max(output.bounds_max[axis], vertex.position[axis]);
                }
                auto influences = weights[static_cast<std::size_t>(control_point)];
                std::ranges::sort(influences, std::greater{}, &decltype(influences)::value_type::second);
                if (influences.size() > vertex.bone_indices.size())
                {
                    influences.resize(vertex.bone_indices.size());
                }
                if (influences.empty())
                {
                    throw std::runtime_error(
                        model_path.string() + ": node " + node->GetNameOnly().Buffer() +
                        " has an unweighted skin control point " +
                        std::to_string(control_point));
                }
                float total{};
                for (const auto &influence : influences)
                {
                    if (influence.first >= bones.size() ||
                        !std::isfinite(influence.second) || influence.second <= 0.0f)
                    {
                        throw std::runtime_error(
                            model_path.string() + ": node " + node->GetNameOnly().Buffer() +
                            " has an invalid selected skin influence at control point " +
                            std::to_string(control_point));
                    }
                    total += influence.second;
                }
                if (!std::isfinite(total) || total <= 0.0f)
                {
                    throw std::runtime_error(
                        model_path.string() + ": node " + node->GetNameOnly().Buffer() +
                        " has an invalid skin influence total at control point " +
                        std::to_string(control_point));
                }
                for (std::size_t index = 0; index < influences.size(); ++index)
                {
                    vertex.bone_indices[index] = influences[index].first;
                    vertex.bone_weights[index] = influences[index].second / total;
                }
                FbxVector2 uv;
                bool unmapped{};
                if (!mesh->GetPolygonVertexUV(polygon, corner, uv_sets[0], uv,
                                              unmapped) || unmapped)
                {
                    throw std::runtime_error("Archer polygon has an unmapped UV");
                }
                vertex.uv = {static_cast<float>(uv[0]),
                             1.0f - static_cast<float>(uv[1])};
                vertex.material_index =
                    PolygonMaterial(*mesh, polygon, node_materials);
            }
            // FBX exports can flip triangle winding when converted to the
            // DirectX axis system.  Keep the authored normal and rasterizer
            // convention consistent so back-face culling does not make a
            // valid skinned surface appear perforated.
            auto edge1 = FbxVector4(
                triangle[1].position[0] - triangle[0].position[0],
                triangle[1].position[1] - triangle[0].position[1],
                triangle[1].position[2] - triangle[0].position[2]);
            auto edge2 = FbxVector4(
                triangle[2].position[0] - triangle[0].position[0],
                triangle[2].position[1] - triangle[0].position[1],
                triangle[2].position[2] - triangle[0].position[2]);
            const auto geometric_normal = edge1.CrossProduct(edge2);
            const auto authored_normal = FbxVector4(
                triangle[0].normal[0] + triangle[1].normal[0] + triangle[2].normal[0],
                triangle[0].normal[1] + triangle[1].normal[1] + triangle[2].normal[1],
                triangle[0].normal[2] + triangle[1].normal[2] + triangle[2].normal[2]);
            if (geometric_normal.DotProduct(authored_normal) < 0.0)
            {
                std::swap(triangle[1], triangle[2]);
                edge1 = FbxVector4(
                    triangle[1].position[0] - triangle[0].position[0],
                    triangle[1].position[1] - triangle[0].position[1],
                    triangle[1].position[2] - triangle[0].position[2]);
                edge2 = FbxVector4(
                    triangle[2].position[0] - triangle[0].position[0],
                    triangle[2].position[1] - triangle[0].position[1],
                    triangle[2].position[2] - triangle[0].position[2]);
            }
            const auto duv1 = FbxVector2(triangle[1].uv[0] - triangle[0].uv[0],
                                         triangle[1].uv[1] - triangle[0].uv[1]);
            const auto duv2 = FbxVector2(triangle[2].uv[0] - triangle[0].uv[0],
                                         triangle[2].uv[1] - triangle[0].uv[1]);
            const auto denominator = duv1[0] * duv2[1] - duv1[1] * duv2[0];
            FbxVector4 tangent{1.0, 0.0, 0.0, 0.0};
            FbxVector4 bitangent{0.0, 0.0, 1.0, 0.0};
            if (std::abs(denominator) > 1e-10)
            {
                const auto inverse = 1.0 / denominator;
                tangent = (edge1 * duv2[1] - edge2 * duv1[1]) * inverse;
                bitangent = (edge2 * duv1[0] - edge1 * duv2[0]) * inverse;
                tangent.Normalize();
                bitangent.Normalize();
            }
            for (auto &vertex : triangle)
            {
                FbxVector4 vertex_normal(vertex.normal[0], vertex.normal[1],
                                         vertex.normal[2], 0.0);
                auto orthogonal_tangent =
                    tangent - vertex_normal * vertex_normal.DotProduct(tangent);
                orthogonal_tangent.Normalize();
                const auto handedness =
                    vertex_normal.CrossProduct(orthogonal_tangent).DotProduct(bitangent) < 0.0
                        ? -1.0f
                        : 1.0f;
                vertex.tangent = {static_cast<float>(orthogonal_tangent[0]),
                                  static_cast<float>(orthogonal_tangent[1]),
                                  static_cast<float>(orthogonal_tangent[2]), handedness};
                output.vertices.push_back(vertex);
            }
        }
    }
    output.mesh_count = static_cast<std::uint32_t>(mesh_nodes.size());
    output.bone_count = static_cast<std::uint32_t>(bones.size());
}

void SealSlimeEyeSocket(CharacterIR &asset)
{
    using Position = std::array<float, 3>;
    using Edge = std::pair<std::size_t, std::size_t>;
    struct EdgeUse
    {
        std::size_t count{};
        std::size_t from{};
        std::size_t to{};
    };

    if (asset.vertices.size() % 3 != 0)
        throw std::runtime_error("Slime mesh is not a triangle list");

    std::map<Position, std::size_t> position_ids;
    std::vector<hs::SkinnedVertex> representatives;
    std::vector<std::size_t> vertex_ids;
    vertex_ids.reserve(asset.vertices.size());
    for (const auto &vertex : asset.vertices)
    {
        const Position position{vertex.position[0], vertex.position[1],
                                vertex.position[2]};
        const auto [found, inserted] = position_ids.try_emplace(
            position, representatives.size());
        if (inserted) representatives.push_back(vertex);
        vertex_ids.push_back(found->second);
    }

    std::map<Edge, EdgeUse> edges;
    for (std::size_t triangle = 0; triangle < vertex_ids.size(); triangle += 3)
    {
        for (std::size_t corner = 0; corner < 3; ++corner)
        {
            const auto from = vertex_ids[triangle + corner];
            const auto to = vertex_ids[triangle + (corner + 1) % 3];
            const Edge key{std::min(from, to), std::max(from, to)};
            auto &[count, first_from, first_to] = edges[key];
            if (count == 0)
            {
                first_from = from;
                first_to = to;
            }
            ++count;
        }
    }

    std::vector<std::vector<std::size_t>> adjacency(representatives.size());
    for (const auto &[edge, use] : edges)
    {
        if (use.count != 1) continue;
        adjacency[edge.first].push_back(edge.second);
        adjacency[edge.second].push_back(edge.first);
    }

    std::vector<bool> visited(representatives.size());
    std::vector<std::vector<std::size_t>> candidates;
    for (std::size_t vertex = 0; vertex < adjacency.size(); ++vertex)
    {
        if (visited[vertex] || adjacency[vertex].empty()) continue;
        std::vector<std::size_t> component;
        std::vector<std::size_t> pending{vertex};
        visited[vertex] = true;
        while (!pending.empty())
        {
            const auto current = pending.back();
            pending.pop_back();
            component.push_back(current);
            for (const auto neighbor : adjacency[current])
            {
                if (!visited[neighbor])
                {
                    visited[neighbor] = true;
                    pending.push_back(neighbor);
                }
            }
        }
        if (component.size() == 10 &&
            std::ranges::all_of(component, [&](const auto index) {
                return adjacency[index].size() == 2;
            }))
            candidates.push_back(std::move(component));
    }
    if (candidates.size() != 1)
        throw std::runtime_error("Slime mesh must contain exactly one 10-edge eye socket");

    const auto start = *std::ranges::min_element(candidates.front());
    std::vector<std::size_t> loop;
    loop.reserve(10);
    auto previous = std::numeric_limits<std::size_t>::max();
    auto current = start;
    do
    {
        loop.push_back(current);
        const auto &neighbors = adjacency[current];
        const auto next = neighbors[0] == previous ? neighbors[1] : neighbors[0];
        previous = current;
        current = next;
    } while (current != start && loop.size() <= 10);
    if (loop.size() != 10 || current != start)
        throw std::runtime_error("Slime eye socket boundary is not one closed loop");

    const Edge first_key{std::min(loop[0], loop[1]), std::max(loop[0], loop[1])};
    const auto &first_use = edges.at(first_key);
    if (first_use.from == loop[0] && first_use.to == loop[1])
        std::reverse(loop.begin() + 1, loop.end());

    const auto before = asset.vertices.size();
    for (std::size_t index = 1; index + 1 < loop.size(); ++index)
    {
        std::array triangle{representatives[loop[0]],
                            representatives[loop[index]],
                            representatives[loop[index + 1]]};
        const auto edge1 = FbxVector4(
            triangle[1].position[0] - triangle[0].position[0],
            triangle[1].position[1] - triangle[0].position[1],
            triangle[1].position[2] - triangle[0].position[2]);
        const auto edge2 = FbxVector4(
            triangle[2].position[0] - triangle[0].position[0],
            triangle[2].position[1] - triangle[0].position[1],
            triangle[2].position[2] - triangle[0].position[2]);
        auto normal = edge1.CrossProduct(edge2);
        if (normal.Length() <= 1e-8)
            throw std::runtime_error("Slime eye socket repair produced a degenerate triangle");
        normal.Normalize();
        auto tangent = edge1 - normal * normal.DotProduct(edge1);
        tangent.Normalize();
        for (auto &vertex : triangle)
        {
            vertex.normal = {static_cast<float>(normal[0]),
                             static_cast<float>(normal[1]),
                             static_cast<float>(normal[2])};
            vertex.tangent = {static_cast<float>(tangent[0]),
                              static_cast<float>(tangent[1]),
                              static_cast<float>(tangent[2]), 1.0f};
            asset.vertices.push_back(vertex);
        }
    }
    if (asset.vertices.size() != before + 24)
        throw std::runtime_error("Slime eye socket repair did not add eight triangles");
}

FbxNode *FindBone(FbxScene &scene, std::string_view name)
{
    FbxNode *result{};
    VisitNodes(scene.GetRootNode(), [&](FbxNode *node) {
        if (!result && BoneName(*node) == name)
        {
            result = node;
        }
    });
    return result;
}

void GatherAnimation(FbxScene &scene, const std::vector<BoneSource> &model_bones,
                     hs::CharacterAnimationClip clip, bool looping,
                     CharacterIR &output,
                     const std::filesystem::path &animation_path, bool ground_archer)
{
    auto *stack = scene.GetSrcObjectCount<FbxAnimStack>() > 0
                      ? scene.GetSrcObject<FbxAnimStack>(0)
                      : nullptr;
    if (!stack)
    {
        throw std::runtime_error("Animation FBX has no animation stack");
    }
    scene.SetCurrentAnimationStack(stack);
    const auto *take = scene.GetTakeInfo(stack->GetName());
    FbxTimeSpan span;
    if (take)
    {
        span = take->mLocalTimeSpan;
    }
    else
    {
        scene.GetGlobalSettings().GetTimelineDefaultTimeSpan(span);
    }
    const auto duration = span.GetDuration().GetSecondDouble();
    if (!(duration > 0.0) || duration > 30.0)
    {
        throw std::runtime_error("Animation duration must be within 0..30 seconds");
    }
    std::vector<FbxNode *> animation_bones;
    std::vector<FbxAMatrix> animation_global_bind;
    animation_bones.reserve(model_bones.size());
    animation_global_bind.reserve(model_bones.size());
    std::unordered_set<std::string> required_names;
    for (const auto &bone : model_bones)
    {
        FbxNode *animation_bone{};
        std::size_t matches{};
        VisitNodes(scene.GetRootNode(), [&](FbxNode *node) {
            if (BoneName(*node) == bone.name)
            {
                animation_bone = node;
                ++matches;
            }
        });
        if (!animation_bone)
        {
            throw std::runtime_error("Animation skeleton is missing bone: " + bone.name);
        }
        if (matches != 1)
            throw std::runtime_error("Animation skeleton contains a duplicate bone name: " +
                                     bone.name);
        animation_bones.push_back(animation_bone);
        animation_global_bind.push_back(animation_bone->EvaluateGlobalTransform(
            FBXSDK_TIME_INFINITE, FbxNode::eSourcePivot, false, true));
        required_names.insert(bone.name);
    }
    for (std::size_t index = 0; index < model_bones.size(); ++index)
    {
        std::string nearest_parent;
        for (auto *parent = animation_bones[index]->GetParent(); parent;
             parent = parent->GetParent())
        {
            if (required_names.contains(BoneName(*parent)))
            {
                nearest_parent = BoneName(*parent);
                break;
            }
        }
        const auto expected_parent = model_bones[index].parent ==
                                             std::numeric_limits<std::size_t>::max()
                                         ? std::string{}
                                         : model_bones[model_bones[index].parent].name;
        if (nearest_parent != expected_parent)
            throw std::runtime_error("Animation skeleton parent hierarchy mismatch for bone: " +
                                     model_bones[index].name);

    }

    const auto frame_count =
        std::max<std::uint32_t>(2, static_cast<std::uint32_t>(std::ceil(duration * 60.0)) + 1);
    if (output.transforms.size() > 800'000 || model_bones.empty() ||
        frame_count > (800'000 - output.transforms.size()) / model_bones.size())
        throw std::runtime_error("Character animation transform budget exceeded");
    hs::CharacterClipHeader header;
    header.clip = clip;
    header.looping = looping;
    header.first_transform = static_cast<std::uint32_t>(output.transforms.size());
    header.frame_count = frame_count;
    header.duration_seconds = static_cast<float>(duration);
    output.clips.push_back(header);

    auto *evaluator = scene.GetAnimationEvaluator();
    std::vector<FbxAMatrix> model_local_bind(model_bones.size());
    std::vector<FbxAMatrix> animation_local_bind(model_bones.size());
    for (std::size_t bone_index = 0; bone_index < model_bones.size(); ++bone_index)
    {
        const auto parent = model_bones[bone_index].parent;
        if (parent == std::numeric_limits<std::size_t>::max())
        {
            model_local_bind[bone_index] = model_bones[bone_index].global_bind;
            animation_local_bind[bone_index] = animation_global_bind[bone_index];
        }
        else
        {
            model_local_bind[bone_index] =
                model_bones[parent].global_bind.Inverse() *
                model_bones[bone_index].global_bind;
            animation_local_bind[bone_index] =
                animation_global_bind[parent].Inverse() *
                animation_global_bind[bone_index];
        }
    }

    // Ground the retargeted archer using its skinned support surface, rather
    // than the pelvis height. Do not apply this to authored slime hops.
    std::vector<const hs::SkinnedVertex *> support_vertices;
    double bind_floor = std::numeric_limits<double>::max();
    if (ground_archer)
    {
        for (const auto &vertex : output.vertices)
        {
            const auto &material = output.materials[vertex.material_index].name;
            if (material != "Bow_MAT" && material != "Arrow_MAT")
                bind_floor = std::min(bind_floor, static_cast<double>(vertex.position[1]));
        }
        for (const auto &vertex : output.vertices)
        {
            const auto &material = output.materials[vertex.material_index].name;
            if (material != "Bow_MAT" && material != "Arrow_MAT" &&
                (clip == hs::CharacterAnimationClip::Death ||
                 vertex.position[1] <= bind_floor + 0.12))
                support_vertices.push_back(&vertex);
        }
        if (support_vertices.empty() || !std::isfinite(bind_floor))
            throw std::runtime_error("Archer animation has no body support vertices");
    }
    std::vector<FbxAMatrix> inverse_bind;
    for (const auto &bone : model_bones)
        inverse_bind.push_back(bone.global_bind.Inverse());
    std::vector<FbxAMatrix> skin_matrices(model_bones.size());
    std::vector<FbxAMatrix> current_globals(model_bones.size());
    std::vector<FbxAMatrix> target_globals(model_bones.size());
    std::vector<FbxAMatrix> corrected_globals(model_bones.size());
    for (std::uint32_t frame = 0; frame < frame_count; ++frame)
    {
        FbxTime sample_time;
        sample_time.SetSecondDouble(span.GetStart().GetSecondDouble() +
                                    duration * static_cast<double>(frame) /
                                        static_cast<double>(frame_count - 1));
        for (std::size_t bone_index = 0; bone_index < model_bones.size(); ++bone_index)
        {
            current_globals[bone_index] = evaluator->GetNodeGlobalTransform(
                animation_bones[bone_index], sample_time);
            const auto parent = model_bones[bone_index].parent;
            const auto current_local =
                parent == std::numeric_limits<std::size_t>::max()
                    ? current_globals[bone_index]
                    : current_globals[parent].Inverse() * current_globals[bone_index];
            const auto target_local = model_local_bind[bone_index] *
                                      animation_local_bind[bone_index].Inverse() *
                                      current_local;
            target_globals[bone_index] =
                parent == std::numeric_limits<std::size_t>::max()
                    ? target_local
                    : target_globals[parent] * target_local;
        }
        const auto root_delta = target_globals.front().GetT() -
                                model_bones.front().global_bind.GetT();
        // SetT on a scaled/rotated global decomposes and loses inherited shear.
        // Apply root-motion removal as a translation matrix instead, preserving
        // the full global transform before reconstructing each local TRS.
        FbxAMatrix root_correction;
        root_correction.SetIdentity();
        root_correction.SetT(FbxVector4(-root_delta[0], 0.0, -root_delta[2]));
        for (std::size_t bone_index = 0; bone_index < model_bones.size(); ++bone_index)
            corrected_globals[bone_index] = root_correction * target_globals[bone_index];
        if (ground_archer)
        {
            for (std::size_t bone_index = 0; bone_index < model_bones.size(); ++bone_index)
                skin_matrices[bone_index] = corrected_globals[bone_index] * inverse_bind[bone_index];
            double support_y = std::numeric_limits<double>::max();
            for (const auto *vertex : support_vertices)
            {
                const FbxVector4 rest(vertex->position[0], vertex->position[1],
                                      vertex->position[2]);
                double skinned_y = 0.0;
                for (std::size_t influence = 0; influence < vertex->bone_indices.size(); ++influence)
                    skinned_y += skin_matrices[vertex->bone_indices[influence]].MultT(rest)[1] *
                                 vertex->bone_weights[influence];
                support_y = std::min(support_y, skinned_y);
            }
            if (!std::isfinite(support_y))
                throw std::runtime_error("Archer skinned support is non-finite");
            const auto correction = bind_floor - support_y;
            FbxAMatrix ground_correction;
            ground_correction.SetIdentity();
            ground_correction.SetT(FbxVector4(0.0, correction, 0.0));
            for (auto &global : corrected_globals)
                global = ground_correction * global;
        }
        for (std::size_t bone_index = 0; bone_index < model_bones.size(); ++bone_index)
        {
            const auto parent = model_bones[bone_index].parent;
            const auto local = parent == std::numeric_limits<std::size_t>::max()
                                   ? corrected_globals[bone_index]
                                   : corrected_globals[parent].Inverse() *
                                         corrected_globals[bone_index];
            FbxVector4 translation;
            FbxQuaternion rotation;
            FbxVector4 shearing;
            FbxVector4 scale;
            double determinant_sign{};
            FbxMatrix(local).GetElements(translation, rotation, shearing, scale,
                                         determinant_sign);
            scale *= determinant_sign;
            FbxAMatrix reconstructed;
            reconstructed.SetT(translation);
            reconstructed.SetQ(rotation);
            reconstructed.SetS(scale);
            double maximum_residual = 0.0;
            for (int row = 0; row < 4; ++row)
            {
                for (int column = 0; column < 4; ++column)
                {
                    maximum_residual = std::max(maximum_residual,
                        std::abs(reconstructed.Get(row, column) - local.Get(row, column)));
                }
            }
            if (maximum_residual > 1e-5)
            {
throw std::runtime_error(std::format(
                    "Animation local transform cannot preserve handedness: clip={} frame={} bone={} maximum residual={:.9g} shear=({:.9g},{:.9g},{:.9g}) scale=({:.9g},{:.9g},{:.9g}) determinant_sign={:.0f} animation={}",
                    static_cast<int>(clip), frame, BoneName(*animation_bones[bone_index]),
                    maximum_residual, shearing[0], shearing[1], shearing[2], scale[0],
                    scale[1], scale[2], determinant_sign, animation_path.string()));
            }
            hs::CharacterLocalTransform transform;
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                const auto component = static_cast<int>(axis);
                transform.translation[axis] = static_cast<float>(translation[component]);
                transform.rotation[axis] = static_cast<float>(rotation[component]);
                transform.scale[axis] = static_cast<float>(scale[component]);
            }
            transform.rotation[3] = static_cast<float>(rotation[3]);
            const auto rotation_length = std::sqrt(
                std::inner_product(transform.rotation.begin(), transform.rotation.end(),
                                   transform.rotation.begin(), 0.0f));
            if (!std::isfinite(rotation_length) || rotation_length < 0.999f ||
                rotation_length > 1.001f ||
                !std::ranges::all_of(transform.translation, [](float value) {
                    return std::isfinite(value);
                }) ||
                !std::ranges::all_of(transform.scale, [](float value) {
                    return std::isfinite(value) && std::abs(value) > 0.0001f;
                }))
            {
                throw std::runtime_error(
                    "Animation local transform is invalid");
            }
            output.transforms.push_back(transform);
        }
    }
}

namespace
{
struct CharacterBc7Encoder
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    std::string identity;
    CharacterBc7Encoder()
    {
        constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        const auto native = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            levels, 1, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, nullptr);
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC description{};
        LARGE_INTEGER driver{};
        if (FAILED(native) || FAILED(device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) ||
            FAILED(adapter->GetDesc(&description)) || FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver)))
            device.Reset();
        identity = "bc7-semantic-v4-color-linear-normal-base-unit-postmip-v1-normal-local-cpu3subsets-mode6-angular-alpha-nonreg-v1-normal-renorm-roughness-linear-alpha020-098-boundary-u8guard-local-mode5-floatclass-zero173opaque-anchor-v1-";
        if (device)
            identity += "gpu-d3d11-3subsets-alpha1-v" + std::to_string(description.VendorId) +
                "-d" + std::to_string(description.DeviceId) + "-r" + std::to_string(description.Revision) +
                "-driver" + std::to_string(driver.QuadPart);
        else identity += "cpu-parallel-3subsets-threshold05";
    }
};
CharacterBc7Encoder &CharacterEncoder()
{
    static CharacterBc7Encoder encoder;
    return encoder;
}
struct CharacterTextureSources
{
    std::map<std::filesystem::path, std::vector<std::uint8_t>> decoded;
    std::map<std::filesystem::path, std::size_t> remaining;
    explicit CharacterTextureSources(std::span<const std::filesystem::path> paths)
    {
        for (const auto &path : paths) if (!path.empty()) ++remaining[path];
    }
    void Consumed(const std::filesystem::path &path)
    {
        if (--remaining.at(path) == 0) decoded.erase(path);
    }
};

bool RepairCharacterNormalBlocks(const CharacterMip &reference,const DirectX::Image &wire,
    const DirectX::Image &encoded,const DirectX::Image &decoded)
{
    constexpr std::array flags{DirectX::TEX_COMPRESS_BC7_USE_3SUBSETS,DirectX::TEX_COMPRESS_BC7_QUICK};
    std::array<std::size_t,2> selected{};
    for(std::uint32_t by=0;by<reference.height;by+=4) for(std::uint32_t bx=0;bx<reference.width;bx+=4)
    {
        CharacterNormalBlock source{},original{};std::array<std::uint8_t,64> input_bytes{};
        bool needs_repair{};
        for(std::uint32_t y=0;y<4;++y) for(std::uint32_t x=0;x<4;++x)
        {
            const auto sx=std::min(bx+x,reference.width-1),sy=std::min(by+y,reference.height-1);
            const auto index=y*4+x;
            source[index]=reference.pixels[std::size_t(sy)*reference.width+sx];
            const auto *pixel=reinterpret_cast<const float *>(decoded.pixels+sy*decoded.rowPitch)+sx*4;
            std::copy_n(pixel,4,original[index].begin());
            std::memcpy(input_bytes.data()+index*4,wire.pixels+sy*wire.rowPitch+sx*4,4);
            needs_repair|=!CharacterNormalDirectionsCompatible(source[index],original[index]);
        }
        if(!needs_repair) continue;
        DirectX::Image input{};input.width=input.height=4;input.format=DXGI_FORMAT_R8G8B8A8_UNORM;
        input.rowPitch=16;input.slicePitch=input_bytes.size();input.pixels=input_bytes.data();
        CharacterNormalBlockScore best;best.valid=false;std::array<std::uint8_t,16> best_bytes{};
        std::size_t backend=flags.size();
        for(std::size_t candidate=0;candidate<flags.size();++candidate)
        {
            DirectX::ScratchImage compressed_candidate,decoded_candidate;
            auto result=DirectX::Compress(input,DXGI_FORMAT_BC7_UNORM,flags[candidate],0.5f,compressed_candidate);
            const auto *bytes=SUCCEEDED(result)?compressed_candidate.GetImage(0,0,0):nullptr;
            if(bytes) result=DirectX::Decompress(*bytes,DXGI_FORMAT_R32G32B32A32_FLOAT,decoded_candidate);
            const auto *image=SUCCEEDED(result)?decoded_candidate.GetImage(0,0,0):nullptr;
            if(!bytes||bytes->format!=DXGI_FORMAT_BC7_UNORM||bytes->slicePitch!=16||!image||image->width!=4||image->height!=4||
                image->format!=DXGI_FORMAT_R32G32B32A32_FLOAT||image->rowPitch<64)
            {
                continue;
            }
            CharacterNormalBlock actual{};
            for(std::size_t y=0;y<4;++y) for(std::size_t x=0;x<4;++x)
                std::copy_n(reinterpret_cast<const float *>(image->pixels+y*image->rowPitch)+x*4,4,actual[y*4+x].begin());
            auto score=ScoreCharacterNormalBlock(source,actual);
            const bool alpha_ok=CharacterNormalAlphaErrorNotWorse(source,original,actual);
            if(!score.valid||!alpha_ok) continue;
            if(backend==flags.size()||PreferCharacterNormalBlock(score,best))
            { best=score;backend=candidate;std::memcpy(best_bytes.data(),bytes->pixels,best_bytes.size()); }
        }
        if(backend==flags.size()) continue;
        std::memcpy(encoded.pixels+std::size_t(by/4)*encoded.rowPitch+(bx/4)*16,best_bytes.data(),best_bytes.size());
        ++selected[backend];
    }
    return selected[0]+selected[1]!=0;
}

bool WriteDds(IWICImagingFactory &factory, CharacterTextureSources &sources, const std::filesystem::path &path,
    const std::filesystem::path &source, std::array<std::uint8_t, 4> fallback,
    std::string &error_message, std::uint32_t final_size, CharacterTextureSemantic semantic,
    CharacterAlphaPolicy alpha = {})
{
    try
    {
        CharacterMip base{final_size, final_size, {}};
        if (source.empty())
        {
            base = CharacterConstantSlice(final_size, final_size, fallback);
        }
        else
        {
            auto [cached, first_decode] = sources.decoded.try_emplace(source);
            auto &pixels = cached->second;
            if (first_decode)
            {
                Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
                Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
                Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
                auto result = factory.CreateDecoderFromFilename(source.c_str(), nullptr, GENERIC_READ,
                    WICDecodeMetadataCacheOnDemand, &decoder);
                if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);
                if (SUCCEEDED(result)) result = factory.CreateFormatConverter(&converter);
                if (SUCCEEDED(result)) result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                    WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
                UINT width{}, height{};
                if (SUCCEEDED(result)) result = converter->GetSize(&width, &height);
                if (FAILED(result) || width != final_size || height != final_size)
                    throw std::runtime_error("character texture extent differs from its final array slice: " + source.string());
                pixels.resize(std::size_t(width) * height * 4);
                result = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data());
                if (FAILED(result)) throw std::runtime_error("cannot copy character texture pixels: " + source.string());
            }
            if (pixels.size() != std::size_t(final_size) * final_size * 4)
                throw std::runtime_error("shared character texture source has an incompatible extent");
            base.pixels.resize(std::size_t(final_size) * final_size);
            for (std::size_t index = 0; index < base.pixels.size(); ++index)
                for (std::size_t channel = 0; channel < 4; ++channel)
                    base.pixels[index][channel] = pixels[index * 4 + channel] / 255.0f;
            sources.Consumed(source); // Release decode-only bytes after their last actual semantic consumer.
        }
        auto mips = BuildCharacterMipChain(std::move(base), semantic, alpha);
        if (semantic == CharacterTextureSemantic::NormalRoughness)
        {
            CanonicalizeCharacterNormalBase(mips.front());
        }
        const bool color = semantic == CharacterTextureSemantic::Color;
        const auto format = color ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
        DirectX::ScratchImage input, compressed;
        auto checked = [&](HRESULT result, const char *operation) {
            if (FAILED(result)) throw std::runtime_error(std::string(operation) + ": " + path.string());
        };
        // Mip RGB is already sRGB encoded; matching input/output tags prevent a second conversion.
        checked(input.Initialize2D(color ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM,
            final_size, final_size, 1, mips.size()), "allocate character semantic mip chain");
        for (std::size_t mip = 0; mip < mips.size(); ++mip)
        {
            const auto *image = input.GetImage(mip, 0, 0);
            for (std::uint32_t row = 0; row < mips[mip].height; ++row)
                for (std::uint32_t column = 0; column < mips[mip].width; ++column)
                    for (std::size_t channel = 0; channel < 4; ++channel)
                        image->pixels[row * image->rowPitch + column * 4 + channel] = static_cast<std::uint8_t>(
                            std::lround(std::clamp(mips[mip].pixels[std::size_t(row) * mips[mip].width + column][channel], 0.0f, 1.0f) * 255));
        }
        const auto &encoder = CharacterEncoder();
        if (encoder.device)
            checked(DirectX::Compress(encoder.device.Get(), input.GetImages(), input.GetImageCount(), input.GetMetadata(),
                format, DirectX::TEX_COMPRESS_BC7_USE_3SUBSETS, 1.0f, compressed), "GPU compress character BC7");
        else
            checked(DirectX::Compress(input.GetImages(), input.GetImageCount(), input.GetMetadata(), format,
                static_cast<DirectX::TEX_COMPRESS_FLAGS>(DirectX::TEX_COMPRESS_PARALLEL | DirectX::TEX_COMPRESS_BC7_USE_3SUBSETS),
                0.5f, compressed), "CPU compress character BC7");
        // Keep decoded QA for every mip, including the minimum-one-block tail.
        // These observations do not declare visual/target-hardware qualification.
        for (std::size_t mip = 0; mip < mips.size(); ++mip)
        {
            const auto *block = compressed.GetImage(mip, 0, 0);
            const auto row = ((mips[mip].width + 3) / 4) * 16;
            const auto rows = (mips[mip].height + 3) / 4;
            if (!block || block->rowPitch != row || block->slicePitch != std::size_t(row) * rows)
                throw std::runtime_error("character BC7 block layout mismatch");
            DirectX::ScratchImage decoded;
            checked(DirectX::Decompress(*block, DXGI_FORMAT_R32G32B32A32_FLOAT, decoded), "decode character BC7 QA");
            const auto *image = decoded.GetImage(0, 0, 0);
            if (!image || image->width != mips[mip].width || image->height != mips[mip].height ||
                image->format != DXGI_FORMAT_R32G32B32A32_FLOAT || image->rowPitch < image->width * 4 * sizeof(float))
                throw std::runtime_error("character BC7 initial QA extent mismatch");
            if(semantic==CharacterTextureSemantic::NormalRoughness &&
                RepairCharacterNormalBlocks(mips[mip],*input.GetImage(mip,0,0),*block,*image))
            {
                decoded.Release();
                checked(DirectX::Decompress(*block,DXGI_FORMAT_R32G32B32A32_FLOAT,decoded),"decode repaired character normal full mip QA");
                image=decoded.GetImage(0,0,0);
                if(!image||image->width!=mips[mip].width||image->height!=mips[mip].height||
                    image->format!=DXGI_FORMAT_R32G32B32A32_FLOAT||image->rowPitch<image->width*4*sizeof(float))
                    throw std::runtime_error("character BC7 repaired normal QA extent mismatch");
            }
            std::size_t repaired_blocks{};
            if (alpha.cutoff > 0)
            {
                // Preserve the semantic float mip's per-texel decision, including
                // decisions changed by the intermediate R8 wire quantization.
                const auto block_columns = (mips[mip].width + 3) / 4;
                std::vector<std::uint8_t> repair(std::size_t(block_columns) * rows);
                for (std::uint32_t y = 0; y < mips[mip].height; ++y)
                    for (std::uint32_t x = 0; x < mips[mip].width; ++x)
                    {
                        const auto &reference = mips[mip].pixels[std::size_t(y) * mips[mip].width + x];
                        const auto *pixel = reinterpret_cast<const float *>(image->pixels + y * image->rowPitch) + x * 4;
                        const bool wanted = reference[3] >= alpha.cutoff;
                        if ((pixel[3] >= alpha.cutoff) != wanted)
                            repair[std::size_t(y / 4) * block_columns + x / 4] = 1;
                    }
                for (std::uint32_t by = 0; by < rows; ++by)
                    for (std::uint32_t bx = 0; bx < block_columns; ++bx)
                    {
                        if (!repair[std::size_t(by) * block_columns + bx]) continue;
                        CharacterBc7Texels source_block{};
                        for (std::uint32_t y = 0; y < 4; ++y) for (std::uint32_t x = 0; x < 4; ++x)
                        {
                            const auto sx = std::min(bx * 4 + x, mips[mip].width - 1);
                            const auto sy = std::min(by * 4 + y, mips[mip].height - 1);
                            source_block[y * 4 + x] = mips[mip].pixels[std::size_t(sy) * mips[mip].width + sx];
                        }
                        auto *destination = block->pixels + std::size_t(by) * block->rowPitch + bx * 16;
                        CharacterBc7Block original{}, replacement{};
                        std::memcpy(original.data(), destination, original.size());
                        if (!EncodeCharacterBc7CoverageBlock(source_block, alpha, original, replacement))
                            throw std::runtime_error("character BC7 cannot preserve this block's float cutoff and zero/body/opaque anchors: " +
                                path.string() + " mip=" + std::to_string(mip) + " block=" + std::to_string(bx) + "," + std::to_string(by));
                        std::memcpy(destination, replacement.data(), replacement.size());
                        ++repaired_blocks;
                    }
                if (repaired_blocks)
                {
                    // The library decoder, not the helper's packing model, is
                    // the final truth for every repaired block and partial tail.
                    decoded.Release();
                    checked(DirectX::Decompress(*block, DXGI_FORMAT_R32G32B32A32_FLOAT, decoded), "decode repaired character BC7 QA");
                    image = decoded.GetImage(0, 0, 0);
                    if (!image || image->width != mips[mip].width || image->height != mips[mip].height ||
                        image->format != DXGI_FORMAT_R32G32B32A32_FLOAT || image->rowPitch < image->width * 4 * sizeof(float))
                        throw std::runtime_error("character BC7 repaired QA extent mismatch");
                    for (std::uint32_t y = 0; y < mips[mip].height; ++y)
                        for (std::uint32_t x = 0; x < mips[mip].width; ++x)
                        {
                            if (!repair[std::size_t(y / 4) * block_columns + x / 4]) continue;
                            const auto &reference = mips[mip].pixels[std::size_t(y) * mips[mip].width + x];
                            const auto *pixel = reinterpret_cast<const float *>(image->pixels + y * image->rowPitch) + x * 4;
                            const bool anchor = reference[3] == 0 || reference[3] == 1 || (alpha.preserve_translucent &&
                                std::abs(reference[3] - 173.0f / 255.0f) < 1.e-7f);
                            if ((reference[3] >= alpha.cutoff) != (pixel[3] >= alpha.cutoff) ||
                                (anchor && std::abs(reference[3] - pixel[3]) > 1.e-7f))
                                throw std::runtime_error("character BC7 repaired block failed actual codec class/anchor verification: " +
                                    path.string() + " mip=" + std::to_string(mip));
                        }
                }
            }
            if (!image || image->width != mips[mip].width || image->height != mips[mip].height ||
                image->format != DXGI_FORMAT_R32G32B32A32_FLOAT)
                throw std::runtime_error("character BC7 QA extent mismatch");
            std::size_t reference_covered{}, decoded_covered{};
            std::size_t coverage_mismatches{};
            for (std::uint32_t y = 0; y < image->height; ++y)
                for (std::uint32_t x = 0; x < image->width; ++x)
                {
                    const auto &reference = mips[mip].pixels[std::size_t(y) * image->width + x];
                    const auto *pixel = reinterpret_cast<const float *>(image->pixels + y * image->rowPitch) + x * 4;
                    for (std::size_t channel = 0; channel < 4; ++channel)
                    {
                        const auto actual = pixel[channel];
                        const auto difference = actual - (color && channel < 3 ? CharacterSrgbToLinear(reference[channel]) : reference[channel]);
                        if (!std::isfinite(difference) || actual < 0 || actual > 1)
                            throw std::runtime_error("character BC7 decode contains an invalid channel");
                    }
                    if (semantic == CharacterTextureSemantic::NormalRoughness)
                    {
                        double dot{}, reference_length{}, actual_length{};
                        for (std::size_t channel = 0; channel < 3; ++channel)
                        {
                            const auto a = reference[channel] * 2.0 - 1.0, b = pixel[channel] * 2.0 - 1.0;
                            dot += a * b; reference_length += a * a; actual_length += b * b;
                        }
                        if (!(reference_length > 0 && actual_length > 0 && dot > 0))
                        {
                            const auto *wire = input.GetImage(mip, 0, 0);
                            const auto *rgba = wire->pixels + y * wire->rowPitch + x * 4;
                            std::ostringstream details;
                            details << std::setprecision(10)
                                << "character BC7 normal is non-normalizable or crossed its source hemisphere: " << path.string()
                                << " source=" << (source.empty() ? std::string("fallback") : source.string())
                                << " semantic=normal-roughness mip=" << mip << " xy=" << x << ',' << y
                                << " texel_index=" << (std::size_t(y) * image->width + x)
                                << " reference_rgba=" << reference[0] << ',' << reference[1] << ',' << reference[2] << ',' << reference[3]
                                << " wire_rgba=" << unsigned(rgba[0]) << ',' << unsigned(rgba[1]) << ',' << unsigned(rgba[2]) << ',' << unsigned(rgba[3])
                                << " decoded_rgba=" << pixel[0] << ',' << pixel[1] << ',' << pixel[2] << ',' << pixel[3]
                                << " reference_normal=" << (reference[0] * 2.0 - 1.0) << ',' << (reference[1] * 2.0 - 1.0) << ',' << (reference[2] * 2.0 - 1.0)
                                << " decoded_normal=" << (pixel[0] * 2.0 - 1.0) << ',' << (pixel[1] * 2.0 - 1.0) << ',' << (pixel[2] * 2.0 - 1.0)
                                << " reference_length_squared=" << reference_length << " decoded_length_squared=" << actual_length
                                << " dot=" << dot << " block_xy=" << x / 4 << ',' << y / 4;
                            throw std::runtime_error(details.str());
                        }
                    }
                    if (alpha.cutoff > 0)
                    {
                        const bool before = reference[3] >= alpha.cutoff, after = pixel[3] >= alpha.cutoff;
                        reference_covered += before; decoded_covered += after;
                        if (before != after)
                        {
                            ++coverage_mismatches;
                        }
                    }
                }
            if (alpha.cutoff > 0 && coverage_mismatches)
                throw std::runtime_error("character BC7 changed the spatial cutoff mask after local repair: " + path.string() +
                    " mip=" + std::to_string(mip));
            // One texel is the smallest nonzero representable coverage quantum.
            // A failing recipe is rejected; this is not an optical-quality pass.
            if (alpha.cutoff > 0 && std::max(reference_covered, decoded_covered) -
                std::min(reference_covered, decoded_covered) > 1)
                throw std::runtime_error("character BC7 changed cutoff coverage by more than one texel: " + path.string() +
                    " mip=" + std::to_string(mip));
        }
        DirectX::Blob dds;
        checked(DirectX::SaveToDDSMemory(compressed.GetImages(), compressed.GetImageCount(), compressed.GetMetadata(),
            DirectX::DDS_FLAGS_FORCE_DX10_EXT, dds), "serialize character BC7 DDS");
        const auto bytes = std::as_bytes(std::span(dds.GetBufferPointer(), dds.GetBufferSize()));
        const auto decoded = render_data::DecodeDdsTexture(bytes);
        render_data::ValidateDdsTextureRole(decoded, render_data::DdsTextureRole::CharacterFinalBc7,
            {final_size, final_size, 1, static_cast<std::uint32_t>(mips.size()), static_cast<std::uint32_t>(format)});
        return AtomicWriteCharacterFile(path, bytes, error_message);
    }
    catch (const std::exception &exception)
    {
        error_message = exception.what();
        return false;
    }
}
} // namespace

const std::string &CharacterTextureCookRecipeIdentity() { return CharacterEncoder().identity; }

bool ImportCharacterAsset(CharacterIR &character, std::string &error_message,
                          const CharacterAssetSource &source)
{
    auto *manager = FbxManager::Create();
    if (!manager)
    {
        error_message = "FBX manager creation failed";
        return false;
    }
    manager->SetIOSettings(FbxIOSettings::Create(manager, IOSROOT));
    try
    {
        auto *model = LoadFbx(*manager, source.model, source.diagnostic_name);
        std::vector<BoneSource> bones;
        const bool ground_archer = source.output_name == "archer";
        GatherModel(*model, character, bones, source.model,
                    source.allow_missing_material_textures, ground_archer);
        character.material_count = static_cast<std::uint32_t>(character.materials.size());
        character.ground_offset = -character.bounds_min[1];
        for (const auto &bone : bones) character.bone_names.emplace_back(bone.node->GetName());
        if (source.seal_eye_socket)
            SealSlimeEyeSocket(character);
        auto &named = character.animation_extension;
        named = {};
        if (!source.named_animations.empty())
        {
            named.character_asset_id = hs::MakeAssetId(
                "render/character/" + source.output_name + ".rpkg");
            named.sample_rate_hz = 60;
            named.bone_names = character.bone_names;
            named.parents = character.parents;
            named.inverse_bind_matrices = character.inverse_bind_matrices;
        }
        character.upper_body_weights.resize(bones.size());
        for (std::size_t bone_index = 0; bone_index < bones.size(); ++bone_index)
        {
            auto ancestor = bone_index;
            while (ancestor != std::numeric_limits<std::size_t>::max())
            {
                auto name = bones[ancestor].name;
                std::ranges::transform(name, name.begin(), [](unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                });
                if (name.contains("spine") || name.contains("chest") ||
                    name.contains("neck") || name.contains("head") ||
                    name.contains("clavicle") || name.contains("shoulder") ||
                    name.contains("arm") || name.contains("hand"))
                {
                    character.upper_body_weights[bone_index] = 1.0f;
                    break;
                }
                ancestor = bones[ancestor].parent;
            }
        }
        model->Destroy();

        for (const auto &[animation_path, clip, looping] :
             std::array{
                 std::tuple{source.animations[0], hs::CharacterAnimationClip::Idle, true},
                 std::tuple{source.animations[1], hs::CharacterAnimationClip::Run, true},
                 std::tuple{source.animations[2], hs::CharacterAnimationClip::Draw, false},
                 std::tuple{source.animations[3], hs::CharacterAnimationClip::Recoil, false},
                 std::tuple{source.animations[4], hs::CharacterAnimationClip::Death, false},
             })
        {
            auto *animation = LoadFbx(*manager, animation_path, source.diagnostic_name);
            GatherAnimation(*animation, bones, clip, looping, character, animation_path, ground_archer);
            animation->Destroy();
        }
        // Reuse the five imported samples exactly. Extra authored files pass
        // through the same hierarchy, coordinate, retargeting and TRS checks,
        // but never become a sixth legacy slot or survive in its transform data.
        std::unordered_set<std::string> named_names;
        std::unordered_set<std::string> named_files;
        for (const auto &authored : source.named_animations)
        {
            if (authored.authored_name != authored.file.stem().string() ||
                !named_names.insert(authored.authored_name).second ||
                !named_files.insert(authored.file.generic_string()).second)
                throw std::runtime_error("Duplicate or mismatched named animation source");
            const auto legacy = std::ranges::find(source.animations, authored.file);
            hs::CharacterClipHeader header;
            const auto legacy_clip_count = character.clips.size();
            const auto legacy_transform_count = character.transforms.size();
            if (legacy != source.animations.end())
                header = character.clips[static_cast<std::size_t>(legacy - source.animations.begin())];
            else
            {
                auto *animation = LoadFbx(*manager, authored.file, source.diagnostic_name);
                GatherAnimation(*animation, bones, hs::CharacterAnimationClip::Count,
                                authored.looping, character, authored.file, ground_archer);
                animation->Destroy();
                header = character.clips.back();
            }
            if ((header.looping != 0) != authored.looping)
                throw std::runtime_error("Named animation loop flag disagrees with legacy clip");
            const auto count = static_cast<std::size_t>(header.frame_count) * bones.size();
            if (named.transforms.size() > 800'000 || count > 800'000 - named.transforms.size())
                throw std::runtime_error("Named animation transform budget exceeded");
            named.clips.push_back({hs::MakeAssetId("character/" + source.output_name +
                "/animation/" + authored.authored_name), authored.authored_name,
                static_cast<std::uint32_t>(named.transforms.size()), header.frame_count,
                header.duration_seconds, authored.looping});
            named.transforms.insert(named.transforms.end(),
                character.transforms.begin() + header.first_transform,
                character.transforms.begin() + header.first_transform + static_cast<std::ptrdiff_t>(count));
            character.clips.resize(legacy_clip_count);
            character.transforms.resize(legacy_transform_count);
        }
        // This was the legacy reader's source-bounds admission; retain it at
        // the direct import boundary after all authored clips are prepared.
        if (!std::isfinite(character.bounds_min[1]) || !std::isfinite(character.bounds_max[1]) ||
            character.bounds_min[1] >= character.bounds_max[1])
            throw std::runtime_error("Invalid character source bounds");
    }
    catch (const std::exception &exception)
    {
        error_message = exception.what();
        manager->Destroy();
        return false;
    }
    manager->Destroy();
    return true;
}

bool WriteCharacterTextures(const std::filesystem::path &output,
    const CharacterIR &character, const CharacterAssetSource &source,
    std::string &error_message)
{
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
    {
        error_message = "WIC factory creation failed";
        return false;
    }
    std::vector<std::filesystem::path> source_paths;
    for (const auto &material : character.materials)
    {
        source_paths.push_back(material.diffuse); source_paths.push_back(material.normal);
    }
    CharacterTextureSources sources(source_paths);
    for (std::size_t index = 0; index < character.materials.size(); ++index)
    {
        const auto suffix = std::to_string(index) + ".dds";
        const auto &material = character.materials[index];
        const bool archer = source.output_name == "archer";
        const bool family = source.output_name.starts_with("enemy_");
        const auto size = archer ? 2048u : family ? 1024u : 1u;
        const CharacterAlphaPolicy coverage{archer ? 0.2f : family ? 0.98f : 0.0f, family};
        if (!WriteDds(*factory.Get(), sources, output / (source.output_name + "_diffuse_" + suffix),
                      material.diffuse, {255, 255, 255, 255}, error_message,
                      size, CharacterTextureSemantic::Color, coverage) ||
            !WriteDds(*factory.Get(), sources, output / (source.output_name + "_normal_" + suffix),
                      material.normal, {128, 128, 255, 255}, error_message,
                      size, CharacterTextureSemantic::NormalRoughness))
        {
            return false;
        }
    }
    return true;
}

bool CookMonsterMaterialTextures(const std::filesystem::path &output,
                                 std::string &error_message,
                                 const std::filesystem::path &source)
{
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
    {
        error_message = "WIC factory creation failed";
        return false;
    }
    const std::array source_paths{source / "BasecolorDefault_TEX.png", source / "Emissive_TEX.png", source / "RAM_TEX.png"};
    CharacterTextureSources sources(source_paths);
    return WriteDds(*factory.Get(), sources, output / "monster_basecolor.dds",
                    source / "BasecolorDefault_TEX.png", {255, 255, 255, 255},
                    error_message, 512, CharacterTextureSemantic::Color) &&
           WriteDds(*factory.Get(), sources, output / "monster_emissive.dds",
                    source / "Emissive_TEX.png", {0, 0, 0, 255}, error_message,
                    512, CharacterTextureSemantic::Color) &&
           WriteDds(*factory.Get(), sources, output / "monster_ram.dds",
                    source / "RAM_TEX.png", {255, 0, 0, 255}, error_message,
                    512, CharacterTextureSemantic::LinearData);
}

bool CookMonsterMaterialTextures(const std::filesystem::path &output,
                                 std::string &error_message)
{
    return CookMonsterMaterialTextures(output, error_message, paths::MonsterTextures());
}
} // namespace hs::content
