# DEV_STATE

AI implementation continuity only. Current base:
ed75a66 (Harden runtime and renderer workflows), committed externally while
Release exclusion work was in progress. Current branch: main. Preserve the user's
commits and dirty files; no agent commit/push. Use main unless isolation is needed.
The dirty worktree is authoritative. Preserve unrelated changes and
.codex-remote-attachments/. No new commit/push/worktree is requested.

## Objective and execution policy

Goal ACTIVE: finish accepted reviews/directives and Debug /Od/assertions/checked
iterators,1080pUltra100%,200enemies,60FPS. Not achieved.
Latest priority: continue measured core bottleneck improvements with tests, plus
the module-wide review of unnecessary checks/dependencies and invalid states.
Do not simply remove guards or add managers/checkers. Keep actual data/ABI/
lifetime contracts, actionable failures, requested profiling and game control.
Current work: Build/frontend-qualification-20261030/RESULTS.ko.md.
P0: prior SDK-OFF 200-enemy DEVICE_HUNG reproduced on Visibility and Compact,
also with GPU timings OFF; CBV-volatile trial failed and was reverted. After
immutable texture state fixes the same SDK-OFF Compact control now passes three
consecutive1800tick runs. This is improvement evidence, not proof of complete
intermittent-hang resolution. DRED's active volume dispatch is not root cause.
Accepted earlier report: Build/further-optimization-20261030/RESULTS.ko.md.
Earlier GPU/module pass: Build/additional-gpu-20261029/CONTINUATION.md.
Earlier logging/test cleanup: Build/test-prune-20261029/RESULTS.ko.md.
Release exclusion evidence is a prior source checkpoint and does not qualify
subsequent edits. Latest Debug200 timing is recorded below from actual foreground flags;
it does not qualify fullUltra GI or60FPS.
Prior test/Desloppify/GPU analysis remains valid only for its recorded source.
The foreground Debug200 run in
Build/foreground-200-20261029/ completes1800ticks/799frames, exit0. Selected
foreground587frames/19.791s average29.609FPS,1%low27.108FPS (slowest6 intervals).
Actual GI is Probe fallback, not fullUltra GI; target60FPS remains unmet.
Batch affected checks;
retain distinct behavior/lifetime/ABI contracts, remove source-shape assertions and
disconnected models. No new permanent runner without an isolation/dependency need.
Minimum CPU-memory calibration remains checkpoint evidence, not a global cap.
Latest VFX steering is moderate: keep existing simple overload protection and
normal authored output; essential attack/risk warnings remain protected. Do not
expand whole-VFX maximum proofs/managers or remove all capacities. The temporary
request to remove every limit was superseded by that choice; no quota removal
was implemented. The prior200-enemy capture admits34781/34781 CPU birth rows and
5118/5118 profiles, zero budget-limited frames, sampled logical peak12.3MiB/64MiB.
Subagents: explicit Luna/high only. No Sol subagents. No Computer Use session.
W12/G02 deferral was explicitly lifted by the user on2026-10-29; queue-local
timestamp/readback and output-dependency interval changes are implemented and
pass the integrated build/native checks. No global timing wait is added.
Linux execution is removed by user request: Windows-only CI and presets.
Keep Windows low-level/portable checks; no Linux qualification remains required.

## Current implementation

- Current qualification: pinned SDK-OFF Debug1080pUltra200 control fails on
  Visibility (57frames in repeated OFF runs), Compact/Enhanced (183frames),
  GPU timing OFF (128frames) and CBV-volatile trial (133frames). The flag trial
  is reverted. Compact/Legacy passes one1800tick/721frame run, not a resolution.
  Validation+GBV existing lockstep600ticks/600frames passes validation0 and
  light1/decal4 coverage. The GBV child process and explicit shutdown completed;
  neither validation-ON success nor timing OFF failure resolves OFF stability.
  Failure-only DRED now emits active/pending before completed nodes, capped128,
  with complete counts and no new allocation. Experiment spec exposes the
  existing lockstep switch; existing runtime.integration passes1/1.
  After fixes, pinned SDK-OFF Compact control15D728DD...B7E77 passes3/3 runs,
  each1800ticks with actual light1/decal4, zero unsupported/dropped VFX. Full
  own-frame GPU coverage is complete; utility sampler was not launched for these
  controls, so analysis is partial and GPU utilization/critical path unavailable.
  verify-core passes19/19 and final Release SDK-OFF renderer library builds.

- Optional Brixelizer SDK1.1.4 is prepared/verified; msvc-debug cache is now ON.
  Full shader tables link only into the build producer; runtime links generated
  immutable bindings/size/SHA and reads selected Generation DXIL. All732 entries
  and7binding groups compare equal to official tables; invalid5keys rejected;
  repeat emission is byte-identical/mtime-stable. No runtime binary catalog or
  generic manager added. Runner link excludes full tables; current exe27231744B.
  Source define scoped to2TUs, producer action.cpp duplication removed. PDB error
  was restricted worker execution; actual host /Zi build passes. Removed temporary
  /Z7 workaround, preserving Debug flags (existing frame.cpp /Od+/Ob2 retained).
  SDK RGBA LUT sample reads.xy. License uses source/brixelizer, not GPU resources;
  optional install carries authoritative MIT license. Removed duplicate provenance
  product: existing Action receipt already owns all inputs/products. All749retained
  IDs/digests/sizes/dependencies unchanged, metadataF1CC3B...A11281 unchanged. This
  also avoids producer-only provenance entering semantic image identity.
  Initial10629 SDK-ON validation errors were traced to packaged immutable character
  arrays left in legacy SRV state and repeated Hero ShaderRead/GI AllShaderRead
  transitions. Upload now uses legacyCOPY_DEST→COMMON→enhancedSHADER_RESOURCE before
  static bindings, retainingAllShaderRead at all family consumers. Existing texture
  helper no longer treats COMMON/PRESENT's shared zero as never-accessed; real copy
  work remains synchronized. SDK restore bindings follow the external handoff.
  Actual SDF/GI/material/cache fixture passes21frames validation0; GBV also passes
  21frames/explicit shutdown/terminal exit0 in255.29s. No barrier/descriptor guards
  or static flags were weakened. Final SDK-ON ProjectHS/experiment/runner build;
  cache ON, current runtime-full56189af3...9411, old control pointers preserved.
  FullUltra200 SDK-ON baseline completes1800ticks/610frames, validation0. Stable
  graph_resources CPU15.866ms and CSM GPU6.488ms identify the next bottlenecks.
  CPU candidate reuses per-slot8,397,504B baked-update descriptions while exclusively
  owned (borrowed owners get a fallback), reusable flat wanted-key vectors and
  once-per-geometry validation/key collection. SDK-required full clear is retained;
  persistent CPU capacity increases by three descriptions, not a memory saving.
  Camera-GBuffer GI seed uses the camera receiver guarantee; its prior unmatched
  SoftwareGI receiver bit forced CSM despite complete baked coverage. Actual
  froxel/world receivers and all guarantee/readiness checks remain. Existing CPU
  render-package/feature/Brix-input tests3/3 and native21frame validation0 pass.
  The combined candidate then fails DEVICE_HUNG after238frames (last render tick720)
  in native-ultra-after-cpu-csm. DRED active VolumetricDensity is not root-cause proof.
  Do not publish failed-run FPS or qualify the candidate. Existing experiment config
  can now override volumetric quality alone for isolation; runtime.integration1/1
  passes. Volume-OFF-only isolation3/3 completes1800ticks (763/762/756frames).
  FullUltra candidate retry fails267frames, and Legacy barriers also fail
  (native-ultra-candidate-legacy-1). Thus Enhanced barriers alone are not the
  explanation; do not switch defaults as a workaround. All failed captures remain
  partial (missing GPU tail); complete OFF captures are partial only for unavailable
  whole-device NVIDIA samples. Full-quality qualification remains. Latest60FPS is
  unproven. Completed SDK-OFF
  GBVraw72files/1249818712B removed; evidence retained.
  CPU candidate native GI fixture also passes21frames/GBV0/terminal exit0 in254.45s
  (sdk/native-gi-cpu-csm-gbv). Subsequent geometry-read cleanup reuses the prepared
  unique tokens for cold capture and pass declarations, removing duplicate vector
  reconstruction and std::set; graph-aware validation/access-conflict checks remain.
  Normal candidate exe/PDB are pinned before a temporary full-GPU-drain isolation
  binary. Serial isolation3/3 completes1800ticks; this is not performance proof or
  root-cause proof. Diagnostic drain was removed immediately after linking; frame
  source hash exactly restored and all normal apps rebuilt. No final per-frame wait.
  Failure reproduced under local CDB with normal source; alive7.74GB process dump
  is preserved with matching private PDB in native-ultra-alive-dump-3. Strict symbol
  reload (LOAD_ANYTHING disabled) confirms current mapped3CBVs each have1volume,
  96slices,240x135froxels and sequential frame307/308/309. This excludes an observed
  CPU count overwrite, not a GPU descriptor/lifetime fault. No definitive cause yet.
  Terrain view normalization moves before the active-layer loop; cooked actualDXIL
  has one dot/rsqrt before loop header. Per-layer POM/samples/quality unchanged.
  Native final-declarations fixture passes validation0/exit0 after this cook and
  duplicate geometry-read cleanup; currentruntime-full8a785e6b...c4e111. Final
  normal Debug apps and ReleaseSDK-OFF renderer build pass. FullUltra200 final
  terrain replay fails267CPUframes/264GPUframes, so the current candidate remains
  unqualified for stability/FPS. No speculative synchronization/flag changes kept.
  Old/current actual terrain DXIL confirm dot/rsqrt moves from active-layer loop
  to preheader; not merely a source edit. P0 next: actual GPU table/dispatch and
  inter-frame use, not CPU constant count corruption or Enhanced-only behavior.
  Complete baseline has149scheduled pass rows,116measured; unmeasured stay N/A.
  Preserved valid dump7743787037B and matching symbols; removed6empty dumps and
  invalid relocated pointer. CDB and diagnostic replay processes are gone.

