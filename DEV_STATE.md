# DEV_STATE

## Objective

Split the monolithic gameplay test executable by preserved CLI/CTest groups and
make authored GameData the production source of truth for deterministic
`SimulationRules`.

## Implemented State

- `hs_gameplay_tests` is split into shared support plus content, combat,
  progression, presentation-contract, and determinism translation units. Existing
  group names and CTest names are unchanged.
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

## Verification

- `hs_content --validate-only`: PASS, 13 documents, source hash
  `4739930181907566560`.
- Required Debug CTest groups: PASS, 6/6 (`architecture.layers`, content, combat,
  progression, presentation-contract, determinism).
- `cmake --workflow --preset verify-core`: PASS, Debug build and 5/5 tests.
- Full `msvc-debug` configure/build: PASS.
- Full Debug CTest run with graphics/process access: PASS, 21/21, including
  render smoke, VFX showcase, barrier comparison, and shader hot reload.
- `git diff --check`: PASS; line-ending conversion warnings only.

## Remaining Acceptance

Automated Debug build, content, gameplay, runtime, and GPU checks are complete.
Final gameplay feel and visual approval remain a user play-review decision.
