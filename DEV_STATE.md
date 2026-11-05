# Current technical state

Working main d04cd4b8e058641fcf38e68b9ece31abdb8ea38c plus uncommitted v3 integration.
Current Debug runtime-full is48e7d6b19b7ce0b2020936ef95a78bce70bed954289fa47841682e67101e0389.
The v2/v3 directives and Debug1080p Ultra200-enemy60FPS goal remain incomplete.
Root owns the sole build/GPU workload. All automated/play/performance runs use
per-run master0 before audio initialization; user volume intent and audio work remain.

## Current AO decision and CPU preparation

CPU-only package pose/instance/VFX preparation runs before the frame-slot wait.
Resident-material validation and ready publication remain after streaming; slot,
LOD, ribbon, descriptor/upload reuse and accepted work lifetimes retain their gates.
Existing resize/native/multi-view cases passed before rollback. scene_prepare_us
includes both CPU preparation intervals; wait_us excludes the newly moved work.
This rearrangement has not independently demonstrated a whole-frame speedup.

Filtered-depth AO was rejected and restored to the original GTAO shader/resources.
The best candidate's measured AO work was2.367ms against1.967ms baseline; warm
foreground Debug1080p Ultra200-enemy FPS was40.548 against40.837. These single-order
runs do not establish a whole-frame regression or gain. No completed reverse-order
comparison is available. Opaque triangle reference at Medium36samples/frame7 found
higher raw/resolved contact error; it does not qualify Ultra144samples quality.
Evidence: Build/v3-ao-filtered/comparison-chain.md and quality-contact-comparison.md.
The experimental depth pyramid, descriptors, shader product and reference-only
runner helper are removed; CPU preparation and analysis-tool fixes remain.
Debug game/experiment/runner build and the existing seven-frame resize check pass
(validation0). That rollback checkpoint restored runtime-full exactly to13c16640461c19f3db95513421fa6eba02915f2da4487af2c4807b33c9ac4b23;
current quality work uses the newer identity below.
Baseline-vs-rollback image equality was not tested at matching settings: baseline
indexed/Visibility and rollback auto/Compact differ, so that A/B is ineligible.
Evidence: Build/v3-ao-filtered/rollback.json and rollback-output-check.json.
Earlier candidate generation622a95cf
and Build/v3-integration/identities.json are historical checkpoints, not the current
source/binary qualification. No new foreground measurement has been run.

Graphics analysis preserves stable IDs55/56 after retired53/54 and reads actual
historical outline catalogs. Existing self-tests pass; captured GPU frame coverage
is complete. Absent external utilization and invalid critical_path remain N/A.

## Debug startup depth-guide fix

Normal startup with existing saved settings reproduced an HDR post guide failure;
the isolated smoke profile did not. Layered translucency exports its reused front
depth as R32_TYPELESS with an R32_FLOAT SRV, while HDR post, reconstruction and
presentation still demanded an R32_FLOAT allocation. All three consumers now use
their existing depth-resource helper, retaining graph identity, extent, format and
lifetime checks. No extra texture, copy, shader or per-frame scan was introduced.
The normal muted executable maintained a responsive Project HS window for15seconds
and exited0 after a normal close, stderr empty. Existing water/mixed/empty fixture
now enables motion blur/DoF and requires actual HDR geometry-post passes; its
15-frame native run passes with validation0. Evidence: Build/v3-debug-startup-fix.json
and v3-debug-startup-fix-build.log. Release and provider-specific qualification have not
been rerun for these three consumer fixes.

## Current lighting quality work

Exposure is now fixed at1 by the selected art policy. Histogram adaptation, the
read/write swap,3GPU dispatches,2scene/reconstruction copies,5shader products,
unused target/PSO state and the dead upload_constants timer/CSV column are removed.
One immutable DEFAULT float2[1,1] is initialized once per view; its initial upload
retains the existing capture/graph state rules. SDK1x1 exposure is produced by the
existing guide pass; DirectSR automatic exposure is disabled. No per-frame exposure
scheduler, extra option, manager or synchronization exception was introduced.
The identical temporal/reactive HDR input was removed from graph declarations and
retained-plan identity(34to33 inputs), rather than relaxing duplicate-read checks.

