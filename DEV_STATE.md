# DEV_STATE

AI implementation continuity only. Workspace is dirty; preserve staged and unstaged
edits. User-requested checkpoint011ef96 `Continue engine optimization and
simplification` was pushed to origin/main on2026-10-27. Newer changes are local;
no further commit/push or worktree requested.

## Objective and execution policy

Goal remains ACTIVE: Debug /Od/assertions/checked iterators,1080pUltra100%,200enemies,
60FPS and all accepted earlier directives, then queued51d5055 review. NOT achieved.
Preserve all dirty edits. No Sol subagents; current workers
are explicit Luna/high. Deferred W12 per-queue timestamp/readback separation stays
untouched; native resource/fence/state/alias and actual submission contracts remain.

## Current source batch and execution policy (2026-10-27)

Latest human steering: SOURCE CHANGES FIRST; defer all new builds/tests/cooks/native
playback/FPS to one later qualification batch. No active build, game or Computer
Use session. Whole-scope engineering estimate55-60% complete /40-45% remaining,
including unqualified source and queued51d5055 detailed review; not item counting.
Queue: .planning/2026-10-23-complete-directives/remaining-work-20261027.md.
Current worktree is NEWER than all executable receipts below. Never claim its FPS.

Native ribbon update reuses source-row workspace and a grouped-output buffer rather
than constructing them per frame. Output-capacity preflight uses packed inclusion
bits in the same source workspace, preserving its conservative retained-tail bound.
OutputsMayExceed is now a nonconst operational method because it owns that scratch;
all current callers already hold mutable state. Both preflight and update clear
logical scratch sizes before return so conditional state checkpoints copy no work
rows. The live output vector and slot histories remain authoritative. Added fixture
sources cover130 slots across bit-word boundaries, repeated early rejection then
valid preflight, warm grouping/shrink and checkpoint restore. None was executed.
Dynamic ribbon duplicate validation now sorts source indices in that same scratch
instead of allocating hash nodes. Native frame validation and public Update call
ValidateFrameSources; the static checked ValidateSources wrapper still accepts raw
callers using local workspace. All source/output conditions and error precedence
remain, and motion decoding is reused within each output check. Empty/single-source
validation needs no duplicate index. Source-key ordering deliberately ignores
importance so a separated duplicate with another priority is rejected. The new
fixture covers rejection, valid recovery and unchanged prior outputs; it is unrun.
Texture-payload ownership was rechecked: completion->read output->texture transfers
move the same byte owner; prepared subresource spans borrow it through upload. The
remaining payload copy populates GPU staging. Do not remove it or the fresh-read
digest. Windows one-shot SHA already uses the CNG pseudo-handle; no hash backend
replacement or IO coalescing was justified without throughput evidence.

OwnerMesh native recording now batches adjacent commands with the same actual
vertex count into DrawInstanced runs, preserving source order and per-row atlas
mesh identity. NextVfxOwnedMeshDrawRun is a no-allocation RenderData-only helper;
the VS reads command[PassValue+SV_InstanceID]. Both native draw starts remain0.
Root layout, upload row ABI, SRV backing, validation and completion are unchanged.
Frame ingress already admits mesh indices1..7 and the upload path bounds count to
UINT; run first/count therefore stay in that command range. OwnerMeshPS consumes
per-row varyings, not the draw selector. Flash already uses blend-group instancing
and was preserved. Existing value test source flattens batches against per-row
draws (empty, mixed IDs/equal counts, alternating sizes,200 rows, final/end case).
Runtime shader producer still compiles OwnerMeshVS/PS under the existing entries.
No shader cook/compile, native smoke, image parity or timing was run; current native
qualification requires regenerated shaders plus matching renderer workload.

Current VFX continuation batches typed ballistic generation across the full event
view. The append helper returns exact logical input indexes for rows that emitted;
Runtime derives Mesh/ShockShell suppression from those exact events, preserving
duplicate-sequence semantics. Sequence and final lifetime bind during emission,
removing later per-output repair scans. Cone cosine and each particle's azimuth
sin/cos are evaluated once in the same scalar expression order. Both event and
persistent append retain prefixes on allocation failure. The independent old-formula
bitwise command/provenance fixture is source-only; no current executable evidence.
New persistent status-burst owners now select through source-row scratch once:
eligible/unseen rows sort by owner/kind/source ordinal, retain the first duplicate,
then return in original source order. The same unborrowed scratch used for ballistic
provenance is reused after that stage consumes it; no new registry or vector owner.
The selected-owner shard helper reads original persistent rows directly, removing
the copied owner/payload vector. All-owner and indexed calls share validation and
emission, empty selection means no output, and an invalid row rolls back the output
prefix. Indexed emitter fixture compares a reordered/duplicate copied-row oracle
and valid-then-invalid rollback; it is unexecuted.
The existing episode set and reserve-before-publication contract remain. Actual
player/projectile VFX stable IDs already include activation/episode+session; no
redundant generation field was added. A source-only old nested-scan oracle covers
all five kinds, repeated/different metadata, rejected/seen rows, reordered input,
scratch reuse and session reset.

