# DEV_STATE

AI implementation continuity only. Original main workspace, HEAD
0546e32ae200ef390b6b46af367b93b89b30fc82. Preserve existing dirty work/external
deletions. No current commit/push/branch authorization. Prior expanded state:
Build/reanalysis-priority-20261031/DEV_STATE.before-priority.md.

## Latest completed audit

Ponytail whole-repo scoped audit+authorized fixes complete:
Build/ponytail-audit-20261031/AUDIT.md, RESULTS.md, final-state.json/source.patch.
Five concrete findings fixed: ActionResult/UpstreamProduct share a ProductSet
GenerationFileLock anchored by stable per-set sidecar; receipt/pin acquisition is
coordinated with publication/GC, busy repairs/GC retained. No shared-owner forced
release in Publish. CharacterContent cooks FBX/animation/FBM and monster PBR from
private copies authenticated against Action input digests; no live-path fallback.
Dot-only experiment scenario names rejected before artifact cleanup. Experiment
child starts suspended, joins kill-on-close Windows Job before resume; termination
and exit outcomes reported. Generation selection briefly holds an existing shared
root mutation lock through manifest lease acquisition (create=false; no writes to
immutable installed snapshots). Retry wrapper was removed; no public API change.

Debug selected targets/app and prebuilt Release build pass. Existing Runtime,
selected-generation, scene-HLOD/action-snapshot, architecture and real generation
lifetime/GC/repair cases pass; final selection+real lifetime suite rerun passes.
Actual slime-melee4products and monster-materials3products retain every digest/size;
producer keys differ, snapshot directories are not published. CPU experiment normal,
invalid '..' sentinel preservation, parent hard-exit child cleanup and suspended-child
watchdog exit-confirmed all pass. Process-fault proof precedes the final selection
coordination-only relink; final normal180tick CLI also passes after that relink.
No new executable/runner/dependency in production, no Linux/CU/GPU/full cook/install
qualification repeated for these offline/startup changes. No current owned child
process left. Current experiment/app hashes are final-state.json; prior GPU/FPS
receipts belong to their older exact PEs and are not current binary qualification.

Audit scope: no new actionable renderer/gameplay/VFX issue found in inspected
paths, not exhaustive safety proof. Deep native audio/PCM, every UI callback,
every producer and shader math were not audited. Earlier optimization and numeric
exposure variability work remain ACTIVE below; audit completion is not60FPS or
full-directive completion.

## Active objective and earlier priority

Goal ACTIVE: finish accepted directives; actual Debug /Od/assertions/checked
iterators,1080p Ultra100%,200enemies toward60FPS remains unmet. Highest priority:
Desktop/Project-HS-Reanalysis-0546e32.md, all381lines read, SHA256
31d5f64cf8d98678fa369443ea15633f4ccdcee96886882b9433ff43be7a44bf.
Its fixed0546 source and historical5879/cc900 measurements are not current PE.
Earlier two workload reports remain W01-W08 in
Build/hero-crowd-20261031/workload-backlog.md. Do not replace the full queue with
this report. .planning was externally deleted; do not restore unrelated deletions.

Use existing narrow checks/batched builds; no permanent comparison tools, extra
runners, disconnected/source-spelling tests. No Linux/CU. Root owns builds/one GPU
workload; subagents Luna/high, no Sol. Preserve translucent slimes, full transport,
4Hero layers, full quality/resolution. Release excludes developer capture/trace/tests.
Keep Public layering/OFF-OFF/60Hz/strictFP/RNG and accepted-prefix/completion/borrow/
old-reader lifetimes. VSBuildTools14.44.35207, existing Tools/CMake4.4.3. Existing
renderer_frame /Ob2 is not permission to enable /O2 Debug.

## Priority report disposition

- A01 one move-only SceneGeometry preparation and G01 actual Hero domain codegen
  are implemented; do not redo. G01 11PS DXIL -16.05% is not FPS evidence.
- A02 first increment: full ordered logical records stay for native GI; only
  SceneGeometryDeformed rows enter GPU records/draw upload/descriptors/dispatch.
  NoD skips unused table/import/pass while preserving CPU rows, owners and pending
  completion. Existing scene-input test calls production planner for mixed static,
  SkinCache and deformed rows, checking offsets/order. Full static/current section
  input architecture is incomplete; do not mark full A02 done.
