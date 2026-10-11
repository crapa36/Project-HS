# VFX reference-driven improvement: preparation state

## State, not acceptance

Base: `codex/engine-v7-backup-20261009`, commit
`b8e9dbb878c8bf1cb0ae609b6143b3ef8dc0ec2a`.

This change adds local reference-ingestion tools and synthetic tests only.
It does NOT claim that reference movies were visually analyzed or that production
VFX were improved. Runtime, renderer, shaders, source textures/meshes, cooked data,
and gameplay simulation are unchanged.

The agreed reference set has 32 files: 19 MP4s, eight animated GIFs, and five stills.
Its identity and paths stay in the private local review inventory, not public Git.
At preparation time the Python/container environment failed with `ClientError`,
and the companion terminal returned `tunnel_client_not_seen`. Actual 32-source
motion review, engine execution, native-frame extraction, and before/after captures
are blocked. Download success must not be described as successful visual analysis.
Synthetic CI tests, when run, must be reported separately from reference validation.

## Ordered work

### A. Complete evidence before selecting production changes

1. Ingest the full agreed set, check file count and fingerprints, fully decode the
   visual stream of each movie/GIF, and identify stills. Record every failure.
2. Use whole-video navigation and normal-speed footage to locate useful skill
   sequences and exclude dialogue, menus and repeated examples with reasons.
3. Extract actual native frames around useful actions, including preparation,
   release, travel, contact peak and residual disappearance. Preserve source timing.
4. Document visible silhouette, spatial structure, material contrast, emission
   hierarchy, surface flow, erosion, ground interaction and motion timing.
5. Separate observed pixels/motion from implementation hypotheses. Creator mesh,
   texture and shader breakdowns can corroborate hypotheses; videos alone cannot
   establish shader source, topology, blend modes, or real-world HDR intensity.

### B. Build three representative original effects to establish target quality

These are proposed design targets, NOT findings from unreviewed movies.

- Crystal/arcane projectile: a deliberately modelled head with thickness and
  readable facets; authored surface and emission masks; layered color ramp and
  internal flow; a head-led trail; contact and residual phases aligned to movement.
- Large melee slash: a purpose-built curved/tapered mesh; authored leading edge
  and torn trailing texture; directional surface flow and erosion; impact fragments,
  optional local distortion, and a ground response aligned to the swing.
- Ice-cluster rupture: multiple designed crystal silhouettes with spatial depth;
  staggered growth/breakage; distinct hard crystal, soft mist and ground frost;
  dedicated surface/emission data; layered impact and residual timing.

The goal is spectacular, attractive skill art, not merely fewer particles.
Do not use a global brightness reduction, global ribbon narrowing, a generic
noise texture on every primitive, or removal of every ring as the improvement.
Invest real modelling effort in prominent objects; use purpose-built curved
meshes for energy volumes; author original textures and material responses.
Keep precise gameplay telegraphs separate from decorative distortion/noise.
Do not copy reference-game textures, meshes, or footage into shipped assets.

### C. Integrate only after reference-informed decisions

Relevant source entry points to inspect before edits:

- `ContentSource/VFX/production_spec.v4.json` (effect composition/timing);
- `ContentSource/VFX/Meshes/author_mesh_atlas.py` (authored mesh generation);
- `ContentSource/GameData/particles.json` (particle presentation data);
- `Content/Shaders/project_hs.hlsl`, `vfx.hlsl`, `vfx_ribbon.hlsli` (rendered appearance);
- `Source/Runtime/Private/vfx_catalog.cpp` (catalog binding).

Re-read scoped AGENTS and ARCHITECTURE before changing runtime/renderer ownership.
Do not edit generated/cooked output as authoritative source. Preserve simulation
and presentation catalog separation and the shared 60Hz simulation/cooker contract.
Map each proposed visual change to its reference evidence and actual source owner.
Do not change all effects at once or replace the backup branch head.

### D. Verify final appearance and integration

Use the actual game camera, normal speed, bright/dark backgrounds and overlapping
effects. Compare preparation, release, travel, impact and residue, not just the
single brightest frame. Retain full-colour spectacle while preserving the effect's
main silhouette, spatial depth and material distinctions. Record measured render
cost for the tested scene rather than claiming a universal frame-rate outcome.
Run focused checks first, then the full repository gate for renderer/content work:
`cmake --workflow --preset verify`. Build success is not visual acceptance.

Visual evidence and review criteria belong here or in the private review report,
not in DEV_STATE.md, which remains technical implementation continuity only.
