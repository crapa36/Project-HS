# Backup branch continuation — 2026-10-14

Branch: `codex/engine-v7-backup-20261009`. Recovered source was `1ba82b7`.
Current code checkpoint: `505272f5254390687f5462bb6a686f02127bcae4`.
The complete attached investigation plan is **not finished**. Hardware unavailability
does not block independent CPU implementation. No user PC worktree was accessed.

## Implemented changes

- `a95aa42`: S01 shares canonical source-index keys, the unchanged hash, and revision
  comparisons in Core's existing snapshot contract. Presentation and Renderer retain
  separate commit/abort maps. Skin/persistent-slot hashes use the same algorithm.
  All16 revision masks, anonymous keys, session restart, aborted reset and missed
  submission recovery have CPU coverage. Named occlusion encoding preserves every
  tested float bit, including signed zero; shaders and GPU layouts are unchanged.
- `0d6101c`: S02 groups13 decoded-retention containers into `DecodedVfxState` and
  replaces three optional terminal-clock fields with one coherent optional record.
  Candidate publication is checked noexcept and delegates to direct vector member swaps. Pending events, burst
  tombstones, low-health pulse and authoritative clock stay outside that swap.
  Capture closure/rebind checks still run before renderer publication. Only the
  three identical light/distortion/decal expiry predicates share a local lambda.
- `906d95d`: Linux recipe generation reproduced a collision between
  `bits/alloc_traits.h` and `ext/alloc_traits.h`. Compiler include roots now retain
  relative subdirectories. GCC/Clang link detection selects the actual `-o` producer
  executable instead of a helper command containing `/lib/`. Windows mapping stays
  unchanged. The real asset pipeline checks the captured executable link.
- `2cc2ce5`: Existing typed VFX frame regressions compile their real CPU sources in
  portable builds; no fake renderer or substituted implementation was introduced.
- `505272f`: retains the original allocation-free member-swap operation inside the
  new decoded unit. Final VFX session/typed-frame tests passed again (2/2).

## Evidence

Local Linux: CMake4.4.4, Ninja1.13.2, GCC13.3.0, Debug, RuntimeOFF/ContentToolsOFF.
Configured existing render-data/asset-pipeline opt-ins, built all targets and
`hs_audio_content_recipe_identity`, then ran CTest: **33/33 passed**.
See `linux-portable-tests.log`, `linux-recipe-regression.log`, and source hashes.
The initial recipe failure is retained in `linux-recipe-before.log`.

The first local build also produced a zero-byte object and two valid ELF outputs
without executable permission. The empty generated object was rebuilt and executable
permissions restored; source checks and assertion/timeout policies were not weakened.
Initial asset tests lacked the explicit recipe target; after building it, its real
header collision was repaired and the complete tests passed.

S01 CI38575153846 and S02 CI38575696579: all three jobs passed (Linux core,
Windows core/portable, production Runtime/Renderer compile plus explicit HZB WARP).
Linux recipe CI38576212038 independently passed core18 and portable32.
Final source505272f CI **38577118658 passed all three jobs**: Linux core18/18 and
portable33/33, Windows core gate and portable35/35, production Runtime/Renderer
MSVC compilation and explicit HZB WARP with Debug layer/GBV. Run:
https://github.com/crapa36/Project-HS/actions/runs/38577118658 . Exact jobs/steps are
in `ci-receipts.json`; selected test log lines are in `ci-test-excerpts.log`.
Hardware/native scene tests remain unexecuted.

## S01 ABI inventory

`RenderMesh` keeps its existing ordinal contract: Archer0; legacy Enemy1–Boss4;
PlayerProjectile5, EnemyProjectile6, Area7, Pickup8, Ground9; MonsterMelee10 through
BossFinal15; TreeTrunk16, TreeCanopy17, Rock18, Grass19, DirtPatch20.
The environment mesh flag is still0x10000, now named at CPU frame call sites.
Complement0x20000, grass history0x200000, cached-skin flags and shadow/geometry
`PassValue` packing retain existing values. `project_hs.hlsl` dispatches shadow
draws through `HsShadowIsDraw/LoadDraw`, and packed geometry through `PassValue>>2`;
that existing helper boundary was not replaced by a new shader schema.
Static history and retest use enabled occlusion; dynamic visibility keeps the
existing conservative disable mode. The encoder does not repair the legacy
negative-zero predicate; actual grass padding remains strictly positive.

## C05 policy audit — unresolved, no rendering-policy change

- `Content/Shaders/project_hs.hlsl`, `EvaluateSceneVertex`: instance normals divide
  by `max(Scale.xyz,0.0001)`. Negative scale signs are lost. Visibility and ray
  vertices share this evaluator. The separately correct skin cofactor path stays intact.
- `Content/Shaders/ray_hit.hlsli`, `HsRayWorldToLocal`: divides directly by scale;
  singular instance transforms cannot be declared safe by the raster epsilon.
- `Source/Apps/Content/scene_hlod_cooker.cpp`: normal inverse scaling, determinant
  tangent handedness and reflected winding are explicitly implemented; finite
  transforms are required and exact zero scale is rejected.
- `Source/RendererD3D12/Private/renderer_local_probe_runtime.cpp`: static capture
  explicitly requires finite, strictly positive scale. A global negative-scale ban
  would therefore hide an existing cross-path policy difference, not resolve it.
- `geometry_package.cpp` validates local geometry bounds; it is not the owner of
  dynamic instance-transform policy. General frame ingress has no unified equivalent
  scale contract. Procedural layout scales and character interchange must be audited
  independently from binary authored FBX/Blend transforms.
- `scale-json-audit.json`:87 parseable ContentSource JSON files, zero parse failures,
  zero numeric properties named scale/scaling.
- `scale-fbx-audit.json`:390 binary FBX files parsed,8729 Model records and536 explicit
  Lcl Scaling/GeometricScaling properties; no parse failures and no nonfinite, negative,
  zero or <=0.0001 component among those properties. `audit_fbx_scales.py` is the
  reproducible read-only scanner; it hashes every inspected file and skips geometry.
  Omitted defaults, animated scaling, parent/pivot/axis transforms, baked handedness
  and the five Blend files are not evaluated. This is not complete mirror qualification.

Next C05 step: evaluate composed/animated transforms and define support per
static/proxy/dynamic/capture/ray path, then implement/test the chosen boundary.
Do not introduce mirror rendering or reject all zero scales from this census alone.

## Remaining work

S01/S02 source and CPU checks do not establish live reload-to-Render equivalence.
Hardware HZB scene/DRS/cluster, native UI/disk-close integration, full native verify,
and the existing full-FHD DeviceHung issue remain open. PC tunnel remains disconnected.
S03–S05 render-entry/preparation/view ownership changes need the actual call graph,
once-per-package event semantics and partial-submission history evidence; the current
`renderer_views.cpp` still calls the legacy argument overload and stores explicit
frame/view pointers. Later S/P/D packages remain pending. No whole-plan acceptance,
GPU image-equivalence or performance improvement is claimed.
