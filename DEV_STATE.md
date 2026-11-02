# Current technical state

Working main ef53ae701d695d955a37be95fbfa2f9dab9f0fe9 plus the receiver-plane
shadow correction. No branch/commit/push by agent. Overall optimization directives
remain incomplete. Root owns source integration and the sole Ninja/GPU workload.

## Active phase22 diagnostic (restore before finishing)

Application.cpp currently has a TEMPORARY append_pickup_probe helper, enabled only
by existing developer vfx_showcase: four stationary pickup bodies and actual idle
owners pass through normal Runtime/VFX/Renderer. Original bytes/hash are saved in
Build/structure-slim-20261101/phase22/application.before-pickup-probe.cpp and
pickup-probe-source.json. Restore the exact original before normal final build;
do not ship or keep this helper. ProjectHS.exe currently contains that diagnostic.

Real native failure isolated: Relic idle's admitted mask slice6/strength0.58 is
rejected by renderer_views PolarRune guard; other logged fields are valid.
First two owned runs exit1/tick1/frames0. Detailed failure string was temporary;
renderer_views.cpp has been restored exactly before worker remove_outline fixes
the actual guard/upload/shader contract. Root owns Application.cpp; worker owns
PolarRune-only changes in views/frame/project_hs.hlsl. No live GPU/build job now.
Do not equate this failure with every reported glow-only pickup. User confirms
the visual symptom persists before collection; fixed actor/idle capture continues.

Nsight GPU Trace CLI never launched the target: Qt platform integration blocked
in a MessageBox. Noninvasive CDB detached, owned ngfx27976 terminated. Its exit0
does not prove a capture; no hardware counters obtained. Local QT plugin path is
a future process-only diagnostic candidate, not an engine failure. Terrain DDS
range probe found maxima0.957..0.969; global-max skip would be small. UV-varying
sampler-feedback writes cannot yet be collapsed with equivalent demand proof.

## Current requested removal and visual investigation (phase21)

Screen-space outline rendering, shader product/PSO, post-graph branch, settings,
UI, CLI and experiment option are removed. Old saved outline controls are ignored
while other settings migrate. Existing surface-classification wire bits and pass
ID15 remain reserved; no screen-space outline is executed. Do not confuse VFX
shape borders with this removed post effect. C++ Debug apps and related leaf
targets build. Official shader/content/shadow/probe closure completes. Default
runtime-full is4167d844fea5f6e635bcfd37ade0d794028d7ba64a959660a83109d7db410c60:
2883products, removed outline_ps.dxil only, no additions. All retained render/audio
payloads match phase20; four GameData values/identities changed after removing the
obsolete authored outline rule. Do not reuse old FPS as this PE's measurement.

Pickup body projection already uses alpha255 and the opaque builtin route.
Phase20/POM-Ultra final frame contains1046opaque pickup body pixels
(Magnet735,Heal311), all base alpha255; this does not reproduce every pickup or
establish final output at the reported symptom. Idle ring/motes are distinct from
the body, current/previous owners share renderer sample time, and no indefinite
missing-owner idle-ring retention was found. Collection bursts intentionally last
0.4..0.9seconds. The reported hollow/glow-only appearance is not yet localized;
no guessed opacity/quality/lifetime change has been adopted. Keep this unresolved.

Evidence: Build/structure-slim-20261101/phase21/outline-pickup-notes.md and
Build/hero-surface-20261031/phase21-outline-build-final.log. Related CPU argument,
feature and settings-store checks pass. The settings-store test failed under the
restricted sandbox, then the identical executable passed unsandboxed; no product
change was needed for that failure. A leftover settings-label array caused the
first C++ failure and was corrected before the successful final C++ batch.
Simulation presentation-contract and runtime.integration also pass. Existing
integration.render-smoke passes180frames/resize/legacy; vfx-camera-basis passes
6frames/enhanced/BloomOff; both validation_errors0. No full Release/FPS rerun.
Shared Hero shader results were missing from gameplay/character test Generation
closures; both now declare the existing BSDF result and compose successfully.
Phase21 verification.json freezes source/PE/content and exact test evidence.
Completed native raw/DDS/PSO caches retired:864157061bytes/237files; compact
results/logs/PNG and prior phase9/18/20 references remain. No owned live job.

