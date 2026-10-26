# DEV_STATE

AI implementation continuity only. Workspace is dirty; preserve staged and unstaged
edits. No commit/push/worktree requested. Current observed HEAD: 51d5055
`Expand packed renderer content and frame systems` (2026-10-26 observation).

## Objective and execution policy

Goal remains ACTIVE: Debug /Od/assertions/checked iterators,1080pUltra100%,200enemies,
60FPS and all accepted earlier directives, then queued51d5055 review. NOT achieved.
No commit/push/worktree; preserve all dirty edits. No Sol subagents; current workers
are explicit Luna/high. Deferred W12 per-queue timestamp/readback separation stays
untouched; native resource/fence/state/alias and actual submission contracts remain.

## Current source batch (2026-10-27)

Current source is newer than the last qualified Debug checkpoint. Record:
.planning/2026-10-23-complete-directives/optimization-20261027-source-batch.md.
Newest continuation: optimization-20261027-continuation.md in the same directory.
ValidationOFF actual200 playback now reproducibly DEVICE_HUNG0x887A0006 at49-63
rendered frames; ordinary validationON900ticks/303frames passed. Turning GPU timing
OFF and using Legacy also fail; no valid current-source FPS comparison exists.
DRED shows partial VolumetricDensity dispatch6/8 with timingsOFF/Legacy. The
timingON first pending AO node is not proof of root cause. No proven queue/CBV/stream
ownership defect yet. No active Computer Use session; exact failure evidence lives
in Build/optimization-20261027-continuation/. Narrow earlier pass receipts remain
valid only for those workloads, not proof the stress game is healthy.
- T14 exact six-channel dirty ranges replace six-array uploads; frame serialization
  scratch is reused. Retained slots reuse the authoritative key map; only new keys
  allocate staging nodes, transferred on Commit. Allocation/rehash moves before
  actual submission. Full package-once/current-vs-view-previous GPU separation and
  removal of full previous CPU mirror remain incomplete.
- T16 authored/masked flags are owned by StaticGpuScene and rebuilt with its source;
  static/dynamic repeated scans short-circuit. Water/Hero use existing cached ranges.
- Capture CSV uses retained strings/to_chars without dropping columns/records.
  Schemas: performance179,GPU-live72,main87,simulation113,timeline6,GPU-pass12.
- T06 TXM/TextureMetadata v2 range digests flow through producers/linker/compiled+
  Dev metadata. Frozen admitted targets, one legal reserved mip step, retained
  prepared data and metadata scratch tickets; no-I/O reserved shrink/fallback.
  Native loose FILE-to-MEMORY requests batch per pump, with terminal status/event
  retention and one async verification. Packs retain verified Core file ownership.
  Full software-chain targets exceeding host cap fail explicitly; legal multi-read
  software assembly/range coalescing still need review before full T06 completion.
- T08 SceneProbeBake directly uses renderer; broad runner/relaunch dependency and
  duplicate layout/output decode/C++ HashInputs removed. Histogram/texture sidecar
  cache wrappers removed; Action cache retained. Catalog-less closure uses bounded
  package directory metadata; ghbin/shl1 fallback still reads complete decoder input.
  The former broad-child20minute timeout is deliberately not recreated; actual
  native errors/atomic publish/exception stage cleanup remain. Bake warmup/readiness
  under new mip progression is being reviewed before any quality-completion claim.

## Current verification / live build

Existing narrow architecture.layers + rendering.persistent-gpu-scene pass2/2:
Build/verification/verify-20261026T202423531Z-27956.log. Channel patches are compared
byte-for-byte to a full table serialization oracle, including abort and new backing.
Current package format test passes after sharing one SupportedVersion policy across
writer/full reader/directory proof, keeping volume v1 guards and shape bounds.
Native DirectStorage1.3.0 compile+actual I/O test passes with no SKIP in validation
root (RelWithDebInfo, HS_WITH_DIRECTSTORAGE=ON):
Build/verification/verify-20261026T205030635Z-33200.log,
Build/optimization-20261027-source-batch/native-io-receipt.json.
Requested cancellation ERROR_OPERATION_ABORTED995 is a cancellation result only;
other native failures still fail. Fixes also include AssetId namespace, test D3D12MA
header dependency and SceneProbe SelectedGeneration STRUCT forward declaration.
No broad test pass/current FPS is claimed from these narrow checks.

