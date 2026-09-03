# DEV_STATE

## Objective

Complete the remaining repository-implementable items recorded in the design
documents, including readable animated monster presentation and higher-quality
skill VFX, without inventing speculative architecture.

## Implemented State

- `hs_gameplay_tests` now also separates boss, skill, upgrade, and relic coverage
  while preserving the existing CLI and CTest groups.
- `SimulationRules` carries fixed-layout typed rules for stats, status,
  progression, character initialization, enemy scaling and geometry, relic drops,
  spawn placement, bosses, and all skill upgrades.
- The content cooker starts from zero-initialized game data, validates exact
  authored IDs/order/parameter keys, converts seconds to 60 Hz ticks, and fills
  fixed arrays directly. It no longer uses defaults as a hidden production input.
- Gameplay combat, progression, spawning, boss scheduling, read models, and
  initialization consume the typed cooked rules. Public gameplay APIs and the
  deterministic phase order remain unchanged.
- Follow-up defects exposed by authored values were corrected: ranged warning
  extent uses projectile range, all active skills receive skills-wide level
  metadata, relic-drop gating is correctly parenthesized, and kill-spawned small
  traps choose the nearest surviving enemy independently of trigger radius.
- Independent review follow-ups connect authored shockwave gap/width/interval
  fields to collision, enforce stat allocation ABI order and fixed status
  capacities, and keep legacy compatibility aliases out of the gameplay hash.
- Authored camera settings feed both Runtime camera behavior and the cooked
  maximum-zoom spawn footprint. Normal enemies use deterministic offscreen
  candidates and fall back to a visible 0.5-second edge warning before spawn.
- Boss and normal-enemy spawn warnings project to visible world effects.
- Spawn-warning VFX parameters retain the valid default forward direction, so
  scheduled boss and fallback-enemy warnings satisfy the runtime VFX contract.
- Spatial-grid candidate selection has a randomized brute-force differential test.
- XAudio2 source voices are retained and reused for compatible wave formats;
  missing-cue behavior remains covered automatically.
- All 148 supplied FBX files were import-checked and the original export bundle is
  retained under `ContentSource/Archive/Monsters/2026-08-24-unreal-export`.
  Six self-compatible model and animation families are authoritative content:
  Slime/Cactus/Swarm09 for melee/ranged/suicide roles and Turtle Shell/Chest
  Monster/Beholder for the three bosses. The existing five-clip character format
  cooks all seven character assets, and D3D12 renders each monster family as one
  instanced skinned batch.
- The shared monster `DefaultPBR_MAT` Base Color, Emissive, and RAM inputs are
  authoritative under `ContentSource/Textures/Monsters/PBR`, cooked to DDS, and
  sampled for all monster batches. The renderer uses the project's stylized
  lighting approximation rather than reproducing Unreal's material graph.
- Monster presentation uses uniform role-specific scales without changing
  gameplay collision: normal Slime/Cactus/Swarm09 are 1.50/2.00/2.40 and
  5m/10m/final bosses are 4.23/4.71/3.51, respectively. The content cooker
  rejects unweighted control points, the renderer validates every cooked skin
  influence, and a muted 4K preview path isolates any of the six assets and five
  clips for visual inspection.
- Particle format v4 adds a renderer-bounded `Flame` primitive. Sprite and ground
  flames use procedural four-octave warped noise, tapered density, HDR thermal
  color, lifecycle fade, soft depth intersection, and weighted OIT rather than
  sampling the authored mask texture. Fire status, explosions, boss fire, and
  fire-area effects use the primitive while retaining secondary sparks/shards.
- Segment VFX now preserve authored UV repeat/scroll through the runtime-to-GPU
  command path and sample their mask as a flowing ribbon without rotating the
  underlying trail geometry.
- Missing presentation coverage now has dedicated authored effects for normal
  enemy spawn warnings, XP collection, cooldown surge/refund, tracking and
  afterimage arrows, revival, and boss death. Traps visibly transition from
  pending to armed, and Retreat Shot's slow-trail upgrade projects along the
  actual movement segment instead of as one circular patch.
- Ranged enemies retain their locked attack aim while stationary, so their
  rendered yaw continues to face the projectile release direction. Normal enemy
  melee, ranged-release, suicide-charge, and suicide-explosion events all have
  catalog coverage; the previously tiny ranged burst is enlarged and lengthened.
- Monster animation cooking preserves reflected FBX root handedness when
  decomposing rebased local matrices and verifies exact reconstruction. Runtime
  skinning rejects non-positive determinants instead of accepting reflected
  poses that can appear as split or inside-out meshes.

## Verification

- `hs_content --cook`: PASS, 13 documents, 7 character assets, source hash
  `18268135721575879612`.
