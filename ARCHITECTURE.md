# Project HS Architecture

This document describes the module graph enforced by the current build and `Tests/check_architecture.cmake`. It is not a future-system roadmap.

## Module DAG

```text
                         Core
              ┌──────────┼───────────────┐
              ▼          ▼               ▼
            Jobs     GameDomain       RenderData
                         │                │
                         │                ▼
                         │           RendererD3D12
                 ┌───────┴────────┐
                 ▼                ▼
             GameRules       Presentation
                 │
                 ▼
              Gameplay
```

RenderData's native-free value target also feeds Presentation. Its package codecs
remain a separate conditional target. Runtime composes Core, Jobs, Gameplay,
Presentation, and RendererD3D12. Apps are executable entry points and composition targets.

CombatSim consumes Gameplay's scoped public tick view for targeting. Its combat and
progression loops do not depend on Presentation or construct render snapshots.

TaskSystem creates its move-only TaskGroups. Submission, parallel ranges and waits
act through the group and its creator; callers cannot submit a group to a foreign
system. Each operation pins its shared work state across reentrant cancellation
callbacks. The system outlives its groups, and accepted work is joined before a
group is destroyed. Task labels are not retained or passed to the backend;
failure publication keeps the first error and its fixed-size message.

| Module | Responsibility | Allowed Project HS dependencies |
|---|---|---|
| Core | Foundation types and shared low-level contracts | none |
| AssetManifest | Selected Generation manifest parsing and product path lookup | Core |
| RenderData | Native-free render values; separate versioned package/codec target | Core |
| Jobs | Task-system integration | Core |
| GameDomain | Stable domain/read-model contracts | Core |
| GameRules | Rules independent of mutable simulation state | Core, GameDomain |
| Gameplay | Deterministic mutable simulation | Core, GameDomain, GameRules |
| Presentation | Domain/read-model projection and concrete snapshot exchange | Core, GameDomain, RenderData values |
| RendererD3D12 | Concrete Windows/D3D12 backend | Core, RenderData |
| Runtime | Concrete subsystem composition and orchestration | Core, Jobs, Gameplay, Presentation, AssetManifest, RenderData, RendererD3D12 |
| Apps | Argument handling and entry-point composition | approved public libraries |

## Forbidden dependencies

- Core must not depend on any higher Project HS module.
- Jobs must not depend on game, presentation, renderer, or runtime modules.
- GameDomain must not depend on Jobs or any higher game/runtime module.
- GameRules must not depend on Jobs, Gameplay, Presentation, RendererD3D12, or Runtime.
- Gameplay must not depend on Jobs, Presentation, RendererD3D12, or Runtime.
- Presentation must not depend on Jobs, GameRules, Gameplay, RendererD3D12, or Runtime.
- AssetManifest must not depend on Jobs, game, presentation, RenderData, renderer, or runtime modules.
- RenderData must not depend on Jobs, game, presentation, renderer, or runtime modules.
- The unconditional `hs_render_data_values` target depends only on Core. It does
  not pull package codecs, content tools, DXC or native renderer prerequisites into
  low-level builds. Presentation links this value target, not `hs_render_data`.
- `hs_runtime_contracts` is a header usage-requirements target for native-free
  Runtime public values/contracts. It exports Runtime/Public and renderer public
  value headers and depends on Core/GameDomain/RenderData values; it neither builds
  nor links the native Runtime/renderer backend, IO, DXC or content products.
  Runtime, content consumers and low-level VFX fixtures inherit it directly.
- RendererD3D12 must not depend on Jobs, game, presentation, or runtime modules.
- Production modules must not include another module's `Private` headers.

Moving a higher-layer type into Core only to bypass these rules is also a dependency violation. Keep types at the lowest layer that owns their meaning.

## Deterministic simulation boundary

Gameplay owns fixed-step state transitions, movement, combat, collision, statuses, progression, cleanup, and deterministic checksums.

Gameplay must not contain D3D12/DXGI, renderer, presentation, UI/window, audio, filesystem/runtime orchestration, or wall-clock-driven game decisions.

Structural changes must preserve phase order, fixed-step behavior, seeds, `/fp:strict`, deterministic iteration, checksums, and public read-model semantics. `Source/Gameplay/Private/simulation_pipeline.hpp` is the authoritative phase order.

Enemy and projectile hot/cold groups are the single authoritative actor storage.
Private status, attribution and history remain in Gameplay. GameDomain's scoped
TickReadView borrows those owners through semantic accessors, immutable spans and
cached small facts; it owns no actor/history mirror. Mutation is rejected while a
scope is active. Presentation consumes the view synchronously and owns the render
packet it publishes across threads. Full read-model copies are development exports.
The `hs_gameplay_development` leaf target owns the explicit copy function and is
linked by tests; ordinary Gameplay and Runtime do not contain the export body.

