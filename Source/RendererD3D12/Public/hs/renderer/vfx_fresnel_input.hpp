#pragma once

#include <hs/renderer/vfx_frame_input.hpp>

#include <cstdint>

namespace hs
{
enum class VfxFresnelShellKind : std::uint8_t
{
    BossCrown,
    BossTransition,
    PlayerInvulnerable,
};

struct VfxFresnelInput
{
    std::uint64_t render_instance_id{};
    std::uint64_t stable_id{};
    VfxEffectHandle effect_handle{kInvalidVfxEffectHandle};
    VfxFresnelShellKind kind{VfxFresnelShellKind::BossCrown};
    VfxTransform current_transform{kVfxIdentityTransform};
    VfxTransform previous_transform{kVfxIdentityTransform};
    float footprint_radius{};
    float source_progress{};
    float elapsed_seconds{};
    Float4 color{}; // Cooked RGB and source-envelope-weighted alpha; HDR is separate.
    float hdr{};
    std::uint32_t gradient_row{};
    float motion_rate_hz{}, motion_amplitude{}, motion_inset_fraction{};
    std::uint32_t sector_count{};
    float rotation_hz{};
    float fresnel_power{};
    float noise_amount{};
    std::uint32_t end_crack{};
};
}
