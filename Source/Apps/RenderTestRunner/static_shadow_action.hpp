#pragma once

#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/render_data/static_shadow_archive.hpp>

#include <functional>
#include <memory>

namespace hs::render_test
{
using BeginStaticShadowArchive = std::function<Result(const render_data::StaticShadowArchiveDescription &)>;
using WriteStaticShadowPage = std::function<Result(std::uint32_t, std::int32_t, std::int32_t, std::span<const float>)>;
int ProduceStaticShadow(int argc, char **argv,
    std::shared_ptr<const StaticRenderScene> (*scene_builder)(const SelectedGeneration &),
    void (*native_bake)(const SelectedGeneration &, const std::shared_ptr<const StaticRenderScene> &,
        const std::filesystem::path &,
        const BeginStaticShadowArchive &, const WriteStaticShadowPage &));
}
