#pragma once
#include <array>
#include <cstdint>

namespace hs
{
// Wire format: little-endian uint32 words, IEEE754 float32; never native struct dumps.
// Header: magic HSV4, format=5, schema, engine contract, minimum runtime,
// section count, payload bytes, FNV1a payload checksum; directory entries: kind,count,offset,bytes.
enum class VfxSourceType : std::uint32_t { Direct, ProjectileFollow, BallisticCollision, CurlMotes, ImpactSprite, HistoryRibbon, VolumeLocal };
enum class VfxOutputProfile : std::uint32_t { SpriteSdfAdd, MeshEmissiveOit, SpriteAdd, RibbonAdd, GroundSdfSoft, GroundSdfAdd, Distortion, GroundSdfOit, SpriteFlameOit, Light, RibbonOit, LocalVolume, Decal, SpriteNoiseOit, ScreenOverlay, EntitySurface, FresnelShellOit, FresnelShellEmissive, SpriteSmoke6WayOit };
enum class VfxParameterType : std::uint32_t { Float, Int, Bool, Enum, CurveRow };
// Every authored motion string has a stable typed opcode. Unknown authored text
// is rejected by the cooker; the renderer must never infer behavior from text.
enum class VfxMotionKind : std::uint32_t {
    ShardsLiftFall, BakedSixWayPlume, BallisticBurstGravitySpin, BoundaryPulseAccelerates,
    BriefRadialResponse, BuoyantTurbulentSmoke, ChevronsDissipate, CompressSnapOutward,
    CurvedLinkTravelingHead, CurvedRecoilArc, CurvesTowardCollector, DirectionalTurbulence,
    FastExpansionSurface, FastStampBreakout, ParticlesPulledInward, FractureAtPeak,
    FresnelShellContracts, ProceduralFlameTongue, GentleGreenPulse, HexSegmentsCollapse,
    HistoryRibbonTearsBackward, HistoryRibbonTaperedFlow, InstantCompressionAxialSplit,
    InstantExpansionCollapse, InstantPointFlash, InwardTeethPulse, LockedChevronsTowardImpact,
    LockedSteadyRingPulse, NoiseEdgeFlowsUpward, OneTightPulse, OptionalTransientArc,
    QuickHeatShock, QuickInwardFracture, RadialLobesUpward, RareInwardDrift,
    ReverseFlowBow, RingContractsReleases, ScrollsAttackDirection, ShortBallisticFan,
    ShortForwardShardFan, ShortLineIncomingVelocity, ShortRefractiveShock, ShortTurbulentWake,
    ShortUpwardSpiral, SingleCappedLightPulse, SingleHighPriorityLightPulse,
    SingleOutwardRefractivePulse, SingleShortPulse, SingleFrameSnapShrink,
    SlowCounterRotationContraction, SlowCounterRotationStableFootprint, SlowInwardCurl,
    SlowPulseStateLocked, SlowRadialFlow, SlowStableRotation, SlowUpwardHelix,
    SnapsInwardUpward, SnapsOutwardAim, SoftWiderSheath, SparseRearwardSparks,
    StableBoundaryInwardPulse, StaticProjectedResidue, StretchesProjectileDirection,
    StrongSnapGameplayResolve, SubtleFresnelPulse, SurfaceNoiseThroughBoss,
    ThinShardsForwardCone, TicksRotateLock, TightHeadDistortion, TwoPhaseNarrowWakes,
    TwoFrameSnapPerpendicular, VelocityAligned, VelocityAlignedHeavyLance,
    VelocityAlignedSharpHead, VeryShortLocalDrift
};
enum class VfxProgramSection : std::uint32_t { Effects=1, Sources, Outputs, Parameters, TextureBindings, SliceIndices, Curves, CurveKeys, EffectLookup, TextureResources, Strings, UpgradeBindings, UpgradeSequences };
struct VfxRange { std::uint32_t first{}, count{}; };
struct VfxEffectRecord {
    std::uint32_t handle{}, input_mode{}, importance{}, role{}, style{}, readability{};
    std::uint32_t payload_kind{}, anchor{}, orientation{}, scale{}, binding_timing{}, geometry{}, must_match_logic{};
    std::uint32_t timing_kind{}; float seconds{}; VfxRange sources{};
};
struct VfxSourceRecord {
    std::uint32_t effect{}, stable_id{}; VfxSourceType type{};
    std::uint32_t knot_count{}; std::array<float,4> knots{};
    VfxRange parameters{}, outputs{}; std::uint32_t authoritative{};
};
struct VfxOutputRecord {
    std::uint32_t source{}; VfxOutputProfile profile{};
    std::uint32_t shape{}, shape_domain{}, shape_scale_rule{}, shape_component_kind{};
    std::array<float,4> rgba{}; float hdr{};
    std::uint32_t gradient_row{}, min_quality{}, coverage_type{}, coverage_ref{}, surface_target{};
    VfxRange parameters{}, textures{};
    VfxMotionKind motion{};
    float motion_rate_hz{};
    float motion_amplitude{};
    float motion_inset_fraction{};
};
struct VfxParameterRecord { std::uint32_t key{}; VfxParameterType type{}; std::uint32_t bits{}; };
struct VfxTextureBindingRecord {
    std::uint32_t role{}, catalog_slot{}, selection{}, min_quality{};
    float strength{1}; VfxRange slices{};
    std::uint32_t first_frame{}, frame_count{}; float fps{};
};
struct VfxCurveRecord { std::uint32_t interpolation{}, domain{}; VfxRange keys{}; };
// Strings section stores UTF-8 bytes, without terminators. Offsets are section-relative.
struct VfxEffectLookupRecord { std::uint64_t effect_id{}; std::uint32_t handle{}; };
struct VfxTextureResourceRecord { std::uint32_t slot{}; VfxRange asset_path_bytes{}; };
struct VfxUpgradeBindingRecord { std::uint64_t skill_id{}; std::uint32_t ordinal{}; VfxRange sequence{}; };
struct VfxCurveKey { float time{}, value{}; };
}