Latest ZIP review priority is reconciled in
.planning/2026-10-23-complete-directives/ponytail-deep-review-011ef96-20261027.md.
Both reports, deep D01-D24/K01-K06/A01-A12, repeated P01-P12 and attached
model/source ledgers were reviewed; embedded scripts were not run. All decisions
are against this dirty tree, not inherited011ef96 execution results.
Selected source changes: streaming uses current O(1) host admission facts instead
of per-request full Accounting; zero retiring count skips allocation collection.
Asset IO shares32 CPU read/verification permits across overlapping owners. Permits
survive completed-but-uncollected futures, release after terminal Poll/owner join,
and do not release on Cancel. Busy reads defer before ledger transition and read
owner allocation; native256 status slots/queue bounds remain separate. Uncommitted
metadata reservations release on defer; committed transaction metadata stays owned.
Fallback range metadata moves once; native verification keeps a retry-safe copy.
Graph topology now owns logical first/last uses and ordered alias descriptors;
warm native binding does not rescan uses or build member descriptor vectors.
SameDescriptor field comparison replaces alias-view descriptor memcmp. Current
import/fence/state/neutral-view/claim rollback stays live. Native cache fixture
sources cover cold/warm/prior-plan restore, unused resources and different backing.
GPU scene Commit consumes the exact removal list prepared before submission.
Renderer VFX compatibility uses one typed-ground coverage pass and indexed probes;
previous-row index preserves duplicate first match via source ordinal. The real
value preparation lives in renderer_vfx_compatibility.hpp with caller-owned scratch,
and existing native-free instance-order tests have an independent nested-scan oracle.
Light preparation uses bounded pointer/ID scratch, directional/local ordered passes
and reusable actual-sized records; zero-light dummy clears. No new camera cache.
Task submission rejects under lock before packing, rechecks on reservation, and
avoids shared callable owners in synchronous fallback. Actor no-op erase/clear does
not advance structural generation; Debug ID scans cover newly appended rows only.
Conditional items remain unadopted pending evidence: incremental CNG, range event
reuse/coalescing, CPU graph chunking, external fence pooling, fused dirty channels,
view-common light/camera encoding, sample leases, area-ID set and PSO lock splitting.
Keep row consistency assertions while columns remain publicly mutable. This is a
source batch with unexecuted fixtures, not final native/performance qualification.

R-V01 continuation now bounds decorative persistent-fire expansion before building
rows. Prepared sources own copied owner/recipe facts and count eligible ordinals
arithmetically. One64MiB soft logical CPU-workspace growth budget includes retained
session/raw/decoded/frozen-native batches, pair indexes, prepared profiles/births,
spark/movement owners, app vector capacities and nested payload/output buffers.
The current native batch is prepared before optional admission. Existing unused
flash capacity is credited, including under pressure, so no shrink/regrow loop or
permanent suppression is introduced. Authoritative fire and existing stamps reserve
their rows first; the recipe carries authored authority through cold preparation,
canonical identity and generated C++. Only decorative rows share a fair capped
quota of newest original ordinals; uncapped output keeps old math/order/seed/ticks.
Caller retains prepared storage, reserves the output once and keeps original
flash/mote/fire/stamp order. Four trailing performance columns expose admission
bytes/requested/admitted/limited; render.json records budget/peak/limited frames.
Budget-bound rows are quality fallback and cannot substantiate equal-quality FPS.
This is NOT a hard aggregate R-V01/process/native allocation bound: activation
fanout, other cosmetics, transient library allocations and external immutable
owners still need their admission boundary. Container capacity/hash estimates are
not RSS and reserve may overallocate. Fire parity/fairness/long-age/authority,
capacity-credit and generated/identity fixtures are source-only. Full source
diff whitespace check passes; builds/tests/cooks/native remain deferred.
Next activation boundary is after DispatchVfxUpgradeEvents and before typed adapter
fanout. Legacy emitter rows, summed ballistic counts, flash rows and burst-mote
count*output bounds are available from accepted data. Safe-sector adapter cloning,
owner/status routes, retained profile occupancy and live rate durations require
their own concrete terms; effect_output_profiles is a mask, not a count. A whole
effect is cosmetic only if must_match_logic is false and all sources are neither
authoritative nor ProjectileFollow. Unknown/mixed events remain essential. Do not
introduce a guessed universal count/byte factor or sequence-only grouping.

Latest focused P01-P12 review is queued ahead of older directives in
.planning/2026-10-23-complete-directives/focused-review-011ef96-20261027.md.
Streaming P05/P06 now cache native mip costs/minimum and sort over-budget victim
indices once in exact old priority/tie order. Host intrinsic threshold scans cache
by fresh cap, and cap clamp keeps better published detail. UI P07/P08 now share a
RenderData UiDrawContent independently from each fresh receipt UiFrame; ordinary
Playing facts follow actual displayed time/labels/waves, not raw advancing tick.
All recognized UI phases/pages now collect their actual visible dependencies;
paused Root still includes the HUD drawn behind its panel. Character pages omit
hidden HUD state; result/selection pages include their visible overlays. Only
unknown phase values retain the conservative fallback. Semantic equality still guards new-owner/
raw cache hits. Parent removed unnecessary raw-model copy on unchanged hits. P09 caches static GI
base draws/classification on StaticGpuScene; P10 uses bounded page marks. P11 reuses
EffectiveAttack and only invalidates small facts at actual mutations/advancing tick;
checksum dirtiness and owner-thread/active-borrow checks remain independent. The
new internal fixture is source-only. None of this source has current FPS evidence.

