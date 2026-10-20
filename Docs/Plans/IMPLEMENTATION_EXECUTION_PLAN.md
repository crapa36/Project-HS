# Project HS execution plan

> 2026-10-16 현재 범위: 레이트레이싱은 제거되었다. 아래 날짜별 구현·검증 기록은 당시 상태를 보존한 이력이며 재구현 요구가 아니다. 현재 계약은 ARCHITECTURE.md, 현재 코드와 Docs/Verification/20261016-rt-removal/REPORT.md를 따른다.

This plan turns `ENGINE_RENDERER_V7.md` and `CONTENT_GENERATION_DIRECTIVE.md` into
connected implementation batches. The two source directives are requirements,
not evidence that the engine already has these capabilities. Current code and
verification receipts determine progress.

## Objective and critical path

1. Make content actions deterministic and independently scheduled. Publish
   immutable ProductSets and complete, scoped Generations; resolve every runtime
   asset from one selected Generation. Preserve the existing simulation and
   presentation binary ABI and legacy hash values through cheap finalizers.
2. Add RenderData and MaterialIR authored/cooked contracts, then migrate the
   existing assets and live renderer consumers to versioned packages.
3. Replace the renderer frame path with persistent GPU scene, real visibility
   frontends, a validated frame graph and lifetime model, lighting/RT/GI,
   transparency/VFX/volumetrics, temporal reconstruction and display paths.
4. Connect each exposed graphics setting and requested SDK provider to its real
   device, resource, execution and teardown path. Package the selected assets,
   shaders and native libraries. Diagnose unsupported prerequisites truthfully.

The dependency order is **Generation contract -> runtime resolver -> versioned
RenderData packages -> renderer asset migration**. A renderer improvement that
uses the existing asset layout can run in parallel with this chain.

## Parallel work packages

| Package | Ownership boundary | Deliverable and acceptance |
| --- | --- | --- |
| A. Toolchain and current graph | Main agent: CMake, presets, `tools/`, CI | Pinned CMake 4.4.3, correct producer/test edges, fixed-output copies, exact test preparation. Build and affected checks run once after a connected implementation batch. |
| B. Asset protocol and Generation | AssetGeneration agent: composer/GC; main agent: CMake/repair/resolver | Canonical ActionKey/ProductSetId/ActionResult; multi-action scope completeness; atomic manifest/current pointer; integrity and repair through CMake; bounded local GC. Audio is the first live producer and consumer proof. |
| C. Content producers and legacy ABI | Separate owners for GameData/finalizers, environment, character/monster, VFX; main agent owns shared CMake wiring | Each expensive action reads only its semantic inputs and emits an ActionResult. Legacy all-source identity is separate and bit-exact. Unreferenced WAV, one character, environment and VFX numeric edits do not rerun unrelated expensive producers. |
| D. RenderData and MaterialIR | RenderData/cooker owner; renderer owner consumes only finalized public schema | Versioned mesh/material/texture/scene packages and generated shader variants; existing terrain, character, slime and VFX inputs migrated before removing legacy readers. Low-level build remains independent of runtime/content SDKs. |
| E. Renderer execution core | RendererD3D12 owner; main agent integrates Runtime diagnostics | Start with connected optional post path. Then dynamic frame graph, completion/lifetime, persistent GPU scene, streaming, visibility and deferred/forward frontends. Each enabled feature performs actual GPU work, and disabled features do not. |
| F. Lighting, temporal, display and settings | Distinct renderer feature owners after E contracts; Runtime/settings owner for save, UI and transactions | Shared lighting/RT scene, shadow/GI/reflection, transparency/VFX/volumetrics, reconstruction and HDR/UI/FG; one settings definition with requested/effective state and transactional apply. |
| G. SDK, packaging and developer evidence | SDK integration owners by provider; main agent owns CMake/package acceptance | Available provider means initialized, fed valid inputs, executed and released. CompiledOut/Unsupported/Unavailable remain distinct. Manifest-driven package and structured timing/capture/query tools work without CAS or Asset Index. |

## Integration rules

- Agents own distinct files; the main agent decides shared contracts and edits
  CMake/Runtime call sites where package boundaries meet. No concurrent edits
  to the same renderer or content source file.
- New packages stay source-root independent in IDs. Stable outputs belong to
  CMake/Ninja; immutable products belong to ProductSet namespaces. A failed
  publication cannot update the current Generation pointer.
- Preserve the 60 Hz simulation, cooked ABI and legacy hash compatibility.
  Renderer quality, frame rate and generated frames cannot alter gameplay.
- Finish one connected slice before widening it. An interface, stub or a
  permanent fallback is not completion of a requested renderer feature.

## Next work orders and handoff boundaries

The 65-test gate recorded below is historical; the current joined checkpoint is the 2026-10-08 section at the end of this plan. Work added
since that checkpoint needs its own affected checks; the checkpoint does not
validate later source changes. The full Renderer-v7 directive remains open.

