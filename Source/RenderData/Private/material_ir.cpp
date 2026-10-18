#include <hs/render_data/material_ir.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace hs::render_data
{
namespace
{
void Require(bool condition, std::string_view reason)
{
    if (!condition)
        throw std::invalid_argument("material graph: " + std::string(reason));
}

bool ValidId(std::string_view id)
{
    if (id.empty() || id.size() > 32 ||
        !((id.front() >= 'a' && id.front() <= 'z') || id.front() == '_'))
        return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}

bool Floating(MaterialType type)
{
    return type == MaterialType::Float || type == MaterialType::Float2 ||
           type == MaterialType::Float3 || type == MaterialType::Float4;
}

bool Numeric(MaterialType type) { return Floating(type) || type == MaterialType::Uint; }
bool Scalar(MaterialType type) { return MaterialComponentCount(type) == 1; }

std::string TypeName(MaterialType type)
{
    switch (type)
    {
    case MaterialType::Bool: return "bool";
    case MaterialType::Uint: return "uint";
    case MaterialType::Float: return "float";
    case MaterialType::Float2: return "float2";
    case MaterialType::Float3: return "float3";
    case MaterialType::Float4: return "float4";
    case MaterialType::TextureRef: return "texture_ref";
    case MaterialType::SamplerRef: return "sampler_ref";
    case MaterialType::TerrainResult: return "HsMaterialTerrainResult";
    case MaterialType::SurfaceClosure: return "HsSurfaceClosure";
    default: Require(false, "unsupported node type"); return {};
    }
}

bool ValidOp(MaterialOp op)
{
    switch (op)
    {
    case MaterialOp::SampledBaseColor: case MaterialOp::SampledRoughness:
    case MaterialOp::SampledAlpha: case MaterialOp::RoughnessScale: case MaterialOp::Parameter:
    case MaterialOp::Constant: case MaterialOp::Multiply: case MaterialOp::Saturate:
    case MaterialOp::Add: case MaterialOp::Subtract: case MaterialOp::Divide:
    case MaterialOp::Min: case MaterialOp::Max: case MaterialOp::Negate:
    case MaterialOp::Lerp: case MaterialOp::Clamp: case MaterialOp::Construct:
    case MaterialOp::Extract: case MaterialOp::Convert: case MaterialOp::Less:
    case MaterialOp::LessEqual: case MaterialOp::Equal: case MaterialOp::Select:
    case MaterialOp::LogicalAnd: case MaterialOp::LogicalOr: case MaterialOp::LogicalNot:
    case MaterialOp::TextureRef: case MaterialOp::SamplerRef: case MaterialOp::Context:
    case MaterialOp::SampleGrad: case MaterialOp::SampleLevel: case MaterialOp::DecodeNormalXY:
    case MaterialOp::NormalRnm: case MaterialOp::DetailFade: case MaterialOp::TangentToWorld:
    case MaterialOp::SeededNoise2D: case MaterialOp::FresnelSchlick:
    case MaterialOp::TerrainStochastic: case MaterialOp::TerrainField:
    case MaterialOp::MetallicRoughnessClosure:
    case MaterialOp::CoatClosure: case MaterialOp::AnisotropyClosure:
    case MaterialOp::SheenClosure: case MaterialOp::ThinTransmissionClosure:
    case MaterialOp::EmissionClosure: case MaterialOp::UnlitClosure:
    case MaterialOp::SubsurfaceClosure: case MaterialOp::SolidTransmissionClosure:
    case MaterialOp::DecalFracture: case MaterialOp::DecalOrganic:
        return true;
    default: return false;
    }
}

MaterialType ContextType(MaterialContextInput input)
{
    switch (input)
    {
    case MaterialContextInput::Uv: case MaterialContextInput::DUvDx: case MaterialContextInput::DUvDy:
        return MaterialType::Float2;
    case MaterialContextInput::Layer: return MaterialType::Uint;
    case MaterialContextInput::ActualWorldPosition: case MaterialContextInput::DActualWorldDx:
    case MaterialContextInput::DActualWorldDy: case MaterialContextInput::StableWorldPosition:
    case MaterialContextInput::DStableWorldDx: case MaterialContextInput::DStableWorldDy:
    case MaterialContextInput::Normal: case MaterialContextInput::ShadingNormal:
    case MaterialContextInput::SampledBaseColor: case MaterialContextInput::SampledEmission:
        return MaterialType::Float3;
    case MaterialContextInput::Tangent: return MaterialType::Float4;
    case MaterialContextInput::EyeDistance: case MaterialContextInput::SampledRoughness:
    case MaterialContextInput::SampledAlpha: case MaterialContextInput::RoughnessScale:
    case MaterialContextInput::SampledAo: case MaterialContextInput::SampledMetallic: return MaterialType::Float;
    case MaterialContextInput::Mesh: case MaterialContextInput::Seed: return MaterialType::Uint;
    case MaterialContextInput::ViewDirection: return MaterialType::Float3;
    case MaterialContextInput::Metallic: return MaterialType::Float;
    case MaterialContextInput::DecalTintHdr: case MaterialContextInput::DecalProjector:
        return MaterialType::Float4;
    case MaterialContextInput::DecalShape: return MaterialType::Float3;
    default: Require(false, "invalid context input"); return {};
    }
}

struct Value
{
    MaterialType type{};
    std::array<float, 4> floats{};
    std::uint32_t integer{};
    bool boolean{};
};

Value Constant(const MaterialNode &node)
{
    return {node.type, node.value, node.uint_value, node.bool_value};
}

// Do not replace operations involving subnormals: shader denorm modes and
// min/max signed-zero choices can differ from the host. Keep nonfinite results
// as HLSL operations instead of writing invalid numeric literals.
bool FoldableFloat(float value)
{
    return std::isfinite(value) && std::fpclassify(value) != FP_SUBNORMAL;
}

std::uint32_t SeededNoiseHash(std::uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float SeededNoiseRandom(std::uint32_t value)
{
    return static_cast<float>(SeededNoiseHash(value) & 0xffffffu) / 16777216.0f;
}

float SeededNoiseValue(const std::array<float, 4> &position, std::uint32_t seed)
{
    const float ix = std::floor(position[0]);
    const float iy = std::floor(position[1]);
    const float fx = position[0] - ix;
    const float fy = position[1] - iy;
    const float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
    const float uy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);
    const auto cell = [&](std::int32_t x, std::int32_t y) {
        const auto ux = static_cast<std::uint32_t>(x);
        const auto uy = static_cast<std::uint32_t>(y);
        return SeededNoiseHash(ux ^ SeededNoiseHash(uy) ^ seed);
    };
    const float x0y0 = SeededNoiseRandom(cell(static_cast<std::int32_t>(ix),
                                               static_cast<std::int32_t>(iy)));
    const float x1y0 = SeededNoiseRandom(cell(static_cast<std::int32_t>(ix + 1.0f),
                                               static_cast<std::int32_t>(iy)));
    const float x0y1 = SeededNoiseRandom(cell(static_cast<std::int32_t>(ix),
                                               static_cast<std::int32_t>(iy + 1.0f)));
    const float x1y1 = SeededNoiseRandom(cell(static_cast<std::int32_t>(ix + 1.0f),
                                               static_cast<std::int32_t>(iy + 1.0f)));
    const float x0 = x0y0 + (x1y0 - x0y0) * ux;
    const float x1 = x0y1 + (x1y1 - x0y1) * ux;
    return (x0 + (x1 - x0) * uy) * 2.0f - 1.0f;
}

std::optional<Value> Fold(const MaterialNode &node,
                        const std::vector<std::optional<Value>> &values)
{
    if (node.op == MaterialOp::Constant) return Constant(node);
    if (node.type == MaterialType::SurfaceClosure) return std::nullopt;
    const auto count = MaterialInputCount(node.op, node.type);
    if (count == 0) return std::nullopt;
    if (node.op == MaterialOp::Select && values[node.inputs[0]])
        return values[node.inputs[values[node.inputs[0]]->boolean ? 1 : 2]];
    for (std::size_t slot = 0; slot < count; ++slot)
        if (!values[node.inputs[slot]]) return std::nullopt;
    const auto &a = *values[node.inputs[0]];
    const auto &b = count > 1 ? *values[node.inputs[1]] : a;
    const auto &c = count > 2 ? *values[node.inputs[2]] : a;
    Value result;
    result.type = node.type;
    if (node.op == MaterialOp::SeededNoise2D)
    {
        if (!FoldableFloat(a.floats[0]) || !FoldableFloat(a.floats[1])) return std::nullopt;
        const auto cell_coordinate_representable = [](float coordinate) {
            const double cell = std::floor(static_cast<double>(coordinate));
            return cell >= static_cast<double>(std::numeric_limits<std::int32_t>::min()) &&
                   cell <= static_cast<double>(std::numeric_limits<std::int32_t>::max()) - 1.0;
        };
        if (!cell_coordinate_representable(a.floats[0]) ||
            !cell_coordinate_representable(a.floats[1])) return std::nullopt;
        const auto value = SeededNoiseValue(a.floats, b.integer);
        if (!FoldableFloat(value)) return std::nullopt;
        result.floats[0] = value;
        return result;
    }
    if (node.op == MaterialOp::FresnelSchlick)
    {
        if (!FoldableFloat(b.floats[0])) return std::nullopt;
        const float factor = std::pow(1.0f - std::min(std::max(b.floats[0], 0.0f), 1.0f), 5.0f);
        if (!FoldableFloat(factor)) return std::nullopt;
        for (std::size_t i = 0; i < 3; ++i)
        {
            if (!FoldableFloat(a.floats[i])) return std::nullopt;
            result.floats[i] = a.floats[i] + (1.0f - a.floats[i]) * factor;
            if (!FoldableFloat(result.floats[i])) return std::nullopt;
        }
        return result;
    }
    if (node.op == MaterialOp::Construct)
    {
        for (std::size_t i = 0; i < count; ++i)
            result.floats[i] = values[node.inputs[i]]->floats[0];
        return result;
    }
    if (node.op == MaterialOp::Extract)
    {
        result.floats[0] = a.floats[node.component];
        return result;
    }
    if (node.op == MaterialOp::Convert)
    {
        if (a.type == node.type) return a;
        if (node.type == MaterialType::Bool)
        {
            if (a.type == MaterialType::Float && !FoldableFloat(a.floats[0]))
                return std::nullopt;
            result.boolean = a.type == MaterialType::Uint ? a.integer != 0 : a.floats[0] != 0;
        }
        else if (node.type == MaterialType::Uint)
            result.integer = a.type == MaterialType::Bool ? std::uint32_t(a.boolean)
                                                        : static_cast<std::uint32_t>(a.floats[0]);
        else
            result.floats[0] = a.type == MaterialType::Bool ? (a.boolean ? 1.0f : 0.0f)
                                                          : static_cast<float>(a.integer);
        return result;
    }
    if (node.op == MaterialOp::LogicalNot) result.boolean = !a.boolean;
    else if (node.op == MaterialOp::LogicalAnd) result.boolean = a.boolean && b.boolean;
    else if (node.op == MaterialOp::LogicalOr) result.boolean = a.boolean || b.boolean;
    else if (node.op == MaterialOp::Select) return a.boolean ? b : c;
    else if (node.op == MaterialOp::Less || node.op == MaterialOp::LessEqual ||
             node.op == MaterialOp::Equal)
    {
        if (a.type == MaterialType::Bool) result.boolean = a.boolean == b.boolean;
        else if (a.type == MaterialType::Uint)
            result.boolean = node.op == MaterialOp::Less ? a.integer < b.integer :
                node.op == MaterialOp::LessEqual ? a.integer <= b.integer : a.integer == b.integer;
        else
        {
            if (!FoldableFloat(a.floats[0]) || !FoldableFloat(b.floats[0])) return std::nullopt;
            result.boolean = node.op == MaterialOp::Less ? a.floats[0] < b.floats[0] :
                node.op == MaterialOp::LessEqual ? a.floats[0] <= b.floats[0] :
                                                  a.floats[0] == b.floats[0];
        }
    }
    else if (node.type == MaterialType::Uint)
    {
        switch (node.op)
        {
        case MaterialOp::Add: result.integer = a.integer + b.integer; break;
        case MaterialOp::Subtract: result.integer = a.integer - b.integer; break;
        case MaterialOp::Multiply:
            result.integer = static_cast<std::uint32_t>(std::uint64_t(a.integer) * b.integer); break;
        case MaterialOp::Divide: result.integer = a.integer / b.integer; break;
        case MaterialOp::Min: result.integer = std::min(a.integer, b.integer); break;
        case MaterialOp::Max: result.integer = std::max(a.integer, b.integer); break;
        case MaterialOp::Clamp: result.integer = std::min(std::max(a.integer, b.integer), c.integer); break;
        default: return std::nullopt;
        }
    }
    else
    {
        for (std::size_t i = 0; i < MaterialComponentCount(node.type); ++i)
        {
            const float x = a.floats[i], y = b.floats[i], z = c.floats[i];
            if (!FoldableFloat(x) || (count > 1 && !FoldableFloat(y)) ||
                (count > 2 && !FoldableFloat(z))) return std::nullopt;
            float value{};
            switch (node.op)
            {
            case MaterialOp::Add: value = x + y; break;
            case MaterialOp::Subtract: value = x - y; break;
            case MaterialOp::Multiply: value = x * y; break;
            case MaterialOp::Divide: value = x / y; break;
            case MaterialOp::Min: value = std::fmin(x, y); break;
            case MaterialOp::Max: value = std::fmax(x, y); break;
            case MaterialOp::Negate: value = -x; break;
            case MaterialOp::Saturate: value = std::min(std::max(x, 0.0f), 1.0f); break;
            case MaterialOp::Clamp: value = std::fmin(std::fmax(x, y), z); break;
            case MaterialOp::Lerp:
            {
                const float difference = y - x;
                const float product = difference * z;
                if (!FoldableFloat(difference) || !FoldableFloat(product)) return std::nullopt;
                value = x + product;
                break;
            }
            default: return std::nullopt;
            }
            if (!FoldableFloat(value)) return std::nullopt;
            result.floats[i] = value;
        }
    }
    return result;
}

void ValidateNode(const MaterialGraph &graph, std::size_t index,
                  const std::vector<std::optional<Value>> &values)
{
    const auto &node = graph.nodes[index];
    Require(MaterialComponentCount(node.type) != 0 || node.type == MaterialType::TextureRef ||
            node.type == MaterialType::SamplerRef || node.type == MaterialType::TerrainResult ||
            node.type == MaterialType::SurfaceClosure,
            "unsupported node type");
    Require(ValidOp(node.op), "invalid operation");
    Require(graph.kind != MaterialGraphKind::Decal ||
            (node.op != MaterialOp::SampledAlpha && node.op != MaterialOp::RoughnessScale),
            "legacy sampled alpha/roughness scale is unavailable in the decal adapter");
    const auto count = MaterialInputCount(node.op, node.type);
    for (std::size_t slot = 0; slot < count; ++slot)
        Require(node.inputs[slot] < index, "input is missing, cyclic or not in topological order");
    for (std::size_t slot = count; slot < node.inputs.size(); ++slot)
        Require(node.inputs[slot] == kNoMaterialInput, "unexpected input");
    const auto type = [&](std::size_t slot) { return graph.nodes[node.inputs[slot]].type; };
    const auto same = [&] {
        for (std::size_t slot = 0; slot < count; ++slot)
            Require(type(slot) == node.type, "operand type mismatch; explicit conversion required");
    };
    Require(node.op == MaterialOp::TextureRef || node.op == MaterialOp::SamplerRef ||
            node.op == MaterialOp::Parameter ||
            node.declaration_index == kNoMaterialInput, "unexpected declaration index");
    switch (node.op)
    {
    case MaterialOp::TextureRef:
        Require(node.type == MaterialType::TextureRef && node.declaration_index < graph.textures.size(),
                "invalid texture declaration reference"); break;
    case MaterialOp::SamplerRef:
        Require(node.type == MaterialType::SamplerRef && node.declaration_index < graph.samplers.size(),
                 "invalid sampler declaration reference"); break;
    case MaterialOp::Parameter:
    {
        Require(node.declaration_index < graph.parameters.size(),
                "invalid material parameter declaration reference");
        const auto declaration = graph.parameters[node.declaration_index].type;
        const auto expected = declaration == MaterialParameterType::Bool ? MaterialType::Bool :
            declaration == MaterialParameterType::Uint ? MaterialType::Uint :
            declaration == MaterialParameterType::Float ? MaterialType::Float :
            declaration == MaterialParameterType::Float2 ? MaterialType::Float2 :
            declaration == MaterialParameterType::Float3 ? MaterialType::Float3 : MaterialType::Float4;
        Require(node.type == expected, "material parameter node type disagrees with declaration");
        break;
    }
    case MaterialOp::Context:
        Require(node.type == ContextType(node.context_input), "context input type mismatch");
        Require(graph.kind == MaterialGraphKind::Decal ||
                node.context_input < MaterialContextInput::DecalTintHdr,
                "decal context is only valid in the decal domain");
        if (graph.kind == MaterialGraphKind::Decal)
            Require(node.context_input == MaterialContextInput::Uv ||
                    node.context_input == MaterialContextInput::DUvDx || node.context_input == MaterialContextInput::DUvDy ||
                    node.context_input == MaterialContextInput::Layer || node.context_input == MaterialContextInput::Seed ||
                    node.context_input == MaterialContextInput::SampledBaseColor ||
                    node.context_input == MaterialContextInput::SampledRoughness || node.context_input == MaterialContextInput::Normal ||
                    node.context_input >= MaterialContextInput::DecalTintHdr,
                    "context input is unavailable in the projected decal adapter");
        break;
    case MaterialOp::DecalFracture: case MaterialOp::DecalOrganic:
    {
        Require(graph.kind == MaterialGraphKind::Decal && node.type == MaterialType::SurfaceClosure,
                "projected decal evaluation requires the decal domain");
        constexpr std::array expected{MaterialType::TextureRef, MaterialType::SamplerRef,
            MaterialType::Float2, MaterialType::Uint, MaterialType::Float2, MaterialType::Float2,
            MaterialType::Float3, MaterialType::Float3, MaterialType::Float, MaterialType::Float4,
            MaterialType::Float4, MaterialType::Float3, MaterialType::Uint, MaterialType::Float,
            MaterialType::Float};
        for (std::size_t slot = 0; slot < expected.size(); ++slot)
            Require(type(slot) == expected[slot], "decal operand type mismatch");
        Require(graph.nodes[node.inputs[0]].op == MaterialOp::TextureRef &&
                graph.nodes[node.inputs[1]].op == MaterialOp::SamplerRef &&
                graph.samplers[graph.nodes[node.inputs[1]].declaration_index].profile == MaterialSamplerProfile::LinearClamp,
                "decal sampling requires a declared texture and LinearClamp sampler");
        const auto semantic = graph.textures[graph.nodes[node.inputs[0]].declaration_index].semantic;
        Require(semantic == (node.op == MaterialOp::DecalFracture ? MaterialTextureSemantic::DecalStructure :
            MaterialTextureSemantic::DecalMask), "decal function disagrees with its declared texture channel semantic");
        break;
    }
    case MaterialOp::SampleGrad: case MaterialOp::SampleLevel:
        Require(node.type == MaterialType::Float4 && type(0) == MaterialType::TextureRef &&
                type(1) == MaterialType::SamplerRef && type(2) == MaterialType::Float2 &&
                type(3) == MaterialType::Uint &&
                (node.op == MaterialOp::SampleGrad
                    ? type(4) == MaterialType::Float2 && type(5) == MaterialType::Float2
                    : type(4) == MaterialType::Float), "texture sample operand type mismatch");
        // References may only originate from declarations, never arithmetic or casts.
        Require(graph.nodes[node.inputs[0]].op == MaterialOp::TextureRef &&
                graph.nodes[node.inputs[1]].op == MaterialOp::SamplerRef,
                "sample requires declaration references"); break;
    case MaterialOp::DecodeNormalXY:
        Require(node.type == MaterialType::Float3 && type(0) == MaterialType::Float2,
                "normal decode requires float2"); break;
    case MaterialOp::NormalRnm:
        Require(node.type == MaterialType::Float3 && type(0) == MaterialType::Float3 &&
                type(1) == MaterialType::Float3, "RNM requires float3 normals"); break;
    case MaterialOp::DetailFade:
        Require(node.type == MaterialType::Float3 && type(0) == MaterialType::Float3 &&
                type(1) == MaterialType::Float, "detail fade operand mismatch"); break;
    case MaterialOp::TangentToWorld:
        Require(node.type == MaterialType::Float3 && type(0) == MaterialType::Float3 &&
                    type(1) == MaterialType::Float3 && type(2) == MaterialType::Float4,
                "tangent transform operand mismatch"); break;
    case MaterialOp::SeededNoise2D:
        Require(node.type == MaterialType::Float && type(0) == MaterialType::Float2 &&
                    type(1) == MaterialType::Uint,
                "seeded noise requires float2 position and uint seed"); break;
    case MaterialOp::FresnelSchlick:
        Require(node.type == MaterialType::Float3 && type(0) == MaterialType::Float3 &&
                    type(1) == MaterialType::Float,
                "Schlick Fresnel requires float3 F0 and float cosTheta"); break;
    case MaterialOp::TerrainStochastic:
    {
        Require(graph.kind == MaterialGraphKind::Terrain && node.type == MaterialType::TerrainResult,
                "terrain stochastic is only valid in terrain graphs");
        constexpr std::array expected_semantics{
            MaterialTextureSemantic::Gaussian, MaterialTextureSemantic::Surface,
            MaterialTextureSemantic::Normal, MaterialTextureSemantic::Detail,
            MaterialTextureSemantic::Height, MaterialTextureSemantic::Histogram};
        Require(graph.textures.size() == expected_semantics.size() && graph.samplers.size() == 2,
                "terrain resources must declare six textures and two samplers");
        for (std::size_t slot = 0; slot < 6; ++slot)
        {
            Require(type(slot) == MaterialType::TextureRef &&
                    graph.nodes[node.inputs[slot]].op == MaterialOp::TextureRef &&
                    graph.nodes[node.inputs[slot]].declaration_index == slot &&
                    graph.textures[slot].semantic == expected_semantics[slot],
                    "terrain texture resource signature or semantic mismatch");
        }
        Require(type(6) == MaterialType::SamplerRef && type(7) == MaterialType::SamplerRef &&
                graph.nodes[node.inputs[6]].op == MaterialOp::SamplerRef &&
                graph.nodes[node.inputs[7]].op == MaterialOp::SamplerRef &&
                graph.nodes[node.inputs[6]].declaration_index == 0 &&
                graph.nodes[node.inputs[7]].declaration_index == 1 &&
                graph.samplers[0].profile == MaterialSamplerProfile::EnvironmentAnisotropicWrap &&
                graph.samplers[1].profile == MaterialSamplerProfile::LinearClamp,
                "terrain sampler resource signature or profile mismatch");
        Require(type(8) == MaterialType::Float3 && type(9) == MaterialType::Float3 &&
                type(10) == MaterialType::Float3 && type(11) == MaterialType::Uint &&
                type(12) == MaterialType::Uint && type(13) == MaterialType::Float3 &&
                type(14) == MaterialType::Float,
                "terrain stochastic context signature mismatch");
        constexpr std::array expected_context{
            MaterialContextInput::ActualWorldPosition,
            MaterialContextInput::DActualWorldDx,
            MaterialContextInput::DActualWorldDy,
            MaterialContextInput::Mesh,
            MaterialContextInput::Seed,
            MaterialContextInput::ViewDirection,
            MaterialContextInput::EyeDistance};
        for (std::size_t slot = 0; slot < expected_context.size(); ++slot)
        {
            const auto &context = graph.nodes[node.inputs[8 + slot]];
            Require(context.op == MaterialOp::Context &&
                        context.context_input == expected_context[slot],
                    "terrain stochastic requires its exact direct context signature");
        }
        break;
    }
    case MaterialOp::TerrainField:
        Require(graph.kind == MaterialGraphKind::Terrain && type(0) == MaterialType::TerrainResult,
                "terrain field requires a terrain aggregate");
        Require((node.terrain_field == TerrainField::BaseColor && node.type == MaterialType::Float3) ||
                (node.terrain_field == TerrainField::Normal && node.type == MaterialType::Float3) ||
                (node.terrain_field == TerrainField::Detail && node.type == MaterialType::Float3) ||
                (node.terrain_field == TerrainField::Roughness && node.type == MaterialType::Float) ||
                (node.terrain_field == TerrainField::Ao && node.type == MaterialType::Float),
                "terrain field type mismatch");
        break;
    case MaterialOp::SampledBaseColor:
        Require(node.type == MaterialType::Float3, "sampled color type mismatch"); break;
    case MaterialOp::SampledRoughness: case MaterialOp::SampledAlpha: case MaterialOp::RoughnessScale:
        Require(node.type == MaterialType::Float, "scalar input type mismatch"); break;
    case MaterialOp::Constant:
        Require(node.type != MaterialType::TextureRef && node.type != MaterialType::SamplerRef &&
                node.type != MaterialType::SurfaceClosure,
                "references cannot be constants");
        if (Floating(node.type))
            for (std::size_t i = 0; i < MaterialComponentCount(node.type); ++i)
                Require(std::isfinite(node.value[i]) && node.value[i] >= -16.0f &&
                            node.value[i] <= 16.0f, "constant out of bounds");
        break;
    case MaterialOp::Add: case MaterialOp::Subtract: case MaterialOp::Multiply:
    case MaterialOp::Min: case MaterialOp::Max: case MaterialOp::Clamp:
        Require(Numeric(node.type), "arithmetic requires numeric values"); same(); break;
    case MaterialOp::Divide:
    {
        Require(Numeric(node.type), "division requires numeric values"); same();
        const auto &divisor = values[node.inputs[1]];
        if (node.type == MaterialType::Uint)
            Require(divisor && divisor->integer != 0, "uint divisor must be constant and nonzero");
        else if (divisor)
            for (std::size_t i = 0; i < MaterialComponentCount(node.type); ++i)
                Require(divisor->floats[i] != 0.0f &&
                            std::fpclassify(divisor->floats[i]) != FP_SUBNORMAL,
                        "constant divisor is zero (including shader-flushed denorms)");
        break;
    }
    case MaterialOp::MetallicRoughnessClosure:
        Require((graph.kind == MaterialGraphKind::Rock0 || graph.kind == MaterialGraphKind::Opaque ||
                 graph.kind == MaterialGraphKind::Masked || graph.kind == MaterialGraphKind::Terrain ||
                 graph.kind == MaterialGraphKind::Translucent || graph.kind == MaterialGraphKind::HeroRefractive) &&
                node.type == MaterialType::SurfaceClosure &&
                type(0) == MaterialType::Float3 && type(1) == MaterialType::Float3 &&
                type(2) == MaterialType::Float && type(3) == MaterialType::Float &&
                type(4) == MaterialType::Float && type(5) == MaterialType::Float,
                "metallic roughness closure signature mismatch");
        break;
    case MaterialOp::CoatClosure: case MaterialOp::AnisotropyClosure:
    case MaterialOp::SheenClosure: case MaterialOp::ThinTransmissionClosure:
    case MaterialOp::EmissionClosure: case MaterialOp::UnlitClosure:
    case MaterialOp::SubsurfaceClosure: case MaterialOp::SolidTransmissionClosure:
    {
        Require(graph.kind != MaterialGraphKind::Leaf0 &&
                node.type == MaterialType::SurfaceClosure,
                "layer closure requires a surface graph and surface closure result");
        if (node.op == MaterialOp::UnlitClosure)
            Require(type(0) == MaterialType::Float3 && type(1) == MaterialType::Float,
                    "unlit closure signature mismatch");
        else
        {
            Require(type(0) == MaterialType::SurfaceClosure,
                    "layer closure base must be a surface closure");
            switch (node.op)
            {
            case MaterialOp::CoatClosure:
                Require(type(1) == MaterialType::Float && type(2) == MaterialType::Float,
                        "coat closure signature mismatch"); break;
            case MaterialOp::AnisotropyClosure:
                Require(type(1) == MaterialType::Float && type(2) == MaterialType::Float3,
                        "anisotropy closure signature mismatch"); break;
            case MaterialOp::SheenClosure:
                Require(type(1) == MaterialType::Float && type(2) == MaterialType::Float3 &&
                        type(3) == MaterialType::Float,
                        "sheen closure signature mismatch"); break;
            case MaterialOp::ThinTransmissionClosure:
                Require(type(1) == MaterialType::Float && type(2) == MaterialType::Float3,
                        "thin transmission closure signature mismatch"); break;
            case MaterialOp::EmissionClosure:
                Require(type(1) == MaterialType::Float3,
                        "emission closure signature mismatch"); break;
            case MaterialOp::SubsurfaceClosure:
                Require((graph.kind == MaterialGraphKind::Opaque || graph.kind == MaterialGraphKind::Masked) &&
                        type(1) == MaterialType::Float && type(2) == MaterialType::Float3 &&
                        type(3) == MaterialType::Float,
                        "subsurface node " + node.id + " requires an opaque/masked surface and weight/radius/thickness");
                break;
            case MaterialOp::SolidTransmissionClosure:
                Require((graph.kind == MaterialGraphKind::Translucent || graph.kind == MaterialGraphKind::HeroRefractive) &&
                        type(1) == MaterialType::Float && type(2) == MaterialType::Float &&
                        type(3) == MaterialType::Float3 && type(4) == MaterialType::Float,
                        "solid transmission node " + node.id + " requires a transparent surface and weight/ior/absorption/thickness");
                break;
            default: break;
            }
        }
        break;
    }
    case MaterialOp::Saturate: case MaterialOp::Negate: case MaterialOp::Lerp:
        Require(Floating(node.type), "operation requires floating values"); same(); break;
    case MaterialOp::Construct:
        Require(Floating(node.type) && node.type != MaterialType::Float, "construct requires a float vector");
        for (std::size_t slot = 0; slot < count; ++slot)
            Require(type(slot) == MaterialType::Float, "construct components must be float");
        break;
    case MaterialOp::Extract:
        Require(node.type == MaterialType::Float && Floating(type(0)) &&
                    type(0) != MaterialType::Float && node.component < MaterialComponentCount(type(0)),
                "extract requires a valid float vector component"); break;
    case MaterialOp::Convert:
        Require(node.type != MaterialType::TextureRef && node.type != MaterialType::SamplerRef &&
                node.type != MaterialType::SurfaceClosure && type(0) != MaterialType::TextureRef &&
                type(0) != MaterialType::SamplerRef && type(0) != MaterialType::SurfaceClosure,
                "references cannot be converted");
        Require(type(0) == node.type || (Scalar(type(0)) && Scalar(node.type)),
                "conversion cannot splat, truncate or resize vectors");
        if (type(0) == MaterialType::Float && node.type == MaterialType::Uint)
        {
            const auto &value = values[node.inputs[0]];
            Require(value && std::isfinite(value->floats[0]) && value->floats[0] >= 0.0f &&
                        static_cast<double>(value->floats[0]) < 4294967296.0,
                    "float to uint conversion must have a constant input in [0, 2^32)");
        }
        break;
    case MaterialOp::Less: case MaterialOp::LessEqual: case MaterialOp::Equal:
        Require(node.type == MaterialType::Bool && type(0) == type(1) && Scalar(type(0)) &&
                    (type(0) != MaterialType::Bool || node.op == MaterialOp::Equal),
                "comparison requires matching scalar operands and a bool result"); break;
    case MaterialOp::Select:
        Require(node.type != MaterialType::TextureRef && node.type != MaterialType::SamplerRef &&
                node.type != MaterialType::SurfaceClosure && type(1) != MaterialType::SurfaceClosure &&
                type(0) == MaterialType::Bool && type(1) == node.type && type(2) == node.type,
                "select requires a bool condition and matching branch types"); break;
    case MaterialOp::LogicalAnd: case MaterialOp::LogicalOr: case MaterialOp::LogicalNot:
        Require(node.type == MaterialType::Bool, "logical operation requires bool"); same(); break;
    default: Require(false, "invalid operation");
    }
}

std::vector<bool> Reachable(const std::vector<MaterialNode> &nodes, std::size_t output)
{
    std::vector<bool> reachable(nodes.size());
    const auto visit = [&](const auto &self, std::size_t index) -> void {
        if (reachable[index]) return;
        reachable[index] = true;
        const auto &node = nodes[index];
        for (std::size_t slot = 0; slot < MaterialInputCount(node.op, node.type); ++slot)
            self(self, node.inputs[slot]);
    };
    visit(visit, output);
    return reachable;
}

std::size_t ClosureOpacity(const std::vector<MaterialNode> &nodes, std::size_t output)
{
    for (;;)
    {
        const auto &node = nodes[output];
        switch (node.op)
        {
        case MaterialOp::MetallicRoughnessClosure: return node.inputs[5];
        case MaterialOp::UnlitClosure: return node.inputs[1];
        case MaterialOp::CoatClosure: case MaterialOp::AnisotropyClosure:
        case MaterialOp::SheenClosure: case MaterialOp::ThinTransmissionClosure:
        case MaterialOp::EmissionClosure: case MaterialOp::SubsurfaceClosure:
        case MaterialOp::SolidTransmissionClosure: output = node.inputs[0]; break;
        default: Require(false, "unsupported opacity closure"); return kNoMaterialInput;
        }
    }
}

std::vector<std::size_t> ClosureChain(const std::vector<MaterialNode> &nodes, std::size_t output)
{
    std::vector<std::size_t> chain;
    std::array<std::size_t, 6> scattering;
    scattering.fill(kNoMaterialInput);
    std::size_t transport = kNoMaterialInput;
    while (output != kNoMaterialInput)
    {
        Require(output < nodes.size(), "closure chain references an invalid node index " +
                std::to_string(output));
        chain.push_back(output);
        const auto &node = nodes[output];
        if (node.op == MaterialOp::MetallicRoughnessClosure) break;
        if (node.op == MaterialOp::UnlitClosure)
        {
            for (const auto layer : scattering)
                if (layer != kNoMaterialInput)
                    Require(false, "scattering node " + nodes[layer].id +
                            " cannot wrap unlit node " + node.id);
            break;
        }
        std::size_t slot = scattering.size();
        switch (node.op)
        {
        case MaterialOp::CoatClosure: slot = 0; break;
        case MaterialOp::AnisotropyClosure: slot = 1; break;
        case MaterialOp::SheenClosure: slot = 2; break;
        case MaterialOp::ThinTransmissionClosure: slot = 3; break;
        case MaterialOp::SubsurfaceClosure: slot = 4; break;
        case MaterialOp::SolidTransmissionClosure: slot = 5; break;
        case MaterialOp::EmissionClosure: break; // Emission wrappers add radiance.
        default: Require(false, "unsupported closure chain node " + node.id);
        }
        if (slot < scattering.size())
        {
            if (scattering[slot] != kNoMaterialInput)
                Require(false, "duplicate scattering node " + node.id +
                        " repeats node " + nodes[scattering[slot]].id);
            scattering[slot] = output;
            if (slot >= 3)
            {
                if (transport != kNoMaterialInput)
                    Require(false, "unsupported transport stack: node " + node.id +
                            " shares optical fields with node " + nodes[transport].id);
                transport = output;
            }
        }
        Require(node.inputs[0] < output, "invalid closure base edge at node " + node.id);
        output = node.inputs[0];
    }
    std::ranges::reverse(chain);
    std::size_t previous_scattering = kNoMaterialInput;
    unsigned previous_rank = 0;
    for (const auto index : chain)
    {
        unsigned rank = 0;
        switch (nodes[index].op)
        {
        case MaterialOp::AnisotropyClosure: rank = 0; break;
        case MaterialOp::ThinTransmissionClosure: case MaterialOp::SubsurfaceClosure:
        case MaterialOp::SolidTransmissionClosure: rank = 1; break;
        case MaterialOp::SheenClosure: rank = 2; break;
        case MaterialOp::CoatClosure: rank = 3; break;
        default: continue; // Terminals and additive emission have no scattering rank.
        }
        if (previous_scattering != kNoMaterialInput && rank < previous_rank)
            Require(false, "unsupported scattering order: node " + nodes[index].id +
                    " wraps node " + nodes[previous_scattering].id +
                    "; expected Anisotropy, one transport profile, Sheen, Coat from base outward");
        previous_scattering = index;
        previous_rank = rank;
    }
    return chain;
}

struct OptimizedGraph
{
    std::vector<MaterialNode> nodes;
    std::vector<std::optional<Value>> values;
    std::vector<std::size_t> representative;
};

std::string EmitLibraryHelpers(const OptimizedGraph &graph,
                               const std::vector<bool> &reachable)
{
    bool needs_noise = false;
    bool needs_fresnel = false;
    for (std::size_t i = 0; i < graph.nodes.size(); ++i)
        if (reachable[i])
        {
            needs_noise = needs_noise || graph.nodes[i].op == MaterialOp::SeededNoise2D;
            needs_fresnel = needs_fresnel || graph.nodes[i].op == MaterialOp::FresnelSchlick;
        }
    std::string result;
    if (needs_noise)
        result += R"hlsl(#ifndef HS_MATERIAL_NOISE_HELPERS
#define HS_MATERIAL_NOISE_HELPERS
uint HsMaterialNoiseHash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu;
    return x ^ (x >> 16);
}
float HsMaterialNoiseRandom(uint x)
{
    return (HsMaterialNoiseHash(x) & 0xffffffu) / 16777216.0;
}
uint HsMaterialNoiseCell(float2 p, uint seed)
{
    return HsMaterialNoiseHash(asuint((int)p.x) ^ HsMaterialNoiseHash(asuint((int)p.y)) ^ seed);
}
float HsMaterialSeededNoise2D(float2 p, uint seed)
{
    float2 i = floor(p), f = frac(p);
    float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    return lerp(lerp(HsMaterialNoiseRandom(HsMaterialNoiseCell(i, seed)),
                     HsMaterialNoiseRandom(HsMaterialNoiseCell(i + float2(1.0, 0.0), seed)), u.x),
                lerp(HsMaterialNoiseRandom(HsMaterialNoiseCell(i + float2(0.0, 1.0), seed)),
                     HsMaterialNoiseRandom(HsMaterialNoiseCell(i + 1.0, seed)), u.x), u.y) * 2.0 - 1.0;
}
#endif
)hlsl";
    if (needs_fresnel)
        result += R"hlsl(#ifndef HS_MATERIAL_FRESNEL_HELPER
#define HS_MATERIAL_FRESNEL_HELPER
float3 HsMaterialFresnelSchlick(float3 f0, float cos_theta)
{
    return f0 + (1.0 - f0) * pow(1.0 - saturate(cos_theta), 5.0);
}
#endif
)hlsl";
    return result;
}