Latest supplied review against checkpoint011ef96 is reconciled in
.planning/2026-10-23-complete-directives/review-011ef96-20261027.md. Source closure
and owner/identity correctness precede further optional optimization. R-B01 now
uses one native-free Runtime contract INTERFACE, including a standalone VfxImage
header TU in the existing low-level test; no native Runtime link. R-B02 private
EffectId helper is renamed HashEffectName. R-B03 typed overload dispatch and R-A01
two Ground semantic fields/mutations were already corrected locally. Ground now
uses a field inventory shared by codegen/canonical writers (typed encoders reject
unknown types) and a versioned canonical VFX identity domain; pack wire format/ActionKey are
unchanged. R-B04 public
result and R-G01 actual other-view owner/zero-GI merge are source fixed. Current
compilation/native qualification is still not run. R-G02 now stages its skin owner
and pool capacity before Execute and publishes actual accepted readers/completion
before fallible callbacks. Parent integration retained the local shared owner for
view history and clears no-skin slot validity; failure injection is still unrun.
R-S01/S02 host-progress source changes are integrated: optional effective mip is
clamped/fallback with original observed demand retained; completed chunk metadata
is released and unsafe in-flight work is never retargeted. No native qualification.
R-V01 full aggregate activation admission remains beyond the optional-fire policy above. R-V02/P03 Ground source
cutover now uses shared exact birth-time suppression facts (no draw object/vector)
for matching circle/fill/preview/line/expanding/charged layers. Legacy construction
skips only those accepted layers; one indexed retirement pass handles older pending
rows. Display-time output remains independent. Existing fixture sources compare
birth facts with actual Ground builder outputs and preserve fallback/accents.
P01/P02 Ground cold knot validation and one source-span resolution are source
integrated; synthetic adapters retain structural guard. P04 Mote/Flash append and
emitted-sequence membership remove reverse scans/temporary result vectors; charged
Ground stages two commands in std::array. Ground birth facts now exist and retain exact age0 eligibility. Full raw-event
activation/capacity admission across all profiles remains next. P12 preflight uses concrete shape/state APIs rather than
fake saved-token DTOs; full bind retains dynamic tokens/resources/lifetime checks.
Scoped R-V01 allocation boundaries are source fixed: Intern stages payload and
reserves row/mask/binding/profile storage, then real index insertion is its sole
potentially throwing publication step (no per-event temporary hash map). Native
freeze reserves both partial prefixes before sequence/identity. Persistent burst
keys/commands stage and reserve before tombstones/append, including zero-output
activation semantics. This is NOT whole-activation atomicity or a byte bound.
Ended status burst tombstones now query the already prepared current and
same-session previous source indexes, independent of typed quality/failure. Stable
episode IDs do not revive; same-session rewind explicitly resets these tombstones.
Reuse the existing semantic pair preparation result so identical publication pairs
do not repeat retirement, and no separate owner-union scan/sort/scratch is built.
Pair index validity publishes only after
successful preparation. Native frozen commands/receipt ownership are independent.
P04 Flash birth paths now append output and actual emitted-sequence facts directly;
the unpaired full-event copy and status/new-flash return vectors are gone. Both
outputs roll back to their original sizes on append exceptions. Ground persistent
and event outputs append to one retained scratch (checked returning wrappers stay),
and frame Ground/visible-Flash/surface-capture scratch keeps capacity across frames.
Prefix/rejection/order/capacity and pair-rebuild fixture sources were extended.
Safe-sector fanout now moves the original ring payload aside while cloning scalar
event facts, avoiding a whole gap-vector copy per sector, then restores the base
event. Source fixture covers all255 gaps/order/base payload preservation. Ground
helpers borrow the input ring within the call, reserve its exact output angle
count, move owning geometry into output and then into renderer frame staging.
Returned geometry remains owned independently of the original input.
Raw Flash/Mote, typed ballistic and legacy particle/line births now prepare in one
reusable DecodedVfxState scratch before native/live queue publication. Destination
growth completes for all channels before their nonthrowing append. Empty queues
accept the scratch backing by swap; no redundant payload copy/reservation. Failed
legacy per-event expansion rolls its stage prefix back; development failures stop
before publication, while shipping preserves other accepted outputs. Existing
pending compatibility cancellation remains after accepted birth publication.
ActiveEffectStore now has reusable prepared batch admission: deduplicated row
copies and transferable index nodes, geometric capacity growth and no-shrink bucket
reuse precede visible membership changes. Commit checks original owner and fixed
base shape, not each already-resolved key. EntitySurface/PointRate and
Ground/Light/Distortion/Decal/Volume/Screen/Ribbon now share one source-order request pass and one
prepared batch before birth publication. All profile storage prepares first; birth
destination reserves then publish, followed by adjacent nonallocating profile commit.
Their consumers retain their original order and new row indices remain opaque.
Ground context-owner replacement also stages in that batch: sorted owner keys
select each last replacement, filter old Ground membership in one pass and append
surviving incoming rows in original order. Other profiles retain their memberships.
Cancellation changes no membership. The temporary dedup index now uses retained
half-full row-index slots and the existing EventKeyHash, with no per-row temporary
hash nodes or key sort. Actual canonical index nodes still transfer into the live
store. Sequential replacement/dedup/order/seed/compaction fixture sources are added.
Persistent frame cursors/caches, aggregate capacity policy and full raw
acknowledgement closure remain open.
Ribbon production outputs now append persistent then event rows directly into one
retained frame vector, with rollback wrappers and no second event result vector.
Nested output descriptor ownership remains unchanged, so this is not a claim that all Ribbon
per-owner allocations were removed.
Light/Distortion/Decal/EntitySurface/Mesh/Fresnel also append into retained frame
vectors now; returning wrappers still serve raw callers/fixtures. Each append
restores its original output size on exceptions. One EntitySurface fixture was
added to the existing synthetic executable, not a new test executable. Ribbon
membership joins the same source-order prepared admission batch. The separate late
Ribbon and Ground event admission scans/individual store appends are removed.
Ground Direct Add/Soft eligibility is aggregated into the first prepared recipe
row at image preparation and carried through shared canonical/codegen fields.
Activation checks the live payload/owned interval without rescanning recipe ranges;
exact birth-time legacy suppression and display-time evaluation remain separate.
Raw ingress now uses a lazily allocated bounded ring with the unchanged4096-entry
pending+inflight limit. Prepare reserves the ready backing before removing events,
closing the old allocation-failure loss; both backings retain and report capacity.
Wraparound/session/ACK/move/reuse fixture sources were added. Prepared profile
payloads own their snapshots and may outlive or differ from the original request
inputs; only the target store must stay unchanged through adjacent publication.
Owner-binding row-ID capacity now reserves before binding/count publication.
Logical-capacity queries cover captured retreat owners, spark cursors and their
retained scratch. The mote birth hash cache is removed: its stored values were only
two deterministic Random(seed,lane) results, while sqrt/trig remained outside the
cache. Direct evaluation preserves those exact values without nodes, key hashing,
full-window pruning, session state or a capacity limit. Output uses the owner's
already interpolated elapsed tick/seconds; unused cache-only clock overloads are
removed. No invented lifetime was introduced.
This remains logical accounting, not an aggregate enforced budget or RSS.
Persistent spark output/cursor changes stage only touched rows in retained scratch,
reserve both live destinations geometrically and commit without allocation. A failed
output allocation can no longer consume a live spark ordinal. Old/staged cursor
lookups share one retained half-full slot index instead of repeated vector scans.
Logical slot size resets per Append while capacity stays reusable, preventing a
peak-size clear scan after population shrink. Expiry, first-match/source order,
skipped births and atomic output/cursor publication remain. Linear-oracle and
stateless mote/rewind fixtures are unexecuted. Full raw activation,
critical-warning/decoration admission and an aggregate retained+reserved-byte policy
are still open; neither the4096 ingress limit nor these queries closes that policy.
Safe-sector owner refresh computes only its indexed normalized gap angle, avoiding
G temporary ring vectors and G-squared angle work for G sectors. Base Ring payloads
reuse their angle backing after validation/resize; scalar math and interpolation
order remain. Source fixture compares255 gaps with the ring oracle and checks
wrapped offsets, count shrink, rejected indices and warm backing reuse.
R-V01 static content inventory found current authored maxima8 ballistic command
rows/event,3 impact flash rows/event and10 curl burst flash rows/event. These are
not total bounds: event/owner multiplicities are live, and persistent fire expands
all births since start with lifetime to the gameplay-owned end. Its CPU work and
208-byte GPU upload rows can grow with duration. Shader trace confirms the current
one-row-per-flash direct raster ABI; a compact owner/ordinal shader would need an
explicit variant/mapping and arithmetic parity work, so it is not adopted without
CPU/upload/GPU cost and quality evidence. Do not invent a short flame lifetime or
infer a whole-state byte budget from the authored burst maxima.
P09 follow-up review found no additional immutable work to move: remaining
per-frame SoftwareGI records bind current page residency/generation, LOD, skin,
wind and graph resources. Retain those checks rather than cache stale bindings.
Captured retreat movement now stages new episode owners and caller-visible output,
reserves both geometric destinations, then commits owners only after output succeeds.
Output exceptions restore the original prefix and leave new owners unpublished;
expiry follows existing source/display age. Clear/rebind/swap cover scratch too.
This is a local state transaction, not the full activation/budget closure.
Parent also corrected Ribbon output codegen aggregate order to match the real
header (profile follows shape fields) and added real-header compiled roundtrip
value checks for new recipe families. They are source-only/unrun.
Current source-only diff --check passes. NO new compile/test/cook/native/FPS run.
Next order remains review R-V01 whole activation and aggregate capacity policy,
remaining static admission cleanup (all named Ribbon families now source prepared,
raw source/output/texture mirrors are prep-only and not shipped; persistent Ground
entity contracts now filter at admission), then executable qualification/remaining
justified review work before earlier directives. Ground P03 source is now connected;
do not substitute display-time emission for its exact birth facts. One per-event
prepared activation still must reserve before every profile/native state commit.
Pending/frozen/episode bytes need one coherent critical-warning/decoration policy,
not unrelated per-vector caps or simulation backpressure. Do not treat this batch
as achieved. UI facts follow actual draw predicates, including the Paused Root
HUD and the HUD behind selection/end overlays. Hidden graphics model updates
no longer invalidate non-settings draw reuse, while receipt metadata stays fresh.
Source-only checks pass; no current executable result exists.