| Order | Parallel ownership | Concrete handoff and acceptance |
| --- | --- | --- |
| 0. Authored material resources (closed) | Compiler owner: RenderData IR/schema, material parser/shader/package producers and package fixture. Renderer owner: environment loaders, binding descriptors and material HLSL. Main: 14 authored definitions, CMake edges and integration. | Typed texture/sampler references, explicit UV gradients and finite array-layer proof; all 14 materials retain existing math. Shared scene/shadow opacity, compatible alternate package binding, frame-slot descriptor publication and CompletionSet retirement. One package/render/streaming batch, matching environment capture and authored resource probe. Close atlas-entry/page confusion, all shader include hashes, and v2 executable conversion before acceptance. |
| 1. Shared geometry packages and execution | Package owner: new geometry codec, render package validation and both environment/character producers. Asset/deformation owner: unique vertex/index/page upload and SkinCache mapping. Geometry/visibility owner: GPU records, indexed/mesh commands and shared shaders. Main: central device/root/pipeline/frame integration, CMake and ABI decisions. | Qualify a mature offline meshlet builder on actual environment/character packages first. Preserve complete vertex records, material/skin seams, duplicate primitive occurrences, bounds and canonical primitive remap. Indexed and mesh paths consume the same geometry and current/previous deformation. Add IndexBuffer graph access, capability-query/mesh PSO/dispatch, complete page publication and retirement. Preserve existing product IDs and authored animation semantics. |
| 2. GPU refinement and opaque frontends | Visibility owner: persistent candidates, output-space geometric-error LOD/cluster refinement, camera/cascade compaction and current-depth retest. Surface owner: VisibilityBuffer resolve, common SurfaceLayout and guide production. Main: surface consumers and settings/capabilities. | Cook a conservative measured/authored geometric error; vertex counts and current distance thresholds are not screen error. Residency selects a complete cluster cut. Both Indexed/Mesh and VisibilityBuffer/CompactDeferred combinations retain material/opacity/deformation/primitive meanings. Reconstruct perspective-correct attributes and analytic gradients at raster samples; remove world-position RT and overloaded normal/position alpha. Reuse existing GPU fixtures and one combined moving capture. |
| 3. Lighting, effects and temporal | Distinct owners for shared light/probe/shadow/RT scene, AO/GI/reflection, transparency/volumetrics and view history/reconstruction. Main freezes signal handles, feature composition and resource lifetimes before parallel writers. | Implement the full requested algorithms and real subgraphs, including shared AS/deformation, closure evaluation, guide conversion, exposure and dynamic resolution. Each enabled feature executes, and disabled features omit work. Use narrow feature evidence, then one integrated gate after the connected block. |
| 4. Display, settings, SDK and package | Display/provider owners get disjoint adapters; Runtime owner handles UI, requested/resolved transactions and persistence. Main owns SDK bootstrap, native library/package graph and acceptance. | Actual HDR/display/UI/FG/SR/denoiser paths with exact SDK input/output and completion contracts; factual CompiledOut/Unsupported/Unavailable status. Package runs from selected manifest without CAS/index. Remaining 56-setting effects must be real, not just IDs. Complete all directive requirements before categorizing and committing the dirty tree. |

Completed foundations include schema-2 settings persistence and render apply
acknowledgement, immutable static scene leases, transactional published scene
deltas, persistent scene slots/tables, shared GPU deformation, production
Direct/Compute/Copy execution, and live texture/geometry streaming slices.
These slices do not close the larger streaming, material, frontend, lighting,
provider or display requirements.

Main-owned central renderer files prevent concurrent edits to shared root,
pipeline and frame execution. Producers and renderer consumers change in one
coherent integration batch after their shared portable/GPU ABI is frozen.

## Verification budget

- The latest user instruction resumes execution at integrated checkpoints. Workers
  do not build, cook or test each edit. Main runs one affected bundle after the
  parallel packages are connected, then assigns repairs from actual diagnostics.
  The latest source-only batches still have no completed execution receipt.
- Implement related changes before executing checks. Workers inspect their owned
  source and report handoffs; the main agent owns the combined build and verification.
  A compiler failure or concrete unresolved risk can justify an affected rerun.
- Use one build tree per batch. Avoid rebuilding the same renderer in Debug and
  RelWithDebInfo unless the changed behavior specifically requires Debug, such as
  shader reload. Retain valid receipts for unchanged contracts.
- Run one affected CPU bundle and the minimum GPU processes that cover new execution
  and lifetime risks after integration. Reuse a process/fixture for related cases;
  no full startup, matrix, capture or whole-tree gate for each small edit.
- After the content and runtime resolver merge, run one clean targeted build,
  legacy hash/ABI oracle and one full `verify` gate.
- After a renderer execution batch, collect the affected graph/feature and actual
  GPU evidence together. Compare output and validation errors; process exit alone
  is insufficient. Broader matrices require a concrete uncovered path.
- At final integration, run deterministic simulation, full runtime/asset gate,
  representative moving captures and timing with validation-on correctness
  separate from validation-off performance. External SDK/hardware limitations
  are recorded as unverified, not passed.

