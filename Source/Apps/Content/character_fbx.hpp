#pragma once

#include "character_cooker.hpp"

#include <fbxsdk.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hs::content
{
FbxScene *LoadFbx(FbxManager &manager, const std::filesystem::path &path,
                  std::string_view scene_name);
std::vector<FbxNode *> CharacterMeshNodes(FbxScene &scene, bool ground_archer);
std::vector<std::uint16_t> GatherCharacterNodeMaterials(FbxNode &node,
    CharacterIR &output, const std::filesystem::path &model_path, bool allow_missing);
}