## Phase20 source/build and current diagnosis

SSR's five kernels now use their own screen-reflections action in the existing
material shader producer. Exclusive ShaderFamily replaces independent switches.
Product IDs, DXC options and source snapshot/hash/cook identity remain; all2884
runtime payloads are byte-identical across the ownership change. SSR-only mtime
invalidation invokes only that action (2.6s cache recheck, not real-edit DXC time).
Debug shader/bake closure passes. Release SSR leaf passes with the same5DXIL and
no Hero-work output; this is not a full Release engine/workflow qualification.

Core gate passed18/19; obsolete fixed202column expectation failed after the CSV
became199columns. Removed that unversioned pin and unused full-schema stdout;
header/value cardinality, order, uniqueness, sentinel and IO checks remain.
Exceptions now report the failing line. Only the failing leaf rebuilt/re-ran:
runtime.diagnostics-bounds passes0.42s. Other18checks retain unchanged valid
evidence. Original285s failure delay has no proven OS attribution.

Short intrusive12tick/200enemy Hero sample:6,918,584indirect entries,
127mixture32entries,6,883,679coated-prefiltered. It does not establish whole-play
frequency or cost proportional to call counts. No legal Hero mesh/material
binding was removed to specialize today's authored data. POM sensitivity at
matched offscreen360ticks: Ultra->Off SurfaceResolve3.572->1.166ms, GPU output
span23.663->19.857ms. This changes material/downstream output: no quality cut
was adopted, no exclusive POM cost or foreground FPS gain is claimed.
Evidence: phase20/review-decisions.md, split-product-comparison.json,
incremental-invalidation.json, release-ssr-identity.json, pom-sensitivity.json
and hero200-branch-summary.json under Build/structure-slim-20261101.

## Current GPU follow-up (phase19, functional checks complete)

Frozen source: Hero Exit4 draws become one selected-width float MAX
target; AO skips consecutive identical pixels within each slice/side; SSR reuses
only the original-input PDF context outside the sample loop; Cluster allocates,
clears and writes only the selected indexed/mesh stream and hoists common bounds.
Existing depth/identity/opacity/fallback, active dimensions, queue completion and
null descriptor ABI remain. Existing Hero probe covers Medium->Ultra format/MAX.
No new test runner, quality cut or permanent profiling option was added.
Old PE+8efc qualified products completed the same300tick/200enemy offscreen baseline
with300frames/assertions valid. First watched attempt timed out at Direct drain
after45submitted frames; a completed direct-child run supersedes that incomplete
attempt, not an assertion that all queue latency is solved. Combined Debug native,
shader/bake closure including final SSR simplification passed. Hero8frame
Medium->Ultra functional validation passes; original GBV raw run has errors0 and
only the PSO metadata identity assertion failed (fixed to distinguish RTV format).
Cluster indexed and mesh14frame GBV probes pass/errors0, including HZB reveal,
retest, selected stride/readback and scene replacement. Capture no longer reads the
removed indexed stream in mesh mode. No new performance claim: elevated PDH found
GF2_Exilium43.6%3D; unchanged passes also inflated in the initial300tick comparison.
SSR full-closure sharing enlarged DXIL; both that experiment and the unconditional
temporal tile were removed rather than adopted without fair net-gain evidence.
Final1080p Ultra100%,200enemy300tick/300frame validation-ON playback passes, same
gameplay/particle facts and30/36byte-identical raw buffers vs the baseline. Remaining
six outputs differ in sparse floating values; composite90/8294400channels differ,
max0.0029296875HDR. No nonfinite values. This is not full-frame bitwise identity.
Existing integration.render-smoke passes180frames with legacy barriers/resize and
validation_errors0. The25Hero/Cluster shader products used by native probes are
byte-identical after SSR simplification. No redundant GBV rerun for those products.
Evidence: Build/structure-slim-20261101/phase19/gpu-followup-result.md,
final-qualification.json, native-shader-reuse.json and default-adoption.json.
Completed phase19 raw/duplicate images, copied PE/DLL and per-run PSO caches were
deleted:2,274,951,981bytes. JSON/CSV receipts, tiny selected-command readbacks and
one final PNG remain. Prior valid foreground/phase9 evidence is untouched.