Central Debug build succeeded, using build-debug.ps1 under
Build/optimization-20261027-source-batch/. It built ProjectHS,hs_experiment,
hs_scene_probe_bake and4existing test executables together. Full native stderr is
in debug-errors.log; terminal exit0 receipt is debug-build-driver.log.
Current Debug selected checks pass5/5: architecture.layers, persistent-gpu-scene,
render-package-format, residency-streaming, runtime.integration. Initial sandbox
denied rename in the latter2; both passed on host retry, without source changes.
Logs: debug-selected-tests.log + debug-retry-tests.log. Existing native checks also
pass2/2: integration.render-smoke (180rendered frames+resize) and vfx.showcase.
Both render.json reports have0validation errors. Enhanced native GBV24frame check
also passed on RTX4080 with0validation errors and normal exit0. It took about448s;
this instrumented check is not gameplay FPS evidence. Capture logs are buffered,
so empty live CSV files do not prove that the first frame has not completed.
Receipt: Build/optimization-20261027-source-batch/verification-receipt.json.
Full115gate and new200-enemy FPS comparison were not repeated in this check batch.
MSVC host access is required for /Zi PDB server. Preserve /Od and checked iterators.
Temporary caches restored by restore-caches.ps1: Debug HS_BUILD_TESTS=OFF,
Debug DirectStorage=OFF, validation root DirectStorage=OFF. Both configure-only
operations succeeded, with no source/default changes or duplicate native build.
No Computer Use session. No current-source FPS claim.

T23 source correction now added: exact cached static material owners, sampled mip
identity/readiness, stale face reset and status-driven producer; canonical SPB1
revisions remain unchanged. SampleReady skips texture facts/scans. New source has
not qualified yet. GPU admission API still cannot prove a retained denial permanent;
no arbitrary timeout/speculative stale-budget failure rule was introduced.
T06 mapping-only shrink skips empty-payload decode and clears consumed range lists.
A single Volumetric CBV DESCRIPTORS_VOLATILE flag is an unqualified diagnostic
candidate; revert if the identical replay does not improve. No claimed root cause.
One central continuation Debug build is currently running, session1026 at last
observation, with scripts/logs under Build/optimization-20261027-continuation/.
Targets ProjectHS,hs_experiment,hs_scene_probe_bake,hs_render_test_runner,
hs_render_graph_multi_queue_tests,hs_residency_streaming_tests. Sources frozen while
native build runs; temporary Debug testsON must be restored afterward.

## Last qualified checkpoint (before this source batch)

Generation a1947bcd6b52ccb81080cb3eec19ed15db6c00c98c85bbeaa9dcb44692175631,
experiment FNV1a64 38201462c3311ddf. Debug1080pUltra200:1800ticks/828frames, allticks
200enemies, no crash/device removal/dropped input/presentation, VFX unsupported/
ribbon drops0. Ordinary-validation smoke180frames+resize passes with0errors.
653qualified foreground intervals30.549FPS,1%low23.219FPS,p9537.090ms,p9942.102ms;
60FPS unmet. GPU GBuffer6.932ms/GTAO3.456ms; CPU render28.131ms includes actual
frame-slot wait7.498ms; previous capture logging3.041ms. GPU828frame coverage full;
OS GPU samples14.853/29.979s only. Never sum overlapping component durations or
claim current source throughput from this older checkpoint.
Receipt: Build/optimization-20261027-gpu/verification-receipt.json.