Checksum capture is optional observability, not a gameplay decision. Existing
simulation callers default to capture enabled; ordinary Runtime play disables
it while smoke, recording and replay retain capture. Disabled TickResult and
read-model checksums are zero. Explicit `ComputeChecksum()` still computes the
same canonical state hash on demand without enabling automatic capture.

Detailed balance capture controls optional upgrade/relic synergy detail.
Independent session-start hit-rate capture controls only unique skill/enemy hit
membership and its retirement. Public callers retain capture by default; ordinary
Runtime explicitly disables both while recording/diagnostic sessions retain them.
Off observations mark hit rates unavailable, and JSON exports use null for those
counts/ratios while damage, uses, hit events and required summary values remain.

## Presentation and rendering

Presentation projects stable GameDomain contracts. It does not execute rules or reach into Gameplay implementation state.

RenderData owns RenderInstance, immutable scene/snapshot values, RenderSnapshotPair,
VFX frame inputs and RenderFrameInput. Presentation owns RenderSnapshotExchange;
Core retains the generic SPSC queue. Renderer consumes the standalone pair value,
so it has no dependency on Presentation's exchange or producer lifecycle.
Raw presentation events end at Runtime's VFX/audio translation. The renderer
frame contains prepared VFX inputs; each view is rendered inside that explicit
package, with no snapshot-to-package compatibility recursion.
Snapshot values/helpers live in render_snapshot_values.hpp; mutable scene/snapshot
builders live in render_snapshot_storage.hpp. The legacy render_snapshot.hpp
includes both. Value consumers include values directly so builder changes do not
invalidate renderer compilation. Builder users include storage explicitly.

Presentation prepares an immutable ProcessSceneLayout for the actual launch seed,
generation and environment geometry. Runtime retains that owner across session
restarts with the same layout. Render packets share its read-only Core
StaticRenderScene view; mutable scene storage is frozen before publication.
Snapshot read pairs expose the publication sequence of each retained slot;
tick/session values alone are not a publication identity.
The exchange issues a move-only write lease with a private slot index. Publishing
or abandoning acts on its originating exchange; an unfinished lease abandons on
destruction. Callers cannot forge a slot or submit it to a different exchange.
Runtime's explicit retained writes reuse a slot's dynamic arrays only for a known
immutable (session, actual tick, dynamic revision) sample. Header/camera/scene/UI
still refresh on publication, and previous/current remain independently owned.
Raw writes clear storage and raw copies always copy. Mutation and copy/move failure
boundaries invalidate reuse; no hash or pointer identity validates the arrays.
Animation updates share the existing actual-dynamic invalidation used by projection.
Renderer prepares HLOD member mappings and proxy bindings once per retained scene
and refreshes only residency-dependent fields each frame.

The optional Brixelizer integration keeps FidelityFX headers private. Its build
producer links the full pinned SDK shader tables and emits immutable binding
metadata. Runtime links that metadata and reads DXIL from selected content;
The SDK license uses the source namespace, outside GPU resources; existing Action
receipts own producer provenance without a duplicate content product.

Presentation's immutable UiFrame owns both draw models and their displayed
interaction facts. The exchange retains that owner with the current packet;
Renderer receives its native-free draw span and the existing publication identity.
Runtime accepts only a matching successful Present receipt, retaining the same UI
owner plus the actual output extent for click admission. UI reuse compares concrete
world/UI/settings dependencies before generating models; it does not compare draws.
Simulation GPU submission history and accepted UI Present history are independent.