- T05 canonical environment material product: render/material/<name>/material.rpkg;
  terrain/ground_detail retain their existing special IDs. Geometry LOD products
  own geometry + checked material reference + LOD0 hierarchy reference; no repeated
  metadata/program/bindings/parameter/surface chunks. RenderPackage recipe15,
  RenderScenePackage recipe3 and HLOD/material identity consumers are updated.
  Release/Dev typed descriptors remain the single material source. Renderer caches
  only per-material variant facts; per-mesh RockMaterial copies, unused handles and
  duplicate terrain loading are removed. Selector bounds remain per geometry.
  Old LOD0 geometry is no longer classified as a material by the linker.
  Candidate variant data swaps atomically with meshes. Fixture source updated;
  compilation/cook/runtime compatibility pending the deferred batch. Environment
  package cache hits now precede texture/mesh source expansion; one Action snapshot
  per namespace supplies identities. Actual upstream/cached bytes still pass size/
  digest checks; certified cached hierarchy pages skip repeated geometry decode.
- T14 packet owner prepares canonical instances, pose indexes, retained surfaces,
  preview/identity, union-consumer monster LOD and stable draw order once across
  views. Actual GI voxel and shadow consumers include every view. Scratch capacity
  is reused; borrowed pose pointers/order validity clear on every Render exit.
  Owned VFX replacement uses one ID/type index and stable erase instead of3 nested
  scans; independent legacy-equivalence fixture source added. Dead default-only
  EnvironmentLod array removed. Current stable-slot reconcile/GpuInstance/canonical
  conversion now run once per packet. Per-view GpuSceneSkinEntry offsets/strides
  now live in waited frame-upload scratch; later views retain the common table
  without a skin mutation transaction or full current-byte comparison.
  Entity-surface bytes copy on first accepted graph only. Submitted packet skin
  owner/range directory now defines current deformation across views; per-view
  skin identity serialization/equality is removed and readers merge completion.
  Owner publication follows successful graph submission, preventing reuse of
  uninitialized pre-submit data. Range/slot/geometry validation remains. Focused
  source fixtures cover abort/retry, stale generation, current sentinels and
  reordered previous ranges. Full previous CPU GpuInstance copies/rebase/uploads
  are now removed in source. Submitted pooled current GPU scene owners plus small
  generation/mesh/motion-epoch facts supply per-view previous transforms directly.
  The singleton mutable current table and extra current-to-history GPU copy pass
  are removed. Each backing uploads active channels changed since its accepted
  payload revision; header/entity-surface publication belongs to that backing.
  Prospective channel stamps commit only after actual native acceptance and abort
  cleanly. Pool reuse needs completed readers, no logical leases and same device.
  Retained frontend identity uses topology/desc/alias pattern, not snapshot native
  addresses; imports/root SRVs rebind each frame. Unknown native issue retains
  max completion and faults before reuse; pre-submit failures remain retryable.
  Direct resource shapes are now collected once per candidate scan into retained
  scratch. Flat sorted alias occurrences replace per-resource hash-node allocation;
  first-occurrence labels preserve equality patterns across native rotation.
  Cold plans refresh shapes after provider preparation. Native certification and
  final binding checks remain; focused alias fixtures are source-only/unrun.
  GBuffer visibility resolve declares ComputeRead; raster/transparent/Hero use
  vertex reads. Current-only shadow/extraction keeps no new history read.
  Old-coordinate skin/static/wind use the original view projection; pixel motion
  uses one perspective-correct unjittered clip varying, adding only one float.
  Current UV comes from the audited active-extent SV_Position, including compute
  resolve/VRS; motion projection matrix products are removed from each fragment
  in both GBuffer paths and Hero guide. Finite motion/depth fallback remains.
  Archer legacy VS consumer closure now covers raster/mesh/visibility/Hero/
  Fresnel/shadow/GI and static-only probe/fixture boundaries. Its unused128-bone
  compatibility array, initialization/copy and SkinArcher fallback are removed.
  FrameConstants is4720 bytes (was12912); all mirrored prefixes and exact offset
  assertions are updated. Exposure/instance upload offsets derive from the aligned
  constant size. Actual compute palettes/sampling remain unchanged. Monster's
  legacy vertex skin/bubble fallback and its unused raster palette descriptor
  are removed too. Qualified bounds without allocated skin now mean no packet
  consumer: visibility emits neither visible nor HZB-rejected refs, still writing
  zero indirect arguments; shadow preparation omits those receivers/casters and
  preserves prior-caster retirement. Required/unknown-bound/attached/light/GI
  consumers retain skin admission and explicit failure, with no quality downgrade.
  Packet-owned instance lookup now serves Fresnel; sorted attachment IDs replace
  per-instance full effect scans and preserve duplicate-ID demand for every draw.
  The native owned-view fixture now skips/reappears Preview, rebases the whole
  packet by1e9, and checks old-basis matrices/cut/epoch plus actual motion readback.
  It remains opt-in/unbuilt/unrun; slot-reuse/native stress and full quality/FPS
  qualification are not proved. Root19 is a two-SRV table (history + view skin),
  keeping root indices and the 64-DWORD budget. Four dedicated descriptors per
  waited bank retain separate scene/ribbon variants; no pending table overwrite.
  Generic/raster, visibility, software geometry and Hero use view-local skin;
  explicit static-table loads remain unchanged. Vertex fallback source closure is
  complete; native consumer/asset qualification and conditional exact-pose sharing remain.
  T15 direct OwnedView source cutover now covers
  initialization, frame/descriptor/lifecycle/capture/reload. Impl mirrors and
  ExchangeView are removed. Actual accepted submissions choose the three global
  FrameContexts independently of output index; RTV/DSV/SRV banks wait per slot.
  View selection switches only retained per-viewport SDK state; real reconfigure/
  reset/BeforeReuse waits remain. Unbuilt; do not claim native qualification.
  Parent integration fixed UI texture initialization ownership and shared-slot
  upload growth/recopy. Existing owned-view fixture now switches different-sized
  auxiliary owners through a slot. Integration now also routes feature preparation/
  live admission, main lighting status and HLOD/geometry motion history to actual
  owners;26 leftover Impl feature/history mirrors are deleted. Shared services and
  completed-timing fallback remain. Hero retirement consumes samples, checks/reset
  native service, then folds counters once; status/drains cover retained owners too.
  Capture now resolves actual submission slot/id, not the independent output
  index. A separate constants stamp invalidates reused/pre-submit CPU scratch;
  geometry/visibility reject stale slots and surface diagnostics omit stale CPU
  metadata while retaining per-view GPU surfaces. Old query timing IDs/storage
  are untouched. Global timing endpoint map remains keyed by view ID; W12 stays
  deferred. No current-source native qualification has run.