Depth of field focuses on the actual view eye-to-target distance, approximately28m
for gameplay, including zoom/auxiliary views. The fixed10m setting is removed without
changing GPU ABI or the physical CoC equation. Eight pre-post inputs matched exactly
in the isolated focus comparison. Camera/exposure/post sequencing remains intact.
Archer's RGB-only normal atlas now supplies neutral roughness1, with authored
skin/cloth/equipment/cutout values. Captured skin/cloth/equipment samples have median
roughness0.498/0.749/0.600 and maximum metallic0, replacing the renderer's universal0.30.
Base color, normal data, alpha and slime materials were not retuned.

SSR RGB is confidence-premultiplied before filtering and added once at composition.
History confidence reductions scale RGB too, so disocclusion immediately restores
the environment. Original RNG/PDF/BRDF/sample count and null-sample denominators
remain. No new probe lookup, buffer or pass. Existing GPU cases cover full miss,
partial hit, half confidence and temporal zero/partial confidence. An obsolete
raw-weight/five-probe-path test was removed because that production API was retired;
current grid/native cases remain. Legacy log readers still handle retired IDs.
The misleading scene_work capture alias is removed; the grass reader uses canonical
composite HDR with the recorded exposure for historical adapted captures.

Debug game/experiment/render runner and official content built. Existing configured
post-topology and BSDF GPU checks pass. Saved-setting muted180ticks/180frames passes
validation0 with unchanged gameplay checksum6077983588339752369. Exposure reads
[1,1]; no exposure passes appear in the completed GPU scope. After a logging-only
field deletion, current game/experiment/runner were rebuilt; actual CSV is199columns
including task extensions, row widths match, and the retired timer is absent.
Normal saved-setting muted startup kept a responsive window15seconds and closed0.
Final water/mixed/empty15frame validation also passes with0errors. Release and
foreground FPS have not been rerun. DirectSR code matches the official exposure API
but HS_WITH_DIRECTSR is OFF and the cached SDK include directory is absent here.
The isolated lighting checkpoint used runtime-full850b34ea6b0876e26f7fe115ec8d318f3d6d326c216c675385d8fba505d8812d;
the current BSDF/demand checkpoint is below.
Evidence: Build/quality-lighting-20261105/RESULTS.md, final-result.json and logs;
Build/ssr-confidence-20261105 contains the isolated SSR product/math comparison.

The unqualified terrain stochastic-reuse candidate is isolated in
Build/terrain-stochastic-20261105/candidate.patch and not in production source.
It requires register/quality/frame-cost qualification before adoption. The static
texture-demand mask candidate was removed because unused consumer planes prevented
pruning while adding mask work. Overall directives remain incomplete and60FPS unmet.

## Current BSDF and static-demand simplification

The current direct-light path prepares base/coat GGX roughness widths and view
Smith terms once before non-area light traversal. Raw BSDF callers keep wrappers;
coat keeps its original separately normalized basis. Anisotropic and area/PDF/
sampling semantics are retained. Coated/prefiltered IBL now streams every admitted
reflection candidate into a running convex mean instead of constructing and
rereading a per-pixel index/weight array. Original coverage/global fallback and
separate base/coat mip samples remain; mixture32 still reuses its blend across
sample directions. No new resource, PSO, manager or production flag.

Actual Front/Layer DXIL confirms direct-view terms outside the main light loop.
Each full static IR has152fewer instruction lines and8fewer array allocas, but
4more rsqrt calls. These are not executed ISA, register or timing measurements.
The same experiment PE with baseline8616944 and current48e7d6b each completes
180lockstep200-enemy frames at1080p Ultra, validation0. Geometry/material/motion/
transparent identity are unchanged;36/42saved buffers are byte-identical. Six
HDR/temporal outputs differ with finite values and maximum absolute delta0.0003052.
Existing hardware BSDF72coated cases and six-frame GPU light-list membership/order
checks pass. Source/content/PE and numerical evidence: Build/coated-view-20261105.
Checksum capture was disabled; checksum0 is not simulation equivalence evidence.
No current foreground FPS or GPU gain is qualified. The shared smoke/capture path
already applies master0; a redundant experiment flag was removed. No audio contract
or new permanent test file/runner remains from this work. The baseline Generation
pointer remains for the still-required clean performance comparison.