std::string EmitLibraryHelpers(const OptimizedGraph &graph,
                               std::initializer_list<std::size_t> outputs)
{
    std::vector<bool> reachable(graph.nodes.size());
    for (const auto output : outputs)
        if (output != kNoMaterialInput)
        {
            const auto live = Reachable(graph.nodes, graph.representative[output]);
            for (std::size_t i = 0; i < live.size(); ++i) reachable[i] = reachable[i] || live[i];
        }
    return EmitLibraryHelpers(graph, reachable);
}

std::string EmitNormalHelpers()
{
    return R"hlsl(#ifndef HS_MATERIAL_NORMAL_HELPERS
#define HS_MATERIAL_NORMAL_HELPERS
float3 HsMaterialDecodeNormalXY(float2 encoded)
{
    float2 xy = encoded * 2.0 - 1.0;
    return float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
}
float3 HsMaterialNormalRnm(float3 normal, float3 detail)
{
    float3 t = normal + float3(0.0, 0.0, 1.0);
    float3 u = detail * float3(-1.0, -1.0, 1.0);
    return normalize(t * dot(t, u) / max(t.z, 0.001) - u);
}
float3 HsMaterialDetailFade(float3 detail, float eye_distance)
{
    return normalize(lerp(float3(0.0, 0.0, 1.0), detail, 1.0 - smoothstep(35.0, 55.0, eye_distance)));
}
float3 HsMaterialTangentToWorld(float3 normal, float3 shading_normal, float4 tangent)
{
    float3 n = normalize(shading_normal);
    float3 t = normalize(tangent.xyz - n * dot(n, tangent.xyz));
    float3 b = normalize(cross(n, t)) * tangent.w;
    return normalize(t * normal.x + b * normal.y + n * normal.z);
}
#endif
)hlsl";
}

