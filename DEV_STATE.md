# DEV_STATE

AI technical continuity only. Current dirty checkout; no commit/push. Active goal:
Debug /Od/assertions/checked iterators, 1920x1080 Ultra100%,200 enemies,60FPS.
NOT achieved. No Computer Use active. Artifact root: Build/csm-ibl-20261025/.

## Active source batch — verification deferred

Latest human instruction2026-10-26: implement all accepted directive changes first;
minimize/defer builds/tests/runtime/FPS until the final integrated batch. No new build,
CPU/native check, fixture or performance run for current W02/W04/W05 edits.
Existing binary/evidence below is a PREVIOUS checkpoint, not current-source proof.
Order: W04 packet/clock -> W05 per-view rendered history (RenderData ownership done),
then W01 remaining binding/Dev admission +W09 animation/LOD; W10/W11/W12; then queued
integrated-doc review/adopted implementation; one required verification batch at end.
Independent W02 admission optimization source implemented in parallel.

Latest source follow-through: W11 main RestorePlan selection is ACTIVE in source;
layout cache publication/eviction is delayed until successful Render return after
all current borrowed layouts finish execution. No runtime hit-rate proof yet.
W09 now routes per-instance monster LOD through18 ranges, skin/page identity, shadow,
GI, Fresnel and Hero. Hero work capacity is12 contributors/168 diagnostic rows per
frame. Immutable interval lookup/enclosure preparation now runs once at asset
adoption; frame LOD/skin/shadow uses direct bounded rows and six per-family resident
masks. Shared-package hysteresis is being tied to the common canonical skin owner
instead of the previous submission of each individual view. W12 native timing producers are the next sequential
frame-owner task; cadence/drift/recovery value/controller and CSV sources exist,
but default-invalid facts do not qualify DRS and legacy reference remains.
PR13 changed layout/model publication is transactional through successful raster,
WIC lock and staging copy; whole raster/upload remains the explicit GR25 fallback.
W05 VFX hot-path follow-through: profile commits deduplicate in linear row scratch,
compaction reuses buffers and preserves unchanged rows/indexes, dispatch reuses two
transactional vectors, and raw events order session before tick so future-session
events wait without repeated heap pop/reinsert. Existing test sources cover these
cases but have NOT run. No build/test/cook/native/diff validation in this source phase.

W02: AdmittedGeometryHierarchy/AdmittedSceneHlod own decoder-validated immutable
metadata and per-page cluster/proxy indexes. RendererGeometryCatalog uses prepared
page validation; raw public validators preserve their untrusted-input contract.
No per-page whole-tree/all-proxy/topology revalidation on compiler admitted path.
W02 VFX atlas source: ContentLinker now emits seven typed mesh bounds/count rows;
Release consumes them, Dev derives/validates once at admission. Native atlas/HLSL
binary layout stays unchanged; shipping recipe is now 15 (previous generated output
and pack are recipe 14). Final batch MUST regenerate the shipping pair before build
qualification. application.cpp wires packed_vfx_mesh_atlas. No generated artifacts
were edited and no cook/build ran. Dev admission source handoff is recorded below.
W01 Dev source handoff: Generation ID/path/size/SHA directory plus bounded texture
metadata proofs and small authenticated materials; no startup geometry payload walk.
Character/animation metadata compilers reuse typed descriptor storage. Proof geometry
has payload_ready=false and cannot BindGeometryDescriptor. Actual requested source
read authenticates selected SHA, performs semantics and compares proof before ready
descriptor/GPU allocation/upload. Environment/cluster/HLOD reuse the prepared bytes;
ReadPackage decoder mode avoids validating encoded vertices before the immediately
following semantic vertex decode. Defaults still validate standalone callers.
Resident catalog and each stream retain their own owner across shader-only reload;
only actual resident-generation adoption swaps resident metadata. Unbuilt source.
Dev admission no longer rehashes the same full texture/geometry/character/animation
span after RendererAssetSource/AssetIo (or ReadProduct) already verified selected
Generation SHA. Caller audit: resident ReadBinary uses RendererAssetSource; async
environment/cluster/HLOD decode runs after verified IO completion. Admission retains
format/extent, metadata proof comparison and semantic decode; read boundary integrity
and generic package validation remain. Admit* exact verified-span precondition explicit.
W01 per-instance binding disposition: retain current dense RenderMesh/environment
variant and compiled material ordinal/base selection instead of adding duplicate
deployment handle fields to every RenderInstance. Native frame uses stable asset/
material IDs for identity/revision/capture, not per-instance product-name lookup.
Evidence: frame geometry bucket/page selection and persistent scene metadata,
Brixelizer fixed submesh material-slot bindings, scene sample content keys. Named
Archer clip ID lookup remains a small immutable array scan inside the shared package
palette build; no second generic cache/extra instance state added without a measured
reason. Stable IDs continue to serve snapshots/replay identity. This is a reviewed
alternative, not an unimplemented extra dense-field requirement. Source unqualified.