## Historical execution state (2026-10-04; superseded by the 2026-10-08 joined checkpoint)

`DEV_STATE.md` records source-specific execution contracts and evidence. This
continuation connected typed SurfaceClosure/BSDF, selected generic shaders and
transactional shader reload, and retired duplicate content production. Narrow
build/test/GPU receipts below cover these inputs; the broad whole-tree gate remains open.

| Work package | Current state | Remaining boundary |
| --- | --- | --- |
| A. Toolchain/current graph | Substantially implemented; pinned CMake, exact preparation and scheduling | Final integrated validation and complete performance evidence |
| B. Generation/resolver | Core connected: immutable publication, scoped atomic selection, strict resolver, repair/basic GC | Complete resident-content reload, remaining I/O reuse and final qualification |
| C. Producers/legacy ABI | Split live producers; earlier incrementality matrix and compatibility checks | Complete bit-exact compatibility and before/after evidence at final source state |
| D. RenderData/MaterialIR | Versioned packages, 15 authored graphs, terrain Gaussian/POM, typed numeric reflection/GPU table and separate defaults | Advanced closure layers/sidecars and remaining asset migration |
| E. Renderer execution | Real queues/graph/SkinCache, whole-object LOD, both Surface paths and actual transient aliasing | Cluster/HLOD/page demand, unified residency, subgraphs/external queue contracts |
| F. Lighting/effects/temporal/display/settings | Connected existing cascades, lighting, OIT/VFX, reprojection and settings persistence | Most advanced v7 lighting/RT/GI, hero/volumetric, TAAU/DRS/HDR/display and 56 actual option effects |
| G. SDK/package/evidence | Selected-Generation package and structured tools connected | Real provider adapters, SDK/native distribution, full timing evidence and final acceptance |

- Selected schema-3 `runtime-full` Generation is
  `c1e4bf3566b9b53a2622838f822b6179ff4a534393532e21ce92f368dfba59cf`,
  143 Actions / 786 Products.
- Content phases 0–4 are substantially implemented; I/O/queryability phases 5–6
  and complete performance evidence remain partial. The retained incrementality
  matrix is `Build/tooling/incrementality-matrix-20261002T173523/summary.json`;
  it belongs to its recorded source state, not the latest renderer changes.
- Geometry is shared across Indexed/Mesh, including complete native four-buffer
  pages, canonical primitive meanings, current/previous SkinCache and procedural
  DEFAULT page 50. GPU LOD is whole-object selection; full cluster/HLOD remains open.
- VisibilityBuffer is implemented, not a next-step placeholder: shared 32-byte
  raster records after retest, 16-variant classification, indirect SurfaceResolve,
  analytic gradients, common six-target Surface ABI and transactional resize.
- Four cases under `Build/tooling/visibility-gpu/` (`indexed-local-footprints`,
  `mesh`, `compact-indexed`, `compact-mesh`) each pass 10 frames with zero GPU errors.
  `visibility-motion-resize` passes 7 frames including 720x405 resize. The comparison
  report confirms exact Indexed/Mesh parity separately in both Surface paths;
  Visibility/Compact maximum final-image channel difference is 4/255.
- Typed SeededNoise2D/FresnelSchlick package tests and 61 generated HLSL fixtures
  pass. These operations do not complete the full material closure requirements.
- Selected-Surface PSO omission has completed build and both active-profile startup
  receipts; current terrain/alias GPU cases also execute both Surface paths.
- Authored terrain v4 uses one typed aggregate, six texture slots (ABI 2), a linear
  RGBA32F histogram package and explicit 40-byte material metadata. Current
  `terrain-alias-gpu/indexed-visibility-authored` (10 Enhanced frames) and
  `mesh-compact-resize-authored` (7 Legacy frames) both pass with zero GPU errors.
  The terrain comparison is pixel-exact against the previous capture.
  The remaining Mesh/Visibility and Indexed/Compact combinations each pass a
  two-frame startup check. `terrain-alias-gpu/acceptance.json` captures the
  source fingerprints and current narrow checks.
- Transient aliasing uses schedule reachability and actual placed views/backing
  blocks with fence-protected reuse. CPU planner and hardware Legacy/Enhanced
  upload/readback checks pass. Distortion/BloomHalf aliasing reduces the measured
  transient backing from 12,320,768 to 10,354,688 bytes (15.96%); these are
  validation-on correctness/allocation receipts, not performance qualification.
- Initial terrain GPU cases without `-authored` retained a wrong compile define
  and flat terrain; only the corrected cases are accepted image evidence.
- Earlier full 65/65 and core 10/10 receipts on 2026-10-03 predate the newer paths.
  They remain baseline evidence and are not current whole-tree acceptance.
- Next parallel implementation boundaries: cluster/residency; material layers/sidecars;
  graph/subgraph/external-queue contracts. Integrate those
  before lighting/RT/GI and temporal/display/provider completion.
