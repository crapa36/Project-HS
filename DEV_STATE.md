# Current technical state

Main HEAD is `c705366990c4054d10dced643937b7826712abb6` with uncommitted v2/v3/v5 integration.
Current Debug runtime-full Generation is
`dd34759eb2866520e434a95b4f56f09ebfee34c363b5e9cbcd39fd7817093bec`.
Latest grouped verification, foreground timing and source/PE/content identities are in
`Build/foreground-20261106`; decal/VFX component receipts remain in `Build/decal-abi-20261106`.
Earlier slot/BSDF/graph timing evidence is in `Build/ao-integral-20261106`.
Terrain receipts remain in `Build/terrain-route-20261106`. Earlier material-demand receipts remain in
`Build/material-demand-terrain-20261106`; other component receipts remain in
`Build/t02-closure-20261106`, `Build/monster-work-20261106` and
`Build/translucency-replay-20261106`; their results qualify their recorded inputs.
Prior Debug/Release shipping identities remain in `Build/r02-recording-20261106/preview-final-identities.json`;
they are not qualification of the later source changes.
The v2/v3/v5 directives and Debug 1080p Ultra, 200-enemy, 60 FPS qualification remain incomplete.
Current ordinary-pipeline foreground ABBA runs are qualified at 56.06–56.50 FPS, below 60 FPS.
Visibility/Compact differences overlap run variation; neither is selected by these FPS results.
Computer Use initialization failed and was immediately ended. External-load queries remain suspended.
Game tests use per-run master output zero before audio initialization, without changing saved audio settings.

## Current verification

- Grouped Debug ProjectHS/experiment/runtime-full builds pass. Current ProjectHS SHA256 is
  `87b3606975efea2baa10ff457d85a980ee618681a714134ee8880257531081d4`;
  measured experiment PE SHA256 is `9ef3f313848aa26bdc0f8f96cdaec47d71d8928944383af9beafba9377a1d748`
  (FNV1a64 `b3d731c79b5ad792`). Current source manifest/identity is in
  `Build/foreground-20261106/final-source-identity.json`; the only source change after the measured PE
  is the shipping test's public gameplay setup. Normal Debug tests remain OFF.
- Full Windows verify ran110 tests:109 passed; light-list exposed an inactive CSM atlas capture.
  Capture now follows the successfully registered per-view CSM fact, invalidated with target replacement;
  this fact is development-only. The existing light fixture exercises inactive, real positive-energy
  fallback CSM and zero-energy directional cases. Scoped light-list/lighting auxiliaries/material reload/
  runtime smoke then pass4/4. Other unchanged full-workflow results are reused, not a second110/110 run.
  Existing residency GBV actually rendered53 frames (128 was the maximum), validation0.
- Existing shipping-content comparison now uses normal movement/basic attacks/natural spawns instead
  of development cheats rejected by Release. Canonical bytes/hashes and all600 tick checksums remain.
  Its current RelWithDebInfo and actual Release runs each pass1/1. Final Release ProjectHS and
  official shipping image build pass; content image is
  `87a5024efc6f02fff93bcc5f3582e917b36675021f6926eaaa554810e5cfd977`.
  Reconfiguration with Runtime ON/content tools OFF/tests OFF keeps the actual current pair's
  ProjectHS build valid and the target graph contains no producer executables. This reuses the
  existing Release cache; it is not a fresh-cache compile. Normal Release tools ON/tests OFF is restored.
  Native checks passed before a PowerShell hash-report assembly failure; root completed the receipt
  without rerunning tests. Exact PE/pack/header identities and scope are in prebuilt-receipt.json.
- Decal constants160B, SRV/UAV slots/formats and command stride now share one feature-local ABI;
  actual descriptor, graph-read and retained input order use the same rows. Root ranges, generated
  material textures, deferred emission and producer snapshots are connected. All291 actual selected
  DXIL files match prior admitted files/digests/sizes; Generation is unchanged. Existing package/graph
  checks2/2 pass. No new FPS comparison is needed to attribute this declaration-only shader change.
