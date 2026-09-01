# Slime Animation Repair and Relic Expansion Plan

## Objective

- Remove the animation-dependent opening above the Slime eyes without changing
  unrelated monster topology or hiding the problem with two-sided rendering.
- Expand the relic roster from 12 to 20 while preserving deterministic
  simulation, existing save state, content ownership, and the current average
  acquisition target.
- Balance all 20 relics from reproducible scenario telemetry, followed by user
  gameplay and visual approval.

## Current Evidence

### Slime eye opening

- The cooked Slime contains 29 real boundary edges in three closed facial loops
  of 12, 10, and 7 vertices. The body has a 10-edge eye-socket opening; the
  separate eye/cover shells have open rear rims.
- The socket rim blends EyeBall, Head, EyeCTRL, and UpperEyeCover influences,
  while the facial shells are rigid to individual bones. Their maximum measured
  relative displacement is approximately 16 mm in Idle, 39 mm in Run, 33 mm in
  Attack01, 45 mm in Attack02, and 70 mm in Death before presentation scale.
- The cooked data has normalized weights, no coincident bind-position groups
  with different weights, no degenerate eye triangles, and no eye-region
  triangle flips across 273 cooked frames. This makes authored open-shell
  overlap exposure the leading cause, not general skin tearing, lost root
  handedness, nearest-frame sampling, or four-weight truncation.
- Unreal normalizes vertex skin weights and supports fixed four/eight-influence
  GPU skinning. Matching that behavior remains an invariant, but increasing the
  influence count is not justified by the current Slime evidence.

### Relic system

- `RelicKind`, `kRelicCount`, JSON ordinal, cooked arrays, rule-table order,
  presentation arrays, telemetry, and tests form one fixed indexed ABI.
- The current roster has 12 relics and uses a 16-bit acquisition mask. Twenty
  relics require a 32-bit mask and an explicit save/replay migration.
- The result UI assumes a 6-by-2 relic layout. It must be paged or reflowed
  before a thirteenth relic is exposed.
- The last progression report averaged 6.54 relics per session over 54 runs,
  close to the authored target of 6, but it predates this plan and must be
  regenerated before tuning.
- Existing reports do not exercise every relic fairly: stationary/invulnerable
  automation under-represents movement, damage, recovery, and revive effects.
  Zero triggers in that harness are not evidence that a relic is weak.

## Phase 1: Repair the Slime Eye Region

1. Freeze the reproducer.
   - Capture asset 0 at yaw 180, 150, and 210; pitch 10; distance about 3.
   - Minimum clip/time set: Idle 0.58, Run 0.0/0.5, Attack01 0.16,
     Attack02 0.72, Death 1.0.
   - Capture the same normalized poses in Unreal from the authoritative source.
   - Run one temporary `CullMode=None` diagnostic capture only to confirm that
     the symptom is exposure of an open/rear shell. Do not ship two-sided mode.

2. Select the repair from the comparison.
   - If Unreal shows the same exposure, edit only the Slime eye socket: extend
     the shell overlap or add a small internal backing surface attached to the
     appropriate head/eye influence set.
   - If Unreal is clean, compare exact bone globals and AnimationSequence
     curves/post-processing with the exported FBX before changing topology.
   - Do not run global `Fill All Holes`; the facial shells and other monster
     openings may be intentional.

3. Re-export and validate the named asset only.
   - Preserve the original source and perform the edit on a targeted duplicate.
   - Verify topology loop counts, skin-weight normalization, material slots,
     bounds, headers, manifest, and SHA-256 before replacing the Project HS
     authoritative Slime FBX.
   - Re-cook from source; do not patch `meshbin` output.

4. Acceptance gate.
   - The minimum matrix above and all five clips at times 0.1/0.5/0.9 must show
     no eye opening from the three front-facing yaws.
   - No new face triangle inversion, silhouette change, floating eye shell, or
     D3D12 validation error.
   - Full `verify` passes; final visual approval remains with the user.

## Phase 2: Freeze the 12-Relic Baseline

1. Re-run the current clean build and record content hash, simulation version,
   combat acceptance, progression acceptance, and deterministic checksums.
2. Add separate baseline scenarios for:
   - no relics;
   - each existing relic alone;
   - current representative basic, status-chain, and projectile-chain builds;
   - movement, incoming-damage/low-health, boss single-target, dense crowd,
     persistent-area, charged-skill, and pickup-economy behavior.
