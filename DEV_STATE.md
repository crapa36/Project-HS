# DEV_STATE

Implementation continuity only: current contracts, verified receipts and unresolved technical work.

## Current technical state
### Renderer lookup and VFX candidates (2026-10-18)

Frame-local direct selected shadow refs preserve full identity/revision/selection
checks and128-byte light ABI. Complete128-bit light masks keep overflow diagnostic
bit0 and invalid-cell world fallback bit1; capture encoding is explicit and older
analysis format remains supported.9 particle bins use exact mesh counts and the
existing20-byte root-constant+draw signature in one ExecuteIndirect. Existing alive
index buffers grow560000 logical bytes total; dead-slot CAS admission is unchanged.
Capture sums all9 commands and records actual bin counts (16 total:11 sprites/5 mesh).
Ribbon persistent key lookup preserves900-frame exact differential output, with
1024-source component median565.046->157.051us; this is not whole-frame FPS.
Current CPU CTest6/6, native light-list/auxiliaries2/2, FHD VFX60 final,101 shader
projection and all3 host builds pass. D3D12 errors0 and diagnostics complete.
GPU/FPS benefit remains unproven:default capture has one measured GPU frame,
not60; no significance/causality claim. Remaining candidate4/6-12 are deliberately
unimplemented under their workload/policy/quality gates; shared canonical terrain
height already exists and is not POM UV. Receipts:Build/20261018-logic-replacements/.
Next technical work:representative full-frame GPU profiling before performance
adoption, and unresolved GBV shutdown localization; no validation gate lowered.

### Current meadow art and hosts (2026-10-18)

Current content Generation `e7e754cc192ec7e2b5a996ff0612950c94f510f8693b707b162e7186e8cf6312` (3,646 products) is
exported and verified in validation/Debug/Release. All hosts include GBV disk-cache
isolation and current light/shadow/particle/ribbon logic. Prior art receipts refer
to the exact older Generation recorded in those receipts.
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