- Actual Explosive Arrow/all-upgrades smoke exposed missing geometry.source_id in pre-explosion
  pull. It now uses the actual projectile ID at production, preserving radius and dispatcher rejection
  of invalid geometry. Existing real Gameplay/Presentation/dispatch cases pass1/1; verify-core19/19
  passes. Same muted native workload now finishes120ticks/120frames, Enhanced barriers, normal
  validation0, actual decal peak4 and unsupported inputs0. It previously failed tick16/15frames.
  This is not GPU validation or a new FPS result. The separate initial bordered1080p launch failed
  window placement at1920x1061; existing borderless mode supplies the intended extent.
- Direct lighting, coated IBL and transmission reuse one surface's normalized view, NoV, F0 and
  coat-entry Fresnel. Per-light shadows, independent layer normals/positions, base/coat DFG and
  probe samples remain. Existing rendering.bsdf-sampling passes1/1, including72 coated and14 Fresnel
  cases. Its actual test Generation now selects only the two consumed products (530,052bytes),
  without depending on the full world cook. No new runner or test file was added.
- The experimental exact AO integral is rejected and removed, including its exclusive cases:
  numerical agreement did not justify larger DXIL and approximately0.011ms measured gain.
  Restored AO raw/resolved hashes equal the control. Prior single-horizon-call consolidation remains.
- Volumetric retained preflight compares actual shape facts without reconstructing24 fake resource
  tokens. Actual registration/binding still checks current versions/resources and completion.
  Development-only smoke synchronization uses a condition variable instead of1ms/busy-yield polling;
  stop/done/failure wake and join paths remain. A deliberately invalid tick7 action exits as expected.
- Swapchain output index, accepted global submission slot and committed history slot are distinct.
  Timing uses the accepted slot plus matching frame/view identity; cluster previous uses committed
  history. Existing grass/multiview D/144Hz/16 fixture passes Enhanced+GPU validation, errors0.
  It checks main/preview order, skipped views and timing ownership, not direct cluster previous-bank
  output. It is separate from the whole-scene GBV qualification below.
- Capture lockstep explicitly requests wait_for_streaming and drains GPU queues before rendering;
  it is not ordinary-pipeline FPS. New non-lockstep1080p Ultra100%/200 foreground runs share one
  PE/content and ticks300..2300 after10s warmup. VisA/CompactB/CompactA/VisB FPS are
  56.50/56.18/56.14/56.06; matching PresentMon display intervals agree. All selected frames are
  Playing/foreground; CompactA's33 background frames are excluded with boundary continuity.
  GPU output dependency spans average17.655/17.754/17.768/17.794ms; gap-free/physical busy are not proved.
  VisA CPU render16.482ms includeswait5.776ms; prepare2.444, plan1.525, submit1.521, build1.398,
  record0.924ms are nested wall scopes. Timing/PresentMon OFF gives56.28FPS, with CPU diagnostics
  retained; one unmatched run cannot isolate instrumentation cost. Full sorted CPU/GPU tables and
  per-run p95/p99/1% lows are in `Build/foreground-20261106/foreground-results.md`.
- Completed raw/DDS/duplicate PNG/PSO capture bulk was deleted after compact evidence,5,572,108,155
  bytes. Inventory is capture-bulk-cleanup.json; result/CSV/PNG/identity receipts remain.
- Latest completed capture raw/DDS/fixture pack/PSO cleanup removes1,749files/11,434,788,685bytes.
  `Build/foreground-20261106/capture-cleanup-result.json` records completion; PNG/CSV/JSON/logs
  and official current builds/content remain. Own-process ETW collection is stopped and its temporary
  executable/helper removed. No game/trace process remains from the foreground runs.

## Reusable scoped evidence

- `Build/terrain-route-20261106/RESULTS.md`: official49-tile production, codec/graph2/2,
  muted200/120 and normal debug-layer14. Resolve1.640->.870ms accompanies local peak3.45->6.43GiB;
  it does not close T-01 quality/residency or whole-frame acceptance.
- `Build/monster-work-20261106/RESULTS.md`: real reduced monster page decode/bind, prepared
  rules/presentation/VFX/audio composition, independent129-action/894-product probe producer,
  9-cube one-shot Enhanced/Legacy GBV and7-frame world-transport readiness. Independent
  multibounce/reference, current final shipping and runtime reduced-LOD benefit remain open.