- A03 eager native SDK batches remain rejected:2.100ms control vs2.163/2.147ms.
- G02 one controlled existing terrain producer trial (isolated copy) completed:
  control82424B matches selectedDXIL; candidate82436B, same9159instructions,
  28SampleGrad/3SampleLevel and linearSSApeak176. No demonstrated reduction;
  not adopted, production terrain never edited. terrain-trial/trial-review.md.
  Not hardware register/occupancy proof; other terrain improvements unresolved.
- T01 fixed proven topology churn: SceneGeometry uses64KB owner-local buffer
  widths consistently for creation/preflight; volumetric CPU-only upload resources
  have whole-buffer graph Read declarations while actual waited-slot CPU/CBV/SRV
  ranges stay exact. No cache enlargement or dependency/fence deletion. Temporary
  FirstMismatch capture identified the causes and was removed. Restoring a file
  via Copy-Item preserved an older mtime and Ninja reused its diagnostic object;
  force-touched/recompiled clean render_graph.cpp and relinked. Current experiment
  has no diagnostic literal; native runner relinked clean after prior run exited.

## Current source/content and checks

Selected Debug SDKON generation9750a33dc4e29081ffb464b2dce7a37ea9776018e9947ee092815ccece73fe05,
pointer GenerationState/runtime-full-pixel-front-20261031.json. Versus da0 only2
normal HeroPS differ; all3639product IDs, CS/nonshaders/bakes/ABI match. Redundant
normal Composite/Guide front-depth SRV equality removed: same read-onlyDSV EQUAL
already applies. Identity check stays (diagnostic9620validrows:0depth rejects,
467identity rejects); Tail predicate and instrumented variants unchanged. Front
world21 and offscreen200x600 pass, no isolated whole-FPS benefit claim.

Current source diff freeze8ff1b3ef571261322f55348ecb48913401e738361957235df19010d69d6a821d:
Build/reanalysis-priority-20261031/t01-final-source.patch and t01-final-identity.json.
Clean experimentFNVbd5fcf031a22e7b7/SHA
c546854534bf2737135c07fd126959c72d6b75fa9f9383555dcc96b538d9862b,Gen9750.
Debug app/experiment/native runner/existing scene-input builds and CPU case pass;
Release ProjectHS SDKOFF/ContentToolsOFF/TestsOFF C++ build passes. No new cook/install.
Final world21 normal D3D validation: execution-valid/errors0, capture/sample-ready
local probe and required transport passes complete. The shortened7frame check failed
only local-probe readiness with capture textures still pending; retain that receipt,
use21matching baseline rather than weaken readiness. Two final normal200x600 runs
are execution-valid/assertions pass/no device loss. No latest foregroundFPS claim.
Software-GI GBV360s and Probe-GI transport GBV240s attempts lack a completed-frame/
result receipt and were stopped: INCOMPLETE, not pass/hang proof. No owned GPU
process left. The240s attempt used interim diagnostic-linked native PE; don't
relabel it as the final clean PE. Three-frame argument attempt failed before GPU
startup because fixture requires minimum7. No further blind GBV repetitions.

Matched481ticks120-600/config2/view1: A02-only vs finalT01 compile145->48frames
(-67%), all48 also A02 compile frames. Per-compile mean8.431->8.568ms, not faster
compiler. GraphPrepare mean3.057->1.352,p9913.588->10.521ms. Render mean40.932->40.622,
p9555.296->52.480,p9966.346->63.781; Wait13.119->14.416. No whole-FPS attribution.
A02 GPU upload compacting itself: oldtwo controls20us->7us; deformation64->63us.
SoftwareScene/Brix input/native CPU means remain about1.97/1.77/2.22ms.
T01 grouped HeroComposite3.892ms, SurfaceResolve3.926ms vs A02 3.702ms; no GPU gain
claim. Earlier control repeats Resolve3.866-3.929ms. Sparse GPU telemetry observes
P0/2805/11201MHz only36-40%window, not continuous external-load exclusion.
t01-comparison.md/json and a02-normal-comparison.md/json retain all distributions.

Numeric output is not blanket-qualified bit-exact:27/36 surfaces exact including
depth/GBuffer/lighting/AO/guides/light lists/VSM. Composite maxabs.00390625,
mean2.855e-6; current exposure relative difference~2.25e-5, histogram55bins differ
(max43counts), total2073600samples unchanged. Oldsame-PEA1/A2 exposure exact;
same-final-PE repeat also has27/36exact and~1.8e-5relative exposure drift,55histogram
bins/max44counts differ, total2073600samples unchanged. Both repeats have the exact
same48compile frame IDs and338/481frontend reuse. This establishes variability
within the final PE, not source-caused drift or bit-exact equivalence. Do not infer harmlessness or root cause
from a small global exposure shift alone. Native final visibility IDs/metadata/
geometry/cluster exact in A02 comparison; worklist scatter order differs with same
per-variant sets. Current full raw comparison remains scoped to recorded snapshots.