Base, anisotropic and layered BSDF evaluations now return their diffuse substrate
alongside total reflection. Direct-light component evaluation no longer computes
the substrate Fresnel/coat attenuation again in a separate split pass. Raw APIs
still normalize inputs; prepared evaluation shares one normalized basis. Thin-sheet
opposite-side diffuse, coat/sheen, optical overrides, unlit and specular clamping
retain their contracts. Floating-point normalization/reassociation may change bits.
Existing GPU cases passed both a temporary former-calculation comparison(max4.77e-7)
and the final analytic diffuse/finite/nonnegative/TIR cases. The temporary copied
calculation was then removed; no new test executable or production switch remains.

Static geometry-demand cells validate immutable bounds at construction. Active
views prepare double plane/affine coefficients once per frame; cell intervals no
longer repeat those finite/order checks. Dynamic radius/error and conservative
double padding/overflow fallback remain. The original per-point calculation still
determines exact demand and refreshes streaming requests. No previous-frame cache
or new registry was added. Existing randomized/invalid-input instance cases pass.

Actual DXIL static IR instructions: Front21793to21305, Layer21687to21199,
Deferred54793to53493; corresponding rsqrt calls218to206/218to206/637to597.
Texture samples are unchanged. These whole-IR counts include alternate branches
and are not executed ISA, register-pressure or whole-frame performance evidence.

Grouped Debug game/experiment/runner and official content build completed. The
BSDF/demand checkpoint used28231153f9bd17a64ae43ab6ca704f3f4613fd28494344b52a4b7122f94c1e51;
the current terrain-offline checkpoint is listed above.
Existing water/mixed/empty native15frames pass validation0. In six selected captures,
222of250 buffers are byte-identical, including geometry/material/motion/identity;
changed buffers are HDR color and its temporal/translucency consumers. HDR RGB p99
absolute difference is0; largest difference0.00390625 is4.7734375to4.77734375 in
the mixed frame, with mean absolute differences at most2.17e-8. This qualifies
these inputs, not every scene or foreground1080p Ultra200-enemy performance.
Evidence: Build/bsdf-demand-20261105/RESULTS.md, result.json, native-output-comparison.json
and static-comparison.json. Current foreground FPS/Release/full workflow remain
unexecuted. Material.Resolve/terrain and crowd optical work remain the main GPU
structural priorities; no new60FPS or net frame-speedup claim is made.

The subsequent terrain stochastic-cell sharing prototype is parked in
Build/terrain-stochastic-qualified-20261105/candidate.patch. The normal terrain
source is restored exactly to SHA256400e9e7194740f5b7c98bbe00680fa81518e0d991436e4b550a8319776ab262f
and its rollback checkpoint used28231153. Three muted offscreen lockstep
200-enemy600-frame runs completed(exit0, validationOFF). Their timing comparison
is ineligible: live GPU engine counters identified external HTGame at59percent.
No foreign process was stopped; the repeat finished before cleanup. Baseline vs
prototype final captures have36of42 buffers byte-identical, including terrain
GBuffer and lighting. OIT/translucency/temporal color differ, so this is not a full
image-equivalence claim. The prototype's static IR grew and its net cost remains
unqualified. Do not adopt it from source-only redundancy reasoning. AGENTS.md now
requires a brief external CPU/GPU load check before each comparison run and
excludes runs contaminated later. No current foreground FPS is established.

## Implemented ownership and retained contracts

Renderer records through existing creator-bound Jobs TaskGroups and joins accepted
work. RenderPackage explicitly owns the display sample, scene, UI and native VFX
batch; obsolete internal clock/UI and snapshot adapters are removed. Native accepted
prefixes, queue completion, alias checks and CPU/native/Present ACKs remain distinct.
Geometry-record SRVs are written only at allocation and cluster-buffer replacement;
resident publication preserves that slot and the existing completion waits.

