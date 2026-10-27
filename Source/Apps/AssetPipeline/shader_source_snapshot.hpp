#pragma once

#include "action.hpp"

#include <stdexcept>

namespace hs::shader_content
{
inline std::filesystem::path CreateShaderSourceSnapshot(
    const std::vector<asset_pipeline::Input> &inputs, const std::filesystem::path &stage)
{
    const auto snapshot = stage / "source-snapshot";
    std::size_t copied{};
    for (const auto &input : inputs)
    {
        if (!input.logical_id.starts_with("Content/Shaders/")) continue;
        const auto target = snapshot / std::filesystem::path(input.logical_id);
        asset_pipeline::MakeDirectory(target.parent_path());
        const auto staged = asset_pipeline::CopyAndHashInput(input.logical_id, input.path, target);
        if (staged.digest != input.digest)
            throw std::runtime_error("shader source changed while creating snapshot: " + input.logical_id);
        ++copied;
    }
    if (copied == 0) throw std::runtime_error("shader source dependency snapshot is empty");
    return snapshot;
}
} // namespace hs::shader_content
