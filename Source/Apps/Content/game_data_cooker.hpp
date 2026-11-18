#pragma once

#include "content_json.hpp"
#include "content_source_inventory.hpp"

#include <hs/game_rules/simulation_rules.hpp>
#include <hs/presentation/presentation_catalog.hpp>

#include <unordered_map>
#include <unordered_set>

namespace hs::content
{

struct ContentSources
{
    std::unordered_map<std::string, Json> documents;
};

void RequireCount(const Json &array, std::string_view file, std::string_view path,
                  std::size_t expected);
bool IsStableId(std::string_view value);
std::unordered_set<std::string> CollectIds(const Json &entries, std::string_view file);
void RequireReference(const std::unordered_set<std::string> &ids,
                      const std::string &reference, std::string_view file,
                      std::string_view path);
template <typename Number>
void RequirePositive(Number value, std::string_view file, std::string_view path)
{
    if (!(value > Number{}))
    {
        ThrowValidationError(file, path, "must be greater than zero");
    }
}

ContentSources LoadAndValidateGameDataDocuments();
ContentSources LoadScopedGameDataSources(bool is_simulation, std::vector<asset_pipeline::Input> &inputs);
hs::PresentationCamera ReadPresentationCamera(const ContentSources &sources);
hs::SimulationRules BuildSimulationRules(const ContentSources &sources, const hs::PresentationCamera &camera);
hs::PresentationCatalog BuildPresentationCatalog(const ContentSources &sources, const hs::PresentationCamera &camera);

} // namespace hs::content
