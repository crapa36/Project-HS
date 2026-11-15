# Project HS Architecture

This document describes the module graph enforced by the current build and `Tests/check_architecture.cmake`. It is not a future-system roadmap.

## Module DAG

```text
                         Core
              ┌──────────┼───────────────┐
              ▼          ▼               ▼
            Jobs ────────────────────► RendererD3D12
                         GameDomain       RenderData
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

Environment texture/histogram and mesh cooking use separate producer executables.
The mesh producer has no DirectXTex, D3D11 or COM requirement. Both retain the same
input-snapshot and publication helpers; ActionKey includes the actual producer
executable identity, so mesh-only implementation changes do not invalidate texture
recipes through a shared executable.

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
| RendererD3D12 | Concrete Windows/D3D12 backend | Core, Jobs, RenderData |
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
- RenderData's generic package framing, material schema/sidecar, and texture package
  codecs are bounded static leaves: `hs_render_data_package` depends on Core and
  RenderData values; material and texture leaves depend on the package leaf.
  `hs_render_data` aggregates these leaves with the remaining runtime codecs.
  The material compiler and material shader producer link the material leaf, while
  the texture package producer links the texture leaf, preserving their existing
  Core, asset pipeline, DirectXTex/DXC, JSON, and bcrypt requirements.
- RenderData's `hs_terrain_surface` codec leaf links `hs_render_data` and only its
  actual consumers; terrain codec edits do not relink unrelated content producers.
- `hs_runtime_contracts` is a header usage-requirements target for native-free
  Runtime public values/contracts. It exports Runtime/Public and renderer public
  value headers and depends on Core/GameDomain/RenderData values; it neither builds
  nor links the native Runtime/renderer backend, IO, DXC or content products.
  Runtime, content consumers and low-level VFX fixtures inherit it directly.
- `hs_renderer_catalog_io` is RenderData's admitted renderer metadata wire codec
  and owns character descriptor admission/compiler code. It depends only on
  RenderData/Core and only targets that consume these codecs link it, so unrelated
  texture/material producers do not relink when character descriptor code changes.
  Native allocation and upload remain RendererD3D12 responsibilities.
  `hs_audio_image_io` similarly owns Runtime's
  native-free audio metadata decode without initializing an audio device.
- Runtime's `PreparedContent` retains the opened image and decoded simulation,
  presentation, audio, VFX and renderer catalog owners for their consumers.
  Generated C++ supplies only the small image manifest; large tables are bounded
  binary pack sections shared by development and shipping admission.
- RendererD3D12 must not depend on game, presentation, or runtime modules.
- Production modules must not include another module's `Private` headers.

Moving a higher-layer type into Core only to bypass these rules is also a dependency violation. Keep types at the lowest layer that owns their meaning.

## Deterministic simulation boundary

Gameplay owns fixed-step state transitions, movement, combat, collision, statuses, progression, cleanup, and deterministic checksums.

Gameplay must not contain D3D12/DXGI, renderer, presentation, UI/window, audio, filesystem/runtime orchestration, or wall-clock-driven game decisions.

Structural changes must preserve phase order, fixed-step behavior, seeds, `/fp:strict`, deterministic iteration, checksums, and public read-model semantics. `Source/Gameplay/Private/simulation_pipeline.hpp` is the authoritative phase order.

Enemy and projectile hot/cold groups are the single authoritative actor storage.
Private status, attribution and history remain in Gameplay. GameDomain's scoped
TickReadView borrows those owners through semantic accessors, immutable spans and
cached small facts; it owns no actor/history mirror. The simulation owner finishes
mutation before creating a view. Presentation consumes it synchronously and owns the render
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

Content remains fixed for the lifetime of a running game. Offline content edits
take effect at the next launch. Build-cache and snapshot ownership do not define
native allocation, queue-completion or file-backing lifetimes.

Renderer catalogue semantics are admitted once by the RenderData decoder or the
loose development-catalogue factory. Their metadata is immutable and retained by
an owning handle; native initialization/resident preparation checks that the bound
views and actual pack size still match that owner. Raw tool inputs use the same
semantic validator from the existing RenderData-owned `hs_renderer_catalog_io` leaf;
RendererD3D12 links that leaf without acquiring Runtime or content-tool dependencies.
Actual file reads still verify bytes, ranges and digests before
decode/upload; payload and GPU lifetime admission remain separate.


Presentation projects stable GameDomain contracts. It does not execute rules or reach into Gameplay implementation state.

Character authoring binds each FBX material slot to a material identity. Cooking
reads that material's published capabilities and program set to select the
opaque/masked or translucent domain; character names do not select a pass.
The admitted GPU binding table stores set headers and per-slot domain ordinals.
Each64-byte geometry record carries its template's material-set index. Vertex
evaluation resolves the slot once and passes the flat ordinal pair to pixel
shading and visibility reconstruction; no fixed actor-range constant table or
mesh-to-material switch selects a domain. Visibility resolve passes the classified
provoking-vertex pair into triangle reconstruction rather than resolving it again.
Ordinary alpha transparency is a
surface domain, independent of solid-transmission optical capability. Preparation
publishes opaque/translucent domains on each draw template; one bounded frame
list drives coverage, ordinary optical phases, OIT fallback and diagnostics.
LOD cuts share their source coverage. Actual per-view bounds determine exclusion;
unknown bounds retain the draw. Shadow feedback uses the same admitted material
domain and qualified yaw/scale/skin/wind surface bounds. Static forward receiver
bounds remain in the immutable scene batch; dynamic receivers follow the current
pose, independently of caster eligibility. Object names or the presence of another
object do not choose an alternate transmission topology. Common Hero pass
shaders are compiled once; material packages reference those shared programs
and retain their own parameter layouts and capabilities. Procedural water keeps
its explicit solid-transmission closure as a bound material program and uses the
same ordered optical kernel as other transmission surfaces.

RenderData owns RenderInstance, immutable scene/snapshot values, RenderSnapshotPair,
VFX frame inputs and RenderPackage. Presentation owns RenderSnapshotExchange;
Core retains the generic SPSC queue. Renderer consumes the standalone pair value,
so it has no dependency on Presentation's exchange or producer lifecycle.
The pair always contains a current snapshot value, including an empty scene.
Only the exchange's ReadPair records whether any publication has arrived.
Runtime selects the non-null immutable UI owner before package assembly; the
renderer uses that owner without a raw-UI fallback. Screen-overlay intensity is
owned by applied renderer settings rather than repeated in each frame package.
Raw presentation events end at Runtime's VFX/audio translation. The renderer
frame contains prepared VFX inputs. The normal Render entry consumes the package's
camera and existing Main owner directly. Tool RenderViews takes samples for
explicitly configured Preview/Capture owners; output size, capacity and role are
not inferred from frame samples. Both entries use the same native view renderer,
with tool-only per-view results and no snapshot-to-package compatibility recursion.
Camera and scene inputs use arena-local Float3 metres. The renderer derives the
camera-relative reconstruction basis from the actual eye; callers do not submit
scene/view origins or request world rebasing. Per-view previous submission,
camera/skin/grass histories and conservative culling arithmetic remain independent.
Lighting owns two per-slot buffers: stable light records and a grid header followed
by complete local-light membership masks. Directional lights retain their prefix
and separate shading loop. With no local lights, only records/header upload is
registered; shaders do not read unused cell masks. Retained layouts distinguish
that topology, and both buffers follow the existing submission completion owner.
Development previews are package producers in Runtime and the existing native
fixtures. They own synthetic scene/pose spans and choose their camera and VFX
inputs before Render; the renderer does not replace the supplied scene. ImGui
preview edits return through the existing development-frame/result values for
the next package, preserving actual previous/current instance transforms.
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

Presentation prepares a compact MeadowGroundQuery from the admitted terrain transforms
once per scene owner. Camera, character/projectile projection, signals and footsteps
borrow that same immutable query. Development rooms publish their actual prepared
query to simulation; a changed owner refreshes the current sample without rotating
history at a frozen tick. Renderer particle collision uses the existing per-slot
terrain section buffer with exact row count and matching transformed-height rules.
The generated height bound is shared; neither path assumes a terrain at the origin.
The exchange issues a move-only write lease with a private slot index. Publishing
or abandoning acts on its originating exchange; an unfinished lease abandons on
destruction. Callers cannot forge a slot or submit it to a different exchange.
Runtime's explicit retained writes reuse a slot's dynamic arrays only for a known
immutable (session, actual tick, dynamic revision) sample. Header/camera/scene/UI
still refresh on publication, and previous/current remain independently owned.
Raw writes clear storage and raw copies always copy. Mutation and copy/move failure
boundaries invalidate reuse; no hash or pointer identity validates the arrays.
Animation updates share the existing actual-dynamic invalidation used by projection.
Renderer shares one decoded AdmittedSceneHlod owner across native view storage;
allocation sizing does not repeat immutable metadata validation. HLOD member
mappings and proxy bindings prepare once per retained scene and refresh only
residency-dependent fields each frame. New page payloads, dynamic bindings and
completion/reuse boundaries remain checked.
The offline scene cooker may start parent proxies from an existing authored LOD2
with matching materials, flags and selector coverage. It certifies the original
to that representation once per source, then carries an outward-rounded scaled
error through each material partition. Leaves retain the original instances;
parent reduction is compared with the actual child cut, not its temporary inputs.
HLOD shares material textures rather than duplicating them into an atlas. Texture
quality follows actual demand and budget pressure; preferred registration alone
preloads the tail and does not force idle textures to their finest mip.

The SPB2 baked lighting package carries world-space cosine-convolved SH9 for
independent regular-grid nodes, visibility masks for each cell's 4x4x4 receiver
subcells, and raw/GGX cubes for a separate set of reflection boxes. Offline
native cubes capture sky, emission and directly lit static surfaces before
exposure. The offline solver feeds each completed diffuse grid into the next
capture until its absolute/relative convergence policy is met within the bounded
iteration budget, then captures reflections against the converged grid. Dynamic
indirect lighting remains outside the solver. Runtime blends up to eight
reachable SH corners and uses a baked list of all overlapping reflection boxes.
Outside the diffuse grid it uses the global environment. Empty interior
visibility masks contribute zero local irradiance. Direct lights, dynamic
shadows and screen-space reflections remain independent.
Admitted baked grids keep all 27 float32 SH channels in seven Float4 rows per
node, followed by five cell-metadata rows per cell in one buffer. Runtime and
offline sampling share normalized reachable-corner interpolation in float32.
The published grid owner pins the buffer until all GPU readers complete.
The offline probe baker owns native capture, convolution and signed first-hit
readback. The runtime adapter admits the baked package, uploads, publishes and
retires its read-only resources. Both consume a shared static scene/material
revision contract and the same common native rendering code. Saved dynamic-GI
settings migrate to Probe at the settings read boundary.
Capture bindings and native leases are compiled only into the offline baker.
The shared graph layout keeps an opaque owner with the same representation in
both compositions; only the baker allocates its capture payload. The sampled
runtime uses the frame's descriptor base directly and carries only the revision
and publication inputs it consumes.

Static lighting compatibility uses authored scene/material inputs and local
material mutations since Generation admission. View quality and capture quality
do not disable a matching bake. Offline capture still tracks its actual quality
and exports only the matching completed capture. Immutable baked publication
waits for its upload producer; retirement waits for every reader.

Shader product maps retain the canonical named path and digest. Feedback selection
chooses a physical variant at read/bind without overwriting those identities.
Loose and packed reads use the same selection, while bake compatibility uses the
canonical products. Admitted reads retain their integrity proof rather than
rehashing the same shader bytes in each consumer.

Terrain surface production belongs to the offline `hs_terrain_surface_bake` tool.
It uses the selected world's actual section transforms and seed, and records the
material/texture/geometry inputs used by the existing page evaluator. TSF1 metadata
retains ordered section records and a source identity; each section's three planes
use an ordinary TexturePackage, keeping the existing package size limits. Runtime
admits matching pages, uploads them, publishes and retires their native resources.
Registered planes retain their admitted tile layout for that pinned generation;
scene changes compare against it without rereading the archive or source identity.
Missing, incompatible or unaffordable pages select the original terrain evaluator;
runtime does not create page-bake PSOs or execute the page-production shaders.
Those two shader products remain in the producer input only. Build provenance is
kept in Action inputs; runtime product dependencies connect metadata to section
textures. Producer and consumer share the same source-identity and section values.
The producer reuses one 4096-square RGBA8 working plane while producing each tile,
with a 256-texel continuation around its 3584-texel interior. The index references
leaf metadata whose three compressed planes are independent ordinary texture
packages. Color/normal mip filtering is preserved before BC7 compression.
Current surface layout keeps color/roughness in plane0, base normal/AO and
residual depth below the mesh(0..0.05m) in plane1, and four layer weights in plane2.
Color uses an sRGB view; all other channels use the linear view. Runtime performs
one residual-height search and samples independent fine normals from the source
detail layers. Page admission excludes legacy channel layouts; insufficient
texel density or complete ray/filter support selects the original evaluator.
Admitted interior samples use the existing anisotropic sampler. Filtered residual
mips are ordinary appearance levels, not conservative ray-skipping bounds.
The meadow's physical base height is authored from the same texture height inputs,
metre scale and fixed asset seed as its surface. The author generator emits the mesh
and shared compact CPU/HLSL height query; authored depressions and water remain
separate explicit deformation inputs. Scene placement uses that same query.
Independent source detail and the page residual remain appearance approximations;
the source evaluator is retained when page density/support cannot represent them.
Packed and developer catalogs admit the same section-image products;
the small TSF1 metadata is not a texture descriptor.
Visibility pixel classification is a separate runtime shader family. Count,
prefix and scatter share variant decoding with material resolve through a small
include; classification-only edits do not invalidate authored material shaders.
Adaptive GTAO is produced by its own runtime shader family while preserving the
`render/shader/adaptive_gtao_cs.dxil` product. Its include closure tracks the shared
AO implementation, independently of the BSDF-preintegrate and Hero-work actions;
the base ambient-occlusion runtime family remains unchanged.
The ProjectHS app selects Visibility by default; experiments and native tools keep
their explicit renderer configuration. Visibility's typed texture stores are
required by the backend's existing Direct3D feature level 12.0 baseline.

Presentation's immutable UiFrame owns both draw models and their displayed
interaction facts. The exchange retains that owner with the current packet;
Renderer receives its native-free draw span and the existing publication identity.
Runtime accepts only a matching successful Present receipt, retaining the same UI
owner plus the actual output extent for click admission. UI reuse compares concrete
world/UI/settings dependencies before generating models; it does not compare draws.
Simulation GPU submission history and accepted UI Present history are independent.

PresentationCatalog and SimulationRules are typed tables. Their developer file
decoders are the separate hs_presentation_io and hs_game_rules_io libraries.
Release Runtime decodes the content-linker's bounded binary sections into typed
owners; generated C++ retains the small image manifest. Developer Runtime retains
the selected-Generation loaders. The admitted startup Generation retains its exact
pointer snapshot and backing; later pointer edits take effect after restart.
Simulation's canonical byte
writer and decoder share one typed member inventory, preserving ABI padding
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
full cook into unrelated consumers. Runtime integration tests use their selected
Generation; shipping content tests retain image and pack ownership checks.
Single-config Release can build with HS_BUILD_CONTENT_TOOLS=OFF using an existing
Shipping/content.generated.cpp and Content/content.hsp in the build directory.
This path has no content-producer or Generation-verification dependency. Missing
files fail configuration; Runtime still verifies that their identities match.
DXC, ImGui and a loose font binary are not required for this runtime-only build;
native renderer dependencies and the font license remain required.
The linker atomically replaces Content/content.hsp before updating its generated
compile input. These two files are not a transaction: Runtime rejects an interrupted
pair by matching the compiled 64-byte header and file extent. An already opened
owner retains its original file. The Release installer reads that same compiled
header from the executable's read-only
`.hsimage` PE section and compares the staged pack header/extent before publishing;
it does not run a development CLI or infer compatibility from copy hashes.
Core's move-only ShippedContentOwner owns the
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
The 64-byte shipping header keeps its semantic content ID at bytes 16..47; bytes
56..63 bind the two packed VFX offsets, sizes and SHA256 digests via the first
eight bytes of a domain-separated canonical SHA256. The semantic ID retains
its original fixed-header marker, independently of that physical storage tag.
Startup still verifies each full VFX range digest.
Release audio borrows admitted cue rows and owns one preload PCM arena. Cooked
chunk digests cover contiguous preload runs and independent transition tracks;
only successful verified reads publish ready PCM. Only
transition tracks use asynchronous preparation and completion-safe eviction.
Developer JSON/WAV loading is isolated in hs_runtime_audio_io and excluded from
Release Runtime. VFX uses the existing record types through VfxProgramView and
VfxImage spans; PreparedContent retains decoded shipping metadata and its backing. Developer
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
Flash and Mote image recipes own immutable display appearance. Retained and visible
births borrow that appearance and carry their own scheduling, geometry, age, seed
and motion state. Runtime pins the image through all retained births and Render;
tools keep their supplied appearance owner alive through the same consumption.
Native upload packs the existing particle ABI from both parts. No per-birth shared
owner or duplicate material registry is used.

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
Opaque GBuffer and AO metadata use one R32_UINT that preserves
the current producers' 24-bit material payload, namespace nibble and six flag
tuples. Hero identities and mixed temporal keys remain RG32_UINT. Pipeline
admission requires the selected generation's surface_layout.abi; developer raw
captures decode metadata to the existing logical material/flags pair. Peeled
layer-zero depth/identity supply reconstruction guides without duplicate targets.
  Surface ABI 5 also pairs VSM page admission with one group per draw segment:
counts load cooperatively, while one lane preserves ordered budget decisions.
Only admitted pages generate depth clears, including pages with no caster draws.
  Indexed shadow groups use one indirect instanced command with contiguous private
  DrawRecords; VS resolves base plus instance and carries that record to PS clipping.
  Group templates live in existing 32-byte segment metadata, while per-caster GPU
  data is 64 bytes. Mesh and local-shadow draws retain per-record commands.
Raw flash, ballistic, mote and compatibility plans expose fanout before sampling.
A shared64MiB soft allowance counts logical canonical rows, profile references,
retained/frozen births and current frame outputs/payload. It does not estimate STL
nodes/buckets, free vector capacity, preparation scratch or future split copies;
process RAM and allocator reservations are separate observations. Required warning,
geometry and status outputs bypass the cosmetic cap and reserve their actual ranges.
Optional selection uses importance and stable input-order ties. Emission preserves
source/ordinal seeds, and intentional cosmetic drops do not revive equivalent legacy
output. Failed conversion retains its actual fallback.

Retained flashes expire before birth admission; display preparation evaluates only
newly appended births. Live spark admission counts retained and visible rows without
predicting a staging vector's allocation. Persistent spark cursors update in place
and births append to their final retained output; allocation failure exits the render
thread instead of retrying a copied cursor/output transaction. Fire/stamps reserve their essential suffix
before decorative rows. Frame scratch is cleared after the prior Render's CPU work
has joined, so a previous frame's spare capacity is never an admission input.

Ground event routing uses the existing prepared effect profile word for charged
pulse and wave contracts. Wave evaluation retains live context/age checks; effect
identity, fixed timing and two-source shape are resolved when recipes are prepared.
Ground admission checks the program's unique lookup directly rather than retaining
the same lookup count in every recipe row.

Empty retained queues may take prepared backing by swap. Native freezing keeps the
larger partition and copies the smaller due/future partition, reusing an empty
allocation when possible. A frozen batch retains its buffers and identity until its
matching submission receipt; GPU completion and UI Present remain separate. No new
handle registry is introduced. Per-view decal selection retains the existing64GPU
outputs in fixed local candidate storage, while distortion sees all eligible inputs.
Actual upload, pool index bounds and native completion are preserved.
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
Their fixed recipe records live in RenderData values. Catalog admission creates
checked borrowed recipe views. Renderer startup retains those views in its immutable
content configuration and checks their bounds against the actual resident atlas once.
Frame commands carry recipe indices and live owner geometry/time only. The catalog
backing outlives the renderer and its joined recording jobs; dynamic command indices
and native upload ranges are still checked for each submitted package.
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
unrelated render-test host edits do not invalidate the archive. Runtime frame
composition is excluded from the offline baker; shared bootstrap, capture,
resource and teardown changes remain producer dependencies.
The render test host links the runtime renderer for its GPU lifetime fixtures.
The explicit hs_render_static_shadow_candidate target uses a shadow-free input
Generation; its output is composed into runtime-full after the shadow-free input.
Gameplay projection and the baker share Presentation's fixed environment-sun direction. Ordinary runtime shadow
sampling supports an optional complete coarse-page archive in the existing VSM
static D32 bank. Fine pages still raster/cache at their original precision;
dynamic depth remains separate until tap-wise reverse-Z composition. RenderData
owns the index and lossless page codec (private pinned meshoptimizer dependency).
The renderer owns bounded coarse staging and GPU seeding, while Apps own atomic
publication. Incompatible sun/grid/layout or material/pipeline contracts use
the realtime path. A per-view CPU plan admits omission of the full CSM producer
only when baked supply, all receiver domains and current coarse-page command
capacity are guaranteed. Otherwise the actual CSM remains registered. Active hero
volumes and allocated froxels add world receivers; disabled volumetrics add no
hero-volume receiver bounds. CPU policy and completed GPU publication are
separate diagnostics.
CPU frame/task capture can omit GPU timing queries and their measurement-only
resolve. This is explicit in diagnostic metadata; absent GPU measurements are
not zero-duration work. Direct and Compute own separate query/readback storage
inside each submission slot; CPU reads only completed slots. Copy timing uses its
own storage when the device supports Copy queue timestamps. Development captures
keep texture-transfer queries separate from Main queries, so Main storage can grow
after accepted transfers without replacing an in-flight query/readback owner.
Both are covered by the frame's merged completion. Capture-only CPU
preparation slices measure main-thread wall intervals; separately reported worker
intervals overlap those slices and must not be added to them.
Active DRS uses an `OutputDependencyInterval` with both endpoints on
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
ContentLinker emits these renderer descriptors into bounded binary shipping tables;
the matching payload ranges are stored in `Content/content.hsp`. Runtime opens the
pack only after its extent and compiled 64-byte header match, then passes the same
bounded owner to renderer resource readers. Producer provenance remains in the
shipping receipt and ActionKey; it is separate from the shipping content identity.

Environment materials have one canonical product per authored material, independent
of geometry LOD. Geometry metadata products contain a checked material dependency
reference; LOD0 also references its hierarchy. Material metadata, program, bindings,
parameter layout and surface layout belong to the material product only. Terrain
and ground-detail geometry share their existing standalone material products.
The renderer derives one immutable material variant per authored ordinal during
startup resident preparation.

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

The graph retains at most eight exact value-only compiled plans. Validated logical
declarations survive Reset; current callbacks, native owners and bindings do not.
Candidate scans share one current resource-shape snapshot. Flat reusable alias
scratch labels the first occurrence rather than retaining native addresses or
allocating a hash node per resource. Cold registration refreshes that snapshot
after provider preparation; native certification and binding remain authoritative.

Physical import identity and placed-resource overlap are checked against current
native bindings before logical compilation, including retained-plan hits. The
logical compiler receives sizes, ranges and versions, without a second native
identity tag or duplicate hash table. Its range, access and version checks remain.

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
Development residency category and storage totals change with the allocation
ledger, so reporting reads fixed totals rather than walking allocations again.
Release omits continuous diagnostic totals and reads admission budgets directly.
The renderer exports its layout-affecting development define to native consumers;
budget observations and completion-based retirement retain their own lifetimes.

Sequential views retain their own temporal and native reconstruction state.
There is no parallel semantic history-resource registry: resources and history
generation owned by each view are authoritative. Main renders first; auxiliary
views retain packet order and share canonical scene simulation. Shared descriptor
banks, allocators and upload scratch belong to the three global submission slots;
each slot waits for its actual completion before reuse. Output backbuffer indices
remain separate from those slots. Texture transfers use a second recording lane
per slot in the same queue executor; these lanes add no output or history banks.
Their small graph declares only actual mapping/copy sources and targets. Main
imports the sampled views independently of pending transfer payloads. Accepted
transfers update the streaming ledger immediately and remain in frame completion
even if later Main preparation or Present fails. OwnedView is used directly during initialization,
rendering, resize, options and capture; there is no view exchange
mirror or whole-device drain for selecting another retained view. Native SDK
selectors preserve separate viewport contexts; actual reset/reconfigure and
presentation reuse still require their completion boundaries. Geometry SRV banks
retain resolved resource ownership and slice ranges; only changed bindings are
rewritten after slot completion. Resident descriptor publication transfers the
prepared bank snapshot into the destination heap. Feature preparation/admission
uses the target view; main lighting status and per-view geometry/HLOD history do
not read a parallel Impl service mirror. Hero retirement drains samples, resets
completed native work, then folds statistics once.
Volumetric compute owns a single root table with 41 descriptors, including its
27 local bindings and the light/shadow inputs it actually consumes. Its compact
descriptor bank belongs to the waited submission slot; raster consumers keep
their existing scene bindings. GPU completion and resource leases govern reuse.
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
not acquire a history read. Per-view projection retains its actual previous camera
across skipped views and ordinary movement. One perspective-correct unjittered clip varying supplies
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
Runtime projects status and persistent snapshot particle births before freezing
that batch. They follow the decoded birth prefix in the same retained vector;
family preview selects the snapshot suffix without changing its acknowledgement.
Renderer consumes explicit births and does not regenerate them from snapshots.
The native-free instance lookup/content identity helper belongs to RenderData;
Runtime evaluates only status-bearing owners while Renderer prepares its shared
scene/pose sample. Both retain the same identity,content and motion-epoch rules.
An explicit zero sequence supplies the held clock without a batch; legacy callers
that omit the clock retain snapshot-tick stepping. Gameplay/rendered snapshot tick
and the common analytic camera clock are not replaced by the frozen target.

Each rendered package evaluates one display sample from the producer's actual
adjacent tick pair. Transforms, poses and endpoint lifetime transitions share that
sample across views. Endpoint samples reuse the caller's current snapshot for the
synchronous Render call; only interpolation owns new instance/pose arrays. Common
ribbon membership and lazily prepared camera/sun cull bases belong to the existing
prepared package. Package-owned Archer palettes feed bounds and canonical skin
compute. All actual character draw consumers use that cache; the unused128-matrix
compatibility cbuffer and legacy vertex deformation are removed. Qualified bounds
with no skin allocation encode no package consumer. Visibility omits their visible
and rejected output, still publishing zero indirect arguments; shadow preparation
omits their caster/receiver work and retains retirement of previous caster records.
Packet-owned instance lookup and sorted attachment IDs serve Fresnel and skin
demand across views, without scanning every effect for every instance.
CPU preparation reads ordinary host FrameConstants and the package-owned Archer
palettes, never the write-combined frame upload. Cluster preparation receives the
same host values. Constants are copied once after graph setup and before Execute;
the waited submission slot still owns the GPU bytes through completion.
Mirrored shader prefixes share the FrameConstants wire layout checked by host
size/offset assertions, and upload offsets follow its alignment.
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
SaveStore writes SettingsDocument and ProfileData; SettingsData is only an
effective simulation/UI/audio projection. Legacy settings are migrated inside
the reader, without a separate public legacy persistence API.
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
Debug frame/completion histories and transition memory snapshots are collected
only when the existing timing-stream capture is enabled. Explicit Debug snapshot
queries may read current memory; normal frames do not build that history.
GetDiagnostics and its Runtime UI telemetry consumers are absent from Release;
actual monitor/HDR/settings receipts remain independent.
Installation verifies staged executable/pack copies outside the game process;
normal startup still checks the compiled content identity against its opened pack.

DirectXTex serves content production and development HDR capture. The Release
renderer neither links it nor requires it in a runtime-only configuration.

Jobs submission acts through a creator-bound TaskGroup and returns admission
success. TaskGroup owns cancellation and joining; task labels, per-task numeric
handles and scheduling priority are not exposed.

Experiment executes its validated spec and finalizes artifacts in one process;
CTest or its invoking tool owns the workload timeout. Static timeline commands
are passed directly to Runtime. Normal Debug, Release and Experiment startup use
the linked content pack. PreparedContent pins its opened pack, simulation rules,
renderer catalog, audio and VFX recipes until exit. An explicit development
`--generation` selects an offline comparison snapshot for that run only. Pending
IO and GPU work retain their actual backing owners through completion.

## Public and Private boundaries

Each module's `Public` directory is its supported cross-module API. `Private` is implementation detail. Tests should prefer public behavior; direct private testing is acceptable only for an existing, legitimate private unit.

## Build isolation

Material graph validation/optimization/HLSL generation is the RenderData-owned
`hs_material_compiler` target, enabled for content tools. The material shader and
render package producers link it. The compiler depends on the bounded material codec leaf; the broad
`hs_render_data` target aggregates package, material and texture leaves alongside
runtime codecs without depending on the compiler. Compiler or leaf edits therefore
do not relink unrelated producers that consume only another leaf.
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
RenderPackage serializes shader logical IDs without consuming DXIL bytes; its
recipe tracks that semantic input only. Generation admission verifies the actual
shader products consumed at runtime. Texture, geometry and material payloads keep
their real byte dependencies.
The Apps-owned `hs_asset_pipeline` static library compiles the shared Action
implementation once. Producers remain separate executables, so a producer-specific
change does not invalidate unrelated recipes. Each process hashes its own executable;
there is no intermediate recipe manifest or post-link preparation target.
Action receipt snapshots pin their ProductSet with the existing OS file-lock
contract. Upstream path values share that owner until consumption ends; publishing
another action cannot release another reader's pin. GC and repair respect the
stable per-set lease anchor. A short shared mutation lock covers receipt/pin and
selected-pointer/manifest acquisition; immutable installed snapshots require no
new lock-file writes.
Material and runtime DXC compilation consume hashed private source/include
snapshots; generated includes are owned by that producer stage. Saved runtime
shader input closures permit cache hits without dependency-scanner processes.
Environment producers derive consumed paths from frozen metadata and hash the
captured bytes they cook. Shader process waits share bounded termination handling;
uncertain process exit preserves its stage instead of racing cleanup.

CharacterContent imports one CharacterIR and directly emits final character
metadata, LOD0 geometry, named animation and DDS products. Normal builds do not
exchange native meshbin or material JSON sidecars through a second producer.
FBX, animations, sibling FBM textures and monster PBR inputs are copied into the
producer's private stage and checked against the input digests before native
import/texture decoding. Those consumers use only the staged paths.
ContentLinker reads exact character/material/texture snapshots, retains checked
bytes while validating their bindings, and reuses the prepared metadata. Explicit
CharacterInterchange compatibility export remains a separate developer leaf.

The low-level configuration is:

```text
HS_BUILD_RUNTIME=OFF
HS_BUILD_CONTENT_TOOLS=OFF
```

It builds Core, Jobs, GameDomain, GameRules, Gameplay and Presentation without native runtime/content prerequisites. With `HS_BUILD_TESTS`, the low-level tests use RenderData header values and the logical graph compiler without JSON, content or package producer dependencies. Neither path needs ImGui, Agility SDK, PIX, D3D12 Memory Allocator, runtime fonts, DXC, DirectXTex or FBX SDK. Shader infrastructure exists only when Runtime or content tools need it. Runtime and content options default to `ON`; tests default to `OFF` outside test-enabled presets.

## Windows verification

Supported builds use single-config generators, as the Ninja presets do. Configure
rejects multi-config generators so Release cannot expose developer targets whose
implementations are excluded from the shipping configuration.

Debug and Release executable builds prepare the same fixed pack incrementally
through `hs_shipping_image`, then link its small generated manifest. Runtime
installation copies that pack in both configurations. Ordinary builds do not
copy a runtime Generation; `hs_content_generation` is an explicit offline/native
fixture snapshot target. Runtime-only library builds do not need that snapshot.

`cmake --workflow --preset verify-core` builds and runs the selected CPU
contracts, including simulation determinism, input admission and graph ranges.
Gameplay keeps MSVC `/fp:strict`. These checks do not replace D3D12 or GPU execution.

`verify` shares the ordinary `Build/msvc-debug` tree. There are no separate
validation, GPU-validation, portable-test or ASan presets. D3D12 validation is
selected by the existing Debug `--validation`/`--gpu-validation` arguments;
native fixtures retain the mode required by their contracts. GPU cases cover
multi-queue execution, residency and owned-view lifetime.
Test executables and development hosts are excluded from default ALL builds;
verification builds only CTest's selected `test_prep/*` dependencies, or
`test_prep/all` for the complete workflow.

## Mechanical enforcement

`Tests/check_architecture.cmake` scans production includes, Gameplay graphics API knowledge, cross-module `Private` access, and major `target_link_libraries` edges. It intentionally uses CMake text checks; no parser framework or second build graph is needed for the current rules.

Code, target links, and executable tests are the source of truth if this document becomes stale. Verification policy is defined by the applicable `AGENTS.md`.

Actual texture copies use the executor-owned Copy recorder in its completion slot. Tile map/unmap runs as an external queue operation only when mappings change; a resource-version dependency orders the subsequent recorded copies. Mapping-only batches still receive a real queue completion. Development upload timing has at most two actual stages in its separate query storage; Release has no such capture storage.

Each graph pass stores one closed execution value: void/result worker recording, owner recording, or a shared external queue operation. The existing topology kind identity is preserved; external operations retain their shared owner through accepted GPU completion. Runtime ends and joins its owned simulation/render threads before allocating final diagnostic output after the main loop; settings OOM is a fail-stop result, not an edit-replay state.
