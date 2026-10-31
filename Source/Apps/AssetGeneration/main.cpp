#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#else
#include <hs/core/sha256.hpp>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>
#include <hs/asset_manifest/generation_lock.hpp>
#include <hs/asset_manifest/selected_generation.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;

fs::path Utf8Path(std::string_view value)
{
    const auto *begin = reinterpret_cast<const char8_t *>(value.data());
    return fs::path(std::u8string(begin, begin + value.size()));
}

std::string DiagnosticPath(const fs::path &path)
{
    try
    {
        const auto utf8 = path.u8string();
        return {reinterpret_cast<const char *>(utf8.data()), utf8.size()};
    }
    catch (const std::system_error &)
    {
        return "<path cannot be encoded as UTF-8>";
    }
}

struct Failure : std::runtime_error
{
    Failure(std::string error_code, std::string action, std::string detail)
        : std::runtime_error(std::move(detail)), code(std::move(error_code)),
          action_id(std::move(action))
    {
    }

    std::string code;
    std::string action_id;
};

[[noreturn]] void Fail(std::string code, const std::string &action_id,
                       std::string detail)
{
    throw Failure(std::move(code), action_id, std::move(detail));
}

std::string Hex(const std::array<unsigned char, 32> &bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const unsigned char byte : bytes)
    {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

#ifdef _WIN32
class Sha256
{
  public:
    Sha256()
    {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr,
                                        0) < 0)
        {
            throw std::runtime_error("BCrypt SHA-256 initialization failed");
        }
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0)
        {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            algorithm_ = nullptr;
            throw std::runtime_error("BCrypt SHA-256 initialization failed");
        }
    }

    Sha256(const Sha256 &) = delete;
    Sha256 &operator=(const Sha256 &) = delete;

    ~Sha256()
    {
        if (hash_)
        {
            BCryptDestroyHash(hash_);
        }
        if (algorithm_)
        {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
        }
    }

    void Add(const unsigned char *data, std::size_t size)
    {
        if (size > std::numeric_limits<ULONG>::max() ||
            BCryptHashData(hash_, const_cast<PUCHAR>(data),
                           static_cast<ULONG>(size), 0) < 0)
        {
            throw std::runtime_error("BCrypt SHA-256 update failed");
        }
    }

    std::string Finish()
    {
        std::array<unsigned char, 32> result{};
        if (BCryptFinishHash(hash_, result.data(),
                             static_cast<ULONG>(result.size()), 0) < 0)
        {
            throw std::runtime_error("BCrypt SHA-256 finish failed");
        }
        return Hex(result);
    }

  private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

#else
class Sha256
{
  public:
    void Add(const unsigned char *data, std::size_t size)
    {
        hash_.Add({reinterpret_cast<const std::byte *>(data), size});
    }
    std::string Finish()
    {
        std::array<unsigned char, 32> bytes{};
        const auto digest = hash_.Finish();
        for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = std::to_integer<unsigned char>(digest[i]);
        return Hex(bytes);
    }
  private:
    hs::Sha256Stream hash_;
};
#endif

std::string HashBytes(std::string_view bytes)
{
    Sha256 hash;
    hash.Add(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size());
    return hash.Finish();
}

bool IsLowerSha256(std::string_view digest)
{
    return digest.size() == 64 &&
           std::all_of(digest.begin(), digest.end(), [](char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
           });
}

bool IsActionId(std::string_view value)
{
    if (value.empty() || value.size() > 128 || value == "." || value == "..")
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
    });
}

std::string RequiredString(const Json &object, const char *field,
                           const std::string &action_id)
{
    if (!object.is_object() || !object.contains(field) ||
        !object[field].is_string() || object[field].get<std::string>().empty())
    {
        Fail("invalid-schema", action_id, std::string("missing string: ") + field);
    }
    return object[field].get<std::string>();
}

std::uint64_t RequiredSize(const Json &object, const std::string &action_id)
{
    if (!object.is_object() || !object.contains("size") ||
        !object["size"].is_number_unsigned())
    {
        Fail("invalid-schema", action_id, "invalid product size");
    }
    return object["size"].get<std::uint64_t>();
}

void RequireVersion(const Json &object, const std::string &action_id)
{
    if (!object.is_object() || !object.contains("schema_version") ||
        !object["schema_version"].is_number_integer() ||
        object["schema_version"].get<std::int64_t>() != 1)
    {
        Fail("invalid-schema", action_id, "expected schema_version=1");
    }
}

Json ReadJson(const fs::path &path, const std::string &missing_code,
              const std::string &action_id)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        Fail(missing_code, action_id, "cannot read " + path.string());
    }
    try
    {
        return Json::parse(input);
    }
    catch (const Json::exception &error)
    {
        Fail("invalid-schema", action_id,
             path.string() + ": " + error.what());
    }
}

std::string ReadBytes(const fs::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("cannot read " + path.string());
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

void WriteBytes(const fs::path &path, std::string_view bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output || !output.write(bytes.data(),
                                 static_cast<std::streamsize>(bytes.size())) ||
        !output.flush())
    {
        throw std::runtime_error("cannot write " + path.string());
    }
}

void RejectReparse(const fs::path &path, const std::string &action_id)
{
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        {
            return;
        }
        Fail("path-error", action_id,
             "cannot inspect " + path.string() + ": " + std::to_string(error));
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        Fail("path-escape", action_id, "reparse point: " + path.string());
    }
#else
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory)
        return;
    if (error)
        Fail("path-error", action_id, "cannot inspect " + path.string() + ": " + error.message());
    if (fs::is_symlink(status))
        Fail("path-escape", action_id, "symbolic link: " + path.string());
#endif
}

void CheckPath(const fs::path &root, const fs::path &relative,
               const std::string &action_id)
{
    RejectReparse(root, action_id);
    fs::path cursor = root;
    for (const auto &part : relative)
    {
        cursor /= part;
        RejectReparse(cursor, action_id);
    }
}

fs::path RelativeLocator(const std::string &locator,
                         const std::string &action_id)
{
    if (locator.empty() || locator.find('\\') != std::string::npos ||
        locator.find(':') != std::string::npos ||
        locator.find('\0') != std::string::npos)
    {
        Fail("invalid-locator", action_id, "invalid product locator");
    }
    const fs::path relative = Utf8Path(locator);
    if (relative.is_absolute() || relative.has_root_path())
    {
        Fail("path-escape", action_id, "absolute product locator: " + locator);
    }
    for (const auto &part : relative)
    {
        if (part == "." || part == ".." || part.empty())
        {
            Fail("path-escape", action_id, "unsafe product locator: " + locator);
        }
    }
    return relative;
}

fs::path NamespacePath(std::string_view value, const std::string &action_id)
{
    const auto relative = RelativeLocator(std::string(value), action_id);
    if (relative.generic_string() != value)
        Fail("invalid-namespace", action_id, "noncanonical action namespace");
    for (const auto &part : relative)
        if (!IsActionId(part.string()))
            Fail("invalid-namespace", action_id, "unsafe action namespace");
    return relative;
}

std::string ResultNamespace(const Json &result, const std::string &action_id)
{
    std::string value;
    if (result.contains("namespace_path"))
        value = RequiredString(result, "namespace_path", action_id);
    else if (action_id == "audio")
        value = "audio";
    else
        Fail("invalid-schema", action_id, "missing namespace_path");
    NamespacePath(value, action_id);
    return value;
}

struct Product
{
    std::string action_id;
    std::string namespace_path;
    std::string product_set_id;
    std::string logical_id;
    std::string locator;
    std::string digest;
    std::uint64_t size = 0;
    std::vector<std::string> dependencies;
};

struct UpstreamProductDependency
{
    std::string action_id;
    std::string logical_id;
    std::string digest;
    std::uint64_t size = 0;
};

std::vector<UpstreamProductDependency> ParseUpstreamDependencies(
    const Json &document, const std::string &action_id, bool from_manifest = false)
{
    if (!document.contains("upstream_product_dependencies") ||
        !document["upstream_product_dependencies"].is_array())
        Fail("invalid-schema", action_id,
             "upstream_product_dependencies must be an array");
    std::vector<UpstreamProductDependency> dependencies;
    for (const Json &entry : document["upstream_product_dependencies"])
    {
        UpstreamProductDependency dependency{
            from_manifest ? RequiredString(entry, "action_id", action_id) : action_id,
            RequiredString(entry, "logical_id", action_id),
            RequiredString(entry, "digest", action_id),
            RequiredSize(entry, action_id)};
        if (!IsActionId(dependency.action_id) ||
            dependency.digest.size() != 71 ||
            dependency.digest.substr(0, 7) != "sha256:" ||
            !IsLowerSha256(std::string_view(dependency.digest).substr(7)))
            Fail("invalid-schema", action_id,
                 "invalid upstream product identity: " + dependency.logical_id);
        dependencies.push_back(std::move(dependency));
    }
    std::sort(dependencies.begin(), dependencies.end(),
              [](const auto &left, const auto &right) {
                  return std::tie(left.action_id, left.logical_id) <
                         std::tie(right.action_id, right.logical_id);
              });
    for (std::size_t index = 1; index < dependencies.size(); ++index)
        if (dependencies[index - 1].action_id == dependencies[index].action_id &&
            dependencies[index - 1].logical_id == dependencies[index].logical_id)
            Fail("invalid-schema", action_id,
                 "duplicate upstream product dependency: " +
                     dependencies[index].logical_id);
    return dependencies;
}

