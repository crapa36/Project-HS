# DEV_STATE

AI implementation continuity only. Current base:
d29b4af51793739401e986a0c6c1199fb1c60a53 (Bound VFX growth and harden content pipelines).
The dirty worktree is authoritative. Preserve unrelated changes and
.codex-remote-attachments/. No new commit/push/worktree is requested.

## Objective and execution policy

Goal ACTIVE: finish accepted reviews/directives and Debug /Od/assertions/checked
iterators,1080pUltra100%,200enemies,60FPS. Not achieved.
Source edits first; batch narrow verification. The authorized minimum CPU-memory
calibration completed; do not repeat broad GPU/FPS/cook per small change.
Subagents: explicit Luna/high only. No Sol subagents. No Computer Use session.
W12 per-queue timestamp/readback separation remains explicitly deferred.

## Current implementation

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
  Cosmetic masks are prepared once per catalog generation. Ground, EntitySurface,
  PointRate, replacement contexts and unknown/authoritative contributors stay required.
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
- Cooker now performs the necessary axis float-progress check for positive legs,
  preserving BasicAttack speed fallback and nonpositive-leg retirement. It covers
  all player skills including Trap via DifferentSkillTracker, Ricochet search reset,
  ranged enemies and projectile-producing boss patterns. Original inputs remain
  unchanged in actual producer16-case before/after tests. This does not establish
  all-direction/homing lifetime, arena return, repeated legs or a whole-VFX cap.
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

1. I02: whole required/future/native bounds remain incomplete. Optional retained
   profile selection is local; required cardinality and subsequent frame/GPU bounds
   are not certified. Existing
   cosmetic64MiB allowance, ingress4096 and GPU particle10000 are local limits.
   Derive producer rate/lifetime/fanout from loaded rules and recipe ranges;
   protect essential warnings before CPU commit. No silent drop or arbitrary cap.
   Automatic ordinary schedule has finite content bounds, debug spawning differs.
   See vfx-producer-bounds-20261028.md for source facts and formula gaps.
   vfx-projectile-envelope-20261029.md derives reset/return legs from loaded rules;
   its nominal tick totals still need float progress/rounding qualification.
   vfx-producer-rate-envelope-20261029.md records cadence/fanout composition and
   pending delays; formulas are not yet an implemented mandatory admission contract.
   vfx-area-status-envelope-20261029.md adds area/status/link lifetimes and the
   four-active-status-identities-per-live-enemy bound. Rounded-zero intervals are
   rejected; boss and both derived fire intervals are consumed. Necessary axis
   progress failures are rejected, but direction/homing/multiple-leg bounds remain.
   Raw ingress now drains its published prefix without the arbitrary256 limit.
   Conditional pop keeps a blocked/unallocated VFX head in the producer FIFO while
   consuming an earlier non-VFX prefix even when raw slots are full. Existing8192
   producer/4096 pending+in-flight bounds and loss diagnostics remain. Backpressure
   at the first blocked VFX still delays later rows; audio has its own FIFO.
   Whole-event discard by importance is not adopted: current metadata proves
   profile/legacy-emitter cosmetics, while an importance2 enemy warning is required.
   Preserve FIFO/CPU ACK and use output-level admission; no new queue/arbitrary cap.
   Fixed-row frame families, Ground/Distortion angle pools, Ribbon source/output
   pools and prospective capture capacity are covered by current admission.
   Required birth reservations and projected native split growth now precede
   cosmetic profile quota; future rows cannot spend that same headroom twice.
   Whole required/future producer retention and native/GPU peaks remain open.
   Persistent base output is preserved,
   not cosmetically reselected. Logical growth excludes allocator-transient peak/RSS.
   The necessary axis check alone cannot prove homing/range progress; do not add an
   arbitrary scalar cap as a substitute for the missing numerical proof.
2. I03: existing birth/visible/native/receipt ownership and failure seams are
   retained. ReadyBatch duplicate copy is removed. Further generic extraction is
   not adopted without a concrete data/failure benefit; do not merge retirement
   across persistent-burst publication or unify the three ACK/receipt meanings.
3. Q01: fast native refine/coarsen/refine, source-absent coarsening, read cancellation,
   four array-slice pixels and actual reserved unmap are qualified. Full-renderer
   Enhanced+GBV128-frame quality cycle also passes, validation0. Small/dynamically changing budgets, generation
   replacement, late in-flight old readers and bake readiness remain unqualified.
   Source review: metadata tickets belong to pending IO/GPU work and release at
   intermediate chunk completion; no all-idle metadata hold-and-wait cycle found
   in that ranged path. A new progress manager is not adopted. Two-owner dynamic
   budget/native qualification and fairness are not proven by this source review.
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
7. Finish earlier accepted directives, then queued51d5055 detailed source review.
   Intake read; do not call that review complete based on overlapping fixes.
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
