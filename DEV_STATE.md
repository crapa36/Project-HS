# DEV_STATE

AI implementation continuity only. Current base:
bdf8944bcaf64401c10886b87ac754dc9987bf21 (Prepare VFX contracts and streamline frame paths).
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

- performance.csv header and numeric row writer expand one explicit private field
  inventory, including the task-column position. Current192 name/expression pairs
  exactly match the prior writer. No per-frame schema validation or new registry.

- Raw ingress has a combined4096 pending/inflight bound, adaptive backing and
  ready-prefix staging. CPU activation ACK, native submission ACK and Main Present
  receipt are separate. Failure still terminates the session; no new retry manager.
- ActiveEffectStore retains canonical rows, masks and indexed profile views.
  Owner-bound sparse refresh preserves birth/capture rows. Empty unchanged compact
  returns early; binding-ID capacity follows active bindings rather than all rows.
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
- Compatibility particle/mesh retirement shares one scan. Both retirement scans
  stable-compact canceled/zero-count rows and update the surviving original-prefix
  marker before the pending/new stable merge. Ground stays after persistent shard
  publication; empty fact sets return without scanning. Frozen ownership is separate.
- Prospective native-copy headroom uses the larger of frozen and reusable empty
  prepared capacity. Nonempty prepared rows cannot supply reusable space.
- Cook parameter narrowing checks finite float range before casting. Positive speed
  checks cover BasicAttack, ranged-enemy and projectile-producing boss patterns.
  Nonbasic skill zero-speed fallback and generic tiny-to-zero rounding remain.
  Spawn-rate narrowing rejects float accumulators that cannot decrease by60.
  No normal spawn arithmetic/checksum or arbitrary actor cap was changed.
- Producer failure cleanup uses RemoveStageAfterFailure: guarded stage removal
  logs secondary failures and preserves the original exception. Successful cleanup
  remains strict. DXC diagnostic read failure becomes message context; unconfirmed
  child exit still preserves stage via DxcStageInUseError without cleanup.
- Mapping-only streaming uses BeginMappingOnly; normal payload upload still requires
  Decoded. Software replacement keeps a hidden candidate, bounded reusable UPLOAD
  window and fixed footprint plan admitted before I/O; only final completion
  publishes. Native cancellation/generation/budget transition qualification remains.
- Scene/view/submission/history owners, effective DRS/frontend variants, changed-only
  geometry descriptors, semantic content identity, frozen compiler inputs and child
  exit/stage lifetime changes remain in the dirty source. Current detailed source
  dispositions are in the review/planning ledgers below.

Latest native-storage work:
- Mixed native prefixes can reuse empty prepared birth allocations. All-due swap,
  active frozen retry, future tails and identity/ACK boundaries remain.
- Renderer particle assembly uses CPU scratch owned by the view and excludes
  canceled count0 input rows before copying. Only CPU value storage is retained;
  GPU upload/completion owners are unchanged.
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
2. I03: activation/visible/native/receipt boundaries are partially extracted;
   finish coherent ownership/failure seams without duplicate state or generic VM.
3. Q01: current native streaming refine/coarsen/refine, small budgets, generations,
   cancellation and in-flight old backing; bake temporary/permanent readiness.
4. Q02/Q03/Q04: current multiview/submission/history failure cases, same-tick/UI
   regression, matching generated/cooked/shader artifacts and install/portable
   closure. Previous success does not qualify changed owners.
5. Q05: shared192-column inventory and task ordering/sentinel reader checks passed.
   Latest-source native capture of actual field values remains; a synthetic
   sentinel is not evidence of real per-stage timing or workload coverage.
6. Conditional GI MRT, compact feedback reduction, HZB dispatch grouping, material
   domains and remaining C/D/X candidates require equal-quality whole-cost evidence
   before adoption. Keep existing useful lifetime/bind checks.
7. Finish earlier accepted directives, then queued51d5055 detailed source review.
   Intake read; do not call that review complete based on overlapping fixes.
8. Final matching-source native stability/performance qualification remains. Debug
   target requires Debug results; no /O2, lower quality or component-time claims.

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
