# DEV_STATE

## Current focus

Remaining acceptance is presentation- and hardware-facing rather than core simulation architecture.

### Camera, shadows and combat (2026-09-15)

Three nested, texel-snapped directional shadow cascades use 18/48/160m coverage and one depth atlas. The near tile has four times the configured side resolution (16x pixel count): 4096 at the 1024 setting, 8192 at 2048. Atlas allocation is approximately 80/320 MiB respectively. Projected coverage, overlap blending and tile-clamped PCF replace world-origin distance switches. All three slime bodies now use alpha-weighted OIT; authored eyes/ornaments remain opaque. Melee alpha was regenerated through Blender MCP from its authoring script.

Wheel zoom preserves the authored 80–120% range and extends to 15%, smoothly lowering pitch to 18 degrees and raising the target to 1m. Window ground-ray aiming and presentation share the camera pose. `--camera-zoom=15|45|80` reproduces the live camera path for captures. One continuous ground slab covers the decorative tree perimeter while gameplay boundaries and grass masks remain unchanged.

Successful skills cancel only unreleased basic casts. ProcessInput observes the last completed tick, so cancellation requires tick < release_tick; fired attacks retain recovery. A new press replaces previous buffered intent, instant skill release preserves the buffer, and charged skill release cancels pending intent or releases only its original slot. Swept obstacle/actor contact ordering blocks both sides' projectiles; melee, AoE and explosive derivatives use obstacle visibility. Applied damage-over-time remains active.

Verification: full 24/24 (`Build/verification/verify-20260914T161615783Z-12324.json`), low-level 7/7 (`verify-core-20260914T161832733Z-26836.json`), GPU smoke/barriers 2/2 (`verify-20260914T161442239Z-7736.json`). The later melee transparency correction passed content/render 3/3 (`verify-20260914T162319701Z-25120.json`) and GPU smoke 1/1 (`verify-20260914T162418005Z-15476.json`). Regression tests cover both sides of terrain, front-of-wall hits, explosion upgrades, charge slot release, stale buffers and post-release recovery. Captures live in `Build/Artifacts/shadow-camera-combat`; review.jpg and transition-review.jpg show actual renderer views; slime-transparent/capture_tick_360.png confirms the corrected melee body over the game floor, and perimeter100/capture_tick_180.png covers the tree perimeter. Four redundant .blend1 backups were removed, and future Blender backup files are ignored. Source FBX/PNG/Blend files and useful validation receipts are retained.

### Environment v9

Latest histogram/scale correction: native PCA Gaussianization + BC7 linear Gaussian terrain + per-mip inverse LUT are integrated. Terrain source tile size increased 3x to 6m (soil/grass) and 4.5m (gravel), with POM world-height scaling retained. Actual BC7 distribution tests improve RGB quantile error 72–85% over ordinary blending; full verify 24/24 passed (`Build/verification/verify-20260914T052652698Z-7384.json`). Camera comparisons: `Build/Artifacts/environment-v9/histogram-scale-review.jpg` and `histogram-mip-review.jpg`. Renderer uploads Gaussian albedo instead of the original terrain colour and adds one LUT; Final GPU validation passed 2/2 (`Build/verification/verify-20260914T103953989Z-26220.json`) and the normal build render/asset gate passed 2/2 (`Build/verification/verify-20260914T104202695Z-22892.json`); see report for approximation limits.

Latest user correction is integrated: grass grows in deterministic dense irregular clusters of 10–16 clumps within existing allowed regions; terrain uses near/far randomized sampling and continuous material blending to remove visible ranked-layer borders and close-range repetition. Full verify passed 24/24 (`Build/verification/verify-20260914T044920563Z-21260.json`); actual gameplay/close/gallery captures report zero validation errors. Latest visual evidence: `Build/Artifacts/environment-v9/dense-grass-review.jpg`.

Blender MCP authored and baked three tree archetypes, four rocks and four grass clumps. Fourteen FBX packages contain 42 LOD meshes and seven collision proxies; all FBX files were reimported for UV/tangent/topology and metre-bound checks. Runtime uses baked PBR arrays and actual-camera LOD crossfades. Final camera diagnosis corrected half-float world-position shadow artifacts with a 32-bit position GBuffer; focused render smoke and final GPU-validation smoke/barrier checks passed (2/2, `Build/verification/verify-20260913T150122131Z-9004.json`). Final full verify passed 24/24 (`Build/verification/verify-20260913T145401396Z-26084.json`). Source/output details, actual game camera evidence and visual limits are in `Docs/ENVIRONMENT_V9_REPORT.md`. Terrain reference height/normal maps and analytic ambient lighting remain provisional relative to a final production scan/IBL pipeline.

### Monster presentation

All three normal-enemy roles now use the authored SlimeFamily assets and five animation slots; legacy Slime/Cactus/Swarm09 sources remain preserved. Final idle/charge/death and player-scale captures were inspected. Full verify passed 23/23; GPU validation passed with zero errors for both barrier modes on a 300-slime overlap preview. Evidence and remaining target-hardware/human acceptance are recorded in `Docs/SLIME_FAMILY_REPORT.md`.

Successful process completion or zero D3D12 validation errors is not visual acceptance.

### Visual presentation

Still requires final human evaluation where applicable:

- texture mapping quality
- icons and UI art
- final VFX readability and quality
- monster visual quality

### Performance and gameplay

Still requires:

- target-hardware performance measurements
- gameplay-feel acceptance

### Audio

Compatible source-voice reuse, 3D audio, and pause/resume require a real audio-device listening pass. Silent fallback does not validate that path.

Generated audio assets require listening approval.

## Accepted baseline

- Slime eye-socket repair is accepted on the focused visual gate.
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