- T06 prior exact channel/digest/fixed-target/no-read shrink changes are retained.
  Mapping-only shrink skips empty-payload decode and frees consumed ranges.
  New software replacements use fixed-target authenticated mip/slice chunks into
  an unpublished candidate, complete every copy before final publication, and keep
  old sampled resource until then. Prepared metadata survives intermediate chunks;
  payload does not. Cancel/stale paths release only terminal owners and preserve
  invalid-transition errors. Range-less sources retain complete package path;
  indivisible ranges exceeding fresh host cap fail explicitly. Chunk fixture source
  updated; native cancellation/accounting/shrink qualification is deferred.
  N7 no-I/O reserved coarsening now enters an explicit mapping-only ledger
  transition; ordinary payload upload still requires Decoded. Native generation/
  unmap evidence, submission and completion-gated publication are preserved.
  New fixture sources are unexecuted.
- T04 Dev admission distinguishes source revision identities from runtime bulk IDs;
  environment mesh/DDS dependencies retain digest/expected source identity but no
  source path read. Packed source revision table reused; generation-only explicit
  product-map path remains compatible. Unknown edges fail; VFX atlas remains bytes.
  No claimed Release payload savings: linker already excluded non-render sources.
- A5 source now separates shipping content image identity from producer/cache
  provenance. Core CanonicalBytes writes explicit LE scalar bits and u64 lengths;
  Simulation reuses the public typed field inventory, Presentation encodes bounded
  strings/camera values, Audio emits canonical release metadata, and VFX encodes
  its actual tables and prepared recipes. Renderer descriptors include defaults,
  skeleton/clip/binding/ranges and revision tables. Sorted asset layout plus actual
  payload digests/offsets/extent enter project-hs-shipping-image version1; generated
  C++ and producer executable bytes do not. ActionKey is unchanged; separate
  content.receipt.json records action/producer provenance. Wire header format2 and
  exact64byte pair matching/atomic replacement remain. No header-only pack reuse.
  Existing foundation/simulation/presentation/audio/VFX/package fixtures cover the
  encoders in source; no extra executable. Actual linker provenance/format-only
  invariance, mixed/truncated pair and Release repack qualification remain deferred.