PresentationCatalog and SimulationRules are typed tables. Their developer file
decoders are the separate hs_presentation_io and hs_game_rules_io libraries.
Release Runtime binds the content-linker's compiled const tables; developer
Runtime retains the selected-Generation loaders. Simulation's canonical byte
writer and C++ emitter share one typed member inventory, preserving ABI padding
normalization and the existing independent gameplay hash.
The Release ProjectHS entry point links the compiled shipping content;
the experiment host is available only in development configurations.
The offline static-shadow tool uses a one-shot bake entry that owns a private
capture bootstrap and teardown. It creates the native device, shared root ABI,
geometry/material/texture inputs and selected capture PSOs, without publishing a
renderable view or creating a window, swapchain, post-processing, UI or font.
It retains the admitted Generation/package dependencies until the capture data
loader itself can consume a narrower set; dropping an unused color PSO alone
does not change package dependency ownership.
The base Runtime target does not force that image's
full cook into unrelated consumers. CPU runtime integration tests use their
selected Generation; a separate shipping test target owns compiled-table/image
and packed-font checks. Both remain part of full verification.
Single-config Release can build with HS_BUILD_CONTENT_TOOLS=OFF using an existing
Shipping/content.generated.cpp and Content/content.hsp in the build directory.
This path has no content-producer or Generation-verification dependency. Missing
files fail configuration; Runtime still verifies that their identities match.
DXC, ImGui and a loose font binary are not required for this runtime-only build;
native renderer dependencies and the font license remain required.
The linker atomically replaces Content/content.hsp before updating its generated
compile input. These two files are not a transaction: Runtime rejects an interrupted
pair by matching the compiled 64-byte header and file extent. An already opened
owner retains its original file. Core's move-only ShippedContentOwner owns the
read handle and bounded offset reads, optionally checking a supplied SHA256 during
that same read; it has no catalog, format, generation or renderer knowledge. This cutover
currently covers PresentationCatalog, SimulationRules, AudioImage and VfxImage. AudioContent
validates WAV inputs once and emits typed cue/sample rows plus headerless PCM;
the linker incorporates these products without decoding the audio domain again.
The linker's content image ID hashes a versioned value manifest, ordered typed
tables and actual pack ranges/payload digests. Canonical scalars are little endian
u32 (including narrow integers/bools) or u64, floats retain their IEEE bits, and
strings/sequences have u64 lengths. Object padding, generated C++ formatting and
producer executables are excluded. ActionKey still includes producer identity;
Shipping/content.receipt.json records that provenance separately. HSCONT01 format2,
13-table header layout and exact executable/pack matching remain unchanged.
Regenerate the pair together when adopting the new content ID algorithm; existing
runtime-only prebuilt configurations still require their exact matched pair.
Release audio borrows generated cue rows and owns one preload PCM arena. Generated
chunk digests cover contiguous preload runs and independent transition tracks;
only successful verified reads publish ready PCM. Only
transition tracks use asynchronous preparation and completion-safe eviction.
Developer JSON/WAV loading is isolated in hs_runtime_audio_io and excluded from
Release Runtime. VFX uses the existing record types through VfxProgramView and
VfxImage spans; generated arrays own shipping metadata for the process. Developer
VfxImageData owns mutable vectors and the shared decoder/static-policy preparation
in hs_vfx_image_io, backed by the CPU-only hs_vfx_program_io format reader.
Developer Catalog file loading is hs_runtime_vfx_io. These decoders are absent
from the Release Runtime link, and IO libraries build only when consumed.
Runtime retains each adapted VFX event payload once in a stable-order
ActiveEffectStore; profile lists and entity-surface captures hold private row
references. Profile consumers borrow indexed views; attached ground/light/decal/
distortion outputs share one sparse current-owner binding without overwriting
birth/capture inputs. Membership masks avoid duplicate scans; a count of rows
without profile references skips compaction when only live rows were added.
Activation plans unique keys and membership order before materializing new payloads
or index nodes. New-row staging reserves by unique keys; first-request indices
preserve canonical first-input semantics without retaining caller pointers.
Capture-only rows remain eligible for retirement, and expiry remaps the existing
episode index incrementally.
Row-local expiry shares owner-binding compaction in Runtime's Ground/Light/
Distortion/Decal paths; the cached canonical owner is still refreshed once per frame.
Gameplay's uniform ring gaps travel as count/offset plus the original event/owner
angle formula through Runtime profiles and typed ground/distortion values. Only
the native upload expands them into the existing flat angle buffer; shaders and
cooked formats retain their ABI. Raw irregular vectors remain checked inputs.
Runtime's rebuilt ground and distortion frames copy irregular angles once per input
into caller-owned pools; command offsets share each slice across recipe outputs. Render
borrows the pool until its call returns and validates each range before use. The
owning Build/Append helpers remain available for callers without a frame pool.
Raw events retain the simulation producer's session/tick FIFO order; CPU activation
retires that ingress independently from pending native emission submission.
Particle and effect-line births are passed only to a package's first selected view;
the retained particle world is rendered by every view. An auxiliary-only package
uses its first view for the same admission.
The raw ring grows with occupancy under the existing combined4096-entry bound.
Ready-prefix staging leaves pending ownership unchanged until allocation completes.
Persistent birth tombstones are sorted keys; reusable key staging/union and command
reservation finish before publishing either keys or commands.
CommitPreparedActivation checks the staged profile batch before birth reservation,
including retained EntitySurface capture capacity, then publishes CPU births/profiles
and acknowledges raw ingress on the render thread.
AcceptRendererReceipts consumes actual native submission and UI Present facts before
overall Render error handling; neither receipt is inferred from the other's success.
Performance capture samples simultaneous retained, raw, prepared, frozen-native and
frame-scratch logical capacities at selected preparation boundaries. Those samples
  are neither process RSS nor a whole-VFX admission limit. Cosmetic growth limits
  do not certify mandatory warning capacity, profile cardinality or GPU residency.
