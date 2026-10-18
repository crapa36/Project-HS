# DEV_STATE

Implementation continuity only: current contracts, verified receipts and unresolved technical work.

## Current technical state
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

## Tooling qualification in progress

The requested candidate batch is not complete. Scope and retained evidence are under `Build/tooling/qualification-20260906/REQUEST.txt` and adjacent reports. Keep existing tool exclusions; do not repeat completed measurements.

The optional `msvc-asan-core` preset built and passed `simulation.determinism-core` against the 2026-09-08 checkout state; invocation and SDK/runtime prerequisites are in `Tools/tooling/WORKFLOW.md`, with receipt `Build/tooling/qualification-20260906/msvc-asan-core-receipt.json`. This evidence does not cover later source changes or establish a new full-project gate.

Tracy/MCP remains inconclusive: captures omit submitted jobs/wait zones and reported metrics need scrutiny. Its isolated instrumentation was removed; retained evidence and cleanup details are under `Q/tracy-cleanup.md`. CodeGraph did not establish a replacement benefit over Repowise; its isolated registration was removed. Debugging skills converged on bounded repairs without demonstrated benefit over the current workflow. TDD baseline/Matt runs completed, but the Superpowers arm remains pending; see `tdd-comparison.md`. `STATUS.md` indexes completed comparisons and remaining candidates. No batch-wide winner or final cleanup is claimed yet.
