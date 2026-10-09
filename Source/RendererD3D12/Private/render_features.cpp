#include "render_features.hpp"
#include "renderer_post_subgraphs.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <map>
#include <tuple>
#include <utility>

namespace hs
{
namespace
{
Result FeatureFailure(const char *message)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_features", message);
}

bool ValidMetadata(const SignalMetadata &metadata)
{
    return (metadata.kind == ResourceKind::Texture || metadata.kind == ResourceKind::Buffer) &&
           metadata.extent.width != 0 && metadata.extent.height != 0 && metadata.layers != 0 &&
           static_cast<unsigned>(metadata.encoding) <= static_cast<unsigned>(SignalEncoding::AccelerationStructure) &&
           static_cast<unsigned>(metadata.filtering) <= static_cast<unsigned>(SignalFiltering::Resolved) &&
           static_cast<unsigned>(metadata.radiometric) <= static_cast<unsigned>(RadiometricMeaning::DisplayEncoded);
}

bool ValidSignal(const SignalHandle &signal)
{
    return signal.resource.resource &&
           signal.resource.resource.kind == signal.declaration.metadata.kind &&
           ValidMetadata(signal.declaration.metadata);
}
} // namespace

Result ComposeRenderFeatures(std::span<const SignalHandle> imported,
                             std::span<const FeatureRegistration> registrations,
                             FeatureComposition &composition)
{
    composition = {};
    struct Producer
    {
        SignalDeclaration declaration;
        std::size_t feature;
    };
    using IndexedSignalKey = std::tuple<SignalSemantic, std::uint32_t, std::uint32_t>;
    const auto signal_key = [](const SignalKey &key) {
        return IndexedSignalKey{key.semantic, key.view, key.version};
    };
    const auto imported_feature = registrations.size();
    std::vector<Producer> producers;
    std::map<IndexedSignalKey, std::size_t> producer_indices;
    for (const auto &signal : imported)
    {
        if (!ValidSignal(signal)) return FeatureFailure("Imported feature signal is invalid.");
        if (!producer_indices.emplace(signal_key(signal.declaration.key), producers.size()).second)
            return FeatureFailure("A feature signal has duplicate producers.");
        producers.push_back({signal.declaration, imported_feature});
    }
    std::vector<bool> enabled(registrations.size());
    for (std::size_t index = 0; index < registrations.size(); ++index)
    {
        const auto &feature = registrations[index];
        enabled[index] = !feature.predicate || feature.predicate();
        if (!enabled[index]) continue;
        if (feature.id == FeatureId::Imported || !feature.build)
            return FeatureFailure("Enabled feature requires an ID and build function.");
        for (std::size_t previous = 0; previous < index; ++previous)
            if (enabled[previous] && registrations[previous].id == feature.id)
                return FeatureFailure("Enabled feature IDs must be unique.");
        for (const auto &output : feature.outputs)
        {
            if (!ValidMetadata(output.metadata))
                return FeatureFailure("Produced feature signal metadata is invalid.");
            if (!producer_indices.emplace(signal_key(output.key), producers.size()).second)
                return FeatureFailure("A feature signal has duplicate producers.");
            producers.push_back({output, index});
        }
    }
    std::vector<std::vector<bool>> edges(registrations.size(),
                                         std::vector<bool>(registrations.size()));
    std::vector<std::size_t> indegree(registrations.size());
    for (std::size_t index = 0; index < registrations.size(); ++index)
    {
        if (!enabled[index]) continue;
        for (const auto &input : registrations[index].inputs)
        {
            if (!ValidMetadata(input.metadata) ||
                static_cast<unsigned>(input.usage) > static_cast<unsigned>(SignalUsage::History))
                return FeatureFailure("Required feature guide/history declaration is invalid.");
            const auto found = producer_indices.find(signal_key(input.key));
            if (found == producer_indices.end()) return FeatureFailure("Required feature signal has no producer.");
            const auto &producer = producers[found->second];
            if (producer.declaration.metadata != input.metadata)
                return FeatureFailure("Required feature signal extent, encoding or meaning disagrees.");
            if (producer.feature == imported_feature) continue;
            if (!edges[producer.feature][index])
            {
                edges[producer.feature][index] = true;
                ++indegree[index];
            }
        }
    }
    std::vector<std::size_t> order;
    const auto count = static_cast<std::size_t>(std::ranges::count(enabled, true));
    for (std::size_t position = 0; position < count; ++position)
    {
        auto ready = registrations.size();
        for (std::size_t index = 0; index < registrations.size(); ++index)
            if (enabled[index] && indegree[index] == 0 &&
                (ready == registrations.size() || registrations[index].id < registrations[ready].id))
                ready = index;
        if (ready == registrations.size()) return FeatureFailure("Feature signals contain a dependency cycle.");
        order.push_back(ready);
        indegree[ready] = std::numeric_limits<std::size_t>::max();
        for (std::size_t next = 0; next < registrations.size(); ++next)
            if (edges[ready][next]) --indegree[next];
    }

    FeatureComposition candidate;
    candidate.signals.assign(imported.begin(), imported.end());
    std::map<IndexedSignalKey, std::size_t> signal_indices;
    for (std::size_t index = 0; index < candidate.signals.size(); ++index)
        signal_indices.emplace(signal_key(candidate.signals[index].declaration.key), index);
    for (const auto index : order)
    {
        const auto &feature = registrations[index];
        std::vector<SignalHandle> inputs;
        for (const auto &required : feature.inputs)
        {
            const auto found = signal_indices.find(signal_key(required.key));
            inputs.push_back(candidate.signals[found->second]);
            inputs.back().declaration.usage = required.usage;
        }
        std::vector<SignalHandle> outputs;
        if (auto built = feature.build(inputs, outputs); !built) return built;
        if (outputs.size() != feature.outputs.size())
            return FeatureFailure("Feature build returned an unexpected signal count.");
        for (std::size_t output = 0; output < outputs.size(); ++output)
        {
            const auto &signal = outputs[output];
            if (!ValidSignal(signal) || signal.producer != feature.id ||
                signal.declaration.key != feature.outputs[output].key ||
                signal.declaration.metadata != feature.outputs[output].metadata)
                return FeatureFailure("Feature build returned a signal that disagrees with its declaration.");
            signal_indices.emplace(signal_key(signal.declaration.key), candidate.signals.size());
            candidate.signals.push_back(signal);
        }
        candidate.order.push_back(feature.id);
    }
    composition = std::move(candidate);
    return Result::Success();
}

