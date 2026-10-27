#pragma once

#include "render_graph_types.hpp"

#include <hs/core/result.hpp>

#include <array>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

namespace hs
{
class RenderGraphBuilder;

using PostHdrHook = std::function<Result(RenderGraphBuilder &, ResourceVersion, ResourceVersion &)>;
struct PostSubgraphInputs
{
    bool bloom{};
    bool outline{};
    ResourceVersion scene_hdr;
    ResourceVersion composite_hdr;
    ResourceVersion depth;
    ResourceVersion normal;
    ResourceVersion metadata;
    ResourceVersion motion;
    ResourceVersion surface_history;
    ResourceVersion temporal_history;
    ResourceVersion ui;
    ResourceVersion temporal_normal_history;
    ResourceVersion temporal_material_history;
    ResourceVersion exposure;
    ResourceVersion external_temporal;
    std::array<ResourceVersion, 7> transparent_guides{};
    PostHdrHook pre_bloom, pre_tone_map;
    PostHdrHook devtools_ui;
    ResourceVersion screen_overlay_alpha, screen_overlay_upload, screen_overlay_gradient;
    ResourceVersion reactive_composite_hdr;
    bool fxaa_identity{};
    bool initialize_post_targets{};
};

struct PostSubgraphTargets
{
    ResourceVersion temporal;
    ResourceVersion bloom_half;
    ResourceVersion bloom_quarter;
    ResourceVersion bloom_half_combined;
    ResourceVersion post_a;
    ResourceVersion post_b;
    ResourceVersion back_buffer;
    ResourceVersion temporal_normal;
    ResourceVersion temporal_material;
    ResourceVersion display_linear;
};

struct PostSubgraphOutputs
{
    ResourceVersion temporal;
    ResourceVersion bloom_half;
    ResourceVersion bloom_quarter;
    ResourceVersion bloom_half_combined;
    ResourceVersion post_a;
    ResourceVersion post_b;
    ResourceVersion back_buffer;
    ResourceVersion temporal_normal;
    ResourceVersion temporal_material;
    ResourceVersion display_linear;
};

struct PostPassRead
{
    ResourceVersion version;
    Access access{Access::ShaderRead};
};

struct PostPassPlan
{
    std::uint32_t id{};
    std::string_view name;
    std::vector<PostPassRead> reads;
    ResourceVersion predecessor;
    ResourceVersion output;
    bool read_write{};
    std::vector<std::pair<ResourceVersion, ResourceVersion>> additional_outputs;
};

struct PostSubgraphPlan
{
    std::vector<PostPassPlan> passes;
    PostSubgraphOutputs outputs;
};

struct PostSubgraphLayout
{
    struct Pass { std::uint32_t declaration_index{}, id{}; };
    std::array<ResourceVersion, 34> inputs{};
    std::array<Pass, 9> passes{};
    PostSubgraphOutputs outputs{};
    std::uint32_t pass_count{};
    bool bloom{}, outline{}, fxaa_identity{}, initialize_post_targets{};
    bool pre_bloom{}, pre_tone_map{}, devtools_ui{};
};

[[nodiscard]] Result CompilePostSubgraphs(const PostSubgraphInputs &inputs,
    const PostSubgraphTargets &targets, PostSubgraphPlan &plan);
} // namespace hs