- Typed SurfaceClosure and matched metallic/roughness GGX Evaluate/Sample/Pdf are
  connected to direct lighting and a generated DFG LUT. Seven actual GPU energy,
  PDF-mass and reciprocity probes pass; advanced layers/HDR/probes remain open.
- All 54 generic runtime shader outputs now belong to immutable Actions; exact
  include/tool/source identities are tracked. Runtime uses selected products;
  legacy projection copies 55 matching shader bytes without duplicate DXC work.
- Debug shader-only Generation adoption passes invalid-candidate rejection,
  valid PSO/LUT replacement, history invalidation, graceful exit0 and validation0.
  Nonshader changes are accepted only for known same-layout parameter products;
  complete resident resource preparation remains open.
  The current two-frame Indexed/Visibility GPU validation also passes errors0.
- Normal content builds no longer invoke the monolithic Cooked writer. The explicit
  excluded legacy oracle preserves compatibility code; split Actions remain authority.
  Arbitrary Action queries work before composition, and GC dry-run reports retained
  and removable snapshots/ProductSets/cleanup metadata without applying deletion.
- Numeric parameters now have six typed 16-byte slot kinds, separate defaults Actions,
  required layout chunks and shared Surface/opacity GPU reads. Terrain/rock_0 default
  edits preserve shader/package ActionKeys, ProductSets and every geometry LOD/page.
  The current two focused CPU checks pass. Actual Debug Indexed/Visibility/Enhanced
  and Mesh/Compact/Legacy reloads each pass six frames/errors0 with incompatible
  layout rejection, PSO revision unchanged, apply/resize/restore and visible output.
  Fresh runtime shader adoption also passes rejection/adoption/exit0/errors0.
- Grid-floor diagnostic reproduced the central/lower-screen warp even without AA.
  Distortion/decal table bindings used stale offsets after environment table growth;
  computed bindings repair the cause. Both Surface paths pass25 corrected GPU frames
  with straight grid lines/errors0; receipt `Build/tooling/grid-smear/acceptance.json`.
- Earlier content-authority/query CPU checks pass 3/3 in
  `Build/tooling/content-authority-query-focused-checks.log`; the consume-only Debug
  and targeted RelWithDebInfo builds pass. Unchanged earlier receipts are retained.
- Source-only continuation now connects uniform Coat/Anisotropy/Sheen/ThinTransmission/
  Emission/Unlit from v7 graphs through reflected package headers to deferred lighting.
  Seven foliage graphs author thin transmission; their analytic-sky cosine lobe is
  integrated in closed form. Coat/aniso/sheen use a matched sampled sky path; sheen
  substrate attenuation currently uses a conservative E=1 bound.
  Scattering stacks use the explicit supported base-outward order
  Anisotropy->ThinTransmission->Sheen->Coat; repeated kinds/order mismatch reject.
  Required MaterialSurfaceLayout v1 records preserve capabilities/varying requirements;
  the newer source batch connects varying fields through material sidecars.
- Source-only infrastructure batches also connect all-family whole-LOD demand/upload,
  production post feature/signal composition and native external-queue ownership.
  Windows one-shot SHA-256 uses BCrypt; portable streaming remains available. These
  changes have not passed a completed build. The selected Generation is unchanged.
- `hs_dev build -Target '<native target>,<Action ID>'` now resolves explicit targets
  before Generation composition and invokes the pinned CMake wrapper once. `-DryRun`
  reports the exact argument array. CLI execution has not been performed for this change.
- Current source-only lighting batch connects varying sidecars, hero closures, diffuse
  SSS, hierarchical SSR, probe cube/SH9/GGX, real RGBE panorama import, actual Rectangle/
  Disk area lighting and immutable LTC products. HDR-log grading, physical DoF and motion
  blur use production HDR graph hooks. No new build/cook/GPU receipt covers these changes.
- Shared DXR candidates, exact draw/deformation/opacity lookup and the RTAO trace/temporal/
  resolve service are connected through native lifecycle and graph completion. A bounded
  source review corrected pre-Prepare import resolution, coverage-sidedness and material
  texture read states. Requested/active lighting modes and fallback reasons now distinguish
  missing consumers/products from actual hardware support; unused ray scenes stay inactive.
- Native PSO cache source and central lifecycle are connected; execution is pending.
- Parallel current boundaries: directional cached virtual/local shadows;
  secondary-hit radiance and RTGI/HybridRT reflection composition. HLOD, scene-local probes,
  volumetrics, other SDK providers/FG/latency, complete reload/packaging and VFX/character
  production coverage remain. Whole-LOD streaming is not HLOD, and the external-queue
  contract does not itself implement an SDK provider.
- Whole-plan completion remains pending. After the requested finish, use one
  whole-592-path backup commit on `codex/engine-v7-backup-20261008`, push it, and
  restore `main` to its original HEAD, index and working-tree bytes. A raw file
  count or equally weighted feature count does not provide a defensible completion percentage.

## Remaining implementation batches (2026-10-07)

