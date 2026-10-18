# Ground detail v001

Authored in Blender 5.2.2 LTS for Project HS. No downloaded assets or paid generation.

## Deliverables and runtime contract

- Seven archetypes, each with LOD0/LOD1/LOD2 at metre-scale ground pivots: flower_daisy, flower_buttercup, flower_violet, clover_patch, ground_leaves, twig_cluster, fence_short.
- Native environment_meshes.json contains the original 42 records unchanged and 21 appended records.
- Native coordinates use the proper rotation (x,z,-y). Runtime UV V is flipped before tangent calculation; the resulting bitangent sign is retained.
- All triangles use material_index 0, renderer material ordinal 15, texture layer 0. One opaque ground_detail material uses three texture products.
- Petals, clover and dried leaves have thin closed shells with front/back/edge geometry. There are no rectangular alpha cards.
- Runtime atlas: 2048x2048, 16 cells, 24-pixel edge dilation in each cell. Basecolor is sRGB RGBA8, surface is linear RGBA8 with roughness R / AO G, normal is linear RGB8.
- Blender/FBX/GLB use a separate normal image with green flipped relative to the runtime V-down basis. Roughness export is the exact surface R channel. The canonical runtime surface map retains AO G.
- The original project bark source is reused only inside the new twig/lichen texture cells; its hash is recorded in atlas_regions.json. Other atlas cells are authored deterministically.

## Topology budget

| Archetype | LOD0 tris | LOD1 tris | LOD2 tris |
|---|---:|---:|---:|
| flower_daisy | 11144 | 4260 | 1050 |
| flower_buttercup | 7168 | 2880 | 738 |
| flower_violet | 7308 | 3360 | 936 |
| clover_patch | 5112 | 2160 | 672 |
| ground_leaves | 2040 | 748 | 264 |
| twig_cluster | 1510 | 520 | 162 |
| fence_short | 1248 | 336 | 128 |

Flower patches contain seven flowers at LOD0; these counts are per patch, not per flower. Fence bounds are 1.8m long and 0.75m high at every LOD. Other exact measured bounds are in verification/runtime_mesh_validation.json.

## Reproduction

Use absolute paths and an isolated Blender batch process:

    blender.exe --background --factory-startup --python tools/art/create_ground_detail_blender.py -- --root REPOSITORY --stage geometry

Inspect preview/blockout.png, then run the same command with --stage finish. That authors the atlas, assigns materials, writes the native JSON/material/manifest additions, exports FBX/GLB, saves the packed .blend and renders previews. Run --stage verify for FBX/GLB round trips. A fresh Scene remains preserved; only the task's new scene and collections are exported. Existing generated artifacts in this GroundDetail directory are task-owned.

## Validation and visual review

- All 21 native meshes: finite 13-component vertices; UV within 0..1; unit normal/tangent and orthogonal basis; bitangent sign +/-1; no degenerate, duplicate polygon or duplicate triangle geometry; all triangles material 0.
- The first 42 native mesh values were independently compared with bc7bd60 and matched exactly. Canonical value SHA-256: ee4c4551cbc8fc42611263e5e37d1d51d185c9432d4eb9ca07d8ac69fa8a7500.
- All seven FBX files were re-imported in isolated scenes: 3 LOD objects each, exact triangle count, UV0, one material and bounds matching within 0.00005m.
- GLB re-import contains exactly the seven LOD0 meshes with expected triangle counts/UV/material, excluding startup and preview objects.
- Near and game-scale Blender CPU renders were inspected. The largest issues found and corrected were violet flower direction/basal leaves, overly regular fence grain, a radial twig pile, detached twig fibres, LOD height drift and duplicate violet root caps.
- Final packed .blend was reopened in the dedicated Blender process through MCP; scene and flower/clover/fence dimensions/materials were inspected and preview/mcp_final.png was rendered through MCP and inspected.
- The final violet close render includes the corrected diverging petioles. The root-cap correction does not change other visible surfaces, so their previous final renders remain representative.
- Real game placement, runtime tone mapping, native shadows, LOD transitions, instance density and hardware performance are outside this asset-authoring validation; the parent integration task owns those checks. The previews establish concrete quality evidence, not an unqualified AAA certification.