W04 SOURCE: ProjectDynamicTick and ProjectTickUi split; existing ProjectTick
composes them for fixtures. Simulation retains current+previous actual dynamic samples
independent of publication; catch-up publishes only final tick (lockstep/final smoke
still publish required ticks). UI only projects for published packet. Frozen UI loops
reuse dynamic values unless debug/harness refresh changes them; no whole GameReadModel copy.
Each SPSC packet owns its actual preceding sample (no previous UI); Consumer retains
one packet, not two delivered packets. Old consumer previous_publication_sequence has
no independent publication for carried samples (zero); existing exchange test sources
now supply producer-owned previous samples, including skipped-publication/concurrent
ownership cases. Tests remain unexecuted. Current publication sequence owns UI acceptance.
FixedStepClock exposes first actual boundary while preserving catch-up/debt policy;
RenderClockStamp records epoch/mode/boundary. Renderer package interpolation uses that boundary,
not packet arrival; Scripted/Frozen samples select current. UI-only publication does
not advance dynamic_pair_revision. Runtime VFX facts key uses actual sample revisions.
Initial actual sample is retained before ticking without an extra publication or event
fanout. Animation living state is seeded without consuming pending domain signals.
Initial projection cost is excluded by realigning the fixed clock after projection.
Resume/session reset/rewind realigns the boundary and stops old admitted catch-up steps;
normal running catch-up debt remains unchanged. Camera configuration is shared between
initial projection and subsequent dynamic/UI-only frames.