Next: continue measured static/current section
packing and remaining graph shape growth; Hero/terrain GPU bottlenecks remain.
All earlier accepted/rejected technical decisions below stay in force.

## Rejected current trials and retained scope

W04 GPU cooperative triangle cache removed; visibility_buffer.hlsl matches HEAD.
User-requested monitored A/B/A (samecb09PE,Gen9750 vs candidatec6be) confirms
SurfaceResolve3.929/20.519/3.866ms over481commonframes, candidate5.27x slower.
All valid/sampledP0clocks; sparse telemetry does not fully exclude other processes.
Build/pixel-work-20261031/RETEST.ko.md preserves source/evidence; no blind retuning.
W03 closed HeroCurrentVS union also removed: DXIL28112->20200B but Hero18
6.7072/6.8045/6.7221ms; temporal guides8208pixels and HDR/exposure differ.
hero-crowd-20261031/RESULTS.ko.md. Normal native slime/water pass; GBV180s attempts
incomplete before receipt. Do not duplicate earlyZ, delete tiny background copies
or remove mixed monster opaque-feature GI to claim the main bottleneck solved.

Next: finish A02 GBV receipt, diagnose T01 exact cache miss and fix proven cause,
temporary capture is closed valid600frames and logging source restored. It identifies
VolumetricDensity resource-use and SceneGeometry.RecordUpload width differences.
Local64KB allocation-granularity buffer widths applied consistently in preflight
and allocation; build/performance pending. Continue measured static/current work.
No latest foregroundFPS/60FPS or full long-run/multiview/failure qualification.
Remaining broader technical decisions/evidence are retained below.

Adopted measured work (scoped receipts, not every current function qualified):
- Dense ordered graph batch membership: GraphPlan3.75->1.94ms; required completion,
  aliasing and first-use order stay. Build/profile-opt-20261031.
- AO full-resolution inverse-W preparation reuses metadata write-bank scratch.
  Prepare+horizon~1.64ms versus3.29ms; hardware oracle24cases/2642pairs/zero bit
  differences, plus native GBV. Prior history/fallback projection rules stay.
- LocalShadow task overlaps software-scene prep on existing executor; all paths join.
  Static Brix preflight decisions reused for mutation. Resources~9.3->~7.0ms in
  both foreground orders. Build/resource-prep-20261031; native GBV/Release pass.
- Retained GI leaf keys sorted with original read-pair ordinals preserve duplicate
  first-match semantics. Shadow receiver reserve and affine camera transform reuse
  accompany it. Both orders improve modestly; native std/GBV and Release install
  pass. Build/cpu-binding-lookup-20261031. No isolated per-edit FPS claim.
- ClusterCutCount combines child completeness/residency/demand walks. Catalog and
  Ancestor fault bits, later valid demands, Prefix/Emit lifetime/overflow stay.
  Same-PE two-order GPU Count .921->.803ms and .894->.823ms; overall~34.3FPS flat.
  Full-arena21-frame pairs cover desired-child path:39360demand words, statuses,
  draw args exact. Temporary readback export removed. Debug/Release/install/app
  smoke pass. Build/cluster-count-20261031. No malformed-catalog native injection.

Current adopted CPU cleanup: Build/dirty-ranges-20261031.
- Batched dynamic union/six-channel dirty ranges use one ascending slot walk,
  replacing seven scans and old overloads. Preserve pending/committed revisions,
  inactive holes, coalescing and per-frame-bank scratch lifetime.
- F01 AddRef->vector exception gap is closed by local shared ownership before
  moving into the existing retained owner vector. No completion/retirement change;
  not a historical-hang diagnosis, no allocation-failure injection performed.
- Debug app/experiment/native runner build and existing persistent-scene test pass.
  Full-arena SoftwareGI/Visibility21frames pass,validation0.
