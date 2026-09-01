# Relic Balance Test Results and Plan

## Verified baseline

- Harness: `hs_combat_sim --relic-balance-suite`
- Matrix: 9 skills x all 70 four-of-eight upgrade sets x 20 relics.
- Pairing: one relic-OFF and relic-ON build per combination with the same seed.
- Executed baseline: 12,600 pairs / 25,200 builds / 60 seconds / seed 1001.
- Single-skill four-upgrade coverage and paired archetype spawn-count validation: complete.
- Detailed report: `Build/msvc-validation/Artifacts/relic-balance-full-60s/combat_report.json`.

The 60-second single-skill scan observed these maximum absolute DPS deltas:

| Relic | Mean | P95 | Maximum | Highest observed build |
|---|---:|---:|---:|---|
| Combat Hit Chain | +0.45 | +1.00 | +1.33 | Explosive Arrow, mask 113 |
| Basic Kill Tracking Arrow | +0.55 | +1.17 | +1.17 | Trap, mask 90 |
| Kill Cooldown Surge | +0.08 | +0.43 | +0.83 | Explosive Arrow, mask 120 |
| Slow Synergy | ~0 | 0 | +0.50 | Basic Attack, mask 57 |
| Projectile Cadence Reward | +0.03 | +0.33 | +0.50 | Explosive Arrow, mask 120 |
| Bleed Burn Explosion | ~0 | 0 | +0.33 | Basic Attack, mask 156 |
| Sixth Basic Radial | +0.03 | +0.08 | +0.08 | Retreat Shot, mask 106 |
| Burn Spread on Kill | ~0 | 0 | +0.02 | Basic Attack, mask 92 |

Zero in this scan means the stationary, invulnerable, single-skill scenario did
not exercise the condition. It is not evidence that the relic is weak.

## Telemetry contract

Keep these values distinct in every result:

- `uses`: attacks or skill casts initiated.
- `casts_with_hit`: distinct casts that hit at least one target.
- `hit_events`: applied damage events.
- `activation_count`: relic condition completions.
- `relic_damage`: direct or attributed amplified damage caused by the relic.
- `relic_kills`: enemies whose final damage source was the relic.
- `cooldown_seconds_saved`: actual active cooldown time removed.
- `damage_prevented`: actual incoming damage removed after rounding.
- `damage_over_time`, `damage_over_time_events`, `damage_over_time_kills`: relic-owned
  status damage, ticks, and final blows.
- `effect_active_ticks`: relic-owned status or timed-buff uptime.
- `healing`, ending health, and survival-time delta: defensive value that is not
  visible in DPS.
- Enemy attack-attempt, hit, and damage deltas: control value from slow and
  displacement.
- `DPS delta`, `kills delta`, `casts delta`: relic-ON minus relic-OFF for the same seed.

Reports must retain active time, total damage, DPS, kills, casts, hit events,
activation count, damage per activation, cooldown saved per minute, and
damage prevented per minute. Indirect cooldown and attack buffs are judged by
paired total deltas, not only by direct relic damage.

## Power targets by condition cost

Targets apply to multi-seed median and P95 paired uplift, not a single run.

| Condition cost | Relics | Target sustained DPS uplift | High-point ceiling |
|---|---|---:|---:|
| Automatic / normal loop | Kill Cooldown Surge, Combat Hit Chain, Projectile Cadence, Hit Streak, Pickup Reward | 4-8% | 12% P95 |
| Narrow action or phase | Sixth Basic Radial, Basic Kill Tracker, Area Resonance, Boss Pressure | 7-12% | 18% P95 |
| Multi-action / reactive | Alternating Active, Cross Active, Damage Knockback, Pre-Damage Guard, Slow Synergy | 10-16% equivalent value | 22% P95 |
| Build-locked or risky | Bleed Kill Heal, Burn Spread, Bleed Burn Explosion, Movement Echo, Low Health Survival, Once Revive | 14-22% equivalent value | 30% P95 |

For defensive relics, convert prevented damage and survival time to equivalent
value; do not force a DPS target. A harder condition may exceed the automatic
tier only while its condition is genuinely maintained.

## Focused follow-up result

The exhaustive follow-up was reduced to nine evidence-selected pairs at five
minutes and five seeds: 90 runs total. No previous scan or smoke was repeated.
The report is `Build/msvc-validation/Artifacts/relic-balance-focused/combat_report.json`.

