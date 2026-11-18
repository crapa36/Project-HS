#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>
#include <hs/asset_manifest/generation_lock.hpp>
#include <hs/core/sha256.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
                        from_manifest ? RequiredString(entry, "product_set_id", action_id)
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

Json BuildManifest(const std::string &scope,
                   const std::vector<Product> &products,
                   const std::vector<UpstreamProductDependency> &dependencies)
{
    Json manifest{{"schema_version", 5}, {"scope", scope},
                  {"products", Json::array()},
                  {"upstream_product_dependencies", Json::array()}};
    for (const Product &product : products)
        manifest["products"].push_back(ProductJson(product));
    for (const auto &dependency : dependencies)
        manifest["upstream_product_dependencies"].push_back(
            UpstreamDependencyJson(dependency));
    const auto id = HashBytes(manifest.dump());
    manifest["generation_id"] = id;
    return manifest;
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

fs::path ManifestRelativePath(const std::string &scope)
{
    return fs::path("Generations") / "current" / ScopeFileName(scope);
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
                    const std::string &action_id)
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
            }
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
    if (fs::exists(destination) && ReadBytes(destination) == bytes)
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

void Compose(const fs::path &root, const std::string &scope,
             const std::vector<fs::path> &result_paths)
{
    std::vector<Product> products;
    std::vector<UpstreamProductDependency> upstream_dependencies;
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
        const auto product_set_id = RequiredString(result, "product_set_id", action_id);
        if (!IsLowerSha256(product_set_id))
            Fail("invalid-schema", action_id, "invalid ProductSet ID");
        auto action_products = ParseProducts(result, action_id, false,
                                             namespace_path, product_set_id);
        for (const auto &product : action_products)
            CheckProductNamespace(product);
        products.insert(products.end(),
                        std::make_move_iterator(action_products.begin()),
                        std::make_move_iterator(action_products.end()));
        auto dependencies = ParseUpstreamDependencies(result, action_id);
        upstream_dependencies.insert(upstream_dependencies.end(),
                                     std::make_move_iterator(dependencies.begin()),
                                     std::make_move_iterator(dependencies.end()));
    }
    std::sort(products.begin(), products.end(),
              [](const Product &left, const Product &right) {
                  return left.logical_id < right.logical_id;
              });
    for (std::size_t index = 1; index < products.size(); ++index)
        if (products[index - 1].logical_id == products[index].logical_id)
            Fail("duplicate-logical-id", "-",
                 "duplicate logical ID: " + products[index].logical_id);
    RequireDependencyClosure(products);
    std::sort(upstream_dependencies.begin(), upstream_dependencies.end(),
               [](const auto &left, const auto &right) {
                   return std::tie(left.action_id, left.logical_id) <
                          std::tie(right.action_id, right.logical_id);
               });
    RequireUpstreamIdentity(products, upstream_dependencies);

    const Json manifest = BuildManifest(scope, products, upstream_dependencies);
    const std::string generation_id = manifest["generation_id"].get<std::string>();
    const std::string manifest_bytes = manifest.dump(2) + "\n";
    VerifyProducts(root, products, "-");
    std::set<std::pair<std::string, std::string>> product_sets;
    for (const auto &product : products)
        product_sets.insert(ProductNamespace(product));
    for (const auto &[name_space, product_set_id] : product_sets)
    {
        const auto lease_directory = fs::path("Products") / name_space / ".leases";
        CheckPath(root, lease_directory, "-");
        fs::create_directories(root / lease_directory);
        CheckPath(root, lease_directory, "-");
        const auto lease_path = lease_directory / (product_set_id + ".lock");
        CheckPath(root, lease_path, "-");
        const hs::GenerationFileLock lease(root / lease_path,
            hs::GenerationFileLock::Mode::Shared, true);
    }

    const auto manifest_relative = ManifestRelativePath(scope);
    CheckPath(root, manifest_relative, "-");
    fs::create_directories((root / manifest_relative).parent_path());
    CheckPath(root, manifest_relative, "-");
    AtomicReplace(root / manifest_relative, manifest_bytes);
    std::cout << "generation.compose generation_id=" << generation_id
              << " scope=" << scope << '\n';
}

void AppendResultList(const fs::path &root, const fs::path &list_path,
                      std::vector<fs::path> &paths)
{
    if (!list_path.is_absolute() || list_path != list_path.lexically_normal())
        Fail("invalid-result-list", "-", "result list must be a canonical absolute path");
    const auto relative = list_path.lexically_relative(root);
    RelativeLocator(relative.generic_string(), "-");
    CheckPath(root, relative, "-");
    const std::string bytes = ReadBytes(list_path);
    if (bytes.empty() || bytes.starts_with("\xef\xbb\xbf"))
        Fail("invalid-result-list", "-", "result list is empty or has a UTF-8 BOM");

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
}