3. Record damage, effective healing, prevented damage, cooldown ticks saved,
   extra targets, activation rate, deaths, clear time, entity peaks, and CPU
   tick p50/p95. Do not reduce every effect to DPS.
4. Keep existing 12-relic values unchanged until the measurement gaps and
   zero-trigger scenarios are fixed.

## Phase 3: Expand the Relic ABI to 20

1. Append eight `RelicKind` values; never reorder the existing twelve.
2. Change relic masks from `uint16_t` to `uint32_t` in Gameplay state,
   observations, rule-table input, checksums, UI/debug formatting, CombatSim,
   playtest recording, tests, and all full-mask comparisons.
3. Preserve the low twelve bits when loading existing saves/replays. Decide
   separately whether the new eight are automatically unlocked.
4. Increment the simulation/gameplay/cooked schema versions required by the ABI
   change and verify deterministic migration behavior.
5. Expand JSON schema/count validation, `RelicDefinitions`, cooker mapping,
   prerequisite tags, presentation names/rules, debug ID tables, telemetry
   arrays, and result serialization.
6. Reflow or paginate the HUD/result display for twenty entries. Treat icon
   rendering as separate scope because `icon_asset_id` is currently metadata.
7. Verify this infrastructure with placeholder-disabled definitions before any
   existing relic number is changed.

## Phase 4: Specify and Implement Eight Relics

Use these non-overlapping design slots; exact names, numbers, and wording require
product approval before implementation.

1. Projectile-hit or pierce reward using the currently unused
   `OnProjectileHit` hook.
2. Pre-damage mitigation using the currently unused `BeforeDamage` hook.
3. Slow-status synergy, complementing the existing bleed and burn relics.
4. Persistent-area/trap synergy for area-focused builds.
5. Boss/single-target pressure that does not depend on normal-enemy kills.
6. Full-charge behavior reward distinct from generic cooldown refunds.
7. XP/heal/magnet pickup interaction; add `OnPickupCollected` only if the final
   design truly needs information absent from existing hooks.
8. Repeatable low-health survival distinct from knockback and one-time revive.

For every relic, approve one compact contract before coding:

- trigger and eligible source (`Original`/`Derived`);
- target selection and deterministic ordering;
- initial authored parameters;
- internal cooldown and per-cast/per-target/per-session cap;
- whether it can create another relic/upgrade trigger;
- prerequisite build tags;
- telemetry metric and expected scenario;
- reused or new VFX/audio signal.

Prefer existing hooks, `EffectOrigin`, caps, catalog effects, and telemetry.
Add a new hook or state field only where an approved effect cannot be expressed
correctly with the existing model.

## Phase 5: Balance the 20-Relic Roster

1. Run all 20 relics alone against the no-relic paired-seed baseline.
2. Run every two-relic pair (`C(20,2) = 190`) as deterministic smoke coverage.
3. Run selected three-to-six-relic builds for status chains, projectile fanout,
   persistent areas, boss damage, movement, defense, and pickup economy.
4. Tune one axis at a time: trigger frequency first or effect magnitude first,
   never both in the same comparison.
5. Retain an average of 5-7 relic acquisitions over the 54-run progression
   suite. Do not raise acquisition count merely because the pool is larger;
   improve prerequisite-aware offerings if build coherence falls.
6. Flag a single relic when its contribution exceeds twice the median of its
   role peers. This triggers review rather than an automatic nerf because
   damage, defense, and utility are not directly interchangeable.
7. Investigate CPU tick p95 regressions of 5% or more per relic; reject 10% or
   more until the cause is corrected.

## Final Acceptance

- Existing saves retain the original twelve acquisition bits.
- All twenty relics can be naturally offered and produce a nonzero useful
  result in their dedicated scenario.
- No recursive derived-effect chain, telemetry overflow, entity-cap overflow,
  or relic-order nondeterminism.
- Same seed and input produce the expected checksum for the new version.
- Content cook; relic, content, progression, determinism, presentation, and
  runtime tests; 190 pair smokes; long acceptance suites; and
  `cmake --workflow --preset verify` pass from a clean MSVC build.
- Automated evidence is separate from final gameplay feel, effect readability,
  Slime visual quality, and user approval.