W05 SOURCE IN PROGRESS: one package evaluates shared instance/pose display samples;
birth/removal and incompatible content/clip changes select the proper endpoint.
Per-view last successful display sample survives native lease retirement and bounds
forward display time; epoch changes invalidate motion history without rebuilding all
session caches. Submitted instance history allocation occurs before native submission,
acceptance after graph/service submission success (before Present). Archer preflight/draw palettes share a package bank;
removed per-view root interpolation and redundant previous-instance lookup.
Runtime supplies RenderFrameInput.sample_time before VFX preparation; renderer samples
that same instant. Event lights/distortion/decal/volume/screen/ground/ribbon, retained
flashes, owner motes/fire tongues and entity surfaces now evaluate fractional display
age. Persistent adapter preserves actual position endpoints, fractional age, delayed
births, previous-only removals and previous status episode before replacement.
Warning events reuse PreparedPairFacts, compact in place and interpolate compatible
geometry with shortest-arc angles; discrete owner metadata changes at the endpoint.
Low-health state still advances only on actual observations; display PhaseAt and entry
fade use the preceding observation. Terminal death keeps fractional fixed-step time;
movement events are ingested on actual ticks and retained until display onset/expiry.
Stateful spark emission reconstructs actual tick age; indexed motes retain stable
owner/episode/source/ordinal births. Actual GPU world-step scheduling/ACK is unchanged.
Entity-surface phase derives once from owner endpoints, without duplicated exact float
age comparisons. Capture uses matching status episode; native resource guards remain.
Ballistic commands append directly into the pending queue; legacy replacement scans
only the old prefix. Persistent status shards also append directly, removing a second
temporary vector/copy; the value-returning entry delegates for existing callers.
Surface effect handles are resolved once per package instead of
rehashing/re-scanning the catalog for each event/capture. Ground persistent current_tick
uses inspected in source are command identity fields; its visible ages come from the
already sampled persistent input (not an additional integer lifetime calculation).
Existing test sources adapted/extended for endpoint lifecycle, angle wrap, fractional
ages, stable mote seeds, pulse/terminal clocks and ballistic queue prefix. NONE executed.
Surface event/persistent capture now falls back to the retained previous actor only
before the current endpoint, matching RenderSceneSample's removal lifetime. Current
actors still use the existing prepared index; the rare previous-only case adds no map.
W05 submission boundary correction: RenderedHistory (instances, skin, camera/projection,
origins, grass, temporal/HZB indices, scene lease, sample clock), camera-cut generation
and rendered frame counter now commit after accepted graph/service submissions and
before Present. RendererFrameResult.render_history_committed records that fact even on
later Present failure; wrapper no longer clears an already committed failed view.
Partial/pre-commit failure retains existing invalidation/fault paths. No queue/fence
policy changed. This native-history source change is NOT runtime/GBV qualified.
Explicit sample_time now bypasses per-view retiming and alpha round-tripping: caller
VFX and all views retain the same sample. Backward/epoch discontinuities still reset
motion history. Implicit direct callers keep the existing history floor; legacy
simulation_time remains a Scripted current-endpoint override.
Runtime main-view camera follow now uses the same package alpha as instances/VFX.
Instant second RetreatShot movement retains a per-player motion epoch across missed
publications. Ordinary RetreatMoved does not change it. Player/camera sampling holds
the prior endpoint until the jump; per-slot submitted epochs suppress previous GPU
transform and skin/grass history across a jump without rebinding content or globally
resetting renderer history. Source assertions added; not compiled or executed.
UI layout caching now updates independently of raster success: removed the entire
text-layout vector/string/COM-owner copy on each dirty raster. Bitmap validity and
per-frame upload completion rules remain separate and unchanged. Source only.
CameraView records the zoom height already included in target.y so follow interpolation
does not smooth user zoom settings; current yaw/pitch/FOV/distance are retained.
Existing scene sample test includes target interpolation/current zoom, NOT executed.
Remaining W05: all-consumer temporal/position history audit and final qualification. Do not
claim all effects synchronized or 60FPS. Subsequent W01/W09/W10/W11/W12 remain open.
W01 fixed packaged asset/material aliases now use constexpr IDs, not per-call hashing;
this is not completion of dense binding/Dev catalog admission.
W09 consumer/bounds source: ordinary monster LOD0 with matching cooked interval
position-enclosure metadata uses a conservative transformed origin sphere in GPU
visibility and skips vertex bounds reduction. Archer/suicide bubble/unknown metadata
retain reduction. Compact ranges/skin dispatch exclude qualified actors outside all
package camera views plus conservative CSM caster influence only when no unknown
consumer exists. VSM/local shadows/GI/Fresnel/entity surfaces retain all actors;
Ultra with GI/probes therefore gains only reduction elimination, not actor exclusion.
LocalProbe captures only immutable static scene (header/runtime rejects dynamic
characters and source-base shader bypasses skin), so its enabled flag no longer
forces offscreen character skin work. GI really consumes dynamic skin and remains.
Broader influence qualification and pose-aware LOD remain open. No tests/build ran.
W10 source: Hero composition step0/1 copies full active background; step2+ copies
the proven outward-rounded/clamped rect R at matching source/destination coordinates.
Unknown deformation/clip crossing or >8 candidates uses full copy. Only 1-2 actual
conservative candidate instances enable occlusion query/predication; dense3+ scene
retains the no-query path. Target generation owns per-slot/layer query results;
existing slot/global completions protect reuse and retirement. EQUAL_ZERO skips
empty deeper front/exit/composite draws. Tail skips only for layer0 empty (the
deeper-layer equality/identity edge does not prove tail empty). Clear/copy/guide/query
resolve remain unconditional. Capture mode disables occupancy to avoid nesting.
W11 topology must key actual use_occupancy_queries, not merely Hero enabled;
composition_rect is per-frame copy data. Renderer frame ownership transferred to
graph_plan_prepare for W11 producer integration. No W10 build/native quality proof.
W11 graph foundation source: opaque bounded PlanHandle restore, borrowed immutable
uses/names, per-frame import/pass/external bindings with native/state/lifetime checks.
Producer now conditionally selects RestorePlan through value-only preflight and
binds current imports/callbacks on warm frames. Exact branch/range source audit is
still in progress. Allocation/DXGI/reserved-tile owner contracts prove current native
shape/alias ownership before restored Prepare. Existing graph test source extended
only; no compiled/native proof or measured frontend elimination claim yet.
W11 producer split now active: graph_plan_prepare owns frame/main graph and
d3d12_resources owner certificates; animation_consumers owns transparency/HeroWork/
post helper cold-layout + warm-binding split. Parent owns AO/adaptive/subsurface.
Parent helper source complete: each exposes value-only *Layout, optional layout
capture in existing Register, and BindRetained(current inputs, retained layout).
Warm paths bind saved imports/pass ordinals and current callbacks, skipping all
Create/Import/AddPass/Read/Write/final-access declarations. AO topology includes
ao/contact/adaptive/history-valid/queue; physical history read index is rebound.
Input guide logical versions must match. Helpers still have current frame validation,
mapped constants/descriptors and completion/failed-submit ownership. Main frame
selection/caching is connected behind exact preflight; final source audit is ongoing.
Parent W11 bundle2 now includes ScreenReflectionsLayout and ScreenOverlayLayout
with optional cold Register/RegisterMask capture and BindRetained. All five parent
helpers (AO/adaptive/subsurface/reflections/screen) expose pure MatchesLayout for
pre-Restore selection. Each checks actual logical versions and hidden topology
flags without native queries or mutation. SSR history-valid includes consecutive
successful frame; sidecar count/ambient visibility branch matter. Screen active is
nonzero intensity plus at least one nonzero-alpha command; counts are frame data.
Their owned D3D12MA imports now EnsureNativeContract and pass cached certificates
on cold/warm binding; raw external guides still follow existing validation paths.
Volumetrics source helper handoff is complete through animation_consumers (7 passes,
14 imports, exact constant/volume/hero buffer use ranges, history and shadow variants).
Parent also completed LtcLutLayout cold capture, pure MatchesLayout(active/first upload)
and BindRetained with certified native imports; first-use staging lifetime unchanged.
Parent LightList/Decal warm source complete; earlier five helper preflight now checks
actual immutable target capacity/trace capacity/AO capacity/VRS tile/upload slot shapes.
LightListLayout keys capacity and max/inline/spill buffers, not live light counts.
DecalSurfaceLayout keys 0/N work, capacity/slots and actual logical input versions;
current command count/offset remain frame data under existing full buffer read use.
Parent HlodSystem + HLOD runtime wrapper warm source complete (renderer_hlod.* and
scoped methods in renderer_hlod_runtime.*; not views): PrepareFrameRecordData computes
CPU membership validity/history once; ephemeral data retains current owner/input address
and frame ID. MatchesLayout is pure. Six passes190..195 plus runtime copy196 bind saved
ordinals/versions and certified current refs/readback/selection buffers without declarations.
HLOD enabled fallback affects constants; history/upload/external-read flags affect topology.
Cold assignment may replace only logical input versions after preparation; keep prepared
input object at same address until Register. No stale frame data can reuse a prior ID.
Texture_streaming/feedback, shadow, HDRPost, reconstruction/presentation guide helpers
source handed off. Animation worker now owns LightingProbe/LocalProbe; Dense owns
SceneGeometry/Brixelizer GI helper source handed off; main Restore selection is active
behind exact preflight. Frontend reuse is recorded separately from compiler reuse.
Software GI is active in Ultra, so leaving its whole graph always cold is not accepted
as W11 completion; actual SDK/external-operation ownership boundary work remains.
Parent reserved texture source certificate connected: explicitly OwnedReservedTiles,
canonical resource/device/description issued at reserved creation and current revision
on import. Each mip mapping owns a unique D3D12MA AllocateMemory range until unmap
completion; no shared tile mappings. No committed-allocation claim for reserved memory.
Sampled/candidate imports use the current owner proof; fallback registration carries
its actual AllocationResource proof by value after EnsureNativeContract. Software path
keeps its allocation proof. Unknown external registrations remain uncertified.
Tile mapping, last_use, pending leases/fences/retirement logic unchanged; not validated.
Dense LOD producer emits optional certified actual LOD1/2
with ABI2 posed surface error + per-LOD interval bounds; consumer owner/residency and
all-consumer choice remain in progress. No actual products were cooked. Main graph
agent owns global warm selector/direct-pass source audit. Steady key has three frame
slot plans. Independent history/capture/upload transitions can combine beyond the
bounded eight and evict transient plans; no hard six-plan transient bound is claimed.
GI and SceneGeometry source keys now follow unique actual read dependencies/capacity,
not per-actor leaf multiplicity, frame IDs or dispatch counts. No runtime key-hit proof.
Source only; all current checks remain deferred.
W09 dispatch preparation: index last poses once, reuse SkinCacheSample.current_pose,
prepare one SkinDispatch list for skin and bounds
passes instead of repeating instance/asset/clip/range lookup. Current/previous
offsets, both GPU passes/UAV barriers and package-only canonical reuse are unchanged.
Dispatch list remains local and allocates only for a non-reused skin package.
Character geometry is synchronously admitted and frame bucket admission precedes
skin-slot assignment; missing active skin geometry is not ordinary streaming wait.
Broader consumer influence qualification and pose-aware LOD remain open.
Source changed in Core/RenderData/Presentation/Runtime/Renderer; NOT compiled/qualified.
Final validation must cover skipped publications/adjacency, pause/resume, catch-up debt,
session/rewind/debug refresh, UI selection/lockstep and per-view rendered sample behavior.
W05 render-history and other remaining directive work still open; no W04 completion claim.