std::vector<std::string> ParseDependencies(const Json &entry,
                                           const std::string &action_id,
                                           const std::string &logical_id)
{
    if (!entry.contains("dependencies"))
        return {};
    if (!entry["dependencies"].is_array())
        Fail("invalid-schema", action_id, "product dependencies must be an array");
    std::vector<std::string> dependencies;
    for (const auto &value : entry["dependencies"])
    {
        if (!value.is_string() || value.get<std::string>().empty())
            Fail("invalid-schema", action_id, "invalid product dependency ID");
        dependencies.push_back(value.get<std::string>());
    }
    if (!std::is_sorted(dependencies.begin(), dependencies.end()) ||
        std::adjacent_find(dependencies.begin(), dependencies.end()) !=
            dependencies.end() ||
        std::ranges::find(dependencies, logical_id) != dependencies.end())
        Fail("invalid-schema", action_id,
             "product dependencies are not sorted unique IDs: " + logical_id);
    return dependencies;
}

std::vector<Product> ParseProducts(const Json &document,
                                   const std::string &action_id,
                                   bool from_manifest = false,
                                   const std::string &result_namespace = {},
                                   const std::string &result_set_id = {})
{
    if (!document.contains("products") || !document["products"].is_array() ||
        document["products"].empty())
    {
        Fail("invalid-schema", action_id, "products must be a nonempty array");
    }
    std::vector<Product> products;
    products.reserve(document["products"].size());
    for (const Json &entry : document["products"])
    {
        Product product{from_manifest ? RequiredString(entry, "action_id", action_id)
                                      : action_id,
                        from_manifest ? RequiredString(entry, "namespace_path", action_id)
                                      : result_namespace,
                        from_manifest ? (document.value("schema_version", 0) == 3
                                             ? std::string{}
                                             : RequiredString(entry, "product_set_id", action_id))
                                      : result_set_id,
                        RequiredString(entry, "logical_id", action_id),
                        RequiredString(entry, "locator", action_id),
                        RequiredString(entry, "digest", action_id),
                        RequiredSize(entry, action_id), {}};
        product.dependencies = ParseDependencies(entry, action_id, product.logical_id);
        if (product.digest.size() != 71 ||
            product.digest.substr(0, 7) != "sha256:" ||
            !IsLowerSha256(std::string_view(product.digest).substr(7)))
        {
            Fail("invalid-schema", action_id,
                 "invalid digest for " + product.logical_id);
        }
        RelativeLocator(product.locator, action_id);
        products.push_back(std::move(product));
    }
    std::sort(products.begin(), products.end(),
              [](const Product &left, const Product &right) {
                  return left.logical_id < right.logical_id;
              });
    for (std::size_t index = 1; index < products.size(); ++index)
    {
        if (products[index - 1].logical_id == products[index].logical_id)
        {
            Fail("invalid-schema", action_id,
                 "duplicate logical_id: " + products[index].logical_id);
        }
    }
    return products;
}

void RequireDependencyClosure(const std::vector<Product> &products)
{
    // Products are sorted by logical ID. Checking every selected product also
    // checks the complete transitive closure of its declared dependencies.
    std::vector<std::uint8_t> state(products.size(), 0);
    std::vector<std::pair<std::size_t, std::size_t>> pending;
    pending.reserve(products.size());
    for (std::size_t index = 0; index < products.size(); ++index)
    {
        if (state[index] == 2) continue;
        state[index] = 1;
        pending.emplace_back(index, 0);
        while (!pending.empty())
        {
            auto &[current, next] = pending.back();
            const auto &dependencies = products[current].dependencies;
            if (next == dependencies.size())
            {
                state[current] = 2;
                pending.pop_back();
                continue;
            }
            const auto &dependency = dependencies[next++];
            const auto found = std::lower_bound(
                products.begin(), products.end(), dependency,
                [](const Product &product, const std::string &id) {
                    return product.logical_id < id;
                });
            if (found == products.end() || found->logical_id != dependency)
                Fail("missing-product-dependency", products[current].action_id,
                     products[current].logical_id + " -> " + dependency);
            const auto child = static_cast<std::size_t>(found - products.begin());
            if (state[child] == 1)
                Fail("cyclic-product-dependency", products[child].action_id,
                     products[child].logical_id);
            if (state[child] == 0)
            {
                state[child] = 1;
                pending.emplace_back(child, 0);
            }
        }
    }
}

void RequireUpstreamIdentity(
    const std::vector<Product> &products,
    const std::vector<UpstreamProductDependency> &dependencies)
{
    for (const auto &dependency : dependencies)
    {
        const auto found = std::lower_bound(
            products.begin(), products.end(), dependency.logical_id,
            [](const Product &product, const std::string &id) {
                return product.logical_id < id;
            });
        if (found == products.end() || found->logical_id != dependency.logical_id)
            Fail("missing-upstream-product", dependency.action_id,
                 "missing selected upstream product: " + dependency.logical_id);
        if (found->digest != dependency.digest || found->size != dependency.size)
            Fail("upstream-product-mismatch", dependency.action_id,
                 "selected upstream product identity differs: " +
                     dependency.logical_id);
        if (found->action_id == dependency.action_id)
            Fail("invalid-schema", dependency.action_id,
                 "ActionResult declares its own product as upstream: " +
                     dependency.logical_id);
    }
}

std::vector<std::string> RequiredIds(const Json &manifest,
                                     const std::vector<Product> &products,
                                     const std::string &action_id)
{
    if (!manifest.contains("required_ids") ||
        !manifest["required_ids"].is_array() ||
        manifest["required_ids"].size() != products.size())
        Fail("invalid-manifest", action_id, "required_ids/product count mismatch");
    std::vector<std::string> ids;
    ids.reserve(products.size());
    for (const auto &entry : manifest["required_ids"])
    {
        if (!entry.is_string() || entry.get<std::string>().empty())
            Fail("invalid-manifest", action_id, "invalid required logical ID");
        ids.push_back(entry.get<std::string>());
    }
    for (std::size_t index = 0; index < products.size(); ++index)
    {
        if (ids[index] != products[index].logical_id)
            Fail("invalid-manifest", action_id, "required_ids are not canonical products");
    }
    return ids;
}