struct GenerationData
{
    std::string id;
    std::vector<Product> products;
};

GenerationData LoadGeneration(const fs::path &root,
                              const std::string &expected_scope)
{
    const auto relative = ManifestRelativePath(expected_scope);
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
        !manifest["schema_version"].is_number_integer() ||
        manifest["schema_version"].get<std::int64_t>() != 5)
        Fail("invalid-schema", "-", "expected Generation schema_version=5");
    const auto scope = RequiredString(manifest, "scope", "-");
    if (ScopeFileName(scope) != relative.filename().string())
        Fail("invalid-manifest", "-", "manifest scope/path mismatch");
    if (scope != expected_scope)
        Fail("invalid-manifest", "-", "manifest scope mismatch");
    const auto generation_id = RequiredString(manifest, "generation_id", "-");
    if (!IsLowerSha256(generation_id))
        Fail("invalid-generation-id", "-", "invalid Generation ID");
    const auto products = ParseProducts(manifest, "-", true);
    const auto upstream_dependencies =
        ParseUpstreamDependencies(manifest, "-", true);
    for (const auto &product : products)
        CheckProductNamespace(product);
    RequireDependencyClosure(products);
    RequireUpstreamIdentity(products, upstream_dependencies);
    const auto canonical_manifest = BuildManifest(scope, products, upstream_dependencies);
    if (canonical_manifest["generation_id"].get<std::string>() != generation_id ||
        bytes != canonical_manifest.dump(2) + "\n")
        Fail("invalid-manifest", "-", "manifest identity/canonical bytes mismatch");
    return {generation_id, products};
}

void Verify(const fs::path &root, const std::string &scope)
{
    hs::GenerationFileLock read_lock(root / ".generation-mutation.lock",
        hs::GenerationFileLock::Mode::Shared, true);
    const auto generation = LoadGeneration(root, scope);
    VerifyProducts(root, generation.products, "-");
    std::cout << "generation.verify generation_id=" << generation.id
              << " scope=" << scope << " products=" << generation.products.size() << '\n';
}

void CheckTree(const fs::path &root, const fs::path &directory)
{
    CheckPath(root, directory.lexically_relative(root), "-");
    if (!fs::is_directory(directory))
        Fail("path-error", "-", "expected generated directory: " + directory.string());
    for (const auto &entry : fs::recursive_directory_iterator(directory))
        CheckPath(root, entry.path().lexically_relative(root), "-");
}

struct RetiredProductSet
{
    fs::path path;
    fs::path lease_path;
    hs::GenerationFileLock lock;
    bool had_lease_file{};
    bool remove{};
};