- Follow-up queue isolation uses a pinned temporary binary only. Before volume
  registration, waiting prior frame Direct completion passes1800ticks/499frames;
  Compute completion passes1800ticks/626frames; Copy-only fails233frames. No
  wait code/environment switch remains in production source. This narrows overlap,
  not a proven faulty resource. Source audit found no un-fenced off-graph writer.
  A proposed unchanged-static lookup cache passed native but measured no resource-
  preparation improvement (13.358–13.453ms before vs13.414ms after in volume-OFF
  component scope), so both added scratch vectors/scan/fast-path were removed.
  Preserve prior pooled-baked/flat-key/prepared-unique-read changes.
  Volume compute now owns one root table with41descriptors rather than cloning
 1392scene entries plus27volume entries and inheriting global bind_compute/root37.
  CBV stays a bounded descriptor with originalflags; SRV/UAV flags/fences/graph
  declarations unchanged. Sources:27volume,1gradient,3light,6VSM,4local-shadow.
  Existing canonical-null initialization separates compact volume from full
  geometry/Brix banks; Brix/view-skin offsets follow actual compact capacity.
  Debug apps, ReleaseSDK-OFF renderer and CPU4/4 pass. Actual native21frame fixture
  validation0/exit0 passes; GBV reportsvalidation0 but terminal process pending.
  Current native fixture waits streaming perframe; it cannot qualify OFF overlap.
  Exact same-generation captures have max4/255channel difference; existing
  same-root repeats also vary by4/255, not a bit-identical oracle. Normal fullUltra
  stress of this new root remains to execute. Do not claim P0 fixed or60FPS.

- Prior measured core bottleneck batch: frontend cache history eligibility is
  checked before newest-candidate selection, using the same once-prepared camera,
  reset and revisions as actual volumetric registration. MatchesProjectedLayout,
  BindRetained, final native contract/alias/fence checks remain. Capacity8 unchanged.
  Two frame import maps share16KiB stack-local PMR with ordinary upstream growth.
  Terrain shares exact per-layer detail fade across3samples; half-resolution AO
  skips duplicate first-guide read while preserving invalid-guide fields.
  Reciprocal AO experiment was reverted; no quality/sample reduction. Off-volume
  revision hashing stays skipped. Temporary rejection counters/writer removed.
  Final Debug app/experiment/runner and Release renderer lib compile; AO realGPU+
  GBV18cases/1812pairs bit-identical; world-transport21frames and owned-view3packages/
  9submissions pass validation0. Actual ProjectHS smoke180ticks/2frames passes0errors.
  Final200 native playback1800ticks/821frames passes, no device removal, assertions
  true, validation0, complete own-frame GPU coverage821/821. Stable Main/config2
  before/after602foreground frames each: frontend343->537hits; graph build mean
  3.803->3.215ms; FPS30.101->30.121,1%low23.129->23.125,p9537.033->36.719ms,
  p9941.929->42.282ms. No meaningful total-FPS gain;60FPS unmet. Final GBuffer7.726ms,
  AO3.321ms,Hero1082.834ms; pass totals must not be added across overlapping queues.
  True graph compile misses still cost about7ms; no unsupported cache-size change.
  GPU utilization summary partial:1PDH counter error, whole-device NVIDIA CSV absent;
  frame/pass timing coverage itself complete. Actual GI remains Probe fallback.
  Measured candidate8ef1da725f86f6e11a2414b0e169bc6843369c4cfae0fd991b4dd2039f6601c0;
  current runtime-full0d2c60c658e3db3f6b6f87065d1fad2f15b598c7de802a4ffb4202328b31b165.
  All2903logical IDs/digests/sizes equal across these scopes;9shader+1baked-probe
  products differ from987426 baseline. Official selected-generation composition only.
  FullUltraGI/full-rendererGBVteardown/Release repack remain unqualified. One-shot
  raw view payloads859805096B removed; receipts/images/metadata/timing CSV retained.

- Further engine-comparison report R01–R11 has been read and dispositioned.
  Hero/froxel queries use the existing complete3D light mask, retaining invalid/
  off-frustum full scans and stable light order. Header-only fast paths skip
  grid matrices/cells for zero locals, forced-world and directional-only queries;
  actual DXIL confirms the zero-local branch precedes matrix loads. Volumetric
  area shadows evaluate once at the first valid direction sample; column ray
  metric and adjacent exact exp2 boundaries are shared. Actual RTX4080+GBV
  baseline/candidate production-header numeric probe passes360cases, nonfinite0,
  errors0; max relative transport delta7.08337e-7, not bitwise/pixel parity.
  SSR reuses first footprint read, prepared BSDF sampling and Temporal center/
  clamped guides. Full-closure forwarding across candidates was rejected after
  DXIL buffer loads42->85; final minimal-guide variant has Trace42->38buffer,
  36->31texture and Temporal200->160buffer,320->260texture loads. These are
  compiled static-work observations, not frame-time or FPS improvements.
  Masked material bindings form a conservative registered opacity texture union;
  shadow/visibility passes declare only that domain and matching feedback writes.
  Surface/resolve/probe-color retains all bindings; shared masked aliases stay.
  Conditional material specialization, uncooked skin partial reduction, terrain/
  foliage bake and SSR sparse list were reviewed against current owners and
  quality/total-cost requirements; no unsupported new system was added.
  Arrow Rain immutable route/radius/window now resolves once per adapter call
  across current/previous endpoints. Ground suppresses legacy before generation;
  the older generate/cancel TODO is stale. Existing T13 pre-ACK reservations and
  separate native/Present receipts remain. Environment snapshot/cook bytes A3
  already match in both batch and single-role paths; no duplicate fix was needed.
  Strict probe capture now rejects an intrinsically impossible required mip set
  against the configured native cap, independently of transient DXGI pressure;
  only explicit wait_for_streaming capture fails intrinsically impossible admission;
  ordinary asynchronous fallback remains. Existing real two-owner native allocation case and
  WARP/GBV queue suite pass27.77s. CPU VFX/streaming/Runtime9/9 pass8.77s.
  Debug app/tools/selected checks compile106.43s; Release renderer/Runtime libs
  compile70.78s. Static bake narrows preparation to16authored material profiles/
  parameters, skipping7Hero and decal work. Existing one-shot GBV120page fixture
  passes5.01s, validation0, all16output metrics identical to the prior checkpoint.
  Prior accepted content generation succeeds930.05s and selected
  987426872b50f1a160170fe54959731392d4aecac3005d1b39bf16ba23beb754.
  Compared with the preserved177948 baseline,2903products have64shader changes
  and one recooked local_probes.spkg; do not call whole-image A/B shader-only.
  Before/after light-list6 and Hero8frames pass validation0. All24light-list
  HDR/depth/metadata raw resources are byte-identical; Hero204sample counter
  totals match. These1280x720 fixtures are functional, not target performance.
  World transport changes lights and needs native recapture. Source diagnosis
  shows all22environment owners at software chunk4 (3uploads/4requests), target1,
  resident fallback4 at frame7; private candidates publish only after final chunk.
  This is insufficient fixture duration, not evidence of deadlock. Existing
  streaming wait is enabled; native24face/SH/GGX completes in frames17-20.
  Simplified fixture keeps required native work and SSRTrace/Temporal, uses32
  as the failure bound and exits at actual readiness. Final default passes at
  frame21, sample/source ready=true, validation0. No extra renderer retry loop.
  Failure-only existing-API mip diagnostics remain actionable; no per-frame log.
  Final strict-capture scope and runner changes compile in Debug/Release.
  FullUltra GI, full-renderer GBV teardown and Release repack remain unqualified.
  Current foreground component timing is superseded by the core batch above.