## Current shadow correction and content

Opaque deferred lighting derives a geometric light-space receiver plane from
world-position fine derivatives. Metadata integer halves and validity derivatives
reject incompatible pixel neighbors without extra texture reads or wave operations.
Fine/root VSM compares each actual clamped texel center against that plane, including
receiver fractional offset and reversed texture Y. Existing bias is unchanged.
Point, volume, translucent and CSM fallback paths retain their existing behavior.
Same metadata does not fully distinguish separate surfaces/depth discontinuities.
No GBuffer channel, native binding, registry, test runner or diagnostic flag was added.
Terrain and local_probe_sampling are restored to HEAD; temporary bypasses are gone.

Official final shader/shadow/probe closure passed. Deferred DXIL has all16 fine
derivatives after world-reconstruction reconvergence and before coherence branching,
with zero QuadOp calls. Final native120ticks/120frames,1080p Ultra100%,200 enemies,
regular validation ON exits0; assertions/execution valid, errors0, no device loss.
Terrain prelight inputs and shadow-bank inputs are identical to the reference;
one nonterrain base pixel differs1/255. Baked SPB1 and all5payloads are identical.
The two measured original stripe-frequency amplitudes fall94.9%/73.3%; cart/fence
occlusion remains. This qualifies the observed patches, not every image artifact.
No new FPS/cost measurement, Release/full workflow or CSM/boundary qualification.

Evidence: Build/structure-slim-20261101/floor-noise-20261102/floor-noise-result.md,
receiver-plane-analysis.json, receiver-plane-source-freeze.json and
receiver-plane-dxil-check.json. Phase19's tested source/content is historical;
current ProjectHS/content is phase21 above.
Current tested scope runtime-full-gpu-phase19 is06d135a869c694b0262f24885232cd94e392040e03d82b27e77748f4f5dbec10.
Phase20 default runtime-full was49215cd52a8b95fb47d16b2aefd9c0f27fd1066bc0c9b800982155ad9723dbd9.
Scope/action ownership participates in Generation identity. All2884logical product
IDs, sizes and digests equal the measured9232a511 generation. Phase20's
default-adoption.json records this comparison; phase19 records the earlier scope
comparison and executable hashes. Never substitute diagnostic pointers.
Measured phase20 runtime experiment PE SHA0a1675b1a0a19b96c5cf68d8bca8aa668e39759a18ac60ac3943dd6cbf811929;
FNV7f2b64b8b5a05590. Simulation content85a0f257edd1ce70.

## Static GI and preserved structures

Baked static indirect lighting uses existing SceneProbe SH diffuse and prefiltered
reflection packages. Dynamic indirect bounce/occlusion and Brixelizer/SDF/history/
seed/SDK build/install paths are removed. Direct lights, dynamic shadows, SSR,
environment fallback, translucent slimes and Hero4 layers remain. Raster geometry
has no SDK position suffix; deferred lighting has3outputs/2optional auxiliaries.
GI modes Off/Probe; legacy modes migrate to Probe, obsolete gi_quality is ignored.
Normal Baked runtime does not recapture; baseline light/env facts are retained after
static/material validation. Explicit Layout tool/test capture remains separate.

Named MaterialShaderOptions fixed the prior positional DXC flag regression that
bound a diagnostic counter UAV in normal Hero shaders. Normal+GBV probe bake,
existing6CPU checks,2Layout native checks and multiqueue GBV passed at the preceding
static-GI checkpoint. These are not a new full-workflow/Release qualification.