enum class OutputKind { BaseColor, Roughness, Opacity, Closure };

void ValidateOutputSources(const MaterialGraph &graph, std::size_t output, OutputKind kind)
{
    const auto reachable = Reachable(graph.nodes, output);
    for (std::size_t i = 0; i < graph.nodes.size(); ++i)
    {
        if (!reachable[i]) continue;
        switch (graph.nodes[i].op)
        {
        case MaterialOp::SampledBaseColor:
            Require(kind == OutputKind::BaseColor || kind == OutputKind::Closure,
                    "base color input unavailable to this output"); break;
        case MaterialOp::SampledRoughness: case MaterialOp::RoughnessScale:
            Require(kind == OutputKind::Roughness || kind == OutputKind::Closure,
                    "roughness input unavailable to this output"); break;
        case MaterialOp::SampledAlpha:
            Require(kind == OutputKind::Opacity || kind == OutputKind::Closure,
                    "alpha input unavailable to this output"); break;
        default: break;
        }
    }
}

std::vector<bool> UniformNodes(const std::vector<MaterialNode> &nodes,
    const std::vector<std::optional<Value>> *values = nullptr);

std::vector<std::optional<Value>> ValidateMaterialGraph(const MaterialGraph &graph,
                                                       MaterialGraphKind expected)
{
    Require(graph.kind == expected, "unexpected graph kind");
    Require(!graph.nodes.empty() && graph.nodes.size() <= kMaxMaterialGraphNodes, "invalid graph shape");
    ValidateMaterialParameters(graph.parameters);
    if (graph.kind == MaterialGraphKind::Decal)
    {
        Require(graph.textures.size() == 1, "decal requires one texture declaration");
        ValidateDecalMaterialTextureDefinition(graph.textures.front());
    }
    else if (!graph.textures.empty()) ValidateMaterialTextureDefinitions(graph.textures);
    std::unordered_set<std::string> declarations;
    Require(graph.samplers.size() <= 4, "too many sampler declarations");
    for (const auto &texture : graph.textures)
        Require(ValidId(texture.id), "invalid texture declaration ID");
    for (const auto &sampler : graph.samplers)
    {
        Require(ValidId(sampler.id) && declarations.insert(sampler.id).second,
                "invalid or duplicate sampler declaration ID");
        (void)MaterialSamplerRegister(sampler.profile);
    }
    std::unordered_set<std::string> ids;
    std::vector<std::optional<Value>> values(graph.nodes.size());
    for (std::size_t i = 0; i < graph.nodes.size(); ++i)
    {
        Require(ValidId(graph.nodes[i].id) && ids.insert(graph.nodes[i].id).second,
                "invalid or duplicate node ID");
        ValidateNode(graph, i, values);
        values[i] = Fold(graph.nodes[i], values);
    }
    const auto output = [&](std::size_t index, MaterialType type, OutputKind kind) {
        Require(index < graph.nodes.size() && graph.nodes[index].type == type, "surface output type mismatch");
        ValidateOutputSources(graph, index, kind);
    };
    switch (expected)
    {
    case MaterialGraphKind::Rock0: case MaterialGraphKind::Opaque:
        if (graph.closure_output != kNoMaterialInput)
        {
            output(graph.closure_output, MaterialType::SurfaceClosure, OutputKind::Closure);
            if (graph.base_color_output != kNoMaterialInput)
            {
                output(graph.base_color_output, MaterialType::Float3, OutputKind::BaseColor);
                output(graph.roughness_output, MaterialType::Float, OutputKind::Roughness);
            }
        }
        else
        {
            output(graph.base_color_output, MaterialType::Float3, OutputKind::BaseColor);
            output(graph.roughness_output, MaterialType::Float, OutputKind::Roughness);
        }
        break;
    case MaterialGraphKind::Masked: case MaterialGraphKind::Translucent:
    case MaterialGraphKind::HeroRefractive:
        if (graph.closure_output != kNoMaterialInput)
        {
            output(graph.closure_output, MaterialType::SurfaceClosure, OutputKind::Closure);
            if (graph.base_color_output != kNoMaterialInput)
            {
                output(graph.base_color_output, MaterialType::Float3, OutputKind::BaseColor);
                output(graph.roughness_output, MaterialType::Float, OutputKind::Roughness);
                output(graph.opacity_output, MaterialType::Float, OutputKind::Opacity);
            }
        }
        else
        {
            output(graph.base_color_output, MaterialType::Float3, OutputKind::BaseColor);
            output(graph.roughness_output, MaterialType::Float, OutputKind::Roughness);
            output(graph.opacity_output, MaterialType::Float, OutputKind::Opacity);
        }
        break;
    case MaterialGraphKind::Terrain:
        if (graph.closure_output != kNoMaterialInput)
        {
            output(graph.closure_output, MaterialType::SurfaceClosure, OutputKind::Closure);
            if (graph.base_color_output != kNoMaterialInput)
            {
                output(graph.base_color_output, MaterialType::Float3, OutputKind::BaseColor);
                output(graph.roughness_output, MaterialType::Float, OutputKind::Roughness);
            }
        }
        else
        {
            output(graph.base_color_output, MaterialType::Float3, OutputKind::BaseColor);
            output(graph.roughness_output, MaterialType::Float, OutputKind::Roughness);
        }
        Require(graph.terrain_output < graph.nodes.size() &&
                graph.nodes[graph.terrain_output].type == MaterialType::TerrainResult,
                "terrain aggregate output type mismatch");
        break;
    case MaterialGraphKind::Leaf0:
        output(graph.opacity_output, MaterialType::Float, OutputKind::Opacity); break;
    case MaterialGraphKind::Decal:
        output(graph.closure_output, MaterialType::SurfaceClosure, OutputKind::Closure);
        Require(graph.normal_space == MaterialNormalSpace::World,
                "decal result normal must be world space");
        Require(graph.textures.size() == 1 && graph.samplers.size() == 1,
                "decal profile requires one declared texture and sampler");
        Require(graph.nodes[graph.closure_output].op == MaterialOp::DecalFracture ||
                graph.nodes[graph.closure_output].op == MaterialOp::DecalOrganic,
                "decal output must be a typed projected decal function");
        ValidateDecalReceiverPolicy(graph.decal_receivers);
        break;
    default: Require(false, "unknown graph kind");
    }
    if (graph.normal_output != kNoMaterialInput)
        Require(graph.normal_output < graph.nodes.size() &&
                graph.nodes[graph.normal_output].type == MaterialType::Float3,
                "normal output type mismatch");
    if (graph.ao_output != kNoMaterialInput)
        Require(graph.ao_output < graph.nodes.size() &&
                graph.nodes[graph.ao_output].type == MaterialType::Float,
                "AO output type mismatch");
    if (graph.closure_output != kNoMaterialInput && graph.kind != MaterialGraphKind::Decal)
    {
        const auto chain = ClosureChain(graph.nodes, graph.closure_output);
        const auto uniform = UniformNodes(graph.nodes, &values);
        for (const auto index : chain)
        {
            const auto &node = graph.nodes[index];
            if (node.op != MaterialOp::SubsurfaceClosure && node.op != MaterialOp::SolidTransmissionClosure)
                continue;
            for (std::size_t slot = 1; slot < MaterialInputCount(node.op, node.type); ++slot)
                Require(uniform[node.inputs[slot]], "optical profile node " + node.id +
                    " has unsupported varying input from node " + graph.nodes[node.inputs[slot]].id);
        }
    }
    return values;
}