- Experiment FNVb198cbfc3d4154b9/SHA c1bc48558b0affa347427642d56fb573e5f70d32b8d0abe8ea85885a8fb8bd1f.
  Same Gencc900, foreground1800ticks receipt valid. Preceding preserved bd7 control
  vs candidate: upload-plan .083->.053ms, observed33.978->34.472FPS. Upload planning
  was already only~.084ms; do not attribute overall FPS difference to this cleanup.
  Final Release source build passes with the static-transform batch below;
  unchanged installer/package checks were not repeated.

Current adopted checkpoint: Build/static-gi-transform-20261031.
Cache exact static world matrix and std::sin/cos yaw in SoftwareSceneBase;
source companion retains optional pointer within synchronous RenderView.
Dynamic transforms, page/generation/residency, skin/wind and page bounds stay dynamic.
Extra72bytes/cache row and8bytes/job source record (~1.34MiB at19515static rows).
Control frozen b198 alias; candidate FNVef8f3f288ed13723/SHA b4ece06f...95fb6.
One candidate playback bracketed by two controls, not two independent candidate
repetitions: Brixinput1.911->1.752ms and1.919->1.759ms; software-scene unchanged.
Observed34.541FPS; controls34.472/33.952, tails mixed. No whole-FPS causal claim.
Debug builds/native SoftwareGI21frames pass,validation0. Final Release source build
passes SDKOFF/TestsOFF/ContentToolsOFF, PE SHA71297477...B16E2. No extra full cook
or install repeat for unchanged package/install inputs. Exact hashes in final-state.

Rejected candidates (source removed; do not repeat same designs):
- Surface header/GetDesc sharing: raw tested outputs exact, no repeatable GPU gain.
- Per-actor whole-unsupported SDF early filter:~34.196->34.197FPS flat.
- Terrain direct GBuffer late/early forms: Resolve cheaper but raster much more
  expensive; early34.322->32.239FPS, p99 worse.
- R8 visibility classification tag/ABI6: tested outputs exact, Resolve3.653->3.655ms,
  FPS33.953->33.925. ABI6 and seven-file trial removed.
- Plan memberwise swaps+buffer merge+lookup PMR combined: two-order FPS mixed,
  candidate1%low and p99 worse both times; all three focused deltas restored.
  Build/cpu-container-20261031. Do not infer isolated edit effects from the batch.
Evidence directories with those names retain source, receipts and raw measurements.

## Remaining measured bottlenecks and unavailable evidence

Current major components: Resources~7ms (software scene~1.9,Brix input~1.9,
native prep~2.1), ConstantsUpload~3.7ms (shadow~1.1,geometry demand~1.0),
SurfaceResolve~3.7ms; Hero composite individual~.39/.65/1.11/1.88ms,
AO horizon~1.6ms plus prepare~.03. GPU queues overlap; never sum passes into busy
frame time. Render/record are composite CPU spans, not pure busy time.
Actual sampled main thread used92.48%one core over3s, despite low total CPU usage.
Twelve CDB snapshots locate work only; they do not establish profile percentages.
Build/cpu-main-sampling-20261031 has identity, thread counters and private stacks.
Frame-slot/skin waits match actual submitted completion; do not remove them.

Official NsightGraphics2026.4 extracted under Build/tooling. Earlier ordinary-token
counter access failed and Windows RunAs was cancelled. User readiness is now
confirmed; root launched an actual admin capture of frozen ef8/Gencc900.
admin-current-throughput first lacked output dir, then profiler window injection
clipped windowed1080client to1061. Both failures preserved. Current retry is
admin-current-borderless: exact1080 monitor, same quality, diagnostic only.
Admin-borderless reaches Presenting and initializes activity/sampling settings,
but fails before first rendered frame/capture activation: swapchain owner IUnknown
differs from renderer device. No hardware counters collected, ngfx exit1.
Canonical direct GetDevice(IUnknown) removes the typed-interface/QI intermediate
and matches graph/allocation code; the same strict comparison remains. It passes
native21 validation0 and ordinary full1080 borderless120ticks, but does not fix
Nsight capture. Current Debug experiment5879/cdbdec68...ea738, game0a71d1f1...66be3,
Releasea25dfd52...7f2cb; builds pass. No fresh FPS claim for this query-only change.
Exact identities/source patch at gpu-hardware-20261031/owner-query-final-state.json.
Do not suppress foreign-device checks or use adapter LUID as device identity.