Runtime prepares cosmetic event-profile masks once per VFX image generation from
source/output ownership and authority. Ground, EntitySurface, PointRate, replacement
contexts and unidentified contributors stay required. Retained profile admission
unions required memberships before choosing cosmetic memberships by importance and
original order; payload copies and index nodes follow the choice. The shared logical
growth allowance covers this boundary, not a physical or whole-pipeline memory cap.
The image plan holds Ground/Light/Distortion/Decal/Volume/Screen/Ribbon/EntitySurface fanout maxima across
quality levels, including Ribbon source and output counts. Existing memberships and persistent base output reserve first;
new memberships project frame-vector growth before selection and reserve before
CPU publication. Ground and Distortion each charge one irregular-angle slice per
membership, independent of recipe count; uniform descriptors need no pool rows.
Ribbon separately charges its pooled outputs. Frame facts are borrowed only during
preparation. Remaining nested geometry, capture
and GPU lifetimes remain separate domains, not implicit zero-cost outputs.
Upgrade movement inputs append before this plan, so captured Line/Context ribbons
participate in the same reservation. Mesh/Fresnel persistent maxima are prepared
with the image. EntitySurface captures reserve both retained and frame-input
storage before CPU publication; only new unique memberships add prospective rows.
Their separate existing capacities are credited independently.
Ground emitters append directly to the final render input; owning typed helpers
share the emitter and retain their observation fields. Runtime has no intermediate
Ground command vector or final full-row conversion pass. Ribbon frame scratch uses
one reusable output pool and source ranges; native state copies the selected spans
into its existing retained GPU-facing slots. Owning helper results materialize
vectors only at their return boundary. Frame spans stay borrowed through Render.
Surface diagnostics use the successfully submitted view's current Surface packet:
decal-owned base/normal/material exports when active, original GBuffer otherwise.
The feature owns those resources and publishes their actual graph access/completion;
capture retains its expected format/state checks and waits for GPU completion.
  Raw flash, ballistic, mote and compatibility output plans expose fanout before sampling. Required storage is reserved
first; decoration spends only the remaining shared capacity across prepared,
retained and visible destinations. Empty retained queues transfer ownership by swap.
Birth reservation and selection share a private helper that measures held capacity
after required storage and ordering scratch are reserved. Retained flashes expire
before selection; later display preparation handles only newly appended births,
preserving the retained/birth/spark order without evaluating retained ages twice.
An empty prepared birth vector can transfer its reserved allocation to an empty
retained destination, including shard-only activations. Mixed native due prefixes
can exchange empty prepared allocations into the frozen owner; active batches keep
their original buffers and identity until receipt. Renderer particle assembly uses
view-owned CPU scratch; native upload and GPU completion ownership stay separate.
Native prefix preparation also grows from the larger empty CPU allocation when
neither allocation is large enough. This realizes the capacity credit without
retaining a larger unused scratch beside newly allocated frozen storage.
Per-view decal selection retains at most the existing64 GPU outputs in fixed
local candidate storage. A small tournament tree preserves the prior stable-sort
prefix without storing every candidate. Distortion coverage selection still sees
all eligible candidates; GPU upload and resource completion contracts are unchanged.
Retirement compacts canceled rows in its existing scans and carries forward the
surviving old-prefix length for stable merging with new births. Later frame
admission counts empty prepared storage as reusable native-copy capacity.
Live spark admission also accounts for its pending workspace. Priority affects
selection, while emission retains original source order and ordinal seeds.
  Typed birth selection accounts for distinct copies and prospective native-prefix
  storage. Compatibility decorations use remaining headroom after actual typed
  emission; intentional cosmetic drops suppress equivalent legacy output, while
  failed conversion keeps its fallback. Essential status shard/tombstone and
  fire/stamp suffix capacity is prepared before optional rows are admitted.
Runtime supplies actual VFX quality during adaptation and updates retained profiles
only when that quality changes, preserving episode identity and clocks.
PreparedPairFacts indexes only the retained snapshot pair by its
semantic publication revisions and row counts. It stores row ordinals, never
borrowed data addresses; frame adapters still recompute live age and geometry.
The admitted VfxImage also exposes a separately sorted effect-ID lookup without
changing authored program handles. Frame adaptation borrows that index and reuses
render-thread output storage; retained movement appends directly to the same owner.
Light output recipes and their effect ranges are prepared by that same CPU IO
phase and emitted as immutable tables. Ballistic mesh recipes likewise prepare
immutable source/output shape, parameter and LUT contracts at admission; Runtime
binds live event or persistent-owner values without reinterpreting that program.
Distortion and decal preparation likewise owns static knots, shapes, parameters,
texture/slice and quality contracts; per-frame evaluation only binds live values.
Output-profile presence is prepared with the image and generated for shipping;
activation does not rescan source/output tables or retain non-ribbon events in
the ribbon profile.
Ribbon and EntitySurface share prepared image tables; Fresnel and direct-owner
Mesh iterate accepted ordered recipes without authored parameter/path decoding.
Successful Ballistic activation suppresses only the corresponding legacy Mesh
before emitter construction; ShockShell and unrelated layers retain their original
meaning. Native pending uses the same owner and stable legacy-before-typed order.
Renderer accepts font bytes and DirectWrite owns its copy after initialization.
Release reads the generated, verified font range from the pack; developer hosts
load their explicit font input. Release Runtime supplies the renderer with the
compiled bulk, texture, geometry, material, character, animation and VFX-atlas
descriptors plus the retained shipping-image owner. Renderer asset resolution
reads the descriptors' bounded ranges from that same pack, including shader
products. Developer Runtime instead resolves renderer products through its
selected Generation and keeps that Generation lease for resident resources.

