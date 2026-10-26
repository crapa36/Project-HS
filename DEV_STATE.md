# DEV_STATE

AI technical continuity only. Current dirty checkout; no commit/push. Active goal:
Debug /Od/assertions/checked iterators, 1920x1080 Ultra100%,200 enemies,60FPS.
NOT achieved. No Computer Use active. Artifact root: Build/csm-ibl-20261025/.

## Current in-flight source

raster-cache-source-ready.json703 hashes frozen; combined build/check running via
verify-raster-cache.ps1, unified exec session63440. DO NOT restart while live.
Changes: existing experiment pom_quality/geometry_frontend exposed via optional
ApplicationConfig fields and preset-after individual POM application. Parser/startup
checks in runtime_tests. Renderer six skinned monster pages append existing meshlet-
ordered IA indices behind unchanged canonical prefix; existing aux permutation
restores canonical PrimitiveID in indexed Visibility(flag64), mesh path unchanged.
Archer/environment untouched. Source/vertex/weight/triangle data not changed; no
new asset format/cooker/backend/queue/fence or profiling framework. About index-size
extra per affected page; actual allocation/residency tracked. CPU suffix tests in
instance-order; shader source44 checks passed. Native/actual FPS still PENDING.

Authoritative locality evidence character-index-locality.json: melee original LRU
misses18720 at16/32/64,meshlet-order12565;ranged21885->14743. Existing actual Hero
query VS4.22M/IA1.41M per phase,not theoretical full-scene counts. Hardware effect
not yet measured. Same primitive order/tie/blend effects need paired native tests.
Script first CPUtarget,then combined runtime/experiment/runner/testgen/runtimeTests,
smoke+Runtime,then before/indexed+GBV/mesh Visibility3frames each. Before uses preserved
spatial-demand runner. No CU active. Last fully verified build is spatial-demand below.

Next candidate (READ ONLY,not implemented): exact per-frame monster deformation
sharing by actual resolved(geometryID,mesh,clip,timebits),not time quantization.
Shader reads no actor ID/world transform/global random; bubble also only time/position.
Pose source modulo45run/120idle suggests repeats but actual resolved time distribution
not measured. Keep all per-entity SkinCache.Resolve/history/previous offsets/bounds,
Archer separate. SkinVertexRanges current-range Alias leader and leader-only vertex
CS dispatch could reduce GPU working set. Existing UAV barrier/immutable frame leases
and source package ownership must remain. Do not merge previous history by pose.
No alias code changed. Focus finish raster-cache build/native/performance first.

## Current source and verification

Current built source snapshot spatial-demand-source-ready/built.json (703 exact
hashes, unchanged after final run). Preserved *-csm-ibl-spatial-demand.exe;
experiment SHA8e38685cee066760dc9a6a907ce81a21c0981d924308bffd61d9de1c1ef47e77.
GenerationState/csm-ibl-spatial-demand.json selects
db566c056be305fd8e4cefff4b2831999eab5bde8e2b6c33c5930b6907db6214.
Same Generation as descriptor-vfx/hero-kernel; preserved descriptor-vfx is immediate
comparison. No CU/game/sampler remains. No commit/push.

Current spatial-demand batch: CPU rendering.instance-order passed(.63s), Debug
runtime/experiment/test-runner/test Generation built, native smoke passed(39.49s),
owned-view3packages/9submissions passed(validation0). 180frame1344x756 PNG AND raw
scene_hdr.dds byte-identical to preceding hero shader baseline; limited fixture,
not a whole1080p optical/temporal proof. Source703 exact hashes match preserved exe.

Spatial demand caches fixed16m AABB membership, maximum scale and wind-presence
with existing StaticDrawData scene/foliage-density lifetime. No copied full actor
state/native metadata/hash/epoch. Current catalog/wind/view values remain fresh.
Cells conservatively reject invisible/uncompetitive work and skip already-proven
inside frustum tests; exact old per-instance float calculation still selects max
priority. Invalid/near-plane/overflow uncertainty uses original calculation. No
new request/quality caps or scheduling policy. Generated clustered/large-coordinate/
negative/invalid scenes check bounds, FMA/non-FMA and bitwise max aggregation.

