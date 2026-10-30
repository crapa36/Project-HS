# DEV_STATE

AI implementation continuity only. Main HEAD d76d5a44cb77e4756ffb5dfd74e0cc786b8817d3;
the dirty worktree includes earlier GI/Visibility work and the structural audit.
Preserve unrelated changes. No agent commit/push/worktree creation. Main is the
working branch. The prior expanded record is archived at
Build/structural-audit-20261030/DEV_STATE.before-final.md; do not promote its older
failed runs or component measurements to current qualification.

## Active objective and policy

Finish accepted reviews/directives and optimize actual Debug /Od/assertions/
checked iterators,1920x1080 Ultra100%,200enemies toward60FPS. Goal ACTIVE, unmet.
Latest request: prioritize measured GPU bottlenecks. Structural-Bug-Audit-d76d5a4
source defects are fixed; broader directives/performance and60FPS remain incomplete.

Use the narrowest existing contract checks and batch affected builds. No new
permanent runner without an isolation/dependency need, no source-spelling tests,
no detached algorithm copies. Keep actual ABI/resource/state/queue completion/
partial-submit/owner contracts. Use existing owners rather than new managers.
No Linux execution or Computer Use. Subagents: Luna/high, no Sol, scoped ownership;
main owns integration and actual builds/GPU processes. No parallel GPU workloads.
Release excludes developer CLI/capture/test/logging code; keep actionable errors.

Current evidence: Build/gpu-hero-next-20261030/RESULTS.ko.md and selected-source.json.
Prior structural checkpoint: Build/structural-audit-20261030/RESULTS.ko.md.
Prior optimization evidence: Build/gi-preparation-20261030/RESULTS.ko.md and
Build/frontend-qualification-20261030/RESULTS.ko.md, scoped to their receipts.

## Current GPU checkpoint

Build/gpu-hero-next-20261030 owns this checkpoint. Debug SDKON Gen65f2765e20324cba40cd280ed068afaf0fa8b74cccd0b7f3df17177095b3656c,
FNV9c572ae24a47d9d7. Release SDKOFF Gen024bc6f3bed7bc060f86353ecd7209552346cf4d067611d2ce898ff35269493c.
Final Release ContentToolsOFF/TestsOFF/all vendor SDKsOFF; actual Runtime install
succeeded at the absolute release-install prefix. Final named build had no work.

- Coated DFG is256x256 with NoV=u^2, sqrt lookup at all consumers,4096 samples/texel.
  Existing32-sample fallback/layer counts and exclusions stay; supported grazing
  threshold is1/1024. Native shape admission rejects128 assets. The existing
  independent65536-sample hardware BSDF oracle passes72coated cases including40
  grazing cases: max spec .000675, diffuse .000008, unchanged .02/.002 tolerances.
- Paired481ticks120..600: Hero layers mean/p95 ms .466/.597->.409/.497,
  .811/.954->.605/.715,1.651/1.837->1.109/1.227,2.759/3.070->1.788/1.999.
  This A/B is the IBL plus probe/shadow repair bundle, not isolated LUT causality;
  the two baked SPKGs also differ in85470 non-revision/checksum bytes.
  Control/candidate AO DXIL are IDENTICAL. The early-Z AO source experiment was
  not selected after a copy preserved an older mtime; it remains unapplied. Do
  not attribute same-shader AO timing variance to it. Source swaps must refresh
  mtime and compare selected Generation product digests before an A/B claim.
- Probe package compatibility now hashes scoped immutable products, material and
  fixed scene/light policy. Screen-space shader/pipeline counters and resident
  authored LOD promotions do not invalidate identical baked content. Default
  native bake seed1 matches gameplay. Physical page/texture identity still
  restarts an in-progress capture; actual shadow view follows page bounds.
  Unready sources preserve baked storage; late matches require completed fences.
  Native trace proves material36e46c8cad95429b->d8e28bc80c7a7813 during one old bake.
- Default600frame runs now only upload the probe package: previous20static shadow,
  24cube,4SH and4GGX operations disappear. This is startup work removal, not a
  steady-FPS or faster-Initialize claim. See probe-compatibility-analysis.
- Existing native queue fixture was stale: root bindings omitted live shader
  registers and its caster80B/segment8B layouts disagreed with ABI5's64B/32B.
  The fixture now matches real buffers and asserts indexed group headers,
  overflow and empty-caster/nonempty-group output. Legacy/Enhanced+GBV pass,
  including late baked adoption. No new runner or production check suppression.
- Same-gen repeat:17/23 raw surfaces exact (GBuffer,lighting,AO included); six
  OIT/composite/temporal outputs have bounded differences, no finite/nonfinite
  transitions. VSM1153page records/counters/layout and74published tiles/bank exact.
  This does not establish pixel equality or explain the prior broad mismatch.