std::string Number(float value)
{
    Require(std::isfinite(value), "cannot emit nonfinite constant");
    std::array<char, 64> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                         std::chars_format::general);
    Require(converted.ec == std::errc{}, "cannot format constant");
    std::string result(buffer.data(), converted.ptr);
    if (result.find_first_of(".eE") == std::string::npos) result += ".0";
    return result;
}

std::string Literal(const Value &value)
{
    if (value.type == MaterialType::Bool) return value.boolean ? "true" : "false";
    if (value.type == MaterialType::Uint) return std::to_string(value.integer) + "u";
    if (value.type == MaterialType::Float) return Number(value.floats[0]);
    std::string result = TypeName(value.type) + "(";
    for (std::size_t i = 0; i < MaterialComponentCount(value.type); ++i)
    {
        if (i) result += ", ";
        result += Number(value.floats[i]);
    }
    return result + ")";
}

OptimizedGraph Optimize(const MaterialGraph &graph, std::vector<std::optional<Value>> values)
{
    OptimizedGraph result{graph.nodes, std::move(values), std::vector<std::size_t>(graph.nodes.size())};
    std::map<std::string, std::size_t> expressions;
    for (std::size_t i = 0; i < result.nodes.size(); ++i)
    {
        auto &node = result.nodes[i];
        const auto count = MaterialInputCount(node.op, node.type);
        for (std::size_t slot = 0; slot < count; ++slot)
            node.inputs[slot] = result.representative[node.inputs[slot]];
        if (node.op == MaterialOp::Select && result.values[node.inputs[0]])
        {
            result.representative[i] = node.inputs[result.values[node.inputs[0]]->boolean ? 1 : 2];
            continue;
        }
        if (node.op == MaterialOp::Convert && result.nodes[node.inputs[0]].type == node.type)
        {
            result.representative[i] = node.inputs[0];
            continue;
        }
        std::string key = TypeName(node.type) + ":";
        if (result.values[i])
        {
            node.op = MaterialOp::Constant;
            node.inputs.fill(kNoMaterialInput);
            key += Literal(*result.values[i]);
        }
        else
        {
            key += std::to_string(static_cast<unsigned>(node.op)) + ":" + std::to_string(node.component) +
                ":" + std::to_string(node.declaration_index) + ":" +
                std::to_string(static_cast<unsigned>(node.context_input)) + ":" +
                std::to_string(static_cast<unsigned>(node.terrain_field));
            for (std::size_t slot = 0; slot < count; ++slot)
                key += ":" + std::to_string(node.inputs[slot]);
        }
        const auto [found, inserted] = expressions.emplace(std::move(key), i);
        (void)inserted;
        result.representative[i] = found->second;
    }
    return result;
}