- New GPU report A01/A02/A03/A05/A06/A07 implemented in source; A04 adjacent
  sample skipping was evaluated and not adopted without real-scene net benefit
  (extra comparisons/branches). AO fuses horizon/resolve and removes sparse
  resource/pass, preserving compatibility fallback. Final production/probe
  sources match;18 real GPU/GBV cases1812texel pairs are bit-identical to the
  original two-pass path. Cluster moves immutable checks to admitted catalog/
  frame setup, prepares original plane lengths, and uses unique representation
  page ranges. Selected35hierarchies:13140clusters->7597page references,30388B.
  Global catalog page offsets are rebased and checked on merge. Terrain/VSM/
  Hero reduce actual DXIL work; this is not a whole-frame/FPS result.
  Feature/native graph checks pass, then the first cluster integration exposed
  a prior Release-strip debug-name regression: optional VSM roots were named
  without being allocated. CDB+matchingPDB locate VirtualShadowSystem::Prepare
  line747; names now occur only in their successful construction branch.
  The corrected14frame visibility/cluster fixture passes with validation0 in
  Build/additional-gpu-20261029/cluster-fixed/; old Artifacts/cluster-retest
  receipts are stale and must not be reused. No other optional-name guard loss
  was found. The CDB dump creation failed due its command-path quoting; stack
  and source-symbol lookup, not a nonexistent dump, establish the cause.
  WriteSlot now owns exchange+privateindex, is noncopyable/move-safe, publishes
  through its owner and abandons via RAII. Existing scene-sample/Runtime tests
  pass; OFF/OFF scene-sample passes. TaskSystem creates move-only, creator-bound
  groups; foreign-system submission and retained task-label strings are removed.
  Runtime/render runner/probe bake keep groups directly, removing the extra heap
  wrapper per creation/join. Work state remains pinned across reentrant cancellation.
  Audio stores actual mono spatial eligibility on successful voice admission;
  listener changes retain voice/sample lifetime checks without rechecking format.
  Per-instance SoftwareGI geometry validation
  is deduplicated per referenced geometry for frame preparation; graph binding
  validation remains. Hero's immutable admitted buffer/range pair no longer
  queries its dimensions per pixel; dynamic material-slot rejection remains.
  Current one-shot static-shadow bake API privately prepares capture resources
  and selected PSOs, without game view/swapchain/post/UI/font/window creation.
  It loads35terminal LOD2 pages instead of105metadata selections/70resident
  LOD1+LOD2 payloads, and skips the global20environment fallback textures,
  BRDF LUT, texture-streaming setup and reduced material fallback uploads.
  Capture-only bindings are admitted against real catalog descriptors; private
  full-chain uploads provide only authored opacity inputs (canopy, and grass if
  ever statically admitted). Opaque material slots use typed null arrays and
  create no texture resources. Actual production index/page digests are identical
  across this last tightening. Canonical null SRVs now carry
  their shader ABI types independently of live view initialization. Existing GBV
  static-bake fixture passes:120pages, validation0, all recorded output metrics
  equal to the full-loader checkpoint, repeat/guards/wind exclusion preserved.
  Current Debug game and affected tools are relinked; Debug+Release libraries
  compile. verify-core19/19 remains valid for unchanged low-level source; latest
  Runtime integration passes and actual Hero8frame execution passes validation0,
  204samples and unchanged workload counters (not pixel/FPS equivalence).
  At that prior checkpoint, official static-shadow/probe products were refreshed
  and runtime-full selected177948c4347129e9eacacbb997eb216d32dd5ec9fe240ecbb2e0030ac68658fb.
  Loose-generation material/parameter catalog closure still remains; scene-probe
  bake still drives full rendering. Do not remove those dependency keys until
  actual input ownership is narrowed. Broader dependency/check review is not a
  declaration that every function has been read or all directives are complete.
  See CONTINUATION.md for active builds, exact evidence and next steps.

- Logging/test cleanup: removed producer current/action-key banners, cooker
  per-slice/QA summaries and their log-only computations, SDK startup version
  banners, the unused camera dump and ordinary game success summaries. Keep
  real producer publish/touch operations and actionable errors. Adapter/driver/
  DRED configuration files and the driver-version query now require explicit
  validation or timing capture. The native runner retains result.json and short
  stderr failures instead of duplicating its entire receipt on stdout.
  Deleted the unregistered AO before/after source+runner and two benchmark-only
  modes. Removed generated-source temporary-name/format assertions, success-log
  assertions and self-created fixture count/prose checks; retained compiled VFX
  round trips, numeric/ABI/identity/repair/restat and native lifetime checks.
  Navigation's assert checks were disabled by NDEBUG in RelWithDebInfo; its
  existing target now undefines NDEBUG rather than silently passing.
  Tracked Tests source files present124->122 (excluding Python caches and the
  verifier deleted in the preceding batch), CTests113->113; no new test file.
  Debug producers/Runtime/renderer/runner/app build succeeds, six selected CPU
  contracts pass cumulatively, navigation passes with /UNDEBUG, and existing
  hs_dev/graphics/play/OIT checks pass. The compiled-VFX retry restored missing
  MSVC INCLUDE; analyzer retries used writable workspace TEMP. These environment
  failures remain in the evidence. Native owned-view3packages/9submissions pass,
  validation0, before the final console-only/device-diagnostic gating edits;
  those subsequent edits compile. No full recook, Release repack, optional-SDK
  build, foreground/FPS comparison or full-renderer GBV rerun in this batch.
  Modified heavy producer scripts received syntax checks only, not new native
  producer scenario qualification. See the batch report for exact limits.

- Release exclusion checkpoint: Build/release-strip-20261029/RESULTS.md and
  package-result.json, before the logging/test cleanup above. Release/Debug
  builds pass at that checkpoint; installed/ZIP five files
  match SHA256. EXE8,551,936bytes vs9,341,952 before (8.46% code-size reduction,
  not an FPS result). Selected20 diagnostic/test strings and PE imports PIX,
  D3D12GetDebugInterface/OutputDebugString are absent. No game process launched.
  Runtime replay/record implementations, smoke/preview injection, optional logs,
  VFX peak capture, renderer recorder/pending diagnostic data, HeroWork native
  instrumentation and debug names are compile-excluded. Release's experiment
  host is absent. Preserve controller-only DRS/activeAutoFG timing and native
  lifetime contracts. Runtime-only Release cache uses the previously qualified
  identical CPP/pack pair; no cook input changed. Core2 checks, existing Debug5
  checks (including WARP/GBV graph51.63s), and install transaction fixture pass.
  Later development-guard-only changes rebuilt successfully; unchanged broad
  checks were not repeated. Audit any remaining observation counters by actual
  consumers before removal; full optional-SDK/foreground qualification remains.

- Current analysis: Build/health-review-20261029/RESULTS.ko.md. Exact foreground
  GPU pass means: GBuffer7.160ms, GTAOHorizon3.465ms, HeroComposite#1082.825ms;
  all passes/p95/queues in gpu-passes.md/.json. PID GPU average73.324%,peak80.226%,
  CPU1.418cores/7.091% of20logical cores, with94.05% foreground GPU coverage.
  Render wall32.042ms includes wait12.922ms; residual is not measured CPU busy.
  Critical path and output span are unavailable; source-ready cadence is separate.
  Fresh full Desloppify scan2 + all20 blind dimension reviews are imported:
  objective/verified78.3,strict/overall87.4. The initial unassessed score19.6 and
  previous CI profile73.9 are not code/performance improvements. Immutable review
  packet/results are in .desloppify/subagents/runs/20261029_110247/.
  Nine review findings:3 implemented and checked,4 naming/folder/config-wrapper
  proposals explicitly declined,2 broad Impl/Render coupling findings remain
  conditional. Do not mark the large-function/header debt fixed by a small cleanup.
  No health-score commits. Mechanical candidates are heuristic, not900confirmed bugs.
  Local C++ review regex repeated the extractor colon bug; one-character tool
  fix plus declaration/case-label self-check passes and review preparation now
  completes. Windows schema read requires python -X utf8 -m desloppify.
- Slime body color investigation: isolated4frame baseline and private
  transmission0/0.5 candidates all complete,validation0. Body hue stays blue/
  green/red-brown but baseline is dark; full transmission removes diffuse by
  construction. Candidate changes only617-byte slime_body parameters, using real
  producer + immutable hardlinked Generation. Source/pointer unchanged. Do not
  claim RGB inversion fixed or silently replace glass with opaque lighting.
  Artifacts: Build/transparent-color-20261029/{slime-reference,transmission-0,
  transmission-05,private-materials}/. Authored slime_body transmission default is
  now0.5, preserving partial dielectric transport and coat. The explicit content
  update completes with selected Generation6d65e4bfe47dab24502d8227af6840e07e1f5542ef68dd12ad639e7553aba896.
  Its617-byte parameter digest ef63eefd031ecfbf742d50b6655e08a414f554d7ae788f3ac8c89d8c928413ca
  matches the private0.5 candidate. Current native4frame capture completes with
  validation0. No shader RGB/sign workaround was applied or literal inversion proved.
- Health source fixes: removed unused raw RenderFrameInput.events and the private
  legacy reentrant bridge; typed VFX/audio event translation stays in Runtime.
  Private RenderView always uses the explicit view camera and package ownership.
  Removed overwritten camera calculations. Graphics/compute PSO cache admission
  shares one helper with unchanged key/lifecycle/error/counter contracts; mesh stays
  separate. Audio reader reports the first retained Result/exception reason once,
  using bounded diagnostics only on failure. DirectXTex pins the same full40char SHA.
  Build health-fixes-20261029 succeeds;6existing contracts pass in58.51s, including
  the native WARP/GBV graph. Owned-view3packages/9submissions and slime4frames pass
  on the current binary (debug layer ON/GBV OFF), validation0. Evidence is under
  Build/health-native-20261029/. Subsequent graph static-facts Debug build and
  existing WARP/GBV graph also pass (46.98s), including owner callback/shared reader
  paths. Existing TopologyValues::Resource holds the shared-reader exclusion fact;
  Capture computes it once and Execute no longer traverses every pass/use to
  rediscover it. ExecutionKind now also distinguishes an empty owner callback's
  owner-thread flag so reuse cannot keep a mismatched exclusion. Dynamic imported
  identity/completion/alias/lease checks stay. Evidence: health-graph-facts-20261029.
  The scene-HLOD/snapshot existing leaf passes1.02s: frozen source/include bytes
  survive source edits and stale input digests are rejected. It does not execute
  the DXC timeout/uncertain-exit branch or prove the whole producer include closure.
- The follow-up200 playback after the above fixes completes1800ticks/385frames,
  exit0, but has no foreground frames. Build/health-foreground-200-20261029/ is
  background functional evidence only, not an A/B FPS comparison. The user again
  defers foreground measurement; continue implementation without another window.
  Keep the earlier587foreground-frame pass table as the foreground baseline.
- Exact baseline retained frontend hit rate is368/587(62.69%). graph_plan p50 is
  hit2.830ms/miss2.821ms and includes executor slot Wait. constants_upload is a
  composite scope containing GPU scene/VFX preparation, not pure constant memcpy.
  Do not add caches for a768-byte zero fill or attribute these whole scopes to it.
