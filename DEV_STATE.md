# DEV_STATE

Implementation continuity only: current contracts, verified checks and unresolved technical work.

## Current technical state
### Engine slimming (2026-10-21)

Current root remains `codex/renderer-meadow-backup-20261018`, based on remote
`6e30b9f55386389b1e282b5ad9d30294fbd1f689`. Changes are uncommitted. The user's
existing HLOD/Debug edits remain; two integration errors were corrected without
discarding them: capture the stored refinement budget in the HLOD worker and
match `MeasureGeometryError`'s definition to its already changed declaration.

`hs_dev` now supports only build/verify. Verification uses explicit CTest
selection, native `test_prep` targets and native failure/timeout behavior.
Changed inference, verification receipts and automatic Generation repair are
removed. Producer recipe identity is the actual linked executable SHA-256;
CMake/Ninja owns header/source rebuilds. Equivalent relinks may recook assets.
`hs_asset_pipeline` now compiles shared Action code once for the separate producer
executables. Recipe JSON generation/preparation has been removed; depfiles track
the actual executable. Verification streams native output and phases immediately,
using stderr for progress in JSON mode.
The MSVC include projector remains the real header-dependency adapter.

Legacy oracle/shader projection, four unused tooling experiments and the
unconnected performance writer are removed. Non-recording ordinary play skips detailed
diagnostic formatting/output; Debug now also requires explicit recording.
Record/output/replay options retain their activation and ordering semantics.
Smoke, Experiment, record/replay and explicit
capture retain their output/failure contracts. Populated non-animated snapshot
revisions skip repeated hashing. Game rules, 60 Hz, saves, UI contribution
statistics, authored rendering and native asset publication remain intact.

The second semantic feature graph is removed. AO/GI/reconstruction use concrete
service inputs, and post processing emits the configured fixed pass chain.
Actual native format/extent and FrameGraph resource/state/lifetime checks remain.
Contiguous scene deltas borrow snapshot spans through the Render call; only the
generation-gap fallback owns new delta vectors. Submit/Commit history is unchanged.

BalanceObserver uses sorted contiguous cast histories with an increasing-ID fast
path and separate enemy archetype histories. Late hits and reset retain the
previous metrics. Synthetic lookup measurements improve substantially; history
still grows with unique casts and late new insertions remain linear.

Experiment executes validated specs in its child, without the duplicated Named
Pipe protocol. The watchdog remains; same-tick actions sort by sequence.
Runtime no longer watches or replaces simulation rules inside a pinned Generation;
rebuild/restart applies new rules. Other renderer reload paths remain.

Generation loading shares one parse/canonical byte snapshot for normal and GC
paths; schema 1 remains GC-only. SceneProbe layout cooking no longer hashes 17
unrelated C++/HLSL sources or rebuilds its package on cache hits. Authored input,
producer identity, upstream closure and native capture invalidation remain.

Runtime is the sole startup settings loader. ProjectHS parses CLI once; explicit
CLI values override saved settings, and the window uses the resolved settings.
Smoke uses process defaults/overrides rather than the user profile.
Jobs Submit now takes a name and returns admission bool; unused priority,
numeric handle and per-submission atomic ID allocation are removed.
New Generation publication hashes while copying the source, then verifies the
destination: two full reads instead of three. Atomic publication, cleanup,
integrity and existing-generation source verification remain.
The unused hs_content_tools executable and its JSON edit/rollback/build/launch
wrappers are removed. Native producers and GameData validation remain.

View history's unused semantic registry is removed; actual resources and history
generations remain authoritative. Per-frame ID/order temporary vectors are gone;
Main-first and auxiliary packet order remain. Sequential view exchange still
waits for shared descriptor-bank reuse. FSR already selects owned contexts;
Streamline's SDK-conditional state exchange is retained pending enabled-SDK evidence.

Capture(request) is the only public capture entry point; format-specific helpers
are private and the unused CaptureViewPng wrapper is removed. The runner now
captures geometry/visibility for the actual last submitted view, preserving
shared-frame ownership rejection. Owned-view probing exposed a second existing
geometry/material ordinal mismatch: CPU diagnostics use the shared material map,
and cluster EmitCuts now uses the prepared MaterialPartition instead of CatalogIndex.

