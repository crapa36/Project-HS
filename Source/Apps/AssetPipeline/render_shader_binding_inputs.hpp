#pragma once

#include "action.hpp"
#include "render_shader_bindings.hpp"

namespace hs::shader_content
{
inline void AddRenderBindingInputs(std::vector<asset_pipeline::Input> &inputs, const std::filesystem::path &source_root)
{
    const auto contract = RenderBindingContract();
    inputs.push_back({"shader/render-binding-defines-ir",
        source_root / "Source/RenderData/Public/hs/render_data/material_schema.hpp",
        asset_pipeline::Sha256Bytes(std::as_bytes(std::span(contract.data(), contract.size())))});
}
} // namespace hs::shader_content