- ProjectileMotion retains its assigned scalar speed, used by homing rather than
  repeated Length(velocity). Ricochet/Basic retargets keep that effective speed,
  including BasicAttack fallback when Ricochet authored speed is nonpositive;
  boss/ranged producer overrides still set their actual assigned speed.
  The scalar participates in canonical checksum; gameplay_hash_version=8 and
  old7/6 replays are explicitly rejected. No arbitrary expiry was added.
  ProcessInput shares squared aim distance and ignores nonfinite/overflowing aim
  targets while retaining the last valid direction. Cooker checks Ricochet return
  progress using actual arena-boundary extents, separately from initial/search legs.
  Current13 documents pass; a private-copy tiny initial/search leg with stalled
  return is rejected at boundary_points_m. Originals are unchanged. OFF/OFF19 and
  five selected Debug contracts pass at the first scalar checkpoint; latest
  fallback production path and checksum case pass the existing internal fixture.
  That fixture's borrow scope/1-based upgrade bit errors were corrected, with
  assertions preserved. Its CRT assertion diagnostics now use stderr to avoid a
  blocking GUI dialog. Final affected Debug relink succeeds; final combat and
  determinism2/2 pass. Foreground input specs prepared before this version8
  binary are stale and must have their build hash regenerated before reuse.
- Owned-view GBV boundary markers flush capture/Shutdown entry and actual renderer
  destruction boundaries. The180s diagnosis initializes7.1s without a completed
  package marker. A subsequent bounded45s CDB diagnosis has real private-symbol
  stacks at20/40s: main joins render-recording tasks; workers are in
  DrawClusterGeometry/DrawGeometry through SDK layers/NVIDIA modules. Offline
  matching-EXE/PDB return-address lookup resolves both to SetPipelineState
  (renderer_cluster_graph.cpp:666, renderer_geometry.cpp:156), consistent with
  GBV first-use patched-PSO compilation. No engine rerun was needed for lookup.
  These are not GPU-fence waits or proof of the earlier teardown stall's cause.
  Moving compilation to creation is not proven to reduce total work and was not
  adopted. Both bounded runs time out, not pass. Earlier622s run
  did complete helper captures. No further blind long repeat is planned.
  Corrected evidence: Build/gbv-stack-corrected-20261029/RESULTS.md. Earlier
  gbv-stack-20261029 CDB logs failed -pd and are not stack evidence. CDB is local
  to Build/tooling/dbgeng/, Microsoft-signed, and qualified on an owned helper.
- New minimal/GPU/test-slimming reports have been read and reconciled with the
  dirty source; full review notes are in Build/directive-intake-20261029/.
  Runtime defers rejected candidate hash growth, removes closed commit rechecks,
  uses aggregate all-fit profile admission, hoists Light facts per event and gives
  birth buffers to the existing session owner. Fixed1–3 destination row admission
  uses capacity thresholds after the original all-fit fast path. Actual old/new
  production headers and232/80/192-byte row types match in531441 cases. First
  no-fast-path trial was rejected for a common-path regression; final Debug
  limited-case microbench is about53→13ms for300000 calls, not frame/FPS evidence.
  Shortage admission now caches row/profile/supplemental cost components and
  recalculates only the candidate's affected components, retaining overflow and
  zero-growth semantics. The existing fixture covers cross-profile rejection
  after accepted Light/Ribbon growth; allocation-failure rollback still passes.
  GPU world particles/Ribbon history update once per explicit package; per-view
  draws/RibbonArgs remain. Queue-local query/readback ownership and the new
  OutputDependencyInterval metric avoid unrelated work joining automatic DRS.
  Existing owned-view fixture now includes an actual retained GPU particle.
  Integrated Debug build passes after fixing a duplicate local and a fixture's
  designated-initializer order. OFF/OFF19 passed, with the affected2 leaves repeated
  after a declaration-warning fix. Debug CPU16 passed and the initially unbuilt
  isolated failure leaf subsequently passed (17 selected contracts cumulatively).
  Actual multi-queue native graph passes42.60s, including independent query resolve
  while Compute/Copy are gated. Full/Critical timing probes and owned-view control
  pass with validation0. The owned-view particle oracle is3 (one explicit birth
  plus two existing effect-line births), not1. Owned-view GBV was stopped after
  622s without a final receipt. Its per-package readbacks and owned-view captures
  prove the helper renders completed; the earlier first-render inference was
  wrong. Generic capture/wait versus later teardown remains unresolved.
- Actual Debug200-enemy diagnosis completes1800ticks/405frames, timing coverage405/405.
  All steady samples are background; requested Software GI falls back to Probe
  (consumer_unavailable, HS_WITH_BRIXELIZER=OFF). Source-output cadence
  p50/p95/p99=32.851/34.761/35.449ms; this does not qualify fullUltra/foreground60FPS.
  Largest measured individual GPU passes are GBuffer+Depth6.94ms median and
  GTAO3.44ms; overlapping queue durations must not be summed. Cold frame local
  probes take13.65s, matched by26 new PSO creates totaling13.59s. The new existing-
  executor fork/join batch parallelizes distinct material PSOs; same-key cache
  requests serialize, lifecycle changes wait for creators, all accepted jobs join
  before return/recording. Native fallback without pipeline libraries stays serial.
  Debug build and4 affected CPU contracts pass; WARP+GBV native graph including
  same-key concurrency/malformed-shader retry passes35.72s, no validation errors.
  Offscreen current200-enemy180-tick execution passes (2 rendered frames), no
  cache load/store errors; existing ordinary15s heartbeat guard is unchanged.
  Its first local-probe prep is76.153ms, warm repeat58.545ms. Warm old-after is
  91.749ms for the same25 post-init PSOs; this is a single component observation,
  not a robust speed ratio. Driver-cache effects dominate the first A/B: old
  initial run hits the running watchdog, then old warm prep3.425s becomes91.749ms
  after the candidate. Do not attribute the cold13.65s-to-warm change to this
  patch. Receipts/dump remain in Build/directive-intake-20261029/pso-component/.
  A later matching-source foreground measurement is recorded above; the older
  background/component observations remain separate. Full-renderer GBV timeout
  is still unresolved.
- Named Debug ProjectHS/hs_experiment builds reuse the selected Generation instead
  of depending on whole-content cooking. Default ALL/runtime and Release shipping
  preparation retain their existing generation dependencies. Actual requested
  app rebuild drops233 scheduled commands to6, without recooking. Content edits
  still require the existing explicit hs_content_generation target.
- Frame lights borrow the immutable snapshot prefix; fallback/default sun or an
  accepted typed light materializes storage once. Authoritative count, sun choice,
  transient order and borrowed lifetime are preserved. Static shadow wind radius
  still invalidates its expanded-bound batch; removing that identity alone is
  unsafe and was not adopted. Geometry-demand bounds already persist in StaticDrawData.
- Desloppify is installed locally from GitHub3a7735d in Build/tooling/desloppify/.venv;
  official Codex skill installed. First C++ ci-profile scan completed:530 production,
  89 test files,898 raw candidates. Local tool-only C++ extractor regex fix avoids
  case-label backtracking; its8 existing checks pass. status/next/backlog ran.
  No engine changes/tests were added for scores. CMake translation units are false
  orphan candidates; numeric statm fscanf is a confirmed heuristic false positive.
  The test log's7546 is weighted potential, not file count. Native analyzers are
  unavailable and20 subjective dimensions are unassessed; no stability/FPS claim.
  Tools/tooling/WORKFLOW.md documents usage and the reinstall caveat; raw results
  and triage are in Build/tooling/desloppify/RESULTS.md. W12 was later approved.
- Current test slimming: nine disconnected Python models and registrations removed;
  shader-reconstruction retains only FrameConstants ABI and light-shadow lookup only
  the real capture reader. Ground admission uses named expected cases instead of the previous implementation.
  Windows CI builds/runs the portable superset once. Linux CI and its configure,
  build, test and workflow presets are removed. Independent Windows verify-core
  remains available. Existing CTest labels are merged.
  VFX native partitions are five named boundary cases, not the full Cartesian set.
  Global allocation injection lives only in one isolated native-free executable;
  it prepares fixtures before injecting exactly one commit operation, then verifies
  rollback and a reserved nonallocating retry. The16 Ground admission boundary rows
  now run in the existing no-cook legacy test; cooked Ground keeps actual product
  integration. The expired unregistered AO DXIL comparison was subsequently
  removed in the logging/test cleanup; production GPU checks remain.
  OFF/OFF19 and portable39 passed cumulatively (only the repaired failure-injection
  leaf was repeated). Logs: slimming-core-console/slimming-failure-leaf-console,
  slimming-portable-tests/slimming-portable-failure-test.
- Windows full gate at this base plus dirty fixes built/cooked successfully and
  ran119 tests:110 passed/9 failed (verify-20261029T030832654Z-16284.log).
  Selected followup repaired environment-family action/offscreen/presentation and
  long native path handling. Actual Ground/Runtime and retained ABI/reader contracts
  passed. Persistent-scene/HZB native checks pass. Full WARP graph Legacy+Enhanced
  passed directly52.55s and in CTest45.61s after its measured timeout budget was
  corrected45->90s. No validation guard/canary was removed. DynamicLOD now excludes
  unrelated post effects, preserves8frames/GBV/history assertions and passes251.38s,
  validation0. Residency stops only after all existing required phases/counters;
  GBV-OFF control passes83frames/17.60s with validation0. GBV-ON reaches all success
  receipts at86frames, then times out600s inside renderer destruction. LOD logged
  renderer destructor109.793s versus0.17s in the GBV-OFF streaming control; explicit
  Shutdown is0.2-0.36s and task destruction1ms. Do not call GBV streaming a pass.
  Logs: verify-20261029T045826298Z-17992.log and streaming-postshutdown-control.*.
  One-frame intentional-incomplete-phase diagnostic took318s rendering and was
  stopped after reaching teardown; its destructor Shutdown116.869ms and body end
  116.919ms prove the remaining delay is later Impl member destruction. It is not a
  qualification pass. Temporary renderer/runner destructor timers are now removed;
  original shutdown/resource lifetimes remain. Final cleanup build/fast native
  check passes (slimming-clean-final-console.log). The existing owned-view/history
  probe passes9.00s/validation0 on the final binary (slimming-owned-view-final.*).
  The two regenerable control/
  diagnostic packs were deleted (2598816764 bytes); receipts/logs remain.
  GBV streaming teardown qualification remains unresolved.

