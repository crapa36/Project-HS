#pragma once
#include <hs/core/types.hpp>
#include <array>
#include <cstdint>
#include <variant>
#include <vector>

namespace hs
{
using VfxEffectHandle = std::uint32_t;
inline constexpr VfxEffectHandle kInvalidVfxEffectHandle = 0xffffffffu;
enum class VfxQuality : std::uint8_t { Low, Medium, High };
struct VfxGroundMotion
{
    float rate_hz{};
    float amplitude{};
    float inset_fraction{};
};
enum class VfxGroundShape : std::uint8_t { Circle, LineBorder, LineHatch, ConeBorder, ConeHatch, RingGapsBorder, RingGapsFill, RingGapsPreviewBorder, RingGapsTicks, SafeSectorMarker, CirclePreviewBorder, CirclePreviewTicks, HexConstellation, CrossRing, BrokenHex, AxialFracture, RepeatingChevron, BrokenCrown, ClosedCrownRing, StateRing, PolarRune };
struct VfxGroundGeometry
{
    VfxGroundShape shape{VfxGroundShape::Circle};
    Float3 direction{0, 0, 1};
    float half_width{}, half_length{}, edge_width{};
    float spacing{1}, scroll{}; // border: metres and m/s; hatch: repeat count and local UV units/s
    float range{}, half_angle_radians{};
    float inner_radius{}, outer_radius{}, gap_half_angle_radians{};
    std::vector<float> gap_angles_radians;
    float progress{}, animation_phase{};
    std::uint32_t tick_count{};
    std::uint32_t spokes{}, rings{};
    std::uint32_t mask_slice{0xffffffffu};
    float mask_strength{};
};
using VfxTransform = std::array<float, 16>;
inline constexpr VfxTransform kVfxIdentityTransform{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

// Geometry remains in metres. Owner transforms and stable identity live in the
// envelope below; these payloads never integrate a gameplay-owned position.
struct VfxPointPayload { Float3 position{}, normal{}, direction{}; float authored_scale{1}, scalar0{}, scalar1{}; };
struct VfxContextPayload { std::uint64_t owner_id{}; Float3 direction{}; float ratio01{}, scalar0{}, scalar1{}; };
struct VfxProjectilePayload { Float3 velocity{}, hitbox_half_extents{}; float hitbox_radius{}, lifetime01{}; std::uint32_t state_flags{}; };
struct VfxProjectilePathPayload { Float3 current_position{}, previous_position{}, velocity{}; Float3 path_origin_position{}; float projectile_radius{}, lifetime01{}; std::uint32_t state_flags{}; };
struct VfxCirclePayload { Float3 center{}, direction{}; float radius{}, inner_radius{}, lifetime01{}; std::uint32_t state_flags{}; };
struct VfxConePayload { Float3 origin{}, direction{}; float range{}, half_angle_degrees{}, lifetime01{}; std::uint32_t state_flags{}; };
struct VfxLinePayload { Float3 start{}, end{}, direction{}; float width{}, lifetime01{}; std::uint32_t state_flags{}; };
struct VfxRingGapsPayload { Float3 center{}; float inner_radius{}, outer_radius{}, gap_half_width_degrees{}, lifetime01{}; std::uint32_t state_flags{}; std::vector<float> gap_angles_degrees; };
struct VfxSafeSectorPayload { Float3 center{}, direction{}; float inner_radius{}, outer_radius{}, half_angle_degrees{}, lifetime01{}; };
struct VfxEntityPayload { float footprint_radius{}, lifetime01{}, health_fraction{1}; std::uint32_t state_flags{}; std::uint64_t render_instance_id{}; };
struct VfxLinkPayload { Float3 source_position{}, target_position{}; std::uint64_t source_id{}, target_id{}; float lifetime01{}, width{}; std::uint32_t state_flags{}; };
struct VfxGeometryPayload { Float3 direction{}; float radius{}, width{}, range{}, angle_degrees{}, lifetime01{}; std::uint32_t state_flags{}; };
enum class VfxPayloadSchema : std::uint16_t { Point, Context, Projectile, ProjectilePath, Circle, Cone, Line, RingGaps, SafeSector, Entity, Link, Geometry };
using VfxPayload = std::variant<VfxPointPayload, VfxContextPayload, VfxProjectilePayload,
    VfxProjectilePathPayload, VfxCirclePayload, VfxConePayload, VfxLinePayload,
    VfxRingGapsPayload, VfxSafeSectorPayload, VfxEntityPayload, VfxLinkPayload, VfxGeometryPayload>;
inline VfxPayloadSchema PayloadSchema(const VfxPayload &payload) noexcept
{ return static_cast<VfxPayloadSchema>(payload.index()); }
struct VfxEventInput
{
    VfxEffectHandle effect_handle{kInvalidVfxEffectHandle};
    Tick event_tick{};
    VfxTransform world_transform{kVfxIdentityTransform};
    VfxPayload payload;
    std::uint32_t stable_seed{};
    VfxQuality quality{VfxQuality::High};
    std::uint8_t importance_override{}; // zero: use cooked importance
    Sequence sequence{};
    std::uint64_t status_episode_generation{};
    // Scheduled warning owner metadata; authored event mode remains unchanged.
    std::uint64_t geometry_owner_id{};
    std::uint64_t geometry_effect_id{}; // Stable warning recipe identity survives handle reloads.
    Tick geometry_start_tick{}, geometry_end_tick{};
    std::uint32_t geometry_sector_index{};
};
enum class VfxImpactShape : std::uint32_t { SoftDisc, CrossSlash, NoiseDisc, StatusStamp, RadialSlash, Pulse, StarRing, LeafMote, SmallMote, DiamondMote, Flame, Smoke6Way, RadialEnergy, ThreeProngBite, BossCrestContact, NeedleStar, Spark };
struct VfxFlashSpawnInput
{
    Float3 position{};
    Float4 color{};
    float size{};
    float delay{};
    float lifetime{};
    Tick event_tick{};
    std::uint32_t stable_seed{};
    std::uint32_t gradient_row{};
    float hdr{};
    float normalized_age{};
    VfxImpactShape shape{VfxImpactShape::SoftDisc};
    Float3 direction{0, 0, 1};
    float aspect{1};
    std::uint32_t curve_row{0xffffffffu};
    std::uint32_t mask_slice{0xffffffffu};
    float mask_strength{};
    // Immutable burst initial conditions; shader evaluates from absolute source age.
    Float3 initial_offset{}, initial_velocity{};
    float orbit_radius{}, orbit_phase{}, orbit_rate{}, drag{};
    bool oit{};
    bool ground_base_anchor{}; // Circle burst support rests above its authoritative base.
    std::uint32_t fbm_octaves{4}, smoke_first_frame{}, smoke_frame_count{};
    float domain_warp{}, smoke_fps{}, opacity_scale{1}, motion_strength{};
};
struct VfxPersistentInput
{
    std::uint64_t stable_id{};
    std::uint64_t status_episode_generation{};
    VfxEffectHandle effect_handle{kInvalidVfxEffectHandle};
    std::uint8_t source_visual_kind{0xff}; // Transitional legacy visual replacement key.
    VfxTransform current_transform{kVfxIdentityTransform}, previous_transform{kVfxIdentityTransform};
    VfxPayload payload;
    // Authoritative wall-independent age in seconds from the gameplay tick.
    // normalized_age remains a gameplay-provided normalized envelope; zero is
    // the live sentinel for projectile records with no authored expiry.
    float elapsed_seconds{};
    float normalized_age{};
    // Authored source interval for a live owner. This can end before the
    // gameplay owner disappears (for example, a held charged draw).
    float source_duration_seconds{};
    std::uint32_t state_flags{}, stable_seed{};
    VfxQuality quality{VfxQuality::High};
};
}
