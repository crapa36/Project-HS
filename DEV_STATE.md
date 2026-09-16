# DEV_STATE

Record only state that changes AI implementation decisions or prevents repeated investigation. Replace superseded state instead of appending history. Source, tests, and current build output override this file.

## Current technical state

### Player presentation

The Archer cooks the five base clips plus `DiveForward`, `RunForwardStop`, and `ReactBack`. Trap forward-roll movement uses `DiveForward` as a full-body clip. Locomotion deceleration uses `RunForwardStop`. Player damage uses `ReactBack` through the existing upper-body mask so lower-body locomotion can continue.

`ReactFront`, `ReactRight`, and `WalkForward` are intentionally excluded from cooking and live under `ContentSource/Unused/Animations/Characters/Archer/`.

All skinned assets retain one fixed cooked clip table. Non-player assets provide compatibility aliases for the three player-only clip ids; monster runtime selection still uses its existing five authored combat clips.

### Camera, shadows, and combat

Directional shadows use three texel-snapped cascades with 18/48/160m coverage in one atlas. The near tile is 4096 at the 1024 setting and 8192 at 2048; atlas allocation is approximately 80/320 MiB. Treat this as an active target-hardware performance consideration.

Wheel zoom extends the authored range down to 15%; window ground-ray aiming and presentation share the same camera pose.

Successful skills cancel only unreleased basic casts. Input observes the last completed simulation tick. A new press replaces previous buffered intent, instant skill release preserves the buffer, and charged release applies only to its original slot. Swept obstacle/actor contact ordering blocks both sides' projectiles; melee, AoE, and explosive derivatives use obstacle visibility. Applied damage-over-time remains active.

### Environment v9

Environment v9 is the active baseline: PCA Gaussianized terrain, BC7 linear Gaussian textures with per-mip inverse LUT, deterministic clustered grass, authored environment meshes with LODs, and a 32-bit position GBuffer. Detailed source/output constraints are in `Docs/ENVIRONMENT_V9_REPORT.md`.

### Monster presentation

All three normal-enemy roles use the authored SlimeFamily assets. Superseded BasicSlime, Cactus, Mushroom, Slime, Swarm08/09, and duplicate `SourceExport` sources are not part of the active content tree. Details of the active family are in `Docs/SLIME_FAMILY_REPORT.md`.

Automated validation establishes only the checks it executes; do not infer presentation quality from process completion or zero D3D12 validation errors.

## Accepted baseline

- Slime eye-socket repair is the current baseline.
- The relic roster contains 20 relics; current tuning state is in `RELIC_BALANCING_PLAN.md`.
- Relic contribution is exposed in the character and result UI.
- Current UI text measurement, clipping, card layout, relic display, bars, and combat-HUD fixes are integrated.

Do not repeat completed relic census/tuning matrices unless a change invalidates their evidence.

## Tooling qualification in progress

`msvc-asan-core` qualifies only `simulation.determinism-core`; it is not a full-project gate. Tracy/MCP qualification remains inconclusive. CodeGraph and workflow-skill comparisons remain in progress. No batch-wide tooling choice is established.