GPU UI uses DirectWrite glyph shaping/fallback, a glyph coverage atlas and ordered
shape/text drawing. WIC whole-bitmap surfaces/lock/copy/version recovery are removed.
UI host/HLSL ABI has one declaration. VFX large generated C++ tables are now binary
shipping ranges, admitted once by the shared reader. The shipping header separately
binds physical VFX metadata and semantic image identity; full-range SHA256 remains.
Translucency/CharacterMaterial names, products and historical reader mappings agree.
Pinned textures skip dynamic demand and local-shadow static batches retain owner facts.

Gameplay retains60Hz, strictFP/RNG/order and structural barriers. Same-tick mutation,
actual adjacent previous/current samples and owner-thread borrows remain independent.
Startup fixed-clock debt is reset only after the matching session's first Playing
Main Present; this removes the observed initial70tick jump, not later render stalls.

## Terrain and grass

Current surface layout2 adds one residual-depth search rather than separate POM
searches for each layer/cell. Bake stores weighted depth below the unchanged mesh
in metres(0..0.05), normalized into plane1 alpha; plane2 stores four layer weights.
Fine normals are sampled independently from the source detail layers using stable
world mapping. Geometry/collision/grass transforms, RNG and wind are unchanged.
Page use requires sufficient minor-axis density and the full ray plus filter
support inside the actual section. Other pixels use the existing evaluator;
transition pixels evaluate both. No new runtime representation flag or manager.
The first candidate exposed an isotropic page-filter mismatch: minor-axis admission
was followed by LinearClamp's major-axis mip. Interior page reads now use the
existing anisotropic sampler; bounded support prevents wrap across a section edge.
Residual mips are appearance averages, not conservative height-search bounds.

TSF1/chunk wire version remains1; header lane12 identifies surface layout2.
The reader decodes legacy0 as layout1, rejects unknown layouts and retains owned
section/product storage. Runtime only admits layout2 plus matching section/source
identity(domain v2); legacy data selects original shading. Producer recipe also
includes the layout. The unchanged3-plane BC7 payload remains83897249bytes across
6products within the existing package/native budgets.

Terrain codec implementation is isolated in RenderData-owned hs_terrain_surface,
linked only by its actual consumers. The preceding broad library edit relinked
unrelated producers: four inspected product groups retained identical digests,
and Archer alone spent97125ms regenerating13unchanged products. This is real build
propagation evidence, not proof of timestamp-only nondeterminism. No ActionKey or
producer integrity check was weakened; shared codec changes still propagate.

Debug game/tool links and official content generation pass. Windows verify-core
passes19/19 and the existing package cases pass after target separation. Final
muted1080p60tick near/far/normal executions each render60frames, exit0, validation0,
pages admitted, checksum1396041847835126483. Final PE/content identities, captures,
build receipts and limitations: Build/terrain-residual-20261105/RESULTS.md and
result.json. This is component/functional evidence, not foreground200-enemy timing.
Full motion stability, current foreground performance and final60FPS remain open.
No new permanent test runner/file was added. Used raw/DDS captures are deleted.

Terrain page production is now offline. hs_terrain_surface_bake shares the
one-shot world-transport bootstrap, bakes the exact seed1 sections, and publishes
TSF1 metadata plus five ordinary section TexturePackages(83897249bytes total).
Production now reuses one2048-square3-slice output/readback pair across sections,
with sources/PSOs/MeadowControls prepared once. Existing color/normal mip generation
precedes BC7 compression; the three planes retain roughness alpha and normal/AO.
The2048 BC7 payload is only84bytes/section larger than1024 RGBA8. Native consumers
derive extent/mip/format from admitted metadata and retain the existing256MiB cap.
Local bake ownership is released before allocator shutdown; unknown submissions
remain held by Impl's pending owner until the existing drain succeeds.
Section images stay within the existing32MiB chunk/64MiB package limits; neither
limit nor the256MiB native page cap was raised. Runtime only admits matching
section values and canonical source identity, uploads, publishes and retires.
Other layouts/seeds, absent data or insufficient budget use original shading.
The page compute/MeadowControls/source-copy producer moved to hs_probe_baker;
ordinary runtime-full no longer contains its BakeCS/MipCS products. The shared
Impl retains the same native ownership slot in both builds for failed submissions;
producer allocation/body remains offline-only. No second renderer/task system.

