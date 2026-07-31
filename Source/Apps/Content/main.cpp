#include <fbxsdk.h>

#include <Windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace
{

struct MeshHeader
{
    std::array<char, 8> magic{'H', 'S', 'M', 'E', 'S', 'H', '1', '\0'};
    std::uint32_t mesh_count{};
    std::uint32_t bone_count{};
};

struct DdsPixelFormat
{
    std::uint32_t size{32};
    std::uint32_t flags{0x41};
    std::uint32_t four_cc{};
    std::uint32_t rgb_bit_count{32};
    std::uint32_t red_mask{0x000000ff};
    std::uint32_t green_mask{0x0000ff00};
    std::uint32_t blue_mask{0x00ff0000};
    std::uint32_t alpha_mask{0xff000000};
};

struct DdsHeader
{
    std::uint32_t size{124};
    std::uint32_t flags{0x100F};
    std::uint32_t height{1};
    std::uint32_t width{1};
    std::uint32_t pitch{4};
    std::uint32_t depth{};
    std::uint32_t mip_count{1};
    std::array<std::uint32_t, 11> reserved{};
    DdsPixelFormat pixel_format;
    std::uint32_t caps{0x1000};
    std::array<std::uint32_t, 4> remaining_caps{};
};

void CountNodes(FbxNode *node, std::uint32_t &meshes, std::uint32_t &bones)
{
    if (const auto *attribute = node->GetNodeAttribute())
    {
        meshes += attribute->GetAttributeType() == FbxNodeAttribute::eMesh;
        const auto type = attribute->GetAttributeType();
        bones += type == FbxNodeAttribute::eSkeleton;
    }
    for (int index = 0; index < node->GetChildCount(); ++index)
    {
        CountNodes(node->GetChild(index), meshes, bones);
    }
}

bool CreateAndImportFbx(const std::filesystem::path &path, MeshHeader &header)
{
    auto *manager = FbxManager::Create();
    if (!manager)
    {
        return false;
    }
    manager->SetIOSettings(FbxIOSettings::Create(manager, IOSROOT));

    auto *scene = FbxScene::Create(manager, "Stage1");
    auto *skeleton = FbxSkeleton::Create(scene, "RootBone");
    skeleton->SetSkeletonType(FbxSkeleton::eRoot);
    auto *bone = FbxNode::Create(scene, "RootBone");
    bone->SetNodeAttribute(skeleton);
    scene->GetRootNode()->AddChild(bone);

    auto *mesh = FbxMesh::Create(scene, "ArcherMesh");
    mesh->InitControlPoints(3);
    auto *points = mesh->GetControlPoints();
    points[0] = FbxVector4(-0.5, 0.0, 0.0);
    points[1] = FbxVector4(0.5, 0.0, 0.0);
    points[2] = FbxVector4(0.0, 1.8, 0.0);
    mesh->BeginPolygon();
    mesh->AddPolygon(0);
    mesh->AddPolygon(1);
    mesh->AddPolygon(2);
    mesh->EndPolygon();

    auto *skin = FbxSkin::Create(scene, "Skin");
    auto *cluster = FbxCluster::Create(scene, "RootWeights");
    cluster->SetLink(bone);
    cluster->SetLinkMode(FbxCluster::eNormalize);
    for (int index = 0; index < 3; ++index)
    {
        cluster->AddControlPointIndex(index, 1.0);
    }
    cluster->SetTransformMatrix(FbxAMatrix());
    cluster->SetTransformLinkMatrix(FbxAMatrix());
    skin->AddCluster(cluster);
    mesh->AddDeformer(skin);

    auto *mesh_node = FbxNode::Create(scene, "Archer");
    mesh_node->SetNodeAttribute(mesh);
    scene->GetRootNode()->AddChild(mesh_node);

    auto *exporter = FbxExporter::Create(manager, "");
    const auto exported = exporter->Initialize(path.string().c_str(), -1, manager->GetIOSettings()) &&
                          exporter->Export(scene);
    exporter->Destroy();
    scene->Destroy();
    if (!exported)
    {
        manager->Destroy();
        return false;
    }

    auto *import_scene = FbxScene::Create(manager, "Imported");
    auto *importer = FbxImporter::Create(manager, "");
    const auto imported =
        importer->Initialize(path.string().c_str(), -1, manager->GetIOSettings()) &&
        importer->Import(import_scene);
    importer->Destroy();
    if (imported)
    {
        CountNodes(import_scene->GetRootNode(), header.mesh_count, header.bone_count);
    }
    import_scene->Destroy();
    manager->Destroy();
    return imported && header.mesh_count == 1 && header.bone_count == 1;
}

bool WriteDds(const std::filesystem::path &path)
{
    constexpr std::uint32_t magic = 0x20534444;
    constexpr std::uint32_t pixel = 0xffffffff;
    const DdsHeader header;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char *>(&magic), sizeof(magic));
    stream.write(reinterpret_cast<const char *>(&header), sizeof(header));
    stream.write(reinterpret_cast<const char *>(&pixel), sizeof(pixel));
    return stream.good();
}

} // namespace

int main(int argc, char **argv)
{
    const auto validate_only = argc == 2 && std::string_view(argv[1]) == "--validate-only";
    if (!validate_only)
    {
        std::cerr << "usage: hs_content --validate-only\n";
        return 2;
    }

    const std::filesystem::path output = HS_COOKED_DIRECTORY;
    std::error_code error;
    std::filesystem::create_directories(output, error);
    if (error)
    {
        std::cerr << error.message() << '\n';
        return 1;
    }

    MeshHeader header;
    if (!CreateAndImportFbx(output / "stage1_archer.fbx", header))
    {
        std::cerr << "FBX skeletal round-trip failed\n";
        return 1;
    }
    std::ofstream mesh(output / "stage1_archer.meshbin", std::ios::binary | std::ios::trunc);
    mesh.write(reinterpret_cast<const char *>(&header), sizeof(header));
    if (!mesh.good() || !WriteDds(output / "stage1_white.dds"))
    {
        std::cerr << "Cook output failed\n";
        return 1;
    }

    std::ofstream manifest(output / "manifest.txt", std::ios::trunc);
    constexpr std::string_view shaders[] = {
        "scene_vs.dxil",     "scene_ps.dxil",    "shadow_vs.dxil",
        "particle_vs.dxil",  "particle_ps.dxil", "particle_cs.dxil",
        "fullscreen_vs.dxil", "deferred_ps.dxil", "composite_ps.dxil",
        "bloom_ps.dxil",     "tonemap_ps.dxil",  "outline_ps.dxil",
        "fxaa_ps.dxil",      "ui_ps.dxil",
    };
    for (const auto shader : shaders)
    {
        const auto source = std::filesystem::path(HS_SHADER_DIRECTORY) / shader;
        const auto destination = output / shader;
        std::filesystem::copy_file(source, destination,
                                   std::filesystem::copy_options::overwrite_existing, error);
        if (error)
        {
            std::cerr << "Shader Cook failed: " << shader << ": " << error.message() << '\n';
            return 1;
        }
    }
    manifest << "fbx_sdk=2020.3.7-vs2022\n"
             << "mesh=stage1_archer.meshbin\n"
             << "texture=stage1_white.dds\n";
    for (const auto shader : shaders)
    {
        manifest << "shader=" << shader << '\n';
    }
    std::cout << "content.validate meshes=" << header.mesh_count << " bones=" << header.bone_count
              << '\n';
    return 0;
}