Material emitters no longer validate the same graph twice. Deferred closure
emission derives its varying sidecar mask internally, removing the caller mask
and duplicate reflection/optimization. The RenderData-owned hs_material_compiler
target isolates this authoring code from runtime data formats; only two producers
and material package tests link it. VFX cases now use synthetic/cooked executables
instead of 11 separate links, retaining each CTest name/process and fixture scope.

Verification: core 18/18
(`verify-core-20261020T153349264Z-5512.log`), targeted checks 17/17
(`verify-20261020T152812294Z-3880.log`), including all 11 VFX cases, package format,
material parameter-only cooking, DRS, architecture, render smoke and grass history.
After capture and cluster-binding fixes, GPU smoke/cluster-retest/grass-history
passed 3/3 (`Build/engine-lean-audit/lean-fourth-final-gpu.log`). Final owned-view
probe passed 3 packages/9 submissions, ordering/history and stale-owner rejection
with validation_errors=0 (`owned-view-fourth-final/result.json`).
The compiler-only changes preserved all 68 shader/parameter Action product
identities and 340 product digests. The final cluster material correction changes
only cluster_emit_cs.dxil; the other 339 products remain identical. VFX executable
bytes fell from 24,392,704 to 7,425,024; this excludes PDBs and is not a timing.
Five post-split material/VFX/architecture checks passed
(`Build/engine-lean-audit/lean-fourth-isolation-tests.log`). Ninja direct executable
dependents for compiler edits fell from 32 to 3 (`material-link-isolation.json`).
Full-suite and optional Brixelizer/FSR/Streamline/XeSS execution are not covered
by these selected checks. Prior unchanged publication checks and timing evidence
remain in the audit; the publication timing ranges overlap and do not establish
a reliable speedup.
Final ProjectHS/render runner/content-generation rebuild reported no work
(`Build/engine-lean-audit/lean-fourth-noop.log`). Runtime Generation is now
`276c695e6f26fd3ff15808ec2751fbf261b5f4c90cb91a8941c9b1748271d573`.
Current decisions/evidence: `Docs/ENGINE_LEAN_AUDIT.md`.

Gameplay enemy lookup now uses binary search over the existing increasing-ID
actor vector, without an index/cache or additional update work. Spawn barriers
append increasing IDs and cleanup preserves survivor order. Delayed cast runtimes
are not ordered this way and retain their existing lookup.
Navigation flow uses fifteen circular buckets for its fixed 10/14 edge costs;
bucket capacity is reused. The distance field, waypoint selection, 60 Hz phase
order and canonical checksum remain unchanged.
Core checks passed 18/18 (`verify-core-20261020T155517272Z-6904.log`).
Related gameplay/content/Experiment/render-smoke checks passed 11/11
(`verify-20261020T155631444Z-9760.log`). Debug navigation additionally passed
72 full-field comparisons against the previous heap implementation, including a
long corridor. Combat regression covers middle-ID removal, respawn and reset.
Navigation microbench improved 63% on an empty grid and 32% with 25% obstacles,
but the single-corridor case regressed 0.018 ms. Retained bucket capacity peaked
at 10,536 bytes per grid, plus vector headers. Whole-TickFixed 1,800-tick scenarios
did not establish an improvement: clear/obstacle medians rose 1.44%/1.51% amid
large variation. Every before/after tick checksum and state/signal summary trace
matched across six runs per implementation per scenario. Evidence:
`Build/engine-lean-audit/gameplay-benchmark-comparison.json` and
`navigation-flow-benchmark.log`. These are CPU scenario timings, not game FPS.
Actual-EnemyActor lookup microbench: mixed hits at 600/1,500 actors improved
74.10->37.06 / 193.37->46.14 ns; small arrays and first-element hits regressed.
Binary search is retained for large-actor scaling without another cache or
size threshold. Evidence: `enemy-lookup-benchmark.log` in the same directory.