- Foreground1080p Ultra100%,200enemies,1800ticks: original control->candidate
  30.917->31.368FPS,1%low21.682->22.908,p95/p9941.203/44.560->40.500/43.088ms.
  Reversed order control->candidate30.681->31.786FPS,1%low22.388->22.915,
  p95/p9940.825/43.169->40.100/42.025ms. All receipts and QPC intervals qualify;
  frame/tick counts differ and are not claimed identical.60FPS remains unmet.
  Whole GPU critical-path fields are invalid throughout; do not sum queue/pass
  times to infer FPS. Render CPU about30ms, GraphBuild11.5..11.8ms, record
  composite3.1ms; graph preparation p95 about8.5ms remains a measured concern.

Keep VSM ABI5's one indexed indirect head per positive-capacity group,64B caster,
32B segment, common root/fine record prefix and SV_InstanceID-resolved shadow
record ID. Mesh/local-shadow paths stay per-record. Preserve overflow admission,
no rejected-page clear, cooperative allocation and actual GPU fences. Earlier
R32 metadata and Hero7->5MRT guide reuse remain. Do not reintroduce rejected
opacity-only peel/parameter baking/AO-cache or normal approximation candidates.
Previous VSM implementation proof remains Build/gpu-vsm-instancing-20261030.

## Next GPU work

Unapplied candidate: Build/gpu-hero-next-20261030/ao-prepared-invw-candidate.patch
and its note/base SHA list. It proposes one validated reciprocal-W preparation
per pixel in the existing current AO metadata history bank, so horizon taps can
reuse validity and unprojection without a new texture. Qualified full-res fast
projections only; arbitrary projection fallback and graph history/fences stay.
No compile/oracle/GPU evidence yet. Review patch, adapt existing24case actual AO
comparison, then keep only if real same-quality full-pass savings outweigh prep.
Do not apply while another build reads the source. Preserve measured IBL baseline.
No Computer Use or owned game/debugger processes remain at checkpoint completion.

## Structural audit B01-B05

- B01: VolumetricLighting's actual shadow read list now includes fixed-root static/
  dynamic textures with ComputeRead. Metadata/page table/banks keep AllShaderRead.
  Unused draw-record graph read is removed; input validation, descriptor and COM
  owner/retirement contracts remain. Cold/retained shape identities include roots.
- B02: ParallelFor catches internal callable/chunk preparation and submission
  exceptions once, records the first failure in existing fixed-size state and
  stops admitting chunks. The caller's normal Wait drains accepted work before
  returning failure. Single Submit's pre-admission exception contract is unchanged.
  HLOD is the production bulk caller. Existing fault fixture covers later failure
  after an earlier chunk is admitted; no executor/allocator framework was added.
- B03: HsHeroTranslucentLighting commits background_uv only after the depth and
  direction policy accepts refraction. Rejected candidates preserve source color,
  UV and depth together. Existing sky/out-of-screen/layered transport policy stays.
- B04: ContentLinker places the existing const ShippingImageInfo in a read-only
  .hsimage PE section. Installer reads the linked64-byte header and compares staged
  pack header/extent before publishing. Runtime keeps the same expected-header
  object and startup match. No duplicate header, Release verification CLI or
  sidecar receipt. A preliminary receipt idea was rejected because it did not
  extract the executable's actual expectation. Existing install fixture now uses
  a compiled PE and tests intact mismatched pairs. Its standalone C++17 requirement
  was fixed after one initial fixture compile failure.
- B05: Single-config support is explicit; multi-config generators fail at configure
  rather than exposing Release developer targets without implementations. Release
  classification is case-insensitive and shared by target/source/fixture gating.
  Dead multi-config hot-reload branching is removed. Ninja presets remain supported.

## Current qualification

- Current GPU qualification is the bounded batch above; raw failures remain for
  provenance. Failed old7MRT fixture expectations were updated to current5formats,
  with all other functional criteria preserved. GBV frame1upload history loss was
  a test consumer defect, not evidence that native probe upload failed.
- Prior unchanged core19/19 and OFF/OFF/architecture, partialParallelFor failures,
  release install transaction/mismatched PE pair/configuration fixtures retain
  their scoped validity. This batch adds real rebuilt Release install pair proof.
- Earlier long5400tick SDKON success belongs to structural checkpoint9caa7ff1...
  f2b34. Current1800tick runs do not establish complete resolution of the old
  intermittent DEVICE_HUNG or all multiview/partial-submit/fault scenarios.
- Automatic performance checksum0 is not an oracle. No Linux, ComputerUse, broad
  unchanged suite or Release actual gameplay was run. Existing failure dump
  7,743,787,037B and matchingPDB remain at Build/frontend-qualification-20261030/
  native-ultra-alive-dump-3. Preserve them; do not promote old receipts to current.

## Preserved implementation decisions

- Volume compute uses its owned41-descriptor single table instead of cloning the
  global1392-entry heap and27-entry suffix. No production global queue drain or
  diagnostic serialization remains. Previous Direct/Compute wait experiments
  narrowed overlap but did not establish a faulty resource. DRED active/pending
  breadcrumbs are not a root-cause oracle. CBV-volatile trial was reverted.
