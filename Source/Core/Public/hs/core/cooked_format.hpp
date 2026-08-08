#pragma once

#include <hs/core/result.hpp>
#include <hs/core/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hs
{

inline constexpr std::uint16_t kCookedFormatVersion = 1;
inline constexpr std::uint16_t kLittleEndianMarker = 0x4C45;
inline constexpr std::uint32_t kCharacterAssetVersion = 4;
inline constexpr std::uint32_t kMaxCharacterBones = 128;
inline constexpr std::uint32_t kMaxCharacterMaterials = 8;

enum class CharacterAnimationClip : std::uint8_t
{
    Idle,
    Run,
    Draw,
    Recoil,
    Death,
    TurnLeft,
    TurnRight,
    Count,
};

struct SkinnedVertex
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<std::uint16_t, 4> bone_indices{};
    std::array<float, 4> bone_weights{};
    std::array<float, 2> uv{};
    std::array<float, 4> tangent{};
    std::uint16_t material_index{};
    std::uint16_t reserved{};
};

static_assert(sizeof(SkinnedVertex) == 76);

struct CharacterClipHeader
{
    CharacterAnimationClip clip{};
    std::uint8_t looping{};
    std::uint16_t reserved{};
    std::uint32_t first_matrix{};
    std::uint32_t frame_count{};
    float duration_seconds{};
};

static_assert(sizeof(CharacterClipHeader) == 16);

struct CharacterAssetHeader
{
    std::array<char, 8> magic{'H', 'S', 'C', 'H', 'A', 'R', '1', '\0'};
    std::uint32_t version{kCharacterAssetVersion};
    std::uint32_t vertex_count{};
    std::uint32_t bone_count{};
    std::uint32_t clip_count{};
    std::uint32_t material_count{};
    std::uint32_t vertices_offset{sizeof(CharacterAssetHeader)};
    std::uint32_t clips_offset{};
    std::uint32_t upper_body_weights_offset{};
    std::uint32_t matrices_offset{};
    std::uint32_t payload_size{};
    std::uint32_t payload_crc32{};
    std::array<float, 3> bounds_min{};
    std::array<float, 3> bounds_max{};
};

static_assert(sizeof(CharacterAssetHeader) == 76);

struct CookedHeader
{
    std::array<char, 4> magic{'H', 'S', 'B', 'N'};
    std::uint16_t format_version{kCookedFormatVersion};
    std::uint16_t endian_marker{kLittleEndianMarker};
    std::uint64_t schema_hash{};
    std::uint64_t source_hash{};
    std::uint32_t table_offset{sizeof(CookedHeader)};
    std::uint32_t table_count{};
    std::uint32_t payload_size{};
    std::uint32_t payload_crc32{};
};

static_assert(sizeof(CookedHeader) == 40);

[[nodiscard]] std::uint64_t Fnv1a64(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] std::uint64_t Fnv1a64(std::string_view text) noexcept;
[[nodiscard]] std::uint32_t Crc32(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] Result NormalizeAssetPath(std::string_view source, std::string &normalized);
[[nodiscard]] AssetId MakeAssetId(std::string_view normalized) noexcept;
[[nodiscard]] Result ReadCookedPayload(const std::filesystem::path &path,
                                       std::uint64_t expected_schema_hash,
                                       CookedHeader &header,
                                       std::vector<std::byte> &payload);

} // namespace hs