Detailed timing evidence is now in `Docs/PERFORMANCE_BREAKDOWN_20261021.md`.
Production code was not changed for profiling; copied Gameplay sources under
Build instrument the existing phases. Attack-fixture player movement is blocked
by held basic attack despite changing move targets; prior results must not be
described as continuous player movement/flow-rebuild coverage.
Hash, movement, spatial rebuild and AI comprise about 98% of measured TickFixed.
Render base/showcase each captured all 600 CPU/GPU frames with no invalid queries
or D3D12 errors. RTX 4080, RelWithDebInfo, validation ON, 1280x720, legacy,
hidden smoke lockstep: warm CPU graph-record 11.38 ms, GPU shadow 5.84 ms and
GBuffer 3.13 ms. The 16.83 ms wait stage includes lockstep's global GPU drain;
actual frame-slot wait is about 0.001 ms. Do not infer normal-play FPS from it.
The earlier capture had about 294 record jobs/frame; import/export batching has
since been implemented as described below. Exact plan caching already exists,
and cache hit frequency was not exported. Full timing CSVs and
provenance are under `Build/engine-lean-audit/detailed-profile/`.
Nested Gameplay measurements and separate moving fixtures each preserve their
uninstrumented reference traces (four fixtures, 5,400 ticks each). Enemy hashing
is 28.9–33.6% of TickFixed, enemy movement 21.3–25.1%, separation 10.7–13.4%,
projection 6.5–7.6%. Moving fixtures rebuild both flow grids in 585–597 events
per 5,400 ticks, accounting for 6.5–6.7%; attack fixtures only 30–33 events.
Debug/UI commands additionally hash; their command cost is outside these tick
timings. Production profiling APIs were not added. Detailed source/executable
hashes and method are in gameplay-profile-manifest.json / gameplay-profile-method.json.

Measured bottleneck changes are implemented; details are in
`Docs/BOTTLENECK_OPTIMIZATION_20261021.md`. `SimulationConfig.capture_checksums`
defaults to true for compatibility. Ordinary Runtime and CombatSim disable
automatic tick/debug/UI checksums; recording/replay/smoke/Experiment retain them.
Disabled TickResult/read-model checksums are zero; explicit `ComputeChecksum()`
always computes the current canonical hash without enabling automatic capture.
Enemy separation uses reusable compact position storage, excludes bosses only
from the separation grid, and preserves pair/order/tie-break semantics.
Movement avoids unused zero-displacement collision math; AI skips path/direction
work when its branch will not move. Independent render final exports are batched
by queue and unchanged unused imports no longer create empty command lists.

CPU comparison used 1,800 ticks x three repeats in six fixtures, with alternating
before/enabled/off execution order. TickFixed median milliseconds before ->
after checksum-enabled -> after checksum-disabled:
clear combat600 1099.390 -> 958.451 -> 597.554;
obstacle combat600 1237.470 -> 1143.190 -> 737.601;
clear moving600 1450.270 -> 1191.210 -> 786.028;
obstacle moving600 1433.630 -> 1211.440 -> 963.134;
obstacle combat1500 3395.070 -> 3142.530 -> 1309.060;
obstacle moving1500 4090.970 -> 3773.240 -> 1764.180.
All before/enabled/disabled-with-explicit-checksum traces match for all repeats;
the explicit checksum runs are separate from disabled timing runs. Read-model
extraction is timed separately; spawn commands and trace I/O are excluded.
Three-repeat variability is substantial, especially at 1,500 actors, so these
are scenario medians, not universal speedups or game FPS. Boss/progression paths
are outside these fixtures. Evidence, preserved binaries/headers, full samples
and method: `Build/engine-lean-audit/bottleneck-opt-20261021-013729/`.
Core 18/18 and the selected content/gameplay/runtime/render checks passed;
the new multi-queue fixture's incorrect native-layout assumption was corrected,
then CTest WARP and hardware Legacy/Enhanced paths passed with GPU validation.
Fresh 600-frame renderer AB/BA captures preserve final PNG pixels, settings and
600/600 CPU/GPU coverage with validation errors 0. Warm recording jobs fall
294.325 -> 90.325; CPU graph record improves 18.5-19.7% and submit 11.2-15.8%.
GPU critical path changes +4.5% in AB and -1.1% in BA, so no consistent GPU gain
or production FPS claim is established. Captures use validation-on smoke
lockstep, including global GPU drain. Owned-view validation completes
3 packages/9 submissions with unchanged outputs and duplicate/nonfinite/stale
input rejection, validation errors 0. Detailed conditions, per-pass data and
limits are recorded in Docs/BOTTLENECK_OPTIMIZATION_20261021.md and the same
Build evidence directory.

