#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/asset_manifest/generation_lock.hpp>
#include <hs/core/sha256.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#endif

namespace hs
{
namespace
{
using Json = nlohmann::json;
namespace fs = std::filesystem;

class PayloadHash
{
public:
#ifdef _WIN32
    PayloadHash()
    {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            throw std::runtime_error("cannot initialize Generation SHA-256 provider");
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0)
        {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            algorithm_ = nullptr;
            throw std::runtime_error("cannot initialize Generation SHA-256 hash");
        }
    }
    ~PayloadHash()
    {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    PayloadHash(const PayloadHash &) = delete;
    PayloadHash &operator=(const PayloadHash &) = delete;
    void Add(std::span<const std::byte> bytes)
    {
        if (BCryptHashData(hash_, reinterpret_cast<PUCHAR>(
                const_cast<std::byte *>(bytes.data())), static_cast<ULONG>(bytes.size()), 0) < 0)
            throw std::runtime_error("cannot hash Generation payload");
    }
    [[nodiscard]] std::array<std::byte, 32> Finish()
    {
        std::array<std::byte, 32> digest{};
        if (BCryptFinishHash(hash_, reinterpret_cast<PUCHAR>(digest.data()),
                             static_cast<ULONG>(digest.size()), 0) < 0)
            throw std::runtime_error("cannot finish Generation SHA-256 hash");
        return digest;
    }
private:
    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_HASH_HANDLE hash_{};
#else
    void Add(std::span<const std::byte> bytes) { hash_.Add(bytes); }
    [[nodiscard]] std::array<std::byte, 32> Finish() { return hash_.Finish(); }
private:
    Sha256Stream hash_;
#endif
};

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

[[nodiscard]] std::string DigestText(const std::array<std::byte, 32> &digest)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string text = "sha256:";
    text.reserve(7 + digest.size() * 2);
    for (const auto byte : digest)
    {
        const auto value = std::to_integer<unsigned char>(byte);
        text.push_back(digits[value >> 4]);
        text.push_back(digits[value & 15]);
    }
    return text;
}

[[nodiscard]] std::string LogicalIdDigest(std::string_view logical_id)
{
    const auto digest = Sha256({reinterpret_cast<const std::byte *>(logical_id.data()),
                                logical_id.size()});
    constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (const auto byte : digest)
    {
        const auto value = std::to_integer<unsigned char>(byte);
        hex.push_back(digits[value >> 4]);
        hex.push_back(digits[value & 15]);
    }
    return hex;
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

Result ReadGenerationPointer(const fs::path &pointer_path, std::string &bytes)
{
    try
    {
        std::string snapshot;
#ifdef _WIN32
        const auto raw = CreateFileW(pointer_path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (raw == INVALID_HANDLE_VALUE)
            throw std::runtime_error("cannot open Generation pointer (Win32 " +
                std::to_string(GetLastError()) + "): " + pointer_path.string());
        const auto close = [](void *handle) { CloseHandle(handle); };
        const std::unique_ptr<void, decltype(close)> handle(raw, close);
        std::array<char, 64 * 1024> buffer{};
        for (;;)
        {
            DWORD count{};
            if (!ReadFile(handle.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                          &count, nullptr))
                throw std::runtime_error("cannot read Generation pointer (Win32 " +
                    std::to_string(GetLastError()) + "): " + pointer_path.string());
            if (count == 0) break;
            snapshot.append(buffer.data(), count);
        }
#else
        std::ifstream input(pointer_path, std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot open Generation pointer: " + pointer_path.string());
        snapshot.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        if (input.bad())
            throw std::runtime_error("cannot read Generation pointer: " + pointer_path.string());
#endif
        bytes = std::move(snapshot);
        return Result::Success();
    }
    catch (const std::exception &error)
    {
        return Result::Failure(ErrorCode::InvalidState, "generation", error.what());
    }
}

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

Result LoadSelectedGeneration(const fs::path &pointer_path,
                              SelectedGeneration &generation)
{
    try
    {
        const auto absolute_pointer = fs::absolute(pointer_path);
        std::string pointer_bytes;
        if (const auto read = ReadGenerationPointer(absolute_pointer, pointer_bytes); !read)
            return read;
        const auto pointer = Json::parse(pointer_bytes);
        if (pointer.at("schema_version") != 1)
            throw std::runtime_error("unsupported Generation pointer schema");
        const auto id = pointer.at("generation_id").get<std::string>();
        const auto scope = pointer.at("scope").get<std::string>();
        const auto locator = pointer.at("manifest_locator").get<std::string>();
        if (!IsDigest(id) || scope.empty() ||
            locator != "Generations/" + id + "/generation.json")
            throw std::runtime_error("invalid Generation pointer identity");
        const auto root = fs::canonical(absolute_pointer.parent_path().parent_path());
        const auto manifest_path = ResolveLocator(root, locator);
        auto lease = std::make_shared<GenerationFileLock>(manifest_path, GenerationFileLock::Mode::Shared);
        const auto manifest = ReadJson(manifest_path);
        const auto &schema = manifest.at("schema_version");
        if (!schema.is_number_integer() || (schema != 2 && schema != 3))
            throw std::runtime_error("unsupported Generation manifest schema");
        const int version = schema.get<int>();
        if (manifest.at("generation_id") != id ||
            manifest.at("scope") != scope)
            throw std::runtime_error("Generation manifest does not match pointer");

        SelectedGeneration loaded;
        loaded.reader_lease_ = std::move(lease);
        loaded.id = id;
        loaded.scope = scope;
        const auto &products = manifest.at("products");
        const auto &required = manifest.at("required_ids");
        if (!products.is_array() || products.empty() ||
            !required.is_array() || required.size() != products.size())
            throw std::runtime_error("Generation product set is incomplete");
        std::vector<std::string> ids;
        ids.reserve(products.size());
        std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> identities;
        std::unordered_map<std::string, std::vector<std::string>> dependencies;
        std::unordered_set<std::string> action_ids;
        for (const auto &item : products)
        {
            const auto logical_id = item.at("logical_id").get<std::string>();
            const auto product_locator = item.at("locator").get<std::string>();
            const auto digest = item.at("digest").get<std::string>();
            const auto size = item.at("size").get<std::uint64_t>();
            const auto action_id = item.at("action_id").get<std::string>();
            if (version == 3)
            {
                const auto namespace_path = item.at("namespace_path").get<std::string>();
                const auto relative_namespace = fs::path(namespace_path);
                if (!IsActionId(action_id) || namespace_path.empty() ||
                    relative_namespace.has_root_path() ||
                    relative_namespace.generic_string() != namespace_path ||
                    product_locator != "Generations/" + id + "/products/" +
                                           LogicalIdDigest(logical_id) + ".bin")
                    throw std::runtime_error("invalid Generation snapshot ownership or locator");
                for (const auto &part : relative_namespace)
                    if (!IsActionId(part.string()))
                        throw std::runtime_error("unsafe Generation action namespace");
            }
            if (logical_id.empty() ||
                !loaded.product_paths.emplace(
                     logical_id, ResolveLocator(root, product_locator)).second)
                throw std::runtime_error("duplicate or empty Generation product ID");
            if (version == 3 && loaded.product_paths.at(logical_id) !=
                                    root / fs::path(product_locator))
                throw std::runtime_error("Generation snapshot locator aliases another path");
            if (action_id.empty() || !digest.starts_with("sha256:") ||
                !IsDigest(std::string_view(digest).substr(7)))
                throw std::runtime_error("invalid Generation product identity");
            if (fs::file_size(loaded.product_paths.at(logical_id)) != size)
                throw std::runtime_error("Generation product size mismatch: " + logical_id);
            if (!loaded.product_digests.emplace(logical_id, digest).second)
                throw std::runtime_error("duplicate Generation product digest: " + logical_id);
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
            ids.push_back(logical_id);
        }
        if (!std::is_sorted(ids.begin(), ids.end()) ||
            std::adjacent_find(ids.begin(), ids.end()) != ids.end())
            throw std::runtime_error("Generation products are not sorted uniquely");
        for (std::size_t index = 0; index < ids.size(); ++index)
            if (!required[index].is_string() ||
                required[index].get<std::string>() != ids[index])
                throw std::runtime_error("Generation required IDs do not match products");
        std::unordered_map<std::string, std::uint8_t> dependency_state;
        std::vector<std::pair<std::string, std::size_t>> pending;
        for (const auto &id : ids)
        {
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

Result VerifySelectedGenerationProducts(const SelectedGeneration &generation)
{
    try
    {
        if (generation.product_paths.empty() ||
            generation.product_paths.size() != generation.product_digests.size())
            throw std::runtime_error("Generation product path/digest keys do not match");

        constexpr std::size_t kChunkSize = 64 * 1024;
        std::array<std::byte, kChunkSize> buffer{};
        for (const auto &[logical_id, path] : generation.product_paths)
        {
            if (logical_id.empty() || path.empty())
                throw std::runtime_error("Generation product has an empty identity");
            const auto expected = generation.product_digests.find(logical_id);
            if (expected == generation.product_digests.end() ||
                !expected->second.starts_with("sha256:") ||
                !IsDigest(std::string_view(expected->second).substr(7)))
                throw std::runtime_error("Generation product digest is missing or invalid: " +
                                         logical_id);

            std::ifstream input(path, std::ios::binary);
            if (!input)
                throw std::runtime_error("cannot read Generation product: " + logical_id);
            PayloadHash hash;
            while (input.read(reinterpret_cast<char *>(buffer.data()),
                              static_cast<std::streamsize>(buffer.size())))
                hash.Add({buffer.data(), buffer.size()});
            const auto count = input.gcount();
            if (count > 0)
                hash.Add({buffer.data(), static_cast<std::size_t>(count)});
            if (!input.eof())
                throw std::runtime_error("cannot read Generation product: " + logical_id);
            if (DigestText(hash.Finish()) != expected->second)
                throw std::runtime_error("Generation product digest mismatch: " + logical_id);
        }
        for (const auto &[logical_id, digest] : generation.product_digests)
            if (!generation.product_paths.contains(logical_id) || digest.empty())
                throw std::runtime_error("Generation product digest has no matching payload: " +
                                         logical_id);
        return Result::Success();
    }
    catch (const std::exception &error)
    {
        return Result::Failure(ErrorCode::InvalidState, "generation", error.what());
    }
}

} // namespace hs
