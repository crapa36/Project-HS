# DEV_STATE

AI implementation continuity only. Original main workspace, HEAD
6521c731782d4f16330ca3e624c37399ebeda986; current changes are uncommitted.
No new branch/commit/push request. Active work: Build/gpu-transport-20261031.
Earlier technical evidence and rejected designs: DEV_STATE.previous.md in that
folder, Build/reanalysis-priority-20261031, and Build/hero-crowd-20261031/workload-backlog.md.
Do not treat older evidence as current binary qualification.

## Active goal and contracts

Accepted directives and W01-W08 remain ACTIVE; Debug 1080p Ultra100%,200 enemies
60FPS has not been achieved or qualified. Preserve /Od, assertions, checked
iterators, existing renderer_frame /Ob2, translucent slimes, four Hero layers,
full authored transport and sample/quality settings. No Linux or Computer Use.
Root owns builds and one GPU workload; subagents are Luna/high, no Sol.
Use existing narrow checks in batches, only when a concrete change needs them.
No new permanent comparison tools, generic cache/dirty registries or test runners.
Release excludes developer capture/trace/test features. Preserve Public layering,
OFF/OFF low-level build,60Hz,strictFP/RNG,partial-submit/accepted-prefix semantics,
GPU completion, frame-slot ownership and old-reader leases. Do not remove waits
or dynamic binding/lifetime validation to manufacture a speedup.

## Current implementation

- A02 keeps immutable static SceneGeometry records/draws under the existing
  SoftwareSceneCache, with waited-slot current rows and compact deformation
  overlays. Current-first/static-second order, wind/SkinCache, page generation/
  shape invalidation and native resource owners remain.
- Native GI now borrows ordered spans of cached immutable static instance rows,
  interleaved with frame-local wind/SkinCache/current rows in the original order.
  It no longer flattens/re-expands static instances each frame. Unique geometry
  templates alone receive current graph bindings. Cache keys include scene and
  generation owners, material revision, actual page resource/allocation identity,
  offsets/counts/generation/digest; admitted page metadata/submeshes are immutable.
  Per-frame SDK validation, SameStatic/diff, invalidation and updates remain.
  Unused geometry-base/offset API and synthetic offset tests were removed.
- DynamicVisibleRefs capacity uses the existing64KB allocation quantum instead
  of replacing a bank/graph shape for one extra record. Actual output stride,
  counts, shader bounds and required synchronization remain unchanged.
- Static builtin Ground/Dirt shape factors are folded once into the GI draw
  transform; they no longer use the per-frame deformation path or dynamic SDK
  instance registration. Wind and current/dynamic terrain remain dynamic.
  Nonfinite/nonpositive/underflow scales retain the original path. Existing CPU
  cases cover both terrain shapes, idempotence and exclusions.
- Terrain SurfaceResolveCS alone is specialized for its closed Ground/Dirt/
  MeadowSurface-ground route. Skin/actor/foliage paths are removed from its
  compiled variant. Current/previous transforms, retained static history,
  canonical vertex decode, UV/derivatives, metadata and full terrain evaluation
  remain. Recipe version11; normal and feedback variants both compile.
  Review caught missing previous_instance_basis and suppressed static history;
  both were repaired before compilation. Runtime default generation is NOT yet
  changed because the new final-composite output difference is unresolved.
- Temporary VFX CPU-upload capture instrumentation has been removed completely
  and clean Debug/native/app targets relinked. No capture-only FrameContext fields
  remain. Source-only counters/extra per-frame hashing were not retained.
- Generation GC verifies explicit retained roots once under its mutation lock,
  and does not duplicate planning tree walks before actual deletion. Actual
  deletion retains full ancestor/reparse checks immediately per candidate;
  dry-run retains preflight. Publication/pin/lease/grace/digest checks remain.
  No whole-sweep rollback contract was introduced or removed.

## Current build and technical checks