| Relic | Median uplift | P95 uplift | Mean activations |
|---|---:|---:|---:|
| Combat Hit Chain | 1.24% | 3.29% | 24.8 |
| Basic Kill Tracking Arrow | 0.80% | 1.11% | 113.0 |
| Kill Cooldown Surge | 1.53% | 2.28% | 32.0 |
| Projectile Cadence Reward | 2.81% | 5.22% | 31.8 |
| Slow Synergy | 18.65% | 20.36% | 278.8 |
| Bleed Burn Explosion | 24.61% | 29.05% | damage-triggered |
| Area Resonance | -0.53% | 0.32% | 102.8 |
| Alternating Active Refund | 1.28% | 3.94% | 53.0 |
| Cross Active Tracking Arrow | 0% | 0% | 0 |

Slow Synergy and Bleed Burn Explosion are near their intended high-cost ceilings,
not above them. Projectile Cadence is inside the automatic tier. Combat Hit
Chain, Basic Kill Tracker, Kill Cooldown Surge, Area Resonance, and Alternating
Active are below their target bands in this workload. Cross Active is unmeasured:
the round-robin policy did not hit the same target with two different active
skills inside its two-second window.

## Reduced next stages

Do not repeat the exhaustive scan, its smoke run, or this focused gate.

1. Add one deterministic shared-target case for Cross Active Tracking Arrow.
2. Add one minimal incoming-damage case covering Guard, Low Health, and Revive.
3. Add one movement case for Movement Echo and one boss case for Boss Pressure.
4. Add one pickup case at normal cadence. Do not enumerate low/medium/high cadence.
5. Tune only relics that remain outside their band in the matching condition,
   changing one parameter and rerunning only that relic's pair.

The discarded scope is: bottom-five-percent reruns, 10/20-minute repeats, all
28 active-skill pairs, and complete Bleed/Burn/Slow producer enumeration.

## All-metric follow-up and final disposition

The previous disposition did not fully value status damage, uptime, defensive
value, or crowd-control denial. The harness now records every upgrade-effect
metric, relic-owned damage-over-time damage/events/kills, effect uptime, healing,
ending health, survival time, and enemy attack-attempt/hit/damage deltas. Only
the ten relics missing condition-valid evidence were run (100 runs); invalid
Guard, Pickup, Low-Health, crowd-mobility, and revive-invulnerability inputs were
corrected by rerunning only those affected pairs (30, 10, and 2 runs). The revive
case is deterministic and therefore uses one seed. Reports:

- `Build/msvc-validation/Artifacts/relic-balance-all-metrics/combat_report.json`
- `Build/msvc-validation/Artifacts/relic-balance-all-metrics-retry/combat_report.json`
- `Build/msvc-validation/Artifacts/relic-balance-all-metrics-retry2/combat_report.json`
- `Build/msvc-validation/Artifacts/relic-balance-all-metrics-retry3/combat_report.json`

An intermediate revive file used `pairs` instead of the required `cases` key,
which unintentionally launched the default 25,200-build matrix at six seconds
and one seed. That smoke output was overwritten, discarded, and is not used in
any judgment below; no prior full-duration suite was repeated.

Judgment is multi-axis. Direct/status offense uses DPS, net kills, casts, hits,
relic damage, and status kills. Tempo uses actual cooldown removed and verifies
the resulting casts and outcomes. Defense and control use prevention, healing,
survival, status uptime, displacement, and denied enemy attacks. These causal
links are not added together as independent DPS. `relic_kills` is final-source
attribution for diagnosis; paired net-kill delta is the outcome measure.

