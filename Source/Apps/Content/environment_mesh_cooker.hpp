#pragma once
#include <filesystem>
namespace hs::content
{
void ValidateEnvironmentMeshes(const std::filesystem::path &source_json);
void CookEnvironmentMeshes(const std::filesystem::path &source_json,
                           const std::filesystem::path &output);
}
