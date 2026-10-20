#pragma once

#include <hs/render_data/material_schema.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace hs::render_data
{
enum class MaterialType { Float, Float3, Bool, Uint, Float2, Float4, TextureRef, SamplerRef,
                          TerrainResult, SurfaceClosure };
enum class MaterialGraphKind { Rock0, Leaf0, Opaque, Masked, Terrain, Translucent, HeroRefractive, Decal };
enum class MaterialNormalSpace { Tangent, World };
enum class MaterialOp
{
    SampledBaseColor,
    SampledRoughness,
    SampledAlpha,
    RoughnessScale,
    Parameter,
    Constant,
    Multiply,
    Saturate,
    Add,
    Subtract,
    Divide,
    Min,
    Max,
    Negate,
    Lerp,
    Clamp,
    Construct,
    Extract,
    Convert,
    Less,
    LessEqual,
    Equal,
    Select,
    LogicalAnd,
    LogicalOr,
    LogicalNot,
    TextureRef,
    SamplerRef,
    Context,
    SampleGrad,
    SampleLevel,
    DecodeNormalXY,
    NormalRnm,
    DetailFade,
    TangentToWorld,
    SeededNoise2D,
    FresnelSchlick,
    TerrainStochastic,
    TerrainField,
    MetallicRoughnessClosure,
    // Layer signatures, in input order (all return SurfaceClosure):
    // Coat(base, float weight, float roughness), inheriting the base normal;
    // Anisotropy(base, float amount, float3 tangent);
    // Sheen(base, float weight, float3 tint, float roughness);
    // ThinTransmission(base, float weight, float3 tint);
    // Emission(base, float3 emission); Unlit(float3 color, float opacity).
    CoatClosure,
    AnisotropyClosure,
    SheenClosure,
    ThinTransmissionClosure,
    EmissionClosure,
    UnlitClosure,
    // Uniform optical profiles: Subsurface(base, weight, radius_m RGB, thickness_m);
    // SolidTransmission(base, weight, ior, absorption_per_m RGB, thickness_m).
    // Solid transmission retains base_color as its authored transmission tint;
    // absorption_per_m independently describes bulk Beer attenuation.
    SubsurfaceClosure,
    SolidTransmissionClosure,
    // Projected decal functions return the modified receiver closure. Signature:
    // texture, sampler, uv, layer, dx, dy, receiver base/normal/roughness,
    // tint_hdr, projector(axis.xy, radius, alpha), shape(age, glow, cells), seed,
    // target roughness, roughness blend. Sampling uses explicit gradients.
    DecalFracture,
    DecalOrganic,
};
enum class MaterialContextInput
{
    Uv, DUvDx, DUvDy, Layer, ActualWorldPosition, DActualWorldDx, DActualWorldDy,
    StableWorldPosition, DStableWorldDx, DStableWorldDy, Normal, Tangent, EyeDistance,
    SampledBaseColor, SampledRoughness, SampledAlpha, RoughnessScale, SampledAo, ShadingNormal,
    Mesh, Seed, ViewDirection, Metallic, SampledEmission, SampledMetallic,
    DecalTintHdr, DecalProjector, DecalShape,
};
enum class TerrainField { BaseColor, Normal, Detail, Roughness, Ao };

// Arithmetic operands have exactly the result type: uint or float/float2/3/4.
// Saturate, Negate and Lerp accept floating types; Lerp's weight is the same type.
// Construct builds float2/3/4 from individual float inputs; Extract returns float.
// Comparisons return bool and compare two scalar float/uint values (Equal also
// accepts bool). Select takes bool, then two values of the result type.
// Convert is an identity or an explicit scalar bool/uint/float conversion.
// Float -> uint truncates toward zero and is allowed only for a constant-folded
// finite input in [0, 2^32). Uint -> float follows HLSL rounding, so may lose bits.
// Uint arithmetic wraps modulo 2^32; uint division needs a constant nonzero
// divisor. No scalar splats, vector truncation or implicit numeric casts occur.

// Explicit texture samples, gradient contexts and normal composition need more
// nodes than the legacy sampled-value graph. Keep a fixed bounded compiler input.
inline constexpr std::size_t kMaxMaterialGraphNodes = 128;
inline constexpr std::size_t kMaxMaterialInputs = 15;
inline constexpr std::size_t kNoMaterialInput = std::numeric_limits<std::size_t>::max();

[[nodiscard]] constexpr std::size_t MaterialComponentCount(MaterialType type)
{
    switch (type)
    {
    case MaterialType::Bool: case MaterialType::Uint: case MaterialType::Float: return 1;
    case MaterialType::Float2: return 2;
    case MaterialType::Float3: return 3;
    case MaterialType::Float4: return 4;
    default: return 0;
    }
}

[[nodiscard]] constexpr std::size_t MaterialInputCount(MaterialOp op, MaterialType type)
{
    switch (op)
    {
    case MaterialOp::Multiply: case MaterialOp::Add: case MaterialOp::Subtract:
    case MaterialOp::Divide: case MaterialOp::Min: case MaterialOp::Max:
    case MaterialOp::Less: case MaterialOp::LessEqual: case MaterialOp::Equal:
    case MaterialOp::LogicalAnd: case MaterialOp::LogicalOr: return 2;
    case MaterialOp::Saturate: case MaterialOp::Negate: case MaterialOp::Extract:
    case MaterialOp::Convert: case MaterialOp::LogicalNot: return 1;
    case MaterialOp::DecodeNormalXY: return 1;
    case MaterialOp::NormalRnm: case MaterialOp::DetailFade: return 2;
    case MaterialOp::TangentToWorld: return 3;
    case MaterialOp::SampleGrad: return 6;
    case MaterialOp::SampleLevel: return 5;
    case MaterialOp::SeededNoise2D: case MaterialOp::FresnelSchlick: return 2;
    case MaterialOp::TerrainStochastic: return 15;
    case MaterialOp::DecalFracture: case MaterialOp::DecalOrganic: return 15;
    case MaterialOp::TerrainField: return 1;
    case MaterialOp::MetallicRoughnessClosure: return 6;
    case MaterialOp::CoatClosure: case MaterialOp::AnisotropyClosure:
    case MaterialOp::ThinTransmissionClosure: return 3;
    case MaterialOp::SheenClosure: return 4;
    case MaterialOp::SubsurfaceClosure: return 4;
    case MaterialOp::SolidTransmissionClosure: return 5;
    case MaterialOp::EmissionClosure: case MaterialOp::UnlitClosure: return 2;
    case MaterialOp::Lerp: case MaterialOp::Clamp: case MaterialOp::Select: return 3;
    case MaterialOp::Construct: return MaterialComponentCount(type);
    case MaterialOp::Parameter: return 0;
    default: return 0;
    }
}

struct MaterialNode
{
    std::string id;
    MaterialOp op{};
    MaterialType type{};
    std::array<float, 4> value{};
    std::array<std::size_t, kMaxMaterialInputs> inputs{
        kNoMaterialInput, kNoMaterialInput, kNoMaterialInput, kNoMaterialInput,
        kNoMaterialInput, kNoMaterialInput, kNoMaterialInput, kNoMaterialInput,
        kNoMaterialInput, kNoMaterialInput, kNoMaterialInput, kNoMaterialInput,
        kNoMaterialInput, kNoMaterialInput, kNoMaterialInput};
    std::uint32_t uint_value{}; // uint constants never round through float
    bool bool_value{};
    std::uint32_t component{}; // Extract only
    std::size_t declaration_index{kNoMaterialInput}; // TextureRef / SamplerRef / Parameter
    MaterialContextInput context_input{}; // Context only
    TerrainField terrain_field{}; // TerrainField only
};

struct MaterialGraph
{
    MaterialGraphKind kind{MaterialGraphKind::Rock0};
    std::vector<MaterialNode> nodes; // topological order; dead nodes are validated, then pruned
    std::vector<MaterialParameterDefinition> parameters;
    std::size_t base_color_output{};
    std::size_t roughness_output{};
    std::size_t opacity_output{};
    std::size_t normal_output{kNoMaterialInput};
    std::size_t ao_output{kNoMaterialInput};
    std::size_t terrain_output{kNoMaterialInput};
    std::size_t closure_output{kNoMaterialInput};
    std::vector<MaterialTextureDefinition> textures;
    std::vector<MaterialSamplerDefinition> samplers;
    MaterialNormalSpace normal_space{MaterialNormalSpace::Tangent};
    DecalReceiverPolicy decal_receivers;
};

enum class MaterialClosureCapability : std::uint32_t
{
    Basic = 1, Coat = 2, Anisotropy = 4, Sheen = 8, ThinTransmission = 16,
    Emission = 32, Unlit = 64, Subsurface = 128, SolidTransmission = 256,
};
enum class MaterialClosureVarying : std::uint32_t
{
    // Tangent covers anisotropy amount or tangent; Emission also covers Unlit
    // color. Basic surface inputs and opacity stay in the raster domain.
    Tangent = 1, Coat = 2, Sheen = 4, Transmission = 8, Emission = 16,
};
struct ClosureReflection
{
    std::uint32_t capability_mask{};
    std::uint32_t varying_mask{};
};
// Reflect the live, optimized closure chain. Parameter defaults never determine
// uniformity: declared numeric parameters and pure operations on them are uniform.
// Scattering layers have one field set each: duplicate kinds and scattering on
// Unlit are invalid. Emission may stack additively on Basic or Unlit.
[[nodiscard]] ClosureReflection AnalyzeClosure(const MaterialGraph &graph);
// Reapply advanced layers to a provided basic closure in lighting. Whole families
// with varying inputs are omitted here and supplied by surface sidecars.
// Mixed uniform/varying operands
// in a selected family are materialized together by the authored surface evaluator.
// The generated signature is HsSurfaceClosure name(HsSurfaceClosure base,
// HsMaterialContext ctx); texture sampling and basic surface evaluation are omitted.
// Unlit color is reevaluated to preserve HDR; opacity is inherited from base.
// The current closure field set requires scattering order Anisotropy, then
// one ThinTransmission/Subsurface/SolidTransmission profile, Sheen, Coat from
// the terminal outward; emission is additive
// and does not affect this order. Unsupported graph orders are rejected.
// Tangent-space anisotropy requires per-pixel conversion of the authored tangent
// to world space by the surface producer, so it always requires a Tangent sidecar.
// With an Emission sidecar, Unlit keeps its flag here and its final radiance is
// supplied by that sidecar; uniform emission wrappers are omitted as well.
// Subsurface/SolidTransmission optical inputs are uniform in this renderer profile;
// unsupported varying inputs fail validation. Thin, subsurface and solid transport
// cannot share a chain. SolidTransmission is restricted to transparent domains.
[[nodiscard]] std::string EmitUniformClosureHlsl(const MaterialGraph &graph,
    std::string_view function_name = "HsMaterialApplyUniformLayers");

// Validate the typed graph independently of its JSON source.
void ValidateRock0MaterialGraph(const MaterialGraph &graph);
void ValidateLeaf0MaterialGraph(const MaterialGraph &graph);
void ValidateEnvironmentMaterialGraph(const MaterialGraph &graph);
void ValidateHeroMaterialGraph(const MaterialGraph &graph);
void ValidateTerrainMaterialGraph(const MaterialGraph &graph);
void ValidateDecalMaterialGraph(const MaterialGraph &graph);
// Emits one typed evaluator and its declared texture at t<texture_register>/space36.
// HsDecalContext is a decal-only extension; shared geometry context is unchanged.
[[nodiscard]] std::string EmitDecalMaterialContextHlsl(const MaterialGraph &graph,
    std::string_view function_name, std::uint32_t texture_register);
// Proves every authored sample layer is in its declared texture array. Context
// layer is enumerated; unknown bool conditions conservatively include both branches.
void ValidateMaterialSampleLayers(const MaterialGraph &graph,
    std::span<const std::uint32_t> texture_layer_counts, std::uint32_t max_context_layer);
[[nodiscard]] std::string EmitRock0MaterialHlsl(const MaterialGraph &graph);
[[nodiscard]] std::string EmitLeaf0MaterialHlsl(const MaterialGraph &graph);
[[nodiscard]] std::string EmitEnvironmentMaterialHlsl(const MaterialGraph &graph);
// Context evaluator owns authored sampling; legacy numeric emitters above retain
// their sampled-value signatures. The texture ordinal is the shared material ID order.
[[nodiscard]] std::string EmitEnvironmentMaterialContextHlsl(
    const MaterialGraph &graph, std::size_t material_ordinal);
// Shared hero adapter supplies sampled textures/expression/charge in ctx. No
// texture declarations are emitted; local node names can be concatenated safely.
[[nodiscard]] std::string EmitHeroMaterialContextHlsl(const MaterialGraph &graph,
    std::string_view surface_function, std::string_view opacity_function);
} // namespace hs::render_data