- `Build/translucency-replay-20261106/RESULTS.md`: volumetric ABI/DXIL equality and actual
  indexing work reduction2.641M->1.296M VS invocations per phase, unchanged triangles. Intrusive
  counters are not FPS evidence; that workload still selectedLOD0. Rejected dispatch chunking
  and its exclusive cases were removed.
- Current full-world Ultra1080/200 Enhanced+GBV run completes120ticks/120frames with validation0,
  valid execution/assertions, full diagnostics and successful GPU drain; owned child exits0.
  The dedicated existing child entry uses an outer600s qualification limit without changing normal
  watchdog/CTest limits. It takes about321s including validation startup and is not FPS evidence.
  Read-only stack sampling saw Jobs workers waiting for tasks and render submission under D3D12
  validation with NVIDIA compiler activity; the eventual pass does not establish an exact lock cause.
  This qualifies the recorded120-frame workload on RTX4080/driver32.0.16.2114, not all modes/devices.
- `Build/r02-recording-20261106/RESULTS.md`: physical recording groups, native multi-queue/actual
  copy/predication/accepted-prefix cases, material reload, skin/grass and that checkpoint's
  Debug/Release builds. `Build/v5-integration-20261105/RESULTS.md` covers earlier water,
  residency/UI/asset contracts. Their unchanged component results are not a new whole-tree gate.

## Ownership and binary content

Simulation rules, presentation, audio and renderer catalogs use bounded explicit binary pack sections.
`PreparedContent` owns the selected Generation or opened pack and decoded backing until consumers, including AudioEngine, stop.
Both source paths prepare rules, presentation, VFX and the same prepared audio catalog; SFX preload and
verified deferred music reads retain their existing lifetimes. Renderer development catalog remains a
source adapter distinct from the shipping renderer table; full renderer-source parity is not complete.
PreparedContent special members are out-of-line to keep RunApplication unwind metadata bounded without /bigobj.
Admission checks domains, versions, extents, digests, semantics and dependency identity; failures preserve prior owners.
Audio cue/sample/chunk ranges, preload owners and transition streams remain distinct; PCM is verified before use.
The generated manifest is about5KB; replacing large C++ tables reduces generated data, not measured frame time.
Do not restore generated simulation, presentation, audio or catalog C++ tables or duplicate payload validation.
Canonical named shader IDs and digests remain immutable; feedback variants are selected only at effective lookup.
Unproved loose-file fallbacks retain integrity checks; admitted product proof avoids redundant whole-file hashing.
Gameplay preserves its 60 Hz phase order, deterministic RNG, strict FP and structural mutation barriers.

RenderPackage carries the explicit display sample, scene, UI owner, views and native VFX batch.
The caller retains borrowed snapshot spans until Render returns; do not replace all borrows with shared ownership.
Renderer uses existing creator-bound Jobs TaskGroups and joins accepted recording work, including after failure.
CPU ingress retirement, actual GPU producer acceptance, per-view history and successful Main Present remain separate.
Accepted prefixes, queue completions, alias/slot checks and native resource owners remain independently tracked.
Unknown or partial submissions retain their owners until completion or the existing fault/drain path.
Compatible Enhanced logical callbacks now share physical recording jobs inside existing submission boundaries.
Worker-sized ranges keep parallelism; owner/import/wait/signal boundaries and Legacy decay remain intact.
Direct/Compute logical boundaries restore fresh-list state with ClearState; Copy lists do not call it.
Logical barriers, markers, timestamps and resource membership/completions remain individually tracked.
Development preview instances/poses/cameras are built by Runtime and existing fixtures before Render.
Normal character preview preserves each sample's transforms and independently indexed pose backing;
synthetic still scenes explicitly replace their scene owner. Release excludes the preview producer and edit copies.
Pure determinism uses the existing rules-only test-combat Generation; UI text uses test-runtime instead of character/shader products.

## Terrain and residency

