#pragma once
#include <cstdint>

namespace hs
{
enum class VfxOutputProfile : std::uint32_t { SpriteSdfAdd, MeshEmissiveOit, SpriteAdd, RibbonAdd, GroundSdfSoft, GroundSdfAdd, Distortion, GroundSdfOit, SpriteFlameOit, Light, RibbonOit, LocalVolume, Decal, SpriteNoiseOit, ScreenOverlay, EntitySurface, FresnelShellOit, FresnelShellEmissive, SpriteSmoke6WayOit };
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
} // namespace hs
