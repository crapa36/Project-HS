#include "../AssetPipeline/action.hpp"
#include "../RenderMaterialShader/material_graph_parser.hpp"

#include <hs/render_data/character_package.hpp>
#include <hs/render_data/geometry_package.hpp>
#include <hs/render_data/material_ir.hpp>
#include <hs/render_data/opacity_micromap_bake.hpp>
#include <hs/render_data/scene_hlod.hpp>
#include <hs/render_data/texture_package.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace ap = hs::asset_pipeline;
namespace rd = hs::render_data;
namespace fs = std::filesystem;
using Bytes = std::vector<std::byte>;
constexpr std::string_view kNamespace = "render/ray/omm";
constexpr std::uint32_t kCutoffBits = 0x3f000000u;
constexpr std::uint32_t kUnknownMaterial = UINT32_MAX;
constexpr std::array kCharacters{
    std::pair{"archer", "archer"}, std::pair{"slime-melee", "enemy_melee"},
    std::pair{"slime-ranged", "enemy_ranged"}, std::pair{"slime-suicide", "enemy_suicide"},
    std::pair{"slime-projectile", "enemy_gel_projectile"}, std::pair{"boss-5m", "boss_5m"},
    std::pair{"boss-10m", "boss_10m"}, std::pair{"boss-final", "boss_final"}};
constexpr std::array<std::string_view, 19> kConstructorInputs{
    "Schemas/material_graph.schema.json", "Source/Apps/RenderOpacityMicromap/main.cpp",
    "Source/Apps/RenderMaterialShader/material_graph_parser.hpp",
    "Source/Apps/RenderMaterialShader/material_graph_parser.cpp",
    "Source/RenderData/Public/hs/render_data/opacity_micromap_package.hpp",
    "Source/RenderData/Private/opacity_micromap_package.cpp",
    "Source/RenderData/Public/hs/render_data/opacity_micromap_bake.hpp",
    "Source/RenderData/Private/opacity_micromap_bake.cpp",
    "Source/RenderData/Public/hs/render_data/material_ir.hpp", "Source/RenderData/Private/material_ir.cpp",
    "Content/Shaders/project_hs.hlsl", "Content/Shaders/ray_hit.hlsli",
    "Content/Shaders/ray_radiance.hlsli", "Content/Shaders/ray_native_trace.hlsli",
    "Content/Shaders/entity_surface.hlsli", "Content/Shaders/material_surface.hlsli",
    "Content/Shaders/hero_material.hlsli", "Content/Shaders/texture_streaming.hlsli",
    "Source/RendererD3D12/Private/renderer_view_resources.cpp"};

