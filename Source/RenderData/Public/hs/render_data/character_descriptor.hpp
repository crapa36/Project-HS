#pragma once

#include <hs/render_data/character_animation_extension.hpp>
#include <hs/render_data/geometry_descriptor.hpp>
#include <hs/render_data/material_descriptor.hpp>
#include <hs/render_data/render_bulk.hpp>
#include <bit>
#include <limits>
#include <type_traits>

namespace hs::render_data
{
struct CharacterHandle
{
    std::uint32_t value{UINT32_MAX};
    explicit operator bool() const noexcept { return value != UINT32_MAX; }
};

// The existing little-endian transform chunk stores ten floats, without Core's padding.
struct PackedCharacterTransform
{
    std::array<float, 3> translation;
    std::array<float, 4> rotation;
    std::array<float, 3> scale;
    [[nodiscard]] CharacterLocalTransform Value() const noexcept
    {
        CharacterLocalTransform result;
        result.translation = translation; result.rotation = rotation; result.scale = scale;
        return result;
    }
};
static_assert(std::endian::native == std::endian::little && sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(std::is_trivially_copyable_v<PackedCharacterTransform> && std::is_standard_layout_v<PackedCharacterTransform>);
static_assert(sizeof(std::array<float, 16>) == 64 && std::is_trivially_copyable_v<std::array<float, 16>>);
static_assert(offsetof(PackedCharacterTransform, translation) == 0);
static_assert(sizeof(PackedCharacterTransform) == 40 &&
    offsetof(PackedCharacterTransform, rotation) == 12 && offsetof(PackedCharacterTransform, scale) == 28);

struct CharacterDescriptor
{
    AssetId asset_id;
    std::uint32_t bulk_index{};
    GeometryHandle geometry_page;
    GeometryMetadata geometry;
    std::span<const AssetId> dependencies;
    std::span<const CharacterMaterialBinding> material_bindings;
    std::span<const MaterialHandle> materials;
    std::uint32_t bone_count{}, material_count{};
    float ground_offset{};
    std::span<const CharacterClipHeader> clips;
    std::span<const std::uint16_t> parents;
    std::span<const std::array<float, 16>> inverse_bind_matrices;
    std::span<const float> upper_body_weights;
    BulkRange transforms;
    std::uint32_t deformation_abi{}, deformation_flags{};
    std::array<std::byte, 32> skeleton_binding{};
    float numerical_margin{}, bubble_amplitude{};
    std::span<const GeometryMetadata> lods;
    std::span<const CharacterIntervalBound> interval_bounds;
    BulkRange final_matrices;
};

struct AnimationClipDescriptor
{
    AssetId id;
    std::string_view name;
    std::uint32_t first_transform{}, frame_count{};
    float duration_seconds{};
    bool looping{};
};
struct AnimationSocketDescriptor
{
    std::string_view name;
    std::uint32_t bone_index{};
    CharacterLocalTransform local_transform;
};
struct AnimationDescriptor
{
    AssetId asset_id;
    std::uint32_t bulk_index{};
    CharacterHandle character;
    AssetId character_asset_id;
    std::uint32_t sample_rate_hz{};
    std::span<const std::string_view> bone_names;
    std::span<const std::uint16_t> parents;
    std::span<const std::array<float, 16>> inverse_bind_matrices;
    std::span<const AnimationClipDescriptor> clips;
    std::span<const AnimationSocketDescriptor> sockets;
    BulkRange transforms;
};

struct CharacterDescriptorStorage
{
    CharacterDescriptor value;
    CharacterMetadataPackageData data;
    std::vector<AssetId> dependencies;
    std::vector<MaterialHandle> materials;
    [[nodiscard]] CharacterDescriptor View() const noexcept;
};
struct AnimationDescriptorStorage
{
    AnimationDescriptor value;
    CharacterAnimationExtensionData data;
    std::vector<std::string_view> bone_names;
    std::vector<AnimationClipDescriptor> clips;
    std::vector<AnimationSocketDescriptor> sockets;
    AnimationDescriptorStorage() = default;
    AnimationDescriptorStorage(const AnimationDescriptorStorage &) = delete;
    AnimationDescriptorStorage &operator=(const AnimationDescriptorStorage &) = delete;
    AnimationDescriptorStorage(AnimationDescriptorStorage &&) noexcept = default;
    AnimationDescriptorStorage &operator=(AnimationDescriptorStorage &&) noexcept = default;
    [[nodiscard]] AnimationDescriptor View() const noexcept;
};

// Prepared data has already passed its package and geometry semantic boundary.
[[nodiscard]] CharacterDescriptorStorage CompileCharacterDescriptor(
    std::span<const std::byte> bytes, const PackageView &package,
    CharacterMetadataPackageData prepared, std::uint32_t bulk_index,
    std::span<const RendererBulkDescriptor> bulks, std::span<const GeometryDescriptor> geometry,
    std::span<const MaterialDescriptor> materials);
[[nodiscard]] AnimationDescriptorStorage CompileAnimationDescriptor(
    std::span<const std::byte> bytes, const PackageView &package,
    CharacterAnimationExtensionData prepared, std::uint32_t bulk_index,
    std::span<const RendererBulkDescriptor> bulks, CharacterHandle character,
    const CharacterDescriptorStorage &character_storage);
void ValidateCharacterDescriptor(const CharacterDescriptor &, std::span<const RendererBulkDescriptor>,
    std::span<const GeometryDescriptor>, std::span<const MaterialDescriptor>);
void ValidateAnimationDescriptor(const AnimationDescriptor &, std::span<const RendererBulkDescriptor>,
    std::span<const CharacterDescriptor>);
} // namespace hs::render_data