std::string Expression(const MaterialNode &node, const std::optional<Value> &value,
                       std::string_view signature,
                       std::span<const MaterialParameterDefinition> parameters = {})
{
    if (value) return Literal(*value);
    const auto input = [&](std::size_t slot) { return "n" + std::to_string(node.inputs[slot]); };
    const auto binary = [&](std::string_view op) { return input(0) + " " + std::string(op) + " " + input(1); };
    const auto call = [&](std::string_view name, std::size_t count) {
        std::string result = std::string(name) + "(";
        for (std::size_t i = 0; i < count; ++i)
        {
            if (i) result += ", ";
            result += input(i);
        }
        return result + ")";
    };
    switch (node.op)
    {
    case MaterialOp::SampledBaseColor: return signature.starts_with("HsRock0") ? "sampled_base_color" : "sampled";
    case MaterialOp::SampledRoughness: return signature.starts_with("HsRock0") ? "sampled_roughness" : "sampled";
    case MaterialOp::RoughnessScale: return signature.starts_with("HsRock0") ? "roughness_scale" : "scale";
    case MaterialOp::Parameter:
        Require(node.declaration_index < parameters.size(),
                "material parameter requires an explicit legacy function signature");
        if (parameters[node.declaration_index].id == "roughness_scale")
            return signature.starts_with("HsRock0") ? "roughness_scale" : "scale";
        Require(false, "material parameter has no legacy function argument"); return {};
    case MaterialOp::SampledAlpha: return signature.starts_with("HsLeaf0") ? "sampled_alpha" : "sampled";
    case MaterialOp::Add: return binary("+");
    case MaterialOp::Subtract: return binary("-");
    case MaterialOp::Multiply: return binary("*");
    case MaterialOp::Divide: return binary("/");
    case MaterialOp::Less: return binary("<");
    case MaterialOp::LessEqual: return binary("<=");
    case MaterialOp::Equal: return binary("==");
    case MaterialOp::LogicalAnd: return binary("&&");
    case MaterialOp::LogicalOr: return binary("||");
    case MaterialOp::LogicalNot: return "!" + input(0);
    case MaterialOp::Negate: return "-" + input(0);
    case MaterialOp::Saturate: return call("saturate", 1);
    case MaterialOp::Min: return call("min", 2);
    case MaterialOp::Max: return call("max", 2);
    case MaterialOp::Lerp: return call("lerp", 3);
    case MaterialOp::Clamp: return call("clamp", 3);
    case MaterialOp::Construct: return call(TypeName(node.type), MaterialComponentCount(node.type));
    case MaterialOp::Extract: return input(0) + "." + "xyzw"[node.component];
    case MaterialOp::Convert: return call(TypeName(node.type), 1);
    case MaterialOp::Select: return input(0) + " ? " + input(1) + " : " + input(2);
    case MaterialOp::MetallicRoughnessClosure:
        return call("HsMaterialMakeSurfaceClosure", 6);
    case MaterialOp::CoatClosure: return call("HsMaterialApplyCoat", 3);
    case MaterialOp::AnisotropyClosure: return call("HsMaterialApplyAnisotropy", 3);
    case MaterialOp::SheenClosure: return call("HsMaterialApplySheen", 4);
    case MaterialOp::ThinTransmissionClosure: return call("HsMaterialApplyThinTransmission", 3);
    case MaterialOp::EmissionClosure: return call("HsMaterialApplyEmission", 2);
    case MaterialOp::UnlitClosure: return call("HsMaterialMakeUnlit", 2);
    case MaterialOp::SubsurfaceClosure: return call("HsMaterialApplySubsurface", 4);
    case MaterialOp::SolidTransmissionClosure: return call("HsMaterialApplySolidTransmission", 5);
    case MaterialOp::SeededNoise2D:
        return "HsMaterialSeededNoise2D(" + input(0) + ", " + input(1) + ")";
    case MaterialOp::FresnelSchlick:
        return "HsMaterialFresnelSchlick(" + input(0) + ", " + input(1) + ")";
    default: Require(false, "invalid emission operation"); return {};
    }
}

std::string EmitFunction(const OptimizedGraph &graph, std::size_t output,
                         MaterialType output_type, std::string_view signature,
                         std::span<const MaterialParameterDefinition> parameters = {})
{
    output = graph.representative[output];
    const auto reachable = Reachable(graph.nodes, output);
    std::string result = TypeName(output_type) + " " + std::string(signature) + "\n{\n";
    for (std::size_t i = 0; i < graph.nodes.size(); ++i)
        if (reachable[i])
            result += "    " + TypeName(graph.nodes[i].type) + " n" + std::to_string(i) + " = " +
                      Expression(graph.nodes[i], graph.values[i], signature, parameters) + ";\n";
    return result + "    return n" + std::to_string(output) + ";\n}\n";
}

std::string ContextField(MaterialContextInput input)
{
    constexpr std::array names{"uv", "d_uv_dx", "d_uv_dy", "layer", "actual_world_position",
        "d_actual_world_dx", "d_actual_world_dy", "stable_world_position", "d_stable_world_dx",
        "d_stable_world_dy", "normal", "tangent", "eye_distance", "sampled_base_color",
        "sampled_roughness", "sampled_alpha", "roughness_scale", "sampled_ao", "shading_normal",
        "mesh", "seed", "view_direction", "metallic", "sampled_emission", "sampled_metallic",
        "decal_tint_hdr", "decal_projector", "decal_shape"};
    const auto index = static_cast<std::size_t>(input);
    Require(index < names.size(), "invalid context input");
    return "ctx." + std::string(names[index]);
}

std::string ContextExpression(const OptimizedGraph &graph, const MaterialGraph &source,
                              std::size_t index, std::size_t texture_slot_base)
{
    const auto &node = graph.nodes[index];
    const auto input = [&](std::size_t slot) { return "n" + std::to_string(node.inputs[slot]); };
    switch (node.op)
    {
    case MaterialOp::Parameter:
    {
        Require(node.declaration_index < source.parameters.size(),
                "material parameter declaration is out of range during emission");
        const auto slot = "MaterialParameters[ctx.parameter_base + " +
            std::to_string(node.declaration_index) + "]";
        switch (source.parameters[node.declaration_index].type)
        {
        case MaterialParameterType::Bool: return slot + ".x != 0u";
        case MaterialParameterType::Uint: return slot + ".x";
        case MaterialParameterType::Float: return "asfloat(" + slot + ".x)";
        case MaterialParameterType::Float2: return "asfloat(" + slot + ".xy)";
        case MaterialParameterType::Float3: return "asfloat(" + slot + ".xyz)";
        case MaterialParameterType::Float4: return "asfloat(" + slot + ")";
        default: Require(false, "invalid material parameter type during emission"); return {};
        }
    }
    case MaterialOp::Context: return ContextField(node.context_input);
    case MaterialOp::SampledBaseColor: return "ctx.sampled_base_color";
    case MaterialOp::SampledRoughness: return "ctx.sampled_roughness";
    case MaterialOp::SampledAlpha: return "ctx.sampled_alpha";
    case MaterialOp::RoughnessScale: return "ctx.roughness_scale";
    case MaterialOp::SampleGrad: case MaterialOp::SampleLevel:
    {
        const auto texture_index = graph.nodes[node.inputs[0]].declaration_index;
        const auto sampler_index = graph.nodes[node.inputs[1]].declaration_index;
        constexpr std::array sampler_names{"LinearClamp", "MaterialSampler", "EnvironmentWrap", "FoliageClamp"};
        const auto profile = static_cast<std::size_t>(source.samplers[sampler_index].profile);
        Require(profile >= 1 && profile <= sampler_names.size(), "invalid sampler profile");
        const auto texture = "HsMaterialTexture" + std::to_string(texture_index);
        const auto prefix = texture_slot_base == kNoMaterialInput
            ? texture + (node.op == MaterialOp::SampleGrad ? ".SampleGrad(" : ".SampleLevel(") +
                sampler_names[profile - 1] + ", float3(" + input(2) + ", " + input(3) + "), " + input(4)
            : (node.op == MaterialOp::SampleGrad ? "HsTextureSampleGrad(" : "HsTextureSampleLevel(") +
                texture + ", " + std::to_string(texture_slot_base + texture_index) + "u, " +
                sampler_names[profile - 1] + ", float3(" + input(2) + ", " + input(3) + "), " + input(4);
        return prefix + (node.op == MaterialOp::SampleGrad ? ", " + input(5) : "") + ")";
    }
    case MaterialOp::DecodeNormalXY: return "HsMaterialDecodeNormalXY(" + input(0) + ")";
    case MaterialOp::NormalRnm: return "HsMaterialNormalRnm(" + input(0) + ", " + input(1) + ")";
    case MaterialOp::DetailFade: return "HsMaterialDetailFade(" + input(0) + ", " + input(1) + ")";
    case MaterialOp::TangentToWorld:
        return "HsMaterialTangentToWorld(" + input(0) + ", " + input(1) + ", " + input(2) + ")";
    case MaterialOp::TerrainStochastic:
    {
        std::string result = "HsMaterialTerrainStochastic(";
        for (std::size_t slot = 0; slot < 6; ++slot)
        {
            if (slot) result += ", ";
            result += "HsMaterialTexture" + std::to_string(
                graph.nodes[node.inputs[slot]].declaration_index);
        }
        result += ", EnvironmentWrap, LinearClamp";
        for (std::size_t slot = 8; slot < 15; ++slot)
            result += ", " + input(slot);
        Require(texture_slot_base != kNoMaterialInput, "terrain requires actual texture binding slots");
        for (std::size_t slot = 0; slot < 6; ++slot)
            result += ", " + std::to_string(texture_slot_base + graph.nodes[node.inputs[slot]].declaration_index) + "u";
        // Meadow ground reuses its authored UV channel for static wet/dry controls.
        return result + ", ctx.uv)";
    }
    case MaterialOp::TerrainField:
    {
        constexpr std::array fields{"base_color", "normal", "detail", "roughness", "ao"};
        return input(0) + "." + std::string(fields[static_cast<std::size_t>(node.terrain_field)]);
    }
    case MaterialOp::DecalFracture: case MaterialOp::DecalOrganic:
    {
        std::string result = node.op == MaterialOp::DecalFracture ? "HsDecalFracture(" : "HsDecalOrganic(";
        result += "HsMaterialTexture" + std::to_string(graph.nodes[node.inputs[0]].declaration_index) + ", LinearClamp";
        for (std::size_t slot = 2; slot < 15; ++slot) result += ", " + input(slot);
        return result + ")";
    }
    default: return Expression(node, graph.values[index], "context");
    }
}