The source key uses six terrain texture products, the two runtime terrain resolve
programs and Meadow geometry. Build-only inputs are Action inputs rather than
runtime product dependencies; metadata depends only on its section products.
The tool traverses required roots' existing Generation dependency graph and checks
actual input bytes once. It no longer reconstructs every Action owner, rejects
valid pinned Generations when current Actions change, or re-parses writer output
before publication. Cache/IO integrity and native callback completion remain.

Grouped Debug/official content build completed after these corrections. Existing
package and residency/catalog tests pass; the latter required the host TEMP path
because sandbox rename was denied. Its optional DirectStorage case remains skipped
without that SDK. Architecture check passes. Muted1080p normal60ticks exits0,
validation0 and explicit capture confirms terrain_surface_pages=true. Existing
water/mixed/empty15frame validation0 takes the custom-layout fallback; all250
captured buffers match the prior BSDF/demand checkpoint hashes. The validation
cleanup produces identical section/metadata products. Evidence:
Build/terrain-surface-offline-20261105/integration-result.json and RESULTS.md.
The follow-up density/packed integration evidence is in
Build/terrain-surface-density-20261105. Debug/content build and existing package,
residency/catalog and architecture cases pass. Muted1080p60tick main execution
passes validation0 and loads the pages. Actual2K bake vs published BC7 records
all5sections/3planes/12mips; finite values and numerical differences are recorded,
not an original-image-is-better quality oracle. At mip0 base-normal mean error
is0.94degrees and maximum21.55degrees. Saved gameplay images were inspected as
comparison candidates; still images do not qualify temporal stability.
Developer and packed catalogs classify section images identically. Shared geometry
metadata collection excludes the terrain archive/images, removing duplicate full
image reads/parsing and the fixture's early-continue descriptor omission.
Texture descriptor compilation reuses authenticated metadata digests instead of
hashing the same admitted subresource again; independent file/range/bind integrity
checks remain. Packed native fixture qualification is being completed separately.
Release/shipping packaging and current foreground performance are not qualified.
That density-only checkpoint did not contain the current residual representation.
Meso geometry and current performance qualification remain open; no steady-frame
speedup is claimed.

The packed world follow-up exposed a separate baked-GI compatibility defect:
Ultra capture density/POM/detail-fade values were also used as runtime compatibility
inputs, so a Medium preset disabled an otherwise matching bake. Capture quality
invalidation is now separate from authored scene/material compatibility, and export
checks the exact completed capture before writing compatibility revisions. Actual
source/product, parameter and scene changes still invalidate the bake. The existing
world fixture also required retired runtime cube/SH/GGX work; it now requires real
baked sampling/upload plus SSR/volumetric/HLOD and forbids recapture. The old
capture-source readiness bit is not a baked publication condition.
The7-frame GBV and standard receipts before this correction are failed workloads,
although terrain pages and HLOD were admitted; do not report them as full passes.
Material mutation is now relative to the admitted Generation, not an absolute
renderer initialization counter; compatibility domain v2 rejects older meanings.
The remaining exact mismatch was feedback aliasing: loose selection rewrote base
shader paths/digests while packed provenance retained the named products. Both
paths now keep canonical maps and choose effective shader variants at read/bind.
Canonical semantic dependencies use exact named digests; actual shader bytes and
owner checks use the effective variant. Admitted whole-file proof avoids a second
shader SHA; unproved loose fallback retains its integrity check.
Baked publication uses its immutable upload producer completion, independent of
later readers or capture binding changes. Full reader completion still gates
retirement/reset. Offline layout capture keeps its quality/invalidation/fence path.
The existing world oracle now requires baked upload/sample readiness and forbids
runtime capture, rather than requiring removed cube/SH/GGX work. Its always-false
capture-source diagnostic is removed. Temporary detailed identity/product dumps
are removed; explicit Debug capture retains only baseline/current revisions/epoch.