- Snapshot values and mutable builders have separate public headers; the existing
  render_snapshot.hpp remains a compatibility entry point. Renderer's compiled
  93-object dependency set now has0 storage/wrapper dependencies, formerly78.
  Value declarations, field order and helper expressions are unchanged. Actual
  storage callers include storage; no additional module/allocator or format.
- Frozen publication explicitly reuses per-slot dynamic arrays by known nonzero
  session/revision plus actual tick (tick0 valid). Header/camera/static-scene/UI
  and new publication/Present identities remain fresh. Raw write/copy behavior
  stays unchanged. Mutators/moved-from/self-move/partial-copy failure invalidate
  the cache. Runtime animation now shares the projection invalidation gate.
- performance.csv header and numeric row writer expand one explicit private field
  inventory, including the task-column position. Current194 columns preserve the
  previous192 names/order and add vfx_profile_prepare_us/calls. Profile preparation
  no longer merges into vfx_local_outputs. No per-frame schema validation/registry.

- Raw ingress has a combined4096 pending/inflight bound, adaptive backing and
  ready-prefix staging. ReadyBatch owns PresentationEvent rows directly; Runtime
  dispatches its span without a second ready_events array or duplicate capacity
  charge. The borrow ends before CPU publication/ACK.
  CPU activation ACK, native submission ACK and Main Present
  receipt are separate. Failure still terminates the session; no new retry manager.
- ActiveEffectStore retains canonical rows, masks and indexed profile views.
  Owner-bound sparse refresh preserves birth/capture rows. Empty unchanged compact
  returns early; binding-ID capacity follows active bindings rather than all rows.
  Ground/Light/Distortion/Decal now combine owner refresh and row-local expiry in
  one stable compaction. Shared bindings still refresh once; two-argument callers
  compile out expiry. The old two-step sequence remains the CPU fixture oracle.
- Profile preparation first plans unique keys and final memberships, preserving
  each profile's request order. Only then does it reserve unique-sized new-row
  staging and clone first inputs/index nodes. Planning records request indices,
  not borrowed pointers in a ready batch. Optional membership selection now precedes
  payload/node creation; profile-admission-bounded records Debug compile/CPU evidence.
  Cosmetic masks are prepared once per catalog generation. Only completely prepared
  fixed-time Direct nonauthoritative cosmetic Ground outputs may be omitted; owner-
  bound Ground, EntitySurface, PointRate, replacement contexts and unknown or
  authoritative contributors stay required.
  Duplicate keys union required bits before selection. Cosmetic memberships rank by
  importance/first request, then publish in original per-profile order. Actual map
  bucket backing and matching vector growth are charged; warmed zero-growth reuse
  remains allowed. This is a logical growth estimate, not RSS or a whole-VFX cap.
- Frame fanout includes Ground, Light, Distortion, Decal, Volume, Screen, Ribbon
  and EntitySurface. Persistent Mesh/Fresnel maxima also reserve before publication.
  Immutable image plans hold all-quality maxima and persistent output bounds.
  Existing/persistent base demand reserves first; new memberships charge future
  frame-vector growth before CPU publication/raw ACK. Duplicate costs take a
  per-key/profile maximum. Required output stays admitted, but its not-yet-allocated
  growth consumes space before optional growth. Planning rows now grow by unique
  keys, preserving the existing duplicate-allocation size guard.
  Upgrade movement suffixes now append before planning; the former late append
  omitted captured Line/Context Ribbon rows/outputs from the reserve. Existing
  captures, pending Surface events and new memberships count once. Both retained
  capture and frame-input arrays reserve before raw ACK, independently crediting
  their warm capacities. The session's existing final reserve is then a no-op.
  Ground uses the full prepared recipe count N across quality levels. Wave's
  special/context categories are disjoint, so no arbitrary N+2 padding is used.
  Existing/persistent required Ground output reserves before CPU publication.
- Runtime Ground directly appends into VfxGroundSpawnInput, sharing the original
  emitter with owning typed APIs. The intermediate command buffer and final full
  row-copy/move loop are gone. Geometry and seeds/order/timing remain unchanged.
  Its frame path converts irregular angles once per input into a reused pool;
  recipe outputs share the slice. Owning helpers retain their vectors. Persistent
  then event append preserves the prefix; failure restores both sizes.
- Ribbon frame scratch uses one reusable contiguous output pool, with per-source
  ranges and O(1) logical capacity accounting. Owning helper APIs materialize at
  their return boundary; failed consuming Take leaves scratch logically empty.
  Source/output rollback, moves and warm reuse remain. Renderer validates pool
  storage and copies spans into the same retained GPU-facing state/ABI. No new
  queue, fence or shader contract. Per-image bounds cover source and pooled-output
  maxima across quality/payload routes, including wake+captured-line. Ground and
  Distortion angle pools plus Ribbon outputs share supplemental growth admission
  before CPU publication/raw ACK. Required uint32 overflow fails there; optional
  overflow skips that membership. Unselected profile columns in an effect's full
  frame-row table do not reject unrelated requests.
- Distortion's rebuilt frame shares a caller-owned irregular gap pool across each
  input's recipe outputs. Uniform gaps remain compact; owning Build/Append behavior
  remains. Render borrows the pool through its call and checks storage/ranges at
  admission. This removes per-output CPU vector copies, not native upload copies.
  Distortion admission now charges frame rows and one pooled irregular slice per
  membership. Existing-key duplicates use retained canonical payload, new keys use
  the first input. Required growth precedes decoration; uint32 offset overflow
  rejects required demand before publication and skips optional demand. No new cap.
- Developer texture proof uses the TXM1 v2 format maximum196648 metadata bytes,
  shared with the encoder/layout. The prior v1-derived65576 cap rejected valid
  large arrays. Volume metadata fits the same read bound. Version/digest checks
  remain; a maximum4096-record v2 CPU fixture uses only10240 payload bytes.
- Surface diagnostics select the submitted view's actual decal base/normal/material
  exports when active, and original GBuffer targets on empty frames. A per-view
  success-path fact tracks that choice. Capture's format/state checks and GPU wait
  stay. The former capture read original normal/material after rendering switched
  to out-of-place decal exports; original input Common is then a valid final access.
- Prepared raw flash/ballistic/mote/legacy plans expose required and optional rows.
  Required status shards/tombstones/fire/stamp suffixes reserve before decoration.
  Optional selection uses existing importance/original order and original seeds.
  Actual typed emission controls fallback; deliberate cosmetic drops suppress the
  matching legacy decoration, failed conversion retains fallback.
- vfx_birth_admission.hpp owns row reservation/selection; its callback measures
  actual workspace after scratch reservation. It separately charges prospective
  native copy capacity. The caller uses vfx_workspace_breakdown, NOT the callback
  that already adds prospective native bytes; do not double-subtract this charge.
- vfx_visible_outputs.hpp owns flash suffix/spark/live mote and fire/stamp output.
  Old flashes retire/project before birth admission; newly appended rows project
  later. Required suffix and retained/birth/spark/fire/stamp order remain.
- Fire scheduling uses the admitted uint32 duration/rate bounds for one uint64
  product/division instead of per-source overflow guards and split arithmetic.
  The legacy floating bound remains; a compile-time rate-width check protects
  widening. Dynamic owner/clock/geometry admission remains.
- Five cooked fields actually assigned to recurring AreaActor::interval now use
  ToAreaTickInterval: existing ToTicks plus a rounded-zero rejection. Generic
  delays/durations still accept zero;64-bit Tick stays. Actual producer comparisons
  prove normal/one-tick acceptance and zero/subtick rejection. Boss ground
  areas now copy action.interval through max(1) instead of silently using30.
  Current authored intervals are30; the public seven-tick/expiry fixture passes.
- Derived ArrowRain and Trap fire use their own authored interval at the local
  materialization loop, not the global ScheduledKind::Area dispatcher. Their
  pending rows store position/cast/skill/source-upgrade only; the matching immutable
  recipe supplies the other fields. MSVC x64 probe:96B ScheduledAction→24B local row.
  Source tags3/4, original skill/cast/order/Derived origin/mask0/no-relic default
  and float(duration_ticks)/60 stay. Exact hit-count/cadence/retirement fixtures pass.
- Cooker now performs a conservative single-step float-progress check for positive legs,
  preserving BasicAttack speed fallback and nonpositive-leg retirement. It covers
  all player skills including Trap via DifferentSkillTracker, Ricochet search reset,
  ranged enemies and projectile-producing boss patterns. Original inputs remain
  unchanged in actual producer16-case before/after tests. The later range-direction
  batch closes diagonal square-underflow admission using a half-speed component
  lower bound for finite normalized2D directions; no movement code or runtime check.
  Debug two-target build and existing HLOD/number CPU check pass. Actual producer
  accepts13 current documents and rejects the precise diagonal counterexample in
  an isolated source copy at skills:$/entries/1/parameters/range. Temporary copies
  were removed. Repeated homing speed drift, arena return, multiple-leg lifetime
  and whole-VFX bounds remain unproven. Evidence: range-direction/RESULTS.md.
- Typed VFX nested capacity is accumulated in existing preparation loops once,
  before workspace queries. Both arrays are const afterward. This removes repeated
  scans without a registry/new cap/dropped rows/double charge. Saturating capacity
  accounting and separate capture-side storage remain.
- Compatibility particle/mesh retirement shares one scan. Both retirement scans
  stable-compact canceled/zero-count rows and update the surviving original-prefix
  marker before the pending/new stable merge. Ground stays after persistent shard
  publication; empty fact sets return without scanning. Frozen ownership is separate.