Previous broader coverage114/115 across one full run and focused repairs, not a
fresh all115pass. WARP-only VSM byte39/44 GBV finding remains unresolved; actual
RTX4080 Legacy/Enhanced multi-queue GBV passed before this batch. AO fresh hardware
GBV776cases/160752texels is bit-exact for the retained horizon helper, including8
production reversed-Z cases. Removed masked-depth candidate had no demonstrated
benefit. Same-old-binary captures vary, so raw image diffs do not prove parity or
regression. Current graph import lookup is already linear/cursor based; no duplicate
optimization was added. Last foreground frontend reuse377/653=57.73%;63nonzero
compile-timer misses averaged7.462ms, requiring actual miss-cause review next.

## Authoritative requirements and active sequence

ARCHITECTURE.md + scoped AGENTS remain contracts. Keep Public module boundaries,
60Hz/strict FP/order/RNG/checksum meaning and OFF/OFF low-level independence.

Planning: .planning/2026-10-23-complete-directives/task_plan.md,
directive_status.md. New documents under references/integrated-20261026/:
- Project HS 전체 구조 개선 통합 작업 지시서.md (T01..23, C01..13)
- Project HS 통합 목표 구조 의사코드.md (sections1..19)
- intake.json preserves original identities.
The documents compare 0f03f5ac; current worktree is authority. Do not reimplement
old diagnoses already superseded. Required T work and conditional C candidates
are different; no generic registry/backend/ECS/executor replacement.

Full directive text has been read; current-code comparison/implementation remains.
T01 and T22 decisions: references/integrated-20261026/integrated-t01-decisions.md.
Source dispositions: integrated_status.md and integrated-t02/t03/t09/t10/t11/t20/t21-decisions.md.
T21 bounded unknown-capacity essential bootstrap source policy remains incomplete;
no suitable existing aggregate native cap was found, and no arbitrary cap was added.
Optional unknown/stale admission defers; retire/fallback progresses. UMA feature
observation is cached for the Impl/device lifetime. Its Luna worker has completed.

Queued follow-up only: Project-HS-Integrated-Fix-Instructions-51d5055.md, baseline
51d505504afce2abdde92725270cc14821b5a26b. Finish earlier accepted directive source
first, then compare/adopt this document. Intake scope/index read; full source
review pending. See .planning/2026-10-23-complete-directives/queued-51d5055.md and
references/integrated-fixes-51d5055/intake.json. Only the observed
volume_texture_package MakeAssetId direct-include build failure was repaired
during this explicit verification request (overlaps queued N1). Other new-document
source review/implementation remains queued.

Next coherent source units: remaining T04 runtime closure, T05 material ownership, T06 fixed texture range
transactions, T07 bounded startup/PSO digest, T08 dedicated probe producer;
T12/T13 concrete recipes,
episodes/frozen batch/clock/ACK; T14/T15 current scene/view history/direct view
owners; T16 cached material facts, T17 multi-LOD directory/legacy skin closure,
T18 proven-empty/ROI qualification, T19 current binding/scratch, T21 budget/ledger,
T23 SampleReady first. Final validation only after accepted source is complete.

## Current source changes (partial qualification)

Current qualification is partial, not blanket unqualified: see the current batch
record for exact passed/failed inputs. Subsequent repairs address Runtime log stack
usage, cancellation facing, VFX fractional tick age, audio fixture order, source
shader assertions, growth history expectations, and native probe graph versions.
Shared-source changes require focused current-input requalification before claims.

- W01 Release typed catalogs/pack, direct geometry upload ranges, material and
  animation descriptors, RuntimeON/ToolsOFF installation closure existed at the
  historical checkpoint. Stable AssetId remains identity; current dense RenderMesh,
  variant/material ordinal selection avoids duplicate deployment fields per actor.
- W01 Dev lazy metadata proof/admission and exact verified-byte boundary reuse are
  in source. Read/admit integrity, decoder semantics, native generation/format,
  lifetime and alias checks remain. Do not infer all old summary claims are current;
  inspect touched code. Apps/Common/renderer_geometry_catalog.hpp currently exists.
- W02 VFX atlas seven cooked bounds/count descriptors; recipe15. Final batch must
  regenerate matching generated CPP/pack and actual LOD products from current inputs.
  Do not hand-edit generated output or reuse historical recipe14 qualification.
