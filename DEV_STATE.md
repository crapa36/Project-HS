# DEV_STATE

Record only information the AI needs to implement or resume work: current code state, contracts, verified test receipts, unresolved technical causes, and next implementation steps. Never add tasks for the user to inspect, approve, playtest, listen, assess visuals, or measure hardware. The AI owns executable validation. Replace superseded state instead of appending history.

## Current technical state

### Elin supplied-base part modeling (2026-10-01)

Separate source work is under `ContentSource/Characters/Elin`; do not confuse it
with the older Archer migration below. Current editable checkpoint:
`Parts/02_HeadFace/Source/HeadFace_Study_v41_OralStudy.blend` relative to that root.
It contains the continuous body/head master plus eye-aperture, spherical eye,
pupil and eyelid-wrap studies. They are not final expression-ready geometry.
The prior body-only checkpoint is
`Parts/01_BaseBody/Source/Body_Fit_v13_RelaxedHands.blend`.
Both body and head derive from the supplied `AnimeBaseMesh.fbx`; its unchanged
normalized coordinates remain in the Basis key (8088 body vertices, 8078 faces).
The first combined assembly and v03 shoulder study are rejected checkpoints.
Current shoulder pose replaces the abrupt armpit rotation; arm centerline, leg
cross sections, calf depth and head registration are separate reversible keys.
New reversible corrections lower the shoulder roof, restore 160 axillary vertices
with constrained source-form ARAP and relax
both hands using axial roll plus individual finger bends. The old outer-canthus
lift and neutral upper-lid closure keys are stored but disabled in v25 onward.
Source normal detail is available in a separate study material;
its convention and final strength are not yet qualified for final textures.
No part is finished. Do not assemble clothing, author the final rig, or export
this checkpoint as a finished character.

Latest direct correction: medial eyes beside the nose are too recessed and cheek
form is unnatural. This supersedes peripheral part polish. `MedialCheek_v41/audit`
identifies the old EyeAperture Z field moving malar/lower-cheek vertices20–21.6mm;
the later sheet key leaves many of those vertices untouched. The medial depth
step is mainly inherited nasal geometry, not posterior movement by the eye keys.
`eye_depth/Candidate_070` is an isolated coupled depth-flattening comparison:
iris/front poles and all XZ preserved; neutral intersections0; blink invalidated.
It is NOT integrated or sufficient to fix the cheek. A connected source-rest
reconstruction is in progress using RegisteredPreEyeForm and the070 rim targets.
Do not integrate the failed `OrbitalDesign_v40/CurvedGuide` (8 cage reversals),
or claim the earlier brow/neck studies resolve this latest complaint.

`Reports/package_reread.json` records all 43 package text files; archive integrity
and numbered-image review evidence are separate reports. Added user workflow is
preserved in `Reports/User_Modeling_Workflow_20261001.txt`. All documents and
images remain requirements/reference evidence; missing manifest payloads and
inconsistent sheet metadata are unresolved, not fabricated approvals.
The fresh direct-ZIP audit is `Reports/Reaudit_20261001/`: 100/100 extracted
payloads match, 43 text files reread, 56 images decoded. Its requirements report
orders work by modeling/visual quality and dependencies; silhouette IoU is only
a diagnostic. User requested another instruction refresh: the new direct archive
integrity receipt is `Reports/Reaudit_20261001_Reminder/archive_integrity.json`
(100/100 unchanged), with current priority/order in `WORKING_RULES.md` in that folder.
Keep latest cranial/face-form priority; isolated shader studies and expression
experiments do not establish completion of upstream modeling or final materials.
`REF_Body_Orthographic` contains packed, locked front/back/right
sheet images, aligned to Z=0 soles and Z=1.75 scalp. Original pixels are unchanged;
opacity is 100 percent behind geometry, orthographic axis views only, render excluded. The alignment
receipt is `References/Body/blender_alignment_validation.json`.
Packed `REF_Head_Orthographic` uses `References/Head/registration.json`; side chin
depth and back alignment are provisional drawing registrations. Head references
are visible, body reference collection hidden. Use all three comparison modes:
opaque model alone, opaque model with 35 percent foreground sheet, and X-Ray
vertices with an opaque background sheet. Do not rely on X-Ray alone. Source
Basis must not be edited; exact orthographic rotations are required for axis-only sheets.