VSBuildTools14.44.35207 and existing Tools/CMake4.4.3. One Ninja per build root.
Build/hero-surface-20261031/native-ranges-final-debug.log: selected Debug app,
experiment,native runner,scene-input test build PASS. Final CPU target rebuild
and native-ranges-final-cpu.log PASS after removing stale offset-test expectations.
native-ranges-final-release.log: Release ProjectHS C++ build PASS with SDKOFF,
ContentToolsOFF,TestsOFF. No full cook/install/Release FPS repeated.
native-ranges-final-native: final clean borrowed-span API,19515 static rows,
SoftwareGI21frames,execution-valid,validation_errors0,exit0 in21.236s,Gen9750.
Temporary capture getters and graph-miss logging are removed from source and
both final binaries relinked; render_graph/capture/SSR/SSS/volumetric sources have
no diagnostic diff. No foreground FPS or blind long GBV repetition. Prior GBV
attempts without a completed receipt remain incomplete, not pass/hang evidence.
Original unresolved crash/PDB/ETW remain protected.
Generation lifetime/GC/repair suite plus actual Windows nested-junction rejection
PASS12.141s; code has not changed since that check.

## Latest source/content and measurements

Final clean Debug experiment SHA141b10f9b48fc65070e9f655cda478482e0963e792f1bf024cd470f2ef0d3c7d.
Debug app45aecd6f...1d9e0, native runner5dff3909...4ea10, Release app78195032...bc57f;
Full identities: native-ranges-final-state.json; source diff: native-ranges-final-source.patch.
Default pointer: Build/msvc-debug/GenerationState/runtime-full.json,
Gen9750a33dc4e29081ffb464b2dce7a37ea9776018e9947ee092815ccece73fe05.
Old dated runtime-full-pixel-front aliases were deleted; do not reuse them.

A02 pre-Ground measured pair: PE50c449 control vs53cb8e4 candidate, same Gen9750,
481 matched offscreen warm ticks120-600. SoftwareScene1.995->1.270ms;
Resources6.867->6.083ms; Render39.549->37.100ms includes Wait14.608->13.967ms.
Native Brix input1.705->1.773ms remains a bottleneck. Compile activity48->48.
This is one pair/component evidence, not current-source or foreground FPS proof.
a02-comparison.md/json and each directory's gpu-pass-times.csv retain tails.

Native ranges profile: PEe8a75dc2...0dd2d before final diagnostic/API cleanup,
sameGen9750,600frames/481matched warmticks. Two old-PE185af controls had native
input1.898/1.885ms ->0.920ms. Resources6.321/6.308 ->5.607ms. SoftwareScene
1.215/1.236 ->1.352 and native SDK prepare2.128/2.134 ->2.250ms increased; do not
hide or add overlapping child timings. Compile48 ->42 in this combined change,
not an isolated attribution to capacity rounding. New source eliminates static
AABB/material/submesh re-expansion and instance flattening; native admission/diff
~2.25ms remains a specific next candidate. This profile used GPU timing OFF,
not foreground FPS or final clean-PE whole-system qualification. Final cleanup
removed diagnostics/unused offsets; existing CPU/native21 then passed.
native-ranges-results.md and native-ranges-comparison.json retain means/tails.

New terrain compiler result: normal82424->49008B(-40.5%), feedback84832->51188B
(-39.7%). SSA-result lines8312->4648 and8360->4696. Actual sampleGrad callsites
28->21, bufferLoad/rawBufferLoad155->73; feedback writes21->21. These are static
compiled-code counts, not per-pixel dynamic work or hardware register counts.
Other eight terrain products byte-exact; all3639 product IDs preserved.
Only two shader products differ in comparison Gen
ed921c7ceb5fb927cb45d184e20596a4f882a9b27f83edcd7932017c9c577364,
selected only by runtime-full-terrain-specialized.json. Both immutable generations
were produced by existing tools; cooked output was not hand-edited.

