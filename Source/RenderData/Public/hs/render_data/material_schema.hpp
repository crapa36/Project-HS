#pragma once

#include <hs/render_data/render_asset_format.hpp>
#include <hs/core/render_snapshot.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hs::render_data
{
enum class MaterialParameterType : std::uint8_t
{
    Bool = 1,
    Uint = 2,
    Float = 3,
    Float2 = 4,
    Float3 = 5,
    Float4 = 6,
};

struct MaterialParameterDefinition
{
    std::string id;
    MaterialParameterType type{};
    std::array<std::uint32_t, 4> default_words{};
    std::uint32_t uint_min{};
    std::uint32_t uint_max{std::numeric_limits<std::uint32_t>::max()};

    friend bool operator==(const MaterialParameterDefinition &,
                           const MaterialParameterDefinition &) = default;
};

struct MaterialParameterSlot
{
    std::array<std::uint32_t, 4> words{};

    friend bool operator==(const MaterialParameterSlot &, const MaterialParameterSlot &) = default;
};
static_assert(sizeof(MaterialParameterSlot) == 16);

// Reflected live closure requirements, independent of mutable parameter defaults.
// The five sidecar families preserve tangent/coat/sheen/thin/emission. Optical
// SSS/solid profiles are currently uniform; consumers must reject unavailable domains.
struct MaterialSurfaceLayout
{
    std::uint32_t capability_mask{};
    std::uint32_t varying_mask{};
    friend bool operator==(const MaterialSurfaceLayout &, const MaterialSurfaceLayout &) = default;
};
[[nodiscard]] std::vector<std::byte> EncodeMaterialSurfaceLayout(MaterialSurfaceLayout layout);
[[nodiscard]] MaterialSurfaceLayout DecodeMaterialSurfaceLayout(std::span<const std::byte> bytes);
void ValidateUniformMaterialSurfaceLayout(MaterialSurfaceLayout layout);

void ValidateMaterialParameters(std::span<const MaterialParameterDefinition> parameters);
[[nodiscard]] std::string MaterialParameterLayoutHash(
    std::span<const MaterialParameterDefinition> parameters);
[[nodiscard]] std::vector<std::byte> EncodeMaterialParameterLayout(
    std::span<const MaterialParameterDefinition> parameters);
[[nodiscard]] std::string DecodeMaterialParameterLayout(std::span<const std::byte> encoded_hash);
void ValidateMaterialParameterLayout(
    std::span<const std::byte> encoded_hash,
    std::span<const MaterialParameterDefinition> parameters);
[[nodiscard]] std::vector<std::byte> EncodeMaterialParameters(
    std::span<const MaterialParameterDefinition> parameters);
[[nodiscard]] std::vector<MaterialParameterDefinition> DecodeMaterialParameters(
    std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> BuildMaterialParameterPackage(
    std::string_view product_id,
    std::span<const MaterialParameterDefinition> parameters);
[[nodiscard]] std::vector<MaterialParameterDefinition> ReadMaterialParameterPackage(
    std::span<const std::byte> bytes, AssetId expected_asset_id);

// Geometry catalog order; the appended detail geometries share one authored material.
inline constexpr std::array<std::string_view, 35> kEnvironmentMaterialNames{
    "trunk_0", "trunk_1", "trunk_2", "leaf_0", "leaf_1", "leaf_2",
    "rock_0", "rock_1", "rock_2", "rock_3", "grass_0", "grass_1", "grass_2", "grass_3",
    "flower_daisy", "flower_buttercup", "flower_violet", "clover_patch",
    "ground_leaves", "twig_cluster", "fence_short",
    "shrub_low", "stump_roots", "crate_intact", "crate_broken", "barrel_intact", "barrel_broken", "cart_broken", "fence_broken", "fence_splitrail", "sack_tied", "fallen_log", "rock_mossy", "meadow_ground", "meadow_water"};
inline constexpr std::array<std::string_view, 16> kAuthoredMaterialNames{
    "trunk_0", "trunk_1", "trunk_2", "leaf_0", "leaf_1", "leaf_2",
    "rock_0", "rock_1", "rock_2", "rock_3", "grass_0", "grass_1", "grass_2", "grass_3", "terrain", "ground_detail"};
inline constexpr std::uint32_t kEnvironmentGeometryLodCount = 3;
inline constexpr std::uint32_t kTerrainMaterialOrdinal = 14;
inline constexpr std::uint32_t kGroundDetailMaterialOrdinal = 15;
inline constexpr std::uint32_t kGroundDetailGeometryBegin = 14;
inline constexpr std::uint32_t kMeadowGeometryBegin = 33;
inline constexpr std::uint32_t kMeadowWaterGeometry = 34;
[[nodiscard]] constexpr bool IsEnvironmentMesh(RenderMesh mesh) noexcept
{ return (mesh >= RenderMesh::TreeTrunk && mesh <= RenderMesh::Grass) || mesh == RenderMesh::GroundDetail || mesh == RenderMesh::MeadowSurface; }
[[nodiscard]] constexpr std::uint32_t EnvironmentGeometryOrdinal(RenderMesh mesh, std::uint32_t variant) noexcept
{
    switch (mesh)
    {
    case RenderMesh::TreeTrunk: return variant % 3u;
    case RenderMesh::TreeCanopy: return 3u + variant % 3u;
    case RenderMesh::Rock: return 6u + variant % 4u;
    case RenderMesh::Grass: return 10u + variant % 4u;
    case RenderMesh::GroundDetail: return kGroundDetailGeometryBegin + variant % kGroundDetailVariantCount;
    case RenderMesh::MeadowSurface: return kMeadowGeometryBegin + variant % 2u;
    default: return UINT32_MAX;
    }
}
[[nodiscard]] constexpr RenderMesh EnvironmentMeshForOrdinal(std::uint32_t ordinal) noexcept
{
    return ordinal < 3u ? RenderMesh::TreeTrunk : ordinal < 6u ? RenderMesh::TreeCanopy :
        ordinal < 10u ? RenderMesh::Rock : ordinal < kGroundDetailGeometryBegin ? RenderMesh::Grass : ordinal < kMeadowGeometryBegin ? RenderMesh::GroundDetail : RenderMesh::MeadowSurface;
}
[[nodiscard]] constexpr std::uint32_t EnvironmentGeometryMaterialOrdinal(std::size_t ordinal) noexcept
{ return ordinal < kGroundDetailGeometryBegin ? static_cast<std::uint32_t>(ordinal) : ordinal == kMeadowGeometryBegin ? kTerrainMaterialOrdinal : kGroundDetailMaterialOrdinal; }

// Separate namespace/ordinals; the environment/terrain ABI above remains fixed.
inline constexpr std::array<std::string_view, 7> kHeroMaterialNames{
    "archer_skin", "archer_cloth", "archer_cutout", "archer_equipment",
    "slime_features", "slime_body", "monster_pbr"};
inline constexpr std::uint32_t kHeroMaterialIdBase = 0x20000000u;
inline constexpr std::array<std::string_view, 2> kDecalMaterialNames{
    "decal_fracture", "decal_organic"};
inline constexpr std::uint32_t kDecalShaderBindingAbi = 1;
enum class DecalReceiverNormalFilter : std::uint32_t { Any = 0, GroundFacing = 1 };
struct DecalReceiverPolicy
{
    // Bit N selects SurfaceLayout's model N: Cel=1, Environment=2, Jelly=3.
    std::uint32_t model_mask{1u << 2u};
    std::uint32_t required_surface_flags{1u};
    std::uint32_t forbidden_surface_flags{(3u << 8u) | (1u << 10u)};
    DecalReceiverNormalFilter normal_filter{DecalReceiverNormalFilter::GroundFacing};
    friend bool operator==(const DecalReceiverPolicy &, const DecalReceiverPolicy &) = default;
};
void ValidateDecalReceiverPolicy(const DecalReceiverPolicy &policy);
enum class HeroMaterialDomain : std::uint8_t { Opaque, Translucent };
struct CharacterMaterialBinding
{
    std::uint16_t slot{};
    std::uint16_t dependency_index{};
    std::uint32_t hero_ordinal{};
    HeroMaterialDomain domain{HeroMaterialDomain::Opaque};
    friend bool operator==(const CharacterMaterialBinding &, const CharacterMaterialBinding &) = default;
};
// Chunk25 v1: LE uint16 slot/dependency, uint32 packed domain/ordinal.
// Bit31 is Translucent; bits0..15 are ordinal; bits16..30 must be zero.
[[nodiscard]] std::uint32_t PackHeroMaterialDomainOrdinal(HeroMaterialDomain domain, std::uint32_t ordinal);
[[nodiscard]] HeroMaterialDomain UnpackHeroMaterialDomain(std::uint32_t packed);
[[nodiscard]] std::uint32_t UnpackHeroMaterialOrdinal(std::uint32_t packed);
[[nodiscard]] std::vector<std::byte> EncodeCharacterMaterialBindings(
    std::span<const CharacterMaterialBinding> bindings, std::span<const std::string> dependencies,
    std::uint32_t material_count);
[[nodiscard]] std::vector<CharacterMaterialBinding> DecodeCharacterMaterialBindings(
    std::span<const std::byte> bytes, std::span<const std::string> dependencies,
    std::uint32_t material_count);
inline constexpr std::size_t kMaterialTextureSlots = 6;
inline constexpr std::uint32_t kMaterialShaderBindingAbi = 2;
enum class MaterialTextureDimension : std::uint16_t { Texture2DArray = 1 };
enum class MaterialTextureSemantic : std::uint16_t
{ BaseColor = 1, Surface = 2, Normal = 3, Detail = 4, Roughness = 5,
  Gaussian = 6, Height = 7, Histogram = 8, DecalStructure = 9, DecalMask = 10 };
enum class MaterialSamplerProfile : std::uint16_t
{ LinearClamp = 1, LinearWrap = 2, EnvironmentAnisotropicWrap = 3, FoliageAnisotropicClamp = 4 };
struct MaterialTextureDefinition
{
    std::string id;
    std::string product_id;
    MaterialTextureDimension dimension{MaterialTextureDimension::Texture2DArray};
    MaterialTextureSemantic semantic{MaterialTextureSemantic::BaseColor};
};
struct MaterialSamplerDefinition
{
    std::string id;
    MaterialSamplerProfile profile{MaterialSamplerProfile::LinearClamp};
};
struct MaterialTextureBinding
{
    std::uint16_t slot{};
    std::uint16_t dependency_index{};
    MaterialTextureDimension dimension{MaterialTextureDimension::Texture2DArray};
    MaterialTextureSemantic semantic{MaterialTextureSemantic::BaseColor};
    std::uint32_t shader_binding_abi{kMaterialShaderBindingAbi};
};
[[nodiscard]] std::uint32_t MaterialSamplerRegister(MaterialSamplerProfile profile);
void ValidateMaterialTextureDefinitions(std::span<const MaterialTextureDefinition> textures);
void ValidateDecalMaterialTextureDefinition(const MaterialTextureDefinition &texture);
// Validates the semantic's format/channel contract and available array layer/mips.
void ValidateMaterialTexturePackage(const MaterialTextureDefinition &texture,
    const struct TextureDescription &description, std::uint32_t required_layer);
[[nodiscard]] std::vector<std::byte> EncodeMaterialBindings(
    std::span<const std::string> dependencies,
    std::span<const MaterialTextureDefinition> textures);
[[nodiscard]] std::vector<MaterialTextureBinding> DecodeMaterialBindings(
    std::span<const std::byte> bytes, std::span<const std::string> dependencies);
// First executable environment material record; shader selection is still owned
// by the renderer. Empty detail means the material has no detail texture.
struct RockMaterial
{
    std::array<std::string, 6> textures; // base color, surface, normal, detail, height, histogram
    float metallic{};
    float roughness_scale{1.0f};
    bool two_sided{};
    bool alpha_mask{};
};

enum class EnvironmentMaterialVariant
{
    OpaqueSingleSided,
    MaskedTwoSided,
};

enum class MaterialProgramKind : std::uint16_t
{
    OpaqueScenePS = 1,
    MaskedScenePS = 2,
    MaskedShadowPS = 3,
    HeroScenePS = 4,
    HeroVisibilityPS = 5,
    HeroSurfaceResolveCS = 6,
    HeroShadowPS = 7,
    HeroTranslucentPS = 8,
    DecalSurfaceCS = 9,
};

struct MaterialProgram
{
    std::uint16_t dependency_index{};
    MaterialProgramKind kind{MaterialProgramKind::OpaqueScenePS};
};

struct MaterialProgramSource
{
    std::string_view shader_id;
    MaterialProgramKind kind{};
};

inline constexpr std::string_view kRock0ShaderProduct =
    "render/shader/rock_0_scene.dxil";
inline constexpr std::string_view kLeaf0SceneShaderProduct =
    "render/shader/leaf_0_scene.dxil";
inline constexpr std::string_view kLeaf0ShadowShaderProduct =
    "render/shader/leaf_0_shadow.dxil";

[[nodiscard]] std::vector<std::byte> EncodeMaterialPrograms(
    std::span<const std::string> dependencies,
    std::span<const MaterialProgramSource> programs);
[[nodiscard]] std::vector<MaterialProgram> DecodeMaterialPrograms(
    std::span<const std::byte> bytes, std::span<const std::string> dependencies);

struct HeroMaterialPackageData
{
    std::vector<std::string> dependencies;
    std::vector<MaterialProgram> programs;
    MaterialSurfaceLayout surface;
    std::string parameter_layout_hash;
    HeroMaterialDomain domain{};
};
[[nodiscard]] std::vector<std::byte> BuildHeroMaterialPackage(std::string_view name,
    std::span<const MaterialParameterDefinition> parameters, MaterialSurfaceLayout surface,
    std::span<const MaterialProgramSource> programs);
[[nodiscard]] HeroMaterialPackageData ReadHeroMaterialPackage(
    std::span<const std::byte> bytes, std::string_view name);

struct DecalMaterialPackageData
{
    std::vector<std::string> dependencies;
    MaterialProgram program;
    std::uint16_t texture_dependency_index{};
    std::string parameter_layout_hash;
    DecalReceiverPolicy receivers;
};
[[nodiscard]] std::vector<std::byte> BuildDecalMaterialPackage(std::string_view name,
    std::span<const MaterialParameterDefinition> parameters, std::string_view texture_id,
    std::string_view shader_id, const DecalReceiverPolicy &receivers);
[[nodiscard]] DecalMaterialPackageData ReadDecalMaterialPackage(
    std::span<const std::byte> bytes, std::string_view name);

[[nodiscard]] std::vector<std::byte> EncodeMaterialProgram(
    std::span<const std::string> dependencies, std::string_view shader_id);
[[nodiscard]] MaterialProgram DecodeMaterialProgram(
    std::span<const std::byte> bytes, std::span<const std::string> dependencies);

[[nodiscard]] EnvironmentMaterialVariant ClassifyEnvironmentMaterial(
    const RockMaterial &material);

void ValidateRockMaterial(const RockMaterial &material);
[[nodiscard]] std::vector<std::byte> EncodeRockMaterial(
    const RockMaterial &material, std::span<const std::string> dependencies);
[[nodiscard]] RockMaterial DecodeRockMaterial(
    std::span<const std::byte> bytes, std::span<const std::string> dependencies);
} // namespace hs::render_data