Current priority is cranium/face form, explicitly clarified by the user. The
reversible `Elin_ThreeViewHead_Form` coordinates body, eyes, lashes and brows;
it broadens the lower jaw and corrects forehead/occiput depth while keeping
chin/scalp anchors. `Reports/HeadRevision_20261001/v16/` has exact orthographic
front/side/back and three-quarter renders plus contour comparisons. Front jaw
width error at Z1.54 changed from -18.3 to -1.0 mm; side contour offsets at Z1.62
changed from +13.6/+9.6 to +2.3/-1.0 mm. These are reference diagnostics only.
Evaluated open-eye rim has minimum +.2798 mm clearance, zero inside samples,
against updated eye center Y=-.07635. v19 changes connected outer-ear flare with fixed root support;
local evaluated triangles have no degeneracy, reversal or nonadjacent overlap
candidates (`Reports/HeadRevision_20261001/v19/ear_geometry_check.json`). Head04
and Body02 anterior-neck contours differ about 20 mm. Latest direct correction
explicitly requires bringing the neck forward; v24 applies a smooth 23 mm maximum
anterior shift, with v28 posterior-junction fairing inside the same neck key.
The slanted Head04 lower cutoff is not an anatomical neck contour.
Source recessed-pupil rings had become radially inverted after earlier
spherical projection (140 inward triangles). v20 restores monotonic concentric
32-spoke rings while preserving topology/UVs; evaluated Eye has 0 inward or
degenerate triangles. Pupil centers are X=+/- .042, Z=1.6075, circularity >.9998.
v25 replaces the prior flat upper-lid form with the traced sheet aperture;
v27 reshapes the existing iris/pupil rings to vertical ovals. v26 reduces the
posterior skull bulge without changing scalp height. v32 fairs the temple while
protecting ear support and blink control vertices; v30 flattened the ear and is
rejected, v31 overprotected the transition. v33 thickens the upper lash strip,
shortens downward tips and lifts the free outer fringe. These are source-derived
shape keys, not replacement meshes. v38 replaces part of the temporal key with
an orbital-valley correction while pinning eyebrow anchors; v39 adds the local
nose-base form at .8. The old submental lift and its v36 replacement are disabled:
native opaque inspection exposed a pinched jaw/neck transition. Further neck
diagnostics are isolated under `NeckAudit_v40`; preserve the user's forward-neck intent.
v40 installs freshly qualified per-eye blink samples from
`Reports/HeadRevision_20261001/Blink_v38`. `Blink_L` means +X anatomical
left; `Blink_R` means -X, both saved zero. 33 interpolation values have zero
triangle-interior globe penetrations. The v39 nose change is outside the exact
246-body-vertex two-ring blink neighborhood; all protected positions are unchanged.
Full-closure evaluated boundary gap is below .001 mm. Actual installed driver
weights agree within 6e-8. `v40_FormReview/saved_revision_validation.json` records
unchanged source Basis/topology/UV for body, eye, lashes and brows, zero evaluated
degenerate triangles and zero nonadjacent body BVH overlap candidates. Uniform
clay five-view renders disconnect the source normal map; source materials remain
unchanged in the authoring file. Static checks and blink do not establish final
face quality: brow/temple transitions, nose/jaw forms and mouth still need work.
This is local facial deformation, not the final rig. Mouth audit found an existing
116-face/127-vertex closed oral cavity; ordered lip loops and 27 real UV split
edges are recorded in `Reports/HeadRevision_20261001/MouthTopology`. v23 adds
reversible `Elin_JawOpen_8deg_Study` and `Elin_ClosedSmile_Study`, both saved zero.
Neutral preflight error is exactly zero against the independently checked v02
candidate; 25 mixed states have no detected head crossings or degenerate triangles.
Source faces 401/4330 have real 17x stretch and become visible inside the mouth;
local oral-wall correction, teeth, tongue and interior materials remain unfinished.
Evidence and limits: `Reports/HeadRevision_20261001/MouthMotion/IndependentQA`.
Offline MouthMotion v03 distributes corner strain (17.34x to 4.08x patch maximum)
and passes 25 mixed-state checks, but its full open smile becomes O-shaped.
v35 integrates v04's restored smile-aperture candidate; v37 adds local MouthOpen,
Smile drivers (J, S, J*S). v40's 25 combinations pass head-crossing/degeneracy
checks, but teeth-fit testing exposed deep-chamber motion faults. An isolated
OralAssets correction fixes chamber roof/floor articulation while preserving
outer lip motion. v41 integrates that correction plus separate teeth/tongue.
OverbiteStudy_v3 extends six anterior upper crowns and shifts the lower arch
2.5mm posterior; this fixes the original hidden-tooth presentation without
shrinking assets. Integrated25-pose collision/degeneracy and source preservation
checks pass in `Oral_v41`; full-face open/mixed renders inspected. Tongue remains
mostly occluded; phonemes, full rig, final oral UV/materials remain unfinished.
Hair v03 full-hair assembly is not ready to integrate. FrontFringe v05 unifies
the crown and primary branch topology. v06 primary shapes remain too broad;
v07 failed sparse frontal coverage; v08 improves coverage but retains an angular
crown/forehead turn and19 shared-root crossings. v09 redesign is isolated.
Innerwear v06 replaces nearest-surface shrinkwrap and wrong inner-arm ray hits
with source-topology torso provenance and stable garment coordinates. It is
isolated. v07 improves band curvature, rear coverage, gusset and straps; root
inspected all4views. Stiff cup form and front lower leg-opening standoff remain;
not integrated. Evidence: `Reports/InnerwearReference/v07_review.md` and
`Parts/04_Innerwear/Review/v07/`. v05 causes remain in `v05_rootcause` for continuity.
`Review/Head_ThreeView_Comparison_v21.png` under Part02 compares identical-camera
clay renders. `Review/ColorStudy_v21/` is temporary vertex-color/light exploration,
not final maps or saved production materials. Separate
`Source/HeadFace_EyeMaterial_Study_v01.blend` and `Review/EyeMaterialStudy_v01/`
add UV-anchored procedural brown iris gradients/fibers without changing geometry
or original UVMap. This is isolated look development, not final export textures.
v23 reopen checks reproduce the qualified mouth candidate within .000112 mm;
receipt: `Review/mouth_study_reopen_check.json`.
`Reports/HeadRevision_20261001/source_uv_coincidence.json` screens exact UV polygon
coincidence only: body has no exact duplicates (partial overlaps untested), both
eyes share 608 pairs, brows have 20 four-way groups, and lashes have no UV layer.
Final unique UV0/UV1 and face asymmetry requirements remain unfulfilled.