Runtime pairs small UI/session facts with each snapshot publication. Only the
matching successful main-Present receipt promotes that frame's draw layout into
the bounded latest UI packet. Main hit testing uses its rects and displayed
facts; queued gameplay actions retain the packet/session/selection identity and
are revalidated against the current simulation facts before application. This is
a Present-accepted input contract, not a physical scan-out receipt.
UiFrame receipt metadata publishes independently from a shared immutable RenderData
UiDrawContent. Ordinary HUD draw facts follow actual displayed clock/label/wave
values and exact bar inputs. Renderer banks retain that owner and their own raster
and font environment; same-owner hits bypass deep comparison, different owners and
legacy raw spans still compare content. CPU text-layout validity is independent of
bitmap success and GPU copy completion. Reused draw data does not freeze click facts.

RendererD3D12 is the single concrete platform backend. D3D12 types stay inside that boundary and Runtime/Apps that compose it. A generic renderer interface, factory, or registry is unwarranted without a real second backend.

RenderData owns the static light-space R32 page archive and its bounded index
and lossless page decoders. RendererD3D12 exposes a synchronous offline depth bake using
canonical environment geometry, full source texture mips and the existing material
opacity shaders. The one-shot offline entry owns its capture bootstrap and teardown;
it creates no game window, font, swapchain, post targets, or general scene pipelines.
It prepares terminal environment LOD pages without the runtime LOD streaming catalog.
Capture-only material binding admission uses the selected catalog; private complete
texture uploads supply only materials used by the depth shader's authored-opacity
branch. Opaque materials bind typed null arrays without allocating textures.
This path creates no global fallback textures or BRDF LUTs.
Typed canonical descriptors are valid before any game-view resources are created.
The initialized-renderer entry remains available for existing capture callers. Apps own
file publication and Generation selection. When content tools are enabled, the
dedicated Apps target `hs_static_shadow_bake` publishes the native bake through
AssetPipeline. Its producer identity follows RendererD3D12 implementation changes;
unrelated render-test host edits do not invalidate the archive.
The explicit hs_render_static_shadow_candidate target uses a shadow-free input
Generation; its output is composed into runtime-full after the shadow-free input.
Gameplay projection and the baker share Presentation's fixed environment-sun direction. Ordinary runtime shadow
sampling supports an optional complete coarse-page archive in the existing VSM
static D32 bank. Fine pages still raster/cache at their original precision;
dynamic depth remains separate until tap-wise reverse-Z composition. RenderData
owns the index and lossless page codec (private pinned meshoptimizer dependency).
The renderer owns bounded coarse staging and GPU seeding, while Apps own atomic
publication. Incompatible sun/grid/layout or reloaded materials/pipelines use
the realtime path. A per-view CPU plan admits omission of the full CSM producer
only when baked supply, all receiver domains and current coarse-page command
capacity are guaranteed. Otherwise the actual CSM remains registered. Software
GI retains CSM until its off-camera receiver domain is closed. CPU policy and
completed GPU publication are separate diagnostics.
CPU frame/task capture can omit GPU timing queries and their measurement-only
resolve. This is explicit in diagnostic metadata; absent GPU measurements are
not zero-duration work. Direct and Compute own separate query/readback storage
inside each submission slot; CPU reads only completed slots. Copy timing is
unavailable. Active DRS uses an `OutputDependencyInterval` with both endpoints on
Direct: the end reads the rendered output as Present and joins only its actual
ancestors. The legacy whole-graph reference span remains a distinct diagnostic
identity; full `SourceOutputReady` capture keeps its calibrated metric semantics.
The fixed shadow command allocation is partitioned by actual coarse demand and
active-group weight for approved no-CSM frames. Fallback frames retain the
original floor division. CPU admission, GPU scatter and native indirect draws
use the same nonoverlapping prefix layout; insufficient total supply retains CSM.

RenderData owns one bounded DDS wire decoder; consumers enforce their own role
and format constraints. Character color and normal/roughness products use full
BC7 mip chains with semantic mip generation at cook time. Runtime consumes the
prepared subresources directly.

The shipping linker closes renderer bulk inputs from the explicit runtime result
list, streams payloads into content.hsp, and emits AssetId-sorted const BulkRange
descriptors. Table indices are deployment-local; stable save/replay IDs are unchanged.
Range offsets, sizes, formats and digests participate in the deployment identity.
Core exposes the identity of the opened file without interpreting pack headers.
Renderer validates its packed directory once and reads individual ranges through
the retained bounded owner. Texture descriptors supply admitted subresource ranges;
geometry descriptors supply the seven GPU chunks, submeshes and ground support.
Material descriptors supply defaults, surface/domain policies and typed program/
texture references. These tables reuse the original product identities and payload
bytes. Packed consumers bind those values without repeating the package decoders;
developer Generation loading keeps its explicit decoding boundary. Character and
named-animation descriptors retain small skeleton/clip/binding tables and verified
subranges of the original products. Packed TRS rows occupy 40 bytes; their sampling
adapter returns Core's padded value only for accessed rows. Monster palette rows
read directly into the existing upload vector in their cooked transposed order.
ContentLinker emits these renderer descriptors into the compiled shipping tables;
the matching payload ranges are stored in `Content/content.hsp`. Runtime opens the
pack only after its extent and compiled 64-byte header match, then passes the same
bounded owner to renderer resource readers. Producer provenance remains in the
shipping receipt and ActionKey; it is separate from the shipping content identity.