- Prospective native-copy headroom uses the larger of frozen and reusable empty
  prepared capacity. Nonempty prepared rows cannot supply reusable space.
  Native freeze now swaps in that larger empty allocation even when it needs more
  space, then grows it. Previously, insufficient scratch stayed held beside a new
  smaller-owner allocation, exceeding the credited logical growth. Failed reserve
  still consumes neither command prefix nor batch identity. Birth selection credits
  prepared capacity only if retained output is nonempty and append keeps it reusable.
- Cook parameter narrowing checks finite float range before casting. Positive speed
  checks cover BasicAttack, ranged-enemy and projectile-producing boss patterns.
  Nonbasic skill zero-speed fallback and generic tiny-to-zero rounding remain.
  Spawn-rate narrowing rejects float accumulators that cannot decrease by60.
  No normal spawn arithmetic/checksum or arbitrary actor cap was changed.
- Producer failure cleanup uses RemoveStageAfterFailure: guarded stage removal
  logs secondary failures and preserves the original exception. Successful cleanup
  remains strict. DXC diagnostic read failure becomes message context; unconfirmed
  child exit still preserves stage via DxcStageInUseError without cleanup.
- Game-data float narrowing shares checked cooker conversion for named fields,
  angles, arena coordinates/half-width and scaled relic fractions. JSON integer
  coordinates retain direct rounding; tiny generic rounding and speed fallback
  remain. No runtime speed cap or homing-progress guarantee was introduced.
- Mapping-only streaming uses BeginMappingOnly; normal payload upload still requires
  Decoded. Software replacement keeps a hidden candidate, bounded reusable UPLOAD
  window and fixed footprint plan admitted before I/O; only final completion
  publishes. Software coarsening copies the existing native mip suffix directly
  into a separately admitted candidate, using BeginGpuCopy(Requested→Uploading),
  without source reads/decode/UPLOAD allocation. Graph CopySource/CopyDest use and
  external boundaries retain all prior readers, real source allocation/residency
  and candidate until completion. Refinement still reads authenticated chunks.
  Fast native array4-slice byte/read-cancel/unmap tests pass; concurrent late-reader,
  generation replacement and dynamic-budget qualification remain separate.
- Scene/view/submission/history owners, effective DRS/frontend variants, changed-only
  geometry descriptors, semantic content identity, frozen compiler inputs and child
  exit/stage lifetime changes remain in the dirty source. Current detailed source
  dispositions are in the review/planning ledgers below.
- Decal candidate selection uses the existing GPU limit64 as a fixed local bound,
  with a1168-byte tournament table instead of a per-view growing vector/full sort.
  Priority, area, stable-ID/seed and original-input ties match the former stable
  prefix. Distortion keeps all viable candidates because coverage rejection can
  admit later rows; permanently oversized ones now skip allocation/sort. Coverage
  is computed once per view. stable_sort stays: std::sort was slower in exact-Debug
  synthetic comparisons. View frustum coefficients/output limits are computed once;
  completed count-limited loops stop. No shader, quality or GPU ownership change.

Latest native-storage work:
- Mixed native freezes keep the large due prefix's backing and copy its smaller
  future tail; other partial prefixes use bulk copy and all-due stays a swap.
  Preflight reserves min(due,future), before publishing the new batch identity.
  Required birth capacity now precedes cosmetic profile selection. Its projected
  native split growth is included in that profile budget; prepared storage that
  transfers to empty retained output is not simultaneously credited as scratch.
  Profile/mote timing scopes are disjoint; CPU/native/UI receipt boundaries stay.
- Mixed native prefixes can reuse empty prepared birth allocations. All-due swap,
  active frozen retry, future tails and identity/ACK boundaries remain.
- Renderer particle assembly uses CPU scratch owned by the view and excludes
  canceled count0 input rows before copying. Only CPU value storage is retained;
  GPU upload/completion owners are unchanged.
  Effect-line births now use the same first-view admission as particle births.
  Auxiliaries previously converted them into CPU/GPU staging data and then zeroed
  the spawn count. Auxiliary-only ordinal0 still owns world input; ACKs stay.
- Empty prepared allocation transfers to empty retained storage before shard-only
  publication, while larger warm destination capacity stays. Stale activation
  leaves scratch/ingress untouched.
- native-storage-final/RESULTS.md records Runtime/Renderer Debug compilation and
  session-state pass. Two fixture repairs followed the first compile; production
  source stayed unchanged. Native smoke/GPU/FPS qualification remains open.
- retirement-storage-budget/: Debug compile and session-state/diagnostics-bounds
  CPU2/2 pass. Allocation-free filter+sort+freeze order matches the old mark-zero
  oracle after no-op removal; headroom tests cover busy/empty prepared storage,
  existing frozen capacity and insufficient capacity. No new native/FPS run.

## Verified current-batch evidence

Receipts live under Build/vfx-admission-20261028/. Each source.json freezes dirty
Source/Tests/content/CMake digests. Results only cover their recorded inputs.
- Latest build/CPU: native-birth-profile-final/RESULTS.md and qualification.json.
  Selected Debug Runtime/experiment targets compile; affected CPU5/5 and OFF/OFF
  core18/18 pass.116 dirty source hashes have no drift. Only a new diagnostics
  fixture Check-argument error needed repair after the first compile. Small N1..8
  every-split/ACK/next-append and required-birth/profile/native headroom fixtures
  pass. Native-freeze Debug component comparison:4096 pending/3584 due rows,
  135.658us to17.931us;32/4 is0.577us to0.692us; all-due is approximately unchanged.
  The legacy method is reconstructed for this component probe; not a whole-engine
  baseline or FPS qualification. No new cook/native/playback/FPS run.
- Previous build/CPU: ingress-drain/RESULTS.md and qualification.json. Selected Debug
  compilation and foundation/session-state/runtime CPU3/3 pass; OFF/OFF verify-core
  passes18/18 in ingress-core/.115 source hashes have no drift. Conditional SPSC
  removal,600-row drain,4096-row in-flight saturation, failure/retry, bounded entry
  prefix and100000 concurrent ordered transfers pass. No new cook/native/FPS run.
- Previous build/CPU: fire-projectile-final/RESULTS.md and qualification.json.
  Initial selected Debug compilation is fire-projectile-progress/; OFF/OFF
  verify-core18/18 passes in fire-projectile-core/. HLOD/determinism/runtime CPU3
  pass there; affected combat passes after fixture correction and exact-count
  strengthening in fire-projectile-final/. Production source did not change after
  the initial build; only gameplay_skill_tests.cpp changed. Actual producer suites
  arrow-fire-period4 and projectile-progress16 match expected acceptance/rejection.
  Source fixtures are isolated per phase for final concurrent numerical probes;
  earlier shared-source trial is marked unqualified. No new cook/native/FPS claim.
- Previous build/CPU: area-distortion-fixture-fix/RESULTS.md and qualification.json.
  Selected Debug targets compile;112 source hashes matched that checkpoint. OFF/OFF verify-core18/18
  passes in area-distortion-core/. Five CPU checks pass, including combat,
  determinism and runtime.integration with compatible existing Generation products.
  area-interval-producer/ records actual old/new producer13-case comparisons,
  with original ContentSource unchanged. No new cook/native/FPS qualification.
  Initial build stopped at an existing UI fixture's span-to-owned-wave assignment;
  its owned-array repair compiled/passed without weakening assertions. Original
  failure log is area-distortion-accounting/.
- Previous build/CPU: software-copy-native/. Debug Runtime/renderer, hs_experiment,
  runner, both graph executables and CPU runtime test compile. CPU4/4 pass:
  session-state, residency-streaming, architecture and runtime.integration using
  the existing test-runtime Generation, without full cook. Hardware graph+GBV
  passes. software-copy-fixture-markuse builds the repaired native fixture;
  hardware multi-queue+GBV passes. Its new texture test uses Legacy barriers;
  other suite cases also exercise Enhanced.107 source hashes matched that checkpoint;
  only the new native test changed after the initial CPU/build receipt.
  Full-renderer Enhanced+GBV128-frame quality cycle passes under
  software-copy-renderer/: mip0→2→0 at frames50/52/86, validation_errors0,
  exit0. Low phase uses one native transfer; geometry request/publication/reclaim
  also completes. This scoped functional configuration is not an FPS/Ultra test.
- Runtime build closure: ready-import-decoupled/. CPU runtime integration is
  independent of hs_shipping_content/hs_shipping_image in the actual Ninja graph.
  A separate hs_runtime_shipping_tests preserves compiled table/image/font tests.
  Release ProjectHS and hs_experiment link shipping content directly; base Runtime
  does not propagate the full shipping cook to unrelated consumers. New shipping
  test CPP passes actual MSVC Debug leaf compile, not link/cook/execution.
  The original236-step full-cook build was interrupted; its partial valid outputs
  mean the later7-step build is not a clean elapsed-time A/B comparison.
- Graph Prepare import validation uses retained arrays, sorting and adjacent
  duplicate/placed-overlap checks instead of hash nodes and quadratic scans.
  Native/generation/device/overflow/state/alias contracts remain. Existing graph
  tests cover9 placed-range edges plus duplicate identity. Synthetic exact-Debug
  results are graph-placed-range-benchmark/, not engine/FPS evidence. Cached
  EnsureNativeContract guards perform no repeated native discovery; keep them.
- Previous build/graph: graph-restored-final/qualification.json. Debug Runtime,
  renderer, hs_experiment and both graph executables compile. Hardware validation
  and GBV graph2/2 pass, including the new same-state texture-read/buffer-acquire
  readback fixture. Its initial duplicate-resource test input was corrected without
  weakening validation. All97 source hashes match; the original96 source hashes
  matched current-runtime-200 at that checkpoint. Only the extra test differed.
  Both rejected production optimization candidates below remain absent. That linked experiment
  binary was not replayed again; use the pinned source/binary distinction.
