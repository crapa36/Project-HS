#pragma once
#include <filesystem>
#include <vector>

struct ID3D11Device;
namespace hs::content
{
void CookEnvironmentHistogram(const std::filesystem::path& source,
    const std::filesystem::path& output, ID3D11Device* device);
void CookEnvironmentHistogram(const std::filesystem::path& source,
    const std::filesystem::path& output, ID3D11Device* device, bool use_cache);
std::vector<std::filesystem::path> EnvironmentHistogramInputs(const std::filesystem::path& source);
}