A prior GBV-only persistent-cache experiment passed cold/warm with zero errors
but did not solve first-frame CPU latency:
390.24s -> 382.01s despite native PSO creations 154 -> 0. The cache-path code
change was reverted. Evidence: Build/verification/gbv-cache-20261020T231026675/.
First-use GBV/driver preparation remains an unresolved cost, not a measured
whole-game performance defect. No validation flags/timeouts were weakened.
The requested view/material/test candidates have been reviewed and applied where
they remove actual work. Fixed material recipes and deletion of multi-view support
would duplicate existing authoring logic or remove AI capture functionality; they
were not applied. Enabled Streamline state ownership remains an unverified
candidate, not a claimed performance fix.
Earlier validation below applies only to its recorded source/configuration.

### Workspace conflict recovery (2026-10-18)

The working root is Documents/GitHub/Project-HS, on branch
codex/renderer-meadow-backup-20261018; conflict recovery used baseline HEAD
90a1d852. All 18 stash conflicts were resolved with source/index preimages
retained. The recovery itself did not reset, commit, push, delete stashes or
touch other worktrees. The 47 staged Elin source/validation assets and five
stashes were protected by byte, staged-blob and identity checks.

Invalid temporal history binds a typed null SRV; shader early return and frame
completion contracts remain intact. Runtime UI-page resets and replay numeric
range checks retain current APIs. The standalone performance writer is tested,
but is not connected to removed timing APIs. Projectile trail centers sample
their owner's ground XZ, while endpoint histories retain their own grounding;
24 flat/meadow regression combinations cover this distinction.

Full verification exposed stale meadow CPU oracles and a visibility diagnostic
using 16 instead of builtin 1 + authored material count. Exact terrain fixtures
and catalog-derived diagnostics now pass the focused 6/6, including grass history
and material reload. Core 20/20 passed. Initial full verify 106/111 is retained as
failed evidence. The final configured full verify passed 111/111, exit 0, full_gate
true: Build/verification/verify-20261018T140746948Z-32008.json.

The historical barrier cache differed from fresh same-mode temporal history.
Two fresh legacy and two enhanced runs were byte-identical, with native exit 0
and validation errors 0. Default barrier comparisons now use fresh paired runs;
explicit baseline input, original pixel tolerances and 180-frame/option/error
checks remain. The final full gate's paired captures also differ by zero pixels.
This does not establish the original history divergence's exact publication cause.

Recovery Validation is RelWithDebInfo with ordinary D3D12 validation ON. Optional
Brixelizer/Streamline/FSR/XeSS/DirectStorage remain OFF as found. Runtime-full
Generation is 0da36dde9a9d18bf46051aa67aab9a12cc22c9d17a9cdbf5cd3d16a0e32c1719.
Configured native unit GBV coverage is not proof for the prior all-features game
configuration. No FPS/latency improvement or Debug/Release export is claimed.
The report is Docs/Verification/20261018-workspace-recovery/REPORT.md; final
preservation evidence is under Build/20261018-workspace-conflict-recovery/.
After the full gate, nine ignored temporary fixture/build/capture directories
were pruned (63.76 GiB of logical file sizes; hardlinks may reduce physical
reclaim). The complete verification JSON/XML/logs, protected conflict preimages,
source assets, stashes, independent worktrees and repo-local CMake tooling were
preserved. Cleanup receipt: Build/verification/cleanup-20261018.json; final
paired barrier summary was copied to Build/verification/barrier-comparison-verified.json.
Deleted validation binary/Generation products must be rebuilt before rerunning
GPU tests. Older ZIP/native-GI evidence below belongs to its recorded configuration.

### ZIP omissions and integrated renderer follow-up (2026-10-18)