void Require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error("hs_render_opacity_micromap: " + std::string(message));
}
std::span<const std::byte> ByteSpan(std::string_view value)
{ return std::as_bytes(std::span(value.data(), value.size())); }
void Word(Bytes &bytes, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(std::byte((value >> (i * 8)) & 255u));
}
void Identity(Bytes &bytes, hs::AssetId value)
{ Word(bytes, std::uint32_t(value.value)); Word(bytes, std::uint32_t(value.value >> 32)); }
Bytes WordBytes(std::span<const std::uint32_t> words)
{
    Bytes bytes;
    bytes.reserve(words.size() * 4);
    for (auto word : words) Word(bytes, word);
    return bytes;
}
rd::OpacityMicromapDigest Digest(std::string_view value)
{
    if (value.starts_with("sha256:")) value.remove_prefix(7);
    Require(value.size() == 64, "SHA-256 digest length");
    const auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return unsigned(c - '0');
        if (c >= 'a' && c <= 'f') return unsigned(c - 'a') + 10;
        throw std::runtime_error("hs_render_opacity_micromap: noncanonical SHA-256 digest");
    };
    rd::OpacityMicromapDigest digest{};
    for (unsigned i = 0; i < 32; ++i)
        digest[i] = std::byte((nibble(value[i * 2]) << 4) | nibble(value[i * 2 + 1]));
    return digest;
}
std::string Configuration(std::uint32_t subdivision, std::uint64_t byte_budget)
{
    return ap::Json{{"subdivision", subdivision}, {"byte_budget_per_page", byte_budget},
        {"baker_abi", rd::kOpacityMicromapBakerAbi}, {"cutoff_bits", kCutoffBits},
        {"sampler_domain", "all-array-layers/all-logical-mips/linear-clamp-wrap/anisotropy1..16"}}.dump();
}
bool IsDigestText(std::string_view value)
{
    if (!value.starts_with("sha256:") || value.size() != 71) return false;
    return std::ranges::all_of(value.substr(7), [](const char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool IsMaterialName(std::string_view name)
{
    return std::ranges::find(rd::kEnvironmentMaterialNames, name) != rd::kEnvironmentMaterialNames.end() ||
           std::ranges::find(rd::kHeroMaterialNames, name) != rd::kHeroMaterialNames.end();
}
std::optional<fs::path> CanonicalInputPath(const fs::path &source, std::string_view logical_id)
{
    if (logical_id == "omm/coverage-configuration")
        return source / "Source/Apps/RenderOpacityMicromap/main.cpp";
    if (std::ranges::find(kConstructorInputs, logical_id) != kConstructorInputs.end())
        return source / fs::path(std::string(logical_id));
    constexpr std::string_view prefix = "ContentSource/Materials/";
    if (logical_id.starts_with(prefix) && logical_id.ends_with(".json"))
    {
        const auto name = logical_id.substr(prefix.size(), logical_id.size() - prefix.size() - 5);
        if (!name.empty() && name.find('/') == std::string_view::npos && IsMaterialName(name))
            return source / fs::path(std::string(logical_id));
    }
    return {};
}
std::optional<std::string> UpstreamNamespace(std::string_view logical_id)
{
    if (logical_id == "render/hlod/arena.shl1" || logical_id.starts_with("render/hlod/arena/"))
        return "render/hlod/arena";
    for (const auto &[mode, stem] : kCharacters)
    {
        if (logical_id == "render/character/" + std::string(stem) + ".rpkg" ||
            logical_id == "render/geometry/character/" + std::string(stem) + ".rpage")
            return "render/character/" + std::string(mode);
        const auto texture_prefix = "render/texture/" + std::string(stem) + "_";
        if (logical_id.starts_with(texture_prefix) && logical_id.ends_with(".rpkg"))
            return "render/texture/character/" + std::string(mode);
    }
    if (logical_id.starts_with("render/texture/monster_") && logical_id.ends_with(".rpkg"))
        return "render/texture/character/monster-materials";
    for (const auto name : rd::kEnvironmentMaterialNames)
    {
        if (logical_id == "render/" + std::string(name) + ".rpkg" ||
            logical_id == "render/geometry/" + std::string(name) + ".rpage")
            return "render/" + std::string(name);
    }
    if (logical_id.starts_with("render/texture/environment_") && logical_id.ends_with(".rpkg"))
    {
        constexpr std::string_view prefix = "render/texture/environment_";
        const auto stem = logical_id.substr(prefix.size(), logical_id.size() - prefix.size() - 5);
        if (!stem.empty() && stem.find('/') == std::string_view::npos)
            return std::string(logical_id.substr(0, logical_id.size() - 5));
    }
    for (const auto name : rd::kEnvironmentMaterialNames)
        if (logical_id == "render/material/" + std::string(name) + "/parameters.rpkg" ||
            logical_id == "render/material/" + std::string(name) + "/material.rpkg" ||
            (logical_id.starts_with("render/shader/" + std::string(name) + "_") && logical_id.ends_with(".dxil")))
            return logical_id.starts_with("render/shader/") ?
                "render/shader/" + std::string(name) :
                (logical_id.ends_with("/parameters.rpkg") ?
                    "render/material/" + std::string(name) + "/parameters" : "render/shader/" + std::string(name));
    for (const auto name : rd::kHeroMaterialNames)
        if (logical_id == "render/material/" + std::string(name) + "/parameters.rpkg" ||
            logical_id == "render/material/" + std::string(name) + "/material.rpkg" ||
            (logical_id.starts_with("render/shader/" + std::string(name) + "_") && logical_id.ends_with(".dxil")))
            return logical_id.starts_with("render/shader/") ?
                "render/shader/" + std::string(name) :
                (logical_id.ends_with("/parameters.rpkg") ?
                    "render/material/" + std::string(name) + "/parameters" : "render/shader/" + std::string(name));
    return {};
}
void Append(Bytes &bytes, const rd::OpacityMicromapDigest &digest)
{ bytes.insert(bytes.end(), digest.begin(), digest.end()); }
const rd::ChunkView &Chunk(const rd::PackageView &package, rd::ChunkType type)
{
    const auto found = std::ranges::find(package.chunks, type, &rd::ChunkView::type);
    Require(found != package.chunks.end() && found->required && found->version == 1,
        "missing required current package chunk");
    return *found;
}

struct SelectedInput
{
    ap::UpstreamProduct product;
    std::string contents;
};
struct Inputs
{
    const fs::path &build;
    ap::ActionSpec &spec;
    std::map<std::string, SelectedInput> selected;
    std::map<std::string, ap::ActionResultSnapshot> snapshots;

    const SelectedInput &Get(std::string_view action, std::string_view id)
    {
        if (const auto found = selected.find(std::string(id)); found != selected.end()) return found->second;
        ap::UpstreamProduct upstream;
        fs::path path;
        auto snapshot = snapshots.try_emplace(std::string(action), build, action).first;
        snapshot->second.ReadProduct(id, upstream, path);
        auto contents = ap::ReadBytes(path);
        // The ActionResult snapshot authenticates the product. Also bind the
        // bytes that are actually parsed below, without a second mutable read.
        Require(ap::Sha256Bytes(ByteSpan(contents)) == upstream.digest && contents.size() == upstream.size,
            "selected product changed while taking the immutable producer snapshot");
        spec.upstream.push_back(upstream);
        return selected.emplace(std::string(id), SelectedInput{std::move(upstream), std::move(contents)}).first->second;
    }
};

struct Material
{
    std::string name, id;
    rd::MaterialGraph graph;
    std::vector<std::uint32_t> words;
    rd::OpacityMicromapDigest program_layout{};
    std::vector<std::string> dependencies;
    bool hero{}, masked{}, unsupported{};
};

// Renderer reconstruction contract (OMP1): four LE marker bytes, selected
// material SHA-256, parameter-layout SHA-256, LE program count, then sorted
// program AssetId (LE64) / actual selected raw SHA-256 pairs. No source or JSON
// parsing is needed in the renderer. OPW1 is packed by the shared public codec.
rd::OpacityMicromapDigest ProgramLayout(const SelectedInput &input,
    const rd::PackageView &package, Inputs &inputs, std::string_view name)
{
    Bytes bytes;
    Word(bytes, 0x31504d4fu);
    Append(bytes, Digest(input.product.digest));
    Append(bytes, Digest(rd::DecodeMaterialParameterLayout(Chunk(package, rd::ChunkType::MaterialParameterLayout).bytes)));
    const auto programs = rd::DecodeMaterialPrograms(Chunk(package, rd::ChunkType::MaterialProgram).bytes, package.dependencies);
    std::map<std::uint64_t, rd::OpacityMicromapDigest> sorted;
    for (const auto &program : programs)
    {
        const auto &id = package.dependencies.at(program.dependency_index);
        const auto &selected = inputs.Get("render/shader/" + std::string(name), id);
        Require(sorted.emplace(hs::MakeAssetId(id).value, Digest(selected.product.digest)).second,
            "duplicate material program identity");
    }
    Require(!sorted.empty() && sorted.size() <= UINT32_MAX, "material program count");
    Word(bytes, std::uint32_t(sorted.size()));
    for (const auto &[id, digest] : sorted) { Identity(bytes, hs::AssetId{id}); Append(bytes, digest); }
    return rd::Sha256(bytes);
}

// Only the already-validated typed graph is recognized. This is deliberately
// not another MaterialIR evaluator: unsupported arithmetic remains unknown.
std::size_t OpacityNode(const rd::MaterialGraph &graph)
{
    auto index = graph.closure_output;
    if (index == rd::kNoMaterialInput) return graph.opacity_output;
    for (std::size_t depth = 0; depth < graph.nodes.size(); ++depth)
    {
        const auto &node = graph.nodes.at(index);
        switch (node.op)
        {
        case rd::MaterialOp::MetallicRoughnessClosure: return node.inputs[5];
        case rd::MaterialOp::UnlitClosure: return node.inputs[1];
        case rd::MaterialOp::CoatClosure: case rd::MaterialOp::AnisotropyClosure:
        case rd::MaterialOp::SheenClosure: case rd::MaterialOp::ThinTransmissionClosure:
        case rd::MaterialOp::EmissionClosure: case rd::MaterialOp::SubsurfaceClosure:
        case rd::MaterialOp::SolidTransmissionClosure: index = node.inputs[0]; break;
        default: return rd::kNoMaterialInput;
        }
    }
    return rd::kNoMaterialInput;
}
std::optional<float> Scalar(const Material &material, std::size_t index)
{
    if (index >= material.graph.nodes.size()) return {};
    const auto &node = material.graph.nodes[index];
    if (node.type != rd::MaterialType::Float) return {};
    if (node.op == rd::MaterialOp::Constant && std::isfinite(node.value[0])) return node.value[0];
    if (node.op == rd::MaterialOp::Parameter && node.declaration_index < material.graph.parameters.size())
    {
        const auto offset = node.declaration_index * 4;
        if (offset < material.words.size())
        {
            const auto value = std::bit_cast<float>(material.words[offset]);
            if (std::isfinite(value)) return value;
        }
    }
    return {};
}
bool FiniteCoordinates(const rd::MaterialGraph &graph, std::size_t index, rd::MaterialContextInput context)
{
    if (index >= graph.nodes.size()) return false;
    const auto &node = graph.nodes[index];
    if (node.type != rd::MaterialType::Float2) return false;
    if (node.op == rd::MaterialOp::Context) return node.context_input == context;
    const auto constant = [&](std::size_t input) {
        if (input >= graph.nodes.size()) return false;
        const auto &value = graph.nodes[input];
        return value.type == rd::MaterialType::Float2 && value.op == rd::MaterialOp::Constant &&
            std::isfinite(value.value[0]) && std::isfinite(value.value[1]) &&
            std::abs(value.value[0]) <= 65536.0f && std::abs(value.value[1]) <= 65536.0f;
    };
    if (node.op == rd::MaterialOp::Multiply || node.op == rd::MaterialOp::Add)
        return (constant(node.inputs[0]) && FiniteCoordinates(graph, node.inputs[1], context)) ||
               (constant(node.inputs[1]) && FiniteCoordinates(graph, node.inputs[0], context));
    if (node.op == rd::MaterialOp::Saturate)
        return context == rd::MaterialContextInput::Uv && FiniteCoordinates(graph, node.inputs[0], context);
    return false;
}
struct AlphaSource
{
    float scale{1};
    std::optional<float> constant;
    std::optional<std::size_t> texture;
    bool context{};
    rd::MaterialSamplerProfile sampler{rd::MaterialSamplerProfile::LinearWrap};
};
std::optional<AlphaSource> RecognizeAlpha(const Material &material)
{
    auto index = OpacityNode(material.graph);
    if (index >= material.graph.nodes.size()) return {};
    const auto &nodes = material.graph.nodes;
    if (nodes[index].op == rd::MaterialOp::Saturate) index = nodes[index].inputs[0];
    if (index >= nodes.size()) return {};
    AlphaSource source;
    if (nodes[index].op == rd::MaterialOp::Multiply)
    {
        if (const auto scalar = Scalar(material, nodes[index].inputs[0]))
        { source.scale = *scalar; index = nodes[index].inputs[1]; }
        else if (const auto scalar = Scalar(material, nodes[index].inputs[1]))
        { source.scale = *scalar; index = nodes[index].inputs[0]; }
        else return {};
    }
    if (index >= nodes.size()) return {};
    if (const auto scalar = Scalar(material, index))
    { source.constant = *scalar; return source; }
    const auto &node = nodes[index];
    if (node.op == rd::MaterialOp::SampledAlpha ||
        (node.op == rd::MaterialOp::Context && node.context_input == rd::MaterialContextInput::SampledAlpha))
    { source.context = true; return source; }
    if (node.op != rd::MaterialOp::Extract || node.component != 3 || node.inputs[0] >= nodes.size()) return {};
    const auto &sample = nodes[node.inputs[0]];
    if (sample.op != rd::MaterialOp::SampleGrad || sample.inputs[0] >= nodes.size() || sample.inputs[1] >= nodes.size()) return {};
    const auto &texture = nodes[sample.inputs[0]], &sampler = nodes[sample.inputs[1]];
    if (texture.op != rd::MaterialOp::TextureRef || texture.declaration_index >= material.graph.textures.size() ||
        sampler.op != rd::MaterialOp::SamplerRef || sampler.declaration_index >= material.graph.samplers.size() ||
        !FiniteCoordinates(material.graph, sample.inputs[2], rd::MaterialContextInput::Uv) ||
        !FiniteCoordinates(material.graph, sample.inputs[4], rd::MaterialContextInput::DUvDx) ||
        !FiniteCoordinates(material.graph, sample.inputs[5], rd::MaterialContextInput::DUvDy)) return {};
    source.texture = texture.declaration_index;
    source.sampler = material.graph.samplers[sampler.declaration_index].profile;
    switch (source.sampler)
    {
    case rd::MaterialSamplerProfile::LinearClamp: case rd::MaterialSamplerProfile::LinearWrap:
    case rd::MaterialSamplerProfile::EnvironmentAnisotropicWrap: case rd::MaterialSamplerProfile::FoliageAnisotropicClamp:
        return source;
    default: return {};
    }
}

// BC7 endpoint decode follows the bit-accurate D3D specification. All index
// weights lie in [0,64], so every decoded alpha lies between its two expanded
// endpoints. Bounding every endpoint also covers unused partitions/padding.
// https://learn.microsoft.com/en-us/windows/win32/direct3d11/bc7-format
// https://learn.microsoft.com/en-us/windows/win32/direct3d11/bc7-format-mode-reference
struct BitReader
{
    std::span<const std::byte> block;
    unsigned position{};
    unsigned Read(unsigned count)
    {
        Require(position + count <= 128, "BC7 endpoint bit range");
        unsigned value{};
        for (unsigned i = 0; i < count; ++i, ++position)
            value |= ((std::to_integer<unsigned>(block[position / 8]) >> (position % 8)) & 1u) << i;
        return value;
    }
};
std::pair<unsigned, unsigned> Bc7Alpha(std::span<const std::byte> block)
{
    Require(block.size() == 16, "BC7 block size");
    BitReader reader{block};
    unsigned mode{};
    while (mode < 8 && reader.Read(1) == 0) ++mode;
    if (mode == 8) return {0, 0}; // specified reserved-mode hardware result
    if (mode < 4) return {255, 255};
    unsigned rotation{};
    if (mode == 4 || mode == 5) rotation = reader.Read(2);
    if (mode == 4) (void)reader.Read(1); // index selection does not enlarge the endpoint interval
    if (mode == 7) (void)reader.Read(6); // every partition endpoint is included
    const unsigned count = mode == 7 ? 4 : 2;
    const unsigned rgb_bits = mode == 4 ? 5 : mode == 5 ? 7 : mode == 6 ? 7 : 5;
    const unsigned alpha_bits = mode == 4 ? 6 : mode == 5 ? 8 : rgb_bits;
    std::array<std::array<unsigned, 4>, 4> endpoints{};
    for (unsigned channel = 0; channel < 4; ++channel)
        for (unsigned endpoint = 0; endpoint < count; ++endpoint)
            endpoints[endpoint][channel] = reader.Read(channel == 3 ? alpha_bits : rgb_bits);
    const bool pbits = mode == 6 || mode == 7;
    if (pbits)
        for (unsigned endpoint = 0; endpoint < count; ++endpoint)
        {
            const auto p = reader.Read(1);
            for (auto &channel : endpoints[endpoint]) channel = (channel << 1) | p;
        }
    const auto channel = rotation ? rotation - 1 : 3;
    const unsigned precision = (channel == 3 ? alpha_bits : rgb_bits) + unsigned(pbits);
    unsigned minimum = 255, maximum{};
    for (unsigned endpoint = 0; endpoint < count; ++endpoint)
    {
        auto value = endpoints[endpoint][channel] << (8 - precision);
        value |= value >> precision;
        minimum = std::min(minimum, value); maximum = std::max(maximum, value);
    }
    return {minimum, maximum};
}
struct AlphaInterval
{
    bool proven{};
    float minimum{}, maximum{1};
};
AlphaInterval TextureAlpha(const rd::TexturePackageView &texture)
{
    unsigned minimum = 255, maximum{};
    const auto format = texture.description.format;
    const bool bc7 = format == rd::kTextureFormatBc7 || format == rd::kTextureFormatBc7Srgb;
    const bool rgba8 = format == rd::kTextureFormatRgba8 || format == rd::kTextureFormatRgba8Srgb;
    if (!bc7 && !rgba8) return {};
    for (const auto &resource : texture.subresources)
    {
        if (bc7)
        {
            Require(resource.bytes.size() % 16 == 0, "compressed alpha block extent");
            for (std::size_t offset = 0; offset < resource.bytes.size(); offset += 16)
            {
                const auto [lo, hi] = Bc7Alpha(resource.bytes.subspan(offset, 16));
                minimum = std::min(minimum, lo); maximum = std::max(maximum, hi);
            }
        }
        else
            for (std::size_t offset = 3; offset < resource.bytes.size(); offset += 4)
            {
                const auto alpha = std::to_integer<unsigned>(resource.bytes[offset]);
                minimum = std::min(minimum, alpha); maximum = std::max(maximum, alpha);
            }
    }
    Require(!texture.subresources.empty(), "empty sampled alpha domain");
    // Alpha is linear even for an sRGB texture. Nonnegative bilinear/trilinear/
    // anisotropic weights cannot escape the whole-texture interval. Expand
    // conversion outward; all array layers/mips cover every admitted physical
    // mip rebase, streaming minimum, atlas rectangle and finite ray gradient.
    return {true, std::max(0.0f, std::nextafter(float(minimum) / 255.0f, -std::numeric_limits<float>::infinity())),
        std::min(1.0f, std::nextafter(float(maximum) / 255.0f, std::numeric_limits<float>::infinity()))};
}

struct Producer
{
    const fs::path &source, &build;
    ap::ActionSpec spec;
    Inputs inputs;
    std::map<std::string, Material> materials;
    std::uint32_t subdivision;
    std::uint64_t byte_budget;
    std::map<std::string, Bytes> outputs;
    std::map<std::string, std::vector<std::string>> dependencies;

    Producer(const fs::path &s, const fs::path &b, std::uint32_t level, std::uint64_t budget)
        : source(s), build(b), inputs{build, spec, {}}, subdivision(level), byte_budget(budget)
    {
        Require(source.is_absolute() && build.is_absolute() && subdivision >= 1 && subdivision <= 4 &&
            byte_budget && byte_budget <= 256ull * 1024 * 1024, "absolute roots, subdivision1..4 and explicit byte budget <=256MiB required");
        spec.action_id = "render.ray.omm";
        spec.recipe_id = "project-hs.render.opacity-micromap";
        spec.recipe_version = 1;
        spec.namespace_path = kNamespace;
        // The producer/baker/input ABI is an authenticated source contract;
        // none of the inputs below are generated files or mutable SDK caches.
        for (const auto relative : kConstructorInputs)
            spec.inputs.push_back(ap::HashInput(std::string(relative), source / relative));
        const auto configuration = Configuration(subdivision, byte_budget);
        spec.inputs.push_back({"omm/coverage-configuration", source / "Source/Apps/RenderOpacityMicromap/main.cpp",
            ap::Sha256Bytes(ByteSpan(configuration))});
    }

    Material &LoadMaterial(std::string_view name, std::string_view id, std::string_view action, bool hero)
    {
        if (const auto found = materials.find(std::string(id)); found != materials.end()) return found->second;
        const auto relative = "ContentSource/Materials/" + std::string(name) + ".json";
        spec.inputs.push_back(ap::HashInput(relative, source / relative));
        const auto authored = ap::ReadBytes(source / relative);
        Require(ap::Sha256Bytes(ByteSpan(authored)) == spec.inputs.back().digest,
            "authored graph changed while taking the immutable input snapshot");
        auto graph = hs::material_content::ParseMaterialDefinition(name, authored).graph;
        const auto &selected = inputs.Get(action, id);
        const auto package = rd::ReadPackage(ByteSpan(selected.contents), hs::MakeAssetId(id));
        const auto parameter_id = "render/material/" + std::string(name) + "/parameters.rpkg";
        const auto &parameter_input = inputs.Get("render/material/" + std::string(name) + "/parameters", parameter_id);
        auto parameters = rd::ReadMaterialParameterPackage(ByteSpan(parameter_input.contents), hs::MakeAssetId(parameter_id));
        Require(rd::MaterialParameterLayoutHash(parameters) == rd::MaterialParameterLayoutHash(graph.parameters) &&
            rd::DecodeMaterialParameterLayout(Chunk(package, rd::ChunkType::MaterialParameterLayout).bytes) ==
                rd::MaterialParameterLayoutHash(parameters), "actual selected parameter/IR/package layouts disagree");
        std::vector<std::uint32_t> words;
        for (const auto &parameter : parameters) words.insert(words.end(), parameter.default_words.begin(), parameter.default_words.end());
        // Proof values come from the selected immutable parameter product,
        // not graph authoring defaults. The live upload uses the same4words/slot.
        graph.parameters = std::move(parameters);
        const auto ordinal = hero ? std::ranges::find(rd::kHeroMaterialNames, name) - rd::kHeroMaterialNames.begin()
            : std::ranges::find(rd::kAuthoredMaterialNames, name) - rd::kAuthoredMaterialNames.begin();
        Require(ordinal < (hero ? rd::kHeroMaterialNames.size() : rd::kAuthoredMaterialNames.size()), "material ordinal");
        const auto semantic = hero ? rd::EmitHeroMaterialContextHlsl(graph,
            "HsHeroEvaluateSurface_" + std::to_string(ordinal), "HsHeroEvaluateOpacity_" + std::to_string(ordinal))
            : rd::EmitEnvironmentMaterialContextHlsl(graph, ordinal);
        const auto shader_result = ap::Json::parse(ap::ReadBytes(build / "Actions/render/shader" / name / "result.json"));
        const auto semantic_id = "material/" + std::string(name) + (hero ? "/hero-context-ir" : "/shader-ir");
        const auto &shader_inputs = shader_result.at("inputs");
        const auto matched = std::ranges::find_if(shader_inputs, [&](const auto &input) { return input.at("logical_id") == semantic_id; });
        Require(matched != shader_inputs.end() && matched->at("digest") == ap::Sha256Bytes(ByteSpan(semantic)),
            "selected material program semantic IR differs from actual opacity graph");
        Material material;
        material.name = name; material.id = id; material.hero = hero;
        material.masked = graph.kind == rd::MaterialGraphKind::Masked || graph.kind == rd::MaterialGraphKind::Leaf0;
        material.unsupported = graph.kind == rd::MaterialGraphKind::Translucent || graph.kind == rd::MaterialGraphKind::HeroRefractive;
        // Interface layers are conservatively unknown even when the current
        // uniform weight is zero; their complete transport validity is not
        // part of this narrow coverage recognizer.
        const auto reflection = rd::AnalyzeClosure(graph);
        constexpr auto interface_mask = std::uint32_t(rd::MaterialClosureCapability::ThinTransmission) |
            std::uint32_t(rd::MaterialClosureCapability::Subsurface) | std::uint32_t(rd::MaterialClosureCapability::SolidTransmission);
        material.unsupported |= (reflection.capability_mask & interface_mask) != 0;
        material.graph = std::move(graph); material.words = std::move(words);
        material.program_layout = ProgramLayout(selected, package, inputs, name);
        material.dependencies = {std::string(id), parameter_id};
        for (const auto &program : rd::DecodeMaterialPrograms(Chunk(package, rd::ChunkType::MaterialProgram).bytes, package.dependencies))
            material.dependencies.push_back(package.dependencies.at(program.dependency_index));
        return materials.emplace(std::string(id), std::move(material)).first->second;
    }

    AlphaInterval AlphaTexture(std::string_view id, std::string_view action, ap::Json &proof,
        std::vector<std::string> &deps)
    {
        const auto &input = inputs.Get(action, id);
        const auto texture = rd::ReadTexturePackage(ByteSpan(input.contents), hs::MakeAssetId(id));
        auto interval = TextureAlpha(texture);
        deps.emplace_back(id);
        ap::Json mip_records = ap::Json::array();
        for (const auto &resource : texture.subresources)
            mip_records.push_back({{"array_slice", resource.array_slice}, {"mip_level", resource.mip_level},
                {"width", resource.width}, {"height", resource.height}, {"row_pitch", resource.row_pitch},
                {"slice_pitch", resource.slice_pitch}});
        proof["alpha_products"].push_back({{"logical_id", id}, {"digest", input.product.digest},
            {"format", texture.description.format}, {"array_size", texture.description.array_size},
            {"mip_count", texture.description.mip_count}, {"subresources", std::move(mip_records)},
            {"all_array_layers", true}, {"all_logical_mips", true}, {"interval_proven", interval.proven},
            {"alpha_min_bits", std::bit_cast<std::uint32_t>(interval.minimum)},
            {"alpha_max_bits", std::bit_cast<std::uint32_t>(interval.maximum)}});
        return interval;
    }

    rd::OpacityMicromapAlphaProof Proof(const Material &material, bool environment,
        std::string_view character, std::string_view texture_action, std::uint32_t slot,
        bool unsupported_binding, ap::Json &proof, std::vector<std::string> &deps)
    {
        rd::OpacityMicromapAlphaProof result;
        proof["alpha_products"] = ap::Json::array();
        proof["material_id"] = material.id;
        proof["cutoff_bits"] = kCutoffBits;
        proof["sampler_domain"] = "all-array-layers/all-logical-mips/linear-clamp-wrap/anisotropy1..16";
        proof["streaming_domain"] = "all-valid-logical-mip-rebases/minimum-resident-mip";
        if (unsupported_binding || material.unsupported)
        { proof["reason"] = "unsupported/translucent/interface binding retains ordinary ray rejection"; return result; }
        // Ordinary unmasked environment coverage bypasses authored alpha.
        if (environment && !material.masked)
        { result.proven = true; result.alpha_min = result.alpha_max = 1; proof["reason"] = "actual unmasked environment coverage"; return result; }
        const auto alpha = RecognizeAlpha(material);
        if (!alpha) { proof["reason"] = "opacity graph or sampler domain is not proved"; return result; }
        result.scale = alpha->scale;
        proof["scale_bits"] = std::bit_cast<std::uint32_t>(result.scale);
        AlphaInterval interval;
        if (alpha->constant) interval = {true, *alpha->constant, *alpha->constant};
        else if (alpha->texture)
        {
            const auto &definition = material.graph.textures.at(*alpha->texture);
            const auto action = definition.product_id.substr(0, definition.product_id.size() - 5);
            interval = AlphaTexture(definition.product_id, action, proof, deps);
            proof["sampler_profile"] = std::uint32_t(alpha->sampler);
        }
        else if (alpha->context && !environment && character == "archer")
        {
            // Actual HsHeroMaterialContext reads the canonical FBX slot's
            // ArcherDiffuse alpha. Expressions/charge alter only color RGB.
            const auto id = "render/texture/archer_diffuse_" + std::to_string(slot) + ".rpkg";
            interval = AlphaTexture(id, texture_action, proof, deps);
            proof["sampler_profile"] = std::uint32_t(rd::MaterialSamplerProfile::LinearWrap);
        }
        else if (alpha->context && !environment && character.starts_with("boss_"))
        {
            // MonsterPBR context initializes sampled_alpha=1; its actual
            // basecolor/RAM samples do not write alpha.
            interval = {true, 1, 1};
        }
        else
        { proof["reason"] = "sampled-alpha adapter domain is not proved"; return result; }
        result.proven = interval.proven;
        result.alpha_min = interval.minimum; result.alpha_max = interval.maximum;
        proof["reason"] = result.proven ? "whole reachable sampling domain proved" : "unsupported alpha format";
        return result;
    }

    void Bake(std::string_view metadata_id, std::string_view page_id, const SelectedInput &page,
        const rd::GeometryData &geometry, std::span<Material *const> selected_materials,
        std::span<const std::uint32_t> triangle_materials,
        std::span<const rd::OpacityMicromapAlphaProof> alpha, ap::Json proof, std::vector<std::string> deps)
    {
        Require((page_id.starts_with("render/geometry/") || page_id.starts_with("render/hlod/")) &&
            page_id.ends_with(".rpage"), "canonical page namespace");
        const auto prefix = page_id.starts_with("render/geometry/") ? 16u : 7u;
        const auto stem = page_id.substr(prefix, page_id.size() - prefix - 6);
        const auto id = "render/ray/omm/" + std::string(stem) + ".rpkg";
        Require(!outputs.contains(id), "duplicate canonical OMM page");
        std::vector<std::array<float, 2>> uvs;
        Bytes topology;
        Word(topology, 0x3154504fu); // OPT1, all original UV/selector/index/submesh words
        Word(topology, std::uint32_t(geometry.vertices.size()));
        for (const auto &vertex : geometry.vertices)
        {
            uvs.push_back(vertex.uv);
            for (float uv : vertex.uv) Word(topology, std::bit_cast<std::uint32_t>(uv));
            Word(topology, vertex.material_index);
        }
        Word(topology, std::uint32_t(geometry.indices.size()));
        for (auto index : geometry.indices) Word(topology, index);
        Word(topology, std::uint32_t(geometry.submeshes.size()));
        for (const auto &submesh : geometry.submeshes)
        { Word(topology, submesh.first_index); Word(topology, submesh.index_count); Word(topology, submesh.material_index); }
        for (auto material : triangle_materials) Word(topology, material);
        std::vector<rd::OpacityMicromapMaterialWords> tags;
        std::set<std::uint64_t> seen;
        for (auto *material : selected_materials)
            if (seen.insert(hs::MakeAssetId(material->id).value).second)
            {
                tags.push_back({hs::MakeAssetId(material->id), material->program_layout, material->words, kCutoffBits});
                deps.insert(deps.end(), material->dependencies.begin(), material->dependencies.end());
            }
        const auto words = rd::BuildOpacityMicromapWords(tags);
        const auto actual_word_bytes = WordBytes(words);
        rd::OpacityMicromapIdentity identity;
        identity.geometry_asset = hs::MakeAssetId(page_id);
        identity.geometry_digest = Digest(page.product.digest);
        identity.topology_digest = rd::Sha256(topology);
        identity.material_digest = rd::Sha256(actual_word_bytes);
        identity.triangle_count = std::uint32_t(geometry.indices.size() / 3);
        proof["schema_version"] = 1;
        proof["canonical_geometry"] = page_id;
        proof["geometry_digest"] = page.product.digest;
        proof["metadata_product"] = metadata_id;
        proof["metadata_digest"] = inputs.selected.at(std::string(metadata_id)).product.digest;
        proof["subdivision_level"] = subdivision;
        proof["byte_budget_per_page"] = byte_budget;
        proof["baker_abi"] = rd::kOpacityMicromapBakerAbi;
        proof["topology_digest"] = ap::Sha256Bytes(topology);
        proof["material_words_digest"] = ap::Sha256Bytes(actual_word_bytes);
        proof["immutable_inputs"] = ap::Json::array();
        for (const auto &input : spec.inputs)
            proof["immutable_inputs"].push_back({{"logical_id", input.logical_id}, {"digest", input.digest}});
        deps.emplace_back(page_id); deps.emplace_back(metadata_id);
        std::ranges::sort(deps); deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
        proof["immutable_selected_products"] = ap::Json::array();
        for (const auto &dependency : deps)
        {
            const auto &product = inputs.selected.at(dependency).product;
            proof["immutable_selected_products"].push_back({{"logical_id", product.logical_id},
                {"digest", product.digest}, {"size", product.size}});
        }
        const auto proof_bytes = proof.dump();
        const auto recipe = rd::BuildOpacityMicromapRecipe(words, ByteSpan(proof_bytes));
        identity.coverage_key = rd::Sha256(recipe);
        const auto data = rd::BakeOpacityMicromaps({uvs, geometry.indices, triangle_materials, alpha, subdivision, byte_budget});
        auto package = rd::BuildOpacityMicromapPackage(hs::MakeAssetId(id), deps, identity, recipe, data);
        const auto decoded = rd::ReadOpacityMicromapPackage(package, hs::MakeAssetId(id));
        Require(decoded.identity.geometry_asset.value == identity.geometry_asset.value &&
            decoded.identity.triangle_count == identity.triangle_count && decoded.known_cells == data.known_cells &&
            decoded.unknown_cells == data.unknown_cells, "OMM self-validation mismatch");
        std::cout << id << " triangles=" << identity.triangle_count << " known_cells=" << data.known_cells
                  << " unknown_cells=" << data.unknown_cells << '\n';
        outputs.emplace(id, std::move(package)); dependencies.emplace(id, std::move(deps));
    }

    void Environment(std::string_view name)
    {
        const auto metadata_id = "render/" + std::string(name) + ".rpkg";
        const auto &metadata_input = inputs.Get("render/" + std::string(name), metadata_id);
        const auto metadata = rd::ReadPackage(ByteSpan(metadata_input.contents), hs::MakeAssetId(metadata_id));
        const auto geometry_metadata = rd::DecodeGeometryMetadata(Chunk(metadata, rd::ChunkType::GeometryMetadata).bytes,
            metadata.dependencies, metadata_id);
        const auto &page_id = metadata.dependencies.at(geometry_metadata.page_dependency_index);
        const auto &page_input = inputs.Get("render/" + std::string(name), page_id);
        const auto geometry = rd::DecodeGeometryPage(ByteSpan(page_input.contents), hs::MakeAssetId(page_id), geometry_metadata);
        auto &material = LoadMaterial(name, metadata_id, "render/" + std::string(name), false);
        const auto actual_material = rd::DecodeRockMaterial(Chunk(metadata, rd::ChunkType::MaterialMetadata).bytes, metadata.dependencies);
        Require(material.masked == actual_material.alpha_mask, "selected material coverage flag disagrees with authored IR");
        ap::Json proof;
        std::vector<std::string> deps;
        ap::Json material_proof;
        const auto alpha = Proof(material, true, {}, {}, 0, false, material_proof, deps);
        proof["materials"] = ap::Json::array({std::move(material_proof)});
        std::vector<std::uint32_t> triangle_materials(geometry.indices.size() / 3, 0);
        // Original canonical topology is not meshlet order. Validate each
        // original triangle's flat layer selector instead of inferring it from
        // the current draw/instance or generated HLOD material buckets.
        for (std::size_t triangle = 0; triangle < triangle_materials.size(); ++triangle)
        {
            const auto a = geometry.vertices.at(geometry.indices[triangle * 3]).material_index;
            if (geometry.vertices.at(geometry.indices[triangle * 3 + 1]).material_index != a ||
                geometry.vertices.at(geometry.indices[triangle * 3 + 2]).material_index != a)
                triangle_materials[triangle] = kUnknownMaterial;
        }
        const std::array material_array{&material};
        const std::array alpha_array{alpha};
        Bake(metadata_id, page_id, page_input, geometry, material_array, triangle_materials, alpha_array, std::move(proof), std::move(deps));
    }

    void Character(std::string_view mode, std::string_view stem)
    {
        const auto action = "render/character/" + std::string(mode);
        const auto metadata_id = "render/character/" + std::string(stem) + ".rpkg";
        const auto &metadata_input = inputs.Get(action, metadata_id);
        const auto package = rd::ReadPackage(ByteSpan(metadata_input.contents), hs::MakeAssetId(metadata_id));
        const auto metadata = rd::DecodeCharacterMetadataPackage(ByteSpan(metadata_input.contents), hs::MakeAssetId(metadata_id), mode == "archer");
        const auto &page_id = package.dependencies.at(metadata.geometry.page_dependency_index);
        const auto &page_input = inputs.Get(action, page_id);
        const auto geometry = rd::DecodeGeometryPage(ByteSpan(page_input.contents), hs::MakeAssetId(page_id), metadata.geometry);
        rd::ValidateCharacterGeometry(metadata, geometry);
        std::vector<Material *> selected;
        std::map<std::uint32_t, Material *> opaque;
        std::set<std::uint32_t> unsupported;
        for (const auto &binding : metadata.material_bindings)
        {
            const auto name = rd::kHeroMaterialNames.at(binding.hero_ordinal);
            const auto &id = package.dependencies.at(binding.dependency_index);
            auto &material = LoadMaterial(name, id, "render/shader/" + std::string(name), true);
            selected.push_back(&material);
            if (binding.domain == rd::HeroMaterialDomain::Opaque)
                Require(opaque.emplace(binding.slot, &material).second, "duplicate canonical opaque material slot");
            else unsupported.insert(binding.slot);
        }
        // Projectile shells are rejected before ordinary opaque coverage in
        // HsRayCovered and carry unsupported-interface distance in RtTrace.
        if (mode == "slime-projectile")
            for (std::uint32_t slot = 0; slot < metadata.material_count; ++slot) unsupported.insert(slot);
        std::vector<rd::OpacityMicromapAlphaProof> alpha(metadata.material_count);
        ap::Json proof{{"materials", ap::Json::array()}};
        std::vector<std::string> deps;
        for (std::uint32_t slot = 0; slot < metadata.material_count; ++slot)
        {
            const auto binding = opaque.find(slot);
            if (binding == opaque.end()) continue;
            ap::Json material_proof;
            alpha[slot] = Proof(*binding->second, false, stem, "render/texture/character/" + std::string(mode),
                slot, unsupported.contains(slot), material_proof, deps);
            material_proof["canonical_binding_slot"] = slot;
            proof["materials"].push_back(std::move(material_proof));
        }
        std::vector<std::uint32_t> triangle_materials(geometry.indices.size() / 3, kUnknownMaterial);
        for (std::size_t triangle = 0; triangle < triangle_materials.size(); ++triangle)
        {
            const auto slot = geometry.vertices.at(geometry.indices[triangle * 3]).material_index;
            if (opaque.contains(slot) && geometry.vertices.at(geometry.indices[triangle * 3 + 1]).material_index == slot &&
                geometry.vertices.at(geometry.indices[triangle * 3 + 2]).material_index == slot)
                triangle_materials[triangle] = slot;
        }
        Bake(metadata_id, page_id, page_input, geometry, selected, triangle_materials, alpha, std::move(proof), std::move(deps));
    }

    void Hlod()
    {
        constexpr std::string_view scene_id = "render/hlod/arena.shl1";
        const auto &scene_input = inputs.Get("render/hlod/arena", scene_id);
        const auto scene = rd::DecodeSceneHlod(ByteSpan(scene_input.contents));
        Require(scene.scene_asset_id.value == hs::MakeAssetId(scene_id).value, "selected HLOD scene identity");
        for (std::uint32_t page_index = 0; page_index < scene.pages.size(); ++page_index)
        {
            const auto &record = scene.pages[page_index];
            const auto &page = inputs.Get("render/hlod/arena", record.logical_id);
            const auto geometry = rd::DecodeGeometryPagePayload(ByteSpan(page.contents),
                hs::MakeAssetId(record.logical_id), record.geometry_flags);
            rd::ValidateSceneHlodPage(scene, page_index, geometry);
            std::map<std::string, std::uint32_t> bindings;
            std::vector<Material *> selected;
            std::vector<rd::OpacityMicromapAlphaProof> alpha;
            std::vector<std::uint32_t> triangle_materials(geometry.indices.size() / 3, kUnknownMaterial);
            std::vector<bool> claimed(triangle_materials.size());
            std::vector<std::string> deps;
            ap::Json proof{{"materials", ap::Json::array()}, {"proxy_bindings", ap::Json::array()},
                {"scene_digest", scene_input.product.digest}};
            for (const auto &proxy : scene.proxies)
            {
                if (proxy.page_index != page_index) continue;
                const auto &material_id = scene.dependencies.at(proxy.material_dependency);
                Require(material_id.starts_with("render/") && material_id.ends_with(".rpkg"), "HLOD material product namespace");
                auto found = bindings.find(material_id);
                if (found == bindings.end())
                {
                    const auto name = std::string_view(material_id).substr(7, material_id.size() - 12);
                    auto &material = LoadMaterial(name, material_id, "render/" + std::string(name), false);
                    ap::Json material_proof;
                    alpha.push_back(Proof(material, true, {}, {}, 0, false, material_proof, deps));
                    selected.push_back(&material);
                    proof["materials"].push_back(std::move(material_proof));
                    found = bindings.emplace(material_id, std::uint32_t(selected.size() - 1)).first;
                }
                Require(proxy.first_index % 3 == 0 && proxy.index_count % 3 == 0 &&
                    std::uint64_t(proxy.first_index) + proxy.index_count <= geometry.indices.size(), "HLOD original primitive binding range");
                proof["proxy_bindings"].push_back({{"first_index", proxy.first_index}, {"index_count", proxy.index_count},
                    {"material_id", material_id}, {"atlas_selector", proxy.atlas_selector},
                    {"mesh_semantic", proxy.mesh_semantic}, {"variant", proxy.variant}});
                for (std::uint32_t index = proxy.first_index; index < proxy.first_index + proxy.index_count; index += 3)
                {
                    const auto triangle = index / 3;
                    const auto a = geometry.vertices.at(geometry.indices[index]).material_index;
                    const bool flat = geometry.vertices.at(geometry.indices[index + 1]).material_index == a &&
                        geometry.vertices.at(geometry.indices[index + 2]).material_index == a;
                    if (!claimed[triangle]) triangle_materials[triangle] = flat ? found->second : kUnknownMaterial;
                    else if (!flat || triangle_materials[triangle] != found->second) triangle_materials[triangle] = kUnknownMaterial;
                    claimed[triangle] = true;
                }
            }
            Require(!selected.empty(), "HLOD page has no actual material bindings");
            Bake(scene_id, record.logical_id, page, geometry, selected, triangle_materials, alpha, std::move(proof), std::move(deps));
        }
    }

    bool Current() const
    {
        const auto result_path = build / "Actions" / spec.namespace_path / "result.json";
        if (!fs::exists(result_path) || !fs::exists(result_path.parent_path() / "result.d")) return false;
        try
        {
            const auto previous = ap::Json::parse(ap::ReadBytes(result_path));
            if (!previous.is_object() || previous.at("schema_version") != 1 ||
                previous.at("action_id") != spec.action_id || previous.at("recipe_id") != spec.recipe_id ||
                previous.at("recipe_version") != spec.recipe_version ||
                previous.at("namespace_path") != spec.namespace_path ||
                !previous.at("inputs").is_array() || !previous.at("upstream_product_dependencies").is_array() ||
                !previous.at("products").is_array())
                return false;
            const ap::ActionResultSnapshot result(build, spec.namespace_path);
            if (result.ActionId() != spec.action_id || result.ActionKeyValue() != previous.at("action_key").get<std::string>() ||
                result.ProductCount() != previous.at("products").size())
                return false;

            ap::ActionSpec current = spec;
            current.inputs.clear();
            current.upstream.clear();
            std::set<std::string> input_ids;
            std::set<std::string> expected_input_ids;
            for (const auto input : kConstructorInputs) expected_input_ids.emplace(input);
            expected_input_ids.emplace("omm/coverage-configuration");
            for (const auto name : rd::kEnvironmentMaterialNames)
                expected_input_ids.emplace("ContentSource/Materials/" + std::string(name) + ".json");
            for (const auto name : rd::kHeroMaterialNames)
                expected_input_ids.emplace("ContentSource/Materials/" + std::string(name) + ".json");
            for (const auto &entry : previous.at("inputs"))
            {
                if (!entry.is_object() || entry.size() != 2 || !entry.at("logical_id").is_string() ||
                    !entry.at("digest").is_string()) return false;
                const auto logical_id = entry.at("logical_id").get<std::string>();
                const auto digest = entry.at("digest").get<std::string>();
                if (!input_ids.emplace(logical_id).second || !IsDigestText(digest)) return false;
                const auto path = CanonicalInputPath(source, logical_id);
                if (!path) return false;
                ap::Input current_input;
                if (logical_id == "omm/coverage-configuration")
                    current_input = {logical_id, *path, ap::Sha256Bytes(ByteSpan(Configuration(subdivision, byte_budget)))};
                else current_input = ap::HashInput(logical_id, *path);
                if (current_input.digest != digest) return false;
                current.inputs.push_back(std::move(current_input));
            }
            if (input_ids != expected_input_ids) return false;
            std::ranges::sort(current.inputs, {}, &ap::Input::logical_id);

            std::map<std::string, ap::ActionResultSnapshot> snapshots;
            std::vector<ap::UpstreamProduct> previous_upstream;
            std::set<std::string> upstream_ids;
            for (const auto &entry : previous.at("upstream_product_dependencies"))
            {
                if (!entry.is_object() || entry.size() != 3 || !entry.at("logical_id").is_string() ||
                    !entry.at("digest").is_string() || !entry.at("size").is_number_unsigned()) return false;
                const auto logical_id = entry.at("logical_id").get<std::string>();
                const auto digest = entry.at("digest").get<std::string>();
                if (!upstream_ids.emplace(logical_id).second || !IsDigestText(digest)) return false;
                const auto namespace_path = UpstreamNamespace(logical_id);
                if (!namespace_path) return false;
                auto snapshot = snapshots.find(*namespace_path);
                if (snapshot == snapshots.end())
                    snapshot = snapshots.try_emplace(*namespace_path, build, *namespace_path).first;
                ap::UpstreamProduct selected;
                fs::path selected_path;
                snapshot->second.ReadProduct(logical_id, selected, selected_path);
                if (selected.digest != digest || selected.size != entry.at("size").get<std::uint64_t>() ||
                    selected.result_path.lexically_normal() !=
                        (build / "Actions" / *namespace_path / "result.json").lexically_normal())
                    return false;
                previous_upstream.push_back({logical_id, digest, entry.at("size").get<std::uint64_t>(), {}});
                current.upstream.push_back(std::move(selected));
            }
            std::ranges::sort(previous_upstream, {}, &ap::UpstreamProduct::logical_id);
            std::ranges::sort(current.upstream, {}, &ap::UpstreamProduct::logical_id);
            if (previous_upstream.empty() || previous_upstream.size() != current.upstream.size())
                return false;
            for (std::size_t index = 0; index < current.upstream.size(); ++index)
                if (previous_upstream[index].logical_id != current.upstream[index].logical_id ||
                    previous_upstream[index].digest != current.upstream[index].digest ||
                    previous_upstream[index].size != current.upstream[index].size)
                    return false;
            if (ap::ActionKey(current) != result.ActionKeyValue()) return false;

            std::vector<ap::Product> products;
            std::string previous_product_id;
            for (const auto &entry : previous.at("products"))
            {
                if (!entry.is_object() || !entry.at("logical_id").is_string() ||
                    !entry.at("locator").is_string() || !entry.at("digest").is_string() ||
                    !entry.at("size").is_number_unsigned()) return false;
                const auto logical_id = entry.at("logical_id").get<std::string>();
                const auto product_suffix = logical_id.starts_with(std::string(kNamespace) + "/") ?
                    std::string_view(logical_id).substr(kNamespace.size() + 1) : std::string_view{};
                if (product_suffix.empty() || product_suffix.starts_with('/') || product_suffix.ends_with('/') ||
                    product_suffix.find("//") != std::string_view::npos || product_suffix.find("..") != std::string_view::npos ||
                    product_suffix.find('\\') != std::string_view::npos || !logical_id.ends_with(".rpkg") ||
                    (!previous_product_id.empty() && logical_id <= previous_product_id)) return false;
                previous_product_id = logical_id;
                ap::UpstreamProduct product;
                fs::path product_path;
                result.ReadProduct(logical_id, product, product_path);
                if (product.digest != entry.at("digest").get<std::string>() ||
                    product.size != entry.at("size").get<std::uint64_t>()) return false;
                const auto filename = fs::path(entry.at("locator").get<std::string>()).filename().generic_string();
                auto expected_filename = std::string(product_suffix);
                std::replace(expected_filename.begin(), expected_filename.end(), '/', '_');
                if (filename.empty() || filename.find('/') != std::string::npos || filename != expected_filename) return false;
                std::vector<std::string> dependencies;
                if (entry.contains("dependencies"))
                {
                    if (!entry.at("dependencies").is_array()) return false;
                    for (const auto &dependency : entry.at("dependencies"))
                    {
                        if (!dependency.is_string()) return false;
                        dependencies.push_back(dependency.get<std::string>());
                    }
                    if (!std::ranges::is_sorted(dependencies) ||
                        std::adjacent_find(dependencies.begin(), dependencies.end()) != dependencies.end() ||
                        std::ranges::any_of(dependencies, [&](const auto &dependency) {
                            return dependency.empty() || dependency == logical_id;
                        })) return false;
                }
                products.push_back({logical_id, filename, product.digest, product.size, std::move(dependencies)});
                const auto package = ap::ReadBytes(product_path);
                (void)rd::ReadOpacityMicromapPackage(ByteSpan(package), hs::MakeAssetId(logical_id));
            }
            if (products.empty() || ap::ProductSetId(products) != previous.at("product_set_id").get<std::string>())
                return false;
            for (const auto &input : current.inputs)
            {
                const auto digest = input.logical_id == "omm/coverage-configuration" ?
                    ap::Sha256Bytes(ByteSpan(Configuration(subdivision, byte_budget))) : ap::Sha256File(input.path);
                if (digest != input.digest) return false;
            }
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    }

    void Run()
    {
        if (Current())
        {
            ap::TouchActionResult(build / "Actions" / spec.namespace_path / "result.json");
            std::cout << spec.action_id << " current\n";
            return;
        }
        for (const auto name : rd::kEnvironmentMaterialNames) Environment(name);
        for (const auto &[mode, stem] : kCharacters) Character(mode, stem);
        Require(outputs.size() == rd::kEnvironmentMaterialNames.size() + kCharacters.size(), "complete canonical OMM output set");
        Hlod();
        std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);
        spec.inputs.erase(std::unique(spec.inputs.begin(), spec.inputs.end(), [](const auto &a, const auto &b) {
            Require(a.logical_id != b.logical_id || (a.digest == b.digest && a.path == b.path), "conflicting immutable input identity");
            return a.logical_id == b.logical_id;
        }), spec.inputs.end());
        std::ranges::sort(spec.upstream, {}, &ap::UpstreamProduct::logical_id);
        for (const auto &input : spec.inputs)
            if (input.logical_id != "omm/coverage-configuration")
                Require(ap::Sha256File(input.path) == input.digest, "immutable producer input changed before publication");
        const auto stage = ap::CreateStage(build / "Actions" / spec.namespace_path);
        try
        {
            std::vector<ap::Product> products;
            for (const auto &[id, package] : outputs)
            {
                auto filename = id.substr(kNamespace.size() + 1);
                std::replace(filename.begin(), filename.end(), '/', '_');
                ap::WriteBytes(stage / filename, package);
                auto product = ap::HashProduct(id, filename, stage / filename);
                product.dependencies = dependencies.at(id);
                products.push_back(std::move(product));
            }
            ap::Publish(build, spec, stage, std::move(products));
        }
        catch (...) { ap::RemoveStage(stage); throw; }
        ap::RemoveStage(stage);
        std::cout << spec.action_id << " published\n";
    }
};
} // namespace

int wmain(int argc, wchar_t **argv)
{
    fs::path source, build;
    std::uint32_t subdivision{};
    std::uint64_t budget{};
    try
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring_view option = argv[i];
            Require(i + 1 < argc, "option requires a value");
            if (option == L"--source-root") source = argv[++i];
            else if (option == L"--build-root") build = argv[++i];
            else if (option == L"--subdivision-level")
            {
                const std::wstring value = argv[++i]; std::size_t consumed{};
                const auto parsed = std::stoull(value, &consumed);
                Require(consumed == value.size() && parsed <= UINT32_MAX, "subdivision integer");
                subdivision = std::uint32_t(parsed);
            }
            else if (option == L"--byte-budget")
            {
                const std::wstring value = argv[++i]; std::size_t consumed{};
                budget = std::stoull(value, &consumed);
                Require(consumed == value.size() && !value.starts_with(L"-"), "byte budget integer");
            }
            else throw std::runtime_error("hs_render_opacity_micromap: invalid command line");
        }
        Require(!source.empty() && !build.empty(), "source/build roots required");
        Producer(source, build, subdivision, budget).Run();
        return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure("hs_render_opacity_micromap", error.what(),
                          {{"action_id", "render.ray.omm"},
                           {"subdivision_level", subdivision}, {"byte_budget", budget},
                           {"source_root", ap::DiagnosticPath(source)},
                           {"build_root", ap::DiagnosticPath(build)}});
        return 1;
    }
}
