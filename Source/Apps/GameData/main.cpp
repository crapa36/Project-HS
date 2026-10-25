#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../AssetPipeline/action.hpp"
#include "../Content/arena_layout_payload.hpp"
#include "../Content/content_cooker.hpp"
#include "../Content/content_paths.hpp"
#include "../Content/release_presentation_codegen.hpp"
#include "../Content/release_simulation_codegen.hpp"

#include <hs/core/cooked_format.hpp>
#include <hs/presentation/presentation_catalog_io.hpp>

#include <array>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace
{
namespace fs = std::filesystem;
namespace ap = hs::asset_pipeline;

constexpr std::array<std::string_view, 9> kSimulationDocuments = {
    "stats", "level", "characters", "skills", "upgrades", "enemies",
    "relics", "spawn_schedule", "bosses"};
constexpr std::array<std::string_view, 2> kPresentationDocuments = {
    "characters", "relics"};

void Require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error("hs_game_data: " + std::string(message));
}

std::span<const std::byte> Bytes(std::string_view value)
{
    return std::as_bytes(std::span(value.data(), value.size()));
}

template <typename T>
std::span<const std::byte> Bytes(const T &value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    return std::as_bytes(std::span(&value, 1));
}

std::uint64_t ParseLegacyHash(std::string_view text)
{
    if (text.ends_with('\n'))
        text.remove_suffix(1);
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    Require(error == std::errc{} && end == text.data() + text.size(),
            "invalid legacy source hash product");
    return value;
}

std::string BuildCooked(std::string_view payload, std::uint64_t schema_hash,
                        std::uint64_t source_hash)
{
    Require(payload.size() <= std::numeric_limits<std::uint32_t>::max(),
            "semantic payload exceeds cooked ABI size");
    hs::CookedHeader header;
    header.schema_hash = schema_hash;
    header.source_hash = source_hash;
    header.table_count = 1;
    header.payload_size = static_cast<std::uint32_t>(payload.size());
    header.payload_crc32 = hs::Crc32(Bytes(payload));
    std::string cooked(sizeof(header) + payload.size(), '\0');
    std::memcpy(cooked.data(), &header, sizeof(header));
    std::memcpy(cooked.data() + sizeof(header), payload.data(), payload.size());
    return cooked;
}

void RequireConfiguredRoots(const fs::path &source_root, const fs::path &build_root)
{
    Require(source_root.is_absolute() && build_root.is_absolute(),
            "source and build roots must be absolute");
    Require(source_root == hs::content::paths::SourceRoot() &&
            build_root == hs::content::paths::BuildRoot(),
            "CLI roots differ from hs_build_config.json");
    ap::RequireDirectory(source_root);
}

void ValidateOnly(const fs::path &source_root, const fs::path &build_root)
{
    RequireConfiguredRoots(source_root, build_root);
    const auto sources = hs::content::LoadAndValidateGameDataDocuments();
    static_cast<void>(hs::content::BuildGameData(sources));
    std::cout << "game-data.validated documents=" << sources.documents.size() << '\n';
}

