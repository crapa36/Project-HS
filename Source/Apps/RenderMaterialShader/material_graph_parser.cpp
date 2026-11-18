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

std::vector<rd::MaterialParameterDefinition> ParseParameterDeclarations(const Json &root, bool require_surface_defaults)
{
    std::vector<rd::MaterialParameterDefinition> result;
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
    else if (require_surface_defaults)
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
    return textures;
}

std::vector<rd::MaterialSamplerDefinition> ParseSamplers(const Json &root)
{
    std::vector<rd::MaterialSamplerDefinition> samplers;
    Require((root.at("samplers").is_array() && !root.at("samplers").empty() &&
             root.at("samplers").size() <= 4) ||
            (IsCharacterMaterialName(root.at("id").get<std::string>()) &&
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


} // namespace

static rd::MaterialGraph ParseMaterialGraphRoot(std::string_view name, const Json &root,
                                               rd::MaterialGraphKind kind)
{
    const bool hero = IsCharacterMaterialName(name);
    const bool decal = kind == rd::MaterialGraphKind::Decal;
    const bool transparent = kind == rd::MaterialGraphKind::Translucent || kind == rd::MaterialGraphKind::HeroRefractive;
    ExactKeys(root, {"id", "version", "domain", "shading_model", "parameters",
                     "textures", "samplers", "nodes", "outputs", "render_flags"});
    Require(root.at("version").is_number_integer() && root.at("version") == 7, "material version must be 7");
    Require(root.at("id").is_string() &&
            std::string_view(root.at("id").get_ref<const std::string &>()) == name &&
            root.at("domain") == (decal ? "decal" : transparent ? "translucent" : "surface") &&
            root.at("shading_model") == "metallic_roughness", "unsupported material");
    const bool has_opacity = kind == rd::MaterialGraphKind::Masked || transparent;
    const auto &flags = root.at("render_flags");
    rd::DecalReceiverPolicy receivers;
    if (decal)
    {
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
    auto textures = ParseTextures(root);
    auto samplers = ParseSamplers(root);
    Require(hero || !textures.empty(), "material requires texture declarations");
    auto parameters = ParseParameterDeclarations(root, !hero);
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
            Require(definition.at("declaration").is_string(), "reference requires a declaration ID");
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
            Require(item.node.type == rd::MaterialType::TerrainResult &&
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
            Require(definition.at("field").is_string(), "terrain field requires a field name");
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
            Require(item.node.type == rd::MaterialType::SurfaceClosure,
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
            item.inputs[0] = ParseEdge(definition.at("base"));
            item.inputs[1] = ParseEdge(definition.at("weight"));
            item.inputs[2] = ParseEdge(definition.at("roughness"));
            break;
        case rd::MaterialOp::AnisotropyClosure:
            ExactKeys(definition, {"id", "op", "type", "base", "amount", "tangent"});
            item.inputs[0] = ParseEdge(definition.at("base"));
            item.inputs[1] = ParseEdge(definition.at("amount"));
            item.inputs[2] = ParseEdge(definition.at("tangent"));
            break;
        case rd::MaterialOp::SheenClosure:
            ExactKeys(definition, {"id", "op", "type", "base", "weight", "tint", "roughness"});
            item.inputs[0] = ParseEdge(definition.at("base"));
            item.inputs[1] = ParseEdge(definition.at("weight"));
            item.inputs[2] = ParseEdge(definition.at("tint"));
            item.inputs[3] = ParseEdge(definition.at("roughness"));
            break;
        case rd::MaterialOp::ThinTransmissionClosure:
            ExactKeys(definition, {"id", "op", "type", "base", "weight", "tint"});
            item.inputs[0] = ParseEdge(definition.at("base"));
            item.inputs[1] = ParseEdge(definition.at("weight"));
            item.inputs[2] = ParseEdge(definition.at("tint"));
            break;
        case rd::MaterialOp::EmissionClosure:
            ExactKeys(definition, {"id", "op", "type", "base", "emission"});
            item.inputs[0] = ParseEdge(definition.at("base"));
            item.inputs[1] = ParseEdge(definition.at("emission"));
            break;
        case rd::MaterialOp::UnlitClosure:
            ExactKeys(definition, {"id", "op", "type", "color", "opacity"});
            item.inputs[0] = ParseEdge(definition.at("color"));
            item.inputs[1] = ParseEdge(definition.at("opacity"));
            break;
        case rd::MaterialOp::SubsurfaceClosure:
            ExactKeys(definition, {"id", "op", "type", "base", "weight", "radius_m", "thickness_m"});
            item.inputs[0] = ParseEdge(definition.at("base"));
            item.inputs[1] = ParseEdge(definition.at("weight"));
            item.inputs[2] = ParseEdge(definition.at("radius_m"));
            item.inputs[3] = ParseEdge(definition.at("thickness_m"));
            break;
        case rd::MaterialOp::SolidTransmissionClosure:
            ExactKeys(definition, {"id", "op", "type", "base", "weight", "ior", "absorption_per_m", "thickness_m"});
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
    ExactKeys(outputs, {"surface", "closure", "normal_space"});
    Require(outputs.at("surface") == "typed_closure", "unsupported surface output");
    const auto closure = ParseEdge(outputs.at("closure"));
    Require(closure.type == rd::MaterialType::SurfaceClosure && ids.contains(closure.id),
            "invalid typed closure output edge");
    Require(outputs.at("normal_space") == "tangent" || outputs.at("normal_space") == "world",
            "invalid normal output space");

    rd::MaterialGraph graph;
    graph.kind = kind;
    graph.decal_receivers = receivers;
    graph.parameters = std::move(parameters);
    graph.textures = std::move(textures);
    graph.samplers = std::move(samplers);
    graph.normal_space = outputs.at("normal_space") == "world"
        ? rd::MaterialNormalSpace::World : rd::MaterialNormalSpace::Tangent;
    graph.nodes.reserve(pending.size());
    std::vector<unsigned char> state(pending.size());
    std::vector<std::size_t> ordered_index(pending.size());
    std::function<void(std::size_t)> visit = [&](std::size_t index) {
        Require(state[index] != 1, "cycle detected");
        if (state[index] == 2) return;
        state[index] = 1;
        auto &node = pending[index].node;
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
    visit(ids.at(closure.id));
    // Validate dead nodes too, including their cycles and type constraints;
    // the emitter prunes them separately for every output.
    for (std::size_t index = 0; index < pending.size(); ++index) visit(index);
    graph.closure_output = ordered_index[ids.at(closure.id)];
    if (kind == rd::MaterialGraphKind::Terrain)
    {
        const auto aggregate = std::ranges::find_if(graph.nodes, [](const rd::MaterialNode &node) {
            return node.op == rd::MaterialOp::TerrainStochastic;
        });
        Require(aggregate != graph.nodes.end(), "terrain graph has no stochastic aggregate");
        graph.terrain_output = static_cast<std::size_t>(aggregate - graph.nodes.begin());
    }
    return graph;
}

bool IsEnvironmentMaterialName(std::string_view name)
{
    return std::ranges::find(rd::kEnvironmentMaterialNames, name) != rd::kEnvironmentMaterialNames.end();
}

bool IsAuthoredMaterialName(std::string_view name)
{
    for (const auto candidate : rd::kAuthoredMaterialNames)
        if (name == candidate) return true;
    return IsCharacterMaterialName(name);
}

bool IsCharacterMaterialName(std::string_view name)
{
    return std::ranges::find(rd::kCharacterMaterialNames, name) != rd::kCharacterMaterialNames.end();
}

bool IsDecalMaterialName(std::string_view name)
{
    return std::ranges::find(rd::kDecalMaterialNames, name) != rd::kDecalMaterialNames.end();
}

rd::PreparedMaterial ParsePreparedDecalMaterial(std::string_view name, std::string_view json_text)
{
    Require(IsDecalMaterialName(name), "unknown decal material name");
    return rd::PrepareMaterial(ParseMaterialGraphRoot(name, Json::parse(json_text),
        rd::MaterialGraphKind::Decal), rd::MaterialPreparationDomain::Decal);
}

bool IsMaskedEnvironmentMaterial(std::string_view name)
{
    return name.starts_with("leaf_") || name.starts_with("grass_");
}

static rd::MaterialGraphKind AuthoredMaterialKind(std::string_view name, const Json &root)
{
    Require(IsAuthoredMaterialName(name), "unknown material name");
    if (name == "terrain") return rd::MaterialGraphKind::Terrain;
    if (IsMaskedEnvironmentMaterial(name)) return rd::MaterialGraphKind::Masked;
    if (!IsCharacterMaterialName(name)) return rd::MaterialGraphKind::Opaque;

    const auto domain = root.at("domain").get<std::string>();
    const auto alpha_mode = root.at("render_flags").at("alpha_mode").get<std::string>();
    if (domain == "translucent")
    {
        Require(alpha_mode == "blend", "translucent Hero domain requires blend render flags");
        return rd::MaterialGraphKind::Translucent;
    }
    Require(domain == "surface", "Hero material domain must be surface or translucent");
    if (alpha_mode == "mask") return rd::MaterialGraphKind::Masked;
    Require(alpha_mode == "opaque", "surface Hero domain requires opaque or mask render flags");
    return rd::MaterialGraphKind::Opaque;
}

MaterialDefinition ParseMaterialDefinition(std::string_view name, std::string_view json_text)
{
    const auto root = Json::parse(json_text);
    const auto kind = AuthoredMaterialKind(name, root);
    const bool hero = IsCharacterMaterialName(name);
    auto prepared = rd::PrepareMaterial(ParseMaterialGraphRoot(name, root, kind), hero
        ? rd::MaterialPreparationDomain::Hero : rd::MaterialPreparationDomain::Environment);
    if (hero) return {{}, std::move(prepared)};
    const auto &graph = prepared.Graph();
    rd::RockMaterial material;
    for (std::size_t slot = 0; slot < graph.textures.size(); ++slot)
        material.textures[slot] = graph.textures[slot].product_id;
    const auto metallic = std::ranges::find(graph.parameters, "metallic",
        &rd::MaterialParameterDefinition::id);
    const auto roughness = std::ranges::find(graph.parameters, "roughness_scale",
        &rd::MaterialParameterDefinition::id);
    Require(metallic != graph.parameters.end() && roughness != graph.parameters.end(),
            "required material parameter is missing");
    Require(metallic->type == rd::MaterialParameterType::Float &&
            roughness->type == rd::MaterialParameterType::Float,
            "required material parameter must be float");
    material.metallic = std::bit_cast<float>(metallic->default_words[0]);
    material.roughness_scale = std::bit_cast<float>(roughness->default_words[0]);
    const auto &flags = root.at("render_flags");
    Require(flags.at("two_sided").is_boolean(), "invalid two-sided flag type");
    material.two_sided = flags.at("two_sided").get<bool>();
    material.alpha_mask = flags.at("alpha_mode") == "mask";
    rd::ValidateRockMaterial(material);
    return {material, std::move(prepared)};
}
} // namespace hs::material_content
