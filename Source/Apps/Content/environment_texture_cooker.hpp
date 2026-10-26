#pragma once
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
struct ID3D11Device;
namespace hs::content
{
struct EnvironmentTextureRoleRequest
{
    std::string group;
    std::string role;
    std::filesystem::path output;
};

void ValidateEnvironmentTextures(const std::filesystem::path& source);
void CookEnvironmentTextures(const std::filesystem::path& source, const std::filesystem::path& output,
                             ID3D11Device* device);
std::vector<std::filesystem::path> EnvironmentTextureInputs(const std::filesystem::path& source);
std::vector<std::string> EnvironmentTextureProducts(const std::filesystem::path& source);
std::vector<std::filesystem::path> EnvironmentTextureRoleInputs(
    const std::filesystem::path& source, const std::string& group, const std::string& role);
std::string EnvironmentTextureRoleProduct(
    const std::filesystem::path& source, const std::string& group, const std::string& role);
void CookEnvironmentTextureRole(const std::filesystem::path& source,
                                const std::filesystem::path& output,
                                const std::string& group, const std::string& role,
                                ID3D11Device* device);
void CookEnvironmentTextureRoles(const std::filesystem::path& source,
                                const std::vector<EnvironmentTextureRoleRequest>& requests,
                                ID3D11Device* device);
}