terrain-control200/candidate200 use the same current PE and game content hash
85a0f257edd1ce70,600frames each; execution-valid/assertions PASS/no device loss.
SurfaceResolve4.355->4.078ms mean, but EternalReturn.exe was present in both runs;
external GPU load was not excluded. This is NOT isolated speedup or FPS proof.
Depth/GBuffer/motion/history/lighting/AO/guides and27/36 raw surfaces match exactly.
Nine post-lighting/composite/temporal/exposure outputs differ; CompositeHDR
maxabs0.460205/meanabs0.004821 and exposure maxabs0.072251. This exceeds earlier
same-PE repeat variation. Do not classify it as harmless or a terrain regression
without root-cause evidence; keep the original runtime-full selection for now.
Fixed dt, temporal/camera/history inputs, terrain mip descriptors and final VFX
counts match. Exposure measures already-different CompositeHDR, so it is downstream.
A targeted45-surface diagnostic then located the first large delta in native
Brix RawDiffuse (meanabs0.1441); CacheCurrent was exact. Raw specular/diffuse delta
and downstream composed/SSR/volumetric changed. Same-PE/same-old-generation repeat
also varied, but less: RawDiffuse meanabs0.00170 and CompositeHDR0.0000207.
CacheCurrent is not the actual CacheHistory temporal input during native GI, so
its equality is not proof of all dispatch input/history equality. No concrete
native synchronization/seed bug was found in source. Do not infer harmlessness,
SDK stochasticity as cause, or a terrain-only regression. The native-ranges oldGen
capture can also show the large shift; different PEs make that non-causal evidence.
See boundary-diagnosis.md, boundary-comparison.json, boundary-repeat-comparison.json
and native-ranges-vs-terrain-candidate.json. Keep minimal unresolved GI buffers.

Earlier same-PE VFX diagnostic (ac31ef5...) captured five actual CPU-upload spans;
all bytes and ordering matched, while27/36 GPU surfaces matched. This rules out
those captured CPU payloads, not GPU history/order/lifetime errors. Temporary
instrumentation is gone; vfx-input/output-comparison.json retains the evidence.
No current foregroundFPS/full GPU critical-path or continuous hardware-busy proof.
Never sum pass timings or parent/child CPU spans to infer frame/FPS savings.

## Cleanup and remaining work

User-authorized deletion completed:93old EXE copies,47PSO caches,redundant
msvc-prebuilt-release,old captures and77obsolete Generation aliases, without archive.
15canonical Debug pointers/currentGen9750 retained. Release GC removed27generations/
148ProductSets(exit0,332s); Debug GC removed211generations/825ProductSets
(exit0,2108.17s),15generations remained; the current terrain trial adds one.
GC-phase observed free-space gain445.907GiB;
prior finished-file deletion measured40.173GiB file bytes, not the same metric.
Finished A02/VFX full capture payloads and one-off scripts were then removed;
numeric receipts and small VFX upload spans remain (post-analysis-cleanup.json).
Earlier terminated GC attempts were deliberate traversal fixes, not final failures.
Additional used capture payloads and duplicate per-run PSO caches were removed:
241payload files/4,172,600,229file bytes (native-ranges-cleanup.json). Numeric
receipts, one opaque input set, unresolved Brix/composite/exposure buffers and
one preview per diagnostic run remain. No owned build/GPU/GC process is running.

Next: close native GI output variability before changing default shader generation.
Continue removing repeated static native admission/diff work (~2.25ms) through the
existing immutable owner boundary, while retaining actual mutation/fence handling.
The new named miss capture maps42warm misses:10UI Upload toggles,29resource-count
changes and3ClusterGeometryPage final-version changes. UI has both post GameUI and
SDK DisplayGuides consumers, plus Copy-queue completion used for reuse; do not
blindly fold it into only GameUI or fake a graph read instead of its real write.
Texture streaming/optional VFX/volumetric/cluster upload transitions also change
layout. Not all compile work is unnecessary. No cache enlargement or guessed
capacity change. Earlier T01 reduced145->48;
64KB SceneGeometry widths and whole-buffer volumetric declarations stay.
Peel coverage review found DXC already removes unused full-surface work. General
material capability review found no proven extra unused feature cost. No speculative
rewrite or arbitrary sample/layer/threshold reduction. Rejected TGSM triangle cache,
HeroCurrentVS domain union, direct-terrain GBuffer, field-wise accumulation,
AO async, R8 tags, header sharing, eager SDK batching and container trials remain
rejected; see prior evidence instead of repeating them.