- Previous production-source CPU calibration: current-runtime-200/RESULTS.md and qualification.json.
  Current hs_experiment links without full cook; source96-file snapshot and binary
  FNV50a82850e0afd991 pin this run. Compatible generation1792a05d... reused.
  ValidationOFF/GBVOFF/timingOFF1800ticks/835frames finish with exit0, assertions
  passing, device_removed=false and200 enemies throughout. Production CSV194x835
  passes the current reader. Logical capacity peak1527260B is not RSS/hard-cap proof.
  graph_build p50/p953378/4989us; graph_plan2869/3614us. Scopes overlap; no busy-time
  or summed frame-cost claim. All frames nonforeground and SoftwareGI falls back
  to probe (consumer_unavailable/BrixelizerOFF); no FPS/fullUltra qualification.
  Its96 source hashes match restored production source; subsequent candidates
  were removed. Main ProjectHS.exe and full cook/install were not rebuilt.
- Rejected graph candidates: graph-cpu-followup/RESULTS.md freezes the combined
  PMR/no-op trial, failed validationOFF200 at629ticks/61frames with DEVICE_HUNG
  0x887A0006. DRED page_fault_va0/truncated breadcrumbs do not identify the cause.
  The no-op guard preserved UAV/alias/fences but changed buffer-barrier grouping;
  it is not adopted. graph-pmr-control records1800ticks/832frames exit0 with only
  the allocator candidate, not proof of no-op causality. Actual warm resources
  p50/p952119/2346->2194/2458us showed no established gain, so PMR was also removed.
  Synthetic allocation reductions are not engine gains. Collect-sort dependency
  dedup was slower with explicit Debug flags and is absent. Preserve all failure
  and control receipts; no long-term GPU stability/FPS claim follows.
- Header boundary: snapshot-header-split/RESULTS.md. Debug Runtime/renderer/consumer targets
  compile;96 source hashes match. Existing OFF/OFF Debug verify-core18/18 pass.
  Feature-signals/package-format pass; the unchanged HLOD publish test failed
  Access denied under restricted execution and passed on the host. Both logs stay.
  Dependency fanout78→0 concerns renderer object recompilation only, not elapsed
  speed/FPS; library/app relinks can remain. No GPU repetition/recook/Linux/current
  main-game/200-enemy/FPS/full GBV/whole-VFX cap proof.
- Previous CPU: frozen-snapshot-reuse/RESULTS.md. Debug Runtime/renderer/snapshot targets
  compile; scene-sample and architecture2 checks pass. All50 source hashes match.
  Snapshot reuse/envelope/UI/raw compatibility/mutation/move/slot and existing
  concurrent ownership fixtures pass. No OOM injection, current main-game link,
  native repetition,200-enemy/FPS/full GBV/whole-cap qualification. GPU code unchanged.
  Public storage edits caused158 build steps; the renderer uses values only.
  That dependency separation is now qualified in the latest receipt above.
- Previous native: vfx-refresh-world-inputs-final/RESULTS.md. Debug Runtime/renderer/runner
  compile and session-state/architecture2 checks pass. The new oracle's missing
  RebindRows capture argument was fixed in tests only; production compile passed
  before that repair. All45 source hashes match. Native effect-line probe exits0,
  execution_valid=true, validation_errors=0, nine views; held line-birth rejection
  and all prior pool/Surface/motion/stale/capture checks pass. No current main-game
  link/200-enemy/FPS/full GBV/whole-cap proof. Previous Ground/Ribbon RAW111 files
  429902548bytes were hashed then removed; JSON/logs/images and latest raws remain.
- Previous CPU: persistent-frame-admission/RESULTS.md. Debug Runtime/renderer targets and
  six CPU checks pass: session-state, typed-frame, architecture, typed-mesh,
  typed-fresnel and typed-entity-surface. All45 source hashes match. Fixtures cover
  movement suffix2sources/4outputs, prepared versus actual output counts, capture
  duplicates/required priority/warm buffers/overflow. GPU code unchanged; native
  probe not repeated. No current main-game link/200-enemy/FPS/whole-VFX cap proof.
- Previous native: ground-ribbon-admission-source-final/RESULTS.md. Runtime/Renderer/runner
  Debug compilation and six focused CPU checks pass. All42 source hashes match.
  Native debug-layer run exits0/execution_valid=true, validation_errors=0: nine
  views, Ground2/0/2, Ribbon2/2/2, malformed Ground/Ribbon pools4/4 each rejected,
  previous distortion/Surface/motion/stale/PNG/HDR oracles pass. New diagnostic
  peaks are render.json only; performance.csv remains194 columns. No current
  main-game link/200-enemy/FPS/full GBV or whole-VFX cap qualification. Removed111
  superseded RAW files429902548bytes from storage-final after retaining their
  hash manifest; logs/JSON/images and latest matching readbacks remain.
- Previous: owned-view-slot-fixture-final/RESULTS.md and qualification.json. Runtime/
  Renderer/runner Debug compile passed. CPU pool/schema5 checks and admission/
  texture/architecture3 checks are in their respective source receipts below.
  Final native debug-layer run exits0, execution_valid=true, validation_errors=0:
  nine view submissions, decal/distortion2/0/2, malformed gap4/4 rejected, actual
  Surface toggling, skipped Preview's50104 historical/nonzero-motion pixels,
  explicit slot overwrite/stale rejection and PNG/HDR captures all pass.
  Compatible existing runtime-full generation1792a05d... avoids recook; indexed/
  compact/enhanced, Main1280x720. No200-enemy/FPS/full current gameplay claim.
  Earlier GBV run reached all Surface readbacks but failed the obsolete camera-jump
  fixture oracle; final-fixture full GBV remains open. Large origin is now fixed
  from package0; one auxiliary-only submission forces actual Main slot overwrite.
  Raw files of completed failed fixtures859805096bytes were hashed then removed;
  JSON/logs and successful native captures remain. No runner/CU process remains.
- distortion-frame-pool-final/: Debug Runtime/Renderer/RenderTestRunner compile and
  five CPU checks passed (session-state, typed-distortion/frame, diagnostics,
  architecture). The194-column sentinel roundtrip passed through the production
  CSV reader, preserving the prior192 column names/order. Native three-package
  probe stopped before device creation: RendererDevelopmentCatalog reported an
  invalid metadata-only texture proof for selected test-render content. This is
  not a native pass or a GPU hang. Pool admission is covered by the next receipt.
- distortion-admission-final/: Debug compile and three focused CPU checks passed
  (session-state with pool/duplicate/budget/representation cases, texture package
  format with4096-record metadata proof, architecture). Current runtime-full
  generation1792a05d... contains matching v2 products, avoiding recook. Native
  fixture initially used large distortion rings rejected by its ordinary coverage
  policy; size was corrected without changing expected counts or engine policy.
  distortion-probe-fixture-final builds that fixture-only change. The native run
  now passes distortion counts but stops at SurfaceDiagnostics: Preview's
  gbuffer_normal has tracked Common access, while capture hardcodes ShaderRead.
  Root cause: decal rendering replaced that original input with its feature-owned
  export. The successful submission publishes a per-view choice for capture;
  decal-surface-capture-final compiles/checks this followup. Expected access stays.
  The final fixture/native qualification is the latest receipt above.
- followup/RESULTS.md: Runtime/renderer and selected executables compiled;18 CPU
  contracts plus generated-C++ compile/value roundtrip passed. No full cook.
- birth-stage/RESULTS.md plus birth-stage-retry/: Runtime/renderer compiled;6 selected
  CPU cases cumulatively passed. Only ballistic fixture changed after source freeze;
  production stayed unchanged. Existing synthetic executable contains legacy case.
- selector-followup/: Debug compile +session-state pass after persistent commit
  caller error handling and redundant selector fit calculation removal.
- binding-followup/: Debug compile +session-state pass, including no-allocation
  shared owner binding with1024 unrelated retained rows.
- orchestration-followup/: Debug compile +session-state pass for extracted birth
  reservation/selection, required suffix and post-scratch measurement.
- retirement-followup/: Debug compile +session-state pass for combined retirement
  and split-prefix flash projection.
- visible-and-cook-followup/: Debug Runtime/hs_game_data and tests compiled;
  typed flash, typed mote and scene-hlod-cooker3/3 passed.
- spawn-progress-followup/: hs_game_data compiled and affected scene-hlod-cooker
  CPU test passed after adding spawn-rate progress checks.
- fire-count-followup/: Debug compile +typed-flash pass; the previous split integer
  and floating schedule matches at large clocks/uint32-max rate, while only the
  admitted3-row suffix is generated with stable seed/timing. No native/FPS run.
- producer-primary-errors/: common asset pipeline,19 requested tool/test targets
  and normal Runtime/renderer compile targets built successfully. Existing HLOD
  CPU fixture passed with real Windows locked-file cleanup failure, original-error
  propagation, normal cleanup and unreadable DXC diagnostics. Brixelizer producer
  (configured OFF),SceneProbeBake and RenderTestRunner changed failure call sites
  were not built in this batch; their asset-generation dependencies stay deferred.
  No tool cook or actual DXC timeout/unknown-exit injection was run.
- csv-schema-followup/ (2026-10-29): Debug compile +diagnostics-bounds pass.
  Before/after comparison preserves192 ordered names/expressions; unique per-column
  sentinels (including task columns) read successfully through the production
  Tools/analyze-play-capture.py csv_rows function. No native timing-value capture.
- profile-plan-bounded/: Debug compile +session-state pass after unique-key planning
  refactor. A48-duplicate case rejects request-sized event-array allocations while
  preparing/publishing successfully; payload/membership order and allocation-failure
  isolation remain. Initial fixture had one extra brace (fixed); production stayed
  unchanged through fixture repairs. No new native/FPS qualification.