std::string EmitContextNodes(const OptimizedGraph &graph, const MaterialGraph &source,
                            const std::vector<bool> &reachable, std::size_t texture_slot_base = kNoMaterialInput)
{
    std::string result;
    for (std::size_t i = 0; i < graph.nodes.size(); ++i)
        if (reachable[i] && graph.nodes[i].type != MaterialType::TextureRef &&
            graph.nodes[i].type != MaterialType::SamplerRef)
            result += "    " + TypeName(graph.nodes[i].type) + " n" + std::to_string(i) + " = " +
                ContextExpression(graph, source, i, texture_slot_base) + ";\n";
    return result;
}

std::vector<bool> UniformNodes(const std::vector<MaterialNode> &nodes,
    const std::vector<std::optional<Value>> *values)
{
    std::vector<bool> uniform(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        const auto &node = nodes[i];
        if (values && (*values)[i]) { uniform[i] = true; continue; }
        if (values && node.op == MaterialOp::Select && (*values)[node.inputs[0]])
        {
            uniform[i] = uniform[node.inputs[(*values)[node.inputs[0]]->boolean ? 1 : 2]];
            continue;
        }
        switch (node.op)
        {
        case MaterialOp::Constant: case MaterialOp::Parameter:
            uniform[i] = true; break;
        case MaterialOp::Multiply: case MaterialOp::Saturate: case MaterialOp::Add:
        case MaterialOp::Subtract: case MaterialOp::Divide: case MaterialOp::Min:
        case MaterialOp::Max: case MaterialOp::Negate: case MaterialOp::Lerp:
        case MaterialOp::Clamp: case MaterialOp::Construct: case MaterialOp::Extract:
        case MaterialOp::Convert: case MaterialOp::Less: case MaterialOp::LessEqual:
        case MaterialOp::Equal: case MaterialOp::Select: case MaterialOp::LogicalAnd:
        case MaterialOp::LogicalOr: case MaterialOp::LogicalNot:
        case MaterialOp::DecodeNormalXY: case MaterialOp::NormalRnm:
        case MaterialOp::DetailFade: case MaterialOp::TangentToWorld:
        case MaterialOp::SeededNoise2D: case MaterialOp::FresnelSchlick:
            uniform[i] = true;
            for (std::size_t slot = 0; slot < MaterialInputCount(node.op, node.type); ++slot)
                uniform[i] = uniform[i] && uniform[node.inputs[slot]];
            break;
        default: break; // Every sampled value and context attribute is varying.
        }
    }
    return uniform;
}

std::vector<bool> UniformNodes(const OptimizedGraph &graph) { return UniformNodes(graph.nodes, &graph.values); }

std::vector<std::size_t> ClosureChain(const OptimizedGraph &graph, std::size_t output)
{
    return ClosureChain(graph.nodes, output);
}

ClosureReflection ReflectClosure(const OptimizedGraph &graph,
                                 const std::vector<std::size_t> &chain,
                                 const std::vector<bool> &uniform,
                                 MaterialNormalSpace normal_space)
{
    ClosureReflection result;
    if (chain.empty()) result.capability_mask = static_cast<std::uint32_t>(MaterialClosureCapability::Basic);
    for (const auto index : chain)
    {
        const auto &node = graph.nodes[index];
        MaterialClosureCapability capability{};
        MaterialClosureVarying varying{};
        std::size_t first = 1;
        std::size_t count = MaterialInputCount(node.op, node.type);
        switch (node.op)
        {
        case MaterialOp::MetallicRoughnessClosure:
            capability = MaterialClosureCapability::Basic; count = first; break;
        case MaterialOp::CoatClosure:
            capability = MaterialClosureCapability::Coat; varying = MaterialClosureVarying::Coat; break;
        case MaterialOp::AnisotropyClosure:
            capability = MaterialClosureCapability::Anisotropy; varying = MaterialClosureVarying::Tangent; break;
        case MaterialOp::SheenClosure:
            capability = MaterialClosureCapability::Sheen; varying = MaterialClosureVarying::Sheen; break;
        case MaterialOp::ThinTransmissionClosure:
            capability = MaterialClosureCapability::ThinTransmission;
            varying = MaterialClosureVarying::Transmission; break;
        case MaterialOp::EmissionClosure:
            capability = MaterialClosureCapability::Emission; varying = MaterialClosureVarying::Emission; break;
        case MaterialOp::UnlitClosure:
            capability = MaterialClosureCapability::Unlit; varying = MaterialClosureVarying::Emission;
            first = 0; count = 1; break; // Opacity is retained by raster coverage.
        case MaterialOp::SubsurfaceClosure:
            capability = MaterialClosureCapability::Subsurface; count = first; break;
        case MaterialOp::SolidTransmissionClosure:
            capability = MaterialClosureCapability::SolidTransmission; count = first; break;
        default: Require(false, "unsupported reflected closure");
        }
        result.capability_mask |= static_cast<std::uint32_t>(capability);
        for (std::size_t slot = first; slot < count; ++slot)
            if (!uniform[node.inputs[slot]])
                result.varying_mask |= static_cast<std::uint32_t>(varying);
    }
    if (normal_space == MaterialNormalSpace::Tangent &&
        (result.capability_mask & static_cast<std::uint32_t>(MaterialClosureCapability::Anisotropy)) != 0)
        result.varying_mask |= static_cast<std::uint32_t>(MaterialClosureVarying::Tangent);
    return result;
}

std::uint32_t ClosureSidecarBit(MaterialOp op)
{
    switch (op)
    {
    case MaterialOp::AnisotropyClosure: return 1u;
    case MaterialOp::CoatClosure: return 2u;
    case MaterialOp::SheenClosure: return 4u;
    case MaterialOp::ThinTransmissionClosure: return 8u;
    case MaterialOp::EmissionClosure: case MaterialOp::UnlitClosure: return 16u;
    default: return 0;
    }
}
} // namespace

ClosureReflection AnalyzeClosure(const MaterialGraph &graph)
{
    if (graph.kind == MaterialGraphKind::Decal)
    {
        ValidateDecalMaterialGraph(graph);
        return {static_cast<std::uint32_t>(MaterialClosureCapability::Basic) |
                static_cast<std::uint32_t>(MaterialClosureCapability::Emission),
                static_cast<std::uint32_t>(MaterialClosureVarying::Emission)};
    }
    const auto optimized = Optimize(graph, ValidateMaterialGraph(graph, graph.kind));
    const auto chain = graph.closure_output == kNoMaterialInput ? std::vector<std::size_t>{} :
        ClosureChain(optimized, optimized.representative[graph.closure_output]);
    return ReflectClosure(optimized, chain, UniformNodes(optimized), graph.normal_space);
}