These are implementation orders, not completed-feature claims. Main owns shared
renderer files, public contracts, producer recipes, CMake and final integration.
Workers preserve the dirty tree and return owned files, contracts, open issues
and concrete diagnostic evidence. No worker runs a separate validation matrix.

| Batch | Parallel ownership and detailed order | Combined checkpoint |
| --- | --- | --- |
| 1. Independent character deformation | Character owner samples actual per-instance named/legacy/retained poses, deduplicates exact palette keys, uses waited frame uploads and existing root17, preserves previous geometry and correct inverse-transpose normals. This source package is frozen. Main inspects the independent review and preserves it during view integration. | Join native C++/HLSL build; rerun the previously failed skin-cache fixture with current products. |
| 2. Explicit views and frame contract | View owner adds Core value types and Renderer frame inputs, lazy view-owned GPU targets/TAA/exposure/HZB/DRS/services/native contexts, successful-frame origins and real offscreen output. GPU Scene/assets/deformation/TLAS stay shared; main alone presents/uses FG. Main migrates Runtime/fixtures and reviews settings/reload/retirement integration. | Validate a sequential main/preview/capture fixture, same-view capacity reuse, separate history and rejection after the combined build. |
| 3. MaterialIR Decal Surface | Decal owner adds validated typed graphs and independent shader/program/default products, receiver metadata and explicit RasterOnly policy. New graph service modifies base/normal/roughness/emission before shared Surface consumers. Main appends space36 table32 descriptors, integrates lifecycle and redirects shared guides/lighting/sidecars without duplicate local-lighting application. | Join producer/native build and inspect modified Surface plus common guide/lighting execution in one fixture. Empty work owns no new pass/resources. |
| 4. Bounded diagnostics | Diagnostics owner implements fence-completed calibrated timelines, fixed storage, validation-separated/per-view percentiles, actual cadence and explicit unavailable measurements. Source package is frozen. View/main owners connect completed/rendered/transition hooks; main audits immutable export and allocation attribution. | Collect current structured evidence with the same representative fixtures; do not add a separate performance matrix. |
| 5. Incremental build dependency refinement | Main extracted existing layout and publication locking into narrow internal targets, retaining compiler/source authentication and public module ownership. SDK pin projections retain full per-SDK authentication. No manual recipe exclusion or aggregate-key bypass. | One legitimate recipe transition in the joined configure; subsequent unrelated source changes must not rebuild those narrowed closures. |
| 6. Original contract and authored-content closure | Main audits canonical public/settings/frame identity contracts and all56 actual option effects/Off lifetimes against source. Complete character consumers only from authentic runtime inputs; absent Elin clips/morph/LOD/secondary authoring remains an external input dependency. Preserve separate unfinished modeling. | Reuse current receipts; obtain asset/reload/package and content before-after evidence only for uncovered contracts. |
| 7. Completion | Resolve concrete joined build/cook/GPU failures by owner as a frozen repair batch. Main updates source-specific continuity and performs the required final gate when the entire requested implementation is ready. | After completion, make one whole-592-path backup commit on `codex/engine-v7-backup-20261008`, push it, and restore `main` to its original state; no source percentage or static audit substitutes for that condition. |

### Batch 1 frozen handoff

- Main root table32 retains space20 (21 descriptors), appends space21 (5 shadow
  SRVs) and space22 (36 transport descriptors), with no extra root DWORDs.
- Shadow raster commands select page draw records using PassValue bit31; regular
  geometry/cascade encoding remains unchanged. Page writes must clip to the tile.
- RTGI computes the delta from the exact deferred probe diffuse. SSS consumes
  adjusted diffuse; final HDR applies diffuse delta and replaces environment
  specular exactly once. Live AO is not multiplied into traced visibility.
- Stage descriptor banks are separate for GI, reflection and composition. Scene,
  material, light and probe revisions invalidate dependent history.
- Producer products are selected Generation logical IDs ending in `.dxil`;
  newly authored shader source is not evidence that the current Generation has it.
- VFX events require actual successful gameplay stages and real geometry/episode
  identity; persistent owners and event dispatch counts are reported separately.
- Each worker hands back one connected package. Main consumes that package and
  supervises failures by owner instead of serially rebuilding after small edits.

### Batch 2 implementation handoff

- HLOD asset/layout owner extracts the existing environment placement into a
  public pure Presentation builder. Preserve every placement and gameplay input;
  derive render IDs from provenance rather than preceding grass counts. Static
  obstacle and border members are eligible; ground and deformed grass retain
  their existing leaf paths. A canonical eligible-layout digest includes the
  layout ABI, mesh/variant, transform and tint, independently of session/grass seed.
- The separate SHL1 scene package represents object membership, not GHI1 triangle
  refinement. Child intervals partition complete member stable IDs. Every parent
  proxy range together replaces its entire descendant member set. World-baked
  geometry uses existing page/vertex ABI, measured error, conservative bounds and
  material/semantic/atlas-selector/tint partitions. Root owns scene publication,
  Generation producer wiring and GPU integration after this asset handoff.