Actual spatial-demand run:1800ticks/710rendered,all710GPU frame/view IDs valid,
selected540 CPU/GPU pairs,normal exit/no crash/device removal/dropped input/events.
28.1217FPS,p50/95/99/max35.43/40.39/44.13/47.93ms,1%low21.99,50ms+0.
Immediate descriptor-vfx baseline28.0526FPS,35.61/40.97/43.41/44.57,1%low22.78.
No material whole-frame gain or uniform tail improvement; one ordered comparison.
CPU geometry demand3130->1032us(-67.0%),scene_gpu_prepare5096->2977us,
constants_upload6840->4753us; wait7187->9360.5us. Nested scopes not additive.
Saved CPU time becomes fence wait; prioritize actual GPU costs now. OS utilization
coverage remains partial(21counter statuses,2sampler logs,coverage issue).
Particle effects hash1189727272018964231 identical; simulation checksum capture0
in both ordinary runs, so do not claim a checksum comparison proved simulation.
Rendered particles459->458 is presentation wall-age/sample timing, not a drop count.

Strict influence-boundary LOD experiment rejected for the main crowd: raw cooked
page inventory shows melee12565/12565 vertices locked, ranged12565/14743 locked.
No mesh reduction would occur for150melee; no LOD cook/runtime code added. Proper
pose-aware error-qualified simplification is still needed; existing16-page metadata
can be reused. Animation interval bounds/weights/seams/all-consumer LOD/history
must remain correct. Inventory: lod-influence-inventory.json.
GPU source review found no duplicated shadow query or mergeable equal IBL samples:
base/coat roughness differs. Repeated normal/view preparation exists at source,
but actual DXIL residual cost is unproved; do not call it a large bottleneck.
Next useful diagnosis: actual GBuffer geometry-vs-pixel/overdraw pipeline statistics,
then major Hero/GBuffer structure. RenderDoc capture remains previously blocked
by DirectWrite E_NOINTERFACE (Tools/tooling/WORKFLOW.md); no unchanged retry done.

Preceding descriptor/VFX verification: combined Debug build and smoke/material-parameter-reload2/2
(40.36s/199.66s; latter uses existing GBV configuration) passed. Owned-view probe
passed3packages/9submissions, including NaN attachment history rejected before
submission and prior evidence retained. Final descriptor simplification removes
only immutable derived-key work; no VFX semantics changed. Final build and12frame
residency native validation passed, validation errors0. Passed long checks were
not repeated for this equivalence reduction. Raw initial relative-path argument
failure and stale-count failures are retained, not counted as passes.
Both preserved before binary and initial after binary requested/uploaded/published/
reclaimed1 geometry family. Old minimum2 assertion failed both despite conservation
and validation log0. Removed fixed family count; nonzero demand and completion of
EVERY actual request remain. Evidence descriptor-vfx-conservation-review.json.

Preceding verified hero batch: native smoke + BSDF2/2 pass; source shader44 pass;
slime/water HeroWork4frames each execution_valid/drain true, failed0, validation0.
Before-water CLI failed an obsolete fine-page requirement despite valid completed
fixed-root producer receipts. Keep failed raw receipt. Separate producer reanalysis
is not a passing rerun. Corrected after-water native fixture passed. Fixed root
requires actual completed228+229; fine path retains coarse coverage/seed checks.
180frame PNG differs in3/1016064 pixels, max channel1; HDR is not bitwise equal.
Slime4frame136/921600 pixels and water9093/921600 differ, max channel3.
These are limited numeric image comparisons, not full optical/temporal qualification.

Retained descriptor/VFX changes:
- CPU material descriptors retain native resource owner and destination heap
  identity by frame slot; unchanged descriptors skip GetDesc/CreateSRV and description
 comparisons. Resident publication
  invalidates cache. FeedbackFrame retains both CPU/GPU heap owners and written
  handles/stride; unchanged immutable bank skips97 descriptor writes. Current target
  lookup, metadata upload, completion/pending checks and GPU publication remain.
- Five immutable typed VFX families validate once at public frame admission.
 294 duplicate packing-validation lines and Fresnel hash set removed. Conditional
 flash previous-position finite check moved to admission (previously missing there).
 Capacity/geometry/ring/native checks remain. Ballistic/selector checks remain:
 renderer appends event-derived commands after public admission, so input differs.
 Existing owned-view fixture adds malformed attachment-history preflight rejection
 with no submissions and preserved last accepted evidence.

## Measured result and priorities