- T12 successful typed pickup/status flash receipts suppress new legacy emitter
  generation. Prior pending rows retire under original rules before expansion;
  Line outputs and later authored ballistic rows remain. New ballistic recipes
  prepare immutable direct/persistent shape/parameter/LUT contracts once in decoded
  VfxImage; generated const arrays share that representation. Builders consume
  prepared rows with original seeds, basis, live input/time/quality. application,
  Dev/shipping owners, codegen and existing fixture sources migrated; unbuilt.
  Distortion/decal now also prepare immutable recipes (knots, shape, parameters,
  texture paths/slices and quality constraints) once in image IO and generated
  const arrays. Builders bind only live payload/time/quality/seed; accepted range
  presence drives retention, preserving legacy fallback. Distortion gap anchors
  borrow live input only until output construction, avoiding a transient vector
  copy; hot payload hashes are forced compile-time constants. Focused/runtime/
  codegen fixture source migrated; no Program-based production caller remains.
  Immutable effect output masks prevent non-ribbon retention. Generated/static
  and developer images share prepared masks. Common Ground Ring/Fill/Border/Hatch/
  Preview/Ticks and typed event Flash now use prepared rows, including authored
  timing/color/quality/mask facts; all Ground/Flash callers use the image-only API.
  Ground selects only its sorted source range rather than rescanning all effect
  recipes per source. Existing malformed/geometry/codegen fixtures migrated, and a
  sparse source-order case added. Specialized event and persistent Ground outputs
  now prepare expansion, Hex/quality masks, Boss signature, CrossRing/Trap/Axial,
  status/Polar/Crown, Chevron, SafeSector and ChargedPulse contracts too; live
  geometry/time/seed/quality and preview curve integration remain per frame.
  Persistent status stamps, fire tongues and owned status-tick Flash now use cold
  image recipes too, emitted as immutable shipping arrays. Runtime/release views
  and all three application callers are connected. Live owner/episode, geometry,
  age/quality/time and deterministic8Hz fire births remain; one mask choice now
  serves all births of an owner/source. Recipe/codegen fixtures are source-only.
  Mote source/output parameters, quality-specific masks, size/curve and Spark
  texture facts now prepare once too, with matching shipping arrays and all six
  Runtime callers connected. Spark Rebind stays on the cold Program view; live
  births/cursors/geometry/quality/owner behavior remains. Redundant copied index
  tags/lookup equality checks are removed; authoritative array bounds remain.
  Mote source/output route and window facts now prepare once too: hot common,
  residual, owned rate/status tick, persistent rate and Spark Emit avoid authored
  source/output/parameter reads and effect lookup scans. Copied identity/order,
  original source knots, seed and live birth/quality/owner behavior stay. Cold
  Spark Rebind retains parameter decode; immutable effect-record gates remain.
  Mote schema/codegen/identity and focused malformed/non-ambient-sector fixture
  sources are connected. Screen hit/low-health and Volume phase/spawn/death/aura
  now prepare immutable effect/source/output/parameter/texture contracts too.
  Actual decode rejects unsupported known Screen/Volume contracts before image
  publication; unrelated families remain independent. Runtime routing/expiry and
  builders consume prepared rows; phase/owner/quality/time/seed remain live.
  Render-thread Screen/Volume buffers retain their capacity on successful frames;
  failure still clears output and preserves the prior pulse. Codegen/identity and
  existing frame/codegen fixture sources cover all new fields, unbuilt/unrun.
  Frame adapter/movement/ground-owner rebinding now consume an immutable effect-ID
  index prepared once and emitted with shipping metadata; authored handle order is
  unchanged. Literal hashes are consteval in that adapter even under /Od. One
  render-thread input owner reuses outer vector capacity, and retained retreat
  movement appends directly without a temporary vector/copy. Existing fixture
  sources cover index misses, handle order, cleared output/capacity and append
  preservation; no executable qualification has run.
  Ribbon projectile/link/pickup/fixed-event/cancel routes and EntitySurface now
  consume prepared effect/source/output/texture facts. Fresnel and direct-owner
  Mesh use concrete ordered accepted recipes; malformed rows skip independently.
  Shipping codegen/canonical identity/caller/fixture sources are connected. Some
  immutable Ribbon route gates, adapter delivery radius and legacy Ground route
  selection remain; T12 is not fully closed.
  Ballistic activation now builds into the existing pending owner before legacy
  expansion, skips only successfully replaced Mesh emitters, preserves ShockShell
  except explosive main, and restores legacy-before-typed order by rotating the
  newly appended range. Pending legacy retirement is one indexed pass rather than
  one full scan per input. No extra command buffer or sequence/seed renumbering.
  Narrow legacy replacement fixture source covers retained accents/order/seeds;
  current execution/performance qualification remains deferred.
  Status/pickup replacement keeps retained sequence scratch and sorted membership;
  event filtering/pending-command retirement use binary search instead of nested
  linear scans. ExpandEvent receives the per-event replacement fact directly;
  line generation and authored-ballistic retirement meaning stay unchanged.