- GPU HLOD selects generation-qualified leaf view masks before both cluster and
  whole-object visibility, and emits proxy draw ranges using existing draw records.
  Suppress members only after every required proxy page is fence-published. Use
  the shared 128-slot page pool, request/retirement and all-queue completion;
  never create a second page allocator. Representation switches invalidate local
  history. Camera, directional/local shadow and ray demand need explicit consumers.
- Volumetric owner implements service-owned imported 3D froxels, density/phase,
  actual light/shadow injection, temporal/spatial resolve and cumulative L/T.
  Passes170–175 are density, lighting, temporal, spatial, integration, opaque
  resolve. Append24 descriptors in space24: b0, t0..13, u0..8; space23 remains
  reserved for local probes. Root owns lifecycle, selected shader products,
  settings and OIT/hero integration. Off owns no service resources/PSOs/passes.
- Froxel quality stride/Z is16/32,12/48,8/64,8/96. Active extent changes within
  capacity preserve allocations. History is qualified by successful rendered
  frames, scene/light/volume revisions, camera/origins and depth. Rectangle/Disk
  lights sample their actual domains; all five directional ShadowData resources
  participate in graph reads. Fog frustum/volume bounds join receiver demand.
- Fog transports raw scene-linear radiance before pre-exposure. Each regular
  transparent fragment samples its own depth. Hero far-to-near composition uses
  interval transport Tfar/Tnear and (Lfar-Lnear)/Tnear, including the opaque limit;
  a final whole-frame fog pass cannot substitute for these layer intervals.
- Local probe owner preserves the strict existing environment v1 package and
  adds a versioned scene package with position/influence/projection bounds,
  radiance/SH/GGX/visibility and bake revisions. Use actual static offscreen cube
  views with common material/opacity and first-hit distance, excluding dynamic
  characters/UI/post/recursive local feedback. Publish matching completed scene
  generations only; unavailable results use the existing environment baseline.
- Root adds world-position arguments to indirect-light evaluation and retains
  global sky radiance for background. Local box projection and normalized
  visibility-weighted blending apply to basic SH/GGX and sampled advanced BSDF.
- The default environment now consumes the authored Forest Slope RGBE source;
  the producer tracks the actual HDR bytes and builds its SH/GGX products.
- Intel 3.0.2, Streamline 2.14.1 and AMD 2.3.0 native inputs are pinned and
  bootstrapped. Native adapter source and shared SR integration exist; the current
  native repair source build passes13/13 jobs, while GPU execution, presentation
  qualification and NRD evidence remain pending.

### Current joined checkpoint and remaining packages (2026-10-08)

Current source-specific receipts live in [completion-evidence-current.md](<C:/Users/crapa/Documents/GitHub/Project HS/Build/tooling/completion-evidence-current.md>). The current test-render Generation is `17033091346d606984da01ef9a918dab8045a72933e1cb105326bc11f3c06e0c`; runtime-full is `02534e5493be816e0d9034e73662bc8ac19ae24cd10d641c691697414bcf73ed`. VSM admission build and latest strict verification are the current source/runtime evidence.

