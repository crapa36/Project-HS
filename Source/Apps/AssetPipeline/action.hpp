#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hs::asset_pipeline
{
namespace fs = std::filesystem;
using Json = nlohmann::json;

struct Input
{
    std::string logical_id;
    fs::path path;
    std::string digest;
};

struct Product
{
    std::string logical_id;
    std::string filename;
    std::string digest;
    std::uint64_t size{};
    std::vector<std::string> dependencies;
};

struct UpstreamProduct
{
    std::string logical_id;
    std::string digest;
    std::uint64_t size{};
    fs::path result_path;
};

struct ActionSpec
{
    std::string action_id;
    std::string recipe_id;
    std::uint32_t recipe_version{};
    std::string namespace_path;
    bool include_namespace_field = true;
    std::vector<Input> inputs;
    std::vector<UpstreamProduct> upstream;
};

class ActionResultSnapshot
{
  public:
    ActionResultSnapshot(const fs::path &build_root, std::string_view namespace_path);
    ActionResultSnapshot(const ActionResultSnapshot &) = delete;
    ActionResultSnapshot &operator=(const ActionResultSnapshot &) = delete;
    ActionResultSnapshot(ActionResultSnapshot &&) noexcept = default;
    ActionResultSnapshot &operator=(ActionResultSnapshot &&) noexcept = default;

    [[nodiscard]] std::string_view ActionId() const;
    [[nodiscard]] std::string_view ActionKeyValue() const;
    [[nodiscard]] std::size_t ProductCount() const;
    [[nodiscard]] std::vector<std::string_view> ProductIds() const;
    // On failure output may contain partial bytes; the caller must discard its staging output.
    void CopyProductBytesTo(std::string_view product_id, std::ostream &output, UpstreamProduct &dependency) const;
    [[nodiscard]] bool HasProduct(std::string_view product_id) const;
    [[nodiscard]] const Json &ProductMetadata(std::string_view product_id) const;
    void ReadProduct(std::string_view product_id, UpstreamProduct &dependency,
                     fs::path &product_path) const;
    // Decode this owned buffer instead of reopening the verified product path.
    [[nodiscard]] std::string ReadProductBytes(std::string_view product_id,
                                               UpstreamProduct &dependency) const;

  private:
    fs::path build_root_;
    fs::path result_path_;
    std::string namespace_path_;
    Json result_;
    std::map<std::string, std::size_t> product_indexes_;
    void ResolveProduct(std::string_view product_id, UpstreamProduct &dependency,
                        fs::path &product_path) const;

    friend Json ReadActionResult(const fs::path &, std::string_view, std::string_view,
                                 UpstreamProduct &, fs::path &);
};

void AtomicPublishFile(const fs::path &temporary, const fs::path &destination);

class Sha256Stream
{
  public:
    Sha256Stream();
    ~Sha256Stream();
    Sha256Stream(const Sha256Stream &) = delete;
    Sha256Stream &operator=(const Sha256Stream &) = delete;
    Sha256Stream(Sha256Stream &&) noexcept;
    Sha256Stream &operator=(Sha256Stream &&) noexcept;
    void Add(std::span<const std::byte> bytes);
    [[nodiscard]] std::string Finish();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string Sha256File(const fs::path &path);
[[nodiscard]] std::string Sha256Bytes(std::span<const std::byte> bytes);
[[nodiscard]] std::string ReadBytes(const fs::path &path);
[[nodiscard]] Input HashInput(std::string logical_id, fs::path path);
[[nodiscard]] Input CopyAndHashInput(std::string logical_id, const fs::path &source,
                                    const fs::path &target);
[[nodiscard]] Product HashProduct(std::string logical_id, std::string filename,
                                  const fs::path &path);
[[nodiscard]] std::string ProductSetId(std::span<const Product> products);
[[nodiscard]] std::string ActionKey(const ActionSpec &spec);
[[nodiscard]] Json ReadActionResult(const fs::path &build_root,
                                    std::string_view namespace_path,
                                    std::string_view product_id,
                                    UpstreamProduct &dependency,
                                    fs::path &product_path);
void WriteBytes(const fs::path &path, std::span<const std::byte> bytes);
void AtomicWriteIfDifferent(const fs::path &path, std::string_view bytes);
void TouchActionResult(const fs::path &path);
void RequireRegularFile(const fs::path &path);
void RequireDirectory(const fs::path &path);
void MakeDirectory(const fs::path &path);
[[nodiscard]] fs::path CreateStage(const fs::path &actions);
void RemoveStage(const fs::path &stage);
void Publish(const fs::path &build_root, const ActionSpec &spec,
             const fs::path &stage, std::vector<Product> products);
[[nodiscard]] std::string DiagnosticPath(const fs::path &path);
void ReportFailure(std::string_view producer_id, std::string_view reason,
                   const Json &context,
                   std::string_view error_code = "HS_ASSET_ACTION_FAILED");

} // namespace hs::asset_pipeline