Actual foreground Playing,200 enemies,1920x1080/100%,Ultra,vsync/cap OFF,
ticks600..1750,CPU diagnostics ON and GPU timing requested in these runs:
- cluster-owner:27.8532FPS,p50/95/99/max35.78/40.92/44.04/47.49ms,1%low21.97.
- hero-warm:28.03FPS,p50/95/99/max35.50/40.94/43.54/46.94ms,1%low22.32,
 50ms+0. Same new binary/Generation as hero-kernel. Native PSO misses0.
- hero-kernel first run:21.818FPS,p99326.69/max912.79ms,50ms+14. Keep this
 failed-performance result.31 native PSO creates consumed26.88s; frame2 already
 tick916. Reusing its completed native cache removed these startup tail stalls;
 do not silently discard them or call cold startup solved.
All three completed normally; shader reduction does not establish a material FPS gain.
hero-frame-tail.json / hero-warm-analysis / hero-kernel-analysis retain full metrics.
CPU/GPU-off prior paired runs:28.096/28.072FPS. Both had CPU diagnostics ON;
not ordinary fully uncaptured play. Explicit GPU opt-out reports GPU N/A, not zero.

Latest GPU p50: GBuffer7.102ms,AO horizon3.454,Hero full serial-Direct feature9.714
(previous10.077),nearest composite2.929(previous3.188),dynamic VSM2.065,root.938.
Hero composites alone are not the full transparency feature. Other queues' pass
sums are not whole-frame critical time. Copy durations unmeasured=N/A.
Actual shader guide bytes284472->232776,SSA36598->30881,Log258->17,Exp332->47.
Concrete admitted material ordinal5 now narrows both context and evaluator; water
branch/current authored parameters/quality fallbacks retained. Schlick/Coat pow5
uses multiplication3, no new clamp. GPU53case BSDF fixture passed independent
CPU double-pow error bound8*float epsilon; existing TIR/PDF/energy bounds unchanged.

Latest descriptor-vfx actual run:1800ticks/710rendered, all710 GPU frame/view IDs
valid, selected539 CPU/GPU pairs, normal exit, crash/device_removed false, input/
presentation drops0.28.0526FPS,p50/95/99/max35.61/40.97/43.41/44.57ms,1%low22.78,
50ms+0. Versus hero-warm28.03: no meaningful average-FPS win. Ordered short samples,
not repeated performance adoption. PSO hits186/misses0. GPU/component metrics valid;
OS sampler error/coverage issues remain partial. All source702 hashes unchanged.
VFX upload-preparation p5058.5->28us; texture prep202->174us; scene_gpu_prepare
5185.5->5096us. Nested scopes are separate component evidence, not total-frame savings.
60 unused task-generated executable copies removed(1467.6MiB). Current/reference,
first failure/relevant root/counts binaries, Generations/raw receipts remain;
executable-cleanup.json lists removals. Do not rerun deleted obsolete aliases.

Six capture-only scene children exposed hero-warm CPU scene_gpu_prepare p505.1855ms:
skin.4765,shadow1.000,cluster.194,geometry demand3.1535,texture.202,upload.132.
Nested scopes must not be added to their parent. Descriptor optimization addresses
only part of .202ms, not the main CPU/GPU bottleneck. The spatial batch subsequently cut that geometry demand; repeated Hero geometry/lighting
and GBuffer/AO GPU work are the primary remaining costs. Native latency active0,
input/sim waits0,latency begin~.0007ms; no SetMaximumFrameLatency cause established.

## Retained architecture and completed implementation slices

All prior final/Profile200/N/C/PU/PC/PR/PT and latest34a8b0d ZIP/W01..W12 remain
active. References: .planning/2026-10-23-complete-directives/references/latest-34a8b0d.
Once-reviewed ledgers are not implementation/qualification completion. Public module
DAG,60Hz strictFP/RNG/IDs/order,OFF/OFF prerequisites and simulation/presentation
content separation remain. ARCHITECTURE.md is canonical. No new ECS/backend/registry.

- Content22-role family batch/LRU decode/fixed sampling; dedicated static shadow
 baker shares actual canonical scene. Generation/package/source digests remain at
 boundaries. Probe v2 identity uses semantic scene/page data, not native allocation.
 Global recipe closure and real canonical texture/LOD qualification remain open.
