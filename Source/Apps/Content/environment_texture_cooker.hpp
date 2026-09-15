#pragma once
#include <filesystem>
namespace hs::content
{
void ValidateEnvironmentTextures(const std::filesystem::path& source);
void CookEnvironmentTextures(const std::filesystem::path& source, const std::filesystem::path& output);
}
