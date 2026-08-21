#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "content_cooker.hpp"

#include <hs/core/dds_format.hpp>

#include <fbxsdk.h>
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hs::content
{

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
                                      const char *property_name)
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
    const auto path = std::filesystem::path(HS_CHARACTER_MODEL).parent_path() /
                      "ErikaArcher.fbm" / filename;
    if (!std::filesystem::is_regular_file(path))
    {
        throw std::runtime_error("Archer material texture is missing: " + path.string());
    }
    return path;
}

std::uint16_t GatherMaterial(FbxSurfaceMaterial &material,
                             CharacterCookResult &output)
{
    auto diffuse = MaterialTexture(material, FbxSurfaceMaterial::sDiffuse);
    auto normal = MaterialTexture(material, FbxSurfaceMaterial::sNormalMap);
    if (normal.empty())
    {
        normal = MaterialTexture(material, FbxSurfaceMaterial::sBump);
    }
    const auto name = std::string(material.GetName());
    const auto existing = std::ranges::find_if(
        output.materials, [&](const CharacterCookResult::Material &candidate) {
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

void GatherModel(FbxScene &scene, CharacterCookResult &output,
                 std::vector<BoneSource> &bones)
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
    for (std::size_t index = 0; index < bones.size(); ++index)
    {
        if (!bone_indices.emplace(bones[index].node,
                                  static_cast<std::uint16_t>(index)).second)
        {
            throw std::runtime_error("Archer skeleton contains a duplicate bone");
        }
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
            node_materials.push_back(GatherMaterial(*material, output));
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
                    if (control_point < 0 || control_point >= mesh->GetControlPointsCount() ||
                        control_weights[weight_index] <= 0.0)
                    {
                        continue;
                    }
                    weights[static_cast<std::size_t>(control_point)].push_back(
                        {bone, static_cast<float>(control_weights[weight_index])});
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
                    influences.push_back({0, 1.0f});
                }
                float total{};
                for (const auto &influence : influences)
                {
                    total += influence.second;
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
            const auto edge1 = FbxVector4(
                triangle[1].position[0] - triangle[0].position[0],
                triangle[1].position[1] - triangle[0].position[1],
                triangle[1].position[2] - triangle[0].position[2]);
            const auto edge2 = FbxVector4(
                triangle[2].position[0] - triangle[0].position[0],
                triangle[2].position[1] - triangle[0].position[1],
                triangle[2].position[2] - triangle[0].position[2]);
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
                     CharacterCookResult &output)
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
    for (const auto &bone : model_bones)
    {
        auto *animation_bone = FindBone(scene, bone.name);
        if (!animation_bone)
        {
            throw std::runtime_error("Animation skeleton is missing bone: " + bone.name);
        }
        animation_bones.push_back(animation_bone);
        animation_global_bind.push_back(
            animation_bone->EvaluateGlobalTransform(
                FBXSDK_TIME_INFINITE, FbxNode::eSourcePivot, false, true));
    }

    const auto frame_count =
        std::max<std::uint32_t>(2, static_cast<std::uint32_t>(std::ceil(duration * 60.0)) + 1);
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
        const auto corrected_root = target_globals.front();
        const auto root_delta =
            corrected_root.GetT() - model_bones.front().global_bind.GetT();
        for (std::size_t bone_index = 0; bone_index < model_bones.size(); ++bone_index)
        {
            corrected_globals[bone_index] = target_globals[bone_index];
            auto translation = corrected_globals[bone_index].GetT();
            translation[0] -= root_delta[0];
            translation[2] -= root_delta[2];
            corrected_globals[bone_index].SetT(translation);
        }
        for (std::size_t bone_index = 0; bone_index < model_bones.size(); ++bone_index)
        {
            const auto parent = model_bones[bone_index].parent;
            const auto local = parent == std::numeric_limits<std::size_t>::max()
                                   ? corrected_globals[bone_index]
                                   : corrected_globals[parent].Inverse() *
                                         corrected_globals[bone_index];
            const auto translation = local.GetT();
            const auto rotation = local.GetQ();
            const auto scale = local.GetS();
            hs::CharacterLocalTransform transform;
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                transform.translation[axis] = static_cast<float>(translation[axis]);
                transform.rotation[axis] = static_cast<float>(rotation[axis]);
                transform.scale[axis] = static_cast<float>(scale[axis]);
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

bool WriteCharacterAsset(const std::filesystem::path &path,
                         const CharacterCookResult &asset,
                         std::string &error_message)
{
    hs::CharacterAssetHeader header;
    header.vertex_count = static_cast<std::uint32_t>(asset.vertices.size());
    header.bone_count = asset.bone_count;
    header.clip_count = static_cast<std::uint32_t>(asset.clips.size());
    header.material_count = static_cast<std::uint32_t>(asset.materials.size());
    header.clips_offset = header.vertices_offset +
                          header.vertex_count * sizeof(hs::SkinnedVertex);
    header.parents_offset = header.clips_offset +
                            header.clip_count * sizeof(hs::CharacterClipHeader);
    header.inverse_bind_matrices_offset =
        header.parents_offset + header.bone_count * sizeof(std::uint16_t);
    header.upper_body_weights_offset =
        header.inverse_bind_matrices_offset +
        header.bone_count * sizeof(asset.inverse_bind_matrices.front());
    header.transforms_offset =
        header.upper_body_weights_offset + header.bone_count * sizeof(float);
    header.payload_size = header.transforms_offset - sizeof(header) +
                          static_cast<std::uint32_t>(
                              asset.transforms.size() * sizeof(asset.transforms.front()));
    header.bounds_min = asset.bounds_min;
    header.bounds_max = asset.bounds_max;

    std::vector<std::byte> file(sizeof(header) + header.payload_size);
    auto *cursor = file.data() + sizeof(header);
    const auto vertices_size = asset.vertices.size() * sizeof(asset.vertices.front());
    std::memcpy(cursor, asset.vertices.data(), vertices_size);
    cursor += vertices_size;
    const auto clips_size = asset.clips.size() * sizeof(asset.clips.front());
    std::memcpy(cursor, asset.clips.data(), clips_size);
    cursor += clips_size;
    const auto parents_size = asset.parents.size() * sizeof(asset.parents.front());
    std::memcpy(cursor, asset.parents.data(), parents_size);
    cursor += parents_size;
    const auto inverse_bind_size =
        asset.inverse_bind_matrices.size() * sizeof(asset.inverse_bind_matrices.front());
    std::memcpy(cursor, asset.inverse_bind_matrices.data(), inverse_bind_size);
    cursor += inverse_bind_size;
    const auto weights_size = asset.upper_body_weights.size() * sizeof(float);
    std::memcpy(cursor, asset.upper_body_weights.data(), weights_size);
    cursor += weights_size;
    const auto transforms_size = asset.transforms.size() * sizeof(asset.transforms.front());
    std::memcpy(cursor, asset.transforms.data(), transforms_size);
    header.payload_crc32 = hs::Crc32(
        std::span(file.data() + sizeof(header), header.payload_size));
    std::memcpy(file.data(), &header, sizeof(header));
    return AtomicWrite(path, file, error_message);
}

bool WriteDds(IWICImagingFactory &factory, const std::filesystem::path &path,
              const std::filesystem::path &source, std::array<std::uint8_t, 4> fallback,
              std::string &error_message)
{
    constexpr auto magic = hs::kDdsMagic;
    DdsHeader header;
    std::vector<std::byte> pixels;
    if (source.empty())
    {
        pixels.resize(fallback.size());
        std::memcpy(pixels.data(), fallback.data(), fallback.size());
    }
    else
    {
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        auto result = factory.CreateDecoderFromFilename(
            source.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
            &decoder);
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        if (SUCCEEDED(result))
        {
            result = decoder->GetFrame(0, &frame);
        }
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(result))
        {
            result = factory.CreateFormatConverter(&converter);
        }
        if (SUCCEEDED(result))
        {
            result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                           WICBitmapDitherTypeNone, nullptr, 0,
                                           WICBitmapPaletteTypeCustom);
        }
        UINT width{};
        UINT height{};
        if (SUCCEEDED(result))
        {
            result = converter->GetSize(&width, &height);
        }
        const auto byte_count = static_cast<std::uint64_t>(width) * height * 4;
        if (FAILED(result) || width == 0 || height == 0 ||
            byte_count > std::numeric_limits<UINT>::max())
        {
            error_message = "cannot decode texture: " + source.string();
            return false;
        }
        pixels.resize(static_cast<std::size_t>(byte_count));
        result = converter->CopyPixels(nullptr, width * 4,
                                       static_cast<UINT>(pixels.size()),
                                       reinterpret_cast<BYTE *>(pixels.data()));
        if (FAILED(result))
        {
            error_message = "cannot copy texture pixels: " + source.string();
            return false;
        }
        header.width = width;
        header.height = height;
        header.pitch = width * 4;
    }
    std::vector<std::byte> file(sizeof(magic) + sizeof(header) + pixels.size());
    auto *cursor = file.data();
    std::memcpy(cursor, &magic, sizeof(magic));
    cursor += sizeof(magic);
    std::memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);
    std::memcpy(cursor, pixels.data(), pixels.size());
    return AtomicWrite(path, file, error_message);
}

bool CookCharacterAsset(const std::filesystem::path &output,
                        CharacterCookResult &character,
                        std::string &error_message)
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
        auto *model = LoadFbx(*manager, HS_CHARACTER_MODEL, "ArcherModel");
        std::vector<BoneSource> bones;
        GatherModel(*model, character, bones);
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

        const auto animation_root = std::filesystem::path(HS_CHARACTER_ANIMATION_DIRECTORY);
        for (const auto &[filename, clip, looping] :
             std::array{
                 std::tuple{"Idle.fbx", hs::CharacterAnimationClip::Idle, true},
                 std::tuple{"RunForward.fbx", hs::CharacterAnimationClip::Run, true},
                 std::tuple{"DrawArrow.fbx", hs::CharacterAnimationClip::Draw, false},
                 std::tuple{"AimRecoil.fbx", hs::CharacterAnimationClip::Recoil, false},
                 std::tuple{"DeathBackward.fbx", hs::CharacterAnimationClip::Death, false},
             })
        {
            auto *animation = LoadFbx(*manager, animation_root / filename, filename);
            GatherAnimation(*animation, bones, clip, looping, character);
            animation->Destroy();
        }
        if (!WriteCharacterAsset(output / "archer.meshbin", character,
                                 error_message))
        {
            manager->Destroy();
            return false;
        }
    }
    catch (const std::exception &exception)
    {
        error_message = exception.what();
        manager->Destroy();
        return false;
    }
    manager->Destroy();

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
    {
        error_message = "WIC factory creation failed";
        return false;
    }
    for (std::size_t index = 0; index < character.materials.size(); ++index)
    {
        const auto suffix = std::to_string(index) + ".dds";
        const auto &material = character.materials[index];
        if (!WriteDds(*factory.Get(), output / ("archer_diffuse_" + suffix),
                      material.diffuse, {255, 255, 255, 255}, error_message) ||
            !WriteDds(*factory.Get(), output / ("archer_normal_" + suffix),
                      material.normal, {128, 128, 255, 255}, error_message))
        {
            return false;
        }
        std::cout << "content.material index=" << index << " name=" << material.name
                  << " diffuse="
                  << (material.diffuse.empty() ? "fallback" : material.diffuse.filename().string())
                  << " normal="
                  << (material.normal.empty() ? "fallback" : material.normal.filename().string())
                  << '\n';
    }
    return true;
}

} // namespace hs::content
