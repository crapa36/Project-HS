#pragma once

#include "../AssetPipeline/action.hpp"

#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/render_data/material_descriptor.hpp>
#include <hs/render_data/material_schema.hpp>

#include <map>
#include <stdexcept>

namespace hs::app
{
class BakeProductInputs
{
public:
    BakeProductInputs(const std::filesystem::path &build, const SelectedGeneration &generation)
        : build_(build), products_root_(build / "Products"), generation_(generation) {}

    asset_pipeline::UpstreamProduct Verify(const std::string &id)
    {
        const auto found = generation_.product_paths.find(id);
        if (found == generation_.product_paths.end())
            throw std::runtime_error("Missing selected bake input: " + id);
        const auto &path = found->second;
        // LoadSelectedGeneration admits Products/<namespace>/<ProductSet>/<filename>.
        const auto owner = path.parent_path().parent_path().lexically_relative(products_root_);
        if (owner.empty() || *owner.begin() == ".." || owner == ".")
            throw std::runtime_error("Selected bake input lies outside build Products: " + id);
        const auto name = owner.generic_string();
        auto receipt = receipts_.try_emplace(name, build_, name).first;
        const auto &metadata = receipt->second.ProductMetadata(id);
        const auto &digest = metadata.at("digest").get_ref<const std::string &>();
        const auto size = metadata.at("size").get<std::uint64_t>();
        if (generation_.product_digests.at(id) != digest || std::filesystem::file_size(path) != size ||
            asset_pipeline::Sha256File(path) != digest ||
            generation_.product_dependencies.at(id) != metadata.value("dependencies", std::vector<std::string>{}))
            throw std::runtime_error("Selected bake input differs from its Action product: " + id);
        return {id, digest, size, build_ / "Actions" / owner / "result.json"};
    }

private:
    const std::filesystem::path &build_;
    std::filesystem::path products_root_;
    const SelectedGeneration &generation_;
    std::map<std::string, asset_pipeline::ActionResultSnapshot> receipts_;
};

// The capture programs replace the runtime raster shaders of authored materials.
inline bool IsRuntimeSurfaceShaderDependency(std::string_view parent, std::string_view dependency)
{
    if (!dependency.starts_with("render/shader/")) return false;
    for (const auto name : render_data::kAuthoredMaterialNames)
    {
        if (parent != render_data::MaterialProduct(name)) continue;
        const auto prefix = "render/shader/" + std::string(name);
        return dependency == prefix + "_scene.dxil" || dependency == prefix + "_shadow.dxil";
    }
    return false;
}
}