// Keep the production post declaration compiler in this D3D-free translation
// unit. The native lowering lives in renderer_post_subgraphs.cpp.
Result CompilePostSubgraphs(const PostSubgraphInputs &inputs,
                            const PostSubgraphTargets &targets,
                            PostSubgraphPlan &plan)
{
    plan = {};
    if (inputs.composite_hdr.resource.value == targets.post_a.resource.value &&
        inputs.composite_hdr.resource.kind == targets.post_a.resource.kind &&
        inputs.composite_hdr.ordinal != targets.post_a.ordinal)
        return FeatureFailure("Post target must continue the current physical composite version.");
    PostSubgraphPlan candidate;
    candidate.outputs = {targets.temporal, targets.bloom_half, targets.bloom_quarter,
                         targets.bloom_half_combined, targets.post_a, targets.post_b,
                         targets.back_buffer, targets.temporal_normal,
                         targets.temporal_material, targets.display_linear};
    const auto metadata = [](SignalExtent extent, SignalEncoding encoding,
                             RadiometricMeaning meaning = RadiometricMeaning::None) {
        return SignalMetadata{ResourceKind::Texture, extent, encoding, meaning};
    };
    const auto hdr_metadata = metadata(inputs.render_extent, SignalEncoding::RgbaFloat,
                                       RadiometricMeaning::SceneLinear);
    auto temporal_metadata = metadata(inputs.output_extent, SignalEncoding::RgbaFloat,
                                      RadiometricMeaning::PreExposedSceneLinear);
    temporal_metadata.filtering = SignalFiltering::Resolved;
    const auto composite_metadata = metadata(inputs.render_extent, SignalEncoding::RgbaFloat,
        inputs.exposure.resource ? RadiometricMeaning::PreExposedSceneLinear : RadiometricMeaning::SceneLinear);
    const auto display_metadata = metadata(inputs.output_extent, SignalEncoding::RgbaFloat,
                                           RadiometricMeaning::DisplayLinear);
    const auto output_metadata = metadata(inputs.output_extent, SignalEncoding::RgbaUnorm,
                                          RadiometricMeaning::DisplayEncoded);
    const auto ui_metadata = metadata(inputs.ui_extent, SignalEncoding::RgbaUnorm,
                                      RadiometricMeaning::DisplayEncoded);
    const auto declaration = [&](SignalSemantic semantic, const SignalMetadata &description,
                                  std::uint32_t version = 0) {
        return SignalDeclaration{{semantic, inputs.view, version}, description};
    };
    const std::array imports{
        SignalHandle{declaration(SignalSemantic::SceneHdr, hdr_metadata), FeatureId::Imported, inputs.scene_hdr},
        SignalHandle{declaration(SignalSemantic::CompositeHdr, composite_metadata), FeatureId::Imported, inputs.composite_hdr},
        SignalHandle{declaration(SignalSemantic::Depth, metadata(inputs.render_extent, SignalEncoding::Depth)), FeatureId::Imported, inputs.depth},
        SignalHandle{declaration(SignalSemantic::Normal, metadata(inputs.render_extent, SignalEncoding::Normal)), FeatureId::Imported, inputs.normal},
        SignalHandle{declaration(SignalSemantic::SurfaceMetadata, metadata(inputs.render_extent, SignalEncoding::Metadata)), FeatureId::Imported, inputs.metadata},
        SignalHandle{declaration(SignalSemantic::Motion, metadata(inputs.render_extent, SignalEncoding::Motion)), FeatureId::Imported, inputs.motion},
        SignalHandle{declaration(SignalSemantic::SurfaceHistory, metadata(inputs.render_extent, SignalEncoding::SurfaceHistory)), FeatureId::Imported, inputs.surface_history},
        SignalHandle{declaration(SignalSemantic::TemporalHistory, temporal_metadata), FeatureId::Imported, inputs.temporal_history},
        SignalHandle{declaration(SignalSemantic::UiColor, ui_metadata), FeatureId::Imported, inputs.ui},
    };
    const auto next_pass = [&](std::uint32_t id, std::string_view name,
                               std::initializer_list<ResourceVersion> reads,
                               ResourceVersion &destination, bool read_write = false) -> Result {
        if (!destination.resource || destination.resource.kind != ResourceKind::Texture ||
            destination.ordinal == UINT32_MAX)
            return FeatureFailure("Post subgraph target is invalid or has no next resource version.");
        PostPassPlan pass;
        pass.id = id;
        pass.name = name;
        for (const auto read : reads) pass.reads.push_back({read, Access::ShaderRead});
        pass.predecessor = destination;
        ++destination.ordinal;
        pass.output = destination;
        pass.read_write = read_write;
        candidate.passes.push_back(std::move(pass));
        return Result::Success();
    };
    const auto produce = [](std::vector<SignalHandle> &signals, FeatureId feature,
                             SignalDeclaration description, ResourceVersion resource) {
        signals.push_back({description, feature, resource});
        return Result::Success();
    };
    const auto temporal_output = declaration(SignalSemantic::TemporalHdr, temporal_metadata, 1);
    const bool external_temporal = static_cast<bool>(inputs.external_temporal.resource);
    std::vector<SignalHandle> composition_imports;
    for (std::size_t index = 0; index < imports.size(); ++index)
    {
        // External reconstruction already consumed its own guides. Native
        // history/normal/motion signals need not exist in this post branch.
        if (external_temporal && (index == 3 || index == 5 || index == 6 || index == 7)) continue;
        composition_imports.push_back(imports[index]);
    }
    if (external_temporal)
    {
        candidate.outputs.temporal = inputs.external_temporal;
        auto external = SignalHandle{temporal_output, FeatureId::Imported, inputs.external_temporal};
        if (inputs.external_temporal_signal.resource.resource)
        {
            const auto &signal = inputs.external_temporal_signal;
            if (!ValidSignal(signal) || signal.declaration.key != temporal_output.key ||
                signal.declaration.metadata != temporal_output.metadata ||
                signal.resource.resource.value != inputs.external_temporal.resource.value ||
                signal.resource.resource.kind != inputs.external_temporal.resource.kind ||
                signal.resource.ordinal != inputs.external_temporal.ordinal)
                return FeatureFailure("Reconstruction provider signal disagrees with the post input.");
            external = signal;
        }
        composition_imports.push_back(external);
    }
    const auto bloom_output = declaration(SignalSemantic::BloomHdr, temporal_metadata, 1);
    const auto tone_output = declaration(SignalSemantic::ToneMappedColor, display_metadata, 1);
    const auto outline_output = declaration(SignalSemantic::OutlinedColor, display_metadata, 1);
    const bool linear_ui = static_cast<bool>(targets.display_linear.resource);
    const auto aa_output = declaration(linear_ui ? SignalSemantic::AntialiasedColor
                                                : SignalSemantic::BackBufferColor,
                                      linear_ui ? display_metadata : output_metadata, 1);
    const auto ui_output = declaration(SignalSemantic::BackBufferColor, output_metadata, 2);

    std::vector<FeatureRegistration> registrations;
    const auto guide = [](SignalDeclaration declaration, bool history = false) {
        declaration.usage = history ? SignalUsage::History : SignalUsage::Guide;
        return declaration;
    };
    registrations.push_back({FeatureId::Temporal, [&] { return !external_temporal; },
        {imports[1].declaration, imports[0].declaration, guide(imports[4].declaration),
         guide(imports[3].declaration), guide(imports[5].declaration), guide(imports[6].declaration, true),
         guide(imports[2].declaration), guide(imports[7].declaration, true)}, {temporal_output},
        [&](std::span<const SignalHandle> signals, std::vector<SignalHandle> &outputs) {
            if (auto added = next_pass(9, "Temporal Resolve",
                    {signals[0].resource, signals[1].resource, signals[2].resource,
                     signals[3].resource, signals[4].resource, signals[5].resource,
                     signals[6].resource, signals[7].resource}, candidate.outputs.temporal); !added)
                return added;
            auto &pass = candidate.passes.back();
            if (inputs.reactive_composite_hdr.resource)
                pass.reads.push_back({inputs.reactive_composite_hdr, Access::ShaderRead});
            for (const auto version : {inputs.temporal_normal_history, inputs.temporal_material_history})
                if (version.resource) pass.reads.push_back({version, Access::ShaderRead});
            if (inputs.exposure.resource) pass.reads.push_back({inputs.exposure, Access::ShaderRead});
            for (const auto guide : inputs.transparent_guides)
                if (guide.resource) pass.reads.push_back({guide, Access::ShaderRead});
            for (auto *version : {&candidate.outputs.temporal_normal, &candidate.outputs.temporal_material})
                if (version->resource)
                {
                    const auto predecessor = *version;
                    ++version->ordinal;
                    pass.additional_outputs.emplace_back(predecessor, *version);
                }
            return produce(outputs, FeatureId::Temporal, temporal_output, candidate.outputs.temporal);
        }});
    registrations.push_back({FeatureId::Bloom, [&] { return inputs.bloom; },
        {imports[1].declaration, temporal_output}, {bloom_output},
        [&](std::span<const SignalHandle> signals, std::vector<SignalHandle> &outputs) {
            if (auto added = next_pass(10, "Bloom Half Extract", {signals[0].resource, signals[1].resource},
                                      candidate.outputs.bloom_half); !added) return added;
            if (auto added = next_pass(11, "Bloom Quarter Downsample", {candidate.outputs.bloom_half},
                                      candidate.outputs.bloom_quarter); !added) return added;
            if (auto added = next_pass(12, "Bloom Half Upsample",
                    {candidate.outputs.bloom_half, candidate.outputs.bloom_quarter},
                    candidate.outputs.bloom_half_combined); !added) return added;
            if (auto added = next_pass(13, "Bloom",
                    {signals[0].resource, signals[1].resource, candidate.outputs.bloom_half_combined},
                    candidate.outputs.post_b); !added) return added;
            return produce(outputs, FeatureId::Bloom, bloom_output, candidate.outputs.post_b);
        }});
    registrations.push_back({FeatureId::ToneMap, {},
        {inputs.bloom ? bloom_output : temporal_output}, {tone_output},
        [&](std::span<const SignalHandle> signals, std::vector<SignalHandle> &outputs) {
            if (auto added = next_pass(14, "ToneMap", {signals[0].resource}, candidate.outputs.post_a); !added)
                return added;
            return produce(outputs, FeatureId::ToneMap, tone_output, candidate.outputs.post_a);
        }});
    registrations.push_back({FeatureId::Outline, [&] { return inputs.outline; },
        {tone_output, imports[4].declaration, imports[2].declaration}, {outline_output},
        [&](std::span<const SignalHandle> signals, std::vector<SignalHandle> &outputs) {
            if (auto added = next_pass(15, "Screen-space Outline",
                    {signals[0].resource, signals[1].resource, signals[2].resource},
                    candidate.outputs.post_b); !added) return added;
            return produce(outputs, FeatureId::Outline, outline_output, candidate.outputs.post_b);
        }});
    registrations.push_back({FeatureId::Fxaa, {},
        {inputs.outline ? outline_output : tone_output}, {aa_output},
        [&](std::span<const SignalHandle> signals, std::vector<SignalHandle> &outputs) {
            auto &destination = linear_ui ? candidate.outputs.display_linear : candidate.outputs.back_buffer;
            if (auto added = next_pass(16, "FXAA", {signals[0].resource}, destination); !added)
                return added;
            return produce(outputs, FeatureId::Fxaa, aa_output, destination);
        }});
    registrations.push_back({FeatureId::Ui, {}, {imports[8].declaration, aa_output}, {ui_output},
        [&](std::span<const SignalHandle> signals, std::vector<SignalHandle> &outputs) {
            // The color input is consumed by ReadWrite on the back buffer.
            if (auto added = next_pass(17, "Game UI", {signals[0].resource},
                                      candidate.outputs.back_buffer, !linear_ui); !added) return added;
            if (linear_ui) candidate.passes.back().reads.push_back({signals[1].resource, Access::ShaderRead});
            return produce(outputs, FeatureId::Ui, ui_output, candidate.outputs.back_buffer);
        }});
    if (auto composed = ComposeRenderFeatures(composition_imports, registrations, candidate.features); !composed)
        return composed;
    plan = std::move(candidate);
    return Result::Success();
}
} // namespace hs