## Current directives-first work

W07 residual source: cast runtime lookup now uses a derived cast-ID/index map;
the ordered vector still owns iteration/checksum semantics. Reset clears the map;
cleanup rebuilds it only when vector compaction removes entries. Creation uses one
try_emplace, matching existing hit-index handling. Slow read/expiry and due/sequence
ordering were already implemented; no extra status cache added. Not built/tested.
R03 source: startup/resident-generation DFG LUT copy now records into the existing
environment texture upload list and shares its retained staging/completion wait.
Removed the separate startup/resident DFG submit/drain; standalone shader-reload
creation retains its own failure-safe submission path. Resident preparation no
longer requires LUT existence before CreateEnvironmentTextures creates it. Unbuilt.

Human priority: decide apply/replace/reject once for every directive, then finish
accepted implementation before more optional measurement or minor optimizations.
Current disposition: .planning/2026-10-23-complete-directives/directive_status.md.
Once-reviewed prior child ledgers contain stale pending/build notes; this state is current.
New integrated documents received2026-10-26 are queued AFTER all prior directives.
Exact copies/hashes: .planning/2026-10-23-complete-directives/references/integrated-20261026/intake.json.
Intake only; their contents have NOT been reviewed/adopted/implemented yet.
Continue current W01 and prior accepted tasks first; then review and implement the new set.

W01 bulk producer + renderer readers implemented; full W01 still OPEN.
Current HEAD observed0f03f5a(user commit); changes remain uncommitted.
ContentLinker explicit runtime closure generates2325 renderer bulk rows,64-byte
aligned pack1415874687bytes, format2/layout13/recipe14 and deployment33ab65a7...
Typed texture schema extends the unchanged1415874687byte pack body; exact body SHA
8d193f5c465ec3975bb7f55262cfaf409e94c74ec9a3fc61995081f652f90886.
Current pack/generated-source/OpenImage checkpoint:
typed-provenance-verification.json. Current value ownership source/native checkpoint:
value-ownership-verification.json +value-ownership-source-ready.json(695unchangedinputs).
Earlier bulk/reader/texture/geometry/material receipts are prior checkpoints.
Both Debug apps freshly relinked for value ownership ABI; report in value-ownership-app-link-20261026.
Main180 smoke exit0,62.465s, assertions passed, checksum6077983588339752369 unchanged.
Evidence value-ownership-app-smoke/result.json; not a performance measurement.
Legacy PCM/font prefix and game hash5323372167194177377 unchanged; producer verifies
every source stream. Prior output readback covered10representative formats,notall2325.