Current three-view renders and registered overlays are in
`Parts/01_BaseBody/Review/`. Before the last hand correction, diagnostic silhouette
IoU was front .9030, side .9018, back .9020; these do not prove shape fidelity or
deformation quality. The static
evaluated-mesh audit found no degenerate faces or edges shared by over two faces.
`Reports/BodyTopologyAudit/` records intentional open eye boundaries, non-spherical
source eyes, small mouth cavity without separate teeth/tongue, and missing lash
UV. `Reports/ArtReview_20261001/v13_intersection_report.json` checks the v13 body
control cage (16,136 triangles) and evaluated subdivision surface (64,584): no BVH
overlap candidates, with the overlap/intersection routines qualified on synthetic
crossing triangles. Combined shoulder correction has no tangent stretches below
.5; one anterior shoulder patch retains area compression (.445 source ratio).
These are static-pose checks, not deformation acceptance. Facial articulation and
final UV tests remain unexecuted. Next implementation: complete silhouette/form
review, independently review the separate hair study, then continue layered parts.
Do not import a hair candidate without inspecting front/side/back/three-quarter
renders. Engine/runtime files were not changed by this part-modeling work.

### Engine and content migration (2026-10-10)

Requirements: `Docs/Plans/ENGINE_RENDERER_V7.md` and
`Docs/Plans/CONTENT_GENERATION_DIRECTIVE.md`; execution map:
`Docs/Plans/IMPLEMENTATION_EXECUTION_PLAN.md`. Whole-plan completion remains
unclaimed. The migration from `fecc13c0f0922c2b70b88a251f50ceea5d93a6a5`
is staged on `main`; its HEAD remains `5ee6bf8d41c1ea8114ee12c0583d671fd25b8ff2`.
Preserve the source commit and `codex/engine-v7-backup-20261009`. Current fixes
remain unstaged above the migration; do not commit or rewrite either branch.

#### Current source contracts

- Local-probe native layout identities and static shadow bounds are cached by
  the retained immutable scene, instance generation, foliage density, complete
  material-page state and authoritative lights. Mutable inputs still validate
  every frame. Capture reuse keeps the original revision serialization.
- Probe recapture waits on cube users separately from the immutable zero-count
  metadata. Full graph/resource completion still gates replacement and release.
- Owned views only override and restore the devtools visibility flag; per-frame
  copies of the complete renderer/content product configuration are removed.
  Visible environment textures request their shared material once per frame.
- GPU cluster cuts traverse validated roots and skip inactive subtrees instead
  of scanning every node and repeating ancestor refinement. Immutable successor
  links and the first root reuse reserved GPU words; record sizes and stable
  group/cluster identities remain unchanged. Preparation proves full traversal
  coverage; shaders retain bounded range/generation/residency/error checks and
  complete-cut publication. Quality/error thresholds are unchanged.
- Settings hit testing uses message-time client coordinates and the renderer's
  reference UI letterbox transform. Labels, values and status summaries are
  Korean; canonical persistence tokens and detailed native diagnostics remain
  intact. Unknown settings extensions retain current-file values before backup.
- Unavailable native settings are filtered from cycling and guarded at Runtime
  command consumption using the same renderer capability catalog. Controls with
  no available alternative are disabled; saved unavailable intent can be turned
  off. RT presets require their actual constituent modes, including automatic AO.
- Core, RenderData, Presentation, Renderer and Runtime retain their Public API
  boundaries. Internal environment-layout and generation-lock targets compile
  their existing implementation once; unrelated producers avoid whole
  Presentation/AssetManifest recipe closures. Compiler/include authentication,
  source-hash semantics, 60Hz and strict gameplay floating point remain intact.
