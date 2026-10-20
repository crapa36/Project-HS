#include "material_graph_parser.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hs::material_content
{
namespace
{
using Json = nlohmann::json;
namespace rd = hs::render_data;

void Require(bool condition, std::string_view reason)
{
    if (!condition) throw std::runtime_error("environment material graph: " + std::string(reason));
}

void ExactKeys(const Json &object, std::initializer_list<std::string_view> keys)
{
    Require(object.is_object() && object.size() == keys.size(), "unexpected fields");
    for (const auto key : keys)
        Require(object.contains(std::string(key)), "missing field");
}

bool ValidId(std::string_view id)
{
    if (id.empty() || id.size() > 32 ||
        !((id.front() >= 'a' && id.front() <= 'z') || id.front() == '_')) return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}

rd::MaterialType ParseType(const Json &type)
{
    Require(type.is_string(), "node type must be a string");
    if (type == "float") return rd::MaterialType::Float;
    if (type == "float2") return rd::MaterialType::Float2;
    if (type == "float3") return rd::MaterialType::Float3;
    if (type == "float4") return rd::MaterialType::Float4;
    if (type == "uint") return rd::MaterialType::Uint;
    if (type == "texture_ref") return rd::MaterialType::TextureRef;
    if (type == "sampler_ref") return rd::MaterialType::SamplerRef;
    if (type == "terrain_result") return rd::MaterialType::TerrainResult;
    if (type == "surface_closure") return rd::MaterialType::SurfaceClosure;
    Require(type == "bool", "unsupported node type");
    return rd::MaterialType::Bool;
}

struct Edge
{
    std::string id;
    rd::MaterialType type{};
};

Edge ParseEdge(const Json &edge)
{
    ExactKeys(edge, {"node", "type"});
    Require(edge.at("node").is_string(), "edge ID must be a string");
    Edge result{edge.at("node").get<std::string>(), ParseType(edge.at("type"))};
    Require(ValidId(result.id), "invalid edge ID");
    return result;
}

struct Pending
{
    rd::MaterialNode node;
    std::array<Edge, rd::kMaxMaterialInputs> inputs;
};

rd::MaterialOp ParseOp(std::string_view op)
{
    constexpr std::array operations{
        std::pair{"sampled_base_color", rd::MaterialOp::SampledBaseColor},
        std::pair{"sampled_roughness", rd::MaterialOp::SampledRoughness},
        std::pair{"sampled_alpha", rd::MaterialOp::SampledAlpha},
        std::pair{"parameter", rd::MaterialOp::Parameter},
        std::pair{"constant", rd::MaterialOp::Constant},
        std::pair{"multiply", rd::MaterialOp::Multiply},
        std::pair{"saturate", rd::MaterialOp::Saturate},
        std::pair{"add", rd::MaterialOp::Add},
        std::pair{"subtract", rd::MaterialOp::Subtract},
        std::pair{"divide", rd::MaterialOp::Divide},
        std::pair{"min", rd::MaterialOp::Min},
        std::pair{"max", rd::MaterialOp::Max},
        std::pair{"negate", rd::MaterialOp::Negate},
        std::pair{"lerp", rd::MaterialOp::Lerp},
        std::pair{"clamp", rd::MaterialOp::Clamp},
        std::pair{"construct", rd::MaterialOp::Construct},
        std::pair{"extract", rd::MaterialOp::Extract},
        std::pair{"convert", rd::MaterialOp::Convert},
        std::pair{"less", rd::MaterialOp::Less},
        std::pair{"less_equal", rd::MaterialOp::LessEqual},
        std::pair{"equal", rd::MaterialOp::Equal},
        std::pair{"select", rd::MaterialOp::Select},
        std::pair{"logical_and", rd::MaterialOp::LogicalAnd},
        std::pair{"logical_or", rd::MaterialOp::LogicalOr},
        std::pair{"logical_not", rd::MaterialOp::LogicalNot},
        std::pair{"texture_ref", rd::MaterialOp::TextureRef},
        std::pair{"sampler_ref", rd::MaterialOp::SamplerRef},
        std::pair{"context", rd::MaterialOp::Context},
        std::pair{"sample_grad", rd::MaterialOp::SampleGrad},
        std::pair{"sample_level", rd::MaterialOp::SampleLevel},
        std::pair{"decode_normal_xy", rd::MaterialOp::DecodeNormalXY},
        std::pair{"normal_rnm", rd::MaterialOp::NormalRnm},
        std::pair{"detail_fade", rd::MaterialOp::DetailFade},
        std::pair{"tangent_to_world", rd::MaterialOp::TangentToWorld},
         std::pair{"seeded_noise_2d", rd::MaterialOp::SeededNoise2D},
         std::pair{"fresnel_schlick", rd::MaterialOp::FresnelSchlick},
         std::pair{"terrain_stochastic", rd::MaterialOp::TerrainStochastic},
          std::pair{"terrain_field", rd::MaterialOp::TerrainField},
          std::pair{"metallic_roughness_closure", rd::MaterialOp::MetallicRoughnessClosure},
          std::pair{"coat_closure", rd::MaterialOp::CoatClosure},
          std::pair{"anisotropy_closure", rd::MaterialOp::AnisotropyClosure},
          std::pair{"sheen_closure", rd::MaterialOp::SheenClosure},
          std::pair{"thin_transmission_closure", rd::MaterialOp::ThinTransmissionClosure},
          std::pair{"emission_closure", rd::MaterialOp::EmissionClosure},
          std::pair{"unlit_closure", rd::MaterialOp::UnlitClosure},
          std::pair{"subsurface_closure", rd::MaterialOp::SubsurfaceClosure},
        std::pair{"solid_transmission_closure", rd::MaterialOp::SolidTransmissionClosure},
        std::pair{"decal_fracture", rd::MaterialOp::DecalFracture},
        std::pair{"decal_organic", rd::MaterialOp::DecalOrganic}};
    for (const auto &[name, operation] : operations)
        if (op == name) return operation;
    Require(false, "unsupported operation");
    return {};
}

std::uint32_t ParseUint(const Json &value)
{
    Require(value.is_number_integer(), "uint value must be an integer");
    if (!value.is_number_unsigned())
        Require(value.get<std::int64_t>() >= 0, "uint value must be nonnegative");
    const auto integer = value.get<std::uint64_t>();
    Require(integer <= std::numeric_limits<std::uint32_t>::max(), "uint value out of bounds");
    return static_cast<std::uint32_t>(integer);
}

float ParseFloat(const Json &value)
{
    Require(value.is_number(), "float constant component must be numeric");
    const double number = value.get<double>();
    Require(std::isfinite(number) && number >= -16.0 && number <= 16.0,
            "float constant out of bounds");
    return static_cast<float>(number);
}

float ParseParameterFloat(const Json &value)
{
    Require(value.is_number(), "material parameter default must be numeric");
    const auto number = value.get<double>();
    Require(std::isfinite(number) && std::isfinite(static_cast<float>(number)),
            "material parameter default must be finite");
    return static_cast<float>(number);
}

rd::MaterialParameterType ParseParameterType(const Json &value)
{
    Require(value.is_string(), "material parameter type must be a string");
    if (value == "bool") return rd::MaterialParameterType::Bool;
    if (value == "uint") return rd::MaterialParameterType::Uint;
    if (value == "float") return rd::MaterialParameterType::Float;
    if (value == "float2") return rd::MaterialParameterType::Float2;
    if (value == "float3") return rd::MaterialParameterType::Float3;
    Require(value == "float4", "unsupported material parameter type");
    return rd::MaterialParameterType::Float4;
}

std::vector<rd::MaterialParameterDefinition> ParseParameterDeclarations(const Json &root)
{
    const auto version = root.at("version").get<unsigned>();
    std::vector<rd::MaterialParameterDefinition> result;
    if (version == 6 || version == 7)
    {
        const auto &definitions = root.at("parameters");
        Require(definitions.is_array() && definitions.size() <= 64,
                "material parameter declarations must be an array of at most 64 items");
        for (const auto &definition : definitions)
        {
            Require(definition.is_object() && definition.contains("id") &&
                    definition.contains("type") && definition.contains("default"),
                    "invalid material parameter declaration");
            const bool has_min = definition.contains("uint_min");
            const bool has_max = definition.contains("uint_max");
            Require(definition.size() == 3 + (has_min ? 1 : 0) + (has_max ? 1 : 0),
                    "unexpected material parameter declaration fields");
            Require(definition.at("id").is_string(), "material parameter ID must be a string");
            rd::MaterialParameterDefinition parameter;
            parameter.id = definition.at("id").get<std::string>();
            parameter.type = ParseParameterType(definition.at("type"));
            const auto components = parameter.type == rd::MaterialParameterType::Bool ||
                parameter.type == rd::MaterialParameterType::Uint ||
                parameter.type == rd::MaterialParameterType::Float ? 1u :
                parameter.type == rd::MaterialParameterType::Float2 ? 2u :
                parameter.type == rd::MaterialParameterType::Float3 ? 3u : 4u;
            const auto &value = definition.at("default");
            if (parameter.type == rd::MaterialParameterType::Bool)
            {
                Require(value.is_boolean(), "bool material parameter default must be boolean");
                parameter.default_words[0] = value.get<bool>() ? 1u : 0u;
            }
            else if (parameter.type == rd::MaterialParameterType::Uint)
                parameter.default_words[0] = ParseUint(value);
            else if (components == 1)
                parameter.default_words[0] = std::bit_cast<std::uint32_t>(ParseParameterFloat(value));
            else
            {
                Require(value.is_array() && value.size() == components,
                        "material parameter vector default component count mismatch");
                for (std::size_t lane = 0; lane < components; ++lane)
                    parameter.default_words[lane] = std::bit_cast<std::uint32_t>(
                        ParseParameterFloat(value[lane]));
            }
            if (has_min) parameter.uint_min = ParseUint(definition.at("uint_min"));
            if (has_max) parameter.uint_max = ParseUint(definition.at("uint_max"));
            result.push_back(std::move(parameter));
        }
    }
    else
    {
        const auto &parameters = root.at("parameters");
        Require(parameters.is_object(), "legacy material parameters must be an object");
        Require(parameters.empty() || (parameters.size() == 2 &&
                    parameters.contains("metallic") && parameters.contains("roughness_scale")),
                "legacy material parameter declarations must contain metallic and roughness_scale");
        const auto add = [&](std::string id, float value) {
            rd::MaterialParameterDefinition parameter;
            parameter.id = std::move(id);
            parameter.type = rd::MaterialParameterType::Float;
            parameter.default_words[0] = std::bit_cast<std::uint32_t>(value);
            parameter.uint_min = 0; parameter.uint_max = std::numeric_limits<std::uint32_t>::max();
            result.push_back(std::move(parameter));
        };
        add("metallic", parameters.empty() ? 0.0f : ParseParameterFloat(parameters.at("metallic")));
        add("roughness_scale", parameters.empty() ? 1.0f : ParseParameterFloat(parameters.at("roughness_scale")));
    }
    rd::ValidateMaterialParameters(result);
    const auto required_float = [&](std::string_view id, float lo, float hi) {
        const auto found = std::ranges::find(result, id, &rd::MaterialParameterDefinition::id);
        Require(found != result.end() && found->type == rd::MaterialParameterType::Float,
                "required material parameter declaration is missing or has the wrong type");
        const auto value = std::bit_cast<float>(found->default_words[0]);
        Require(std::isfinite(value) && value >= lo && value <= hi,
                "required material parameter default is out of range");
    };
    if (root.at("domain") == "decal")
    {
        required_float("target_roughness", 0.0f, 1.0f);
        required_float("roughness_blend", 0.0f, 1.0f);
    }
    else
    {
        required_float("metallic", 0.0f, 1.0f);
        required_float("roughness_scale", 0.0f, 2.0f);
    }
    return result;
}

rd::MaterialTextureSemantic ParseSemantic(const Json &value)
{
    if (value == "decal_structure") return rd::MaterialTextureSemantic::DecalStructure;
    if (value == "decal_mask") return rd::MaterialTextureSemantic::DecalMask;
    if (value == "gaussian") return rd::MaterialTextureSemantic::Gaussian;
    if (value == "base_color") return rd::MaterialTextureSemantic::BaseColor;
    if (value == "surface") return rd::MaterialTextureSemantic::Surface;
    if (value == "normal") return rd::MaterialTextureSemantic::Normal;
    if (value == "detail") return rd::MaterialTextureSemantic::Detail;
    if (value == "height") return rd::MaterialTextureSemantic::Height;
    if (value == "histogram") return rd::MaterialTextureSemantic::Histogram;
    Require(value == "roughness", "unsupported texture semantic");
    return rd::MaterialTextureSemantic::Roughness;
}

std::vector<rd::MaterialTextureDefinition> ParseTextures(const Json &root)
{
    std::vector<rd::MaterialTextureDefinition> textures;
    if (root.at("version") == 3 || root.at("version") == 4 || root.at("version") == 5 ||
        root.at("version") == 6 || root.at("version") == 7)
    {
        Require(root.at("textures").is_array(), "texture declarations must be an array");
        for (const auto &definition : root.at("textures"))
        {
            ExactKeys(definition, {"id", "product_id", "dimension", "semantic"});
            Require(definition.at("id").is_string() && definition.at("product_id").is_string() &&
                    definition.at("dimension") == "texture2d_array", "invalid texture declaration");
            const auto id = definition.at("id").get<std::string>();
            Require(ValidId(id), "invalid texture declaration ID");
            textures.push_back({id, definition.at("product_id").get<std::string>(),
                rd::MaterialTextureDimension::Texture2DArray, ParseSemantic(definition.at("semantic"))});
        }
    }
    else
    {
        // Explicit legacy DDS role mapping is content-only. Runtime metadata uses
        // typed packages, whose own upstream dependencies retain the DDS identity.
        const auto &definitions = root.at("textures");
        ExactKeys(definitions, {"base_color", "surface", "normal", "detail"});
        constexpr std::array roles{"base_color", "surface", "normal", "detail"};
        for (std::size_t slot = 0; slot < roles.size(); ++slot)
        {
            Require(definitions.at(roles[slot]).is_string(), "legacy texture ID must be a string");
            const auto source = definitions.at(roles[slot]).get<std::string>();
            if (source.empty()) { Require(slot == 3, "required legacy texture absent"); continue; }
            constexpr std::string_view prefix = "environment/";
            Require(source.starts_with(prefix) && source.ends_with(".dds"), "invalid legacy texture ID");
            const auto stem = source.substr(prefix.size(), source.size() - prefix.size() - 4);
            auto semantic = slot == 0 ? rd::MaterialTextureSemantic::BaseColor :
                slot == 1 ? rd::MaterialTextureSemantic::Surface :
                slot == 2 ? rd::MaterialTextureSemantic::Normal : rd::MaterialTextureSemantic::Detail;
            const std::string suffix = slot == 0 ? "_basecolor" : slot == 2 ? "_normal" :
                slot == 3 ? "_detail" : stem.ends_with("_roughness") ? "_roughness" : "_surface";
            Require(stem.ends_with(suffix), "legacy texture ID does not match role");
            if (slot == 1 && suffix == "_roughness") semantic = rd::MaterialTextureSemantic::Roughness;
            textures.push_back({roles[slot], "render/texture/" + stem + ".rpkg",
                rd::MaterialTextureDimension::Texture2DArray, semantic});
        }
    }
    if (IsDecalMaterialName(root.at("id").get<std::string>()))
    {
        Require(textures.size() == 1, "decal requires one texture declaration");
        rd::ValidateDecalMaterialTextureDefinition(textures.front());
    }
    else if (!textures.empty() || !IsHeroMaterialName(root.at("id").get<std::string>()))
        rd::ValidateMaterialTextureDefinitions(textures);
    return textures;
}

std::vector<rd::MaterialSamplerDefinition> ParseSamplers(const Json &root)
{
    std::vector<rd::MaterialSamplerDefinition> samplers;
    if (root.at("version") != 3 && root.at("version") != 4 && root.at("version") != 5 &&
        root.at("version") != 6 && root.at("version") != 7) return samplers;
    Require((root.at("samplers").is_array() && !root.at("samplers").empty() &&
             root.at("samplers").size() <= 4) ||
            (IsHeroMaterialName(root.at("id").get<std::string>()) &&
             root.at("samplers").is_array() && root.at("samplers").empty()), "expected one to four samplers");
    for (const auto &definition : root.at("samplers"))
    {
        ExactKeys(definition, {"id", "profile"});
        Require(definition.at("id").is_string() && definition.at("profile").is_string(),
                "invalid sampler declaration");
        rd::MaterialSamplerProfile profile{};
        const auto &value = definition.at("profile");
        if (value == "linear_clamp") profile = rd::MaterialSamplerProfile::LinearClamp;
        else if (value == "linear_wrap") profile = rd::MaterialSamplerProfile::LinearWrap;
        else if (value == "environment_anisotropic_wrap") profile = rd::MaterialSamplerProfile::EnvironmentAnisotropicWrap;
        else { Require(value == "foliage_anisotropic_clamp", "unsupported sampler profile");
            profile = rd::MaterialSamplerProfile::FoliageAnisotropicClamp; }
        samplers.push_back({definition.at("id").get<std::string>(), profile});
    }
    return samplers;
}

rd::MaterialContextInput ParseContext(const Json &value)
{
    constexpr std::array names{"uv", "d_uv_dx", "d_uv_dy", "layer", "actual_world_position",
        "d_actual_world_dx", "d_actual_world_dy", "stable_world_position", "d_stable_world_dx",
        "d_stable_world_dy", "normal", "tangent", "eye_distance", "sampled_base_color",
        "sampled_roughness", "sampled_alpha", "roughness_scale", "sampled_ao", "shading_normal",
        "mesh", "seed", "view_direction", "metallic", "sampled_emission", "sampled_metallic",
        "decal_tint_hdr", "decal_projector", "decal_shape"};
    for (std::size_t i = 0; i < names.size(); ++i)
        if (value == names[i]) return static_cast<rd::MaterialContextInput>(i);
    Require(false, "unknown material context input"); return {};
}

void LowerLegacyMaterialSamples(MaterialDefinition &definition)
{
    const auto old = std::move(definition.graph);
    rd::MaterialGraph graph;
    graph.kind = old.kind;
    graph.parameters = old.parameters;
    graph.textures = old.textures;
    graph.samplers.push_back({"legacy_sampler", definition.material.alpha_mask
        ? rd::MaterialSamplerProfile::FoliageAnisotropicClamp
        : rd::MaterialSamplerProfile::EnvironmentAnisotropicWrap});
    std::unordered_set<std::string> ids;
    for (const auto &node : old.nodes) ids.insert(node.id);
    const auto add = [&](rd::MaterialType type, rd::MaterialOp op,
                         std::initializer_list<std::size_t> inputs = {}) {
        rd::MaterialNode node;
        node.type = type; node.op = op;
        auto suffix = graph.nodes.size();
        do { node.id = "_hs_legacy_" + std::to_string(suffix++); }
        while (!ids.insert(node.id).second);
        std::copy(inputs.begin(), inputs.end(), node.inputs.begin());
        graph.nodes.push_back(std::move(node));
        return graph.nodes.size() - 1;
    };
    const auto context = [&](rd::MaterialType type, rd::MaterialContextInput input) {
        const auto index = add(type, rd::MaterialOp::Context);
        graph.nodes[index].context_input = input;
        return index;
    };
    const auto uv = context(rd::MaterialType::Float2, rd::MaterialContextInput::Uv);
    const auto dx = context(rd::MaterialType::Float2, rd::MaterialContextInput::DUvDx);
    const auto dy = context(rd::MaterialType::Float2, rd::MaterialContextInput::DUvDy);
    const auto layer = context(rd::MaterialType::Uint, rd::MaterialContextInput::Layer);
    const auto eye = context(rd::MaterialType::Float, rd::MaterialContextInput::EyeDistance);
    const auto sampler = add(rd::MaterialType::SamplerRef, rd::MaterialOp::SamplerRef);
    graph.nodes[sampler].declaration_index = 0;
    std::vector<std::size_t> samples;
    for (std::size_t slot = 0; slot < graph.textures.size(); ++slot)
    {
        const auto texture = add(rd::MaterialType::TextureRef, rd::MaterialOp::TextureRef);
        graph.nodes[texture].declaration_index = slot;
        samples.push_back(add(rd::MaterialType::Float4, rd::MaterialOp::SampleGrad,
                             {texture, sampler, uv, layer, dx, dy}));
    }
    const auto extract = [&](std::size_t input, std::uint32_t component) {
        const auto index = add(rd::MaterialType::Float, rd::MaterialOp::Extract, {input});
        graph.nodes[index].component = component;
        return index;
    };
    const auto color_r = extract(samples[0], 0), color_g = extract(samples[0], 1),
               color_b = extract(samples[0], 2);
    const auto color = add(rd::MaterialType::Float3, rd::MaterialOp::Construct,
                           {color_r, color_g, color_b});
    const auto alpha = extract(samples[0], 3);
    const auto roughness = extract(samples[1], 0);
    const auto decode = [&](std::size_t input) {
        const auto x = extract(input, 0), y = extract(input, 1);
        const auto xy = add(rd::MaterialType::Float2, rd::MaterialOp::Construct, {x, y});
        return add(rd::MaterialType::Float3, rd::MaterialOp::DecodeNormalXY, {xy});
    };
    graph.normal_output = decode(samples[2]);
    if (samples.size() == 4)
    {
        const auto detail = decode(samples[3]);
        const auto faded = add(rd::MaterialType::Float3, rd::MaterialOp::DetailFade, {detail, eye});
        graph.normal_output = add(rd::MaterialType::Float3, rd::MaterialOp::NormalRnm,
                                  {graph.normal_output, faded});
    }
    if (graph.textures[1].semantic == rd::MaterialTextureSemantic::Surface)
        graph.ao_output = extract(samples[1], 1);
    else
    {
        graph.ao_output = add(rd::MaterialType::Float, rd::MaterialOp::Constant);
        graph.nodes[graph.ao_output].value[0] = 1.0f;
    }
    const auto offset = graph.nodes.size();
    for (auto node : old.nodes)
    {
        for (std::size_t slot = 0; slot < rd::MaterialInputCount(node.op, node.type); ++slot)
            node.inputs[slot] += offset;
        if (node.op == rd::MaterialOp::SampledBaseColor || node.op == rd::MaterialOp::SampledRoughness ||
            node.op == rd::MaterialOp::SampledAlpha)
        {
            const auto input = node.op == rd::MaterialOp::SampledBaseColor ? color :
                node.op == rd::MaterialOp::SampledRoughness ? roughness : alpha;
            node.op = rd::MaterialOp::Convert;
            node.inputs[0] = input;
        }
        graph.nodes.push_back(std::move(node));
    }
    graph.base_color_output = offset + old.base_color_output;
    graph.roughness_output = offset + old.roughness_output;
    if (graph.kind == rd::MaterialGraphKind::Masked) graph.opacity_output = offset + old.opacity_output;
    rd::ValidateEnvironmentMaterialGraph(graph);
    definition.graph = std::move(graph);
}
} // namespace

rd::MaterialGraph ParseMaterialGraph(std::string_view name, std::string_view json_text,
                                     rd::MaterialGraphKind kind)
{
    const auto root = Json::parse(json_text);
    const bool hero = IsHeroMaterialName(name);
    const bool decal = kind == rd::MaterialGraphKind::Decal;
    const bool transparent = kind == rd::MaterialGraphKind::Translucent || kind == rd::MaterialGraphKind::HeroRefractive;
    const bool context = root.at("version") == 3 || root.at("version") == 4 ||
                         root.at("version") == 5 || root.at("version") == 6 || root.at("version") == 7;
    if (context) ExactKeys(root, {"id", "version", "domain", "shading_model", "parameters",
                     "textures", "samplers", "nodes", "outputs", "render_flags"});
    else ExactKeys(root, {"id", "version", "domain", "shading_model", "parameters",
                     "textures", "nodes", "outputs", "render_flags"});
    Require(root.at("id").is_string() &&
            std::string_view(root.at("id").get_ref<const std::string &>()) == name &&
            (root.at("version") == 2 || context) &&
            (root.at("version") != 4 || kind == rd::MaterialGraphKind::Terrain) &&
            root.at("domain") == (decal ? "decal" : transparent ? "translucent" : "surface") &&
            root.at("shading_model") == "metallic_roughness", "unsupported material");
    Require(!hero || root.at("version") == 7, "hero context adapters require material version7");
    const bool has_color = kind != rd::MaterialGraphKind::Leaf0;
    const bool has_opacity = kind == rd::MaterialGraphKind::Leaf0 ||
                             kind == rd::MaterialGraphKind::Masked || transparent;
    const auto &flags = root.at("render_flags");
    rd::DecalReceiverPolicy receivers;
    if (decal)
    {
        Require(root.at("version") == 7, "decal requires typed material version7");
        ExactKeys(flags, {"alpha_mode", "receivers"});
        Require(flags.at("alpha_mode") == "mask", "decal profile requires masked evaluation");
        const auto &policy = flags.at("receivers");
        ExactKeys(policy, {"models", "required_surface_flags", "forbidden_surface_flags", "normal_filter"});
        Require(policy.at("models").is_array(), "decal receiver models must be an array");
        receivers.model_mask = 0;
        for (const auto &model : policy.at("models"))
        {
            const auto bit = model == "cel" ? 1u << 1u : model == "environment" ? 1u << 2u :
                model == "jelly" ? 1u << 3u : 0u;
            Require(bit != 0 && (receivers.model_mask & bit) == 0, "unknown or duplicate Surface receiver model");
            receivers.model_mask |= bit;
        }
        receivers.required_surface_flags = ParseUint(policy.at("required_surface_flags"));
        receivers.forbidden_surface_flags = ParseUint(policy.at("forbidden_surface_flags"));
        Require(policy.at("normal_filter") == "any" || policy.at("normal_filter") == "ground_facing",
                "unknown decal receiver normal filter");
        receivers.normal_filter = policy.at("normal_filter") == "any"
            ? rd::DecalReceiverNormalFilter::Any : rd::DecalReceiverNormalFilter::GroundFacing;
        rd::ValidateDecalReceiverPolicy(receivers);
    }
    else
    {
        ExactKeys(flags, {"two_sided", "alpha_mode"});
        Require(flags.at("two_sided") == (hero ? transparent : has_opacity) &&
                flags.at("alpha_mode") == (transparent ? "blend" : has_opacity ? "mask" : "opaque"),
                "graph kind disagrees with render flags");
    }
    const auto &definitions = root.at("nodes");
    Require(definitions.is_array() && !definitions.empty() && definitions.size() <= rd::kMaxMaterialGraphNodes,
            "node count out of bounds");
    const auto textures = context ? ParseTextures(root) : std::vector<rd::MaterialTextureDefinition>{};
    const auto samplers = ParseSamplers(root);
    Require(!hero || (textures.empty() && samplers.empty()), "hero graph must use the native sampled-context adapter");
    const auto parameters = ParseParameterDeclarations(root);
    std::vector<Pending> pending;
    std::unordered_map<std::string, std::size_t> ids;
    pending.reserve(definitions.size());
    for (const auto &definition : definitions)
    {
        Require(definition.is_object() && definition.contains("op") &&
                definition.at("op").is_string(), "invalid node operation");
        Pending item;
        item.node.op = ParseOp(definition.at("op").get<std::string>());
        Require(definition.contains("type"), "missing node type");
        item.node.type = ParseType(definition.at("type"));
        switch (item.node.op)
        {
        case rd::MaterialOp::DecalFracture: case rd::MaterialOp::DecalOrganic:
            ExactKeys(definition, {"id", "op", "type", "inputs"});
            Require(decal && definition.at("inputs").is_array() && definition.at("inputs").size() == 15,
                    "projected decal requires fifteen typed operands in the decal domain");
            for (std::size_t slot = 0; slot < 15; ++slot)
                item.inputs[slot] = ParseEdge(definition.at("inputs")[slot]);
            break;
        case rd::MaterialOp::TextureRef: case rd::MaterialOp::SamplerRef:
        {
            ExactKeys(definition, {"id", "op", "type", "declaration"});
             Require(context && definition.at("declaration").is_string(), "reference requires a declaration ID");
            const auto id = definition.at("declaration").get<std::string>();
            if (item.node.op == rd::MaterialOp::TextureRef)
            {
                const auto found = std::ranges::find(textures, id, &rd::MaterialTextureDefinition::id);
                Require(found != textures.end(), "unknown texture declaration");
                item.node.declaration_index = static_cast<std::size_t>(found - textures.begin());
            }
            else
            {
                const auto found = std::ranges::find(samplers, id, &rd::MaterialSamplerDefinition::id);
                Require(found != samplers.end(), "unknown sampler declaration");
                item.node.declaration_index = static_cast<std::size_t>(found - samplers.begin());
            }
            break;
        }
        case rd::MaterialOp::Context:
            ExactKeys(definition, {"id", "op", "type", "name"});
            Require(context, "context requires material version3");
            item.node.context_input = ParseContext(definition.at("name"));
            if (item.node.context_input == rd::MaterialContextInput::Metallic ||
                item.node.context_input == rd::MaterialContextInput::RoughnessScale)
            {
                const auto parameter_name = item.node.context_input == rd::MaterialContextInput::Metallic
                    ? std::string_view{"metallic"} : std::string_view{"roughness_scale"};
                const auto found = std::ranges::find(parameters, parameter_name,
                    &rd::MaterialParameterDefinition::id);
                Require(found != parameters.end(), "numeric context has no parameter declaration");
                item.node.op = rd::MaterialOp::Parameter;
                item.node.declaration_index = static_cast<std::size_t>(found - parameters.begin());
            }
            break;
        case rd::MaterialOp::Parameter:
        {
            ExactKeys(definition, {"id", "op", "type", "name"});
            if (!context)
            {
                Require(definition.at("name") == "roughness_scale", "unknown parameter");
                item.node.op = rd::MaterialOp::RoughnessScale;
                break;
            }
            Require(definition.at("name").is_string(), "material parameter name must be a string");
            const auto parameter_name = definition.at("name").get<std::string>();
            const auto found = std::ranges::find(parameters, parameter_name,
                &rd::MaterialParameterDefinition::id);
            Require(found != parameters.end(), "unknown material parameter declaration");
            item.node.declaration_index = static_cast<std::size_t>(found - parameters.begin());
            const auto expected = found->type == rd::MaterialParameterType::Bool ? rd::MaterialType::Bool :
                found->type == rd::MaterialParameterType::Uint ? rd::MaterialType::Uint :
                found->type == rd::MaterialParameterType::Float ? rd::MaterialType::Float :
                found->type == rd::MaterialParameterType::Float2 ? rd::MaterialType::Float2 :
                found->type == rd::MaterialParameterType::Float3 ? rd::MaterialType::Float3 : rd::MaterialType::Float4;
            Require(item.node.type == expected, "material parameter node type disagrees with declaration");
            break;
        }
        case rd::MaterialOp::SampleGrad: case rd::MaterialOp::SampleLevel:
        {
            if (item.node.op == rd::MaterialOp::SampleGrad)
                ExactKeys(definition, {"id", "op", "type", "texture", "sampler", "uv", "layer", "dx", "dy"});
            else ExactKeys(definition, {"id", "op", "type", "texture", "sampler", "uv", "layer", "lod"});
            Require(context, "sample requires material version3");
            constexpr std::array fields{"texture", "sampler", "uv", "layer", "dx", "dy"};
            for (std::size_t i = 0; i < rd::MaterialInputCount(item.node.op, item.node.type); ++i)
                item.inputs[i] = ParseEdge(definition.at(i == 4 && item.node.op == rd::MaterialOp::SampleLevel ? "lod" : fields[i]));
            break;
        }
        case rd::MaterialOp::DecodeNormalXY:
            ExactKeys(definition, {"id", "op", "type", "input"});
            item.inputs[0] = ParseEdge(definition.at("input")); break;
        case rd::MaterialOp::NormalRnm:
            ExactKeys(definition, {"id", "op", "type", "a", "b"});
            item.inputs[0] = ParseEdge(definition.at("a"));
            item.inputs[1] = ParseEdge(definition.at("b")); break;
        case rd::MaterialOp::DetailFade:
            ExactKeys(definition, {"id", "op", "type", "input", "eye_distance"});
            item.inputs[0] = ParseEdge(definition.at("input"));
            item.inputs[1] = ParseEdge(definition.at("eye_distance")); break;
        case rd::MaterialOp::TangentToWorld:
            ExactKeys(definition, {"id", "op", "type", "input", "normal", "tangent"});
            item.inputs[0] = ParseEdge(definition.at("input"));
            item.inputs[1] = ParseEdge(definition.at("normal"));
            item.inputs[2] = ParseEdge(definition.at("tangent")); break;
        case rd::MaterialOp::SeededNoise2D:
            ExactKeys(definition, {"id", "op", "type", "position", "seed"});
            item.inputs[0] = ParseEdge(definition.at("position"));
            item.inputs[1] = ParseEdge(definition.at("seed")); break;
        case rd::MaterialOp::FresnelSchlick:
            ExactKeys(definition, {"id", "op", "type", "f0", "cos_theta"});
            item.inputs[0] = ParseEdge(definition.at("f0"));
            item.inputs[1] = ParseEdge(definition.at("cos_theta")); break;
        case rd::MaterialOp::TerrainStochastic:
        {
            ExactKeys(definition, {"id", "op", "type", "textures", "samplers", "world",
                                   "world_dx", "world_dy", "mesh", "seed", "view_direction",
                                   "eye_distance"});
            Require(context && item.node.type == rd::MaterialType::TerrainResult &&
                    definition.at("textures").is_array() && definition.at("textures").size() == 6 &&
                    definition.at("samplers").is_array() && definition.at("samplers").size() == 2,
                    "terrain stochastic requires six textures and two samplers");
            for (std::size_t i = 0; i < 6; ++i)
                item.inputs[i] = ParseEdge(definition.at("textures")[i]);
            for (std::size_t i = 0; i < 2; ++i)
                item.inputs[6 + i] = ParseEdge(definition.at("samplers")[i]);
            constexpr std::array fields{"world", "world_dx", "world_dy", "mesh", "seed",
                                        "view_direction", "eye_distance"};
            for (std::size_t i = 0; i < fields.size(); ++i)
                item.inputs[8 + i] = ParseEdge(definition.at(fields[i]));
            break;
        }
        case rd::MaterialOp::TerrainField:
            ExactKeys(definition, {"id", "op", "type", "input", "field"});
            Require(context && definition.at("field").is_string(), "terrain field requires a field name");
            if (definition.at("field") == "base_color") item.node.terrain_field = rd::TerrainField::BaseColor;
            else if (definition.at("field") == "normal") item.node.terrain_field = rd::TerrainField::Normal;
            else if (definition.at("field") == "detail") item.node.terrain_field = rd::TerrainField::Detail;
            else if (definition.at("field") == "roughness") item.node.terrain_field = rd::TerrainField::Roughness;
            else { Require(definition.at("field") == "ao", "unsupported terrain result field");
                item.node.terrain_field = rd::TerrainField::Ao; }
            item.inputs[0] = ParseEdge(definition.at("input")); break;
        case rd::MaterialOp::MetallicRoughnessClosure:
            ExactKeys(definition, {"id", "op", "type", "base_color", "normal", "roughness",
                                   "metallic", "ao", "opacity"});
            Require((root.at("version") == 5 || root.at("version") == 6 || root.at("version") == 7) && context &&
                        item.node.type == rd::MaterialType::SurfaceClosure,
                    "metallic roughness closure requires a typed context graph");
            item.inputs[0] = ParseEdge(definition.at("base_color"));
            item.inputs[1] = ParseEdge(definition.at("normal"));
            item.inputs[2] = ParseEdge(definition.at("roughness"));
            item.inputs[3] = ParseEdge(definition.at("metallic"));
            item.inputs[4] = ParseEdge(definition.at("ao"));
            item.inputs[5] = ParseEdge(definition.at("opacity"));
            break;
        case rd::MaterialOp::CoatClosure:
            ExactKeys(definition, {"id", "op", "type", "base", "weight", "roughness"});
            Require(root.at("version") == 7, "advanced closures require material version7");
            item.inputs[0] = ParseEdge(definition.at("base"));
            item.inputs[1] = ParseEdge(definition.at("weight"));
            item.inputs[2] = ParseEdge(definition.at("roughness"));
            break;
        case rd::MaterialOp::AnisotropyClosure:
            ExactKeys(definition, {"id", "op", "type", "base", "amount", "tangent"});
            Require(root.at("version") == 7, "advanced closures require material version7");
            item.inputs[0] = ParseEdge(definition.at("base"));
            item.inputs[1] = ParseEdge(definition.at("amount"));
            item.inputs[2] = ParseEdge(definition.at("tangent"));
            break;
        case rd::MaterialOp::SheenClosure:
            ExactKeys(definition, {"id", "op", "type", "base", "weight", "tint", "roughness"});
            Require(root.at("version") == 7, "advanced closures require material version7");
            item.inputs[0] = ParseEdge(definition.at("base"));
            item.inputs[1] = ParseEdge(definition.at("weight"));
            item.inputs[2] = ParseEdge(definition.at("tint"));
            item.inputs[3] = ParseEdge(definition.at("roughness"));
            break;
        case rd::MaterialOp::ThinTransmissionClosure:
            ExactKeys(definition, {"id", "op", "type", "base", "weight", "tint"});
            Require(root.at("version") == 7, "advanced closures require material version7");
            item.inputs[0] = ParseEdge(definition.at("base"));
            item.inputs[1] = ParseEdge(definition.at("weight"));
            item.inputs[2] = ParseEdge(definition.at("tint"));
            break;
        case rd::MaterialOp::EmissionClosure:
            ExactKeys(definition, {"id", "op", "type", "base", "emission"});
            Require(root.at("version") == 7, "advanced closures require material version7");
            item.inputs[0] = ParseEdge(definition.at("base"));
            item.inputs[1] = ParseEdge(definition.at("emission"));
            break;
        case rd::MaterialOp::UnlitClosure:
            ExactKeys(definition, {"id", "op", "type", "color", "opacity"});
            Require(root.at("version") == 7, "advanced closures require material version7");
            item.inputs[0] = ParseEdge(definition.at("color"));
            item.inputs[1] = ParseEdge(definition.at("opacity"));
            break;
        case rd::MaterialOp::SubsurfaceClosure:
            ExactKeys(definition, {"id", "op", "type", "base", "weight", "radius_m", "thickness_m"});
            Require(root.at("version") == 7, "subsurface closure requires material version7");
            item.inputs[0] = ParseEdge(definition.at("base"));
            item.inputs[1] = ParseEdge(definition.at("weight"));
            item.inputs[2] = ParseEdge(definition.at("radius_m"));
            item.inputs[3] = ParseEdge(definition.at("thickness_m"));
            break;
        case rd::MaterialOp::SolidTransmissionClosure:
            ExactKeys(definition, {"id", "op", "type", "base", "weight", "ior", "absorption_per_m", "thickness_m"});
            Require(root.at("version") == 7, "solid transmission closure requires material version7");
            item.inputs[0] = ParseEdge(definition.at("base"));
            item.inputs[1] = ParseEdge(definition.at("weight"));
            item.inputs[2] = ParseEdge(definition.at("ior"));
            item.inputs[3] = ParseEdge(definition.at("absorption_per_m"));
            item.inputs[4] = ParseEdge(definition.at("thickness_m"));
            break;
        case rd::MaterialOp::SampledBaseColor:
        case rd::MaterialOp::SampledRoughness:
        case rd::MaterialOp::SampledAlpha:
            ExactKeys(definition, {"id", "op", "type"});
            break;
        case rd::MaterialOp::RoughnessScale:
            ExactKeys(definition, {"id", "op", "type", "name"});
            Require(definition.at("name") == "roughness_scale", "unknown parameter");
            break;
        case rd::MaterialOp::Constant:
            ExactKeys(definition, {"id", "op", "type", "value"});
            break;
        case rd::MaterialOp::Add: case rd::MaterialOp::Subtract:
        case rd::MaterialOp::Multiply: case rd::MaterialOp::Divide:
        case rd::MaterialOp::Min: case rd::MaterialOp::Max:
        case rd::MaterialOp::Less: case rd::MaterialOp::LessEqual:
        case rd::MaterialOp::Equal: case rd::MaterialOp::LogicalAnd:
        case rd::MaterialOp::LogicalOr:
            ExactKeys(definition, {"id", "op", "type", "a", "b"});
            item.inputs[0] = ParseEdge(definition.at("a"));
            item.inputs[1] = ParseEdge(definition.at("b"));
            break;
        case rd::MaterialOp::Saturate: case rd::MaterialOp::Negate:
        case rd::MaterialOp::Convert: case rd::MaterialOp::LogicalNot:
            ExactKeys(definition, {"id", "op", "type", "input"});
            item.inputs[0] = ParseEdge(definition.at("input"));
            break;
        case rd::MaterialOp::Lerp:
            ExactKeys(definition, {"id", "op", "type", "a", "b", "t"});
            item.inputs[0] = ParseEdge(definition.at("a"));
            item.inputs[1] = ParseEdge(definition.at("b"));
            item.inputs[2] = ParseEdge(definition.at("t"));
            break;
        case rd::MaterialOp::Clamp:
            ExactKeys(definition, {"id", "op", "type", "input", "min", "max"});
            item.inputs[0] = ParseEdge(definition.at("input"));
            item.inputs[1] = ParseEdge(definition.at("min"));
            item.inputs[2] = ParseEdge(definition.at("max"));
            break;
        case rd::MaterialOp::Select:
            ExactKeys(definition, {"id", "op", "type", "condition", "a", "b"});
            item.inputs[0] = ParseEdge(definition.at("condition"));
            item.inputs[1] = ParseEdge(definition.at("a"));
            item.inputs[2] = ParseEdge(definition.at("b"));
            break;
        case rd::MaterialOp::Extract:
            ExactKeys(definition, {"id", "op", "type", "input", "component"});
            item.inputs[0] = ParseEdge(definition.at("input"));
            item.node.component = ParseUint(definition.at("component"));
            break;
        case rd::MaterialOp::Construct:
        {
            ExactKeys(definition, {"id", "op", "type", "components"});
            const auto &components = definition.at("components");
            const auto count = rd::MaterialComponentCount(item.node.type);
            Require(count >= 2 && components.is_array() && components.size() == count,
                    "construct requires exactly one scalar edge per vector component");
            for (std::size_t i = 0; i < count; ++i)
                item.inputs[i] = ParseEdge(components[i]);
            break;
        }
        default: Require(false, "unsupported operation");
        }
        Require(definition.at("id").is_string(), "node ID must be a string");
        item.node.id = definition.at("id").get<std::string>();
        Require(ValidId(item.node.id) && ids.emplace(item.node.id, pending.size()).second,
                "invalid or duplicate node ID");
        if (item.node.op == rd::MaterialOp::Constant)
        {
            const auto &value = definition.at("value");
            if (item.node.type == rd::MaterialType::Bool)
            {
                Require(value.is_boolean(), "bool constant must be a boolean");
                item.node.bool_value = value.get<bool>();
            }
            else if (item.node.type == rd::MaterialType::Uint)
                item.node.uint_value = ParseUint(value);
            else if (item.node.type == rd::MaterialType::Float)
                item.node.value[0] = ParseFloat(value);
            else
            {
                const auto count = rd::MaterialComponentCount(item.node.type);
                Require(value.is_array() && value.size() == count,
                        "vector constant component count mismatch");
                for (std::size_t i = 0; i < count; ++i)
                    item.node.value[i] = ParseFloat(value[i]);
            }
        }
        pending.push_back(std::move(item));
    }
    const auto &outputs = root.at("outputs");
    const bool typed_closure = root.at("version") == 5 || root.at("version") == 6 || root.at("version") == 7;
    Edge first, roughness, opacity, normal, ao, closure;
    if (typed_closure)
    {
        ExactKeys(outputs, {"surface", "closure", "normal_space"});
        Require(outputs.at("surface") == "typed_closure", "unsupported surface output");
        closure = ParseEdge(outputs.at("closure"));
        Require(closure.type == rd::MaterialType::SurfaceClosure && ids.contains(closure.id),
                "invalid typed closure output edge");
        Require(outputs.at("normal_space") == "tangent" || outputs.at("normal_space") == "world",
                "invalid normal output space");
    }
    else
    {
        if (context && has_color && has_opacity)
            ExactKeys(outputs, {"surface", "base_color", "roughness", "opacity", "normal", "normal_space", "ao"});
        else if (context && has_color)
            ExactKeys(outputs, {"surface", "base_color", "roughness", "normal", "normal_space", "ao"});
        else if (has_color && has_opacity)
            ExactKeys(outputs, {"surface", "base_color", "roughness", "opacity"});
        else if (has_color)
            ExactKeys(outputs, {"surface", "base_color", "roughness"});
        else ExactKeys(outputs, {"surface", "opacity"});
        Require(outputs.at("surface") == "builtin_metallic_roughness", "unsupported surface output");
        first = ParseEdge(outputs.at(has_color ? "base_color" : "opacity"));
        Require(first.type == (has_color ? rd::MaterialType::Float3 : rd::MaterialType::Float) &&
                ids.contains(first.id), "invalid first output edge");
        if (has_color)
        {
            roughness = ParseEdge(outputs.at("roughness"));
            Require(roughness.type == rd::MaterialType::Float && ids.contains(roughness.id),
                    "invalid roughness output edge");
        }
        if (has_color && has_opacity)
        {
            opacity = ParseEdge(outputs.at("opacity"));
            Require(opacity.type == rd::MaterialType::Float && ids.contains(opacity.id),
                    "invalid opacity output edge");
        }
        if (context)
        {
            Require(has_color, "context graph requires full surface outputs");
            normal = ParseEdge(outputs.at("normal"));
            ao = ParseEdge(outputs.at("ao"));
            Require(normal.type == rd::MaterialType::Float3 && ids.contains(normal.id) &&
                    ao.type == rd::MaterialType::Float && ids.contains(ao.id), "invalid normal or AO output");
            Require(outputs.at("normal_space") == "tangent" || outputs.at("normal_space") == "world",
                    "invalid normal output space");
        }
    }

    rd::MaterialGraph graph;
    graph.kind = kind;
    graph.decal_receivers = receivers;
    graph.parameters = parameters;
    graph.textures = textures;
    graph.samplers = samplers;
    if (context) graph.normal_space = outputs.at("normal_space") == "world"
        ? rd::MaterialNormalSpace::World : rd::MaterialNormalSpace::Tangent;
    graph.nodes.reserve(pending.size());
    std::vector<unsigned char> state(pending.size());
    std::vector<std::size_t> ordered_index(pending.size());
    std::function<void(std::size_t)> visit = [&](std::size_t index) {
        Require(state[index] != 1, "cycle detected");
        if (state[index] == 2) return;
        state[index] = 1;
        auto node = pending[index].node;
        for (std::size_t slot = 0; slot < rd::MaterialInputCount(node.op, node.type); ++slot)
        {
            const auto &edge = pending[index].inputs[slot];
            const auto found = ids.find(edge.id);
            Require(found != ids.end(), "edge references unknown node");
            Require(pending[found->second].node.type == edge.type,
                    "edge type mismatch");
            visit(found->second);
            node.inputs[slot] = ordered_index[found->second];
        }
        ordered_index[index] = graph.nodes.size();
        graph.nodes.push_back(std::move(node));
        state[index] = 2;
    };
    if (typed_closure) visit(ids.at(closure.id));
    else
    {
        visit(ids.at(first.id));
        if (has_color) visit(ids.at(roughness.id));
        if (has_color && has_opacity) visit(ids.at(opacity.id));
        if (context) { visit(ids.at(normal.id)); visit(ids.at(ao.id)); }
    }
    // Validate dead nodes too, including their cycles and type constraints;
    // the emitter prunes them separately for every output.
    for (std::size_t index = 0; index < pending.size(); ++index) visit(index);
    if (typed_closure)
    {
        graph.base_color_output = graph.roughness_output = graph.opacity_output = rd::kNoMaterialInput;
        graph.closure_output = ordered_index[ids.at(closure.id)];
    }
    else if (has_color)
    {
        graph.base_color_output = ordered_index[ids.at(first.id)];
        graph.roughness_output = ordered_index[ids.at(roughness.id)];
        if (has_opacity) graph.opacity_output = ordered_index[ids.at(opacity.id)];
    }
    else
    {
        graph.opacity_output = ordered_index[ids.at(first.id)];
    }
    if (context && !typed_closure)
    {
        graph.normal_output = ordered_index[ids.at(normal.id)];
        graph.ao_output = ordered_index[ids.at(ao.id)];
    }
    if (context && !typed_closure && has_color)
    {
        const auto unique_id = [&](std::string base) {
            const auto used = [&](std::string_view candidate) {
                return std::ranges::any_of(graph.nodes, [&](const rd::MaterialNode &node) {
                    return node.id == candidate;
                });
            };
            if (!used(base)) return base;
            for (std::size_t suffix = 1;; ++suffix)
            {
                auto candidate = base + "_" + std::to_string(suffix);
                if (!used(candidate)) return candidate;
            }
        };
        rd::MaterialNode metallic;
        metallic.id = unique_id("legacy_metallic");
        metallic.op = rd::MaterialOp::Parameter;
        metallic.type = rd::MaterialType::Float;
        const auto metallic_parameter = std::ranges::find(graph.parameters, "metallic",
            &rd::MaterialParameterDefinition::id);
        Require(metallic_parameter != graph.parameters.end(),
                "legacy material graph has no metallic parameter declaration");
        metallic.declaration_index = static_cast<std::size_t>(metallic_parameter - graph.parameters.begin());
        graph.nodes.push_back(std::move(metallic));
        const auto metallic_output = graph.nodes.size() - 1;

        std::size_t opacity_output = graph.opacity_output;
        if (!has_opacity)
        {
            rd::MaterialNode opacity_node;
            opacity_node.id = unique_id("legacy_opacity");
            opacity_node.op = rd::MaterialOp::Constant;
            opacity_node.type = rd::MaterialType::Float;
            opacity_node.value[0] = 1.0f;
            graph.nodes.push_back(std::move(opacity_node));
            opacity_output = graph.nodes.size() - 1;
        }

        rd::MaterialNode closure_node;
        closure_node.id = unique_id("legacy_surface_closure");
        closure_node.op = rd::MaterialOp::MetallicRoughnessClosure;
        closure_node.type = rd::MaterialType::SurfaceClosure;
        closure_node.inputs[0] = graph.base_color_output;
        closure_node.inputs[1] = graph.normal_output;
        closure_node.inputs[2] = graph.roughness_output;
        closure_node.inputs[3] = metallic_output;
        closure_node.inputs[4] = graph.ao_output;
        closure_node.inputs[5] = opacity_output;
        graph.nodes.push_back(std::move(closure_node));
        graph.closure_output = graph.nodes.size() - 1;
        Require(graph.nodes.size() <= rd::kMaxMaterialGraphNodes,
                "legacy closure lowering exceeded the graph node bound");
    }
    if (kind == rd::MaterialGraphKind::Rock0) rd::ValidateRock0MaterialGraph(graph);
    else if (kind == rd::MaterialGraphKind::Leaf0) rd::ValidateLeaf0MaterialGraph(graph);
    else if (kind == rd::MaterialGraphKind::Terrain)
    {
        const auto aggregate = std::ranges::find_if(graph.nodes, [](const rd::MaterialNode &node) {
            return node.op == rd::MaterialOp::TerrainStochastic;
        });
        Require(aggregate != graph.nodes.end(), "terrain graph has no stochastic aggregate");
        graph.terrain_output = static_cast<std::size_t>(aggregate - graph.nodes.begin());
        rd::ValidateTerrainMaterialGraph(graph);
    }
    else if (decal) rd::ValidateDecalMaterialGraph(graph);
    else if (hero) rd::ValidateHeroMaterialGraph(graph);
    else rd::ValidateEnvironmentMaterialGraph(graph);
    return graph;
}

bool IsEnvironmentMaterialName(std::string_view name)
{
    for (const auto candidate : rd::kEnvironmentMaterialNames)
        if (name == candidate) return true;
    return false;
}

bool IsAuthoredMaterialName(std::string_view name)
{
    for (const auto candidate : rd::kAuthoredMaterialNames)
        if (name == candidate) return true;
    return IsHeroMaterialName(name);
}

bool IsHeroMaterialName(std::string_view name)
{
    return std::ranges::find(rd::kHeroMaterialNames, name) != rd::kHeroMaterialNames.end();
}

bool IsDecalMaterialName(std::string_view name)
{
    return std::ranges::find(rd::kDecalMaterialNames, name) != rd::kDecalMaterialNames.end();
}

rd::MaterialGraph ParseDecalMaterialGraph(std::string_view name, std::string_view json_text)
{
    Require(IsDecalMaterialName(name), "unknown decal material name");
    return ParseMaterialGraph(name, json_text, rd::MaterialGraphKind::Decal);
}

bool IsMaskedEnvironmentMaterial(std::string_view name)
{
    return name.starts_with("leaf_") || name.starts_with("grass_");
}

rd::MaterialGraph ParseEnvironmentMaterialGraph(std::string_view name,
                                                 std::string_view json_text)
{
    Require(IsAuthoredMaterialName(name), "unknown material name");
    const auto kind = name == "slime_body" ? rd::MaterialGraphKind::Translucent :
        IsHeroMaterialName(name) && name != "monster_pbr" ? rd::MaterialGraphKind::Masked :
        name == "terrain" ? rd::MaterialGraphKind::Terrain :
        IsMaskedEnvironmentMaterial(name) ? rd::MaterialGraphKind::Masked
        : rd::MaterialGraphKind::Opaque;
    return ParseMaterialGraph(name, json_text, kind);
}

rd::MaterialGraph ParseRock0MaterialGraph(std::string_view json_text)
{
    return ParseMaterialGraph("rock_0", json_text, rd::MaterialGraphKind::Rock0);
}

rd::MaterialGraph ParseLeaf0MaterialGraph(std::string_view json_text)
{
    return ParseMaterialGraph("leaf_0", json_text, rd::MaterialGraphKind::Leaf0);
}

rd::MaterialGraph ParseTerrainMaterialGraph(std::string_view json_text)
{
    return ParseMaterialGraph("terrain", json_text, rd::MaterialGraphKind::Terrain);
}

MaterialDefinition ParseMaterialDefinition(std::string_view name, std::string_view json_text)
{
    const auto root = Json::parse(json_text);
    MaterialDefinition result;
    result.graph = ParseEnvironmentMaterialGraph(name, json_text);
    if (IsHeroMaterialName(name)) return result;
    if (root.at("version") == 2) result.graph.textures = ParseTextures(root);
    for (std::size_t slot = 0; slot < result.graph.textures.size(); ++slot)
        result.material.textures[slot] = result.graph.textures[slot].product_id;
    const auto find_parameter = [&](std::string_view id) -> const rd::MaterialParameterDefinition & {
        const auto found = std::ranges::find(result.graph.parameters, id,
            &rd::MaterialParameterDefinition::id);
        Require(found != result.graph.parameters.end(), "required material parameter is missing");
        Require(found->type == rd::MaterialParameterType::Float,
                "required material parameter must be float");
        return *found;
    };
    result.material.metallic = std::bit_cast<float>(find_parameter("metallic").default_words[0]);
    result.material.roughness_scale = std::bit_cast<float>(find_parameter("roughness_scale").default_words[0]);
    const auto &flags = root.at("render_flags");
    Require(flags.at("two_sided").is_boolean(), "invalid two-sided flag type");
    result.material.two_sided = flags.at("two_sided").get<bool>();
    result.material.alpha_mask = flags.at("alpha_mode") == "mask";
    rd::ValidateRockMaterial(result.material);
    if (root.at("version") == 2) LowerLegacyMaterialSamples(result);
    return result;
}
} // namespace hs::material_content
