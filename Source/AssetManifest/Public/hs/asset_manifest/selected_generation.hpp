#pragma once

#include <hs/core/result.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hs
{

class GenerationFileLock;

struct SelectedGeneration
{
    std::string id;
    std::string scope;
    std::unordered_map<std::string, std::filesystem::path> product_paths;
    std::unordered_map<std::string, std::string> product_digests;
    std::unordered_map<std::string, std::vector<std::string>> product_dependencies;

    [[nodiscard]] Result Require(std::string_view logical_id,
                                 std::filesystem::path &path) const;

private:
    // Copies share each ProductSet lease; the final owner releases them.
    std::vector<std::shared_ptr<GenerationFileLock>> reader_leases_;
    friend Result LoadSelectedGeneration(const std::filesystem::path &, SelectedGeneration &);
};

[[nodiscard]] Result LoadSelectedGeneration(
    const std::filesystem::path &manifest_path, SelectedGeneration &generation);

} // namespace hs