- Source/recipe plus CMake/Ninja control immutable Action ProductSets and scoped,
  atomically selected Generations. Runtime/fixtures resolve selected manifests,
  retain reader leases, and reject missing/corrupt dependencies. A manifest-only
  package export was verified for1021 products without destination Actions/CAS;
  that receipt predates current source. Content performance phases5–6 still need
  the uncovered current before/after evidence; the retained incrementality
  receipts do not provide an old-engine before/after speedup baseline.
- Typed MaterialIR and versioned packages feed common opacity, deformation,
  Surface, BSDF and sidecars. Authored terrain Gaussian/POM/RNM, environment
  HDR/probes, HLOD membership and actual OMM geometry/material dependencies are
  source-connected. Decal Surface has independent shader/program/default Actions,
  typed Fracture/Organic graphs, receiver policy and explicit RasterOnly metadata.
  Actual admitted commands lazily allocate view-owned targets; pass295 modifies
  base/normal/roughness/emission before common lighting/AO/reflection/native
  guides. Empty frames restore original Surface descriptors and read no exports.
- Public rendering uses explicit RenderFrameInput, Core RenderViewInput and
  RenderSceneRef. Every keyed view owns targets, exposure, TAA/HZB/DRS, services,
  native reconstruction context and successful previous camera/origins/instances/
  slot generations/poses. GPU scene, assets, TLAS, SDK sessions and canonical
  immutable deformation remain shared. Main alone presents and owns FG/latency.
  Rejected preflight leaves publication/result identities unchanged; actual
  submissions consume package identities even if a later view fails.
- Cluster cut/readback/history banks and HLOD selection/history/demand are keyed
  by view/session. Only successful matching camera/scene/extent generations
  reuse history. Immutable scene pages and the128-slot physical pool stay shared;
  complete nonoverlapping HLOD groups reserve optional slots before suppressing
  original members. In-flight uploads retain all queue requirements.
- HLOD cooking packs compatible material/tint/selector fragments into pages,
  preserves exact proxy ranges and uses four-object leaves. Parents above32
  distinct pages retain their complete child forest before error certification;
  aggregate pages are optional. Mandatory per-asset roots remain pinned.
- Previous instance snapshots are per-view, success-only and generation-qualified.
  Previous transforms are rebased using local+(oldOrigin-currentOrigin), with
  the origin delta computed before adding local coordinates. Surface ViewOrigin
  and existing eye-relative service matrices have separate matching contracts.
  Runtime aiming receives the actual unjittered camera result. Canonical skin
  palettes sample independent named/legacy/retained poses and retain GPU leases.
- Capture(CaptureRequest) routes real keyed color/Surface/geometry/visibility
  readbacks and restores the active view. Geometry/visibility scratch evidence
  rejects a bank overwritten by another view. Legacy span Render and the
  RendererOptions transaction helper are private. GraphicsSettingsRequest and
  copied status/capabilities retain owned requested values and actual resolutions;
  Runtime keeps window acknowledgment, confirmation/rollback and persistence.
- HDR capture retains the actual per-view reconstructed floating-point output
  before post processing and writes Radiance RGBE plus submission/package/tick
  metadata. Internal and native reconstruction paths publish strong allocation
  leases after successful execution; resize/options/resident reload invalidate
  them. The current joined build/GPU receipt records successful native execution;
  full-gate and package acceptance remain separate.
- Graphics settings preserve canonical Auto intent and transport the resolved
  native output separately. User/System generation0 is rejected, receipt data
  is staged before native preparation, and failed monitor changes suspend render
  until window rollback. Material quality controls detail reach and SSS samples;
  external reconstruction releases native resolve histories, and PostB exists
  only for Bloom/Outline. Contradictory grid AA-Off/external temporal requests
  are rejected before provider preparation.
- Runtime UI consumes category-qualified native capability options. Streamline
  selectable SR/RR/FG/Reflex plugins bootstrap before device creation; evaluation
  and per-view histories remain lazy. Device admission enumerates alternative
  providers/qualities/modes without inactive evaluations. Actual receipts include
  native HDR output and NRD radiance/AO/SIGMA selection. The current native batch
  now has zero-error lighting and RR execution; FSR RR remains device-unsupported
  with internal reconstruction, while DLSS RR is active natively.
- Source validation belongs to each selected producer. GameData payloads include
  their schema input; simulation uses nine documents and presentation two.
  Portable/lifetime tests prepare actual producer RecipeIdentity manifests.
  Environment no-op freshness uses metadata/size/header checks rather than full
  DDS hashes; integrity consumers and repair retain SHA verification.
- FrameGraph owns queue DAG/barriers/ranges/aliasing and bounded transient pools.
  Canonical CPU descriptors publish to waited shader-visible frame banks. Native
  feedback uses MIN_MIP reserved resources/tails; software uses reduced mip
  resources. DirectStorage terminal work and all-queue resource retirement are
  connected. Native SDK/SM6.9 OMM/SER paths are implemented with authenticated
  pinned inputs; execution remains source-specific and hardware-dependent.