Renderer synchronous readers, texture/environment/cluster/HLOD streams, direct VFX
atlas and probe reads now use RendererAssetSource: loose Dev product or retained
Core owner+BulkRange. Pack sizes come from ranges,not file_size; no pack pathname
copies/reopens. Static shadow pages read bounded subranges with existing page SHA,
not the entire pages bulk. Verified geometry digest is reused through preparation.
Packed resident preparation skips dependency-graph preflight reads/decodes; actual
read/decode/native allocation budget admission remains. Feedback shader selection
uses compiled descriptor variants. Explicit developer Generation reload clears
packed selection; a missing new probe must fail rather than silently reuse pack.

Decision replacing pseudo second native pack handle: actual optional DirectStorage
path is FILE->CPU MEMORY with no compression,not GPU direct. Retaining its file
blocks atomic pathname replacement(ERROR_SHARING_VIOLATION reproduced).
Pack therefore uses the SAME retained Core native file already validated by
Runtime OpenImage, with range bounds/digest and async/cancel/retirement ownership.
Removed new DS pack cache/header reread/extra32MiB transport. Optional loose-file
DirectStorage stays unchanged. Core Identity(move/failed-open/atomic replacement)
remains cached once, no per-read metadata calls. This is an evidence-based replacement,
not a claim of literal pseudo DirectStorage compliance or pack GPU-direct IO.

Current W01 typed textures: const TextureDescriptor tables cover63 2D arrays+1 3D
volume,3297 subresources. Compiler reads/verifies each texture once, ReadPackage once,
then dispatches TXM1/TXV1 using existing validators. Emits typed dependency IDs,
product-relative offsets, row/slice/full-byte pitches, dimension/depth and bulk index.
Metadata enters deployment identity. Actual bulk bytes and legacy hash remain exact.
Packed startup, material/VFX/probe/GI texture reads bind admitted metadata to existing
authenticated bytes without package parsing/dependency strings. Streaming retains
TextureHandle and immutable table span. Native footprints/state/fences remain.
No mip payload subset IO yet; actual product bytes still read/authenticated as before.
Dev loose path retains original parsers; Dev typed-catalog admission remains open.

Latest scoped checks: typed-texture-build-check.log Debug package/producer/runner/
renderer/Runtime static library built. content.render-package-format passed0.67s;
unchanged architecture result reused. Final native residency12frames normal shutdown,
validation0;2323 bulk products/64texture rows(1volume) from prior compatible
test-render Gen818d6bfc...,not2325-row Release installation.
Typed fixture19requests/11uploads/publications/reclaims; geometry1each and target
terrain mip1 completed. Missing developer-probe reload rejection remains passed.
Actual generated CPP constinit compiled+linked+executed18.94s: ALL64texture rows,
3297 layouts,461575887 authenticated product bytes,10414588font bytes;2D/volume
metadata/pitch/dependency/byte-alias parity with old parsers and wrongID/layout
rejection(owner retained) passed. Evidence typed-texture-image-20261026/.
Source53file and actual Debug binary hashes: typed-texture-verification.json.

Pixel comparison is NOT qualified: old18 vs new11 asynchronous publications at
capture;568858/921600 pixels differ,maxchannel111. Both images visually inspected;
keep exact mismatch evidence typed-texture-pixel-comparison.json, no equality claim.
Do not use elapsed initialization39.05->27.78s or render-loop intervals as a clean
performance comparison(driver caches/residency differ). No new FPS measured.
Full GPU validation193.6s timeout from prior checkpoint remains unqualified,
not repeated unchanged. Earlier DS-cache sharing failure remains preserved.
Native optional SDK/Core coexistence test from prior unchanged source still valid.

Integration fixes preserved in logs: CPU bulk helpers separated from RendererConfig
bridge(renderer_content.hpp) to keep package tests renderer-independent; direct
virtual-shadow bridge include fixed. New tests use AssetId.value(no equality operator).
Actual fixture exposed TXV1 volume, so same typed table supports both dimensions
rather than dropping volume. New negative-volume fixture had wrong metadata
count/stride; fixed fixture only, validation rules unchanged. No hidden failure.

Current W01/W02 geometry:1753 generatedGeometryDescriptor rows bind7 raw GPU
chunks and compiledsubmeshes without restoring GeometryData/ReadPackage in pack
direct,character,environment,cluster/HLODupload paths. Existing Dev prepared decode
remains. Shared Apps/Common/renderer_geometry_catalog.hpp owns one offline context
invocation: directcharacterLOD,hierarchy,HLOD semantics validated before compile.
Source metadata/pages are verified once and those samebytes written. Metadata
contains flags/counts/bounds/all4maxboneinclzero-weightindices; typeddependencyIDs,
rawranges and210 exact Archer ground-supportvertices. Character-specific semantics
preserved; prepared-character overload avoids repeated fulltopology inDev/compiler.
Compiled runtime character uses validated counts/maxbone/material; no fullvertexrestore.
Archer support comes from C++ table; procedural36vertexpage stays existing local path.
GPUuploadcopies refs/triangles/remap straightintostaging; temporaryauxvector removed.
Compiledenvironment/cluster reads no longerreserve2xdecodedGeometryData memory.
Removed perpage whole product-map scan for source name; actual caller ID retained.
Native allocation,initialstates,copy/queue/fence/cancel/slot ownership remain unchanged.

