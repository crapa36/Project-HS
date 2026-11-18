#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/asset_manifest/generation_lock.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hs
{
namespace
{
using Json = nlohmann::json;
namespace fs = std::filesystem;

[[nodiscard]] bool IsDigest(std::string_view value)
{
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
           });
}

[[nodiscard]] bool IsActionId(std::string_view value)
{
    return !value.empty() && value.size() <= 128 && value != "." && value != ".." &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
           });
}

[[nodiscard]] std::string ScopeFileName(std::string_view scope)
{
    if (scope.empty() || scope.size() > 128)
        throw std::runtime_error("invalid Generation scope length");
    std::string encoded;
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char ch : scope)
    {
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.')
            encoded.push_back(static_cast<char>(ch));
        else if (ch == ':')
        {
            encoded.push_back('%');
            encoded.push_back(hex[ch >> 4]);
            encoded.push_back(hex[ch & 15]);
        }
        else
            throw std::runtime_error("unsafe Generation scope character");
    }
    return encoded + ".json";
}

[[nodiscard]] bool IsSafeProductFilename(std::string_view value)
{
    if (value.empty() || value == "." || value == ".." ||
        value.find_first_of("/\\:") != std::string_view::npos)
        return false;
    const auto path = fs::u8path(value);
    return path.filename() == path && !path.has_root_path();
}

[[nodiscard]] fs::path ResolveLocator(const fs::path &root,
                                      const std::string &locator)
{
    const fs::path relative = fs::u8path(locator);
    if (locator.empty() || relative.is_absolute() || relative.has_root_path())
        throw std::runtime_error("absolute or empty Generation locator");
    for (const auto &part : relative)
        if (part.empty() || part == "." || part == "..")
            throw std::runtime_error("unsafe Generation locator: " + locator);
    const auto full = fs::canonical(root / relative);
    const auto within = full.lexically_relative(root);
    if (within.empty() || within.is_absolute())
        throw std::runtime_error("Generation locator escapes build root");
    for (const auto &part : within)
        if (part == "..")
            throw std::runtime_error("Generation locator escapes build root");
    if (!fs::is_regular_file(full))
        throw std::runtime_error("Generation product is not a file: " + locator);
    return full;
}

[[nodiscard]] Json ReadJson(const fs::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open Generation file: " + path.string());
    return Json::parse(input);
}
} // namespace

Result SelectedGeneration::Require(std::string_view logical_id,
                                   fs::path &path) const
{
    const auto found = product_paths.find(std::string(logical_id));
    if (found == product_paths.end())
        return Result::Failure(ErrorCode::InvalidState, "generation",
                               "selected Generation lacks product " +
                                   std::string(logical_id));
    path = found->second;
    return Result::Success();
}