- Diagnostics stores240 frames/512 passes, reads timestamps only after fence
  completion, separates validation and views, and reports calibrated critical
  paths/percentiles and unavailable native counters honestly. DXGI budget/usage,
  known view allocations and shared deformation identities are separate; opaque
  native SDK allocations are not claimed as complete accounting.
- VFX capture occurs before entity removal and retains actual identity/pose/
  generation closures. Entity Surface uses authored3D noise/STBN/gradient and
  coverage confidence; arrow/thorn geometry and world shell emission are connected.
  DLSS fused RR receives actual responsive guides and one fused evaluation.
  Static audit reports534 connected source paths and0 pending. Current native
  execution is recorded, while runtime_ready and high-quality admission remain
  gated on the full gate and package acceptance.

#### Consolidated review contracts (2026-10-10)

- Snapshot acquisition claims a slot before rejecting publication sequences at or
  below the held current sequence. Sequence numbers are strictly increasing for
  the exchange lifetime; simulation ticks are not publication identities.
- Additional retreat uses the existing disc sweep before firing. Projectile
  redirects recompute contacts and walls over the remaining tick; straight
  piercing retains ordered hits. A tick permits 64 redirects, then resumes from
  the last verified contact next tick. Original casts retain their actual saved
  cooldown budget while live projectiles can still spend it. Exact enemy overlap
  uses stable opposite separation directions without imposing hard collision.
- DRS preparation and control share inclusive integer extent bounds. Feasible
  unaligned extents outrank 8-pixel alignment; invalid provider/capacity domains
  are rejected. Per-view history and capacity ownership are unchanged.
- Temporal reconstruction validates all contributing history taps. Spatial AA
  includes center luminance. Camera-facing VFX use the uploaded camera basis;
  secondary RT shadows honor the requested bit and dynamic entity emission uses
  the same generation-qualified surface inputs as raster rendering.
- Weighted OIT accumulation uses blendable RGBA32F throughout allocation, views,
  PSOs and capture; revealage remains R16F and existing weights are unchanged.
  The extra accumulation payload is 15.82MiB per FHD view; bandwidth cost is not
  inferred from format size. Stable instance buckets replace repeated scans while
  preserving canonical indices, foliage retention and per-view preparation.
- Development reload tracks observed and applied pointers separately. One CPU
  preparation future validates immutable captured inputs; stale/ABA results are
  discarded and failures retry with 250ms–30s capped backoff. GPU/VFX commit stays
  on the render thread. Pending I/O is not cancellable and shutdown joins it.
  Probe preparation uses the resident scene generation independently of selected
  shaders. Compatible full reload prepares immutable scene copies before commit;
  narrow retains the scene identity and incompatible full closures are rejected.
- Commit-pinned DirectXTex/D3D12MA fetches use full clones. Portable CPU CI includes
  render-data and asset-pipeline checks without runtime/content SDK prerequisites.
  Review decisions and conditional limits: `Docs/Consolidated_Review_Resolution.md`.

#### Current evidence and next checkpoint

