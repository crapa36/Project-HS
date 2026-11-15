#pragma once
#include "../AssetPipeline/action.hpp"
#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace hs::environment_producer
{
namespace fs = std::filesystem;
namespace ap = hs::asset_pipeline;
inline void Require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error("hs_environment: " + std::string(message));
}

inline void AddInputs(ap::ActionSpec& spec, const fs::path& source_root,
               const std::vector<fs::path>& paths)
{
    for (const auto& path : paths) {
        const auto relative = path.lexically_relative(source_root);
        Require(!relative.empty() && !relative.is_absolute() &&
                relative.begin() != relative.end() && *relative.begin() != "..",
                "input escapes source root");
        const auto logical_id=relative.generic_string();
        if(std::ranges::none_of(spec.inputs,[&](const auto& input){return input.logical_id==logical_id;}))
            spec.inputs.push_back(ap::HashInput(logical_id,path));
    }
}

struct InputSnapshot
{
    fs::path source_root;
    fs::path snapshot_root;
    std::map<std::string, ap::Input> captured;

    void Capture(const fs::path& snapshot_path)
    {
        const auto relative = snapshot_path.lexically_relative(snapshot_root);
        Require(!relative.empty() && !relative.is_absolute() &&
                relative.begin() != relative.end() && *relative.begin() != "..",
                "snapshot input escapes its root");
        const auto logical_id = relative.generic_string();
        if (captured.contains(logical_id)) return;
        const auto source_path = source_root / relative;
        const auto target_path = snapshot_root / relative;
        ap::MakeDirectory(target_path.parent_path());
        captured.emplace(logical_id,
            ap::CopyAndHashInput(logical_id, source_path, target_path));
    }

    void SetInputs(ap::ActionSpec& spec, const std::vector<fs::path>& snapshot_paths)
    {
        spec.inputs.clear();
        for (const auto& path : snapshot_paths)
        {
            Capture(path);
            const auto relative = path.lexically_relative(snapshot_root).generic_string();
            spec.inputs.push_back(captured.at(relative));
        }
        std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);
        spec.inputs.erase(std::unique(spec.inputs.begin(), spec.inputs.end(),
            [](const auto& left, const auto& right) { return left.logical_id == right.logical_id; }),
            spec.inputs.end());
    }
};

inline bool IsCurrent(const fs::path& build_root, const ap::ActionSpec& spec,
               const std::vector<std::string>& names)
{
    const auto result_path = build_root / "Actions" / spec.namespace_path / "result.json";
    if (!fs::exists(result_path) || !fs::exists(result_path.parent_path() / "result.d"))
        return false;
    try {
        const ap::ActionResultSnapshot snapshot(build_root, spec.namespace_path);
        if (snapshot.ActionId() != spec.action_id ||
            snapshot.ActionKeyValue() != ap::ActionKey(spec) ||
            snapshot.ProductCount() != names.size()) return false;
        for (const auto& name : names) {
            ap::UpstreamProduct dependency;
            fs::path product_path;
            snapshot.ReadProduct("environment/" + name, dependency, product_path);
        }
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

}
