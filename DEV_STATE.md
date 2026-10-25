# DEV_STATE

Implementation continuity only. Dirty main at
5a674bcf6ec833b0fec3d8a122ccc61dfe3e1504. Preserve staged/unstaged work.
The canceled95cbe20 reversal stays canceled/restored. No commit/push.

## Current target and evidence

Target: Debug1920x1080/Ultra/100%/200enemies/60FPS. NOT achieved.
Latest changes: Build/followup-optimization-20261025/RESULTS.ko.md.
Last valid foreground FPS: Build/debug-ultra-60fps-20261025/RESULTS.ko.md.
Last ordered before/final observations19.586/19.799FPS, GPU critical48.909/48.386ms;
CPU graph build9.082/8.729ms, prepare4.448/2.701ms, nested compile3.091/1.297ms.
These are short ordered observations, not repeated controlled adoption. GPU16.67ms
budget remains substantially exceeded. Before CSM10.386/GBuffer7.215/four Hero
composites sum14.848/tail1.759ms are components, not added critical-path proof.
Final game_tick1.078ms/sim iteration1.975ms/Main0.036ms/audio drain0.025ms.
Graph validation0.114ms; info/key1.110ms is a different composite scope.
SoftwareHybrid GI consumer_unavailable -> Probe; native software GI not qualified.
PID GPU-engine counter unavailable; NVIDIA whole-GPU samples are distinct.

## Current implementation

Prior typed optional graphics preset, exact BSDF preparation, graph use-capacity
reuse and8-entry value-only plan cache remain. No /Od/assert/checked-iterator
change. Sample/Pdf normalization chains and all32sample coordinates remain;
exact graph keys retain current callbacks/imports/native leases. Four optional
ScreenOverlay/Decal topologies exceeded former3-entry cache;8 is fixed, not an
unbounded cache or topology/native-binding separation.

New renderer-only changes:
- Identity FXAA predicate matches actual FrameCB: temporal_options.x>.5 or
  lod_clock.w bit2. Last ToneMap/Outline writes same RGBA16F display_linear.
  Actual filtering path remains. Optional ScreenOverlay retains pass16 RW;
  otherwise copy pass16 is omitted. UI/SDK final physical resource unchanged.
- First-use clears follow actual retargeted RTV. No-outline identity initializes
  unused post_a once and declares additional write, preserving later AA switch.
  Steady frames have no extra post_a write. Existing hooks remain.
- CPU-only frustum_receiver_index marks only camera-frustum world AABB for
  froxels. Existing AABB-intersect-six-frustum-plane convex coverage proof reused.
  GPU feedback AABB/receiver layout/shader uploads unchanged. All other World/
  media/capture receivers retain original tests; malformed marker/actual outside
  retains CSM. No new cache, shader, setting, build layer, dependency or executable.
- Prepared command maximum survives early receiver-coverage denial. This fixes
  misleading0 diagnostic, not denial priority or capacity admission.

Current actual Debug ProjectHS/hs_experiment rebuilt. Source-ready690inputs
have no drift;9renderer/test inputs changed from prior batch, shaders0.
Build/followup-optimization-20261025/build-identity.json has exe hashes.
Generation unchanged6d6745bd66b3cf0c11cb1625ffa8c235592798c8d95229912949d1c7077442c7.
Before exe/Generation preserved as *-followup-before.exe/GenerationState/followup-before.json.

## Current checks and limits

Existing six-check batch:5passed, new feature test had predecessor-vs-read
expectation error. Fixed test only and reran rendering.feature-signals1/1 pass;
other5valid results reused. Logs verify-20261025T012438362Z-32632.log and
verify-20261025T012803828Z-17372.log. Existing content.render-package-format,
transient/multi-queue graphs (native GBV enabled), render-smoke and vfx.showcase
passed. No checks suppressed. Build log in followup root.

Frozen before/after Debug fixed8tick/8frame640x360/enhanced/driver-validation:
standard and no-bloom/no-outline each execution_valid/assertions/validation0.
Both final PNGs byte-identical, different pixels0, max channel error0; all game
checksums15443066793104118741. Actual pass logs remove FXAA in both cases.
One-frame baseline copy0.0195/0.0164ms is functional evidence only, not1080p
FPS gain. No new200-enemy FPS or all-time/HDR quality qualification in this batch.
Fixed scene STILL ReceiverOutsideCoverage(bits256), CSM remains. Diagnostic
maximum0->3417; total13730/capacity65536. Do not claim CSM10ms removed.