- Work remains on `main` at `5ee6bf8d41c1ea8114ee12c0583d671fd25b8ff2`: staged migration equals source commit `fecc13c0f0922c2b70b88a251f50ceea5d93a6a5`, index tree `f545c39df35df3562abcb06f261ff821be794731`. The backup branch `codex/engine-v7-backup-20261009` records review checkpoints on top of that unchanged source commit. Main's index and working files remain in place, with fixes unstaged above the migration.
- The b0 table lookup failure was isolated by same-draw raw-SRV/direct-CBV probes. FrameConstants now uses root15 direct CBV, including private probe banks; immutable t18 occupies the former b0 slot without changing HLSL registers, heap stride or the 64-DWORD budget. All 2,077 sampled ROI frames were finite/nonblack (`Build/tooling/flicker-binding-fix.json`). Fence UINT64_MAX is rejected by Completed/Wait/Drain, preserving DeviceRemoved HRESULT instead of consuming unfinished readbacks. The original DeviceHung cause remains unconfirmed; these fixes do not establish all-scene stability.
- Settings use message-time click coordinates and the reference letterbox transform, Korean labels/values, shared UI/Runtime capability guards, working 144/165/custom caps and current-file extension preservation. Unsupported HDR/DLSS/FG cannot be enabled. Presentation/settings-store checks pass in the current full execution; title and float display fixes are included.
- Unhandled WaveTimer/pickup-idle owners no longer fall through to white legacy ground commands. Typed pickup bursts replace legacy collection layers only after successful conversion; malformed/unsupported fallback and ballistic debris remain. `Build/tooling/vfx-final-comparison.json` records white pixels 117756→1986, zero in scene rows 200–899, with unchanged depth/GBuffer/LightingHDR. The eight-frame Enhanced pickup fixture confirms 4 events/4 flashes/4 ribbons and zero drops/unsupported/validation errors (`Build/verification/vfx-pickup-enhanced-8.log`). Package-wide VFX qualification remains incomplete; keep `runtime_ready=false` and admission flags truthful.
- Review decisions cover 28 IDs: 18 corrections/verification improvements and 10 conditional/retained decisions (`Docs/Consolidated_Review_Resolution.md`). Existing core 10/10 and portable 23/23 pass (`Build/verification/review-portable-final.log`). Full execution `verify-20261010T094257223Z-4768` passed 76/77; only camera fixture occlusion/clipping failed. Isolating those inputs retained the original aspect/extent criteria, and camera/analyzer passed 2/2 in `verify-20261010T101003093Z-34424`. These are combined valid receipts, not one 78/78 full-gate run.
- Actual FP32 OIT surfaces have 43,431 nonempty pixels, 22-layer alpha 66000 and 64-layer alpha 192000; accumulation/revealage/composite nonfinite and out-of-tolerance linearity mismatches are 0 (`Build/reviews/consolidated-20261010/oit-after-analysis.json`). This resolves observed FP16 saturation, not reproduced Inf/NaN. Added storage/bandwidth cost remains unmeasured.
- Development reload separates selected shader and resident scene generations. CTest registers the single-config shader reload; hidden-window shutdown targets only the child PID's `ProjectHS.Window`. `verify-20261010T102630399Z-33072` passed shader/material/local-probe 3/3. Full-resident Runtime scene binding and closure admission passed expanded shader reload/watch/architecture 3/3 in `verify-20261010T104523795Z-9732`: narrow frame59 keeps the original resident ID, incompatible layout is rejected before commit, full VFX frame156 uses the new ID. Native exit0/validation0/static scene uploads2; original pointer preserved. Prepare/commit, immutable snapshot leases and strict scene/page checks remain; no generic transaction framework.
- Latest recorded Release runtime-full generation is `86c6bb06570d2b8c6b4199eb0418247b776b482b0dd1aaafd9cd6498a70381f1` with 3,680 products. Two FHD Ultra 240-tick runs, excluding 60 warmup frames, have render-scope p50 60.5075/60.7185ms and p95 68.699/68.683ms (`Build/reviews/consolidated-20261010/performance-after.json`). Lockstep drains all GPU queues each frame inside this scope; final readback is outside it. Release optimization/RTX4080/validation and GBV OFF are confirmed. These are not display FPS or a controlled source-equivalent A/B; 144FPS is not established. Native reconstruction/GI SDKs remain disabled; software-hybrid GI uses its probe fallback.
- Final Release ProjectHS/render runner rebuild and 3,680-product verification passed (`Build/verification/review-release-completed.log`). Normal Runtime's existing 60-second input ran FHD Ultra without lockstep, VSync or frame cap: exit0, 1,021frames, checksum15682705976600721736, validation/dropped inputs/dropped presentation/unsupported VFX0 (`Build/verification/review-final-normal.log`). The 456 Render scopes with300<=tick<1818 have p50 55.37ms/p95 60.41ms, excluding CPU preparation before Render; no actual present-interval or144FPS claim (`Build/reviews/consolidated-20261010/normal-play-after.json`).
- A02's 1,200 randomized order/range cases and CPU microbenchmark cover dynamic classification only. A04's approximately 0.082ms sums two separate function p50s; it does not justify a broader dirty-bank cache or prove an FPS gain. A08's 2.513–2.674s measures whole CLI Verify for3,680 products; render-thread VFX/commit cost and shutdown join remain unmeasured. Exact boss RT secondary-hit and dedicated TAA/FXAA edge GPU oracles are unexecuted; conditional decisions remain in the 28-ID document.
- Next: investigate normal-play throughput separately from lockstep drains and obtain paired pass-level measurements for remaining CSM/GBuffer/GTAO cost. Full VFX changes combined with scene shader/geometry/layout changes require new scene projection and are currently rejected before adoption. Retain strict prepare/commit/abort, GPU waits and ownership checks. Use `Build/tooling/performance-investigation-current.md` and the consolidated review reports for unresolved technical work.

### VFX v4 runtime execution (2026-09-27)

Authoritative inputs: `ContentSource/VFX/production_spec.v4.json` (196 effects, 534 outputs, 72 upgrade bindings), 13 original DDS and seven authored meshes. The source archive lacks the v3 `components[]` schema, so truthful v3 migration remains unavailable. Cooked format 5/schema 4 loads 75 motion opcodes, but `runtime_ready=false` must remain until every production output and live binding is executable. Recipe/decoder support alone does not prove live ingress.

Current source alignment is recomputed from the authoritative spec: 196 effects, 534 outputs, 72 upgrade rows and 69 nonempty bindings; the current spec SHA matches the audit, with 534 source paths connected and 0 pending converters. Status/stamp shards, ribbons, motes, TAA and hero volumes are connected in source. `admission_verified=0` and `534 joined execution pending` describe static coverage only; they do not establish 534 GPU tests or individual runtime qualification.

