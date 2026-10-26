#pragma once

#include "render_graph.hpp"

#include <array>
#include <functional>

namespace hs
{
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
    // Already produced by the external reconstruction graph: output-resolution
    // radiance multiplied by current exposure. Bypasses native pass 9 and its
    // normal/material history writes; subsequent post features consume it.
    ResourceVersion external_temporal;
    std::array<ResourceVersion, 7> transparent_guides{};
    // Native lowering hooks run during graph construction. Temporal ownership
    // remains unchanged; only subsequent HDR reads consume their replacements.
    PostHdrHook pre_bloom, pre_tone_map;
    // Encoded ImGui reuses post_a after its tone-map/FXAA consumers finish.
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

// This CPU plan is also the production lowering input: tests inspect the same
// configured reads, writes and bypasses that BuildPostSubgraphs gives the graph.
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

using PostPassCallback = std::move_only_function<void(RenderPassContext &)>;
// Index zero corresponds to stable pass ID 9, through ID 17 at index eight.
using PostPassCallbacks = std::array<PostPassCallback, 9>;

[[nodiscard]] Result CompilePostSubgraphs(const PostSubgraphInputs &inputs,
                                          const PostSubgraphTargets &targets,
                                          PostSubgraphPlan &plan);
[[nodiscard]] Result BuildPostSubgraphs(RenderGraphBuilder &graph,
                                       const PostSubgraphInputs &inputs,
                                       const PostSubgraphTargets &targets,
                                       PostPassCallbacks callbacks,
                                       PostSubgraphOutputs &outputs,
                                       PostSubgraphLayout *layout = nullptr);
[[nodiscard]] bool MatchesPostSubgraphLayout(const PostSubgraphInputs &inputs,
    const PostSubgraphTargets &targets, const PostSubgraphLayout &layout) noexcept;
[[nodiscard]] Result BindRetainedPostSubgraphs(RenderGraphBuilder &graph,
    const PostSubgraphInputs &inputs, const PostSubgraphTargets &targets,
    const PostSubgraphLayout &layout, PostPassCallbacks callbacks,
    PostSubgraphOutputs &outputs);
} // namespace hs
