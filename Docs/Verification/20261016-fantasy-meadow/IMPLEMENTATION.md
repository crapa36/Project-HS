# Fantasy meadow implementation — active

Source: DIRECTIVE.ko.md, exact copy of user-authorized desktop directive (2026-10-16).
Status: implementation in progress; no full completion or 144 FPS claim.

## Contracts

- Active cpu-gpu-utilization worktree; preserve unrelated dirty work and all raw captures.
- Existing Windows D3D12 engine, metre units, 1.75m character, native monitor 1920x1080, highest remaining non-RT options, frame generation allowed, target144 FPS.
- Existing elevated game camera and 2D gameplay stay; no engine replacement, new enemies/weather/fluid simulation, paid assets, uploads or plugin installation.
- Art assumption: late spring/early summer, clear daylight following rain. Existing tone mapping/exposure remains baseline.
- Representative area first: footpath and paired cart ruts, dry/wet mud, localized depressions and separate level water surfaces in some wheel/human/deer tracks. Deer is a track-only art assumption, no new animal gameplay.
- Existing seven ground-detail variants preserved; append shrub, rooted stump, intact/broken crate, intact/broken barrel, broken cart. Wood hoops are wooden, not falsely shaded metallic.
- Model producer owns new MeadowProps source/script; integration worker owns variant/catalog/build registration and placement; ground design worker audits terrain/water contracts. Parent owns source registration, provenance, integration and visual validation.

## Baseline and tool evidence

- Pre-meadow native image: Build/msvc-validation/Artifacts/render-baseline/8d63afca7ac0172c69127f55d88f1de153d7cad062a2d7405a8bd3c0f215f01b/capture_tick_180.png. Opened and visually inspected before meadow edits.
- Latest raw play baseline: Build/play-captures/20261016-ao-direct-view-01. Game/sampler exit0. 1724 CPU/GPU diagnostic frames;818 Playing frames;36.437 base FPS;26.443ms GPU critical path. Analysis partial; all818 Playing frames background, excluded from foreground performance comparison. SDK presented36.524FPS does not prove frame generation active. Audit: measurement-audit.json.
- Ground currently cube slab at y=0; wet/rut/shoe shader is explicitly material-only, no physical water or animal tracks. Replacing local ground must not leave the slab covering depressions.
- Installed Blender --version verified5.2.2LTS. Blenderwright addon manifest2.2.1. Scene query succeeded after starting existing localhost addon. Original startup scene (Camera/Cube/Light) preserved.
- MCP code_exec toolset enable succeeds server-side, but tool is not callable in current session. Use installed Blender batch execution for authored scripts; do not claim MCP code execution.

## References verified now

| Source | Observe / apply | Exclude / reuse |
|---|---|---|
| https://seblagarde.wordpress.com/2013/04/14/water-drop-3b-physically-based-wet-surfaces/ | Separate porous wet ground, film, pooled water and normals | No wholesale shader import, no new rain system. Text is reference only |
| https://www.usgs.gov/media/images/white-tailed-deer-tracks | Split-hoof shape and mud impression | No inference of exact depth/age. Page identifies public-domain image; no image copied into game |
| https://research.colonialwilliamsburg.org/Foundation/journal/Winter04-05/wheel.cfm | Hub/spokes/rim structure planned | Live page open failed; directive's prior observation only, not newly verified medieval dimensions |

## Remaining gates

1. Lock shared terrain/water/placement descriptors and representative region.
2. Build and inspect geometry blockout, then materials and multiview previews; export/reimport3LOD assets.
3. Integrate true depressions and separate level water, dry/wet/film distinction, connected travel traces.
4. Cluster vegetation/natural remnants/props, ground contact, exclusion and correct metre scale; inspect missing dry/pressed grass, moss and pebbles explicitly.
5. Cook/build smallest affected targets, native views and motion/combat, repeat same foreground route once for representative area.
6. Expand only validated rules; record all directive requirements as completed/partial/not performed with actual evidence.

## Requirement coverage ledger (implementation gate, not completion)

| Directive requirements | Current evidence / owner | State |
|---|---|---|
| §1–2 engine, resolution, units, scope, preservation | Existing D3D12 worktree;1.75m character;1920x1080;no gameplay-system replacement;raw baseline and directive preserved | Verified setup |
| §3 references/style | Wet-surface/USGS/Guerrilla/Riot sources opened. RHS and wheel reference live fetch failed; no reference image used as texture | Partial |
| §4,9 small grass/3flowers/clover/leaves/twigs/fence | Existing authored7species/3LOD, metre-scale adjustments retained | Existing; new-scene visual gate pending |
| §4,9 shrubs/stump+roots/crate/barrel/cart broken structures | New7species×3LOD blockout generated and inspected. Shrub/stump silhouettes rejected; refinement in progress | Partial modeling; not engine verified |
| §4,9 dry/pressed grass, moss, gravel/small stones | Existing terrain moss and small rock/grass placement; directive-specific distribution and dry/pressed variants pending | Partial |
| §5 distinct foot/cart routes and supply context | Ground source plus placement integration in progress; no new village | In progress |
| §6 terrain scale/actual local depressions | Existing slab replacement in representative24m square proposed; common source/height sampling being implemented | In progress |
| §7 four wet states and level water | New separate water geometry/closure planned; existing shader wetness is insufficient | In progress |
| §8 three trace kinds with partial fill | Human soles, split deer hoof, 1.64m cart wheel track source planned | In progress |
| §10 exposure, material spaces, contact, wind/history, hierarchy | Existing exposure baseline retained; atlas reused; new dynamic vegetation/geometry verification pending | Partial |
| §11 common source/seed/exclusions/regeneration | Existing registration now updates owned records while preserving other meshes; actual-function idempotence selfcheck passes | Partial |
| §12 same-camera/frame-cost/stability/LOD | Prior native smoke baseline exists. New performance attempt excluded: all Playing background. New scene measurements pending | Incomplete |
| §13 outputs | Directive, implementation ledger, source generator, blockout .blend and preview exist. Final files require finish/roundtrip/native gates | Partial |
| §14 complete acceptance | Cannot pass until all mandatory elements, true water, motion/combat/native visual checks and target performance are evidenced | Not achieved |

Additional reference observations (2026-10-16): Guerrilla's procedural placement introduction supports artist-controlled road/vegetation rules (https://www.guerrilla-games.com/read/gpu-based-procedural-placement-in-horizon-zero-dawn); Riot's clarity article supports combat-first silhouettes and restrained decorative contrast (https://www.leagueoflegends.com/en-us/news/dev/clarity-in-league/). Only their public text introductions were inspected; no claim of video/full presentation review or asset reuse. RHS habitat page timed out. No downloaded third-party textures, purchases, external uploads or new addons.