Ordinary native renderer post-target GBV2frame fixture initialized7.1s but did
not complete before120s limit; terminated, no result/frame proof. Isolated existing
entity-surface fixture produced8surface captures/final color and logged render
94.07s/capture0.029s/explicit Shutdown0.170s, but hit122.08s process limit with no
final result.json. Intermediate captures/empty validation log are not a completed
test. Both runs terminated. Prior renderer90s GBV timeout remains; graph GBV
success is scoped. Need separate GBV initial render and post-Shutdown completion
costs before attributing deadlock or teardown fault. No Computer Use this batch.

## Next critical implementation

1. B01-B matched base/coat prefiltered radiance/DFG/energy recipe for supported
   coated dielectrics, with converged same-BSDF HDR/grazing/TIR/probe/motion
   reference before production replacement. LUT-only expansion was investigated,
   not implemented: existing32branch would not consume it. Reuse HDR capture.
2. S01/B04 actual Ultra/volume receiver and caster supply closure; exact froxel
   proof alone did not remove current CSM. Then remove baked-static/full-CSM
   duplicated work under real guarantees, preserving fallback otherwise.
3. GBuffer terrain/foliage/opaque pixel/overdraw/class measurement and qualified
   material/geometry LOD. POM gating already exists.
4. N19/B08 topology/native bindings and registration/static refs uploads;8-entry
   plan cache only reduces recompilation, graph build8.73ms still substantial.
5. PT13 normal four-payload SPB1/cooked BRDF/startup native PSO closure. Existing
   layout-only producer still needs runtime capture; don't move work to combat.
   HLOD first Debug cook used CPU minutes; exact phase attribution remains open.

Whole directives, full-renderer GBV, HDR/guide/temporal/filtered quality,
long-run/uninstrumented/repeated performance/peak/C32 and60FPS remain incomplete.

## Preserved broader contracts and work

Final2026-10-24 directives/pseudocode and both Profile200 reviews/new ZIP remain
scope. References under .planning/2026-10-23-complete-directives/references/.
Final SHA256:
A44FCCA39E45F50E81A8AA86E2E138739250D4399E09A964EDA2379D1BF0952B /
4CEB65B6E48BB6A01FCF280B8C402E89CFBDDA42E760E8A954CA1098458D5A60.
ZIP940FC6D0906C2F1A870D7E6CFC697F22C675DE61734EB7BB5D8450DAC7F5375B.
Build/combined-directives-review-20261025/RESULTS.ko.md and3audits cover24N/32C/
PU14/PC27/PR16/PT17/179D/315K contracts+B01–10. New report supersedes its N19
3plan and B01 sampling-prep checkpoint only. Inventory is not qualification;
N03/N08/N12/N16 central source mechanisms exist,20otherN partial. Old60% referred
only to earlier Profile200 implementation/function scope.

Preserve Public module DAG/OFF-OFF/60Hz/strictFP/RNG/IDs/order/native leases.
PU05 PreparedMaterial compiler-owned state/producers exist; compiler build-only.
N05 BC7/full mips/shared DDS cooks all characters; filtered quality not approved.
N07/N12 Ready/StopFlush failure retirement/direct fanout/session/cold ambience
pending delivery/Main event wake exist. N16 ordered active XP work/N17 immutable
process scene/HLOD preparation exist. N18 skin36B exists; qualified new-LOD12B
previous pose absent. S01 VMOff/GIProbe no-CSM and65,536 partition proof retains
exact legacy fallback on unsupported domains. T03 nearest HDR+six guides fused;
farther layers single RT. H01 frame-slot ribbon args exist, warm growth benefit
unverified. I01 startup attribution exists, demand scheduling absent.
Remaining Shipping renderer/DXC, PU06–09 VFX store/recipes/due ACK, adjacent
packets/UI display-hit/dirty transactions, query/combat owners/dense CueId/
settings bridge/LOD, PT09/10/12/13/15/17 remain in integrated backlog.

Explicit Luna/high implementation and luna_runner execution only (no root model/
reasoning inheritance). Fixed APIs/file ownership; root integrates. Freeze source
then batch narrow existing checks. Default sandbox launcher failed0x80070020;
host exec works. Python source/path text UTF8; PS5 redirected logs can be UTF16.