void Produce(std::string_view mode, const fs::path &source_root,
             const fs::path &build_root)
{
    RequireConfiguredRoots(source_root, build_root);
    ap::MakeDirectory(build_root);

    ap::ActionSpec spec;
    spec.action_id = "game-data." + std::string(mode);
    spec.recipe_id = "project-hs.game-data." + std::string(mode);
    spec.recipe_version = mode == "simulation-payload" ? 4 :
                          (mode == "finalize-simulation" || mode == "presentation-payload") ? 2 : 1;
    spec.namespace_path = "game-data/" + std::string(mode);
    std::string filename;
    std::string logical_id;
    std::string bytes;
    std::string arena_layout_bytes;
    std::string generated_cpp;

    if (mode == "simulation-payload" || mode == "presentation-payload")
    {
        const auto names = mode == "simulation-payload"
            ? std::span<const std::string_view>(kSimulationDocuments)
            : std::span<const std::string_view>(kPresentationDocuments);
        const auto sources = hs::content::LoadScopedGameDataSources(names);
        spec.inputs.push_back(ap::HashInput(
            "Schemas/game.schema.json", source_root / "Schemas/game.schema.json"));
        for (const auto name : names)
        {
            const auto path = source_root / "ContentSource" / "GameData" /
                              (std::string(name) + ".json");
            const auto position = std::ranges::find(hs::content::kDocumentNames, name);
            const auto &text = sources.inventory.document_text[static_cast<std::size_t>(
                std::distance(hs::content::kDocumentNames.begin(), position))];
            spec.inputs.push_back({
                "ContentSource/GameData/" + std::string(name) + ".json", path,
                ap::Sha256Bytes(Bytes(std::string_view(text)))});
        }
        if (mode == "simulation-payload")
        {
            const auto rules = hs::content::BuildGameData(sources).simulation_rules;
            generated_cpp = hs::content::EmitReleaseSimulationCpp(rules);
            const auto payload = hs::CanonicalSimulationRulesBytes(rules);
            Require(payload.size() == sizeof(rules),
                    "canonical simulation payload size differs from cooked ABI");
            bytes.assign(reinterpret_cast<const char *>(payload.data()), payload.size());
            arena_layout_bytes = hs::content::EncodeArenaLayoutPayload(
                {rules.arena_half_extent, rules.arena_boundary, rules.arena_obstacles,
                 rules.arena_obstacle_count});
            filename = "simulation_rules.payload";
            logical_id = "game-data/simulation-payload";
        }
        else
        {
            const auto presentation = hs::content::BuildPresentationCatalog(sources);
            generated_cpp = hs::content::EmitReleasePresentationCpp(presentation);
            const auto payload = Bytes(presentation);
            bytes.assign(reinterpret_cast<const char *>(payload.data()), payload.size());
            filename = "presentation_catalog.payload";
            logical_id = "game-data/presentation-payload";
        }
    }
    else if (mode == "gameplay-identity" || mode == "content-identity")
    {
        const auto full = mode == "content-identity";
        std::optional<ap::Sha256Stream> digest;
        const auto scan = hs::content::ScanLegacyIdentity(
            full, [&](std::string_view name, const fs::path &path,
                      std::string_view chunk, bool last_chunk) {
                if (!digest)
                {
                    ap::RequireRegularFile(path);
                    digest.emplace();
                }
                digest->Add(Bytes(chunk));
                if (last_chunk)
                {
                    spec.inputs.push_back({std::string(name), path, digest->Finish()});
                    digest.reset();
                }
            });
        Require(!digest && spec.inputs.size() == scan.inputs.size(),
                "incomplete identity input digest");
        bytes = std::to_string(full ? scan.all_source_hash : scan.gameplay_source_hash) + "\n";
        filename = full ? "content_source_hash.txt" : "gameplay_source_hash.txt";
        logical_id = full ? "game-data/content-identity" : "game-data/gameplay-identity";
    }
    else if (mode == "finalize-simulation" || mode == "finalize-presentation")
    {
        const auto simulation = mode == "finalize-simulation";
        ap::UpstreamProduct payload_dependency;
        ap::UpstreamProduct hash_dependency;
        const auto payload_mode = simulation ? "simulation-payload" : "presentation-payload";
        const auto hash_mode = simulation ? "gameplay-identity" : "content-identity";
        const ap::ActionResultSnapshot payload_result(build_root, "game-data/" + std::string(payload_mode));
        const ap::ActionResultSnapshot hash_result(build_root, "game-data/" + std::string(hash_mode));
        const auto payload = payload_result.ReadProductBytes("game-data/" + std::string(payload_mode), payload_dependency);
        const auto hash_bytes = hash_result.ReadProductBytes("game-data/" + std::string(hash_mode), hash_dependency);
        spec.upstream = {payload_dependency, hash_dependency};
        Require(payload.size() == (simulation ? sizeof(hs::SimulationRules)
                                              : sizeof(hs::PresentationCatalog)),
                "semantic payload has unexpected ABI size");
        bytes = BuildCooked(payload,
                            simulation ? hs::SimulationRulesSchemaHash()
                                       : hs::PresentationCatalogSchemaHash(),
                            ParseLegacyHash(hash_bytes));
        filename = simulation ? "simulation_rules.hsbin" : "presentation_catalog.hsbin";
        logical_id = simulation ? "game-data/simulation-rules" :
                                  "game-data/presentation-catalog";
    }
    else
        throw std::runtime_error("hs_game_data: unsupported action mode");

    const auto stage = ap::CreateStage(build_root / "Actions" / spec.namespace_path);
    try
    {
        ap::WriteBytes(stage / filename, Bytes(std::string_view(bytes)));
        std::vector<ap::Product> products;
        products.push_back(ap::HashProduct(logical_id, filename, stage / filename));
        if (mode == "simulation-payload")
        {
            const auto arena_path = stage / "arena_layout.json";
            ap::WriteBytes(arena_path, Bytes(std::string_view(arena_layout_bytes)));
            products.push_back(ap::HashProduct("game-data/arena-layout", "arena_layout.json",
                                               arena_path));
        }
        if (!generated_cpp.empty())
        {
            const bool simulation = mode == "simulation-payload";
            const auto cpp_name = simulation ? "simulation_rules.generated.cpp" : "presentation_catalog.generated.cpp";
            const auto cpp_path = stage / cpp_name;
            ap::WriteBytes(cpp_path, Bytes(std::string_view(generated_cpp)));
            products.push_back(ap::HashProduct(simulation ? "game-data/simulation-cpp" : "game-data/presentation-cpp",
                                               cpp_name, cpp_path));
        }
        ap::Publish(build_root, spec, stage, std::move(products));
    }
    catch (...)
    {
        ap::RemoveStage(stage);
        throw;
    }
    ap::RemoveStage(stage);
    std::cout << spec.action_id << " published\n";
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    std::string mode;
    ap::Json action_id = nullptr;
    fs::path source_root;
    fs::path build_root;
    try
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring_view arg = argv[i];
            if (arg == L"--source-root" && source_root.empty() && ++i < argc)
                source_root = fs::path(argv[i]);
            else if (arg == L"--build-root" && build_root.empty() && ++i < argc)
                build_root = fs::path(argv[i]);
            else if (arg.starts_with(L"--") && mode.empty())
            {
                for (const auto character : arg.substr(2))
                {
                    Require((character >= L'a' && character <= L'z') ||
                            character == L'-', "action mode must be ASCII");
                    mode.push_back(static_cast<char>(character));
                }
            }
            else
                throw std::runtime_error("hs_game_data: invalid command line");
        }
        Require(!mode.empty() && !source_root.empty() && !build_root.empty(),
                "usage: hs_game_data --<action> --source-root <absolute> --build-root <absolute>");
        source_root = source_root.lexically_normal();
        build_root = build_root.lexically_normal();
        if (mode == "validate-only")
            ValidateOnly(source_root, build_root);
        else
        {
            if (mode == "simulation-payload" || mode == "presentation-payload" ||
                mode == "gameplay-identity" || mode == "content-identity" ||
                mode == "finalize-simulation" || mode == "finalize-presentation")
                action_id = "game-data." + mode;
            Produce(mode, source_root, build_root);
        }
        return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure("hs_game_data", error.what(),
                          {{"action_id", action_id}, {"mode", mode},
                           {"source_root", ap::DiagnosticPath(source_root)},
                           {"build_root", ap::DiagnosticPath(build_root)}});
        return 1;
    }
}