void GarbageCollect(const fs::path &root)
{
    using ProductSet = std::pair<std::string, std::string>;
    std::set<ProductSet> retained_sets;
    const auto retain_products = [&](const std::vector<Product> &products) {
        for (const auto &product : products)
        {
            const auto identity = ProductNamespace(product);
            retained_sets.insert(identity);
        }
    };

    const auto actions = root / "Actions";
    CheckPath(root, "Actions", "-");
    if (fs::exists(actions))
    {
        std::set<std::string> action_ids;
        for (const auto &entry : fs::recursive_directory_iterator(actions))
        {
            const auto relative = entry.path().lexically_relative(root);
            CheckPath(root, relative, "-");
            if (!entry.is_regular_file() || entry.path().filename() != "result.json")
                continue;
            const auto namespace_path =
                entry.path().parent_path().lexically_relative(actions).generic_string();
            NamespacePath(namespace_path, "-");
            const auto result = ReadJson(entry.path(), "missing-action-result", "-");
            const auto action_id = RequiredString(result, "action_id", "-");
            if (!IsActionId(action_id) || ResultNamespace(result, action_id) != namespace_path ||
                entry.path() != actions / NamespacePath(namespace_path, action_id) / "result.json")
                Fail("invalid-action-result", action_id, "ActionResult path/namespace mismatch");
            if (!action_ids.insert(action_id).second)
                Fail("duplicate-action", action_id, "duplicate current ActionResult");
            RequireVersion(result, action_id);
            const auto product_set_id = RequiredString(result, "product_set_id", action_id);
            auto products = ParseProducts(result, action_id, false,
                                          namespace_path, product_set_id);
            for (const auto &product : products)
                CheckProductNamespace(product);
            retain_products(products);
        }
    }

    const auto current = root / "Generations" / "current";
    CheckPath(root, "Generations", "-");
    CheckPath(root, fs::path("Generations") / "current", "-");
    if (fs::exists(current))
    {
        for (const auto &entry : fs::directory_iterator(current))
        {
            CheckPath(root, entry.path().lexically_relative(root), "-");
            if (!entry.is_regular_file() || entry.path().extension() != ".json")
                continue;
            const auto manifest = ReadJson(entry.path(), "missing-manifest", "-");
            const auto scope = RequiredString(manifest, "scope", "-");
            if (entry.path().filename().string() != ScopeFileName(scope))
                Fail("invalid-manifest", "-", "current manifest scope/path mismatch");
            const auto generation = LoadGeneration(root, scope);
            retain_products(generation.products);
        }
    }

    std::vector<RetiredProductSet> candidates;
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
            if (entry.path().filename() == ".leases")
            {
                iterator.disable_recursion_pending();
                continue;
            }
            const auto product_set_id = entry.path().filename().string();
            if (!IsLowerSha256(product_set_id))
                continue;
            const auto name_space = entry.path().parent_path()
                .lexically_relative(products_root).generic_string();
            NamespacePath(name_space, "-");
            iterator.disable_recursion_pending();
            if (retained_sets.contains({name_space, product_set_id}))
                continue;
            const auto lease_directory = entry.path().parent_path() / ".leases";
            const auto lease_path = lease_directory / (product_set_id + ".lock");
            CheckPath(root, lease_directory.lexically_relative(root), "-");
            CheckPath(root, lease_path.lexically_relative(root), "-");
            candidates.push_back({entry.path(), lease_path, {}, fs::exists(lease_path), false});
        }
    }

    for (auto &candidate : candidates)
    {
        if (candidate.had_lease_file)
        {
            candidate.lock = hs::GenerationFileLock(
                candidate.lease_path, hs::GenerationFileLock::Mode::TryExclusive);
            candidate.remove = candidate.lock.OwnsLock();
        }
        else
            candidate.remove = true;
    }
    // Validate the complete deletion set before removing any ProductSet.
    for (const auto &candidate : candidates)
        if (candidate.remove)
        {
            CheckTree(root, candidate.path);
            CheckPath(root, candidate.lease_path.lexically_relative(root), "-");
        }

    std::size_t removed = 0;
    for (auto &candidate : candidates)
        if (candidate.remove)
        {
            fs::remove_all(candidate.path);
            if (candidate.had_lease_file)
            {
                candidate.lock = {};
                fs::remove(candidate.lease_path);
                std::error_code ignored;
                fs::remove(candidate.lease_path.parent_path(), ignored);
            }
            ++removed;
        }
    std::cout << "generation.gc removed_product_sets=" << removed << '\n';
}
} // namespace

int main(int argc, char **argv)
{
    bool compose = false, verify = false, gc = false;
    std::string root_arg, scope, result_list_arg;
    std::vector<fs::path> result_paths;
    const auto report_failure = [](std::string_view code, std::string_view action,
                                   std::string_view reason) {
        std::cerr << "generation.error code=" << code << " action=" << action
                  << " reason=" << reason << '\n';
    };
    try
    {
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument(argv[index]);
            if (argument == "--compose") compose = true;
            else if (argument == "--verify") verify = true;
            else if (argument == "--gc") gc = true;
            else if ((argument == "--build-root" || argument == "--scope" ||
                      argument == "--result" || argument == "--result-list") &&
                     index + 1 < argc)
            {
                const std::string value = argv[++index];
                if (argument == "--build-root") root_arg = value;
                else if (argument == "--scope") scope = value;
                else if (argument == "--result") result_paths.push_back(Utf8Path(value));
                else if (argument == "--result-list")
                {
                    if (!result_list_arg.empty() || value.empty())
                        Fail("invalid-arguments", "-", "result list must be supplied once");
                    result_list_arg = value;
                }
            }
            else
                Fail("invalid-arguments", "-", "unknown or incomplete argument");
        }
        const bool has_results = !result_paths.empty() || !result_list_arg.empty();
        if (static_cast<int>(compose) + static_cast<int>(verify) + static_cast<int>(gc) != 1 ||
            root_arg.empty() ||
            (gc && !scope.empty()) ||
            (!gc && scope.empty()) ||
            compose != has_results)
            Fail("invalid-arguments", "-",
                 "usage: --compose --build-root <absolute> --scope <scope> "
                 "[--result-list <absolute-file>] [--result <absolute>]... | "
                 "--verify --build-root <absolute> --scope <scope> | "
                 "--gc --build-root <absolute>");

        if (!gc) ScopeFileName(scope);
        const fs::path root = BuildRoot(root_arg);
        if (compose)
        {
            hs::GenerationFileLock mutation(root / ".generation-mutation.lock",
                hs::GenerationFileLock::Mode::Exclusive, true);
            if (!result_list_arg.empty())
                AppendResultList(root, Utf8Path(result_list_arg), result_paths);
            Compose(root, scope, result_paths);
        }
        else if (verify)
            Verify(root, scope);
        else
        {
            hs::GenerationFileLock mutation(root / ".generation-mutation.lock",
                hs::GenerationFileLock::Mode::Exclusive, true);
            GarbageCollect(root);
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
