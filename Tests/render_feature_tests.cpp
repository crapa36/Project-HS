#include "render_features.hpp"
#include "renderer_post_subgraphs.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace
{
using namespace hs;

void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

ResourceVersion Texture(std::uint32_t id, std::uint32_t version = 0)
{
    return {{id, ResourceKind::Texture}, version};
}

bool SameVersion(ResourceVersion left, ResourceVersion right)
{
    return left.resource.value == right.resource.value &&
           left.resource.kind == right.resource.kind && left.ordinal == right.ordinal;
}

const SignalMetadata hdr_metadata{ResourceKind::Texture, {640, 360},
    SignalEncoding::RgbaFloat, RadiometricMeaning::SceneLinear};
const SignalDeclaration source{{SignalSemantic::SceneHdr, 3, 0}, hdr_metadata};
const SignalDeclaration temporal{{SignalSemantic::TemporalHdr, 3, 1}, hdr_metadata};
const SignalDeclaration bloom{{SignalSemantic::BloomHdr, 3, 1}, hdr_metadata};

FeatureRegistration Feature(FeatureId id, std::vector<SignalDeclaration> inputs,
                            SignalDeclaration output, int &build_count)
{
    return {id, {}, std::move(inputs), {output},
        [id, output, &build_count](std::span<const SignalHandle>, std::vector<SignalHandle> &outputs) {
            ++build_count;
            outputs.push_back({output, id, {{100 + static_cast<std::uint32_t>(id), output.metadata.kind}, 1}});
            return Result::Success();
        }};
}

void CompositionValidation()
{
    const std::array imports{SignalHandle{source, FeatureId::Imported, Texture(1)}};
    int build_count{};
    std::array registrations{
        Feature(FeatureId::Bloom, {temporal}, bloom, build_count),
        Feature(FeatureId::Temporal, {source}, temporal, build_count),
    };
    FeatureComposition result;
    Check(ComposeRenderFeatures(imports, registrations, result).Succeeded(), "compose reversed registrations");
    Check(result.order == std::vector{FeatureId::Temporal, FeatureId::Bloom} && build_count == 2,
          "signal dependency controls feature order");

    auto rejected = [&](std::vector<FeatureRegistration> invalid, const char *message) {
        build_count = 0;
        Check(!ComposeRenderFeatures(imports, invalid, result), message);
        Check(build_count == 0 && result.signals.empty() && result.order.empty(),
              "invalid declarations fail before building and clear stale composition");
    };
    rejected({Feature(FeatureId::Temporal, {source}, temporal, build_count),
              Feature(FeatureId::Bloom, {source}, temporal, build_count)}, "reject duplicate semantic producer");
    rejected({Feature(FeatureId::Temporal, {source}, source, build_count)}, "reject producer conflicting with import");
    rejected({Feature(FeatureId::Bloom, {temporal}, bloom, build_count)}, "reject missing required producer");
    rejected({Feature(FeatureId::Temporal, {bloom}, temporal, build_count),
              Feature(FeatureId::Bloom, {temporal}, bloom, build_count)}, "reject signal dependency cycle");
    rejected({Feature(FeatureId::Temporal, {source}, temporal, build_count),
              Feature(FeatureId::Temporal, {source}, bloom, build_count)}, "reject duplicate stable feature ID");

    for (unsigned mutation = 0; mutation != 7; ++mutation)
    {
        auto incompatible = source;
        switch (mutation)
        {
        case 0: ++incompatible.metadata.extent.width; break;
        case 1: incompatible.metadata.encoding = SignalEncoding::RgbaUnorm; break;
        case 2: incompatible.metadata.radiometric = RadiometricMeaning::DisplayEncoded; break;
        case 3: ++incompatible.key.view; break;
        case 4: ++incompatible.key.version; break;
        case 5: ++incompatible.metadata.layers; break;
        case 6: incompatible.metadata.filtering = SignalFiltering::Noisy; break;
        }
        rejected({Feature(FeatureId::Temporal, {incompatible}, temporal, build_count)},
                 "reject extent, encoding, radiometric, view or version mismatch");
    }

    auto disabled = Feature(FeatureId::Bloom, {temporal}, temporal, build_count);
    disabled.predicate = [] { return false; };
    disabled.build = {};
    std::array optional{Feature(FeatureId::Temporal, {source}, temporal, build_count), std::move(disabled)};
    build_count = 0;
    Check(ComposeRenderFeatures(imports, optional, result).Succeeded() && build_count == 1,
          "disabled registration neither requires signals nor claims a producer");

    auto dishonest = Feature(FeatureId::Temporal, {source}, temporal, build_count);
    dishonest.build = [&](std::span<const SignalHandle>, std::vector<SignalHandle> &outputs) {
        auto wrong = temporal;
        wrong.metadata.extent.height = 1;
        outputs.push_back({wrong, FeatureId::Temporal, Texture(2, 1)});
        return Result::Success();
    };
    const std::array dishonest_registration{std::move(dishonest)};
    Check(!ComposeRenderFeatures(imports, dishonest_registration, result) && result.signals.empty(),
          "reject build result that disagrees with declared signal");

    const SignalDeclaration depth{{SignalSemantic::Depth, 3, 0},
        {ResourceKind::Texture, {640, 360}, SignalEncoding::Depth, RadiometricMeaning::None}, SignalUsage::Guide};
    const SignalDeclaration history{{SignalSemantic::SurfaceHistory, 3, 0},
        {ResourceKind::Texture, {640, 360}, SignalEncoding::SurfaceHistory, RadiometricMeaning::None}, SignalUsage::History};
    const SignalDeclaration noisy{{SignalSemantic::NoisyDiffuseRadiance, 3, 1},
        {ResourceKind::Texture, {320, 180}, SignalEncoding::RgbaFloat, RadiometricMeaning::SceneLinear, 1, SignalFiltering::Noisy}};
    const SignalDeclaration clean{{SignalSemantic::DiffuseRadiance, 3, 1},
        {ResourceKind::Texture, {640, 360}, SignalEncoding::RgbaFloat, RadiometricMeaning::SceneLinear, 1, SignalFiltering::Resolved}};
    const std::array lighting_imports{SignalHandle{depth, FeatureId::Imported, Texture(31, 2)},
        SignalHandle{history, FeatureId::Imported, Texture(32, 4)}};
    auto trace = Feature(FeatureId::DiffuseTransport, {depth}, noisy, build_count);
    auto denoise = Feature(FeatureId::DiffuseDenoising, {noisy, depth, history}, clean, build_count);
    denoise.build = [&](std::span<const SignalHandle> inputs, std::vector<SignalHandle> &outputs) {
        ++build_count;
        Check(inputs.size() == 3 && inputs[0].producer == FeatureId::DiffuseTransport &&
            SameVersion(inputs[1].resource, Texture(31, 2)) && inputs[1].declaration.usage == SignalUsage::Guide &&
            SameVersion(inputs[2].resource, Texture(32, 4)) && inputs[2].declaration.usage == SignalUsage::History,
            "denoising receives the actual noisy producer and resolved guide/history versions");
        outputs.push_back({clean, FeatureId::DiffuseDenoising, Texture(33, 1)});
        return Result::Success();
    };
    const std::array lighting_features{denoise, trace};
    build_count = 0;
    Check(ComposeRenderFeatures(lighting_imports, lighting_features, result).Succeeded() && build_count == 2 &&
        result.order == std::vector{FeatureId::DiffuseTransport, FeatureId::DiffuseDenoising},
        "concrete transport/denoising order is established by noisy-signal dependencies");
    build_count = 0;
    Check(!ComposeRenderFeatures(std::span(lighting_imports).first(1), lighting_features, result) && build_count == 0,
        "missing history producer fails before transport or native denoising mutates state");
    auto wrong_guide = denoise;
    ++wrong_guide.inputs[1].metadata.extent.width;
    const std::array invalid_lighting{trace, wrong_guide};
    build_count = 0;
    Check(!ComposeRenderFeatures(lighting_imports, invalid_lighting, result) && build_count == 0,
        "denoising guide metadata is validated before any feature build");

    auto buffer_source = source;
    buffer_source.metadata.kind = ResourceKind::Buffer;
    buffer_source.metadata.extent = {32, 1};
    buffer_source.metadata.encoding = SignalEncoding::Metadata;
    buffer_source.metadata.radiometric = RadiometricMeaning::None;
    auto buffer_output = buffer_source;
    buffer_output.key = temporal.key;
    const std::array buffer_imports{SignalHandle{buffer_source, FeatureId::Imported,
        {{1, ResourceKind::Buffer}, 0}}};
    const std::array buffer_features{Feature(FeatureId::Temporal, {buffer_source}, buffer_output, build_count)};
    Check(ComposeRenderFeatures(buffer_imports, buffer_features, result).Succeeded(),
          "semantic signals can refer to buffers");
}

void LargeSignalComposition()
{
    std::vector<SignalHandle> imports;
    std::vector<SignalDeclaration> required;
    constexpr std::uint32_t views = 128;
    constexpr std::uint32_t versions = 64;
    imports.reserve(2 * views * versions);
    required.reserve(2 * views * versions);
    for (const auto semantic : {SignalSemantic::SceneHdr, SignalSemantic::CompositeHdr})
        for (std::uint32_t view = 0; view < views; ++view)
            for (std::uint32_t version = 0; version < versions; ++version)
            {
                const SignalDeclaration declaration{{semantic, view, version}, hdr_metadata};
                imports.push_back({declaration, FeatureId::Imported,
                    Texture(static_cast<std::uint32_t>(imports.size()) + 1, version)});
                required.push_back(declaration);
            }
    required.back().usage = SignalUsage::History;
    int build_count{};
    int predicate_count{};
    auto resolve = Feature(FeatureId::Temporal, std::move(required), temporal, build_count);
    resolve.predicate = [&] { ++predicate_count; return true; };
    resolve.build = [&](std::span<const SignalHandle> inputs, std::vector<SignalHandle> &outputs) {
        ++build_count;
        Check(inputs.size() == imports.size(), "large composition resolves every required signal");
        for (std::size_t index = 0; index < inputs.size(); ++index)
            Check(inputs[index].declaration.key == imports[index].declaration.key &&
                SameVersion(inputs[index].resource, imports[index].resource),
                "large signal keys retain semantic, view, version and required input order");
        Check(inputs.back().declaration.usage == SignalUsage::History,
              "large composition preserves required history usage");
        outputs.push_back({temporal, FeatureId::Temporal, Texture(20000, 1)});
        return Result::Success();
    };
    auto consume = Feature(FeatureId::Bloom, {temporal}, bloom, build_count);
    consume.build = [&](std::span<const SignalHandle> inputs, std::vector<SignalHandle> &outputs) {
        ++build_count;
        Check(inputs.size() == 1 && SameVersion(inputs[0].resource, Texture(20000, 1)),
              "large composition resolves a newly built producer after signal storage grows");
        outputs.push_back({bloom, FeatureId::Bloom, Texture(20001, 1)});
        return Result::Success();
    };
    const std::array registrations{std::move(consume), std::move(resolve)};
    FeatureComposition result;
    Check(ComposeRenderFeatures(imports, registrations, result).Succeeded() && build_count == 2 &&
        predicate_count == 1 && result.signals.size() == imports.size() + 2 &&
        result.order == std::vector{FeatureId::Temporal, FeatureId::Bloom},
        "large unique signal set composes in deterministic dependency order");
    for (std::size_t index = 0; index < imports.size(); ++index)
        Check(result.signals[index].declaration.key == imports[index].declaration.key,
              "large composition keeps imported signals in canonical declaration order");
    imports.push_back(imports.back());
    build_count = predicate_count = 0;
    Check(!ComposeRenderFeatures(imports, registrations, result) && build_count == 0 &&
        predicate_count == 0 && result.signals.empty() && result.order.empty(),
        "late duplicate in a large signal set is rejected before predicates or builds and clears composition");
}

struct PostFixture
{
    PostSubgraphInputs inputs{7, {641, 361}, {1280, 720}, {1920, 1080}, false, false,
        Texture(1), Texture(2), Texture(3), Texture(4), Texture(5), Texture(6),
        Texture(7), Texture(8), Texture(9)};
    PostSubgraphTargets targets{Texture(10), Texture(11), Texture(12), Texture(13),
        Texture(2), Texture(14), Texture(15)};
};

const PostPassPlan &Pass(const PostSubgraphPlan &plan, std::uint32_t id)
{
    const auto found = std::ranges::find(plan.passes, id, &PostPassPlan::id);
    Check(found != plan.passes.end(), "expected configured post pass");
    return *found;
}

void ConfiguredPostTopology()
{
    for (const bool enable_bloom : {false, true})
        for (const bool enable_outline : {false, true})
        {
            PostFixture fixture;
            fixture.inputs.bloom = enable_bloom;
            fixture.inputs.outline = enable_outline;
            PostSubgraphPlan plan;
            Check(CompilePostSubgraphs(fixture.inputs, fixture.targets, plan).Succeeded(), "compile post topology");
            Check(plan.passes.size() == 4u + (enable_bloom ? 4u : 0u) + (enable_outline ? 1u : 0u),
                  "disabled features omit their actual GPU passes");
            for (std::uint32_t id = 10; id <= 13; ++id)
                Check((std::ranges::find(plan.passes, id, &PostPassPlan::id) != plan.passes.end()) == enable_bloom,
                      "bloom subgraph is present only when enabled");
            Check((std::ranges::find(plan.passes, 15u, &PostPassPlan::id) != plan.passes.end()) == enable_outline,
                  "outline subgraph is present only when enabled");
            const auto tone_input = enable_bloom ? Pass(plan, 13).output : Pass(plan, 9).output;
            Check(Pass(plan, 14).reads.size() == 1 && SameVersion(Pass(plan, 14).reads[0].version, tone_input),
                  "tone mapping consumes bloom output or bypasses to temporal output");
            const auto aa_input = enable_outline ? Pass(plan, 15).output : Pass(plan, 14).output;
            Check(Pass(plan, 16).reads.size() == 1 && SameVersion(Pass(plan, 16).reads[0].version, aa_input),
                  "FXAA consumes outline output or bypasses to tone mapping");
            Check(Pass(plan, 17).read_write && SameVersion(Pass(plan, 17).predecessor, Pass(plan, 16).output),
                  "UI loads and blends into the produced back buffer version");
            Check(SameVersion(plan.outputs.temporal, Texture(10, 1)) &&
                  SameVersion(plan.outputs.post_a, Texture(2, 1)) &&
                  SameVersion(plan.outputs.post_b, Texture(14, static_cast<std::uint32_t>(enable_bloom) +
                                                             static_cast<std::uint32_t>(enable_outline))) &&
                  SameVersion(plan.outputs.back_buffer, Texture(15, 2)),
                  "post outputs preserve physical ping-pong targets and final versions");
            const auto display = std::ranges::find_if(plan.features.signals, [&](const auto &signal) {
                return signal.declaration.key == SignalKey{SignalSemantic::BackBufferColor, fixture.inputs.view, 2};
            });
            Check(display != plan.features.signals.end() && display->producer == FeatureId::Ui &&
                  display->declaration.metadata.extent == fixture.inputs.output_extent &&
                  display->declaration.metadata.radiometric == RadiometricMeaning::DisplayEncoded,
                  "final view signal carries actual output extent and color meaning");
        }

    PostFixture fixture;
    fixture.inputs.bloom = true;
    fixture.targets.temporal.ordinal = 4;
    fixture.targets.post_a.ordinal = 3;
    fixture.inputs.composite_hdr.ordinal = 3;
    fixture.targets.post_b.ordinal = 5;
    fixture.targets.back_buffer.ordinal = 7;
    PostSubgraphPlan plan;
    Check(CompilePostSubgraphs(fixture.inputs, fixture.targets, plan).Succeeded(), "continue existing resource versions");
    Check(plan.outputs.temporal.ordinal == 5 && plan.outputs.post_a.ordinal == 4 &&
          plan.outputs.post_b.ordinal == 6 && plan.outputs.back_buffer.ordinal == 9,
          "post composition continues prior frame graph writes without resetting ordinals");
    fixture.targets.temporal.ordinal = UINT32_MAX;
    Check(!CompilePostSubgraphs(fixture.inputs, fixture.targets, plan) && plan.passes.empty(),
          "invalid version cannot publish a partial post plan");
    fixture.targets.temporal.ordinal = 0;
    fixture.targets.post_a.ordinal = 2;
    Check(!CompilePostSubgraphs(fixture.inputs, fixture.targets, plan),
          "post target cannot disconnect the physical composite version");
    fixture.targets.post_a.ordinal = fixture.inputs.composite_hdr.ordinal;
    fixture.inputs.depth.resource = {};
    Check(!CompilePostSubgraphs(fixture.inputs, fixture.targets, plan), "required guide needs an actual resource");
}
} // namespace

int main()
{
    try
    {
        CompositionValidation();
        LargeSignalComposition();
        ConfiguredPostTopology();
        std::puts("Render feature signal and configured post topology checks passed.");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