The normative `incoming_arrow` owner seam remains a separate authored-owner contract: spec21665 requires `gameplay_projectile`, `projectile_hitbox` and `fall_lifetime`, while adapter925–957 supplies synthetic cosmetic descent from the authoritative scheduled area and ledger185 records no physical projectile. V7 §10 preserves existing VFX meaning and does not require changing gameplay into new physical projectiles. Treat this as an authored V4 owner-contract limitation, not an unimplemented engine feature.

Current integrated evidence: `Build/verification/verify-20260927T121907542Z-3320.log` passed 42/42 after the alias, DamageTrail-owner and SourceTarget ribbon slice. Actual `ProjectHS` GPU-validation captures report unsupported typed inputs0 and D3D12 validation errors0: ArrowRain mask2 tick35 has ribbon peak18 versus no-upgrade peak1; Retreat mask4 tick25 is clean; Piercing mask8 is clean at ticks20/55; Trap mask2 tick80 does not isolate the alias in its cluttered screenshot, while the simulation/snapshot/adapter test proves the persistent owner. Bleed/burn GPU tick capture remains absent because no tick event was produced.

The retained strict payload and GPU receipts remain source-specific evidence for their exercised paths; static source alignment does not replace runtime qualification or add unexecuted GPU coverage.

Current targeted corrections additionally passed the16/16 VFX contracts,3/3 native routing/integration checks and1/1 expanded pickup showcase recorded above. These receipts qualify their exercised paths; they do not qualify all534 outputs. Keep `runtime_ready=false` and admission flags unchanged until package-wide production outputs and live bindings are individually qualified. Use current source/specification and retained receipts for continuation; the previously referenced `Docs/VFX_V4_EXECUTION_CONTEXT.md` is absent from this checkout.
### Metre scale, contact and slime motion (2026-09-16)

User confirmed 1 unit = 1 metre and standing character height 1.75m. Player scale is .97174913; cooked bind body measures 1.750000057m after scaling. Normal slime body heights are .8/.9/1m, with per-role collision radii .63/.71/.79m. Tree/rock visual footprints match obstacle radii; grass retains cluster width with .15–.35m height. Projectile presentation originates at 1.05m for player arrows and .45m for enemy globes; simulation distances remain metres.

The archer cooker excludes Arrow_MAT geometry and grounds authored frames. Runtime corrects the final blended standing support surface: per-frame cooking alone left up to 16cm penetration during Idle/Run blending. Independent runtime-equivalent probes verify corrected sampled blends; Death keeps whole-body baked grounding with midpoint error -2.66mm to +5.46mm. Bow and body remain intact.

Audio master output mutes whenever the game window is not foreground, preserving user volume settings. Skills can cancel basic recovery after release while preserving already emitted projectiles and basic attack cadence. Tests cover instant/charged skills and focus/settings interaction.

Blender MCP regenerated the three slime animation sets: melee crouch/jump/headbutt, ranged cheek inflation/spit, and suicide slight growth/strain. Suicide cancellation replays the charge backwards from its current progress over 36 ticks, including redness and bubbling; attack recoil keeps facing for 18 ticks. Shared shader expressions provide frowns and death X eyes; body opacity multiplier increased .45 to .52.

Full gate: 24/24 PASS, `Build/verification/verify-20260916T014834794Z-31908.json`. GPU validation smoke/barriers: 2/2 PASS, `Build/verification/verify-20260916T015008036Z-11980.json`. Actual game capture `Build/Artifacts/metre-slime-20260916/game-close/capture_tick_360.png` has zero validation errors. Slime key-pose captures are in `Build/Artifacts/metre-slime-20260915/front-slime-review.jpg`.

Removed four unused generated build trees (msvc-debug, msvc-release, msvc-core-cached, msvc-core-depcheck), reclaiming 3,079,934,022 bytes. Current validation builds, source assets, registered tooling worktrees and verification receipts are retained. Cleanup receipt: `Build/verification/cleanup-20260915.json`.

### Camera, shadows and combat (2026-09-15)

Three nested, texel-snapped directional shadow cascades use 18/48/160m coverage and one depth atlas. The near tile has four times the configured side resolution (16x pixel count): 4096 at the 1024 setting, 8192 at 2048. Atlas allocation is approximately 80/320 MiB respectively. Projected coverage, overlap blending and tile-clamped PCF replace world-origin distance switches. All three slime bodies use alpha-weighted OIT; authored eyes/ornaments remain opaque. Melee alpha was regenerated through Blender MCP from its authoring script.

Wheel zoom preserves the authored 80–120% range and extends to 15%, smoothly lowering pitch to 18 degrees and raising the target to 1m. Window ground-ray aiming and presentation share the camera pose. `--camera-zoom=15|45|80` reproduces the live camera path for captures. One continuous ground slab covers the decorative tree perimeter while gameplay boundaries and grass masks remain unchanged.