Actual schema10 generatedCPP(5665357bytes) compiled/linked; all1753geometryrows
qualified via4 disjoint rowruns(18.17/19.67/18.50/8.50s), exact coverage.
8,374,230vertices;19,676,559indices;141,181meshlets;812,819,520authenticated bytes;
maxpayload7,302,307;210supportvertices;8skinnedrows. Checksallchunkspans/countstride/
aliases,deps,submeshfields,CPPsupport and64retainedtexturemetadata; font/hash unchanged.
Two monolithic60stimeouts retained; redundant same-addressbytewalk removed, then
boundedshards completedallranges. No validation criteria weakened.
Evidence typed-geometry-image-20261026/sharded-result.json +commands/hashes/logs.
Packbody byte-identical; schema10deployment0839b87b...,recipe11.
New scoped packageCPU0.77s+architecture0.36s passed. Nativecompiledfixture12frames
normalshutdown/validation0,1753geometry+64texture rows. Existing environmentgeometry
request/upload/publication/reclaim1each. HLOD matching_scene=false: no fullnative
HLOD scene qualification claimed. No new FPS/pixel/temporal/fullGBV claim.

Character checkpoint mainapps: fresh3entryTUs compiled /Od/MTd/RTC1(/Z7 debug-info only)
and linked exactNativeNinja libs/manifest without contentrecook. Link inputs hashes
stable, exports/PMv2verified, oldappsbackedup, actualBuild/msvc-debug outputs published.
ProjectHS SHA7720eae5...;hs_experiment176d123e...;character-app-link-20261026/report.json.
Fresh180tick/180frame main smoke26.196s,execution/assertions true,validation0,
checksum6077983588339752369/gamehash5323372167194177377;character-app-smoke/.
No freshFPS/200enemy/fullGBV/controlledpixelclaim.
NewProjectHS --smoke --smoke-render-lockstep --ticks8 --validation on priorcompatible
Gen8ba16d... passes:8ticks/8frames,assertions_passed=true,validation_errors0,exit0,
normal21.80s. No pendingApprelink. Smoke is not180framefullgate or200enemyperformance.
Current62source/binaryhashes +exactscope: typed-geometry-verification.json.
Current material checkpoint:25 descriptors(16Environment/7Hero/2Decal),101default
slots,50programs,60typedtexturebindings emitted. Packed surface/parameter/environment/
Hero-domain/decal consumers no longer read/parse material/default packages.
Actual generatedCPP compile/link: all37291material+parameterbytes agree field-for-field
with existing decoders; execution0.692s. Font/gameplayhash unchanged. BodySHA exact,
CPP5711844bytes,layout11recipe12,deployment45abdba3...;typed-material-image-20261026/.
PackageCPU0.81s,architecture1.93s passed. Native12frames was INCOMPLETE because
terrain async read remainedpending; preservedreceipt,notcountedaspass. Native64frames
then passedvalidation0:19texture requests/uploads/publications/reclaims,1geometryeach,
representativemip1 completed;missingDevreloadrejectiontrue. No newGPUvalidation/FPS/
controlledpixeltest. TestflagcurrentlyON forbatchedcharacterchecks; noCUused.

Current character/animation checkpoint:8CharacterDescriptor+1AnimationDescriptor.
Generated smallmetadata references admittedgeometry/material handles; originalauthenticated
subranges retain40BTRS and64Balreadytransposedpalettes. Runtime reads directly into
packed40Btransformvectors/nativepalettematrixvector; nocharacter/animation package
parse,fullpalette scalar decode/copy,or largegeneratedC++transform/matrixarrays.
Staticmetadata spans andDevownedstorage followresidentcandidateswap/reset/accounting.
Named andlegacysampling/blending/groundmath preserved; accessedpackedrows convert
intopaddedCore48Bvalues. Compilerreusesexistingdecodedcharacter+PackageView after
allgeometryadmission, exactskeleton/mask/FBXdomain/LOD/boundschecksretained.
ActualgeneratedCPP6613823bytes compiled/linked; all8+1metadata,211bones,51clips,
16bindings,7LODs,1820intervals,116130TRS/35550matrices allfloatbits/rawuploadbytes
matcholddecoders. Reads7014741relatedbulkB+6920400directrangeB,1.508s; no unchanged
812MBgeometry/461MBtexturecorpus reread. BodySHA unchanged/layout12recipe13.
CPU4/4:architecture0.10s,character-interchange1.13s,package0.74s,skin-cache0.12s.
Native64framesvalidation0,19texture+1geometrycompletedandallreclaimed;8chars1animation,
missingDevreloadrejectiontrue. Actualmain180smokepassedabove; noFPS/perfqualification.
Source690hashesunchangedthroughallchecks. TestsremainONfornextbatchedshaderslice;
noCU/game/sampler remains. Duplicatefixturepacksremoved,receiptsevidencekept.

