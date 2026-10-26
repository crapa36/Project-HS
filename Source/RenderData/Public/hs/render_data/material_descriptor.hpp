#pragma once

#include <hs/render_data/material_schema.hpp>
#include <hs/render_data/texture_descriptor.hpp>
#include <deque>

namespace hs::render_data
{
enum class MaterialKind : std::uint32_t { Environment, Hero, Decal };
struct MaterialHandle
{
    std::uint32_t value{std::numeric_limits<std::uint32_t>::max()};
    explicit operator bool() const noexcept { return value != std::numeric_limits<std::uint32_t>::max(); }
};
inline constexpr std::uint32_t kMaterialFlagTwoSided = 1, kMaterialFlagAlphaMask = 2;
struct MaterialProgramDescriptor
{
    MaterialProgramKind kind{};
    std::uint32_t bulk_index{};
    AssetId asset_id;
    std::string_view product_id; // Canonical capture/reload identity; execution also has a dense bulk index.
};
struct MaterialTextureDescriptor
{
    std::uint16_t slot{};
    TextureHandle texture;
    MaterialTextureDimension dimension{MaterialTextureDimension::Texture2DArray};
    MaterialTextureSemantic semantic{MaterialTextureSemantic::BaseColor};
    std::uint32_t shader_binding_abi{kMaterialShaderBindingAbi};
    std::string_view product_id;
};
struct MaterialDescriptor
{
    AssetId asset_id;
    std::uint32_t bulk_index{};
    MaterialKind kind{};
    MaterialSurfaceLayout surface;
    std::array<std::byte, 32> parameter_layout;
    std::span<const MaterialParameterSlot> defaults;
    std::span<const MaterialProgramDescriptor> programs;
    // Declared bindings only; fixed Hero adapter textures retain their native owner.
    std::span<const MaterialTextureDescriptor> textures;
    float metallic{}, roughness_scale{1.0f};
    std::uint32_t flags{};
    HeroMaterialDomain domain{HeroMaterialDomain::Opaque};
    DecalReceiverPolicy receivers;
};
struct MaterialDescriptorStorage
{
    MaterialDescriptor value;
    std::vector<MaterialParameterSlot> defaults;
    std::vector<MaterialProgramDescriptor> programs;
    std::vector<MaterialTextureDescriptor> textures;
    std::deque<std::string> product_ids;
    MaterialDescriptorStorage() = default;
    MaterialDescriptorStorage(const MaterialDescriptorStorage &) = delete;
    MaterialDescriptorStorage &operator=(const MaterialDescriptorStorage &) = delete;
    MaterialDescriptorStorage(MaterialDescriptorStorage &&) noexcept = default;
    MaterialDescriptorStorage &operator=(MaterialDescriptorStorage &&) noexcept = default;
    [[nodiscard]] MaterialDescriptor View() const noexcept
    {
        auto result = value;
        result.defaults = defaults; result.programs = programs; result.textures = textures;
        return result;
    }
};
[[nodiscard]] bool IsMaterialDescriptorProduct(std::string_view product_id) noexcept;
[[nodiscard]] std::string MaterialParametersProduct(std::string_view material_product);
[[nodiscard]] MaterialDescriptorStorage CompileMaterialDescriptor(
    std::string_view product_id, std::span<const std::byte> material_bytes,
    std::span<const std::byte> parameter_bytes, std::uint32_t bulk_index,
    std::span<const AssetId> bulk_assets, std::span<const TextureDescriptor> textures);
void ValidateMaterialDescriptor(const MaterialDescriptor &, std::span<const AssetId> bulk_assets,
    std::span<const TextureDescriptor> textures);
} // namespace hs::render_data