Exact2original ZIPs recovered/read with priorSHA match; primary310 rows/HTML and
40directive tasks/14couplings/12adoption candidates now have individual decisions.
21former no-followthrough IDs have reasons; not310implemented features.
NewN15 retained slot staging preserves rollback/duplicate/generation; N20 immutable
transparency descriptors prepared once per target/frame/phase; N07 SSS center-guide
reuse; N08 zero-scattering skips lighting but writes emission+extinction; signed
scale normal/tangent handedness corrected. Pending grass grids/Jacobian/canonical
particle terrain contact are now wired. Particle update/spawn combines compaction
and9bins with group reservations and mutable-only stores; dead-slot CAS unchanged.
SSR3level reduction preserves odd/ceil/MAX and reduces FHDdispatch12->5; exact high
PCF Gather keeps scalar border path/static-probe;64cell light view bounds shared.
FrameConstants12848B; upload prefix14KiB retains disabled fog/exposure boundary.
Prior ZIP validation Generation 7a86b9c93ec7ac8dd2fa8ad4b2c7eb260bc3b3841eb62598c7c066e012a8ed19;
Debug/Release retain e7e754cc earlier coherent hosts/content, no new export.
69focused checks and existing4test narrow Tools/verify batch PASS; FHDenhanced
lockstep4frames exit0/assertions true/16particles/complete diagnostics/empty
validation log. Basic showcase rendered1GPUframe, not60GPUframes.
GBV4stopped at120s afterinitialize with no capture/result/final diagnostics; not
PASS and no teardown-stage diagnosis. No gate suppressed. No144FPS/GPUgain proof.
Remaining:full stateless/active indirect dispatch, terrainLOD decline and exact
contact-vsmesh error, GI energy, graph liveness, cones and full temporal/failure
corpus remain explicitly scoped; no unmeasured quality cuts. Decorative collision
uses capped64sweep samples and1impact per tick, not fullCCD. Source uncommitted;
backupa5e2183a unchanged. Evidence:Build/20261018-zip-followup/; report:
Docs/Verification/20261018-zip-followup/REPORT.md.

### Current meadow art and hosts (2026-10-18)

Earlier meadow art Generation `e7e754cc192ec7e2b5a996ff0612950c94f510f8693b707b162e7186e8cf6312` (3,646 products)
remains in Debug/Release. The ZIP-stage Validation host used the coherent Generation
listed in that section, with unchanged meadow source art and updated shaders. Prior art
receipts refer to their exact older Generation and are not current GPU gain proof.
Terrain uses shared mild macro relief, banks/spill-limited water and baked UV wet/dry
controls; generated terrain remains world mapped. Ground triangles119110/117926/117466
and100micrometre discrete oracle preserved. Analytic contact/triangle-centroid difference
can reach12.409mm; static mask interpolation is approximate.

Environment geometry catalog35, GroundDetail variants19, Meadow33/34; authored
materials16/ABI2 unchanged. New five props use ground_detail atlas slice1. HLOD
keeps same-material atlas selectors distinct through cooked parent payloads.
Existing five supply props LOD0 triangles35000->11240; new five add4772. Four flower/
clover meshes were refined; bounded24m short/long/dry grass/flowers and narrow
road-centre weeds are integrated. 60Hz simulation/collision is unchanged.

Earlier catalog/architecture/cooker/instance/presentation/HLOD and180 integration
checks passed. Current terrain structural/oracle and shader41/41 checks, content
environment-assets and FHD16 validation-host smoke pass (exit0/assertions true/
validation_errors0/diagnostics complete). Subpixel POM skips <=0.25 raster pixels
with0.25-0.5 continuous fade; independent material height blending remains.
Two additional GBV16 attempts stalled during teardown after image capture and
were stopped; they are incomplete, not passes. Disk PSO cache separation did not
resolve the stall. Exact remaining shutdown stage/cause is unconfirmed.
No new gameplay FPS proof; older GPU timing and exact-output receipts below
predate this art generation and are not current-art performance results.
Current receipts:Build/20261018-terrain-perceptual/final-receipt.json. Earlier art
receipts:Build/20261018-meadow-quality-continue/final-receipt.json.
Next technical work is to localize GBV teardown without changing error gates.

### GPU work reduction and current hosts (2026-10-18)

Highest target remains1920x1080/native internal100%, native SoftwareHybrid GI,
DLSS-G2 permitted;144FPS has not been achieved. Last actual foreground05 remains
base25.485FPS/strictSDK50.977FPS,GPUcritical35.571ms,CPUgraph8.786ms.
ProcessCPU4.663%/20LP,PIDbusiestGPU59.701%(93.62%coverage),NVIDIAwhole95%
point samples;these are different metrics. Whole raw capture partial due23PDH
invalid samples;valid foreground subset audited.No scanout/inputlatency proof.

