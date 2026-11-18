#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "character_fbx.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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


namespace
{
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


} // namespace
std::vector<FbxNode *> CharacterMeshNodes(FbxScene &scene, bool ground_archer)
{
    FbxGeometryConverter converter(scene.GetFbxManager());
    if (!converter.Triangulate(&scene, true, false))
    {
        throw std::runtime_error("Archer FBX triangulation failed");
    }

    std::vector<FbxNode *> mesh_nodes;
    const auto collect = [&](auto &&self, FbxNode *node) -> void {
        if (node->GetMesh())
        {
            // This FBX has permuted node names: Arrow_Mesh is the clothed body,
            // while Eyes_Mesh carries the independent arrow.
            bool arrow_only = ground_archer && node->GetMaterialCount() > 0;
            for (int index = 0; index < node->GetMaterialCount(); ++index)
            {
                const auto *material = node->GetMaterial(index);
                arrow_only = arrow_only && material &&
                             std::string_view(material->GetName()) == "Arrow_MAT";
            }
            if (!arrow_only) mesh_nodes.push_back(node);
        }
        for (int index = 0; index < node->GetChildCount(); ++index)
            self(self, node->GetChild(index));
    };
    collect(collect, scene.GetRootNode());
    return mesh_nodes;
}

std::vector<std::uint16_t> GatherCharacterNodeMaterials(
    FbxNode &node, CharacterIR &output, const std::filesystem::path &model_path,
    bool allow_missing)
{
    std::vector<std::uint16_t> node_materials;
    node_materials.reserve(static_cast<std::size_t>(node.GetMaterialCount()));
    for (int material_index = 0; material_index < node.GetMaterialCount(); ++material_index)
    {
        auto *material = node.GetMaterial(material_index);
        if (!material)
        {
            throw std::runtime_error("Archer node has an empty material slot");
        }
        node_materials.push_back(GatherMaterial(
            *material, output, model_path, allow_missing));
    }
    if (node_materials.empty())
    {
        if (output.materials.empty())
        {
            output.materials.push_back({{}, {}, "Default"});
        }
        node_materials.push_back(0);
    }
    return node_materials;
}

bool ImportCharacterMaterials(CharacterIR &character, std::string &error_message,
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
        const auto mesh_nodes = CharacterMeshNodes(*model, source.output_name == "archer");
        if (mesh_nodes.empty())
        {
            throw std::runtime_error("Archer FBX has no skinned mesh");
        }
        for (auto *node : mesh_nodes)
            (void)GatherCharacterNodeMaterials(*node, character, source.model,
                                               source.allow_missing_material_textures);
        character.material_count = static_cast<std::uint32_t>(character.materials.size());
        model->Destroy();
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
} // namespace hs::content