| Evidence | Receipt | Current result |
| --- | --- | --- |
| Skin ABI | [post-repair.json](<C:/Users/crapa/Documents/GitHub/Project HS/Build/tooling/skin-palette-abi-current/post-repair.json>) | Generation bdec2327ddd2; both selected matrices row-major, stride64. |
| Grid | [result.json](<C:/Users/crapa/Documents/GitHub/Project HS/Build/Artifacts/checkpoint-D-20261008T065418Z/grid/result.json>) | Generation ff0c97727d19; 25/25 frames, validation errors0, captures1/9/17/25. |
| Owned views | [result.json](<C:/Users/crapa/Documents/GitHub/Project HS/Build/Artifacts/checkpoint-D-20261008T071800Z/owned/result.json>) and [receipt.json](<C:/Users/crapa/Documents/GitHub/Project HS/Build/Artifacts/checkpoint-D-20261008T071800Z/receipt.json>) | Generation 7761d5eae70b; 9 completed submissions, validation errors0, named clips/decals/HDR metadata. |
| Core gate | [verify-core-20261008T105844062Z-52824.json](<C:/Users/crapa/Documents/GitHub/Project HS/Build/verification/verify-core-20261008T105844062Z-52824.json>) | PASS10/10, Debug, RuntimeOFF/ContentToolsOFF; navigation assertions enabled. |
| Ordinary Debug build | [ordinary-debug-build.receipt.json](<C:/Users/crapa/Documents/GitHub/Project HS/Build/tooling/ordinary-debug-build.receipt.json>) | Incremental Debug validation OFF / GPU validation OFF; `ProjectHS` exit0 in7.0041335s; exported runtime-full generation `02534e5493be816e0d9034e73662bc8ac19ae24cd10d641c691697414bcf73ed`; executable SHA256 `B7AA8906D78A4D1D3156E8A6049107C492A2AAFF1414598EC6828DC33C036D08`. First fresh build retained at [ordinary-debug-first-build.receipt.json](<C:/Users/crapa/Documents/GitHub/Project HS/Build/tooling/ordinary-debug-first-build.receipt.json>). |
| Window lifecycle and display-output proof | [window-startup-after.json](<C:/Users/crapa/Documents/GitHub/Project HS/Build/tooling/window-startup-after.json>) | `Source/Runtime/Private/window.cpp` preserves existing non-frame bits when `SetBorderless`/`ApplyDisplayPlacement` replace `WS_POPUP|WS_OVERLAPPEDWINDOW`. Init-ready PID58420 `ProjectHS.Window` had renderer-ready artifacts before normal `WM_CLOSE`; the earlier PID58440 visible-window observation later hit the display-output error and is not stable proof. `Source/Runtime/Private/application.cpp` now resolves Auto output through HWND-aware Win32 enumeration before vendor initialization, maps the requested ID or current monitor rectangle to a concrete ID, preserves canonical Auto, and uses the actual resolved ID for rollback tracking. Latest no-args relaunch PID59344 at21:56:05 is pending root HWND/log validation. |
| Providers | [result.json](<C:/Users/crapa/Documents/GitHub/Project HS/Build/Artifacts/checkpoint-D-20261008T081652Z/providers/result.json>) | PASS38, validation errors0, normal completion; pipeline cache192 hits, 0 misses and 0 load/store/IO failures. |
| Software GI / Brixelizer | [result.json](<C:/Users/crapa/Documents/GitHub/Project HS/Build/Artifacts/checkpoint-D-20261008T090137Z/software/result.json>) | PASS7, validation errors0, normal completion. |
| Lighting | [result.json](<C:/Users/crapa/Documents/GitHub/Project HS/Build/Artifacts/checkpoint-D-20261008T091106Z/lighting/result.json>) | PASS7, `execution_valid=true`, validation errors0, normal shutdown; active native RT lighting with no fallback. |
| FSR RR | [result.json](<C:/Users/crapa/Documents/GitHub/Project HS/Build/Artifacts/checkpoint-D-20261008T091106Z/fsrrr/result.json>) | PASS3, `execution_valid=true`, validation errors0, normal shutdown; signed FSR SDK reports device unsupported, so active reconstruction is internal rather than native FSR RR. |
| DLSS RR | [result.json](<C:/Users/crapa/Documents/GitHub/Project HS/Build/Artifacts/checkpoint-D-20261008T091106Z/dlssrr/result.json>) | PASS3, `execution_valid=true`, validation errors0, normal shutdown; active `dlss_rr` is native/available. |

Retained verification also includes composed source-scoped D/E/G receipts (12 checks), portable2/2, GameData9 and CLI8/8. The full59/71 gate is historical; the eight incrementality classes remain covered by their own receipts. These receipts establish action reuse/selectivity, not an old-engine before/after speedup baseline.

| Package | Receipt / pointer | State |
| --- | --- | --- |
| AS CreateResource3 / UNDEFINED and active RR pass assertion | — | Source-complete and closed by the 091106 GPU batch. |
| Geometry/composition boundaries | — | Geometry-domain filtering and shared lookup, physical-version signal identity and indexed composition are source-complete and reviewed; retained GPU batch passes. |
| Native providers | — | Current lighting and DLSS RR execute with zero validation errors; FSR RR remains device-unsupported and uses internal reconstruction. |
| Historical full gate | [summary](<C:/Users/crapa/Documents/GitHub/Project HS/Build/tooling/final-gate-current/20261008T091423227Z-42788.summary.json>) | Retained source-specific evidence only; not current whole-plan acceptance. |
| VSM admission | [vsm-admission-build.stdout.json](<C:/Users/crapa/Documents/GitHub/Project HS/Build/tooling/vsm-admission-build.stdout.json>) | Native/content build PASS. Actual overflow cap256, maximum765, 249 arrival-order rejections; deterministic Count → Admit Complete Pages → Scatter preserves capacity and PSO modes; independent review accepted. |
| Latest strict verification | [verify-20261008T122351645Z-56864.json](<C:/Users/crapa/Documents/GitHub/Project HS/Build/verification/verify-20261008T122351645Z-56864.json>) | Legacy/Enhanced final-image pair changed pixels0; 23 diagnostic resources/raw buffers are byte-identical and D3D12 validation errors0. Retained CTest failures are final-options no-bloom mapping and particle10000 expected5000; CLI source fix is complete, but final-options GPU CTest was not rerun. |
| HLOD repair and equivalence | [scene-cook-live-stack.log](<C:/Users/crapa/Documents/GitHub/Project HS/Build/tooling/scene-cook-live-stack.log>) and [hlod-parallel-equivalence.json](<C:/Users/crapa/Documents/GitHub/Project HS/Build/tooling/hlod-parallel-equivalence.json>) | Source/verifier accepted serialized `MeasureGeometryError`, arena1259 independent measurements, 64 qualified groups and at most21 keys. Existing one-pool parallel-per-key fixed rows/ordered fold; no error algorithm, tolerance or query-budget change. 1245 products have 0 changed digest/size; serial673716ms versus parallel248895ms, about63% lower and2.71x faster. Exact parallel receipt: [ActionReceipts/render/hlod/arena/20261008T105434591Z-56188-12d0d0eb8aab17f70891266281323fe4.json](<C:/Users/crapa/Documents/GitHub/Project HS/Build/msvc-validation/ActionReceipts/render/hlod/arena/20261008T105434591Z-56188-12d0d0eb8aab17f70891266281323fe4.json>). |
| Material and dependency receipts | [verify-20261008T113013133Z-14044.json](<C:/Users/crapa/Documents/GitHub/Project HS/Build/verification/verify-20261008T113013133Z-14044.json>) and [verify-20261008T105858699Z-8056.json](<C:/Users/crapa/Documents/GitHub/Project HS/Build/verification/verify-20261008T105858699Z-8056.json>) | Classified-variant GPU material PASS273.892s with zero errors; dependencies8.27583s PASS. Six final Surface buffers are byte-identical; no generalized raw-guide visual/material claim is made. |
| Package / performance | Current finish state | Ordinary `msvc-debug` passes; package-smoke/perf3case and fresh package/performance runs are not claimed. |