| # | Relic | All-metric evidence | Final disposition |
|---:|---|---|---|
| 1 | Bleed Kill Heal | 1 hard-condition activation, 1 healing/ending HP; no offense delta | Increase; wounded bleed-final-blow restriction is too costly for 1 HP |
| 2 | Burn Spread on Kill | 72 activations; 296 DOT over 296 ticks; 12 DOT kills; +11 net kills; +10.73% DPS | Slight increase; strong condition remains below the build-locked target |
| 3 | Kill Cooldown Surge | 32 activations; 6.16 sec saved/min; +12 casts; +6.6 net kills; 2.28% P95 | Increase; real tempo conversion is present but below automatic target |
| 4 | Bleed Burn Explosion | 1,137.2 relic damage; 100.6 attributed kills; +59.6 net kills; 29.05% P95 | Keep/watch; justified high point at the build-locked ceiling |
| 5 | Sixth Basic Radial | 35 activations; 273.2 damage; +140.8 hits; +22.2 net kills; 23.72% P95 | Reduce; repeatable narrow-action payoff exceeds its ceiling |
| 6 | Basic Kill Tracking Arrow | 113 activations; 1,019.6 damage; 38.6 attributed kills; only +3.2 net kills and 1.11% P95 | Investigate/hold; attribution and outcome disagree, so tuning now risks masking targeting/overkill |
| 7 | Movement Afterimage Arrow | 36 activations; 216 damage; 27 extra hits; 26.67% P95 | Keep; continuous movement constraint justifies the high point |
| 8 | Alternating Active Refund | 53 activations; 12.02 sec saved/min; +11 casts; +7.6 net kills; 3.94% P95 | Keep/watch; cooldown already converts to outcomes and must not be double-counted |
| 9 | Cross Active Tracking Arrow | 8 activations; 400 damage; 8 extra hits; 17.18% P95 | Keep; difficult shared-target/two-active condition supports the reward |
| 10 | On-Damage Push Slow | 99 activations; 360 slows; 43,200 target-ticks; 1,080 m displacement; 961 attempts/963 hits denied | Reduce; reactive crowd control suppresses nearly the entire six-enemy attack stream |
| 11 | Once Revive | 1 activation; 50 healing; 10 invulnerability prevention; +9 ending HP; +3.1 sec survival; +3 casts/+4 hits | Keep; one-use lethal-pressure condition earns strong recovery |
| 12 | Combat Hit Chain | 24.8 activations; 968.4 damage; 99.2 attributed kills; only +6.8 net kills, -341.6 hits, 3.29% P95 | Investigate/hold; strong attribution conflicts with weak paired outcome |
| 13 | Projectile Cadence Reward | 31.8 activations; 2.98 sec saved/min; +11 casts; +5 hits; +11.8 net kills; 5.22% P95 | Keep; automatic effect lands inside target and converts to kills |
| 14 | Pre-Damage Guard | 30 activations; 180 prevented/60 per min; 2.53% of incoming damage | Increase; easy automatic defense is functional but too small |
| 15 | Slow Synergy | 278.8 activations; 489.8 damage; 190 attributed/+31.2 net kills; +236.2 hits; 20.36% P95 | Keep/watch; Slow prerequisite justifies near-ceiling output |
| 16 | Area Resonance | 100 activations; 200 damage; +100 hits; no net kills; 3.50% P95 | Increase; Slow plus persistent-area restriction is under target |
| 17 | Boss Pressure | 117.2 activations; 351.6 damage; +117.2 hits; 18.94% P95 | Keep/watch; boss-only restriction justifies the marginal 0.94-point ceiling excess |
| 18 | Hit Streak Reward | 16 activations; 80 damage; +16 hits; no net kills; 1.39% P95 | Increase; ten direct-hit gate is under-rewarded |
| 19 | Pickup Reward | 61.4 activations; 181 damage; 9,709 active ticks (~90% uptime); +10 net kills; 20.40% P95 | Reduce; near-permanent easy pickup buff greatly exceeds automatic target |
| 20 | Low Health Survival | 23 activations; 184 prevented/61.33 per min; 2.58% of incoming damage | Increase; risky <=35% HP constraint is under-rewarded |

Final count: seven increase candidates, three reduce candidates, four keep,
four keep/watch, and two investigate/hold. Numeric value edits are intentionally
not applied by this judgment pass; each candidate should change one parameter
and rerun only its own validated pair.

## Applied tuning and final multi-axis judgment

The candidate-only follow-up used the same five seeds and matching OFF/ON builds;
180-second cases were retained except Kill Cooldown Surge, whose prior high point
requires 300 seconds. Completed suites were not repeated. Reports are under
`Build/msvc-validation/Artifacts/relic-balance-tuning-round1-180` through
`relic-balance-tuning-round8`. Round 3 intentionally reused the Round 2 input
after changing only its two cooked values, and Round 6 reused the Round 5 input
after changing only Basic Kill Tracker; the output directory names preserve the
actual tuning sequence. Rounds 7 and 8 have explicit matching input files.

A correctness audit found that same-tick damage queued after a lethal hit could
still count as a hit, dispatch relic hooks, and overwrite final-source attribution.
Damage processing now rejects targets whose health is already zero, and the combat
regression test proves a second same-tick hit cannot advance Combat Hit Chain.
Basic Kill Tracker and Combat Hit Chain were then measured in new, nonlethal or
paired-target isolation cases instead of reusing their invalidated prior results.