std::vector<std::string> ActionIds(const std::vector<Product> &products)
{
    std::vector<std::string> ids;
    for (const auto &product : products)
        ids.push_back(product.action_id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

void CheckProductNamespace(const Product &product)
{
    if (!IsActionId(product.action_id) ||
        !IsLowerSha256(product.product_set_id))
        Fail("invalid-schema", product.action_id, "invalid action or ProductSet ID");
    NamespacePath(product.namespace_path, product.action_id);
    const auto relative = RelativeLocator(product.locator, product.action_id);
    if (relative.generic_string() != product.locator)
        Fail("invalid-locator", product.action_id, "noncanonical product locator");
    const auto prefix = "Products/" + product.namespace_path + "/" +
                        product.product_set_id + "/";
    if (!product.locator.starts_with(prefix) ||
        product.locator.size() == prefix.size())
        Fail("invalid-locator", product.action_id,
             "product locator does not belong to its ProductSet");
}

std::pair<std::string, std::string> ProductNamespace(const Product &product)
{
    CheckProductNamespace(product);
    return {product.namespace_path, product.product_set_id};
}

Json ProductJson(const Product &product)
{
    Json json{{"action_id", product.action_id},
              {"namespace_path", product.namespace_path},
              {"product_set_id", product.product_set_id},
              {"logical_id", product.logical_id},
              {"locator", product.locator},
              {"digest", product.digest},
              {"size", product.size}};
    if (!product.dependencies.empty())
        json["dependencies"] = product.dependencies;
    return json;
}

Json UpstreamDependencyJson(const UpstreamProductDependency &dependency)
{
    return Json{{"action_id", dependency.action_id},
                {"logical_id", dependency.logical_id},
                {"digest", dependency.digest},
                {"size", dependency.size}};
}

Json GenerationProductJson(const Product &product, bool with_locator, int version)
{
    if (version == 2)
        return ProductJson(product);
    Json json{{"action_id", product.action_id},
              {"namespace_path", product.namespace_path},
              {"logical_id", product.logical_id}};
    if (with_locator)
        json["locator"] = product.locator;
    json["digest"] = product.digest;
    json["size"] = product.size;
    if (!product.dependencies.empty())
        json["dependencies"] = product.dependencies;
    return json;
}

std::string SnapshotLocator(const std::string &generation_id,
                            const std::string &logical_id)
{
    return "Generations/" + generation_id + "/products/" +
           HashBytes(logical_id) + ".bin";
}

void CheckGenerationProduct(const Product &product,
                            const std::string &generation_id, int version)
{
    if (version == 2)
    {
        CheckProductNamespace(product);
        return;
    }
    if (!IsActionId(product.action_id))
        Fail("invalid-schema", product.action_id, "invalid action ID");
    NamespacePath(product.namespace_path, product.action_id);
    if (product.locator != SnapshotLocator(generation_id, product.logical_id))
        Fail("invalid-locator", product.action_id,
             "product locator does not belong to its Generation snapshot");
}

std::string GenerationId(const std::string &scope,
                         const std::vector<std::string> &required_ids,
                         const std::vector<Product> &products,
                         const std::vector<UpstreamProductDependency> &dependencies,
                         int version = 3)
{
    Json semantics{{"schema_version", version}, {"scope", scope},
                   {"required_ids", required_ids},
                   {"products", Json::array()},
                   {"upstream_product_dependencies", Json::array()}};
    for (const Product &product : products)
        semantics["products"].push_back(GenerationProductJson(product, false, version));
    for (const auto &dependency : dependencies)
        semantics["upstream_product_dependencies"].push_back(
            UpstreamDependencyJson(dependency));
    return HashBytes(semantics.dump());
}

std::string ManifestBytes(const std::string &scope,
                          const std::vector<std::string> &required_ids,
                          const std::vector<Product> &products,
                          const std::vector<UpstreamProductDependency> &dependencies,
                          const std::string &generation_id, int version = 3)
{
    Json manifest{{"schema_version", version},
                  {"generation_id", generation_id},
                  {"scope", scope},
                  {"required_ids", required_ids},
                  {"products", Json::array()},
                  {"upstream_product_dependencies", Json::array()}};
    for (const Product &product : products)
        manifest["products"].push_back(GenerationProductJson(product, true, version));
    for (const auto &dependency : dependencies)
        manifest["upstream_product_dependencies"].push_back(
            UpstreamDependencyJson(dependency));
    return manifest.dump(2) + "\n";
}

// Schema 1 is read only by GC so old immutable snapshots can keep their
// ProductSets alive until their pointer, pin, and grace period expire.
std::string LegacyGenerationId(const std::string &scope,
                               const std::vector<std::string> &required_ids,
                               const std::vector<Product> &products)
{
    Json semantics{{"schema_version", 1}, {"scope", scope},
                   {"required_ids", required_ids}, {"products", Json::array()}};
    for (const auto &product : products)
        semantics["products"].push_back(ProductJson(product));
    return HashBytes(semantics.dump());
}

std::string LegacyManifestBytes(const std::string &scope,
                                const std::vector<std::string> &required_ids,
                                const std::vector<Product> &products,
                                const std::string &generation_id)
{
    Json manifest{{"schema_version", 1}, {"generation_id", generation_id},
                  {"scope", scope}, {"required_ids", required_ids},
                  {"products", Json::array()}};
    for (const auto &product : products)
        manifest["products"].push_back(ProductJson(product));
    return manifest.dump(2) + "\n";
}

std::string ScopeFileName(const std::string &scope)
{
    if (scope.empty() || scope.size() > 128)
    {
        Fail("invalid-scope", "-", "scope must contain 1..128 characters");
    }
    std::string encoded;
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char ch : scope)
    {
        if ((ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.')
        {
            encoded.push_back(static_cast<char>(ch));
        }
        else if (ch == ':')
        {
            encoded.push_back('%');
            encoded.push_back(hex[ch >> 4]);
            encoded.push_back(hex[ch & 15]);
        }
        else
        {
            Fail("invalid-scope", "-", "unsafe scope character");
        }
    }
    return encoded + ".json";
}

fs::path BuildRoot(const std::string &value)
{
    fs::path root = Utf8Path(value);
    if (!root.is_absolute() || !fs::is_directory(root))
    {
        Fail("invalid-build-root", "-", "build-root must be an existing absolute directory");
    }
    root = root.lexically_normal();
    fs::path cursor = root.root_path();
    RejectReparse(cursor, "-");
    for (const auto &part : root.relative_path())
    {
        cursor /= part;
        RejectReparse(cursor, "-");
    }
    return root;
}

void VerifyProducts(const fs::path &root, const std::vector<Product> &products,
                    const std::string &action_id,
                    const fs::path &snapshot_directory = {})
{
    for (const Product &product : products)
    {
        const fs::path relative = RelativeLocator(product.locator, action_id);
        CheckPath(root, relative, action_id);
        const fs::path path = root / relative;
        std::error_code error;
        if (!fs::is_regular_file(path, error))
        {
            Fail("missing-product", action_id,
                 product.logical_id + ": " + path.string());
        }
        if (fs::file_size(path, error) != product.size || error)
        {
            Fail("corrupt-product", action_id,
                 product.logical_id + ": size mismatch");
        }
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            Fail("missing-product", action_id,
                 product.logical_id + ": cannot open product");
        }
        std::ofstream output;
        fs::path snapshot_relative;
        if (!snapshot_directory.empty())
        {
            const auto destination = snapshot_directory / (HashBytes(product.logical_id) + ".bin");
            snapshot_relative = destination.lexically_relative(root);
            CheckPath(root, snapshot_relative, action_id);
            // New snapshots copy the exact bytes being hashed without overwriting an existing file.
            output.open(destination, std::ios::binary | std::ios::noreplace);
            if (!output)
                Fail("io-error", action_id, product.logical_id + ": cannot create snapshot product");
        }
        Sha256 hash;
        std::array<unsigned char, 64 * 1024> buffer{};
        std::uint64_t actual_size = 0;
        while (input)
        {
            input.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
            const auto count = input.gcount();
            if (count > 0)
            {
                hash.Add(buffer.data(), static_cast<std::size_t>(count));
                actual_size += static_cast<std::uint64_t>(count);
                if (output.is_open())
                {
                    output.write(reinterpret_cast<const char *>(buffer.data()), count);
                    if (!output)
                        Fail("io-error", action_id, product.logical_id + ": cannot write snapshot product");
                }
            }
        }
        if (output.is_open())
        {
            output.close();
            if (!output)
                Fail("io-error", action_id, product.logical_id + ": cannot finalize snapshot product");
            CheckPath(root, snapshot_relative, action_id);
        }
        if (input.bad() || actual_size != product.size ||
            "sha256:" + hash.Finish() != product.digest)
        {
            Fail("corrupt-product", action_id,
                 product.logical_id + ": digest/size mismatch");
        }
        CheckPath(root, relative, action_id);
    }
}

std::string TempSuffix()
{
    static std::atomic<unsigned> sequence = 0;
#ifdef _WIN32
    const auto process = GetCurrentProcessId();
    const auto ticks = GetTickCount64();
#else
    const auto process = getpid();
    const auto ticks = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
    return ".tmp-" + std::to_string(process) + "-" + std::to_string(ticks) + "-" +
           std::to_string(sequence.fetch_add(1));
}

void AtomicReplace(const fs::path &destination, std::string_view bytes)
{
    if (fs::exists(destination) && ReadBytes(destination) == std::string(bytes))
        return;
    const std::string suffix = TempSuffix();
    fs::path temporary = destination;
    temporary += suffix;
    try
    {
        WriteBytes(temporary, bytes);
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            throw std::runtime_error("atomic replace failed: " + destination.string() +
                                     " (Win32 " + std::to_string(GetLastError()) + ")");
        }
#else
        // Same-filesystem rename gives atomic visibility, not power-loss durability.
        fs::rename(temporary, destination);
#endif
    }
    catch (...)
    {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

void CheckTree(const fs::path &root, const fs::path &path);

constexpr std::uint64_t kMaximumPublicationManifestSize = 64 * 1024 * 1024;

struct PendingPublication
{
    fs::path path;
    std::string generation_id;
    std::string manifest_digest;
    std::uint64_t manifest_size;
};

PendingPublication LoadPendingPublication(const fs::path &root, const fs::path &path)
{
    CheckPath(root, path.lexically_relative(root), "-");
    if (!fs::is_regular_file(path) || fs::file_size(path) > 1024)
        Fail("invalid-publication", "-", "invalid pending Generation marker: " + path.string());
    const auto marker = ReadJson(path, "invalid-publication", "-");
    RequireVersion(marker, "-");
    if (marker.size() != 5 || !marker.contains("kind") ||
        !marker.contains("generation_id") || !marker.contains("manifest_digest") ||
        !marker.contains("manifest_size") ||
        RequiredString(marker, "kind", "-") != "project-hs-generation-publication")
        Fail("invalid-publication", "-", "invalid pending Generation marker keys/kind");
    PendingPublication publication{path, RequiredString(marker, "generation_id", "-"),
        RequiredString(marker, "manifest_digest", "-"), 0};
    if (!IsLowerSha256(publication.generation_id) ||
        path.filename().string() != ".pending-" + publication.generation_id + ".json" ||
        publication.manifest_digest.size() != 71 ||
        !publication.manifest_digest.starts_with("sha256:") ||
        !IsLowerSha256(std::string_view(publication.manifest_digest).substr(7)) ||
        !marker["manifest_size"].is_number_unsigned())
        Fail("invalid-publication", "-", "invalid pending Generation marker identity");
    publication.manifest_size = marker["manifest_size"].get<std::uint64_t>();
    if (publication.manifest_size == 0 ||
        publication.manifest_size > kMaximumPublicationManifestSize)
        Fail("invalid-publication", "-", "invalid pending Generation manifest size");
    return publication;
}

std::string PendingPublicationBytes(const std::string &generation_id,
                                    std::string_view manifest_bytes)
{
    if (manifest_bytes.empty() || manifest_bytes.size() > kMaximumPublicationManifestSize)
        Fail("invalid-publication", "-", "Generation manifest exceeds publication size bound");
    return Json{{"schema_version", 1}, {"kind", "project-hs-generation-publication"},
                {"generation_id", generation_id},
                {"manifest_digest", "sha256:" + HashBytes(manifest_bytes)},
                {"manifest_size", manifest_bytes.size()}}.dump(2) + "\n";
}

void Compose(const fs::path &root, const std::string &scope,
             const std::vector<fs::path> &result_paths,
             std::vector<std::string> required_ids)
{
    std::vector<Product> available;
    std::vector<UpstreamProductDependency> upstream_dependencies;
    Json action_provenance = Json::array();
    std::set<std::string> seen_actions;
    for (const auto &result_path : result_paths)
    {
        if (!result_path.is_absolute())
            Fail("invalid-action-result", "-", "result must be an absolute path");
        const auto relative = result_path.lexically_normal().lexically_relative(root);
        RelativeLocator(relative.generic_string(), "-");
        CheckPath(root, relative, "-");
        const Json result = ReadJson(result_path, "missing-action-result", "-");
        const std::string action_id = RequiredString(result, "action_id", "-");
        const std::string namespace_path = ResultNamespace(result, action_id);
        if (!IsActionId(action_id) ||
            result_path.lexically_normal() !=
                (root / "Actions" / NamespacePath(namespace_path, action_id) /
                 "result.json"))
            Fail("invalid-action-result", action_id,
                 "result must match Actions/<namespace_path>/result.json");
        if (!seen_actions.insert(action_id).second)
            Fail("duplicate-action", action_id, "duplicate ActionResult");
        RequireVersion(result, action_id);
        const auto action_key = RequiredString(result, "action_key", action_id);
        const auto product_set_id = RequiredString(result, "product_set_id", action_id);
        if (!IsLowerSha256(product_set_id))
            Fail("invalid-schema", action_id, "invalid ProductSet ID");
        action_provenance.push_back(Json{{"action_id", action_id},
                                        {"namespace_path", namespace_path},
                                        {"action_key", action_key},
                                        {"product_set_id", product_set_id}});
        auto products = ParseProducts(result, action_id, false,
                                      namespace_path, product_set_id);
        for (const auto &product : products)
            CheckProductNamespace(product);
        available.insert(available.end(), products.begin(), products.end());
        auto dependencies = ParseUpstreamDependencies(result, action_id);
        upstream_dependencies.insert(upstream_dependencies.end(),
                                     dependencies.begin(), dependencies.end());
    }
    std::sort(available.begin(), available.end(),
              [](const Product &left, const Product &right) {
                  return left.logical_id < right.logical_id;
              });
    for (std::size_t index = 1; index < available.size(); ++index)
        if (available[index - 1].logical_id == available[index].logical_id)
            Fail("duplicate-logical-id", "-",
                 "duplicate logical ID: " + available[index].logical_id);

    if (required_ids.empty())
    {
        for (const auto &product : available)
            required_ids.push_back(product.logical_id);
    }
    else
    {
        std::sort(required_ids.begin(), required_ids.end());
        if (std::adjacent_find(required_ids.begin(), required_ids.end()) !=
            required_ids.end())
            Fail("duplicate-required-id", "-", "duplicate required logical ID");
    }
    std::vector<Product> products;
    products.reserve(required_ids.size());
    for (const auto &id : required_ids)
    {
        const auto found = std::lower_bound(
            available.begin(), available.end(), id,
            [](const Product &product, const std::string &key) {
                return product.logical_id < key;
            });
        if (found == available.end() || found->logical_id != id)
            Fail("missing-required-product", "-", "missing logical ID: " + id);
        products.push_back(*found);
    }
    RequireDependencyClosure(products);
    const auto action_ids = ActionIds(products);
    upstream_dependencies.erase(
        std::remove_if(upstream_dependencies.begin(), upstream_dependencies.end(),
                       [&](const auto &dependency) {
                           return !std::binary_search(action_ids.begin(),
                                                      action_ids.end(),
                                                      dependency.action_id);
                       }),
        upstream_dependencies.end());
    std::sort(upstream_dependencies.begin(), upstream_dependencies.end(),
               [](const auto &left, const auto &right) {
                   return std::tie(left.action_id, left.logical_id) <
                          std::tie(right.action_id, right.logical_id);
               });
    RequireUpstreamIdentity(products, upstream_dependencies);

    const std::string generation_id = GenerationId(
        scope, required_ids, products, upstream_dependencies);
    const auto source_products = products;
    for (auto &product : products)
        product.locator = SnapshotLocator(generation_id, product.logical_id);
    const std::string manifest_bytes = ManifestBytes(
        scope, required_ids, products, upstream_dependencies, generation_id);
    const fs::path generations = root / "Generations";
    const fs::path generation = generations / generation_id;
    const fs::path manifest = generation / "generation.json";
    const fs::path pending = generations / (".pending-" + generation_id + ".json");
    // Reuse/repair still rejects damaged producer bytes before modifying an existing snapshot.
    if (fs::exists(generation))
        VerifyProducts(root, source_products, "-");
    CheckPath(root, fs::path("Generations"), "-");
    CheckPath(root, pending.lexically_relative(root), "-");
    fs::create_directories(generations);
    CheckPath(root, fs::path("Generations"), "-");
    // A repair can stop between retiring its manifest and quarantining its tree.
    // Restore the manifest beside the tree before retrying the normal repair path.
    const auto quarantine = root / "CorruptGenerations";
    CheckPath(root, "CorruptGenerations", "-");
    if (fs::exists(quarantine))
        for (const auto &entry : fs::directory_iterator(quarantine))
        {
            const auto name = entry.path().filename().string();
            if (!name.starts_with(generation_id + ".tmp-") || !name.ends_with(".json"))
                continue;
            CheckPath(root, entry.path().lexically_relative(root), "-");
            if (!entry.is_regular_file() || ReadBytes(entry.path()) != manifest_bytes)
                Fail("generation-conflict", "-", "invalid retired repair manifest: " + generation_id);
            auto retired_tree = entry.path();
            retired_tree.replace_extension();
            CheckPath(root, retired_tree.lexically_relative(root), "-");
            const auto tree = fs::exists(retired_tree) ? retired_tree : generation;
            CheckTree(root, tree);
            if (!fs::is_directory(tree) || fs::exists(tree / "generation.json"))
                Fail("generation-conflict", "-", "invalid interrupted repair tree: " + generation_id);
            fs::rename(entry.path(), tree / "generation.json");
        }
    if (fs::exists(pending))
    {
        const auto publication = LoadPendingPublication(root, pending);
        // A committed manifest wins over a stale, well-formed publication marker.
        if (!fs::exists(manifest))
        {
            if (publication.manifest_size != manifest_bytes.size() ||
                publication.manifest_digest != "sha256:" + HashBytes(manifest_bytes))
                Fail("generation-conflict", "-", "pending Generation identity differs: " + generation_id);
            if (fs::exists(generation))
            {
                CheckTree(root, generation);
                fs::remove_all(generation);
            }
        }
    }
    if (fs::exists(generation) && !fs::exists(manifest))
    {
        // Finish a GC interrupted after retiring this manifest, before republish.
        for (const auto &entry : fs::directory_iterator(generations))
            if (entry.is_regular_file() && entry.path().filename().string().starts_with(".gc-" + generation_id + ".tmp-"))
            {
                CheckTree(root, generation);
                fs::remove_all(generation);
                break;
            }
    }
    if (fs::exists(generation))
    {
        CheckPath(root, fs::path("Generations") / generation_id /
                            "generation.json", "-");
        if (!fs::is_regular_file(manifest) || ReadBytes(manifest) != manifest_bytes)
        {
            Fail("generation-conflict", "-",
                 "existing generation differs: " + generation_id);
        }
        try
        {
            VerifyProducts(root, products, "-");
        }
        catch (const Failure &failure)
        {
            if (failure.code != "missing-product" && failure.code != "corrupt-product")
                throw;
            // Repair must not retire an active reader's canonical paths.
            hs::GenerationFileLock repair_lock(manifest, hs::GenerationFileLock::Mode::TryExclusive);
            if (!repair_lock.OwnsLock())
                Fail("generation-in-use", "-", "cannot repair active Generation: " + generation_id);
            // Repair replaces the complete snapshot; producer bytes never alias it.
            const auto quarantine = root / "CorruptGenerations";
            CheckPath(root, "CorruptGenerations", "-");
            fs::create_directories(quarantine);
            CheckPath(root, "CorruptGenerations", "-");
            const auto destination = quarantine / (generation_id + TempSuffix());
            const auto retired_manifest = fs::path(destination.string() + ".json");
            fs::rename(manifest, retired_manifest);
            repair_lock = {};
            fs::rename(generation, destination);
            fs::rename(retired_manifest, destination / "generation.json");
        }
    }
    if (!fs::exists(generation))
    {
        // The external marker records ownership before any uncommitted tree exists.
        AtomicReplace(pending, PendingPublicationBytes(generation_id, manifest_bytes));
        if (!fs::create_directory(generation))
            Fail("generation-conflict", "-", "Generation directory appeared during publication: " + generation_id);
        try
        {
            fs::create_directory(generation / "products");
            VerifyProducts(root, source_products, "-", generation / "products");
            VerifyProducts(root, products, "-");
            // Publishing the canonical manifest commits the immutable snapshot.
            // Atomic visibility covers process crashes, not power-loss durability.
            CheckPath(root, manifest.lexically_relative(root), "-");
            AtomicReplace(manifest, manifest_bytes);
        }
        catch (...)
        {
            // Never remove a committed manifest/tree, including a failed pointer update.
            try
            {
                if (!fs::exists(manifest))
                {
                    CheckTree(root, generation);
                    std::error_code cleanup_error;
                    fs::remove_all(generation, cleanup_error);
                    if (!cleanup_error)
                    {
                        std::error_code ignored;
                        fs::remove(pending, ignored);
                    }
                }
            }
            catch (...) {} // Preserve the original error and the recovery marker.
            throw;
        }
    }
    // Both existing and newly copied snapshots were fully verified above.
    // Readers require only the committed manifest; marker removal is best effort.
    std::error_code marker_cleanup_error;
    fs::remove(pending, marker_cleanup_error);

    const fs::path state_dir = root / "GenerationState";
    CheckPath(root, fs::path("GenerationState"), "-");
    fs::create_directories(state_dir);
    CheckPath(root, fs::path("GenerationState"), "-");
    const std::string locator = "Generations/" + generation_id + "/generation.json";
    Json pointer{{"schema_version", 1},
                 {"scope", scope},
                 {"action_ids", action_ids},
                 {"generation_id", generation_id},
                 {"manifest_locator", locator}};
    if (action_ids.size() == 1)
        pointer["action_id"] = action_ids.front();
    const fs::path state_relative = fs::path("GenerationState") / ScopeFileName(scope);
    CheckPath(root, state_relative, "-");
    AtomicReplace(root / state_relative, pointer.dump(2) + "\n");
    // Physical producer provenance is observational, never immutable identity.
    const auto receipts_relative = fs::path("GenerationReceipts") / ScopeFileName(scope);
    CheckPath(root, receipts_relative, "-");
    fs::create_directories(root / receipts_relative);
    CheckPath(root, receipts_relative, "-");
    Json receipt{{"schema_version", 1}, {"generation_id", generation_id},
                 {"scope", scope}, {"actions", Json::array()},
                 {"products", Json::array()}};
    std::sort(action_provenance.begin(), action_provenance.end(),
              [](const auto &left, const auto &right) {
                  return left.at("action_id").template get<std::string>() <
                         right.at("action_id").template get<std::string>();
              });
    for (const auto &action : action_provenance)
        if (std::binary_search(action_ids.begin(), action_ids.end(),
                               action.at("action_id").get<std::string>()))
            receipt["actions"].push_back(action);
    for (const auto &product : source_products)
        receipt["products"].push_back(ProductJson(product));
    WriteBytes(root / receipts_relative / ("compose" + TempSuffix() + ".json"),
               receipt.dump(2) + "\n");
    std::cout << "generation.compose generation_id=" << generation_id
              << " scope=" << scope << '\n';
}

std::vector<fs::path> ReadResultList(const fs::path &root,
                                     const fs::path &list_path)
{
    if (!list_path.is_absolute() || list_path != list_path.lexically_normal())
        Fail("invalid-result-list", "-", "result list must be a canonical absolute path");
    const auto relative = list_path.lexically_relative(root);
    RelativeLocator(relative.generic_string(), "-");
    CheckPath(root, relative, "-");
    const std::string bytes = ReadBytes(list_path);
    if (bytes.empty() || bytes.starts_with("\xef\xbb\xbf"))
        Fail("invalid-result-list", "-", "result list is empty or has a UTF-8 BOM");

    std::vector<fs::path> paths;
    std::set<fs::path> seen;
    std::size_t begin = 0;
    while (begin < bytes.size())
    {
        const auto end = bytes.find('\n', begin);
        const auto line_end = end == std::string::npos ? bytes.size() : end;
        std::string_view line(bytes.data() + begin, line_end - begin);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (line.empty() || std::any_of(line.begin(), line.end(), [](char ch) {
                return static_cast<unsigned char>(ch) < 0x20;
            }))
            Fail("invalid-result-list", "-", "empty or control character in result list");
        const fs::path path = Utf8Path(line);
        if (!path.is_absolute() || path != path.lexically_normal())
            Fail("invalid-result-list", "-", "result path must be canonical and absolute");
        const auto result_relative = path.lexically_relative(root);
        RelativeLocator(result_relative.generic_string(), "-");
        if (!seen.insert(path).second)
            Fail("invalid-result-list", "-", "duplicate result path");
        paths.push_back(path);
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return paths;
}

struct GenerationData
{
    std::string scope;
    std::vector<Product> products;
    int version = 1;
};

GenerationData LoadGeneration(const fs::path &root,
                              const std::string &generation_id,
                              bool allow_legacy_for_gc = false)
{
    if (!IsLowerSha256(generation_id))
        Fail("invalid-generation-id", "-", "invalid Generation ID");
    const auto relative = fs::path("Generations") / generation_id /
                          "generation.json";
    CheckPath(root, relative, "-");
    const auto path = root / relative;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        Fail("missing-manifest", "-", "cannot read " + path.string());
    const std::string bytes(std::istreambuf_iterator<char>(input), {});
    Json manifest;
    try
    {
        manifest = Json::parse(bytes);
    }
    catch (const Json::exception &error)
    {
        Fail("invalid-schema", "-", path.string() + ": " + error.what());
    }
    if (!manifest.is_object() || !manifest.contains("schema_version") ||
        !manifest["schema_version"].is_number_integer())
        Fail("invalid-schema", "-", allow_legacy_for_gc
            ? "missing Generation schema_version" : "expected Generation schema_version=2 or 3");
    const auto schema_version = manifest["schema_version"].get<std::int64_t>();
    const bool legacy = schema_version == 1 && allow_legacy_for_gc;
    if (!legacy && schema_version != 2 && schema_version != 3)
        Fail("invalid-schema", "-", allow_legacy_for_gc
            ? "unsupported GC Generation schema" : "expected Generation schema_version=2 or 3");
    const auto version = static_cast<int>(schema_version);
    const auto scope = RequiredString(manifest, "scope", "-");
    ScopeFileName(scope);
    if (RequiredString(manifest, "generation_id", "-") != generation_id)
        Fail("invalid-manifest", "-", legacy
            ? "legacy manifest Generation ID mismatch" : "manifest Generation ID mismatch");
    const auto products = ParseProducts(manifest, "-", true);
    const auto required_ids = RequiredIds(manifest, products, "-");
    if (legacy)
    {
        for (const auto &product : products)
            ProductNamespace(product);
        RequireDependencyClosure(products);
        if (LegacyGenerationId(scope, required_ids, products) != generation_id ||
            bytes != LegacyManifestBytes(scope, required_ids, products, generation_id))
            Fail("invalid-manifest", "-", "legacy manifest identity/canonical bytes mismatch");
        return {scope, products};
    }
    const auto upstream_dependencies =
        ParseUpstreamDependencies(manifest, "-", true);
    for (const auto &product : products)
        CheckGenerationProduct(product, generation_id, version);
    RequireDependencyClosure(products);
    RequireUpstreamIdentity(products, upstream_dependencies);
    if (GenerationId(scope, required_ids, products, upstream_dependencies, version) !=
            generation_id ||
        bytes !=
            ManifestBytes(scope, required_ids, products,
                          upstream_dependencies, generation_id, version))
        Fail("invalid-manifest", "-", "manifest identity/canonical bytes mismatch");
    return {scope, products, version};
}

std::string LoadPointerId(const fs::path &root, const fs::path &relative,
                          const std::string *expected_scope,
                          bool allow_legacy_for_gc = false,
                          const std::set<std::string> *unpublished_generations = nullptr,
                          const Json *snapshot = nullptr)
{
    CheckPath(root, relative, "-");
    const Json pointer = snapshot ? *snapshot : ReadJson(root / relative, "missing-pointer", "-");
    RequireVersion(pointer, "-");
    const std::string pointer_scope = RequiredString(pointer, "scope", "-");
    if (expected_scope && pointer_scope != *expected_scope)
        Fail("invalid-pointer", "-", "pointer scope mismatch");
    if (relative != fs::path("GenerationState") / ScopeFileName(pointer_scope))
        Fail("invalid-pointer", "-", "pointer filename/scope mismatch");
    const std::string generation_id = RequiredString(pointer, "generation_id", "-");
    const std::string locator = RequiredString(pointer, "manifest_locator", "-");
    if (!IsLowerSha256(generation_id) ||
        locator != "Generations/" + generation_id + "/generation.json")
        Fail("invalid-pointer", "-", "pointer scope, identity, or locator mismatch");
    if (!pointer.contains("action_ids") || !pointer["action_ids"].is_array())
        Fail("invalid-pointer", "-", "missing action_ids");
    std::vector<std::string> pointer_actions;
    for (const auto &entry : pointer["action_ids"])
    {
        if (!entry.is_string() || !IsActionId(entry.get<std::string>()))
            Fail("invalid-pointer", "-", "invalid action ID");
        pointer_actions.push_back(entry.get<std::string>());
    }
    if (pointer_actions.empty() ||
        !std::is_sorted(pointer_actions.begin(), pointer_actions.end()) ||
        std::adjacent_find(pointer_actions.begin(), pointer_actions.end()) !=
            pointer_actions.end())
        Fail("invalid-pointer", "-", "action_ids are not canonical");
    if (pointer_actions.size() == 1)
    {
        if (RequiredString(pointer, "action_id", "-") != pointer_actions.front())
            Fail("invalid-pointer", "-", "single action_id mismatch");
    }
    else if (pointer.contains("action_id"))
        Fail("invalid-pointer", "-", "multi-action pointer has action_id");

    // GC retains a marked unpublished identity without treating its tree as data.
    if (allow_legacy_for_gc && unpublished_generations &&
        unpublished_generations->contains(generation_id))
        return generation_id;
    const auto generation = LoadGeneration(root, generation_id, allow_legacy_for_gc);
    if (generation.scope != pointer_scope ||
        ActionIds(generation.products) != pointer_actions)
        Fail("invalid-pointer", "-", "pointer/manifest mismatch");
    return generation_id;
}

struct SelectedSnapshot
{
    std::string pointer_bytes, id;
    hs::GenerationFileLock lease;
    GenerationData generation;

    SelectedSnapshot(const fs::path &root, const std::string &scope)
    {
        const auto relative = fs::path("GenerationState") / ScopeFileName(scope);
        CheckPath(root, relative, "-");
        if (auto read = hs::ReadGenerationPointer(root / relative, pointer_bytes); !read)
            Fail("missing-pointer", "-", std::string(read.Message()));
        const auto pointer = Json::parse(pointer_bytes);
        id = RequiredString(pointer, "generation_id", "-");
        if (!IsLowerSha256(id) ||
            RequiredString(pointer, "manifest_locator", "-") != "Generations/" + id + "/generation.json")
            Fail("invalid-pointer", "-", "invalid snapshot manifest locator");
        const auto manifest = fs::path("Generations") / id / "generation.json";
        CheckPath(root, manifest, "-");
        // Hold the exact immutable generation while its products are read or
        // copied. Atomic publication may replace the pointer without replacing
        // this snapshot; GC must keep it until the reader lease ends.
        lease = hs::GenerationFileLock(root / manifest, hs::GenerationFileLock::Mode::Shared);
        id = LoadPointerId(root, relative, &scope, false, nullptr, &pointer);
        generation = LoadGeneration(root, id);
    }
};

void Verify(const fs::path &root, const std::string &scope)
{
    const SelectedSnapshot snapshot(root, scope);
    VerifyProducts(root, snapshot.generation.products, "-");
    std::cout << "generation.verify generation_id=" << snapshot.id
              << " scope=" << scope << " products=" << snapshot.generation.products.size() << '\n';
}

void ExportSnapshot(const fs::path &root, const std::string &scope, const std::string &destination_arg)
{
    const auto destination = BuildRoot(destination_arg);
    if (fs::equivalent(root, destination))
        Fail("invalid-destination", "-", "snapshot destination must differ from the source root");
    // Serialize publication and GC at the destination, while the source needs
    // only its selected manifest's shared reader lease.
    hs::GenerationFileLock publication(destination / ".generation-mutation.lock",
        hs::GenerationFileLock::Mode::Exclusive, true);
    const SelectedSnapshot snapshot(root, scope);
    VerifyProducts(root, snapshot.generation.products, "-");
    const auto manifest_relative = fs::path("Generations") / snapshot.id / "generation.json";
    const auto manifest_bytes = ReadBytes(root / manifest_relative);
    const auto pending_relative = fs::path("Generations") / (".pending-" + snapshot.id + ".json");
    CheckPath(destination, pending_relative, "-");
    fs::create_directories((destination / pending_relative).parent_path());
    if (!fs::exists(destination / manifest_relative))
        AtomicReplace(destination / pending_relative, PendingPublicationBytes(snapshot.id, manifest_bytes));
    for (const auto &product : snapshot.generation.products)
    {
        const auto relative = RelativeLocator(product.locator, "-");
        CheckPath(destination, relative, "-");
        const auto target = destination / relative;
        if (fs::exists(target)) continue; // Existing immutable bytes are verified below, never overwritten.
        fs::create_directories(target.parent_path());
        CheckPath(destination, relative, "-");
        const auto temporary = target.parent_path() / TempSuffix();
        try
        {
            fs::copy_file(root / relative, temporary);
            fs::rename(temporary, target);
        }
        catch (...)
        {
            std::error_code ignored; fs::remove(temporary, ignored); throw;
        }
    }
    CheckPath(destination, manifest_relative, "-");
    fs::create_directories((destination / manifest_relative).parent_path());
    if (fs::exists(destination / manifest_relative) && ReadBytes(destination / manifest_relative) != manifest_bytes)
        Fail("corrupt-manifest", "-", "destination contains conflicting immutable manifest bytes");
    AtomicReplace(destination / manifest_relative, manifest_bytes);
    const auto relocated = LoadGeneration(destination, snapshot.id);
    VerifyProducts(destination, relocated.products, "-");
    std::error_code marker_error;
    fs::remove(destination / pending_relative, marker_error);
    const auto pointer_relative = fs::path("GenerationState") / ScopeFileName(scope);
    CheckPath(destination, pointer_relative, "-");
    fs::create_directories((destination / pointer_relative).parent_path());
    AtomicReplace(destination / pointer_relative, snapshot.pointer_bytes);
    std::cout << "generation.export generation_id=" << snapshot.id << " scope=" << scope
              << " products=" << relocated.products.size() << " destination=" << destination.string() << '\n';
}

void CheckTree(const fs::path &root, const fs::path &directory)
{
    CheckPath(root, directory.lexically_relative(root), "-");
    if (!fs::is_directory(directory))
        Fail("path-error", "-", "expected generated directory: " + directory.string());
    for (const auto &entry : fs::recursive_directory_iterator(directory))
        CheckPath(root, entry.path().lexically_relative(root), "-");
}

bool PastGracePeriod(const fs::path &path, std::uint64_t grace_seconds)
{
    if (grace_seconds == 0)
        return true;
    return fs::last_write_time(path) <=
           fs::file_time_type::clock::now() -
               std::chrono::seconds(static_cast<std::int64_t>(grace_seconds));
}

struct RetiredGeneration
{
    fs::path path;
    hs::GenerationFileLock lock;
    fs::path pending_marker;
    std::string reason;
};

struct RetiredProductSet
{
    fs::path path;
    fs::path lease_path;
    hs::GenerationFileLock reader_lock;
    bool had_lease_file{};
};

struct GarbageCollectionPlan
{
    std::vector<RetiredGeneration> obsolete_generations;
    std::vector<fs::path> stale_retirements;
    std::vector<fs::path> stale_publications;
    std::vector<RetiredProductSet> obsolete_sets;
    std::map<std::string, std::set<std::string>> retained_generation_reasons;
};

std::uint64_t TreeBytes(const fs::path &root, const fs::path &path)
{
    std::uint64_t bytes = 0;
    for (const auto &entry : fs::recursive_directory_iterator(path))
    {
        CheckPath(root, entry.path().lexically_relative(root), "-");
        std::error_code error;
        if (!entry.is_regular_file(error))
        {
            if (error)
                Fail("path-error", "-", "cannot inspect candidate: " + entry.path().string());
            continue;
        }
        const auto size = entry.file_size(error);
        if (error || size > std::numeric_limits<std::uint64_t>::max() - bytes)
            Fail("path-error", "-", "candidate size cannot be estimated: " + entry.path().string());
        bytes += size;
    }
    return bytes;
}

std::uint64_t FileBytes(const fs::path &root, const fs::path &path)
{
    CheckPath(root, path.lexically_relative(root), "-");
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error)
        Fail("path-error", "-", "cannot estimate metadata candidate: " + path.string());
    const auto size = fs::file_size(path, error);
    if (error)
        Fail("path-error", "-", "cannot estimate metadata candidate: " + path.string());
    return size;
}

void PrintGarbageCollectionDryRun(
    const fs::path &root, std::uint64_t grace_seconds,
    const GarbageCollectionPlan &plan, bool json)
{
    std::vector<std::uint64_t> generation_sizes;
    std::vector<std::uint64_t> product_set_sizes;
    std::vector<std::uint64_t> metadata_sizes;
    std::uint64_t generation_bytes = 0;
    std::uint64_t product_set_bytes = 0;
    std::uint64_t metadata_bytes = 0;
    const auto add_size = [](std::uint64_t &total, std::uint64_t size) {
        if (size > std::numeric_limits<std::uint64_t>::max() - total)
            Fail("path-error", "-", "candidate size total overflow");
        total += size;
    };
    for (const auto &candidate : plan.obsolete_generations)
    {
        const auto size = TreeBytes(root, candidate.path);
        generation_sizes.push_back(size);
        add_size(generation_bytes, size);
    }
    for (const auto &candidate : plan.obsolete_sets)
    {
        const auto size = TreeBytes(root, candidate.path);
        product_set_sizes.push_back(size);
        add_size(product_set_bytes, size);
    }
    for (const auto &candidate : plan.stale_retirements)
    {
        const auto size = FileBytes(root, candidate);
        metadata_sizes.push_back(size);
        add_size(metadata_bytes, size);
    }
    for (const auto &candidate : plan.stale_publications)
    {
        const auto size = FileBytes(root, candidate);
        metadata_sizes.push_back(size);
        add_size(metadata_bytes, size);
    }

    if (json)
    {
        Json report{{"schema_version", 1},
                    {"command", "gc"},
                    {"dry_run", true},
                    {"build_root", root.generic_string()},
                    {"grace_seconds", grace_seconds},
                    {"candidates", Json{{"generations", Json::array()},
                                         {"product_sets", Json::array()},
                                         {"metadata_cleanup", Json::array()}}},
                    {"retained", Json::array()},
                    {"totals", Json{{"generation_count", plan.obsolete_generations.size()},
                                     {"generation_bytes", generation_bytes},
                                     {"product_set_count", plan.obsolete_sets.size()},
                                     {"product_set_bytes", product_set_bytes},
                                     {"metadata_cleanup_count", metadata_sizes.size()},
                                     {"metadata_cleanup_bytes", metadata_bytes}}}};
        for (std::size_t index = 0; index < plan.obsolete_generations.size(); ++index)
        {
            const auto &candidate = plan.obsolete_generations[index];
            report["candidates"]["generations"].push_back(
                Json{{"locator", candidate.path.lexically_relative(root).generic_string()},
                     {"estimated_bytes", generation_sizes[index]},
                     {"reason", candidate.reason}});
        }
        for (std::size_t index = 0; index < plan.obsolete_sets.size(); ++index)
        {
            const auto &candidate = plan.obsolete_sets[index];
            report["candidates"]["product_sets"].push_back(
                Json{{"locator", candidate.path.lexically_relative(root).generic_string()},
                     {"estimated_bytes", product_set_sizes[index]},
                     {"reason", "unreferenced ProductSet past grace period"}});
        }
        std::size_t metadata_index = 0;
        for (const auto &candidate : plan.stale_retirements)
        {
            report["candidates"]["metadata_cleanup"].push_back(
                Json{{"locator", candidate.lexically_relative(root).generic_string()},
                     {"estimated_bytes", metadata_sizes[metadata_index++]},
                     {"reason", "retired generation metadata cleanup"}});
        }
        for (const auto &candidate : plan.stale_publications)
        {
            report["candidates"]["metadata_cleanup"].push_back(
                Json{{"locator", candidate.lexically_relative(root).generic_string()},
                     {"estimated_bytes", metadata_sizes[metadata_index++]},
                     {"reason", "stale pending publication metadata cleanup"}});
        }
        for (const auto &[identity, reasons] : plan.retained_generation_reasons)
        {
            Json reason_list = Json::array();
            for (const auto &reason : reasons)
                reason_list.push_back(reason);
            report["retained"].push_back(
                Json{{"kind", "generation"}, {"identity", identity},
                     {"reasons", std::move(reason_list)}});
        }
        std::cout << report.dump(2) << '\n';
        return;
    }

    std::cout << "generation.gc dry_run=true build_root=" << root.generic_string()
              << " grace_seconds=" << grace_seconds << '\n';
    for (std::size_t index = 0; index < plan.obsolete_generations.size(); ++index)
    {
        const auto &candidate = plan.obsolete_generations[index];
        std::cout << "generation.gc candidate_generation locator="
                  << candidate.path.lexically_relative(root).generic_string()
                  << " estimated_bytes=" << generation_sizes[index]
                  << " reason=" << candidate.reason << '\n';
    }
    for (std::size_t index = 0; index < plan.obsolete_sets.size(); ++index)
    {
        const auto &candidate = plan.obsolete_sets[index];
        std::cout << "generation.gc candidate_product_set locator="
                  << candidate.path.lexically_relative(root).generic_string()
                  << " estimated_bytes=" << product_set_sizes[index]
                  << " reason=unreferenced ProductSet past grace period\n";
    }
    std::size_t metadata_index = 0;
    for (const auto &candidate : plan.stale_retirements)
        std::cout << "generation.gc candidate_metadata locator="
                  << candidate.lexically_relative(root).generic_string()
                  << " estimated_bytes=" << metadata_sizes[metadata_index++]
                  << " reason=retired generation metadata cleanup\n";
    for (const auto &candidate : plan.stale_publications)
        std::cout << "generation.gc candidate_metadata locator="
                  << candidate.lexically_relative(root).generic_string()
                  << " estimated_bytes=" << metadata_sizes[metadata_index++]
                  << " reason=stale pending publication metadata cleanup\n";
    for (const auto &[identity, reasons] : plan.retained_generation_reasons)
    {
        std::cout << "generation.gc retained_generation identity=" << identity
                  << " reasons=";
        bool first = true;
        for (const auto &reason : reasons)
        {
            if (!first)
                std::cout << ',';
            first = false;
            std::cout << reason;
        }
        std::cout << '\n';
    }
    std::cout << "generation.gc totals generations=" << plan.obsolete_generations.size()
              << " generation_bytes=" << generation_bytes
              << " product_sets=" << plan.obsolete_sets.size()
              << " product_set_bytes=" << product_set_bytes
              << " metadata_cleanup=" << metadata_sizes.size()
              << " metadata_cleanup_bytes=" << metadata_bytes << '\n';
}

void GarbageCollect(const fs::path &root, const std::vector<std::string> &pins,
                    std::uint64_t grace_seconds, bool dry_run, bool json)
{
    using ProductSet = std::pair<std::string, std::string>;
    std::set<ProductSet> retained_sets;
    std::set<std::string> known_namespaces;
    std::set<std::string> retained_generations(pins.begin(), pins.end());
    GarbageCollectionPlan plan;
    for (const auto &pin : pins)
        plan.retained_generation_reasons[pin].insert("pin");
    const auto generations = root / "Generations";
    CheckPath(root, "Generations", "-");
    std::vector<PendingPublication> pending_publications;
    std::set<std::string> unpublished_generations;
    if (fs::exists(generations))
        for (const auto &entry : fs::directory_iterator(generations))
        {
            const auto name = entry.path().filename().string();
            if (!name.starts_with(".pending-") || !name.ends_with(".json"))
                continue;
            auto publication = LoadPendingPublication(root, entry.path());
            const auto manifest = generations / publication.generation_id / "generation.json";
            CheckPath(root, manifest.lexically_relative(root), "-");
            if (!fs::exists(manifest))
                unpublished_generations.insert(publication.generation_id);
            pending_publications.push_back(std::move(publication));
        }
    const auto retain_generation = [&](const std::string &id) {
        if (unpublished_generations.contains(id))
            return;
        const auto generation = LoadGeneration(root, id, true);
        VerifyProducts(root, generation.products, "-");
        for (const auto &product : generation.products)
        {
            known_namespaces.insert(product.namespace_path);
            if (generation.version != 3)
                retained_sets.insert(ProductNamespace(product));
        }
    };

    const auto actions = root / "Actions";
    CheckPath(root, "Actions", "-");
    if (fs::exists(actions))
    {
        std::set<std::string> current_action_ids;
        for (const auto &entry : fs::recursive_directory_iterator(actions))
        {
            CheckPath(root, entry.path().lexically_relative(root), "-");
            if (!entry.is_regular_file() ||
                entry.path().filename() != "result.json")
                continue;
            const auto relative_namespace =
                entry.path().parent_path().lexically_relative(actions);
            const auto expected_namespace = relative_namespace.generic_string();
            NamespacePath(expected_namespace, "-");
            const auto result = ReadJson(entry.path(),
                                         "missing-action-result", "-");
            const auto action_id = RequiredString(result, "action_id", "-");
            if (!IsActionId(action_id) ||
                ResultNamespace(result, action_id) != expected_namespace)
                Fail("invalid-action-result", action_id, "ActionResult namespace mismatch");
            if (!current_action_ids.insert(action_id).second)
                Fail("duplicate-action", action_id, "duplicate current ActionResult");
            RequireVersion(result, action_id);
            const auto product_set_id = RequiredString(result, "product_set_id", action_id);
            if (!IsLowerSha256(product_set_id))
                Fail("invalid-schema", action_id, "invalid ProductSet ID");
            const auto products = ParseProducts(result, action_id, false,
                                                expected_namespace,
                                                product_set_id);
            for (const auto &product : products)
                CheckProductNamespace(product);
            retained_sets.emplace(expected_namespace, product_set_id);
            known_namespaces.insert(expected_namespace);
        }
    }

    const auto state = root / "GenerationState";
    CheckPath(root, "GenerationState", "-");
    if (fs::exists(state))
    {
        for (const auto &entry : fs::directory_iterator(state))
        {
            const auto relative = entry.path().lexically_relative(root);
            CheckPath(root, relative, "-");
            if (!entry.is_regular_file() || entry.path().extension() != ".json")
                continue;
            const auto id = LoadPointerId(root, relative, nullptr, true,
                                          &unpublished_generations);
            retained_generations.insert(id);
            plan.retained_generation_reasons[id].insert(
                "current pointer:" + relative.generic_string());
        }
    }
    for (const auto &id : retained_generations)
        retain_generation(id);

    std::set<std::string> retired_ids;
    for (const auto &publication : pending_publications)
    {
        const auto tree = generations / publication.generation_id;
        if (!unpublished_generations.contains(publication.generation_id))
        {
            // Validate the committed manifest before removing its stale marker.
            LoadGeneration(root, publication.generation_id, true);
            plan.stale_publications.push_back(publication.path);
        }
        else if (!retained_generations.contains(publication.generation_id) &&
                 PastGracePeriod(publication.path, grace_seconds) && !fs::exists(tree))
            plan.stale_publications.push_back(publication.path);
    }
    if (fs::exists(generations))
    {
        for (const auto &entry : fs::directory_iterator(generations))
        {
            if (!entry.path().filename().string().starts_with(".gc-") || !entry.is_regular_file())
                continue;
            CheckPath(root, entry.path().lexically_relative(root), "-");
            const auto manifest = ReadJson(entry.path(), "missing-manifest", "-");
            const auto id = RequiredString(manifest, "generation_id", "-");
            if (!IsLowerSha256(id) || !entry.path().filename().string().starts_with(".gc-" + id + ".tmp-"))
                Fail("invalid-manifest", "-", "invalid retired Generation identity");
            for (const auto &product : ParseProducts(manifest, "-", true))
                known_namespaces.insert(product.namespace_path);
            retired_ids.insert(id);
            plan.stale_retirements.push_back(entry.path());
        }
        for (const auto &entry : fs::directory_iterator(generations))
        {
            const auto relative = entry.path().lexically_relative(root);
            CheckPath(root, relative, "-");
            const auto id = entry.path().filename().string();
            if (!IsLowerSha256(id) || !entry.is_directory())
                continue;
            if (unpublished_generations.contains(id))
            {
                const auto pending = generations / (".pending-" + id + ".json");
                if (dry_run) CheckTree(root, entry.path());
                if (!retained_generations.contains(id) &&
                    PastGracePeriod(entry.path(), grace_seconds) &&
                    PastGracePeriod(pending, grace_seconds))
                    plan.obsolete_generations.push_back(
                        {entry.path(), {}, pending,
                         "unpublished pending generation past grace period"});
                else if (retained_generations.contains(id))
                    plan.retained_generation_reasons[id].insert("pending publication");
                else
                {
                    plan.retained_generation_reasons[id].insert("pending publication");
                    plan.retained_generation_reasons[id].insert("grace period");
                }
                continue;
            }
            if (retired_ids.contains(id) && !fs::exists(entry.path() / "generation.json"))
            {
                if (dry_run) CheckTree(root, entry.path());
                plan.obsolete_generations.push_back(
                    {entry.path(), {}, {}, "retired generation cleanup"});
                continue;
            }
            const auto generation = LoadGeneration(root, id, true);
            const bool explicitly_retained = retained_generations.contains(id);
            bool keep = explicitly_retained || !PastGracePeriod(entry.path(), grace_seconds);
            if (keep && !explicitly_retained)
                plan.retained_generation_reasons[id].insert("grace period");
            hs::GenerationFileLock reader_lock;
            if (!keep)
            {
                reader_lock = hs::GenerationFileLock(entry.path() / "generation.json",
                    hs::GenerationFileLock::Mode::TryExclusive);
                keep = !reader_lock.OwnsLock();
                if (keep)
                    plan.retained_generation_reasons[id].insert("reader lease");
            }
            // Explicit roots were already verified under this same mutation lock.
            if (keep && !explicitly_retained)
                VerifyProducts(root, generation.products, "-");
            for (const auto &product : generation.products)
            {
                known_namespaces.insert(product.namespace_path);
                if (keep && generation.version != 3)
                    retained_sets.insert(ProductNamespace(product));
            }
            if (keep)
                continue;
            if (dry_run) CheckTree(root, entry.path());
            plan.obsolete_generations.push_back(
                {entry.path(), std::move(reader_lock), {},
                 "unreferenced generation past grace period"});
        }
    }

    const auto products_root = root / "Products";
    CheckPath(root, "Products", "-");
    if (fs::exists(products_root))
    {
        for (auto iterator = fs::recursive_directory_iterator(products_root);
             iterator != fs::recursive_directory_iterator(); ++iterator)
        {
            const auto &entry = *iterator;
            CheckPath(root, entry.path().lexically_relative(root), "-");
            if (!entry.is_directory())
                continue;
            const auto set_id = entry.path().filename().string();
            if (!IsLowerSha256(set_id))
                continue;
            const auto namespace_relative =
                entry.path().parent_path().lexically_relative(products_root);
            const auto namespace_path = namespace_relative.generic_string();
            NamespacePath(namespace_path, "-");
            if (!known_namespaces.contains(namespace_path))
                continue;
            bool contains_file = false;
            bool contains_directory = false;
            for (const auto &child : fs::directory_iterator(entry.path()))
            {
                CheckPath(root, child.path().lexically_relative(root), "-");
                contains_file |= child.is_regular_file();
                contains_directory |= child.is_directory();
            }
            if (contains_directory || !contains_file)
                continue;
            iterator.disable_recursion_pending();
            if (retained_sets.contains({namespace_path, set_id}) ||
                !PastGracePeriod(entry.path(), grace_seconds))
                continue;
            if (dry_run) CheckTree(root, entry.path());
            const auto lease_path = entry.path().parent_path() / ".leases" /
                (set_id + ".lock");
            RetiredProductSet candidate{entry.path(), lease_path, {}, fs::exists(lease_path)};
            bool reader_active = false;
            if (candidate.had_lease_file)
            {
                candidate.reader_lock = hs::GenerationFileLock(lease_path,
                    hs::GenerationFileLock::Mode::TryExclusive);
                if (!candidate.reader_lock.OwnsLock())
                {
                    reader_active = true;
                }
            }
            if (!reader_active)
                plan.obsolete_sets.push_back(std::move(candidate));
        }
    }

    if (dry_run)
    {
        PrintGarbageCollectionDryRun(root, grace_seconds, plan, json);
        return;
    }

    // Each independent tree is checked at deletion; dry-run checks during planning.
    for (auto &candidate : plan.obsolete_generations)
    {
        CheckTree(root, candidate.path);
        if (candidate.lock.OwnsLock())
        {
            const auto retired = generations / (".gc-" + candidate.path.filename().string() + TempSuffix() + ".json");
            // Windows permits renaming the shared-delete file, not its open parent.
            fs::rename(candidate.path / "generation.json", retired);
            candidate.lock = {};
            plan.stale_retirements.push_back(retired);
        }
        fs::remove_all(candidate.path);
        if (!candidate.pending_marker.empty())
            fs::remove(candidate.pending_marker);
    }
    for (auto &candidate : plan.obsolete_sets)
    {
        CheckTree(root, candidate.path);
        fs::remove_all(candidate.path);
        if (candidate.had_lease_file)
            fs::remove(candidate.lease_path);
        candidate.reader_lock = {};
        if (candidate.had_lease_file)
        {
            std::error_code ignored;
            fs::remove(candidate.lease_path.parent_path(), ignored);
        }
    }
    // Retired manifests retain namespace discovery until ProductSet sweep completes.
    for (const auto &path : plan.stale_retirements)
        fs::remove_all(path);
    for (const auto &path : plan.stale_publications)
        fs::remove(path);
    std::cout << "generation.gc removed_generations=" << plan.obsolete_generations.size()
              << " removed_product_sets=" << plan.obsolete_sets.size() << '\n';
}

} // namespace

int main(int argc, char **argv)
{
    bool compose = false;
    bool verify = false;
    bool export_snapshot = false;
    bool gc = false;
    bool dry_run = false;
    bool json = false;
    std::string root_arg;
    std::string scope;
    std::string destination_arg;
    std::vector<fs::path> result_paths;
    std::string result_list_arg;
    bool result_list_supplied = false;
    std::vector<std::string> required_ids;
    std::vector<std::string> pins;
    std::uint64_t grace_seconds = 24 * 60 * 60;
    bool grace_supplied = false;
    const auto report_failure = [&](std::string_view error_code, std::string_view action_id,
                                    std::string_view reason) {
        Json results = Json::array();
        for (const auto &path : result_paths)
            results.push_back(DiagnosticPath(path));
        const Json diagnostic{
            {"schema_version", 1}, {"error_code", error_code},
            {"producer_id", "hs_generation"}, {"action_id", action_id}, {"reason", reason},
            {"build_root", root_arg}, {"scope", scope},
            {"compose", compose}, {"verify", verify}, {"export_snapshot", export_snapshot},
            {"gc", gc}, {"destination", destination_arg},
            {"dependency", {{"results", results}, {"result_list", result_list_arg},
                            {"required_products", required_ids}}},
            {"remediation", "Check the reported Generation inputs and dependencies, correct the failure, and rerun the command."}};
        std::cerr << diagnostic.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
    };
    try
    {
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument(argv[index]);
            if (argument == "--compose")
            {
                compose = true;
            }
            else if (argument == "--verify")
            {
                verify = true;
            }
            else if (argument == "--export-snapshot")
            {
                export_snapshot = true;
            }
            else if (argument == "--gc")
            {
                gc = true;
            }
            else if (argument == "--dry-run")
            {
                dry_run = true;
            }
            else if (argument == "--json")
            {
                json = true;
            }
            else if ((argument == "--build-root" || argument == "--scope" ||
                      argument == "--result" || argument == "--result-list" ||
                      argument == "--require" ||
                      argument == "--pin" || argument == "--grace-seconds" || argument == "--destination") &&
                     index + 1 < argc)
            {
                const std::string value = argv[++index];
                if (argument == "--build-root")
                {
                    root_arg = value;
                }
                else if (argument == "--scope")
                {
                    scope = value;
                }
                else if (argument == "--destination")
                {
                    if (!destination_arg.empty() || value.empty())
                        Fail("invalid-arguments", "-", "destination must be supplied once");
                    destination_arg = value;
                }
                else if (argument == "--result")
                {
                    result_paths.push_back(Utf8Path(value));
                }
                else if (argument == "--result-list")
                {
                    if (result_list_supplied || value.empty())
                        Fail("invalid-arguments", "-", "result list must be supplied once");
                    result_list_arg = value;
                    result_list_supplied = true;
                }
                else if (argument == "--require")
                {
                    if (value.empty())
                        Fail("invalid-arguments", "-", "required logical ID is empty");
                    required_ids.push_back(value);
                }
                else if (argument == "--pin")
                {
                    if (!IsLowerSha256(value))
                        Fail("invalid-arguments", "-", "pin must be a Generation ID");
                    pins.push_back(value);
                }
                else
                {
                    const auto *begin = value.data();
                    const auto *end = begin + value.size();
                    const auto parsed = std::from_chars(begin, end, grace_seconds);
                    if (parsed.ec != std::errc{} || parsed.ptr != end ||
                        grace_seconds > static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max()))
                        Fail("invalid-arguments", "-", "invalid grace seconds");
                    grace_supplied = true;
                }
            }
            else
            {
                Fail("invalid-arguments", "-", "unknown or incomplete argument");
            }
        }
        if (static_cast<int>(compose) + static_cast<int>(verify) +
                static_cast<int>(gc) + static_cast<int>(export_snapshot) != 1 ||
            root_arg.empty() ||
            (export_snapshot != !destination_arg.empty()) ||
            (gc && (!scope.empty() || !result_paths.empty() ||
                    result_list_supplied || !required_ids.empty())) ||
            (!gc && (scope.empty() || !pins.empty() || grace_supplied)) ||
            (!gc && (dry_run || json)) ||
            (json && !dry_run) ||
            (compose && (result_paths.empty() == !result_list_supplied)) ||
            ((verify || export_snapshot) && (!result_paths.empty() || result_list_supplied ||
                        !required_ids.empty())))
        {
            Fail("invalid-arguments", "-",
                 "usage: hs_generation --compose --build-root <absolute> --scope <scope> "
                 "(--result <absolute>... | --result-list <absolute-file>) "
                 "[--require <logical-id>...] | "
                 "--verify --build-root <absolute> --scope <scope> | "
                 "--export-snapshot --build-root <absolute> --scope <scope> --destination <existing-absolute> | "
                 "--gc --build-root <absolute> [--pin <generation-id>...] "
                 "[--grace-seconds <n>] [--dry-run [--json]]");
        }
        if (!gc)
            ScopeFileName(scope);
        const fs::path root = BuildRoot(root_arg);
        if (compose)
        {
            hs::GenerationFileLock mutation(root / ".generation-mutation.lock",
                hs::GenerationFileLock::Mode::Exclusive, true);
            if (result_list_supplied)
                result_paths = ReadResultList(root, Utf8Path(result_list_arg));
            Compose(root, scope, result_paths, required_ids);
        }
        else if (verify)
        {
            Verify(root, scope);
        }
        else if (export_snapshot)
        {
            ExportSnapshot(root, scope, destination_arg);
        }
        else
        {
            hs::GenerationFileLock mutation(root / ".generation-mutation.lock",
                hs::GenerationFileLock::Mode::Exclusive, true);
            GarbageCollect(root, pins, grace_seconds, dry_run, json);
        }
        return 0;
    }
    catch (const Failure &failure)
    {
        report_failure(failure.code, failure.action_id, failure.what());
    }
    catch (const std::exception &error)
    {
        report_failure("io-error", "-", error.what());
    }
    return 1;
}