Shaderdescriptor decision: existingRendererBulkDescriptor(ShaderDxil+ID/range/digest)
plusMaterialProgramDescriptor(kind/index/ID/name) alreadycontains itsrequiredmetadata;
standaloneShaderDescriptor tablewouldduplicateit. Material PSO preparation now consumes
dense program indices through ReadRendererMaterialProgram, preserving feedback/failure behavior. Environmentvisibility/resolve variantslackprogram
kinds andretainexistingtypedbulklookup, notinventedsemantics. PipelineCache currently
rescansDXILforPSOkeys; thisisNOT rereadingfilesperPSO andnotyetoptimized.

Completed provenance/Release composition: 564 source revision rows and631 bulkrefs
reconstruct all761 canonical material revision entries without bulk rereads. Native64
passes with both Generation maps EMPTY; missing Dev reload fails preserving residents.
Release content path no longer loads SelectedGeneration. Audio retains generic owner.
ImGui uses owned copy of compiled font bytes, fixing loose-font assertion in empty install.
Hybrid Release-content/Debug-native180frame test: validation0, same checksum6077983588339752369;
release-content-no-generation-font-20261026 receipt. This is NOT a full Release build.

Value ownership migration: plain snapshots/frame/VFX inputs moved Core/Renderer ->
RenderData; native-free hs_render_data_values target. Concrete snapshot exchange moved
Core -> Presentation; RenderSnapshotPair remains standalone Data value. No forwarding
headers. Four snapshot/lifetime tests moved to existing scene-sample test target.
verify-core18/18 (RuntimeOFF/ToolsOFF), focusedCPU3/3, native64 validation0; all19textures
and1geometry publish/reclaim.695sourcehashes unchanged through verification.
Receipt value-ownership-verification.json. No FPS or full application claim from this.

A04 single-config Release RuntimeON/ToolsOFF now uses existing Shipping/content.generated.cpp
and Content/content.hsp; missing files fail configure. ProjectHS excludes producer/Generation
verification/DXC/ImGui/font binary inputs; actual native libraries and font license retained.
Configure-only negative/graph checks passed; full actual Release compile qualification passed
in Build/msvc-prebuilt-release, receipts prebuilt-release-20261026. No content recook.
Actual Release compile exposed unconditional SelectedGeneration include in common VFX
rebind header; gated its developer-only include/closure helper with HS_DEVELOPMENT_TOOLS.
Common handle rebinding remains available; incremental Release retry passed.
Actual full Release180frame smoke passed,47.373s,validation0, checksum6077983588339752369.
No GenerationState or loosefont exists in build tree. Receipt prebuilt-release-20261026/verification.json.
This is deployment qualification, NOT Debug200-enemy performance or atomic installer qualification.
W02 cluster catalog: removed unused Load depth computation/member and PrepareGpu repeat
whole-hierarchy validation. Decode admission owns private immutable hierarchy; actual GPU
depth calculation/rebased ranges and page/native validation retained. Host Debug build passed;
native64 passed validation0,19texture+1geometry publish/reclaim, emptyGenerationmaps.
Receipt cluster-admission-verification.json (three changed source inputs unchanged). Initial sandbox C1902
failed; same pinned MSVC14.44 host settings compiled without changing Debug optimization.

A04 immutable Release install implemented: early sibling-stage hook redirects all
existing Runtime/dependency install rules, process lock prevents concurrent publishes,
nonempty target and stale stage reject before copying. Finish checks required runtime
files, invokes staged ProjectHS --verify-shipping-content (existing OpenImage64byte
header+extent, no window/device), then NO_REPLACE directory rename into missing/empty
prefix. Empty prefix parked and preserved on failure; CMake prefix/DESTDIR/manifest restored.
No in-place upgrade or Windows atomic replacement of existing populated directories.
ProjectHS Dev macro now matches Runtime and excludes shipping table calls in Dev.
Tests/check_release_install.cmake + tiny compiled verifier fixture cover interrupted
prepublish/retry, missingnative, verifierfailure, nonempty/dotfile, missing/emptyprefix,
defaultcomponent, DESTDIR, CPackZIP and published manifest. Worker test passed.
Actual fullRelease rebuilt; install sixfiles into immutable-install-20261026/release
succeeded. Actual CLI17ms positive; wrongheader/missingpack rejected, restoredpair passes,
existing-install retry rejects preserving files; verification.json retains evidence.
Actual installed-game180frame run passed exit0,41.570s,checksum6077983588339752369.
Ordinary shipping options/no debug DLL, no validation activation claim from this run.
Debug apps isolated3TU relink passed; explicit Release-only command rejects in Dev(1),
without launching a window. Tests cache restoredOFF; no content recook.
Optional old scratch install-fixture cleanup auto-review rejected (authored test inputs);
small scratch files preserved, no bypass/retry. No implementation is blocked by cleanup.

Next prior-directive work:
dense instance bindings, Dev typed admission; then W04/W05 temporal packet/history work.
Geometry compiler still repeats some hierarchy/page topology validation.
W04/W05/W09..12 and60FPS/quality qualifications remain. Newintegrateddocs stayqueued
untilprior directivescomplete; no contentreview ofthose docs yet.
No second rendererbackend/registry introduced.