| # | Final value/change | Condition-valid result | Final judgment |
|---:|---|---|---|
| 1 | Heal 0.5% -> 5% max HP | 1 activation, 5 healing/end HP, no offense delta | Keep; pure utility reward reflects bleed-final-blow and missing-health restrictions |
| 2 | Copied burn 40% -> 50% | 72 activations, 568 relic-owned DOT, +21 kills, +21.37% DPS | Keep/watch; high but earned by burn final blows plus nearby targets |
| 3 | Cooldown removal 1.0s -> 2.0s per 10 kills | 31.8 activations, 58.11s removed, +16.6 casts, +2.8 kills, +0.77% DPS | Keep; damage-free tempo now produces a clear cast gain |
| 4 | Unchanged | 29.05% P95 in the prerequisite-heavy bleed+burn case | Keep/watch at build-locked ceiling |
| 5 | Eight arrows 45% -> 15% each | 35 activations, 194.6 damage, +16.8 kills, +14.76% mean / 17.31% P95 DPS | Keep; basic-attack-only high point is about 1.5-2x the broad baseline |
| 6 | Tracking arrow 100% -> 20% | Isolated paired targets: 138 activations, 276 damage, +15 kills, +14.42% mean/median/P95 DPS | Keep; difficult paired-target ceiling is below the 18% P95 limit |
| 7 | Unchanged | 36 activations, 216 damage, 26.67% P95 while continuously moving | Keep; movement constraint pays strongly |
| 8 | Cooldown refund 15% -> 20% | 14.72 cooldown sec/min, +6 casts, +0.6 kills, +0.23% mean DPS | Keep/watch; pure utility is stronger but remains sequence- and cooldown-limited |
| 9 | Unchanged | 8 activations, 400 damage, 17.18% P95 | Keep; two different actives on one target is restrictive |
| 10 | Push 3m -> 0.25m; cooldown 0.1s -> 1s | 178 activations, 267m displacement, 128,160 slow target-ticks, 163 hits denied, +3.57% DPS | Keep; reactive defense remains strong without suppressing the whole attack stream |
| 11 | Unchanged | One revive, 50 healing, 10 prevention, +3.1s survival | Keep; once-per-session lethal gate |
| 12 | Unchanged | Four durable targets: 20 activations, 2,000 damage, 100/activation, +11.31% DPS, no false kills | Keep; universal high point is bounded by 12 direct hits and four nearby targets |
| 13 | Cooldown removal 0.5s -> 0.75s per 8 projectile hits | 31.6 activations, 4.62 cooldown sec/min, +9.6 casts, +4.8 kills, +1.49% mean DPS | Keep; projectile-dependent utility gains real tempo without reaching direct-damage tiers |
| 14 | Reduction 15% -> 20% | 30 activations, 240 prevented (80/min) | Keep; automatic defense is useful but cooldown-bounded |
| 15 | Unchanged | 489.8 damage, +31.2 kills, 20.36% P95 | Keep/watch; Slow prerequisite supports the ceiling |
| 16 | Area damage 20% -> 60% | 100 activations, 600 damage, +10.49% DPS | Keep; Slow prerequisite plus area/trap hit double lock earns constrained-tier output |
| 17 | Unchanged | 351.6 damage, 18.94% P95 | Keep/watch; boss-only ceiling |
| 18 | Tenth-hit damage 50% -> 150% | 16 activations, 240 damage, +4.18% DPS | Keep; automatic hit gate reaches the low end of target |
| 19 | Buff duration 3s -> 1.5s | 54.8 activations, 4,767 active ticks, 88 amplified damage, +3 kills, +5.33% DPS | Keep; pickup cadence no longer produces near-permanent uptime |
| 20 | Reduction 20% -> 40% | 23 activations, 368 prevented (122.67/min) | Keep; damage-free low-health utility earns stronger risk compensation |

The final judgment intentionally does not sum correlated measures. For example,
relic damage can cause extra kills, which can remove enemy attacks and change later
hit counts; these are a causal chain, not four independent bonuses. Damage-over-time
is included in relic damage and DPS but remains separately reported for provenance.
Final state: 13 keep, 7 keep/watch, zero unresolved correctness holds.

## Constraint and pure-utility retier

The final retier reads both rule text and actual handlers rather than treating
every trigger as equally available. Broad damage effects target roughly 5-8%
paired value; single-build/action effects may reach 8-14%; double-locked or risky
effects may reach 10-18%. Pure utility may sit below those DPS bands when cooldown,
casts, prevention, healing, or denied attacks demonstrate equivalent value.

Only seven affected pairs were run in Round 10, followed by the changed Sixth
Basic Radial pair in Round 11. No completed matrix or unrelated pair was repeated.
Reports:

- `Build/msvc-validation/Artifacts/relic-balance-tuning-round10-180/combat_report.json`
- `Build/msvc-validation/Artifacts/relic-balance-tuning-round10-300/combat_report.json`
- `Build/msvc-validation/Artifacts/relic-balance-tuning-round11/combat_report.json`