- Runtime canonical VFX rows/heap/world-step ACK, snapshot-pair facts, mote unit
 space; transactional accepted-Present UI packets; audio completion ownership.
 Renderer unused persistent payload copies/reconciliation/sort/handle/age/duplicate
 scans deleted; Runtime still owns real commands. Ribbon validates once and performs
 actual update once. No entity-surface path borrows actor span rather than copies.
- Gameplay CSR broadphase/canonical hits, pure Slow reads, one movement-expiry owner,
 due queues/sequence ordering; semantics version6 rejects replay5. Optional detailed
 observer retires dedup by future causes.
- Early shadow planning/lazy CSM/current view demand; stable count/coverage/fine-prefix
 caches preserve changed wind/alpha/dynamic/native inputs. GPU root scatter phases3/4
 replace CPU work. Actual CullCS native Legacy/Enhanced GBV oracle passed.
- HLOD slot bitsets/direct mapped member copy; descriptor95->6, duplicate zero-fill
 removed; receiver validation fused with actual depth loop. False Meadow ground/water
 HLOD dependencies removed; actual49 upstream inputs remain, no-op build proved.
- Graph typed value cache8 skips compiler-input/invariant-use construction on exact
 hit. Producers still declare (W11 OPEN). Current binding/device/initial state,
 physical duplicate/fence/external owner checks remain. Graph build5.172->~3.55ms.
- Cluster immutable candidate owner caches validation/upload by runtime generation,
 exact owner and static error. Active-view mask uniform at constants byte60; current
 culling/history/material/capacity/native fences preserved. ~20k refers ALL static
 objects, not a proven cluster-candidate count. Extra cache FPS gain negligible.
- Skin duplicate FNV removed, exact deformation identity retained. Static geometry/
 material demand reuses prepared retained ranges. POM ray/footprint/weight shared
 across3rotations; gradients/march/refinement retained. Native180 image exact then.
- AO homogeneous inverse-projection fast path qualified on actual GPU768cases/
 159744 pairs, max7.15e-6<=2e-5. Actual GPU AO gain negligible. Masked surface
 duplicate opacity sample removed. HeroCurrentVS excludes unused previous pose.
- Empty entire Hero contributor set skips work; current four layers remain. Measured
 all405 frames had all four layers occupied; no extra occupancy query adopted.
 Coated prefiltered branch98.83..99.62%,mixture32 .021..114%; preserve rare-path quality.
- Ordinary non-DRS rendering omits GPU query/resolve. Capture/DRS retains existing
 safe resolve. DRED ordinary follows OS policy; error-only CheckDevice retains DRED.
- Actual DFG COMMON->COPY_DEST startup transition fixed. Preflight rejection no longer
 clears last accepted diagnostics; failed calls still clear new VFX/UI ACK receipts.

## Reusable checks and unresolved work

Core17/17: Build/verification/verify-core-20261025T151649889Z-324.log.
Portable3/3 latest-portable.log; fresh OFF/OFF and RuntimeON/ToolsOFF configure pass.
Prior cluster smoke/retest2/2 and repaired owned-view3packages/9submissions passed.
Other scoped package/graph/Runtime/ribbon/query checks in existing batch logs cover
unchanged inputs; do not rerun whole gates without relevant changed input/reason.

Initial lean-run failed after383frames with Direct fence DeviceHung0x887A0006;
not reproduced/confirmed fixed despite subsequent normal1800tick runs. Do not count
that failed run as performance success. Scoped checks do not prove whole lifetime.
Full-renderer GBV previously90/120second timeouts remains incomplete.
Per-queue timestamp resolve proposal was twice rejected by automatic approval review
for race/device-hang/lifetime-proof concerns. NOT APPLIED; pending user approval.
Review-only proposed-per-queue-timestamp.patch/.md. Do not retry unchanged or bypass.
Current GPU-capture resolve still has all-batch fan-in; GPU opt-out is a different path.

Remaining W01 fixed tables/payload pack,W04 adjacent snapshot/Presentation ownership,
W05 actual per-view motion,W09 skin/bounds consumer union,W10 qualified rect,
W11 declaration-free producer path,W12 cadence/backlog/drift/DRS/recovery; native failure
and optical/temporal/stress/long-run qualification all remain. No60FPS/full-directive
completion claim. Actual monster packages currently contain onlyLOD0; a proper LOD
pipeline needs retained skin weights/seams, animated interval bounds, per-instance
all-consumer selection and history transition. No actual skinned-LOD code implemented.