Next checkpoint: retain the completed CLI source fix, corrected incremental Debug build and display-output source repair; latest PID59344 HWND/log validation remains pending and final-options GPU CTest was not rerun. The 592-path backup dry-run passes, but the remote name is absent. Then perform one whole-592-path backup commit on branch `codex/engine-v7-backup-20261008`, push it, and restore `main` to its original HEAD, index and working-tree bytes. Use pending `Build/tooling/backup-and-restore-main.receipt.json` as outcome authority; original main ref prefix is `5ee6bf8`, and commit/push/restoration are unclaimed until that receipt exists. Package/performance runs are not claimed. HLOD equivalence is closed with 0 changed digest/size across1245 products and a measured2.71x action speedup; no engine-wide old/new speedup percentage is available. Whole-plan completion remains pending.

## Backup-branch correctness and structural follow-up (2026-10-14)

Current user direction: work on `codex/engine-v7-backup-20261009`, including
Linux setup when needed. This supersedes the historical main/staged-tree
workflow for this isolated checkout. Base: `a0d98d477099509fe7eb0db379de537d2cbf8ac9`;
initial index/worktree clean. User PC tunnel is disconnected; no local PC
worktree was inspected or changed.

Execution owner: main agent. This section tracks the attached
`Project_HS_종합조사_정밀수정계획.md` full requested follow-up;
`DEV_STATE.md` remains the technical resume record.

- B00/G01: original CPU baseline and candidate 16-test Linux integration passed;
  affected HZB/UI follow-ups passed. Windows core17 and portable31 also passed.
  Production runtime/renderer compile and explicit HZB WARP gates are added.
- C01: source correction and production shader compilation complete. The compiled
  helper readback harness now replays the independent CPU oracle cases; explicit
  WARP CI passed with Debug layer and GBV. Target-GPU scene/DRS/cluster gates remain open.
- C02A/C02B: Linux and MSVC Debug core regressions passed, including actual
  executor spill growth rejection. Corrected MSVC Debug noexcept string-proxy
  allocation paths; no timeout or assertion was relaxed.
- C03: enqueue-gated state/feedback and portable production-resolver tests passed.
  Native compilation found a missing SimulationPorts read reference, now wired;
  production runtime and renderer compilation passed. Windows runtime interaction
  remains open.
- C04: bounded diagnostics and explicit streamed capture implemented/tested;
  native disk/close integration remains open.
- S01: shared scene rules and bit-preserving visibility encoding implemented.
  Producer/submission history independence, all16 revision masks and reset/abort pass.
- S02: decoded retention/terminal clock grouping and identical owner-expiry predicate
  implemented. New session-state and existing typed-frame CPU regressions pass.
- Linux: compiler-header basename collision and linker-command identification repaired.
  Full portable32, then33 with existing VFX frame coverage, passed locally.
- C05: source-policy audit found conflicting boundaries (HLOD reflection support versus
  positive-only static capture and clamped raster normals). No blanket scale rejection.
- Final source505272f CI38577118658 passes all3 jobs: Linux core18/portable33,
  Windows core/portable35, production Runtime/Renderer compile and explicit WARP.
  S01/S02 source completion is not reload-to-Render/GPU acceptance.
  Remaining S03+/P/D work is pending; existing full-FHD DeviceHung remains unresolved.
- Current evidence: `Docs/Verification/20261014-backup-structure/REPORT.md`.
  Prior correctness evidence: `Docs/Verification/20261013-backup-correctness/REPORT.md`.
  ASan/UBSan+LSan attempt failed on inaccessible /proc; no sanitizer PASS claim.

Evidence directory (ignored build output):
`Build/verification/backup-fixes-20261013`. Linux configure uses the existing
`linux-core` preset, CMake 4.4.4, Ninja 1.13.2, GCC 13.3.0. No changes to
production dependency pins, compiler warning policy, or validation strength.
