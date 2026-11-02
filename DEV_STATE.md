# Current technical state

Working main b23c78ea31023a3c3cb7d71274d2a13ade261b30 plus dirty phase18 changes.
Active work is incomplete. Root owns Build/structure-slim-20261101 continuity,
source integration and the sole Ninja/GPU workload. No commit/push or branch.

## Static GI replacement in progress

The selected product contract is baked static indirect lighting, using the existing
SceneProbe SH diffuse and prefiltered reflection package. Dynamic light/emission
bounce and dynamic indirect occlusion are removed. Direct lights, dynamic shadows,
SSR, environment reflection fallback, translucent slimes and four Hero layers stay.
Brixelizer/SDF scene, history, cache injection, dedicated direct-diffuse seed pass,
SDK producer/bootstrap/lock/build/install paths and GI-only tests are removed.
Raster geometry no longer appends an SDK float3 position stream. Deferred lighting
has three outputs with two optional auxiliary targets (SSS and environment specular).
Core GI modes are Off/Probe; saved software_hybrid/ray_traced migrate to Probe.
Obsolete gi_quality is ignored on load. Runtime/native submission lifetimes remain.
Debug production is built. Static shadow/probe official closure and the existing settings/architecture/native boundaries now pass; qualification details below.

## Other phase18 changes and scoped evidence

- Existing compiled topology retains immutable execution resource membership and
  external-pass boundary facts. Ordinary Execute batches borrow those lists;
  synthetic batches still collect current resources. Native bindings, state,
  alias/shared-reader/fence and partial-submit rules remain current. Full retained
  dependency/barrier planning and compatible command-list grouping are unfinished.
- Geometry demand cells retain admitted static point facts, while streamed coarse
  error/radius and grass wind allowance remain live. Existing independent-reference
  cases check cached payloads and demand equivalence. Added point payload is about
  20B per static instance versus indices; actual process RAM was not measured.
- Exact independent POM-ray interleaving candidate is active in material_terrain.
  Same PE/matched official bake fixed300 surface check completed before GI removal:
  all 8 SurfaceResolve boundary resources are byte-identical; events/render/particle
  facts match. 27/37 raw/DDS files match; downstream OIT/exposure/temporal/HDR differ
  with the old NativeGI path, not attributed to POM. No foreground adoption claim.
- Pre-removal Debug batch and existing CPU instance-order/graph-validation plus
  hardware single/multiqueue graph checks passed. These do not qualify the later
  static-GI migration. Receipt: phase18/source-freeze.json and binary.json.

## Latest measured frame evidence (old NativeGI checkpoint)

phase17/foreground2400: Debug /Od, 1080p Ultra100%, 200 enemies, timing ON,
validation OFF, RTX4080 driver32.0.16.2101; 1326/1326 foreground frames. Warm1067:
35.544FPS, 1%low26.801, interval28.1341ms, p9532.4299, p9934.7285.
GPU output dependency envelope27.8821ms; marker union26.6239, uncovered1.2582ms.
HeroComposite4layers3.810, SurfaceResolve3.442, NativeGI2.365, GTAO1.490ms means.
CPU graph_build5.232, record2.384, submit2.075, plan2.013ms are nested wall times,
not additive CPU busy. Gameplay1.099ms per60Hz tick. CPU submitted before previous
GPU output ready1013/1067; frame/skin GPU wait4.675ms mean. GPU is the primary
average limiter here; full shader utilization and causal GPU critical path unknown.
FPS matches Present-return cadence. Interval = prior render26.0433 + adapter1.1244
+ capture/log0.9562 + residual0.0102ms. Inside-render residual0.3657ms is already
inside render. Legacy critical-path-valid0 is uncomputed in SourceOutputReady mode,
not invalid measured passes. Join rows by view_id + actual source frame identity.
These are NOT static-GI FPS or evidence that the 60FPS target has been met.

## Preserved implemented contracts

Runtime normalizes explicit frame/view inputs once. Immutable UI draw content is
retained independently of raster target/layout and Main Present/click acceptance.
VFX PairFacts masks and borrowed sealed recipes avoid repeated copies and scans;
dynamic payload/range checks and activation/native/Present ACK boundaries remain.
Gameplay preserves60Hz, strictFP, RNG/iteration order and checksum semantics; no-op
UI restores checksum dirtiness only after normal return. Cosmetic links live in
Runtime with stable DomainSignal.sequence/lifetimes. Public module boundaries and
Runtime OFF/ContentTools OFF builds remain requirements. Release omits diagnostic
recording/replay/CSV/PIX/debug helpers; later changes have not been Release-built.
Texture feedback keeps bank snapshots and SSE2 valid-row reduction/scalar tail.
Queue-local timestamp storage/resolve intervals preserve independent completion.
Static baked VSM/coarse coverage and precise CSM fallback remain separate.

## Next integration and remaining work

Finish stale-source/dependency audit, then one cohesive Debug build and matching
official shader/shadow/probe content closure. Run existing settings/architecture,
relevant native smoke/lighting/probe/graph checks and actual200 workload only after
source freeze. No Ninja during edits and no EXE relink while live. Existing full
workflow/Release receipts do not cover this migration; no Linux run is intended.
Default runtime-full still9750a33d..., simulation content85a0f257edd1ce70. Phase18
candidate7ae09e09 contains pre-removal products and cannot qualify static GI.

Continue measured Hero Exit4-to-one MAX-depth reduction (design only), full retained
execution planning and GPU UI replacement. Conditional terrain representation,
whole-LOD/instancing and fixed-shadow candidates require concrete consumer/quality
contracts. No generic registry/cache/VM or duplicate verification managers. POM
foreground control/candidate comparison must use the new static-GI representation.
Old NativeGI history variance is an issue of a removed feature, not a proven fix.
Preserve phase9/original crash evidence; retire completed raw/caches after compact
receipts. Do not alter/fabricate Generation manifests, Action results or product IDs.

## Latest phase18 qualification at stop

Normal and GPU-validation explicit Layout probe bakes complete; GBV20frames and
empty error log. First native attempt failed because removal shifted positional
DXC flags, enabling HS_CAPTURE_HERO_WORK in normal translucent pixel shaders and
accessing unbound root UAV4. Fixed with named MaterialShaderOptions; actual normal
Peel DXIL omits the counter UAV and diagnostic variant retains it. Temporary host
validation flags are removed. Normal bake target exits0. Existing multiqueue GBV,
6affected CPU checks and native Layout revision/auxiliary2 checks pass (settings
atomic-save test required normal Windows permissions). Native revision fixture
now explicitly consumes Layout test-render, not immutable Baked runtime-full.

Official selected candidate scope runtime-full-phase18-static-gi ->
decc8f408a5b6850cc68257e68f63a9b8bdbf8355af9cc26ebe358af0797e998.
Default runtime-full9750 remains unchanged. Runtime PE FNV54990505f4e27d23/SHA
82f011d3964d50b9a857147f34ce954e445086485132654afbcad4bcf71eb835.
First present2400 run exits0/valid,1502frames/no device loss, but all background:
its39.66FPS/warm1190 must not be used for fair foreground comparison. Second
attempt exits0; window activation returned failure and latest rows background.
ComputerUse kernel reset immediately; no active owned benchmark/Ninja sessions.

Remaining: valid foreground profile; low-level OFF/OFF gate after module/settings
migration; obsolete SDK/build/cache cleanup; final receipt/default adoption review.
Remove stale runtime_capture cpu_scopes text for deleted software-scene scope in
application.cpp:786 in the next implementation batch. No throughput claim/60FPS
completion. Full retained executor, HeroExit4->1 and remaining directives incomplete.