Terrain page production is offline in `hs_terrain_surface_bake`; ordinary runtime never executes its programs.
Shipping retains source-only producer digests without their DXIL bulk.
Five physical source sections currently produce49 material tiles (at most24m per axis), with49 layout4
metadata leaves and147 independent4096-square/13-mip BC7 planes through a layout5 root.
The producer reuses one plane's GPU output/readback and borrows CPU RGBA8 into compression;
per-allocation native peak cap remains256MiB. Generic32MiB payload and64-dependency caps remain.
TSF1 wire/chunk version is1. Admitted apron, interior and mip facts determine sampling support.
Its planes are color/roughness, base normal/AO/residual depth and four layer weights.
Residual depth is normalized over 0–0.05 m; residual mips are appearance averages, not conservative height bounds.
Metadata and each section product stay within existing package limits; no common wire limit was raised.
Runtime admits pages only for matching layout, source identity and section facts; otherwise original shading remains.
Supported pixels evaluate one page macro plus fine/residual representation; unsupported pixels use original shading once.
Ray/filter support includes the apron. Fine height and its coarse reference are evaluated at the same moved ray position.
Mesh, collision, RNG, wind and grass transforms are unchanged. Current quality/cost selection remains open;
the bake's stochastic mapping and consumer's stable micro mapping are not an exact complementary decomposition.
The previous layout3 reverse patch is historical and does not describe the current49-tile candidate.
Runtime reads/uploads one plane at a time into the current native array; profile finest mip is2/1/0/0.
All tile/mip publication fields participate in replacement; existing child-product changes already
rebuild resident owners. No separate global revision or virtual-texture manager was added.
Own DXGI local peak increased3.45->6.43GiB in the recorded comparison; independent plane streaming
or another bounded residency choice is still required before broad supported-device acceptance.
Dynamic terrain fallback tables and static page backing have separate owners and waited-slot lifetimes.
Invalid backing is not imported; old backing retires after merged actual readers. Unknown submission retains it.
A mismatched optional archive falls back without a global drain. Temporary budget refusal can retry on recovery.
Identity tracks actual bake/mip producers and admitted terrain sources, not downstream resolve shader revisions.
Generation input closure verifies actual dependency bytes; section products do not gain spurious runtime source deps.
Terrain evidence and limits: `Build/terrain-residual-20261105/RESULTS.md` and `Build/terrain-surface-density-20261105/RESULTS.md`.
Current captures are functional evidence; full motion stability and independent relief quality remain open.

Texture preferred residency uses actual static materials and dynamic materials observed within the current scene.
Existing texture quality selects its finest allowed mip; observed OS/native/host budgets govern optional detail.
Preferred owners are not pinned: budget pressure can coarsen or evict them, and recovery can request them again.
Required fallback/pinned roots, real native allocation, old/candidate overlap, staged reads and completion gates remain.
Registration, scene membership, Generation and quality changes invalidate the appropriate preferred working set.
UMA is propagated consistently to budget and host-admission consumers. Full supported-device peak qualification is open.
Geometry pinned roots and streaming remain separate from texture residency; no full-arena fit is assumed.

## Lighting, transparency and geometry

SPB2 separates 162 SH9 diffuse nodes from four reflection probes; wall-aware subcell masks restrict diffuse corners.
Baked capture quality is separate from authored scene/material compatibility; material epochs are Generation-relative.
Baked publication waits for immutable upload completion, not unrelated current readers; retirement still waits on readers.
Runtime admits, uploads, samples and retires the bake; native capture/filter/export remain in `hs_probe_baker`.
The production app now calls its independent one-shot source bootstrap. Offline state is function-owned,
not conditional Impl instance storage shared with differently compiled translation units. Runtime startup
keeps its fallback/streaming policy; capture explicitly loads the selected LOD0 source payloads.
Normal projection and the offline scene producer share the unchanged EnvironmentSun() value.
The current offline solver includes directly lit surfaces and one surface bounce only; multibounce remains open.
Volumetrics share baked SH grid sampling; dynamic lights, participating medium and emission remain live.
The 4x4x4 binary visibility-mask boundary may cause discontinuity and requires reproduction before any replacement.
Baked GI packed-world upload/sample readiness and true material/scene rejection passed at the earlier qualified checkpoint.
Evidence: `Build/terrain-surface-density-20261105/RESULTS.md` and `canonical-exits.json`.

