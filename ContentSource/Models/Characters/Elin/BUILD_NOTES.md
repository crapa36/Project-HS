# Elin

2026-10-12. Static archer character assembled and refined through Blenderwright MCP in Blender 5.2.2 LTS, using the supplied Elin.zip meshes and 24 reference sheets.

## Files

- `Elin.glb`: primary transferable asset; one scene, geometry and textures embedded.
- `Elin_v003.blend`: editable character, materials, packed textures, studio lights and four cameras. Contains only the final scene.
- `Elin.fbx`: additional interchange export with embedded textures.
- `Textures/`: 36 PNG source textures with clothing value adjustments baked in.

## Model

The `Elin` root owns 72 mesh objects: fitted body and head, blouse, corset, wrap skirt, belts and pouches, boots, cape, feathered hat, bow and quiver, fitted fingerless gloves, bracer bands, burgundy hair locks, eyes and lashes. The bow is independent equipment beside the neutral character.

Blender coordinates are metres, Z up, front -Y. Ground is Z=0; height including the hat is 2.032500m. GLB and FBX use Y-up interchange conversion. There are 742,267 evaluated triangles and 23 materials. GLB combines roughness and metallic channels into 27 embedded images.

This is a static modeling deliverable. It has no armature, skin weights, shape keys, animation, LODs or game-specific collision. Retopology and runtime integration are outside this delivery. The triangle count needs reduction for constrained real-time targets. Separate garment and body surfaces can require additional deformation fitting after rigging.

## Provenance

Source archive `Elin.zip` has SHA-256 `AC7DBFF1971486DF0C4C90A90DA5E6EEDE1ACA4A09913A8B6798BA1FFB381847`.