External ETW now succeeds without injection: etw-borderless/dxtiming.etl943718400B,
custom D3D12 0x5F7/DxgKrnl0x4847 profile enabled before process creation.
Same5879/Gencc900,1080Ultra100,200,1800ticks838frames,exit0/execution/assertions pass;
rendererPID20352,supervisor32916. Recorder.complete=true and WPR is stopped; no
owned games/profilers/builds remain. Ordinary windowed1080 also produced1061client
in this environment, so do not attribute that failure specifically to Nsight.
The failed startup-only ETL55.6MB was removed with cleanup manifest; logs preserved.
Offline decoder finds3,250,201history rows. Perfetto sample's64-lane cap drops116621
slices, so no complete interval union is yet valid. Investigate lossless offline
export rather than infer GPU idle from this lossy view. ETW execution windows are
not physical hardware utilization or a sum of overlapping pass durations.
The temporary local tool exports every calibrated callback before that display
cap:3,035,087numeric rows,3queue definitions, buffered IO/counts complete.
Rawhistory difference215114 exactly equals107138 BeginSubmission+107138 EndSubmission
+838 Present markers. QPC-aligned steady foreground tick600..1800 window20.056781s:
observed callback union16.778665s(83.656%), queue sums17.357701s with.579036s duplicate
overlap. Negative0, zero286974, max11.368ms; no long trace-end tail. This is observed
interval coverage, not physical GPU utilization/critical path. DXTIMING_RESULTS.ko.md
contains exact rows/clock proof/limits; keep the original ETL and lossless CSV.
No global counter policy/clocks/Codex config changed.
Existing ETL lacks D3D12/history detail for GPU busy proof. Hardware capture status
is Build/gpu-hardware-20261031/STATUS.md. Surface architecture needs better evidence.
SSR sample.pdf reuse rejected: layered normalization differs from original Pdf.
AO duplicate tap skip is exact in source but real off-center duplicate rate is
unknown; existing captures lack exact projection/frame-phase constants. No change.

StaticGpuScene already owns base classification/draw records. Keep actual page
binding, deformation, ABI, frame resource ranges and old-reader lifetime dynamic.
GI MRT, compact feedback reduction, HZB grouping and material-use domain remain
conditional on equal-quality whole-cost evidence; no new measured benefit yet.
Old WARP VSM GBV byte39/44 issue remains unresolved, not newly reproduced.
Past intermittent hang receipts are checkpoint-specific; current bounded checks
do not prove every multiview/failure/long-run case or a cause of historical hangs.

## Preserved contracts and remaining directives

Native SDK handoffs restore engine bindings. Volumetric compute owns its narrow
41-descriptor table; no diagnostic global queue drain. CBV-volatile trial removed.
Immutable character uploads use COPY_DEST->COMMON->enhanced SRV; read contracts,
completion and allocator backing ownership remain. SDK1.1.4 bindings load selected
immutable Generation DXIL; runtime has no SDK shader compiler/producers.
VSM indexed instancing/ABI5 stays; FL12 required typed-store support is not a new
older-GPU restriction. Main package/current/previous/Present identities stay separate.
VFX prepared recipes/indexed store/semantic pair identity and distinct CPU/native/UI
ACKs stay. Moderate cosmetic overload protection retains essential warnings.
Software mip transactions retain published fallback until final publication; keep
mapping-only admission, chunk completion/cancel/generation and budget boundaries.

New two-report27-row ledger: .planning/2026-10-23-complete-directives/senior-gpu-e448dc4-backlog.md.
Originals and reference evidence in incoming-senior-gpu-e448dc4; no embedded code run.
Prior accepted/disposition records in that planning directory: remaining-work-20261027,
directive_status, review-bdf8944-20261028, review-011ef96-20261027,
focused-review-011ef96-20261027, ponytail-deep-review-011ef96-20261027,
integrated_status, queued-51d5055, structural-audit-d76d5a4 and references.
External D1-D4 structure generator unavailable; do not mark complete.
Build-cost lead remains: restored ActionKey misses latest result and reruns DXC
before Publish finds retained immutable ProductSet. Prefer existing verified
publication/receipt adoption, not binary-identity removal or a new cache service.
QueueExecutor final-only state publication was reviewed and rejected: accepted
prefix/lease fences and intermediate external callback visibility require the
current boundaries. Do not replace them with a failure replay journal without
distinct cost evidence; resource-publication-next-review.md records the limits.
Static full-array snapshot reuse also lacks a safe narrow current-span/page/owner
boundary. static-batch-next-review.md describes a small per-page leaf-recipe lead,
but its share of1.75ms input cost is unmeasured; do not build another low-value
cache solely from the parent timing or repeat previously rejected finite caches.
