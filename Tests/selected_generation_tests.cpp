#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/core/sha256.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
namespace fs = std::filesystem;
using Json = nlohmann::json;

void Check(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void Write(const fs::path &path, const std::string &bytes)
{
    std::ofstream output(path, std::ios::binary);
    output << bytes;
    output.close();
    Check(output.good(), "cannot write fixture");
}

std::string Digest(std::string_view bytes)
{
    const auto digest = hs::Sha256({reinterpret_cast<const std::byte *>(bytes.data()),
                                    bytes.size()});
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "sha256:";
    result.reserve(71);
    for (const auto byte : digest)
    {
        const auto value = std::to_integer<unsigned char>(byte);
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 15]);
    }
    return result;
}

struct Fixture
{
    fs::path root;
    Fixture()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0;; ++attempt)
        {
            root = fs::temp_directory_path() /
                   ("hs-selected-generation-" + std::to_string(stamp) + "-" +
                    std::to_string(attempt));
            if (fs::create_directory(root))
                break;
        }
    }
    ~Fixture()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};
} // namespace

int main()
{
    try
    {
        Fixture fixture;
        const std::string id(64, 'a');
        const auto pointer = fixture.root / "GenerationState" / "test.json";
        const auto manifest = fixture.root / "Generations" / id / "generation.json";
        fs::create_directories(pointer.parent_path());
        fs::create_directories(manifest.parent_path());
        Write(fixture.root / "a.bin", "a");
        Write(fixture.root / "b.bin", "b");
        Write(pointer, Json{{"schema_version", 1}, {"generation_id", id},
                            {"scope", "test"},
                            {"manifest_locator", "Generations/" + id + "/generation.json"}}.dump());
        const auto product = [](const std::string &name) {
            return Json{{"logical_id", name}, {"action_id", "fixture"},
                        {"locator", name + ".bin"}, {"size", 1},
                        {"digest", "sha256:" + std::string(64, '0')}};
        };
        Json valid{{"schema_version", 2}, {"generation_id", id}, {"scope", "test"},
                   {"required_ids", {"a", "b"}},
                   {"products", {product("a"), product("b")}},
                   {"upstream_product_dependencies", Json::array()}};
        valid["products"][0]["dependencies"] = {"b"};
        valid["products"][0]["digest"] = Digest("a");
        valid["products"][1]["digest"] = Digest("b");
        Write(manifest, valid.dump());
        hs::SelectedGeneration loaded;
        const auto baseline = hs::LoadSelectedGeneration(pointer, loaded);
        Check(baseline.Succeeded(), std::string(baseline.Message()));
        Check(loaded.product_dependencies.at("a") == std::vector<std::string>{"b"} &&
                  loaded.product_dependencies.at("b").empty(),
              "selected Generation did not retain its validated direct dependencies");
        Check(hs::VerifySelectedGenerationProducts(loaded).Succeeded(),
              "selected Generation payload digest verification failed");
        const auto saved = loaded;
        Write(fixture.root / "a.bin", "A");
        const auto corrupted = hs::VerifySelectedGenerationProducts(loaded);
        Check(!corrupted.Succeeded(), "same-size corrupted Generation product accepted");
        Check(loaded.id == saved.id && loaded.scope == saved.scope &&
                  loaded.product_paths == saved.product_paths &&
                  loaded.product_digests == saved.product_digests &&
                  loaded.product_dependencies == saved.product_dependencies,
              "failed payload verification changed the selected Generation");
        Write(fixture.root / "a.bin", "a");
        const auto rejects = [&](const Json &candidate, const std::string &reason) {
            Write(manifest, candidate.dump());
            const auto result = hs::LoadSelectedGeneration(pointer, loaded);
            Check(!result.Succeeded(), "accepted " + reason);
            Check(loaded.id == saved.id && loaded.scope == saved.scope &&
                      loaded.product_paths == saved.product_paths &&
                      loaded.product_digests == saved.product_digests &&
                      loaded.product_dependencies == saved.product_dependencies,
                  "failed load changed the selected Generation");
        };
        auto invalid = valid;
        invalid["products"][0]["size"] = 999;
        rejects(invalid, "product size mismatch");
        invalid = valid;
        invalid["products"][0]["dependencies"] = {"missing"};
        rejects(invalid, "missing declared product dependency");
        invalid = valid;
        invalid["products"][1]["dependencies"] = {"a"};
        rejects(invalid, "dependency cycle");
        invalid = valid;
        invalid["products"][0]["dependencies"] = {"b", "b"};
        rejects(invalid, "duplicate dependency");
        invalid = valid;
        invalid["products"][0]["dependencies"] = {"a"};
        rejects(invalid, "self dependency");
        invalid = valid;
        invalid["products"][0]["dependencies"] = "b";
        rejects(invalid, "non-array dependencies");
        Json chain = valid;
        chain["products"] = Json::array();
        chain["required_ids"] = Json::array();
        constexpr unsigned chain_length = 20000;
        const auto chain_id = [](unsigned index) {
            return "chain-" + std::to_string(100000 + index);
        };
        for (unsigned index = 0; index < chain_length; ++index)
        {
            auto entry = product(chain_id(index));
            entry["locator"] = "a.bin";
            if (index + 1 < chain_length)
                entry["dependencies"] = {chain_id(index + 1)};
            chain["products"].push_back(std::move(entry));
            chain["required_ids"].push_back(chain_id(index));
        }
        Write(manifest, chain.dump());
        hs::SelectedGeneration long_chain;
        Check(hs::LoadSelectedGeneration(pointer, long_chain).Succeeded(),
              "rejected long acyclic dependency chain");
        chain["products"].back()["dependencies"] = {chain_id(0)};
        rejects(chain, "long dependency cycle");
        // Schema 2 remains readable; schema 3 owns deterministic snapshot files.
        auto snapshot = valid;
        snapshot["schema_version"] = 3;
        for (auto &entry : snapshot["products"])
        {
            const auto logical_id = entry["logical_id"].get<std::string>();
            const auto digest = hs::Sha256({reinterpret_cast<const std::byte *>(logical_id.data()),
                                            logical_id.size()});
            constexpr char digits[] = "0123456789abcdef";
            std::string hex;
            for (const auto byte : digest)
            {
                const auto value = std::to_integer<unsigned char>(byte);
                hex.push_back(digits[value >> 4]);
                hex.push_back(digits[value & 15]);
            }
            const auto locator = "Generations/" + id + "/products/" + hex + ".bin";
            entry["locator"] = locator;
            entry["namespace_path"] = "fixture/products";
            fs::create_directories((fixture.root / locator).parent_path());
            Write(fixture.root / locator, logical_id);
        }
        Write(manifest, snapshot.dump());
        hs::SelectedGeneration snapshot_loaded;
        const auto snapshot_result = hs::LoadSelectedGeneration(pointer, snapshot_loaded);
        Check(snapshot_result.Succeeded(), std::string(snapshot_result.Message()));
        Check(hs::VerifySelectedGenerationProducts(snapshot_loaded).Succeeded(),
              "schema-3 payload digest verification failed");
        Check(snapshot_loaded.product_paths.at("a") != saved.product_paths.at("a"),
              "snapshot resolves producer-era locator");
#ifndef _WIN32
        const auto snapshot_a = fixture.root /
                                snapshot["products"][0]["locator"].get<std::string>();
        const auto snapshot_b = fixture.root /
                                snapshot["products"][1]["locator"].get<std::string>();
        fs::remove(snapshot_a);
        fs::create_symlink(snapshot_b, snapshot_a);
        rejects(snapshot, "schema-3 snapshot symlink alias");
        fs::remove(snapshot_a);
        Write(snapshot_a, "a");
#endif
        invalid = snapshot;
        invalid["products"][0]["locator"] = "a.bin";
        rejects(invalid, "schema-3 locator outside snapshot");
        invalid = snapshot;
        invalid["products"][0]["locator"] = snapshot["products"][1]["locator"];
        rejects(invalid, "schema-3 locator for another logical product");
        invalid = snapshot;
        invalid["products"][0]["namespace_path"] = "../fixture";
        rejects(invalid, "unsafe snapshot producer namespace");
        invalid = snapshot;
        invalid["products"][0]["namespace_path"] = "fixture\\products";
        rejects(invalid, "backslash snapshot producer namespace");
        invalid = snapshot;
        invalid["products"][0].erase("namespace_path");
        rejects(invalid, "missing snapshot producer namespace");
        invalid = snapshot;
        invalid["schema_version"] = 3.5;
        rejects(invalid, "fractional Generation schema version");
        invalid = snapshot;
        invalid["schema_version"] = std::uint64_t{4294967299};
        rejects(invalid, "narrowing Generation schema version");
        invalid = snapshot;
        invalid["schema_version"] = std::uint64_t{18446744073709551615ULL};
        rejects(invalid, "overflowing Generation schema version");
        invalid = snapshot;
        invalid["products"][0]["size"] = 2;
        rejects(invalid, "schema-3 product size mismatch");
        invalid = snapshot;
        invalid["products"][0]["digest"] = "not-a-digest";
        rejects(invalid, "schema-3 invalid product digest");
        invalid = snapshot;
        invalid["products"][0]["dependencies"] = {"missing"};
        rejects(invalid, "schema-3 missing dependency closure");
        std::cout << "selected Generation size, dependency closure, and failure isolation passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "selected Generation: " << error.what() << '\n';
        return 1;
    }
}