Grouped Debug/content build and residency/catalog alias tests pass. Existing native
multi-queue/service tests pass, including completed-upload/pending-reader publication,
reader-protected reset, capture-binding independence, actual revision rejection and
qualified export. Packed world7frames passes validation0: pages=true, sample_ready=true,
HLOD accepted and no runtime capture; loose muted1080p60ticks/60frames also passes
validation0 with checksum1396041847835126483. Both capture the same expected/current
environment/material/layout revisions. Evidence:
Build/terrain-surface-density-20261105/canonical-exits.json and RESULTS.md.
The original package/architecture checks remain valid; optional DirectStorage SDK
coverage, final-source GBV, Release/shipping package, movement stability and current
foreground200-enemy performance remain unqualified. No60FPS claim is made.

Optional pages are bounded to256MiB. Oversized valid layouts now retain the original
material instead of failing rendering. The CPU section retains its real span/seed;
GPU span0 signals no page. The small section table is imported even without a page.
Fallback is retained by existing scene/layout/pipeline ownership rather than retried
per frame. Invalid shapes, overflow and real native failures remain errors. Allocate
CPU section storage before submission and retire old pages after GPU drain, before
D3D12MA shutdown. No new retry manager or separate texture was added.

Accepted grass history commits before Present consistently with other view history.
This fixes the failure boundary; ordinary-play grass height/position/shape popping
is still unlocalized. No guessed LOD/wind/bender quality change was applied.

## Static lighting and transparency

SPB2 separates162SH9 nodes from4reflection probes. Wall-aware subcell masks select
valid grid corners and cells retain all reflection candidates. Runtime only admits,
uploads/publishes, samples and retires; hs_probe_baker owns native capture/filter/export.
The solver evaluates sky/emission/directly lit surfaces, one surface bounce only.
Dynamic indirect GI/SDK/SDF/history/seed paths are removed; direct lights, shadows,
SSR and translucent slimes remain. Shared point preparation locates a probe cell
once when diffuse/reflection need it; corner arithmetic and sample ordering remain.

Water-only scenes now use SingleLayerWaterPS and skip crowd peel/Exit/tail draws.
They preserve existing optics, authored thickness, refraction background, fog and
motion/history/identity/classification outputs. A typed water closure removes unused
character material/family bindings. Mixed water/slime scenes deliberately keep the
original ordered path. This is partial water separation, not completed mixed routing
or the final ordered-vs-shape comparison. The normal character path retains runtime
material parameters and coating/grazing fallback; no unjustified specialized PSOs.

Existing direct-light traversal/background reuse and merged Exit remain. AO avoids
same-pixel taps within each horizon side; SSR shares only its original-input PDF
context. Cluster chooses the indexed/mesh stream and common bounds once. Do not
count these previous changes as new savings. Offline LTC row parallelism produced
byte-identical output in6.120s versus63.467s (Build/ltc-comparison/result.json).

## Release capture separation and current evidence

Release normal rendering no longer retains capture-only HDR allocations or updates
capture-only slot/constants/metadata. Submitted timing/FG identity and skin lifetime
remain. renderer_capture.cpp is excluded from ordinary Release and included by the
offline baker; Impl layout is controlled consistently by HS_DEVELOPMENT_TOOLS.
Release hs_scene_probe_bake actually completed183frames and SceneProbePackage export,
then published scene-probes-baked using the matching executable recipe SHA256
e0a9e65de16f205f0249d2b26b47c27e77868f8496fe14453d08e64e37054e1a.
Evidence: Build/v3-release-capture-split-build.log and the Release Action result.

Previous qualified Debug runtime-full13c16640461c19f3db95513421fa6eba02915f2da4487af2c4807b33c9ac4b23,
shipping imagec3793c66226066dcae10df5a1991309d444ad2902cb2ef4f3d4e7c5d38337fb0.
Previous qualified Release runtime-fullb75d990ce0834226c0220002c20eafbc1b607ffa6fa8d5201288fb58492f02a7,
shipping image9c4b7ba45b88f48ded816815cc9acc6723c7bbc1a5b652525ab84c4325fb5169.
Exact source/PE/DXIL hashes and receipts are recorded in Build/v3-integration/identities.json.
Do not substitute a historical checkpoint's identities or FPS.

