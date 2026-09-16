# DEV_STATE

Record only state that changes AI implementation decisions or prevents repeated investigation. Do not track manual/human acceptance, playtest, listening, visual-review, or target-hardware tasks here. Replace superseded state instead of appending history.

## Current technical state

### Metre scale, contact and slime motion (2026-09-16)

User confirmed 1 unit = 1 metre and standing character height 1.75m. Player scale is .97174913; cooked bind body measures 1.750000057m after scaling. Normal slime body heights are .8/.9/1m, with per-role collision radii .63/.71/.79m. Tree/rock visual footprints match obstacle radii; grass retains cluster width with .15–.35m height. Projectile presentation originates at 1.05m for player arrows and .45m for enemy globes; simulation distances remain metres.

The archer cooker excludes Arrow_MAT geometry and grounds authored frames. Runtime corrects the final blended standing support surface: per-frame cooking alone left up to 16cm penetration during Idle/Run blending. Independent runtime-equivalent probes verify corrected sampled blends; Death keeps whole-body baked grounding with midpoint error -2.66mm to +5.46mm. Bow and body remain intact.

Audio master output mutes whenever the game window is not foreground, preserving user volume settings. Skills can cancel basic recovery after release while preserving already emitted projectiles and basic attack cadence. Tests cover instant/charged skills and focus/settings interaction.

Blender MCP regenerated the three slime animation sets: melee crouch/jump/headbutt, ranged cheek inflation/spit, and suicide slight growth/strain. Suicide cancellation replays the charge backwards from its current progress over 36 ticks, including redness and bubbling; attack recoil keeps facing for 18 ticks. Shared shader expressions provide frowns and death X eyes; body opacity multiplier increased .45 to .52.

Full gate: 24/24 PASS, `Build/verification/verify-20260916T014834794Z-31908.json`. GPU validation smoke/barriers: 2/2 PASS, `Build/verification/verify-20260916T015008036Z-11980.json`. Actual game capture `Build/Artifacts/metre-slime-20260916/game-close/capture_tick_360.png` has zero validation errors. Slime key-pose captures are in `Build/Artifacts/metre-slime-20260915/front-slime-review.jpg`. Automated checks and key frames do not replace final motion/feel or real-device listening acceptance.

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

The optional `msvc-asan-core` preset built and passed `simulation.determinism-core` on the current checkout; invocation and SDK/runtime prerequisites are in `Tools/tooling/WORKFLOW.md`, with receipt `Build/tooling/qualification-20260906/msvc-asan-core-receipt.json`. This does not establish a new full-project gate.

Tracy/MCP remains inconclusive: captures omit submitted jobs/wait zones and reported metrics need scrutiny. See `tracy-qualification.md`; do not adopt the isolated instrumentation. CodeGraph and workflow skill comparisons remain in progress. No batch-wide winner or final cleanup is claimed yet.