- W04 actual producer previous/current sample retention independent of publication,
  one leased self-contained packet, final catch-up publication, scheduled boundary,
  frozen/scripted modes and clock epochs. Same-tick mutation/resume/pool-failure
  edges require the new T09 comparison; no extrapolation/tick dropping.
- W05 one package display time for actors/pose/camera/VFX, fractional analytic age,
  terminal-only clock, exact motion epochs. Rendered history commits after accepted
  graph/service submission BEFORE Present. UI commits only after successful Present.
- VFX store has reused linear dedup scratch/in-place compaction; future-session raw
  events no longer pop/reinsert every frame; dispatch uses reusable transactional
  vectors. T13 geometry_sector_index now participates in key/hash/equality/rebind;
  four-sector fixture source added. Full T12/T13 episode/activation/batch rewrite is
  NOT complete: profile materialization, legacy outputs and snapshot-tick ACK remain.
- W07 derived cast index, stable compaction rebuild only; due/sequence semantics6
  and replay5 rejection remain. T01 adds pending/delayed retreat future causes,
  skips unused ordinary membership in detailed=false and empty nonzero membership
  gather/sort; preserves actual gameplay membership and UI damage. Obsolete spatial
  candidate helper removed and oracle moved to EnemyQuery. T22 nav/process-scene
  reuse preserved; no C11 bake introduced.
- N07 Play/UpdateListener share existing fixed X3DAudio matrix scratch and last
  exact position/scaler-bit key; listener/device/release invalidates. Native matrix
  application stays per voice. Spatial timing now counts actual calculations;
  NativeVoice includes matrix application. Existing source fixture covers reuse.
- N09 full read-model copy moved to EXCLUDE_FROM_ALL hs_gameplay_development;
  free CopyReadModelForDevelopment(simulation, storage) uses public tick borrow.
  60 calls/10 test files adapted; ordinary Gameplay/Runtime cannot link dev leaf.
  Read-value types remain transitively public; no new actor mirror.
- T11 footstep production moved to every advancing tick, original45tick timeline,
  current blend/position, tick-as-sequence; render audio queue/dual drain removed.
  EnemyAnimationSample queried once/actor, safe release/recoil order, ordinary
  actors skip boss-action scans. Source-only fixtures and decision record exist.
- T03 storage now has sole void Add methods; duplicate Append/always-success bool
  propagation and dead Character header are removed. Values are stored unchanged;
  dynamic revision derivation remains at final displayed/canonical renderer owners.
  Static revisions derive once at scene creation. Missing scene returns concrete
  Presentation Result before packet mutation; Runtime preserves that error.
- T09 TickResult.world_changed reports actual same-tick UI/input mutation; no-op
  actions do not reproject. Current sample refresh does not rotate pair/advance time.
  Existing scheduled boundary/debt, latest-frozen resume and epoch paths retained.
- N24 Runtime startup is a thin request carrier; Renderer projects native options
  once. Runtime initial VFX quality reads typed startup request. Capability discovery
  caches value-only device/output admissions; choice/mode/vsync does not recreate
  vendor probes. Current Streamline failure and queue state overlay cached facts;
  display/HDR queries remain current. No SDK context retained by the cache.
- W09 pose-certified optional lower pages/resident slots, O(1) interval lookup and
  enclosure prepared at adoption; actual own-resident masks per six families.
  18 family/LOD draw ranges route skin, shadow, GI, Fresnel, Hero. Shared package
  locks first committed common LOD; per-view motion history remains separate.
  This is NOT yet T17's distinct-view LOD range directory or legacy skin removal.
  Actual cooked accepted page counts and asset quality remain unknown.
- Hero work capacity12 contributors/168 diagnostic rows per frame. W10 preserves
  four layers; conservative rect copy and zero-occupancy predication are in source.
  Clear/copy/query/guide lifetime stays unconditional where needed. ROI native
  quality/total-cost qualification and new T18 offscreen empty proof remain.
- B05 fine VSM count/scatter now shares conservative projected sphere XY interval
  binary search, row/word request mask intersection, original exact DrawCandidate;
  fixed page/command budget/admission and root path unchanged. No perf claim.