- T13 settings-suspended guard now precedes VFX activation/state mutation while
  retaining bounded ingress/recovery. Pending particle due order sorts only newly
  appended suffix and merges only on overlap, preserving original birth, ties and
  accepted-prefix retirement. Raw ingress now follows proven producer FIFO order,
  and CPU activation retires before Render independently from GPU emission ACK.
  Profiles borrow canonical indexed rows; four owner-attached outputs share sparse
  frame bindings while original birth/capture payloads remain unchanged. Membership
  masks replace per-profile duplicate scans; Materialize/Commit and nine decoded
  scratch vectors are removed. One zero-profile row count replaces dirty/capture
  flags: additions with live profiles skip full compaction, capture-only rows stay
  eligible for retirement, and explicit key changes still rebuild the index. Expiry
  remaps existing index entries instead of clearing/rebuilding the whole map.
  Focused fixture sources cover binding sharing/drop/reset, captures, sectors,
  stable compaction, rebind and delayed A/B activation. Unbuilt/unexecuted.
  Frozen native batch storage/identity is now source implemented with retained
  channel capacity, exact submission ACK, stable rewind retry and reset handling.
  Public optional identity at the end of RenderVfxFrameInput is now wired into
  Runtime and renderer. Zero sequence is explicit held clock/no actual batch;
  nullopt preserves legacy callers. Integer display floor selects due prefixes,
  collision age/delta/AuthoredParticleClock and last committed tick. Ordinary
  analytic native ages use display time. Only the first view resets/emits the
  shared world. Exact session/target/sequence submission ACK precedes handling
  later view/Present error. Catalog adoption discards old expanded batches and
  requests particle reset; raw CPU activation and UI receipts remain separate.
  Native and state fixture sources cover different snapshot/target, no-step and
  bad session, plus frozen retries/rewind/capacity reuse. All are unexecuted.
  Entirely due native channels now transfer vector ownership into the frozen
  batch without copying rows. Mixed/future channels keep the due-prefix path;
  both partial buffers reserve before transfer. Exact identity/ACK and unchanged
  retry birth position/seed are covered by extended unrun source fixtures.
  Birth-space source audit distinguishes owner-bound persistent Motes (existing
  fixture explicitly follows current owner), first-world-position Sparks retained
  in decoded.flashes, and frozen native particle/line commands. No extra world-birth
  cache is adopted for those paths; their actual retry qualification remains.
  Full pending/fanout byte admission and wider output coverage require follow-through. Audit
  confirms10k particle/32 distortion/64 decal/32 fresnel limits do not bound Ground/
  Flash/mesh upload or episode/pending bytes. Do not silently treat10k as a shared
  budget. Avoid a second eligibility scan; finish immutable recipes before a
  single prepared activation with concrete per-output admission.
  Sparse binding rows also retain nested RingGaps vector storage between frames.
  PreparedPairFacts now keys ordered identities by session/publication revisions
  and counts, never span addresses or unrelated quality/pipeline/static pointers.
  Resolved values always come from the currently supplied borrowed spans.
- T17/T19 current-only CSM/VSM/local-shadow callbacks bind current deformation as
  their unused history fallback and no longer declare previous skin reads. Actual
  motion consumers keep history. Empty SourceRenderOutputReady omitted when no
  Full timing or SDK consumer; reference DRS span and SDK tagging completion kept.
  Final topology is in the early graph key. W12 query ownership is untouched.
  N6 layout publication matches exact EarlyKey plus low-level plan handle before
  unused/LRU selection; frame-bank variants no longer overwrite one another just
  because they share a plan. RC1/N5 now derive effective DRS from the preserved
  request and resolved SoftwareGI constraints. Controller, actual reporting,
  provider/capacity preparation and reference timing/key/topology share that value.
  Device startup recomputes after actual capability admission. Resolver fixtures
  are source-only; warm-key/native mode transitions remain unqualified.
- T16 authored/masked facts cache with StaticGpuScene. Capture CSV retains all
  fields/records with reused strings/to_chars. Do not weaken actual state/fences.
- Latest9f91820 reanalysis supplied in chat is recorded in queued-51d5055.md.
  N8 source split graph value/post topology declarations from native lowering;
  the existing portable test still calls the production topology compiler.
  N9 retains producer automatic repair; fixture now asserts exact repaired bytes,
  quarantined complete old set and unchanged independently pinned reader data.
  No current-source portable/native qualification has run for these changes.
- S2 projectile body/head/trail/outer share current/previous terrain samples once
  per projectile. Link targets/return-start retain distinct samples, and low-arena
  no-op preserves position bits. Existing meadow fixture source extended; unrun.