- Immutable character textures reach enhanced AllShaderRead through COPY_DEST to
  COMMON to enhanced SRV; Hero/GI share the read scope. COMMON/PRESENT's zero alias
  does not imply never-accessed. SDK native handoff restores engine bindings.
- SDK1.1.4 full shader tables link only in the build producer. Runtime owns generated
  immutable bindings/size/SHA and loads selected Generation DXIL. All732keys/7groups
  had exact metadata evidence. Debug cache is SDK-ON; Release cache is SDK-OFF.
- GI preparation reuses per-slot8,397,504B baked descriptions under exclusive owner
  (three retained capacities are a memory tradeoff), flat wanted keys, prepared
  unique geometry reads and sorted membership merge. Resident geometry pages own
  exact stable submesh keys; frame-local transforms/skin/wind/tokens remain dynamic.
  Two throwaway row vectors and unused mesh-name hash are removed. Prior measured
  graph_resources12.929 to9.924ms is background component evidence, not60FPS proof.
  Unprofitable static lookup/finite reuse trials were removed; do not reintroduce
  them without a distinct reuse boundary and measured benefit.
- Camera-GBuffer GI seed no longer claims an unmatched SoftwareGI receiver domain.
  Actual world/froxel receivers and baked-coverage readiness remain. Terrain view
  normalization and layer-invariant mip preparation are hoisted without lowering
  POM/sample/quality. Actual producer DXIL was inspected at that checkpoint.
- Visibility count/scatter use SM6.0 groupshared8x8 histograms/reservations, with
  every invalid/OOB lane reaching every barrier. No WaveOps requirement. Separate
  classification shader root removes material-shader fanout for classification-only
  changes. Existing GPU count/cursor/prefix/unique-pixel/entity/grass/resize checks
  passed at its receipt. ProjectHS-only defaults select Visibility; experiment/native
  explicit configuration stays. The six typed-store formats are already mandatory
  at existing FL12.0; prior extra older-GPU compatibility warning was incorrect.
- W12/G02 queue-local timestamp/readback separation was explicitly approved and
  implemented. Do not reintroduce a global timing wait or ask again for approval.
- VFX uses immutable prepared recipes, indexed canonical store and semantic pair
  identity, distinct CPU activation/native emission/UI Present acknowledgements.
  Keep moderate simple overload protection; essential attack/risk warnings stay
  protected. Earlier34781/34781 births and5118/5118 profiles had no budget drops,
  sampled logical peak12.3MiB/64MiB; that calibration is not a new global proof.
- Main simulation60Hz/order/strictFP/RNG/checksum contracts stay. Current/predecessor
  sample rotation uses actual adjacent ticks; same-tick world mutations refresh
  current. UI/skin/frame-bank ownership and Present acceptance remain separate.
- Software mip transactions retain the published fallback until final publication;
  native chunk/readback and bounded two-owner budget-shrink cases passed earlier.
  Preserve mapping-only ledger admission, cancellation/generation and fence lifetime.
  Do not infer general fairness/arbitrary budget progress from those bounded cases.

## Remaining accepted work and limits

Continue measured GI CPU preparation and SurfaceResolve/AO/Hero GPU costs toward
60FPS with equal Debug quality. Current matched-tick components: SurfaceResolve
3.672ms, HeroComposite5.673ms, AO3.342ms; per-pass queues overlap.
StaticGpuScene already owns admitted static records; any larger static reuse must
cover actual resident page generations/material variants and keep deformation,
current graph versions and old-reader lifetime correct. No new cache manager.

Conditional GI MRT, compact feedback reduction, HZB grouping and material domain
changes still require equal-quality whole-cost evidence. Multiview/partial submit,
software budget/generation/bake readiness and full package qualification remain
scoped to their existing receipts. Prior broad114/115 is not a current all-pass;
older WARP VSM GBV byte39/44 finding remains unresolved, not newly reproduced.
External D1-D4 structure generator is unavailable; do not mark its work complete.

Authoritative remaining/disposition ledgers under
.planning/2026-10-23-complete-directives/:
remaining-work-20261027.md, directive_status.md, review-bdf8944-20261028.md,
review-011ef96-20261027.md, focused-review-011ef96-20261027.md,
ponytail-deep-review-011ef96-20261027.md, integrated_status.md, queued-51d5055.md,
structural-audit-d76d5a4.md and their references/. Prior source decisions and
unavailable/conditional tasks remain there; do not repeat completed fixes.

## Execution

ARCHITECTURE.md and scoped AGENTS govern Public-only modules, OFF/OFF layering,
authored ContentSource/Schemas/Content/Shaders and native lifetime contracts.
Use VS2022BuildTools14.44.35207, cached CMake4.4.3 and one Ninja per build root.
Actual /Zi builds work on the host; restricted worker C1902 was environmental,
so do not add /Z7 workarounds. Preserve /Od and existing renderer_frame /Ob2;
optimized offline producer/HLOD flags are separate from Debug game qualification.
Named app builds use existing content; explicitly build hs_content_generation
when shader/content inputs change. Do not recook unchanged inputs for every check.