Retained executor reuses immutable resource membership/external-pass facts; current
native binding/state/alias/fence/partial-submit rules remain. Full retained planning
and compatible command-list grouping are incomplete. Static demand-point reuse is
implemented; streamed errors/radii and foliage wind remain live. UI content reuse
is independent of raster and Present/click acceptance. VFX identity/borrowed recipes
retain dynamic payload checks and activation/native/Present ACK boundaries. Gameplay
keeps60Hz, strictFP, RNG/order/checksum and public module/OFFOFF contracts. Release
omits diagnostic logging/replay/CSV/PIX helpers; no later Release rebuild is claimed.

## Latest foreground profile (phase20, before outline removal)

phase20/foreground2400: Debug /Od,1080p Ultra100%,200 enemies, Hero4,
RTX4080/driver32.0.16.2101, validation OFF/timing ON; all1497frames foreground,
2400ticks complete, exit0. Measured Generation9232a511 and phase20 runtime PE;
later producer/CMake ownership edits do not alter any of its2884payloads.
Warm1195:39.829FPS,1%low30.162, interval25.108ms,p9528.521,p9931.261.
Calibrated GPU output-dependency span24.821ms, union22.803, uncovered2.018ms.
Uncovered is not idle and union includes markers outside the output closure.
HeroComposite4.067,SurfaceResolve3.355,GTAO1.584,HeroExit0.266ms means;
queue/pass sums are not whole-frame totals. CPU render23.216ms includes slot
wait8.288ms; constants3.548,scene prepare2.289,graph build1.814,record1.100,
submit1.873,plan1.644ms are nested wall scopes. Gameplay1.058ms/60Hz tick.
1176/1195submits precede previous GPU output-ready: GPU throughput is the primary
average limiter, shader busy time unknown. Present-return cadence39.810FPS agrees.
Full96GPU groups plus sorted CPU/simulation/audio/VFX timers and provenance are in
phase20/foreground2400/latest-foreground-profile.md. All1497GPU diagnostics valid,
drop0. Phase18's40.459FPS is a different checkpoint, not controlled A/B evidence.
60FPS has not been achieved. Computer Use kernel ended immediately after activation;
no owned control helper remains. Existing app-managed helper was retained.

## Remaining work and investigation limits

Hero Exit reduction is implemented and current foreground aggregate is measured;
its causal before/after gain is not qualified by a controlled A/B.
Continue retained execution planning, GPU UI backend and other measured high-cost
work. Cone/POM bounds and AO hierarchy/swizzle remain conditional quality/locality
research, not unconditionally required code. Terrain traversal/representation and
the common coated Hero path are current priorities. Averaged BC4 height mips are
not conservative bounds. Retained frontend misses include38low-level hits and
141real compiles; geometry-page membership is a hypothesis, not proven by CSV.
SSR action ownership is now split and checked without changing runtime payloads.
Low-level OFF/OFF gate has18unchanged passes plus the corrected leaf pass above.
Full Release engine/workflow remains unrun.
Do not repeat passed narrow checks without changed inputs or an unresolved risk.
Audit remaining obsolete SDK build-cache residue. In the next C++ batch, remove
the stale software-scene entry from application.cpp runtime_capture.cpu_scopes.

Probe radial-hit/receiver-ray comparison has a real approximation problem; native
visibility-only bypass preserved this observed stripe. Surface-aware correction
needs geometric normals and point/volume semantics; do not blur distance maps,
increase bias drastically or permanently bypass occlusion. Authored vertical fade
also weakens floor probe cover and must be considered separately. Old12/9m terrain
periods differ from authored2/1.5m since initial import; tested scale/near-single
trials did not resolve the stripe and are not adopted. Dark linear8GBuffer precision
is a separate unqualified candidate, not the proven cause of this stripe.

Keep phase9/original crash evidence and latest valid foreground timing. Completed
floor diagnostics retain compact provenance/analysis and selected reference buffers;
unused raw/PNG/cache is retired with scoped receipts. Do not fabricate Generation,
ActionResult or product identities. No owned live benchmark/Ninja session remains.