Exposure is fixed at 1 by the selected art policy. DoF focuses on actual view eye-to-target distance.
Archer normal-map roughness uses neutral alpha with authored material values; other authored optical inputs remain.
SSR RGB is confidence-premultiplied and composed once; temporal confidence reductions scale RGB as well.
Its BRDF/PDF/RNG, sample count and original null-sample denominator remain unchanged.
Direct lighting shares prepared GGX view terms and diffuse/total work; coated IBL preserves probe fallback and samples.
GTAO remains the selected implementation; the filtered-depth experiment was rejected and removed.
GTAO host/HLSL constants, slots and graph roles share one local ABI; six qualified DXIL products were byte-identical.
SSR also shares its constants, slots, formats and per-pass resource roles; all five selected DXIL products
were byte-identical at the R-02 checkpoint. Volumetrics also shares its local constants, slots and resource-use declaration; actual seven DXILs
are byte-identical in Build/translucency-replay-20261106. Other feature conversions remain incomplete.
The typeless reused front depth is accepted by HDR post, reconstruction and presentation through existing depth helpers.
Evidence: `Build/quality-lighting-20261105/RESULTS.md`, `Build/ssr-confidence-20261105`,
`Build/bsdf-demand-20261105/RESULTS.md`, `Build/coated-view-20261105`, `Build/v3-ao-filtered/comparison-chain.md`.
Static IR counts and prior isolated component timings are not executed ISA or whole-frame speed evidence.

Water-only rendering avoids crowd peel work; mixed water/slime retains the existing ordered path.
Merged Exit has one shared SRV; translucent banks use 18 SRVs and ten dense phases.
Mixed routing, solid thickness and ordered-vs-shape optical selection remain incomplete.
Source-absent static/dynamic geometry buckets retain no unnecessary draw records and explicitly clear their own arguments.
Source-present Cluster/HLOD-suppressed buckets retain records; dynamic empty must never clear static rejected arguments.
No-sun frames avoid three CSM visibility views. Indexed and mesh 24-frame slot-wrap cases passed validation 0;
settled static Meadow rejected counts were positive, and the 10-frame GPU LOD fixture passed.
Geometry route selection still needs whole-LOD/instancing versus Cluster comparison and cooked per-asset decisions.
Do not add duplicate pose banks or bounds work; exact pose sharing and actual camera/shadow/attachment demand remain scoped work.
Evidence: `Build/sparse-draw-ao-20261105/RESULTS.md`.

## UI, tools and unresolved work

GPU UI retains DirectWrite shaping/fallback, ordered clipped premultiplied draws and a glyph coverage atlas.
Existing layout owners retain shaped quads; color/bar-only changes reuse geometry when topology permits.
Atlas/page and target changes invalidate the appropriate offsets. Successful Main Present owns hit-testing publication.
The former WIC whole-screen bitmap, lock/copy and bitmap-specific recovery are removed.
VFX retains one canonical payload row with profile views and distinguishes CPU ingress from GPU birth acceptance.
Gameplay owns warning geometry and ticks; cosmetic budget must not discard mandatory warnings.
Architecture layers use generated CMake File API 2.9 link and compile interfaces plus small include/Private lint.
Production shadow bake actions are independent of the test runner; terrain producer inputs use their actual closure.
Rules-only determinism and UI text fixture dependencies are narrowed. Static-shadow input now has
its actual119-action/854-product closure; standalone probe bootstrap is implemented with its actual
129-action/894-product closure. Remaining B-02 work is source-current shipping/build combinations
and the unqualified identity/invalidation boundaries in the content coverage ledger.
R-02 recording and SSR declaration integration, S-01 membership/binding separation and D-01 preview producers are implemented.
Other feature PassParameters and wider shadow-change qualification remain open.
T-02 rechecks terrain archive identity only at real scene/Generation/producer-input changes or pending
admission retry. Terrain parameter publications decide baseline compatibility once; unrelated shader
changes do not invalidate the page. SurfaceDiagnostics reports usable/current backing.
Other open decisions include static atlas/overlay versus VSM, local/volume invalidation and final surface frontend selection.
Full offline multibounce, optical-route comparison, supported-device residency peaks and SDK failure boundaries remain open.
Grass ordinary-play popping and observed pickup glow-only appearance are unresolved; do not apply unverified quality changes.
DirectSR was not qualified in the earlier isolated checkpoint because its optional SDK was unavailable; check current build scope.
Prior AO candidate, discarded stochastic terrain candidate and old foreground measurements are historical, not current winners.
No current full-frame 60 FPS, all-motion quality, all-SDK or all-supported-device claim follows from the receipts above.