Water-only/mixed/empty15frame transitions, actual HDR/guide/identity output and warm
bank history passed with standard D3D12 validation0. Shared-cell change matches all
30selected reference buffers byte-for-byte (184320000bytes), not all scenes.
Existing BSDF/shipping checks pass. Oversized88-ground environment preview renders
3frames with validation0 and actual textured terrain. Evidence:
Build/v3-water-native-tests.log, v3-probe-fallback-tests.log,
v3-environment-preview/result.json, v3-integration/probe-cell-equivalence.json.
After the capture split, grouped Debug/Release builds completed with exit0. Existing
render-smoke180frames/resize and shipping-content pass (44.35s/1.36s), validation0.
Evidence: Build/v3-capture-split-final-build.log and v3-capture-split-native-tests.log.

Existing grass-history D144Hz16frame, cluster-retest14frame and render-smoke180frame
resize checks passed at the previous terrain/descriptor checkpoint. Standard200-enemy
validation ON180frames and OFF900frames passed at the earlier quality checkpoint;
a later real-time1200tick/760frame playback had no input/presentation drops. Those
runs were background/offscreen and do not qualify current FPS or grass geometry.
GPU-based validation200-enemy and Cluster checks exceeded their existing120s startup
limit; worker stacks were in NVIDIA shader JIT (nvgpucomp64), not a proved GPU root
cause. They remain unpassed. Keep v3-terrain-grass-cluster-stack.log/timeout receipt.

## Remaining directives and known limits

Remaining work: high-density terrain/derived bake and residual geometry contract,
multibounce offline solver, complete mixed water/solid thickness and ordered-vs-shape
selection, any AO replacement with a new measured quality/performance hypothesis,
Visibility/compact full closure,
static-atlas/dynamic-overlay vs VSM and local/volume invalidation, cook geometry route,
supported-memory peak residency, immutable draw templates, full PassParameters/ABI,
retained recording batches, remaining large binary tables and multi-view/SDK failure
boundaries. Full target-device foreground Debug/Release qualification/deletion follows.
Current wall masks make hardware SH interpolation ineligible for most central-floor
receivers, so a new Texture3D path is not adopted without a measured whole-pass gain.

Screen-space outline and old settings/product are removed; VFX borders are separate.
Four controlled pickup kinds have opaque body pixels in Compact/Visibility captures,
but ordinary-play glow-only appearance remains unresolved. PolarRune optional mask
uses existing PreviewTiming.z with admitted12slice ranges. The VSM receiver-plane
correction reduced the previously observed floor-stripe patches while preserving
bias; it does not prove every later terrain artifact fixed. Evidence remains under
Build/structure-slim-20261101/phase22 and floor-noise-20261102.

Latest foreground comparison evidence is Build/v3-ao-filtered, Debug/Od with the
existing renderer-frame Ob2,1080p Ultra100%,200enemies. Baseline warm40.837FPS,
1%low32.404, output dependency span24.326ms; the rejected candidate is recorded
separately. Baseline
Material.Resolve3.568ms, CompositeFront1.918ms, AdaptiveGTAO1.522ms. CPU render
22.723ms includes slot wait9.785ms. The older phase20/foreground2400 profile is
historical only. Queue scopes overlap; pass sums and DXIL/source reductions are
not whole-frame FPS.60FPS16.67ms has not been demonstrated.
Nsight hardware capture previously failed before target launch in Qt initialization;
no hardware counters were obtained. WPR/Nsight failure is not an engine diagnosis.

Used raw captures are deleted after comparison, retaining compact receipts/results,
selected reference buffers and PNGs. Cleanup manifests are under Build/v3-integration.
No full workflow is inferred from leaf checks, and checks are not repeated without
changed inputs or unresolved risk. Preserve GPU accepted-prefix/fence/alias lifetime,
actual view history and separate activation/native/Present receipts.