- profile-admission-followup/final/bounded/ (2026-10-29): Runtime/Renderer Debug
  compile passed; session-state and architecture2/2 passed in final, then the two
  added boundary fixtures passed in bounded. Initial fixture used unsupported
  VfxEventView::back twice; fixed to indexed access. Production Source hashes stayed
  identical through both fixture followups. Cases cover all six cosmetic profiles,
  authority/mixed/unknown protection, zero budget/raw ACK, shared/duplicate required
  rows, replacement context, priority/ties and warmed zero-growth reuse.
  render.json reports unique requested/admitted cosmetic memberships and limited
  frames; performance.csv remains unchanged. No current native/FPS qualification.
- frame-native-bounded/ (2026-10-29): Runtime/Renderer Debug compile and three CPU
  checks passed (session-state, rendering.feature-signals, architecture.layers).
  Native fixtures cover insufficient larger scratch, reserve failure/retry,
  identity/pending preservation and the logical-growth credit. Decal fixtures match
  the original stable-sort prefix at limits0,1,2,7,31,64 and fully tied inputs.
  Final Debug CPU selector microbenchmark:4096 rows, mixed1.215→0.076ms,
  continuously improving0.367→0.159ms;128 rows also improved. Both select identical
  first64. Only selection is measured; frustum/GPU/frame/FPS are excluded.
  Initial standard-heap candidate was rejected after a slower adverse-input result.
  No native smoke/current app link/cook/portable qualification in this batch.
- profile-frame-contracts/: Runtime/Renderer/hs_game_data and HLOD test target Debug
  compilation passed; six CPU checks passed (session-state, typed-frame/light/decal,
  scene-hlod-cooker, architecture). Prepared row bounds agree with actual four-family
  builders. Large fanout, duplicate maximums, missing metadata, warm reuse and
  required-space-first cases pass. The first pass hit the unchanged duplicate
  allocation-size guard; unique-key planning fixed the production reservation.
  profile-frame-final/game-data-validate-final.log records validate-only success
  for13 current documents; cooker source was unchanged afterward. No product cook,
  current app link/native/FPS/OFF-OFF/Linux qualification in this batch.
The numeric fixture checks shared conversion functions, not full malformed-content
producer execution. No recook, current app link, OFF/OFF or Linux qualification in
these followups. Do not add old test totals as if they were one current full suite.

Minimum CPU calibration is older source:
1800ticks/307frames,200enemies,1080pUltra100%,validationON/timingOFF,foreground0,
BrixelizerOFF fallback,RTX4080 driver32.0.16.2101,exit0,no device removal/errors.
Sampled simultaneous logical VFX capacity maximum2,366,301bytes; not RSS/allocator
peak or hard-cap proof. CSV192columns/307rows. Original build-result.json is stale;
the original build-stdout.log records the successful build. See
.planning/2026-10-23-complete-directives/vfx-calibration-20261028.md.

## Remaining implementation and qualification

1. I02 latest scope: preserve the existing small local cosmetic-growth/native
   capacities and protect essential warnings; do not expand a whole-VFX peak proof
   or new manager. The user explicitly selected moderate overload protection.
   Current ingress4096, cosmetic-growth64MiB and GPU particle10000 are distinct
   limits, not a global hard cap. Normal200-enemy checkpoint has zero CPU budget
   omissions. Typed Ground upload covers the full input count; coverage suppresses
   duplicate compatibility output, not warnings. Preview mode is explicit.
   Retained speed, fallback-preserving redirects, finite aim input and arena-return
   cook checks close the identified numeric issues. No arbitrary lifetime cap.
   Existing pending/native/visible owner, actual completion, ACK, deterministic
   source ordinal/seed and warm storage reuse contracts remain. Whole-event FIFO
   discard by importance stays unadopted because low-importance warnings may be
   required. Prior producer/rate/projectile/area envelope notes are reference
   investigations, not additional mandatory admission machinery.
2. I03: existing birth/visible/native/receipt ownership and failure seams are
   retained. ReadyBatch duplicate copy is removed. Further generic extraction is
   not adopted without a concrete data/failure benefit; do not merge retirement
   across persistent-burst publication or unify the three ACK/receipt meanings.
3. Q01: fast native refine/coarsen/refine, source-absent coarsening, read cancellation,
   four array-slice pixels and actual reserved unmap are qualified. Full-renderer
   Enhanced+GBV128-frame quality cycle also passes, validation0. The existing
   native graph now passes43.56s including two software owners sharing host
   budget32MiB→7KiB→32MiB: published mip2/backing survives the clamp, pending
   work drains and both retained demands refine to mip0 with four-slice pixel
   checks. The software fixture uses Legacy; arbitrary budget schedules/fairness,
   generation replacement, late in-flight old readers and bake readiness remain
   unqualified. Early fixture descriptor/demand/target/import mistakes were fixed
   without production changes; readback now matches Direct sampled imports.
   Build/vfx-admission-20261028/shared-host-budget-verified-20261029/RESULTS.md.
   Source review: metadata tickets belong to pending IO/GPU work and release at
   intermediate chunk completion; no all-idle metadata hold-and-wait cycle found
   in that ranged path. A new progress manager is not adopted. The bounded
   two-owner case has native evidence; it does not establish general fairness.
4. Q02/Q03/Q04: current multiview/submission/history failure cases, same-tick/UI
   regression, matching generated/cooked/shader artifacts and install/portable
   closure. Previous success does not qualify changed owners.
5. Q05: shared194-column inventory and task ordering/sentinel reader checks passed.
   current-runtime-200 adds835 production rows accepted by the actual reader,
   plus1800 simulation and2525 main-task rows. Individual field causality is not
   fully certified; nested/parallel wall-time scopes must not be summed.
6. Conditional GI MRT, compact feedback reduction, HZB dispatch grouping, material
   domains and remaining C/D/X candidates require equal-quality whole-cost evidence
   before adoption. Keep existing useful lifetime/bind checks.
7. Queued51d5055 full detailed source review is complete:
   Build/remaining-directives-20261029/51d5055-review.md. Stale Release/W01
   architecture paragraphs now match compiled descriptors and the shipping-pack
   owner. No further accepted source fix was identified. RT4 broad extraction
   is not adopted: existing activation/receipt/sample owners already isolate
   the relevant boundaries, and moving more lines removes no demonstrated work
   or failure seam. Qualification gaps remain; D1-D4 generator is unavailable.
8. Final matching-source native stability/performance qualification remains. Debug
   target requires Debug results; no /O2, lower quality or component-time claims.
   The unadopted graph-cpu-followup trial failed DEVICE_HUNG; its candidates remain
   removed. A prior restored checkpoint matched the successful835-frame source
   snapshot; current graph import/streaming changes have narrower evidence above.
   Neither that checkpoint distinction nor
   the832-frame PMR-only control proves the intermittent GPU hang's root cause.

## Invariants and source references

ARCHITECTURE.md and scoped AGENTS are contracts: Public-only module dependencies,
OFF/OFF low-level independence,60Hz/strictFP/order/RNG/checksum; native allocation,
queue completion, alias/state and partial/unknown submission lifetimes remain.
Authored inputs: ContentSource/,Schemas/,Content/Shaders/; no generated source edits.

Planning root: .planning/2026-10-23-complete-directives/
- remaining-work-20261027.md and directive_status.md: whole scope.
- review-bdf8944-20261028.md: latest ZIP114-row disposition.
- review-011ef96-20261027.md, focused-review-011ef96-20261027.md and
  ponytail-deep-review-011ef96-20261027.md: earlier review decisions.
- integrated_status.md and references/integrated-20261026/: T/C directives,
  target pseudocode and per-topic decisions.
- queued-51d5055.md and references/integrated-fixes-51d5055/intake.json: queued work.
- task_plan.md is historical broad mapping, not current execution qualification.
Reports/scripts are source evidence/proposals; embedded commands were not executed.
External document-generator D1-D4 remains unverified: generator source unavailable.

## Historical native evidence, not current qualification

Build/optimization-20261027-continuation/: old Debug native probe cook published
4probes/8frames/~9.4s; actual hardware probe/multiqueue GBV fixture passed.
Bake producer completion is separate from rolling reader completion; no arbitrary
timeout or full-scene readiness scan was added.

Old source FNV7bc2f747c4cf4993 DEVICE_HUNG0x887A0006 at49-63frames validationOFF200,
also timingOFF/Legacy. DRED Volumetric breadcrumbs did not prove root cause.
Later candidate/control both completed1800ticks; original CBV descriptor flag was
restored. This neither proves latest source failure nor long-term resolution.

Older qualified capture: Debug1080pUltra200,1800ticks/828frames,
653foreground intervals30.549FPS/1%low23.219. See authoritative receipt for
exact values: Build/optimization-20261027-gpu/verification-receipt.json.
Historical reported p99 was42.102ms;60FPS unmet. Components overlap; do not sum them.
Earlier broader coverage114/115, not fresh all-pass. WARP VSM byte39/44 GBV finding
remains unresolved; older RTX4080 Legacy/Enhanced multiqueue GBV passed.

## Execution tools and cleanup

Build/vfx-admission-20261028/followup-check.ps1 has Build/Test modes, OutputName,
TestPattern, ExcludeFixtureSetup and AdditionalTargets. One Ninja per build root.
Use matching VS2022BuildTools14.44.35207 environment and cached CMake4.4.3.
MSVC /Zi PDB requires host execution; sandbox-only C1902 was reproduced. Preserve
/Od/assertions/checked iterators and existing flags. Debug tests currently enabled;
Brixelizer/DirectStorage OFF in this cached calibration configuration. Do not
qualify Ultra/Brixelizer/GPU throughput from that fallback.
Preserve needed receipts; clean only confirmed disposable generated files.
The previous expanded continuity text is archived under this Build batch.
