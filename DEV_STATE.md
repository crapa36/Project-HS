# DEV_STATE

## Objective

Match the detailed player-skill VFX appearance specification and requested
skill balance through the existing gameplay, particle, ground, line, and
snapshot paths while preserving deterministic simulation.

## Verified State

The low-level and full validation workflows pass on the current checkout.

Charged-shot guide center, length, yaw, and half-width derive from the live
player position, aim, charge range, and charge collision radius. Four continuous
SolidTrail segments show the two side borders and near/far caps without a dotted
trajectory. Segment orientation is preserved through the GPU particle path.
Projectile collision radii remain in the gameplay read model while body visual
sizes use explicit per-skill presentation values.

Projectile trails begin exactly at the arrow body's rear, include the renderer's
half-height body-center offset, and interpolate from the same snapshot pair as the body.
Their shader mask tapers toward the tail and fades over the short particle life.
The short Basic Attack and Multi Shot trails remain narrower than their arrow
heads but stay above the normal-camera one-pixel detail threshold.
The trail alpha fades once through its start/end color instead of being
multiplied by a duplicate lifetime fade in the shader. Slow-area chevrons remain
an interior secondary motif rather than a competing full-radius boundary.

The common VFX grammar now distinguishes directional slashes, radial fracture,
expanding explosion shells and debris, inward pull chevrons, forward push
sectors, ground status marks, and compact apply/tick status intensity. Ground
arrows and velocity-facing ground motifs are rendered by their intended masks
instead of falling through to the generic crack/octahedron path.

Ground Ring strokes and Rune outer-ring strokes render at one quarter of their
previous width while preserving their center radius and gameplay boundary.

Piercing Shot uses the restored double-size arrow body
`(0.28, 0.28, 1.25)` without changing its collision. Charged Shot collision
radius now interpolates from `0.4` to `0.88` metres and its projectile body
from `(0.228, 0.228, 0.81)` to `(0.42, 0.42, 1.26)`, half of the immediately
preceding values. Its doubled cooldown and damage and two additional pierces
remain unchanged. Enemy pull and push displacement is applied over eight
deterministic fixed ticks instead of teleporting.

Burn and Bleed status masks continuously emit sprite particles around affected
enemies. Burn rises around the body with the fire sheet; Bleed drops with
gravity using the blood sprite.

## Changes

`.github/workflows/foundation.yml` delegates configuration, build, and test
selection to `cmake --workflow --preset verify-core`.

Presentation now emits stable persistent VFX for charged-shot range guidance,
Arrow Rain areas, fire/slow/trap zones, damage trails, and player projectile
trails. Arrow Rain uses `ArrowRainArea`, and Trap uses `TrapArmed`, from
placement through expiry with the same stable ID instead of switching from a
separate pre-active kind. Slow-only Arrow Rain follow-ups no longer retain the
damaging-area motif.

The renderer now supplies a negligible direction-carrier velocity to Segment
particles because the GPU derives their axis from initial velocity. Charged
Shot was captured against the distinct diagonal world target `(9, 6)`.

All nine player attacks were cast through the runtime capture path against
`(9, 6)` with upgrade mask `255`. Twelve frames cover Basic Attack, Piercing
Shot, Multi Shot, Charged Shot charge/release, Explosive Arrow, Ricochet Arrow,
Arrow Rain early/late, Trap early/late, and Retreat Shot. The long Piercing Shot
strip is its live 24 by 1 metre upgraded damage area, not a projectile trail;
overlapping Arrow Rain, Trap, and Retreat Shot rings are distinct live upgraded
areas and displacement boundaries.

Gameplay VFX signals now use actual effect radii and displacement directions,
emit one Multi Shot cast effect per fan, suppress derived projectile cast
duplicates, and expose previously missing slow, push, transfer, retreat, and
charged explosion events. Content compositions provide a primary/secondary
hierarchy for hits, explosions, heavy hits, statuses, healing, pull, and push.

Charged Shot uses a narrow inner trail plus a faint outer energy layer while
Basic Attack and Multi Shot keep one compact trail. Ricochet Shot no longer
emits an ordinary next-target dotted link and instead uses a continuous,
fading projectile ribbon. The charge guide is a continuous outlined gameplay
footprint matching its live collision width and range. Piercing and other
damage strips use two borders with an interior dash, Arrow Rain areas use small
arrow marks, range indicators use small inward chevrons, and trap triggers
expand from the trap position at the live effect radius.

## Verification

- `cmake --workflow --preset verify-core`: PASS, 5/5 tests.
- `cmake --workflow --preset verify`: PASS, 20/20 tests.
- Final `ctest --test-dir Build/msvc-validation -C Debug`: PASS, 20/20 tests.
- Enemy displacement interpolation test: PASS; the first push tick is partial
  and the full requested distance is reached after eight ticks.
- `vfx.showcase`: PASS with `execution_valid=true`,
  `assertions_passed=true`, 16 rendered particles, and an empty D3D12
  validation log.
- Charged Shot diagonal capture: PASS at tick 60 with 7 rendered particles and
  an empty D3D12 validation log.
- Basic Attack compact-arrow capture: PASS at tick 20 with an empty D3D12
  validation log.
- Actual-cast VFX audit: PASS, 12/12 captures report
  `execution_valid=true`, `assertions_passed=true`, one rendered frame, and a
  zero-byte D3D12 validation log.
- Detailed-spec capture audit: PASS for the 12 actual-cast frames under
  `Artifacts/vfx-spec-20260821`; all report valid execution and assertions and
  all 12 D3D12 validation logs are empty.
- Enlarged visual audit: PASS using the original 1920 by 1080 frames and 2x
  nearest-neighbour crops under `Artifacts/vfx-spec-20260821/zoom`. Projectile
  silhouettes, distinct diagonal targets, persistent-area boundaries, ground
  motifs, and primary/secondary hierarchy remain readable at normal camera.
- Quarter-width ring audit: PASS for Arrow Rain, Trap, and Retreat Shot under
  `Artifacts/vfx-ring-quarter`; 3/3 captures are valid with empty D3D12 logs.
- Charged outline and Ricochet ribbon audit: PASS under
  `Artifacts/vfx-indicators-20260822`; actual-cast captures report valid
  execution and assertions, and 2x crops confirm continuous boundaries/trails.
- Scale and unified-area audit: PASS under
  `Artifacts/vfx-scale-unified-20260822`; eight actual-skill captures exited
  successfully, each rendered its requested tick with zero renderer validation
  errors, and 2x crops cover Charged Shot, Piercing Shot, Arrow Rain before and
  after activation, and Trap before and after activation.
- `git diff --check`: PASS.

## Current Failures

None.

## Rejected Hypotheses

Do not couple projectile body scale to collision diameter. Do not add a second
VFX framework or copied radius state while the existing snapshot primitives
and live read model express the required behavior.

## Remaining Work

Final in-game grayscale readability, effect-noise, stress-scene, and gameplay
quality approval remains with the user; automated artifacts report
`user_review: awaiting`.