- `cmake --workflow --preset verify-core`: PASS after a clean Debug rebuild,
  5/5 tests.
- Full `msvc-debug` configure/build: PASS.
- Full Debug CTest run with graphics/process access: PASS, 21/21, including
  render smoke, VFX showcase, barrier comparison, and shader hot reload.
- Current VFX/scale delta: full Debug build and shader compilation PASS;
  `vfx.showcase` PASS with zero validation errors; the clean-rebuilt
  `runtime.integration` PASS. The earlier all-suite pass was not repeated after
  these targeted fixes to avoid redundant GPU verification.
- Monster animation/scale delta: clean `msvc-debug` build PASS. All 30 asset/clip
  combinations were captured at normalized times 0.10, 0.50, and 0.90 from
  yaw 0/90/180/270 (360 captures) under `Artifacts/monster_animation_matrix`.
  The HWND swap chain limited the requested 4K size to 1920x1080, so centered
  source-pixel crops were inspected at enlarged scale. All 360 D3D12 validation
  logs are empty. The cooked FBX inverse bind must be transposed into DirectX
  row-vector layout before multiplying by the runtime global transform.
- Monster boundary repair: Cactus, TurtleShell, and Beholder now use the
  Geometry Script closed meshes from `temp`, with only their generated cap UVs
  repacked into nondegenerate 0.01 atlas islands. Debug cook/build and targeted
  content/render/runtime tests pass. All 180 final top/bottom captures across
  five clips, three times, and yaw 0/180 pass with empty D3D12 validation logs
  under `Artifacts/monster_boundary_final_audit`.
- Normal-enemy concept mapping is wired to Slime/Cactus/Swarm09 with each model's
  own animation family, and the Debug cook/build plus five targeted tests pass.
  The character cooker now normalizes per-triangle winding against the authored
  normals after DirectX axis conversion, preserving back-face culling without
  hiding valid surfaces.
  However, the 180-capture machine audit under
  `Artifacts/concept_monster_final_audit` only proves process completion and
  empty D3D12 validation logs: enlarged visual review still shows malformed
  silhouettes/poses, so this presentation change remains unresolved and is not
  accepted.
- 4K upgraded Explosive Arrow capture: PASS, 181 live GPU particles and zero
  validation errors at tick 30 under
  `Build/msvc-debug/Artifacts/explosive-fire-close`.
- 8K monster-material render experiment: PASS, assertions passed, enhanced
  barriers enabled, zero D3D12 validation errors; artifacts under
  `Build/msvc-debug/Artifacts/monster-pbr-8k-current`. Scheduled 5-, 10-, and
  15-minute boss render experiments also complete after the warning-VFX fix.
- Debug combat acceptance: PASS, 18 minutes, 3 builds × 3 seeds; report at
  `Build/msvc-debug/Artifacts/acceptance-combat/combat_report.json`.
- Debug progression acceptance: PASS, 20 minutes, 18 profiles × 3 seeds; report
  at `Build/msvc-debug/Artifacts/acceptance-progression/progression_report.json`.
- `git diff --check`: PASS; line-ending conversion warnings only.
- Missing-VFX delta: Debug content cook/build PASS; content, combat,
  progression, presentation-contract, and runtime integration tests PASS.
  A muted 60-tick direct GPU smoke passes with enhanced barriers and zero
  validation errors under `Build/msvc-debug/Artifacts/vfx_missing_audit`.
  The full CTest GPU wrappers were not re-run successfully because their shared
  `hs_playtest` path was access-denied; the direct no-recording smoke avoids that
  unrelated artifact-permission failure.
- 2026-08-30 enemy presentation/animation delta: clean RelWithDebInfo rebuild
  and CTest PASS, 20/20. Targeted content, combat, presentation-contract, and
  runtime tests PASS. The formerly failing Cactus clip-2 time-0.5 close-up now
  renders the full mesh; final multi-clip visual-quality approval remains manual.
- 2026-08-30 Slime eye-socket repair keeps the original 66,976-byte FBX and
  applies one fail-closed cooker repair after metre/axis conversion: exactly one
  10-edge loop receives eight skinned triangles with regenerated normals and
  tangents. Cooked `enemy_melee.meshbin` is 3,051 vertices / 1,017 triangles;
  Idle, Run, both attacks, and Death D3D12 previews complete successfully.
- 2026-09-02 continuation found the first Debug render crash was a stale-object
  ABI mismatch after the expanded read-model balance summary. The crash mapped
  to `GameSimulation::WriteReadModel`; a clean MSVC Debug rebuild fixed it and
  `integration.render-smoke` passes again. A fresh, non-repeated Slime-only gate
  captured 5 clips x 3 normalized times from the front under
  `Artifacts/slime_eye_clean_front`. All 15 runs complete with empty D3D12
  validation logs, and enlarged review shows no detached triangles or open eye
  socket. Attack/death frames do close or occlude the eye as part of the authored
  pose; final motion-quality approval remains manual.