Native GI compact exclusion preserves original source validation/domain priority,
supported geometry/order/transform and checked counters. Fixed-generation64
CPUgraph84.036->10.036ms;leaf~70->.87ms;jobs17554->702;records441302->706;
supported705/unique142/excluded440597 retained. GBuffer/depth/HDR/sceneWork
exact;CPU fixture/architecture and native64 standard+GBV4 errors0 passed.
Temporary attribution probes removed. Noise bias .25voxel,epsilon .001 retained;
GI brightness/energy calibration remains unresolved.

POM metadata is prepared once per invocation after original early returns;
originalUV/ray/steps/refinement/feedback footprint retained. AO skips self-pixel
before loading guide. Actual BC4 highest64 comparison matches all37surface files,
52setting fingerprints/camera/checksum. Observed GPU46.707->40.772ms includes
7-15%faster unrelated controls,so isolated gain is not established. Existing
POM native2304pairs/feedback4416bytes and AO39936pairs bitdiff0 reused.

ActualAppliedGraphics now stores configured FG multiplier policy,not last SDK
present count. Observed native telemetry unchanged. Settings-store test passed;
new realFG receipt has not been recaptured.

Cluster Count now publishes selected group IDs/view masks and Emit reuses them.
Count traversal,Prefix,EmitCluster and validation/admission contracts retained.
Per-catalog OutputCapacity>=maxInstances*WorstCut and positive group clusterCount
bound selected cache stride. Additional cache/clear memory is a pending timing
cost. Native old/new command parity,GBV and FPS measurements not yet executed.
Prototype proof and source receipt:Build/20261018-cluster-cut-cache/.

Directive package40 tasks have explicit decisions in
Build/20261018-renderer-directive/execution-ledger.json; deferred items are not
native validation/comparison completion. Four additions: cached GPU reservation
and host held sums (overflow recovery preserved); sampler feedback global-min
scan exits at0; Decal Prepare uses the existing registered-root pipeline cache;
SSR raw allocation follows trace_capacity (highest100% saving0). Existing CPU
residency/streaming regression passes,including budget boundaries,overflow,
commit/abort and completion recovery. No new GPU or FPS measurement.

The GPU component receipts above were captured before the current meadow art Generation.
Current selected hosts/content are recorded in the section above.
Host builds precede portable snapshot publication to avoid an old small allocation
with the new cache-reading shader. Current kernel package is regenerated by the
normal cooker. No new game process or ComputerUse was started for this batch.

Cleanup removed completed GPU readbacks and stale Generation/ProductSet targets;
source/Git/current packages,all play-captures,SDK/deps and small receipts/CSV/PNG
retained. Historical raw comparisons are now hash/summary evidence;many old
Generation payloads are no longer present. Cleanup receipts:Build/20261018-disk-cleanup/.
Next technical work:cluster native command equivalence and precise pass timing;
remaining shadow/VSM redundant work. No gain claimed from compiler metrics alone.

## Accepted baseline

- Slime eye-socket repair is the current accepted baseline.
- The relic roster contains 20 relics with no unresolved correctness hold; current tuning state is in `RELIC_BALANCING_PLAN.md`.
- Relic contribution is exposed in the character and result UI.
- Current UI text measurement, clipping, card layout, relic display, bars, and combat-HUD readability fixes are integrated.
- Latest verified RelWithDebInfo CTest baseline: `24/24` PASS (2026-09-14; receipt in the environment report).
- Accepted render/runtime validation gates complete with zero D3D12 validation errors.

Do not repeat completed relic census/tuning matrices unless a change invalidates their evidence.

Use source, tests, and current build output over this file if they disagree.

## Tooling scope

Native builds, explicit CTest selection and optional LSP remain the supported
loop. Retired tool comparisons under `Build/tooling/qualification-20260906`
are historical evidence, not a pending qualification gate. Profiling and
sanitizers remain on-demand; current commands are in `tools/tooling/WORKFLOW.md`.