- N17/T16 static/HLOD refs now have one scene-owned DEFAULT table + CPU lease;
  existing copy pass uploads once per actual density/proxy/layout revision. HLOD,
  all raster/visibility/probe readers share its graph handle; delayed diagnostics
  use the frame's CPU owner. refs_dirty survives failed preparation; retired tables
  and staging wait on their actual completions. Static material facts still need
  the integrated T16 comparison.
- W11 source restores exact retained layout before declarations on warm path,
  with value-only layouts/current bindings across helpers. Cache publication and
  eviction happen after successful Render return so borrowed spans survive Execute.
  Allocation/DXGI/ReservedTiles certificates, alias and external checks remain.
  Actual hit rate/perf unproven. Treat expanded declaration reuse as C09 candidate
  with historical CPU bottleneck entry evidence, not universal completion.
- W12 source cadence/span/calibration/backlog candidates are unqualified; reference
  controller/scheduling remains. Deferred per-queue patch is review artifact only.
  T20 Off/Critical/Full source: DRS-only Critical records Direct37/38 two query pairs
  (four indices), no per-pass names or cross-queue calibration; Full explicit capture
  retains pass diagnostics. Stored frame mode/layout/mapping governs readback.
  Only Full supplies all-pass CPU output storage; Critical retains frame-level CPU
  aggregate, avoiding two endpoints being passed as an all-pass output array.
- T04 current pack already streams raw renderer bulks. New follow-through streams
  PCM and pinned font too, sharing one1MiB scratch; CopyFileBytesTo and product copy
  verify actual size/digest, close/final extent before atomic publication. Metadata
  prepare buffers remain owned as needed. No format/order/identity change intended.
- T10 one immutable Presentation UiFrame owns models and displayed interaction
  facts. Exchange retains it; Runtime passes only draw spans/publication identity
  to Renderer. Removed three-slot facts/mutex join and Present-time model/facts
  copy. Concrete dependencies are checked before model generation; unchanged UI
  reuses the owner. Receipt repeats retain owner; resize changes receipt extent.
  UI preparation/publication failure abandons Writing slot, retains accepted UI;
  actual samples/audio keep progressing in realtime. Final/lockstep failure remains.
- T21 source separates last-valid LOCAL/NON_LOCAL/host observations, freshness,
  sampled D3D12MA BlockBytes/adjusted usage and raw DXGI usage. Successful segment
  refresh alone absorbs its measured growth. UMA maps upload/readback to LOCAL.
  Diagnostics use incremental counters/cached observations, no all-view/skin walk.
  Essential unknown-capacity bootstrap follow-through remains; current source built.
- UI layout/model candidates publish only after raster/WIC/staging success. Whole
  raster/upload is explicit PR13/GR25 fallback; partial UI copies are not qualified.

## Historical evidence only and final risks

Artifacts: Build/csm-ibl-20261025/, Build/debug-ultra-60fps-20261025/,
Build/combined-directives-review-20261025/. Old logs/snapshots prove their recorded
source only, never this entire changed batch. Historical low-level18/18, portable3/3,
native64 validation0 and actual Release install/Main180 successes remain checkpoint
receipts. Runtime-only Release build path: Build/msvc-prebuilt-release.

60FPS is unproven. Historical ordinary pair~28.12->29.47FPS is NOT current-source
performance; component GPU durations are not whole FPS. Retain old failures:
DeviceHung0x887A0006 after383frames; full GBV90/120/193.6timeouts incomplete;
RenderDoc DirectWrite E_NOINTERFACE/noRDC; uncontrolled pixel pair not quality proof.
No native issue is fixed merely by source review or general validation0.

Do not retry auto-review-rejected cleanup of Build/csm-ibl-20261025/install-fixture.
Preserve review artifacts proposed-per-queue-timestamp.patch/.md; do not apply them.
Final batch must qualify native resource lifetime, partial/unknown submission,
temporal/optical/LOD, source actor/VFX behavior and controlled200enemy frame tails.
No manual/user acceptance checklist belongs here.
