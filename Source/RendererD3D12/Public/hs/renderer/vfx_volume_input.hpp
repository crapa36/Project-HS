#pragma once

#include <hs/core/types.hpp>
#include <array>
#include <cstdint>
#include <span>
#include <type_traits>

namespace hs
{
enum class VfxVolumeShape : std::uint32_t { NoisySphere, BoundedCrown };
enum class VfxVolumeNoise : std::uint32_t { None, Fbm3D };
struct VfxVolumeIdentity
{
    std::uint64_t effect_id{};
    std::uint32_t effect_handle{}, source_id{}, source_index{}, output_index{};
    std::uint64_t event_sequence{}, owner_id{}, episode_generation{};
    bool operator==(const VfxVolumeIdentity &) const = default;
};

// A live, bounded LocalVolume output. Matrices use absolute DirectX row-vector
// convention; the canonical shape occupies [-1,1]^3. Bounds are the transformed
// canonical box and can be used for world-space shadow receiver demand.
// This is separate from the cooked 112-byte scene VolumetricVolume ABI.
struct VfxVolumeInput
{
    VfxVolumeIdentity identity;
    std::array<float,16> world_to_local{}, local_to_world{};
    Float3 bounds_min{}, bounds_max{};
    VfxVolumeShape shape{};
    VfxVolumeNoise noise{};
    std::uint32_t stable_seed{}, steps{}, minimum_quality{2}, half_resolution{};
    float elapsed_seconds{}, source_age{}, source_envelope{1.0f}; // Live knot envelope; gradient alpha is sampled on GPU.
    float authored_scale{}, density{1.0f}, alpha{}, anisotropy{};
    // Explicit LocalVolume profile: extinction is 1 m^-1; authored density,
    // alpha and smoke_fade multiply it. Scattering albedo is authored linear
    // RGB clamped to [0,1], independently of HDR. Authored linear RGB*HDR is a
    // separate emissive source (radiance per metre before density), integrated
    // in the same L/T interval as regular fog. It never gets composited twice.
    float extinction{1.0f};
    Float3 scattering_albedo{}, emission{};
    float hdr{};
    std::uint32_t gradient_row{};
    std::uint64_t transform_revision{}, density_revision{}, appearance_revision{};
};
static_assert(std::is_trivially_copyable_v<VfxVolumeInput>);
using VfxVolumeInputs = std::span<const VfxVolumeInput>;
} // namespace hs