Environment materials have one canonical product per authored material, independent
of geometry LOD. Geometry metadata products contain a checked material dependency
reference; LOD0 also references its hierarchy. Material metadata, program, bindings,
parameter layout and surface layout belong to the material product only. Terrain
and ground-detail geometry share their existing standalone material products.
The renderer derives one immutable material variant per authored ordinal and swaps
those facts with the candidate geometry on resident reload.

Geometry page preparation returns its parsed package view and validated geometry
together. Startup, streaming and GPU upload reuse that result. Package views borrow
the read bytes through the final CPU copy; GPU staging retains its copy until the
existing upload completion. Metadata-backed reads do not recompute payload bounds
or repeat topology and vertex validation after decoding.

Developer Hero work capture selects five separate diagnostic pixel shaders and
owns bounded per-slot/per-phase query, counter and coverage storage. Runtime
writes completed samples through its existing diagnostic output; the normal
renderer selects the original shaders and allocates none of this storage.
The diagnostic shading modes measure sensitivity and do not select production
quality. These products are excluded from the shipping Generation closure.
The nearest Hero composite writes HDR and its six reconstruction guides in one
draw, sharing the selected surface. Farther layers retain one color target.
Ribbon indirect arguments are frame-slot scratch; shared history alone needs
all-reader completion when its layout changes.

AO, GI, reconstruction and post processing connect through their concrete inputs.
The FrameGraph owns resource dependency ordering, versions and lifetimes; there
is no second semantic registration graph above it. The post compiler emits the
configured fixed pass chain. Native services validate actual resource formats
and extents at their boundaries.

The graph retains at most eight exact value-only compiled plans. Declaration
storage can survive Reset; callbacks, external owners and logical uses do not.
Candidate scans share one current resource-shape snapshot. Flat reusable alias
scratch labels the first occurrence rather than retaining native addresses or
allocating a hash node per resource. Cold registration refreshes that snapshot
after provider preparation; native certification and binding remain authoritative.

Essential renderer initialization has a per-segment ceiling from reported adapter
capacity, charging successful native allocation sizes only during that startup
scope. Static capacity is never reported as currently available memory or used for
optional streaming/candidate admission. Those paths retain observed OS budgets.
Standard pools resolve by heap type; only CUSTOM heaps resolve by L0/L1 preference.

Authenticated ranged software textures admit their unpublished candidate and one
bounded reusable UPLOAD window together before payload IO. Device footprint plans
belong to that fixed transaction; chunks batch within host and native window limits.
Copy completion permits window reuse, and final publication or safe cancellation
releases it. Existing one-shot and reserved/mapping-only paths keep their contracts.
Local texture pressure excludes nonlocal upload heaps on discrete adapters; total
physical-memory diagnostics still report both segments.