- Relic ABI/content/gameplay now contains 20 relics. Six appended relics are
  universal; Slow Synergy and Area Resonance require the Slow build tag. The
  54-run progression matrix is valid and observes all new offensive/utility
  effects; dedicated gameplay tests verify 20% guard and 30% low-health damage
  reduction. Final RelWithDebInfo CTest is PASS, 20/20.
- Relic balance telemetry now records relic kills, exact prevented damage, and
  Pickup Reward amplified damage while preserving upgrade/relic provenance.
  CombatSim has a paired single-skill relic mode. Its 60-second baseline completed
  12,600 OFF/ON pairs (25,200 builds), covering all 9 single-skill cases, all 70
  four-upgrade sets per skill, and all 20 relics with matching archetype spawn
  counts. Multi-skill and condition-specific high points remain pending. Detailed
  results and staged multi-skill/conditional follow-up are in
  `RELIC_BALANCING_PLAN.md`.
- The next balance gate was intentionally reduced to nine evidence-selected
  OFF/ON pairs at five minutes and five seeds (90 runs); no prior scan or smoke
  was repeated. The run passed with matching paired spawn counts under
  `Build/msvc-validation/Artifacts/relic-balance-focused`. Remaining work is
  four minimal condition cases, not another combination census.
- The all-metric relic gate is complete. Ten missing condition pairs ran at
  three minutes x five seeds; only four invalid
  inputs plus the deterministic revive-invulnerability pair were rerun. Telemetry
  now includes relic-owned DOT damage/events/kills,
  effect uptime, healing, ending health, survival, and enemy attack denial.
  Runtime playtest output now includes relic-attributed kills and uses the same
  public relic IDs as CombatSim/content.
  A malformed revive retry key unintentionally ran a six-second, one-seed full
  matrix smoke; it was discarded and no prior full-duration suite was repeated.
  Candidate-only tuning is now applied and verified. Same-tick damage no longer
  advances relic hooks or overwrites kill attribution after target health reaches
  zero. New isolated cases resolved Basic Kill Tracker and Combat Hit Chain;
  candidate-only tuning reports cover only affected candidates. Basic Kill Tracker
  settles at 20% tracking damage with +14.42% mean/median/P95 in its isolated
  high-point case. Final state is 13 keep and 7 keep/watch with no correctness holds.
  Exact values, causal multi-axis evidence,
  and report paths are in `RELIC_BALANCING_PLAN.md`.
- A constraint/utility retier now rewards build-locked effects at roughly 1.5-2x
  broad baseline while valuing cooldown, casts, healing, prevention, and control
  separately from DPS. Candidate-only Round 10/11 verifies: Bleed Kill Heal 5 HP;
  Kill Cooldown Surge 11.62 cooldown sec/min and +16.6 casts; Sixth Basic Radial
  +14.76% mean/+17.31% P95; Alternating Active 14.72 cooldown sec/min; Projectile
  Cadence +9.6 casts; Area Resonance +10.49% DPS; Low Health Survival 122.67
  prevention/min. No completed matrix was repeated.
- Relic presentation now exposes all 20 owned relics by name instead of a hex
  mask. The character relic page shows per-relic activations, damage, kills,
  cooldown saved, damage prevented, and healing, and its four-column cards stay
  inside the character panel. The result screen also lists relic contribution.
  New relics 13-20 emit one source-tagged `RelicTriggered` domain signal per
  activation and project to eight distinct primitive-based VFX assets. A clean
  RelWithDebInfo rebuild and the full validation suite pass, 20/20; the VFX
  showcase capture is mechanically valid, with final in-game readability still
  requiring visual approval.
- UI rendering now honors authored text colors, applies consistent button/panel
  padding and rounded corners, and renders the complete bounded UI model rather
  than silently dropping entries after 32. The combat HUD has a readable dark
  backing panel; selection cards separate category-colored titles, descriptions,
  and actions; the result screen shows readable skill levels and upgrade counts
  instead of hexadecimal masks. RelWithDebInfo CTest passes 20/20, and a fresh
  render-smoke capture confirms intact HP/XP labels and the updated HUD styling.

## Remaining Acceptance

Automated Debug build, content, gameplay, runtime, GPU, and long-session checks
are complete. Monster concept mapping is mechanically wired but its visual
correctness is unresolved after enlarged multi-angle review; texture mapping,
icons, UI art, approved VFX quality, target-hardware performance measurements,
gameplay feel, and final visual/audio approval remain acceptance items.
Compatible source-voice reuse, 3D audio, and pause/resume still require a real
audio-device listening pass because silent fallback cannot exercise that path.
