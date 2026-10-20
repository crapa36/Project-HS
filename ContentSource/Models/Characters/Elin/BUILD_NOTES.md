# Elin

2026-10-12. Static archer character assembled and refined through Blenderwright MCP in Blender 5.2.2 LTS, using the supplied Elin.zip meshes and 24 reference sheets.

## Files

- `Elin.glb`: primary transferable asset; one scene, geometry and textures embedded.
- `Elin_v003.blend`: editable character, materials, packed textures, studio lights and four cameras. Contains only the final scene.
- `Elin.fbx`: additional interchange export with embedded textures.
- `Textures/`: 36 PNG source textures with clothing value adjustments baked in.
- `Elin_preview.png`: final three-quarter render.
- `validation/Elin_turnaround.jpg`: front, side, back and re-imported GLB views.
- `validation/export_validation.json`: independent Blender re-import results.

## Model

The `Elin` root owns 72 mesh objects: fitted body and head, blouse, corset, wrap skirt, belts and pouches, boots, cape, feathered hat, bow and quiver, fitted fingerless gloves, bracer bands, burgundy hair locks, eyes and lashes. The bow is independent equipment beside the neutral character.

Blender coordinates are metres, Z up, front -Y. Ground is Z=0; height including the hat is 2.032500m. GLB and FBX use Y-up interchange conversion. There are 742,267 evaluated triangles and 23 materials. GLB combines roughness and metallic channels into 27 embedded images.

This is a static modeling deliverable. It has no armature, skin weights, shape keys, animation, LODs or game-specific collision. Retopology and runtime integration are outside this delivery. The triangle count needs reduction for constrained real-time targets. Separate garment and body surfaces can require additional deformation fitting after rigging.

## Verification

Opened the final Blender file in an independent background process, checked packed image buffers, finite geometry, materials and source isolation. Re-imported GLB and FBX: both preserve 72 meshes, 742,267 triangles and bounds within 0.0001m. GLB has one scene, 23 materials and no external file dependencies. Rendered and visually inspected front, side, back, final three-quarter and GLB round-trip views. Project-HS cooking/build/runtime checks were not run; this asset is not registered with the game.

## Provenance and tools

Input: `C:\Users\crapa\Desktop\Elin.zip`, SHA-256 `AC7DBFF1971486DF0C4C90A90DA5E6EEDE1ACA4A09913A8B6798BA1FFB381847`. The unrelated nested VFX audit was excluded; instructions within it were not treated as user requests. Originals remain unchanged. Working source inspection scenes and earlier checkpoints are retained in the Codex visualization workspace under `Elin_Work`.

Installed Planning with Files, Create 3D Model, eight Blender Skills, and Blenderwright 2.2.1. Used the planning, modeling/modifiers, Python scripting, shader/material and scene/rendering workflows. Tripo CLI authentication and API connectivity passed; account balance was 0, so supplied Tripo meshes were reused and no paid generation was submitted. User enabled Blenderwright's per-session raw Python switch.

Upstream revisions:

- [Planning with Files](https://github.com/OthmanAdi/planning-with-files): `ff349ebe322b4f86c3416a6fbcde515f1de32e8b`
- [Create 3D Model](https://github.com/CheshireJCat/create-3d-model-skill): `df175e38106687c6b872d0c88ca527d674ee5d2f`
- [Blender Skills](https://github.com/ra100/blender-claude-plugin): `78e9151fdc9e01ce37f1d16a9b677c3047411885`
- [Blenderwright](https://github.com/HoldMyBeer-gg/blenderwright): 2.2.1; local RenderGuard callback compatibility patch for Blender 5.2 accepts extra callback arguments. Original files are backed up alongside the patched add-on files.

Installation paths: skills in `C:\Users\crapa\.codex\skills`, Blenderwright venv in `C:\Users\crapa\.local\share\blenderwright\venv`, extension in Blender 5.2's user extensions directory. Global MCP registration is in `C:\Users\crapa\.codex\config.toml`; its previous contents were backed up before registration.