Sequential views retain their own temporal and native reconstruction state.
There is no parallel semantic history-resource registry: resources and history
generation owned by each view are authoritative. Main renders first; auxiliary
views retain packet order and share canonical scene simulation. Shared descriptor
banks, allocators and upload scratch belong to the three global submission slots;
each slot waits for its actual completion before reuse. Output backbuffer indices
remain separate from those slots. OwnedView is used directly during initialization,
rendering, resize, options, capture and resident reload; there is no view exchange
mirror or whole-device drain for selecting another retained view. Native SDK
selectors preserve separate viewport contexts; actual reset/reconfigure and
presentation reuse still require their completion boundaries. Geometry SRV banks
retain resolved resource ownership and slice ranges; only changed bindings are
rewritten after slot completion. Resident descriptor publication transfers the
prepared bank snapshot into the destination heap. Feature preparation/admission
uses the target view; main lighting status and per-view geometry/HLOD history do
not read a parallel Impl service mirror. Hero retirement drains samples, resets
completed native work, then folds statistics once.
UI targets/bitmap state are per view; upload buffers grow and reuse capacity in
the waited submission slot. Pending cached bitmaps recopy into that shared scratch.
Instance identity, retained surfaces, union-consumer monster LOD and stable draw
order are prepared once per package. Current stable-slot/GPU-instance reconciliation
runs on its first view. View skin offsets/strides live in each waited submission
slot's upload table; later views do not mutate the common scene table. Two immutable
descriptor-table variants select scene history or ribbon history while retaining
the same view skin table, without adding a root parameter.
The submitted canonical skin owner/range directory defines current deformation
across all views; previous leases/ranges and history validity remain per view.
Current instance tables use pooled GPU owners; each view leases its own submitted
owner directly as previous data. There is no current-to-history GPU copy. Each
backing uploads only active slot/channels changed since its accepted payload
revision, including changes across skipped revisions. Header and entity-surface
publication belong to that backing. Only generation, mesh and motion-epoch facts
remain on the CPU, not previous full instances.
Reuse requires the same device, no logical readers and completed GPU uses. Scene
upload-only frames still publish a truthful table. Prior table reads cover compute
visibility resolve and raster/transparent/Hero motion; current-only consumers do
not acquire a history read. Original per-view projection retains old coordinates
through origin changes. One perspective-correct unjittered clip varying supplies
previous UV/depth, and active raster coordinates supply current motion UV.
Capture identifies frame scratch by its actual submitted slot/id and committed
constants stamp; output image indices remain separate. A reused slot cannot be
reported as fresh per-view CPU capture metadata.
GPU-read completion retains that owner independently of slot and UI lifetimes.
Borrowed pose pointers end with the Render
call; retained scratch capacity does not extend the caller's input lifetime.
Runtime supplies a package sample timestamp captured before effect preparation;
direct renderer callers may omit it to sample at entry. Explicit timestamps are not
retimed by individual view history; backward samples invalidate motion history.
Event light, distortion,
decal and volume lifetimes use fractional display ticks from that timestamp.
Native VFX input owns a frozen session/target/sequence batch until actual producer
submission, independently of CPU activation or UI Present. Collision particles
hold at the integer display target; native analytic slots age at display time.
An explicit zero sequence supplies the held clock without a batch; legacy callers
that omit the clock retain snapshot-tick stepping. Gameplay/rendered snapshot tick
and the common analytic camera clock are not replaced by the frozen target.

Each rendered package evaluates one display sample from the producer's actual
adjacent tick pair. Transforms, poses and endpoint lifetime transitions share that
sample across views; package-owned Archer palettes feed bounds and canonical skin
compute. All actual character draw consumers use that cache; the unused128-matrix
compatibility cbuffer and legacy vertex deformation are removed. Qualified bounds
with no skin allocation encode no package consumer. Visibility omits their visible
and rejected output, still publishing zero indirect arguments; shadow preparation
omits their caster/receiver work and retains retirement of previous caster records.
Packet-owned instance lookup and sorted attachment IDs serve Fresnel and skin
demand across views, without scanning every effect for every instance.
Mirrored shader prefixes
share the4720-byte FrameConstants layout, and upload offsets follow its alignment.
Each view records its successful sample time independently of retired GPU resources.
CPU scene payload revisions and current GPU owners commit together at successful
graph execution, before fallible service completion. Per-view display samples and
history accept after successful graph/service submissions, before Present. A later
Present failure preserves them; a partial/pre-commit failure does not advance the
display-clock record.
Capture callers use `Capture(request)` with a view ID. Legacy format-specific
capture helpers are private, so callers cannot bypass submitted-frame ownership
checks. Shared geometry/visibility diagnostics require a retained submission
belonging to the requested view.

Material emitters validate a graph once before optimization. Deferred closure
emission derives sidecar families from that same graph; callers do not supply a
second mask. Authored graph operations and parameter-only cooking remain shared.

## Runtime and Apps

Runtime is the composition root, so it may know the concrete systems it runs. Cross-subsystem knowledge in Runtime is not by itself an architecture violation.

Runtime loads saved settings once and applies explicit command-line overrides
before creating the window. Apps parse arguments once and do not load profiles.
Smoke uses process graphics defaults and explicit overrides.
Experiments can supply a typed graphics preset through the same startup rules;
explicit process fields then override that preset without persisting it.

Apps parse arguments, select configuration, call a library/runtime entry point, and return process results. They must use public module APIs rather than another module's private implementation.

Release omits development recording/replay implementations, diagnostic CSV/log
writers, simulation timing scopes and debug mutations. Developer command-line
modes are compiled only in non-Release builds. Renderer development features,
PIX markers, resource labels and debug-layer/DRED setup use the same non-Release
boundary; no PIX or SDK debug-layer DLL is deployed for Release. Game-control
timing for dynamic resolution and available automatic frame generation is
separate from optional diagnostic recording. Auto-FG cadence is retained only
while that policy is active, and accepts samples after submission completion.
Installation verifies staged executable/pack copies outside the game process;
normal startup still checks the compiled content identity against its opened pack.

Jobs submission acts through a creator-bound TaskGroup and returns admission
success. TaskGroup owns cancellation and joining; task labels, per-task numeric
handles and scheduling priority are not exposed.

Experiment children execute their validated spec directly. The parent retains
process isolation and a watchdog; static timeline commands are not retransmitted
through a separate IPC protocol. Runtime holds its selected simulation rules for
the session. Rebuilding and restarting selects a new Generation.

## Public and Private boundaries

Each module's `Public` directory is its supported cross-module API. `Private` is implementation detail. Tests should prefer public behavior; direct private testing is acceptable only for an existing, legitimate private unit.