Updated by the 2026-09-16 request: successful skills cancel basic recovery as well as unreleased casts; fired projectiles survive. ProcessInput observes the last completed tick, so only tick < release_tick removes the pending basic cast. A new press replaces previous buffered intent, instant skill release preserves the buffer, and charged skill release cancels pending intent or releases only its original slot. Swept obstacle/actor contact ordering blocks both sides' projectiles; melee, AoE and explosive derivatives use obstacle visibility. Applied damage-over-time remains active.

Verification: full 24/24 (`Build/verification/verify-20260914T161615783Z-12324.json`), low-level 7/7 (`verify-core-20260914T161832733Z-26836.json`), GPU smoke/barriers 2/2 (`verify-20260914T161442239Z-7736.json`). The later melee transparency correction passed content/render 3/3 (`verify-20260914T162319701Z-25120.json`) and GPU smoke 1/1 (`verify-20260914T162418005Z-15476.json`). Regression tests cover both sides of terrain, front-of-wall hits, explosion upgrades, charge slot release, stale buffers and post-release recovery. Captures live in `Build/Artifacts/shadow-camera-combat`. Source FBX/PNG/Blend files and useful validation receipts are retained.

### Environment v9

Native PCA Gaussianization + BC7 linear Gaussian terrain + per-mip inverse LUT are integrated. Terrain source tile size is 6m for soil/grass and 4.5m for gravel, with POM world-height scaling retained. Actual BC7 distribution tests improve RGB quantile error 72–85% over ordinary blending. Renderer uploads Gaussian albedo instead of the original terrain colour and adds one LUT.

Grass uses deterministic dense irregular clusters of 10–16 clumps within existing allowed regions. Terrain uses near/far randomized sampling and continuous material blending to remove ranked-layer borders and close-range repetition.

Blender MCP authored and baked three tree archetypes, four rocks and four grass clumps. Fourteen FBX packages contain 42 LOD meshes and seven collision proxies. Runtime uses baked PBR arrays and actual-camera LOD crossfades. A 32-bit position GBuffer replaced the half-float world-position path that caused shadow artifacts. Source/output details and approximation limits are in `Docs/ENVIRONMENT_V9_REPORT.md`. Terrain reference height/normal maps and analytic ambient lighting remain provisional relative to a production scan/IBL pipeline.

Latest retained verification: full 24/24 (`Build/verification/verify-20260914T052652698Z-7384.json`), GPU validation 2/2 (`Build/verification/verify-20260914T103953989Z-26220.json`), and normal render/asset gate 2/2 (`Build/verification/verify-20260914T104202695Z-22892.json`).

### Monster presentation

All three normal-enemy roles use the authored SlimeFamily assets and five animation slots; legacy Slime/Cactus/Swarm09 sources remain preserved. Full verify passed 23/23 and GPU validation reported zero errors for both barrier modes on a 300-slime overlap preview. Details are in `Docs/SLIME_FAMILY_REPORT.md`.

Automated validation establishes only the checks it executes; do not infer untested presentation quality from a successful process or zero D3D12 validation errors.

## Accepted baseline

- Slime eye-socket repair is the current accepted baseline.
- The relic roster contains 20 relics with no unresolved correctness hold; current tuning state is in `RELIC_BALANCING_PLAN.md`.
- Relic contribution is exposed in the character and result UI.
- Current UI text measurement, clipping, card layout, relic display, bars, and combat-HUD readability fixes are integrated.
- Latest verified RelWithDebInfo CTest baseline: `24/24` PASS (2026-09-14; receipt in the environment report).
- Accepted render/runtime validation gates complete with zero D3D12 validation errors.

Do not repeat completed relic census/tuning matrices unless a change invalidates their evidence.

Use source, tests, and current build output over this file if they disagree.

## Tooling qualification in progress

The requested candidate batch is not complete. Scope and retained evidence are under `Build/tooling/qualification-20260906/REQUEST.txt` and adjacent reports. Keep existing tool exclusions; do not repeat completed measurements.

The optional `msvc-asan-core` preset built and passed `simulation.determinism-core` against the 2026-09-08 checkout state; invocation and SDK/runtime prerequisites are in `Tools/tooling/WORKFLOW.md`, with receipt `Build/tooling/qualification-20260906/msvc-asan-core-receipt.json`. This evidence does not cover later source changes or establish a new full-project gate.

Tracy/MCP remains inconclusive: captures omit submitted jobs/wait zones and reported metrics need scrutiny. Its isolated instrumentation was removed; retained evidence and cleanup details are under `Q/tracy-cleanup.md`. CodeGraph did not establish a replacement benefit over Repowise; its isolated registration was removed. Debugging skills converged on bounded repairs without demonstrated benefit over the current workflow. TDD baseline/Matt runs completed, but the Superpowers arm remains pending; see `tdd-comparison.md`. `STATUS.md` indexes completed comparisons and remaining candidates. No batch-wide winner or final cleanup is claimed yet.
