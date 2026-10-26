#pragma once

#include <hs/render_data/character_package.hpp>

namespace hs::render_data
{
inline constexpr std::uint32_t kCharacterAnimationMetadataStride = 32;
inline constexpr std::uint32_t kCharacterNamedClipStride = 32;
inline constexpr std::uint32_t kCharacterAnimationSocketStride = 52;

struct NamedCharacterClip
{
    AssetId id{};
    std::string name;
    std::uint32_t first_transform{};
    std::uint32_t frame_count{};
    float duration_seconds{};
    bool looping{};
};

struct CharacterAnimationSocket
{
    std::string name;
    std::uint32_t bone_index{};
    CharacterLocalTransform local_transform{};
};

// A separate HSRPKG product; the legacy CharacterAnimationClip slots do not change.
// Skeleton order, names, parents and inverse binds belong to the authored source.
// Samples span [0,duration] uniformly, as in the legacy character cooker. The
// nominal source rate is 60 Hz for FBX cooking; zero means the interchange did
// not declare a rate. Duration/frame_count remain authoritative for playback.
struct CharacterAnimationExtensionData
{
    AssetId character_asset_id{};
    std::uint32_t sample_rate_hz{};
    std::vector<std::string> bone_names;
    std::vector<std::uint16_t> parents;
    std::vector<std::array<float, 16>> inverse_bind_matrices;
    std::vector<NamedCharacterClip> clips;
    std::vector<CharacterLocalTransform> transforms;
    std::vector<CharacterAnimationSocket> sockets;
};

// v1 chunks encode individual little-endian scalars, never C++ object bytes.
// Names starts with bone_count pairs of uint32 offset/length. Packed UTF-8 names
// then follow in bone, clip and socket order. Socket transforms use TRS floats.
[[nodiscard]] std::vector<std::byte> BuildCharacterAnimationExtension(
    AssetId asset_id, std::span<const std::string> dependencies,
    const CharacterAnimationExtensionData &animation);
[[nodiscard]] CharacterAnimationExtensionData DecodeCharacterAnimationExtension(
    std::span<const std::byte> bytes, AssetId expected_asset_id);
[[nodiscard]] CharacterAnimationExtensionData DecodeCharacterAnimationExtension(const PackageView &package);

// Exact ordered parent/inverse-bind comparison prevents a named clip product
// from being played on a different skeleton with the same number of bones.
void ValidateCharacterAnimationBinding(
    const CharacterAnimationExtensionData &animation,
    const CharacterMetadataPackageData &character, AssetId expected_character_asset_id);
void ValidateCharacterAnimationBinding(
    const CharacterAnimationExtensionData &animation,
    const CharacterPackageData &character, AssetId expected_character_asset_id);
} // namespace hs::render_data