W02 PreparedGeometryPage source integrated in RenderData and Renderer startup,
streaming and upload. One ReadPackage + vertex/topology validation, borrowed spans
retain caller read bytes through CPU copy. Metadata-backed decode skips discarded
PayloadBounds and duplicate full topology pass. Character previously parsed4times
and decoded geometry twice; environment startup3parses; stream2parses. Now1each.
Character ValidateCharacterGeometry binding remains; original CPU bytes and native
staging/fence/cancellation lifetimes remain. Existing malformed-data tests extended.
Source receipt prepared-geometry-source.json (8affected files only).
W02 CPU content.render-package-format passed0.67s. New render-only runner linked;
existing12frame residency-streaming-v3 native probe passed,validation0. Actual
geometry requests/uploads/publications/reclaims1each; all resources19each.
Previous test-render Generation818d6bfc... used intentionally: format/shaders unchanged.
Source8file receipt still matches. Logs prepared-geometry-build-check.log (CPU),
prepared-geometry-native-check.log and prepared-geometry-residency/result.json.
Broad production/content target was stopped, not a passing full Runtime build.
ProjectHS/hs_experiment are now freshly linked in the geometry batch above;
8frame main smoke is scoped evidence,not full runtime/performance qualification.
Two integration faults fixed: wrong adjacent private signature and missing meshlet
triangle_count copy. Initial failure logs preserved. Existing checks were not weakened.
No live build/test/profile/CU; HS_BUILD_TESTS restored OFF.

A03 verified-byte ownership implemented in RenderCharacterTexturePackage/main.cpp:
Current uses one ActionResultSnapshot and ReadProductBytes directly; Produce owns
one verified DDS buffer per texture through packing, never reopens authenticated
paths. Existing role/package/dependency validation remains. Temporary input-memory
peak increases because all invocation sources are retained. No permanent proof cache.
Only producer rebuilt. monster-materials(3products) and archer(10products) each ran
Produce then Current; ProductSet IDs, bytes/digests/sizes/dependencies unchanged.
verified-texture-build-check.log and verified-texture-comparison.json. Existing
ActionSnapshot checks actually belong to scene-hlod-cooker, not render-package-format;
this W01 batch ran that target and passed its old/new owned/streamed-byte cases.
No fresh gameplay FPS for W02/A03; these are startup/cook preparation reductions.

Latest fully verified raster-cache build: CPU instance-order, Runtime integration,
render smoke, indexed-before/indexed-after GBV/mesh3frames all pass,validation0.
Native180frame PNG AND HDR byte-identical to prior spatial-demand baseline.
Monster fixture changes24251/921600 pixels,max4channel(Indexed); Mesh281,max3.
Limited numeric comparisons, not whole optical/temporal qualification.
Preserved *-csm-ibl-raster-cache.exe and GenerationState/csm-ibl-raster-cache.json;
exe SHA93a2e1a03cddf123208a94cf9267d084a946f688cced13b9f9b2f9cbd261bf03,
Gen8ba16d96635a069a4ac502b411360ef79779232b09fe1d25177df101858a1f3a.
703source receipt raster-cache-source-ready/built.json covers this build, not W02.

Actual Debug1080pUltra200 foreground raster-cache28.1217->29.4749FPS,
p50/95/99/max35.43/40.39/44.13/47.93 ->33.87/39.43/42.24/45.18ms,
1%low21.99->22.92,50ms+0. One ordered comparison, not performance adoption.
565selected CPU/GPU pairs,normal exit,extent verified; OS sampler remains partial.
No additional HeroWork/ablation runs now: directives implementation takes priority.
No CU/game/sampler remains.60FPS NOT reached.

Admit-once subsequent source batch built and owned-view3packages/9submissions pass,
validation0. Removed repeated per-view light/decal checks, particle mesh-selector and
ballistic revalidation; all source generators default selector0/nonballistic,
public span still validated before ANY view. Added malformed light/decal/ballistic/
selector preflight cases to existing fixture; rejection preserves accepted evidence.
DrawGeometry uses one descriptor-size query and one mesh pipeline search.

Next accepted work: remaining W01 renderer typed catalog/packed deployment,
then W04/W05 clock/actual history. Other W09..W12 remain open; timestamp proposal
is still UNAPPLIED, user asked explanation, not approval. Do not infer authorization
from that question. Ordinary GPU timing opt-out already exists independently.

## Earlier spatial-demand source checkpoint

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

Core18/18: Build/csm-ibl-20261025/value-ownership-core.log (current ownership migration).
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

Current source status is in the active batch at the top; old check logs above cover
only their recorded source checkpoint. W01 pack/typed metadata and W04/W05/W10 source
have advanced; W11 exact global selector, W12 metrics/scheduling, and W09 frame LOD
selection/history still require implementation/qualification. Native failure and
optical/temporal/stress/long-run qualification remain. No60FPS/full-directive claim.
Existing cooked images remain LOD0-only until final cook; new producer/resident code
supports qualified compact LOD1/2, but no actual asset acceptance count is verified.