std::string EmitUniformClosureHlsl(const MaterialGraph &graph, std::string_view function_name,
                                  std::uint32_t materialized_mask)
{
    Require(graph.kind != MaterialGraphKind::Decal,
            "decal domain must be evaluated in the common Surface pass before lighting");
    // The identifier shares the authored ID rules except for generated uppercase names.
    Require(!function_name.empty() && function_name.size() <= 128, "invalid uniform closure function name");
    for (std::size_t i = 0; i < function_name.size(); ++i)
    {
        const auto c = function_name[i];
        Require((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
                (i != 0 && c >= '0' && c <= '9'), "invalid uniform closure function name");
    }
    const auto optimized = Optimize(graph, ValidateMaterialGraph(graph, graph.kind));
    const auto chain = graph.closure_output == kNoMaterialInput ? std::vector<std::size_t>{} :
        ClosureChain(optimized, optimized.representative[graph.closure_output]);
    const auto uniform = UniformNodes(optimized);
    const auto reflection = ReflectClosure(optimized, chain, uniform, graph.normal_space);
    Require((materialized_mask & ~31u) == 0, "uniform closure emitter has unknown sidecar mask bits");
    Require((reflection.varying_mask & ~materialized_mask) == 0,
            "uniform closure emitter requires sidecar families for varying mask " +
            std::to_string(reflection.varying_mask & ~materialized_mask));
    std::vector<bool> reachable(optimized.nodes.size());
    for (const auto index : chain)
    {
        const auto &node = optimized.nodes[index];
        if (node.op == MaterialOp::MetallicRoughnessClosure ||
            (ClosureSidecarBit(node.op) & materialized_mask) != 0) continue;
        const auto first = node.op == MaterialOp::UnlitClosure ? 0u : 1u;
        const auto count = node.op == MaterialOp::UnlitClosure ? 1u : MaterialInputCount(node.op, node.type);
        for (std::size_t slot = first; slot < count; ++slot)
        {
            Require(uniform[node.inputs[slot]], "uniform closure emitter requires varying sidecars for node " +
                    node.id + " input " + std::to_string(slot));
            const auto live = Reachable(optimized.nodes, node.inputs[slot]);
            for (std::size_t i = 0; i < live.size(); ++i) reachable[i] = reachable[i] || live[i];
        }
    }
    std::string result = EmitLibraryHelpers(optimized, reachable);
    for (std::size_t i = 0; i < optimized.nodes.size(); ++i)
        if (reachable[i] && (optimized.nodes[i].op == MaterialOp::DecodeNormalXY ||
            optimized.nodes[i].op == MaterialOp::NormalRnm || optimized.nodes[i].op == MaterialOp::DetailFade ||
            optimized.nodes[i].op == MaterialOp::TangentToWorld))
        {
            result += EmitNormalHelpers(); break;
        }
    result += "HsSurfaceClosure " + std::string(function_name) +
        "(HsSurfaceClosure base, HsMaterialContext ctx)\n{\n";
    result += EmitContextNodes(optimized, graph, reachable);
    for (const auto index : chain)
    {
        const auto &node = optimized.nodes[index];
        if ((ClosureSidecarBit(node.op) & materialized_mask) != 0)
        {
            if (node.op == MaterialOp::UnlitClosure)
                result += "    base.unlit = true;\n";
            continue;
        }
        std::string_view helper;
        switch (node.op)
        {
        case MaterialOp::MetallicRoughnessClosure: continue;
        case MaterialOp::CoatClosure: helper = "HsMaterialApplyCoat"; break;
        case MaterialOp::AnisotropyClosure: helper = "HsMaterialApplyAnisotropy"; break;
        case MaterialOp::SheenClosure: helper = "HsMaterialApplySheen"; break;
        case MaterialOp::ThinTransmissionClosure: helper = "HsMaterialApplyThinTransmission"; break;
        case MaterialOp::EmissionClosure: helper = "HsMaterialApplyEmission"; break;
        case MaterialOp::SubsurfaceClosure: helper = "HsMaterialApplySubsurface"; break;
        case MaterialOp::SolidTransmissionClosure: helper = "HsMaterialApplySolidTransmission"; break;
        case MaterialOp::UnlitClosure:
            result += "    base = HsMaterialMakeUnlit(n" + std::to_string(node.inputs[0]) +
                ", base.opacity);\n";
            continue;
        default: Require(false, "unsupported uniform closure");
        }
        result += "    base = " + std::string(helper) + "(base";
        for (std::size_t slot = 1; slot < MaterialInputCount(node.op, node.type); ++slot)
            result += ", n" + std::to_string(node.inputs[slot]);
        result += ");\n";
    }
    return result + "    return base;\n}\n";
}

void ValidateRock0MaterialGraph(const MaterialGraph &graph)
{
    (void)ValidateMaterialGraph(graph, MaterialGraphKind::Rock0);
}

void ValidateLeaf0MaterialGraph(const MaterialGraph &graph)
{
    (void)ValidateMaterialGraph(graph, MaterialGraphKind::Leaf0);
}

void ValidateEnvironmentMaterialGraph(const MaterialGraph &graph)
{
    Require(graph.kind == MaterialGraphKind::Opaque || graph.kind == MaterialGraphKind::Masked,
            "expected generic material graph");
    (void)ValidateMaterialGraph(graph, graph.kind);
}

void ValidateHeroMaterialGraph(const MaterialGraph &graph)
{
    Require(graph.kind == MaterialGraphKind::Opaque || graph.kind == MaterialGraphKind::Masked ||
            graph.kind == MaterialGraphKind::Translucent || graph.kind == MaterialGraphKind::HeroRefractive,
            "expected hero material graph");
    Require(graph.closure_output != kNoMaterialInput && graph.textures.empty() && graph.samplers.empty(),
            "hero materials require a typed closure and the native sampled-context adapter");
    Require(graph.normal_space == MaterialNormalSpace::Tangent ||
            graph.normal_space == MaterialNormalSpace::World, "invalid hero normal output space");
    (void)ValidateMaterialGraph(graph, graph.kind);
}

void ValidateTerrainMaterialGraph(const MaterialGraph &graph)
{
    Require(graph.kind == MaterialGraphKind::Terrain, "expected terrain material graph");
    (void)ValidateMaterialGraph(graph, MaterialGraphKind::Terrain);
}

void ValidateDecalMaterialGraph(const MaterialGraph &graph)
{
    (void)ValidateMaterialGraph(graph, MaterialGraphKind::Decal);
}

void ValidateMaterialSampleLayers(const MaterialGraph &graph,
    std::span<const std::uint32_t> texture_layer_counts, std::uint32_t max_context_layer)
{
    if (graph.kind == MaterialGraphKind::Terrain) ValidateTerrainMaterialGraph(graph);
    else if (graph.kind == MaterialGraphKind::Decal) ValidateDecalMaterialGraph(graph);
    else ValidateEnvironmentMaterialGraph(graph);
    const auto values = ValidateMaterialGraph(graph, graph.kind);
    Require(texture_layer_counts.size() == graph.textures.size() && max_context_layer < 2048,
            "invalid texture layer validation shape");
    for (const auto count : texture_layer_counts)
        Require(count > 0 && count <= 2048, "invalid texture array layer count");
    struct Interval { std::uint32_t lo, hi; };
    using Domain = std::vector<Interval>;
    constexpr std::uint64_t modulus = std::uint64_t{1} << 32;
    const auto normalize = [](Domain domain) {
        std::ranges::sort(domain, {}, &Interval::lo);
        Domain result;
        for (const auto interval : domain)
            if (!result.empty() && std::uint64_t(interval.lo) <= std::uint64_t(result.back().hi) + 1)
                result.back().hi = std::max(result.back().hi, interval.hi);
            else result.push_back(interval);
        // Keep proof work bounded by the input graph limit. Coalescing to the
        // enclosing interval is conservative; it never hides an invalid layer.
        if (result.size() > kMaxMaterialGraphNodes)
            return Domain{{result.front().lo, result.back().hi}};
        return result;
    };
    const auto wrap_range = [&](std::uint64_t lo, std::uint64_t hi) -> Domain {
        if (hi - lo >= modulus - 1) return {{0, UINT32_MAX}};
        const auto first = static_cast<std::uint32_t>(lo);
        const auto last = static_cast<std::uint32_t>(hi);
        if (lo / modulus == hi / modulus) return {{first, last}};
        return {{0, last}, {first, UINT32_MAX}};
    };
    const auto binary = [&](MaterialOp op, const Domain &a, const Domain &b) {
        Domain result;
        for (const auto x : a) for (const auto y : b)
        {
            Domain pair;
            switch (op)
            {
            case MaterialOp::Add:
                pair = wrap_range(std::uint64_t(x.lo) + y.lo, std::uint64_t(x.hi) + y.hi); break;
            case MaterialOp::Subtract:
                pair = wrap_range(modulus + x.lo - y.hi, modulus + x.hi - y.lo); break;
            case MaterialOp::Multiply:
                pair = wrap_range(std::uint64_t(x.lo) * y.lo, std::uint64_t(x.hi) * y.hi); break;
            case MaterialOp::Divide:
                Require(y.lo != 0, "invalid sample layer divisor");
                pair = {{x.lo / y.hi, x.hi / y.lo}}; break;
            case MaterialOp::Min: pair = {{std::min(x.lo, y.lo), std::min(x.hi, y.hi)}}; break;
            case MaterialOp::Max: pair = {{std::max(x.lo, y.lo), std::max(x.hi, y.hi)}}; break;
            default: Require(false, "unsupported sample layer arithmetic");
            }
            result.insert(result.end(), pair.begin(), pair.end());
        }
        return normalize(std::move(result));
    };
    for (std::uint32_t layer = 0; layer <= max_context_layer; ++layer)
    {
        std::vector<std::optional<Domain>> uint_cache(graph.nodes.size());
        std::vector<unsigned char> bool_cache(graph.nodes.size());
        std::function<Domain(std::size_t)> uint_domain;
        std::function<unsigned char(std::size_t)> bool_domain;
        bool_domain = [&](std::size_t index) -> unsigned char {
            if (bool_cache[index]) return bool_cache[index];
            const auto &node = graph.nodes[index];
            Require(node.type == MaterialType::Bool, "sample layer condition is not bool");
            unsigned char result = 3; // bit 0 false, bit 1 true
            if (values[index]) result = values[index]->boolean ? 2 : 1;
            else if (node.op == MaterialOp::LogicalNot)
            {
                const auto source = bool_domain(node.inputs[0]);
                result = static_cast<unsigned char>(((source & 1) << 1) | ((source & 2) >> 1));
            }
            else if (node.op == MaterialOp::Select)
            {
                const auto condition = bool_domain(node.inputs[0]);
                result = 0;
                if (condition & 2) result |= bool_domain(node.inputs[1]);
                if (condition & 1) result |= bool_domain(node.inputs[2]);
            }
            else if (node.op == MaterialOp::LogicalAnd || node.op == MaterialOp::LogicalOr ||
                     (node.op == MaterialOp::Equal && graph.nodes[node.inputs[0]].type == MaterialType::Bool))
            {
                const auto a = bool_domain(node.inputs[0]), b = bool_domain(node.inputs[1]);
                result = 0;
                for (unsigned x = 0; x < 2; ++x) for (unsigned y = 0; y < 2; ++y)
                    if ((a & (1u << x)) && (b & (1u << y)))
                    {
                        const bool value = node.op == MaterialOp::LogicalAnd ? x && y :
                            node.op == MaterialOp::LogicalOr ? x || y : x == y;
                        result |= static_cast<unsigned char>(value ? 2 : 1);
                    }
            }
            else if (node.op == MaterialOp::Convert && graph.nodes[node.inputs[0]].type == MaterialType::Uint)
            {
                result = 0;
                for (const auto range : uint_domain(node.inputs[0]))
                {
                    if (range.lo == 0) result |= 1;
                    if (range.hi > 0) result |= 2;
                }
            }
            else if ((node.op == MaterialOp::Less || node.op == MaterialOp::LessEqual ||
                      node.op == MaterialOp::Equal) && graph.nodes[node.inputs[0]].type == MaterialType::Uint)
            {
                result = 0;
                for (const auto a : uint_domain(node.inputs[0])) for (const auto b : uint_domain(node.inputs[1]))
                    if (node.op == MaterialOp::Equal)
                    {
                        if (a.lo <= b.hi && b.lo <= a.hi) result |= 2;
                        if (a.lo != a.hi || b.lo != b.hi || a.lo != b.lo) result |= 1;
                    }
                    else
                    {
                        if (node.op == MaterialOp::Less ? a.lo < b.hi : a.lo <= b.hi) result |= 2;
                        if (node.op == MaterialOp::Less ? a.hi >= b.lo : a.hi > b.lo) result |= 1;
                    }
            }
            bool_cache[index] = result;
            return result;
        };
        uint_domain = [&](std::size_t index) -> Domain {
            if (uint_cache[index]) return *uint_cache[index];
            const auto &node = graph.nodes[index];
            Require(node.type == MaterialType::Uint, "sample layer expression is not uint");
            Domain result;
            if (values[index]) result = {{values[index]->integer, values[index]->integer}};
            else if (node.op == MaterialOp::Context && node.context_input == MaterialContextInput::Layer)
                result = {{layer, layer}};
            else if (node.op == MaterialOp::Parameter &&
                     graph.parameters[node.declaration_index].type == MaterialParameterType::Uint)
                result = {{graph.parameters[node.declaration_index].uint_min,
                           graph.parameters[node.declaration_index].uint_max}};
            else if (node.op == MaterialOp::Select)
            {
                const auto condition = bool_domain(node.inputs[0]);
                if (condition & 2) result = uint_domain(node.inputs[1]);
                if (condition & 1)
                {
                    const auto branch = uint_domain(node.inputs[2]);
                    result.insert(result.end(), branch.begin(), branch.end());
                }
            }
            else if (node.op == MaterialOp::Convert)
            {
                if (graph.nodes[node.inputs[0]].type == MaterialType::Uint)
                    result = uint_domain(node.inputs[0]);
                else if (graph.nodes[node.inputs[0]].type == MaterialType::Bool)
                {
                    const auto source = bool_domain(node.inputs[0]);
                    if (source & 1) result.push_back({0, 0});
                    if (source & 2) result.push_back({1, 1});
                }
                else Require(false, "sample float-to-uint layer did not constant fold");
            }
            else if (node.op == MaterialOp::Clamp)
                result = binary(MaterialOp::Min, binary(MaterialOp::Max,
                    uint_domain(node.inputs[0]), uint_domain(node.inputs[1])), uint_domain(node.inputs[2]));
            else
            {
                Require(MaterialInputCount(node.op, node.type) >= 2,
                        "sample layer input cannot be proven within the declared texture array");
                result = binary(node.op, uint_domain(node.inputs[0]), uint_domain(node.inputs[1]));
            }
            uint_cache[index] = normalize(std::move(result));
            return *uint_cache[index];
        };
        for (const auto &node : graph.nodes)
            if (node.op == MaterialOp::SampleGrad || node.op == MaterialOp::SampleLevel ||
                node.op == MaterialOp::DecalFracture || node.op == MaterialOp::DecalOrganic)
            {
                const auto texture = graph.nodes[node.inputs[0]].declaration_index;
                for (const auto range : uint_domain(node.inputs[3]))
                    Require(range.hi < texture_layer_counts[texture],
                            "authored sample layer cannot be proven within declared texture array");
            }
    }
}

std::string EmitRock0MaterialHlsl(const MaterialGraph &graph)
{
    const auto optimized = Optimize(graph, ValidateMaterialGraph(graph, MaterialGraphKind::Rock0));
    return "// Generated from validated rock_0 MaterialIR.\n" +
        EmitLibraryHelpers(optimized, {graph.base_color_output, graph.roughness_output}) +
        EmitFunction(optimized, graph.base_color_output, MaterialType::Float3,
                     "HsRock0BaseColor(float3 sampled_base_color)", graph.parameters) +
        EmitFunction(optimized, graph.roughness_output, MaterialType::Float,
                     "HsRock0Roughness(float sampled_roughness, float roughness_scale)", graph.parameters);
}

std::string EmitLeaf0MaterialHlsl(const MaterialGraph &graph)
{
    const auto optimized = Optimize(graph, ValidateMaterialGraph(graph, MaterialGraphKind::Leaf0));
    return "// Generated from validated leaf_0 MaterialIR.\n" +
        EmitLibraryHelpers(optimized, {graph.opacity_output}) +
        EmitFunction(optimized, graph.opacity_output, MaterialType::Float,
                     "HsLeaf0Opacity(float sampled_alpha)", graph.parameters);
}

std::string EmitEnvironmentMaterialHlsl(const MaterialGraph &graph)
{
    if (graph.kind == MaterialGraphKind::Rock0)
        return EmitRock0MaterialHlsl(graph) +
            "float3 HsMaterialBaseColor(float3 sampled) { return HsRock0BaseColor(sampled); }\n"
            "float HsMaterialRoughness(float sampled, float scale) { return HsRock0Roughness(sampled, scale); }\n"
            "float HsMaterialOpacity(float sampled) { return sampled; }\n";
    if (graph.kind == MaterialGraphKind::Leaf0)
        return EmitLeaf0MaterialHlsl(graph) +
            "float3 HsMaterialBaseColor(float3 sampled) { return sampled; }\n"
            "float HsMaterialRoughness(float sampled, float scale) { return saturate(sampled * scale); }\n"
            "float HsMaterialOpacity(float sampled) { return HsLeaf0Opacity(sampled); }\n";
    ValidateEnvironmentMaterialGraph(graph);
    Require(graph.closure_output == kNoMaterialInput,
            "typed surface closure requires the context surface emitter");
    const auto optimized = Optimize(graph, ValidateMaterialGraph(graph, graph.kind));
    std::string result = "// Generated from validated environment MaterialIR.\n";
    result += EmitLibraryHelpers(optimized, {graph.base_color_output, graph.roughness_output,
                                             graph.kind == MaterialGraphKind::Masked
                                                 ? graph.opacity_output : kNoMaterialInput});
    result += EmitFunction(optimized, graph.base_color_output, MaterialType::Float3,
                           "HsMaterialBaseColor(float3 sampled)", graph.parameters);
    result += EmitFunction(optimized, graph.roughness_output, MaterialType::Float,
                           "HsMaterialRoughness(float sampled, float scale)", graph.parameters);
    if (graph.kind == MaterialGraphKind::Masked)
        result += EmitFunction(optimized, graph.opacity_output, MaterialType::Float,
                               "HsMaterialOpacity(float sampled)", graph.parameters);
    else result += "float HsMaterialOpacity(float sampled) { return sampled; }\n";
    return result;
}

std::string EmitEnvironmentMaterialContextHlsl(const MaterialGraph &graph,
                                              std::size_t material_ordinal)
{
    Require(material_ordinal < kAuthoredMaterialNames.size(), "material ordinal out of range");
    if (graph.kind == MaterialGraphKind::Terrain) ValidateTerrainMaterialGraph(graph);
    else ValidateEnvironmentMaterialGraph(graph);
    Require(graph.normal_space == MaterialNormalSpace::Tangent ||
            graph.normal_space == MaterialNormalSpace::World, "invalid normal output space");
    const auto optimized = Optimize(graph, ValidateMaterialGraph(graph, graph.kind));
    std::string result = "// Validated authored context MaterialIR; shader binding ABI 2.\n";
    for (std::size_t slot = 0; slot < graph.textures.size(); ++slot)
        result += "Texture2DArray<float4> HsMaterialTexture" + std::to_string(slot) +
            " : register(t" + std::to_string(material_ordinal * kMaterialTextureSlots + slot) + ", space4);\n";
    result += "#include \"material_surface.hlsli\"\n";
    result += "#include \"texture_streaming.hlsli\"\n";
    if (graph.kind == MaterialGraphKind::Terrain)
        result += "#include \"material_terrain.hlsli\"\n";
    result += EmitNormalHelpers();
    if (graph.closure_output != kNoMaterialInput)
        result += EmitLibraryHelpers(optimized, {graph.closure_output});
    else
        result += EmitLibraryHelpers(optimized, {graph.base_color_output, graph.roughness_output,
                                                 graph.normal_output, graph.ao_output,
                                                 graph.kind == MaterialGraphKind::Masked
                                                     ? graph.opacity_output : kNoMaterialInput});
    const auto representative = [&](std::size_t index) { return optimized.representative[index]; };
    const auto value = [&](std::size_t index) { return "n" + std::to_string(representative(index)); };
    std::vector<bool> surface_reachable(optimized.nodes.size());
    const auto surface_output = graph.closure_output != kNoMaterialInput
        ? graph.closure_output : graph.base_color_output;
    if (graph.closure_output != kNoMaterialInput)
    {
        const auto output_reachable = Reachable(optimized.nodes, representative(surface_output));
        for (std::size_t i = 0; i < surface_reachable.size(); ++i)
            surface_reachable[i] = output_reachable[i];
    }
    else for (const auto output : {graph.base_color_output, graph.roughness_output,
            graph.normal_output, graph.ao_output,
            graph.kind == MaterialGraphKind::Masked ? graph.opacity_output : kNoMaterialInput})
        if (output != kNoMaterialInput)
        {
            const auto output_reachable = Reachable(optimized.nodes, representative(output));
            for (std::size_t i = 0; i < surface_reachable.size(); ++i)
                surface_reachable[i] = surface_reachable[i] || output_reachable[i];
        }
    result += "HsMaterialSurface HsMaterialEvaluateSurface(HsMaterialContext ctx)\n{\n";
    result += EmitContextNodes(optimized, graph, surface_reachable, material_ordinal * kMaterialTextureSlots);
    if (graph.closure_output != kNoMaterialInput)
        result += "    HsMaterialSurface result = " + value(graph.closure_output) +
            ";\n    result.normal_is_world = " +
            (graph.normal_space == MaterialNormalSpace::World ? "true" : "false") +
            ";\n    return result;\n}\n";
    else
        result += "    HsMaterialSurface result = (HsMaterialSurface)0;\n    result.base_color = " + value(graph.base_color_output) +
            ";\n    result.roughness = " + value(graph.roughness_output) +
            ";\n    result.metallic = ctx.metallic;\n    result.normal = " +
            (graph.normal_output == kNoMaterialInput ? "ctx.normal" : value(graph.normal_output)) +
            ";\n    result.ao = " +
            (graph.ao_output == kNoMaterialInput ? "ctx.sampled_ao" : value(graph.ao_output)) +
            ";\n    result.opacity = " +
            (graph.kind == MaterialGraphKind::Masked ? value(graph.opacity_output) : "1.0") +
            ";\n    result.normal_is_world = " +
            (graph.normal_space == MaterialNormalSpace::World ? "true" : "false") +
            ";\n    return result;\n}\n";
    result += "float HsMaterialEvaluateOpacity(HsMaterialContext ctx)\n{\n";
    if (graph.closure_output != kNoMaterialInput && graph.kind == MaterialGraphKind::Masked)
    {
        const auto closure = optimized.representative[graph.closure_output];
        const auto opacity = ClosureOpacity(optimized.nodes, closure);
        result += EmitContextNodes(optimized, graph, Reachable(optimized.nodes, opacity), material_ordinal * kMaterialTextureSlots) +
            "    return n" + std::to_string(opacity) + ";\n";
    }
    else if (graph.kind == MaterialGraphKind::Masked)
        result += EmitContextNodes(optimized, graph,
            Reachable(optimized.nodes, representative(graph.opacity_output)), material_ordinal * kMaterialTextureSlots) +
            "    return " + value(graph.opacity_output) + ";\n";
    else result += "    return 1.0;\n";
    return result + "}\n";
}

std::string EmitHeroMaterialContextHlsl(const MaterialGraph &graph,
    std::string_view surface_function, std::string_view opacity_function)
{
    ValidateHeroMaterialGraph(graph);
    for (const auto name : {surface_function, opacity_function})
    {
        Require(!name.empty() && name.size() <= 128, "invalid hero material function name");
        for (std::size_t i = 0; i < name.size(); ++i)
        {
            const auto c = name[i];
            Require((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
                    (i != 0 && c >= '0' && c <= '9'), "invalid hero material function name");
        }
    }
    Require(surface_function != opacity_function, "hero material function names must differ");
    const auto optimized = Optimize(graph, ValidateMaterialGraph(graph, graph.kind));
    const auto closure = optimized.representative[graph.closure_output];
    const auto opacity = ClosureOpacity(optimized.nodes, closure);
    const auto surface_live = Reachable(optimized.nodes, closure);
    std::string result = "#include \"material_surface.hlsli\"\n";
    result += EmitLibraryHelpers(optimized, surface_live) + EmitNormalHelpers();
    result += "HsSurfaceClosure " + std::string(surface_function) + "(HsMaterialContext ctx)\n{\n";
    result += EmitContextNodes(optimized, graph, surface_live);
    result += "    HsSurfaceClosure result = n" + std::to_string(closure) + ";\n";
    result += "    result.normal_is_world = " +
        std::string(graph.normal_space == MaterialNormalSpace::World ? "true" : "false") + ";\n";
    result += "    return result;\n}\nfloat " + std::string(opacity_function) + "(HsMaterialContext ctx)\n{\n";
    result += EmitContextNodes(optimized, graph, Reachable(optimized.nodes, opacity));
    return result + "    return n" + std::to_string(opacity) + ";\n}\n";
}

std::string EmitDecalMaterialContextHlsl(const MaterialGraph &graph,
    std::string_view function_name, std::uint32_t texture_register)
{
    ValidateDecalMaterialGraph(graph);
    Require(!function_name.empty() && function_name.size() <= 128, "invalid decal function name");
    for (std::size_t i = 0; i < function_name.size(); ++i)
    {
        const char c = function_name[i];
        Require((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
                (i != 0 && c >= '0' && c <= '9'), "invalid decal function name");
    }
    const auto optimized = Optimize(graph, ValidateMaterialGraph(graph, MaterialGraphKind::Decal));
    const auto closure = optimized.representative[graph.closure_output];
    // The producer assigns a dedicated texture register and symbol to each
    // evaluator in its combined program. Decal ordinals never enter the
    // geometry material or texture ABI.
    std::string result = "Texture2DArray<float4> HsMaterialTexture0 : register(t" +
        std::to_string(texture_register) + ", space36);\n";
    result += EmitLibraryHelpers(optimized, Reachable(optimized.nodes, closure)) + EmitNormalHelpers();
    result += "HsSurfaceClosure " + std::string(function_name) + "(HsDecalContext ctx)\n{\n";
    result += EmitContextNodes(optimized, graph, Reachable(optimized.nodes, closure));
    result += "    HsSurfaceClosure result = n" + std::to_string(closure) + ";\n";
    return result + "    result.normal_is_world = true;\n    return result;\n}\n";
}
} // namespace hs::render_data