Result LoadSelectedGeneration(const fs::path &manifest_path,
                              SelectedGeneration &generation)
{
    try
    {
        const auto absolute_manifest = fs::absolute(manifest_path);
        const auto root = fs::canonical(absolute_manifest.parent_path().parent_path().parent_path());
        GenerationFileLock root_reader_lease;
        const auto mutation_lock_path = root / ".generation-mutation.lock";
        if (fs::exists(mutation_lock_path))
            root_reader_lease = GenerationFileLock(
                mutation_lock_path, GenerationFileLock::Mode::Shared, false);

        const auto manifest = ReadJson(absolute_manifest);
        const auto &schema = manifest.at("schema_version");
        if (!schema.is_number_integer() || schema != 5)
            throw std::runtime_error("unsupported Generation manifest schema");
        const auto id = manifest.at("generation_id").get<std::string>();
        const auto scope = manifest.at("scope").get<std::string>();
        const auto expected_manifest = root / "Generations" / "current" / ScopeFileName(scope);
        if (!IsDigest(id) || absolute_manifest != expected_manifest ||
            fs::canonical(expected_manifest) != expected_manifest)
            throw std::runtime_error("invalid Generation manifest identity or path");

        SelectedGeneration loaded;
        loaded.id = id;
        loaded.scope = scope;
        const auto &products = manifest.at("products");
        if (!products.is_array() || products.empty())
            throw std::runtime_error("Generation product set is incomplete");
        std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> identities;
        std::unordered_map<std::string, std::vector<std::string>> dependencies;
        std::unordered_set<std::string> action_ids;
        std::unordered_set<std::string> leased_product_sets;
        std::string_view previous_id;
        for (const auto &item : products)
        {
            const auto &logical_id = item.at("logical_id").get_ref<const std::string &>();
            if (logical_id <= previous_id)
                throw std::runtime_error("Generation products are not sorted uniquely");
            previous_id = logical_id;
            const auto product_locator = item.at("locator").get<std::string>();
            const auto digest = item.at("digest").get<std::string>();
            const auto size = item.at("size").get<std::uint64_t>();
            const auto action_id = item.at("action_id").get<std::string>();
            const auto namespace_path = item.at("namespace_path").get<std::string>();
            const auto product_set_id = item.at("product_set_id").get<std::string>();
            const auto relative_namespace = fs::path(namespace_path);
            const auto product_prefix = "Products/" + namespace_path + "/" +
                product_set_id + "/";
            if (!IsActionId(action_id) || namespace_path.empty() ||
                relative_namespace.has_root_path() ||
                relative_namespace.generic_string() != namespace_path ||
                !IsDigest(product_set_id) || !product_locator.starts_with(product_prefix) ||
                !IsSafeProductFilename(std::string_view(product_locator).substr(product_prefix.size())))
                throw std::runtime_error("invalid Generation product ownership or locator");
            for (const auto &part : relative_namespace)
                if (!IsActionId(part.string()))
                    throw std::runtime_error("unsafe Generation action namespace");

            std::string product_set_key = namespace_path;
            product_set_key.push_back('\0');
            product_set_key += product_set_id;
            if (leased_product_sets.insert(std::move(product_set_key)).second)
            {
                const auto lease_path = root / "Products" / fs::u8path(namespace_path) /
                    ".leases" / (product_set_id + ".lock");
                if (fs::canonical(lease_path) != lease_path)
                    throw std::runtime_error("ProductSet lease path aliases another path");
                loaded.reader_leases_.push_back(std::make_shared<GenerationFileLock>(
                    lease_path, GenerationFileLock::Mode::Shared, false));
            }
            const auto path = ResolveLocator(root, product_locator);
            if (path != root / fs::u8path(product_locator))
                throw std::runtime_error("Generation product locator aliases another path");
            if (!digest.starts_with("sha256:") ||
                !IsDigest(std::string_view(digest).substr(7)))
                throw std::runtime_error("invalid Generation product identity");
            if (fs::file_size(path) != size)
                throw std::runtime_error("Generation product size mismatch: " + logical_id);
            loaded.product_paths.emplace(logical_id, path);
            loaded.product_digests.emplace(logical_id, digest);
            auto &required_products = dependencies[logical_id];
            if (item.contains("dependencies"))
            {
                if (!item["dependencies"].is_array())
                    throw std::runtime_error("Generation product dependencies must be an array");
                for (const auto &dependency : item["dependencies"])
                {
                    const auto required_id = dependency.get<std::string>();
                    if (required_id.empty() || required_id == logical_id)
                        throw std::runtime_error("invalid Generation product dependency");
                    required_products.push_back(required_id);
                }
                if (!std::is_sorted(required_products.begin(), required_products.end()) ||
                    std::adjacent_find(required_products.begin(), required_products.end()) !=
                        required_products.end())
                    throw std::runtime_error("Generation product dependencies are not sorted uniquely");
            }
            identities.emplace(logical_id, std::pair{digest, size});
            action_ids.insert(action_id);
        }
        root_reader_lease = GenerationFileLock{};
        std::unordered_map<std::string, std::uint8_t> dependency_state;
        std::vector<std::pair<std::string, std::size_t>> pending;
        for (const auto &product : products)
        {
            const auto &id = product.at("logical_id").get_ref<const std::string &>();
            if (dependency_state[id] == 2)
                continue;
            dependency_state[id] = 1;
            pending.emplace_back(id, 0);
            while (!pending.empty())
            {
                auto &[logical_id, next] = pending.back();
                const auto &required_products = dependencies.at(logical_id);
                if (next == required_products.size())
                {
                    dependency_state[logical_id] = 2;
                    pending.pop_back();
                    continue;
                }
                const auto &dependency = required_products[next++];
                if (!dependencies.contains(dependency))
                    throw std::runtime_error("Generation lacks product dependency: " + dependency);
                if (dependency_state[dependency] == 1)
                    throw std::runtime_error("cyclic Generation product dependency: " + dependency);
                if (dependency_state[dependency] == 0)
                {
                    dependency_state[dependency] = 1;
                    pending.emplace_back(dependency, 0);
                }
            }
        }
        const auto &edges = manifest.at("upstream_product_dependencies");
        if (!edges.is_array())
            throw std::runtime_error("Generation upstream identities are missing");
        std::pair<std::string, std::string> previous_edge;
        bool has_previous_edge = false;
        for (const auto &edge : edges)
        {
            const auto action_id = edge.at("action_id").get<std::string>();
            const auto logical_id = edge.at("logical_id").get<std::string>();
            const auto digest = edge.at("digest").get<std::string>();
            const auto size = edge.at("size").get<std::uint64_t>();
            const auto key = std::pair{action_id, logical_id};
            const auto found = identities.find(logical_id);
            if (!action_ids.contains(action_id) || found == identities.end() ||
                found->second != std::pair{digest, size} ||
                (has_previous_edge && !(previous_edge < key)))
                throw std::runtime_error("Generation upstream identity mismatch");
            previous_edge = key;
            has_previous_edge = true;
        }
        loaded.product_dependencies = std::move(dependencies);
        generation = std::move(loaded);
        return Result::Success();
    }
    catch (const std::exception &error)
    {
        return Result::Failure(ErrorCode::InvalidState, "generation", error.what());
    }
}

} // namespace hs