## Build isolation

Material graph validation/optimization/HLSL generation is the RenderData-owned
`hs_material_compiler` target, enabled only for content tools or RenderData tests.
Only the material shader producer, render package producer and material package
tests link it. `hs_render_data` contains runtime data formats without depending
on this compiler, so compiler edits do not relink unrelated producers or the game.
Each material producer invocation owns a move-only PreparedMaterial with its
validated/folded graph, reflection and reachability. Context, uniform, opacity,
layout and layer-proof consumers borrow that preparation; JSON is parsed once.
Legacy lowering retains its separate pre/post transformation checks. Raw graph
compiler APIs delegate to the same emitters for standalone compatibility calls.

`hs_environment_layout` is an internal Presentation target that compiles the
existing environment builder once. `hs_presentation` exposes it transitively;
layout-only scene producers link it directly. `hs_generation_lock` similarly
compiles AssetManifest publication locking once and is exposed by
`hs_asset_manifest`. Action producers link this narrow locking target; consumers
that parse selected manifests still link `hs_asset_manifest`. These targets retain
their module ownership and ordinary compiler/Ninja dependency tracking, so unrelated
presentation or selected-manifest edits do not invalidate producer recipes.

Asset recipes identify the linked producer executable by SHA-256. CMake/Ninja
owns source/header rebuild dependencies; a second source/compiler/link-command
authentication graph is not maintained. Explicit asset and shader-tool inputs
still participate in each Action key. Equivalent but byte-different relinks can
recook assets; identical products still retain their ProductSet identity.
The Apps-owned `hs_asset_pipeline` static library compiles the shared Action
implementation once. Producers remain separate executables, so a producer-specific
change does not invalidate unrelated recipes. Each process hashes its own executable;
there is no intermediate recipe manifest or post-link preparation target.
Material and runtime DXC compilation consume hashed private source/include
snapshots; generated includes are owned by that producer stage. Saved runtime
shader input closures permit cache hits without dependency-scanner processes.
Environment producers derive consumed paths from frozen metadata and hash the
captured bytes they cook. Shader process waits share bounded termination handling;
uncertain process exit preserves its stage instead of racing cleanup.

CharacterContent imports one CharacterIR and directly emits final character
metadata, LOD0 geometry, named animation and DDS products. Normal builds do not
exchange native meshbin or material JSON sidecars through a second producer.
ContentLinker reads exact character/material/texture snapshots, retains checked
bytes while validating their bindings, and reuses the prepared metadata. Explicit
CharacterInterchange compatibility export remains a separate developer leaf.

The low-level configuration is:

```text
HS_BUILD_RUNTIME=OFF
HS_BUILD_CONTENT_TOOLS=OFF
```

It builds Core, Jobs, GameDomain, GameRules, Gameplay, Presentation, and their low-level tests without AssetManifest, RenderData, ImGui, Agility SDK, PIX, D3D12 Memory Allocator, runtime fonts, DXC, DirectXTex, or FBX SDK. Shader infrastructure exists only when Runtime or content tools need it. Runtime and content options default to `ON`.

## Windows verification

Named Debug executable builds compile/link against existing content; they do not
recook the full generation. Build `hs_content_generation` explicitly when content
changes. The default ALL build still prepares/verifies the runtime generation,
and Release retains its compiled shipping-image dependencies.

Runtime VFX cases share two test executables, grouped by synthetic versus cooked
inputs. Each case remains a separate CTest process and name. Only cooked cases
prepare a Generation; compiling one group never compiles the other group's cases.

`cmake --workflow --preset verify-core` builds the low-level libraries and tests
with MSVC `/fp:strict`. `verify-portable` runs the core, CPU RenderData and asset
pipeline superset once on Windows. CI runs only on Windows; Linux execution
jobs and presets have been removed. These CPU checks do not replace D3D12 or
GPU execution.

`HS_BUILD_RENDER_DATA_TESTS=ON` explicitly opts into CPU RenderData, selected
Generation, character-interchange, and the private production FrameGraph range
compiler checks without enabling the runtime or
FBX/DXC content tooling. This option defaults to OFF, so the ordinary low-level
configuration retains its dependency isolation. The real asset-action scheduler
regression additionally uses Python when available with the Ninja generator.

`HS_BUILD_ASSET_PIPELINE_TESTS=ON` separately opts into the real portable Audio
and Generation executables plus publication fixtures. Core owns shared streaming
SHA-256; RenderData retains its public wrapper. Neither opt-in enables D3D12 or
content SDK dependencies. Both remain OFF by default.

## Mechanical enforcement

`Tests/check_architecture.cmake` scans production includes, Gameplay graphics API knowledge, cross-module `Private` access, and major `target_link_libraries` edges. It intentionally uses CMake text checks; no parser framework or second build graph is needed for the current rules.

Code, target links, and executable tests are the source of truth if this document becomes stale. Verification policy is defined by the applicable `AGENTS.md`.
