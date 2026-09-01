# MonsterForSurvivalGame - Unreal-native PBR export

Generated with Unreal Engine 5.8 built-in exporters and Geometry Script only.
No Blender or third-party DCC processing was used.

- `Mesh/PBR`: 8 skeletal mesh FBX files
- `Animation/PBR`: 140 animation FBX files
- `Texture/PBR`: 3 source PBR textures as PNG
- `Material/PBR/material_mapping.json`: material settings, graph inputs, texture settings, and mesh slot mapping
- `Mesh/PBR/unreal_geometry_repair.json`: Unreal Geometry Script repair evidence
- `export_manifest.json`: source assets, hashes, byte sizes, and export results

Geometry repair:

- Beholder_SK: 9 open boundary loops filled, 0 remain
- Cactus_SK: 57 open boundary loops filled, 0 remain
- TurtleShell_SK: 13 open boundary loops filled, 0 remain
- Mushroom_SK and ChestMonster_SK: intentional openings retained

Material reconstruction:

- Material: Opaque, Default Lit, one-sided (`two_sided=false`)
- Base Color: `BasecolorDefault_TEX.png`, RGB, sRGB on
- Emissive Color: `Emissive_TEX.png`, RGB multiplied by 80, sRGB on
- Roughness: `RAM_TEX.png`, R channel, sRGB off
- Metallic: `RAM_TEX.png`, B channel, sRGB off
- Ambient Occlusion: unconnected
- Mesh material slot: `lambert1` -> `DefaultPBR_MAT`

FBX cannot serialize the complete Unreal material graph. Reconnect the supplied PNG textures exactly as listed above or in `material_mapping.json`.