- A3 material shader production now shares the existing runtime shader source
  snapshot helper: authored source/includes are copied and hashed on cache miss;
  DXC runs in that private snapshot with relative source/include paths, plus owned
  generated includes. Missing transitive inputs fail rather than falling back to
  originals. Five omitted common shader includes are now action inputs. Environment
  producers freeze metadata before deriving/copying consumed inputs and rekey before
  cooking; valid-hit roles skip payload copies in mixed batches. Snapshot/digest
  fixture sources extended; concurrent edits/native cooking are not yet qualified.
  All five material/shader cache paths now parse one ActionResultSnapshot and use
  its verified owned product bytes, avoiding reopen/digest TOCTOU and repeated
  manifest parsing. Runtime shader families hash their saved transitive input
  closure first; valid hits return before stage creation or any DXC scan. Changed/
  unknown inputs fall back to live dependency discovery and frozen source. Narrow
  InputMetadata accessor retains existing receipt touch behavior; no raw JSON
  mirror, fake DXC harness or extra native test target. Actual no-DXC hit and
  include-change behavior still need the combined producer execution batch.
- A4 material compilation and runtime compile/dependency scan share bounded DXC
  exit handling. Timeout/failed wait requests termination then waits at most10s;
  only confirmed exit permits stage cleanup/diagnostic reads. Unconfirmed exit
  preserves stage and reports PID/path/wait errors; infinite waits are removed.
  Native timeout/uncertain-exit qualification remains deferred.
- RC2 waited descriptor banks retain the actual resource owner, resolved fallback
  and slice offset/size, skipping unchanged geometry SRV writes. Resident publication
  transfers the prepared bank snapshot into the active heap; shutdown clears owners.
  Existing native bank-cycling/reload coverage remains unexecuted for this source.
  No brittle source-string fixture or extra descriptor validation framework added.
- T21 essential startup now has a scoped per-segment allowance from adapter
  physical capacity. Successful D3D12MA allocation sizes consume it; failed or
  oversize attempts do not. Unknown capacity is explicit failure, not unlimited.
  Static limits never substitute for dynamic OS budgets or optional admission.
  UMA routing and standard-vs-CUSTOM pool routing share the actual allocation
  resolver with resident candidate admission; native failures/completion remain.
  The scope ends on Initialize return/abort, and teardown clears policy state.
  Pure boundary/segment fixtures are source-only; native startup remains unrun.
- S4/T01 conflict is reconciled in source with independent capture_hit_rates,
  default true for public callers. Detailed=false alone preserves fixed hit-rate
  aggregates; ordinary Runtime explicitly disables both as before. Recording,
  diagnostics, smoke and replay explicitly retain both. Record/seen/cause/retire
  and reserve/reset follow the hit-rate flag together; combat state is unchanged.
  Observation exposes hit_rates_captured; CombatSim/playtest exporters emit null
  for unavailable counts/ratios. Disabled synergy detail is not reported as zero
  observed activations. Extended existing fixtures cover mode parity/checksums,
  restart and exported null-vs-zero semantics in source only. Existing ordinary
  opt-out policy is retained without a new UI/CLI setting or a claimed cost win;
  current-input executable parity and measured telemetry cost remain unqualified.

## Latest completed executable checkpoint (before new T05/T14 source)

Artifacts: Build/optimization-20261027-continuation/.
Central Debug continuation build exited0; matching native scene-probe cook publishes
4probes in8frames/~9.4s. Architecture/streaming2/2 and updated real-hardware probe/
multi-queue GBV fixture pass (probe-hardware-fixture-final.log). T23 readiness,
transient sampled-mip identity and stale reset are covered. BakeStatus uses fixed
bake_completion; export/reset retain rolling all-use completion. Later readers
cannot extend the producer's completion target indefinitely. Ordinary SampleReady
performs no new full-scene material/texture scan/hash. Permanent GPU-admission
rejection still has no proof API; no arbitrary timeout is introduced.

Old source-batch FNV7bc2f747c4cf4993 reproducibly DEVICE_HUNG0x887A0006 at49-63frames
in validationOFF200, including timingOFF and Legacy. Partial VolumetricDensity
breadcrumbs do not prove a root cause. Subsequent T23 binary candidate and original
binding control both exited0 for1800ticks:422frames and826frames respectively.
Original CBV descriptor flag is restored; extra volatile flag had no proven benefit.
Do not assert the historical native hang's sole root cause or long-term resolution.
Candidate FNV94080dd25a8387c6; original-flag control FNVcac5b8d72cb99d7c; generation
ee658631a1bf3d9490e6044eaeb179f0fb78e6b02959de73b8264a1907dc0357.
Candidate392qualified foreground intervals:30.13FPS,1%low22.14,p99=43.20ms.
Components: capture log0.778ms, upload plan0.069ms, GBuffer6.84ms/AO3.45ms,
frame-slot wait9.19ms/graph build4.07ms. These overlap and do not prove whole-frame
improvement. OS GPU sampling is partial; game/per-pass capture is complete.
Original-flag control results are preserved but not yet analyzed. Goal60FPS unmet.

Other earlier narrow receipts remain valid for their original source/inputs only:
architecture/persistent scene byte oracle, package formats, native DS1.3 I/O,
Debug selected5/5, smoke180+resize/VFX showcase0validation errors. Full115 was not
repeated. Temporary Debug HS_BUILD_TESTS=ON remains for the eventual batch;
Debug/validation DirectStorage=OFF. Restore temporary test cache after that batch.
MSVC /Zi PDB needs host access; preserve /Od/assertions/checked iterators.

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
  prepares one union-consumer LOD/order; per-view motion history remains separate.
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
  and staging wait on their actual completions. Authored/masked material facts are
  also cached on the StaticGpuScene owner.
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
