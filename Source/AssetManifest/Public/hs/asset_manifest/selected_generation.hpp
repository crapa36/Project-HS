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
    // Copies share the read lease; the final owner releases it.
    std::shared_ptr<GenerationFileLock> reader_lease_;
    friend Result LoadSelectedGeneration(const std::filesystem::path &, SelectedGeneration &);
};

[[nodiscard]] Result LoadSelectedGeneration(
    const std::filesystem::path &pointer_path, SelectedGeneration &generation);

// Reads one pointer snapshot while allowing atomic replacement of its pathname.
[[nodiscard]] Result ReadGenerationPointer(
    const std::filesystem::path &pointer_path, std::string &bytes);

// Validate the immutable payload bytes against the manifest identities. Startup
// selection intentionally performs only structural and size checks; callers
// preparing a candidate for adoption invoke this explicit integrity pass.
[[nodiscard]] Result VerifySelectedGenerationProducts(
    const SelectedGeneration &generation);

} // namespace hs
